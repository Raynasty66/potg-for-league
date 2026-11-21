# POTG for League

An Overwatch-style **Play of the Game** for League of Legends. It adds a Play of the Game tab to the League client. Pick one of your recent games and it scores every kill, assist and objective, finds the best 15 seconds, and plays it back as a real replay. The replay opens with a 3D intro of your champion and skin, a title card and music. You can also save the play as an mp4.

## How it works

- **The tab** (`pengu/potg/index.js`) is a [Pengu Loader](https://pengu.lol) plugin. It lists your games in the client's match-history style and talks to the helper.
- **The helper** (`main.exe`) runs in the background on `http://127.0.0.1:47811`. It does the rest:
  - reads each game's timeline from the client (and from Riot's match-v5 API when a key is saved);
  - scores the plays (`potg_engine.hpp`);
  - drives the League replay API to film the play (`potg_video.cpp`).
- **The intro** (`intro/auto_intro.py`) is rendered once per skin by Blender. It uses the champion's model from modelviewer.lol, framed close on the champion. The title card and music are then composited on top.

### Scoring

| Event | Points |
| --- | --- |
| Kill | 50, times your share of the victim's damage |
| Assist | 25, times your share of the victim's damage |
| Double / triple / quadra / penta kill | +100 / +250 / +700 / +1500 |
| Shutdown | up to +30 |
| Kill started with less health | (kill + multikill) × health bars behind |
| Dragon or Baron taken / stolen | 25 / 150 or 350 |
| Elder taken / stolen | 50 / 500 |
| Outnumbered fight | +50% per extra enemy, up to +100% |
| Team far behind in gold | small boost |

Every play shows its breakdown as plain "+N for ..." lines that add up to the total.

## Requirements

- Windows and League of Legends (the client at `C:\Riot Games\League of Legends`)
- [Pengu Loader](https://pengu.lol)
- Visual Studio Build Tools (MSVC) and CMake 3.16+
- [Blender 5.2](https://www.blender.org) at `C:\Program Files\Blender Foundation\Blender 5.2\blender.exe`
- `ffmpeg` and `ffprobe` on your PATH
- The replay API turned on. Add `EnableReplayApi=1` under `[General]` in `C:\Riot Games\League of Legends\Config\game.cfg`.
- Borderless or windowed League, since the camera only moves while League is on screen

### Files you supply

Riot's fonts and songs aren't in this repo. Put them here:

| File | Used for |
| --- | --- |
| `intro/fonts/BeaufortForLOL-Bold.otf`, `BeaufortForLOL-Heavy.otf`, `Spiegel-SemiBold.otf` | the title card |
| `intro/music/legends_never_die.wav`, `intro/music/coming_alive.wav` | the music |

The champion models, the map background and the rendered intros are downloaded or made on first use. They're cached in `intro/models` and `intro/cache`.

## Build and install

```
cmake -S . -B build
cmake --build build --config Release
```

1. Run `build\Release\main.exe`. It has no window and keeps running in the background.
2. Copy (or junction) `pengu\potg` into Pengu Loader's `plugins` folder and restart the client.
3. Open the **Play of the Game** tab, pick a game, and press **Find play of the game**.

### Riot API key (optional)

Without a key, every kill counts as if you did all the damage. Saving a key from the tab adds:

- damage shares and the health-behind bonus;
- **Show older games**, beyond the client's latest 20.

The key is written to `settings.json`, which never leaves your machine. Development keys expire after 24 hours, but each game's timeline is kept once it has been fetched.

---

Not endorsed by Riot Games. League of Legends and its fonts, music and models belong to Riot Games.
