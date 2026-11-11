// net/multiplayer_client.h over a scripted transport and a hand-driven clock: the hello and what follows the welcome, the pres send
// rules (occupancy, 10 a second, only on change, the stop), the keep-alive ping, the reconnect back-off, the remote party table, chat
// lines, mutes, the banned / version answers that end the reconnecting, private messages, the pvp flag and the battle challenges up
// to the pvp_start.
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
  CHECK(client.status_text() == "Connected");
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
  CHECK(client.admin() && client.status_text() == "Connected");
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
  session.wire.receive(R"({"t":"chat","scope":"global","pid":"p1","name":"Kid","text":"one","ts":1000})");
  session.wire.receive(R"({"t":"chat","scope":"global","pid":"p2","name":"Leena","text":"two","ts":2000,"admin":true})");
  session.wire.receive(R"({"t":"chat","scope":"room","pid":"p1","name":"Kid","text":"hi é","ts":3000})");
  session.wire.receive(R"({"t":"chat","scope":"nowhere","pid":"p1","name":"Kid","text":"lost","ts":3500})");
  session.step(1.0);
  const auto& global = session.client.chat(net::ChatScope::Global);
  const auto& room = session.client.chat(net::ChatScope::Room);
  CHECK(global.size() == 2 && global[1].text == "two" && global[1].ts == 2000);
  CHECK(!global[0].admin && global[1].admin && !room[0].admin);
  CHECK(room.size() == 1 && room[0].text == "hi \xC3\xA9" && room[0].received == 1.0);
  const std::uint64_t serial = session.client.chat_serial();
  CHECK(serial == 3);
  // The server keeps no history: a history message is an unknown type and adds nothing.
  session.wire.receive(R"({"t":"history","scope":"global","lines":[{"pid":"p3","name":"Harle","text":"three","ts":4000}]})");
  session.step(0.1);
  CHECK(global.size() == 2);
  // Sending: trimmed, cut to chat_len (10 here), empty refused.
  CHECK(session.client.send_chat(net::ChatScope::Global, "  hello there everyone  "));
  std::vector<Json> sent = session.sent_of("chat");
  CHECK(sent.size() == 1 && sent[0].string_of("scope") == "global" && sent[0].string_of("text") == "hello ther");
  CHECK(!session.client.send_chat(net::ChatScope::Room, "   "));
  CHECK(session.sent_of("chat").empty());
  // A rate-limited line leaves a notice in the scope it was sent to.
  session.wire.receive(R"({"t":"error","code":"rate","msg":"slow down"})");
  session.step(0.1);
  CHECK(global.size() == 3 && global[2].pid.empty() && global[2].ts == 3000);
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
  {
    // A ws:// address elsewhere is never connected to.
    Session session;
    session.client.start("ws://example.invalid/ws", {kId, "Serge", 3, "0.5.4"});
    session.step(0.0);
    session.step(60.0);
    CHECK(session.client.state() == MultiplayerClient::State::BadAddress && session.wire.connects.size() == 1);
  }
  CHECK(net::server_url_allowed("ws://127.0.0.1:8787/ws") && net::server_url_allowed("ws://localhost/ws") && net::server_url_allowed("ws://[::1]:8787/ws"));
  CHECK(net::server_url_allowed("wss://example.invalid/ws") && !net::server_url_allowed("wss:///ws"));
  CHECK(!net::server_url_allowed("ws://example.invalid/ws") && !net::server_url_allowed("ws://127.0.0.1.example.invalid/ws"));
  CHECK(!net::server_url_allowed("ws://user@example.invalid/ws") && !net::server_url_allowed("http://127.0.0.1/ws"));
  {
    const std::string other = net::server_player_id(kId, "wss://Example.invalid/ws");
    CHECK(net::valid_player_id(other) && other != kId);
    CHECK(other == net::server_player_id(kId, "wss://example.invalid/other"));
    CHECK(other != net::server_player_id(kId, "wss://example.invalid:444/ws"));
    CHECK(net::server_player_id("short", "wss://example.invalid/ws").empty());
  }
}

// Private lines: sent with `pm`, kept in their own scope with the recipient, and a reply goes to the newest line
// from someone else.
void test_pm() {
  Session session;
  CHECK(session.client.send_pm("p1", "  psst  "));
  std::vector<Json> sent = session.sent_of("pm");
  CHECK(sent.size() == 1 && sent[0].string_of("to") == "p1" && sent[0].string_of("text") == "psst");
  CHECK(!session.client.send_pm("me00000000000000", "hi"));
  CHECK(!session.client.send_pm("a,b", "hi"));
  CHECK(!session.client.send_pm("p1", "   "));
  CHECK(!session.client.send_chat(net::ChatScope::Pm, "hi"));
  CHECK(session.wire.take().empty());
  session.wire.receive(R"({"t":"chat","scope":"pm","pid":"p1","name":"Kid","text":"hello","ts":10,"to":"me00000000000000","to_name":"Serge"})");
  session.wire.receive(R"({"t":"chat","scope":"pm","pid":"me00000000000000","name":"Serge","text":"psst","ts":11,"to":"p1","to_name":"Kid"})");
  session.wire.receive(R"({"t":"chat","scope":"pm","pid":"p1","name":"Kid","text":"not for me","ts":12,"to":"p2","to_name":"Leena"})");
  session.wire.receive(R"({"t":"chat","scope":"pm","pid":"p1","name":"Kid","text":"bad","ts":13,"to":"a,b","to_name":"x"})");
  session.step(0.1);
  const auto& pm = session.client.chat(net::ChatScope::Pm);
  CHECK(pm.size() == 2);
  if (pm.size() != 2) return;
  CHECK(pm[0].scope == net::ChatScope::Pm && pm[0].pid == "p1" && pm[0].to == "me00000000000000" && pm[0].to_name == "Serge");
  CHECK(pm[1].pid == "me00000000000000" && pm[1].to == "p1" && pm[1].to_name == "Kid");
  CHECK(session.client.chat(net::ChatScope::Room).empty() && session.client.chat(net::ChatScope::Global).empty());
  const net::ChatLine* newest = session.client.newest_pm();
  CHECK(newest && newest->pid == "p1" && newest->text == "hello");
  // A player who is not online: a notice in the private scope.
  session.wire.receive(R"({"t":"error","code":"offline","msg":"player not online","pid":"0123456789abcdef"})");
  session.step(0.1);
  CHECK(pm.size() == 3 && pm[2].pid.empty());
  // A rate-limited private line leaves its notice there too.
  session.client.send_pm("p1", "again");
  session.wire.receive(R"({"t":"error","code":"rate","msg":"slow down"})");
  session.step(0.1);
  CHECK(pm.size() == 4 && pm[3].pid.empty());
}

// The pvp flag goes in hello and, on a change, in profile; remote parties carry theirs.
void test_pvp_flag() {
  Wire wire;
  MultiplayerClient client(std::make_unique<FakeTransport>(wire));
  client.set_local(in_room("alu_a01f"));
  net::ClientIdentity identity{kId, "Serge", 3, "0.5.4"};
  identity.pvp = false;
  client.start(kUrl, identity);
  client.update(0.0);
  wire.opened();
  client.update(0.1);
  std::vector<Json> sent = wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("t") == "hello" && sent[0].get("pvp").type() == Json::Type::Bool && !sent[0]["pvp"].as_bool(true));
  CHECK(!client.challenges_on());
  wire.receive(R"({"t":"welcome","pid":"me"})");
  client.update(0.2);
  wire.take();
  client.set_challenges(true);
  client.update(0.3);
  sent = wire.take();
  CHECK(sent.size() == 1 && sent[0].string_of("t") == "profile" && sent[0]["pvp"].as_bool());
  client.update(0.4);
  CHECK(wire.sent.empty());
  wire.receive(R"({"t":"peers","room":"alu_a01f","list":[{"pid":"p1","name":"Kid","star":5,"pvp":true,"models":[3],"pres":null},
    {"pid":"p2","name":"Leena","star":2,"models":[7],"pres":null}]})");
  client.update(0.5);
  CHECK(client.parties().size() == 2 && client.parties()[0].pvp && !client.parties()[1].pvp);
  wire.receive(R"({"t":"profile","pid":"p1","name":"Kid","star":5,"pvp":false})");
  wire.receive(R"({"t":"profile","pid":"p2","name":"Leena","star":2,"pvp":true})");
  wire.receive(R"({"t":"profile","pid":"p2","name":"Leena","star":3})");
  client.update(0.6);
  CHECK(!client.parties()[0].pvp && client.parties()[1].pvp && client.parties()[1].star == 3);
}

net::PartyCard test_card(const std::string& name) {
  net::PartyCard card;
  card.star = 3;
  card.members.push_back({0, name, 320, 35, 18, 12, 10, 4});
  return card;
}

// The local player's challenge: what it sends, when it may, the server's end, the client's own give-up, and the pvp_start.
void test_challenge_out() {
  Session session;
  MultiplayerClient& client = session.client;
  CHECK(client.can_challenge());
  CHECK(!client.challenge("me00000000000000", "Me", test_card("Serge")));
  CHECK(!client.challenge("a,b", "Bad", test_card("Serge")));
  client.set_battle_data("abc123");
  CHECK(client.challenge("p1", "Kid", test_card("Serge")));
  std::vector<Json> sent = session.sent_of("challenge");
  CHECK(sent.size() == 1);
  if (sent.size() != 1) return;
  CHECK(sent[0].string_of("to") == "p1" && sent[0].string_of("ver") == "0.5.4" && sent[0].string_of("data") == "abc123");
  const Json& card = sent[0]["card"];
  CHECK(card.int_of("star") == 3 && card["members"].items().size() == 1);
  const Json& member = card["members"].items()[0];
  CHECK(member.int_of("char") == 0 && member.string_of("name") == "Serge" && member.int_of("hp") == 320 && member.int_of("atk") == 35);
  CHECK(member.int_of("def") == 18 && member.int_of("mag") == 12 && member.int_of("mdef") == 10 && member.int_of("elements") == 4);
  CHECK(client.outgoing() && client.outgoing()->to == "p1" && client.outgoing()->name == "Kid" && !client.can_challenge());
  CHECK(!client.challenge("p2", "Leena", test_card("Serge")));
  // An end for another player is not this challenge's.
  session.wire.receive(R"({"t":"challenge_end","pid":"p2","reason":"declined"})");
  session.wire.receive(R"({"t":"challenge_end","pid":"p1","reason":"nonsense"})");
  session.step(0.1);
  CHECK(client.outgoing().has_value());
  session.wire.receive(R"({"t":"challenge_end","pid":"p1","reason":"declined"})");
  session.step(0.1);
  CHECK(!client.outgoing() && client.can_challenge());
  CHECK(client.last_challenge_end() && client.last_challenge_end()->reason == net::ChallengeEnd::Declined && client.last_challenge_end()->outgoing);
  const auto& global = client.chat(net::ChatScope::Global);
  CHECK(!global.empty() && global.back().pid.empty() && global.back().text == "Kid declined your challenge");
  // Every server reason reads back.
  const std::pair<const char*, net::ChallengeEnd> reasons[] = {{"busy", net::ChallengeEnd::Busy},       {"timeout", net::ChallengeEnd::Timeout},
                                                               {"off", net::ChallengeEnd::Off},         {"left", net::ChallengeEnd::Left},
                                                               {"version", net::ChallengeEnd::Version}, {"data", net::ChallengeEnd::Data},
                                                               {"cooldown", net::ChallengeEnd::Cooldown}, {"pending", net::ChallengeEnd::Pending}};
  for (const auto& [text, reason] : reasons) {
    CHECK(client.challenge("p1", "Kid", test_card("Serge")));
    session.wire.receive(std::format(R"({{"t":"challenge_end","pid":"p1","reason":"{}"}})", text));
    session.step(0.1);
    CHECK(!client.outgoing() && client.last_challenge_end()->reason == reason);
  }
  CHECK(global.back().text == "You already have a challenge pending");
  // Cancel: sent, cleared, no notice.
  session.wire.take();
  const size_t lines = global.size();
  CHECK(client.challenge("p1", "Kid", test_card("Serge")));
  client.cancel_challenge();
  CHECK(session.sent_of("challenge_cancel").size() == 1 && !client.outgoing() && global.size() == lines);
  // No answer: given up after the server's limit and the grace.
  CHECK(client.challenge("p1", "Kid", test_card("Serge")));
  session.step(MultiplayerClient::kChallengeTime + MultiplayerClient::kChallengeGrace - 0.5);
  CHECK(client.outgoing().has_value());
  session.step(1.0);
  CHECK(!client.outgoing() && client.last_challenge_end()->reason == net::ChallengeEnd::Timeout && global.back().text == "Kid didn't respond");
  // With challenges off nothing goes out.
  client.set_challenges(false);
  CHECK(!client.can_challenge() && !client.challenge("p1", "Kid", test_card("Serge")));
  client.set_challenges(true);
  // An unasked pvp_start is ignored.
  session.wire.receive(R"({"t":"pvp_start","battle":"00ff00ff00ff00ff","seed":5,"first":"p1","opponent":"p1","name":"Kid","star":5,"card":{},"opening_stamina":35})");
  session.step(0.1);
  CHECK(!client.take_pvp_start() && client.pvp_battle().empty());
  // A pvp_start with a bad seed, first side or battle id is dropped; a good one starts the battle.
  CHECK(client.challenge("p1", "Kid", test_card("Serge")));
  session.wire.receive(R"({"t":"pvp_start","battle":"00ff00ff00ff00ff","seed":4294967296,"first":"p1","opponent":"p1","name":"Kid","star":5,"card":{},"opening_stamina":35})");
  session.wire.receive(R"({"t":"pvp_start","battle":"00ff00ff00ff00ff","seed":-1,"first":"p1","opponent":"p1","name":"Kid","star":5,"card":{},"opening_stamina":35})");
  session.wire.receive(R"({"t":"pvp_start","battle":"00ff00ff00ff00ff","seed":5,"first":"p2","opponent":"p1","name":"Kid","star":5,"card":{},"opening_stamina":35})");
  session.wire.receive(R"({"t":"pvp_start","battle":"a b","seed":5,"first":"p1","opponent":"p1","name":"Kid","star":5,"card":{},"opening_stamina":35})");
  session.step(0.1);
  CHECK(client.outgoing() && client.pvp_battle().empty());
  session.wire.receive(R"({"t":"pvp_start","battle":"00ff00ff00ff00ff","seed":4294967295,"first":"me00000000000000","opponent":"p1","name":"Kid","star":5,
    "card":{"star":5,"members":[{"char":1,"name":"Kid","hp":250,"atk":20,"def":15,"mag":11,"mdef":9,"elements":7}]},"opening_stamina":99})");
  session.step(0.1);
  CHECK(!client.outgoing() && client.pvp_battle() == "00ff00ff00ff00ff" && !client.can_challenge());
  const std::optional<net::PvpStart> start = client.take_pvp_start();
  CHECK(start.has_value() && !client.take_pvp_start());
  if (!start) return;
  CHECK(start->seed == 4294967295u && start->first == "me00000000000000" && start->opponent == "p1" && start->name == "Kid" && start->star == 5);
  CHECK(start->opening_stamina == 70 && start->card.members.size() == 1 && start->card.members[0].hp == 250 && start->card.members[0].char_id == 1);
  CHECK(start->own == test_card("Serge"));
  session.wire.take();
  client.end_pvp(net::PvpResult::Aborted);
  sent = session.sent_of("pvp_end");
  CHECK(sent.size() == 1 && sent[0].string_of("battle") == "00ff00ff00ff00ff" && sent[0].string_of("result") == "aborted");
  CHECK(client.pvp_battle().empty() && client.can_challenge());
  client.end_pvp(net::PvpResult::Won);
  CHECK(session.sent_of("pvp_end").empty());
}

// Typing in the chat still counts as free; a battle starting withdraws the own challenge and answers an incoming one busy.
void test_challenge_battle_start() {
  Session session;
  MultiplayerClient& client = session.client;
  net::LocalPresence typing = in_room("alu_a01f");
  typing.pose.state = net::PresenceState::Typing;
  client.set_local(typing);
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,"card":{}})");
  session.step(0.1);
  CHECK(client.incoming().has_value() && session.sent_of("challenge_reply").empty());
  net::LocalPresence battle = in_room("alu_a01f");
  battle.pose.state = net::PresenceState::Battle;
  client.set_local(battle);
  std::vector<Json> sent = session.sent_of("challenge_reply");
  CHECK(!client.incoming() && sent.size() == 1 && sent[0].string_of("reason") == "busy");
  client.set_local(in_room("alu_a01f"));
  CHECK(client.challenge("p2", "Leena", test_card("Serge")));
  session.wire.take();
  client.set_local(battle);
  CHECK(!client.outgoing() && session.sent_of("challenge_cancel").size() == 1);
}

// A challenge to the local player: kept for the popup when free, answered busy / off by the client itself otherwise, the replies, the
// server's ends, the client's own time limits and a drop.
void test_challenge_in() {
  Session session;
  MultiplayerClient& client = session.client;
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,
    "card":{"star":5,"members":[{"char":1,"name":"Kid","hp":99999,"atk":-3,"def":15,"mag":11,"mdef":9,"elements":7},{},{},{},{}]}})");
  session.step(1.0);
  CHECK(client.incoming().has_value());
  if (!client.incoming()) return;
  const net::IncomingChallenge& incoming = *client.incoming();
  CHECK(incoming.from == "p1" && incoming.name == "Kid" && incoming.star == 5 && incoming.deadline == 31.0);
  CHECK(incoming.card.members.size() == 3 && incoming.card.members[0].hp == 9999 && incoming.card.members[0].atk == 0);
  CHECK(session.wire.take().empty() && !client.can_challenge());
  // A second challenge meanwhile is busy.
  session.wire.receive(R"({"t":"challenge","from":"p2","name":"Leena","star":2,"expires":30000,"card":{}})");
  session.step(0.1);
  std::vector<Json> sent = session.sent_of("challenge_reply");
  CHECK(sent.size() == 1 && sent[0].string_of("from") == "p2" && !sent[0]["accept"].as_bool(true) && sent[0].string_of("reason") == "busy");
  CHECK(client.incoming()->from == "p1");
  client.decline_challenge();
  sent = session.sent_of("challenge_reply");
  CHECK(sent.size() == 1 && sent[0].string_of("from") == "p1" && sent[0].string_of("reason") == "declined" && !client.incoming());
  // Not free in the field: busy; challenges off: off; a bad pid: nothing.
  net::LocalPresence menu = in_room("alu_a01f");
  menu.pose.state = net::PresenceState::Menu;
  client.set_local(menu);
  session.wire.receive(R"({"t":"challenge","from":"p3","name":"Harle","star":9,"expires":30000,"card":{}})");
  session.step(0.1);
  CHECK(session.sent_of("challenge_reply").size() == 1 && !client.incoming());
  client.set_local(in_room(""));
  session.step(0.1);
  session.wire.take();
  session.wire.receive(R"({"t":"challenge","from":"p3","name":"Harle","star":9,"expires":30000,"card":{}})");
  session.step(0.1);
  sent = session.sent_of("challenge_reply");
  CHECK(sent.size() == 1 && sent[0].string_of("reason") == "busy");
  client.set_local(in_room("alu_a01f"));
  client.set_challenges(false);
  session.step(0.1);
  session.wire.take();
  session.wire.receive(R"({"t":"challenge","from":"p3","name":"Harle","star":9,"expires":30000,"card":{}})");
  session.wire.receive(R"({"t":"challenge","from":"x\ny","name":"Bad","star":9,"expires":30000,"card":{}})");
  session.step(0.1);
  sent = session.sent_of("challenge_reply");
  CHECK(sent.size() == 1 && sent[0].string_of("reason") == "off" && !client.incoming());
  client.set_challenges(true);
  session.step(0.1);
  session.wire.take();
  // Accept: the card, version and data go back; a version refusal ends it with a notice.
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,"card":{}})");
  session.step(0.1);
  client.set_battle_data("ff");
  CHECK(client.accept_challenge(test_card("Serge")));
  sent = session.sent_of("challenge_reply");
  CHECK(sent.size() == 1 && sent[0]["accept"].as_bool() && sent[0].string_of("ver") == "0.5.4" && sent[0].string_of("data") == "ff");
  CHECK(sent.size() == 1 && sent[0]["card"]["members"].items().size() == 1 && client.accepted() && !client.incoming());
  session.wire.receive(R"({"t":"challenge_end","pid":"p1","reason":"version"})");
  session.step(0.1);
  CHECK(!client.accepted() && client.last_challenge_end()->reason == net::ChallengeEnd::Version && !client.last_challenge_end()->outgoing);
  CHECK(client.chat(net::ChatScope::Global).back().text == "Your OpenCross build differs from Kid's (battles need the same opencross.exe)");
  // Accept, then the pvp_start.
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,"card":{}})");
  session.step(0.1);
  CHECK(client.accept_challenge(test_card("Serge")));
  session.wire.receive(R"({"t":"pvp_start","battle":"1234567890abcdef","seed":7,"first":"p1","opponent":"p1","name":"Kid","star":5,"card":{},"opening_stamina":35})");
  session.step(0.1);
  std::optional<net::PvpStart> start = client.take_pvp_start();
  CHECK(start && start->first == "p1" && start->opening_stamina == 35 && start->own == test_card("Serge") && !client.accepted());
  // In a battle every challenge is busy.
  session.wire.take();
  session.wire.receive(R"({"t":"challenge","from":"p2","name":"Leena","star":2,"expires":30000,"card":{}})");
  session.step(0.1);
  CHECK(session.sent_of("challenge_reply").size() == 1 && !client.incoming());
  client.end_pvp(net::PvpResult::Lost);
  session.wire.take();
  // The challenger withdrew.
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,"card":{}})");
  session.step(0.1);
  session.wire.receive(R"({"t":"challenge_end","pid":"p1","reason":"cancelled"})");
  session.step(0.1);
  CHECK(!client.incoming() && client.chat(net::ChatScope::Global).back().text == "Kid withdrew the challenge");
  // Unanswered: the client answers timeout at the deadline; the limit is at most 60 s.
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":999999999,"card":{}})");
  session.step(0.1);
  CHECK(client.incoming() && client.incoming()->deadline == session.now + MultiplayerClient::kMaxChallengeTime);
  session.wire.take();
  session.step(MultiplayerClient::kMaxChallengeTime);
  sent = session.sent_of("challenge_reply");
  CHECK(sent.size() == 1 && sent[0].string_of("reason") == "timeout" && !client.incoming());
  CHECK(client.chat(net::ChatScope::Global).back().text == "The challenge from Kid timed out");
  // An accept whose pvp_start never comes is given up.
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,"card":{}})");
  session.step(0.1);
  client.accept_challenge(test_card("Serge"));
  session.step(MultiplayerClient::kStartWait);
  CHECK(!client.accepted() && client.can_challenge());
  // A drop forgets every challenge.
  session.wire.receive(R"({"t":"challenge","from":"p1","name":"Kid","star":5,"expires":30000,"card":{}})");
  session.step(0.1);
  CHECK(client.incoming().has_value());
  session.wire.closed("Connection lost");
  session.step(0.1);
  CHECK(!client.incoming() && !client.outgoing() && !client.accepted() && client.pvp_battle().empty());
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
  test_pm();
  test_pvp_flag();
  test_challenge_out();
  test_challenge_battle_start();
  test_challenge_in();
  test_player_id();
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("multiplayer client: all passed\n");
  return failures ? 1 : 0;
}
