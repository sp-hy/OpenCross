#pragma once
// Bespoke battle-effect overlays: the 89 hand-coded overlay .prg files re-expressed as
// data-parameterised C++ modules. Each effect emits FxPrim (screen-space gouraud tris / quads / lines with the GPU blend modes of
// the emitters + an "invert backdrop" mode) instead of textured sprites, so it sits next to EmitterSystem's DrawQuads in the same
// frame. Lifecycle: start(ctx) -> tick() per 60 Hz step -> draw() -> done(). Private overlay ops (0x100.., 0x180..) arrive
// through command().
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "game/effects/effect_slots.h"
#include "game/effects/emitter_system.h"
#include "game/effects/fx_math.h"

namespace oc::effects {

// ---- output primitives -----------------------------------------------------------------------------------------------------
struct FxVert {
  float x = 0, y = 0;  // layer space (320x240)
  u8 r = 128, g = 128, b = 128;
};
// blend: 0 opaque, 1 B/2+F/2, 2 B+F, 3 B-F, 4 B+F/4 (same as DrawQuad) and 5 = invert the backdrop (the NEGAPOSI screen negative).
constexpr int kBlendInvert = 5;
// FxHooks::set_stage(kStageSlot + e): the stage files of effect slot e. The overlays read group-11 local files:
// 3 w + 2 / 3 w + 3 = the .ctd / .prd of slot w + 1 (slot(e) = fnd 4322 + 3 e + {0, 1, 2}); lasbos ops 0x100 / 0x101.
constexpr int kStageSlot = 0x10000;
struct FxPrim {
  enum class Kind : u8 { Tri, Quad, Line } kind = Kind::Quad;
  // Tri uses 0..2, Line 0..1, Quad corners in DrawQuad order (0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right)
  FxVert v[4];
  int blend = 0;
  int slot = 0;      // 0..0xFFF, larger = farther (same rule as DrawQuad)
  u32 order = 0;
  // Textured primitive (the overlays' own GT3 / GT4 packets of a pack mesh, 0x34 / 0x3C): texture index into FxContext::meshes->textures
  // and per-corner UVs as stored in assets::StageVertex::uv (the host applies its V flip). -1 = untextured (gouraud colour only).
  int tex = -1;
  float u[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};
  // Texture source (the overlays' private model draws): Pack = `tex` of the pack's mesh textures (above); Model =
  // texture `tex` of host unit `unit`'s battle model, u / w normalised 0..1 (v down, no flip); Vram = the 256 x 256 texel window of the
  // pack's VRAM at tpage word `tpage` with CLUT word `clut` (as the overlay writes them into its GT3 / GT4 / FT3 packets), u / w in
  // texels 0..255 (the VRAM moves of FxModels::vram_moves applied). Colours are per corner (Gouraud) for every textured source.
  // PackModel = texture `tex` of the pack's sprite-type-20 model FxModels::pack_model (the trap summons), u / w as Model.
  // Env = the stock env page (the stage's own picture), u / w in texels 0..255.
  // Capture = the frame copy FxModels::capture asks for, u / w in layer pixels of the copy (320 x 224, the same grid as the vertices): the host
  // makes the copy before the first item at or above the request's slot and maps the pixels to the copy at any render scale and aspect.
  enum class TexSrc : u8 { Pack, Model, Vram, PackModel, Capture, Env } src = TexSrc::Pack;
  int unit = -1;
  u16 tpage = 0, clut = 0;
  // Model only: the texture drawn through a white CLUT (the global rows the battle loads at (0, 243) / (0, 245), or the white
  // copies of the model's own rows the death overlays load at (0x100, 0xEC)): 1 = every texel that is not transparent becomes white (row 243:
  // index 0 transparent; the model-CLUT copies: non-zero entries), 2 = every texel white (row 245, garadeth's all-0xFFFF rows). STP set.
  u8 white = 0;
};
// Sorts farthest first, later submitted first inside one slot.
void sort_prims(std::vector<FxPrim>& prims);

// ---- meshes / actors / context ---------------------------------------------------------------------------------------------
struct FxMesh {
  std::vector<Vec3> verts;                  // actor-local, y down
  std::vector<std::array<u8, 3>> colours;   // per vertex (0x80 neutral)
  std::vector<std::array<u16, 3>> tris;
  Vec3 centre{0, 0, 0};
  float radius = 1;
  void finish();  // recomputes centre / radius
  // Synthetic humanoid (torso + head + limbs as boxes) about 300 units tall, used by the dummy stage and the tests.
  static FxMesh dummy_figure(u8 r = 200, u8 g = 160, u8 b = 120);
};
struct FxActor {
  Vec3 pos{0, 0, 0};   // world position of the model origin
  const FxMesh* mesh = nullptr;
};
struct FxHooks {
  // Overlay wants an actor's own model hidden (it re-emits the mesh) / its tint changed / a stock effect spawned.
  std::function<void(int side, bool visible)> set_visible;  // side 0 = caster, 1 = target
  std::function<void(int side, int r, int g, int b)> actor_tint;
  std::function<void(int side, const Vec3& pos, float yaw)> actor_pose;  // scripted-motion overlays move the actors
  std::function<void(std::string_view name)> spawn;
  // Starts the pack's sequence `name` bound to entry `caster` of the acting list.
  std::function<void(std::string_view name, int caster)> start_for_caster;
  // Scene remaps (seint / unico / etrap override the party / enemy getters; bgchng / miyademo swap the backdrop).
  std::function<void(int side, bool listed)> set_listed;  // actor in / out of the getter lists the sequence ops iterate
  // Replace the battle backdrop (-1 free it, 0..99 bg_NN, 100 battle/bg/kamei,
  // kStageSlot + e = the .prd / .ctd of effect slot e: lasbos 0x101 / 0x102).
  std::function<void(int stage)> set_stage;
  // hensin 0x182 / 0x183: the host's Dopplegang form change (1 = starts, 2 = the new form takes over).
  std::function<void(int phase)> form_swap;
  // hinsi / lasdes4 / kamedes: sets a unit's idle-pose word (the one the battle script's pose op writes; 1 = stand-by, 8 = dead).
  std::function<void(int unit, int pose)> set_pose;
  // Clears the depth cue flag the pack's op 0x24 set (kamei state 3).
  std::function<void()> fog_off;
  // The GTE light rows / colour columns built for a world point (PS1 axes) from the battle's light slots, and the back colour (bytes / 256):
  // what the lit packers read (kamei's dome). False = no lighting (dummy stage).
  std::function<bool(const Vec3& pos, LightMatrices& matrices, std::array<float, 3>& back)> lights;
  // The global anmseq word 0 that battle scripts write: spark's impact helper runs while it is non-zero and clears it at the last slot.
  std::function<int()> script_gate;
  std::function<void()> clear_script_gate;
  // The song-change request of the field's music player: the current song ends and `song` starts (lasdes2 ops 0x101 / 0x103).
  std::function<void(int song)> play_song;
};
// ---- battle models the overlays steer (PS1 model block fields) ------------------------------------------------
// Units and axes are the PS1's: world y down, positions in world units (the 20.12 fields >> 12), angles 4096 = one turn, scale 4096 = 1.0.
// The unit index FxModels::skin and the Model texture source take for the pack's pseudo-actor (caster[-1], the `act0` model).
inline constexpr int kPseudoUnit = -2;
struct FxModel {
  int unit = -1;                         // host unit index
  bool pc = false;
  // Snapshot when the effect starts: the list entry's s16 home position = the model position. Rotations are the model block's, rot[1] the
  // 12-bit yaw: 0xC00 looks along +x, 0x400 along -x.
  Vec3 home{0, 0, 0};
  int home_rot[3] = {0, 0, 0};
  float height = 1000;                   // model extent along y
  int hit = 0;                           // the action's result on the unit (0 hit, 1 no damage, 2 miss, 3 heal)
  bool dead = false;
  // Live block fields. `steer` = an overlay wrote position / rotation / scale: the host then draws the model at pos / rot / scale
  // instead of its own placement (until the effect ends).
  bool steer = false;
  Vec3 pos{0, 0, 0};
  int rot[3] = {0, 0, 0};
  int scale[3] = {4096, 4096, 4096};
  bool tinted = false;
  int rgb[3] = {128, 128, 128};          // 0x80 = neutral
  bool hidden = false;                   // not drawn
  // A unit a battle script keeps off-stage (ghost ops 34 / 35 / 5B) when the effect starts, and `reveal` = the pack shows it (the model's hidden
  // bit cleared by the pack's own entry; the ghost word stays until the script's ghost op). The host keeps drawing it after the effect ends.
  // The host copies rot[1] into the unit's facing every tick (opthedo's slide: the pup takes the door's yaw).
  bool face_rot = false;
  bool offstage = false, reveal = false;
  // The overlay lets go of the model: the host copies its x / z / facing into the unit's own placement and stops steering it (the overlay moved the
  // model the way the engine's own tasks do; what the unit's anmseq entry does next - MoveHome - starts from there). opthedo's pup at the end of its slide.
  bool release = false;
  u32 part_hide = 0, part_show = 0;      // per-part flag bit 0 forced on / off
  u16 flags = 0x200;                     // model flags (0x200 = drop shadow)
  // Requests the host consumes (and clears) after every tick.
  // SetAnim(model, clip, flags): clip < 0x80 = the model's own S4 clip, >= 0x80 = table clip.
  int clip = -1, clip_flags = 0;
  // The keyframe count of the clip the host last started for this model (what an overlay's wait reads); the host sets it
  // when it consumes `clip`, so an overlay reads it from the next tick on.
  int clip_frames = 1;
  // Hang the pack's own type-0x19 entry `clip_pack` as the model's external clip set (SetAnim 0x80 + k); consumed by the host. "" = none.
  std::string clip_pack;
  // The pack set the model's hidden bit and the host keeps it hidden after the effect ends until the unit's own rules
  // (the script's ghost words) say otherwise (kamei: the shell form once the new form has taken its place).
  bool conceal = false;
  // The model was rebuilt mid-effect: its block starts clean, so the visibility ops' hide bit is gone, and it plays its spawn script.
  // Consumed by the host.
  bool rebuilt = false;
  // The model's block is given up (ThirdSlotRelease): `hidden` holds through every show op until the reload.
  bool unloaded = false;
  // Run an anmseq entry on the model: script_entry >= 0x80 = entry of the external table; script_table = tag of the type-0x1A entry
  // assigned as the external table first ("" = keep).
  int script_entry = -1, script_variant = 0;
  std::string script_table;
  void move_to(const Vec3& position) { pos = position, steer = true; }
  // Overlay draws of the model's own mesh (the ports that skin the model and re-emit it: 44get / 45infe / 38super ...).
  // The host skins the posed mesh on the CPU, moves every skinned vertex (world, PS1 axes) by `offset`
  // and then through `deform`, and draws the faces textured, back faces culled (NCLIP), one OT slot per face, colour `rgb` (0x80 neutral)
  // and blend `abr` (-1 = opaque, 0..3 = the PS1 ABR of a semi-transparent model, model flag 2 + tpage ABR). The module rebuilds the list
  // every tick; set `hidden` as well when the overlay hides the normal draw.
  struct Copy {
    std::function<void(Vec3&)> deform;
    Vec3 offset{0, 0, 0};
    int rgb[3] = {128, 128, 128};
    int abr = -1;
  };
  std::vector<Copy> copies;
  // The overlay's release clears the hidden bit (lasdes1): fx_end shows the model again.
  bool unhide_at_end = false;
  // An overlay draws this model itself through FxModels::skin (trace only).
  bool custom_draw = false;
  int anim_speed = -1;                   // -1 = untouched, 0 = the pose is frozen (the host skips its step)
  // The model's own CLUT rows faded toward grey (ki_deth / octdeth op 0x180, per 5-bit entry: g = (R + G + B) / 3 * k, R' = (g + R (0x1000 - k))
  // >> 12 ...): k 0..0x1000, -1 = the original colours. grey_stp: ki_deth ORs STP (0x8000) into every non-zero entry (zero stays transparent);
  // octdeth writes no STP, so an entry that ends 0 (0x8000 STP-only black) turns transparent. The host re-uploads the textures on a change
  // (every unit sharing the model sees it, as the PS1 CLUT is shared) and restores them when the effect ends.
  int grey = -1;
  bool grey_stp = true;
  // The overlay's draw draws the unit shadow itself: drawn while hidden, regardless of 0x200.
  bool own_shadow = false;
  // Joint `joint`'s world-matrix translation in the model's own space (y down, the placement not applied), from the
  // host's posed model (FxUnit::joint). Unset when the unit has no model. `joint_world` (overlay_base.h) adds the placement.
  std::function<bool(int joint, Vec3& local)> joint_local;
  // The model's size fields set at load (the S1 extents scaled, 4096 = 1.0; FxUnit::fx_scale / fx_scale_h): the overlays'
  // size fields (42hevi's dent radius = fx_scale_h / 6, clamped to 0x800).
  int fx_scale = 4096, fx_scale_h = 4096;
  // Previous tick's pos / rot / scale (the host copies them before each tick) for the interpolated draw.
  Vec3 prev_pos{0, 0, 0};
  int prev_rot[3] = {0, 0, 0};
  int prev_scale[3] = {4096, 4096, 4096};
  void save_prev() {
    prev_pos = pos;
    for (int k = 0; k < 3; ++k) prev_rot[k] = rot[k], prev_scale[k] = scale[k];
  }
};
// Draw pose of a steered model at `alpha` (0 = previous tick, 1 = this tick): `pos` = world position with y UP (the battle view's axes),
// `rel` = rotation * scale in model space (PS1 axes, y down) relative to the home pose: Ry(-home ry) * RotMatrix(rot) * Scale (the root is
// the rotation matrix scaled by the model scale). The host draws translate(pos) * its own home yaw * y-flip * rel.
struct FxModelPose {
  Vec3 pos{0, 0, 0};
  Mat3 rel;
};
FxModelPose fx_model_pose(const FxModel& model, float alpha);
// The model an overlay skins itself (its private copy of the model draw): WORLD vertices of the posed model (PS1 axes, y down; the s16
// values the skinner writes) and the normal bytes it writes (n << 5 rotated by the joint's world matrix,
// clamped to +-0xFFF, >> 5), and every part's faces in UM order with their texture coordinates. Quads keep their GPU Z-order corners.
struct FxSkinFace {
  u8 n = 3;
  u32 v[4] = {0, 0, 0, 0};          // indices into FxSkinnedModel::verts
  float u[4] = {0, 0, 0, 0}, w[4] = {0, 0, 0, 0};  // normalised 0..1 texture coordinates of the face's model texture (v down)
  // Texture index of the unit's model (the CLUT bit selects texture 1; the page bit is folded into u).
  int tex = -1;
  int part = 0;                     // construct (S3 shading entry)
};
struct FxSkinnedModel {
  std::vector<Vec3> verts;
  std::vector<std::array<s8, 3>> normals;
  std::vector<FxSkinFace> faces;
  std::array<std::array<u8, 4>, 16> shading{};  // S3 r, g, b, flags per construct
  std::vector<int> tex_width;                     // per model texture, in texels (the env UVs of shading flag 0x80 parts)
  int parts = 0;
  Vec3 pos{0, 0, 0};                 // model position >> 12 (the pack model: as its emitter object last set it)
  int rgb[3] = {128, 128, 128};      // model colour (the pack model: the object colour >> 4)
};
// The GTE depth cue as the frame stands: p = far (z - near) / ((far - near) z), colour moved toward `rgb` (0x80 = neutral) by p.
struct FxDepthCue {
  bool on = false;
  float near_z = 0, far_z = 0;
  float rgb[3] = {128, 128, 128};
  float amount(float z) const { return on && far_z > near_z + 1.0f && z > 0.0f ? std::clamp(far_z * (z - near_z) / ((far_z - near_z) * z), 0.0f, 1.0f) : 0.0f; }
};
struct FxModels {
  std::vector<FxModel> units;            // indexed by host unit
  // Set by the host before the overlays draw.
  FxDepthCue depth_cue;
  std::vector<int> casters, targets;     // the acting / target lists, indices into units
  // "Everybody": the units in the order battle.prg registered them: the enemy units (unit order) first, then the party. kamei's
  // everybody(1) is the second enemy.
  std::vector<int> everyone;
  FxModel* everybody(int i) { return i >= 0 && static_cast<size_t>(i) < everyone.size() ? &units[static_cast<size_t>(everyone[static_cast<size_t>(i)])] : nullptr; }
  int cursor = 0;                        // the per-target loop index of the sequence when a private op arrives
  // Host: skins unit `unit`'s model in its current draw pose (steered placement included) into `out`; false when the unit has no model.
  // Called by the overlays' draw() (null in tests / tools without models).
  // The target / caster getters replaced by the overlay (seint unico etrap tunico): the lists the
  // sequence ops and the emitters' actor selectors see while it runs, as unit indices (repeats allowed, empty = every index gives NULL).
  // Unset = the stock lists. The overlay restores them at its exit (the host drops the override when the effect ends).
  bool caster_override = false, target_override = false;
  std::vector<int> caster_list, target_list;
  // An overlay hung the pack's rigid mesh `mount_mesh` on mount record 0 of the pack's model `mount_model` (seint: the angel's spear).
  std::string mount_model, mount_mesh;
  std::function<bool(int unit, FxSkinnedModel& out)> skin;
  // The disc read of a unit's model files, in ticks; set by the host.
  std::function<int(int unit)> model_read_ticks;
  // The overlay closed the pack before the battle may go on: its sounds are cut. Consumed by the host.
  bool pack_closed = false;
  // Host: the WORLD position (PS1 axes) of attach point `point` (S3 table) of the posed unit model.
  std::function<bool(int unit, int point, Vec3& out)> attach;
  // caster[-1] (the pack's `act0` pseudo-actor model): the same for that model. Unset / false = no pseudo-actor.
  std::function<bool(int point, Vec3& out)> pseudo_attach;
  // The pseudo-actor's hidden bit as an overlay writes it (1 set, 0 clear, -1 none); the host applies it after the overlay tick and
  // resets it. `pseudo_own_shadow` = the overlay draws the hidden pseudo-actor itself and the host still draws its shadow.
  int pseudo_hide = -1;
  bool pseudo_own_shadow = false;
  // The pack's own model (type 0x14 = the drp's type-0x0B Mdl record drawn by sprite-type-20 objects, which copy the object's place,
  // angles, scale and colour into it every tick): skinned like `skin` at the place the last of its objects gave it. `pack_model` = its name
  // (set by the host); `pack_model_hidden` = the hidden bit set by an overlay: the objects' draws of it are skipped.
  std::function<bool(FxSkinnedModel& out)> skin_pack;
  std::string pack_model;
  bool pack_model_hidden = false;
  // Keyframes the overlay's own pose steps added to the pack model on top of its objects' motion counter (the trap summons: every tick
  // = K / 256 keyframes, K = 4 / 32 / 64 / 64; the model block keeps one frame counter, so the extra steps accumulate).
  // The host adds it to the objects' motion frame when it skins the pack model.
  float pack_extra_frames = 0;
  // The overlay owns the pack model (saraman: no sprite-type-20 object places 'sara'; the overlay writes its place,
  // calls SetAnim, hangs streamed motion files on it (the model's external clip table := the .mtc, so SetAnim 0x80 plays that file's
  // animation), sets its anim speed (64 = one keyframe per 4 ticks, 0 = frozen) and draws it itself through `skin_pack`). While `active`,
  // `skin_pack` skins model `name` at this place and pose instead of the objects' one. `motion_request` = a file the overlay streams
  // (group-11 local index 0x2D + n = st_01 / st_03 / st_02, n = 0..2); the host loads and decodes it after the tick and reports
  // `motion_state[n]` (0 = not loaded, 1 = loaded, 2 = failed: treated as loaded with no clip, so the overlay's wait cannot hang on a
  // missing file). `at_end` = the playing clip reached its last keyframe (a 1-frame clip is at its end at once). `hidden` = the
  // model's hidden bit (the stock draw skips it).
  struct PackDrive {
    bool active = false;
    std::string name;
    Vec3 pos{0, 0, 0};
    int rot[3] = {0, 0, 0};
    int scale[3] = {4096, 4096, 4096};
    int rgb[3] = {128, 128, 128};
    int speed = 64;
    // SetAnim request (< 0x80 the model's own clip; 0x80 the attached motion file's); consumed by the host.
    int clip = -1, clip_flags = 0;
    int motion = -1;                    // the attached motion file (index n), -1 = none
    int motion_request = -1;             // file index n to stream; consumed by the host
    // A host consumes the requests (BattleView); without one BattleFxRun acknowledges them.
    bool host = false;
    std::array<u8, 4> motion_state{};
    // A stream in progress: ticks left of its disc read and the state it then reports.
    std::array<int, 4> motion_read_left{};
    std::array<u8, 4> motion_read_state{};
    bool at_end = false;
    bool hidden = true;
  } pack_drive;
  // The overlay's init starts the pack's sequences itself (saraman: 'init' at init, 'main' / 'own' once its first motion
  // file is in): the host's own start of 'main' / 'own' at the effect start is skipped while this is set.
  bool suppress_main = false;
  // VRAM copies the overlay made this tick (on the pack's VRAM: jakotu's env texture scroll, rakugaki's tile reveal). The host applies
  // them in order to its copy of the pack VRAM (FxPack::build_vram) after the tick, then clears
  // the list; TexSrc::Vram primitives sample that copy. Rectangles in halfwords, wrapping at the VRAM edges.
  struct VramMove {
    int x = 0, y = 0, w = 0, h = 0, dx = 0, dy = 0;
    // true: the rectangle scrolls up by `dy` rows, the top rows reappearing at the bottom; dx unused.
    bool scroll = false;
  };
  std::vector<VramMove> vram_moves;
  // While set, every model part whose shading flag has bit 0x80 takes the env page (tpage
  // 0x95 = page (320, 256) 8 bit, CLUT 0x3BC0 = (0, 239)) of the effect's VRAM instead of its own tpage / CLUT; with the flag bits
  // 0x20 / 0x40 its corner UVs come from the normals. kamei (the dome of the shell form, then its own scrolling copy of `okat`).
  bool env_page = false;
  // darkfeit / lasdes3 (op 0x102 task, D = s16 @+4): every frame the CLUT row (0, 239) = the palette of the stock env page (CLUT word
  // 0x3BC0, what the units' env-mapped parts sample) is written from the saved row (stored at init) with each 5-bit channel x k >> 8 and
  // STP on every entry (| 0x8000: entry 0 turns opaque black), k = 0x100 - (t << 8) / D, 0 once t > D (the task ends, the row stays black); op 0x103
  // loads the saved row back. env_clut = k (-1 = the saved original), env_clut_set = written this tick. The host keeps the last value for the rest of the
  // stage (it is VRAM, not pack state).
  int env_clut = -1;
  bool env_clut_set = false;
  // Pad 0's newly pressed buttons this tick in the PS1 bit layout (0x20 = the confirm button; 155slot stops a reel on it).
  u16 pad_edge = 0;
  // miki / ripple (its TexSrc::Capture grid also samples it): the picture is copied to VRAM (x, y) at OT entry `ot` every frame (x < 0 = none); the battle host makes the copy and the pack
  // mesh faces on that direct-colour page sample it (BattleView::fx_draw).
  struct Capture {
    int x = -1, y = 0, ot = 0;
  } capture;
  // A camera cross-fade (lasdes2 ops 0x104 - 0x106): a second camera task plays shot `shot` of the pack's bank (`serial` changes on every
  // start; the host cuts it to key 0 then) while the battle camera keeps running. The picture is the battle camera's view, every channel
  // lowered by `fill` (0..255, subtractive), plus the second camera's view times alpha / 128 (additive). `promote`: the battle camera stops
  // and the second one takes its place (the cross-fade is over); `stop`: the second camera ends. The host consumes both requests.
  struct CrossView {
    std::string shot;
    int serial = 0;
    int alpha = 0, fill = 0;
    bool promote = false, stop = false;
  } cross;
  // A full-screen flat colour drawn over the finished picture (lasdes2 op 0x10D): mode 0 opaque (nothing while the colour is black),
  // 1..4 the semi-transparency ABR mode - 1.
  struct Tint {
    u8 rgb[3] = {0, 0, 0};
    int mode = 0;
  } tint;
  FxModel* caster(int i) { return i >= 0 && static_cast<size_t>(i) < casters.size() ? &units[static_cast<size_t>(casters[static_cast<size_t>(i)])] : nullptr; }
  FxModel* target(int i) { return i >= 0 && static_cast<size_t>(i) < targets.size() ? &units[static_cast<size_t>(targets[static_cast<size_t>(i)])] : nullptr; }
};

// ---- the battle stage ground (battle.prg's type-0x13 object) ------------------------------
// Built from the stage's drp type-0x12 'ring' mesh: its FT4 / GT4 packets become a 10 x 10 grid of 0x400 cells,
// its FT3 / GT3 packets stay triangle lists (the rim); F3 / G3 packets are dropped.
struct FxGroundCell {
  bool present = false;
  u8 uv[4][2] = {};       // corners left-top, right-top, left-bottom, right-bottom ("top" = the larger z)
  u16 clut = 0, tpage = 0;
  u8 rgb[4][3] = {};      // per corner (FT4 cells repeat the packet colour)
};
// One polygon an overlay draws with the STAGE's textures (its own pack meshes after the ground bake):
// PS1 world positions (y down), packet texel bytes + CLUT / tpage words, corner colours (textured: 0x80 = neutral; untextured: raw) with
// the mesh colour already applied, blend 0 opaque / 1 B/2+F/2 / 2 B+F / 3 B-F / 4 B+F/4, OT bias. The slot is the
// packet's average camera z >> 4 + bias (AVSZ3 / AVSZ4).
struct FxGroundPrim {
  u8 n = 3;                 // 3 or 4 (GPU Z-order quad: triangles (0,1,2) (2,1,3))
  Vec3 p[4];
  u8 uv[4][2] = {};
  u16 clut = 0, tpage = 0;
  bool textured = true;
  u8 rgb[4][3] = {};
  int blend = 0, bias = 0;
  bool two_sided = false;   // false: drawn only when NCLIP(0,1,2) > 0 like the builders
  // `p` already in camera space (an emitter mesh object the overlay draws itself), else PS1 world.
  bool camera_space = false;
  // While the fog flag is up each corner colour moves toward the far colour by that corner's own depth.
  bool depth_cue = false;
};
// A rim triangle of the ground (the ring's FT3 packets, then its GT3 packets, in file order): PS1 positions (y down),
// corner k <-> uv k <-> colour k (the packet's idx1, idx2, idx0 order).
struct FxGroundTri {
  std::array<std::array<s16, 3>, 3> v{};
  u8 uv[3][2] = {};
  u16 clut = 0, tpage = 0;
  u8 rgb[3][3] = {};
  bool gouraud = false;  // GT3 (0x34), else FT3 (0x24: one colour)
};
struct FxGround {
  bool present = false;
  // index cx + 10 * row; cell left x = -0x1400 + 0x400 cx, top z = 0x1400 - 0x400 row
  std::array<FxGroundCell, 100> cells{};
  std::vector<FxGroundTri> rim;
  u8 rgb[3] = {128, 128, 128};            // the ring object's colour (copied every frame)
  // The stage 'ring' emitter's op-0x20 COLOR record >> 4 (tekken reads it).
  u8 ring_colour[3] = {128, 128, 128};
  // 46black (on the ground in its own ground copy, on the 'near' / 'far' scenery objects): when set, every vertex of the
  // ground and of the mesh objects of the emitters named in `warp_emitters` goes through `warp` in PS1 world space (y down) before the
  // projection. `draw_bias_add` = extra OT slots of the overlay's own ground draws (46black: OT + 0x640 bytes = 400).
  std::function<void(Vec3&)> warp;
  std::vector<std::string> warp_emitters;
  // 14bar: the warp reaches only the mesh objects alive when it started, and those that have a blend mode are drawn with the quarter-add
  // mode (4) whatever their own mode is.
  bool warp_quarter_add = false;
  int draw_bias_add = 0;
  // Overlay state. `skip_stock` (the stock draw skips the ground) and `bias` (the ground's OT bias: 100, ramped by sequence ops
  // 0x6D / 0x6E, 100 again when the overlay is freed) persist; `draws` (the overlay's own ground draws with a rect: the whole ground
  // without the cells x >= r[0] && x < r[2] && z > r[3] && z <= r[1], x / z = the cell's left / top edge) and `prims` are what the
  // overlay drew in its last tick (it rebuilds both every tick).
  bool skip_stock = false;
  int bias = 100;
  std::vector<std::array<s16, 4>> draws;
  std::vector<FxGroundPrim> prims;
  const FxGroundCell* cell(int cx, int row) const {
    return cx >= 0 && cx < 10 && row >= 0 && row < 10 ? &cells[static_cast<size_t>(cx + 10 * row)] : nullptr;
  }
};
// The loaded battle stage's ground (game::StageScene registers it; null without a stage, in the tools and tests).
FxGround* fx_ground();
void set_fx_ground(FxGround* ground);

}  // namespace oc::effects
namespace oc::assets {
struct StageAsset;
struct RoomEffects;
}
namespace oc::effects {
class SequencePlayer;

struct FxContext {
  FxActor caster, target;
  // Battle host (null in the dummy stage / tests): the steerable models, the pack's emitter engine and sequence player, its type-0x02
  // meshes (+ textures) for the overlays that draw pack meshes themselves.
  FxModels* models = nullptr;
  EmitterSystem* emitters = nullptr;
  SequencePlayer* sequences = nullptr;
  const assets::StageAsset* meshes = nullptr;
  // The pack's own entries (the overlays iterate the resources of their owner only).
  const assets::RoomEffects* pack_fx = nullptr;
  // The raw pack drp (the ground overlays copy / bake its type-0x02 meshes packet by packet; empty in the dummy stage).
  ByteSpan pack_prd;
  u8 colour[3] = {255, 255, 255};  // element colour
  int stage = 0;                   // battle stage id (backdrop selection; unused by the modules)
  ViewParams view;
  u32 seed = 1;
  int duration = 0;                // ticks (the op operand); 0 = module default
  std::array<int, 8> arg{};        // overlay specific extra operands (module comments)
  bool auto_input = false;         // modules that wait for the pad (155slot) press by themselves
  FxHooks hooks;
};

// Projects a world point through the view (Pc = rot * P + trans). False when behind the camera.
bool fx_project(const ViewParams& view, const Vec3& point, float& sx, float& sy, float& depth);
int fx_slot_of_depth(float z);  // OT slot from camera depth
// Camera eye / look-at point in world space (PS1 axes) of a view, as the overlays read them (41grav).
Vec3 fx_eye(const ViewParams& view);
// libgte SquareRoot0 exactly, table lookup included: LZCS / LZCR on the argument, then the main exe's square-root table,
// so a negative argument (mido2's radius) reads the bytes before the table like the PS1 does. The host hands over the main exe image
// (cdrom prog/slps_023.64) once; without it the result is the plain integer square root (0 for value <= 0).
void set_ps1_main_exe(Bytes exe);
int ps1_square_root0(int value);
Vec3 fx_look_at(const ViewParams& view, float dist = 4096.0f);

// A dummy stage (two figures, field-ish camera) for soak / preview / tests. Mesh storage is owned by the stage.
struct DummyStage {
  FxMesh caster_mesh, target_mesh;
  FxContext ctx;
  DummyStage();
  DummyStage(const DummyStage&) = delete;
  DummyStage& operator=(const DummyStage&) = delete;
};

// ---- effect interface --------------------------------------------------------------------------------------------------------
class BespokeEffect {
 public:
  virtual ~BespokeEffect() = default;
  virtual void start(const FxContext& ctx) = 0;
  virtual void tick() = 0;
  virtual void draw(std::vector<FxPrim>& out) const = 0;
  virtual bool done() const = 0;
  // Private overlay op (0x100.. / 0x180..) with its operand words (module-specific, see each module). Returns true when the module
  // recognised the op and acted on it, false for an op it ignores.
  virtual bool command(int op, const int* args, int count) {
    (void)op;
    (void)args;
    (void)count;
    return false;
  }
  // A stock sequence op (< 0x100) as the overlay's dispatcher sees it before the stock handler: true = the overlay
  // replaced it (the deaths' 0x59), false = the stock handling follows (kamedes' 0x59 prefix falls through).
  virtual bool stock_op(int op, const int* args, int count) {
    (void)op;
    (void)args;
    (void)count;
    return false;
  }
  // The current camera (the host calls it every frame before draw(); the overlays read the live camera).
  virtual void set_view(const ViewParams& view) { ctx_.view = view; }
  const std::string& label() const { return label_; }

 protected:
  FxContext ctx_;
  std::string label_;
  u32 order_ = 0;
  friend class BespokeRegistry;
};

// ---- registry ------------------------------------------------------------------------------------------------------------------
struct BespokeInfo {
  std::string name;    // overlay stem, lower case ("canon", "46black")
  std::string family;  // "band-wipe", "line-ring", "death-dissolve", "mesh-deform"
  std::string module;  // implementing class + parameter set
  std::function<std::unique_ptr<BespokeEffect>()> make;
  // Run in battle (false: the module still re-emits a synthetic stand-in mesh and needs the actors' meshes).
  bool battle = true;
};
class BespokeRegistry {
 public:
  static BespokeRegistry& instance();  // builtin overlays registered on first use
  void add(BespokeInfo info);
  // Accepts "battle/effects/black/46black.prg", "46black" or "black" (digits stripped).
  const BespokeInfo* find(std::string_view name_or_path) const;
  const BespokeInfo* find_element(int eid) const;
  void bind_element(int eid, std::string_view overlay);
  // eid -> overlay for every slot whose .prg is registered; returns the number bound.
  size_t bind_slots(const std::vector<EffectSlot>& slots);
  std::vector<const BespokeInfo*> all() const;  // sorted by family then name
  // start() is already called on the result.
  std::unique_ptr<BespokeEffect> create(std::string_view name_or_path, const FxContext& ctx) const;

 private:
  BespokeRegistry() = default;
  std::vector<BespokeInfo> infos_;
  std::vector<std::pair<int, std::string>> elements_;
};
// Stem of an overlay path: directory and extension removed, lower case.
std::string overlay_stem(std::string_view path);
// Texture binding of a pack's type-0x0B model whose name is not the usual 4-hex-digit CLUT / page code (the overlay sets it at init
// instead: saraman sets page (0x1C0, 0x100) + CLUT row 0xEE on 'sara'): the equivalent hex name ("ee97"), or
// null when the model has no binding.
const char* fx_pack_model_binding(std::string_view model, std::string_view overlay = {});
// The pack loader's Mdl step: a type-0x0B model named 'act0' becomes pseudo slot A in every pack.
bool fx_slot_a_model(std::string_view name, int drp_type);

// ---- software raster (preview / tests) + flattening for the GPU quad batcher ----------------------------------------------------
class SoftCanvas;
void raster_prims(SoftCanvas& canvas,const std::vector<FxPrim>& sorted);
// Untextured prims for the particle batch: one quad each (tri = degenerate fourth corner, line = thin quad) with the colour PER CORNER
// (`c`, the PS1 GPU's Gouraud shading; a gradient prim must not be flattened to its mean). `cells` > 1 additionally
// cuts a gradient quad into a cells x cells grid (each cell still carries its own corner colours; only for consumers that read `r g b`).
// Blend 5 is returned as-is (needs a dedicated pipeline); callers may skip it.
struct FlatQuad {
  float x[4], y[4];  // texture order u0v0,u1v0,u0v1,u1v1
  u8 c[4][3];        // per-corner colour (same order)
  u8 r, g, b;        // the mean of `c` (flat consumers)
  int blend, slot;
};
void flatten_prims(const std::vector<FxPrim>& sorted, std::vector<FlatQuad>& out, int cells = 4);

// ---- registration of the builtin modules (called by the registry) ------------------------------------------------------------------
void register_band_wipes(BespokeRegistry& registry);
void register_line_rings(BespokeRegistry& registry);
void register_mesh_deforms(BespokeRegistry& registry);
void register_motion_tasks(BespokeRegistry& registry);
// actor_script.cpp: 19helf golem shine opthedo kamedes 41grav 43acce 22torune kamei + state-only
void register_actor_scripts(BespokeRegistry& registry);
// mesh_warp.cpp: daimou_d godhand 14bar 38super
void register_mesh_warps(BespokeRegistry& registry);
// scene_remap.cpp: seint unico bgchng miyademo + shell-only hinsi lasdes4 etrap
void register_scene_remaps(BespokeRegistry& registry);
// model_steer.cpp: ports that steer the battle models (41grav 43acce 19helf golem ...)
void register_model_steers(BespokeRegistry& registry);
// emitter_patch.cpp: icerance 27earth ...
void register_emitter_patches(BespokeRegistry& registry);
// model_deform.cpp: 44get 45infe 38super 46black (the overlay skins and re-draws the model)
void register_model_deforms(BespokeRegistry& registry);
// summon_ports.cpp: kuzira3 mido2 enrou hebi theend
void register_summon_ports(BespokeRegistry& registry);
// ground_ports.cpp, ground_ports_b.cpp, ground_ports_c.cpp (ground_base.h): the ported effects that stand on the stage ground
void register_ground_ports(BespokeRegistry& registry);
// tech_ports.cpp: shine opthedo kamedes k_monk soidash bboy shock uzumaki 3mix godhand
void register_tech_ports(BespokeRegistry& registry);
// mesh_techs.cpp: jakotu fnoise rakugaki hensin ghost2 miki (private model draws, model_draw.h)
void register_mesh_techs(BespokeRegistry& registry);
// death_ports.cpp: garadeth mushi ki_deth drg_deth octdeth daimou_d + the dissolves, lasdes1, lasdes2
void register_death_ports(BespokeRegistry& registry);
// screen_ports.cpp: muteki 155slot
void register_screen_ports(BespokeRegistry& registry);
// kamei_port.cpp: kamei (Bunyip's transformation; replaces the actor_script stand-in)
void register_kamei_port(BespokeRegistry& registry);

}  // namespace oc::effects
