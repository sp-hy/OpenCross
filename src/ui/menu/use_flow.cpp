#include "ui/menu/use_flow.h"

#include <algorithm>

namespace oc::ui {

namespace {
constexpr int kTargetPause = 3, kUsePause = 6, kCursorSfx = 1, kCancelSfx = 6, kDeniedSfx = 0x18, kHealSfx = 0x12, kHealLayerSfx = 0x13, kCureSfx = 0xBE;
}  // namespace

// A refusal (an absent character, nothing to heal or cure) changes nothing; a use that leaves copies returns to the same target after a
// short pause in the list state, the last copy returns to the list with its cursor and scroll kept.
UseFlow::Result UseFlow::step(const Input& in, GameState& gs, int visible_rows) {
  Result result;
  const std::vector<int> list = gs.usable_elements();
  const int count = static_cast<int>(list.size());
  if (wait > 0) {
    if (--wait == 0) target = next >= 1, roster = next == 2;
    return result;
  }
  if (!target) {
    const int rows = std::max(count, visible_rows), scroll_before = list_scroll;
    if (in.up) list_cur = std::max(0, list_cur - 1), result.sfx = kCursorSfx;
    if (in.down) list_cur = std::min(rows - 1, list_cur + 1), result.sfx = kCursorSfx;
    if (list_cur < list_scroll) list_scroll = list_cur;
    if (list_cur >= list_scroll + visible_rows) list_scroll = list_cur - visible_rows + 1;
    list_scroll = std::clamp(list_scroll, 0, rows - visible_rows);
    list_cur = std::clamp(list_cur - (scroll_before - list_scroll), list_scroll, list_scroll + visible_rows - 1);
    if (in.cancel) result.sfx = kCancelSfx, result.leave = true;
    else if (in.ok && list_cur < count) result.sfx = kCursorSfx, pause(kTargetPause, 1);
    return result;
  }
  if (!roster) {
    const std::vector<int> members = gs.party();
    if (in.up) --target_cur, result.sfx = kCursorSfx;
    if (in.down) ++target_cur, result.sfx = kCursorSfx;
    target_cur = std::clamp(target_cur, 0, std::max(0, static_cast<int>(members.size()) - 1));
    if (in.left) {
      roster = true;
      roster_row = roster_top = 0;
      stand_in = 0;
      result.sfx = kCursorSfx;
    } else if (in.cancel) {
      target = false;
      result.sfx = kCancelSfx;
    } else if (in.ok && !members.empty()) {
      result = use(list, members[static_cast<size_t>(target_cur)], gs);
    }
    return result;
  }
  if (in.up) --roster_row, result.sfx = kCursorSfx;
  if (in.down) ++roster_row, result.sfx = kCursorSfx;
  if (roster_row < 0) roster_top = std::max(0, roster_top - 1), roster_row = 0;
  if (roster_row >= kRosterRows) roster_top = std::min(kRosterLastTop, roster_top + 1), roster_row = kRosterRows - 1;
  stand_in = roster_top + roster_row;
  if (in.right || in.cancel) {
    roster = false;
    stand_in = -1;
    result.sfx = in.right ? kCursorSfx : kCancelSfx;
  } else if (in.ok) {
    result = use(list, stand_in, gs);
  }
  return result;
}

void UseFlow::pause(int ticks, int next_state) {
  target = roster = false;
  wait = ticks;
  next = next_state;
}

UseFlow::Result UseFlow::use(const std::vector<int>& list, int character, GameState& gs) {
  Result result;
  if (list_cur >= static_cast<int>(list.size())) return result;
  const int id = gs.slot.elements[static_cast<size_t>(list[static_cast<size_t>(list_cur)])].id;
  if (gs.use_element(id, character) != Why::Ok) {
    result.sfx = kDeniedSfx;
    return result;
  }
  const PgRow* row = gs.rules ? gs.rules->pg_row(id) : nullptr;
  if (row && row->heals()) result.sfx = kHealSfx, result.layer_sfx = kHealLayerSfx;
  else result.sfx = kCureSfx;
  if (gs.element_count(id) == 0) {
    target = roster = false;
    stand_in = -1;
  } else {
    pause(kUsePause, roster ? 2 : 1);
  }
  return result;
}

}  // namespace oc::ui
