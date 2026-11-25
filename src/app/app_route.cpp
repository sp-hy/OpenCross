// Scripted input instead of the keyboard: [Debug] Script routes (one command per tick, assertions, screenshots), [Debug] Replay of a
// recorded input file, and the room sweep ([Debug] SweepTicks: every room's scripts for N ticks, counting the rooms that fail).
#include "app/app.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>

#include "core/log.h"
#include "data/table_rows.h"
#include "game/game_session.h"
#include "script/field_host.h"

namespace oc::app {

namespace {

// Route `remote`: the test remote parties of the field (FieldWorld::route_remote_parties). Every change is a new pose_seq, as a network
// message would be; a move keeps the party's state. False when the named party does not exist.
bool apply_route_remote(game::FieldWorld& world, const game::RouteOut& out) {
  std::vector<net::RemoteParty>& parties = world.route_remote_parties();
  const net::RemoteParty& given = out.remote_party;
  if (out.remote == "clear") {
    parties.clear();
    return true;
  }
  const auto it = std::find_if(parties.begin(), parties.end(), [&](const net::RemoteParty& party) { return party.pid == given.pid; });
  if (out.remote == "add") {
    net::RemoteParty party = given;
    party.has_pose = true;
    party.pose_seq = it != parties.end() ? it->pose_seq + 1 : 1;
    if (it != parties.end()) *it = party;
    else parties.push_back(party);
    return true;
  }
  if (it == parties.end()) return false;
  if (out.remote == "move") {
    const net::PresenceState state = it->pose.state;
    it->pose = given.pose;
    it->pose.state = state;
  } else {
    it->pose.state = given.pose.state;
  }
  ++it->pose_seq;
  return true;
}

}  // namespace

void App::init_route_state() {
  rstate.ent_xz = [this](int ent, float& x, float& z) { return world.entity_xz(ent, x, z); };
  rstate.ent_facing = [this](int ent) { return world.entity_facing(ent); };
  rstate.ent_yaw = [this](int ent) { return world.entity_yaw(ent); };
  rstate.ent_head = [this](int ent, int& ax, int& az) { return world.entity_head(ent, ax, az); };
  rstate.ent_clip = [this](int ent, bool src) { return world.entity_clip(ent, src); };
  rstate.party_ent = [this](int slot) { return world.party_entity(slot); };
  rstate.path = [this](float x0, float z0, float x1, float z1, bool smooth) { return world.find_path(x0, z0, x1, z1, smooth); };
  rstate.var = [this](int id) { return world.vm().var(static_cast<u16>(id)); };
  rstate.item_count = [this](int id) { return world.item_count(id); };
  rstate.char_stat = [this](int id, int what) { return world.debug_char_stat(id, what); };
  rstate.set_char_stat = [this](int id, int what, int value) { world.debug_set_char_stat(id, what, value); };
  rstate.remote_drawn = [this] { return world.remote_drawn(); };
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
    for (const int id : world.game_state().slot.party())
      party_text += (party_text.empty() ? "" : ",") + (id < 0 ? std::string("-") : std::to_string(id));
    return party_text;
  };
  rstate.disc = [this] { return world.disc(); };
  rstate.location = [this] {
    world.capture_state();
    return static_cast<int>(world.game_state().slot.location());
  };
  rstate.song = [this] { return mixer.sounding() ? mixer.current_song() : -1; };
  rstate.gold = [this] { return static_cast<long long>(world.game_state().gold()); };
  rstate.options = [this] { return static_cast<int>(world.game_state().options()); };
  rstate.config_value = [this](const std::string& section, const std::string& key) { return config.to_ini().get(section, key); };
  rstate.battle_events = [this] { return battle_ready ? battle.route_events() : std::vector<std::string>(); };
  rstate.battle_camera = [this] { return battle_ready ? battle.route_camera() : std::string(); };
  rstate.battle_menu = [this] { return battle_ready ? battle.route_menu() : std::string(); };
  rstate.battle_facing = [this] { return battle_ready ? battle.route_facing() : std::string(); };
  rstate.battle_hit_box = [this] { return battle_ready ? battle.route_hit_box() : std::string(); };
  rstate.battle_plate = [this] { return battle_ready ? battle.route_plate() : std::string(); };
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
  const std::pair<s32, s32> scroll = field_visible() ? world.camera_position() : std::pair<s32, s32>{0, 0};
  rstate.scroll_x = scroll.first;
  rstate.scroll_y = scroll.second;
  rstate.dialogue = field_visible() && world.dialogue_is_open();
  rstate.dialogue_choice = field_visible() && world.dialogue_choice_pending();
  rstate.portrait = field_visible() ? world.dialogue_portrait_side() : "none";
  rstate.message = field_visible() ? world.last_dialogue_message() : -1;
  rstate.choice_line = field_visible() ? world.dialogue_choice_line() : -1;
  rstate.battle = battle_in_field || battle_ok;
  rstate.battle_idle = battle_ready && (battle_in_field || battle_ok) && battle.route_idle();
  rstate.battle_actions = battle_ready ? battle.debug_actions() : 0;
  // The results screen is up, or a versus battle's outcome plate (route: wait battle results).
  rstate.battle_finished = (results_ok && results.active()) || (battle_ready && battle_in_field && battle.versus_plate());
  rstate.settling = (world_ok && world.input_settling()) || (flow_ready && flow.veiled());
  rstate.field_idle = field_visible() && world.idle() && world.fade_level() < 0.05f && !movie_open && !(menu_ok && !menu.closed()) && !shop_open && !(key_popup_ok && key_popup.active()) && !(pause_ok && pause_screen.active()) && !dialogue_log_open && !(party_ok && party_screen.active()) && !(monster_picker_ok && monster_picker.active());
  rstate.fade = world_ok ? world.fade_level() : 0.0f;
  rstate.battle_result = field_visible() ? world.battle_result() : 0;
  rstate.flow = flow_ready && flow_enabled ? game::flow_state_name(flow.state()) : "field";
  rstate.movie = movie_open;
  rstate.menu = (menu_ok && !menu.closed()) || (flow_ready && (flow.rename_active() || flow.party_active() || flow.ng_plus_active())) || (party_ok && party_screen.active()) ||
               (monster_picker_ok && monster_picker.active());
  rstate.shop = shop_open;
  rstate.guide_page = flow_ready ? flow.guide_page() : 0;
  rstate.records_ready = records_info.has_value();
  rstate.pause = pause_ok && pause_screen.active();
  const bool log_page = menu_ok && !menu.closed() && menu.screen() == ui::MenuScreen::OcPage && dialogue_log_view.active();
  rstate.dialogue_log = dialogue_log_open || log_page ? static_cast<int>(dialogue_log_view.entry_count()) : -1;
  rstate.dialogue_log_cursor = dialogue_log_view.cursor();
  rstate.retry = retry_choosing;
  rstate.feature_icons = feature_icons_ok ? feature_icons.shown() : 0;
  rstate.field_ticks = world_ok ? world.tick_count() : 0;
  rstate.room_loads = world.room_loads();
  rstate.art_swaps = world.art_swaps();
  rstate.battle_starts = battle_starts;
  rstate.title_plays = title_plays;
  rstate.title_seq = title_seq;
  // Still fading in / out.
  if (field_visible() && world.fade_level() > 0.05f) rstate.room_valid = false;
}

// Route `request`: rename selects the character, then name entry; party_add adds a character and rebinds the party; party_remove
// empties the slot of a character reference (REMOVE_FROM_PARTY); item gives a field-script item id (BB's ranges); money sets the
// gold; save is the ending's cleared-save prompt (strips the roster, variable 0x7E = 3, save list); ngplus is the New Game+ title list
// over the current game; slotdump writes the game as one 0x2000 save block to $OC_SLOT_DUMP<N>.bin; savemark remembers that block (play
// time left out) and savecheck fails the run when the game's block differs from it; battle starts an encounter id on
// battlefield 0 with the session party; battle_disguise is the Dragoon disguise fight (jac_b25i); versus starts a versus battle (seed, side)
// against the local opponent.
void App::route_request(const game::RouteOut& out) {
  using MK = script::MenuKind;
  const std::string& rk = out.request;
  if (rk == "rename") world.request_menu(MK::NameEntry, out.request_arg);
  else if (rk == "party") world.request_menu(MK::PartyScreen, 0);
  // The character id as a script of the running room names it (a mod's own id through the room's owner), or a character mods add by
  // its name (any room).
  else if (rk == "party_add") {
    const std::string& text = out.request_text;
    const bool named = !text.empty() && !std::isdigit(static_cast<unsigned char>(text[0])) && text[0] != '-';
    const std::optional<size_t> by_name = named ? data::table_rows().id_of("char", text) : std::nullopt;
    const int character = named ? (by_name ? static_cast<int>(*by_name) : -1) : world.script_character(out.request_arg);
    if (!world.party_add(character))
      log::warn("route: party_add {}: not added (no such character here, already in the party or the party is full)", text);
    world.state_edited();
  }
  // script <ent> <slot> [class]: start that entity's script slot as a spawn would (FieldVM::request_script; class 3 = a talk's): an
  // editor runs any one script of a room, not only the ones a talk or a touch starts; one live already is the asked-for one
  else if (rk == "script") world.vm().request_script(out.request_arg, out.request_arg2, out.request_arg3 > 0 ? out.request_arg3 : 3);
  else if (rk == "party_remove") {
    const int ref = out.request_arg;
    world.party_remove(ref >= 0xFD && ref <= 0xFF ? world.party_member(ref - 0xFD) : world.script_character(ref));
    world.state_edited();
  }
  else if (rk == "money") world.set_money(out.request_arg);
  else if (rk == "keyitem") { world.take_item(0x400 + out.request_arg); world.give_item(0x400 + out.request_arg); }
  else if (rk == "item") world.give_item(out.request_arg);
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
  else if (rk == "savemark") route_save_mark = route_save_block();
  else if (rk == "savecheck") {
    const Bytes now = route_save_block();
    const auto differ = std::mismatch(now.begin(), now.end(), route_save_mark.begin(), route_save_mark.end());
    if (route_save_mark.empty() || differ.first != now.end() || differ.second != route_save_mark.end()) {
      log::error("route: savecheck: the save differs from the mark at byte {:#x}", static_cast<size_t>(differ.first - now.begin()));
      exit_code = 1;
      route_finished = true;
    } else {
      log::info("route: savecheck: the save is unchanged ({} bytes)", now.size());
    }
  }
  else if (rk == "versus") {
    if (!start_local_versus(static_cast<u32>(out.request_arg), out.request_arg2)) {
      log::error("route: the versus battle could not start");
      exit_code = 1;
      route_finished = true;
    }
  }
  else if (rk == "menu") world.request_menu(MK::MainMenu, 0);
  else if (rk == "disc") world.request_menu(MK::ChangeDisc, out.request_arg);
  else if (rk == "movie") world.play_movie(out.request_arg, out.request_arg2);
  else if (rk == "battle") world.request_engine_battle(out.request_arg, out.request_arg2, out.request_arg3);
  else if (rk == "battle_disguise") world.request_engine_battle(out.request_arg, 0, 4);
  else if (rk == "trading") world.request_engine_menu(MK::TradingPost, out.request_arg);
  else if (rk == "elementshop") world.request_engine_menu(MK::ElementShop, out.request_arg);
  else if (rk == "weaponshop") world.request_engine_menu(MK::WeaponShop, out.request_arg);
  else {
    log::error("route: unknown request {}", rk);
    exit_code = 1;
    route_finished = true;
  }
}

// The play time and the variables that show it (0x2C hours, 0x2E minutes, 0x30 seconds) are left out.
Bytes App::route_save_block() {
  world.capture_state();
  save::SaveSlot slot = world.game_state().slot;
  slot.set_playtime_frames(0);
  for (const int id : {0x2C, 0x2E, 0x30}) slot.set_var(id, 0);
  return save::encode_block(slot);
}

// Scripted input: fixed ticks per frame, one route step per tick.
void App::route_frame() {
  if (controlled) {
    control_frame();
    return;
  }
  for (int k = 0; k < config.debug_script_speed && !route_finished; ++k)
    if (route_step()) break;
  step = {0, 0.0f};
}

// A controlled run goes on ticking with no input once its route has ended (the scene plays out).
bool App::route_step() {
  platform::Input in;
  game::RouteOut out;
  fill_route_state();
  if (!(controlled && route_finished)) {
    route.step(rstate, in, out);
    pad_remap.apply(in, platform::active_map());
    if (out.battle_auto && battle_ready) battle.set_auto(true);
    if (out.battle_auto && results_ok) results.set_auto(true);
    // Battle harness (`cast`, `attack`, `set invincible`): written now, so the next tick of a seek sees it.
    if (battle_ready && (out.battle_cast > 0 || out.battle_weapon >= 0 || out.invincible_party >= 0)) {
      game::BattleView::Debug debug = battle.debug();
      if (out.battle_cast > 0) {
        debug.cast_element = out.battle_cast;
        // Both or neither: the view's explicit caster / target path, else PC `lead` on the first enemy.
        const bool pair = out.battle_caster >= 0 && out.battle_target >= 0;
        debug.cast_actor = pair ? out.battle_caster : -1;
        debug.cast_target = pair ? out.battle_target : -1;
        debug.lead = !pair && out.battle_caster >= 0 ? out.battle_caster : 0;
      }
      if (out.battle_weapon >= 0) debug.weapon = out.battle_weapon, debug.lead = std::max(0, out.battle_caster);
      if (out.invincible_party >= 0) {
        debug.inv_party = out.invincible_party != 0, debug.inv_enemies = out.invincible_enemies != 0;
        debug_tools.set_invincible(debug.inv_party, debug.inv_enemies);
      }
      battle.set_debug(debug);
    }
    // Battle harness `set battle hp` / `set critical` / `set hit`: the battle's outcome set up before an action (a previewer's Low HP / Dies /
    // Critical / Miss).
    if (battle_ready && out.hp_unit >= 0 && !battle.debug_set_hp(out.hp_unit, out.hp_value, out.hp_percent))
      log::warn("route: set battle hp: no unit {} in the battle", out.hp_unit);
    if (battle_ready && out.critical >= 0 && battle.battle_mut()) battle.battle_mut()->set_force_crit(out.critical != 0);
    if (battle_ready && out.hit >= 0 && battle.battle_mut()) battle.battle_mut()->set_force_hit(out.hit);
    if (out.dump) log::info("route: tick {} entities of {}:\n{}", route_tick, rstate.room, world.dump_entities());
    // Route `art`: the F6 path (not saved to the ini).
    if (!out.art.empty()) {
      const bool hd = out.art == "toggle" ? !platform::art_is_hd(config.art_mode) : out.art == "hd" || out.art == "modded";
      config.art_mode = !hd ? platform::ArtMode::Original : out.art == "modded" ? platform::ArtMode::Modded : platform::ArtMode::Hd;
      apply_art_mode();
    }
    if (out.settings == "close") ui.options_open = false;
    else if (!out.settings.empty()) ui.open(out.settings);
    if (!out.request.empty()) route_request(out);
    if (!out.remote.empty() && !apply_route_remote(world, out)) {
      log::error("route: no remote party {}", out.remote_party.pid);
      exit_code = 1;
      route_finished = true;
    }
    if (out.put && out.put_ent >= 0)
      world.set_position_xz(out.put_ent, static_cast<s32>(out.put_x), static_cast<s32>(out.put_z), 0);
    else if (out.put)
      for (int slot = 0; slot < (out.put_v ? 1 : 3); ++slot)
        if (const int pe = world.party_entity(slot); pe >= 0) world.set_position_xz(pe, static_cast<s32>(out.put_x), static_cast<s32>(out.put_z), 0);
    if (out.put && out.put_ent < 0 && out.put_v) world.place_followers();
    if (out.put_beside >= 0) {
      float bx = 0, bz = 0;
      const bool placed = world.put_party_beside(out.put_beside, out.put_dist, bx, bz);
      if (placed)
        log::info("route: put beside ent {}: the party at {:.0f},{:.0f}", out.put_beside, bx, bz);
      else
        log::warn("route: put beside ent {}: no walkable spot beside it (the party stays)", out.put_beside);
      // `talk`: only when the field takes a talk (no transition, fade, modal window, input lock or other talk running), as for the
      // player; then the asked-for entity's talk itself, as the trigger pass starts it (not a press: a press goes to the first entity
      // in range in index order, which may be another one beside it). An entity the player can't talk to here (nowhere to stand on its
      // floor, or no talk now) gets its talk script started directly. A trace notes every outcome but a talk the trigger pass
      // would start (`k <tick> <entity> busy|nospot|notalk|absent|none`).
      auto note_talk = [&](const char* why) {
        if (auto* trace = world.vm().trace()) trace->notes.push_back(std::format("k {} {} {}", trace->tick, out.put_beside, why));
      };
      if (out.talk && !world.idle()) {
        note_talk("busy");
        log::warn("route: talk ent {}: the field takes no talk now (a script, window or fade holds it); nothing started", out.put_beside);
      } else if (out.talk && placed && world.takes_talk(out.put_beside)) {
        world.vm().trigger_action(out.put_beside);
      } else if (out.talk) {
        // Not in the room in this run (inactive: shown only by another scene, or hidden by one): brought in and shown, so its talk
        // script runs and the editor sees what it does; put back when it has none.
        const bool exists = static_cast<size_t>(out.put_beside) < world.vm().entity_count();
        const u32 flags = exists ? world.vm().entity(out.put_beside).flags38 : 0;
        const bool absent = world.bring_in(out.put_beside);
        const char* why = absent ? "absent" : placed ? "notalk" : "nospot";
        if (world.vm().request_script(out.put_beside, 2, 3)) {
          note_talk(why);
          log::info("route: talk ent {}: {}, its talk script started directly", out.put_beside, why);
        } else {
          if (absent) world.vm().entity(out.put_beside).flags38 = flags;
          note_talk("none");
          log::warn("route: talk ent {}: {}, and it has no talk script to start", out.put_beside, why);
        }
      }
    }
    // The party moved by the route: the camera on the leader, as a walk there would have left it.
    if ((out.put && out.put_ent < 0) || out.put_beside >= 0) world.settle_camera_on_player();
    if (out.set_buttons) world.game_state().slot.button_cfg[static_cast<size_t>(out.buttons_index)] = static_cast<u32>(out.buttons_value);
    if (out.set_option) world.game_state().set_option(static_cast<u16>(out.option_mask), out.option_on != 0);
    if (out.set_skin) {
      world.game_state().skin = static_cast<u8>(out.skin);
      world.game_state().set_option(ui::opt::kCustomWindow, true);
      world.vm().set_var(script::var::kCurrentFrame, out.skin);
    }
    if (out.set_group) world.bg_layer(out.group, out.group_on != 0);
    if (out.set_var) world.vm().set_var(static_cast<u16>(out.var_id), out.var_value);
    // A warp takes the disc from the story variable 0x42 like a map jump; a room only the other disc holds takes that disc.
    if (!out.load_room.empty()) {
      world.set_disc(world.vm().var(0x42) + 1);
      world.select_disc_for_room(out.load_room);
    }
    if (!out.load_room.empty() && !world.load_room(out.load_room, out.load_param)) {
      log::error("route: load {} failed", out.load_room);
      exit_code = 1;
      route_finished = true;
    }
    if (!out.screenshot.empty()) {
      std::filesystem::path shot = out.screenshot;
      if (shot.is_relative()) shot = shots_dir / shot;
      ctx.request_screenshot(shot);
      if (movie_open)
        log::info("route: tick {} screenshot {} (room {}, movie frame {})", route_tick, shot.string(), rstate.room, movie_player.frame_index());
      else
        log::info("route: tick {} screenshot {} (room {})", route_tick, shot.string(), rstate.room);
    }
    if (out.measure_sides)
      ctx.request_capture([this](std::vector<u8>&& pixels, u32 width, u32 height) {
        if (!game::side_columns_lit(pixels.data(), width, height, rstate.side_lit[0], rstate.side_lit[1])) rstate.side_lit[0] = rstate.side_lit[1] = -1.0f;
        ++rstate.side_frames;
      });
    if (out.failed) {
      log::error("route FAILED at tick {}: {}\nentities:\n{}", route_tick, out.message, field_visible() ? world.dump_entities() : std::string("(no room)"));
      if (controlled) control_reply(std::format("failed {} {}", route_tick, out.message));
      else ctx.request_screenshot(shots_dir / "route_failed.png");
      exit_code = 1;
    }
    if (controlled && !out.note.empty()) control_reply(std::format("log {} {}", route_tick, out.note));
    if (out.done) route_finished = true;
  }
  update_loop_speed(1.0 / 60.0);
  loop_speed.field = loop_speed.field_factor == 1.0f ? platform::FixedStep::Result{1, 0.0f} : field_stepper.advance(loop_speed.field_factor / 60.0);
  advance(in, 1.0 / 60.0, 1);
  if (control_anim_playing) control_anim_step();
  mixer.capture_tick(1.0 / 60.0);
  if (recorder.active()) recorder.write(1.0 / 60.0, 1, in);
  if (battle_ready) {
    const int phase = battle.action_phase();
    if (phase != 0 && act_phase == 0) act_begun = route_tick;
    act_phase = phase;
  }
  ++route_tick;
  // Render this tick before the next command.
  return !out.screenshot.empty() || out.measure_sides || route_finished;
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
  if (config.debug_sweep_talk > 0) world.sweep_talk(config.debug_sweep_talk, [this] { ctx.drain_releases(); });
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
  ctx.drain_releases();
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
