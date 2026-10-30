# DMC1 Practice Mod (v1.4.1)

Created by **waifu**.

A practice tool for individual-level (IL) runs of Devil May Cry 1 (HD Collection, PC):
- a mission timer and a room timer
- personal bests, kept per preset
- live rank criteria
- instant mission (F3) and room (F4) restarts
- presets that drop you into any mission with any loadout, including Nightmare's core states
- off by default (F10 toggles it); while on, the game saves to a practice file and never to
  your real save, which is also backed up every start
- export a preset to slot 10 of the real save, to play it without the mod
- capture-safe: drawn before OBS sees the frame, with a "PRACTICE MOD" tag while on

Player instructions are in `README.txt` (the one that ships in the release zip).

## Source layout
- `src/`: the mod, a `dinput8.dll` proxy that draws with ImGui from the D3D11 swap chain's Present
  (hooked in the swap chain's function table, so capture software sees the overlay).
  - `game_addrs.*`: finds every game address by pattern scanning (no hardcoded addresses), so the
    mod works on both the 2018 build and the newer one and survives game updates that keep the
    same code.
  - `reset.*`: mission and room restarts.
  - `builder.*` and `presets.*`: building save slots from presets.
  - `menu.*`: the F2 menu and preset editor.
  - `overlay.*`: the timers and the panels.
  - `mode.*`, `steam_redirect.*`, `backup.*`: the F10 on / off switch, keeping the practice save
    apart from the real one (Steam remote storage writes), and the save backups.
- `docs/SAVE_FORMAT.md`: notes on the save slot layout (story flags, equipment, devil arm flags,
  Nightmare cores and more).
- `tools/`: Python prototypes of the save builder and the story table generator.
- `presets/`, `dmc1_practice.ini`, `dmc1_criteria.csv`, `fonts/`: data that ships next to the DLL.
- `README.txt` (player guide, ships with the release) and `THIRD-PARTY-NOTICES.txt`.

## Building
CMake with a MinGW-w64 (llvm-mingw) or MSVC toolchain targeting x64 Windows; ImGui, MinHook and
nlohmann/json are fetched by CMake.

```
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<your mingw toolchain file>
cmake --build build
```

## Credits
- **serpentiem**: DDMK, which the room and event research builds on
- **Chris** and **Aisu**: Cheat Engine tables
- **Mysterion**: the DMC1 autosplitter
- **dmc.wiki**: rank criteria
- Libraries: [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause),
  [JSON for Modern C++](https://github.com/nlohmann/json) (MIT); see `THIRD-PARTY-NOTICES.txt`
- Font: EB Garamond by Georg Duffner and Octavio Pardo (SIL Open Font License 1.1)

## License
MIT (see `LICENSE`). Use it, change it, build your own things on it. If you do, a credit to
**waifu** is appreciated.

## About how this was made
This mod was made with the help of AI. The code, the reverse engineering of the game's memory
and save format, and the documentation were written with Claude (Anthropic's AI assistant),
directed and tested in-game by waifu.
