// Texture choice of battle player models: in both art modes every player folder's mesh is drawn on its .ltc (the HD .ltd copies
// and a disc .ltd under an HD mesh sample another atlas). Needs the game install (SKIP without one).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "game/model_files.h"
#include "import/model.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

using namespace oc;

int main() {
  const char* configured = std::getenv("OPENCROSS_GAMEDIR");
  const auto dir = platform::find_game_dir(configured ? configured : "");
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  int failures = 0;
  int checked = 0;
  for (const bool hd : {false, true}) {
    files.set_hd_mode(hd);
    for (const std::string& stem : game::model_stems(files)) {
      if (!stem.starts_with("battle/player/")) continue;
      const std::string base = "cdrom/" + stem;
      const auto ltc = files.read(base + ".ltc");
      const auto obj = files.read(base + ".obj");
      if (!ltc || !obj) continue;
      std::string err;
      const auto asset = game::read_model_files(files, stem, &err);
      const auto expected = import::load_model(*obj, *ltc, {}, &err);
      ++checked;
      if (!asset || !expected || asset->textures.empty() || expected->textures.empty() ||
          asset->textures[0].pixels != expected->textures[0].pixels || asset->textures[0].palette != expected->textures[0].palette) {
        std::printf("FAIL %s (%s art): main texture is not the .ltc image %s\n", stem.c_str(), hd ? "HD" : "original", err.c_str());
        ++failures;
      }
    }
  }
  if (checked == 0) {
    std::printf("FAIL no battle player with an .ltc\n");
    ++failures;
  }
  std::printf("%d player models checked, %d failures\n", checked, failures);
  return failures == 0 ? 0 : 1;
}
