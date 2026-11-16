#pragma once
// Type-0x02 mesh objects of a room's effect .prd (sprite type 14: `elem`, `tori`, `tor1`, ...). The PS1 draws them like the battle stage
// meshes: textured / Gouraud packets with the object's colour, blend and OT bias, one ordering-table slot per packet (packet centre
// depth >> 4, + bias, or the fixed slot).
#include <memory>
#include <string>
#include <vector>

#include "assets/battle_stage.h"
#include "core/types.h"
#include "game/effects/emitter_system.h"
#include "render/camera.h"
#include "render/sprite_batch.h"
#include "render/texture.h"

namespace oc::effects {

// One projected, ready-to-submit triangle (corner 3 repeats corner 2). `set` empty: untextured (the caller's white texture).
struct RoomMeshTri {
  float x[4]{}, y[4]{}, u[4]{}, v[4]{};
  // A textured face: each corner's packet texel coordinate (texel k is centred on k) and the face's place in `set`: the normalised position
  // of the texture page origin (0, 1) and the size of one texel (2, 3), so u = texels[0] + texel_u * texels[2]. A texel size of 0 = none.
  float texel_u[4]{}, texel_v[4]{};
  float texels[4]{};
  u8 rgb[4][3]{};
  // GPU blend of ParticleBatch: 0 opaque, 1 B/2+F/2, 2 B+F, 3 B-F, 4 B+F/4.
  int blend = 0;
  int slot = 0;
  render::TexRef set;
};

class RoomMeshes {
 public:
  RoomMeshes() = default;
  ~RoomMeshes() { clear(); }
  RoomMeshes(const RoomMeshes&) = delete;
  RoomMeshes& operator=(const RoomMeshes&) = delete;

  // `meshes`: import::load_stage() of the room's effect .prd. Uploads its texture atlases (a null ctx = CPU only, for tests).
  void set(assets::StageAsset meshes, render::GpuContext* ctx, render::SpriteBatch* sprites);
  void clear();
  bool empty() const { return parts_.empty(); }
  bool has(const std::string& name) const { return find(name) != nullptr; }

  // Projects the faces of mesh `mesh.name` (view-space centre + matrix of the object, EmitterSystem::MeshInstance) to layer-space triangles.
  // Back faces are dropped like the game's NCLIP test; op 37 morphs the vertices toward `mesh.morph_name` (phase interpolated by `alpha`),
  // op 36 scrolls the texture inside its window (a triangle is cut along the wrap lines). Returns the number of triangles appended.
  size_t project(const EmitterSystem::MeshInstance& mesh, const ViewParams& view, const render::Letterbox& letterbox, float alpha,
                 std::vector<RoomMeshTri>& out) const;
  // The tick's final phase (no render interpolation).
  size_t project(const EmitterSystem::MeshInstance& mesh, const ViewParams& view, const render::Letterbox& letterbox, std::vector<RoomMeshTri>& out) const {
    return project(mesh, view, letterbox, 1.0f, out);
  }

 private:
  const assets::StagePart* find(const std::string& name) const;
  // Live tile replacement (render/stage_tiles.h): project() (const, once per object per frame) polls texture_registry::generation() and, when the mod overlay
  // changed, re-composes the baked object images from the asset's pristine tiles, idles the GPU, and re-uploads (hence mutable).
  void refresh_tiles() const;
  void upload_textures() const;
  // Kept for baked assets (tiles + object textures + the parts the recompose edits).
  mutable assets::StageAsset asset_;
  mutable unsigned tile_gen_ = 0;
  mutable std::vector<assets::StagePart> parts_;
  mutable std::vector<std::unique_ptr<render::Texture>> textures_;
  mutable std::vector<render::TexRef> sets_;
  render::Sampler sampler_;
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
};

}  // namespace oc::effects
