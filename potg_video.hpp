#pragma once
// The play of the game: a champion intro over the replay, played live by default or saved as a video
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct PotgPlayer {
    std::string name;     // Riot ID game name
    int champion_id = 0;
    int skin = 0;         // skin number within the champion
    int profile_icon = 0; // summoner icon
};

struct ReplayClip {
    std::filesystem::path video;   // 1080p60, starts at the clip start
    double play_x = 0, play_z = 0; // where on the map the play happened
};

// One stretch of a song: from and to are seconds into the song file
struct SongSpan {
    double from = 0;
    double to = 0;
};

// A song cut around the moment the intro cuts to the replay. The intro plays the song up to the first span of parts; the
// spans then play back to back under the replay. Every time is seconds into the song file. The fields, with Legends
// Never Die's values (pick_music has both songs):
//
//   field       type              example                                 meaning
//   file        text              "legends_never_die.wav"                 the song, in intro/music
//   parts       list of spans     {{41.525, 48.382}, {51.811, 60.6}}      each {from, to} is one stretch of the song; the
//                                                                         first starts on the beat that lands on the cut
//   intro_from  seconds or -1     -1                                      where the intro's music starts (-1: wherever it
//                                                                         has to so it runs straight into the first span)
//   intro_to    seconds or -1     -1                                      where the intro's music stops (-1: at the first span)
//   fade        true / false      false                                   fade out at the end instead of stopping on the
//                                                                         song's own break
//   skip        seconds           27.459                                  the intro jumps this far ahead, from an earlier
//                                                                         pass of a repeated passage to a later one (0: no jump)
//   skip_at     seconds           39.56                                   where in the later pass the jump's crossfade ends
//   crossfade   seconds           1.8                                     how long the earlier pass fades into the later one
struct Music {
    std::string file;
    std::vector<SongSpan> parts;
    double intro_from = -1;
    double intro_to = -1;
    bool fade = false;
    double skip = 0;
    double skip_at = 0;
    double crossfade = 0;
    double replay_length() const { // seconds of song under the replay
        double total = 0;
        for (const SongSpan& span : parts) total += span.to - span.from;
        return total;
    }
};
// The song with that name, or a random one when the name is empty
Music pick_music(const std::string& name);

// Where progress messages go ("Rendering the Gwen intro in Blender..."); the console if never set
void set_potg_status(std::function<void(const std::string&)> sink);

// Where this game's files go
std::filesystem::path potg_output_dir(long long game_id);

// Ends whatever is running (Esc does the same) at its next step, taking the intro down at once; cleared for the next play
void request_stop();
void clear_stop();
// Throws "Stopped." once a stop has been asked for
void check_stop();

// True when a replay is answering on the local game API
bool replay_running();

// Waits for the open replay to finish loading, then pauses it at game time t
void hold_replay_at(double t);

// Covers the screen with a Play of the Game loading card until hidden; the watch hides it itself just before the intro
void show_loading();
void hide_loading();
// The step the loading screen shows under LOADING ("Opening the replay...")
void show_step(const std::string& step);

// Silences the replay (only it, in the Windows mixer) until let go; the watch lets go as the scene starts
void quiet_replay(bool on);

// Puts the League client in front, rather than leaving Windows to pick whatever window was next
void bring_client_back();

// Closes the open replay (only ever a replay, never a game), so the client comes back
void close_replay();

// Dragon, elder and baron steals the open replay reports, as (game time, "DRAGON"/"ELDER_DRAGON"/"BARON_NASHOR"); nothing if it reports none
std::optional<std::vector<std::pair<double, std::string>>> replay_steals();

// Names of the players in the open replay, to tell which game it is
std::vector<std::string> replay_player_names();

// Champion, skin and summoner icon of a player, from the client's match details and the open replay
PotgPlayer find_potg_player(const nlohmann::json& game, const std::string& name);

// Champion recall over the shared background with the title card, ending on a white flash; made once per player and skin
std::filesystem::path make_intro(const PotgPlayer& player);

// Plays the intro over League, then fades into the replay at the play with the camera on the player, start to end
void watch_play(const std::string& player, double start, double end, const std::filesystem::path& intro, const Music& music,
                const std::filesystem::path& camera_cache);

// Records start..end of the open replay with the camera following the player and the HUD hidden
ReplayClip record_replay_clip(const std::string& player, double start, double end, const std::filesystem::path& out,
                              const std::filesystem::path& camera_cache);

// Intro then the replay fading in from white
void stitch_potg(const std::filesystem::path& intro, const ReplayClip& clip, const Music& music, const std::filesystem::path& out);
