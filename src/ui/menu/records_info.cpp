#include "ui/menu/records_info.h"

#include <algorithm>

#include "ui/menu/save_layout.h"

namespace oc::ui {

namespace {
constexpr int kStoryVar = 0;
}  // namespace

int story_point(int story) { return save_layout::location_key(story); }

bool story_reopens(const RecordsInfo& info, int last) {
  return std::any_of(info.story_drops.begin(), info.story_drops.end(), [&](const StoryDrop& drop) { return drop.to <= last && last < drop.from; });
}

bool character_missed(const RecordsInfo& info, const save::SaveSlot& slot, int character) {
  if (character < 0 || character >= RecordsInfo::kCharacters || slot.recruited(character) || slot.ng_flag() != 0) return false;
  const CharacterInfo& entry = info.characters[static_cast<size_t>(character)];
  return !entry.sites.empty() && entry.last_story < kStoryOpen && slot.var(kStoryVar) > entry.last_story && !story_reopens(info, entry.last_story);
}

bool character_closing(const RecordsInfo& info, const save::SaveSlot& slot, int character) {
  if (character < 0 || character >= RecordsInfo::kCharacters || slot.recruited(character) || character_missed(info, slot, character)) return false;
  const CharacterInfo& entry = info.characters[static_cast<size_t>(character)];
  if (entry.sites.empty() || entry.last_story >= kStoryOpen || story_reopens(info, entry.last_story)) return false;
  return story_point(entry.last_story) <= story_point(slot.var(kStoryVar)) + 1;
}

bool give_missed(const RecordsInfo& info, const GiveInfo& give, const save::SaveSlot& slot) {
  if (give.one_offs.empty() || give.repeatable || give.shop || give.enemy) return false;
  const int story = slot.var(kStoryVar);
  return std::all_of(give.one_offs.begin(), give.one_offs.end(), [&](const GiveInfo::OneOff& one_off) {
    return !one_off.flag.is_set(slot) && one_off.story_hi < story && !story_reopens(info, one_off.story_hi);
  });
}

std::vector<TreasureTally> tally_treasures(const RecordsInfo& info, const save::SaveSlot& slot) {
  std::vector<TreasureTally> out(info.areas.size());
  for (const TreasureInfo& treasure : info.treasures) {
    if (treasure.area < 0 || static_cast<size_t>(treasure.area) >= out.size()) continue;
    TreasureTally& tally = out[static_cast<size_t>(treasure.area)];
    ++tally.total;
    if (treasure.flag.is_set(slot)) ++tally.opened;
  }
  return out;
}

}  // namespace oc::ui
