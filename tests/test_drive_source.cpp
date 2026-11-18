// Optical drive source on a fake device: raw / cooked selection (a raw read that fails or returns sectors without a valid sync and
// address falls back to cooked), no medium, the block cache (read-ahead, hits, eviction, the short last block, reads larger than
// the cache), a retried transient failure, a sector the device refuses (its neighbours still read), media-change invalidation and
// concurrent readers.
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "import/disc/drive_source.h"

using namespace oc;
using namespace oc::import::disc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

constexpr u32 kSectors = 1000;

u8 to_bcd(u32 value) { return static_cast<u8>((value / 10) << 4 | value % 10); }

u8 pattern(u32 lba, size_t at) { return static_cast<u8>(lba * 7 + at * 13 + 1); }

void make_raw(u32 lba, u8* sector) {
  std::memset(sector, 0, kRawSectorSize);
  std::memset(sector + 1, 0xFF, 10);
  const u32 frames = lba + 150;
  sector[12] = to_bcd(frames / 75 / 60);
  sector[13] = to_bcd(frames / 75 % 60);
  sector[14] = to_bcd(frames % 75);
  sector[15] = 2;
  sector[18] = sector[22] = kSubmodeData;
  for (size_t i = 0; i < kUserSectorSize; ++i) sector[kMode2PayloadOffset + i] = pattern(lba, i);
}

struct DeviceState {
  bool raw_works = true;
  bool raw_synced = true;
  bool cooked_works = true;
  bool media = true;
  u32 generation = 5;
  int fail_next = 0;
  // A sector every read that covers it fails on.
  u32 bad_lba = ~0u;
  int calls = 0;
};

class FakeDevice final : public SectorDevice {
 public:
  FakeDevice(DeviceState& state, u32 sectors) : state_(state), sectors_(sectors) {}
  u32 sector_count() const override { return sectors_; }
  bool read_raw(u32 lba, u32 count, u8* out) override {
    ++state_.calls;
    if (!state_.raw_works || lba + count > sectors_ || covers_bad(lba, count) || consume_failure()) return false;
    for (u32 i = 0; i < count; ++i) {
      make_raw(lba + i, out + static_cast<size_t>(i) * kRawSectorSize);
      if (!state_.raw_synced) out[i * kRawSectorSize + 1] = 0;
    }
    return true;
  }
  bool read_cooked(u32 lba, u32 count, u8* out) override {
    ++state_.calls;
    if (!state_.cooked_works || lba + count > sectors_ || covers_bad(lba, count) || consume_failure()) return false;
    for (u32 i = 0; i < count; ++i)
      for (size_t j = 0; j < kUserSectorSize; ++j) out[i * kUserSectorSize + j] = pattern(lba + i, j);
    return true;
  }
  std::optional<u32> media_generation() override {
    if (!state_.media) return std::nullopt;
    return state_.generation;
  }
  std::string description() const override { return "X: FAKE"; }

 private:
  bool covers_bad(u32 lba, u32 count) const { return state_.bad_lba >= lba && state_.bad_lba - lba < count; }
  bool consume_failure() {
    if (state_.fail_next == 0) return false;
    --state_.fail_next;
    return true;
  }
  DeviceState& state_;
  u32 sectors_;
};

std::unique_ptr<DriveSource> open_fake(DeviceState& state, u32 sectors = kSectors, std::string* error = nullptr) {
  std::string scratch;
  return DriveSource::open(std::make_unique<FakeDevice>(state, sectors), error ? error : &scratch);
}

bool user_ok(const DriveSource& source, u32 lba, u32 count) {
  const Bytes bytes = source.read_user(lba, count);
  if (bytes.size() != static_cast<size_t>(count) * kUserSectorSize) return false;
  for (u32 i = 0; i < count; ++i)
    for (size_t j = 0; j < kUserSectorSize; j += 97)
      if (bytes[i * kUserSectorSize + j] != pattern(lba + i, j)) return false;
  return true;
}

void test_selection() {
  DeviceState raw;
  const auto source = open_fake(raw);
  CHECK(source && source->raw());
  CHECK(source && source->description().find("raw 2352") != std::string::npos);
  CHECK(source && user_ok(*source, 100, 3));
  u8 sector[kRawSectorSize];
  CHECK(source && source->read_raw(37, 1, sector) && has_sync(sector) && sector[15] == 2);
  CHECK(source && source->info(37) && source->info(37)->mode == 2 && source->info(37)->form == 1);

  DeviceState failing;
  failing.raw_works = false;
  const auto cooked = open_fake(failing);
  CHECK(cooked && !cooked->raw());
  CHECK(cooked && cooked->description().find("cooked 2048") != std::string::npos);
  CHECK(cooked && user_ok(*cooked, 999, 1));
  CHECK(cooked && !cooked->read_raw(0, 1, sector));

  DeviceState unsynced;
  unsynced.raw_synced = false;
  const auto garbled = open_fake(unsynced);
  CHECK(garbled && !garbled->raw());
  CHECK(garbled && user_ok(*garbled, 5, 2));

  DeviceState unreadable;
  unreadable.raw_works = false;
  unreadable.cooked_works = false;
  std::string error;
  CHECK(!open_fake(unreadable, kSectors, &error));
  CHECK(error.find("cannot read") != std::string::npos);

  DeviceState empty;
  empty.media = false;
  error.clear();
  CHECK(!open_fake(empty, kSectors, &error));
  CHECK(error.find("no disc") != std::string::npos);
}

void test_cache() {
  DeviceState state;
  const auto source = open_fake(state);
  if (!source) {
    CHECK(source);
    return;
  }
  state.calls = 0;
  for (u32 lba = 0; lba < DriveSource::kBlockSectors; ++lba) CHECK(user_ok(*source, lba, 1));
  CHECK(state.calls == 1);
  CHECK(user_ok(*source, 30, 4));
  CHECK(state.calls == 2);
  CHECK(user_ok(*source, 0, 64));
  CHECK(state.calls == 2);

  for (u32 block = 2; block < 2 + DriveSource::kCacheBlocks; ++block) CHECK(user_ok(*source, block * DriveSource::kBlockSectors, 1));
  const int before = state.calls;
  CHECK(user_ok(*source, 5, 1));
  CHECK(state.calls == before + 1);

  state.calls = 0;
  CHECK(user_ok(*source, 300, 600));
  CHECK(state.calls == 1);
  CHECK(user_ok(*source, 990, 10));
  CHECK(!source->read_user(995, 6, std::vector<u8>(6 * kUserSectorSize).data()));

  DeviceState fresh;
  const auto whole = open_fake(fresh);
  fresh.calls = 0;
  CHECK(whole && user_ok(*whole, 0, kSectors));
  CHECK(fresh.calls == 2);

  DeviceState short_disc;
  const auto small = open_fake(short_disc, 70);
  CHECK(small && user_ok(*small, 69, 1));
  CHECK(small && user_ok(*small, 60, 10));
  CHECK(small && user_ok(*small, 0, 70));
}

void test_retry_and_change() {
  DeviceState state;
  const auto source = open_fake(state);
  if (!source) {
    CHECK(source);
    return;
  }
  state.fail_next = 1;
  CHECK(user_ok(*source, 200, 2));
  state.fail_next = 3;
  CHECK(user_ok(*source, 300, 1));
  state.fail_next = 4;
  CHECK(source->read_user(400, 1).empty());
  CHECK(user_ok(*source, 400, 1));

  CHECK(user_ok(*source, 0, 1));
  CHECK(!source->media_changed());
  state.generation = 6;
  CHECK(source->media_changed());
  CHECK(source->read_user(0, 1).empty());
  state.generation = 5;
  CHECK(source->media_changed());
  CHECK(source->read_user(0, 1).empty());

  DeviceState ejected;
  const auto gone = open_fake(ejected);
  ejected.media = false;
  CHECK(gone && gone->read_user(10, 1).empty());
  CHECK(gone && gone->media_changed());
}

void test_bad_sector() {
  DeviceState state;
  state.bad_lba = 13;
  const auto source = open_fake(state);
  if (!source) {
    CHECK(source);
    return;
  }
  CHECK(user_ok(*source, 16, 1));
  CHECK(user_ok(*source, 0, 13));
  CHECK(user_ok(*source, 14, 18));
  CHECK(source->read_user(12, 2).empty());
  state.calls = 0;
  CHECK(user_ok(*source, 16, 4));
  CHECK(state.calls == 0);
  state.bad_lba = ~0u;
  CHECK(user_ok(*source, 13, 1));
  CHECK(user_ok(*source, 32, 1));
}

void test_threads() {
  DeviceState state;
  const auto source = open_fake(state);
  if (!source) {
    CHECK(source);
    return;
  }
  std::vector<std::thread> threads;
  std::vector<int> bad(4, 0);
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&, worker] {
      u32 lba = static_cast<u32>(worker) * 211;
      for (int i = 0; i < 200; ++i) {
        lba = (lba * 37 + 11) % (kSectors - 8);
        if (!user_ok(*source, lba, 1 + static_cast<u32>(i % 8))) ++bad[static_cast<size_t>(worker)];
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (const int count : bad) CHECK(count == 0);
}

}  // namespace

int main() {
  test_selection();
  test_cache();
  test_retry_and_change();
  test_bad_sector();
  test_threads();
  if (failures == 0) std::printf("drive source: all passed\n");
  return failures == 0 ? 0 : 1;
}
