# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)

## 2. Fidelity gaps (not modelled)

* **P3 Model flag bit 0 (anim-sequence ops 19 / 1A) is not modelled.** `src/game/actor.cpp` skips ops 19 / 1A, and `Actor::compose` / `UnitPose` always build a joint's local rotation as one ZYX matrix of rest + clip angles. On the PS1 the joint matrix builder (`sys_model_build_joint_matrices`, 0x8009AD38) does that only with the model flag bit 0 clear; with it set and no head-turn callback it builds `RotMatrixZYX(rest) x RotMatrixZYX(clip)` (`docs/formats/model.md` 6.4). The two differ only when both rest and clip rotate a joint about more than one axis. To do: count the anim sequences that run op 1A (field `.seq` and battle anmseq), and if any shipped model is posed with the flag set, keep the flag per model and compose the product there; compare one such pose against a PS1 capture. Done when the flag is modelled (or the count shows no shipped use and the doc says so).

## 3. Open comparisons (need a capture against the PS1 or RDE)


## 4. Code hygiene

## 5. Tests and tooling
