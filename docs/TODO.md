# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## 1. Bugs (behaviour)

## 2. Fidelity gaps (not modelled)

- **Unit timing around an element command's hand-over** (`src/game/battle/battle_view_sequence.cpp`, `end_group` / `tick_action`). Reproduce with `opencross ocbattleview 6 --party 0 --grid 39 --etrap 39 --keys ...` (menu-driven cast, `docs/tools.md`), `OC_BATTLE_SEED=14`, a plain config, `OC_DRAW_CAM=1`, against a PS1 menu cast of element 39 in battle scene 6 with Serge alone. The camera matches RDE within a few frames, but three unit-side differences remain. (1) The enemy's approach run lasts about 5 frames longer than RDE's, so the attack shot cuts in later; on the PS1 the unit arrives about 5 frames later than ours. (2) On the original, every unit's facing snaps home about 8 frames after the orbit camera resumes (the menu step after an element command, 0x800CD0AC, sets every unit's facing to its home byte). OpenCross turns only the actor home, at the end of its group. (3) Not checked: an element cast that hands the menu straight to another PC with no enemy action in between. Done when a per-frame table of unit positions and yaws matches RDE within a few frames for these three cases.

## 3. Open comparisons (need a capture against the PS1 or RDE)

## 4. Code hygiene

## 5. Tests and tooling
