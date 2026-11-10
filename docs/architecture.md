# OpenCross engine architecture

This document is a map of the engine for a developer who is new to the code base. It says what each module owns, how modules call each other, and how a frame, a room load and a battle move through the code. It names real files, classes and functions. Anything that could not be confirmed from the source is marked "(unverified)".

OpenCross is a from-scratch C++20 engine. It plays Chrono Cross from the data files of the Radical Dreamers Edition (RDE) install. One executable, `opencross.exe`, holds the game and the developer tools. The data formats are described in `docs/formats/*.md`; this document links to them and does not repeat them.

## 1. Modules and dependencies

Each module is a static library `oc_<name>`. The comment in `src/CMakeLists.txt` says dependencies point downward. The link edges read from the `CMakeLists.txt` files (public edges only; the platform, miniz and SDL3 links are left out):

```
 opencross (exe) ---- oc_tools
      |
 oc_flow    oc_menu_model    oc_movie              application tier
      |
 oc_keymap -> oc_title
      |
 oc_battle_view -> oc_battle_fx -> oc_battle_anim  battle presentation
      |
 oc_game  (field world, actors, autosave, routes)  integration
      |
 oc_ui   oc_script   oc_effects_gpu -> oc_effects  screens, field VM, effect engine
      |
 oc_render   oc_audio   oc_battle   oc_data        engines and rules
      |
 oc_text   oc_stage   oc_import   oc_save          importers, save
      |
 oc_assets   oc_platform   oc_net   oc_vfs
      |
 oc_core
```

Each tier links the ones below it, not strictly the next one down (`oc_ui` links `oc_save` and `oc_data`, `oc_menu_model` links `oc_game`). The edges as a list (each library also reaches `oc_core` through the ones below it):

| Library | Links |
|---|---|
| `oc_core` | nothing |
| `oc_platform`, `oc_vfs`, `oc_save`, `oc_script` | `oc_core` only (`oc_platform` also SDL3; `oc_vfs` and `oc_save` miniz) |
| `oc_assets` (INTERFACE) | `oc_core` |
| `oc_net` | `oc_core` (also WinHTTP and bcrypt) |
| `oc_import` | `oc_core`, `oc_assets` |
| `oc_text`, `oc_stage` (both in `src/import/`) | `oc_import` (`oc_text` also `oc_vfs`) |
| `oc_data` | `oc_vfs`, `oc_import`, `oc_text` |
| `oc_battle` | `oc_data` |
| `oc_versus_link` (`src/game/battle/versus_link.*`, `versus_net_link.*`) | `oc_battle`, `oc_data`, `oc_net` |
| `oc_render` | `oc_platform`, `oc_net`, `oc_vfs`, `oc_text` |
| `oc_audio` | `oc_vfs` (private) |
| `oc_movie` | `oc_vfs`, `oc_render`, `oc_audio`, librempeg |
| `oc_ui` | `oc_platform`, `oc_vfs`, `oc_import`, `oc_text`, `oc_render`, `oc_save`, `oc_data` |
| `oc_title` (`src/ui/title/`) | `oc_ui`, `oc_audio`, `oc_save` |
| `oc_keymap` (`src/ui/keymap_screen.*`) | `oc_title`, `oc_ui`, `oc_platform`, `oc_import` |
| `oc_effects` | `oc_platform`, `oc_assets`, `oc_import`, `oc_audio`, `oc_stage` |
| `oc_effects_gpu` | `oc_effects`, `oc_render` |
| `oc_game` | `oc_platform`, `oc_vfs`, `oc_import`, `oc_assets`, `oc_render`, `oc_script`, `oc_ui`, `oc_text`, `oc_audio`, `oc_effects`, `oc_effects_gpu`, `oc_battle` |
| `oc_battle_anim` | `oc_import`, `oc_assets` |
| `oc_battle_fx` | `oc_battle_anim`, `oc_stage`, `oc_effects`, `oc_battle`, `oc_data`, `oc_vfs`, `oc_import`, `oc_assets` |
| `oc_battle_view` | `oc_battle_fx`, `oc_effects_gpu`, `oc_battle_anim`, `oc_stage`, `oc_text`, `oc_ui`, `oc_game`, `oc_battle`, `oc_versus_link`, `oc_data`, `oc_render`, `oc_audio`, plus the lower modules |
| `oc_menu_model` | `oc_ui`, `oc_game`, `oc_battle_view`, `oc_render`, `oc_import` |
| `oc_flow` | `oc_game`, `oc_battle_view`, `oc_title`, `oc_keymap`, `oc_ui`, `oc_data`, `oc_save`, `oc_audio`, `oc_battle`, `oc_battle_anim`, `oc_stage`, `oc_effects`, `oc_effects_gpu`, `oc_render`, `oc_import`, `oc_vfs`, `oc_assets` |
| `oc_tools` (`tools/`, private) | nearly every library above |
| `opencross` | `oc_tools`, `oc_flow`, `oc_menu_model`, `oc_movie`, `oc_battle_view`, `oc_game`, `oc_ui`, `oc_render`, `oc_audio`, `oc_data`, `oc_save`, `oc_import`, `oc_vfs`, `oc_platform`, `oc_net`, `oc_versus_link`, `oc_core`, SDL3 |

Rules of thumb that the link lines enforce:

- `oc_script` links only `oc_core`. The field VM reaches the world through the `script::FieldHost` interface (section 4.3).
- `oc_battle` links `oc_core` and `oc_data`. The battle rules have no rendering, audio or file access and run headless in tests and in `ocbattle`.
- `oc_import` links `oc_core` and `oc_assets`. Importers take byte spans and return asset structs; they never open files.
- `oc_game` (`src/game/*.cpp`: the field) is the integration library for field play. The battle presentation is three sub-libraries under `src/game/battle/` (`oc_battle_anim`, `oc_battle_fx`, `oc_battle_view`) that sit above `oc_game`; the effect engine is in `src/game/effects/` (`oc_effects`, `oc_effects_gpu`).
- `oc_title` holds the title screen, name entry and slot list. `oc_flow` (`src/game/game_flow.*`, `game_session.*`, `party_editor.*`, `title_backdrop.*`, `debug_tools.*`) binds them to a state machine.

## 2. Process lifetime

### 2.1 Entry point

`src/main.cpp` is 14 lines. `run_tool(argc, argv)` (in `src/tool_main.cpp`) checks `argv[1]`; if it names a tool, the tool runs and its exit code is returned (a negative code becomes 1). If the arguments are not `--config <file>` / `--profile <dir>` pairs it prints the usage and returns 2. Otherwise `oc::app::App app; app.init(argc, argv); return app.run();`. `--config <file>` selects another ini (routes use it; such a run never connects, applies no mods and never rewrites the ini). `--profile <dir>` gives a normal run its own `opencross.ini` in that folder (created when missing), with its log, shots and a relative `[Paths] SaveDir` there: a second copy of the game on one machine gets its own multiplayer `PlayerId`.

### 2.2 The App class

`src/app/app.h` declares `oc::app::App`, one translation unit per concern:

| File | Concern |
|---|---|
| `app_boot.cpp` | `init()` and the `init_*` steps |
| `app_run.cpp` | `run()`: main loop, hotkeys, per-frame settings, mode dispatch (`simulate`) |
| `app_sim.cpp` | `advance()`: one block of simulation in the current mode; art and audio switches; movies |
| `app_battle.cpp` | field to battle hand-off and back |
| `app_draw.cpp` | `draw_frame()` and `draw_scene()`: frame composition |
| `app_feedback.cpp` | encounter feed-back, FE 14 frame echo and damage-jolt passes over the scene target |
| `app_multiplayer.cpp` | multiplayer presence and chat: the Multiplayer tab, the connection, the local player's state, remote parties, the chat panel and name labels (section 10) |
| `app_route.cpp` | scripted routes, input replay, room sweep |
| `app_debug.cpp` | the Debug tab of the settings overlay |

Members are declared in creation order and destroyed in reverse: every GPU resource goes before `ctx`, the voice player before the mixer.

### 2.3 Boot

`App::init()` runs, in order: `init_config` (exe folder, `opencross.ini`, `--profile` or `--config`, `platform::Config::load`, log, `bind_cheats`, game and save directories; a first launch writes defaults), `init_window`, `init_vfs` (`Vfs::open`, `ModManager::scan` and `apply`, `texture_registry::bind`, the texture-dump hooks for the Mods tab; then the menu's `kb_70` font face, about 190 MB, starts reading on a worker so that `init_menus` finds it parsed), `init_render` (`GpuContext::init` then `set_msaa` / `set_anisotropy` from `[Video] Msaa` / `Anisotropy`, `CrtPass`, `SpriteBatch`, `MeshPipeline`, `ImGuiLayer` and its Mods, Cheats and Update tabs, the 320x224 `field_rt`), `init_audio` (`VoicePlayer`, `SfxBank`, `Mixer::open_device`), `init_routes`, `init_flow` (`GameTables::load`, `GameFlow::init`), `init_saves`, `init_menus`, `init_field` (`FieldWorld::init`), `init_battle` (`BattleView::init`, `BattleResults::init`), `init_title`, `init_start_state`, `init_route_state`, `init_debug_tab`, `init_multiplayer` (the Multiplayer tab). It returns a non-zero exit code if the window (1), the renderer (1) or the route file (2) fails.

A missing piece sets a `*_ok` flag false and the App skips that mode. Without game data the world and battle are never created.

After a fresh boot to the title, `GameFlow` shows a boot logo (`ui::BootLogo`) before anything of the game runs: `ui/logo.png`, or in Japanese `ui/notice.png`, `ui/notice2.png`, `ui/logo.png`, in both art modes. Each image lasts 180 ticks over black: alpha ramps in over the first 30, holds, and ramps out over the last 30. Any key, click or pad button ends the current image at once (no fade-out); the logo is skipped entirely for routes, replays, `[Debug] Screenshot` runs and a return to the title, and does not run while the settings overlay (other than its Debug tab) is open. A missing image drops the whole logo.

### 2.4 Time: 60 Hz logic, interpolated drawing

`App::run()` loops while `window.pump(input)` is true. The simulation runs at a fixed 60 Hz; drawing runs once per presented frame.

- `platform::FrameTimer::tick()` gives the real seconds since the last frame.
- `[Game] FastMode` / `SlowMode` are RDE's speed modes and change only the field loop and the battle loop (below, Fast and slow mode).
- `platform::FixedStep::advance(dt)` (`window.cpp`) accumulates time and returns `{ticks, alpha}`. One tick is 1/60 s. `dt` is clamped to 0.25 s and at most `kMaxTicks = 5` ticks run per frame, so a stall drops time instead of spiralling. `alpha` (0..1) is the remainder. Drawers use it to interpolate between the last two simulated states.
- `platform::FrameLimiter::wait(config.frame_cap)` sleeps at the end of the frame. Routes, replays and sweeps skip it. VSync is a swapchain setting (`ctx.set_vsync`).
- In borderless and exclusive fullscreen the mouse pointer hides after `platform::kPointerIdleMs` (3 s) without mouse motion, button or wheel input, and returns with the next such input. It stays visible while the settings window is open and in windowed mode (`platform::pointer_hidden`, `ImGuiLayer::set_pointer_hidden`).

The field takes the tick count: `FieldWorld::update(in, ticks)` calls `sim_tick()` once per tick. `BattleView::update(dt, in)` ignores the tick count and runs its own `stepper_`. Both interpolate at draw time.

What the field blends by `alpha`: the scroll (`RoomView::scroll_at`), actor position and yaw (`Actor::pose`, with the segment smoothing of `docs/rules/field_actors.md` 6.9), clip time, the script-set model axes (pitch, model yaw, roll of ops 27 / 28 / 29 / AA and the per-axis scale of op 82, `Actor::axes_at`: a step over 512 units or a scale jump over x1.5 or through 0 is a reset and drawn at once, as is the first tick of a new actor), effect objects (position, angles, scale, colour, UV scroll and morph phase; `emitter_collect.cpp`; the room effects step once per 2-tick field frame like the PS1, so they blend over the whole frame step by `(ticks since the step + alpha) / 2`, `docs/formats/room_effects.md` 3.2.1) and the 8F screen ripple (`FieldWorld::wave_view(alpha)`: phases and ramped values; the grid lines move by sub-pixel amounts where the PS1 steps whole pixels). Drawn at the current tick's value: colour ramps (ENTITY_RGB tint, background group tint C2, ambient ramp, sequence dim tasks), the model y offset of ops B0 / B1, and the matrix and colour of effect mesh objects (types 14 / 19 / 20; only their position, UV scroll and morph are blended). While a frozen-capture exit fade runs, `alpha` is 1.

#### Fast and slow mode

RDE's fast and slow modes (F2 / F1, pad RT / LT, the Cheats tab; `App::update_loop_speed`, `game::speed_factor`) do not speed up the whole program. As in RDE they change only how many logic steps the field loop and the battle loop run per frame:

- The field runs on its own `FixedStep` (`App::field_stepper`) fed with the frame time times the field factor: 4 with fast mode (`FastRate` 0; 2..5 = that many), `SlowRate` percent with slow mode, with one 1/30 s stretch at full speed after every 299 slow field frames (`game::SlowFieldCycle`). The factor is 1 whenever `FieldWorld::speed_allowed()` is false: on the title and ending maps 8 and 9, while variable 0x36 is non-zero, during a room load or room change, while a movie runs and during the input locks: the pad lock that follows a room load or a field movie (24 field frames) and the one after a menu screen (4 field frames) or the key-item list (8 field frames; `docs/rules/field_actors.md`). The field is drawn at its own stepper's `alpha`.
- A battle advances by the frame time times the battle factor: 2 with fast mode, `SlowRate` percent with slow mode, but only while `BattleView::speed_allowed()` holds. That gate opens at the end of the intro (when the `bti0` layer stops) and closes when the outro brings `bti0` back; a pack's op 0x60 opens it and op 0x5F closes it, and while an overlay that holds the speed runs (the Time Devourer's end `lasdes2`, `miyademo`; `BespokeEffect::holds_game_speed`) the gate counts as shut. The battle load, intro and outro therefore run at 1x in either mode; the toggles still flip there and their icons show. The results screen runs at normal speed.
- Everything else keeps the real step: the main menu, shops, name entry, the party screens, the key-item list, the pause screen, the title, the results screen and movies. Play time counts real ticks in every mode.
- Message windows in the field keep the real clock (`FieldWorld::set_text_clock`) at RDE's text pace (`game::text_pace`): fast mode reveals 2.25 times as fast and divides every `{wait(n)}` pause and `{wend(n)}` countdown by 8; slow mode reveals at half speed and doubles them. `FastRate` and `SlowRate` do not change the text pace.
- The SFX sequencer ticks as fast as the loop that runs (`Mixer::set_sfx_tempo`): 4x in a fast field, 2x in a fast battle (after the intro), half speed in slow mode, the normal rate on every other screen and while the field's factor is 1. Music keeps its tempo.
- Both modes do nothing in room 386 (zel_a12i) and in the battles launched from it (`App::speed_locked`): RDE tests the room id, the field's own while the field runs and the room register (the room the battle was launched from, which a battle never changes) during a battle. A battle id plays no part, so the Radius tutorial (battle 386, launched from alu_a04f) runs at the chosen speed; a standalone battle (`[Debug] Battle`, `ocbattleview`) has no launching room and is never locked.
- The five play-time toggles (fast, slow, encounters off, battle boost, auto battle) are session-only: every launch starts with them off (`Config::clear_session_toggles`) and the ini never stores them. A `--config` ini may set them for tests; a route then runs the field and battle at the ini's speed per route tick. A replay runs 1:1 (it holds the field ticks of the recorded session).

### 2.5 Sim and draw

`App::simulate()` picks one mode per frame: `route_frame()`, `replay_frame()`, the boot logo, a battle, `sweep_frame()` or `field_frame()`. All reach `App::advance(in, dt, ticks)` (`app_sim.cpp`), a priority chain where the first branch that applies consumes the frame: play-time counter, load fades (`flow.tick_fade`), the open pause screen (`pause_modal`), a standalone battle (`battle_ok`), a playing movie, the title scene, flow screens, a field battle (`battle_in_field`), the main menu, party screen, the open key-item list, then the key-item and pause openers, a shop, script-requested menus and movies, the menu key, and last the normal field (`world.update(in, ticks)`, autosave, battle request checks). `App::draw_frame()` never changes game state; it reads it (section 6.3).

### 2.6 Hooks for routes and debugging

`[Debug]` ini keys (`platform::Config::debug_*`) select a room, flow state, seed, route script, input record or replay, a standalone battle and the sweep. A disabled flow (`[Debug] Battle`, the sweep, a replay) draws nothing of its own, except the screens a field script opens (FE 4A name entry, the party screen, the New Game+ list), which run in it too. `route_frame()` runs `ScriptSpeed` route steps per frame, each followed by `advance(in, 1/60, 1)`, so a route is tick-exact and independent of real time. About 110 `OC_*` environment variables are read with `getenv` (none are ini keys); they are developer switches and the route runner sets them per run from a route's `.env` file. The ones that shape a run: `OC_QUIET` (test window takes no focus and opens behind every other window, mixer gain 0), `OC_AUDIO_DUMP` (write the mix of a scripted run to a wav file), `OC_FRAMETIME` (log slow frames during a field to battle hand-over), `OC_NO_WARP` (draw the screen ripple unwarped), `OC_EXIT_KIND` (force the room-change fade kind), `OC_STUCK_TICKS` (softlock limit of the field watchdog), `OC_SWEEP_ROOMS` / `OC_SWEEP_FROM` / `OC_SWEEP_COUNT` / `OC_SWEEP_SCENARIO` (narrow the room sweep), `OC_SLOT_DUMP` (prefix for the route `request slotdump` output), and for battles `OC_BATTLE_SEED`, `OC_BATTLE_FIELD`, `OC_BATTLE_INTRO`, `OC_BATTLE_MISS`, `OC_BATTLE_NORUN`, `OC_BATTLE_AUDIT`, `OC_ROUTE_ELEM`, `OC_ROUTE_GAUGE`, `OC_RESULTS_HURT`, `OC_RESULTS_HEALER`, `OC_CAM_KEYS`, `OC_CAM_PICK`. The rest are trace and override switches grouped by prefix: `OC_*_TRACE` / `*_LOG` / `*_DBG`, `OC_FX_*`, `OC_FXDBG*`, `OC_STAGE_*`, `OC_SFX_*`, `OC_SEQ_*`, `OC_DEV_*`, `OC_FIND_*`. The tools add their own (`OCC_*`, `OCPRD_VERBOSE`, `OCSTAGE_*`, `FXA_*`, and `OPENCROSS_GAMEDIR` for `ocbattleseq`). F8 opens the Debug tab (`game::DebugTools`; using a tool locks saving for the session). F6 cycles the art mode (Original, Hd, Modded), F7 toggles widescreen, F9 toggles the battle camera debug overlay (`BattleView::handle_input`; F3 as well in `ocbattleview`); PageUp / PageDown cycle through the room list in `field_frame`, only when the game flow is disabled (a `[Debug] Room` start) and no movie is open; `ui/keymap_screen.*` is the New Game user-guide screen (two baked images with the key and pad glyphs drawn on them), not a rebinding UI: keys are fixed by `platform/keymap.h`; F1 / F2 (slow / fast), F3 / F4 / F5 (No Encounters, Battle Boost, Auto Battle) toggle RDE's play-time modes, which show as feature icons (`App::rde_toggle_keys`).

## 3. Data access

### 3.1 The VFS

`src/vfs/vfs.h`: `oc::vfs::Vfs` knows archives and paths, not what the bytes mean.

- `open(game_dir)` mounts `<GameDir>\data\*.dat` lazily. Each is a ZIP read through miniz (`class Archive` in `vfs.cpp`). Archives used: `cdrom`, `cdrom2`, `hd`, `lang`, `font`, `ui`, `movie`. See `docs/formats/rde_archives.md`.
- A path is `<archive>/<member>`, for example `cdrom/map/mapbin/alu_a01f.bin`. `normalise_path` lower-cases and maps `\` to `/`; `split_archive_path` splits off the archive.
- API: `read`, `read_disc`, `exists`, `list`, `loose_file`. Reads are thread-safe; the loaders use worker threads.
- `read` order: mod overlay file, then `cdrom2` for `cdrom/...` paths when `cdrom2_first(member)` is true, then the archive.
- `read_disc` skips the `cdrom2` override, for original-art screens that need the files RDE blanks there.

### 3.2 Art selection: SD, HD, Modded

`platform::ArtMode` is `Original`, `Hd` or `Modded`. `App::apply_art_mode()` applies it:

1. `mods.apply(files, render::mod_art(mode))` rebuilds the overlay and bumps `Vfs::generation()` if it changed.
2. `files.set_hd_mode(platform::art_is_hd(mode))`. In HD mode `cdrom2_first` is true for every member. In Original mode only a fixed list goes to `cdrom2`: `map/etc/openfont.tim`, `menu/title/cmptitle.tim`, `map/mapbin/ending.ctd`, and anything under `map/window/`.
3. Each consumer rebuilds its art in place, without touching the simulation: `FieldWorld::set_art_mode(hd, force)` runs `swap_art()` (`RoomView::reload_art`; every model an entity shows rebuilt on its own worker thread at once, then handed over through `Actor::swap_model_art`; effect atlases), then `BattleView`, `BattleResults`, `PartyScreen`, `GameFlow`, `MainMenu` and `ShopScreen` follow.

`Overlay::enhance`, read through `Vfs::enhanced()`, is true only for `Modded` (`ModManager::apply`). In the source its one reader is `render/model_tiles.cpp`; other improvements are separate `[Enhancements]` config keys (section 6.4). The code comments state the rule: HD art is exactly what RDE draws, and upgrades exist only in Modded.

Path-keyed texture caches use `Vfs::cache_key(path)` (path plus overlay generation), so a mod switch loads the new file.

### 3.3 Mods

`src/vfs/mods.h`, `docs/formats/mods.md`. `ModManager::scan` walks `<GameDir>\mods\<name>\**`, plus the bare `.dat` files there and the members of `.zip` / `.7z` / `.rar` files, read live (`mod_archive.*`), and resolves each loose file to a shipped member by matching trailing path components, or adds it as a new member when it matches none and sits below a folder named after an archive. `apply` builds an `Overlay`: `files` (member replacements: loose files, or `OverlayFile` members read in place from a packed mod or a mod's `<archive>.dat`) and `textures` (replacement PNGs, see `render/texture_registry.*`). Each mod has a `ModLoadOn` (Modded, Sd, Hd, All) and a priority from list order. Game data (`vfs::is_game_data`) ignores `ModLoadOn` and is fixed by the first `apply` until the next launch. Voice packs are read by `audio::VoicePlayer` from the enabled mod roots, not through the overlay (`ModManager::packed_voice` finds a packed mod's lines). A `mod.ini` at a mod's root is its manifest (`Mod::manifest`, `core/features.h`); `feature_inputs` hands the enabled ones to `features::resolve`, whose result `App::features` holds for the run.

### 3.4 Importers and assets

`src/import/` turns bytes into `src/assets/` structs. Callers read the bytes through the VFS and pass them in.

- `container.*`, `lzss.*`, `tim.*`: cpt / drp lumps, LZSS, TIM images and a 1024x512 VRAM model (`rde_archives.md`, `room.md`).
- `room.*`, `chip2.*`: `assets::RoomScene` (camera, layers, walk mesh, models, script bytes) and RDE HD backgrounds (`room.md`, `fieldscript.md`).
- `room_effects.*`: `assets::RoomEffects` (sprite sheets, emitters, sequences) for rooms and battle packs (`room_effects.md`, `battle_effects.md`).
- `model.*`: `assets::ModelAsset` for field and battle models (`model.md`). `battle_stage.*`: `assets::StageAsset` (`battle_stage.md`).
- `text.*`, `spritefont.*`: `lang.dat` tables and message blocks as rich text; `font.dat` and `ui.dat` fonts (`text.md`).

`oc_assets` is an INTERFACE library of plain float structs with no PS1 types. Renderers and the world consume them.

### 3.5 Game tables

`src/data/tables.*`: `data::GameTables::load(vfs)` fills one struct with the battle and menu tables: `enemies` (`StatRec`), `char_sets`, `elements` (`ElemRec`), `growth`, `scenes` (`SceneRec`), `symtbl`, `drops`, `gear`, `item_info`, shop and trade lists, plus names from `lang.dat`. Records keep raw bytes with accessors. `scene_for(battle_id, variant)` maps an encounter id to a scene (ids from 0x200 are scene numbers). See `docs/formats/battle_data.md`. `battlefields.h` and `battlefield_rooms.inc` (generated by `occ battlefields`) and `area_names.inc` serve the Debug tab. Other tables sit beside their users: `battle/dopple_table.h`, `audio/bgm_table.*`, `import/text_tables.inc`, the two opcode `.inc` files, `game/disc_index.*`.

### 3.6 The disc file index

`src/game/disc_index.*`: `game::DiscIndex` loads `cdrom/cdrom.fnd` (disc 1) and `cdrom/cdrom2.fnd` (disc 2) through the VFS; when disc 2's index is missing both discs share disc 1's. A record is 0x40 bytes, a NUL-padded path; record number = file number. The loader lower-cases the path and maps `\` to `/`, and `record(disc, index)` returns "" outside the file. The game does not open files by name: the index plus the disc in the drive says which file answers.

- **Map ids.** Room id `n` owns three records starting at `kMapBase + 3 * n` (368): `.bin`, `.prd`, `.ctd`. `room_stem(disc, id)` takes the stem from the `.bin` record under `/map/mapbin/`. Several records name another room's file (`FieldWorld::load_room` reads the `.prd` through the record, section 12.2), and a record may differ between discs, so a room id can be a different room on disc 2.
- **Movies.** A movie number `m` is record `kMovieBase + m` (6035), an `.stv` path under `/movie/` whose stem is the `movie.dat` member (`newop` maps to `a01op`). The `PLAY_MOVIE` operand is not the number: it indexes a parameter table (`movie_row`: frame range, mode, number, volume), disc 1 using rows 0 to 23 and disc 2 rows from 24.
- **Dummy records.** Files that exist only on disc 1 appear on disc 2 as a placeholder path (`dummy.dat`, a 24-byte text file); `room_stem` and `movie_name_of` return "" for a record that is not the expected kind. 80 rooms are named only by the disc 1 index and 80 only by the disc 2 index; 52 ids hold a room on disc 1 and the placeholder on disc 2. A map jump resolves its id through the current disc only (`FieldWorld::room_stem_for_id`): a placeholder record gives no room and the jump is refused (the PS1 would read the placeholder as the room); the `lang.dat` room table is used only for an id the index has no record for. A scan of every 5A / DC with a literal room id in the rooms the disc 2 index names finds 39 sites aiming at a disc-2 placeholder; 20 of them sit behind a scenario test below 184 (the scenario at the disc change in bak_a14e), the other 19 (in blk_00, dem_11, gal_b13i, jac_b16i, jac_b17i, mis_b01f, sui_a10e, tel_b09i, tel_b25f, tel_b28e, wld_b00, wld_b01, you_b08f) behind room flags and cutscene state that the static scan does not resolve. If one of them is taken on disc 2, the PS1 reads the placeholder as a room; OpenCross stays in the room and logs the refused exit.
- **Debug starts.** `[Debug] Room`, the route `load` and the debug menu's teleport call `FieldWorld::select_disc_for_room`: a room that only the other disc's index names switches to that disc and sets variable 0x42 to match; a room on both discs keeps the current one.
- **Switching disc.** Script variable 0x42 holds disc number minus one. Op `FE 65` (`CHANGE_DISC`) goes through the menu request path: `FieldWorld::request_menu` stores the operand in variable 0x42 and calls `set_disc(operand + 1)`, which swaps the index at once, even while another screen request is pending. `step_transition` re-reads variable 0x42 before every room jump and `restore_vars` after a load, so a save made on disc 2 continues on disc 2. Starting a new game resets to disc 1.

## 4. The field

### 4.1 Pieces

`game::FieldWorld` (`src/game/field_world.h`, split across `field_world*.cpp`) is the field game mode. It owns:

- `RoomView view_` (`room_view.*`): loads a room, uploads background layers, owns the `RoomScene`, the walk mesh (`walk_mesh.*`) and the projection camera.
- `std::vector<Ent> ents_`: one per script entity. An `Ent` holds an `Actor` (model, skeleton pose, clip playback, position on the walk mesh; `actor.*`), a `Motion` (script-started movement), the party slot and visual state.
- `script::FieldVM vm_`, `FieldCamera camera_` (scroll, scripted target, follow ease, limits; `field_camera.*`), `FieldInput input_`, `FieldWatchdog watchdog_`.
- Dialogue: `ui::DialogueDemo dialogue_` plus `extra_win_` for more windows.
- Effects: `effects::EmitterSystem`, `effects::SequencePlayer`, `GpuEffects` (section 5.5).
- `BattleHandoff handoff_` (`field_battle_handoff.*`): the pending encounter.
- `DiscIndex discs_`: the game's two file indexes (disc 1, disc 2). The disc decides which file answers a map record or movie number.
- `ui::GameState* gs_`: the shared game state (wrapper over `save::SaveSlot`), owned by `GameFlow`.
- `RemoteParties remote_` (`remote_parties.*`, `field_world_remote.cpp`): other players' parties in the room (multiplayer, `formats/multiplayer.md` section 5). Never entities: their own `Actor`s outside `ents_`, with no script, collision or trigger; their models come from the character-model cache through its workers.

`FieldWorld` is `final : public script::FieldHost` and implements every world effect a script can ask for.

World maps are ordinary field rooms whose scripts do the work: they load through `load_room` and run on the same VM, camera and walk mesh, and no separate world-map module exists under `src/game/`. Encounters are script-driven: scripts run battle ops `A3` and `C9`.

### 4.2 The field script VM

`src/script/`:

- `field_script.*`: `FieldScriptModule` parses the room's script bytes into a code area and per-entity script pointers (`kScriptSlots = 32`). The opcode table is `field_opcodes.inc`, one `FS_OP(key, lengths, spec, name, confidence)` row per opcode; `find_op`, `op_table` and `decode_insn` (disassembler) read it. The extended page is keyed `0xFE00 | sub`; `is_extension_op` marks its extension range `FE C0`-`FE FF`, where a size byte frames every instruction. Format: `docs/formats/fieldscript.md`.
- `field_vm.*`: `FieldVM`. Handlers are split across `field_vm_ops_core.cpp`, `_scene.cpp`, `_game.cpp` and `field_vm_ext.cpp` (extended page), `field_vm_extension.cpp` (the extension range: engine features for mods, always stepped over by the size byte; the mod UI ops keep their elements in `mod_ui.*`, `FieldVM::mod_ui()`, drawn by `FieldWorld::draw_mod_ui` and `sync_mod_ui_windows`). `exec(op)` tries `exec_core`, `exec_scene`, `exec_game`; an opcode in the table with no handler is counted in `VmStats::unimplemented` and skipped by its table length.

**Entity threads.** Each entity has `kRecordsPerEntity = 8` `ScriptRecord`s (pc, priority class `state`, script id, wait counter, handshake `phase`) and a 4-entry call stack. The `state` is the priority class: 0xF idle, 7 the default UPDATE loop (script slot 1), 4 ON_COLLISION, 3 ON_ACTION, 2 touch trigger. The lowest class runs; the others stay suspended. Script slot 0 is INIT.

**Scheduling.** `FieldVM::tick()` calls `run_entity(ent)` for every entity in index order, then `host_.end_of_tick(*this)`. `run_entity` picks the live record with the lowest class (`pick_record`), or restarts UPDATE in record 0 when all are idle. `dispatch()` runs at most `budget_ = 8` instructions per entity per tick (some ops grant extra; `OC_VM_TRACE=1` logs every executed instruction with its tick and the budget). A handler yields by setting `yield_` without advancing the pc; the same instruction is retried next tick. Every wait works this way: `WAIT_DIALOG`, `WAIT_MOVIE`, battles and motion waits poll a host method (`dialogue_open()`, `movie_playing()`, `battle_active()`, `motion_busy()`) and yield while it is true.

**Variables.** 16-bit slots addressed by byte offset (`kVarSlots = 0x400`). Below `kGlobalVarSlots = 0x200` they are global and saved with the game; the rest are room-local and cleared by `load()`. Known ids are in `namespace script::var` (`kScenario`, `kRoomParam`, `kMapId`, `kBattleResult`, ...).

**Loading.** `FieldVM::load(module, flag_table)` resets entities and room-local variables, then runs each entity's INIT to completion (`run_init`, capped by `kStepCapInit`) before the first tick.

**Triggers.** `trigger_action`, `trigger_collision`, `trigger_touch` start the matching script slot. `FieldWorld::step_triggers()` (`field_world_collide.cpp`) tests the player against each entity's talk and touch range or box and gives the action button to the nearest talkable entity.

### 4.3 The host interface

`src/script/field_host.h` defines `script::FieldHost`: everything the VM needs from the rest of the engine, one virtual method per op family. Conventions from the header:

- `ent` is an entity index of the current room, already resolved from the script's character reference; -1 means none.
- Methods named `*_busy`, `*_active`, `*_playing`, `*_open` are polls behind wait forms. A `NullHost` answers "done" (the VM unit test `tests/test_script.cpp` uses it).
- Coordinates are script integer units; angles are 12 bit (4096 = full turn).
- Headings cross the host boundary rotated by half a turn: `to_internal(h) = (h + 0x800) & 0xFFF` on the way in (SET_ROTATION, move headings, entrance records) and `to_script(h) = (h - 0x800) & 0xFFF` on the way out (GET_FACING, the facing variables), both in `field_world_internal.h`. An actor's model yaw is its facing minus a further `kModelYawBias` (0x400); the yaw query reports the yaw plus the bias through `to_script`, so it equals the facing query once the model has finished turning.
- Host methods run inside `FieldVM::tick()` or `load()`. A host may call `set_var`, never `tick` or `load`.

Families: entity setup, position and facing, animation, motion (`start_motion`), camera, formation walks, dialogue, layers and screen colour, music and sound, named effects, movies, battle, map jump, menus, party and roster, items and money. Globals that a script writes but the VM does not model itself (control yaw, camera y offset, push strength, pad mask, exit transition, footstep registers, ...) are named by `script::Global`; the VM stores every value (`FieldVM::global`) and forwards it through `set_global`, where the world applies the ones it models and logs the rest as stubs.

### 4.4 Windows and dialogue

Ops `E0 DIALOG`, `E1` (choice) and `E2 WAIT_DIALOG` call `open_dialogue`, `dialogue_choice`, `dialogue_open`, `dialogue_result`. `FieldWorld::open_dialogue` (`field_world_dialogue.cpp`) uses window 0 if free, else the first free extra window, resolves the portrait from the speaker entity and opens a `ui::DialogueDemo` (`ui/dialogue_box.*`, layout in `ui/typewriter.*`, data in `docs/formats/text.md`). `FieldWorld::update` routes confirm and direction input to the open window; `modal_window_open()` decides whether the action key reaches the world or the window.

### 4.5 Pending transitions

A script never changes the host mode itself. It records a request in the world and waits; the App polls it in `advance()`.

| Request | Script op | World state | Handler |
|---|---|---|---|
| Room switch | map jump (`SET_NEXT_ROOM`, ...; `MAP_JUMP_DC` only records a jump that the movie end takes, `set_movie_jump`) | `request_map_jump` sets `jump_`; `begin_jump` starts a fade (none for a movie-end jump); `step_transition` calls `load_room` once the screen is dark | inside `FieldWorld` |
| Battle | `A3`, `C9` | `request_battle`; `handoff_.battle_state`: `None, Intro, Hold, Ready, Running` | `App::field_battle_load_step`, `start_field_battle`, `finish_field_battle` |
| Menu or shop | the screen ops FE 4A / 5B / 5E / 67 / 68 / 72 / 75 / 84 / 85 open it; the `FE 61` after each waits while it is pending or open | `request_menu`; `menu_state_`: `None, Requested, Open` | `App::handle_script_menu`; `menu_closed(result)` lets the FE 61 step on |
| Movie | `7E`, `D4` | `play_movie`; `movie_state_` and a `MovieRequest` | `App::start_movie`; `end_movie` calls `movie_finished()` |

The App asks `menu_pending()`, `movie_pending()`, `battle_ready()` and takes the request with `take_menu()`, `take_movie()`, `take_battle()`. Room-switch fade kinds come from op `FE 02`: 0 fades to black, 2 to white; 1, 3, 4, 5, 6 capture the shown frame (`ctx.request_capture`, `set_captured_frame`) and dissolve or slide it over the new room (`step_capture`).

### 4.6 One field tick

`FieldWorld::sim_tick()` (`field_world_tick.cpp`): the hold-to-ready check of a pending battle; fades and exit handling (`step_fade`, `step_transition` while the room-exit fade runs); `step_capture`; `actor.begin_tick()`; `step_cull`; **`vm_.tick()`**; `step_watchdog`; then the world steps that realise what the scripts asked for and run the player: `step_motions` (the WANDER steps, the turn rates, the scripted motions), `step_player` (pad movement on the walk mesh), `step_status`, `step_followers`, `step_triggers`, `step_visuals`, then `step_push`, `step_bone_emitters`, `step_sound_emitters`, `step_wave`, `step_sfx_slides`, `step_camera`; the effect systems (`emitters_->tick()`, `step_wakes`, `step_shadows`, `sequences_->tick()`); the encounter timeline; `actor.end_tick()`; and last the pending room jump. After each `sim_tick()`, `update()` runs `step_remote()`: the remote parties take the tick's received poses, move and animate. It reads the room and writes none of it.

Followers are not entities driven by the walk mesh: `step_followers` replays two position rings of 0x80 entries (ring 0 the leader, ring 1 the first follower) and writes the member's position directly toward an entry. An entry is appended only on a tick where that member actually moved (the leader's pad step records the spot it stood on before the move; a scripted move appends once the leader is more than half a unit from the last entry, and only while the leader's script runs the update op). The turn-rate and scripted-turn bookkeeping (`step_turn_rates`) and the water wake spawn run only on every second tick (odd tick counts), the engine's per-frame cadence. After a room load `step_cull` skips its off-screen test for 4 ticks.

## 5. Battle

Three layers: the rules (`src/battle/`, headless), the enemy script VM (`src/battle/pac_vm.*`), and the presentation (`src/game/battle/`, `src/game/effects/`).

### 5.1 The rules

`battle::Battle` (`battle.h`, `battle.cpp`). `kPcSlots = 3`, `kEnemySlots = 6`, `kUnits = 9`; units 0..2 are the party, 3..8 the enemies. It is constructed with `GameTables`, an injectable `Rng` (`rng.h`; tests script it) and an `EnemyBrain`.

- `start(party, scene, options)` builds the units, calls `brain.begin_battle`, then runs enemy turns and time until a party member can act.
- `apply(Command)` is the one entry for player input. A `Command` is `Attack` (a chain of strengths 0, 1, 2), `Element` (grid cell and target), `Defend` or `Run`. After `validate` it resolves the action (`do_weapon_attack`, `cast_element`), `post_action`, `check_end`, then `run_reactions` (script counter-attacks) and `settle()` (enemy turns and `time_advance()` until a party member can act or the battle ends).
- `open_menu(pc)` is the host's report that a PC's top command menu came up (the menu passed to it, an L1 / R1 switch, a re-entry); it ends that PC's Defend guard (`docs/rules/battle.md` section 2.2). `BattleView` calls it every tick its top menu is up and not script-locked, from the first tick after the menu entry (the PC's command window opening, `docs/rules/battle.md` section 3.4: `begin_menu_entry`, `tick_menu_entry`); input reaches the top menu only from then on.
- `rules.*` holds the pure formulas (hit chance, damage, heal, status rolls, stamina, growth, loot). Statuses are bit sets in `namespace st`. Every random term goes through the `Rng`.
- Every resolved effect is appended as an `ActionEvent` (kind, actor, target, `hp_before`/`hp_after`, hit code, crit, KO, ring state, the element-trap block (`TrapHud`, drawn by the HUD), text, `action` id); `drain_events()` hands them over. `outcome()` is `Ongoing, Victory, Wiped, Fled, Scripted, Stalled` (`Stalled` ends a `settle()` that used up its step guard; the view's outro and the field hand-off treat it like a flight); `result_code()`, `rewards()` and `growth()` carry the after-battle data. `set_cheats` and `set_invincible` take the cheat flags.

Pip's evolution is `Battle::evolve(unit, form)`, run by the form-change element (pgdat mode 45): it sets the innate colour to the one with the most colour points in the battle script variables (the last of equal ones), zeroes the six counters, sets the unit's kind, AP and countdown, rewrites the form's tech cells and growth column on the unit and the party record, and marks the member `evolved`. `apply_battle_result` (`game_session.cpp`) persists it into the save record: the model id, the tech cells and (through the growth column) the growth.

The Dopplegang pool is a 256-bit mask held in 16 script variables of the field. `app_battle.cpp` passes it to `Battle` at the start (`options.dopple_mask`); `Battle::dopple_credit` sets the bit of a defeated enemy when the last acting party member is Sprigg (character 26) or wears accessory 0xFC and the enemy's record allows it, i.e. the enemy is not flagged uncopyable; `finish_field_battle` writes `Battle::dopple_mask()` back with `FieldWorld::set_dopple_mask`, into both the variables and the save header. Variables 0x3D0..0x3DE (Pip's colour points among them) take the same path as the battle's script variables 0x00..0x0E: `options.saved_vars` from `FieldWorld::battle_saved_vars`, back with `FieldWorld::set_battle_saved_vars`.

`Battle` resolves an action instantly. The presentation decides when each result becomes visible. That split lets the same class run in `ocbattle sim`.

Versus battles (`docs/rules/versus.md`) run in the same class. `start_versus(side_a, side_b, battlefield)` takes two `VersusSide`s (party, summon stars, Dopplegang pool) into units 0..2 and 3..5, with no scene and a `VersusBrain` (no script). `side_of(unit)` and `player_unit(unit)` replace the index tests: targeting, auto-targets, sweeps, the gauge, double and triple techs, stamina recovery (an action of one side recovers the other side as a teammate's would; the turn side gets nothing from its own actions, and a Defend ends the defender's turn: `done(unit)`), upkeep, traps, stars, the pool, Pip's tech colour and the Dopplegang backup are per side; enemy-cast reductions, rewards and the cheats are off. `can_act` adds the turn: only `turn_side()` acts. `acting_side()` is the side whose command comes next: the side that owes a Dopplegang pick (`form_owed()`, also after its cast ended its turn), else the turn side; hi status 0x20000 waives no stamina in a versus battle (`costs_waived`). The menu state that a normal battle's view sets directly travels as commands through `apply` like any action, so two games apply it at the same point: `EndTurn` (with `timeout`), `Focus` (`open_menu`), `ElementMenu` (`open_element_menu`) and `Form` (the pick of a Dopplegang cast). `settle_versus` runs the scheduled turns (confused or frozen characters), passes the turn of a side that cannot act and advances time for a side whose turn starts stuck. `outcome()` reads from side A (`Draw` added), `versus_end()` says how it ended, `winner()` which side won. `state_hash()` digests every rules field and the generator's position (`Rng::fingerprint`), not names or log text; `encode_versus_side` / `decode_versus_side` (`versus.h`) are the versioned exchange format, `versus_battlefield(tables, seed)` the stage pick, and `data::battle_data_fingerprint(tables)` the data check (it includes `data::kVersusRulesVersion`). `ocbattle versus` plays a battle twice in lockstep and compares the hashes after every command; `ocbattle versus-net` plays battles between two clients over a running server.

### 5.2 The PAC enemy-script VM

`src/battle/pac_vm.h`, namespace `oc::battle::pac`. Scripts are `battle/event/evtNNN.pac`, resolved by `game::DiscIndex::battle_script` from the scene's file-index record (fallback `evt0000.pac`); `BattleView` supplies the loader. Format: `docs/formats/battlescript.md`.

- `pac_opcodes.inc` is the opcode table, `PAC_OP(code, length, name, confidence)`; `pac_vm.cpp` includes it to build `op_info(code)`. Opcodes not listed get the default stall handler.
- `Module::parse` reads the entity count, an `[entity][slot]` pointer table (`kSlots = 32`) and the code. `Vm` interprets frames per entity; `tick()` makes one pass over all entities; `launch(unit, slot)` pushes a slot frame; `run_slot(unit, slot, out)` launches and ticks until it returns, collecting attacks queued with op 67 as `QueuedAttack`.
- Slots as `ScriptedBrain` uses them: 0 init, 1 idle loop, 2 the unit may act (`plan_turn`), 3 the unit was killed (`on_killed`), 4 the unit was attacked (`on_attacked`).
- An idle loop can queue an attack itself (Mama Komodo's arrival: self-cast, wait, reveal). `Vm::settle` moves such entries to a list, `ScriptedBrain::take_idle_plans` turns them into actions and `Battle::run_idle_plans` executes them as their own action after the one that woke the loop, so the cast and its camera play before the reveal.
- The VM reaches the battle only through `pac::Host` (`alive_mask`, `read_field`, `write_field`, `rnd`, `pick_element`, ...). `ScriptedBrain::BattleHost` implements it over `Battle`, so the VM is testable alone.
- Presentation requests (dialogue, camera shots, poses, effect starts) are not performed by the VM. It appends `pac::Event` records stamped with a virtual time; `BattleView::collect_events()` moves them into `script_stream_` for replay.
- `ScriptedBrain` implements `EnemyBrain`; with no script it falls back to `RandomBrain`. `Battle` asks it `hidden`, `untargetable`, `death_owed`, `forced_result`, `no_drops`, `menu_locked`.

### 5.3 How an action is resolved

1. The player confirms in the HUD; `BattleView::execute(Command)` (`battle_view_action.cpp`) calls `Battle::apply`. HP is final inside `Battle` at this point.
2. `BattleView::collect_events()` moves `drain_events()` into `queue_` (grouped by `ActionEvent::action`) and the PAC events into `script_stream_`.
3. `BattleView::tick()` (60 Hz) calls `tick_action()` (`battle_view_sequence.cpp`) to play one group: `begin_engage`, `start_hit`, `start_return`, `start_release`, `end_group`. The motion follows the models' animation-sequence scripts (`battle_seq.*`): approach, attack, reaction, return. Displayed HP, damage numbers (`battle_numbers.*`) and KOs change on the script's events (`on_script_event`, `on_damage`, `spawn_pop`), not when `apply` ran.
4. Element casts start an effect pack (`battle_view_fx_cast.cpp`); hits are queued behind the effect's timing.
5. When the actor's own action is over (before any enemy actions in the queue) the next party member's command window starts opening (`begin_menu_entry`); with the queue empty and the window open its top menu takes over, or `begin_finish` / `tick_finish` ends the battle and sets `finished()`.

### 5.4 Presentation

`BattleView` (`src/game/battle/battle_view.h`) is split by concern: loading (`battle_view.cpp`, `battle_model_cache.*`), camera (`battle_camera.*`, `battle_view_camera.cpp`), unit animation (`battle_pose.*`, `battle_seq.*`, `battle_view_units.cpp`), action playback (`battle_view_action.cpp`, `battle_view_sequence.cpp`, `battle_view_script.cpp`), effects (`battle_fx.*`, `battle_fx_textures.*`, `battle_spell_lights.*`, `battle_view_fx*.cpp`, the trail and pseudo-actor files), the stage (`stage_scene.*`), HUD and menus (`battle_view_ui.cpp`, `battle_view_hud.cpp`, which also draws the element-trap block), results (`battle_results.*`), sound (`battle_audio.*`) and the swirl (`battle_transition.cpp`). `BattleView::set_select_held` (Select held during a cast hides the HUD), `set_autohide_cast` (`[Enhancements] AutoHideCastHud`: the same hiding on every cast) and `set_keep_intro_layer` (`[Enhancements] RetainBattleScenery`: the stage's intro scenery layer is not stopped when the intro ends) are pushed by the App every frame. `texture_dump.*` is the headless "Dump all textures" job used by the Mods tab and `occ dump-textures`.

A versus battle (`StartOptions::versus`, a `VersusSetup`: the seed, both sides, the side this game plays, the opening stamina and the battlefield) is drawn by the same view (`battle_view_versus.cpp`). The rules start with `Battle::start_versus` on a `VersusBrain` and the shared seed (the environment's seed is not read); the scene given to `start` is a carrier (`battle::versus_carrier_scene`: the lowest ordinary scene of that background) that lends its stage and party spots only. Each game shows its own player's party in units 0..2: the game that plays side B swaps units 0..2 with 3..5 between the rules and everything the view keeps (`rules_unit`, `bunit`, the query wrappers, `map_event` for the events' actor, target, aim, group and trap block, `rules_command` for the commands), so the menus, targets, camera and HUD keep their party = 0..2 logic. The other party stands on the party's spots mirrored in x, turned half a turn, and spawns as enemies do. The menu state the view sets directly in a normal battle goes through the rules as commands (`Focus` when a PC's top menu takes over, `ElementMenu` when its grid opens, `Form` right after a Dopplegang cast), and the view never settles the rules: the turns run on by themselves. The other side's commands come through a `VersusLink` (`versus_link.h`, library `oc_versus_link`: `send` this game's accepted commands, `poll` the other side's in the order its game applied them, `send_hash` the state hash after every command, `status` open / desync / closed / conceded / forfeited, `away` who is out of reach and for how long); `versus_pump` applies them one action at a time while the view is idle and the other side acts (`Battle::acting_side`; the view's `can_act` keeps this game's menus shut while the other side owes a Dopplegang pick), with the state commands that go with it, a refused one ending the battle with no result; a conceded or forfeited link ends it with `Battle::concede`. `LocalVersusOpponent` is a link that plays the other side in this game (`battle::VersusAutoPlayer` without early End Turns, timeouts or Run), and under `set_auto` the same player commands this game's side. `NetworkVersusLink` (`versus_net_link.*`) is the other player's game over `net::MultiplayerClient` (`formats/multiplayer.md` section 8.4): commands as text (`versus_command_text` / `parse_versus_command`), each of the other game's checked to be the next of the stream, the hashes compared both ways, the server's word on the other player (away, gone) and the 90 s silence limit. The view adds the End Turn row, the turn banner and 60 s clock (the banner says "Waiting for <name>... N" or "Reconnecting... N" while the link reports a player out of reach), the Run confirm, and the outcome plate in place of the results; `versus_result()` tells the host how it ended.

`BattleResults::open(battle, scene)` returns false when there is nothing to show (result codes 1, 3 and 4, scene 0x1DD, or an outcome other than victory, flight or scripted end); the host then calls `finish_field_battle` at once, otherwise after the screen's `done()`. Pages, each advanced by Ok: the star-up message first when the battle raised the star; then one page per member who gained a stat; then the heal-item menu, only if a member is hurt and the grids still hold an unused healing element (Cancel or "do not heal" skips it); then the spoils; then the white exit fade. A member's HP gain is added to their current HP (cap 999) when their page is first shown, so the screen's HP is the final one. After `done()` the host reads it with `take_final_hp()` (per party slot, -1 for no member; false when no screen ran since the last call) and writes it over what `apply_battle_result` stored, clamped to 1..max HP.

### 5.5 The effect engine

`src/game/effects/` (`oc_effects` headless, `oc_effects_gpu` GPU glue). The same engine runs room effects, battle effects, the battle stage and the title scene.

- `EmitterSystem` (`emitter_*.cpp`) instantiates type-0x0A emitters from parsed `RoomEffects`, steps them per tick and turns live objects into `DrawQuad`s with the ordering-table depth rule and blend modes. No GPU or audio dependency.
- `SequencePlayer` (`sequence_player.*`) runs type-0x10 sequence tasks: each command has a wait in ticks; emitter control, ambient colour, shake, rumble and sound ops run here; scene ops (actor show/hide, camera records, flashes) go out through `SequenceHooks`.
- `effect_slots.*` maps an effect id to its pack files. `gpu_effects.*` uploads sprite sheets as atlases and draws quads through `render::ParticleBatch`. `soft_raster.*` is a CPU twin for `ocprd preview` and tests. `room_meshes.*` draws effect mesh objects; `light_extract.*` serves the spell lighting of the optional `[Enhancements] Lighting` setting; `strip_smooth.*` serves `SmoothEffectRings`; `fx_math.*` holds the fixed-point and matrix helpers.
- `effects/bespoke/` holds hand-written overlays for individual techs that the generic engine cannot express; `bespoke.cpp` is the registry (`BespokeRegistry::instance()` registers every family, the later model-steer ports replacing older stand-ins of the same name). The selection rule: an effect slot's overlay file (`EffectSlot::prg`) is matched by its lower-cased file stem against the registered names (`find`, tolerating a leading authoring index such as `46black` for `black`); `bind_slots` binds each matching slot's element id, and `find_element(eid)` returns the overlay that `BattleFx` creates (`create`) when that element's pack runs.

The field starts effects by name through `named_effect`; `FieldWorld::build_effects()` creates a room's emitters and sequences and starts the pack's `loop` sequence. `game::TitleBackdrop` plays the title scene's effects. Formats: `docs/formats/battle_effects.md`, `room_effects.md`, `battle_stage.md`, `title_scene.md`.

### 5.6 Field and battle hand-off (`app_battle.cpp`)

The App owns the hand-off because it owns both objects. `BattleView` is initialised once; each encounter loads a scene into it.

- `make_field_battle_args(rq)`: `tables.scene_for` gives the scene; `world.party_chars()` and `game::party_member_from_state` build the party (the Relief Charm swaps in); `game::battle_setup` adds growth and the bench; the A3/C9 battlefield operand picks the stage.
- `field_battle_load_step()`: from the fourth frame after the request, loads in 3 ms slices (`BattleView::begin_load`, `load_step(3.0)`, `prepare_fx_targets`) behind the field's encounter swirl. The steps: rules, stage files, stage scene, the shared `battle.prd`, the effect sheets and meshes every attack draws from (the resident `battle.prd` set and the particle library, uploaded in slices here instead of at the battle's first hit), one unit model each, the HUD fonts.
- `start_field_battle()`: `world.take_battle()`, `battle.start(...)`, `battle.begin_transition(...)`, sets `battle_in_field`. An invalid scene is reported as "fled" (`world.end_battle(2)`). `take_battle` drops the extra animation packs as the original's field snapshot does (`Actor::drop_ex_pack`): every entity that holds a pack (a C1 or BF not yet followed by C0; `Actor::holds_ex_pack`) loses it and its current animation becomes its stand slot, whatever it was playing, a base clip included; a stand slot that names a pack clip (0x80 and up) is set to 1 for good. Entities without a pack keep their animation. A script can reach a battle op with a pack loaded (gal_b16e, hid_a04f, jac_b01f and mis_b02e run C9 after a C1 with no C0 / BF in between; mis_b02e's Kid plays pack clip 0x80 into battle 390).
- `finish_field_battle()`: maps `battle.outcome()` to a code (Victory 0, Wiped 1, Scripted = `result_code()`, else 2), calls `game::apply_battle_result` (`game_session.*`), copies the results screen's final HP back, and calls `world.end_battle(code)`. The original rebuilds the field after a battle: every model starts afresh (no clip, no blend, no sequence) and every entity with a 3D object (`EF.14` clear) has its current animation submitted again, so a model with an animation-sequence table runs that id's sequence from the top and the sequence cuts its clip in at frame 0, and any other model cuts the clip in at frame 0; the anim-done bit is set. No entity holds a pack then, so a pack id plays the stand slot. `end_battle` does this with `Actor::restart_anim`. A frozen clip after the return is not a kept frame: a monster beaten by its own fight is made inactive and hidden (`D2`), and a hidden or culled entity runs no animation (fieldscript.md section 8.1). The original's fresh model has no previous clip, so its first sequence clip blends in from an undefined pose for the sequence's blend length (16 ticks for the party's stand); that blend is never seen: it ends about 30 frames before the return's fade-in lights its first frame, and the first lit frames show only the stand sequence's own loop blend (stand into stand, below a quarter of full brightness). OpenCross cuts the clip in, as at a room load. A wipe in a fight that is not losable loads the game-over room (map id 9).
- A versus battle (`start_versus_battle(setup, link, net)`: from a free field, refused while a battle dev switch of the environment is set) takes the same path: `versus_next` makes the next field battle request (battle flags 0, the regular battle music) a versus battle, and `make_field_battle_args` then gives only its carrier scene, battlefield and setup (no Relief Charm write, seed variable, party swap or Dopplegang setup). At the end `finish_versus_battle` writes nothing back (no `finish_field_battle`, `apply_battle_result` or results screen): `FieldWorld::end_sandbox_battle` restores the music and fades in as `end_battle` does, with `BATTLE_RESULT` and the last result code as they were, so the save is unchanged; a server's battle sends its `pvp_end` (`desync` when the link ended out of step). Pause, fast and slow mode (`speed_locked`) and the Debug tab's casts are off while it runs.

`app_feedback.cpp` joins the pictures: the field's last output and the battle's first frames share one previous-output feed-back (`fb_rt[]`), so no black frame appears. `encounter_strips.h` holds the curve both sides evaluate.

## 6. Rendering

### 6.1 Backend

`src/render/gpu_context.*`: `GpuContext` wraps SDL_GPU. `init(window, vsync, driver)` takes an optional driver name (`vulkan`, `direct3d12`, `metal`) and falls back to SDL's choice. Shaders are GLSL in `src/render/shaders/` (`sprite`, `mesh`, `stage`, `particle`, `crt`), compiled at build time and embedded as SPIR-V, DXIL and MSL (`shaders.h`, `embed_shader.cmake`; without shadercross only SPIR-V is embedded); `create_shader` picks what the device accepts. `set_msaa(samples)` (1, 2, 4 or 8, lowered to what the GPU supports; very large render-scale targets get fewer samples) and `set_anisotropy(level)` (1 to 16) take `[Video] Msaa` and `Anisotropy` and apply live: a change releases the multisampled images, and the 3D pipelines bind `filtered(sampler)`, the anisotropic twin of a linear sampler. `begin_overlay()` moves the rest of the frame onto the resolved image for ImGui.

`begin_frame(Frame&)` starts a render pass into colour and depth images and returns false when the window is minimised or resizing. `end_frame()` presents. Each frame records two command buffers: `upload_cmd()` (copies, run first) and `cmd()` (draws). `begin_target` / `end_target` render into a `RenderTarget` and nest. `request_screenshot` and `request_capture` read a frame back. `kFramesInFlight = 2`.

### 6.2 Batches and pipelines

- `SpriteBatch`: 2D textured quads in a caller-chosen virtual space; blend modes Alpha, Multiply, Add, Subtract, Average, Premultiplied, AddAlpha. `TextBatch` draws text over it (RDE spritefont faces, the PS1 bitmap font).
- `MeshPipeline`: skinned, alpha-tested 3D meshes (palette of up to 128 joints) for field actors, battle units and the Status model. `DrawParams::opacity` below 1 draws a model see-through (a depth pass, then its solid pass alpha-blended); only the multiplayer `Translucent` option uses it.
- `StagePipeline`: battle stage and title scenery packet meshes as instances placed by object scripts (colour, blend, ordering-table bias per instance).
- `ParticleBatch`: effect quads, depth-tested against room layers and actors.
- `ScreenPass`: the optional full-screen shaders (`[Shaders] Fullscreen`, `Grade`), also the pause screen's colour ramp. `RenderTarget`: off-screen colour and depth. `ImGuiLayer`: Dear ImGui over SDL_GPU.

HD images that RDE ships per language (`<stem>_<en|fr|it|de|es|jp><ext>` beside the plain `<stem><ext>`: the battle results UP marker, the HIT% leader line, the party-change frame, the menu panel shapes) are read through `ui::localized_path` (`src/ui/localized_path.h`): the suffixed name of the game language when the file system holds it (a mod's copy counts), else the plain name.

Textures that are not whole files (stage atlases, effect sheets, room-model pages) are managed by `texture_registry.*`, `stage_tiles.*` and `model_tiles.*`: they are cut into content-hashed tiles that can be dumped to PNG and replaced by mods. Replacement does not stall the frame: stage tiles are replaced with `parallel_for` on worker threads (`stage_tiles.cpp`), their textures are created from a worker with `Texture::create_blocking` (upload in bands on separate command buffers), and field and battle models resolve their texture mods in the CPU half of a load on a worker (`render::resolve_model_textures`, `Actor::make_model`, `battle_model_cache.*`). A character model's files are found and decoded by one reader, `game::read_model_files` (`src/game/model_files.*`: the field pairing `.tim` + `hd/` 4x `.tim` + `fieldobj/ex` pack, the battle pairing `.ltd` / `.ltc` + `at0.prd`), used by the field, battle, the texture dump and `ocmodelview`.

### 6.3 What draws what in a frame

`App::draw_frame(frame)` (`app_draw.cpp`): pause handling (freezes battle, movie, mixer) and CRT begin; `apply_frame_options()`; then one of movie (`movie_player.draw`), boot logo (`flow.draw_boot_logo`), a flow screen (`flow.draw`) or the scene (`draw_scene`); then veils (`movie_player.draw_held`, `flow.draw_overlay`), CRT end, feature icons, pause screen.

`App::draw_scene(SceneFrame&)`:

1. Set `render::g_ps1_jitter`; `begin_scene_target` (render scale, damage jolt).
2. Title sea (`title_bd.draw`).
3. Room background layers (`FieldWorld::draw_background`), clipped to the field picture.
4. Battle 3D (`draw_battle_picture`, `BattleView::draw_scene`).
5. Field actors, effects, late layers (`FieldWorld::draw_actors`, `draw_effects`, `draw_background_late`), through `draw_field_full` or `draw_field_lowres`. `draw_actors` also draws the remote parties' members and computes their label points (`FieldWorld::remote_labels()`); `draw_background` draws their floor shadows (with `[Enhancements] Lighting`, the silhouette tiles `prepare_shadows` cast for them after the room's actors).
6. `composite_scene` if a scene target was used.
7. UI: `draw_battle_ui`, `draw_field_ui` (dialogue via `FieldWorld::draw_windows`, fades, world name), `draw_title_menu`.

The screen ripple (ops 8F/90) draws the deep part of the field into a target, re-draws it warped (`draw_warp`), then draws the near part over it (`FieldWorld::set_draw_slab`).

### 6.4 PS1-style quirks, widescreen, scale, post-processing

- **Layer space.** `render/camera.h`: a 320x240 layer space; the field is a 320x224 buffer with a 2-unit crop (`kFieldRows`, `kFieldCrop`, `kPresentRows`). `Letterbox` (`fit`, `field`, `wide`, `identity`, `veil_rect`) maps it to the window. The room's camera block gives the view matrix and an off-centre perspective.
- **Ps1Jitter** (`ps1_jitter.h`): the 3D pipelines snap projected vertices to whole PS1 pixels and interpolate UVs screen-linearly.
- **Ps1ResGraphics / Ps1ResUi**: two independent switches. Ps1ResGraphics draws the field picture, battle 3D and title sea into a PS1-sized target (`field_rt` 320x224); Ps1ResUi draws the UI screens, battle HUD, results, pause screen and field windows into PS1-sized targets (`ui::LowRes`, `ps1_pass_43`, `ps1_pass_wide` in `app.h`). Both scale up with nearest sampling. `cull_frame(wide_cols)` scales the PS1 packet-culling limits with the picture width.
- **Widescreen.** `render::wide_extra()` is the number of extra layer columns per side, set each frame in `App::frame_settings` from the window shape. Layer space and script units do not change. The title room's 4:3 layers compose with 0 extra columns. `[Video] WideHud` limits how far the battle HUD and main menu spread. `[Video] FovDeg` sets `render::fov_scale()` for 3D pictures not tied to painted backgrounds (battle, title sea).
- **Render scale.** `[Video] RenderScale` draws the 3D pictures into `scene_rt` at scale times the window size (`scene_extent`, at most 16384 per side), then `composite_scene` filters it down. HUD, menus and veils stay at window resolution. With Ps1ResGraphics the PS1 target is used and the scale does not apply.
- **Post and enhancements.** Full-screen shader pass, damage-floor jolt blur (`jolt_rt[2]`), encounter feed-back and the FE 14 frame echo (`fb_rt[2]`), screen ripple. The `[Enhancements]` keys (SmoothEffectRings, SmoothWaterLine, SmoothReflections, Lighting, EdgeBlend, AutoHideCastHud, RetainBattleScenery, FullPrecisionEffects) are applied in `apply_frame_options`; with them off the output is the PS1 picture. `Lighting` covers the cast shadows of battle units and field actors (`render::ShadowPipeline`; `battle_view_shadows.cpp`, `FieldWorld::prepare_shadows`), the model key light and the spell glow; the older names `EnhancedLighting`, `SpellLights` and `SeamBlend` are still read and mapped to `Lighting` and `EdgeBlend` (`config.cpp`).
- **Shaders.** The `[Shaders]` character keys make one `render::CharacterShading` for the field actors', the battle units' (and weapons') and the status screen model's mesh draws (`FieldWorld::set_character_shading`, `BattleView::set_character_shading`, `StatusModelView::set_character_shading`, from `apply_frame_options`); every other mesh draw (title, Grand Slam picker, effect models, shadows) stays unshaded. The normal is the skinned VDDM normal, or a rigid mesh's smoothed normal; the light is the camera-relative key light `k` of `Lighting`, `d = n . k`, and the shaded colour is the PS1 colour `C` (texel x light multiplier) scaled per pixel:
  - realistic: `C (0.6 + 0.5 (d / 2 + 1/2)) + L 0.22 max(n . h, 0)^16`, `L` the light multiplier, `h` the unit halfway vector of `k` and the direction to the eye;
  - toon: `C mix(0.55, 1, smoothstep(-0.05, 0.2, d))`;
  - cel: `C (0.5 + 0.6 b)`, `b = (floor(4 t) + smoothstep(0.42, 0.58, fract(4 t))) / 4`, `t = d / 2 + 1/2`;
  - matcap: `1.6 C M`, `M` the matcap texel at the normal in the key light's frame (camera right, model up, toward the camera; the built-in looks are all lit by the key light) (`MeshPipeline::matcap_index`: the built-in `clay metal ink`, made at init, then the mods' own, `App::load_shader_looks`).
  A normal pointing away from the viewer is turned toward it first. `Rim` adds `0.4 L rim (1 - f)^3` (`f = n . v`, `rim` white, or with `SpellRim` the hue of the summed spell light, `BattleView::unit_shading`); `CastGlow` mixes in the casting element's colour, `mix(C, 1.5 C g, 0.35 a) + g a (0.12 + 0.5 (1 - f)^2)` (`BattleView::cast_glow_tick`).
  On a shaded draw the `Lighting` key-light term is not added; the spell lights and the scene ambient are. `Outlines` adds a pass before the opaque pass, depth tested but not written, so the model covers it inside its silhouette: the opaque one-sided parts with the back-face test inverted, every corner moved along its projected normal by `MeshPipeline::kOutlinePx` PS1 pixels on screen, coloured `0.15 C`; parts without normals are not extruded. With every `[Shaders]` key off the mesh output is unchanged. The full-screen keys go to `render::ScreenPass` (`App::screen_look`): the whole frame before the ImGui overlay is drawn into its target, then through `screen.frag`: the filter (`crt` with its mask, curvature and glow; `composite`: YIQ with the chroma blurred over about three PS1 pixels, a dot crawl and a ghost), and the grade (built-in curves or a mod's LUT).

## 7. UI

Menus draw on a fixed 1280x960 canvas fitted into the view. The same layout code serves both art modes; original art swaps textures and fonts (`ui/menu/ps1_art.*` decodes disc art). `ui::LowRes` renders a screen into a PS1-sized target when Ps1ResUi is on. The target is cleared transparent and composited premultiplied; Subtract and Multiply quads (window bases) change only its colour and are drawn again into two more targets of the same size (subtracted amounts on black, factors on white) that are applied to the frame before the composite, so windows stay see-through as at window resolution. Text goes through `ui/ps1_font.*` and `render::TextBatch`.

Screens: boot logo, title, name entry and slot list in `ui/title/`; the user guide in `ui/keymap_screen.*`; the main menu (Status, Elements, Equip, Key Items, Settings, Save) in `ui/menu/main_menu*.cpp` with one `Impl` in `main_menu_impl.h`; `ui::GameState` (`ui/menu/game_state.*`: party, roster, inventory, element grid, options word); the Status 3D model in `ui/menu/status_model.*` (`oc_menu_model`, because `oc_ui` cannot depend on the mesh pipeline); shops in `ui/shop/`; party screens (`ui/party_screen.*`, rules in `game/party_editor.h`), pause (`pause_screen.*`: the PAUSE bar over the colour-ramped picture, with Vibration and World Names rows; the vibration option is stored in the game's options word and World Names in script variable 0x52) and key-item list (`keyitem_popup.*`); dialogue (`dialogue_box.*`, `typewriter.*`, `ps1_window.*`); RDE's feature icons (`feature_icons.*`: the speed, No Encounters, Battle Boost, Auto Battle and AUTO SAVE marks) and world name (`world_name.*`). The battle HUD and results live in `src/game/battle/`.

**Game flow.** `game::GameFlow` (`game_flow.*`) binds `FlowMachine` (a pure state machine; `FlowState` is `Title, SlotSelect, NameEntry, Opening, Field`) to the title screens. Events (`on_title`, `on_name`, `on_slot`, `on_load`, `new_game_plus`, ...) return a `FlowAction` (load room, restore position, movie row, quit, stop music, open settings) that `App::apply_flow_action` obeys. New game and New Game+ transforms are pure functions in `new_game.*` and `new_game_plus.*`. The title scene is the field room `opening` (`kTitleRoom`) with `TitleBackdrop` drawing the 3D sea. Flow states: boot and Restart enter `Title`, which loads the title room with ROOM_PARAM 0 and runs it with no input while the title screen reads the buttons (`GameFlow::scene_mode`); New Game goes `NameEntry` -> `Opening` (the dream rooms `opn_*`) -> `Field`, Continue `SlotSelect` -> `Field`. During play the game ends by a jump to the title room with ROOM_PARAM 0 (after a game over or the credits): its INIT runs FE 2F, which resets the state to the boot state as the original does, and the field reports it (`FieldWorld::take_game_reset`); in `Field` or `Opening` the host then calls `GameFlow::return_to_title`, which enters `Title` over the room already running (no boot logo, no reload; the room plays its own logo FMV), so the title after a game is the boot title. FE 2F at the boot title itself changes nothing.

**Settings and debug overlay.** `render::ImGuiLayer` draws a tabbed window opened with Esc (or a long pad Start press). General (order 0; Game, Audio and Controls sections) and Graphics (10) are built in; others register with `add_tab(name, fn, order, highlight)` (`highlight`, when given and true, draws the tab label in green with a small red dot at its top-right): Mods (30, `imgui_mods_tab.*`), Cheats (40, `game::draw_cheats_tab`), Multiplayer (50, `imgui_multiplayer.*`, `App::init_multiplayer`), Update (80, `imgui_update_tab.*`), Debug (90, `App::init_debug_tab`). `options_window(config, file)` edits the `platform::Config` and returns an `OptionsChange`; `App::apply_options_change` applies it (VSync, MSAA, anisotropy, window, art, audio, BGM source, portraits, language). `toast(text, seconds)` shows a notice (1.8 s unless given). The open window pauses the field (ticks = 0) except on the Debug tab.

## 8. Audio and movies

All in `src/audio/`; output is 48 kHz stereo float.

- `Mixer`: opens an SDL audio stream; `render()` is the device-independent core, also called by tests and tools. Buses: `Master, Bgm, Sfx, Ui, Movie, Voice` with atomic gains. One BGM channel driven by a thread-safe command queue (`play`, `play_akao_bgm`, `stop`, `fade`, `pause`, `resume`, `hold`). `hold(true)` freezes music, voice and movie under the pause screen.
- **Remastered music**: `HcaStream` decodes the HCA stream inside a `.sab` (`sab.*` parses the container; `clhca_unit.c` decodes). `bgm_table.*` maps a song id to its file and loop points. `FieldWorld::start_song` reads the file through the VFS and calls `Mixer::play`.
- **Original music**: `AkaoPlayer` is a sequencer plus SPU model (24 voices with ADSR, 240 Hz update clock, Gaussian interpolation). `akao_seq.*`, `akao_bank.*` (instrument banks, SPU ADPCM in `adpcm.*`), `akao_tables.*` and `spu_reverb.*` support it. `Mixer::play_akao_bgm(song, volume)` starts a song. `bgm_switch.h` maps positions so `[Audio] BgmSource` can switch sources live.
- **Sound effects**: the `Sfx` interface (`sfx.h`) serves the field host (op `C6`) and the battle sequencer. `LogSfx` only logs; `AkaoSfx` plays original effects from `SfxBank` on the AKAO SFX channel pool. Field sounds carry channel masks so a room change or battle can evict them (`Mixer::evict_sfx`).
- **Voice**: `VoicePlayer` decodes MP3 lines on a worker thread (Windows Media Foundation) and feeds the voice bus; it searches enabled mod folders.
- **Mono**: the options word's Sound row (Stereo / Monaural) is pushed to `Mixer::set_mono` every frame; with it on, every SPU voice (original music and effects) takes the centre pan and the voice line is folded to L = R = (L + R) / 2; the remastered stream and the movie sound stay stereo, as in RDE (`formats/audio.md` 6.3).
- **Battle sound** (`game::BattleAudio`): sounds are named by tag and looked up in a list of sound groups, newest group first. A group is `Resident` (opened for the whole battle: `battle.prd`, the stage), `Actor` (the acting unit's file, one at a time, opened at its action's start and closed at its end) or `Pack` (a spell's effect pack, closed at the pack's end). Closing a group cuts the sounds it started: each group kind adds its own channel mask bit to the battle mask, and `stop_sounds` cuts all of them at the end of the battle. A play with a non-zero mask first ends the sounds carrying it, so the cast chant (mask 0x100) replaces the previous chant; `fade_chant` fades it out when the pack opens. The battle theme is ducked only around element casts (`duck_cast` at the cast start, `restore_cast` at the effect's end).
- Formats: `docs/formats/audio.md`; voice layouts: `docs/formats/mods.md`.

**Movies** (`src/movie/`). `movie::Decoder` wraps librempeg: Bink 2 (and HEVC / H.264 in mp4 or mkv for mod movies) to RGBA video and 48 kHz audio. `movie::Player` opens a `movie.dat` member by name, decodes on a worker thread, paces frames against the mixer's movie PCM clock, and exposes the frame as a texture. `App::start_movie` sets `movie_open` and the movie's sound level (the row's volume; the BGM keeps playing); `advance()` calls `movie_player.update(dt)` until `finished()`; `end_movie` keeps the last frame over the resumed field, held for 10 frames or faded out over 160 by the movie's flag 8 (`movies.md` section 6). `DiscIndex::movie_range` gives the frame range of excerpt rows. See `docs/formats/movies.md`.

## 9. Save

`src/save/`; format: `docs/formats/save.md`.

- `cc_save.*`: `SaveSlot` (the 0x2000-byte slot block) with `decode_block` / `encode_block` (exact inverse; unknown fields survive). `SaveDat` wraps `save.dat`, a ZIP of `memcard1.mcr`, `memcard2.mcr`, `rd_save.dat`, `achievement.dat`, `ext_save.dat` (`load`, `save` via temp file and rename, `create_empty`, `ensure_cards`, `card(n)`).
- `memcard.*`: the 128 KB PS1 card image. `encrypted_zip.*`, `crypto.*`: WinZip AE-2 AES-256 reader and writer with local AES, SHA-1, HMAC, PBKDF2.
- `ext_save.*`: per-slot language, battle counter, 44 custom names (`ExtHost` is the live source). `oc_saves.*`: the OpenCross page, an auto-save plus 100 files in an `OpenCross` folder next to `save.dat`.
- `game::Autosave` decides when to write on a location change; `App::advance` writes card 1 slot 0 and/or the OpenCross page.
- The single source of truth in memory is `ui::GameState`. `FieldWorld::capture_state()` writes VM variables into it before a save; `restore_vars()` reads them back after a load. `game_session.*` converts it to `battle::PartyMember` and back. Save and load screens: `ui/menu/main_menu_save.cpp`, `ui/title/slot_list.cpp`.
- Memory cards: both cards are first-class. `SaveDat::ensure_cards` formats a card member that is missing or not 0x20000 bytes, and the load screens (`SlotList::collect(dat, card)`) and the Save menu choose between Slot 1 (`memcard1.mcr`), Slot 2 (`memcard2.mcr`) and the OpenCross page (card 0 in `SlotEntry`); card 1's slot 00 is the RDE auto-save.
- `init_saves` creates `save.dat` with two formatted cards on first boot and loads the OpenCross page from its folder (`OpenCross`, or `<name>_OpenCross` for another `[Paths] SaveFile`); `init_menus` makes a one-time `.ocbak` copy.

## 10. Platform, config, core

- `platform::Window`: SDL3 window, event pump, gamepad. `pump(Input&)` fills `platform::Input` (key and button down/pressed arrays, axes) and returns false on quit. `platform::stick_to_dpad` turns the left stick tilted past half way into the matching d-pad button (held, and pressed on its first frame); `App::advance` applies it to the input of every tick, so every screen, the field's direction bits and its choices take the stick with their own d-pad repeat. Field walking still reads the stick's angle as analog input. Window modes (windowed, borderless, exclusive), size and display (`apply_video`), swapchain resize signal (`take_resized`), rumble (`set_rumble`). The window is never user-resizable.
- **Rumble.** Each frame the App passes `Rumble::flush(vibration option, dt)` to `Window::set_rumble`. The flush adds the sequence players' motor values (small 0 or 1, big clamped to 255) and the test pulse, all zero when the options word's Vibration bit is off. `rumble_map` converts to the pad: the low-frequency motor is `(min(big, 255) * 0.0037255 + 0.05) * 65535` (0 when big is 0), the high-frequency motor is full when small is non-zero. Switching the option on in the pause screen starts a pulse of big 0xE0 that falls by 10 per 1/60 s. `set_rumble` sends zero when there is no pad or the window lacks input focus, and otherwise sends a changed state at once, a repeated non-zero state every 250 ms (each request lasts 400 ms), and nothing for a repeated zero.
- `platform::Config` (`config.*`): `opencross.ini` reader/writer (`class Ini`; comments are not preserved) and the typed `Config` struct; every key is listed in `docs/formats/config.md`. Sections found in `config.cpp`: `[Video]`, `[Game]`, `[Audio]`, `[Enhancements]`, `[Cheats]`, `[Mods]` (texture-dump categories and per-mod preferences), `[Paths]`, `[Ui]` (the settings window's position, size and last tab), `[Debug]`. `[Video]` includes `Driver`, `Msaa`, `Anisotropy`, `RenderScale`, `Ps1ResGraphics`, `Ps1ResUi`, `Ps1Jitter`, `FovDeg`, `WideHud` and `FrameCap`.
- **Steam** (`steam.*`, `game/achievements.*`). `App::update_achievements` connects when `[Game] Steam` (default off) turns on and disconnects when it turns off; runs with `--config` or `OC_QUIET` never connect, and they alone evaluate the rules without Steam. With the setting off nothing is earned. Steam keeps showing the game as playing after a disconnect until the process exits. It sets `SteamAppId` and `SteamGameId` to 1133760 in the process environment, requires a running client (`HKCU\Software\Valve\Steam\ActiveProcess`: non-zero `pid`, then `SteamClientDll64`), loads that DLL, and uses its exports `CreateInterface`, `Steam_BGetCallback`, `Steam_FreeLastCallback`. Through `SteamClient017` it creates a pipe and connects the global user, which is what makes Steam show the game as playing; `STEAMUSERSTATS_INTERFACE_VERSION012` gives the stats and `SteamFriends017` the user's persona name (`persona_name()`, multiplayer's default name; its absence leaves the name empty and changes nothing else). `SteamClient` talks to the stats through `platform::SteamBackend`; the running client is one implementation, tests supply another. No SDK header, library or DLL is involved: only the interface names and the slot numbers in `steam.cpp` are declared by hand. `pump()` runs every frame. Callback 1101 (stats received) makes unlocks possible; `unlock()` queues until then. An unlock reads the achievement first, sets it, then stores; after an unlock that leaves all 37 names achieved, the meta `ach_ps41_all` is set in the same store (`set_meta`); callback 1102 (stats stored) with a failure, or a failed store call, retries every 10 seconds, and failed stats requests likewise. Steam missing at any step leaves the client unconnected and every call a no-op. `App::update_achievements` runs at the top of `advance`: it feeds `game::Achievements` (room rules, the rules in `docs/rules/achievements.md`), passes earned names to Steam unless the debug tools were used, and shows `Achievement unlocked - <title>` as a toast for each one Steam accepted, with the title in the Steam client's language. The events come from `FieldWorld` hooks (item given, variable bit set), `BattleView::take_cast_elements`, `ShopScreen::take_first_forged` and `finish_field_battle`. Without an overlay in a directly started exe, the toast is the player's only notice.
- **Multiplayer** (`src/net/`, `render/imgui_multiplayer.*`, `app/app_multiplayer.cpp`; protocol in `docs/formats/multiplayer.md`). `oc_net` holds a small JSON value with its reader and writer (`json.*`), the `net::Transport` interface (connect, send, poll opened / message / closed, close) with the WinHTTP WebSocket implementation (`websocket_transport.cpp`: a worker thread connects, upgrades and sends; a receiver thread blocks in the receive call; `close()` aborts both by closing the handles), `net::MultiplayerClient` (the protocol state machine, below) and the records shared with the field (`presence.h`). The client takes its time only from the `now` passed to `update()`, so `test_multiplayer_client` drives it on a fake transport and clock. `App::multiplayer_frame` runs after `simulate()` every frame: with `[Multiplayer] Enabled` on, an address set and the run allowed (not `--config`, `OC_QUIET` or `[Debug] Script`), it makes the client on first use (and a `PlayerId` if there is none), starts it with the name (`Name`, else the Steam persona name, else `Player`) and `game::session_star`, gives it the local player and calls `update()`; then it hands the field the remote parties (without muted ones when `MuteHidesParty`) and the draw options (`FieldWorld::set_remote_parties`). The local player is `FieldWorld::local_presence()` with the room blanked on the title, during a movie or outside a loaded room, and the state raised to Battle during a field battle and to Menu under the main menu, a shop, the pause screen, the key-item list, the party and monster screens, a flow screen or the settings window. With multiplayer off the client is never made and the field is never called. `multiplayer_input` (before the hotkeys) opens the chat on `platform::kChatKey` (T; Enter is the first key set's Ok) and, while the chat is typed in, takes every key from the game (Esc closes it); while a settings text field has the keyboard, every key but Esc. `multiplayer_overlay` (inside the ImGui frame) draws the labels of `FieldWorld::remote_labels()` on ImGui's background list when the field shows them (not under a battle, a menu or a movie) and the chat panel (`render::MultiplayerUi`), then the player menu a click on a label or a chat name opens, the challenge popups (Y / N in `multiplayer_input` answer an incoming one) and the versus screen; `multiplayer_frame` takes the client's `pvp_start` (`App::begin_pvp`: this game's side, `encode_versus_side` of the party as it stands, goes out as `pvp_side` and is kept in `mp_own_side`) and shows the versus screen until the other side has come; then `App::start_pvp_battle` starts the versus battle (section 5.6) from both exchanged sides with a `game::NetworkVersusLink`. The client keeps a battle over a dropped connection and resumes it (`formats/multiplayer.md` section 8.4). The party card a challenge carries is `game::party_card` (`game_session.*`) on the loaded game's party, through the card source `init_multiplayer` gives the UI. Player text is drawn with `ImGuiLayer::chat_font()`: Segoe UI with Latin, Greek and Cyrillic, Meiryo / Yu Gothic / MS Gothic merged for ImGui's Japanese ranges, Segoe UI Symbol for arrows and symbols, rasterised at 24 px into the shared atlas the first time multiplayer is on (the atlas and its texture are rebuilt before that frame); a missing font is left out, and characters outside those ranges draw as `?`.
- `keymap.*`: RDE's two key sets per action (`platform::Action`; `action_down`, `action_pressed`). `paths.*`: `exe_dir`, `find_game_dir`, `find_save_dir`. `rumble.*`: the pad motor model. `updater.*`: self-update, below.
- **Quit and Restart** (General tab). Quit leaves the main loop. Restart is `App::restart_game`, at the top of the next `advance`: it stops the movie, battle, results, menus, shop and pause screen, stops the music, and `GameFlow::restart` clears the flow's overlays and enters the title with the boot logo, in the same process. The title then resets the field as at boot. The battle counter stays until a save is loaded, as it does for a new game.
- **Updater** (`updater.*`, `render/imgui_update_tab.*`). `Updater::check()` asks the GitHub API for the latest release of `sp-hy/OpenCross` (hard-coded in `updater.cpp`; drafts and pre-releases are never "latest") and compares its tag with `OC_VERSION_STRING`. `pick_asset` takes `opencross.exe`, else a `.zip` whose name starts with `opencross`, else a single `.exe` or `.zip`. `download()` fetches it into memory and applies: the asset size, its SHA-256 when GitHub lists a digest, `opencross.exe` at the root or one folder deep when it is a zip, a PE header check (`MZ` and `PE\0\0`), then writes `<exe>.new` and requires its version resource to be newer than the running build. The swap renames the running exe to `<exe>.old` and `<exe>.new` to `<exe>` (a running exe can be renamed, not overwritten; a failed second rename restores the first). The Update tab's "Restart now" ends the main loop and `Updater::restart()` starts the new exe with the same command line. `init_config` calls `cleanup_after_update()` at the next boot: it deletes a stray `<exe>.new` and, on a detached thread that retries for about ten seconds, the `<exe>.old`. All network work runs on one worker thread and the tab polls `status()`; `update_pending` (a newer exe found, downloading or installed, not `launcher_only`) highlights the tab label. `init_render` starts `check()` at every launch, except for `--config`, `OC_QUIET` and `[Debug] Script` runs; `run()` waits for it to finish and, when a newer exe is available, shows `Update: OpenCross <tag> is available (Esc > Update)` as a 4-second toast. A failed launch check only logs. The tab also links to the Nexus Mods page and its bug list. `pick_launcher_proxy` finds the asset `XINPUT9_1_0.dll` (`docs/formats/launcher.md`). When RDE's launcher is beside the exe and the proxy there is missing or differs from the asset's digest, `check()` includes it, and offers it alone (`Status::launcher_only`) when the exe is current; that is how an install from a bare `opencross.exe`, or from an updater older than the proxy, gets it. It uses the exe's rename swap, since a launcher running behind the game has it loaded. Once the exe is in place a proxy failure only logs, and the next check offers it again.
**Files OpenCross writes.**

| File | When |
|---|---|
| `opencross.ini` (next to the exe, or the `--config` file) | first launch writes defaults; every settings change, hotkey toggle and window move saves it |
| `opencross.log` next to the exe (with `--config`, `<config name>.log` beside that ini) | always for `--config` runs and Debug builds; otherwise only with `[Debug] Log` (Debug tab checkbox, default off) |
| `<save dir>\save.dat` | created on first boot with two formatted cards; rewritten by saves and the auto-save (temp file, then rename) |
| `save.dat.ocbak` | one-time copy of `save.dat`, made by `init_menus` when absent |
| `<save dir>\OpenCross\auto.ocsav` and `001.ocsav` to `100.ocsav` | the OpenCross save page (`<name>_OpenCross` for another `[Paths] SaveFile`) |
| `<GameDir>\dump\textures\...` | texture dumps (Mods tab, `occ dump-textures`) |
| `<GameDir>\mods\` | created empty at boot if missing |
| `shots\` beside the route ini | route screenshots and `route_failed.png` |
| `menu_shots\`, `shop_shots\` next to the exe | `ocmenudemo` / `ocshopdemo` screenshot sequences (`--shotdir` or the tools' arguments override); `ocbattleview --shotdir` writes `shot_NNNNNN.png` |
| the file named by `[Debug] Record` | input record (`OCREPLAY 1` header, one line per frame) for `[Debug] Replay` |
| `<exe>.new`, `<exe>.old`, `XINPUT9_1_0.dll.new`, `XINPUT9_1_0.dll.old` | transient files of the updater |
| `XINPUT9_1_0.dll` next to the exe | the launcher proxy, installed by the updater when `CHRONOCROSS_LAUNCHER.exe` is there (`docs/formats/launcher.md`) |

- `src/core/`: `log` (stderr plus optional file), `ScopedTimer`, `span_reader.h`, `parallel.h`, `types.h`, `version.h.in`, `features` (the engine features mods turn on: the build's table, the `mod.ini` parser and `resolve`, `docs/formats/mods.md` section 8).

## 11. Testing and tooling

### 11.1 Unit tests

`tests/CMakeLists.txt` builds one executable per `test_*.cpp`, registered with `add_test` (plain executables that return non-zero on failure; no framework). Run with `ctest`. They cover the importers, the field VM (`test_script`, with a null host), walk mesh, battle rules and animation, effects, audio, movie, menus, shops, flow, save, config, updater, cheats, Steam, the multiplayer JSON and client (`test_net_json`, `test_multiplayer_client`, on a fake transport and clock), the versus battle link (`test_versus_link`: the client's battle messages and two games through a scripted relay) and the route parser (`test_route`). The link lines show the module split at work: `test_battle` links only `oc_core oc_battle`, `test_script` only `oc_core oc_script`.

### 11.2 Deterministic routes

`tests/routes/` holds about 129 routes: `<name>.ini` plus `<name>.txt`. `tests/routes/run.ps1 <name>` starts `opencross.exe --config <ini>` with `OC_QUIET=1` (no focus, no sound), waits, prints the route lines of the log and returns the exit code. An optional `<name>.env` sets environment variables for one run. The ini sets `[Debug] Room`, `Seed`, `Script`, `ScriptSpeed`.

The script language is specified in `docs/formats/routes.md` (and in the header comment of `src/game/route_script.h`): lines of `<when> <command>` with input (`key`, `tap`, `pad`, `walk`, `skip`, `mash`, `choose`, `fight`), waits (`wait room|dialogue|battle|idle|flow|movie|menu|shop|text`), debug warps and writes (`load`, `set var`, `put`, `request`), screenshots and `assert` checks (room, variable, dialogue text, battle events and result, party, position). A failed check logs `route FAILED`, writes `route_failed.png` and sets exit code 1. A route is deterministic because `App::route_frame` feeds a synthetic `platform::Input` one tick at a time and the RNG is seeded. Examples: `alu_chair` (walk-mesh blocking), `art_swap` (live art switch), `battle_hud`. Battle routes are only as deterministic as the battle's seed: a battle started by a field `A3` / `C9` (or a route `request battle`) with `[Debug] Seed` set gets a seed derived from it (`make_field_battle_args` passes it through `OC_BATTLE_SEED`), and a standalone battle (`[Debug] Battle=<scene>`: `battle_chain`, `battle_dash`, `battle_dash2`, `battle_defend`, `battle_run`, `results_heal`, `smoke_battle`) takes `[Debug] Seed` itself (`App::init_battle`) unless the run's environment already sets `OC_BATTLE_SEED`. Which of the others still vary with timing cannot be told from the route files (unverified).

Other automation: `[Debug] Record` / `Replay` (`InputRecorder`, `InputReplayer`); `[Debug] SweepTicks` makes `sweep_frame` visit every room, run its scripts, auto-win battles, skip movies and menus, and log VM statistics.

### 11.3 Tools

Subcommands, flags, environment variables and helper scripts are catalogued in `docs/tools.md`; this section is the overview.

`tool_main.cpp` dispatches `opencross <tool> [args]` (the `oc` prefix is optional; `opencross tools` lists them).

- `occ` (`tools/occ*.cpp`): rooms, models, scripts, effects, saves: list, extract, decode, validate; `battlefields`; texture dump.
- `octext`: message viewer for `lang.dat` and PS1 dialogue. `ocaudio`: decode and play BGM. `ochd`: validate `hd.dat` TIMs. `ocmovie`: play, dump, inspect FMVs.
- `ocprd`: room `.prd` effects: info, validate, soak, sprite PNGs, preview. `ocstage`: battle stage inspect, export, validate.
- `ocbattle` (`tools/ocbattle*.cpp`): battle tables, headless simulator (`sim`, always the auto policy), formulas, soaks (`pac-soak`), the effect-pack audit (`fx-audit`, `tools/ocbattle_fx.cpp`; `fx-reg` in `ocbattle_reg.cpp` lists the effect-resource entries a battle start has) and the versus checks (`versus` headless in one process, `versus-net` over a running multiplayer server). Game folder: `-g <dir>`, `OPENCROSS_GAMEDIR`, or `platform::find_game_dir`.
- `ocbattleview`, `ocbattleseq` (`src/game/battle/demo_main.cpp`, `seq_tool.cpp`): one battle in a window; animation-sequence dump.
- `ocflow`, `ocmenudemo`, `ocshopdemo` (`src/game/flow_demo_main.cpp`, `src/ui/menu/demo_main.cpp`, `src/ui/shop/demo_main.cpp`): title screens, main menu and shops in a window.

`tools/` also holds PowerShell and Python helpers that are not part of the engine: `tools/py/cc_*` (importers and validators that mirror the formats in `docs/formats`), `tools/py/ps1*` and `tools/duckstation.ps1` (PS1 reference captures), `tools/rde_*.py` and `tools/py/rde_*.py` (RDE window captures), `tools/ours_cast*.ps1` and the `fx_`, `sfx_`, `cmp_` and `trans_` comparison scripts. Their use is described in `docs/README.md` and `docs/validation.md`, not here.

## 12. Walkthroughs

### 12.1 How a frame flows

```
App::run iteration
  window.pump(input)
  log_frametime, frame_hotkeys, frame_settings, debug_tools_frame
  frame_dt = timer.tick()
  step = stepper.advance(frame_dt)    -> {ticks, alpha}
  loop_speed.field = field_stepper.advance(frame_dt * field factor)   (fast / slow mode, section 2.4)
  simulate()
    route_frame | replay_frame | tick_boot_logo | advance | sweep_frame | field_frame
    advance(in, dt, ticks)
      fades, pause_modal, battle.update, flow.update, menu.update, ...
      world.update(in, ticks)
        input sample, window input
        ticks x sim_tick():  vm_.tick() -> FieldHost calls -> step_* -> step_camera -> effects
      autosave, battle request checks
  ctx.begin_frame(frame)
  draw_frame(frame)
    crt begin, apply_frame_options
    movie | boot logo | flow screen | draw_scene
      scene target, jitter, title sea
      draw_background, draw_actors, draw_effects, draw_background_late
      draw_battle_picture, composite_scene
      draw_battle_ui | draw_field_ui | draw_title_menu
    veils, crt end, feature icons, pause screen
  ui.new_frame; ui.options_window; apply_options_change; ui.render
  ctx.end_frame
  limiter.wait(frame_cap)
```

### 12.2 How a room loads

Triggers: a map-jump op (`request_map_jump`, `begin_jump`, `step_transition` once dark), `App::apply_flow_action`, the Debug tab, a route `load`, the game-over path. All end in `FieldWorld::load_room(name, param)` (`field_world.cpp`):

1. `files_->set_hd_mode(hd_)`, `clear_room()` (waits for GPU idle).
2. `map_id_for(name)` and `discs_.record(...)` find the room's `.prd` (some rooms use another room's).
3. `view_.load(name)`: reads `cdrom/map/mapbin/<name>.bin` and `.ctd` through the VFS, `import::load_room` builds the `RoomScene`, HD rooms read the `.chip2` alongside, layers upload, walk mesh and camera are built. The CPU half (file reads, import, 1x and 4x composition, and the room's own models with their texture mods resolved and uploads prepared; with a GPU context also the 4x layer textures and the model textures, made with `Texture::create_blocking` on the worker: `RoomView::prepare`, `game/room_models.h`, `render::prepare_model_textures`) can run ahead on a worker: `begin_jump` starts it for the target room when the exit fade starts, and the host starts it for the saved room during the load fade-out (`FieldWorld::prefetch_room`). `load` takes that result when the room, `.prd`, art mode and VFS generation still match (else it waits for the worker, discards it and loads normally), so only the GPU uploads remain at the black screen; `FieldWorld::room_model` uploads a prepared model on first use (`take_prepared_models`), unless the texture mods changed since. During the load fade-out the host also loads the save's party field models on workers, textures included (`FieldWorld::prefetch_party_models`), and every `load_room` starts the same for the current party before it imports the room. A room loaded without a prefetch still builds its own models in parallel inside the load, not one by one as INIT asks for them. A room on the other disc, and 4x images above 512 MB, are not prepared ahead. An art-mode switch drops a running prefetch before the VFS changes.
4. `FieldScriptModule::parse(scene->script_bytes)` (the script as `import::load_room` left it, with RDE's per-room corrections applied: `import::apply_script_fixes`, `fieldscript.md` section 13); `sfx_->leave_room` and `set_room(yos0, yos1)` for the room's sound banks.
5. `room_id_`, `map_id_`, `defeated_enter_room()`, `ents_.resize(entity_count)`; the room variables `kRoomParam`, `kMapId`, 0x3C (can-save, cleared), 0x3A (entrance), 0x50 (world name, cleared), 0x34 (set to 1).
6. `sync_party()`, `build_effects()`.
7. `vm_.load(module, attr_flags)` with `in_init_ = true`: clears room-local variables and runs every entity's INIT. INIT calls `setup_pc`, `setup_npc`, `load_model`, `set_position_xz`, ... which bind actors and models.
8. Party spawns are placed, leader first (`place_party_spawn`, `settle_spawned_followers`); then `update_input_lock`, `snap_camera_to_player`, `pick_auto_target`.
9. `step_transition` sets the fade-in (black over 0x30 ticks, white over 0x20, by kind). The first `sim_tick()` runs the UPDATE scripts.

What a load resets: every entry runs `clear_room` first, whatever started it (a map jump, a Load from the main menu, Continue, New Game, the title scene's rooms, a debug or route warp, the game-over room), as the original's room init does on every field entry. Besides the room's entities, windows, effects, room sounds and camera it drops every request the room before left pending: the map jump and the movie-end jump, a running exit (the transition state goes back to none, so a load during an exit fade does not leave the new room in the exit state with the controls, the pause screen and every gated op held), the exit's frame capture, a script screen still requested or open (a Load picked from a save point's main menu enters the loaded room with no menu state, so the menu key, the pause screen and the next save point, shop or name entry work), the battle hand-off and the input lock; the entry fade then starts from black. A load keeps the game state (after a Load or Continue `restore_vars` has already copied the save's variables 0..0x3FF into the VM), the disc and the music. A room change checks that its target is a field room before anything is cleared and otherwise refuses the exit (`finish_jump_failed`, the room stays); if the target's files or script then fail to load, the room before is already gone, so the world stops ticking and reports it once (`take_room_lost`) and the App returns to the title.

### 12.3 How a battle starts and ends

Start:

1. A room script reaches `A3` or `C9` (`field_vm_ops_game.cpp`). The op waits for the system gate (`host_.system_ready()`, which a pending battle holds, so a second entity's `A3` waits for its own fight), asks `host_.skip_battle` (No Encounters cheat), then calls `host_.request_battle(id, field, flags)`, steps on and yields; the `FE 10` that follows every `A3` / `C9` yields while `host_.battle_active()`.
2. `FieldWorld::request_battle`: unless flag 1 keeps the field music, saves the song and position, stops the mixer and starts the battle song (`audio::battle_song`: the boss theme with flag 0x40, else the battle theme); sets `BattleState::Intro`; starts the `enct` sequence. The field keeps ticking under the swirl.
3. `sim_tick` follows the encounter timeline (`kEncounterTearDown`, `kEncounterMain3E`, `kEncounterMainRamp`). When `enct` ends: `Hold`; after `kEncounterHoldTicks`: `Ready`.
4. Meanwhile `App::advance` sees `world.battle_requested()` and, from the fourth frame, calls `field_battle_load_step()`: `make_field_battle_args`, `BattleView::begin_load`, `load_step(3.0)`, `prepare_fx_targets`.
5. When `world.battle_ready()` and the view's load is complete, `start_field_battle()`: `world.take_battle()` (state `Running`, encounter effect torn down), `BattleView::start` (`finish_start` builds `battle::Battle` with a `ScriptedBrain` and calls `Battle::start`), `BattleView::begin_transition`; `battle_in_field = true`.

Play:

6. Each frame `advance()` calls `battle.update(dt, in)`. The view plays the intro camera and spawn scripts, then the command menu. A command goes through `execute` to `Battle::apply`, and the events replay in `tick()` / `tick_action()` (section 5.3).

End:

7. The rules set the outcome; `begin_finish` / `tick_finish` sets `finished()`.
8. `advance()` opens `BattleResults` (`results.open`) and updates it until `results.done()`.
9. `finish_field_battle()`: code from `battle.outcome()`; `game::apply_battle_result` writes rewards, growth and HP into the game state; the Dopplegang mask and the saved variables go back to the world; the results screen's healed HP is copied; `world.end_battle(code)`.
10. `FieldWorld::end_battle` writes `BATTLE_RESULT` (var 0x32), restores the saved song (where the encounter cut it: the remastered stream by position, the original music by the music push, `audio.md` section 2.5), or stops the battle's song when the field had none, and starts a 0x30-tick fade-in (8 ticks later after a results screen) (unless flag 0x20). `battle_active()` is now false, so the blocked script resumes and reads the result; the fade does not hold the system gate (`FieldWorld::system_ready`: only INIT, a room transition, a pending jump or a battle from its request to its end do), so its PLAY_SONG and the ops after it run under the fade-in, and another entity's waiting `A3` / `C9` requests its battle in the same tick. It also opens the room-entry window (`entry_window_`, 4 ticks) as a room load does, since the field restarts after a battle: nothing is culled, the models take their facing at once (`step_turn_rates`), and `3C` / `A9` wait on the first 3 ticks (`FieldWorld::entry_window`).
11. After a wipe in a fight that is not losable, `finish_field_battle` loads map id 9 (`room_stem_for_id(9)`); the title is the fallback if that load fails. That room's script shows GAME OVER and jumps to the title room with ROOM_PARAM 0, whose INIT resets the game (FE 2F); the flow then returns to its title (section 7, *Game flow*).

## 13. Deliberate differences from the original

- The General tab's Restart returns to the boot logo without leaving the process; RDE has no such control.
- Esc opens and closes the settings window on the press; RDE's Esc menu toggles on the key's release. The pad's Start held 500 ms also opens it, and a shorter Start tap opens the pause screen on its release; on RDE, Start goes straight to the game and the pause opens on the press. The LT + L3 chord behaves as RDE's.
- The achievement room rules sample every 60 real ticks in every speed mode. RDE's sampler counts down by a step that depends on the speed mode (4 times as fast in fast mode, half as fast in slow mode).
- A session that used the debug tools earns no achievements. RDE has no debug tools.
- OpenCross shows its own notice for each achievement, since a directly started exe has no Steam overlay.
- The 8F screen ripple warps the whole picture; the original leaves the top 32 rows as the unwarped copy.
- Widescreen draws a room's art out to the real extent of its tiles, not to the camera block's pan limits.
- Sound effects keep running under the pause screen so its own sounds play; music, voice and movie audio are held.
- Start over a movie opens the pause screen; the original never pauses a movie, and RDE's Start skips a skippable one.
- The debug free camera bypasses the original's near-drop rules (the GPU's near-plane clip applies instead); a widescreen or wide-FOV picture also clips instead of dropping big stage packets.
- A follower's "too close to the leader" step that would leave the walk mesh or change floor is replaced by a probed slide step, or stands still when blocked; the original writes the position without a mesh test.
- Start-pause is allowed at any point of a battle, including the effects during which the PS1 and RDE refuse it (the Time Devourer ending, the Bunyip transformation, the final-boss stages, bgchng / miyademo and battle song changes).
- Title Continue is greyed (sfx 0x18) when there is no save; the original always opens the save screen.
- First launch starts windowed at 1280x720 with widescreen on, MSAA off and no FXAA; RDE starts in exclusive fullscreen at desktop size, 4:3, MSAA 8x and FXAA on.
- Every movie can be skipped, including every 7E and the D4 movies whose second operand lacks bit 0x200 (RDE plays those to the end). The pad triggers (L2 / R2) and stick clicks (L3 / R3) stay hotkeys and do not skip; in RDE every pad button skips (`movies.md` section 8).
- Movie sound follows the row volume on the movie bus only; RDE also applies the Music slider and a factor of 0.6.
- No key rebinding: the keyboard sets are fixed (RDE's Esc menu offers two rebindable keyboard sets). RDE's Brightness, Screen Type, FXAA and Help options and its Quit confirmation are not offered.
- Effect sprites (field, battle and title, every art mode) lose half a 5-bit colour level per channel, the average the PS1 GPU loses by rounding texel x colour down to 5 bits, taken off smoothly without the PS1's steps (`room_effects.md` section 4.3). RDE modulates at full precision with no loss, which is not a choice of its art but the absence of the PS1's rounding: the effects were tuned on the PS1, so its faint additive glows come out about twice as bright and pulse about twice as hard when one object ends and the next grows in (`jac_b05f`'s light patches). This departs from the rule that RDE is the reference where the PS1 and RDE differ; keep it as the default, and do not reintroduce the exact 5-bit rounding, which bands soft glows at modern resolutions (`jac_b08i`'s lamps). `[Enhancements] FullPrecisionEffects` (off by default) restores RDE's full-precision drawing (`render::ParticleBatch::set_full_precision`, set in `App::apply_frame_options`).
