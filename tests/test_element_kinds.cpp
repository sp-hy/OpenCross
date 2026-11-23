// Element kinds (data::element_kind): the element list's Attack first / Heal first orders and the New Game+ carry-over decide by the
// record. With the game's records every vanilla id keeps the group the id blocks gave it (checked id by id, on the full element set
// and on a forged stock with holes); a mod's records past the vanilla ones are placed and carried by their kind.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "data/tables.h"
#include "game/new_game_plus.h"
#include "platform/paths.h"
#include "save/cc_save.h"
#include "ui/menu/game_state.h"
#include "vfs/vfs.h"

using namespace oc;
using ui::GameState;
using ui::MenuRules;
using ui::SortMode;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// The id blocks the orders and the carry-over used before they read the record: group and key per order.
struct Placement {
  int group = 0, sub = 0;
  bool operator==(const Placement&) const = default;
};
Placement block_attack_first(int id) { return id >= 1 && id <= 0x30 ? Placement{0, -id} : Placement{1, id}; }
Placement block_heal_first(int id) {
  if (id == 0x80) return {0, 0};
  if (id >= 0x55 && id <= 0x6B) return {1, id};
  if (id >= 0x31 && id <= 0x54) return {2, id};
  if (id >= 0x01 && id <= 0x30) return {3, -id};
  if (id >= 0x6C && id <= 0x7D) return {4, -id};
  return {5, id};
}
bool block_carries(int id) { return id > 0 && id < 0x7E && (id < 0x25 || id > 0x30); }

Placement kind_attack_first(const MenuRules& rules, int id) {
  const data::ElementKind kind = rules.el_kind(id);
  return kind == data::ElementKind::Attack || kind == data::ElementKind::Summon ? Placement{0, -id} : Placement{1, id};
}
Placement kind_heal_first(const MenuRules& rules, int id) {
  if (id == 0x80) return {0, 0};
  switch (rules.el_kind(id)) {
    case data::ElementKind::Attack:
    case data::ElementKind::Summon: return {3, -id};
    case data::ElementKind::Support: return {2, id};
    case data::ElementKind::Healing:
    case data::ElementKind::Consumable: return {1, id};
    case data::ElementKind::Trap: return {4, -id};
    default: return {5, id};
  }
}

// The stock ids after sort_elements, and the order the id blocks give the same stock (stable by group, then key).
std::vector<int> stock_ids(const GameState& game_state) {
  std::vector<int> ids;
  for (const save::ItemStack& stack : game_state.slot.elements)
    if (stack.id != 0) ids.push_back(stack.id);
  return ids;
}
std::vector<int> block_order(const GameState& game_state, SortMode mode) {
  struct Entry {
    int id;
    Placement placement;
  };
  std::vector<Entry> entries;
  for (const save::ItemStack& stack : game_state.slot.elements)
    if (stack.id != 0 && stack.qty != 0)
      entries.push_back({stack.id, mode == SortMode::AttackFirst ? block_attack_first(stack.id) : block_heal_first(stack.id)});
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) {
    return lhs.placement.group != rhs.placement.group ? lhs.placement.group < rhs.placement.group : lhs.placement.sub < rhs.placement.sub;
  });
  std::vector<int> ids;
  for (const Entry& entry : entries) ids.push_back(entry.id);
  return ids;
}

void check_sorts_match_blocks(const MenuRules& rules, const save::SaveSlot& stock, const char* what) {
  for (SortMode mode : {SortMode::AttackFirst, SortMode::HealFirst}) {
    GameState game_state(stock, &rules);
    const std::vector<int> expected = block_order(game_state, mode);
    game_state.sort_elements(mode, 0);
    const bool same = stock_ids(game_state) == expected;
    if (!same) std::printf("FAIL %s: %s order differs from the id blocks\n", what, mode == SortMode::AttackFirst ? "attack first" : "heal first");
    CHECK(same);
  }
}

// The game's own records: every vanilla id is grouped and carried as its id block says.
void check_vanilla(const data::GameTables& tables) {
  MenuRules rules;
  rules.tables = &tables;
  CHECK(static_cast<int>(tables.elements.size()) == data::kVanillaElemRecs);
  int mismatches = 0;
  for (int id = 0; id < data::kVanillaElemRecs; ++id) {
    const bool attack = kind_attack_first(rules, id) == block_attack_first(id), heal = kind_heal_first(rules, id) == block_heal_first(id),
               carry = game::ngplus::carries_element(tables.elements, id) == block_carries(id);
    if (attack && heal && carry) continue;
    std::printf("FAIL vanilla id 0x%X: attack first %d heal first %d carry %d\n", id, attack, heal, carry);
    ++mismatches;
  }
  CHECK(mismatches == 0);

  // What the kinds are: the counts of each over the element ids 1..0x7F.
  int counts[7] = {};
  for (int id = 1; id < 0x80; ++id) ++counts[static_cast<int>(data::element_kind(tables.elements, id))];
  std::printf("vanilla kinds 1..0x7F: attack %d summon %d support %d healing %d consumable %d trap %d other %d\n", counts[0], counts[1],
              counts[2], counts[3], counts[4], counts[5], counts[6]);
  CHECK(counts[0] == 36 && counts[1] == 12 && counts[2] == 36 && counts[3] == 12 && counts[4] == 11 && counts[5] == 18 && counts[6] == 2);

  // The full element set in id order, then reversed, with a tech and the HealFirst id 0x80 among them.
  save::SaveSlot full;
  int at = 0;
  for (int id = 1; id < 0x80; ++id) full.elements[static_cast<size_t>(at++)] = {static_cast<u16>(id), 1};
  full.elements[static_cast<size_t>(at++)] = {0x80, 1};
  full.elements[static_cast<size_t>(at++)] = {0x9A, 2};
  check_sorts_match_blocks(rules, full, "full set");
  save::SaveSlot reversed;
  std::reverse_copy(full.elements.begin(), full.elements.begin() + at, reversed.elements.begin());
  check_sorts_match_blocks(rules, reversed, "full set reversed");

  // A forged stock: ids over 1..0xFF with repeats, holes and empty stacks, from a fixed seed.
  save::SaveSlot forged;
  u32 seed = 0x2468ACE1u;
  auto next = [&seed] { return seed = seed * 1103515245u + 12345u, static_cast<int>((seed >> 16) & 0x7FFF); };
  for (save::ItemStack& stack : forged.elements) {
    const int roll = next() % 10;
    if (roll < 2) continue;
    stack.id = static_cast<u16>(1 + next() % 0xFF);
    stack.qty = static_cast<u8>(roll == 2 ? 0 : 1 + next() % 99);
  }
  check_sorts_match_blocks(rules, forged, "forged stock");

  // The carry-over of the forged stock equals the id blocks' pick.
  save::SaveSlot target;
  game::ngplus::carry_over(forged, target, &tables);
  save::SaveSlot expected;
  for (const save::ItemStack& stack : forged.elements)
    if (stack.qty != 0 && block_carries(stack.id)) game::ngplus::add_stack(expected.elements, stack.id, stack.qty);
  bool carried_same = true;
  for (size_t i = 0; i < expected.elements.size(); ++i)
    carried_same = carried_same && target.elements[i].id == expected.elements[i].id && target.elements[i].qty == expected.elements[i].qty;
  CHECK(carried_same);
}

// A mod's records past the vanilla ones, one of each kind plus a tech, beside vanilla-shaped records.
void check_mod_records() {
  data::GameTables tables;
  tables.elements.resize(data::kVanillaElemRecs + 8);
  auto flags = [&](int id, u8 flags0, u8 flags1) {
    tables.elements[static_cast<size_t>(id)].raw[0] = flags0;
    tables.elements[static_cast<size_t>(id)].raw[1] = flags1;
  };
  const int base = data::kVanillaElemRecs;
  const int attack = base, summon = base + 1, support = base + 2, healing = base + 3, consumable = base + 4, trap = base + 5, other = base + 6,
            tech = base + 7;
  flags(attack, 0x01, 0x01);
  flags(summon, 0x41, 0x00);
  flags(support, 0x08, 0x01);
  flags(healing, 0x04, 0x01);
  flags(consumable, 0x14, 0x00);
  flags(trap, 0x12, 0x00);
  flags(other, 0x01, 0x00);
  flags(tech, 0x01, 0x02);
  flags(0x10, 0x01, 0x01);
  flags(0x31, 0x08, 0x01);
  flags(0x55, 0x04, 0x01);
  flags(0x6C, 0x12, 0x00);
  CHECK(data::element_kind(tables.elements, attack) == data::ElementKind::Attack);
  CHECK(data::element_kind(tables.elements, summon) == data::ElementKind::Summon);
  CHECK(data::element_kind(tables.elements, support) == data::ElementKind::Support);
  CHECK(data::element_kind(tables.elements, healing) == data::ElementKind::Healing);
  CHECK(data::element_kind(tables.elements, consumable) == data::ElementKind::Consumable);
  CHECK(data::element_kind(tables.elements, trap) == data::ElementKind::Trap);
  CHECK(data::element_kind(tables.elements, other) == data::ElementKind::Other);
  CHECK(data::element_kind(tables.elements, tech) == data::ElementKind::Other);
  // A record past the table is no element.
  CHECK(data::element_kind(tables.elements, base + 8) == data::ElementKind::Other);

  MenuRules rules;
  rules.tables = &tables;
  save::SaveSlot stock;
  int at = 0;
  for (int id : {tech, other, trap, consumable, healing, support, summon, attack, 0x6C, 0x55, 0x31, 0x10})
    stock.elements[static_cast<size_t>(at++)] = {static_cast<u16>(id), 3};

  GameState game_state(stock, &rules);
  game_state.sort_elements(SortMode::AttackFirst, 0);
  // Attack and summon elements first by descending id, then the rest by id; the mod's attack and summon lead (higher ids).
  CHECK((stock_ids(game_state) == std::vector<int>{summon, attack, 0x10, 0x31, 0x55, 0x6C, support, healing, consumable, trap, other, tech}));
  game_state.sort_elements(SortMode::HealFirst, 0);
  // Healing and consumables, support, attack and summons (descending), traps (descending), the rest.
  CHECK((stock_ids(game_state) ==
         std::vector<int>{0x55, healing, consumable, 0x31, support, summon, attack, 0x10, trap, 0x6C, other, tech}));

  // New Game+ carries the mod's elements but its summon, its Other record and its tech.
  save::SaveSlot target;
  game::ngplus::carry_over(stock, target, &tables);
  auto held = [&](int id) {
    for (const save::ItemStack& stack : target.elements)
      if (stack.id == id) return static_cast<int>(stack.qty);
    return 0;
  };
  CHECK(held(attack) == 3 && held(support) == 3 && held(healing) == 3 && held(consumable) == 3 && held(trap) == 3);
  CHECK(held(summon) == 0 && held(other) == 0 && held(tech) == 0);
  CHECK(held(0x10) == 3 && held(0x31) == 3 && held(0x55) == 3 && held(0x6C) == 3);
  // Without the tables no element is carried.
  save::SaveSlot bare;
  game::ngplus::carry_over(stock, bare, nullptr);
  CHECK(std::all_of(bare.elements.begin(), bare.elements.end(), [](const save::ItemStack& stack) { return stack.id == 0; }));
}
}  // namespace

int main() {
  check_mod_records();
  const char* env = std::getenv("OPENCROSS_GAMEDIR");
  const auto dir = platform::find_game_dir(env ? env : "");
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP vanilla records: no game install\n");
  } else {
    data::GameTables tables;
    std::string err;
    if (!tables.load(files, &err)) {
      std::printf("FAIL tables: %s\n", err.c_str());
      return 1;
    }
    check_vanilla(tables);
  }
  if (failures == 0) std::printf("element_kinds: all passed\n");
  return failures == 0 ? 0 : 1;
}
