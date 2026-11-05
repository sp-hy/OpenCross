# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)

### Effect models of unsteered units do not carry the unit's live position (cook / swordbox vacuum reads y 0)
Priority: P2
Impact: medium; confirmed by capture. The cook / swordbox vacuum (Mystery Menu, Sword Trick / Sword Storm) and any overlay that reads the model position of a unit it does not steer see the home y 0 while the unit's reaction script has it in the air.
Known: the PS1 vacuum task 0x801C6040 reads the model block y (+0x88 >> 12) every tick; `thr = a1 t / dur - a1 - y_M + H/2`, `tau = (y + H + y_M) dur / a1` (both signs are now right in `Vacuum::Task::step`, `src/game/effects/bespoke/model_deform.cpp`). In a `ps1_cast.py force --scene 7 --area 1 --char 15 --ids 206:0 --target 3` capture (Mystery Menu, Orcha), target 3's block y while hidden reads -751, -768, -765, -709, -521, -206, 0. OpenCross (`ocbattleview 7 --cast 206 --party 15 --field 1`) reads `model->pos[1]` = 0 every tick while the skinned vertices span y -1639..-710: `FxModel::pos` is only written when an overlay steers the model (`battle_fx.cpp` `BattleFxRun::tick` / `save_prev`; selectors use `-fu.y` for unsteered units); the toss lives in `FxUnit::y` / the view's placement. Unknown: whether the PS1 skinned vertex buffer (0x800A7E34) is world space with the lift, i.e. what the PS1 picture looks like with y_M about -750.
Reproduce: the capture above and `tools\ours_cast34.ps1 206 -Orig`; compare PS1 frames 100-130 with our ticks 455-530 (offset 209) and log `my` / `thr` in `Vacuum::Task::step`.
Done when: overlays see the live block position of unsteered units, the vacuum's `my` follows the PS1 values above, and a cropped PS1 vs OpenCross frame set of the vacuum matches; fx-audit / pac-soak stay clean. Also check scene 488's raised Beach Bums (units 4-6 at y -16 / -16 / -48 on the PS1) once they appear in OpenCross (they stay hidden for 3000 frames with no input today).

### Frozen PC turn and an all-frozen enemy side are not captured
Priority: P3
Impact: low; follows from the frozen countdown rule (`docs/rules/battle.md` 2.5 / 6.5).
Known: a frozen enemy keeps counting down, takes its turn at 0 and thaws just before its action (PS1 capture). A frozen PC at 0 gets its script turn in OpenCross, as a confused one does; that part comes from the disassembly only. With every enemy frozen, OpenCross ends the battle as a victory (frozen units fail `alive()` for the win test); not checked against the PS1.
Reproduce: freeze a PC (a forced Ice Blast from an enemy with the chance set to 100, `ps1_cast.py force`), let its countdown reach 0 and log 0x800E0662, its countdown word and its status words; then freeze every enemy of a fight and watch for the victory.
Done when: what a frozen PC does on its turn (pass / A4, or an action that thaws it) and the all-frozen outcome match the PS1.

### No-encounters re-collision interval in opa_b02f: RDE about 3.3 s, OpenCross a fixed 133 ticks
Priority: P3
Impact: low; seen in RDE.
With encounters off, entrance 1's monster in opa_b02f (entity 4, ON_COLLISION `A3 / FE 10 / D2 / WAIT 128 / STOP`) re-fires its skipped A3 while the player stands on it: RDE every 2.86-4.02 s (about 3.3 s on average, 11 skips in 37 s), OpenCross every 133 ticks (2.2 s). The spread suggests the repeat depends on the monster walking back into the player.
Reproduce: route `no_encounters` with a longer wait, counting the `cheat No encounters` log lines, against an RDE run (`rde_bg.py`, encounters-off flag set) that counts the variable 0x32 writes (reset it to 5 after each).
Done when: the interval matches within frame tolerance, or the difference is shown to be RDE pacing.

### Emitter objects are stepped in spawn order; the PS1 walks its heap newest first
Priority: P3
Impact: low; ordering only (which of several objects writes last, the order periodic spawns draw random numbers, possibly the draw order within one depth slot).
Known: the effect-object allocator cuts each block from the top of a free block while walking backwards from a roving pointer, and the step loop walks the heap in ascending address order, so on a heap without holes newer objects are stepped before older ones. Confirmed in jac_b17i RAM: `lgt0`, `lgt1`, `lgt2` sit at descending addresses 0xA0 apart. OpenCross steps `objs` in spawn order and reverses the order only for the light write (`src/game/effects/emitter_tick.cpp`).
Reproduce: read the heap order of the live objects in a PS1 state for a pack with periodic spawns, and compare which spawn draws from the random number generator first against ours.
Done when: the order rule (with holes) is specified in `docs/formats/room_effects.md` 3.2, and either the step order follows it or a capture shows the difference is never visible.

### Lynx's scythe: 4 FT3 faces drawn only on the no-glint path
Priority: P3
Impact: low; confirmed in code and data.
Of the 29 weapon packs only `battle/wepon/yamaneko.bin` and `ply_yama.bin` (Lynx) have mesh groups other than GT3 / GT4: 4 FT3 (0x24) faces in every tier mesh. On the PS1 a plain weapon that passes the eye test (`docs/formats/model.md` 7.1) uses the specular dispatcher, which draws only GT3 / GT4, so these faces are skipped; when the test fails the stock dispatcher draws them. OpenCross `import::parse_mesh` always draws FT3, and a draw can only hide whole meshes (`DrawParams::hidden_meshes`), not one group type.
Reproduce: `ocbattleview <scene> --party <Lynx id> --orig` with `OC_GLINT_TRACE=1`; compare an "on" frame with a PS1 screenshot of the same orbit frame.
Done when: the FT3 faces are hidden on frames where the test passes (keep the group type per submesh, add a draw flag), checked against a PS1 capture, and model.md 7.1 no longer says they are always drawn.

### AKAO channel start keeps the previous program's LFO rate words on the PS1
Priority: P3
Impact: low; seen in a PS1 trace.
The PS1's channel reset leaves the vibrato, tremolo and autopan rate words (and their slide steps) at whatever the channel's previous sound left (a trace showed 0x800 left over); OpenCross starts each channel at 0x40000 (`src/audio/akao_player.cpp`). This only affects an E4 / E5 / E6 rate slide that runs before B4 / B8 / BC turns the LFO on.
Reproduce: play two SFX on the same channel slot, the second starting with E4 without B4, and trace the rate word.
Done when: OpenCross keeps the slot's rate words across starts, or a data scan shows no sequence runs E4 / E5 / E6 before its LFO-on op.

### Top command rows: Attack and Element not dimmed at stamina 0; HIT% rows ignore hi status 0x20000
Priority: P3
Impact: low; confirmed in code.
0x800C56EC state 0: Attack dims when unit stamina (+0x19E) == 0; Element dims when gauge (+0x16E) < 0, stamina == 0 or no usable
cell in the first (gauge+1)*10. State 1: every row colour is ANDed with 0xFFFFFF00 when hi status (+0x1B4) has 0x20000.
OpenCross `battle_view_ui.cpp:1292` dims only Element on gauge/cells; the HIT% rows (`:1296-1301`) have no 0x20000 rule. Done when:
both rules are added and bit 0x20000's meaning is named in docs/rules/battle.md (capture of a PC opening the menu at 0 stamina).

### B2 clears EF.1 instead of EF.0 on the party entities
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 599 uses; no EF.0 reader in `src/`, so the visible effect cannot be decided from data. Impact low.
Original: B2 (0x800C2684), in both modes, clears flags38 bit 0 on every bound party entity, then sets bit 0 and the controlled entity
on the entity whose character (+0x116) equals slot 0's. OpenCross: `FieldWorld::party_override` (`src/game/field_world_party.cpp:300`)
does `flags38 &= ~0x2u` and then sets `kSelected` on slot 0, so the old leader keeps EF.0 and EF.1 (stored by op 37) is wiped.
No reader of EF.0 was found in `src/`, so nothing visible happens yet. Done when: the mask is `~script::flag38::kSelected`.

### Script ALU: DIV by zero and the RAND range
Priority: P3
Impact: low; confirmed in code.
Original: 21 DIV (0x800C0F58) treats a zero divisor as 1, so the variable keeps its value. 1E RAND (0x800C0D2C) writes `rand()` =
the 15-bit LCG (`seed * 0x41C64E6D + 0x3039`, `>> 16 & 0x7FFF`, main 0x80010774). 1F RAND_MOD (0x800C0D80) writes `(rand() * (U + 1)) >> 15`.
OpenCross: `field_vm.cpp:505` returns 0 for a zero divisor. `field_vm_ops_core.cpp:221-227` writes the raw 32-bit xorshift (which can come out negative
after the 16-bit store) and `% (U + 1)`. The `fieldscript.md` row 1F says `rand() % (U+1)`. Done when: DIV uses 1, RAND is masked to
15 bits, RAND_MOD uses the scaled multiply, the doc rows 1E/1F/21 are fixed, and there are unit tests in `tests/`.

### Pressing the action button inside talk range suppresses the touch event even when the cone fails
Priority: P3
Impact: low; confirmed in code.
Original 0x800BB530: for a circle actor, when the action press is pending, GF.1 is clear, flags38 & 8 is clear and the player is inside the
talk radius, the touch branch is skipped whatever the cone (0x545..0xABB) or modal test says. A box actor with flag 0x1000 that
fails the cone also fires nothing. OpenCross: `src/game/field_world_collide.cpp:350-362` folds the cone into `talk`, so a failed cone
with the button pressed falls through to `trigger_collision`. The same function also uses the constant 300 as the player's height
(`:336`, `:211`). The original reads the player's own +0x102 (op 8B), which defaults to 300. Done when: touch is skipped in
that case, the player height is `ent_height(player)`, and `field_actors.md` 5.2/5.3 are updated.

### A positional sound whose source entity turns inactive is killed for good
Priority: P3
Impact: low; confirmed in code.
Original 0x800B2224: for a record bound to an entity with flags38 & 0x10, it stops the voice and sets distance 0x7FFF, id 0xFF and
volume 0, so the record stays silent until C5/C8 reuses it. OpenCross `step_sound_emitters` (`src/game/field_world_effects.cpp:145-150`)
only stops the voice, so the sound comes back when the entity is shown again. Done when: that branch also sets `record.id = 0xFF` and
`record.volume = 0`.

### AKAO drum keymap note: reverb bit, persistent channel pan and envelope guard differ from the PS1
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): the reverb bit is set on 320 of the 372 drum notes songs play while the channel has no reverb (45 songs, e.g. music003, music004): the PS1 adds reverb OpenCross does not. AR bit 15: 56 notes in music003 / 004 / 005 / 007 / 012 / 050 keep the instrument's exponential attack on the PS1. Persistent pan is audible only in music013. The all-zero envelope guard never matters (no entry has an all-zero envelope, SR > 0x7F or RR > 0x1F). Impact raised to med for the reverb part.
Capture (PS1 audio against OpenCross, original-music mode): music003's drum channel soloed: fully dry in OpenCross, a clear reverb return on the PS1; this explains the -0.8 to -1.0 dB stereo-width deficit of music003 / music013 and the band offsets in music004.
PS1 `Sound_PlayKeymapNote`-equivalent at main 0x80053370: entry byte 7 bit 7 sets or clears the voice's bit in the music reverb mask
(the same mask C2 / C3 drive); the entry pan `((b7 & 0x7F) + 0x40) << 8` is written into the channel pan and persists for later notes
and pan slides; AR / SR / mode / RR are always applied (AR keeps the instrument's bit 15, SR is a full byte << 6, RR is not masked),
unless the AD / B1 / B2 override flags are set. OpenCross `note_on` (`akao_player.cpp:533-547`): reverb bit ignored, `drum_pan` per note
(reset each note), envelope applied only when one of the four bytes is non-zero, SR & 0x7F, RR & 0x1F. `docs/formats/audio.md:72`
states the guard and "reverb bit not applied" as fact.
Repro: render a drum-mode song (`opencross ... render-seq <NNN>`) against a PS1 recording (`ps1_audio.py`) of the same song with
`BgmSource=original`.
Done when: the three rules follow the PS1 (or a capture proves the guard), doc line 72 corrected.

### AKAO AD / B1 / B2 overrides are ignored in drum and patch mode
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): patch mode only, 11 songs: AD in music013, 023, 024, 043, 044, 051, 063, 071; B1 in music062, 063; B2 in music026. No drum-mode channel and no SFX uses them.
PS1: AD / B1 / B2 (0x80055140 / 0x800551E8 / 0x8005522C) set channel flags 0x1000000 / 0x8000000 / 0x10000000; the keymap and patch
zone setups keep the channel's attack / sustain rate / release rate when the flag is set; B3 (0x80054680) clears all three. OpenCross
has no such flags: `note_on` (`akao_player.cpp:541-553`) always takes the entry / zone envelope.
Done when: three override bits on `Channel`, honoured in the keymap / zone branches, cleared by B3 (and A1 / FE 14 per the PS1);
corpus scan shows whether data uses AD/B1/B2 inside drum/patch channels.

### AKAO note lengths: F0..FD with length 0, A2 and DC on ties / rests
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): F0..FD never carry length 0 in executed code (the three zeros in `battle/monster/{kagari,q_con,q_con2}/at0.prd` are padding after the program end), so only the A2 / DC part stands: A2 before a rest in 488 units and a tie in 486 (SFX-heavy; repro alu_a02i frame 2, music069); DC is used in 330 units, 14 before a rest and 17 before a tie.
PS1 `Sound_ProcessAkaoVM` 0x800535E4: an F-op with operand 0 takes the length table's first entry (192 ticks); A2 (0x800555BC) writes
the length counter directly and DC (0x800555E4) is applied before the note / tie / rest split, so ties and rests consume them; the DC
base is the last length of any note, tie or rest. OpenCross (`akao_player.cpp:774-780`): length 0 -> the A2 value or 1; A2 / DC and
`last_len` only on notes.
Done when: lengths follow the PS1 rule; corpus scan counts F-op length 0 and A2 / DC before ties / rests.
More (second audit pass): New facts: F0..FB store their length only in the length counter (+0x7A) and take the "length known" gate branch, which subtracts 2
from the STALE gate counter (+0x7C, already <= 0 at that point), so an F-op note never gets a gate key-off (on the 64 full-gate songs
the flag-0x40 rule makes this moot; it matters on music019 / 020 / 039 / 069-0..9 and in SFX). A2 / DC set both counters, so they get
`len - 2`; a length-2 A2 / DC note has gate 0, which never fires (OpenCross `akao_player.cpp:791` gives 1 tick; audio.md:79 "a length-2
note keys off after 1 tick" is wrong). In SFX a rest does not request a key-off (only music rests do) and A0 (0x80053E68 /
0x80053DFC) only clears mask bits without keying off; OpenCross `rest -> key_off` (`:803-805`) and `end_channel -> key_off` on all
channels. Data: SFX F-notes followed by a note in 9 units (`battle/effects/monster/137black`, `bre_adkr`, `leo_dash`, `kagari`,
`ktokage`, `q_con`, `q_con2`, `farga` at0), by the program end in 5 (`jibaku`, `tubo` at0, `mell` at0, `fin_06` frame 5, `ten_b07f`
frame 5). Capture: `137black` se85 on `ps1_audio.py` (does the F-op note ring to the next key-on?). Done when: the capture decides,
then F-op notes set `gate_left = 0`, SFX rest / A0 do not release, len-2 gates 0, audio.md:79 corrected.

### AKAO FE 12 balance slide advances on SFX channels; D8 / D9 do not re-pitch a held note
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): FE 12 never occurs in shipped data, so only the D8 / D9 part stands: D8 is held by a following tie in 36 units (22 songs, 13 packs, room sta_a01i frame 0) and 16 more pack sounds hold it under legato or full length; no D9 is followed by a tie. Repro: music004 or music007, or pack tech/daiti se00.
PS1 `Sound_UpdateSlidesAndDelays` 0x8004C094 skips the balance slide when called from the SFX loop (param 3 = 1, 0x80052790), and D8 /
D9 (0x800547A0 / 0x80054800) recompute the fine-tune delta from the current pitch and mark the pitch dirty. OpenCross `tick_channel`
(`akao_player.cpp:1102`) steps `bal_slide_left` on every channel; D8 / D9 (`:989-990`) only store `fine_tune` (delta computed at the
next `note_on`). Done when: balance slide gated on `channel.music`; D8 / D9 recompute `fine_delta` from `pitch_base`.

### Save-slot faces use the plain character id, not the model-based face byte
Priority: P3
Impact: low; confirmed in code.
`menu_save_write_slot` (menutest 0x801F0A04) writes each summary party byte as 0xFF for empty, else
`menu_char_weapon_model_id` (0x801D8290: record +0x2D when <= 0x31, else the id); RDE's detail-panel hook 0x801EC5E0
draws HD sprites 0x300+id from those bytes. OpenCross `set_party` writes plain ids into 0x59C and the preview 0x34
(`src/save/cc_save.cpp:92-94`) and the list reads 0x59C (`src/ui/menu/main_menu_save.cpp:22`, draw skips ids >= 44 at
:531). Player effect: saves made as Lynx-Serge (model 0x31) or with Pip in a form show the wrong face, in our list and in
RDE's for OpenCross-written saves. Done when: the preview bytes are written model-based, the list draws from head
0x34..0x36 (ids up to 0x31, 0xFF empty), `docs/formats/save.md:76` is corrected.

### Blacksmith sell pays floor(price/4) per piece instead of (price*qty)>>2
Priority: P3
Impact: low; confirmed in code.
shopmenu 0x801DC27C shows and pays `0x801DBCE4(qty) >> 2` where 0x801DBCE4 = price10 * qty * 10 (on the total, like the
element sell). OpenCross `gear_sell_unit` = price/4 and `sell_gear` adds unit*qty (`src/ui/shop/shop_logic.cpp:242-264`),
also the screen value (`shop_screen.cpp:1188`). Odd price10 loses 0.5 gold per piece (3 x price 10: PS1 7, ours 6).
Done when: sale and display use `(price*qty)>>2`; `docs/formats/battle_data.md` 11.4 and `menus.md` 8 corrected; a unit
test with price 10 x 3.

### Blacksmith Sell list omits worn gear and its refusal message
Priority: P3
Impact: low; confirmed in code.
shopmenu list builder 0x801D9FA0 (break down mode 0x20 and sell mode 0x30): pass 1 bag pieces of the category minus flag
0x20 (break down) / 0x80 (sell); pass 2 in both modes appends every available character's worn pieces of the category
not flagged 0x20. Sell OK on a worn-only piece plays 0x18 and shows message 0x32 for 60 ticks. OpenCross `gear_sell_list`
(`src/ui/shop/shop_logic.cpp:247-256`) lists bag pieces only. Note: pass 2 skips the 0x80 test, so a no-sale piece that
is both worn and in the bag becomes sellable on the PS1 (and RDE); decide with the owner whether to copy that. Done when:
worn pieces listed, refusal message shown, quirk decision recorded in menus.md 8.

### Name entry (Latin): the screen opens on the button column, not the grid
Priority: P3
Impact: low; seen in RDE.
RDE (EN, script rename FE 4A #27 reached through a patched door trigger): the Latin screen opens with "Change Name" highlighted, and the PS1 name-entry focus byte (`menu_name_focus` in `tools/ghidra/symbols/ps1_menu.txt`) reads 2; Down goes to Confirm (3), then Default (4); Up goes back. Presses in the first ~2 s after the screen appears seem to be dropped. OpenCross (`NameEntry::begin`, `src/ui/title/name_entry.cpp`) starts the Latin layout in the grid at cell (0,0), and `docs/rules/menus.md` section 9 says the same.
Reproduce: RDE through `rde_bg.py` with a door trigger patched to run FE 4A #27 (see the `mojo_name` route for the OpenCross side); read the focus byte after each key press.
Done when: the opening focus and where Left / Right go from the buttons match RDE in a table of the focus byte per key press, and menus.md section 9 is corrected.

### Entities never get `EF.14` (no 3D object) at load
Priority: P3
Impact: low; confirmed in code and with a script trace.
The PS1 sets flags38 = 0x4000 on every entity at init (`fld_actor_defaults_init`); 0E, 0F and D1 clear it. OpenCross only clears it (0F / D1 in `setup_npc` and the D1 case in `field_vm_ops_game.cpp`), and `setup_pc` never does, so a model-less entity (talk box, zone watcher) counts as a model for 3C's budget bonus, C4, AC, the head ops and `ent_not_drawn`.
Reproduce: `OC_VM_TRACE=1` on alu_a04f patched to run 3C on entity 4 (a box): the trace shows `budget 9`, the PS1 gives 8.
Done when: `kNo3dObject` is set in `FieldVM::load` and cleared in `setup_pc` as well; 3C on a box shows `budget 8`; `alu_chair`, `art_swap`, `battle_hud` and `chair_talk` are unchanged.

### Main menu input extras not in the original (equip Triangle, L1/R1 with a stand-in, extra directions)
Priority: P3
Impact: low; confirmed in code.
Decision (owner): remove all three (Equip-screen Triangle, L1/R1 on a roster stand-in, the extra directions); the prompted unequip on the member pick stays.
`menu_equip_loop` 0x801E599C has no Triangle handling (unequip-all exists only on the roster Triangle prompt); Status
(0x801DCB74) and Equip ignore L1/R1 while 0x801D8CCC (saved lead, 0x801F4628) != -1; the member pick (0x801D916C states
1/3) reads only Left/Right and the Use/Allocate choice only Up/Down. OpenCross: Triangle unequips all accessories with
no prompt (`src/ui/menu/main_menu_equip.cpp:20-23`), `cycle_member` ignores `stand_in` (`main_menu.cpp:126-131`), and
`main_menu_main.cpp:55-56,176-177` accept the other axis. Rule 5: invented input; a mis-press strips accessories.
Done when: removed (or made opt-in), menus.md sections 1 and 4 corrected.
Also check the L1 / R1 member switch on the Equip screen: on the PS1 it returns 4 from the equip loop and the screen setup then puts the slot column on the weapon with list cursor and scroll 0, while OpenCross `update_equip` keeps `eslot` and `eq_cur`. Reproduce in RDE (`rde_bg.py`): open Equip on slot 3, press R1, read 0x801F54A8 / 0x801F54AC. Done when that matches too.

### Title idle timer keeps counting during the Prompt -> Menu fade
Priority: P3
Impact: low; confirmed in code.
The `opening` room counts its idle timer in room var 2 (script variable 0x404) to 900 and leaves on Start at once. In
OpenCross the host takes Start, and `src/app/app_sim.cpp:465-466` zeroes 0x404 only when the phase (read before the update)
is Menu or FadeOut; during Prompt with `to_menu_` (22-frame hold + 32-frame fade, `title_screen.cpp`) the room script runs
on, so a press in the last second before 15 s starts the attract demo under the menu. Reproduce: a route that presses
Start at title tick ~880. Done when: the hold also covers `to_menu_`; the route reaches the menu.

### Hi status bits 29..31 expire in OpenCross, never in the original
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): elements 1..36 are the only source, applied only with a Pendragon Sigil (0xF9..0xFB; dropped by Lynx, Radius, Peppor, Solt, Dario, Dark Serge).
Original: the countdown loop of 0x800D7D20 walks lo bits 0..31 and hi bits 0..17 only (limits 0x20 / 0x12), so an inflicted hi bit 29..31 (weapon power level, set directly by the 0xF9..0xFB accessories in 0x800DBA08) is never counted down, whatever the timer table holds.
OpenCross: `kTimedHi = 0xE003FFFF` (rules.h:26) gives them a timer that `post_action` expires.
Done when: kTimedHi drops 0xE0000000 (or the expiry loop stops at hi bit 17), docs say these bits last the battle.

### Op 83 cannot clear a forced result; op 2F max-multiplicity ignores empty slots
Priority: P3
Impact: low; confirmed in code.
Original: op 83 (0x800D2A00) always writes the pending result 0x80075909 = operand - 0x80. Op 2F (0x800D14EC -> 0x800D13E8): the argument goes through the bit helper; a colour (< 0x40) counts equal ring slots, otherwise the result is the largest group of equal ring-slot values among the three, empty slots forming a group too.
OpenCross: pac_vm.cpp:728-731 stores only negative results; pac_vm.cpp:609-619 counts colours 0..5 only (ring [empty, empty, green] -> 1, PS1 2; all empty -> 0, PS1 3).
Done when: op 83 always overwrites (clears on >= 0x80) and op 2F groups all three slot values; grep the evt pacs for `2F` with a colour >= 6 to see who is affected.

### "Landed" flag not cleared by element modes 5, 6, 12, 50
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): modes 5, 12 and 50 never carry a +0x13 row; only mode 6 matters: Magnify (77) and Diminish (83) cast while a field state is active show message rows 49 / 54 in OpenCross where the original shows none. Repro: cast Diminish twice.
Original: the executor presets 0x800E3121 = 1 per target and queues the record +0x13 message only when the OR is set. Mode 5 (0x800D4A10) clears it when the side's trap already holds the id; mode 6 (0x800D4A98) when E has no colour bits and a field state is active; mode 12 (0x800D4E90) when the cast row < 2; mode 50 (0x800D6B38) always.
OpenCross: `landed` is cleared only by modes 1/7/13 (battle.cpp:1213, 1416, 1632), so modes 5/6/12/50 (battle.cpp:1392, 1355, 1402, 1350) still show the +0x13 row.
Done when: those four paths set `landed = false` under the same conditions; visible only for records with +0x13 != 0 (sweep pgdat with `tools\py\cc_tables.py`).

### HIT% shown in the menu differs from the rolled value
Priority: P3
Impact: low; confirmed in code.
Original: 0x800D7818 computes the displayed percentages as min(99, hit base x Hit%/100) minus the target's raw EVD x EVD%/100 (no gear evade, hero x125 or dizzy halving), then the chain pull, the always-hit 99 and darkness halving; the roll 0x800D9BA4 uses the full evade term. A miss (0x800E310C set) leaves the chain counter 0x800E0694 untouched.
OpenCross: one value for both (`hit_chance_for`, rules.cpp:70-93); `do_weapon_attack` zeroes `chain_hits_` on a PC miss (battle.cpp:1038).
Done when: the menu uses the display formula and a miss keeps the chain; see the chain-reset comparison entry for the rest.
More (second audit pass): New facts only: 0x800D7818 also copies the low byte of its heavy figure into 0x800E068D, which the pilfer uses while a chain runs (see the
pilfer entry). Its symbol comment and 0x800CA020's are swapped (see Symbols).

### Mono output is 1.8 dB too quiet and folds the reverb to mono (PS1: per-voice centre gain, stereo reverb return)
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured (time): needs an RDE Mono capture against OpenCross with a hard-panned effect.
Capture (PS1 audio against OpenCross, original-music mode): OpenCross mono is 1.8 dB under the PS1 on music004 and music013, and the PS1 keeps the reverb return stereo in mono mode (side / mid -10.6 dB) where OpenCross folds it to mono.
PS1 (main 0x8004CA1C / 0x8004C5A4): with the mono bit both voice gains = volume x the centre entry of the stereo pan gain table
(halfword at 0x8007255C), ignoring pan. OpenCross `Mixer` (`src/audio/mixer.cpp:493`) averages the final L and R, so hard-panned voices
are ~3 dB quieter. RDE's remastered stream path may differ.
Repro: Settings speaker = Mono on RDE and in OpenCross with a hard-panned effect; compare levels (`ps1_audio.py` for the PS1 side).
Done when: the rule matches RDE's output (or the difference is documented in `docs/formats/audio.md`).
More (second audit pass): New facts: the centre gain is `kAkaoPanGain[0x80]` = 17418 (0.531); in mono every voice, music and SFX, gets L = R = vol * 17418 >> 15
regardless of pan. A hard-panned voice (L 24576, R 276) averages to 12426 (0.379) in OpenCross: 2.9 dB under the PS1; centred voices match.

### Double-take partner term reads past the unit table (decision needed)
Priority: P3
Impact: low; seen in PS1 captures.
For Double-take (pilfer with `E` != 0) the PS1 adds the heavy-hit byte of acting-unit list entry 1, which is 0xFF in every observed cast, so it reads RAM 0x80121335 (0xFE in the room-53 battle, other values in other scenes). OpenCross adds nothing (battle_data.md mode 30 says so).
Done when: the owner rules (keep adding nothing, or emulate a fixed value), or a capture shows a cast where that slot holds a real unit.

## 2. Fidelity gaps (not modelled)

### End card `hd/map/Fin/fin.png` after the credits is not drawn
Priority: P3
Impact: low-med; needs a capture or data sweep.
Capture (RDE against OpenCross): RDE: fades a "Fin" card in at the bottom right over about 1.3 s when the ending room is re-entered after the final movie, and holds it. OpenCross has no reader for the file.
RDE: the HD init 0x5A9D20 loads `data\hd\map\Fin\fin.png` into 0x7000D4. 0x4290C0 (`hd_world_name_draw`) draws it as a second quad after the world name, with an alpha ramp at 0x7000E4 (rate 0x7000E8 / 0x7000EC, target flag 0x7000F0). The field fade hooks start and reset it: 0x800ACF20 (fade value 0x3B90 sets target 2), 0x800AD230 (counts that fade call), 0x800B2080 (starts the ramp when the count reaches the target), 0x800B3E50 / 0x800B3FB0 / 0x800AD3C0 (reset). OpenCross: no use of `fin.png`.
Settle it: an RDE capture of the end of the `true_ending` route (`rde_bg.py`) shows when the card appears and how long it ramps.
Done when: the card fades in and out on the same fade trigger, and the trigger is described in `docs/formats/movies.md` or `title_scene.md`.

### GF.19 (FE 81) and GF.13 (FE 80) have consumers: menu under input lock, key-item list off
Priority: P3
Impact: low-med; confirmed in code.
Data use (scan of the shipped data): FE 81: 8 uses as 4 set / clear pairs in mja_a04f, mja_b04f and ryu_b29i (twice), each bracketing the 128-frame wait before retrying a lost fight. FE 80: once, in inj_b03i. Repro: ryu_b29i (Lynx duel), lose and press the menu button during the wait.
Original: 0x800B6714 (RDE: native port) allows the menu when (GF.19 set or GF.1 clear) and no room jump is pending (0x800791F4 == -1).
0x800AE2CC adds the start-of-room counter 0x800E9D1A == 0, menu cooldown 0x800EA4DC == 0, post-menu pad freeze 0x800EC4FA == 0, key-item
list closed (0x800E9D18) and no message window (0x800D5EAC). 0x800AFA60 opens the menu only with that, the system gate and GF.12 clear.
In 0x800AE354, the key-item list (0x800D6AAC) only runs and freezes the field when GF.13 is clear and 0x800B6714 passes. 0x800D6AAC is the
key-item list, not a second draw layer. OpenCross: bits stored only (`field_vm_ext.cpp:455-456`), and the menu needs
`world.idle()` = `controls_free()` (`app_sim.cpp:627`, `field_world_sim.cpp:49`), which refuses under the input lock. The docs call both bits "HUD gate".
Player impact: the 8 FE 81 scenes let the player open the menu during a locked scene on PS1/RDE, but not in OpenCross. First step: list the
FE 81 / FE 80 rooms. Done when: the menu-key test uses `!GF.1 || GF.19` for the lock term (keeping window/fade/cooldown), GF.13 stops
the key-item list, and the doc names the bits.

### E2 closes a culled speaker's window and ends its script
Priority: P3
Impact: low-med; confirmed in code.
Data use (scan of the shipped data): op 62 flag 0x400 (the opt-out) occurs in 13 rooms (38 sites); about 1,373 E0 sites in NPC-owned non-ON_ACTION scripts in 165 rooms are candidates. Capture a cutscene where an NPC speaks outside the 4:3 box.
Capture (PS1 against OpenCross): PS1 (staged): when a far-off (culled) NPC speaks, its window never appears and the op after E2 never runs; with op 62 flag 0x400 the window stays open. OpenCross shows the window in both cases. Not done: which real speakers are culled, and the cull margin under widescreen.
Decision (owner): cull against the visible screen (a widescreen picture may show windows the PS1 culls).
Original 0x800CE0E8 (E2): if the running entity owns a window and its flags38 bit 15 (culled off-screen) is set, and window byte
+0x54B bit 0 is clear (persistent, set at open when the op 62 flags have 0x400), the window is closed and the current record ends
(state 0xF, id 0xFF). If the entity owns no window, E2 stores var 4, adds 0x20 to the budget and advances. OpenCross: the E2 case
(`field_vm_ops_game.cpp:442-454`) only waits while `dialogue_open()`. `fieldscript.md:353` already describes the force-close, but the
code lacks it, and the op 62 flag 0x400 is not documented. Caution: OpenCross culls with a widescreen margin
(`fieldscript.md:534`), so the decision must use the 4:3 test, or scenes end where the PS1 does not. Done when: E2 implements the
branch with the PS1 cull box, op 62 stores the persistent flag, and a sweep (`cc_fieldscript.py`) lists the E0/E2 speakers that can
leave the screen, with one PS1 capture.

### AKAO slides and LFO delays start one tick late
Priority: P3
Impact: low; confirmed in code.
Original: per channel and tick the counters decrement, the VM runs when the length reaches 0, then 0x8004C094 steps slides / delays
(0x80052458, 0x80052790), so a slide takes its first step in the tick that issued it; the tempo and reverb-depth slides step after all
channels. OpenCross `tick_channel` steps slides first, then the VM (`akao_player.cpp:1096-1134`); tempo / reverb slides at the start of
`tick_all` (`:1137-1150`). Audible only for A4 length 1 (1154 SFX uses: the pitch jump lands a tick late). Done when: reordered (and the
A4 length-1 case compared on one SFX), or documented in audio.md as accepted.

### AKAO voice-coupling ops: music C6 / C7 FM and D4 pitch sidechain are not modelled, docs call D4 unused
Priority: P3
Impact: low; confirmed in code.
Original: C6 0x80054ED0 on a music channel sets its bit in the music FM mask with no carrier condition (only SFX need flag 0x10000), so
the voice is modulated by hardware voice v-1, whatever it plays. D4 (flag 0x10) makes the channel's register pitch the previous
channel's final pitch plus its own vibrato and slide. OpenCross: `channel.fm = channel.fm_carrier` (`akao_player.cpp:955`, `:970`), never
true for music; D4 skipped (`OC_SFX_UNIMPL`). Data: C6 / C7 only in music069 channel 5; D4 in `gat_a01f` frame 7, `ryu_b18i` / `ryu_b29i`
frame 8 (second program of a pair). audio.md:119 and :272 say no data uses D4. Done when: audio.md lists the users; optional model of
D4 (channel k pitch := channel k-1 register) compared on `gat_a01f`; music FM waits on the shared voice pool entry below.

### AKAO music voice pool: shared 24 voices, free = level 0, steal = lowest level, notes can be dropped
Priority: P3
Impact: low; confirmed in code.
Original: 0x8004CF5C takes a voice whose SPU envelope level is 0 (levels refreshed each update by 0x8004D294), scanning from the FE 10
reserve index; 0x8004CEAC steals the voice with the smallest level < 0x7FFF (a voice keyed this update reads 0x7FFF), release phase or
not, with no volume weighting; if none, the note is dropped. Voice 12+k is pinned at 0x7FFF while SFX channel k is busy / suspended /
reserved, and music key-offs aimed at it are filtered (0x8004D978). OpenCross `alloc_voice` (`akao_player.cpp:469`) prefers released
voices, ranks by `level * (|gl| + |gr|)`, never drops, separate 24-voice pools for music and SFX. Only `BgmSource=original` with dense
music plus several SFX. Done when: audio.md 7.6 states the PS1 rule (level 0, min level, drop, pin), and either a shared pool is built
or the separate pools are documented as an accepted difference.

### Field battle hand-over: extra animation packs are dropped and every clip restarts on the PS1
Priority: P3
Impact: low; needs a capture or data sweep.
Original (field): before the battle snapshot 0x800DDE38, 0x800DDC5C puts the stand clip back on every entity with an extra pack (ES.12),
maps an extra-pack stand id (bit 0x80) to 1, sets the current animation to it, clears ES.12 and frees the pack; the restore 0x800DE5A0
re-submits every visible entity's current animation (clips restart at frame 0) and keeps the battle-side variable block 0x3D0..0x3FF
(already matched by `set_dopple_mask` / `set_battle_saved_vars`, field_world_party.cpp:660-688).
OpenCross: the field is never torn down; `Actor::clear_ex_clips` runs only for ops C0 / BF / C1 (field_world_entity.cpp:638).
Settle it: scan the field scripts (`tools\py\cc_fieldscript.py`) for a C1 start followed by A3 / C9 before a C0 / BF on the same entity; if
any, capture that room after the battle on RDE (`rde_bg.py`). Done when: no script reaches it (say so in architecture.md 5.6), or
`FieldWorld` drops ex clips at battle start and restarts current clips at `end_battle`.

### 46black model warp ignores the op 0x180 operands
Priority: P3
Impact: low; needs a capture or data sweep.
46black op 0x180 (0x801C5EF8) stores s16 @+8 / @+10 / @+12 (angle shift, distance shift, span) into 0x801C8E4C / 8E50 / 8E54, which the model warp 0x801C663C reads. OpenCross `Black46::command` ignores the args (`model_deform.cpp:242`) and `frame` hard-codes 4 / 5 / 112 (:280); the ground side `BlackWarp` (`ground_ports_c.cpp:202`) reads its 0x181 operands.
Done when: the shipped 46black pack's 0x180 operands are dumped (cc_effects seq dump); if they are 4 / 5 / 112 Black46 still reads them from the op (a hard-coded copy of pack data), else this becomes a bug.

### Alt+Enter fullscreen / windowed toggle is missing
Priority: P3
Impact: low; confirmed in code.
Decision (owner): add Alt+Enter; the borderless-window drag is not needed (the borderless window fills the desktop).
NEW.
RDE window proc (0x584010): `WM_SYSKEYDOWN` + VK_RETURN flips ScreenMode 0 (fullscreen) <-> 2 (windowed) and re-applies it
(same code as 0x5860D0); Alt+F4 goes to the default handler. In ScreenMode 1 (borderless) a left-button press sends
`WM_SYSCOMMAND SC_MOVE|HTCAPTION` so the window can be dragged. In fullscreen the cursor is clipped to the window on activation
and released on deactivation. OpenCross: `src/platform/window.cpp` records keys only; window mode changes only in the settings
window. Idle cursor hiding exists (`app_run.cpp`), the drag and the clip do not.
Done when: Alt+Enter toggles windowed <-> the configured fullscreen mode and saves `WindowMode` (config.md "When it is saved"
updated), and a borderless window that is smaller than the desktop can be dragged (only if OpenCross's borderless is ever not
desktop-sized; today `borderless` is desktop-size, so the drag may be moot: say so in config.md).

### FE 14 (GF.10) frame echo / capture grid is not drawn (11 rooms)
Priority: P3
Impact: low; needs a capture or data sweep.
Data use (scan of the shipped data): op 91: 3 uses (ending 0188, jac_b35e, ryu_b29i 1286), always right before a ripple. FE 12: 6 uses (opa_a04f 017F, opa_b04f); FE 13: once (jac_b16i 0A5C); FE 14: 178 uses in 71 rooms, mostly a toggle pair. Capture those rooms on RDE.
Capture (PS1 against OpenCross): PS1: GF.5 (op 91) makes no visible change, so it needs nothing. GF.10 with FE 12 = 4196 draws a zoomed echo of the frame (image sharpness 27 -> 18); OpenCross draws nothing. Real users: opa_a04f, opa_b04f, ending, ryu_b29i, jac_b16i, jac_b17i, jac_b36e, cao_a04f, gat_a01f, sta_a24i; the other ~60 FE 14 rooms hold only a debug-menu toggle.
Original: 0x800AE354 and 0x800DF118 skip the per-frame clear when GF.5 is set. Op 91 (0x800BFD54) sets it, and the ripple start
0x800B46D4 sets it too. With GF.10, 0x800AE354 draws the 5-quad capture grid (0x800DFAB8) with the FE 12 rotation and FE 13 scale. OpenCross
stores both (`fieldscript.md:269, 386-388`). Uses: op 91 3, FE 12 6, FE 13 1, FE 14 178. Capture: the 3 op-91 rooms and one FE 12 room in RDE
(`rde_bg.py`) against OpenCross. Done when: each effect is implemented or documented as invisible in RDE.

### PAC op 84 force low-HP stance is a dead event
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 5 uses; scenes 394, 419, 422, 434, 450, 484.
0x800D2A60 ORs 0x80000000 into the unit's control word (0x800E31C4 + unit*0x30; operand 0xE9 = self); the resolve pass recomputes
it from HP each action, anim scripts read it through GetStatusH (0x4A). Users: evt394 (slot 4), evt419/422/434 (slot 3, kill
scripts), evt450 (slot 1), all `84 e9 80`. OpenCross: `pac_vm.cpp:732` emits `QuarterHp`, nothing consumes it; the view derives
the bit from HP only (`battle_view_units.cpp:171`). Effect: those bosses do not take the wounded stance in their death/attacked
scripts. Done when: the event sets a per-unit override on `ctx.flags` bit 31 until the next action resolves, compared on PS1 for
one of the five scenes.

### PAC ops B0 / B1 (lift / land, 10-tick blocking tween) are no-ops
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 1 B0 and 14 B1 in scenes 399, 417, 418, 423, 428, 438, 441, 451, 452, 454, 455, 457.
0x800D15AC (B0 via 0x800D1730, B1 via 0x800D1750) arms a 10-tick height tween ((A/10) << 12 for B0, restore for B1) on the
entity's first call and blocks until the tween flag clears. B1 sits in the kill or attacked scripts of 13+ boss scenes (evt399,
417, 418, 423, 428, 438, 441, 451, 452, 454 ...); B0 only in evt399. OpenCross `pac_vm.cpp:791-792` breaks: no height change,
no 10-tick wait, so the following E0 / 67 run 10 ticks early. Done when: an Event moves the model height over 10 ticks and the VM
blocks on it; one B1 kill script compared on PS1 (what height B1 restores to must come from that capture or the tween step).

### PAC op C5 (wait for shot tag) never blocks
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): `evt386` only; C3 and C4 occur in no script.
0x800D1CC8 retries while the pool-B object with the tag lives. Only user: `evt386.pac` entity 0 slot 1 (`C5 'ev01'`, after a
`C2 ev01`). OpenCross: `pac_vm.cpp:827-832` asks `Host::effect_busy`, which `ScriptedBrain::BattleHost` never overrides (default
false, `pac_vm.h:169`). C3 / C4 occur in no script (see Rejected). Done when: C5 holds until the shot started by C2 ends
(an Event the view answers), checked in scene 386.

### PAC op 67 type 6 (forced trap fizzle) is queued as a plain cast
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): `evt478` only, shared by scenes 478 and 485.
0x800CFCE4 type 6 queues the element like 1/2/3 and sets script state +0x838 = 1; the element resolve 0x800D8F5C then forces
0x800E3122 (trap fired) = 1, clears the enemy trap id 0x800E0042 and the banner state 0x800E0062, so the cast fizzles into the
colour's trap pack. Only user: `evt478.pac` entity 5 slot 4. OpenCross `pac_vm.cpp:698` / `:1238` treat 6 as 1. Done when:
type 6 routes the cast through `Battle::catch_in_trap`'s caught path and scene 478's counter matches a PS1 capture.

### Effect timeline op 0x7A (wait for the song change) never stalls
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): only `lasdes2.prd` (sequences `main` and `msta`).
Timeline dispatcher 0x8002CE50 case 0x7A polls 0x8001AA8C (steps 0x8001B860) each tick and stalls until the new song runs.
OpenCross: `SequenceHooks::wait_cond` (`src\game\effects\sequence_player.h:42`, used `sequence_player.cpp:384`) is assigned nowhere.
Only `battle\effects\death\lasdes2.prd` (Time Devourer's end) uses it. Done when: the battle hooks set `wait_cond` to "song change
pending" from the audio player, and lasdes2's audio/visual alignment is compared with a PS1 audio capture (`ps1_audio.py`).

### Single-use anim ops without behaviour: 0xAA, 0x88/0x89, 0xAC, 0xA2-0xA5
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 0xAA x3 (shinryu); 0x88 and 0x89 x1 each (konsin); 0xAC x1 (deadc); 0xA2 x2, 0xA3 x2, 0xA4 x1, 0xA5 x2 (canon). Anim op 0x75 (GetPnum) has no use at all.
All fall to `battle_seq.cpp:738`. 0xAA (0x800B5C38, PitchDown toward a point held in variables): `tech\shinryu.prd` 3x, the
dragon's tilt is missing. 0x88 / 0x89 (0x800B628C / 0x800B6404, head look-at task on / off): `monster\konsin.prd` only. 0xAC
(0x800B6D90, model idle-pose word := w1): `death\deadc.prd` `dead` variant 7 ends with `AC 8`; `FxHooks::set_pose` /
`Visual::script_pose` already exist. 0xA2-0xA5 (0x800BA8D4 / 0x800BA914 / 0x800BA954 / 0x800BAB44, task 0x800BAA20: two rects and a
task alternating a texture window): `tech\canon.prd` only, needs a texture-window flip in engine terms. Done when: each is
implemented (0xAC and 0xAA first) and the four packs are compared with PS1 captures.

### Dialogue window geometry: auto placement ignores other windows; substituted names count as 0x30
Priority: P3
Impact: low; confirmed in code.
Original 0x800CE8A8 mode 0 asks 0x800D5DC4 (-1 no other window, 1 another window has the "above" bit 23 at +0x548, 0 otherwise).
For heading < 0x800 the window goes above for -1 or 1 and below for 0. For heading >= 0x800 it goes above only for 1. The width measure
0x800DC48C calls 0x800DC2BC for codes 7/8/0x12/0x15/0x18/0x19/0x1B and gets the real pixel width (8 for a glyph < 0x75, else 12; nested
code 8 recursive; Serge reads as the Lynx name 0x31 when char 0's model byte is 0x31 and GF.16 is clear; +0x18 for char 5 in 0x15/0x18).
OpenCross: `field_world_dialogue.cpp:270` handles only the no-other-window case. `ps1_message_width` (`:182-217`) adds a flat 0x30 by design.
Done when: both are matched for Classic PS1 text and a PS1 capture of one two-window scene and one short-name line agrees.

### Death overlays do not set the dying unit's idle-pose word 8 at init
Priority: P3
Impact: low; confirmed in code.
PS1 init writes pose word +0x146 := 8 on the model of target 0 in bossde (0x801C6268), darkfeit (0x801C6388), dorublue
(0x801C6314), dorured (0x801C635C), kroto (0x801C6360), lasdes3 (0x801C6414), sfin (0x801C6268), objcrash (0x801C62F8), and on
caster 0 in mushi (0x801C5F98); not in skadeth, lasdes1, octdeth, garadeth, ki_deth (grep for 0x146). OpenCross: the hook exists
(`FxHooks::set_pose` -> `BattleView::fx_set_pose`, `battle_view_action.cpp:412`) and only Lasdes2 (`death_ports.cpp:891-894`),
Kamedes and PoseWord call it; `Dissolve` (`death_ports.cpp:540`, table 859-867) and `GaraDeth` (`death_ports.cpp:41`, mushi) do not.
Effect: a still-running idle restarts on the dead pose as the dissolve starts; the dissolve skins whatever pose the unit holds.
Done when: a `FbParams` flag (all but skadeth) and GaraDeth(mushi) call `set_pose(unit, 8)` in init, and a `ps1_cast.py force`
capture of one F-B death (e.g. dorublue) matches the pose of the first frames.

### Overlay op 0x59 drops the loop flag and the anim-speed word
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 28 op 0x59 records in the seven packs. The loop flag is 1 in 6 records, all on clip 0xA (dorublue, dorured, dorugren, doruyelo, kroto deaths). The speed word is non-zero in 11 records (-8 .. -51, a slower clip), all on the 0x80 death clip of ato, beril (objcrash), dorublue, dorugren, kroto, rake and lasdes2, so the speed matters too.
PS1: darkfeit, dorublue, dorured, kroto, objcrash, lasdes2, lasdes3 replace stock op 0x59: on target s16@+10 they write model
+0x22 := s16@+0xC (loop) and +0x2A := s16@+0xE + 0x40 (anim speed scale; 0 freezes, cf. 44get), then SetAnim(s16@+8, 0x20). The stock
op writes only +0x22. OpenCross `death_ports.cpp:583-587` `Dissolve::stock_op` returns true after `set_anim` alone, so even the loop
flag that the stock path applies (`battle_view_fx_state.cpp:259-267`) is lost; speeds other than 0 / normal are not modelled
(`FxModel::anim_speed`, `bespoke.h:174`). Matters for unit clips (< 0x80); clips >= 0x80 are one-shot anyway.
Data sweep first: list the op 0x59 operands @+0xC / @+0xE in those seven packs (cc_effects.py / seq dump).
Done when: stock_op applies the loop flag as the stock op does and, if the sweep finds @+0xE != 0, the speed scale.

### Hell Soul (44get) does not freeze the real target
Priority: P3
Impact: low; confirmed in code.
PS1 frame 0x801C5FC0: in modes 0x180 and 0x181 it writes target 0 model +0x2A := 0 every tick (the field stays 0 between frames,
so the stock pose step is frozen too); when 0x181 runs out and at exit (0x801C632C) it writes +0x2A := 0x40 and anim speed dt<<6.
OpenCross `src/game/effects/bespoke/model_deform.cpp:29-84` (`Get44`) never sets `FxModel::anim_speed` (the host freezes on 0,
`battle_view_units.cpp:781`), so the target keeps playing its idle / flinch under the rising copy (eid 7; 15 enemy refs).
Done when: Get44 sets `anim_speed = 0` in 0x180 / 0x181 and -1 when 0x181 ends and in the destructor; a `ps1_cast.py force` capture of
Hell Soul shows the same held pose.

### Remastered-music script syncs (song 0x17 wait, room 0x1C8 transition) missing
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): not one room: 24 `WAIT #345` sites, about 20 under song 0x17; seven start the song in the same script (crn_b17i, jac_b10i, jac_b19i, jac_b30d, kur_b04i, man_a04d, tok_a01f). Map 0x1C8 is hes_a04f, hook at offset 067A. Repro: jac_b10i 030A or tok_a01f 00F1.
RDE hooks the field WAIT op (field.prg 0x800CB714, host 0x57D610): when the current song is 0x17 and the wait operand resolves to
0x159, the original countdown is bypassed and the op only advances (pc += 3) once the remastered stream position reaches 5.75 s.
Map 0x1C8, script word 0x36012B (0x800BDB38, host 0x57D770): if song 8 is not playing, fade the BGM to 0 over 30 frames, hold 500
frames, then start song 8 at volume 0 and fade to 0x7F over 45 frames. OpenCross: WAIT is a plain counter; no 0x1C8 handling
(`src/game/field_world_audio.cpp`, `src/audio/mixer.h:127` `bgm_seconds` exists). Both belong to the script-correction table above
(keyed on song + operand / room + script word), active only with the remastered soundtrack.
Repro: find the room that waits 0x159 under song 0x17 (`occ` script dump); play it on RDE and OpenCross and compare the cue timing.
Done when: both syncs are applied with remastered BGM and verified against RDE.

### Pip form change at 99 stars does not retarget stats or OR the unlock masks
Priority: P3
Impact: low; confirmed in code.
RDE snapshots Pip's form byte `0x80077B81` at main 0x800192E8 and checks it at 0x800194DC (host 0x449310 / 0x449340): when old and new
form are both 0x2B..0x30 and differ, and the star count (u16 guest 0x800789BC) == 99, Pip's max HP becomes the new form's Lv99 value
(exe table host 0x6B20DA), current HP is rebased by the same delta and clamped to 0..max, the six stats and their snapshots take the
form's values (host 0x6B20D5..0x6B20FD), and the form's mask words are ORed into Pip's unlock masks (host 0x448670, which also clears
bit 14 on Pip's owned cells). OpenCross `Battle::evolve` (`src/battle/battle.cpp:718-751`) and `game_session.cpp:199-210` write only the
kind, growth column and tech cells; `rde_playinit_end` (`src/game/new_game.cpp:152`) covers the load-time re-roll only. Also open: the
btlexit snapshot/restore pair 0x80010414 / 0x801EC93C (0x410-byte unit copied back when the character changed).
Repro: forge a 99-star save with Pip one colour point from a form change; fight; compare Pip's status page on RDE (`rde_save_redirect.py`).
Done when: the 99-star retarget runs at the form change using `lv99para.bin` per form, and the restore pair's effect is decided by capture.

### Field C6 sound slots are not faded on main-menu entry nor restarted on return
Priority: P3
Impact: low; confirmed in code.
Field 0x800DDAE4 (called from `fld_menu_overlay_launch` 0x800AFA60): for the 8 C6 slots (mask `1 << (slot+8)`) a record whose sound
has ended is cleared (id 0xFF); then all running slot sounds get their volume modifier faded to 0 over 0x20 frames. 0x800DDB74 (on
return): ends every slot sound and restarts each slot with id != 0xFF through the C6 path from the record at 0x80079228 (id, mask,
volume, pan; slides come back at their stored values). No RDE hook on either. OpenCross: the main-menu open (`src/app/app_sim.cpp:628`,
`flow.begin_menu_open_fade`) leaves field sfx alone; `field_world_audio.cpp:127-170` has no suspend/replay. `docs/formats/audio.md:228`
already mentions the replay table.
Repro: a room with a looping C6 ambient (e.g. waterfall / rain); open the main menu, close it; compare on RDE.
Done when: menu entry fades the slot sounds over 32 ticks and the return restarts live slots from their records; checked on RDE.

### miyademo stage swap side effects not reproduced
Priority: P3
Impact: low; confirmed in code.
miyademo 0x801C5E04 op 0x101: kills the old pack's objects by owner (0x801C6554), starts the new pack, plays one sound
while the swap counter < 5, starts a random 'i'-tagged sequence of the pack, restarts "miya" on the first five swaps,
and spawns a "fill" object from the old pack's script head. OpenCross `Miyademo` (`src/game/effects/bespoke/scene_remap.cpp:109-140`)
only swaps the stage and restarts "miya". Also check the 9 halfwords of the background table in miyademo.prg against
`kBg` (+0x12 pack bias). Done when: sound, 'i' sequence and fill object ported, or a `ps1_cast.py force` capture shows
them absent; the table bytes confirmed.

### Dopplegang creature forms are not scaled by stars
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 71 forms (ids >= 0x40 with +0xA6 bit 7 clear), 70 with non-zero +0xA7 selectors; the extra is stars - threshold (up to ATK +6, DEF +12, MAG +24 for a 0xFF selector). Repro: Scorpiod 65 (threshold 0x0D) at 40+ stars.
Original: the swap machine 0x800C8CCC calls 0x800C8414 for forms with record id >= 0x40: extra = current stars (0x800789BE) - (record +0xA6 & 0x7F); if positive, ATK / DEF / MAG / MDEF each add extra >> 4 / >> 3 / >> 2 (2-bit selectors in +0xA7, 0 = none) via 0x800C8354, capped 255.
OpenCross: `Battle::transform` (battle.cpp:677-714) loads the record unscaled; battle_data.md lists +0xA7 as unknown.
Done when: transform applies the scaling; +0xA7 documented.

### RDE battle fixes: battle 0x1C2 script condition, element 0xC9 end message, unit +0x13E term
Priority: P3
Impact: low; needs a capture or data sweep.
Decision (owner): implement through the same data-driven RDE corrections table as the field fixes.
(a) Battle 0x1C2 (450): battle.prg 0x800CFC68 arms a flag when the script byte after the op is 'Y', 0x800CFCAC disarms it on the word
pair E3047C76 / 60500000, and while armed 0x800CEE84 makes the PAC condition whose next word is 9C01067E return 0 (reset at 0x800BE500).
(b) In `btl_resolve_element_action` (0x800D8F5C) 0x800D93FC uses end-of-action message row 0x2F for element id 0xC9 (u16 0x800E3556)
instead of record byte +0x13, and 0x800D8F90 forces the halfword loaded from +0x13E to 0 (it feeds the call at the function start;
meaning not decoded). OpenCross: `src/battle/pac_vm.cpp` runs the script unmodified; `src/battle/battle.cpp:1710` uses `element.b(0x13)`.
Repro: `opencross ocbattleview 450` (plain ini, mods off) vs the same battle on RDE; a cast of element 201 on RDE (`rde_cast.py`).
Done when: (a) is a data-keyed PAC correction (battle id + byte pattern), (b) the 201 row is in the message rule, (c) the +0x13E term is
decoded, each with an RDE capture showing equal actions / messages.

### Scene-specific HD effect sheets `battle/shine/renz0.tim` (scene 0x29B) and `battle/canon/face.tim` (scene 0x568, HD only)
Priority: P3
Impact: low; needs a capture or data sweep.
RDE hook 0x800B122C (0x436E10): when the battle scene id (u16 at 0x80085A20) is 0x29B it uploads `renz0.tim` over the `renz` page (0x2C0,0x180); for 0x568 in HD it uploads `face.tim` over (0x2C0,0x100). OpenCross draws the 1x pack data (`src/game/effects/bespoke/screen_ports.cpp:50`, `battle_view_pseudo.cpp:33`). Next step: identify both scenes (the number `ocbattleview` takes), then compare an RDE frame. Done when: these scenes draw the HD sheet at its 4x rect in HD (and `renz0` in classic too if RDE does, since that hook is not HD-gated).

### SPU output: small measured differences (resampler roll-off, reverb headroom, depth at song load, stale reverb after a pause)
Priority: P3
Impact: low; measured, no audible effect found.
The SPU reimplementation otherwise matches the PS1 within about 0.2 dB (all 10 reverb presets and the work-area table identical register for register; songs, jingles and dry SFX within 0.1-0.2 dB; reverb tails equal to -90 dBFS; the reverb mode is always 4 and rooms differ only in their song's depth 0x2500..0x3C00). Remaining: the 44.1 -> 48 kHz resampler loses 1 to 1.7 dB at 14-20 kHz (`mixer.cpp`); the reverb input is scaled to half and back, 6 dB more headroom than the PS1; the PS1 zeroes the reverb depth at song load; the reverb unit resumes with stale content after a 4 s pause; `render-sfx` runs at the boot depth 0x3FFF where a PS1 room uses its song's depth (reverb SFX up to 1.6 dB louder in that tool only). Done when: each is either matched or noted in `docs/formats/audio.md` as accepted.

## 3. Open comparisons (need a capture against the PS1 or RDE)

### Staff roll skip: the credits strip vanishes at once instead of fading with the movie panel
Priority: P3
Impact: low; only when the player skips the ending.
Known: disc 2 rows 11 / 12 (`ending_s`) draw the movie in a panel at the lower right and scroll the credits strip (`hd/staff/staff24us.png` / `staff24.png`) on the left (`src/movie/player.cpp`). On a skip mid-roll `Player::draw_held` cross-fades only the panel; the strip disappears at once. At a natural end the strip has already scrolled out, so nothing shows. RDE's skip behaviour was not captured.
Repro: route `tests/routes/staff_roll` plus a confirm press about 600 ticks in; RDE with a forged save (slot 2, room 9, entrance 1, var 0x42 = 1), Continue, skip at the same frame, through `tools\py\rde_bg.py`.
Done when: the strip follows RDE on a skip (fades with the panel without darkening the scene behind it, or whatever RDE does) and `docs/formats/movies.md` says so.

### AKAO music: vibrato restart at key-on and the rest freeze have no PS1 capture
Priority: P3
Impact: low; the effect side is captured and matches.
Known: every key-on restarts the vibrato / tremolo waves (wave start, step 1, fresh delay), and a music channel stops sending volume / pan / pitch / LFO to its voice from a rest to the next note (`src/audio/akao_player.cpp`). Both are checked on the PS1 for sound effects only: the capture helper plays an effect from the frame hook by a GDB call of the play-effect command, but no songs. Music rests on the code reading and `test_note_restarts` (`tests/test_audio.cpp`).
Repro: record music004 or music012 (B4 delay 38) on the PS1 with a per-update register trace of the music channels (pitch, volume, update mask), e.g. by changing the field song from the frame hook, and compare against `opencross ocaudio render-seq` with `OC_AUDIO_PROBE`.
Done when: the vibrato onset after each key-on and the frozen tail after a rest match the trace.

### Scripted PC weapon turns: poison / timer step not checked against the PS1
Priority: P3
Impact: low; confirmed only for elements.
Known: the post-action upkeep runs only for PC-led actions and Defend. A queued PC element (forced action queue, scene 6 from `tmp\ref\states\bg00_menu.sav`) steps poison by 1 on the PS1, a menu element by 7; OpenCross matches both (`Battle::exec_action`, `src/battle/battle.cpp`). A queued PC weapon entry (confused PC script, Pip's form turn) still steps s+1 / s+1 without a reference.
Repro: queue a weapon entry for unit 0 the way `tools\py\ps1_cast.py force` queues an element, with poison set on every unit (unit +0xB0 bit 0), and log every unit's HP per frame; OpenCross side `opencross ocbattle sim 6 1 --party 0 --attack --poison`.
Done when: the step matches the capture and `docs/rules/battle.md` 2.4 says so.

### Revive at 0 HP: OpenCross floors the revived HP at 1
Priority: P3
Impact: low; rare (very low max HP).
Known: element mode 4 sets HP = max HP x pct / 100, which is 0 when max HP < 20 at 5 % or < 7 at 15 %. On the PS1 the handler writes HP 0 with KO cleared, then the HP pass runs; whether the unit is knocked out again or stays up at 0 HP is unknown. OpenCross keeps `max(1, ...)` in the mode 4 case of `Battle::do_element` (`src/battle/battle.cpp`).
Repro: `python -X utf8 tools\py\ps1_cast.py force` of Revive 94 on a KO'd unit whose max HP is poked under 20, against `tools\ours_cast34.ps1`.
Done when: the floor matches the capture and `docs/formats/battle_data.md` 5.2 mode 4 says so.

### A consumable stack left at 0 copies after a fight: greyed or hidden in the next battle
Priority: P3
Impact: low; behaviour chosen without a capture.
Known: after a battle the party write-back restocks every stack cell below 5 copies from the element stock; a cell with no spares keeps its element at 0 copies (PS1 capture: Capsule cell 0 copies, id kept). OpenCross `party_member_from_state` (`src/game/game_session.cpp`) passes the real count and marks a 0-count stack cell not unlocked, so it is unusable in the next fight. How the PS1 battle menu shows that cell (greyed, hidden, or usable once) was not captured.
Repro: on the PS1, use every Tablet of a 5-stack with no spares in the bag, win, then open the element menu in the next battle (`ps1ref.Session` on a battle prestart state from `tmp\ref\states`); same with OpenCross.
Done when: the cell's look and usability match the capture and `docs/rules/battle.md` section 10 says so.

### Save/Load screen plays sounds 0x19 and 0x76 when its fade-in ends; RDE stops the remastered BGM around 0x76
Priority: P3
Impact: low-med; needs a capture or data sweep.
PS1 menutest `menu_save_load_driver` 0x801EC948: in two states, once the screen alpha reaches 0xFF, it plays sound 0x19 (mask 0x20) and 0x76 (mask 0x40)
through 0x801D86C8. RDE hooks that call (return address 0x801EE310) and 0x8004A1F8 in overlay 10 ("jingle channel stops PC BGM", `rde_hooks.csv`).
OpenCross: `main_menu_save.cpp` plays only cursor sounds (1, 6, 8, 0x18); nothing plays 0x19 / 0x76 or stops the BGM.
Unknown: what 0x76 is, which entry (in-game Save, Load, title Continue) reaches the two sites, and whether the field song comes back.
Settle with `rde_bg.py` (`OC_AUDIO_DUMP`-style audio capture of RDE opening Save / Load over a field song, remastered and original) and a `ps1_audio.py`
capture of the same. Done when: OpenCross plays the same sounds and BGM handling, documented in `docs/rules/menus.md`.

### chip2 per-map fixups RDE hard-codes (tel_b12i depth, you_b11f fill, zel_a17i and man_a04d classic paths)
Priority: P3
Impact: low-med; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured: tel_b12i frames identical but no actor stood in the remapped depth band; you_b11f did not reach the same script moment; the classic-art maps need RDE in original art (the tools copy the real config.ini, set to HD).
Decision (owner): implement through the same data-driven RDE corrections table (import-time, keyed by map); capture each map in RDE first.
RDE, keyed on the field map number: map 0x5E (`tel_b12i`) changes every tile whose depth is 0x13B to 0x141 at load, in both art modes (exe checks `== 0x13B` near the 0x5E branches). In the chip2 layer draw 0x5CDED0, map 0x110 (`you_b11f`) layer 7 first fills the draw area with RGB(8,8,8). Map 0x187 (`zel_a17i`) in original art still takes layer 12 from chip2. Map 0x80 (`man_a04d`) in the 1x bake path (0x5CD1C0) overwrites two 16-line blocks of the 8bpp BG sheet at (0x90,0x1F0) and (0xE8,0x110) with constants from 0x6CC0A0 / 0x6CC300.
OpenCross has none of these: `src/import/room*.cpp`, `src/import/chip2.cpp` and `src/game/room_view.cpp` use the file data as is.
Rule 3 forbids room code in the engine. The general form is a data table of RDE fixups owned by `src/import`, keyed by map id, with kinds depth remap / pre-fill before layer / HD layer in classic / texel block patch. The importer applies it when it builds the room, so `game/` stays room-agnostic.
Settle it: take an RDE frame of each room (`rde_bg.py`) and an OpenCross frame (route or `Room=`); tel_b12i should have an actor near the moved layer. The two classic-only cases (zel_a17i, man_a04d) need an owner ruling: "classic = PS1 assets" against "RDE is the reference".
Done when: each room's diff is equal or the accepted difference is recorded in `docs/formats/room.md`.

### Dragon-feeding room `jac_b07i` (map 0x98): HD counter sprites `number.png` / `esa.png`
Priority: P3
Impact: low-med; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured: needs the mini-game trigger in RDE.
Decision (owner): keep; capture the mini-game in RDE.
RDE: field hook 0x800B36D0 (handler 0x424900) on map 0x98 queues three `esa` icons (x = 0x11B - 0x15*i, y = 2) and two columns of digit sprites 0x4000..0x4009 from `hd/map/jac_b07i/number.png` (10 cells of 20 x 22, x 0x32 / 0x1C, y 6). The guest's flag bytes show and hide them. OpenCross: no reader of either file. It is not known whether the PS1 draws this count from BG tiles (which chip2 would already cover) or as an overlay.
Settle it: an RDE HD frame and a PS1 frame (`ps1_capture.py scene`) in the room during the mini-game, compared with OpenCross. Done when: OpenCross shows the same counters in HD, or the doc records that chip2 already covers them.

### AKAO patch-zone choice depends on the previous note on the PS1
Priority: P3
Impact: low; needs a capture or data sweep.
Original 0x80052FB8: input the untransposed note; same note as the remembered one (+0xFA; FE 14 forces a rescan): keep the zone; higher
(or rescan): first zone whose high key >= note; lower: the last zone whose low key <= note. Equal to "the containing zone" only when
zones are contiguous. OpenCross `note_on` (`akao_player.cpp:548-562`): first containing zone of `note + transpose`, else last below, else
first. Data: 21 patches in 19 songs have gaps or overlaps (e.g. music007 patch 0 zones 69..69 / 71..73, music010 patch 0 0..70 / 59..63).
Settle it: a corpus scan of played notes that fall in a gap or overlap; if none, document and close, else port the two scans.

### Original-music song change has no fade-out / fade-in
Priority: P3
Impact: low; needs a capture or data sweep.
Original: the song-change machine 0x8001B860 fades a playing song out (0x8007D288 frames, set to 8 by the fast caller form; one reading gives 16
as the default) before loading, then fades the new one up over `0x800853B1 << 5` frames. OpenCross original mode swaps at once
(`FieldWorld::play_song_at` -> `Mixer::play_akao_bgm`, `mixer.cpp:127-139`). Settle it: `ps1_audio.py` across a room change that changes
song, with the frame count from the fade start. Done when: original mode fades like the PS1, or audio.md states the cut.

### AKAO SFX request volume wraps at 0x80 on the PS1, clamps in OpenCross
Priority: P3
Impact: low; needs a capture or data sweep.
Original: 0x8004E7D8 and 0x8004A118 mask the request volume with 0x7F. OpenCross `sfx_volume_modifier` clamps (`src/audio/sfx.cpp:39`,
`akao_player.cpp:403`). Settle it: a data sweep of the volume operands of field / battle SFX calls (C5 / C8 / sound ops, PAC sound ops);
if none is >= 0x80, close with a doc note; else mask like the PS1.

### Battle camera between actions: the PS1 holds the pose when no PC is ready, OpenCross orbits the last actor
Priority: P3
Impact: low; needs a capture or data sweep.
Original: 0x800C3F28 picks the camera record by menu state 0x800E065A; state 4 (between actions) orbits the unit in 0x800E0662 and does
nothing when it is 0xFF (the camera keeps its last pose). OpenCross `BattleView::menu_camera` (battle_view_camera.cpp:149-151) falls back
to `active_pc_` when `menu_pc_after()` is -1 and keeps orbiting. It is not proven that the state is 4 for the whole wait (0x800C6AC4 /
0x800D0A74 set it, 0x800D0B28 / 0x800D0CC8 / 0x800C704C reset it). Reproduce: one-PC fight, the seconds after his action while his
countdown refills: `ps1_capture.py scene` frames against the route's camera log. Done when: the capture decides and `between && next_pc < 0`
holds (or the comment above `menu_camera` records the orbit).

### A finished one-shot song stays current in RDE; OpenCross clears it
Priority: P3
Impact: low; needs a capture or data sweep.
RDE ignores a play request for the current song, and a song that ran out (songs 0, 9, 60, 61, 62, 67) stays current (`remastered_music.md`).
OpenCross: `Mixer` sets `song_ = -1` when the stream ends (`mixer.cpp:447`), so a second request plays it again.
Settle with a script sweep (`cc_fieldscript.py` / `occ` dump): list `88` / FE play requests of those six ids that repeat with no stop or other song in between.
Done when: no such site exists (drop the entry), or `song_` is kept until stop / next play and the case is checked on RDE.

### A disc-2 jump to an id whose disc-2 record is dummy.dat loads the real room in OpenCross
Priority: P3
Impact: low; needs a capture or data sweep.
`FieldWorld::room_stem_for_id` (`field_world.cpp:657-661`) falls back to the lang.dat room name when the disc record is not a map `.bin`; the PS1
loads what the disc-2 index holds (the dummy scene). Whether any disc-2 script reaches such an id is unknown.
Settle with a data sweep: for every 5A / DC target in the 566 rooms, read the disc-2 record (`cdrom2.fnd`, `DiscIndex::room_stem`) through a headless
`opencross` scanner and list the ids that are `dummy.dat` on disc 2 but reachable from disc-2 scenario values. Done when: the list is empty, or the fallback follows the PS1.

### tgfrog: the gap between shots `2` and `3` is about 34 refreshes longer in OpenCross
Priority: P3
Impact: low; seen in a capture.
In the caught green-summon cast (eid 208 caught by an enemy trap) the PS1 and OpenCross show the same camera order, but the `2` -> `3` hand-over comes about 34+ refreshes later in OpenCross, beyond the ~10-refresh tolerance (validation.md 3.5). Repro: `ps1_cast.py force` the caught cast and `tools\ours_cast34.ps1`, diff the shot start ticks. Done when: the hand-over lands within tolerance.

### Radius tutorial (battle 386): RDE keeps the first enemy alive
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured: forced onto room 53's battlefield (not Arni's); during the tutorial talk RDE restored Radius's HP within one vblank; with his HP set to 0 Serge's command menu stopped taking input. Needs the real Arni setup.
Decision (owner): mimic RDE; capture on the real Arni battlefield with Serge alone first.
RDE battle.prg 0x800D86D4 (always, not only with boost): in battle 0x182, unit 3 never stays KO'd (the KO bit is cleared, state set to
2) and its pending value `+0x2A` is kept below its HP `+0xB8`. OpenCross has only the speed lock for 386 (`src/game/cheats.h:36`);
nothing in `src/battle` protects unit 3. Spec: `research\docs\reimpl\gameplay\battle.md` "0x800D86D4".
Repro: the tutorial on RDE and in OpenCross (real battlefield, Serge only): deal lethal damage to Radius; compare outcome.
Done when: the rule is applied as RDE does if the capture shows a difference.

### `data/hd/model.txt` per-model fine-Z flags (Serge 1, Guile 4, scene 0x10F01 yamaneko 2)
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured: needs a Guile close-up and the you_b10e cat scene in RDE.
Decision (owner): keep; capture in RDE.
hd.dat `model.txt` holds the uncommented rows `0 1 selju` and `2 4 guill` plus a scene key; RDE reads it in HD only. Per the report: flag 1 = nearest-corner face slot, 2 = depth bias, 4 = fine-Z off. RDE's fine-Z re-sorts primitives inside each OT bucket, while OpenCross puts a per-fragment sub-slot depth inside the slot (`src/render/mesh_pipeline.h:16-22`). OpenCross does not read `model.txt`.
Settle it: HD close-ups of Serge and Guile and `you_b10e` with the cat, RDE (`rde_bg.py`) against OpenCross, looking for wrong ordering of intersecting parts. Done when: these match, or the flag meanings are recorded in model.md with a reason OpenCross does not need them.

### `eyel` model-part HD offset (x -4, y +0x1C)
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured: needs the tech that loads `b_press`.
Decision (owner): keep; capture in RDE.
RDE hook 0x8003EBB4 (handler 0x436C40, HD only) moves a part tagged `eyel` before drawing it (x -= 4, y += 0x1C in camera space). The part lives in `battle/battle.prd` and `battle/effects/tech/b_press.prd`. OpenCross: no `eyel`. Which effect draws it is unknown. Find it by grepping the effect scripts for the part, then compare an RDE frame. Done when: the offset is applied in HD, or the part is shown to be unreachable.

### Classic art: RDE swaps Doc's texture on seven `gal_*` maps (`fieldobj/ctd.txt` + `player_sd/doc`)
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): Not captured: needs an RDE run in classic art.
Decision (owner): keep; capture RDE in classic art. If RDE really swaps it, follow RDE and note it as the one classic-art exception.
RDE 0x437640 reads `fieldobj/ctd.txt` (cdrom2). In original art it uploads `fieldobj/player_sd/<model>/<model>.tim` at 1x for the rows `doc 256 224 896 384` (gal_a05i/a06i/b05i/b06i/b08i/b12i/b13i). OpenCross reads neither file. This is a design question for the owner (classic = PS1 assets, against RDE as the reference); if it is adopted it is a per-map import data table, like the chip2 fixups. Done when: the owner rules, and if adopted, a classic-art frame on gal_a05i matches RDE.

### PC chain: the PS1 keeps the remembered actor / target across enemy turns
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): PS1: every captured enemy action zeroed the chain counter, including a hit on a PC that does not own the chain (so OpenCross's reset matches); the PS1 keeps the remembered actor / target, OpenCross clears them. The effect on the HIT% display was not compared.
Original (read, not captured): the counter 0x800E0694 is reset by 0x800CA020 (new actor or a different remembered target), when the chain owner is hit or drops to 0 stamina (0x800C704C / 0x800C7484), by Defend and a forced miss; not by an unrelated enemy turn.
OpenCross: `apply` / `settle` reset `chain_actor_` / `chain_target_` / `chain_hits_` after every enemy turn (battle.cpp:2047-2054, 2025).
Settle with a PS1 HIT% readout: attack, let an enemy act on another PC, attack the same target again. Done when: reset points match.

### PCs start a battle with lo status 0x00400400 on the PS1, 0 in OpenCross
Priority: P3
Impact: unknown (low expected); seen in a PS1 capture.
The PS1 starts every PC's battle unit from the record status words OR the gear words plus lo 0x00400400; OpenCross starts lo at 0 (`src/battle/battle.cpp`, the PC setup). The two bits 0x400 and 0x400000 are also the bits the equip commit keeps in record +0xA8. Find their readers in the battle overlay, then compare one battle's first turn. Done when: the bits are named in docs/rules/battle.md and OpenCross sets them if any reader acts on them.

### Battle SFX: OpenCross plays a battlefield loop (`se35`) the PS1 battlefield does not
Priority: P3
Impact: low; needs a capture.
In an audio capture of a battle, OpenCross played a looping `se35` at -21 dBFS that the PS1 battlefield did not have; which battlefield the PS1 encounter used was not confirmed. Repro: the same scene on both sides with `ps1_audio.py --state ... ` and `ocbattleseq sfx`, battlefield fixed. Done when: the loop is shown to belong to that battlefield on the PS1 too, or OpenCross stops starting it.

## 4. Code hygiene

### Dead stand-in modules: every class below is overwritten in the registry and can never run
Priority: P3
Impact: low; confirmed in code.
`BespokeRegistry::add` (`src/game/effects/bespoke/bespoke.cpp:231-237`) replaces an entry with the same name; the order is `bespoke.cpp:208-226`. No file outside the defining one names these classes (grep of src, tests, tools). Simulating the registration order:
- `mesh_flight.cpp` (whole file): FaceFlight rows saraman, wipeoff, lasbos (ground ports), drg_deth (death), fnoise (mesh_techs), theend (summon); GhostCopies tumaru (tech_ports); DustSweep tekken (ground); CellReveal rakugaki (mesh_techs).
- `mesh_sweep.cpp` (whole file): SweepCollapse tfleaf tgfrog tgolem tunico (mesh_techs Trap), cook swordbox (Vacuum), 45infe (Infe45); WaveRise 44get (Get44); BlackHole 46black (Black46, then wrapped by BlackWarp); GateFx mushi garadeth octdeth ki_deth (death_ports), hensin ghost2 jakotu (mesh_techs).
- `motion_tasks.cpp` (whole file): MotionTasks mido2 kuzira3 (summon_ports), uzumaki 3mix (tech_ports).
- `actor_script.cpp`: ScaleMove 19helf golem, Grav41, Acce43 (model_steer); Shine, Opthedo, Kamedes (tech_ports); Torune (Shell); IceLance (emitter_patch); StateOnly rows k_monk soidash bboy shock (tech_ports), 27earth (emitter_patch), enrou hebi (summon_ports). Only StateOnly "3voice" (and the ActorScript base) is live.
- `mesh_warp.cpp`: WarpKind Daimou (death_ports Daimou), GodHand (tech_ports Godhand), Super38 (model_deform Super38); only Bar14 is live (wrapped by `add_ground_part<BarWave>`).
- `band_wipe.cpp`: rows muteki (WipeStyle::Flare) and 155slot (WipeStyle::Reels) are replaced by screen_ports; the Flare and Reels code paths of BandWipe are dead, the six Negative rows live.
Keep what live code uses (`mesh_base.h` for mesh_water, the ActorScript base for 3voice / MeshWarp). Header comments of the trimmed files, `docs/formats/battle_effects.md:177` ("older stand-ins in ...") and :252 ("3voice: stand-in") follow.
Done when: the listed classes / rows are deleted, `.\build.ps1 debug -Test`, `opencross ocbattle fx-audit` (542 clean) and `pac-soak` are unchanged, and `ocbattle fx-reg` / the registry lists the same module per name as before.

### fieldscript.md B3 row says the entity operand is ignored
Priority: P3
Impact: low; confirmed in code.
0x800C25F8 reads the entity from operand index 1, as `field_vm_ops_game.cpp:163` does; `docs/formats/fieldscript.md:303` says "a constant 1 is used".
Done when: the row says "the entity is operand 1".

### Stale comments and doc sentences about the two script VMs
Priority: P3
Impact: low; confirmed in code.
`src\game\battle\battle_seq.h:10` lists 0x52 / 0x5D as not interpreted (both are). `docs\formats\battlescript.md:29` says
"call stack (4 return addresses per frame, no bounds check)": 0x800CEE84 / 0x800CEF28 index the slot by frame*8 + depth*2 with ONE
depth byte per entity (+0x184), which `halt` (0x800CED08) does not reset; halt also jumps straight to frame 0 (frame index := 0)
where OpenCross (`pac_vm.cpp:268-283`) pops only the top frame. Section 2's "blocked ops retry next tick": the dispatcher 0x800D4248
retries within the tick until the budget runs out. Done when: header and doc are corrected (or the per-entity depth is modelled).

### fieldscript.md says ops 52/53/57/58/DE are "in no source"
Priority: P3
Impact: low; confirmed in code.
Data use (scan of the shipped data): 0 uses of 52 / 53 / 57 / 58 / DE in reachable or unreferenced room code (controls 50, 51, 55, 56 are found), so only the doc wording and the table rows are at stake.
The PS1 table has handlers. 50 0x800C4FB0 / 52 0x800C4F60 test the held pad word (0x800EA488); 51 0x800C4F88 / 53 0x800C4F38 test the latch (0x800E9CCC).
52/53 jump unless the word == U (5 bytes `w J`). 57 0x800C4A14 calls a subroutine if the leader is inside region b (4 bytes); 58
0x800C4B74 is 57 with 56's height band. DE 0x800C06A0 is 9B anchored on joint b of entity c (8 bytes). None occurs in the data.
OpenCross decodes them as unknown, advance(1). Done when: `fieldscript.md:508` and `:752` list them as present on the PS1 but unused
(with their lengths), and `field_opcodes.inc` has rows so a mod or other-region data decodes in step.

### Hensin / idle-pose / kamedes docs and comments contradict the code
Priority: P3
Impact: low; confirmed in code.
The hensin 0x181 reveal is implemented (`mesh_techs.cpp:469-492` state 0x181, clipped draw + shards) and the unit swap happens at
0x181 (`form_swap(1)`, line 419), but `mesh_techs.cpp:408-409` says "Not reproduced ... OpenCross has no unit swap",
`docs/formats/battle_effects.md:268` says "Not built: the 0x181 reveal" and "apply_form at 0x183", :337 says "Not done: the 0x181 reveal"
and "apply_form at hensin 0x183", :253 lists "hensin's new form" and "the idle-pose word" as host gaps (the pose hook is wired).
kamedes exit (0x801C6268) clears bit 1 of the target's CONTROL BLOCK flag word +0x14, not a model flag (:225 says "model flag 0x0002";
`ps1_effects_kamedes.txt` says "semi-transparent flag"). Done when: those lines match the code (compare the hensin path against a
`ps1_cast.py force` capture of element 238 before rewriting the reveal paragraph).

### Doc lines that contradict the original sound driver
Priority: P3
Impact: low; confirmed in code.
`docs/formats/audio.md:72` (keymap envelope guard, "reverb bit is not applied") and `:107` ("other values 01") are contradicted by
main 0x80053370 / 0x800552B0. Fix together with the AKAO entries above.
More (second audit pass): More lines: audio.md:64 (+0x28 is the full-gate mask, non-zero in 4 songs, not "alloc_mask, 3 files"), :79 (default gate is full length
on unmasked music channels; length-2 A2 / DC notes never key off), :135 (FE 0B semantics), :143 (FE 1A / 1B set / clear a flag that is
on by default), :196 (A4 also starts at a tie), :119 / :272 (D4 used by 3 room frames), 7.6 (music voice search / steal rule). Fix each
together with its entry above.

### Stale "not modelled / stub" comments that contradict the code
Priority: P3
Impact: low; confirmed in code.
`src/game/battle/battle_seq.h:10` calls 0x52 / 0x5D no-ops (implemented in `battle_seq.cpp:425,455`);
`src/game/effects/bespoke/mesh_techs.cpp:408-409` says 0x181 / unit replacement are not reproduced while the class calls
`form_swap` (:419-435) (also `docs/formats/battle_effects.md:253`); `src/battle/battle.cpp:44,2291` and `battle.h:97` say
STUB for documented rules; `src/ui/dialogue_box.h:64`, `dialogue_box.cpp:771` say stub before real code;
`scene_remap.cpp:111` says the host does not swap stages while `battle_view_fx_cast.cpp:164,357` wires `fx_swap_stage`;
`docs/formats/fieldscript.md:325` (C5) and 11.4 say the positional voice is not updated while
`field_world_effects.cpp:125-190` does it; `new_game_plus.cpp:156` says the cell keeps bits 14-15 while the code zeroes it
(fixed by the FE 85 entry). Done when: each comment / doc row describes what the code does.

## 5. Tests and tooling

### `Room=` / `load` debug start of a disc-2 room loads the disc-1 map record
Priority: P3
Impact: low (dev start only); seen during a capture.
Starting OpenCross at crn_b16i (a disc-2 room) through `Room=` / `load` reads the disc-1 map record. Done when: the debug start picks the disc of the room and a route test starts in a disc-2 room.

### `GameTables::load` reads the 408-byte tail of `char_w1..5.bin` as Lv99 targets
Priority: P3
Impact: low (dev tools only); confirmed in code and data.
`src/data/tables.cpp:127-132` takes the bytes after the 51 records of `char_w1..5.bin` as that set's `lv99para`. They are not: they differ from `lv99para.bin` (which starts `52 03 58 4E ...`, the tail `00 40 00 40 00 40 23 C1 ...`) and decode as nonsense rows (HP up to 49312, stats up to 254). Battles use `char.bin` with `lv99para.bin`, so only `ocbattle sim --set char_w1..5` and anything else that picks those sets sees the garbage. `docs/formats/battle_data.md` now says the tail is not `lv99para`; what it holds is unknown. Done when: the loader uses `lv99para.bin` for every set (or decodes the tail once its meaning is known) and a unit test checks a `char_w1` Lv99 row equals `lv99para.bin`.

