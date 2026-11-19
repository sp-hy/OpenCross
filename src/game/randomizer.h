#pragma once
// The randomizer (feature Randomizer, docs/formats/mods.md section 8.1): every placement is a pure function of the seed, the settings and
// the stable ids of the game's data (a treasure's flag, a ditem row and slot, a shop row and index, a scene), so nothing is stored in a save
// and the same seed gives the same game and the same spoiler log on every machine with the same data. gather() reads the pools from the
// room-script scan and the tables; build() shuffles them; the Plan answers the reads the game makes (a chest's literal, a drop, a shop row,
// an encounter group, the New Game state).
#include <array>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "battle/battle.h"
#include "core/types.h"
#include "data/tables.h"

namespace oc::features {
struct Resolved;
}
namespace oc::save {
struct SaveSlot;
}
namespace oc::ui {
struct MenuRules;
}

namespace oc::game {
struct ScriptScan;
}

namespace oc::game::randomizer {

struct Settings {
  u32 seed = 1;
  // 0 = an entry only swaps with one of similar value and stage, 100 = anywhere in its pool.
  int spread = 0;
  bool items = false;
  bool elements = false;
  bool enemies = false;
  bool start_gear = false;
  bool start_elements = false;
  // The randomizer is on while any switch is.
  bool enabled() const { return items || elements || enemies || start_gear || start_elements; }
  bool operator==(const Settings&) const = default;
};
// The [Randomizer] feature's values of a run.
Settings settings_from(const features::Resolved& features);

// Things in the field scripts' numbering: an item below 0x200, an element 0x200 + its id. A drop word's element bit 0x8000 becomes 0x200.
inline int drop_thing(u16 word) { return (word & 0x8000) ? 0x200 + (word & 0x1FF) : word; }
inline u16 thing_drop(int thing) { return thing >= 0x200 ? static_cast<u16>(0x8000 | (thing - 0x200)) : static_cast<u16>(thing); }

// One place the game puts a thing. `site` is stable within its pool (treasure: the flag key; drop: row * 4 + slot; shop: row * 64 +
// index; encounter: the scene), `slot` tells apart two things of one site. `value`: price in G times the copies, -1 unknown; `stage`:
// how far into the game the place lies (any scale that orders; gather uses the area stage), -1 unknown. `where`: the spoiler log's label.
struct Entry {
  u32 site = 0;
  int slot = 0;
  int thing = 0;
  int value = -1;
  int stage = -1;
  std::string where;
};

enum class Pool : u8 { ChestItems, ChestElements, DropItems, DropElements, SmithStock, ElementStock, Encounters };
inline constexpr size_t kPools = 7;
const char* pool_name(Pool pool);

// A literal word in a room's script code that holds a treasure's id (script::ScanEvent::value_sites).
struct Literal {
  u32 pos = 0;
  u32 treasure = 0;
  int thing = 0;
};

// An area (room prefix + world letter, "alu_a"): the means of its own normal encounters' enemies (each record once), and its stage: the
// percentile (0..1000000) of that mean HP among the areas with encounters.
struct Area {
  std::string name;
  int hp = 0;
  int power = 0;
  int gold = 0;
  int stage = -1;
};

// A normal encounter group: its area and the drop rows of its enemy groups (-1 = no group).
struct Encounter {
  int scene = 0;
  std::string area;
  std::array<int, 3> drop_rows{-1, -1, -1};
};

// A starting piece of a character: equipment slot 0 weapon, 1 armour, 2..4 accessories, or an element of the grid (slot -1).
struct StartPiece {
  int character = 0;
  int slot = 0;
  int thing = 0;
};
// A thing a starting piece may become: gear kind 0 weapon, 1 armour, 2 accessory (`equip` bit = character id), or an element of `level`
// (`equip` from menu_pg).
struct Choice {
  int thing = 0;
  int kind = 0;
  int level = 0;
  int value = -1;
  u64 equip = 0;
};

struct Pools {
  std::array<std::vector<Entry>, kPools> entries;
  std::map<std::string, std::vector<Literal>, std::less<>> literals;
  std::map<std::string, Area, std::less<>> areas;
  std::vector<Encounter> encounters;
  std::vector<data::DropRow> drops;
  std::vector<std::vector<u16>> el_shop, smith_shop;
  std::vector<StartPiece> start;
  std::vector<Choice> choices;
  // Spoiler log names: things by id (0x200 + element id for elements), enemies by record, characters by id.
  std::vector<std::string> names;
  std::vector<std::string> enemy_names;
  std::vector<std::string> char_names;
  std::vector<std::array<int, 3>> scene_enemies;
  std::string thing_name(int thing) const;
};

// Reads the pools: the scan's treasures (key items, pickups that also give a key item and gives without a literal of their own left
// out), ditem, the element shops and the smiths (`smith_rows` = waa_shop as the shop reads it), the normal encounter groups (a scene only
// encounter ids below 0x200 reach, never a direct scene start, not a boss), char.bin's starting gear and elements. `rules` gives the
// element prices and who may equip an element (null: unknown).
Pools gather(const ScriptScan& scan, const data::GameTables& tables, const ui::MenuRules* rules, const std::vector<std::vector<u16>>& smith_rows);

// The permutation of one pool: out[i] = the index of the entry whose thing goes to entry i. Entries are ranked by the mean of their value
// and stage percentiles (the one known when the other is not; an entry with neither keeps its thing below spread 100); with spread < 100
// a thing moves less than a window of 4 % + 0.96 % x spread of the ranked entries from its rank, with 100 it is a plain shuffle. `stream`
// selects the pool's random stream.
std::vector<size_t> shuffle(std::span<const Entry> entries, u32 seed, u32 stream, int spread);

class Plan {
 public:
  const Settings& settings() const { return settings_; }
  // The thing treasure `key` gives instead of `original`.
  int treasure(u32 key, int original) const;
  // Rewrites the room's literal words of the moved treasures (the chest's message, stock test and give then agree). Count of words changed.
  int patch_room(std::string_view stem, Bytes& code) const;
  // ditem with the moved drops and steals; null when neither tier is on.
  std::shared_ptr<const std::vector<data::DropRow>> drops() const { return drops_; }
  // A shop row as the shop reads it (`elements`: an element shop, ids without 0x200; else a smith).
  std::vector<int> stock(bool elements, int shop_id, std::vector<int> row) const;
  // The encounter group that comes instead of `scene`, and the tuning of its enemy groups for the area it lands in.
  int scene(int original) const;
  std::array<battle::EnemyTuning, 3> tuning(int original_scene) const;
  bool moved_scene(int original) const { return scene(original) != original; }
  // The New Game state: starting gear and elements (the fresh game FE 3F built).
  void start_game(save::SaveSlot& slot) const;
  // The spoiler log (docs/formats/mods.md 8.1).
  const std::string& spoiler() const { return spoiler_; }
  // Per pool: entries, entries that moved.
  std::array<std::pair<int, int>, kPools> counts() const { return counts_; }

 private:
  friend Plan build(const Settings& settings, const Pools& pools);
  Settings settings_;
  std::map<std::pair<u32, int>, int> treasures_;
  std::map<std::string, std::vector<Literal>, std::less<>> literals_;
  std::shared_ptr<const std::vector<data::DropRow>> drops_;
  std::vector<std::vector<int>> el_rows_, smith_rows_;
  std::map<int, int> scenes_;
  std::map<int, std::array<battle::EnemyTuning, 3>> tuning_;
  std::map<std::pair<int, int>, int> start_gear_;
  std::map<std::pair<int, int>, int> start_elements_;
  std::string spoiler_;
  std::array<std::pair<int, int>, kPools> counts_{};
};

Plan build(const Settings& settings, const Pools& pools);

}  // namespace oc::game::randomizer
