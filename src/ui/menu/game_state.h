#pragma once
// In-memory game state for the menus (a thin wrapper over save::SaveSlot) and the menu rule helpers:
// party / roster, inventory stock ops, the element grid editor (legality, place / remove / swap, clear, auto-fill,
// sorting), equipment compatibility and stat previews, element use (heal / cure) and the options word.
// Pure data logic; no rendering.
#include <array>
#include <string>
#include <vector>

#include "data/tables.h"
#include "import/text.h"
#include "save/cc_save.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::ui {

// menu_pg.bin (cdrom/battle/data, 16-byte rows, index = element id): eligibility, price, use flags, auto-fill bonuses.
struct PgRow {
  std::array<u8, 16> raw{};
  bool eligible(int character) const {
    return character >= 0 && character < 48 && ((raw[static_cast<size_t>(character >> 3)] >> (character & 7)) & 1) != 0;
  }
  int price() const { return static_cast<s16>(raw[8] | (raw[9] << 8)); }
  // 0:+50 1:+100 2:+200 3:0
  int heal_size() const { return raw[0xA] & 3; }
  bool usable() const { return (raw[0xB] & 0x80) != 0; }
  bool heals() const { return (raw[0xB] & 0x40) != 0; }
  // Auto-fill score with 0 copies placed.
  int bonus_none() const { return raw[0xC]; }
  // Auto-fill score with 1 copy placed.
  int bonus_one() const { return raw[0xD]; }
};

// Static tables the rules need. `tables` must outlive it. `pg` may be empty (everybody eligible, no bonuses).
struct MenuRules {
  const data::GameTables* tables = nullptr;
  std::vector<PgRow> pg;

  bool load_pg(const vfs::Vfs& vfs);

  const data::ElemRec* elem(int id) const {
    return tables && id > 0 && id < static_cast<int>(tables->elements.size()) ? &tables->elements[static_cast<size_t>(id)]
                                                                                 : nullptr;
  }
  const PgRow* pg_row(int id) const {
    return id >= 0 && id < static_cast<int>(pg.size()) ? &pg[static_cast<size_t>(id)] : nullptr;
  }
  // ids >= 0x80 are techs / special cells, flag 0x200 = fixed innate-colour element: neither can be moved or removed.
  bool el_fixed(int id) const;
  // Consumable stack (placed up to 5 at a time) / innate-colour-only element.
  bool el_stack(int id) const;
  bool el_innate_only(int id) const;
  int el_level(int id) const;
  int el_colour(int id) const;
  // Level window: legal in grid row r (0-based) iff L-hi <= r+1 <= L+lo.
  bool window_ok(int id, int row) const;
  bool is_element(int id) const { return id >= 1 && id < 0x80 && elem(id) != nullptr; }
};

enum class Why { Ok, NoCell, Locked, Fixed, NotEligible, Window, NoStock, Full, Same, Empty };

struct Stats {
  int hp = 0, atk = 0, def = 0, hit = 0, evd = 0, mag = 0, mdef = 0;
};

struct EquipCand {
  int stock_slot = -1;  // index into SaveSlot::items; -1 = the "remove" row
  int item = 0;
  int qty = 0;
  bool ok = false;      // equippable by the character (and no accessory conflict)
};

enum class SortMode { InnateFirst = 0, AttackFirst = 1, HealFirst = 2 };

// Option bits of the options word (save head 0x5A0).
namespace opt {
constexpr u16 kCursorMemory = 0x01, kAutoAttack = 0x02, kMono = 0x04, kCustomWindow = 0x08, kControllerEdit = 0x100,
              kButtonWalks = 0x200, kVibration = 0x400, kAnalogStick = 0x800;
}

class GameState {
 public:
  save::SaveSlot slot;
  const MenuRules* rules = nullptr;
  u8 skin = 0;  // window skin byte (0..14), not part of the slot image

  GameState() = default;
  GameState(save::SaveSlot initial_slot, const MenuRules* menu_rules) : slot(std::move(initial_slot)), rules(menu_rules) {}

  // ---- party / roster / misc
  // Valid member ids in order (0..3).
  std::vector<int> party() const;
  // Recruited character ids (0..43), ascending.
  std::vector<int> roster() const;
  // Recruited and not excluded: the characters the menus may show and act on outside the party.
  bool available(int character) const;
  u32 gold() const { return slot.gold(); }
  void set_gold(u32 amount) { slot.set_gold(amount); }
  u32 play_seconds() const { return slot.playtime_frames() / 60; }
  u16 options() const { return static_cast<u16>(slot.head[0x5A0] | (slot.head[0x5A1] << 8)); }
  bool option(u16 mask) const { return (options() & mask) != 0; }
  void set_option(u16 mask, bool on);
  // Innate colour bit of a character (0 if none).
  int innate(int character) const;
  // Location caption: the savetbl row key is the number of the 30 progress thresholds (start values 0, 8, 16, ...
  // 209, 999) that the story variable 0 (SCENARIO, mirrored in head 0x40) has reached; the caption has two lines.
  // The key is 1..30.
  static int location_key(u32 scenario);
  int location_key() const { return location_key(slot.progression()); }
  // First line of the caption in `lang` ("" when the savetbl is unavailable); `second` (optional) receives the second line.
  static std::string location_name(const import::LangTables& lt, import::Lang lang, u32 scenario, std::string* second = nullptr);
  std::string location_name(const import::LangTables& lt, import::Lang lang, std::string* second = nullptr) const {
    return location_name(lt, lang, slot.progression(), second);
  }
  bool has_key_item(int id) const { return id >= 1 && id <= 64 && ((slot.key_items() >> (id - 1)) & 1) != 0; }
  // Key item ids, ascending.
  std::vector<int> key_items() const;

  // ---- element stock (192 slots) and gear/material stock (384 slots), 99 per slot
  int element_count(int id) const;
  bool add_element(int id);
  bool take_element(int id);
  int item_count(int id) const;
  bool add_item(int id);
  bool take_item(int id);
  // An add would land: the id already has a stack (at 99 the add is accepted without raising the count) or a slot is free.
  bool element_fits(int id) const;
  bool item_fits(int id) const;

  // ---- element grid: 8 rows (star level) x 10 columns; cells are stored compactly over the set bits of the shape mask
  static constexpr int kRows = data::kGridRows, kCols = data::kGridCols;
  // -1 if the character has no such cell.
  int cell_index(int character, int row, int col) const;
  bool cell_exists(int character, int row, int col) const { return cell_index(character, row, col) >= 0; }
  // Unlocked mask bit.
  bool cell_available(int character, int row, int col) const;
  // Stored id (0 empty; >= 0x80 tech / special).
  int cell_element(int character, int row, int col) const;
  // Copies held by a stack cell.
  int cell_count(int character, int row, int col) const;
  bool row_has_open_cell(int character, int row) const;
  int first_open_row(int character) const;
  // Copies of `id` in the member's unlocked cells.
  int copies_placed(int character, int id) const;
  // The member may hold the element: its eligibility bit, and the innate colour for innate-only elements.
  bool eligible(int character, int id) const;

  Why can_place(int character, int row, int col, int id) const;
  Why can_remove(int character, int row, int col) const;
  Why place(int character, int row, int col, int id);
  Why remove(int character, int row, int col);
  Why swap(int character, int row_a, int col_a, int row_b, int col_b);
  // Returns the number of cells emptied.
  int clear_all(int character);
  // Story tech learning: `tech` goes into the cell that closes grid row `rows - 1` (the last cell of its first 8 columns, counted
  // over the level masks); an element the cell held goes back to the stock, the cell keeps its flag bits. Returns the cell, -1 none.
  int learn_tech(int character, int rows, int tech);
  // reset = recommended layout (the grid is cleared first), else fill the empty cells; returns cells placed. The party version clears
  // every member first and fills row by row across the members, so they share the stock in that order.
  int auto_fill(int character, bool reset);
  int auto_fill_party(bool reset);
  // Re-packs the element stock.
  void sort_elements(SortMode mode, int character);

  // ---- element use from the menu (menu_pg flag 0x80)
  // Stock slots whose element can be used from the menu.
  std::vector<int> usable_elements() const;
  // Heal / cure, consumes one on success.
  Why use_element(int id, int target);

  // ---- equipment
  // Weapon, armour, accessory x3.
  static constexpr int kEquipSlots = 5;
  int equipped(int character, int eslot) const;
  Stats stats(int character) const { return stats_with(character, -1, 0); }
  // Status screen "STAMINA RECOVERY": the character record value plus the stamina accessories.
  int stamina_recovery(int character) const;
  // eslot < 0: current gear.
  Stats stats_with(int character, int eslot, int item) const;
  // The candidate list of a slot. An accessory list opens with a reserved row (stock_slot -1, item 0; selectable when the slot is
  // filled), then the bag's accessories, then every bag material (no character can wear one), each part in bag order. A weapon or armour list holds the
  // bag's stacks of its kind and ends with one reserved row, never selectable.
  std::vector<EquipCand> equip_candidates(int character, int eslot) const;
  bool can_equip(int character, int eslot, int item) const;
  // Item 0 clears an accessory slot.
  bool equip(int character, int eslot, int item);
  void unequip_accessories(int character);

 private:
  bool add_to(save::ItemStack* stacks, size_t count, int id);
  bool take_from(save::ItemStack* stacks, size_t count, int id);
  static bool fits(const save::ItemStack* stacks, size_t count, int id);
  // Auto-fill candidates: per element level (bucket level - 1), the stock slots holding an element of that level, in stock order.
  using LevelBuckets = std::array<std::vector<int>, kRows>;
  LevelBuckets level_buckets() const;
  int score(int character, int id) const;
  void sort_bucket(int character, std::vector<int>& bucket) const;
  int pick_for_cell(int character, int row, bool fallback, const LevelBuckets& buckets) const;
  int fill_row(int character, int row, bool fallback, LevelBuckets& buckets);
  int auto_fill_members(const std::vector<int>& members, bool reset);
  const data::StatRec* rec(int character) const;
  int accessory_conflict(int character, int eslot, int item) const;
  // Bag ids below this are the materials.
  static constexpr int kFirstGearId = 0x20;
  // Whether a bag id belongs in the slot's list: its item kind is min(eslot, 2); an accessory list also takes every material id.
  bool listed(int item, int eslot) const;
};

}  // namespace oc::ui
