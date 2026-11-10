#pragma once
// What two versus players exchange and derive before the battle starts: a side (party, summon stars, Dopplegang pool) as a compact,
// versioned byte string, and the battlefield picked from the battle seed. Both games then build the same Battle (Battle::start_versus).
// Also a random legal player for either side (the headless fuzz, a local opponent, a route's auto play).
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "battle/battle.h"

namespace oc::battle {

// The format of encode_versus_side; decode_versus_side refuses any other.
inline constexpr u8 kVersusSideFormat = 1;

// Every field of every member (names included, as UTF-8), the stars and the pool. Integers are LEB128 varints (signed ones zigzagged),
// flag arrays packed into bits.
std::string encode_versus_side(const VersusSide& side);
// Nothing on a wrong format byte, a truncated or overlong string, or a value out of its field's range.
std::optional<VersusSide> decode_versus_side(std::string_view bytes);

// The stage (btbgdat row) of a versus battle: the backgrounds of the ordinary scenes (no boss flag, at least one enemy group, background
// below 100), sorted and without repeats, one of them picked by a generator seeded from the battle seed. -1 when there is none.
int versus_battlefield(const data::GameTables& tables, u32 seed);
// A built-in side (tests, a local opponent without a party of its own): those characters (at most 3) at that star, kVersusTestStars summon
// stars and every creature of the Dopplegang table in the pool.
inline constexpr int kVersusTestStars = 5;
VersusSide versus_test_side(const data::GameTables& tables, const std::vector<int>& char_ids, int star);
// The scene a versus battle is drawn as (its stage, party spots and facings; never its enemies or script): the lowest ordinary scene of the
// battlefield's background. -1 when there is none.
int versus_carrier_scene(const data::GameTables& tables, int battlefield);

// Random legal play for the side whose turn it is, from its own generator: a Focus before a new actor's command, an ElementMenu before a
// cast and the Dopplegang pick after one, an End Turn now and then (one in ten a timeout), a Run very rarely. The options turn the End
// Turn before the side is spent, the timeouts and the Run off (the generator is drawn from the same way). A batch may hold commands the
// rules refuse (a Form after a cast the trap caught): the caller validates each one when its turn comes.
class VersusAutoPlayer {
 public:
  struct Options {
    bool early_end = true, timeouts = true, surrender = true;
  };
  explicit VersusAutoPlayer(u32 seed, Options options = {}) : rng_(seed), options_(options) {}
  std::vector<Command> next(const Battle& battle);

 private:
  int pick(const std::vector<int>& items);
  std::vector<int> standing(const Battle& battle, int side) const;
  void attack(const Battle& battle, Command& cmd);
  // A usable cell and a target its element may take; false when the actor has none.
  bool element(const Battle& battle, Command& cmd);
  GameRng rng_;
  Options options_;
  int focused_ = -1;
};

}  // namespace oc::battle
