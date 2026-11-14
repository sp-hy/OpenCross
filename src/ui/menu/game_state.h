#pragma once
// In-memory game state for the menus (a thin wrapper over save::SaveSlot) and the menu rule helpers:
// party / roster, inventory stock ops, the element grid editor (legality, place / remove / swap, clear, auto-fill,
// sorting), equipment compatibility and stat previews, element use (heal / cure) and the options word.
// Pure data logic; no rendering.
#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "data/tables.h"
#include "import/text.h"
#include "save/cc_save.h"
#include "save/grid_preset.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::ui {

// menu_pg.bin (cdrom/battle/data, 16-byte rows, index = element id): eligibility (48 character bits; `who` decides the characters mods
// add and the rows' exceptions), price, use flags, auto-fill bonuses.
struct PgRow {
  std::array<u8, 16> raw{};
  data::Eligibility who;
  bool eligible(int character) const {
    if (const std::optional<bool> listed = who.rule(character)) return *listed;
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
  // Techs / special cells (ids >= 0x80 but a mod's element records, data::mod_element) and the fixed innate-colour elements (kind bit
  // 0x02): neither can be moved or removed.
  bool el_fixed(int id) const;
  // Consumable stack (placed up to 5 at a time) / innate-colour-only element.
  bool el_stack(int id) const;
  bool el_innate_only(int id) const;
  bool el_summon(int id) const;
  // The record's kind (data::element_kind); Other without the tables.
  data::ElementKind el_kind(int id) const { return tables ? data::element_kind(tables->elements, id) : data::ElementKind::Other; }
  int el_level(int id) const;
  int el_colour(int id) const;
  // Level window: legal in grid row r (0-based) iff L-hi <= r+1 <= L+lo.
  bool window_ok(int id, int row) const;
  // Vanilla 1..0x7F (with or without the tables), or a mod record past 527 that isn't a tech (data::mod_element).
  bool is_element(int id) const { return (id >= 1 && id < 0x80) || (elem(id) != nullptr && data::mod_element(tables->elements, id)); }
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

// What a saved grid preset shows in place of a name: the two colours its single-colour elements hold most of (colour index 0..5 as the
// grid screen's plates: green, white, black, red, yellow, blue; -1 none) and its highest-level element (0 when it has none).
struct PresetSummary {
  int colours[2] = {-1, -1};
  int top_id = 0;
};

// The result of loading a preset: the elements placed and the elements the stock lacked, as (element id, copies short).
struct PresetLoad {
  int placed = 0;
  std::vector<std::pair<int, int>> missing;
};

// Option bits of the options word (save head 0x5A0).
namespace opt {
constexpr u16 kCursorMemory = 0x01, kAutoAttack = 0x02, kMono = 0x04, kCustomWindow = 0x08, kControllerEdit = 0x100,
              kButtonWalks = 0x200, kVibration = 0x400, kAnalogStick = 0x800;
}

class GameState {
 public:
  save::SaveSlot slot;
  const MenuRules* rules = nullptr;
  // Window skin byte (0..14). The slot carries it as variable 0x44 (FRAME).
  u8 skin = 0;

  GameState() = default;
  GameState(save::SaveSlot initial_slot, const MenuRules* menu_rules) : slot(std::move(initial_slot)), rules(menu_rules) {}

  // ---- party / roster / misc
  // Valid member ids in order (0..3).
  std::vector<int> party() const;
  // Recruited character ids (the game's 0..43, then the characters mods add), ascending.
  std::vector<int> roster() const;
  // Recruited and not excluded: the characters the menus may show and act on outside the party.
  bool available(int character) const;
  // A character mods add whose model byte is its record's (no disguise): it shows its own art by name where its mod ships some.
  bool own_body(int character) const;
  // The rows of the roster lists (main menu, Elements use): one per game character by id (blank when not available), then one per
  // character mods add. roster_at = the character of row `row`, -1 past the end.
  int roster_rows() const { return save::kNumChars + std::max(0, static_cast<int>(slot.chars.size()) - save::kFirstAddedChar); }
  int roster_at(int row) const {
    if (row < 0 || row >= roster_rows()) return -1;
    return row < save::kNumChars ? row : save::kFirstAddedChar + row - save::kNumChars;
  }
  // The party reserve (party swap) as `size` slots: the saved slots, cut or padded with empties (-1); a character that is not available,
  // is in the party or appears twice leaves its slot empty. A save that never had a reserve starts with the first available characters
  // outside the party (ascending ids).
  std::vector<int> reserve(int size) const;
  u32 gold() const { return slot.gold(); }
  void set_gold(u32 amount) { slot.set_gold(amount); }
  u32 play_seconds() const { return slot.playtime_frames() / 60; }
  u16 options() const { return static_cast<u16>(slot.head[0x5A0] | (slot.head[0x5A1] << 8)); }
  bool option(u16 mask) const { return (options() & mask) != 0; }
  void set_option(u16 mask, bool on);
  // Leaving Customize and every load: with Window Frame Default the skin byte becomes 0; variable 0x44 takes the skin byte.
  void store_window_frame();
  // A loaded slot: the skin byte from variable 0x44, then store_window_frame.
  void load_window_frame();
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
  // Key item `id` (1-based): the game's 1..64, then the key items mods add by name (save::SaveSlot::key_items_wide).
  bool has_key_item(int id) const { return slot.has_key_item(id); }
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
  // Stored id (0 empty; >= 0x80 tech / special, or a mod's element record).
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
  // `keep_summon_copy` = the Cheats tab's "More than one summon": a summon is placed without taking its copy from the stock.
  Why place(int character, int row, int col, int id, bool keep_summon_copy = false);
  Why remove(int character, int row, int col);
  Why swap(int character, int row_a, int col_a, int row_b, int col_b);
  // The menu's Remove All: unlocked cells only, each through remove(). Returns the number of cells emptied.
  int clear_all(int character);
  // Copies a grid cell holds count for `id` (the element table's counted flag); other elements are one copy.
  bool counts_copies(int id) const;
  // MenuRules::is_element; without rules the vanilla range 1..0x7F.
  bool is_element(int id) const { return rules ? rules->is_element(id) : id >= 1 && id < 0x80; }
  // Script strip (FE 6F / FE 7D / the cleared-save strip): every cell holding an element (is_element), locked or fixed alike, returns its
  // element to the stock (its counted copies, else one; none when the stock is full), keeps only its flag bits and its count is cleared.
  void strip_grid(int character);
  // A gear word given back by a script strip: < 0x200 one item, 0x200..0x3FF an element (five copies for 0x261..0x26B, else one),
  // 0x401..0x43F the key item bit (key items 1..63, as the script item ops), from data::kScriptElementBase one copy of any element;
  // the word is cleared whatever it held.
  void return_gear(u16& word);
  // Story tech learning: `tech` goes into the cell that closes grid row `rows - 1` (the last cell of its first 8 columns, counted
  // over the level masks); an element the cell held goes back to the stock, the cell keeps its flag bits. Returns the cell, -1 none.
  int learn_tech(int character, int rows, int tech);
  // reset = recommended layout (the grid is cleared first), else fill the empty cells; returns cells placed. The party version clears
  // every member first and fills row by row across the members, so they share the stock in that order.
  int auto_fill(int character, bool reset);
  int auto_fill_party(bool reset);
  // The member's element cells (unlocked, below 0x80, not fixed) as a preset of slot 0.
  save::GridPreset preset_of(int character) const;
  // Loads a preset the way the editor's own moves do: Remove All first, then every preset cell that is unlocked and empty takes its
  // element from the stock through place() (a stack cell keeps the saved count, extra copies go back). An element the stock lacks
  // leaves its cell empty and is reported; no element is created or lost.
  PresetLoad apply_preset(int character, const save::GridPreset& preset, bool keep_summon_copy = false);
  PresetSummary preset_summary(const save::GridPreset& preset) const;
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
  // Bag ids below this are the materials.
  static constexpr int kFirstGearId = 0x20;
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
  // Whether a bag id belongs in the slot's list: its item kind is min(eslot, 2); an accessory list also takes every material id.
  bool listed(int item, int eslot) const;
};

}  // namespace oc::ui
