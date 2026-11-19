#include "game/records_info.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

#include "data/battlefields.h"
#include "data/bestiary.h"
#include "data/tables.h"
#include "game/game_flow.h"
#include "import/text.h"

namespace oc::game {

namespace {

using script::Cond;
using script::Subject;

std::string area_of(const std::string& stem) {
  const char* name = data::area_name(stem);
  return name ? name : "";
}

void add_area(std::vector<std::string>& areas, const std::string& area) {
  if (!area.empty() && std::find(areas.begin(), areas.end(), area) == areas.end()) areas.push_back(area);
}

ui::RecordFlag record_flag(const script::ScanFlag& flag) { return {flag.var, flag.bit, flag.value}; }

// A recruitment the path allows only for a character carried over from a cleared game.
bool needs_carryover(const std::vector<Cond>& conds) {
  return std::any_of(conds.begin(), conds.end(), [](const Cond& cond) { return cond.subject == Subject::Carryover && cond.value != 0; });
}

ui::RecruitSite recruit_site(const std::string& stem, const script::ScanEvent& event, int character, const script::StoryRange& range) {
  ui::RecruitSite site;
  site.area = area_of(stem);
  site.story_lo = range.lo;
  site.story_hi = range.hi >= ui::kStoryOpen ? ui::kStoryOpen : range.hi;
  for (const Cond& cond : event.conds) {
    if (cond.subject == Subject::Choice) site.how |= ui::kHowChoice;
    if (cond.subject == Subject::Item && cond.value != 0 && script::give_class(cond.id) == script::GiveClass::KeyItem)
      site.how |= ui::kHowItem, site.key_item = cond.id - 0x400;
    if (cond.subject == Subject::InParty && cond.value != 0 && cond.id != character) site.how |= ui::kHowParty, site.partner = cond.id;
    if (cond.subject == Subject::Recruited && cond.value == 0 && cond.id != character) site.how |= ui::kHowAlone;
  }
  return site;
}

}  // namespace

std::vector<bool> reachable_rooms(const ScriptScan& scan) {
  std::map<std::string, size_t> by_stem;
  for (size_t i = 0; i < scan.rooms.size(); ++i) by_stem[scan.rooms[i].stem] = i;
  std::vector<bool> reached(scan.rooms.size(), false);
  const auto start = by_stem.find(kNewGameRoom);
  if (start == by_stem.end()) return reached;
  std::vector<size_t> pending = {start->second};
  reached[start->second] = true;
  while (!pending.empty()) {
    const size_t room = pending.back();
    pending.pop_back();
    for (const script::ScanEvent& event : scan.rooms[room].scan.events) {
      if (event.kind != script::EventKind::Exit) continue;
      for (const int target : event.values) {
        const char* stem = import::room_name_for_id(target);
        if (!stem || std::string_view(stem) == kTitleRoom) continue;
        const auto found = by_stem.find(stem);
        if (found == by_stem.end() || reached[found->second]) continue;
        reached[found->second] = true;
        pending.push_back(found->second);
      }
    }
  }
  return reached;
}

namespace {

constexpr int kStoryVar = 0x00;
constexpr int kEntranceVar = 0x02;
constexpr int kRoomVarBase = 0x400;
// Global variables the engine writes besides the scripts: the room parameter, party, results and the New Game+ state below 0x80; the
// registered positions, Kid's saved grid and the new-game table from 0x300.
constexpr int kEngineVarsBelow = 0x80;
constexpr int kEngineVarsFrom = 0x300;
// An entrance the room's own scripts may have rewritten, or an exit's entrance that is not a literal.
constexpr int kAnyEntrance = -1;

bool cond_holds(int lhs, script::Cmp cmp, int rhs) {
  switch (cmp) {
    case script::Cmp::Eq: return lhs == rhs;
    case script::Cmp::Ne: return lhs != rhs;
    case script::Cmp::Gt: return lhs > rhs;
    case script::Cmp::Lt: return lhs < rhs;
    case script::Cmp::Ge: return lhs >= rhs;
    case script::Cmp::Le: return lhs <= rhs;
    case script::Cmp::AnyBit: return (lhs & rhs) != 0;
    case script::Cmp::NoBit: return (lhs & rhs) == 0;
  }
  return true;
}

// The (room, entrance, story on entry, story now) states a game can reach, over the scan; see story_drops.
class StorySearch {
 public:
  explicit StorySearch(const ScriptScan& scan) : scan_(scan), constant_(kRoomVarBase / 2, true) {
    for (size_t i = 0; i < scan.rooms.size(); ++i) by_stem_[scan.rooms[i].stem] = i;
    for (int var = 0; var < kRoomVarBase; var += 2) constant_[static_cast<size_t>(var / 2)] = var >= kEngineVarsBelow && var < kEngineVarsFrom;
    for (const RoomScan& room : scan.rooms) {
      if (room.scan.writes_any) std::fill(constant_.begin(), constant_.end(), false);
      for (const int var : room.scan.written) constant_[static_cast<size_t>(var / 2)] = false;
    }
  }

  std::vector<ui::StoryDrop> run() {
    std::set<ui::StoryDrop> drops;
    const auto start = by_stem_.find(kNewGameRoom);
    if (start == by_stem_.end()) return {};
    push({start->second, 0, 0, 0});
    while (!todo_.empty()) {
      const State state = todo_.back();
      todo_.pop_back();
      const RoomScan& room = scan_.rooms[state.room];
      const int entrance = writes_entrance(room) ? kAnyEntrance : state.entrance;
      for (const script::ScanEvent& event : room.scan.events) {
        if (event.kind != script::EventKind::Exit && event.kind != script::EventKind::Scenario) continue;
        if (!may_run(room, event, entrance, state.entry, state.story)) continue;
        if (event.kind == script::EventKind::Scenario) {
          if (event.values.empty()) {
            drops.insert({0, state.story});
            continue;
          }
          for (const int value : event.values) {
            if (value < state.story) drops.insert({value, state.story});
            push({state.room, state.entrance, state.entry, value});
          }
          continue;
        }
        for (const int target : event.values) {
          const char* stem = import::room_name_for_id(target);
          if (!stem || std::string_view(stem) == kTitleRoom) continue;
          if (const auto found = by_stem_.find(stem); found != by_stem_.end())
            push({found->second, event.entrance, state.story, state.story});
        }
      }
    }
    return {drops.begin(), drops.end()};
  }

 private:
  struct State {
    size_t room = 0;
    int entrance = 0;
    int entry = 0;
    int story = 0;
    auto operator<=>(const State&) const = default;
  };

  static bool writes_entrance(const RoomScan& room) {
    return room.scan.writes_any || std::binary_search(room.scan.written.begin(), room.scan.written.end(), kEntranceVar);
  }

  void push(const State& state) {
    if (seen_.insert(state).second) todo_.push_back(state);
  }

  // A test's outcome when the state decides it.
  std::optional<bool> decided(const Cond& cond, int entrance, int story) const {
    if (cond.subject != Subject::Var || cond.value_var) return std::nullopt;
    if (cond.id == kStoryVar) return cond_holds(story, cond.cmp, cond.value);
    if (cond.id == kEntranceVar) return entrance == kAnyEntrance ? std::nullopt : std::optional<bool>(cond_holds(entrance, cond.cmp, cond.value));
    if (cond.id >= 0 && cond.id < kRoomVarBase && constant_[static_cast<size_t>(cond.id / 2)]) return cond_holds(0, cond.cmp, cond.value);
    return std::nullopt;
  }

  bool inactive(const RoomScan& room, int entity, int entrance, int entry) const {
    if (entity < 0 || static_cast<size_t>(entity) >= room.scan.entities.size()) return false;
    const script::EntityScan& scan = room.scan.entities[static_cast<size_t>(entity)];
    if (scan.role == script::EntityRole::Party || scan.reactivated) return false;
    return std::any_of(scan.inactive_at.begin(), scan.inactive_at.end(), [&](const script::InactiveRegion& region) {
      const bool story_in = entry >= region.story_lo && entry <= region.story_hi;
      const bool entrance_in = entrance == kAnyEntrance ? region.entrance_lo == 0 && region.entrance_hi >= 0x7FFF
                                                        : entrance >= region.entrance_lo && entrance <= region.entrance_hi;
      return story_in && entrance_in;
    });
  }

  bool may_run(const RoomScan& room, const script::ScanEvent& event, int entrance, int entry, int story) const {
    if (event.slot != 0 && inactive(room, event.entity, entrance, entry)) return false;
    return std::none_of(event.conds.begin(), event.conds.end(), [&](const Cond& cond) { return decided(cond, entrance, story) == false; });
  }

  const ScriptScan& scan_;
  std::map<std::string, size_t> by_stem_;
  // Per global variable slot: no script and not the engine writes it, so it holds 0.
  std::vector<bool> constant_;
  std::set<State> seen_;
  std::vector<State> todo_;
};

}  // namespace

std::vector<ui::StoryDrop> story_drops(const ScriptScan& scan) { return StorySearch(scan).run(); }

ui::RecordsInfo records_info(const ScriptScan& scan, const data::GameTables& tables) {
  ui::RecordsInfo info;
  info.story_drops = story_drops(scan);
  const std::vector<bool> reached = reachable_rooms(scan);
  for (size_t room = 0; room < scan.rooms.size(); ++room) {
    if (!reached[room]) continue;
    const RoomScan& entry = scan.rooms[room];
    for (const script::ScanEvent& event : entry.scan.events) {
      const script::StoryRange range = script::story_range(event.conds);
      if (range.lo > range.hi) continue;
      if (event.kind == script::EventKind::Join || event.kind == script::EventKind::Roster) {
        if (needs_carryover(event.conds)) continue;
        for (const int character : event.values) {
          if (character < 0 || character >= ui::RecordsInfo::kCharacters) continue;
          ui::CharacterInfo& who = info.characters[static_cast<size_t>(character)];
          const ui::RecruitSite site = recruit_site(entry.stem, event, character, range);
          const auto same = [&](const ui::RecruitSite& other) {
            return std::tie(other.area, other.story_lo, other.story_hi, other.how, other.key_item, other.partner) ==
                   std::tie(site.area, site.story_lo, site.story_hi, site.how, site.key_item, site.partner);
          };
          if (std::none_of(who.sites.begin(), who.sites.end(), same)) who.sites.push_back(site);
        }
      }
      if (event.kind != script::EventKind::Give) continue;
      for (const int value : event.values) {
        const script::GiveClass kind = script::give_class(value);
        if (kind != script::GiveClass::Element && kind != script::GiveClass::KeyItem) continue;
        ui::GiveInfo& give = kind == script::GiveClass::Element ? info.elements[value - 0x200] : info.key_items[value - 0x400];
        add_area(give.areas, area_of(entry.stem));
        if (event.flag.valid())
          give.one_offs.push_back({record_flag(event.flag), range.hi >= ui::kStoryOpen ? ui::kStoryOpen : range.hi});
        else
          give.repeatable = true;
      }
    }
  }
  for (ui::CharacterInfo& who : info.characters) {
    who.last_story = who.sites.empty() ? ui::kStoryOpen : 0;
    for (const ui::RecruitSite& site : who.sites) who.last_story = std::max(who.last_story, site.story_hi);
  }
  for (const std::vector<u16>& row : tables.el_shop)
    for (const u16 element : row)
      if (element) info.elements[element].shop = true;
  // The drop rows of every enemy some encounter scene places: bit 15 marks an element.
  std::set<size_t> rows;
  for (const data::SceneRec& scene : tables.scenes)
    for (int group = 0; group < 3; ++group)
      if (scene.enemy_count(group) > 0 && scene.enemy_id(group) < tables.enemies.size())
        rows.insert(static_cast<size_t>(tables.enemies[scene.enemy_id(group)].drop_row() & 0x3FFF));
  for (const size_t row : rows) {
    if (row >= tables.drops.size()) continue;
    for (const u16 loot : tables.drops[row].slot)
      if (loot & 0x8000) info.elements[loot & 0x7FFF].enemy = true;
  }

  // Treasures: the reachable sites of each; the areas by place name and world (a world only for a name both worlds carry), the rooms
  // without a place name last.
  const std::set<std::string> both = data::places_in_both_worlds(room_stems(scan));
  std::vector<std::pair<ui::TreasureArea, ui::TreasureInfo>> found;
  for (const Treasure& treasure : treasures(scan)) {
    const auto site = std::find_if(treasure.sites.begin(), treasure.sites.end(), [&](const auto& at) { return reached[at.first]; });
    if (site == treasure.sites.end()) continue;
    ui::TreasureInfo entry;
    entry.flag = record_flag(treasure.flag);
    entry.chest = treasure.chest;
    entry.gives = treasure.gives;
    const std::string& stem = scan.rooms[site->first].stem;
    ui::TreasureArea area{area_of(stem)};
    if (both.contains(area.name)) area.world = data::world_of(stem);
    found.push_back({std::move(area), std::move(entry)});
  }
  std::set<ui::TreasureArea> names;
  bool unnamed = false;
  for (const auto& [area, entry] : found) {
    if (area.name.empty()) unnamed = true;
    else names.insert(area);
  }
  info.areas.assign(names.begin(), names.end());
  if (unnamed) info.areas.emplace_back();
  for (auto& [area, entry] : found) {
    entry.area = static_cast<int>(std::find(info.areas.begin(), info.areas.end(), area) - info.areas.begin());
    info.treasures.push_back(std::move(entry));
  }
  return info;
}

}  // namespace oc::game
