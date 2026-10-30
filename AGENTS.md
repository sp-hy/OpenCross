# Agent guide for OpenCross

Read this first, then `docs/README.md` (the developer guide), `docs/architecture.md` (how the engine runs) and `docs/validation.md` (how behaviour is checked against the original game). Format specs are in `docs/formats/` (`config.md` is `opencross.ini`, `routes.md` the route and replay files), behaviour specs (field actors, battle, menus, achievements) in `docs/rules/`; known bugs and open issues in `docs/TODO.md` (remove an entry in the change that fixes it); `docs/tools.md` catalogues the dev tools, `OC_*` variables, hotkeys and scripts.

## What this is

OpenCross is a from-scratch C++20 engine that plays Chrono Cross from the Steam Radical Dreamers Edition (RDE) data files. It is original code under GPL-3; it contains no Square Enix code and ships no game data. Behaviour is specified from the original game and implemented fresh.

## Rules that are not negotiable

1. **Original code only.** Nothing is copied from the game, from any decompilation, or from unlicensed community tools. Read them to understand behaviour; then specify the behaviour and implement it yourself.
2. **Formats are import formats.** PS1 and RDE file formats are decoded in `src/import` and nowhere else. The engine has no PS1 concepts (no GTE, ordering tables, VRAM, Psy-Q).
3. **No per-room, per-scene, per-entity hacks.** Fix the general behaviour. If a fix only works for one room, it is wrong.
4. **Faithful by default.** Classic mode uses only the PS1's own assets; HD mode is exactly RDE's art. Improvements exist only in Modded mode or as opt-in enhancements, off by default.
5. **Never invent behaviour.** A change is backed by the original game (PS1 in DuckStation, or RDE); where the two differ, RDE is the reference. A verdict is a table or frame comparison, not an impression.
6. **Docs describe the engine and the formats, nothing else.** No decompiler names, addresses, memory offsets, external sources, history of past attempts, or references to private research material. Credits go in `CREDITS.md`. Every corrected fact lands in `docs/formats` or `docs/architecture.md` in the same change.
7. **Source comments carry no provenance.** No decompiler names, addresses, measurement notes, doc paths, dates or history. Complicated files and functions get a short header comment saying what they do.
8. **Naming.** No single-letter identifiers except `i j k` and `x y z w h u v r g b a`. No dead code, no unused includes.
9. **Every function or variable you identify while reverse engineering gets a symbol.** Add it to the matching file in `tools/ghidra/symbols/` in the same change (`name = 0xADDR;  # function|data: what it does`, snake_case with the subsystem prefix, a paraphrase of what you worked out; never a name copied from a third-party symbol list). The address itself never goes into source comments or docs.
10. **Commits are made by a human.** Agents leave changes uncommitted and report what changed.

## Workflow for a change

1. Build Debug and run the unit tests: `.\build.ps1 debug -Test`. Release: `.\build.ps1 release`. A header change can leave stale objects; when something odd happens after one, clean-rebuild.
2. Reproduce with a route (`tests/routes/run.ps1 <name>`) or a dev tool (`opencross <tool>`; `opencross tools` lists them) before touching code. The deterministic routes `alu_chair`, `art_swap`, `battle_hud` are the refactor baselines.
3. Battle gates must not regress: `opencross ocbattle fx-audit` and `opencross ocbattle pac-soak` (zero exceptions, outcome counts unchanged unless the change is meant to alter battle scripts).
4. Compare against the original game, not against memory: a save state in DuckStation, or RDE itself, next to the OpenCross capture. Automate the PS1 side with `tools/py/ps1_capture.py`, `ps1_audio.py` and `ps1ref.py` (`docs/validation.md`, section 3).
5. Update the docs in the same change. Tests for new behaviour go in `tests/` (unit) or `tests/routes/` (scripted).
6. Report faithfully: failing tests with their output, skipped steps as skipped, verified work as verified.

## Spinning up testing

- **Build and routes.** `.\build.ps1 debug -Test`; routes: `$env:OC_BUILD_DIR="build\win-x64"; tests\routes\run.ps1 <name>`; one battle in a window: `opencross ocbattleview <scene> --cast <id> --shotevery N --shotdir D`.
- **PS1 reference (DuckStation).** Once per machine: `tools\duckstation.ps1 -Setup -Settings`, copy a PS1 BIOS into `tools\duckstation\bios\`, and put your own disc images of the Japanese game (SLPS-02364 or its re-release; Disc 1 and Disc 2, `.cue`+`.bin` or `.chd`, names containing `Disc 1` / `Disc 2`) into `tools\duckstation\games\` (`OC_PS1_DISC` overrides; `Documents\DuckStation\games\` is the fallback). The English (USA) discs may sit in the same folder: they are only picked with `OC_PS1_REGION=usa` or `--disc`, and only for screenshots and states, since the RAM map applies to the Japanese executable. Then `python -X utf8 tools\py\ps1_capture.py prep` forges the memory card from `tmp\save.dat` (a copy of a save, never the real folder) and writes the base states to `tmp\ref\states\`. Captures: `ps1_capture.py scene <name>`, `ps1_capture.py cast batch|summon|tech|enemy|attack ...`, `ps1_audio.py --state ... --cast N`, `ps1_registry.py`; the OpenCross side and the diff: `tools\ours_cast34.ps1 <id>` then `tools\py\fx_diff.py`. Anything scripted goes through `ps1ref.Session` (frame-exact stepping, pad injection, screenshots, states); details and limits in `docs/validation.md` section 3.
- **Parallel runs.** One emulator per portable folder and port: copy `tools\duckstation\` to `tmp\ds_<name>\` (keep the `resources` folder; empty `savestates`, `screenshots`, `videos`), set `GDBServerPort` in its `settings.ini`, and pass `--ds-dir`/`--port`. Kill only your own emulator process, never by image name. Audio captures must run at real-time pacing (`realtime=True`), everything else unthrottled.
- **RDE itself.** HD-art and RDE-only behaviour are checked against the Steam build with `tools\rde_*.py`.
- **Disassembly.** `tools\ghidra_setup.ps1 -Ps1Exe ..\data\cdrom.dat` (a minute) or `-GameDir ..` (hours) downloads Ghidra, imports, analyses, applies the project symbol lists and exports the decompilation outside the repo.

## Practical rules

- Test windows take no focus and play no sound: set `OC_QUIET=1` (the route runner and `ocbattleview --frames` do).
- Never touch a real save folder (`Documents\My Games\...\Steam\<id>`); use forged saves under `tmp\` or `[Paths] SaveDir`.
- Unique techs are cast by their owner character in tests, not by Serge.
- Performance checks run at the settings a player uses (high resolution, render scale), not at route defaults.
- Several files have mixed line endings; a bulk `sed -i` flips them. Compare `git diff -w --stat` with `git diff --stat` before handing over, and fix flips rather than committing them.
- `tmp\` is scratch: captures, forged saves, agent work. Keep it tidy and never reference it from code or docs.
- Decompilations and disc dumps you make for yourself stay outside the repo and are never committed.

## Delegating to agents

Use cheaper models for inventories, mechanical edits and build-and-verify loops; keep design, review and behaviour verdicts with the strongest model. Give each agent the files to read, the exact scope, and the rules above. Every agent reports sizes, what it verified in `src/`, and what it was unsure about; the lead re-checks before hand-over.
