# POTG for League

Overwatch's Play of the Game, but for League. It adds a Play of the Game tab to the client: pick a game, it finds your best play and replays it with an intro of your champion. You can also save it as a video.

- `pengu/potg/index.js`: the tab (a Pengu Loader plugin)
- `main.cpp`, `potg_engine.hpp`, `potg_video.cpp`: the helper that scores the game and runs the replay
- `intro/auto_intro.py`: renders the champion intro in Blender

Plays are scored on kills, assists, multikills, shutdowns, objective steals, and fighting outnumbered or low on health.

## Setup

You need Windows, League in borderless or windowed mode, [Pengu Loader](https://pengu.lol), [Blender 5.2](https://www.blender.org), ffmpeg on your PATH, and CMake with MSVC.

1. Turn on the replay API: add `EnableReplayApi=1` under `[General]` in `C:\Riot Games\League of Legends\Config\game.cfg`.
2. Add Riot's fonts and songs (not included):
   - `intro/fonts/`: `BeaufortForLOL-Bold.otf`, `BeaufortForLOL-Heavy.otf`, `Spiegel-SemiBold.otf`
   - `intro/music/`: `legends_never_die.wav`, `coming_alive.wav`, `awaken.wav`
3. Build:
   ```
   cmake -S . -B build
   cmake --build build --config Release
   ```
4. Run `build\Release\main.exe`, copy `pengu\potg` into Pengu Loader's `plugins` folder, and restart the client.

A Riot API key is optional. Paste one in the tab to count damage share and to load games older than your last 20.

Not endorsed by Riot Games.
