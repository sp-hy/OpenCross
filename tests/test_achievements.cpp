// game/achievements.h: the trigger rules, without Steam.
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "game/achievements.h"

using namespace oc;
using game::Achievements;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

constexpr int kSampleTicks = 60;

// Runs `samples` room samples standing in (prev, room).
void stay(Achievements& achievements, int prev, int room, int samples = 1) {
  Achievements::Progress progress;
  progress.prev_room = prev;
  progress.room = room;
  achievements.tick(samples * kSampleTicks, progress, false);
}

bool took(Achievements& achievements, const char* name) {
  bool found = false;
  for (const char* earned : achievements.take_earned()) found |= std::strcmp(earned, name) == 0;
  return found;
}

// Walks room to room, one sample in each; `moves` = {prev, room} pairs.
void walk(Achievements& achievements, const std::vector<std::pair<int, int>>& moves) {
  for (const auto& [prev, room] : moves) stay(achievements, prev, room);
}
}  // namespace

int main() {
  {  // A pair must hold for its samples; leaving restarts them.
    Achievements achievements;
    stay(achievements, 0x1A, 0x1F, 19);
    stay(achievements, 0x1F, 0x20);
    stay(achievements, 0x1A, 0x1F, 19);
    CHECK(!took(achievements, "ach_eve1_yumemezame"));
    stay(achievements, 0x1A, 0x1F);
    CHECK(took(achievements, "ach_eve1_yumemezame"));
    stay(achievements, 0x1A, 0x1F, 40);
    CHECK(achievements.take_earned().empty());
  }
  {  // Rooms are not sampled in battle, and only every 60 ticks.
    Achievements achievements;
    Achievements::Progress progress;
    progress.prev_room = 0x103;
    progress.room = 0x1DD;
    achievements.tick(600, progress, true);
    CHECK(achievements.take_earned().empty());
    achievements.tick(59, progress, false);
    CHECK(achievements.take_earned().empty());
    achievements.tick(1, progress, false);
    CHECK(took(achievements, "ach_eve6_chronop"));
  }
  {  // A numbered ending: armed from the beach, awarded on reaching room 9.
    Achievements achievements;
    walk(achievements, {{0x20, 0x38}, {0x38, 0x1CE}, {0x1CE, 0x1D0}});
    CHECK(achievements.take_earned().empty());
    stay(achievements, 0x1D0, 9);
    CHECK(took(achievements, "ach_end3_kaihatu"));
  }
  {  // An ending's room reached from elsewhere arms nothing.
    Achievements achievements;
    walk(achievements, {{0x20, 0x21}, {0x21, 0x1CE}, {0x1CE, 9}});
    CHECK(achievements.take_earned().empty());
  }
  {  // Ending 3's route turns into ending 4 through room 0x1F2, from either beach.
    Achievements achievements;
    walk(achievements, {{0x20, 0x39}, {0x39, 0x1ED}, {0x1ED, 0x1F2}, {0x1F2, 9}});
    CHECK(took(achievements, "ach_end6_shop"));
  }
  {
    Achievements achievements;
    walk(achievements, {{0x20, 0x39}, {0x39, 0x1ED}, {0x1ED, 9}});
    CHECK(took(achievements, "ach_end5_akasia"));
  }
  {  // Good ending: not on reaching room 9, but on room 9's jump to itself.
    Achievements achievements;
    walk(achievements, {{0x20, 0x39}, {0x39, 0x216}, {0x216, 0x39}, {0x39, 9}});
    CHECK(achievements.take_earned().empty());
    stay(achievements, 9, 9);
    CHECK(took(achievements, "ach_end1_good"));
  }
  {  // Bad ending: straight to room 9 from either beach.
    Achievements achievements;
    walk(achievements, {{0x20, 0x38}, {0x38, 9}});
    CHECK(took(achievements, "ach_end2_bad"));
    Achievements other;
    walk(other, {{0x20, 0x39}, {0x39, 9}});
    CHECK(took(other, "ach_end2_bad"));
  }
  {  // Bosses count on a win (0) or a scripted end (3).
    Achievements achievements;
    achievements.battle_ended(433, 1, 0);
    achievements.battle_ended(433, 2, 0);
    CHECK(achievements.take_earned().empty());
    achievements.battle_ended(433, 0, 0);
    CHECK(took(achievements, "ach_eve3_yamaneko"));
    achievements.battle_ended(462, 3, 0);
    CHECK(took(achievements, "ach_eve7_fate"));
  }
  {  // Fleeing is the outcome, not code 2, and it is earned 180 ticks after the flight begins. 500 wins count at a counted win only.
    Achievements achievements;
    achievements.battle_ended(1, 2, 600);
    CHECK(achievements.take_earned().empty());
    Achievements::Progress progress;
    progress.fled = true;
    achievements.tick(1, progress, true);
    achievements.tick(179, progress, true);
    CHECK(achievements.take_earned().empty());
    achievements.tick(1, progress, true);
    CHECK(took(achievements, "ach_battle1_condition"));
    achievements.battle_ended(1, 0, 499);
    CHECK(achievements.take_earned().empty());
    achievements.battle_ended(1, 0, 500);
    CHECK(took(achievements, "ach_battle8_master"));
  }
  {  // Items: rooms matter where the id is shared.
    Achievements achievements;
    achievements.item_given(0x06, 0x10);
    achievements.item_given(0x9E, 0x10);
    CHECK(achievements.take_earned().empty());
    achievements.item_given(0x06, 0x182);
    CHECK(took(achievements, "ach_minigame3_roulette"));
    achievements.item_given(0x42D, 0x10);
    CHECK(took(achievements, "ach_eve8_chrinocross"));
    achievements.var_bit_set(0x16C6, 0x10);
    CHECK(achievements.take_earned().empty());
    achievements.var_bit_set(0x16C6, 0x187);
    CHECK(took(achievements, "ach_minigame2_gran"));
  }
  {  // Techs arrive after their delay, and only the listed element: 152 shares Forever Zero's name.
    Achievements achievements;
    Achievements::Progress progress;
    achievements.element_cast(152);
    achievements.element_cast(158);
    achievements.element_cast(158);
    achievements.tick(59, progress, true);
    CHECK(achievements.take_earned().empty());
    achievements.tick(1, progress, true);
    CHECK(took(achievements, "ach_battle4_zero"));
  }
  {  // Forged rainbow gear.
    Achievements achievements;
    achievements.forged(0x24);
    CHECK(achievements.take_earned().empty());
    achievements.forged(0x25);
    CHECK(took(achievements, "ach_item1_nijibuki"));
    achievements.forged(0xC7);
    CHECK(took(achievements, "ach_item2_nijibougu"));
  }
  {  // Stars: the 98 -> 99 step only, so a save already at 99 does not count. All 15 frames do, even on load.
    Achievements achievements;
    Achievements::Progress progress;
    progress.stars = 99;
    achievements.tick(1, progress, false);
    CHECK(achievements.take_earned().empty());
    progress.stars = 98;
    achievements.tick(1, progress, false);
    progress.stars = 99;
    achievements.tick(1, progress, false);
    CHECK(took(achievements, "ach_level1_gold"));
    progress.window_frames = 0x7FFF;
    progress.all_recruited = true;
    achievements.tick(kSampleTicks, progress, false);
    const std::vector<const char*> earned = achievements.take_earned();
    CHECK(earned.size() == 2);
  }

  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("achievements: all checks passed\n");
  return failures ? 1 : 0;
}
