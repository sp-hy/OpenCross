// ModelCache: per-unit model, weapon and script file lookup and loading.
#include "game/battle/battle_model_cache.h"
#include "game/battle/model_table.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <string_view>

#include "battle/battle.h"
#include "core/log.h"
#include "data/table_rows.h"
#include "data/tables.h"
#include "game/model_files.h"
#include "import/model.h"
#include "import/room_effects.h"
#include "render/mesh_pipeline.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"
namespace oc::game {


namespace {

// Folder under battle/player/ (and fieldobj/player/) of every playable character id 0..43, same order as the field table.
constexpr const char* kCharFolders[44] = {
    "selju",  "kid",     "guill",  "ishito",   "slash",   "jakotu", "riddle", "carshu", "zoah",    "malchera", "colcha",
    "lutianna", "poshul", "lazzuly", "zappa",    "ocha",    "radius", "farga",  "obachan", "gren",   "lena",     "miki",
    "harley", "jyaness", "dragon", "hosi",      "spri",    "dan",    "kabuo",  "rablue", "jilbelt", "hone",     "kinoko",
    "ilenes", "mell",    "liia",   "banc",      "sunef",   "steena", "doc",    "gyada",  "pierre",  "olha",     "tumaru"};
}  // namespace

// Part colour: the per-part colour word is (model colour x entry rgb) >> 7 for every part (the env parts' word is multiplied the same way),
// so an entry rgb other than 0x80 scales that part's texels: sky's act0 / act1 (tech/sky.prd, 0xFC 0xFC 0xFC on parts 0 / 1) draw at ~2x,
// gyada / jakotu / malchera / pierre / zoah's env-over parts (0x60..0x70) at 0.75..0.88. Op 0xA0 writes the same word and overrides this.
void model_shading_entries(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp) {
  for (size_t k = 0; k < asset.shading.size() && k < 32; ++k) {
    const assets::ShadingEntry& entry = asset.shading[k];
    if (entry.flags & 1) dp.hidden_meshes |= 1u << k;
    if ((entry.flags & 0x80) && (entry.flags & 0x60)) dp.env_meshes |= 1u << k;
    if ((entry.flags & 0x60) && !(entry.flags & 0x80)) {
      if (entry.flags & 0x40) dp.stock_env_over |= 1u << k;
      else dp.stock_env_replace |= 1u << k;
      dp.stock_env_abr[k] = static_cast<u8>((entry.flags >> 2) & 3);
    }
    if (entry.r != 0x80 || entry.g != 0x80 || entry.b != 0x80) {
      dp.part_mask |= 1u << k;
      dp.part_tint[k][0] = entry.r / 128.0f, dp.part_tint[k][1] = entry.g / 128.0f, dp.part_tint[k][2] = entry.b / 128.0f;
    }
  }
}

void model_ot_faces(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp) {
  dp.ot_faces = true;
  for (size_t k = 0; k < asset.shading.size() && k < 32; ++k)
    if (asset.shading[k].flags & 0x10) dp.near_meshes |= 1u << k;
}

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

std::string ModelCache::named_monster_model(const vfs::Vfs& vfs, std::string_view name) {
  if (name.empty()) return {};
  std::string stem = std::format("battle/monster/{}/{}", name, name);
  return vfs.added("cdrom/" + stem + ".obj") ? stem : std::string{};
}

// A monster mods add by name with a model of its own (named_monster_model); else monster.bin byte 0x2D = battle model id: file-table
// entry 3194 + 3 * id is the model's .obj (model_table.h).
std::string ModelCache::monster_model(const vfs::Vfs& vfs, const data::GameTables* tables, int monster_id) {
  if (monster_id < 0 || static_cast<size_t>(monster_id) >= tables->enemies.size()) return {};
  if (std::string own = named_monster_model(vfs, data::table_rows().name_of("monster", static_cast<size_t>(monster_id))); !own.empty())
    return own;
  const size_t model = tables->enemies[static_cast<size_t>(monster_id)].model();
  return model < kBattleModels.size() ? std::string(kBattleModels[model]) : std::string{};
}

std::string ModelCache::monster_weapon_bin(const vfs::Vfs& vfs, const data::GameTables* tables, int monster_id) {
  if (monster_id < 0 || static_cast<size_t>(monster_id) >= tables->enemies.size()) return {};
  if (const std::string own = named_monster_model(vfs, data::table_rows().name_of("monster", static_cast<size_t>(monster_id))); !own.empty())
    return vfs.added("cdrom/" + own + ".bin") ? own + ".bin" : std::string{};
  return weapon_bin(static_cast<int>(tables->enemies[static_cast<size_t>(monster_id)].model()));
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

std::string ModelCache::named_player_model(const vfs::Vfs& vfs, std::string_view name) {
  if (name.empty()) return {};
  std::string stem = std::format("battle/player/{}/{}", name, name);
  return vfs.added("cdrom/" + stem + ".obj") ? stem : std::string{};
}

// The model stem of a unit.
std::string ModelCache::unit_model_stem(const vfs::Vfs& vfs, const data::GameTables* tables, const battle::Unit& unit) {
  if (unit.body >= 0) return battle::pip_form(unit.body) ? std::string(kBattleModels[static_cast<size_t>(unit.body)]) : player_model(vfs, unit.body);
  // A character mods add wears a battle model of its own (named_player_model) while its kind is its record's model byte (no disguise).
  if (unit.pc && unit.form < 0 && unit.src_id >= data::kNumChars && tables) {
    const auto chars = tables->char_sets.find("char");
    const size_t id = static_cast<size_t>(unit.src_id);
    if (chars != tables->char_sets.end() && id < chars->second.rec.size() && unit.kind == chars->second.rec[id].model())
      if (std::string own = named_player_model(vfs, data::table_rows().name_of("char", id)); !own.empty()) return own;
  }
  // A PC's model is loaded from its kind byte (file group 0xE, kind * 3 + 3): Pip's forms are battle/player/tu_p .. tu_mm, Serge's Lynx body
  // (0x31) is sel_yam2. A kind equal to the character id keeps the character's player folder.
  if (unit.pc && unit.form < 0 && (unit.kind != unit.src_id || unit.src_id >= 44) && static_cast<size_t>(unit.kind) < kBattleModels.size() &&
      !kBattleModels[static_cast<size_t>(unit.kind)].empty())
    return std::string(kBattleModels[static_cast<size_t>(unit.kind)]);
  return unit.pc && unit.form < 0 ? player_model(vfs, unit.src_id) : monster_model(vfs, tables, unit.form >= 0 ? unit.form : unit.src_id);
}

int ModelCache::model_number(const data::GameTables& tables, const battle::Unit& unit) {
  if (unit.pc && unit.form < 0) return unit.kind;
  return static_cast<int>(tables.enemies[static_cast<size_t>(unit.form >= 0 ? unit.form : unit.src_id)].model());
}

int ModelCache::weapon_digit(const data::GameTables* tables, const battle::Unit& unit) {
  if ((!unit.pc && unit.body < 0) || unit.form >= 0) return -1;
  const data::GearRec* gear = tables ? tables->gear_rec(unit.eq.weapon_id) : nullptr;
  return gear ? (gear->tier() & 0xF) : 0;
}

std::string ModelCache::weapon_bin(int model) {
  return model < 0 || static_cast<size_t>(model) >= kBattleWeaponBins.size() ? std::string{} : std::string(kBattleWeaponBins[static_cast<size_t>(model)]);
}

std::shared_ptr<ModelHandle> ModelCache::load_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, int kind, int part) {
  const std::string bin = weapon_bin(kind);
  if (bin.empty() || part < 0) return nullptr;
  return weapon_part(vfs, meshes, bin, part);
}

// A monster's weapon, present when its weapon item and weapon model are both non-zero. The texture and the
// mesh come from the MODEL's own weapon .bin - the third entry of its file-table triple {.ltd, .obj, weapon .bin} (3195 + 3 m, kBattleWeaponBins):
// pair `item byte 0 & 0x7F` (TIM 4bpp 128x85 -> VRAM (416, 256 + 85 slot), CLUT row; mesh -> the weapon object), exactly like a PC's pack.
// The 17 battle/wepon/*.kmd (file-table 3985 + weapon_model) only size the buffers; each equals one pair's mesh. It is then
// hung on the model's two mount records.
std::shared_ptr<ModelHandle> ModelCache::load_enemy_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const data::GameTables* tables,
                                                           int monster_id, int part) {
  if (part < 0) return nullptr;
  const std::string bin = monster_weapon_bin(vfs, tables, monster_id);
  if (bin.empty()) {
    log::warn("battle: enemy weapon: monster {} has no weapon .bin", monster_id);
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

// S5 (state scripts) of the model and the 'atk' / camera data of its attack pack (attack_pack_of: its own at0.prd or the one the file table lends it).
std::shared_ptr<ModelScripts> ModelCache::load_scripts_for(const vfs::Vfs& vfs, const CamBank* bank, const std::string& stem) {
  if (stem.empty()) return nullptr;
  if (auto it = scripts.find(stem); it != scripts.end()) return it->second;
  auto loaded = std::make_shared<ModelScripts>();
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
  if (auto at0 = vfs.read("cdrom/" + attack_pack_of(stem))) {
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
