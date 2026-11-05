# Open issues and known bugs

The working list for the engine: behaviour bugs found by reading the code, fidelity gaps against the original game, open comparisons that need a capture, and code hygiene. Each entry names the file to look at. Remove an entry in the same change that fixes it; keep the sections. This file is temporary working notes, so unlike the other docs it may carry RAM addresses; they leave with their entry, and anything worth keeping moves to `docs/formats` as a fact without the address and to `tools/ghidra/symbols/` as a symbol.

Entries that need a PS1 comparison use `python -X utf8 tools\py\ps1_cast.py force` (`docs/validation.md`, section 3.4) for the reference and `tools\ours_cast34.ps1` for the OpenCross side, which runs `tools\py\fx_diff.py`. Reading a diff: a sequence name that only one side has is not a bug until these are ruled out: a random camera pool or selector op, a different target count (a debug cast hits every enemy of an all-targets element, a forced PS1 entry hits one), our run ending before the sequence starts (raise `-Frames`), the capture holding two casts (`--cast-index`, `--window`), the scene's own opening action running first on our side, and `-Dt` pacing, which caps per-tick emitter fires. Unique techs are cast by their owner: `0xC000 | element` in a grid cell of any `char*.bin` set, or pgdat bytes +0x14..+0x16 for a double or triple tech.

## Start here (P1)

Player-visible, high-reach entries; each is described in full in its section below.

None open.

Priority: P1 = player-visible with wide reach, P2 = visible or behaviour-changing, P3 = small, rare or invisible. Entries with "Decision (owner)" carry the owner's ruling on RDE versus OpenCross behaviour; follow it.

## 1. Bugs (behaviour)

### Speed / cheat toggles must reset every launch (RDE never saves them)
Priority: P2
Impact: low; confirmed in code.
Decision (owner): reset all five every launch, as RDE; never write them to the INI.
NEW.
RDE: slow, fast, no-encounters, battle boost and auto battle (`sys_fast_mode` 0x1C6DD81 and neighbours 0x1C6DD82/83/90/91) live
in .bss, are written only by `sys_poll_hotkeys`, and have no `config.ini` key (the key-name builder 0x40CF50 has none), so every
launch starts with them off. OpenCross: `[Game] FastMode/SlowMode`, `[Cheats] NoEncounters/BattleBoost/AutoBattle` are read
at boot (`src/platform/config.cpp:182-183, 273`) and saved on every toggle (`src/app/app_run.cpp:199`); config.md 1 documents
it. A player who left fast mode or no-encounters on gets it again from the title on, without a reminder.
Done when: the owner decides; either the five are session-only (still writable by hand / `--config` for tests), or the
persistence is listed in architecture.md 13 (optionally a boot toast naming the toggles that are on).

### AKAO music channels gate notes at full length unless the song header masks them
Priority: P2
Impact: med; confirmed in code.
Capture (PS1 audio against OpenCross, original-music mode): PS1 key-on / key-off trace: in music002 (header mask 0) every old voice is keyed off in the same update as the next key-on (no gate); in music019 (mask 0x3F) the key-off comes 5 updates early (the length - 2 rule). OpenCross applies length - 2 to every song.
Original: song load 0x8004E010 sets channel flag 0x40 (word +0x34) on every music channel whose bit is NOT in header u32 +0x28; the
VM 0x800535E4 then sets the gate counter (+0x7C) to the full length for flagged music channels, so the music tick 0x80052458 never
issues a gate key-off: the voice sounds until the next key-on (which keys it off first), a rest, or the channel end. FE 1A sets the
flag, FE 1B clears it (neither appears in data). Header +0x28 is 0 in 64 of 68 songs; 0x3F in music019 / 020 (all channels), 0xFF in
music039 (all), 0x3FF in music069 (channels 0-9 only): only those channels get `length - 2`.
OpenCross: `Channel::tenuto` defaults false (`src/audio/akao_player.h:171`), set only by FE 1A (`akao_player.cpp:1073`); `set_gate`
(`:788-791`) gives every non-continuing music note `len - 2`; `parse_seq` (`akao_seq.cpp`) does not keep +0x28.
Player effect (original music mode): every note of 64 songs releases 2 ticks early (a length-3 note sounds 1 tick), clipping
arpeggios and staccato and starting tails early.
Settle it: `ps1_audio.py` recordings of music002 (mask 0) and music019 (mask 0x3F) against `ocaudio render-seq` of the same songs;
the masked song should show the 2-tick earlier release, the other not.
Done when: the sequence keeps header +0x28, `load_music` sets `tenuto = !(mask >> channel & 1)` for music channels, FE 1B clears it;
audio.md:64 names the field (gate mask; non-zero in music019 / 020 / 039 / 069), :79 and :143 state the default; the comment at
`akao_player.cpp:682` ("legato and tenuto flags (flags & 0xFFFA)") says legato / full-length (D0), which is what it clears.

### AKAO DA portamento: sets legato, forgets the previous note, ends at a rest, glides linearly in the pitch register
Priority: P2
Impact: med; confirmed in code.
Capture (PS1 audio against OpenCross, original-music mode): Not captured: only room frames use DA; needs a save in zel_a02i.
Original: DA 0x80054760 stores the step count (+0xAE, 0 = 256), clears the remembered note +0xAC and transpose +0xFE, and writes 1 to the
mode word +0xB0 (the same value CC writes; it also clears D0 full length). Following notes are legato (re-pitch, no re-key, no gate
cut); the first note after DA does not glide (no previous note). The glide reuses the A4 bend: start = the previous note's register
pitch, target = the new one's, constant 16.16 step per tick. The rest branch clears +0xAE (portamento off until the next DA); DB clears
only +0xAE (legato stays).
OpenCross: DA only sets `porta_steps` (`akao_player.cpp:991`); `note_on` (`:595-603`) glides from `last_note`, which survives DA and rests,
as a semitone offset applied by `exp2(off / 12)` (`:1212`, which also scales vibrato and fine tune); legato untouched; rest keeps
`porta_steps`.
Player effect: 89 SFX units use DA, 37 without any CC (e.g. `zel_a02i..a10i` frame 3 `DA 10; note B 12; note G# 96`: PS1 one voice
gliding, OpenCross a cut note and a re-keyed glide; `jac_a03f/a04f/b03f/b04f` frame 0 glide from the note before DA). Mid-glide pitch
differs (octave leap halfway: 702 vs 600 cents).
Repro: render `zel_a02i` frame 3 against a `ps1_audio.py` capture of the room.
Done when: DA sets `legato = true`, `full_length = legato_prev = false`, `last_note = -1`; rest zeroes `porta_steps`; the glide is a
linear register bend from pitch(previous) to pitch(target); zel_a02i matches the capture.

### Pilfer chance: the PS1 reads the thief's raw heavy-hit byte on a fresh target, OpenCross the HIT% box figure
Priority: P2
Impact: med; confirmed in code.
Original: mode 30 (0x800D5454) sets the chance byte 0x800E068D := byte +0xA5 of the menu PC 0x800E0649 when the chain counter
0x800E0694 is 0 (no target or evade term). 0x800E068D is otherwise written only by 0x800D7818 (low byte of the heavy HIT% 0x800E0692 it
recomputes after a PC weapon action), and 0x800CA020 zeroes 0x800E0694 for a new actor or a different target. So the box figure is used
only when the PC already chained weapon hits on that same target. Then opposite gender doubles it (8-bit), `E` != 0 sets rare 20 and adds
the +0xA5 byte of unit 0x800E0635 (0xFF for a solo cast: value unknown statically).
OpenCross: battle.cpp:1514-1517 uses `hit_chance_for(actor, target, 2)` and its comment states the opposite of the PS1 code;
battle_data.md:165 repeats it. Player effect: early game ~20 % (40 % opposite gender) on the PS1 vs ~40-46 % minus evade here.
Reproduce: read 0x800E068D at a pilfer with and without a prior weapon hit on the target (`ps1_capture.py`), 100 casts each.
Done when: chance = `hit_heavy` (raw +0xA5) when `chain_hits_ == 0` for that actor / target, else the stored heavy HIT% display figure;
the Double-take partner term decided by that capture; the comment and battle_data.md fixed.

### Minor stat growth rounding: the PS1 aim and gain go through two odd roundings, not round-half-up
Priority: P2
Impact: med; confirmed in code.
Original (btlexit): 0x801EFCE8 turns `x = pending x tenths / 10` into the aim K by a six-place digit loop whose inner loop stops at
`remainder <= 2w` plus a carry pass (net: K = ceil(x) - 1 for x >= 2; rounds below 2). 0x801EFECC: `v = 0.5 x ((snap + K) - cur) x
sqrt(progress / cap)` (sqrt by Newton, main 0x80018694), then +1 when the remainder of a "subtract 1 while v - 1 > 1" loop is >= 0.5 (v > 2
always +1), then main 0x80028698 (double to unsigned) whose threshold constant is the immediate 0x3E22725DD1D243AC (2.147483648e-9, not 2^31),
so positive values convert as trunc(v - 2.1e-9): exact integers lose 1. Net gain: v >= 1.5 -> ceil(v); (1,1.5) -> 1; 1.0 -> 0; [0.5,1) -> 1.
Verified by running the original code in a MIPS emulator (the disc's main image at 0x80010000, btlexit at 0x801EA800, the
overlays.csv base); 0x80028698 disassembled from that image matches the decompile and the constant; the signature (short a0, double
a2/a3, short cur at sp+0x10) matches the o32 ABI; RDE has no hook on any of these addresses. In a random sweep 816 of 4000 cases differ by 1, both ways.
OpenCross: `minor_growth` rules.cpp:237-243 `floor(gain + 0.5)` with fractional aim; tests/test_battle.cpp:308-311; docs battle_data.md
section 3 and rules/battle.md 8.2 say round half up. Done when: K and the gain follow the procedure above (port the Newton sqrt too), tests
add 62/37/4/64/1/8 -> 3 and 204/30/8/205/8/8 -> 11, `full_minor_growth` uses the same K, docs updated; confirm with one PS1 victory's gains.

### 2boll (Aqua Ball) ripple buffers never alternate; ripple's first step runs in place
Priority: P2
Impact: med; confirmed in code.
The 2boll frame (0x801C7210) reads buffer `cur` (0x801C857C), writes buffer `other` (0x801C8580), then sets (cur, other) := (1, 0) if cur was 0, else (0, 1). The particle hooks (0x801C6FC8 / 0x801C7160) stamp into `cur`, the sheet draws `other`. Nothing initialises the two statics; the shipped `prog/2boll.prg` holds cur 0, other 1 (jitanda 0x801C858C / 8590: 0 / 1; ripple 0x801C89FC / 8A00: 0 / 1). So every step on the PS1 reads one buffer and writes the other.
OpenCross: `Boll2::init` sets `ripple_.cur = ripple_.old = 0` (`src/game/effects/bespoke/ground_ports.cpp:395`) and `Ripple::step` toggles with `std::swap(cur, old)` (:303), so the pair stays (0, 0): every step runs in place on buffer 0 for the whole effect (the wave neither spreads nor decays as on the PS1, stimuli add to leftover state). The `Ripple` struct default (0, 1) is right; jitanda keeps it and matches.
The ripple overlay's port `HeightField` starts `newest = oldest = 0` (`mesh_water.cpp:45`), so its first step runs in place where the PS1's does not (one tick; element 524 is cast by no battle script).
Docs to fix with it: `docs/formats/battle_effects.md:238` ("both buffer indices 0 at the start") and :243 ("the first step in place"), the comment at `ground_ports.cpp:385` and `mesh_water.cpp:41-42`.
Done when: Boll2 keeps cur 0 / old 1, HeightField starts newest 0 / oldest 1, the docs say so, and a `ps1_cast.py force` capture of Aqua Ball (eid 33) against `tools\ours_cast34.ps1` shows the same sheet heights over ~60 ticks.

### tekken start cell uses the caster's model origin instead of attach point 8
Priority: P2
Impact: med; confirmed in code.
tekken op 0x100 (0x801C74B4): the start cell comes from attach point 8 of caster 0 (0x8009E494 with 8; gx = (x + 0x1400) >> 9, gz = (z + 0x1400) >> 8), the end cell from target 0's position; the piece count |gxT - gxC| + 1 and the z step ((gzT - gzC) << 12) / |dx| depend on it.
OpenCross `Tekken::command` (`src/game/effects/bespoke/ground_ports.cpp:217-220`) grids `cm->pos` with the comment "the host keeps no joint matrices here". The host does provide the attach point: `OverlayFx::attach(model, k, out)` (same numbering as 0x8009E494; used by line_ring.cpp:253, death_ports.cpp:658, mesh_techs.cpp:40). (`joint_world` takes a joint, not an attach index; do not use it here.)
Effect: the trail can start one cell (0x200 x / 0x100 z) off and have one piece too many / few when the attack pose puts the hand joint more than half a cell from the origin.
Done when: gxC / gzC come from `attach(caster(0), 8, ...)` (model origin only as fallback) and a `ps1_cast.py force` capture of the tekken tech (owner character) matches the piece positions.

### lasdes2 dissolve draws hidden parts and uses the part's ABR instead of ABR 1
Priority: P2
Impact: med; confirmed in code.
lasdes2 dissolve draw 0x801C8708: all four primitive branches skip a part whose flag byte (+0x37) has bit 0 set (the asset's hide bit, and the bit lasdes2 ops 0x107 / 0x108 set / clear); every semi-transparent face's tpage is forced to ABR 1 (`& 0xFF9F | 0x20`). The other eight F-B overlays have no bit-0 test.
OpenCross: `Dissolve::pass` model_colour branch (only `kLasdes2Fb` sets model_colour, `death_ports.cpp:883`) never tests `sk.shading[part][3] & 1` (comment at :684 says the F-B draws do not skip parts, true for the others), although `fx_skin_asset` folds `part_hide` into that bit (`battle_view_fx_skin.cpp:378`); and :703 sets `semi_blend = ((part_flags >> 2) & 3) + 1`.
Effect: in the Time Devourer death chain (scene 472, element 526) parts hidden by 0x107 are dissolved anyway; faces whose part ABR bits are not 1 blend 50/50, B-F or B+F/4 instead of additive.
Done when: the model_colour branch skips parts with flag bit 0 and uses ABR 1 (semi_blend 2); first check with a cc_* model reader which lasdes2 parts have ABR bits != 1, and whether `lost` / `main` send 0x107; compare one dissolve frame against `ps1_cast.py force` (or RDE).

### The "all achievements" meta unlock `ach_ps41_all` is never awarded
Priority: P2
Impact: med; confirmed in code.
NEW.
RDE: `ach_unlock` (0x4480C0) accepts indices 0..37, sets the bit in `ach_bits` (0x1C783F0), sends the name to Steam, and then,
when bits 1..37 are all set (byte 0 & 0xFE, bytes 1..3 = 0xFF, byte 4 & 0x3F), calls itself for index 0, whose name in
`ach_name_table` (0x6B1648) is `ach_ps41_all`. `ach_steam_stats_received` (0x40C020) re-seeds bits 1..37 from Steam, so the meta
fires on whichever award completes the set, even if the other 36 came in earlier sessions.
OpenCross: `src/game/achievements.cpp:51-52` has `kApiNames[0] = nullptr`, the enum starts at 1, `done_` is per run, and nothing
earns index 0; `docs/rules/achievements.md` lists 37 and never names the meta one. A player who earns all 37 through OpenCross
never gets the 38th on Steam.
Fix shape: after `SteamClient::flush_unlocks` (`steam.cpp:185`) accepts a name, if `achieved()` is true for all 37 names, unlock
`ach_ps41_all` (Steam is the store, since OpenCross keeps no bits). Respect the same "debug tools used" gate as other awards.
Done when: the meta is unlocked once the 37th is set (test with the local Steam emulator dll, never the real account),
achievements.md gets a "meta" rule, and the `ach_unlock` symbol comment stays as is (it already mentions the meta check).

### Movie end: RDE holds the last frame for 10 frames, or fades it over 160 frames when flag 8 is set; OpenCross fades over 0.5 s
Priority: P2
Impact: med; confirmed in code.
Decision (owner): mimic RDE's hold / fade.
RDE: 0x44E140 stores `fade = (movie flags 0x800EBA94 >> 3) & 1` in 0x1C25816 when it opens the Bink file. At the end frame (or on the PS1 close hook)
0x5836A0 pauses the movie. With fade clear it arms a hold (0x1C2581C): the last frame stays at alpha 1.0 for 10 frames (counter 0x1C25820), then
closes. With fade set the paused frame is drawn at alpha `(160 - n) / 160` for n up to 159 (counter 0x1C25818, constant 0x6CBBC4 = 160.0), about 2.7 s,
then closes. Flag 8 is set for every D4 (all use `#8` or `#520`) and for every movie after a DC (entry above). Fade clear: the other 7E rows
(opn_a05i 3, mis_b02e 5, jac_b20i 7, you_b06i 10, ryu_b29i 13..17, ...).
OpenCross: `App::end_movie` keeps the frame, and `fade_held(dt, 0.5)` (`app_sim.cpp:365-368`, `player.cpp:270`) fades every movie over 0.5 s,
4 ticks after the movie. `movies.md` section 6 says "RDE replaces the routine with Bink, so the fade is OpenCross's own": wrong.
Open: what RDE shows behind the 160-frame fade (the field resumes under it, or after the DC, the new room). Settle with an `rde_bg.py` frame
series after the last frame of `D4` crn_b16i row 6 (entrance 2) and of `7E` opn_a05i row 3. Done when: hold 10 / fade 160 by the flag rule, `movies.md` section 6 fixed.

### chip2 ABR 0 semi groups: RDE draws B + F/2, OpenCross draws (B+F)/2
Priority: P2
Impact: med; needs a capture or data sweep.
Capture (RDE against OpenCross): RDE hid_b09f: the frame fits B + F/2 (error 16) and not (B+F)/2; OpenCross (B+F)/2 is about 30 luma darker over the whole screen. Acceptance: an RDE / OpenCross frame diff of hid_b09f.
Decision (owner): use RDE's blend (B + F/2) in HD art; Classic keeps the PS1 blend.
RDE: in the chip2 layer draw 0x5CDED0, a group that is semi with ABR 0 has its tpage ABR switched to 1 (additive) and its three colour bytes shifted right by 1 before the draw. The result is B + F/2, and the halved colour stays set for the group's following draw. OpenCross: `docs/formats/room.md:174` and `RoomView` draw ABR 0 as `BlendMode::Average` (B+F)/2 in both art modes. The doc's reasoning ("would brighten every covered pixel") is a PS1-first argument, not an RDE measurement.
There are 6,882 ABR 0 tiles; `hid_b09f` layer 5 (a whole-screen water surface) is the clearest case.
Settle it: RDE HD frame of hid_b09f (`rde_bg.py`) against an OpenCross HD frame, plus a mean-brightness table over the water area. Also check whether the halved colour darkens the group's opaque (texture B) texels.
Done when: the HD path matches RDE's blend (classic keeps the PS1 average) and room.md 5.1 says which rule applies to each art mode.

### Anim op 0x69 GetOption is a stub returning 0; 194 tech reaction scripts branch on it
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): 594 uses in 198 scripts (194 `atk` tables + 4 tech packs); the tests are on masks 0x10 and 0x40 only. The 14 monsters with low byte 0x70 appear in 23 scenes. Best repro: Hard Hit (`hardhit.prd`, element 217) on Hydra (enemy 50, scene 415) or Mama Komodo (enemy 5, scene 60).
Battle overlay 0x800B6C60 (anmseq op 0x69, `69 selector out`) resolves the control block of the running model through 0x800BC1EC
(selector 0: acting list first, 1/2: target list first, then every unit) and stores its word +0x28. That word is copied at block
build (0x800C8248) from unit record +0x14 (0x800E06B4 + unit*0x410) = the stat record's u16 at 0x14: low byte 0 / 1 / 0x70
(14 monsters have 0x70: ids 5 13 21 27 50 55 73 98 102 107 127 144 146 151), bits 14-15 the gender. OpenCross already holds that
word as `battle::Unit::gender` (`src\battle\battle.cpp:174,232,291`), but `src\game\battle\battle_seq.cpp:369` writes 0.
The standard variant-7 reaction of ~194 tech packs (e.g. `battle\effects\tech\hardhit.prd` main at +0x66C / +0x75E / +0x7BC)
reads it with `69 1 1` and tests bits 0x10 / 0x40 / 0x80; with 0 every target takes the small-unit branch (the 0x40 test skips
a stand-pos JumpT hop), so heavy enemies flinch/hop under techs where retail keeps them planted. While there: the query ops
0x47-0x58/0x63/0x9F/0x69/0xA9 ignore their selector (`Ctx` is one record, `battle_view_units.cpp:142-194`); only matters for a
unit that is both caster and target. Reproduce: Hard Hit (or any tech pack) on monster id 5 vs PS1 `ps1_cast.py force`.
Done when: `Ctx` gets `option = unit.gender`, 0x69 writes it, and a PS1 capture of one tech on a 0x70 monster matches.

### PAC op 96 damage colour gate is never applied (and 0 results print no number)
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): `evt463` only (scene 463).
0x800D311C stores the operand byte in 0x800E047D. Per element/weapon resolve (0x800D8F5C calls 0x800D8EA4): when the actor is a PC
(slot < 3), the target an enemy, the gate non-zero, and (weapon action, or element whose colour lookup 0x800DEEF4[colour byte>>2]
differs from the gate), the target's pending result word (+0x2A) is set to 0. 0x800B0400 then still spawns a number for any value
but -1, so retail shows "0". Only user: `evt463.pac` entity 4 slot 0 (`96 04 80`). OpenCross: `src\battle\pac_vm.cpp:759` emits
`Event::Kind::Invuln`, which nothing consumes (`battle_view_script.cpp:235` comment only), and `battle_view_units.cpp:620-622`
spawns numbers only for value > 0. Player effect: that boss is hurt by every colour. Done when: the gate lives in `Battle`
(PC -> enemy only), gated hits deal 0 and show "0", and scene 463 is compared with a PS1 capture (`ps1_capture.py scene`).

### PAC A6 (enemy) does not drop the entity or clear the slot-3 flag
Priority: P2
Impact: med; confirmed in code.
0x800D0CC8 for an enemy clears entity +0x189 (slot-3 running), writes 0xFF into its run-order entry (script state +0x684 + entity),
clears the alive-mask bit and ORs the defeated mask; element revive 0x800D46FC writes the entity back into the run list. A6 also
blocks while 0x800A9ED8 != 0 (set by the return-script start 0x800BC62C). OpenCross `pac_vm.cpp:770-775` only clears `s4` and
emits `Killed`; `ctx.s3` is set in `launch` (`:185-186`) and never cleared, `dropped` is not set, so a dead enemy's slot-1 idle
loop keeps ticking (the `battle.cpp:2015` workaround exists because of it) and a revived enemy can never run slot 3 or 4 again
(`launch` refuses on `s3`). Done when: A6 clears `s3` and drops the entity, a revive re-adds it, `ocbattle pac-soak` outcome
counts are explained, and one revive-capable scene is checked on PS1.

### Stair and ladder tiles: the clip time follows vertical motion
Priority: P2
Impact: med; confirmed in code.
Original: 0x800B9220 (RDE: native port, same logic) runs once per rendered frame for every actor with flags38 & 0x1C020 clear.
On a tile whose flag word (+0x7C) has 0xC000, the clip delta `ticks * rate(+0x10E) * 0x40 >> 12` is applied only when the
whole-unit Y (+0x42) differs from the previous frame's (+0x242). The delta is forward when Y fell (climbing) and negative when Y rose
(descending), and 0 when Y did not change. +0x242 is refreshed every frame. OpenCross: `Actor::advance_anim` (`src/game/actor.cpp:628`)
always advances by `kKeyframesPerTick * anim_rate`, and stair tiles only pick the slot and the facing (`actor.cpp:566`, `:931`).
Player impact: standing on a ladder keeps animating the climb, and climbing down plays the climb clip forwards. Reproduce in any
room with a ladder (walk-mesh attr 80/82): climb, stop halfway, then go down. Compare with a `ps1_capture.py` state of the same
ladder. Done when: on 0xC000 tiles the delta is +d, -d or 0 by the Y change, the per-actor previous Y is kept, and
`docs/rules/field_actors.md` section 6 says so.

### Follower back-off (rule 1) faces the leader and plays the walk clip in reverse
Priority: P2
Impact: med; confirmed in code.
Original: in 0x800CBF9C rule 1 (leader distance < 0x50, BE wait byte clear), the code first stores the atan2 of the target. It then
overwrites both +0xF8 and +0xFA with `-(a +- 0x80) & 0xFFF`, where a is the angle from the member to the leader, so the member faces
the leader. It also sets bit 0x400 of the 0x270 word and the walk clip (+0xE2). 0x800B9220 negates the clip delta while bit 0x400 is
set on non-stair tiles, and the per-entity tick 0x800CF090 clears the bit every tick. OpenCross:
`FieldWorld::follower_back_off` (`src/game/field_world_sim.cpp:579`) does `set_facing(bearing(mx, mz))`, which faces along the move
(away from the leader), and plays the walk forwards. `field_actors.md` 4.2 documents that. Player impact: when the
leader backs into the party, the followers turn around and walk away instead of stepping back while facing him. Positions are
unaffected. Capture: a `ps1_capture.py` state with the leader walking backwards into a follower, a few frames. Done when: rule 1
faces the swung angle `ar` toward the leader, the actor plays the walk clip with a negative rate for that tick only, and the doc is fixed.

### REMOVE_FROM_PARTY (7D): the member who moves up keeps his own position
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): 257 uses, but only about 19 sites let a member move up: kji_b05i, mis_b02e, inj_a03i, mab_a04i, you_b10e, plus 12 removals by character id. The remove-all sites are unaffected. Impact low.
Original 0x800C7CF0: removing slot 0 or slot 1 shifts the slot model buffers up (0x800C74CC) and rebuilds the slot entities.
When a member was in slot 2, 0x800C7A98(1, 2) copies position (+0x3C/40/44), +0x7C/+0x80 and facing (+0xF8/FA/FC) from slot 2's entity
to slot 1's entity, so the member who moved up appears where he stood. 0x800C7A14 then hides the emptied last slot's entity
(flags38 |= 0x10) and clears its binding. The op yields while a model load is pending (byte 0x800EB76C), and in replay mode
it only shifts the tables. OpenCross: `FieldWorld::party_remove` (`src/game/field_world_party.cpp:253`) erases the id and calls
`refresh_party_models`. Entities stay per slot and no pose is copied, so the slot-2 character pops onto the old slot-1 entity's
spot. Reproduce: a cutscene `7D FD`/`7D FE` with three members spread out. Done when: the shift copies the slot-2 entity's pose
to the slot-1 entity, the emptied entity is hidden, and a PS1 state of one such scene matches.

### Auto battle does not follow RDE: target choice, PC rotation, Run/Defend heuristics
Priority: P2
Impact: med; confirmed in code.
Decision (owner): mimic RDE (repeat each PC's last attack target, nothing else).
RDE's auto battle (F5 / L3+R3) forces the command-menu selector `u8 0x800DFC40` to 5/6 in battle.prg at 0x800CB7A8, 0x800CBA64,
0x800CBD68, 0x800CC3AC, 0x800CCB48. State 5 calls `btl_command_repeat_remembered` (0x800CB550), which opens the attack target cursor
(target byte 0x80) on the acting unit's remembered target `u8 0x800E0AAC + unit*0x410`; 0x800C9A30 forces Accuracy Selection = Auto so
the HIT% cursor auto-picks; the menu sounds at 0x800CB8A0 / 0x800CC0B0 / 0x800CC15C are dropped; a debounce gate (host 0x449E50) only
lets auto engage after 9 stable calls of the menu sub-state `u8 0x800E065A`. It attacks only (it does NOT repeat Elements or Items) and
never runs or defends. OpenCross `BattleView::auto_pick` (`src/game/battle/battle_view_ui.cpp:636-687`) picks the lowest-HP enemy,
rotates to the PC with the most stamina, Defends when weak is unaffordable and Runs below 12 % party HP. A player using F5 can flee a
fight RDE would keep fighting and hits a different enemy than the one they last attacked. Spec: `research\docs\reimpl\gameplay\battle.md`.
Repro: auto battle in any fight after attacking the second enemy by hand; compare on RDE with `tools\py\rde_bg.py`.
Done when: auto attacks the per-PC remembered target (`tgt_mem_`), on the PC the game's menu opens for, with no Run/Defend/rotation
heuristics (keep those behind `OC_*` for the test driver only), menu sounds muted while auto drives, and an RDE run of the same fight shows
the same command sequence.

### Fast / slow mode speeds up every screen; RDE only speeds the field and battle loops
Priority: P2
Impact: med; confirmed in code.
Capture (RDE against OpenCross): RDE: the main menu opens in 0.95 s with fast on and off (not sped up). OpenCross side from the code (routes never apply the factor). Shop, name entry and dialogue not captured.
Decision (owner): mimic RDE exactly (field and battle loops only, RDE's text-speed scaling, play time not sped up).
RDE changes only the per-frame logic step count of the field loop (field.prg 0x800AE838 / 0x800AE880, latched at 0x800ADD80) and of
battle (main 0x80035660 / 0x80035684, only while `u8 0x800720C4` and `0x800E3366` are set). Menus, shops, name entry, title and results
run at normal speed; the field latch is forced off while the field input lock `u16 0x800EC4FA` is non-zero. RDE's own text layer
scales dialogue reveal x2.25 (fast) / x0.5 (slow) and divides text waits by 8 (fast) / doubles them (slow) (host 0x43A060, 0x43A154,
0x4464E0). Room 0x208 uses a field fast step of 6 instead of 8 vblanks (bugfix 0x800AE870). OpenCross `App::run`
(`src/app/app_run.cpp:31-37`) multiplies `frame_dt` by `game::speed_factor` (`src/game/cheats.cpp:131`) for everything except movies,
routes and map/battle 386. Spec: `research\docs\reimpl\gameplay\speed.md` sections 0, 2.4, 3.2.
Repro: F2 then open the main menu / a shop / a long dialogue; compare the same on RDE (`rde_bg.py`).
Done when: the factor applies only to field ticks (not while the field input lock or a menu/shop/name-entry screen is up) and battle
ticks; dialogue reveal and waits follow the 2.25 / 0.5 and /8 / x2 rule; play time accrues as on RDE; documented in `docs/architecture.md`.
More (second audit pass): New fact (sound side): the SFX tick in 0x80052790 scales its rate by mode word 0x80092AD4 (1 = 1/4, 2 = 1/2, 4 = x2, 8 = x4 by running
the SFX loop twice per update); music is not scaled there. OpenCross runs SFX at a fixed rate. Settle with `rde_bg.py` whether RDE fast
mode sets 0x80092AD4; if so, the SFX player's tick rate follows the same factor.

### FE 85 cleared-save strip: nibble stride 44 (RDE fixed to 48), grid bits 14-15 lost, gear count 5 / key items ignored
Priority: P2
Impact: med; confirmed in code.
PS1 FE 85 (`0x800D3BF8`) reads and clears each cell's equipped-copy nibble through 0x800D3B88 / 0x800D3AFC, which index
`char*0x16 + cell/2` (44 cells per character). RDE replaces both (`rde_hooks.csv` rows 0x800d3b88 / 0x800d3afc, category
bugfix, handlers 0x449030 / 0x449090) with `char*0x18 + cell/2` (48 cells), the stride the menus and save use. The cell is
masked to `& 0xC000` (bits 14-15 kept). Gear words go through 0x800C21A4: < 0x200 item +1; 0x200..0x3FF element with
count 5 for 0x261..0x26B else 1; 0x400..0x43F key item set. OpenCross `strip_for_cleared_save`
(`src/game/new_game_plus.cpp:155-190`) keeps stride 44 (comment calls it a retail quirk), zeroes the whole cell
(`rec.grid[k] = 0`, contradicting its own comment), adds every element gear word as 1, and drops key-item words.
Player effect: after the ending's save prompt, wrong element stock counts and stale nibbles for every character but
Serge, in the NG+ / Continue+ state. Reproduce: unit test with a counted element on char 5 cell 3 and an element gear word
0x261. Done when: stride 48, `& 0xC000`, the 0x800C21A4 three-way mapping, and `docs/formats/save.md` section 14 state
the RDE behaviour.

### Element shop Sell: placed copies are taken automatically; PS1/RDE make the player pick whose copies
Priority: P2
Impact: med; confirmed in code.
shopmenu 0x801D57D8: the quantity window's max is the bag count only; raising it past the bag count while
0x801D3C9C (party placed copies) > 0 snapshots per-character counts (0x801D3E04) and opens a 30-frame wait then a
two-column list of up to 44 characters; OK takes one copy from the character under the cursor, OK on the OK cell
writes the reduced counts back (0x801D4090) and sells bag qty + picked qty at `(price*total)>>2`; Cancel sells nothing.
Placed copies are counted per cell (0x4000 cells skipped, stack elements by stack size). OpenCross
(`src/ui/shop/shop_logic.cpp:117-133`, `shop_screen.cpp:355`) lets the quantity run up to bag + all removable placed
copies and empties cells in roster order, so the player cannot choose and loses copies from grids without picking them.
Reproduce: a Heal in the bag and one in two grids, Sell Heal, raise past 1. Done when: the per-character list exists,
the sale removes exactly the picked copies, `docs/rules/menus.md` section 8 is corrected.

### Elements > Use: no roster target (Left), party cursor wraps, refusal stays on the target list
Priority: P2
Impact: med; confirmed in code.
menutest `menu_elements_use_loop` 0x801DDA94: state 1 (party target) clamps Up/Down to 0..party-1; Left opens state 2, a
12-row roster scrolling up to 0x20 whose cursor character is written into party slot 0 (0x800786F4) while shown, Right
restores it; OK checks availability (0x800E2D98) and applies the element to that character. After any OK the loop
returns to the element list (state 0) unless copies remain after a successful use (then an animation returns to the
target state). OpenCross `update_use` (`src/ui/menu/main_menu_elements.cpp:13-37`) targets only `party()` with wrap and
stays on the target list after a refusal; `docs/rules/menus.md` section 6 documents that. Player effect: benched
characters cannot be healed from the menu. Reproduce: Elements > Use > Heal > Left. Confirm the state after a refused
OK with a DuckStation capture (`ps1_capture.py`) before changing it. Done when: roster target, clamp, post-OK state
match; menus.md section 6 updated.
More (second audit pass): New facts only: menutest 0x801DDA94 tests and clears `character status word & element +0x18 dword`; heal elements add 50 / 100 / 200 by
pg byte 0xA & 3 (3 adds nothing and still consumes one). OpenCross `use_element` (game_state.cpp:489-507) masks `status_lo() & 0xFF`.
Settle with a pgdat sweep (`tools\py\cc_tables.py`) of usable cure elements for bits above 7, and check the save status field width.

### HD images with `_<lang>` variants are loaded without the suffix (up / osak-010 / memmenu)
Priority: P2
Impact: med; confirmed in code.
RDE: the image loader 0x5B1DD0 builds `<stem><suffix><ext>` from the table at 0x6C2400 (`_en _fr _it _de _es _jp`, index = the language 0x0122F458) and falls back to the plain name. Every `data\hd\*.png` registration goes through it. A sweep of hd.dat / ui.dat finds suffixed families that OpenCross reads by their plain name: `battle/btend/up.png` (en, jp), `battle/shapes/battle-osak-010.png` (en, es, fr), `menu/shapes/memmenu.png` and `memmenu2.png` (de, es, fr, it). The others (menu-main-NNN, battle_33, HomeAnother, system-*, userguide, exticon) are already handled.
OpenCross: `src/game/battle/battle_results.cpp:224` (`up.png`), `src/game/battle/battle_view_hud.cpp:174` (`osak-010`), `src/ui/party_screen.cpp:483` (`memmenu`/`memmenu2` through `png()`, line 126). `ui::lang_suffix` (`src/ui/menu/menu_common.h:76`) is used only for the menu-main shapes.
Player effect: in English and Japanese the battle results "UP" marker shows an arrow where RDE shows the word UP. In de/es/fr/it the party-change frame lines are not cut to the localised captions. In en/es/fr the HIT% box line end differs.
Done when: a shared helper does the suffix-first lookup the way RDE does (one place, so mod paths work too), these three call sites use it, and a results-page frame in English matches RDE (`rde_bg.py`).

### Status-duration modifier of gear flag 0x80 (+4 byte) is ignored
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): only the Resistance Ring 0xE4 (+4 = -1) and Resistance Belt 0xE5 (-2) carry a non-zero value; the Stamina Ring / Belt have 0. Repro: wear 0xE5 and take a timed status (e.g. Aqua Ball flu), read the timer.
Capture (PS1 against OpenCross): PS1: Turn Red cast by Serge on himself (duration stat 10), timer at unit +0x3D0 after the cast: no accessory 29, Resistance Ring 26, Resistance Belt 23 (3 x (10 + modifier) less one step for the cast); OpenCross 29 in all three. Acceptance: the same three numbers.
Original: gear totals 0x800C7DA8 (battle) and 0x801EE850 (btlinit): for every accessory with flag 0x80, +4 (signed: < 0x50 adds, else subtracts `v & 0x7F`) goes into the per-unit short 0x800E3048 (stride 8), +5 into the stamina modifier 0x800E304A. 0x800D6C54 stores target duration byte (+0x1E) + 0x800E3048 at 0x800E3120; 0x800DB764 sets a landed timer to table multiplier x that sum. (The caster-side copy 0x800E313D is written but never read.)
OpenCross: `make_equip` (battle.cpp:123) reads only +5; `set_status_bit` (battle.cpp:949) passes dur_mod 0 to `status_timer` (rules.cpp:200). docs/formats/battle_data.md "Gear flags" 0x80 calls the +4 byte a second stamina modifier.
Impact: Resistance Ring/Belt-type accessories (negative +4) never shorten statuses on the wearer.
Done when: Equip carries a signed `dur_mod`, set_status_bit uses the target's; doc line corrected; one PS1 capture of a timer byte on a wearer (`ps1_capture.py`) as acceptance.

### Sprained attacker (lo 0x10) is not weakened
Priority: P2
Impact: med; confirmed in code.
Original: 0x800D6C54 attacker setup: weapon term = ceil(ceil(wp x Att%/100) x (100 + 10 x (hi >> 29)) / 100), doubled by the hero set, THEN x300/400 when the attacker's lo has 0x10; the MAG term ceil((MAG gear + 1) x MAtt%/100) likewise x300/400.
OpenCross: `weapon_term` (battle.cpp:71-74) and `in.mag_term` (battle.cpp:1143) have no 0x10 test.
Done when: both terms apply x300/400 after the hero doubling for a sprained attacker (weapon hits, weapon techs, ATK and MAG elements, PCs and enemies).

### Enemies sealed in a colour still cast it
Priority: P2
Impact: med; confirmed in code.
Original: after each target 0x800D8F5C calls 0x800D7704, which sets cell flag 0x80 on every cell (PC or enemy) whose element colour is sealed by the unit's lo word (0x88888800 group); the enemy element picks 0x800D1F3C (ops 55 / 8F) skip cells with flags 0x81 or 0x10. The executor also replaces any queued element id <= 0x12D whose record byte 1 bit 0 is set, cast by a unit sealed in its colour, with element 0x32 and message row 0xBE + colour, so a scripted op 67 cast fizzles too.
OpenCross: seals are tested only for PC menus (`colour_sealed`, battle.cpp:464); `pick_element` (pac_vm.cpp:1075) and `cast_element` ignore them.
Done when: pick_element skips sealed cells; the cast path swaps a sealed qualifying element to 0x32 with the message row for any caster; check one Seal cast on an enemy against PS1 (`ps1_capture.py scene`).

### PC countdowns are not lowered by actions
Priority: P2
Impact: med; confirmed in code.
Original: 0x800D7C64(n) lowers the three PCs' countdowns (+0x16C, floor 0, alive only): from the 0x800D7D20 tail by the party action's steps, and with n = 3 after every enemy action. The scheduler 0x800D3C0C gives a PC at 0 whose merged lo has 0x20100000 (frozen / confused) or a pending form change its script turn, else refills it with AP. Pip's form change (mode 45, 0x800D6158) sets his countdown to 21, not 8.
OpenCross: PC countdowns move only in `time_advance` (battle.cpp:1850); `step_after_pc_action` lowers enemies only (1817); `evolve` sets 8 (battle.cpp:730).
Impact: a confused PC almost never gets its random-attack turn while others can act.
Done when: step_after_pc_action lowers PC countdowns by `steps`, each enemy action by 3, evolve writes 21; check a confused PC's turn spacing against PS1.

### Weapon hits do not cure confusion; weapon techs wake nobody, roll crits and apply gear twice
Priority: P2
Impact: med; confirmed in code.
Original: weapon mode 0x800D5060 ends each landed hit with 0x800DBE1C(0): clears asleep (0x4000) and confused (0x100000) with their timers. Weapon tech mode 0x800D50C4 calls 0x800DBE1C(record +0x18 lo mask) per hit (clears both unless the tech's own mask names them), sets the crit range byte 0x800E3113 to 0 so the crit roll rnd(0, 0) returns 0 without drawing (no random crit, hero set included; only the slayer mask crits), and after the per-hit gear steps (0x800DA1D8 -> 0x800D9AE8) runs 0x800D9AE8 again on the total in 0x800DA424.
OpenCross: `do_weapon_attack` clears only asleep (battle.cpp:1082); mode 29 (battle.cpp:1637-1675) wakes nobody, uses strength 1 (an rnd(0,14) draw per hit, crit at >= 7 with the hero set) and applies the gear steps once per hit.
Done when: weapon hits also clear confusion; mode 29 wakes per hit unless masked, makes no crit draw (slayer only) and applies x125/x75 again on the total; docs/rules/battle.md 6.3 and battle_data.md section 10 updated.

### Customize Vibration / Left Stick rows: what RDE's emulated pad reports
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (RDE against OpenCross): RDE with keyboard input: the cursor lands on both rows, they draw normally and Left / Right toggle them. OpenCross greys them and ignores Left / Right when no pad is connected.
Decision (owner): match RDE: both rows always active and toggled by Left / Right, pad or not.
The PS1 cursor skips row 6 unless 0x801E81E4 (pad has actuators) and row 7 unless 0x801E820C (analog pad), separately;
the rows are never landed on. OpenCross lands on both and greys them on `pad_connected` (`main_menu_settings.cpp:55,84`).
Neither function is hooked by RDE, so the answer depends on the pad RDE emulates with keyboard vs controller. Capture:
`rde_bg.py` Customize screen with keyboard only and with a pad, cursor stepped through rows 5..7. Done when: gating
matches RDE for both input kinds; menus.md section 2 says so.

### Save loader ignores the checksum and the version gates
Priority: P3
Impact: low; confirmed in code.
Decision (owner): match the original: refuse a bad-checksum block with its message and honour the version gates.
`menu_save_unpack_state` 0x801F1B1C reads the version at 0x1FF8, checks the checksum (0x180..0x1FF7, plus 0x000..0x17F when
> 0x102; failure = "data corrupted", load result 2) and gates fields: > 0x100 two bitmaps, > 0x101 per-character +0xA8
bytes, < 0x104 frames mask 0xFF, < 0x105 innate colours 8, < 0x106 NG+ words 0. OpenCross `decode_block`
(`src/save/cc_save.cpp:288`) decodes all 54 steps unconditionally (`docs/rules/menus.md:237` admits it), so an old PS1
card decodes garbage tail fields. Done when: gates + defaults in `decode_block`, a bad checksum shows the corrupted state
like RDE, unit tests with a 0x102 block and a corrupted block.

### Element mode 40 (Suicide Bomb / Toxic Burst) skips the hit pass: no status, no immune / absorb
Priority: P3
Impact: low-med; confirmed in code.
Original: 0x800D5CA8 runs the full mode 0 hit pass 0x800D44C4 (status roll, E-bit rules, colour step) and only when the target's hi word
lacks 0x10000 (immune) replaces the caster's and the target's value by target HP / 2; the hit pass's result code stays (absorb = code 3
heals, immune = miss).
OpenCross: battle.cpp:1482-1494 calls `element_damage` and always `damage_unit(actor, target, hp / 2, 0)`; `apply_status` is never called,
though its comment says "unless it missed". Player effect (record read): Toxic Burst (387, poison 100 %) never poisons; an immune or
absorbing PC loses half its HP. Done when: mode 40 goes through the mode 0 pass and overrides the value only when not immune, keeping
code 3; check record 387 / 389 in pgdat with `tools\py\cc_tables.py` first.

### Pip's form change does not make his used tech cells usable again
Priority: P3
Impact: low-med; confirmed in code.
Original: mode 45 (0x800D6158) rewrites each of the form's tech cells (27, 47, 67; the third only for a second form) through 0x800D6044,
which writes the new id into the battle grid and the unit's cell word and clears bit 0 (used) of the battle cell flags (0x800E0874 area).
OpenCross: `Battle::evolve` (battle.cpp:718-751) rewrites `unit.cells` and the record cell but leaves `unit.used[cell]` set.
Player effect: a tech Pip used before evolving stays spent for the rest of the fight. Done when: evolve clears `used` for every rewritten
non-empty cell; a unit test covers it.

### Emitter death children ignore the dying object's op-0x39 basis (and its local position)
Priority: P3
Impact: low-med; confirmed in code.
main 0x8003CA70 (death spawn): when the dying object's own header has an op 0x39 record (found with 0x800307B0), it builds that basis with 0x80032154, uses the dying position as its translation only when the object has flag bit 19, and publishes it in 0x80072128 for the child spawns, so the child's parent-relative ops (0x08, 0x0A, 0x0B, 0x0C, 0x27, 0x2F, 0x41, 0x3B / 0x42 / 0x46) are rotated by it. With that basis, a dying object with flag bit 15 (local coordinates) and a child without bit 15 gets its position run through the basis (0x80042168) before it is copied to the child.
OpenCross `EmitterSystem::Impl::die` (`src/game/effects/emitter_spawn.cpp:619-646`) calls `fire(dying.death, nullptr)` (no parent basis) and copies `dying.pos` as it is (:636-637).
Data scan: 504 of 5857 death children of op-0x39 parents use velocity / acceleration ops without their own 0x39 (robo2, saigai, misso, 37comet, hasira, ...), mostly symmetric sprays; exactly 13 chains have the local-position case (syoukan/thndr tam3->tam4, tam6->tam8; tech/balls sb01->sb11, sb0-->sb1-; tech/ganseki iw01->iw04, iw11->iw14, iw21->iw24; tech/kagou san1->san2, mar1->mar2; tech/oodama 5-0, 2-0, 1-0, 0-0 -> *-01).
Done when: `die()` passes the dying header's op-0x39 basis (translation = dying position for bit 19) and converts a bit-15 dying position to world for a non-bit-15 child; a `ps1_cast.py force` capture of thndr (Thunderstorm) and robo2 / 37comet matches child positions (fx_diff).

### D4 takes its wipe variant from the wrong operand (skipping stays allowed)
Priority: P3
Impact: low-med; needs a capture or data sweep.
Data use (scan of the shipped data): 11 of 20 D4 lack 0x200 (crn_b16i, jac_b20i, kji_b06i, sui_a09e, ten_b07f, you_b10e) and are unskippable on the PS1 while OpenCross lets them be skipped; RDE check: crn_b16i row 6. The variant is 0 at every D4 and 7E, so the docs' "variant 1 in the data" is wrong.
Capture (RDE against OpenCross): RDE: crn_b16i row 6 (`D4 #6,#8`, no 0x200) played its full 51 s through 17 confirm presses; the same movie patched to `#520` was skipped within 3 s. OpenCross skips both. (The movie events start only when crn_b16i is entered by entrance 2.)
Decision (owner): every movie stays skippable (architecture.md 13); fix only where D4 takes its wipe variant and flags from.
Original 0x800C85E4 (D4): row = U@1 & 0xFF, flags = U@3 (+8 with the DC movie-end jump), variant = U@3 & 3. Rows 3/4 switch to the
alternate mode. The runner 0x800DD130 lets the player skip only when flags & 0x200 (or the debug byte). 7E (0x800C8400): variant = high
byte of U@1, flags 0. OpenCross: D4 (`field_vm_ops_game.cpp:355-360`) takes the variant from U@1's high byte and is always
skippable. `MovieRequest::variant` is unused (`field_world_audio.cpp:185-197`). The New Game `D4 #2,#520` has 0x200 set. RDE plays
movies through its own player (0x800DD130 is only an observer hook), so RDE decides. Steps: list every D4's U@3 with `cc_fieldscript.py`.
For any D4 without 0x200, check in RDE (`rde_bg.py`) whether confirm skips it. Done when: D4 follows RDE and the `fieldscript.md` D4 row is right.
More (second audit pass): The "any new pad bit skips, then 8 more frames" rule read in 0x800DD130 sits in the variant-1 branch, which no movie op in the data uses; the variant-0
branch is cut off in the export. So which keys skip is RDE's decision: OpenCross skips on Ok / pad South only (`app_sim.cpp:438`). Add to the RDE check:
whether d-pad, Triangle or other keyboard keys skip a `#520` movie.

### Mojo's default name does not become "Mojoy" after he learns his tech (FE 7E)
Priority: P3
Impact: low-med (only a player who kept the default name sees it); confirmed in code and data.
Not the recruit rename: that is FE 4A at alu_b09i 08AC (entity 6, right after ADD_TO_PARTY #27), fixed earlier, and naming Mojo works.
FE 7E is a later, separate scene in the same room: entity 8's ON_ACTION, only with Mojo in the party (JUMP_IF_NOT_IN_PARTY #27 at 09D1),
messages 53..55 ("A lucky...doll...?", "Mojo changed for the better... Just a little bit.", "Mojo learned <tech>!"), RECEIVE_ITEM #753 at
0A69, then FE 7E at 0A6D; it is the only FE 7E in the data. PS1: FE 7E turns the default kana record name "Lucky Dan" into "Happy Dan".
RDE hooks it (field 0x800D57E0, handler 0x424BD0, field overlay only): if the custom-name map has an entry for character 0x1B it does
nothing; otherwise it stores the `cmnmes` row keyed "ハッピーダン" for the current language as his custom name, at once, saved through
ext_save (EN "Mojoy", FR "Mojoie", IT "Mojoy", DE "Frohjo", ES "Buduy", JP "ハッピーダン"; the row is in `lang.dat` `cmnmes.txt`).
It never looks at the record name, so a player who typed a name keeps it in both games. OpenCross (`src/game/field_world_tick.cpp:206-215`)
only patches the two kana bytes of the record, and non-JP display names come from the custom map or sysmes (`src/data/char_name.cpp`),
so a player who kept "Mojo" still reads "Mojo" after this scene where RDE shows "Mojoy". This interacts with the name-entry entry
("confirming the stock name pins it"): RDE erases the custom name when the confirmed name equals the default, OpenCross stores it, so
even with FE 7E fixed OpenCross would skip it unless that entry is fixed too.
Reproduce: recruit Mojo and confirm the default name, then trigger entity 8's scene in alu_b09i with Mojo in the party (EN); compare
the name in the main menu with RDE (`rde_bg.py`).
Done when: FE 7E inserts custom_names[27] from `cmnmes` when absent (all six languages), shows at once, persists through ext_save,
the name-entry default rule matches RDE, and the fieldscript.md FE 7E row says so.

### Script instruction budget: 3C / A9 / FE 78 add 1; 3C / A9 wait out the room-entry window
Priority: P3
Impact: low-med; confirmed in code.
Data use (scan of the shipped data): no run of consecutive bonus ops exceeds 5, but about 216 straight-line segments in 104 rooms cross the 8-op budget (approximate yielding-op list). Repro: crn_b16i 07A0, bak_a02f 029C.
Capture (PS1 against OpenCross): PS1 (patched alu_a04f): with 3C, A9 or FE 78 in front of a run of 24 / 11 / 9 marker ops the first tick runs 9 ops, OpenCross 8; a control run of 22 markers with no bonus op splits at 8 on both. The room-entry window part was not captured.
Original: the dispatcher 0x800D58CC runs up to the budget (0x800EC778, 8 + bonuses) per record and tick. 3C (0x800C2D20) adds 1 when the
entity has a 3D model (flags38 & 0x4000 clear). A9 (0x800C2DE4) adds 1 whenever the target resolves. FE 78 (0x800BF4A0) adds 1 when the song
started (normal mode). 3C and A9 yield without advancing while the room-entry counter 0x800EA4DE is non-zero (2 frames after a load or
a battle return). OpenCross adds +1 only for 88 (`field_vm_ops_scene.cpp:251`) and the 55/56 false branches. 3C
(`field_vm_ops_core.cpp:390`), A9 (`field_vm_ops_game.cpp:104`) and FE 78 (`field_vm_ext.cpp:438`) neither add nor yield,
although the `fieldscript.md` rows 3C and FE 78 say they do. Player impact: cutscene chains of more than 8 non-yielding ops that
include 3C/A9 split over two ticks in OpenCross, so poses land one tick late. Done when: the three budget bonuses and the 3C/A9
entry-window yield exist (`entry_window_` in `src/game/field_world.h:841` already counts the window). Check with a script-op trace on
`alu_chair` and `art_swap` (the timing of the first following WAIT).

### A3 / C9 reached while a battle is pending is dropped; the script gate is a subset
Priority: P3
Impact: low-med; confirmed in code.
Data use (scan of the shipped data): 683 A3 / C9 sites in 221 rooms; 150 rooms have battle ops in more than one entity. Whether two fire on the same tick needs a capture: touch two enemies of tok_a01f / mab_a01f / des_a06f in one frame.
Capture (PS1 against OpenCross): PS1 (staged in alu_a04f, encounter 1 on battlefield 0): the second entity's A3 waited, a second battle started about 200 frames after the first ended, and that entity's script went on only after its own fight. OpenCross fought one battle and the second script carried on as if it had fought. One PS1 run; repeat on a real double fight to make it firm.
Original: A3 (0x800C3348) and C9 (0x800C30A4) return without advancing while the early-out byte 0x800EA48C is 1 or the system gate
0x800C47E4 is busy. That gate is the about-17-flag chain 0x800C4798 (battle/encounter, menu, movie request 0x800EB760, room jump
0x8008537C, model swap 0x800EB76C, shop / smith 0x800EB768 ...) plus one more term (0x800C44A0, byte 0x800EA4D0). 07/08/09 hold on 0x800C4630.
The launcher ops (FE 4A/59/5E/67/68/72/84/85) use 0x800D47FC, which is the system gate OR any of the 4 dialogue windows open (0x800D5EAC).
OpenCross: `FieldWorld::system_ready` (`src/game/field_world.cpp:535`) is only "no transition and no jump". `request_battle`
(`field_world_transition.cpp:161`) silently returns while `battle_state != None`, but A3/C9 (`field_vm_ops_game.cpp:73-95`) still go to phase 1,
wait for the first fight and advance. So a second entity's battle is never fought, and its script continues as if it had been
(it reads the first fight's `$32`). `skip_battle` can also overwrite the running fight's `last_battle_code`. `idle_gate`
(`field_vm_ext.cpp:17`) ignores open windows. Menu and movie requests are mostly covered because `App` stops ticking the field while
they are pending (`app_sim.cpp:619-626`). The battle intro (`enct`) is not covered: the field keeps running during it.
Done when: `system_ready()` (or a battle-op gate) also refuses while `battle_state != None`, `idle_gate` refuses while any dialogue or label window
is open, and a unit test with two entities issuing A3 on the same tick shows the second one waiting and then fighting.

### "No encounters" auto-win does not write variable 0x32 = 2 as RDE does
Priority: P3
Impact: low-med; confirmed in code.
Data use (scan of the shipped data): variable 0x32 is only read (644 compares: 0 x273, 1 x85, 2 x284, 3 x2). In the four lose-and-retry loops (mja_a04f, mja_b04f, ryu_b29i twice) 2 is the "lost, retry" branch. Capture: ryu_b29i `BATTLE #432,#48,#80` with encounters off on RDE.
Capture (RDE against OpenCross): RDE: writes 2 to variable 0x32 on the auto-won encounter entering room 53 (opa_b02f); OpenCross leaves the old value (5). RDE fights ryu_b29i's C9 #432 normally even with encounters off. The OpenCross skip also logged three times for one room entry where RDE replaces the battle op once.
RDE: the encounter hook writes host 0x00B1AD66 = 2 (guest 0x800EBADA = script
variable 0x32, `fld_battle_result`) and the outcome byte 0x80075909 = 2, then patches in `09 00 02`. OpenCross:
`FieldWorld::skip_battle` (`src/game/field_world_transition.cpp:148-155`) sets only `handoff_.last_battle_code = 2`. The A3 row and section
6.11 of `fieldscript.md` (line 520) say "variable 0x32 is untouched", which is wrong. Player impact: with the cheat on, a script that tests
`$32` after the skipped op takes the branch of the previous fight (usually victory) instead of the fled branch. Done when:
`skip_battle` also sets `kBattleResult` to 2, the doc line is corrected, and an RDE run (`rde_bg.py`, encounters off) of one table
entry is compared.

### AKAO C9 ends an evicted protected channel; the PS1 ends it only at CA
Priority: P3
Impact: low; confirmed in code.
Original: only the VM's CA path tests flag 0x200000 (set when an E0-protected channel is evicted) and finishes the channel; C9
0x800553EC has no such test. OpenCross `case 0xC9` (`akao_player.cpp:931-941`) ends the channel when `stop_at_loop` is set even with
passes left. Data: E0 plus C9 in `battle/monster/ki`, `komodo`, `opthe` at0 `chit`. Done when: the `stop_at_loop` test leaves C9
(the peek already matches the PS1).

### AKAO E4 / E5 / E6 rate slides: no 0 -> 256 target substitution, PS1 rounding
Priority: P3
Impact: low; confirmed in code.
Original: E4 0x80054A30 (E5 / E6 alike) substitutes 0 -> 256 only for the slide length; the target is `operand << 10` and the step comes
from 0x80053D1C: the current rate's low 10 bits are cleared, step = (target - cur) / n, plus 1 when rising, minus 1 (and the low bits
filled) when falling. OpenCross (`akao_player.cpp:1004-1013`) maps target 0 to 0x40000, keeps the low bits, no bias. Data: target 0 in
`bak_a14e` frame 1, `tech/onedari` se00, `ryu_b18i` / `ryu_b29i` frame 13 (rate slides to stop vs to max). Done when: target unsubstituted,
step per the PS1 rule.

### Miss pops of element modes 1 and 36 are inverted
Priority: P3
Impact: low; confirmed in code.
Original: mode 1 (0x800D4580) sets the result code 0, then `landed` from the status roll (or 1 for a cure, E bit 0), and 0x800D99A4 makes
the code 2 (MISS) when it did not land. Mode 36 (0x800D5988): only a failed `rnd(0,99) < +0xA` gate clears `landed`; a passed gate sets
the curse through the unconditional setter 0x800DBCB8 and keeps code 0 even when every bit was already on.
OpenCross: mode 1 (battle.cpp:1579-1583) sets `landed` only, so a failed Turn colour / Negate / Infoscope shows nothing; mode 36
(battle.cpp:1337-1344) sets code 2 both on a failed gate (right) and when `force_status_bits` set nothing new (wrong: a hit on the PS1).
Done when: mode 1 sets `code_override = 2` when `!landed`; mode 36 sets code 2 only on a failed gate.

### Emitter death children are stepped in the frame they are born
Priority: P3
Impact: low; confirmed in code.
main 0x8003CA70 runs the object step 0x80038E60 on a new death child with the frame tick count forced to 0 (matrices only) and, when the child sits after the parent in the task list, installs 0x80041F20 so the loop's own step is skipped that frame: a death child first ages one frame after its parent expires.
OpenCross `step_frame` (`src/game/effects/emitter_tick.cpp:256-270`) walks `objs` up to the live size, so an object appended by `die()` during `update()` gets `ticks` in the same pass. Each generation of a death chain runs one frame ahead (compounds in chains; shows in fx-audit counts).
Done when: objects created by `die()` skip their birth frame's update (spawn-with children keep their single immediate step), and `opencross ocbattle fx-audit` / `pac-soak` stay clean.

### Light objects (type 0x11) with blend 0..4 use the op-05 operand as slot and are never released
Priority: P3
Impact: low; confirmed in code.
main 0x80039A00 op 0x05: the type-specific setup (light slot hunt, release flag bit 31, also the 0x16 sound start) sits inside `(object flags & 7) > 4`; for blend 0..4 the slot stays the op-05 operand (1 in all 47 shipped lights) and nothing clears it when the object dies.
OpenCross allocates a slot for every type-17 object (`src/game/effects/emitter_spawn.cpp:493-503`) and zeroes the slot of dead lights (`emitter_tick.cpp:303-311`); it already applies the same `> 4` gate to type 0x16 (`emitter_spawn.cpp:69`).
Data: 20 of 47 light emitters have blend 0 (dark / darkdark zlg1 lt2 l0 lr0, tekkyu lt, fin_02 litm, rooms bak_a12f, bak_a17f, jac_a29i, jac_b17i lgt0-2, kji_b09e, ryu_a24i, sui_a09e, ten_b05f). In jac_b17i the three lights share slot 1 on the PS1.
Done when: blend <= 4 lights use the operand as slot without release; a DuckStation screenshot of jac_b17i (`ps1_capture.py scene`) and a `dark` cast match OpenCross lighting; `docs/formats/room_effects.md` 3.6 states the rule.

### 3mix (Delta Force) return faces the casters' mean, the original their bounding-box middle
Priority: P3
Impact: low; confirmed in code.
3mix return task 0x801C6190 (and uzumaki 0x801C61B8) take the caster point from 0x80031A5C selector 3, which runs 0x8002AFDC: per axis (max + min) / 2 of the casters' integer positions; final yaw = -0x400 - ratan2(Cz - z, Cx - x).
OpenCross `Return::step` (`src/game/effects/bespoke/tech_ports.cpp:333-337`) uses the arithmetic mean (float). Same for two casters (uzumaki); differs for Delta Force's three unless symmetric (a few degrees of final facing). `docs/formats/battle_effects.md` already states the bbox rule for lasbos 0x107.
Done when: the centre is (max + min) / 2 per axis on the s16 positions; a `ps1_cast.py force` capture of Delta Force shows the same final yaw of each target.

### 11boru (Volcano) phase-1 piece rotation restarts from 0 instead of the init roll
Priority: P3
Impact: low; confirmed in code.
11boru init (0x801C7178) rolls rot0 = r % 200 - 100 per axis into 0x801C87C0; the frame (0x801C7BC8) phase 0 only reads it (rot0 * t16 >> 11, so the piece ends phase 0 turned by rot0), phase 1 adds the rate (0x801C8810) to that same array.
OpenCross `ground_ports_b.cpp:294` adds the rate to `piece.rot`, zero-initialised (:370) and never seeded: every piece snaps back by up to +-100/4096 per axis at the phase change.
Done when: `piece.rot = rot0` when phase 1 starts; a `ps1_cast.py force` capture of Volcano around tick 128 matches piece rotations.

### cook / swordbox vacuum front: model-y term has the wrong sign
Priority: P3
Impact: low; confirmed in code.
cook vacuum task 0x801C6040: thr = (a1 t / dur - a1) - (model y >> 12) + H/2; the per-vertex tau uses + model y.
OpenCross `Vacuum::Task::step` (`src/game/effects/bespoke/model_deform.cpp:376`) adds `my`; same in the comment at :311 and `docs/formats/battle_effects.md:224`. Identical for ground targets (y 0); off by 2 y for a raised target.
Done when: `thr` uses `- my`, the comment and doc follow; a target with model y != 0 is found (battle placement data) or the entry closes as cosmetic.

### fnoise frost-stage quads sort by AVSZ4; the original uses AVSZ3
Priority: P3
Impact: low; confirmed in code.
fnoise quad builder 0x801C8E1C (0x180 model draw) uses AVSZ3 (GTE 0x158002D: v1 v2 v3) or min SZ with byte-3 bit 0x10; only the shard quads (0x801C67A0) use AVSZ4.
OpenCross `mesh_techs.cpp:146-147` sets `spec.quad_avsz3 = false` ("Quads use AVSZ4") in `Fnoise::draw`.
Done when: that line and comment are removed (default true); shards keep the 4-corner mean.

### The user guide's "ESC Menu (LT + L3)" pad chord does not open the settings window
Priority: P3
Impact: low; confirmed in code.
Decision (owner): LT+L3 opens the Esc menu, as RDE.
NEW.
RDE: `sys_poll_hotkeys` (0x5860F0) sets the Esc-menu input when the host mask has LT (0x400) and L3 (0x4000) both down, with
one of the two newly pressed this frame, OR'd with the Esc key; the menu toggles on the RELEASE of that input (current 0, last
frame non-zero). Back+Start and "hold Start" do nothing on RDE; Start goes straight to the guest, so the PS1 pause opens on the press.
OpenCross: `src/ui/keymap_screen.cpp:46` draws "ESC Menu (LT + L3)" on the pad page, but `App::frame_hotkeys`
(`src/app/app_run.cpp:111-133`) opens settings only on Esc press or Start held 500 ms; LT+L3 just toggles slow mode and
no-encounters. The guide tells the player a chord that does not work. Esc firing on press (not release) and the Start tap
pausing on release are documented choices (`docs/tools.md` 486); list them in `architecture.md` 13 if they stay.
Open point (rde_bg.py with an injected XInput state): whether the LT+L3 chord on RDE also flips slow mode and no-encounters on
the way (the LT and L3 edges also feed those toggles in the same function).
Done when: LT+L3 opens / closes the settings window on release (alongside Start-hold), the RDE run decides the side toggles,
and tools.md / menus.md name the chord.

### A refused key item does not write $2A (0x43F on the PS1); invisible unless a script copies it
Priority: P3
Impact: low; needs a capture or data sweep.
Data use (scan of the shipped data): no script compares $2A with the gated item values; only 29 copies of $2A in 18 rooms could show a difference. Capture: refuse the Smith Spirit next to a touch trigger in alu_a04f (03D8) or alu_b01f (02D8) on RDE.
Capture (PS1 against OpenCross): PS1: a refused Smith Spirit sets $2A = 0x43F and the shown byte; closing the list clears the byte without firing any key-item event, so nothing leaks into the next trigger pass. OpenCross matches what the player sees (no event) but leaves $2A unchanged. Only a script copying $2A right after could differ.
Original 0x800D6AAC: on confirm, it writes var 0x2A = 0x401 + entry and byte 0x800EBAA3 = 1 BEFORE the usable test. A gated item (Tele-Porter,
Relief Charm, Smith Spirit without `$46 == 1 && $3C == 1`) then plays sound 0x18 and the list stays open. 0x800AE354 clears 0x800EBAA3
only for 0x401/0x43D, so a refused Smith Spirit leaves it set for the next trigger pass (key-item event 4 on touch-flag
entities in range). OpenCross: the popup refuses disabled entries itself (`app_sim.cpp:575-578`), so `use_key_item` (which writes $2A) is
never called. Capture: in RDE (`rde_bg.py`) or `ps1_capture.py`, refuse Smith Spirit next to a touch-trigger entity and read $2A.
Done when: OpenCross matches the capture.

### Accessory list: the PS1 always leads with an empty entry and lists bag materials greyed at the end
Priority: P3
Impact: low; needs a capture or data sweep.
Data use (scan of the shipped data): the conflict rule never matters (every accessory slot mask is a single bit or 0). The materials tail does: 22 material ids (1..0x16) exist and 69 of the 72 accessories have a non-zero high nibble in their category byte, so a capture of the RDE accessory list with a material in the bag is still needed.
Capture (PS1 against OpenCross): PS1 (JP disc 1, forged save): the list always starts with one empty entry, even when the slot is empty, with the cursor on it; then the bag's accessories, then every bag material greyed at the end, both in bag order; confirming a material does nothing. Accessories with category byte 0x62 are listed, so a literal "category byte == 2" rule is wrong. OpenCross shows no empty entry for an empty slot and no materials. The conflict rule was not captured (the data scan says it cannot change any list).
Original 0x800E3DF0, accessory mode: it includes every bag id < 0x20 with no category test, and ids >= 0x20 when the full category byte
(+10) == 2. When the candidate's conflict mask overlaps the accessory being replaced, it skips the conflict test entirely. 0x800E3C9C
puts ids < 0x20 after the real gear, and the list starts with one empty entry. OpenCross `equip_candidates` / `accessory_conflict`
(`src/ui/menu/game_state.cpp:535-566`) masks the nibble, hides materials and still tests the other two slots. Capture: the RDE equip
screen (`rde_bg.py`) with materials in the bag and two overlapping-mask accessories. Done when: the list content and greying match RDE.
More (second audit pass): New facts only (menutest 0x801E599C renderer): entries with 0x4000 are not drawn, so the leading reserved entry is an invisible but
selectable first row that equips item 0 (take off); entries without 0x8000 are drawn grey 0x404040 (equippable 0x808080); the count column
shows the bag byte, or 2 for item 0x40; the weapon and armour lists have no reserved entry and compare the category nibble only.

### Fast / slow mode is not suspended during the battle intro, lasdes2 and miyademo
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (RDE against OpenCross): RDE: the battle intro runs at 1x with FastMode on, doubling only after the intro; vblanks to the first command menu are 307 with fast on and off. OpenCross applies the factor in the intro (from the code). lasdes2 and miyademo not captured.
Decision (owner): mimic RDE (intro at 1x).
PS1: in battle (0x800720FD == 1) with the time-shift gate 0x800720DB set, the main loop's L2 / R2 speed change (dt doubled or
tempo -1) runs only while byte 0x800720C4 is non-zero. btlinit (0x801F2BEC) clears it, btl_intro_layer_stop (0x800B08D8) sets it at
the intro end, btl_stage_layers_restart (0x800B0964) clears it, lasdes2 and miyademo save / zero / restore it. The symbol
`sys_time_shift_gate` says this L2 / R2 path is RDE's slow / fast mode, so RDE fast mode should run at 1x during the intro, the
Time Devourer ending and miyademo. OpenCross: `src/app/app_run.cpp:34-36` + `src/game/cheats.cpp:125-130` apply the factor in any
battle except map 386 (`App::speed_locked`, app_run.cpp:161-164). Capture: `rde_bg.py` with FastMode on, time a battle intro and
the lasdes2 chain (refresh counts) against FastMode off; also find when btl_stage_layers_restart runs (outro?).
Done when: the RDE timing shows whether the gate holds; if it does, a battle-side "speed allowed" flag drives `speed_locked()`.

### opthedo exit (Portalgheist, element 359) leaves the target at yaw 0xA00; the PS1 snaps it to 0xC00
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): PS1 scene 84: the caster is the Portalgheist door (unit 5, element 359), not a party tech; target pup = unit 3. Same overshoot point (-909, 1651) and home (-1280, 1280) on both sides; pup's final yaw PS1 0xC00, OpenCross 0xA00.
PS1 opthedo exit (0x801C6200): every target-list unit gets yaw +0xA6 := 0xC00 and position := its home point. OpenCross `Opthedo`
(`src/game/effects/bespoke/tech_ports.cpp:105`) has no exit; the host returns units from where they stand. Capture the end of the
tech with `ps1_cast.py force` (owner character) and `tools\ours_cast34.ps1`, diff facing and position of each target.
Done when: either the host's end-of-action reset already gives the same yaw / position (close) or the port adds the exit.

### Weapon specular glint only near the camera on the PS1
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): PS1: frames with a glint: `apr0` 0/131, `atk0` 12/323, `zen0` 3/76, `atk5` 91/92, idle orbit 299/300; the test is per weapon part, |eye - part| < 0x1000 on every axis. OpenCross glints on every frame.
`sys_weapon_draw` (main 0x800A0104), non-prism weapon: the specular dispatcher `sys_weapon_draw_plain` (0x800A0580, GT3 / GT4 only)
is installed only when |eye - weapon position| < 0x1000 on all three axes (eye = `sys_weapon_eye_pos` 0x800A7E10); otherwise the
stock dispatcher draws every group type with no glint. OpenCross `src/game/battle/battle_view_draw.cpp:357-368` always sets
`wd.light.spec`; `docs/formats/model.md` 7.1 states the GT3/GT4 filter unconditionally.
Repro: a battle with a plain weapon in a wide and a close camera shot; PS1 (`ps1_capture.py scene`) vs `ocbattleview --shotevery`.
Done when: the distance gate is added (or shown never to fail in battle cameras) and model.md 7.1 states it.

### Frozen landing adds 3 to the countdown; does a frozen enemy's countdown run?
Priority: P3
Impact: low; needs a capture or data sweep.
Capture (PS1 against OpenCross): PS1 scene 6, unit 4 (Ice Blast freeze forced): countdown 1 -> 3 when Frozen lands (+3, then the cast's own -1) and keeps dropping with every party action (3 -> 2 -> 0); the Frozen timer starts at 12, drops by attack strength + 1 and thaws at 0. OpenCross skips frozen units in the countdown and adds nothing. Not seen: whether a frozen enemy at 0 takes its turn.
Original: 0x800DB764, when lo 0x20000000 newly lands with a timer: unit flag +0x1CF := 1, message flag 0x800DF7F4 := 1, countdown += 3. The scheduler gives a frozen PC its script turn at 0; enemies keep their turn script.
OpenCross: Frozen is `kDefeated` (battle.h:166), so a frozen unit is out of `alive()`: no countdown, no turn; no +3.
Settle with `ps1_capture.py scene` freezing an enemy (Ice Blast type) and logging its countdown word (0x800E080C + 0x410 x unit) until thaw. Done when: the +3 is added and the frozen unit's countdown/turn behaviour matches the capture.

### Japanese equip panel: the "%" after HIT / EVD and the preview arrows are missing
Priority: P3
Impact: low; seen in a PS1 capture.
In Japanese the PS1 equip panel draws a "%" after the HIT and EVD values and the preview arrows; OpenCross leaves both out (`src/ui/menu/main_menu_equip.cpp`). Repro: JP, main menu > Equip, any character. Done when: a JP frame matches the PS1 panel.

### Target cursor wraps in NORMAL camera; retail stops at the ends
Priority: P3
Impact: low; confirmed in code.
0x800C26CC steps through 0x800C2300; with the dynamic camera flag 0 (0x800758B4) it calls 0x800C1F5C, which picks the nearest
candidate strictly right/left of the current one in projected screen x and returns the current unit when none exists (no wrap).
With the flag set, 0x800C2300 sorts candidates by -atan2 of their world position and wraps. OpenCross `battle_view_ui.cpp:882`
`step` wraps in both modes (and also steps on up/down: `:1096` passes `vdir + hdir`); `target_candidates` sorts by screen x for
both modes. Reproduce: 3 enemies, Attack, Right on the rightmost. Done when: NORMAL does not wrap, the DYNAMIC order and its
left wrap are taken from a PS1 capture, and docs/rules/battle.md section 3 states both.

### Negative-stamina bar is drawn empty
Priority: P3
Impact: low; confirmed in code.
0x800BECC0: for negative stamina the value is negated, capped at 70, fill width = v*100/140 over the 52x4 track, colour from the
byte table at 0x800DE808 index (v/10) + 8 (the second half used for "out"). OpenCross `battle_view_ui.cpp:479` sets the fill to 0
when out. Done when: the fill uses |stamina| and the out colours, whose values come from a PS1 capture (or a dump of 0x800DE808)
of a PC below 0 stamina.

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

### Name entry: Default ignores the stored custom name; confirming the stock name pins it
Priority: P3
Impact: low; confirmed in code.
RDE's init/Default hook (namemenu 0x801D31AC, handler 0x44E550 -> 0x44C9F0) loads the custom-map entry for the
character, else sysmes 0x10000+id, so Default restores the name as it was when the screen opened. RDE's confirm hook
(0x801D3764, handler 0x44E6E0 -> 0x44CB90) erases the map entry when the trimmed text equals the current-language
default, else stores it. OpenCross: Default sets `default_` (`src/ui/title/name_entry.cpp:133`, default from
`game_flow.cpp:325`); `FlowMachine::set_custom_name` (`src/game/game_flow.cpp:56-62`) always stores. Player effect: a kept
default name stops following a language switch and lands in ext_save; Default on a renamed character jumps to the stock
name. Done when: Default uses `initial` when set; confirming the language default erases the entry; menus.md 9/14
updated; unit test.

### Customize input: Left/Right should XOR the row, OK should only open sub-pages, skin preview ignores Cancel
Priority: P3
Impact: low; confirmed in code.
`menu_settings_loop` 0x801EA27C state 0: a Left or Right edge calls 0x801E828C (options word ^= row mask); OK acts only on
row 1 with Custom frame (skin preview) and row 4 with Custom controller (button page); state 1 (skin preview) leaves
only on OK. RDE only observes this function (HD backdrop). OpenCross `update_settings`
(`src/ui/menu/main_menu_settings.cpp:47-71`): Left clears, Right sets, OK toggles any row, the preview closes on Cancel
too; `docs/rules/menus.md` section 2 documents ours. Done when: XOR on Left/Right, OK gated as above, preview Cancel
inert; menus.md section 2 corrected.

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

## 2. Fidelity gaps (not modelled)

### HD art: battle PCs use the 4x `hd/menu/player/<name>.tim` texture in RDE
Priority: P2
Impact: high; needs a capture or data sweep.
Capture (RDE against OpenCross): RDE (rde_cast): the left half of `hd/menu/player/<name>.tim`, downscaled 4x, is the same art as the cdrom2 `.ltc` OpenCross uses (Serge, Kid, Karsh: mean colour difference 5..7 of 255, same atlas layout); the TIM needs its own CLUT. So a result that "looks off" comes from the pairing (CLUT, mesh) or 8x minification, not from the art. At the battle camera at 1280x800 RDE and OpenCross look equally sharp; texel density was not decided.
Decision (owner): match RDE in HD art: the left half of the TIM with its own CLUT on the battle model plus the 4x face cells; verify with an RDE frame diff. Classic keeps the `.ltc`. Add a setting so players can keep the PS1 textures in HD art: `[Video] HdBattleTextures` (bool, default 1 = RDE's 4x TIM; 0 = the cdrom2 `.ltc` at 1x, as today), applied live like the other art options, with a row next to Art mode in the settings window; document it in `docs/formats/config.md` (sections 2.2 and 4). Fix `docs/formats/model.md` ("Battle models have no hd.dat TIM") and describe both paths there.
RDE does this, read from its handler: hook 0x801EF088 (handler 0x436500; installed by 0x435920 only when the HD flag 0x0122F4C0 is set) runs straight after the btlinit unit loader `btl_player_spawn` (0x801EEE4C) uploads the unit's battle texture with the call at 0x801EF080. That call puts the `.ltc` second image at VRAM (0x280 + 64*slot, 0), CLUT row 0xF0 + 2*slot, and the loader then sets the model's texture page and CLUT to exactly that place (the same function's later calls). The handler picks a name from the unit's model byte (record +0x2D; below 0x2C it goes through the character table; table 0x6BEAE0, `selju`..., 0x33 entries), loads `data\hd\menu\player\<name>.tim`, crops its LEFT half (width/2 x full height = 512 x 1024 px, so 4x the 128 x 256 battle page) and uploads it at (0x280 + 64*slot, 0) with the TIM's CLUT at row 0xF0 + 2*slot. In other words, the 4x image replaces the battle model's own texture page, not a menu portrait. Hook 0x801EF1F4 (0x436700) then copies two 128 x 128 px cells from the TIM's RIGHT half (eyes shut), placed by the unit's own (u,v) pair list, over the expression strip at (0x3E0 / 0x3F0, 0x100 + 0x20*slot). Hook 0x801F0040 (0x4369C0) does the same upload for a unit loaded by TIM file name. Hook 0x801EF2A4 moves model byte 0x14 (Leena) up 1 px. Hooks 0x801EEE4C / 0x801EF7C8 free the cache.
OpenCross: battle units take the cdrom2 `.ltc` at 1x (`src/game/model_files.cpp:11`, `battle_model_cache.cpp:92`). `docs/formats/mods.md:149` records the owner's decision that the TIM "is deliberately NOT used in battle because the art looks off". `docs/formats/model.md:353` says "Battle models have no hd.dat TIM", which is false against the handler above.
Why it matters: in HD art every PC in battle shows 1x texels where RDE shows 4x texels and 4x blink/hurt cells. That breaks the "HD = exactly RDE" rule. The "looks off" result could come from an OpenCross pairing bug, for example a wrong CLUT (RDE uses the TIM's own CLUT) or the wrong mesh/UV scale, rather than from the art.
Settle it: run `tools\py\rde_bg.py` on a battle in HD (Serge, Kid, Karsh as a party member, camera close-up) and capture the same frame with `opencross ocbattleview <scene> --cast <id>` (plain ini, OC_QUIET=1, mods off, smallest window) in two builds: today's, and a test build with the TIM left half plus its CLUT as the unit texture. Diff the crops.
Done when: the owner re-decides from that diff. Then either implement the swap (with the right-half expression cells and the Leena -1 px nudge) or record in model.md the measured reason it is not RDE's look. In either case fix model.md:353.

### AKAO FE 0B (music channel triggers a sound effect) is not modelled; music063 plays 90
Priority: P2
Impact: med; confirmed in code.
Capture (PS1 audio against OpenCross, original-music mode): music063: the PS1 plays a -31.3 dBFS layer (1-4 kHz) during the song's first 4.5 s where OpenCross is silent; the main part is also 0.4 dB lower (-2.6 dB below 63 Hz).
Original: FE 0B 0x800558CC plays effect00 id n through command 20 (0x8004F518, chains and eviction included) with mask = a value
toggling 1 / 2 on every call (0x80090A34 ^= 3, reset to 1 at every song load 0x8004E010), pan = channel pan high byte (+0xA2 >> 8),
volume = channel balance high byte (+0x96 >> 8).
OpenCross: skipped (`akao_player.cpp` FE default case); audio.md:135 says "not modelled"; the music `AkaoPlayer` has no SFX sink.
Data: only music063 (song id 59) channel 0: effect00 ids 163 x15, 167 x30, 171 x15, 175 x15, 179 x15.
Done when: the music player can request `(id, volume, pan, mask 1/2)` from the SFX player like command 20, `ocaudio render-seq 63`
contains the hits, compared with a `ps1_audio.py` capture of song 59; audio.md:135 describes the op.

### Original soundtrack: the field song restarts after a battle; the PS1 resumes it mid-song
Priority: P2
Impact: med; confirmed in code.
PS1: A3 (0x800C30A4) and C9 (0x800C3348) with BATTLE flag bit 0 clear call 0x8004A03C (sound command 0x40, 0x8004F088: copies the whole music context
and 32 channels into the pushed context) before the battle song. Commands 0x10 (field PLAY_SONG, 0x8004EF8C) and 0x14 (battle, 0x8004F008) compare
the requested id with the pushed id (halfword 0x800919AE): equal means restore the pushed context (0x8004ECDC). Command 0x1A (0x8004F1F8) also pushes the playing
song when nothing is pushed, so a jingle followed by PLAY_SONG of the old id resumes too. The push is cleared (0x8004A05C) only from 0x800AFA60 (load / new game).
On RDE's original-music mode the PS1 sound driver runs as is, so it is the reference there too.
OpenCross: `request_battle` stores the resume position only when `bgm_remastered_` (`field_world_transition.cpp:171`); `start_song` ignores
`start_seconds` on the AKAO path (`field_world_audio.cpp:61-69`). `audio.md` unknown 4 lists push / pop as not modelled.
Player effect: with BgmSource=original every battle ends with the field song from bar 1; also the jingle-over-song pattern (recruit scenes) restarts the song.
Done when: `AkaoPlayer` can snapshot and restore a music context (channels, loop stacks, tempo, instruments), the field uses it on battle end and on the
command-0x1A case, and a `ps1_audio.py` capture of the field song across a battle matches (10 s before / after).

### Anim op 0xA9 GetBeta not implemented: Beta always flashes blue, Shuffle always shows 0 0 0
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): 4 uses in `tech/beta.prd` and `tech/shuffle.prd` only. No anim script reads the 155slot reel word, and the slot element 358 is in no enemy grid, tech grid, shop or enemy script, so the 155slot result has no consumer in shipped data.
0x800B7104 (op 0xA9, `A9 selector out`) stores control-block word +0x2A (per-unit at 0x800E31B0 + unit*0x30 + 0x2A, the pending
result word). Writers: element mode 47 (0x800D6768, Beta) writes `(1 << colour) << 2` into the caster's word; mode 48 (0x800D6868,
Shuffle) writes the swapped HP value; op 96's gate (entry A4) zeroes it. Readers in data: `battle\effects\tech\beta.prd` main
(colour chain on bits 4..0x80: green, white, black, red, yellow, else blue), `tech\shuffle.prd` main (three reads split into
digits), and the walk also reaches one in `battle\monster\ms_robo\at0.prd` atk (check what it reads). OpenCross: no `case 0xA9`
(`battle_seq.cpp:738` default); `battle.cpp:1438` / `:1453` compute the colour / value but `ActionEvent` / `bseq::Ctx` carry no
result word. Who consumes 155slot's reel word is still open (no anim script reads it). Done when: `Ctx::beta` is filled from the
caster's mode 47/48 result, `case 0xA9` writes it, and Beta / Shuffle are compared with `ps1_cast.py force` captures (owner chars).

### Anim op 0xA1 GetNumberTarget not implemented: multi-target stagger collapses
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): 9 uses in 5 player-tech packs: ransha, aurashot, dance, onedari, shock.
0x800B6EB4 (op 0xA1, `A1 out_count out_index`) writes the number of target-list entries (0x8003096C walk) and the running model's
index in it (-1 plus an error print when absent). Used by `tech\onedari.prd`, `tech\aurashot.prd`, `tech\dance.prd`,
`tech\shock.prd`, `monster\ransha.prd` (main): each target waits index * 0x18 (aurashot 0x34) ticks before reacting; ransha picks
a sub-routine by index. OpenCross: no case (`battle_seq.cpp:738`), so every target of a group cast reacts in the same tick.
Done when: `Ctx` gets `target_count` / `target_index` for the reacting unit, op 0xA1 writes them, and a 3-target Dance /
Aura Shot capture (`ps1_cast.py force`, the owner casting) matches the number timing.

### Customize "Controller: Custom" page and the guest pad remap are not modelled
Priority: P2
Impact: med; confirmed in code.
Decision (owner): implement the page and the remap (RDE keeps it).
PS1 menutest `menu_settings_loop` 0x801EA27C: with option bit 0x100 set, OK on row 4 opens the assignment page (states 2/3,
Left/Right toggles the page, Up/Down rows 0..7 or 0..11 on an analog pad, a face/shoulder press assigns that button via
0x801E8398 / 0x801E8304, Select resets with 0x801E840C to 0x76543210 / 0x3210, Start alone leaves). Main 0x80022288 remaps
the pad word through the four nibble tables (0x8006AEF8..), 0x800223D0 applies it only while 0x8006AF18 == 0 (the loop
sets 0x8006AF18 = Controller row is Default) and 0x8006AF14 == 0 (page open), 0x8002247C swaps stick byte pairs, and
0x800221E8 makes `{key(n)}` glyphs follow the map. RDE keeps the page: it hooks its draw (0x801E97D4 / 0x801E992C /
0x801E9B9C / 0x801E9D24 text-id remaps, 0x801EA95C row limit 0xB -> 9) and does not hook the remap. OpenCross: the row only
toggles the bit (`src/ui/menu/main_menu_settings.cpp:65-71`), `slot.button_cfg` is loaded/stored but never read
(`src/save/cc_save.cpp:240,276`), `docs/rules/menus.md` section 2 says "not modelled". Player effect: Custom does nothing;
RDE saves with a custom layout behave differently. Reproduce with `rde_bg.py`: open Customize > Controller Custom, grab
the page (row list, glyphs) for the table. Done when: the page (RDE's 9/12-row variant) edits button_cfg and field /
battle input + glyph prompts go through the remap, or the owner records it as dropped; menus.md updated.

### Hi statuses 0x20000 (no stamina cost) and 0x40000 (drain) not modelled
Priority: P2
Impact: med; confirmed in code.
Data use (scan of the shipped data): 0x20000 comes from one record, Vigora (54): repro an enemy cast in scenes 445 / 467 / 473, or a PC casting a Vigora dropped by Vita Unus. No table record grants 0x40000 (battle-script writes were not scanned), so the drain half is likely unreachable.
Original: 0x800D7D20 multiplies the actor's stamina cost (weapon, element 70, every combo caster) by 0 when its merged hi word has 0x20000. 0x800DA1D8 and 0x800DA424 give the attacker a code-3 result equal to the damage dealt when its hi word has 0x40000; 0x800D85E4 then heals it (with the enemy-element halving applied).
OpenCross: nothing reads hi 0x20000 / 0x40000 (rules.h only names lo 0x20000 / 0x40000); `kHiApplyMask` lets elements set them.
Done when: both implemented and named in docs/rules/battle.md; a pgdat / gear sweep (`cc_tables.py`) lists which records grant them, which sets the impact.

### RDE per-script data corrections in the field are not applied
Priority: P2
Impact: med; needs a capture or data sweep.
Decision (owner): implement RDE's corrections as a data-driven table applied at import (`src/import`, keyed by map / script offset, each row documented as RDE behaviour); no per-room code (rule 3).
RDE patches retail field scripts at run time (field.prg hooks, guards in `research\docs\reimpl\gameplay\bugfixes.md`): map 0x1DE op
0x800BF31C (entity step 4: u16 operand at +3 minus 0x1E; step 5: plus 0x1E); maps 0x10F / 0x1F4 op C2 `fld_set_bg_group_tint`
0x800BFBB0 (group 1, colour 0x28,0x28,0x14 replaced by 0xFF,0x19,0x08 in some channel order); map 0x57 op at 0x800BFF14 skipped while
var 0x15A bit 6 is set; map 0x1A0 step 0xB op 0x800C5C74 tests var/bit operand 0x18E2 instead of 0x18E4; map 0x183 op 0x800D0040
skipped when its byte is 3; map 0xE6 op 0x800D3460 clears bit 0 of the stored status unless var 0x1A6 bit 1; room 0x5E op 0x800BD0F0
adds 5 (entities 5, 6) / 3 (entity 7) to a non-zero +0xB8 value; 0x800B7588 forces "no shadow" for entities 8 / 9 while
`u32 0x800852E0 == 0x101` (not a room id; meaning unknown). OpenCross runs the retail bytes (`src/script/*`, no patch table). These are
data fixes RDE ships, so the general form is a table of script corrections (room + entity + script offset + expected bytes -> new bytes
or skip) applied at script load, documented in `docs/formats/fieldscript.md`; not engine special cases.
Repro: per room, compare RDE (`rde_bg.py` / `rde_save_redirect.py` with a save near the room) with OpenCross (route or `[Debug] Room`).
Done when: each correction is in the table or shown unnecessary, the visible effect of each is recorded (bg tint 0x10F / 0x1F4,
flag test 0x1A0), and the 0x800B7588 shadow condition is decoded.

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

### `ocbattleseq -g <dir>` can start the full game and read the real save folder
Priority: P2
Impact: med for tooling safety; seen during a capture.
A mis-parsed `-g` argument made `opencross ocbattleseq` start the whole game to its title screen, which loads the default save folder (`Documents\My Games\...`) for up to its timeout. Nothing was written, but a dev tool must never fall through to the game. Done when: an unknown or malformed tool argument exits with usage and a non-zero code, and no tool path opens the game window.

### `Room=` / `load` debug start of a disc-2 room loads the disc-1 map record
Priority: P3
Impact: low (dev start only); seen during a capture.
Starting OpenCross at crn_b16i (a disc-2 room) through `Room=` / `load` reads the disc-1 map record. Done when: the debug start picks the disc of the room and a route test starts in a disc-2 room.

### `GameTables::load` reads the 408-byte tail of `char_w1..5.bin` as Lv99 targets
Priority: P3
Impact: low (dev tools only); confirmed in code and data.
`src/data/tables.cpp:127-132` takes the bytes after the 51 records of `char_w1..5.bin` as that set's `lv99para`. They are not: they differ from `lv99para.bin` (which starts `52 03 58 4E ...`, the tail `00 40 00 40 00 40 23 C1 ...`) and decode as nonsense rows (HP up to 49312, stats up to 254). Battles use `char.bin` with `lv99para.bin`, so only `ocbattle sim --set char_w1..5` and anything else that picks those sets sees the garbage. `docs/formats/battle_data.md` now says the tail is not `lv99para`; what it holds is unknown. Done when: the loader uses `lv99para.bin` for every set (or decodes the tail once its meaning is known) and a unit test checks a `char_w1` Lv99 row equals `lv99para.bin`.

