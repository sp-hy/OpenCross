# Route scripts and input replays

A route is a text script of timed inputs and checks that drives a real OpenCross process without a human: the driver feeds the same `platform::Input` the window would, one 1/60 s simulation tick at a time, and can screenshot, warp, write debug state and assert on the game. An input recording (`Record` / `Replay`) is the same idea with captured input instead of a script. This document describes the files, the language, the failure behaviour, the replay format, the room sweep and the save-checking routes (`src/game/route_script.*`, `src/app/app_route.cpp`, `src/platform/config.cpp`, `tests/routes/`). It is OpenCross tooling, not an RDE feature.

## 1. Files of a route

All files live in `tests\routes\` and share a name:

| File | Role |
|---|---|
| `<name>.ini` | The config the run uses (`--config`). Holds `[Debug] Script=...`, `Seed`, `ScriptSpeed`, `[Video] VSync=0`, `[Paths] SaveDir`, `ArtMode`. Keys it omits take their defaults. |
| `<name>.txt` | The route script (section 3). |
| `<name>.env` | Optional. One `KEY=VALUE` per line (`KEY` = letters, digits, `_`, not starting with a digit); other lines (comments, blanks) are ignored. Set for this run only and removed afterwards. Used for developer switches such as `OC_BATTLE_NORUN`, `OC_ROUTE_ELEM`, `OC_FXDBG`, `OC_BATTLE_AUDIT`. |
| `<name>.prep.ps1` | Optional. Run by `run.ps1` before the route starts, to generate the scratch files it needs (`true_ending.prep.ps1` forges the save the route continues from with `tools\py\cc_save_forge.py`, from `tmp\save.dat` or `$env:OC_SOURCE_SAVE`, on disc 2 so the ending's movies are disc 2's; `ngplus_ending.prep.ps1` empties card 1 of that save with `opencross occ save-blank`, using the Debug exe of the build tree, else the Release one). A non-zero exit code stops the run. |
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

Why a run is reproducible: input is synthetic, the step is fixed at 1/60 s regardless of wall time, the frame limiter is skipped while a script, replay or sweep runs, the audio device is not opened for a scripted run (`OC_ROUTE_AUDIO=1` opens it), no mod is applied, and `[Debug] Seed` fixes the random sources (a non-zero seed seeds the field script RNG, the flow's RNG and, for each battle, `OC_BATTLE_SEED`; seed 0 takes a clock seed for the field RNG and leaves battles unseeded). Set `VSync=0` in the ini so frames are not paced by the display.

A battle started from the field with `[Debug] Seed` set is seeded (`OC_BATTLE_SEED` is derived from the seed and a per-run battle counter). A standalone battle (`[Debug] Battle=<scene>`) takes `[Debug] Seed` as its seed unless the `.env` sets `OC_BATTLE_SEED`; `battle_camera`, `battle_chain`, `battle_dash`, `battle_dash2`, `battle_defend`, `battle_harness`, `battle_hit_switch`, `battle_menu_entry`, `battle_run`, `results_heal` and `smoke_battle` all set a seed. Deterministic baselines for refactor checks are `alu_chair`, `art_swap`, `battle_hud`.

## 3. Verbs

`ent` is an entity index of the room, `char` a character id, `who` is `player`, `p0`, `p1`, `p2` (party slot) or an entity index. `[timeout]` is in ticks, default 1800. A verb with a bad or missing argument fails the run with a usage message.

### 3.1 Input

| Verb | Meaning |
|---|---|
| `key down\|up <name>` | Hold / release a key (anything other than `down` releases). Held until released. |
| `tap <name> [ticks]` | Press, hold `ticks` (default 3, minimum 1), release, then 3 free ticks; finishes after the gap. |
| `action` | `tap` of Enter (the confirm key) with the default timing. |
| `pad on\|off` | Virtual pad connected with no button held / disconnected; releases its buttons. |
| `pad tap <a\|b\|x\|y\|start\|l3\|r3\|back\|lb\|rb>[+<button>...] [ticks]` | Press and release a pad button (a = south, b = east, x = west, y = north, l3 / r3 = the stick clicks, back = Back / View, lb / rb = the shoulders), or several joined by `+` held together (a chord, e.g. `rb+back`); connects the pad; same timing as `tap`. |
| `walk <x> <z> [tol] [timeout]` | Steer the virtual stick along a walk-mesh path until the player is within `tol` (default 24) of the world point. Replans every 40 ticks. Succeeds early if the room changed or a battle began. Fails when blocked (no 3-unit move in 150 ticks while the controls are free) or on timeout. |
| `walk_ent <ent> [tol] [timeout]` | Same toward an entity's current position. Fails if the entity does not exist. |
| `approach <ent> [tol] [timeout]` | `walk_ent` that never fails: it also ends (with a log line) when blocked for 40 ticks, on timeout, or when a dialogue opens / the controls are taken. |
| `skip [timeout]` | Press confirm every 10 ticks while a dialogue is open; finishes after 45 consecutive ticks with no dialogue open. Fails if a dialogue stays open past the timeout. |
| `mash until <what> <value> [timeout]` | Press confirm for 3 of every 12 ticks until the condition holds. `<what> <value>` = `flow <state>`, `room <name>`, `movie start\|end`, `idle <n>` (the controls free for n ticks in a row; 1 = at once: a room's own scene can free them for a moment between its steps), `shop open\|closed`, `menu open\|closed`, `message <n>` (a dialogue is open and the last field window opened shows message `n` of the room). |
| `choose <n> [timeout]` | Confirm plain messages (3 of every 24 ticks) until a choice is pending, press Down `n` times (every 6 ticks), then confirm every 12 ticks until the dialogue closes. |
| `read <ticks>` | From now on, in the background while later lines run: whenever a message window is open and no window's typewriter position has changed for `ticks` ticks, press confirm for 2 ticks, as a player who reads each page and then presses on. A `{wait(n)}` pause longer than `ticks` is pressed through as well. `read 0` turns it off. Returns at once. |
| `cast <element> [caster] [target] [timeout]` | Battle harness: ask the battle view to have unit `caster` (PCs 0-2, enemies 3+) cast element record `element` on unit `target` as soon as it is idle (past the intro and the spawn scripts, nothing running); without both units, party slot `caster` (default 0) casts it on the first enemy (an ally element on its own side, a double or triple tech by every participant). Finishes when the cast has begun. Fails when no battle comes within `timeout`, the battle ends first, or the cast does not begin within `timeout`. |
| `attack <0\|1\|2> [unit] [timeout]` | Battle harness: party slot `unit` (default 0) attacks the first enemy with a weak / medium / strong hit as soon as the view is idle; finishes when the attack has begun (failures as `cast`). |
| `fight [timeout]` | Wait for a battle (fail "no battle started" after `timeout`), then switch the battle view and results screen to auto and tap confirm 2 of every 8 ticks until the battle is over or the Retry battle choice is up. Fails after 3 x `timeout` ticks in the battle. |

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
| `wait parity odd\|even` | The field tick count has that parity. The scripted-turn bookkeeping runs on odd ticks only (`rules/field_actors.md` section 2.5), so a cutscene with a rotation op lasts one tick more or less by the parity it started on; a route that asserts its timing aligns first. |
| `wait room <name>` | The field room is `<name>`, no battle is active and the screen has faded back in. |
| `wait dialogue open\|closed` | A dialogue is open (anything other than `open` means closed). |
| `wait text <words...>` | An open dialogue's message (plain text, newlines as spaces) contains the words joined by single spaces. |
| `wait battle start\|end\|results\|retry\|idle` | A battle is active / not active / the post-battle results screen (or a versus battle's outcome plate) is up / the Retry battle choice is up (`rules/battle.md` 7.6) / the battle view is idle: past the intro and the spawn scripts, no action, event, script window or menu hand-over running and no `cast` / `attack` waiting (where a command menu would take over). |
| `wait idle` | The controls are free: no dialogue, fade, transition, menu, shop, key-item list, pause, dialogue log window, party screen or movie, and no battle; and the input has settled: the pad lock after a room load or a closed screen and the menu-button wait after a menu screen have run out, and no load or menu-open fade (or its black veil) is running. A menu key tapped right after it opens the menu. `mash until idle` uses the same test. |
| `wait flow <title\|name\|slots\|opening\|field>` | The game flow is in that state and no movie is playing. |
| `wait movie start\|end` | A movie is playing / has finished (`start` or `open` = wanted playing). |
| `wait guide <0\|1\|2>` | The user guide shows that page (1 controller, 2 keyboard, 0 closed). |
| `wait menu open\|closed` | The main menu, a save screen, rename, party or New Game+ list is up / gone. |
| `wait records` | The record pages' room-script scan has finished (`[Game] OpenCrossMenu`; `rules/menus.md` 17.6). |
| `wait shop open\|closed` | A shop screen is up / gone. |

### 3.3 Loads, writes and requests (debug state changes, return at once unless noted)

| Verb | Meaning |
|---|---|
| `load <room> [param]` | Warp to a room; `param` is the entrance (default 0). The disc follows variable 0x42 as on a map jump; a room that only the other disc's index names (a disc 2 room such as `crn_b16i` from a disc 1 start) switches to that disc and sets variable 0x42 to match. `[Debug] Room` and the debug menu's teleport pick the disc the same way. A room both discs hold keeps the current disc (`set var 42 1` first to start one on disc 2). Fails the run (exit 1) if the room cannot load. |
| `set var <hexid> <value>` | Write a script variable. |
| `set invincible <party 0\|1> <enemies 0\|1>` | No unit of that side drops below 1 HP (the Debug tab's Invincible boxes; the battle view applies it every tick, also to the next battles). |
| `set battle hp <unit> <n \| n%>` | Battle harness: battle unit `unit`'s HP now (PCs 0-2, enemies 3+), `n` or `n` % of its max HP (1 .. max). With its side's `set invincible` off, `set battle hp <unit> 1` makes the next hit a kill; a quarter or less ends its hit reaction in the weak stance. |
| `set critical <0\|1>` | Battle harness: the battle's weapon hits are criticals (the roll is still drawn, so the random stream is the same) until 0 or the next battle. Element casts never roll one. |
| `set hit <0\|1>` | Battle harness: the battle's weapon attacks and steals hit (1) or miss (0) whatever the hit roll (the roll is still drawn, so the random stream is the same) until the next battle. |
| `set group <0..31> <0\|1>` | Show / hide a background tile group as BG_GROUP_ON / OFF (80 / 81) do. |
| `set option <hexmask> <0\|1>` | Set or clear bits of the settings options word (0x200 run/walk toggle, 0x800 analog stick). |
| `set skin <byte>` | Set the window frame byte of the game options (0..14; 15 and up reach the last sheet) with Custom window frame (options bit 0x8) on, and variable 0x44 to it, as leaving the frame picker does. |
| `set buttons <0..3> <hexword>` | Write one word of the game's button configuration (0 field, 1 battle, 2 field analog, 3 battle analog; `formats/save.md` section 6). It acts while the options word has 0x100 set (`set option 100 1`); the route's keys and pad buttons then pass through the assignment like real input. |
| `set hp <char> <value>` | Write a character's current HP. |
| `set status <char> <hexbits>` | Write a character's status bits (1 poison, 8 burn, 10 sprain, 20 flu, hex). |
| `set model <char> <byte>` | Write the model byte of a party member's character record. |
| `put [ent <n> \| v] <x> <z>` | Place every party entity, or entity `n` only, at a world point (integer coordinates). `put v`: the leader there and the members of slots 1 and 2 in the party V behind it, as on a room entry (128 units, heading +- 0x200 off straight behind, the leader's height and facing; `fieldscript.md` 0E), the follower trails restarting there: the party stands apart instead of on one spot. The party moved: the camera settles on the leader as a room entry leaves it (not while a script holds the camera). |
| `put beside <ent> [dist]` | Place every party entity on the walk mesh beside entity `ent` where it stands now, the leader facing it: a following `tap confirm` talks to it at once (no walk; a patrolling entity is met where it is). The spot is inside the talk range and outside the touch range (`dist` = centre distance instead), on the entity's floor, clear of other entities when it can be, toward where the party stood first. The camera settles on the leader as after `put`. No walkable spot: a warning, the party stays. |
| `talk <ent>` (ent > 0) | `put beside <ent>` and its talk started the same tick, as the trigger pass starts it (not a button press: a press goes to the first entity in range in index order), so a walking entity is still in range. Only when the field takes a talk, as for the player (no transition or fade, no modal window, no input lock, no other talk running: `wait idle` first); else nothing starts (logged, `k ... busy`). An entity the player can't talk to from where the route can put the party (no walkable spot beside it on its floor: raised above it or off the walk mesh; or one that takes no talk now: inactive, flag 0x400002 or no-action) gets its talk script (slot 2) started directly as `request script` would (an inactive one is first made active and shown: not in the room in this run, its talk still plays), and a `trace` notes `k <tick> <entity> nospot\|notalk\|absent` (`none`: it has no talk script; nothing started, an inactive one stays so). |
| `art hd\|original\|modded\|toggle` | Switch the art mode as F6 does, without saving the ini; `toggle` flips between the HD family and original. |
| `settings <tab>\|close` | Open the settings window on the tab named `tab` (as Esc does, which a route cannot press), or close it. |
| `request <kind> [arg]` | Ask the host for what a field opcode would, then wait 4 ticks. The screen it opens is handled by following `wait` / `tap` lines. Kinds in the table below. |
| `remote add <pid> <name> <m0> <m1> <m2> <x> <y> <z> <facing>` | A remote party (`multiplayer.md` section 5) as if the network had sent it, without a server: the field model byte of each party slot (`-1` empty), and its leader standing at the world point with the facing (4096 per turn), in the field state. Adding a `pid` again replaces it. The route's parties are shown with the network's (the network's entry wins a shared `pid`) and stay across room loads until `remote clear`. |
| `remote move <pid> <x> <y> <z> <facing> <clip> <moving>` | A new leader pose of the party, as one `pres` message: `clip` a field clip id (2 run, 3 walk), `moving` 0 or 1. The state is kept. A `pid` the route never added fails the run (exit 1). |
| `remote state <pid> <0..3>` | The party's state: 0 field, 1 battle, 2 menu, 3 event. |
| `remote clear` | No route party any more. |

`request` kinds (`arg` is a decimal integer, default 0):

| Kind | Effect |
|---|---|
| `money <n>` | Set the gold. |
| `restart` | The General tab's Restart. |
| `keyitem <n>` | Give key item `n` (replacing a copy already held). |
| `battle <enc> [field] [flags]` | Start encounter `enc` with the session party, on battlefield `field` (default 0) with the battle flags `flags` (default 0), as the A3 / C9 operands give them. |
| `battle_disguise <enc>` | Same with the disguise-fight flag (battle flag 4). |
| `rename <char>` | Open name entry for the character, then the main menu. |
| `party` | Open the party screen. |
| `party_add <char>` | Add a character to the party. |
| `script <ent> <slot> [class]` | Start entity `ent`'s script slot `slot` as a spawn would (`FieldVM::request_script`; class default 3, a talk's): an editor runs one script of the room that no talk or touch starts. Nothing when the slot is empty; one already running (the room's own scene started it) goes on, and a `trace` counts it as the scene's. |
| `party_remove <ref>` | Empty a party slot as REMOVE_FROM_PARTY (7D) does; `ref` is a character reference in decimal (253 / 254 / 255 = slot 0 / 1 / 2, else a character id). |
| `save` | Open the cleared-save prompt (strips the roster, sets variable 0x7E to 3) and the save list. |
| `ngplus` | Open the New Game+ list over the current game. |
| `menu` | Open the main menu. |
| `disc <n>` | Disc-change screen (`n` = disc - 1). |
| `movie <n> [flags]` | Play movie row `n` from the disc in use as a `D4` with the second operand `flags` (default 0: the last frame is held; with flag 8 it fades out, `movies.md` section 6). |
| `trading <row>` | Open the trading post at `row`, then the main menu. |
| `elementshop <row>` | Open the element shop at `row`. |
| `weaponshop <row>` | Open the blacksmith at `row`. |
| `slotdump` | Write the current game as one 0x2000-byte save block (section 7). |
| `savemark` | Remember the current game as one save block, with the play time and the variables that show it (0x2C, 0x2E, 0x30) left out. |
| `savecheck` | The current game, taken the same way, must equal the `savemark` copy; otherwise log `route: savecheck: the save differs from the mark at byte <n>` and exit 1. |
| `versus <seed> [side]` | Start a versus battle (`rules/versus.md`) from the field against the local opponent: the session party as side `side` (0 = A, the default; 1 = B), the built-in test party (characters 3 4 26, star 20) as the other side, played by the random legal player; `fight` lets the same player command the session party. Exit 1 when it cannot start (not a free field, a battle under way, or a battle dev variable such as `OC_DEV_*` set). |

An unknown kind logs `route: unknown request <kind>` and exits with code 1.

### 3.4 Screenshots

| Verb | Meaning |
|---|---|
| `screenshot <file.png>` | Capture the next rendered frame. Relative names go to `shots\` next to the ini; absolute paths are used as given. The log gets `route: tick N screenshot <path> (room R)`, with `, movie frame F` (the 0-based frame on screen) while a movie plays. |
| `autoshot <every> <count> <prefix>` | In the background, request a screenshot every `every` ticks, `count` times, named `<prefix>00.png`, `<prefix>01.png`, ... while later lines run. Returns at once. A screenshot from `screenshot` on the same tick takes priority. |
| `sides <min%> <max%>` | Measure the next rendered frame's columns beside the centred 4:3 picture (the frame is rendered before the next line runs, like a screenshot): the share of lit pixels (a colour channel above 8 of 255) of the left side and of the right side must each be within the range, in whole percent. `sides 99 100` = a backdrop covers both sides, `sides 0 0` = black bars. Logs `route: sides <min> <max>: left L % right R % lit`; fails when a side is outside the range, when the window is no wider than the 4:3 picture, or when no frame was measured within 60 ticks. |

### 3.5 Marks, snapshots and traces

| Verb | Meaning |
|---|---|
| `mark` | Remember the player's screen position (for `assert screen`). |
| `mark ent <n>` | Remember entity `n`'s world position (for `assert moved`). |
| `mark time` | Remember the driver tick (for `assert elapsed`). |
| `mark anim` | From now on count, tick by tick, how often each party member's submitted clip changes (for `assert changes`). |
| `snap [hexvar...]` | Remember the simulation state: room, flow, room-load / battle-start / title-sequence / field-tick counters, the battle summary, every entity's position, facing and model yaw, the message windows' typewriter positions and the listed variables. |
| `trace <ent> <ticks>` | For `ticks` ticks log the entity's position, height, submitted animation id, step, facing, facing change, model yaw, animation clip and keyframe, the script pitch, whether it is drawn, the screen fade level (0 = the picture, 1 = black or white) (and the head-turn angles while a head callback is on), then a summary of speed, turns and clip behaviour. The statistics feed the `assert trace*` lines. |
| `track <ticks> [keys] [ent...]` | In the background, while later lines run, log one row per tick for `ticks` ticks: `route: track <room> t <n>` and, for each placed party slot, `p<slot> <x> <z> <y> <submitted clip id> <facing hex>`, then the same group as `e<ent>` for each listed entity (a pushed block, a follower that is not a party member). With `keys` every group ends in `<clip>:<key4>`, the playing clip's index and its keyframe in quarter keyframes (`-` without a clip). Returns at once. Two runs' rows compare the party's formation tick by tick; the run speed cheat's row k is compared with row 2k of the normal-speed run (`rules/field_actors.md` 2.8; routes `runspeed_ladder`, `runspeed_push`, `runspeed_scene`, `runspeed_waterfall`). |
| `dump` | Log the room's entity table. |
| `pos` | Log tick, room, player position and control yaw. |
| `log <text...>` | Log `route: <text>`. In a controlled run (section 8.1) the controller also gets `log <tick> <text>`. |
| `quit` | End the run (exit 0). |

### 3.6 Asserts

An assert finishes at once; a failing one stops the run (section 4). Passing asserts that measure something log it (`route: ...`).

| Assert | Passes when |
|---|---|
| `room <name>` | The field room is `<name>`. |
| `var <hexid> <value>` | The script variable equals `value`. |
| `title_hold on\|off` | The title scene is / is not held on black until its logo movie takes over. |
| `earned <name>`, `unearned <name>` | The achievement was / was not earned this run: a Steam API name, or the id of an OpenCross achievement (`rules/oc_achievements.md`; the route's ini sets `[Game] Achievements=1`). Route runs never connect to Steam; the rules still run. |
| `dialogue open\|closed` | A dialogue is open / closed. |
| `portrait left\|right\|none` | A dialogue is open and the last field window opened has its face panel on that side (`none`: no face, or a label). |
| `text <words...>` | An open dialogue's message contains the words. |
| `notext <words...>` | A dialogue is open and its message does not contain the words. |
| `choiceline <n>` | The choice hand sits on message line `n` of the open list (translated numbering). |
| `battle active\|inactive` | A battle is / is not active. |
| `battle events <tokens...>` | The battle view's route events (`w<msg>` = script window opened, `a` = action began) start with exactly those tokens. |
| `battle menu <slot\|-> <guards>` | The party slot whose top command menu is up and has taken over from the menu entry (`-` = none, or the window still opening), then one digit per party slot: 1 = the unit holds Defend's guard. Example: `battle menu 1 100`. |
| `battle hitbox <shown> <cursor> <waiting> <stamina>` | The HIT% box of the PC whose menu is up: 1 when it is drawn (also during the run-up and the hits it starts), else 0; its cursor row (0..2 the strengths, 3 Element); the strength picks waiting for the running hit; that PC's stamina. `- - - -` without a menu PC. Example: `battle hitbox 1 0 2 50`. |
| `battle facing <yaw> <yaw> <yaw>` | The yaw each party slot's model shows (12 bit, 4096 = 360 deg), `-` for an empty slot. Example: `battle facing 1258 1024 1024`. |
| `battle plate [word]` | The last versus battle ended on its outcome plate; with a word, that plate (`Victory`, `Defeat`, `Draw`, `No result`). |
| `battle camera orbit <slot>\|shot\|target\|hold` | What drove the battle camera on the last tick: the menu orbit round that party slot, a camera shot or timeline, a target-selection glide, or nothing (the pose stays). |
| `result <n>` | The battle-result variable equals `n`. |
| `flow <state>` | The flow state name equals `state`. |
| `menu open\|closed`, `shop open\|closed` | The screen is up / not up. |
| `movie playing\|idle` | A movie is / is not playing. |
| `guide <0\|1\|2>` | The user guide page equals the value. |
| `pause open\|closed` | The pause screen is up / not up. |
| `dialoguelog open\|closed`, `dialoguelog entries <min> [max]`, `dialoguelog cursor <n>` | The dialogue log window (over the field, or the OpenCross row's page) is up / not up; it lists between min and max entries (logged with the count and the cursor); its selected entry is n (0 = the oldest). |
| `retry open\|closed` | The Retry battle choice is up / not up. |
| `icons <n>` | The last frame drew n play-time feature icons (slow, fast, encounters off, battle boost, auto battle; `text.md` 10.2). |
| `name <char> <text>` | The typed name of the character (UTF-8; one word, since arguments split on spaces); `-` = no typed name. |
| `party <id,id,id>` | The party ids in slot order (`-` = empty slot), e.g. `0,1,22`. |
| `disc <n>` | The disc in use. |
| `song <id\|none>` | The music mixer is sounding song `id` (`none`: no song, or a one-shot that has ended). The mixer follows its commands only while it runs, so the route needs an output device or `OC_AUDIO_DUMP` (its `.env`). |
| `location <id>` | The saved location word of the state. |
| `gold <n>` | The gold held. |
| `option <hexmask> set\|clear` | All mask bits of the settings options word are set / not all set. |
| `config <section> <key> <value>` | The settings in force store that key with that value (as `opencross.ini` would hold it; logged), e.g. `assert config Game RecordsHints full`. A `--config` run never writes its ini, so this is how a route checks a setting changed in a menu. |
| `hp <char> <min> <max>` | Current HP in the range. |
| `status <char> <hexmask> set\|clear` | All mask bits are set / not all set. |
| `near <ent\|player> <x> <z> <tol>` | World position within `tol` of the point. |
| `facing <who> <angle> [tol]` | Facing (4096 per turn) within `tol` (default 64). |
| `gap <who> <who> <min> <max>` | Horizontal distance between two entities within the range. |
| `level <who> <who> <tol>` | The world y of two entities (`p0`, `p1`, `p2` or an entity index) differs by at most `tol`: a rider stays on its platform. |
| `moved <ent> <min>` | The entity is at least `min` units from where `mark ent` saw it. |
| `screen right\|left\|up\|down` | Player screen movement since `mark`: right/left need more than 8 px and mostly horizontal; up/down need more than 1 px. |
| `rel <who> <ref> <dx> <dy> <tol>` | Screen offset of `who` from `ref` equals `(dx, dy)` within `tol` on each axis. |
| `ent <n> model\|body\|char\|shown\|moving\|paused\|wet\|pitch <value>` | Room model index / the character model byte the entity wears (its record's model byte once that model is on the entity, -1 for an entity without a character model, or while its model could not be loaded) / its character id (-1 none) / drawn / script motion active / motion pre-empted / walk flags carry water / script pitch (op 27, 4096 per turn); each 0 or 1 except `model`, `body`, `char` and `pitch`. |
| `height <who> <min> <max>` | World y (down is positive) in range. |
| `anim <who> <id\|stair>` | Submitted clip id equals `id`, or the entity stands on a stair tile. |
| `key <p0..p2\|ent> <clip> <key4>` | The entity plays the clip with that index at that keyframe, counted in quarter keyframes (the values of `track ... keys`). |
| `changes <p0..p2> <max>` | The member's submitted clip changed at most `max` times since `mark anim` (a clip that flips every tick fails). |
| `offmesh <p1\|p2\|ent N\|npcs> <max>` | Ticks the member stood off the walkable mesh is at most `max`; `npcs` takes the worst placed non-party model entity. |
| `elapsed <min> <max>` | Driver ticks since `mark time` within the range. |
| `scroll <x> <y> [tol]` | The field scroll (layer pixels, before the shake offset) is within `tol` (default 0) of `(x, y)` on each axis. |
| `same [tol]` | Nothing from the `snap` changed: room, flow, room-load / battle-start / title-sequence counters, battle summary, every entity's position (within `tol` units, default 0), facing and yaw, listed variables. |
| `frozen` | No field tick ran and no message window's typewriter moved since `snap`. |
| `swaps <n>` | Exactly `n` in-place art swaps of the field room since `snap`. |
| `remote <n>` | Exactly `n` remote parties were picked to be drawn on the last field tick (shown, not in a battle, leader model loaded, among the nearest). |
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
| `Battle` | -1 | Start a standalone battle view on this scene (scenedat index) instead of a room. |
| `BattleParty` | empty | The standalone battle's party: character ids, slot 0 first (`0,1,22`); empty = Serge, Kid, Harle. |
| `BattleField` | -1 | The standalone battle's battlefield (stage `bg_NN`); -1 = `ShowcaseStage`, else the scene's own. |
| `BattleIntroLive` | off | The standalone battle's stage intro (`bti*`) plays live for the load's ticks before the party intro cuts it, without the field battle's darkening ('fade', under the hand-over's warp in the game); off, the load fast-forwards it (a stage previewer turns it on). |
| `HoldScripts` | off | A controlled run's room stops running scripts after its INIT (run at the load): no script record runs, so no cutscene starts and the actors stand, while the effects, background animation and the world's clock go on (`FieldWorld::set_hold_scripts`). An editor's picture of a room's effects; with it, a pack whose effects all play on load shows every one. |
| `SkipBattles` | empty | A battle the field asks for is not fought: the encounter's hand-over plays, then the battle ends at once with a result (0 won, 1 lost, 2 fled; `r,r,...`: the run's nth skipped battle takes the nth, later ones the last) and the field comes back as after a losable fight; the scene goes on. A `trace` notes it as `b <tick> <battle id> <result>` and does not end `battle`. An editor's preview of a scene around a fight (a boss mid-scene, a touched monster's win). |
| `SkipMovies` | off | A movie the field asks for is over at once (its `f` note stays): an editor plays the movie's length itself. |
| `SkipMenus` | off | A screen a script opens (a save point's menu, a shop, name entry; one a route's `request` opens too) closes at once, as when the player backs out of it (var 0x4C = 0); the scene goes on. A `trace` notes it as `u <tick> <kind>`: 0 the party exchange (FE 5B), 1 the main menu (FE 72), 2 an element shop (FE 5E), 3 a blacksmith (FE 68), 4 name entry (FE 4A), 5 the disc change (FE 65), 6 a trading post (FE 67), 7 the ending's save (FE 85), 8 the New Game+ list (FE 84), 9 the monster picker (FE 75). |
| `ForceBranches` | empty | `<pc>:<to>,...` (decimal script addresses): after the instruction at `<pc>` runs, its record goes on at `<to>` whatever the instruction decided (`FieldVM::set_forced_branches`). An editor's run of one way of a test that no start state reaches (the entrance used, an item, a party member): each test of the way is listed with the address it jumps to or the one after it. Only in a controlled run (`Control`). |
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
| `Control` | off | With `Script`: another program drives the run (section 8.1). |
| `ControlDepth` | off | With `Control`: each frame comes with its depth image (section 8.1). |

### 8.1 Controlled runs

With `[Debug] Control=1` and a `Script`, the program that started OpenCross drives the run through its stdin and stdout, one line per command or reply. A scene editor uses it to show any tick of a scripted scene: it seeks to a tick, gets that frame, and seeks back and forth.

The run does not advance on its own. A `seek` runs the route and the game tick by tick (as with `ScriptSpeed`, without drawing the ticks between) until the driver tick asked for, draws that tick and writes the frame. A seek to an earlier tick first restarts the run in the same process: a new game, the `Seed` (1 when it is 0), the `Room` and `Entrance`, the field tick count and the field generators as a fresh start has them, no movie and no screen fade left from the run before, and the route from its first line. A restarted run plays exactly as the first did, so the same tick always gives the same frame.

With `[Debug] Battle` the run is a standalone battle instead (a battle-action previewer uses it): the start and every restart is that scene with `BattleParty` on `BattleField`, seeded (`OC_BATTLE_SEED`, else `Seed`, else 1), and a `cast` / `attack` the route had asked for and not begun is dropped. The route waits with `wait battle idle`, then `cast` / `attack`, and marks the action's start and end with `log` lines. Each start empties the view's model, stage and effect-pack caches, so a restart plays changed files of the draft folder (a `reload` when the set of files changed). A restarted battle runs as a fresh one does (the route's actions begin at the same ticks, the units and the camera are in the same places; the effect runs' particle seeds count from the battle's start), and its frames match a fresh process's to about 1 % of the pixels. Once the route has ended the game goes on ticking with no input (the scene plays out), and the run only ends on `quit` or when stdin closes.

Commands (stdin):

| Command | Meaning |
|---|---|
| `seek <tick> <file>` | Run to driver tick `tick` (restarting first when it is behind the run), draw it and write the frame to `file`: the window's pixels as raw RGBA, 4 bytes a pixel, rows top to bottom. A new `seek` replaces one still running. |
| `restart` | Restart the run now (as above). The draft folder's files are read as the run loads them, its `text\<member>` rows (an editor's unsaved dialog) again at the restart; files added to or removed from it need `reload`. |
| `reload` | Look at the mods again (the `[Debug] DraftDir` folder included), apply them and restart the run: a controller that changed files in the draft folder sees them played. Replies `reloaded`. A room whose art did not change (only its script, walk mesh or zones) keeps its built art, models and effects: a reload and the run back to a scene's start take a few tens of ms. |
| `scroll <x> <y>` / `scroll off` | Hold the field scroll at (x, y) layer pixels from now on (the camera's follow and script targets are ignored) and redo the off-screen cull for it at once; `scroll off` gives the scroll back to the camera. A `seek` to the tick the run is at then draws that part of the room: a controller stitches a whole room from frames at several scrolls. |
| `room <tick> <x max> <x min> <y max> <y min> <file>` | Like `seek`, but the picture is the whole room in one pass (`App::draw_room_shot`): the camera's pan range given (`x max` .. `y min`, the room's `RoomView::Limits`), drawn 2x with square pixels into a target of (320 + x max - x min) x (224 + y max - y min) layer pixels at the middle of the range (a draw-only scroll; columns and rows 0..320 / 0..224 centred, `Letterbox::room`), every entity drawn whatever the camera's off-screen cull says. The `state` reply gives the picture's top-left scroll (`x max`, `y max`) and every entity's place, drawn = not hidden by its script; `depth` (with `ControlDepth`) and `frame` read the target, not the window. One draw: no seams between screens. Differences from a stitch: parallax groups and effects filed under a layer sit at the one draw scroll; effects culled to the 224-row band stay in it. |
| `view <tick> <scroll x> <scroll y> <zoom> <file>` | Like `room`, but the game camera's frame at that scroll (its widescreen columns included) showing `zoom` times as much room each way (below 1: zoomed in), at the window's resolution (2x at least), with the message windows screen-fixed over it (depth stamped with `ControlDepth`): an editor's camera pan / zoom. Replies `state` (the run's own camera scroll), then `depth` / `frame` as `seek`. |
| `camera <r0 .. r8> <tx> <ty> <tz> <h> <rrggbb> [noactors]` / `camera off` | The field drawn through this GTE camera (rotation row-major with 1.0 = 1, translation, projection distance H: a room camera block's values) instead of the room's: actors, shadows, effects and room meshes over a flat `rrggbb` in place of the background layers, every column drawn, the PS1 packet-drop rules off (as for the free camera); `noactors` = the effects alone. Drawing only: the simulation keeps the room's camera. Frames come from `view <tick> 0 0 1 <file>`: a point projects to layer (160 + H x / z, 120 + H y / z), the picture's pixel (w / 2 + (sx - 160) k, sy k) with k = h / 224. Kept across restarts; a `plan` or `camera off` ends it. Replies `camera on` / `camera off`. An editor's orbit around effects. In a standalone battle (`[Debug] Battle`) the same camera drives the battle camera instead (`BattleView::set_control_camera`: exactly H whatever the FOV setting, no ground lift); `noactors` hides the units and their shadows, `rrggbb` is the clear colour behind the stage (seen where no stage object draws), frames come from `seek`, and a point projects about row 112 instead of 120 (the battle's projection centre). |
| `grid <cx> <cz> <step> <half>` / `grid off` | While the run draws through its own camera (`camera`): a floor grid at height 0, a line every `step` units, `half` steps each way from (cx, cz) (the lines through the origin red along x, blue along z, a green stub up), drawn with the effects: each cell side is a line primitive filed by its depth like an effect's line, so particles and actors sort and depth-test against it. `camera off` and a `plan` end it. Replies `grid on` / `grid off`. In a standalone battle the grid is drawn with the stage's own effects. |
| `xform <name,...> <a0 .. a8> <pivot x y z> <shift x y z>` / `xform off` | The named room effects' objects drawn as if moved: a point x shows at A (x - pivot) + pivot + shift (A row-major, 1.0 = 1; field units). Drawing only (the effects run on unchanged), so an editor shows a drag live as plain frame requests and applies the edit on release. `camera off` and a `plan` end it. Replies `xform on` / `xform off`. In a standalone battle the stage's own emitters of those names are moved (battle units). |
| `replay <name,...>` | The named effects played again from their start where the run stands: each emitter of the name killed (its objects freed) and started again from its first frame (`EmitterSystem::replay`), everything else going on; in a standalone battle the stage's own emitters. An editor loops an effect without restarting the room or battle. Replies `replayed <n>` (the names found). |
| `fxmem` | The effect object arena's use (`EmitterSystem::heap_usage`), measured after each tick since the last `replay` or the run's start: an editor's effect-memory gauge. Replies `fxmem <size> <used> <peak bytes> <peak objects> <full> <per>`: the arena's size (the field 0x4000, room 8 0xC000; battle 0x13000), the bytes in use now and at most (blocks with their 12-byte node headers), the most objects live at once, the spawns that found no room (the game drops those objects), and `name:n,...` each emitter's most objects at once (`-` none). In a standalone battle the stage's own emitters. `fxmem none` before anything plays effects. |
| `fxsprites <tick> <r0 .. r8> <tx> <ty> <tz> <h> <file>\|<texdir>` | The room effects' sprite quads at driver tick `tick`, for an editor that draws them in its own scene: the run goes to the tick without drawing (an earlier tick restarts it, as `trace`), then `EmitterSystem::collect_quads` through the given GTE camera (as `camera` takes it; the two paths are the rest of the line, split at `|`, so they may hold spaces; nothing is drawn, the run's own camera untouched). `file`: one line per quad, farthest first: `q <sheet> <frame> <sprite> <untextured> <blend> <r> <g> <b> <r1> <g1> <b1> <slot> <cam z> <x0> <y0> .. <x3> <y3>`, corners in the texture's corner order (u0v0, u1v0, u0v1, u1v1; mirroring folded in) at the layer point (160 + H x / z, 120 + H y / z), `cam z` the owning object's camera depth (0 for lines), sheet `-` for untextured quads (1 flat, 2 line to r1 g1 b1); blend 0 opaque, 1 B/2+F/2, 2 B+F, 3 B-F, 4 B+F/4. Each sprite's texels go once into `texdir` as `<sheet>_<frame>_<sprite>.rgba` (u16 w, u16 h, RGBA; RDE's 4x HD texels when there are some). Then one line per face of the room effects' meshes (sprite type 14 of the room's effect .prd; no order, an editor depth-tests them): `t <tex> <blend> <slot>` and per corner `<x> <y> <z> <u> <v> <r> <g> <b>` (the layer point, the corner's view depth, u / v normalised in the texture); `tex` `-` = untextured, else `mesh_<room>_<n>`, its texels once into `texdir` as `<tex>.rgba`. Replies `fxsprites <quads> <file>`, `fxsprites none` (no effects in this run). |
| `campose` | A standalone battle's camera pose where the run stands (`BattleView::camera_pose`, the battle's own camera whatever `camera` draws), in the PS1's y-down space that `camera` takes: `campose <eye x y z> <target x y z> <h>`. An editor's free camera starts from it, so turning it on doesn't move the picture. `campose none` outside a battle. |
| `anim <entity> <anim> [min ticks]` | The field entity's actor plays script animation id `anim` (0..254: an s4 slot, 0x80 + k the k-th extra clip; as SET_ANIMATION sets it; the id it already has starts again from its top) from the next tick on, then goes back to the id it had before (its automatic stand / walk / run clip when it had none) once the id has played out (the anim-done bit WAIT_ANIMATION waits for; a looping clip at once) and at least `min ticks` (default 0) have passed: an editor plays an animation on an actor of a room that stands still (`HoldScripts`), then `seek`s / `room`s on to see it. A second `anim` replaces the first (another actor goes back at once); a restart, a `plan` or another room drops it. Replies `anim <entity> <anim>`, and `anim done <entity> <tick>` (the driver tick) when the actor has gone back (also when a second `anim` sends it back), or an error when no room is on show, the entity does not exist or has no model, or its model has no such animation. |
| `trace <from> <ticks> <file>` | The scripts' timeline of a scene, for a controller's own timeline view: the run goes to driver tick `from` (a scene mark; restarting first when it is behind) and on without drawing, the field VM tracing (`FieldVM::Trace`): one event per instruction a script record runs at a new place (a wait that runs again at the same place every tick is one event) and one when a record ends; INIT runs and the idle UPDATE loops (class 7, slot 1) are left out. The scene is the records of class < 7 a trigger started (a talk, a touch, a collision, an editor's `request script`) and the ones they started; the room's own (its INIT, the UPDATE loops and what they start: wandering or animating NPCs, an opening scene) never end and don't count. It stops when none of the scene's records is live but parked ones (0B, 3B, FE 16) and records on an entity that doesn't run (inactive: a script that hid its own entity, a monster gone after its fight, one asked for on an inactive entity; nothing brings it back once no other scene record runs), and no room change is pending (`done`), every one left has waited or polled in place 600 ticks or has only gone round code its entity's script slot ran before for 3600 ticks (a minute) (a loop until a flag this run never sets, an idle animation or patrol the scene left running, a touch started again and again; a counted loop is shorter), unless one of them (not a looping one) waits on a walk op whose entity has moved in that time or on E2 while a message window still pages on (`stalled`; a long walk or a long dialog waits in place while it plays, and runs on), another room loaded (`room`), a field battle began (`battle`: the scene hands over to it; with `SkipBattles` the battle ends at once and the scene goes on), none of the scene's records ran within 300 ticks and none waits for its turn on an active entity (`none`; a script asked for on an inactive entity never runs) or `ticks` ran (`limit`). `file` holds 16 bytes per event, little-endian: tick u32 (counted from `from`), pc u32, entity u16, record u8, script slot u8, class u8, flags u8 (1 the instruction yielded, 2 the record ended there), 2 zero bytes. Beside it, `<file>.txt` holds notes, one per line, ticks as the events': `v <tick> <entity> <pc> <handle> <value> r\|w` a script instruction read / wrote a variable (a bit test reads the variable that holds the bit; a read only when its value differs from the last one noted at that pc and entity, so a polling loop notes each change once), `d <tick> <window> <speaker entity> <message> <opener entity>` a message window opened (speaker -1: none; the opener = the entity whose script ran the op), `w <tick> <window> page <n>` / `typed <n>` (the page fully shown, waiting for the player) / `close` a window's state changed after the tick, `f <tick> <movie>` a movie started, `b <tick> <battle id> <result>` a battle `SkipBattles` skipped (0 won, 1 lost, 2 fled), `u <tick> <kind>` a screen `SkipMenus` closed (kinds as for `SkipMenus`), `k <tick> <entity> busy\|nospot\|notalk\|absent\|none` a `talk` that started the script directly or started nothing (`talk`), `p <tick> <entity> <x> <y> <z> <facing> <drawn>` an entity's place after the tick when it changed (as `state` gives it: world units, y down = +, facing 4096 a turn, drawn 0 / 1), `s <tick> <x> <y>` the camera's scroll after the tick when it changed, and on any end but `done` `x <tick> <entity> <record> <pc> <op key> <ticks waited> <class> <state>` for each of the scene's records still live (what it waits on: the op at `pc`, key 0xFExx on the extended page; state 0 waits in place, 1 never ran, its entity inactive, 2 loops, its ticks counted since it last ran new code). The run stays where the trace stopped. |
| `btrace <from> <ticks> <file>` | A battle action's timeline (a `[Debug] Battle` run): the run goes to driver tick `from` (the action's mark) and on without drawing, the battle view tracing (`BattleView::set_route_trace`) until the view is idle again after it was busy (`done`), it never got busy within 120 ticks (`none`), the battle ended (`end`) or `ticks` ran (`limit`). `file` is text, one event per line, ticks counted from `from`: `a <tick> <unit> <table> <pc> <word>...` an anmseq op (unit 0-8, 9 = the effect pack's pseudo-actor, -1 a VM no unit shows; table `pack:<tag>` / `pack:anim` / `s5:<model>` / `atk:<model>` / `own` / `?`; pc = the op's word index in that table's block; the op's words), `s <tick> <sequence> <offset> <op> <hex>` an effect-sequence command (its byte offset in the sequence's payload, its bytes after the 4-byte prefix, `-` = none), `c <tick> <shot>` a camera shot selected, `p <tick> <eye x y z> <target x y z>` the camera's pose after each tick (stage world, y up), `u <tick> <unit> <x> <z>` each unit's place on the floor after each tick, `h <tick> <phase>` the presented action's phase when it changes (`none` / `approach` / `hit` / `return` / `cast` / `release` / `run` / `swap` / `settle`; `BattleView::action_phase`): the action runs from the tick its phase leaves `none` to the tick it is `none` again, its pre-roll and settle (the hit reactions and numbers) included; one under way at `from` comes first with the tick it began (negative: before `from`). The run stays where the trace stopped. |
| `plan <ini>` | Load another run into the process, so a controller keeps one engine for every preview it shows: `ini` holds the keys a controlled run's ini would (`[Debug]` `Room`, `Entrance`, `Battle`, `BattleParty`, `BattleField`, `BattleIntroLive`, `HoldScripts`, `Script`, `DraftDir`; `ForceBranches`, empty when left out; `SkipBattles` (empty), `SkipMovies` and `SkipMenus` (off) when left out; `[Video]` `Width`, `Height`, `Widescreen`); a key left out keeps its value. `Battle` -1 is a room run (a standalone battle running is left), `Battle` >= 0 a standalone battle (over the field). A changed `DraftDir` looks at the mods again (as `reload`); a changed size resizes the window (the frames' shape: with `Widescreen` on, a wider window shows the extra columns). Then the run restarts from its first route line, as a fresh process with that ini would start it (same frames). The process must have started as a controlled run (`Script` + `Control`); `[Video]` / `[Debug]` keys read only at boot (`ControlDepth`, the mod list) stay. Replies `planned room` / `planned battle`. |
| `hud 0` / `hud 1` | A standalone battle's HUD, menus and windows not drawn / drawn again (the damage digits stay): the bare action. Kept across restarts and plans. Replies `hud <0 / 1>`. |
| `stage 0 <rrggbb>` / `stage 1` | A standalone battle's stage not drawn / drawn again: its meshes, the unit shadows on them and its own emitters' sprites; the picture is cleared to `rrggbb` behind the units and the action's effects, and a `grid` still draws (the floor without the battlefield). Drawing only: the stage loads and runs as usual, so the action, its camera and the units' places are the same. Kept across restarts and plans. Replies `stage <0 / 1>`. |
| `tick` | Report the driver tick. |
| `quit` | End the run. |

Replies (stdout):

| Reply | Meaning |
|---|---|
| `ready` | The run has started and takes commands. |
| `state <tick> <scroll x> <scroll y> <entity...>` | Sent when a `seek` reaches its tick, before its `frame`: the field camera's scroll (layer pixels, before shakes) and, for each entity that has a place, `<n>:<x>,<y>,<z>,<facing>,<drawn>,<yaw>,<clip>,<at>,<from clip>,<from at>,<weight>` (world units with y down = +, facing and the model's yaw 4096 per turn, drawn 0 / 1; the actor's pose at the end of the tick: its clip and keyframe position x 256, the clip a cross-fade comes from, -1 for none, and its position, the current clip's share of the blend x 4096), so a controller can lay its own markers over the frame and pose the models itself; then `windows:1` while a message window is open. Outside the field: the tick and `0 0` only. |
| `depth <tick> <width> <height> <file>` | With `ControlDepth`, sent before its `frame`: the frame's depth image has been written to `<frame file>.depth`, RGBA8 like the frame, each pixel's depth-buffer value (0..1, ordering-table slot / 8192: a background tile's slot, a model's camera z / 16) as a 24-bit integer in r, g, b, high byte first. 1.0 = nothing drawn. With it a controller can sort its own overlays into the frame as the engine sorted the frame. |
| `frame <tick> <width> <height> <file>` | The frame of `tick` has been written. |
| `scroll <x> <y>` / `scroll off` | Reply to the command of the same name. |
| `restarted` | `restart` is done. |
| `planned room` / `planned battle` | `plan` is done: the new run is at its start. |
| `hud <0 / 1>` | Reply to `hud`. |
| `stage <0 / 1>` | Reply to `stage`. |
| `trace <ticks run> <events> <why> <file>` | `trace` is done: the ticks it ran (the scene's length), the events written, why it stopped (`done` / `stalled` / `room` / `battle` / `none` / `limit`). |
| `btrace <ticks run> <events> <why> <file>` | `btrace` is done: the ticks it ran (the action's length), the lines written, why it stopped (`done` / `none` / `end` / `limit`). |
| `tick <n>` | The driver tick. |
| `log <tick> <text>` | A route `log` line ran at that tick (a controller marks scene points with them). |
| `failed <tick> <message>` | A route line failed (section 4); the run goes on with no input. |
| `error <text>` | A command it could not take. |

Only replies go to stdout; the log goes to stderr (and to the log file), each reply also as a `control: <reply>` log line.

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
