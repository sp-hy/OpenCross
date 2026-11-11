// Multiplayer presence, chat and challenges in the main loop: the settings tab, the connection following [Multiplayer] Enabled / Server, the
// local player's state fed to the client every frame (Battle while a battle runs, Menu under a menu, a shop or the pause screen, otherwise
// what the field reports), the remote parties handed to the field, the chat panel, name labels, player menu and challenge popups over the
// frame, and the versus screen and side exchange that lead to a versus battle over the battle link. Runs that never connect (--config, OC_QUIET, [Debug] Script) leave the client
// unmade, so the game runs exactly as without multiplayer.
#include "app/app.h"

#include <algorithm>
#include <map>
#include <set>

#include "battle/versus.h"
#include "core/log.h"
#include "core/version.h"
#include "data/tables.h"
#include "game/battle/versus_net_link.h"
#include "game/game_session.h"
#include "platform/keymap.h"
#include "platform/paths.h"
#include "platform/updater.h"
#include "render/texture.h"
#include "ui/menu/menu_common.h"

namespace oc::app {

void App::init_multiplayer() {
  mp_allowed = steam_allowed && config.debug_script.empty();
  mp_ui.set_card_source([this] { return multiplayer_card(); });
  // The challenge popup's and the versus screen's portraits: the face set the menus show (HD art: [Game] Portraits; original art: 1999).
  mp_ui.set_face_source(ctx, [this](int char_id, render::DecodedImage& image) {
    const std::string set = platform::art_is_hd(config.art_mode) ? config.portraits : std::string("1999");
    const std::map<int, std::string> faces = ui::menudraw::list_face_files(files, set);
    const auto found = faces.find(char_id);
    if (found == faces.end()) return false;
    const std::optional<Bytes> bytes = files.read(found->second);
    return bytes && render::decode_image(*bytes, image);
  });
  ui.add_tab("Multiplayer", [this] {
    if (mp_ui.draw_tab(config.multiplayer, mp_client.get(), multiplayer_default_name(), mp_allowed, multiplayer_free(), ui.chat_font()))
      ui.mark_changed();
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

// While the chat is typed in, the game gets no keys (Esc clears a private target, else closes the chat); a text field of the settings
// window keeps every key but Esc from the game. Esc closes an open player menu first. Y / N answer an incoming challenge while its popup
// shows, and on a pad the d-pad picks Accept or Decline, the confirm button presses the pick and the cancel button declines (those
// buttons do not reach the game meanwhile). The chat key opens the chat in play.
void App::multiplayer_input() {
  if (mp_ui.take_close_options()) ui.options_open = false;
  if (mp_ui.player_menu_open() && input.key_pressed[SDL_SCANCODE_ESCAPE]) {
    mp_ui.close_player_menu();
    input.key_pressed[SDL_SCANCODE_ESCAPE] = false;
  }
  if (mp_ui.chat_open()) {
    const bool escape = input.key_pressed[SDL_SCANCODE_ESCAPE];
    if (escape && mp_ui.pm_target()) mp_ui.clear_pm();
    else if (escape || ui.options_open || !(mp_client && mp_client->started())) mp_ui.close_chat();
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
  if (mp_client && mp_client->incoming() && !ui.options_open) {
    const auto pad = [&](SDL_GamepadButton button) { return input.pad_connected && input.pad_pressed[button]; };
    if (input.key_pressed[SDL_SCANCODE_Y]) mp_ui.accept(*mp_client);
    else if (input.key_pressed[SDL_SCANCODE_N] || pad(SDL_GAMEPAD_BUTTON_EAST)) mp_ui.decline(*mp_client);
    else if (pad(SDL_GAMEPAD_BUTTON_SOUTH)) mp_ui.press_choice(*mp_client);
    else if (pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) mp_ui.toggle_choice();
    for (const SDL_Scancode key : {SDL_SCANCODE_Y, SDL_SCANCODE_N}) input.key_down[key] = input.key_pressed[key] = false;
    for (const SDL_GamepadButton button : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT})
      input.pad_down[button] = input.pad_pressed[button] = false;
  }
  if (!mp_client || !mp_client->started() || ui.options_open || routing || replaying || !input.key_pressed[platform::kChatKey]) return;
  mp_ui.open_chat();
  input.key_down[platform::kChatKey] = input.key_pressed[platform::kChatKey] = false;
}

bool App::game_screen_up() const {
  return (menu_ok && !menu.closed()) || shop_open || (pause_ok && pause_screen.active()) || (key_popup_ok && key_popup.active()) ||
         (party_ok && party_screen.active()) || (monster_picker_ok && monster_picker.active()) || (flow_ready && flow.owns_screen());
}

// The local player as the server should see them: the field's report, the room blanked outside field play, the state raised to Battle or
// Menu by what the App runs over the field, and Typing while the chat's input line is open in free play.
net::LocalPresence App::multiplayer_local() const {
  net::LocalPresence local;
  const bool in_room = world_ok && field_visible() && !movie_open && !(flow_ready && flow.scene_mode());
  if (!in_room) return local;
  local = world.local_presence();
  if (battle_in_field) local.pose.state = net::PresenceState::Battle;
  else if (game_screen_up() || ui.options_open) local.pose.state = net::PresenceState::Menu;
  else if (mp_ui.chat_open() && local.pose.state == net::PresenceState::Field) local.pose.state = net::PresenceState::Typing;
  return local;
}

bool App::multiplayer_free() const {
  const bool in_room = world_ok && field_visible() && !movie_open && !(flow_ready && flow.scene_mode());
  return in_room && !battle_in_field && !game_screen_up() && world.local_presence().pose.state == net::PresenceState::Field;
}

std::optional<net::PartyCard> App::multiplayer_card() {
  if (!world_ok) return std::nullopt;
  const ui::GameState& state = flow_ready ? flow.game_state() : world.game_state();
  return game::party_card(state, world.party_chars(), char_names());
}

// The pvp_start: this game's side as it stands now goes to the other game, and the versus screen shows while the other side comes.
void App::begin_pvp(net::PvpStart start) {
  std::optional<battle::VersusSide> own = multiplayer_free() ? local_versus_side() : std::nullopt;
  mp_own_side = own ? battle::encode_versus_side(*own) : std::string();
  if (!mp_own_side.empty() && mp_client->send_pvp_side(mp_own_side)) {
    log::info("multiplayer: side sent for battle {} ({} bytes)", start.battle, mp_own_side.size());
    mp_ui.start_versus(std::move(start), multiplayer_clock());
    // The battle's theme starts with the versus screen and plays on into the battle.
    world.start_battle_music(kVersusBattleFlags);
    return;
  }
  ui.toast("The battle could not start", 3.0);
  mp_client->end_pvp(net::PvpResult::Aborted);
}

// A server's versus battle: this game's side from `first`, both sides as the games exchanged them (this game's own read back from the
// bytes it sent, so both games build from the same data), the battle from the shared seed, the other side's commands over the battle
// link; its result is reported with pvp_end.
void App::start_pvp_battle(const net::PvpStart& start) {
  log::info("multiplayer: pvp battle {} against {} (seed {}, {} first, opening stamina {})", start.battle, start.opponent, start.seed, start.first,
            start.opening_stamina);
  if (!mp_client || mp_client->pvp_battle() != start.battle) {
    world.cancel_battle_music();
    ui.toast("The battle could not start", 3.0);
    return;
  }
  if (mp_client->pvp_gone() != net::PvpGone::None) {
    world.cancel_battle_music();
    ui.toast("The battle could not start", 3.0);
    mp_client->end_pvp(net::PvpResult::Aborted);
    return;
  }
  std::optional<battle::VersusSide> own = battle::decode_versus_side(mp_own_side);
  const std::optional<std::string>& sent = mp_client->pvp_opponent_side();
  std::optional<battle::VersusSide> other = sent ? battle::decode_versus_side(*sent) : std::nullopt;
  if (sent && !other) log::warn("multiplayer: the other side of battle {} cannot be read ({} bytes)", start.battle, sent->size());
  if (std::string why; other && !(tables_ok && battle::legal_versus_side(tables, *other, &why))) {
    log::warn("multiplayer: the other side of battle {} is refused: {}", start.battle, why);
    other.reset();
  }
  game::BattleView::VersusSetup setup;
  setup.seed = start.seed;
  setup.local_side = start.first == mp_client->pid() ? 0 : 1;
  setup.opening_stamina = battle::cap_stamina(std::max(0, start.opening_stamina));
  setup.battlefield = tables_ok ? battle::versus_battlefield(tables, start.seed) : -1;
  setup.opponent = start.name;
  const int local_side = setup.local_side;
  bool started = false;
  if (own && other && multiplayer_free()) {
    setup.side_a = local_side == 0 ? std::move(*own) : std::move(*other);
    setup.side_b = local_side == 0 ? std::move(*other) : std::move(*own);
    started = start_versus_battle(std::move(setup), std::make_unique<game::NetworkVersusLink>(*mp_client, local_side), true);
  }
  if (started) return;
  world.cancel_battle_music();
  ui.toast("The battle could not start", 3.0);
  mp_client->end_pvp(net::PvpResult::Aborted);
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
  if (mp_version.empty()) mp_version = std::string(OC_VERSION_STRING) + "+" + platform::exe_build_id();
  if (!mp_battle_data && tables_ok) mp_battle_data = data::battle_data_fingerprint(tables);
  if (mp_battle_data) mp_client->set_battle_data(*mp_battle_data);
  const std::string player_id = master ? settings.player_id : net::server_player_id(settings.player_id, settings.url());
  mp_client->start(settings.url(), {player_id, name, local.star, mp_version, master ? settings.admin_token : std::string(), settings.challenges});
  mp_client->set_profile(name, local.star);
  mp_client->set_challenges(settings.challenges);
  mp_client->set_muted(std::set<std::string>(settings.muted.begin(), settings.muted.end()));
  mp_client->set_local(local);
  mp_client->update(multiplayer_clock());
  if (std::optional<net::PvpStart> start = mp_client->take_pvp_start()) begin_pvp(std::move(*start));
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
  platform::MultiplayerSettings& settings = config.multiplayer;
  const double now = multiplayer_clock();
  if (mp_labels_shown && world_ok) {
    const std::vector<net::RemoteLabel> labels = world.remote_labels();
    const bool names = settings.show_players && settings.show_labels;
    if (names) mp_ui.draw_labels(labels, *mp_client, ui.chat_font());
    if (settings.chat_bubbles) mp_ui.draw_bubbles(labels, world.local_label(), *mp_client, now, names, ui.chat_font());
  }
  if (!ui.options_open) {
    mp_ui.draw_chat(*mp_client, settings, now, ui.chat_font());
    if (mp_ui.draw_player_menu(*mp_client, settings, multiplayer_free(), ui.chat_font())) save_config();
    mp_ui.draw_challenges(*mp_client, now, ui.chat_font());
  }
  if (std::optional<net::PvpStart> start = mp_ui.draw_versus(*mp_client, now, ui.chat_font())) start_pvp_battle(*start);
}

void App::versus_banner_overlay() {
  if (!battle_ready || !(battle_ok || battle_in_field)) return;
  const std::optional<game::BattleView::VersusBanner> banner = battle.versus_banner();
  if (!banner) return;
  ui.request_chat_font();
  render::MultiplayerUi::draw_turn_banner(banner->text, banner->own, ui.chat_font());
}

}  // namespace oc::app
