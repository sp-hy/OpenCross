#pragma once
// What the record pages of the OpenCross row (Characters, Elements, Key items, Treasure) know from the room-script scan, as plain data
// the menu reads (game/records_info.h builds it), and the rules that read it against a save: which chances are gone, which close at the
// next story point, which treasures are opened. Story values are script variable 0, the story counter. It does not only rise: a chance
// whose last story value some story set can bring the counter back to is never called gone or closing.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "core/types.h"
#include "data/bestiary.h"
#include "save/cc_save.h"

namespace oc::ui {

// The highest story value a window can have: open-ended.
inline constexpr int kStoryOpen = 0x7FFF;

// A flag the scripts set to remember something: bit `bit` of global variable `var` (byte offset), or with bit < 0 the variable equal
// to `value`.
struct RecordFlag {
  int var = -1;
  int bit = -1;
  int value = 0;
  bool is_set(const save::SaveSlot& slot) const {
    if (var < 0) return false;
    const int current = slot.var(var);
    return bit >= 0 ? ((current >> bit) & 1) != 0 : current == value;
  }
};

// How a recruitment happens, from the tests on the path to it (bits): a dialogue choice, a key item held, a character in the party,
// only when another character has not joined.
enum RecruitHow : u8 { kHowChoice = 0x01, kHowItem = 0x02, kHowParty = 0x04, kHowAlone = 0x08 };

// One place where a character joins: its area (empty when the room has no place name), the story values that allow it
// ([story_lo, story_hi]), how, the key item it needs (kHowItem) and the character who must be in the party (kHowParty).
struct RecruitSite {
  std::string area;
  int story_lo = 0;
  int story_hi = kStoryOpen;
  u8 how = 0;
  int key_item = -1;
  int partner = -1;
};

struct CharacterInfo {
  std::vector<RecruitSite> sites;
  // The last story value at which some site still allows the recruitment; kStoryOpen when a site has no upper bound.
  int last_story = kStoryOpen;
};

// Where an element or a key item comes from: the areas of the script gives, the other sources (element shops; enemy drops and
// steals), whether some give can be had again (it sets no flag), and each one-off give's flag and last story value.
struct GiveInfo {
  struct OneOff {
    RecordFlag flag;
    int story_hi = kStoryOpen;
  };
  std::vector<std::string> areas;
  bool shop = false;
  bool enemy = false;
  bool repeatable = false;
  std::vector<OneOff> one_offs;
};

// A chest or pickup: the flag it sets once taken, its area (an index into RecordsInfo::areas), whether it is a chest, and what it gives
// (item ids in the script's numbering: below 0x200 an item, 0x200.. an element, 0x400.. a key item).
struct TreasureInfo {
  RecordFlag flag;
  int area = 0;
  bool chest = false;
  std::vector<int> gives;
};

// A story set the game may run while the counter is above the value it writes: it takes the counter from `from` down to `to`.
struct StoryDrop {
  int to = 0;
  int from = 0;
  auto operator<=>(const StoryDrop&) const = default;
};

// A row of the Treasure page: the place name (English; empty for the rooms without one) and the world it is shown with (None unless
// rooms of both worlds carry the name, data::places_in_both_worlds). Ordered by name, then Home before Another.
struct TreasureArea {
  std::string name;
  data::World world = data::World::None;
  auto operator<=>(const TreasureArea&) const = default;
};

struct RecordsInfo {
  static constexpr int kCharacters = 44;
  std::array<CharacterInfo, kCharacters> characters;
  // Every drop of the story counter the scan cannot rule out, sorted.
  std::vector<StoryDrop> story_drops;
  std::map<int, GiveInfo> elements;
  std::map<int, GiveInfo> key_items;
  // The treasure areas in list order (the last one has an empty name when some treasure's room has no place name).
  std::vector<TreasureArea> areas;
  std::vector<TreasureInfo> treasures;
};

// The story point of a story value: the number of save-caption thresholds it has reached (save_layout::location_key).
int story_point(int story);

// A window that ends at story value `last` can open again: some story drop goes from above `last` to `last` or below.
bool story_reopens(const RecordsInfo& info, int last);
// A character no longer joins in this save: not recruited, not a New Game+ game (carried-over characters can join again there), the
// scan knows where it joins, and every place has closed for good (the story counter is past its last value, and no drop reopens it).
bool character_missed(const RecordsInfo& info, const save::SaveSlot& slot, int character);
// Not recruited, not missed, the chance ends before the story point after the current one, and no drop reopens it.
bool character_closing(const RecordsInfo& info, const save::SaveSlot& slot, int character);
// A one-off element or key item no longer obtainable in this save: it has gives and no other source, none can be had again, and every
// one-off give is untaken (its flag clear) and closed for good (the story counter past its last value, and no drop reopens it).
bool give_missed(const RecordsInfo& info, const GiveInfo& give, const save::SaveSlot& slot);

struct TreasureTally {
  int opened = 0;
  int total = 0;
};
// Opened / total per area of info.areas (a treasure counts as opened when its flag is set in the save).
std::vector<TreasureTally> tally_treasures(const RecordsInfo& info, const save::SaveSlot& slot);

}  // namespace oc::ui
