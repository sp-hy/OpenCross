#pragma once
// Generic effect player, part 1: the emitter system. Instantiates 0x0A emitters from parsed room / battle eins data and runs
// them exactly as the PS1 engine does: per-tick header stepper, spawner (incl. the extended ops), per-object update, and turns
// the live objects into screen quads with the OT-slot depth rule and the blend modes. No GPU / audio dependency: render glue is game/effects/gpu_effects.h and the
// CPU rasteriser soft_raster.h.
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/room_effects.h"
#include "assets/room_scene.h"
#include "game/effects/fx_math.h"

namespace oc::effects {

// The lens flare's full-screen flash channel: colour = the object's colour byte (col >> 4), (sx, sy) = the light's projected position
// in the 320 x 224 buffer. falloff = 0x25400 - (dx^2 + dy^2) with dx = 320 - 2 sx, dy = 224 - 2 sy; channel = falloff / (0x25400 / colour)
// (integer steps), so it is colour with the light at the centre and falls to 0 at the corners (clamped to 0 beyond them, which only the
// widescreen columns can reach).
inline int lens_flash_channel(int colour, int sx, int sy) {
  if (colour <= 0) return 0;
  const int dx = 0x140 - 2 * sx, dy = 0xE0 - 2 * sy;
  const int falloff = 0x25400 - (dx * dx + dy * dy);
  const int level = falloff / (0x25400 / colour);
  return level < 0 ? 0 : level > 255 ? 255 : level;
}

// The wash only exists while the light is inside the PS1's own 320 x 224 frame (the PS1 callback draws nothing otherwise); (sx, sy) = the light
// projected with the PS1 focal length (h / fov_scale). Outside the frame the level is 0 however wide our picture is.
inline int lens_wash_channel(int colour, int sx, int sy) {
  if (sx < 0 || sy < 0 || sx > 0x140 || sy > 0xE0) return 0;
  return lens_flash_channel(colour, sx, sy);
}

// The ghost sprites follow the light in OUR frame (user focal length): visible while it is inside the drawn picture, i.e. the 320 x 224 buffer
// plus `extra` widescreen columns on each side (0 in 4:3).
inline bool lens_ghosts_visible(int sx, int sy, int extra) { return sy >= 0 && sy <= 0xE0 && sx >= -extra && sx <= 0x140 + extra; }

// ---- type 0x18 path curve --------------------------------------------------------------------
struct Curve {
  struct Key {
    s16 x = 0, y = 0, z = 0;
    u16 time = 0;
  };
  // Basis selectors a, b, c, d.
  std::array<u8, 4> basis_sel{};
  // bit0 = rebuild the basis from the live actor every tick.
  u32 flags = 0;
  std::vector<Key> keys;
  bool valid = false;
  // time: 0..4096. Linear between the bracketing keys, extrapolates before the first, clamps after the last.
  Vec3 sample(int time) const;
  static Curve parse(ByteSpan data);
};

// ---- lookup over the loaded packs (room first, then the shared particle library) ---------------------------------------
class EffectBank {
 public:
  void add(const assets::RoomEffects* pack) {
    if (pack) packs_.push_back(pack);
  }
  // A resident library (battle.prd in battle): searched like any pack, but its emitters are registered stopped (they run only when a
  // sequence or an overlay starts them, not when the effect registers its own pack).
  void add_library(const assets::RoomEffects* pack) {
    if (!pack) return;
    packs_.push_back(pack);
    library_.resize(packs_.size(), false);
    library_.back() = true;
  }
  bool is_library(size_t i) const { return i < library_.size() && library_[i]; }
  size_t pack_count() const { return packs_.size(); }
  const assets::RoomEffects& pack(size_t i) const { return *packs_[i]; }
  const assets::SpriteSheet* sheet(std::string_view name) const;
  const assets::Sequence* sequence(std::string_view name) const;
  const assets::EffectBlob* blob(u8 type, std::string_view name) const;
  // Cached parse of the type-0x18 blob.
  const Curve* curve(std::string_view name) const;

 private:
  std::vector<const assets::RoomEffects*> packs_;
  std::vector<bool> library_;
  mutable std::map<std::string, Curve, std::less<>> curves_;
};

// ---- camera / projection -----------------------------------------------------------------------------------------------
// Pc = rot * P + trans; screen = (ox + h * x / z + scroll_x, oy + h * y / z + scroll_y), the field camera.
struct ViewParams {
  Mat3 rot;
  Vec3 trans{0, 0, 0};
  float h = 512.0f;
  float ox = 160.0f, oy = 120.0f;
  float scroll_x = 0, scroll_y = 0;
  // GTE depth cue (when the fog flag is on): objects with flag bit 17 have their colour moved toward the far colour (bytes) by
  // p(z) = far * (z - near) / ((far - near) * z) of their view depth.
  struct DepthCue {
    bool on = false;
    float near = 0, far = 0;
    float rgb[3] = {0, 0, 0};
  } cue;
  static ViewParams from_room(const assets::RoomCamera& camera);
};

// One drawn sprite quad. Corners are in the order of the texture's corners (u0v0, u1v0, u0v1, u1v1): the orientation flag of
// the sprite is already folded into the screen positions, so rasterisers sample the unmirrored RGBA.
struct DrawQuad {
  const assets::EffectSprite* sprite = nullptr;
  const assets::SpriteSheet* sheet = nullptr;
  // frame: frame table index (0-based); sprite_index: sprite inside it.
  int frame = 0, sprite_index = 0;
  // Layer space (scroll included).
  float x[4]{}, y[4]{};
  // Modulation colour, 0x80 = neutral.
  u8 r = 128, g = 128, b = 128;
  // untextured == 2 only: the colour at the far end of the line (corners 1 / 3); r g b is the near end (0 / 2).
  u8 r1 = 0, g1 = 0, b1 = 0;
  // GPU blend: 0 opaque, 1 B/2+F/2, 2 B+F, 3 B-F, 4 B+F/4.
  int blend = 0;
  // OT slot 0..0xFFF (larger = farther); depth value = slot / 8192.
  int slot = 0;
  // Submission order (later submitted = drawn first inside one slot).
  u32 order = 0;
  // Camera-space centre of the owning object (sheet sprites; SpellLights un-projects it, light_extract.h).
  float cam[3]{};
  // Built-in sprite type 15 (no sheet): 1 = 'flat' (POLY_F4 of the object colour), 2 = 'line' (a segment of a Gouraud line
  // whose end is black: one thin quad Gouraud-shaded from r g b to r1 g1 b1). `sprite` is null for both.
  u8 untextured = 0;
};
// Sorts into draw order: farthest slot first, and inside a slot the later-submitted quad first.
void sort_draw_order(std::vector<DrawQuad>& quads);

// ---- light slots (sprite type 17 = a light) ------------------------------------------
// The three GTE light slots: every live type-17 object writes its slot each tick (position, colour bytes, intensity = scale x,
// falloff = angle x), sequence op 0x66 zeroes the three intensities, op 0x65 ramps the back colour. The lit model draw reads them.
// PS1 world units, y down. The table outlives the effect: a slot keeps its last light after the object dies (only an intensity 0
// frees it for a new light).
struct LightTable {
  // pos: world position (the s16 high halves of the object's 16.16 position); rgb: colour bytes (colour accumulators >> 4);
  // intensity: object scale x (4096 = 1.0); falloff: object angle x.
  struct Slot {
    s32 pos[3]{};
    u8 rgb[3]{};
    s32 intensity = 0;
    s32 falloff = 0;
  };
  std::array<Slot, 3> slot{};
};
// The GTE light matrix rows and colour matrix columns for a model at `pos` (PS1 world units, the unit's
// attach point 9): row i = normalise(light_i - pos) * k_i / 4096 with k_i = intensity_i - (|light_i - pos| * falloff_i >> 12) clamped to
// 0..0x7FFF (the falloff is << 3 in the field); column i = colour bytes << 4 (4.12), returned as bytes / 256.
// dir: row i = light i direction * k_i / 4096 (PS1 axes); rgb: light i colour / 256.
struct LightMatrices {
  float dir[3][3]{};
  float rgb[3][3]{};
};
LightMatrices light_matrices(const LightTable& lights, const Vec3& pos, bool field = false);

// ---- game-side callbacks ------------------------------------------------------------------------------------------------
struct ActorHooks {
  // Basis of actor selector `a`, joint `b` (and 2nd point c,d for look-at): op 39 / path nodes.
  // `scale` receives the actor scale (4096 = 1.0). Unset / false -> identity at the origin.
  std::function<bool(int a, int b, int c, int d, Basis& out, int& scale)> actor_basis;
  // Frame of the entity an emitter was attached to (field op 9B, op 0x30): world translation + rotation.
  std::function<bool(int anchor_id, Basis& out)> anchor_basis;
  // Same at a draw with the render interpolation factor (0..1 between the last two ticks); unset: anchor_basis is used.
  std::function<bool(int anchor_id, float alpha, Basis& out)> anchor_basis_at;
  // Sprite type 0x16 (play AKAO by pointer; unused in the shipped data).
  std::function<void(std::string_view name)> akao;
};

struct EmitterStats {
  // dropped = spawn refused at the object cap.
  u64 spawned = 0, died = 0, dropped = 0;
  u32 live = 0, headers = 0, running = 0;
  u32 unresolved_sprites = 0, mesh_objects = 0;
  // Objects spawned per op-05 sprite type (index min(type, 31); 0 = no op 05).
  std::array<u32, 32> type_spawns{};
  // Header starts (start / start_for_target / attach).
  u32 starts = 0;
};

class EmitterSystem {
 public:
  struct Config {
    u32 seed = 1;
    size_t max_objects = 4096;
    // The engine's position shift: 3 in the field and the title, 5 in battle. Used by the integrator (pos += vel << S, op 0x38),
    // the floor (floor << S, flag bit 4), the op-0x27 radius (<< S), the path ride (op 0x3E: (target - pos) >> S), the camera-relative z
    // (flag bit 24: << S), the sprite-sheet quad corners (sheet x / y / w / h << S) and the type-15 'line'
    // trail (-vel >> (12 - S)). In battle every sprite quad is therefore 4x the field size for the same scale, velocities move 4x and the
    // op-0x27 spread is 4x.
    int pos_shift = 3;
    // Dev: honour OC_FX_ONLY=<emitter names> (only those emitters' objects are drawn) on this instance (the battle effect player sets it;
    // the stage scene's own emitters are never filtered).
    bool dev_only_filter = false;
    // Sequence op 0x55 (stop_expire) is ignored. The title sea sets it: the kill sequences would switch the
    // cave and reef scenery sets off per shot, and the whole sea should stay visible (free camera, widescreen, high FOV).
    bool keep_on_stop_expire = false;
  };
  explicit EmitterSystem(const EffectBank& bank) : EmitterSystem(bank, Config{}) {}
  EmitterSystem(const EffectBank& bank, Config cfg);
  ~EmitterSystem();
  EmitterSystem(const EmitterSystem&) = delete;
  EmitterSystem& operator=(const EmitterSystem&) = delete;

  void set_hooks(ActorHooks hooks);
  // The GTE light slots the type-17 objects write (null = none: the objects still take a slot number and live out their life). The
  // table belongs to the caller (the battle view: it outlives the effect).
  void set_lights(LightTable* lights);

  // Registers every emitter of every pack: emitters without flag bit2 run at once,
  // bit2 ones wait for start(). Safe to call once per bank.
  void register_all();
  // Header index or -1.
  int find(std::string_view name) const;
  size_t header_count() const;
  const assets::Emitter& header_def(size_t i) const;
  bool header_running(size_t i) const;

  // Field script / sequence entry points. Names: 4 chars; "0000" = all emitters for stop and kill (start looks it up as a name).
  // start: field ACTIVATE_NAMED_OBJ, sequence op 22.
  bool start(std::string_view name);
  // Sequence op 22 of a task serving a target: target > 0 starts a clone whose op-39 selectors 1 / 2 read 9 / 0xD + target.
  bool start_for_target(std::string_view name, int target);
  // Both parameters: selectors 1 -> 9 + caster, 2 -> 0xD + target. A group cast's
  // charge sequence is cloned once per extra caster, so its emitters follow that caster.
  bool start_for_actor(std::string_view name, int caster, int target);
  // Battle stage layers (sequence ops 0x26 / 0x27 / 0x36-0x3D 'near' / 'far' / 'ring' / 'roof', 0x5F / 0x60 'bti0'): every emitter of the
  // name. layer_off kills its objects and stops it; layer_on restarts the ones with
  // duration 0 (or all with `any_duration`): relinked, and restarted from frame 0 when header
  // flag bit 10 is set. Returns the number of emitters touched.
  int layer_off(std::string_view name);
  int layer_on(std::string_view name, bool any_duration = false);
  // The stage's lens-flare emitters (sprite type 21): off while an element pack runs, respawned at its end.
  int lens_off();
  int lens_on();
  // Stop emitting; live objects finish.
  bool stop(std::string_view name);
  // Stop and kill every live object.
  bool kill(std::string_view name);
  // Sequence op 0x55: stop the named headers, then every live object of theirs with life != 0 gets life := 1 (it dies on the
  // next tick, death child included) and a persistent one (life 0) is freed.
  bool stop_expire(std::string_view name);
  // Patch the op-0x30 anchor, then start.
  bool attach(std::string_view name, int anchor_id);
  // Anim-sequence op 1D: EVERY header named `name` gets the anchor (its op 0x30) and is restarted.
  // Returns the number of headers; `duration` = the first one's header +6, 0 if none.
  int attach_all(std::string_view name, int anchor_id, int& duration);
  bool start_index(size_t i);

  // One 60 Hz tick: header stepper for every linked emitter, then the update of every live object (new ones included).
  void tick();
  // The field actor wake / footprint spawner: one firing of emitter `name` (whatever its header state), then the object is moved to the world point (x, y, z) with angle Z = -angle. False if the name
  // is unknown or the object cap refused it. `normal` (optional, debug logs) receives the unit normal of the sprite plane of the
  // object's rotation (RotMatrix of its angles, before the camera): (0, +-1, 0) = lying flat on the ground / water.
  bool spawn_at(std::string_view name, int x, int y, int z, int angle, Vec3* normal = nullptr);

  // Live objects -> quads at interpolation factor alpha (0 = previous tick, 1 = current). Appends to `out`.
  void collect_quads(const ViewParams& view, float alpha, std::vector<DrawQuad>& out) const;

  // Live mesh sprites (sprite type 14, battle effects). `centre` / `mat` already include the view: camera-space = centre + mat * v with v
  // the mesh vertex in PS1 file space (x, y down, z). The renderer projects with ViewParams::h / ox / oy and orders by OT slot.
  struct MeshInstance {
    const std::string* name = nullptr;
    Vec3 centre{0, 0, 0};
    Mat3 mat;
    u8 rgb[3] = {128, 128, 128};
    int blend = 0, bias = 0, frame = 0;
    // Object flags (op 06; bit 17 = depth-cued colour).
    u32 flags = 0;
    bool fixed_slot = false;
    // Extended flag word.
    u32 ext = 0;
    // Object flag bit 29 (op 35): drawn by the extended mesh renderer (ABR = blend, not blend - 1).
    bool ext_path = false;
    // Op 36 UV scroll phase the draw adds to the packet texel: the object's 24.8 counter at the tick alpha, in texels wrapped to 0..128.
    std::array<float, 2> uv_phase{};
    // Op 36 texture window (FxUvScroll of the owning header) when ext bit 0 is set: {off_u, off_v, win_w, win_h} in texels, a width / height of 0 =
    // wrap inside the 256-texel page. Filled by collect_meshes so every mesh drawer (battle stage scene, field room meshes) reads one rule.
    bool uv_scroll = false;
    std::array<int, 4> uv_window{};
    // Header index of the emitter that spawned the object (header_def()).
    int owner = -1;
    // mark_mesh_objects().
    bool marked = false;
    // Op 05 type: 14 = type-0x02 mesh, 19 = the battle stage ground (type 0x12 `ring`), 20 = type-0x0B model.
    int sprite_type = 14;
    // Sprite type 20 (the model follows the particle): world position / object matrix without the view, and the motion
    // frame (keyframes, interpolated between `motion_prev` and `motion` with the tick alpha).
    Vec3 world_pos{0, 0, 0};
    Mat3 world_mat;
    float motion = 0, motion_prev = 0;
    // clip: motion set started by op 49 (>= 0x80 = the model's extension table); -1 = none set.
    // loop: the motion wraps, else it stops on its last keyframe.
    int clip = -1;
    bool loop = false;
    // Op 37 mesh morph: the object is drawn with its vertices lerped toward the named target mesh by
    // `morph_t` (0..1; `morph_t_prev` is the previous tick's value for interpolation). nullptr = no morph.
    const std::string* morph_name = nullptr;
    float morph_t = 0, morph_t_prev = 0;
    // The object screen test of flag bit 28 on a type-0x02 mesh: the six points (assets::StagePart::cull_points) go through
    // the object transform and the projection with the GTE's limits (view x / y saturate at +-0x7FFF, depth at 0..0xFFFF, a
    // depth up to h / 2 or behind the eye projects at the largest scale ~2, screen x / y saturate at -1024..1023); the object
    // is not drawn when no point has screen x in 1..351 or screen y in -31..223 (the x limits widen with the widescreen columns like
    // render::cull_frame). Every other type tests its origin instead (off_screen(view)). False without the flag and under the free camera.
    bool off_screen(const std::array<std::array<s16, 3>, 6>& points, const ViewParams& view) const;
    // The flag bit 28 test of the other types (ground, models): the object origin is projected and the object is not drawn when the
    // projection reports a GTE error (view x / y or depth saturated, a depth up to h / 2 or behind the eye, screen x / y outside
    // -1024..1023). False for a type-0x02 mesh, without the flag and under the free camera.
    bool off_screen(const ViewParams& view) const;
  };
  // `with_suppressed`: also the objects of emitters an overlay draws itself (set_draw_suppressed), for that overlay's own draw.
  void collect_meshes(const ViewParams& view, float alpha, std::vector<MeshInstance>& out, bool with_suppressed = false) const;

  // Pseudo-actor slots A / B registered by ops 3F / 40: world position of the registering particle.
  bool slot_position(int slot, Vec3& out) const;
  // Same with the registering particle's velocity (the camera anchor types 8 / 12 take their yaw from it).
  bool slot_object(int slot, Vec3& pos, Vec3& vel) const;

  // Read-only view of a live object (tests / debug overlays). Index < live_objects().
  struct ObjectView {
    // World units (position) / raw 16.16 velocity and acceleration divided by 65536.
    Vec3 pos{}, vel{}, acc{};
    int life = 0, frame = 0, bias = 0;
    int scale[3]{}, scale_rate[3]{}, angle[3]{}, color[3]{};
    u32 flags = 0;
    int owner = -1;
    // The owning header's name (header_def(owner).name).
    const std::string* emitter = nullptr;
    // The op-05 record name (sheet / mesh / model / light), null when unresolved.
    const std::string* sprite = nullptr;
    // The op-05 sprite type.
    int stype = 0;
    // Draw colour bytes (colour accumulators >> 4).
    u8 rgb[3]{};
    // Sprite type 20 only: the motion set op 49 started (-1 = none), the
    // keyframe position and the loop flag.
    int clip = -1;
    float motion = 0;
    bool loop = false;
  };
  bool object_view(size_t i, ObjectView& out) const;

  // ---- battle effect overlay hooks (the bespoke overlays patch emitters and steer their objects) ----------------
  // Overwrite the op-08 spawn position of the first emitter named `name` (s16 world units). False if unknown.
  bool set_position(std::string_view name, int x, int y, int z);
  // Replace the operands of op `op` of header `header` for every later spawn. Supported:
  // 0x08 POS / 0x0A VEL / 0x19 ANGLE (x, y, z as the file's s16 values) and 0x39 ORIENT_BASIS (selector bytes a, b, c, d).
  bool patch_op(size_t header, int op, const std::array<int, 4>& v);
  // The operands op `op` of header `header` has now (patched or from the file); false if the emitter has no such op.
  bool op_value(size_t header, int op, std::array<int, 4>& v) const;
  // Mutable access to one live object (pos / vel / acc in the engine's 16.16, angles 4096 = turn, scale 4096 = 1, colour 8.8).
  struct ObjectRef {
    s32* pos = nullptr;
    s32* vel = nullptr;
    s32* acc = nullptr;
    s16* scale = nullptr;
    s16* ang = nullptr;
    u16* col = nullptr;
    u16* life = nullptr;
    u32* flags = nullptr;
    // Header index.
    int owner = -1;
    // First actor selector of the emitter's op 0x39 (an overlay's per-target clone carries the target's own).
    int basis_sel = 0;
    // The engine's position shift (3 field / title, 5 battle): hook bodies that re-do the integration must use it.
    int shift = 3;
  };
  // `fn` runs on every live object of emitter `name` each tick before the object's own step (null removes it);
  // `after_step` = after it instead (a replaced draw callback / an op-0x48 callback runs after the tick's integration).
  // The hook must not spawn objects (start emitters instead).
  bool set_object_hook(std::string_view name, std::function<void(ObjectRef&)> fn, bool after_step = false);
  void for_each_object(std::string_view name, const std::function<void(ObjectRef&)>& fn);
  // Marks the live type-14 mesh objects of emitter `name` (MeshInstance::marked); objects spawned later stay unmarked. `on` = false clears them.
  void mark_mesh_objects(std::string_view name, bool on);
  // The overlay draws the mesh objects of emitter `name` itself, collect_meshes() leaves them out.
  bool set_draw_suppressed(std::string_view name, bool on);
  // Op 0x48: the record's callback REPLACES the object's draw function. A callback that
  // does not call the stock draw itself leaves the sprite undrawn; clones (`sado`, copied headers) inherit it.
  bool set_draw_dropped(std::string_view name, bool on);
  // One firing of emitter `name` now (whatever its header state), at its own op-08 position. False if unknown.
  bool fire(std::string_view name);
  // Fires, then sets the primary object's position := (x, y, z) << 16 (the death overlays fire 'kie0' / 'kie1' at face centroids).
  // `tag` >= 0: the overlay replaced the object's draw callback (the F-B shard clones): collect_quads / collect_meshes leave it out
  // and collect_tagged lists it with its transform. False if the name is unknown or the object cap refused it.
  // `init` (optional) edits the new object (lasdes3 gives its shards a velocity).
  bool fire_at(std::string_view name, int x, int y, int z, int tag = -1, const std::function<void(ObjectRef&)>& init = {});
  // Overlay pacing: the PS1 main loop calls an overlay's tick ONCE per loop iteration, and the iteration
  // lasts dt vsyncs (the vsyncs since the last frame, 1..10). The engine's own loops (emitters, sequences, objects)
  // run dt sub-steps, so their counts do not depend on dt; an overlay's per-call fire does
  // (e.g. an overlay that fires 'kem0' on every call after a piece landed). The host calls begin_overlay_tick(new_window) around the
  // module's tick: with a window of dt ticks, each fire site (emitter name + ordinal within the tick) fires at most once per window.
  void begin_overlay_tick(bool new_window);
  void end_overlay_tick();
  struct TaggedInstance {
    int tag = -1;
    // Camera space (as MeshInstance): camera point = centre + mat * v, v in the object's space.
    Vec3 centre{0, 0, 0};
    Mat3 mat;
    // Colour accumulators >> 4.
    u8 rgb[3] = {128, 128, 128};
    int bias = 0;
    u32 flags = 0;
  };
  void collect_tagged(const ViewParams& view, float alpha, std::vector<TaggedInstance>& out) const;

  EmitterStats stats() const;
  size_t live_objects() const;
  u64 ticks() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::effects
