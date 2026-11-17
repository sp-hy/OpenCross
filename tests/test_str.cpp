// PS1 movie streams on synthetic data: MDEC frames built bit by bit (flat and coloured macroblocks, AC and escape codes, a bad
// code), sector streams in both interleaves (15 fps L R V..V, 30 fps with staff strip sectors), frame assembly cut to the bitstream
// size, audio pairing, strip rows, and the movie decoder playing a stream end to end.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "import/mdec.h"
#include "import/str_stream.h"
#include "movie/decoder.h"

using namespace oc;
using import::StrStream;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// MDEC bitstream writer: bits from the most significant end of little-endian 16-bit words.
class BitWriter {
 public:
  void put(u32 value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      word_ = static_cast<u16>(word_ << 1 | ((value >> i) & 1));
      if (++bits_ == 16) flush();
    }
  }
  void put_code(const char* bits) {
    for (const char* bit = bits; *bit; ++bit) put(*bit == '1', 1);
  }
  // Header (quantiser, version 2) + the bits, padded to a whole word.
  Bytes frame(u16 quantiser) {
    while (bits_) put(0, 1);
    Bytes out = {0, 0, 0x00, 0x38, static_cast<u8>(quantiser), 0, 2, 0};
    out.insert(out.end(), data_.begin(), data_.end());
    return out;
  }

 private:
  void flush() {
    data_.push_back(static_cast<u8>(word_));
    data_.push_back(static_cast<u8>(word_ >> 8));
    word_ = 0;
    bits_ = 0;
  }
  Bytes data_;
  u16 word_ = 0;
  int bits_ = 0;
};

void flat_block(BitWriter& writer, int dc) {
  writer.put(static_cast<u32>(dc) & 0x3FF, 10);
  writer.put_code("10");
}

// One macroblock: Cr, Cb and four luma blocks, each flat.
void flat_macroblock(BitWriter& writer, int cr, int cb, int luma) {
  flat_block(writer, cr);
  flat_block(writer, cb);
  for (int i = 0; i < 4; ++i) flat_block(writer, luma);
}

void test_mdec() {
  {
    BitWriter writer;
    flat_macroblock(writer, 0, 0, 40);
    std::vector<u8> rgba;
    CHECK(import::mdec_decode(writer.frame(1), 16, 16, rgba));
    CHECK(rgba.size() == 16u * 16u * 4u);
    // DC 40 x 2 through the inverse DCT = +10 on 128.
    CHECK(rgba[0] == 138 && rgba[1] == 138 && rgba[2] == 138 && rgba[3] == 255);
    CHECK(rgba[(15 * 16 + 15) * 4] == 138);
  }
  {
    // Red chroma: Cr +200 (x 2 / 8 = +50) lifts red by 1.402 x 50 and lowers green by 0.714 x 50.
    BitWriter writer;
    flat_macroblock(writer, 200, 0, 0);
    std::vector<u8> rgba;
    CHECK(import::mdec_decode(writer.frame(1), 16, 16, rgba));
    CHECK(rgba[0] == 198 && rgba[1] == 92 && rgba[2] == 128);
  }
  {
    // Two macroblocks (32 x 16, column order): the first block uses a table code, a long code and an escape; the second macroblock
    // must still decode flat, so every code consumed exactly its bits.
    BitWriter writer;
    writer.put(0, 10);
    writer.put_code("11");
    writer.put(1, 1);
    writer.put_code("0000000000011011");
    writer.put(0, 1);
    writer.put_code("000001");
    writer.put(3, 6);
    writer.put(static_cast<u32>(-5) & 0x3FF, 10);
    writer.put_code("10");
    flat_block(writer, 0);
    for (int i = 0; i < 4; ++i) flat_block(writer, 0);
    flat_macroblock(writer, 0, 0, -40);
    std::vector<u8> rgba;
    CHECK(import::mdec_decode(writer.frame(2), 32, 16, rgba));
    CHECK(rgba[16 * 4] == 118 && rgba[(15 * 32 + 31) * 4] == 118);
    CHECK(rgba[2] == 128);
  }
  {
    BitWriter writer;
    writer.put(0, 10);
    writer.put_code("0000000000000001");
    std::vector<u8> rgba;
    CHECK(!import::mdec_decode(writer.frame(1), 16, 16, rgba));
  }
  {
    // Truncated: one macroblock's worth of data for a 32 x 16 frame.
    BitWriter writer;
    flat_macroblock(writer, 0, 0, 0);
    std::vector<u8> rgba;
    CHECK(!import::mdec_decode(writer.frame(1), 32, 16, rgba));
  }
}

// Sector builders. Every sector is 2048 bytes of user data with the 32-byte header.
Bytes sector_header(u8 type, u8 flags) {
  Bytes sector(StrStream::kSectorBytes, 0xFF);
  sector[0] = 0x60;
  sector[1] = 0x01;
  sector[2] = type;
  sector[3] = flags;
  return sector;
}

void put16(Bytes& sector, size_t at, u32 value) {
  sector[at] = static_cast<u8>(value);
  sector[at + 1] = static_cast<u8>(value >> 8);
}

void put32(Bytes& sector, size_t at, u32 value) {
  put16(sector, at, value);
  put16(sector, at + 2, value >> 16);
}

Bytes video_sector(u8 disc_bits, u16 chunk, u16 chunks, u32 number, const Bytes& bitstream, u16 width, u16 height) {
  Bytes sector = sector_header(1, static_cast<u8>(0x80 | disc_bits));
  put16(sector, 4, chunk);
  put16(sector, 6, chunks);
  put32(sector, 8, number);
  put32(sector, 0x0C, static_cast<u32>(bitstream.size()));
  put16(sector, 0x10, width);
  put16(sector, 0x12, height);
  put32(sector, 0x1C, 0);
  const size_t from = static_cast<size_t>(chunk) * StrStream::kChunkBytes;
  for (size_t i = 0; i < StrStream::kChunkBytes && from + i < bitstream.size(); ++i) sector[0x20 + i] = bitstream[from + i];
  return sector;
}

// SPU ADPCM payload of 105 blocks: shift 12, filter 0, every nibble `nibble`.
Bytes audio_sector(u8 channel, u8 disc_bits, u8 nibble) {
  Bytes sector = sector_header(channel, disc_bits);
  put16(sector, 4, channel);
  put16(sector, 6, 2);
  for (size_t block = 0; block < 105; ++block) {
    u8* at = sector.data() + 0xD0 + block * 16;
    at[0] = 0x0C;
    at[1] = block == 104 ? 3 : 2;
    std::memset(at + 2, nibble * 0x11, 14);
  }
  return sector;
}

Bytes strip_sector(u8 disc_bits, u8 row_a, u8 row_b) {
  Bytes sector = sector_header(3, disc_bits);
  put16(sector, 4, 0);
  put16(sector, 6, 1);
  std::memset(sector.data() + 0x20, row_a, 0x1B0);
  std::memset(sector.data() + 0x20 + 0x1B0, row_b, 0x1B0);
  return sector;
}

void append(Bytes& stream, const Bytes& sector) { stream.insert(stream.end(), sector.begin(), sector.end()); }

// A 32 x 16 frame of two flat macroblocks plus `extra` bytes of padding codes, so its bitstream spans more than one chunk.
Bytes test_frame(int luma, size_t extra) {
  BitWriter writer;
  flat_macroblock(writer, 0, 0, luma);
  flat_macroblock(writer, 0, 0, luma);
  Bytes frame = writer.frame(1);
  frame.resize(frame.size() + extra, 0);
  return frame;
}

void test_stream_15fps() {
  // Disc 1 layout, three groups L R V0..V7; frame 2 spans two chunks, the other chunks are filler.
  Bytes stream;
  const Bytes frames[3] = {test_frame(40, 0), test_frame(-40, 3000), test_frame(0, 0)};
  for (u32 group = 0; group < 3; ++group) {
    append(stream, audio_sector(0, 0, 1));
    append(stream, audio_sector(1, 0, 15));
    for (u16 chunk = 0; chunk < 8; ++chunk) append(stream, video_sector(0, chunk, 8, group + 1, frames[group], 32, 16));
  }
  StrStream index;
  CHECK(index.parse(stream));
  CHECK(index.frames().size() == 3);
  CHECK(index.frame_count() == 3);
  CHECK(index.width() == 32 && index.height() == 16);
  CHECK(std::abs(index.fps() - 15.0) < 1e-9);
  CHECK(index.audio().size() == 3);
  CHECK(index.frames()[0].audio_before == 1 && index.frames()[2].audio_before == 3);
  CHECK(index.frames()[1].chunks.size() == 2);
  CHECK(index.bitstream(index.frames()[1]) == frames[1]);
  CHECK(index.audio_payload(index.audio()[0], 1)[2] == 0xFF);
  CHECK(index.strip_rows() == 0);

  movie::Decoder decoder;
  CHECK(decoder.open(stream, "movie/test.stv"));
  CHECK(decoder.width() == 32 && decoder.height() == 16 && decoder.display_width() == 128 && decoder.display_height() == 96);
  CHECK(decoder.frame_count() == 3 && decoder.has_audio());
  std::vector<u8> strip;
  u32 strip_w = 0, strip_h = 0;
  CHECK(!decoder.staff_strip(false, strip, strip_w, strip_h));
  size_t frames_seen = 0, floats = 0;
  bool left_positive = true, right_negative = true;
  while (decoder.step()) {
    for (const auto& frame : decoder.video()) {
      CHECK(frame.index == frames_seen);
      CHECK(std::abs(frame.pts - static_cast<double>(frames_seen) / 15.0) < 1e-9);
      CHECK(frame.rgba[0] == (frames_seen == 0 ? 138 : frames_seen == 1 ? 118 : 128));
      ++frames_seen;
    }
    for (size_t i = 0; i + 1 < decoder.audio().size(); i += 2) {
      left_positive = left_positive && decoder.audio()[i] > 0.0f;
      right_negative = right_negative && decoder.audio()[i + 1] < 0.0f;
    }
    floats += decoder.audio().size();
    decoder.video().clear();
    decoder.audio().clear();
  }
  floats += decoder.audio().size();
  CHECK(frames_seen == 3);
  CHECK(decoder.eof());
  // Three groups of 2940 samples at 44.1 kHz, resampled to 48 kHz: 9600 stereo frames less the last held back for interpolation.
  CHECK(floats / 2 >= 9598 && floats / 2 <= 9600);
  CHECK(left_positive && right_negative);
}

void test_stream_30fps() {
  // ending_s layout on disc 2: groups L R S V0 V1 V2 S V0 V1 V2, two frames each, strip sectors with rows A / B; an audio-end pair
  // and a stray right channel are ignored.
  Bytes stream;
  const Bytes frame = test_frame(0, 0);
  u32 number = 1;
  for (u32 group = 0; group < 4; ++group) {
    append(stream, audio_sector(0, 1, 0));
    append(stream, audio_sector(1, 1, 0));
    for (int half = 0; half < 2; ++half, ++number) {
      append(stream, strip_sector(1, static_cast<u8>(number), static_cast<u8>(100 + number)));
      for (u16 chunk = 0; chunk < 3; ++chunk) append(stream, video_sector(1, chunk, 3, number, frame, 32, 16));
    }
  }
  append(stream, sector_header(2, 0));
  append(stream, audio_sector(1, 1, 0));
  StrStream index;
  CHECK(index.parse(stream));
  CHECK(index.frames().size() == 8 && index.frame_count() == 8);
  CHECK(std::abs(index.fps() - 30.0) < 1e-9);
  CHECK(index.audio().size() == 4);
  CHECK(index.frames()[1].audio_before == 1 && index.frames()[2].audio_before == 2);
  CHECK(index.strip_rows() == 8);
  const std::vector<u8> row_a = index.strip_image(false), row_b = index.strip_image(true);
  CHECK(row_a.size() == 144u * 8u * 4u);
  CHECK(row_a[0] == 1 && row_a[3] == 255 && row_a[(7 * 144 + 143) * 4 + 2] == 8);
  CHECK(row_b[0] == 101 && row_b[7 * 144 * 4] == 108);

  movie::Decoder decoder;
  CHECK(decoder.open(stream, "movie/ending.stv"));
  CHECK(std::abs(decoder.fps() - 30.0) < 1e-9);
  std::vector<u8> strip;
  u32 strip_w = 0, strip_h = 0;
  CHECK(decoder.staff_strip(true, strip, strip_w, strip_h) && strip_w == 144 && strip_h == 8 && strip == row_b);
}

void test_not_a_stream() {
  StrStream index;
  Bytes noise(StrStream::kSectorBytes * 4, 0xAB);
  CHECK(!index.parse(noise));
  // A frame missing a chunk its bitstream needs is dropped.
  Bytes stream;
  const Bytes frame = test_frame(0, 3000);
  append(stream, video_sector(0, 0, 8, 1, frame, 32, 16));
  CHECK(!index.parse(stream));
  movie::Decoder decoder;
  CHECK(!decoder.open(noise, "x.stv"));
}

}  // namespace

int main() {
  test_mdec();
  test_stream_15fps();
  test_stream_30fps();
  test_not_a_stream();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("str: all passed\n");
  return 0;
}
