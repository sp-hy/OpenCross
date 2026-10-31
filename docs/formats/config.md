# opencross.ini

OpenCross keeps every user setting, the developer/automation switches and the Esc settings window's geometry in one INI file. This document describes the file location, how it is read and written, and every key (`src/platform/config.*`, `src/app/app_boot.cpp`). Mods and the `Dump*` keys also appear in `docs/formats/mods.md`; the save folder in `docs/formats/save.md`.

## 1. Location, lifecycle, syntax

* **Location.** `opencross.ini` next to the exe. `--config <file>` (anywhere on the command line, followed by a path) uses that file instead; the path is made absolute.
* **Log file.** With the default ini the log is `opencross.log` next to the exe and is written only when `[Debug] Log=1` (Debug builds always log). With `--config` the log is the ini's path with the extension replaced by `.log`, and is always written.
* **First launch.** If the file is missing or cannot be opened, the engine starts with the first-launch values of section 3 and writes the file at once. After the window exists it sets `[Game] Language` from the OS locale and writes the file again.
* **When it is saved.** Every change made in the Esc settings window (all tabs, including the window's own position, size and tab), F6 (art mode cycle), F7 (widescreen toggle), the RDE hotkeys that flip slow / fast / no encounters / battle boost / auto battle (F1-F5 and the pad equivalents), and the Debug tab's *Write log file* box. There is no save on exit. A write that fails is logged as a warning.
* **Rewrite, not patch.** Saving rebuilds the whole file from the in-memory settings: comments and key order of a hand-edited file are not preserved, unknown sections and keys are dropped, and the file starts with the one-line comment `; OpenCross settings. Leave GameDir/SaveDir empty to auto-discover.` The file is written with LF line endings.
* **Parsing (`Ini`).**
  * A line is trimmed of spaces, tabs and `\r`. Empty lines and lines starting with `;` or `#` are skipped. A comment must be a whole line; text after a value is part of the value.
  * `[Name]` starts a section (the name is trimmed; a line with `[` but no `]` is skipped). `key=value` splits at the first `=`; both sides are trimmed; a line without `=` is skipped.
  * Section and key names are case-sensitive and compared exactly. A repeated key keeps the last value; a repeated section merges into the first. Keys before any section header belong to the unnamed section, which no setting reads.
  * Booleans are read by `parse_bool`: `1 true yes on` are true, `0 false no off` are false (lowercase only); anything else keeps the default. Booleans are written as `1` / `0`.
  * Integers that do not parse keep the default. Floats are read with `strtof` and written with two decimals.
  * Keys that are not read by `Config::load` are ignored. Choice values that match no name keep the default unless stated in a table.
* **Missing keys.** Every key is read with the in-memory default as the fallback, so an ini may hold any subset of the keys.

## 2. Keys

Sections are listed in the order `Config::save` writes them; keys in each table are in written order. "Default" is the value the struct holds before the file is read (the first-launch overrides are in section 3). "Written" notes keys that are saved only conditionally.

### 2.1 `[Paths]`

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `GameDir` | empty | folder containing `data\cdrom.dat` | Game install. Used only if it holds `data\cdrom.dat`; otherwise (and when empty) the exe folder and up to 12 parent folders are searched. |
| `SaveDir` | empty | folder | Folder holding the save file. Non-empty = used as given. Empty = the newest `Documents\My Games\CHRONO CROSS  THE RADICAL DREAMERS EDITION\Steam\<id>` folder that contains a `save.dat`; else the current Steam user's folder (registry, then `loginusers.vdf`); else the most recently modified `Steam\<id>` folder; else `saves` next to the exe. |
| `SaveFile` | `save.dat` | file name | Save file name inside `SaveDir`. Written only when it differs from `save.dat`. Automated runs point it at a scratch copy. |

### 2.2 `[Video]`

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `Aspect` | `4:3` | `4:3 16:10 16:9 21:9 32:9 System` | Aspect ratio the settings window's Resolution list is filtered by. If missing or not one of the names, the first entry matching the stored `Width` x `Height` within 3.5 % is taken (`System` matches any). |
| `Width` | 1280 | min 320 | Window width (windowed) or the requested display mode width (exclusive). |
| `Height` | 960 | min 240 | Window height, as `Width`. |
| `WindowMode` | `windowed` | `windowed borderless exclusive` | Fixed-size window / desktop-size fullscreen window / fullscreen with the display mode closest to `Width` x `Height`. |
| `Display` | 0 | min 0 | Monitor index (0 = primary). |
| `VSync` | 1 | bool | Wait for the display refresh. |
| `Driver` | `auto` | `auto`, `vulkan`, `direct3d12` | Graphics backend. `auto` is read as empty (the library's choice); any other value is passed on as the driver name. If the driver is unavailable a toast names the one in use. |
| `FrameCap` | 0 | 0..1000 | Rendered frames per second limit; 0 = unlimited. The settings window offers 0, 30, 60, 75, 120, 144, 165, 240. Not applied in routes and replays. |
| `Filter` | `bilinear` | `nearest bilinear` | Texture sampling of the 3D models, battle stages and title sea, in every art mode. |
| `FilterRooms` | `nearest` | `nearest bilinear` | Sampling of the room background tiles. |
| `FilterEffects` | `bilinear` | `nearest bilinear` | Sampling of the field and battle effect sprites. |
| `FovDeg` | 35 | 30..110 | Horizontal field of view, in degrees of the 4:3 picture, of the battle and the title sea. 35 is the PS1 view; field rooms always keep the PS1 view. |
| `WideHud` | `full` | `default 16:9 full` | Widescreen only. `default` = battle HUD and main menu stay on the 4:3 canvas (shown as "4:3" in the window); `16:9` = spread to at most a 16:9 frame's width; `full` = to the window's edges. An unknown value keeps the default. |
| `WideEdgeFade` | 4 | 0..96 | Widescreen only: fade the room picture's cut edge to black over this many layer pixels; 0 = hard cut. |
| `Widescreen` | 0 | bool | The field, world map, battle and title sea fill the window's width; menus, dialogue, logo and movies stay 4:3. Off = 4:3 with black bars. F7 toggles it. |
| `ArtMode` | `hd` | `hd original modded` | Art set: RDE's HD assets / the PS1 disc assets / HD plus the mods and OpenCross's own improvements. `classic` is accepted as an alias of `original`. Any other value reads as `hd` and logs a warning. F6 cycles it. |
| `Ps1Res` | 0 | bool | "Retro look": draw the 3D and the menus at PS1 resolution and scale up with sharp pixels. |
| `RenderScale` | 1.00 | `0.5 0.75 1 1.5 2 3 4` | Resolution of the 3D pictures as a multiple of the output (with `Ps1Res`: of the 320 x 224 PS1 picture). The stored value snaps to the nearest listed value. |
| `Ps1Jitter` | 0 | bool | Integer vertex snapping and affine texture mapping in the 3D passes. |
| `Crt` | 0 | bool | Final full-frame CRT pass (scanlines, mask, vignette); the settings window and toasts are excluded. The pause screen also uses the pass. |
| `Msaa` | 1 | 1, 2, 4, 8 | Multisample antialiasing of the window-resolution pictures. The stored value is rounded down to a listed level; the GPU layer lowers it to what the hardware supports. |
| `Anisotropy` | 1 | 1, 2, 4, 8, 16 | Anisotropic filtering of the bilinear 3D textures. Rounded down to a listed level. |

### 2.3 `[Game]`

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `FastMode` | 0 | bool | RDE fast mode (F2, pad RT). |
| `SlowMode` | 0 | bool | RDE slow mode (F1, pad LT). Read as off when `FastMode` is on; the two are exclusive. |
| `Steam` | 0 | bool | Connect to Steam as the game: presence and achievements (General tab). `--config` runs and `OC_QUIET` runs never connect. |
| `AutoSave` | 1 | bool | RDE's auto-save (card 1, slot 0) on every location change. |
| `FastRate` | 0 | 0..5; 1 reads as 0 | Fast mode speed. 0 = RDE's (4x in the field, 2x in battle); 2..5 = that many times in both. |
| `SlowRate` | 50 | 10..90 | Slow mode speed in percent of normal, in the field and in battle. In the field, one 1/30 s stretch every 10 s (every 300th field frame) runs at normal speed. |
| `Language` | `en` | `en fr it de es jp` | Dialogue and menu text language. Stored as given; an unknown value is logged at boot and `en` is used. |
| `Portraits` | `2021` | `2021 1999` | RDE face art set; any other value reads as `2021`. |

Movies stay at normal speed in both speed modes, and both are off in the Radius tutorial (map and battle 386): the factor is 1 there and the speed hotkeys are swallowed.

### 2.4 `[Mods]`

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `DumpStages` | 1 | bool | "Dump all textures" includes the battle stage textures. |
| `DumpTitle` | 1 | bool | ... the title 3D scene textures. |
| `DumpRooms` | 1 | bool | ... the room 3D meshes and the models in them. |
| `DumpModels` | 1 | bool | ... the character and monster models. |
| `DumpWeapons` | 1 | bool | ... the battle weapons. |
| `DumpEffects` | 0 | bool | ... the effect sprite sheets and meshes (large). |
| `<mod name>` | `1,modded` | `<enabled>,<loadon>` | One line per mod of `<GameDir>\mods` (a folder, or a `.zip` `.7z` `.rar` `.dat` file, by its full name), in priority order (top wins). |

* **Mod line.** `<mod name>=<enabled>,<loadon>`: `enabled` is read with `parse_bool` (a value without a comma is `enabled` alone, `loadon` then `modded`); `loadon` is `modded`, `sd`, `hd` or `all` (a mod's game data loads in every art mode), kept as text here and interpreted by the mod manager (`docs/formats/mods.md`). Mods absent from the list are appended in name order with `1,modded`; lines whose mod no longer exists are dropped on the next save.
* The `Dump*` keys share the section with the mod lines and are never treated as mod names, except as noted in section 5. `DumpTextures` and `ReplaceTextures` are skipped on load and dropped on save.

### 2.5 `[Enhancements]`

Opt-in improvements, independent of `Ps1Res`; each is also a checkbox in the Graphics tab.

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `SmoothWaterLine` | 1 | bool | Submerged actor parts are cut at the exact water plane, not per whole triangle. |
| `BattleShadows` | 1 | bool | Smooth per-vertex floor darkening with distance in battles; off = the stepped PS1 look. |
| `AutoHideCastHud` | 0 | bool | Hide the battle HUD, menus and windows during element, tech and summon casts (as pad Select held). |
| `RetainBattleScenery` | 0 | bool | Keep the battle stage's intro-only scenery layer for the whole battle. |
| `SmoothEffectRings` | 1 | bool | Ring and tube shaped effect meshes get extra segments. |
| `SmoothReflections` | 1 | bool | Environment-mapped model parts slide their reflection sub-texel instead of in whole texels. |
| `Lighting` | 0 | bool | Key light on field and battle characters, and bright spell effects light the battle units and stage. |
| `EdgeBlend` | 0 | bool | Cross-fade the faces along texture seams of the battle stage and title meshes. |

### 2.6 `[Audio]`

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `Master` | 1.00 | 0..1 | Overall gain (music, sound effects and voices). |
| `Music` | 1.00 | 0..1 | Music bus gain. |
| `Sfx` | 1.00 | 0..1 | Sound effect bus gain. |
| `Voices` | 1 | bool | Play the voice lines of the mods' `voices` folders. |
| `VoiceVolume` | 100 | 0..100 | Voice bus gain in percent, on top of `Master`. |
| `BgmSource` | `remastered` | `remastered original` | RDE's HCA soundtrack / the PS1 AKAO sequences through the built-in synthesiser. Any other value reads as `remastered`. |

### 2.7 `[Debug]`

Developer and automation switches, read once at boot (except `Log` and `Panels`, see section 4). Keys marked "conditional" are written only when set (non-empty / non-zero).

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `Room` | empty | room name; conditional | Start in this field room instead of the title. The default debug room is `alu_a01f`. |
| `Flow` | empty | `title name slots opening field`; conditional | Start screen. Empty = `title`, or `field` when `Room` is set, a sweep or replay runs, or `Battle` is set. `field` = no flow screens. An unknown name is logged and the title is used. |
| `Seed` | 0 | min 0; conditional | Seeds every RNG (field, battle, effects) for reproducible runs; 0 = unseeded battles / effects. |
| `Screenshot` | 0 | bool | Save the first rendered frame as `screenshot.png` (`screenshot_dialogue.png` with `AutoWalk`) next to the exe. |
| `AutoWalk` | 0 | bool | Walk the player to the target NPC and press the action key at `AutoActionTick`. |
| `AutoActionTick` | 200 | int | Tick at which `AutoWalk` presses the action key. |
| `AutoTarget` | -1 | int | Entity index `AutoWalk` goes to; -1 = the nearest NPC with an `ON_ACTION` script. |
| `ScreenshotTick` | -1 | int | Tick of the screenshot; -1 = 40, or `AutoActionTick` + 90 with `AutoWalk`. |
| `ExitAfterScreenshot` | 0 | bool | Quit once the screenshot is written. |
| `Log` | 0 | bool | Write `opencross.log` next to the exe (Debug tab checkbox, hidden when logging is forced). |
| `Panels` | 0 | bool | Open the field debug panels (Debug tab) at start; F8 toggles the tab. |
| `Entrance` | 0 | int | `ROOM_PARAM` (entrance record) the debug room is entered with. |
| `AutoConfirm` | 0 | int | Press confirm every N ticks while a dialogue is open; 0 = never. |
| `AutoTeleport` | 0 | bool | Put the player next to the target at once instead of walking. |
| `SweepTicks` | 0 | int | Load every room in turn, run it for N ticks, log failures, then exit; 0 = off. |
| `SweepTalk` | 0 | int | With `SweepTicks`, also run every entity's `ON_ACTION` / `ON_COLLISION` for N ticks. |
| `ShowMessage` | -1 | int | Open this dialogue message of the debug room at tick 30; -1 = off. |
| `ShowChoice` | -1 | int | Make lines 0..N the in-window choices; -1 = plain message. |
| `ShowChoiceFirst` | 0 | int | First choice line (operand high nibble). |
| `ShowFace` | -1 | int | Force this portrait (face) id on the message; -1 = the scripts' own. |
| `Battle` | -1 | int | Start directly in the battle with this encounter scene id; -1 = off. |
| `BattleAuto` | 0 | bool | The battle plays itself (first ready PC: Attack, fierce on the first enemy). |
| `Showcase` | 0 | bool | With `Battle`: every playable character on stage, free camera on. |
| `ShowcaseStage` | -1 | int | Battlefield `bg_NN` used by `Showcase`; -1 = the scene's own. |
| `Script` | empty | file path | Route script of timed inputs and commands; the run exits when it ends. |
| `ScriptSpeed` | 2 | min 1 | Simulation ticks per rendered frame while a route script runs. |
| `Record` | empty | file path | Write the session's inputs to this file (replayable). Ignored while replaying. |
| `Replay` | empty | file path | Replay a recorded session; its room and entrance replace `Room` and `Entrance`. |

### 2.8 `[Ui]`

| Key | Default | Values / clamp | Meaning |
|---|---|---|---|
| `SettingsX` | -1 | int | Settings window x in unscaled logical units (pixels / UI scale); -1 = centred. |
| `SettingsY` | -1 | int | Settings window y; -1 = centred. |
| `SettingsW` | -1 | int | Settings window width; -1 = the default size. Clamped at open to 360..(display - 16) x UI scale. |
| `SettingsH` | -1 | int | Settings window height; -1 = default. Clamped to 240..(display - 16) x UI scale. |
| `SettingsTab` | empty | tab name | Tab shown on open; empty = General. |

Position and size are used only when both members of the pair are 0 or more.

### 2.9 `[Cheats]`

All off by default; they never lock saving. The first three also toggle from the RDE hotkeys.

| Key | Default | Meaning |
|---|---|---|
| `NoEncounters` | 0 | Ordinary field battles are won without being fought (F3, pad L3). |
| `BattleBoost` | 0 | Every living PC is refilled each update (HP to max, stamina to 70, element gauge to max), a knocked-out PC is revived, and the negative statuses are cleared (F4, pad R3). It includes everything God Mode does. |
| `AutoBattle` | 0 | The party plays the command menus itself (F5, pad L3 + R3); the results page and message windows stay manual. It keeps one PC while it can still chain Weak, Strong, Fierce on the weakest living enemy (the strongest hit above 79 % whose stamina is paid, else Weak), then hands over to the ready PC with the most stamina; a PC that cannot pay a Weak hit defends, and a party under 12 % HP with stamina under 30 runs. |
| `GodMode` | 0 | Damage to a PC is turned into "no damage" (heals still work), a knocked-out PC is revived at full HP, and the negative statuses are cleared. |
| `NoClip` | 0 | The leader ignores the walk mesh and solid entities. |
| `MaxStamina` | 0 | Living PCs' stamina is set to 70 every update. |
| `FreeElements` | 0 | Element casts cost no gauge, mark no grid cell as used and use no stack copy. |
| `AlwaysHit` | 0 | The hit chance of every PC weapon attack is 100 %. |
| `MaxMoney` | 0 | G is set to the maximum (9,999,999) on every field tick. |
| `ShopAllElements` | 0 | An element shop's buy list also gets every other element (ids 1..0x7F) that is sellable and has a price. |
| `ShopAllWeapons` | 0 | The smith's forge list of the chosen category (weapon, armour, accessory) also gets every item of that category with a price and a recipe. |
| `RunSpeed` | 0 | Field walk / run speed x2. |
| `AlwaysLoot` | 0 | Every enemy drops its rare drop (the common one when it has no rare drop), and every PC steal succeeds and takes the rare word when the enemy has one. |
| `InstantGrowth` | 0 | In a non-boss fight, the star's growth progress jumps to its cap, so each living member gets the full stat gain of that star at once. |
| `NoCraftCost` | 0 | The forge builds without needing materials or gold and takes neither. |

## 3. First-launch defaults

When no ini exists the following differ from the values an ini without those keys reads (route inis keep the struct defaults, so their frame size and framing do not change):

| Key | First launch | Key absent from an existing ini |
|---|---|---|
| `[Video] Aspect` | `16:9` | `4:3` (or the shape of the stored size) |
| `[Video] Width` x `Height` | 1280 x 720 | 1280 x 960 |
| `[Video] Widescreen` | 1 | 0 |
| `[Game] Language` | from the OS | `en` |

The language comes from the OS's preferred locales in order: the first of `en fr it de es` maps to itself, `ja` to `jp`; region suffixes (`en-US`, `pt_BR`) are ignored; with nothing supported, `en`. Everything else is the default of section 2. The `[Paths]` keys are written empty, so game and save folders are discovered on every run.

## 4. Live versus restart

The settings window applies a change as soon as it is made and saves the file in the same frame. The change flags (`OptionsChange`) and what they do:

| Key(s) | Effect |
|---|---|
| `VSync`, `Msaa`, `Anisotropy` | Applied to the GPU context at once. |
| `WindowMode`, `Width`, `Height`, `Display` | Applied at once: the window is reconfigured and the swapchain rebuilt. |
| `ArtMode`, mod lines | Art swap at once (rooms, models and caches reload); also on F6. |
| `Master`, `Music`, `Sfx`, `Voices`, `VoiceVolume` | Mixer gains re-applied at once. |
| `BgmSource` | Field and battle music switch at once. |
| `Language` | Dialogue, flow, menus, shop and results switch at once. A movie already playing keeps its language. |
| `Portraits` | Field, menu and results portraits switch at once. |
| `Driver` | Needs a restart; the Backend row offers a *Restart now* button once it differs from the one the run started with. |
| `Filter`, `FilterRooms`, `FilterEffects`, `FovDeg`, `Widescreen`, `WideHud`, `WideEdgeFade`, `Ps1Res`, `RenderScale`, `Ps1Jitter`, `Crt`, `FrameCap`, all of `[Enhancements]`, `FastMode`, `SlowMode`, `FastRate`, `SlowRate`, `AutoSave`, all of `[Cheats]` | Read every frame (or every tick) from the in-memory settings; no restart. |
| `Log` | The Debug tab starts or stops the log at once. |
| `Steam` | On: connects at once. Off: disconnects and stops achievements at once; Steam shows the game as playing until the process exits. |
| `[Paths]`, `Aspect`, `[Debug]` (other than `Log`, `Panels`), `[Ui]` | `[Paths]` and `[Debug]` are read at boot only. `Aspect` only filters the Resolution list. `[Ui]` is read each time the settings window opens. |

Editing the file by hand while the game runs has no effect: the file is read once at boot and overwritten by the next save.

## 5. Legacy keys read by `load`

| Legacy key | Read as |
|---|---|
| `[Video] Fullscreen` | `1` = `WindowMode=borderless`, used only when `WindowMode` is missing or not one of its three names. |
| `[Video] FilterHD` | `Filter`; read first, so an explicit `Filter` wins over it. |
| `[Video] WideHudEdges` | Nonzero = `WideHud=full`, used only when `WideHud` is empty. |
| `[Video] Ps1Res3D` | Default for `Ps1Res`. It is forced off when the ini has `FilterHD` but no `Filter` and `ArtMode` is not `original`. |
| `[Enhancements] EnhancedLighting`, `SpellLights` | Either on = `Lighting` default on; an explicit `Lighting` wins. |
| `[Enhancements] SeamBlend` | `0` = `EdgeBlend` off, `1` or `2` = on; an explicit `EdgeBlend` wins. |
| `[Mods] DumpTextures`, `ReplaceTextures` | Skipped (never taken as mod names) and not written back. |

None of the legacy keys is written by `save`.
