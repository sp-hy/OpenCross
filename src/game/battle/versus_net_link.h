#pragma once
// The versus link to the other player's game over the multiplayer server (formats/multiplayer.md section 8.4). This game's accepted
// commands go out as pvp_cmd with their seq and state hash; the other game's arrive in its order and are handed to the view only while
// that side acts, each checked to be the next command of the stream; after every command the other game applied, this game's state hash
// goes back as pvp_hash, and the other game's hashes are compared with this game's. A hash that differs, a command out of order or one
// that cannot be read, the server's word that the battle is gone, this game's own connection gone past the away time, or the other game
// silent for kSilenceLimit while it should act end the link (Status).
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "game/battle/versus_link.h"
#include "net/multiplayer_client.h"

namespace oc::game {

// A command as the link writes it: kind, actor, target, cell, form, timeout, swap_in, then the chain's strengths, as decimal integers
// separated by single spaces (Command::Kind in its declared order, 0 = Attack).
std::string versus_command_text(const battle::Command& cmd);
// Nothing for text that is not a command or holds a value out of its field's range.
std::optional<battle::Command> parse_versus_command(std::string_view text);

class NetworkVersusLink final : public VersusLink {
 public:
  // The other game counts as gone after this long without a message while its side should act; this game gives its own dropped
  // connection up kSelfGrace after the server's away time.
  static constexpr double kSilenceLimit = 90.0;
  static constexpr double kSelfGrace = 5.0;
  // State hashes of this game's own commands kept for the other game's check.
  static constexpr size_t kHashKeep = 4096;
  // The other game's commands waiting to be applied; more than this ends the link.
  static constexpr size_t kCommandKeep = 1024;

  // `client` holds the battle (pvp_battle) and must outlive the link; `side` is the side this game plays (0 = A).
  NetworkVersusLink(net::MultiplayerClient& client, int side);
  void send(const battle::Command& cmd) override;
  bool poll(const battle::Battle& battle, battle::Command& cmd) override;
  void send_hash(u32 seq, u64 hash) override;
  Status status() const override;
  std::optional<Away> away() const override;

 private:
  // Takes the client's received steps: state checks are compared at once, commands wait for poll().
  void pull();
  void mismatch(u32 seq, u64 ours, u64 theirs);

  net::MultiplayerClient& client_;
  std::string battle_;
  int side_ = 0;
  std::optional<battle::Command> pending_;
  std::deque<net::PvpStep> commands_;
  std::map<u32, u64> own_hashes_;
  // The other game's hash sent with the command poll() handed out last.
  std::optional<net::PvpStep> expected_;
  u32 applied_ = 0;
  // When this game began waiting for the other's command, and when the other game last moved the battle on.
  double waiting_since_ = -1.0, progress_at_ = -1.0;
  bool desync_ = false, broken_ = false, silent_ = false;
};

}  // namespace oc::game
