#include "import/disc/drive_source.h"

#include <algorithm>
#include <cstring>

#include "platform/optical.h"

namespace oc::import::disc {

namespace {

constexpr u32 kLeadInFrames = 150;
constexpr u32 kProbeLba = 16;

u32 from_bcd(u8 value) { return (value >> 4) * 10u + (value & 0x0F); }

bool address_matches(const u8* sector, u32 lba) {
  return (from_bcd(sector[12]) * 60 + from_bcd(sector[13])) * 75 + from_bcd(sector[14]) == lba + kLeadInFrames;
}

class OpticalDevice final : public SectorDevice {
 public:
  explicit OpticalDevice(std::unique_ptr<platform::OpticalDrive> drive) : drive_(std::move(drive)) {}

  u32 sector_count() const override { return drive_->sector_count(); }
  bool read_raw(u32 lba, u32 count, u8* out) override { return drive_->read_raw(lba, count, out); }
  bool read_cooked(u32 lba, u32 count, u8* out) override { return drive_->read_cooked(lba, count, out); }
  std::optional<u32> media_generation() override { return drive_->media_generation(); }
  std::string description() const override {
    std::string out = drive_->drive();
    if (!drive_->product().empty()) out += " " + drive_->product();
    if (drive_->raw_method() == platform::RawReadMethod::Cooked) return out;
    return out + ", " + platform::raw_read_method_name(drive_->raw_method());
  }

 private:
  std::unique_ptr<platform::OpticalDrive> drive_;
};

}  // namespace

std::unique_ptr<DriveSource> DriveSource::open(std::unique_ptr<SectorDevice> device, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return std::unique_ptr<DriveSource>();
  };
  const std::optional<u32> generation = device->media_generation();
  if (!generation) return fail("no disc in " + device->description());
  const u32 count = device->sector_count();
  if (count == 0) return fail("empty disc in " + device->description());
  const u32 probe = kProbeLba < count ? kProbeLba : 0;
  u8 sector[kRawSectorSize];
  const bool raw = device->read_raw(probe, 1, sector) && has_sync(sector) && address_matches(sector, probe);
  if (!raw && !device->read_cooked(probe, 1, sector)) return fail("cannot read sectors from " + device->description());
  return std::unique_ptr<DriveSource>(new DriveSource(std::move(device), raw, *generation));
}

DriveSource::DriveSource(std::unique_ptr<SectorDevice> device, bool raw, u32 generation)
    : SectorSource(device->sector_count(), raw, device->description() + (raw ? ", raw 2352" : ", cooked 2048 (no form-2 data)")),
      device_(std::move(device)),
      generation_(generation) {}

bool DriveSource::media_changed() const {
  const std::lock_guard lock(cache_mutex_);
  return !same_medium();
}

DriveSource::Block* DriveSource::find(u32 index) const {
  for (Block& block : cache_)
    if (block.index == index) return &block;
  return nullptr;
}

DriveSource::Block& DriveSource::insert(u32 index, u32 sectors) const {
  Block* slot = find(index);
  if (!slot && cache_.size() < kCacheBlocks) slot = &cache_.emplace_back();
  if (!slot) slot = &*std::min_element(cache_.begin(), cache_.end(), [](const Block& a, const Block& b) { return a.last_use < b.last_use; });
  slot->index = index;
  slot->sectors = sectors;
  slot->last_use = ++use_clock_;
  slot->data.resize(static_cast<size_t>(sectors) * sector_size());
  slot->unread.clear();
  return *slot;
}

void DriveSource::load(u32 index, u32 last) const {
  const size_t size = sector_size();
  const u32 first = index * kBlockSectors;
  const u32 stop = std::min((last + 1) * kBlockSectors, sector_count());
  Bytes data(static_cast<size_t>(stop - first) * size);
  const bool whole = read_device(first, stop - first, data.data());
  for (u32 block = index; block <= last; ++block) {
    const u32 block_first = block * kBlockSectors;
    const u32 sectors = std::min(stop, block_first + kBlockSectors) - block_first;
    Block& slot = insert(block, sectors);
    std::memcpy(slot.data.data(), data.data() + static_cast<size_t>(block_first - first) * size, slot.data.size());
    if (!whole) slot.unread.assign(sectors, true);
  }
}

bool DriveSource::same_medium() const {
  if (changed_) return false;
  if (device_->media_generation() == generation_) return true;
  changed_ = true;
  cache_.clear();
  return false;
}

bool DriveSource::read_device(u32 lba, u32 count, u8* out) const {
  for (int attempt = 0; attempt < 2; ++attempt)
    if (raw() ? device_->read_raw(lba, count, out) : device_->read_cooked(lba, count, out)) return true;
  return false;
}

bool DriveSource::fetch(u32 lba, u32 count, u8* out) const {
  const std::lock_guard lock(cache_mutex_);
  if (changed_) return false;
  const size_t size = sector_size();
  const u32 end = lba + count;
  bool checked = false;
  u32 at = lba;
  while (at < end) {
    const u32 index = at / kBlockSectors;
    const u32 first = index * kBlockSectors;
    Block* block = find(index);
    if (!block) {
      u32 last = index;
      while (last - index + 1 < kCacheBlocks && (last + 1) * kBlockSectors < end && !find(last + 1)) ++last;
      if (!checked && !same_medium()) return false;
      checked = true;
      load(index, last);
      block = find(index);
    }
    block->last_use = ++use_clock_;
    const u32 stop = std::min(end, first + block->sectors);
    if (stop <= at) return false;
    for (u32 sector = at; sector < stop && !block->unread.empty(); ++sector) {
      if (!block->unread[sector - first]) continue;
      if (!checked && !same_medium()) return false;
      checked = true;
      if (!read_device(sector, 1, block->data.data() + static_cast<size_t>(sector - first) * size)) return false;
      block->unread[sector - first] = false;
    }
    std::memcpy(out + static_cast<size_t>(at - lba) * size, block->data.data() + static_cast<size_t>(at - first) * size, (stop - at) * size);
    at = stop;
  }
  return true;
}

std::unique_ptr<DriveSource> open_optical_drive(const std::string& drive, std::string* error) {
  std::unique_ptr<platform::OpticalDrive> handle = platform::OpticalDrive::open(drive, error);
  if (!handle) return nullptr;
  return DriveSource::open(std::make_unique<OpticalDevice>(std::move(handle)), error);
}

std::vector<DriveCandidate> probe_optical_drives() {
  std::vector<DriveCandidate> out;
  for (const platform::OpticalDriveInfo& info : platform::list_optical_drives()) {
    DriveCandidate candidate;
    candidate.drive = info.drive;
    candidate.product = info.product;
    candidate.label = info.label;
    candidate.media = info.media;
    if (info.media) {
      candidate.source = open_optical_drive(info.drive, &candidate.error);
      if (candidate.source) candidate.identity = identify(*candidate.source);
    } else {
      candidate.error = "no disc";
    }
    out.push_back(std::move(candidate));
  }
  return out;
}

std::vector<DriveCandidate> scan_optical_drives() {
  std::vector<DriveCandidate> out;
  for (DriveCandidate& candidate : probe_optical_drives())
    if (candidate.source && candidate.identity.chrono_cross) out.push_back(std::move(candidate));
  return out;
}

}  // namespace oc::import::disc
