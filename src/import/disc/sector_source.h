#pragma once
// A CD image as an array of sectors addressed by LBA (LBA 0 = the first sector of the first track). Raw images give the full
// 2352-byte sector (sync, header, subheader, payload, EDC/ECC); cooked images keep only the 2048 user bytes of each sector, so a
// reader of form-2 data (XA audio, STR streams) has to check raw() first. Every public read is thread-safe.
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "core/types.h"

namespace oc::import::disc {

constexpr size_t kRawSectorSize = 2352;
constexpr size_t kUserSectorSize = 2048;
constexpr size_t kForm2PayloadSize = 2324;
constexpr size_t kSubchannelSize = 96;
// Sync (12) + header (4) + mode-2 subheader (8): where a mode-2 payload starts.
constexpr size_t kMode2PayloadOffset = 24;
constexpr size_t kMode1PayloadOffset = 16;

// Mode-2 subheader submode bits.
constexpr u8 kSubmodeEndOfRecord = 0x01;
constexpr u8 kSubmodeVideo = 0x02;
constexpr u8 kSubmodeAudio = 0x04;
constexpr u8 kSubmodeData = 0x08;
constexpr u8 kSubmodeForm2 = 0x20;
constexpr u8 kSubmodeEndOfFile = 0x80;

// True when the 12 bytes start with the CD sync pattern 00 FF*10 00.
bool has_sync(const u8* bytes);

struct SectorInfo {
  // 0 = no sync (an audio sector), 1 or 2 = the header's mode byte. A cooked image reports mode 1.
  u8 mode = 0;
  // Mode-2 subheader (file, channel, submode, coding); zero for other modes.
  u8 file = 0;
  u8 channel = 0;
  u8 submode = 0;
  u8 coding = 0;
  // 1 or 2 for mode-2 sectors, 1 for mode 1 and cooked sectors, 0 for audio.
  u8 form = 0;
  // Bytes of payload the sector carries: 2048, 2324 (form 2), 2352 (audio).
  size_t payload_size() const { return mode == 0 ? kRawSectorSize : form == 2 ? kForm2PayloadSize : kUserSectorSize; }
};

class SectorSource {
 public:
  virtual ~SectorSource() = default;
  SectorSource(const SectorSource&) = delete;
  SectorSource& operator=(const SectorSource&) = delete;

  u32 sector_count() const { return sector_count_; }
  // True when read_raw() gives whole sectors; false for cooked (2048-byte) images.
  bool raw() const { return raw_; }
  // Container and layout, e.g. "cue+bin, raw 2352".
  const std::string& description() const { return description_; }

  // `count` whole sectors, 2352 bytes each, into `out`. False when the image is cooked or the range runs past the end.
  bool read_raw(u32 lba, u32 count, u8* out) const;
  // `count` sectors of user data, 2048 bytes each: the mode-1 payload or the mode-2 form-1 payload (for a form-2 sector, its first
  // 2048 payload bytes, as a drive in 2048-byte mode returns them).
  bool read_user(u32 lba, u32 count, u8* out) const;
  Bytes read_user(u32 lba, u32 count) const;
  // The 2324-byte payload of a form-2 sector. False on a cooked image or a sector that is not mode 2.
  bool read_form2(u32 lba, u8* out) const;
  std::optional<SectorInfo> info(u32 lba) const;

 protected:
  SectorSource(u32 sector_count, bool raw, std::string description)
      : sector_count_(sector_count), raw_(raw), description_(std::move(description)) {}
  // `count` sectors as stored, already range-checked: 2352 bytes each when raw(), else 2048, packed into `out`. Called with the
  // source's lock held.
  virtual bool fetch(u32 lba, u32 count, u8* out) const = 0;

 private:
  u32 sector_count_;
  bool raw_;
  std::string description_;
  mutable std::mutex mutex_;
};

}  // namespace oc::import::disc
