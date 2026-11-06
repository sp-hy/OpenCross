#pragma once
// Bridges between the in-memory ui::GameState (the single source of truth: save::SaveSlot + helpers) and the battle core:
// builds battle::PartyMember from it and writes a finished battle back (the after-battle state).
//
// Layout notes: the party's saved record gives max HP / stats (SaveChar), current HP (cur_hp), gear, the
// element grid (GameState::cell_element) and the unlocked-level masks. The growth snapshot / pending deltas live in the record
// fields rec+BC/BE (HP) and rec+C0..CB (ATK, HIT, DEF, EVD, MAG, MDEF: snapshot, pending). Stars: head 0x5A4 (star) / 0x5A5
// (counter). The growth progress counter is head 0x598 (SaveSlot::growth_progress): battle_setup takes it,
// apply_battle_result returns the new value, and the host stores it back; a new game starts at 0.
#include <string>
#include <string_view>
#include <vector>

#include "battle/battle.h"
#include "data/char_name.h"
#include "ui/menu/game_state.h"

namespace oc::game {

// Star (0..99) the save is at.
int session_star(const ui::GameState& gs);

// PartyMember for character `char_id` from the saved state. `names` (optional) gives the typed names and the language of the member's
// names; without it the member has the English default names.
// A record whose growth snapshot was never written (new game) gets snapshot = stats and a deterministic first pending roll.
battle::PartyMember party_member_from_state(const data::GameTables& tables, const ui::GameState& gs, int char_id,
                                            const data::CharNames* names = nullptr);

// The Lynx duel's names (battle::BattleOptions::duel_names).
std::vector<std::string> duel_names(const ui::GameState& gs, const data::CharNames& names);

struct BattleSummary {
  int gold = 0;
  int items = 0, elements = 0;
  bool star_up = false;
};

// What the battle core needs besides the party: NG+ flag (head 0x42 bit 1), the growth state (star, progress, counter) and the bench
// (every filled character record outside the party, recruited or not, for the star-up).
struct BattleSetup {
  bool ng_plus = false;
  battle::GrowthState growth;
  std::vector<battle::PartyMember> bench;
};
BattleSetup battle_setup(const data::GameTables& tables, const ui::GameState& gs, const std::vector<int>& party_ids, int progress,
                         const data::CharNames* names = nullptr);

// Writes a finished Battle back into the state: HP (min(HP, max), at least 1), persistent status bits (poison, burn, sprain,
// flu), stats / snapshots / pending deltas from the growth, gold (cap 9,999,999), dropped items and elements, the star
// counters and (after a boss) the new grid unlocks. Stamina is not carried (every battle starts at 7.0).
// Rewards only on Outcome::Victory; HP / status / growth state are written for every outcome, also for the characters that fought and
// were swapped out (party swap). The saved party itself is not changed (keep_swapped_lineup).
// `progress` (optional) receives the growth progress counter after the battle.
BattleSummary apply_battle_result(ui::GameState& gs, const data::GameTables& tables, const battle::Battle& finished_battle, int* progress = nullptr);

// The last step of a fight, after the results screen has written its HP: when Pip's record form differs from `form_before` (his form
// when the fight began) and both are forms 0x2B..0x30, a 99-star game gives him the new form's Lv 99 stats and its full grid
// (newgame::pip_form_stats / pip_form_unlocks).
void settle_pip_form(ui::GameState& gs, const data::GameTables& tables, int form_before);

// Party swap's "keep lineup" setting: the saved party becomes the lineup the battle ended with, then every locked character (party lock
// bitmap, FE 5D) that ended on the bench goes back to the slot it held in the saved party, and `stand_in` (the Relief Charm's member that
// took Serge's slots, -1 = none) is written as Serge unless Serge himself came in. True when the saved party changed.
bool keep_swapped_lineup(ui::GameState& gs, const battle::Battle& finished_battle, int stand_in);

std::string utf8_from_u32(std::u32string_view text);

}  // namespace oc::game
