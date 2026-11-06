// Texture choice of battle player models: in both art modes every player folder's mesh is drawn on its .ltc (the HD .ltd copies
// and a disc .ltd under an HD mesh sample another atlas); in HD art with the HD battle textures on, the left half of the Status-screen TIM
// is the 4x image of that page and the eye strip's shut frame comes from its right half. Needs the game install (SKIP without one).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  // HD art, HD battle textures: 4x image of the .ltc page (mean colour error of the box-filtered image), the shut cells of the right half.
  files.set_hd_mode(true);
  int hd_checked = 0;
  for (const std::string& stem : game::model_stems(files)) {
    if (!stem.starts_with("battle/player/")) continue;
    const auto tim = files.read("hd/menu/player/" + stem.substr(stem.find_last_of('/') + 1) + ".tim");
    if (!tim) continue;
    std::string err;
    const auto asset = game::read_model_files(files, stem, &err, true);
    const auto full = import::load_hd_tim(*tim);
    ++hd_checked;
    if (!asset || !full || !asset->has_hd(0) || asset->hd_scale != 4 || asset->hd_palette.size() != 256) {
      std::printf("FAIL %s: no 4x HD battle texture %s\n", stem.c_str(), err.c_str());
      ++failures;
      continue;
    }
    const import::Image& one = asset->textures[0];
    const import::Image& hd = asset->hd_textures[0];
    double error = 0;
    for (u32 y = 0; y < one.height; ++y)
      for (u32 x = 0; x < one.width; ++x)
        for (int channel = 0; channel < 3; ++channel) {
          int sum = 0;
          for (u32 k = 0; k < 16; ++k) sum += hd.pixels[((static_cast<size_t>(y) * 4 + k / 4) * hd.width + x * 4 + k % 4) * 4 + static_cast<size_t>(channel)];
          error += std::abs(sum / 16.0 - one.pixels[(static_cast<size_t>(y) * one.width + x) * 4 + static_cast<size_t>(channel)]);
        }
    error /= 3.0 * one.width * one.height;
    if (hd.width != one.width * 4 || hd.height != one.height * 4 || error > 16.0) {
      std::printf("FAIL %s: HD page %ux%u, mean colour error %.1f\n", stem.c_str(), hd.width, hd.height, error);
      ++failures;
    }
    if (!asset->eye.valid) continue;
    const size_t strip = asset->main_texture_count;
    if (!asset->has_hd(strip) || asset->hd_textures[strip].width != asset->textures[strip].width * 4) {
      std::printf("FAIL %s: no 4x eye strip\n", stem.c_str());
      ++failures;
      continue;
    }
    const import::Image& cells = asset->hd_textures[strip];
    for (size_t half = 0; half < 2; ++half) {
      const size_t from = ((static_cast<size_t>(asset->eye.y[half]) * 4 + 5) * full->width + full->width / 2 + asset->eye.x[half] * 4u + 7) * 4;
      const size_t to = (5 * static_cast<size_t>(cells.width) + (64 + 32 * half) * 4 + 7) * 4;
      if (std::memcmp(&full->pixels[from], &cells.pixels[to], 4) != 0) {
        std::printf("FAIL %s: shut cell %zu is not the TIM's right half\n", stem.c_str(), half);
        ++failures;
      }
    }
  }
  if (hd_checked < 50) {
    std::printf("FAIL only %d battle players with a Status-screen TIM\n", hd_checked);
    ++failures;
  }
  if (checked == 0) {
    std::printf("FAIL no battle player with an .ltc\n");
    ++failures;
  }
  std::printf("%d player models checked, %d failures\n", checked, failures);
  return failures == 0 ? 0 : 1;
}
