#include "net/multiplayer_client.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <random>

#include "core/log.h"
#include "net/json.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace oc::net {

namespace {

const char* scope_name(ChatScope scope) { return scope == ChatScope::Global ? "global" : "room"; }

bool parse_scope(const std::string& text, ChatScope& scope) {
  if (text == "room") scope = ChatScope::Room;
  else if (text == "global") scope = ChatScope::Global;
  else return false;
  return true;
}

// A `models` array: up to 3 model bytes, leader first; missing slots are -1.
PartyModels read_models(const Json& list) {
  PartyModels models{-1, -1, -1};
  for (size_t i = 0; i < list.items().size() && i < models.size(); ++i) models[i] = static_cast<int>(list.items()[i].as_int(-1));
  return models;
}

std::string_view trim_space(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

}  // namespace

MultiplayerClient::MultiplayerClient(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

MultiplayerClient::~MultiplayerClient() { stop(); }

void MultiplayerClient::start(const std::string& url, const ClientIdentity& identity) {
  if (state_ != State::Off && url == url_ && identity.player_id == identity_.player_id && identity.admin_token == identity_.admin_token) {
    set_profile(identity.name, identity.star);
    return;
  }
  stop();
  url_ = url;
  identity_ = identity;
  state_ = State::Connecting;
  connect_pending_ = true;
  attempts_ = 0;
  last_error_.clear();
  server_message_.clear();
}

void MultiplayerClient::stop() {
  if (state_ == State::Off) return;
  transport_->close();
  state_ = State::Off;
  connect_pending_ = false;
  pid_.clear();
  parties_.clear();
  party_seq_.clear();
  occupancy_ = 0;
  room_chat_.clear();
}

void MultiplayerClient::set_profile(const std::string& name, int star) {
  identity_.name = name;
  identity_.star = star;
}

void MultiplayerClient::set_local(const LocalPresence& local) { local_ = local; }

void MultiplayerClient::connect_now() {
  connect_pending_ = false;
  state_ = State::Connecting;
  room_sent_ = party_sent_ = pose_sent_ = false;
  occupancy_ = 0;
  parties_.clear();
  party_seq_.clear();
  last_sent_ = last_received_ = now_;
  log::info("multiplayer: connecting to {}", url_);
  transport_->connect(url_);
}

void MultiplayerClient::dropped(const std::string& reason) {
  last_error_ = reason.empty() ? "The server closed the connection" : reason;
  const double delay = kBackoff[std::min<size_t>(static_cast<size_t>(attempts_), std::size(kBackoff) - 1)];
  ++attempts_;
  retry_at_ = now_ + delay;
  state_ = State::Waiting;
  pid_.clear();
  parties_.clear();
  party_seq_.clear();
  occupancy_ = 0;
  log::info("multiplayer: disconnected ({}); retrying in {:.0f} s", last_error_, delay);
}

void MultiplayerClient::send(const Json& message) {
  transport_->send(to_json(message));
  last_sent_ = now_;
}

void MultiplayerClient::update(double now) {
  now_ = now;
  if (state_ == State::Off || state_ == State::Banned || state_ == State::VersionTooOld) return;
  if (connect_pending_ || (state_ == State::Waiting && now_ >= retry_at_)) connect_now();
  TransportEvent event;
  while (transport_->poll(event)) {
    switch (event.kind) {
      case TransportEvent::Kind::Opened: {
        state_ = State::Greeting;
        last_received_ = now_;
        Json hello = Json::object();
        hello.set("t", "hello").set("v", kProtocolVersion).set("id", identity_.player_id).set("name", sent_name());
        hello.set("star", identity_.star).set("ver", identity_.version);
        if (!identity_.admin_token.empty()) hello.set("admin", identity_.admin_token);
        sent_name_ = identity_.name;
        sent_star_ = identity_.star;
        send(hello);
        break;
      }
      case TransportEvent::Kind::Message: {
        if (state_ != State::Greeting && state_ != State::Connected) break;
        last_received_ = now_;
        const std::optional<Json> message = parse_json(event.text);
        if (message && message->is_object()) handle(*message);
        break;
      }
      case TransportEvent::Kind::Closed: dropped(event.text); break;
    }
    if (state_ == State::Off || state_ == State::Banned || state_ == State::VersionTooOld || state_ == State::Waiting) return;
  }
  if (state_ != State::Greeting && state_ != State::Connected) return;
  if (now_ - last_received_ >= kSilentDrop) {
    transport_->close();
    dropped("The server stopped answering");
    return;
  }
  if (state_ != State::Connected) return;
  send_changes();
  send_pres_if_due();
  if (now_ - last_sent_ >= kPingAfter) send(Json::object().set("t", "ping"));
}

void MultiplayerClient::send_changes() {
  if (!room_sent_ || local_.room != sent_room_) {
    send(Json::object().set("t", "room").set("room", local_.room));
    sent_room_ = local_.room;
    room_sent_ = true;
    pose_sent_ = false;
    occupancy_ = 0;
    parties_.clear();
    party_seq_.clear();
    room_chat_.clear();
  }
  if (!party_sent_ || local_.models != sent_models_) {
    Json models = Json::array();
    for (const int model : local_.models) {
      if (model < 0) break;
      models.push(model);
    }
    send(Json::object().set("t", "party").set("models", std::move(models)));
    sent_models_ = local_.models;
    party_sent_ = true;
  }
  if (identity_.name != sent_name_ || identity_.star != sent_star_) {
    send(Json::object().set("t", "profile").set("name", sent_name()).set("star", identity_.star));
    sent_name_ = identity_.name;
    sent_star_ = identity_.star;
  }
}

std::string MultiplayerClient::sent_name() const { return utf8_truncate(identity_.name, static_cast<size_t>(std::max(1, limits_.name_len))); }

void MultiplayerClient::send_pres_if_due() {
  if (occupancy_ <= 0 || local_.room.empty()) return;
  if (pose_sent_ && local_.pose == sent_pose_) return;
  const double interval = std::max(kPresInterval, limits_.pres_hz > 0 ? 1.0 / limits_.pres_hz : kPresInterval);
  if (pose_sent_ && now_ - last_pres_ < interval - 1e-9) return;
  const LeaderPose& pose = local_.pose;
  Json pres = Json::object();
  pres.set("t", "pres").set("x", pose.x).set("y", pose.y).set("z", pose.z).set("f", pose.facing).set("c", pose.clip);
  pres.set("m", pose.moving ? 1 : 0).set("s", static_cast<int>(pose.state)).set("seq", ++pres_seq_);
  send(pres);
  sent_pose_ = pose;
  pose_sent_ = true;
  last_pres_ = now_;
}

bool MultiplayerClient::send_chat(ChatScope scope, std::string_view text) {
  if (state_ != State::Connected) return false;
  const std::string line = utf8_truncate(trim_space(text), static_cast<size_t>(std::max(1, limits_.chat_len)));
  if (line.empty()) return false;
  last_scope_ = scope;
  send(Json::object().set("t", "chat").set("scope", scope_name(scope)).set("text", line));
  return true;
}

RemoteParty* MultiplayerClient::find_party(const std::string& pid) {
  for (RemoteParty& party : parties_)
    if (party.pid == pid) return &party;
  return nullptr;
}

void MultiplayerClient::apply_pres(RemoteParty& party, const Json& pres) {
  if (!pres.is_object()) return;
  const std::int64_t seq = pres.int_of("seq", -1);
  if (seq >= 0) {
    auto [it, fresh] = party_seq_.try_emplace(party.pid, seq);
    if (!fresh) {
      if (seq <= it->second) return;
      it->second = seq;
    }
  }
  LeaderPose& pose = party.pose;
  pose.x = static_cast<int>(pres.int_of("x"));
  pose.y = static_cast<int>(pres.int_of("y"));
  pose.z = static_cast<int>(pres.int_of("z"));
  pose.facing = static_cast<int>(pres.int_of("f"));
  pose.clip = static_cast<int>(pres.int_of("c"));
  pose.moving = pres.int_of("m") != 0;
  pose.state = static_cast<PresenceState>(std::clamp<std::int64_t>(pres.int_of("s"), 0, static_cast<int>(PresenceState::Typing)));
  party.has_pose = true;
  ++party.pose_seq;
}

void MultiplayerClient::handle_party_entry(const Json& entry) {
  const std::string& pid = entry.string_of("pid");
  if (!valid_pid(pid) || pid == pid_) return;
  RemoteParty* party = find_party(pid);
  if (!party) {
    if (parties_.size() >= kMaxParties) return;
    parties_.push_back({});
    party = &parties_.back();
    party->pid = pid;
  }
  party->name = entry.string_of("name");
  party->star = static_cast<int>(entry.int_of("star"));
  party->models = read_models(entry["models"]);
  party_seq_.erase(pid);
  apply_pres(*party, entry["pres"]);
}

void MultiplayerClient::add_chat(ChatLine line) {
  last_ts_ = std::max(last_ts_, line.ts);
  std::deque<ChatLine>& log = line.scope == ChatScope::Room ? room_chat_ : global_chat_;
  log.push_back(std::move(line));
  while (log.size() > kChatKeep) log.pop_front();
  ++chat_serial_;
}

void MultiplayerClient::add_notice(std::string text) {
  ChatLine line;
  line.scope = last_scope_;
  line.text = std::move(text);
  line.ts = last_ts_;
  line.received = now_;
  add_chat(std::move(line));
}

// One server message. Unknown types are dropped.
void MultiplayerClient::handle(const Json& message) {
  const std::string& type = message.string_of("t");
  if (type == "welcome") {
    pid_ = message.string_of("pid");
    admin_ = message["admin"].as_bool();
    const Json& limits = message["limits"];
    limits_.chat_len = static_cast<int>(limits.int_of("chat_len", limits_.chat_len));
    limits_.name_len = static_cast<int>(limits.int_of("name_len", limits_.name_len));
    limits_.pres_hz = static_cast<int>(limits.int_of("pres_hz", limits_.pres_hz));
    state_ = State::Connected;
    attempts_ = 0;
    last_error_.clear();
    log::info("multiplayer: connected as {}", pid_);
    send_changes();
  } else if (type == "peers") {
    if (!room_sent_ || message.string_of("room") != sent_room_) return;
    parties_.clear();
    party_seq_.clear();
    for (const Json& entry : message["list"].items()) handle_party_entry(entry);
  } else if (type == "join") {
    handle_party_entry(message);
  } else if (type == "leave") {
    const std::string& pid = message.string_of("pid");
    std::erase_if(parties_, [&](const RemoteParty& party) { return party.pid == pid; });
    party_seq_.erase(pid);
  } else if (type == "profile") {
    if (RemoteParty* party = find_party(message.string_of("pid"))) {
      party->name = message.string_of("name");
      party->star = static_cast<int>(message.int_of("star"));
    }
  } else if (type == "party") {
    if (RemoteParty* party = find_party(message.string_of("pid"))) party->models = read_models(message["models"]);
  } else if (type == "pres") {
    if (RemoteParty* party = find_party(message.string_of("pid"))) apply_pres(*party, message);
  } else if (type == "occupancy") {
    occupancy_ = static_cast<int>(std::max<std::int64_t>(0, message.int_of("others")));
  } else if (type == "chat") {
    ChatLine line;
    if (!parse_scope(message.string_of("scope"), line.scope)) return;
    line.pid = message.string_of("pid");
    if (!valid_pid(line.pid)) return;
    line.name = message.string_of("name");
    line.text = message.string_of("text");
    line.admin = message["admin"].as_bool();
    line.ts = message.int_of("ts");
    line.received = now_;
    add_chat(std::move(line));
  } else if (type == "history") {
    ChatScope scope;
    if (!parse_scope(message.string_of("scope"), scope)) return;
    (scope == ChatScope::Room ? room_chat_ : global_chat_).clear();
    for (const Json& entry : message["lines"].items()) {
      ChatLine line;
      line.scope = scope;
      line.pid = entry.string_of("pid");
      if (!valid_pid(line.pid)) continue;
      line.name = entry.string_of("name");
      line.text = entry.string_of("text");
      line.admin = entry["admin"].as_bool();
      line.ts = entry.int_of("ts");
      line.received = now_;
      line.fresh = false;
      add_chat(std::move(line));
    }
  } else if (type == "error") {
    const std::string& code = message.string_of("code");
    if (code == "rate") {
      add_notice("(Not sent: too many messages, wait a moment)");
    } else if (code == "version" || code == "banned") {
      const bool banned = code == "banned";
      state_ = banned ? State::Banned : State::VersionTooOld;
      server_message_ = message.string_of("msg");
      log::warn("multiplayer: {} ({})", banned ? "banned by the server" : "the server needs a newer version", server_message_);
      transport_->close();
      parties_.clear();
      occupancy_ = 0;
    } else {
      log::warn("multiplayer: server error {} {}", code, message.string_of("msg"));
    }
  }
}

std::string MultiplayerClient::status_text() const {
  switch (state_) {
    case State::Off: return "Disconnected";
    case State::Connecting:
    case State::Greeting: return "Connecting...";
    case State::Connected: return "Connected as " + pid_ + (admin_ ? " (admin)" : "");
    case State::Waiting: return std::format("{}; retrying in {:.0f} s", last_error_, std::max(0.0, retry_at_ - now_));
    case State::Banned: return server_message_.empty() ? "Banned from this server" : "Banned from this server: " + server_message_;
    case State::VersionTooOld: return server_message_.empty() ? "This OpenCross version is too old for the server" : server_message_;
  }
  return {};
}

std::string new_player_id() {
  unsigned char bytes[16] = {};
  bool filled = false;
#ifdef _WIN32
  filled = BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#endif
  if (!filled) {
    std::random_device device;
    for (unsigned char& byte : bytes) byte = static_cast<unsigned char>(device());
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id;
  for (const unsigned char byte : bytes) {
    id += kHex[byte >> 4];
    id += kHex[byte & 0xF];
  }
  return id;
}

bool valid_pid(std::string_view pid) {
  return !pid.empty() && pid.size() <= 32 &&
         std::all_of(pid.begin(), pid.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

bool valid_player_id(std::string_view id) {
  return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char letter) { return (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f'); });
}

}  // namespace oc::net
