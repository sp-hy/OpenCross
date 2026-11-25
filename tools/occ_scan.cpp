// occ scan <outdir> [--save <save.dat> <card> <slot>] [--room <stem>]: the static room-script scan (game/room_scan.h) as tables.
//   events.csv    every event of every room: gives, takes, gold, battles, joins, roster changes, story sets, exits
//   treasure.csv  the chests and pickups (gives whose flag the room tests at load), with their stable ids
//   battles.csv   every battle op with its encounter groups and the enemies in them
//   enemies.csv   per enemy record: the rooms whose battles can field it and the lowest story value guarding them
//   entities.csv  per entity: its role and the tests under which its INIT makes it inactive
//   sources.csv   per id a script gives: one-off (flagged) and other gives, shop and trading-post rows, enemies that drop or yield it
//   bestiary.csv  the Enemies page's species in number order with their records and places (data/bestiary.h)
//   areas.csv     the story areas in order with their story values, the story values the scan sees set in them, and their names
//   records.txt   what the record pages know (game/records_info.h): each character's places and last story value, the elements and key
//                 items whose only sources are one-off gives with a last story value, the treasures per area; with --save the
//                 characters, elements and key items that save has missed
//   summary.txt   counts, unresolved operands, the scan time
// --save marks each treasure opened or not from that memory-card slot's variables. --room prints one room's events.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "data/battlefields.h"
#include "data/tables.h"
#include "game/records_info.h"
#include "game/room_scan.h"
#include "import/text.h"
#include "save/cc_save.h"
#include "script/script_scan.h"
#include "ui/shop/shop_logic.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

std::string csv(std::string_view text) {
  if (text.find_first_of(",\"\n") == std::string_view::npos) return std::string(text);
  std::string out = "\"";
  for (char ch : text) out += ch == '"' ? std::string("\"\"") : std::string(1, ch);
  return out + "\"";
}

std::string give_name(const data::GameTables& tables, int id) {
  switch (script::give_class(id)) {
    case script::GiveClass::Item: return "item:" + tables.item_name(id);
    case script::GiveClass::Element: return "element:" + tables.element_name(id - 0x200) + (script::give_count(id) > 1 ? " x5" : "");
    case script::GiveClass::KeyItem: {
      const size_t key = static_cast<size_t>(id - 0x400);
      return "key:" + (key < tables.key_item_names.size() ? tables.key_item_names[key] : std::string("?"));
    }
    case script::GiveClass::None: break;
  }
  return std::format("none:{}", id);
}

std::string value_name(const data::GameTables& tables, const script::ScanEvent& event, int value) {
  switch (event.kind) {
    case script::EventKind::Give:
    case script::EventKind::Take: return give_name(tables, value);
    case script::EventKind::Join:
    case script::EventKind::Roster:
    case script::EventKind::Unroster: return tables.char_name(value);
    case script::EventKind::Exit: {
      const char* stem = import::room_name_for_id(value);
      return stem ? stem : std::to_string(value);
    }
    case script::EventKind::Battle:
    case script::EventKind::Gold:
    case script::EventKind::Scenario:
    case script::EventKind::SetFlag: break;
  }
  return std::to_string(value);
}

std::string join_values(const data::GameTables& tables, const script::ScanEvent& event, bool names) {
  std::string out;
  for (int value : event.values) out += (out.empty() ? "" : "|") + (names ? value_name(tables, event, value) : std::to_string(value));
  return out;
}

std::string join_conds(const std::vector<script::Cond>& conds) {
  std::string out;
  for (const auto& cond : conds) out += (out.empty() ? "" : " & ") + script::cond_text(cond);
  return out;
}

const char* role_name(script::EntityRole role) {
  switch (role) {
    case script::EntityRole::None: return "-";
    case script::EntityRole::Party: return "party";
    case script::EntityRole::Npc: return "npc";
    case script::EntityRole::Monster: return "monster";
  }
  return "?";
}

std::string story_text(const std::vector<script::Cond>& conds) {
  const script::StoryRange range = script::story_range(conds);
  return std::format("{},{}", range.lo, range.hi == 0x7FFF ? std::string() : std::to_string(range.hi));
}

std::string area_of(const std::string& stem) {
  const char* name = data::area_name(stem);
  return name ? name : "";
}

std::string world_tag(data::World world) { return world == data::World::Home ? "Home" : world == data::World::Another ? "Another" : ""; }

script::EntityRole role_of(const game::RoomScan& room, int entity) {
  return static_cast<size_t>(entity) < room.scan.entities.size() ? room.scan.entities[static_cast<size_t>(entity)].role : script::EntityRole::None;
}

}  // namespace

int occ_scan(const vfs::Vfs& fs, const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "usage: occ scan <outdir> [--save <save.dat> <card> <slot>] [--room <stem>]\n");
    return 2;
  }
  const std::filesystem::path out_dir = args[1];
  std::string only_room, save_path;
  int save_card = 1, save_slot = 1;
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--room" && i + 1 < args.size()) only_room = args[++i];
    else if (args[i] == "--save" && i + 3 < args.size()) {
      save_path = args[++i];
      save_card = std::atoi(args[++i].c_str());
      save_slot = std::atoi(args[++i].c_str());
    }
  }
  data::GameTables tables;
  std::string err;
  if (!tables.load(fs, &err)) {
    std::fprintf(stderr, "scan: tables: %s\n", err.c_str());
    return 1;
  }
  std::optional<save::SaveSlot> slot;
  if (!save_path.empty()) {
    const save::SaveDat dat = save::SaveDat::load(save_path);
    const save::MemCard* card = dat.card(save_card);
    if (card)
      for (const auto& ref : save::list_slots(*card))
        if (ref.slot == save_slot) slot = save::read_slot(*card, ref.block);
    if (!slot) {
      std::fprintf(stderr, "scan: %s has no slot %d on card %d\n", save_path.c_str(), save_slot, save_card);
      return 1;
    }
  }

  const game::ScriptScan scan = game::scan_rooms(fs);
  std::filesystem::create_directories(out_dir);
  std::ofstream events(out_dir / "events.csv", std::ios::binary), treasure(out_dir / "treasure.csv", std::ios::binary),
      battles(out_dir / "battles.csv", std::ios::binary), enemies(out_dir / "enemies.csv", std::ios::binary),
      summary(out_dir / "summary.txt", std::ios::binary);
  events << "room,room_id,area,entity,role,slot,offset,kind,op,values,names,operand_var,flag,flag_guarded,flag_at_load,capacity_check,"
            "story_lo,story_hi,conds\n";
  treasure << "key,flag,chest,gives,sites,areas,story_lo,story_hi,conds" << (slot ? ",opened" : "") << "\n";
  battles << "room,area,entity,role,slot,offset,op,battle_ids,scenes,enemies,story_lo,story_hi,conds\n";

  std::map<script::EventKind, int> kind_count;
  int unresolved = 0, multi_valued = 0, gives_with_flag = 0, gives_without_flag = 0, failed_rooms = 0;
  struct EnemySeen {
    std::set<std::string> rooms;
    int story_lo = 0x7FFF;
    bool random = false, scripted = false;
  };
  std::map<int, EnemySeen> seen;
  for (const game::RoomScan& room : scan.rooms) {
    if (!room.error.empty()) {
      ++failed_rooms;
      continue;
    }
    const std::string area = area_of(room.stem);
    for (const script::ScanEvent& event : room.scan.events) {
      ++kind_count[event.kind];
      if (event.values.empty() && event.kind != script::EventKind::SetFlag) ++unresolved;
      if (event.values.size() > 1) ++multi_valued;
      const std::string role = role_name(role_of(room, event.entity));
      const std::string conds = join_conds(event.conds);
      const std::string story = story_text(event.conds);
      events << std::format("{},{},{},{},{},{},{:04X},{},{:X},{},{},{},{},{},{},{},{},{}\n", room.stem, room.room_id, csv(area), event.entity,
                            role, event.slot, event.offset, script::event_kind_name(event.kind), event.op, join_values(tables, event, false),
                            csv(join_values(tables, event, true)), event.operand_var < 0 ? std::string() : std::format("{:04X}", event.operand_var),
                            script::flag_text(event.flag), event.flag_guarded ? 1 : 0, event.flag_at_load ? 1 : 0, event.capacity_check ? 1 : 0,
                            story, csv(conds));
      if (!only_room.empty() && room.stem == only_room)
        std::printf("%-3d %-7s %-2d %04X %-8s %-40s flag %-10s %s\n", event.entity, role.c_str(), event.slot, event.offset,
                    script::event_kind_name(event.kind), join_values(tables, event, true).c_str(), script::flag_text(event.flag).c_str(),
                    conds.c_str());
      if (event.kind == script::EventKind::Give) ++(event.flag.valid() ? gives_with_flag : gives_without_flag);
      if (event.kind != script::EventKind::Battle) continue;
      std::string scene_list, enemy_list;
      std::set<int> enemy_ids;
      for (int battle_id : event.values)
        for (int scene : game::battle_scenes(tables, battle_id)) {
          scene_list += (scene_list.empty() ? "" : "|") + std::to_string(scene);
          const data::SceneRec& rec = tables.scenes[static_cast<size_t>(scene)];
          for (int k = 0; k < 3; ++k)
            if (rec.enemy_count(k) && rec.enemy_id(k) != 0xFF) enemy_ids.insert(rec.enemy_id(k));
        }
      const script::StoryRange range = script::story_range(event.conds);
      for (int enemy : enemy_ids) {
        enemy_list += (enemy_list.empty() ? "" : "|") + std::format("{}:{}", enemy, tables.enemy_name(enemy));
        EnemySeen& entry = seen[enemy];
        entry.rooms.insert(room.stem);
        entry.story_lo = std::min(entry.story_lo, range.lo);
        (event.op == 0xC9 ? entry.scripted : entry.random) = true;
      }
      battles << std::format("{},{},{},{},{},{:04X},{:X},{},{},{},{},{}\n", room.stem, csv(area), event.entity, role, event.slot, event.offset, event.op,
                             join_values(tables, event, false), scene_list, csv(enemy_list), story, csv(conds));
    }
  }
  // Other sources of every id a script gives: shops (element shop and smith rows), the trading post, and the kill drops and steals
  // of the enemies some battle op fields.
  struct Sources {
    int flagged = 0, unflagged = 0, shops = 0, trade = 0, drops = 0, steals = 0;
  };
  std::map<int, Sources> sources;
  for (const game::RoomScan& room : scan.rooms)
    for (const script::ScanEvent& event : room.scan.events)
      if (event.kind == script::EventKind::Give)
        for (int value : event.values) ++(event.flag.valid() ? sources[value].flagged : sources[value].unflagged);
  for (const auto& row : tables.el_shop)
    for (u16 element : row)
      if (sources.contains(0x200 + element)) ++sources[0x200 + element].shops;
  ui::shop::ShopData shops = ui::shop::ShopData::from_tables(tables);
  shops.load_waa_shop(fs);
  for (const auto& row : shops.waa_shop)
    for (u16 item : row)
      if (sources.contains(item)) ++sources[item].shops;
  for (const auto& row : tables.trade)
    for (const auto& entry : row)
      if (const int goods = entry.goods; sources.contains(goods)) ++sources[goods].trade;
  for (const auto& [enemy, entry] : seen) {
    if (static_cast<size_t>(enemy) >= tables.enemies.size()) continue;
    const size_t row = tables.enemies[static_cast<size_t>(enemy)].drop_row();
    if (row >= tables.drops.size()) continue;
    for (int k = 0; k < 4; ++k) {
      const u16 loot = tables.drops[row].slot[static_cast<size_t>(k)];
      const int id = loot & 0x8000 ? 0x200 + (loot & 0x7FFF) : loot;
      if (loot && sources.contains(id)) ++(k < 2 ? sources[id].drops : sources[id].steals);
    }
  }
  std::ofstream source_file(out_dir / "sources.csv", std::ios::binary);
  source_file << "id,name,one_off_gives,other_gives,shop_rows,trade_rows,drop_enemies,steal_enemies\n";
  for (const auto& [id, entry] : sources)
    source_file << std::format("{},{},{},{},{},{},{},{}\n", id, csv(give_name(tables, id)), entry.flagged, entry.unflagged, entry.shops, entry.trade,
                               entry.drops, entry.steals);

  std::ofstream entities(out_dir / "entities.csv", std::ios::binary);
  entities << "room,entity,role,inactive_when\n";
  for (const game::RoomScan& room : scan.rooms)
    for (size_t entity = 0; entity < room.scan.entities.size(); ++entity) {
      std::string when;
      for (const auto& conds : room.scan.entities[entity].inactive_when) when += (when.empty() ? "" : " || ") + ("(" + join_conds(conds) + ")");
      entities << std::format("{},{},{},{}\n", room.stem, entity, role_name(room.scan.entities[entity].role), csv(when));
    }
  enemies << "enemy,name,battle_kinds,story_lo,rooms\n";
  for (const auto& [enemy, entry] : seen) {
    std::string rooms;
    for (const auto& stem : entry.rooms) rooms += (rooms.empty() ? "" : " ") + stem;
    enemies << std::format("{},{},{},{},{}\n", enemy, csv(tables.enemy_name(enemy)),
                           entry.random && entry.scripted ? "both" : entry.random ? "symtbl" : "scene", entry.story_lo, rooms);
  }

  const data::BestiaryOrder order = game::bestiary_order(scan, tables, fs);
  const auto story_areas = data::story_areas();
  std::ofstream bestiary(out_dir / "bestiary.csv", std::ios::binary);
  bestiary << "number,name,records,places\n";
  for (size_t index = 0; index < order.species.size(); ++index) {
    std::string records, places;
    for (int record : order.species[index]) {
      records += (records.empty() ? "" : " ") + std::to_string(record);
      for (int area : order.places[static_cast<size_t>(record)]) {
        const std::string key = story_areas[static_cast<size_t>(area)].key;
        if (places.find(key) == std::string::npos) places += (places.empty() ? "" : " ") + key;
      }
    }
    bestiary << std::format("{},{},{},{}\n", index + 1, csv(tables.enemy_name(order.species[index].front())), records, places);
  }
  // The story values each area's own scripts set: the anchors the table's order is checked against.
  std::map<int, std::set<int>> sets_in;
  for (const game::RoomScan& room : scan.rooms)
    for (const script::ScanEvent& event : room.scan.events)
      if (event.kind == script::EventKind::Scenario)
        for (int value : event.values) sets_in[data::story_area_of(room.stem)].insert(value);
  import::LangTables texts;
  texts.init(&fs);
  std::ofstream areas(out_dir / "areas.csv", std::ios::binary);
  areas << "row,key,story,sets,world,en,fr,it,de,es,jp\n";
  for (size_t area = 0; area < story_areas.size(); ++area) {
    std::string sets;
    for (int value : sets_in[static_cast<int>(area)]) sets += (sets.empty() ? "" : " ") + std::to_string(value);
    areas << std::format("{},{},{},{},{}", area, story_areas[area].key, story_areas[area].story, sets, world_tag(order.worlds[area]));
    for (int lang = 0; lang < 6; ++lang) areas << "," << csv(data::place_name(texts, static_cast<int>(area), static_cast<import::Lang>(lang)));
    areas << "\n";
  }

  int opened = 0, chests = 0;
  std::map<script::GiveClass, int> treasure_class;
  const std::vector<game::Treasure> all_treasures = game::treasures(scan);
  for (const game::Treasure& entry : all_treasures) {
    chests += entry.chest ? 1 : 0;
    ++treasure_class[script::give_class(entry.gives.front())];
    std::string gives, sites, areas, conds;
    script::StoryRange range{0x7FFF, 0};
    for (int value : entry.gives) gives += (gives.empty() ? "" : "|") + give_name(tables, value);
    for (const auto& [room_index, event_index] : entry.sites) {
      const game::RoomScan& room = scan.rooms[room_index];
      const script::ScanEvent& event = room.scan.events[event_index];
      sites += (sites.empty() ? "" : " ") + game::event_id(room.stem, event);
      const std::string area = area_of(room.stem);
      if (!area.empty() && areas.find(area) == std::string::npos) areas += (areas.empty() ? "" : "|") + area;
      const script::StoryRange site_range = script::story_range(event.conds);
      range.lo = std::min(range.lo, site_range.lo);
      range.hi = std::max(range.hi, site_range.hi);
      if (conds.empty()) conds = join_conds(event.conds);
    }
    std::string state;
    if (slot) {
      const bool is_open = entry.flag.is_set([&](int var) { return static_cast<int>(slot->var(var)); });
      opened += is_open ? 1 : 0;
      state = is_open ? ",1" : ",0";
    }
    treasure << std::format("{},{},{},{},{},{},{},{},{}{}\n", entry.key, script::flag_text(entry.flag), entry.chest ? 1 : 0, csv(gives), sites, csv(areas),
                            range.lo, range.hi == 0x7FFF ? std::string() : std::to_string(range.hi), csv(conds), state);
  }

  const ui::RecordsInfo info = game::records_info(scan, tables);
  std::ofstream records(out_dir / "records.txt", std::ios::binary);
  for (const ui::StoryDrop& drop : info.story_drops) records << std::format("story drop to {} from {}\n", drop.to, drop.from);
  for (int character = 0; character < ui::RecordsInfo::kCharacters; ++character) {
    const ui::CharacterInfo& who = info.characters[static_cast<size_t>(character)];
    records << std::format("char {:2} {:<8} last {:<5}{}{}", character, tables.char_name(character),
                           who.last_story >= ui::kStoryOpen ? std::string("open") : std::to_string(who.last_story),
                           slot && ui::character_missed(info, *slot, character) ? " MISSED" : "",
                           who.last_story < ui::kStoryOpen && ui::story_reopens(info, who.last_story) ? " REOPENS" : "");
    for (const ui::RecruitSite& site : who.sites)
      records << std::format(" | {} {}..{} how {:X}", site.area.empty() ? "-" : site.area, site.story_lo,
                             site.story_hi >= ui::kStoryOpen ? std::string() : std::to_string(site.story_hi), site.how);
    records << "\n";
  }
  auto one_off_only = [&](const char* label, const std::map<int, ui::GiveInfo>& gives, int base) {
    for (const auto& [id, give] : gives) {
      if (give.repeatable || give.shop || give.enemy || give.one_offs.empty()) continue;
      int last = 0;
      for (const ui::GiveInfo::OneOff& one_off : give.one_offs) last = std::max(last, one_off.story_hi);
      if (last >= ui::kStoryOpen) continue;
      records << std::format("{} {} {} last {}{}{}\n", label, id, give_name(tables, base + id), last,
                             slot && ui::give_missed(info, give, *slot) ? " MISSED" : "", ui::story_reopens(info, last) ? " REOPENS" : "");
    }
  };
  one_off_only("element", info.elements, 0x200);
  one_off_only("key", info.key_items, 0x400);
  const std::vector<ui::TreasureTally> tally = slot ? ui::tally_treasures(info, *slot) : std::vector<ui::TreasureTally>(info.areas.size());
  for (size_t area = 0; area < info.areas.size(); ++area) {
    int total = 0;
    for (const ui::TreasureInfo& entry : info.treasures) total += entry.area == static_cast<int>(area) ? 1 : 0;
    const ui::TreasureArea& at = info.areas[area];
    const std::string world = at.world == data::World::None ? std::string() : " (" + world_tag(at.world) + ")";
    records << std::format("area {}{} treasures {} opened {}\n", at.name.empty() ? "-" : at.name, world, total, tally[area].opened);
  }

  u32 steps = 0;
  for (const auto& room : scan.rooms) steps += room.scan.steps;
  std::string text = std::format("rooms {} (failed {}), walk steps {}, scan {:.1f} ms (reading and decoding the rooms {:.1f} ms)\n",
                                 scan.rooms.size(), failed_rooms, steps, scan.milliseconds, scan.read_milliseconds);
  std::vector<const game::RoomScan*> slowest;
  for (const auto& room : scan.rooms) slowest.push_back(&room);
  std::sort(slowest.begin(), slowest.end(), [](const auto* lhs, const auto* rhs) { return lhs->milliseconds > rhs->milliseconds; });
  text += "slowest rooms:";
  for (size_t i = 0; i < slowest.size() && i < 5; ++i)
    text += std::format(" {} {:.0f} ms ({} steps)", slowest[i]->stem, slowest[i]->milliseconds, slowest[i]->scan.steps);
  text += "\n";
  for (const auto& [kind, count] : kind_count) text += std::format("  {:<9} {}\n", script::event_kind_name(kind), count);
  text += std::format("events with no resolved operand {}, with several candidate values {}\n", unresolved, multi_valued);
  text += std::format("gives with a flag {}, without {}\n", gives_with_flag, gives_without_flag);
  text += std::format("treasures {} (chest routine {}): items {}, elements {}, key items {}\n", all_treasures.size(), chests,
                      treasure_class[script::GiveClass::Item], treasure_class[script::GiveClass::Element], treasure_class[script::GiveClass::KeyItem]);
  if (slot) text += std::format("treasures opened in {} card {} slot {}: {}\n", save_path, save_card, save_slot, opened);
  text += std::format("enemy records fielded by some battle op: {}\n", seen.size());
  summary << text;
  std::fwrite(text.data(), 1, text.size(), stdout);
  return failed_rooms ? 1 : 0;
}
