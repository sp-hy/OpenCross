// Shop rule tests on synthetic tables: element prices / sell rule, forge cost and consumption, breakdown (incl. overflow gold),
// gear sale and the trading post level sums with the colour mask.
#include <cstdio>

#include "ui/shop/shop_logic.h"

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
  shop::ShopData sd;

  void el(int id, int level, int colour, int price) {
    auto& element = tables.elements[static_cast<size_t>(id)];
    element.raw[0] = 0x01;
    element.raw[5] = static_cast<u8>(colour);
    element.raw[6] = static_cast<u8>(level);
    auto& rule = rules.pg[static_cast<size_t>(id)];
    rule.raw.fill(0);
    rule.raw[8] = static_cast<u8>(price & 0xFF);
    rule.raw[9] = static_cast<u8>(price >> 8);
  }
  void item(int id, int kind, int price10, int extra = 0) {
    tables.item_info[static_cast<size_t>(id)].price10 = static_cast<s16>(price10);
    tables.item_info[static_cast<size_t>(id)].cat = static_cast<u8>(kind);
    tables.item_info[static_cast<size_t>(id)].extra = static_cast<u8>(extra);
  }
  void give(int id, int count) {
    for (int i = 0; i < count; ++i) gs.add_item(id);
  }

  Fixture() {
    tables.elements.resize(0x90);
    rules.pg.resize(0x90);
    tables.item_info.resize(256);
    tables.forge.resize(256);
    rules.tables = &tables;
    gs.rules = &rules;
    gs.slot.set_party({0, 0xFF, 0xFF});
    gs.slot.set_recruited(0, true);
    // elements: 1 lv1 green price 100, 2 lv2 blue price 101, 3 lv3 red price 400 (unsellable), 4 lv1 blue price 50
    el(1, 1, 0x04, 100);
    el(2, 2, 0x80, 101);
    el(3, 3, 0x20, 400);
    rules.pg[3].raw[0xA] = 0x20;
    el(4, 1, 0x80, 50);
    // materials: 1 (tier 1, price 100), 3 (tier 3, price 300), 8 (not a tier, price 40); gear 20 weapon (price 120, recipe), 21 armour
    // with no tier material, 22 unsellable weapon (flag 0x80), 23 unbreakable (flag 0x20)
    item(1, 4, 10);
    item(3, 4, 30);
    item(8, 4, 4);
    item(20, 0, 12);
    item(21, 1, 9);
    item(22, 0, 5, 0x80);
    item(23, 0, 5, 0x20);
    tables.forge[20][1] = 2, tables.forge[20][3] = 1, tables.forge[20][8] = 3;
    tables.forge[21][8] = 1;
    tables.forge[23][1] = 1;
    // shop rows
    sd.el_shop = {{1, 2, 4}};
    // The 0 ends the weapon row.
    sd.waa_shop = {{20, 21, 22, 0, 20}};
    sd.trade = {{{static_cast<u16>(8 | (0x80 << 8)), 5}, {static_cast<u16>(1), 3}, {0, 9}, {7, 0}}};
    gs.set_gold(1000);
  }
};

void test_element_shop() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  CHECK(fixture.sd.element_row(0) == (std::vector<int>{1, 2, 4}));
  CHECK(shop::element_price(game_state, 2) == 101);
  // The sell unit is 101 >> 2; a stack sells for 303 >> 2, not 3 * 25.
  CHECK(shop::element_sell_unit(game_state, 2) == 25);
  CHECK(shop::element_sell_total(game_state, 2, 3) == 75);
  CHECK(!shop::element_sellable(game_state, 3));
  // 1000 gold / 100.
  CHECK(shop::element_buy_max(game_state, 1) == 10);
  CHECK(shop::buy_element(game_state, 1, 3) == shop::Res::Ok);
  CHECK(game_state.gold() == 700 && game_state.element_count(1) == 3);
  CHECK(shop::buy_element(game_state, 3, 5) == shop::Res::NoGold);
  CHECK(game_state.gold() == 700);
  // 96 owned; the cap is 99 - 96.
  for (int i = 0; i < 96; ++i) game_state.add_element(4);
  game_state.set_gold(100000);
  CHECK(shop::element_buy_max(game_state, 4) == 3);
  CHECK(shop::buy_element(game_state, 4, 4) == shop::Res::Full);
  CHECK(shop::buy_element(game_state, 4, 3) == shop::Res::Ok && game_state.element_count(4) == 99);

  // sell: bag copies first, then those placed on a party member (the cell is emptied)
  game_state.set_gold(0);
  CHECK(shop::sell_element(game_state, 1, 2) == shop::Res::Ok);
  CHECK(game_state.element_count(1) == 1 && game_state.gold() == 50);
  CHECK(shop::sell_element(game_state, 1, 2) == shop::Res::NoStock);
  CHECK(shop::sell_element(game_state, 3, 1) == shop::Res::NotSellable);
  const auto list = shop::element_sell_list(game_state);
  CHECK(!list.empty() && list[0] == 1);
  for (int id : list) CHECK(id != 3);
}

// Element Sell past the bag count: the player picks whose placed copies go; the sale takes exactly those and pays (price * total) >> 2.
void test_element_sell_picks() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  fixture.el(5, 1, 0x04, 50);
  fixture.el(6, 1, 0x04, 50);
  fixture.tables.elements[6].raw[0] = 0x11;
  auto& char_set = fixture.tables.char_sets["char"];
  for (int character = 0; character < 3; ++character) {
    char_set.rec[static_cast<size_t>(character)].raw[0x30] = 0xFC;
    char_set.rec[static_cast<size_t>(character)].raw[0x31] = 0x03;
    game_state.slot.chars[static_cast<size_t>(character)].unlocked[0] = 0x3FC;
  }
  game_state.slot.set_recruited(1, true);
  game_state.slot.set_recruited(2, true);
  game_state.slot.chars[0].grid[0] = 5;
  game_state.slot.chars[1].grid[0] = 5;
  game_state.slot.chars[1].grid[1] = 5;
  game_state.slot.chars[1].grid[2] = 5;
  game_state.slot.chars[2].grid[0] = 6;
  game_state.slot.chars[2].equipped_qty[0] = 4;
  game_state.slot.chars[2].grid[1] = 5;
  game_state.add_element(5);
  game_state.add_element(5);
  CHECK(shop::placed_copies(game_state, 0, 5) == 1 && shop::placed_copies(game_state, 1, 5) == 3 && shop::placed_copies(game_state, 2, 5) == 1);
  CHECK(shop::placed_copies(game_state, 2, 6) == 4 && shop::party_copies(game_state, 5) == 5);
  CHECK(shop::element_sell_max(game_state, 5) == 2);
  CHECK(shop::element_sell_list(game_state) == (std::vector<int>{5, 6}));

  game_state.set_gold(0);
  CHECK(shop::sell_element(game_state, 5, 3) == shop::Res::NoStock);
  CHECK(shop::sell_element(game_state, 5, 0, {3}) == shop::Res::NoStock);
  CHECK(shop::sell_element(game_state, 5, 2, {0, 4}) == shop::Res::NoStock);
  CHECK(game_state.gold() == 0 && game_state.element_count(5) == 2 && shop::party_copies(game_state, 5) == 5);
  CHECK(shop::sell_element(game_state, 5, 2, {1, 3}) == shop::Res::Ok);
  CHECK(game_state.gold() == (50 * 6) >> 2 && game_state.element_count(5) == 0);
  CHECK(shop::placed_copies(game_state, 0, 5) == 0 && shop::placed_copies(game_state, 1, 5) == 0 && shop::placed_copies(game_state, 2, 5) == 1);
  CHECK(game_state.slot.chars[0].grid[0] == 0 && game_state.slot.chars[1].grid[2] == 0 && game_state.slot.chars[2].grid[1] == 5);
  CHECK(shop::element_sell_list(game_state) == (std::vector<int>{6, 5}));

  game_state.set_gold(0);
  CHECK(shop::sell_element(game_state, 6, 0, {0, 0, 3}) == shop::Res::Ok);
  CHECK(game_state.slot.chars[2].grid[0] == 6 && game_state.slot.chars[2].equipped_qty[0] == 1 && game_state.gold() == (50 * 3) >> 2);
  CHECK(shop::sell_element(game_state, 6, 0, {0, 0, 1}) == shop::Res::Ok);
  CHECK(game_state.slot.chars[2].grid[0] == 0 && game_state.slot.chars[2].equipped_qty[0] == 0);
  game_state.slot.set_recruited(2, false);
  CHECK(shop::sell_element(game_state, 5, 0, {0, 0, 1}) == shop::Res::NoStock);
}

void test_forge() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  // Weapons only; the row stops at the 0.
  CHECK(shop::forge_list(game_state, fixture.sd, 0, 0) == (std::vector<int>{20, 22}));
  CHECK(shop::forge_list(game_state, fixture.sd, 0, 1) == (std::vector<int>{21}));
  CHECK(shop::forge_list(game_state, fixture.sd, 5, 0).empty());
  // Cheats "Every weapon in shops": own row first, then every other recipe piece of the kind by id (the materials 1 / 3 / 8 are kind 4)
  CHECK(shop::forge_list_all(game_state, fixture.sd, 0, 0) == (std::vector<int>{20, 22, 23}));
  CHECK(shop::forge_list_all(game_state, fixture.sd, 5, 0) == (std::vector<int>{20, 23}));
  CHECK(shop::forge_list_all(game_state, fixture.sd, 5, 1) == (std::vector<int>{21}));
  const auto recipe = shop::recipe(game_state, 20);
  CHECK(recipe.size() == 3 && recipe[0].id == 1 && recipe[0].qty == 2 && recipe[2].id == 8 && recipe[2].qty == 3);
  // sum 6, highest tier material below 7 is 3 (price 300): (300 / 5) * 6
  CHECK(shop::forge_cost(game_state, 20) == 360);
  // No tier material: P = 1000 -> (1000 / 5) * 1.
  CHECK(shop::forge_cost(game_state, 21) == 200);
  CHECK(shop::can_forge(game_state, 20) == shop::Res::ShortMaterials);
  CHECK(shop::can_forge(game_state, 22) == shop::Res::NoRecipe);
  fixture.give(1, 2);
  fixture.give(3, 1);
  fixture.give(8, 3);
  game_state.set_gold(359);
  CHECK(shop::can_forge(game_state, 20) == shop::Res::NoGold);
  game_state.set_gold(400);
  CHECK(shop::forge(game_state, 20) == shop::Res::Ok);
  CHECK(game_state.gold() == 40 && game_state.item_count(20) == 1);
  CHECK(game_state.item_count(1) == 0 && game_state.item_count(3) == 0 && game_state.item_count(8) == 0);
  CHECK(shop::forge(game_state, 20) == shop::Res::ShortMaterials);
  // Cheats "No crafting cost": no materials, no gold, still needs a recipe and room in the stack
  game_state.set_gold(0);
  CHECK(shop::can_forge(game_state, 20, true) == shop::Res::Ok && shop::can_forge(game_state, 22, true) == shop::Res::NoRecipe);
  CHECK(shop::forge(game_state, 20, true) == shop::Res::Ok && game_state.item_count(20) == 2 && game_state.gold() == 0);

  // At 99 held the forge still goes through: materials and gold are taken and the count stays 99.
  fixture.give(20, 97);
  fixture.give(1, 2);
  fixture.give(3, 1);
  fixture.give(8, 3);
  game_state.set_gold(400);
  CHECK(shop::forge(game_state, 20) == shop::Res::Ok && game_state.item_count(20) == 99 && game_state.gold() == 40);
  CHECK(game_state.item_count(1) == 0 && game_state.item_count(3) == 0 && game_state.item_count(8) == 0);
  // With no stack of the piece and no free slot it is refused before anything is taken.
  for (auto& stack : game_state.slot.items)
    if (stack.id == 20 || stack.id == 0) stack.id = 0x1FF, stack.qty = 1;
  CHECK(shop::forge(game_state, 20, true) == shop::Res::Full && game_state.item_count(20) == 0);
}

void test_breakdown_and_sell() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  game_state.set_gold(0);
  fixture.give(20, 1);
  fixture.give(21, 1);
  fixture.give(23, 1);
  // 23 carries flag 0x20, so it is not listed.
  CHECK(shop::breakdown_list(game_state, 0) == (std::vector<int>{20}));
  CHECK(shop::breakdown_list(game_state, 1) == (std::vector<int>{21}));
  int gain = -1;
  CHECK(shop::breakdown(game_state, 20, &gain) == shop::Res::Ok);
  CHECK(gain == 0 && game_state.gold() == 0);
  CHECK(game_state.item_count(20) == 0 && game_state.item_count(1) == 2 && game_state.item_count(3) == 1 && game_state.item_count(8) == 3);
  // Nothing is left in the bag.
  CHECK(shop::breakdown(game_state, 20) == shop::Res::Equipped);
  CHECK(shop::breakdown(game_state, 23) == shop::Res::NotSellable);
  // worn copies are listed but refused
  game_state.slot.chars[0].weapon = 20;
  CHECK(shop::equipped_copies(game_state, 20) == 1);
  CHECK(shop::breakdown_list(game_state, 0) == (std::vector<int>{20}));
  CHECK(shop::breakdown(game_state, 20) == shop::Res::Equipped);
  // overflow past 99 is paid in gold: material 8 (price 40): 2 over -> 2 * 40 / 4 = 20; recipe needs 3 of it
  game_state.slot.chars[0].weapon = 0;
  fixture.give(20, 1);
  // 3 + 96 = 99 already.
  fixture.give(8, 96);
  CHECK(game_state.item_count(8) == 99);
  CHECK(shop::breakdown(game_state, 20, &gain) == shop::Res::Ok);
  // 3 overflow * 40 / 4.
  CHECK(gain == 30 && game_state.gold() == 30);
  CHECK(game_state.item_count(8) == 99);

  // gear sale: (price10 * 10 * qty) >> 2 on the total
  fixture.give(22, 1);
  // Two of 21 (price 90): 180 >> 2 = 45, not 2 * (90 / 4) = 44.
  fixture.give(21, 1);
  CHECK(shop::gear_sell_price(game_state, 21, 1) == 22 && shop::gear_sell_price(game_state, 21, 2) == 45);
  CHECK(shop::gear_sell_list(game_state, 1) == (std::vector<int>{21}));
  CHECK(shop::gear_sell_list(game_state, 0).empty() || shop::gear_sell_list(game_state, 0)[0] != 22);
  const u32 before = game_state.gold();
  CHECK(shop::sell_gear(game_state, 21, 2) == shop::Res::Ok);
  CHECK(game_state.gold() == before + 45 && game_state.item_count(21) == 0);
  CHECK(shop::sell_gear(game_state, 21, 1) == shop::Res::NoStock);
  CHECK(shop::sell_gear(game_state, 22, 1) == shop::Res::NotSellable);
  // Price 10 x 3: 30 >> 2 = 7 (per piece it would be 3 * 2 = 6).
  fixture.item(24, 0, 1);
  fixture.give(24, 3);
  const u32 before_three = game_state.gold();
  CHECK(shop::gear_sell_price(game_state, 24, 3) == 7);
  CHECK(shop::sell_gear(game_state, 24, 3) == shop::Res::Ok && game_state.gold() == before_three + 7);
  // Worn pieces follow the bag in the sell list (unless flag 0x20) and cannot be sold; an unavailable character's are not listed. The
  // bag's 23 (flag 0x20, not 0x80) is sellable.
  game_state.slot.chars[0].weapon = 20;
  game_state.slot.chars[0].armour = 21;
  game_state.slot.set_recruited(5, true);
  game_state.slot.chars[5].weapon = 23;
  game_state.slot.chars[5].accessory[0] = 24;
  CHECK(shop::gear_sell_list(game_state, 0) == (std::vector<int>{23, 20}));
  CHECK(shop::gear_sell_list(game_state, 1) == (std::vector<int>{21}));
  CHECK(shop::sell_gear(game_state, 20, 1) == shop::Res::NoStock);
  game_state.slot.set_recruited(0, false);
  CHECK(shop::gear_sell_list(game_state, 0) == (std::vector<int>{23}) && shop::breakdown_list(game_state, 0).empty());
  game_state.slot.set_recruited(0, true);
  game_state.slot.chars[0].weapon = game_state.slot.chars[0].armour = 0;
  // Materials are not gear.
  CHECK(shop::sell_gear(game_state, 8, 1) == shop::Res::NotSellable);
}

void test_trade() {
  Fixture fixture;
  GameState& game_state = fixture.gs;
  const shop::TradeRow row = shop::trade_row(fixture.sd, 0);
  // The row stops at the first goods id 0.
  CHECK(row.count == 2);
  CHECK(row.goods[0] == 8 && row.slot[0].mask == 0x80 && row.slot[0].need == 5);
  CHECK(row.goods[1] == 1 && row.slot[1].mask == 0 && row.slot[1].need == 3);
  CHECK(row.goods[2] == 0 && row.goods[3] == 0);
  CHECK(shop::trade_row(fixture.sd, 9).count == 0);
  CHECK(shop::colour_index(0) == 0 && shop::colour_index(0x04) == 1 && shop::colour_index(0x80) == 6 && shop::colour_index(0x24) == 4);
  // Three blue lv2, a blue lv1, a green lv1 and a red lv3 (flag 0x20: not offered).
  for (int i = 0; i < 3; ++i) game_state.add_element(2);
  game_state.add_element(4);
  game_state.add_element(1);
  game_state.add_element(3);
  int total_levels = 0;
  const std::vector<int> els = shop::trade_elements(game_state, &total_levels);
  // Slot order, padded to 11 rows.
  CHECK(total_levels == 11 && els[0] == 2 && els[1] == 4 && els[2] == 1 && els[3] == 0);
  CHECK(shop::element_stock(game_state, 2) == 3 && shop::element_stock(game_state, 3) == 1);
  CHECK(shop::trade_accepts(game_state, 0x80, 2) && shop::trade_accepts(game_state, 0x80, 4));
  // Mask 0 accepts any element.
  CHECK(!shop::trade_accepts(game_state, 0x80, 1) && shop::trade_accepts(game_state, 0, 1));
  std::vector<int> off(192, 0);
  off[0] = 2;
  CHECK(shop::offer_levels(game_state, els, off) == 4);
  off[1] = 1;
  CHECK(shop::offer_levels(game_state, els, off) == 5);
  CHECK(shop::trade_commit(game_state, els, off, row.goods[0]));
  CHECK(game_state.item_count(8) == 1 && game_state.element_count(2) == 1 && game_state.element_count(4) == 0 && game_state.element_count(1) == 1);
  // At 99 held the offer is still taken and the count stays 99.
  fixture.give(8, 98);
  std::vector<int> one(192, 0);
  one[0] = 1;
  CHECK(shop::trade_commit(game_state, els, one, row.goods[0]) && game_state.item_count(8) == 99 && game_state.element_count(2) == 0);
  // With no stack of the goods and no free slot nothing is taken.
  for (auto& stack : game_state.slot.items)
    if (stack.id == 8 || stack.id == 0) stack.id = 0x1FF, stack.qty = 1;
  std::vector<int> green(192, 0);
  green[2] = 1;
  CHECK(!shop::trade_commit(game_state, els, green, row.goods[0]) && game_state.element_count(1) == 1);
}
}  // namespace

int main() {
  test_element_shop();
  test_element_sell_picks();
  test_forge();
  test_breakdown_and_sell();
  test_trade();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("shop tests ok\n");
  return 0;
}
