// The built-in disc file indexes: DiscIndex falls back to them without cdrom.fnd, every named record round-trips through DiscNames
// (path -> record -> the same path), and with the game install they equal RDE's cdrom.fnd / cdrom2.fnd (that part SKIPs without one).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/disc_index.h"
#include "platform/paths.h"
#include "vfs/disc_names.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// A source with no files at all (no cdrom.fnd).
class EmptyBackend final : public vfs::Backend {
 public:
  bool has_archive(std::string_view archive) const override { return archive == "cdrom"; }
  bool contains(std::string_view, const std::string&, int) const override { return false; }
  std::optional<Bytes> read(std::string_view, const std::string&, int) const override { return std::nullopt; }
  std::vector<std::string> list(std::string_view) const override { return {}; }
};

void check_round_trip() {
  const vfs::DiscNames& names = vfs::DiscNames::builtin();
  int named = 0, unnamed = 0;
  for (int disc = 1; disc <= 2; ++disc) {
    const std::vector<std::string>& records = vfs::builtin_disc_records(disc);
    CHECK(records.size() == 6060);
    for (size_t record = 0; record < records.size(); ++record) {
      const int index = static_cast<int>(record);
      const std::string member = vfs::disc_record_member(records[record]);
      if (member.empty()) {
        ++unnamed;
        CHECK(names.find(vfs::DiscNames::record_path(index), disc).disc == index);
        continue;
      }
      ++named;
      const std::vector<int> all = names.records_of("cdrom/" + member, disc);
      const vfs::DiscNames::Records found = names.find("cdrom/" + member, disc);
      CHECK(!all.empty() && found.disc == all.front() && found.disc <= index);
      CHECK(std::find(all.begin(), all.end(), index) != all.end());
      CHECK(vfs::disc_record_member(records[static_cast<size_t>(found.disc)]) == member);
    }
  }
  std::printf("%d named and %d unnamed records round-trip, %zu members\n", named, unnamed, names.members().size());
  for (const std::string& member : names.members()) {
    const vfs::DiscNames::Records found = names.find("cdrom/" + member, 1);
    CHECK(found.disc >= 0 || found.other >= 0);
  }
  // Shared names and placeholders resolve to their lowest record.
  for (const char* shared : {"cdrom/dummy/dummy.dat", "cdrom/map/mapbin/test.prd", "cdrom/prog/stdtech0.prg"}) {
    const std::vector<int> all = names.records_of(shared, 2);
    CHECK(all.size() > 1 && names.find(shared, 2).disc == all.front());
  }
  // A file only one disc's index names: the other disc's record is the fallback.
  const vfs::DiscNames::Records disc1_room = names.find("cdrom/map/mapbin/bak_a02f.bin", 2);
  CHECK(disc1_room.disc == -1 && disc1_room.other == game::DiscIndex::kMapBase + 3 * 459);
  CHECK(names.find("cdrom/prog/slps_023.64", 1).disc == 0 && names.find("cdrom/movie/title.stv", 1).disc >= game::DiscIndex::kMovieBase);
}

void check_index_fallback() {
  vfs::Vfs files;
  files.open(std::make_unique<EmptyBackend>(), {});
  game::DiscIndex index;
  CHECK(index.load(files) && index.loaded());
  CHECK(index.size(1) == 6060 && index.size(2) == 6060);
  CHECK(index.room_stem(1, 459) == "bak_a02f" && index.room_stem(2, 459) == "fin_06");
  CHECK(index.record(1, 0) == "c:/work/cdrom/prog/slps_023.64");
}

// The built-in table equals RDE's files, and DiscIndex reads RDE's files when they are there.
void check_against_install() {
  const char* configured = std::getenv("OPENCROSS_GAMEDIR");
  const auto dir = platform::find_game_dir(configured ? configured : "");
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return;
  }
  const auto disc1 = files.read_disc("cdrom/cdrom.fnd");
  const auto disc2 = files.read_disc("cdrom/cdrom2.fnd");
  CHECK(disc1 && disc2);
  if (!disc1 || !disc2) return;
  CHECK(vfs::parse_disc_records(*disc1) == vfs::builtin_disc_records(1));
  CHECK(vfs::parse_disc_records(*disc2) == vfs::builtin_disc_records(2));
  game::DiscIndex index;
  CHECK(index.load(files));
  for (int disc = 1; disc <= 2; ++disc) {
    CHECK(index.size(disc) == vfs::builtin_disc_records(disc).size());
    for (size_t record = 0; record < index.size(disc); ++record)
      if (index.record(disc, static_cast<int>(record)) != vfs::builtin_disc_records(disc)[record]) {
        CHECK(!"record differs");
        break;
      }
  }
  // Every named file of the work tree is a cdrom.dat member under the name the lookup gives (system.cnf lies outside the file tree).
  int missing = 0;
  for (const std::string& member : vfs::DiscNames::builtin().members())
    if (!member.starts_with("movie/") && member != "system.cnf" && !files.exists("cdrom/" + member)) {
      if (++missing <= 5) std::printf("not in cdrom.dat: %s\n", member.c_str());
    }
  CHECK(missing == 0);
}

}  // namespace

int main() {
  check_round_trip();
  check_index_fallback();
  check_against_install();
  if (failures) return 1;
  std::printf("test_disc_index: ok\n");
  return 0;
}
