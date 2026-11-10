// The versus link over the multiplayer client: the command text, the order and state checks, and the link's end.
#include "game/battle/versus_net_link.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <vector>

#include "core/log.h"
#include "data/tables.h"

namespace oc::game {

namespace {

constexpr int kLastKind = static_cast<int>(battle::Command::Kind::Form);
// An attack chain holds at most this many hits.
constexpr size_t kChainHits = 4;

}  // namespace

std::string versus_command_text(const battle::Command& cmd) {
  std::string text = std::format("{} {} {} {} {} {} {}", static_cast<int>(cmd.kind), cmd.actor, cmd.target, cmd.cell, cmd.form, cmd.timeout ? 1 : 0,
                                 cmd.swap_in);
  for (const int strength : cmd.chain) text += std::format(" {}", strength);
  return text;
}

std::optional<battle::Command> parse_versus_command(std::string_view text) {
  std::vector<int> values;
  while (!text.empty()) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end == text.data()) return std::nullopt;
    values.push_back(value);
    text.remove_prefix(static_cast<size_t>(end - text.data()));
    if (!text.empty()) {
      if (text.front() != ' ' || text.size() == 1) return std::nullopt;
      text.remove_prefix(1);
    }
  }
  if (values.size() < 7 || values.size() > 7 + kChainHits) return std::nullopt;
  const auto in = [](int value, int low, int high) { return value >= low && value <= high; };
  battle::Command cmd;
  if (!in(values[0], 0, kLastKind) || !in(values[1], 0, battle::kPlayerUnits - 1) || !in(values[2], -1, battle::kUnits - 1) ||
      !in(values[3], -1, data::kGridCells - 1) || !in(values[4], -1, 255) || !in(values[5], 0, 1) || !in(values[6], -1, 255))
    return std::nullopt;
  cmd.kind = static_cast<battle::Command::Kind>(values[0]);
  cmd.actor = values[1];
  cmd.target = values[2];
  cmd.cell = values[3];
  cmd.form = values[4];
  cmd.timeout = values[5] != 0;
  cmd.swap_in = values[6];
  for (size_t i = 7; i < values.size(); ++i) {
    if (!in(values[i], 0, 2)) return std::nullopt;
    cmd.chain.push_back(values[i]);
  }
  return cmd;
}

NetworkVersusLink::NetworkVersusLink(net::MultiplayerClient& client, int side) : client_(client), battle_(client.pvp_battle()), side_(side & 1) {}

void NetworkVersusLink::pull() {
  net::PvpStep step;
  while (client_.take_pvp_step(step)) {
    if (!step.cmd.empty()) {
      commands_.push_back(std::move(step));
      continue;
    }
    const auto own = own_hashes_.find(step.seq);
    if (own == own_hashes_.end()) continue;
    if (own->second != step.hash) mismatch(step.seq, own->second, step.hash);
    own_hashes_.erase(own);
  }
}

void NetworkVersusLink::mismatch(u32 seq, u64 ours, u64 theirs) {
  if (desync_) return;
  desync_ = true;
  log::warn("battle: versus state check failed after command {}: this game {:016x}, the other {:016x}", seq, ours, theirs);
}

void NetworkVersusLink::send(const battle::Command& cmd) { pending_ = cmd; }

// The other side's next command while it acts. While none has come the other game's silence is timed, except while either player is
// known to be out of reach (the server's away time rules then).
bool NetworkVersusLink::poll(const battle::Battle& battle, battle::Command& cmd) {
  pull();
  if (battle.acting_side() == side_ || status() != Status::Open) {
    waiting_since_ = -1.0;
    return false;
  }
  if (commands_.empty()) {
    if (away()) {
      waiting_since_ = -1.0;
    } else {
      const double now = client_.now();
      if (waiting_since_ < 0) waiting_since_ = now;
      if (now - std::max(waiting_since_, client_.pvp_last_heard()) >= kSilenceLimit && !silent_) {
        silent_ = true;
        log::warn("battle: versus: the other game sent nothing for {:.0f} s while its side acts", kSilenceLimit);
      }
    }
    return false;
  }
  net::PvpStep step = std::move(commands_.front());
  commands_.pop_front();
  waiting_since_ = -1.0;
  if (step.seq != applied_ + 1) {
    log::warn("battle: versus command {} of the other game arrived after command {}", step.seq, applied_);
    desync_ = true;
    return false;
  }
  const std::optional<battle::Command> parsed = parse_versus_command(step.cmd);
  if (!parsed) {
    log::warn("battle: versus command {} of the other game cannot be read: '{}'", step.seq, step.cmd);
    broken_ = true;
    return false;
  }
  cmd = *parsed;
  expected_ = std::move(step);
  return true;
}

void NetworkVersusLink::send_hash(u32 seq, u64 hash) {
  pull();
  applied_ = seq;
  if (pending_) {
    client_.send_pvp_step(seq, versus_command_text(*pending_), hash);
    pending_.reset();
    own_hashes_[seq] = hash;
    while (own_hashes_.size() > kHashKeep) own_hashes_.erase(own_hashes_.begin());
    return;
  }
  if (expected_ && expected_->seq == seq && expected_->hash != hash) mismatch(seq, hash, expected_->hash);
  expected_.reset();
  client_.send_pvp_step(seq, {}, hash);
}

VersusLink::Status NetworkVersusLink::status() const {
  if (desync_) return Status::Desync;
  if (broken_ || client_.pvp_battle() != battle_) return Status::Closed;
  switch (client_.pvp_gone()) {
    case net::PvpGone::None: break;
    case net::PvpGone::Left: return Status::Conceded;
    case net::PvpGone::Forfeit: return Status::Forfeited;
    case net::PvpGone::Aborted:
    case net::PvpGone::Closed:
    case net::PvpGone::Gap: return Status::Closed;
  }
  if (client_.pvp_self_away() >= net::MultiplayerClient::kAwayTime + kSelfGrace) return Status::Forfeited;
  return silent_ ? Status::Conceded : Status::Open;
}

std::optional<VersusLink::Away> NetworkVersusLink::away() const {
  const double self = client_.pvp_self_away();
  if (self >= 0) return Away{true, static_cast<int>(std::ceil(std::max(0.0, net::MultiplayerClient::kAwayTime - self)))};
  const double other = client_.pvp_opponent_away();
  if (other >= 0) return Away{false, static_cast<int>(std::ceil(other))};
  return std::nullopt;
}

}  // namespace oc::game
