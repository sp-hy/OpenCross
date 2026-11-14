#include "game/new_game_plus.h"

#include <algorithm>

#include "data/tables.h"
#include "game/new_game.h"
#include "ui/menu/game_state.h"

namespace oc::game::ngplus {

int title_variant(int launcher_mode) {
  switch (launcher_mode) {
    case 3: return 1;
    case 4: return 3;
    case 0x30: return 2;
    default: return 0;
  }
}

int launcher_mode_for_op(int fe_op) {
  switch (fe_op) {
    case 0x59: return 3;
    case 0x84: return 0x30;
    case 0x85: return 4;
    default: return -1;
  }
}

bool carries_item(int id) {
  if (id <= 0) return false;
  for (u8 banned : kNotCarriedItems)
    if (banned == id) return false;
  return true;
}

bool carries_element(const std::vector<data::ElemRec>& elements, int id) {
  const data::ElementKind kind = data::element_kind(elements, id);
  return kind != data::ElementKind::Summon && kind != data::ElementKind::Other;
}

bool add_stack(std::span<save::ItemStack> stock, int id, int count) {
  if (id <= 0) return false;
  int at = -1;
  for (size_t i = 0; i < stock.size() && at < 0; ++i)
    if (stock[i].id == id && stock[i].qty != 0) at = static_cast<int>(i);
  for (size_t i = 0; i < stock.size() && at < 0; ++i)
    if (stock[i].id == 0 || stock[i].qty == 0) {
      stock[i].qty = 0;
      at = static_cast<int>(i);
    }
  if (at < 0) return false;
  save::ItemStack& stack = stock[static_cast<size_t>(at)];
  stack.id = static_cast<u16>(id);
  stack.qty = static_cast<u8>(std::min(99, stack.qty + count));
  if (stack.qty == 0) stack.id = 0;
  return true;
}

namespace {
u32 rd32(const save::SaveSlot& slot, size_t offset) {
  return static_cast<u32>(slot.head[offset]) | (static_cast<u32>(slot.head[offset + 1]) << 8) |
         (static_cast<u32>(slot.head[offset + 2]) << 16) | (static_cast<u32>(slot.head[offset + 3]) << 24);
}
}  // namespace

// Character records: the whole record from the cleared game, then the saved fields of the new game put back (innate colour, gear,
// model byte, grid); the field status byte is cleared and the equipped nibbles are not part of the record and stay the new game's.
// The carry-over bitmaps are assigned (the new game's value is dropped). The star unlocks ORed in afterwards are a subset of a
// cleared game's masks; the grid lock bits are derived from the masks (GameState::cell_available).
void carry_over(const save::SaveSlot& cleared, save::SaveSlot& dst, const data::GameTables* tables) {
  std::vector<int> both;
  for (const int char_id : dst.character_ids())
    if (cleared.is_character(char_id)) both.push_back(char_id);
  for (const int char_id : both) {
    const save::SaveChar& src = cleared.chars[static_cast<size_t>(char_id)];
    save::SaveChar& rec = dst.chars[static_cast<size_t>(char_id)];
    rec.rec02 = src.rec02;
    rec.rec_a8 = src.rec_a8;
    rec.status = 0;
    rec.max_hp = src.max_hp;
    rec.cur_hp = src.cur_hp;
    rec.hp_extra = src.hp_extra;
    rec.unk_be = src.unk_be;
    rec.atk = src.atk, rec.def = src.def, rec.hit = src.hit, rec.evd = src.evd, rec.mag = src.mag, rec.mdef = src.mdef;
    rec.bonus = src.bonus;
  }
  for (const save::ItemStack& item : cleared.items)
    if (item.qty != 0 && carries_item(item.id)) add_stack(dst.items, item.id, item.qty);
  if (tables)
    for (const save::ItemStack& element : cleared.elements)
      if (element.qty != 0 && carries_element(tables->elements, element.id)) add_stack(dst.elements, element.id, element.qty);
  dst.unk_e1ec = cleared.unk_e1ec | rd32(cleared, save::head::kRecruited) | cleared.unk_e1e0;
  dst.unk_e1f0 = (cleared.unk_e1f0 | rd32(cleared, save::head::kRecruited + 4) | cleared.unk_e1e4) & 0xFFFu;
  for (const int char_id : both)
    if (char_id >= save::kNumChars)
      dst.set_char_flag(char_id, save::kCharCarried, cleared.char_flag(char_id, save::kCharCarried) || cleared.recruited(char_id) ||
                                                         cleared.char_flag(char_id, save::kCharRosterSaved));
  dst.set_gold(std::min<u32>(dst.gold() + cleared.gold(), kGoldCap));
  for (const int char_id : both)
    if (!dst.recruited(char_id) && !dst.char_flag(char_id, save::kCharRosterSaved))
      dst.chars[static_cast<size_t>(char_id)].name = cleared.chars[static_cast<size_t>(char_id)].name;
  for (const int char_id : both) dst.chars[static_cast<size_t>(char_id)].unlocked = cleared.chars[static_cast<size_t>(char_id)].unlocked;
  dst.head[save::head::kStarsTotal] = cleared.head[save::head::kStarsTotal];
  dst.head[save::head::kStarsCurrent] = cleared.head[save::head::kStarsCurrent];
  dst.head[save::head::kPreviewStars] = cleared.head[save::head::kStarsTotal];
  if (tables) newgame::star_unlocks(tables, dst, false, false);
  dst.frames = static_cast<u16>(dst.frames | cleared.frames);
  for (int id = 0x3E0; id < 0x400; id += 2) dst.set_var(id, static_cast<u16>(dst.var(id) | cleared.var(id)));
  dst.set_var(kFlagVar, kFlagPlus);
  dst.head[save::head::kFlags42] = static_cast<u8>((dst.head[save::head::kFlags42] & ~3u) | kFlagPlus);
  dst.set_key_items(dst.key_items() | kKeyReliefCharm | kKeyTimeShifter);
}

// RDE copies Pip's masks from the CLEARED game (RAM still holds it the frame after the slot pick, before the card read); the
// carry-over then copies the same masks, so the OR of rde_playinit_end adds nothing.
void continue_plus(const save::SaveSlot& cleared, save::SaveSlot& loaded, const data::GameTables* tables) {
  const std::array<u16, 8> pip = cleared.chars[newgame::kPip].unlocked;
  carry_over(cleared, loaded, tables);
  newgame::rde_playinit_end(tables, loaded, &pip, loaded.playtime_frames() | 1u);
  loaded.set_key_items(loaded.key_items() | kKeyTimeEgg);
}

void fresh_record(const data::GameTables* tables, int id, save::SaveChar& out) {
  out = save::SaveChar{};
  out.unlocked = newgame::kInitialUnlocks;
  if (!tables || id < 0) return;
  auto it = tables->char_sets.find("char");
  if (it == tables->char_sets.end() || static_cast<size_t>(id) >= it->second.rec.size()) return;
  const data::StatRec& file_rec = it->second.rec[static_cast<size_t>(id)];
  out.colour_bit = file_rec.innate();
  out.max_hp = file_rec.max_hp();
  out.cur_hp = file_rec.max_hp();
  out.atk = file_rec.atk(), out.def = file_rec.def(), out.hit = file_rec.hit(), out.evd = file_rec.evd(), out.mag = file_rec.mag(),
  out.mdef = file_rec.mdef();
  out.weapon = file_rec.weapon();
  out.armour = file_rec.armour();
  for (int i = 0; i < 3; ++i) out.accessory[static_cast<size_t>(i)] = file_rec.accessory(i);
  out.model_id = file_rec.model();
  for (int k = 0; k < 48; ++k) out.grid[static_cast<size_t>(k)] = file_rec.cell_listed(k) & 0x3FFF;
  out.unlocked = newgame::kInitialUnlocks;
}

// The header frame / icon / title stay; the summary (party, time, gold, stars, progress, flags) is cleared; DEBUG_MODE (variable
// 0x34) is kept. MAP_ID (variable 6) ends 0 and the party is all-empty at the name entry (FE 3F later sets {Serge, -, -}). Serge
// is un-recruited around the carry so that every name is carried.
save::SaveSlot new_game_plus_slot(const data::GameTables* tables, const save::SaveSlot& cleared) {
  const u32 seed = cleared.playtime_frames() | 1u;
  save::SaveSlot slot = newgame::boot_state(tables, seed);
  std::copy(cleared.head.begin(), cleared.head.begin() + 0x180, slot.head.begin());
  std::fill(slot.head.begin() + save::head::kPreviewParty, slot.head.begin() + 0x44, u8{0});
  slot.tail = cleared.tail;
  slot.set_var(0x34, cleared.var(0x34));
  slot.set_var(6, 0);
  slot.set_party({-1, -1, -1});
  newgame::real_records(tables, slot, seed);
  slot.set_recruited(0, false);
  carry_over(cleared, slot, tables);
  newgame::rde_playinit_end(tables, slot, nullptr, seed);
  slot.set_recruited(0, true);
  slot.set_var(0x42, 0);
  return slot;
}

// Every recruited or parked character gives its grid elements (GameState::strip_grid) and its five gear words back to the stock.
void strip_for_cleared_save(ui::GameState& gs) {
  save::SaveSlot& sl = gs.slot;
  for (const int char_id : sl.character_ids()) {
    if (!sl.recruited(char_id) && !sl.char_flag(char_id, save::kCharRosterSaved)) continue;
    gs.strip_grid(char_id);
    save::SaveChar& rec = sl.chars[static_cast<size_t>(char_id)];
    for (u16* gear : {&rec.weapon, &rec.armour, &rec.accessory[0], &rec.accessory[1], &rec.accessory[2]}) gs.return_gear(*gear);
  }
}

std::array<u16, 24> new_game_globals() { return newgame::globals(); }

void fe3f(ui::GameState& gs, bool ng_plus, u32 seed) {
  save::SaveSlot& sl = gs.slot;
  const data::GameTables* tables = gs.rules ? gs.rules->tables : nullptr;
  if (!ng_plus) {
    newgame::real_records(tables, sl, seed);
    newgame::reset_party(sl);
    sl.frames = newgame::kNewGameFrames;
    return;
  }
  newgame::reset_party(sl);
  sl.set_key_items(sl.key_items() | kKeyReliefCharm | kKeyTimeShifter);
}

}  // namespace oc::game::ngplus
