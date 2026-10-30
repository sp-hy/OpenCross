#include "game/achievements.h"

#include <algorithm>

namespace oc::game {

namespace {

// Achievement numbers, the positions of the API names below.
enum Index {
  kWakeUp = 1,
  kDreamLabyrinth,
  kBeatLynx,
  kBeatDarkSerge,
  kSeaOfEden,
  kChronopolis,
  kBeatFate,
  kChronoCrossElement,
  kBeatTimeDevourer,
  kGoodEnding,
  kBadEnding,
  kDevelopersEnding,
  kAcaciaEnding,
  kDreamersEnding,
  kShopEnding,
  kMarbuleEnding,
  kLastStandEnding,
  kOrphanageEnding,
  kDarkFateEnding,
  kDragonGodEnding,
  kFled,
  kFlyingArrow,
  kHotShot,
  kForeverZero,
  kLunairetic,
  kDeltaForce,
  kZSlash,
  kVeteran,
  kAllRecruited,
  kRainbowWeapon,
  kRainbowArmorMade,
  kMastermune,
  kAllFrames,
  kFeedDragons,
  kGrandSlam,
  kRoulette,
  kGoldStars,
  kCount,
};

constexpr const char* kApiNames[kCount] = {
    nullptr,
    "ach_eve1_yumemezame",
    "ach_eve2_yumenaka",
    "ach_eve3_yamaneko",
    "ach_eve4_darks",
    "ach_eve5_shikaku",
    "ach_eve6_chronop",
    "ach_eve7_fate",
    "ach_eve8_chrinocross",
    "ach_eve9_tokiwokurau",
    "ach_end1_good",
    "ach_end2_bad",
    "ach_end3_kaihatu",
    "ach_end4_akasia",
    "ach_end5_akasia",
    "ach_end6_shop",
    "ach_end7_mabure",
    "ach_end8_yamaneko",
    "ach_end9_youtien",
    "ach_end10_kuroi",
    "ach_end11_ryujin",
    "ach_battle1_condition",
    "ach_battle2_sky",
    "ach_battle3_heat",
    "ach_battle4_zero",
    "ach_battle5_tuki",
    "ach_battle6_mixdelta",
    "ach_battle7_zetgiri",
    "ach_battle8_master",
    "ach_nakama1_jinkaku",
    "ach_item1_nijibuki",
    "ach_item2_nijibougu",
    "ach_item3_gran",
    "ach_frame1_master",
    "ach_minigame1_esa",
    "ach_minigame2_gran",
    "ach_minigame3_roulette",
    "ach_level1_gold",
};

constexpr int kSampleTicks = 60;
constexpr int kAnyRoom = -1;

struct RoomPair {
  int index, prev_room, room, samples;
};
constexpr RoomPair kRoomPairs[] = {
    {kWakeUp, 0x1A, 0x1F, 20},
    {kDreamLabyrinth, 0x164, 0x166, 6},
    {kSeaOfEden, 0x1D7, 0xFF, 1},
    {kChronopolis, 0x103, 0x1DD, 1},
};

struct SceneWon {
  int index, scene;
};
constexpr SceneWon kScenesWon[] = {
    {kBeatLynx, 433},
    {kBeatDarkSerge, 473},
    {kBeatFate, 462},
    {kBeatTimeDevourer, 472},
};

struct ItemGiven {
  int index, item, room;
};
constexpr ItemGiven kItemsGiven[] = {
    {kChronoCrossElement, 0x42D, kAnyRoom},
    {kMastermune, 0x26, kAnyRoom},
    {kFeedDragons, 0x9E, 0x98},
    {kRoulette, 0x06, 0x182},
};

constexpr std::uint32_t kGrandSlamBit = 0x16C6;
constexpr int kGrandSlamRoom = 0x187;

// Ticks from the event to the award.
constexpr int kFleeDelay = 180;
constexpr int kCastDelay = 60;
constexpr int kDeathCastDelay = 300;
struct ElementCast {
  int index, element, delay;
};
constexpr ElementCast kElementCasts[] = {
    {kFlyingArrow, 162, kCastDelay},
    {kHotShot, 165, kCastDelay},
    {kForeverZero, 158, kCastDelay},
    {kLunairetic, 228, kCastDelay},
    {kDeltaForce, 137, kCastDelay},
    {kZSlash, 136, kCastDelay},
    {kBeatTimeDevourer, 526, kDeathCastDelay},
};

constexpr int kRainbowWeapons[] = {0x25, 0x2C, 0x2F, 0x35, 0x3C, 0x45, 0x4D, 0x55, 0x5D, 0x63, 0x6A, 0x73, 0x7A};
constexpr int kRainbowArmor[] = {0x9B, 0xA1, 0xC7};

constexpr std::uint32_t kBattlesForVeteran = 500;
constexpr std::uint32_t kEveryWindowFrame = 0x7FFF;
constexpr int kMaxStars = 99;

constexpr int kNoEnding = -1;
constexpr int kBeachA = 0x38, kBeachB = 0x39;
constexpr int kEndingRoom = 9;
constexpr int kTitleRoom = 0;
constexpr int kGoodEndingRoom = 0x216;
constexpr int kShopEndingRoom = 0x1F2;

struct EndingArm {
  int room, ending;
};
constexpr EndingArm kEndingArms[] = {
    {0x1CB, kDragonGodEnding}, {0x1CE, kDevelopersEnding}, {0x1E8, kAcaciaEnding},    {0x1EC, kLastStandEnding},
    {0x1ED, kDreamersEnding},  {0x1F8, kMarbuleEnding},    {0x21C, kDarkFateEnding},  {0x21E, kOrphanageEnding},
};

}  // namespace

Achievements::Achievements() {
  for (size_t i = 0; i < pair_left_.size(); ++i) pair_left_[i] = kRoomPairs[i].samples;
}

void Achievements::earn(int index) {
  if (done_[static_cast<size_t>(index)]) return;
  done_[static_cast<size_t>(index)] = true;
  earned_.push_back(kApiNames[index]);
}

void Achievements::earn_after(int index, int passes) {
  pending_ = {index, passes};
}

void Achievements::tick(int ticks, const Progress& progress, bool in_battle) {
  if (pending_.index != 0) {
    // The delay counts main-loop passes: one tick each in a battle, two in the field.
    field_ticks_ = in_battle ? 0 : field_ticks_ + ticks;
    pending_.passes -= in_battle ? ticks : field_ticks_ / 2;
    field_ticks_ %= 2;
    if (pending_.passes <= 0) {
      earn(pending_.index);
      pending_ = {0, 0};
    }
  }

  if (progress.fled && !fled_seen_ && !done_[kFled]) earn_after(kFled, kFleeDelay);
  fled_seen_ = progress.fled;

  if (stars_seen_ == kMaxStars - 1 && progress.stars == kMaxStars) earn(kGoldStars);
  stars_seen_ = progress.stars;

  if (in_battle) return;
  sample_ticks_ += ticks;
  while (sample_ticks_ >= kSampleTicks) {
    sample_ticks_ -= kSampleTicks;
    sample(progress);
  }
}

void Achievements::sample(const Progress& progress) {
  for (size_t i = 0; i < pair_left_.size(); ++i) {
    const RoomPair& pair = kRoomPairs[i];
    if (progress.prev_room != pair.prev_room || progress.room != pair.room) pair_left_[i] = pair.samples;
    else if (--pair_left_[i] == 0) earn(pair.index);
  }
  update_ending(progress.room, progress.prev_room);
  if (progress.all_recruited) earn(kAllRecruited);
  if (frames_seen_ < progress.window_frames && progress.window_frames == kEveryWindowFrame) earn(kAllFrames);
  frames_seen_ = progress.window_frames;
}

// The good ending's room jumps to room 9 and room 9 then jumps to itself; only that second jump awards it, so it is told from the others
// by the previous room changing with the room standing still. Reaching room 9 straight from the beach with nothing armed is the bad
// ending.
void Achievements::update_ending(int room, int prev_room) {
  if (prev_room != last_prev_room_) {
    if (room == kEndingRoom && prev_room == kEndingRoom && ending_ == kGoodEnding) {
      earn(kGoodEnding);
      ending_ = kNoEnding;
    }
    last_prev_room_ = prev_room;
  }
  if (room == last_room_) return;
  const bool from_beach = prev_room == kBeachA || prev_room == kBeachB;
  if (prev_room == kBeachA && room == kEndingRoom) ending_ = kBadEnding;
  if (from_beach)
    for (const EndingArm& arm : kEndingArms)
      if (room == arm.room) ending_ = arm.ending;
  if (room == kGoodEndingRoom) {
    ending_ = kGoodEnding;
  } else if (room == kShopEndingRoom) {
    if (ending_ == kDreamersEnding) ending_ = kShopEnding;
  } else if (room == kTitleRoom) {
    ending_ = kNoEnding;
  } else if (room == kEndingRoom) {
    const bool straight_from_beach = prev_room == kBeachB && last_room_ == kBeachB;
    if (ending_ == kNoEnding && straight_from_beach) ending_ = kBadEnding;
    if (ending_ != kNoEnding && ending_ != kGoodEnding) {
      earn(ending_);
      ending_ = kNoEnding;
    }
  }
  last_room_ = room;
}

void Achievements::battle_ended(int scene, int result, std::uint32_t battles_won) {
  if (result != 0 && result != 3) return;
  for (const SceneWon& won : kScenesWon)
    if (scene == won.scene) earn(won.index);
  if (battles_won >= kBattlesForVeteran) earn(kVeteran);
}

void Achievements::item_given(int id, int room) {
  for (const ItemGiven& given : kItemsGiven)
    if (id == given.item && (given.room == kAnyRoom || room == given.room)) earn(given.index);
}

void Achievements::var_bit_set(std::uint32_t bit_ref, int room) {
  if (bit_ref == kGrandSlamBit && room == kGrandSlamRoom) earn(kGrandSlam);
}

void Achievements::element_cast(int element) {
  for (const ElementCast& cast : kElementCasts)
    if (element == cast.element) earn_after(cast.index, cast.delay);
}

void Achievements::forged(int id) {
  if (std::find(std::begin(kRainbowWeapons), std::end(kRainbowWeapons), id) != std::end(kRainbowWeapons)) earn(kRainbowWeapon);
  if (std::find(std::begin(kRainbowArmor), std::end(kRainbowArmor), id) != std::end(kRainbowArmor)) earn(kRainbowArmorMade);
}

std::vector<const char*> Achievements::take_earned() {
  std::vector<const char*> names;
  names.swap(earned_);
  return names;
}

}  // namespace oc::game
