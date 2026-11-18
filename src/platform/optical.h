#pragma once
// Optical drives: which ones the system has, whether a disc is in them, and sector reads from it. Raw reads give whole 2352-byte
// sectors (sync, header, subheader, payload, EDC/ECC); cooked reads give the 2048 user bytes the drive returns in data mode. On
// systems without a drive API this finds no drives.
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::platform {

struct OpticalDriveInfo {
  // The drive as the system names it, e.g. "D:".
  std::string drive;
  // Vendor and product of the device ("" when the system does not say).
  std::string product;
  bool media = false;
  // The drive's media-change count (OpticalDrive::media_generation); nullopt without a disc.
  std::optional<u32> generation;
  // Volume label of the disc in it ("" without a disc or a label).
  std::string label;
};

std::vector<OpticalDriveInfo> list_optical_drives();

struct OpticalTrack {
  u32 number = 0;
  u32 start_lba = 0;
  bool data = false;
};

// How raw sectors are read; Cooked when no raw method works on this drive.
enum class RawReadMethod {
  Cooked,
  ReadCd,
  RawXaForm2,
  RawYellowMode2,
};

const char* raw_read_method_name(RawReadMethod method);

class OpticalDrive {
 public:
  // The drive `drive` names ("D:", "D" or "D:\"), with a disc in it. Null with `error` filled otherwise.
  static std::unique_ptr<OpticalDrive> open(const std::string& drive, std::string* error);
  ~OpticalDrive();
  OpticalDrive(const OpticalDrive&) = delete;
  OpticalDrive& operator=(const OpticalDrive&) = delete;

  const std::string& drive() const { return drive_; }
  // Vendor and product of the device ("" when the system does not say).
  const std::string& product() const { return product_; }
  // Sectors from LBA 0 to the end of the disc: the larger of the table of contents' lead-out and the device length, less any
  // trailing sectors that do not read.
  u32 sector_count() const { return sector_count_; }
  const std::vector<OpticalTrack>& tracks() const { return tracks_; }
  // The raw method that worked on the probe at open (Cooked when none did).
  RawReadMethod raw_method() const;

  // `count` whole 2352-byte sectors. False when the drive cannot read raw sectors or the read fails.
  bool read_raw(u32 lba, u32 count, u8* out);
  // `count` sectors of 2048 user bytes each.
  bool read_cooked(u32 lba, u32 count, u8* out);
  // A number that changes whenever the medium is changed (ejected, inserted, swapped); nullopt when the drive holds no disc.
  std::optional<u32> media_generation();

  struct Handle;

 private:
  OpticalDrive(std::string drive, std::unique_ptr<Handle> handle);
  bool read_toc();
  // One device request of at most a chunk, into an aligned buffer; no lock, no range check.
  bool raw_with(RawReadMethod method, u32 lba, u32 count, u8* out);
  bool cooked_with(u32 lba, u32 count, u8* out);

  std::string drive_;
  std::string product_;
  std::unique_ptr<Handle> handle_;
  u32 sector_count_ = 0;
  std::vector<OpticalTrack> tracks_;
  RawReadMethod raw_method_ = RawReadMethod::Cooked;
  mutable std::mutex mutex_;
  std::vector<u8> scratch_;
};

}  // namespace oc::platform
