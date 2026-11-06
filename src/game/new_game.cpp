#include "game/new_game.h"

#include <algorithm>

#include "battle/rng.h"
#include "battle/rules.h"
#include "data/tables.h"

namespace oc::game::newgame {

namespace {

// Without char20.bin the dream uses the real records.
const data::CharSet* char_set(const data::GameTables* tables, bool dream) {
  if (!tables) return nullptr;
  auto it = tables->char_sets.find(record_set(dream));
  if (it == tables->char_sets.end() && dream) it = tables->char_sets.find("char");
  return it == tables->char_sets.end() ? nullptr : &it->second;
}

u8 u7(int value) { return static_cast<u8>(std::clamp(value, 0, 127)); }

}  // namespace

const char* record_set(bool dream) { return dream ? "char20" : "char"; }

std::array<u16, 24> globals() {
  return {0, 0x3F, 0, 0, 0, 0, 0, 0, 0x40A, 0x404, 0, 0x2060, 0x80, 0, 0, 0, 0x2000, 0, 0, 0, 0, 0, 0, 0};
}

void load_records(const data::GameTables* tables, save::SaveSlot& slot, bool dream, bool reset_names) {
  const data::CharSet* set = char_set(tables, dream);
  for (int char_id = 0; char_id < save::kNumChars; ++char_id) {
    save::SaveChar& rec = slot.chars[static_cast<size_t>(char_id)];
    const auto name = rec.name;
    rec = save::SaveChar{};
    if (!reset_names) rec.name = name;
    rec.unlocked = kInitialUnlocks;
    if (!set) continue;
    const data::StatRec& file_rec = set->rec[static_cast<size_t>(char_id)];
    rec.colour_bit = file_rec.raw[0x01];
    rec.rec02 = file_rec.raw[0x02];
    rec.rec_a8 = file_rec.raw[0xA8];
    rec.max_hp = file_rec.max_hp();
    rec.cur_hp = file_rec.max_hp();
    rec.atk = file_rec.atk(), rec.def = file_rec.def(), rec.hit = file_rec.hit(), rec.evd = file_rec.evd(), rec.mag = file_rec.mag(),
    rec.mdef = file_rec.mdef();
    rec.weapon = file_rec.weapon();
    rec.armour = file_rec.armour();
    for (int i = 0; i < 3; ++i) rec.accessory[static_cast<size_t>(i)] = file_rec.accessory(i);
    rec.model_id = file_rec.model();
    for (int k = 0; k < 48; ++k) rec.grid[static_cast<size_t>(k)] = file_rec.cell_listed(k) & 0x1FF;
  }
  for (auto& item : slot.items) item = {};
  for (auto& element : slot.elements) element = {};
  slot.set_gold(100);
  slot.set_growth_progress(0);
  const auto vars = globals();
  for (size_t i = 0; i < vars.size(); ++i) slot.set_var(0x3D0 + 2 * static_cast<int>(i), vars[i]);
}

void derive_growth(const data::GameTables* tables, save::SaveSlot& slot, bool dream, u32 seed) {
  const data::CharSet* set = char_set(tables, dream);
  if (!set) return;
  const int star = slot.head[save::head::kStarsTotal];
  battle::GameRng rng(seed ? seed : 0x5EED0001u, tables->rng_table);
  auto roll = [&](int gap) { return battle::growth_roll(rng, gap, star); };
  for (int char_id = 0; char_id < save::kNumChars; ++char_id) {
    save::SaveChar& rec = slot.chars[static_cast<size_t>(char_id)];
    const data::Lv99& lv99 = set->lv99[static_cast<size_t>(char_id)];
    rec.hp_extra = rec.max_hp;
    rec.unk_be = static_cast<u16>(std::clamp(roll(lv99.hp - rec.max_hp) + 2, 0, 999));
    rec.bonus[0] = u7(rec.atk), rec.bonus[1] = u7(roll(lv99.atk - rec.atk));
    rec.bonus[2] = u7(rec.hit), rec.bonus[3] = u7(roll(lv99.hit - rec.hit));
    rec.bonus[4] = u7(rec.def), rec.bonus[5] = u7(roll(lv99.def - rec.def));
    rec.bonus[6] = u7(rec.evd), rec.bonus[7] = u7(roll(lv99.evd - rec.evd));
    rec.bonus[8] = u7(rec.mag), rec.bonus[9] = u7(roll(lv99.mag - rec.mag));
    rec.bonus[10] = u7(rec.mdef), rec.bonus[11] = u7(roll(lv99.mdef - rec.mdef));
  }
}

void star_unlocks(const data::GameTables* tables, save::SaveSlot& slot, bool dream, bool fresh) {
  const data::CharSet* set = char_set(tables, dream);
  const int stars = slot.head[save::head::kStarsTotal];
  for (int char_id = 0; char_id < save::kNumChars; ++char_id) {
    save::SaveChar& rec = slot.chars[static_cast<size_t>(char_id)];
    if (set && tables) {
      const int col = growth_column(tables, slot, char_id, dream);
      for (int star = 0; star < stars && star < static_cast<int>(tables->growth.size()); ++star) {
        const int cell_code = tables->growth[static_cast<size_t>(star)][static_cast<size_t>(col & 7)];
        if (cell_code == 0xFF || cell_code / 10 >= 8) continue;
        rec.unlocked[static_cast<size_t>(cell_code / 10)] =
            static_cast<u16>(rec.unlocked[static_cast<size_t>(cell_code / 10)] | (1u << (cell_code % 10)));
      }
    }
    if (fresh) rec.equipped_qty.fill(kEquippedFill);
  }
}

void reset_party(save::SaveSlot& slot) {
  slot.set_party({0, 0xFF, 0xFF});
  slot.set_key_items(kKeyTimeShifter);
  for (int char_id = 0; char_id < save::kNumChars; ++char_id) slot.set_recruited(char_id, char_id == 0);
  slot.unk_e1b8 = {1u, 0u, 0u, 0u};
  slot.unk_e1e0 = slot.unk_e1e4 = 0;
}

void reset_options(save::SaveSlot& slot) {
  slot.head[0x5A0] = static_cast<u8>(kOptionsDefault);
  slot.head[0x5A1] = slot.head[0x5A2] = slot.head[0x5A3] = 0;
  slot.button_cfg = kDefaultButtons;
  slot.set_playtime_frames(0);
}

void real_records(const data::GameTables* tables, save::SaveSlot& slot, u32 seed) {
  load_records(tables, slot, false, false);
  slot.head[save::head::kStarsTotal] = slot.head[save::head::kStarsCurrent] = 0;
  slot.head[save::head::kPreviewStars] = 0;
  derive_growth(tables, slot, false, seed);
  star_unlocks(tables, slot, false, true);
  slot.dream_records = false;
}

// A fresh slot already has variables 0..0x3CF = 0; variable 0x42 = disc - 1 = 0.
save::SaveSlot boot_state(const data::GameTables* tables, u32 seed) {
  save::SaveSlot slot;
  load_records(tables, slot, true, true);
  slot.head[save::head::kStarsTotal] = slot.head[save::head::kStarsCurrent] = kDreamStars;
  slot.head[save::head::kPreviewStars] = kDreamStars;
  slot.head[0x59F] = 0xFF;
  derive_growth(tables, slot, true, seed);
  star_unlocks(tables, slot, true, true);
  reset_options(slot);
  reset_party(slot);
  slot.frames = kNewGameFrames;
  slot.unk_e1ec = slot.unk_e1f0 = 0;
  slot.set_var(0x42, 0);
  slot.dream_records = true;
  return slot;
}

void ensure_time_shifter(save::SaveSlot& slot) { slot.set_key_items(slot.key_items() | kKeyTimeShifter); }

int growth_column(const data::GameTables* tables, const save::SaveSlot& slot, int char_id, bool dream) {
  const data::CharSet* set = char_set(tables, dream);
  if (!set || char_id < 0 || char_id >= save::kNumChars) return 0;
  const int form = slot.chars[static_cast<size_t>(char_id)].model_id;
  if (char_id == kPip && form >= 0x2B && form <= 0x30) return set->rec[static_cast<size_t>(form)].growth_col();
  return set->rec[static_cast<size_t>(char_id)].growth_col();
}

void rde_playinit_end(const data::GameTables* tables, save::SaveSlot& slot, const std::array<u16, 8>* pip_masks, u32 seed) {
  save::SaveChar& pip = slot.chars[static_cast<size_t>(kPip)];
  const int form = pip.model_id;
  if (tables && form >= 0x2B && form <= 0x30) {
    const int stars = slot.head[save::head::kStarsTotal];
    if (stars < 99) {
      auto it = tables->char_sets.find("char");
      if (it != tables->char_sets.end()) {
        const data::Lv99& lv99 = it->second.lv99[static_cast<size_t>(form)];
        battle::GameRng rng(seed ? seed : 0x5EED0002u, tables->rng_table);
        auto roll = [&](int snap, int target) { return snap < target ? rng.libc_rand() % ((target - snap) / (99 - stars) + 1) : 0; };
        pip.unk_be = static_cast<u16>(roll(pip.hp_extra, lv99.hp));
        pip.bonus[1] = u7(roll(pip.bonus[0], lv99.atk));
        pip.bonus[5] = u7(roll(pip.bonus[4], lv99.def));
        pip.bonus[3] = u7(roll(pip.bonus[2], lv99.hit));
        pip.bonus[7] = u7(roll(pip.bonus[6], lv99.evd));
        pip.bonus[9] = u7(roll(pip.bonus[8], lv99.mag));
        pip.bonus[11] = u7(roll(pip.bonus[10], lv99.mdef));
      }
    } else {
      pip.unk_be = 0;
      pip.bonus[1] = pip.bonus[3] = pip.bonus[5] = pip.bonus[7] = pip.bonus[9] = pip.bonus[11] = 0;
    }
  }
  if (pip_masks)
    for (size_t row = 0; row < 8; ++row) pip.unlocked[row] = static_cast<u16>(pip.unlocked[row] | (*pip_masks)[row]);
}

void pip_form_stats(const data::GameTables* tables, save::SaveSlot& slot) {
  save::SaveChar& pip = slot.chars[static_cast<size_t>(kPip)];
  const int form = pip.model_id;
  if (!tables || slot.head[save::head::kStarsTotal] != 99 || form < 0x2B || form > 0x30) return;
  auto it = tables->char_sets.find("char");
  if (it == tables->char_sets.end()) return;
  const data::Lv99& lv99 = it->second.lv99[static_cast<size_t>(form)];
  pip.cur_hp = static_cast<u16>(std::clamp(pip.cur_hp + lv99.hp - pip.max_hp, 0, static_cast<int>(lv99.hp)));
  pip.max_hp = pip.hp_extra = lv99.hp;
  pip.atk = lv99.atk, pip.def = lv99.def, pip.hit = lv99.hit, pip.evd = lv99.evd, pip.mag = lv99.mag, pip.mdef = lv99.mdef;
  pip.bonus[0] = pip.atk, pip.bonus[2] = pip.hit, pip.bonus[4] = pip.def, pip.bonus[6] = pip.evd, pip.bonus[8] = pip.mag,
  pip.bonus[10] = pip.mdef;
}

void pip_form_unlocks(const data::GameTables* tables, save::SaveSlot& slot) {
  save::SaveChar& pip = slot.chars[static_cast<size_t>(kPip)];
  const int form = pip.model_id;
  if (!tables || slot.head[save::head::kStarsTotal] != 99 || form < 0x2B || form > 0x30) return;
  const int column = growth_column(tables, slot, kPip, false) & 7;
  for (size_t row = 0; row < pip.unlocked.size(); ++row) pip.unlocked[row] = static_cast<u16>(pip.unlocked[row] | kInitialUnlocks[row]);
  for (const auto& star : tables->growth) {
    const int cell = star[static_cast<size_t>(column)];
    if (cell < data::kGridCells) pip.unlocked[static_cast<size_t>(cell / data::kGridCols)] |= static_cast<u16>(1u << (cell % data::kGridCols));
  }
}

}  // namespace oc::game::newgame
