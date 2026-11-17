#pragma once
// The three Square container lumps used on the Chrono Cross disc:
//   cpt  - u32 count, u32 offsets[count] (+ optional EOF offset), untyped subfiles
//   drp  - "drp\0" header, typed/named subfiles
//   tbt  - pointer table of strings (handled by the text importer)
// Both parsers return views into the parent buffer; nothing is copied.
#include <optional>
#include <string>

#include "core/types.h"

namespace oc::import {

struct CptEntry {
  u32 offset = 0;
  u32 size = 0;  // up to the next entry (or the EOF pointer / end of buffer)
  ByteSpan data;
};

struct Cpt {
  std::vector<CptEntry> entries;
  bool has_eof_pointer = false;
};

// A file read from a PS1 disc is up to kFilePadding bytes longer than the file itself: the disc's file table gives sizes rounded
// up to 8 bytes. A block whose own header gives its length and that runs to the end of the buffer (the last entry of a cpt without
// EOF word, the last section of a file) ends at `end` when that is the buffer's `size` or at most kFilePadding bytes before it. A
// shorter buffer is truncated data.
inline constexpr u64 kFilePadding = 7;
constexpr bool ends_at(u64 end, u64 size) { return end <= size && size - end <= kFilePadding; }

// Parses a cpt lump. Returns std::nullopt if the pointer table is inconsistent with the buffer size.
std::optional<Cpt> parse_cpt(ByteSpan data);

struct DrpEntry {
  std::string name;  // 4 chars, zero padded
  u8 type = 0;       // 0x02 mesh, 0x03 timinfo, 0x04 TIM, 0x0A emitter, 0x0C camera, 0x10 sequence, 0x12 ground mesh, 0x15 lens flare, ...
  u32 offset = 0;    // of the 12-byte subfile header
  ByteSpan data;     // payload (length = 16 * header length field)
};

struct Drp {
  std::vector<DrpEntry> entries;
};

bool is_drp(ByteSpan data);
std::optional<Drp> parse_drp(ByteSpan data);

// "It's CDMAKE dummy" placeholder files (first word 0x73277449).
bool is_cdmake_dummy(ByteSpan data);

}  // namespace oc::import
