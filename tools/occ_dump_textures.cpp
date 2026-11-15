// occ dump-textures [stages|title|effects|rooms|models|weapons|all] [--raw] [--source] <outdir>
//   The same headless dump as the Mods tab's "Dump all textures" button, into <outdir> instead of <GameDir>\dump\textures, in the
//   texture registry's layout (a folder tree copied below a mod as `textures\...` replaces the textures). The category names are the
//   Mods tab's checkboxes; no category (or only --raw / --source) means everything, --raw adds the raw atlases under raw/ and --source
//   writes the models at their 1x size instead of RDE's 4x images (the "Models at 1x" checkbox). Prints per-phase
//   file counts and the time taken; progress goes to stderr every 5 s while the dump runs.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "game/battle/texture_dump.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

using namespace oc;

int occ_dump_textures(vfs::Vfs& game_fs, const std::vector<std::string>& args) {
  unsigned categories = 0;
  std::string outdir;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "stages") categories |= game::kDumpStages;
    else if (args[i] == "title") categories |= game::kDumpTitle;
    else if (args[i] == "effects") categories |= game::kDumpEffects;
    else if (args[i] == "rooms") categories |= game::kDumpRooms;
    else if (args[i] == "models") categories |= game::kDumpModels;
    else if (args[i] == "weapons") categories |= game::kDumpWeapons;
    else if (args[i] == "all") categories |= game::kDumpAll;
    else if (args[i] == "--raw") categories |= game::kDumpRaw;
    else if (args[i] == "--source") categories |= game::kDumpModelSource;
    else outdir = args[i];
  }
  if (!(categories & game::kDumpAll)) categories |= game::kDumpAll;
  if (outdir.empty()) {
    std::fprintf(stderr, "usage: occ [-g <gamedir>] dump-textures [stages|title|effects|rooms|models|weapons|all] [--raw] [--source] <outdir>\n");
    return 2;
  }
  std::filesystem::create_directories(outdir);
  render::texture_registry::set_dump_root(outdir);
  game::TextureDumpProgress progress;
  std::atomic<bool> stop{false};
  std::thread watcher([&] {
    while (!stop) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      const std::string phase = progress.phase();
      if (!stop && progress.running) std::fprintf(stderr, "  %s %u / %u (%u files)\n", phase.c_str(), progress.done.load(), progress.total.load(), progress.files.load());
    }
  });
  const int rc = game::run_texture_dump(game_fs.game_dir(), categories, progress);
  stop = true;
  watcher.join();
  std::printf("%s\n", progress.summary().c_str());
  std::printf("dump-textures: %s\n", outdir.c_str());
  return rc;
}
