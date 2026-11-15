// Ring / tube shaped untextured effect meshes (a loop of quads sharing edges, like the field
// contact ring `enc1`) are drawn with extra segments along the loop at high resolution. The PS1 data is unchanged; the original vertices stay.
#pragma once
#include <functional>
#include <vector>

namespace oc::effects {

// `p` = position (object space for meshes, z = 0 for screen quads), `c` = colour.
struct SmoothVert {
  float p[3];
  float c[3];
};
// One quad in loop order (v0 -> v1 -> v2 -> v3 around the perimeter; v0v1, v1v2, v2v3, v3v0 are its edges). `tag` is the caller's id
// (index of the source primitive); sub-quads inherit it.
struct SmoothQuad {
  SmoothVert v[4];
  int tag = 0;
};

// Projects a point to screen pixels; false = not drawable (the ring is then left alone).
using SmoothProject = std::function<bool(const float point[3], float xy[2])>;

constexpr int kMaxRingSegments = 8;

// Finds closed quad strips in `quads` (every quad of the strip shares exactly two opposite edges with the next / previous quad and the chain
// closes). Each is replaced by N sub-quads per source quad on a Catmull-Rom curve through the original vertices along both rails (colours
// linearly interpolated); N = ceil(longest rail segment in pixels / max_px), 1..kMaxRingSegments. Everything else is kept unchanged and in
// order. A ring whose rail is shared with a primitive outside the refined rings (a fan, a plain quad or `foreign_tris`, a triangle list of the
// rest of the mesh) is left alone, as is any ring sharing a rail with such a ring: the curve would otherwise open a crack. Returns true when a
// ring was refined.
bool smooth_quad_rings(std::vector<SmoothQuad>& quads, float max_px, const SmoothProject& project,
                       const std::vector<SmoothVert>* foreign_tris = nullptr);

// Same for a triangle list (3 vertices per triangle): consecutive triangle pairs forming a quad are merged for the detection. Unpaired
// triangles come first, then every quad as two triangles (refined or not). Returns true (and rewrites the list) only when a ring was refined.
bool smooth_triangle_rings(std::vector<SmoothVert>& tri_verts, float max_px, const SmoothProject& project);

}  // namespace oc::effects
