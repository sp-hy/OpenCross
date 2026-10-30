# Model, skeleton, texture and animation formats

This document covers the skinned character and monster models (battle players, battle monsters, field/overworld models), their skeletons and animation packs, the animation-sequence scripts that drive field clips, the rigid weapon meshes, the Atim/TIM textures, and the HD texture overrides. It concerns `battle/player`, `battle/monster`, `fieldobj/player`, `fieldobj/ex`, `battle/wepon` and the `hd.dat` / `cdrom2.dat` overrides. All integers are little-endian. Counts quoted below were measured over every model-bearing file of the disc; the reference parser is `tools/py/cc_model.py` (`python -I tools/py/cc_model.py validate`).

## 1. What each file type holds

| Path (inside cdrom.dat) | Count | Content |
|---|---|---|
| `battle/player/<n>/<n>.obj` | 52 | **Mdl** (skinned battle model, sections 2-6). Table order = character id; char 18 Macha is `obachan` (her field model is `mamacha`) |
| `battle/monster/<n>/<n>.obj` | 148 | Mdl (enemies and bosses) |
| `fieldobj/player/<n>/<n>.obj` | 56 | Mdl (field/overworld model; same layout, 2-30 joints, larger scale) |
| `*.ltd` (battle player/monster) | 158 | **Atim**: 1 CLUT object + 1..2 8bpp image objects |
| `*.ltc` (battle player) | 52 | cpt of 2 Atims: main texture + a 128x32 overlay strip (the closed-eye frames, section 8) |
| `fieldobj/player/<n>/<n>.tim` | 56 | standard PS1 TIM, 8bpp + CLUT (flag 9), 16928 bytes |
| `battle/*/at0.prd` | 194 | **DRP** (nested resource pack, below). Type 0x19 entry `at0` = the external attack-animation pack of the model |
| `fieldobj/ex/*.bin` | 116 | cpt of 2: [animation pack (event/extra clips), animation-sequence table for those clips (6.1)]; `dummy.bin` is a text stub, not a cpt |
| `battle/wepon/*.bin` | 29 | cpt: (TIM 4bpp, Mesh) x N pairs + 2 four-byte `77 77 77 20` pad entries |
| `battle/wepon/**.kmd` | 17 | single **Mesh** (section 7) |

There are no `.ome` model files in the archive except the status-screen models under `menu/player` (section 12). `.ltc` exists only for players; `.ltd` exists for 10 players and all monsters (a player `.ltd` equals the first Atim of its `.ltc`). One monster (`battle/monster/p_gorem/P_gorem.obj`) has no texture sibling. Folder names are Japanese romanisation (`selju` = Serge; `carshu` is a player folder).

**cpt** container: `u32 N; N x u32 offset` (relative to the cpt start) `+ u32 EOF`. The EOF pointer is present in every cpt, so an entry's end is the next pointer.

**DRP** container: `"drp\0", u32 0, u16 size_field` (`count = size_field/64`), then `count x u32` absolute offsets. The entry at offset p is `u32 0, char name[4], u8 type, u24 v` followed by the data; the data length is `v / 16` (v is 16x the length).

## 2. Mdl file layout

```
0x00 u32 6                      section count
0x04 u32 S1,S2,S3,S4,S5,S6     absolute offsets; S6 = 0 when there is no DRP
0x1C u32 EOF                    == file size
S1 geometry | S2 skeleton | S3 attach/shading (0x160 bytes) | S4 anim pack (cpt) | S5 animation-sequence table (6.1; 0 in room models) | S6 optional DRP
```
Sections are contiguous and monotonic (S1 starts at 0x20). S6 is present in 44 monsters and 5 field models (entry types seen 0x0A/0x0C/0x10/0x12/0x16); all players and 104 of 148 monsters have none. While the model lives, its DRP is a resident registry: its type-0x0A headers answer the animation-sequence "name walk" op 0x1D (battle_effects.md), but only 5 models carry type-0x0A headers there (torimeca, ki, q_con2, bom, sfin). The other models' S6 hold `mag<k>` / `big<k>` shots and sequences, which are also resident, so a script cutscene without an actor of that model still finds them by name.

## 3. S1 geometry

```
S1+0   u32 nConstructs
       u32 constructOffset[n]   relative to S1
       u32 a, u32 b             s16 extent x, y, z (+ s16 pad): the model's authored extents
construct (each starts at its offset):
 +0  u32 UM   offset (rel. S1) of the UV-map (face) block of this construct
 +4  u32 VP   offset of the vertex pool
 +8  u32 VD   offset of the VDDM (vertex data / normals) block
 +12 u32 #A   number of 8-byte-mode assignments; u32 VO8 (always 0)
     #A x { u16 NV, u16 joint }                     next NV vertices of the 8-byte pool belong to `joint`
     u32 #B   number of 16-byte-mode assignments; u32 VO16 (== 8 * n8 : byte offset of the 16-byte area in VP)
     #B x { u16 NV, u16 0, u16 J1, u16 W1, u16 J2, u16 W2 }
     u32 #E; #E x { u16 type, u16 count, u32 offset (rel. UM) }   footer: UV-map entry table
```
A construct's footer ends exactly where the next construct begins. Constructs are separate meshes (body, hair, props; 1-13 per model). The UM, VP and VD arrays of all constructs are stored one array type after another (all UM, then all VP, then all VD).

The extents set the effect scales of a battle unit: `scale_y = clamp((ey << 12) / 0x42B - 0x1000) / 2 + 0x1000` and `scale_xz = the same of SquareRoot0(ex^2 + ez^2) / 0x23C` (MIPS division, result clamped 0x300..0x8000; above 0x10000 it becomes 0x8000). Examples: Serge (515, 1055, 257) -> 4073 / 4096; Sand Squirt (390, 850, 1292) -> 3679 / 6874; Komodo Pup (478, 969, 987) -> 3908 / 5961. `SquareRoot0` is the PS1 library table root (`effects::ps1_square_root0`), not an exact sqrt (Sand Squirt 1348 -> 6874). The values come from the HD (cdrom2) file when it is loaded: `battle/player/selju2/selju.obj` has (515, 1055, 257) in cdrom2 and (517, 1067, 249) in cdrom.

### 3.1 Vertex pool (VP)
* **8-byte area**, `n8 = sum(NV of #A)` records: `s16 x, s16 y, s16 z, u16 index`. The first field is x (it runs along the bone axis; +y is down). Units are model units (Serge is ~1070 high).
* **16-byte area**, `n16` records = two 8-byte records `(rec0, rec1)` with identical `index`. rec0 is relative to J2 (the child joint), rec1 relative to J1 (the parent): `rec1 = t + R*rec0`. Weights W1 (for J1) and W2 (for J2) are 12-bit and sum to 4095 or 4096. Final position = `w1*M(J1)*rec1 + w2*M(J2)*rec0` (weights / 4096). J2 is always a child of J1. The W1<->J1 pairing was settled by distortion tests on animated poses (mean edge distortion 4.5 vs 8.4 units).
* `index` is a **global** vertex number: construct k's records carry `first_index .. first_index+nverts-1` (sequential in all files seen; treat the field as authoritative). 8-byte-area vertices come first (index order), then 16-byte ones.
* At most 2 influences per vertex; joint ids are skeleton joint indices (< S2 count).

### 3.2 VDDM (VD)
One 4-byte record per position record (`n8 + 2*n16`): `s8 nx, ny, nz, u8 pad`, a normal scaled to +-127. Used for lighting and environment mapping (3.6, 5.1). For an 8-byte vertex the record and joint are its own; for a 16-byte vertex it is the first record of the pair (rec0, joint J2). OpenCross keeps them as `SkinVertex::normal` / `normal_joint`.

### 3.3 UV map / faces (UM)
Entry table types (`type` = GPU primitive code without the colour byte):

| type | primitive | record size | layout |
|---|---|---|---|
| 0x24 | textured tri (FT3) | 12 | `u,v x3` (6 B), 3 x s16 pointer |
| 0x2C | textured quad (FT4) | 16 | `u,v x4` (8 B), 4 x s16 pointer |
| 0x20 | flat tri (F3) | 12 | `r,g,b,code`, 3 x s16 pointer, 2 B pad |
| 0x28 | flat quad (F4) | 12 | `r,g,b,code`, 4 x s16 pointer |

Texture coordinates are 8-bit pixel coordinates in one 8bpp page (u < 128, v < 256); flat colours are 0xC8,0xC8,0xC8, 0xFF,0xFF,0xFF or a small set. Entries are laid out in table order (0x20, 0x24, 0x28, 0x2C) and an entry's `offset` is the start of its records. Quad vertex order is the GPU Z-order (v0,v1,v2,v3 with v3 diagonal to v0; triangles v0,v1,v2). Counts over the disc: 0x24: 66k, 0x2C: 94k, 0x20: 56, 0x28: 36.

**Pointer to vertex rule** (decodes all 252 models without an out-of-range or dangling reference):
1. Each vertex slot holds an s16 `p`. For textured faces the first two vertices (in UV-record order) use unit 16, the rest unit 8; for flat faces all slots use unit 8. `q = floor(p / unit)`; the flag bits are `p mod unit`.
2. **Textured triangles store their pointers rotated**: pointer slot 0 belongs to UV3, slot 1 to UV1, slot 2 to UV2 (`ordered = [p1, p2, p0]`). Flat triangles are not rotated.
3. `p >= 0`: `q` is the absolute global vertex index. `p < 0`: `q` is a back-reference to the vertex of the slot `-q` positions earlier in a running slot list built over the construct's faces in UM order, where **every face occupies 4 slots** (a triangle's 4th slot is a dummy). Floor division matters (`-47 = -48 + 1`: flag bit set).
4. **CLUT bit and page bit.** The two bits are independent. Let ptr_a / ptr_b be the first and second pointer in UV-record order (a triangle's rotated slots 1 and 2, a quad's slots 0 and 1). **Bit 0 of ptr_a = CLUT row + 1** (the next VRAM row). **Bit 0 of ptr_b = texture page + 1** (128 texels to the right at 8bpp). The page bit is never set on the third/fourth pointer (unit 8, back-references are multiples of 8).
   * Battle monsters: every 256-px Atim has 2 CLUT rows and its right-half faces carry both bits (17,936 faces: the right half is drawn with the second palette row). `manet` / `mizu_c` (128-px image, 2 rows) carry the CLUT bit only (355 faces). No monster face has the page bit alone. Players and field characters never set either bit.
   * Room models (room.md): 57 of 2,203 carry the bits (24,303 faces: 22,203 both, 96 CLUT only, 16 page only), for example the Hydra Marshes hydra (`hid_a10f` / `dem_06` model 1: image (768,256) CLUT 225, its other half is the `.ctd` TIM at (832,256) with CLUT 226).
   * OpenCross: the importer maps the CLUT bit to `textures[1]` (warning when the texture has no second CLUT row) and the page bit to `u += 128` (warning when the texture is narrower than 256). A room model gets the 256-texel VRAM window of its page and the next one, coloured with CLUT rows `clut_y` and `clut_y + 1` (`RoomModelHeader::page_textures`, `import::vram_page_textures`).

A decoded face is `(kind, [global vertex index...], [uv...], page, colour)`.

### 3.4 Face culling (one-sided faces)
Every model face is drawn only when it faces the camera; no shading flag makes a construct double-sided. The test is the GTE `NCLIP` cross product (`MAC0 = SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1`, screen y down) on the corners in UV-record order (for a textured triangle: pointer slots 1, 2, 0).

| builder | triangle drawn when | quad drawn when |
|---|---|---|
| field and battle lit/env builders | `NCLIP > 0` and no GTE error flag | `NCLIP(v0,v1,v2) > 0`, or (after projecting v3) `NCLIP(v1,v2,v3) <= 0` |
| main-library builders (menus, scene player) | `NCLIP > 0` and no GTE error flag | `NCLIP(v0,v1,v2) > 0` or `NCLIP(v1,v2,v3) < 0` |

A quad is kept or dropped as a whole: it is drawn when either half faces the camera. The two quad rules differ only at `NCLIP = 0`: the 12 degenerate quads of the data (`battle/monster/maturi`, v1 = v3) are always drawn by the battle rule. The GTE error-flag part is not reproduced.

OpenCross: `MeshPipeline` (field, room, battle, menu, title models, weapons) runs the test in the vertex shader on the projected corners (`assets::Mesh::tri_quad` marks the two halves of each source quad; the importer keeps the source winding). Rigid weapon meshes are culled the same way (section 7). Debug: `OC_MESH_CULL=0` (off) / `-1` (inverted).

**Flat faces** (UM types 0x20 / 0x28, 92 in the whole data) are never drawn by the game: every construct loop dispatches types 0x24 and 0x2C only and steps over other entries. OpenCross decodes them (their pointer slots feed the back-references of later faces, rule 3 above) and drops them from the mesh (`build_mesh`).

**Packet budget (battle).** The battle mesh draw checks, before each run of faces, that the frame's packet buffer still has room for the run (32 bytes per triangle, 40 per quad on the plain path) and skips the run otherwise. The battle buffer is 0x2C100 bytes per half for most scenes; RDE enlarges it by 0x8000 (0xC000 on maps 0x1AB / 0x1DD) and keeps the per-run check in its native draw. In practice it is never reached: in FATE's `boss` intro (scene 462, bg_40) at least 0x2BC00 bytes are free at every mesh draw of the whole intro. What hides FATE's pieces there (the leg for most of shot `i04`, other pieces later) is the object screen test of flag bit 28 (`battle_stage.md` 8.4), which RDE does not replace. OpenCross has no budget check; it runs the screen test.

### 3.5 Face ordering: one ordering-table slot per face
There is no depth buffer: every polygon is filed in one ordering-table slot and the GPU paints them far to near. Models in battle and field use the same rule: `slot = average camera z >> 4` (three corners for a triangle, all four for a quad). A construct whose shading flag has **0x10** (section 5) uses `slot = nearest corner's z >> 4` instead. A quad is one primitive: both of its triangles take the slot of all four corners.

The 0x10 parts are hair, capes and scarves laid over the body, and they interpenetrate it (Guile's battle model `battle/player/guill`, constructs 2-4, has 78-119 triangle pairs crossing a body face in every frame of his stand/idle clips). The nearest-corner slot puts such a face in front of the body face it crosses, so the PS1 never shows the shoulder or chest through the hair; a per-pixel z-buffer does, and which side wins flips with the view angle (visible as flicker as the camera rotates).

OpenCross: every battle model draw (units, their weapons, the pseudo-actor slot, effect model objects) sets `MeshPipeline::DrawParams::ot_faces` with `near_meshes` = the shading entries with 0x10 (`BattleView::unit_ot_faces`; weapons none). The vertex shader computes the face's slot and the fragment depth is that slot plus a sub-slot fraction (`mesh_pipeline.h`), the same depth space as the stage packets (battle_stage.md section 8).

### 3.6 Lit vertex colour: the GTE lighting path
A model is drawn flat (one colour per construct, section 5) unless its **model flag 0x100** is set, or the env-map flags 0x20 / 0x40 are (the test is `flags & 0x160`). The lit path emits Gouraud textured polygons (GT3 / GT4, codes 0x34 / 0x3C) with one colour per corner. Flag 0x100 is never set by the model data. It is set and cleared at run time:
* sequence ops 0x61 / 0x63 / 0x68 set it on targets / casters / everyone; 0x62 / 0x64 / 0x69 clear it;
* the battle animation-sequence op 0x5E sets it on every actor and target while its light ramp runs and clears it at the end;
* emitter op 49's byte at +1 sets or clears it on a type-20 model object;
* the field draw sets it on an actor whose actor flag (bit 10) says so.

Env-map parts (S3 flag 0x20 / 0x40: 17 models, e.g. bannin, lavos, fate, zoah) take the lit skinner too but keep the flat colour (the colour test uses the 0x100 flag).

**Normals.** The skinner rotates each vertex's VDDM record as `n << 5` (4.12: 127 -> 0x0FE0 ~ 0.99) by the joint's world matrix (rotation only; an 8-byte vertex by its own joint, a 16-byte vertex's rec0 by J2), clamps every component to +-0xFFF and stores `>> 5` as s8. (The other skinner, used by the effect overlays' env UVs, clamps to +-0x1FF and stores `>> 2`.) The result is a world-space vector of length <= ~0.99 (never normalised; a scaled joint matrix scales it).

**Lights.** Before each unit's draw, for each of the three light slots (world position, RGB bytes, s32 intensity, s32 falloff), with pos = the unit's **attach point 9** (waist):
```
d = light - pos                         (s16 arithmetic)
dist = SquareRoot0(|d|^2);  n = normalise(d)   (4.12)
k = intensity - (dist * falloff >> 12)  clamped 0..0x7FFF      (falloff << 3 first in the field)
light matrix row i = n * k >> 12;       colour matrix column i = RGB << 4
```
The slots are written by sprite type 17 objects (battle_effects.md) and zeroed (intensity) by sequence op 0x66. The **back colour** BK (bytes << 4) is set by op 0x65's ramp, by a pack's type-0x24 entry, and by lit effect-mesh draws. Nothing in battle init or a stage file sets a light or BK; at boot both are zero.

**Field lit actors** use the same path, with the lit builder using `NCCT` and no depth cue (the flat branch of the same builder uses `DPCS` with the fog). The back colour is not 0 in the field: it is zeroed at every room init, then set by the room pack's type-0x24 entry (first three bytes) and by sequence op 0x65 ramps. Example, retail `opn_a01i` (lift): BK = (159, 160, 115) << 4 constant, light slot 1 at (26, -656, 1758), rgb (129, 129, 127), intensity 10656, falloff 2048 (`<< 3` = 16384, so `k = 10656 - 4 * dist`): the party is lit by the lamp near the lift and falls back to the ambient as it runs down the hallway. Party models have flag 0x0100 and the actor lit bit set.

**Colour**, per corner (`NCCT`):
```
I_i    = clamp(LLM_i . N, 0, 0x7FFF)                    (4.12; LLM rows above, N = n << 5)
c      = clamp(BK + LCM . I, 0, 0x7FFF)                 (BK = byte << 4, LCM = byte << 4)
colour = clamp(RGBC * c >> 12, 0, 255)                  RGBC = (unit colour * construct colour >> 7) * model colour >> 7 (0x80 default)
```
The GPU then modulates the texel by colour / 128 (Gouraud between corners). In units of 1.0: `c = BK_byte / 256 + sum_i rgb_i / 256 * max(0, dot(dir_i * k_i / 4096, n / 128))`. For example BK 0x80 plus one light of colour 0x80 with k = 0x1000 facing the normal gives 0.5 + 0.5 = 1.0 (the flat brightness); BK 0x80 alone is half brightness. That is the darkened look of 9boll / dark while the ambient ramps to (0x50, 0x3C, 0x3E) and the red light sweeps.

OpenCross: `render::SkinnedVertex::normal` (n / 128 plus the rotating joint) and `MeshPipeline::DrawParams::Light` (BK, rows, colours in the engine's mirrored world: the battle view mirrors the waist into PS1 space, `effects::light_matrices`, and the rows back). The vertex shader computes the corner multiplier `min(tint * c, 255 / 128)` and the fragment uses it instead of the flat tint. `BattleView::fx_lit_` is the 0x100 flag; `fx_lights_` / `fx_ambient_` are the slots and BK and persist across packs within a battle, as on the PS1. Not reproduced: the `>> 5` requantisation of the normal and the 8-bit corner colour; the animation-sequence light ops 0x5C-0x5E (light position := an actor's attach point, colour/intensity ramp).

## 4. S2 skeleton (joints)

`u32 N` then N records of 20 bytes: `s32 parent (-1 root)`, `s16 rx,ry,rz`, `s16 tx,ty,tz`, `u32 boneRef`. Parent < own index always. N: battle 22-40, field 2-30. `boneRef` is `-1` or `k << 16` (k counts up through the "real" bones in joint order; joint-less helper joints are -1).

OpenCross: the `MeshPipeline` joint palette holds 128 matrices (`kMaxJoints`), enough for the largest model (the 98-joint summon `sara`); joint ids above the limit would be clamped.

Angles are 12-bit: **4096 = 360 degrees**. Translations are in model units. A joint is placed in the model by:
```
u_parent = R * u_joint + t          R = Rz(rz) * Ry(ry) * Rx(rx)      (x rotation applied first; right-handed math, +y down)
```
This is the only Euler order that reproduces seam vertices to <1.5 units (all others >= 3.8 mean error) and assembles every model: bind pose has the arms down, feet at y = 0, head at negative y. The root joint translation is the pelvis height (negative y). The rest pose is the bind pose of all vertices.

## 5. S3 (0x160 bytes, constant)

**Attach points** at `0x000`: 32 entries `{s16 x, y, z; u16 joint}` (8 bytes each), position in the joint's space. Indices: 00 head top, 01 face, 02 neck, 03/04 R/L shoulder, 05/06 elbows, 07/08 hands, 09 waist, 0A/0B knees, 0C/0D ankles, 0E camera height, 0F attack approach distance, 10 torso, 12 weapon (Serge). Unused slots are zero.

**Shading entries** at `0x100`: 16 x `{r,g,b,flags}`, neutral `0x80 0x80 0x80 0x00`. Entry k applies to construct k (= OpenCross mesh k). Flag bits:

| bit | meaning |
|---|---|
| 0 (0x01) | construct not drawn (odd values = invisible) |
| 1 (0x02) | semi-transparent (GPU code 0x34 or 2) |
| 2-3 (0x0C) | ABR (`(flags & 0xC) << 3` into the tpage) |
| 4 (0x10) | faces filed at the nearest corner's z instead of the average (3.5) |
| 5 / 6 (0x20 / 0x40) | environment-mapped part (second primitive, UVs from normals; 5.1) |
| 7 (0x80) | with 0x20 / 0x40: env UVs inside the face's own half-page (5.1) |


Data seen: field models have no entry with bit 0; 0x10 appears on constructs 1/2 of 31 models (kid, guill, gren, poshul ...); 0x12 on harley / hosi / miki construct 1 and ilenes construct 3. Battle models: 0x12 on harley / hosi / miki / rablue (player), hosi / ryu_b (monster); 0x10 on `guill` constructs 2-4; bit 0 on 14 monster constructs (k_monk, ms_robo, su_a, kamei ...).

**Semi-transparency is per texel.** The packet code 0x34 or 2 makes the GPU blend only texels whose CLUT colour has STP (bit 15) set; texels without STP are drawn opaque, and 0x0000 is transparent. Field player CLUTs carry STP on every non-zero entry (1x and the cdrom2 / hd.dat TIMs alike), so those constructs blend as a whole; all 447 semi-transparent room-model constructs also sample STP texels only.

OpenCross: the importer sets `Mesh::trans = 1 + ABR` from bit 1 for every Mdl (`load_mdl`). Bit 0 stays an instance state (field ops 1B / 1C, `DrawParams::hidden_meshes`). STP is kept in the texture alpha (`import::kStpAlpha`, section 8). `MeshPipeline` draws non-STP texels opaque and STP texels with ABR 0 = (B + F) / 2 (an alpha blend at 0.5), 1 = B + F, 2 = B - F, 3 = B + F / 4.

### 5.1 Environment-mapped parts
* **With flag 0x80** (kamei part 3 = 0xA1, kamei_sp part 1 = 0xB0, shinshi, slime): the corner UVs come from the skinned normal bytes, `u = (nx >> 1) + 0x40`, `v = (ny >> 1) + 0xC0` (PS1 axes), inside the face's own 128-texel half of the page (the page bit adds 128). `DrawParams::env_meshes`; `mesh.vert` receives the texture width in `pc.fog.w`. While an effect overlay holds the env page (`FxModels::env_page`) the same parts sample the effect VRAM page (320, 256) / CLUT (0, 239) instead (kamei's scrolling `okat` copy).
* **Without flag 0x80 = the stock env page.** The packet's tpage becomes `0x95 | ((flag >> 2) & 3) << 5` (page (320, 256), 8 bit, ABR = flag bits 2-3) and its CLUT `0x3BC0` = (0, 239). Corner UV = `((nx >> 1) + 0x40, (ny >> 1) + 0xC0)` with no half-page base, so it samples texels u 0..127, v 128..255 of VRAM x 640.., y 256+ through CLUT row 239. This is **the battle stage's own texture**: the right-hand 128 x 128 block of the stitched 768 x 256 stage atlas (`ocstage envpage bg_NN` dumps it; 94 of 100 stages carry it, bg_56-58 / 96-98 and `kamei` do not). Flag 0x40 adds a second primitive (code 0x36, semi-transparent, same colour and corners) filed one slot nearer, drawn over the model's own primitive with ABR from the flag; flag 0x20 alone re-textures the model primitive itself (awa: parts 0x32 / 0x20). Both the triangle and quad builders do it. In a Lavos / Time Devourer capture, 195 of 198 env packets carry CLUT 0x3BC0, tpage 0xB5 (flag 0x44, ABR 1) or 0xF5 (flag 0x4E, ABR 3), code 0x36, colour 0x80 80 80 (the part tint), and the sampled texels equal the stage atlas texel for texel. OpenCross: `StageAsset::env_rgba` (import `build_asset`), `BattleView::stock_env_page_set`, `DrawParams::stock_env_set / stock_env_replace / stock_env_over / stock_env_abr` (an extra Solid + Semi pass for the 0x40 parts, a texture swap for 0x20), `mesh.vert` with `pc.fog.w < 0`. Models with such parts: monsters awa (0x32 / 0x20), bannin (0x42 x2, 0x40 x2), fate, hosi, kurage, lavos (0x44 / 0x4E), o_slime; players gyada (0x4C x2), jakotu (0x4C), malchera (0x5C), pierre (0x5C), zoah (0x5C).
* **Update rate.** The env UVs are rebuilt from the current pose for every packet build, i.e. every game frame; there is no texture-page, UV-scroll or CLUT animation on these parts - the moving reflection is the pose. Because a UV is an integer texel of an 8-bit normal (1 texel = 1/64 of a unit normal, ~0.9 degree), a slowly moving part steps its reflection one texel at a time (FATE: 22-70 % of faces change per 60 Hz build, every corner change exactly 1 texel, one corner jumping every ~5-25 frames). OpenCross recomputes them every rendered frame from `UnitPose::palette(alpha)`, so the same texel steps appear at a finer frame rate. `[Enhancements] SmoothReflections` (default 0 = retail) keeps the sub-texel fraction (`mesh.vert`: `nb * 0.5` instead of `floor(nb * 0.5)`), so the reflection slides continuously; it covers every part drawn by the stock unit path (flag 0x80 own-page and stock env page). Not covered: the overlays' own CPU-built env packets (`model_deform.cpp` `env_uv_of`).

### 5.2 Whole-model semi-transparency and the flash routine
Field ops AC / FE 45 (fieldscript.md) use model flag bit 1: it makes every drawn construct OR code 2 into every primitive and use the model's tpage word as is, whose ABR is set to n - 1. The model ABR replaces the shading entries' own; per-texel STP still decides what blends; colour is unchanged.

Draw routine 4 (FE 45, dry actors only) draws every polygon twice with code 0x26: first with the white CLUT (the model's 1-2 CLUT rows with 0x7FFF ORed into every non-zero entry, STP kept) and ABR 2 (B - F), then with the model's CLUT and ABR 1 (B + F). Non-STP texels end opaque with their normal colour; STP texels (every non-zero entry of the field player CLUTs) give `B - light + texel x light`, so the entity colour (op A1) fades the model in and out. The two primitives of a polygon go into the ordering table together, so the polygons are drawn back to front, each one white then normal before the next: a nearer polygon's subtraction removes what the farther ones added and, at a high light colour, the model looks opaque and darkened, turning translucent as the colour falls. The battle uses the same routine for the model-colour fade of anmseq op 0x8E (an enemy's death fade, S5 entry 9; Boxer's end-of-pack fade). OpenCross: `DrawParams::model_abr` / `flash`; the flash draw first writes the depth of the blended texels, so both blended passes reach only the nearest surface (`max(B - light, 0) + texel x light` per pixel) instead of adding up every layer of the model (the lid's red inside showing through Boxer's blue panels as a red-orange glow).

### 5.3 Attached-object mount records
`0x140`: **two mount records** of 16 bytes: `s16 rot[3]` (4096 = a turn, ZYX order), `s16 pos[3]` (model units, PS1 y down), `u16 attach` (attach point index), `u16 pad`. Slot k is drawn at `JointMatrix(points[attach].joint) * [RotMatrixZYX(rot) | points[attach].pos + pos]` (the offset is in the joint's space, not turned by `rot`), unless the slot's hide word is set (written by animation-sequence ops 79 / 7A).

Slot 0 is the weapon; slot 1 is **the same weapon mesh a second time**: battle init hands one weapon resource to both slots. The hide words start at 0 (shown). The battle-start script **S5 entry 8** (run on every unit) begins with `7A 1` in 51 of the 52 player models (slot 1 hidden); Harle, Lutianna and Sunef follow it with `7A 0` (no weapon at all); Glenn's second form `gren2` has none (both sticks show; its records are attach 31 and 30). Only the `atk` blocks of `banc` and `mell` ever show slot 1 again (hiding slot 0, showing slot 1 and back per swing: `7A 0 ... 79 1 ... 7A 1`, the hand swap). So Serge's and Kid's record 1 (the other hand, attach 7 / 8) stays unused: Kid carries one dagger.

Survey of the 52 player models: Serge `selju` slot 0 attach 18 (joint 14) + (0, 0, -96), slot 1 attach 7 rot (3072, 3072, 4064); Kid slot 0 attach 7 rot (3072, 3072, 4064), slot 1 attach 8 same; Guile / Doc / Fargo / Glenn / Pierre / Hosi / Irenes / Norris / Janice slot 0 attach 31 = a **dedicated weapon bone** (Guile joint 21, attach points 29 / 30 at z +440 / -536 along it) that the tech clips animate; Sneff attach 7 + (-32, 160, 8). All-zero records mean no mount object (Viper, Zoah, Poshul, Dragon, Harle, Leena-type unarmed or integrated weapons). The field model checked has zero records.

OpenCross: `assets::MountPoint` (`AttachPoints::mounts`); the `BattleView` weapon loop draws both slots, `anm_weapon_hidden` = a script's write, else `bseq::Table::initial_weapon_hide()` (the `7A` / `79` prefix of entry 8). An all-zero record keeps an older attach rule (point 0x12 / 7 / 15).

## 6. S4 / external animations

An animation pack is a **cpt with EOF pointer**; each entry is one animation:
```
u32  frameCount F
u32  flags[ceil(2J/32)]       2 bits per joint, bit 2j = rotation flag, bit 2j+1 = translation flag (LSB first)
u32  framePtr[F]              relative to the animation start; framePtr[0] == 4 + 4*ceil(2J/32) + 4F
frame 0   : 12 bytes per joint for ALL joints: s16 rx,ry,rz, s16 tx,ty,tz     (J = skeleton joint count; frame size = 12J)
frame k>0 : for each joint in order, 6 bytes rotation if rot flag, then 6 bytes translation if trans flag; size = 6*popcount(flags)
            the final frame may carry 1-3 padding bytes (0x77) up to the animation end
```
Every frame size matches over 25 anims x 252 models and all external packs. Joints with both flags set exist (e.g. carshu joint 6).

**Semantics.** The values are **deltas applied to the S2 rest pose**: `rot_final = rest_rot + delta` (per-axis sum, then R = Rz*Ry*Rx; for small deltas identical to `R_rest*R_delta`, while replacing the rest rotation breaks the spine) and `trans_final = rest_trans + delta`. Unflagged joints keep their frame-0 value. An all-zero frame 0 (e.g. anim 0 of every model, 1 frame) is the rest pose. The root translation delta moves the whole body (x forward, y = height change). Playback rate: 15 keyframes per second = 0.25 keyframe per 60 Hz tick (the default playback step 0x40 of 6.1; `assets::kModelAnimFps`).

**Slots.** Battle models carry 9-28 internal anims (index 0 empty/rest; PC clip 1 = stand-by idle, 2 = battle-ready stance held at the enemy, 3 = run, 15 = crouch). Field models normally carry 9 (default, stand-by, run, walk, idle, sit/climb, head turns, battle stance). **Attack animations are not in the model**: they live in `battle/<class>/<name>/at0.prd` (entry `at0`, type 0x19), also a cpt of anims with the model's joint count (194/194 packs' J equals the sibling model's J). `fieldobj/ex/<n>.bin` entry 0 is the same kind of pack for field models (event animations; ex files with a numeric suffix such as `jil00`, `yama01` match models by prefix; 10 have no `fieldobj/player` partner, J 21-28).

The summon motion streams `battle/effects/syoukan/st_01.mtc` / `st_03.mtc` / `st_02.mtc` are the same kind of pack: a 1-entry cpt of one animation (109 / 20 / 62 frames, 7 flag words, J = 98 = the `sara` model of `syoukan/saraman.prd`). saraman.prg hangs each on that model as its external table and plays it as SetAnim 0x80 (battle_effects.md; OpenCross `import::load_mdl_clips`).

### 6.1 Animation sequences (S5, ex .bin entry 1)
Every field character model drives its clips through a small per-model script, not directly: the script starts the clip, decides whether it loops, sets the **anim-done** variable that `WAIT_ANIMATION` waits for, and raises the **foot events** that make splashes, footprints and footstep sounds (room_effects.md).

**Tables.** Mdl S5 = `u32 n; u32 off[n]` (offsets relative to S5; 0 = none); entry i runs to the next higher offset. The same layout is entry 1 of every `fieldobj/ex/*.bin` (its clips are entry 0). All 56 field models carry 9 entries (ids 0..8), byte-identical in `cdrom2/`; room models have S5 = 0 (their clips are started directly). An entry = `s16 header[8]` (byte offset, from the entry start, of sub-sequence k; always 0x10 for k = 0) + the script as `s16` halfwords, padded with `0x7777`. The field only runs sub-sequence 0; the scene player may run others (e.g. 7).

A model has two script tables: ids < 0x80 use S5, ids 0x80 + k use the table of the loaded ex pack (set by LOAD_ANIMATION, cleared on unload). Per model the interpreter keeps a pc (0 = no sequence), a wait counter, a 4-deep return stack, and **16 s16 variables** (index `& 0x7F`), plus the loop flag (init 1), the cross-fade length and weight, the playback step (init 0x40 = 0.25 keyframe per tick) and the clip length (frames << 8).

**Start / run.** Each frame the id in force is submitted when it differs from the last one (ids >= 0x80 without a loaded pack become the stand id). With a sequence table the pc is set to entry + header[0] (counter and call depth 0, variables kept); without one the clip is simply started with a 0x20-tick fade. Every actor with a table is stepped once per frame (the second attached model too when its actor flag says so): handlers run until one yields (returns 0: the same op runs again next frame). A handler returns its length in halfwords; jumps move pc first. Waits count ticks.

| op | len | meaning |
|---|---|---|
| 00 / 3E / 4C | - | end (pc = 0) |
| 01 `off` | 2 | jump: pc += off, then + 1 (off counts from the op; Serge's run `01 -20` loops back to its first wait) |
| 02 `off` / 03 | 2 / 1 | call (push pc, pc += off + 1) / return (pop, continue after the 02) |
| 04 `a b c off` | 5 | if var a `c` var b continue, else pc += off (off 0 = wait until true); c: 0 ==, 1 <, 2 >, 3 <=, 4 >=, 5 !=, 6 & != 0 |
| 06 `a b c off` | 5 | as 04, else pc += off + 2 |
| 05 / 07 `off` | 2 | jump pc += off + 2 |
| 08 / 3C, 32, 1E | 2, 4, 8 | no-op |
| **09 `n`** | 2 | **wait n ticks** (n = 0 holds for ever) |
| **0A** / 42 `n` | 1 / 2 | **wait for the clip's end**: (frames - 1) * 4 - n ticks from now |
| **15 `clip blend`** | 3 | **start clip**: id < 0x80 = S4 clip, 0x80 + k = ex clip k, past the table = error / no change; `blend & 0x1FFF` = cross-fade ticks, bit 0x8000 = the old clip keeps running during the fade (6.2), bits 0x6000 = weight curve; restarts even the clip in force |
| 3F `va vb` | 3 | start clip var a with blend var b |
| 16 `r` | 2 | playback step = r (unused by field data) |
| **17 / 18** | 1 | **loop flag** = 1 / 0 (looping clips wrap, others clamp at the last keyframe) |
| 19 / 1A | 1 | clear / set model flag bit 0 |
| 1B / 1C `k` | 2 | clear / set bit 0 of shading entry k's flags (odd = invisible, section 5) |
| 1D `fl pt name name ...` | 11 | spawn library emitter `name` (2 halfwords) at attach point pt (flag 0x80: follows it) |
| 26 / 27 `a` | 2 | var a ++ / -- |
| 28 29 2A 2B 2C 76 **77** `a b` | 3 | var a += -= /= *= %= &= **or=** var b |
| 2D / 2E `ang a` | 3 | var a = rsin / rcos(ang & 0xFFF), `ang` immediate (4.12 result) |
| 2F `x y a` | 4 | var a = ratan2(y, x), x / y immediates |
| 30 `n a` | 3 | var a = rand() % n |
| 31 `a b c` | 4 | var c = SquareRoot0(dx² + dz²), dx = (var b - var a) >> 12, dz = (var b+1 - var a+1) >> 12 (the vars are (x, z) pairs) |
| 3D `a` | 2 | debug print of var a |
| **40 `a v`** / 4B `a b` | 3 | **var a = v** / var a = var b |
| 93 `k on` | 3 | set (on != 0) / clear flag 0x10 of shading entry k = nearest-corner ordering of construct k (section 5); not a show / hide |

Other table slots (0B-14, 1F-25, 33-3B, 41, 43 ...) belong to the battle model scripts; field data never uses them. Use in the field data (56 models + 115 ex packs): 15 x2057, 09 x1870, 40 x1746, 17 x1361, 18 x693, 0A x691, 77 x203, 01 x112, 1B x38, 1C x19, 1D x12 (colcha00 only), 93 x5 (harley00 only).

**Variables.** **var 0 bit 0 = anim done**: each frame it is ORed into the actor's anim-done bit; SET_ANIMATION (3C / A9) zeroes var 0 and the bit (fieldscript.md), so `WAIT_ANIMATION` ends when the new sequence reaches its `40 0 1`. **var 1 = foot events**: bits 0-3 = foot 0..3 landed (attach points 12, 13, 7, 8; room_effects.md), bits 4 / 5 = left / right footstep sound; read and cleared once per frame. Var 2 is scratch.

**Typical entries (Serge `selju`):**
* 1 stand / 0, 4, 5, 8: `17 15 n 8|10 40 0 1 09 0` (loop on, start clip n with an 8- or 16-tick fade, done at once, hold).
* 2 run / 3 walk: `17 15 n 8 { 09 20|28; 40 2 11; 77 1 2; 09 20|28; 40 2 22; 77 1 2; 40 0 1; 01 -20 }` = a foot event every 20 (run) / 28 (walk) ticks, alternating feet 0 / 1, done after the first cycle.
* 6 / 7 (the turn clips): `18 15 n 8 0A 09 8 17 15 1 10 40 0 1 09 0` = play the turn clip once, wait for its end and 8 ticks, then back to clip 1 (stand) and done.
* Ex sequences chain clips the same way (harley00 entry 1: `18 15 81 10 0A ... 17 15 82 1 40 0 1 09 0` = 0x81 once, then 0x82 looping).

**Op 1D** (colcha00 / mama00: `1D 1 16 'oar1' 0 0 0 0 0 0`): for every emitter header of the resident emitter library named `name` (the room's and particle.prd's) a follower object is made: it holds fl, pt, a life equal to the header's duration, an offset (ox, oy, oz) and angles. Its matrix is RotMatrix(angles) (mode `fl & 0x7F` = 0), RotMatrix(angles + model yaw) (1) or RotMatrix(angles) x joint matrix (2), scaled by the model scale; translation = attach point pt (world) + (ox, oy, oz). It is written as the header's anchor matrix and the header is restarted. Each tick: with fl & 0x80 the matrix is rebuilt; life decreases, and at <= 0 the follower is freed (the anchor keeps its last matrix). Not reproduced: a negative wait counter samples the joint at a later keyframe first.

OpenCross: `import::parse_anim_seq_table` (`ModelAsset::anim_seq` / `ex_anim_seq`; a LOAD_ANIMATION pack's table in `game::ExPack`); interpreter `Actor::run_seq`, run once per tick after the submission in `Actor::step_visual`. Ops 1B / 1C / 93 act on the instance's entry flags (`Actor::hidden_parts` / `near_parts`, initialised from the file's S3 at every model reset) and are drawn via `DrawParams::hidden_meshes` / `near_meshes`. Ops 2D / 2E / 2F / 31 and 1D are implemented (`Actor::take_bone_emitters`, `FieldWorld::step_bone_emitters`, `EmitterSystem::attach_all`).

### 6.2 Cross-fade
When a new clip starts, the old clip's pointer, time and length are kept as the fade source and the new clip's time and the weight are zeroed. The pose build advances the new clip's time every tick, but the **old clip's time only when blend bit 0x8000 is set** (clear by default), so the source pose stays frozen at the keyframe it had at the switch for the whole fade. A one-shot held at its last keyframe (loop flag 0) therefore blends out from that held pose; letting the source keep looping would make it wrap to keyframe 0 during the fade (the one-frame pose snap seen in the Guldove bed hop, Kid's fall, the Dragon Shrine bow). OpenCross: `Actor::start_clip`, `from_runs_`.

**Weight curve.** Blend bits 0x6000: 1 = exponential, per tick `d = 0x1000 - w; w = d < 4 ? 0x1000 : w + (d >> 2)` (0, 0x400, 0x700, 0x940 ... a quarter of the gap per tick, 28 ticks to 0x1000, whatever the length field says); 0, 2 and 3 = linear, `w += 0x1000 / (blend & 0x1FFF)` per tick. The pose is blended with the weight of the previous tick; the fade ends once the weight was already 0x1000, after which the single-clip path runs. No shipped sequence sets the curve bits: every op 15 in the 56 field `.obj` (606), the 115 ex packs (1458) and the battle `.obj` (1096) has `blend & 0x6000 == 0`. OpenCross: `Actor::fade_curve_`, `exp_blend_weight` (`actor.cpp`).

### 6.3 Rotation interpolation arc
Each joint is interpolated linearly between keyframe k and k + 1 (the last key interpolates toward key 0), for rotation and translation, at weight `(t & 0xFF) * 16 / 0x1000`; the clip blend does the same between the two clips' joint values. Before interpolating, each pair of rotation channels is normalised: a pair a turn (4096) or more apart has each value of magnitude >= 4096 reduced to `|v| & 0xFFF` (sign kept); then **only when the values are more than 0xC00 (270 degrees) apart** is the second moved by a turn toward the first. So pairs 180..270 degrees apart interpolate the long way round (retail is not shortest-arc). Example: the robots of `opn_a02i`, joint 26 channel x, stand clip 0 degrees versus clip s4_02 -182 degrees: a 180-degree shortest arc flips the leg during the cross-fade.

OpenCross: `game::retail_arc` (`src/game/anim_arc.h`), used by `sample_keys` and the blend in `Actor::locals` (field) and by `UnitPose` in `battle_pose.cpp` (battle uses the same routine); test `test_battle_anim` (`test_retail_arc`). Debug: `OC_POSE_TRACE=1` logs `POSETRACE` for any joint whose rotation turns more than 60 degrees between two ticks.

### 6.4 Head joint under a head turn
The joint-local matrix builder works per joint. With the skeleton record's flag bit 0 clear, the local matrix is rest + clip delta per axis (ZYX). With the flag set and a head-turn callback active, the callback builds the local matrix from the rest rotation/translation only, so the clip's rotation of that joint (attach point 1's joint) is not used while the head follows a target. FE 31 / FE 55 (look at another entity) take that entity's attach point 1 in the engine frame (`Actor::attach_world` = `engine_matrix` x joint), not the render frame, which would mirror the head offset.

## 7. Mesh (weapon `.kmd`, weapon-pack meshes)

A rigid mesh with no skeleton or skinning (17 `.kmd` and 105 pack meshes):
```
u32 0x80000001; u16 nVerts; u16 nFaces; u32 vertexOffset; u32 faceOffset(0x28); u32 groupHeaderOffset(0x14)
vertex: 8 bytes {s16 x,y,z, u16 pad}   (first field = x)
group header: u32 nGroups; nGroups x { u16 type, u16 count, u32 offset }  (offsets from the mesh start)
face packet = PS1 packet body: type bit4 = per-vertex colour (n x 4 B, else 1 x 4 B incl. code), bit3 = quad, bit2 = textured.
  textured: after colours  tri `u v clutLo clutHi u v tpLo tpHi u v` (10 B), quad 12 B ; then n x u16 vertex index.
  untextured triangles carry 2 pad bytes after the indices.   sizes: 0x34=28, 0x3C=36, 0x24=20, 0x2C=24, 0x20=12, 0x28=12, 0x30=20, 0x38=24
```
A weapon `.bin` is `(TIM 4bpp 128x85 + Mesh)` pairs (one per upgrade level; 8 pairs for `axe`) followed by two empty entries. A weapon is drawn rigidly at the owner's hand joint (attach point 07/08 or 12, or the S3 mount records, 5.3) and follows the owner's animation.

**Weapon names for effects.** The battle registers the equipped weapon mesh under the PC's two-digit character number ("02" = Guile, "19" = Glenn, "37" = Sneff, "39" = Doc), as ASCII tens digit | ones digit << 8. The weapon techs' effect packs (guill_1-3, stick / stick2, bigcard, high) draw it as a type-14 mesh sprite under that name.

**Monster weapons.** A monster carries a weapon when its monster.bin record has weapon item (+0x20) != 0 and weapon model (+0x2F) != 0. The `.kmd` at file-table entry `3985 + n` (n = weapon model) is used only to size the battle buffers: 1 sword, 2 m_adkz, 3 m_adky, 4 m_adkk, 5 s_kama, 6 tue/gin, 7 scop, 8 axe/dou, 9 kama2, 10 wing_ken, 11 kaji/flypan, 12 gun_d, 13 big_s, 14 monster/soiso, 15 ken/hagane, 16 monster/adk_yari, 17 monster/mop (all `battle/wepon/`). The kmd has no texture of its own and does not use the monster's texture (its polygons carry tpage `0x16` = VRAM (384,256) 4bpp and CLUT word `0x3fc0` = (0,255), UVs u 0..127 / v 0..84 = the 128x85 weapon window). The real source is the model's own weapon `.bin`: file-table entry `3195 + 3 m` (the third entry of the model's triple {`.ltd`, `.obj`, weapon `.bin`}, m = monster.bin +0x2D; Karsh m70 `axe.bin`, Solt `m_adky.bin`, Peppor `m_adkk.bin`, Acacia soldiers `m_adks.bin`, Radius `tue.bin`). Battle init loads it, takes pair `k = item-record byte 0 & 0x7F` (the weapon's tier, kept in the unit), uploads the pair's TIM to VRAM `(0x1A0, 0x100 + 0x55 * slot)` with CLUT row `0xF7 + 2 * slot`, rewrites every polygon's CLUT word to that row and adds `slot * 0x5500 + 0x80` to every uv/tpage word (u + 128, v + 85 per slot). So a monster weapon is exactly a PC weapon pack pair; the 17 `.kmd` are the same meshes (each equals the mesh of its bin's pair: dou = axe pair 1, sword = m_adks pair 0, adk_yari = monster/adk_yari pair 0 ...). The monster's S3 mount records and S5 entry 8 hide words apply as for PCs (monsters begin `7A 1`: slot 0 shown, slot 1 hidden; no record = attach rule 0x12 / 07 / 15). OpenCross: model -> bin table `kBattleWeaponBins` (`model_table.h`); `BattleView::load_enemy_weapon(model, part)` (part = `GearRec::tier() & 0x7F` of the monster's weapon item) reads the pack with `import::load_weapon_pack`.

### 7.1 Weapon renderer
There is one weapon renderer in the main library (no separate battle / field / menu renderer). Bit 0x1000 of the model flags chooses the draw routine (set = per-vertex normals with a light and colour matrix; clear = a view-direction light matrix):
* **clear**: only group types 0x34 (GT3) and 0x3C (GT4) are drawn; every other group is skipped.
* **set**: 0x20 (F3), 0x24 (FT3), 0x28 (F4), 0x2C (FT4), 0x34 (GT3), 0x3C (GT4). Types 0x30 / 0x38 (G3 / G4) are drawn by neither (the importer skips them).
* **Culling**: triangles are drawn when `NCLIP > 0` (no two-sided path). Quads in the flag-clear path test only `NCLIP(v0,v1,v2) > 0`; in the flag-set path `NCLIP(v0,v1,v2) > 0 || NCLIP(v1,v2,v3) < 0` (the main-library Mdl rule, 3.4). OpenCross culls weapons like Mdl constructs (either half facing the camera keeps the quad); the first-half-only rule differs only for non-planar quads.
* **Textured triangles** load their vertices as idx1, idx2, idx0 and attach the packet's corner data in packet order, so corner k (uv, colour) belongs to `idx[(k + 1) % 3]` - the same pairing as battle stage packets (battle_stage.md section 3). The importer (`parse_mesh`) pairs them so; the cyclic order (NCLIP side) is unchanged.
* **Specular term.** The GT3 routine adds a per-vertex specular term to each corner colour: the negated light direction reflected about the vertex normal, dotted with the normalised vertex-to-eye vector, clamped at 0, raised to the 9th power (`i * i >> 12` three times, then `* i >> 16`), min 255; colour byte = packet colour + term, clamped 255. The pairing is colour 0 += term(idx1), colour 1 += term(idx2), colour 2 += term(idx0). OpenCross: `DrawParams::Light::spec` (battle weapon draw, `battle_view_draw.cpp`).
* **Prism weapons (bit 0x1000 set).** Battle init sets the bit when the PC's weapon tier byte (item record byte 0) is exactly 5 (the prism / Spectra tier); nothing else sets it (the form-change rebuild does not, so a swapped model draws its weapon plain; monsters never). With the bit, per-vertex normals are built (face normal `normalise((b - c) x (a - c))` for triangle indices a b c in packet order, quads a b c d: `(a - b) x (d - b)`; each corner adds its face normal to its vertex normal and normalises again, every group in order, G3 / G4 included), and a follower task turns the lights each tick: angle x += 0x20, z += 0x40 (& 0xFFF, y 0), RotMatrixZYX, light matrix rows = that rotation applied to (0, 4096, 0), (-2896, -2896, 0), (2896, -2896, 0). The draw then uses LLM = weapon world matrix x that light matrix, colour matrix rows R (0, 0, 3072) / G (0, 3072, 0) / B (3072, 0, 0) (light 0 -> blue, 1 -> green, 2 -> red), back colour 0xC0, and the faces go through the flag-set routine (GT3: `NCCS` on the negated vertex normal, packet colour as RGBC; no specular). The turning lights give the rolling rainbow over the blade. OpenCross: `import::parse_mesh` builds normals for every rigid mesh, `Visual::weapon_prism` (set at battle start only), `prism_weapon_light` in `battle_view_draw.cpp` (lights turn by game tick plus interpolation). Weapons never take the unit's env-map part mask.
* **CLUT placement.** The pack TIM says CLUT (0, 255) and image (384, 256) 32 x 85 (kai pair 0 = the Sea Swallow), but the loader uploads the CLUT at (384, 511); the packets' CLUT word `0x3FC0` is relocated at load. The palette bytes are the file's, so decoding from the TIM's own CLUT (OpenCross) gives the same texels.

## 8. Textures

**Atim** (`.ltd`, each half of `.ltc`): `cpt{ CLUT object, image object [, image object] }`. Object header 16 bytes: `u32 type (0x1101 CLUT / 0x1100 image), u16 vramX, u16 vramY, u32 0, u16 w, u16 h`.
* CLUT: 256 x RGB555 (`w = 256`), `h` rows (1 or 2; 2 = alternate palette, 33 monsters). Colour 0x0000 is transparent. Bit 15 is STP (per-texel semi-transparency, section 5). OpenCross keeps STP in the RGBA alpha of every model texture (Atim, field TIM, HD TIM, room-model VRAM window): 0 = colour 0x0000, `kStpAlpha` 0xBF = STP set (0x8000 = opaque black + STP), 255 = STP clear.
* Image: `w` in 16-bit words (pixels = 2w, 8bpp), `h` rows, data size `2*w*h`; VRAM position (0,256) for the main image. Sizes seen: 128x256 (players, most monsters), 256x256, 128x128, a few odd (112x128, 64x...). Two-image files (kurage, manet) hold the images stacked vertically (v >= the first image height addresses the second).

**Closed-eye strip.** The `.ltc` second Atim is a 128x32 strip at VRAM (960,256) (384 for one player) sharing the same palette. It holds the closed-eye frames: 128 px = two 64 px frames (1 = half shut, 2 = shut), each a left and a right 32 x 32 half; the VRAM stash is `(960 + 32 * (k - 1), 256 + 32 * u)` per party slot u. The two destination rects in the main texture are the four bytes `x1 y1 x2 y2` of the **u32 at +0x80 of S5 entry 0** (no code there; x in texels, used as halfwords `x >> 1`; y relative to the texture page, 8bpp texels), 32 x 32 texels each: Serge `(26, 28)` twice (both eyes share one region; the right half then overwrites the left), Harle `(48, 37)` + `(18, 37)`, Kid `(14, 22)` twice. Frame 0 is the model's own texture (the open eye is copied to a stash at battle start; see battle ops 72 / 73 / 4F). The cdrom2 (HD) `.obj` of selju2 / kabuo / lazzuly / riddle carry other rects (their atlas differs). Some HD textures re-lay the atlas but keep the PS1 rect word (Guile: rect (52, 28) is braid in the HD texture, the eye is at (2, 28)); the importer therefore checks each rect against the strip's half-shut frame (share of the 32 x 32 texels within a small colour distance) and, when the fit is below 0.1, moves the rect to the best-fitting position (scan step 2 texels in x) if that fit is at least 0.5 (`fit_eye_rects`). OpenCross: `ModelAsset::eye` (rects, parsed in `load_mdl` for models with a strip), `eye_frame_pixels` (the strip pasted over the rects, scaled by texture width / 128 for HD or mod-replaced textures), `MeshModel::eye_base` (the two variants are extra GPU textures; `DrawParams::eye_frame` picks them).

**TIM** (field): standard PS1 TIM, flag 9 (8bpp, CLUT), 256x128-word layout, 16928 bytes.

## 9. Differences between classes

| | battle player/monster | field (overworld) | weapon / kmd |
|---|---|---|---|
| Container | Mdl (6 sections) | Mdl, same | cpt / Mesh |
| Joints | 22-40 (players), 11-41 (monsters) | 2-30 | none |
| Scale | ~1100 units tall | ~2100 units tall | |
| Texture | `.ltd/.ltc` Atim 8bpp | `.tim` 8bpp TIM | TIM 4bpp in pack |
| Anims | S4 (9-28) + `at0.prd` | S4 (9) + `ex/*.bin` | uses owner's |
| CLUT / page bits (3.3 rule 4) | monsters only | never (room models: 57) | - |

## 10. Validation

`python -I tools/py/cc_model.py validate` parses every class: 52 battle players, 148 monsters, 56 field `.obj`, 158 `.ltd` / 52 `.ltc`, 17 `.kmd` / 29 weapon `.bin`, 115 of 116 `ex/*.bin` (`dummy.bin` is a non-cpt stub), 194 `at0.prd` (anim J == model J), 56 field `.tim`. Checks per model: header/EOF, section monotonicity, skeleton parent order, vertex pool offsets and sequential global indices, every face index inside its own construct, all back-references resolve, 16-byte weights sum 4095/4096, every animation frame size, S3 size, texture sibling exists, UV within image, external anim J.

Quirks: `P_gorem` has no texture; `mizu_c` uses the page flag with a 128-px image; the last animation frame may carry 1-3 padding bytes; 16-byte weights sum to 4095 rather than 4096 (rounding of x/4095).

## 11. Unknowns
* Unknown: whether the animation Euler composition is a sum of angles or a matrix product; the two are visually identical for small deltas and the sum is used.
* Unknown: which weight belongs to which record in 16-byte vertices (settled by distortion tests only).
* Unknown: the S1 words beyond the extents, the S2 `boneRef` purpose, the S3 tail 0x20 bytes, and the contents of some S6 DRP entries.
* Whether some anims are state-dependent aliases of `at0.prd` anims (which index = which action) is a game-logic question, not a format one.

## 12. HD textures (hd.dat, cdrom2.dat)

* Plain TIM (magic 0x10, flags bit 3 = CLUT), no extension. The HD CLUT carries STP like the 1x one (field players: every entry but 0x0000), and RDE takes STP and 0x0000 transparency from the HD CLUT colour; `load_hd_tim` keeps it (section 8). 8bpp with a single 256x1 CLUT carried by the HD file itself (the 1x CLUT is not used), except the small `battle/battle_*.tim` and `map/etc/tile.tim` (4bpp, 16x1 CLUT). The VRAM x/y in the header is the 1x position (e.g. field players `0,256`) with the width in halfwords already scaled, so only the pixel size changes. `ochd -v` decodes all 115 `hd/**/*.tim`.
* Field players `hd/fieldobj/player/<n>/<n>.tim` (51 with a `cdrom/` counterpart): 512x512 px versus the 1x 128x128, exactly 4x in both axes. Texel (u,v) of the HD-mode 1x image is (4u,4v) at HD, so UVs `(u+0.5)/w` are valid unchanged **provided the mesh and 1x TIM come from the HD-mode set**.
* **HD textures only fit the `cdrom2.dat` models.** `cdrom2.dat` is the HD-mode override of `cdrom.dat`: it holds `fieldobj/player/<n>/<n>.obj` and `.tim` for every playable/party field model (and `battle/player/<n>/*`, `menu/player/*`), with a different mesh and UV atlas (selju 28336 B vs 24940 B, kid 26764 vs 23076; the 1x `.tim` is also a different 128x128 atlas of the same size). The `hd/` TIMs are painted on the cdrom2 atlas; combined with the `cdrom/` mesh UVs the islands land in the wrong places. Rule: in HD art mode read `cdrom2/<path>` first and fall back to `cdrom/<path>` (caller-side), for `.obj`, `.tim` and the HD TIM together; SD mode reads `cdrom/` only. `ex/<n>.bin` exists only in `cdrom/` (the cdrom2 skeletons keep the same joint count). `ochd -m <name> <outdir>` exports the cdrom2 OBJ with its UVs.
* cdrom2 `.obj` files may be longer than their EOF pointer (word at 0x1C): 5 models (farga, ilenes, kabuo, kinoko, lazzuly) carry a 0.1-0.5 KB trailing block (a second `drp`-looking table, unused). `load_mdl` truncates to the EOF pointer instead of rejecting the file.
* **`menu/player/<name>.ome` (Status screen model) in HD mode is the cdrom2 variant too**, and differs from the disc one: its Mdl (cpt entry 3) has a **4-section header** (count 4, S1..S4, then the EOF word, S1 at 0x18; the disc one has 5 sections, S1 at 0x1C); the "EOF" word equals S4 (the anim pack follows and runs to the end of the entry, so the word must not be used to truncate); entry 5 (Atim) is empty - the hd.dat TIM (`hd/menu/player/<name>.tim`, 1024 x 1024 = 4x a 256 x 256 atlas) is the only texture; and the mesh is the 4x-UV one (selju 1510 vertices vs 925 on the disc). OpenCross: `ui::StatusModelView` rebuilds either header into the 6-section form; `import::load_model` stands in a 1x main texture of hd/4 when there is no Atim but an HD TIM is given.
* Models without an HD TIM (NPC/room models) keep the 1x-only texture path. Room models do have cdrom2 variants: 213 room `.bin` files differ from the disc in subfiles 11+ (room-model meshes / UVs) and 214 `.ctd` files in their texture pages. The cached `char_models_` / room models are rebuilt when the art mode toggles because mesh and texture both change.
* **A mod's 4x TIM at the 1x path** (HD Boat: `fieldobj/player/adk_sp|neko|neko2|cl_bort`, 512 x 512): field model UVs are texel coordinates of the 128 x 128 atlas, so a 512-wide texture must not normalise them. OpenCross `load_field_model` subsamples a square image wider than 128 to the 1x texture and passes the original as the HD texture (`hd_scale` = width / 128) when no hd.dat TIM is given.
* **Clips are the same data in both sets**: the anim section (S4) of every `fieldobj/player` and `battle/**` `.obj` is byte-identical between cdrom and cdrom2 (51 / 53 files). The skeleton section (S2) differs in 6 field models (kabuo, liia, malchera, mell, sunef, zoah; same joint count) and S5 (battle state scripts) in 5 battle models (yamaneko, kabuo, lazzuly, riddle, selju2). OpenCross: the art switch hands the rebuilt model to the actor without touching its playback (`Actor::swap_model_art`; in battle the mesh and GPU copy replace the old ones inside the shared `ModelHandle`) and keeps the loaded battle scripts.
* Without a same-path 1x counterpart (64): `menu/player/*.tim` 1024x1024 (portraits, 39), `battle/battle/battle_04|33*|37` (4bpp, 256x64, 576x60, 128x128), `battle/canon/face`, `battle/shine/renz0`, `map/etc/tile`, `fieldobj/player/roulet0|roulet1`, and `fieldobj/player_sd/doc/doc.tim` (128x128, the SD-size model; its HD scale is not 4x of the field texture). Unknown: the scale relation for these.
* OpenCross importer: `import::load_hd_tim(bytes)` -> RGBA `Image`; `load_model` / `load_field_model` take an optional trailing `hd_tim` span and, when its size is an integer multiple of `textures[0]`, fill `ModelAsset::hd_textures[0]` and `hd_scale` (main texture only; alternate CLUT rows and overlays have no HD image). Read with `ModelAsset::texture_for(i, hd)`.
* **Battle models have no hd.dat TIM**: their HD art is only the cdrom2 `.obj` + `.ltc` pair (same 1x size, other mesh / atlas). Effect packs have no cdrom2 member, so RDE draws pack models with disc data. OpenCross gives a pack Mdl that is a byte copy of a battle model (S1 + S2) on that model's disc texture the model's cdrom2 mesh and texture in HD art (`import::mdl_with_mesh_of`, `mdl_section`, `load_atim_main`; only sky's `act0` = selju2/selju qualifies): see battle_effects.md.
* **Flat / submerged room models.** A model with one axis scale 0 (the Hydra before its rise: y 0) is a planar silhouette, drawn like any other (the one-sided test still applies per face). Its faces are all below the water level, so only the submerged pass draws it; that pass keeps its own uniform slots in the frame (`Frame::serial`), or it would be drawn with another actor's matrix / palette (room_effects.md "Water").
