// The main loop: a fixed-step 60 Hz simulation with interpolated drawing. Every iteration reads the hotkeys and the live [Video]
// settings, runs the Debug tab's requests, advances the game in its current mode (route / replay / boot logo / battle / sweep /
// field), composes the frame and applies what the Esc options window changed.
#include "app/app.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/effects/gpu_effects.h"
#include "game/free_cam.h"
#include "platform/keymap.h"
#include "render/camera.h"

namespace oc::app {

int App::run() {
  frametime_log = std::getenv("OC_FRAMETIME") != nullptr;
  ft_prev = std::chrono::steady_clock::now();
  while (window.pump(input) && !quit_requested && !restart_for_update) {
    log_frametime();
    frame_hotkeys();
    frame_settings();
    debug_tools_frame();
    frame_dt = timer.tick();
    // [Game] FastMode / SlowMode: more or fewer 60 Hz ticks per frame.
    if (!movie_open && !routing && !replaying) {
      const bool in_battle = battle_ok || battle_in_field, locked = speed_locked();
      const float factor = game::speed_factor(config.fast_mode, config.slow_mode, config.fast_rate, config.slow_rate, in_battle, locked);
      const bool field_slow = config.slow_mode && !config.fast_mode && !in_battle && !locked;
      frame_dt *= slow_field_cycle.apply(frame_dt, field_slow, factor);
    }
    step = stepper.advance(frame_dt);
    // The art mode cycles SD -> HD -> Modded -> SD (also in the options window).
    if (input.key_pressed[SDL_SCANCODE_F6]) {
      config.art_mode = config.art_mode == platform::ArtMode::Original ? platform::ArtMode::Hd
                        : config.art_mode == platform::ArtMode::Hd     ? platform::ArtMode::Modded
                                                                       : platform::ArtMode::Original;
      config.save(config_file);
      apply_art_mode();
      ui.toast(config.art_mode == platform::ArtMode::Modded ? "Art: Modded" : config.art_mode == platform::ArtMode::Hd ? "Art: HD" : "Art: Original");
    }
    if (!simulate()) break;

    ++frame_count;
    if (screenshot_pending && (battle_ok ? frame_count >= screenshot_tick : (world_ok && world.tick_count() >= screenshot_tick))) {
      ctx.request_screenshot(exe / screenshot_name);
      screenshot_pending = false;
      exit_when_idle = config.debug_exit_after_screenshot;
    }
    // The field background is composed over a BLACK framebuffer (PS1: semi-transparent tiles blend with whatever is behind them, and the
    // base layer of many rooms is itself semi-transparent: a navy clear colour would wash every dark area). The battle buffer is cleared
    // black as well, also for [Debug] Battle.
    const bool boot_logo = flow_ready && flow.boot_logo_active();
    const bool black_clear = field_visible() || battle_ok || boot_logo;
    if (!movie_open) ctx.set_clear_color(black_clear ? 0.0f : 0.08f, black_clear ? 0.0f : 0.09f, black_clear ? 0.0f : 0.14f);
    render::Frame frame;
    if (!ctx.begin_frame(frame)) {
      // Minimised or swapchain being rebuilt.
      SDL_Delay(10);
      continue;
    }
    draw_frame(frame);

    ui.new_frame();
    // The room's entity / script tables stay while a battle, menu, movie or flow screen covers the field.
    debug_field = field_visible();
    apply_options_change(ui.options_window(config, config_file));
    ui.render(frame);
    ctx.end_frame();
    if (!routing && !replaying && !sweeping) limiter.wait(config.frame_cap);

    if (exit_when_idle && ++exit_countdown > 3) break;
    if (route_finished) break;
  }

  if (world_ok && (routing || replaying) && world.view().loaded()) {
    float fx = 0, fz = 0;
    world.player_xz(fx, fz);
    log::info("final state: room {} player {:.0f},{:.0f} field ticks {} BATTLE_RESULT {} scenario {}", world.room_name(), fx, fz, world.tick_count(),
              world.battle_result(), world.vm().var(0));
  }
  if (movie_open) movie_player.close();
  mixer.close_device();
  log::info("exiting after {} simulation ticks, {} validation messages", world_ok ? world.tick_count() : 0,
            render::GpuContext::validation_message_count());
  ctx.wait_idle();
  if (restart_for_update) platform::Updater::restart();
  return exit_code;
}

// OC_FRAMETIME logs the wall time of main-loop iterations over 20 ms while a field -> battle hand-over is running; a value starting
// with 2 logs every iteration.
void App::log_frametime() {
  if (!frametime_log) return;
  const auto now = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(now - ft_prev).count();
  ft_prev = now;
  if (world_ok && (world.battle_active() || battle_in_field) && (ms > 20.0 || std::getenv("OC_FRAMETIME")[0] == '2'))
    log::info("frametime: frame {} tick {} {:.1f} ms{}", ++ft_frame, world.tick_count(), ms, ms > 20.0 ? "  <-- SLOW" : "");
}

void App::frame_hotkeys() {
  // The settings window: Esc, or the pad's Start held for kStartHoldMs (it opens while still held); a Start press closes it. A shorter
  // Start press is RDE's Start: it opens the pause screen on its release (start_tap), so holding never shows the pause first (the keyboard's
  // Start, Space, opens it on the press). The pad's Back / View and the K key are the PS1 Select: it rotates the party in the field and, held,
  // hides the battle overlays during a cast.
  constexpr u64 kStartHoldMs = 500;
  const bool start_down = input.pad_connected && input.pad_down[SDL_GAMEPAD_BUTTON_START];
  const bool start_pressed = input.pad_connected && input.pad_pressed[SDL_GAMEPAD_BUTTON_START];
  const u64 now_ms = SDL_GetTicks();
  bool settings_key = input.key_pressed[SDL_SCANCODE_ESCAPE];
  start_tap = false;
  if (start_pressed) {
    settings_key = settings_key || ui.options_open;
    start_held = !ui.options_open;
    start_down_ms = now_ms;
  } else if (start_held && !start_down) {
    start_held = false;
    start_tap = input.pad_connected;
  } else if (start_held && now_ms - start_down_ms >= kStartHoldMs) {
    start_held = false;
    settings_key = !ui.options_open;
  }
  if (settings_key && !routing && !replaying) ui.options_open = !ui.options_open;
  const bool select_down = input.key_down[platform::kSelectKey] || (input.pad_connected && input.pad_down[SDL_GAMEPAD_BUTTON_BACK]);
  battle.set_select_held(select_down && !ui.options_open && !routing && !replaying);
  settings_pad_filter();
  if (input.key_pressed[SDL_SCANCODE_F8]) ui.toggle("Debug");
  // A mode switch / monitor move rebuilds the swapchain at the new size.
  if (window.take_resized()) ctx.request_resize();
  // [Video] Widescreen on / off, live and saved (Graphics tab too).
  if (input.key_pressed[SDL_SCANCODE_F7] && !routing && !replaying) {
    config.widescreen = !config.widescreen;
    config.save(config_file);
    ui.toast(config.widescreen ? "Widescreen: On" : "Widescreen: Off");
  }
  rde_toggle_keys();
}

// The settings window takes the pad (ImGui reads it from SDL directly): while it is open the game sees no pad input, and a button or
// axis still held when it closes stays hidden from the game until it is let go.
void App::settings_pad_filter() {
  for (size_t i = 0; i < settings_held_buttons.size(); ++i) {
    settings_held_buttons[i] = input.pad_down[i] && (ui.options_open || settings_held_buttons[i]);
    if (ui.options_open || settings_held_buttons[i]) input.pad_down[i] = input.pad_pressed[i] = false;
  }
  for (size_t i = 0; i < settings_held_axes.size(); ++i) {
    settings_held_axes[i] = std::fabs(input.pad_axis[i]) >= 0.25f && (ui.options_open || settings_held_axes[i]);
    if (ui.options_open || settings_held_axes[i]) input.pad_axis[i] = 0.0f;
  }
}

bool App::speed_locked() const {
  if (battle_ok || battle_in_field) return battle_ready && battle.scene_id() == game::kSpeedLockedMap;
  return world_ok && world.map_id() == game::kSpeedLockedMap;
}

// RDE's play-time toggles ([Game] SlowMode / FastMode, [Cheats] NoEncounters / BattleBoost / AutoBattle) from its keys and pad
// buttons. What is on shows as RDE's icons (draw_frame).
void App::rde_toggle_keys() {
  if (routing || replaying) return;
  auto held = [&](platform::Action action) {
    // The speed keys must not also count as an "any button" for the title / flow screens.
    for (int key : platform::action_keys(action))
      if (key > 0) input.key_pressed[static_cast<size_t>(key)] = false;
    return platform::action_down(input, action);
  };
  game::HotkeyLevels levels;
  levels.slow_key = held(platform::Action::SlowSpeed);
  levels.fast_key = held(platform::Action::FastSpeed);
  // The Status screen's pad camera reads them as L2 / R2.
  const bool menu_camera = menu_ok && menu.camera_takes_triggers();
  if (menu_camera) levels.slow_key = levels.fast_key = false;
  levels.encounters_key = input.key_down[SDL_SCANCODE_F3];
  levels.boost_key = input.key_down[SDL_SCANCODE_F4];
  levels.auto_key = input.key_down[SDL_SCANCODE_F5];
  // The Debug free camera flies on the triggers and stick clicks. A trigger counts as pressed from half way.
  if (input.pad_connected && !game::free_cam().on()) {
    levels.lt = !menu_camera && input.pad_axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] >= 0.5f;
    levels.rt = !menu_camera && input.pad_axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] >= 0.5f;
    levels.l3 = input.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK];
    levels.r3 = input.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
  }
  platform::Cheats& cheats = config.cheats;
  game::Toggles toggles{config.slow_mode, config.fast_mode, cheats.no_encounters, cheats.battle_boost, cheats.auto_battle};
  const game::Toggles before = toggles;
  rde_hotkeys.poll(levels, speed_locked(), toggles);
  if (toggles == before) return;
  config.slow_mode = toggles.slow, config.fast_mode = toggles.fast;
  cheats.no_encounters = toggles.encounters, cheats.battle_boost = toggles.boost, cheats.auto_battle = toggles.auto_battle;
  config.save(config_file);
}

// The live [Video] settings read every frame, so the overlay's changes apply at once.
void App::frame_settings() {
  // Room edge fade / texture filters of the field view (Graphics tab).
  if (world_ok) {
    world.view().bg_linear = config.filter_rooms == platform::TexFilter::Bilinear;
    world.view().edge_fade = config.widescreen ? static_cast<float>(config.wide_edge_fade) : 0.0f;
  }
  effects::GpuEffects::set_linear(config.filter_effects == platform::TexFilter::Bilinear);
  // [Video] Widescreen: the extra layer columns of the field / world-map / battle picture, for this frame's
  // ticks (the widened scroll limits) and its drawing, from the window's own shape (any width / height above 4:3); 0 = the 4:3 framing
  // with black bars. screen_extra = the same for the screen veils and the title's 3D sea (never forced to 0 below).
  wide_extra = config.widescreen ? render::wide_extra_for(static_cast<float>(ctx.extent().width), static_cast<float>(ctx.extent().height))
                                 : 0.0f;
  render::set_wide_extra(wide_extra);
  render::set_screen_extra(wide_extra);
  // [Video] FovDeg (horizontal degrees of the 4:3 picture) -> the H scale: the PS1's H 512 gives 2 atan(160 / 512); 35 = exactly the PS1 view.
  render::set_fov_scale(config.fov_deg == 35 ? 1.0f
                                             : (160.0f / 512.0f) / std::tan(static_cast<float>(config.fov_deg) * 0.5f * 3.14159265f / 180.0f));
  // [Video] WideHud ("Wide Menus/HUD"): the columns per side the battle HUD edge groups and the main-menu screens spread out by -
  // default 0 (the 4:3 canvas), 16:9 = at most a 16:9 frame's columns (49 at the 236-row fit), full = the window's.
  hud_cols = !config.widescreen || config.wide_hud == 0 ? 0.0f
             : config.wide_hud == 1 ? std::min(wide_extra, render::wide_extra_for(16.0f, 9.0f)) : wide_extra;
  if (battle_ready) battle.set_wide_hud_columns(hud_cols);
  if (menu_ok) menu.set_wide_columns(wide_extra, hud_cols);
  if (results_ok) results.set_wide_columns(wide_extra, hud_cols);
  if (party_ok) party_screen.set_wide_columns(wide_extra, hud_cols);
  if (battle_ready) {
    battle.set_auto(config.debug_battle_auto);
    // Cheats: Auto battle = the command picks only; the results page stays manual (RDE).
    battle.set_auto_commands(config.cheats.auto_battle);
  }
}

// Advances the game in its current mode.
bool App::simulate() {
  if (routing && world_ok) {
    route_frame();
  } else if (replaying) {
    replay_frame();
  } else if (flow_ready && flow.boot_logo_active()) {
    // RDE's boot logo: nothing of the game runs until it is over; one image frame
    // per 60 Hz tick; any key / pad button jumps to the end of the current image.
    const bool settings_pause = ui.options_open && ui.current_tab() != "Debug";
    flow.tick_boot_logo(settings_pause ? 0 : step.ticks, !settings_pause && game::GameFlow::any_button(input));
  } else if (battle_ok || battle_in_field) {
    advance(input, frame_dt, step.ticks);
    if (recorder.active()) recorder.write(frame_dt, step.ticks, input);
  } else if (world_ok && sweeping) {
    if (!sweep_frame()) return false;
  } else if (world_ok) {
    field_frame();
  }
  return true;
}

void App::field_frame() {
  // PageUp / PageDown: cycle through the rooms (rooms that fail to load are skipped); only in the field without the flow.
  int dir = 0;
  if (!flow_enabled && !movie_open) {
    if (input.key_pressed[SDL_SCANCODE_PAGEDOWN]) dir = 1;
    if (input.key_pressed[SDL_SCANCODE_PAGEUP]) dir = -1;
  }
  if (dir != 0) {
    const auto& rooms = world.room_list();
    const int room_count = static_cast<int>(rooms.size());
    auto it = std::find(rooms.begin(), rooms.end(), world.room_name());
    int idx = it == rooms.end() ? 0 : static_cast<int>(it - rooms.begin());
    for (int tries = 0; tries < room_count; ++tries) {
      idx = (idx + dir + room_count) % room_count;
      if (world.load_room(rooms[static_cast<size_t>(idx)], 0)) break;
    }
  } else {
    // The settings window pauses the game (the Debug tab watches it running). While paused the render interpolation stays at the
    // last tick's state: the accumulator's alpha kept cycling between the previous and current states (the title sea jittered).
    const bool settings_pause = ui.options_open && ui.current_tab() != "Debug";
    const int ticks = settings_pause ? 0 : step.ticks;
    if (settings_pause) step.alpha = 1.0f;
    advance(input, frame_dt, ticks);
    if (recorder.active()) recorder.write(frame_dt, ticks, input);
  }
}

// What the Esc options window changed this frame.
void App::apply_options_change(const render::OptionsChange& change) {
  if (change.quit) quit_requested = true;
  if (change.restart_game) restart_requested = true;
  if (change.vsync) ctx.set_vsync(config.vsync);
  if (change.msaa) ctx.set_msaa(static_cast<u32>(config.msaa));
  if (change.anisotropy) ctx.set_anisotropy(static_cast<u32>(config.anisotropy));
  // [Video] WindowMode / Width / Height / Display apply live; the swapchain follows.
  if (change.window) {
    window.apply_video(static_cast<int>(config.window_mode), config.width, config.height, config.display);
    ctx.request_resize();
  }
  if (change.art_mode || mods_dirty) apply_art_mode();
  mods_dirty = false;
  if (change.audio) apply_audio_config();
  // A live switch remastered <-> AKAO, field and battle.
  if (change.bgm_source) {
    const bool remastered = config.bgm_source != "original";
    // A battle's own song switches itself.
    const bool battle_owns = battle_ready && battle.set_bgm_remastered(remastered);
    if (world_ok) world.set_bgm_remastered(remastered, !battle_owns);
  }
  if (world_ok && change.portraits) {
    world.set_portrait_set(config.portraits);
    if (menu_ok) menu.set_portrait_set(config.portraits);
    if (results_ok) results.set_portrait_set(config.portraits);
  }
  if (change.language) {
    import::Lang nl;
    if (import::parse_lang(config.language, nl)) {
      if (world_ok) world.set_language(nl);
      if (flow_ready) flow.set_language(nl);
      ext_host.language = static_cast<u32>(nl);
      if (menu_ok) menu.set_language(nl);
      if (shop_ok) shop.set_language(nl);
      if (results_ok) results.set_language(nl);
    }
  }
}

}  // namespace oc::app
