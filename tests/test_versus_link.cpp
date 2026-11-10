// The versus battle link over scripted transports and a hand-driven clock: the client's side exchange, paced steps, relay numbers,
// absence and resume, and game::NetworkVersusLink between two games through a relay that numbers messages as the server does: a whole
// battle in step, the order check, a state check that differs, a resume after a dropped connection, the forfeits and the silence limit.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "battle/battle.h"
#include "battle/rng.h"
#include "battle/versus.h"
#include "game/battle/versus_link.h"
#include "game/battle/versus_net_link.h"
#include "net/json.h"
#include "net/multiplayer_client.h"

using namespace oc;
using net::Json;
using net::MultiplayerClient;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

struct Wire {
  std::vector<std::string> sent;
  std::deque<net::TransportEvent> incoming;
  int connects = 0;

  void receive(const std::string& json) { incoming.push_back({net::TransportEvent::Kind::Message, json}); }
  std::vector<Json> take() {
    std::vector<Json> out;
    for (const std::string& text : sent) out.push_back(net::parse_json(text).value_or(Json()));
    sent.clear();
    return out;
  }
};

class FakeTransport final : public net::Transport {
 public:
  explicit FakeTransport(Wire& wire) : wire_(wire) {}
  void connect(const std::string&) override { ++wire_.connects; }
  void send(std::string text) override { wire_.sent.push_back(std::move(text)); }
  bool poll(net::TransportEvent& event) override {
    if (wire_.incoming.empty()) return false;
    event = wire_.incoming.front();
    wire_.incoming.pop_front();
    return true;
  }
  void close() override { wire_.incoming.clear(); }

 private:
  Wire& wire_;
};

const std::string kBattle = "00ff00ff00ff00ff";

// A client welcomed as `pid`, in a battle against `other` (its own challenge, accepted), with the greeting's messages taken.
struct Player {
  Wire wire;
  MultiplayerClient client{std::make_unique<FakeTransport>(wire)};
  std::string pid, other;
  double now = 0.0;

  Player(const std::string& own, const std::string& opponent, bool first) : pid(own), other(opponent) {
    net::LocalPresence local;
    local.room = "alu_a01f";
    client.set_local(local);
    client.start("ws://127.0.0.1:8787/ws", {std::string(32, own == "pa00000000000000" ? 'a' : 'b'), own, 3, "test"});
    step(0.0);
    open();
    CHECK(client.challenge(other, "Other", {}));
    wire.receive(std::format(R"({{"t":"pvp_start","battle":"{}","seed":9,"first":"{}","opponent":"{}","name":"Other","star":3,"card":{{}},"opening_stamina":35}})",
                             kBattle, first ? pid : other, other));
    step(0.0);
    CHECK(client.take_pvp_start().has_value() && client.pvp_battle() == kBattle);
    wire.take();
  }
  void open() {
    wire.incoming.push_back({net::TransportEvent::Kind::Opened, {}});
    step(0.0);
    wire.receive(std::format(R"({{"t":"welcome","pid":"{}","limits":{{}}}})", pid));
    step(0.0);
  }
  void drop() {
    wire.incoming.push_back({net::TransportEvent::Kind::Closed, "gone"});
    step(0.0);
  }
  void step(double dt) {
    now += dt;
    client.update(now);
  }
  // Time passes a second at a time while the server answers the pings.
  void idle(double seconds) {
    for (double left = seconds; left > 0; left -= 1.0) {
      wire.receive(R"({"t":"pong"})");
      step(std::min(1.0, left));
    }
  }
};

// The server's relay for two players: every battle message one sends goes to the other with the next relay number; `hold` keeps a
// player's messages from being delivered (its connection is down).
struct Relay {
  Player& a;
  Player& b;
  std::int64_t number_a = 0, number_b = 0;
  std::uint32_t seq_a = 0, seq_b = 0;
  bool hold_a = false;
  std::vector<Json> log_a;

  void pass() {
    for (int round = 0; round < 2; ++round) {
      forward(a, b, number_b, seq_a, false);
      forward(b, a, number_a, seq_b, hold_a);
    }
  }
  void forward(Player& from, Player& to, std::int64_t& number, std::uint32_t& seq, bool hold) {
    for (Json& message : from.wire.take()) {
      const std::string& type = message.string_of("t");
      if (type != "pvp_side" && type != "pvp_cmd" && type != "pvp_hash") continue;
      if (type != "pvp_side") seq = static_cast<std::uint32_t>(message.int_of("seq"));
      message.set("n", ++number);
      message.set("ack", Json(nullptr));
      if (&to == &a) log_a.push_back(message);
      if (!hold) to.wire.receive(net::to_json(message));
    }
  }
};

data::GameTables make_tables() {
  data::GameTables tables;
  tables.elements.resize(2);
  auto& beam = tables.elements[1];
  beam.raw[0] = 1, beam.raw[1] = 1, beam.raw[5] = 0x80, beam.raw[6] = 1, beam.raw[7] = 0, beam.raw[8] = 12, beam.raw[0xC] = 20;
  tables.enemies.resize(1);
  return tables;
}

battle::VersusSide make_side(int first_char) {
  battle::VersusSide side;
  for (int i = 0; i < 3; ++i) {
    battle::PartyMember member;
    member.char_id = first_char + i;
    member.name = std::format("M{}", first_char + i);
    member.stat = {60, 9, 5, 85, 8, 7, 7};
    member.hp = 60;
    member.snap = member.stat;
    member.lv99 = {850, 88, 78, 95, 25, 52, 43};
    member.innate = 0x08;
    member.dur = 13;
    member.cells[0] = 0x4001;
    member.unlocked[0] = true;
    side.party.push_back(member);
  }
  return side;
}

// One game: its battle, its own random player and the link to the other game.
struct Game {
  data::GameTables tables = make_tables();
  battle::GameRng rng{9};
  battle::VersusBrain brain;
  battle::Battle battle{tables, rng, brain};
  int side;
  game::LocalVersusOpponent own;
  game::NetworkVersusLink link;
  std::uint32_t applied = 0;

  Game(Player& player, int game_side) : side(game_side), own(game_side, 100 + static_cast<u32>(game_side)), link(player.client, game_side) {
    CHECK(battle.start_versus(make_side(0), make_side(3), -1));
  }
  // The next command of whichever side acts: this game's own player, or the other game's through the link.
  void tick() {
    if (battle.outcome() != battle::Outcome::Ongoing || link.status() != game::VersusLink::Status::Open) return;
    battle::Command cmd;
    const bool local = battle.acting_side() == side;
    if (!(local ? own.poll(battle, cmd) : link.poll(battle, cmd))) return;
    CHECK(battle.apply(cmd));
    if (local) link.send(cmd);
    link.send_hash(++applied, battle.state_hash());
  }
};

void step_all(Player& a, Player& b, Relay& relay, double dt) {
  a.step(dt);
  b.step(dt);
  relay.pass();
}

void test_command_text() {
  battle::Command cmd;
  cmd.kind = battle::Command::Kind::Attack;
  cmd.actor = 4;
  cmd.target = 1;
  cmd.chain = {0, 2, 1};
  const std::string text = game::versus_command_text(cmd);
  CHECK(text == "0 4 1 -1 -1 0 -1 0 2 1");
  const std::optional<battle::Command> back = game::parse_versus_command(text);
  CHECK(back && back->kind == cmd.kind && back->actor == 4 && back->target == 1 && back->chain == cmd.chain && !back->timeout);
  battle::Command end;
  end.kind = battle::Command::Kind::EndTurn;
  end.timeout = true;
  CHECK(game::parse_versus_command(game::versus_command_text(end))->timeout);
  for (const char* bad : {"", "0", "9 0 -1 -1 -1 0 -1", "0 6 -1 -1 -1 0 -1", "0 0 9 -1 -1 0 -1", "0 0 -1 80 -1 0 -1", "0 0 -1 -1 -1 2 -1",
                          "0 0 -1 -1 -1 0 -1 3", "0 0 -1 -1 -1 0 -1 0 0 0 0 0", "0  0 -1 -1 -1 0 -1", "0 0 -1 -1 -1 0 -1 ", "a 0 -1 -1 -1 0 -1"})
    CHECK(!game::parse_versus_command(bad));
}

// The side goes out once, base64; steps go out paced with the last relay number; received messages are taken in relay order, a repeat is
// dropped and a skip ends the link.
void test_client_link() {
  Player player("pa00000000000000", "pb00000000000000", true);
  MultiplayerClient& client = player.client;
  CHECK(client.send_pvp_side(std::string("\x01\x02\xFF", 3)));
  CHECK(!client.send_pvp_side("again"));
  for (std::uint32_t seq = 1; seq <= 30; ++seq) client.send_pvp_step(seq, seq % 2 ? "5 0 -1 -1 -1 0 -1" : "", seq);
  client.send_pvp_step(31, "not a command", 31);
  player.step(0.0);
  std::vector<Json> sent = player.wire.take();
  CHECK(sent.size() == static_cast<size_t>(MultiplayerClient::kPvpPerSecond));
  CHECK(sent[0].string_of("t") == "pvp_side" && sent[0].string_of("side") == "AQL/" && sent[0].string_of("battle") == kBattle);
  CHECK(sent[1].string_of("t") == "pvp_cmd" && sent[1].int_of("seq") == 1 && sent[1].string_of("hash") == "0000000000000001" && sent[1].int_of("ack") == 0);
  CHECK(sent[2].string_of("t") == "pvp_hash" && sent[2]["cmd"].is_null());
  player.step(1.0);
  sent = player.wire.take();
  CHECK(sent.size() == 11 && sent.back().int_of("seq") == 30);
  player.wire.receive(R"({"t":"pvp_side","battle":"00ff00ff00ff00ff","side":"AQL/","n":1})");
  player.wire.receive(R"({"t":"pvp_cmd","battle":"00ff00ff00ff00ff","seq":1,"cmd":"6 3","hash":"00000000000000aa","n":2})");
  player.wire.receive(R"({"t":"pvp_cmd","battle":"00ff00ff00ff00ff","seq":1,"cmd":"6 3","hash":"00000000000000aa","n":2})");
  player.wire.receive(R"({"t":"pvp_cmd","battle":"ffffffffffffffff","seq":2,"cmd":"6 3","hash":"00000000000000aa","n":3})");
  player.step(0.1);
  CHECK(client.pvp_opponent_side() && *client.pvp_opponent_side() == std::string("\x01\x02\xFF", 3));
  net::PvpStep step;
  CHECK(client.take_pvp_step(step) && step.seq == 1 && step.cmd == "6 3" && step.hash == 0xAA);
  CHECK(!client.take_pvp_step(step));
  client.send_pvp_step(31, "", 1);
  player.step(0.0);
  CHECK(player.wire.take().back().int_of("ack") == 2);
  player.wire.receive(R"({"t":"pvp_away","battle":"00ff00ff00ff00ff","ms":90000})");
  player.step(0.0);
  CHECK(client.pvp_opponent_away() == MultiplayerClient::kAwayTime);
  player.idle(10.0);
  CHECK(client.pvp_opponent_away() == MultiplayerClient::kAwayTime - 10.0);
  player.wire.receive(R"({"t":"pvp_back","battle":"00ff00ff00ff00ff"})");
  player.step(0.0);
  CHECK(client.pvp_opponent_away() < 0);
  player.wire.receive(R"({"t":"pvp_hash","battle":"00ff00ff00ff00ff","seq":2,"hash":"00000000000000aa","n":4})");
  player.step(0.0);
  CHECK(client.pvp_gone() == net::PvpGone::Gap && !client.take_pvp_step(step));
}

// A drop keeps the battle: the client reconnects after kBattleRetry, asks to resume with its last relay number, sends nothing of the battle
// until pvp_resumed, then everything after the server's seq again; a resend it no longer holds ends the link.
void test_client_resume() {
  Player player("pa00000000000000", "pb00000000000000", true);
  MultiplayerClient& client = player.client;
  player.wire.receive(R"({"t":"pvp_cmd","battle":"00ff00ff00ff00ff","seq":1,"cmd":"6 3","hash":"00000000000000aa","n":1})");
  player.step(0.0);
  for (std::uint32_t seq = 1; seq <= 4; ++seq) client.send_pvp_step(seq, seq == 1 ? "" : "5 0 -1 -1 -1 0 -1", seq);
  player.step(0.0);
  player.wire.take();
  player.drop();
  CHECK(client.pvp_battle() == kBattle && client.pvp_self_away() == 0.0);
  client.send_pvp_step(5, "5 0 -1 -1 -1 0 -1", 5);
  player.step(MultiplayerClient::kBattleRetry - 0.5);
  CHECK(player.wire.connects == 1);
  player.step(0.5);
  CHECK(player.wire.connects == 2);
  player.open();
  std::vector<Json> sent = player.wire.take();
  bool resume = false;
  for (const Json& message : sent) {
    resume = resume || (message.string_of("t") == "pvp_resume" && message.string_of("battle") == kBattle && message.int_of("ack") == 1);
    CHECK(message.string_of("t") != "pvp_cmd" && message.string_of("t") != "pvp_hash");
  }
  CHECK(resume);
  CHECK(client.pvp_self_away() == MultiplayerClient::kBattleRetry);
  player.wire.receive(R"({"t":"pvp_resumed","battle":"00ff00ff00ff00ff","seq":2,"side":true})");
  player.step(0.0);
  sent = player.wire.take();
  CHECK(sent.size() == 3 && sent[0].int_of("seq") == 3 && sent[1].int_of("seq") == 4 && sent[2].int_of("seq") == 5);
  CHECK(client.pvp_self_away() < 0 && client.pvp_gone() == net::PvpGone::None);

  Player late("pa00000000000000", "pb00000000000000", true);
  for (std::uint32_t seq = 1; seq <= MultiplayerClient::kPvpResendKeep + 5; ++seq) {
    late.client.send_pvp_step(seq, "", seq);
    late.idle(1.0);
  }
  late.drop();
  late.step(MultiplayerClient::kBattleRetry);
  late.open();
  late.wire.receive(R"({"t":"pvp_resumed","battle":"00ff00ff00ff00ff","seq":1,"side":true})");
  late.step(0.0);
  CHECK(late.client.pvp_gone() == net::PvpGone::Gap);
}

// Two games with their own random players play a whole battle through the relay and end in step; every step was checked.
void test_lockstep() {
  Player pa("pa00000000000000", "pb00000000000000", true);
  Player pb("pb00000000000000", "pa00000000000000", false);
  Relay relay{pa, pb};
  Game ga(pa, 0), gb(pb, 1);
  for (int frame = 0; frame < 20000 && (ga.battle.outcome() == battle::Outcome::Ongoing || gb.battle.outcome() == battle::Outcome::Ongoing); ++frame) {
    ga.tick();
    gb.tick();
    step_all(pa, pb, relay, 0.05);
  }
  CHECK(ga.battle.outcome() != battle::Outcome::Ongoing && ga.battle.outcome() == gb.battle.outcome());
  CHECK(ga.applied == gb.applied && ga.applied > 0 && ga.battle.state_hash() == gb.battle.state_hash());
  CHECK(ga.link.status() == game::VersusLink::Status::Open && gb.link.status() == game::VersusLink::Status::Open);
}

// The other game's state check that differs from this game's hash ends the link as a desync, and so does a command out of order.
void test_desync_and_order() {
  Player pa("pa00000000000000", "pb00000000000000", true);
  Player pb("pb00000000000000", "pa00000000000000", false);
  Relay relay{pa, pb};
  Game ga(pa, 0), gb(pb, 1);
  ga.tick();
  step_all(pa, pb, relay, 0.05);
  CHECK(ga.applied == 1);
  pb.step(0.05);
  gb.tick();
  CHECK(gb.applied == 1);
  pb.wire.take();
  pa.wire.receive(std::format(R"({{"t":"pvp_hash","battle":"{}","seq":1,"hash":"{:016x}","n":1}})", kBattle, ga.battle.state_hash() ^ 1));
  pa.step(0.05);
  ga.tick();
  CHECK(ga.link.status() == game::VersusLink::Status::Desync);

  Player pc("pa00000000000000", "pb00000000000000", false);
  Game gc(pc, 1);
  pc.wire.receive(std::format(R"({{"t":"pvp_cmd","battle":"{}","seq":2,"cmd":"6 0 -1 -1 -1 0 -1","hash":"0000000000000001","n":1}})", kBattle));
  pc.step(0.05);
  gc.tick();
  CHECK(gc.applied == 0 && gc.link.status() == game::VersusLink::Status::Desync);
}

// A game whose connection drops in the middle of a battle resumes it: what it sent meanwhile and what the relay kept for it arrive, and
// the battle ends in step.
void test_resume_in_battle() {
  Player pa("pa00000000000000", "pb00000000000000", true);
  Player pb("pb00000000000000", "pa00000000000000", false);
  Relay relay{pa, pb};
  Game ga(pa, 0), gb(pb, 1);
  for (int frame = 0; frame < 40; ++frame) {
    ga.tick();
    gb.tick();
    step_all(pa, pb, relay, 0.05);
  }
  CHECK(ga.applied > 4);
  relay.hold_a = true;
  pa.drop();
  CHECK(ga.link.away() && ga.link.away()->self && ga.link.away()->seconds == 30);
  pb.wire.receive(std::format(R"({{"t":"pvp_away","battle":"{}","ms":30000}})", kBattle));
  for (int frame = 0; frame < 40; ++frame) {
    ga.tick();
    gb.tick();
    step_all(pa, pb, relay, 0.05);
  }
  CHECK(gb.link.away() && !gb.link.away()->self);
  pa.open();
  relay.hold_a = false;
  std::int64_t ack = -1;
  for (const Json& message : pa.wire.take())
    if (message.string_of("t") == "pvp_resume") ack = message.int_of("ack");
  CHECK(ack >= 0);
  pa.wire.receive(std::format(R"({{"t":"pvp_resumed","battle":"{}","seq":{},"side":false}})", kBattle, relay.seq_a));
  for (const Json& message : relay.log_a)
    if (message.int_of("n") > ack) pa.wire.receive(net::to_json(message));
  pb.wire.receive(std::format(R"({{"t":"pvp_back","battle":"{}"}})", kBattle));
  for (int frame = 0; frame < 20000 && (ga.battle.outcome() == battle::Outcome::Ongoing || gb.battle.outcome() == battle::Outcome::Ongoing); ++frame) {
    ga.tick();
    gb.tick();
    step_all(pa, pb, relay, 0.05);
  }
  CHECK(!ga.link.away() && !gb.link.away());
  CHECK(ga.battle.outcome() != battle::Outcome::Ongoing && ga.battle.state_hash() == gb.battle.state_hash() && ga.applied == gb.applied);
  CHECK(ga.link.status() == game::VersusLink::Status::Open && gb.link.status() == game::VersusLink::Status::Open);
}

// The server's forfeits, this game's own connection gone past the away time, and the other game's silence while it acts.
void test_forfeits() {
  {
    Player player("pa00000000000000", "pb00000000000000", true);
    Game game(player, 0);
    player.wire.receive(std::format(R"({{"t":"pvp_gone","battle":"{}","reason":"left"}})", kBattle));
    player.step(0.0);
    CHECK(game.link.status() == game::VersusLink::Status::Conceded);
  }
  {
    Player player("pa00000000000000", "pb00000000000000", true);
    Game game(player, 0);
    player.wire.receive(std::format(R"({{"t":"pvp_gone","battle":"{}","reason":"forfeit"}})", kBattle));
    player.step(0.0);
    CHECK(game.link.status() == game::VersusLink::Status::Forfeited);
  }
  {
    Player player("pa00000000000000", "pb00000000000000", true);
    Game game(player, 0);
    player.wire.receive(std::format(R"({{"t":"pvp_gone","battle":"{}","reason":"aborted"}})", kBattle));
    player.step(0.0);
    CHECK(game.link.status() == game::VersusLink::Status::Closed);
  }
  {
    Player player("pa00000000000000", "pb00000000000000", true);
    Game game(player, 0);
    player.drop();
    player.step(MultiplayerClient::kAwayTime);
    CHECK(game.link.status() == game::VersusLink::Status::Open && game.link.away()->seconds == 0);
    player.step(game::NetworkVersusLink::kSelfGrace);
    CHECK(game.link.status() == game::VersusLink::Status::Forfeited);
  }
  {
    Player player("pa00000000000000", "pb00000000000000", false);
    Game game(player, 1);
    battle::Command cmd;
    player.step(1.0);
    CHECK(!game.link.poll(game.battle, cmd));
    player.wire.receive(std::format(R"({{"t":"pvp_away","battle":"{}","ms":30000}})", kBattle));
    player.idle(60.0);
    CHECK(!game.link.poll(game.battle, cmd) && game.link.status() == game::VersusLink::Status::Open);
    player.wire.receive(std::format(R"({{"t":"pvp_back","battle":"{}"}})", kBattle));
    player.step(0.0);
    CHECK(!game.link.poll(game.battle, cmd));
    player.idle(game::NetworkVersusLink::kSilenceLimit - 1.0);
    CHECK(!game.link.poll(game.battle, cmd) && game.link.status() == game::VersusLink::Status::Open);
    player.idle(1.0);
    CHECK(!game.link.poll(game.battle, cmd) && game.link.status() == game::VersusLink::Status::Conceded);
  }
  {
    Player player("pa00000000000000", "pb00000000000000", true);
    Game game(player, 0);
    player.client.stop();
    CHECK(game.link.status() == game::VersusLink::Status::Closed);
  }
}

}  // namespace

int main() {
  test_command_text();
  test_client_link();
  test_client_resume();
  test_lockstep();
  test_desync_and_order();
  test_resume_in_battle();
  test_forfeits();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("versus link: all passed\n");
  return failures ? 1 : 0;
}
