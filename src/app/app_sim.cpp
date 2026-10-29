// One block of simulation (advance): the flow screens, the pause screen, movies, the main menu / shops / party screen / key-item list,
// the field itself and its auto-save; plus the live art-mode and audio switches.
#include "app/app.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <exception>

#include "core/log.h"
#include "game/disc_index.h"
#include "game/free_cam.h"
#include "platform/keymap.h"
#include "platform/rumble.h"
#include "render/imgui_mods_tab.h"
#include "script/field_host.h"
#include "script/field_vm.h"

namespace oc::app {

bool App::confirm_edge(const platform::Input& in) {
  return platform::action_pressed(in, platform::Action::Ok) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_SOUTH]);
}

void App::apply_audio_config() {
  mixer.set_bus_gain(audio::Bus::Master, config.master);
  mixer.set_bus_gain(audio::Bus::Bgm, config.music);
  mixer.set_bus_gain(audio::Bus::Sfx, config.sfx);
  mixer.set_bus_gain(audio::Bus::Voice, float(config.voice_volume) / 100.0f);
  if (voice_player) voice_player->set_enabled(config.voices);
}

// Art mode switch (F6, the Esc options overlay, route `art`): a pure rendering swap everywhere. Nothing is reloaded or
// restarted - field room (title room included), battle, results, menus, shops - each part only rebuilds its art for the new mode.
// Modded = HD art + the mod overlay: the VFS is re-pointed (hd mode + overlay) and HD <-> Modded / mod list edits force the same swap.
void App::apply_art_mode() {
  const bool hd = platform::art_is_hd(config.art_mode);
  const ui::ArtMode am = hd ? ui::ArtMode::Hd : ui::ArtMode::Original;
  const std::uint32_t mod_gen = files.generation();
  mods.apply(files, render::mod_art(config.art_mode));
  const bool mods_changed = files.generation() != mod_gen;
  files.set_hd_mode(hd);
  if (world_ok) world.set_art_mode(hd, mods_changed);
  if (battle_ready) battle.set_art_mode(hd, mods_changed);
  if (results_ok) results.set_art_mode(hd, mods_changed);
  if (party_ok) party_screen.set_hd(hd);
  if (flow_ready) flow.set_art_mode(am);
  if (menu_ok) menu.set_mode(am);
  if (shop_ok) shop.set_mode(am);
  log::info("art mode: {}", config.art_mode == platform::ArtMode::Modded ? "modded" : hd ? "hd" : "original");
}

void App::end_movie() {
  // Keep the last frame for the cross-fade.
  movie_player.close(true);
  ghost_room = std::string(world.room_name());
  ghost_tick0 = world.tick_count();
  movie_open = false;
  ctx.set_clear_color(0.08f, 0.09f, 0.14f);
  mixer.resume();
  world.movie_finished();
  log::info("movie: finished");
  if (flow_ready) flow.set_title_logo_ready(true);
}

void App::start_movie(const game::FieldWorld::MovieRequest& rq) {
  movie_player.set_mixer(mixer.device_open() ? &mixer : nullptr);
  // The field movie table's frame range: a few rows play part of a file (the title / Zelbess excerpts).
  u32 mstart = 0, mend = 0;
  game::DiscIndex::movie_range(world.disc(), rq.index, mstart, mend);
  if (!movie_player.open(files, rq.name, config.language, mstart, mend) ||
      !movie_player.init_gpu(ctx, sprites)) {
    log::warn("movie: {} could not be opened; skipped", rq.name);
    movie_player.close();
    world.movie_finished();
    if (flow_ready) flow.set_title_logo_ready(true);
    return;
  }
  movie_open = true;
  movie_skippable = rq.skippable;
  ctx.set_clear_color(0.0f, 0.0f, 0.0f);
  mixer.pause();
  log::info("movie: {} ({}x{}, {:.1f} fps){}", movie_player.member(), movie_player.width(), movie_player.height(), movie_player.fps(),
            rq.skippable ? ", skippable" : "");
}

void App::open_main_menu(bool script) {
  world.capture_state();
  menu.open(flow.game_state(), world.can_save());
  menu_from_script = script;
  log::info("menu: opened ({}, saving {})", script ? "script" : "key", world.can_save() ? "allowed" : "blocked");
}

void App::handle_script_menu(const game::FieldWorld::MenuRequest& rq) {
  using script::MenuKind;
  switch (rq.kind) {
    case MenuKind::MainMenu:
      if (menu_ok) {
        open_main_menu(true);
        return;
      }
      break;
    case MenuKind::Save:
      // Using debug tools only warns; the save point still opens.
      if (debug_tools.saving_disabled()) ui.toast("Debug tools were used this session");
      if (menu_ok) {
        world.capture_state();
        menu.open(flow.game_state(), true);
        // A save point goes straight to the save list.
        menu.debug_show(ui::MenuScreen::Save, 1);
        menu_from_script = true;
        return;
      }
      break;
    case MenuKind::ElementShop:
    case MenuKind::WeaponShop:
    case MenuKind::TradingPost:
      if (shop_ok) {
        shop.open(flow.game_state(), rq.kind == MenuKind::ElementShop ? ui::ShopKind::Element : rq.kind == MenuKind::TradingPost ? ui::ShopKind::Trading : ui::ShopKind::Smith, rq.arg);
        shop_open = true;
        log::info("shop: {} {} opened", rq.kind == MenuKind::ElementShop ? "element shop" : rq.kind == MenuKind::TradingPost ? "trading post" : "blacksmith", rq.arg);
        return;
      }
      break;
    case MenuKind::PartyScreen:
      if (party_ok) {
        world.capture_state();
        party_screen.set_hd(platform::art_is_hd(config.art_mode));
        // Argument 1 opens the Relief Charm variant.
        if (rq.arg == 1) party_screen.open_relief(world.game_state());
        else party_screen.open(world.game_state());
        return;
      }
      if (flow_ready) {
        world.capture_state();
        flow.begin_party();
        return;
      }
      break;
    case MenuKind::NameEntry:
      if (flow_ready) {
        // RDE shows the user guide first when MAP_ID (variable 6) is 0 or 8 (New Game+).
        const int map_var = world.vm().var(script::var::kMapId);
        flow.begin_rename(rq.arg < 0 ? 0 : rq.arg, map_var == 0 || map_var == 8);
        return;
      }
      break;
    case MenuKind::NewGamePlus:  // in the ending room: the title list in variant 2 over the cleared game
      if (flow_ready) {
        world.capture_state();
        flow.begin_ng_plus(flow.game_state().slot);
        return;
      }
      break;
    default: log::info("menu: kind {} is not implemented; closed at once", static_cast<int>(rq.kind)); break;
  }
  world.menu_closed();
}

// Applies what the flow asked for (New Game / Continue load a room, settings, quit).
void App::apply_flow_action(const game::FlowAction& action) {
  // After a load / Continue / New Game the next tick only records the location.
  if (action.state_changed && flow.state() != game::FlowState::Opening) autosave.note_load();
  if (action.quit) quit_requested = true;
  if (action.open_settings) ui.options_open = true;
  if (action.stop_music && world_ok) world.stop_music();
  if (action.state_changed && world_ok) {
    world.set_game_state(&flow.game_state());
    growth_progress = 0;
    if (flow.state() == game::FlowState::Opening) world.reset_for_new_game();
    else world.restore_vars();
  }
  // The title script plays its movie before the jump.
  if (action.load_room && world_ok && action.movie_row >= 0) {
    world.play_movie(action.movie_row, 0, true);
    if (world.movie_pending()) {
      deferred_flow = action;
      deferred_flow.movie_row = -1;
      deferred_flow.state_changed = false;
      deferred_flow.stop_music = false;
      deferred_flow_set = true;
      start_movie(world.take_movie());
      if (movie_open) return;
      // The movie could not open: load the room at once.
      deferred_flow_set = false;
    }
  }
  if (action.load_room && world_ok) {
    std::string room = action.room;
    // Continue: the saved id names a room on the saved disc (variable 0x42 was restored above).
    if (action.room_id >= 0) {
      const std::string stem = world.room_stem_for_id(action.room_id);
      if (!stem.empty()) room = stem;
    }
    if (room.empty() || !world.load_room(room, action.param)) {
      log::error("flow: could not load {}", room);
      flow.start(game::FlowState::Title);
    } else if (action.restore_position) {
      world.place_leader(action.x, action.z);
    }
  }
}

// Start (RDE: pad Start, keyboard Space) over a running battle or an FMV, and the pause screen's modal input (all contexts). A battle allows
// pausing for the whole fight (the field allows it per frame, FieldWorld::pause_allowed); pausing an FMV is our addition (RDE never does).
// The pad's Start opens it on the release of a short press (a long one opens the settings window) and closes it on the press.
bool App::pause_modal(const platform::Input& in) {
  if (!pause_ok) return false;
  const bool start_edge = in.key_pressed[platform::kStartKey] || start_tap;
  if (!pause_screen.active()) {
    const bool over_battle = (battle_ok || battle_in_field) && !battle.transition_active();
    if (start_edge && (over_battle || movie_open)) {
      open_pause();
      log::info("pause: opened over {} (vibration {})", movie_open ? "a movie" : "a battle", pause_screen.vibration());
      return true;
    }
    return false;
  }
  const int dx = (platform::action_pressed(in, platform::Action::Right) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_RIGHT])) -
                 (platform::action_pressed(in, platform::Action::Left) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT]));
  const int dy = (platform::action_pressed(in, platform::Action::Down) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN])) -
                 (platform::action_pressed(in, platform::Action::Up) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_UP]));
  // Start again or B closes.
  const bool close = in.key_pressed[platform::kStartKey] || platform::action_pressed(in, platform::Action::Cancel) ||
                     (in.pad_connected && (in.pad_pressed[SDL_GAMEPAD_BUTTON_START] || in.pad_pressed[SDL_GAMEPAD_BUTTON_EAST]));
  if (const int sfx = pause_screen.input(dx, dy, confirm_edge(in), close)) ui_sfx(sfx);
  // The Start press that closed it neither reopens it on its release nor opens the settings window when held.
  if (close && !pause_screen.active()) start_held = false;
  // Switching vibration on gives a test pulse.
  if (pause_screen.vibration() && !world.game_state().option(ui::opt::kVibration)) platform::Rumble::instance().pulse();
  // The vibration option is saved with the game.
  world.game_state().set_option(ui::opt::kVibration, pause_screen.vibration());
  // World Names = script var 0x52 (the world-map scripts and the HD name read it), written back on closing while its row is shown.
  if (!pause_screen.active() && pause_screen.world_row() && world_ok) world.vm().set_var(0x52, pause_screen.world_names() ? 1 : 0);
  return true;
}

// The World Names row needs a map with a world name (var 0x50) and the scenario past Viper Manor's first talk with Melchior (0x34).
void App::open_pause() {
  pause_screen.set_language(lang);
  const bool world_row = world_ok && world.vm().var(script::var::kScenario) >= 0x34 && world.vm().var(0x50) != 0;
  pause_screen.open(world.game_state().option(ui::opt::kVibration), world_row, world_ok && world.vm().var(0x52) != 0);
}

// `ticks` field ticks (or battle / menu / movie time) for the input `in` over `dt` seconds.
void App::advance(const platform::Input& in, double dt, int ticks) {
  // Play time: the retail main loop adds the elapsed vsyncs every iteration, so it runs through field, battle,
  // results, menus and shops alike (+1 per 60 Hz frame in a battle). Not during a movie (the host player owns the loop) or the
  // title scene (New Game / a load replaces the state). The options window pauses the simulation (ticks = 0).
  if (ticks > 0 && (flow_ready || world_ok) && !movie_open && !(flow_ready && flow.scene_mode())) {
    auto& pslot = flow_ready ? flow.game_state().slot : world.game_state().slot;
    pslot.set_playtime_frames(pslot.playtime_frames() + static_cast<u32>(ticks));
  }
  if (title_bd.running() && world.room_name() != game::kTitleRoom) title_bd.stop();
  // Cross-fade the last movie frame into the scene behind it.
  if (movie_player.held() && !movie_open && world_ok) {
    const u64 tk = world.tick_count();
    if (std::string(world.room_name()) != ghost_room || tk < ghost_tick0 || tk >= ghost_tick0 + 4) movie_player.fade_held(std::min(dt, 0.05), 0.5);
  }
  // Screen fades around a load (title Continue / in-game Load): the game stands still while it fades out.
  if (flow_ready) {
    const bool out = flow.fading_out();
    const game::FlowAction fa = flow.tick_fade(static_cast<float>(dt));
    if (out) {
      // Continue+ (New Game+ list): the ending room's FE 61 is answered before the loaded room replaces it.
      if (flow.take_ng_plus_result() == game::GameFlow::NgPlusResult::ContinuePlus) world.menu_closed();
      apply_flow_action(fa);
      return;
    }
    // Field main menu: the fade-out is over, the menu opens under the veil.
    if (flow.take_open_ready() && menu_ok && menu.closed()) {
      open_main_menu(false);
      log::info("fade: menu opened after the fade-out, fading in");
      return;
    }
  }
  // The pause screen (any context) owns the input; battle / movie / field stand still under it.
  if (pause_modal(in)) return;
  if (battle_ok) {
    if (results_ok && results.active()) {
      results.update(static_cast<float>(dt), in);
      return;
    }
    battle.update(static_cast<float>(dt), in);
    if (battle.finished() && results_ok && !results_shown && battle.battle()) {
      results_shown = true;
      results.open(*battle.battle(), config.debug_battle);
    }
    return;
  }
  // Entering the title: run the title room behind the menu.
  if (flow_ready && flow.take_scene_request()) {
    world.reset_for_new_game();
    if (world.load_room(game::kTitleRoom, 0)) {
      log::info("title: scene room {} running", game::kTitleRoom);
    } else {
      log::warn("title: could not load {}; plain title screen", game::kTitleRoom);
      flow.set_title_scene(false);
      flow.start(game::FlowState::Title);
    }
  }
  if (movie_open) {
    movie_player.update(dt);
    if (movie_skippable && confirm_edge(in)) movie_player.skip();
    if (movie_player.finished()) {
      end_movie();
      if (deferred_flow_set) {
        deferred_flow_set = false;
        apply_flow_action(deferred_flow);
      }
    }
    return;
  }
  if (flow_ready && flow.scene_mode() && field_visible()) {
    // The title room's script runs (logo FMV, then the pulsing logo scene); the title screen reads the buttons while it is the scene.
    if (world.movie_pending()) {
      start_movie(world.take_movie());
      return;
    }
    if (world.menu_pending()) {
      world.take_menu();
      world.menu_closed();
    }
    // A title-scene battle request is dropped (see scene_battle).
    scene_battle();
    if (world.room_name() == game::kTitleRoom) {
      const auto ph = flow.title().phase();
      apply_flow_action(flow.update(in, static_cast<float>(dt)));
      // The title room's music ends as the menu appears.
      if (ph != ui::TitleScreen::Phase::Menu && flow.title().phase() == ui::TitleScreen::Phase::Menu) world.stop_music();
      // Hold the idle timer: in the menu, and while the Debug free camera is on (no attract demo / opening FMV while flying the sea).
      if (ph == ui::TitleScreen::Phase::Menu || ph == ui::TitleScreen::Phase::FadeOut || game::free_cam().on()) world.vm().set_var(0x404, 0);
    // An attract demo is running: any button returns to the title logo.
    } else if (game::GameFlow::any_button(in)) {
      if (world.load_room(game::kTitleRoom, 1)) flow.title().restart_prompt(), flow.set_title_logo_ready(true);
    }
    if (flow.scene_mode()) world.update(platform::Input{}, ticks);
    if (title_bd.running() && world.room_name() == game::kTitleRoom)
      for (int i = 0; i < ticks; ++i) title_bd.tick();
    return;
  }
  if (flow_ready && flow.owns_screen()) {
    apply_flow_action(flow.update(in, static_cast<float>(dt)));
    if (flow.take_rename_finished()) world.menu_closed();
    if (flow.ng_plus_active()) {
      // the list stays up
    } else if (flow.take_ng_plus_result() == game::GameFlow::NgPlusResult::NewGamePlus) {
      // New Game+: the state was replaced in place; the field takes its variables (0x7E = 2, MAP_ID 8, disc 1) and party, then the
      // ending room's script goes on (it names Serge, the dream party, then jumps with movie 2).
      autosave.note_load();
      world.restore_vars();
      // The previous map id becomes 8 (opn_a01i then has PREVIOUS_MAP_ID 8).
      world.vm().set_room_register(8);
      world.state_edited();
      world.menu_closed();
    }
    if (flow.take_party_finished()) {
      // The party changed: rebind the party entities and models.
      world.state_edited();
      world.menu_closed();
    }
    return;
  }
  if (battle_in_field) {
    // The results screen: the rewards are written back when it has exited.
    if (results_ok && results.active()) {
      results.update(static_cast<float>(dt), in);
      if (results.done()) finish_field_battle();
      return;
    }
    battle.update(static_cast<float>(dt), in);
    if (battle.finished() && !(results_ok && battle.battle() && results.open(*battle.battle(), battle_scene))) finish_field_battle();
    return;
  }
  // Some room exits want the frame shown at the exit captured.
  if (world_ok && world.capture_wanted())
    ctx.request_capture([this](std::vector<u8>&& pixels, u32 width, u32 height) {
      world.set_captured_frame(std::move(pixels), width, height);
    });
  if (menu_ok && !menu.closed()) {
    menu.update(in, ticks);
    if (menu.closed()) {
      // Save/Load > Load: start the slot like Continue.
      if (auto lr = menu.take_load_request()) {
        ui::SlotEntry entry;
        entry.card = lr->card;
        entry.slot = lr->slot;
        entry.block = lr->block;
        entry.data = std::move(lr->data);
        log::info("menu: load slot {}", entry.slot);
        menu_from_script = false;
        if (flow_ready) apply_flow_action(flow.load_slot(entry));
        return;
      }
      world.state_edited();
      if (menu_from_script) world.menu_closed(menu.saved() ? 1 : 0);
      menu_from_script = false;
      // The field fades in from black over 0x10 ticks after the menu.
      world.fade_in_from_black(0x10);
      log::info("fade: field fade-in after menu close (0x10 ticks)");
    }
    return;
  }
  // The party screen owns the input until its closing fade has run.
  if (party_ok && party_screen.active()) {
    if (const int sfx = party_screen.update(in, static_cast<float>(dt))) ui_sfx(sfx);
    if (party_screen.take_finished()) {
      // The Relief Charm variant stores the relief member.
      if (party_screen.kind() == ui::PartyScreen::Kind::Relief) {
        world.game_state().slot.head[ui::kReliefHead] = party_screen.relief_result();
      } else {
        world.game_state().slot.set_party(party_screen.result());
        // The party changed: rebind the party entities and models.
        world.state_edited();
      }
      world.menu_closed();
    }
    return;
  }
  // Modal: the field waits while the key-item list is up.
  if (key_popup_ok && key_popup.active()) {
    // The Item key / pad-button key-item list: the stick / arrows move the hand.
    const int dx = (platform::action_pressed(in, platform::Action::Right) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_RIGHT])) -
                   (platform::action_pressed(in, platform::Action::Left) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT]));
    const int dy = (platform::action_pressed(in, platform::Action::Down) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN])) -
                   (platform::action_pressed(in, platform::Action::Up) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_UP]));
    // Confirm uses the item (sfx 8) or refuses it (0x18); cancel / the list button closes (6). RDE pad: B cancels, X (the list button) closes.
    const bool cancel = platform::action_pressed(in, platform::Action::Cancel) || in.key_pressed[SDL_SCANCODE_ESCAPE] ||
                        platform::action_pressed(in, platform::Action::Item) ||
                        (in.pad_connected && (in.pad_pressed[SDL_GAMEPAD_BUTTON_EAST] || in.pad_pressed[SDL_GAMEPAD_BUTTON_WEST]));
    if (const int sfx = key_popup.list_input(dx, dy, confirm_edge(in), cancel)) ui_sfx(sfx);
    if (const int used = key_popup.take_used()) {
      log::info("key item list: used {}", used);
      world.use_key_item(used);
    }
    key_popup.update(static_cast<float>(dt));
    return;
  }
  // Item key (F / R) / pad button (RDE; pad: X = key items): the key-item list panel opens straight over the field, without the main menu.
  if (key_popup_ok && menu_ok && world_ok && world.view().loaded() && !world.menu_active() && !world.battle_active() && world.idle() && menu.closed() &&
      (platform::action_pressed(in, platform::Action::Item) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_WEST]))) {
    std::vector<ui::KeyItemPopup::ListEntry> list;
    for (const int id : world.game_state().key_items()) {
      const std::string nm = id < static_cast<int>(tables.key_item_names.size()) ? tables.key_item_names[static_cast<size_t>(id)] : std::string("?");
      list.push_back({id, import::utf8_to_u32(nm), world.key_item_usable(id)});
    }
    const ui::GameState& kgs = world.game_state();
    key_popup.set_style(platform::art_is_hd(config.art_mode), kgs.option(ui::opt::kCustomWindow) ? kgs.skin : 0, lang == import::Lang::Jp);
    // Row names come from the system messages in the text language.
    key_popup.set_language(lang);
    std::string ids;
    for (const auto& entry : list) ids += std::to_string(entry.id) + (entry.enabled ? " " : "(off) ");
    log::info("key item list: opened, owned {}", ids);
    // The list opens without a sound.
    key_popup.open_list(std::move(list));
    return;
  }
  // Start (RDE: pad Start, keyboard Space): the pause screen over the frozen field / world map, message windows and cutscenes included.
  if (pause_ok && world_ok && world.view().loaded() && world.pause_allowed() && menu.closed() &&
      (in.key_pressed[platform::kStartKey] || start_tap)) {
    open_pause();
    log::info("pause: opened (vibration {}, world names row {})", pause_screen.vibration(), pause_screen.world_row());
    return;
  }
  if (shop_open) {
    shop.update(in, ticks);
    if (shop.closed()) {
      shop_open = false;
      world.state_edited();
      world.menu_closed();
    }
    return;
  }
  if (!world_ok || !world.view().loaded()) return;
  if (world.menu_pending()) {
    handle_script_menu(world.take_menu());
    return;
  }
  if (world.movie_pending()) {
    start_movie(world.take_movie());
    return;
  }
  // The Menu key (I / Tab) / pad Y opens the menu (Start = the pause screen).
  if (menu_ok && !world.menu_active() && !world.battle_active() && world.idle() && world.menu_key_allowed() &&
      (platform::action_pressed(in, platform::Action::Menu) || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_NORTH]))) {
    // Fade to black, then open the menu.
    if (flow_ready) {
      log::info("fade: menu-open fade-out started");
      flow.begin_menu_open_fade();
    } else {
      open_main_menu(false);
    }
    return;
  }
  world.update(in, ticks);
  if (flow_ready && config.auto_save && have_save_dat && !save_path.empty() && world.view().loaded() && flow.state() == game::FlowState::Field) {
    // RDE auto-save on a location change: card 1 slot 0 in the save.dat the Save menu uses, and / or the OpenCross page's
    // auto-save (Autosave::targets: each only where saves already exist; with none anywhere, the OpenCross page).
    if (autosave.update({static_cast<u16>(std::max(world.map_id(), 0)), world.can_save(), world.map_id(), world.entrance_param()})) {
      world.capture_state();
      const game::Autosave::Targets targets = game::Autosave::targets(&save_dat, &oc_saves);
      bool written = false;
      try {
        if (targets.card && game::Autosave::write(save_dat, flow.game_state().slot)) {
          save_dat.save(save_path);
          written = true;
        }
        if (targets.page) {
          game::Autosave::write(oc_saves, flow.game_state().slot);
          written = true;
        }
      } catch (const std::exception& error) {
        log::warn("autosave failed: {}", error.what());
      }
      if (written) {
        log::info("autosave: map {:#x} written ({}{})", world.map_id(), targets.card ? "card 1" : "", targets.page ? (targets.card ? " + OpenCross" : "OpenCross") : "");
        // RDE's "AUTO SAVE" strip.
        if (feature_icons_ok) feature_icons.show_autosave();
      }
    }
  }
  if (flow_ready) {
    flow.note_field(world.room_name());
  }
  // The effects library + JP HUD fonts load on worker threads from the title / first room, not at the first encounter.
  if (battle_ready && !battle_warmed && flow_ready) battle.warm_up(), battle_warmed = true;
  if (world.battle_requested()) {
    // Fallback (a no-op when warmed): the JP fonts load on a worker thread during the swirl.
    if (!fb_prefetched && battle_ready) battle.prefetch_fonts(), fb_prefetched = true;
    // From the request on (the swirl is ~105 ticks of idle time) the load runs ~3 ms per frame; not on the frames that start the effect.
    if (++fb_req_frames >= 4) field_battle_load_step();
  } else {
    fb_prefetched = false;
    fb_req_frames = 0;
  }
  // A title-scene (attract demo) request that arrived while the view was not loaded (room change) is never a real battle: the dummy
  // title party was wiped and the GAME OVER panel showed.
  if (world.battle_ready() && flow_ready && flow.scene_mode()) {
    world.take_battle();
    world.end_battle(0);
  } else if (world.battle_ready() && (!battle_ready || battle.load_complete())) {
    // A late load keeps the field (white) up, never waits.
    start_field_battle();
  }
}

}  // namespace oc::app
