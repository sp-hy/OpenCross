#pragma once
// A room's own models (RoomScene::models): the model asset built from its scene record, its texture mods resolved, and its GPU uploads
// prepared. CPU only and thread-safe, so a room prepared ahead (RoomView::prefetch) builds them on a worker; FieldWorld::room_model
// uploads them.
#include <optional>
#include <string>

#include "assets/model.h"
#include "assets/room_scene.h"
#include "render/mesh_pipeline.h"

namespace oc::game {

struct PreparedRoomModel {
  assets::ModelAsset asset;
  render::ModelUploads uploads;
  // Made ahead when a GPU context was given.
  render::ModelTextures textures;
  // render::texture_registry::generation() the texture mods were resolved under.
  unsigned generation = 0;
};

// Model `index` of the scene with its transparency modes applied; texture mods not yet resolved.
std::optional<assets::ModelAsset> build_room_model(const assets::RoomScene& scene, size_t index, std::string* err);
// The same from one model record (another room's: import::load_room_models).
std::optional<assets::ModelAsset> build_room_model(const assets::RoomModelHeader& model, std::string* err);
// build_room_model + render::resolve_model_textures + render::prepare_model_uploads, and with `ctx` render::prepare_model_textures.
std::optional<PreparedRoomModel> prepare_room_model(const assets::RoomScene& scene, size_t index, render::GpuContext* ctx);

}  // namespace oc::game
