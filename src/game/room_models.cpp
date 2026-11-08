#include "game/room_models.h"

#include "import/model.h"
#include "render/texture_registry.h"

namespace oc::game {

std::optional<assets::ModelAsset> build_room_model(const assets::RoomScene& scene, size_t index, std::string* err) {
  if (index >= scene.models.size()) return std::nullopt;
  return build_room_model(scene.models[index], err);
}

std::optional<assets::ModelAsset> build_room_model(const assets::RoomModelHeader& model, std::string* err) {
  // Page window + CLUT rows clut_y / clut_y + 1 (faces with the CLUT / page bits); the bare TIM if a scene lacks them.
  auto asset = model.page_textures.empty() ? import::load_room_model(model.mdl, model.texture, err) : import::load_room_model(model.mdl, model.page_textures, err);
  if (!asset) return std::nullopt;
  // Texture dump / replace path: room_model/<content tag>.png
  asset->source = "@room_model";
  // S3 shading entry k (low nybble = transparency mode: 2 transparent, 6 bright, A inverted, E faint, odd invisible) applies to construct k;
  // opn_a01i's lower door pair (models 3 / 4, entry 0 = 0x0A) is the subtractive floor shadow of the doors, not an opaque black slab.
  for (size_t mi = 0; mi < asset->meshes.size() && mi < asset->shading.size(); ++mi) {
    const u8 shading_flags = asset->shading[mi].flags & 0xF;
    asset->meshes[mi].trans = (shading_flags & 1) ? u8{0xFF} : (shading_flags & 2) ? static_cast<u8>(1 + ((shading_flags >> 2) & 3)) : u8{0};
  }
  return asset;
}

std::optional<PreparedRoomModel> prepare_room_model(const assets::RoomScene& scene, size_t index, render::GpuContext* ctx) {
  const unsigned generation = render::texture_registry::generation();
  std::string err;
  std::optional<assets::ModelAsset> asset = build_room_model(scene, index, &err);
  if (!asset) return std::nullopt;
  render::resolve_model_textures(*asset);
  render::ModelUploads uploads = render::prepare_model_uploads(*asset);
  render::ModelTextures textures = ctx ? render::prepare_model_textures(*ctx, *asset, uploads) : render::ModelTextures{};
  return PreparedRoomModel{std::move(*asset), std::move(uploads), std::move(textures), generation};
}

}  // namespace oc::game
