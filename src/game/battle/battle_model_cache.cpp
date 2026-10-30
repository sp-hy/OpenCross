// ModelCache: per-unit model, weapon and script file lookup and loading.
#include "game/battle/battle_model_cache.h"
#include "game/battle/model_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "battle/battle.h"
#include "core/log.h"
#include "data/tables.h"
#include "game/model_files.h"
#include "import/model.h"
#include "import/room_effects.h"
#include "render/mesh_pipeline.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"
namespace oc::game {


namespace {

// Weapon pack (battle/wepon/<name>.bin) of each character id 0..43, same order as the field table: file table 3195 + 3 m; the part = weapon gear tier & 0xF.
constexpr const char* kWeaponPacks[44] = {
    "kai",  "knife", "rod",   "gun",   "black", "sword", "rod",  "axe",  "black", "black", "black", "nage", "black", "minirod", "axe",
    "cook", "tue",   "sword", "cook",  "sword", "cook",  "black", "nage", "ninjin", "black", "gun", "tue",  "black", "sword", "black",
    "black", "black", "axe",  "harp",  "boom",  "axe",   "boom", "nage", "sword", "nage",  "black", "sword", "black", "black"};

// Folder under battle/player/ (and fieldobj/player/) of every playable character id 0..43, same order as the field table.
constexpr const char* kCharFolders[44] = {
    "selju",  "kid",     "guill",  "ishito",   "slash",   "jakotu", "riddle", "carshu", "zoah",    "malchera", "colcha",
    "lutianna", "poshul", "lazzuly", "zappa",    "ocha",    "radius", "farga",  "obachan", "gren",   "lena",     "miki",
    "harley", "jyaness", "dragon", "hosi",      "spri",    "dan",    "kabuo",  "rablue", "jilbelt", "hone",     "kinoko",
    "ilenes", "mell",    "liia",   "banc",      "sunef",   "steena", "doc",    "gyada",  "pierre",  "olha",     "tumaru"};
}  // namespace

void ModelCache::clear() {
  models.clear();
  weapons.clear();
  scripts.clear();
  if (prefetch_job_.valid()) prefetch_job_.wait();
  prefetch_job_ = {};
  prefetched_ = {};
}

void ModelCache::prefetch(const vfs::Vfs& vfs, std::vector<std::string> stems, std::vector<std::pair<std::string, int>> weapon_parts) {
  if (prefetch_job_.valid()) prefetch_job_.wait();
  std::erase_if(stems, [&](const std::string& stem) { return stem.empty() || models.contains(stem); });
  std::erase_if(weapon_parts, [&](const auto& part) { return part.first.empty() || weapons.contains(part.first + "#" + std::to_string(part.second)); });
  prefetch_job_ = std::async(std::launch::async, [&vfs, stems = std::move(stems), weapon_parts = std::move(weapon_parts)] {
    Prefetched out;
    out.generation = render::texture_registry::generation();
    auto add = [](std::map<std::string, PrefetchedModel>& to, const std::string& key, assets::ModelAsset asset) {
      render::ModelUploads uploads = render::prepare_model_uploads(asset);
      to.emplace(key, PrefetchedModel{std::move(asset), std::move(uploads)});
    };
    for (const std::string& stem : stems)
      if (auto asset = read_model(vfs, stem, nullptr)) add(out.models, stem, std::move(*asset));
    for (const auto& [bin, part] : weapon_parts)
      if (auto asset = read_weapon(vfs, bin, part)) add(out.weapons, bin + "#" + std::to_string(part), std::move(*asset));
    return out;
  });
}

bool ModelCache::prefetch_ready() const {
  return !prefetch_job_.valid() || prefetch_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// The GPU half of a prefetched load; null when it was not prefetched (or the texture mods changed since).
std::shared_ptr<ModelHandle> ModelCache::make_prefetched(render::MeshPipeline& meshes, bool weapon, const std::string& key) {
  if (prefetch_job_.valid()) prefetched_ = prefetch_job_.get();
  auto& from = weapon ? prefetched_.weapons : prefetched_.models;
  const auto it = from.find(key);
  if (it == from.end()) return nullptr;
  PrefetchedModel entry = std::move(it->second);
  from.erase(it);
  if (prefetched_.generation != render::texture_registry::generation()) return nullptr;
  return Actor::make_resolved_model(meshes, std::move(entry.asset), &entry.uploads);
}

std::string ModelCache::player_model(const vfs::Vfs& vfs, int char_id) {
  if (char_id < 0 || char_id >= 44) return {};
  const std::string folder = kCharFolders[char_id];
  // Folder variants: selju -> selju2/selju, gren -> gren1/gren1 (battle/player has "2" / "1" suffixed folders).
  for (const std::string& cand : {"battle/player/" + folder + "/" + folder, "battle/player/" + folder + "1/" + folder + "1", "battle/player/" + folder + "2/" + folder})
    if (vfs.exists("cdrom/" + cand + ".obj")) return cand;
  return {};
}

// monster.bin byte 0x2D = battle model id: file-table entry 3194 + 3 * id is the model's .obj (model_table.h).
std::string ModelCache::monster_model(const data::GameTables* tables, int monster_id) {
  if (monster_id < 0 || static_cast<size_t>(monster_id) >= tables->enemies.size()) return {};
  const size_t model = tables->enemies[static_cast<size_t>(monster_id)].model();
  return model < kBattleModels.size() ? std::string(kBattleModels[model]) : std::string{};
}

std::optional<assets::ModelAsset> ModelCache::read_model(const vfs::Vfs& vfs, const std::string& stem, std::string* err) {
  auto asset = read_model_files(vfs, stem, err);
  if (!asset) return std::nullopt;
  render::resolve_model_textures(*asset);
  return asset;
}

std::shared_ptr<ModelHandle> ModelCache::load_model_for(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const std::string& stem) {
  if (stem.empty()) return nullptr;
  if (auto it = models.find(stem); it != models.end()) return it->second;
  std::shared_ptr<ModelHandle> handle = make_prefetched(meshes, false, stem);
  if (!handle) {
    std::string err;
    if (std::optional<assets::ModelAsset> asset = read_model(vfs, stem, &err)) handle = Actor::make_resolved_model(meshes, std::move(*asset));
    else log::warn("battle: model {}: {}", stem, err);
  }
  models[stem] = handle;
  return handle;
}

// The model stem of a unit.
std::string ModelCache::unit_model_stem(const vfs::Vfs& vfs, const data::GameTables* tables, const battle::Unit& unit) {
  // A PC's model is loaded from its kind byte (file group 0xE, kind * 3 + 3): Pip's forms 0x2C..0x30 are
  // battle/player/tu_p .. tu_mm (kBattleModels 44..48); the other characters keep their player folder.
  if (unit.pc && unit.form < 0 && battle::pip_form(unit.kind) && unit.kind != 0x2B && static_cast<size_t>(unit.kind) < kBattleModels.size())
    return std::string(kBattleModels[static_cast<size_t>(unit.kind)]);
  return unit.pc && unit.form < 0 ? player_model(vfs, unit.src_id) : monster_model(tables, unit.form >= 0 ? unit.form : unit.src_id);
}

int ModelCache::weapon_digit(const data::GameTables* tables, const battle::Unit& unit) {
  if (!unit.pc || unit.form >= 0) return -1;
  const data::GearRec* gear = tables ? tables->gear_rec(unit.eq.weapon_id) : nullptr;
  return gear ? (gear->tier() & 0xF) : 0;
}

std::string ModelCache::player_weapon_bin(int char_id) {
  return char_id < 0 || char_id >= 44 ? std::string{} : std::string("battle/wepon/") + kWeaponPacks[char_id] + ".bin";
}

std::string ModelCache::enemy_weapon_bin(int model) {
  return model < 0 || static_cast<size_t>(model) >= kBattleWeaponBins.size() ? std::string{} : std::string(kBattleWeaponBins[static_cast<size_t>(model)]);
}

std::shared_ptr<ModelHandle> ModelCache::load_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, int char_id, int part) {
  if (char_id < 0 || char_id >= 44 || part < 0) return nullptr;
  return weapon_part(vfs, meshes, player_weapon_bin(char_id), part);
}

// A monster's weapon, present when its weapon item and weapon model are both non-zero. The texture and the
// mesh come from the MODEL's own weapon .bin - the third entry of its file-table triple {.ltd, .obj, weapon .bin} (3195 + 3 m, kBattleWeaponBins):
// pair `item byte 0 & 0x7F` (TIM 4bpp 128x85 -> VRAM (416, 256 + 85 slot), CLUT row; mesh -> the weapon object), exactly like a PC's pack.
// The 17 battle/wepon/*.kmd (file-table 3985 + weapon_model) only size the buffers; each equals one pair's mesh. It is then
// hung on the model's two mount records.
std::shared_ptr<ModelHandle> ModelCache::load_enemy_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, int model, int part) {
  if (model < 0 || static_cast<size_t>(model) >= kBattleWeaponBins.size() || part < 0) return nullptr;
  const std::string bin = enemy_weapon_bin(model);
  if (bin.empty()) {
    log::warn("battle: enemy weapon: model {} has no weapon .bin", model);
    return nullptr;
  }
  return weapon_part(vfs, meshes, bin, part);
}

std::shared_ptr<ModelHandle> ModelCache::weapon_part(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const std::string& bin, int part) {
  const std::string key = bin + "#" + std::to_string(part);
  if (auto it = weapons.find(key); it != weapons.end()) return it->second;
  std::shared_ptr<ModelHandle> handle = make_prefetched(meshes, true, key);
  if (!handle) handle = build_weapon(vfs, meshes, bin, part);
  weapons[key] = handle;
  return handle;
}

std::shared_ptr<ModelHandle> ModelCache::build_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const std::string& bin, int part) {
  std::optional<assets::ModelAsset> asset = read_weapon(vfs, bin, part);
  return asset ? Actor::make_resolved_model(meshes, std::move(*asset)) : nullptr;
}

std::optional<assets::ModelAsset> ModelCache::read_weapon(const vfs::Vfs& vfs, const std::string& bin, int part) {
  std::string err;
  const auto raw = vfs.read("cdrom/" + bin);
  const auto pack = raw ? import::load_weapon_pack(*raw, &err) : std::nullopt;
  if (!pack || part >= static_cast<int>(pack->parts.size())) {
    log::warn("battle: weapon {}#{}: {}", bin, part, !raw ? "file not found" : err.empty() ? "no such part" : err);
    return std::nullopt;
  }
  const assets::WeaponPart& weapon = pack->parts[static_cast<size_t>(part)];
  assets::ModelAsset weapon_asset;
  weapon_asset.kind = "weapon";
  weapon_asset.source = weapon_texture_source(bin, part);
  weapon_asset.skeleton.joints.emplace_back();
  weapon_asset.textures.push_back(weapon.texture);
  weapon_asset.main_texture_count = 1;
  weapon_asset.meshes.push_back(weapon.mesh);
  log::info("battle: weapon {} part {} (texture {}x{})", bin, part, weapon.texture.width, weapon.texture.height);
  render::resolve_model_textures(weapon_asset);
  return weapon_asset;
}

void ModelCache::reload_weapons(const vfs::Vfs& vfs, render::MeshPipeline& meshes) {
  for (auto& [key, live] : weapons) {
    if (!live) continue;
    const size_t hash = key.rfind('#');
    std::shared_ptr<ModelHandle> fresh = build_weapon(vfs, meshes, key.substr(0, hash), std::stoi(key.substr(hash + 1)));
    if (!fresh || !fresh->gpu) continue;
    live->asset = std::move(fresh->asset);
    live->gpu = std::move(fresh->gpu);
  }
}

// S5 (state scripts) of the model and the 'atk' / camera data of its at0.prd.
std::shared_ptr<ModelScripts> ModelCache::load_scripts_for(const vfs::Vfs& vfs, const CamBank* bank, const std::string& stem) {
  if (stem.empty()) return nullptr;
  if (auto it = scripts.find(stem); it != scripts.end()) return it->second;
  auto loaded = std::make_shared<ModelScripts>();
  const std::string folder = stem.substr(0, stem.find_last_of('/'));
  if (auto obj = vfs.read("cdrom/" + stem + ".obj")) {
    loaded->has_s5 = bseq::load_model_table(*obj, loaded->s5);
    // S6 = the model's own DRP (44 monsters): sequences mag1..mag4 (type 0x10) + camera records mag<k> / big<k> (0x0C). The monster's cast script
    // (S5 entry 5) starts 'mag<k>' with anmseq 0x68 / 0x7D.
    if (obj->size() >= 32) {
      u32 s6 = 0;
      std::memcpy(&s6, obj->data() + 4 + 5 * 4, 4);
      if (s6 != 0 && s6 + 12 < obj->size()) {
        loaded->cam.add_drp(ByteSpan(obj->data() + s6, obj->size() - s6));
        // Every model load opens the model's S6 DRP as a file of the registry (from the first battle frame). The headers
        // stay registered while the model lives, so the anmseq op 0x1D walk (by literal name) finds them for every actor.
        static const import::Vram kNoVram;
        auto fx = std::make_shared<assets::RoomEffects>(import::load_battle_effects(ByteSpan(obj->data() + s6, obj->size() - s6), {}, kNoVram, {}));
        if (!fx->emitters.empty()) loaded->s6fx = std::move(fx);
      }
    }
  }
  if (auto at0 = vfs.read("cdrom/" + folder + "/at0.prd")) {
    loaded->has_atk = bseq::load_atk_table(*at0, loaded->atk);
    loaded->cam.add_drp(*at0);
    loaded->at0 = std::move(*at0);
  }
  loaded->cam.set_parent(bank);
  // Op 0x44 reads the blend table of S5 entry 0.
  if (loaded->has_atk) loaded->atk.set_model_table(&loaded->s5);
  scripts[stem] = loaded;
  return loaded;
}

}  // namespace oc::game
