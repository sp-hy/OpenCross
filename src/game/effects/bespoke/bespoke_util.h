#pragma once
// Internal helpers shared by the bespoke modules.
#include <algorithm>
#include <cmath>

#include "game/effects/bespoke/bespoke.h"

namespace oc::effects::util {

inline u8 clamp8(float value) { return static_cast<u8>(std::clamp(value, 0.0f, 255.0f)); }
inline u8 clamp8i(int value) { return static_cast<u8>(std::clamp(value, 0, 255)); }

inline FxPrim rect_prim(float x0, float y0, float x1, float y1, u8 r, u8 g, u8 b, int blend, int slot = 0) {
  FxPrim prim;
  prim.kind = FxPrim::Kind::Quad;
  const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
  for (int i = 0; i < 4; ++i) {
    prim.v[i].x = xs[i];
    prim.v[i].y = ys[i];
    prim.v[i].r = r;
    prim.v[i].g = g;
    prim.v[i].b = b;
  }
  prim.blend = blend;
  prim.slot = slot;
  return prim;
}

// The NEGAPOSI screen negative that seven overlays share: the frame is copied into a 0x1C0,0x100 VRAM page and
// redrawn as 5 columns of 0x40 px, then a white quad subtracts it; net result = inverted backdrop for the lifetime of the task.
// Task word [0] is a countdown in ticks; the task frees itself on the tick it goes negative (no draw that tick).
struct NegativeStrip {
  int columns = 5, col_w = 64, height = 240;
  int remaining = -1;  // < 0 = inactive
  bool active() const { return remaining >= 0; }
  void start(int ticks) { remaining = std::max(ticks, 0); }
  void tick() {
    if (remaining >= 0) --remaining;
  }
  void draw(std::vector<FxPrim>& out) const {
    if (remaining < 0) return;
    // One quad over the columns' whole width (the same picture: the invert never overlaps itself), so the battle's draw widens it over
    // the widescreen columns like the other full-width washes (Letterbox::widen_full_width only widens a quad spanning 0..320).
    out.push_back(rect_prim(0, 0, static_cast<float>(columns * col_w), static_cast<float>(height), 255, 255, 255, kBlendInvert, 0));
  }
};

// World-space mesh -> Tri prims (two-sided). `world[i]`/`col[i]` per vertex; faces with a vertex behind the camera are skipped.
inline void emit_mesh(const ViewParams& view, const FxMesh& mesh, const std::vector<Vec3>& world, const std::vector<std::array<u8, 3>>& colours,
                      int blend, std::vector<FxPrim>& out, u32 order_base = 0) {
  for (size_t face = 0; face < mesh.tris.size(); ++face) {
    FxPrim prim;
    prim.kind = FxPrim::Kind::Tri;
    prim.blend = blend;
    float zs = 0;
    bool ok = true;
    for (int k = 0; k < 3 && ok; ++k) {
      const size_t vi = mesh.tris[face][static_cast<size_t>(k)];
      float z;
      ok = fx_project(view, world[vi], prim.v[k].x, prim.v[k].y, z);
      zs += z;
      prim.v[k].r = colours[vi][0];
      prim.v[k].g = colours[vi][1];
      prim.v[k].b = colours[vi][2];
    }
    if (!ok) continue;
    prim.slot = fx_slot_of_depth(zs / 3.0f);
    prim.order = order_base + static_cast<u32>(face);
    out.push_back(prim);
  }
}

// Additive soft sparkle / debris point as a small quad.
inline void emit_point(const ViewParams& view, const Vec3& point, float half, u8 r, u8 g, u8 b, int blend, std::vector<FxPrim>& out) {
  float sx, sy, z;
  if (!fx_project(view, point, sx, sy, z)) return;
  const float size = std::max(1.0f, half * view.h / std::max(z, 64.0f));
  FxPrim prim = rect_prim(sx - size, sy - size, sx + size, sy + size, r, g, b, blend, fx_slot_of_depth(z));
  out.push_back(prim);
}

inline Vec3 add(const Vec3& lhs, const Vec3& rhs) { return {lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2]}; }
inline Vec3 sub(const Vec3& lhs, const Vec3& rhs) { return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]}; }
inline Vec3 mul(const Vec3& vec, float factor) { return {vec[0] * factor, vec[1] * factor, vec[2] * factor}; }
inline float len(const Vec3& vec) { return std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]); }
inline Vec3 lerp(const Vec3& from, const Vec3& to, float amount) { return add(from, mul(sub(to, from), amount)); }

// Option accessor for an operand list.
inline int opnd(const int* args, int count, int i, int def = 0) { return i < count ? args[i] : def; }

}  // namespace oc::effects::util
