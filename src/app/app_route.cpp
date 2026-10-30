// Scripted input instead of the keyboard: [Debug] Script routes (one command per tick, assertions, screenshots), [Debug] Replay of a
// recorded input file, and the room sweep ([Debug] SweepTicks: every room's scripts for N ticks, counting the rooms that fail).
#include "app/app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>

#include "core/log.h"
#include "game/game_session.h"
#include "script/field_host.h"

namespace oc::app {

void App::init_route_state() {
  rstate.ent_xz = [this](int ent, float& x, float& z) { return world.entity_xz(ent, x, z); };
  rstate.ent_facing = [this](int ent) { return world.entity_facing(ent); };
  rstate.ent_yaw = [this](int ent) { return world.entity_yaw(ent); };
  rstate.ent_head = [this](int ent, int& ax, int& az) { return world.entity_head(ent, ax, az); };
  rstate.ent_clip = [this](int ent, bool src) { return world.entity_clip(ent, src); };
  rstate.party_ent = [this](int slot) { return world.party_entity(slot); };
  rstate.path = [this](float x0, float z0, float x1, float z1, bool smooth) { return world.find_path(x0, z0, x1, z1, smooth); };
  rstate.var = [this](int id) { return world.vm().var(static_cast<u16>(id)); };
  rstate.char_stat = [this](int id, int what) { return world.debug_char_stat(id, what); };
  rstate.set_char_stat = [this](int id, int what, int value) { world.debug_set_char_stat(id, what, value); };
  rstate.dialogue_text = [this] { return world.dialogue_text(); };
  rstate.dialogue_progress = [this] { return world.dialogue_progress(); };
  rstate.char_name = [this](int id) {
    if (!flow_ready) return std::string();
    const auto& cn = flow.machine().custom_names();
    const auto it = cn.find(id);
    return it == cn.end() ? std::string() : game::utf8_from_u32(it->second);
  };
  rstate.party = [this] {
    std::string party_text;
    for (u8 id : world.game_state().slot.party())
      party_text += (party_text.empty() ? "" : ",") + (id == 0xFF ? std::string("-") : std::to_string(id));
    return party_text;
  };
  rstate.disc = [this] { return world.disc(); };
  rstate.location = [this] {
    world.capture_state();
    return static_cast<int>(world.game_state().slot.location());
  };
  rstate.battle_events = [this] { return battle_ready ? battle.route_events() : std::vector<std::string>(); };
  // For route `snap` / `assert same`: what an art switch must not change in a battle.
  rstate.battle_sig = [this] {
    const battle::Battle* active_battle = battle_ready ? battle.battle() : nullptr;
    if (!active_battle) return std::string();
    std::string signature = std::format("scene {} outcome {}", battle_scene, static_cast<int>(battle.outcome()));
    for (int i = 0; i < battle::kUnits; ++i)
      if (active_battle->unit(i).present) signature += std::format(" u{} hp {}", i, active_battle->unit(i).hp);
    return signature;
  };
}

void App::fill_route_state() {
  rstate.tick = route_tick;
  rstate.room = field_visible() ? world.room_name() : std::string();
  rstate.earned_achievements = &earned_achievements;
  rstate.title_hold = title_hold();
  rstate.room_valid = field_visible() && !world.room_name().empty();
  rstate.have_player = field_visible() && world.player_xz(rstate.px, rstate.pz);
  rstate.control_yaw = field_visible() ? world.control_yaw() : 0;
  rstate.dialogue = field_visible() && world.dialogue_is_open();
  rstate.dialogue_choice = field_visible() && world.dialogue_choice_pending();
  rstate.choice_line = field_visible() ? world.dialogue_choice_line() : -1;
  rstate.battle = battle_in_field || battle_ok;
  // The results screen is up (route: wait battle results).
  rstate.battle_finished = results_ok && results.active();
  rstate.field_idle = field_visible() && world.idle() && world.fade_level() < 0.05f && !movie_open && !(menu_ok && !menu.closed()) && !shop_open && !(key_popup_ok && key_popup.active()) && !(pause_ok && pause_screen.active()) && !(party_ok && party_screen.active()) && !(monster_picker_ok && monster_picker.active());
  rstate.battle_result = field_visible() ? world.battle_result() : 0;
  rstate.flow = flow_ready && flow_enabled ? game::flow_state_name(flow.state()) : "field";
  rstate.movie = movie_open;
  rstate.menu = (menu_ok && !menu.closed()) || (flow_ready && (flow.rename_active() || flow.party_active() || flow.ng_plus_active())) || (party_ok && party_screen.active()) ||
               (monster_picker_ok && monster_picker.active());
  rstate.shop = shop_open;
  rstate.guide_page = flow_ready ? flow.guide_page() : 0;
  rstate.pause = pause_ok && pause_screen.active();
  rstate.field_ticks = world_ok ? world.tick_count() : 0;
  rstate.room_loads = world.room_loads();
  rstate.art_swaps = world.art_swaps();
  rstate.battle_starts = battle_starts;
  rstate.title_plays = title_plays;
  rstate.title_seq = title_seq;
  // Still fading in / out.
  if (field_visible() && world.fade_level() > 0.05f) rstate.room_valid = false;
}

// Route `request`: rename selects the character, then name entry; party_add adds a character and rebinds the party; money sets the
// gold; save is the ending's cleared-save prompt (strips the roster, variable 0x7E = 3, save list); ngplus is the New Game+ title list
// over the current game; slotdump writes the game as one 0x2000 save block to $OC_SLOT_DUMP<N>.bin; battle starts an encounter id on
// battlefield 0 with the session party; battle_disguise is the Dragoon disguise fight (jac_b25i).
void App::route_request(const game::RouteOut& out) {
  using MK = script::MenuKind;
  const std::string& rk = out.request;
  if (rk == "rename") world.request_menu(MK::NameEntry, out.request_arg);
  else if (rk == "party") world.request_menu(MK::PartyScreen, 0);
  else if (rk == "party_add") { world.party_add(out.request_arg); world.state_edited(); }
  else if (rk == "money") world.set_money(out.request_arg);
  else if (rk == "keyitem") { world.take_item(0x400 + out.request_arg); world.give_item(0x400 + out.request_arg); }
  else if (rk == "save") world.request_menu(MK::Save, 0);
  else if (rk == "ngplus") world.request_menu(MK::NewGamePlus, 0);
  else if (rk == "restart") restart_requested = true;
  else if (rk == "slotdump") {
    world.capture_state();
    const char* dump_prefix = std::getenv("OC_SLOT_DUMP");
    const std::string path = dump_prefix ? std::string(dump_prefix) + std::to_string(out.request_arg) + ".bin" : std::string();
    const Bytes block = save::encode_block(world.game_state().slot);
    if (std::FILE* file = dump_prefix ? std::fopen(path.c_str(), "wb") : nullptr) {
      std::fwrite(block.data(), 1, block.size(), file);
      std::fclose(file);
      log::info("route: slot dump {} ({} bytes)", path, block.size());
    } else {
      log::error("route: slotdump needs OC_SLOT_DUMP");
    }
  }
  else if (rk == "menu") world.request_menu(MK::MainMenu, 0);
  else if (rk == "disc") world.request_menu(MK::ChangeDisc, out.request_arg);
  else if (rk == "movie") world.play_movie(out.request_arg, 0, true);
  else if (rk == "battle") world.request_battle(out.request_arg, 0, 0);
  else if (rk == "battle_disguise") world.request_battle(out.request_arg, 0, 4);
  else if (rk == "trading") world.request_menu(MK::TradingPost, out.request_arg);
  else {
    log::error("route: unknown request {}", rk);
    exit_code = 1;
    route_finished = true;
  }
}

// Scripted input: fixed ticks per frame, one route step per tick.
void App::route_frame() {
  for (int k = 0; k < config.debug_script_speed && !route_finished; ++k) {
    platform::Input in;
    game::RouteOut out;
    fill_route_state();
    route.step(rstate, in, out);
    if (out.battle_auto && battle_ready) battle.set_auto(true);
    if (out.battle_auto && results_ok) results.set_auto(true);
    if (out.dump) log::info("route: tick {} entities of {}:\n{}", route_tick, rstate.room, world.dump_entities());
    // Route `art`: the F6 path (not saved to the ini).
    if (!out.art.empty()) {
      const bool hd = out.art == "toggle" ? !platform::art_is_hd(config.art_mode) : out.art == "hd" || out.art == "modded";
      config.art_mode = !hd ? platform::ArtMode::Original : out.art == "modded" ? platform::ArtMode::Modded : platform::ArtMode::Hd;
      apply_art_mode();
    }
    if (!out.request.empty()) route_request(out);
    if (out.put)
      for (int slot = 0; slot < 3; ++slot)
        if (const int pe = world.party_entity(slot); pe >= 0) world.set_position_xz(pe, static_cast<s32>(out.put_x), static_cast<s32>(out.put_z), 0);
    if (out.set_option) world.game_state().set_option(static_cast<u16>(out.option_mask), out.option_on != 0);
    if (out.set_var) world.vm().set_var(static_cast<u16>(out.var_id), out.var_value);
    if (!out.load_room.empty() && !world.load_room(out.load_room, out.load_param)) {
      log::error("route: load {} failed", out.load_room);
      exit_code = 1;
      route_finished = true;
    }
    if (!out.screenshot.empty()) {
      std::filesystem::path shot = out.screenshot;
      if (shot.is_relative()) shot = shots_dir / shot;
      ctx.request_screenshot(shot);
      log::info("route: tick {} screenshot {} (room {})", route_tick, shot.string(), rstate.room);
    }
    if (out.failed) {
      log::error("route FAILED at tick {}: {}\nentities:\n{}", route_tick, out.message, field_visible() ? world.dump_entities() : std::string("(no room)"));
      ctx.request_screenshot(shots_dir / "route_failed.png");
      exit_code = 1;
    }
    if (out.done) route_finished = true;
    advance(in, 1.0 / 60.0, 1);
    mixer.capture_tick(1.0 / 60.0);
    if (recorder.active()) recorder.write(1.0 / 60.0, 1, in);
    ++route_tick;
    // Render this tick before the next command.
    if (!out.screenshot.empty() || route_finished) break;
  }
  step = {0, 0.0f};
}

void App::replay_frame() {
  game::ReplayFrame frame;
  if (!replayer.next(frame)) {
    log::info("replay finished after {} ticks", world_ok ? world.tick_count() : 0);
    route_finished = true;
  } else {
    input = frame.in;
    frame_dt = frame.dt;
    step = {frame.ticks, 0.0f};
    advance(input, frame_dt, frame.ticks);
  }
}

// Test hook: visit every room, run its scripts for N ticks in one go, count the rooms that fail to load.
bool App::sweep_frame() {
  world.update(input, config.debug_sweep_ticks);
  if (config.debug_sweep_talk > 0) world.sweep_talk(config.debug_sweep_talk);
  // No battle view in the sweep: every encounter is "won" so the scripts keep running.
  if (world.battle_ready()) {
    world.take_battle();
    world.end_battle(0);
  }
  // No movie player either: the FMV is over at once.
  if (world.movie_pending()) {
    world.take_movie();
    world.movie_finished();
  }
  if (world.menu_pending()) {
    world.take_menu();
    world.menu_closed();
  }
  {
    int off_ent = -1;
    const int off = world.worst_npc_off_mesh(off_ent);
    if (off > 0) {
      ++sweep_npc_rooms;
      log::info("sweep: npc off the walk mesh: room {} entity {} for {} ticks", world.view().room_name(), off_ent, off);
    }
  }
  const auto& rooms = sweep_rooms;
  static const size_t sweep_count = std::getenv("OC_SWEEP_COUNT") ? static_cast<size_t>(std::max(1, std::atoi(std::getenv("OC_SWEEP_COUNT")))) : size_t{1} << 30;
  if (++sweep_idx >= rooms.size() || sweep_idx - sweep_first >= sweep_count) {
    log::info("sweep: {} rooms with NPCs standing off the walk mesh", sweep_npc_rooms);
    for (const auto& [name, hit_count] : world.stub_hits()) log::info("sweep: stubbed host call {} x{}", name, hit_count);
    log::info("sweep: vm instructions {}, invalid pc {}, INIT overruns {}, unimplemented {}, unknown ops {}",
              world.vm().stats().instructions, world.vm().stats().invalid_pc, world.vm().stats().init_overruns,
              world.vm().stats().unimplemented, world.vm().stats().unknown_ops.size());
    log::info("sweep: {} rooms with layer problems", sweep_layer_problems);
    log::info("sweep: {} rooms visited, {} failed to load, {} validation messages", rooms.size(), sweep_failed,
              render::GpuContext::validation_message_count());
    return false;
  }
  // OC_SWEEP_SCENARIO=n: the story counter (variable 0) every swept room is entered with (reaches the story-gated branches).
  if (const char* sc = std::getenv("OC_SWEEP_SCENARIO"); sc && *sc) world.vm().set_var(0, std::atoi(sc));
  if (!world.load_room(rooms[sweep_idx], 0)) {
    ++sweep_failed;
    log::warn("sweep: {} failed", rooms[sweep_idx]);
  } else {
    std::string note;
    if (const std::string lr = world.layer_report(&note); !lr.empty()) {
      ++sweep_layer_problems;
      log::warn("sweep: {}: {}", rooms[sweep_idx], lr);
    }
    if (!note.empty()) log::info("sweep: note {}: {}", rooms[sweep_idx], note);
  }
  return true;
}

}  // namespace oc::app
