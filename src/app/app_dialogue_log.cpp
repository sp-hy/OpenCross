// The dialogue log ([Game] OpenCrossMenu): the file beside the OpenCross saves (a --config run keeps the log in memory only), the field's
// and the battle's hooks that add every finished message window, a divider for every loaded game, the log window over the field (the L
// key or the pad's RB + Back, mid-scene too; the field stands still under it), the replay of a logged line's voice files and the
// window's Clear log.
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "app/app.h"
#include "audio/voice.h"
#include "core/log.h"
#include "platform/keymap.h"

namespace oc::app {

void App::init_dialogue_log() {
  dialogue_log.set_writable(own_ini);
  if (oc_saves.dir().empty()) return;
  dialogue_log.set_path(oc_saves.dir() / save::DialogueLog::kFileName);
  dialogue_log.load();
}

// Every frame: the hooks (always on: the log is kept whether or not the OpenCross menu shows it), the window's style, a Clear log the window was answered Yes to, and the replay's next
// voice file once the previous one has ended.
void App::dialogue_log_frame() {
  if (!dialogue_log_hooked) {
    dialogue_log_hooked = true;
    const std::function<void(save::DialogueEntry)> hook = [this](save::DialogueEntry entry) { add_dialogue_line(std::move(entry)); };
    if (world_ok) world.set_dialogue_log(hook);
    if (battle_ready) battle.set_story_log(hook);
  }
  if (!config.opencross_menu && dialogue_log_open) close_dialogue_log();
  if (!dialogue_log_view_ok) return;
  import::Lang text_lang = import::Lang::En;
  import::parse_lang(config.language, text_lang);
  const ui::GameState& state = world_ok ? world.game_state() : flow.game_state();
  dialogue_log_view.set_style(platform::art_is_hd(config.art_mode) ? ui::ArtMode::Hd : ui::ArtMode::Original,
                              state.option(ui::opt::kCustomWindow) ? state.skin : 0, config.portraits, text_lang);
  dialogue_log_view.set_key_caps(dialogue_log_keys);
  if (dialogue_log_view.take_clear()) {
    dialogue_log.clear();
    dialogue_log_replay.clear();
    log::info("dialogue log: cleared");
  }
  audio::VoicePlayer* voices = audio::VoicePlayer::instance();
  if (std::vector<std::string> fresh = dialogue_log_view.take_replay(); !fresh.empty()) {
    dialogue_log_replay = std::move(fresh);
    if (voices) voices->stop();
  }
  if (!dialogue_log_replay.empty() && voices && !voices->active()) {
    log::info("dialogue log: replay voices/{}", dialogue_log_replay.front());
    voices->replay(dialogue_log_replay.front(), &dialogue_log_view);
    dialogue_log_replay.erase(dialogue_log_replay.begin());
  }
}

void App::add_dialogue_line(save::DialogueEntry entry) {
  std::string shown = entry.text.substr(0, 80);
  std::replace(shown.begin(), shown.end(), '\n', ' ');
  log::info("dialogue log: {} | {}{}{}", entry.speaker.empty() ? "-" : entry.speaker, shown, entry.choice.empty() ? "" : " | picked " + entry.choice,
            entry.voices.empty() ? "" : " | " + std::to_string(entry.voices.size()) + " voice file(s)");
  dialogue_log.append(std::move(entry));
}

// A loaded game (Continue, Load): its area (the first line of the save's location caption without a closing dash) and play time.
void App::note_dialogue_load() {
  if (!flow_ready) return;
  save::DialogueEntry divider;
  divider.kind = save::DialogueEntry::Kind::Divider;
  divider.area = flow.game_state().location_name(name_tables, lang);
  // A space or a dash (hyphen, em dash, en dash, full-width hyphen) that leads on to the caption's second line.
  const auto trailing = [](const std::string& text) -> size_t {
    for (const std::string_view tail : {std::string_view(" "), std::string_view("-"), std::string_view("\xE2\x80\x94"), std::string_view("\xE2\x80\x93"),
                                        std::string_view("\xEF\xBC\x8D")})
      if (text.ends_with(tail)) return tail.size();
    return 0;
  };
  while (const size_t cut = trailing(divider.area)) divider.area.erase(divider.area.size() - cut);
  divider.play_seconds = flow.game_state().slot.playtime_frames() / 60;
  log::info("dialogue log: loaded {} at {} s of play", divider.area, divider.play_seconds);
  dialogue_log.append(std::move(divider));
}

bool App::dialogue_log_allowed() const {
  return dialogue_log_view_ok && field_visible() && !battle_ok && !battle_in_field && !movie_open && !(menu_ok && !menu.closed()) && !shop_open &&
         !(key_popup_ok && key_popup.active()) && !(party_ok && party_screen.active()) && !(monster_picker_ok && monster_picker.active()) &&
         !(flow_ready && (flow.owns_screen() || flow.scene_mode() || flow.fading_out())) && !ui.options_open;
}

void App::close_dialogue_log() {
  dialogue_log_open = false;
  dialogue_log_view.close();
  dialogue_log_replay.clear();
  if (audio::VoicePlayer* voices = audio::VoicePlayer::instance()) voices->stop_if(&dialogue_log_view);
  log::info("dialogue log: closed");
}

// While open: Up / Down one entry (held: again every 4 frames after 20), Left / Right, LB / RB (L1 / R1) and PageUp / PageDown a
// screen, Home / End the ends, confirm hears the selected line's voice again, cancel, Esc (frame_hotkeys), L or the chord close it.
bool App::dialogue_log_step(platform::Input& in) {
  const bool rb = in.pad_connected && in.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER];
  const bool both = rb && in.pad_down[SDL_GAMEPAD_BUTTON_BACK];
  const bool chord = both && !dialogue_log_chord;
  dialogue_log_chord = both;
  if (std::any_of(in.pad_pressed.begin(), in.pad_pressed.end(), [](bool pressed) { return pressed; })) dialogue_log_keys = false;
  if (std::any_of(in.key_pressed.begin(), in.key_pressed.end(), [](bool pressed) { return pressed; })) dialogue_log_keys = true;
  if (!config.opencross_menu && !dialogue_log_open) return false;
  if (rb) in.pad_pressed[SDL_GAMEPAD_BUTTON_BACK] = false;
  const bool toggle = chord || (in.key_pressed[SDL_SCANCODE_L] && !render::ImGuiLayer::text_input() && !ui.options_open);
  if (!dialogue_log_open) {
    if (!toggle || !dialogue_log_allowed()) return false;
    dialogue_log_open = true;
    dialogue_log_held = 0;
    dialogue_log_view.open();
    log::info("dialogue log: opened over {} ({} entries)", world.room_name(), dialogue_log_view.entry_count());
    return true;
  }
  using platform::Action;
  const auto pressed = [&](Action action, int button) { return platform::action_pressed(in, action) || (in.pad_connected && in.pad_pressed[static_cast<size_t>(button)]); };
  const auto held = [&](Action action, int button) { return platform::action_down(in, action) || (in.pad_connected && in.pad_down[static_cast<size_t>(button)]); };
  const int dir = held(Action::Up, SDL_GAMEPAD_BUTTON_DPAD_UP) ? -1 : held(Action::Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1 : 0;
  ui::DialogueLogView::Nav nav;
  if (dir == 0) {
    dialogue_log_held = 0;
  } else {
    if (dialogue_log_held == 0 || (dialogue_log_held >= 20 && (dialogue_log_held - 20) % 4 == 0)) nav.move = dir;
    ++dialogue_log_held;
  }
  const bool page_up = pressed(Action::Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || pressed(Action::SelectLeft, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
                       in.key_pressed[SDL_SCANCODE_PAGEUP];
  const bool page_down = pressed(Action::Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (!both && pressed(Action::SelectRight, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) ||
                         in.key_pressed[SDL_SCANCODE_PAGEDOWN];
  nav.page = (page_down ? 1 : 0) - (page_up ? 1 : 0);
  nav.first = in.key_pressed[SDL_SCANCODE_HOME];
  nav.last = in.key_pressed[SDL_SCANCODE_END];
  nav.replay = confirm_edge(in);
  nav.clear = pressed(Action::Item, SDL_GAMEPAD_BUTTON_WEST);
  nav.cancel = pressed(Action::Cancel, SDL_GAMEPAD_BUTTON_EAST);
  const bool asking = dialogue_log_view.asking();
  if (const int sound = dialogue_log_view.input(nav)) ui_sfx(sound);
  if (asking) return true;
  if (nav.cancel) {
    ui_sfx(6);
    close_dialogue_log();
  } else if (toggle) {
    close_dialogue_log();
  }
  return true;
}

}  // namespace oc::app
