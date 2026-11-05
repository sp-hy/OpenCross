#pragma once
// Cheats (Esc overlay "Cheats" tab): the toggles live in platform::Cheats ([Cheats] in opencross.ini, all off by default, saving
// is never locked by them; RDE's three are never saved). This file holds the tab, the battle-side translation and the RDE "encounters off" tables.
#include <functional>
#include <span>

#include "battle/battle.h"
#include "platform/config.h"
#include "ui/typewriter.h"

namespace oc::game {

// The live set as the battle core's bypass flags (BattleView pushes it into Battle::set_cheats every update).
battle::Battle::Cheats battle_cheats();

// The tab body (call inside the registered tab's draw). Returns true when a toggle changed (the caller saves the ini).
bool draw_cheats_tab(platform::Config& cfg);

// ---- RDE "encounters off". `story(var)` reads one byte of the field script's global variables by its variable id (a byte offset
// into the 16-bit variable array: even = low byte, odd = high byte) and returns -1 when it is not available; an unknown byte makes
// the rule err on the side of the fight.
using StoryByte = std::function<int(unsigned var)>;
// The global half of the variable array (the first kStoryVarBytes byte ids), saved with the game. `vars` = FieldVm::variables();
// an id outside that window returns -1.
constexpr unsigned kStoryVarBytes = 0x400;
int story_byte_from_vars(std::span<const u16> vars, unsigned var);
// A3 / C9 with the op's raw battle id `id` (operand & 0x7FF) in `map` (the script / save map id) run by entity `ent`: true = RDE
// would patch the op into the auto-win (the A3 exception table, the C9 id list).
bool encounter_auto_won(int map, int ent, int id, bool c9, const StoryByte& story);
// Op D9 (character model swap) skipped while encounters are off (needs a particular story flag bit set).
bool model_swap_skipped(int map, int ent, const StoryByte& story);

// ---- RDE's play-time toggles and their keys / pad buttons. Keyboard: the slow / fast keys (default F1 / F2), F3 encounters off,
// F4 battle boost, F5 auto battle. Pad: LT slow, RT fast, L3 tap encounters off, R3 tap battle boost, L3 + R3 auto battle (fires
// when one of the two is let go; neither tap fires then). Slow and fast exclude each other; a press in the Radius tutorial
// (map / battle 386) is swallowed for them. Every toggle flips on the press, not while held.
constexpr int kSpeedLockedMap = 386;
struct HotkeyLevels {
  bool slow_key = false, fast_key = false, encounters_key = false, boost_key = false, auto_key = false;
  bool lt = false, rt = false, l3 = false, r3 = false;
};
struct Toggles {
  bool slow = false, fast = false, encounters = false, boost = false, auto_battle = false;
  bool operator==(const Toggles&) const = default;
};
class RdeHotkeys {
 public:
  // One frame: flips `toggles` on the presses.
  void poll(const HotkeyLevels& in, bool speed_locked, Toggles& toggles);

 private:
  bool slow_ = false, fast_ = false, encounters_ = false, boost_ = false, auto_ = false;
  bool l3_ = false, r3_ = false, chord_used_ = false;
};
// RDE's pad chord for the Esc menu, LT + L3: both down with one of them pressed this frame is a one-frame pulse, and the menu opens or
// closes as that pulse ends (the frame after the chord closes, while the buttons are still held). The same LT press and L3 release
// still reach RdeHotkeys, so the chord also flips slow mode and encounters off, as on RDE.
class MenuChord {
 public:
  // One frame of the raw LT / L3 levels; true on the frame the menu toggles.
  bool poll(bool lt, bool l3);

 private:
  bool lt_ = false, l3_ = false, pulse_ = false;
};
// The game-speed factor of the [Game] FastMode / SlowMode settings: fast_rate 0 = RDE's own (4x in the field, 2x in battle), else
// that many times; slow_percent of normal speed. 1 when neither is on or the speed is locked.
float speed_factor(bool fast, bool slow, int fast_rate, int slow_percent, bool in_battle, bool speed_locked);
// RDE's message text under fast / slow mode keeps the real clock at its own pace: fast reveals 2.25 times as fast and divides
// the {wait} / {wend} counts by 8, slow reveals at half speed and doubles them (whatever FastRate / SlowRate say).
ui::TextPace text_pace(bool fast, bool slow);
// RDE's field slow mode runs every 300th field frame at normal speed (the field draws at 30 fps: one 1/30 s stretch at full speed
// after every 299 slow frames, i.e. every 10 s). `apply` returns the factor for a frame of `dt` real seconds: `slow` outside that
// stretch, 1 inside it, blended by the overlap. While field slow mode is not `active` the count restarts and `slow` (the frame's
// factor, whatever it is) comes back unchanged.
class SlowFieldCycle {
 public:
  static constexpr double kFrame = 1.0 / 30.0, kCycle = 300.0 * kFrame;
  float apply(double dt, bool active, float slow);

 private:
  double clock_ = 0.0;
};

}  // namespace oc::game
