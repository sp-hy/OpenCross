#include "ui/shop/shop_logic.h"

#include <algorithm>

#include "vfs/vfs.h"

namespace oc::ui::shop {

namespace {
const data::ItemInfo* info(const GameState& gs, int id) {
  return gs.rules && gs.rules->tables ? gs.rules->tables->item(id) : nullptr;
}
int lv(const GameState& gs, int id) {
  const data::ElemRec* record = gs.rules ? gs.rules->elem(id) : nullptr;
  return record ? record->level() : 0;
}
void add_gold(GameState& gs, long long delta) {
  gs.set_gold(static_cast<u32>(std::clamp<long long>(static_cast<long long>(gs.gold()) + delta, 0, kMaxGold)));
}
std::vector<int> row_ids(const std::vector<std::vector<u16>>& rows, int shop_id) {
  std::vector<int> out;
  if (shop_id < 0 || shop_id >= static_cast<int>(rows.size())) return out;
  for (u16 id : rows[static_cast<size_t>(shop_id)]) {
    if (id == 0) break;
    out.push_back(id);
  }
  return out;
}
}  // namespace

bool ShopData::load_waa_shop(const vfs::Vfs& vfs) {
  auto bytes = vfs.read("cdrom/menu/waa_shop.bin");
  if (!bytes) return false;
  waa_shop.clear();
  for (size_t shop = 0; shop < 32; ++shop) {
    std::vector<u16> row;
    for (size_t col = 0; col < 64; ++col) {
      const size_t offset = shop * 128 + col * 2;
      if (offset + 2 > bytes->size()) break;
      row.push_back(static_cast<u16>((*bytes)[offset] | ((*bytes)[offset + 1] << 8)));
    }
    while (!row.empty() && row.back() == 0) row.pop_back();
    waa_shop.push_back(std::move(row));
  }
  return true;
}

std::vector<int> ShopData::element_row(int shop_id) const { return row_ids(el_shop, shop_id); }
std::vector<int> ShopData::gear_row(int shop_id) const { return row_ids(waa_shop, shop_id); }

// ------------------------------------------------------------------------------------------------ element shop
int element_price(const GameState& gs, int id) {
  const PgRow* pg_entry = gs.rules ? gs.rules->pg_row(id) : nullptr;
  return pg_entry ? std::max(0, pg_entry->price()) : 0;
}
bool element_sellable(const GameState& gs, int id) {
  const PgRow* pg_entry = gs.rules ? gs.rules->pg_row(id) : nullptr;
  if (id <= 0 || (pg_entry && (pg_entry->raw[0xA] & 0x20))) return false;
  return !(gs.rules && gs.rules->el_fixed(id));
}
std::vector<int> element_row_all(const GameState& gs, std::vector<int> row) {
  for (int id = 1; id < 0x80; ++id) {
    if (!gs.rules || !gs.rules->is_element(id) || !element_sellable(gs, id) || element_price(gs, id) <= 0) continue;
    if (std::find(row.begin(), row.end(), id) == row.end()) row.push_back(id);
  }
  return row;
}
int element_sell_unit(const GameState& gs, int id) { return element_price(gs, id) >> 2; }
int element_sell_total(const GameState& gs, int id, int qty) { return (element_price(gs, id) * qty) >> 2; }
int element_buy_max(const GameState& gs, int id) {
  const int price = element_price(gs, id);
  if (price <= 0) return 0;
  return std::max(0, std::min(kStackCap - gs.element_count(id), static_cast<int>(gs.gold() / static_cast<u32>(price))));
}
Res buy_element(GameState& gs, int id, int qty) {
  const int price = element_price(gs, id);
  if (qty < 1 || price <= 0) return Res::NoStock;
  if (gs.element_count(id) + qty > kStackCap) return Res::Full;
  if (static_cast<long long>(price) * qty > gs.gold()) return Res::NoGold;
  int added = 0;
  while (added < qty && gs.add_element(id)) ++added;
  // No free slot: undo.
  if (added < qty) {
    for (int i = 0; i < added; ++i) gs.take_element(id);
    return Res::Full;
  }
  add_gold(gs, -static_cast<long long>(price) * qty);
  return Res::Ok;
}

int placed_copies(const GameState& gs, int character, int id) {
  if (!gs.rules || id <= 0) return 0;
  const bool stack = gs.rules->el_stack(id);
  int copies = 0;
  for (int row = 0; row < GameState::kRows; ++row)
    for (int col = 0; col < GameState::kCols; ++col)
      if (gs.cell_available(character, row, col) && gs.cell_element(character, row, col) == id)
        copies += stack ? gs.cell_count(character, row, col) : 1;
  return copies;
}

int party_copies(const GameState& gs, int id) {
  int copies = 0;
  for (int character = 0; character < save::kNumChars; ++character)
    if (gs.available(character)) copies += placed_copies(gs, character, id);
  return copies;
}

std::vector<int> element_sell_list(const GameState& gs) {
  std::vector<int> out;
  auto push = [&](int id) {
    if (id > 0 && element_sellable(gs, id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  };
  for (const auto& stack : gs.slot.elements)
    if (stack.qty > 0) push(stack.id);
  if (gs.rules)
    for (int character = 0; character < save::kNumChars; ++character) {
      if (!gs.available(character)) continue;
      for (int row = 0; row < GameState::kRows; ++row)
        for (int col = 0; col < GameState::kCols; ++col) {
          const int element = gs.cell_element(character, row, col);
          if (element > 0 && gs.rules->is_element(element) && placed_copies(gs, character, element) > 0) push(element);
        }
    }
  return out;
}
int element_sell_max(const GameState& gs, int id) { return gs.element_count(id); }

namespace {
void take_placed(GameState& gs, int character, int id, int count) {
  const bool stack = gs.rules->el_stack(id);
  for (int row = 0; row < GameState::kRows && count > 0; ++row)
    for (int col = 0; col < GameState::kCols && count > 0; ++col) {
      if (!gs.cell_available(character, row, col) || gs.cell_element(character, row, col) != id) continue;
      const size_t cell = static_cast<size_t>(gs.cell_index(character, row, col));
      save::SaveChar& member = gs.slot.chars[static_cast<size_t>(character)];
      int left = 1;
      if (stack) {
        const int held = member.equipped_qty[cell];
        left = std::max(0, held - count);
        count = std::max(0, count - held);
        member.equipped_qty[cell] = static_cast<u8>(left);
      } else {
        --count;
        left = 0;
      }
      if (left < 1) member.grid[cell] = 0;
    }
}
}  // namespace

Res sell_element(GameState& gs, int id, int bag_qty, const std::vector<int>& picks) {
  if (!element_sellable(gs, id)) return Res::NotSellable;
  int picked = 0;
  for (size_t character = 0; character < picks.size(); ++character) {
    if (picks[character] == 0) continue;
    if (picks[character] < 0 || character >= static_cast<size_t>(save::kNumChars) || !gs.available(static_cast<int>(character)) ||
        picks[character] > placed_copies(gs, static_cast<int>(character), id))
      return Res::NoStock;
    picked += picks[character];
  }
  if (bag_qty < 0 || bag_qty > gs.element_count(id)) return Res::NoStock;
  for (int i = 0; i < bag_qty; ++i) gs.take_element(id);
  for (size_t character = 0; character < picks.size(); ++character)
    if (picks[character] > 0) take_placed(gs, static_cast<int>(character), id, picks[character]);
  add_gold(gs, element_sell_total(gs, id, bag_qty + picked));
  return Res::Ok;
}

// ------------------------------------------------------------------------------------------------ smith
std::vector<int> forge_list(const GameState& gs, const ShopData& shop_data, int shop_id, int category) {
  std::vector<int> out;
  for (int id : shop_data.gear_row(shop_id))
    if (const data::ItemInfo* item_info = info(gs, id); item_info && item_info->kind() == category) out.push_back(id);
  return out;
}

std::vector<int> forge_list_all(const GameState& gs, const ShopData& shop_data, int shop_id, int category) {
  std::vector<int> out = forge_list(gs, shop_data, shop_id, category);
  const auto* tables = gs.rules ? gs.rules->tables : nullptr;
  if (!tables) return out;
  for (int id = 1; id < static_cast<int>(tables->forge.size()) && id < 256; ++id) {
    const data::ItemInfo* item_info = info(gs, id);
    if (!item_info || item_info->kind() != category || item_info->price() <= 0 || recipe(gs, id).empty()) continue;
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

std::vector<Mat> recipe(const GameState& gs, int item) {
  std::vector<Mat> out;
  const auto* tables = gs.rules ? gs.rules->tables : nullptr;
  if (!tables || item < 0 || item >= static_cast<int>(tables->forge.size())) return out;
  for (int material = 1; material < 32; ++material) {
    const int qty = tables->forge[static_cast<size_t>(item)][static_cast<size_t>(material)];
    if (qty > 0) out.push_back({material, qty});
  }
  return out;
}

int forge_cost(const GameState& gs, int item) {
  int sum = 0, tier = 0;
  for (const Mat& material : recipe(gs, item)) {
    sum += material.qty;
    if (material.id < 7) tier = std::max(tier, material.id);
  }
  int tier_price = 1000;
  if (tier > 0)
    if (const data::ItemInfo* tier_info = info(gs, tier)) tier_price = tier_info->price();
  return (tier_price / 5) * sum;
}

Res can_forge(const GameState& gs, int item, bool free) {
  const auto materials = recipe(gs, item);
  if (materials.empty()) return Res::NoRecipe;
  if (free) return Res::Ok;
  for (const Mat& material : materials)
    if (gs.item_count(material.id) < material.qty) return Res::ShortMaterials;
  if (static_cast<long long>(forge_cost(gs, item)) > gs.gold()) return Res::NoGold;
  return Res::Ok;
}

Res forge(GameState& gs, int item, bool free) {
  const Res check = can_forge(gs, item, free);
  if (check != Res::Ok) return check;
  if (!gs.item_fits(item)) return Res::Full;
  const int cost = free ? 0 : forge_cost(gs, item);
  if (!free)
    for (const Mat& material : recipe(gs, item))
      for (int i = 0; i < material.qty; ++i) gs.take_item(material.id);
  gs.add_item(item);
  add_gold(gs, -cost);
  return Res::Ok;
}

int equipped_copies(const GameState& gs, int item) {
  int copies = 0;
  for (int character : gs.roster())
    for (int k = 0; k < GameState::kEquipSlots; ++k) copies += gs.equipped(character, k) == item;
  return copies;
}

std::vector<int> breakdown_list(const GameState& gs, int category) {
  std::vector<int> out;
  auto push = [&](int id) {
    const data::ItemInfo* item_info = info(gs, id);
    if (id <= 0 || !item_info || item_info->kind() != category || (item_info->extra & 0x20) || recipe(gs, id).empty()) return;
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  };
  for (const auto& stack : gs.slot.items)
    if (stack.qty > 0) push(stack.id);
  for (int character : gs.roster())
    for (int k = 0; k < GameState::kEquipSlots; ++k) push(gs.equipped(character, k));
  return out;
}

Res breakdown(GameState& gs, int item, int* gold_gain) {
  if (gold_gain) *gold_gain = 0;
  const data::ItemInfo* item_info = info(gs, item);
  if (!item_info || (item_info->extra & 0x20)) return Res::NotSellable;
  const auto materials = recipe(gs, item);
  if (materials.empty()) return Res::NoRecipe;
  if (gs.item_count(item) < 1) return Res::Equipped;
  gs.take_item(item);
  long long gold = 0;
  for (const Mat& material : materials) {
    int over = 0;
    for (int k = 0; k < material.qty; ++k)
      if (gs.item_count(material.id) >= kStackCap || !gs.add_item(material.id)) ++over;
    // Materials that no longer fit in the bag are paid out as gold: trunc(overflow * p10 * 10 / 4).
    if (const data::ItemInfo* material_info = info(gs, material.id)) gold += static_cast<long long>(over) * material_info->price() / 4;
  }
  add_gold(gs, gold);
  if (gold_gain) *gold_gain = static_cast<int>(gold);
  return Res::Ok;
}

int gear_sell_unit(const GameState& gs, int item) {
  const data::ItemInfo* item_info = info(gs, item);
  return item_info ? item_info->price() / 4 : 0;
}

std::vector<int> gear_sell_list(const GameState& gs, int category) {
  std::vector<int> out;
  for (const auto& stack : gs.slot.items) {
    const data::ItemInfo* item_info = stack.qty > 0 ? info(gs, stack.id) : nullptr;
    if (item_info && item_info->kind() == category && !(item_info->extra & 0x80) &&
        std::find(out.begin(), out.end(), static_cast<int>(stack.id)) == out.end())
      out.push_back(stack.id);
  }
  return out;
}

Res sell_gear(GameState& gs, int item, int qty) {
  const data::ItemInfo* item_info = info(gs, item);
  if (!item_info || (item_info->extra & 0x80) || item_info->kind() == 4) return Res::NotSellable;
  if (qty < 1 || qty > gs.item_count(item)) return Res::NoStock;
  for (int k = 0; k < qty; ++k) gs.take_item(item);
  add_gold(gs, static_cast<long long>(gear_sell_unit(gs, item)) * qty);
  return Res::Ok;
}

// ------------------------------------------------------------------------------------------------ trading post
TradeRow trade_row(const ShopData& shop_data, int shop_id) {
  TradeRow row;
  if (shop_id < 0 || shop_id >= static_cast<int>(shop_data.trade.size())) return row;
  const auto& src = shop_data.trade[static_cast<size_t>(shop_id)];
  for (size_t i = 0; i < 16 && i < src.size(); ++i) {
    const data::TradeEntry& entry = src[i];
    row.slot[i] = {entry.id & 0xFF, (entry.id >> 8) & 0xFF, entry.qty};
    // The row ends at the first goods byte 0.
    if ((entry.id & 0xFF) == 0) break;
    if (entry.qty != 0) {
      row.goods[i] = entry.id & 0xFF;
      ++row.count;
    }
  }
  return row;
}

std::vector<int> trade_elements(const GameState& gs, int* count) {
  std::vector<int> out(192, 0);
  int listed = 0;
  for (const auto& stack : gs.slot.elements) {
    if (stack.id == 0) continue;
    const PgRow* pg = gs.rules ? gs.rules->pg_row(stack.id) : nullptr;
    if (pg && (pg->raw[0xA] & 0x20)) continue;
    if (listed < 192) out[static_cast<size_t>(listed++)] = stack.id;
  }
  if (count) *count = std::max(listed, 11);
  return out;
}

int element_stock(const GameState& gs, int id) {
  int qty = 0;
  for (const auto& stack : gs.slot.elements)
    if (stack.id == id) qty = stack.qty;
  return qty;
}

int item_stock(const GameState& gs, int id) {
  int qty = 0;
  for (const auto& stack : gs.slot.items)
    if (stack.id == id) qty = stack.qty;
  return qty;
}

int colour_index(int mask) {
  int index = 0;
  for (int bit = 2; bit < 8; ++bit)
    if (mask & (1 << bit)) index = bit - 1;
  return index;
}

bool trade_accepts(const GameState& gs, int mask, int element) {
  if (mask == 0) return true;
  return gs.rules && (mask & gs.rules->el_colour(element)) != 0;
}

int offer_levels(const GameState& gs, const std::vector<int>& els, const std::vector<int>& offered) {
  int total = 0;
  for (size_t i = 0; i < els.size() && i < offered.size(); ++i)
    if (els[i] > 0 && offered[i] > 0) total += lv(gs, els[i]) * offered[i];
  return total;
}

bool trade_commit(GameState& gs, const std::vector<int>& els, const std::vector<int>& offered, int goods) {
  if (goods > 0 && !gs.item_fits(goods)) return false;
  for (size_t i = 0; i < els.size() && i < offered.size(); ++i)
    for (int k = 0; els[i] > 0 && k < offered[i]; ++k) gs.take_element(els[i]);
  if (goods > 0) gs.add_item(goods);
  return true;
}

}  // namespace oc::ui::shop
