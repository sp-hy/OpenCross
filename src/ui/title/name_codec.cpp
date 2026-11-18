#include "ui/title/name_codec.h"

#include <algorithm>

namespace oc::ui {

const std::array<std::u32string_view, kLatinRows>& latin_rows() {
  static const std::array<std::u32string_view, kLatinRows> rows = {
      U"ABCDEFGHIJKLMN", U"OPQRSTUVWXYZ  ", U"abcdefghijklmn", U"opqrstuvwxyz  ", U"ÀÁÂÃÄÆÇÈÉÊËÌÍÎ",
      U"ÏÑÒÓÔÕÖŒÙÚÛÜ# ", U"àáâãäæçèéêëìíî", U"ïñòóôõöœùúûüß ", U"0123456789+-*@", U"<>()[]:;,.!?¿¡"};
  return rows;
}

namespace {

// A game-font glyph code: 0xF8xx two-byte codes or one byte. 0x75 is the blank cell.
char32_t codepoint_of(const import::JpGlyphs& glyphs, unsigned code) {
  if (code == 0x75) return 0;
  return code >= 0xF800 ? glyphs.to_unicode(static_cast<u8>(code >> 8), static_cast<u8>(code & 0xFF))
                        : glyphs.to_unicode(static_cast<u8>(code), 0);
}

// Left block (5 columns) of the kana pages: 7 full rows, then ya yu yo / ra row / wa wo n with blanks between.
void fill_kana_left(Ps1Grid& grid, const import::JpGlyphs& glyphs, unsigned first) {
  unsigned code = first;
  for (size_t row = 0; row < 7; ++row)
    for (size_t col = 0; col < 5; ++col) grid[row][col] = codepoint_of(glyphs, code++);
  grid[7][0] = codepoint_of(glyphs, code++);
  grid[7][2] = codepoint_of(glyphs, code++);
  grid[7][4] = codepoint_of(glyphs, code++);
  for (size_t col = 0; col < 5; ++col) grid[8][col] = codepoint_of(glyphs, code++);
  grid[9][0] = codepoint_of(glyphs, code++);
  grid[9][2] = codepoint_of(glyphs, code++);
  grid[9][4] = codepoint_of(glyphs, code++);
}

void fill_punct(Ps1Grid& grid, const import::JpGlyphs& glyphs) {
  for (size_t col = 0; col < 5; ++col) {
    grid[7][5 + col] = codepoint_of(glyphs, 0xC6 + static_cast<unsigned>(col));
    grid[8][5 + col] = codepoint_of(glyphs, 0xCB + static_cast<unsigned>(col));
  }
}

}  // namespace

Ps1Grid ps1_page(Ps1Page page, const import::JpGlyphs& glyphs) {
  Ps1Grid grid{};
  switch (page) {
    case Ps1Page::Katakana: {
      fill_kana_left(grid, glyphs, 0xF81A);
      // Voiced / semi-voiced block, 5 per row.
      unsigned code = 0xF848;
      for (size_t row = 0; row < 6; ++row)
        for (size_t col = 0; col < 5; ++col) grid[row][5 + col] = codepoint_of(glyphs, code++);
      fill_punct(grid, glyphs);
      break;
    }
    case Ps1Page::Hiragana: {
      fill_kana_left(grid, glyphs, 0x76);
      unsigned code = 0xA4;
      for (size_t i = 0; i < 35; ++i) {
        // Codes 0xA4..0xC5, then the comma.
        const unsigned cell_code = i < 34 ? code++ : 0xF5;
        grid[i / 5][5 + i % 5] = codepoint_of(glyphs, cell_code);
      }
      fill_punct(grid, glyphs);
      break;
    }
    case Ps1Page::Alnum: {
      // Full-width A-Z (0xDA..0xF3), a-z (0xF800..0xF819), digits (0xD0..0xD9): 10 letters per row, left 5 | right 5.
      auto put = [&](size_t row, size_t idx, unsigned code) { grid[row][(idx / 5) % 2 * 5 + idx % 5] = codepoint_of(glyphs, code); };
      for (size_t i = 0; i < 26; ++i) put(i / 10, i % 10, 0xDA + static_cast<unsigned>(i));
      for (size_t i = 0; i < 26; ++i) put(3 + i / 10, i % 10, 0xF800 + static_cast<unsigned>(i));
      for (size_t i = 0; i < 10; ++i) put(6, i, 0xD0 + static_cast<unsigned>(i));
      break;
    }
  }
  return grid;
}

GameFontCodec::GameFontCodec(const import::JpGlyphs& glyphs) : glyphs_(&glyphs) {
  for (unsigned b0 = 0x20; b0 < 0xF8; ++b0)
    if (char32_t ch = glyphs.to_unicode(static_cast<u8>(b0), 0)) code_.emplace(ch, static_cast<u16>(b0));
  for (unsigned b0 = 0xF8; b0 <= 0xFE; ++b0)
    for (unsigned b1 = 0; b1 < 256; ++b1)
      if (char32_t ch = glyphs.to_unicode(static_cast<u8>(b0), static_cast<u8>(b1))) code_.emplace(ch, static_cast<u16>(b0 << 8 | b1));
  // The glyph table only holds the characters seen in the JP text; the single-byte Latin block is contiguous
  // (0x28 digits, 0x32 A-Z, 0x4C a-z), so fill the gaps.
  for (unsigned i = 0; i < 26; ++i) {
    code_.emplace(static_cast<char32_t>(U'A' + i), static_cast<u16>(0x32 + i));
    code_.emplace(static_cast<char32_t>(U'a' + i), static_cast<u16>(0x4C + i));
  }
}

std::vector<u8> GameFontCodec::encode(std::u32string_view name, size_t max_symbols) const {
  std::vector<u8> out;
  size_t symbols = 0;
  for (char32_t ch : name) {
    if (symbols >= max_symbols) break;
    auto it = code_.find(ch);
    if (it == code_.end()) continue;
    if (it->second >= 0x100) out.push_back(static_cast<u8>(it->second >> 8));
    out.push_back(static_cast<u8>(it->second & 0xFF));
    ++symbols;
  }
  return out;
}

std::u32string GameFontCodec::decode(ByteSpan bytes) const {
  std::u32string out;
  for (size_t i = 0; i < bytes.size() && bytes[i] != 0; ++i) {
    const u8 b0 = bytes[i];
    u8 b1 = 0;
    if (b0 >= 0xF8) {
      if (i + 1 >= bytes.size()) break;
      b1 = bytes[++i];
    }
    char32_t ch = glyphs_->to_unicode(b0, b1);
    if (!ch && b0 >= 0x32 && b0 <= 0x4B) ch = static_cast<char32_t>(U'A' + (b0 - 0x32));
    if (!ch && b0 >= 0x4C && b0 <= 0x65) ch = static_cast<char32_t>(U'a' + (b0 - 0x4C));
    out.push_back(ch ? ch : U'�');
  }
  return out;
}

std::array<u8, kNameRecord> GameFontCodec::pack_record(std::u32string_view name) const {
  std::array<u8, kNameRecord> rec{};
  const auto bytes = encode(name, 6);
  std::copy_n(bytes.begin(), std::min<size_t>(bytes.size(), kNameRecord - 1), rec.begin());
  return rec;
}

std::u32string trim_blanks(std::u32string_view name) {
  size_t len = name.size();
  while (len > 0 && (name[len - 1] == U' ' || name[len - 1] == U'　')) --len;
  return std::u32string(name.substr(0, len));
}

bool is_reserved_name(std::u32string_view name) {
  if (name == U"クロノ" || name == U"くろの") return true;
  std::u32string lower;
  for (char32_t ch : name) lower.push_back(ch >= U'A' && ch <= U'Z' ? ch + 32 : ch);
  return lower == U"chrono" || lower == U"crono";
}

int max_name_length(int char_id, import::Lang lang, bool hd_keyboard) {
  if (hd_keyboard && lang != import::Lang::Jp) return 12;
  return char_id == 5 ? 4 : 6;
}

std::u32string validate_name(std::u32string_view typed, const import::NameMenuText* disc) {
  std::u32string trimmed = trim_blanks(typed);
  if (trimmed.empty()) return {};
  if (!disc) return is_reserved_name(trimmed) ? std::u32string() : trimmed;
  for (const std::u32string& reserved : disc->reserved)
    if (disc->reserved_prefix ? trimmed.starts_with(reserved) : trimmed == reserved) return {};
  return trimmed;
}

}  // namespace oc::ui
