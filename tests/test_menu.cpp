// Menu rule tests on synthetic data: grid allocation legality, stock accounting, auto-fill, the three sort orders,
// equipment compatibility / stat previews, element use, the options word, the Status model camera between ticks and the language
// suffix lookup of HD images.
#include <cstdio>

#include <cmath>
#include <string>
#include <vector>

#include "ui/localized_path.h"
#include "ui/menu/game_state.h"
#include "ui/menu/hand.h"
#include "ui/menu/menu_model.h"
#include "ui/menu/use_flow.h"
#include "ui/menu/ps1_art.h"

using namespace oc;
using namespace oc::ui;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

struct Fixture {
  data::GameTables tables;
  MenuRules rules;
  GameState gs;

  void el(int id, int level, int colour, u8 window, u8 flag0 = 0x01, u8 flag1 = 0, u8 flag3 = 0) {
    auto& element = tables.elements[static_cast<size_t>(id)];
    element.raw[0] = flag0;
    element.raw[1] = flag1;
    element.raw[3] = flag3;
    element.raw[5] = static_cast<u8>(colour);
    element.raw[6] = static_cast<u8>(level);
    element.raw[0x12] = window;
    // Eligible for everybody.
    rules.pg[static_cast<size_t>(id)].raw.fill(0xFF);
  }

  Fixture() {
    tables.elements.resize(0x90);
    rules.pg.resize(0x90);
    auto& char_set = tables.char_sets["char"];
    // Characters 0 and 1 share Serge's shape 03FC x3, 03F8, 01F0, 01E0 x3 (48 cells).
    const u16 masks[8] = {0x3FC, 0x3FC, 0x3FC, 0x3F8, 0x1F0, 0x1E0, 0x1E0, 0x1E0};
    for (int character = 0; character < 2; ++character)
      for (int row = 0; row < 8; ++row) {
        char_set.rec[static_cast<size_t>(character)].raw[static_cast<size_t>(0x30 + 2 * row)] = static_cast<u8>(masks[row]);
        char_set.rec[static_cast<size_t>(character)].raw[static_cast<size_t>(0x31 + 2 * row)] = static_cast<u8>(masks[row] >> 8);
      }
    char_set.rec[1].raw[1] = 0x20;
    // Character 0 keeps its innate colour in the save.
    gs.slot.chars[0].colour_bit = 0x08;
    rules.tables = &tables;
    gs.rules = &rules;
    gs.slot.set_party({0, 1, 0xFF});
    gs.slot.set_recruited(0, true);
    gs.slot.set_recruited(1, true);
    // Unlocked: row 0 columns 2..5, row 1 columns 2..3, row 2 column 2.
    gs.slot.chars[0].unlocked[0] = 0x3C;
    gs.slot.chars[0].unlocked[1] = 0x0C;
    gs.slot.chars[0].unlocked[2] = 0x04;
    gs.slot.chars[0].max_hp = 100;
    gs.slot.chars[0].cur_hp = 40;
    gs.slot.chars[0].atk = 10;
    gs.slot.chars[0].hit = 30;
    gs.slot.chars[0].def = 5;
    // 1 lv1 any row, 2 lv3 rows 2..4 only, 3 consumable stack lv1 (own row only), 4 fixed, 5 innate-colour-only (white),
    // 6 not for character 0, 7 lv2 with an auto-fill bonus.
    el(1, 1, 0x80, 0x77);
    el(2, 3, 0x80, 0x11);
    el(3, 1, 0x04, 0x00, 0x14);
    el(4, 1, 0x08, 0x77, 0x01, 0x02);
    el(5, 1, 0x08, 0x77, 0x01, 0, 0x80);
    el(6, 1, 0x20, 0x77);
    // Character 0 is excluded from element 6.
    rules.pg[6].raw[0] = 0xFE;
    el(7, 2, 0x10, 0x66);
    rules.pg[7].raw[0xC] = 5;
  }
  void stock(int id, int qty) {
    for (auto& stack : gs.slot.elements)
      if (stack.id == 0) {
        stack.id = static_cast<u16>(id);
        stack.qty = static_cast<u8>(qty);
        return;
      }
  }
};

void test_grid() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  // Ordinal mapping over the set mask bits.
  CHECK(game_state.cell_index(0, 0, 2) == 0);
  CHECK(game_state.cell_index(0, 0, 0) == -1);
  CHECK(game_state.cell_index(0, 1, 2) == 8);
  CHECK(game_state.cell_index(0, 4, 4) == 31);
  CHECK(game_state.cell_index(0, 7, 8) == 47);
  CHECK(game_state.first_open_row(0) == 0);
  CHECK(game_state.row_has_open_cell(0, 2) && !game_state.row_has_open_cell(0, 3));
  fixture.stock(1, 2);
  fixture.stock(2, 1);
  fixture.stock(3, 7);
  fixture.stock(4, 1);
  fixture.stock(5, 1);
  fixture.stock(6, 1);
  CHECK(game_state.can_place(0, 0, 0, 1) == Why::NoCell);
  // The cell exists but is not unlocked.
  CHECK(game_state.can_place(0, 0, 6, 1) == Why::Locked);
  // None owned.
  CHECK(game_state.can_place(0, 0, 2, 7) == Why::NoStock);
  // The element's eligibility table excludes the character.
  CHECK(game_state.can_place(0, 0, 2, 6) == Why::NotEligible);
  CHECK(game_state.can_place(0, 0, 2, 0x90) == Why::NotEligible);
  // Level 3 with window 0x11: rows 2..4 only.
  CHECK(game_state.can_place(0, 0, 2, 2) == Why::Window);
  CHECK(game_state.can_place(0, 1, 2, 2) == Why::Ok);
  CHECK(game_state.can_place(0, 2, 2, 2) == Why::Ok);
  // The stack goes in its own row; window 0 means exactly its level.
  CHECK(game_state.can_place(0, 0, 2, 3) == Why::Ok);
  CHECK(game_state.can_place(0, 1, 2, 3) == Why::Window);
  // Innate-only: white matches character 0.
  CHECK(game_state.can_place(0, 0, 2, 5) == Why::Ok);
  fixture.tables.char_sets["char"].rec[1].raw[1] = 0x20;
  game_state.slot.chars[1].unlocked[0] = 0x3C;
  // Character 1 is red.
  CHECK(game_state.can_place(1, 0, 2, 5) == Why::NotEligible);

  // Place a plain element: the stock drops; replacing returns the old copy.
  CHECK(game_state.place(0, 0, 2, 1) == Why::Ok);
  CHECK(game_state.cell_element(0, 0, 2) == 1 && game_state.element_count(1) == 1);
  CHECK(game_state.place(0, 0, 2, 5) == Why::Ok);
  CHECK(game_state.cell_element(0, 0, 2) == 5 && game_state.element_count(1) == 2 && game_state.element_count(5) == 0);
  CHECK(game_state.remove(0, 0, 2) == Why::Ok && game_state.element_count(5) == 1 && game_state.cell_element(0, 0, 2) == 0);
  CHECK(game_state.remove(0, 0, 2) == Why::Empty);

  // A stack takes up to 5 and stores the count; removing returns them all.
  CHECK(game_state.place(0, 0, 3, 3) == Why::Ok);
  CHECK(game_state.element_count(3) == 2 && game_state.cell_count(0, 0, 3) == 5);
  CHECK(game_state.remove(0, 0, 3) == Why::Ok && game_state.element_count(3) == 7);

  // Fixed (flag 0x200) and tech (>= 0x80) occupants can be neither replaced nor removed.
  // Cell 2 = row 0 column 4.
  game_state.slot.chars[0].grid[2] = 4;
  CHECK(game_state.can_remove(0, 0, 4) == Why::Fixed);
  CHECK(game_state.can_place(0, 0, 4, 1) == Why::Fixed);
  game_state.slot.chars[0].grid[2] = 0xA0;
  CHECK(game_state.can_remove(0, 0, 4) == Why::Fixed);
  game_state.slot.chars[0].grid[2] = 0;

  // A tech held in the stock goes to its owner's grid like an element, then stays fixed.
  fixture.el(0x85, 1, 0x08, 0x00, 0x01, 0x02);
  fixture.rules.pg[0x85].raw.fill(0);
  fixture.rules.pg[0x85].raw[0] = 0x01;
  fixture.stock(0x85, 1);
  CHECK(game_state.can_place(1, 0, 2, 0x85) == Why::NotEligible);
  CHECK(game_state.place(0, 0, 4, 0x85) == Why::Ok);
  CHECK(game_state.cell_element(0, 0, 4) == 0x85 && game_state.element_count(0x85) == 0);
  CHECK(game_state.can_remove(0, 0, 4) == Why::Fixed);
  game_state.slot.chars[0].grid[2] = 0;

  // Swap: the window of both elements is checked at the new rows.
  CHECK(game_state.place(0, 0, 2, 1) == Why::Ok);
  CHECK(game_state.place(0, 1, 2, 2) == Why::Ok);
  // Element 2 does not work in row 0.
  CHECK(game_state.swap(0, 0, 2, 1, 2) == Why::Window);
  CHECK(game_state.cell_element(0, 0, 2) == 1 && game_state.cell_element(0, 1, 2) == 2);
  fixture.stock(7, 1);
  CHECK(game_state.place(0, 0, 3, 7) == Why::Ok);
  CHECK(game_state.swap(0, 0, 2, 0, 3) == Why::Ok);
  CHECK(game_state.cell_element(0, 0, 2) == 7 && game_state.cell_element(0, 0, 3) == 1);
  CHECK(game_state.swap(0, 0, 2, 0, 2) == Why::Same);

  // Clear all returns everything; techs stay.
  CHECK(game_state.place(0, 0, 4, 3) == Why::Ok);
  // A tech in row 0 column 5; clearing returns 1, 2, 7 and the stack.
  game_state.slot.chars[0].grid[3] = 0xA1;
  CHECK(game_state.clear_all(0) == 4);
  CHECK(game_state.element_count(1) == 2 && game_state.element_count(2) == 1 && game_state.element_count(3) == 7 && game_state.element_count(7) == 1);
  CHECK(game_state.slot.chars[0].grid[3] == 0xA1);

  // Placing takes the new copies before the occupant goes back: a stack refilled with itself holds what the stock had (5 placed,
  // 2 left -> the cell keeps 2, the stock gets the 5 back), and replacing an element with itself at 99 in stock stays at 99.
  CHECK(game_state.place(0, 0, 2, 3) == Why::Ok && game_state.cell_count(0, 0, 2) == 5 && game_state.element_count(3) == 2);
  CHECK(game_state.place(0, 0, 2, 3) == Why::Ok && game_state.cell_count(0, 0, 2) == 2 && game_state.element_count(3) == 5);
  CHECK(game_state.place(0, 0, 3, 1) == Why::Ok && game_state.element_count(1) == 1);
  for (auto& stack : game_state.slot.elements)
    if (stack.id == 1) stack.qty = 99;
  CHECK(game_state.place(0, 0, 3, 1) == Why::Ok && game_state.element_count(1) == 99);
  // With no stack of the occupant and no free slot the occupant cannot go back: refused, nothing changes.
  for (auto& stack : game_state.slot.elements) {
    if (stack.id == 1) stack.id = 0x7E;
    if (stack.id == 0) stack.id = 0x7E, stack.qty = 1;
  }
  CHECK(game_state.can_remove(0, 0, 3) == Why::Full && game_state.remove(0, 0, 3) == Why::Full && game_state.cell_element(0, 0, 3) == 1);
  CHECK(game_state.place(0, 0, 3, 7) == Why::Full && game_state.cell_element(0, 0, 3) == 1 && game_state.element_count(7) == 1);
  // A stack cell that holds 0 copies returns none.
  const int stock_before = game_state.element_count(3);
  game_state.slot.chars[0].grid[2] = 3;
  game_state.slot.chars[0].equipped_qty[2] = 0;
  CHECK(game_state.remove(0, 0, 4) == Why::Ok && game_state.cell_element(0, 0, 4) == 0 && game_state.element_count(3) == stock_before);
}

void test_autofill() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  fixture.stock(1, 3);
  fixture.stock(7, 2);
  fixture.stock(2, 1);
  const int placed = game_state.auto_fill(0, true);
  // Exact levels: level 3 -> row 2, level 2 -> row 1 (two copies), level 1 -> row 0 (at most two copies per element).
  CHECK(placed == 5);
  CHECK(game_state.cell_element(0, 2, 2) == 2);
  CHECK(game_state.cell_element(0, 1, 2) == 7 && game_state.cell_element(0, 1, 3) == 7);
  CHECK(game_state.copies_placed(0, 1) == 2 && game_state.copies_placed(0, 7) == 2);
  for (int row = 0; row < 8; ++row)
    for (int column = 0; column < 10; ++column)
      // Locked cells are never filled.
      if (game_state.cell_element(0, row, column)) CHECK(game_state.cell_available(0, row, column));
  CHECK(game_state.element_count(1) == 1 && game_state.element_count(7) == 0 && game_state.element_count(2) == 0);
  // Fill-empty keeps what is there and adds nothing without stock.
  CHECK(game_state.auto_fill(0, false) == 0);
  CHECK(game_state.cell_element(0, 2, 2) == 2);
  // The recommended layout resets first.
  CHECK(game_state.auto_fill(0, true) == 5);
}

void test_autofill_order() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  // Three level-1 elements in stock order 1, 0x10, 0x11; only 0x11 has a bonus. The exchange sort moves 0x11 to the front by swapping
  // it with 1, so after it is placed 0x10 comes before 1.
  fixture.el(0x10, 1, 0x80, 0x77);
  fixture.el(0x11, 1, 0x80, 0x77);
  for (int id : {1, 0x10, 0x11}) fixture.rules.pg[static_cast<size_t>(id)].raw[0xC] = fixture.rules.pg[static_cast<size_t>(id)].raw[0xD] = 0;
  fixture.rules.pg[0x11].raw[0xC] = 5;
  fixture.stock(1, 1);
  fixture.stock(0x10, 1);
  fixture.stock(0x11, 1);
  CHECK(game_state.auto_fill(0, true) == 3);
  CHECK(game_state.cell_element(0, 0, 2) == 0x11 && game_state.cell_element(0, 0, 3) == 0x10 && game_state.cell_element(0, 0, 4) == 1);

  // Party: member 0 has only a level-2 cell, member 1 only a level-1 cell, one level-1 element. The exact-level pass runs for every
  // member before the fallback pass, so member 1's own row wins it; member 0 alone takes it into its level-2 cell by the fallback.
  Fixture party_fixture;
  GameState& party_state = party_fixture.gs;
  party_state.slot.chars[0].unlocked = {0, 0x04, 0, 0, 0, 0, 0, 0};
  party_state.slot.chars[1].unlocked = {0x04, 0, 0, 0, 0, 0, 0, 0};
  party_fixture.stock(1, 1);
  CHECK(party_state.auto_fill_party(true) == 1);
  CHECK(party_state.cell_element(1, 0, 2) == 1 && party_state.cell_element(0, 1, 2) == 0);
  party_state.clear_all(1);
  CHECK(party_state.auto_fill(0, false) == 1 && party_state.cell_element(0, 1, 2) == 1);
}

void test_sort() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  auto ids = [&] {
    std::vector<int> result;
    for (const auto& stack : game_state.slot.elements)
      if (stack.id) result.push_back(stack.id);
    return result;
  };
  for (int id : {0x80, 0x10, 0x03, 0x60, 0x35, 0x70, 0x55, 0x6C, 0x02, 0x40}) fixture.stock(id, 1);
  game_state.sort_elements(SortMode::AttackFirst, 0);
  CHECK((ids() == std::vector<int>{0x10, 0x03, 0x02, 0x35, 0x40, 0x55, 0x60, 0x6C, 0x70, 0x80}));
  game_state.sort_elements(SortMode::HealFirst, 0);
  // 0x80, 0x55..0x6B ascending, 0x31..0x54 ascending, 0x01..0x30 descending, 0x6C..0x7D descending.
  CHECK((ids() == std::vector<int>{0x80, 0x55, 0x60, 0x35, 0x40, 0x10, 0x03, 0x02, 0x70, 0x6C}));
  // Empty slots sink to the end.
  CHECK(game_state.slot.elements[10].id == 0);
  // Innate-first: character 0 is white (0x08), its opposite black (0x10). White first by id; then the rest through the exchange sort
  // over the stock with the white and black slots emptied (slots 2, 5, 6, 7), which leaves 0x6C and 0x70 ahead of the lower ids;
  // black last in stock order.
  fixture.el(0x10, 1, 0x08, 0x77);
  fixture.el(3, 1, 0x08, 0x77);
  fixture.el(2, 1, 0x10, 0x77);
  fixture.el(0x60, 1, 0x10, 0x77);
  game_state.sort_elements(SortMode::InnateFirst, 0);
  CHECK((ids() == std::vector<int>{3, 0x10, 0x6C, 0x70, 0x35, 0x40, 0x55, 0x80, 0x60, 2}));
}

// Story tech learning (FE 79 / 74 / 73): the cell closing star level 3 / 5 / 7 over the first 8 columns of that row.
void test_learn_tech() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  const int before = game_state.element_count(1);
  game_state.slot.chars[0].grid[21] = 0x4001;
  CHECK(game_state.learn_tech(0, 3, 0x98) == 21);
  CHECK(game_state.slot.chars[0].grid[21] == 0x4098);
  CHECK(game_state.element_count(1) == before + 1);
  CHECK(game_state.learn_tech(0, 5, 0x99) == 34 && game_state.slot.chars[0].grid[34] == 0x99);
  CHECK(game_state.learn_tech(0, 7, 0x9A) == 42 && game_state.slot.chars[0].grid[42] == 0x9A);
  // A tech replacing a tech returns nothing to the stock.
  CHECK(game_state.learn_tech(0, 7, 0x9B) == 42 && game_state.element_count(0x9A) == 0);
  CHECK(game_state.learn_tech(2, 3, 0x98) == -1);
}

void test_equip() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  fixture.tables.item_info.resize(256);
  fixture.tables.gear.resize(256);
  auto item = [&](int id, int kind, u32 mask_lo) {
    fixture.tables.item_info[static_cast<size_t>(id)].cat = static_cast<u8>(kind);
    fixture.tables.item_info[static_cast<size_t>(id)].equip_lo = mask_lo;
  };
  item(1, 4, 0xFFFF);
  // Item 2 is a material no character can wear.
  item(2, 4, 0);
  // Item 32 is a weapon for character 0, item 33 one for character 1 only.
  item(32, 0, 0x1);
  item(33, 0, 0x2);
  item(150, 1, 0x3);
  item(194, 2, 0x3);
  item(195, 2, 0x3);
  item(196, 2, 0x3);
  fixture.tables.gear[32].raw[4] = 20;
  fixture.tables.gear[32].raw[6] = 9;
  fixture.tables.gear[150].raw[4] = 7;
  // Item 194 is an offence accessory (adds byte 4 to ATK) in conflict group 1; item 195 is in the same group.
  fixture.tables.gear[194].raw[8] = 0x20;
  fixture.tables.gear[194].raw[4] = 4;
  fixture.tables.gear[194].raw[10] = 0x01;
  fixture.tables.gear[195].raw[10] = 0x01;
  fixture.tables.gear[196].raw[8] = 0x40;
  fixture.tables.gear[196].raw[4] = 3;
  for (int id : {1, 32, 33, 150, 2, 194, 195, 196}) game_state.add_item(id);

  auto cands = game_state.equip_candidates(0, 0);
  // The materials and the armour are never weapon candidates; character 0 cannot use item 33; the list ends with the reserved row.
  CHECK(cands.size() == 3);
  CHECK(cands[0].item == 32 && cands[0].ok);
  CHECK(cands[1].item == 33 && !cands[1].ok);
  CHECK(cands[2].stock_slot == -1 && cands[2].item == 0 && !cands[2].ok);
  // An accessory list opens with the reserved row (not selectable on an empty slot), then the accessories, then every material, each
  // in bag order; a material is selectable only through its equippable-by mask.
  cands = game_state.equip_candidates(0, 2);
  CHECK(cands.size() == 6);
  CHECK(cands[0].stock_slot == -1 && cands[0].item == 0 && !cands[0].ok);
  CHECK(cands[1].item == 194 && cands[2].item == 195 && cands[3].item == 196);
  CHECK(cands[4].item == 1 && cands[4].ok && cands[5].item == 2 && !cands[5].ok);
  CHECK(!game_state.can_equip(0, 2, 2));
  CHECK(!game_state.can_equip(0, 0, 33));
  CHECK(!game_state.can_equip(0, 0, 150));

  CHECK(game_state.stats(0).atk == 10 && game_state.stats(0).def == 5);
  CHECK(game_state.equip(0, 0, 32));
  CHECK(game_state.equipped(0, 0) == 32 && game_state.item_count(32) == 0);
  CHECK(game_state.stats(0).atk == 30);
  CHECK(game_state.stats(0).hit == (2 * 30) / 3 + 9 / 3);
  CHECK(game_state.equip(0, 1, 150) && game_state.stats(0).def == 12);
  // A preview changes nothing.
  CHECK(game_state.stats_with(0, 0, 0).atk == 10 && game_state.equipped(0, 0) == 32);
  // Accessories: the offence class adds byte 4 to ATK; the same conflict group is refused.
  CHECK(game_state.equip(0, 2, 194) && game_state.stats(0).atk == 34);
  bool saw195 = false, ok195 = true;
  for (const auto& candidate : game_state.equip_candidates(0, 3))
    if (candidate.item == 195) saw195 = true, ok195 = candidate.ok;
  CHECK(saw195 && !ok195);
  CHECK(!game_state.can_equip(0, 3, 195));
  CHECK(game_state.can_equip(0, 3, 196));
  // The reserved row leads every accessory list and is selectable only on a filled slot; weapon / armour lists end with it.
  CHECK(game_state.equip_candidates(0, 2).front().stock_slot == -1 && game_state.equip_candidates(0, 2).front().ok);
  CHECK(game_state.equip_candidates(0, 4).front().stock_slot == -1 && !game_state.equip_candidates(0, 4).front().ok);
  CHECK(game_state.equip_candidates(0, 0).front().stock_slot != -1 && game_state.equip_candidates(0, 1).back().stock_slot == -1);
  CHECK(!game_state.can_equip(0, 0, 0) && !game_state.can_equip(0, 1, 0));
  game_state.unequip_accessories(0);
  CHECK(game_state.equipped(0, 2) == 0 && game_state.item_count(194) == 1);
  // A candidate overlapping the accessory it replaces skips the conflict test, even when another worn accessory shares its mask.
  item(198, 2, 0x3);
  fixture.tables.gear[198].raw[10] = 0x01;
  game_state.add_item(198);
  game_state.slot.chars[0].accessory = {194, 195, 0};
  CHECK(game_state.can_equip(0, 2, 198) && !game_state.can_equip(0, 4, 198));
  game_state.slot.chars[0].accessory = {0, 0, 0};
  game_state.take_item(198);
  // Stamina recovery (status screen "STAMINA RECOVERY"): record +1F plus the MAG byte of stamina accessories.
  fixture.tables.char_sets["char"].rec[0].raw[0x1F] = 10;
  CHECK(game_state.stamina_recovery(0) == 10);
  item(197, 2, 0x3);
  fixture.tables.gear[197].raw[8] = static_cast<u8>(data::gearflag::kStamina & 0xFF);
  fixture.tables.gear[197].raw[9] = static_cast<u8>(data::gearflag::kStamina >> 8);
  fixture.tables.gear[197].raw[5] = 5;
  game_state.add_item(197);
  CHECK(game_state.equip(0, 3, 197) && game_state.stamina_recovery(0) == 15);
  game_state.unequip_accessories(0);
  CHECK(game_state.stamina_recovery(0) == 10);
  // Swapping weapons returns the old one to the stock.
  game_state.add_item(32);
  CHECK(game_state.equip(0, 0, 32) && game_state.item_count(32) == 1);
  // The replaced piece needs its stack or a free slot: with neither the change is refused and nothing moves.
  item(34, 0, 0x1);
  game_state.take_item(32);
  game_state.add_item(34);
  game_state.add_item(34);
  for (auto& stack : game_state.slot.items)
    if (stack.id == 0) stack.id = 0x1FF, stack.qty = 1;
  CHECK(!game_state.equip(0, 0, 34) && game_state.equipped(0, 0) == 32 && game_state.item_count(34) == 2 && game_state.item_count(32) == 0);
  // Glenn: the second Einlanzer (0x40) sets the two-sword model 0x32 and the two-sword techs 0x9A / 0x9B; any other weapon sets 0x13 and
  // 0xDA / 0xDB. The cell's 0x4000 bit is kept.
  for (auto& stack : game_state.slot.items)
    if (stack.id == 0x1FF) stack = {};
  item(0x3F, 0, 1u << 0x13);
  item(0x40, 0, 1u << 0x13);
  game_state.add_item(0x3F);
  game_state.add_item(0x40);
  save::SaveChar& glenn = game_state.slot.chars[0x13];
  glenn.model_id = 0x13;
  glenn.grid[3] = 0x4000 | 0xDA;
  glenn.grid[9] = 0xDB;
  CHECK(game_state.equip(0x13, 0, 0x40));
  CHECK(glenn.model_id == 0x32 && glenn.grid[3] == (0x4000 | 0x9A) && glenn.grid[9] == 0x9B);
  CHECK(game_state.equip(0x13, 0, 0x3F));
  CHECK(glenn.model_id == 0x13 && glenn.grid[3] == (0x4000 | 0xDA) && glenn.grid[9] == 0xDB);
  // Another character's weapon change leaves the record's model byte alone.
  game_state.slot.chars[0].model_id = 0x31;
  game_state.add_item(32);
  CHECK(game_state.equip(0, 0, 32) && game_state.slot.chars[0].model_id == 0x31);
}

void test_use_and_options() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  // Usable heal, size 0 = +50.
  fixture.rules.pg[7].raw[0xB] = 0xC0;
  fixture.rules.pg[7].raw[0xA] = 0x00;
  fixture.stock(7, 1);
  CHECK(game_state.usable_elements().size() == 1);
  CHECK(game_state.use_element(7, 0) == Why::Ok && game_state.slot.chars[0].cur_hp == 90 && game_state.element_count(7) == 0);
  fixture.stock(7, 1);
  game_state.slot.chars[0].cur_hp = 100;
  CHECK(game_state.use_element(7, 0) == Why::Full && game_state.element_count(7) == 1);
  // The target must be recruited and not excluded: an unrecruited, an excluded or an out-of-range character is refused and nothing is consumed.
  game_state.slot.chars[1].cur_hp = 10;
  game_state.slot.chars[1].max_hp = 100;
  game_state.slot.chars[5].cur_hp = 10;
  game_state.slot.chars[5].max_hp = 100;
  CHECK(game_state.use_element(7, 5) == Why::NotEligible && game_state.use_element(7, -1) == Why::NotEligible && game_state.element_count(7) == 1);
  game_state.slot.unk_e1b8[2] |= 1u << 1;
  CHECK(game_state.use_element(7, 1) == Why::NotEligible && game_state.slot.chars[1].cur_hp == 10 && game_state.element_count(7) == 1);
  game_state.slot.unk_e1b8[2] &= ~(1u << 1);
  CHECK(game_state.use_element(7, 1) == Why::Ok && game_state.slot.chars[1].cur_hp == 60 && game_state.element_count(7) == 0);
  game_state.set_option(opt::kAutoAttack, true);
  game_state.set_option(opt::kAnalogStick, true);
  CHECK(game_state.option(opt::kAutoAttack) && game_state.option(opt::kAnalogStick) && !game_state.option(opt::kMono));
  game_state.set_option(opt::kAutoAttack, false);
  CHECK(!game_state.option(opt::kAutoAttack) && game_state.option(opt::kAnalogStick));
  CHECK(game_state.party().size() == 2 && game_state.roster().size() == 2);
  // The skin byte travels in variable 0x44: kept with Window Frame Custom, 0 with Default, clamped on a load.
  game_state.set_option(opt::kCustomWindow, true);
  game_state.skin = 6;
  game_state.store_window_frame();
  CHECK(game_state.slot.var(0x44) == 6);
  game_state.skin = 0;
  game_state.load_window_frame();
  CHECK(game_state.skin == 6);
  game_state.slot.set_var(0x44, 40);
  game_state.load_window_frame();
  CHECK(game_state.skin == 14 && game_state.slot.var(0x44) == 14);
  game_state.set_option(opt::kCustomWindow, false);
  game_state.store_window_frame();
  CHECK(game_state.skin == 0 && game_state.slot.var(0x44) == 0);
}

// Original-art frame pieces (ps1::frame_pieces) and the dial needle angle (hand.h).
void test_ps1_art() {
  struct Piece {
    float dx, dy, dw, dh, sx, sy, sw, sh;
  };
  std::vector<Piece> pieces;
  ps1::frame_pieces([&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) { pieces.push_back({dx, dy, dw, dh, sx, sy, sw, sh}); }, 10, 20, 100, 60, 2, 2);
  // Top, bottom, left, right edges + 4 corners.
  CHECK(pieces.size() == 8);
  float top_w = 0;
  for (const Piece& piece : pieces)
    if (piece.dy == 20 && piece.dh == 16 && piece.sx == 31) top_w += piece.dw;
  CHECK(std::fabs(top_w - (100 - 2 * 16)) < 0.01f);
  pieces.clear();
  ps1::frame_pieces([&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) { pieces.push_back({dx, dy, dw, dh, sx, sy, sw, sh}); }, 10, 20, 100, 60, 2, 2, 30, 40);
  // The tab splits the top edge in two.
  CHECK(pieces.size() == 9);
  float gap_w = 0;
  for (const Piece& piece : pieces)
    if (piece.dy == 20 && piece.dh == 16 && piece.sx == 31) gap_w += piece.dw;
  CHECK(std::fabs(gap_w - (100 - 2 * 16 - 40)) < 0.01f);
  // needle: the tip points along the requested direction for every angle
  for (int angle = -2048; angle < 2048; angle += 97) {
    const float th = (static_cast<float>(angle) + 1024.0f) * 3.14159265f / 2048.0f;
    // The canvas position of the tip.
    const float dx = -std::cos(th) * 2.5f * 76.0f, dy = -std::sin(th) * 2.2222f * 76.0f;
    int toward = ui::hand_angle_toward(dx, dy, 2.5f, 2.2222f);
    CHECK(std::abs(toward - angle) <= 1 || std::abs(std::abs(toward - angle) - 4096) <= 1);
  }
  // Caption notch table (the main menu / shop draw the disc's wind pieces in classic art, with the same gaps as the RDE shapes).
  // The element list frame's bottom edge is open for the Reorder prompt.
  CHECK(ps1::bottom_gap(53).start == 130 && ps1::bottom_gap(53).end == 228 && ps1::bottom_gap(56).end == 0);
  CHECK(ps1::caption_gap_end(49) == 79 && ps1::caption_gap_end(64) == 61 && ps1::caption_gap_end(82) == 48 && ps1::caption_gap_end(7) == 0);
  CHECK(std::string(ps1::face_name(0)) == "seljyu" && std::string(ps1::face_name(1)) == "kid" && ps1::face_name(50) == nullptr);
}

// The Status camera drawn between two model ticks: the previous tick's camera at 0, the current one at 1, linear in between; the turn
// carries on across the wrap of 4096 instead of spinning back.
void test_menu_camera_blend() {
  MenuCamera prev, cur;
  prev.yaw = 4092, prev.pitch = -16, prev.distance = 2000, prev.pan_x = 8, prev.pan_y = -8;
  cur.yaw = 4100, cur.pitch = -32, cur.distance = 2032, cur.pan_x = 16, cur.pan_y = -16;
  const MenuCameraNow start = menu_camera_at(prev, cur, 0.0f), half = menu_camera_at(prev, cur, 0.5f), end = menu_camera_at(prev, cur, 1.0f);
  CHECK(start.yaw == 4092.0f && start.distance == 2000.0f && start.pan_x == 8.0f && start.pan_y == -8.0f);
  CHECK(half.yaw == 4096.0f && half.distance == 2016.0f && half.pan_x == 12.0f && half.pan_y == -12.0f);
  CHECK(end.yaw == 4100.0f && end.distance == 2032.0f);
  CHECK(std::fmod(start.pitch, 4096.0f) == 4080.0f && end.pitch - start.pitch == -16.0f);
  const MenuCameraNow still = menu_camera_at(cur, cur, 0.7f);
  CHECK(still.yaw == static_cast<float>(cur.yaw & 0xFFF) && still.distance == 2032.0f);
}

void test_localized_path() {
  vfs::Vfs files;
  vfs::Overlay overlay;
  const std::filesystem::path none;
  overlay.files["hd/battle/btend/up.png"] = {none, ""};
  overlay.files["hd/battle/btend/up_en.png"] = {none, ""};
  overlay.files["hd/menu/shapes/memmenu.png"] = {none, ""};
  overlay.files["hd/menu/shapes/memmenu_fr.png"] = {none, ""};
  files.set_overlay(std::move(overlay));
  CHECK(localized_path(files, "hd/battle/btend/up.png", import::Lang::En) == "hd/battle/btend/up_en.png");
  CHECK(localized_path(files, "hd/battle/btend/up.png", import::Lang::De) == "hd/battle/btend/up.png");
  CHECK(localized_path(files, "hd/menu/shapes/memmenu.png", import::Lang::Fr) == "hd/menu/shapes/memmenu_fr.png");
  CHECK(localized_path(files, "hd/menu/shapes/memmenu.png", import::Lang::Jp) == "hd/menu/shapes/memmenu.png");
  CHECK(localized_path(files, "hd/menu/shapes/memmenu2.png", import::Lang::Fr) == "hd/menu/shapes/memmenu2.png");
}
}  // namespace

// The Use screen's state machine: the party cursor clamps, Left opens a roster of all 44
// characters whose cursor character stands in for slot 0, a refusal changes nothing, a use with copies left returns to the same target after
// 6 ticks, the last copy returns to the list with the cursor kept, and the cursor may rest on a blank list row.
void test_use_flow() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  fixture.rules.pg[7].raw[0xB] = 0xC0;
  fixture.rules.pg[7].raw[0xA] = 0x00;
  fixture.tables.elements[8].raw[0x18] = 0x08;
  fixture.el(8, 1, 0x04, 0x00);
  fixture.rules.pg[8].raw[0xB] = 0x80;
  fixture.stock(7, 3);
  fixture.stock(8, 1);
  game_state.slot.set_party({0, 1, 2});
  const int hp[5] = {5, 100, 30, 10, 95}, max_hp[5] = {100, 100, 100, 90, 90};
  for (int character = 0; character < 5; ++character) {
    game_state.slot.set_recruited(character, true);
    game_state.slot.chars[static_cast<size_t>(character)].cur_hp = static_cast<u16>(hp[character]);
    game_state.slot.chars[static_cast<size_t>(character)].max_hp = static_cast<u16>(max_hp[character]);
  }
  game_state.slot.chars[1].status = 0x08;
  UseFlow flow;
  UseFlow::Result last;
  auto tap = [&](UseFlow::Input input) {
    last = flow.step(input, game_state, 11);
    const int sfx = last.sfx;
    while (flow.wait > 0) flow.step({}, game_state, 11);
    last.sfx = sfx;
  };
  const UseFlow::Input up{true}, down{false, true}, left{false, false, true}, right{false, false, false, true}, ok{false, false, false, false, true},
      cancel{false, false, false, false, false, true};

  last = flow.step(ok, game_state, 11);
  CHECK(!flow.target && flow.wait == 3 && last.sfx == 1);
  flow.step({}, game_state, 11);
  flow.step({}, game_state, 11);
  CHECK(!flow.target);
  flow.step({}, game_state, 11);
  CHECK(flow.target && !flow.roster && flow.target_cur == 0);
  tap(up);
  CHECK(flow.target_cur == 0 && last.sfx == 1);
  for (int i = 0; i < 4; ++i) tap(down);
  CHECK(flow.target_cur == 2 && last.sfx == 1);
  tap(left);
  CHECK(flow.roster && flow.stand_in == 0 && flow.roster_row == 0 && flow.roster_top == 0);
  for (int i = 0; i < 14; ++i) tap(down);
  CHECK(flow.roster_row == 11 && flow.roster_top == 3 && flow.stand_in == 14);
  for (int i = 0; i < 3; ++i) tap(up);
  CHECK(flow.roster_row == 8 && flow.roster_top == 3 && flow.stand_in == 11);
  tap(right);
  CHECK(!flow.roster && flow.target && flow.stand_in == -1 && flow.target_cur == 2);
  tap(left);
  CHECK(flow.roster_row == 0 && flow.roster_top == 0 && flow.stand_in == 0);

  tap(ok);
  CHECK(game_state.slot.chars[0].cur_hp == 55 && game_state.element_count(7) == 2 && last.sfx == 0x12 && last.layer_sfx == 0x13);
  CHECK(flow.roster && flow.stand_in == 0);
  tap(down);
  tap(ok);
  CHECK(game_state.slot.chars[1].cur_hp == 100 && game_state.element_count(7) == 2 && last.sfx == 0x18 && flow.roster && flow.stand_in == 1);
  tap(down);
  tap(down);
  tap(ok);
  CHECK(game_state.slot.chars[3].cur_hp == 60 && game_state.element_count(7) == 1 && flow.roster && flow.stand_in == 3);
  for (int i = 0; i < 8; ++i) tap(down);
  CHECK(flow.roster_row == 11 && flow.stand_in == 11);
  tap(ok);
  CHECK(last.sfx == 0x18 && game_state.element_count(7) == 1 && flow.roster);
  tap(cancel);
  CHECK(!flow.roster && flow.target && flow.stand_in == -1 && last.sfx == 6);
  tap(cancel);
  CHECK(!flow.target && !last.leave);

  tap(ok);
  CHECK(flow.target && flow.target_cur == 2);
  tap(up);
  tap(up);
  tap(ok);
  CHECK(game_state.slot.chars[0].cur_hp == 100 && game_state.element_count(7) == 0 && !flow.target && flow.wait == 0 && flow.list_cur == 0);
  CHECK(game_state.usable_elements().size() == 1);
  tap(ok);
  tap(down);
  tap(ok);
  CHECK(game_state.slot.chars[1].status == 0 && game_state.element_count(8) == 0 && !flow.target && flow.list_cur == 0);
  CHECK(last.sfx == 0xBE && last.layer_sfx == 0);
  CHECK(game_state.usable_elements().empty());
  tap(ok);
  CHECK(!flow.target && last.sfx == 0);
  for (int i = 0; i < 12; ++i) tap(down);
  CHECK(flow.list_cur == 10 && flow.list_scroll == 0 && last.sfx == 1);
  tap(cancel);
  CHECK(last.leave && last.sfx == 6);
}

int main() {
  test_localized_path();
  test_ps1_art();
  test_menu_camera_blend();
  test_grid();
  test_autofill();
  test_autofill_order();
  test_sort();
  test_learn_tech();
  test_equip();
  test_use_and_options();
  test_use_flow();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("menu tests passed\n");
  return 0;
}
