// Texture choice of battle player models: in both art modes every player folder's mesh is drawn on its .ltc (the HD .ltd copies
// and a disc .ltd under an HD mesh sample another atlas); in HD art with the HD battle textures on, the left half of the Status-screen TIM
// is the 4x image of that page and the eye strip's shut frame comes from its right half. The field's models from elsewhere (FE D0):
// battle models made field models, their field scale, another room's model. Needs the game install (SKIP without one).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/battle/battle_seq.h"
#include "game/battle/model_table.h"
#include "game/field_model_source.h"
#include "game/model_files.h"
#include "import/model.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

// FE D0's sources (field_model_source.h): every battle model reads and is renumbered for the field (stand / run / stance in the field
// slots, its own clips as ex clips, no animation scripts left); the party models' own scripts name stand 1 and run 3 (the stance is
// 2, but Lynx's approach ends on 1); another room's model reads with its own pages.
int check_model_sources(const vfs::Vfs& files) {
  int failures = 0, read = 0, from_scripts = 0;
  // Reading the clips runs the models' scripts; the state the battle's VMs share is left as it was.
  game::bseq::Vm::Shared before;
  before.globals[3] = 77;
  before.prev_action = 2;
  before.seq_last = 0x616263;
  game::bseq::Vm::restore(before);
  for (size_t id = 0; id < game::kBattleModels.size(); ++id) {
    const std::string stem(game::kBattleModels[id]);
    if (stem.empty()) continue;
    const auto obj = files.read("cdrom/" + stem + ".obj");
    if (!obj) continue;
    const game::BattleFieldClips clips = game::battle_field_clips(ByteSpan(*obj));
    if (clips.stand >= 0 && clips.run >= 0) ++from_scripts;
    if (stem.starts_with("battle/player/") && (clips.stand != 1 || clips.run != 3 || clips.stance < 1)) {
      std::printf("FAIL %s: party model clips stand %d run %d stance %d (want 1 3 and a stance)\n", stem.c_str(), clips.stand, clips.run,
                  clips.stance);
      ++failures;
    }
    std::string err;
    const auto asset = game::read_battle_model_for_field(files, stem, false, &err);
    if (!asset) {
      std::printf("FAIL %s: %s\n", stem.c_str(), err.c_str());
      ++failures;
      continue;
    }
    ++read;
    bool slots = true;
    for (const u32 slot : {0u, 1u, 2u, 3u, 8u}) {
      bool found = false;
      for (const assets::AnimationClip& clip : asset->clips) found = found || (clip.source == "s4" && clip.index == slot);
      slots = slots && found;
    }
    if (!slots || !asset->anim_seq.empty() || !asset->ex_anim_seq.empty()) {
      std::printf("FAIL %s: field slots 0 1 2 3 8 %s, scripts left %zu / %zu\n", stem.c_str(), slots ? "present" : "missing", asset->anim_seq.size(),
                  asset->ex_anim_seq.size());
      ++failures;
    }
  }
  const game::bseq::Vm::Shared after = game::bseq::Vm::shared();
  if (after.globals != before.globals || after.prev_action != before.prev_action || after.seq_last != before.seq_last) {
    std::printf("FAIL reading battle models changed the battle VMs' shared state\n");
    ++failures;
  }
  if (read < 200) {
    std::printf("FAIL only %d battle models read for the field\n", read);
    ++failures;
  }
  // The field scale of a battle model: field height / battle height of the same character (extent y), the median over the cast.
  std::vector<double> ratios;
  for (const char* folder : {"selju", "kid", "guill", "ishito", "slash", "jakotu", "riddle", "carshu", "zoah", "malchera", "colcha",
                             "lutianna", "poshul", "lazzuly", "zappa", "ocha", "radius", "farga", "mamacha", "gren", "lena", "miki"}) {
    std::string unused;
    const auto field = game::read_model_files(files, std::string("fieldobj/player/") + folder + "/" + folder, &unused);
    std::string battle_stem;
    for (const std::string& stem : {std::string("battle/player/") + folder + "/" + folder, std::string("battle/player/") + folder + "1/" + folder + "1",
                                    std::string("battle/player/") + folder + "2/" + folder})
      if (battle_stem.empty() && files.exists("cdrom/" + stem + ".obj")) battle_stem = stem;
    const auto battle = battle_stem.empty() ? std::nullopt : game::read_model_files(files, battle_stem, &unused);
    if (!field || !battle || field->extent[1] == 0 || battle->extent[1] == 0) continue;
    ratios.push_back(std::abs(static_cast<double>(field->extent[1])) / std::abs(static_cast<double>(battle->extent[1])));
  }
  std::sort(ratios.begin(), ratios.end());
  const double median = ratios.empty() ? 0.0 : ratios[ratios.size() / 2];
  std::printf("field / battle model height: median %.3f over %zu characters (%.3f .. %.3f)\n", median, ratios.size(),
              ratios.empty() ? 0.0 : ratios.front(), ratios.empty() ? 0.0 : ratios.back());
  if (ratios.size() < 15 || std::abs(median - game::kBattleModelFieldScale) > 0.1) {
    std::printf("FAIL kBattleModelFieldScale %.2f is not the measured median %.3f\n", game::kBattleModelFieldScale, median);
    ++failures;
  }
  std::string err;
  const auto room = game::read_room_model(files, "alu_a01f", 1, &err);
  if (!room || room->textures.empty() || room->meshes.empty()) {
    std::printf("FAIL alu_a01f model 1 from another room: %s\n", err.c_str());
    ++failures;
  }
  std::printf("%d battle models read for the field (%d with stand / run from their scripts)\n", read, from_scripts);
  return failures;
}

}  // namespace

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
  files.set_hd_mode(false);
  failures += check_model_sources(files);
  std::printf("%d player models checked, %d failures\n", checked, failures);
  return failures == 0 ? 0 : 1;
}
