// Everything the play of the game puts on screen: driving League's replay (port 2999) and its camera, the loading screen,
// the intro overlay, window focus and muting, the intro compositor, and video export.
//
// Sections, top to bottom: small helpers and the replay API; windows, focus and sound (InputWatch, bring_to_front,
// LeagueInFront, quiet_replay); the camera path; IntroOverlay, LoadingScreen and ScreenStill; drawing (Canvas, Font,
// system_text) and the Encoder; then the functions potg_video.hpp lists, ending with make_intro and stitch_potg.
//
// Windows types used here:
//   HWND     a window's handle, the ID Windows gives each window (FindWindowW returns one)
//   DWORD    an unsigned 32-bit number; here process IDs, thread IDs and GetTickCount's milliseconds
//   HDC      a device context, what GDI draws text and pixels into
//   HBITMAP, HFONT, HBRUSH   GDI's handles to a bitmap, a font and a fill colour
//   HHOOK    a system-wide keyboard or mouse hook (InputWatch)
//   LRESULT, WPARAM, LPARAM  the return value and two arguments every window message and hook callback carries
//   HANDLE   a handle to a process or other kernel object (OpenProcess returns one)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include "potg_video.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <io.h>
#include <atomic>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <cpr/cpr.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const fs::path ASSETS = POTG_ASSETS;
const std::string BLENDER = R"(C:\Program Files\Blender Foundation\Blender 5.2\blender.exe)";
const std::string CDRAGON = "https://raw.communitydragon.org/latest/plugins/rcp-be-lol-game-data/global/default/v1";
const std::string GAME_API = "https://127.0.0.1:2999";
const double PI = 3.14159265358979;

void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

std::atomic<bool> stop_wanted{false};

bool replay_window() { return FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client") != nullptr; }

// Esc counts only while the replay is in front, so one pressed in another app doesn't stop the play
bool escape_pressed() {
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    bool ours = front && (front == FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client") || pid == GetCurrentProcessId());
    return ours && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

std::function<void(const std::string&)> status_sink;

void status(const std::string& message) {
    if (status_sink) status_sink(message);
    else std::cout << message << "\n";
}

// A command line run through cmd.exe with no console window, with its stdout ('r') or stdin ('w') piped to us if asked
struct Child {
    HANDLE process = nullptr;
    FILE* pipe = nullptr;
};

Child launch(const std::string& cmd, char pipe_mode = 0) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    STARTUPINFOA si{};
    si.cb = sizeof si;
    HANDLE ours = nullptr, theirs = nullptr;
    if (pipe_mode) {
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 1 << 20)) throw std::runtime_error("can't make a pipe");
        ours = pipe_mode == 'r' ? read : write;
        theirs = pipe_mode == 'r' ? write : read;
        SetHandleInformation(ours, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = pipe_mode == 'w' ? theirs : nullptr;
        si.hStdOutput = pipe_mode == 'r' ? theirs : nullptr;
    }
    // cmd.exe strips one pair of outer quotes, so wrap the whole line
    std::string line = "cmd.exe /S /C \"" + cmd + "\"";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("can't start: " + cmd);
    CloseHandle(pi.hThread);
    if (theirs) CloseHandle(theirs);
    Child child{pi.hProcess};
    if (ours) child.pipe = _fdopen(_open_osfhandle((intptr_t)ours, pipe_mode == 'r' ? _O_RDONLY : 0), pipe_mode == 'r' ? "rb" : "wb");
    return child;
}

// Closes our end of the pipe, waits for the command and returns its exit code
int finish(Child& child) {
    if (child.pipe) fclose(child.pipe);
    child.pipe = nullptr;
    WaitForSingleObject(child.process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.process, &code);
    CloseHandle(child.process);
    return (int)code;
}

void run(const std::string& cmd) {
    Child child = launch(cmd);
    if (finish(child) != 0) throw std::runtime_error("command failed: " + cmd);
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// Length of a media file in seconds
double duration(const fs::path& media) {
    Child probe = launch("ffprobe -v error -show_entries format=duration -of csv=p=0 " + quote(media), 'r');
    char buf[64] = {};
    fgets(buf, sizeof buf, probe.pipe);
    finish(probe);
    return std::atof(buf);
}

// The intro's music, then the replay's parts joined on their downbeats; made once per song and intro length
fs::path music_bed(const Music& music, double intro_len) {
    fs::path out = ASSETS / "cache" / (fs::path(music.file).stem().string() + "_" + std::to_string(std::lround(intro_len * 1000)) + ".wav");
    if (fs::exists(out)) return out;
    fs::create_directories(out.parent_path());
    fs::path song = ASSETS / "music" / music.file;
    const double SPLICE = 0.03; // each later part fades in over the 30ms before its downbeat, so the joins don't click

    // Pieces of the song in order; each crossfades in over `join` seconds that end on its own start in the song
    struct Piece { double from, to, join; };
    std::vector<Piece> pieces;
    double hit = music.parts.front().from;
    bool pad_intro = false;
    if (music.skip > 0 && hit - intro_len < music.skip_at - music.crossfade) {
        // The earlier pass, in step with the later one, fades into it and the later pass runs on through the hit
        pieces.push_back({hit - intro_len - music.skip, music.skip_at - music.skip, 0});
        pieces.push_back({music.skip_at - music.crossfade, music.parts.front().to, music.crossfade});
    } else {
        double to = music.intro_to >= 0 ? music.intro_to : hit;
        double from = music.intro_from >= 0 ? music.intro_from : to - intro_len;
        pieces.push_back({from, from + std::min(intro_len, to - from), 0});
        pad_intro = true;
        pieces.push_back({music.parts.front().from, music.parts.front().to, 0});
    }
    for (size_t k = 1; k < music.parts.size(); ++k)
        pieces.push_back({music.parts[k].from - SPLICE, music.parts[k].to, SPLICE});

    std::string inputs, graph = "[0:a]afade=in:d=0.01" + (pad_intro ? ",apad=whole_dur=" + std::to_string(intro_len) : "") + "[c0];";
    for (size_t k = 0; k < pieces.size(); ++k) {
        inputs += " -ss " + std::to_string(pieces[k].from) + " -t " + std::to_string(pieces[k].to - pieces[k].from) + " -i " + quote(song);
        if (k == 0) continue;
        std::string join = pieces[k].join > 0
            ? "acrossfade=d=" + std::to_string(pieces[k].join) + ":c1=qsin:c2=qsin"
            : "concat=n=2:v=0:a=1";
        graph += "[c" + std::to_string(k - 1) + "][" + std::to_string(k) + ":a]" + join + "[c" + std::to_string(k) + "];";
    }
    double total = intro_len + music.replay_length(), fade = music.fade ? 1.5 : 0.2;
    graph += "[c" + std::to_string(pieces.size() - 1) + "]afade=out:st=" + std::to_string(total - fade) + ":d=" + std::to_string(fade) +
             ",volume=-3dB";
    run("ffmpeg -v error -y" + inputs + " -filter_complex \"" + graph + "\" -ar 48000 -c:a pcm_s16le " + quote(out));
    return out;
}

// Downloads once and keeps the file; empty when the server has nothing there
std::string fetch(const std::string& url, const fs::path& cache) {
    if (fs::exists(cache)) return read_file(cache);
    cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Header{{"User-Agent", "potg-intro"}}, cpr::Timeout{120000});
    if (r.status_code != 200) return "";
    fs::create_directories(cache.parent_path());
    std::ofstream(cache, std::ios::binary) << r.text;
    return r.text;
}

// ---- The game's local API (replay control while a replay is open, live data in a game) ----

json api_get(const std::string& path) {
    for (int tries = 0; tries < 10; ++tries) { // it stalls for seconds at a time while the engine encodes 4K frames
        check_stop();
        cpr::Response r = cpr::Get(cpr::Url{GAME_API + path}, cpr::VerifySsl{false}, cpr::Timeout{10000});
        if (r.status_code == 200) return json::parse(r.text, nullptr, false);
        if (r.status_code == 0 && !replay_window()) {
            check_stop(); // closed because they stopped it
            throw std::runtime_error("The replay was closed.");
        }
        sleep_s(0.5);
    }
    throw std::runtime_error("game API not answering: " + path);
}

void api_post(const std::string& path, const json& body) {
    for (int tries = 0; tries < 10; ++tries) {
        check_stop();
        cpr::Response r = cpr::Post(cpr::Url{GAME_API + path}, cpr::Body{body.dump()},
                                    cpr::Header{{"Content-Type", "application/json"}}, cpr::VerifySsl{false}, cpr::Timeout{10000});
        if (r.status_code == 200) return;
        if (r.status_code == 0 && !replay_window()) {
            check_stop(); // closed because they stopped it
            throw std::runtime_error("The replay was closed.");
        }
        sleep_s(0.5);
    }
    throw std::runtime_error("game API not answering: " + path);
}

void seek(double t) {
    api_post("/replay/playback", {{"time", t}, {"paused", true}, {"speed", 1.0}});
    for (int i = 0; i < 80 && api_get("/replay/playback").value("seeking", true); ++i) sleep_s(0.25);
}

// Only the play: scoreboard, portraits, minimap and replay panels off; multikill banners and kill callouts stay
const json HUD_OFF = {{"interfaceAll", true}, {"interfaceScore", false}, {"interfaceScoreboard", false}, {"interfaceFrames", false},
                      {"interfaceMinimap", false}, {"interfaceTimeline", false}, {"interfaceReplay", false}, {"interfaceChat", false},
                      {"interfaceTarget", false}, {"interfaceNeutralTimers", false},
                      {"interfaceAnnounce", true}, {"interfaceKillCallouts", true}, {"outlineSelect", false},
                      {"champions", true}, {"characters", true}, {"healthBarChampions", true}, {"healthBarMinions", true},
                      {"healthBarStructures", true}, {"healthBarWards", true}, {"healthBarPets", true}, {"floatingText", true},
                      {"cameraMode", "top"}};

json key(double t, const json& value) { return {{"time", t}, {"value", value}, {"blend", "linear"}}; }

// Applies render settings for a recording and puts the replay back exactly as it was afterwards, even on errors
class RenderSettings {
    json saved;
public:
    explicit RenderSettings(const json& changes) {
        json now = api_get("/replay/render");
        for (auto& [k, v] : changes.items())
            if (now.contains(k) && !now[k].is_null()) saved[k] = now[k];
        api_post("/replay/sequence", json::object());
        api_post("/replay/render", changes);
    }
    ~RenderSettings() {
        try {
            api_post("/replay/sequence", json::object());
            api_post("/replay/render", saved);
        } catch (...) {}
    }
};

void alt_tap() {
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
}

// Their own key presses and clicks; Windows marks input that programs fake (our Alt taps) as injected, so it doesn't count
std::atomic<DWORD> last_real_input{0};
LRESULT CALLBACK watch_keys(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && !(reinterpret_cast<KBDLLHOOKSTRUCT*>(l)->flags & LLKHF_INJECTED)) last_real_input = GetTickCount();
    return CallNextHookEx(nullptr, code, w, l);
}
LRESULT CALLBACK watch_clicks(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w != WM_MOUSEMOVE && !(reinterpret_cast<MSLLHOOKSTRUCT*>(l)->flags & LLMHF_INJECTED))
        last_real_input = GetTickCount();
    return CallNextHookEx(nullptr, code, w, l);
}

// Watches their keyboard and mouse while a play loads, on a thread of its own so their input never waits on us
class InputWatch {
    DWORD id = 0;
    std::atomic<bool> ready{false};
    std::thread thread;

public:
    InputWatch() : thread([this] {
        id = GetCurrentThreadId();
        HHOOK keys = SetWindowsHookExW(WH_KEYBOARD_LL, watch_keys, GetModuleHandleW(nullptr), 0);
        HHOOK clicks = SetWindowsHookExW(WH_MOUSE_LL, watch_clicks, GetModuleHandleW(nullptr), 0);
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE); // the thread's queue exists from here, so the quit message lands
        ready = true;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
        if (keys) UnhookWindowsHookEx(keys);
        if (clicks) UnhookWindowsHookEx(clicks);
    }) {
        while (!ready) sleep_s(0.001);
    }
    ~InputWatch() {
        PostThreadMessageW(id, WM_QUIT, 0, 0);
        if (thread.joinable()) thread.join();
    }
};

// True if they pressed a key or clicked just now
bool user_just_acted() { return GetTickCount() - last_real_input < 1500; }

// They switched to another app while a play was loading; it waits for them rather than pull them back
std::atomic<bool> user_away{false};

// A background process may not take the foreground, so borrow the current foreground thread's input; true if it worked
bool bring_to_front(HWND window) {
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE); // a minimized window can still be the foreground one
    for (int i = 0; i < 10 && GetForegroundWindow() != window; ++i) {
        DWORD them = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), us = GetCurrentThreadId();
        AttachThreadInput(us, them, TRUE);
        alt_tap(); // a bare Alt tap lets SetForegroundWindow through
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        AttachThreadInput(us, them, FALSE);
        sleep_s(0.3);
    }
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    return GetForegroundWindow() == window && !IsIconic(window);
}

// Restores the League client just behind the replay, so it's already drawn when the replay closes
void ready_client_behind() {
    HWND client = FindWindowW(L"RCLIENT", L"League of Legends"), league = FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client");
    if (!client || !league) return;
    // The client hides itself while a replay runs; shown straight into the slot under League, it never covers the scene
    if (IsIconic(client)) ShowWindow(client, SW_SHOWNOACTIVATE);
    SetWindowPos(client, league, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Mutes or unmutes every sound the process makes, as the Windows volume mixer would
void set_muted(DWORD pid, bool muted) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* speakers = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &speakers)) &&
        SUCCEEDED(speakers->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&manager)) &&
        SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* control = nullptr;
            IAudioSessionControl2* control2 = nullptr;
            ISimpleAudioVolume* volume = nullptr;
            DWORD owner = 0;
            if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2))) &&
                SUCCEEDED(control2->GetProcessId(&owner)) && owner == pid && SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&volume)))) {
                volume->SetMute(muted, nullptr);
            }
            if (volume) volume->Release();
            if (control2) control2->Release();
            if (control) control->Release();
        }
    }
    if (sessions) sessions->Release();
    if (manager) manager->Release();
    if (speakers) speakers->Release();
    if (devices) devices->Release();
    if (SUCCEEDED(com)) CoUninitialize();
}

DWORD replay_pid() {
    DWORD pid = 0;
    if (HWND league = FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client")) GetWindowThreadProcessId(league, &pid);
    return pid;
}

// Keeps the replay silent while it loads and is lined up behind the loading screen; its sound starts with the scene.
// The replay opens its sound a while after it starts, so it's muted again until let go
std::atomic<bool> quiet{false};
std::atomic<bool> replay_on_stage{false}; // the camera is being lined up, which a minimized replay can't do
std::thread quiet_thread;
std::mutex quiet_lock;

// The engine renders at the game window's size, and a tabbed-out fullscreen League is 1x1, so it comes forward;
// when recording, focus goes back afterwards, when watching it stays
class LeagueInFront {
    HWND previous;
    POINT cursor{};
    bool stay;
public:
    void keep() { stay = true; }
    explicit LeagueInFront(bool stay = false) : stay(stay) {
        SetProcessDPIAware();
        previous = GetForegroundWindow();
        GetCursorPos(&cursor);
        SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2); // at a screen edge it pans the camera
        HWND league = FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client");
        if (!league) throw std::runtime_error("The replay was closed.");
        // League only moves its camera while it's on screen
        if (!bring_to_front(league)) throw std::runtime_error("Couldn't bring the replay to the front.");
        sleep_s(2.0); // fullscreen mode switch settles
    }
    ~LeagueInFront() {
        if (stay) return;
        alt_tap();
        SetForegroundWindow(previous);
        SetCursorPos(cursor.x, cursor.y);
    }
};

// Closes the replay when the play is over or stopped, so the client comes back where it was; only ever a replay, never a game
struct CloseReplayAfter {
    bool keep = false;
    ~CloseReplayAfter() {
        if (keep) return;
        bring_client_back(); // first, so whatever was behind the replay doesn't flash up in between
        close_replay();
    }
};

struct CameraPath {
    json keys = json::array(), rotation;
    double x = 0, z = 0; // where on the map the play happened
};

// Keyframes that keep the camera on the champion from start to end. Found by playing the stretch with the camera
// attached to them and logging its path; the attachment falls to another champion whenever this one turns
// untargetable (Gwen's W), which shows up as a jump across the map, so those samples are dropped and the camera
// re-attached. While they're dead or flying in, it can't attach at all and sits where it was, so until it locks on
// the camera waits where they first show up. A play never changes, so the path is cached.
CameraPath camera_path(const std::string& player, double start, double end, double speed, const fs::path& cache, bool retry = true) {
    CameraPath path;
    json cached = fs::exists(cache) ? json::parse(read_file(cache), nullptr, false) : json();
    if (cached.is_object() && cached.value("start", -1.0) == start && cached.value("end", -1.0) == end) {
        path.keys = cached["keys"];
        path.rotation = cached["rotation"];
        path.x = cached.value("x", 0.0);
        path.z = cached.value("z", 0.0);
        return path;
    }
    auto attach = [&] { api_post("/replay/render", {{"selectionName", player}, {"cameraAttached", true}}); };
    struct Sample { double t, x, z; };
    std::vector<Sample> track;
    double height = 0;
    bool locked = false;
    // The attached camera only snaps onto them after a few real seconds of playback, so the pass starts that much early.
    // If it still locked on after the play began, the start would sit still while they move, so it runs again from earlier
    for (double preroll : {3.0 * speed, 8.0 * speed}) {
        track.clear();
        locked = false;
        std::vector<Sample> waiting;
        double locked_at = -1;
        seek(std::max(0.0, start - preroll));
        attach();
        if (lower(api_get("/replay/render").value("selectionName", "")) != lower(player))
            throw std::runtime_error("the replay has no champion selectable as " + player);
        api_post("/replay/playback", {{"paused", false}, {"speed", speed}});
        auto relocked = std::chrono::steady_clock::now();
        std::optional<std::pair<double, double>> origin; // where the camera sat before it locked on
        while (true) {
            double t = api_get("/replay/playback").value("time", 0.0);
            json r = api_get("/replay/render");
            if (t > end) break;
            json p = r["cameraPosition"];
            path.rotation = r["cameraRotation"];
            height = p.value("y", 0.0);
            double x = p.value("x", 0.0), z = p.value("z", 0.0);
            if (!origin) origin = std::make_pair(x, z);
            if (!locked) {
                double moved = std::hypot(x - origin->first, z - origin->second);
                if (moved < 1.0) { // not on them yet, or they're standing still
                    if (t >= start) waiting.push_back({t, x, z});
                    if (std::chrono::steady_clock::now() - relocked > std::chrono::seconds(1)) {
                        relocked = std::chrono::steady_clock::now();
                        attach();
                    }
                    continue;
                }
                locked = true;
                locked_at = t;
                if (moved < 1500) track = waiting; // it was on them all along
            }
            if (t < start) continue;
            bool on_track = track.empty() || std::hypot(x - track.back().x, z - track.back().z) < 1500 + 800 * (t - track.back().t);
            if (on_track) {
                track.push_back({t, x, z});
            } else if (std::chrono::steady_clock::now() - relocked > std::chrono::milliseconds(400)) {
                relocked = std::chrono::steady_clock::now();
                attach();
            }
        }
        api_post("/replay/playback", {{"paused", true}});
        if (!locked) track = waiting;
        if (!locked || locked_at <= start + 0.3) break; // on them from the start (or they never moved at all)
        // Late: drop the still samples from before the lock rather than show a frozen camera
        track.erase(std::remove_if(track.begin(), track.end(), [&](const Sample& s) { return s.t < locked_at; }), track.end());
    }
    api_post("/replay/playback", {{"paused", true}, {"speed", 1.0}});
    api_post("/replay/render", {{"cameraAttached", false}});
    if (track.size() < 10) throw std::runtime_error("couldn't follow " + player);
    std::set<std::pair<long, long>> spots;
    for (const Sample& sample : track) spots.insert({std::lround(sample.x), std::lround(sample.z)});
    // Never on them (it sat in one spot the whole time): measure again rather than play, or keep, a frozen camera
    if (spots.size() < 5) {
        if (retry) return camera_path(player, start, end, speed, cache, false);
        throw std::runtime_error("Couldn't get the camera onto " + player + " in the replay. Try again.");
    }
    bool still = !locked;

    // Fill the gaps, smooth the jitter, and key the camera every 1/20s
    auto at = [&](double t) {
        if (t <= track.front().t) return std::make_pair(track.front().x, track.front().z);
        for (size_t i = 1; i < track.size(); ++i)
            if (t <= track[i].t) {
                const Sample &a = track[i - 1], &b = track[i];
                double u = (t - a.t) / std::max(b.t - a.t, 1e-6);
                return std::make_pair(a.x + u * (b.x - a.x), a.z + u * (b.z - a.z));
            }
        return std::make_pair(track.back().x, track.back().z);
    };
    for (int i = 0; i <= int((end - start) * 20) + 1; ++i) {
        double t = start + i / 20.0, x = 0, z = 0, n = 0;
        for (int k = -8; k <= 8; ++k) {
            double w = std::exp(-0.5 * (k / 4.0) * (k / 4.0));
            auto [sx, sz] = at(t + k * 0.12 / 4);
            x += w * sx; z += w * sz; n += w;
        }
        path.keys.push_back(key(t, {{"x", x / n}, {"y", height}, {"z", z / n}}));
        path.x += x / n;
        path.z += z / n;
    }
    path.x /= path.keys.size();
    path.z /= path.keys.size();
    if (!still) std::ofstream(cache) << json{{"start", start}, {"end", end}, {"keys", path.keys}, {"rotation", path.rotation},
                                 {"x", path.x}, {"z", path.z}}.dump();
    return path;
}

void follow(const CameraPath& path, double start, double end) {
    api_post("/replay/sequence", {{"cameraPosition", path.keys},
                                  {"cameraRotation", json::array({key(start, path.rotation), key(end, path.rotation)})}});
}

// Plays the intro full screen on top of everything without taking focus, so League keeps running underneath and the
// replay can be lined up behind it; waits on its first frame until told to start, so the song's hit lands on the cut,
// and holds the closing white until told to fade into the replay. Esc, a stop, an error or a closed replay takes it
// down at once, so it can never hold the screen
class IntroOverlay {
    std::atomic<bool> go{false}, shown{false}, ended{false}, reveal{false}, abort{false};
    std::thread thread;

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

    void play(const fs::path& video, const fs::path& music) {
        SetProcessDPIAware();
        const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN), dw = W / 2, dh = H / 2;
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PotgIntro";
        RegisterClassW(&wc);
        HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                   wc.lpszClassName, L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
        SetLayeredWindowAttributes(win, 0, 255, LWA_ALPHA);

        // Decoded at half the screen size to keep the pipe light; it fills the screen, trimming the sides on 16:10
        std::string cmd = "ffmpeg -v error -i " + quote(video) + " -vf \"scale=" + std::to_string(dw) + ":" + std::to_string(dh) +
                          ":force_original_aspect_ratio=increase,crop=" + std::to_string(dw) + ":" + std::to_string(dh) +
                          "\" -f rawvideo -pix_fmt bgra -";
        Child decoder = launch(cmd, 'r');
        FILE* pipe = decoder.pipe;
        std::vector<unsigned char> frame(size_t(dw) * dh * 4);
        BITMAPINFO bmi{};
        bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), dw, -dh, 1, 32, BI_RGB};
        auto pump = [] { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m); };
        auto show = [&] {
            HDC dc = GetDC(win);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, W, H, 0, 0, dw, dh, frame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            ReleaseDC(win, dc);
        };
        auto next = [&] { return pipe && fread(frame.data(), 1, frame.size(), pipe) == frame.size(); };
        auto quit = [&] {
            if (escape_pressed()) request_stop();
            return abort || stop_wanted || !replay_window();
        };

        if (next()) {
            ShowWindow(win, SW_SHOWNOACTIVATE);
            show();
            shown = true;
            while (!go && !quit()) { pump(); sleep_s(0.005); }
            if (!quit()) PlaySoundW(music.wstring().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 1; !quit() && next(); ++i) {
                auto due = t0 + std::chrono::duration<double>(i / 30.0);
                if (std::chrono::steady_clock::now() > due + std::chrono::milliseconds(33)) continue; // behind: drop it
                while (std::chrono::steady_clock::now() < due) { pump(); sleep_s(0.002); }
                show();
            }
        }
        finish(decoder);
        ended = true;
        while (!reveal && !quit()) { pump(); sleep_s(0.01); }
        if (quit()) PlaySoundW(nullptr, nullptr, 0);
        auto f0 = std::chrono::steady_clock::now();
        for (double u = 0; u < 1 && !quit(); u = std::chrono::duration<double>(std::chrono::steady_clock::now() - f0).count() / 0.45) {
            SetLayeredWindowAttributes(win, 0, (BYTE)std::lround(255 * (1 - u)), LWA_ALPHA);
            pump();
            sleep_s(0.008);
        }
        DestroyWindow(win);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }

public:
    IntroOverlay(const fs::path& video, const fs::path& music) : thread([this, video, music] { play(video, music); }) {}
    void start() { go = true; }
    void wait_until_shown() const {
        while (!shown && !ended) sleep_s(0.01);
        check_stop();
    }
    void wait_until_played() const {
        while (!ended) sleep_s(0.01);
        check_stop();
    }
    void fade_out() { reveal = true; }
    ~IntroOverlay() {
        abort = true;
        go = reveal = true;
        if (thread.joinable()) thread.join();
    }
};

// A full-screen Play of the Game card from the click until the intro, so the replay's own loading screen, the wait and the
// intro being made all happen behind it. Esc or a stop ends the play; an error or the play moving on takes it down, so it
// can never hold the screen
std::mutex step_lock;
std::wstring loading_step; // what's happening, shown under LOADING

class LoadingScreen {
    std::atomic<bool> done{false};
    InputWatch input;
    std::thread thread;

    void run() {
        SetProcessDPIAware();
        const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
        AddFontResourceExW((ASSETS / "fonts" / "BeaufortForLOL-Bold.otf").wstring().c_str(), FR_PRIVATE, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // the arrow
        wc.lpszClassName = L"PotgLoading";
        RegisterClassW(&wc);
        HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Play of the Game", WS_POPUP, 0, 0, W, H,
                                   nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(win, SW_SHOW);
        bring_to_front(win); // keys go to us, not to whatever was in front underneath

        auto font = [&](int height) {
            return CreateFontW(height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Beaufort for LOL");
        };
        HFONT title = font(H / 14), label = font(H / 50);
        HDC screen = GetDC(win), dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, W, H);
        SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
        HBRUSH back = CreateSolidBrush(RGB(1, 10, 19)), track = CreateSolidBrush(RGB(30, 35, 40)), gold = CreateSolidBrush(RGB(200, 170, 110));
        auto text = [&](HFONT f, COLORREF color, const wchar_t* s, int top, int height) {
            SelectObject(dc, f);
            SetTextColor(dc, color);
            RECT r{0, top, W, top + height};
            DrawTextW(dc, s, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        };
        // Switching to another app (anything but us, League, its client, or Explorer's Alt+Tab and taskbar) puts the card away
        // What's in front: League (us, the replay, the client, the Riot Client), nothing in particular (Explorer's Alt+Tab
        // and taskbar), or some other app they switched to
        enum Front { LEAGUE, NEUTRAL, OTHER };
        auto in_front = [] {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (!pid) return NEUTRAL;
            if (pid == GetCurrentProcessId()) return LEAGUE;
            wchar_t path[MAX_PATH] = L"";
            DWORD size = MAX_PATH;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process) {
                QueryFullProcessImageNameW(process, 0, path, &size);
                CloseHandle(process);
            }
            std::wstring name = fs::path(path).filename().wstring();
            if (_wcsicmp(name.c_str(), L"explorer.exe") == 0) return NEUTRAL;
            for (const wchar_t* league : {L"League of Legends.exe", L"LeagueClientUx.exe", L"LeagueClientUxRender.exe", L"LeagueClient.exe",
                                          L"Riot Client.exe", L"RiotClientServices.exe", L"RiotClientUx.exe"})
                if (_wcsicmp(name.c_str(), league) == 0) return LEAGUE;
            return OTHER;
        };
        bool away = false;
        auto t0 = std::chrono::steady_clock::now(), other_since = t0;
        while (!done) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            if (escape_pressed()) request_stop();
            if (stop_wanted) break;
            Front front = in_front();
            if (front != OTHER) other_since = std::chrono::steady_clock::now();
            if (!away && front == OTHER) {
                // They switched (they pressed or clicked just before) and stayed there most of a second
                if (user_just_acted() && std::chrono::steady_clock::now() - other_since > std::chrono::milliseconds(700)) {
                    away = user_away = true;
                    ShowWindow(win, SW_HIDE); // keeps loading; the intro still comes up when it's ready
                } else if (!user_just_acted()) {
                    bring_to_front(win); // Windows moved focus on its own (League starting up), so take it back
                }
            } else if (away && front == LEAGUE) { // back to League: cover it again
                away = user_away = false;
                ShowWindow(win, SW_SHOW);
                bring_to_front(win);
            }
            if (away) {
                // The replay waits minimized rather than sitting over their other app; it isn't in front, so focus stays put
                HWND league = FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client");
                if (league && !replay_on_stage && !IsIconic(league) && GetForegroundWindow() != league) ShowWindow(league, SW_SHOWMINNOACTIVE);
                sleep_s(0.1);
                continue;
            }
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            RECT all{0, 0, W, H};
            FillRect(dc, &all, back);
            text(title, RGB(240, 230, 210), L"PLAY OF THE GAME", H * 40 / 100, H / 10);
            // A gold sliver sweeping along a track under the title
            int bw = W / 5, bx = (W - bw) / 2, by = H * 53 / 100, seg = bw / 4;
            RECT bar{bx, by, bx + bw, by + 2};
            FillRect(dc, &bar, track);
            int sx = bx + (int)((bw - seg) * (0.5 - 0.5 * std::cos(t * 2.2)));
            RECT sweep{sx, by, sx + seg, by + 2};
            FillRect(dc, &sweep, gold);
            text(label, RGB(160, 155, 140), L"LOADING", by + H / 40, H / 30);
            std::wstring step;
            {
                std::lock_guard<std::mutex> guard(step_lock);
                step = loading_step;
            }
            text(label, RGB(160, 155, 140), step.c_str(), by + H / 40 + H / 28, H / 30);
            text(label, RGB(91, 90, 86), L"ESC TO CANCEL", H - H / 10, H / 30);
            BitBlt(screen, 0, 0, W, H, dc, 0, 0, SRCCOPY);
            sleep_s(1.0 / 60);
        }
        DeleteObject(back); DeleteObject(track); DeleteObject(gold);
        DeleteObject(title); DeleteObject(label);
        DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(win, screen);
        DestroyWindow(win);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }

public:
    LoadingScreen() : thread([this] { run(); }) {}
    ~LoadingScreen() {
        done = true;
        if (thread.joinable()) thread.join();
    }
};

std::mutex loading_lock;
std::unique_ptr<LoadingScreen> loading;
std::atomic<bool> loading_up{false}; // a play is loading behind the loading screen

// Holds a still of the screen on top while windows change underneath (the replay closing, the client coming back and
// drawing itself), then fades it away; nearly opaque, so the client underneath still counts as on screen and draws
class ScreenStill {
    std::atomic<bool> shown{false}, fading{false};
    std::thread thread;

    void run(HBITMAP still, int W, int H) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PotgStill";
        RegisterClassW(&wc);
        HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                   wc.lpszClassName, L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);
        SetLayeredWindowAttributes(win, 0, 254, LWA_ALPHA);
        ShowWindow(win, SW_SHOWNOACTIVATE);
        HDC src = CreateCompatibleDC(nullptr);
        SelectObject(src, still);
        auto paint = [&] {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            HDC dc = GetDC(win);
            BitBlt(dc, 0, 0, W, H, src, 0, 0, SRCCOPY);
            ReleaseDC(win, dc);
        };
        paint();
        shown = true;
        while (!fading) { paint(); sleep_s(0.02); }
        auto f0 = std::chrono::steady_clock::now();
        for (double u = 0; u < 1; u = std::chrono::duration<double>(std::chrono::steady_clock::now() - f0).count() / 0.35) {
            SetLayeredWindowAttributes(win, 0, (BYTE)std::lround(254 * (1 - u)), LWA_ALPHA);
            paint();
            sleep_s(0.01);
        }
        DestroyWindow(win);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        DeleteDC(src);
        DeleteObject(still);
    }

public:
    ScreenStill() {
        const int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        HBITMAP still = CreateCompatibleBitmap(screen, W, H);
        SelectObject(mem, still);
        BitBlt(mem, 0, 0, W, H, screen, 0, 0, SRCCOPY);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        thread = std::thread([this, still, W, H] { run(still, W, H); });
        while (!shown) sleep_s(0.005);
    }
    ~ScreenStill() {
        fading = true;
        if (thread.joinable()) thread.join();
    }
};

// The engine's recorder: drops recordings under about a second, and writes the file a moment after it stops
void record(const fs::path& webm, double start, double end, int fps) {
    fs::remove(webm);
    api_post("/replay/recording", {{"recording", true}, {"path", fs::absolute(webm).string()}, {"codec", "webm"},
                                   {"startTime", start}, {"endTime", end}, {"framesPerSecond", fps},
                                   {"enforceFrameRate", true}, {"lossless", false}});
    api_post("/replay/playback", {{"paused", false}});
    while (api_get("/replay/recording").value("recording", false)) sleep_s(0.25);
    api_post("/replay/playback", {{"paused", true}});
    for (int i = 0; i < 40 && !fs::exists(webm); ++i) sleep_s(0.25);
    if (!fs::exists(webm)) throw std::runtime_error("the replay didn't write " + webm.string());
}

// ---- Drawing ----

struct Rgb { float r, g, b; };
Rgb hex(unsigned v) { return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f}; }
Rgb mix(Rgb a, Rgb b, float u) { return {a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u}; }
float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float ease(float t, float t0, float d) { float p = clamp01((t - t0) / d); return 1 - std::pow(1 - p, 3.f); }

// League client palette
const Rgb GOLD = hex(0xC8AA6E), GOLD_DARK = hex(0x785A28), CREAM = hex(0xF0E6D2), GREY = hex(0xA09B8C), NAVY = hex(0x010A13);

struct Canvas {
    int w, h;
    std::vector<float> c, a; // colour premultiplied by coverage, and coverage
    Canvas(int w, int h, float alpha) : w(w), h(h), c(size_t(w) * h * 3, 0.f), a(size_t(w) * h, alpha) {}
    void blend(int x, int y, Rgb col, float alpha) {
        if (x < 0 || y < 0 || x >= w || y >= h || alpha <= 0) return;
        size_t i = size_t(y) * w + x;
        alpha = std::min(alpha, 1.f);
        c[i * 3] = c[i * 3] * (1 - alpha) + col.r * alpha;
        c[i * 3 + 1] = c[i * 3 + 1] * (1 - alpha) + col.g * alpha;
        c[i * 3 + 2] = c[i * 3 + 2] * (1 - alpha) + col.b * alpha;
        a[i] = a[i] + alpha * (1 - a[i]);
    }
};

struct Image {
    int w = 0, h = 0, n = 0;
    std::vector<unsigned char> px;
};

Image load_image(const fs::path& p, int channels) {
    Image im;
    unsigned char* data = stbi_load(p.string().c_str(), &im.w, &im.h, &im.n, channels);
    if (!data) throw std::runtime_error("can't read image " + p.string());
    im.n = channels;
    im.px.assign(data, data + size_t(im.w) * im.h * channels);
    stbi_image_free(data);
    return im;
}

std::u32string utf32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
        out += cp;
        i += n;
    }
    return out;
}

std::u32string upper(std::u32string s) {
    for (auto& c : s) if (c < 128) c = (char32_t)std::toupper((int)c);
    return s;
}

// A name the League fonts can't draw (Arabic, Korean, Chinese...), laid out by Windows so letters join and run right to
// left where they should, as a coverage mask
struct TextMask {
    int w = 0, h = 0;
    std::vector<float> a;
};
TextMask system_text(const std::u32string& text, int px, int max_width) {
    std::wstring w;
    for (char32_t ch : text) {
        if (ch > 0xFFFF) {
            ch -= 0x10000;
            w += wchar_t(0xD800 + (ch >> 10));
            w += wchar_t(0xDC00 + (ch & 0x3FF));
        } else {
            w += wchar_t(ch);
        }
    }
    HDC dc = CreateCompatibleDC(nullptr);
    auto make_font = [&](int size) {
        return CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT font = make_font(px);
    HGDIOBJ old_font = SelectObject(dc, font);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, w.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    if (r.right > max_width) { // too long: smaller until it fits
        px = std::max(12, px * max_width / std::max(1, (int)r.right));
        SelectObject(dc, old_font);
        DeleteObject(font);
        font = make_font(px);
        SelectObject(dc, font);
        r = {0, 0, 0, 0};
        DrawTextW(dc, w.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    }
    TextMask m;
    m.w = r.right + 8;
    m.h = r.bottom + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), m.w, -m.h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, size_t(m.w) * m.h * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT d{4, 4, m.w, m.h};
    DrawTextW(dc, w.c_str(), -1, &d, DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    m.a.resize(size_t(m.w) * m.h);
    const unsigned char* px8 = static_cast<const unsigned char*>(bits);
    for (size_t i = 0; i < m.a.size(); ++i) m.a[i] = px8[i * 4 + 1] / 255.f;
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return m;
}

struct Font {
    std::string data;
    stbtt_fontinfo info{};
    explicit Font(const fs::path& p) : data(read_file(p)) {
        auto* d = reinterpret_cast<const unsigned char*>(data.data());
        if (data.empty() || !stbtt_InitFont(&info, d, stbtt_GetFontOffsetForIndex(d, 0))) throw std::runtime_error("bad font " + p.string());
    }
    float scale(float size) const { return stbtt_ScaleForMappingEmToPixels(&info, size); }
    float advance(char32_t cp, char32_t next, float size, float tracking) const {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, (int)cp, &adv, &lsb);
        float kern = next ? (float)stbtt_GetCodepointKernAdvance(&info, (int)cp, (int)next) : 0.f;
        return (adv + kern) * scale(size) + tracking * size;
    }
    // Distance from the y a line is drawn at (its ascent line) down to the baseline, and down to a letter's top
    float baseline(float size) const {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        return ascent * scale(size);
    }
    float cap_top(char32_t cp, float size) const {
        int x0, y0, x1, y1;
        stbtt_GetCodepointBox(&info, (int)cp, &x0, &y0, &x1, &y1);
        return baseline(size) - y1 * scale(size);
    }
    // Gap between the pen and where a letter's ink starts, so lines can line up by their ink
    float lead(char32_t cp, float size) const {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, (int)cp, &adv, &lsb);
        return lsb * scale(size);
    }
    float width(const std::u32string& s, float size, float tracking = 0) const {
        float w = 0;
        for (size_t i = 0; i < s.size(); ++i) w += advance(s[i], i + 1 < s.size() ? s[i + 1] : 0, size, tracking);
        return w;
    }
    // One glyph with its top at y, coloured top-to-bottom from one colour to another, under a soft drop shadow
    void glyph(Canvas& c, char32_t cp, float size, float x, float y, Rgb top, Rgb bottom, float alpha, bool shadow = true) const {
        float s = scale(size);
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        float base = y + ascent * s, shift = x - std::floor(x);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&info, (int)cp, s, s, shift, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0 || alpha <= 0) return;
        std::vector<unsigned char> bmp(size_t(gw) * gh);
        stbtt_MakeCodepointBitmapSubpixel(&info, bmp.data(), gw, gh, gw, s, s, shift, 0, (int)cp);
        int ox = (int)std::floor(x) + x0, oy = (int)std::lround(base) + y0;
        float cap = ascent * s * 0.95f;
        for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
            for (int j = 0; j < gh; ++j)
                for (int i = 0; i < gw; ++i) {
                    float cov = bmp[size_t(j) * gw + i] / 255.f * alpha;
                    if (pass == 0) c.blend(ox + i + 2, oy + j + 3, NAVY, cov * 0.6f);
                    else c.blend(ox + i, oy + j, mix(top, bottom, clamp01((oy + j - y) / cap)), cov);
                }
    }
    float text(Canvas& c, const std::u32string& s, float size, float x, float y, Rgb top, Rgb bottom, float alpha, float tracking = 0) const {
        float pen = x;
        for (size_t i = 0; i < s.size(); ++i) {
            glyph(c, s[i], size, pen, y, top, bottom, alpha);
            pen += advance(s[i], i + 1 < s.size() ? s[i + 1] : 0, size, tracking);
        }
        return pen - x;
    }
};

void diamond(Canvas& c, float cx, float cy, float r, Rgb col, float alpha) {
    for (int y = (int)(cy - r - 1); y <= cy + r + 1; ++y)
        for (int x = (int)(cx - r - 1); x <= cx + r + 1; ++x)
            c.blend(x, y, col, alpha * clamp01(r - (std::abs(x + 0.5f - cx) + std::abs(y + 0.5f - cy)) + 0.5f));
}

// Summoner icon in a round gold frame, like the client's profile frame
void icon_frame(Canvas& c, const Image& icon, float cx, float cy, float r, float alpha) {
    for (int y = (int)(cy - r - 9); y <= cy + r + 9; ++y)
        for (int x = (int)(cx - r - 9); x <= cx + r + 9; ++x) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::hypot(dx, dy);
            c.blend(x, y, NAVY, alpha * clamp01(r + 8.5f - d));
            c.blend(x, y, mix(CREAM, GOLD_DARK, clamp01((dy / r + 1) / 2)), alpha * clamp01(r + 5.5f - d));
            float cov = clamp01(r - d + 0.5f);
            if (cov <= 0) continue;
            int u = std::clamp(int((dx / r * 0.5f + 0.5f) * icon.w), 0, icon.w - 1);
            int v = std::clamp(int((dy / r * 0.5f + 0.5f) * icon.h), 0, icon.h - 1);
            const unsigned char* p = &icon.px[(size_t(v) * icon.w + u) * 3];
            c.blend(x, y, {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f}, alpha * cov);
        }
}

void box_blur(std::vector<float>& img, int w, int h, int radius) {
    std::vector<float> tmp(img.size());
    for (int pass = 0; pass < 3; ++pass)
        for (int dir = 0; dir < 2; ++dir) {
            int len = dir ? h : w, lines = dir ? w : h;
            for (int l = 0; l < lines; ++l)
                for (int ch = 0; ch < 3; ++ch) {
                    auto at = [&](int i) -> float& { i = std::clamp(i, 0, len - 1); return img[(dir ? size_t(i) * w + l : size_t(l) * w + i) * 3 + ch]; };
                    float sum = 0;
                    for (int i = -radius; i <= radius; ++i) sum += at(i);
                    for (int i = 0; i < len; ++i) {
                        tmp[(dir ? size_t(i) * w + l : size_t(l) * w + i) * 3 + ch] = sum / (2 * radius + 1);
                        sum += at(i + radius + 1) - at(i - radius);
                    }
                }
            img.swap(tmp);
        }
}

// Raw RGB frames into an ffmpeg encode
class Encoder {
    Child ffmpeg;
public:
    Encoder(const std::string& args) : ffmpeg(launch("ffmpeg -v error -y " + args, 'w')) {}
    void write(const std::vector<unsigned char>& frame) { fwrite(frame.data(), 1, frame.size(), ffmpeg.pipe); }
    ~Encoder() { finish(ffmpeg); }
};

} // namespace

// ================================================================================================

void set_potg_status(std::function<void(const std::string&)> sink) { status_sink = std::move(sink); }

// The songs to pick from; what each Music field means is in potg_video.hpp
Music pick_music(const std::string& name) {
    Music legends;
    legends.file = "legends_never_die.wav";
    legends.parts = {
        //from    to
        {41.525, 48.382}, // the chorus, starting on the cut: its opening phrase twice (each 3.4286 s, two bars at 140 BPM)
        {51.811, 60.6},   // skips the phrase's third repeat, then runs to the song's dead stop at 1:00.4 plus its ring-out
    };
    // The song's opening plays three times before the chorus. The intro starts in the soft first pass, then jumps
    // 27.459 s ahead (exactly two passes) to the same spot of the third, which runs into the chorus
    legends.skip = 27.459;
    legends.skip_at = 39.56; // the crossfade into the third pass is done by 0:39.56, two seconds before the cut
    legends.crossfade = 1.8; // seconds the first pass fades into the third
    // intro_from and intro_to stay -1: the intro's music simply runs up to the first part. fade stays false: it ends on
    // the song's own dead stop

    Music coming_alive;
    coming_alive.file = "coming_alive.wav";
    coming_alive.parts = {
        //from   to
        {45.12, 62.2}, // from the drop after the break, which lands on the cut, for 17 seconds under the replay
    };
    coming_alive.intro_from = 39.5; // the intro starts on the hit at 0:39.5
    coming_alive.fade = true;       // the song has no clean stop here, so it fades out

    static const std::vector<std::pair<std::string, Music>> songs = {{"legends", legends}, {"coming_alive", coming_alive}};
    for (const auto& [key, music] : songs)
        if (key == name) return music;
    if (!name.empty()) throw std::runtime_error("no song called " + name);
    std::mt19937 rng(std::random_device{}());
    return songs[rng() % songs.size()].second;
}

fs::path potg_output_dir(long long game_id) {
    fs::path dir = ASSETS.parent_path() / "potg" / std::to_string(game_id);
    fs::create_directories(dir);
    return dir;
}

bool replay_running() {
    cpr::Response r = cpr::Get(cpr::Url{GAME_API + "/replay/playback"}, cpr::VerifySsl{false}, cpr::Timeout{2000});
    return r.status_code == 200;
}

void quiet_replay(bool on) {
    std::lock_guard<std::mutex> guard(quiet_lock);
    if (on && !quiet) {
        quiet = true;
        quiet_thread = std::thread([] {
            while (quiet) {
                DWORD pid = replay_pid();
                if (pid && replay_running()) set_muted(pid, true); // a replay, never a real game
                sleep_s(0.3);
            }
        });
    } else if (!on && quiet) {
        quiet = false;
        if (quiet_thread.joinable()) quiet_thread.join();
        if (DWORD pid = replay_pid()) set_muted(pid, false);
    }
}

void bring_client_back() {
    if (HWND client = FindWindowW(L"RCLIENT", L"League of Legends")) bring_to_front(client);
}

void show_step(const std::string& step) {
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, step.c_str(), (int)step.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, step.c_str(), (int)step.size(), wide.data(), (int)wide.size());
    std::lock_guard<std::mutex> guard(step_lock);
    loading_step = wide;
}

void show_loading() {
    std::lock_guard<std::mutex> guard(loading_lock);
    loading_up = true;
    replay_on_stage = false;
    user_away = false;
    if (!loading) loading = std::make_unique<LoadingScreen>();
}

void hide_loading() {
    std::unique_ptr<LoadingScreen> old;
    std::lock_guard<std::mutex> guard(loading_lock);
    loading_up = false;
    old = std::move(loading);
}

void close_replay() {
    HWND league = FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client");
    // Only ever a replay: one that answers on the replay API, or, while a play is loading (before the replay answers),
    // the one the helper just launched, since the client won't start a replay during a real game
    if (!league || !(replay_running() || loading_up)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(league, &pid);
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    TerminateProcess(process, 0);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
}

void hold_replay_at(double t) {
    // The API answers during the loading screen, so wait for the replay to start playing before moving it
    for (int i = 0; i < 240; ++i) {
        json playback = api_get("/replay/playback");
        if (playback.value("length", 0.0) > 0.0 && (playback.value("time", 0.0) > 0.5 || playback.value("paused", false))) break;
        sleep_s(0.5);
    }
    seek(t);
}

void request_stop() {
    stop_wanted = true;
    close_replay(); // the game exits with it, before League can open its own menu on that Esc
}

void clear_stop() { stop_wanted = false; }

void check_stop() {
    if (escape_pressed()) request_stop();
    if (stop_wanted) throw std::runtime_error("Stopped.");
}

std::vector<std::string> replay_player_names() {
    std::vector<std::string> names;
    cpr::Response r = cpr::Get(cpr::Url{GAME_API + "/liveclientdata/playerlist"}, cpr::VerifySsl{false}, cpr::Timeout{5000});
    json list = r.status_code == 200 ? json::parse(r.text, nullptr, false) : json();
    if (!list.is_array()) return names;
    for (const auto& p : list) {
        std::string name = p.value("riotIdGameName", "");
        if (name.empty()) name = p.value("summonerName", "").substr(0, p.value("summonerName", "").find('#'));
        names.push_back(name);
    }
    return names;
}

std::optional<std::vector<std::pair<double, std::string>>> replay_steals() {
    // The event feed only lists what has happened up to the playback time, so read it near the end of the game; not at
    // the very end, where the camera locks onto the nexus and won't follow anyone for seconds afterwards
    json playback = api_get("/replay/playback");
    double was = playback.value("time", 0.0);
    seek(std::max(0.0, playback.value("length", 0.0) - 30.0));
    sleep_s(2.0);
    cpr::Response r = cpr::Get(cpr::Url{GAME_API + "/liveclientdata/eventdata"}, cpr::VerifySsl{false}, cpr::Timeout{5000});
    seek(was);
    json feed = r.status_code == 200 ? json::parse(r.text, nullptr, false) : json();
    if (!feed.is_object() || !feed.contains("Events")) return std::nullopt;
    std::vector<std::pair<double, std::string>> steals;
    for (const auto& e : feed["Events"]) {
        if (e.value("Stolen", "False") != "True") continue;
        std::string name = e.value("EventName", "");
        if (name == "DragonKill") steals.push_back({e.value("EventTime", 0.0), e.value("DragonType", "") == "Elder" ? "ELDER_DRAGON" : "DRAGON"});
        if (name == "BaronKill") steals.push_back({e.value("EventTime", 0.0), "BARON_NASHOR"});
    }
    return steals;
}

PotgPlayer find_potg_player(const json& game, const std::string& name) {
    PotgPlayer p;
    p.name = name;
    int id = 0;
    for (const auto& identity : game.value("participantIdentities", json::array())) {
        json player = identity.value("player", json::object());
        if (lower(player.value("gameName", "")) == lower(name) || lower(player.value("summonerName", "")) == lower(name)) {
            id = identity.value("participantId", 0);
            p.profile_icon = player.value("profileIcon", 0);
        }
    }
    for (const auto& participant : game.value("participants", json::array()))
        if (participant.value("participantId", -1) == id) p.champion_id = participant.value("championId", 0);
    // Match history has no skins, the replay does
    for (const auto& player : api_get("/liveclientdata/playerlist"))
        if (lower(player.value("riotIdGameName", "")) == lower(name)) p.skin = player.value("skinID", 0);
    return p;
}

ReplayClip record_replay_clip(const std::string& player, double t0, double t1, const fs::path& out, const fs::path& cache) {
    const double LEAD = 1.5; // starting a recording resets the camera for a moment, so record early and cut this off
    double start = std::max(0.0, t0 - LEAD);
    if (user_away) { // they're in another app: wait for them to come back to League rather than pull them back
        status("Ready: switch back to League to watch it");
        while (user_away) {
            check_stop();
            sleep_s(0.2);
        }
    }
    json changes = HUD_OFF;
    changes["cameraAttached"] = false;
    RenderSettings settings(changes);
    hide_loading();
    LeagueInFront front;
    CameraPath path = camera_path(player, start, t1, 1.0, cache);
    seek(start);
    follow(path, start, t1);
    fs::path raw = fs::path(out).replace_extension(".webm");
    record(raw, start, t1, 60);

    // Frames carry wall-clock stamps, so restamp at 60fps, drop the lead-in, and crop the 16:10 window to 16:9 1080p
    run("ffmpeg -v error -y -i " + quote(raw) + " -vf \"setpts=N/60/TB,fps=60,trim=start_frame=" +
        std::to_string(std::lround((t0 - start) * 60)) + ",setpts=PTS-STARTPTS,crop=iw:iw*9/16,scale=1920:1080\"" +
        " -an -c:v libx264 -preset fast -crf 17 -pix_fmt yuv420p -r 60 " + quote(out));
    fs::remove(raw);
    return {out, path.x, path.z};
}

void watch_play(const std::string& player, double t0, double t1, const fs::path& intro, const Music& music, const fs::path& cache) {
    if (user_away) { // they're in another app: wait for them to come back to League rather than pull them back
        status("Ready: switch back to League to watch it");
        while (user_away) {
            check_stop();
            sleep_s(0.2);
        }
    }
    CloseReplayAfter close;
    // The camera path is measured in a fast pass behind the loading screen: League keeps moving its camera under a plain
    // window (not under a see-through one, or off-screen or minimized), and the replay is muted. Champions, effects and
    // HUD are hidden as well, so nothing leaks if they look behind it
    json bare = HUD_OFF;
    for (const char* k : {"interfaceAll", "champions", "characters", "minions", "particles", "healthBarChampions", "healthBarMinions",
                          "healthBarPets", "healthBarStructures", "healthBarWards", "floatingText", "banners", "outlineHover"}) bare[k] = false;
    bare["cameraAttached"] = false;
    std::optional<RenderSettings> hidden(std::in_place, bare);
    replay_on_stage = true;
    LeagueInFront front(true); // the job brings the client back afterwards
    status("Lining up the camera...");
    CameraPath path = camera_path(player, t0, t1, 4.0, cache);

    IntroOverlay overlay(intro, music_bed(music, duration(intro))); // on top the whole time the replay is being lined up
    overlay.wait_until_shown();
    hide_loading(); // the intro's first frame is up, so the loading screen can go without a gap
    hidden.reset();
    status("Playing the play of the game in the replay...");
    json changes = HUD_OFF;
    changes["cameraAttached"] = false;
    RenderSettings settings(changes);
    seek(t0);
    follow(path, t0, t1);
    overlay.start();
    overlay.wait_until_played();
    if (HWND league = FindWindowW(L"RiotWindowClass", L"League of Legends (TM) Client")) bring_to_front(league); // in case they clicked away
    quiet_replay(false);
    api_post("/replay/playback", {{"paused", false}, {"speed", 1.0}});
    overlay.fade_out();
    ready_client_behind(); // a hidden window takes a moment to draw, so give it the whole scene
    for (;;) {
        json playback = api_get("/replay/playback");
        if (playback.value("time", 0.0) >= t1) break;
        if (playback.value("paused", false)) { // they paused it themselves, so it's theirs now
            close.keep = true;
            front.keep();
            return;
        }
        sleep_s(0.1);
    }
    api_post("/replay/playback", {{"paused", true}});
    PlaySoundW(nullptr, nullptr, 0);
    ready_client_behind();
    sleep_s(1.5); // a beat on the last frame before going back to the client
    // Straight from that frame to the client: putting League's settings back first would flash its HUD for a moment, and
    // the client takes a moment to draw once it's in front, so the last frame is held on top meanwhile and fades into it
    ScreenStill still;
    bring_client_back();
    close_replay();
    sleep_s(1.0);
}

namespace {

// One low shot across the dragon pit, reused behind every intro; shot from the open replay the first time it's needed
fs::path shared_background() {
    fs::path out = ASSETS / "background.png";
    if (fs::exists(out)) return out;
    json position = {{"x", 11660}, {"y", 650}, {"z", 2810}}, rotation = {{"x", -41.6}, {"y", 10}, {"z", 0}};
    // Just the map: no units, bars, numbers or fog
    json changes = HUD_OFF;
    for (const char* k : {"champions", "characters", "healthBarChampions", "healthBarMinions", "healthBarStructures", "healthBarWards",
                          "healthBarPets", "floatingText", "fogOfWar", "interfaceAnnounce", "interfaceKillCallouts"}) changes[k] = false;
    changes["cameraAttached"] = false;
    fs::path webm = fs::path(out).replace_extension(".webm");
    {
        RenderSettings settings(changes);
        api_post("/replay/sequence", {{"cameraPosition", json::array({key(0, position), key(1e5, position)})},
                                      {"cameraRotation", json::array({key(0, rotation), key(1e5, rotation)})}});
        LeagueInFront front;
        double t = api_get("/replay/playback").value("time", 300.0);
        seek(t);
        sleep_s(1.0);
        record(webm, t, t + 1.0, 30);
    }
    run("ffmpeg -v error -y -i " + quote(webm) + " -vf \"select=eq(n\\,15),crop=iw:iw*9/16,scale=2560:1440\" -frames:v 1 " + quote(out));
    fs::remove(webm);
    return out;
}

} // namespace

fs::path make_intro(const PotgPlayer& player) {
    const fs::path models = ASSETS / "models";
    json champ = json::parse(fetch(CDRAGON + "/champions/" + std::to_string(player.champion_id) + ".json",
                                   models / ("champ_" + std::to_string(player.champion_id) + ".json")), nullptr, false);
    if (!champ.is_object()) throw std::runtime_error("unknown champion " + std::to_string(player.champion_id));
    std::string alias = lower(champ.value("alias", "")), champ_name = champ.value("name", alias);

    // Skin ids are champion id * 1000 + skin number; chromas are recolours without a model of their own, so use their skin
    int skin_id = player.champion_id * 1000 + player.skin, model_id = player.champion_id * 1000;
    for (const auto& s : champ.value("skins", json::array())) {
        bool match = s.value("id", 0) == skin_id;
        for (const auto& c : s.value("chromas", json::array())) match = match || c.value("id", 0) == skin_id;
        if (match) model_id = s.value("id", 0);
    }
    auto model = [&](int id) { return models / (alias + "_" + std::to_string(id) + ".glb"); };
    auto model_url = [&](int id) { return "https://cdn.modelviewer.lol/lol/models/" + alias + "/" + std::to_string(id) + "/model.glb"; };
    if (fetch(model_url(model_id), model(model_id)).empty()) { // skin not hosted: base model
        model_id = player.champion_id * 1000;
        if (fetch(model_url(model_id), model(model_id)).empty()) throw std::runtime_error("no model for " + champ_name);
    }

    // A finished intro only depends on the skin, the name and the icon, so it's made once and reused
    const std::string look = "_fit"; // the framing; renders made with an older one are made again
    const std::string card = "_steady"; // the title card's layout; intros made with an older one are put together again
    fs::path out = ASSETS / "cache" / (alias + "_" + std::to_string(model_id) + look + card + "_" + std::to_string(player.profile_icon) + "_" +
                                       lower(player.name) + ".mp4");
    if (fs::exists(out)) return out;
    fs::create_directories(out.parent_path());
    fs::path background = shared_background();

    // The 3D render only depends on the skin, so it's cached; Blender can only be scripted in Python, hence auto_intro.py
    fs::path frames = models / (alias + "_" + std::to_string(model_id) + look + "_frames");
    if (!fs::exists(frames / "f_0150.png")) {
        status("Making the intro (only the first time for this champion and skin)...");
        run(quote(BLENDER) + " -b --python-exit-code 1 -P " + quote(ASSETS / "auto_intro.py") + " -- " + quote(model(model_id)) + " " +
            quote(frames) + " 75 > NUL"); // 75%: it plays at half size during a watch, and renders in about half the time
    }
    std::vector<float> orbit;
    {
        std::ifstream f(frames / "orbit.txt");
        for (float v; f >> v;) orbit.push_back(v);
    }

    const int W = 1920, H = 1080, FPS = 30, N = (int)orbit.size(), TOTAL = N + 11; // 11 extra frames for the white flash
    const float DUR = N / (float)FPS;

    // Background: the map shot with its empty sky painted as a dusk gradient, darkened and softly out of focus
    Image shot = load_image(background, 3);
    const int PW = shot.w, PH = shot.h;
    std::vector<float> plate(size_t(PW) * PH * 3);
    const unsigned char* sky = &shot.px[(size_t(2) * PW + PW / 2) * 3];
    auto sky_like = [&](size_t i) {
        const unsigned char* p = &shot.px[i * 3];
        return std::abs(p[0] - sky[0]) + std::abs(p[1] - sky[1]) + std::abs(p[2] - sky[2]) < 24;
    };
    std::vector<float> is_sky(size_t(PW) * PH, 0.f);
    std::deque<size_t> fill;
    for (int x = 0; x < PW; ++x) if (sky_like(x)) { is_sky[x] = 1; fill.push_back(x); }
    while (!fill.empty()) { // the empty space past the map edge, flooded from the top
        size_t i = fill.front();
        fill.pop_front();
        int x = int(i % PW), y = int(i / PW);
        for (auto [nx, ny] : {std::pair{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}}) {
            if (nx < 0 || ny < 0 || nx >= PW || ny >= PH) continue;
            size_t j = size_t(ny) * PW + nx;
            if (!is_sky[j] && sky_like(j)) { is_sky[j] = 1; fill.push_back(j); }
        }
    }
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < PW; ++x) {
            size_t i = size_t(y) * PW + x;
            Rgb dusk = mix(NAVY, hex(0x0A323C), clamp01(y / (PH * 0.45f)));
            float vx = (x / (float)PW - 0.5f) * 2, vy = (y / (float)PH - 0.5f) * 2;
            float shade = 0.62f * (1 - 0.3f * (vx * vx + vy * vy) / 2);
            const unsigned char* p = &shot.px[i * 3];
            Rgb col = is_sky[i] ? dusk : Rgb{p[0] / 255.f * shade, p[1] / 255.f * shade, p[2] / 255.f * shade};
            plate[i * 3] = col.r; plate[i * 3 + 1] = col.g; plate[i * 3 + 2] = col.b;
        }
    box_blur(plate, PW, PH, 4);

    Image icon = player.profile_icon > 0 && !fetch(CDRAGON + "/profile-icons/" + std::to_string(player.profile_icon) + ".jpg",
                                                  models / ("icon_" + std::to_string(player.profile_icon) + ".jpg")).empty()
                     ? load_image(models / ("icon_" + std::to_string(player.profile_icon) + ".jpg"), 3) : Image{};
    const Font title_f(ASSETS / "fonts" / "BeaufortForLOL-Heavy.otf"), name_f(ASSETS / "fonts" / "BeaufortForLOL-Bold.otf"),
        sub_f(ASSETS / "fonts" / "Spiegel-SemiBold.otf");

    // Title card: Overwatch's layout (icon, title, name decoding in, champion), League's type and colours.
    // PLAY OF THE GAME is the headline, then the name, then AS <CHAMPION>; each line shrinks only if it's too wide
    const float TITLE = 104, NAME = 64, SUB = 56;              // line sizes in px
    const float TITLE_TRACK = 0.02f, NAME_TRACK = 0.06f;      // extra letter spacing, in em
    const std::u32string title = U"PLAY OF THE GAME", name = upper(utf32(player.name)), sub = U"AS " + upper(utf32(champ_name));
    float title_size = std::min(TITLE, TITLE * 1050 / std::max(1.f, title_f.width(title, TITLE, TITLE_TRACK)));
    float name_size = std::min(NAME, NAME * 900 / std::max(1.f, name_f.width(name, NAME, NAME_TRACK)));
    // Names with letters the League font doesn't have are set by Windows instead, and fade in rather than decode
    bool foreign = std::any_of(name.begin(), name.end(), [&](char32_t ch) { return ch != U' ' && !stbtt_FindGlyphIndex(&name_f.info, (int)ch); });
    TextMask foreign_name = foreign ? system_text(name, (int)NAME, 900) : TextMask{};
    std::mt19937 rng(66);
    std::uniform_real_distribution<float> uni(0, 1);
    std::vector<float> lock;
    for (size_t i = 0; i < name.size(); ++i) lock.push_back(0.55f + 0.6f * uni(rng));
    const std::u32string glyphs = U"ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";
    const float X = 150, Y = 130, R = 74, TX = X + 2 * R + 40; // clear of the sides a 16:10 screen trims; every line starts at TX
    // Each line's top (fonts draw from their ascent line, so the letters' own tops are measured): the title's capitals
    // level with the top of the icon, then fixed gaps between one line's baseline and the next line's capitals
    const float title_y = Y + 2 - title_f.cap_top(U'P', title_size);
    const float name_y = foreign ? title_y + title_f.baseline(title_size) + 12
                                 : title_y + title_f.baseline(title_size) + 18 - name_f.cap_top(U'R', name_size);
    const float rule_y = foreign ? name_y + foreign_name.h + 6 : name_y + name_f.baseline(name_size) + 16;
    const float sub_y = rule_y + 16 - sub_f.cap_top(U'A', SUB);
    const float DRIFT = 4; // px per second

    status("Putting the intro together...");
    Encoder enc("-f rawvideo -pix_fmt rgb24 -s 1920x1080 -r 30 -i - -c:v libx264 -preset fast -crf 16 -pix_fmt yuv420p " + quote(out));
    // Rows are independent, so each frame's per-pixel work is split across the cores
    auto rows = [&](auto&& draw) {
        unsigned k = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> workers;
        for (unsigned w = 0; w < k; ++w) workers.emplace_back([&, w] { for (int y = (int)w; y < H; y += (int)k) draw(y); });
        for (auto& worker : workers) worker.join();
    };
    auto champion_frame = [&](int f) {
        char file[32];
        std::snprintf(file, sizeof file, "f_%04d.png", f + 1);
        return load_image(frames / file, 4);
    };
    std::future<Image> next_champion = std::async(std::launch::async, champion_frame, 0); // the next frame loads meanwhile
    std::vector<unsigned char> frame(size_t(W) * H * 3);
    float orbit_mid = (orbit.front() + orbit.back()) / 2;
    Image champion;
    for (int f = 0; f < TOTAL; ++f) {
        float t = f / (float)FPS;
        Canvas c(W, H, 1.f);

        // The background pans against the camera's orbit and creeps in, so it reads as the same space
        float zoom = 1.25f + 0.08f * ease(t, 0, DUR);
        float sw = PW / zoom, sh = sw * 9 / 16;
        float cx = PW / 2.f - (orbit[std::min(f, N - 1)] - orbit_mid) * (PW - PW / 1.25f) / 2 * 0.8f / 42;
        float cy = PH / 2.f - 30;
        rows([&](int y) {
            for (int x = 0; x < W; ++x) {
                float sx = cx + (x / (float)W - 0.5f) * sw, sy = cy + (y / (float)H - 0.5f) * sh;
                int x0 = std::clamp(int(sx), 0, PW - 2), y0 = std::clamp(int(sy), 0, PH - 2);
                float u = clamp01(sx - x0), v = clamp01(sy - y0);
                size_t i = (size_t(y) * W + x) * 3;
                for (int ch = 0; ch < 3; ++ch) {
                    auto P = [&](int px, int py) { return plate[(size_t(py) * PW + px) * 3 + ch]; };
                    c.c[i + ch] = (P(x0, y0) * (1 - u) + P(x0 + 1, y0) * u) * (1 - v) + (P(x0, y0 + 1) * (1 - u) + P(x0 + 1, y0 + 1) * u) * v;
                }
            }
        });

        // The champion, rendered with a transparent background; the last frame holds through the flash
        if (f < N) {
            champion = next_champion.get();
            if (f + 1 < N) next_champion = std::async(std::launch::async, champion_frame, f + 1);
        }
        // Rendered below full size to save time, so it's scaled up (bilinear) to fill the frame
        const float kx = champion.w / (float)W, ky = champion.h / (float)H;
        rows([&](int y) {
            if (champion.w < 2 || champion.h < 2) return;
            float sy = std::clamp((y + 0.5f) * ky - 0.5f, 0.f, champion.h - 1.001f);
            int y0 = int(sy);
            float v = sy - y0;
            for (int x = 0; x < W; ++x) {
                float sx = std::clamp((x + 0.5f) * kx - 0.5f, 0.f, champion.w - 1.001f);
                int x0 = int(sx);
                float u = sx - x0, rgb[3] = {0, 0, 0}, alpha = 0;
                // Weighted by alpha, so the clear pixels around the champion don't darken its edges
                for (auto [qx, qy, w] : {std::tuple{x0, y0, (1 - u) * (1 - v)}, {x0 + 1, y0, u * (1 - v)}, {x0, y0 + 1, (1 - u) * v},
                                         {x0 + 1, y0 + 1, u * v}}) {
                    const unsigned char* q = &champion.px[(size_t(qy) * champion.w + qx) * 4];
                    float a = w * q[3] / 255.f;
                    for (int ch = 0; ch < 3; ++ch) rgb[ch] += a * q[ch] / 255.f;
                    alpha += a;
                }
                if (alpha > 0) c.blend(x, y, {rgb[0] / alpha, rgb[1] / alpha, rgb[2] / alpha}, alpha);
            }
        });

        // The whole card slides in then drifts right slowly, icon and text together
        float a = ease(t, 0.1f, 0.35f);
        if (!icon.px.empty() && a > 0) icon_frame(c, icon, X + R + DRIFT * t, Y + R, R * (0.7f + 0.3f * a), a);
        float tx = TX - 50 * (1 - a) + DRIFT * t;
        title_f.text(c, title, title_size, tx - title_f.lead(title[0], title_size), title_y, CREAM, GOLD, a, TITLE_TRACK);

        if (t >= 0.55f && foreign) {
            float fade = ease(t, 0.55f, 0.45f), x0 = TX + DRIFT * t, y0 = name_y;
            for (int y = 0; y < foreign_name.h; ++y)
                for (int x = 0; x < foreign_name.w; ++x) {
                    float cover = foreign_name.a[size_t(y) * foreign_name.w + x] * fade;
                    if (cover <= 0) continue;
                    c.blend(int(x0 + x + 3), int(y0 + y + 4), {0, 0, 0}, cover * 0.45f); // the same soft drop shadow
                }
            for (int y = 0; y < foreign_name.h; ++y)
                for (int x = 0; x < foreign_name.w; ++x) {
                    float cover = foreign_name.a[size_t(y) * foreign_name.w + x] * fade;
                    if (cover > 0) c.blend(int(x0 + x), int(y0 + y), GOLD, cover);
                }
        } else if (t >= 0.55f) { // the name decodes: letters flicker through glyphs until each locks in gold
            float x = TX - name_f.lead(name[0], name_size) + DRIFT * t;
            for (size_t i = 0; i < name.size(); ++i) {
                if (t >= lock[i]) name_f.glyph(c, name[i], name_size, x, name_y, GOLD, GOLD, 1.f);
                else if (name[i] != U' ' && uni(rng) < 0.55f)
                    name_f.glyph(c, glyphs[rng() % glyphs.size()], name_size, x, name_y, GREY, GREY, 0.55f);
                x += name_f.advance(name[i], i + 1 < name.size() ? name[i + 1] : 0, name_size, NAME_TRACK);
            }
        }

        float rule = ease(t, 0.9f, 0.5f);
        float rx = TX + DRIFT * t, ry = rule_y;
        for (int x = 0; x < 560 * rule; ++x)
            for (int y = 0; y < 2; ++y) c.blend(int(rx + x), int(ry + y), GOLD_DARK, 1 - x / 560.f);
        diamond(c, rx + 6, ry + 1, 6, GOLD, rule); // left tip on the line's start

        size_t typed = size_t(sub.size() * ease(t, 0.9f, 0.35f));
        sub_f.text(c, sub.substr(0, typed), SUB, rx - sub_f.lead(sub[0], SUB), sub_y, CREAM, CREAM, 0.9f, 0.08f);

        // Blow out to white for the cut into the replay
        float flash = clamp01((t - (DUR - 0.12f)) / 0.35f);
        for (size_t i = 0; i < frame.size(); ++i)
            frame[i] = (unsigned char)std::lround(255 * clamp01(c.c[i] * (1 - flash) + flash));
        enc.write(frame);
    }
    return out;
}

void stitch_potg(const fs::path& intro, const ReplayClip& clip, const Music& music, const fs::path& out) {
    fs::path bed = music_bed(music, duration(intro));
    run("ffmpeg -v error -y -i " + quote(intro) + " -i " + quote(clip.video) + " -i " + quote(bed) +
        " -filter_complex \"[0:v]fps=60,format=yuv420p[i];[1:v]fade=in:st=0:d=0.45:color=white,format=yuv420p[r];"
        "[i][r]concat=n=2:v=1:a=0[v]\" -map \"[v]\" -map 2:a -c:v libx264 -preset slow -crf 17 -r 60 -c:a aac -b:a 256k -shortest " +
        quote(out));
}
