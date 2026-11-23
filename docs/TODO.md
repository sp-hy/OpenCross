# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)

* **P3. Message width of a mod element's name.** A message that names an element by a script item id from 0x1000 (`data::kScriptElementBase`, ItemWork text) sizes its window from the executable's element name table (`FieldWorld::ps1_string_width`, `src/game/field_world_dialogue.cpp`); a mod's records past that table measure 0, so the window can be too narrow for the name. Reproduce: a test mod with a `pgdat.bin` record 527 and its sysmes name, a room script that puts 0x1000 + 527 in a variable and shows `{itemwork(n)}`. Done when the width comes from the name the dialogue draws (the sysmes element name) for these ids.
* **P3. Element ids past the vanilla records in the sort and the NG+ carry.** A mod's element records (`data::mod_element`) fall into the last group of the element list's sort (`GameState::sort_elements`, `src/ui/menu/game_state.cpp`, the group by id range) and the New Game+ carry-over skips them like the ids from 0x7E up (`src/game/new_game_plus.cpp`). Done when both decide by the record (element / consumable / summon) instead of the id range.

* **P3. First charge of each colour stalls one frame.** The frame `fx_charge_begin` (`src/game/battle/battle_view_fx_cast.cpp`) starts a colour's charge pack for the first time in a battle builds that pack's GPU resources at once (`fx_gpu_for` of `charge_pack(colour)`): 25-30 ms in Release on that frame. Reproduce: `ocbattleview 0 --cast 40` with a plain `--config` ini, read the frame times in the log at the charge start. Done when the charge packs are built at battle load (or stepped like the cast pack, `fx_prefetch_step`) and no charge-start frame exceeds about 17 ms in Release.

## 2. Fidelity gaps (not modelled)

* **P2. Caster's result code for element casts.** On the PS1 the per-unit result code (control block +0x10, table `0x800E31C0`, stride 0x30; 0 hit, 1 no damage, 2 miss, 3 heal) is written into the caster's block too by the result setters (hit, heal, no damage, all-targets miss, damage from raw). `BattleView::fill_ctx` (`src/game/battle/battle_view_units.cpp`) gives the caster that code only for weapon attacks, Steal and Big Catch; every other element caster reads 0 through anmseq op 0x47. Reproduce: a heal or a resisted cast in DuckStation (`ps1_audio.py --cast N`), read the caster's word, and compare with the caster pack script's op-0x47 branches. Done when element casters get the code the PS1 writes and fx-audit and pac-soak stay unchanged (or change only where a pack branches on it, verified).

## 3. Open comparisons (need a capture against the PS1 or RDE)

* **P3. The screen after a movie that is followed by a room change.** A movie's end clears the screen fade (`FieldWorld::movie_finished`, `src/game/field_world_audio.cpp`), as a PS1 capture of jac_b20i's no-Kid branch showed (the room returns over about 45 refreshes after movie 16). Not captured: scripts that fade to black, play a movie and change room without a fade-in (des_b06f `FADE 1,#90; 7E #11; 5A #253`, the ending room), where OpenCross now shows the old room under the held last movie frame until the exit fade, and jac_b20i's with-Kid branch after `D4 #16,#8`. Done when one PS1 or RDE capture of each matches OpenCross.
* **P3. Rainbow cell of the battle element minimap in HD.** An element of every colour (Chrono Cross, element 126) draws as `menuelem_8` in HD art (`BattleView::build_element_ui`, `src/game/battle/battle_view_ui.cpp`); the PS1 draws its rainbow plate (verified), RDE HD was not captured. Reproduce: put element 126 in a grid cell of a forged save, open the element list in RDE via `tools\py\rde_bg.py`. Done when an RDE HD capture of that cell matches OpenCross HD.

## 4. Code hygiene

## 5. Tests and tooling
