// ocshopdemo: the shops in their own window on slot 4 of a save copy (in-memory edits only, nothing is written back).
//   ocshopdemo [--shots [dir]] [--orig]     F5 element shop, F6 blacksmith, F7 trading post (shop id 0 or 1 with Shift),
//   F4 runs the screenshot sequence, F1 art mode, Esc quits.
#include <SDL3/SDL.h>

#include <cstring>
#include <filesystem>

#include "core/log.h"
#include "import/text.h"
#include "platform/config.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/gpu_context.h"
#include "ui/shop/shop_demo.h"

int ocshopdemo_main(int argc, char** argv) {
  using namespace oc;
  bool shots = false, hd = true;
  std::filesystem::path shot_dir;
  import::Lang lang = import::Lang::En;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--shots")) {
      shots = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') shot_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--lang") && i + 1 < argc) {
      import::parse_lang(argv[++i], lang);
    } else if (!std::strcmp(argv[i], "--orig")) {
      hd = false;
    }
  }
  const std::filesystem::path exe = platform::exe_dir();
  log::init((exe / "ocshopdemo.log").string());
  platform::Config config;
  config.load(exe / "opencross.ini");
  const std::filesystem::path game_dir = platform::find_game_dir(config.game_dir);
  vfs::Vfs files;
  if (game_dir.empty() || !files.open(game_dir)) {
    log::error("game dir not found");
    return 1;
  }
  platform::Window window;
  if (!window.create("OpenCross shops", 1280, 960, false)) return 1;
  render::GpuContext ctx;
  if (!ctx.init(window.handle(), true)) return 1;
  ctx.set_clear_color(0.10f, 0.14f, 0.24f);
  render::SpriteBatch sprites;
  if (!sprites.init(ctx)) return 1;

  int rc = 0;
  {
    ui::ShopDemo demo;
    if (!demo.init(ctx, files, sprites, lang)) {
      log::error("shop demo failed to initialise");
      rc = 1;
    } else {
      demo.set_mode(hd ? ui::ArtMode::Hd : ui::ArtMode::Original);
      if (shots) demo.screenshot_sequence(ctx, shot_dir.empty() ? exe / "shop_shots" : shot_dir);
      else demo.open(ui::ShopKind::Element, 0);
      platform::Input input;
      std::array<bool, 4> stick_dpad{};
      while (rc == 0 && window.pump(input)) {
        platform::stick_to_dpad(input, stick_dpad);
        if (input.key_pressed[SDL_SCANCODE_ESCAPE]) break;
        const int id = (input.key_down[SDL_SCANCODE_LSHIFT] || input.key_down[SDL_SCANCODE_RSHIFT]) ? 1 : 0;
        if (input.key_pressed[SDL_SCANCODE_F5]) demo.open(ui::ShopKind::Element, id);
        if (input.key_pressed[SDL_SCANCODE_F6]) demo.open(ui::ShopKind::Smith, id);
        if (input.key_pressed[SDL_SCANCODE_F7]) demo.open(ui::ShopKind::Trading, id);
        if (input.key_pressed[SDL_SCANCODE_F4]) demo.screenshot_sequence(ctx, exe / "shop_shots");
        if (input.key_pressed[SDL_SCANCODE_F1]) {
          hd = !hd;
          demo.set_mode(hd ? ui::ArtMode::Hd : ui::ArtMode::Original);
        }
        demo.update(input);
        if (shots && !demo.sequence_active()) break;
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
  return rc;
}
