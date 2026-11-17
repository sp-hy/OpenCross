// game/oc_achievements.h: OpenCross's own achievement set: every rule kind, the battle events, counters, profile sets across saves,
// sets, the gates, the profile file round trip, and the ending state machine shared with the Steam evaluator.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "game/achievements.h"
#include "game/oc_achievements.h"
#include "game/oc_achievements_ui.h"

using namespace oc;
using game::OcAchievements;
using game::OcCategory;
using game::OcRow;
using game::OcRule;
using game::OcSource;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

constexpr int kSampleTicks = 60;
constexpr int kNgPlusVar = 0x7E;

// Rows of kinds and shapes the built-in table does not hold.
constexpr OcRow kTestRows[] = {
    {.id = "t_room", .title = "Room", .description = "", .category = OcCategory::Story, .rule = OcRule::Room, .ids = {10, 11}},
    {.id = "t_item", .title = "Item", .description = "", .category = OcCategory::Items, .rule = OcRule::Item, .ids = {0x20}},
    {.id = "t_bit", .title = "Bit", .description = "", .category = OcCategory::Story, .rule = OcRule::VarBits, .var = 0x150, .ids = {4, 6}},
    {.id = "t_a", .title = "A", .description = "", .category = OcCategory::Story, .rule = OcRule::Room, .ids = {20}},
    {.id = "t_b", .title = "B", .description = "", .category = OcCategory::Story, .rule = OcRule::Room, .ids = {21}},
    {.id = "t_ab", .title = "AB", .description = "", .category = OcCategory::Story, .rule = OcRule::Set, .members = "t_a t_b missing_row"},
    {.id = "t_all", .title = "All", .description = "", .category = OcCategory::Story, .rule = OcRule::Set, .members = "t_a* t_b"},
};

size_t row_of(const OcAchievements& achievements, const char* id) {
  for (size_t i = 0; i < achievements.rows().size(); ++i)
    if (std::strcmp(achievements.rows()[i].id, id) == 0) return i;
  std::printf("no row %s\n", id);
  ++failures;
  return 0;
}

bool has(const OcAchievements& achievements, const char* id) { return achievements.unlocked(row_of(achievements, id)); }

OcAchievements make(std::span<const OcRow> rows = OcAchievements::table()) {
  OcAchievements achievements(rows);
  achievements.set_enabled(true);
  achievements.set_clock([] { return std::string("2026-11-16T14:02Z"); });
  return achievements;
}

// One room sample (60 ticks outside battle).
void sample(OcAchievements& achievements, OcAchievements::Sample now) {
  const int due = achievements.tick(kSampleTicks, false);
  for (int i = 0; i < due; ++i) achievements.sample(now);
}

OcAchievements::Sample at_room(int prev, int room) {
  OcAchievements::Sample now;
  now.prev_room = prev;
  now.room = room;
  return now;
}

battle::ActionEvent cast(int actor, int element) {
  battle::ActionEvent event;
  event.kind = battle::ActionEvent::Kind::Element;
  event.actor = actor;
  event.target = actor < battle::kPcSlots ? 3 : 0;
  event.element = element;
  return event;
}

void present(OcAchievements& achievements, std::vector<std::vector<battle::ActionEvent>> actions) { achievements.battle_actions(actions); }

// The same profile in a fresh evaluator, as a later session (or another save on the same profile) would read it.
OcAchievements reload(const OcAchievements& achievements, std::span<const OcRow> rows = OcAchievements::table()) {
  OcAchievements next = make(rows);
  next.load(*OcAchievements::parse_profile(OcAchievements::serialize_profile(achievements.profile())));
  return next;
}
}  // namespace

int main() {
  {  // The built-in table: 173 rows with distinct ids, spoilers where the tab hides them.
    const std::span<const OcRow> rows = OcAchievements::table();
    CHECK(rows.size() == 173);
    std::set<std::string> ids;
    for (const OcRow& row : rows) ids.insert(row.id);
    CHECK(ids.size() == rows.size());
    for (const char* dropped : {"oc_item_mastermune", "oc_tech_wind_slash", "oc_tech_max_defense", "oc_boss_devourer"}) CHECK(!ids.count(dropped));
    OcAchievements achievements = make();
    int spoilers = 0;
    for (const OcRow& row : rows) spoilers += row.spoiler ? 1 : 0;
    CHECK(spoilers == 25);
    for (const char* spoiler : {"oc_story_swap", "oc_story_tower", "oc_boss_dragons", "oc_boss_dragoon", "oc_boss_vita", "oc_end_all"})
      CHECK(rows[row_of(achievements, spoiler)].spoiler);
    for (const char* plain : {"oc_story_fort", "oc_place_guldove", "oc_boss_sky_dragon", "oc_join_kid"}) CHECK(!rows[row_of(achievements, plain)].spoiler);
  }
  {  // room_pair: the previous room in the first list and the room in the second.
    OcAchievements achievements = make();
    sample(achievements, at_room(373, 236));
    sample(achievements, at_room(372, 235));
    CHECK(!has(achievements, "oc_story_vortex"));
    sample(achievements, at_room(374, 236));
    CHECK(has(achievements, "oc_story_vortex"));
  }
  {  // var_bits: a listed bit of the variable set.
    OcAchievements test = make(kTestRows);
    OcAchievements::Sample now;
    now.var = [](int id) { return id == 0x150 ? 0x20 : 0; };
    sample(test, now);
    CHECK(!has(test, "t_bit"));
    now.var = [](int id) { return id == 0x150 ? 0x40 : 0; };
    sample(test, now);
    CHECK(has(test, "t_bit"));
  }
  {  // Room samples: one every 60 ticks outside battle, none in battle.
    OcAchievements achievements = make();
    CHECK(achievements.tick(59, false) == 0);
    CHECK(achievements.tick(1, false) == 1);
    CHECK(achievements.tick(120, true) == 0);
    CHECK(achievements.tick(120, false) == 2);
    achievements.sample(at_room(0, 197));
    CHECK(has(achievements, "oc_story_manor"));
    CHECK(!has(achievements, "oc_story_fort"));
    const std::vector<size_t> unlocked = achievements.take_unlocked();
    CHECK(unlocked.size() == 1 && unlocked[0] == row_of(achievements, "oc_story_manor"));
    CHECK(achievements.unlock_time(row_of(achievements, "oc_story_manor")) == "2026-11-16T14:02Z");
    CHECK(achievements.take_dirty());
    CHECK(!achievements.take_dirty());
  }
  {  // Recruit, recruits, body and save rules.
    OcAchievements achievements = make();
    OcAchievements::Sample now;
    for (int i = 0; i < 10; ++i) now.recruited[static_cast<size_t>(i)] = true;
    now.party_bodies = {0x31, 5};
    now.gold = 100000;
    now.var = [](int id) { return id == kNgPlusVar ? 3 : 0; };
    sample(achievements, now);
    CHECK(has(achievements, "oc_join_kid"));
    CHECK(has(achievements, "oc_join_marcy"));
    CHECK(!has(achievements, "oc_join_korcha"));
    CHECK(has(achievements, "oc_join_10"));
    CHECK(!has(achievements, "oc_join_20"));
    CHECK(has(achievements, "oc_story_swap"));
    CHECK(has(achievements, "oc_item_gold"));
    CHECK(!has(achievements, "oc_item_gold_max"));
    CHECK(!has(achievements, "oc_end_ngplus"));
    now.var = [](int id) { return id == kNgPlusVar ? 2 : 0; };
    sample(achievements, now);
    CHECK(has(achievements, "oc_end_ngplus"));
  }
  {  // Fights: won with result 0 or 3, a boss row of several scenes, the battle counter.
    OcAchievements achievements = make();
    achievements.battle_ended(389, 1, true, 0, {-1, -1, -1});
    achievements.battle_ended(389, 2, true, 3, {-1, -1, -1});
    CHECK(!has(achievements, "oc_boss_komodo"));
    CHECK(!has(achievements, "oc_fight_first"));
    achievements.battle_ended(389, 0, true, 3, {-1, -1, -1});
    CHECK(has(achievements, "oc_boss_komodo"));
    CHECK(has(achievements, "oc_fight_first"));
    achievements.battle_ended(485, 3, true, 3, {-1, -1, -1});
    CHECK(has(achievements, "oc_boss_solt"));
    CHECK(!has(achievements, "oc_boss_ketchop"));
    const std::optional<OcAchievements::Progress> wins = achievements.progress(row_of(achievements, "oc_fight_100"));
    CHECK(wins && wins->have == 2 && wins->need == 100);
    CHECK(!achievements.progress(row_of(achievements, "oc_boss_hydra")));
  }
  {  // Casts: a party cast that runs; not a trapped one, not an enemy's.
    OcAchievements achievements = make();
    battle::ActionEvent trapped = cast(0, 37);
    trapped.caught = true;
    present(achievements, {{trapped}, {cast(3, 37)}});
    CHECK(!has(achievements, "oc_cast_frog_prince"));
    present(achievements, {{cast(1, 37)}});
    CHECK(has(achievements, "oc_cast_frog_prince"));
    present(achievements, {{cast(2, 159)}});
    CHECK(has(achievements, "oc_tech_x_strike"));
  }
  {  // Items (op BB) in any room.
    OcAchievements achievements = make();
    achievements.item_given(0x434);
    CHECK(!has(achievements, "oc_item_time_egg"));
    achievements.item_given(0x435);
    CHECK(has(achievements, "oc_item_time_egg"));
    achievements.item_given(0x415);
    achievements.item_given(0x418);
    CHECK(has(achievements, "oc_story_humour") && has(achievements, "oc_story_tear"));
    OcAchievements test = make(kTestRows);
    test.item_given(0x20);
    CHECK(has(test, "t_item"));
  }
  {  // Counters: steals that take an item, every forge, kept in the profile.
    OcAchievements achievements = make();
    battle::ActionEvent steal = cast(0, 0x9C);
    steal.steal_msg = 0;
    battle::ActionEvent element_stolen = steal;
    element_stolen.steal_msg = 1;
    battle::ActionEvent failed = steal;
    failed.steal_msg = 2;
    for (int i = 0; i < 9; ++i) present(achievements, {{steal}});
    present(achievements, {{element_stolen}, {failed}});
    CHECK(!has(achievements, "oc_fight_steal_10"));
    present(achievements, {{steal}});
    CHECK(has(achievements, "oc_fight_steal_10"));
    CHECK(achievements.profile().counters.at("steals") == 10);
    achievements.forged(1);
    CHECK(has(achievements, "oc_item_forge"));
    achievements.forged(8);
    CHECK(!has(achievements, "oc_item_forge_10"));
    achievements.forged(1);
    CHECK(has(achievements, "oc_item_forge_10"));
    const std::optional<OcAchievements::Progress> forges = achievements.progress(row_of(achievements, "oc_item_forge_50"));
    CHECK(forges && forges->have == 10 && forges->need == 50);
    OcAchievements later = reload(achievements);
    CHECK(later.profile().counters.at("forges") == 10);
    later.forged(40);
    later.tick(1, false);
    CHECK(has(later, "oc_item_forge_50"));
  }
  {  // Profile sets across saves: the members seen are kept in the profile, whatever save they were seen in.
    OcAchievements first = make();
    sample(first, at_room(0, 474));
    first.battle_ended(458, 0, true, 3, {-1, -1, -1});
    OcAchievements::Sample pip;
    pip.party_bodies = {44, 46};
    sample(first, pip);
    OcAchievements second = reload(first);
    sample(second, at_room(0, 475));
    second.battle_ended(459, 0, true, 3, {-1, -1, -1});
    CHECK(!has(second, "oc_place_fates"));
    const std::optional<OcAchievements::Progress> fates = second.progress(row_of(second, "oc_place_fates"));
    CHECK(fates && fates->have == 2 && fates->need == 3);
    OcAchievements third = reload(second);
    sample(third, at_room(0, 476));
    third.battle_ended(460, 3, true, 3, {-1, -1, -1});
    CHECK(has(third, "oc_place_fates"));
    CHECK(has(third, "oc_boss_vita"));
    CHECK(third.profile().seen.at("oc_join_pip_forms") == 0b101);
    pip.party_bodies = {45, 47, 48};
    sample(third, pip);
    CHECK(has(third, "oc_join_pip_forms"));
  }
  {  // A profile set over a variable's bits (the guide of each route to Viper Manor), across saves: each bit is a member.
    OcAchievements first = make();
    OcAchievements::Sample now;
    now.var = [](int id) { return id == 0x1C2 ? 0x1 : 0; };
    sample(first, now);
    CHECK(first.profile().seen.at("oc_story_three_routes") == 1);
    OcAchievements second = reload(first);
    now.var = [](int id) { return id == 0x1C2 ? 0x2 : 0; };
    sample(second, now);
    CHECK(!has(second, "oc_story_three_routes"));
    const std::optional<OcAchievements::Progress> routes = second.progress(row_of(second, "oc_story_three_routes"));
    CHECK(routes && routes->have == 2 && routes->need == 3);
    OcAchievements third = reload(second);
    now.var = [](int id) { return id == 0x1C2 ? 0x4 : 0; };
    sample(third, now);
    CHECK(has(third, "oc_story_three_routes"));
    CHECK(third.profile().seen.at("oc_story_three_routes") == 7);
  }
  {  // Sets: every listed row present; a name with no row is left out; a prefix takes every other row that starts with it.
    OcAchievements test = make(kTestRows);
    sample(test, at_room(0, 20));
    CHECK(!has(test, "t_ab"));
    CHECK(!has(test, "t_all"));
    sample(test, at_room(0, 21));
    CHECK(has(test, "t_ab"));
    CHECK(has(test, "t_all"));
    OcAchievements achievements = make();
    const int dragons[] = {426, 451, 452, 454, 455};
    for (int scene : dragons) achievements.battle_ended(scene, 0, true, 3, {-1, -1, -1});
    CHECK(!has(achievements, "oc_boss_dragons"));
    achievements.battle_ended(457, 0, true, 3, {-1, -1, -1});
    CHECK(has(achievements, "oc_boss_dragons"));
    for (const OcRow& row : achievements.rows())
      if (row.rule == OcRule::Room && std::string(row.id).starts_with("oc_place_")) sample(achievements, at_room(0, row.ids.values[0]));
    CHECK(!has(achievements, "oc_place_all"));
    sample(achievements, at_room(0, 474));
    sample(achievements, at_room(0, 475));
    sample(achievements, at_room(0, 476));
    CHECK(has(achievements, "oc_place_all"));
  }
  {  // big_hit: one hit of 9,999 or more by a party member on an enemy.
    OcAchievements achievements = make();
    battle::ActionEvent hit;
    hit.kind = battle::ActionEvent::Kind::Weapon;
    hit.actor = 0;
    hit.target = 4;
    hit.value = 9998;
    present(achievements, {{hit}});
    battle::ActionEvent on_party = hit;
    on_party.value = 9999;
    on_party.actor = 4;
    on_party.target = 0;
    present(achievements, {{on_party}});
    CHECK(!has(achievements, "oc_fight_big_hit"));
    hit.value = 9999;
    present(achievements, {{hit}});
    CHECK(has(achievements, "oc_fight_big_hit"));
  }
  {  // last_standing: a boss fight won with exactly one party member above 0 HP.
    OcAchievements achievements = make();
    achievements.battle_ended(1, 0, false, 1, {-1, -1, -1});
    achievements.battle_ended(415, 0, true, 2, {-1, -1, -1});
    achievements.battle_ended(415, 1, true, 1, {-1, -1, -1});
    CHECK(!has(achievements, "oc_fight_last_standing"));
    achievements.battle_ended(415, 0, true, 1, {-1, -1, -1});
    CHECK(has(achievements, "oc_fight_last_standing"));
  }
  {  // one_colour: the field colours a party cast leaves, read at the next action or at the end of the battle.
    OcAchievements achievements = make();
    battle::ActionEvent next = cast(4, 10);
    next.ring = {3, 3, 3};
    present(achievements, {{next}});
    CHECK(!has(achievements, "oc_cast_one_colour"));
    battle::ActionEvent mixed = next;
    mixed.ring = {3, 3, 2};
    present(achievements, {{cast(0, 10)}, {mixed}});
    CHECK(!has(achievements, "oc_cast_one_colour"));
    present(achievements, {{cast(0, 10)}, {next}});
    CHECK(has(achievements, "oc_cast_one_colour"));
    OcAchievements at_end = make();
    present(at_end, {{cast(0, 10)}});
    at_end.battle_ended(1, 0, false, 3, {5, 5, 5});
    CHECK(has(at_end, "oc_cast_one_colour"));
    OcAchievements new_battle = make();
    present(new_battle, {{cast(0, 10)}});
    new_battle.battle_started();
    present(new_battle, {{next}});
    CHECK(!has(new_battle, "oc_cast_one_colour"));
  }
  {  // trap_catch: an enemy's element caught by the party's trap; not a forced fizzle the party's trap did not hold.
    OcAchievements achievements = make();
    battle::ActionEvent caught = cast(4, 10);
    caught.caught = true;
    battle::ActionEvent own = cast(0, 10);
    own.caught = true;
    own.trap.enemy = 10;
    battle::ActionEvent forced = caught;
    forced.trap.pc = 11;
    present(achievements, {{own}, {cast(4, 10)}, {forced}});
    CHECK(!has(achievements, "oc_cast_trap"));
    caught.trap.pc = 10;
    present(achievements, {{caught}});
    CHECK(has(achievements, "oc_cast_trap"));
  }
  {  // level_eight, the summon colours and the double / triple techs read the element table.
    OcAchievements achievements = make();
    std::vector<OcAchievements::ElementInfo> elements(200);
    elements[126].level = 8;
    elements[30].level = 8;
    for (int summon = 37; summon <= 48; ++summon) elements[static_cast<size_t>(summon)] = {8, (summon - 37) / 2, 0};
    elements[150].casters = 2;
    elements[151].casters = 3;
    elements[152].casters = 1;
    achievements.set_elements(elements);
    present(achievements, {{cast(0, 126)}, {cast(3, 30)}});
    CHECK(!has(achievements, "oc_cast_level_eight"));
    present(achievements, {{cast(0, 30)}});
    CHECK(has(achievements, "oc_cast_level_eight"));
    for (int summon = 37; summon <= 47; summon += 2) {
      CHECK(!has(achievements, "oc_cast_spectrum"));
      present(achievements, {{cast(0, summon)}});
    }
    CHECK(has(achievements, "oc_cast_spectrum"));
    CHECK(!has(achievements, "oc_cast_summoner"));
    for (int summon = 38; summon <= 48; summon += 2) present(achievements, {{cast(1, summon)}});
    CHECK(has(achievements, "oc_cast_summoner"));
    present(achievements, {{cast(0, 150)}, {cast(0, 152)}});
    const std::optional<OcAchievements::Progress> techs = achievements.progress(row_of(achievements, "oc_tech_all"));
    CHECK(techs && techs->have == 1 && techs->need == 2);
    present(achievements, {{cast(2, 151)}});
    CHECK(has(achievements, "oc_tech_all"));
  }
  {  // Endings: the Steam evaluator's state machine, profile set of every ending, New Game+ finished while variable 0x7E is 2.
    const std::vector<std::vector<std::pair<int, int>>> routes = {
        {{0x20, 0x38}, {0x38, 0x1CE}, {0x1CE, 0x1D0}, {0x1D0, 9}},
        {{0x20, 0x39}, {0x39, 0x1ED}, {0x1ED, 0x1F2}, {0x1F2, 9}},
        {{0x20, 0x39}, {0x39, 0x216}, {0x216, 0x39}, {0x39, 9}, {9, 9}},
        {{0x20, 0x38}, {0x38, 9}},
    };
    OcAchievements achievements = make();
    for (const auto& route : routes) {
      game::Achievements steam;
      game::EndingTracker endings;
      std::vector<std::string> from_steam, from_tracker;
      for (const auto& [prev, room] : route) {
        game::Achievements::Progress progress;
        progress.prev_room = prev;
        progress.room = room;
        steam.tick(kSampleTicks, progress, false);
        for (const char* name : steam.take_earned()) from_steam.emplace_back(name);
        if (const int ending = endings.sample(room, prev); ending >= 0) from_tracker.push_back(game::Achievements::api_names()[static_cast<size_t>(9 + ending)]);
        sample(achievements, at_room(prev, room));
      }
      CHECK(from_steam.size() == 1);
      CHECK(from_steam == from_tracker);
    }
    CHECK(!has(achievements, "oc_end_ngplus_done"));
    const std::optional<OcAchievements::Progress> seen = achievements.progress(row_of(achievements, "oc_end_all"));
    CHECK(seen && seen->have == 4 && seen->need == game::EndingTracker::kEndings);
    OcAchievements::Sample ending = at_room(0x20, 0x38);
    ending.var = [](int id) { return id == kNgPlusVar ? 2 : 0; };
    sample(achievements, ending);
    ending.prev_room = 0x38;
    ending.room = 9;
    sample(achievements, ending);
    CHECK(has(achievements, "oc_end_ngplus_done"));
  }
  {  // Gates: off evaluates nothing; a session that used the debug tools records nothing; an unlock is never evaluated again.
    OcAchievements off = make();
    off.set_enabled(false);
    CHECK(off.tick(kSampleTicks, false) == 0);
    off.sample(at_room(0, 197));
    off.battle_ended(389, 0, true, 1, {-1, -1, -1});
    off.forged(5);
    off.item_given(0x435);
    CHECK(off.unlocked_count() == 0);
    CHECK(off.profile().counters.empty());
    CHECK(!off.take_dirty());
    OcAchievements blocked = make();
    blocked.set_blocked(true);
    blocked.sample(at_room(0, 197));
    blocked.battle_ended(389, 0, true, 1, {-1, -1, -1});
    present(blocked, {{cast(0, 37)}});
    CHECK(blocked.unlocked_count() == 0);
    CHECK(blocked.profile().counters.empty());
    CHECK(blocked.take_unlocked().empty());
    OcAchievements once = make();
    once.sample(at_room(0, 197));
    once.set_clock([] { return std::string("2030-01-01T00:00Z"); });
    once.take_unlocked();
    once.sample(at_room(0, 194));
    CHECK(once.take_unlocked().empty());
    CHECK(once.unlock_time(row_of(once, "oc_story_manor")) == "2026-11-16T14:02Z");
  }
  {  // The file: sections, unknown ids and sections kept, LF lines; a bad value fails the parse.
    const std::string text =
        "\xEF\xBB\xBF[Unlocked]\r\noc_boss_hydra=2026-11-16T14:02Z\r\noc_from_a_newer_build=2027-01-02T03:04Z\r\n; comment\r\n[Counters]\r\nbattles_won=212\r\n"
        "steals=31\r\nforges=7\r\n[Seen]\r\noc_story_three_routes=5\r\n[Later]\r\nkey=value\r\n";
    const std::optional<OcAchievements::Profile> profile = OcAchievements::parse_profile(text);
    CHECK(profile.has_value());
    CHECK(profile->unlocked.size() == 2);
    CHECK(profile->unlocked.at("oc_from_a_newer_build") == "2027-01-02T03:04Z");
    CHECK(profile->counters.at("battles_won") == 212);
    CHECK(profile->seen.at("oc_story_three_routes") == 5);
    CHECK(profile->other.size() == 1 && profile->other[0].first == "Later");
    const std::string written = OcAchievements::serialize_profile(*profile);
    CHECK(written.find('\r') == std::string::npos);
    CHECK(written.find("[Unlocked]\n") == 0);
    CHECK(written.find("oc_from_a_newer_build=2027-01-02T03:04Z\n") != std::string::npos);
    CHECK(written.find("[Later]\nkey=value\n") != std::string::npos);
    const std::optional<OcAchievements::Profile> again = OcAchievements::parse_profile(written);
    CHECK(again && OcAchievements::serialize_profile(*again) == written);
    OcAchievements achievements = make();
    achievements.load(*profile);
    CHECK(has(achievements, "oc_boss_hydra"));
    CHECK(achievements.unlocked_count() == 1);
    achievements.tick(1, false);
    CHECK(has(achievements, "oc_fight_100"));
    CHECK(has(achievements, "oc_fight_steal_10"));
    CHECK(!has(achievements, "oc_fight_steal_50"));
    CHECK(OcAchievements::serialize_profile(achievements.profile()).find("oc_from_a_newer_build=2027-01-02T03:04Z") != std::string::npos);
    CHECK(!OcAchievements::parse_profile("[Counters]\nsteals=many\n"));
    CHECK(!OcAchievements::parse_profile("[Seen]\noc_place_fates=-1\n"));
    CHECK(!OcAchievements::parse_profile("[Unlocked]\noc_boss_hydra=yesterday\n"));
    CHECK(OcAchievements::parse_profile("").has_value());
    CHECK(OcAchievements::utc_now().size() == 17 && OcAchievements::utc_now().back() == 'Z');
  }
  {  // The popup: a panel takes every unlock waiting when it opens; unlocks during a panel wait for the next one.
    game::OcAchievementPopup popup;
    CHECK(popup.update(1.0, false).empty());
    for (const char* title : {"First", "Second", "Third", "Fourth", "Fifth"}) popup.push(title);
    CHECK(popup.update(1.0, true).empty());
    CHECK(popup.update(1.0, false).size() == 5);
    popup.push("Late");
    CHECK(popup.update(200.0, false).size() == 5);
    CHECK(popup.update(100.0, true).size() == 5);
    const std::vector<std::string> next = popup.update(40.0, false);
    CHECK(next.size() == 1 && next[0] == "Late");
    CHECK(popup.update(240.0, false).empty());
    popup.push("Dropped");
    popup.clear();
    CHECK(popup.update(1.0, false).empty());
  }
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("oc achievements: ok\n");
  return failures ? 1 : 0;
}
