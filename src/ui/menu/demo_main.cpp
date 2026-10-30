// ocmenudemo: the main menu in its own window on slot 4 of a save copy (writes go to a separate menu save file only).
//   ocmenudemo [--shots [dir]] [--orig] [--drive] [--slot N]      --slot: save slot used as the state (default 4); --drive plays a scripted key sequence through every screen and logs the
//   resulting state (smoke test of the input handling). F3 opens the menu, F4 runs the screenshot sequence, F1 art mode, Esc quits.
#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "core/log.h"
#include "import/text.h"
#include "platform/config.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/gpu_context.h"
#include "render/mesh_pipeline.h"
#include "ui/menu/menu_demo.h"
#include "ui/menu/status_model.h"

int ocmenudemo_main(int argc, char** argv) {
  using namespace oc;
  bool shots = false, hd = true, drive = false;
  std::filesystem::path shot_dir;
  import::Lang lang = import::Lang::En;
  int slot = 4;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--shots")) {
      shots = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') shot_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--lang") && i + 1 < argc) {
      import::parse_lang(argv[++i], lang);
    } else if (!std::strcmp(argv[i], "--orig")) {
      hd = false;
    } else if (!std::strcmp(argv[i], "--slot") && i + 1 < argc) {
      slot = std::atoi(argv[++i]);
    } else if (!std::strcmp(argv[i], "--drive")) {
      drive = true;
    }
  }
  const std::filesystem::path exe = platform::exe_dir();
  log::init((exe / "ocmenudemo.log").string());
  platform::Config config;
  config.load(exe / "opencross.ini");
  const std::filesystem::path game_dir = platform::find_game_dir(config.game_dir);
  vfs::Vfs files;
  if (game_dir.empty() || !files.open(game_dir)) {
    log::error("game dir not found");
    return 1;
  }
  platform::Window window;
  if (!window.create("OpenCross menu", 1280, 960, false)) return 1;
  render::GpuContext ctx;
  if (!ctx.init(window.handle(), true)) return 1;
  ctx.set_clear_color(0.10f, 0.14f, 0.24f);
  render::SpriteBatch sprites;
  if (!sprites.init(ctx)) return 1;

  int rc = 0;
  render::MeshPipeline meshes;
  if (!meshes.init(ctx)) return 1;
  ui::StatusModelView status_model(meshes, files);
  {
    ui::MenuDemo demo;
    if (!demo.init(ctx, files, sprites, lang, slot)) {
      log::error("menu demo failed to initialise");
      rc = 1;
    } else {
      demo.menu().set_model_view(&status_model);
      demo.set_mode(hd ? ui::ArtMode::Hd : ui::ArtMode::Original);
      demo.open();
      if (shots) demo.screenshot_sequence(ctx, shot_dir.empty() ? exe / "menu_shots" : shot_dir);
      platform::Input input;
      std::array<bool, 4> stick_dpad{};
      bool quit_after = shots;
      int drive_frame = 0;
      while (rc == 0 && window.pump(input)) {
        platform::stick_to_dpad(input, stick_dpad);
        if (input.key_pressed[SDL_SCANCODE_ESCAPE]) break;
        if (input.key_pressed[SDL_SCANCODE_F3]) demo.open();
        if (input.key_pressed[SDL_SCANCODE_F4]) demo.screenshot_sequence(ctx, exe / "menu_shots");
        if (input.key_pressed[SDL_SCANCODE_F1]) {
          hd = !hd;
          demo.set_mode(hd ? ui::ArtMode::Hd : ui::ArtMode::Original);
        }
        if (drive) {
          // One scripted key every 48 frames (past the element grid input holds); the log shows the screen after each key.
          static const SDL_Scancode kScript[] = {
              // Status: pick member
              SDL_SCANCODE_RETURN,
              // status, next member, back
              SDL_SCANCODE_RETURN, SDL_SCANCODE_E, SDL_SCANCODE_BACKSPACE,
              // Elements -> Grid -> member
              SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN,
              // back to the first member, enter grid, Right to Auto Allocate, ask
              SDL_SCANCODE_UP, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN,
              // member only
              SDL_SCANCODE_RETURN,
              // back, Equip, member
              SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_RETURN,
              // weapon list, equip, back
              SDL_SCANCODE_RETURN, SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_BACKSPACE,
              // Key items
              SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_DOWN, SDL_SCANCODE_BACKSPACE,
              // Settings
              SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
              // Save: confirm Yes
              SDL_SCANCODE_DOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_RETURN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RETURN,
              SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_BACKSPACE};
          constexpr int kScriptLength = static_cast<int>(sizeof kScript / sizeof kScript[0]);
          constexpr int kStep = 48;
          const int step = drive_frame / kStep;
          if (drive_frame % kStep == 3 && step < kScriptLength) input.key_pressed[static_cast<size_t>(kScript[step])] = input.key_down[static_cast<size_t>(kScript[step])] = true;
          // Events are persistent, so the key is released one frame after the press.
          if (drive_frame % kStep == 4 && step < kScriptLength) input.key_down[static_cast<size_t>(kScript[step])] = false;
          if (drive_frame % kStep == kStep - 1 && step < kScriptLength) {
            ui::GameState& state = demo.state();
            int placed = 0, stock = 0;
            for (int row = 0; row < 8; ++row)
              for (int col = 0; col < 10; ++col) placed += state.cell_element(0, row, col) > 0 && state.cell_element(0, row, col) < 0x80;
            for (const auto& entry : state.slot.elements) stock += entry.qty;
            log::info("drive {:2}: screen {:<14} weapon {} grid elements {} stock {} opts {:04X} closed {}", step,
                      ui::menu_screen_name(demo.menu().screen()), state.equipped(0, 0), placed, stock, state.options(), demo.menu().closed());
          }
          if (step >= kScriptLength + 2) break;
          ++drive_frame;
        }
        demo.update(input);
        if (quit_after && !demo.sequence_active()) break;
        render::Frame frame;
        if (!ctx.begin_frame(frame)) {
          SDL_Delay(10);
          continue;
        }
        demo.draw(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
        ctx.end_frame();
      }
    }
  }
  ctx.wait_idle();
  status_model.release();
  return rc;
}
