// ocflow: the game flow screens in their own window (no field).
//   ocflow                     interactive: Title -> New Game -> name entry (logs the FlowAction) / Continue -> slot list
//   ocflow --shots [dir]       writes title / name entry / slot list screenshots for both art modes, then exits
//   ocflow --orig              start in the original (PS1) art mode      --lang <en|fr|it|de|es|jp>
// Save file for Continue: <exe>\..\..\..\tmp\save.dat if present, else the game's own save folder.
#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>
#include <functional>

#include "core/log.h"
#include "game/game_flow.h"
#include "platform/config.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/gpu_context.h"

namespace {

using namespace oc;

struct Step {
  std::function<void()> setup;
  int frames = 1;
  std::string shot;
};

}  // namespace

int ocflow_main(int argc, char** argv) {
  bool shots = false, hd = true;
  std::filesystem::path shot_dir, save_path;
  std::string lang_code = "en";
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--shots")) {
      shots = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') shot_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--orig")) {
      hd = false;
    } else if (!std::strcmp(argv[i], "--lang") && i + 1 < argc) {
      lang_code = argv[++i];
    } else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) {
      save_path = argv[++i];
    }
  }
  const std::filesystem::path exe = platform::exe_dir();
  log::init((exe / "ocflow.log").string());
  platform::Config config;
  config.load(exe / "opencross.ini");
  import::Lang lang = import::Lang::En;
  import::parse_lang(lang_code, lang);
  const std::filesystem::path game_dir = platform::find_game_dir(config.game_dir);
  vfs::Vfs files;
  if (game_dir.empty() || !files.open(game_dir)) {
    log::error("game dir not found");
    return 1;
  }
  platform::Window window;
  if (!window.create("OpenCross flow", 1280, 960, false)) return 1;
  render::GpuContext ctx;
  if (!ctx.init(window.handle(), true)) return 1;
  ctx.set_clear_color(0.0f, 0.0f, 0.0f);
  render::SpriteBatch sprites;
  if (!sprites.init(ctx)) return 1;

  int rc = 0;
  {
    data::GameTables tables;
    const bool have_tables = tables.load(files);
    audio::Mixer mixer;
    game::GameFlow flow;
    if (!flow.init(ctx, files, sprites, have_tables ? &tables : nullptr, lang, shots ? nullptr : &mixer)) {
      log::error("flow init failed");
      return 1;
    }
    // Save file: an explicit path, the repo's tmp\save.dat (walking up from the exe), else the real save folder.
    if (save_path.empty()) {
      for (std::filesystem::path dir = exe; !dir.empty() && dir != dir.root_path(); dir = dir.parent_path())
        if (std::filesystem::exists(dir / "tmp" / "save.dat")) {
          save_path = dir / "tmp" / "save.dat";
          break;
        }
    }
    save::SaveDat dat;
    if (!save_path.empty()) {
      try {
        dat = save::SaveDat::load(save_path);
        flow.set_save_dat(&dat);
        log::info("save: {}", save_path.string());
      } catch (const std::exception& error) {
        log::warn("save {}: {}", save_path.string(), error.what());
      }
    }
    flow.set_sfx([](int id) { log::debug("sfx {:#x}", id); });
    const auto mode_of = [](bool use_hd) { return use_hd ? ui::ArtMode::Hd : ui::ArtMode::Original; };
    flow.set_art_mode(mode_of(hd));
    flow.start(game::FlowState::Title);

    std::vector<Step> steps;
    if (shots) {
      if (shot_dir.empty()) shot_dir = exe / "flow_shots";
      std::filesystem::create_directories(shot_dir);
      for (bool use_hd : {true, false}) {
        const std::string suffix = use_hd ? "hd" : "orig";
        steps.push_back({[&flow, &mode_of, use_hd] {
                           flow.set_art_mode(mode_of(use_hd));
                           flow.start(game::FlowState::Title);
                         },
                         45, "title_prompt_" + suffix + ".png"});
        steps.push_back({[&flow] { flow.title().skip_to_menu(); }, 3, "title_menu_" + suffix + ".png"});
        steps.push_back({[&flow] {
                           flow.start(game::FlowState::NameEntry);
                           flow.name_entry().press(ui::NameKey::Down);
                           flow.name_entry().press(ui::NameKey::Right);
                           flow.name_entry().press(ui::NameKey::Right);
                         },
                         3, "name_entry_" + suffix + ".png"});
        steps.push_back({[&flow] {
                           flow.start(game::FlowState::NameEntry);
                           flow.name_entry().press(ui::NameKey::Left);
                         },
                         3, "name_entry_change_" + suffix + ".png"});
        steps.push_back({[&flow] { flow.name_entry().press(ui::NameKey::Down); }, 3, "name_entry_confirm_" + suffix + ".png"});
        steps.push_back({[&flow] { flow.start(game::FlowState::SlotSelect); }, 3, "slots_" + suffix + ".png"});
      }
    }

    platform::Input input;
    std::array<bool, 4> stick_dpad{};
    size_t si = 0;
    int step_frame = 0;
    bool step_started = false;
    platform::FrameTimer timer;
    while (rc == 0 && window.pump(input)) {
      platform::stick_to_dpad(input, stick_dpad);
      if (input.key_pressed[SDL_SCANCODE_ESCAPE]) break;
      float dt = static_cast<float>(timer.tick());
      if (shots) {
        if (si >= steps.size()) break;
        if (!step_started) {
          steps[si].setup();
          step_started = true;
          step_frame = 0;
        }
        dt = 1.0f / 60.0f;
      } else if (input.key_pressed[SDL_SCANCODE_F1]) {
        hd = !hd;
        flow.set_art_mode(mode_of(hd));
      }
      const game::FlowAction result = flow.update(input, dt);
      if (result.quit) break;
      if (result.load_room)
        log::info("flow: load_room {} param {} restore {} ({}, {}) state_changed {}", result.room, result.param, result.restore_position, result.x, result.z, result.state_changed);
      if (result.open_settings) log::info("flow: open settings");
      if (result.load_room && !shots) {
        log::info("flow: reached {} -> restarting at the title", game::flow_state_name(flow.state()));
        flow.start(game::FlowState::Title);
      }
      render::Frame frame;
      if (!ctx.begin_frame(frame)) {
        SDL_Delay(10);
        continue;
      }
      if (shots && ++step_frame == steps[si].frames) ctx.request_screenshot(shot_dir / steps[si].shot);
      flow.draw(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
      ctx.end_frame();
      if (shots && step_frame >= steps[si].frames) {
        log::info("shot {}", (shot_dir / steps[si].shot).string());
        ++si;
        step_started = false;
      }
    }
    ctx.wait_idle();
    flow.shutdown();
  }
  return rc;
}
