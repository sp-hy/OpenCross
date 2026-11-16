# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)


## 2. Fidelity gaps (not modelled)

* **P2 OpenCross achievements: finish the implementation.** `docs/rules/oc_achievements.md` is the spec: 173 achievements, rule kinds, battle events, the profile file `achievements.txt`, the `[Game] Achievements` key, the Achievements tab and the popup. Every id in it has been read from the game data (room scripts, `pgdat` participants, the name tables). Done when every row and rule kind is in `src/game/oc_achievement_table.inc` and `game::OcAchievements`, each rule kind and battle event has a unit test in `tests/`, and the tab and popup are checked in a run; then remove this entry.



## 3. Open comparisons (need a capture against the PS1 or RDE)



## 4. Code hygiene

## 5. Tests and tooling


