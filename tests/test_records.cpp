// The record pages' knowledge built from a hand-made room-script scan (game/records_info.h): only rooms reachable from the first room of a
// new game count, carried-over recruitments are left out, a treasure two rooms share counts once, the story counter's drops the scan
// cannot rule out; and the rules that read it against a save (ui/menu/records_info.h): missed and closing characters, one-off gives no
// longer obtainable, a window a drop can reopen never called closed, the treasure tally per area, the areas split by world.
#include <cstdio>

#include "data/tables.h"
#include "game/records_info.h"
#include "import/text.h"

using namespace oc;
using script::Cond;
using script::EventKind;
using script::ScanEvent;
using script::Subject;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

namespace {

constexpr int kLeena = 20, kGuile = 2, kKid = 1;
constexpr int kStoryVar = 0, kTreasureVar = 0x80, kMedalVar = 0x146;

ScanEvent event(EventKind kind, std::vector<int> values, std::vector<Cond> conds = {}) {
  ScanEvent out;
  out.kind = kind;
  out.values = std::move(values);
  out.conds = std::move(conds);
  return out;
}

Cond story(script::Cmp cmp, int value) { return {Subject::Var, 0, cmp, value}; }

game::RoomScan room(const char* stem, std::vector<ScanEvent> events) {
  game::RoomScan out;
  out.stem = stem;
  out.room_id = import::room_id_from_name(stem);
  out.scan.events = std::move(events);
  return out;
}

ScanEvent treasure(int give, int bit) {
  ScanEvent out = event(EventKind::Give, {give});
  out.flag = {kTreasureVar, bit, 0};
  out.flag_at_load = true;
  out.capacity_check = true;
  return out;
}

// opn_a01i -> alu_a01f -> tel_b08i; dummy is entered from nowhere.
game::ScriptScan sample_scan() {
  game::ScriptScan scan;
  scan.rooms.push_back(room("opn_a01i", {event(EventKind::Exit, {import::room_id_from_name("alu_a01f")})}));
  ScanEvent medal = event(EventKind::Give, {0x400 + 52}, {story(script::Cmp::Lt, 36)});
  medal.flag = {kMedalVar, 13, 0};
  scan.rooms.push_back(room("alu_a01f", {event(EventKind::Exit, {import::room_id_from_name("tel_b08i")}),
                                         event(EventKind::Join, {kLeena}, {story(script::Cmp::Eq, 28), {Subject::Recruited, kKid, script::Cmp::Eq, 0}}),
                                         medal, event(EventKind::Give, {0x200 + 7}), treasure(0x200 + 1, 3), treasure(0x10, 4)}));
  scan.rooms.push_back(room("tel_b08i", {event(EventKind::Roster, {kGuile}, {story(script::Cmp::Lt, 56), {Subject::Choice, 4, script::Cmp::Eq, 1}}),
                                         event(EventKind::Roster, {kLeena}, {{Subject::Carryover, kLeena, script::Cmp::Eq, 1}}),
                                         treasure(0x200 + 1, 3)}));
  scan.rooms.push_back(room("dummy", {event(EventKind::Join, {kLeena}), treasure(0x20, 5)}));
  return scan;
}

save::SaveSlot slot_at(int story_value) {
  save::SaveSlot slot;
  slot.set_var(kStoryVar, static_cast<u16>(story_value));
  return slot;
}

}  // namespace

static void test_build() {
  const game::ScriptScan scan = sample_scan();
  const std::vector<bool> reached = game::reachable_rooms(scan);
  CHECK(reached.size() == 4 && reached[0] && reached[1] && reached[2] && !reached[3]);
  data::GameTables tables;
  const ui::RecordsInfo info = game::records_info(scan, tables);
  const ui::CharacterInfo& leena = info.characters[kLeena];
  CHECK(leena.sites.size() == 1 && leena.last_story == 28);
  CHECK(leena.sites.size() == 1 && leena.sites[0].area == "Arni Village" && (leena.sites[0].how & ui::kHowAlone));
  const ui::CharacterInfo& guile = info.characters[kGuile];
  CHECK(guile.sites.size() == 1 && guile.last_story == 55 && (guile.sites[0].how & ui::kHowChoice));
  CHECK(info.characters[kKid].sites.empty() && info.characters[kKid].last_story == ui::kStoryOpen);
  CHECK(info.key_items.contains(52) && info.key_items.at(52).one_offs.size() == 1 && info.key_items.at(52).one_offs[0].story_hi == 35);
  CHECK(info.elements.contains(7) && info.elements.at(7).repeatable && info.elements.at(1).one_offs.size() == 2);
  // The element treasure of alu_a01f and tel_b08i shares one flag: one treasure; dummy's is not reachable.
  CHECK(info.treasures.size() == 2 && info.areas.size() == 1 && info.areas[0].name == "Arni Village" && info.areas[0].world == data::World::None);
}

static void test_rules() {
  data::GameTables tables;
  const ui::RecordsInfo info = game::records_info(sample_scan(), tables);
  CHECK(!ui::character_missed(info, slot_at(28), kLeena));
  CHECK(ui::character_missed(info, slot_at(29), kLeena));
  save::SaveSlot recruited = slot_at(40);
  recruited.set_recruited(kLeena, true);
  CHECK(!ui::character_missed(info, recruited, kLeena) && !ui::character_closing(info, recruited, kLeena));
  save::SaveSlot plus = slot_at(40);
  plus.set_var(0x7E, 2);
  CHECK(!ui::character_missed(info, plus, kLeena));
  CHECK(!ui::character_missed(info, slot_at(200), kKid));
  // Story 26 and 28 are neighbouring story points: the chance ends at the next one.
  CHECK(ui::character_closing(info, slot_at(26), kLeena) && !ui::character_closing(info, slot_at(16), kLeena));
  CHECK(!ui::character_closing(info, slot_at(30), kLeena));
  const ui::GiveInfo& medal = info.key_items.at(52);
  CHECK(!ui::give_missed(info, medal, slot_at(35)) && ui::give_missed(info, medal, slot_at(36)));
  save::SaveSlot taken = slot_at(40);
  taken.set_var(kMedalVar, 1 << 13);
  CHECK(!ui::give_missed(info, medal, taken));
  CHECK(!ui::give_missed(info, info.elements.at(7), slot_at(200)));
}

// A story set of entity 1 of alu_a01f writes `value`. The game enters alu_a01f by entrance 0 at story 0, tel_b08i raises the counter to
// 30, and tel_b08i leads back to alu_a01f by entrance 0.
static game::ScriptScan drop_scan(ScanEvent lowering, script::EntityScan setter = {}) {
  game::ScriptScan scan = sample_scan();
  scan.rooms[0].scan.events[0].entrance = 0;
  game::RoomScan& arni = scan.rooms[1];
  arni.scan.events[0].entrance = 0;
  lowering.entity = 1;
  lowering.slot = 2;
  arni.scan.events.push_back(lowering);
  arni.scan.entities = {script::EntityScan{}, setter};
  game::RoomScan& termina = scan.rooms[2];
  termina.scan.events.push_back(event(EventKind::Scenario, {30}, {story(script::Cmp::Lt, 30)}));
  termina.scan.events.push_back(event(EventKind::Exit, {import::room_id_from_name("alu_a01f")}));
  termina.scan.events.back().entrance = 0;
  return scan;
}

static void test_drops() {
  data::GameTables tables;
  CHECK(game::story_drops(sample_scan()).empty());
  // Set 20 with no test: the counter can go from 30 back to 20, so Leena's window (until 28) can open again; the medal's (until 35) cannot.
  const ui::RecordsInfo info = game::records_info(drop_scan(event(EventKind::Scenario, {20})), tables);
  CHECK((info.story_drops == std::vector<ui::StoryDrop>{{20, 30}}));
  CHECK(ui::story_reopens(info, 28) && !ui::story_reopens(info, 35) && !ui::story_reopens(info, 19));
  CHECK(!ui::character_missed(info, slot_at(40), kLeena) && !ui::character_closing(info, slot_at(26), kLeena));
  CHECK(ui::give_missed(info, info.key_items.at(52), slot_at(36)));
  // The same set behind a test of the story counter, of a variable no script writes, or of an entrance the room is never entered by.
  CHECK(game::story_drops(drop_scan(event(EventKind::Scenario, {20}, {story(script::Cmp::Lt, 20)}))).empty());
  CHECK(game::story_drops(drop_scan(event(EventKind::Scenario, {20}, {{Subject::Var, 0x200, script::Cmp::Eq, 1}}))).empty());
  CHECK(game::story_drops(drop_scan(event(EventKind::Scenario, {20}, {{Subject::Var, 0x02, script::Cmp::Eq, 1}}))).empty());
  game::ScriptScan written = drop_scan(event(EventKind::Scenario, {20}, {{Subject::Var, 0x200, script::Cmp::Eq, 1}}));
  written.rooms[2].scan.written = {0x200};
  CHECK(game::story_drops(written).size() == 1);
  // The setter's INIT makes it inactive when alu_a01f is entered at story 30 or later, unless some script makes it active again.
  script::EntityScan hidden;
  hidden.inactive_at = {{30, ui::kStoryOpen, 0, 0x7FFF}};
  CHECK(game::story_drops(drop_scan(event(EventKind::Scenario, {20}), hidden)).empty());
  hidden.reactivated = true;
  CHECK(game::story_drops(drop_scan(event(EventKind::Scenario, {20}), hidden)).size() == 1);
}

static void test_tally() {
  data::GameTables tables;
  const ui::RecordsInfo info = game::records_info(sample_scan(), tables);
  std::vector<ui::TreasureTally> tally = ui::tally_treasures(info, slot_at(0));
  CHECK(tally.size() == 1 && tally[0].total == 2 && tally[0].opened == 0);
  save::SaveSlot slot = slot_at(0);
  slot.set_var(kTreasureVar, 1 << 3);
  tally = ui::tally_treasures(info, slot);
  CHECK(tally.size() == 1 && tally[0].opened == 1);
  slot.set_var(kTreasureVar, (1 << 3) | (1 << 4) | (1 << 5));
  tally = ui::tally_treasures(info, slot);
  CHECK(tally.size() == 1 && tally[0].opened == 2 && tally[0].total == 2);
}

// opn_a01i leads to Arni in both worlds, Another World's Termina, Terra Tower and the bluffs: Arni Village is split by world, Termina
// (only its Another room here) and Terra Tower keep their names, the bluffs have no place name.
static void test_worlds() {
  game::ScriptScan scan;
  std::vector<int> exits;
  for (const char* stem : {"alu_a01f", "alu_b01f", "tel_b08i", "sta_a01i", "chi_a01d"}) exits.push_back(import::room_id_from_name(stem));
  scan.rooms.push_back(room("opn_a01i", {event(EventKind::Exit, exits)}));
  scan.rooms.push_back(room("alu_a01f", {treasure(0x10, 0)}));
  scan.rooms.push_back(room("alu_b01f", {treasure(0x11, 1), treasure(0x12, 2)}));
  scan.rooms.push_back(room("tel_b08i", {treasure(0x13, 3)}));
  scan.rooms.push_back(room("sta_a01i", {treasure(0x14, 4)}));
  scan.rooms.push_back(room("chi_a01d", {treasure(0x15, 5)}));
  data::GameTables tables;
  const ui::RecordsInfo info = game::records_info(scan, tables);
  using data::World;
  const std::vector<ui::TreasureArea> expected = {
      {"Arni Village", World::Home}, {"Arni Village", World::Another}, {"Termina", World::None}, {"Terra Tower", World::None}, {"", World::None}};
  CHECK(info.areas == expected);
  save::SaveSlot slot = slot_at(0);
  slot.set_var(kTreasureVar, (1 << 1) | (1 << 2) | (1 << 5));
  const std::vector<ui::TreasureTally> tally = ui::tally_treasures(info, slot);
  CHECK(tally.size() == 5);
  if (tally.size() == 5) {
    CHECK(tally[0].total == 1 && tally[0].opened == 0);
    CHECK(tally[1].total == 2 && tally[1].opened == 2);
    CHECK(tally[2].total == 1 && tally[3].total == 1 && tally[4].total == 1 && tally[4].opened == 1);
  }
}

int main() {
  test_build();
  test_rules();
  test_drops();
  test_tally();
  test_worlds();
  if (g_fail == 0) std::printf("records: all passed\n");
  return g_fail == 0 ? 0 : 1;
}
