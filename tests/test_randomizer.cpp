// The randomizer's rules on hand-built pools and a hand-built scan (no game data): the same seed gives the same plan and spoiler log,
// every pool stays a permutation (each thing exactly once), the balanced shuffle keeps a thing near its rank while a pure one does not,
// key items and bosses and scripted battles never move, a chest's literal words are rewritten in both operand forms, moved drops and
// shop rows read back, and a moved encounter group is scaled by the two areas' means.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/features.h"
#include "game/randomizer.h"
#include "game/room_scan.h"
#include "script/script_scan.h"

using namespace oc;
using namespace oc::game::randomizer;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

Pools sample_pools() {
  Pools pools;
  for (int i = 0; i < 60; ++i)
    pools.entries[static_cast<size_t>(Pool::ChestItems)].push_back({static_cast<u32>(1000 + i), 0, 1 + i, 10 * (i + 1), i % 7 == 0 ? -1 : i * 3, "chest"});
  for (int i = 0; i < 20; ++i)
    pools.entries[static_cast<size_t>(Pool::ChestElements)].push_back({static_cast<u32>(2000 + i), 0, 0x200 + 1 + i, 100 * (i + 1), -1, "pickup"});
  pools.drops.resize(4);
  for (u32 row = 0; row < 4; ++row)
    for (u32 slot = 0; slot < 4; ++slot) {
      const int thing = slot == 3 ? 0x200 + 30 + static_cast<int>(row) : 70 + static_cast<int>(row * 4 + slot);
      pools.drops[row].slot[slot] = thing_drop(thing);
      pools.entries[static_cast<size_t>(thing >= 0x200 ? Pool::DropElements : Pool::DropItems)].push_back({row * 4 + slot, 0, thing, thing, -1, "row"});
    }
  pools.el_shop = {{1, 2, 3, 4, 5}, {6, 7, 8, 9, 10}};
  pools.smith_shop = {{100, 101, 102}, {103, 104, 105}};
  for (u32 row = 0; row < 2; ++row)
    for (u32 index = 0; index < 5; ++index)
      pools.entries[static_cast<size_t>(Pool::ElementStock)].push_back({row * 64 + index, 0, 0x200 + pools.el_shop[row][index], 50, -1, "shop"});
  for (u32 row = 0; row < 2; ++row)
    for (u32 index = 0; index < 3; ++index)
      pools.entries[static_cast<size_t>(Pool::SmithStock)].push_back({row * 64 + index, 0, pools.smith_shop[row][index], 500, -1, "smith"});
  pools.areas["aaa_a"] = {"A", 100, 20, 50, 10};
  pools.areas["bbb_a"] = {"B", 400, 40, 200, 60};
  for (int scene = 1; scene <= 6; ++scene) {
    const std::string area = scene <= 3 ? "aaa_a" : "bbb_a";
    pools.encounters.push_back({scene, area, {scene * 10, -1, -1}});
    pools.entries[static_cast<size_t>(Pool::Encounters)].push_back({static_cast<u32>(scene), 0, scene, pools.areas[area].hp, pools.areas[area].stage, area});
  }
  pools.literals["tst_a01f"] = {{2, 1000, 1}, {6, 1001, 2}};
  return pools;
}

std::vector<int> things_of(const std::vector<Entry>& entries) {
  std::vector<int> out;
  for (const Entry& entry : entries) out.push_back(entry.thing);
  std::sort(out.begin(), out.end());
  return out;
}

void check_shuffles() {
  std::vector<Entry> entries;
  for (int i = 0; i < 200; ++i) entries.push_back({static_cast<u32>(i), 0, i, 10 * i, -1, ""});
  for (int spread : {0, 30, 100}) {
    const std::vector<size_t> perm = shuffle(entries, 77, 0, spread);
    std::vector<size_t> sorted = perm;
    std::sort(sorted.begin(), sorted.end());
    bool permutation = true;
    for (size_t i = 0; i < sorted.size(); ++i) permutation = permutation && sorted[i] == i;
    CHECK(permutation);
    CHECK(perm == shuffle(entries, 77, 0, spread));
    CHECK(perm != shuffle(entries, 78, 0, spread));
    long total = 0, widest = 0;
    for (size_t i = 0; i < perm.size(); ++i) {
      const long moved = std::labs(static_cast<long>(perm[i]) - static_cast<long>(i));
      total += moved, widest = std::max(widest, moved);
    }
    // Balanced: within the window (8 of 200); pure: a typical thing travels a third of the pool.
    if (spread == 0) CHECK(widest <= 8 && total > 0);
    if (spread == 30) CHECK(widest <= 64 && widest > 8);
    if (spread == 100) CHECK(total / 200 > 40);
  }
}

void check_plan() {
  const Pools pools = sample_pools();
  Settings settings;
  settings.items = settings.elements = settings.enemies = true;
  settings.seed = 4242;
  const Plan plan = build(settings, pools);
  const Plan again = build(settings, pools);
  CHECK(plan.spoiler() == again.spoiler());
  Settings other = settings;
  other.seed = 4243;
  CHECK(build(other, pools).spoiler() != plan.spoiler());

  // Every pool is a permutation: the things after the shuffle are the things before.
  std::vector<int> chest_after;
  for (const Entry& entry : pools.entries[static_cast<size_t>(Pool::ChestItems)]) chest_after.push_back(plan.treasure(entry.site, entry.thing));
  std::sort(chest_after.begin(), chest_after.end());
  CHECK(chest_after == things_of(pools.entries[static_cast<size_t>(Pool::ChestItems)]));
  CHECK(plan.counts()[static_cast<size_t>(Pool::ChestItems)].second > 0);
  const auto drops = plan.drops();
  CHECK(drops && drops->size() == pools.drops.size());
  std::vector<int> drop_before, drop_after;
  for (size_t row = 0; row < pools.drops.size(); ++row)
    for (size_t slot = 0; slot < 4; ++slot) {
      drop_before.push_back(drop_thing(pools.drops[row].slot[slot]));
      drop_after.push_back(drop_thing((*drops)[row].slot[slot]));
      // Items stay items and elements stay elements.
      CHECK(((*drops)[row].slot[slot] & 0x8000) == (pools.drops[row].slot[slot] & 0x8000));
    }
  std::sort(drop_before.begin(), drop_before.end());
  std::sort(drop_after.begin(), drop_after.end());
  CHECK(drop_before == drop_after);
  std::vector<int> stock;
  for (int shop = 0; shop < 2; ++shop) {
    const std::vector<int> row = plan.stock(true, shop, {});
    CHECK(row.size() == 5);
    std::vector<int> unique = row;
    std::sort(unique.begin(), unique.end());
    CHECK(std::unique(unique.begin(), unique.end()) == unique.end());
    stock.insert(stock.end(), row.begin(), row.end());
  }
  std::sort(stock.begin(), stock.end());
  CHECK(stock == std::vector<int>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  CHECK(plan.stock(false, 0, {}).size() == 3 && plan.stock(false, 7, {9}) == std::vector<int>({9}));

  // Encounters: a permutation of the scenes; a group moved from area B into area A is scaled by A's means over B's and rolls the drop
  // row of the group it replaced.
  std::vector<int> scenes;
  for (int scene = 1; scene <= 6; ++scene) scenes.push_back(plan.scene(scene));
  std::sort(scenes.begin(), scenes.end());
  CHECK(scenes == std::vector<int>({1, 2, 3, 4, 5, 6}));
  CHECK(plan.scene(99) == 99);
  for (int scene = 1; scene <= 6; ++scene) {
    const int guest = plan.scene(scene);
    const auto tuning = plan.tuning(scene);
    if (guest == scene) {
      CHECK(tuning[0].hp_percent == 100 && tuning[0].drop_row == -1);
      continue;
    }
    CHECK(tuning[0].drop_row == scene * 10);
    if (scene <= 3 && guest > 3) CHECK(tuning[0].hp_percent == 25 && tuning[0].stat_percent == 50 && tuning[0].gold_percent == 25);
    if (scene > 3 && guest <= 3) CHECK(tuning[0].hp_percent == 400 && tuning[0].stat_percent == 200 && tuning[0].gold_percent == 400);
  }

  // The chest literals: a U operand (literal flag 0x8000) and an S operand (plain word) of moved treasures are rewritten; a word that no
  // longer holds the treasure's thing is left alone.
  Bytes code(10, 0);
  code[2] = 0x01, code[3] = 0x80;
  code[6] = 0x02, code[7] = 0x00;
  const int first = plan.treasure(1000, 1), second = plan.treasure(1001, 2);
  const int expected = (first != 1 ? 1 : 0) + (second != 2 ? 1 : 0);
  CHECK(plan.patch_room("tst_a01f", code) == expected);
  CHECK((code[2] | (code[3] << 8)) == (first | 0x8000));
  CHECK((code[6] | (code[7] << 8)) == second);
  CHECK(plan.patch_room("tst_a02f", code) == 0);

  // Off: nothing moves; a tier switched off leaves its pools alone.
  Settings off = settings;
  off.items = off.elements = off.enemies = false;
  const Plan none = build(off, pools);
  CHECK(none.treasure(1000, 1) == 1 && !none.drops() && none.scene(1) == 1 && none.stock(true, 0, {1, 2}) == std::vector<int>({1, 2}));
  Settings items_only = settings;
  items_only.elements = items_only.enemies = false;
  const Plan items = build(items_only, pools);
  CHECK(items.stock(true, 0, {1}) == std::vector<int>({1}) && items.scene(1) == 1);
  for (const Entry& entry : pools.entries[static_cast<size_t>(Pool::ChestElements)]) CHECK(items.treasure(entry.site, entry.thing) == entry.thing);
}

// The randomizer is on while any switch is: no switch is the game as it ships, each of the five alone turns it on, and a stored
// "Randomizer.Enabled=0" (its former master switch) turns every switch off wherever it sits, "Enabled=1" changes nothing.
void check_switches() {
  const Pools pools = sample_pools();
  Settings none;
  CHECK(!none.enabled());
  const Plan plain = build(none, pools);
  CHECK(plain.treasure(1000, 1) == 1 && !plain.drops() && plain.scene(1) == 1 && plain.stock(true, 0, {1, 2}) == std::vector<int>({1, 2}));
  for (bool Settings::*member : {&Settings::items, &Settings::elements, &Settings::enemies, &Settings::start_gear, &Settings::start_elements}) {
    Settings one;
    one.*member = true;
    CHECK(one.enabled());
  }

  const auto resolved = [](std::vector<std::pair<std::string, std::string>> overrides) { return features::resolve({}, overrides); };
  CHECK(!settings_from(resolved({})).enabled());
  CHECK(settings_from(resolved({{"Randomizer.Items", "1"}})).enabled());
  CHECK(!settings_from(resolved({{"Randomizer.Enabled", "0"}, {"Randomizer.Items", "1"}, {"Randomizer.Enemies", "1"}})).enabled());
  CHECK(!settings_from(resolved({{"Randomizer.Items", "1"}, {"Randomizer.Enabled", "0"}})).enabled());
  const Settings kept = settings_from(resolved({{"Randomizer.Enabled", "1"}, {"Randomizer.Items", "1"}, {"Randomizer.Seed", "77"}}));
  CHECK(kept.items && kept.seed == 77 && !kept.elements);
  const Settings seeded = settings_from(resolved({{"Randomizer.Enabled", "0"}, {"Randomizer.Seed", "77"}, {"Randomizer.Spread", "40"}}));
  CHECK(!seeded.enabled() && seeded.seed == 77 && seeded.spread == 40);
  CHECK(resolved({{"Randomizer.Enabled", "0"}}).issues.empty() && resolved({{"Randomizer.Enabled", "1"}}).issues.empty());

  const features::Manifest manifest = features::parse_manifest("[Randomizer]\nEnabled=0\nItems=1\nElements=1\n");
  const std::vector<features::ModInput> mods = {{"legacy", &manifest}};
  CHECK(!settings_from(features::resolve(mods, {})).enabled());
  CHECK(settings_from(features::resolve(mods, std::vector<std::pair<std::string, std::string>>{{"Randomizer.Elements", "1"}})).enabled());
}

// gather() on a one-room scan: a key item never enters a pool; a boss scene and a scene a direct scene start also reaches stay out of the
// encounter groups.
void check_gather() {
  game::ScriptScan scan;
  game::RoomScan& room = scan.rooms.emplace_back();
  room.stem = "tst_a01f";
  const auto give = [&](int thing, int bit, u32 pos) {
    script::ScanEvent event;
    event.kind = script::EventKind::Give;
    event.values = {thing};
    event.flag = {0x80, bit, 0};
    event.flag_at_load = true;
    event.value_sites = {pos};
    room.scan.events.push_back(event);
  };
  give(0x401, 0, 10);
  give(5, 1, 20);
  give(0x205, 2, 30);
  const auto fight = [&](int entity, u16 op, int battle_id) {
    script::ScanEvent event;
    event.kind = script::EventKind::Battle;
    event.entity = entity;
    event.op = op;
    event.values = {battle_id};
    room.scan.events.push_back(event);
  };
  fight(1, 0xA3, 0);
  fight(1, 0xA3, 1);
  fight(1, 0xA3, 2);
  fight(2, 0xC9, 0x200 + 2);
  fight(1, 0xA3, 3);
  data::GameTables tables;
  tables.scenes.resize(4);
  for (data::SceneRec& scene : tables.scenes) scene.raw.fill(0xFF), scene.raw[3] = 1, scene.raw[4] = 0, scene.raw[5] = 0, scene.raw[0] = 0, scene.raw[0x1A] = 0;
  tables.scenes[0].raw[0x1A] = 0x80;
  tables.symtbl.assign(4 * 8, 0);
  for (u16 id = 0; id < 4; ++id)
    for (u16 variant = 0; variant < 8; ++variant) tables.symtbl[id * 8u + variant] = id;
  tables.enemies.resize(1);
  const Pools pools = gather(scan, tables, nullptr, {});
  const auto& items = pools.entries[static_cast<size_t>(Pool::ChestItems)];
  const auto& elements = pools.entries[static_cast<size_t>(Pool::ChestElements)];
  CHECK(items.size() == 1 && items[0].thing == 5);
  CHECK(elements.size() == 1 && elements[0].thing == 0x205);
  for (size_t pool = 0; pool < kPools; ++pool)
    for (const Entry& entry : pools.entries[pool]) CHECK(entry.thing != 0x401 || static_cast<Pool>(pool) == Pool::Encounters);
  std::vector<int> normal;
  for (const Encounter& encounter : pools.encounters) normal.push_back(encounter.scene);
  CHECK(normal == std::vector<int>({1, 3}));
  Settings settings;
  settings.spread = 100;
  settings.items = settings.elements = settings.enemies = true;
  for (u32 seed = 1; seed <= 20; ++seed) {
    settings.seed = seed;
    const Plan plan = build(settings, pools);
    CHECK(plan.treasure(0x80 * 8 + 0, 0x401) == 0x401);
    CHECK(plan.scene(0) == 0 && plan.scene(2) == 2);
  }
}

}  // namespace

int main() {
  check_shuffles();
  check_plan();
  check_switches();
  check_gather();
  if (failures) return 1;
  std::printf("randomizer checks passed\n");
  return 0;
}
