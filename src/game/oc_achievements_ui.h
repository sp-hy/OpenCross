#pragma once
// The Esc overlay's Achievements tab and the unlock popup of OpenCross's own achievement set (ImGui).
#include <string>
#include <vector>

#include "game/oc_achievements.h"

namespace oc::game {

// The tab's session state: the category filter (0 = all) and the spoiler box.
struct OcAchievementsTab {
  int category = 0;
  bool show_spoilers = false;
};

// `hide_notices` the [Game] HideAchievementNotices setting, edited by the tab's box. Returns true when it changed.
bool draw_oc_achievements_tab(const OcAchievements& achievements, OcAchievementsTab& state, bool& hide_notices);

// The unlock notice: a panel in the top-right corner for 240 ticks. A panel takes every unlock waiting when it opens, so a batch (a
// progressed save loaded) shows once; unlocks during a panel wait for the next one.
class OcAchievementPopup {
public:
  void push(std::string title) { queue_.push_back(std::move(title)); }
  void clear();
  // `ticks` 60 Hz ticks since the last call. `held`: a movie plays or the settings window is open, so the time stands. Returns the
  // titles of the panel on screen (empty when none).
  const std::vector<std::string>& update(double ticks, bool held);
  void draw(double ticks, bool held);

private:
  std::vector<std::string> queue_;
  std::vector<std::string> panel_;
  double shown_ = 0.0;
};

}  // namespace oc::game
