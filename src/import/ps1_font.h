#pragma once
// The PS1's own text font: the 1-bit glyph table of the main executable that the field message windows draw.
//
// A glyph is 11 rows of one u16 little-endian, bit 15 = leftmost pixel, 22 bytes; glyph i belongs to the single-byte code 0x20 + i
// (the Japanese two-byte codes continue the index, JpGlyphs::bitmap_index). Pen advance: the Japanese build uses 8 pixels for
// glyphs up to 0x74 and 12 for the rest; the USA build reads a 256-byte width table (disc record 2059) indexed like the glyphs.
// Lines are 13 pixels apart in both builds.
//   auto font = import::load_ps1_font(vfs);   // null when the disc's executable is not there
//   int i = font->index_for(U'A');  font->rows(i);  font->advance(i);
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "import/text.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::import {

class Ps1Font {
 public:
  enum class Build { Japan, Usa };
  static constexpr int kRows = 11;
  static constexpr int kLineHeight = 13;
  // Widest glyph of either build (pixels); the atlas cell is this wide.
  static constexpr int kMaxWidth = 16;
  // File offsets of the glyph table in each build's main executable.
  static constexpr size_t kJapanOffset = 0x46F0C;
  static constexpr size_t kUsaOffset = 0x46FD0;
  static constexpr int kUsaGlyphs = 191;

  // Japanese build: glyphs from the executable, code points through the JpGlyphs table.
  bool load_japan(ByteSpan exe);
  // USA build: glyphs from the executable, pen advance from the width record.
  bool load_usa(ByteSpan exe, ByteSpan widths);

  Build build() const { return build_; }
  int count() const { return static_cast<int>(rows_.size() / kRows); }
  // Glyph of a code point, -1 when the font has none. Characters the font lacks are tried in a substitute form (full-width <->
  // ASCII, typographic quotes and dashes, accented capitals drawn with the build's generic accent).
  int index_for(char32_t cp) const;
  // Rows of a glyph (kRows u16, bit 15 = leftmost pixel); nullptr when out of range.
  const u16* rows(int index) const;
  // Pen advance in pixels.
  int advance(int index) const;
  // USA build: code point of glyph `index` (single-byte code 0x20 + index); U+E000 + the code for a
  // symbol with no Unicode form; 0 when out of range.
  static char32_t usa_char(int index);

 private:
  int direct(char32_t cp) const;
  Build build_ = Build::Japan;
  std::vector<u16> rows_;
  std::vector<u8> widths_;
  // USA build: code point -> glyph (the first glyph wins).
  std::unordered_map<char32_t, int> usa_index_;
  JpGlyphs jp_;
};

// The font of the PS1 disc the VFS serves: the USA build when disc record 2059 is the 256-byte width table, else the Japanese
// build. Null when the executable is missing or too short.
std::shared_ptr<const Ps1Font> load_ps1_font(const vfs::Vfs& vfs);

}  // namespace oc::import
