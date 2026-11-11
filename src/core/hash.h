#pragma once
// 64-bit FNV-1a over a stream of integers (each fed as eight little-endian bytes) and byte strings: the same value on every build and
// platform, for state and data fingerprints.
#include <format>
#include <string>
#include <string_view>

#include "core/types.h"

namespace oc {

class Fnv64 {
 public:
  void add(u64 value) {
    for (int i = 0; i < 8; ++i) byte(static_cast<u8>(value >> (8 * i)));
  }
  void add_bytes(const u8* data, size_t size) {
    add(size);
    for (size_t i = 0; i < size; ++i) byte(data[i]);
  }
  void add_text(std::string_view text) { add_bytes(reinterpret_cast<const u8*>(text.data()), text.size()); }
  u64 value() const { return hash_; }
  std::string hex() const { return std::format("{:016x}", hash_); }

 private:
  void byte(u8 value) {
    hash_ ^= value;
    hash_ *= 0x100000001B3ull;
  }
  u64 hash_ = 0xCBF29CE484222325ull;
};

}  // namespace oc
