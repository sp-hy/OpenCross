# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)

## 2. Fidelity gaps (not modelled)

### A free-height actor still moves on 42 / 43 / 44 / 46 walks
Priority: P3
Impact: an actor with the free-height flag (`EF.6`, 0x40; set by 26, 3F / 40, 5B, B8 / B9, BA, cleared by 25) walks on a 42 / 43 / 44 / 46 in OpenCross; in the original's movement phase it would not move; reach in the data unknown; confidence: code reading (both sides), not captured.
Original: the actor movement phase after the scripts (`fld_move_actor_collide` and its caller) skips the heading / velocity step for any actor with `(0x38 & 0x50) != 0`, so the velocity a 42 / 43 / 44 / 46 sets moves nobody while 0x40 is set (the record still waits for arrival).
OpenCross: `FieldWorld::step_motions` (`src/game/field_world_sim.cpp`, `y_valid = flags38 & 0x40`) moves a free-height actor straight to its target on those walks (`free_y`, `move_xz` writing the position directly, the `!motion.direct && !y_valid` branch).
Repro: scan the room scripts for an entity that runs 42 / 43 / 44 / 46 while its 0x40 is set (after 26 / 3F / 40 / 5B / B8 / B9 / BA without a 25 in between); if one exists, `ps1ref.Session` in that room, poll its 0x38 word and position during the walk, and compare with an OpenCross trace (`OC_VM_TRACE=1`, `npc_trace`-style dump).
Done when: the scan finds no reachable case (drop this entry with the scan as evidence, and note the rule in the 42 / 44 / 46 rows of `docs/formats/fieldscript.md`), or the capture shows the original's behaviour and `step_motions` follows it with routes `alu_chair`, `art_swap`, `battle_hud`, `opn_wander`, `npc_trace`, `npc_walls`, `script_walk_slide` unchanged.

## 3. Open comparisons (need a capture against the PS1 or RDE)

## 4. Code hygiene

## 5. Tests and tooling
