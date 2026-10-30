#include "game/model_files.h"

#include <algorithm>
#include <cctype>

#include "import/model.h"
#include "vfs/vfs.h"

namespace oc::game {

std::optional<assets::ModelAsset> read_model_files(const vfs::Vfs& files, const std::string& stem, std::string* err) {
  auto read = [&](const std::string& path) { return files.read(path).value_or(Bytes{}); };
  const std::string base = "cdrom/" + stem;
  const Bytes obj = read(base + ".obj");
  if (obj.empty()) {
    if (err) *err = base + ".obj not found";
    return std::nullopt;
  }
  const size_t slash = stem.find_last_of('/');
  const std::string folder = slash == std::string::npos ? std::string{} : stem.substr(0, slash);
  std::optional<assets::ModelAsset> asset;
  if (stem.starts_with("fieldobj/")) {
    const Bytes tim = read(base + ".tim");
    if (tim.empty()) {
      if (err) *err = base + ".tim not found";
      return std::nullopt;
    }
    const std::string name = stem.substr(slash + 1);
    asset = import::load_field_model(obj, tim, read("cdrom/fieldobj/ex/" + name + ".bin"), err, read("hd/" + stem + ".tim"));
  } else {
    Bytes texture = read(base + ".ltd");
    if (texture.empty()) texture = read(base + ".ltc");
    asset = import::load_model(obj, texture, read("cdrom/" + folder + "/at0.prd"), err);
  }
  if (asset) asset->source = stem;
  return asset;
}

std::vector<std::string> model_stems(const vfs::Vfs& files) {
  std::vector<std::string> stems;
  for (std::string path : files.list("cdrom")) {
    for (char& letter : path) letter = letter == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    if (!path.ends_with(".obj")) continue;
    if (path.starts_with("fieldobj/player/") || path.starts_with("battle/player/") || path.starts_with("battle/monster/"))
      stems.push_back(path.substr(0, path.size() - 4));
  }
  std::ranges::sort(stems);
  stems.erase(std::ranges::unique(stems).begin(), stems.end());
  return stems;
}

}  // namespace oc::game
