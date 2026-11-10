#pragma once
// The seam between a versus BattleView and whoever plays the other side (rules/versus.md). The view applies its own player's commands to
// its Battle and hands every accepted one to the link; it applies the other side's commands as the link gives them, in the order the other
// game applied them, and hands the link its state hash after every command so that two games can tell they are still in step. Commands
// and units are the rules' (side A in units 0..2), whichever side this game plays.
//
// LocalVersusOpponent plays the other side in this game: battle::VersusAutoPlayer from the battle as it stands, while its side acts only
// (Battle::acting_side). NetworkVersusLink (versus_net_link.h) is the other game over the multiplayer server.
#include <deque>
#include <optional>

#include "battle/battle.h"
#include "battle/versus.h"

namespace oc::game {

class VersusLink {
 public:
  // Open: the battle goes on. Desync: the games' states differ; Closed: the other game is gone without a result. Both end the battle with
  // no result. Conceded: the other side lost by a decision outside the rules (a player who stayed away); Forfeited: this side lost so.
  enum class Status { Open, Desync, Closed, Conceded, Forfeited };
  // A player out of reach for now: this game's own connection (`self`) or the other player's, with the whole seconds left to come back.
  struct Away {
    bool self = false;
    int seconds = 0;
  };
  virtual ~VersusLink() = default;
  // A command this game's player gave and the rules accepted.
  virtual void send(const battle::Command& cmd) = 0;
  // The other side's next command, false when none is waiting. `battle` is this game's battle as it stands (a local opponent decides from
  // it; a link to another game does not need it).
  virtual bool poll(const battle::Battle& battle, battle::Command& cmd) = 0;
  // The state hash after the `seq`-th command applied (both games count the commands they applied, from 1, whichever side gave them).
  virtual void send_hash(u32 seq, u64 hash) = 0;
  virtual Status status() const = 0;
  // Who is out of reach now; nothing while both games are.
  virtual std::optional<Away> away() const { return std::nullopt; }
};

// A local opponent's generator is seeded with the battle seed XOR this, apart from the battle's own.
inline constexpr u32 kLocalOpponentSalt = 0x0B5E55EDu;

class LocalVersusOpponent final : public VersusLink {
 public:
  // `side`: the side it plays (0 = A); `seed`: its own generator.
  LocalVersusOpponent(int side, u32 seed);
  void send(const battle::Command&) override {}
  bool poll(const battle::Battle& battle, battle::Command& cmd) override;
  void send_hash(u32, u64) override {}
  Status status() const override { return Status::Open; }

 private:
  int side_ = 0;
  battle::VersusAutoPlayer player_;
  std::deque<battle::Command> queued_;
  // The attacker and target of the last engage it gave, -1 = none since its last other command.
  int engaged_actor_ = -1, engaged_target_ = -1;
};

}  // namespace oc::game
