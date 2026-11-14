#pragma once
// ModelCache: the battle's loaded unit models, weapon models and per-model script blocks, keyed by model folder stem, plus the
// file lookups that name them (character / monster model folders, weapon packs).
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "assets/room_effects.h"
#include "core/types.h"
#include "game/actor.h"
#include "game/battle/battle_camera.h"
#include "game/battle/battle_seq.h"

namespace oc::battle {
struct Unit;
}
namespace oc::data {
struct GameTables;
}
namespace oc::render {
class MeshPipeline;
}
namespace oc::vfs {
class Vfs;
}

namespace oc::game {

// The S3 shading entries of a battle model as the stock model draw reads them, whatever the stage: flag bit 0 = part hidden, 0x80 with
// 0x20 / 0x40 = env-mapped with the model's own page, 0x20 / 0x40 without 0x80 = the stock env page (replace / over, ABR = flags bits
// 2-3; the caller supplies the page), and the part colour word = model colour x the entry's rgb >> 7 (0x80 neutral).
void model_shading_entries(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp);
// Battle models are painter-sorted per face like every PS1 model: one ordering-table slot per face = average z >> 4, or the NEAREST
// corner's z >> 4 for the parts whose shading flag has 0x10 (hair / capes laid over the body).
void model_ot_faces(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp);

// Scripts and resources shared by every unit that uses the same model folder: the model's S5 state-script block, the external 'atk'
// table and the camera entries (type 0x0C / 0x10: the atk0..3 sequences and their shots) of its at0.prd, the raw at0.prd (sound
// registration happens per battle, with the weapon class) and the model's own S6 DRP (44 monsters), whose emitter headers stay
// registered for the whole battle.
struct ModelScripts {
  bseq::Table s5, atk;
  bool has_s5 = false, has_atk = false;
  CamBank cam;
  Bytes at0;
  std::shared_ptr<const assets::RoomEffects> s6fx;
};

struct ModelCache {
  std::map<std::string, std::shared_ptr<ModelHandle>> models;
  // "<pack member path>#<part>" (battle/wepon/sword.bin#3): a PC's and a monster's use of one pack part share it.
  std::map<std::string, std::shared_ptr<ModelHandle>> weapons;
  std::map<std::string, std::shared_ptr<ModelScripts>> scripts;
  // Used when a model has no 'atk' table.
  bseq::Table fallback_atk;

  // Drops every loaded model, weapon and script block and the prefetch (the fallback table stays).
  void clear();

  // The CPU half of the model loads a battle is about to make (files, import, the texture mods' decode, the bled uploads), run on a
  // worker from the battle setup on, so the load steps only create the GPU objects. Keyed like `models` / `weapons`; not used when the
  // texture mods changed meanwhile.
  struct PrefetchedModel {
    assets::ModelAsset asset;
    render::ModelUploads uploads;
  };
  struct Prefetched {
    unsigned generation = 0;
    std::map<std::string, PrefetchedModel> models, weapons;
  };
  // Starts the worker for the model stems and weapon pack parts ("battle/wepon/sword.bin", 3) not loaded yet.
  void prefetch(const vfs::Vfs& vfs, std::vector<std::string> stems, std::vector<std::pair<std::string, int>> weapon_parts);
  // False while the worker runs (a load step taking from it would wait).
  bool prefetch_ready() const;

  static std::string player_model(const vfs::Vfs& vfs, int char_id);
  // A character mods add by name (tables\char.txt `[name]`) whose mod adds cdrom/battle/player/<name>/<name>.obj (with its at0.prd
  // beside it); empty when it adds none (the character then wears its model byte's model, unit_model_stem).
  static std::string named_player_model(const vfs::Vfs& vfs, std::string_view name);
  // A monster's model: one mods add by name (tables\monster.txt `[name]`) whose mod adds cdrom/battle/monster/<name>/<name>.obj uses it
  // (named_monster_model; a model the game ships under that name does not count), any other its model byte (kBattleModels).
  static std::string monster_model(const vfs::Vfs& vfs, const data::GameTables* tables, int monster_id);
  static std::string named_monster_model(const vfs::Vfs& vfs, std::string_view name);
  // A monster's weapon pack: a model of its own takes <stem>.bin beside it ("" = none), any other the model byte's (weapon_bin).
  static std::string monster_weapon_bin(const vfs::Vfs& vfs, const data::GameTables* tables, int monster_id);
  // The unit's model folder: the character's, or the creature's after a Dopplegang form change.
  static std::string unit_model_stem(const vfs::Vfs& vfs, const data::GameTables* tables, const battle::Unit& unit);
  // The model number the animation scripts read (GetModelNum): a PC's kind byte (the record's model byte: Glenn with the Einlanzer pair
  // is 0x32), else the monster record's model (the creature's after a Dopplegang form change).
  static int model_number(const data::GameTables& tables, const battle::Unit& unit);
  // The hit-sound / hit-emitter group digit of a unit: the equipped weapon's material tier & 0xF (the same byte as the weapon model part;
  // NOT the crit class), -1 for monsters (their "_s00" entries are not renamed).
  static int weapon_digit(const data::GameTables* tables, const battle::Unit& unit);
  // The weapon pack of a kind byte: a monster's model, or the kind of a PC (a character id, a Pip form, a body) ("" = none).
  static std::string weapon_bin(int model);
  // The CPU half of a load (texture mods resolved): any thread.
  static std::optional<assets::ModelAsset> read_model(const vfs::Vfs& vfs, const std::string& stem, std::string* err);
  static std::optional<assets::ModelAsset> read_weapon(const vfs::Vfs& vfs, const std::string& bin, int part);

  std::shared_ptr<ModelHandle> load_model_for(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const std::string& dir_stem);
  std::shared_ptr<ModelHandle> load_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, int kind, int part);
  std::shared_ptr<ModelHandle> load_enemy_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const data::GameTables* tables,
                                                 int monster_id, int part);
  // Part `part` of the weapon pack `bin` ("battle/wepon/sword.bin"), cached; build_weapon is the uncached load.
  std::shared_ptr<ModelHandle> weapon_part(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const std::string& bin, int part);
  static std::shared_ptr<ModelHandle> build_weapon(const vfs::Vfs& vfs, render::MeshPipeline& meshes, const std::string& bin, int part);
  // A texture mod switched on / off: every loaded weapon is built again through the VFS and swapped INTO its handle (the units keep pointing at it).
  void reload_weapons(const vfs::Vfs& vfs, render::MeshPipeline& meshes);
  std::shared_ptr<ModelScripts> load_scripts_for(const vfs::Vfs& vfs, const CamBank* bank, const std::string& dir_stem);

 private:
  std::shared_ptr<ModelHandle> make_prefetched(render::MeshPipeline& meshes, bool weapon, const std::string& key);

  std::future<Prefetched> prefetch_job_;
  Prefetched prefetched_;
};

}  // namespace oc::game
