// The OpenCross collection file: the enemy flags, the "ever had" chunks a game fills, the grid preset chunk, the byte layout, chunks of
// other builds kept on a round trip, and the rejection of damaged files (a temp folder; no real save folder is touched).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "save/collection.h"
#include "save/grid_preset.h"

using namespace oc;
using save::Collection;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool parses(const Bytes& bytes) {
  try {
    Collection::parse(bytes);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

static void test_flags() {
  Collection collection;
  CHECK(collection.empty());
  CHECK(!collection.enemy_met(5));
  CHECK(collection.mark_enemy(5, Collection::kEnemyMet));
  CHECK(!collection.mark_enemy(5, Collection::kEnemyMet));
  CHECK(collection.enemy_met(5) && !collection.enemy_defeated(5));
  CHECK(collection.mark_enemy(5, Collection::kEnemyDefeated | Collection::loot_flag(3)));
  CHECK(collection.enemy_defeated(5) && collection.enemy_loot(5, 3) && !collection.enemy_loot(5, 0));
  CHECK(!collection.enemy_loot(5, 4) && !collection.enemy_loot(5, -1));
  CHECK(!collection.mark_enemy(-1, Collection::kEnemyMet) && !collection.mark_enemy(Collection::kMaxEnemies, Collection::kEnemyMet));
  CHECK(!collection.enemy_met(4) && !collection.enemy_met(200));
  collection.clear();
  CHECK(collection.empty() && !collection.enemy_met(5));
}

static void test_layout() {
  Collection collection;
  collection.mark_enemy(2, Collection::kEnemyMet | Collection::kEnemyDefeated);
  const Bytes bytes = collection.serialize();
  // "OCCL", version 1, one chunk "ENMY" of 3 bytes (records 0..2), CRC-32.
  CHECK(bytes.size() == 12 + 8 + 3 + 4);
  CHECK(bytes[0] == 'O' && bytes[1] == 'C' && bytes[2] == 'C' && bytes[3] == 'L');
  CHECK(bytes[4] == 1 && bytes[5] == 0 && bytes[8] == 1);
  CHECK(bytes[12] == 'E' && bytes[13] == 'N' && bytes[14] == 'M' && bytes[15] == 'Y' && bytes[16] == 3);
  CHECK(bytes[20] == 0 && bytes[21] == 0 && bytes[22] == 3);
  const Collection back = Collection::parse(bytes);
  CHECK(back.enemy_met(2) && back.enemy_defeated(2) && !back.enemy_met(1));
}

static void test_grid_presets() {
  Collection collection;
  save::GridPresets presets = save::GridPresets::read(collection);
  CHECK(presets.all().empty());
  save::GridPreset first;
  first.character = 1;
  first.slot = 2;
  first.cells = {{0, 2, 17, 1}, {3, 9, 0x31, 5}};
  presets.set(first);
  save::GridPreset other = first;
  other.character = 0;
  other.slot = 0;
  presets.set(other);
  presets.write(collection);
  const Collection back = Collection::parse(collection.serialize());
  const save::GridPresets read = save::GridPresets::read(back);
  CHECK(read.all().size() == 2 && !read.find(1, 0) && !read.find(1, 3));
  const save::GridPreset* found = read.find(1, 2);
  CHECK(found && found->cells.size() == 2 && found->cells[1].row == 3 && found->cells[1].col == 9 && found->cells[1].id == 0x31 &&
        found->cells[1].count == 5);
  // Version 1, two presets, then per preset character, slot, cell count and 5 bytes a cell.
  const Bytes payload = presets.serialize();
  CHECK(payload.size() == 3 + 2 * (3 + 2 * 5) && payload[0] == 1 && payload[1] == 2 && payload[2] == 0);
  // Setting a preset again replaces it; an empty one removes it.
  save::GridPresets edited = read;
  edited.set({1, 2, {{1, 1, 5, 1}}});
  CHECK(edited.all().size() == 2 && edited.find(1, 2)->cells.size() == 1);
  edited.set({1, 2, {}});
  CHECK(edited.all().size() == 1 && !edited.find(1, 2));
  // Damaged payloads are refused and read as none.
  auto parses_presets = [](const Bytes& bytes) {
    try {
      save::GridPresets::parse(bytes);
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  CHECK(parses_presets({1, 0, 0}) && !parses_presets({}) && !parses_presets({2, 0, 0}));
  Bytes truncated = payload;
  truncated.pop_back();
  CHECK(!parses_presets(truncated));
  Bytes bad_slot = payload;
  bad_slot[4] = 3;
  CHECK(!parses_presets(bad_slot));
  Bytes repeated = payload;
  repeated[3 + 13] = repeated[3];
  repeated[3 + 14] = repeated[4];
  CHECK(!parses_presets(repeated));
  Collection damaged;
  damaged.set_chunk(save::GridPresets::kChunk, truncated);
  CHECK(save::GridPresets::read(damaged).all().empty());
}

static void test_foreign_chunks() {
  Collection collection;
  collection.mark_enemy(0, Collection::kEnemyMet);
  const u32 other = save::collection_tag("XTRA");
  collection.set_chunk(other, Bytes{9, 8, 7});
  const Collection back = Collection::parse(collection.serialize());
  CHECK(back.chunk(other) && *back.chunk(other) == (Bytes{9, 8, 7}));
  CHECK(back.serialize() == collection.serialize());
}

static void test_damaged() {
  Collection collection;
  collection.mark_enemy(7, Collection::kEnemyMet);
  const Bytes good = collection.serialize();
  CHECK(parses(good));
  Bytes flipped = good;
  flipped[20] ^= 1;
  CHECK(!parses(flipped));
  Bytes magic = good;
  magic[0] = 'X';
  CHECK(!parses(magic));
  Bytes newer = good;
  newer[4] = 2;
  CHECK(!parses(newer));
  CHECK(!parses(Bytes(good.begin(), good.begin() + 10)));
  CHECK(!parses(Bytes(good.begin(), good.end() - 1)));
}

static void test_file() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "oc_test_collection";
  std::filesystem::remove_all(dir);
  const std::filesystem::path path = dir / "OpenCross" / Collection::kFileName;
  Collection collection;
  collection.set_path(path);
  CHECK(collection.load() && collection.empty());
  collection.mark_enemy(12, Collection::kEnemyDefeated);
  CHECK(collection.save());
  CHECK(std::filesystem::exists(path) && !std::filesystem::exists(path.string() + ".tmp"));
  Collection again;
  again.set_path(path);
  CHECK(again.load() && again.enemy_defeated(12));
  {
    std::ofstream junk(path, std::ios::binary | std::ios::trunc);
    junk << "not a collection";
  }
  Collection bad;
  bad.set_path(path);
  CHECK(!bad.load() && bad.empty());
  CHECK(!std::filesystem::exists(path) && std::filesystem::exists(path.string() + ".bad"));
  std::filesystem::remove_all(dir);
}

// A slot's recruited characters, the elements in its stock and in the recruited characters' grids, and its key items are marked; an
// unrecruited character's grid, techs and empty stacks are not. Marking again changes nothing, and the chunks survive a round trip.
static void test_had() {
  save::SaveSlot slot;
  slot.set_recruited(0, true);
  slot.set_recruited(5, true);
  slot.chars[5].grid[0] = 0x4000 | 7;
  slot.chars[5].grid[1] = 0x85;
  slot.chars[9].grid[0] = 12;
  slot.elements[0] = {9, 1};
  slot.elements[1] = {11, 0};
  slot.set_key_items((1ull << 2) | (1ull << 63));
  Collection collection;
  CHECK(collection.record_slot(slot));
  CHECK(!collection.record_slot(slot));
  CHECK(collection.had(Collection::kCharacterChunk, 0) && collection.had(Collection::kCharacterChunk, 5));
  CHECK(!collection.had(Collection::kCharacterChunk, 9) && !collection.had(Collection::kCharacterChunk, 1));
  CHECK(collection.had(Collection::kElementChunk, 7) && collection.had(Collection::kElementChunk, 9));
  CHECK(!collection.had(Collection::kElementChunk, 12) && !collection.had(Collection::kElementChunk, 11));
  CHECK(!collection.had(Collection::kElementChunk, 5) && !collection.had(Collection::kElementChunk, 0x85));
  CHECK(collection.had(Collection::kKeyItemChunk, 3) && collection.had(Collection::kKeyItemChunk, 64));
  CHECK(!collection.had(Collection::kKeyItemChunk, 2) && !collection.had(Collection::kKeyItemChunk, 4));
  CHECK(!collection.mark_had(Collection::kKeyItemChunk, -1) && !collection.mark_had(Collection::kKeyItemChunk, Collection::kMaxHadId));
  const Collection copy = Collection::parse(collection.serialize());
  CHECK(copy.had(Collection::kCharacterChunk, 5) && copy.had(Collection::kElementChunk, 7) && copy.had(Collection::kKeyItemChunk, 64));
  CHECK(copy.chunk(Collection::kCharacterChunk)->size() == 6 && copy.chunk(Collection::kKeyItemChunk)->size() == 65);
  CHECK(!copy.enemy_met(0));
}

int main() {
  test_flags();
  test_had();
  test_layout();
  test_grid_presets();
  test_foreign_chunks();
  test_damaged();
  test_file();
  if (g_fail == 0) std::printf("collection: all passed\n");
  return g_fail == 0 ? 0 : 1;
}
