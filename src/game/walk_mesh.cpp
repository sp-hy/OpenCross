#include "game/walk_mesh.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace oc::game {

namespace {
// The original also bounds a move at 64 triangle crossings.
constexpr int kMaxSteps = 64;
constexpr float kEps = 1e-3f;

// The sum of squares reduced to its eight leading bits: `lead` in 64..255 and the even count of leading zero bits it was found at.
void leading_bits(u32 value, u32& lead, int& zeros) {
  zeros = std::countl_zero(value) & ~1;
  lead = zeros >= 24 ? value << (zeros - 24) : value >> (24 - zeros);
}
}  // namespace

int mover_sqrt(int value) {
  if (value <= 0) return 0;
  u32 lead;
  int zeros;
  leading_bits(static_cast<u32>(value), lead, zeros);
  const int root = static_cast<int>(512.0 * std::sqrt(static_cast<double>(lead)));
  return (root << ((31 - zeros) >> 1)) >> 12;
}

void mover_normalise(const int in[3], int out[3]) {
  const u32 sum = static_cast<u32>(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
  if (sum == 0) {
    out[0] = out[1] = out[2] = 0;
    return;
  }
  u32 lead;
  int zeros;
  leading_bits(sum, lead, zeros);
  const int scale = static_cast<int>(32768.0 / std::sqrt(static_cast<double>(lead)));
  for (int i = 0; i < 3; ++i) out[i] = (in[i] * scale) >> ((31 - zeros) >> 1);
}

void WalkMesh::init(const assets::WalkMesh& mesh, std::vector<u32> attr_flags) {
  mesh_ = mesh;
  tris_ = mesh.triangles;
  attr_flags_ = std::move(attr_flags);
}

u32 WalkMesh::flags(int tri) const {
  if (tri < 0 || static_cast<size_t>(tri) >= tris_.size()) return 0;
  const s16 attr = tris_[static_cast<size_t>(tri)].attr;
  return attr >= 0 && static_cast<size_t>(attr) < attr_flags_.size() ? attr_flags_[static_cast<size_t>(attr)] : 0;
}

float WalkMesh::orientation(int tri) const {
  const auto& triangle = tris_[static_cast<size_t>(tri)];
  const auto& v0 = mesh_.vertices[triangle.v[0]];
  const auto& v1 = mesh_.vertices[triangle.v[1]];
  const auto& v2 = mesh_.vertices[triangle.v[2]];
  return (v1[0] - v0[0]) * (v2[2] - v0[2]) - (v1[2] - v0[2]) * (v2[0] - v0[0]);
}

// Triangles are not necessarily counter-clockwise, so the side test is normalised by the orientation.
float WalkMesh::edge_side(int tri, int edge, float x, float z) const {
  const auto& triangle = tris_[static_cast<size_t>(tri)];
  const auto& va = mesh_.vertices[triangle.v[static_cast<size_t>(edge)]];
  const auto& vb = mesh_.vertices[triangle.v[static_cast<size_t>((edge + 1) % 3)]];
  const float cross = (vb[0] - va[0]) * (z - va[2]) - (vb[2] - va[2]) * (x - va[0]);
  return orientation(tri) < 0 ? -cross : cross;
}

bool WalkMesh::contains(int tri, float x, float z) const {
  if (tri < 0 || static_cast<size_t>(tri) >= tris_.size()) return false;
  for (int k = 0; k < 3; ++k)
    if (edge_side(tri, k, x, z) < -kEps) return false;
  return true;
}

void WalkMesh::plane_normal(int tri, float& nx, float& ny, float& nz) const {
  const auto& triangle = tris_[static_cast<size_t>(tri)];
  const auto& v0 = mesh_.vertices[triangle.v[0]];
  const auto& v1 = mesh_.vertices[triangle.v[1]];
  const auto& v2 = mesh_.vertices[triangle.v[2]];
  const float ax = v1[0] - v0[0], ay = v1[1] - v0[1], az = v1[2] - v0[2];
  const float bx = v2[0] - v0[0], by = v2[1] - v0[1], bz = v2[2] - v0[2];
  nx = ay * bz - az * by;
  ny = az * bx - ax * bz;
  nz = ax * by - ay * bx;
}

float WalkMesh::height(int tri, float x, float z) const {
  const auto& v0 = mesh_.vertices[tris_[static_cast<size_t>(tri)].v[0]];
  float nx, ny, nz;
  plane_normal(tri, nx, ny, nz);
  if (std::fabs(ny) < 1e-6f) return v0[1];
  return v0[1] + (-(nz * (z - v0[2])) - nx * (x - v0[0])) / ny;
}

int WalkMesh::floor_height(int tri, float x, float z) const {
  const auto& v0 = mesh_.vertices[tris_[static_cast<size_t>(tri)].v[0]];
  float nx, ny, nz;
  plane_normal(tri, nx, ny, nz);
  const int base = static_cast<int>(v0[1]);
  if (std::fabs(ny) < 1e-6f) return base;
  return base + static_cast<int>((-(nz * (std::floor(z) - v0[2])) - nx * (std::floor(x) - v0[0])) / ny);
}

bool WalkMesh::normal_xz(int tri, float& nx, float& nz) const {
  if (tri < 0 || static_cast<size_t>(tri) >= tris_.size()) return false;
  float ny;
  plane_normal(tri, nx, ny, nz);
  return true;
}

bool WalkMesh::normal_up(int tri, float normal[3]) const {
  if (tri < 0 || static_cast<size_t>(tri) >= tris_.size()) return false;
  plane_normal(tri, normal[0], normal[1], normal[2]);
  const float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
  if (len < 1e-6f) return false;
  const float scale = (normal[1] < 0 ? -1.0f : 1.0f) / len;
  normal[0] *= scale;
  normal[1] *= scale;
  normal[2] *= scale;
  return true;
}

int WalkMesh::find_triangle(float x, float z, float y_hint) const {
  int best = -1;
  float best_dist = std::numeric_limits<float>::max();
  for (size_t i = 0; i < tris_.size(); ++i) {
    if (!contains(static_cast<int>(i), x, z)) continue;
    const float dist = std::fabs(height(static_cast<int>(i), x, z) - y_hint);
    if (dist < best_dist) {
      best_dist = dist;
      best = static_cast<int>(i);
    }
  }
  return best;
}

int WalkMesh::find_triangle_level(float x, float z, int index) const {
  std::vector<std::pair<float, int>> hit;
  for (size_t i = 0; i < tris_.size() && hit.size() < 8; ++i)
    if (contains(static_cast<int>(i), x, z)) hit.emplace_back(height(static_cast<int>(i), x, z), static_cast<int>(i));
  if (hit.empty()) return -1;
  std::stable_sort(hit.begin(), hit.end(), [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
  return hit[index >= 0 && static_cast<size_t>(index) < hit.size() ? static_cast<size_t>(index) : 0].second;
}

std::array<float, 2> WalkMesh::centroid(int tri) const {
  const auto& triangle = tris_[static_cast<size_t>(tri)];
  float x = 0, z = 0;
  for (u16 vertex : triangle.v) {
    x += mesh_.vertices[vertex][0];
    z += mesh_.vertices[vertex][2];
  }
  return {x / 3, z / 3};
}

int WalkMesh::nearest_vertex(float x, float z, u32 class_mask, int* tri_out) const {
  int best = -1, best_tri = -1;
  float best_dist2 = std::numeric_limits<float>::max();
  for (size_t i = 0; i < tris_.size(); ++i) {
    if (blocked(static_cast<int>(i), class_mask)) continue;
    for (u16 vertex : tris_[i].v) {
      const float dx = mesh_.vertices[vertex][0] - x, dz = mesh_.vertices[vertex][2] - z;
      const float dist2 = dx * dx + dz * dz;
      if (dist2 < best_dist2) {
        best_dist2 = dist2;
        best = vertex;
        best_tri = static_cast<int>(i);
      }
    }
  }
  if (tri_out) *tri_out = best_tri;
  return best;
}

// Each step crosses the edge the target is furthest outside of; a boundary or blocked neighbour stops the walk just before
// that edge along the move. Out of steps: the triangle reached is kept.
bool WalkMesh::walk(int& tri, float& x, float& z, float tx, float tz, u32 class_mask, int* wall_edge) const {
  const float sx = x, sz = z;
  for (int step = 0; step < kMaxSteps; ++step) {
    int worst = -1;
    float worst_side = -kEps;
    for (int k = 0; k < 3; ++k) {
      const float side = edge_side(tri, k, tx, tz);
      if (side < worst_side) {
        worst_side = side;
        worst = k;
      }
    }
    if (worst < 0) {
      x = tx;
      z = tz;
      return true;
    }
    const int next = tris_[static_cast<size_t>(tri)].adj[static_cast<size_t>(worst)];
    if (next < 0 || static_cast<size_t>(next) >= tris_.size() || blocked(next, class_mask)) {
      const float s0 = edge_side(tri, worst, sx, sz), s1 = edge_side(tri, worst, tx, tz);
      const float fraction = (s0 - s1) > 1e-9f ? std::clamp(s0 / (s0 - s1), 0.0f, 1.0f) : 0.0f;
      x = sx + (tx - sx) * fraction * 0.999f;
      z = sz + (tz - sz) * fraction * 0.999f;
      if (!contains(tri, x, z)) {
        x = sx;
        z = sz;
      }
      if (wall_edge) *wall_edge = worst;
      return false;
    }
    tri = next;
  }
  x = tx;
  z = tz;
  return true;
}

// Wall slide: the remaining motion is projected onto the blocking edge and walked once more.
WalkMesh::Move WalkMesh::move(int tri, float x, float z, float dx, float dz, u32 class_mask) const {
  Move result;
  result.tri = tri;
  result.x = x;
  result.z = z;
  if (tri < 0 || static_cast<size_t>(tri) >= tris_.size()) return result;
  int wall = -1;
  const float tx = x + dx, tz = z + dz;
  if (walk(result.tri, result.x, result.z, tx, tz, class_mask, &wall)) return result;
  result.blocked = true;
  const auto& triangle = tris_[static_cast<size_t>(result.tri)];
  const auto& va = mesh_.vertices[triangle.v[static_cast<size_t>(wall)]];
  const auto& vb = mesh_.vertices[triangle.v[static_cast<size_t>((wall + 1) % 3)]];
  float ex = vb[0] - va[0], ez = vb[2] - va[2];
  const float len = std::hypot(ex, ez);
  if (len < 1e-6f) return result;
  ex /= len;
  ez /= len;
  const float along = (tx - result.x) * ex + (tz - result.z) * ez;
  int wall2 = -1;
  walk(result.tri, result.x, result.z, result.x + ex * along, result.z + ez * along, class_mask, &wall2);
  return result;
}

// The engine's mover probes the destination with three feelers of `standoff` units: at heading -45 deg, +45 deg and straight
// ahead (in that order); the first blocked one gives the edge the slide projects onto. A diagonal feeler catches a convex wall
// corner beside the path that the single probe along the move passes. Only the first attempt has them; the chained slides keep
// the single probe along the slid move.
// A slid move that meets the next wall of a bend (an obtuse corner between two wall edges) is slid once more along that edge,
// up to 3 times, so the actor follows the wall round the corner instead of wedging in it. A chained slide that runs back
// against the push is a concave corner (two walls facing the move): the move is rejected and the actor stands, instead of
// flipping between the two edges every tick.
// The first slide decides head-on and the side from the heading, not from the move: a script walk keeps the heading of its start
// while its velocity re-aims at the target each tick, so a walker sliding along a wall at a shallow angle to its target goes on
// sliding to the wall's end instead of stopping once the live direction turns head-on.
WalkMesh::Move WalkMesh::move_actor(int tri, float x, float z, float dx, float dz, u32 class_mask, float standoff,
                                    float head_on, float hx, float hz) const {
  Move result;
  result.tri = tri;
  result.x = x;
  result.z = z;
  const float len = std::hypot(dx, dz);
  if (tri < 0 || static_cast<size_t>(tri) >= tris_.size() || len < 1e-6f) return result;

  int fail_tri = -1, fail_edge = -1;
  auto try_move = [&](float mx, float mz, bool record, float stand) {
    const float move_len = std::hypot(mx, mz);
    int probe_tri = tri;
    float px = x, pz = z;
    int edge = -1;
    const float extra = stand / move_len;
    if (walk(probe_tri, px, pz, x + mx * (1 + extra), z + mz * (1 + extra), class_mask, &edge)) return true;
    if (record) {
      fail_tri = probe_tri;
      fail_edge = edge;
    }
    return false;
  };

  auto commit = [&](float mx, float mz) {
    result.tri = tri;
    result.x = x;
    result.z = z;
    walk(result.tri, result.x, result.z, x + mx, z + mz, class_mask, nullptr);
  };

  const float ux = dx / len, uz = dz / len;
  const bool own_heading = hx != 0.0f || hz != 0.0f;
  constexpr float kHalfSqrt2 = 0.70710678f;
  const float fx[2] = {(ux - uz) * kHalfSqrt2, (ux + uz) * kHalfSqrt2};
  const float fz[2] = {(ux + uz) * kHalfSqrt2, (uz - ux) * kHalfSqrt2};
  bool clear = true;
  for (int feeler = 0; feeler < 2 && clear; ++feeler) {
    int probe_tri = tri, edge = -1;
    float px = x, pz = z;
    if (!walk(probe_tri, px, pz, x + dx + fx[feeler] * standoff, z + dz + fz[feeler] * standoff, class_mask, &edge)) {
      clear = false;
      fail_tri = probe_tri;
      fail_edge = edge;
    }
  }
  if (clear && try_move(dx, dz, true, standoff)) {
    commit(dx, dz);
    return result;
  }
  result.blocked = true;
  float cx = dx, cz = dz;
  for (int pass = 0; pass < 3; ++pass) {
    if (fail_tri < 0 || fail_edge < 0) return result;
    const auto& triangle = tris_[static_cast<size_t>(fail_tri)];
    const auto& va = mesh_.vertices[triangle.v[static_cast<size_t>(fail_edge)]];
    const auto& vb = mesh_.vertices[triangle.v[static_cast<size_t>((fail_edge + 1) % 3)]];
    float ex = vb[0] - va[0], ez = vb[2] - va[2];
    const float edge_len = std::hypot(ex, ez);
    if (edge_len < 1e-6f) return result;
    ex /= edge_len;
    ez /= edge_len;
    const float along = pass == 0 && own_heading ? hx * ex + hz * ez : (cx * ex + cz * ez) / len;
    if (std::fabs(along) < std::sin(head_on)) return result;
    const float sgn = along < 0 ? -1.0f : 1.0f;
    if (pass > 0 && (ex * sgn * dx + ez * sgn * dz) <= 0) return result;
    const float sx = ex * sgn * len, sz = ez * sgn * len;
    fail_tri = fail_edge = -1;
    if (try_move(sx, sz, true, standoff)) {
      commit(sx, sz);
      result.slid = true;
      result.slide_dx = sx;
      result.slide_dz = sz;
      return result;
    }
    cx = sx;
    cz = sz;
  }
  return result;
}

}  // namespace oc::game
