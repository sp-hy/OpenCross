#include "import/mdec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace oc::import {

namespace {

constexpr u16 kVersion = 2;
constexpr int kHeaderBytes = 8;

// The MPEG-1 intra AC codes (the DCT coefficient table, "next coefficient" form): the code bits without the trailing sign bit.
struct AcCode {
  const char* bits;
  u8 run, level;
};
constexpr AcCode kAcCodes[] = {
    {"11", 0, 1},
    {"011", 1, 1},
    {"0100", 0, 2},
    {"0101", 2, 1},
    {"00101", 0, 3},
    {"00111", 3, 1},
    {"00110", 4, 1},
    {"000110", 1, 2},
    {"000111", 5, 1},
    {"000101", 6, 1},
    {"000100", 7, 1},
    {"0000110", 0, 4},
    {"0000100", 2, 2},
    {"0000111", 8, 1},
    {"0000101", 9, 1},
    {"00100110", 0, 5},
    {"00100001", 0, 6},
    {"00100101", 1, 3},
    {"00100100", 3, 2},
    {"00100111", 10, 1},
    {"00100011", 11, 1},
    {"00100010", 12, 1},
    {"00100000", 13, 1},
    {"0000001010", 0, 7},
    {"0000001100", 1, 4},
    {"0000001011", 2, 3},
    {"0000001111", 4, 2},
    {"0000001001", 5, 2},
    {"0000001110", 14, 1},
    {"0000001101", 15, 1},
    {"0000001000", 16, 1},
    {"000000011101", 0, 8},
    {"000000011000", 0, 9},
    {"000000010011", 0, 10},
    {"000000010000", 0, 11},
    {"000000011011", 1, 5},
    {"000000010100", 2, 4},
    {"000000011100", 3, 3},
    {"000000010010", 4, 3},
    {"000000011110", 6, 2},
    {"000000010101", 7, 2},
    {"000000010001", 8, 2},
    {"000000011111", 17, 1},
    {"000000011010", 18, 1},
    {"000000011001", 19, 1},
    {"000000010111", 20, 1},
    {"000000010110", 21, 1},
    {"0000000011010", 0, 12},
    {"0000000011001", 0, 13},
    {"0000000011000", 0, 14},
    {"0000000010111", 0, 15},
    {"0000000010110", 1, 6},
    {"0000000010101", 1, 7},
    {"0000000010100", 2, 5},
    {"0000000010011", 3, 4},
    {"0000000010010", 5, 3},
    {"0000000010001", 9, 2},
    {"0000000010000", 10, 2},
    {"0000000011111", 22, 1},
    {"0000000011110", 23, 1},
    {"0000000011101", 24, 1},
    {"0000000011100", 25, 1},
    {"0000000011011", 26, 1},
    {"00000000011111", 0, 16},
    {"00000000011110", 0, 17},
    {"00000000011101", 0, 18},
    {"00000000011100", 0, 19},
    {"00000000011011", 0, 20},
    {"00000000011010", 0, 21},
    {"00000000011001", 0, 22},
    {"00000000011000", 0, 23},
    {"00000000010111", 0, 24},
    {"00000000010110", 0, 25},
    {"00000000010101", 0, 26},
    {"00000000010100", 0, 27},
    {"00000000010011", 0, 28},
    {"00000000010010", 0, 29},
    {"00000000010001", 0, 30},
    {"00000000010000", 0, 31},
    {"000000000011000", 0, 32},
    {"000000000010111", 0, 33},
    {"000000000010110", 0, 34},
    {"000000000010101", 0, 35},
    {"000000000010100", 0, 36},
    {"000000000010011", 0, 37},
    {"000000000010010", 0, 38},
    {"000000000010001", 0, 39},
    {"000000000010000", 0, 40},
    {"000000000011111", 1, 8},
    {"000000000011110", 1, 9},
    {"000000000011101", 1, 10},
    {"000000000011100", 1, 11},
    {"000000000011011", 1, 12},
    {"000000000011010", 1, 13},
    {"000000000011001", 1, 14},
    {"0000000000010011", 1, 15},
    {"0000000000010010", 1, 16},
    {"0000000000010001", 1, 17},
    {"0000000000010000", 1, 18},
    {"0000000000010100", 6, 3},
    {"0000000000011010", 11, 2},
    {"0000000000011001", 12, 2},
    {"0000000000011000", 13, 2},
    {"0000000000010111", 14, 2},
    {"0000000000010110", 15, 2},
    {"0000000000010101", 16, 2},
    {"0000000000011111", 27, 1},
    {"0000000000011110", 28, 1},
    {"0000000000011101", 29, 1},
    {"0000000000011100", 30, 1},
    {"0000000000011011", 31, 1},
};
constexpr const char* kEndOfBlock = "10";
constexpr const char* kEscape = "000001";

// Decoding table indexed by the next 16 bits: code length (0 = invalid), and the run / level or a marker.
struct AcEntry {
  u8 length = 0;
  u8 run = 0;
  u8 level = 0;
  u8 kind = 0;
};
constexpr u8 kKindCode = 0, kKindEnd = 1, kKindEscape = 2;

struct AcTable {
  std::array<AcEntry, 1u << 16> entries{};

  void add(const char* bits, AcEntry entry) {
    u32 code = 0, length = 0;
    for (const char* bit = bits; *bit; ++bit, ++length) code = code << 1 | static_cast<u32>(*bit == '1');
    entry.length = static_cast<u8>(length);
    const u32 free_bits = 16 - length;
    for (u32 suffix = 0; suffix < (1u << free_bits); ++suffix) entries[code << free_bits | suffix] = entry;
  }

  AcTable() {
    for (const AcCode& ac : kAcCodes) add(ac.bits, {0, ac.run, ac.level, kKindCode});
    add(kEndOfBlock, {0, 0, 0, kKindEnd});
    add(kEscape, {0, 0, 0, kKindEscape});
  }
};

const AcTable& ac_table() {
  static const AcTable table;
  return table;
}

constexpr std::array<u8, 64> kZigzag = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                        41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                        30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
// The MPEG-1 default intra quantiser matrix, natural order.
constexpr std::array<u8, 64> kIntraMatrix = {8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
                                             34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
                                             35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
// The 10-bit DC value scales by 2 (its precision), the AC levels by quantiser x matrix / 8.
constexpr int kDcScale = 2;

// basis[u][x] = C(u) / 2 * cos((2x + 1) u pi / 16), C(0) = 1 / sqrt 2: f(x, y) = sum_u sum_v basis[u][x] basis[v][y] F(v, u).
struct IdctBasis {
  float basis[8][8];
  IdctBasis() {
    for (int u = 0; u < 8; ++u) {
      const double scale = (u == 0 ? std::numbers::sqrt2 / 2.0 : 1.0) / 2.0;
      for (int x = 0; x < 8; ++x) basis[u][x] = static_cast<float>(scale * std::cos((2 * x + 1) * u * std::numbers::pi / 16.0));
    }
  }
};

const IdctBasis& idct_basis() {
  static const IdctBasis basis;
  return basis;
}

// Little-endian 16-bit words, bits taken from the most significant end.
class BitReader {
 public:
  explicit BitReader(ByteSpan data) : data_(data) {}

  u32 peek(int count) {
    fill(count);
    return static_cast<u32>(buffer_ >> (bits_ - count)) & ((1u << count) - 1);
  }
  void skip(int count) {
    fill(count);
    bits_ -= count;
  }
  u32 read(int count) {
    const u32 value = peek(count);
    bits_ -= count;
    return value;
  }
  int read_signed(int count) {
    const u32 value = read(count);
    return (value & (1u << (count - 1))) ? static_cast<int>(value) - (1 << count) : static_cast<int>(value);
  }
  // True once the bits consumed run past the end of the data (peeking past it reads zeros).
  bool overrun() const { return pos_ * 8 - static_cast<size_t>(bits_) > data_.size() * 8; }

 private:
  void fill(int count) {
    while (bits_ < count) {
      u32 word = 0;
      if (pos_ + 1 < data_.size()) word = static_cast<u32>(data_[pos_] | data_[pos_ + 1] << 8);
      pos_ += 2;
      buffer_ = buffer_ << 16 | word;
      bits_ += 16;
    }
  }

  ByteSpan data_;
  size_t pos_ = 0;
  u64 buffer_ = 0;
  int bits_ = 0;
};

// One 8 x 8 block to samples centred on 0 (out[y * 8 + x]). False on a bad code or a run past the block.
bool decode_block(BitReader& reader, int quantiser, float* out) {
  int coefficients[64] = {};
  coefficients[0] = reader.read_signed(10) * kDcScale;
  const AcTable& table = ac_table();
  for (int pos = 0;;) {
    const AcEntry& entry = table.entries[reader.peek(16)];
    if (entry.length == 0) return false;
    reader.skip(entry.length);
    if (entry.kind == kKindEnd) break;
    int run, level;
    if (entry.kind == kKindEscape) {
      run = static_cast<int>(reader.read(6));
      level = reader.read_signed(10);
    } else {
      run = entry.run;
      level = reader.read(1) ? -entry.level : entry.level;
    }
    pos += run + 1;
    if (pos > 63) return false;
    const int at = kZigzag[static_cast<size_t>(pos)];
    coefficients[at] = (level * quantiser * kIntraMatrix[static_cast<size_t>(at)] + 4) / 8;
  }
  if (reader.overrun()) return false;
  const IdctBasis& idct = idct_basis();
  float rows[8][8];
  for (int v = 0; v < 8; ++v) {
    for (int x = 0; x < 8; ++x) {
      float sum = 0.0f;
      for (int u = 0; u < 8; ++u) sum += idct.basis[u][x] * static_cast<float>(coefficients[v * 8 + u]);
      rows[v][x] = sum;
    }
  }
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      float sum = 0.0f;
      for (int v = 0; v < 8; ++v) sum += idct.basis[v][y] * rows[v][x];
      out[y * 8 + x] = sum;
    }
  }
  return true;
}

u8 to_byte(float value) { return static_cast<u8>(std::clamp(std::lround(value), 0l, 255l)); }

}  // namespace

bool mdec_decode(ByteSpan frame, u32 width, u32 height, std::vector<u8>& rgba) {
  if (frame.size() < kHeaderBytes || width == 0 || height == 0 || width % 16 || height % 16) return false;
  const int quantiser = frame[4] | frame[5] << 8;
  const int version = frame[6] | frame[7] << 8;
  if (version != kVersion) return false;
  rgba.resize(static_cast<size_t>(width) * height * 4);
  BitReader reader(frame.subspan(kHeaderBytes));
  float cr[64], cb[64], luma[4][64];
  for (u32 mx = 0; mx < width / 16; ++mx) {
    for (u32 my = 0; my < height / 16; ++my) {
      if (!decode_block(reader, quantiser, cr) || !decode_block(reader, quantiser, cb)) return false;
      for (float* block : luma)
        if (!decode_block(reader, quantiser, block)) return false;
      for (u32 y = 0; y < 16; ++y) {
        u8* out = rgba.data() + ((static_cast<size_t>(my) * 16 + y) * width + static_cast<size_t>(mx) * 16) * 4;
        for (u32 x = 0; x < 16; ++x, out += 4) {
          const float lum = luma[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)] + 128.0f;
          const size_t chroma = (y >> 1) * 8 + (x >> 1);
          const float red = cr[chroma], blue = cb[chroma];
          out[0] = to_byte(lum + 1.402f * red);
          out[1] = to_byte(lum - 0.344136f * blue - 0.714136f * red);
          out[2] = to_byte(lum + 1.772f * blue);
          out[3] = 255;
        }
      }
    }
  }
  return true;
}

}  // namespace oc::import
