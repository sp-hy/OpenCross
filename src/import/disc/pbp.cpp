// PSN-style PBP images as made by PC converters: a "\0PBP" header whose last section is either one PSISOIMG disc or a PSTITLEIMG
// table of up to five of them. A disc keeps a block index at +0x4000 (32-byte entries: offset, length) and its blocks of 16 raw
// sectors from +0x100000, each raw-deflated unless it is stored at full size. Encrypted (store-bought) PBPs do not decode to a
// synced first sector and are refused.
#include <miniz.h>

#include <cstring>
#include <fstream>

#include "import/disc/image_files.h"

namespace oc::import::disc {

namespace {

constexpr u32 kSectorsPerBlock = 16;
constexpr u32 kBlockSize = kSectorsPerBlock * kRawSectorSize;
constexpr u64 kIndexOffset = 0x4000;
constexpr u64 kDataOffset = 0x100000;
constexpr size_t kIndexEntrySize = 32;
constexpr u64 kTitleDiscTable = 0x200;
constexpr int kMaxDiscs = 5;

struct Block {
  u32 offset;
  u32 length;
};

u32 get32(const u8* bytes) { return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<u32>(bytes[3]) << 24; }

class PbpSource final : public SectorSource {
 public:
  PbpSource(const std::filesystem::path& path, u64 data_base, std::vector<Block> blocks, std::string description)
      : SectorSource(static_cast<u32>(blocks.size()) * kSectorsPerBlock, true, std::move(description)),
        stream_(path, std::ios::binary),
        data_base_(data_base),
        blocks_(std::move(blocks)) {}

 protected:
  bool fetch(u32 lba, u32 count, u8* out) const override {
    for (u32 i = 0; i < count; ++i, ++lba) {
      if (!load_block(lba / kSectorsPerBlock)) return false;
      std::memcpy(out + static_cast<size_t>(i) * kRawSectorSize, plain_.data() + (lba % kSectorsPerBlock) * kRawSectorSize, kRawSectorSize);
    }
    return true;
  }

 private:
  bool load_block(u32 block) const {
    if (block == cached_) return true;
    cached_ = ~0u;
    const Block& entry = blocks_[block];
    compressed_.resize(entry.length);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(data_base_ + entry.offset));
    if (!stream_.read(reinterpret_cast<char*>(compressed_.data()), entry.length)) return false;
    plain_.assign(kBlockSize, 0);
    if (entry.length == kBlockSize) {
      plain_ = compressed_;
    } else if (tinfl_decompress_mem_to_mem(plain_.data(), plain_.size(), compressed_.data(), compressed_.size(), 0) ==
               TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
      return false;
    }
    cached_ = block;
    return true;
  }

  mutable std::ifstream stream_;
  u64 data_base_;
  std::vector<Block> blocks_;
  mutable u32 cached_ = ~0u;
  mutable Bytes compressed_;
  mutable Bytes plain_;
};

std::unique_ptr<SectorSource> open_pbp_disc(const std::filesystem::path& path, u64 base, size_t number, std::string* error) {
  Bytes magic;
  if (!read_file_range(path, base, 12, magic) || std::memcmp(magic.data(), "PSISOIMG0000", 12) != 0) {
    *error = "pbp: disc " + std::to_string(number) + " has no PSISOIMG header (encrypted or damaged)";
    return nullptr;
  }
  Bytes index;
  if (!read_file_range(path, base + kIndexOffset, static_cast<size_t>(kDataOffset - kIndexOffset), index)) {
    *error = "pbp: block index truncated";
    return nullptr;
  }
  std::vector<Block> blocks;
  for (size_t at = 0; at + kIndexEntrySize <= index.size(); at += kIndexEntrySize) {
    const Block block{get32(&index[at]), static_cast<u32>(index[at + 4] | index[at + 5] << 8)};
    if (block.length == 0) break;
    if (block.length > kBlockSize || (!blocks.empty() && block.offset < blocks.back().offset + blocks.back().length)) {
      *error = "pbp: block index is not a plain PSISOIMG index (encrypted PBPs are not supported)";
      return nullptr;
    }
    blocks.push_back(block);
  }
  if (blocks.empty() || blocks[0].offset != 0) {
    *error = "pbp: empty or encrypted block index (encrypted PBPs are not supported)";
    return nullptr;
  }
  auto source = std::make_unique<PbpSource>(path, base + kDataOffset, std::move(blocks), "pbp disc " + std::to_string(number) + ", raw 2352");
  u8 first[kRawSectorSize];
  if (!source->read_raw(0, 1, first) || !has_sync(first)) {
    *error = "pbp: the first block does not decode to a disc sector (encrypted PBPs are not supported)";
    return nullptr;
  }
  return source;
}

}  // namespace

std::vector<std::unique_ptr<SectorSource>> open_pbp(const std::filesystem::path& path, std::string* error) {
  std::vector<std::unique_ptr<SectorSource>> discs;
  Bytes header;
  if (!read_file_range(path, 0, 0x28, header) || std::memcmp(header.data(), "\0PBP", 4) != 0) {
    *error = "pbp: not a PBP file";
    return discs;
  }
  const u64 psar = get32(&header[0x24]);
  Bytes magic;
  if (!read_file_range(path, psar, 16, magic)) {
    *error = "pbp: data section missing";
    return discs;
  }
  std::vector<u64> bases;
  if (std::memcmp(magic.data(), "PSISOIMG0000", 12) == 0) {
    bases.push_back(psar);
  } else if (std::memcmp(magic.data(), "PSTITLEIMG000000", 16) == 0) {
    Bytes table;
    if (!read_file_range(path, psar + kTitleDiscTable, 4 * kMaxDiscs, table)) {
      *error = "pbp: disc table truncated";
      return discs;
    }
    for (int i = 0; i < kMaxDiscs; ++i)
      if (const u32 offset = get32(&table[4 * static_cast<size_t>(i)]); offset != 0) bases.push_back(psar + offset);
  } else {
    *error = "pbp: data section is neither PSISOIMG nor PSTITLEIMG (encrypted PBPs are not supported)";
    return discs;
  }
  for (size_t i = 0; i < bases.size(); ++i) {
    std::unique_ptr<SectorSource> disc = open_pbp_disc(path, bases[i], i + 1, error);
    if (!disc) {
      discs.clear();
      return discs;
    }
    discs.push_back(std::move(disc));
  }
  if (discs.empty()) *error = "pbp: no discs";
  return discs;
}

}  // namespace oc::import::disc
