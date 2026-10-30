# Route scripts and input replays

A route is a text script of timed inputs and checks that drives a real OpenCross process without a human: the driver feeds the same `platform::Input` the window would, one 1/60 s simulation tick at a time, and can screenshot, warp, write debug state and assert on the game. An input recording (`Record` / `Replay`) is the same idea with captured input instead of a script. This document describes the files, the language, the failure behaviour, the replay format, the room sweep and the save-checking routes (`src/game/route_script.*`, `src/app/app_route.cpp`, `src/platform/config.cpp`, `tests/routes/`). It is OpenCross tooling, not an RDE feature.

## 1. Files of a route

All files live in `tests\routes\` and share a name:

| File | Role |
|---|---|
| `<name>.ini` | The config the run uses (`--config`). Holds `[Debug] Script=...`, `Seed`, `ScriptSpeed`, `[Video] VSync=0`, `[Paths] SaveDir`, `ArtMode`. Keys it omits take their defaults. |
| `<name>.txt` | The route script (section 3). |
| `<name>.env` | Optional. One `KEY=VALUE` per line (`KEY` = letters, digits, `_`, not starting with a digit); other lines (comments, blanks) are ignored. Set for this run only and removed afterwards. Used for developer switches such as `OC_BATTLE_NORUN`, `OC_ROUTE_ELEM`, `OC_FXDBG`, `OC_BATTLE_AUDIT`. |
| `<name>.log` | Written by the run, next to the ini (a `--config` run always logs). Deleted by `run.ps1` before the run starts. |
| `shots\` | Next to the ini (`tests\routes\shots\`), created at start. Relative screenshot names land here. |
| `shots\route_failed.png` | Screenshot taken when a route check fails (section 4). |

Example ini (`alu_chair.ini`):

```
[Debug]
Room=alu_a03i
Entrance=0
Panels=0
Seed=1
Script=tests/routes/alu_chair.txt
ScriptSpeed=3
[Video]
VSync=0
```

Relative paths in the ini (`Script`, `SaveDir`) are resolved against the working directory, which `run.ps1` sets to the repository root.

### 1.1 `run.ps1`

```
$env:OC_BUILD_DIR = "build\win-x64"        # optional; this is the default
tests\routes\run.ps1 <name> [debug|release]  # second argument defaults to debug
```

* The executable is `<repo>\<OC_BUILD_DIR>\bin\<Debug|Release>\opencross.exe`.
* Any `<name>.env` lines are set as process environment variables; `OC_QUIET=1` is always set (the test window takes no focus, the mixer runs at zero gain). All of them are removed again when the run ends.
* The process is started with `--config "<name>.ini"` and the repository root as working directory, and waited for.
* It prints `exit code <n>`, then the last 40 log lines that contain `route`, `ERROR`, `[E]`, `WARN` or `[W]`, and exits with the process exit code.

Exit codes of the process:

| Code | Meaning |
|---|---|
| 0 | The script reached `quit` or its last line without a failure. |
| 1 | A check or blocking command failed (`route FAILED`), `load <room>` failed, an unknown `request` kind was used, or window / renderer start-up failed. |
| 2 | The route (or replay) file could not be read, or a line has a time field but no command. |

## 2. Timing model

* The driver counts **ticks** (`RouteState::tick`), one per simulated 1/60 s step, in field and in battle alike. `[Debug] ScriptSpeed` (integer, minimum 1, default 2) is how many route steps run per rendered frame. Each step is: read the game state, run one tick of the current script line, apply what it asked for (warp, variable write, screenshot...), then advance the game by exactly one tick of 1/60 s.
* After a step that requested a screenshot, or after the last step, the frame loop stops stepping and renders, so the screenshot shows the state of that tick. Such a frame therefore carries fewer than `ScriptSpeed` steps; the tick count is unaffected.
* A line is `<when> <command> [args]`. `<when>`:

| Form | Meaning |
|---|---|
| `N` | Start once the driver tick has reached `N`. |
| `+N` | Start `N` ticks after the previous line finished. |
| `-` | Start right after the previous line finished. |

* A line starts only after the previous line has finished. Blocking commands finish when their condition holds; each has a timeout (default 1800 ticks, given as the last argument where the table says so) and fails the run when it runs out. Tick counts of timeouts are measured from the tick the line began.
* `#` or `;` starts a comment anywhere on a line (so neither can appear inside arguments). Blank and comment-only lines are skipped. Every other line must have a `<when>` and a command, otherwise loading fails (exit code 2). Integer arguments are read in decimal or with a `0x` prefix; arguments named `hex...` (variable ids, masks, status bits) are read as hex with or without `0x`.
* The route ends at `quit`, after its last line, or at a failure. There is no global time limit.

Why a run is reproducible: input is synthetic, the step is fixed at 1/60 s regardless of wall time, the frame limiter is skipped while a script, replay or sweep runs, the audio device is not opened for a scripted run, and `[Debug] Seed` fixes the random sources (a non-zero seed seeds the field script RNG, the flow's RNG and, for each battle, `OC_BATTLE_SEED`; seed 0 takes a clock seed for the field RNG and leaves battles unseeded). Set `VSync=0` in the ini so frames are not paced by the display.

A battle started from the field with `[Debug] Seed` set is seeded (`OC_BATTLE_SEED` is derived from the seed and a per-run battle counter). A standalone battle (`[Debug] Battle=<scene>`) takes `[Debug] Seed` as its seed unless the `.env` sets `OC_BATTLE_SEED`; `battle_chain`, `battle_dash`, `battle_dash2`, `battle_defend`, `battle_run`, `results_heal` and `smoke_battle` all set a seed. Deterministic baselines for refactor checks are `alu_chair`, `art_swap`, `battle_hud`.

## 3. Verbs

The parser accepts 27 top-level verbs. `ent` is an entity index of the room, `char` a character id, `who` is `player`, `p0`, `p1`, `p2` (party slot) or an entity index. `[timeout]` is in ticks, default 1800. A verb with a bad or missing argument fails the run with a usage message.

### 3.1 Input

| Verb | Meaning |
|---|---|
| `key down\|up <name>` | Hold / release a key (anything other than `down` releases). Held until released. |
| `tap <name> [ticks]` | Press, hold `ticks` (default 3, minimum 1), release, then 3 free ticks; finishes after the gap. |
| `action` | `tap` of Enter (the confirm key) with the default timing. |
| `pad on\|off` | Virtual pad connected with no button held / disconnected; releases its buttons. |
| `pad tap <a\|b\|x\|y\|start> [ticks]` | Press and release a pad button (a = south, b = east, x = west, y = north); connects the pad; same timing as `tap`. |
| `walk <x> <z> [tol] [timeout]` | Steer the virtual stick along a walk-mesh path until the player is within `tol` (default 24) of the world point. Replans every 40 ticks. Succeeds early if the room changed or a battle began. Fails when blocked (no 3-unit move in 150 ticks while the controls are free) or on timeout. |
| `walk_ent <ent> [tol] [timeout]` | Same toward an entity's current position. Fails if the entity does not exist. |
| `approach <ent> [tol] [timeout]` | `walk_ent` that never fails: it also ends (with a log line) when blocked for 40 ticks, on timeout, or when a dialogue opens / the controls are taken. |
| `skip [timeout]` | Press confirm every 10 ticks while a dialogue is open; finishes after 45 consecutive ticks with no dialogue open. Fails if a dialogue stays open past the timeout. |
| `mash until <what> <value> [timeout]` | Press confirm for 3 of every 12 ticks until the condition holds. `<what> <value>` = `flow <state>`, `room <name>`, `movie start\|end`, `idle <any>` (the argument must be present, its value is ignored), `shop open\|closed`, `menu open\|closed`. |
| `choose <n> [timeout]` | Confirm plain messages (3 of every 24 ticks) until a choice is pending, press Down `n` times (every 6 ticks), then confirm every 12 ticks until the dialogue closes. |
| `fight [timeout]` | Wait for a battle (fail "no battle started" after `timeout`), then switch the battle view and results screen to auto and tap confirm 2 of every 8 ticks until the battle is over. Fails after 3 x `timeout` ticks in the battle. |

Key names for `key` and `tap`:

| Name | Key |
|---|---|
| `up` `down` `left` `right` | arrows (first binding of the action) |
| `confirm`, `ok` | Enter |
| `cancel` | Backspace |
| `run` | Left Shift (second Cancel binding) |
| `menu`, `tab` | Tab (second Menu binding) |
| `triangle` | I (first Menu binding) |
| `item` | F (first Item binding) |
| `select` | K |
| `start`, `space` | Space |
| `escape` | Escape |
| `a`..`z` | the letter key |
| `f1`..`f9` | function keys (the hotkeys: F1 / F2 speed, F3-F5 play-time modes, F6 art mode, F7 widescreen, F8 debug tab) |

The bindings are those of `src/platform/keymap.h`. An unknown name fails the run.

### 3.2 Waits

All waits take an optional trailing `[timeout]` except `wait <ticks>` and `wait text` (default timeout only).

| Verb | Finishes when |
|---|---|
| `wait <ticks>` | `ticks` ticks have passed since the line began. |
| `wait room <name>` | The field room is `<name>`, no battle is active and the screen has faded back in. |
| `wait dialogue open\|closed` | A dialogue is open (anything other than `open` means closed). |
| `wait text <words...>` | An open dialogue's message (plain text, newlines as spaces) contains the words joined by single spaces. |
| `wait battle start\|end\|results` | A battle is active / not active / the post-battle results screen is up. |
| `wait idle` | The controls are free: no dialogue, fade, transition, menu, shop, key-item list, pause, party screen or movie, and no battle. |
| `wait flow <title\|name\|slots\|opening\|field>` | The game flow is in that state and no movie is playing. |
| `wait movie start\|end` | A movie is playing / has finished (`start` or `open` = wanted playing). |
| `wait guide <0\|1\|2>` | The user guide shows that page (1 controller, 2 keyboard, 0 closed). |
| `wait menu open\|closed` | The main menu, a save screen, rename, party or New Game+ list is up / gone. |
| `wait shop open\|closed` | A shop screen is up / gone. |

### 3.3 Loads, writes and requests (debug state changes, return at once unless noted)

| Verb | Meaning |
|---|---|
| `load <room> [param]` | Warp to a room; `param` is the entrance (default 0). Fails the run (exit 1) if the room cannot load. |
| `set var <hexid> <value>` | Write a script variable. |
| `set option <hexmask> <0\|1>` | Set or clear bits of the settings options word (0x200 run/walk toggle, 0x800 analog stick). |
| `set hp <char> <value>` | Write a character's current HP. |
| `set status <char> <hexbits>` | Write a character's status bits (1 poison, 8 burn, 10 sprain, 20 flu, hex). |
| `set model <char> <byte>` | Write the model byte of a party member's character record. |
| `put <x> <z>` | Place every party entity at a world point (integer coordinates). |
| `art hd\|original\|modded\|toggle` | Switch the art mode as F6 does, without saving the ini; `toggle` flips between the HD family and original. |
| `request <kind> [arg]` | Ask the host for what a field opcode would, then wait 4 ticks. The screen it opens is handled by following `wait` / `tap` lines. Kinds in the table below. |

`request` kinds (`arg` is a decimal integer, default 0):

| Kind | Effect |
|---|---|
| `money <n>` | Set the gold. |
| `restart` | The General tab's Restart. |
| `keyitem <n>` | Give key item `n` (replacing a copy already held). |
| `battle <enc>` | Start encounter `enc` on battlefield 0 with the session party. |
| `battle_disguise <enc>` | Same with the disguise-fight flag (battle flag 4). |
| `rename <char>` | Open name entry for the character, then the main menu. |
| `party` | Open the party screen. |
| `party_add <char>` | Add a character to the party. |
| `save` | Open the cleared-save prompt (strips the roster, sets variable 0x7E to 3) and the save list. |
| `ngplus` | Open the New Game+ list over the current game. |
| `menu` | Open the main menu. |
| `disc <n>` | Disc-change screen (`n` = disc - 1). |
| `movie <n>` | Play movie `n` from the disc in use. |
| `trading <row>` | Open the trading post at `row`, then the main menu. |
| `slotdump` | Write the current game as one 0x2000-byte save block (section 7). |

An unknown kind logs `route: unknown request <kind>` and exits with code 1.

### 3.4 Screenshots

| Verb | Meaning |
|---|---|
| `screenshot <file.png>` | Capture the next rendered frame. Relative names go to `shots\` next to the ini; absolute paths are used as given. The log gets `route: tick N screenshot <path> (room R)`. |
| `autoshot <every> <count> <prefix>` | In the background, request a screenshot every `every` ticks, `count` times, named `<prefix>00.png`, `<prefix>01.png`, ... while later lines run. Returns at once. A screenshot from `screenshot` on the same tick takes priority. |

### 3.5 Marks, snapshots and traces

| Verb | Meaning |
|---|---|
| `mark` | Remember the player's screen position (for `assert screen`). |
| `mark ent <n>` | Remember entity `n`'s world position (for `assert moved`). |
| `mark time` | Remember the driver tick (for `assert elapsed`). |
| `snap [hexvar...]` | Remember the simulation state: room, flow, room-load / battle-start / title-sequence / field-tick counters, the battle summary, every entity's position, facing and model yaw, the message windows' typewriter positions and the listed variables. |
| `trace <ent> <ticks>` | For `ticks` ticks log the entity's position, step, facing, facing change, model yaw, animation clip and keyframe (and the head-turn angles while a head callback is on), then a summary of speed, turns and clip behaviour. The statistics feed the `assert trace*` lines. |
| `dump` | Log the room's entity table. |
| `pos` | Log tick, room, player position and control yaw. |
| `log <text...>` | Log `route: <text>`. |
| `quit` | End the run (exit 0). |

### 3.6 Asserts

An assert finishes at once; a failing one stops the run (section 4). Passing asserts that measure something log it (`route: ...`).

| Assert | Passes when |
|---|---|
| `room <name>` | The field room is `<name>`. |
| `var <hexid> <value>` | The script variable equals `value`. |
| `title_hold on\|off` | The title scene is / is not held on black until its logo movie takes over. |
| `earned <api name>`, `unearned <api name>` | The achievement was / was not earned this run. Route runs never connect to Steam; the rules still run. |
| `dialogue open\|closed` | A dialogue is open / closed. |
| `text <words...>` | An open dialogue's message contains the words. |
| `notext <words...>` | A dialogue is open and its message does not contain the words. |
| `choiceline <n>` | The choice hand sits on message line `n` of the open list (translated numbering). |
| `battle active\|inactive` | A battle is / is not active. |
| `battle events <tokens...>` | The battle view's route events (`w<msg>` = script window opened, `a` = action began) start with exactly those tokens. |
| `result <n>` | The battle-result variable equals `n`. |
| `flow <state>` | The flow state name equals `state`. |
| `menu open\|closed`, `shop open\|closed` | The screen is up / not up. |
| `movie playing\|idle` | A movie is / is not playing. |
| `guide <0\|1\|2>` | The user guide page equals the value. |
| `pause open\|closed` | The pause screen is up / not up. |
| `name <char> <text>` | The typed name of the character (UTF-8; one word, since arguments split on spaces). |
| `party <id,id,id>` | The party ids in slot order (`-` = empty slot), e.g. `0,1,22`. |
| `disc <n>` | The disc in use. |
| `location <id>` | The saved location word of the state. |
| `hp <char> <min> <max>` | Current HP in the range. |
| `status <char> <hexmask> set\|clear` | All mask bits are set / not all set. |
| `near <ent\|player> <x> <z> <tol>` | World position within `tol` of the point. |
| `facing <who> <angle> [tol]` | Facing (4096 per turn) within `tol` (default 64). |
| `gap <who> <who> <min> <max>` | Horizontal distance between two entities within the range. |
| `moved <ent> <min>` | The entity is at least `min` units from where `mark ent` saw it. |
| `screen right\|left\|up\|down` | Player screen movement since `mark`: right/left need more than 8 px and mostly horizontal; up/down need more than 1 px. |
| `rel <who> <ref> <dx> <dy> <tol>` | Screen offset of `who` from `ref` equals `(dx, dy)` within `tol` on each axis. |
| `ent <n> model\|shown\|moving\|paused\|wet <value>` | Room model index / drawn / script motion active / motion pre-empted / walk flags carry water; each 0 or 1 except `model`. |
| `height <who> <min> <max>` | World y (down is positive) in range. |
| `anim <who> <id\|stair>` | Submitted clip id equals `id`, or the entity stands on a stair tile. |
| `offmesh <p1\|p2\|ent N\|npcs> <max>` | Ticks the member stood off the walkable mesh is at most `max`; `npcs` takes the worst placed non-party model entity. |
| `elapsed <min> <max>` | Driver ticks since `mark time` within the range. |
| `same [tol]` | Nothing from the `snap` changed: room, flow, room-load / battle-start / title-sequence counters, battle summary, every entity's position (within `tol` units, default 0), facing and yaw, listed variables. |
| `frozen` | No field tick ran and no message window's typewriter moved since `snap`. |
| `swaps <n>` | Exactly `n` in-place art swaps of the field room since `snap`. |
| `traceclip <max_src_moves> <max_clip_changes>` | The last `trace` saw at most that many cross-fade source keyframe moves and clip changes. |
| `traceheadturn <ticks> <span>` | The last `trace` saw the head turn on at least `ticks` ticks over an angle range of at least `span` (4096 per turn). |
| `traceyaw <ticks> <octants>` | The last `trace` saw the model yaw change on at least `ticks` ticks and visit at least `octants` of the 8 sectors of 45 degrees. |

## 4. Assert and command failure

When a blocking command times out, a check fails or an argument is invalid, the driver:

1. logs an error `route FAILED at tick <t>: route line <n> (<verb>): <reason>` followed by `entities:` and the room's entity table (or `(no room)`);
2. requests `shots\route_failed.png`;
3. sets the exit code to 1 and ends the run after rendering that frame.

A run that passes ends with `final state: room <r> player <x>,<z> field ticks <n> BATTLE_RESULT <b> scenario <s>` and `exiting after <n> simulation ticks, <m> validation messages`; there is no explicit "route passed" line, the proof is exit code 0 and no `route FAILED` in the log. `run.ps1` shows the `route FAILED` line among its last 40 lines. Failures of `load` (room does not load) and unknown `request` kinds log `route: ...` errors and also exit 1, but take no `route_failed.png`.

## 5. Input record and replay

`[Debug] Record=<file>` writes every simulated step of a live or scripted session; `[Debug] Replay=<file>` plays one back. A route (`Script`) takes precedence over `Replay`; `Replay` is ignored when `Script` is set. Recording is not started during a replay.

Related behaviour of a replay: the room and entrance come from the file header (they override `Room` / `Entrance`), the flow is the field only, and the run is unthrottled like a route.

### 5.1 File format

Text, one record per line, written by `InputRecorder`, read by `InputReplayer`.

```
OCREPLAY 1 <room> <entrance>
<dt_us> <ticks> <held> <pressed> <pad_connected> <pad_down> <pad_pressed> <axes>
```

| Field | Meaning |
|---|---|
| `OCREPLAY 1` | Magic and version; anything else is rejected ("not an OCREPLAY 1 file", exit 2). |
| `<room>`, `<entrance>` | Field room name and entrance the session started in (the `[Debug] Room`, or `alu_a01f` when none; `[Debug] Entrance`). |
| `dt_us` | Frame time in microseconds (rounded `dt * 1e6`). 1/60 s steps of a route are written as `16667`. |
| `ticks` | Simulation ticks run for this frame (1 for route steps). |
| `held` | Comma-separated SDL scancodes that were down, or `-`. |
| `pressed` | Comma-separated scancodes with a press edge this frame, or `-`. |
| `pad_connected` | `1` or `0`. |
| `pad_down`, `pad_pressed` | Hexadecimal bit masks, bit `i` = pad button `i` (SDL gamepad button numbering) down / newly pressed. |
| `axes` | Comma-separated pad axes (SDL gamepad axis order), each multiplied by 1000 and rounded. |

Empty lines are skipped. A script step records its own synthetic input, so recording a routed run produces a replay of the route.

### 5.2 Playback

`replay_frame` takes one line per rendered frame. It rebuilds the `Input` (key down / pressed sets, pad state, axes divided by 1000), sets the frame time to `dt_us` and advances the game by `ticks` ticks of that frame time. Window events are not read. At the end of the file it logs `replay finished after <n> ticks` and ends the run (exit 0). The game does not re-derive edges: `pressed` is replayed as recorded.

## 6. Room sweep

`[Debug] SweepTicks=<n>` (n > 0) turns the run into a headless pass over every room of the game: no flow, no audio device, no battle or movie, no GPU effects. Per rendered frame it does this:

1. Update the loaded room's scripts for `n` ticks in one call (`SweepTalk` below).
2. If `[Debug] SweepTalk=<m>` is above 0, probe every non-party entity of the room: trigger its action script (pass one) and its collision script (pass two), each run for up to `m` ticks with a confirm press every 10 ticks, auto-winning battles, finishing movies and closing menus at once. A probe that leaves the room, or leaves scripts running, reloads the room before the next probe. Each room logs `sweep talk: room <r>: <p> probes, <u> unfinished, <n> reloads`, and an action script still running after `m` ticks logs `sweep talk: room <r> ent <e> ON_ACTION still running ...` with the script records.
3. An encounter that begins is won at once (`end_battle(0)`), a pending movie is finished, a pending menu is closed.
4. If a placed NPC stood off the walk mesh, log `sweep: npc off the walk mesh: room <r> entity <e> for <t> ticks`.
5. Load the next room (entrance 0). A room that fails to load logs `sweep: <room> failed` (counted); layer problems log `sweep: <room>: <report>` (counted); notes log `sweep: note <room>: ...`.

At the end the run logs: `sweep: <k> rooms with NPCs standing off the walk mesh`, `sweep: stubbed host call <name> x<n>` per host call that was stubbed, the VM statistics (`instructions, invalid pc, INIT overruns, unimplemented, unknown ops`), `sweep: <k> rooms with layer problems` and `sweep: <rooms> rooms visited, <f> failed to load, <v> validation messages`. The sweep ends with exit code 0 whatever it counted; judge it from those lines.

Environment variables read by the sweep:

| Variable | Meaning |
|---|---|
| `OC_SWEEP_ROOMS=a,b,c` | Sweep only those rooms. |
| `OC_SWEEP_FROM=<room>` | Start at that room (the list is not cut). |
| `OC_SWEEP_COUNT=<n>` | Stop after `n` rooms (minimum 1). |
| `OC_SWEEP_SCENARIO=<n>` | Set the story counter (variable 0) to `n` before each room loads, to reach story-gated branches. |

## 7. Save-checking routes

Routes that write saves use forged copies under `tmp\`, never the real save folder: `[Paths] SaveDir` points there and `[Paths] SaveFile` names a scratch file.

* **`run_save.ps1`** (Debug build): deletes `tmp\save_route.dat` and its folder, makes a scratch copy of `tmp\save.dat` with card 1 emptied (`opencross occ save-blank`), runs route `save` (`save.ini` / `save.txt`: new game with name entry, debug warp to Arni, sets the scenario and save-gate variables, opens the main menu, Save / Load, writes file 2 through the Save screen, `assert location 27`), then dumps the slots, the checksum and file 2 as JSON with `tools\py\cc_save.py`, runs `check_save_dump.py` on the dump, and finally runs route `continue_save` (Continue from the written file). Any non-zero exit code stops the chain.
* **`run_save_fresh.ps1`**: empties `tmp\save_fresh`, runs route `save_fresh` (the same `save.txt` against a folder with no `save.dat`, so the boot creates the file as a first launch does), checks the file exists, runs `cc_save.py slots`, `cc_save.py checksum` and `tools\py\save_check.py`, then route `continue_fresh`.
* **`check_save_dump.py <dump.json>`**: reads the `cc_save.py` dump of file 2 and checks that the title ends with the full-width file number `03`, the preview leader is `Serge`, the party is 0 and 1, the location word is 27 and the progression is 44, the preview stars match, the party bytes and the hidden party copy are at their offsets, variable 0 (offset 0x188) is 44, Serge has HP and gear, and the frame header is `SC`. It prints `save dump check FAILED:` with the list and exits 1, or `save dump check OK`.
* **`request slotdump <n>`** with **`OC_SLOT_DUMP=<prefix>`**: encodes the current game as one 0x2000-byte save block and writes it to `<prefix><n>.bin` (logged as `route: slot dump <path> (<bytes> bytes)`). Without `OC_SLOT_DUMP` it logs `route: slotdump needs OC_SLOT_DUMP` and writes nothing. Set the variable in the route's `.env` file.

## 8. `[Debug]` keys used by routes and tools

Read from the ini (`src/platform/config.cpp`); the ones a route ini sets are marked in section 1.

| Key | Default | Meaning |
|---|---|---|
| `Script` | empty | Route file to run (section 3). |
| `ScriptSpeed` | 2 | Route steps per rendered frame, minimum 1. |
| `Record`, `Replay` | empty | Input recording / replay file (section 5). |
| `Room`, `Entrance` | empty, 0 | Start in this field room and entrance (`alu_a01f` when a recording or sweep needs a room and none is set). |
| `Flow` | empty | Start state: `title`, `name`, `slots` (or `continue`), `opening`, or `field` / `off` / `none`. With no `Flow` and no `Room` the session starts at the title; with a `Room` it starts in the field. Forced to the field for sweeps, replays and `Battle`. |
| `Seed` | 0 | Fixed random seed (section 2). |
| `Battle` | -1 | Start a standalone battle view on this encounter instead of a room. |
| `BattleAuto` | off | The battle view and the results screen play themselves. |
| `Showcase`, `ShowcaseStage` | off, -1 | Showcase camera for a standalone battle; the battlefield it uses. |
| `SweepTicks`, `SweepTalk` | 0, 0 | Room sweep (section 6). |
| `Screenshot` | off | Take one screenshot of the live session to `screenshot.png` (or `screenshot_dialogue.png` with `AutoWalk`) in the executable's folder. |
| `ScreenshotTick` | -1 | Field tick (frame count in a battle) at which `Screenshot` fires; default 40, or `AutoActionTick` + 90 with `AutoWalk`. |
| `ExitAfterScreenshot` | off | Quit about 3 frames after that screenshot. |
| `AutoWalk` | off | The player walks to the nearest actionable entity (or `AutoTarget`) and triggers its action at field tick `AutoActionTick` (default 200). |
| `AutoTarget` | -1 | Entity `AutoWalk` goes to. |
| `AutoTeleport` | off | `AutoWalk` places the player next to the target instead of walking. |
| `AutoConfirm` | 0 | With `AutoWalk`: confirm an open message window every `n` ticks. |
| `ShowMessage`, `ShowChoice`, `ShowChoiceFirst`, `ShowFace` | -1, -1, 0, -1 | Open message `n` (with a choice list / face) at field tick 30 for dialogue layout checks. |
| `Panels` | off | Open the Debug tab at start (ignored for scripts and replays). |
| `Log` | off | Write `opencross.log`; a `--config` run always logs next to its ini. |

## 9. Example

`alu_chair.txt` (deterministic; walks a bounded path and checks the walk mesh stops the player):

```
0 wait 5
- request party_add 1
- set var 0x00 8
- load alu_a03i 0
- wait 90
- walk -84 210 24 900
- key down up
- trace 1 120
- key up up
- assert near player 26 519 12
- quit
```

`battle_hud.txt` shows the long form: boot to the title with `wait flow` / `wait movie`, `tap` through the menu, `wait flow field`, `request battle 1`, `wait battle start`, `screenshot`, `autoshot`, `fight 6000`, `quit`. `battle_dash.env` shows the environment file form (`OC_ROUTE_ELEM=147`, `OC_FXDBG=1`, `OC_BATTLE_AUDIT=1`).
