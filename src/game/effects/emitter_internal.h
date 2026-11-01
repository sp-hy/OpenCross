#pragma once
// Shared internals of the emitter system: the per-header and per-object state, the wrapping integer helpers and the
// EmitterSystem::Impl declaration. The members are defined across emitter_system.cpp, emitter_spawn.cpp, emitter_tick.cpp
// and emitter_collect.cpp.

#include "game/effects/emitter_system.h"

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace oc::effects::emitter_detail {

// Wrapping 32-bit arithmetic like the MIPS core (positions of long-lived objects may overflow).
inline s32 add32(s32 lhs, s32 rhs) { return static_cast<s32>(static_cast<u32>(lhs) + static_cast<u32>(rhs)); }
inline s32 shl32(s32 value, int bits) { return static_cast<s32>(static_cast<u32>(value) << bits); }
inline s16 add16(s16 value, int delta) { return static_cast<s16>(static_cast<u16>(static_cast<u16>(value) + static_cast<u16>(delta))); }
inline s16 trunc16(int value) { return static_cast<s16>(static_cast<u16>(static_cast<u32>(value) & 0xFFFFu)); }

constexpr u32 kFlagFloor = 1u << 4;
constexpr u32 kFlagInheritAngle = 1u << 8, kFlagInheritVel = 1u << 9, kFlagInheritScale = 1u << 10, kFlagInheritColor = 1u << 11;
constexpr u32 kFlagFaceVelMask = 3u << 13;
// With bit 15, multiply the actor's attach joint rotation into the matrix.
constexpr u32 kFlagJointRot = 1u << 7;
constexpr u32 kFlagRelParent = 1u << 15, kFlagZyx = 1u << 16, kFlagFixedSlot = 1u << 23, kFlagCamRel = 1u << 24;
constexpr u32 kFlagViewSpace = 1u << 25, kFlagWorldQuad = 1u << 26, kFlagNoDeathSpawn = 1u << 27, kFlagScreenTest = 1u << 28, kFlagExt = 1u << 29;

// Nested spawn limit (death / child chains).
constexpr int kMaxDepth = 6;

struct Header {
  const assets::Emitter* def = nullptr;
  // A per-target clone header is named "sado"; the original keeps its name.
  std::string alias;
  const std::string& nm() const { return alias.empty() ? def->name : alias; }
  bool linked = false, stopped = false, lens_hidden = false;
  s16 frame = 0, countdown = 0;
  // Anchor id patched by field op 9B.
  int anchor = -1;
  bool big = false, huge = false;
  bool has_anchor_op = false;
  bool has_basis_op = false;
  std::array<u8, 4> basis_sel{};
  u16 basis_flags = 0;
  // battle effect overlay patches (EmitterSystem::patch_op / set_object_hook / set_draw_suppressed)
  bool draw_off = false;
  // An op-0x48 callback replaced the draw and never calls the stock draw: the objects are not drawn.
  bool cb_no_draw = false;
  // op -> operands (EmitterSystem::patch_op).
  std::map<int, std::array<int, 4>> patch;
  const std::array<int, 4>* patched(int op) const {
    auto it = patch.find(op);
    return it == patch.end() ? nullptr : &it->second;
  }
  std::function<void(EmitterSystem::ObjectRef&)> hook;
  bool hook_after = false;
};

struct Object {
  s32 pos[3]{}, vel[3]{}, acc[3]{};
  s32 ppos[3]{};
  // Spawn-tick display offset (anchor entity's step), applied once by the first update().
  s32 born_shift[3]{};
  s16 scale[3]{4096, 4096, 4096}, srate[3]{}, ang[3]{}, arate[3]{};
  s16 pscale[3]{4096, 4096, 4096}, pang[3]{};
  u16 col[3]{0x800, 0x800, 0x800}, crate[3]{}, pcol[3]{0x800, 0x800, 0x800};
  u8 anim_ctr = 0, anim_per = 0;
  s16 frame = 0;
  u16 life = 0;
  s16 bias = 0, drag = 0;
  u16 floor_y = 0;
  u8 rest = 0;
  u32 flags = 0;
  const assets::SpriteSheet* sheet = nullptr;
  s16 stype = 0;
  // Sprite type 14 / 19 / 20: name of the type-0x02 / 0x12 / 0x0B record (points into the pack's op list).
  const std::string* mesh_name = nullptr;
  // Sprite type 15: 1 'flat', 2 'line' (other names: 'scro' / 'sun0', not drawn).
  u8 builtin = 0;
  // Sprite type 20: motion keyframe counter (advances ticks * motion_rate / 256).
  float motion = 0, motion_prev = 0;
  // 64 = 0.25 keyframe per tick (the engine's 15 fps default); op 49 sets it.
  u16 motion_rate = 64;
  // Sprite type 20: motion set number given by op 49; -1 = none.
  s16 model_clip = -1;
  // Op 49: the motion wraps, else it holds its last keyframe.
  bool model_loop = false;
  int owner = -1, death = -1, periodic = -1;
  Mat3 mat;
  // Sprite type 17 (a light): the GTE light slot and the sprite record name.
  s8 light_slot = -1;
  const std::string* light_name = nullptr;
  // Extended object block.
  u32 ext = 0;
  s32 uv_phase[2]{}, uv_rate[2]{}, uv_phase_prev[2]{};
  s16 morph_phase = 0, morph_rate = 0, morph_phase_prev = 0;
  // Op 37 target mesh (points into the pack's op list).
  const std::string* morph_name = nullptr;
  s16 sub_s[3]{}, acc_s[3]{}, sub_a[3]{}, acc_a[3]{};
  const Curve* path = nullptr;
  Basis path_basis;
  std::array<u8, 4> path_sel{};
  bool path_live = false;
  s16 path_mode = 0;
  u16 path_l0 = 0;
  // 0 = A, 1 = B.
  int slot_reg = -1;
  bool alive = true;
  // EmitterSystem::mark_mesh_objects().
  bool marked = false;
  // >= 0: an overlay replaced the draw callback and draws the object itself (fire_at / collect_tagged).
  int poly_tag = -1;
};

// ApplyMatrix on an integer vector with a float matrix (matrix is 4.12 on the PS1; truncation like the GTE).
inline void rot_int(const Basis& basis, int vec[3], int out[3]) {
  const Vec3 rotated = basis.m.apply({static_cast<float>(vec[0]), static_cast<float>(vec[1]), static_cast<float>(vec[2])});
  for (int i = 0; i < 3; ++i) out[i] = static_cast<int>(rotated[static_cast<size_t>(i)]);
}

inline s32 osc_value(const assets::OscTerm& term, s16 frame) {
  if (term.freq == 0) return 0;
  const int sine = rsin(((static_cast<u16>(frame)) & 0xFFF) * static_cast<int>(term.freq) + static_cast<int>(term.phase));
  return static_cast<s32>(static_cast<s64>(sine) * term.amp * 16);
}

}  // namespace oc::effects::emitter_detail

namespace oc::effects {

using namespace emitter_detail;

struct EmitterSystem::Impl {
  // begin_overlay_tick / end_overlay_tick: the fire sites already used in the current pacing window
  bool pace_on = false;
  std::set<std::pair<std::string, int>> pace_sites;
  std::map<std::string, int> pace_ord;
  const EffectBank& bank;
  Config cfg;
  Rng rng;
  ActorHooks hooks;
  // Set by set_lights().
  LightTable* lights = nullptr;
  std::vector<Header> headers;
  std::vector<Object> objs;
  EmitterStats st;
  u64 tick_count = 0;
  // Last op-08 position (16.16) of a spawn.
  s32 origin[3]{};
  int depth = 0;
  // Op 45.
  int pending_child = -1;
  // objs index of the particle registered as slot A / B.
  size_t slot_idx[2]{~size_t{0}, ~size_t{0}};

  // OC_FX_ONLY=<emitter names, comma-separated>: only those emitters' objects are drawn.
  std::string dev_only;

  bool pace_blocks(std::string_view name);
  Impl(const EffectBank& effect_bank, Config config);
  bool dev_skip(const Object& obj) const;

  // -- registry
  int find_header(std::string_view name) const;
  void register_all();
  bool start_index(size_t i);
  void kill_objects(int owner);

  // -- basis helpers
  Basis actor_basis(int sel_a, int sel_b, int sel_c, int sel_d, int& scale, bool& found) const;
  Basis heading_basis(const Object& obj) const;

  // -- random helpers
  int rand16() { return rng.next() & 0xFFFF; }

  // -- spawn
  int spawn(size_t hi, const Basis* parent);
  static void build_matrix(Object& obj);
  static void integrate_step(Object& obj, int shift);
  int fire(size_t hi, const Object* parent_obj);
  void die(size_t idx, bool spawn_children);

  // -- header stepper
  void step_header(size_t hi);
  void replay(Object& obj, Header& header);
  void update(size_t idx);
  void ext_step(Object& obj);
  void periodic_spawn(size_t idx);
  bool anchor_at(int id, float alpha, Basis& basis) const;
  void tick();
  EmitterSystem::ObjectRef ref(Object& obj);

  // -- quads
  void collect_meshes(const ViewParams& view, float alpha, std::vector<MeshInstance>& out, bool with_suppressed) const;
  void collect_tagged(const ViewParams& view, float alpha, std::vector<EmitterSystem::TaggedInstance>& out) const;
  void collect_builtin(const Object& obj, const ViewParams& view, float alpha, u32& order, std::vector<DrawQuad>& out) const;
  void collect_flare(const Object& obj, const ViewParams& view, float alpha, u32& order, std::vector<DrawQuad>& out) const;
  void collect(const ViewParams& view, float alpha, std::vector<DrawQuad>& out) const;
};

}  // namespace oc::effects
