#include "game/effects/bespoke/env_mesh.h"

#include <algorithm>
#include <cmath>

#include "game/effects/fx_math.h"
#include "import/container.h"

namespace oc::effects {

std::array<int, 3> normal4096(double x, double y, double z) {
  const double len = std::sqrt(x * x + y * y + z * z);
  if (len < 1e-9) return {0, 0, 0};
  return {static_cast<int>(std::lround(x * 4096.0 / len)), static_cast<int>(std::lround(y * 4096.0 / len)), static_cast<int>(std::lround(z * 4096.0 / len))};
}

void build_normals(EnvMesh& mesh) {
  mesh.vnorm.assign(mesh.verts.size(), {0, 0, 0});
  for (const import::StageFace& face : mesh.faces) {
    const size_t corner_a = face.idx[1], corner_b = face.idx[0], corner_c = face.n == 4 ? face.idx[3] : face.idx[2];
    if (corner_a >= mesh.verts.size() || corner_b >= mesh.verts.size() || corner_c >= mesh.verts.size()) continue;
    const double ux = mesh.verts[corner_b][0] - mesh.verts[corner_a][0], uy = mesh.verts[corner_b][1] - mesh.verts[corner_a][1],
                 uz = mesh.verts[corner_b][2] - mesh.verts[corner_a][2];
    const double vx = mesh.verts[corner_c][0] - mesh.verts[corner_a][0], vy = mesh.verts[corner_c][1] - mesh.verts[corner_a][1],
                 vz = mesh.verts[corner_c][2] - mesh.verts[corner_a][2];
    const auto face_normal =
        normal4096(std::trunc((uy * vz - uz * vy) / 64.0), std::trunc((uz * vx - ux * vz) / 64.0), std::trunc((ux * vy - uy * vx) / 64.0));
    for (int k = 0; k < face.n; ++k) {
      const size_t vi = face.idx[static_cast<size_t>(k)];
      if (vi >= mesh.vnorm.size()) continue;
      auto& normal = mesh.vnorm[vi];
      normal = normal4096(normal[0] + face_normal[0], normal[1] + face_normal[1], normal[2] + face_normal[2]);
    }
  }
}

std::array<u8, 2> env_uv(const Mat3& rotation, const std::array<int, 3>& normal) {
  const Vec3 rotated = rotation.apply({static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2])});
  const auto unit = normal4096(rotated[0], rotated[1], rotated[2]);
  const int u = (unit[0] >> 6) + 0x40, v = (unit[1] >> 6) + 0xC0;
  const int halfword = ((v << 8) | u) & 0xFFFF;
  return {static_cast<u8>(halfword & 0xFF), static_cast<u8>(halfword >> 8)};
}

const EnvMesh& EnvMeshes::find(const std::string& name) {
  if (const auto it = meshes_.find(name); it != meshes_.end()) return it->second;
  EnvMesh& mesh = meshes_[name];
  if (const auto drp = import::parse_drp(prd_))
    for (const auto& entry : drp->entries)
      if (entry.type == 0x02 && entry.name == name) {
        if (auto stage_mesh = import::parse_stage_mesh(entry.data)) {
          mesh.verts = stage_mesh->verts;
          mesh.faces = stage_mesh->faces;
          build_normals(mesh);
        }
        break;
      }
  return mesh;
}

void EnvMeshes::emit(const EmitterSystem::MeshInstance& instance, float morph, const ViewParams& view, std::vector<FxPrim>& out) {
  const EnvMesh& mesh = find(*instance.name);
  const EnvMesh* target = instance.morph_name ? &find(*instance.morph_name) : nullptr;
  if (target && target->verts.size() != mesh.verts.size()) target = nullptr;
  const int weight = target ? std::clamp(static_cast<int>(morph * 4096.0f), 0, 0x1000) : 0;
  std::vector<Vec3> camera(mesh.verts.size());
  std::vector<std::array<u8, 2>> uvs(mesh.verts.size());
  for (size_t i = 0; i < mesh.verts.size(); ++i) {
    Vec3 vertex;
    std::array<int, 3> normal = mesh.vnorm[i];
    for (size_t k = 0; k < 3; ++k) {
      vertex[k] = static_cast<float>(mesh.verts[i][k]);
      if (target) {
        vertex[k] = static_cast<float>((mesh.verts[i][k] * (0x1000 - weight) + target->verts[i][k] * weight) >> 12);
        normal[k] = (mesh.vnorm[i][k] * (0x1000 - weight) + target->vnorm[i][k] * weight) >> 12;
      }
    }
    const Vec3 turned = instance.mat.apply(vertex);
    for (size_t k = 0; k < 3; ++k) camera[i][k] = turned[k] + instance.centre[k];
    uvs[i] = env_uv(instance.world_mat, normal);
  }
  for (const import::StageFace& face : mesh.faces) {
    if (!face.textured) continue;
    FxPrim prim;
    prim.kind = face.n == 3 ? FxPrim::Kind::Tri : FxPrim::Kind::Quad;
    prim.src = FxPrim::TexSrc::Env;
    float sx[4], sy[4], depth_sum = 0;
    bool visible = true;
    for (int k = 0; k < face.n && visible; ++k) {
      const size_t vi = face.idx[static_cast<size_t>(k)];
      if (vi >= camera.size() || camera[vi][2] < 32.0f) {
        visible = false;
        break;
      }
      sx[k] = view.ox + view.h * camera[vi][0] / camera[vi][2] + view.scroll_x;
      sy[k] = view.oy + view.h * camera[vi][1] / camera[vi][2] + view.scroll_y;
      if (k < 3) depth_sum += camera[vi][2];
      u8 rgb[3];
      for (size_t channel = 0; channel < 3; ++channel) rgb[channel] = static_cast<u8>(std::min(255, (face.rgb[static_cast<size_t>(k)][channel] * instance.rgb[channel]) >> 7));
      prim.v[k] = {sx[k], sy[k], rgb[0], rgb[1], rgb[2]};
      prim.u[k] = uvs[vi][0];
      prim.w[k] = uvs[vi][1];
    }
    if (!visible) continue;
    const float nclip = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (!((instance.ext & 0x20) != 0 || ((instance.ext & 0x10) ? nclip < 1.0f : nclip > 0.0f))) continue;
    prim.blend = instance.blend >= 5 ? 2 : instance.blend;
    prim.slot = instance.fixed_slot ? instance.bias : (static_cast<int>(depth_sum / 3.0f) >> 4) + instance.bias;
    prim.order = static_cast<u32>(out.size());
    out.push_back(prim);
  }
}

}  // namespace oc::effects
