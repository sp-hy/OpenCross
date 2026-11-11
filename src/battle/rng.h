#pragma once
// Battle RNG. rnd(lo,hi) is inclusive; small ranges (hi < 0x100) walk a fixed 256-byte
// permutation table, large ranges use a libc-style rand(). Injectable so tests can script every roll.
#include <deque>
#include <vector>

#include "core/types.h"

namespace oc::battle {

class Rng {
 public:
  virtual ~Rng() = default;
  virtual int rnd(int lo, int hi) = 0;
  // libc rand() (15-bit): battle script op 1F draws from it, never from the permutation table. Test doubles script it as rnd(0, 0x7FFF).
  virtual int libc_rand() { return rnd(0, 0x7FFF); }
  // The generator's position (internal state and call count): two generators in step report the same value (Battle::state_hash).
  virtual u64 fingerprint() const = 0;
};

// The game's generator. `table` = the exe's 256-byte permutation; if it is not 256 bytes a seeded permutation is generated
// instead (tests, missing exe).
class GameRng final : public Rng {
 public:
  explicit GameRng(u32 seed, const std::vector<u8>& table = {});
  int rnd(int lo, int hi) override;
  int libc_rand() override;
  u32 calls() const { return calls_; }
  u64 fingerprint() const override { return static_cast<u64>(state_) << 32 | static_cast<u64>(idx_) << 24 | (calls_ & 0xFFFFFFu); }

 private:
  u8 table_[256]{};
  u8 idx_ = 0;
  u32 state_ = 1;
  u32 calls_ = 0;
};

// Test double: returns scripted values (clamped into [lo,hi]); once the script runs out it returns `fallback`
// (Lo = lo, Hi = hi, Mid = (lo+hi)/2).
class ScriptedRng final : public Rng {
 public:
  enum class Fallback { Lo, Hi, Mid };
  explicit ScriptedRng(Fallback fallback = Fallback::Lo) : fallback_(fallback) {}
  ScriptedRng(std::initializer_list<int> values, Fallback fallback = Fallback::Lo) : queue_(values), fallback_(fallback) {}
  void push(int value) { queue_.push_back(value); }
  int rnd(int lo, int hi) override;
  u64 fingerprint() const override { return static_cast<u64>(queue_.size()) << 32 | calls; }
  u32 calls = 0;

 private:
  std::deque<int> queue_;
  Fallback fallback_;
};

}  // namespace oc::battle
