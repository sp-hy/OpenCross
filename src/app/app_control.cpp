// A route run another program drives over stdin / stdout ([Debug] Control): commands come in as lines, the run advances to the tick
// asked for without drawing the ticks between, and the frame of that tick is written to a file. An earlier tick restarts the run in
// the same process, so a controller can scrub back and forth through a scene.
#include "app/app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include "core/log.h"
#include "game/effects/emitter_system.h"

namespace oc::app {

struct ControlInbox {
  std::mutex mutex;
  std::condition_variable arrived;
  std::vector<std::string> lines;
  bool closed = false;
};

namespace {

// Ticks run per pass of the main loop while seeking, so the window keeps answering during a long seek.
constexpr int kSeekTicksPerPass = 600;

// A comma list of effect names (Control `xform`, `replay`).
std::vector<std::string> split_names(const std::string& list) {
  std::vector<std::string> names;
  for (size_t start = 0; start <= list.size();) {
    const size_t comma = std::min(list.find(',', start), list.size());
    if (comma > start) names.push_back(list.substr(start, comma - start));
    start = comma + 1;
  }
  return names;
}

// An effect texture for `fxsprites`: u16 width, u16 height, then width x height RGBA, little-endian. Nothing for a size past 0xFFFF.
void write_rgba(const std::filesystem::path& path, u32 width, u32 height, const std::vector<u8>& rgba) {
  if (width > 0xFFFF || height > 0xFFFF) return;
  std::ofstream texture(path, std::ios::binary);
  const u8 head[4] = {static_cast<u8>(width), static_cast<u8>(width >> 8), static_cast<u8>(height), static_cast<u8>(height >> 8)};
  texture.write(reinterpret_cast<const char*>(head), 4);
  texture.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
}

}  // namespace

void App::init_control() {
  controlled = routing && config.debug_control;
  if (!controlled) return;
  if (!config.debug_seed) world.set_seed(1);
  effects::track_heap_usage(true);
  world.set_keep_same_art(true);
  control_apply_debug_keys();
  control_inbox = std::make_shared<ControlInbox>();
  std::thread([inbox = control_inbox] {
    std::string line;
    while (std::getline(std::cin, line)) {
      {
        const std::lock_guard lock(inbox->mutex);
        inbox->lines.push_back(line);
      }
      inbox->arrived.notify_one();
    }
    {
      const std::lock_guard lock(inbox->mutex);
      inbox->closed = true;
    }
    inbox->arrived.notify_one();
  }).detach();
  control_reply("ready");
}

void App::control_reply(const std::string& line) {
  log::info("control: {}", line);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// The same start as a [Debug] Room boot without the game flow (App::init_start_state), or as a [Debug] Battle boot, then the route from
// its first line.
void App::control_restart() {
  std::string err;
  route = game::Route{};
  if (!route.load(config.debug_script, &err)) control_reply("failed 0 " + err);
  route_tick = 0;
  route_finished = false;
  control_anim_playing.reset();
  // A field battle the run before started (a scene's own fight): over, as at boot (the room load below clears the field's side). Left
  // running, every later run would wait for a room with no battle (route `wait room`).
  if (battle_in_field) {
    if (results_ok && results.active()) results.abort();
    battle.stop_music();
    battle_in_field = false;
    battle_scene = -1;
    fb_args.reset();
  }
  // A standalone battle ([Debug] Battle): the scene again, as at boot (the seed is pinned for controlled runs, init_battle).
  if (config.debug_battle >= 0) {
    if (results_ok && results.active()) results.abort();
    if (battle_ready) {
      // A harness action the run had asked for and not begun yet is dropped (the route asks again).
      game::BattleView::Debug debug = battle.debug();
      debug.cast_element = 0, debug.weapon = -1, debug.cast_actor = debug.cast_target = -1;
      battle.set_debug(debug);
      battle_ok = start_debug_battle();
    }
    return;
  }
  if (flow_ready) {
    flow.machine().new_game(U"Serge", false);
    flow.machine().start(game::FlowState::Field);
    world.set_game_state(&flow.game_state());
  }
  // A movie the run before was playing (the opening room's FMV) and the screen fade it left: gone, as at boot.
  if (movie_open) {
    movie_player.close();
    movie_open = movie_fade = false;
    ctx.set_clear_color(0.08f, 0.09f, 0.14f);
  }
  world.clear_screen_fade();
  world.reset_for_new_game();
  world.reset_clock();
  world.set_seed(config.debug_seed ? config.debug_seed : 1u);
  world.select_disc_for_room(dev_room);
  world.load_room(dev_room, config.debug_entrance);
}

// `state <tick> <scroll x> <scroll y> <entities...>`: the field camera's scroll and each entity that has a place, as
// `<n>:<x>,<y>,<z>,<facing>,<drawn>,<yaw>,<clip>,<at>,<from clip>,<from at>,<weight>` (world units, y down = +, facing and the
// model's yaw 4096 per turn, drawn 0 / 1; the actor's pose: its clip and keyframe position x 256, the clip a cross-fade comes from
// (-1: none) and its position, the current clip's share of the blend x 4096), so a controller can lay its own markers over the frame
// and pose the models itself; then `windows:1` while a message window is open (a screen-fixed picture: no whole-room view of it).
std::string App::control_state() {
  const std::pair<s32, s32> scroll = field_visible() ? world.camera_position() : std::pair<s32, s32>{0, 0};
  std::string line = std::format("state {} {} {}", route_tick, scroll.first, scroll.second);
  if (!field_visible()) return line;
  for (int ent = 0; ent < static_cast<int>(world.entity_count()); ++ent) {
    float x = 0, z = 0, height = 0, clip = 0, pitch = 0, drawn = 0;
    if (!world.entity_xz(ent, x, z)) continue;
    world.entity_xz(-3000 - ent, height, clip);
    world.entity_xz(-4000 - ent, pitch, drawn);
    const game::Actor::PoseNow pose = world.entity_pose(ent);
    line += std::format(" {}:{:.0f},{:.0f},{:.0f},{},{},{},{},{},{},{},{}", ent, x, height, z, world.entity_facing(ent), drawn > 0.5f ? 1 : 0,
                        world.entity_yaw(ent), pose.clip, std::lround(pose.time * 256.0f), pose.from_clip,
                        std::lround(pose.from_time * 256.0f), std::lround(pose.weight * 4096.0f));
  }
  if (world.dialogue_is_open()) line += " windows:1";
  return line;
}

// `trace <from> <ticks> <file>`: the scripts' timeline of a scene, for a controller's own timeline view. The run goes to driver tick
// `from` (the scene mark; an earlier tick restarts it) and on without drawing, the VM tracing (FieldVM::Trace: every instruction a
// record runs at a new place, every record's end, ticks counted from `from`) until the triggered scripts are over (`done`: none of
// class < 7 left but parked ones), all of them have waited in place kTraceStall ticks and none waits on a walk whose entity moved
// meanwhile (`stalled`), another room loaded (`room`), none started within kTraceStart ticks (`none`) or `ticks` ran (`limit`). The
// file holds 16 bytes per event: tick u32, pc u32, entity u16, record u8, script slot u8, class u8, flags u8 (1 yield, 2 end), 2 zero
// bytes; little-endian. Reply: `trace <ticks run> <events> <why> <file>`.
std::string App::control_trace(u64 from, u64 ticks, const std::string& file) {
  constexpr u32 kTraceStall = 600;
  constexpr u64 kTraceStart = 300;
  const auto start = std::chrono::steady_clock::now();
  if (from < route_tick) control_restart();
  while (route_tick < from) route_step();
  script::FieldVM::Trace trace;
  world.vm().set_trace(&trace);
  const char* why = "limit";
  bool started = false;
  u64 ran = 0;
  // `p <tick> <entity> <x> <y> <z> <facing> <drawn>`: each entity's place after a tick when it changed (an editor's screenplay:
  // where a walk ends, who appears / disappears)
  std::vector<std::array<s32, 5>> places;
  // 1 + the trace tick each entity's place (x, y, z) last changed (0: not since the trace began): a scene record that waits in place
  // on its walk op while its entity still moves is a slow walk, not a stall (a walk longer than kTraceStall would end the trace).
  std::vector<u64> moved_at;
  std::pair<s32, s32> last_scroll{INT32_MIN, INT32_MIN};
  auto note_places = [&]() {
    if (!field_visible()) return;
    // `s <tick> <x> <y>`: the camera's scroll when it changed (where a pan is; the editor's markers)
    if (const std::pair<s32, s32> scroll = world.camera_position(); scroll != last_scroll) {
      last_scroll = scroll;
      trace.notes.push_back(std::format("s {} {} {}", trace.tick, scroll.first, scroll.second));
    }
    const int count = static_cast<int>(world.entity_count());
    if (places.size() < static_cast<size_t>(count)) places.resize(static_cast<size_t>(count), {INT32_MIN, 0, 0, 0, 0});
    for (int ent = 0; ent < count; ++ent) {
      float x = 0, z = 0, height = 0, clip = 0, pitch = 0, drawn = 0;
      if (!world.entity_xz(ent, x, z)) continue;
      world.entity_xz(-3000 - ent, height, clip);
      world.entity_xz(-4000 - ent, pitch, drawn);
      const std::array<s32, 5> now{static_cast<s32>(std::lround(x)), static_cast<s32>(std::lround(height)), static_cast<s32>(std::lround(z)),
                                   static_cast<s32>(world.entity_facing(ent)), drawn > 0.5f ? 1 : 0};
      if (now == places[static_cast<size_t>(ent)]) continue;
      const std::array<s32, 5>& was = places[static_cast<size_t>(ent)];
      if (moved_at.size() < places.size()) moved_at.resize(places.size(), 0);
      if (was[0] != INT32_MIN && (was[0] != now[0] || was[1] != now[1] || was[2] != now[2])) moved_at[static_cast<size_t>(ent)] = ran + 1;
      places[static_cast<size_t>(ent)] = now;
      trace.notes.push_back(std::format("p {} {} {} {} {} {} {}", trace.tick, ent, now[0], now[1], now[2], now[3], now[4]));
    }
  };
  while (ran < ticks) {
    trace.tick = static_cast<u32>(ran);
    route_step();
    world.trace_windows();
    note_places();
    ++ran;
    if (trace.room_loaded) {
      why = "room";
      break;
    }
    // A room change asked for keeps the scene going until the next room has loaded (`room`): 5A then parks the script.
    const script::FieldVM::SceneState scene =
        world.room_change_pending() ? script::FieldVM::SceneState::Running : world.vm().scene_state(kTraceStall);
    started |= scene != script::FieldVM::SceneState::Done;
    if (started && scene == script::FieldVM::SceneState::Done) {
      why = "done";
      break;
    }
    if (scene == script::FieldVM::SceneState::Stalled) {
      // A scene record waiting on a walk op (3F / 40, 42 / 43, 44 / 45 to an actor, 5B / 5C jumps, B8 / B9) whose entity moved within the
      // stall window: a slow walk, still running. Only walks: an entity moved by something else while its record waits on a flag never set
      // is a stall.
      bool walking = false;
      for (const script::FieldVM::StuckInfo& stuck : world.vm().stuck_records(kTraceStall)) {
        const size_t ent = static_cast<size_t>(stuck.ent);
        const u16 key = stuck.key;
        const bool walk_op = key == 0x3F || key == 0x40 || (key >= 0x42 && key <= 0x45) || key == 0x5B || key == 0x5C || key == 0xB8 || key == 0xB9;
        walking |= walk_op && stuck.cls < 7 && ent < moved_at.size() && moved_at[ent] != 0 && ran + 1 - moved_at[ent] < kTraceStall;
      }
      if (!walking) {
        why = "stalled";
        break;
      }
    }
    if (!started && ran >= kTraceStart) {
      why = "none";
      break;
    }
  }
  world.vm().set_trace(nullptr);
  std::vector<u8> out(trace.events.size() * 16, 0);
  for (size_t i = 0; i < trace.events.size(); ++i) {
    const script::FieldVM::TraceEvent& event = trace.events[i];
    u8* row = out.data() + i * 16;
    for (int k = 0; k < 4; ++k) row[k] = static_cast<u8>(event.tick >> (8 * k)), row[4 + k] = static_cast<u8>(event.pc >> (8 * k));
    row[8] = static_cast<u8>(event.ent), row[9] = static_cast<u8>(event.ent >> 8);
    row[10] = event.rec, row[11] = event.id, row[12] = event.cls, row[13] = event.flags;
  }
  std::ofstream(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary)
      .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  // The notes (variables, dialog windows, movies, places, scroll) beside it, one per line.
  {
    std::ofstream notes(std::filesystem::path(std::u8string(file.begin(), file.end())).concat(".txt"), std::ios::binary);
    for (const std::string& line : trace.notes) notes << line << '\n';
  }
  log::debug("control: trace {} ticks, {} events ({}) in {:.1f} ms", ran, trace.events.size(), why,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return std::format("trace {} {} {} {}", ran, trace.events.size(), why, file);
}

// `btrace <from> <ticks> <file>`: the battle action's timeline, for a controller's own timeline view. The run goes to driver tick `from`
// (the action's mark; an earlier tick restarts it) and on without drawing, the battle view tracing (BattleView::set_route_trace: every
// anmseq op of every unit, every effect-sequence command, every camera select; ticks counted from `from`) until the view is idle again after
// it was busy (`done`), it never got busy within kBtraceStart ticks (`none`), the battle ended (`end`) or `ticks` ran (`limit`). The file
// holds one event per line (the format in battle_view.h). Reply: `btrace <ticks run> <events> <why> <file>`.
std::string App::control_btrace(u64 from, u64 ticks, const std::string& file) {
  constexpr u64 kBtraceStart = 120;
  if (from < route_tick) control_restart();
  while (route_tick < from) route_step();
  std::vector<std::string> lines;
  u64 rel = 0;
  const char* why = "limit";
  if (battle_ready) {
    battle.set_route_trace(&lines, &rel);
    int selects = battle.camera_selects();
    bool busy = false;
    // `h <tick> <phase>`: the presented action's phase when it changes (BattleView::action_phase); an action already under way at `from`
    // (the route marks its start a tick or so after it began) is listed first with the tick it began, before `from` (negative).
    int phase = act_phase;
    if (phase != 0)
      lines.push_back(
          std::format("h {} {}", static_cast<long long>(act_begun) - static_cast<long long>(from), game::BattleView::action_phase_name(phase)));
    for (; rel < ticks; ++rel) {
      route_step();
      if (act_phase != phase) {
        phase = act_phase;
        lines.push_back(std::format("h {} {}", rel, game::BattleView::action_phase_name(phase)));
      }
      if (const int now = battle.camera_selects(); now != selects) {
        selects = now;
        lines.push_back(std::format("c {} {}", rel, battle.camera_shot()));
      }
      // The camera's pose after the tick (eye, target; stage world, y up).
      const game::CamPose pose = battle.camera_pose();
      lines.push_back(std::format("p {} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}", rel, pose.eye[0], pose.eye[1], pose.eye[2], pose.target[0],
                                  pose.target[1], pose.target[2]));
      // Each unit's place after the tick.
      for (int unit = 0; unit < battle::kUnits; ++unit)
        if (float ux = 0, uz = 0; battle.unit_place(unit, ux, uz)) lines.push_back(std::format("u {} {} {:.1f} {:.1f}", rel, unit, ux, uz));
      if (!battle.active()) {
        why = "end";
        ++rel;
        break;
      }
      const bool idle = battle.route_idle();
      busy |= !idle;
      if (busy && idle) {
        why = "done";
        ++rel;
        break;
      }
      if (!busy && rel >= kBtraceStart) {
        why = "none";
        ++rel;
        break;
      }
    }
    battle.set_route_trace(nullptr, nullptr);
  } else {
    why = "none";
  }
  std::ofstream out(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary);
  for (const std::string& line : lines) out << line << '\n';
  return std::format("btrace {} {} {} {}", rel, lines.size(), why, file);
}

// `plan <ini>`: the next run in the same process, so an editor keeps one engine for every preview it shows. The file holds the keys a
// controlled run's ini would ([Debug] Room / Entrance / Battle / BattleParty / BattleField / BattleIntroLive / HoldScripts / Script /
// DraftDir, ForceBranches (empty when left out), [Video] Width / Height / Widescreen); any other key left out keeps its value. A room
// plan (Battle -1) leaves the standalone battle; a battle plan (Battle >= 0) starts one over the field. A changed DraftDir scans the
// mods again (as `reload`); a changed size resizes the window (its shape is the frame's). Reply: `planned <room | battle>` or
// `error plan ...`.
std::string App::control_plan(const std::string& file) {
  const auto start = std::chrono::steady_clock::now();
  std::ifstream in(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary);
  if (!in) return "error plan: can't read " + file;
  std::stringstream text;
  text << in.rdbuf();
  platform::Ini ini;
  if (!ini.parse(text.str())) return "error plan: not an ini " + file;
  auto num = [&](const char* section, const char* key, int fallback) {
    const std::string value = ini.get(section, key);
    return value.empty() ? fallback : std::atoi(value.c_str());
  };
  const int plan_battle = num("Debug", "Battle", config.debug_battle);
  if (plan_battle >= 0 && !battle_ready) return "error plan: no battle view in this run";
  if (plan_battle < 0 && !world_ok) return "error plan: no field in this run";
  config.debug_room = ini.get("Debug", "Room", config.debug_room);
  config.debug_entrance = num("Debug", "Entrance", config.debug_entrance);
  config.debug_battle_party = ini.get("Debug", "BattleParty", config.debug_battle_party);
  config.debug_battle_field = num("Debug", "BattleField", config.debug_battle_field);
  config.debug_battle_intro_live = num("Debug", "BattleIntroLive", config.debug_battle_intro_live ? 1 : 0) != 0;
  config.debug_hold_scripts = num("Debug", "HoldScripts", config.debug_hold_scripts ? 1 : 0) != 0;
  config.debug_force_branches = ini.get("Debug", "ForceBranches", "");
  config.debug_script = ini.get("Debug", "Script", config.debug_script);
  if (!config.debug_room.empty()) dev_room = config.debug_room;
  // The draft folder (an editor's unsaved files over every mod): a new folder is a new overlay.
  const std::string draft = ini.get("Debug", "DraftDir", config.debug_draft_dir);
  if (draft != config.debug_draft_dir) {
    config.debug_draft_dir = draft;
    control_rescan_mods();
  }
  // The window's shape: [Video] Widescreen draws the extra columns of a wider window (App::frame_settings), so a 16:9 preview is a
  // 16:9 window with Widescreen on.
  if (const std::string wide = ini.get("Video", "Widescreen"); !wide.empty())
    config.widescreen = wide == "1" || wide == "on" || wide == "true" || wide == "yes";
  const int width = num("Video", "Width", config.width), height = num("Video", "Height", config.height);
  if (width > 0 && height > 0 && (width != config.width || height != config.height)) {
    config.width = width, config.height = height;
    window.apply_video(static_cast<int>(platform::WindowMode::Windowed), width, height, config.display);
    ctx.request_resize();
  }
  // Leaving the standalone battle for a room: its music and results go; entering one: the controlled seed (init_battle).
  if (plan_battle < 0 && battle_ok) {
    if (results_ok && results.active()) results.abort();
    battle.stop_music();
    battle_ok = false;
  }
  if (plan_battle >= 0 && !std::getenv("OC_BATTLE_SEED")) _putenv_s("OC_BATTLE_SEED", std::to_string(config.debug_seed ? config.debug_seed : 1u).c_str());
  config.debug_battle = plan_battle;
  control_apply_debug_keys();
  // A new run starts on the room's camera.
  control_camera_off();
  control_restart();
  log::debug("control: plan {} in {:.1f} ms", plan_battle >= 0 ? "battle" : config.debug_room,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return plan_battle >= 0 ? "planned battle" : "planned room";
}

// `camera <r0 .. r8> <tx> <ty> <tz> <h> <rrggbb> [noactors]`: the field drawn through this GTE camera (rotation row-major, 1.0 = 1;
// translation; projection distance H, all as a room's camera block holds them) instead of the room's: the actors, their shadows, the
// effects and the room's meshes, over a flat rrggbb in place of the background layers (they are the room camera's picture), every
// column of the picture drawn and the PS1 packet-drop rules off (as for the free camera: an editor's orbit flies close). `noactors`: the
// effects alone. The simulation keeps the room's camera, so a run gives the same ticks with or without it. Frames come from `view <tick>
// 0 0 1 <file>`: a point projects to layer (160 + H x / z, 120 + H y / z), the picture's pixel (w / 2 + (sx - 160) k, sy k), k = h / 224.
// Kept across restarts; a `plan` or `camera off` ends it. Reply: `camera on` / `camera off` / `error camera ...`.
std::string App::control_camera(std::istringstream& words) {
  std::string first;
  words >> first;
  if (first == "off") {
    control_camera_off();
    return "camera off";
  }
  constexpr const char* kUsage = "error camera <r0 .. r8> <tx> <ty> <tz> <h> <rrggbb> [noactors] | camera off";
  float values[13]{};
  const bool first_ok = static_cast<bool>(std::istringstream(first) >> values[0]);
  for (int i = 1; i < 13; ++i) words >> values[i];
  const bool numbers = first_ok && !words.fail();
  std::string rgb, extra;
  words >> rgb >> extra;
  if (!numbers || rgb.size() != 6 || !(values[12] > 0.0f) || (!extra.empty() && extra != "noactors"))
    return kUsage;
  unsigned colour = 0;
  if (std::sscanf(rgb.c_str(), "%6x", &colour) != 1) return kUsage;
  assets::Mat3 rot{};
  for (int i = 0; i < 9; ++i) rot[static_cast<size_t>(i)] = values[i];
  control_backdrop[0] = static_cast<float>((colour >> 16) & 0xFF) / 255.0f;
  control_backdrop[1] = static_cast<float>((colour >> 8) & 0xFF) / 255.0f;
  control_backdrop[2] = static_cast<float>(colour & 0xFF) / 255.0f;
  control_backdrop[3] = 1.0f;
  // A standalone battle ([Debug] Battle): the same GTE camera as the battle camera's pose (BattleView::set_control_camera). Eye = -R^T t
  // and the view axis = R's third row, in the PS1's y-down space; a pose is y-up (battle_camera.h cam_axes), so y is negated. No roll: an
  // editor's orbit keeps R's first row level. The flat colour shows where no stage object draws (a stage pack without its own emitters).
  // Frames come from `seek`; a point projects as on the field (layer (160 + H x / z, 120 + H y / z)).
  if (config.debug_battle >= 0) {
    if (!battle_ready) return "error camera: the battle is not running";
    game::CamPose pose;
    for (size_t i = 0; i < 3; ++i) {
      const float eye = -(rot[0 * 3 + i] * values[9] + rot[1 * 3 + i] * values[10] + rot[2 * 3 + i] * values[11]);
      const float ahead = eye + 4096.0f * rot[2 * 3 + i];
      pose.eye[i] = i == 1 ? -eye : eye;
      pose.target[i] = i == 1 ? -ahead : ahead;
    }
    pose.h = values[12];
    battle.set_control_camera(pose, extra == "noactors");
    return "camera on";
  }
  if (!world_ok) return "error camera: no field in this run";
  world.view().set_camera_override(render::FieldCamera::from_gte(rot, {values[9], values[10], values[11]}, values[12]));
  world.set_hide_actors(extra == "noactors");
  render::set_cull_bypass(true);
  return "camera on";
}

// `fxsprites <tick> <r0 .. r8> <tx> <ty> <tz> <h> <file>|<texdir>`: the room effects' sprite quads at driver tick `tick` (an earlier tick
// restarts the run; the run goes there without drawing, as `trace`), projected through the given GTE camera (as `camera` takes it), for
// an editor that draws the effects in its own scene. Nothing is drawn and the run's own camera is untouched. `file` gets one line per
// quad in draw order (farthest first): `q <sheet> <frame> <sprite> <untextured> <blend> <r> <g> <b> <r1> <g1> <b1> <slot> <cam z> <x0> <y0>
// .. <x3> <y3>`: corners in the texture's corner order (u0v0, u1v0, u0v1, u1v1; mirroring already folded in), projected to (160 + H x /
// z, 120 + H y / z); sheet `-` for untextured quads (1 flat, 2 line: r1 g1 b1 = the far end's colour); blend 0 opaque, 1 B/2+F/2, 2 B+F,
// 3 B-F, 4 B+F/4. Each sprite's texture is written once into `texdir` as `<sheet>_<frame>_<sprite>.rgba` (u16 w, u16 h, w x h RGBA
// little-endian: RDE's 4x HD texels when the sprite has them, else the PS1 texels; alpha 0 where the palette entry is 0), left alone when
// it exists. After the quads, one line per face of the room effects' meshes (FieldWorld::collect_effect_meshes, in no order: an editor
// depth-tests them): `t <tex> <blend> <slot>` then per corner `<x> <y> <z> <u> <v> <r> <g> <b>` (the layer point as above, the corner's
// view depth, u / v normalised in the texture, colours at 128 = neutral); `tex` `-` = untextured, else `mesh_<room>_<n>`, written once
// into `texdir` as `<tex>.rgba` like the sprites. Reply: `fxsprites <quads> <file>` / `fxsprites none` (no effects in this run) / `error fxsprites ...`.
std::string App::control_fxsprites(std::istringstream& words) {
  constexpr const char* kUsage = "error fxsprites <tick> <r0 .. r8> <tx> <ty> <tz> <h> <file>|<texdir>";
  u64 tick = 0;
  float values[13]{};
  words >> tick;
  for (float& value : values) words >> value;
  if (words.fail()) return kUsage;
  // the two paths are the rest of the line, split at '|' (paths may hold spaces)
  std::string rest;
  std::getline(words, rest);
  const size_t bar = rest.find('|');
  const size_t first = rest.find_first_not_of(' ');
  if (bar == std::string::npos || first == std::string::npos || first >= bar) return kUsage;
  const std::string file = rest.substr(first, bar - first), texdir = rest.substr(bar + 1);
  if (file.empty() || texdir.empty() || !(values[12] > 0.0f)) return kUsage;
  if (!world_ok) return "error fxsprites: no field in this run";
  if (tick < route_tick) control_restart();
  while (route_tick < tick) route_step();
  assets::Mat3 rot{};
  for (int i = 0; i < 9; ++i) rot[static_cast<size_t>(i)] = values[i];
  const render::FieldCamera cam = render::FieldCamera::from_gte(rot, {values[9], values[10], values[11]}, values[12]);
  // ox / oy 160 / 120, no scroll: the projection `camera` documents.
  effects::ViewParams vp;
  for (size_t i = 0; i < 9; ++i) vp.rot.m[i] = cam.rot()[i];
  for (size_t i = 0; i < 3; ++i) vp.trans[i] = cam.trans()[i];
  vp.h = cam.h();
  std::vector<effects::DrawQuad> quads;
  if (!world.collect_effect_quads(vp, quads)) return "fxsprites none";
  const std::filesystem::path dir(std::u8string(texdir.begin(), texdir.end()));
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string out;
  for (const effects::DrawQuad& quad : quads) {
    const std::string sheet = quad.sheet != nullptr && quad.sprite != nullptr ? quad.sheet->name : "-";
    out += std::format("q {} {} {} {} {} {} {} {} {} {} {} {} {:.2f}", sheet, quad.frame, quad.sprite_index, quad.untextured, quad.blend, quad.r,
                       quad.g, quad.b, quad.r1, quad.g1, quad.b1, quad.slot, quad.cam[2]);
    for (int corner = 0; corner < 4; ++corner) out += std::format(" {:.2f} {:.2f}", quad.x[corner], quad.y[corner]);
    out += '\n';
    if (sheet == "-") continue;
    const std::filesystem::path tex = dir / std::format("{}_{}_{}.rgba", sheet, quad.frame, quad.sprite_index);
    if (std::filesystem::exists(tex, ec)) continue;
    const assets::EffectSprite& sprite = *quad.sprite;
    const bool hd = !sprite.hd_rgba.empty();
    const u32 scale = hd ? 4 : 1;
    write_rgba(tex, static_cast<u32>(sprite.w) * scale, static_cast<u32>(sprite.h) * scale, hd ? sprite.hd_rgba : sprite.rgba);
  }
  std::vector<effects::RoomMeshTri> tris;
  world.collect_effect_meshes(vp, tris);
  for (const effects::RoomMeshTri& tri : tris) {
    const std::string key = tri.tex >= 0 ? std::format("mesh_{}_{}", world.room_name(), tri.tex) : "-";
    out += std::format("t {} {} {}", key, tri.blend, tri.slot);
    for (int corner = 0; corner < 3; ++corner)
      out += std::format(" {:.2f} {:.2f} {:.2f} {:.5f} {:.5f} {} {} {}", tri.x[corner], tri.y[corner], tri.z[corner], tri.u[corner], tri.v[corner],
                         tri.rgb[corner][0], tri.rgb[corner][1], tri.rgb[corner][2]);
    out += '\n';
    if (key == "-") continue;
    const std::filesystem::path tex = dir / (key + ".rgba");
    if (std::filesystem::exists(tex, ec)) continue;
    u32 width = 0, height = 0;
    std::vector<u8> texels;
    if (world.room_meshes().texture_rgba(tri.tex, width, height, texels)) write_rgba(tex, width, height, texels);
  }
  std::ofstream(std::filesystem::path(std::u8string(file.begin(), file.end())), std::ios::binary) << out;
  return std::format("fxsprites {} {}", quads.size(), file);
}

// `seek <tick> <file>`, `room <tick> <x max> <x min> <y max> <y min> <file>` (the whole room in one pass, App::draw_room_shot) and
// `view <tick> <scroll x> <scroll y> <zoom> <file>` (the game camera's frame at that scroll showing zoom times as much room each way, below
// 1 zoomed in, the message windows screen-fixed in it): the tick to run to and the file its frame goes to. The reply comes with the frame.
std::string App::control_seek(const std::string& op, std::istringstream& words) {
  long long tick = -1;
  RoomBounds bounds;
  words >> tick;
  if (op == "room") words >> bounds.x_max >> bounds.x_min >> bounds.y_max >> bounds.y_min;
  if (op == "view") words >> bounds.x >> bounds.y >> bounds.zoom;
  const bool numbers = !words.fail();
  std::string file;
  std::getline(words >> std::ws, file);
  const bool zoom_ok = op != "view" || (bounds.zoom > 0.05f && bounds.zoom < 20.0f);
  if (!numbers || tick < 0 || file.empty() || !zoom_ok) {
    if (op == "room") return "error room <tick> <x max> <x min> <y max> <y min> <file>";
    if (op == "view") return "error view <tick> <scroll x> <scroll y> <zoom> <file>";
    return "error seek <tick> <file>";
  }
  control_target = tick;
  control_frame_file = file;
  control_room.reset();
  if (op != "seek") control_room = bounds;
  return {};
}

// `scroll <x> <y>` / `scroll off`: the field scroll held at (x, y) from now on (FieldWorld::debug_pin_scroll) / the camera's again.
std::string App::control_scroll(std::istringstream& words) {
  std::string first;
  words >> first;
  if (first == "off") {
    world.debug_unpin_scroll();
    return "scroll off";
  }
  int x = 0, y = 0;
  std::istringstream(first) >> x;
  if (first.empty() || !(words >> y)) return "error scroll <x> <y> | scroll off";
  world.debug_pin_scroll(x, y);
  return std::format("scroll {} {}", x, y);
}

// `anim <entity> <anim> [min ticks]`: the entity's actor plays the script animation id now (as SET_ANIMATION), then, once that id has
// played out (the anim-done bit WAIT_ANIMATION waits for) and at least `min ticks` have passed, goes back to the id it had (an editor's
// play button on a room that stands still); the id in force starts again from the top. A second `anim` replaces the first; its actor goes back at once when it is another one.
std::string App::control_anim(std::istringstream& words) {
  int ent = -1, anim = -1, min_ticks = 0;
  words >> ent >> anim;
  if (!(words >> min_ticks)) min_ticks = 0;
  if (ent < 0 || anim < 0 || anim > 0xFE || min_ticks < 0) return "error anim <entity> <anim 0..254> [min ticks]";
  if (!field_visible()) return "error anim: no room on show";
  if (static_cast<size_t>(ent) >= world.entity_count()) return std::format("error anim: no entity {}", ent);
  const game::Actor& actor = world.ent(static_cast<size_t>(ent)).actor;
  if (!actor.loaded()) return std::format("error anim: entity {} has no model", ent);
  if (!actor.has_script_clip(anim)) return std::format("error anim: entity {} has no animation {}", ent, anim);
  int idle = actor.requested_anim();
  if (control_anim_playing) {
    if (control_anim_playing->ent == ent) idle = control_anim_playing->idle;
    else control_anim_end();
  }
  // The id already in force plays again from its start (SET_ANIMATION 0xFF re-submits it; the same id would go on where it is).
  world.set_animation(ent, anim == actor.requested_anim() ? 0xFF : anim);
  control_anim_playing = ControlAnim{ent, idle, 0, min_ticks, world.room_loads()};
  return std::format("anim {} {}", ent, anim);
}

void App::control_anim_step() {
  ControlAnim& playing = *control_anim_playing;
  if (playing.room_load != world.room_loads()) {
    control_anim_playing.reset();
    return;
  }
  if (++playing.ticks < playing.min_ticks || world.animation_busy(playing.ent)) return;
  control_anim_end();
}

void App::control_anim_end() {
  const ControlAnim playing = *control_anim_playing;
  control_anim_playing.reset();
  if (playing.room_load != world.room_loads()) return;
  if (playing.idle < 0) world.clear_animation(playing.ent);
  else world.set_animation(playing.ent, playing.idle);
  control_reply(std::format("anim done {} {}", playing.ent, route_tick));
}

// `xform <name,name,...> <a0 .. a8> <pivot x y z> <shift x y z>` / `xform off`: the named effects drawn as moved by A about the pivot and
// by the shift (an editor's live drag, effects::DrawXform); in a standalone battle the stage's own emitters (an editor's effects drafted
// into the stage pack). `camera off` and a `plan` end it.
std::string App::control_xform(std::istringstream& words) {
  std::string first;
  words >> first;
  if (first == "off") {
    if (world_ok) world.set_effect_xform(std::nullopt);
    if (battle_ready) battle.set_stage_xform(std::nullopt);
    return "xform off";
  }
  effects::DrawXform xform;
  xform.names = split_names(first);
  for (float& value : xform.matrix.m) words >> value;
  words >> xform.pivot[0] >> xform.pivot[1] >> xform.pivot[2] >> xform.shift[0] >> xform.shift[1] >> xform.shift[2];
  const bool parsed = !xform.names.empty() && !words.fail();
  if (parsed && config.debug_battle >= 0 && battle_ready) {
    battle.set_stage_xform(xform);
    return "xform on";
  }
  if (!parsed || !world_ok || !world.set_effect_xform(xform)) return "error xform <names> <a0 .. a8> <pivot x y z> <shift x y z> | xform off";
  return "xform on";
}

// `grid <cx> <cz> <step> <half>` / `grid off`: the floor grid drawn with the effects while the run has its own camera (FieldWorld::
// set_debug_grid; in a standalone battle with the stage's effects, StageScene::set_debug_grid). `camera off` and a `plan` end it.
std::string App::control_grid(std::istringstream& words) {
  std::string first;
  words >> first;
  if (first == "off") {
    if (world_ok) world.set_debug_grid(std::nullopt);
    if (battle_ready) battle.set_stage_grid(std::nullopt);
    return "grid off";
  }
  effects::FloorGrid grid;
  std::istringstream(first) >> grid.cx;
  words >> grid.cz >> grid.step >> grid.half;
  if (first.empty() || words.fail() || !(grid.step > 0.0f) || grid.half < 1 || grid.half > 64) return "error grid <cx> <cz> <step> <half> | grid off";
  if (config.debug_battle >= 0 && battle_ready) battle.set_stage_grid(grid);
  else if (world_ok) world.set_debug_grid(grid);
  else return "error grid: no field or battle in this run";
  return "grid on";
}

// `replay <name,name,...>`: those effects played again from their start (killed, started), the room / battle going on: an editor's loop
// without restarting the run. In a standalone battle the stage's emitters.
std::string App::control_replay(std::istringstream& words) {
  std::string list;
  words >> list;
  const std::vector<std::string> names = split_names(list);
  if (names.empty()) return "error replay <names>";
  if (config.debug_battle >= 0) return std::format("replayed {}", battle_ready ? battle.replay_stage_effects(names) : 0);
  return std::format("replayed {}", world_ok ? world.replay_effects(names) : 0);
}

// `fxmem`: the effect object arena's use since the last replay (or the run's start), measured after each tick.
std::string App::control_fxmem() {
  const effects::EmitterSystem* emitters = config.debug_battle >= 0 ? (battle_ready ? battle.stage_emitters() : nullptr)
                                                                    : (world_ok ? world.emitters() : nullptr);
  if (emitters == nullptr) return "fxmem none";
  const effects::HeapUsage& usage = emitters->heap_usage();
  std::string per;
  for (const auto& [name, count] : usage.per_emitter) per += std::format("{}{}:{}", per.empty() ? "" : ",", name, count);
  return std::format("fxmem {} {} {} {} {} {}", usage.size, usage.used, usage.peak_bytes, usage.peak_objects, usage.full, per.empty() ? "-" : per);
}

// `campose`: the battle camera's pose where the run stands, in the PS1's y-down space (Control `camera`'s): an editor's free camera starts
// there. The battle's own camera, whatever `camera` override is drawn.
std::string App::control_campose() {
  if (config.debug_battle < 0 || !battle_ready) return "campose none";
  const game::CamPose pose = battle.camera_pose();
  return std::format("campose {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}", pose.eye[0], -pose.eye[1], pose.eye[2], pose.target[0],
                     -pose.target[1], pose.target[2], pose.h);
}

// `stage 0 <rrggbb>` / `stage 1`: a standalone battle's stage not drawn, the picture cleared to rrggbb behind the units and the effects
// (BattleView::set_stage_hidden; the `grid` still drawn) / drawn again. Kept across restarts and plans.
std::string App::control_stage(std::istringstream& words) {
  constexpr const char* kUsage = "error stage 0 <rrggbb> | stage 1";
  int on = -1;
  std::string rgb;
  words >> on >> rgb;
  unsigned colour = 0;
  if (on == 1) {
    if (battle_ready) battle.set_stage_hidden(false);
    return "stage 1";
  }
  if (on != 0 || rgb.size() != 6 || std::sscanf(rgb.c_str(), "%6x", &colour) != 1) return kUsage;
  if (!battle_ready) return "error stage: no battle view in this run";
  stage_backdrop[0] = static_cast<float>((colour >> 16) & 0xFF) / 255.0f;
  stage_backdrop[1] = static_cast<float>((colour >> 8) & 0xFF) / 255.0f;
  stage_backdrop[2] = static_cast<float>(colour & 0xFF) / 255.0f;
  battle.set_stage_hidden(true);
  return "stage 0";
}

// The mods looked at again ([Debug] DraftDir included) and applied: the art, the mod rooms and the text rows (an editor's changed files).
void App::control_rescan_mods() {
  mods.scan(files, mods.dir().empty() ? files.game_dir() / "mods" : mods.dir(), mods.prefs(),
            std::filesystem::path(std::u8string(config.debug_draft_dir.begin(), config.debug_draft_dir.end())));
  apply_art_mode();
  install_mod_rooms_and_text();
}

// [Debug] HoldScripts and ForceBranches into the field: at the controlled run's start and after each `plan`.
void App::control_apply_debug_keys() {
  world.set_hold_scripts(config.debug_hold_scripts);
  forced_branches.clear();
  std::istringstream list(config.debug_force_branches);
  for (std::string item; std::getline(list, item, ',');) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    try {
      forced_branches[static_cast<u32>(std::stoul(item.substr(0, colon)))] = static_cast<u32>(std::stoul(item.substr(colon + 1)));
    } catch (const std::exception&) {
      log::warn("[Debug] ForceBranches: not <pc>:<to>: {}", item);
    }
  }
  world.vm().set_forced_branches(forced_branches.empty() ? nullptr : &forced_branches);
}

void App::control_camera_off() {
  if (battle_ready) {
    battle.set_control_camera(std::nullopt, false);
    battle.set_stage_xform(std::nullopt);
    battle.set_stage_grid(std::nullopt);
  }
  if (!world_ok) return;
  if (world.view().camera_overridden()) render::set_cull_bypass(false);
  world.view().set_camera_override(std::nullopt);
  world.set_hide_actors(false);
  world.set_debug_grid(std::nullopt);
  world.set_effect_xform(std::nullopt);
}

// One command line's work: its reply, "" when there is none now (a seek's comes with its frame).
std::string App::control_command(const std::string& op, std::istringstream& words) {
  if (op == "seek" || op == "room" || op == "view") return control_seek(op, words);
  if (op == "scroll") return control_scroll(words);
  if (op == "reload") {
    // The mods (the draft folder included) looked at again, then the run restarted: an editor's changed files are what it plays.
    control_rescan_mods();
    control_restart();
    return "reloaded";
  }
  if (op == "restart") {
    // The draft folder's text rows (an editor's unsaved dialog) are read again: a restart plays changed contents of its files.
    const auto draft = std::find_if(mods.mods().begin(), mods.mods().end(), [](const vfs::Mod& mod) { return mod.name == vfs::kDraftMod; });
    if (draft != mods.mods().end() && !draft->text.empty()) install_mod_rooms_and_text();
    control_restart();
    return "restarted";
  }
  if (op == "trace" || op == "btrace") {
    long long from = -1, ticks = -1;
    std::string file;
    words >> from >> ticks;
    std::getline(words >> std::ws, file);
    if (from < 0 || ticks <= 0 || file.empty()) return std::format("error {} <from> <ticks> <file>", op);
    return op == "trace" ? control_trace(static_cast<u64>(from), static_cast<u64>(ticks), file)
                         : control_btrace(static_cast<u64>(from), static_cast<u64>(ticks), file);
  }
  if (op == "plan") {
    std::string file;
    std::getline(words >> std::ws, file);
    control_room.reset();
    control_target = -1;
    return file.empty() ? "error plan <ini>" : control_plan(file);
  }
  if (op == "camera") return control_camera(words);
  if (op == "xform") return control_xform(words);
  if (op == "grid") return control_grid(words);
  if (op == "replay") return control_replay(words);
  if (op == "fxsprites") return control_fxsprites(words);
  if (op == "fxmem") return control_fxmem();
  if (op == "campose") return control_campose();
  if (op == "hud") {
    int on = 1;
    words >> on;
    if (battle_ready) battle.set_hud_hidden(on == 0);
    return std::format("hud {}", on == 0 ? 0 : 1);
  }
  if (op == "stage") return control_stage(words);
  if (op == "anim") return control_anim(words);
  if (op == "tick") return std::format("tick {}", route_tick);
  if (op == "quit") {
    quit_requested = true;
    return {};
  }
  return "error unknown command " + op;
}

void App::control_frame() {
  step = {0, 0.0f};
  std::vector<std::string> lines;
  bool closed = false;
  {
    const std::lock_guard lock(control_inbox->mutex);
    lines.swap(control_inbox->lines);
    closed = control_inbox->closed;
  }
  for (const std::string& line : lines) {
    std::istringstream words(line);
    std::string op;
    words >> op;
    if (op.empty()) continue;
    if (const std::string reply = control_command(op, words); !reply.empty()) control_reply(reply);
  }
  if (closed && lines.empty()) quit_requested = true;
  // A frame being read back: the next passes deliver it, at once (no wait: a frame costs a few passes). Idle: until a command comes
  // (or 4 ms, so the window keeps answering).
  if (control_capture_pending) return;
  if (control_target < 0) {
    std::unique_lock lock(control_inbox->mutex);
    control_inbox->arrived.wait_for(lock, std::chrono::milliseconds(4), [&] { return !control_inbox->lines.empty() || control_inbox->closed; });
    return;
  }
  if (static_cast<u64>(control_target) < route_tick) control_restart();
  {
    const u64 from = route_tick;
    const auto start = std::chrono::steady_clock::now();
    for (int budget = kSeekTicksPerPass; route_tick < static_cast<u64>(control_target) && budget > 0; --budget) route_step();
    if (route_tick > from)
      log::debug("control: ticks {}..{} in {:.1f} ms", from, route_tick,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  if (route_tick != static_cast<u64>(control_target)) return;
  if (control_room) {
    // The whole room: every entity's place, drawn = not hidden by its script (the off-screen cull is the camera's); the scroll is the
    // picture's top-left (the greatest scroll: x max, y max), as a stitch reports it.
    world.set_draw_offscreen(true);
    std::string state = control_state();
    world.set_draw_offscreen(false);
    if (field_visible()) {
      const auto scroll = world.camera_position();
      const std::string from = std::format("state {} {} {}", route_tick, scroll.first, scroll.second);
      if (state.starts_with(from) && control_room->zoom <= 0)
        state = std::format("state {} {} {}", route_tick, control_room->x_max, control_room->y_max) + state.substr(from.size());
    }
    control_reply(state);
    room_shot_pending = control_room;
    control_room.reset();
  } else {
    control_reply(control_state());
  }
  control_capture_pending = true;
  // The depth comes first (end_frame hands it over before the colour): `depth` then `frame`.
  if (control_depth)
    ctx.request_depth_capture([this, tick = control_target, file = control_frame_file](std::vector<u8>&& packed, u32 width, u32 height) {
      std::filesystem::path depth_file = file;
      depth_file += ".depth";
      std::ofstream(depth_file, std::ios::binary).write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
      control_reply(std::format("depth {} {} {} {}", tick, width, height, depth_file.string()));
    });
  ctx.request_capture([this, tick = control_target, file = control_frame_file](std::vector<u8>&& rgba, u32 width, u32 height) {
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
    control_reply(std::format("frame {} {} {} {}", tick, width, height, file.string()));
    control_capture_pending = false;
  });
  control_target = -1;
}

}  // namespace oc::app
