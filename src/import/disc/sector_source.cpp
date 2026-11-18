#include "import/disc/sector_source.h"

#include <cstring>

namespace oc::import::disc {

bool has_sync(const u8* bytes) {
  if (bytes[0] != 0 || bytes[11] != 0) return false;
  for (int i = 1; i < 11; ++i)
    if (bytes[i] != 0xFF) return false;
  return true;
}

bool SectorSource::read_raw(u32 lba, u32 count, u8* out) const {
  if (!raw_ || lba > sector_count_ || count > sector_count_ - lba) return false;
  const std::lock_guard lock(mutex_);
  return fetch(lba, count, out);
}

bool SectorSource::read_user(u32 lba, u32 count, u8* out) const {
  if (lba > sector_count_ || count > sector_count_ - lba) return false;
  if (!raw_) {
    const std::lock_guard lock(mutex_);
    return fetch(lba, count, out);
  }
  Bytes sectors(static_cast<size_t>(count) * kRawSectorSize);
  {
    const std::lock_guard lock(mutex_);
    if (!fetch(lba, count, sectors.data())) return false;
  }
  for (u32 i = 0; i < count; ++i) {
    const u8* sector = sectors.data() + static_cast<size_t>(i) * kRawSectorSize;
    const size_t at = has_sync(sector) && sector[15] == 1 ? kMode1PayloadOffset : kMode2PayloadOffset;
    std::memcpy(out + static_cast<size_t>(i) * kUserSectorSize, sector + at, kUserSectorSize);
  }
  return true;
}

Bytes SectorSource::read_user(u32 lba, u32 count) const {
  Bytes out(static_cast<size_t>(count) * kUserSectorSize);
  if (!read_user(lba, count, out.data())) out.clear();
  return out;
}

bool SectorSource::read_form2(u32 lba, u8* out) const {
  u8 sector[kRawSectorSize];
  if (!read_raw(lba, 1, sector) || !has_sync(sector) || sector[15] != 2) return false;
  std::memcpy(out, sector + kMode2PayloadOffset, kForm2PayloadSize);
  return true;
}

std::optional<SectorInfo> SectorSource::info(u32 lba) const {
  if (lba >= sector_count_) return std::nullopt;
  SectorInfo out;
  if (!raw_) {
    out.mode = 1;
    out.form = 1;
    return out;
  }
  u8 sector[kRawSectorSize];
  if (!read_raw(lba, 1, sector)) return std::nullopt;
  if (!has_sync(sector)) return out;
  out.mode = sector[15];
  out.form = 1;
  if (out.mode == 2) {
    out.file = sector[16];
    out.channel = sector[17];
    out.submode = sector[18];
    out.coding = sector[19];
    out.form = (out.submode & kSubmodeForm2) ? 2 : 1;
  }
  return out;
}

}  // namespace oc::import::disc
