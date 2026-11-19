#pragma once
// OpenCross's own achievements as the Achievements page of the OpenCross row reads them: plain rows in table order (game/oc_achievements_ui.h
// builds them from game::OcAchievements), and the page's filter over them.
#include <string>
#include <vector>

namespace oc::ui {

// The page's category filter: 0 every row, else a row's category + 1 (Story and places, Bosses, Recruits, Elements and techs, Battle,
// Items and endgame).
inline constexpr int kAchievementFilters = 7;

// One achievement: its English title and description, its category (0..5), whether it is a spoiler, and either the local date it was
// earned (YYYY-MM-DD) or, while locked, its progress ("212 / 300"; empty when it has none).
struct AchievementRow {
  std::string title;
  std::string description;
  int category = 0;
  bool spoiler = false;
  bool unlocked = false;
  std::string date;
  std::string progress;
};

struct AchievementList {
  std::vector<AchievementRow> rows;
  int unlocked = 0;
};

// The rows `filter` shows, as indices into `list.rows` in table order.
std::vector<int> filter_achievements(const AchievementList& list, int filter);

// A locked spoiler row shows "???" for its title and description unless spoilers are shown.
inline bool achievement_hidden(const AchievementRow& row, bool show_spoilers) { return !row.unlocked && row.spoiler && !show_spoilers; }

}  // namespace oc::ui
