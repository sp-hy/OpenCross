#pragma once
// The multiplayer presence and chat client. It owns the connection's state machine: the
// hello, the reconnect back-off, the keep-alive ping, the room / party / profile messages on change, the presence sends (only while
// someone else is in the room, at most 10 a second, only on change), the table of remote parties in the local player's room, the chat
// log per scope and the player's mutes. All time comes from the `now` (seconds) passed to update(), so tests run it on a fake clock
// over a fake Transport.
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
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

enum class ChatScope { Room, Global };

struct ChatLine {
  ChatScope scope = ChatScope::Room;
  // Empty for the client's own notices (a rate-limited message).
  std::string pid;
  std::string name;
  std::string text;
  // The server's Unix ms, and the client clock (update's `now`) when the line arrived; history lines arrive as old (`fresh` false).
  std::int64_t ts = 0;
  double received = 0.0;
  bool fresh = true;
  // The server vouches that the sender holds its admin token.
  bool admin = false;
};

// What the client may say about itself.
struct ClientIdentity {
  std::string player_id;
  std::string name;
  int star = 0;
  std::string version;
  // The server's admin token ([Multiplayer] AdminToken); sent in hello only when set.
  std::string admin_token;
};

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
  };

  // Server limits from the welcome (defaults until then): chat and name lengths in code points, pres messages a second.
  struct Limits {
    int chat_len = 200;
    int name_len = 24;
    int pres_hz = 10;
  };

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

  // A chat line from the player; false when it is empty or not connected. Cut to the server's chat_len.
  bool send_chat(ChatScope scope, std::string_view text);

  State state() const { return state_; }
  // The status line: "Connected as <pid>", "Connecting...", "Could not reach the server; retrying in 8 s", the server's version message...
  std::string status_text() const;
  const std::string& pid() const { return pid_; }
  // The server accepted the admin token.
  bool admin() const { return admin_; }
  // Other players in the local player's room (from the server's `occupancy`).
  int occupancy() const { return occupancy_; }

  // The remote parties in the local player's room, in the order they arrived.
  const std::vector<RemoteParty>& parties() const { return parties_; }
  // One scope's chat, oldest first; `serial` rises with every line added to either scope.
  const std::deque<ChatLine>& chat(ChatScope scope) const { return scope == ChatScope::Room ? room_chat_ : global_chat_; }
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
  void add_notice(std::string text);
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
  LeaderPose sent_pose_;
  bool pose_sent_ = false;
  double last_pres_ = -1.0;
  std::uint32_t pres_seq_ = 0;
  int occupancy_ = 0;

  std::vector<RemoteParty> parties_;
  // The last pres seq taken from each remote party.
  std::map<std::string, std::int64_t> party_seq_;
  std::deque<ChatLine> room_chat_, global_chat_;
  // The scope of the player's last chat line: a rate-limit notice goes there, stamped with the newest server time seen.
  ChatScope last_scope_ = ChatScope::Room;
  std::int64_t last_ts_ = 0;
  std::uint64_t chat_serial_ = 0;
  std::set<std::string> muted_;
};

// A new random player id: 32 lower-case hex digits (128 bits from the system's random generator).
std::string new_player_id();
// `id` has the player id's form.
bool valid_player_id(std::string_view id);
// A usable public player id from a server: 1 to 32 letters and digits (the protocol's are 16 hex digits). A pid with anything else, which
// could break the ini line that stores the mutes, is dropped with its message.
bool valid_pid(std::string_view pid);

}  // namespace oc::net
