#include "game/model_files.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

#include "game/battle/model_table.h"
#include "import/container.h"
#include "import/model.h"
#include "render/character_texture.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

namespace oc::game {

namespace {
// RDE's HD art moves the weapon of battle model 0x14 (Leena) along its mount record 0: offset z, in model units.
constexpr size_t kMountNudgeModel = 0x14;
constexpr float kMountNudgeZ = -16.0f;

// The file name of the battle player whose Status-screen TIM a battle model without one of its own (a player, a monster) borrows in the
// Modded art mode: the first player with a TIM whose main page (.ltc / .ltd image 0: 8 bpp indices and CLUT) is the same image; empty =
// none. Cached per file-set generation.
std::string borrowed_tim(const vfs::Vfs& files, const std::string& stem, ByteSpan texture) {
  static std::mutex mutex;
  static std::map<std::pair<std::uint32_t, std::string>, std::string> cache;
  const auto key = std::make_pair(files.generation(), stem);
  {
    const std::lock_guard lock(mutex);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
  }
  std::string found;
  if (const auto own = import::load_atim_main(texture)) {
    for (const std::string& other : model_stems(files)) {
      if (other == stem || !other.starts_with("battle/player/")) continue;
      const std::string name = other.substr(other.find_last_of('/') + 1);
      if (!files.exists("hd/menu/player/" + name + ".tim")) continue;
      const auto other_ltc = files.read("cdrom/" + other + ".ltc");
      const auto page = other_ltc ? import::load_atim_main(*other_ltc) : std::nullopt;
      if (page && page->width == own->width && page->height == own->height && page->indices == own->indices && page->clut == own->clut) {
        found = name;
        break;
      }
    }
  }
  const std::lock_guard lock(mutex);
  cache[key] = found;
  return found;
}

// The Status-screen TIM (and character textures) battle model `stem` draws on in HD art: a player's own; with `modded` also, for a
// player without one and for a monster, a borrowed one. Empty = none.
std::string character_name(const vfs::Vfs& files, const std::string& stem, ByteSpan texture, bool modded) {
  const bool player = stem.starts_with("battle/player/");
  if (player) {
    const std::string name = stem.substr(stem.find_last_of('/') + 1);
    if (files.exists("hd/menu/player/" + name + ".tim")) return name;
  }
  if (!modded || !(player || stem.starts_with("battle/monster/"))) return {};
  return borrowed_tim(files, stem, texture);
}
}  // namespace

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
    // A player folder holds a .ltc (main image + closed-eye strip); its .ltd is a copy of the main image that HD art did not redraw for the
    // cdrom2 mesh, so the .ltc is read whenever there is one, for the party and for the enemies sharing the model.
    Bytes texture = read(base + ".ltc");
    if (texture.empty()) texture = read(base + ".ltd");
    asset = import::load_model(obj, texture, read("cdrom/" + folder + "/at0.prd"), err);
    if (asset && files.hd_mode()) {
      const std::string name = character_name(files, stem, texture, files.enhanced());
      const Bytes tim = name.empty() ? Bytes{} : read("hd/menu/player/" + name + ".tim");
      if (!tim.empty() && import::apply_battle_hd_tim(*asset, tim)) asset->character = name;
      if (stem == kBattleModels[kMountNudgeModel]) asset->attach.mounts[0].pos[2] += kMountNudgeZ;
    }
  }
  if (asset) asset->source = stem;
  return asset;
}

std::string battle_character(const vfs::Vfs& files, const std::string& stem) {
  auto texture = files.read("cdrom/" + stem + ".ltc");
  if (!texture) texture = files.read("cdrom/" + stem + ".ltd");
  return texture ? character_name(files, stem, *texture, true) : std::string{};
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
  // HD art only (its .ome has no Atim): the original art's menu model is another mesh with the disc texture. The texture mod gives the
  // character textures (render/character_texture.h), either of them alone too, turned back into the 4x image's two pages; the
  // replacement stands in for the 1x texture too.
  namespace tr = render::texture_registry;
  const auto cpt = import::parse_cpt(*ome);
  const bool hd_only = cpt && cpt->entries.size() > 5 && cpt->entries[5].data.empty();
  if (hd_only && tr::active() && asset->has_hd(0) && !asset->hd_textures[0].indexed && !asset->textures.empty()) {
    import::Image& image = asset->hd_textures[0];
    auto dump = [](const render::TextureKey& key, const import::Image& file) {
      if (file.width) tr::dump(key, file.width, file.height, render::model_file_pixels(file.width, file.height, file.pixels.data()).data());
    };
    if (tr::settings().dump)
      if (const auto rde = read_character_textures(files, stem)) {
        dump(render::character_texture_key(stem), rde->texture);
        dump(render::character_eyes_key(stem), rde->eyes);
      }
    import::Image open, eyes;
    const bool open_replaced = tr::replace(render::character_texture_key(stem), open.width, open.height, open.pixels);
    const bool eyes_replaced = tr::replace(render::character_eyes_key(stem), eyes.width, eyes.height, eyes.pixels);
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
    bool replaced = false;
    if (open_replaced || eyes_replaced)
      if (auto rde = read_character_textures(files, stem)) {
        if (!open_replaced) open = std::move(rde->texture);
        if (!eyes_replaced) eyes = std::move(rde->eyes);
        import::Image status = render::character_status_image(open, eyes.width ? &eyes : nullptr, {rde->eye.valid, rde->eye.x, rde->eye.y});
        replaced = status.width != 0;
        width = status.width, height = status.height, rgba = std::move(status.pixels);
      }
    // Legacy layout: model/menu/player/<stem>/clut0.png, both pages whole, from mods made before the character texture.
    if (!replaced) {
      width = image.width, height = image.height, rgba = image.pixels;
      replaced = tr::replace(render::model_texture_key("menu/player/" + stem, 0, 1, image.width, image.height, image.pixels.data()), width, height, rgba);
    }
    if (replaced) {
      render::model_restore_stp(image.width, image.height, image.pixels.data(), width, height, rgba);
      image.width = width, image.height = height;
      image.pixels = std::move(rgba);
      asset->textures[0] = image;
    }
  }
  return asset;
}

std::optional<CharacterTextures> read_character_textures(const vfs::Vfs& files, const std::string& stem, bool original_size) {
  std::optional<assets::ModelAsset> battle;
  for (int model = 0; model <= kLastMenuModel && !battle; ++model)
    if (menu_model_stem(model) == stem) battle = read_model_files(files, std::string(kBattleModels[static_cast<size_t>(model)]), nullptr);
  if (!battle || battle->character != stem || !battle->has_hd(0)) return std::nullopt;
  CharacterTextures out;
  const size_t strip = battle->main_texture_count;
  out.texture = original_size ? battle->textures[0] : battle->hd_textures[0];
  out.eye = battle->eye;
  if (battle->eye.valid && strip < battle->textures.size()) out.eyes = original_size ? battle->textures[strip] : battle->has_hd(strip) ? battle->hd_textures[strip] : import::Image{};
  return out;
}

std::string body_model_stem(int body) {
  if (body <= kLastMenuModel) return menu_model_stem(body);
  if (static_cast<size_t>(body) >= kBattleModels.size()) return {};
  const std::string_view path = kBattleModels[static_cast<size_t>(body)];
  return std::string(path.substr(path.rfind('/') + 1));
}

std::optional<assets::ModelAsset> read_body_model(const vfs::Vfs& files, int body, bool hd_art, std::string* err) {
  if (body <= kLastMenuModel) return read_menu_model(files, body, hd_art, err);
  if (body_model_stem(body).empty()) {
    if (err) *err = "no battle model " + std::to_string(body);
    return std::nullopt;
  }
  return read_model_files(files, std::string(kBattleModels[static_cast<size_t>(body)]), err);
}

}  // namespace oc::game
