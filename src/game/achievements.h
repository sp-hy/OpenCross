#pragma once
// When the Radical Dreamers Edition's achievements are earned. The game keeps no achievement flags in its story: what it watches is the
// pair (room, room before it), the fights started by scripts, a few items and script bits, element casts, the smith, and four save
// fields. All of that is the table in achievements.cpp; this class evaluates it and reports names, and knows nothing of Steam.
//
// The room rules are sampled once a second outside battle, so a room that is only passed through does not count: some pairs must hold
// for several samples. The endings are a small state machine: arriving from the beach at an ending's room arms it, and the ending
// room 9 awards it.
#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

namespace oc::game {

class Achievements {
public:
  // What the room rules read.
  struct Progress {
    int room = 0;
    int prev_room = 0;
    std::uint32_t window_frames = 0;
    bool all_recruited = false;
    int stars = 0;
    bool fled = false;
  };

  Achievements();

  // `ticks` 60 Hz ticks of play. Outside a battle the room rules run.
  void tick(int ticks, const Progress& progress, bool in_battle);
  // A field fight ended: `scene` its battle scene, `result` 0 won, 1 wiped, 2 fled, 3 or more scripted; `battles_won` the save's counter.
  void battle_ended(int scene, int result, std::uint32_t battles_won);
  // A script gave item `id` (ids as script ops use them) while in `room`.
  void item_given(int id, int room);
  // A script set the variable bit `bit_ref` while in `room`.
  void var_bit_set(std::uint32_t bit_ref, int room);
  void element_cast(int element);
  // The smith forged `id` and none was held before.
  void forged(int id);

  // API names earned since the last call, each once per run.
  std::vector<const char*> take_earned();

private:
  struct Delayed {
    int index;
    int passes;
  };

  void earn(int index);
  void earn_after(int index, int passes);
  void sample(const Progress& progress);
  void update_ending(int room, int prev_room);

  std::bitset<38> done_;
  std::vector<const char*> earned_;
  // The one award waiting out its delay; a later cast or flight replaces it. index 0 = none.
  Delayed pending_{0, 0};
  int field_ticks_ = 0;
  std::array<int, 4> pair_left_;
  int sample_ticks_ = 0;
  int ending_ = -1;
  int last_room_ = 0;
  int last_prev_room_ = 0;
  std::uint32_t frames_seen_ = 0;
  int stars_seen_ = -1;
  bool fled_seen_ = false;
};

}  // namespace oc::game
