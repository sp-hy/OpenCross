#pragma once
// Environment-mapped pack meshes. A mesh object with extended flag 4 is drawn on the stock env page: page (320, 256) 8 bit, CLUT (0, 239), the
// stage's own backdrop picture. Each corner's UV comes from its vertex normal turned by the object's rotation (no camera), so the mesh mirrors
// the stage: u = (x >> 6) + 0x40, v = (y >> 6) + 0xC0 of the unit normal. With a morph target the normals are lerped with the vertices.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "game/effects/bespoke/bespoke.h"
#include "game/effects/emitter_system.h"
#include "import/battle_stage.h"

namespace oc::effects {

struct EnvMesh {
  std::vector<std::array<s16, 3>> verts;
  std::vector<import::StageFace> faces;
  // Vertex normals (4.12, length 4096).
  std::vector<std::array<int, 3>> vnorm;
};

// v * 4096 / |v|, 0 for the zero vector.
std::array<int, 3> normal4096(double x, double y, double z);
// Per face in file order, the face normal cross(B - A, C - A) >> 6 normalised with A = idx1, B = idx0, C = idx3 (quads) / idx2 (triangles);
// it is added to every corner and each corner is normalised again at once.
void build_normals(EnvMesh& mesh);
// The normal turned by the rotation matrix only and normalised, as the two bytes of the packet's UV halfword (the window u 0..127, v 128..255).
std::array<u8, 2> env_uv(const Mat3& rotation, const std::array<int, 3>& normal);

// The pack's meshes in this form, parsed when an object first names them.
class EnvMeshes {
 public:
  EnvMeshes() = default;
  explicit EnvMeshes(ByteSpan prd) : prd_(prd) {}
  // The object's textured packets as TexSrc::Env primitives: the extended builders' cull bits, slot = average z of corners 0..2 >> 4 + bias,
  // colour = packet colour x object colour >> 7. `morph` = the morph weight 0..1 at the tick alpha.
  void emit(const EmitterSystem::MeshInstance& instance, float morph, const ViewParams& view, std::vector<FxPrim>& out);

 private:
  const EnvMesh& find(const std::string& name);
  ByteSpan prd_;
  std::map<std::string, EnvMesh> meshes_;
};

}  // namespace oc::effects
