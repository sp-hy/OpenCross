// CHD disc reader on synthetic version-5 CHD files (uncompressed hunks) written to a temp folder: raw, cooked and mode-2 (2336) data
// tracks, a second audio track with a virtual pregap, hunk-cache eviction under random reads, detection by content, and the refusal
// of a CHD that is not a CD image.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "import/disc/disc_image.h"

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

constexpr size_t kFrame = 2448;
constexpr u32 kFramesPerHunk = 8;
constexpr size_t kHeaderSize = 124;

struct TrackSpec {
  std::string type;
  u32 frames = 0;
  u32 pregap = 0;
  u32 seed = 0;
};

void put32be(Bytes& out, size_t at, u32 value) {
  for (int i = 0; i < 4; ++i) out[at + i] = static_cast<u8>(value >> (24 - 8 * i));
}

void put64be(Bytes& out, size_t at, u64 value) {
  put32be(out, at, static_cast<u32>(value >> 32));
  put32be(out, at + 4, static_cast<u32>(value));
}

size_t payload_size(const std::string& type) {
  if (type == "MODE1" || type == "MODE2_FORM1") return 2048;
  if (type == "MODE2") return 2336;
  return 2352;
}

// The stored bytes of frame `index` of a track, deterministic from the seed; raw types carry sync and a mode-2 header.
Bytes make_frame(const TrackSpec& track, u32 index) {
  Bytes frame(kFrame, 0);
  const size_t size = payload_size(track.type);
  for (size_t i = 0; i < size; ++i) frame[i] = static_cast<u8>(track.seed * 31 + index * 7 + i * 13 + 5);
  if (track.type == "MODE2_RAW") {
    static constexpr u8 kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(frame.data(), kSync, sizeof(kSync));
    frame[15] = 2;
    frame[18] = frame[22] = 0x08;
  } else if (track.type == "MODE2") {
    frame[2] = frame[6] = 0x08;
  }
  for (size_t i = 0; i < 96; ++i) frame[2352 + i] = static_cast<u8>(i + 1);
  return frame;
}

void write_chd(const fs::path& path, const std::vector<TrackSpec>& tracks, bool odd_hunks = false) {
  Bytes data;
  for (const TrackSpec& track : tracks) {
    for (u32 i = 0; i < track.frames; ++i) {
      const Bytes frame = make_frame(track, i);
      data.insert(data.end(), frame.begin(), frame.end());
    }
    data.resize((data.size() / kFrame + 3) / 4 * 4 * kFrame, 0);
  }
  data.resize((data.size() / kFrame + kFramesPerHunk - 1) / kFramesPerHunk * kFramesPerHunk * kFrame, 0);
  const u32 hunk_bytes = odd_hunks ? 1000 : static_cast<u32>(kFrame * kFramesPerHunk);
  const u32 hunks = static_cast<u32>(data.size() / hunk_bytes + (data.size() % hunk_bytes ? 1 : 0));
  const size_t map_end = kHeaderSize + 4 * static_cast<size_t>(hunks);
  const size_t data_start = (map_end + hunk_bytes - 1) / hunk_bytes * hunk_bytes;
  Bytes file(data_start + data.size(), 0);
  std::memcpy(file.data(), "MComprHD", 8);
  put32be(file, 8, kHeaderSize);
  put32be(file, 12, 5);
  put64be(file, 32, data.size());
  put64be(file, 40, kHeaderSize);
  put32be(file, 56, hunk_bytes);
  put32be(file, 60, odd_hunks ? 1 : static_cast<u32>(kFrame));
  for (u32 i = 0; i < hunks; ++i) put32be(file, kHeaderSize + 4 * static_cast<size_t>(i), static_cast<u32>(data_start / hunk_bytes + i));
  std::memcpy(file.data() + data_start, data.data(), data.size());
  const size_t meta_at = file.size();
  put64be(file, 48, meta_at);
  for (size_t i = 0; i < tracks.size(); ++i) {
    const TrackSpec& track = tracks[i];
    const std::string text = "TRACK:" + std::to_string(i + 1) + " TYPE:" + track.type + " SUBTYPE:NONE FRAMES:" + std::to_string(track.frames) +
                             " PREGAP:" + std::to_string(track.pregap) + " PGTYPE:MODE1 PGSUB:NONE POSTGAP:0";
    Bytes block(16 + text.size() + 1, 0);
    std::memcpy(block.data(), "CHT2", 4);
    put32be(block, 4, 1u << 24 | static_cast<u32>(text.size() + 1));
    const u64 next = i + 1 < tracks.size() ? file.size() + block.size() : 0;
    put64be(block, 8, next);
    std::memcpy(block.data() + 16, text.c_str(), text.size() + 1);
    file.insert(file.end(), block.begin(), block.end());
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void test_raw_track(const fs::path& dir) {
  const fs::path path = dir / "raw.chd";
  const TrackSpec track{"MODE2_RAW", 100, 0, 1};
  write_chd(path, {track});
  CHECK(detect_format(path) == ImageFormat::Chd);
  std::string error;
  auto source = open_image(path, &error);
  CHECK(source != nullptr);
  if (!source) return;
  CHECK(source->raw());
  CHECK(source->sector_count() == 100);
  std::mt19937 rng(7);
  for (int n = 0; n < 400; ++n) {
    const u32 lba = rng() % 100;
    Bytes raw(kRawSectorSize);
    CHECK(source->read_raw(lba, 1, raw.data()));
    const Bytes frame = make_frame(track, lba);
    CHECK(std::equal(raw.begin(), raw.end(), frame.begin()));
    const Bytes user = source->read_user(lba, 1);
    CHECK(user.size() == kUserSectorSize && std::equal(user.begin(), user.end(), frame.begin() + 24));
  }
  Bytes run(kRawSectorSize * 30);
  CHECK(source->read_raw(5, 30, run.data()));
  for (u32 i = 0; i < 30; ++i) {
    const Bytes frame = make_frame(track, 5 + i);
    CHECK(std::equal(frame.begin(), frame.begin() + kRawSectorSize, run.begin() + i * kRawSectorSize));
  }
  const auto info = source->info(3);
  CHECK(info && info->mode == 2 && info->submode == 0x08);
  CHECK(!source->read_raw(95, 6, run.data()));
}

void test_two_tracks(const fs::path& dir) {
  const fs::path path = dir / "two.chd";
  const TrackSpec data{"MODE2_RAW", 10, 0, 2};
  const TrackSpec audio{"AUDIO", 6, 2, 3};
  write_chd(path, {data, audio});
  auto source = open_image(path, nullptr);
  CHECK(source != nullptr);
  if (!source) return;
  CHECK(source->sector_count() == 18);
  Bytes sector(kRawSectorSize);
  for (u32 lba = 0; lba < 10; ++lba) {
    CHECK(source->read_raw(lba, 1, sector.data()));
    const Bytes frame = make_frame(data, lba);
    CHECK(std::equal(sector.begin(), sector.end(), frame.begin()));
  }
  for (u32 lba = 10; lba < 12; ++lba) {
    CHECK(source->read_raw(lba, 1, sector.data()));
    CHECK(std::all_of(sector.begin(), sector.end(), [](u8 byte) { return byte == 0; }));
  }
  for (u32 i = 0; i < 6; ++i) {
    CHECK(source->read_raw(12 + i, 1, sector.data()));
    const Bytes frame = make_frame(audio, i);
    CHECK(std::equal(sector.begin(), sector.end(), frame.begin()));
  }
  const auto info = source->info(12);
  CHECK(info && info->mode == 0);
}

void test_cooked_track(const fs::path& dir) {
  const fs::path path = dir / "cooked.chd";
  const TrackSpec track{"MODE1", 20, 0, 4};
  write_chd(path, {track});
  auto source = open_image(path, nullptr);
  CHECK(source != nullptr);
  if (!source) return;
  CHECK(!source->raw() && source->sector_count() == 20);
  Bytes raw(kRawSectorSize);
  CHECK(!source->read_raw(0, 1, raw.data()));
  for (u32 lba = 0; lba < 20; ++lba) {
    const Bytes user = source->read_user(lba, 1);
    const Bytes frame = make_frame(track, lba);
    CHECK(user.size() == kUserSectorSize && std::equal(user.begin(), user.end(), frame.begin()));
  }
}

void test_mode2_track(const fs::path& dir) {
  const fs::path path = dir / "mode2.chd";
  const TrackSpec track{"MODE2", 12, 0, 5};
  write_chd(path, {track});
  auto source = open_image(path, nullptr);
  CHECK(source != nullptr);
  if (!source) return;
  CHECK(source->raw());
  Bytes sector(kRawSectorSize);
  CHECK(source->read_raw(80, 1, sector.data()) == false);
  CHECK(source->read_raw(7, 1, sector.data()));
  const Bytes frame = make_frame(track, 7);
  CHECK(has_sync(sector.data()));
  CHECK(sector[12] == 0x00 && sector[13] == 0x02 && sector[14] == 0x07 &&sector[15] == 2);
  CHECK(std::equal(frame.begin(), frame.begin() + 2336, sector.begin() + 16));
  const Bytes user = source->read_user(7, 1);
  CHECK(std::equal(user.begin(), user.end(), frame.begin() + 8));
}

void test_not_a_cd(const fs::path& dir) {
  const fs::path path = dir / "hd.chd";
  write_chd(path, {TrackSpec{"MODE2_RAW", 8, 0, 6}}, true);
  std::string error;
  CHECK(open_image(path, &error) == nullptr);
  CHECK(error.find("CHD") != std::string::npos);
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / ("oc_test_chd_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  test_raw_track(dir);
  test_two_tracks(dir);
  test_cooked_track(dir);
  test_mode2_track(dir);
  test_not_a_cd(dir);
  fs::remove_all(dir, ec);
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("chd: ok\n");
  return failures ? 1 : 0;
}
