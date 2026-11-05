#include "ui/menu/game_state.h"

#include <algorithm>
#include <bit>

#include "ui/menu/save_layout.h"
#include "vfs/vfs.h"

namespace oc::ui {

namespace {

// Copies of a consumable a grid cell can hold.
constexpr int kStackMax = 5;

// Glenn with the second Einlanzer: the two-sword model and the two-sword versions of Sonic Sword and Dive & Drive.
constexpr int kGlenn = 0x13, kGlennTwinModel = 0x32, kTwinEinlanzer = 0x40;
struct TechPair {
  u16 one_sword, two_swords;
};
constexpr TechPair kGlennTechs[] = {{0xDA, 0x9A}, {0xDB, 0x9B}};

// A weapon change on Glenn rewrites his model byte and swaps his sword techs in every grid cell (the cell's 0x4000 bit stays).
void apply_glenn_weapon(save::SaveChar& ch) {
  const bool twin = ch.weapon == kTwinEinlanzer;
  ch.model_id = static_cast<u16>((ch.model_id & 0xFF00) | (twin ? kGlennTwinModel : kGlenn));
  for (u16& cell : ch.grid)
    for (const TechPair& pair : kGlennTechs)
      if ((cell & 0x3FFF) == (twin ? pair.one_sword : pair.two_swords)) cell = static_cast<u16>((cell & 0x4000) | (twin ? pair.two_swords : pair.one_sword));
}

int opposite_bit(int colour_bit) {
  switch (colour_bit) {
    case 0x04: return 0x40;
    case 0x40: return 0x04;
    case 0x08: return 0x10;
    case 0x10: return 0x08;
    case 0x20: return 0x80;
    case 0x80: return 0x20;
    default: return 0;
  }
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------- rules
bool MenuRules::load_pg(const vfs::Vfs& vfs) {
  pg.clear();
  auto bytes = vfs.read("cdrom/battle/data/menu_pg.bin");
  if (!bytes) return false;
  pg.resize(bytes->size() / 16);
  for (size_t i = 0; i < pg.size(); ++i) std::copy_n(bytes->begin() + static_cast<std::ptrdiff_t>(i * 16), 16, pg[i].raw.begin());
  return true;
}

bool MenuRules::el_fixed(int id) const {
  if (id >= 0x80) return true;
  const data::ElemRec* record = elem(id);
  return record && (record->raw[1] & 0x02) != 0;
}
bool MenuRules::el_stack(int id) const {
  const data::ElemRec* record = elem(id);
  return record && (record->raw[0] & 0x30) == 0x10;
}
bool MenuRules::el_innate_only(int id) const {
  const data::ElemRec* record = elem(id);
  return record && (record->raw[3] & 0x80) != 0;
}
int MenuRules::el_level(int id) const {
  const data::ElemRec* record = elem(id);
  return record ? record->level() : 0;
}
int MenuRules::el_colour(int id) const {
  const data::ElemRec* record = elem(id);
  return record ? record->colour_bit() : 0;
}
bool MenuRules::window_ok(int id, int row) const {
  const data::ElemRec* record = elem(id);
  if (!record) return false;
  const int level = record->level(), hi = record->raw[0x12] >> 4, lo = record->raw[0x12] & 15;
  return level - hi <= row + 1 && row + 1 <= level + lo;
}

// ---------------------------------------------------------------------------------------------------------- basics
std::vector<int> GameState::party() const {
  std::vector<int> out;
  for (u8 id : slot.party())
    if (id < save::kNumChars) out.push_back(id);
  return out;
}

std::vector<int> GameState::roster() const {
  std::vector<int> out;
  for (int character = 0; character < save::kNumChars; ++character)
    if (slot.recruited(character)) out.push_back(character);
  return out;
}

bool GameState::available(int character) const {
  if (character < 0 || character >= save::kNumChars) return false;
  const bool excluded = ((slot.unk_e1b8[static_cast<size_t>(2 + (character >> 5))] >> (character & 31)) & 1) != 0;
  return slot.recruited(character) && !excluded;
}

void GameState::set_option(u16 mask, bool on) {
  u16 bits = options();
  bits = on ? static_cast<u16>(bits | mask) : static_cast<u16>(bits & ~mask);
  slot.head[0x5A0] = static_cast<u8>(bits & 0xFF);
  slot.head[0x5A1] = static_cast<u8>(bits >> 8);
}

int GameState::stamina_recovery(int character) const {
  const data::StatRec* record = rec(character);
  int total = record ? record->stamina_rate() : 0;
  if (rules && rules->tables)
    for (int k = 2; k < kEquipSlots; ++k) {
      const int id = equipped(character, k);
      const data::GearRec* gear = id > 0 ? rules->tables->gear_rec(id) : nullptr;
      if (gear && (gear->flags() & data::gearflag::kStamina)) total += gear->mag();
    }
  return total;
}

const data::StatRec* GameState::rec(int character) const {
  if (!rules || !rules->tables || character < 0 || character >= data::kNumChars) return nullptr;
  // The record set of the state (the dream's char20.bin until FE 3F, else char.bin; SaveSlot::dream_records): the +0x30 level masks
  // place the grid cells, +3 / +0x1F the growth column and stamina rate.
  auto it = rules->tables->char_sets.find(slot.dream_records ? "char20" : "char");
  if (it == rules->tables->char_sets.end()) it = rules->tables->char_sets.find("char");
  return it == rules->tables->char_sets.end() ? nullptr : &it->second.rec[static_cast<size_t>(character)];
}

int GameState::innate(int character) const {
  if (character < 0 || character >= save::kNumChars) return 0;
  if (character == 0 || character == 43) return slot.chars[static_cast<size_t>(character)].colour_bit;
  const data::StatRec* record = rec(character);
  return record ? record->innate() : 0;
}

std::vector<int> GameState::key_items() const {
  std::vector<int> out;
  for (int id = 1; id <= 64; ++id)
    if (has_key_item(id)) out.push_back(id);
  return out;
}

// ---------------------------------------------------------------------------------------------------------- stock
bool GameState::add_to(save::ItemStack* stacks, size_t count, int id) {
  if (id <= 0) return false;
  for (size_t i = 0; i < count; ++i)
    if (stacks[i].id == id && stacks[i].qty > 0) {
      if (stacks[i].qty < 99) ++stacks[i].qty;
      return true;
    }
  for (size_t i = 0; i < count; ++i)
    if (stacks[i].id == 0 || stacks[i].qty == 0) {
      stacks[i].id = static_cast<u16>(id);
      stacks[i].qty = 1;
      return true;
    }
  return false;
}

bool GameState::take_from(save::ItemStack* stacks, size_t count, int id) {
  if (id <= 0) return false;
  for (size_t i = 0; i < count; ++i)
    if (stacks[i].id == id && stacks[i].qty > 0) {
      if (--stacks[i].qty == 0) stacks[i].id = 0;
      return true;
    }
  return false;
}

int GameState::element_count(int id) const {
  int total = 0;
  for (const auto& entry : slot.elements)
    if (entry.id == id) total += entry.qty;
  return total;
}
int GameState::item_count(int id) const {
  int total = 0;
  for (const auto& entry : slot.items)
    if (entry.id == id) total += entry.qty;
  return total;
}
bool GameState::fits(const save::ItemStack* stacks, size_t count, int id) {
  if (id <= 0) return false;
  for (size_t i = 0; i < count; ++i)
    if ((stacks[i].id == id && stacks[i].qty > 0) || stacks[i].id == 0 || stacks[i].qty == 0) return true;
  return false;
}
bool GameState::element_fits(int id) const { return fits(slot.elements.data(), slot.elements.size(), id); }
bool GameState::item_fits(int id) const { return fits(slot.items.data(), slot.items.size(), id); }
bool GameState::add_element(int id) { return add_to(slot.elements.data(), slot.elements.size(), id); }
bool GameState::take_element(int id) { return take_from(slot.elements.data(), slot.elements.size(), id); }
bool GameState::add_item(int id) { return add_to(slot.items.data(), slot.items.size(), id); }
bool GameState::take_item(int id) { return take_from(slot.items.data(), slot.items.size(), id); }

// ---------------------------------------------------------------------------------------------------------- grid
int GameState::cell_index(int character, int row, int col) const {
  const data::StatRec* record = rec(character);
  if (!record || character >= save::kNumChars || row < 0 || row >= kRows || col < 0 || col >= kCols) return -1;
  const u16 mask = record->level_mask(row);
  if (!((mask >> col) & 1)) return -1;
  int cell = 0;
  for (int i = 0; i < row; ++i) cell += std::popcount(static_cast<unsigned>(record->level_mask(i)));
  cell += std::popcount(static_cast<unsigned>(mask & ((1u << col) - 1)));
  return cell < 48 ? cell : -1;
}

bool GameState::cell_available(int character, int row, int col) const {
  return cell_exists(character, row, col) && ((slot.chars[static_cast<size_t>(character)].unlocked[static_cast<size_t>(row)] >> col) & 1) != 0;
}
int GameState::cell_element(int character, int row, int col) const {
  const int cell = cell_index(character, row, col);
  return cell < 0 ? 0 : slot.chars[static_cast<size_t>(character)].grid[static_cast<size_t>(cell)] & 0x1FF;
}
int GameState::cell_count(int character, int row, int col) const {
  const int cell = cell_index(character, row, col);
  return cell < 0 ? 0 : slot.chars[static_cast<size_t>(character)].equipped_qty[static_cast<size_t>(cell)];
}
bool GameState::row_has_open_cell(int character, int row) const {
  for (int col = 0; col < kCols; ++col)
    if (cell_available(character, row, col)) return true;
  return false;
}
int GameState::first_open_row(int character) const {
  for (int row = 0; row < kRows; ++row)
    if (row_has_open_cell(character, row)) return row;
  return 0;
}
int GameState::copies_placed(int character, int id) const {
  int total = 0;
  for (int row = 0; row < kRows; ++row)
    for (int col = 0; col < kCols; ++col)
      if (cell_available(character, row, col) && cell_element(character, row, col) == id) ++total;
  return total;
}
bool GameState::eligible(int character, int id) const {
  if (const PgRow* pg_entry = rules->pg_row(id); pg_entry && !pg_entry->eligible(character)) return false;
  return !rules->el_innate_only(id) || rules->el_colour(id) == innate(character);
}

Why GameState::can_place(int character, int row, int col, int id) const {
  if (cell_index(character, row, col) < 0) return Why::NoCell;
  if (!cell_available(character, row, col)) return Why::Locked;
  const int occ = cell_element(character, row, col);
  if (occ && rules->el_fixed(occ)) return Why::Fixed;
  if (!rules->is_element(id) || !eligible(character, id)) return Why::NotEligible;
  if (!rules->window_ok(id, row)) return Why::Window;
  if (element_count(id) < 1) return Why::NoStock;
  if (occ && !element_fits(occ)) return Why::Full;
  return Why::Ok;
}

Why GameState::can_remove(int character, int row, int col) const {
  if (cell_index(character, row, col) < 0) return Why::NoCell;
  const int occ = cell_element(character, row, col);
  if (occ == 0) return Why::Empty;
  if (rules->el_fixed(occ)) return Why::Fixed;
  if (!cell_available(character, row, col)) return Why::Locked;
  if (!element_fits(occ)) return Why::Full;
  return Why::Ok;
}

// The new copies are taken before the occupant goes back to the stock, so a refill of the same element counts only what the stock
// held, and a return to a stack at 99 is accepted without raising the count.
Why GameState::place(int character, int row, int col, int id) {
  const Why why = can_place(character, row, col, id);
  if (why != Why::Ok) return why;
  const int cell = cell_index(character, row, col);
  save::SaveChar& ch = slot.chars[static_cast<size_t>(character)];
  const int occ = cell_element(character, row, col);
  const int occ_copies = !occ ? 0 : rules->el_stack(occ) ? static_cast<int>(ch.equipped_qty[static_cast<size_t>(cell)]) : 1;
  const int want = rules->el_stack(id) ? kStackMax : 1;
  int taken = 0;
  while (taken < want && take_element(id)) ++taken;
  for (int i = 0; i < occ_copies; ++i) add_element(occ);
  ch.grid[static_cast<size_t>(cell)] = static_cast<u16>(id);
  if (rules->el_stack(id)) ch.equipped_qty[static_cast<size_t>(cell)] = static_cast<u8>(taken);
  return Why::Ok;
}

Why GameState::remove(int character, int row, int col) {
  const Why why = can_remove(character, row, col);
  if (why != Why::Ok) return why;
  const int cell = cell_index(character, row, col);
  save::SaveChar& ch = slot.chars[static_cast<size_t>(character)];
  const int occ = cell_element(character, row, col);
  const bool stack = rules->el_stack(occ);
  const int copies = stack ? static_cast<int>(ch.equipped_qty[static_cast<size_t>(cell)]) : 1;
  for (int i = 0; i < copies; ++i) add_element(occ);
  ch.grid[static_cast<size_t>(cell)] = 0;
  if (stack) ch.equipped_qty[static_cast<size_t>(cell)] = 0;
  return Why::Ok;
}

Why GameState::swap(int character, int ra, int ca, int rb, int cb) {
  const int ka = cell_index(character, ra, ca), kb = cell_index(character, rb, cb);
  if (ka < 0 || kb < 0) return Why::NoCell;
  if (ka == kb) return Why::Same;
  if (!cell_available(character, ra, ca) || !cell_available(character, rb, cb)) return Why::Locked;
  const int ia = cell_element(character, ra, ca), ib = cell_element(character, rb, cb);
  if ((ia && rules->el_fixed(ia)) || (ib && rules->el_fixed(ib))) return Why::Fixed;
  if ((ia && !rules->window_ok(ia, rb)) || (ib && !rules->window_ok(ib, ra))) return Why::Window;
  save::SaveChar& ch = slot.chars[static_cast<size_t>(character)];
  std::swap(ch.grid[static_cast<size_t>(ka)], ch.grid[static_cast<size_t>(kb)]);
  std::swap(ch.equipped_qty[static_cast<size_t>(ka)], ch.equipped_qty[static_cast<size_t>(kb)]);
  return Why::Ok;
}

int GameState::clear_all(int character) {
  int cleared = 0;
  for (int row = 0; row < kRows; ++row)
    for (int col = 0; col < kCols; ++col)
      if (cell_available(character, row, col) && cell_element(character, row, col) > 0 && cell_element(character, row, col) < 0x80 &&
          remove(character, row, col) == Why::Ok)
        ++cleared;
  return cleared;
}

int GameState::learn_tech(int character, int rows, int tech) {
  const data::StatRec* record = rec(character);
  if (!record || character >= save::kNumChars || rows < 1 || rows > kRows) return -1;
  int cells = 0;
  for (int row = 0; row < rows; ++row) cells += std::popcount(static_cast<unsigned>(record->level_mask(row) & (row == rows - 1 ? 0xFF : 0xFFFF)));
  if (cells < 1 || cells > 48) return -1;
  u16& cell = slot.chars[static_cast<size_t>(character)].grid[static_cast<size_t>(cells - 1)];
  const int held = cell & 0x1FF;
  if (held != 0 && held < 0x80) add_element(held);
  cell = static_cast<u16>(tech | (cell & 0xC000));
  return cells - 1;
}

// The grid editor's list holds two label rows ahead of the occupied stock slots and ends at 192 entries, so only the first 190
// occupied slots are candidates. The buckets are built once per run and keep their order from cell to cell.
GameState::LevelBuckets GameState::level_buckets() const {
  constexpr int kListedStock = 190;
  LevelBuckets buckets;
  int listed = 0;
  for (size_t stock_slot = 0; stock_slot < slot.elements.size() && listed < kListedStock; ++stock_slot) {
    const save::ItemStack& stack = slot.elements[stock_slot];
    if (stack.id == 0 || stack.qty == 0) continue;
    ++listed;
    const int level = rules->el_level(stack.id);
    if (stack.id < 0x80 && level >= 1 && level <= kRows) buckets[static_cast<size_t>(level - 1)].push_back(static_cast<int>(stock_slot));
  }
  return buckets;
}

// menu_pg's bonus for no / one copy placed, +10 when the element has the member's innate colour and -10 when it has the opposite one
// (members with no single innate colour get neither); an empty slot -200, an element already placed twice -100.
int GameState::score(int character, int id) const {
  if (id == 0) return -200;
  const int placed = copies_placed(character, id);
  if (placed >= 2) return -100;
  int total = 0;
  if (const PgRow* pg_entry = rules->pg_row(id)) total = placed == 0 ? pg_entry->bonus_none() : pg_entry->bonus_one();
  const int innate_colour = innate(character) & 0xFC, opposite = opposite_bit(innate_colour), element_colour = rules->el_colour(id);
  if (opposite != 0) {
    if (element_colour & opposite) total -= 10;
    else if (element_colour & innate_colour) total += 10;
  }
  return total;
}

// Exchange sort to descending score; a candidate is swapped forward only by a strictly higher one.
void GameState::sort_bucket(int character, std::vector<int>& bucket) const {
  auto slot_score = [&](int stock_slot) { return score(character, slot.elements[static_cast<size_t>(stock_slot)].id); };
  for (size_t i = 0; i + 1 < bucket.size(); ++i)
    for (size_t j = i; j < bucket.size(); ++j)
      if (slot_score(bucket[i]) < slot_score(bucket[j])) std::swap(bucket[i], bucket[j]);
}

// First candidate the member may hold, legal in `row`, with enough copies in its own stock slot (5 for a stack) and fewer than two
// placed. The fallback pass also searches the buckets of rows +1, -1, +2, -2 ... around it.
int GameState::pick_for_cell(int character, int row, bool fallback, const LevelBuckets& buckets) const {
  const int reach = fallback ? kRows : 0;
  for (int distance = 0; distance <= reach; ++distance)
    for (int side = 1; side >= -1; side -= 2) {
      const int level_row = row + side * distance;
      if ((distance == 0 && side < 0) || level_row < 0 || level_row >= kRows) continue;
      for (int stock_slot : buckets[static_cast<size_t>(level_row)]) {
        const save::ItemStack& stack = slot.elements[static_cast<size_t>(stock_slot)];
        if (stack.id == 0 || !eligible(character, stack.id) || !rules->window_ok(stack.id, row)) continue;
        const int need = rules->el_stack(stack.id) ? kStackMax : 1;
        if (stack.qty >= need && copies_placed(character, stack.id) < 2) return stack.id;
      }
    }
  return 0;
}

int GameState::fill_row(int character, int row, bool fallback, LevelBuckets& buckets) {
  int placed = 0;
  for (int col = 0; col < kCols; ++col) {
    if (!cell_available(character, row, col) || cell_element(character, row, col) != 0) continue;
    sort_bucket(character, buckets[static_cast<size_t>(row)]);
    if (const int id = pick_for_cell(character, row, fallback, buckets); id != 0 && place(character, row, col, id) == Why::Ok) ++placed;
  }
  return placed;
}

// Rows from the top level down, each row for every member in turn; the exact-level pass over all rows comes before the fallback pass.
int GameState::auto_fill_members(const std::vector<int>& members, bool reset) {
  if (reset)
    for (int character : members) clear_all(character);
  LevelBuckets buckets = level_buckets();
  int placed = 0;
  for (bool fallback : {false, true})
    for (int row = kRows - 1; row >= 0; --row)
      for (int character : members) placed += fill_row(character, row, fallback, buckets);
  return placed;
}

int GameState::auto_fill(int character, bool reset) {
  if (character < 0 || character >= save::kNumChars) return 0;
  return auto_fill_members({character}, reset);
}

int GameState::auto_fill_party(bool reset) { return auto_fill_members(party(), reset); }

void GameState::sort_elements(SortMode mode, int character) {
  struct SortEntry {
    int id, qty, slot;
    int group, sub;
  };
  std::vector<SortEntry> entries;
  const int innate_colour = innate(character) & 0xFC, opposite = opposite_bit(innate_colour);
  for (size_t i = 0; i < slot.elements.size(); ++i) {
    const auto& stack = slot.elements[i];
    if (stack.id == 0 || stack.qty == 0) continue;
    SortEntry entry{stack.id, stack.qty, static_cast<int>(i), 0, 0};
    const int id = stack.id;
    switch (mode) {
      case SortMode::InnateFirst: {
        const int col = rules->el_colour(id);
        if (col & innate_colour) entry.group = 0, entry.sub = id;
        else if (col & opposite) entry.group = 2, entry.sub = entry.slot;
        else entry.group = 1;
        break;
      }
      case SortMode::AttackFirst:
        if (id >= 1 && id <= 0x30) entry.group = 0, entry.sub = -id;
        else entry.group = 1, entry.sub = id;
        break;
      case SortMode::HealFirst:
        if (id == 0x80) entry.group = 0;
        else if (id >= 0x55 && id <= 0x6B) entry.group = 1, entry.sub = id;
        else if (id >= 0x31 && id <= 0x54) entry.group = 2, entry.sub = id;
        else if (id >= 0x01 && id <= 0x30) entry.group = 3, entry.sub = -id;
        else if (id >= 0x6C && id <= 0x7D) entry.group = 4, entry.sub = -id;
        else entry.group = 5, entry.sub = id;
        break;
    }
    entries.push_back(entry);
  }
  if (mode == SortMode::InnateFirst) {
    // The middle group keeps the stock slots of the other groups and the holes as 0 and goes through an exchange sort over every
    // slot that also swaps on a 0 in front: the ids come out ascending only where no hole lies between them.
    std::vector<int> order(slot.elements.size(), -1);
    for (size_t i = 0; i < entries.size(); ++i)
      if (entries[i].group == 1) order[static_cast<size_t>(entries[i].slot)] = static_cast<int>(i);
    auto key = [&](int entry) { return entry < 0 ? 0 : entries[static_cast<size_t>(entry)].id; };
    for (size_t i = 0; i + 1 < order.size(); ++i)
      for (size_t j = i; j < order.size(); ++j)
        if (key(order[j]) < key(order[i]) || key(order[i]) == 0) std::swap(order[i], order[j]);
    int position = 0;
    for (int entry : order)
      if (entry >= 0) entries[static_cast<size_t>(entry)].sub = position++;
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const SortEntry& lhs, const SortEntry& rhs) { return lhs.group != rhs.group ? lhs.group < rhs.group : lhs.sub < rhs.sub; });
  slot.elements.fill({});
  for (size_t i = 0; i < entries.size(); ++i) {
    slot.elements[i].id = static_cast<u16>(entries[i].id);
    slot.elements[i].qty = static_cast<u8>(entries[i].qty);
  }
}

// ---------------------------------------------------------------------------------------------------------- use
std::vector<int> GameState::usable_elements() const {
  std::vector<int> out;
  for (size_t i = 0; i < slot.elements.size(); ++i) {
    const auto& stack = slot.elements[i];
    const PgRow* pg_entry = rules->pg_row(stack.id);
    if (stack.qty > 0 && stack.id > 0 && pg_entry && pg_entry->usable()) out.push_back(static_cast<int>(i));
  }
  return out;
}

Why GameState::use_element(int id, int target) {
  const PgRow* pg_entry = rules->pg_row(id);
  if (!pg_entry || !pg_entry->usable() || element_count(id) <= 0) return Why::NoStock;
  if (!available(target)) return Why::NotEligible;
  save::SaveChar& ch = slot.chars[static_cast<size_t>(target)];
  if (pg_entry->heals()) {
    static const int kHeal[4] = {50, 100, 200, 0};
    if (ch.cur_hp >= ch.max_hp) return Why::Full;
    ch.cur_hp = static_cast<u16>(std::min<int>(ch.max_hp, ch.cur_hp + kHeal[pg_entry->heal_size()]));
  } else {
    const data::ElemRec* record = rules->elem(id);
    const u32 mask = record ? record->status_lo() & 0xFF : 0;
    if ((ch.status & mask) == 0) return Why::Full;
    ch.status = static_cast<u8>(ch.status & ~mask);
  }
  take_element(id);
  return Why::Ok;
}

// ---------------------------------------------------------------------------------------------------------- equipment
int GameState::equipped(int character, int eslot) const {
  if (character < 0 || character >= save::kNumChars) return 0;
  const save::SaveChar& ch = slot.chars[static_cast<size_t>(character)];
  if (eslot == 0) return ch.weapon;
  if (eslot == 1) return ch.armour;
  if (eslot >= 2 && eslot < 5) return ch.accessory[static_cast<size_t>(eslot - 2)];
  return 0;
}

Stats GameState::stats_with(int character, int eslot, int item) const {
  Stats result;
  if (character < 0 || character >= save::kNumChars) return result;
  const save::SaveChar& ch = slot.chars[static_cast<size_t>(character)];
  auto id_at = [&](int equip_slot) { return equip_slot == eslot ? item : equipped(character, equip_slot); };
  auto gear = [&](int id) -> const data::GearRec* { return id > 0 && rules->tables ? rules->tables->gear_rec(id) : nullptr; };
  const data::GearRec* wp = gear(id_at(0));
  const data::GearRec* ar = gear(id_at(1));
  // Accessory sums (offence class 0x20, defence class 0x40).
  int o4 = 0, o5 = 0, o6 = 0, d4 = 0, d5 = 0, d6 = 0;
  for (int k = 2; k < 5; ++k)
    if (const data::GearRec* accessory = gear(id_at(k))) {
      if (accessory->flags() & data::gearflag::kOffence) o4 += accessory->raw[4], o5 += accessory->raw[5], o6 += accessory->raw[6];
      if (accessory->flags() & data::gearflag::kDefence) d4 += accessory->raw[4], d5 += accessory->raw[5], d6 += accessory->raw[6];
    }
  result.hp = ch.max_hp;
  result.atk = ch.atk + o4 + (wp ? wp->raw[4] : 0);
  result.mag = ch.mag + o5 + (wp ? wp->raw[5] : 0);
  result.hit = (2 * (ch.hit + o6 / 3)) / 3 + (wp ? wp->raw[6] / 3 : 0);
  result.def = ch.def + d4 + (ar ? ar->raw[4] : 0);
  result.mdef = ch.mdef + d5 + (ar ? ar->raw[5] : 0);
  result.evd = ch.evd + d6 + (ar ? ar->raw[6] : 0);
  return result;
}

// A candidate whose slot mask overlaps the accessory it replaces is never in conflict; otherwise its mask is tested against every
// worn accessory.
int GameState::accessory_conflict(int character, int eslot, int item) const {
  if (eslot < 2 || item <= 0 || !rules->tables) return 0;
  auto mask = [&](int id) -> int {
    const data::GearRec* gear = id > 0 ? rules->tables->gear_rec(id) : nullptr;
    return gear ? gear->slot_mask() : 0;
  };
  const int candidate = mask(item);
  if (candidate & mask(equipped(character, eslot))) return 0;
  int worn = 0;
  for (int k = 2; k < 5; ++k) worn |= mask(equipped(character, k));
  return candidate & worn;
}

bool GameState::listed(int item, int eslot) const {
  const int kind = std::min(eslot, 2);
  if (kind == 2 && item > 0 && item < kFirstGearId) return true;
  const data::ItemInfo* info = rules->tables->item(item);
  return info && info->kind() == kind;
}

std::vector<EquipCand> GameState::equip_candidates(int character, int eslot) const {
  std::vector<EquipCand> out;
  if (eslot < 0 || eslot >= kEquipSlots || !rules->tables) return out;
  const bool accessory = eslot >= 2;
  const EquipCand reserved{-1, 0, 0, accessory && equipped(character, eslot) != 0};
  if (accessory) out.push_back(reserved);
  // Accessories: gear first (pass 0), then the material ids (pass 1).
  for (int pass = 0; pass < (accessory ? 2 : 1); ++pass)
    for (size_t i = 0; i < slot.items.size(); ++i) {
      const auto& stack = slot.items[i];
      if (stack.id == 0 || stack.qty == 0 || !listed(stack.id, eslot)) continue;
      if (accessory && (stack.id < kFirstGearId) != (pass == 1)) continue;
      const data::ItemInfo* info = rules->tables->item(stack.id);
      out.push_back({static_cast<int>(i), stack.id, stack.qty, info && info->can_equip(character) && accessory_conflict(character, eslot, stack.id) == 0});
    }
  if (!accessory) out.push_back(reserved);
  return out;
}

bool GameState::can_equip(int character, int eslot, int item) const {
  if (character < 0 || character >= save::kNumChars || eslot < 0 || eslot >= kEquipSlots || !rules->tables) return false;
  if (item == 0) return eslot >= 2 && equipped(character, eslot) != 0;
  const data::ItemInfo* info = rules->tables->item(item);
  if (!info || !listed(item, eslot) || !info->can_equip(character)) return false;
  if (accessory_conflict(character, eslot, item) != 0) return false;
  return item_count(item) > 0;
}

bool GameState::equip(int character, int eslot, int item) {
  if (!can_equip(character, eslot, item)) return false;
  save::SaveChar& ch = slot.chars[static_cast<size_t>(character)];
  const int old = equipped(character, eslot);
  if (item) take_item(item);
  if (old && !add_item(old)) {
    if (item) add_item(item);
    return false;
  }
  if (eslot == 0) {
    ch.weapon = static_cast<u16>(item);
    if (character == kGlenn) apply_glenn_weapon(ch);
  } else if (eslot == 1) ch.armour = static_cast<u16>(item);
  else ch.accessory[static_cast<size_t>(eslot - 2)] = static_cast<u16>(item);
  return true;
}

void GameState::unequip_accessories(int character) {
  for (int k = 2; k < 5; ++k)
    if (equipped(character, k)) equip(character, k, 0);
}

int GameState::location_key(u32 scenario) { return save_layout::location_key(static_cast<int>(scenario)); }

std::string GameState::location_name(const import::LangTables& lt, import::Lang lang, u32 scenario, std::string* second) {
  constexpr char kLf = char(10), kCr = char(13);
  std::string text;
  if (const import::TextTable* table = const_cast<import::LangTables&>(lt).table("savetbl"))
    if (const std::string* found = table->get(static_cast<u64>(location_key(scenario)), lang)) text = *found;
  text.erase(std::remove(text.begin(), text.end(), kCr), text.end());
  const auto trim = [](std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == kLf)) value.pop_back();
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == kLf)) ++begin;
    return value.substr(begin);
  };
  const size_t nl = text.find(kLf);
  if (second) *second = nl == std::string::npos ? std::string() : trim(text.substr(nl + 1));
  return trim(nl == std::string::npos ? text : text.substr(0, nl));
}

}  // namespace oc::ui
