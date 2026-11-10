// net/multiplayer_client.h over a scripted transport and a hand-driven clock: the hello and what follows the welcome, the pres send
// rules (occupancy, 10 a second, only on change, the stop), the keep-alive ping, the reconnect back-off, the remote party table, chat
// history, mutes, and the banned / version answers that end the reconnecting.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <string>
#include <vector>

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

// What the client did to the connection, and what the test lets it receive.
struct Wire {
  std::vector<std::string> connects;
  std::vector<std::string> sent;
  std::deque<net::TransportEvent> incoming;
  int closes = 0;
  bool open = false;

  void opened() { incoming.push_back({net::TransportEvent::Kind::Opened, {}}); }
  void receive(const std::string& json) { incoming.push_back({net::TransportEvent::Kind::Message, json}); }
  void closed(const std::string& reason) { incoming.push_back({net::TransportEvent::Kind::Closed, reason}); }
  // The messages sent since the last take(), parsed.
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
  void connect(const std::string& url) override {
    wire_.connects.push_back(url);
    wire_.open = false;
  }
  void send(std::string text) override { wire_.sent.push_back(std::move(text)); }
  bool poll(net::TransportEvent& event) override {
    if (wire_.incoming.empty()) return false;
    event = wire_.incoming.front();
    wire_.incoming.pop_front();
    if (event.kind == net::TransportEvent::Kind::Opened) wire_.open = true;
    return true;
  }
  void close() override {
    ++wire_.closes;
    wire_.open = false;
    wire_.incoming.clear();
  }

 private:
  Wire& wire_;
};

const std::string kId = "0123456789abcdef0123456789abcdef";
const std::string kUrl = "ws://127.0.0.1:8787/ws";

net::LocalPresence in_room(const std::string& room, int x = 0, bool moving = false) {
  net::LocalPresence local;
  local.room = room;
  local.models = {1, 5, -1};
  local.pose.x = x;
  local.pose.moving = moving;
  return local;
}

// A client connected and welcomed at time 0 in room alu_a01f, with the messages of the greeting taken.
struct Session {
  Wire wire;
  MultiplayerClient client{std::make_unique<FakeTransport>(wire)};
  double now = 0.0;

  Session() {
    client.set_local(in_room("alu_a01f"));
    client.start(kUrl, {kId, "Serge", 3, "0.5.4"});
    step(0.0);
    wire.opened();
    step(0.0);
    wire.receive(R"({"t":"welcome","pid":"me00000000000000","limits":{"chat_len":10,"name_len":24,"pres_hz":10,"chat_per_10s":5}})");
    step(0.0);
    wire.take();
  }
  void step(double dt) {
    now += dt;
    client.update(now);
  }
  std::vector<Json> sent_of(const char* type) {
    std::vector<Json> out;
    for (Json& message : wire.take())
      if (message.string_of("t") == type) out.push_back(message);
    return out;
  }
};

void test_greeting() {
  Wire wire;
  MultiplayerClient client(std::make_unique<FakeTransport>(wire));
  client.set_local(in_room("alu_a01f"));
  CHECK(client.state() == MultiplayerClient::State::Off);
  client.start(kUrl, {kId, "Serge", 3, "0.5.4"});
  CHECK(wire.connects.empty());
  client.update(0.0);
  CHECK(wire.connects.size() == 1 && wire.connects[0] == kUrl);
  CHECK(client.state() == MultiplayerClient::State::Connecting);
  CHECK(wire.sent.empty());
  wire.opened();
  client.update(0.5);
  std::vector<Json> sent = wire.take();
  CHECK(sent.size() == 1);
  CHECK(sent[0].string_of("t") == "hello");
  CHECK(sent[0].int_of("v") == 1);
  CHECK(sent[0].string_of("id") == kId);
  CHECK(sent[0].string_of("name") == "Serge");
  CHECK(sent[0].int_of("star") == 3);
  CHECK(sent[0].string_of("ver") == "0.5.4");
  // Nothing else before the welcome.
  client.update(1.0);
  CHECK(wire.sent.empty());
  CHECK(client.state() == MultiplayerClient::State::Greeting);
  wire.receive(R"({"t":"welcome","pid":"abcd","limits":{"chat_len":200}})");
  client.update(1.5);
  CHECK(client.state() == MultiplayerClient::State::Connected);
  CHECK(client.pid() == "abcd");
  CHECK(client.status_text() == "Connected as abcd");
  CHECK(!client.admin() && sent[0].get("admin").is_null());
  sent = wire.take();
  CHECK(sent.size() == 2);
  CHECK(sent[0].string_of("t") == "room" && sent[0].string_of("room") == "alu_a01f");
  CHECK(sent[1].string_of("t") == "party");
  CHECK(sent[1]["models"].items().size() == 2);
  CHECK(sent[1]["models"].items()[1].as_int() == 5);
  // Changes go out once each.
  client.set_profile("Kid", 3);
  client.update(2.0);
  sent = wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("t") == "profile" && sent[0].string_of("name") == "Kid" && sent[0].int_of("star") == 3);
  client.update(2.1);
  CHECK(wire.sent.empty());
  // A name longer than the server's name_len is cut to it.
  client.set_profile("abcdefghijklmnopqrstuvwxyz", 3);
  client.update(2.15);
  sent = wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("name") == "abcdefghijklmnopqrstuvwx");
  net::LocalPresence local = in_room("alu_a01f");
  local.models = {1, 5, 9};
  client.set_local(local);
  client.update(2.2);
  sent = wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("t") == "party" && sent[0]["models"].items().size() == 3);
  // Unknown messages and members are ignored; broken JSON is dropped.
  wire.receive(R"({"t":"surprise","x":1})");
  wire.receive("{not json");
  client.update(2.3);
  CHECK(client.state() == MultiplayerClient::State::Connected);
}

// The admin token goes in hello only when set; the welcome says whether it was taken, and chat lines carry the server's admin mark.
void test_admin() {
  Wire wire;
  MultiplayerClient client(std::make_unique<FakeTransport>(wire));
  client.set_local(in_room("alu_a01f"));
  client.start(kUrl, {kId, "Serge", 3, "0.5.4", "s3cret"});
  client.update(0.0);
  wire.opened();
  client.update(0.5);
  const std::vector<Json> sent = wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("admin") == "s3cret");
  wire.receive(R"({"t":"welcome","pid":"abcd","admin":true})");
  wire.receive(R"({"t":"chat","scope":"global","pid":"abcd","name":"Serge","text":"hi","ts":1,"admin":true})");
  client.update(1.0);
  CHECK(client.admin() && client.status_text() == "Connected as abcd (admin)");
  CHECK(client.chat(net::ChatScope::Global).size() == 1 && client.chat(net::ChatScope::Global)[0].admin);
}

void test_pres_rules() {
  Session session;
  // Alone in the room: nothing is sent while moving.
  session.client.set_local(in_room("alu_a01f", 100, true));
  session.step(0.5);
  CHECK(session.sent_of("pres").empty());
  // Someone joins: the pose goes out at once.
  session.wire.receive(R"({"t":"occupancy","others":1})");
  session.step(0.5);
  std::vector<Json> pres = session.sent_of("pres");
  CHECK(pres.size() == 1);
  CHECK(pres.size() == 1 && pres[0].int_of("x") == 100 && pres[0].int_of("m") == 1 && pres[0].int_of("seq") == 1 && pres[0].int_of("s") == 0);
  // Unchanged: nothing.
  session.step(0.5);
  CHECK(session.sent_of("pres").empty());
  // At most 10 a second: a change 50 ms later waits, the newest pose goes at 100 ms.
  session.client.set_local(in_room("alu_a01f", 110, true));
  session.step(0.02);
  CHECK(session.sent_of("pres").size() == 1);
  session.client.set_local(in_room("alu_a01f", 120, true));
  session.step(0.05);
  CHECK(session.sent_of("pres").empty());
  session.client.set_local(in_room("alu_a01f", 130, true));
  session.step(0.05);
  pres = session.sent_of("pres");
  CHECK(pres.size() == 1 && pres[0].int_of("x") == 130 && pres[0].int_of("seq") == 3);
  // The stop is never lost to the rate limit.
  session.client.set_local(in_room("alu_a01f", 131, false));
  session.step(0.01);
  CHECK(session.sent_of("pres").empty());
  session.step(0.1);
  pres = session.sent_of("pres");
  CHECK(pres.size() == 1 && pres[0].int_of("m") == 0 && pres[0].int_of("x") == 131 && pres[0].int_of("seq") == 4);
  session.step(1.0);
  CHECK(session.sent_of("pres").empty());
  // A battle is a state change.
  net::LocalPresence battle = in_room("alu_a01f", 131, false);
  battle.pose.state = net::PresenceState::Battle;
  session.client.set_local(battle);
  session.step(0.2);
  pres = session.sent_of("pres");
  CHECK(pres.size() == 1 && pres[0].int_of("s") == 1);
  // Everyone left: the client goes quiet again.
  session.wire.receive(R"({"t":"occupancy","others":0})");
  session.client.set_local(in_room("alu_a01f", 500, true));
  session.step(0.2);
  CHECK(session.sent_of("pres").empty());
  // A new room: room sent, the old room's table and occupancy forgotten, no pres until someone is there.
  session.wire.receive(R"({"t":"occupancy","others":2})");
  session.step(0.2);
  CHECK(session.sent_of("pres").size() == 1);
  session.client.set_local(in_room("alu_a02f", 0, false));
  session.step(0.2);
  const std::vector<Json> sent = session.wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("t") == "room" && sent[0].string_of("room") == "alu_a02f");
  CHECK(session.client.occupancy() == 0);
  // Not in a field room: no pres even with company.
  session.client.set_local(in_room(""));
  session.step(0.2);
  session.wire.take();
  session.wire.receive(R"({"t":"occupancy","others":3})");
  session.step(0.2);
  CHECK(session.sent_of("pres").empty());
}

void test_ping_and_silence() {
  Session session;
  session.step(19.0);
  CHECK(session.sent_of("ping").empty());
  session.step(1.0);
  CHECK(session.sent_of("ping").size() == 1);
  session.step(10.0);
  CHECK(session.sent_of("ping").empty());
  session.wire.receive(R"({"t":"pong"})");
  session.step(10.0);
  CHECK(session.sent_of("ping").size() == 1);
  // Nothing received for the silence limit: the connection is dropped and made again after the back-off.
  const int closes = session.wire.closes;
  session.step(MultiplayerClient::kSilentDrop);
  CHECK(session.wire.closes == closes + 1);
  CHECK(session.client.state() == MultiplayerClient::State::Waiting);
}

void test_backoff() {
  Session session;
  session.wire.closed("Connection lost");
  session.step(0.0);
  CHECK(session.client.state() == MultiplayerClient::State::Waiting);
  CHECK(session.client.status_text() == "Connection lost; retrying in 2 s");
  CHECK(session.client.parties().empty());
  const double expected[] = {2, 4, 8, 16, 30, 30, 30};
  for (const double delay : expected) {
    const size_t before = session.wire.connects.size();
    session.step(delay - 0.25);
    CHECK(session.wire.connects.size() == before);
    session.step(0.25);
    CHECK(session.wire.connects.size() == before + 1);
    session.wire.closed("Could not reach the server");
    session.step(0.0);
  }
  // A welcome starts the back-off over.
  session.step(30.0);
  session.wire.opened();
  session.wire.receive(R"({"t":"welcome","pid":"me"})");
  session.step(0.0);
  CHECK(session.client.state() == MultiplayerClient::State::Connected);
  const std::vector<Json> sent = session.wire.take();
  CHECK(sent.size() == 3 && sent[0].string_of("t") == "hello" && sent[1].string_of("t") == "room" && sent[2].string_of("t") == "party");
  session.wire.closed("");
  session.step(0.0);
  CHECK(session.client.status_text() == "The server closed the connection; retrying in 2 s");
  const size_t before = session.wire.connects.size();
  session.step(2.0);
  CHECK(session.wire.connects.size() == before + 1);
  // stop() closes and stays off.
  session.client.stop();
  CHECK(session.client.state() == MultiplayerClient::State::Off);
  session.step(100.0);
  CHECK(session.wire.connects.size() == before + 1);
}

void test_remote_table() {
  Session session;
  session.wire.receive(R"({"t":"peers","room":"alu_a01f","list":[
    {"pid":"p1","name":"Kid","star":5,"models":[3,4],"pres":null},
    {"pid":"p2","name":"Leena","star":2,"models":[7],"pres":{"x":10,"y":-5,"z":20,"f":1024,"c":2,"m":1,"s":2,"seq":5}}]})");
  session.wire.receive(R"({"t":"peers","room":"elsewhere","list":[]})");
  session.step(0.1);
  const std::vector<net::RemoteParty>& parties = session.client.parties();
  CHECK(parties.size() == 2);
  if (parties.size() != 2) return;
  CHECK(parties[0].pid == "p1" && parties[0].name == "Kid" && parties[0].star == 5 && !parties[0].has_pose);
  CHECK(parties[0].models[0] == 3 && parties[0].models[1] == 4 && parties[0].models[2] == -1);
  CHECK(parties[1].has_pose && parties[1].pose_seq == 1);
  CHECK(parties[1].pose.x == 10 && parties[1].pose.y == -5 && parties[1].pose.z == 20 && parties[1].pose.facing == 1024 && parties[1].pose.clip == 2);
  CHECK(parties[1].pose.moving && parties[1].pose.state == net::PresenceState::Menu);
  // Out of order: dropped; newer: taken.
  session.wire.receive(R"({"t":"pres","pid":"p2","x":99,"y":0,"z":0,"f":0,"c":0,"m":0,"s":0,"seq":4})");
  session.wire.receive(R"({"t":"pres","pid":"p2","x":11,"y":0,"z":0,"f":0,"c":0,"m":0,"s":0,"seq":5})");
  session.step(0.1);
  CHECK(session.client.parties()[1].pose.x == 10 && session.client.parties()[1].pose_seq == 1);
  session.wire.receive(R"({"t":"pres","pid":"p2","x":12,"y":0,"z":0,"f":0,"c":0,"m":0,"s":3,"seq":6})");
  session.wire.receive(R"({"t":"pres","pid":"p1","x":1,"y":2,"z":3,"f":0,"c":0,"m":1,"s":0,"seq":1})");
  session.step(0.1);
  CHECK(session.client.parties()[1].pose.x == 12 && session.client.parties()[1].pose_seq == 2);
  CHECK(session.client.parties()[1].pose.state == net::PresenceState::Event && !session.client.parties()[1].pose.moving);
  CHECK(session.client.parties()[0].has_pose && session.client.parties()[0].pose.z == 3);
  // Typing is state 4; an unknown higher state reads as the highest known.
  session.wire.receive(R"({"t":"pres","pid":"p2","x":12,"y":0,"z":0,"f":0,"c":0,"m":0,"s":4,"seq":7})");
  session.step(0.1);
  CHECK(session.client.parties()[1].pose.state == net::PresenceState::Typing);
  session.wire.receive(R"({"t":"pres","pid":"p2","x":12,"y":0,"z":0,"f":0,"c":0,"m":0,"s":9,"seq":8})");
  session.step(0.1);
  CHECK(session.client.parties()[1].pose.state == net::PresenceState::Typing);
  // Join, profile, party, leave.
  session.wire.receive(R"({"t":"join","pid":"p3","name":"Harle","star":9,"models":[12,13,14],"pres":null})");
  session.wire.receive(R"({"t":"profile","pid":"p1","name":"Kid!","star":6})");
  session.wire.receive(R"({"t":"party","pid":"p2","models":[8,9]})");
  session.wire.receive(R"({"t":"leave","pid":"p1"})");
  session.wire.receive(R"({"t":"leave","pid":"nobody"})");
  session.step(0.1);
  CHECK(session.client.parties().size() == 2);
  CHECK(session.client.parties()[0].pid == "p2" && session.client.parties()[0].models[1] == 9);
  CHECK(session.client.parties()[1].pid == "p3" && session.client.parties()[1].star == 9 && session.client.parties()[1].models[2] == 14);
  // A player who rejoins starts a new seq count.
  session.wire.receive(R"({"t":"join","pid":"p2","name":"Leena","star":2,"models":[7],"pres":{"x":1,"y":0,"z":0,"f":0,"c":0,"m":0,"s":0,"seq":1}})");
  session.step(0.1);
  CHECK(session.client.parties()[0].pose.x == 1);
  // A room change empties the table.
  session.client.set_local(in_room("alu_a02f"));
  session.step(0.1);
  CHECK(session.client.parties().empty());
}

void test_chat() {
  Session session;
  session.wire.receive(R"({"t":"history","scope":"global","lines":[
    {"t":"chat","scope":"global","pid":"p1","name":"Kid","text":"one","ts":1000},
    {"t":"chat","scope":"global","pid":"p2","name":"Leena","text":"two","ts":2000,"admin":true}]})");
  session.wire.receive(R"({"t":"chat","scope":"room","pid":"p1","name":"Kid","text":"hi é","ts":3000})");
  session.wire.receive(R"({"t":"chat","scope":"nowhere","pid":"p1","name":"Kid","text":"lost","ts":3500})");
  session.step(1.0);
  const auto& global = session.client.chat(net::ChatScope::Global);
  const auto& room = session.client.chat(net::ChatScope::Room);
  CHECK(global.size() == 2 && !global[0].fresh && global[1].text == "two" && global[1].ts == 2000);
  CHECK(!global[0].admin && global[1].admin && !room[0].admin);
  CHECK(room.size() == 1 && room[0].fresh && room[0].text == "hi \xC3\xA9" && room[0].received == 1.0);
  const std::uint64_t serial = session.client.chat_serial();
  CHECK(serial == 3);
  // A second history replaces the first.
  session.wire.receive(R"({"t":"history","scope":"global","lines":[{"pid":"p3","name":"Harle","text":"three","ts":4000}]})");
  session.step(0.1);
  CHECK(global.size() == 1 && global[0].text == "three");
  // Sending: trimmed, cut to chat_len (10 here), empty refused.
  CHECK(session.client.send_chat(net::ChatScope::Global, "  hello there everyone  "));
  std::vector<Json> sent = session.sent_of("chat");
  CHECK(sent.size() == 1 && sent[0].string_of("scope") == "global" && sent[0].string_of("text") == "hello ther");
  CHECK(!session.client.send_chat(net::ChatScope::Room, "   "));
  CHECK(session.sent_of("chat").empty());
  // A rate-limited line leaves a notice in the scope it was sent to.
  session.wire.receive(R"({"t":"error","code":"rate","msg":"slow down"})");
  session.step(0.1);
  CHECK(global.size() == 2 && global[1].pid.empty() && global[1].ts == 4000);
  // A room change clears the room's chat; mutes are the client's own.
  session.client.set_local(in_room("alu_a02f"));
  session.step(0.1);
  CHECK(room.empty());
  session.client.set_muted({"p1"});
  CHECK(session.client.muted("p1") && !session.client.muted("p2"));
  CHECK(session.sent_of("chat").empty());
  // Not connected: refused.
  session.wire.closed("Connection lost");
  session.step(0.1);
  CHECK(!session.client.send_chat(net::ChatScope::Room, "hello"));
}

// A hostile server: pids that could break the ini are dropped with their message, and no more than kMaxParties parties are kept.
void test_hostile_server() {
  Session session;
  session.wire.receive(R"({"t":"join","pid":"ab\n[Debug]\nRoom=x","name":"Bad","star":1,"models":[1],"pres":null})");
  session.wire.receive(R"({"t":"chat","scope":"global","pid":"a,b","name":"Bad","text":"hi","ts":1})");
  session.step(0.1);
  CHECK(session.client.parties().empty());
  CHECK(session.client.chat(net::ChatScope::Global).empty());
  for (int i = 0; i < 80; ++i)
    session.wire.receive(std::format(R"({{"t":"join","pid":"p{}","name":"P","star":1,"models":[1],"pres":null}})", i));
  session.step(0.1);
  CHECK(session.client.parties().size() == net::kMaxParties);
}

void test_refusals() {
  {
    Session session;
    session.wire.receive(R"({"t":"error","code":"banned","msg":"bye"})");
    session.step(0.1);
    CHECK(session.client.state() == MultiplayerClient::State::Banned);
    CHECK(session.client.status_text() == "Banned from this server: bye");
    CHECK(session.wire.closes == 1);
    const size_t connects = session.wire.connects.size();
    session.step(600.0);
    CHECK(session.wire.connects.size() == connects);
    // The same server again does not retry; only a stop and start does.
    session.client.start(kUrl, {kId, "Serge", 3, "0.5.4"});
    session.step(1.0);
    CHECK(session.wire.connects.size() == connects);
    session.client.stop();
    session.client.start(kUrl, {kId, "Serge", 3, "0.5.4"});
    session.step(0.0);
    CHECK(session.wire.connects.size() == connects + 1);
  }
  {
    Session session;
    session.wire.receive(R"({"t":"error","code":"version","msg":"Please update OpenCross"})");
    session.step(0.1);
    CHECK(session.client.state() == MultiplayerClient::State::VersionTooOld);
    CHECK(session.client.status_text() == "Please update OpenCross");
    const size_t connects = session.wire.connects.size();
    session.step(600.0);
    CHECK(session.wire.connects.size() == connects);
  }
  {
    Session session;
    session.wire.receive(R"({"t":"error","code":"bad","msg":"what"})");
    session.step(0.1);
    CHECK(session.client.state() == MultiplayerClient::State::Connected);
  }
  {
    // A different server reconnects at once.
    Session session;
    session.client.start("wss://example.invalid/ws", {kId, "Serge", 3, "0.5.4"});
    session.step(0.0);
    CHECK(session.wire.connects.size() == 2 && session.wire.connects[1] == "wss://example.invalid/ws");
  }
}

void test_player_id() {
  const std::string first = net::new_player_id(), second = net::new_player_id();
  CHECK(net::valid_player_id(first) && net::valid_player_id(second) && first != second);
  CHECK(!net::valid_player_id(""));
  CHECK(!net::valid_player_id("0123456789ABCDEF0123456789abcdef"));
  CHECK(!net::valid_player_id("0123456789abcdef0123456789abcde"));
}

}  // namespace

int main() {
  test_greeting();
  test_admin();
  test_hostile_server();
  test_pres_rules();
  test_ping_and_silence();
  test_backoff();
  test_remote_table();
  test_chat();
  test_refusals();
  test_player_id();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("multiplayer client: all passed\n");
  return failures ? 1 : 0;
}
