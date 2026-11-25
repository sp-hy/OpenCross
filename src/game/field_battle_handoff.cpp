// BattleHandoff: the encounter timeline queries and the battle disguise.
#include "game/field_battle_handoff.h"

#include "core/log.h"
#include "game/encounter_strips.h"
#include "save/cc_save.h"
#include "ui/menu/game_state.h"

namespace oc::game {

void BattleHandoff::reset() {
  battle_state = BattleState::None;
  battle_anchor = -1;
}

// BATTLE flag 4 (the Viper Manor / Dragoon disguise fights): the battle loader and the field's battle return both write
// model byte 0x3B (adk_sp) into the character record of every party member
// but Poshul (id 12) and Mojo (id 27). The byte is saved state: the field party is rebuilt from it after the battle (a
// slot whose loaded model differs from the record byte is reloaded), so the disguise stays until a script's D9 restores the character's own model.
void BattleHandoff::apply_battle_disguise(ui::GameState* game_state, const std::vector<int>& party_chars, u64 tick_count) const {
  if (!(battle_req.flags & 4) || !game_state) return;
  for (size_t slot = 0; slot < party_chars.size() && slot < 3; ++slot) {
    const int char_id = party_chars[slot];
    if (!game_state->slot.is_character(char_id) || char_id == 12 || char_id == 27) continue;
    game_state->slot.chars[static_cast<size_t>(char_id)].model_id = 0x3B;
    log::debug("field: tick {} battle flag 4: character {} model byte 0x3B", tick_count, char_id);
  }
}

// The battle opening's white clear: 4 frames (frames 101..104). The host also puts it into the strips' feed-back chain (app/app_feedback.cpp).
bool BattleHandoff::transition_white(u64 tick_count) const {
  if (!in_intro_or_hold()) return false;
  const u64 elapsed = tick_count - intro_tick;
  return elapsed >= static_cast<u64>(kEncounterWhiteFrom) && elapsed < static_cast<u64>(kEncounterWhiteFrom + kEncounterWhiteTicks);
}

// The hold shows the output of the last frame before the tear-down tick (its sparks and discs still in the picture); the tear-down frame itself (e 109)
// is a one-frame dip and is drawn as it is.
bool BattleHandoff::transition_prefreeze(u64 tick_count) const {
  if (!in_intro_or_hold()) return false;
  return tick_count - intro_tick == static_cast<u64>(kEncounterWhiteFrom - 1);
}

// The strips' bracket clock m = ticks since `main`'s first ramp (the first update after the white is m 1).
int BattleHandoff::transition_main_clock(u64 tick_count) const {
  return static_cast<int>(tick_count - intro_tick) - (kEncounterWhiteFrom + kEncounterWhiteTicks - 1);
}

BattleHandoff::TransitionStrips BattleHandoff::transition_strips(u64 tick_count) const {
  TransitionStrips strips;
  if (!in_intro_or_hold()) return strips;
  const int elapsed = static_cast<int>(tick_count - intro_tick);
  // Game ticks after the white.
  const int ticks_after_white = elapsed - (kEncounterWhiteFrom + kEncounterWhiteTicks);
  if (ticks_after_white < 0) return strips;
  strips.active = true;
  // A pass every tick (App::feedback_hold_pass: partial factors from the update's anchor).
  strips.pass = true;
  strips.update = ticks_after_white / 4 + 1;
  strips.m = transition_main_clock(tick_count);
  strips.scene_gain = encstrips::hold_scene_gain(static_cast<float>(strips.update));
  return strips;
}

}  // namespace oc::game
