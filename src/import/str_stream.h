#pragma once
// PS1 movie streams (the discs' movie/<name>.stv records): the 2048-byte user data of each sector in order. Every sector starts with a
// 32-byte header that says what it carries: a chunk of an MDEC video frame, one channel of a group's SPU ADPCM audio, or two rows of
// the staff-roll strip. The stream is read front to back in blocks, never whole (a disc drive streams it as the PS1 does): open() reads
// its first sectors (picture size, frame rate) and its last ones (frame count); next() hands out the audio groups and the assembled
// frames in stream order, reading a little ahead; the strip rows pile up as the stream goes by.
#include <deque>
#include <functional>
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
  // The two strip rows of a strip sector (row A, then row B).
  static constexpr size_t kStripRowBytes = kStripWidth * 3;
  static constexpr size_t kStripSectorBytes = 2 * kStripRowBytes;

  // `count` sectors from sector `first` of the stream into `out`, 2048 bytes each; false when the read fails.
  using ReadSectors = std::function<bool(u32 first, u32 count, u8* out)>;

  // One item of the stream: an audio group (`audio`: the left and right channel's ADPCM payloads, kAudioBytes each) or a video frame
  // whose bitstream chunks are all there (its 1-based number and picture size from the headers, its MDEC bitstream cut to the frame
  // size). An audio group comes before a frame when it is stored before the frame's first chunk. `sector`: where the item starts.
  struct Item {
    bool audio = false;
    u32 sector = 0;
    Bytes left, right;
    u32 number = 0;
    u16 width = 0, height = 0;
    Bytes bitstream;
  };

  // Starts reading a stream of `sector_count` sectors (a trailing partial sector is not counted). False when its first sectors hold no
  // complete video frame.
  bool open(ReadSectors read, u32 sector_count);
  // The next item; false at the end of the stream or once a read failed.
  bool next(Item& out);

  u32 width() const { return width_; }
  u32 height() const { return height_; }
  // The highest frame number of the stream's last sectors.
  u32 frame_count() const { return frame_count_; }
  // 150 sectors a second divided by the sectors per frame of the first frames (15 or 30).
  double fps() const { return fps_; }
  // The first sectors hold an audio group.
  bool has_audio() const { return has_audio_; }
  // The first sectors hold a strip sector. A strip stream carries one strip sector per frame, so its strip is frame_count() rows.
  bool has_strip() const { return has_strip_; }
  // The strip sectors read so far, in order, kStripSectorBytes each.
  const Bytes& strip_data() const { return strip_; }
  // `rows` rows of strip sectors (`data`, kStripSectorBytes each, possibly fewer than `rows`) as RGBA, kStripWidth x rows: row A of
  // each sector (`second` = false) or row B; rows not in `data` are black.
  static std::vector<u8> strip_image(ByteSpan data, size_t rows, bool second);

 private:
  // An item being read: a frame stays open (`done` false) until a sector of another frame or the end of the stream; `usable` = its
  // bitstream's chunks are all there.
  struct Pending {
    Item item;
    bool done = true, usable = true;
    u32 size = 0;
    std::vector<bool> have;
  };
  // Reads and takes the next block of sectors. False at the end of the stream or when the read fails.
  bool read_block();
  void take(u32 index, const u8* sector);
  void close_frame();
  // Removes the frames found unusable from the front.
  void drop_unusable();

  ReadSectors read_;
  u32 sector_count_ = 0, cursor_ = 0;
  bool failed_ = false;
  Bytes block_;
  std::deque<Pending> pending_;
  Pending* open_frame_ = nullptr;
  Bytes left_;
  bool have_left_ = false;
  Bytes strip_;
  u32 width_ = 0, height_ = 0, frame_count_ = 0;
  double fps_ = 15.0;
  bool has_audio_ = false, has_strip_ = false;
};

}  // namespace oc::import
