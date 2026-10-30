# Validating behaviour: DuckStation and Ghidra

## 1. Why

OpenCross behaviour is validated against two references:

- the original PlayStation game (the Japanese release, SLPS-02364, which is the version the Radical Dreamers Edition embeds) running in DuckStation, and
- the Radical Dreamers Edition (RDE) itself.

Where the two differ, RDE is the reference. The PS1 game is the tie-breaker for anything RDE does not touch: timing, damage numbers, script behaviour, menus, effects.

You need your own disc image of the PS1 game: a dump of a legitimately owned disc, as `.bin/.cue` or `.chd`. The repository ships none, and `data\cdrom.dat` in the RDE folder is **not** a disc image (it is a plain ZIP of the game's files). You also need a PS1 BIOS dump from a console you own; DuckStation will not boot without one. Put the images in `tools\duckstation\games\` (created by `-Setup`, git-ignored with the emulator): both discs, as `.cue`+`.bin` or `.chd`, with file names containing `Disc 1` and `Disc 2`. `OC_PS1_DISC` can point at a file or another folder instead; `Documents\DuckStation\games\` is the last fallback. The English (USA) discs may sit in the same folder. The lookup prefers the Japanese set; `OC_PS1_REGION=usa` (or `--disc`) selects the English one for text checks by screenshot or save state only: it is a different executable, so the RAM-driven stepping and the watch addresses do not apply to it.

For reading the code rather than running it, section 3 automates reference captures, sections 4 and 5 build Ghidra disassemblies, and section 6 covers the clean-room rule.

## 2. DuckStation as the behaviour reference

### 2.1 Installing with `tools\duckstation.ps1`

```powershell
.\tools\duckstation.ps1 -Setup -Settings
.\tools\duckstation.ps1 -Run -Image "D:\games\ChronoCross.cue"
.\tools\duckstation.ps1 -Run -Image "D:\games\ChronoCross.cue" -State "D:\states\field.sav" -Fullscreen
```

- `-Setup` asks the GitHub releases API for the latest stable `stenzek/duckstation` release, downloads `duckstation-windows-x64-release.zip` (about 70 MB), extracts it to `tools\duckstation\` and writes `portable.txt` so settings, save states, screenshots and the BIOS folder stay in that directory. It does nothing if the exe is already there; `-Force` reinstalls. `tools\duckstation\` is git-ignored.
- `-Settings` writes or merges `tools\duckstation\settings.ini` with the comparison and automation settings of section 2.3. Other keys in an existing file are left alone. Run it after the first launch if you want DuckStation's own defaults filled in first.
- `-Run` launches `duckstation-qt-x64-ReleaseLTCG.exe` (the script finds whichever `duckstation-qt*.exe` the zip contains) with `-batch -nogui`, `-nofullscreen` or `-fullscreen`, `-statefile <state>` when `-State` is given, and `-fastboot -- <image>` when `-Image` is given (`-State` alone is allowed).

Copy your BIOS dump into `tools\duckstation\bios\` (the script creates the folder). Without it DuckStation shows an error instead of booting.

### 2.2 Manual equivalent and command-line switches

Unzip the release anywhere, create an empty `portable.txt` next to `duckstation-qt-x64-ReleaseLTCG.exe`, put the BIOS in `bios\`, and start the exe. The switches below come from the exe's own `-help` output, which it prints to stdout when started from a console:

| Switch | Meaning |
| --- | --- |
| `-batch` | Batch mode: the exe exits after powering off. |
| `-nogui` | No main window; exits on shutdown. |
| `-fastboot` / `-slowboot` | Force fast boot (skip the BIOS intro) or slow boot for the given file. |
| `-fullscreen` / `-nofullscreen` | Enter fullscreen at start, or prevent fullscreen even if enabled in settings. |
| `-statefile <file>` | Load a save state from that file. No boot file is required with it. |
| `-state <index>` | Load save state slot `index`: per-game if a boot file is given, otherwise global. |
| `-resume` | Load the resume state (per-game if a boot file is given). |
| `-bios` | Boot into the BIOS shell. |
| `-exe <file>` | Boot a PS-EXE instead of a disc. |
| `-earlyconsole` | Create the console window early, for logging. |
| `-bigpicture` | Start the big picture UI. |
| `-version`, `-help` | Print version / usage and exit. |
| `--` | End of switches; everything after is the boot file (use it for paths with spaces). |

Notes:

- There is no `-portable` or `-settings` switch. Portable mode is the presence of `portable.txt` beside the exe, and the settings file is always `settings.ini` in the data directory (`tools\duckstation\` in portable mode).
- Whether `-statefile` combined with a disc image on one command line works in every case is (unverified); the script passes both when you give `-Image` and `-State`. If it misbehaves, run with `-State` alone: the state already references its disc.

Portable-mode layout under `tools\duckstation\`: `settings.ini`, `bios\`, `savestates\`, `screenshots\`, `memcards\`, `cache\`, `covers\`, `gamesettings\`, `inputprofiles\`, `shaders\`, `textures\`, `videos\`. These folders are created on first launch. Exact save state file names inside `savestates\` are (unverified); pick a slot in the UI and look.

### 2.3 Settings for 1:1 comparison with Classic mode

OpenCross's Classic mode uses only the PS1's own textures at the console's native resolution, so the reference must be an unenhanced render. `-Settings` writes these keys (all names were read from a `settings.ini` that DuckStation generated, enum values from the exe):

| Section / key | Value | Why |
| --- | --- | --- |
| `[GPU] Renderer` | `Software` | Bit-exact rasteriser, no hardware differences. |
| `[GPU] ResolutionScale` | `1` | Native resolution. |
| `[GPU] PGXPEnable` | `false` | No geometry precision enhancement. |
| `[GPU] TextureFilter`, `SpriteTextureFilter` | `Nearest` | No texture filtering. |
| `[GPU] DitheringMode` | `Unscaled` | Retail dithering (not true colour). |
| `[GPU] WidescreenHack` | `false` | 4:3 as shipped. |
| `[GPU] ForceVideoTiming` | `Disabled` | Console timing as the game sets it. |
| `[Display] AspectRatio` | `Auto (Game Native)` | Game's own aspect. |
| `[Display] Scaling` | `NearestInteger` | No bilinear blur when the window is scaled. |
| `[Display] ShowFPS`, `ShowFrameNumber` | `true` | Frame counter for lining up scenes. |
| `[CDROM] ReadSpeedup`, `SeekSpeedup` | `1` | Retail disc timing. |
| `[Main] EmulationSpeed` | `1` | Normal speed for manual play and audio captures. The capture driver sets `0` (unthrottled) for frame-stepped runs and `1` for free runs itself, per launch. |
| `[Main] SaveStateOnExit`, `ConfirmPowerOff`, `PauseOnFocusLoss` | `false` | Unattended runs. |
| `[Main] StartFullscreen`, `SyncToHostRefreshRate`, `SetupWizardIncomplete` | `false` | Windowed, free-running, no first-run wizard. |
| `[Main] SaveStateCompression` | `Uncompressed` | States are read by `tools\py\ps1_state.py` without a decompressor. |
| `[AutoUpdater] CheckAtStartup` | `false` | No update prompts. |
| `[Display] ScreenshotMode` | `InternalResolution` | Screenshots are the 320x240 internal frame. |
| `[CPU] ExecutionMode` | `Interpreter` | Deterministic, exact timing for frame stepping. |
| `[Debug] EnableGDBServer`, `GDBServerPort` | `true`, `2345` | The control channel of section 3. |
| `[Hotkeys] Screenshot`, `SaveSelectedSaveState`, `ToggleAudioCapture` | `F10`, `F2`, `F12` | Posted to the window by the capture tools. |
| `[MediaCapture] AudioContainer`, `AudioCodec` | `wav`, `pcm` | Audio capture is a plain WAV. |
| `[MemoryCards] Card1Type` | `PerGame` | Card file `memcards\SLPM-87395_1.mcd` (the serial of the Japanese disc). |
| `[Audio] OutputMuted` | `true` | Unattended runs are silent; capture still records. |
| `[Logging] LogToFile` | `true` | Log in `duckstation.log`. |

Interlacing: the default `[GPU] DeinterlacingMode = Progressive` is left alone. Choose `Weave` in Settings > Graphics only if you need to see retail interlacing artefacts; it is not set by the script (value spelling unverified).

`ScreenshotMode = InternalResolution` is written by the script, so `F10` screenshots are pixel-comparable with OpenCross's PS1-size captures.

### 2.4 Hotkeys: screenshots and save states

Defaults from a freshly generated `settings.ini` (`[Hotkeys]`; rebind in Settings > Hotkeys):

| Action | Default |
| --- | --- |
| Screenshot | `F10` |
| Save state to selected slot | `F2` |
| Load state from selected slot | `F1` |
| Previous / next save state slot | `F3` / `F4` |
| Pause / resume | `Space` |
| Fast forward (hold) | `Tab` |
| Toggle fullscreen | `F11` |
| Pause menu | `Esc` |

There is also a "Frame Step" hotkey, unbound by default; bind it in Settings > Hotkeys to advance one frame while paused. Screenshots go to `screenshots\` and save states to `savestates\` under `tools\duckstation\`.

### 2.5 Recipe: compare one scene

1. Boot the disc once with `-Run -Image ...`, play to the scene, and press `F2` to save a state. Keep the state files outside the repo (they contain game data).
2. Reload that state with `-Run -Image ... -State <file>` whenever you need the identical starting point.
3. Pause (`Space`), then step with the Frame Step hotkey to the frame you care about (the frame counter helps). Press `F10`.
4. In OpenCross, run the equivalent route from `tests\routes` (`tests\routes\run.ps1 <name>`; see [README.md](README.md#routes)) in Classic mode; its screenshots land in `tests\routes\shots\`. Put the two images side by side and compare layout, palette, positions and animation frame.
5. For anything that is not a picture (damage, timers, script order), record numbers from both sides and compare those; a screenshot only proves what was on screen.

## 3. Automated reference captures

### 3.1 Mechanism

DuckStation's built-in GDB server (`[Debug] EnableGDBServer`) gives memory access, breakpoints and pause / continue; a breakpoint on the game's per-frame pad read turns that into frame-exact stepping, and writing the pad buffer at that stop injects controller input for exactly one game frame. Screenshots and save states are DuckStation's own hotkeys, posted to its window as key messages, so they are taken while paused and are frame-exact; audio capture is wall-clock and therefore only works on free runs (no breakpoint stops, no input).

### 3.2 Prerequisites

- `tools\duckstation.ps1 -Setup -Settings` (section 2.1) and a BIOS in `tools\duckstation\bios\`. `-Settings` writes every key the tools need (section 2.3: GDB server on port 2345, Interpreter, uncompressed states, `F10` / `F2` / `F12`, WAV audio capture, `PerGame` card 1, muted output).
- The disc images in `tools\duckstation\games\` (section 1), found by `ps1ref.default_disc()` / `ps1ref.disc2()`, or given with `--disc` or `OC_PS1_DISC`. Scenes after the disc change need the disc 2 image present.
- Python 3.13 with `numpy`, `PIL` and `zstandard`. Run the drivers as `python -X utf8` (the emulator's window title is Japanese). Keep the emulator window restored, not minimised, or screenshots fail; it takes no focus.
- A forged memory card: `tools\py\cc_save_forge.py <save.dat> <out> ... --mcd tools\duckstation\memcards\SLPM-87395_1.mcd` (`--slot N` picks the slot to rewrite; `--mcd-only` skips the `save.dat` output) writes a raw 128 KiB card image. Start from a copy of a save under `tmp\`, never a real save folder. `ps1_capture.py prep` does this for you.

### 3.3 The library

`tools\py\ps1ref.py` holds `Session`, which launches DuckStation, attaches and stops at the first frame hook:

```python
from ps1ref import Session
with Session(disc, state="tmp/ref/states/bg00_menu.sav") as g:  # state is optional
    g.run(120)                                   # 120 game frames, no input
    g.press("Circle", hold=12)                   # Cross Circle Triangle Square Up Down Left Right Start Select L1 R1 L2 R2 L3 R3 ("A+B" = both)
    g.run_until(lambda w: w["btl_frame"] > 60, 600)   # w = g.watch(), the named RAM table
    g.screenshot("tmp/x/frame.png"); g.save_state("tmp/x/state.sav")
```

`g.step(held)`, `g.read` / `g.write` / `g.poke` and `g.add_breakpoint(addr)` (override `on_stop(pc)`; `g.gdb.regs()` gives the registers) cover the rest. `python -X utf8 tools\py\ps1ref.py boot --frames 600 --press Start:12@300 --shot-every 60 --out tmp\x` runs one scripted session from the command line.

### 3.4 Drivers and outputs

| Tool | Example | Output |
| --- | --- | --- |
| `ps1_capture.py prep` | `python -X utf8 tools\py\ps1_capture.py prep --save tmp\save.dat` | Forges the card, drives the title screen and Load menu, writes base states `tmp\ref\states\{field53_encounter,bg00_prestart,bg00_menu}.sav` and a manifest of the frame of each step. Run once before `scene` / `cast`. `--disc 2` (or `--disc2`) makes the disc 2 set (`*_d2.sav`, its own per-game card). |
| `ps1_capture.py scene` | `python -X utf8 tools\py\ps1_capture.py scene serge_attack --state-every 5` | `tmp\ref\<scene>\{frames,states,manifest.txt}` (named scenes: element casts, `serge_attack`, `bg00_zen0`, `bg21_cam4`, `golem`, `frogprince`). |
| `ps1_capture.py cast` | `python -X utf8 tools\py\ps1_capture.py cast summon --ids 34` | `tmp\r34\<id>\ref\{states,frames,manifest.txt}` for a battle effect (modes `batch`, `summon`, `tech`, `enemy`, `attack`). |
| `ps1_audio.py` | `python -X utf8 tools\py\ps1_audio.py --state tmp\ref\states\bg00_menu.sav --out tmp\x2c\fireball --cast 5 --frames 1100` | `audio.wav` (44.1 kHz stereo; `audio.raw` is the same samples headerless), `trace.txt` (every sound request with its frame), `action.sav`, `manifest.txt`. A stepped pass does the input and logs requests; a free run from the saved action state records the audio. |
| `ps1_registry.py` | `python -X utf8 tools\py\ps1_registry.py --frames 200 --every 40 --out tmp\x\reg 34-36` | `<out>\<scene>.txt`: the effect resource list of a battle's first frames, one block per sample. |
| `ps1_state.py` | `python -X utf8 tools\py\ps1_state.py watch tmp\ref\states` | Reads saved states without the emulator: `info watch extract hex poke regs window manifest prune campick harvest`. `poke` writes a modified, uncompressed state that `-statefile` loads. |

### 3.5 The compare loop

1. Capture the reference (`ps1_capture.py cast ...`, or `ps1_audio.py` for sound).
2. Run the OpenCross side: `tools\ours_cast34.ps1 <id>` runs `ocbattleview` headless with deterministic ticks for the length of the capture in refreshes, writes `tmp\r34\<id>\ours\` and then runs `tools\py\fx_diff.py`, which extracts the reference states into `ref\tables.json` and writes `diff.md` (a verdict block, per-frame differences) and `shots.png` beside them. The PS1 battle advances one game frame (one capture frame) per 2-3 refreshes, so every reference state carries the battle refresh counter (`bf` in `tables.json`) and `fx_diff.py` aligns on it: PS1 time = counter minus the first state's counter, compared 1:1 with our ticks (our tick = PS1 time + offset); `diff.md` lists times as `R<refresh>`. `tools\ours_cast.ps1 -Out tmp\x2c\heal -Cast 88` is the audio counterpart, writing `ours.log` and `ours.csv`.
3. For sound, build the reference event list with `python -X utf8 tools\py\sfx_cmp2.py ps1 <dir>`, then `sfx_cmp2.py cmp <ps1 events.csv> <ours events.csv>` lists missing, extra and mistimed sounds.
4. Fix the engine, rerun step 2, and read the verdict tables; a screenshot only proves what was on screen.

### 3.6 Speed and limits

- About 45 game frames per second unthrottled (the stop / continue round trip dominates; 12 at real-time pacing): 3000 frames take about a minute.
- One emulator per portable folder and port. For parallel runs copy `tools\duckstation\` (without the contents of `savestates\`, `screenshots\`, `videos\`) to another folder, change `GDBServerPort` in its `settings.ini`, and pass `Session(ds_dir=..., port=...)` (the drivers take `--ds-dir` and `--port`). Kill only your own emulator process, never by image name; `Session` closes the one it started.
- Guest RAM is 2 MiB; the audio capture is wall-clock, so audio needs a free run from a saved state.
- HD-art comparisons are not covered by these tools: they use the Steam RDE itself and the `tools\rde_*.py` scripts.

### 3.7 Driving RDE with a forged or copied save

RDE reads and writes the save archive through one path string and keeps memory card 1 as an in-memory image (`rde_internals.md` section 6). `tools\py\rde_save_redirect.py` launches `CHRONOCROSS.exe`, waits for the title screen, rewrites that string to a file you name and can swap the card 1 image, so Continue lists the slots of your file and every write (auto-save, save point) goes to your file:

```
python -X utf8 tools\py\rde_save_redirect.py --save tmp\work\save.dat --title-wait 30 ^
    --keys "enter,w7,hold:down:0.3,w1.5,hold:down:0.3,w1.5,hold:enter:0.3,w4,hold:enter:0.3,w6" --shot slots.png
```

The sequence presses "any button", moves the cursor to Continue and confirms, then confirms the vibration prompt and ends on the slot list. Key taps are shorter than the game's input poll, so use `hold:KEY:SECONDS`; the first cursor key after the window gets focus is dropped, hence the two Down presses. Without `--keep-open` the tool kills the process it started and only that one.

Safety rule: the tool refuses a `--save` inside `Documents\My Games`. Always pass a copy (or a file made with `tools\py\cc_save_forge.py`) and compare its hash afterwards if it must stay untouched; never save from RDE over a real file. Run only one RDE at a time. `tools\rde_snap.py`, `rde_key.py` and `rde_capture.py` then work on the running window as usual.

For scripted checks use `tools\py\rde_bg.py`: RDE runs behind every other window and never takes focus, input goes into the running game's memory instead of through the keyboard, frames come from window capture, and what you type does not reach the game. Only one RDE runs at a time. The game files are not changed.

## 4. Ghidra for the RDE exe

### 4.1 What you are looking at

`CHRONOCROSS.exe` runs the PS1 game (the JP SLPS-02364 binaries stored in `cdrom.dat`) on a MIPS interpreter plus some recompiled functions. About two thousand hooks map a PS1 program counter to an x86 handler; those handlers are where RDE's own features (HD textures, widescreen, quality-of-life toggles) live, so most "what did RDE change here" questions end in a hook. [`rde_internals.md`](rde_internals.md) explains the runtime (interpreter, guest RAM, hooks, guards, overlays, file input, saves) for someone reading their own import; the hook catalogue is `tools\ghidra\symbols\rde_hooks.csv`.

### 4.2 Importing and exporting

Requirements: Ghidra 11.3.2 or newer and a Temurin JDK 21 (the helper script below downloads the latest Ghidra release and JDK 21). Version covered by the symbol list: RDE exe v1.0.1.0, an x86 PE.

1. Import `CHRONOCROSS.exe` from the game folder (File > Import File; the format and language are detected as x86 PE).
2. Run auto-analysis with the default analyzers. This takes a long time (hours); let it finish.
3. Export with `tools\ghidra\ExportAll.java` (Script Manager, or headless below). It writes `decomp.c`, `strings.txt`, `imports.txt` and `functions.txt`.

Headless, from PowerShell (cmd.exe breaks on paths containing `(x86)`, and Ghidra 11+ fails to load scripts from a path with spaces, so copy the scripts to a clean folder such as `C:\ghidra_scripts`):

```powershell
$env:JAVA_HOME = "C:\path\to\jdk-21"
Copy-Item tools\ghidra\*.java C:\ghidra_scripts\
& "C:\path\to\ghidra\support\analyzeHeadless.bat" C:\work\proj rde `
    -import C:\work\CHRONOCROSS.exe -overwrite -scriptPath C:\ghidra_scripts `
    -postScript ExportAll.java C:\work\out
```

To resolve individual hook handlers later, use `RdeDecompTargets.java <targets.txt> <out.c>` against the saved project with `-process CHRONOCROSS.exe -noanalysis`.

### 4.3 Project symbol list

`tools\ghidra\symbols\chronocross_1010.txt` holds the names this project has established for the exe, one `name = 0xADDR;  # function|data|label: description` per line: the hook registrar (`register_hook`), the resume entry (`guest_resume`), the master installer (`install_all_hooks`), the sub-installers (`install_hooks_<addr>`) and one `hook_<category>_<guest pc>_<mode>` name per hook handler, replaced by a descriptive name where the handler's role is known (the hook pc stays in the description). Library and compiler-generated names are left to Ghidra and are deliberately not shipped. Apply it to an imported program with `tools\ghidra\ApplySymbols.java <file>` (creates missing functions, names them, and prints counts), or let `ghidra_setup.ps1` do it (see 4.4). `tools\ghidra\DumpSymbols.java <outdir>` writes every symbol of a program as `address,name,kind,source,namespace` so that user-defined names can be harvested back into the list. The file is maintained by the project and grows as more functions are identified.

The PS1 programs have lists of their own in the same folder: `ps1_main.txt` (main exe), `ps1_field.txt`, `ps1_battle.txt`, `ps1_btlinit.txt`, `ps1_btlexit.txt`, `ps1_menu.txt` (menu overlays) and `ps1_effects_<overlay>.txt` (one per effect overlay). Apply each only to the matching program, imported raw with the Binary loader and `MIPS:LE:32:default` at the load base `overlays.csv` gives; `tools\ghidra\symbols\README.md` has the table of lists and load bases and the command.

### 4.4 One command

`tools\ghidra_setup.ps1` does all of it, including downloading Ghidra and JDK 21 into `tools\ghidra\work\` (git-ignored):

```powershell
.\tools\ghidra_setup.ps1 -GameDir "C:\path\to\CHRONO CROSS  THE RADICAL DREAMERS EDITION" -OutDir D:\private\decomp
```

It stages everything in a folder free of spaces and parentheses, imports and analyses the exe, runs `ExportAll.java`, and copies the result to `<OutDir>\rde`. Expect hours. The symbol list from 4.3 is applied before the export, so `decomp.c` and `functions.txt` carry the names; `-Symbols <file>` substitutes another list. The PS1 main exe and overlays can be imported the same way with `-Ps1Exe <cdrom.dat> [-Overlays field,battle,darkfeit]` (see section 5). Scripts are described in [`tools/ghidra/README.md`](../tools/ghidra/README.md).

## 5. Ghidra for the PS1 game code

The PS1 binaries are plain files inside `data\cdrom.dat` (a ZIP). The main executable is `prog/slps_023.64`.

1. Extract it, for example with Python (`python -I`): open `data\cdrom.dat` with `zipfile`, read `prog/slps_023.64`.
2. Strip the first 2 KB (the PS-EXE header). Bytes `0x10..0x13` of the header hold the entry PC.
3. Import the remainder with the **Binary** loader, language `MIPS:LE:32:default`, at the main exe's `load_base` from `tools\ghidra\symbols\overlays.csv` (headless: `-loader BinaryLoader -processor MIPS:LE:32:default -loader-baseAddr <load_base>`).
4. Analyse. The Binary loader defines no entry point, so also run `tools\ghidra\Ps1Seed.java <entry PC>`, which creates functions at the entry point and every `jal` target.
5. Optionally apply a symbol list of `name = 0xADDR;` lines with `tools\ghidra\ApplySymbols.java <symbols.txt>` (or `Ps1MainExport.java <symbols.txt> <outdir>`, which also writes one C file per function). The main exe list is `tools\ghidra\symbols\ps1_main.txt`, applied automatically by `ghidra_setup.ps1`; the overlay lists are in section 4.3.

The overlays in `prog/*.prg` are raw binaries; import each one on its own with the Binary loader at its `load_base` from `overlays.csv` (the mode overlays `field`, `battle`, `title`, `miyatest`; the menu overlays; `btlexit`; `btlinit`, `playinit` and the sub-menus; and about 93 small battle-effect / event overlays such as `11boru`, `2boll`, `darkfeit`). Overlays that share a load base are alternatives: only one is resident at a time, so each is its own Ghidra program.

One command for the main exe:

```powershell
.\tools\ghidra_setup.ps1 -Ps1Exe "C:\path\to\game\data\cdrom.dat" -OutDir D:\private\decomp
.\tools\ghidra_setup.ps1 -Ps1Exe "C:\path\to\game\data\cdrom.dat" -Symbols D:\private\symbols.txt -OutDir D:\private\decomp   # applied with ApplySymbols.java
```

`-Ps1Exe` accepts `cdrom.dat` (it extracts `prog/slps_023.64`) or an already extracted file, with or without header, and exports to `<OutDir>\ps1`. Overlays are imported by the same script:

```powershell
.\tools\ghidra_setup.ps1 -Ps1Exe "C:\path\to\game\data\cdrom.dat" -Overlays field,battle,darkfeit -OutDir D:\private\decomp
```

`-Overlays` takes names from `overlays.csv` (or `all`; it needs the `cdrom.dat` archive), imports each as its own raw program at its load base into the same project, applies the matching `ps1_<overlay>.txt` / `ps1_effects_<overlay>.txt` / `ps1_menu.txt` when one exists, and exports to `<OutDir>\ps1\<overlay>\`. The main exe is skipped unless `-IncludeMain` is added. `-SetupOnly` only downloads Ghidra and the JDK.

**Keep all of this private.** The exports are decompiled Square Enix code. Never commit them, never paste them into issues or pull requests, and keep the output directory outside the repository (`-OutDir` makes that easy; the default `tools\ghidra\work\out` is git-ignored but still inside the tree).

## 6. How this fits with OpenCross

OpenCross contains no code from either decompilation. The decompilations, like DuckStation, are instruments for understanding behaviour: you read them, work out what the original does, write that down as a specification of behaviour, and implement it fresh. If a function in the engine looks like a transcription of decompiler output, it does not belong in the tree.
