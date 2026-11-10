#pragma once
// The multiplayer screens, all ImGui: the Esc overlay tab "Multiplayer" (the [Multiplayer] settings, the connection's status and the
// players in the room with their Message / Challenge / Mute buttons), the chat panel at the bottom left (T opens its input line, Enter
// sends, Esc closes; room, global and private lines), the name labels over the remote party leaders, the player menu a click on a name
// opens, the challenge popups (incoming with Accept / Decline, outgoing with Cancel) and the versus screen before a battle.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "net/multiplayer_client.h"
#include "net/presence.h"
#include "platform/config.h"

struct ImFont;

namespace oc::render {

class MultiplayerUi {
 public:
  // The local party's card for a challenge or an accept; nullopt when no game is loaded.
  using CardSource = std::function<std::optional<net::PartyCard>()>;
  void set_card_source(CardSource source) { card_source_ = std::move(source); }

  // The tab body. `client` is null until multiplayer was first enabled; `default_name` is the name sent while Name is empty; `allowed` is
  // false in runs that never connect; `free`: the player could take a battle once the settings window closes. `font` draws the players'
  // names. Returns true when the settings changed.
  bool draw_tab(platform::MultiplayerSettings& settings, net::MultiplayerClient* client, const std::string& default_name, bool allowed,
                bool free, ImFont* font);
  // A tab button that needs the field (Message, Challenge) asked for the settings window to close; true once.
  bool take_close_options();

  bool chat_open() const { return chat_open_; }
  void open_chat();
  void close_chat() { chat_open_ = false; }
  // Opens the input line with a private target; clear_pm goes back to Room / Global.
  void open_pm(const std::string& pid, const std::string& name);
  bool pm_target() const { return !pm_to_.empty(); }
  void clear_pm() { pm_to_.clear(); }
  // The chat panel of this frame (the input line while open, else the recent lines as [Multiplayer] Chat says). `now` is the client's clock.
  // A click on a name in the open panel opens the player menu, on a private line's name a reply.
  void draw_chat(net::MultiplayerClient& client, const platform::MultiplayerSettings& settings, double now, ImFont* font);
  // The remote leaders' labels: name, Star Level and an icon for a player in a menu or an event, behind every ImGui window. A click on one
  // opens the player menu.
  void draw_labels(const std::vector<net::RemoteLabel>& labels, const net::MultiplayerClient& client, ImFont* font);
  // Speech bubbles: each player's newest room chat line for a few seconds after it arrives, over their label point (`own` = the local
  // leader's, for the player's own lines), behind every ImGui window. `names_shown`: the name labels sit at the remote points.
  void draw_bubbles(const std::vector<net::RemoteLabel>& labels, const std::optional<net::RemoteLabel>& own, const net::MultiplayerClient& client,
                    double now, bool names_shown, ImFont* font) const;

  // The player menu: the player's name and Star Level, Message, Challenge to battle (greyed unless both sides take challenges, both are
  // free and nothing is pending) and Mute / Unmute. `free`: the local player is free in a field room. Returns true when the mutes changed.
  void open_player_menu(const std::string& pid, const std::string& name, int star);
  bool player_menu_open() const { return menu_.has_value(); }
  void close_player_menu() { menu_.reset(); }
  bool draw_player_menu(net::MultiplayerClient& client, platform::MultiplayerSettings& settings, bool free, ImFont* font);

  // The incoming challenge's popup (the card, the time left, Accept / Decline) and the outgoing challenge's waiting popup (Cancel).
  void draw_challenges(net::MultiplayerClient& client, double now, ImFont* font);
  // Y / N on the incoming popup.
  void accept(net::MultiplayerClient& client);
  void decline(net::MultiplayerClient& client) { client.decline_challenge(); }
  // The pad on the incoming popup: the d-pad switches the chosen button (Decline when the popup opens), the confirm button presses it.
  void toggle_choice() { accept_chosen_ = !accept_chosen_; }
  void press_choice(net::MultiplayerClient& client) {
    if (accept_chosen_) accept(client);
    else decline(client);
  }

  // The versus screen: both cards and who moves first, for kVersusSeconds from `now` and then until the other game's side has come (at
  // most kSideWait longer); draw_versus hands the start back once, when it ends or the battle is over before it started.
  static constexpr double kVersusSeconds = 3.0;
  static constexpr double kSideWait = 15.0;
  void start_versus(net::PvpStart start, double now);
  std::optional<net::PvpStart> draw_versus(const net::MultiplayerClient& client, double now, ImFont* font);

  // An ImGui text field has the keyboard: game keys must not act on what is typed.
  static bool typing();

 private:
  // A text setting edited in `buffer` and stored when the field is left.
  struct TextField {
    char buffer[512] = {};
    bool active = false;
  };
  struct PlayerMenu {
    std::string pid;
    std::string name;
    int star = 0;
    float x = 0, y = 0;
    bool fresh = true;
  };
  bool edit_text(const char* id, const char* hint, std::string& value, TextField& field);
  // Why the player menu cannot challenge `pid` now; empty when it can.
  std::string challenge_block(const net::MultiplayerClient& client, const platform::MultiplayerSettings& settings, const std::string& pid,
                              bool free) const;
  bool send_challenge(net::MultiplayerClient& client, const std::string& pid, const std::string& name);

  CardSource card_source_;
  bool chat_open_ = false;
  // [Multiplayer] HideGlobalChat as of the last chat panel: no Global target, Tab does not switch to it.
  bool hide_global_ = false;
  bool accept_chosen_ = false;
  bool focus_input_ = false;
  bool global_ = false;
  // The private target of the input line (empty: Room / Global).
  std::string pm_to_, pm_name_;
  char input_[1024] = {};
  // The chat serial the open panel last scrolled to.
  std::uint64_t scrolled_serial_ = 0;
  TextField server_url_, name_;
  std::optional<PlayerMenu> menu_;
  bool close_options_ = false;
  std::optional<net::PvpStart> versus_;
  double versus_end_ = 0.0;
};

}  // namespace oc::render
