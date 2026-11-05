#pragma once
// The Elements > Use screen's state machine: the element list, the party target and the roster of all 44 characters. Pure logic on a
// GameState; the screen only feeds it the pad and draws its fields.
#include <vector>

#include "ui/menu/game_state.h"

namespace oc::ui {

struct UseFlow {
  struct Input {
    bool up = false, down = false, left = false, right = false, ok = false, cancel = false;
  };
  // sfx: the sound to play (0 = none); layer_sfx: a second sound played with it on its own sound mask (a heal rings two); leave: the
  // screen was cancelled from the list.
  struct Result {
    int sfx = 0, layer_sfx = 0;
    bool leave = false;
  };

  static constexpr int kRosterRows = 12, kRosterLastTop = save::kNumChars - kRosterRows;

  // list_cur / list_scroll: the list cursor (absolute row; it may rest on a blank row) and scroll. target_cur: the party target cursor.
  // roster_row / roster_top: the roster cursor row (0..11) and its first character. target: the target (or roster) is up. wait / next:
  // ticks of the list state left before the target (1) or roster (2) returns. stand_in: the roster character standing in for party slot 0
  // while the roster is up (-1 none).
  int list_cur = 0, list_scroll = 0, target_cur = 0, roster_row = 0, roster_top = 0, wait = 0, next = 0, stand_in = -1;
  bool target = false, roster = false;

  Result step(const Input& in, GameState& gs, int visible_rows);

 private:
  void pause(int ticks, int next_state);
  Result use(const std::vector<int>& list, int character, GameState& gs);
};

}  // namespace oc::ui
