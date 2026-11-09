#pragma once
// Walk-mesh movement helper: point-in-triangle tests in XZ, moving across `adj` links, wall flags from the attr -> flags
// table, height from the triangle plane. Pure maths, no GPU dependencies.
#include <array>
#include <span>
#include <vector>

#include "assets/room_scene.h"

namespace oc::game {

// Flag-word bits of the attr table that matter for blocking.
// ClassA blocks actors whose class bits match; BlockAll blocks everyone.
constexpr u32 kFlagBlockClassA = 0x200000;
constexpr u32 kFlagBlockClassB = 0x400000;
constexpr u32 kFlagBlockAll = 0x800000;

// The integer square root and vector normalisation of the engine's mover. Both look at the eight leading bits of the sum of
// squares only (shifted by an even count into 64..255), so they are a little coarse: mover_sqrt(962) is 30, and a unit vector
// can come out up to about 0.5 % long.
int mover_sqrt(int value);
// `in` scaled to length 4096 (4.12); zero stays zero.
void mover_normalise(const int in[3], int out[3]);

class WalkMesh {
 public:
  void init(const assets::WalkMesh& mesh, std::vector<u32> attr_flags);
  // The script-edited flag table.
  void set_attr_flags(std::span<const u32> flags) { attr_flags_.assign(flags.begin(), flags.end()); }
  bool empty() const { return tris_.empty(); }
  size_t triangle_count() const { return tris_.size(); }
  const assets::WalkMesh& mesh() const { return mesh_; }

  // Flag word of a triangle (0 when the attr is outside the table).
  u32 flags(int tri) const;
  // `class_mask` = the mover class bits (0x200000 / 0x400000); a triangle blocks if flags & class_mask or kFlagBlockAll.
  bool blocked(int tri, u32 class_mask) const {
    return (flags(tri) & ((class_mask & (kFlagBlockClassA | kFlagBlockClassB)) | kFlagBlockAll)) != 0;
  }

  bool contains(int tri, float x, float z) const;
  // Plane height at (x, z) of triangle `tri` (Y down); the first vertex height if the plane is vertical.
  float height(int tri, float x, float z) const;
  // The engine's whole-unit floor height: the plane at (floor x, floor z), its offset from the first vertex truncated toward zero.
  int floor_height(int tri, float x, float z) const;
  // Horizontal part (Nx, Nz) of the plane normal N = (v1-v0) x (v2-v0) (the same N the height uses). On a stair / ladder tile
  // the engine turns the actor toward the engine direction (cos h, -sin h) = (Nx, Nz), i.e. uphill. False for a bad index.
  bool normal_xz(int tri, float& nx, float& nz) const;
  // The unit plane normal of `tri`, flipped to point up (+y); false for a bad index or a degenerate triangle.
  bool normal_up(int tri, float normal[3]) const;
  // Containing triangle whose plane height is nearest `y_hint`, or -1.
  int find_triangle(float x, float z, float y_hint) const;
  // Up to 8 triangles containing (x, z) sorted by plane Y descending (stable); element `index`, 0 if out of range.
  int find_triangle_level(float x, float z, int index) const;
  // Index of the vertex nearest (x, z) that belongs to a non-blocked triangle, or -1; `tri` gets such a triangle.
  int nearest_vertex(float x, float z, u32 class_mask, int* tri = nullptr) const;
  std::array<float, 2> centroid(int tri) const;

  struct Move {
    int tri = -1;
    float x = 0, z = 0;
    // The full move was stopped by a boundary / wall (a slide may still have moved).
    bool blocked = false;
    // move_actor: the move was projected onto the blocking edge; slide_dx/dz is then the slid move vector (full length).
    bool slid = false;
    float slide_dx = 0, slide_dz = 0;
  };
  // Moves from (x, z) inside `tri` by (dx, dz): crosses edges through `adj`, stops at boundaries and blocked
  // triangles and slides along the blocking edge with the remaining motion.
  Move move(int tri, float x, float z, float dx, float dz, u32 class_mask) const;

  // Actor step: the move (dx, dz) is accepted if the point `standoff` units ahead of its end can be reached through the
  // mesh; otherwise it is projected onto the blocking edge at FULL speed (cancelled when nearly head-on, within `head_on`
  // radians of the edge normal) and the slid move is tried once more, at its end point alone. The head-on test and the slide's side are taken from the
  // actor's heading (hx, hz), a unit vector; (0, 0) = the direction of the move.
  Move move_actor(int tri, float x, float z, float dx, float dz, u32 class_mask, float standoff = 8.0f,
                  float head_on = 0.1963f, float hx = 0.0f, float hz = 0.0f) const;

 private:
  // Walks the straight segment to (tx, tz); returns false if it hit a wall (position/tri hold the last valid point).
  bool walk(int& tri, float& x, float& z, float tx, float tz, u32 class_mask, int* wall_edge) const;
  float orientation(int tri) const;
  // > 0 inside, < 0 outside (orientation-normalised).
  float edge_side(int tri, int edge, float x, float z) const;
  // Unnormalised plane normal (v1 - v0) x (v2 - v0) of a valid triangle index.
  void plane_normal(int tri, float& nx, float& ny, float& nz) const;

  assets::WalkMesh mesh_;
  std::vector<assets::WalkTriangle> tris_;
  std::vector<u32> attr_flags_;
};

}  // namespace oc::game
