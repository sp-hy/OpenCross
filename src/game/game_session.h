#pragma once
// Bridges between the in-memory ui::GameState (the single source of truth: save::SaveSlot + helpers) and the battle core:
// builds battle::PartyMember from it and writes a finished battle back (the after-battle state).
//
// Layout notes: the party's saved record gives max HP / stats (SaveChar), current HP (cur_hp), gear, the
// element grid (GameState::cell_element) and the unlocked-level masks. The growth snapshot / pending deltas live in the record
// fields rec+BC/BE (HP) and rec+C0..CB (ATK, HIT, DEF, EVD, MAG, MDEF: snapshot, pending). Stars: head 0x5A4 (star) / 0x5A5
// (counter). The growth progress counter is not stored anywhere in the save: the host keeps it for the session
// (battle_setup / apply_battle_result take and return it; a loaded or new game starts at 0).
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
// (every recruited character outside the party, for the star-up).
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
// Rewards only on Outcome::Victory; HP / status / growth state are written for every outcome.
// `progress` (optional) receives the growth progress counter after the battle.
BattleSummary apply_battle_result(ui::GameState& gs, const data::GameTables& tables, const battle::Battle& finished_battle, int* progress = nullptr);

std::string utf8_from_u32(std::u32string_view text);

}  // namespace oc::game
