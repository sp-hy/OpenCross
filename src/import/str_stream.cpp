#include "import/str_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace oc::import {

namespace {

constexpr u16 kMagic = 0x0160;
constexpr u8 kVideoBit = 0x80;
constexpr u8 kTypeLeft = 0, kTypeRight = 1, kTypeStrip = 3;
constexpr u32 kMissing = std::numeric_limits<u32>::max();
constexpr size_t kAudioOffset = 0xD0;
constexpr size_t kStripOffset = 0x20, kStripRowBytes = 0x1B0;
// Sectors read per second at double CD speed.
constexpr double kSectorRate = 150.0;

u16 le16(const u8* bytes) { return static_cast<u16>(bytes[0] | bytes[1] << 8); }
u32 le32(const u8* bytes) { return static_cast<u32>(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | static_cast<u32>(bytes[3]) << 24); }

}  // namespace

bool StrStream::parse(ByteSpan data) {
  data_ = data;
  frames_.clear();
  audio_.clear();
  strip_.clear();
  width_ = height_ = frame_count_ = 0;
  fps_ = 15.0;
  const u32 sectors = static_cast<u32>(data.size() / kSectorBytes);
  u32 pending_left = kMissing;
  std::vector<Frame> raw;
  for (u32 index = 0; index < sectors; ++index) {
    const u8* header = sector(index);
    if (le16(header) != kMagic) continue;
    const u8 type = header[2];
    if (header[3] & kVideoBit) {
      if (type != 1) continue;
      const u16 chunk = le16(header + 4), count = le16(header + 6);
      const u32 number = le32(header + 8);
      if (count == 0 || chunk >= count) continue;
      if (raw.empty() || raw.back().number != number) {
        Frame frame;
        frame.number = number;
        frame.size = le32(header + 0x0C);
        frame.width = le16(header + 0x10);
        frame.height = le16(header + 0x12);
        frame.chunks.assign(count, kMissing);
        frame.audio_before = static_cast<u32>(audio_.size());
        raw.push_back(std::move(frame));
      }
      if (chunk < raw.back().chunks.size()) raw.back().chunks[chunk] = index;
    } else if (type == kTypeLeft) {
      pending_left = index;
    } else if (type == kTypeRight) {
      if (pending_left != kMissing) audio_.push_back({pending_left, index});
      pending_left = kMissing;
    } else if (type == kTypeStrip) {
      strip_.push_back(index);
    }
  }
  // A frame is usable when the chunks its bitstream spans are all there; the filler chunks after it may be missing.
  for (Frame& frame : raw) {
    const size_t needed = (static_cast<size_t>(frame.size) + kChunkBytes - 1) / kChunkBytes;
    if (frame.size == 0 || needed > frame.chunks.size() || frame.width == 0 || frame.height == 0) continue;
    if (std::any_of(frame.chunks.begin(), frame.chunks.begin() + static_cast<std::ptrdiff_t>(needed), [](u32 at) { return at == kMissing; }))
      continue;
    frame.chunks.resize(needed);
    frames_.push_back(std::move(frame));
  }
  if (frames_.empty()) return false;
  width_ = frames_.front().width;
  height_ = frames_.front().height;
  for (const Frame& frame : frames_) frame_count_ = std::max(frame_count_, frame.number);
  if (frames_.size() >= 2) {
    const double per_frame = static_cast<double>(frames_.back().chunks.front() - frames_.front().chunks.front()) /
                             static_cast<double>(frames_.size() - 1);
    const double sectors_per_frame = std::round(per_frame);
    if (sectors_per_frame >= 1.0) fps_ = kSectorRate / sectors_per_frame;
  }
  return true;
}

Bytes StrStream::bitstream(const Frame& frame) const {
  Bytes out(frame.chunks.size() * kChunkBytes);
  for (size_t i = 0; i < frame.chunks.size(); ++i) std::memcpy(out.data() + i * kChunkBytes, sector(frame.chunks[i]) + 0x20, kChunkBytes);
  out.resize(frame.size);
  return out;
}

ByteSpan StrStream::audio_payload(const AudioGroup& group, int channel) const {
  return {sector(channel ? group.right : group.left) + kAudioOffset, kAudioBytes};
}

std::vector<u8> StrStream::strip_image(bool second) const {
  std::vector<u8> rgba(static_cast<size_t>(kStripWidth) * strip_.size() * 4);
  u8* out = rgba.data();
  for (u32 index : strip_) {
    const u8* row = sector(index) + kStripOffset + (second ? kStripRowBytes : 0);
    for (u32 x = 0; x < kStripWidth; ++x, out += 4, row += 3) {
      out[0] = row[0];
      out[1] = row[1];
      out[2] = row[2];
      out[3] = 255;
    }
  }
  return rgba;
}

}  // namespace oc::import
