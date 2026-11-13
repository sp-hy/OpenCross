#pragma once
// Shop rules: element shop prices / sell rule, smith forge cost + breakdown + gear sale and
// the trading post (elements paid by summed level). Pure data logic on a GameState; no rendering.
//
// Table caveat: data::GameTables::waa_shop is parsed as 32 u16 per row; the real layout is 32 rows x 64 u16, so
// ShopData re-reads it with load_waa_shop() when a Vfs is available.
#include <array>
#include <map>
#include <vector>

#include "ui/menu/game_state.h"

namespace oc::ui::shop {

enum class Kind { Element, Smith, Trading };

enum class Res {
  Ok,
  NoGold,
  Full,            // no bag slot / 99 cap
  NoStock,
  ShortMaterials,
  NotSellable,
  Equipped,        // only a worn copy exists
  NoRecipe,
  BadOffer,        // offer below the required level sum, or an element the entry refuses
};

struct ShopData {
  std::vector<std::vector<u16>> el_shop, waa_shop;
  std::vector<std::vector<data::TradeEntry>> trade;

  static ShopData from_tables(const data::GameTables& tables) { return {tables.el_shop, tables.waa_shop, tables.trade}; }
  // 32 rows x 64 u16 (0-terminated rows); false if the file is missing.
  bool load_waa_shop(const vfs::Vfs& vfs);
  // ids > 0 up to the first 0
  std::vector<int> element_row(int shop_id) const;
  std::vector<int> gear_row(int shop_id) const;
};

constexpr int kMaxGold = 9999999;
constexpr int kStackCap = 99;

// ---- element shop
// Sellable: the element's flag bit 0x20 is clear and it is not a fixed cell element. Sell unit = price >> 2, total =
// (price * qty) >> 2. Buy max = min(99 - owned, gold / price).
int element_price(const GameState& gs, int id);
bool element_sellable(const GameState& gs, int id);
int element_sell_unit(const GameState& gs, int id);
int element_sell_total(const GameState& gs, int id, int qty);
int element_buy_max(const GameState& gs, int id);
// The info panel's target text from the element's targeting byte: `all` = bit 0x10 (All, else Single); the side is
// Enemy when only bit 0x80 is set, Ally when bit 0x80 is clear, Ally/Enemy when both 0x80 and 0x40 are set.
enum class TargetSide { Enemy, Ally, Both };
struct TargetWord {
  bool all = false;
  TargetSide side = TargetSide::Enemy;
};
TargetWord element_target_word(u8 targeting);
Res buy_element(GameState& gs, int id, int qty);
// Cheats "Every element in shops": the shop's own row first, then every other element that can be sold (price > 0), by id.
std::vector<int> element_row_all(const GameState& gs, std::vector<int> row);
// placed_copies: copies of `id` in the unlocked cells of one character (a stack cell counts its copies); party_copies: the sum over the
// available characters. Sell list: bag elements (slot order), then the ones only placed in an available character's grid. Sell max: the
// bag copies; past it the player picks whose placed copies go too. sell_element sells `bag_qty` bag copies and `picks[character]` copies
// from each character's cells (first cells first, a stack cell is reduced); the total is paid at (price * total) >> 2. Nothing is returned
// to the bag.
int placed_copies(const GameState& gs, int character, int id);
int party_copies(const GameState& gs, int id);
std::vector<int> element_sell_list(const GameState& gs);
int element_sell_max(const GameState& gs, int id);
Res sell_element(GameState& gs, int id, int bag_qty, const std::vector<int>& picks = {});

// ---- smith
struct Mat {
  int id = 0, qty = 0;
};
std::vector<int> forge_list(const GameState& gs, const ShopData& shop_data, int shop_id, int category);
// Cheats "Every weapon in shops": the smith's own list first, then every other piece of that category (weapon / armour / accessory) that has a
// forge recipe and is not a dummy (price > 0), by item id. Availability only: materials and gold are still required.
std::vector<int> forge_list_all(const GameState& gs, const ShopData& shop_data, int shop_id, int category);
// Recipe: materials 1..31 with qty > 0. Forge cost = (P / 5) * sum(qty), P = price of the highest tier material 1..6.
std::vector<Mat> recipe(const GameState& gs, int item);
int forge_cost(const GameState& gs, int item);
// `free` = the Cheats tab's "No crafting cost": the material and gold checks pass and nothing is consumed.
// Result: Ok / ShortMaterials / NoGold / NoRecipe. forge also returns Full, before taking anything, when the bag has neither a stack
// of the piece nor a free slot; at 99 held it takes the materials and the gold and the count stays 99.
Res can_forge(const GameState& gs, int item, bool free = false);
Res forge(GameState& gs, int item, bool free = false);

// equipped_copies: characters wearing it. Breakdown list: bag gear without flag 0x20, then the available characters' worn gear
// without flag 0x20; both with a recipe. breakdown is refused when only worn copies exist.
int equipped_copies(const GameState& gs, int item);
std::vector<int> breakdown_list(const GameState& gs, int category);
Res breakdown(GameState& gs, int item, int* gold_gain = nullptr);

// Gear sale proceeds = (price10 * 10 * qty) >> 2, on the total. Sell list: bag gear of that category without flag 0x80, then the
// available characters' worn gear without flag 0x20; sell_gear sells bag copies only (NoStock for a worn-only piece).
long long gear_sell_price(const GameState& gs, int item, int qty);
std::vector<int> gear_sell_list(const GameState& gs, int category);
Res sell_gear(GameState& gs, int item, int qty);

// ---- trading post. trade.bin row = 16 entries {u8 goods id, u8 colour mask, u16 level sum}.
struct TradeSlot {
  int item = 0, mask = 0, need = 0;
};
struct TradeRow {
  // The row's entries as stored.
  std::array<TradeSlot, 16> slot{};
  // Goods id per entry index, 0 = none.
  std::array<int, 16> goods{};
  // Entries with a level sum.
  int count = 0;
};
// Stops at the first goods id 0; need 0 = hole (index kept).
TradeRow trade_row(const ShopData& shop_data, int shop_id);
// Element stock ids in slot order (192 entries, 0 = none) without flag bit 0x20; `count` = listed ids, at least 11.
std::vector<int> trade_elements(const GameState& gs, int* count);
// element_stock: count of the last element slot holding id. item_stock: count of the last equipment / material slot
// holding id. colour_index: highest colour bit 0x04..0x80 -> 1..6, 0 = none.
int element_stock(const GameState& gs, int id);
int item_stock(const GameState& gs, int id);
int colour_index(int mask);
// mask 0 = any, else it must share a bit with the element's colour mask.
bool trade_accepts(const GameState& gs, int mask, int element);
// Sum of element level x offered count over the list.
int offer_levels(const GameState& gs, const std::vector<int>& els, const std::vector<int>& offered);
// "Yes": every offered copy is taken, then the goods are added once; at 99 held the add is accepted without raising the count.
// False (nothing taken) when the bag has neither a stack of the goods nor a free slot.
bool trade_commit(GameState& gs, const std::vector<int>& els, const std::vector<int>& offered, int goods);

}  // namespace oc::ui::shop
