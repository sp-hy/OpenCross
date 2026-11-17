#pragma once
// PS1 movie streams (the discs' movie/<name>.stv records): the 2048-byte user data of each sector in order. Every sector starts with a
// 32-byte header that says what it carries: a chunk of an MDEC video frame, one channel of a group's SPU ADPCM audio, or two rows of
// the staff-roll strip. parse() indexes the sectors; the accessors hand out a frame's assembled bitstream, a group's ADPCM payloads
// and the strip rows. The stream bytes are not copied: the span given to parse() must outlive the index.
#include <vector>

#include "core/types.h"

namespace oc::import {

class StrStream {
 public:
  static constexpr size_t kSectorBytes = 2048;
  // Payload of a video chunk (user data 0x20..0x7FF).
  static constexpr size_t kChunkBytes = 2016;
  // SPU ADPCM bytes per channel and group: 105 blocks of 28 samples.
  static constexpr size_t kAudioBytes = 1680;
  static constexpr u32 kAudioSamples = 2940;
  static constexpr u32 kAudioRate = 44100;
  // A strip row: 144 pixels of 24-bit RGB.
  static constexpr u32 kStripWidth = 144;

  struct Frame {
    // 1-based frame number from the headers.
    u32 number = 0;
    u32 size = 0;
    u16 width = 0, height = 0;
    // Sector of each chunk, in chunk order.
    std::vector<u32> chunks;
    // Audio groups that come before the frame's first chunk.
    u32 audio_before = 0;
  };
  struct AudioGroup {
    u32 left = 0, right = 0;
  };

  // Indexes `data` (whole sectors; a trailing partial sector is ignored). False when it holds no complete video frame.
  bool parse(ByteSpan data);

  const std::vector<Frame>& frames() const { return frames_; }
  const std::vector<AudioGroup>& audio() const { return audio_; }
  u32 width() const { return width_; }
  u32 height() const { return height_; }
  // The highest frame number.
  u32 frame_count() const { return frame_count_; }
  // 150 sectors a second divided by the sectors per frame (15 or 30).
  double fps() const { return fps_; }
  size_t strip_rows() const { return strip_.size(); }

  // The frame's MDEC bitstream: its chunks' payloads in order, cut to the frame size.
  Bytes bitstream(const Frame& frame) const;
  // A group's ADPCM payload for channel 0 (left) or 1 (right): kAudioBytes.
  ByteSpan audio_payload(const AudioGroup& group, int channel) const;
  // The strip as RGBA, kStripWidth x strip_rows(): row A of every strip sector (`second` = false) or row B.
  std::vector<u8> strip_image(bool second) const;

 private:
  const u8* sector(u32 index) const { return data_.data() + static_cast<size_t>(index) * kSectorBytes; }

  ByteSpan data_;
  std::vector<Frame> frames_;
  std::vector<AudioGroup> audio_;
  std::vector<u32> strip_;
  u32 width_ = 0, height_ = 0, frame_count_ = 0;
  double fps_ = 15.0;
};

}  // namespace oc::import
