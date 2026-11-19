#pragma once
// OpenCross's own achievement set as the OpenCross row's Achievements page reads it, and the unlock popup (ImGui).
#include <string>
#include <vector>

#include "game/oc_achievements.h"
#include "ui/menu/achievements_info.h"

namespace oc::game {

// The page's rows in table order: title, description, category, spoiler, the local date of an unlock, the progress of a locked row.
ui::AchievementList achievement_list(const OcAchievements& achievements);

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
