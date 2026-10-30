# OpenCross developer guide

OpenCross is a from-scratch native PC engine (C++20, SDL3 with Vulkan, Windows x64) that plays Chrono Cross from the Steam Radical Dreamers Edition (RDE) data files in place. One self-contained `opencross.exe` is dropped into the RDE folder and keeps its settings in `opencross.ini` next to it; an optional `XINPUT9_1_0.dll` beside it adds a button to RDE's launcher. PS1 art and RDE HD art can be switched live (F6), music is remastered or original, and every toggle lives in the Esc settings window. The project is GPL-3 and redistributes no Square Enix data or code. Libraries and references are credited in [`CREDITS.md`](../CREDITS.md).

This folder documents the engine, the file formats it reads, and how its behaviour is validated.

## 1. Engineering rules

- Original code only. Game rules are specified from the original game's behaviour and implemented fresh; nothing is copied from Square Enix code or from any decompilation.
- PS1 and RDE file formats are import formats. They are decoded in `src/import` and nowhere else. The engine itself has no PS1 concepts (no GTE, ordering tables, VRAM, Psy-Q).
- No per-room, per-scene or per-entity hacks. Fix the general behaviour.
- Classic (SD) mode uses only the PS1's own disc textures and fonts, nothing invented. HD mode is exactly RDE's art. Upgrades exist only in Modded mode and as opt-in enhancements; with them off, the picture is the PS1 picture.
- Never invent behaviour. Every change is backed by the original game, and every verdict is a mechanical table or frame comparison, not an impression (see [`validation.md`](validation.md)). Where the PS1 game and the RDE differ, the RDE is the reference.
- Every corrected fact goes into `docs/formats` or `docs/architecture.md` in the same change, and every function or variable identified while reverse engineering gets a symbol in `tools/ghidra/symbols/` (see its README).
- Source comments carry no provenance: no decompiler names, addresses, measurement notes, doc paths, history or trailing inline comments. Complicated files and functions get a header comment.
- Naming: no single-letter identifiers except `i j k` and `x y z w h u v r g b a`.
- Never touch a real save folder (`Documents\My Games\...\Steam\<id>`); test with forged saves under `tmp\` or `[Paths] SaveDir`.
- Test windows take no focus and play no sound (`OC_QUIET=1`; the route runner and `ocbattleview --frames` set it).
- Use an editor that preserves line endings; several files have mixed endings, and a bulk `sed -i` flips them (compare `git diff -w --stat` with `git diff --stat`). Keep `tmp\` clean.

## 2. Repository layout

- `src/` engine and game code; `src/main.cpp` game entry, `src/tool_main.cpp` dev-tool dispatcher.
- `tools/` dev tools linked into `opencross.exe` (`opencross <tool>`; `oc*.cpp`), capture and comparison helper scripts (`duckstation.ps1`, `ours_cast*.ps1`, `quiet_start.ps1`, `rde_*.py`), `ghidra_setup.ps1` with `tools/ghidra/` (Ghidra scripts and the `symbols/` catalogues), and `tools/py/` (Python importers, validators, PS1 reference drivers, comparison scripts).
- `tests/` unit tests (`test_*.cpp`, ctest) and `tests/routes/` scripted routes (`<name>.ini` + `<name>.txt`, `run.ps1`).
- `launcher/` the 32-bit proxy DLL for RDE's launcher, a CMake project of its own.
- `docs/` this guide, `architecture.md`, `validation.md`, `rde_internals.md`, `tools.md`, `formats/` (file formats), `rules/` (game behaviour). `third_party/` vendored libraries (`clhca`, `librempeg`); `scripts/` librempeg build helpers.
- `build.ps1`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json` the build; `build/` output; `tmp/` scratch (captures, forged saves).
- `README.md`, `CREDITS.md`, `LICENSE`.

`src/` modules:

- `platform` window, input, key maps, `opencross.ini`, paths, rumble, updater, Steam.
- `vfs` RDE ZIP archives (AES), cdrom2 / HD override layering, mods overlay.
- `import` PS1 / RDE decoders: containers, LZSS, TIM, chip2, rooms, models, battle stages, room effects, text, sprite fonts.
- `assets` engine-side asset types the importers produce.
- `data` static tables, battlefield-to-room map, area names. `core` logging, types, timers, parallel helpers, version header.
- `render` GPU layer: pipelines, CRT pass, lights, ImGui settings window (Mods / Update tabs), GLSL shaders (SPIR-V at build time).
- `audio` mixer, HCA (SAB) decoding, AKAO sequencer and SPU-style sampler, banks, SFX, voices, BGM table, reverb.
- `script` field script VM. `battle` battle rules, RNG, Dopplegang table, `.pac` battle-script VM.
- `game` field world (actors, camera, collision, dialogue, transitions, party), game flow, new game / New Game+, autosave, cheats, debug tools, route scripting; `game/battle` battle view (units, camera, HUD, results, transition, audio); `game/effects` effect engine (emitters, sequence player, lights, room meshes, bespoke overlays).
- `ui` menus, shops, title, pause screen, dialogue box, window frames, PS1 font, low-res targets, key mapping screen.
- `save` `save.dat` and memory cards, `ext_save.dat`, OpenCross save page (`.ocsav`). `movie` Bink FMV (librempeg); MP3 voices use Windows Media Foundation.
- `app` application loop: boot, simulation, draw, battle hand-off, route runner, debug.

## 3. Build, test, tools, routes

### Build

`.\build.ps1 debug [-Configure] [-Test] [-Target a,b]` / `.\build.ps1 release`. Requirements: Visual Studio C++ toolset, CMake, Ninja Multi-Config, vcpkg (`x64-windows-static`; `-VcpkgRoot` sets the vcpkg root) and the Vulkan SDK. CMake preset `win-x64` (build presets `debug` / `release`, test preset `debug`). Output: `build\win-x64\bin\{Debug,Release}\opencross.exe`. `.\build.ps1 -Launcher` builds the separate 32-bit launcher proxy (`build\launcher-x86\bin\XINPUT9_1_0.dll`, source in `launcher/`, see [`formats/launcher.md`](formats/launcher.md)).

- LNK1104 means the exe is running; stop it first.
- A header member change, or a re-configure, can leave stale objects: do a clean rebuild.

### Unit tests

`.\build.ps1 debug -Test` or `ctest --preset debug` (executables `build\win-x64\tests\Debug\test_*.exe`). Battle subset: `ctest --preset debug -R "battle|effects"`.

### Dev tools

`opencross tools` lists them: `occ`, `octext`, `ocaudio`, `ochd`, `ocprd`, `ocstage`, `ocbattle`, `ocmovie`, `ocbattleview`, `ocbattleseq`, `ocflow`, `ocmenudemo`, `ocshopdemo`, `ocmodelview`, `ocsteam`. Every subcommand and flag, the `OC_*` environment variables, the hotkeys and the helper scripts are catalogued in [`tools.md`](tools.md); the settings file in [`formats/config.md`](formats/config.md).

- `ocbattle` subcommands: `scenes`, `sim <scene>`, `cast`, `elements`, `xcheck`, `soak`, `pac-disasm <scene>`, `pac-soak`, `fx-reg <scene>`, `fx-audit`, `debug-kill [enemy_id [count]]` (the Debug tab's "Replace the enemies" headless).
- Gates: `opencross ocbattle fx-audit` (`--find NAME`; must not regress) and `opencross ocbattle pac-soak` (`--scene N`, `--from A --to B`, `OC_SOAK_LIST=1`; zero exceptions, outcome counts unchanged by changes that should not touch battle scripts).
- One battle in a window: `opencross ocbattleview <scene> [--weapon N] [--party ..] [--cast id --caster U --casttgt U --castat F] [--ecast N] [--auto] [--field <bg>] [--keys "Down:200,.."] [--ko] [--stats] [--mods] [--sliced] [--shotevery N --shotdir D --frames M]`. Units 0-2 are PCs, 3+ enemies; `--shotevery` gives deterministic 1/60 s ticks.
- Reference captures of the PS1 game (frame-stepped DuckStation driven over its GDB server): `tools/py/ps1ref.py` (library), `ps1_capture.py` (`prep`, `scene`, `cast`; back ends `ps1_scene.py`, `ps1_cast.py`), `ps1_audio.py`, `ps1_registry.py`, `ps1_state.py`; see [`validation.md`](validation.md#3-automated-reference-captures).
- `tools/py/` (run with `python -I`): the `cc_*` importers and validators (`cc_room`, `cc_model`, `cc_stage`, `cc_fieldscript`, `cc_text`, `cc_tables`, `cc_effects`, `cc_audio`, `cc_save`, ...) and `cc_save_forge` (forged saves). Comparison scripts such as `fx_diff.py`, `sfx_cmp2.py` and `rde_*.py` (RDE window capture, in `tools/` and `tools/py/`) need plain `python` (numpy / zstandard / OpenCV / pywin32).

### Routes

`$env:OC_BUILD_DIR="build\win-x64"; tests\routes\run.ps1 <name> [debug|release]` runs `tests/routes/<name>.ini` with `<name>.txt` (optional `<name>.env` of KEY=VALUE switches) and prints route / ERROR / WARN log lines (`OC_BUILD_DIR` defaults to `build\win-x64`). `run_save.ps1` and `run_save_fresh.ps1` chain the save round-trip routes. The route language is defined in `src/game/route_script.h` (verbs include `assert battle events`, `assert pause open|closed`, `assert frozen`, `request slotdump N`, `set option`, `put`, `request money`, `pad on|off`, `pad tap`, `wait/assert guide`, autoshot; keys f1..f9).

- Deterministic baselines for refactor checks: `alu_chair`, `art_swap`, `battle_hud` (build a baseline exe first and compare). Standalone `[Debug] Battle=` routes (`battle_run`, `battle_chain`, `battle_dash`, `battle_dash2`, `battle_defend`, `results_heal`, `smoke_battle`) set `[Debug] Seed`, so their RNG is seeded; the fights still run in real time, so only the outcome of a frame-paced run repeats exactly.
- The route language, the `.env` convention, input records and replays, and the room sweep are specified in [`formats/routes.md`](formats/routes.md).
- Others include `save`, `continue_save`, `ngplus_title`, `ngplus_continue`, `ngplus_ending`, `pause`, `pause_dialog`, `party_rotate`, `entry_fade` (room-entry fade-in after Continue / `Room=` / `load`), `grand_slam` (the FE 75 monster picker and its monster-party battle) and `true_ending` (Opassa Beach to the planet scene; needs a forged save in its scratch save folder, see the route's `.ini`).

## 4. Validating behaviour

Behaviour is validated against the original PlayStation game running in DuckStation and against the Radical Dreamers Edition itself; where they differ, the Radical Dreamers Edition is the reference. How to set up a PS1 reference and your own disassembly is described in [`validation.md`](validation.md).

## 5. Documentation map

- [`architecture.md`](architecture.md) engine architecture: app loop, VFS, field-script VM, battle, render, audio, save.
- [`validation.md`](validation.md) setting up a PS1 reference in DuckStation and a disassembly in Ghidra; how behaviour is compared.
- [`tools.md`](tools.md) dev tools and their subcommands, gates, `OC_*` environment variables, hotkeys, helper scripts.
- [`TODO.md`](TODO.md) open issues: known bugs, fidelity gaps, comparisons that need a capture, code hygiene.
- [`rde_internals.md`](rde_internals.md) how the RDE executable runs the PS1 game: interpreter, guest RAM, hooks and guards, overlays, file input, saves, the host-side feature layer; companion to the catalogues in `tools/ghidra/symbols/`.

Formats (`docs/formats/`):

- `audio.md` SEAD / HCA BGM, AKAO sequences, SPU instrument banks, SFX and sound-RAM behaviour.
- `battle_data.md` battle and menu data tables in `cdrom.dat` (enemies, elements, groups, drops, growth, characters, key items, shops).
- `battle_effects.md` element / tech / summon effect pipeline: packs, overlays, emitters, sequences, camera tracks.
- `battle_stage.md` battlefield (stage) mesh and texture format.
- `battlescript.md` the `.pac` battle-script VM: opcodes, scheduler, variables.
- `config.md` `opencross.ini`: every section and key, defaults, clamps, legacy keys, live versus restart.
- `fieldscript.md` room event bytecode: opcodes and semantics.
- `launcher.md` RDE's launcher (`ui.csv`, its art) and the proxy DLL that adds the OpenCross button.
- `launcher.md` RDE's launcher (`ui.csv`, its art) and the proxy DLL that adds the OpenCross button.
- `model.md` model, skeleton, texture and animation formats.
- `mods.md` loose-file mods, the Modded art mode, texture tile dump and replacement.
- `movies.md` which FMV a script plays, file names, play ranges.
- `rde_archives.md` RDE ZIP archives and the cdrom2 override rule.
- `room.md` room file format (`.bin`, `.ctd`, `.prd`): layers, walk mesh, triggers, camera.
- `room_effects.md` room `.prd` effects: emitters, sprite sheets, sequences.
- `routes.md` route scripts (`tests/routes/`), input records and replays, the room sweep.
- `save.md` `save.dat`, memory cards, the save block, `ext_save.dat`, the OpenCross save page.
- `text.md` message blocks, JP glyphs, `lang.dat` tables, sprite fonts.
- `title_scene.md` the underwater 3D title backdrop.

Behaviour rules (`docs/rules/`), the game rules the engine implements, every number from the source:

- `achievements.md` when each Steam achievement is earned.
- `field_actors.md` the field tick, player movement, camera follow, followers, collision and triggers, animation, shadows, field status effects, footsteps, message windows, spawns.
- `battle.md` battle start, the turn system, targeting, grid and gauge, the field ring, status application, battle end, rewards and growth, Pip and Dopplegang, cheats.
- `menus.md` main menu, settings and the options word, status and equip formulas, the element grid editor, key items, shops, name entry, save and load screens, party editors, pause, title flow.

## 6. Open issues

Known bugs, fidelity gaps, open comparisons and code hygiene are tracked in [`TODO.md`](TODO.md). Remove an entry in the change that fixes it.
