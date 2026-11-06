#include "game/game_session.h"

#include <algorithm>
#include <array>

#include "battle/rng.h"
#include "battle/rules.h"
#include "core/log.h"
#include "game/new_game.h"

namespace oc::game {

namespace {

constexpr size_t kStarOff = save::head::kStarsTotal, kCounterOff = save::head::kStarsCurrent;

// bonus[] layout: C0 snap ATK, C1 pending ATK, C2 snap HIT, C3 pending HIT, C4/C5 DEF, C6/C7 EVD, C8/C9 MAG, CA/CB MDEF.
// Stat order (PartyMember): HP, ATK, DEF, HIT, EVD, MAG, MDEF.
constexpr int kBonusIndex[7] = {-1, 0, 4, 2, 6, 8, 10};

int stat_of(const save::SaveChar& save_char, int k) {
  switch (k) {
    case 0: return save_char.max_hp;
    case 1: return save_char.atk;
    case 2: return save_char.def;
    case 3: return save_char.hit;
    case 4: return save_char.evd;
    case 5: return save_char.mag;
    default: return save_char.mdef;
  }
}

void set_stat(save::SaveChar& save_char, int k, int value) {
  switch (k) {
    case 0: save_char.max_hp = static_cast<u16>(std::clamp(value, 1, 999)); break;
    case 1: save_char.atk = static_cast<u8>(std::clamp(value, 0, 127)); break;
    case 2: save_char.def = static_cast<u8>(std::clamp(value, 0, 127)); break;
    case 3: save_char.hit = static_cast<u8>(std::clamp(value, 0, 127)); break;
    case 4: save_char.evd = static_cast<u8>(std::clamp(value, 0, 127)); break;
    case 5: save_char.mag = static_cast<u8>(std::clamp(value, 0, 127)); break;
    default: save_char.mdef = static_cast<u8>(std::clamp(value, 0, 127)); break;
  }
}

void read_growth(const save::SaveChar& save_char, battle::PartyMember& member) {
  member.snap[0] = save_char.hp_extra;
  member.pending[0] = save_char.unk_be;
  for (int k = 1; k < 7; ++k) {
    member.snap[static_cast<size_t>(k)] = save_char.bonus[static_cast<size_t>(kBonusIndex[k])];
    member.pending[static_cast<size_t>(k)] = save_char.bonus[static_cast<size_t>(kBonusIndex[k] + 1)];
  }
}

void write_growth(save::SaveChar& save_char, const battle::PartyMember& member) {
  save_char.hp_extra = static_cast<u16>(std::clamp(member.snap[0], 0, 999));
  save_char.unk_be = static_cast<u16>(std::clamp(member.pending[0], 0, 999));
  for (int k = 1; k < 7; ++k) {
    save_char.bonus[static_cast<size_t>(kBonusIndex[k])] = static_cast<u8>(std::clamp(member.snap[static_cast<size_t>(k)], 0, 127));
    save_char.bonus[static_cast<size_t>(kBonusIndex[k] + 1)] = static_cast<u8>(std::clamp(member.pending[static_cast<size_t>(k)], 0, 127));
  }
}

}  // namespace

void settle_pip_form(ui::GameState& gs, const data::GameTables& tables, int form_before) {
  const int form = gs.slot.chars[static_cast<size_t>(newgame::kPip)].model_id;
  if (form == form_before || !battle::pip_form(form) || !battle::pip_form(form_before)) return;
  newgame::pip_form_stats(&tables, gs.slot);
  newgame::pip_form_unlocks(&tables, gs.slot);
}

std::string utf8_from_u32(std::u32string_view text) {
  std::string out;
  for (char32_t code_point : text) {
    if (code_point < 0x80) {
      out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
      out += static_cast<char>(0xC0 | (code_point >> 6));
      out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
      out += static_cast<char>(0xE0 | (code_point >> 12));
      out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code_point >> 18));
      out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
  }
  return out;
}

int session_star(const ui::GameState& gs) { return std::min<int>(gs.slot.head[kStarOff], 99); }

namespace {
int record_body(const ui::GameState& gs, int char_id) {
  return data::character_body(gs.slot.chars[static_cast<size_t>(char_id)].model_id, [&](int var) { return gs.slot.var(var); });
}
}  // namespace

battle::PartyMember party_member_from_state(const data::GameTables& tables, const ui::GameState& gs, int char_id,
                                            const data::CharNames* names) {
  const int star = session_star(gs);
  // The record set of the state: the dream (char20.bin) until FE 3F, else char.bin (SaveSlot::dream_records, game/new_game.h).
  const bool dream = gs.slot.dream_records && tables.char_sets.count("char20") != 0;
  battle::PartyMember member = battle::make_party_member(tables, char_id, star, dream ? "char20" : "char");
  if (char_id < 0 || char_id >= save::kNumChars) return member;
  // Pip's forms: the evolution rewrites only the record's growth column (3 3 3 4 5 6 for kinds 0x2B..0x30 = char.bin 43..48) and
  // the 2-3 tech cells; the grid layout stays record 43's (identical in 43..48). The kind comes from the record's model id.
  member.growth_col = static_cast<u8>(newgame::growth_column(&tables, gs.slot, char_id, dream));
  if (gs.slot.chars[static_cast<size_t>(char_id)].max_hp > 0) member.kind = gs.slot.chars[static_cast<size_t>(char_id)].model_id;
  // Pip in a form grows toward that form's Lv 99 record: the growth clamp and the star-up's next pending roll.
  if (char_id == newgame::kPip && battle::pip_form(member.kind)) {
    member.lv99 = battle::lv99_targets(tables, member.kind);
    const data::StatRec& form_rec = tables.chars(dream ? "char20" : "char").rec[static_cast<size_t>(member.kind)];
    member.base = {form_rec.max_hp(), form_rec.atk(), form_rec.def(), form_rec.hit(), form_rec.evd(), form_rec.mag(), form_rec.mdef()};
  }
  const save::SaveChar& save_char = gs.slot.chars[static_cast<size_t>(char_id)];
  // A record that was never filled keeps the table defaults.
  if (save_char.max_hp > 0) {
    for (int k = 0; k < 7; ++k) member.stat[static_cast<size_t>(k)] = stat_of(save_char, k);
    member.hp = std::min<int>(save_char.cur_hp, member.stat[0]);
    if (save_char.colour_bit) member.innate = save_char.colour_bit;
    member.eq = battle::make_equip(tables, save_char.weapon, save_char.armour, save_char.accessory);
    // Grid: the saved element per cell, open exactly where the saved masks open it (what the menu shows).
    member.cells.fill(0);
    member.unlocked.fill(false);
    for (int row = 0; row < ui::GameState::kRows; ++row)
      for (int col = 0; col < ui::GameState::kCols; ++col) {
        const size_t cell = static_cast<size_t>(row * ui::GameState::kCols + col);
        if (cell >= member.cells.size()) continue;
        member.cells[cell] = static_cast<u16>(gs.cell_element(char_id, row, col));
        member.unlocked[cell] = gs.cell_available(char_id, row, col) && member.cells[cell] != 0;
      }
    for (int row = 0; row < ui::GameState::kRows; ++row)
      for (int col = 0; col < ui::GameState::kCols; ++col) {
        const size_t cell = static_cast<size_t>(row * ui::GameState::kCols + col);
        if (cell < member.qty.size() && (member.cells[cell] & 0x3FFF) != 0 && (member.cells[cell] & 0x3FFF) < tables.elements.size() && (tables.elements[member.cells[cell] & 0x3FFF].flags0() & 0x10)) {
          member.qty[cell] = static_cast<u8>(gs.cell_count(char_id, row, col));
          member.stack[cell] = true;
        }
      }
    member.lo_persist = save_char.status & battle::st::kPersist;
    read_growth(save_char, member);
    const bool fresh = save_char.hp_extra == 0 && save_char.unk_be == 0 && std::all_of(save_char.bonus.begin(), save_char.bonus.end(), [](u8 bonus) { return bonus == 0; });
    if (fresh) {
      member.snap = member.stat;
      battle::GameRng rng(0x5EED0000u + static_cast<u32>(char_id) * 977u);
      member.pending[0] = battle::next_pending_hp(rng, member.stat[0], member.lv99[0], star);
      for (size_t k = 1; k < 7; ++k) member.pending[k] = battle::next_pending_stat(rng, member.stat[k], member.lv99[k], star);
    }
  }
  const int body = record_body(gs, char_id);
  if (names && names->tables) {
    member.own_name = utf8_from_u32(data::own_name(char_id, *names));
    member.name = utf8_from_u32(data::character_name(char_id, body, data::NameContext::Menu, false, *names));
  } else if (data::shows_lynx(char_id, body, data::NameContext::Menu, false)) {
    member.name = tables.char_name(data::kLynxBody);
  }
  return member;
}

std::vector<std::string> duel_names(const ui::GameState& gs, const data::CharNames& names) {
  std::vector<std::string> out(data::kLynxBody + 1);
  for (int id = 0; id <= data::kLastNamedChar; ++id)
    out[static_cast<size_t>(id)] = utf8_from_u32(data::character_name(id, record_body(gs, id), data::NameContext::Menu, false, names));
  if (names.tables) out[data::kLynxBody] = utf8_from_u32(names.tables->default_name(data::kLynxBody, names.lang));
  return out;
}

BattleSetup battle_setup(const data::GameTables& tables, const ui::GameState& gs, const std::vector<int>& party_ids, int progress,
                         const data::CharNames* names) {
  BattleSetup out;
  out.ng_plus = gs.slot.new_game_plus();
  out.growth.star = session_star(gs);
  out.growth.progress = progress;
  out.growth.counter = gs.slot.head[kCounterOff];
  for (int char_id = 0; char_id < save::kNumChars; ++char_id) {
    if (std::find(party_ids.begin(), party_ids.end(), char_id) != party_ids.end()) continue;
    if (gs.slot.chars[static_cast<size_t>(char_id)].max_hp == 0) continue;
    out.bench.push_back(party_member_from_state(tables, gs, char_id, names));
  }
  return out;
}

namespace {

// The star-up's grid unlocks: in every record, recruited or not, the el_mask cells of stars 0 .. `star` (the new star) that the
// record's grid has are opened in the save's per-row masks, whatever the cells hold.
void star_up_unlocks(ui::GameState& gs, const data::GameTables& tables, int star) {
  const bool dream = gs.slot.dream_records && tables.char_sets.count("char20") != 0;
  const int last = std::min<int>(star, static_cast<int>(tables.growth.size()) - 1);
  for (int char_id = 0; char_id < save::kNumChars; ++char_id) {
    const int growth_col = newgame::growth_column(&tables, gs.slot, char_id, dream) & 7;
    save::SaveChar& save_char = gs.slot.chars[static_cast<size_t>(char_id)];
    for (int level = 0; level <= last; ++level) {
      const int cell = tables.growth[static_cast<size_t>(level)][static_cast<size_t>(growth_col)];
      if (cell == 0xFF || cell >= data::kGridCells) continue;
      const int row = cell / ui::GameState::kCols, col = cell % ui::GameState::kCols;
      if (gs.cell_exists(char_id, row, col)) save_char.unlocked[static_cast<size_t>(row)] |= static_cast<u16>(1u << col);
    }
  }
}


// Every consumable stack cell below 5 copies takes the missing copies from the element stock. A stack that ran out keeps its element.
void restock_stacks(ui::GameState& gs, const data::GameTables& tables, save::SaveChar& save_char) {
  constexpr int kStackMax = 5;
  for (size_t k = 0; k < save_char.grid.size(); ++k) {
    const size_t id = save_char.grid[k] & 0x3FFF;
    if (id == 0 || id >= tables.elements.size() || (tables.elements[id].flags0() & 0x30) != 0x10) continue;
    u8& qty = save_char.equipped_qty[k];
    while (qty < kStackMax && gs.take_element(static_cast<int>(id))) ++qty;
  }
}
}  // namespace

BattleSummary apply_battle_result(ui::GameState& gs, const data::GameTables& tables, const battle::Battle& finished_battle, int* progress) {
  BattleSummary sum;
  const bool victory = finished_battle.outcome() == battle::Outcome::Victory;
  // Scene 0x1D8 leaves the party with no persistent status unless it fled.
  const bool fled = finished_battle.outcome() == battle::Outcome::Fled || finished_battle.outcome() == battle::Outcome::Stalled;
  const bool clear_status = finished_battle.scene() == 0x1D8 && !fled;
  const auto& party = finished_battle.party();
  // The party, and with party swap the characters that fought and left the field (bench records).
  std::vector<const battle::PartyMember*> fought;
  for (const battle::PartyMember& member : party) fought.push_back(&member);
  for (const battle::PartyMember& member : finished_battle.bench())
    if (std::find(finished_battle.swapped_out().begin(), finished_battle.swapped_out().end(), member.char_id) != finished_battle.swapped_out().end())
      fought.push_back(&member);
  for (const battle::PartyMember* fought_member : fought) {
    const battle::PartyMember& member = *fought_member;
    if (member.char_id < 0 || member.char_id >= save::kNumChars) continue;
    save::SaveChar& save_char = gs.slot.chars[static_cast<size_t>(member.char_id)];
    for (int k = 0; k < 7; ++k) set_stat(save_char, k, member.stat[static_cast<size_t>(k)]);
    save_char.cur_hp = static_cast<u16>(std::clamp(member.hp, 1, static_cast<int>(save_char.max_hp)));
    save_char.status = clear_status ? 0 : static_cast<u8>(member.lo_persist & battle::st::kPersist);
    write_growth(save_char, member);
    if (member.evolved && battle::pip_form(member.kind)) {
      // Pip's form change, written into the record at once: the kind byte (its growth column follows it,
      // newgame::growth_column) and the tech cells (row 2 / 4 / 6, column 7; the third only for a second form).
      save_char.model_id = static_cast<u16>(member.kind);
      for (size_t k = 0; k < battle::kPipTechCells.size(); ++k) {
        if (k == 2 && member.kind <= 0x2D) break;
        const int cell = battle::kPipTechCells[k];
        const int idx = gs.cell_index(member.char_id, cell / ui::GameState::kCols, cell % ui::GameState::kCols);
        if (idx >= 0) save_char.grid[static_cast<size_t>(idx)] = static_cast<u16>(member.cells[static_cast<size_t>(cell)] & 0x1FF);
      }
      log::info("session: {} keeps form {:#x}", member.name, member.kind);
    }
    // Consumable stacks the fight counted down (the battle's per-cell counter); a stack that ran out empties its grid cell.
    for (size_t cell = 0; cell < member.qty.size(); ++cell) {
      if ((member.cells[cell] & 0x3FFF) == 0 || (member.cells[cell] & 0x3FFF) >= tables.elements.size() || !(tables.elements[member.cells[cell] & 0x3FFF].flags0() & 0x10)) continue;
      const int k = gs.cell_index(member.char_id, static_cast<int>(cell) / ui::GameState::kCols, static_cast<int>(cell) % ui::GameState::kCols);
      if (k < 0) continue;
      const u8 before = static_cast<u8>(std::max<int>(1, save_char.equipped_qty[static_cast<size_t>(k)]));
      if (member.qty[cell] >= before) continue;
      save_char.equipped_qty[static_cast<size_t>(k)] = member.qty[cell];
    }
    restock_stacks(gs, tables, save_char);
  }
  if (progress) *progress = finished_battle.growth().progress;
  const battle::Rewards& rewards = finished_battle.rewards();
  // Element traps (pgdat mode 5), written during the battle whatever its outcome: an enemy cast caught by the party's trap
  // goes into the element stock (id & 0xFF, 0xC0 slots, count cap 99); a PC cast caught by the enemy's trap destroys its grid
  // cell in the save record (the battle cell -> record cell index map is GameState::cell_index).
  int trapped = 0, lost = 0;
  for (u16 id : rewards.trapped)
    if (gs.add_element(id & 0xFF)) ++trapped;
  for (const auto& [char_id, cell] : rewards.lost_cells) {
    if (char_id < 0 || char_id >= save::kNumChars) continue;
    const int k = gs.cell_index(char_id, cell / ui::GameState::kCols, cell % ui::GameState::kCols);
    if (k < 0) continue;
    u16& grid_word = gs.slot.chars[static_cast<size_t>(char_id)].grid[static_cast<size_t>(k)];
    grid_word = static_cast<u16>(grid_word & 0xC000);
    ++lost;
  }
  if (trapped || lost) log::info("session: element traps: {} element(s) taken into the stock, {} grid cell(s) lost", trapped, lost);
  // Stars current: the summons cast spent theirs whatever the outcome (a star-up raised it too).
  gs.slot.head[kCounterOff] = static_cast<u8>(std::clamp(finished_battle.growth().counter, 0, 255));
  if (!victory) return sum;
  // Pilfered loot (mode 30) is kept only on a victory: bit 15 = element stock (id & 0xFF, as a trapped element), else the item stock.
  for (u16 loot : rewards.stolen) {
    if (loot & 0x8000) gs.add_element(loot & 0xFF);
    else if (loot) gs.add_item(loot);
  }
  sum.gold = rewards.gold;
  gs.set_gold(static_cast<u32>(std::min<u64>(9999999ull, static_cast<u64>(gs.gold()) + static_cast<u64>(std::max(0, rewards.gold)))));
  for (u16 id : rewards.items)
    if (gs.add_item(id)) ++sum.items;
  for (u16 id : rewards.elements)
    if (gs.add_element(id)) ++sum.elements;
  if (rewards.star_up) {
    sum.star_up = true;
    // The core ran from the real star: its growth state is the new one (stars 0..99).
    gs.slot.head[kStarOff] = static_cast<u8>(std::min(99, finished_battle.growth().star));
    if (session_star(gs) < 99) star_up_unlocks(gs, tables, session_star(gs));
    // The bench got its old pending deltas and a new roll (stats / snapshots / pending in `bench()`).
    for (const battle::PartyMember& member : finished_battle.bench()) {
      if (member.char_id < 0 || member.char_id >= save::kNumChars) continue;
      save::SaveChar& save_char = gs.slot.chars[static_cast<size_t>(member.char_id)];
      for (int k = 0; k < 7; ++k) set_stat(save_char, k, member.stat[static_cast<size_t>(k)]);
      write_growth(save_char, member);
    }
  } else if (!rewards.bench_gains.empty()) {
    // Shared growth: the recruited characters outside the party grew with it in a normal fight.
    for (const battle::PartyMember& member : finished_battle.bench()) {
      if (member.char_id < 0 || member.char_id >= save::kNumChars || !gs.slot.recruited(member.char_id)) continue;
      save::SaveChar& save_char = gs.slot.chars[static_cast<size_t>(member.char_id)];
      for (int k = 0; k < 7; ++k) set_stat(save_char, k, member.stat[static_cast<size_t>(k)]);
    }
  }
  log::info("session: battle result written back (gold +{}, {} items, {} elements{}; star {} progress {})", sum.gold, sum.items, sum.elements,
            sum.star_up ? ", star up" : "", finished_battle.growth().star, finished_battle.growth().progress);
  return sum;
}

bool keep_swapped_lineup(ui::GameState& gs, const battle::Battle& finished_battle, int stand_in) {
  if (finished_battle.swaps() == 0) return false;
  const std::array<u8, 3> saved = gs.slot.party();
  std::array<u8, 3> ids{0xFF, 0xFF, 0xFF};
  const auto& party = finished_battle.party();
  for (size_t i = 0; i < party.size() && i < ids.size(); ++i) ids[i] = static_cast<u8>(party[i].char_id);
  const auto holds = [&](u8 char_id) { return std::find(ids.begin(), ids.end(), char_id) != ids.end(); };
  if (stand_in >= 0 && !holds(0)) {
    // The stand-in fought in Serge's place: its slot is Serge's; when it swapped out, Serge takes back his saved slot.
    if (holds(static_cast<u8>(stand_in))) {
      for (u8& id : ids)
        if (id == stand_in) id = 0;
    } else {
      for (size_t i = 0; i < saved.size(); ++i)
        if (saved[i] == 0) ids[i] = 0;
    }
  }
  // The party lock bitmap: words 0-1 of the slot's FE 5D / FE 60 table (FieldWorld::table_op).
  const auto locked = [&](u8 char_id) { return char_id < 0x40 && (gs.slot.unk_e1b8[static_cast<size_t>(char_id >> 5)] >> (char_id & 31) & 1u) != 0; };
  for (size_t i = 0; i < saved.size(); ++i)
    if (saved[i] != 0xFF && locked(saved[i]) && !holds(saved[i])) ids[i] = saved[i];
  if (ids == saved) return false;
  gs.slot.set_party(ids);
  log::info("session: party swap keeps the lineup {} {} {}", ids[0], ids[1], ids[2]);
  return true;
}

}  // namespace oc::game
