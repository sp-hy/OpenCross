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

* **P3 Darkness after a battle-script cutscene** (`src/battle/battle.cpp` `hit_chance_for`, `src/battle/pac_vm.cpp` op 2C). Darkness halves every weapon hit while the active PC (last menu PC, or the PC whose script queued an op-67 action last) is dark; op 2C clears the active PC. On the PS1 op 2C clears it (`btl_active_pc` = 0x800E0649 := 0xFF) only when no action runs and the battle menu state is 4 or below, and with no active PC the PS1 reads past the unit table; OpenCross treats "none" as not dark. Reproduce: a boss scene whose script runs op 2C, poke darkness (lo 0x20000) on the menu PC, break at the weapon hit roll and read the chance of the next enemy hit. Done when OpenCross gives the same chance or the doc records why it cannot differ.
* **P3 HP written back after a win** (`src/game/game_session.cpp`, `docs/rules/battle.md` 7.3). A forged Serge (record max HP 850, battle HP poked to 1023) ended a won fight at 1022 HP; his record then held 849 and the results screen showed 849 / 850, where the rule `hp = min(unit HP, record max HP)` gives 850. Reproduce with the stamina-status scratch script (scene 6, `bg00_menu_g5.sav`, Serge alone): win, then read the record HP. Done when the 849 is explained (results-screen step, growth, or the forged state) and the doc or code matches.
* **P3 Status screen pose and curing items through the menu** (`src/ui/menu`). The field statuses match the PS1 in play, but two parts were checked by reading code only: the status screen's 3D model hurt pose and status badges for a member with a status and low HP, and using a curing item (Antidote on a poisoned member) through the menu, its status byte change and sound. Reproduce with `tools/py/cc_save_forge.py --char ID:status=N` on both sides. Done when a PS1 screenshot and RAM read match OpenCross.

## 4. Code hygiene

## 5. Tests and tooling
