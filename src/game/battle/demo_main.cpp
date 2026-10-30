// ocbattleview: runs one battle in its own window (development tool for BattleView).
//   ocbattleview [scene] [--shot <frames>] [--orig] [--auto]     scene = scenedat index (default 0)
//   --trans <frame>  start the battle-entry transition (field-to-battle hand-over) at that frame
//   --cast <element id>  the first PC casts that pgdat element on the first enemy as soon as the view ticks (effect packs)
//   --caster <unit> --casttgt <unit>  with --cast: that unit (PCs 0-2, enemies 3+) casts the element on that unit (ally elements, Pilfer ...)
//   --ecast <element id>  the first enemy casts it on the first PC;  --weapon <0|1|2>  PC 0 attacks the first enemy (weak / medium / heavy);  --cam  camera overlay (F3)
//   --field <n>  battlefield operand (stage battle/bg/bg_NN; default: the scene's own)
//   --autoframes <n>  --auto stops pressing after n frames (e.g. 60: one attack, then the menu comes back)
//   --nofx  element casts take the generic path (no effect pack);  --castat <frame>  issue the --cast at that frame (after the intro / fade-in)
//   --shotevery <n> --shotdir <dir> [--frames <n>] [--size <w> <h>]  deterministic run (one tick per frame) writing <dir>/shot_NNNNNN.png every n frames, exit after --frames
//   --shotfrom <f> --shotto <f>  with --shotevery: only frames f1..f2 (inclusive; default 0..unbounded) write a shot
//   --stats hp=1023,atk=1,def=127,hit=127,evd=0,mag=1,mdef=127  base stats of every party member (default party {0,1,22} when no --party); --enemyhp u3,u4,u5  HP (and max HP) of the enemy units 3 / 4 / 5 (-1 or empty = unchanged)
//   --ko <unit>  that party unit starts KO'd (hp 0 + status KO: Revive / Full Revival targets)
//   --equip <item id>  party slot 0 holds that weapon (e.g. 0x26 = Mastermune); armour / accessories stay the character table's
//   --party a[,b[,c]]  character ids, slot 0 = the acting PC (default party {0, 1, 22}); the battle seats the party in this order, so
//                      --caster / --casttgt / --cast2tgt / --ko 0..2 are the party slots of this list
//   --ring c,c,c  the field ring at the start: colour names (green white black red yellow blue), indices 0..5, 'all' or 'none'
//   --cast2 <element> --cast2at <frame> [--cast2tgt <unit>]  a second cast (consecutive casts)
//   --keys "Down:200,Right:240"  a key press (SDL scancode name) at that frame
//   --auto  feeds a confirm every 12 frames (Attack, Weak, target, repeat) for smoke tests
//   --bauto the battle plays itself (BattleView::set_auto); --ui <1|2|3> opens the element grid / attack chain / element targeting
//   --mods  the ini's enabled mods in Modded art (texture replacements included), as the game installs them
//   --sliced  the load in 3 ms slices per frame (the field encounter's way); logs the longest slice
// Keys: arrows/WASD + Enter/Z confirm + X back, Q/E switch PC, F1 art mode. --shot writes battle_shot.png after N frames and exits.
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/mixer.h"
#include "core/log.h"
#include "data/tables.h"
#include "game/battle/battle_view.h"
#include "platform/config.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/gpu_context.h"
#include "render/imgui_mods_tab.h"
#include "render/texture_registry.h"
#include "vfs/mods.h"

int ocbattleview_main(int argc, char** argv) {
  using namespace oc;
  int scene = 0, shot_frames = -1;
  bool hd = true, auto_keys = false, bauto = false;
  int ui_mode = 0, cast_element = 0, cast_at = -1;
  bool fx_on = true, enemy_cast = false, overlay = false, with_mods = false, sliced = false;
  std::vector<int> party_ids;
  int weapon = -1, field = -1, auto_frames = 1 << 30, trans_frame = -1;
  int caster = -1, casttgt = -1;
  // --stats overrides in the order hp, atk, def, hit, evd, mag, mdef (-1 = keep).
  std::array<int, 7> stat_set{-1, -1, -1, -1, -1, -1, -1};
  bool have_stats = false;
  int ko_unit = -1;
  // --status unit:lo[,unit:lo]: inflicted status bits OR-ed into the units' merged lo words.
  std::vector<std::pair<int, u32>> status_lo;
  int equip_weapon = -1;
  std::array<int, 3> enemy_hp{-1, -1, -1};
  // --ring: the field ring set after the start (unset = the stage's own).
  std::array<int, 3> start_ring{-1, -1, -1};
  bool have_ring = false;
  int cast2 = 0, cast2_at = -1, cast2_tgt = -1;
  int shot_every = 0, shot_from = 0, shot_to = 1 << 30, max_frames = -1, win_w = 1280, win_h = 960;
  std::filesystem::path shot_dir;
  // (frame, key) presses from --keys.
  std::vector<std::pair<int, SDL_Scancode>> key_script;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot_frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--orig")) hd = false;
    else if (!std::strcmp(argv[i], "--auto")) auto_keys = true;
    else if (!std::strcmp(argv[i], "--bauto")) bauto = true;
    else if (!std::strcmp(argv[i], "--ui") && i + 1 < argc) ui_mode = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cast") && i + 1 < argc) cast_element = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--castat") && i + 1 < argc) cast_at = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--ecast") && i + 1 < argc) cast_element = std::atoi(argv[++i]), enemy_cast = true;
    else if (!std::strcmp(argv[i], "--caster") && i + 1 < argc) caster = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--casttgt") && i + 1 < argc) casttgt = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cast2") && i + 1 < argc) cast2 = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cast2at") && i + 1 < argc) cast2_at = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cast2tgt") && i + 1 < argc) cast2_tgt = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--weapon") && i + 1 < argc) weapon = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--party") && i + 1 < argc) {
      for (const char* cursor = argv[++i]; *cursor;) {
        party_ids.push_back(std::atoi(cursor));
        while (*cursor && *cursor != ',') ++cursor;
        if (*cursor == ',') ++cursor;
      }
    }
    else if (!std::strcmp(argv[i], "--stats") && i + 1 < argc) {
      static const char* const kNames[7] = {"hp", "atk", "def", "hit", "evd", "mag", "mdef"};
      std::string list = argv[++i];
      for (size_t start = 0; start < list.size();) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        const std::string item = list.substr(start, end - start);
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
          for (int k = 0; k < 7; ++k)
            if (item.compare(0, eq, kNames[k]) == 0) stat_set[static_cast<size_t>(k)] = std::atoi(item.c_str() + eq + 1), have_stats = true;
        start = end + 1;
      }
    }
    else if (!std::strcmp(argv[i], "--enemyhp") && i + 1 < argc) {
      const char* cursor = argv[++i];
      for (size_t k = 0; k < 3 && *cursor; ++k) {
        if (*cursor != ',') enemy_hp[k] = std::atoi(cursor);
        while (*cursor && *cursor != ',') ++cursor;
        if (*cursor == ',') ++cursor;
      }
    }
    else if (!std::strcmp(argv[i], "--ring") && i + 1 < argc) {
      static const char* const kColourNames[6] = {"green", "white", "black", "red", "yellow", "blue"};
      const std::string list = argv[++i];
      size_t slot = 0;
      for (size_t start = 0; start <= list.size() && slot < 3; ++slot) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string item = list.substr(start, end - start);
        int colour = item == "all" ? battle::kRingAllColours : item == "none" || item.empty() ? -1 : std::atoi(item.c_str());
        for (int k = 0; k < 6; ++k)
          if (item == kColourNames[k]) colour = k;
        start_ring[slot] = colour;
        start = end + 1;
      }
      have_ring = true;
    }
    else if (!std::strcmp(argv[i], "--ko") && i + 1 < argc) ko_unit = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--status") && i + 1 < argc) {
      for (char* cursor = argv[++i]; *cursor;) {
        char* end = cursor;
        const int unit = static_cast<int>(std::strtol(cursor, &end, 0));
        if (*end != ':') break;
        status_lo.emplace_back(unit, static_cast<u32>(std::strtoul(end + 1, &end, 0)));
        cursor = *end == ',' ? end + 1 : end;
      }
    }
    else if (!std::strcmp(argv[i], "--equip") && i + 1 < argc) equip_weapon = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
    else if (!std::strcmp(argv[i], "--autoframes") && i + 1 < argc) auto_frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--field") && i + 1 < argc) field = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--trans") && i + 1 < argc) trans_frame = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--cam")) overlay = true;
    else if (!std::strcmp(argv[i], "--mods")) with_mods = true;
    else if (!std::strcmp(argv[i], "--sliced")) sliced = true;
    else if (!std::strcmp(argv[i], "--nofx")) fx_on = false;
    else if (!std::strcmp(argv[i], "--shotevery") && i + 1 < argc) shot_every = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--shotfrom") && i + 1 < argc) shot_from = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--shotto") && i + 1 < argc) shot_to = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--shotdir") && i + 1 < argc) shot_dir = argv[++i];
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--keys") && i + 1 < argc) {
      const std::string list = argv[++i];
      for (size_t start = 0; start < list.size();) {
        const size_t end = std::min(list.find(',', start), list.size()), colon = list.find(':', start);
        if (colon != std::string::npos && colon < end) {
          const SDL_Scancode scancode = SDL_GetScancodeFromName(list.substr(start, colon - start).c_str());
          if (scancode != SDL_SCANCODE_UNKNOWN) key_script.emplace_back(std::atoi(list.substr(colon + 1, end - colon - 1).c_str()), scancode);
        }
        start = end + 1;
      }
    }
    else if (!std::strcmp(argv[i], "--size") && i + 2 < argc) win_w = std::atoi(argv[++i]), win_h = std::atoi(argv[++i]);
    else scene = std::atoi(argv[i]);
  }
  const std::filesystem::path exe = platform::exe_dir();
  log::init((exe / "ocbattleview.log").string());
  platform::Config config;
  config.load(exe / "opencross.ini");
  // [Cheats] of the harness ini (e.g. AlwaysLoot = a Pilfer that lands).
  platform::bind_cheats(&config.cheats);
  const std::filesystem::path game_dir = platform::find_game_dir(config.game_dir);
  vfs::Vfs files;
  if (game_dir.empty() || !files.open(game_dir)) {
    log::error("game dir not found");
    return 1;
  }
  // In HD mode cdrom2 overrides cdrom.
  files.set_hd_mode(hd);
  vfs::ModManager mods;
  if (with_mods) {
    mods.scan(files, files.game_dir() / "mods", render::mod_prefs(config));
    mods.apply(files, vfs::ModArt::Modded);
    render::texture_registry::bind(&files);
    render::texture_registry::configure({false, true});
  }
  data::GameTables tables;
  std::string err;
  if (!tables.load(files, &err)) {
    log::error("tables: {}", err);
    return 1;
  }

  // Dev: lists the element ids whose effect id is N (OC_LIST_ELEMENTS_EID=38).
  if (const char* eid = std::getenv("OC_LIST_ELEMENTS_EID")) {
    for (size_t i = 0; i < tables.elements.size(); ++i)
      if ((tables.elements[i].raw[0x10] | (tables.elements[i].raw[0x11] << 8)) == std::atoi(eid)) std::printf("element %zu\n", i);
    return 0;
  }
  // A scripted run (--frames) is a test: its window takes no focus and stays silent (platform::Window / audio::Mixer read OC_QUIET).
  if (max_frames > 0) _putenv_s("OC_QUIET", "1");
  platform::Window window;
  if (!window.create("OpenCross battle", win_w, win_h, false)) return 1;
  render::GpuContext ctx;
  if (!ctx.init(window.handle(), true)) return 1;
  ctx.set_clear_color(0.0f, 0.0f, 0.0f);
  render::SpriteBatch sprites;
  render::MeshPipeline meshes;
  if (!sprites.init(ctx) || !meshes.init(ctx)) return 1;

  std::vector<battle::PartyMember> party;
  for (size_t k = 0; k < party_ids.size() && k < 3; ++k) party.push_back(battle::make_party_member(tables, party_ids[k], 0, "char"));
  // --stats: the default party when none was given, then the overrides on every member (hp = max HP and current HP).
  if (have_stats) {
    if (party.empty())
      for (int id : {0, 1, 22}) party.push_back(battle::make_party_member(tables, id, 0, "char"));
    for (auto& member : party) {
      for (size_t k = 0; k < 7; ++k)
        if (stat_set[k] >= 0) member.stat[k] = stat_set[k];
      member.hp = member.stat[0];
    }
  }
  if (equip_weapon >= 0) {
    if (party.empty())
      for (int id : {0, 1, 22}) party.push_back(battle::make_party_member(tables, id, 0, "char"));
    battle::PartyMember& lead = party.front();
    const auto& rec = tables.chars("char").rec[static_cast<size_t>(lead.char_id)];
    lead.eq = battle::make_equip(tables, equip_weapon, rec.armour(), lead.eq.acc);
  }
  int rc = 0;
  {
    game::BattleView view;
    // OC_SFX_TRACE=1: a device-less mixer renders the battle's sounds per tick into a wav (sfx trace)
    audio::Mixer trace_mixer;
    audio::Mixer* mixer = nullptr;
    if (audio::sfx_trace_on()) {
      mixer = &trace_mixer;
      trace_mixer.start_capture((exe / "sfx_trace.wav").string());
    }
    game::BattleView::StartOptions options;
    options.battlefield = field;
    bool view_ok = view.init(ctx, files, sprites, meshes, tables, mixer);
    // --sliced: the load in 3 ms slices, one per 60 Hz frame, as the field encounter runs it behind the transition.
    if (view_ok && sliced && view.begin_load(scene, party, options)) {
      double longest = 0.0;
      int slices = 0;
      const auto t0 = std::chrono::steady_clock::now();
      for (bool done = false; !done; ++slices) {
        const auto t_slice = std::chrono::steady_clock::now();
        done = view.load_step(3.0);
        longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_slice).count());
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
      log::info("sliced load: {} slices over {:.0f} ms, longest slice {:.1f} ms", slices,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), longest);
    }
    if (!view_ok || !view.start(scene, party, options)) {
      log::error("battle view failed to start");
      rc = 1;
    } else {
      view.set_art_mode(hd);
      // --ring
      if (battle::Battle* current = view.battle_mut(); current && have_ring) current->set_ring(start_ring);
      // --enemyhp
      if (battle::Battle* current = view.battle_mut())
        for (int k = 0; k < 3; ++k)
          if (enemy_hp[static_cast<size_t>(k)] >= 0) {
            battle::Unit& unit = current->unit_mut(3 + k);
            if (unit.present) unit.max_hp = unit.hp = enemy_hp[static_cast<size_t>(k)];
          }
      // --ko
      if (battle::Battle* current = view.battle_mut(); current && ko_unit >= 0) {
        battle::Unit& unit = current->unit_mut(ko_unit);
        if (unit.present) unit.hp = 0, unit.lo |= battle::st::kKO;
      }
      // --status
      if (battle::Battle* current = view.battle_mut())
        for (const auto& [unit_index, bits] : status_lo)
          if (unit_index >= 0 && unit_index < battle::kUnits && current->unit(unit_index).present) current->unit_mut(unit_index).lo |= bits;
      // [Enhancements] from opencross.ini; OC_LIGHTING=1 forces [Enhancements] Lighting on (verification runs without touching the ini)
      meshes.set_enhanced_lighting(config.lighting || std::getenv("OC_LIGHTING") != nullptr);
      // [Enhancements] SmoothReflections; OC_SMOOTH_REFL=1 / 0 overrides it (side-by-side runs without touching the ini)
      const char* smooth_env = std::getenv("OC_SMOOTH_REFL");
      meshes.set_smooth_reflections(smooth_env ? smooth_env[0] == '1' : config.smooth_reflections);
      view.set_spell_lights(config.lighting || std::getenv("OC_LIGHTING") != nullptr);
      // The game's [Video] Widescreen / WideHudEdges, from this window's shape.
      if (config.widescreen) {
        const float wx = render::wide_extra_for(static_cast<float>(ctx.extent().width), static_cast<float>(ctx.extent().height));
        render::set_wide_extra(wx);
        render::set_screen_extra(wx);
      }
      view.set_wide_hud_columns(!config.widescreen || config.wide_hud == 0 ? 0.0f : config.wide_hud == 1 ? std::min(render::wide_extra(), render::wide_extra_for(16.0f, 9.0f)) : render::wide_extra());
      view.set_auto(bauto);
      view.debug_ui(ui_mode);
      game::BattleView::Debug dbg;
      // --castat N: the intro plays, the cast is set at frame N.
      dbg.cast_element = cast_at > 0 ? 0 : cast_element;
      dbg.effects = fx_on;
      dbg.enemy_cast = enemy_cast;
      if (cast_at <= 0) dbg.cast_actor = caster, dbg.cast_target = casttgt;
      dbg.weapon = weapon;
      dbg.overlay = overlay;
      dbg.overlay_key = true;
      view.set_debug(dbg);
      platform::Input input;
      std::array<bool, 4> stick_dpad{};
      platform::FrameTimer timer;
      int frames = 0, lost_frames = 0;
      while (rc == 0 && window.pump(input)) {
        platform::stick_to_dpad(input, stick_dpad);
        if (input.key_pressed[SDL_SCANCODE_ESCAPE]) break;
        if (input.key_pressed[SDL_SCANCODE_F1]) {
          hd = !hd;
          files.set_hd_mode(hd);
          view.set_art_mode(hd);
        }
        if (auto_keys && frames < auto_frames) {
          // one confirm every 12 frames: Attack -> Weak -> first target (the attack is applied at once), the menu returns, repeat
          static const SDL_Scancode kScript[] = {SDL_SCANCODE_RETURN};
          if (frames % 12 == 5) input.key_pressed[static_cast<size_t>(kScript[(frames / 12) % (sizeof kScript / sizeof kScript[0])])] = true;
        }
        for (const auto& [key_frame, scancode] : key_script)
          if (key_frame == frames) input.key_pressed[static_cast<size_t>(scancode)] = true;
        if (frames == trans_frame) view.begin_transition(ctx);
        // --weapon first, then --castat: the attack gives the cast its history.
        if (cast_at > 0 && frames == cast_at) {
          dbg.cast_element = cast_element;
          dbg.cast_actor = caster;
          dbg.cast_target = casttgt;
          dbg.weapon = -1;
          view.set_debug(dbg);
        }
        // The second cast: same caster, the --cast2tgt unit (default --casttgt).
        if (cast2 > 0 && cast2_at > 0 && frames == cast2_at) {
          dbg.cast_element = cast2;
          dbg.cast_actor = caster;
          dbg.cast_target = cast2_tgt >= 0 ? cast2_tgt : casttgt;
          dbg.weapon = -1;
          view.set_debug(dbg);
        }
        // Determinism: with a fixed dt the frame is acquired before the update, so a failed begin_frame (occluded /
        // minimised window) skips the tick as well and the --castat frame always lands on the same battle tick.
        const bool fixed_dt = shot_frames >= 0 || shot_every > 0;
        render::Frame frame;
        bool have_frame = false;
        if (fixed_dt) {
          if (!ctx.begin_frame(frame)) {
            ++lost_frames;
            SDL_Delay(10);
            continue;
          }
          have_frame = true;
        }
        const double real_dt = timer.tick();
        // --shot: one tick per frame, deterministic.
        view.update(shot_frames >= 0 || shot_every > 0 ? 1.0f / 60.0f : static_cast<float>(real_dt), input);
        if (mixer) mixer->capture_tick(1.0 / 60.0);
        if (view.finished()) break;
        if (!have_frame && !ctx.begin_frame(frame)) {
          SDL_Delay(10);
          continue;
        }
        if (shot_frames >= 0 && frames == shot_frames) ctx.request_screenshot(exe / "battle_shot.png");
        if (shot_every > 0 && frames % shot_every == 0 && frames >= shot_from && frames <= shot_to) {
          char name[32];
          std::snprintf(name, sizeof name, "shot_%06d.png", frames);
          ctx.request_screenshot((shot_dir.empty() ? exe : shot_dir) / name);
          log::debug("shot frame {}", frames);
        }
        view.draw(frame);
        view.draw_transition(frame, false);
        ctx.end_frame();
        ++frames;
        if (shot_frames >= 0 && frames > shot_frames + 3) break;
        if (max_frames >= 0 && frames > max_frames) break;
      }
      if (lost_frames) log::warn("ocbattleview: {} frame acquires failed (ticks skipped with them)", lost_frames);
      if (mixer) mixer->trace_report("end of run");
    }
  }
  ctx.wait_idle();
  return rc;
}
