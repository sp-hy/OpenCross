// The boot sequence: config -> window -> VFS / mods -> GPU -> audio -> routes -> flow / saves -> menus -> field -> battle -> title,
// then the start state ([Debug] Room / Flow / Battle, the room sweep, or the title with RDE's boot logo).
#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include "core/log.h"
#include "game/cheats.h"
#include "game/free_cam.h"
#include "platform/paths.h"
#include "render/imgui_mods_tab.h"
#include "render/imgui_update_tab.h"
#include "render/texture_registry.h"
#include "script/field_host.h"

namespace oc::app {

namespace {

// Uploads ui/background.png from the install; leaves `texture` empty if the file is missing.
bool load_test_image(const vfs::Vfs& files, render::GpuContext& ctx, render::Texture& texture) {
  auto bytes = files.read("ui/background.png");
  if (!bytes) {
    log::warn("ui/background.png not found in the install");
    return false;
  }
  render::DecodedImage img;
  if (!render::decode_image(*bytes, img)) return false;
  log::info("ui/background.png: {}x{}", img.width, img.height);
  return texture.create(ctx, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data(), nullptr, true);
}

}  // namespace

int App::init(int argc, char** argv) {
  init_config(argc, argv);
  if (!init_window()) return 1;
  init_vfs();
  if (!init_render()) return 1;
  init_audio();
  if (!init_routes()) return 2;
  init_flow();
  init_saves();
  init_menus();
  init_field();
  init_battle();
  init_title();
  init_start_state();
  init_route_state();
  init_debug_tab();
  return 0;
}

void App::init_config(int argc, char** argv) {
  exe = platform::exe_dir();
  // --config <file>: use another ini (automated runs: tests/routes/*.ini); the log then sits next to it as <name>.log.
  config_file = exe / "opencross.ini";
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string_view(argv[i]) == "--config") config_file = std::filesystem::absolute(argv[i + 1]), own_ini = false;
  // The log file: [Debug] Log (Debug tab checkbox, default off) for the player's ini; --config runs (routes) and Debug builds always log.
  log_path = own_ini ? (exe / "opencross.log").string() : std::filesystem::path(config_file).replace_extension(".log").string();
#ifdef NDEBUG
  log_forced = !own_ini;
#else
  log_forced = true;
#endif
  first_launch = !config.load(config_file);
  if (log_forced || config.debug_log) log::init(log_path);
  if (first_launch) {
    // A first launch (no ini) opens 16:9 widescreen, 1280 x 720. Only here: the Config member defaults (4:3, 1280 x 960,
    // widescreen off) stay what an existing / --config ini without those keys reads, so route inis keep their frame size.
    config.aspect = "16:9";
    config.width = 1280;
    config.height = 720;
    config.widescreen = true;
    log::info("no opencross.ini, writing defaults");
    config.save(config_file);
  }
  // The rule code reads the live [Cheats] set.
  platform::bind_cheats(&config.cheats);
  // A previous Update tab install left <exe>.old behind.
  platform::Updater::cleanup_after_update();

  // Scripted runs stay off the player's Steam account.
  steam_allowed = own_ini && !std::getenv("OC_QUIET");

  game_dir = platform::find_game_dir(config.game_dir);
  const platform::SaveDirPick save_pick = platform::find_save_dir(config.save_dir);
  save_dir = save_pick.dir;
  log::info("game dir: {}", game_dir.empty() ? "(not found)" : game_dir.string());
  log::info("save dir: {} ({})", save_dir.empty() ? "(not found)" : save_dir.string(), save_pick.how);
}

bool App::init_window() {
  if (!window.create("OpenCross", config.width, config.height, static_cast<int>(config.window_mode), config.display)) return false;
  // No ini yet: [Game] Language follows the OS (SDL is initialised by the window now); an existing ini is never overridden.
  if (first_launch) {
    config.language = platform::detect_os_language();
    log::info("first launch: [Game] Language = {} (from the OS locale)", config.language);
    config.save(config_file);
  }
  return true;
}

void App::init_vfs() {
  if (game_dir.empty() || !files.open(game_dir)) log::warn("VFS unavailable; running without game data");
  // RDE: cdrom2 overrides cdrom in HD mode.
  files.set_hd_mode(platform::art_is_hd(config.art_mode));
  // Mods: <GameDir>\mods\<name>\..., archives and bare .dat files there, resolved once; the overlay follows the art mode (Modded = HD + mods).
  if (!files.game_dir().empty()) {
    {  // a missing <GameDir>\mods is created so players see where mods go (the Mods tab's Rescan finds what they drop in)
      std::error_code ec;
      if (!std::filesystem::exists(files.game_dir() / "mods", ec)) {
        if (std::filesystem::create_directories(files.game_dir() / "mods", ec)) log::info("mods: created {}", (files.game_dir() / "mods").string());
        else log::warn("mods: cannot create {}: {}", (files.game_dir() / "mods").string(), ec.message());
      }
    }
    mods.scan(files, files.game_dir() / "mods", render::mod_prefs(config));
    // New folders get their defaults (written with the next save).
    render::store_mod_prefs(config, mods);
    mods.apply(files, render::mod_art(config.art_mode));
  }
  // Texture replacement is always on; a texture mod is switched by its own Mods row. Per-visit dumping is off - the
  // "Dump all textures" button / `occ dump-textures` write the files.
  render::texture_registry::bind(&files);
  render::texture_registry::configure({false, true});
  // Mods tab "Dump all textures": the headless dump on a worker thread, progress polled by the tab.
  render::TextureDumpHooks hooks;
  // `what` holds game::DumpSet bits.
  hooks.start = [this](unsigned what) { return !files.game_dir().empty() && what != 0 && texture_dump_job.start(files.game_dir(), what); };
  hooks.cancel = [this] { texture_dump_job.request_cancel(); };
  hooks.running = [this] { return texture_dump_job.running(); };
  hooks.status = [this] {
    auto& progress = texture_dump_job.progress();
    return progress.phase() + " " + std::to_string(progress.done.load()) + " / " + std::to_string(progress.total.load()) + ", " +
           std::to_string(progress.files.load()) + " files";
  };
  hooks.fraction = [this] {
    auto& progress = texture_dump_job.progress();
    return progress.total ? static_cast<float>(progress.done.load()) / static_cast<float>(progress.total.load()) : 0.0f;
  };
  hooks.summary = [this] { return texture_dump_job.progress().summary(); };
  render::set_texture_dump_hooks(std::move(hooks));
}

bool App::init_render() {
  if (!ctx.init(window.handle(), config.vsync, config.gpu_driver)) return false;
  ctx.set_msaa(static_cast<u32>(config.msaa));
  ctx.set_anisotropy(static_cast<u32>(config.anisotropy));
  ctx.set_clear_color(0.08f, 0.09f, 0.14f);

  // The [Video] Crt pass.
  crt_ok = crt_pass.init(ctx);
  if (!crt_ok) log::warn("CRT pass unavailable");
  if (!sprites.init(ctx)) return false;
  if (!meshes.init(ctx)) return false;
  if (!linear.create(ctx, true)) return false;
  if (!ui.init(ctx, window.handle())) return false;
  if (ctx.driver_fallback()) ui.toast("Graphics backend: " + config.gpu_driver + " unavailable, using " + ctx.driver());
  ui.on_restart = [this] { restart_for_update = true; };
  window.set_event_hook(&render::ImGuiLayer::event_hook, &ui);
  ui.add_tab("Mods", [this] {
    bool settings_dirty = false;
    if (render::draw_mods_tab(config, mods, files, &settings_dirty)) {
      ui.mark_changed();
      mods_dirty = true;
    } else if (settings_dirty) {
      // The dump category checkboxes: save the ini only.
      ui.mark_changed();
    }
  }, 30);
  ui.add_tab("Cheats", [this] {
    if (game::draw_cheats_tab(config)) ui.mark_changed();
  }, 40);
  // The Update tab: GitHub release check / install. "Restart now" leaves the loop, then starts the new exe.
  ui.add_tab("Update", [this] { render::draw_update_tab(updater, &restart_for_update); }, 80);

  if (load_test_image(files, ctx, background)) background_set = render::TexRef(background, linear);
  if (nearest.create(ctx, false) && field_rt.create(ctx, static_cast<u32>(render::kLayerW), static_cast<u32>(render::kFieldRows)))
    field_rt_set = field_rt.ref(nearest);
  return true;
}

void App::init_audio() {
  if (!import::parse_lang(config.language, lang)) log::warn("unknown [Game] Language '{}', using en", config.language);
  sweeping = config.debug_sweep_ticks > 0;

  // BGM mixer (RDE gain formula with the config percentages as bus gains), the AKAO sound-effect bank (field C6 / FE 22, menus,
  // battle) and the AKAO sequencer for [Audio] BgmSource=original.
  // Voice lines: searched in the enabled mods' voices folders, priority order.
  voice_player = std::make_unique<audio::VoicePlayer>(mixer);
  audio::VoicePlayer::set_instance(voice_player.get());
  voice_player->set_search([this] {
    std::vector<std::filesystem::path> roots;
    for (const vfs::Mod& mod : mods.mods())
      if (mod.enabled) roots.push_back(mod.root.empty() ? mod.source : mod.root);
    return roots;
  });
  voice_player->set_packed_lookup([this](const std::filesystem::path& path) -> audio::VoicePlayer::PackedLine {
    const auto line = mods.packed_voice(path);
    if (!line) return {};
    return [this, file = *line] { return files.read_mod_file(file); };
  });
  apply_audio_config();
  if (!files.game_dir().empty()) mixer.set_vfs(&files);
  sfx_ok = !files.game_dir().empty() && !sweeping && sfx_bank.load(files);
  if (!sfx_ok && !sweeping) log::warn("audio: effect00.dat / wave0000.dat unavailable; sound effects are only logged");
  sfx_sink = sfx_ok ? static_cast<audio::Sfx*>(&akao_sfx) : static_cast<audio::Sfx*>(&log_sfx);
  ui_sfx = [this](int id) {
    // Menu sounds use the menus' mask; a new sound cuts the last one.
    if (sfx_ok) akao_sfx.play_menu(id);
  };
  // Scripted runs stay silent.
  if (!sweeping && config.debug_script.empty() && !mixer.open_device()) log::warn("audio: no output device, running silent");
  // OC_AUDIO_DUMP captures the mix of a scripted run, rendered per game tick (Mixer::capture_tick).
  if (const char* dump = std::getenv("OC_AUDIO_DUMP"); dump && *dump && !mixer.device_open())
    mixer.start_capture(dump);
}

// [Debug] Script / Record / Replay: scripted or recorded input instead of the keyboard.
bool App::init_routes() {
  routing = !config.debug_script.empty();
  replaying = !routing && !config.debug_replay.empty();
  shots_dir = config_file.parent_path() / "shots";
  if (routing) {
    std::string err;
    if (!route.load(config.debug_script, &err)) {
      log::error("[Debug] Script: {}", err);
      return false;
    }
    log::info("route: {} commands from {}", route.line_count(), config.debug_script);
    std::filesystem::create_directories(shots_dir);
  }
  if (replaying) {
    std::string err;
    if (!replayer.load(config.debug_replay, &err)) {
      log::error("[Debug] Replay: {}", err);
      return false;
    }
    config.debug_room = replayer.room();
    config.debug_entrance = replayer.entrance();
    log::info("replay: {} frames from {}", replayer.frames(), config.debug_replay);
  }

  // How the session starts: the title (default), a flow screen, or straight into a field room ([Debug] Room / Flow).
  {
    std::string fl = config.debug_flow;
    if (fl.empty()) fl = config.debug_room.empty() && !sweeping && !replaying && config.debug_battle < 0 ? "title" : "field";
    if (!game::parse_flow_start(fl, flow_start, flow_enabled)) log::warn("unknown [Debug] Flow '{}', starting at the title", fl);
    if (sweeping || replaying || config.debug_battle >= 0) flow_enabled = false;
  }
  dev_room = config.debug_room.empty() ? "alu_a01f" : config.debug_room;
  if (!replaying && !config.debug_record.empty() && recorder.open(config.debug_record, dev_room, config.debug_entrance))
    log::info("recording input to {}", config.debug_record);
  return true;
}

void App::init_flow() {
  if (!files.game_dir().empty() && !sweeping) tables_ok = tables.load(files);

  if (tables_ok) {
    flow_ready = flow.init(ctx, files, sprites, &tables, lang, &mixer);
    if (flow_ready) {
      flow.machine().set_seed(config.debug_seed);
      flow.set_art_mode(platform::art_is_hd(config.art_mode) ? ui::ArtMode::Hd : ui::ArtMode::Original);
      flow.set_sfx(ui_sfx);
      // The press-start chime: each sound on its own field sound slot (centre pan, full volume), so none cuts another.
      flow.title().set_layered_sfx([this](int id, int layer) {
        if (sfx_ok) akao_sfx.play_field(0x100 + id, 127, 64, 1u << (layer + 8));
      });
      // The title room's script plays no song (the title FMV carries the audio).
      flow.set_title_song(-1);
    } else {
      log::warn("game flow unavailable (title / name entry / continue)");
      flow_enabled = false;
    }
  } else {
    flow_enabled = false;
  }
}

// The save file for Continue and the menu's Save screen. The first write keeps a one-time backup next to it.
void App::init_saves() {
  save_path = save_dir.empty() ? std::filesystem::path() : save_dir / config.save_file;
  if (!save_path.empty() && !sweeping) {
    // RDE's boot: the Steam\<id> folder is created, then each card member that is missing or not 0x20000 bytes is formatted and
    // written, which creates save.dat with memcard1.mcr + memcard2.mcr on the first launch. A save.dat that does not open is left
    // alone (RDE's zip_open fails on it too).
    std::error_code ec;
    try {
      if (std::filesystem::exists(save_path, ec)) {
        save_dat = save::SaveDat::load(save_path);
        have_save_dat = true;
        log::info("save: {}", save_path.string());
        if (save_dat.ensure_cards()) {
          save_dat.save(save_path);
          log::info("save: formatted a missing memory card in {}", save_path.string());
        }
      } else {
        std::filesystem::create_directories(save_dir, ec);
        save_dat = save::SaveDat::create_empty();
        save_dat.save(save_path);
        have_save_dat = true;
        log::info("save: created {} (two formatted memory cards, as RDE's first boot)", save_path.string());
      }
    } catch (const std::exception& error) {
      log::warn("save {}: {}", save_path.string(), error.what());
    }
  }
  if (have_save_dat) {
    // The OpenCross page lives next to its save file: "OpenCross" for save.dat, "<name>_OpenCross" for another [Paths] SaveFile.
    const std::string stem = save_path.stem().string();
    oc_saves.set_dir(save_path.parent_path() / (stem == "save" ? std::string("OpenCross") : stem + "_OpenCross"));
    oc_saves.load();
  }
  if (flow_ready && have_save_dat) flow.set_save_dat(&save_dat, &oc_saves);
  // ext_save.dat: every slot write (Save menu, auto-save) also stores the language, the battle counter and the custom character
  // names; a load (GameFlow::tick_fade) restores them for RDE-compatible saves.
  ext_host.language = static_cast<u32>(lang);
  if (flow_ready) ext_host.names = &flow.machine().custom_names();
  save_dat.set_ext_host(&ext_host);
  oc_saves.set_ext_host(&ext_host);
}

void App::init_menus() {
  if (!flow_ready) return;
  const ui::ArtMode art = platform::art_is_hd(config.art_mode) ? ui::ArtMode::Hd : ui::ArtMode::Original;
  if (menu_text.init(ctx, sprites)) {
    menu_ok = menu.init(ctx, files, sprites, menu_text, tables, lang);
    if (menu_ok) {
      // A rotating 3D member on the Status screen.
      menu.set_model_view(&status_model);
      menu.set_rules(&flow.machine().rules());
      menu.set_mode(art);
      menu.set_portrait_set(config.portraits);
      menu.set_sfx(ui_sfx);
      if (have_save_dat) {
        std::filesystem::path bak = save_path;
        bak += ".ocbak";
        std::error_code ec;
        if (!std::filesystem::exists(bak, ec)) std::filesystem::copy_file(save_path, bak, ec);
        menu.set_save_target(&save_dat, save_path, &oc_saves);
      }
    }
    shop_ok = shop.init(ctx, files, sprites, menu_text, tables, lang);
    if (shop_ok) {
      shop.set_rules(&flow.machine().rules());
      shop.set_mode(art);
      shop.set_sfx(ui_sfx);
    }
    key_popup_ok = key_popup.init(ctx, files, sprites, menu_text);
    // RDE pause screen (Start / P): colour filter + PAUSE bar + Vibration / World Names rows.
    pause_ok = pause_screen.init(ctx, files, sprites, menu_text);
    feature_icons_ok = feature_icons.init(ctx, files, sprites);
    world_name_ok = world_name.init(ctx, files, sprites);
    // The Tele-Porter's "Current Party" screen.
    party_ok = party_screen.init(ctx, files, sprites, menu_text, lang);
    if (party_ok) {
      party_screen.set_portrait_set(config.portraits);
      party_screen.set_custom_names(&flow.machine().custom_names());
    }
    // The Grand Slam monster picker: its frames are the room's background group 1.
    monster_picker_ok = monster_picker.init(ctx, files, sprites, menu_text, lang);
    if (monster_picker_ok) {
      monster_picker.set_model_view(&picker_models);
      monster_picker.set_backdrop([this](render::SpriteBatch& batch, float vw, float vh) { world.draw_room(batch, vw, vh); });
    }
  }
  if (!menu_ok || !shop_ok) log::warn("menu {} / shop {} unavailable", menu_ok ? "ok" : "failed", shop_ok ? "ok" : "failed");
}

void App::init_field() {
  if (files.game_dir().empty() || !world.init(files, ctx, sprites, meshes, lang)) return;
  // No pickup panel: the room script shows its own "received" message window; only logged here.
  world.set_on_key_item([this](int item) {
    const std::string name = item < static_cast<int>(tables.key_item_names.size()) ? tables.key_item_names[static_cast<size_t>(item)] : std::string("?");
    log::info("key item {} obtained: {}", item, name);
  });
  world.set_on_item_given([this](int id) { achievements.item_given(id, world.vm().var(script::var::kMapId)); });
  world.set_on_var_bit_set([this](u32 bit_ref) { achievements.var_bit_set(bit_ref, world.vm().var(script::var::kMapId)); });
  world.set_art_mode(platform::art_is_hd(config.art_mode));
  world.set_portrait_set(config.portraits);
  if (!sweeping) world.set_audio(&mixer, sfx_sink);
  world.set_effects_gpu(!sweeping);
  // [Debug] Seed = 0: the field script RNG (RAND, e.g. the title's `RAND & 3` scene pick) takes a clock seed like the PS1's
  // rand() state, which depends on the frames elapsed before the call; a set seed keeps routes reproducible.
  world.set_seed(config.debug_seed ? config.debug_seed
                                   : static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFF) | 1u);
  world.set_bgm_remastered(config.bgm_source != "original");
  game::FieldDebug dbg;
  dbg.auto_walk = config.debug_autowalk;
  dbg.action_tick = config.debug_action_tick;
  dbg.target = config.debug_auto_target;
  dbg.auto_confirm = config.debug_auto_confirm;
  dbg.teleport = config.debug_auto_teleport;
  dbg.show_message = config.debug_show_message;
  dbg.show_choice = config.debug_show_choice;
  dbg.show_choice_first = config.debug_show_choice_first;
  dbg.show_face = config.debug_show_face;
  world.set_debug(dbg);
  if (flow_ready) {
    world.set_game_state(&flow.game_state());
    world.set_custom_names(&flow.machine().custom_names());
  }
  world_ok = true;
}

// [Debug] Battle=<scene>: start straight in a battle (development view) instead of the field.
void App::init_battle() {
  if (!files.game_dir().empty() && !sweeping && (config.debug_battle >= 0 || world_ok)) {
    if (!tables_ok) tables_ok = tables.load(files);
    battle_ready = tables_ok && battle.init(ctx, files, sprites, meshes, tables, &mixer);
    if (battle_ready) {
      battle.set_art_mode(platform::art_is_hd(config.art_mode));
      battle.set_bgm_remastered(config.bgm_source != "original");
      battle.set_auto(config.debug_battle_auto);
      battle.set_language(lang);
    } else {
      log::warn("battle view unavailable; field encounters will be skipped");
    }
    if (config.debug_battle >= 0) {
      game::BattleView::StartOptions options;
      options.battlefield = config.debug_showcase_stage;
      battle.set_showcase(config.debug_showcase);
      // [Debug] Seed makes the standalone fight repeatable unless the environment already pins OC_BATTLE_SEED.
      if (config.debug_seed && !std::getenv("OC_BATTLE_SEED")) _putenv_s("OC_BATTLE_SEED", std::to_string(config.debug_seed).c_str());
      battle_ok = battle_ready && battle.start(config.debug_battle, {}, options);
      if (battle_ok && config.debug_showcase) {
        // Front-on overview of the line-up (the characters face -x).
        game::free_cam().set_on(true);
        game::free_cam().init_from({-15000.0f, 3500.0f, 0.0f}, {0.0f, 600.0f, 0.0f});
      }
      if (!battle_ok) log::warn("[Debug] Battle={}: could not start", config.debug_battle);
    }
  }
  // Post-battle results screen: shown by the host after a won, fled or script-ended (code 5+) battle, before the rewards are written back and the field resumes.
  if (battle_ready) {
    results_ok = results.init(ctx, files, sprites, tables, lang);
    if (results_ok) {
      results.set_art_mode(platform::art_is_hd(config.art_mode));
      results.set_portrait_set(config.portraits);
      results.set_sfx(ui_sfx);
      results.set_on_exit([this] { battle.stop_music(); });
      results.set_auto(config.debug_battle_auto);
      battle.set_external_results(true);
    } else {
      log::warn("battle results screen unavailable; the battle view shows its own panel");
    }
  }
}

void App::init_title() {
  if (flow_ready) flow.set_title_scene(world_ok && config.debug_battle < 0 && !sweeping);
  // The title room's underwater 3D scene: D3 PLAY_EFFECT "tit0".."tit3" are sequences of the global title.prd.
  if (flow_ready && world_ok && !files.game_dir().empty() && config.debug_battle < 0 && !sweeping && title_bd.init(files, ctx, sprites, meshes)) {
    world.set_effect_tap([this](script::NamedOp op, std::string_view effect_name) {
      if (world.room_name() != game::kTitleRoom) return false;
      if (op == script::NamedOp::PlayEffect && effect_name.size() == 4 && effect_name.compare(0, 3, "tit") == 0) {
        ++title_plays;
        title_seq = std::string(effect_name);
        return title_bd.play(effect_name);
      }
      return false;
    });
  }
}

void App::init_start_state() {
  if (world_ok && config.debug_battle < 0) {
    if (flow_ready) {
      if (!flow_enabled) flow.machine().new_game(U"Serge", false);
      else if (*flow_start == game::FlowState::Opening) flow.machine().new_game(U"Serge");
      world.set_game_state(&flow.game_state());
    }
    if (sweeping) {
      world.load_room(dev_room, 0);
      // OC_SWEEP_ROOMS=a,b,c: sweep only those rooms; OC_SWEEP_FROM=<room>: start at that room; OC_SWEEP_COUNT=n: stop after n rooms
      // (softlock hunting on a few rooms).
      sweep_rooms = world.room_list();
      if (const char* only = std::getenv("OC_SWEEP_ROOMS"); only && *only) {
        std::vector<std::string> keep;
        const std::string list = std::string(",") + only + ",";
        for (const auto& room : sweep_rooms)
          if (list.find("," + room + ",") != std::string::npos) keep.push_back(room);
        sweep_rooms = std::move(keep);
      }
      if (const char* from = std::getenv("OC_SWEEP_FROM"); from && *from) {
        const auto it = std::find(sweep_rooms.begin(), sweep_rooms.end(), std::string(from));
        if (it != sweep_rooms.end()) sweep_first = static_cast<size_t>(it - sweep_rooms.begin());
      }
      if (!sweep_rooms.empty()) world.load_room(sweep_rooms[sweep_first], 0);
    } else if (!flow_enabled || !flow_ready) {
      if (flow_ready) flow.machine().start(game::FlowState::Field);
      world.reset_for_new_game();
      world_ok = world.load_room(dev_room, config.debug_entrance);
    } else if (*flow_start == game::FlowState::Opening) {
      world.reset_for_new_game();
      flow.start(game::FlowState::Opening);
      world_ok = world.load_room(game::kNewGameRoom, game::kNewGameParam);
    } else {
      flow.start(*flow_start);
      // RDE's boot logo: a fresh boot to the title shows ui/logo.png before the core runs at all. Not for scripted
      // routes / replays / [Debug] Screenshot runs (their tick counts start at the title).
      if (*flow_start == game::FlowState::Title && !routing && !replaying && !config.debug_screenshot) flow.begin_boot_logo();
    }
  }

  screenshot_pending = config.debug_screenshot;
  screenshot_tick = config.debug_screenshot_tick >= 0
                        ? static_cast<u64>(config.debug_screenshot_tick)
                        : (config.debug_autowalk ? static_cast<u64>(config.debug_action_tick + 90) : 40u);
  screenshot_name = config.debug_autowalk ? "screenshot_dialogue.png" : "screenshot.png";
  sweep_idx = sweep_first;
}

}  // namespace oc::app
