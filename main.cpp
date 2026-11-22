// The Play of the Game helper: runs in the background behind the Play of the Game tab in the League client (pengu/potg).
// The tab lists your games itself; this scores a game's plays, then plays one in the replay or saves it as a video
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "httplib.h"
#include <windows.h>
#include <shellapi.h>

#include "potg_engine.hpp"
#include "potg_video.hpp"

#include <functional>
#include <map>
#include <mutex>

namespace {

const int PORT = 47811; // the tab calls http://127.0.0.1:47811
const std::filesystem::path SETTINGS = std::filesystem::path(POTG_ASSETS).parent_path() / "settings.json";

// The one background job at a time (watching or saving a play) and what it last reported
struct JobState {
    std::mutex lock;
    bool busy = false;
    std::string message = "";
    std::string error = "";
    std::string saved = "";
    std::thread worker;
} job;

void report(const std::string& message) {
    show_step(message);
    std::lock_guard<std::mutex> guard(job.lock);
    job.message = message;
}

// Scored games, so switching back and forth in the tab is instant; a game is scored again with the replay's steal flags
// once a play of it has been shown, since only then is its replay ours to scrub through
std::mutex scores_lock;
std::map<long long, json> scores;

std::string riot_key() {
    const char* key = std::getenv("RIOT_API_KEY");
    return key ? key : "";
}

void set_riot_key(const std::string& key) {
    _putenv_s("RIOT_API_KEY", key.c_str());
    std::ofstream(SETTINGS) << json{{"riot_api_key", key}}.dump(2);
    std::lock_guard<std::mutex> guard(scores_lock);
    scores.clear();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// True when the replay that's open is this game: most of its players are this game's players
bool replay_is(LcuClient& lcu, long long game_id) {
    if (!replay_running()) return false;
    std::vector<std::string> open = replay_player_names();
    int matches = 0;
    for (const auto& [id, player] : lcu.get_players(game_id)) {
        for (const auto& name : open) {
            if (!player.name.empty() && lower(name) == lower(player.name)) matches++;
        }
    }
    return matches >= 6;
}

using Steals = std::vector<std::pair<double, std::string>>;

// What a game's replay told us, kept beside the game's other files so it's read once. A game has no play of the game
// until its replay has been read
std::filesystem::path replay_facts(long long game_id) { return potg_output_dir(game_id) / "replay_facts.json"; }

json score_game(LcuClient& lcu, long long game_id) {
    {
        std::lock_guard<std::mutex> guard(scores_lock);
        auto cached = scores.find(game_id);
        if (cached != scores.end()) return cached->second;
    }
    json facts = json::parse(std::ifstream(replay_facts(game_id)), nullptr, false);
    if (!facts.is_object()) return {{"game", game_id}, {"found", false}};
    std::optional<Steals> steals;
    if (facts.value("steals", json()).is_array()) steals = facts["steals"].get<Steals>();

    std::unordered_map<int, Player> players = lcu.get_players(game_id);
    // Riot's timeline (damage and health) is kept once fetched, so the game keeps them after the API key expires
    std::filesystem::path kept = potg_output_dir(game_id) / "riot_timeline.json";
    json timeline = json::parse(std::ifstream(kept), nullptr, false);
    if (!timeline.is_object()) {
        timeline = lcu.get_timeline(game_id);
        if (lcu.riot_problem.empty() && timeline.is_object() && timeline.contains("info")) std::ofstream(kept) << timeline.dump();
    }
    if (players.empty() || !timeline.is_object()) throw std::runtime_error("The client has no match data for this game.");

    EventTracker tracker;
    for (const auto& event : timeline_to_events(timeline, players, steals)) tracker.add_event(event);

    std::unordered_map<std::string, Player> by_name;
    for (const auto& [id, player] : players) by_name[player.name] = player;
    json plays = json::array();
    for (const auto& play : tracker.top_plays(5)) {
        json events = json::array();
        for (const auto& e : play.events) {
            int victim_champion = e.victim.empty() || !by_name.count(e.victim) ? 0 : by_name[e.victim].champion_id;
            events.push_back({{"time", e.timestamp}, {"label", e.label}, {"points", e.score}, {"why", e.why}, {"victim", e.victim},
                              {"victim_champion", victim_champion}});
        }
        const Player& who = by_name[play.player];
        plays.push_back({{"player", play.player}, {"champion_id", who.champion_id}, {"profile_icon", who.profile_icon},
                         {"start", play.start}, {"end", play.end}, {"score", play.score}, {"events", events}});
    }
    json result = {{"game", game_id}, {"found", true}, {"plays", plays}, {"riot_key", !riot_key().empty()}};
    std::lock_guard<std::mutex> guard(scores_lock);
    scores[game_id] = result;
    return result;
}

// Opens this game's replay in the client, downloading it first if needed
void open_replay(LcuClient& lcu, long long game_id) {
    if (replay_running()) {
        if (replay_is(lcu, game_id)) return;
        throw std::runtime_error("Another game's replay is open. Close it and try again.");
    }
    std::string id = std::to_string(game_id);
    auto metadata = [&] {
        json meta = lcu.get("/lol-replays/v1/metadata/" + id);
        return meta.is_object() ? meta : json::object();
    };
    std::string state = metadata().value("state", "");
    if (state.empty()) { // the client only knows a game's replay once its match history page has been opened, so do what that page does
        json game = lcu.get_game(game_id);
        if (!game.is_object()) throw std::runtime_error("The client has no match data for this game.");
        lcu.post("/lol-replays/v1/metadata/" + id + "/create/gameVersion/" + game.value("gameVersion", "") + "/gameType/" +
                 game.value("gameType", "") + "/queueId/" + std::to_string(game.value("queueId", 0)), json::object());
        for (int i = 0; i < 20 && state.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            state = metadata().value("state", "");
        }
    }
    if (state == "download" || state == "retryDownload") {
        report("Downloading the replay...");
        lcu.post("/lol-replays/v1/rofls/" + id + "/download", {{"componentType", "replay-button_match-history"}});
        for (int i = 0; i < 600 && state != "watch"; ++i) {
            check_stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            json meta = metadata();
            state = meta.value("state", "");
            if (state == "downloading") report("Downloading the replay... " + std::to_string(meta.value("downloadProgress", 0)) + "%");
        }
    }
    if (state != "watch") throw std::runtime_error("This game's replay isn't available (" + (state.empty() ? "unknown" : state) + ").");

    report("Opening the replay...");
    lcu.post("/lol-replays/v1/rofls/" + id + "/watch", {{"componentType", "replay-button_match-history"}});
    for (int i = 0; i < 180 && !replay_running(); ++i) {
        check_stop();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!replay_running()) throw std::runtime_error("The replay didn't open.");
}

// Plays (or with save, records) one play of a game: opens its replay, readies the intro, then hands over to the video code
// Everything a play does before its scene happens behind the loading screen with the replay silent; afterwards the client
// comes back, unless they paused the replay to keep watching
struct BehindLoading {
    BehindLoading() {
        show_loading();
        quiet_replay(true);
    }
    ~BehindLoading() {
        hide_loading();
        quiet_replay(false);
        if (!replay_running()) bring_client_back();
    }
};

void show_play(long long game_id, std::string name, double start, double end, std::string song, bool save) {
    clear_stop();
    BehindLoading loading;
    LcuClient lcu;
    if (!lcu.connect()) throw std::runtime_error("The League client isn't open.");
    // The clip ends just after the play's last event and runs back as long as the song plays after its hit
    Music music = pick_music(song);
    double clip_end = end + 1.5;
    double clip_start = std::max(0.0, std::min(start, clip_end - music.replay_length()));

    open_replay(lcu, game_id);
    report("Waiting for the replay to load...");
    hold_replay_at(clip_start); // paused on the play rather than running from the start while the intro is made

    std::filesystem::path dir = potg_output_dir(game_id);
    PotgPlayer player = find_potg_player(lcu.get_game(game_id), name);
    report("Getting the intro ready...");
    std::filesystem::path intro = make_intro(player);
    if (!save) {
        report("Playing the play of the game in the replay...");
        watch_play(player.name, clip_start, clip_end, intro, music, dir / "camera_watch.json");
        report("Done.");
        return;
    }
    report("Recording the play (League comes to the front for about a minute)...");
    ReplayClip clip = record_replay_clip(player.name, clip_start, clip_end, dir / "replay.mp4", dir / "camera_save.json");
    std::filesystem::path out = dir / (player.name + "_potg.mp4");
    report("Putting the video together...");
    stitch_potg(intro, clip, music, out);
    std::lock_guard<std::mutex> guard(job.lock);
    job.saved = out.string();
    job.message = "Saved.";
}

// Opens a game's replay out of the way and reads it, which settles its play of the game, then plays that: the intro
// first, then the scene. Nobody learns whose play it is before the intro shows them
void find_play(long long game_id, const std::string& song) {
    clear_stop();
    BehindLoading loading;
    LcuClient lcu;
    if (!lcu.connect()) throw std::runtime_error("The League client isn't open.");
    try {
        open_replay(lcu, game_id);
        report("Reading the replay...");
        hold_replay_at(0.0);
        std::optional<Steals> steals = replay_steals();
        std::ofstream(replay_facts(game_id)) << json{{"steals", steals ? json(*steals) : json()}}.dump(2);
        {
            std::lock_guard<std::mutex> guard(scores_lock);
            scores.erase(game_id);
        }
        json plays = score_game(lcu, game_id).value("plays", json::array());
        if (plays.empty()) {
            close_replay();
            report("Nobody scored in this game.");
            return;
        }
        const json& best = plays[0];
        show_play(game_id, best.value("player", ""), best.value("start", 0.0), best.value("end", 0.0), song, false);
    } catch (...) {
        close_replay(); // never leave it open behind the client
        throw;
    }
}

// Runs one background job (finding, watching or saving) unless one is already running
bool start_job(std::function<void()> work) {
    std::lock_guard<std::mutex> guard(job.lock);
    if (job.busy) return false;
    if (job.worker.joinable()) job.worker.join();
    job.busy = true;
    job.error = job.saved = "";
    job.message = "Starting...";
    job.worker = std::thread([work] {
        std::string error;
        try {
            work();
        } catch (const std::exception& e) {
            error = e.what();
        }
        std::lock_guard<std::mutex> guard(job.lock);
        job.busy = false;
        job.error = error;
    });
    return true;
}

void reply(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

} // namespace

int main() {
    // One helper at a time
    CreateMutexW(nullptr, TRUE, L"Local\\PotgHelper");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    json settings = json::parse(std::ifstream(SETTINGS), nullptr, false);
    if (settings.is_object() && !settings.value("riot_api_key", "").empty()) _putenv_s("RIOT_API_KEY", settings.value("riot_api_key", "").c_str());
    set_potg_status(report);

    httplib::Server server;
    // The tab runs inside the client's page, so it needs CORS (and Chrome's private network opt-in) to reach us
    server.set_default_headers({{"Access-Control-Allow-Origin", "*"}, {"Access-Control-Allow-Headers", "Content-Type"},
                                {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"}, {"Access-Control-Allow-Private-Network", "true"}});
    server.Options(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    server.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> guard(job.lock);
        reply(res, {{"busy", job.busy}, {"message", job.message}, {"error", job.error}, {"saved", job.saved}, {"riot_key", !riot_key().empty()}});
    });

    server.Get("/api/plays", [](const httplib::Request& req, httplib::Response& res) {
        LcuClient lcu;
        if (!lcu.connect()) return reply(res, {{"error", "The League client isn't open."}}, 503);
        try {
            reply(res, score_game(lcu, std::atoll(req.get_param_value("game").c_str())));
        } catch (const std::exception& e) {
            reply(res, {{"error", e.what()}}, 500);
        }
    });

    // Body: {game, music}
    server.Post("/api/find", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return reply(res, {{"error", "Bad request."}}, 400);
        long long game = body.value("game", 0LL);
        std::string song = body.value("music", "");
        if (!start_job([game, song] { find_play(game, song); })) return reply(res, {{"error", "Still busy with the last play."}}, 409);
        reply(res, {{"ok", true}});
    });

    // Body: {game, player, start, end, music, save}
    server.Post("/api/show", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return reply(res, {{"error", "Bad request."}}, 400);
        bool started = start_job([body] {
            show_play(body.value("game", 0LL), body.value("player", ""), body.value("start", 0.0), body.value("end", 0.0),
                      body.value("music", ""), body.value("save", false));
        });
        if (!started) return reply(res, {{"error", "Still busy with the last play."}}, 409);
        reply(res, {{"ok", true}});
    });

    server.Post("/api/stop", [](const httplib::Request&, httplib::Response& res) {
        request_stop();
        reply(res, {{"ok", true}});
    });

    // Older games: the client's history stops at your latest 20, so their IDs come from Riot's API instead (needs the key).
    // Query: start (how many recent games to skip), count. Replies {"games": [game IDs, newest first]}
    server.Get("/api/older", [](const httplib::Request& req, httplib::Response& res) {
        std::string key = riot_key();
        if (key.empty()) return reply(res, {{"error", "Showing older games needs a Riot API key."}}, 400);
        LcuClient lcu;
        if (!lcu.connect()) return reply(res, {{"error", "The League client isn't open."}}, 503);
        json me = lcu.get("/lol-summoner/v1/current-summoner");
        std::string region = lcu.get("/riotclient/region-locale").value("region", "NA");
        std::string routing = "americas"; // Riot groups regions into four routing servers for match history
        for (const char* r : {"EUW", "EUNE", "TR", "RU", "ME"}) if (region == r) routing = "europe";
        for (const char* r : {"KR", "JP"}) if (region == r) routing = "asia";
        for (const char* r : {"OCE", "PH", "SG", "TH", "TW", "VN"}) if (region == r) routing = "sea";
        // Riot's API knows the account by an ID of its own, looked up from the Riot ID (name#tag)
        cpr::Response account = cpr::Get(cpr::Url{"https://" + routing + ".api.riotgames.com/riot/account/v1/accounts/by-riot-id/" +
                                                  cpr::util::urlEncode(me.value("gameName", "")) + "/" + cpr::util::urlEncode(me.value("tagLine", ""))},
                                         cpr::Header{{"X-Riot-Token", key}}, cpr::Timeout{15000});
        std::string puuid = json::parse(account.text, nullptr, false).value("puuid", "");
        cpr::Response r = cpr::Get(cpr::Url{"https://" + routing + ".api.riotgames.com/lol/match/v5/matches/by-puuid/" + puuid + "/ids"},
                                   cpr::Parameters{{"start", req.get_param_value("start")}, {"count", req.get_param_value("count")}},
                                   cpr::Header{{"X-Riot-Token", key}}, cpr::Timeout{15000});
        json ids = json::parse(r.text, nullptr, false);
        if (r.status_code != 200 || !ids.is_array()) {
            return reply(res, {{"error", r.status_code == 401 || r.status_code == 403
                                             ? "Riot turned the API key down; development keys last 24 hours, so save a new one."
                                             : "Riot's API answered " + std::to_string(r.status_code) + "."}}, 502);
        }
        json games = json::array();
        for (const auto& id : ids) { // "NA1_5676123429" -> 5676123429
            std::string text = id.get<std::string>();
            games.push_back(std::atoll(text.substr(text.find('_') + 1).c_str()));
        }
        reply(res, {{"games", games}});
    });

    server.Post("/api/key", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        set_riot_key(body.is_object() ? body.value("key", "") : "");
        reply(res, {{"ok", true}});
    });

    // Shows the saved video in Explorer
    server.Post("/api/reveal", [](const httplib::Request&, httplib::Response& res) {
        std::string saved;
        {
            std::lock_guard<std::mutex> guard(job.lock);
            saved = job.saved;
        }
        if (!saved.empty()) ShellExecuteA(nullptr, "open", "explorer.exe", ("/select,\"" + saved + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        reply(res, {{"ok", !saved.empty()}});
    });

    // Replies {"songs": [{key, name}]} for the tab's song list
    server.Get("/api/music", [](const httplib::Request& req, httplib::Response& res) {
        json list = json::array();
        for (const auto& [key, music] : load_music()){
            list.push_back({{"key",key},{"name",music.name}});

        }
        reply(res,{{"songs",list}});
    });
    server.listen("127.0.0.1", PORT);
    return 0;
        }   
