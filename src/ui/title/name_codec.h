#pragma once
// Name entry data: the RDE Latin keyboard, the PS1 kana / alphanumeric pages, the game-font byte codec for the 16-byte
// save name record and the name rules (length limits, reserved names).
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "import/name_menu.h"
#include "import/text.h"

namespace oc::ui {

constexpr int kLatinRows = 10, kLatinCols = 14;
constexpr int kPs1Rows = 10, kPs1Cols = 10;
// Size of save::SaveChar::name.
constexpr int kNameRecord = 16;

// RDE grid: 10 rows of 14 cells; a blank cell (' ') inserts U+0020.
const std::array<std::u32string_view, kLatinRows>& latin_rows();

enum class Ps1Page { Katakana = 0, Hiragana = 1, Alnum = 2 };
// PS1 page: 10 rows x 10 columns (two blocks of 5), 0 = blank cell. Needs the glyph table (code -> Unicode).
using Ps1Grid = std::array<std::array<char32_t, kPs1Cols>, kPs1Rows>;
Ps1Grid ps1_page(Ps1Page page, const import::JpGlyphs& glyphs);

// Unicode <-> game-font bytes. A symbol is one byte (< 0xF8) or a prefixed pair (0xF8..0xFE + next byte).
class GameFontCodec {
 public:
  explicit GameFontCodec(const import::JpGlyphs& glyphs);
  bool can_encode(char32_t ch) const { return code_.contains(ch); }
  // Encodes up to `max_symbols` symbols; characters the font lacks are skipped.
  std::vector<u8> encode(std::u32string_view name, size_t max_symbols = 6) const;
  // Stops at NUL / the end; unknown codes become U+FFFD.
  std::u32string decode(ByteSpan bytes) const;
  // 16-byte NUL padded save record of the encodable part of `name` (6 symbols at most).
  std::array<u8, kNameRecord> pack_record(std::u32string_view name) const;
  std::u32string unpack_record(ByteSpan rec) const { return decode(rec.first(std::min<size_t>(rec.size(), kNameRecord))); }

 private:
  // Unicode -> game-font code: b0 or (b0 << 8 | b1).
  std::unordered_map<char32_t, u16> code_;
  const import::JpGlyphs* glyphs_;
};

// Strips trailing U+0020 / U+3000.
std::u32string trim_blanks(std::u32string_view name);
// 「クロノ」 「くろの」 and chrono / crono (case-insensitive); `name` is already trimmed.
bool is_reserved_name(std::u32string_view name);
// Longest name: RDE Latin keyboard 12, PS1 pages 6; Viper (id 5) 4 on the PS1 pages (honorific).
int max_name_length(int char_id, import::Lang lang, bool hd_keyboard);
// Validates a typed name: trims, rejects empty and reserved names. Returns the stored form ("" = rejected). `disc`: played from the PS1
// discs, the disc's own reserved names and rule replace is_reserved_name.
std::u32string validate_name(std::u32string_view typed, const import::NameMenuText* disc = nullptr);

}  // namespace oc::ui
