#include "import/script_fixes.h"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>

namespace oc::import {

namespace {

// Per room: the code-area offset, the retail bytes and RDE's bytes.
constexpr ScriptFix kFixes[] = {
    {"crn_b02i", 0x130, "CD FD", "AF FD"},
    {"crn_b02i", 0x1A1, "33 02", "51 02"},
    {"dry_a03f", 0xE2F, "E4 18", "E2 18"},
    {"fin_18", 0x26, "28 80 28 80 14 80", "19 80 FF 80 08 80"},
    {"hid_a01f", 0x2F7, "FE 3B 0A 04 FD 80 FE 3B 0C 04 FE 80 FE 3B 0E 04 FF 80", "01 87 52 06 06 06 06 06 06 06 06 06 06 06 06 06 06 06"},
    {"hid_a01f", 0x5287, "",
     "FE 3B 0A 04 FD 80 FE 3B 0C 04 FE 80 FE 3B 0E 04 FF 80 73 61 1A 80 A2 52 01 09 03 "
     "19 0A 04 FE FF 40 19 0C 04 FE FF 40 19 0E 04 FE FF 40 01 09 03"},
    {"kji_b01i", 0x841, "02 26 04 00 00 40 B6 08", "01 5E 16 06 06 06 06 06"},
    {"kji_b01i", 0x165E, "", "FE D1 00 02 26 04 00 00 40 B6 08 01 49 08"},
    {"tel_a09i", 0x6C, "94 05 80 75 01", "01 2A 0E 06 06"},
    {"tel_a09i", 0xE2A, "", "73 A6 15 80 31 0E 00 94 05 80 75 01 01 71 00"},
    {"tel_b12i", 0x585, "F6 FF", "FB FF"},
    {"tel_b12i", 0x5AE, "F8 FF", "FD FF"},
    {"tel_b12i", 0x5D1, "F8 FF", "FB FF"},
    {"wld_b03", 0x407, "0F 02 80", "01 76 13"},
    {"wld_b03", 0x55A, "0F 02 80", "01 7E 13"},
    {"wld_b03", 0x1376, "", "0F 02 80 93 01 01 0A 04 0F 02 80 93 01 01 5D 05"},
    {"you_b10e", 0x49, "28 80 28 80 14 80", "19 80 FF 80 08 80"},
    {"you_b10e", 0x59, "28 80 28 80 14 80", "19 80 FF 80 08 80"},
    {"zel_a13f", 0x58, "FE 05 03 80", "06 06 06 06"},
};

std::optional<Bytes> parse_hex(std::string_view text) {
  Bytes out;
  int high = -1;
  for (const char ch : text) {
    if (ch == ' ') continue;
    int nibble;
    if (ch >= '0' && ch <= '9') {
      nibble = ch - '0';
    } else if (ch >= 'A' && ch <= 'F') {
      nibble = ch - 'A' + 10;
    } else if (ch >= 'a' && ch <= 'f') {
      nibble = ch - 'a' + 10;
    } else {
      return std::nullopt;
    }
    if (high < 0) {
      high = nibble;
    } else {
      out.push_back(static_cast<u8>(high << 4 | nibble));
      high = -1;
    }
  }
  if (high >= 0) return std::nullopt;
  return out;
}

struct Edit {
  size_t at;
  Bytes bytes;
  bool append;
};

}  // namespace

std::span<const ScriptFix> script_fixes() { return kFixes; }

int apply_script_fixes(std::string_view room, Bytes& script, std::span<const ScriptFix> rows, std::string* error) {
  auto fail = [&](std::string message) {
    if (error) *error = std::format("{}: script correction: {}", room, message);
    return -1;
  };
  std::vector<Edit> edits;
  size_t code_base = 0;
  size_t size = script.size();
  for (const ScriptFix& row : rows) {
    if (row.room != room) continue;
    if (edits.empty()) {
      if (script.size() < 0x84) return fail("script too short");
      const u32 entities = static_cast<u32>(script[0x80]) | static_cast<u32>(script[0x81]) << 8 | static_cast<u32>(script[0x82]) << 16 |
                           static_cast<u32>(script[0x83]) << 24;
      code_base = 0x84 + 0x40 * static_cast<size_t>(entities);
      if (code_base > script.size()) return fail("entity table past the end");
    }
    const auto expect = parse_hex(row.expect);
    auto bytes = parse_hex(row.bytes);
    if (!expect || !bytes || bytes->empty()) return fail(std::format("row at {:#x}: bad hex", row.offset));
    const size_t at = code_base + row.offset;
    if (expect->empty()) {
      if (at != size) return fail(std::format("append at {:#x}: the code area ends at {:#x}", row.offset, size - code_base));
      size += bytes->size();
      edits.push_back({at, std::move(*bytes), true});
      continue;
    }
    if (expect->size() != bytes->size()) return fail(std::format("row at {:#x}: expected and new bytes differ in length", row.offset));
    if (at + expect->size() > script.size() || !std::equal(expect->begin(), expect->end(), script.begin() + static_cast<ptrdiff_t>(at)))
      return fail(std::format("row at {:#x}: the bytes there are not {}", row.offset, row.expect));
    edits.push_back({at, std::move(*bytes), false});
  }
  for (const Edit& edit : edits) {
    if (edit.append)
      script.insert(script.end(), edit.bytes.begin(), edit.bytes.end());
    else
      std::copy(edit.bytes.begin(), edit.bytes.end(), script.begin() + static_cast<ptrdiff_t>(edit.at));
  }
  return static_cast<int>(edits.size());
}

}  // namespace oc::import
