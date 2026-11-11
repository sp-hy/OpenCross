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
  else if (text == "pm") scope = ChatScope::Pm;
  else return false;
  return true;
}

int clamped(const Json& object, std::string_view key, int low, int high) {
  return static_cast<int>(std::clamp<std::int64_t>(object.int_of(key), low, high));
}

Json card_json(const PartyCard& card) {
  Json members = Json::array();
  for (size_t i = 0; i < card.members.size() && i < 3; ++i) {
    const CardMember& member = card.members[i];
    Json entry = Json::object();
    entry.set("char", member.char_id).set("name", member.name).set("hp", member.hp).set("atk", member.atk).set("def", member.def);
    entry.set("mag", member.mag).set("mdef", member.mdef).set("elements", member.elements);
    members.push(std::move(entry));
  }
  Json out = Json::object();
  out.set("star", card.star).set("members", std::move(members));
  return out;
}

// A card from the server: at most 3 members, names cut to `name_len` code points, numbers held to the protocol's ranges.
PartyCard read_card(const Json& value, size_t name_len) {
  PartyCard card;
  card.star = clamped(value, "star", 0, 999);
  for (const Json& entry : value["members"].items()) {
    if (card.members.size() >= 3) break;
    if (!entry.is_object()) continue;
    CardMember member;
    member.char_id = clamped(entry, "char", 0, 255);
    member.name = utf8_truncate(entry.string_of("name"), name_len);
    member.hp = clamped(entry, "hp", 0, 9999);
    member.atk = clamped(entry, "atk", 0, 999);
    member.def = clamped(entry, "def", 0, 999);
    member.mag = clamped(entry, "mag", 0, 999);
    member.mdef = clamped(entry, "mdef", 0, 999);
    member.elements = clamped(entry, "elements", 0, 99);
    card.members.push_back(std::move(member));
  }
  return card;
}

constexpr const char* kEndNames[] = {"declined", "busy", "timeout", "off", "cancelled", "left", "version", "data", "cooldown", "pending"};

bool parse_end(const std::string& text, ChallengeEnd& reason) {
  for (size_t i = 0; i < std::size(kEndNames); ++i)
    if (text == kEndNames[i]) {
      reason = static_cast<ChallengeEnd>(i);
      return true;
    }
  return false;
}

// The chat panel's notice for a challenge with `name` that ended; empty when the player needs none (their own answers).
std::string end_notice(ChallengeEnd reason, const std::string& name, bool outgoing) {
  switch (reason) {
    case ChallengeEnd::Declined: return outgoing ? name + " declined your challenge" : std::string();
    case ChallengeEnd::Busy: return outgoing ? name + " is busy" : std::string();
    case ChallengeEnd::Timeout: return outgoing ? name + " didn't respond" : "The challenge from " + name + " timed out";
    case ChallengeEnd::Off: return outgoing ? name + " isn't taking challenges" : std::string();
    case ChallengeEnd::Cancelled: return outgoing ? std::string() : name + " withdrew the challenge";
    case ChallengeEnd::Left: return name + " left";
    case ChallengeEnd::Version: return "Your OpenCross build differs from " + name + "'s (battles need the same opencross.exe)";
    case ChallengeEnd::Data: return "Your game data differs from " + name + "'s";
    case ChallengeEnd::Cooldown: return "Wait a moment before challenging " + name + " again";
    case ChallengeEnd::Pending: return "You already have a challenge pending";
  }
  return {};
}

// A `models` array: up to 3 model bytes, leader first; missing slots are -1.
PartyModels read_models(const Json& list) {
  PartyModels models{-1, -1, -1};
  for (size_t i = 0; i < list.items().size() && i < models.size(); ++i) models[i] = static_cast<int>(list.items()[i].as_int(-1));
  return models;
}

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const size_t left = bytes.size() - i;
    std::uint32_t triple = static_cast<std::uint8_t>(bytes[i]) << 16;
    if (left > 1) triple |= static_cast<std::uint8_t>(bytes[i + 1]) << 8;
    if (left > 2) triple |= static_cast<std::uint8_t>(bytes[i + 2]);
    out += kBase64[triple >> 18 & 0x3F];
    out += kBase64[triple >> 12 & 0x3F];
    out += left > 1 ? kBase64[triple >> 6 & 0x3F] : '=';
    out += left > 2 ? kBase64[triple & 0x3F] : '=';
  }
  return out;
}

// Strict base64 (padded, no white space); nothing for any other text.
std::optional<std::string> base64_decode(std::string_view text) {
  if (text.empty() || text.size() % 4 != 0) return std::nullopt;
  std::string out;
  out.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    std::uint32_t quad = 0;
    int pad = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char letter = text[i + k];
      int value = 0;
      if (letter == '=' && i + 4 == text.size() && k >= 2) {
        ++pad;
      } else {
        const char* found = pad ? nullptr : std::char_traits<char>::find(kBase64, 64, letter);
        if (!found) return std::nullopt;
        value = static_cast<int>(found - kBase64);
      }
      quad = quad << 6 | static_cast<std::uint32_t>(value);
    }
    out += static_cast<char>(quad >> 16 & 0xFF);
    if (pad < 2) out += static_cast<char>(quad >> 8 & 0xFF);
    if (pad < 1) out += static_cast<char>(quad & 0xFF);
  }
  return out;
}

std::string hash_text(std::uint64_t hash) { return std::format("{:016x}", hash); }

// A state hash as sent: 16 lower-case hex digits.
bool parse_hash(const std::string& text, std::uint64_t& hash) {
  if (text.size() != 16) return false;
  hash = 0;
  for (const char letter : text) {
    const int digit = letter >= '0' && letter <= '9' ? letter - '0' : letter >= 'a' && letter <= 'f' ? letter - 'a' + 10 : -1;
    if (digit < 0) return false;
    hash = hash << 4 | static_cast<std::uint64_t>(digit);
  }
  return true;
}

// A command's text: signed decimal integers separated by single spaces.
bool valid_cmd_text(std::string_view text) {
  if (text.empty() || text.size() > MultiplayerClient::kMaxCmdText) return false;
  bool digit_seen = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char letter = text[i];
    if (letter >= '0' && letter <= '9') {
      digit_seen = true;
    } else if (letter == '-') {
      if (i > 0 && text[i - 1] != ' ') return false;
    } else if (letter == ' ') {
      if (!digit_seen) return false;
      digit_seen = false;
    } else {
      return false;
    }
  }
  return digit_seen;
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
  clear_challenges();
  pvp_battle_.clear();
  pvp_ = {};
}

void MultiplayerClient::set_profile(const std::string& name, int star) {
  identity_.name = name;
  identity_.star = star;
}

// A battle starting or the player leaving field play ends what is pending: the own challenge is withdrawn, an incoming one answered busy.
void MultiplayerClient::set_local(const LocalPresence& local) {
  local_ = local;
  if (local_.pose.state != PresenceState::Battle && !local_.room.empty()) return;
  if (outgoing_) cancel_challenge();
  if (incoming_) {
    if (state_ == State::Connected) reply(incoming_->from, "busy");
    incoming_.reset();
  }
}

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

// A drop keeps a running battle: the link waits, and resumes after the next welcome.
void MultiplayerClient::dropped(const std::string& reason) {
  last_error_ = reason.empty() ? "The server closed the connection" : reason;
  const bool battle = !pvp_battle_.empty();
  const double delay = battle ? kBattleRetry : kBackoff[std::min<size_t>(static_cast<size_t>(attempts_), std::size(kBackoff) - 1)];
  ++attempts_;
  retry_at_ = now_ + delay;
  state_ = State::Waiting;
  pid_.clear();
  parties_.clear();
  party_seq_.clear();
  occupancy_ = 0;
  clear_challenges();
  if (!pvp_battle_.empty()) {
    pvp_.resuming = true;
    if (pvp_.dropped_at < 0) pvp_.dropped_at = now_;
  }
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
        hello.set("star", identity_.star).set("ver", identity_.version).set("pvp", identity_.pvp);
        if (!identity_.admin_token.empty()) hello.set("admin", identity_.admin_token);
        sent_name_ = identity_.name;
        sent_star_ = identity_.star;
        sent_pvp_ = identity_.pvp;
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
  const bool battle = !pvp_battle_.empty();
  if (now_ - last_received_ >= (battle ? kBattleSilentDrop : kSilentDrop)) {
    transport_->close();
    dropped("The server stopped answering");
    return;
  }
  if (state_ != State::Connected) return;
  send_changes();
  send_pres_if_due();
  check_challenge_times();
  send_pvp_due();
  if (now_ - last_sent_ >= (battle ? kBattlePingAfter : kPingAfter)) send(Json::object().set("t", "ping"));
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
  if (identity_.name != sent_name_ || identity_.star != sent_star_ || identity_.pvp != sent_pvp_) {
    send(Json::object().set("t", "profile").set("name", sent_name()).set("star", identity_.star).set("pvp", identity_.pvp));
    sent_name_ = identity_.name;
    sent_star_ = identity_.star;
    sent_pvp_ = identity_.pvp;
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
  if (state_ != State::Connected || scope == ChatScope::Pm) return false;
  const std::string line = utf8_truncate(trim_space(text), static_cast<size_t>(std::max(1, limits_.chat_len)));
  if (line.empty()) return false;
  last_scope_ = scope;
  send(Json::object().set("t", "chat").set("scope", scope_name(scope)).set("text", line));
  return true;
}

bool MultiplayerClient::send_pm(const std::string& to, std::string_view text) {
  if (state_ != State::Connected || !valid_pid(to) || to == pid_) return false;
  const std::string line = utf8_truncate(trim_space(text), static_cast<size_t>(std::max(1, limits_.chat_len)));
  if (line.empty()) return false;
  last_scope_ = ChatScope::Pm;
  send(Json::object().set("t", "pm").set("to", to).set("text", line));
  return true;
}

const ChatLine* MultiplayerClient::newest_pm() const {
  for (auto line = pm_chat_.rbegin(); line != pm_chat_.rend(); ++line)
    if (!line->pid.empty() && line->pid != pid_) return &*line;
  return nullptr;
}

const std::deque<ChatLine>& MultiplayerClient::chat(ChatScope scope) const {
  switch (scope) {
    case ChatScope::Room: return room_chat_;
    case ChatScope::Global: return global_chat_;
    case ChatScope::Pm: return pm_chat_;
  }
  return room_chat_;
}

std::deque<ChatLine>& MultiplayerClient::log_of(ChatScope scope) {
  switch (scope) {
    case ChatScope::Room: return room_chat_;
    case ChatScope::Global: return global_chat_;
    case ChatScope::Pm: return pm_chat_;
  }
  return room_chat_;
}

bool MultiplayerClient::can_challenge() const {
  return state_ == State::Connected && identity_.pvp && !incoming_ && !outgoing_ && !accepted_ && pvp_battle_.empty() && !pvp_start_;
}

bool MultiplayerClient::challenge(const std::string& to, const std::string& name, const PartyCard& card) {
  if (!can_challenge() || !valid_pid(to) || to == pid_) return false;
  Json message = Json::object();
  message.set("t", "challenge").set("to", to).set("card", card_json(card)).set("ver", identity_.version).set("data", battle_data_);
  send(message);
  outgoing_ = OutgoingChallenge{to, name, now_ + kChallengeTime, now_ + kChallengeTime + kChallengeGrace};
  sent_card_ = card;
  return true;
}

void MultiplayerClient::cancel_challenge() {
  if (!outgoing_) return;
  if (state_ == State::Connected) send(Json::object().set("t", "challenge_cancel"));
  outgoing_.reset();
}

void MultiplayerClient::reply(const std::string& from, const char* reason) {
  send(Json::object().set("t", "challenge_reply").set("from", from).set("accept", false).set("reason", reason));
}

bool MultiplayerClient::accept_challenge(const PartyCard& card) {
  if (!incoming_ || state_ != State::Connected) return false;
  Json message = Json::object();
  message.set("t", "challenge_reply").set("from", incoming_->from).set("accept", true).set("card", card_json(card));
  message.set("ver", identity_.version).set("data", battle_data_);
  send(message);
  accepted_ = Accepted{incoming_->from, incoming_->name, now_ + kStartWait};
  sent_card_ = card;
  incoming_.reset();
  return true;
}

void MultiplayerClient::decline_challenge() {
  if (!incoming_) return;
  if (state_ == State::Connected) reply(incoming_->from, "declined");
  incoming_.reset();
}

std::optional<PvpStart> MultiplayerClient::take_pvp_start() {
  std::optional<PvpStart> start = std::move(pvp_start_);
  pvp_start_.reset();
  return start;
}

void MultiplayerClient::end_pvp(PvpResult result) {
  if (pvp_battle_.empty()) return;
  static constexpr const char* kResults[] = {"won", "lost", "aborted", "draw", "desync"};
  if (state_ == State::Connected) send(Json::object().set("t", "pvp_end").set("battle", pvp_battle_).set("result", kResults[static_cast<int>(result)]));
  pvp_battle_.clear();
  pvp_start_.reset();
  pvp_ = {};
}

bool MultiplayerClient::send_pvp_side(std::string_view bytes) {
  if (pvp_battle_.empty() || !pvp_.side.empty() || bytes.empty() || bytes.size() > kMaxSideBytes) return false;
  pvp_.side = base64_encode(bytes);
  pvp_.queued.push_back({.side = true});
  return true;
}

void MultiplayerClient::send_pvp_step(std::uint32_t seq, std::string_view cmd, std::uint64_t hash) {
  if (pvp_battle_.empty() || (!cmd.empty() && !valid_cmd_text(cmd))) return;
  pvp_.queued.push_back({.seq = seq, .cmd = std::string(cmd), .hash = hash});
}

bool MultiplayerClient::take_pvp_step(PvpStep& step) {
  if (pvp_.inbox.empty()) return false;
  step = std::move(pvp_.inbox.front());
  pvp_.inbox.pop_front();
  return true;
}

double MultiplayerClient::pvp_opponent_away() const { return pvp_.away_until < 0 ? -1.0 : std::max(0.0, pvp_.away_until - now_); }

double MultiplayerClient::pvp_self_away() const { return pvp_.dropped_at < 0 ? -1.0 : now_ - pvp_.dropped_at; }

// The queued battle messages, at most kPvpPerSecond in any second, each with the last relay number received; kept for a resend.
void MultiplayerClient::send_pvp_due() {
  if (pvp_battle_.empty() || pvp_.resuming) return;
  while (!pvp_.queued.empty()) {
    if (pvp_.window < 0 || now_ - pvp_.window >= 1.0) pvp_.window = now_, pvp_.window_sent = 0;
    if (pvp_.window_sent >= kPvpPerSecond) return;
    PvpOut out = std::move(pvp_.queued.front());
    pvp_.queued.pop_front();
    Json message = Json::object();
    if (out.side) {
      message.set("t", "pvp_side").set("battle", pvp_battle_).set("side", pvp_.side);
    } else {
      message.set("t", out.cmd.empty() ? "pvp_hash" : "pvp_cmd").set("battle", pvp_battle_).set("seq", static_cast<std::int64_t>(out.seq));
      if (!out.cmd.empty()) message.set("cmd", out.cmd);
      message.set("hash", hash_text(out.hash)).set("ack", pvp_.relay);
    }
    send(message);
    ++pvp_.window_sent;
    pvp_.sent.push_back(std::move(out));
    while (pvp_.sent.size() > kPvpResendKeep) pvp_.sent.pop_front();
  }
}

// After pvp_resumed: everything the server has not taken goes out again in order (the side when it does not hold it, the steps after
// its seq). A step the client no longer keeps cannot be sent again, which ends the link.
void MultiplayerClient::resend_after_resume(std::uint32_t server_seq, bool side_known) {
  std::deque<PvpOut> again;
  std::uint32_t last = 0;
  for (std::deque<PvpOut>* list : {&pvp_.sent, &pvp_.queued})
    for (PvpOut& out : *list) {
      if (out.side) continue;
      last = std::max(last, out.seq);
      if (out.seq > server_seq) again.push_back(std::move(out));
    }
  if (last > server_seq && (again.empty() || again.front().seq != server_seq + 1)) pvp_.gone = PvpGone::Gap;
  if (!side_known && !pvp_.side.empty()) again.push_front({.side = true});
  pvp_.sent.clear();
  pvp_.queued = std::move(again);
}

bool MultiplayerClient::take_relay_number(const Json& message) {
  const std::int64_t number = message.int_of("n", -1);
  if (number >= 0 && number <= pvp_.relay) return false;
  if (number != pvp_.relay + 1) {
    log::warn("multiplayer: battle message {} after {}: messages are missing", number, pvp_.relay);
    pvp_.gone = PvpGone::Gap;
    return false;
  }
  pvp_.relay = number;
  pvp_.last_heard = now_;
  return true;
}

// The battle link's messages, for the battle the player is in only.
void MultiplayerClient::handle_pvp(const std::string& type, const Json& message) {
  if (pvp_battle_.empty() || message.string_of("battle") != pvp_battle_ || pvp_.gone != PvpGone::None) return;
  if (type == "pvp_side") {
    if (!take_relay_number(message)) return;
    std::optional<std::string> side = base64_decode(message.string_of("side"));
    if (!side || side->size() > kMaxSideBytes || pvp_.opponent_side) {
      pvp_.gone = PvpGone::Gap;
      return;
    }
    pvp_.opponent_side = std::move(side);
  } else if (type == "pvp_cmd" || type == "pvp_hash") {
    if (!take_relay_number(message)) return;
    PvpStep step;
    const std::int64_t seq = message.int_of("seq", -1);
    const bool command = type == "pvp_cmd";
    if (command) step.cmd = message.string_of("cmd");
    if (seq < 1 || seq > 0x7FFFFFFF || !parse_hash(message.string_of("hash"), step.hash) || (command && !valid_cmd_text(step.cmd)) ||
        pvp_.inbox.size() >= kPvpInboxMax) {
      pvp_.gone = PvpGone::Gap;
      return;
    }
    step.seq = static_cast<std::uint32_t>(seq);
    pvp_.inbox.push_back(std::move(step));
  } else if (type == "pvp_away") {
    pvp_.away_until = now_ + std::clamp(static_cast<double>(message.int_of("ms")) / 1000.0, 0.0, kAwayTime);
    pvp_.last_heard = now_;
  } else if (type == "pvp_back") {
    pvp_.away_until = -1.0;
    pvp_.last_heard = now_;
  } else if (type == "pvp_gone") {
    const std::string& reason = message.string_of("reason");
    pvp_.gone = reason == "left" ? PvpGone::Left : reason == "aborted" ? PvpGone::Aborted : reason == "forfeit" ? PvpGone::Forfeit : PvpGone::Closed;
    log::info("multiplayer: battle {} gone ({})", pvp_battle_, reason);
  } else if (type == "pvp_resumed") {
    const std::int64_t seq = message.int_of("seq", -1);
    if (!pvp_.resuming || seq < 0 || seq > 0x7FFFFFFF) return;
    pvp_.resuming = false;
    pvp_.dropped_at = -1.0;
    pvp_.last_heard = now_;
    resend_after_resume(static_cast<std::uint32_t>(seq), message["side"].as_bool());
    log::info("multiplayer: battle {} resumed at seq {}", pvp_battle_, seq);
  }
}

void MultiplayerClient::challenge_ended(const std::string& pid, const std::string& name, ChallengeEnd reason, bool outgoing) {
  last_end_ = ChallengeEnded{pid, name, reason, outgoing};
  log::info("multiplayer: challenge {} {} ended: {}", outgoing ? "to" : "from", pid, kEndNames[static_cast<int>(reason)]);
  if (std::string text = end_notice(reason, name, outgoing); !text.empty()) add_notice(std::move(text), ChatScope::Global);
}

// The client's own time limits: an incoming challenge left unanswered is answered `timeout`; an outgoing one, or an accept, that never
// got its answer is given up.
void MultiplayerClient::check_challenge_times() {
  if (incoming_ && now_ >= incoming_->deadline) {
    const IncomingChallenge expired = std::move(*incoming_);
    incoming_.reset();
    reply(expired.from, "timeout");
    challenge_ended(expired.from, expired.name, ChallengeEnd::Timeout, false);
  }
  if (outgoing_ && now_ >= outgoing_->deadline) {
    const OutgoingChallenge expired = std::move(*outgoing_);
    outgoing_.reset();
    challenge_ended(expired.to, expired.name, ChallengeEnd::Timeout, true);
  }
  if (accepted_ && now_ >= accepted_->deadline) {
    const Accepted expired = std::move(*accepted_);
    accepted_.reset();
    challenge_ended(expired.from, expired.name, ChallengeEnd::Timeout, false);
  }
}

// A pvp_start the host has not taken yet goes with them: its battle never starts here.
void MultiplayerClient::clear_challenges() {
  incoming_.reset();
  outgoing_.reset();
  accepted_.reset();
  if (pvp_start_) {
    pvp_battle_.clear();
    pvp_ = {};
  }
  pvp_start_.reset();
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
  party->pvp = entry["pvp"].as_bool();
  party->models = read_models(entry["models"]);
  party_seq_.erase(pid);
  apply_pres(*party, entry["pres"]);
}

void MultiplayerClient::add_chat(ChatLine line) {
  last_ts_ = std::max(last_ts_, line.ts);
  std::deque<ChatLine>& log = log_of(line.scope);
  log.push_back(std::move(line));
  while (log.size() > kChatKeep) log.pop_front();
  ++chat_serial_;
}

void MultiplayerClient::add_notice(std::string text, ChatScope scope) {
  ChatLine line;
  line.scope = scope;
  line.text = std::move(text);
  line.ts = last_ts_;
  line.received = now_;
  add_chat(std::move(line));
}

// A chat line. A private one must be from or to the local player, with a valid recipient.
void MultiplayerClient::handle_chat(const Json& message) {
  ChatLine line;
  if (!parse_scope(message.string_of("scope"), line.scope)) return;
  line.pid = message.string_of("pid");
  if (!valid_pid(line.pid)) return;
  if (line.scope == ChatScope::Pm) {
    line.to = message.string_of("to");
    if (!valid_pid(line.to) || (line.pid != pid_ && line.to != pid_)) return;
    line.to_name = message.string_of("to_name");
  }
  line.name = message.string_of("name");
  line.text = message.string_of("text");
  line.admin = message["admin"].as_bool();
  line.ts = message.int_of("ts");
  line.received = now_;
  add_chat(std::move(line));
}

// A challenge to the local player: answered `off` with challenges off and `busy` unless the player is free in a field room (walking or
// typing in the chat) with nothing else pending; otherwise kept for the popup.
void MultiplayerClient::handle_challenge(const Json& message) {
  const std::string& from = message.string_of("from");
  if (!valid_pid(from) || from == pid_) return;
  if (!identity_.pvp) return reply(from, "off");
  const bool free = (local_.pose.state == PresenceState::Field || local_.pose.state == PresenceState::Typing) && !local_.room.empty();
  if (!free || incoming_ || outgoing_ || accepted_ || !pvp_battle_.empty() || pvp_start_) return reply(from, "busy");
  IncomingChallenge challenge;
  challenge.from = from;
  challenge.name = utf8_truncate(message.string_of("name"), static_cast<size_t>(std::max(1, limits_.name_len)));
  challenge.star = clamped(message, "star", 0, 999);
  challenge.card = read_card(message["card"], static_cast<size_t>(std::max(1, limits_.name_len)));
  const double seconds = static_cast<double>(std::clamp<std::int64_t>(message.int_of("expires"), 0, 60000)) / 1000.0;
  challenge.deadline = now_ + std::min(seconds, kMaxChallengeTime);
  log::info("multiplayer: challenge from {}", from);
  incoming_ = std::move(challenge);
}

// The end of the challenge with `pid`, whichever side the local player is on; an end for no challenge of theirs is dropped.
void MultiplayerClient::handle_challenge_end(const Json& message) {
  const std::string& pid = message.string_of("pid");
  ChallengeEnd reason;
  if (!valid_pid(pid) || !parse_end(message.string_of("reason"), reason)) return;
  if (outgoing_ && outgoing_->to == pid) {
    const std::string name = outgoing_->name;
    outgoing_.reset();
    challenge_ended(pid, name, reason, true);
  } else if (incoming_ && incoming_->from == pid) {
    const std::string name = incoming_->name;
    incoming_.reset();
    challenge_ended(pid, name, reason, false);
  } else if (accepted_ && accepted_->from == pid) {
    const std::string name = accepted_->name;
    accepted_.reset();
    challenge_ended(pid, name, reason, false);
  }
}

// The battle's start, taken only for the challenge the local player sent or accepted.
void MultiplayerClient::handle_pvp_start(const Json& message) {
  const std::string& opponent = message.string_of("opponent");
  const bool ours = (outgoing_ && outgoing_->to == opponent) || (accepted_ && accepted_->from == opponent);
  const std::string& first = message.string_of("first");
  const std::string& battle = message.string_of("battle");
  const std::int64_t seed = message.int_of("seed", -1);
  if (!ours || !valid_pid(opponent) || !valid_pid(battle) || (first != pid_ && first != opponent) || seed < 0 || seed > 0xFFFFFFFFll) return;
  PvpStart start;
  start.battle = battle;
  start.seed = static_cast<std::uint32_t>(seed);
  start.first = first;
  start.opponent = opponent;
  start.name = utf8_truncate(message.string_of("name"), static_cast<size_t>(std::max(1, limits_.name_len)));
  start.star = clamped(message, "star", 0, 999);
  start.card = read_card(message["card"], static_cast<size_t>(std::max(1, limits_.name_len)));
  start.opening_stamina = clamped(message, "opening_stamina", 0, 70);
  start.own = sent_card_;
  outgoing_.reset();
  accepted_.reset();
  pvp_battle_ = battle;
  if (local_.pose.state == PresenceState::Battle || local_.room.empty()) {
    log::info("multiplayer: battle {} with {} dropped, the player is no longer free", battle, opponent);
    end_pvp(PvpResult::Aborted);
    return;
  }
  log::info("multiplayer: battle {} with {} starts, {} first", battle, opponent, first);
  pvp_ = {};
  pvp_.last_heard = now_;
  pvp_start_ = std::move(start);
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
    if (!pvp_battle_.empty() && pvp_.resuming) send(Json::object().set("t", "pvp_resume").set("battle", pvp_battle_).set("ack", pvp_.relay));
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
      party->pvp = message["pvp"].as_bool(party->pvp);
    }
  } else if (type == "party") {
    if (RemoteParty* party = find_party(message.string_of("pid"))) party->models = read_models(message["models"]);
  } else if (type == "pres") {
    if (RemoteParty* party = find_party(message.string_of("pid"))) apply_pres(*party, message);
  } else if (type == "occupancy") {
    occupancy_ = static_cast<int>(std::max<std::int64_t>(0, message.int_of("others")));
  } else if (type == "chat") {
    handle_chat(message);
  } else if (type == "challenge") {
    handle_challenge(message);
  } else if (type == "challenge_end") {
    handle_challenge_end(message);
  } else if (type == "pvp_start") {
    handle_pvp_start(message);
  } else if (type.starts_with("pvp_")) {
    handle_pvp(type, message);
  } else if (type == "error") {
    const std::string& code = message.string_of("code");
    if (code == "rate") {
      add_notice("(Not sent: too many messages, wait a moment)", last_scope_);
    } else if (code == "offline") {
      add_notice("(Not sent: that player is not online)", ChatScope::Pm);
    } else if (code == "version" || code == "banned") {
      const bool banned = code == "banned";
      state_ = banned ? State::Banned : State::VersionTooOld;
      server_message_ = message.string_of("msg");
      log::warn("multiplayer: {} ({})", banned ? "banned by the server" : "the server needs a newer version", server_message_);
      transport_->close();
      parties_.clear();
      occupancy_ = 0;
      clear_challenges();
      pvp_battle_.clear();
      pvp_ = {};
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
