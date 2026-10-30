#pragma once
// The files of a character model, found from its member stem (no "cdrom/", no ".obj") and decoded without texture
// replacement. Field models (fieldobj/player/<dir>/<name>) pair <stem>.tim, the 4x hd/<stem>.tim and the animation pack
// fieldobj/ex/<name>.bin; battle models (battle/player/.., battle/monster/..) pair <stem>.ltd (else .ltc) and the at0.prd of
// their folder. The asset's source is the stem (it names the texture dump / replacement folder).
#include <optional>
#include <string>
#include <vector>

#include "assets/model.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

std::optional<assets::ModelAsset> read_model_files(const vfs::Vfs& files, const std::string& stem, std::string* err);

// Sorted lower-case stems of every character model in the cdrom archive (field players, battle players, battle monsters).
std::vector<std::string> model_stems(const vfs::Vfs& files);

}  // namespace oc::game
