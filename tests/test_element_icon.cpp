// Element colour icons: polygon triangulation (area kept for convex and concave outlines, either winding), every colour's icon fits its
// square and is lit or dimmed as asked, the simple shapes are smaller than the full ones, and the symbols switch starts off.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/element_icon.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

float polygon_area(const std::vector<std::array<float, 2>>& points) {
  float area = 0.0f;
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& from = points[i];
    const auto& to = points[(i + 1) % points.size()];
    area += from[0] * to[1] - to[0] * from[1];
  }
  return std::fabs(area) * 0.5f;
}

float triangles_area(const std::vector<std::array<float, 2>>& points, const std::vector<std::array<int, 3>>& triangles) {
  float area = 0.0f;
  for (const auto& tri : triangles) {
    const auto& a = points[static_cast<size_t>(tri[0])];
    const auto& b = points[static_cast<size_t>(tri[1])];
    const auto& c = points[static_cast<size_t>(tri[2])];
    area += std::fabs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * 0.5f;
  }
  return area;
}

bool inside_square(const IconTriangle& tri, float cx, float cy, float half) {
  for (size_t corner = 0; corner < 3; ++corner)
    if (std::fabs(tri.xy[corner * 2] - cx) > half + 1e-3f || std::fabs(tri.xy[corner * 2 + 1] - cy) > half + 1e-3f) return false;
  return true;
}

float covered_area(const std::vector<IconTriangle>& triangles) {
  float area = 0.0f;
  for (const IconTriangle& tri : triangles)
    area += std::fabs((tri.xy[2] - tri.xy[0]) * (tri.xy[5] - tri.xy[1]) - (tri.xy[3] - tri.xy[1]) * (tri.xy[4] - tri.xy[0])) * 0.5f;
  return area;
}
}  // namespace

int main() {
  const std::vector<std::array<float, 2>> bolt = {{0.34f, -1.0f}, {-0.52f, 0.14f}, {-0.04f, 0.14f}, {-0.32f, 1.0f}, {0.54f, -0.22f}, {0.06f, -0.22f}, {0.58f, -1.0f}};
  std::vector<std::array<float, 2>> reversed(bolt.rbegin(), bolt.rend());
  const std::vector<std::array<int, 3>> forward_tris = triangulate_polygon(bolt), backward_tris = triangulate_polygon(reversed);
  CHECK(forward_tris.size() == bolt.size() - 2 && backward_tris.size() == bolt.size() - 2);
  CHECK(std::fabs(triangles_area(bolt, forward_tris) - polygon_area(bolt)) < 1e-4f);
  CHECK(std::fabs(triangles_area(reversed, backward_tris) - polygon_area(reversed)) < 1e-4f);
  const std::vector<std::array<float, 2>> ell = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
  CHECK(std::fabs(triangles_area(ell, triangulate_polygon(ell)) - 3.0f) < 1e-5f);
  CHECK(triangulate_polygon({{0, 0}, {1, 1}}).empty());

  CHECK(!element_symbols());
  set_element_symbols(true);
  CHECK(element_symbols());
  set_element_symbols(false);

  const float cx = 100.0f, cy = 50.0f, size = 40.0f;
  for (int colour = 0; colour < kElementIconCount; ++colour) {
    std::vector<IconTriangle> full, simple, dim;
    element_icon(colour, cx, cy, size, 40.0f, full);
    element_icon(colour, cx, cy, size, 10.0f, simple);
    element_icon(colour, cx, cy, size, 40.0f, dim, 64);
    CHECK(!full.empty() && !simple.empty() && full.size() == dim.size());
    CHECK(simple.size() <= full.size());
    for (const IconTriangle& tri : full) CHECK(inside_square(tri, cx, cy, size * 0.5f));
    for (const IconTriangle& tri : simple) CHECK(inside_square(tri, cx, cy, size * 0.5f));
    CHECK(full.front().colour != dim.front().colour);
    CHECK((full.front().colour >> 24) == 0xFF && (dim.front().colour >> 24) == 0xFF);
    CHECK(covered_area(full) > size * size * 0.15f);
  }
  std::vector<IconTriangle> none;
  element_icon(-1, cx, cy, size, 40.0f, none);
  element_icon(kElementIconCount, cx, cy, size, 40.0f, none);
  CHECK(none.empty());

  std::vector<IconTriangle> flame_full, flame_simple;
  element_icon(3, cx, cy, size, 40.0f, flame_full);
  element_icon(3, cx, cy, size, 10.0f, flame_simple);
  CHECK(flame_simple.size() < flame_full.size());

  if (failures == 0) std::printf("element_icon: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
