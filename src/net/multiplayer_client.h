#pragma once
// The multiplayer presence and chat client. It owns the connection's state machine: the
// hello, the reconnect back-off, the keep-alive ping, the room / party / profile messages on change, the presence sends (only while
// someone else is in the room, at most 10 a second, only on change), the table of remote parties in the local player's room, the chat
// log per scope (room, global, private), the player's mutes, the battle challenges up to the battle's start (the incoming and
// outgoing challenge, the automatic busy / off answers, the time limits, the pvp_start) and the battle link (the side exchange, the paced
// command and state-check stream with its relay numbers, the resume after a dropped connection, the opponent's absence and end). All time
// comes from the `now` (seconds) passed to update(), so tests run it on a fake clock over a fake Transport.
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "net/presence.h"
#include "net/transport.h"

namespace oc::net {

class Json;

inline constexpr int kProtocolVersion = 1;
// Remote parties the client keeps for one room; a server that announces more is not followed past this.
inline constexpr size_t kMaxParties = 64;

enum class ChatScope { Room, Global, Pm };

struct ChatLine {
  ChatScope scope = ChatScope::Room;
  // Empty for the client's own notices (a rate-limited message).
  std::string pid;
  std::string name;
  std::string text;
  // The server's Unix ms, and the client clock (update's `now`) when the line arrived.
  std::int64_t ts = 0;
  double received = 0.0;
  // The server vouches that the sender holds its admin token.
  bool admin = false;
  // A private line: the recipient. The sender's own copy has `pid` equal to the client's pid.
  std::string to;
  std::string to_name;
};

// What the client may say about itself.
struct ClientIdentity {
  std::string player_id;
  std::string name;
  int star = 0;
  std::string version;
  // The server's admin token ([Multiplayer] AdminToken); sent in hello only when set.
  std::string admin_token;
  // Other players may challenge this one ([Multiplayer] Challenges): the `pvp` flag.
  bool pvp = true;
};

// Why a challenge ended without a battle: the server's challenge_end reasons; Timeout is also the client's own when an answer never came.
enum class ChallengeEnd { Declined, Busy, Timeout, Off, Cancelled, Left, Version, Data, Cooldown, Pending };

// A challenge to the local player, waiting for Accept / Decline until `deadline` (the client's clock).
struct IncomingChallenge {
  std::string from;
  std::string name;
  int star = 0;
  PartyCard card;
  double deadline = 0.0;
};

// The local player's own challenge, waiting for the answer: the server ends it at `expires`; the client gives it up at `deadline` when no
// answer came.
struct OutgoingChallenge {
  std::string to;
  std::string name;
  double expires = 0.0;
  double deadline = 0.0;
};

// How the last challenge ended: the other player, and whether it was the local player's own challenge.
struct ChallengeEnded {
  std::string pid;
  std::string name;
  ChallengeEnd reason = ChallengeEnd::Declined;
  bool outgoing = false;
};

// A challenge was accepted: the battle's id and seed, the pid of the side that moves first, the opponent with their card, and the first
// side's stamina on its opening turn in tenths. `own` is the card this side sent (not part of the message).
struct PvpStart {
  std::string battle;
  std::uint32_t seed = 0;
  std::string first;
  std::string opponent;
  std::string name;
  int star = 0;
  PartyCard card;
  int opening_stamina = 0;
  PartyCard own;
};

enum class PvpResult { Won, Lost, Aborted, Draw, Desync };

// One step of the other game's battle stream: a command it applied (`cmd`, the link's text) or, with `cmd` empty, its state check after a
// command this game sent; `seq` counts the commands both games applied, `hash` the sender's state hash after it.
struct PvpStep {
  std::uint32_t seq = 0;
  std::string cmd;
  std::uint64_t hash = 0;
};

// How the battle ended outside its own rules: the opponent stayed away (Left: this side wins), the opponent's game ended it without a
// result (Aborted), this player stayed away too long (Forfeit: this side lost), the server no longer holds the battle (Closed), or the
// server's relay numbers skipped messages that cannot come back (Gap).
enum class PvpGone { None, Left, Aborted, Forfeit, Closed, Gap };

class MultiplayerClient {
 public:
  enum class State {
    // Not started, or stopped.
    Off,
    Connecting,
    // The socket is open and the hello sent; waiting for the welcome.
    Greeting,
    Connected,
    // Dropped: the next attempt waits for the back-off.
    Waiting,
    // The server refused for good (banned, or the protocol is too old); no further attempts until start() again.
    Banned,
    VersionTooOld,
    // The address may not be used (server_url_allowed); nothing is attempted until start() with another one.
    BadAddress,
  };

  // Server limits from the welcome (defaults until then): chat and name lengths in code points, pres messages a second.
  struct Limits {
    int chat_len = 200;
    int name_len = 24;
    int pres_hz = 10;
  };

  // The largest limits a server's welcome may set.
  static constexpr int kMaxChatLen = 1000;
  static constexpr int kMaxNameLen = 64;
  static constexpr int kMaxPresHz = 30;

  // How many lines each scope keeps.
  static constexpr size_t kChatKeep = 200;
  // Back-off before the attempts after a drop, in seconds; the last one repeats.
  static constexpr double kBackoff[] = {2, 4, 8, 16, 30};
  // The client sends a ping after this long without sending anything.
  static constexpr double kPingAfter = 20.0;
  // A connection that has received nothing for this long (a ping is answered within it) is dropped and made again.
  static constexpr double kSilentDrop = 75.0;
  // The shortest time between two pres messages.
  static constexpr double kPresInterval = 0.1;
  // The server's time limit for an answer; the client gives its own challenge up kChallengeGrace later when no answer came. An accept
  // waits kStartWait for the pvp_start.
  static constexpr double kChallengeTime = 30.0;
  static constexpr double kChallengeGrace = 5.0;
  static constexpr double kStartWait = 10.0;
  // The longest time limit taken from an incoming challenge, in seconds.
  static constexpr double kMaxChallengeTime = 60.0;
  // During a battle: a ping after kBattlePingAfter without sending, a drop after kBattleSilentDrop without receiving, reconnects every
  // kBattleRetry, at most kPvpPerSecond battle messages sent a second (the server allows 30), and the time an absent player has to come
  // back (also the longest `ms` taken from the server's pvp_away).
  static constexpr double kBattlePingAfter = 5.0;
  static constexpr double kBattleSilentDrop = 15.0;
  static constexpr double kBattleRetry = 2.0;
  static constexpr int kPvpPerSecond = 20;
  static constexpr double kAwayTime = 30.0;
  // The longest side (bytes before base64) and command text, the battle messages kept for a resend after a resume, and the received steps
  // the client holds before the battle takes them.
  static constexpr size_t kMaxSideBytes = 9216;
  static constexpr size_t kMaxCmdText = 64;
  static constexpr size_t kPvpResendKeep = 1024;
  static constexpr size_t kPvpInboxMax = 4096;

  explicit MultiplayerClient(std::unique_ptr<Transport> transport);
  ~MultiplayerClient();
  MultiplayerClient(const MultiplayerClient&) = delete;
  MultiplayerClient& operator=(const MultiplayerClient&) = delete;

  // Connects to `url` (at the next update) and keeps the connection up until stop(). A different url or player id reconnects.
  void start(const std::string& url, const ClientIdentity& identity);
  void stop();
  bool started() const { return state_ != State::Off; }

  // The name or Star Level to show; a change is sent as `profile`.
  void set_profile(const std::string& name, int star);
  // The local player as the field reports it; room / party / pres are sent from update() as the rules allow.
  void set_local(const LocalPresence& local);

  // Reads the transport and sends what is due. Call once per frame.
  void update(double now);

  // A room or global chat line from the player; false when it is empty or not connected. Cut to the server's chat_len.
  bool send_chat(ChatScope scope, std::string_view text);
  // A private line to player `to`, as send_chat; false also for a pid that is not valid or is the player's own.
  bool send_pm(const std::string& to, std::string_view text);
  // The newest private line from another player (the one a reply answers), or null.
  const ChatLine* newest_pm() const;

  // Challenges. set_challenges is [Multiplayer] Challenges, sent as `pvp` (a change goes out as `profile`); set_battle_data the battle
  // data fingerprint sent with challenges and accepts ("" while unknown).
  void set_challenges(bool on) { identity_.pvp = on; }
  bool challenges_on() const { return identity_.pvp; }
  void set_battle_data(std::string data) { battle_data_ = std::move(data); }
  // Free to challenge: connected, challenges on, no challenge pending either way and not in a battle.
  bool can_challenge() const;
  // Challenges player `to` (`name` for the notices) with `card`; false when not free or `to` is not a valid other pid.
  bool challenge(const std::string& to, const std::string& name, const PartyCard& card);
  void cancel_challenge();
  // Answers the incoming challenge. An accept then waits for the pvp_start (or the server's version / data refusal).
  bool accept_challenge(const PartyCard& card);
  void decline_challenge();
  const std::optional<IncomingChallenge>& incoming() const { return incoming_; }
  const std::optional<OutgoingChallenge>& outgoing() const { return outgoing_; }
  // An accept was sent and the pvp_start has not come yet.
  bool accepted() const { return accepted_.has_value(); }
  const std::optional<ChallengeEnded>& last_challenge_end() const { return last_end_; }
  // The pvp_start, once, for the host to show the versus screen and start the battle.
  std::optional<PvpStart> take_pvp_start();
  // The battle the player is in since its pvp_start ("" none); other challenges are answered busy until end_pvp. A dropped connection
  // keeps it: the client resumes it after the next welcome.
  const std::string& pvp_battle() const { return pvp_battle_; }
  void end_pvp(PvpResult result);

  // The battle link (section 8.4). The side is sent once, base64 inside; a step is sent for every command this game applies (`cmd` the
  // command's text for its own, empty for a state check after the other game's) and goes out paced, with the last relay number received.
  bool send_pvp_side(std::string_view bytes);
  void send_pvp_step(std::uint32_t seq, std::string_view cmd, std::uint64_t hash);
  // The other side as its game sent it (decoded bytes), once it arrived.
  const std::optional<std::string>& pvp_opponent_side() const { return pvp_.opponent_side; }
  // The next step of the other game, in its order; false when none is waiting.
  bool take_pvp_step(PvpStep& step);
  // Seconds left for an absent opponent to come back (-1 while it is present), and seconds this client has been without its connection
  // during the battle (-1 while connected and resumed).
  double pvp_opponent_away() const;
  double pvp_self_away() const;
  // When the opponent was last reachable again (its pvp_back, or the end of its away time); -1 = never away.
  double pvp_opponent_back() const;
  PvpGone pvp_gone() const { return pvp_.gone; }
  // The clock of the last update().
  double now() const { return now_; }

  State state() const { return state_; }
  // The status line: "Connected", "Connecting...", "Could not reach the server; retrying in 8 s", the server's version message...
  std::string status_text() const;
  const std::string& pid() const { return pid_; }
  // The server accepted the admin token.
  bool admin() const { return admin_; }
  // Other players in the local player's room (from the server's `occupancy`).
  int occupancy() const { return occupancy_; }

  // The remote parties in the local player's room, in the order they arrived.
  const std::vector<RemoteParty>& parties() const { return parties_; }
  // One scope's chat, oldest first; `serial` rises with every line added to either scope.
  const std::deque<ChatLine>& chat(ChatScope scope) const;
  std::uint64_t chat_serial() const { return chat_serial_; }

  // The player's mutes (pids). Nothing about them is sent.
  void set_muted(std::set<std::string> pids) { muted_ = std::move(pids); }
  bool muted(const std::string& pid) const { return muted_.count(pid) != 0; }

 private:
  void connect_now();
  void dropped(const std::string& reason);
  void send(const Json& message);
  void handle(const Json& message);
  void handle_party_entry(const Json& entry);
  void apply_pres(RemoteParty& party, const Json& pres);
  RemoteParty* find_party(const std::string& pid);
  void add_chat(ChatLine line);
  void add_notice(std::string text, ChatScope scope);
  std::deque<ChatLine>& log_of(ChatScope scope);
  void handle_chat(const Json& message);
  void handle_challenge(const Json& message);
  void handle_challenge_end(const Json& message);
  void handle_pvp_start(const Json& message);
  void handle_pvp(const std::string& type, const Json& message);
  // A relayed battle message's relay number: false for a repeat; a skip ends the link with Gap.
  bool take_relay_number(const Json& message);
  void send_pvp_due();
  void resend_after_resume(std::uint32_t server_seq, bool side_known);
  void reply(const std::string& from, const char* reason);
  void challenge_ended(const std::string& pid, const std::string& name, ChallengeEnd reason, bool outgoing);
  void check_challenge_times();
  void clear_challenges();
  void send_changes();
  // The name as sent: cut to the server's name_len.
  std::string sent_name() const;
  void send_pres_if_due();

  std::unique_ptr<Transport> transport_;
  State state_ = State::Off;
  std::string url_;
  ClientIdentity identity_;
  std::string pid_;
  bool admin_ = false;
  Limits limits_;
  std::string last_error_;
  std::string server_message_;
  // Waiting: when the next attempt starts; attempts_ = the drops since the last welcome.
  double retry_at_ = 0.0;
  int attempts_ = 0;
  bool connect_pending_ = false;
  // The clock of the current update(); when the client last sent and last received anything.
  double now_ = 0.0;
  double last_sent_ = 0.0, last_received_ = 0.0;

  LocalPresence local_;
  // What the server was last told on this connection (the *_sent_ flags are false until the first).
  std::string sent_room_;
  bool room_sent_ = false;
  PartyModels sent_models_{-1, -1, -1};
  bool party_sent_ = false;
  std::string sent_name_;
  int sent_star_ = 0;
  bool sent_pvp_ = true;
  LeaderPose sent_pose_;
  bool pose_sent_ = false;
  double last_pres_ = -1.0;
  std::uint32_t pres_seq_ = 0;
  int occupancy_ = 0;

  std::vector<RemoteParty> parties_;
  // The last pres seq taken from each remote party.
  std::map<std::string, std::int64_t> party_seq_;
  std::deque<ChatLine> room_chat_, global_chat_, pm_chat_;
  // The scope of the player's last chat line: a rate-limit notice goes there, stamped with the newest server time seen.
  ChatScope last_scope_ = ChatScope::Room;
  std::int64_t last_ts_ = 0;
  std::uint64_t chat_serial_ = 0;
  std::set<std::string> muted_;

  std::string battle_data_;
  std::optional<IncomingChallenge> incoming_;
  std::optional<OutgoingChallenge> outgoing_;
  // An accepted challenge's challenger, waiting for the pvp_start until the deadline.
  struct Accepted {
    std::string from;
    std::string name;
    double deadline = 0.0;
  };
  std::optional<Accepted> accepted_;
  // The card sent with the pending challenge or accept, for the versus screen.
  PartyCard sent_card_;
  std::optional<ChallengeEnded> last_end_;
  std::optional<PvpStart> pvp_start_;
  std::string pvp_battle_;
  // A battle message waiting to go out, or kept for a resend: the side, or a step (`seq`, `cmd`, `hash`).
  struct PvpOut {
    bool side = false;
    std::uint32_t seq = 0;
    std::string cmd;
    std::uint64_t hash = 0;
  };
  struct PvpLink {
    std::string side;
    std::optional<std::string> opponent_side;
    std::deque<PvpStep> inbox;
    std::deque<PvpOut> queued, sent;
    // The last relay number received; the paced sends of the current second.
    std::int64_t relay = 0;
    double window = -1.0;
    int window_sent = 0;
    // After a reconnect: nothing goes out until pvp_resumed.
    bool resuming = false;
    double away_until = -1.0, dropped_at = -1.0, back_at = -1.0;
    PvpGone gone = PvpGone::None;
  };
  PvpLink pvp_;
};

// A new random player id: 32 lower-case hex digits (128 bits from the system's random generator).
std::string new_player_id();
// `id` has the player id's form.
bool valid_player_id(std::string_view id);
// The id a server other than the Master is given: the SHA-256 of the player id and the server's host (and port), cut to the player id's
// form, so a server learns nothing it could use to claim the player on another server. Empty when it cannot be made.
std::string server_player_id(std::string_view id, std::string_view url);
// A ws:// address must name this machine (localhost, 127.x.x.x or ::1); wss:// may name any host.
bool server_url_allowed(std::string_view url);
// A usable public player id from a server: 1 to 32 letters and digits (the protocol's are 16 hex digits). A pid with anything else, which
// could break the ini line that stores the mutes, is dropped with its message.
bool valid_pid(std::string_view pid);

}  // namespace oc::net
