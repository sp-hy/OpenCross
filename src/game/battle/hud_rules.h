#pragma once
// Pure rules of the battle HUD that BattleView applies: where a left / right press moves the target cursor, and how the stamina bar of a
// party panel is filled.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace oc::game::bhud {

// One unit of the target ring: its view-space x under the current camera (larger = further right on screen) and its home position on the
// battlefield floor.
struct RingUnit {
  int unit;
  float view_x;
  float home_x, home_z;
};

// The unit the cursor moves to from `current` for a press of `dir` (+1 right, -1 left); `ring` is in unit order. Up and down do not move it.
//  * NORMAL camera: the nearest unit strictly right (left) of the current one in view-space x; none there = the cursor stays (no wrap). On
//    equal distance the later unit wins.
//  * DYNAMIC camera: the ring ordered by the angle atan2(z, x) of the home positions, ascending (equal angles keep unit order); left steps
//    to the next unit, right to the previous one, both wrapping.
inline int target_step(const std::vector<RingUnit>& ring, int current, int dir, bool dynamic) {
  const auto at = std::find_if(ring.begin(), ring.end(), [&](const RingUnit& entry) { return entry.unit == current; });
  if (dir == 0 || at == ring.end()) return current;
  if (dynamic) {
    std::vector<RingUnit> order = ring;
    std::stable_sort(order.begin(), order.end(),
                     [](const RingUnit& lhs, const RingUnit& rhs) { return std::atan2(lhs.home_z, lhs.home_x) < std::atan2(rhs.home_z, rhs.home_x); });
    const int count = static_cast<int>(order.size());
    const int pos = static_cast<int>(std::find_if(order.begin(), order.end(), [&](const RingUnit& entry) { return entry.unit == current; }) - order.begin());
    return order[static_cast<size_t>(((pos - dir) % count + count) % count)].unit;
  }
  int pick = current;
  float best = 0;
  for (const RingUnit& entry : ring) {
    if (entry.unit == current) continue;
    const float delta = (entry.view_x - at->view_x) * static_cast<float>(dir);
    if (delta <= 0) continue;
    if (pick == current || delta <= best) pick = entry.unit, best = delta;
  }
  return pick;
}

// The stamina bar's fill: |stamina| (tenths) capped at 70, width |stamina| * 100 / 140 PS1 px of the 50 px track but never below 1 (an empty
// fill still lights the track's first column). Colour: red below 1.0, yellow to 3.9, cyan from 4.0; every negative value is red.
enum class BarColour { Red, Yellow, Cyan };
struct StaminaFill {
  int width;
  BarColour colour;
};
inline StaminaFill stamina_fill(int stamina) {
  const int magnitude = std::min(std::abs(stamina), 70);
  const BarColour colour = stamina < 0 || magnitude < 10 ? BarColour::Red : magnitude < 40 ? BarColour::Yellow : BarColour::Cyan;
  return {std::max(magnitude * 100 / 140, 1), colour};
}

}  // namespace oc::game::bhud
