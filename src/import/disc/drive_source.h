#pragma once
// A disc in an optical drive as a SectorSource. Raw when the drive returns whole sectors with a valid sync and address, cooked
// (2048-byte) otherwise. Reads go through a small cache of aligned sector blocks, so a run of small reads costs one seek; a medium
// change empties the cache and makes every later read fail until the drive is opened again.
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "import/disc/disc_ident.h"
#include "import/disc/sector_source.h"
#include "platform/optical.h"

namespace oc::import::disc {

// The reads a drive offers. The optical drive implements it; tests use a fake.
class SectorDevice {
 public:
  virtual ~SectorDevice() = default;
  virtual u32 sector_count() const = 0;
  // `count` whole 2352-byte sectors; false when the device cannot read raw sectors.
  virtual bool read_raw(u32 lba, u32 count, u8* out) = 0;
  // `count` sectors of 2048 user bytes.
  virtual bool read_cooked(u32 lba, u32 count, u8* out) = 0;
  // Changes whenever the medium is changed; nullopt when there is none.
  virtual std::optional<u32> media_generation() = 0;
  // The drive and how it reads, e.g. "D: VENDOR PRODUCT, raw via READ CD".
  virtual std::string description() const = 0;
};

class DriveSource final : public SectorSource {
 public:
  // Sectors per cache block (the read-ahead unit) and blocks kept.
  static constexpr u32 kBlockSectors = 32;
  static constexpr size_t kCacheBlocks = 16;

  // Null with `error` filled when the device holds no medium or neither a raw nor a cooked read of the probe sector works.
  static std::unique_ptr<DriveSource> open(std::unique_ptr<SectorDevice> device, std::string* error);

  // True once the medium has been changed or removed since the source opened (one cheap device query). From then on the cache is
  // empty and reads fail; open the drive again to read the new disc.
  bool media_changed() const;

 protected:
  bool fetch(u32 lba, u32 count, u8* out) const override;

 private:
  struct Block {
    u32 index = 0;
    u32 sectors = 0;
    u64 last_use = 0;
    Bytes data;
    // Empty when the whole block was read; otherwise one flag per sector still to be read on its own (a run the device refused
    // as a whole, e.g. one crossing a sector it cannot return in this mode).
    std::vector<bool> unread;
  };

  DriveSource(std::unique_ptr<SectorDevice> device, bool raw, u32 generation);
  size_t sector_size() const { return raw() ? kRawSectorSize : kUserSectorSize; }
  Block* find(u32 index) const;
  Block& insert(u32 index, u32 sectors) const;
  // Reads blocks `index` to `last` into the cache in one device request.
  void load(u32 index, u32 last) const;
  bool same_medium() const;
  bool read_device(u32 lba, u32 count, u8* out) const;

  std::unique_ptr<SectorDevice> device_;
  u32 generation_;
  mutable std::mutex cache_mutex_;
  mutable std::vector<Block> cache_;
  mutable u64 use_clock_ = 0;
  mutable bool changed_ = false;
};

// An optical drive ("D:") as a source. Null with `error` filled when it is not a drive, holds no disc or cannot be read.
std::unique_ptr<DriveSource> open_optical_drive(const std::string& drive, std::string* error);

struct DriveCandidate {
  // "D:".
  std::string drive;
  std::string product;
  std::string label;
  bool media = false;
  // Null when the drive holds no disc or cannot be read (`error` says why).
  std::unique_ptr<DriveSource> source;
  DiscIdentity identity;
  std::string error;
};

// Every optical drive, with its disc opened and identified.
std::vector<DriveCandidate> probe_optical_drives();
// The drives holding a Chrono Cross disc, in drive-letter order.
std::vector<DriveCandidate> scan_optical_drives();

// One line per drive: its name, media-change count and volume label. It changes whenever a disc is put in, taken out, swapped or
// becomes readable, whatever the disc is; nothing is opened or read (list_optical_drives).
std::string drive_media_signature(const std::vector<platform::OpticalDriveInfo>& drives);

}  // namespace oc::import::disc
