#include "game/effects/emitter_internal.h"

#include "render/camera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace oc::effects {

namespace {

// Dev: OC_BIT7_TRACE=1 prints once per (header, selector, joint) every object with flag bits 15 + 7 (the joint-rotation path).
// OC_BIT7_TRACE=2: every draw prints the object's final matrix.
void trace_bit7_mat(const std::string& name, const s32 (&pos)[3], const Mat3& matrix, int tick) {
  static const char* mode = std::getenv("OC_BIT7_TRACE");
  if (!mode || mode[0] != '2') return;
  std::fprintf(stderr, "bit7mat: t%d %s pos %d %d %d mat %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", tick, name.c_str(), pos[0] >> 16, pos[1] >> 16, pos[2] >> 16, matrix.m[0], matrix.m[1], matrix.m[2], matrix.m[3], matrix.m[4], matrix.m[5], matrix.m[6], matrix.m[7], matrix.m[8]);
}
void trace_bit7(const char* where, const std::string& name, bool anchor, bool basis_op, const std::array<u8, 4>& sel, bool ok, bool has_joint) {
  static const bool on = std::getenv("OC_BIT7_TRACE") != nullptr;
  if (!on) return;
  static std::set<std::string> seen;
  char key[160];
  std::snprintf(key, sizeof key, "%s/%s a%d b%d %d.%d.%d.%d ok%d j%d", where, name.c_str(), anchor, basis_op, sel[0], sel[1], sel[2], sel[3], ok, has_joint);
  if (seen.insert(key).second) std::fprintf(stderr, "bit7: %s\n", key);
}

// The object screen test of flag bit 28 for every type but the type-0x02 mesh: the object origin (its view-space position) goes
// through the projection and the object is dropped when the GTE would report an error: view x / y outside -0x8000..0x7FFF, depth
// outside 0..0xFFFF, a depth of at most h / 2 (also behind the eye), or a screen x / y outside -1024..1023 (x widened like
// render::cull_frame). False under the free camera.
bool origin_off_screen(u32 flags, const Vec3& origin, const ViewParams& view) {
  if (!(flags & kFlagScreenTest) || render::cull_bypass()) return false;
  const s64 ir1 = static_cast<s64>(std::floor(origin[0])), ir2 = static_cast<s64>(std::floor(origin[1]));
  const s64 depth = static_cast<s64>(std::floor(origin[2]));
  if (ir1 < -0x8000 || ir1 > 0x7FFF || ir2 < -0x8000 || ir2 > 0x7FFF || depth < 0 || depth > 0xFFFF) return true;
  const s64 proj_h = std::llround(view.h);
  if (depth * 2 <= proj_h) return true;
  const s64 scale = std::min<s64>((proj_h << 16) / depth, 0x1FFFF);
  const s64 sx = (std::llround(view.ox * 65536.0f) + ir1 * scale) >> 16, sy = (std::llround(view.oy * 65536.0f) + ir2 * scale) >> 16;
  return !render::cull_screen_ok(render::cull_frame(render::wide_extra()), static_cast<float>(sx), static_cast<float>(sy));
}

}  // namespace

using namespace emitter_detail;

void EmitterSystem::Impl::collect_meshes(const ViewParams& view, float alpha, std::vector<MeshInstance>& out, bool with_suppressed) const {
  for (const Object& obj : objs) {
    if (!obj.alive || (obj.stype != 14 && obj.stype != 19 && obj.stype != 20) || !obj.mesh_name || obj.poly_tag >= 0 || dev_skip(obj)) continue;
    if (!with_suppressed && obj.owner >= 0 && static_cast<size_t>(obj.owner) < headers.size() && headers[static_cast<size_t>(obj.owner)].draw_off) continue;
    MeshInstance mi;
    mi.name = obj.mesh_name;
    mi.world_mat = obj.mat;
    mi.motion = obj.motion;
    mi.motion_prev = obj.motion_prev;
    mi.clip = obj.model_clip;
    mi.loop = obj.model_loop;
    Vec3 wpos;
    for (size_t i = 0; i < 3; ++i) {
      const double prev = static_cast<double>(obj.ppos[i]) / 65536.0, cur = static_cast<double>(obj.pos[i]) / 65536.0;
      wpos[i] = static_cast<float>(std::fabs(cur - prev) > 20000.0 ? cur : prev + (cur - prev) * alpha);
    }
    if (obj.flags & kFlagRelParent) {
      const Header& header = headers[static_cast<size_t>(std::max(obj.owner, 0))];
      Basis basis;
      bool ok = false;
      if (header.has_anchor_op && header.anchor >= 0 && (hooks.anchor_basis_at || hooks.anchor_basis)) ok = anchor_at(header.anchor, alpha, basis);
      else if (header.has_basis_op && hooks.actor_basis) {
        int sc = 0;
        ok = hooks.actor_basis(header.basis_sel[0], header.basis_sel[1], header.basis_sel[2], header.basis_sel[3], basis, sc);
      }
      if (obj.flags & kFlagJointRot) trace_bit7("mesh", header.nm(), header.has_anchor_op, header.has_basis_op, header.basis_sel, ok, basis.has_joint);
      if (ok) {
        const Vec3 world = basis.m.apply(wpos);
        wpos = {world[0] + basis.t[0], world[1] + basis.t[1], world[2] + basis.t[2]};
        // Flag bit 7: the attach joint's world rotation in front of the object's own matrix.
        if (basis.has_joint && (obj.flags & kFlagJointRot) && !header.has_anchor_op) mi.world_mat = basis.joint * mi.world_mat;
        if (basis.has_joint && (obj.flags & kFlagJointRot) && !header.has_anchor_op) trace_bit7_mat(header.nm(), obj.pos, mi.world_mat, static_cast<int>(tick_count));
      }
    }
    mi.world_pos = wpos;
    if ((obj.ext & 2) && obj.morph_name) {
      // Blend = ease(phase) with the s16 phase reflected at bit 12 (a triangle wave of period 0x2000), mode = ext bits 13-15
      auto blend_of = [&](s16 phase) {
        int folded = static_cast<int>(static_cast<u16>(phase)) & 0x1FFF;
        if (folded >= 0x1000) folded = 0x2000 - folded;
        return static_cast<float>(ease(folded, static_cast<int>((obj.ext >> 13) & 7u))) / 4096.0f;
      };
      mi.morph_name = obj.morph_name;
      mi.morph_t = blend_of(obj.morph_phase);
      mi.morph_t_prev = blend_of(obj.morph_phase_prev);
    }
    Vec3 vc = (obj.flags & kFlagViewSpace) ? wpos : view.rot.apply(wpos);
    // Camera-relative z (flag bit 24): view.h << pos_shift.
    if (obj.flags & kFlagCamRel) vc[2] += view.h * static_cast<float>(1 << cfg.pos_shift);
    else for (size_t i = 0; i < 3; ++i) vc[i] += view.trans[i];
    mi.centre = vc;
    // world_mat = obj.mat, with the attach joint's rotation in front (flag bit 7).
    mi.mat = (obj.flags & kFlagWorldQuad) ? view.rot * mi.world_mat : mi.world_mat;
    for (size_t i = 0; i < 3; ++i) mi.rgb[i] = static_cast<u8>((static_cast<int>(obj.col[i]) >> 4) & 0xFF);
    mi.blend = static_cast<int>(obj.flags & 7);
    mi.flags = obj.flags;
    mi.bias = obj.bias;
    mi.fixed_slot = (obj.flags & kFlagFixedSlot) != 0;
    mi.frame = obj.frame;
    mi.ext = obj.ext;
    mi.ext_path = (obj.flags & kFlagExt) != 0;
    // The 24.8 phase between the previous tick and this one, in texels wrapped to 0..128 (the 7-bit texel field).
    for (size_t k = 0; k < 2; ++k) {
      const s32 delta = static_cast<s32>(static_cast<u32>(obj.uv_phase[k]) - static_cast<u32>(obj.uv_phase_prev[k]));
      const float raw = static_cast<float>(obj.uv_phase_prev[k] & 0x7FFF) + static_cast<float>(delta) * alpha;
      mi.uv_phase[k] = std::fmod(std::fmod(raw / 256.0f, 128.0f) + 128.0f, 128.0f);
    }
    mi.owner = obj.owner;
    mi.marked = obj.marked;
    mi.sprite_type = obj.stype;
    if ((obj.ext & 1) && obj.owner >= 0 && static_cast<size_t>(obj.owner) < headers.size() && headers[static_cast<size_t>(obj.owner)].def)
      for (const auto& op : headers[static_cast<size_t>(obj.owner)].def->ops)
        if (op.op == 0x36)
          if (const auto* uv_scroll = std::get_if<assets::FxUvScroll>(&op.arg)) {
            mi.uv_scroll = true;
            mi.uv_window = {uv_scroll->off_u, uv_scroll->off_v, uv_scroll->win_w, uv_scroll->win_h};
          }
    out.push_back(std::move(mi));
  }
}

bool EmitterSystem::MeshInstance::off_screen(const std::array<std::array<s16, 3>, 6>& points, const ViewParams& view) const {
  if (sprite_type != 14) return off_screen(view);
  if (!(flags & kFlagScreenTest) || render::cull_bypass()) return false;
  // Widescreen columns widen the x limits about the PS1 centre column like the packet rules (render::cull_frame).
  const render::CullFrame frame = render::cull_frame(render::wide_extra());
  const float widen = frame.x_span / 1023.0f;
  const s64 x_min = static_cast<s64>(std::floor(frame.x_lo)), x_max = static_cast<s64>(std::floor(frame.x_hi));
  const s64 x_in_lo = static_cast<s64>(std::ceil(160.0f - 159.0f * widen)), x_in_hi = static_cast<s64>(std::floor(160.0f + 191.0f * widen));
  const s64 proj_h = std::llround(view.h);
  const s64 ofx = std::llround(view.ox * 65536.0f), ofy = std::llround(view.oy * 65536.0f);
  for (const auto& point : points) {
    const Vec3 rotated = mat.apply({static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])});
    const s64 ir1 = std::clamp<s64>(static_cast<s64>(std::floor(rotated[0] + centre[0])), -0x8000, 0x7FFF);
    const s64 ir2 = std::clamp<s64>(static_cast<s64>(std::floor(rotated[1] + centre[1])), -0x8000, 0x7FFF);
    const s64 depth = std::clamp<s64>(static_cast<s64>(std::floor(rotated[2] + centre[2])), 0, 0xFFFF);
    const s64 scale = proj_h < depth * 2 ? std::min<s64>((proj_h << 16) / depth, 0x1FFFF) : 0x1FFFF;
    const s64 sx = std::clamp<s64>((ofx + ir1 * scale) >> 16, x_min, x_max), sy = std::clamp<s64>((ofy + ir2 * scale) >> 16, -0x400, 0x3FF);
    if ((sx >= x_in_lo && sx <= x_in_hi) || (sy >= -31 && sy <= 223)) return false;
  }
  return true;
}

bool EmitterSystem::MeshInstance::off_screen(const ViewParams& view) const {
  return sprite_type != 14 && origin_off_screen(flags, centre, view);
}

// Objects whose draw an overlay replaced (poly_tag >= 0): the object transform set up for it (the mesh-sprite one above).
void EmitterSystem::Impl::collect_tagged(const ViewParams& view, float alpha, std::vector<EmitterSystem::TaggedInstance>& out) const {
  for (const Object& obj : objs) {
    if (!obj.alive || obj.poly_tag < 0) continue;
    EmitterSystem::TaggedInstance ti;
    ti.tag = obj.poly_tag;
    Vec3 wpos;
    for (size_t i = 0; i < 3; ++i) {
      const double prev = static_cast<double>(obj.ppos[i]) / 65536.0, cur = static_cast<double>(obj.pos[i]) / 65536.0;
      wpos[i] = static_cast<float>(std::fabs(cur - prev) > 20000.0 ? cur : prev + (cur - prev) * alpha);
    }
    Vec3 vc = (obj.flags & kFlagViewSpace) ? wpos : view.rot.apply(wpos);
    if (obj.flags & kFlagCamRel) vc[2] += view.h * static_cast<float>(1 << cfg.pos_shift);
    else for (size_t i = 0; i < 3; ++i) vc[i] += view.trans[i];
    ti.centre = vc;
    ti.mat = (obj.flags & kFlagWorldQuad) ? view.rot * obj.mat : obj.mat;
    for (size_t i = 0; i < 3; ++i) ti.rgb[i] = static_cast<u8>((static_cast<int>(obj.col[i]) >> 4) & 0xFF);
    ti.bias = obj.bias;
    ti.flags = obj.flags;
    out.push_back(ti);
  }
}

// Sprite type 15: 'flat' = POLY_F4 with the object-space corners (-2048, 2048), (2048, 2048), (-2048, -2048), (2048, -2048)
// through the object's transform, flat object colour; 'line' = LineG2 from the object
// origin (object colour) to -vel >> (12 - pos_shift) (black): a trail as long as 16 ticks of travel. Both are depth-cued like the sheet sprites
// (flag 17), semi-transparent with the object's blend, one OT slot from the projected depth + bias (or the fixed slot).
void EmitterSystem::Impl::collect_builtin(const Object& obj, const ViewParams& view, float alpha, u32& order, std::vector<DrawQuad>& out) const {
  Vec3 wpos;
  for (size_t i = 0; i < 3; ++i) {
    const double prev = static_cast<double>(obj.ppos[i]) / 65536.0, cur = static_cast<double>(obj.pos[i]) / 65536.0;
    wpos[i] = static_cast<float>(std::fabs(cur - prev) > 20000.0 ? cur : prev + (cur - prev) * alpha);
  }
  Vec3 vc = (obj.flags & kFlagViewSpace) ? wpos : view.rot.apply(wpos);
  if (obj.flags & kFlagCamRel) vc[2] += view.h * static_cast<float>(1 << cfg.pos_shift);
  else for (size_t i = 0; i < 3; ++i) vc[i] += view.trans[i];
  if (origin_off_screen(obj.flags, vc, view)) return;
  const Mat3 mc = (obj.flags & kFlagWorldQuad) ? view.rot * obj.mat : obj.mat;
  int rgb[3];
  for (size_t i = 0; i < 3; ++i) rgb[i] = (static_cast<int>(obj.col[i]) >> 4) & 0xFF;
  if (view.cue.on && (obj.flags & (1u << 17)) && view.cue.far > view.cue.near) {
    const float z = std::max(vc[2], 1.0f);
    const float cue_blend = std::clamp(view.cue.far * (z - view.cue.near) / ((view.cue.far - view.cue.near) * z), 0.0f, 1.0f);
    for (size_t i = 0; i < 3; ++i)
      rgb[i] = std::clamp(static_cast<int>(static_cast<float>(rgb[i]) + (view.cue.rgb[i] - static_cast<float>(rgb[i])) * cue_blend), 0, 255);
  }
  auto project = [&](const Vec3& local, float& sx, float& sy, float& sz) {
    const Vec3 offset = mc.apply(local);
    sz = vc[2] + offset[2];
    if (sz < 1.0f) return false;
    sx = view.ox + view.h * (vc[0] + offset[0]) / sz + view.scroll_x;
    sy = view.oy + view.h * (vc[1] + offset[1]) / sz + view.scroll_y;
    return true;
  };
  const int blend = static_cast<int>(obj.flags & 7);
  auto slot_of = [&](float z) { return std::clamp((obj.flags & kFlagFixedSlot) ? static_cast<int>(obj.bias) : (static_cast<int>(z) >> 4) + obj.bias, 0, 0xFFF); };
  if (obj.builtin == 1) {
    const Vec3 corner[4] = {{-2048, 2048, 0}, {2048, 2048, 0}, {-2048, -2048, 0}, {2048, -2048, 0}};
    DrawQuad quad;
    quad.untextured = 1;
    float z[4];
    for (int k = 0; k < 4; ++k)
      if (!project(corner[k], quad.x[k], quad.y[k], z[k])) return;
    // A flat that covers the whole PS1 frame (in the PS1's own projection) is a screen veil: it covers our whole picture too (the widescreen
    // columns, a wider FovDeg); the draw scissor clips it.
    const float ps1_scale = 1.0f / render::fov_scale();
    const float left = view.ox - 160.0f + view.scroll_x, top = view.oy - 112.0f + view.scroll_y;
    // Each corner must lie beyond a different corner of the frame.
    int corners_beyond = 0;
    for (int k = 0; k < 4; ++k) {
      const float px = view.ox + view.scroll_x + (quad.x[k] - view.ox - view.scroll_x) * ps1_scale, py = view.oy + view.scroll_y + (quad.y[k] - view.oy - view.scroll_y) * ps1_scale;
      const bool on_left = px <= left + 0.5f, on_right = px >= left + 319.5f, on_top = py <= top + 0.5f, on_bottom = py >= top + 223.5f;
      if ((on_left || on_right) && (on_top || on_bottom)) corners_beyond |= 1 << ((on_right ? 1 : 0) + (on_bottom ? 2 : 0));
    }
    if (corners_beyond == 0xF) {
      constexpr float kReach = 4096.0f;
      quad.x[0] = quad.x[2] = left - kReach, quad.x[1] = quad.x[3] = left + 320.0f + kReach;
      quad.y[0] = quad.y[1] = top - kReach, quad.y[2] = quad.y[3] = top + 224.0f + kReach;
    }
    quad.r = static_cast<u8>(rgb[0]);
    quad.g = static_cast<u8>(rgb[1]);
    quad.b = static_cast<u8>(rgb[2]);
    quad.blend = std::min(blend, 4);
    // The slot comes from the 4th vertex's depth.
    quad.slot = slot_of(z[3]);
    quad.order = order++;
    out.push_back(quad);
    return;
  }
  // line: one thin quad, colour falling linearly to black (Gouraud between the two ends); the tail is -vel >> (12 - pos_shift).
  const int ts = 12 - cfg.pos_shift;
  const Vec3 tail{-static_cast<float>(obj.vel[0] >> ts), -static_cast<float>(obj.vel[1] >> ts), -static_cast<float>(obj.vel[2] >> ts)};
  float ax, ay, az, bx, by, bz;
  if (!project({0, 0, 0}, ax, ay, az) || !project(tail, bx, by, bz)) return;
  const float dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
  // Half a pixel each side.
  const float nx = len > 1e-4f ? -dy / len * 0.5f : 0.5f, ny = len > 1e-4f ? dx / len * 0.5f : 0.0f;
  {
    DrawQuad quad;
    quad.untextured = 2;
    quad.x[0] = ax - nx, quad.y[0] = ay - ny, quad.x[1] = bx - nx, quad.y[1] = by - ny;
    quad.x[2] = ax + nx, quad.y[2] = ay + ny, quad.x[3] = bx + nx, quad.y[3] = by + ny;
    quad.r = static_cast<u8>(rgb[0]);
    quad.g = static_cast<u8>(rgb[1]);
    quad.b = static_cast<u8>(rgb[2]);
    // The tail vertex is black; the slot comes from the origin's depth.
    quad.r1 = quad.g1 = quad.b1 = 0;
    quad.blend = std::min(blend, 4);
    quad.slot = slot_of(az);
    quad.order = order++;
    out.push_back(quad);
  }
}

// Sprite type 21 (the lens flare): the object's position (world, or through its parent frame with
// flag bit 15) projected with the camera (RotTransPers); only when it lands in the 320 x 224 buffer (0 <= sx <= 320, 0 <= sy <= 224). Frame
// k = 1..N of its sheet (N = the frame count) is drawn flat on the screen (the frame's parts at their pixel offsets, colour
// 0x808080, additive blend, OT entry 1) at (sx, sy) + (k - 1) * ((320 - 2 sx) / N, (224 - 2 sy) / N) (20.12 steps), i.e. from the light towards
// its mirror point through the screen centre. If the object's colour bytes (colour >> 4) are not all 0, a full-screen POLY_F4
// (0,0)-(320,224) at OT entry 0, additive blend, channel c * (0x25400 - (dx^2 + dy^2)) / 0x25400 with integer steps
// d / (0x25400 / c), dx = 320 - 2 sx, dy = 224 - 2 sy (0x25400 = 320^2 + 224^2): brightest when the light is at the centre.
void EmitterSystem::Impl::collect_flare(const Object& obj, const ViewParams& view, float alpha, u32& order, std::vector<DrawQuad>& out) const {
  if (!obj.sheet || obj.sheet->frames.empty()) return;
  Vec3 wpos;
  for (size_t i = 0; i < 3; ++i) {
    const double prev = static_cast<double>(obj.ppos[i]) / 65536.0, cur = static_cast<double>(obj.pos[i]) / 65536.0;
    wpos[i] = static_cast<float>(std::fabs(cur - prev) > 20000.0 ? cur : prev + (cur - prev) * alpha);
  }
  if (obj.flags & kFlagRelParent) {
    const Header& header = headers[static_cast<size_t>(std::max(obj.owner, 0))];
    Basis basis;
    bool ok = false;
    if (header.has_anchor_op && header.anchor >= 0 && (hooks.anchor_basis_at || hooks.anchor_basis)) ok = anchor_at(header.anchor, alpha, basis);
    else if (header.has_basis_op && hooks.actor_basis) {
      int sc = 0;
      ok = hooks.actor_basis(header.basis_sel[0], header.basis_sel[1], header.basis_sel[2], header.basis_sel[3], basis, sc);
    }
    if (ok) {
      const Vec3 world = basis.m.apply(wpos);
      wpos = {world[0] + basis.t[0], world[1] + basis.t[1], world[2] + basis.t[2]};
    }
  }
  Vec3 vc = view.rot.apply(wpos);
  for (size_t i = 0; i < 3; ++i) vc[i] += view.trans[i];
  if (vc[2] < 1.0f) return;
  // The 320 x 224 buffer in layer space.
  const float ex = view.ox - 160.0f + view.scroll_x, ey = view.oy - 112.0f + view.scroll_y;
  // The light in the PS1's own frame (h without the [Video] FovDeg scale: the flare is a 320 x 224 / H 512 mechanism) gates and scales ONLY
  // the full-screen wash (the flash is brightest with the sun near the centre; a wider FOV / the widescreen columns bring the sun into OUR
  // picture where the PS1 frame never had it). The ghost sprites follow the sun in our own frame.
  const float h_ps1 = view.h / render::fov_scale();
  const int sx1 = static_cast<int>(std::floor(160.0f + h_ps1 * vc[0] / vc[2])), sy1 = static_cast<int>(std::floor(112.0f + h_ps1 * vc[1] / vc[2]));
  // The ghost discs follow the sun where it is actually drawn (the user's projection), on the line to the buffer centre as the PS1.
  const int sx = static_cast<int>(std::floor(160.0f + view.h * vc[0] / vc[2])), sy = static_cast<int>(std::floor(112.0f + view.h * vc[1] / vc[2]));
  const int extra = static_cast<int>(std::floor(render::wide_extra()));
  if (!lens_ghosts_visible(sx, sy, extra)) return;
  const assets::SpriteSheet& sh = *obj.sheet;
  const int frame_count = static_cast<int>(sh.frames.size());
  const int dx = 0x140 - 2 * sx, dy = 0xE0 - 2 * sy;
  const int stepx = dx * 0x1000 / frame_count, stepy = dy * 0x1000 / frame_count;
  int fx = sx << 12, fy = sy << 12;
  for (int k = 1; k <= frame_count; ++k) {
    const int px = static_cast<s16>((fx << 4) >> 16), py = static_cast<s16>((fy << 4) >> 16);
    const auto& frame = sh.frames[static_cast<size_t>(k - 1)];
    for (size_t j = 0; j < frame.size(); ++j) {
      const assets::EffectSprite& sp = frame[j];
      float x0 = static_cast<float>(px + sp.x), x1 = x0 + static_cast<float>(sp.w);
      float y0 = static_cast<float>(py + sp.y), y1 = y0 + static_cast<float>(sp.h);
      if (sp.mirror_x()) std::swap(x0, x1);
      if (sp.mirror_y()) std::swap(y0, y1);
      DrawQuad quad;
      quad.sprite = &sp;
      quad.sheet = &sh;
      quad.frame = k - 1;
      quad.sprite_index = static_cast<int>(j);
      quad.x[0] = quad.x[2] = ex + x0, quad.x[1] = quad.x[3] = ex + x1;
      quad.y[0] = quad.y[1] = ey + y0, quad.y[2] = quad.y[3] = ey + y1;
      quad.blend = 2;
      quad.slot = 1;
      quad.order = order++;
      out.push_back(quad);
    }
    fy += stepy;
    fx += stepx;
  }
  int rgb[3];
  for (size_t i = 0; i < 3; ++i) rgb[i] = (static_cast<int>(obj.col[i]) >> 4) & 0xFF;
  if (rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0) return;
  DrawQuad quad;
  quad.untextured = 1;
  // The wash covers the whole visible frame like the stage itself: the 320 columns plus the widescreen columns on both sides (the PS1 quad's
  // (0, 0)-(320, 224) in 4:3, where wide_extra() = 0); the draw scissor clips it to the picture.
  const float we = static_cast<float>(extra);
  quad.x[0] = quad.x[2] = ex - we, quad.x[1] = quad.x[3] = ex + 320.0f + we;
  quad.y[0] = quad.y[1] = ey, quad.y[2] = quad.y[3] = ey + 224.0f;
  quad.r = static_cast<u8>(lens_wash_channel(rgb[0], sx1, sy1));
  quad.g = static_cast<u8>(lens_wash_channel(rgb[1], sx1, sy1));
  quad.b = static_cast<u8>(lens_wash_channel(rgb[2], sx1, sy1));
  if (quad.r == 0 && quad.g == 0 && quad.b == 0) return;
  if (std::getenv("OC_FLARE_LOG")) std::fprintf(stderr, "lens flare: ps1 sx %d sy %d (ours %d %d) col %d %d %d -> flash %d %d %d (frames %d)\n", sx1, sy1, sx, sy, rgb[0], rgb[1], rgb[2], quad.r, quad.g, quad.b, frame_count);
  quad.blend = 2;
  quad.slot = 0;
  quad.order = order++;
  out.push_back(quad);
}

void EmitterSystem::Impl::collect(const ViewParams& view, float alpha, std::vector<DrawQuad>& out) const {
  u32 order = out.empty() ? 0 : out.back().order + 1;
  for (const Object& obj : objs) {
    // Drawn by its overlay (collect_tagged) / filtered out by OC_FX_ONLY.
    if (obj.poly_tag >= 0 || dev_skip(obj)) continue;
    if (obj.owner >= 0 && static_cast<size_t>(obj.owner) < headers.size() && headers[static_cast<size_t>(obj.owner)].cb_no_draw) continue;
    if (obj.alive && obj.stype == 15 && obj.builtin) {
      collect_builtin(obj, view, alpha, order, out);
      continue;
    }
    if (obj.alive && obj.stype == 21) {
      collect_flare(obj, view, alpha, order, out);
      continue;
    }
    if (!obj.alive || !obj.sheet || (obj.stype != 3 && obj.stype != 21)) continue;
    const assets::SpriteSheet& sh = *obj.sheet;
    int fidx = obj.frame;
    if (fidx == 0) continue;
    if (fidx < 0 || static_cast<size_t>(fidx) > sh.frames.size()) fidx = 1;
    if (sh.frames.empty()) continue;
    const auto& frame = sh.frames[static_cast<size_t>(fidx - 1)];
    if (frame.empty()) continue;

    auto lerp_s16 = [&](s16 prev, s16 cur) {
      return static_cast<float>(prev) + static_cast<float>(static_cast<s16>(cur - prev)) * alpha;
    };
    Vec3 wpos;
    for (size_t i = 0; i < 3; ++i) {
      const double prev = static_cast<double>(obj.ppos[i]) / 65536.0, cur = static_cast<double>(obj.pos[i]) / 65536.0;
      wpos[i] = static_cast<float>(std::fabs(cur - prev) > 20000.0 ? cur : prev + (cur - prev) * alpha);
    }
    Mat3 mat = obj.mat;
    if (alpha < 1.0f && !(obj.flags & kFlagFaceVelMask)) {
      const int a0 = static_cast<int>(std::lround(lerp_s16(obj.pang[0], obj.ang[0])));
      const int a1 = static_cast<int>(std::lround(lerp_s16(obj.pang[1], obj.ang[1])));
      const int a2 = static_cast<int>(std::lround(lerp_s16(obj.pang[2], obj.ang[2])));
      const float s0 = lerp_s16(obj.pscale[0], obj.scale[0]), s1 = lerp_s16(obj.pscale[1], obj.scale[1]), s2 = lerp_s16(obj.pscale[2], obj.scale[2]);
      mat = (a0 == 0 && a1 == 0 && a2 == 0) ? Mat3::identity() : Mat3::from_angles(a0, a1, a2, obj.flags & kFlagZyx);
      mat = mat.scaled_columns(s0 / 4096.0f, s1 / 4096.0f, s2 / 4096.0f);
    }
    int rgb[3];
    for (size_t i = 0; i < 3; ++i) {
      const float colour = static_cast<float>(obj.pcol[i]) + static_cast<float>(static_cast<s16>(obj.col[i] - obj.pcol[i])) * alpha;
      rgb[i] = (static_cast<int>(colour) >> 4) & 0xFF;
    }

    // Child expressed in a parent frame (flag bit 15), resolved live.
    if (obj.flags & kFlagRelParent) {
      const Header& header = headers[static_cast<size_t>(std::max(obj.owner, 0))];
      Basis basis;
      bool ok = false;
      if (header.has_anchor_op && header.anchor >= 0 && (hooks.anchor_basis_at || hooks.anchor_basis)) ok = anchor_at(header.anchor, alpha, basis);
      else if (header.has_basis_op && hooks.actor_basis) {
        int sc = 0;
        ok = hooks.actor_basis(header.basis_sel[0], header.basis_sel[1], header.basis_sel[2], header.basis_sel[3], basis, sc);
      }
      if (obj.flags & kFlagJointRot) trace_bit7("spr", header.nm(), header.has_anchor_op, header.has_basis_op, header.basis_sel, ok, basis.has_joint);
      if (ok) {
        const Vec3 world = basis.m.apply(wpos);
        wpos = {world[0] + basis.t[0], world[1] + basis.t[1], world[2] + basis.t[2]};
        // Flag bit 7 (see collect_meshes).
        if (basis.has_joint && (obj.flags & kFlagJointRot) && !header.has_anchor_op) mat = basis.joint * mat;
      }
    }

    Vec3 vc = (obj.flags & kFlagViewSpace) ? wpos : view.rot.apply(wpos);
    if (obj.flags & kFlagCamRel) vc[2] += view.h * static_cast<float>(1 << cfg.pos_shift);
    else for (size_t i = 0; i < 3; ++i) vc[i] += view.trans[i];
    if (origin_off_screen(obj.flags, vc, view)) continue;
    const Mat3 mc = (obj.flags & kFlagWorldQuad) ? view.rot * mat : mat;

    const int blend = static_cast<int>(obj.flags & 7);
    // Depth cue with the object's view depth.
    if (view.cue.on && (obj.flags & (1u << 17)) && view.cue.far > view.cue.near) {
      const float z = std::max(vc[2], 1.0f);
      const float cue_blend = std::clamp(view.cue.far * (z - view.cue.near) / ((view.cue.far - view.cue.near) * z), 0.0f, 1.0f);
      for (size_t i = 0; i < 3; ++i)
        rgb[i] = std::clamp(static_cast<int>(static_cast<float>(rgb[i]) + (view.cue.rgb[i] - static_cast<float>(rgb[i])) * cue_blend), 0, 255);
    }
    int slot = 0;
    for (size_t j = 0; j < frame.size(); ++j) {
      const assets::EffectSprite& sp = frame[j];
      // Corner = (sheet x, y) << pos_shift (3 field / title, 5 battle) in object space, then the object matrix and projection.
      const float ps = static_cast<float>(1 << cfg.pos_shift);
      const float x0 = static_cast<float>(sp.x) * ps, y0 = static_cast<float>(sp.y) * ps;
      const float x1 = x0 + static_cast<float>(sp.w) * ps, y1 = y0 + static_cast<float>(sp.h) * ps;
      // Top-left, top-right, bottom-left, bottom-right.
      const Vec3 corner[4] = {{x0, y0, 0}, {x1, y0, 0}, {x0, y1, 0}, {x1, y1, 0}};
      static constexpr int perm[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1}, {3, 2, 1, 0}};
      const int pi = sp.orient();
      DrawQuad quad;
      quad.sprite = &sp;
      quad.sheet = &sh;
      quad.cam[0] = vc[0], quad.cam[1] = vc[1], quad.cam[2] = vc[2];
      quad.frame = fidx - 1;
      quad.sprite_index = static_cast<int>(j);
      bool visible = true, drop_object = false;
      float zq[4];
      // The real picture's frame; bypass (debug free camera) = near-plane clip instead of the PS1 drop rules.
      const render::CullFrame cf = render::cull_frame(render::wide_extra());
      const bool bypass = render::cull_bypass();
      if (bypass) {
        Vec3 corners_cam[4];
        bool front[4];
        int nfront = 0;
        for (int k = 0; k < 4; ++k) {
          const Vec3 offset = mc.apply(corner[perm[pi][k]]);
          corners_cam[k] = {vc[0] + offset[0], vc[1] + offset[1], vc[2] + offset[2]};
          front[k] = corners_cam[k][2] >= render::kBypassNear;
          nfront += front[k] ? 1 : 0;
        }
        if (nfront == 0) visible = false;
        else {
          Vec3 clipped[4] = {corners_cam[0], corners_cam[1], corners_cam[2], corners_cam[3]};
          for (int k = 0; k < 4; ++k) {
            if (front[k]) continue;
            // A quad-edge neighbour in front, else any.
            int neighbour = front[k ^ 1] ? (k ^ 1) : front[k ^ 2] ? (k ^ 2) : -1;
            for (int other = 0; neighbour < 0 && other < 4; ++other)
              if (front[other]) neighbour = other;
            const float clip_t = (render::kBypassNear - corners_cam[neighbour][2]) / (corners_cam[k][2] - corners_cam[neighbour][2]);
            for (int axis = 0; axis < 3; ++axis) clipped[k][axis] = corners_cam[neighbour][axis] + (corners_cam[k][axis] - corners_cam[neighbour][axis]) * clip_t;
          }
          for (int k = 0; k < 4; ++k) {
            zq[k] = clipped[k][2];
            quad.x[k] = view.ox + view.h * clipped[k][0] / clipped[k][2] + view.scroll_x;
            quad.y[k] = view.oy + view.h * clipped[k][1] / clipped[k][2] + view.scroll_y;
          }
        }
      } else
      {
      for (int k = 0; k < 4; ++k) {
        const Vec3 offset = mc.apply(corner[perm[pi][k]]);
        const float vx = vc[0] + offset[0], vy = vc[1] + offset[1], vz = vc[2] + offset[2];
        zq[k] = vz;
        // Only the last vertex (perm[pi][3]) decides the packet: on a GTE overflow (large coordinates, depth outside 0..65535,
        // H >= 2 * depth, or the screen position outside -1024..1023) the packet is dropped and
        // the object's remaining sprites with it ('break'). The first three vertices only saturate the screen position to -1024..1023 (kept below).
        // The GPU then skips a triangle whose vertices are > 1023 apart in x or > 511 in y (native PS1 pixels).
        // Without this a floor quad with a corner just in front of the camera became a giant magnified sliver.
        if (vz < 1.0f) {
          if (k == 3) drop_object = true;
          visible = false;
          break;
        }
        if (k == 3 && (std::fabs(vx) > 32767.0f || std::fabs(vy) > 32767.0f || vz > 65535.0f || view.h >= 2.0f * vz)) drop_object = true;
        const float sx = view.ox + view.h * vx / vz, sy = view.oy + view.h * vy / vz;
        if (k == 3 && !render::cull_screen_ok(cf, sx, sy)) drop_object = true;
        quad.x[k] = std::clamp(sx, -1024.0f, 1023.0f) + view.scroll_x;
        quad.y[k] = std::clamp(sy, -1024.0f, 1023.0f) + view.scroll_y;
      }
      }
      if (drop_object) break;
      if (visible && !bypass) {
        auto tri_ok = [&](int first, int second, int third) {
          const float xs[3] = {quad.x[first] - view.scroll_x, quad.x[second] - view.scroll_x, quad.x[third] - view.scroll_x};
          const float ys[3] = {quad.y[first] - view.scroll_y, quad.y[second] - view.scroll_y, quad.y[third] - view.scroll_y};
          return render::cull_span_ok(cf, std::max({xs[0], xs[1], xs[2]}) - std::min({xs[0], xs[1], xs[2]}),
                                      std::max({ys[0], ys[1], ys[2]}) - std::min({ys[0], ys[1], ys[2]}));
        };
        if (!tri_ok(0, 1, 2) || !tri_ok(2, 1, 3)) visible = false;
      }
      if (j == 0) {
        if (obj.flags & kFlagFixedSlot) slot = obj.bias;
        else slot = (static_cast<int>(visible ? zq[2] : vc[2]) >> 4) + obj.bias;
        slot = std::clamp(slot, 0, 0xFFF);
      }
      if (!visible) continue;
      quad.slot = slot;
      quad.r = static_cast<u8>(rgb[0]);
      quad.g = static_cast<u8>(rgb[1]);
      quad.b = static_cast<u8>(rgb[2]);
      // Blend 5 / 6: the frame is submitted twice (add + subtract).
      if (blend >= 5) {
        DrawQuad add_quad = quad, sub_quad = quad;
        add_quad.blend = 2;
        sub_quad.blend = 3;
        // Blend 5: add submitted first, drawn last.
        if (blend == 5) { add_quad.order = order++; sub_quad.order = order++; }
        else { sub_quad.order = order++; add_quad.order = order++; }
        out.push_back(add_quad);
        out.push_back(sub_quad);
      } else {
        quad.blend = blend;
        quad.order = order++;
        out.push_back(quad);
      }
    }
  }
}

}  // namespace oc::effects
