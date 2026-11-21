# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)

## 2. Fidelity gaps (not modelled)

## 3. Open comparisons (need a capture against the PS1 or RDE)

## 4. Code hygiene

## 5. Tests and tooling

- **P3: a controlled battle's restart draws different frames from a fresh process.** `src/app/app_control.cpp` (`control_restart`), `src/game/battle/battle_view.cpp` (`run_load_step`). A `[Debug] Control` run with `[Debug] Battle` restarts the standalone battle in place; the simulation matches a fresh process exactly (the route's `log` lines come at the same ticks, `campose` agrees to 0.1 units), but the frame does not: in scene 0 with `BattleParty=0,1,22`, `Seed=3` and `tests/routes/battle_harness.txt` (its `quit` line removed), `seek 1100` after one `restart` differs from a fresh process's `seek 1100` in 4070 of 307200 pixels at 640x480 (channel deltas up to 255), 3568 more after a second restart; already 6 pixels at tick 90, before any action. Two fresh processes agree byte for byte, and so does a redraw of the same tick after idle frames. Mostly the stage's foliage and the floating command title; with `stage 0` and `hud 0` about 400 pixels remain (the units). Ruled out: `std::srand` / battle RNG seeding (reseeded per load), the stage scene (fully cleared and rebuilt per load), the animation-sequence VM's shared state (`bseq::Vm::Shared`: restoring it to a fresh process's changes nothing), `frame_count_` (logging and a relative fade only). The difference grows with the number of battle loads in the process, so look for state that survives `BattleView::unload_scene` (model cache, effect library, uploaded textures, per-unit animation state). Done when `seek`, `restart`, `seek` to the same tick gives byte-identical frames, as it does for field runs.
