#pragma once
// The files of a character model, found from its member stem (no "cdrom/", no ".obj") and decoded without texture
// replacement. Field models (fieldobj/player/<dir>/<name>) pair <stem>.tim, the 4x hd/<stem>.tim and the animation pack
// fieldobj/ex/<name>.bin; battle models (battle/player/.., battle/monster/..) pair <stem>.ltc (else .ltd) and the at0.prd of
// their folder. The asset's source is the stem (it names the texture dump / replacement folder). In HD art (Vfs::hd_mode) a battle player
// model also gets RDE's HD-art weapon mount correction and its 4x HD image from hd/menu/player/<name>.tim (import::apply_battle_hd_tim);
// it then names <name> as its character textures (render/character_texture.h). In the Modded art mode a battle player without a TIM of
// its own, and a battle monster, borrows the TIM (and the character textures) of the battle player whose main page is the same image. An
// active mod's model replacement (game/model_mods.h) then replaces the meshes, here and in read_menu_model; a battle player model with a
// Status-screen model and no file of its own wears its character's all-uses file (wear_character_model).
#include <optional>
#include <string>
#include <vector>

#include "assets/model.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

std::optional<assets::ModelAsset> read_model_files(const vfs::Vfs& files, const std::string& stem, std::string* err);
// A field model with the animation pack fieldobj/ex/<ex_pack>.bin instead of the one named after its file ("" = no pack): a character
// model byte's pack is the one its file record names (DiscIndex::field_pack_name).
std::optional<assets::ModelAsset> read_model_files(const vfs::Vfs& files, const std::string& stem, const std::string& ex_pack, std::string* err);

// Sorted lower-case stems of every character model in the cdrom archive (field players, battle players, battle monsters).
std::vector<std::string> model_stems(const vfs::Vfs& files);

// The character textures (render/character_texture.h) battle model `stem` draws from in the Modded art mode: its own Status-screen TIM's
// file name, else the one it borrows (players and monsters); empty for models with neither (HD file set).
std::string battle_character(const vfs::Vfs& files, const std::string& stem);

// Menu models (the Status screen's) 0..kLastMenuModel follow the battle model order of the player block.
constexpr int kLastMenuModel = 0x32;
// The file name of menu model `model` (menu/player/<name>.ome), empty outside the player block.
std::string menu_model_stem(int model);
// The model file a menu model wears: Status, the Status screen's (its own menu/player file, else the character's all-uses file); AllUses,
// only the all-uses file (what the field and battle draw where their own model has no file).
enum class MenuModelMods { Status, AllUses };
// Menu model `model`, cdrom/menu/player/<name>.ome; in HD art with the 4x hd/menu/player/<name>.tim, which is then also dumped / replaced
// as the character textures <name> (render/character_texture.h) and stands in for the 1x texture.
std::optional<assets::ModelAsset> read_menu_model(const vfs::Vfs& files, int model, bool hd_art, std::string* err,
                                                  MenuModelMods mods = MenuModelMods::Status);
// The same for a menu model by its stem (menu/player/<stem>.ome: a character mods add ships one by its name).
std::optional<assets::ModelAsset> read_menu_model_stem(const vfs::Vfs& files, const std::string& stem, bool hd_art, std::string* err,
                                                       MenuModelMods mods = MenuModelMods::Status);
// The character textures of status-screen model <stem> as RDE has them (HD file set): its battle model's 4x main texture (the open half
// of the TIM) and 4x eye strip (empty when the model has no eye rects), with the battle eye rects. `original_size`: the 1x page and strip
// the 4x images were upscaled from instead.
struct CharacterTextures {
  import::Image texture, eyes;
  assets::ModelAsset::EyeSwap eye;
};
std::optional<CharacterTextures> read_character_textures(const vfs::Vfs& files, const std::string& stem, bool original_size = false);
// The detailed body the HD models in fields draw for battle model `body` (the game's battle model numbering, which the menu models
// share): menu model `body` up to kLastMenuModel, else the battle model itself (battle/monster/...: an NPC's).
std::optional<assets::ModelAsset> read_body_model(const vfs::Vfs& files, int body, bool hd_art, std::string* err,
                                                  MenuModelMods mods = MenuModelMods::Status);
// Its file name: the menu model's, else the battle model's.
std::string body_model_stem(int body);
// The skeleton alone of field player model `stem` (fieldobj/player/<folder>/<file>, no ".obj"), nullopt when it does not read.
std::optional<assets::Skeleton> read_field_skeleton(const vfs::Vfs& files, const std::string& stem);

}  // namespace oc::game
