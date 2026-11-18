// The PS1 discs as a VFS backend over two synthetic in-memory discs (file table, disc-number sector, small files): lookup by name on
// the disc in the drive, the fall back to the other disc, empty slots, by-number paths, the absent archives, the member list, reads
// while a disc is mounted, the same through Vfs; and the disc search: the image-file filter and folder order, grouping by build and
// the choice of the set to play.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "import/disc/disc_backend.h"
#include "import/disc/disc_search.h"
#include "vfs/disc_names.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::import::disc;
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

// A cooked disc held in memory.
class MemorySource final : public SectorSource {
 public:
  explicit MemorySource(std::vector<Bytes> sectors)
      : SectorSource(static_cast<u32>(sectors.size()), false, "memory"), sectors_(std::move(sectors)) {}

 protected:
  bool fetch(u32 lba, u32 count, u8* out) const override {
    for (u32 i = 0; i < count; ++i) std::memcpy(out + i * kUserSectorSize, sectors_[lba + i].data(), kUserSectorSize);
    return true;
  }

 private:
  std::vector<Bytes> sectors_;
};

void put32(u8* out, u32 value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<u8>(value >> (8 * i));
}

Bytes content(char tag, size_t size) {
  Bytes bytes(size);
  for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<u8>(tag + i * 3);
  return bytes;
}

// A disc whose file records are `files` in order (nullopt = an empty slot); sizes are multiples of 8.
std::shared_ptr<const SectorSource> make_disc(u32 disc_number, const std::vector<std::optional<Bytes>>& files) {
  std::vector<Bytes> sectors(38, Bytes(kUserSectorSize, 0));
  put32(sectors[kDiscNumberSector].data(), disc_number);
  for (size_t record = 0; record < files.size(); ++record) {
    const u32 lba = static_cast<u32>(sectors.size());
    u32 word = lba;
    if (!files[record]) {
      word |= 0x800000u;
    } else {
      const Bytes& data = *files[record];
      const size_t count = std::max<size_t>(1, (data.size() + kUserSectorSize - 1) / kUserSectorSize);
      word |= static_cast<u32>((count * kUserSectorSize - data.size()) / 8) << 24;
      for (size_t i = 0; i < count; ++i) {
        Bytes sector(kUserSectorSize, 0);
        const size_t at = i * kUserSectorSize;
        std::memcpy(sector.data(), data.data() + at, std::min(kUserSectorSize, data.size() - at));
        sectors.push_back(std::move(sector));
      }
    }
    u8* slot = &sectors[kFileTableFirstSector + record * 4 / kUserSectorSize][record * 4 % kUserSectorSize];
    put32(slot, word);
  }
  return std::make_shared<MemorySource>(std::move(sectors));
}

// Disc 1 and disc 2 indexes: a.bin on both (different bytes, to see which disc served it), b.bin on disc 1, c.bin on disc 2,
// dummy.dat twice (an empty slot, then a record with data), a name whose only record is empty, a movie, an unnamed record.
const std::vector<std::string> kIndex1 = {"c:/work/cdrom/a.bin", "c:/work/cdrom/b.bin", "c:/work/cdrom/gone.bin", "c:/work/cdrom/dummy.dat",
                                          "c:/work/cdrom/dummy.dat", "c:/movie/x.stv", ""};
const std::vector<std::string> kIndex2 = {"c:/work/cdrom/a.bin", "c:/work/cdrom/c.bin", "", "", "", "", ""};

const Bytes kA1 = content('a', 3000), kA2 = content('A', 400), kB = content('b', 16), kC = content('c', 4096), kDummy = content('d', 8),
            kMovie = content('m', 5000), kHidden = content('h', 24), kHidden2 = content('H', 32);

std::shared_ptr<const SectorSource> disc1() { return make_disc(1, {kA1, kB, std::nullopt, std::nullopt, kDummy, kMovie, kHidden}); }
std::shared_ptr<const SectorSource> disc2() { return make_disc(2, {kA2, kC, std::nullopt, std::nullopt, std::nullopt, std::nullopt, kHidden2}); }

bool same(const std::optional<Bytes>& got, const Bytes& want) { return got && *got == want; }

void test_backend() {
  const vfs::DiscNames names(kIndex1, kIndex2);
  DiscBackend backend(names);
  CHECK(!backend.mounted(1) && !backend.mounted(2));
  CHECK(!backend.read("cdrom", "a.bin", 1));
  CHECK(backend.mount(1, disc1()));
  CHECK(backend.mounted(1) && !backend.mounted(2));

  CHECK(backend.has_archive("cdrom"));
  for (const char* archive : {"cdrom2", "hd", "ui", "font", "lang", "movie"}) {
    CHECK(!backend.has_archive(archive));
    CHECK(!backend.read(archive, "a.bin", 1));
    CHECK(backend.list(archive).empty());
  }

  CHECK(same(backend.read("cdrom", "a.bin", 1), kA1));
  CHECK(same(backend.read("cdrom", "b.bin", 1), kB));
  CHECK(same(backend.read("cdrom", "movie/x.stv", 1), kMovie));
  // The empty slot is skipped for the record of the same name that holds data.
  CHECK(same(backend.read("cdrom", "dummy.dat", 1), kDummy));
  CHECK(!backend.contains("cdrom", "gone.bin", 1));
  CHECK(!backend.read("cdrom", "gone.bin", 1));
  CHECK(!backend.contains("cdrom", "c.bin", 1));
  CHECK(same(backend.read("cdrom", "#record/6", 1), kHidden));
  CHECK(same(backend.read("cdrom", "#record/00000", 1), kA1));
  // Disc 2 in the drive but not mounted: names come from disc 1, numbers do not.
  CHECK(same(backend.read("cdrom", "a.bin", 2), kA1));
  CHECK(same(backend.read("cdrom", "b.bin", 2), kB));
  CHECK(!backend.read("cdrom", "#record/6", 2));

  CHECK(backend.mount(2, disc2()));
  CHECK(same(backend.read("cdrom", "a.bin", 2), kA2));
  CHECK(same(backend.read("cdrom", "a.bin", 1), kA1));
  CHECK(same(backend.read("cdrom", "c.bin", 2), kC));
  CHECK(same(backend.read("cdrom", "c.bin", 1), kC));
  CHECK(backend.contains("cdrom", "c.bin", 1));
  CHECK(same(backend.read("cdrom", "#record/6", 2), kHidden2));
  CHECK(same(backend.read("cdrom", "#record/1", 2), kC));
  CHECK(!backend.read("cdrom", "#record/7", 2));

  const std::vector<std::string> listed = backend.list("cdrom");
  CHECK((listed == std::vector<std::string>{"a.bin", "b.bin", "dummy.dat", "movie/x.stv", "c.bin"}));

  std::string error;
  CHECK(!backend.mount(1, nullptr, &error) && !error.empty());
  CHECK(!backend.mount(1, std::make_shared<MemorySource>(std::vector<Bytes>(20, Bytes(kUserSectorSize, 0))), &error));
  CHECK(same(backend.read("cdrom", "b.bin", 1), kB));

  // Disc 1 taken out: its names come from disc 2, its numbers from nowhere.
  backend.unmount(1);
  CHECK(!backend.mounted(1) && backend.mounted(2));
  CHECK(same(backend.read("cdrom", "a.bin", 1), kA2));
  CHECK(!backend.read("cdrom", "b.bin", 1));
  CHECK(!backend.read("cdrom", "#record/6", 1));
  CHECK((backend.list("cdrom") == std::vector<std::string>{"a.bin", "c.bin"}));
}

void test_through_vfs() {
  static const vfs::DiscNames names(kIndex1, kIndex2);
  auto backend = std::make_unique<DiscBackend>(names);
  CHECK(backend->mount(1, disc1()));
  DiscBackend* raw = backend.get();
  vfs::Vfs files;
  files.open(std::move(backend), fs::temp_directory_path());
  CHECK(files.has_archive("cdrom") && !files.has_archive("hd"));
  CHECK(same(files.read("CDROM\\A.BIN"), kA1));
  CHECK(same(files.read_disc("cdrom/b.bin"), kB));
  CHECK(files.exists("cdrom/movie/x.stv"));
  CHECK(!files.exists("hd/a.bin"));
  files.set_disc(2);
  CHECK(same(files.read("cdrom/a.bin"), kA1));
  CHECK(raw->mount(2, disc2()));
  CHECK(same(files.read("cdrom/a.bin"), kA2));
  files.set_hd_mode(true);
  CHECK(same(files.read("cdrom/c.bin"), kC));
}

void test_concurrent_mount() {
  const vfs::DiscNames names(kIndex1, kIndex2);
  DiscBackend backend(names);
  CHECK(backend.mount(1, disc1()));
  std::atomic<int> bad{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&] {
      for (int round = 0; round < 300; ++round) {
        const auto got = backend.read("cdrom", "a.bin", 2);
        if (!got || (*got != kA1 && *got != kA2)) ++bad;
      }
    });
  for (int round = 0; round < 50; ++round) CHECK(backend.mount(2, disc2()));
  for (std::thread& reader : readers) reader.join();
  CHECK(bad.load() == 0);
}

FoundDisc found(Region region, bool known, u32 number, const std::string& sha = "e1") {
  FoundDisc disc;
  disc.where = "disc";
  disc.identity.chrono_cross = true;
  disc.identity.known_build = known;
  disc.identity.region = region;
  disc.identity.build = known ? (region == Region::Usa ? "usa" : "jp") : "";
  disc.identity.exe_sha256 = sha;
  disc.identity.disc_number = number;
  return disc;
}

void test_choice() {
  CHECK(choose_set({}, Region::Unknown).group < 0);
  CHECK(!choose_set({}, Region::Unknown).ask_region);

  // Both regions complete: ask, unless a region is preferred.
  const std::vector<FoundDisc> both = {found(Region::Japan, true, 1), found(Region::Usa, true, 2), found(Region::Usa, true, 1),
                                       found(Region::Japan, true, 2), found(Region::Japan, true, 1)};
  const std::vector<DiscGroup> groups = group_discs(both);
  CHECK(groups.size() == 2);
  CHECK(groups[0].key == "jp" && groups[0].disc[0] == 0 && groups[0].disc[1] == 3);
  CHECK(groups[1].key == "usa" && groups[1].disc[0] == 2 && groups[1].disc[1] == 1);
  CHECK(choose_set(groups, Region::Unknown).ask_region);
  CHECK(choose_set(groups, Region::Unknown).group < 0);
  CHECK(choose_set(groups, Region::Usa).group == 1 && !choose_set(groups, Region::Usa).ask_region);
  CHECK(choose_set(groups, Region::Japan).group == 0);

  // A complete set beats a single disc of the other region; no question.
  const std::vector<DiscGroup> partial = group_discs({found(Region::Japan, true, 1), found(Region::Usa, true, 1), found(Region::Usa, true, 2)});
  CHECK(choose_set(partial, Region::Unknown).group == 1 && !choose_set(partial, Region::Unknown).ask_region);
  CHECK(choose_set(partial, Region::Japan).group == 1);

  // Only single discs of both regions: the question too.
  const std::vector<DiscGroup> singles = group_discs({found(Region::Japan, true, 1), found(Region::Usa, true, 2)});
  CHECK(choose_set(singles, Region::Unknown).ask_region);
  CHECK(choose_set(singles, Region::Usa).group == 1);

  // A known build beats an unknown one of the same region; unknown builds group by executable.
  const std::vector<DiscGroup> unknown = group_discs({found(Region::Japan, false, 1, "x"), found(Region::Japan, false, 2, "x"),
                                                      found(Region::Japan, true, 1), found(Region::Japan, true, 2), found(Region::Japan, false, 1, "y")});
  CHECK(unknown.size() == 3);
  CHECK(unknown[0].key == "unknown x" && unknown[0].complete() && !unknown[0].known_build);
  CHECK(!unknown[2].complete());
  CHECK(choose_set(unknown, Region::Unknown).group == 1);

  // An unknown build alone is played; one whose region is unknown does not make the question.
  const std::vector<DiscGroup> lone = group_discs({found(Region::Unknown, false, 2, "z")});
  CHECK(choose_set(lone, Region::Unknown).group == 0);
  CHECK(choose_set(lone, Region::Usa).group == 0);
  const std::vector<DiscGroup> odd = group_discs({found(Region::Unknown, false, 1, "z"), found(Region::Usa, true, 1)});
  CHECK(choose_set(odd, Region::Unknown).group == 1 && !choose_set(odd, Region::Unknown).ask_region);
}

void test_folder_scan() {
  const fs::path dir = fs::temp_directory_path() / "oc_test_disc_backend";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "sub.cue");
  for (const char* name : {"b.BIN", "a.cue", "notes.txt", "game.ISO", "c.m3u", "x.chd", "z.Pbp", "readme"})
    std::ofstream(dir / name, std::ios::binary) << "not a disc";
  const std::vector<fs::path> files = image_files_in(dir);
  std::vector<std::string> names;
  for (const fs::path& file : files) names.push_back(file.filename().string());
  CHECK((names == std::vector<std::string>{"a.cue", "c.m3u", "z.Pbp", "b.BIN", "game.ISO", "x.chd"}));
  CHECK(image_files_in(dir / "missing").empty());
  CHECK(has_image_extension("disc.MdS") && has_image_extension("disc.img") && !has_image_extension("disc") && !has_image_extension("disc.zip"));

  std::vector<FoundDisc> discs;
  std::vector<std::string> notes;
  add_image_file(dir / "game.ISO", discs, notes);
  CHECK(discs.empty() && notes.size() == 1);
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_backend();
  test_through_vfs();
  test_concurrent_mount();
  test_choice();
  test_folder_scan();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("disc backend: all passed\n");
  return 0;
}
