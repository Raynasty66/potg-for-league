#pragma once
// Scores a game from its timeline: every kill, assist and objective gets points, and the best runs of one player's
// points within 15 seconds are the plays of the game
#include <vector>            // For the dynamic list of events (std::vector)
#include <string>            // For text (std::string)
#include <unordered_map>     // For the dictionary (std::unordered_map)
#include <unordered_set>     // For counting distinct players in a fight (std::unordered_set)
#include <iostream>          // For printing to console (std::cout)
#include <fstream>           // For reading the League client lockfile
#include <sstream>           // For splitting the lockfile on ':'
#include <filesystem>        // For finding the newest replay file
#include <iomanip>           // For the numbers in the score breakdown
#include <cmath>             // For std::hypot
#include <algorithm>         // For std::stable_sort
#include <nlohmann/json.hpp> // Converting strings to json
#include <cpr/cpr.h>         // For making HTTP requests in LcuClient
#include <chrono>            // For waiting on the replay
#include <thread>            // For std::this_thread::sleep_for
#include <optional>          // For steals the replay may or may not report
#include <cstdlib>           // For reading RIOT_API_KEY

using json = nlohmann::json;

enum class EventType {
    ChampionKill,
    Assist,
    DragonKill,
    ElderKill,
    BaronKill,
    Other
};

inline EventType event_string_to_type(const std::string& event_string, const std::string& monster_type = "", const std::string& monster_sub_type = "") {
    if (event_string == "CHAMPION_KILL") return EventType::ChampionKill;
    if (event_string == "ELITE_MONSTER_KILL") {
        if (monster_type == "DRAGON") {
            if (monster_sub_type == "ELDER_DRAGON") return EventType::ElderKill;
            return EventType::DragonKill;
        }
        if (monster_type == "BARON_NASHOR") return EventType::BaronKill;
    }
    return EventType::Other;
}

struct CombatEvent {
    double timestamp = 0.0;
    EventType type = EventType::Other;
    std::string player = "";
    std::string victim = "";
    int multikill_count = 1;
    bool is_stolen = false;
    int shutdown_spree = 0;      // Kill streak of the victim this kill ended
    int allies_in_fight = 1;     // Distinct players on each side in the same fight
    int enemies_in_fight = 0;
    double gold_deficit = 0.0;   // How far the player's team was behind in gold
    double health_deficit = 0.0; // How far behind on health the killer started the fight, in health bars (Riot API timelines)
    double damage_share = -1.0;  // Share of the victim's damage this player did, 0..1 (Riot API timelines; -1 when unknown)
    double score = 0.0;
    std::string label = "";      // What happened, for the play's breakdown
    std::string why = "";        // How its points add up
};

struct Player {
    std::string name = "";
    int team = 0;
    int champion_id = 0;
    int profile_icon = 0;
};

// One player's best run of points within the window
struct Play {
    std::string player = "";
    double start = 0.0;
    double end = 0.0;
    double score = 0.0;
    std::vector<CombatEvent> events;
};

// Every number the scoring uses. Points are whole numbers in the play's breakdown; "share" means the share of the
// victim's health bar this player took out (from Riot's timeline; counted as 100% without a Riot API key)
struct ScoreWeights {
    // Kills and assists: the points times your share
    double kill = 50.0;   // a kill where you did all the damage
    double assist = 25.0; // an assist where you did all the damage (in practice, the share you did)

    // Bonuses on a kill, also times your share
    double double_kill = 100.0;  // 2nd kill within 10 s of the 1st
    double triple_kill = 250.0;  // 3rd within 10 s of the 2nd
    double quadra_kill = 700.0;  // 4th within 10 s of the 3rd
    double penta_kill = 1500.0;  // 5th within 30 s of the 4th
    int shutdown_min_spree = 3;          // ending a streak pays only if they had at least this many kills since dying
    double shutdown_per_spree_kill = 5.0; // points per kill in the streak you ended
    double shutdown_max = 30.0;           // most a shutdown can pay
    // (Winning with less health has no number here: it pays (kill + multikill bonus) x how many health bars behind you
    // started, so a whole bar behind doubles them; see calculate_score)

    // Objectives: flat points, whoever did the damage. Taking one is worth a little, stealing one a lot; an Elder steal
    // equals a full triple kill (3 x 50 + 100 + 250 = 500)
    double dragon_normal = 25.0;
    double dragon_steal = 150.0;
    double baron_normal = 25.0;
    double baron_steal = 350.0;
    double elder_normal = 50.0;
    double elder_steal = 500.0;

    // Boosts to an event's total for the fight around it (not times your share)
    double outnumbered_step = 0.5;            // +50% per enemy more than your side had in the fight (a 2v3 is +50%)
    double outnumbered_max = 2.0;             // at most +100%
    double comeback_min_deficit = 1500.0;     // the comeback boost only applies once your team is this much gold behind
    double comeback_per_gold = 1.0 / 20000.0; // +1% per 200 gold behind, counted from 0 (so +7.5% at 1,500)
    double comeback_max = 1.15;               // at most +15%
};

struct TimelineKill {
    double time = 0.0;
    int killer = 0;
    int victim = 0;
    std::vector<int> assisters;
    double damage_taken = 0.0;        // Everything the victim took before dying (Riot API timelines only)
    std::unordered_map<int, double> damage_by; // What each champion did of that
    double killer_damage_taken = 0.0; // Damage the victim did to the killer
    double x = 0.0;
    double y = 0.0;
};

struct TimelineMonster {
    double time = 0.0;
    int killer = 0;
    std::string monster_type = "";
    std::string monster_sub_type = "";
    std::vector<int> assisters;
    double x = 0.0;
    double y = 0.0;
};

inline double damage_of(const json& hit) {
    return hit.value("physicalDamage", 0.0) + hit.value("magicDamage", 0.0) + hit.value("trueDamage", 0.0);
}

// steals: (game time, "DRAGON"/"ELDER_DRAGON"/"BARON_NASHOR") from the replay, or nothing to guess them from the fight
inline std::vector<CombatEvent> timeline_to_events(const json& timeline, const std::unordered_map<int, Player>& players,
                                            const std::optional<std::vector<std::pair<double, std::string>>>& steals) {
    // Kills this close in time and space count as the same fight
    const double fight_seconds = 10.0;
    const double fight_radius = 2500.0;

    json frames = timeline.contains("info") ? timeline["info"].value("frames", json::array()) : timeline.value("frames", json::array());

    auto name_of = [&](int id) { auto it = players.find(id); return it == players.end() ? std::string("") : it->second.name; };
    auto team_of = [&](int id) { auto it = players.find(id); return it == players.end() ? 0 : it->second.team; };

    std::vector<TimelineKill> kills;
    std::vector<TimelineMonster> monsters;
    for (const auto& frame : frames) {
        for (const auto& e : frame.value("events", json::array())) {
            std::string type = e.value("type", "");
            json position = (e.contains("position") && e["position"].is_object()) ? e["position"] : json::object();
            if (type == "CHAMPION_KILL") {
                TimelineKill k;
                k.time = e.value("timestamp", 0.0) / 1000.0;
                k.killer = e.value("killerId", 0);
                k.victim = e.value("victimId", 0);
                k.assisters = e.value("assistingParticipantIds", std::vector<int>{});
                // The death recap: what hit the victim and what the victim hit (only in match-v5 timelines)
                for (const auto& hit : e.value("victimDamageReceived", json::array())) {
                    double amount = damage_of(hit);
                    k.damage_taken += amount;
                    if (hit.value("type", "") == "OTHER") k.damage_by[hit.value("participantId", 0)] += amount; // champions
                }
                for (const auto& hit : e.value("victimDamageDealt", json::array())) {
                    if (hit.value("participantId", 0) == k.killer) k.killer_damage_taken += damage_of(hit);
                }
                k.x = position.value("x", 0.0);
                k.y = position.value("y", 0.0);
                kills.push_back(k);
            } else if (type == "ELITE_MONSTER_KILL") {
                TimelineMonster m;
                m.time = e.value("timestamp", 0.0) / 1000.0;
                m.killer = e.value("killerId", 0);
                m.monster_type = e.value("monsterType", "");
                m.monster_sub_type = e.value("monsterSubType", "");
                m.assisters = e.value("assistingParticipantIds", std::vector<int>{});
                m.x = position.value("x", 0.0);
                m.y = position.value("y", 0.0);
                monsters.push_back(m);
            }
        }
    }
    std::stable_sort(kills.begin(), kills.end(), [](const TimelineKill& a, const TimelineKill& b) { return a.time < b.time; });

    // Gold the given team is behind by at the last minute frame before time t
    auto gold_deficit = [&](int team, double t) {
        const json* last = nullptr;
        for (const auto& frame : frames) {
            if (frame.value("timestamp", 0.0) / 1000.0 <= t) last = &frame;
        }
        if (last == nullptr) return 0.0;
        double ally_gold = 0.0;
        double enemy_gold = 0.0;
        json participant_frames = last->value("participantFrames", json::object());
        for (const auto& item : participant_frames.items()) {
            double gold = item.value().value("totalGold", 0.0);
            if (team_of(std::stoi(item.key())) == team) ally_gold += gold;
            else enemy_gold += gold;
        }
        return enemy_gold - ally_gold;
    };

    // A player's health and max health at the last minute frame before time t, with that frame's time (match-v5 only)
    struct Health { double now = 0.0, max = 0.0, at = -1.0; };
    auto health_at = [&](int id, double t) {
        Health h;
        for (const auto& frame : frames) {
            double at = frame.value("timestamp", 0.0) / 1000.0;
            if (at > t) break;
            json stats = frame.value("participantFrames", json::object()).value(std::to_string(id), json::object()).value("championStats", json::object());
            if (stats.contains("healthMax")) h = {stats.value("health", 0.0), stats.value("healthMax", 0.0), at};
        }
        return h;
    };

    // Health bars the killer was behind when the fight started. The victim started it with about the damage they took
    // before dying; the killer with their health at the last minute frame, if it was close enough to the fight to trust
    // (no death since, under 30s old), and at least the damage the victim then did to them
    auto health_deficit = [&](const TimelineKill& k, double fight_start) {
        Health killer = health_at(k.killer, fight_start), victim = health_at(k.victim, fight_start);
        if (killer.max <= 0.0 || victim.max <= 0.0 || k.damage_taken <= 0.0) return 0.0;
        bool died_since = false;
        for (const auto& other : kills) {
            if (other.victim == k.killer && other.time > killer.at && other.time < fight_start) died_since = true;
        }
        double killer_start = died_since ? killer.max : killer.now;
        if (fight_start - killer.at > 30.0) killer_start = killer.max;
        killer_start = std::max(killer_start, k.killer_damage_taken);
        double victim_start = std::min(victim.max, k.damage_taken);
        return std::max(0.0, (victim_start - killer_start) / std::max(killer.max, victim.max));
    };

    // Everyone who killed, died or assisted near (x, y) around time t, split by side
    auto fight_sides = [&](int team, double t, double x, double y, double seconds, double radius) {
        std::unordered_set<int> allies;
        std::unordered_set<int> enemies;
        for (const auto& other : kills) {
            if (std::abs(other.time - t) > seconds) continue;
            if (std::hypot(other.x - x, other.y - y) > radius) continue;
            std::vector<int> involved = other.assisters;
            involved.push_back(other.killer);
            involved.push_back(other.victim);
            for (int id : involved) {
                if (players.find(id) == players.end()) continue;
                if (team_of(id) == team) allies.insert(id);
                else enemies.insert(id);
            }
        }
        return std::make_pair(allies, enemies);
    };

    std::vector<CombatEvent> events;
    std::unordered_map<int, int> spree;          // Kills since each player's last death
    std::unordered_map<int, int> chain;          // Current multikill length
    std::unordered_map<int, double> last_kill;   // Time of each player's last kill

    for (const auto& k : kills) {
        int victim_spree = spree[k.victim];
        spree[k.victim] = 0;
        chain[k.victim] = 0;
        last_kill.erase(k.victim);

        // Killer id 0 means a tower or minion got the kill
        if (players.find(k.killer) == players.end()) continue;

        // Multikills chain within 10s per kill, 30s for the penta
        double limit = chain[k.killer] == 4 ? 30.0 : 10.0;
        bool chained = last_kill.count(k.killer) && k.time - last_kill[k.killer] <= limit;
        chain[k.killer] = chained ? chain[k.killer] + 1 : 1;
        last_kill[k.killer] = k.time;
        spree[k.killer]++;

        int team = team_of(k.killer);
        auto [allies, enemies] = fight_sides(team, k.time, k.x, k.y, fight_seconds, fight_radius);
        double deficit = gold_deficit(team, k.time);

        CombatEvent kill_event;
        kill_event.timestamp = k.time;
        kill_event.type = EventType::ChampionKill;
        kill_event.player = name_of(k.killer);
        kill_event.victim = name_of(k.victim);
        kill_event.multikill_count = chain[k.killer];
        kill_event.shutdown_spree = victim_spree;
        kill_event.allies_in_fight = (int)allies.size();
        kill_event.enemies_in_fight = (int)enemies.size();
        kill_event.gold_deficit = deficit;
        kill_event.health_deficit = health_deficit(k, k.time - fight_seconds);
        if (k.damage_taken > 0.0) kill_event.damage_share = (k.damage_by.count(k.killer) ? k.damage_by.at(k.killer) : 0.0) / k.damage_taken;
        events.push_back(kill_event);

        for (int id : k.assisters) {
            CombatEvent assist_event;
            assist_event.timestamp = k.time;
            assist_event.type = EventType::Assist;
            assist_event.player = name_of(id);
            assist_event.victim = name_of(k.victim);
            assist_event.allies_in_fight = (int)allies.size();
            assist_event.enemies_in_fight = (int)enemies.size();
            assist_event.gold_deficit = deficit;
            if (k.damage_taken > 0.0) assist_event.damage_share = (k.damage_by.count(id) ? k.damage_by.at(id) : 0.0) / k.damage_taken;
            events.push_back(assist_event);
        }
    }

    for (const auto& m : monsters) {
        EventType type = event_string_to_type("ELITE_MONSTER_KILL", m.monster_type, m.monster_sub_type);
        if (type == EventType::Other || players.find(m.killer) == players.end()) continue;

        CombatEvent monster_event;
        monster_event.timestamp = m.time;
        monster_event.type = type;
        monster_event.player = name_of(m.killer);
        if (steals) {
            // The replay flags steals itself
            std::string kind = m.monster_sub_type == "ELDER_DRAGON" ? "ELDER_DRAGON" : m.monster_type;
            for (const auto& [time, stolen] : *steals) {
                if (stolen == kind && std::abs(time - m.time) < 5.0) monster_event.is_stolen = true;
            }
        } else {
            // The timeline has no steal flag, so call it a steal when the killer had no help
            // and at least two enemies were fighting at the pit
            auto [allies, enemies] = fight_sides(team_of(m.killer), m.time, m.x, m.y, 15.0, 3000.0);
            monster_event.is_stolen = m.assisters.empty() && enemies.size() >= 2;
        }
        events.push_back(monster_event);
    }
    return events;
}

class EventTracker {
private:
    std::vector<CombatEvent> events;
    ScoreWeights score_weights;

    static std::string number(double value, int digits = 0) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(digits) << value;
        return out.str();
    }

    double multikill_bonus(int multikill_count) const {
        if (multikill_count == 2) return score_weights.double_kill;
        if (multikill_count == 3) return score_weights.triple_kill;
        if (multikill_count == 4) return score_weights.quadra_kill;
        if (multikill_count >= 5) return score_weights.penta_kill;
        return 0.0;
    }

    static std::string grouped(double value) { // 2358 -> "2,358"
        std::string digits = number(value), out;
        for (size_t i = 0; i < digits.size(); ++i) {
            if (i && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return out;
    }

    // Each reason is the points it added, in plain words, so the reasons add up to the event's points
    struct Breakdown {
        double score = 0.0;
        std::vector<std::string> why;
        void add(double points, const std::string& reason) {
            double shown = std::round(score + points) - std::round(score);
            score += points;
            if (shown != 0.0) why.push_back("+" + number(shown) + " " + reason);
        }
        void boost(double factor, const std::string& reason) { add(score * (factor - 1.0), reason); }
    };

    void fight_bonuses(const CombatEvent& event, Breakdown& points) const {
        if (event.enemies_in_fight > event.allies_in_fight) {
            points.boost(std::min(score_weights.outnumbered_max,
                                  1.0 + score_weights.outnumbered_step * (event.enemies_in_fight - event.allies_in_fight)),
                         "for fighting " + std::to_string(event.allies_in_fight) + " against " + std::to_string(event.enemies_in_fight));
        }
        if (event.gold_deficit > score_weights.comeback_min_deficit) {
            points.boost(std::min(score_weights.comeback_max, 1.0 + event.gold_deficit * score_weights.comeback_per_gold),
                         "because your team was " + grouped(event.gold_deficit) + " gold behind");
        }
    }

    // Points for one event; also fills in its label and how the points add up
    double calculate_score(CombatEvent& event) const {
        static const char* tiers[] = {"", "", "double kill", "triple kill", "quadra kill", "penta kill"};
        Breakdown points;
        switch (event.type) {
            case EventType::ChampionKill: {
                event.label = "Killed " + event.victim;
                if (event.multikill_count >= 2) event.label += std::string(" (") + tiers[std::min(event.multikill_count, 5)] + ")";
                // Everything the kill earns is credited by the share of their damage you did: 47% of it, 47% of the points
                bool known = event.damage_share >= 0.0;
                double share = known ? std::min(1.0, event.damage_share) : 1.0;
                auto credited = [&](double full, const std::string& what) {
                    points.add(full * share, known ? what + " (max " + number(full) + ")" : what);
                };
                points.add(score_weights.kill * share, known ? "for doing " + number(share * 100) + "% of their health bar (max " +
                                                               number(score_weights.kill) + ")"
                                                         : "for the kill");
                double multikill = multikill_bonus(event.multikill_count);
                if (multikill > 0.0) credited(multikill, std::string("for the ") + tiers[std::min(event.multikill_count, 5)]);
                if (event.shutdown_spree >= score_weights.shutdown_min_spree) {
                    credited(std::min(score_weights.shutdown_max, event.shutdown_spree * score_weights.shutdown_per_spree_kill),
                             "for ending their " + std::to_string(event.shutdown_spree) + "-kill streak");
                }
                // Winning from behind on health: the kill and its multikill bonus over again, by how far behind you started
                // (a whole health bar behind doubles them)
                if (event.health_deficit > 0.01) {
                    double behind = std::min(1.0, event.health_deficit), whole = score_weights.kill + multikill;
                    credited(behind * whole, "for winning with " + number(behind * 100) + "% less health");
                }
                fight_bonuses(event, points);
                break;
            }

            case EventType::Assist:
                event.label = "Assist on " + event.victim;
                if (event.damage_share >= 0.0) { // like a kill, by the share of their health bar you took
                    double share = std::min(1.0, event.damage_share);
                    points.add(score_weights.assist * share, "for doing " + number(share * 100) + "% of their health bar (max " +
                                                                 number(score_weights.assist) + ")");
                } else {
                    points.add(score_weights.assist, "for the assist");
                }
                fight_bonuses(event, points);
                break;

            case EventType::DragonKill:
                event.label = event.is_stolen ? "Stole a dragon" : "Took a dragon";
                points.add(event.is_stolen ? score_weights.dragon_steal : score_weights.dragon_normal, event.is_stolen ? "for the steal" : "for the dragon");
                break;

            case EventType::BaronKill:
                event.label = event.is_stolen ? "Stole Baron" : "Took Baron";
                points.add(event.is_stolen ? score_weights.baron_steal : score_weights.baron_normal, event.is_stolen ? "for the steal" : "for Baron");
                break;

            case EventType::ElderKill:
                event.label = event.is_stolen ? "Stole the Elder Dragon" : "Took the Elder Dragon";
                points.add(event.is_stolen ? score_weights.elder_steal : score_weights.elder_normal, event.is_stolen ? "for the steal" : "for the Elder Dragon");
                break;

            default:
                break;
        }
        for (size_t i = 0; i < points.why.size(); ++i) event.why += (i ? "\n" : "") + points.why[i];
        return points.score;
    }

public:
    void add_event(CombatEvent event) {
        event.score = calculate_score(event);
        events.push_back(event);
    }

    // The best plays, highest first: each player's best run of events within time_interval seconds, skipping any
    // that overlaps a better play in time
    std::vector<Play> top_plays(size_t count, double time_interval = 15.0) const {
        std::unordered_map<std::string, std::vector<CombatEvent>> by_player;
        for (const auto& event : events) {
            if (!event.player.empty()) by_player[event.player].push_back(event);
        }

        std::vector<Play> candidates;
        for (auto& [player, list] : by_player) {
            std::stable_sort(list.begin(), list.end(),
                [](const CombatEvent& a, const CombatEvent& b) { return a.timestamp < b.timestamp; });
            size_t left = 0;
            double sum = 0.0;
            for (size_t right = 0; right < list.size(); right++) {
                sum += list[right].score;
                while (list[right].timestamp - list[left].timestamp > time_interval) {
                    sum -= list[left].score;
                    left++;
                }
                Play play;
                play.player = player;
                play.start = list[left].timestamp;
                play.end = list[right].timestamp;
                play.score = sum;
                play.events.assign(list.begin() + left, list.begin() + right + 1);
                candidates.push_back(play);
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](const Play& a, const Play& b) { return a.score > b.score; });
        std::vector<Play> picked;
        for (const auto& candidate : candidates) {
            if (picked.size() >= count || candidate.score <= 0.0) break;
            bool overlaps = std::any_of(picked.begin(), picked.end(),
                [&](const Play& p) { return candidate.start <= p.end && p.start <= candidate.end; });
            if (!overlaps) picked.push_back(candidate);
        }
        return picked;
    }
};

class LcuClient {
    /*
    Reads match history from the League client, no Riot API key needed
    */
private:
    std::string base_url = "";
    std::string password = "";
public:
    // Why the last timeline came without damage and health (no key, or Riot refused it), or empty if it had them
    std::string riot_problem = "";
    bool connect(const std::string& lockfile_path = "C:/Riot Games/League of Legends/lockfile") {
        // The lockfile looks like LeagueClient:pid:port:password:https
        std::ifstream file(lockfile_path);
        if (!file) return false;
        std::string line;
        std::getline(file, line);
        std::vector<std::string> parts;
        std::stringstream stream(line);
        std::string part;
        while (std::getline(stream, part, ':')) parts.push_back(part);
        if (parts.size() < 5) return false;
        base_url = "https://127.0.0.1:" + parts[2];
        password = parts[3];
        return true;
    }

    json get(const std::string& path) {
        cpr::Response r = cpr::Get(cpr::Url{base_url + path},
            cpr::Authentication{"riot", password, cpr::AuthMode::BASIC},
            cpr::VerifySsl{false}, cpr::Timeout{5000});
        if (r.status_code != 200) return json();
        return json::parse(r.text, nullptr, false);
    }

    std::unordered_map<int, Player> get_players(long long game_id) {
        std::unordered_map<int, Player> players;
        json game = get("/lol-match-history/v1/games/" + std::to_string(game_id));
        if (!game.is_object()) return players;
        for (const auto& participant : game.value("participants", json::array())) {
            Player& player = players[participant.value("participantId", 0)];
            player.team = participant.value("teamId", 0);
            player.champion_id = participant.value("championId", 0);
        }
        for (const auto& identity : game.value("participantIdentities", json::array())) {
            json player = identity.value("player", json::object());
            std::string name = player.value("gameName", "");
            if (name.empty()) name = player.value("summonerName", "");
            players[identity.value("participantId", 0)].name = name;
            players[identity.value("participantId", 0)].profile_icon = player.value("profileIcon", 0);
        }
        return players;
    }

    json post(const std::string& path, const json& body) {
        cpr::Response r = cpr::Post(cpr::Url{base_url + path},
            cpr::Authentication{"riot", password, cpr::AuthMode::BASIC},
            cpr::Body{body.dump()}, cpr::Header{{"Content-Type", "application/json"}},
            cpr::VerifySsl{false}, cpr::Timeout{5000});
        return json::parse(r.text, nullptr, false);
    }

    json get_game(long long game_id) {
        return get("/lol-match-history/v1/games/" + std::to_string(game_id));
    }

    json get_timeline(long long game_id) {
        // With a Riot API key the match-v5 timeline also has each death's damage recap and everyone's health every minute
        const char* key = std::getenv("RIOT_API_KEY");
        riot_problem = "";
        if (key == nullptr || *key == '\0') riot_problem = "There's no Riot API key saved.";
        if (key != nullptr && *key != '\0') {
            std::string platform = get_game(game_id).value("platformId", "NA1");
            std::string region = "americas";
            for (const char* p : {"EUW1", "EUN1", "TR1", "RU", "ME1"}) if (platform == p) region = "europe";
            for (const char* p : {"KR", "JP1"}) if (platform == p) region = "asia";
            for (const char* p : {"OC1", "PH2", "SG2", "TH2", "TW2", "VN2"}) if (platform == p) region = "sea";
            cpr::Response r = cpr::Get(cpr::Url{"https://" + region + ".api.riotgames.com/lol/match/v5/matches/" + platform + "_" +
                                                std::to_string(game_id) + "/timeline"},
                                       cpr::Header{{"X-Riot-Token", key}}, cpr::Timeout{15000});
            if (r.status_code == 200) return json::parse(r.text, nullptr, false);
            if (r.status_code == 401 || r.status_code == 403)
                riot_problem = "Riot turned the API key down (" + std::to_string(r.status_code) + "). Development keys stop working 24 hours "
                               "after they're made; get a new one at developer.riotgames.com and save it.";
            else if (r.status_code == 429) riot_problem = "Riot's API is rate-limiting the key right now; try again in a minute.";
            else if (r.status_code == 0) riot_problem = "Couldn't reach Riot's API (no connection?).";
            else riot_problem = "Riot's API answered " + std::to_string(r.status_code) + " for this game's timeline.";
        }
        return get("/lol-match-history/v1/game-timelines/" + std::to_string(game_id));
    }
};

