// VFS backends: RDE's ZIP archives (read, read_disc, exists, list, the cdrom2 override rule, mod overlay precedence) and a backend
// that serves only "cdrom" with files depending on the disc in the drive, plus DiscNames's path -> record lookup on small indexes.
#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vfs/backend.h"
#include "vfs/disc_names.h"
#include "vfs/vfs.h"

using namespace oc;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

using Entries = std::vector<std::pair<std::string, std::string>>;

void write_zip(const fs::path& file, const Entries& entries) {
  mz_zip_archive zip{};
  CHECK(mz_zip_writer_init_file(&zip, file.string().c_str(), 0));
  for (const auto& [name, data] : entries) CHECK(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
  CHECK(mz_zip_writer_finalize_archive(&zip));
  mz_zip_writer_end(&zip);
}

std::string text(const std::optional<Bytes>& bytes) { return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("<none>"); }

// Serves "cdrom" only: disc 1 and disc 2 each have their own files, and a member both have differs between them.
class TwoDiscBackend final : public vfs::Backend {
 public:
  bool has_archive(std::string_view archive) const override { return archive == "cdrom"; }
  bool contains(std::string_view archive, const std::string& member, int disc) const override { return find(archive, member, disc) != nullptr; }
  std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const override {
    const std::string* found = find(archive, member, disc);
    if (!found) return std::nullopt;
    return Bytes(found->begin(), found->end());
  }
  std::vector<std::string> list(std::string_view archive) const override {
    if (archive != "cdrom") return {};
    return {"map/a.bin", "map/both.bin", "map/b.bin"};
  }

 private:
  const std::string* find(std::string_view archive, const std::string& member, int disc) const {
    if (archive != "cdrom") return nullptr;
    const auto& files = disc >= 2 ? disc2_ : disc1_;
    auto it = files.find(member);
    return it == files.end() ? nullptr : &it->second;
  }
  std::map<std::string, std::string> disc1_ = {{"map/a.bin", "disc 1 a"}, {"map/both.bin", "disc 1 both"}};
  std::map<std::string, std::string> disc2_ = {{"map/b.bin", "disc 2 b"}, {"map/both.bin", "disc 2 both"}};
};

void check_rde_backend(const fs::path& root) {
  const fs::path game = root / "game";
  fs::create_directories(game / "data");
  write_zip(game / "data" / "cdrom.dat", {{"map/x.bin", "disc x"}, {"map/window/w0.tim", "disc w0"}, {"map/y.bin", "disc y"}});
  write_zip(game / "data" / "cdrom2.dat", {{"map/x.bin", "hd x"}, {"map/window/w0.tim", "hd w0"}});
  write_zip(game / "data" / "hd.dat", {{"bg/z.png", "hd z"}});
  vfs::Vfs files;
  CHECK(!files.has_archive("cdrom"));
  CHECK(!files.open(root / "nowhere"));
  CHECK(files.open(game));
  CHECK(files.game_dir() == game);
  CHECK(files.has_archive("cdrom") && files.has_archive("hd") && !files.has_archive("movie"));
  // Original art: only the fixed list goes to cdrom2.
  CHECK(text(files.read("cdrom/map/x.bin")) == "disc x");
  CHECK(text(files.read("CDROM\\Map\\Window\\W0.tim")) == "hd w0");
  CHECK(text(files.read_disc("cdrom/map/window/w0.tim")) == "disc w0");
  // HD art: every cdrom path tries cdrom2 first; read_disc never does.
  files.set_hd_mode(true);
  CHECK(text(files.read("cdrom/map/x.bin")) == "hd x");
  CHECK(text(files.read("cdrom/map/y.bin")) == "disc y");
  CHECK(text(files.read_disc("cdrom/map/x.bin")) == "disc x");
  CHECK(files.exists("cdrom/map/y.bin") && !files.exists("cdrom/map/none.bin"));
  CHECK(text(files.read("hd/bg/z.png")) == "hd z");
  CHECK(!files.read("movie/a01op_en.bk2"));
  // The disc number means nothing to RDE's archives.
  files.set_disc(2);
  CHECK(files.disc() == 2 && text(files.read("cdrom/map/y.bin")) == "disc y");
  CHECK(files.list("cdrom") == std::vector<std::string>({"map/x.bin", "map/window/w0.tim", "map/y.bin"}));
  CHECK(files.list("movie").empty());
  // The overlay wins over cdrom2 and the archive, in read and read_disc.
  const fs::path loose = root / "x.bin";
  std::ofstream(loose, std::ios::binary) << "mod x";
  vfs::Overlay overlay;
  overlay.files["cdrom/map/x.bin"] = {loose, ""};
  files.set_overlay(overlay);
  CHECK(text(files.read("cdrom/map/x.bin")) == "mod x" && text(files.read_disc("cdrom/map/x.bin")) == "mod x");
  CHECK(files.loose_file("cdrom/map/x.bin") == loose);
}

void check_disc_backend(const fs::path& root) {
  vfs::Vfs files;
  files.open(std::make_unique<TwoDiscBackend>(), root);
  CHECK(files.game_dir() == root && files.disc() == 1);
  for (const char* absent : {"cdrom2", "hd", "ui", "font", "lang", "movie"}) CHECK(!files.has_archive(absent));
  CHECK(files.has_archive("cdrom"));
  CHECK(!files.read("hd/bg/z.png") && !files.read("lang/en.dat") && files.list("hd").empty());
  // HD art finds no cdrom2 and reads the disc.
  files.set_hd_mode(true);
  CHECK(text(files.read("cdrom/map/both.bin")) == "disc 1 both");
  CHECK(text(files.read("cdrom/map/a.bin")) == "disc 1 a" && !files.exists("cdrom/map/b.bin"));
  // Disc swap.
  files.set_disc(2);
  CHECK(files.disc() == 2);
  CHECK(text(files.read("cdrom/map/both.bin")) == "disc 2 both" && text(files.read_disc("cdrom/map/b.bin")) == "disc 2 b");
  CHECK(!files.read("cdrom/map/a.bin") && files.exists("cdrom/map/b.bin"));
  files.set_disc(7);
  CHECK(files.disc() == 2);
  files.set_disc(0);
  CHECK(files.disc() == 1);
  CHECK(files.list("cdrom").size() == 3);
  // The mod overlay applies over any backend.
  const fs::path loose = root / "both.bin";
  std::ofstream(loose, std::ios::binary) << "mod both";
  vfs::Overlay overlay;
  overlay.files["cdrom/map/both.bin"] = {loose, ""};
  files.set_overlay(overlay);
  CHECK(text(files.read("cdrom/map/both.bin")) == "mod both" && files.exists("cdrom/map/both.bin"));
}

void check_disc_names() {
  const std::vector<std::string> disc1 = {"c:/work/cdrom/prog/slps_023.64", "", "c:/work/cdrom/prog/shared.prg", "c:/work/cdrom/map/mapbin/one.bin",
                                          "c:/work/cdrom/prog/shared.prg", "c:/work/cdrom/battle/wepon//ken/hagane.kmd", "c:/movie/title.stv"};
  const std::vector<std::string> disc2 = {"c:/work/cdrom/prog/slps_023.64", "", "c:/work/cdrom/dummy/dummy.dat", "c:/work/cdrom/map/mapbin/two.bin",
                                          "c:/work/cdrom/prog/shared.prg", "c:/work/cdrom/battle/wepon//ken/hagane.kmd", "c:/movie/end.stv"};
  const vfs::DiscNames names(disc1, disc2);
  CHECK(vfs::disc_record_member("c:/work/cdrom/battle/wepon//ken/hagane.kmd") == "battle/wepon/ken/hagane.kmd");
  CHECK(vfs::disc_record_member("c:/movie/title.stv") == "movie/title.stv" && vfs::disc_record_member("").empty());
  // A shared name: the lowest record on each disc.
  vfs::DiscNames::Records shared = names.find("cdrom/prog/shared.prg", 1);
  CHECK(shared.disc == 2 && shared.other == 4);
  CHECK(names.records_of("CDROM\\Prog\\Shared.prg", 1) == std::vector<int>({2, 4}));
  // A file of one disc: found on the other for the fallback.
  shared = names.find("cdrom/map/mapbin/one.bin", 2);
  CHECK(shared.disc == -1 && shared.other == 3);
  shared = names.find("cdrom/map/mapbin/two.bin", 2);
  CHECK(shared.disc == 3 && shared.other == -1);
  CHECK(names.find("cdrom/battle/wepon/ken/hagane.kmd", 1).disc == 5);
  CHECK(names.find("cdrom/movie/end.stv", 2).disc == 6);
  CHECK(names.find("hd/prog/shared.prg", 1).disc == -1 && names.find("cdrom/none.bin", 1).other == -1);
  // An unnamed record by number, on either disc; past the end gives nothing.
  CHECK(vfs::DiscNames::record_path(1) == "cdrom/#record/00001");
  shared = names.find(vfs::DiscNames::record_path(1), 2);
  CHECK(shared.disc == 1 && shared.other == 1);
  CHECK(names.records_of("cdrom/#record/7", 1).empty() && names.records_of("cdrom/#record/x1", 1).empty());
  CHECK(names.records_of("cdrom/#record/6", 1) == std::vector<int>({6}));
  CHECK(names.members() == std::vector<std::string>({"prog/slps_023.64", "prog/shared.prg", "map/mapbin/one.bin", "battle/wepon/ken/hagane.kmd",
                                                      "movie/title.stv", "dummy/dummy.dat", "map/mapbin/two.bin", "movie/end.stv"}));
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "oc_test_vfs";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);
  check_rde_backend(root);
  check_disc_backend(root);
  check_disc_names();
  fs::remove_all(root, ec);
  if (failures) return 1;
  std::printf("test_vfs: ok\n");
  return 0;
}
