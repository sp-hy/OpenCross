#include "game/model_files.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "game/battle/model_table.h"
#include "import/container.h"
#include "import/model.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

namespace oc::game {

namespace {
// RDE's HD art moves the weapon of battle model 0x14 (Leena) along its mount record 0: offset z, in model units.
constexpr size_t kMountNudgeModel = 0x14;
constexpr float kMountNudgeZ = -16.0f;
}  // namespace

std::optional<assets::ModelAsset> read_model_files(const vfs::Vfs& files, const std::string& stem, std::string* err, bool hd_battle_texture) {
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
    // A player folder holds a .ltc (main image + closed-eye strip); its .ltd is a copy of the main image that HD art did not redraw for the
    // cdrom2 mesh, so the .ltc is read whenever there is one, for the party and for the enemies sharing the model.
    Bytes texture = read(base + ".ltc");
    if (texture.empty()) texture = read(base + ".ltd");
    asset = import::load_model(obj, texture, read("cdrom/" + folder + "/at0.prd"), err);
    if (asset && files.hd_mode() && stem.starts_with("battle/player/")) {
      if (hd_battle_texture) {
        const Bytes tim = read("hd/menu/player/" + stem.substr(slash + 1) + ".tim");
        if (!tim.empty()) import::apply_battle_hd_tim(*asset, tim);
      }
      if (stem == kBattleModels[kMountNudgeModel]) asset->attach.mounts[0].pos[2] += kMountNudgeZ;
    }
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

std::optional<assets::Skeleton> read_field_skeleton(const vfs::Vfs& files, const std::string& stem) {
  const auto obj = files.read("cdrom/fieldobj/player/" + stem + ".obj");
  return obj ? import::load_mdl_skeleton(*obj) : std::nullopt;
}

std::string menu_model_stem(int model) {
  if (model < 0 || model > kLastMenuModel) return {};
  const std::string_view path = kBattleModels[static_cast<size_t>(model)];
  return std::string(path.substr(path.rfind('/') + 1));
}

std::optional<assets::ModelAsset> read_menu_model(const vfs::Vfs& files, int model, bool hd_art, std::string* err) {
  const std::string stem = menu_model_stem(model);
  if (stem.empty()) {
    if (err) *err = "no menu model " + std::to_string(model);
    return std::nullopt;
  }
  const auto ome = files.read("cdrom/menu/player/" + stem + ".ome");
  if (!ome) {
    if (err) *err = "menu/player/" + stem + ".ome not found";
    return std::nullopt;
  }
  // The 4x texture is used only in HD art: the original art keeps the disc texture.
  const Bytes hd = hd_art ? files.read("hd/menu/player/" + stem + ".tim").value_or(Bytes{}) : Bytes{};
  auto asset = import::load_menu_model(*ome, hd, err);
  if (!asset) return std::nullopt;
  // HD art only (its .ome has no Atim): the original art's menu model is another mesh with the disc texture. The texture mod sees the 4x
  // image, both pages, and the replacement stands in for the 1x texture too.
  namespace tr = render::texture_registry;
  const auto cpt = import::parse_cpt(*ome);
  const bool hd_only = cpt && cpt->entries.size() > 5 && cpt->entries[5].data.empty();
  if (hd_only && tr::active() && asset->has_hd(0) && !asset->hd_textures[0].indexed && !asset->textures.empty()) {
    import::Image& image = asset->hd_textures[0];
    const render::TextureKey key = render::model_texture_key("menu/player/" + stem, 0, 1, image.width, image.height, image.pixels.data());
    if (tr::settings().dump) tr::dump(key, image.width, image.height, render::model_file_pixels(image.width, image.height, image.pixels.data()).data());
    u32 width = image.width, height = image.height;
    std::vector<u8> rgba = image.pixels;
    if (tr::replace(key, width, height, rgba)) {
      render::model_restore_stp(image.width, image.height, image.pixels.data(), width, height, rgba);
      image.width = width, image.height = height;
      image.pixels = std::move(rgba);
      asset->textures[0] = image;
    }
  }
  return asset;
}

}  // namespace oc::game
