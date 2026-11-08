#pragma once
// Models from elsewhere on a field entity (field script op FE D0 MODEL_SOURCE): a room model number can stand for a playable character's
// field model, any field player model, another room's model, or a battle model (a monster's, or a playable character's). The readers
// here are CPU only and thread-safe; FieldWorld caches and uploads what they return.
//
// A battle model is made to behave like a field model: its battle animation scripts (S5) are dropped and its clips are renumbered to
// the field's slots, so the same field script moves a field model and a battle model alike:
//   s4 0 / 1 / 4..7 = its idle (stand), 2 / 3 = its run (run and walk), 8 = its battle stance;
//   ex k (script animation 0x80 + k) = its own clip k (hits, KO, dodge, cast ...); ex 0x20 + k = clip k of its at0.prd (attacks).
// Which clip is its idle, run and stance is read from its own scripts: the clip the idle script (entry 1) starts, and in the
// approach script (entry 2) the clip in force when it starts moving and the clip it ends on.
#include <optional>
#include <string>

#include "assets/model.h"
#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

struct ModelSource {
  enum Kind : int {
    // id = character (0..0x2B): its field model, through its model byte (a D9 disguise shows)
    kCharacter = 0,
    // id = field model byte (0..0xFF): fieldobj/player/<folder> by the disc's character model records
    kFieldModel = 1,
    // id = room id (as map jumps name it), sub = model k of that room's list
    kRoomModel = 2,
    // id = battle model id (0..223: party models, then monsters; a monster record's model byte)
    kBattleModel = 3,
    // id = character (0..0x2B): its battle model
    kCharacterBattle = 4
  };
  int kind = kCharacter;
  int id = 0;
  int sub = 0;
};

// Field scale of a battle model: a character's field model is about 1.97 times the height of its battle model, so a battle model on a
// field entity is drawn that much larger.
constexpr float kBattleModelFieldScale = 1.97f;

// The clips of a battle model the field uses (indices of its own s4 clips); -1 = not found.
struct BattleFieldClips {
  int stand = -1, run = -1, stance = -1;
};
// Read from the model's S5 scripts (the .obj bytes); defaults where a script names none: stand 1, run 3, stance 2.
BattleFieldClips battle_field_clips(ByteSpan obj);
// Renumbers a battle model's clips for the field and drops its animation scripts (see the top of this file).
void adapt_battle_model(assets::ModelAsset& asset, const BattleFieldClips& clips);

// The battle model stem of a playable character (battle/player/<folder>, the "1" / "2" folder variants), empty when none.
std::string character_battle_stem(const vfs::Vfs& files, int char_id);
// A battle model (stem under cdrom/, no ".obj") ready for the field: read_model_files + battle_field_clips + adapt_battle_model.
std::optional<assets::ModelAsset> read_battle_model_for_field(const vfs::Vfs& files, const std::string& stem, bool hd_battle_texture,
                                                              std::string* err);
// Model k of room `room` (its stem), with its own texture pages and transparency modes.
std::optional<assets::ModelAsset> read_room_model(const vfs::Vfs& files, const std::string& room, int k, std::string* err);
// Its Mdl bytes alone (what LOAD_ANIMATION decodes a pack against).
std::optional<Bytes> read_room_model_mdl(const vfs::Vfs& files, const std::string& room, int k);

}  // namespace oc::game
