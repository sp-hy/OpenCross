# The title scene: the underwater 3D backdrop (`title.prd` + `title.ltd`)

The title screen is a 2D room (logo, fade cover, glow) drawn over a 3D underwater backdrop. The backdrop is not part of the room: it is a set of effect sequences in the global `cdrom/title.prd` with textures in `cdrom/title.ltd`. This document covers where the data lives, how the backdrop is built and drawn, the scenery sets and kill sequences that make each `titN` sequence look different, HD/SD behaviour, and the title menu gating. Engine code: `src/game/title_backdrop.*`, `src/ui/title/title_screen.cpp`. The `titN` sequence can be forced with the environment variable `OC_TITLE_SEQ=titN` (title route).

## 1. What draws it

The title room (`opening.bin`, rooms 1 / 8) has only three 2D layers: L0 = the PS1 logo + (c)1999 SQUARE, L1 = a fade-in cover (opacity 248 -> 0), L2 = the pulsing glow (the logo halo; opacity 0..128). **The sea is not in the room.** The room script runs `D3 PLAY_EFFECT "tit0".."tit3"` (random `RAND & 3`) after the opening FMV and every time the loop restarts. With `[Debug] Seed` = 0 the field RNG is clock-seeded, so the pick varies; a fixed seed always picks the same sequence.

### 1.1 Data files
* `title.prd` is a stage-like drp (`battle_stage.md`) with 122 entries (record 23 on disc 1, 393 on disc 2), loaded by the engine at boot. It holds:
  * 36 type-0x02 scenery meshes (`kaso`, `joso`, `jos1/2`, `roof`, `far*`, `fish`, `kame`, `hugu`, `ei` ...);
  * 59 type-0x0A emitters that instance them (sprite type 14);
  * 11 type-0x10 sequences (`tit0..tit3`, children `indi`, `indj`, `kill`, `kil2`, `kil3`, `kilj`, and `mo1c`, seven controller-rumble ramps (op 0x29) that the field's playback of movie 2 starts, `room_effects.md` 5.2);
  * 14 type-0x0C camera shots (`k001`, `k002`, `j001..j003`, `j101/2`, `j201..3`, `j301..3`, `1`). A `titN` sequence selects camera shots (op 0x23, with delays), starts emitters (0x22) and starts child sequences (0x2F); its screen-colour (0x2B) and tint-bracket (0x3F) commands are not presented in the title.
* `title.ltd` is a cpt of VRAM uploads `{u32 0x1101 CLUT | 0x1100 image, u16 x, u16 y, u16 dx, u16 dy, u16 w (halfwords), u16 h, data}`; the VRAM position is (x + dx, y + dy). Five blocks, all uploaded (they are not animation frames):
  * CLUT rows at (256, 228), 256 x 12;
  * images at (192, 256) 512 x 256 halfwords (1024 x 256 texels at 8 bpp, the main page strip), (704, 256) 68 x 96, (704, 352) 56 x 32 and (704, 384) 32 x 24 (the fish / manta / tree pages at tpage 704; their `x,y` is (192, 256) with `dx` = 512 and `dy` = 0 / 96 / 128). The disc has no disc-2 variant of these files.

## 2. Building and drawing the backdrop (`TitleBackdrop`)

* OpenCross: `import::load_battle_effects(title.prd, no ctd, particle lib)` loads emitters and sequences; `import::load_stage(title.prd, ctd)` loads meshes and atlases, with `title.ltd` re-wrapped as the sector `.ctd` (every block becomes type 0x1201 / 0x1200 at its (x + dx, y + dy); `TitleBackdrop::ltd_to_ctd`). Cameras: `CamBank` / `BattleCamera` driven by the sequence's op 0x23.
* One `EmitterSystem` + `SequencePlayer` per run; `FieldWorld::set_effect_tap` hands the room's `PLAY_EFFECT` to the backdrop. It ticks at the field's 60 Hz and is drawn **first**, before the room layers (the window background image is skipped), so the logo layers blend over it.
* **Projection centre**: the title uses geometry offset (160, 112), not the field's (160, 120); the backdrop's view parameters use oy = 112. **Fog**: the title sets near/far 0x3000 / 0x6000 with a black far colour, but the depth-cue flag stays 0 in the real game (only sequence op 0x24 sets it), so no fog is applied.
* **Mesh objects are drawn like a battle stage** (`render::StagePipeline`, the path of `StageScene::draw`): type-14 objects use the same model renderer as battlefield meshes (`room_effects.md`), so one `StageInstance` per live object carries the object's matrix, colour, blend (op 06), ordering-table bias (slot = camera z / 16 + bias, or the bias alone with the fixed-slot flag) and cull side. Opaque faces draw first with depth, blended ones back to front. The GPU clips at the near plane instead of dropping a polygon with a vertex behind the camera. Texture atlases are sampled with the `[Video] Filter` setting (bilinear by default, nearest when set so; the same in both art modes), with transparent texels bled so the filter leaves no dark fringe. With `[Video] Ps1Res` the sea is drawn into a native 320 (+ the widescreen columns) x 224 target and scaled up (`App::draw_title_lowres`), and `Ps1Jitter` snaps its vertices to that grid. The op-36 window sampling is clamped half a texel inside the window so HD bilinear does not bleed the neighbouring atlas page along face edges. Sprite objects (type 3: bubbles, plankton) go through the particle batch afterwards, depth tested against the meshes.
* **Generic effect rules the title relies on** (`room_effects.md`): a LIFE 0 object stops its header (otherwise the sea surface `roo1/roo2`, seabed and giant `kma1/2` shafts would re-spawn every 11 ticks and reset the op-36 UV scroll); op 34 `VEL_ADD_AWAY` adds k / 1024 units per tick (the `suji/suj1` surface streaks are an `effe` shaft every 2 / 5 ticks with k = 1014).
* **Light shafts**: `kma1/2` are persistent giant `eff6` fans whose colour oscillates (op 21, period 4096/48 = 85 ticks: the slow fade in/out); `ksu1..3` are `effe` beams every 30-54 ticks fading over 130 ticks; `suji` streaks run under the surface. They are additive `effe` / `eff6` quads with ordering bias -70..-100.

### 2.1 Model objects (sprite type 20)
The drp's type-0x0B records `ed9a` (manta ray: 14 joints, 114 faces, a rest clip and a 10-keyframe swim) and `ed9b` (turtle: 9 joints, 178 faces, rest + 10 keyframes) are model containers (`model.md`; no S5/S6). The emitters `j-ei`/`j-e1` (manta, colour (103,116,171), scale 2.0 / 1.0, bias 200) and `kame`/`kam1`/`kam2` (turtle, scale 2.75 / 2.25) spawn them with `SPRITE type=20`. Each tick the model takes the object's draw colour, position (pos >> 4 = world units in 20.12), angles and scale, and with ext bit 21 (op 35 word 0x200000 on all five) advances its motion by `ticks * speed / 256` keyframes, then poses and draws it. The speed is set by op 49, which the title does not use; OpenCross assumes 64 (0.25 keyframe per tick, the engine's 15 fps).

The records carry no texture words: **the record name is the binding**. Chars 0-1 = CLUT row in hex (`ed` = 237); char 2 = a nibble (bit 0 -> tpage y 256, bit 3 -> CLUT x 256); char 3 = tpage x / 64 (`b` = 704 turtle, `a` = 640 manta). Both title models use CLUT row 237. They are drawn through the skinned mesh pipeline (`render::MeshPipeline`, same camera and depth units as the scenery), model matrix = `T(x, -y, z) * diag(1,-1,1) * (R * S)`.

### 2.2 Ext-object blend rules
* **Ext objects (op 35) take the plain blend word.** Blend n > 0 is semi-transparency with ABR n - 1 (2 = B + F, 4 = B + F/4), blend 0 is opaque; nothing else in the extended path scales the colour (packet corner x object colour / 128). This holds for the morphing fish too: the big teal fish are `cfis` (mesh `fis4`, blend 0, object colour (88,134,225)/128: opaque); the small pale yellow fish are `afi1` (`fis1`, blend 2: olive texels added to the water).
* **Scrolling blend-2 ext objects** (op 36): the caustic overlay `utur` (mesh `jos2`) and the sea surface `roo1`/`roo2` carry op 35 and blend 2 and add at full strength. `OC_STAGE_EXT_QUARTER=1` selects a quarter-strength add (B + F/4) for every non-morphing ext object with blend 2 instead. Unknown: whether the original adds a quarter there.
* **Op 37 mesh morph**: the fish (`afi1`/`afis` fis1 -> fish, `bfis` fis2 -> fis3, `cfis` fis4 -> fis5), the jellyfish (`hugu` -> hug1), the eel (`utub` -> utu1) and `kura` -> kur1 swim by morphing: phase += rate per tick (s16), reflected at bit 12 into a triangle wave of period 0x2000, eased by the op's mode, and the object is drawn with vertices = `lerp(A, B, blend)`. Every pair has identical packets (indices, uvs, colours); only positions differ. `render::StagePipeline` rebuilds the blended vertices per frame through its dynamic ring buffer.
* The PS1 draws type-14 objects with their vertex colours only (no lighting).

### 2.3 Scenery sets and the kill sequences
The sea is two sets of emitters:
* `indi` starts the **cave set**: `kaso`, the `kma1/2` / `ksu1..3` shafts, `ne-a` / `ne-s`, `kfar`, `kfi1/2`, `kawa`, `tuna`, `snak`, `eyes`, `smok`.
* `indj` starts the **reef set**: `jos1/2`, `utur` caustic, `utub`, `suji`, `sun`, `roo1/2`, `far1/2`, `hous`, `awa`, `tre0..9`/`tre:`.

Headers without flag bit 2 are linked at registration, so both sets run from about tick 10 unless a sequence stops them. The op-0x55 sequences (name at +8; the u32 at +4, 0 or 1, is never read) stop a set and expire its objects (life 0 = freed, others life := 1):
* `kill` = the cave set + the mantas `ei`/`ei1`; `kilj` = the reef set; `kil2` = 14 reef headers + `tuna`; `kil3` = `jos2` + `utub`.

Timelines (waits apply after the command):
* `tit0` kills the cave set at tick 1000 (its j-shots).
* `tit1` runs `kill` at tick 0 (cave set back at 1100 with `indi`) and `kilj` at 1290.
* `tit2` runs `kill` at 0 and `kil2` at tick 1; the 14 reef headers (`utur`, `jos2`, `hous`, `utub`, most trees) return with `indj` at 700, the cave set at 1530.
* `tit3` runs `kill` at 0 (cave set back at 1185) and `kil3` at 601.

So in `tit1..3` the cave and the caustic/rocks of `kil2` are absent for the first 11-25 s by design, even where the camera would show them. The title uses the same effect dispatcher and resource list as battle; there is no title-specific rule.

OpenCross: **the title keeps every set** (deliberate departure): `TitleBackdrop::build_run` sets `EmitterSystem::Config::keep_on_stop_expire`, so op 0x55 does nothing in the title run and the whole sea (cave + reef) shows from the start in every sequence, including with free camera, widescreen and high FOV. Battle and field effects keep the retail op.

## 3. HD / SD modes, prompt, widescreen
* **HD mode**: room groups 0..2 are hidden (`RoomView::set_hidden_groups(7)`) and `hd/title/title.png` is composited as an RGBA image (transparent outside the lettering). "Press any button" is drawn by `TitleScreen`.
* **Original (SD) mode**: nothing is hidden; `title.png` and the RDE prompt are not drawn. The disc's own room layers (PS1 logo, (c), prompt) show over the sea. The menu phase is the same bg0 / bg1 picture in both modes.
* **Prompt fade** (alpha for both the Normal and Outline/Shadow text, from the 70-frame counter `cnt`): `cnt` 0..20 = `cnt / 20`; 21..60 = `(60 - cnt) / 40`; 61..69 not drawn.
* **Widescreen**: the 3D sea is drawn across the widened frame only in the splash / prompt phase; in the New Game / Continue menu phase the sides are black (the menu is the static 4:3 picture). Cancel in the menu goes back to the prompt through a black fade (an OpenCross addition: the original's menu has no way back) and starts the room's song again; the scene keeps running.
* **Art mode switch (F6) on the title**: a pure rendering swap. `FieldWorld::set_art_mode` swaps the title room's art in place (`RoomView::reload_art`, actor models via `Actor::swap_model_art`) without reloading the room, so the script (logo FMV, `tit0..3` pick, timers), flow state, title phase and the running sequence are untouched. The title sea and models have no disc-2 variant and look the same in both modes (the sampling filter is the `[Video] Filter` setting, `TitleBackdrop::draw(.., linear)`). Route: `art_title`.

## 4. Boot order, menu gating and attract demo
* The opening FMV plays first on a cold boot (`D4 #0` = a01op, `movies.md`); a button during it only skips the movie. Before it, once per process, RDE's boot logo `ui/logo.png` is shown (SQUARE ENIX, 180 frames, 30-frame fades, skippable; both art modes; the Japanese build shows `notice.png`, `notice2.png` and `logo.png` in turn).
* The title menu is not accepted until the logo FMV has ended or been skipped (`GameFlow::set_title_logo_ready`); before that any button only skips the FMV.
* The press on the prompt plays the chime (global sounds 59, 61, 62 together) and fades the room's song out (14 frames after the press, over 20 frames), then stops it (`menus.md` section 13).
* Menu: New Game, Continue (disabled without a save), Settings, Quit; fades take 0.5 s. New Game opens a vibration page (On / Off) that starts the game; Cancel there returns to the list. After a cleared game the New Game + / Continue + variant opens directly (no prompt, no way back to it). The prompt text exists in the six languages.
* The attract demo (the `dem_*` rooms the title room's script enters when its idle timer runs out; the sea of `tit0..tit3` is only the backdrop of the title room; a battle request from a demo room is dropped and reported as won) is cancelled by any button press, which reloads the title room and returns to its "Press any button" prompt (`TitleScreen::restart_prompt`; the idle timer is held while the menu is open and while the debug free camera is on).

## 5. Open items
* Unknown: brightness of `awa` / `awa2` (the cyan bubble blobs) against retail.
* Unknown: the manta / turtle motion rate (64 is assumed, section 2.1); the default must come from the spawner.
* Unknown: the exact pose of `tit0`'s first camera shots `k001` / `k002` (blue `kaso` rock faces where retail shows teal rocks).
* Camera shots are played as the battle camera does (spline approximated); per-sequence timing and any AKAO sounds the sequences start were not compared.
