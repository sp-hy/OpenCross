# Developer tools, scripts and environment variables

Reference for everything a developer runs besides the game itself: the tools linked into `opencross.exe`, the validation gates, every `OC_*` environment variable, the hotkeys, and the helper scripts. The workflow that uses them is in [`README.md`](README.md) (section 3) and [`validation.md`](validation.md) (section 3).

## 1. Dev tools linked into `opencross.exe`

`opencross <tool> [args]` runs a tool instead of the game; `opencross tools` lists them. The name is matched exactly or without its `oc` prefix (`opencross ocbattle ...` and `opencross battle ...` are the same; `occ` is also reachable as `c`). An argument that names no tool starts the game.

The exe is a GUI-subsystem program. A tool attaches to the calling console only when its stdout is not already redirected, so from PowerShell pipe the output to make the shell wait: `opencross occ rooms-check | Out-Host`.

Common conventions:

| Item | Meaning |
|---|---|
| `-g <dir>` | Game folder. Must come first for `occ`, `ocaudio`, `ocprd`, `ocbattleseq`; anywhere for `octext`, `ochd`, `ocstage`, `ocbattle`; before the subcommand for `ocmovie`. |
| `OPENCROSS_GAMEDIR` | Game folder when `-g` is absent. Most tools then fall back to the usual discovery (`platform::find_game_dir`); `occ`, `ocaudio`, `ocprd` fail without either. |
| `-l <lang>` | `ocmovie` only: language of the movie (default `en`). |
| `-v` | Verbose: `ochd`, `ocstage validate`, `ocbattle soak`, `pac-soak`, `fx-audit`, `occ dialogue-*`. |
| `ocbattleview`, `ocflow`, `ocmenudemo`, `ocshopdemo` | Take no `-g`; they read `opencross.ini` next to the exe (`[Paths] GameDir`, else discovery). |
| `ocmodelview` | `-g`, else `OPENCROSS_GAMEDIR`, else `[Paths] GameDir` of `opencross.ini` next to the exe, else discovery. |

`ocbattle soak` exits 1 on anomalies or exceptions, `pac-soak` on exceptions, `ochd` on decode or size failures, `ocstage validate` when a stage fails; `ocbattle fx-audit` exits 0 after a completed run.

### 1.1 `occ` (assets: rooms, models, scripts, saves)

Source: `tools/occ.cpp`, `tools/occ_save.cpp`, `tools/occ_dump_textures.cpp`. `save-*` commands need no game folder.

| Subcommand | Arguments | Meaning |
|---|---|---|
| `list` | `<archive>` | Lists the members of a `data/<archive>.dat` archive (or a VFS folder). |
| `cat` | `<path> <out>` | Extracts one member, e.g. `cdrom/map/mapbin/alu_a01f.bin`. |
| `info` | `<path>` | Identifies the container type and prints its subfile table. |
| `lzss` | `<path> <out>` | Decodes an LZSS member. |
| `dump-textures` | `[stages\|title\|effects\|rooms\|models\|weapons\|all] [--raw] <outdir>` | Headless texture dump (the Mods tab's "Dump all textures") into `<outdir>`; no category = all; `--raw` adds raw atlases under `raw/`; prints per-phase counts. |
| `room` | `<name> <outdir>` | Decodes a room, writes one PNG per layer and group, prints camera and walk-mesh statistics. |
| `rooms-validate` | none | Validates every room that has a `.bin` and a `.ctd`. |
| `rooms-check` | none | Initial-frame viewport coverage of every room; flags rooms under 90 %. |
| `rooms-frame` | none | Empty bottom rows per display height (216 / 224 / 240) over every reachable scroll; checks the room bounding-box rule. |
| `chip2` | `<room> <outdir>` | Writes the RDE HD (4x) group images of a room. |
| `chip2-validate` | none | Parses every HD chip2 and reports tiles without an HD tile. |
| `model` | `<path-stem> <outdir>` | Imports a model (e.g. `battle/player/selju/selju`): OBJ, skeleton / animation / mesh JSON, PNGs. |
| `models-validate` | none | Validates every model class (players, monsters, field, at0, weapons, kmd, ex). |
| `script-disasm` | `<room> [entity]` | Field script listing of a room, optionally one entity. |
| `script-soak` | none | Runs every room's script (INIT plus 600 ticks, null host) and reports problems. |
| `dialogue-choices` | `[room] [-v]` | Every multi-choice box of every room and language: headless layout, cursor stepped through the choices, classes and counts. |
| `dialogue-sizes` | `[room] [-v]` | Messages whose window height changes under the no-3-line-minimum rule (count and examples). |
| `room-songs` | `[room]` | Every PLAY_SONG of the room scripts with its song id and BGM track (`$` = song from a variable). |
| `room-models` | `[room]` | Imports every room model (subfile 11+) of all rooms or one room and reports failures. |
| `battlefields` | `[--inc <file>]` | Which rooms fight on which stage (every battle op of every room script to `bg_NN`); `--inc` writes the per-stage room lists as a C++ initializer. |
| `world-markers` | `[--inc <file>]` | Overworld location markers: label text (EN) to target room, and the room-prefix to place-name table; `--inc` writes it as a C++ initializer. |
| `save-info` | `<save.dat>` | Container members, cards, slot count. |
| `save-slots` | `<save.dat>` | Per-slot preview fields. |
| `save-roundtrip` | `<save.dat> <out.dat>` | Decodes and re-encodes every block (bit-exact check), rewrites and reopens the file. |
| `save-blank` | `<save.dat> <out.dat>` | Copy with card 1 emptied (scratch file for the `save` route). |

Environment: `OCC_WALK`, `OCC_JOINTS` (section 3.5). Writes only to the given `<out>` / `<outdir>` / `--inc` paths. The `usage()` text lists only 10 of these subcommands.

### 1.2 `octext` (message viewer)

Source: `tools/octext.cpp`. Language arguments are the `--lang` codes (`en fr it de es jp`); default `en`.

| Subcommand | Arguments | Meaning |
|---|---|---|
| `msg` | `<room> <idx> [lang]` | One message in every form: PS1 stream as RDE markup, `lang.dat` row, rich-text tokens, flattened tokens. `<room>` is a map file stem or a numeric `lang.dat` id. |
| `find` | `<text> [lang]` | Every room message whose `lang.dat` row contains the text. |
| `coverage` | none | Per language: message records of every room versus `lang.dat` rows (`{ERROR}` placeholders). |

### 1.3 `ocaudio` (BGM and SFX)

Source: `tools/ocaudio.cpp`. `<song>` is a song id or `BGMnnn`.

| Subcommand | Arguments | Meaning |
|---|---|---|
| `decode` | `<song> <out.wav> [seconds]` | Remastered track to WAV; no seconds = whole track, no looping. |
| `play` | `<song> [seconds]` | Plays the remastered track (default 10 s). |
| `render-seq` | `<music NNN> <out.wav> [seconds] [rate]` | Original AKAO music through the engine's sequencer (default 20 s, 44100 Hz). |
| `play-seq` | `<music NNN> [seconds]` | Plays the same live (default 20 s). |
| `render-sfx` | `<sfx id> <out.wav> [seconds] [rate] [room]` | Renders one sound effect (default 3 s, 44100 Hz = SPU rate, 48000 = mixer). `room` selects the room bank overlaying instruments 208..223; `battle:<room>` = that room re-entered after a battle. |
| `play-sfx` | `<sfx id> [seconds]` | Plays one sound effect (default 3 s). |
| `sfx-pitch` | none | Pitch-register audit of every SFX id against the retail formula; lists mismatches. |
| `sfx-bytes` | `<id>` | Raw program bytes of a global SFX id (both channels) and its chain count. |
| `sfx-ops` | none | Per SFX id the sequencer opcodes the engine ignores. |
| `room-sfx` | `<room>` | Pitch trace of the room's footstep frames with the room bank's instruments. |
| `banks` | none | First index, count and SPU address of every instrument bank file; music file to bank. |
| `timing` | none | Per song: HCA intro / loop against the AKAO sequence's measured intro / loop (seconds). |
| `switch-test` | `<song> <seconds> [volume]` | Headless mixer run of the live BGM source switch (remastered, original, remastered); volume default 100. |
| `inst-dump` | `<index>` | Instrument table entry of the first bank plus ADPCM block flags and first decoded samples. |

Environment: `OC_SFX_ONLY` (`render-sfx`). The `usage()` text lists only `decode` and `play`. Writes only to the given WAV paths.

### 1.4 `ochd` (HD texture validator)

Source: `tools/ochd.cpp`. Decodes every `hd/**/*.tim` and, where a 1x counterpart exists, checks the HD image is exactly 4x its size. Exit 0 = no decode failures and no size mismatches.

| Form | Meaning |
|---|---|
| `ochd [-v]` | Validate; `-v` also lists members without a 1x counterpart. |
| `ochd -m <name> <outdir>` | Debug export of field model `fieldobj/player/<name>` from the HD-mode override set with its `hd.dat` texture: `<outdir>/<name>.obj` plus a size summary. |

### 1.5 `ocprd` (room and bespoke effects)

Source: `tools/ocprd.cpp`. `fx-soak` and `fx-preview` need no game data.

| Subcommand | Arguments | Meaning |
|---|---|---|
| `info` | `<room\|path>` | Sheets (every frame slice), emitters (decoded ops), sequences of a room `.prd`. |
| `validate` | none | Every room `.prd`: totals, stream failures, timinfo / TIM mismatches. |
| `render` | `<room> <outdir>` | One PNG per sprite slice plus the composed frame of every sheet. |
| `soak` | `[ticks]` | Every room emitter and battle-effect emitter through the effect player (default 600 ticks). |
| `preview` | `<room> <outdir>` | The room's emitters at ticks 0 / 30 / 60 / 120 over the layer-0 backdrop (2x PNGs). |
| `fx-soak` | `[ticks]` | Every registered bespoke overlay against a dummy stage (default 600). |
| `fx-preview` | `<overlay> <outdir>` | Software-rendered PNGs of one bespoke overlay at ticks 6 / 30 / 60 / 100. |

Environment: `OCPRD_VERBOSE` (`preview`). The `usage()` text omits `fx-soak` and `fx-preview`.

### 1.6 `ocstage` (battle stages)

Source: `tools/ocstage.cpp`. `<bg_NN>` may also be a path below `cdrom/`, with or without `.prd`.

| Subcommand | Arguments | Meaning |
|---|---|---|
| `info` | `<bg_NN>` | Parts, atlases, cameras, sequences, emitters, lens tables of one stage. |
| `export` | `<bg_NN> <outdir>` | `part_<entry>_<name>.obj`, `stage.mtl`, `tex_m*_c*.png`, `stage.json`. |
| `objects` | `<bg_NN>` | The stage's object scripts as the engine draws them (mesh, sprite type, blend, flags, transform, colour, life). |
| `envpage` | `<bg_NN\|all> [outdir]` | The stock environment page as `<bg>_env.png`; `all` lists every stage's coverage (visible texels, distinct colours). |
| `validate` | none | Every `battle/bg/*.prd`; exit 0 when all pass; `-v` lists the passing ones too. |

Environment: `OCSTAGE_COLORS`, `OCSTAGE_UV` (`info`).

### 1.7 `ocbattle` (battle rules, simulator, soaks)

Source: `tools/ocbattle.cpp`, `tools/ocbattle_fx.cpp`, `tools/ocbattle_reg.cpp`. Global flags: `-g <dir>`, `--rng lo|mid|hi|seed:N` (roll source for formulas with random terms, default `mid`; consumed before the subcommand).

| Subcommand | Arguments and flags | Meaning |
|---|---|---|
| `tables` | none | Counts and a few named rows of the game tables. |
| `scenes` | none | One line per scene: enemy ids, names, counts, battlefield, boss flag. |
| `elements` | none | Per element: id, raw name, colour, level, mode, first record bytes. |
| `sim` | `<scene> [seed] [--party a,b,c] [--star N] [--set char\|char_w0..] [--quiet] [--brain scripted\|random] [--party-from file[:slot]] [--attack] [--field N]` | Plays one battle with the auto policy and prints the log. `--party` default `0,1,2`; `--star` default 0; `--set` default `char`; `--brain` default `scripted`; `--party-from` reads the party of a save slot (default newest); `--attack` = weapon chains only; `--field` = battlefield. |
| `soak` | `[--seeds N] [--scene S \| --from A --to B] [--cap N] [--party-from file[:slot]] [--brain scripted\|random] [-v]` | Random-policy battles over scenes; checks invariants, stalls, unimplemented ops, stub hits. Defaults: 3 seeds, all scenes, cap 2000 commands, `scripted`. A 60 s watchdog reports hangs; exit 1 on anomalies or exceptions. |
| `xcheck` | `[--n N] [--seed S]` | Cross-check of the weapon / element damage code against an independent formula transcription over random triples (default 20 cases per kind, seed 1234). |
| `cast` | `<scene> <actor> <element[,element...]> <target> [seeds]` | Rules-only cast (no view): starts the scene, casts as the Debug tab does, prints the target's status words and the grid cells a seal greys. |
| `debug-kill` | `[enemy_id [count]] [--scene S] [--keep-script]` | Headless "Replace the enemies": per scene replaces the groups, knocks every enemy out and prints the scenes that do not end in victory. |
| `pac-disasm` | `<scene>` | Disassembles the scene's battle event script (`evtNNN.pac`, else the shared template). |
| `pac-soak` | `[turns] [-v] [--scene N \| --from A --to B]` | Runs every scene's scripts in a dummy battle for N commands (default 40). |
| `formula` | `<name> <args...>` | Evaluates one formula; no name lists them: `ceil100 colour hit slot weapon element heal status_timer growth minor recovery stamina_cost sum`. |
| `fx-audit` | `[--csv F] [--md F] [--eid N] [--ticks N] [-v] [--find NAME]` | Plays every battle effect pack headless and reports per-pack status. `--csv` default `battle_effects_audit.csv` in the working folder (`--csv ""` disables); `--md` writes a status table; `--eid` one effect slot; `--ticks` caps the run; `--find NAME` lists packs with an emitter header of that name (`*` = every `at0.prd` emitter name), no audit and no CSV. |
| `fx-reg` | see below | Effect-registry model of a battle start and registry searches. |

`fx-reg` forms:

| Form | Meaning |
|---|---|
| `fx-reg SCENE [--bg N]` | The (type, name) entries the engine has at battle start: `battle.prd` (nested drps included), the stage `bg_NN.prd` (`--bg` overrides), each present unit's model effect DRP (party = Serge only). One line per entry: type, name, count, sources. |
| `fx-reg --index [--all]` | Every (type, name) of every drp the game has, with the file list; `--all` covers every `cdrom` member that parses as a drp. |
| `fx-reg --file PATH` | One drp (path under `cdrom/`): its entries in order (type, name, size), nested drps indented. |
| `fx-reg --refs NAME` | Every drp file / entry whose decoded entries contain the four bytes `NAME`, then the scenes whose enemy models own that `at0.prd`. |
| `fx-reg --grep NAME` | The four bytes `NAME` anywhere in any `cdrom` member (raw, LZSS-inflated, or nested). |
| `fx-reg --s6` | Every battle model with an effect DRP: stem, header names, the enemy ids using it, the scenes. |

Environment: `OC_SOAK_LIST`, `OC_FIND_*`, `OC_FX_MISSING`, `FXA_TRACE`, `FXA_TEX` (section 3.5). The `usage()` text lists only `tables sim soak pac-disasm pac-soak formula`; it omits `scenes elements xcheck cast debug-kill fx-audit fx-reg` and the flags `--cap --attack --field` (and `pac-soak`'s `--scene/--from/--to/-v`).

### 1.8 `ocmovie` (FMV)

Source: `tools/ocmovie.cpp`. `<name>` is a `movie.dat` member without extension (`a01op`, `title`); `-l` picks the language variant.

| Subcommand | Arguments | Meaning |
|---|---|---|
| `play` | `<name> [seconds]` | Window and audio, A/V synced; Esc / Enter / Space skips. |
| `dump` | `<name> <frame> <out.png>` | Headless decode to frame N (0-based), saved as PNG. |
| `info` | `<name>` | Size, frame rate, frame count, duration, audio. |
| `table` | none | The field script's movie operand to `movie.dat` member mapping of both discs. |

### 1.9 `ocbattleview` (one battle in a window)

Source: `src/game/battle/demo_main.cpp`. First non-flag argument = scene (scenedat index, default 0). Reads `opencross.ini` next to the exe, logs to `ocbattleview.log` there. With `--frames` it runs deterministically (one tick per frame), takes no focus and plays no sound. `--shot` writes `battle_shot.png` next to the exe; `--shotdir` shots go to that folder (default the exe folder) as `shot_NNNNNN.png`.

| Flag | Argument | Default | Meaning |
|---|---|---|---|
| `--orig` | none | HD art | Original (PS1) art instead of HD. |
| `--mods` | none | off | Apply the ini's enabled mods in Modded art, texture replacements included. |
| `--field` | `<n>` | the scene's own | Battlefield operand (stage `bg_NN`). |
| `--size` | `<w> <h>` | `1280 960` | Window size. |
| `--sliced` | none | off | Load in 3 ms slices per frame (the field encounter's way); logs the longest slice. |
| `--cam` | none | off | Camera overlay on (F3 / F9 toggle). |
| `--party` | `a[,b[,c]]` | `0,1,22` | Character ids; slot 0 = the acting PC (it makes `--cast` / `--weapon`). The battle seats the party in this order, so `--caster`, `--casttgt`, `--cast2tgt` and `--ko` values 0..2 are slots of this list and units alike. |
| `--ring` | `c,c,c` | the stage's ring | Field ring at the start: colour names (`green white black red yellow blue`), indices 0..5, `all` or `none` per slot. Matches a PS1 capture's ring for enemy-cast damage comparisons. |
| `--stats` | `hp=..,atk=..,def=..,hit=..,evd=..,mag=..,mdef=..` | unchanged | Base stats of every party member. |
| `--enemyhp` | `u3,u4,u5` | unchanged | HP (and max HP) of enemy units 3 / 4 / 5; empty or -1 keeps. |
| `--ko` | `<unit>` | none | That party unit starts KO'd. |
| `--status` | `<unit>:<lo>[,...]` | none | OR these status bits into the units' merged `lo` words at the start (e.g. `0:0x19,3:0x100020`); for checking the status plates and icons. |
| `--equip` | `<item id>` | character table | Party slot 0 holds that weapon (e.g. `0x26`). |
| `--cast` | `<element id>` | none | First PC casts that element on the first enemy once the view ticks. |
| `--caster`, `--casttgt` | `<unit>` | PC 0 / first enemy | With `--cast`: that unit casts on that unit (PCs 0-2, enemies 3+). |
| `--castat` | `<frame>` | immediately | Issue the cast at that frame. |
| `--cast2`, `--cast2at`, `--cast2tgt` | `<element>`, `<frame>`, `<unit>` | none | A second cast (consecutive casts). |
| `--ecast` | `<element id>` | none | First enemy casts it on the first PC. |
| `--weapon` | `<0\|1\|2>` | none | The acting PC attacks the first enemy (weak / medium / heavy). |
| `--nofx` | none | off | Element casts take the generic path (no effect pack). |
| `--auto` | none | off | Feeds a confirm every 12 frames (smoke test). |
| `--autoframes` | `<n>` | unlimited | `--auto` stops pressing after n frames. |
| `--bauto` | none | off | The battle plays itself (auto-battle). |
| `--ui` | `<1\|2\|3>` | none | Opens the element grid / attack chain / element targeting. |
| `--keys` | `"Name:frame,..."` | none | Presses SDL scancode names at frames, e.g. `Down:200,Right:240`. |
| `--trans` | `<frame>` | none | Starts the battle transition at that frame. |
| `--shot` | `<frames>` | none | Writes `battle_shot.png` after N frames and exits. |
| `--shotevery` | `<n>` | 0 | Write a shot every n frames. |
| `--shotfrom`, `--shotto` | `<frame>` | all frames | Only frames in the range write a shot. |
| `--shotdir` | `<dir>` | exe folder | Shot folder. |
| `--frames` | `<n>` | unlimited | Deterministic run; exit after n frames. |

Environment: `OC_LIST_ELEMENTS_EID`, `OC_LIGHTING`, `OC_SMOOTH_REFL` and the battle variables of section 3.3.

### 1.10 `ocbattleseq` (animation sequences, battle sounds)

Source: `src/game/battle/seq_tool.cpp`. Without a subcommand the first argument is a model folder.

| Form | Meaning |
|---|---|
| `<folder> [entry variant [a b hit dead]]` | Dumps what the animation-sequence VM does for a battle model (`player/selju2`, `monster/x`); entry -1 = attack table. |
| `pack <effect .prd> [entry variant ...]` | The same for an effect pack. |
| `emitters <cdrom path>` | The emitter entry names of a drp, nested drps included. |
| `audit` | Every executed op over every attack and effect script of every battle model. |
| `sfx <folder> <tag> [weapon class] [seconds]` | Renders a battle sound offline and prints its level (default 12 s analysis). |
| `sfxscan [weapon class]` | Every battle sound of every unit: stuck voices, parked channels, long sounds (40 s). |
| `sfxslots <stage.prd> <unit folder\|.prd> <weapon class> [pack .prd ...]` | Instrument slots each sound reads and which bank the single table gives them, against a per-file table; paths under `cdrom/battle/`. |
| `sfxbanks <prd>` | Instrument banks of a battle `.prd` (path under `cdrom/battle/`): first index, count, sample bytes. |
| `sfxfind <hexbytes>` | Which battle sound contains a hex byte pattern. |

Environment: `OC_SEQ_*`, `OC_AUDIT_*`, `OC_PACK_*`, `OC_SFX_VOL/PAN/WAV` (sections 3.4 and 3.5). The `usage()` text omits `emitters audit sfx sfxscan sfxslots sfxbanks sfxfind`.

### 1.11 `ocflow`, `ocmenudemo`, `ocshopdemo` (UI screens in a window)

Sources: `src/game/flow_demo_main.cpp`, `src/ui/menu/demo_main.cpp`, `src/ui/shop/demo_main.cpp`. The two save-based demos work on slot 4 of a save copy and write only to a separate save file (menu) or nowhere (shop). Keys are in section 4.4.

| Tool | Flags | Meaning |
|---|---|---|
| `ocflow` | `--shots [dir]`, `--orig`, `--lang <en\|fr\|it\|de\|es\|jp>`, `--save <file>` | Title, name entry and slot list screens without a field. `--shots` writes title / name entry / slot list shots for both art modes to `dir` (default `flow_shots` next to the exe) and exits. Continue reads `--save`, else a `save.dat` in the scratch folder, else the game's own save folder. |
| `ocmenudemo` | `--shots [dir]`, `--orig`, `--lang <code>`, `--slot N`, `--drive` | Main menu on slot N (default 4). `--shots` writes the sequence to `dir` (default `menu_shots` next to the exe) and exits; `--drive` plays a scripted key sequence through every screen and logs the resulting state. |
| `ocshopdemo` | `--shots [dir]`, `--orig`, `--lang <code>` | Element shop, blacksmith, trading post. `--shots` writes to `dir` (default `shop_shots` next to the exe) and exits. |

### 1.12 `ocmodelview` (character models in a window)

Sources: `tools/ocmodelview.cpp`, `tools/model_view.*`; the model files are read by `game::read_model_files` (`src/game/model_files.*`), the same reader the field, battle and texture dump use. Also opened by the Mods tab's **Model Viewer** button and the Debug tab's *Model viewer* row (Tools). Flags: `-g <dir>`, `--model <stem>` (a stem as listed, e.g. `fieldobj/player/selju/selju`; default Serge's field model), `--sd` (start in original art). Bad arguments exit 2.

- **Catalog.** Every field character (`fieldobj/player`), battle player (`battle/player`) and monster (`battle/monster`) `.obj` of the cdrom archive, filtered by a search box and a group.
- **Original / modded.** Two copies of the model: the original reads a VFS without mods and skips texture replacement; the modded copy applies the mods enabled in `opencross.ini` (Modded art set with *HD art*, SD art set without). Tab switches between them without moving the camera or the playback time. *Reload* rescans the mods folder.
- **Animation.** The model's raw clips (`<source>[<index>] <name>`), with play, loop, speed and a frame slider; picking a clip starts it. The viewer's keys (section 4.4) work whenever the search box is not being typed in. The modded copy plays the clip with the same source and index, not the same list position; a missing or ambiguous clip shows the rest pose with a note. No animation-sequence, battle or field scripts run.
- **View.** Orbit camera framed on the rest-pose bounds of both copies; Front / Side / Back / Reset. *Inspection light* (off by default) adds a fixed model-space key light and ambient; off, the model is drawn unlit like the game draws it. *Show initially hidden parts* draws the meshes whose S3 shading entry starts them hidden (the game's scripts show those).
- **Stats.** Vertices, triangles, parts, joints and clips of each copy, and the reader's error when a file is missing or rejected. Models with more joints than the mesh renderer's 64 are drawn as the game draws them, with a note.

It writes nothing: no save, no ini, no mod settings.

### 1.13 `ocsteam` (Steam connection check)

Source: `tools/ocsteam.cpp`. Connects to the running Steam client as the game, waits for the stats, and lists every achievement of the app with its API name, display name and state (`*` = unlocked). Exits 1 when Steam is closed, logged out, the game is not owned, or the stats do not arrive. Read-only: nothing is set or stored, though Steam shows the game as playing while it runs.

### 1.14 Files written by the tools

| Tool | Output |
|---|---|
| `occ dump-textures` | Folder tree under `<outdir>` (the game's `dump\textures` when run from the Mods tab). |
| `occ room`, `chip2`, `model` | PNG / OBJ / JSON under `<outdir>`. |
| `ocprd render`, `preview`, `fx-preview` | PNGs under `<outdir>`. |
| `ocstage export`, `envpage` | OBJ / MTL / PNG / JSON under `<outdir>`. |
| `ocbattle fx-audit` | `battle_effects_audit.csv` in the working folder (or `--csv`), optional `--md`. |
| `ocbattleview` | `ocbattleview.log`, `battle_shot.png`, `shot_NNNNNN.png` next to the exe or in `--shotdir`. |
| `ocflow`, `ocmenudemo`, `ocshopdemo` | `flow_shots/`, `menu_shots/`, `shop_shots/` next to the exe, or the folder given to `--shots`. |
| `ocaudio decode`, `render-seq`, `render-sfx` | The WAV path given. |

## 2. Gates

Battle changes must not regress these two runs.

| Gate | What it checks |
|---|---|
| `ocbattle fx-audit` | Plays every effect pack headless (a fixed scene, caster PC 0, targets enemies 3 and 4) until it ends or hits the tick cap, plus the six cast-charge packs. Per pack: emitters started, objects spawned, quads / mesh triangles / bespoke primitives per tick, sequence ops not presented, sprites and faces with all-transparent texels, non-finite or runaway coordinates, effects that end at once or never. Prints `fx-audit: N packs (C clean, G clean on the generic path)` and the issue-class counts. |
| `ocbattle pac-soak` | Starts every scene's battle with the default party and runs its enemy scripts: a direct pass over every slot 2..4 of every unit (twice), then up to N PC commands. Prints scenes, battles started, commands, exceptions, outcome counts (victory / wiped / fled / other), ops executed, scripted-turn statistics, executed opcodes that no shipped script contains (confidence `U`, should stay empty) and invalid opcodes, unmodelled unit fields. |

"No regression" means: `pac-soak` reports zero exceptions (`0 exceptions`, exit 0) and every number on its output lines (outcome counts, ops, scripted turns, unused / invalid opcode lists) is unchanged against the baseline run, and `fx-audit` keeps the same clean counts and issue-class counts, unless the change is meant to alter battle scripts or effect handling. To diff two builds scene by scene, set `OC_SOAK_LIST=1` (one `scene N outcome K` line per scene) and compare the output files.

`OC_BATTLE_AUDIT` turns on a per-tick checker in the battle view (`src/game/battle/battle_audit.h`, `battle_view_anchor.cpp`, `battle_camera.cpp`); each anomaly logs one `AUDIT ...` warning.

| Value | Checks |
|---|---|
| set to any value (empty included); `2` adds the extra checks | A queued action group that cannot start for 900 consecutive ticks; a UI wait with nothing to wait for for 300 ticks; camera selection without a bank, a missing shot or timeline, invalid anchor units; anchor types that give no frame or reference empty or wrong unit lists; actions refused by the rules (`apply refused`). |
| `2` | Everything in `1`, plus once at start a list of every distinct (target byte, mode) pair of the element table with its first element. |

## 3. Environment variables

Most `static const` reads are cached for the process lifetime, so set the variable before launching. "set" means the variable exists, whatever its value (an empty value counts), unless the Values column says otherwise. Battle source files below are under `src/game/battle/` unless a directory is shown. Logging variables write through the log (debug or info level) unless the Meaning says stderr.

### 3.1 App, routes and paths

| Variable | Read by | Values | Meaning |
|---|---|---|---|
| `OPENCROSS_GAMEDIR` | `tools/tool_util.cpp` (`octext`, `ochd`, `ocmovie`, `ocstage`), `tools/occ.cpp`, `tools/ocprd.cpp`, `tools/ocaudio.cpp`, `tools/ocbattle.cpp`, `src/game/battle/seq_tool.cpp`, `tools/py/cc_save_forge.py`, `tests/test_*.cpp` | folder path | Game folder used by the dev tools, the unit tests and `cc_save_forge.py` when no `-g` / `--game` is given. |
| `OC_QUIET` | `src/platform/window.cpp`, `src/audio/mixer.cpp`, `src/app/app_boot.cpp` (no Steam connection); set by `src/game/battle/demo_main.cpp` (`--frames`), `tests/routes/run.ps1`, `tools/ours_cast34.ps1` | set | Test window is shown without activation and never focusable, and the mixer output gain is 0. |
| `OC_SOURCE_SAVE` | `tests/routes/true_ending.prep.ps1` | file path | The save.dat the route forges its Opassa Beach save from (default `tmp\save.dat`). |
| `OC_FRAMETIME` | `src/app/app_run.cpp` | set; `2` | Logs the wall time of main-loop iterations while a field-to-battle hand-over runs: only those over 20 ms, or every iteration when the value starts with `2`. |
| `OC_NO_WARP` | `src/app/app_draw.cpp` | `1`, `2` | A/B check of the screen ripple: `1` draws its frame unwarped; `2` (classic art) shows only the deep part, unwarped. |
| `OC_SLOT_DUMP` | `src/app/app_route.cpp` | path prefix | A route `request slotdump N` writes the encoded save block to `<prefix>N.bin` (an error is logged when unset). |
| `OC_SWEEP_ROOMS` | `src/app/app_boot.cpp` | `a,b,c` | Room sweep (`[Debug] SweepTicks` > 0) covers only these rooms. |
| `OC_SWEEP_FROM` | `src/app/app_boot.cpp` | room name | Room sweep starts at this room of its list. |
| `OC_SWEEP_COUNT` | `src/app/app_route.cpp` | integer >= 1 | Room sweep stops after this many rooms. |
| `OC_SWEEP_SCENARIO` | `src/app/app_route.cpp` | integer | Story counter (script variable 0) every swept room is entered with. |
| `OC_STUCK_TICKS` | `src/game/field_world.cpp` | integer > 0 | Field watchdog limit in ticks for a stuck script wait or input lock (the sweeps use a short one). |
| `OC_EXIT_KIND` | `src/game/field_world_transition.cpp` | `0`..`6` | Forces the fade kind of every room change. |
| `OC_ATTACH_DRIFT` | `src/game/field_world_tick.cpp` | set | After every field tick, draws each D6-attached model at render fractions 0 and 0.5 and logs `attach drift: room <r> ent <e> on ent <p> point <n>: drawn <d> units off the parent's attach point` when it is more than half a unit away from its parent's attach point (+ the FE 46 offset). Zero lines over a run = the attachments stay rigid. |
| `OC_BUILD_DIR` | `tests/routes/run.ps1`, `tests/routes/run_save.ps1` | build folder | Build tree the route runner uses (default `build\win-x64`). |
| `OC_EXE` | `tools/py/sfx_batch.py` | exe path | `opencross.exe` that `sfx_batch.py` runs (default `build\win-x64\bin\Debug\opencross.exe`). |
| `OC_WC` | `tools/py/sfx_batch.py` | integer | Passed to `ocbattleseq sfx` as the weapon class (default `0`). |
| `OC_PS1_DISC` | `tools/py/ps1ref.py` | disc file or folder | Disc image, or a folder searched for `Disc 1` / `Disc 2` images. |
| `OC_PS1_REGION` | `tools/py/ps1ref.py` | `japan` (default), `usa` | Region of the disc images picked. |
| `CROP` | `tools/py/cmp_frames.py`, `tools/py/trans_measure.py`, `tools/py/trans_sharp.py` | `y0,y1` fractions (`cmp_frames`, default `0.10,0.74`); `x,y,w,h` px (others) | Crop of the compared frames. |
| `RDE_WHITE_ONLY` | `tools/rde_capture.py` | set | Only a frame with mean luma above 200 starts the post-white recording; a sudden luma jump (room fade) does not. |

### 3.2 Field, rendering and UI

| Variable | Read by | Values | Meaning |
|---|---|---|---|
| `OC_NO_CULL` | `src/game/field_world_collide.cpp` | set | A/B check of the off-screen animation freeze: keeps the cull grace on permanently. |
| `OC_FIELD_SHADOWS` | `src/game/field_world_draw.cpp` | `0` | Disables field shadows. |
| `OC_NO_MODEL_FX` | `src/game/field_world_draw.cpp` | set | Field models are drawn without their blend mode and their flash. |
| `OC_FIELD_FX_LOG` | `src/game/field_world_effects.cpp` | set | Logs field effect state once a second and each effect tick. |
| `OC_ROOM_LAYERS` | `src/game/room_view.cpp` | `i,j,k` | Draws only these background layer indices. |
| `OC_TITLE_SEQ` | `src/game/title_backdrop.cpp` | `tit0`..`tit3` | Forces the title backdrop sequence instead of the script's random pick. |
| `OC_TITLE_MODEL_REST` | `src/game/title_backdrop.cpp` | set | Draws the title models in their rest pose. |
| `OC_MESH_CULL` | `src/render/mesh_pipeline.cpp` | `1`, `0`, `-` | Mesh back-face test by the first character: `0` off, `-` inverted, anything else the default test. |
| `OC_LOWRES_LOG` | `src/render/sprite_batch.cpp` | not empty, not `0` | While a tag is set, logs the size of the first draw of each texture once per tag. |
| `OC_TEXT_AUDIT` | `src/render/text_batch.cpp` | not empty, not `0`; `2` | Text layout audit (overlaps, missing glyphs, edge crossings); `2` also lists every string with its ink box and samples a frame every 300 frames. |
| `OC_MENU_ONLY` | `src/ui/menu/menu_demo.cpp` | tag substring | Keeps only the menu-demo steps whose tag contains it. |
| `OC_SHOP_LOG` | `src/ui/shop/shop_screen.cpp` | set | Logs the shop compass selection and angle. |
| `OC_FREECAM_TRACE` | `src/game/debug_tools.cpp` | set | Logs the raw and smoothed frame time of the free camera. |
| `OC_LIGHTING` | `src/game/battle/demo_main.cpp` | set | `ocbattleview`: forces `[Enhancements] Lighting` on (mesh lighting and spell lights). |
| `OC_SMOOTH_REFL` | `src/game/battle/demo_main.cpp` | `1`, other | `ocbattleview`: overrides `[Enhancements] SmoothReflections`; `1` on, any other value off. |

### 3.3 Battle view and effects

| Variable | Read by | Values | Meaning |
|---|---|---|---|
| `OC_BATTLE_AUDIT` | `battle_camera.cpp`, `battle_view_anchor.cpp` | set; `2` | Per-tick battle flow checks, one `AUDIT` warning per anomaly (section 2); `2` adds the element-table pair list. |
| `OC_BATTLE_SEED` | `battle_view.cpp`; set from `[Debug] Seed` by `src/app/app_battle.cpp` (field battles) and `app_boot.cpp` (standalone `[Debug] Battle`) | decimal integer | Fixes the battle RNG seed (and the random camera picks); for a field encounter also its enemy-group variant roll. |
| `OC_BATTLE_MISS` | `src/battle/battle.cpp` | set | Every PC weapon attack misses. |
| `OC_BATTLE_NORUN` | `battle_view_ui.cpp` | set | Auto-battle's last-resort run-away is disabled, so a fight ends in a win or a wipe. |
| `OC_BATTLE_FIELD` | `battle_view.cpp` | stage number | Forces the battle stage (background). |
| `OC_BATTLE_INTRO` | `battle_view.cpp` | sequence name | Forces the stage intro camera sequence (ignored when the bank has no such sequence). |
| `OC_DOPPLE_SLOTS` | `battle_view.cpp` | `all`, `debug`, or `n,n,...` | Dopplegang pool slots: all, the table's non-zero rows, or a list. |
| `OC_CAST_PARTY` | `battle_view_action.cpp` | integer (default 99) | Limits a party-side all-targets cast to the first n PCs. |
| `OC_ROUTE_ELEM` | `battle_view_action.cpp` | element id | Fills every occupied grid cell of PC 0 with that element, downs every enemy but the first, and leaves the target of a confirmed element at 1 HP. |
| `OC_ROUTE_GAUGE` | `battle_view_action.cpp` | set | With `OC_ROUTE_ELEM`: fills PC 0's gauge to the maximum. |
| `OC_GHOST` | `battle_view_action.cpp` | `u:flag,...` | Sets the script ghost word of units before a harness cast (1 off-stage, 0 shown). |
| `OC_DEV_VAR` | `battle_view_action.cpp` | `0xOFF=val,...` | Writes battle-script variables (byte offsets) once after the slot-0 init pass. |
| `OC_DEV_CHIST` | `battle_view_action.cpp` | `e,e,...` | Feeds colour history entries (colour + 1, oldest first) once after the slot-0 init pass, as if those casts had been made: `OC_DEV_CHIST=5,4,1,6,3,2,7` makes the Time Devourer's death (scene 472) take the true ending. |
| `OC_DEV_FORM` | `battle_view_action.cpp` | creature slot, `-1` own form | Form taken by the cast of element 238. |
| `OC_DEV_WTGT` | `battle_view_action.cpp` | unit, or `owedK` | Aims the `--weapon` attack at that unit; `owedK` = the K-th (0-based) enemy the battle scripts have marked as owing a death. |
| `OC_DEV_HP1` | `battle_view_action.cpp` | set | Leaves the `--weapon` target with 1 HP first. |
| `OC_DEV_ONLY` | `battle_view_action.cpp` | set | With `--weapon`: downs every other enemy so the one kill ends the fight. |
| `OC_DEV_TGTMEM` | `battle_view.cpp` | unit | Starts every PC's remembered target at that unit. |
| `OC_ACT0_MODEL` | `battle_view_pseudo.cpp` | model number | Overrides the model of the pseudo-action. |
| `OC_RESULTS_HURT` | `battle_results.cpp` | HP | Caps every living member's HP at this value (at least 1) on the results screen. |
| `OC_RESULTS_HEALER` | `battle_results.cpp` | `id,id,...` | Puts these element ids in the first open cells of every living member. |
| `OC_CAM_PICK` | `battle_camera.cpp`, `battle_view_util.h`, `src/game/effects/sequence_player.cpp` | camera name (e.g. `apr0`, `p01`) | Forces the camera pick; in an effect sequence's random camera pool a comma list is accepted and `name@k` limits the match to the k-th pool of the run. |
| `OC_CAM_KEYS` | `battle_camera.cpp` | set | Logs the keys of each camera shot. |
| `OC_CAM_TRACE` | `battle_camera.cpp` | set | Logs the camera pose every tick. |
| `OC_DRAW_CAM` | `battle_view_camera.cpp` | set | Logs the interpolated camera of each drawn frame. |
| `OC_STAGE_ONLY` | `stage_scene.cpp`, `title_backdrop.cpp` | `a,b` | Keeps only these stage parts (the title backdrop matches by substring). |
| `OC_STAGE_SKIP` | `title_backdrop.cpp` | names | Skips title-backdrop objects whose name occurs in the value. |
| `OC_STAGE_CULL` | `stage_scene.cpp`, `title_backdrop.cpp` | `swap`, `none`, other | Overrides the cull side of every stage object: `swap` inverted, `none` off, any other value forces side 0. |
| `OC_STAGE_REJECT_LOG` | `stage_scene.cpp` | set | Logs the stage sprite counts and the packets rejected per frame. |
| `OC_STAGE_NOSHADOW` | `stage_scene.cpp` | set | Leaves the unit drop shadows out of the stage. |
| `OC_STAGE_NOREJECT` | `src/render/stage_pipeline.cpp` | set | Turns the stage packet reject test off: every packet is drawn, including those the GPU would clip. |
| `OC_STAGE_EXT_QUARTER` | `src/render/stage_pipeline.cpp` | set | A scrolling blend-2 ext object adds a quarter of its colour (blend mode 4) instead of the full colour. |
| `OC_NO_BAKE` | `stage_scene.cpp` | set | Skips the per-object texture bake (raw atlases). |
| `OC_FX_SEED` | `battle_view_fx_cast.cpp` | decimal integer | Fixed effect RNG seed (default derived from the cast count). |
| `OC_FX_DT` | `battle_fx.cpp`, `battle_view_fx_cast.cpp` | `tick:dt,...` (dt 1..10) | Effect pacing table: ticks per pack tick from the given pack tick; also turns the hand-over delay emulation on. |
| `OC_FX_ONLY` | `src/game/effects/emitter_system.cpp` | `name,name` | Draws only these emitters' objects (battle effect player). |
| `OC_FX_HDR` | `src/game/effects/emitter_system.cpp` | name substring | Dumps the op list of matching emitter headers. |
| `OC_FX_START` | `emitter_api.cpp`, `emitter_spawn.cpp`, `emitter_system.cpp` (`src/game/effects/`) | set | Writes emitter starts, header words and periodic children to stderr. |
| `OC_FX_SEQ` | `src/game/effects/sequence_player.cpp` | set | Writes one stderr line per executed effect sequence command and per camera pool. |
| `OC_FX_OBJECTS` | `battle_view_fx_draw.cpp` | N | Every N video frames logs one line per live emitter object. |
| `OC_FX_UNITS` | `battle_view_fx_draw.cpp` | N | Every N video frames logs one line per present unit and the effect camera. |
| `OC_FX_TRACE` | `battle_view_fx_draw.cpp` | set | Every 10 ticks logs the effect camera. |
| `OC_FX_MESH_TRACE` | `battle_view_fx_draw.cpp` | set | Logs every effect mesh drawn. |
| `OC_FX_MESH_CULL` | `battle_view_fx_draw.cpp` | integer >= 0 | Forces the cull mode of effect meshes. |
| `OC_FX_NODEPTH` | `battle_view_fx_draw.cpp` | set | Draws effect triangles at depth 0. |
| `OC_FXDBG` | `battle_view_fx_draw.cpp`, `battle_view_fx_state.cpp`, `battle_view_trail.cpp`, `battle_view_units.cpp` | set | Effect debug log (global effect state, tilts, warp clears, sequence starts, rotations, links). |
| `OC_FXDBG_POSE` | `battle_pose.cpp` | joint (default 0) | Logs that joint's frozen rotation each tick. |
| `OC_BESPOKE_TRACE` | `src/game/effects/bespoke/overlay_base.h`, `battle_view_fx_skin.cpp`, `stage_scene.cpp`, `src/game/effects/bespoke/ground_ports_b.cpp` | set | Logs steered models of bespoke effects every 10 ticks, the overlays' ground state every 60 frames and the state changes of two ground overlays. |
| `OC_BIT7_TRACE` | `src/game/effects/emitter_collect.cpp` | set; `2` | Logs once per header the objects with flag bits 15 + 7; `2` also prints each draw's final matrix to stderr. |
| `OC_BASIS_TRACE` | `battle_fx.cpp` | set | Logs every ORIENT_BASIS resolve (selector, joint, second selector and joint). |
| `OC_FLARE_LOG` | `src/game/effects/emitter_collect.cpp` | set | Writes lens-flare values to stderr. |
| `OC_LINE_DBG` | `src/game/effects/bespoke/line_ring.cpp` | set | Writes line-ring op arguments and rope end points to stderr. |
| `OC_LIGHT_TRACE` | `battle_spell_lights.cpp` | set | Every 5 ticks logs the spell lights, their summed colour and the multiplier at the strongest light. |
| `OC_ANM_TRACE` | `battle_view_anm.cpp`, `battle_view_fx_cast.cpp`, `battle_view_trail.cpp` | set | Logs the animation-sequence trail events, eye toggles and the running pack's emitter names. |
| `OC_APPROACH_TRACE` | `battle_view_units.cpp` | set | Logs the reach terms of every approach movement. |
| `OC_APPROACH_CLIPS` | `battle_view_units.cpp` | set | Logs the reach of the source model's attach point per clip and frame, and the target's per clip. |
| `OC_REACH_TRACE` | `battle_view_units.cpp` | set | Logs every set-animation event and, per tick, the acting unit's posed reach. |
| `OC_DEATH_TRACE` | `battle_view_sequence.cpp`, `battle_view_units.cpp` | set | Logs the event group of each action and the KO / fade state of dying enemy units. |
| `OC_PAC_TRACE` | `src/battle/pac_vm.cpp` | set | Writes every executed battle-script op (entity, unit, slot, pc, op) to stderr. |
| `OC_PA_TRACE` | `battle_view_fx.cpp` | set | Every 20 ticks writes the pseudo-action pose to stderr. |
| `OC_SEQ_TRACE` | `battle_view_pseudo.cpp`, `battle_view_units.cpp`, `seq_tool.cpp` | set | Traces battle sequence VM ops. |
| `OC_SEQ_RAND` | `battle_seq.cpp` | integer | Forces the sequence `Rand m` op to return `k % m`. |
| `OC_SCRIPT_LOG` | `battle_view_units.cpp` | set | Logs each battle script start (tick, unit, kind, entry, variant). |
| `OC_OVAL_DEBUG` | `battle_seq.cpp` | set | Writes the oval-move parameters to stderr. |
| `OC_VRAM_DUMP` | `battle_view_fx.cpp` | file path | Writes the effect pack's 1024 x 512 VRAM image (halfwords) to this file. |
| `OC_LIST_ELEMENTS_EID` | `src/game/battle/demo_main.cpp` | effect id | `ocbattleview` prints the element ids using that effect id, then exits. |

### 3.4 Audio

| Variable | Read by | Values | Meaning |
|---|---|---|---|
| `OC_SFX_TRACE` | `src/audio/akao_player.cpp` (`sfx_trace_on`); set by `tools/ours_cast.ps1` | not empty, not `0`; `2` | Logs every SFX request, channel and voice event with its tick; `2` dumps 0x1000 bytes of program code instead of 0x30. With `ocbattleview` a device-less mixer also writes `sfx_trace.wav` next to the exe. |
| `OC_SFX_KEEP` | `akao_player.cpp` | hex mask | Plays only SFX requests whose bit is set; request n tests bit (n-1) mod 8, so only the low 8 bits count. |
| `OC_SFX_UNIMPL` | `akao_player.cpp` | set | Reports once per op the SFX opcodes with no audible model. |
| `OC_SFX_ONLY` | `tools/ocaudio.cpp` | `0`, `1` | `render-sfx`: plays only program 0 or 1 of the pair, no chained ids. |
| `OC_SFX_VOL` | `seq_tool.cpp`; set by `tools/py/sfx_batch.py` | 0..127 (default 127) | Volume of `ocbattleseq sfx`. |
| `OC_SFX_PAN` | `seq_tool.cpp`; set by `tools/py/sfx_batch.py` | 0..127 (default -1 = centre) | Pan of `ocbattleseq sfx` (64 = centre). |
| `OC_SFX_WAV` | `seq_tool.cpp`; set by `tools/py/sfx_batch.py` | file path | `ocbattleseq sfx` writes the render as a 44.1 kHz stereo 16-bit WAV. |
| `OC_AUDIO_DUMP` | `src/app/app_boot.cpp` | wav path | With no audio device open, captures the mix of a scripted run (per game tick) to this 48 kHz stereo 16-bit wav. |

### 3.5 Dev tools

| Variable | Read by | Values | Meaning |
|---|---|---|---|
| `OC_SOAK_LIST` | `tools/ocbattle.cpp` | set | `pac-soak` prints each scene's outcome. |
| `OC_FIND_TREE` | `tools/ocbattle_fx.cpp` | path below `cdrom/` | Only with `fx-audit --find <name>`: prints the drp tree with the emitter names of every nested drp (a model `.obj` path walks its effect drp). |
| `OC_FIND_HEX` | `tools/ocbattle_fx.cpp` | hex bytes | Only with `fx-audit --find <name>`: byte-searches every pack file and `at0.prd`. |
| `OC_FIND_USERS` | `tools/ocbattle_fx.cpp` | `a,b,c` | Only with `fx-audit --find <name>`: enemy ids and scenes whose model folder name is one of the names. |
| `OC_FIND_SEQOP` | `tools/ocbattle_fx.cpp` | hex op | Only with `fx-audit --find <name>`: every sequence command of that op in `at0.prd` and effect DRPs. |
| `OC_FX_MISSING` | `tools/ocbattle_fx.cpp` | set | `fx-audit`: reports parts missing from a pack on stderr. |
| `FXA_TEX` | `tools/ocbattle_fx.cpp` | set | `fx-audit`: prints each pack's atlas, texture and per-part visible-triangle counts. |
| `FXA_TRACE` | `tools/ocbattle_fx.cpp` | set | Every 100 ticks prints emitter and sequence statistics. |
| `OC_AUDIT_OPS` | `seq_tool.cpp` | `19,1A,...` | `audit` prints the operands of those ops once per script. |
| `OC_AUDIT_TRACE` | `seq_tool.cpp` | model stem | `audit` traces the attack variant-7 script of that stem. |
| `OC_AUDIT_HIT` | `seq_tool.cpp` | integer (default 0) | With `OC_AUDIT_TRACE`: hit code = value mod 10, and values of 10 or more select the dead case. |
| `OC_SEQ_GETPOS` | `seq_tool.cpp` | `ux,uz,tx,tz` | Unit position and the position the get-position op returns. |
| `OC_SEQ_G` | `seq_tool.cpp` | `idx[=val],...` | Sets sequence VM globals (value defaults to 1). |
| `OC_PACK_MODEL` | `seq_tool.cpp` | model name | `pack` mode: model whose attack table is loaded. |
| `OC_PACK_TABLE` | `seq_tool.cpp` | entry name | `pack` mode: restricts to this table entry. |
| `OCC_WALK` | `tools/occ.cpp` | set | Prints the walk triangles with flags. |
| `OCC_JOINTS` | `tools/occ.cpp` | set | Prints joints with both rotation and translation tracks of single-frame clips. |
| `OCPRD_VERBOSE` | `tools/ocprd.cpp` | set | Prints the first live objects of the emitter system. |
| `OCSTAGE_COLORS` | `tools/ocstage.cpp` | set | Prints every vertex colour. |
| `OCSTAGE_UV` | `tools/ocstage.cpp` | set | Prints the UV ranges of each submesh. |

### 3.6 Unit tests

| Variable | Read by | Values | Meaning |
|---|---|---|---|
| `BFX_NAMES` | `tests/test_battle_fx.cpp` | set | Prints every element's name, effect id and pack files, then exits. |
| `BFX_DIAG` | `tests/test_battle_fx.cpp` | element id | Prints what that element's effect pack holds. |
| `BFX_EID` | `tests/test_battle_fx.cpp` | effect id | With `BFX_DIAG`: effect id to inspect instead of the element's. |
| `BFX_SEQ` | `tests/test_battle_fx.cpp` | set | With `BFX_DIAG`: lists every command of every sequence. |
| `BFX_EMIT` | `tests/test_battle_fx.cpp` | emitter name | With `BFX_DIAG`: prints the ops of the emitter of that exact name. |
| `BFX_RUN` | `tests/test_battle_fx.cpp` | set | With `BFX_DIAG`: runs the pack for 200 ticks and prints the result. |
| `BFX_CHARGE` | `tests/test_battle_fx.cpp` | set | Prints the line for every cast-charge pack, not only failures. |
| `RM_LIST` | `tests/test_room_meshes.cpp` | file path | File with one room `.prd` stem per line; prints the stage load and validation result of each, then exits. |
| `RM_SIGN` | `tests/test_room_meshes.cpp` | file path | File with room `.prd` stems; data check of the mesh winding sign, then exits. |
| `OC_TEST_FORGE_SAVE` | `tests/test_save.cpp` | file path | Also writes the forged `save.dat` of the save test to this path. |
| `OC_TEST_MP3` | `tests/test_voice.cpp` | file path | MP3 file for the decoder check (skipped when unset). |

### 3.7 Footnotes

- `OC_SHADER_FORMATS` and `OC_VERSION_STRING` are compile-time definitions (CMake), not environment variables; `OC_GLSLC`, `OC_SHADERCROSS`, `OC_SHADER_SRC`, `OC_SPV_DIR` and `OC_SHADER_INCS` are CMake variables of `src/render/CMakeLists.txt`.
- `tests/routes/*.env` files set `OC_*` variables for one route, one `NAME=value` per line, with the meaning in a leading comment. Variables used there: `OC_ROUTE_ELEM`, `OC_ROUTE_GAUGE`, `OC_FXDBG`, `OC_BATTLE_AUDIT`, `OC_BATTLE_NORUN`, `OC_BATTLE_MISS`, `OC_BATTLE_SEED`, `OC_BATTLE_FIELD`, `OC_CAM_KEYS`, `OC_CAM_PICK`, `OC_AUDIO_DUMP`, `OC_RESULTS_HURT`, `OC_RESULTS_HEALER`, `OC_STUCK_TICKS`.
- `tools/ours_cast34.ps1` sets `OC_QUIET`, `OC_BATTLE_SEED`, `OC_CAST_PARTY`, `OC_CAM_PICK`, `OC_FX_OBJECTS`, `OC_FX_SEQ`, `OC_FX_UNITS`, `OC_FX_DT` before launching.
- The game also reads the system variables `USERPROFILE` (Documents folder) and `WINDIR` (system font folder).

## 4. Hotkeys

### 4.1 Running game

| Key | Handled in | Action |
|---|---|---|
| Esc | `app_run.cpp` | Opens or closes the settings window (not during a route or replay). |
| Pad Start, held 500 ms | `app_run.cpp` | Opens the settings window (a Start press while it is open closes it); a shorter tap opens the pause screen on release. |
| Space | `keymap.h`, `app_sim.cpp` | Start (the pause screen), on the press. |
| K, pad Back | `keymap.h`, `app_run.cpp` | Select (change the lead character); held in battle, hides the overlays during a cast. |
| F1 / F2 | `app_run.cpp`, `cheats.cpp` | Toggle slow / fast speed on the press (the two exclude each other; pad LT / RT); not during a route or replay, and ignored on the speed-locked map. Saved to the config. |
| F3 | `app_run.cpp`, `cheats.cpp` | Toggles No Encounters on the press (pad L3 tap); saved. |
| F4 | `app_run.cpp`, `cheats.cpp` | Toggles Battle Boost on the press (pad R3 tap); saved. |
| F5 | `app_run.cpp`, `cheats.cpp` | Toggles Auto-battle on the press (pad L3 + R3); saved. |
| F6 | `app_run.cpp` | Cycles art mode SD, HD, Modded, then SD; saved to the config. |
| F7 | `app_run.cpp` | Toggles widescreen; saved (not during a route or replay). |
| F8 | `app_run.cpp` | Toggles the Debug tab. |
| F9 | `battle_view_ui.cpp` | Toggles the battle camera debug overlay. |
| PageDown / PageUp | `app_run.cpp` | Next / previous room; only in the field with the flow disabled and no movie. |

The top of the Debug tab (`app_debug.cpp`) has two panes. **Tools**: *Model viewer* (Open starts `opencross modelview`, section 1.12, as a separate process) and *Write log file*. **Sound test** (when the sound effects loaded) picks a global sound id 0..255 (effect00; `<` / `>` step with wrap, or type the id) and plays it on the menus' channel when Play is pressed; an id with no program is reported as not defined.

### 4.2 Debug free camera (Debug tab, battle 3D or title sea)

| Input | Action |
|---|---|
| W / S | Forward / back. |
| A / D | Strafe left / right. |
| E / Q | Up / down. |
| Shift (either) | Sprint. |
| Mouse left or right button held, pressed outside UI windows | Look; the pointer is captured while held. |
| Pad left stick / right stick | Move / look. |
| Pad triggers (RT / LT) | Up / down. |
| Pad left stick click | Sprint. |

The camera takes W A S D Q E and the pad sticks, triggers and left stick click away from the game while it is on and a battle scene or the title sea is shown. With "Hide overlays" on, the game receives no input except Esc, F8, F7 and the F-keys.

### 4.3 Keymap (two key sets per action)

| Action | Set 0 | Set 1 | PS1 pad |
|---|---|---|---|
| Up / Down / Left / Right | arrows | W / S / A / D | d-pad (the left stick past half way counts as the d-pad) |
| Ok | Enter | X | Circle |
| Cancel (also run in the field) | Backspace | Left Shift | Cross |
| Item (key-item list in the field) | F | R | Square |
| Menu (main menu in the field) | I | Tab | Triangle |
| SelectLeft | H | Q | L1 |
| SelectRight | J | E | R1 |
| SlowSpeed | F1 | unbound | L2 |
| FastSpeed | F2 | unbound | R2 |
| Start (fixed) | Space | | Start |
| Select (fixed) | K | | Back |

Screens show the key caps of set 0.

### 4.4 Demo tools

| Tool | Key | Action |
|---|---|---|
| all five | Esc | Quits. |
| `ocbattleview` | F1 | Toggles HD / original art. |
| `ocbattleview` | F3 | Toggles the camera overlay (F9 works too). |
| `ocbattleview` | Enter | Confirm (scripted by `--auto`, `--keys`); the other keys are the battle keymap. |
| `ocflow` | F1 | Toggles HD / original art (not in `--shots` runs). |
| `ocmenudemo` | F1 | Toggles HD / original art. |
| `ocmenudemo` | F3 | Opens the main menu. |
| `ocmenudemo` | F4 | Writes the screenshot sequence to `menu_shots`. |
| `ocshopdemo` | F1 | Toggles HD / original art. |
| `ocshopdemo` | F5 / F6 / F7 | Opens the Element / Smith / Trading shop. |
| `ocshopdemo` | Shift held | With F5..F7, opens shop id 1 instead of 0. |
| `ocshopdemo` | F4 | Writes the screenshot sequence to `shop_shots`. |
| `ocmodelview` | Space | Plays / pauses the clip. |
| `ocmodelview` | Tab | Switches between the original and the modded copy. |
| `ocmodelview` | R | Reloads both copies and rescans the mods folder. |
| `ocmodelview` | Left drag / wheel | Orbits / zooms (outside the panel). |

## 5. Scripts

Run the scripts from the repo root. `python -I` (isolated: no user site-packages, no current-directory imports) is used for the stdlib-only scripts, marked `[-I]`. Scripts marked `[np]` import numpy, Pillow, OpenCV (`cv2`), `zstandard` or pywin32 / `mss`, or run a script that does, and take plain `python`. Scripts that write files default to the scratch folder, or write to the output path given.

### 5.1 Effects diff (PS1 reference versus OpenCross)

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/py/fx_diff.py` [np] | a capture folder (`ref\states`, `ref\frames`, `ours\ours.log`, `ours\shots`); `--step N` (default 5), `--offset N\|auto`, `--cast-index N` (align on the Nth cast, 0 = first, when a capture holds several casts of the id; without it the cast under which most other sequences start together wins, ties to the later one, so a capture whose wanted cast is not the best-scoring one needs the flag), `--window A B`, `--info`, `--extract`, `--cam-score LOG..`, `--set-cam-pick S`, `--prune` | `ref\tables.json`, `diff.md`, `diff_full.md`, `shots.png` in the folder; the verdict block on stdout | Mechanical diff of one effect cast: sequence tasks, live objects, unit blocks, camera, fog, warp. The PS1 states are placed on a refresh-time axis from the battle refresh counter each carries (one PS1 game frame takes 2-3 refreshes; our ticks are refreshes), `diff.md` times are `R<refresh>`, `--window` takes refresh times and `--info` also prints `last_refresh:`. |
| `tools/py/fx_ref.py` [np] | `frames DIR`, `vram STATE\|DIR`, `objects STATE\|DIR`, `seqs STATE\|DIR`, `warp STATE\|DIR`, `models STATE\|DIR`, `nodes STATE`; `--every --crop x y w h --to W H --grid C R --from --upto --name --mat --bit7` | per-frame statistics on stdout | Brightness and colour curves of frame sequences; display buffers, live effect objects, sequence tasks, warp globals, unit model blocks and effect registry of a state. |
| `tools/py/cmp_frames.py` [np] | `REFDIR OURSDIR OFFSET FIRST LAST STEP [COLS ROWS]`; env `CROP` | per-frame luma and luma-grid table on stdout | Region statistics of reference frames versus OpenCross shots. |
| `tools/py/cmp_objs.py` [np] | `REFDIR OURS.log OFFSET FIRST LAST STEP` (log made with `OC_FX_OBJECTS`) | per-emitter live object counts on stdout | Compares live object counts per emitter, state versus OpenCross log (runs `fx_ref.py` per state). |
| `tools/py/reg_compare.py` [-I] | `REFDIR OURSDIR [--index FILE] [--md OUT] [--tsv OUT]` | per-scene tables on stdout, optional `.md` and `.tsv` | Compares the PS1 effect registry capture with `ocbattle fx-reg` output. |
| `tools/py/bit7_cmp.py` [-I] | `REF_MAT_TXT OURS_LOG [--offset N \| --scan] [--win W]` (log with `OC_BIT7_TRACE=2`) | per-name match table on stdout | Compares the matrices of attach-joint objects (flag bits 7 and 15) between reference and OpenCross. |
| `tools/py/ref_info.py` [np] | `REFDIR [step]` (default 10) | one line per state on stdout | Object count, camera eye / target / H and fog per state of a reference capture. |
| `tools/py/inst_dump.py` [np] | `STATE.sav [--all]`; reads `data\cdrom.dat` of the game folder | instrument table on stdout | Dumps the PS1 instrument table of a state, marking slots that differ from the first wave bank (needs `zstandard`). |
| `tools/py/vram_frame.py` [np] | `png STATE OUT.png [x y w h]`, `resample IN OUT W H [x y w h]`, `rgbgrid IN cols rows [x y w h]`, `mask A B thr`, `maskstats A B thr X...` | PNG or numbers on stdout | Writes a VRAM rectangle of a state as a PNG (only `png` needs `zstandard`); box-resamples, grids and masks PNGs. |
| `tools/py/png_stats.py` [-I] | `stats\|diff\|grid\|hist` plus PNG paths and optional box | numbers on stdout | Pixel statistics, diff, luma grid and histogram of PNGs with the stdlib only. |

### 5.2 Transition analysis (field to battle swirl)

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/py/trans_window.py` [np] | `segment DIR`, `table RDE_DIR PS1_DIR OURS_DIR [--step N]`, `rings FRAME.png [--centre cx cy]` | per-phase table on stdout; `trans_stats.json` cache in the frame folder | Finds the transition window in a frame folder; aligns RDE, PS1 and OpenCross on the first white frame; measures the contact effect's ring shape. |
| `tools/py/trans_measure.py` [np] | `DIR [step] [cx cy]`; env `CROP` (`x,y,w,h`) | one line per frame on stdout | Mean luma, percentiles, zoom scale, white and dark percentages per frame. |
| `tools/py/trans_cmp.py` [-I] | `retail.txt ours.txt first last step` (`trans_measure.py` tables) | side-by-side luma on stdout | Compares mean luma between two measurement tables with a fixed 9-tick offset. |
| `tools/py/trans_sharp.py` [np] | `DIR first last`; env `CROP` (`x,y,w,h`) | per-frame metrics on stdout | Mean luma and mean absolute luma gradient (x and y) per frame. |
| `tools/py/rde_trans_analyze.py` [np] | one or more RDE capture folders | per-folder table and merged 50 ms curve on stdout | Luma, gradient, white and dark percentages per RDE capture frame, relative to the first white. |
| `tools/py/rde_vs_ours.py` [np] | `RDEDIR [RDEDIR2...]` (reads `tests\routes\shots\ek_*.png`) | table on stdout | Compares RDE and OpenCross route shots aligned on the swirl's white frame. |
| `tools/py/rde_side.py` [np] | `RDEDIR out.png ms [ms ...]` (needs `cv2`) | montage PNG | Side-by-side montage, RDE on top and OpenCross below, at given times after white. |
| `tools/py/t_measure.py` [np] | `DIR_RDE WHITE_MS PREFIX_OURS FIRST GF...` | per-phase measures on stdout | Structure measures (centre / band luma, sharpness, radial autocorrelation) at matched phases. |
| `tools/py/t_mont.py` [np] | `rde\|ours` mode, source, reference, `OUT.png`, `GF...`, `[--crop x,y,w,h]` | montage PNG | Matched-phase montages RDE versus OpenCross, whole frames or crops. |
| `tools/py/t_sim.py` [np] | none (fixed paths inside the script, frames under the scratch folder) | error table on stdout | Offline simulation of the transition strip feedback against RDE frames. |
| `tools/py/zoom_track.py` [np] | `DIR[#prefix] FIRST_MS LAST_MS [--cx --cy] [--fps60]` | per-frame zoom table on stdout | Estimates the radial zoom of the smear between consecutive frames. |
| `tools/py/s_mont.py` [np] | `DIR[#prefix] OUT.png T0 T1 STEP [COLS]` | montage PNG | Montage of frames nearest to given times, labelled with time and luma. |
| `tools/py/k2_mont.py` [np] | `analysis.txt framesdir out.png ms1,ms2,...` | montage PNG | Montage of RDE frames nearest to given times from the analysis table. |

### 5.3 Frame time

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/py/u_ft.py` [-I] | `LOGFILE` (`frametime:` lines) | stats on stdout | n, mean, p99, max of frame time overall and per 40-frame window. |
| `tools/py/v_ft.py` [-I] | `LOGFILE` | stats on stdout | Frame-time statistics per phase (swirl, hold, battle) and every frame slower than 17.5 ms. |

### 5.4 SFX comparison

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/py/sfx_cmp.py` [np] | `ps1 DIR`, `ours LOG [OUT.csv]`, `cmp PS1.csv OURS.csv [--align TAG]` | `events.csv`, differences on stdout | Sound-request comparison of a PS1 capture and an `OC_SFX_TRACE=1` run. |
| `tools/py/sfx_cmp2.py` [np] | `ps1 DIR`, `ours LOG [OUT.csv]`, `cmp PS1.csv OURS.csv [--shift S]` | `events.csv`, differences on stdout | Same comparison with effect-sequence and animation-sequence paths and a shift option. |
| `tools/py/sfx_batch.py` [np] | optional substring filter of the cases; env `OC_EXE`, `OC_WC` | table on stdout, WAVs in the scratch folder | Renders every recorded battle sound offline with `opencross.exe` and compares it with the PS1 recording. |
| `tools/py/sfx_wave_cmp.py` [np] | `audio.raw t_event ours.wav [dur] [win_ms]` | per-window rms, centroid, L/R ratio and best lag on stdout | Compares a PS1 recording segment with an offline render of one sound. |
| `tools/py/sfx_bands.py` [np] | `raw t_event ours.wav t0_ms t1_ms win_ms` | band energy table on stdout | Per-window band energy (dB), PS1 versus OpenCross. |
| `tools/py/sfx_spec.py` [np] | `raw t_event ours.wav t0_ms t1_ms` | band table on stdout | Band energies (dB) over one segment, PS1 versus OpenCross. |
| `tools/py/sfx_xc.py` [np] | `raw t_event ours.wav t0_ms t1_ms` | best lag, correlation and gain on stdout | Waveform cross-correlation of a PS1 segment and OpenCross. |
| `tools/py/wav_rms.py` [-I] | `FILE.wav` | `seconds:rms` line on stdout | RMS level of a 16-bit WAV in half-second windows. |

### 5.5 Sample alignment (`x2_*`)

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/py/x2_audio.py` [np] | `raw [t0 t1]` | helpers (`load`, `load_wav`, `wav`) and stats | Loading and analysis helpers for PS1 `audio.raw` recordings (raw s16 stereo 44.1 kHz). |
| `tools/py/x2_xc.py` [np] | `ps1.raw t0 dur ours.wav [step_ms]` | alignment and per-block gain / correlation on stdout | FFT cross-correlation alignment. |
| `tools/py/x2_wave.py` [np] | `ps1.raw t0 dur ours.wav` | best lag, correlation and gain on stdout | Sample-level alignment by lag search. |
| `tools/py/x2_div.py` [np] | `ps1.raw t0 ours.wav` | onsets and divergence sample on stdout | First sample where OpenCross diverges from the PS1 recording (onsets are detected by the script). |
| `tools/py/x2_table.py` [np] | `raw:t0:dur:ours.wav ...` | one comparison line each on stdout | Batch comparison table of recordings versus renders. |

### 5.6 RDE capture (Windows, RDE running)

`rde_win.py`, `rde_capture.py`, `rde_key.py`, `rde_seq.py`, `rde_snap.py` and `rde_watch.py` need pywin32 and `mss` through `rde_win.py`; `rde_montage.py` needs only `cv2` and numpy. `rde_key.py`, `rde_seq.py`, `rde_snap.py` and `rde_watch.py` add `tools` to `sys.path`, so run them from the repo root. `rde_key.py`, `rde_seq.py` and `rde_capture.py` also need `pydirectinput`.

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/rde_win.py` | none (imported) | none | Helpers: find the RDE window, bring it to the front, grab its client area. |
| `tools/rde_capture.py` | `[outdir] [key list] [max_s] [after_white_s] [pre_s]`; env `RDE_WHITE_ONLY`; needs `windows-capture`, OpenCV | `f_<ms>_<mean*10>.png` frames | Captures the RDE field to battle transition frame by frame. |
| `tools/rde_key.py` | `out.png [key \| wSECONDS ...]` | 640x512 PNG | Presses keys in the RDE window and saves a screenshot. |
| `tools/rde_seq.py` | `out.png key\|- [n] [dt]` | montage PNG | Presses a key, then montages n thumbnails taken dt seconds apart. |
| `tools/rde_snap.py` | `out.png [width] [height]` | PNG | Best-of-burst screenshot of the RDE window (default 640x512). |
| `tools/rde_watch.py` | `seconds` | brightest-of-burst mean luma every 0.5 s on stdout | Watches the RDE window brightness. |
| `tools/rde_montage.py` | `dir out.png n [start end]` | 4-column montage PNG | Tiles n evenly spaced frames of a capture folder. |
| `tools/py/rde_bg.py` | library: `launch(save)`, `attach(pid)`, `press`, `hold`, `guest`, `Frames`; needs `windows-capture`, OpenCV | none | Drives RDE in the background: no focus change, in-memory pad input, frame capture while covered. Same save rules as `rde_save_redirect.py`. |
| `tools/py/rde_save_redirect.py` | `--save PATH [--card1 FILE.mcd] [--title-wait S] [--keys LIST] [--shot out.png] [--attach] [--keep-open] [--lang N]`; needs `pydirectinput` | optional 640x512 PNG | Launches RDE, waits for the title, points its save path at a copy or forged `save.dat` and optionally swaps the memory card 1 image; refuses paths inside `Documents\My Games`. Importable: `launch`, `Rde.redirect`, `Rde.replace_card1`, `check_save_path`. Run it under the RDE lock when several agents share the machine. |

### 5.7 Save checks

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tools/py/save_check.py` [-I] | `<save.dat> [slot]` | verdict per slot on stdout | Read-only directory, title, checksum and preview check of card 1, plus the ext_save member and the HMAC of the other members. |
| `tools/py/ngplus_diff.py` [np] | `A B [--all] [--saved-vars]`; a state `.sav` (needs `zstandard`), a raw save block `.bin` (`OC_SLOT_DUMP`) or `<save.dat>:<slot>` | field diff on stdout | Field-by-field diff of two game states in save terms. |
| `tools/py/cc_save_keyitem.py` [-I] | `<in save.dat> <out save.dat> <slot> <key item>...` | a plain stored ZIP `save.dat` | Copies a save with key items added to one slot, checksum recomputed. |
| `tests/routes/check_save_dump.py` [-I] | `<dump.json>` (from `cc_save.py dump`) | pass or fail list | Checks that the `save` route's fields land at their expected save-record offsets. |
| `tests/routes/run_save.ps1` | none; `OC_BUILD_DIR`; needs a save copy `tmp\save.dat` | exit code | Save round trip: blank scratch card, route `save`, `cc_save.py` checks, route `continue_save`. |
| `tests/routes/run_save_fresh.ps1` | none | exit code | Boot with an empty save folder, route `save_fresh`, `cc_save.py` and `save_check.py`, route `continue_fresh`. |

### 5.8 Helpers

| Script | Inputs | Outputs | Purpose |
|---|---|---|---|
| `tests/routes/run.ps1` | `<name> [-Config debug]` (route ini `tests\routes\<name>.ini`, optional `<name>.env`); env `OC_BUILD_DIR` | `<name>.log` next to the ini, the last route lines and the exit code on the console | Runs one route with `OC_QUIET` set and waits for it. |
| `tools/quiet_start.ps1` | dot-sourced; `Start-Quiet exe args workdir [stdout] [stderr] [-NoMinimize]` | a process object | Starts a test window minimised without activation, so it never takes focus. |
| `tools/ours_cast.ps1` | `-Out -Cast [-Tgt -Caster -Scene -Field -At -Frames -Cast2 -Cast2At -Cast2Tgt]` | `<Out>\ours.log`, `ours.csv`, `ours.wav` | Runs a battle cast with `OC_SFX_TRACE=1` at deterministic ticks for `sfx_cmp2.py`. |
| `tools/ours_cast34.ps1` | `<element id>` or `-Folder <name>`; `[-Scene -Caster -Tgt -Ecast -Weapon -Party -CamPick -Seed -CastAt -Frames -Ko -Keep -NoDiff -NoCamSearch -NoPoolSearch -CamPool -Dt -NoDt -NoStats -Auto -AutoFrames -EnemyHp -Offset -CastIndex -Exe -Ring]` | `ours\ours.log`, `ours\shots\`, then the `fx_diff.py` diff | OpenCross side of one per-id effect comparison against a PS1 capture. The run length defaults to `CastAt` + the capture span in refreshes (`fx_diff.py --info` `last_refresh:`) + 60. `-Ring` passes `--ring` (the capture's field ring). |
| `tools/ours_trans.sh` | `[tag]` (default `now`; needs the `enc_scale` route shots `es_*.png`) | `ours_<tag>.txt` in the scratch folder | Collects the `enc_scale` route shots and writes the `trans_measure.py` table. |

### 5.9 Covered elsewhere

| Script | Documented in |
|---|---|
| `tools/duckstation.ps1`, `tools/py/ps1ref.py`, `ps1_capture.py`, `ps1_cast.py`, `ps1_audio.py`, `ps1_registry.py`, `ps1_scene.py`, `ps1_state.py` | `README.md` section 3 and `validation.md` section 3 |
| `tools/ghidra_setup.ps1`, `tools/ghidra/` | `README.md` |
| `tools/py/cc_*.py` importers and save tools (`cc_audio`, `cc_container`, `cc_effects`, `cc_emitter_ops`, `cc_fieldscript`, `cc_lzss`, `cc_model`, `cc_model_util`, `cc_prd`, `cc_room`, `cc_save`, `cc_save_forge`, `cc_stage`, `cc_tables`, `cc_text`, `cc_tim`) | `README.md`, [`formats/`](formats/) |
