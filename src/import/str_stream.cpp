#include "import/str_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oc::import {

namespace {

constexpr u16 kMagic = 0x0160;
constexpr u8 kVideoBit = 0x80;
constexpr u8 kTypeLeft = 0, kTypeRight = 1, kTypeStrip = 3;
constexpr size_t kAudioOffset = 0xD0;
constexpr size_t kStripOffset = 0x20;
// Sectors read per second at double CD speed.
constexpr double kSectorRate = 150.0;
// Sectors per read (an optical drive's cache block), the first sectors open() takes the picture size and the frame rate from, and the
// last ones it takes the frame count from.
constexpr u32 kBlockSectors = 32, kHeadSectors = 256, kTailSectors = 256;
// How far next() reads past the item it hands out, at most two blocks a call. The staff roll shows strip rows up to about 1.4% of the
// frame number ahead of the frame on screen (125 rows at the end of `ending_s`); 2048 sectors hold 409 strip rows at 5 sectors a frame.
constexpr u32 kLookaheadSectors = 2048, kLookaheadBlocks = 2;

u16 le16(const u8* bytes) { return static_cast<u16>(bytes[0] | bytes[1] << 8); }
u32 le32(const u8* bytes) { return static_cast<u32>(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<u32>(bytes[3]) << 24); }

// A video chunk's header: the magic, video type 1, a chunk index below the chunk count.
bool is_video_chunk(const u8* sector) {
  if (le16(sector) != kMagic || !(sector[3] & kVideoBit) || sector[2] != 1) return false;
  const u16 chunk = le16(sector + 4), count = le16(sector + 6);
  return count != 0 && chunk < count;
}

}  // namespace

bool StrStream::open(ReadSectors read, u32 sector_count) {
  read_ = std::move(read);
  sector_count_ = sector_count;
  cursor_ = 0;
  failed_ = have_left_ = has_audio_ = has_strip_ = false;
  pending_.clear();
  open_frame_ = nullptr;
  strip_.clear();
  width_ = height_ = frame_count_ = 0;
  fps_ = 15.0;
  // The first sectors, read on their own: the first usable frame's size, the spacing of the first frames.
  StrStream head;
  head.read_ = read_;
  head.sector_count_ = std::min(sector_count, kHeadSectors);
  Item item;
  u32 frames = 0, first_sector = 0, last_sector = 0;
  while (head.next(item)) {
    if (item.audio) {
      has_audio_ = true;
      continue;
    }
    if (frames++ == 0) {
      width_ = item.width;
      height_ = item.height;
      first_sector = item.sector;
    }
    last_sector = item.sector;
    frame_count_ = std::max(frame_count_, item.number);
  }
  has_strip_ = !head.strip_.empty();
  if (frames == 0) return false;
  if (frames >= 2) {
    const double sectors_per_frame = std::round(static_cast<double>(last_sector - first_sector) / static_cast<double>(frames - 1));
    if (sectors_per_frame >= 1.0) fps_ = kSectorRate / sectors_per_frame;
  }
  // The last frame carries the highest frame number.
  if (sector_count > kHeadSectors) {
    const u32 tail = std::min(sector_count, kTailSectors);
    Bytes sectors(static_cast<size_t>(tail) * kSectorBytes);
    if (read_(sector_count - tail, tail, sectors.data())) {
      for (u32 i = 0; i < tail; ++i) {
        const u8* sector = sectors.data() + static_cast<size_t>(i) * kSectorBytes;
        if (is_video_chunk(sector)) frame_count_ = std::max(frame_count_, le32(sector + 8));
      }
    }
  }
  return true;
}

bool StrStream::read_block() {
  if (failed_ || cursor_ >= sector_count_) return false;
  const u32 count = std::min(kBlockSectors - cursor_ % kBlockSectors, sector_count_ - cursor_);
  block_.resize(static_cast<size_t>(count) * kSectorBytes);
  if (!read_ || !read_(cursor_, count, block_.data())) {
    failed_ = true;
    return false;
  }
  for (u32 i = 0; i < count; ++i) take(cursor_ + i, block_.data() + static_cast<size_t>(i) * kSectorBytes);
  cursor_ += count;
  if (cursor_ >= sector_count_) close_frame();
  return true;
}

void StrStream::take(u32 index, const u8* sector) {
  if (le16(sector) != kMagic) return;
  const u8 type = sector[2];
  if (sector[3] & kVideoBit) {
    if (!is_video_chunk(sector)) return;
    const u16 chunk = le16(sector + 4), count = le16(sector + 6);
    const u32 number = le32(sector + 8);
    if (!open_frame_ || open_frame_->item.number != number) {
      close_frame();
      Pending frame;
      frame.done = false;
      frame.item.sector = index;
      frame.item.number = number;
      frame.size = le32(sector + 0x0C);
      frame.item.width = le16(sector + 0x10);
      frame.item.height = le16(sector + 0x12);
      frame.item.bitstream.assign(static_cast<size_t>(count) * kChunkBytes, 0);
      frame.have.assign(count, false);
      pending_.push_back(std::move(frame));
      open_frame_ = &pending_.back();
    }
    if (chunk < open_frame_->have.size()) {
      std::memcpy(open_frame_->item.bitstream.data() + static_cast<size_t>(chunk) * kChunkBytes, sector + 0x20, kChunkBytes);
      open_frame_->have[chunk] = true;
    }
  } else if (type == kTypeLeft) {
    left_.assign(sector + kAudioOffset, sector + kAudioOffset + kAudioBytes);
    have_left_ = true;
  } else if (type == kTypeRight) {
    if (have_left_) {
      Pending group;
      group.item.audio = true;
      group.item.sector = index;
      group.item.left = std::move(left_);
      group.item.right.assign(sector + kAudioOffset, sector + kAudioOffset + kAudioBytes);
      pending_.push_back(std::move(group));
    }
    have_left_ = false;
  } else if (type == kTypeStrip) {
    strip_.insert(strip_.end(), sector + kStripOffset, sector + kStripOffset + kStripSectorBytes);
  }
}

// A frame is usable when the chunks its bitstream spans are all there; the filler chunks after it may be missing.
void StrStream::close_frame() {
  if (!open_frame_) return;
  Pending& frame = *open_frame_;
  open_frame_ = nullptr;
  frame.done = true;
  const size_t needed = (static_cast<size_t>(frame.size) + kChunkBytes - 1) / kChunkBytes;
  frame.usable = frame.size != 0 && needed <= frame.have.size() && frame.item.width != 0 && frame.item.height != 0 &&
                 std::all_of(frame.have.begin(), frame.have.begin() + static_cast<std::ptrdiff_t>(needed), [](bool have) { return have; });
  if (frame.usable) frame.item.bitstream.resize(frame.size);
  else frame.item.bitstream.clear();
  frame.have.clear();
}

void StrStream::drop_unusable() {
  while (!pending_.empty() && pending_.front().done && !pending_.front().usable) pending_.pop_front();
}

bool StrStream::next(Item& out) {
  for (;;) {
    drop_unusable();
    if (!pending_.empty() && pending_.front().done) break;
    if (!read_block()) {
      drop_unusable();
      if (pending_.empty() || !pending_.front().done) return false;
      break;
    }
  }
  const u32 ahead = pending_.front().item.sector + kLookaheadSectors;
  for (u32 block = 0; block < kLookaheadBlocks && cursor_ < ahead; ++block)
    if (!read_block()) break;
  out = std::move(pending_.front().item);
  pending_.pop_front();
  return true;
}

std::vector<u8> StrStream::strip_image(ByteSpan data, size_t rows, bool second) {
  std::vector<u8> rgba(static_cast<size_t>(kStripWidth) * rows * 4, 0);
  const size_t have = std::min(rows, data.size() / kStripSectorBytes);
  u8* out = rgba.data();
  for (size_t index = 0; index < rows; ++index) {
    const u8* row = index < have ? data.data() + index * kStripSectorBytes + (second ? kStripRowBytes : 0) : nullptr;
    for (u32 x = 0; x < kStripWidth; ++x, out += 4) {
      if (row) {
        out[0] = row[x * 3];
        out[1] = row[x * 3 + 1];
        out[2] = row[x * 3 + 2];
      }
      out[3] = 255;
    }
  }
  return rgba;
}

}  // namespace oc::import
