// The randomizer's pools (randomizer.h gather): what the game places where, read from the room-script scan and the tables.
#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "data/battlefields.h"
#include "game/randomizer.h"
#include "game/room_scan.h"
#include "script/script_scan.h"
#include "ui/menu/game_state.h"

namespace oc::game::randomizer {

namespace {

constexpr int kCharacters = 44;
constexpr int kShopRowWidth = 64;

// "alu_a": the room prefix and its world letter.
std::string area_key(const std::string& stem) { return stem.substr(0, std::min<size_t>(stem.size(), 5)); }

std::string area_label(const std::string& stem) {
  const char* name = data::area_name(stem);
  return std::format("{} [{}]", name ? name : stem.substr(0, 3), area_key(stem));
}

int item_value(const data::GameTables& tables, const ui::MenuRules* rules, int thing) {
  if (thing >= 0x200) {
    const ui::PgRow* row = rules ? rules->pg_row(thing - 0x200) : nullptr;
    return row && row->price() > 0 ? row->price() : -1;
  }
  const data::ItemInfo* info = tables.item(thing);
  return info && info->price() > 0 ? info->price() : -1;
}

}  // namespace

Pools gather(const ScriptScan& scan, const data::GameTables& tables, const ui::MenuRules* rules, const std::vector<std::vector<u16>>& smith_rows) {
  Pools pools;
  pools.drops = tables.drops;
  pools.el_shop = tables.el_shop;
  pools.smith_shop = smith_rows;
  for (int id = 0; id < 0x200; ++id) pools.names.push_back(tables.item_name(id));
  for (int id = 0; id < 0x200; ++id) pools.names.push_back(tables.element_name(id) + (script::give_count(0x200 + id) > 1 ? " x5" : ""));
  for (size_t id = 0; id < tables.enemies.size(); ++id) pools.enemy_names.push_back(tables.enemy_name(static_cast<int>(id)));
  for (int id = 0; id < kCharacters; ++id) pools.char_names.push_back(tables.char_name(id));
  for (const data::SceneRec& scene : tables.scenes) {
    std::array<int, 3>& enemies = pools.scene_enemies.emplace_back();
    for (int group = 0; group < 3; ++group)
      enemies[static_cast<size_t>(group)] =
          scene.enemy_id(group) != 0xFF && scene.enemy_count(group) != 0 && scene.enemy_id(group) < tables.enemies.size() ? scene.enemy_id(group) : -1;
  }

  // The story anchors: per area the lowest story value one of its rooms sets.
  std::map<std::string, int> anchors;
  for (const RoomScan& room : scan.rooms)
    for (const script::ScanEvent& event : room.scan.events)
      if (event.kind == script::EventKind::Scenario)
        for (int value : event.values) {
          auto [found, added] = anchors.try_emplace(area_key(room.stem), value);
          if (!added) found->second = std::min(found->second, value);
        }

  // Normal encounter groups: the scenes reached only through an encounter id (A3 below 0x200, the variant roll), never as a scene started
  // directly, none a boss; each at home in the area that fields it most.
  std::map<int, std::map<std::string, int>> fielded;
  std::set<int> scripted;
  for (const RoomScan& room : scan.rooms)
    for (const script::ScanEvent& event : room.scan.events) {
      if (event.kind != script::EventKind::Battle) continue;
      for (int battle_id : event.values) {
        const bool encounter = event.op == 0xA3 && battle_id < 0x200;
        for (int scene : battle_scenes(tables, battle_id)) {
          if (encounter) ++fielded[scene][area_key(room.stem)];
          else scripted.insert(scene);
        }
      }
    }
  std::map<std::string, std::string> first_stem;
  for (const RoomScan& room : scan.rooms) first_stem.try_emplace(area_key(room.stem), room.stem);
  std::map<std::string, std::set<int>> area_enemies;
  for (const auto& [scene, areas] : fielded) {
    if (scripted.count(scene) || static_cast<size_t>(scene) >= tables.scenes.size() || tables.scenes[static_cast<size_t>(scene)].boss()) continue;
    const auto home = std::max_element(areas.begin(), areas.end(), [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    Encounter& encounter = pools.encounters.emplace_back();
    encounter.scene = scene;
    encounter.area = home->first;
    for (size_t group = 0; group < 3; ++group) {
      const int enemy = pools.scene_enemies[static_cast<size_t>(scene)][group];
      if (enemy < 0) continue;
      encounter.drop_rows[group] = tables.enemies[static_cast<size_t>(enemy)].drop_row() & 0x3FFF;
      area_enemies[home->first].insert(enemy);
    }
  }
  for (const auto& [key, enemies] : area_enemies) {
    Area& area = pools.areas[key];
    area.name = area_label(first_stem[key]);
    s64 hp = 0, power = 0, gold = 0;
    for (int enemy : enemies) {
      const data::StatRec& rec = tables.enemies[static_cast<size_t>(enemy)];
      hp += rec.max_hp();
      power += (rec.atk() + rec.def() + rec.mag() + rec.mdef()) / 4;
      gold += rec.gold();
    }
    const s64 count = static_cast<s64>(enemies.size());
    area.hp = static_cast<int>(hp / count), area.power = static_cast<int>(power / count), area.gold = static_cast<int>(gold / count);
  }
  // An area's stage: the percentile of its enemies' mean HP among the areas with encounters; an area without them takes the percentile
  // of its story anchor among the anchored areas. (An anchor alone misleads: a place revisited late sets a late story value.)
  const auto percentiles = [](std::vector<std::pair<int, std::string>> ranked, std::map<std::string, int>& out) {
    std::sort(ranked.begin(), ranked.end());
    for (size_t i = 0; i < ranked.size(); ++i)
      out[ranked[i].second] = ranked.size() > 1 ? static_cast<int>(static_cast<s64>(i) * 1000000 / static_cast<s64>(ranked.size() - 1)) : 500000;
  };
  std::map<std::string, int> by_hp, by_anchor;
  std::vector<std::pair<int, std::string>> hp_rank, anchor_rank;
  for (const auto& [key, area] : pools.areas) hp_rank.push_back({area.hp, key});
  for (const auto& [key, value] : anchors) anchor_rank.push_back({value, key});
  percentiles(hp_rank, by_hp);
  percentiles(anchor_rank, by_anchor);
  const auto stage_of = [&](const std::string& stem) {
    const std::string key = area_key(stem);
    if (const auto found = by_hp.find(key); found != by_hp.end()) return found->second;
    const auto found = by_anchor.find(key);
    return found == by_anchor.end() ? -1 : found->second;
  };
  for (auto& [key, area] : pools.areas) area.stage = by_hp[key];
  // An encounter group ranks by its area's mean HP (its stage is that); an enemy's stage is the earliest area that fields it.
  std::map<int, int> enemy_stage;
  for (const Encounter& encounter : pools.encounters) {
    const Area& area = pools.areas[encounter.area];
    pools.entries[static_cast<size_t>(Pool::Encounters)].push_back({static_cast<u32>(encounter.scene), 0, encounter.scene, area.hp, -1, area.name});
    for (int enemy : pools.scene_enemies[static_cast<size_t>(encounter.scene)]) {
      if (enemy < 0) continue;
      auto [found, added] = enemy_stage.try_emplace(enemy, area.stage);
      if (!added) found->second = std::min(found->second, area.stage);
    }
  }

  // Treasures with a literal to rewrite in every site; a literal word two treasures share leaves both where they are, and so does a
  // pickup that also gives a key item (a story pickup stays whole). A give's value is its price times the copies it adds.
  std::map<std::pair<std::string, u32>, std::set<u32>> owners;
  const std::vector<Treasure> found_treasures = treasures(scan);
  for (const Treasure& treasure : found_treasures)
    for (const auto& [room, index] : treasure.sites)
      for (u32 pos : scan.rooms[room].scan.events[index].value_sites) owners[{scan.rooms[room].stem, pos}].insert(treasure.key);
  for (const Treasure& treasure : found_treasures) {
    bool movable = !treasure.sites.empty() && std::none_of(treasure.gives.begin(), treasure.gives.end(), [](int thing) {
      return script::give_class(thing) == script::GiveClass::KeyItem;
    });
    int stage = -1;
    std::string where;
    for (const auto& [room, index] : treasure.sites) {
      const script::ScanEvent& event = scan.rooms[room].scan.events[index];
      const std::string& stem = scan.rooms[room].stem;
      movable = movable && !event.value_sites.empty();
      for (u32 pos : event.value_sites) movable = movable && owners[{stem, pos}].size() == 1;
      stage = std::max(stage, stage_of(stem));
      if (where.find(stem) == std::string::npos) where += (where.empty() ? "" : ", ") + stem;
    }
    if (!movable) continue;
    where = std::format("{} ({})", area_label(scan.rooms[treasure.sites.front().first].stem), where);
    for (size_t slot = 0; slot < treasure.gives.size(); ++slot) {
      const int thing = treasure.gives[slot];
      const script::GiveClass kind = script::give_class(thing);
      if (kind != script::GiveClass::Item && kind != script::GiveClass::Element) continue;
      const Pool pool = kind == script::GiveClass::Item ? Pool::ChestItems : Pool::ChestElements;
      const int value = item_value(tables, rules, thing);
      pools.entries[static_cast<size_t>(pool)].push_back(
          {treasure.key, static_cast<int>(slot), thing, value > 0 ? value * script::give_count(thing) : value, stage, where});
      for (const auto& [room, index] : treasure.sites)
        for (u32 pos : scan.rooms[room].scan.events[index].value_sites) pools.literals[scan.rooms[room].stem].push_back({pos, treasure.key, thing});
    }
  }

  // Kill drops (slots 0 / 1) and steals (2 / 3) of the rows some enemy record rolls on.
  std::map<int, std::vector<int>> row_users;
  for (size_t enemy = 0; enemy < tables.enemies.size(); ++enemy) row_users[tables.enemies[enemy].drop_row() & 0x3FFF].push_back(static_cast<int>(enemy));
  static constexpr const char* kSlotNames[] = {"drop", "rare drop", "steal", "rare steal"};
  for (const auto& [row, users] : row_users) {
    if (static_cast<size_t>(row) >= tables.drops.size()) continue;
    int stage = -1;
    std::string names;
    for (int enemy : users) {
      if (const auto found = enemy_stage.find(enemy); found != enemy_stage.end()) stage = stage < 0 ? found->second : std::min(stage, found->second);
      const std::string& name = pools.enemy_names[static_cast<size_t>(enemy)];
      if (!name.empty() && names.find(name) == std::string::npos) names += (names.empty() ? "" : ", ") + name;
    }
    for (int slot = 0; slot < 4; ++slot) {
      const u16 word = tables.drops[static_cast<size_t>(row)].slot[static_cast<size_t>(slot)];
      if (word == 0) continue;
      const int thing = drop_thing(word);
      const Pool pool = thing >= 0x200 ? Pool::DropElements : Pool::DropItems;
      pools.entries[static_cast<size_t>(pool)].push_back({static_cast<u32>(row * 4 + slot), 0, thing, item_value(tables, rules, thing), stage,
                                                          std::format("row {} {} ({})", row, kSlotNames[slot], names)});
    }
  }

  // Shop stock: every listed entry of the element shops and the smiths.
  for (size_t row = 0; row < pools.el_shop.size(); ++row)
    for (size_t index = 0; index < pools.el_shop[row].size() && index < kShopRowWidth && pools.el_shop[row][index] != 0; ++index) {
      const int thing = 0x200 + pools.el_shop[row][index];
      pools.entries[static_cast<size_t>(Pool::ElementStock)].push_back(
          {static_cast<u32>(row * kShopRowWidth + index), 0, thing, item_value(tables, rules, thing), -1, std::format("element shop {}", row)});
    }
  for (size_t row = 0; row < smith_rows.size(); ++row)
    for (size_t index = 0; index < smith_rows[row].size() && index < kShopRowWidth && smith_rows[row][index] != 0; ++index) {
      const int thing = smith_rows[row][index];
      pools.entries[static_cast<size_t>(Pool::SmithStock)].push_back(
          {static_cast<u32>(row * kShopRowWidth + index), 0, thing, item_value(tables, rules, thing), -1, std::format("smith {}", row)});
    }

  // Starting gear and grid elements of char.bin, and what they may become: equipment with a price, the elements the shops sell.
  if (const auto set = tables.char_sets.find("char"); set != tables.char_sets.end()) {
    for (int character = 0; character < kCharacters; ++character) {
      const data::StatRec& rec = set->second.rec[static_cast<size_t>(character)];
      const std::array<u16, 5> gear = {rec.weapon(), rec.armour(), rec.accessory(0), rec.accessory(1), rec.accessory(2)};
      for (size_t slot = 0; slot < gear.size(); ++slot)
        if (gear[slot] != 0) pools.start.push_back({character, static_cast<int>(slot), gear[slot]});
      std::set<int> placed;
      for (u16 cell : rec.grid())
        if (!(cell & 0x8000) && (cell & 0x1FF) != 0 && placed.insert(cell & 0x1FF).second) pools.start.push_back({character, -1, 0x200 + (cell & 0x1FF)});
    }
  }
  for (int item = 0; item < static_cast<int>(tables.item_info.size()); ++item) {
    const data::ItemInfo& info = tables.item_info[static_cast<size_t>(item)];
    if (info.kind() > 2 || info.price() <= 0) continue;
    pools.choices.push_back({item, info.kind(), 0, info.price(), static_cast<u64>(info.equip_lo) | (static_cast<u64>(info.equip_hi) << 32)});
  }
  std::set<int> sold;
  for (const std::vector<u16>& row : tables.el_shop)
    for (u16 id : row)
      if (id != 0) sold.insert(id);
  for (int id : sold) {
    if (static_cast<size_t>(id) >= tables.elements.size()) continue;
    u64 equip = ~0ull;
    if (const ui::PgRow* row = rules ? rules->pg_row(id) : nullptr) {
      equip = 0;
      for (int character = 0; character < kCharacters; ++character)
        if (row->eligible(character)) equip |= 1ull << character;
    }
    pools.choices.push_back({0x200 + id, 0, tables.elements[static_cast<size_t>(id)].level(), item_value(tables, rules, 0x200 + id), equip});
  }
  return pools;
}

}  // namespace oc::game::randomizer
