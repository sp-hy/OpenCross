#pragma once
// Internal to the disc reader: a sector source over runs of sectors stored in one or more files, and the PBP opener.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "import/disc/sector_source.h"

namespace oc::import::disc {

// `count` sectors from disc LBA `first_lba` on, stored `stride` bytes apart (2048 cooked, 2352 raw, 2448 raw + interleaved
// subchannel) from byte `offset` of file `file`. File -1 is a gap the image does not store; it reads as zeros.
struct Extent {
  int file = -1;
  u64 offset = 0;
  u32 stride = 0;
  u32 first_lba = 0;
  u32 count = 0;
};

// A source over `extents`, which must be sorted and contiguous from LBA 0. It is raw when every stored extent is.
std::unique_ptr<SectorSource> make_file_source(const std::vector<std::filesystem::path>& files, std::vector<Extent> extents,
                                               const std::string& container, std::string* error);

// The discs of a PBP file (one for PSISOIMG, up to five for PSTITLEIMG).
std::vector<std::unique_ptr<SectorSource>> open_pbp(const std::filesystem::path& path, std::string* error);

// `size` bytes from `offset` of a file; false when the file is shorter.
bool read_file_range(const std::filesystem::path& path, u64 offset, size_t size, Bytes& out);

}  // namespace oc::import::disc
