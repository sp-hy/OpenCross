#pragma once
// OpenCross's own achievement set. The rows (oc_achievement_table.inc) are evaluated over room samples outside battle, battle results,
// the actions a battle presents, script item gives and smith forges; unlocks, counters and the members seen of the sets that span saves
// belong to the player's profile, which parse_profile / serialize_profile turn into text and back. The class only watches the game; the
// host reads and writes the profile file and draws the tab and the popups.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "battle/battle.h"
#include "game/achievements.h"

namespace oc::game {

enum class OcCategory { Story, Bosses, Recruits, Elements, Battle, Items };

// Rule kinds. RoomPair: the previous room in `prev_ids`, the room in `ids`. VarBits: a bit of `var` listed in `ids` is set. Ending: an
// ending of the room samples is awarded while the variable `var` holds `value`.
enum class OcRule { Room, RoomPair, Recruit, Recruits, Body, Save, VarBits, Fight, Cast, Item, Count, Profile, Set, Event, Ending };

// What a rule reads: the save value of a Save rule, the counter of a Count rule, the event of an Event rule, the members of a Profile rule.
enum class OcSource {
  None,
  Gold,
  Variable,
  BattlesWon,
  Steals,
  Forges,
  BigHit,
  LastStanding,
  OneColour,
  TrapCatch,
  LevelEight,
  Room,
  VarBits,
  Fight,
  Body,
  SummonColour,
  Tech,
  Ending,
};

struct OcIds {
  std::array<int, 16> values{};
  int count = 0;
  constexpr OcIds() = default;
  constexpr OcIds(std::initializer_list<int> list) {
    for (int value : list) values[static_cast<size_t>(count++)] = value;
  }
  constexpr const int* begin() const { return values.data(); }
  constexpr const int* end() const { return values.data() + count; }
  constexpr bool has(int value) const {
    for (int id : *this)
      if (id == value) return true;
    return false;
  }
};

// One achievement. `value`: the threshold of Recruits / Save gold / Count, the value of a Save or Ending variable. `var`: the variable of
// a Save, VarBits or Ending rule or of a Profile over its bits. `prev_ids`: the previous rooms of a RoomPair. `ids`: rooms, characters,
// model bytes, bit numbers, scenes, elements, items, or a Profile's members (rooms, scenes, model bytes, bits; the summons whose colours
// count). `members`: a Set's achievement ids, space-separated; one ending in `*` takes every row with that prefix.
struct OcRow {
  const char* id;
  const char* title;
  const char* description;
  OcCategory category;
  bool spoiler = false;
  OcRule rule;
  OcSource source = OcSource::None;
  int value = 0;
  int var = -1;
  OcIds prev_ids;
  OcIds ids;
  const char* members = "";
};

class OcAchievements {
public:
  static constexpr int kCharacters = 44;

  // The element record fields the rules read, indexed by element id.
  struct ElementInfo {
    int level = 0;
    // 0..5, -1 = none.
    int colour = -1;
    // The characters a double or triple tech names (0 for every other element).
    int casters = 0;
  };

  // One room sample outside battle.
  struct Sample {
    int room = 0, prev_room = 0;
    std::array<bool, kCharacters> recruited{};
    // The model byte of each party member.
    std::vector<int> party_bodies;
    std::uint32_t gold = 0;
    std::function<int(int)> var;
  };

  // The player's achievements file: unlock times (UTC, YYYY-MM-DDTHH:MMZ), counters, seen masks, and any other section as it was read.
  struct Profile {
    std::map<std::string, std::string> unlocked;
    std::map<std::string, std::uint64_t> counters;
    std::map<std::string, std::uint64_t> seen;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> other;
  };

  // A row's progress for the tab: a counter against its target, or the members seen of a profile set.
  struct Progress {
    std::uint64_t have = 0, need = 0;
  };

  // The built-in rows (oc_achievement_table.inc).
  static std::span<const OcRow> table();
  static std::optional<Profile> parse_profile(std::string_view text);
  static std::string serialize_profile(const Profile& profile);
  // The current UTC time as the file stores it.
  static std::string utc_now();

  // `rows` the achievements evaluated (tests give their own).
  explicit OcAchievements(std::span<const OcRow> rows = table());

  std::span<const OcRow> rows() const { return rows_; }

  // Replaces the profile; rows it already satisfies (counters, seen sets, sets) unlock at the next evaluation.
  void load(Profile profile);
  const Profile& profile() const { return profile_; }
  // The profile changed since the last call.
  bool take_dirty();
  // Rows unlocked since the last call, in unlock order.
  std::vector<size_t> take_unlocked();
  void set_elements(std::vector<ElementInfo> elements);
  // The double and triple techs (elements naming two or more casters), the members of a Profile over techs.
  const std::vector<int>& techs() const { return techs_; }
  void set_clock(std::function<std::string()> clock) { clock_ = std::move(clock); }
  // Off: nothing is evaluated. Blocked (the debug tools were used this session): nothing is evaluated or recorded.
  void set_enabled(bool on) { enabled_ = on; }
  void set_blocked(bool on) { blocked_ = on; }

  // `ticks` 60 Hz ticks of play; outside a battle returns the room samples now due (one every 60 ticks).
  int tick(int ticks, bool in_battle);
  void sample(const Sample& now);
  void battle_started();
  // Actions the battle has begun to present (BattleView::take_begun_actions).
  void battle_actions(std::span<const std::vector<battle::ActionEvent>> actions);
  // A field fight ended: `result` 0 won, 1 wiped, 2 fled, 3 or more scripted; `boss` the scene's boss flag; `standing` the party members
  // above 0 HP; `ring` the field-effect colours at the end.
  void battle_ended(int scene, int result, bool boss, int standing, const std::array<int, 3>& ring);
  // A script gave item `id` (op BB).
  void item_given(int id);
  void forged(int count);

  bool unlocked(size_t row) const;
  // The UTC unlock time, empty while locked.
  std::string unlock_time(size_t row) const;
  int unlocked_count() const;
  std::optional<Progress> progress(size_t row) const;

private:
  bool active() const { return enabled_ && !blocked_; }
  void unlock(size_t row);
  void check_sets();
  void check_counter(OcSource counter);
  void check_profiles();
  void add_counter(OcSource counter, std::uint64_t amount);
  // A profile set's member was seen.
  void mark(size_t row, int member);
  void see(OcSource source, int member);
  void happen(OcSource source);
  void element_cast(int element);
  void check_ring(const std::array<int, 3>& ring);
  void recheck();
  int member_count(const OcRow& row) const;
  int member_index(const OcRow& row, int member) const;

  std::span<const OcRow> rows_;
  Profile profile_;
  std::vector<ElementInfo> elements_;
  std::vector<int> techs_;
  std::function<std::string()> clock_;
  std::vector<size_t> new_unlocks_;
  EndingTracker endings_;
  bool enabled_ = false, blocked_ = false, dirty_ = false, recheck_ = true;
  int sample_ticks_ = 0;
  // A party cast was presented: the field colours at the start of the next action (or at the end) are the ones it left.
  bool ring_pending_ = false;
};

}  // namespace oc::game
