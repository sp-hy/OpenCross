// Tile library of the effect packs' meshes: the scroll windows of the emitters that can draw them, and the bake.
#include "game/battle/fx_mesh_tiles.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <variant>

namespace oc::game {

render::TileUses fx_mesh_uses(const std::vector<const assets::RoomEffects*>& packs) {
  render::TileUses uses;
  for (const assets::RoomEffects* pack : packs) {
    if (!pack) continue;
    for (const assets::Emitter& emitter : pack->emitters) {
      std::vector<std::string> meshes;
      std::vector<std::array<int, 4>> windows;
      for (const assets::EffectOp& op : emitter.ops) {
        if (const auto* sprite = std::get_if<assets::FxSprite>(&op.arg); sprite && op.op == 0x05 && (sprite->type == 14 || sprite->type == 19)) meshes.push_back(sprite->name);
        if (const auto* scroll = std::get_if<assets::FxUvScroll>(&op.arg); scroll && op.op == 0x36) windows.push_back({scroll->off_u, scroll->off_v, scroll->win_w, scroll->win_h});
      }
      for (const std::string& mesh : meshes)
        for (const std::array<int, 4>& window : windows) {
          std::vector<std::array<int, 4>>& known = uses.scroll[mesh];
          if (std::find(known.begin(), known.end(), window) == known.end()) known.push_back(window);
        }
    }
  }
  for (auto& [mesh, windows] : uses.scroll) std::sort(windows.begin(), windows.end());
  return uses;
}

namespace {

bool unbaked(const std::shared_ptr<assets::StageAsset>& asset) { return asset && asset->tile_map.id_map.empty() && !asset->textures.empty(); }

double bake(assets::StageAsset& asset, const std::vector<const assets::RoomEffects*>& packs) {
  const auto start = std::chrono::steady_clock::now();
  const render::TileUses uses = fx_mesh_uses(packs);
  render::bake_object_textures(asset, &uses);
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

double bake_fx_resident_meshes(const FxResident& resident, const assets::RoomEffects& particle_lib) {
  if (!unbaked(resident.meshes)) return 0.0;
  std::vector<const assets::RoomEffects*> packs{&particle_lib};
  for (const auto& resident_pack : resident.packs) packs.push_back(resident_pack.get());
  return bake(*resident.meshes, packs);
}

double bake_fx_pack_meshes(const FxPack& pack, const assets::RoomEffects& particle_lib) {
  if (!unbaked(pack.meshes)) return 0.0;
  std::vector<const assets::RoomEffects*> packs{&pack.fx, pack.extra.get(), &particle_lib};
  if (pack.resident)
    for (const auto& resident_pack : pack.resident->packs) packs.push_back(resident_pack.get());
  return bake(*pack.meshes, packs);
}

std::string fx_mesh_tile_names(const assets::StageAsset& asset) {
  std::string names;
  for (const assets::StageTile& tile : asset.tiles) names += " tile_" + tile.hash;
  return names;
}

}  // namespace oc::game
