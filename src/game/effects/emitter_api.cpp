#include "game/effects/emitter_internal.h"

#include <cstdio>
#include <cstdlib>

namespace oc::effects {

using namespace emitter_detail;

// ---- public API ---------------------------------------------------------------------------------------------------------------

EmitterSystem::EmitterSystem(const EffectBank& bank, Config cfg) : impl_(std::make_unique<Impl>(bank, cfg)) {}
EmitterSystem::~EmitterSystem() = default;

void EmitterSystem::set_hooks(ActorHooks hooks) { impl_->hooks = std::move(hooks); }
void EmitterSystem::set_lights(LightTable* lights) { impl_->lights = lights; }
void EmitterSystem::register_all() { impl_->register_all(); }
int EmitterSystem::find(std::string_view name) const { return impl_->find_header(name); }
bool EmitterSystem::set_position(std::string_view name, int x, int y, int z) {
  const int i = impl_->find_header(name);
  return i >= 0 && patch_op(static_cast<size_t>(i), 0x08, {x, y, z, 0});
}
bool EmitterSystem::patch_op(size_t header_index, int op, const std::array<int, 4>& values) {
  if (header_index >= impl_->headers.size() || (op != 0x08 && op != 0x0A && op != 0x19 && op != 0x39)) return false;
  Header& header = impl_->headers[header_index];
  header.patch[op] = values;
  if (op == 0x39 && header.has_basis_op)
    for (size_t k = 0; k < 4; ++k) header.basis_sel[k] = static_cast<u8>(values[k]);
  return true;
}
bool EmitterSystem::op_value(size_t header_index, int op, std::array<int, 4>& values) const {
  if (header_index >= impl_->headers.size()) return false;
  const Header& header = impl_->headers[header_index];
  if (const auto* pv = header.patched(op)) {
    values = *pv;
    return true;
  }
  for (const auto& entry : header.def->ops) {
    if (entry.op != op) continue;
    if (const auto* xyz = std::get_if<assets::FxXyz>(&entry.arg)) {
      values = {xyz->x, xyz->y, xyz->z, 0};
      return true;
    }
    if (const auto* basis_arg = std::get_if<assets::FxBasis>(&entry.arg)) {
      values = {basis_arg->a, basis_arg->b, basis_arg->c, basis_arg->d};
      return true;
    }
    return false;
  }
  return false;
}
bool EmitterSystem::set_object_hook(std::string_view name, std::function<void(ObjectRef&)> hook, bool after_step) {
  bool any = false;
  for (auto& header : impl_->headers)
    if (header.nm() == name) header.hook = hook, header.hook_after = after_step, any = true;
  return any;
}
void EmitterSystem::for_each_object(std::string_view name, const std::function<void(ObjectRef&)>& visit) {
  for (auto& obj : impl_->objs) {
    if (!obj.alive || obj.owner < 0 || static_cast<size_t>(obj.owner) >= impl_->headers.size()) continue;
    if (impl_->headers[static_cast<size_t>(obj.owner)].nm() != name) continue;
    ObjectRef object_ref = impl_->ref(obj);
    visit(object_ref);
  }
}
void EmitterSystem::mark_mesh_objects(std::string_view name, bool on) {
  for (auto& obj : impl_->objs) {
    if (!obj.alive || obj.owner < 0 || static_cast<size_t>(obj.owner) >= impl_->headers.size()) continue;
    if (impl_->headers[static_cast<size_t>(obj.owner)].nm() == name && obj.stype == 14) obj.marked = on;
  }
}
void EmitterSystem::begin_overlay_tick(bool new_window) {
  if (new_window) impl_->pace_sites.clear();
  impl_->pace_ord.clear();
  impl_->pace_on = true;
}
void EmitterSystem::end_overlay_tick() { impl_->pace_on = false; }
bool EmitterSystem::fire(std::string_view name) {
  // Already fired in this window (see begin_overlay_tick).
  if (impl_->pace_blocks(name)) return true;
  const int hi = impl_->find_header(name);
  return hi >= 0 && impl_->fire(static_cast<size_t>(hi), nullptr) >= 0;
}
bool EmitterSystem::fire_at(std::string_view name, int x, int y, int z, int tag, const std::function<void(ObjectRef&)>& init) {
  const int hi = impl_->find_header(name);
  if (hi < 0) return false;
  if (impl_->pace_blocks(name)) return true;
  const int oi = impl_->fire(static_cast<size_t>(hi), nullptr);
  if (oi < 0) return false;
  Object& obj = impl_->objs[static_cast<size_t>(oi)];
  obj.pos[0] = obj.ppos[0] = static_cast<s32>(static_cast<u32>(x) << 16);
  obj.pos[1] = obj.ppos[1] = static_cast<s32>(static_cast<u32>(y) << 16);
  obj.pos[2] = obj.ppos[2] = static_cast<s32>(static_cast<u32>(z) << 16);
  obj.poly_tag = tag;
  if (init) {
    ObjectRef object_ref = impl_->ref(obj);
    init(object_ref);
  }
  return true;
}
void EmitterSystem::collect_tagged(const ViewParams& view, float alpha, std::vector<TaggedInstance>& out) const {
  impl_->collect_tagged(view, alpha, out);
}
bool EmitterSystem::set_draw_suppressed(std::string_view name, bool on) {
  bool any = false;
  for (auto& header : impl_->headers)
    if (header.nm() == name) header.draw_off = on, any = true;
  return any;
}
bool EmitterSystem::set_draw_dropped(std::string_view name, bool on) {
  bool any = false;
  for (auto& header : impl_->headers)
    if (header.nm() == name) header.cb_no_draw = on, any = true;
  return any;
}
size_t EmitterSystem::header_count() const { return impl_->headers.size(); }
const assets::Emitter& EmitterSystem::header_def(size_t i) const { return *impl_->headers.at(i).def; }
bool EmitterSystem::header_running(size_t i) const { return i < impl_->headers.size() && impl_->headers[i].linked; }

bool EmitterSystem::start(std::string_view name) {
  const int i = impl_->find_header(name);
  // Dev trace: every sequence / overlay emitter start by name.
  static const bool trace = std::getenv("OC_FX_START") != nullptr;
  if (trace) std::fprintf(stderr, "fx start '%.*s' -> header %d\n", static_cast<int>(name.size()), name.data(), i);
  return i >= 0 && impl_->start_index(static_cast<size_t>(i));
}
bool EmitterSystem::start_index(size_t i) { return impl_->start_index(i); }

// Sequence op 0x22 inside a per-target loop (the task's target index != 0): starts a clone
// of the emitter whose op-39 record has actor selector 1 rewritten to 9 (+ caster index 0) and 2 to 0xD + target, in both the
// first (a) and the second (c) point. The clone keeps following that target while the loop moves on; the original still means target 0.
bool EmitterSystem::start_for_target(std::string_view name, int target) { return start_for_actor(name, 0, target); }

bool EmitterSystem::start_for_actor(std::string_view name, int caster, int target) {
  const int i = impl_->find_header(name);
  static const bool trace = std::getenv("OC_FX_START") != nullptr;
  if (trace) std::fprintf(stderr, "fx start '%.*s' caster %d target %d -> header %d\n", static_cast<int>(name.size()), name.data(), caster, target, i);
  if (i < 0) return false;
  if (target <= 0 && caster <= 0) return impl_->start_index(static_cast<size_t>(i));
  Header clone = impl_->headers[static_cast<size_t>(i)];
  clone.alias = "sado";
  clone.linked = false;
  clone.stopped = true;
  clone.frame = clone.countdown = 0;
  std::array<int, 4> sel{};
  bool has = false;
  if (const auto* pv = clone.patched(0x39)) {
    sel = *pv;
    has = true;
  } else {
    for (const auto& op : clone.def->ops)
      if (op.op == 0x39)
        if (const auto* basis_arg = std::get_if<assets::FxBasis>(&op.arg)) {
          sel = {basis_arg->a, basis_arg->b, basis_arg->c, basis_arg->d};
          has = true;
          break;
        }
  }
  if (has) {
    for (size_t point : {size_t{0}, size_t{2}}) {
      if (sel[point] == 1) sel[point] = 9 + std::max(0, caster);
      else if (sel[point] == 2) sel[point] = 0xD + std::max(0, target);
    }
    clone.patch[0x39] = sel;
    if (clone.has_basis_op)
      for (size_t k = 0; k < 4; ++k) clone.basis_sel[k] = static_cast<u8>(sel[k]);
  }
  impl_->headers.push_back(std::move(clone));
  ++impl_->st.headers;
  return impl_->start_index(impl_->headers.size() - 1);
}

int EmitterSystem::layer_off(std::string_view name) {
  int count = 0;
  for (size_t i = 0; i < impl_->headers.size(); ++i) {
    Header& header = impl_->headers[i];
    if (header.nm() != name) continue;
    impl_->kill_objects(static_cast<int>(i));
    header.linked = false;
    header.stopped = true;
    ++count;
  }
  return count;
}

// The stage's lens-flare emitters (a sprite op of type 21: the stage header 'far' without a mesh). The stage flare object is gone
// from the element pack's start until the pack ends (a fresh object returns 3 ticks after the effect ends), so the flare and its
// full-screen additive flash are not drawn while a pack runs.
static bool header_is_lens(const assets::Emitter& emitter) {
  for (const auto& op : emitter.ops)
    if (op.op == 0x05)
      if (const auto* sprite = std::get_if<assets::FxSprite>(&op.arg)) return sprite->type == 21;
  return false;
}

int EmitterSystem::lens_off() {
  int count = 0;
  for (size_t i = 0; i < impl_->headers.size(); ++i) {
    Header& header = impl_->headers[i];
    if (!header_is_lens(*header.def)) continue;
    // The flare is one object the header spawned once: its header is normally stopped already.
    impl_->kill_objects(static_cast<int>(i));
    header.linked = false;
    header.stopped = true;
    header.lens_hidden = true;
    ++count;
  }
  return count;
}

int EmitterSystem::lens_on() {
  int count = 0;
  for (Header& header : impl_->headers) {
    if (!header_is_lens(*header.def) || !header.lens_hidden) continue;
    header.lens_hidden = false;
    header.linked = true;
    header.frame = 0;
    header.countdown = 1;
    header.stopped = false;
    ++count;
  }
  return count;
}

int EmitterSystem::layer_on(std::string_view name, bool any_duration) {
  int count = 0;
  for (Header& header : impl_->headers) {
    if (header.nm() != name || (!any_duration && header.def->duration != 0)) continue;
    // Relink; header bit 10 restarts the emitter.
    if (header.stopped) {
      header.linked = true;
      if (header.def->flags & (1u << 10)) {
        header.frame = 0;
        header.countdown = 1;
      }
    }
    header.stopped = false;
    ++count;
  }
  return count;
}

// Stop / kill reach every header of the name: the clones of start_for_target share it.
bool EmitterSystem::stop(std::string_view name) {
  bool any = false;
  for (auto& header : impl_->headers)
    if (name == "0000" || header.nm() == name) {
      header.linked = false;
      header.stopped = true;
      any = true;
    }
  return any || name == "0000";
}

bool EmitterSystem::stop_expire(std::string_view name) {
  // The op does nothing for a zero name word.
  if (name.empty()) return false;
  // The title sea keeps every scenery set (Config).
  if (impl_->cfg.keep_on_stop_expire) return false;
  bool any = false;
  for (size_t i = 0; i < impl_->headers.size(); ++i) {
    Header& header = impl_->headers[i];
    if (header.nm() != name) continue;
    header.linked = false;
    header.stopped = true;
    any = true;
    for (auto& obj : impl_->objs) {
      if (!obj.alive || obj.owner != static_cast<int>(i)) continue;
      // A persistent object (life 0) is freed; any other one expires on the next step.
      if (obj.life == 0) obj.alive = false;
      else obj.life = 1;
    }
  }
  return any;
}

bool EmitterSystem::kill(std::string_view name) {
  if (name == "0000") {
    stop(name);
    impl_->kill_objects(-1);
    return true;
  }
  if (!stop(name)) return false;
  for (size_t i = 0; i < impl_->headers.size(); ++i)
    if (impl_->headers[i].nm() == name) impl_->kill_objects(static_cast<int>(i));
  return true;
}

bool EmitterSystem::attach(std::string_view name, int anchor_id) {
  const int i = impl_->find_header(name);
  if (i < 0) return false;
  impl_->headers[static_cast<size_t>(i)].anchor = anchor_id;
  return impl_->start_index(static_cast<size_t>(i));
}

int EmitterSystem::attach_all(std::string_view name, int anchor_id, int& duration) {
  int count = 0;
  duration = 0;
  for (size_t i = 0; i < impl_->headers.size(); ++i) {
    if (impl_->headers[i].nm() != name) continue;
    if (count++ == 0) duration = impl_->headers[i].def->duration;
    impl_->headers[i].anchor = anchor_id;
    impl_->start_index(i);
  }
  return count;
}
void EmitterSystem::tick() { impl_->tick(); }
bool EmitterSystem::spawn_at(std::string_view name, int x, int y, int z, int angle, Vec3* normal) {
  // Fire the header, then set the object's position := (x, y, z) << 16 and angle Z := -angle.
  const int hi = impl_->find_header(name);
  if (hi < 0) return false;
  const int oi = impl_->fire(static_cast<size_t>(hi), nullptr);
  if (oi < 0) return false;
  Object& obj = impl_->objs[static_cast<size_t>(oi)];
  obj.pos[0] = obj.ppos[0] = static_cast<s32>(static_cast<u32>(x) << 16);
  obj.pos[1] = obj.ppos[1] = static_cast<s32>(static_cast<u32>(y) << 16);
  obj.pos[2] = obj.ppos[2] = static_cast<s32>(static_cast<u32>(z) << 16);
  obj.ang[2] = obj.pang[2] = static_cast<s16>(-angle);
  if (normal) {
    const Mat3 rot = Mat3::from_angles(obj.ang[0], obj.ang[1], obj.ang[2], obj.flags & kFlagZyx);
    // The sprite's x / y axes (columns 0 / 1).
    const Vec3 axis_x{rot.m[0], rot.m[3], rot.m[6]}, axis_y{rot.m[1], rot.m[4], rot.m[7]};
    *normal = {axis_x[1] * axis_y[2] - axis_x[2] * axis_y[1], axis_x[2] * axis_y[0] - axis_x[0] * axis_y[2], axis_x[0] * axis_y[1] - axis_x[1] * axis_y[0]};
  }
  return true;
}
void EmitterSystem::collect_quads(const ViewParams& view, float alpha, std::vector<DrawQuad>& out) const {
  impl_->collect(view, alpha, out);
}
void EmitterSystem::collect_meshes(const ViewParams& view, float alpha, std::vector<MeshInstance>& out, bool with_suppressed) const {
  impl_->collect_meshes(view, alpha, out, with_suppressed);
}
bool EmitterSystem::slot_object(int slot, Vec3& pos, Vec3& vel) const {
  if (slot < 0 || slot > 1) return false;
  const size_t i = impl_->slot_idx[slot];
  if (i >= impl_->objs.size()) return false;
  for (size_t k = 0; k < 3; ++k) {
    pos[k] = static_cast<float>(impl_->objs[i].pos[k]) / 65536.0f;
    vel[k] = static_cast<float>(impl_->objs[i].vel[k]) / 65536.0f;
  }
  return true;
}
bool EmitterSystem::slot_position(int slot, Vec3& out) const {
  if (slot < 0 || slot > 1) return false;
  const size_t i = impl_->slot_idx[slot];
  if (i >= impl_->objs.size()) return false;
  for (size_t k = 0; k < 3; ++k) out[k] = static_cast<float>(impl_->objs[i].pos[k]) / 65536.0f;
  return true;
}
bool EmitterSystem::object_view(size_t i, ObjectView& out) const {
  if (i >= impl_->objs.size()) return false;
  const Object& obj = impl_->objs[i];
  for (size_t k = 0; k < 3; ++k) {
    out.pos[k] = static_cast<float>(obj.pos[k]) / 65536.0f;
    out.vel[k] = static_cast<float>(obj.vel[k]) / 65536.0f;
    out.acc[k] = static_cast<float>(obj.acc[k]) / 65536.0f;
    out.scale[k] = obj.scale[k];
    out.scale_rate[k] = obj.srate[k];
    out.angle[k] = obj.ang[k];
    out.color[k] = obj.col[k];
  }
  out.life = obj.life;
  out.frame = obj.frame;
  out.bias = obj.bias;
  out.flags = obj.flags;
  out.owner = obj.owner;
  out.emitter = obj.owner >= 0 && static_cast<size_t>(obj.owner) < impl_->headers.size() && impl_->headers[static_cast<size_t>(obj.owner)].def
                    ? &impl_->headers[static_cast<size_t>(obj.owner)].nm()
                    : nullptr;
  out.sprite = obj.sheet ? &obj.sheet->name : (obj.mesh_name ? obj.mesh_name : obj.light_name);
  out.stype = obj.stype;
  for (size_t k = 0; k < 3; ++k) out.rgb[k] = static_cast<u8>((obj.col[k] >> 4) & 0xFF);
  out.clip = obj.stype == 20 ? obj.model_clip : -1;
  out.motion = obj.motion;
  out.loop = obj.model_loop;
  return true;
}

EmitterStats EmitterSystem::stats() const { return impl_->st; }
size_t EmitterSystem::live_objects() const { return impl_->objs.size(); }
u64 EmitterSystem::ticks() const { return impl_->tick_count; }

}  // namespace oc::effects
