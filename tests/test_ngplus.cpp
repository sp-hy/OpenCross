// New Game+: the title variant decision, the Continue+ slot refusal, the playinit carry-over
// on a fresh game and on a loaded save, the ending's cleared-save strip (FE 85) and FE 3F's New Game+ branch.
#include <array>
#include <cstdio>

#include "data/tables.h"
#include "game/new_game.h"
#include "game/new_game_plus.h"
#include "save/cc_save.h"
#include "ui/menu/game_state.h"
#include "ui/menu/save_layout.h"

using namespace oc;
namespace ng = oc::game::ngplus;
namespace nw = oc::game::newgame;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

int count(const std::array<save::ItemStack, save::kNumItems>& stacks, int id) {
  int total = 0;
  for (const auto& stack : stacks)
    if (stack.id == id) total += stack.qty;
  return total;
}
int count_el(const std::array<save::ItemStack, save::kNumElements>& stacks, int id) {
  int total = 0;
  for (const auto& stack : stacks)
    if (stack.id == id) total += stack.qty;
  return total;
}

void test_variant() {
  // FE 59 -> mode 3 -> variant 1 (title list), FE 84 -> mode 0x30 -> variant 2 (New Game+ list), FE 85 -> mode 4 -> variant 3 (save list).
  CHECK(ng::title_variant(ng::launcher_mode_for_op(0x59)) == 1);
  CHECK(ng::title_variant(ng::launcher_mode_for_op(0x84)) == 2);
  CHECK(ng::title_variant(ng::launcher_mode_for_op(0x85)) == 3);
  CHECK(ng::title_variant(0) == 0 && ng::title_variant(1) == 0);
  // Continue+ refuses slots by the 2 summary flag bits (head 0x42).
  save::SaveSlot slot;
  CHECK(ng::refusal(slot) == ng::Refusal::None);
  slot.head[0x42] = 3;
  CHECK(ng::refusal(slot) == ng::Refusal::Completed);
  slot.head[0x42] = 2;
  CHECK(ng::refusal(slot) == ng::Refusal::AlreadyPlus);
  // Bits above the 2 flag bits are not flags.
  slot.head[0x42] = 4;
  CHECK(ng::refusal(slot) == ng::Refusal::None);
  // The battle's NG+ test reads variable 0x7E (head 0x206), not the summary.
  save::SaveSlot var_slot;
  CHECK(!var_slot.new_game_plus());
  var_slot.set_var(0x7E, 2);
  CHECK(var_slot.new_game_plus() && var_slot.ng_flag() == 2 && var_slot.head[0x206] == 2);
  // Not-carried lists.
  CHECK(!ng::carries_item(0x26) && !ng::carries_item(0x58) && !ng::carries_item(0xFD) && ng::carries_item(0x126) && ng::carries_item(1));
  CHECK(ng::carries_element(0x10) && ng::carries_element(0x24) && !ng::carries_element(0x25) && !ng::carries_element(0x30) &&
        ng::carries_element(0x31) && !ng::carries_element(0x7E) && !ng::carries_element(0));
}

save::SaveSlot cleared_game() {
  save::SaveSlot cleared;
  cleared.set_var(0x7E, 3);
  cleared.head[0x42] = 3;
  for (int id : {0, 1, 2, 10}) cleared.set_recruited(id, true);
  // unk_e1e0: Viper; unk_e1ec: a leftover bit; unk_e1e4: Orlha... (high word).
  cleared.unk_e1e0 = 1u << 5;
  cleared.unk_e1ec = 1u << 20;
  cleared.unk_e1e4 = 1u << (40 - 32);
  cleared.set_gold(9999990);
  cleared.head[save::head::kStarsTotal] = 45;
  cleared.head[save::head::kStarsCurrent] = 3;
  cleared.frames = 0x7FFF;
  cleared.set_var(0x3E0, 0x8001);
  // Key items are not carried.
  cleared.set_key_items(0xFFull);
  save::SaveChar& serge = cleared.chars[0];
  serge.max_hp = 999, serge.cur_hp = 500, serge.atk = 90, serge.mdef = 70, serge.status = 0x21, serge.hp_extra = 7, serge.bonus[3] = 9;
  // Mastermune equipped (the ending's cleared-save strip puts it in the stock; here it stays on the record and is not carried there).
  serge.weapon = 0x26;
  serge.colour_bit = 0x08;
  serge.model_id = 0x31;
  serge.grid[0] = 0x10;
  serge.unlocked = {0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x3FF};
  serge.name = {'S', 'e', 'r', 'g', 'e', 0};
  cleared.chars[1].name = {'K', 'i', 'd', 0};
  cleared.chars[5].name = {'V', 'i', 'p', 0};
  // Mastermune is not carried; an id >= 0x100 is carried (the byte table cannot match); an empty stack is skipped; the Frog Prince
  // (summon) and the Chrono Cross are not carried.
  cleared.items[0] = {0x26, 1};
  cleared.items[1] = {0x126, 3};
  cleared.items[2] = {0x05, 98};
  cleared.items[3] = {0x05, 0};
  cleared.elements[0] = {0x25, 1};
  cleared.elements[1] = {0x7E, 1};
  cleared.elements[2] = {0x10, 7};
  cleared.elements[3] = {0x31, 2};
  return cleared;
}

void test_new_game_plus() {
  const save::SaveSlot cleared = cleared_game();
  const save::SaveSlot plus = ng::new_game_plus_slot(nullptr, cleared);
  const save::SaveChar& serge = plus.chars[0];
  // records: stats from the cleared game, gear / model / innate / grid of the new game (zeros without tables), status cleared
  CHECK(serge.max_hp == 999 && serge.cur_hp == 500 && serge.atk == 90 && serge.mdef == 70 && serge.hp_extra == 7 && serge.bonus[3] == 9);
  CHECK(serge.status == 0 && serge.weapon == 0 && serge.model_id == 0 && serge.colour_bit == 0 && serge.grid[0] == 0);
  CHECK(serge.unlocked[7] == 0x3FF);
  // every name carried (nobody recruited while playinit runs)
  CHECK(plus.chars[0].name[0] == 'S' && plus.chars[1].name[0] == 'K' && plus.chars[5].name[0] == 'V');
  // items / elements
  CHECK(count(plus.items, 0x26) == 0 && count(plus.items, 0x126) == 3 && count(plus.items, 0x05) == 98);
  CHECK(count_el(plus.elements, 0x25) == 0 && count_el(plus.elements, 0x7E) == 0 && count_el(plus.elements, 0x10) == 7 && count_el(plus.elements, 0x31) == 2);
  // The leftover word = cleared word | recruited | Viper word.
  CHECK(plus.unk_e1ec == ((1u << 20) | 1u | 2u | 4u | (1u << 10) | (1u << 5)));
  CHECK(plus.unk_e1f0 == (1u << 8));
  CHECK(plus.unk_e1e0 == 0 && plus.unk_e1e4 == 0);
  // gold 100 (new game) + cleared, capped
  CHECK(plus.gold() == 9999999);
  CHECK(plus.stars_total() == 45 && plus.stars_current() == 3 && plus.head[save::head::kPreviewStars] == 45);
  CHECK(plus.frames == 0x7FFF);
  // The new-game table | the cleared pool.
  CHECK((plus.var(0x3E0) & 0x8001) == 0x8001 && (plus.var(0x3E0) & 0x40A) == 0x40A);
  CHECK(plus.ng_flag() == 2 && plus.summary_flags() == 2 && plus.new_game_plus());
  CHECK(plus.key_items() == (ng::kKeyReliefCharm | ng::kKeyTimeShifter));
  // After New Game+: party bytes FF FF FF, MAP_ID (variable 6 = the location word) 0.
  CHECK(plus.recruited(0) && !plus.recruited(1) && plus.party()[0] == 0xFF && plus.party()[1] == 0xFF);
  // Around playinit: relief 0xFF, default pad tables, element nibbles 5, real (not dream) records.
  CHECK(plus.head[0x59F] == 0xFF && plus.button_cfg == nw::kDefaultButtons && plus.chars[3].equipped_qty[0] == nw::kEquippedFill && !plus.dream_records);
  CHECK(plus.playtime_frames() == 0 && plus.location() == 0 && plus.var(6) == 0 && plus.var(0x42) == 0 && plus.var(0x3D2) == 0x3F);
  // the save image keeps all of it
  const save::SaveSlot reloaded = save::decode_block(save::encode_block(plus));
  CHECK(reloaded.unk_e1ec == plus.unk_e1ec && reloaded.unk_e1f0 == plus.unk_e1f0 && reloaded.ng_flag() == 2 && reloaded.summary_flags() == 2 && reloaded.chars[0].atk == 90);
}

void test_continue_plus() {
  const save::SaveSlot cleared = cleared_game();
  // An ordinary save: Serge + Kid recruited, own names.
  save::SaveSlot loaded;
  loaded.set_recruited(0, true);
  loaded.set_recruited(1, true);
  loaded.set_gold(1000);
  loaded.chars[0].name = {'A', 0};
  loaded.chars[1].name = {'B', 0};
  loaded.chars[0].weapon = 0x10;
  loaded.set_key_items(1);
  ng::continue_plus(cleared, loaded);
  // Only the non-recruited names are carried.
  CHECK(loaded.chars[0].name[0] == 'A' && loaded.chars[1].name[0] == 'B' && loaded.chars[5].name[0] == 'V');
  CHECK(loaded.chars[0].weapon == 0x10 && loaded.chars[0].atk == 90);
  CHECK(loaded.gold() == 9999999);
  CHECK(loaded.key_items() == (1ull | ng::kKeyTimeEgg | ng::kKeyReliefCharm | ng::kKeyTimeShifter));
  // The roster is the loaded save's.
  CHECK(loaded.recruited(0) && loaded.recruited(1) && !loaded.recruited(2));
  CHECK(loaded.ng_flag() == 2);
}

void test_strip_and_fe3f() {
  // No rules: every grid element returns as one copy.
  ui::GameState gs;
  save::SaveSlot& slot = gs.slot;
  slot.set_recruited(0, true);
  // Guile is parked in the unk_e1e0 word.
  slot.unk_e1e0 = 1u << 2;
  slot.chars[0].weapon = 0x40;
  slot.chars[0].accessory[1] = 0x90;
  slot.chars[0].grid[3] = 0x12;
  // A tech (id >= 0x80) stays.
  slot.chars[0].grid[4] = 0xA3;
  slot.chars[2].armour = 0x50;
  // Character 7 is not in the roster: untouched.
  slot.chars[7].weapon = 0x41;
  slot.chars[2].grid[0] = 0x13, slot.chars[2].equipped_qty[45] = 3, slot.chars[1].equipped_qty[40] = 4;
  slot.chars[2].grid[1] = 0x4014, slot.chars[2].grid[2] = 0x8000 | 0x15;
  slot.chars[2].accessory[0] = 0x261, slot.chars[2].accessory[1] = 0x270, slot.chars[2].accessory[2] = 0x405;
  ng::strip_for_cleared_save(gs);
  CHECK(slot.chars[2].grid[0] == 0 && slot.chars[2].equipped_qty[45] == 0 && slot.chars[1].equipped_qty[40] == 4 && count_el(slot.elements, 0x13) == 1);
  CHECK(slot.chars[2].grid[1] == 0x4000 && slot.chars[2].grid[2] == 0x8000);
  CHECK(count_el(slot.elements, 0x61) == 5 && count_el(slot.elements, 0x70) == 1 && (slot.key_items() >> 5 & 1) == 1);
  CHECK(slot.chars[0].weapon == 0 && slot.chars[0].accessory[1] == 0 && slot.chars[0].grid[3] == 0 && slot.chars[0].grid[4] == 0xA3);
  CHECK(slot.chars[2].armour == 0 && slot.chars[7].weapon == 0x41);
  CHECK(count(slot.items, 0x40) == 1 && count(slot.items, 0x90) == 1 && count(slot.items, 0x50) == 1 && count(slot.items, 0x41) == 0);
  CHECK(count_el(slot.elements, 0x12) == 1);
  // New Game+ branch: the dream roster is dropped, Relief Charm + Time Shifter, the carried data stays
  slot.set_recruited(1, true);
  slot.set_key_items(ng::kKeyTimeEgg);
  slot.chars[0].atk = 77;
  ng::fe3f(gs, true);
  CHECK(slot.recruited(0) && !slot.recruited(1) && slot.unk_e1e0 == 0 && slot.unk_e1b8[0] == 1);
  CHECK(slot.key_items() == (ng::kKeyReliefCharm | ng::kKeyTimeShifter));
  CHECK(slot.chars[0].atk == 77 && count(slot.items, 0x40) == 1);
  // Normal branch: fresh data, gold 100; key items = the Time Shifter.
  ng::fe3f(gs, false);
  CHECK(slot.key_items() == ng::kKeyTimeShifter && slot.gold() == 100 && count(slot.items, 0x40) == 0 && slot.chars[0].atk == 0 && slot.frames == 0x203);
  CHECK(slot.chars[0].unlocked == nw::kInitialUnlocks && slot.chars[0].equipped_qty[47] == 5 && slot.stars_total() == 0 && slot.var(0x3E0) == 0x40A);
}

// Synthetic char.bin / char20.bin / lv99para / el_mask for the new-game routines (game/new_game.h).
data::GameTables make_tables() {
  data::GameTables tables;
  data::CharSet real, dream;
  for (size_t ch = 0; ch < static_cast<size_t>(data::kNumChars); ++ch) {
    auto& real_raw = real.rec[ch].raw;
    real_raw[0x01] = 0x08, real_raw[0x03] = static_cast<u8>(ch % 2), real_raw[0x16] = 50, real_raw[0x18] = 9, real_raw[0x19] = 5, real_raw[0x1A] = 85, real_raw[0x1B] = 8, real_raw[0x1C] = 7, real_raw[0x1D] = 7;
    real_raw[0x20] = 0x20, real_raw[0x2D] = static_cast<u8>(ch), real_raw[0x40] = 0x69, real_raw[0x41] = 0x40, real_raw[0xA8] = 3;
    // Level 1 opens columns 2..9.
    real_raw[0x30] = 0xFC, real_raw[0x31] = 0x03;
    auto& dream_raw = dream.rec[ch].raw;
    dream_raw = real_raw;
    // The dream's level 1 opens columns 5..7.
    dream_raw[0x16] = 243, dream_raw[0x18] = 26, dream_raw[0x40] = 0x19, dream_raw[0x30] = 0xE0, dream_raw[0x31] = 0;
    real.lv99[ch] = dream.lv99[ch] = {850, 88, 78, 95, 25, 52, 43};
  }
  tables.char_sets.emplace("char", real);
  tables.char_sets.emplace("char20", dream);
  tables.growth.assign(100, std::array<u8, 8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  // Column 0 of the el_mask.bin rows.
  tables.growth[1][0] = 7, tables.growth[2][0] = 17, tables.growth[3][0] = 27, tables.growth[19][0] = 8, tables.growth[20][0] = 36;
  tables.growth[1][1] = 5;
  return tables;
}

void test_new_game_state() {
  const data::GameTables tables = make_tables();
  // Boot / title = the dream: char20 records, 20 stars, unlocks of stars 0..19, growth for 20 stars, nibbles 5.
  const save::SaveSlot boot = nw::boot_state(&tables, 7);
  const save::SaveChar& serge = boot.chars[0];
  CHECK(boot.dream_records && serge.max_hp == 243 && serge.cur_hp == 243 && serge.atk == 26 && serge.grid[0] == 0x19 && serge.weapon == 0x20 && serge.rec_a8 == 3);
  CHECK(boot.stars_total() == 20 && boot.stars_current() == 20 && boot.head[0x59F] == 0xFF && boot.gold() == 100);
  // Unlocks 7, 17, 27, 8; not 36 (star 20).
  CHECK(serge.unlocked[0] == (0x40 | 0x80 | 0x100) && serge.unlocked[1] == (0x40 | 0x80) && serge.unlocked[2] == (0x40 | 0x80) && serge.unlocked[3] == 0);
  // Column 1: 5.
  CHECK(boot.chars[1].unlocked[0] == (0x40 | 0x20));
  CHECK(serge.equipped_qty[0] == 5 && serge.hp_extra == 243 && serge.bonus[0] == 26 && serge.bonus[2] == 85 && serge.bonus[4] == 5 && serge.unk_be >= 2);
  CHECK(boot.key_items() == nw::kKeyTimeShifter && boot.recruited(0) && !boot.recruited(1) && boot.party()[0] == 0 && boot.party()[1] == 0xFF);
  CHECK(boot.head[0x5A0] == 2 && boot.head[0x5A1] == 0 && boot.button_cfg == nw::kDefaultButtons && boot.playtime_frames() == 0 && boot.frames == 0x203);
  CHECK(boot.var(0x3D2) == 0x3F && boot.var(0x3E0) == 0x40A && boot.var(0x3E2) == 0x404 && boot.var(0x3E6) == 0x2060 && boot.var(0x3E8) == 0x80 && boot.var(0x3F0) == 0x2000);
  CHECK(boot.var(0) == 0 && boot.var(0x42) == 0 && boot.unk_e1ec == 0 && boot.unk_e1b8[0] == 1);
  // The menu / battle read the dream's record set for the grid layout.
  ui::MenuRules rules;
  rules.tables = &tables;
  ui::GameState gs(boot, &rules);
  // The char20.bin level masks apply.
  CHECK(!gs.cell_exists(0, 0, 2) && gs.cell_exists(0, 0, 5));
  CHECK(gs.cell_available(0, 0, 6) && gs.cell_available(0, 0, 7) && !gs.cell_available(0, 0, 2));
  // Normal branch: the real records (names kept), 0 stars, the initial masks only, growth for 0 stars.
  gs.slot.chars[0].name = {'Z', 0};
  gs.slot.set_recruited(1, true);
  ng::fe3f(gs, false, 9);
  const save::SaveChar& fresh = gs.slot.chars[0];
  // The char.bin level masks apply.
  CHECK(!gs.slot.dream_records && gs.cell_exists(0, 0, 2));
  CHECK(fresh.max_hp == 50 && fresh.atk == 9 && fresh.grid[0] == 0x69 && fresh.name[0] == 'Z' && fresh.unlocked == nw::kInitialUnlocks && fresh.equipped_qty[3] == 5);
  CHECK(gs.slot.stars_total() == 0 && !gs.slot.recruited(1) && gs.slot.key_items() == nw::kKeyTimeShifter && gs.slot.gold() == 100);
  CHECK(fresh.hp_extra == 50 && fresh.bonus[0] == 9 && fresh.unk_be >= 2);
  CHECK(gs.cell_available(0, 0, 6) && !gs.cell_available(0, 0, 7));
  // New Game+: after the carry the unlock step ORs the star unlocks of stars 0 .. stars - 1 into the copied masks.
  save::SaveSlot cleared = cleared_game();
  cleared.head[save::head::kStarsTotal] = 3;
  cleared.chars[0].unlocked = {0x40, 0, 0, 0, 0, 0, 0, 0};
  const save::SaveSlot plus = ng::new_game_plus_slot(&tables, cleared);
  // Unlocks 7, 17 (stars 0..2).
  CHECK(plus.chars[0].unlocked[0] == (0x40 | 0x80) && plus.chars[0].unlocked[1] == 0x80 && plus.chars[0].unlocked[2] == 0);
  // New game gear / grid, carried stats.
  CHECK(plus.chars[0].weapon == 0x20 && plus.chars[0].grid[0] == 0x69 && plus.chars[0].atk == 90 && !plus.dream_records);
  // End of playinit: Pip in a form re-rolls his pending growth against lv99para record <form>; Continue+ ORs the host copy of the
  // unlock word, taken while memory still holds the cleared game: the loaded save's own Pip masks do not come back.
  save::SaveSlot pc = cleared_game();
  pc.head[save::head::kStarsTotal] = 10;
  save::SaveChar& pip = pc.chars[nw::kPip];
  // The def snapshot 90 >= target 78: pending 0.
  pip.hp_extra = 400, pip.bonus[0] = 30, pip.bonus[1] = 99, pip.bonus[4] = 90;
  save::SaveSlot ld;
  ld.chars[nw::kPip].model_id = 0x2E;
  ld.chars[nw::kPip].unlocked = {0, 0x200, 0, 0, 0, 0, 0, 0x1};
  ng::continue_plus(pc, ld, &tables);
  const save::SaveChar& loaded_pip = ld.chars[nw::kPip];
  CHECK(loaded_pip.unk_be <= (850 - 400) / 89 && loaded_pip.bonus[1] <= (88 - 30) / 89 && loaded_pip.bonus[5] == 0);
  CHECK(!(loaded_pip.unlocked[1] & 0x200) && !(loaded_pip.unlocked[7] & 0x1));
  CHECK(nw::growth_column(&tables, ld, nw::kPip, false) == tables.char_sets.at("char").rec[0x2E].growth_col());
  // Continue+ carries the cleared game's Pip as he is, weaker or stronger than the loaded one, with the cleared game's grid; the loaded
  // save keeps its form byte (RDE: cleared Pip 300 HP / ATK 20 over a loaded 900 HP / ATK 90 one -> 300 / 20; cleared Demonic Beast
  // masks over a base Pip -> the cleared masks, form 0x2B).
  save::SaveSlot weak = cleared_game();
  weak.head[save::head::kStarsTotal] = 60;
  weak.chars[nw::kPip].model_id = 0x2E;
  weak.chars[nw::kPip].max_hp = 300, weak.chars[nw::kPip].atk = 20;
  weak.chars[nw::kPip].unlocked = {0x3FC, 0x3FC, 0x1F8, 0x1F8, 0x1F0, 0xE0, 0xC0, 0xC0};
  save::SaveSlot strong;
  strong.chars[nw::kPip].model_id = 0x2B;
  strong.chars[nw::kPip].max_hp = 900, strong.chars[nw::kPip].atk = 90;
  ng::continue_plus(weak, strong, &tables);
  CHECK(strong.chars[nw::kPip].max_hp == 300 && strong.chars[nw::kPip].atk == 20 && strong.chars[nw::kPip].model_id == 0x2B);
  CHECK(strong.chars[nw::kPip].unlocked[0] == 0x3FC && strong.chars[nw::kPip].unlocked[7] == 0xC0);
  // RDE's load hook: every loaded game has the Time Shifter.
  save::SaveSlot loaded;
  nw::ensure_time_shifter(loaded);
  CHECK(loaded.key_items() == nw::kKeyTimeShifter);
}

void test_add_stack() {
  std::array<save::ItemStack, 4> stacks{};
  // A slot with quantity 0 is free.
  stacks[0] = {7, 0};
  CHECK(ng::add_stack(stacks, 5, 60) && ng::add_stack(stacks, 5, 60));
  CHECK(stacks[0].id == 5 && stacks[0].qty == 99);
  CHECK(ng::add_stack(stacks, 9, 1) && stacks[1].id == 9);
  CHECK(!ng::add_stack(stacks, 0, 1));
}
}  // namespace

// Save grid boxes: a file in its own box shows its face; a lone CHRONO01 in box 0 shows the cursor's face
// (Serge with the cursor on box 0, Kid on box 1); the caption is the file number + 1.
void test_box_icons() {
  namespace sl = oc::ui::save_layout;
  CHECK(sl::box_icon(3, 3, 0, 15) == 3);
  CHECK(sl::box_icon(0, 1, 0, 2) == 0 && sl::box_icon(0, 1, 1, 2) == 1);
  // cursor + 1 > the largest n: own face.
  CHECK(sl::box_icon(0, 1, 5, 2) == 1);
  CHECK(sl::file_number(1) == "  2");
  // Saving screen: card holding files 00 01 02 04 in boxes 0..3. A new save on the empty box 5 shows file 03's face there; an
  // overwrite of box 2 (after 03 was written) draws file 05, whose rule gives the cursor's own face; a full card draws nothing.
  std::array<bool, 15> used{};
  used[0] = used[1] = used[2] = used[4] = true;
  CHECK(sl::saving_number(used) == 3 && sl::saving_icon(5, 3, 5) == 3);
  used[3] = true;
  CHECK(sl::saving_number(used) == 5 && sl::saving_icon(2, 5, 5) == 2);
  used.fill(true);
  CHECK(sl::saving_number(used) == -1);
}

int main() {
  test_box_icons();
  test_variant();
  test_new_game_plus();
  test_continue_plus();
  test_strip_and_fe3f();
  test_new_game_state();
  test_add_stack();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("ngplus: all passed\n");
  return failures ? 1 : 0;
}
