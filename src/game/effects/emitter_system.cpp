#include "game/effects/emitter_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace oc::effects {

using namespace emitter_detail;

// ---- Curve ---------------------------------------------------------------------------------------------------------------

Curve Curve::parse(ByteSpan data) {
  Curve curve;
  auto rd16 = [&](size_t offset) -> u32 { return offset + 2 <= data.size() ? static_cast<u32>(data[offset] | (data[offset + 1] << 8)) : 0u; };
  auto rd32 = [&](size_t offset) -> u32 { return rd16(offset) | (rd16(offset + 2) << 16); };
  if (data.size() < 0x34) return curve;
  curve.flags = rd32(0x20);
  for (size_t i = 0; i < 4; ++i) curve.basis_sel[i] = data[0x28 + i];
  const u32 table_count = rd32(0x30);
  const size_t base = 0x34 + 8ull * table_count;
  if (base + 0x14 > data.size()) return curve;
  const u32 key_count = rd32(base);
  if (base + 0x14 + 8ull * key_count > data.size()) return curve;
  for (u32 i = 0; i < key_count; ++i) {
    const size_t key_offset = base + 0x14 + 8ull * i;
    curve.keys.push_back({static_cast<s16>(rd16(key_offset)), static_cast<s16>(rd16(key_offset + 2)), static_cast<s16>(rd16(key_offset + 4)),
                          static_cast<u16>(rd16(key_offset + 6))});
  }
  curve.valid = !curve.keys.empty();
  return curve;
}

Vec3 Curve::sample(int time) const {
  if (keys.empty()) return {0, 0, 0};
  auto key = [&](size_t i) { return Vec3{static_cast<float>(keys[i].x), static_cast<float>(keys[i].y), static_cast<float>(keys[i].z)}; };
  if (keys.size() <= 1) return key(0);
  if (time >= keys.back().time) return key(keys.size() - 1);
  size_t i = 0;
  // Segment [i, i+1]; before the first key this extrapolates.
  while (i + 2 < keys.size() && time >= keys[i + 1].time) ++i;
  const float t0 = keys[i].time, t1 = keys[i + 1].time;
  const float blend = t1 > t0 ? (static_cast<float>(time) - t0) / (t1 - t0) : 0.0f;
  const Vec3 from = key(i), to = key(i + 1);
  return {from[0] + (to[0] - from[0]) * blend, from[1] + (to[1] - from[1]) * blend, from[2] + (to[2] - from[2]) * blend};
}

// ---- EffectBank ----------------------------------------------------------------------------------------------------------

const assets::SpriteSheet* EffectBank::sheet(std::string_view name) const {
  for (const auto* pack : packs_)
    if (auto* found = pack->find_sheet(name)) return found;
  return nullptr;
}
const assets::Sequence* EffectBank::sequence(std::string_view name) const {
  for (const auto* pack : packs_)
    if (auto* found = pack->find_sequence(name)) return found;
  return nullptr;
}
const assets::EffectBlob* EffectBank::blob(u8 type, std::string_view name) const {
  for (const auto* pack : packs_)
    if (auto* found = pack->find_blob(type, name)) return found;
  return nullptr;
}
const Curve* EffectBank::curve(std::string_view name) const {
  auto it = curves_.find(name);
  if (it == curves_.end()) {
    const auto* record = blob(0x18, name);
    it = curves_.emplace(std::string(name), record ? Curve::parse(record->data) : Curve{}).first;
  }
  return it->second.valid ? &it->second : nullptr;
}

ViewParams ViewParams::from_room(const assets::RoomCamera& camera) {
  ViewParams view;
  for (size_t i = 0; i < 9; ++i) view.rot.m[i] = static_cast<float>(camera.matrix[i]) / 4096.0f;
  for (size_t i = 0; i < 3; ++i) view.trans[i] = static_cast<float>(camera.trans[i]);
  view.h = static_cast<float>(static_cast<u16>(camera.proj_h));
  return view;
}

void add_grid_quads(const ViewParams& vp, const FloorGrid& grid, std::vector<DrawQuad>& quads) {
  const auto& rot = vp.rot.m;
  auto project = [&](float x, float y, float z, float& sx, float& sy, float& depth) {
    const float view_x = rot[0] * x + rot[1] * y + rot[2] * z + vp.trans[0], view_y = rot[3] * x + rot[4] * y + rot[5] * z + vp.trans[1];
    depth = rot[6] * x + rot[7] * y + rot[8] * z + vp.trans[2];
    if (depth < 16.0f) return false;
    sx = vp.ox + vp.h * view_x / depth + vp.scroll_x, sy = vp.oy + vp.h * view_y / depth + vp.scroll_y;
    return true;
  };
  u32 order = quads.empty() ? 0 : quads.back().order + 1;
  auto line = [&](float ax, float ay, float az, float bx, float by, float bz, std::array<u8, 3> rgb, float width) {
    float x0, y0, d0, x1, y1, d1;
    if (!project(ax, ay, az, x0, y0, d0) || !project(bx, by, bz, x1, y1, d1)) return;
    const float dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
    const float nx = len > 1e-4f ? -dy / len * width : width, ny = len > 1e-4f ? dx / len * width : 0.0f;
    DrawQuad quad;
    quad.untextured = 2;
    quad.x[0] = x0 - nx, quad.y[0] = y0 - ny, quad.x[1] = x1 - nx, quad.y[1] = y1 - ny;
    quad.x[2] = x0 + nx, quad.y[2] = y0 + ny, quad.x[3] = x1 + nx, quad.y[3] = y1 + ny;
    quad.r = quad.r1 = rgb[0], quad.g = quad.g1 = rgb[1], quad.b = quad.b1 = rgb[2];
    quad.blend = 0;
    quad.slot = std::clamp(static_cast<int>((d0 + d1) * 0.5f) >> 4, 0, 0xFFF);
    quad.order = order++;
    quads.push_back(quad);
  };
  const float step = grid.step, x0 = (std::round(grid.cx / step) - static_cast<float>(grid.half)) * step,
              z0 = (std::round(grid.cz / step) - static_cast<float>(grid.half)) * step;
  const int cells = 2 * grid.half;
  constexpr std::array<u8, 3> kGrey{92, 92, 92}, kX{200, 64, 64}, kY{90, 180, 72}, kZ{72, 112, 210};
  for (int i = 0; i <= cells; ++i)
    for (int j = 0; j < cells; ++j) {
      const float along = static_cast<float>(i) * step, across = static_cast<float>(j) * step;
      // Lines along z at x = x0 + along, lines along x at z = z0 + along.
      const bool on_z_axis = x0 + along == 0.0f, on_x_axis = z0 + along == 0.0f;
      line(x0 + along, 0, z0 + across, x0 + along, 0, z0 + across + step, on_z_axis ? kZ : kGrey, on_z_axis ? 0.75f : 0.4f);
      line(x0 + across, 0, z0 + along, x0 + across + step, 0, z0 + along, on_x_axis ? kX : kGrey, on_x_axis ? 0.75f : 0.4f);
    }
  // PS1 y is down: up = -y.
  const float span = static_cast<float>(cells) * step;
  if (x0 <= 0.0f && 0.0f <= x0 + span && z0 <= 0.0f && 0.0f <= z0 + span) line(0, 0, 0, 0, -step, 0, kY, 0.75f);
}

void sort_draw_order(std::vector<DrawQuad>& quads) {
  std::stable_sort(quads.begin(), quads.end(), [](const DrawQuad& lhs, const DrawQuad& rhs) {
    if (lhs.slot != rhs.slot) return lhs.slot > rhs.slot;
    return lhs.order > rhs.order;
  });
}

// ---- Impl ----------------------------------------------------------------------------------------------------------------

bool EmitterSystem::Impl::pace_blocks(std::string_view name) {
  if (!pace_on) return false;
  const int ord = pace_ord[std::string(name)]++;
  return !pace_sites.emplace(std::string(name), ord).second;
}

EmitterSystem::Impl::Impl(const EffectBank& effect_bank, Config config) : bank(effect_bank), cfg(config), heap(config.heap_bytes) {
  rng.state = config.seed ? config.seed : 1;
  if (const char* names = config.dev_only_filter ? std::getenv("OC_FX_ONLY") : nullptr) dev_only = std::string(",") + names + ",";
}

bool EmitterSystem::Impl::dev_skip(const Object& obj) const {
  if (dev_only.empty() || obj.owner < 0 || static_cast<size_t>(obj.owner) >= headers.size()) return false;
  return dev_only.find("," + headers[static_cast<size_t>(obj.owner)].nm() + ",") == std::string::npos;
}

// -- registry --------------------------------------------------------------------------------------------------------
int EmitterSystem::Impl::find_header(std::string_view name) const {
  for (size_t i = 0; i < headers.size(); ++i)
    if (headers[i].nm() == name) return static_cast<int>(i);
  return -1;
}

void EmitterSystem::Impl::register_all() {
  for (size_t pack_index = 0; pack_index < bank.pack_count(); ++pack_index)
    for (const auto& emitter : bank.pack(pack_index).emitters) {
      Header header;
      header.def = &emitter;
      header.big = emitter.flags & 0x202;
      header.huge = emitter.flags & (1u << 9);
      for (const auto& op : emitter.ops) {
        if (op.op == 0x30) header.has_anchor_op = true;
        if (op.op == 0x39 && !header.has_basis_op) {
          if (const auto* basis_arg = std::get_if<assets::FxBasis>(&op.arg)) {
            header.has_basis_op = true;
            header.basis_sel = {basis_arg->a, basis_arg->b, basis_arg->c, basis_arg->d};
            header.basis_flags = basis_arg->flags;
          }
        }
      }
      // Dev: a header's op list.
      if (static const char* hdr_dump = std::getenv("OC_FX_HDR"); hdr_dump && emitter.name.find(hdr_dump) != std::string::npos) {
        std::string line = "fx hdr " + emitter.name + " flags " + std::to_string(emitter.flags) + ":";
        for (const auto& op : emitter.ops) {
          line += " " + std::to_string(op.op);
          if (const auto* bs = std::get_if<assets::FxBasis>(&op.arg)) line += "(sel " + std::to_string(bs->a) + " j " + std::to_string(bs->b) + " sel2 " + std::to_string(bs->c) + " j2 " + std::to_string(bs->d) + ")";
        }
        std::fprintf(stderr, "%s\n", line.c_str());
      }
      // Link unless bit2, then stopped := bit2. A resident library was registered when it was loaded (battle start), long
      // before this effect: its headers wait for a start.
      // A header that waits keeps the frame / countdown words the file stores (only a linked header with bit 10 clears them):
      // a periodic child nothing ever starts (op 0x2D) reads its stored frame as the oscillator phase. A start zeroes them.
      const bool waits = (emitter.flags & 4) != 0 || bank.is_library(pack_index);
      if (waits) {
        header.frame = static_cast<s16>(emitter.frame0);
        header.countdown = emitter.count0;
      } else {
        header.linked = true;
        header.frame = 0;
        header.countdown = static_cast<s16>(emitter.period);
      }
      header.stopped = waits;
      headers.push_back(header);
    }
  st.headers = static_cast<u32>(headers.size());
}

bool EmitterSystem::Impl::start_index(size_t i) {
  if (i >= headers.size()) return false;
  Header& header = headers[i];
  // Dev trace: the header words of every emitter start.
  if (static const bool trace = std::getenv("OC_FX_START") != nullptr; trace)
    std::fprintf(stderr, "fx header %s start %d dur %d period %d repeat %d jitter %u flags %08X\n", header.def->name.c_str(), header.def->start, header.def->duration,
                 header.def->period, header.def->repeat, header.def->jitter(), header.def->flags);
  if (header.stopped) header.linked = true;
  ++st.starts;
  header.frame = 0;
  header.countdown = 1;
  header.stopped = false;
  return true;
}

void EmitterSystem::Impl::kill_objects(int owner) {
  for (auto& obj : objs)
    if (obj.alive && (owner < 0 || obj.owner == owner)) release(obj);
}

void EmitterSystem::Impl::release(Object& obj) {
  obj.alive = false;
  if (!obj.heap_held) return;
  heap.free(obj.heap_node);
  obj.heap_held = false;
}

// -- basis helpers ---------------------------------------------------------------------------------------------------
Basis EmitterSystem::Impl::actor_basis(int sel_a, int sel_b, int sel_c, int sel_d, int& scale, bool& found) const {
  Basis out;
  scale = 0;
  found = false;
  if (sel_a == 0) return out;
  if (hooks.actor_basis) {
    int actor_scale = 4096;
    if (hooks.actor_basis(sel_a, sel_b, sel_c, sel_d, out, actor_scale)) {
      scale = actor_scale;
      found = true;
    }
  }
  return out;
}

Basis EmitterSystem::Impl::heading_basis(const Object& obj) const {
  Basis basis;
  basis.m = Mat3::from_angles(0, -ratan2(obj.vel[2] >> 4, obj.vel[0] >> 4), 0);
  basis.t = {static_cast<float>(obj.pos[0] >> 16), static_cast<float>(obj.pos[1] >> 16), static_cast<float>(obj.pos[2] >> 16)};
  return basis;
}

// Per light slot, delta = light - pos (s16 arithmetic on the PS1), dist = sqrt(|delta|^2), n = normalised delta
// (4.12 unit vector, zero for a zero vector), k =intensity - (dist * falloff >> 12) (falloff << 3 in the field) clamped to 0..0x7FFF,
// row i = n * k >> 12; colour matrix column i = colour bytes << 4. Here in float: row = n * k / 4096, colour = bytes / 256.
LightMatrices light_matrices(const LightTable& lights, const Vec3& pos, bool field) {
  LightMatrices matrices;
  for (size_t i = 0; i < 3; ++i) {
    const LightTable::Slot& slot = lights.slot[i];
    float delta[3];
    for (size_t axis = 0; axis < 3; ++axis) delta[axis] = static_cast<float>(static_cast<s16>(slot.pos[axis] - static_cast<s32>(pos[axis])));
    const float dist = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    const s64 fall = field ? static_cast<s64>(slot.falloff) << 3 : static_cast<s64>(slot.falloff);
    s64 k = static_cast<s64>(slot.intensity) - ((static_cast<s64>(dist) * fall) >> 12);
    k = std::clamp<s64>(k, 0, 0x7FFF);
    for (size_t axis = 0; axis < 3; ++axis) {
      matrices.dir[i][axis] = dist > 0.0f ? delta[axis] / dist * static_cast<float>(k) / 4096.0f : 0.0f;
      matrices.rgb[i][axis] = static_cast<float>(slot.rgb[axis]) / 256.0f;
    }
  }
  return matrices;
}

}  // namespace oc::effects
