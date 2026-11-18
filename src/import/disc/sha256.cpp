#include "import/disc/sha256.h"

#include <array>
#include <format>

namespace oc::import::disc {

namespace {

constexpr std::array<u32, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr u32 rotr(u32 value, int bits) { return value >> bits | value << (32 - bits); }

void compress(std::array<u32, 8>& state, const u8* block) {
  std::array<u32, 64> words{};
  for (int i = 0; i < 16; ++i)
    words[i] = static_cast<u32>(block[4 * i]) << 24 | static_cast<u32>(block[4 * i + 1]) << 16 | static_cast<u32>(block[4 * i + 2]) << 8 |
               block[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const u32 low = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
    const u32 high = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
    words[i] = words[i - 16] + low + words[i - 7] + high;
  }
  std::array<u32, 8> work = state;
  for (int i = 0; i < 64; ++i) {
    const u32 sum1 = rotr(work[4], 6) ^ rotr(work[4], 11) ^ rotr(work[4], 25);
    const u32 choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
    const u32 temp1 = work[7] + sum1 + choose + kRound[i] + words[i];
    const u32 sum0 = rotr(work[0], 2) ^ rotr(work[0], 13) ^ rotr(work[0], 22);
    const u32 majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
    for (int j = 7; j > 0; --j) work[j] = work[j - 1];
    work[4] += temp1;
    work[0] = temp1 + sum0 + majority;
  }
  for (int i = 0; i < 8; ++i) state[i] += work[i];
}

}  // namespace

std::string sha256_hex(ByteSpan bytes) {
  std::array<u32, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t at = 0;
  for (; at + 64 <= bytes.size(); at += 64) compress(state, bytes.data() + at);
  u8 tail[128] = {};
  const size_t rest = bytes.size() - at;
  for (size_t i = 0; i < rest; ++i) tail[i] = bytes[at + i];
  tail[rest] = 0x80;
  const size_t tail_size = rest < 56 ? 64 : 128;
  const u64 bits = static_cast<u64>(bytes.size()) * 8;
  for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = static_cast<u8>(bits >> (8 * i));
  for (size_t offset = 0; offset < tail_size; offset += 64) compress(state, tail + offset);
  std::string out;
  for (const u32 word : state) out += std::format("{:08x}", word);
  return out;
}

}  // namespace oc::import::disc
