// The local versus opponent: the auto player's batches, each command checked against the battle when its turn comes (a Dopplegang pick
// also after the cast ended its side's turn).
#include "game/battle/versus_link.h"

namespace oc::game {

namespace {

// Batches the auto player may offer in one call before the turn is simply ended.
constexpr int kBatchTries = 16;

}  // namespace

LocalVersusOpponent::LocalVersusOpponent(int side, u32 seed) : side_(side & 1), player_(seed, {.early_end = false, .timeouts = false, .surrender = false}) {}

bool LocalVersusOpponent::poll(const battle::Battle& battle, battle::Command& cmd) {
  if (!battle.versus() || battle.outcome() != battle::Outcome::Ongoing || battle.acting_side() != side_) {
    queued_.clear();
    return false;
  }
  for (int tries = 0; tries < kBatchTries; ++tries) {
    if (queued_.empty() && battle.form_owed() >= 0) {
      cmd = {};
      cmd.kind = battle::Command::Kind::Form;
      cmd.actor = battle.form_owed();
      return true;
    }
    if (queued_.empty()) {
      const std::vector<battle::Command> batch = player_.next(battle);
      queued_.assign(batch.begin(), batch.end());
    }
    while (!queued_.empty()) {
      cmd = queued_.front();
      queued_.pop_front();
      if (!battle.validate(cmd)) continue;
      // An attack on a new target is engaged first, as a player's target confirm is: the attacker runs up before its hits.
      if (cmd.kind == battle::Command::Kind::Attack && cmd.target >= 0 && (cmd.actor != engaged_actor_ || cmd.target != engaged_target_)) {
        queued_.push_front(cmd);
        engaged_actor_ = cmd.actor;
        engaged_target_ = cmd.target;
        cmd.kind = battle::Command::Kind::Engage;
        cmd.chain.clear();
        return true;
      }
      if (cmd.kind != battle::Command::Kind::Attack) engaged_actor_ = engaged_target_ = -1;
      return true;
    }
  }
  cmd = {};
  cmd.kind = battle::Command::Kind::EndTurn;
  cmd.actor = battle::side_first(side_);
  return true;
}

}  // namespace oc::game
