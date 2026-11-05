// ochd - validates the RDE HD textures: decodes every hd/**/*.tim and, where a 1x counterpart exists
// (cdrom/<same path>), checks the HD image is exactly 4x its size.
//   ochd [-g <gamedir>] [-v]                 -v also lists the members without a 1x counterpart
//   ochd [-g <gamedir>] -m <name> <outdir>   debug export of field model fieldobj/player/<name> from the HD-mode override set
//                                            (cdrom2 before cdrom) with its hd.dat texture: <outdir>/<name>.obj plus a size summary
// Exit code 0 = no decode failures and no size mismatches (members without a 1x counterpart are only reported).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "import/model.h"
#include "import/tim.h"
#include "platform/paths.h"
#include "tool_util.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

int export_hd_model(const vfs::Vfs& game_fs, const std::string& name, const std::string& outdir) {
  const std::string stem = "fieldobj/player/" + name + "/" + name;
  auto pick = [&](const std::string& rel) {
    auto bytes = game_fs.read("cdrom2/" + rel);
    return bytes ? bytes : game_fs.read("cdrom/" + rel);
  };
  auto obj = pick(stem + ".obj");
  auto tim = pick(stem + ".tim");
  auto ex = game_fs.read("cdrom/fieldobj/ex/" + name + ".bin");
  auto hd_tim = game_fs.read("hd/" + stem + ".tim");
  if (!obj || !tim) {
    std::fprintf(stderr, "model %s not found\n", name.c_str());
    return 1;
  }
  std::string err;
  auto model = import::load_field_model(*obj, *tim, ex ? ByteSpan(*ex) : ByteSpan{}, &err, hd_tim ? ByteSpan(*hd_tim) : ByteSpan{});
  if (!model) {
    std::fprintf(stderr, "load: %s\n", err.c_str());
    return 1;
  }
  std::filesystem::create_directories(outdir);
  FILE* file = std::fopen((outdir + "/" + name + ".obj").c_str(), "wb");
  if (!file) return 1;
  tool::write_obj_rest_pose(file, *model);
  std::fclose(file);
  std::printf("%s: %zux%u sd, hd=%d (%ux%u), obj bytes %zu (cdrom2 %d)\n", name.c_str(), static_cast<size_t>(model->textures[0].width),
              model->textures[0].height, static_cast<int>(model->has_hd()), model->has_hd() ? model->hd_textures[0].width : 0,
              model->has_hd() ? model->hd_textures[0].height : 0, obj->size(), static_cast<int>(game_fs.read("cdrom2/" + stem + ".obj").has_value()));
  return 0;
}

}  // namespace

int ochd_main(int argc, char** argv) {
  std::string gamedir;
  bool verbose = false;
  std::string model_name, model_out;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-g" && i + 1 < argc) gamedir = argv[++i];
    else if (arg == "-v") verbose = true;
    else if (arg == "-m" && i + 2 < argc) {
      model_name = argv[i + 1];
      model_out = argv[i + 2];
      i += 2;
    } else {
      std::fprintf(stderr, "usage: ochd [-g <gamedir>] [-v] [-m <model> <out>]\n");
      return 2;
    }
  }
  if (gamedir.empty()) gamedir = tool::env_game_dir();
  vfs::Vfs game_fs;
  auto dir = platform::find_game_dir(gamedir);
  if (dir.empty() || !game_fs.open(dir)) {
    std::fprintf(stderr, "game directory not found (use -g <dir> or OPENCROSS_GAMEDIR)\n");
    return 1;
  }
  if (!model_name.empty()) return export_hd_model(game_fs, model_name, model_out);
  int total = 0, bad = 0, paired = 0, mismatched = 0;
  std::vector<std::string> unpaired;
  for (const std::string& name : game_fs.list("hd")) {
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".tim") != 0) continue;
    ++total;
    const auto bytes = game_fs.read("hd/" + name);
    auto hd = bytes ? import::load_hd_tim(*bytes) : std::nullopt;
    if (!hd) {
      std::printf("FAIL decode hd/%s\n", name.c_str());
      ++bad;
      continue;
    }
    const auto sd_bytes = game_fs.read("cdrom/" + name);
    if (!sd_bytes) {
      unpaired.push_back(name + " (" + std::to_string(hd->width) + "x" + std::to_string(hd->height) + ")");
      continue;
    }
    auto sd_tim = import::parse_tim(*sd_bytes);
    auto sd = sd_tim ? import::decode_tim(*sd_tim, 0) : std::nullopt;
    if (!sd) {
      std::printf("FAIL decode cdrom/%s\n", name.c_str());
      ++bad;
      continue;
    }
    ++paired;
    if (hd->width != sd->width * 4 || hd->height != sd->height * 4) {
      std::printf("MISMATCH %s: hd %ux%u vs 1x %ux%u\n", name.c_str(), hd->width, hd->height, sd->width, sd->height);
      ++mismatched;
    } else if (verbose) {
      std::printf("ok %s %ux%u = 4 x %ux%u\n", name.c_str(), hd->width, hd->height, sd->width, sd->height);
    }
  }
  std::printf("hd tims: %d, decode failures: %d, with 1x counterpart: %d (size mismatches: %d), no 1x counterpart: %zu\n",
              total, bad, paired, mismatched, unpaired.size());
  if (verbose)
    for (const auto& entry : unpaired) std::printf("  unpaired %s\n", entry.c_str());
  return (bad || mismatched) ? 1 : 0;
}
