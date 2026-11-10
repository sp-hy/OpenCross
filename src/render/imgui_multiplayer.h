#pragma once
// The multiplayer screens, all ImGui: the Esc overlay tab "Multiplayer" (the [Multiplayer] settings, the connection's status and the
// players in the room with their mute buttons), the chat panel at the bottom left (T opens its input line, Enter sends, Esc closes) and
// the name labels over the remote party leaders.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/presence.h"
#include "platform/config.h"

struct ImFont;

namespace oc::net {
class MultiplayerClient;
}

namespace oc::render {

class MultiplayerUi {
 public:
  // The tab body. `client` is null until multiplayer was first enabled; `default_name` is the name sent while Name is empty; `allowed` is
  // false in runs that never connect. `font` draws the players' names. Returns true when the settings changed.
  bool draw_tab(platform::MultiplayerSettings& settings, const net::MultiplayerClient* client, const std::string& default_name, bool allowed,
                ImFont* font);

  bool chat_open() const { return chat_open_; }
  void open_chat();
  void close_chat() { chat_open_ = false; }
  // The chat panel of this frame (the input line while open, else the recent lines as [Multiplayer] Chat says). `now` is the client's clock.
  void draw_chat(net::MultiplayerClient& client, const platform::MultiplayerSettings& settings, double now, ImFont* font);
  // The remote leaders' labels: name, Star Level and an icon for a player in a menu or an event, behind every ImGui window.
  void draw_labels(const std::vector<net::RemoteLabel>& labels, const net::MultiplayerClient& client, ImFont* font) const;
  // Speech bubbles: each player's newest room chat line for a few seconds after it arrives, over their label point (`own` = the local
  // leader's, for the player's own lines), behind every ImGui window. `names_shown`: the name labels sit at the remote points.
  void draw_bubbles(const std::vector<net::RemoteLabel>& labels, const std::optional<net::RemoteLabel>& own, const net::MultiplayerClient& client,
                    double now, bool names_shown, ImFont* font) const;

  // An ImGui text field has the keyboard: game keys must not act on what is typed.
  static bool typing();

 private:
  // A text setting edited in `buffer` and stored when the field is left.
  struct TextField {
    char buffer[512] = {};
    bool active = false;
  };
  bool edit_text(const char* id, const char* hint, std::string& value, TextField& field);

  bool chat_open_ = false;
  bool focus_input_ = false;
  bool global_ = false;
  char input_[1024] = {};
  // The chat serial the open panel last scrolled to.
  std::uint64_t scrolled_serial_ = 0;
  TextField server_url_, name_;
};

}  // namespace oc::render
