// Multiplayer presence and chat in the main loop: the settings tab, the connection following [Multiplayer] Enabled / Server, the local
// player's state fed to the client every frame (Battle while a battle runs, Menu under a menu, a shop or the pause screen, otherwise what
// the field reports), the remote parties handed to the field, and the chat panel and name labels over the frame. Runs that never
// connect (--config, OC_QUIET, [Debug] Script) leave the client unmade, so the game runs exactly as without multiplayer.
#include "app/app.h"

#include <set>

#include "core/version.h"
#include "game/game_session.h"
#include "platform/keymap.h"
#include "platform/paths.h"

namespace oc::app {

void App::init_multiplayer() {
  mp_allowed = steam_allowed && config.debug_script.empty();
  ui.add_tab("Multiplayer", [this] {
    if (mp_ui.draw_tab(config.multiplayer, mp_client.get(), multiplayer_default_name(), mp_allowed, ui.chat_font())) ui.mark_changed();
  }, 50);
}

std::string App::multiplayer_default_name() {
  std::string persona = steam.connected() ? steam.persona_name() : std::string();
  if (persona.empty()) {
    if (!mp_stored_persona) mp_stored_persona = platform::current_steam_persona();
    persona = *mp_stored_persona;
  }
  return persona.empty() ? "Player" : persona;
}

double App::multiplayer_clock() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - mp_epoch).count(); }

// While the chat is typed in, the game gets no keys (Esc closes the chat); a text field of the settings window keeps every key but Esc
// from the game. The chat key opens the chat in play.
void App::multiplayer_input() {
  if (mp_ui.chat_open()) {
    if (input.key_pressed[SDL_SCANCODE_ESCAPE] || ui.options_open || !(mp_client && mp_client->started())) mp_ui.close_chat();
    input.key_down.fill(false);
    input.key_pressed.fill(false);
    input.pad_down.fill(false);
    input.pad_pressed.fill(false);
    input.pad_axis.fill(0.0f);
    return;
  }
  if (ui.options_open && render::MultiplayerUi::typing()) {
    const bool escape = input.key_pressed[SDL_SCANCODE_ESCAPE];
    input.key_down.fill(false);
    input.key_pressed.fill(false);
    input.key_pressed[SDL_SCANCODE_ESCAPE] = escape;
    return;
  }
  if (!mp_client || !mp_client->started() || ui.options_open || routing || replaying || !input.key_pressed[platform::kChatKey]) return;
  mp_ui.open_chat();
  input.key_down[platform::kChatKey] = input.key_pressed[platform::kChatKey] = false;
}

// The local player as the server should see them: the field's report, the room blanked outside field play, the state raised to Battle or
// Menu by what the App runs over the field, and Typing while the chat's input line is open in free play.
net::LocalPresence App::multiplayer_local() const {
  net::LocalPresence local;
  const bool in_room = world_ok && field_visible() && !movie_open && !(flow_ready && flow.scene_mode());
  if (!in_room) return local;
  local = world.local_presence();
  const bool menu_up = (menu_ok && !menu.closed()) || shop_open || (pause_ok && pause_screen.active()) || (key_popup_ok && key_popup.active()) ||
                       (party_ok && party_screen.active()) || (monster_picker_ok && monster_picker.active()) || (flow_ready && flow.owns_screen()) ||
                       ui.options_open;
  if (battle_in_field) local.pose.state = net::PresenceState::Battle;
  else if (menu_up) local.pose.state = net::PresenceState::Menu;
  else if (mp_ui.chat_open() && local.pose.state == net::PresenceState::Field) local.pose.state = net::PresenceState::Typing;
  return local;
}

void App::multiplayer_frame() {
  platform::MultiplayerSettings& settings = config.multiplayer;
  mp_labels_shown = false;
  if (mp_allowed && settings.enabled && !net::valid_player_id(settings.player_id)) {
    settings.player_id = net::new_player_id();
    save_config();
  }
  const bool wanted = mp_allowed && settings.enabled && !settings.url().empty();
  if (!wanted) {
    if (mp_client && mp_client->started()) mp_client->stop();
    if (mp_parties_given && world_ok) world.set_remote_parties({}, {});
    mp_parties_given = false;
    return;
  }
  if (!mp_client) mp_client = std::make_unique<net::MultiplayerClient>(net::make_websocket_transport(std::string("OpenCross/") + OC_VERSION_STRING));
  ui.request_chat_font();
  const std::string name = settings.name.empty() ? multiplayer_default_name() : settings.name;
  net::LocalPresence local = multiplayer_local();
  if (world_ok) local.star = game::session_star(world.game_state());
  const bool master = settings.server == platform::MultiplayerServer::Master;
  mp_client->start(settings.url(), {settings.player_id, name, local.star, OC_VERSION_STRING, master ? settings.admin_token : std::string()});
  mp_client->set_profile(name, local.star);
  mp_client->set_muted(std::set<std::string>(settings.muted.begin(), settings.muted.end()));
  mp_client->set_local(local);
  mp_client->update(multiplayer_clock());
  if (!world_ok) return;
  std::vector<net::RemoteParty> parties;
  for (const net::RemoteParty& party : mp_client->parties())
    if (!(settings.mute_hides_party && mp_client->muted(party.pid))) parties.push_back(party);
  world.set_remote_parties(std::move(parties), {settings.show_players, settings.max_parties, settings.translucent, true});
  mp_parties_given = true;
  const net::PresenceState state = local.pose.state;
  mp_labels_shown = !local.room.empty() && (state == net::PresenceState::Field || state == net::PresenceState::Event || state == net::PresenceState::Typing);
}

void App::multiplayer_overlay() {
  if (!mp_client || !mp_client->started()) return;
  const platform::MultiplayerSettings& settings = config.multiplayer;
  if (mp_labels_shown && world_ok) {
    const std::vector<net::RemoteLabel> labels = world.remote_labels();
    const bool names = settings.show_players && settings.show_labels;
    if (names) mp_ui.draw_labels(labels, *mp_client, ui.chat_font());
    if (settings.chat_bubbles) mp_ui.draw_bubbles(labels, world.local_label(), *mp_client, multiplayer_clock(), names, ui.chat_font());
  }
  if (!ui.options_open) mp_ui.draw_chat(*mp_client, settings, multiplayer_clock(), ui.chat_font());
}

}  // namespace oc::app
