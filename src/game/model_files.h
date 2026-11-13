#pragma once
// The files of a character model, found from its member stem (no "cdrom/", no ".obj") and decoded without texture
// replacement. Field models (fieldobj/player/<dir>/<name>) pair <stem>.tim, the 4x hd/<stem>.tim and the animation pack
// fieldobj/ex/<name>.bin; battle models (battle/player/.., battle/monster/..) pair <stem>.ltc (else .ltd) and the at0.prd of
// their folder. The asset's source is the stem (it names the texture dump / replacement folder). In HD art (Vfs::hd_mode) a battle player
// model also gets RDE's HD-art weapon mount correction and, with `hd_battle_texture`, its 4x HD image from hd/menu/player/<name>.tim
// (import::apply_battle_hd_tim); without it the model keeps its 1x .ltc only.
#include <optional>
#include <string>
#include <vector>

#include "assets/model.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

std::optional<assets::ModelAsset> read_model_files(const vfs::Vfs& files, const std::string& stem, std::string* err, bool hd_battle_texture = false);

// Sorted lower-case stems of every character model in the cdrom archive (field players, battle players, battle monsters).
std::vector<std::string> model_stems(const vfs::Vfs& files);

// Menu models (the Status screen's) 0..kLastMenuModel follow the battle model order of the player block.
constexpr int kLastMenuModel = 0x32;
// The file name of menu model `model` (menu/player/<name>.ome), empty outside the player block.
std::string menu_model_stem(int model);
// Menu model `model`, cdrom/menu/player/<name>.ome; in HD art with the 4x hd/menu/player/<name>.tim, which is then also dumped / replaced
// as the texture model/menu/player/<name> and stands in for the 1x texture.
std::optional<assets::ModelAsset> read_menu_model(const vfs::Vfs& files, int model, bool hd_art, std::string* err);
// The detailed body [Enhancements] MenuModelsInField draws for battle model `body` (the game's battle model numbering, which the menu models
// share): menu model `body` up to kLastMenuModel, else the battle model itself (battle/monster/...: an NPC's).
std::optional<assets::ModelAsset> read_body_model(const vfs::Vfs& files, int body, bool hd_art, std::string* err);
// Its file name: the menu model's, else the battle model's.
std::string body_model_stem(int body);
// The skeleton alone of field player model `stem` (fieldobj/player/<folder>/<file>, no ".obj"), nullopt when it does not read.
std::optional<assets::Skeleton> read_field_skeleton(const vfs::Vfs& files, const std::string& stem);

}  // namespace oc::game
