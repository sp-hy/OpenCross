#pragma once
// What a Chrono Cross disc holds: the ISO9660 volume (only the boot executable and SYSTEM.CNF live there), the game's own file
// table (one word per file record) and the disc-number sector.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "import/disc/sector_source.h"

namespace oc::import::disc {

struct IsoEntry {
  // As stored, without the ";1" version suffix.
  std::string name;
  u32 lba = 0;
  u32 size = 0;
  bool directory = false;
};

struct IsoVolume {
  std::string system_id;
  std::string volume_id;
  u32 volume_sectors = 0;
  std::vector<IsoEntry> root;

  // Root entry by name, ignoring case; null when absent.
  const IsoEntry* find(std::string_view name) const;
};

// The primary volume descriptor (sector 16) and the root directory.
std::optional<IsoVolume> read_iso_volume(const SectorSource& source, std::string* error);
std::optional<Bytes> read_iso_file(const SectorSource& source, const IsoEntry& entry);
// The executable SYSTEM.CNF's BOOT line names ("SLPS_023.64"), without device, path or version; "" when there is none.
std::string boot_file_name(std::string_view system_cnf);

constexpr u32 kFileTableFirstSector = 24;
constexpr u32 kFileTableSectors = 12;
constexpr u32 kDiscNumberSector = 37;

struct FileSpan {
  u32 lba = 0;
  u32 size = 0;
  // The record names no data (size 0).
  bool empty = false;
};

class FileTable {
 public:
  // Decodes the table's bytes: little-endian words, bits 0-22 the start LBA, bit 23 the no-data flag, bits 24-31 the unused bytes
  // of the last sector divided by 8. The table ends at the first zero word. A record's size runs to the next record's LBA; the
  // last one's to `end_lba`.
  static FileTable decode(ByteSpan words, u32 end_lba);
  static std::optional<FileTable> read(const SectorSource& source, std::string* error);

  size_t size() const { return spans_.size(); }
  const FileSpan& span(size_t record) const { return spans_[record]; }
  const std::vector<FileSpan>& spans() const { return spans_; }
  // Start LBAs never decrease and stay inside the image.
  bool plausible(u32 sector_count) const;

 private:
  std::vector<FileSpan> spans_;
};

// The bytes of `record` (empty for an empty record); nullopt when the record does not exist or the read fails.
std::optional<Bytes> read_record(const SectorSource& source, const FileTable& table, size_t record);

// The u32 at the start of the disc-number sector's user data: 1 or 2 on a Chrono Cross disc.
std::optional<u32> read_disc_number(const SectorSource& source);

}  // namespace oc::import::disc
