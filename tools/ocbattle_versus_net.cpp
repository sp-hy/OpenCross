// ocbattle versus-net - versus battles played end to end over a running multiplayer server, as two players' games would: two
// MultiplayerClient connections in this process challenge and accept, exchange their sides (pvp_side), and each runs its own Battle with
// its own random player (game::LocalVersusOpponent) for its side and game::NetworkVersusLink for the other, so every command, state check
// and turn goes through the server. The two games' battles are compared at the end (outcome, commands applied, state hash); a link that
// ends early (a desync, a gap, a forfeit) is a failure.
//   ocbattle versus-net <ws-url> [--fuzz N] [--cut K] [--timeout S] [-v]
// --fuzz N plays N battles (default 1), each with new player ids; --cut K drops the second game's connection after it applied K commands
// (the client reconnects and resumes the battle); --timeout caps each battle in seconds (default 300).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "battle/battle.h"
#include "battle/rng.h"
#include "battle/versus.h"
#include "data/tables.h"
#include "game/battle/versus_link.h"
#include "game/battle/versus_net_link.h"
#include "net/multiplayer_client.h"

using namespace oc;

namespace {

// The two games' parties: characters at this star (versus_test_side).
constexpr int kStar = 20;
const std::vector<int> kPartyFirst = {0, 1, 2}, kPartySecond = {3, 4, 26};
// Seeds each game's own random player apart from the battle seed.
constexpr u32 kPlayerSalt = 0x7E57AB1Eu;
constexpr auto kTick = std::chrono::milliseconds(5);
constexpr double kConnectTime = 15.0;

// The WebSocket transport with a switch that drops the connection as a network failure would (a Closed event, no close handshake).
class CuttableTransport final : public net::Transport {
 public:
  explicit CuttableTransport(std::unique_ptr<net::Transport> inner) : inner_(std::move(inner)) {}
  void connect(const std::string& url) override { inner_->connect(url); }
  void send(std::string text) override { inner_->send(std::move(text)); }
  bool poll(net::TransportEvent& event) override {
    if (!cut_.empty()) {
      event = cut_.front();
      cut_.pop_front();
      return true;
    }
    return inner_->poll(event);
  }
  void close() override { inner_->close(); }
  void cut() {
    inner_->close();
    cut_.push_back({net::TransportEvent::Kind::Closed, "cut by the test"});
  }

 private:
  std::unique_ptr<net::Transport> inner_;
  std::deque<net::TransportEvent> cut_;
};

struct Player {
  CuttableTransport* transport = nullptr;
  std::unique_ptr<net::MultiplayerClient> client;
  std::optional<net::PvpStart> start;
  std::string side_bytes;
};

struct Game {
  battle::GameRng rng;
  battle::VersusBrain brain;
  battle::Battle battle;
  int side;
  game::LocalVersusOpponent own;
  game::NetworkVersusLink link;
  u32 applied = 0;

  Game(const data::GameTables& tables, net::MultiplayerClient& client, u32 seed, int game_side)
      : rng(seed, tables.rng_table), battle(tables, rng, brain), side(game_side), own(game_side, seed ^ kPlayerSalt ^ static_cast<u32>(game_side)),
        link(client, game_side) {}

  // One command of whichever side acts: this game's own random player, or the other game's through the link. False when the rules refuse it.
  bool tick(bool verbose, const char* name) {
    if (battle.outcome() != battle::Outcome::Ongoing || link.status() != game::VersusLink::Status::Open) return true;
    battle::Command cmd;
    const bool local = battle.acting_side() == side;
    if (!(local ? own.poll(battle, cmd) : link.poll(battle, cmd))) return true;
    std::string why;
    if (!battle.apply(cmd, &why)) {
      std::printf("  %s: command %u of the %s side refused: %s\n", name, applied + 1, local ? "own" : "other", why.c_str());
      return false;
    }
    if (local) link.send(cmd);
    link.send_hash(++applied, battle.state_hash());
    if (verbose) std::printf("  %s [%u] %s %s %016llx\n", name, applied, local ? "own  " : "other", game::versus_command_text(cmd).c_str(),
                             static_cast<unsigned long long>(battle.state_hash()));
    battle.drain_events();
    battle.drain_log();
    return true;
  }
};

const char* status_name(game::VersusLink::Status status) {
  switch (status) {
    case game::VersusLink::Status::Open: return "open";
    case game::VersusLink::Status::Desync: return "desync";
    case game::VersusLink::Status::Closed: return "closed";
    case game::VersusLink::Status::Conceded: return "conceded";
    case game::VersusLink::Status::Forfeited: return "forfeited";
  }
  return "?";
}

double seconds_since(std::chrono::steady_clock::time_point epoch) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count(); }

// Updates both clients until `done` holds or `limit` seconds pass.
template <class Done>
bool pump(Player& first, Player& second, std::chrono::steady_clock::time_point epoch, double limit, Done done) {
  const double end = seconds_since(epoch) + limit;
  while (seconds_since(epoch) < end) {
    first.client->update(seconds_since(epoch));
    second.client->update(seconds_since(epoch));
    if (done()) return true;
    std::this_thread::sleep_for(kTick);
  }
  return false;
}

// One battle; true when both games finished it in step.
bool play(const data::GameTables& tables, const std::string& url, int index, int cut, double timeout, bool verbose, long long& commands) {
  const auto epoch = std::chrono::steady_clock::now();
  Player players[2];
  for (int i = 0; i < 2; ++i) {
    auto transport = std::make_unique<CuttableTransport>(net::make_websocket_transport("OpenCross versus-net"));
    players[i].transport = transport.get();
    players[i].client = std::make_unique<net::MultiplayerClient>(std::move(transport));
    net::LocalPresence local;
    local.room = "versus_net";
    players[i].client->set_local(local);
    players[i].client->start(url, {net::new_player_id(), i == 0 ? "NetA" : "NetB", kStar, "versus-net"});
  }
  Player& first = players[0];
  Player& second = players[1];
  const auto connected = [&] {
    return first.client->state() == net::MultiplayerClient::State::Connected && second.client->state() == net::MultiplayerClient::State::Connected;
  };
  if (!pump(first, second, epoch, kConnectTime, connected)) {
    std::printf("battle %d: could not connect (%s / %s)\n", index, first.client->status_text().c_str(), second.client->status_text().c_str());
    return false;
  }
  if (!first.client->challenge(second.client->pid(), "NetB", {})) return false;
  if (!pump(first, second, epoch, kConnectTime, [&] { return second.client->incoming().has_value(); })) {
    std::printf("battle %d: the challenge did not arrive\n", index);
    return false;
  }
  second.client->accept_challenge({});
  const bool started = pump(first, second, epoch, kConnectTime, [&] {
    for (Player& player : players)
      if (!player.start) player.start = player.client->take_pvp_start();
    return first.start && second.start;
  });
  if (!started) {
    std::printf("battle %d: no pvp_start\n", index);
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    players[i].side_bytes = battle::encode_versus_side(battle::versus_test_side(tables, i == 0 ? kPartyFirst : kPartySecond, kStar));
    players[i].client->send_pvp_side(players[i].side_bytes);
  }
  if (!pump(first, second, epoch, kConnectTime, [&] { return first.client->pvp_opponent_side() && second.client->pvp_opponent_side(); })) {
    std::printf("battle %d: the sides were not exchanged\n", index);
    return false;
  }
  const u32 seed = first.start->seed;
  const int field = battle::versus_battlefield(tables, seed);
  std::unique_ptr<Game> games[2];
  for (int i = 0; i < 2; ++i) {
    Player& player = players[i];
    const int side = player.start->first == player.client->pid() ? 0 : 1;
    std::optional<battle::VersusSide> own = battle::decode_versus_side(player.side_bytes);
    std::optional<battle::VersusSide> other = battle::decode_versus_side(*player.client->pvp_opponent_side());
    games[i] = std::make_unique<Game>(tables, *player.client, seed, side);
    if (!own || !other || !games[i]->battle.start_versus(side == 0 ? *own : *other, side == 0 ? *other : *own, field, player.start->opening_stamina)) {
      std::printf("battle %d: a side cannot fight\n", index);
      return false;
    }
  }
  std::printf("battle %d: %s, seed %u, battlefield %d, %s plays side A, sides of %zu and %zu bytes\n", index, first.start->battle.c_str(), seed, field,
              games[0]->side == 0 ? "NetA" : "NetB", first.side_bytes.size(), second.side_bytes.size());
  bool refused = false, was_cut = false;
  const auto over = [&] {
    if (cut > 0 && !was_cut && games[1]->applied >= static_cast<u32>(cut)) {
      second.transport->cut();
      was_cut = true;
      std::printf("  NetB's connection cut after command %u\n", games[1]->applied);
    }
    for (int i = 0; i < 2; ++i) refused = !games[i]->tick(verbose, i == 0 ? "NetA" : "NetB") || refused;
    bool finished = !refused;
    for (const auto& game : games)
      finished = finished && (game->battle.outcome() != battle::Outcome::Ongoing || game->link.status() != game::VersusLink::Status::Open);
    return refused || finished;
  };
  const bool ended = pump(first, second, epoch, timeout, over);
  const Game& one = *games[0];
  const Game& two = *games[1];
  const bool in_step = ended && !refused && one.battle.outcome() != battle::Outcome::Ongoing && one.battle.outcome() == two.battle.outcome() &&
                       one.applied == two.applied && one.battle.state_hash() == two.battle.state_hash();
  commands += one.applied;
  std::printf("  %s: %u commands, outcome %d (%s), final hash %016llx | %s: %u commands, outcome %d (%s), final hash %016llx -> %s\n", "NetA",
              one.applied, static_cast<int>(one.battle.outcome()), status_name(one.link.status()), static_cast<unsigned long long>(one.battle.state_hash()),
              "NetB", two.applied, static_cast<int>(two.battle.outcome()), status_name(two.link.status()),
              static_cast<unsigned long long>(two.battle.state_hash()), in_step ? "in step" : ended ? "DIVERGED" : "TIMED OUT");
  for (int i = 0; i < 2; ++i) {
    const int winner = games[i]->battle.winner();
    players[i].client->end_pvp(!in_step ? net::PvpResult::Aborted
                               : winner < 0 ? net::PvpResult::Draw
                               : winner == games[i]->side ? net::PvpResult::Won
                                                          : net::PvpResult::Lost);
  }
  pump(first, second, epoch, 0.3, [] { return false; });
  return in_step;
}

}  // namespace

int cmd_versus_net(const data::GameTables& tables, const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "usage: ocbattle versus-net <ws-url> [--fuzz N] [--cut K] [--timeout S] [-v]\n");
    return 2;
  }
  int battles = 1, cut = 0;
  double timeout = 300.0;
  bool verbose = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const bool has_value = i + 1 < args.size();
    if (args[i] == "--fuzz" && has_value) battles = std::max(1, std::atoi(args[++i].c_str()));
    else if (args[i] == "--cut" && has_value) cut = std::atoi(args[++i].c_str());
    else if (args[i] == "--timeout" && has_value) timeout = std::atof(args[++i].c_str());
    else if (args[i] == "-v") verbose = true;
  }
  int failed = 0;
  long long commands = 0;
  for (int index = 1; index <= battles; ++index)
    if (!play(tables, args[0], index, cut, timeout, verbose, commands)) ++failed;
  std::printf("versus-net: %d battles, %lld commands, %d not in step\n", battles, commands, failed);
  return failed == 0 ? 0 : 1;
}
