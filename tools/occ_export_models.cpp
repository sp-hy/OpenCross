// occ export-models [field|battle|menu|weapons|all] [--classic] [--hd] [--only TEXT] [--verify] <outdir>
//   The same headless export as the Mods tab's "Export all models" button, into <outdir> instead of <GameDir>\dump\models: every model as
//   glTF 2.0 binary under <outdir>\classic\ and <outdir>\hd\ (--classic / --hd: one art set only). No kind named = all. --only TEXT exports
//   the models whose member stem contains TEXT ("selju", "battle/wepon/sword"). --verify reads every file back and compares counts, joints
//   and the skinned vertices at every keyframe of every clip with the engine's pose (one "model check" line each). Prints the summary.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "game/model_export.h"
#include "vfs/vfs.h"

using namespace oc;

int occ_export_models(vfs::Vfs& game_fs, const std::vector<std::string>& args) {
  unsigned what = 0;
  std::string outdir, only;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "field") what |= game::kExportField;
    else if (args[i] == "battle") what |= game::kExportBattle;
    else if (args[i] == "menu") what |= game::kExportMenu;
    else if (args[i] == "weapons") what |= game::kExportWeapons;
    else if (args[i] == "all") what |= game::kExportAll;
    else if (args[i] == "--classic") what |= game::kExportClassic;
    else if (args[i] == "--hd") what |= game::kExportHd;
    else if (args[i] == "--verify") what |= game::kExportVerify;
    else if (args[i] == "--only" && i + 1 < args.size()) only = args[++i];
    else outdir = args[i];
  }
  if (!(what & game::kExportAll)) what |= game::kExportAll;
  if (outdir.empty()) {
    std::fprintf(stderr, "usage: occ [-g <gamedir>] export-models [field|battle|menu|weapons|all] [--classic] [--hd] [--only TEXT] [--verify] <outdir>\n");
    return 2;
  }
  game::ModelExportProgress progress;
  std::atomic<bool> stop{false};
  std::thread watcher([&] {
    while (!stop) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (!stop && progress.running)
        std::fprintf(stderr, "  %s %u / %u (%u files)\n", progress.phase().c_str(), progress.done.load(), progress.total.load(), progress.files.load());
    }
  });
  const int rc = game::run_model_export(game_fs.game_dir(), outdir, what, progress, only);
  stop = true;
  watcher.join();
  std::printf("%s\n", progress.summary().c_str());
  std::printf("export-models: %s\n", outdir.c_str());
  return rc;
}
