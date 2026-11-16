#pragma once
// The tile library of the battle effect packs' meshes (render/stage_tiles.h): a pack's own type-0x02 meshes and the resident battle.prd ones are cut
// into tiles like a battle stage's, named by content under one group (StageAsset::tex_group "effect_mesh": effect_mesh/tile_<hash8>.png), so identical
// art is one file across packs.
#include <string>
#include <vector>

#include "assets/battle_stage.h"
#include "assets/room_effects.h"
#include "game/battle/battle_fx.h"
#include "render/stage_tiles.h"

namespace oc::game {

// The op-36 texture windows the emitters of `packs` scroll their mesh sprites (types 14 / 19) through, by mesh name.
render::TileUses fx_mesh_uses(const std::vector<const assets::RoomEffects*>& packs);

// Cut the tiles of the resident meshes / of the pack's own meshes, each asset once (a baked asset is left alone). A pack's meshes take the
// windows of the emitters that can name them while it plays: its own, the resident packs' and the particle library's; the resident meshes
// those of the resident packs and the library (an element pack that scrolls a resident mesh through another window draws it from the
// atlases: render::tiles_hold_window). Return the milliseconds spent (0 when nothing was left to bake).
double bake_fx_resident_meshes(const FxResident& resident, const assets::RoomEffects& particle_lib);
double bake_fx_pack_meshes(const FxPack& pack, const assets::RoomEffects& particle_lib);

// "tile_<hash8>" of every tile of a baked asset, space separated (the dump's log line).
std::string fx_mesh_tile_names(const assets::StageAsset& asset);

}  // namespace oc::game
