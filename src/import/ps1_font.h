#pragma once
// The PS1's own text fonts: the 1-bit glyph table of the main executable that the field message windows draw, and the USA menus'
// anti-aliased glyph sheet.
//
// 1-bit font: a glyph is 11 rows of one u16 little-endian, bit 15 = leftmost pixel, 22 bytes; glyph i belongs to the single-byte code
// 0x20 + i (the Japanese two-byte codes continue the index, JpGlyphs::bitmap_index). Pen advance: the Japanese build uses 8 pixels for
// glyphs up to 0x74 and 12 for the rest; the USA build reads a 256-byte width table (disc record 2059) indexed like the glyphs.
// Lines are 13 pixels apart in both builds; the text is drawn over a dark copy one pixel down and right.
// USA menu sheet: a 4-bit picture of grey-ramp glyphs cut by the frames of a sprite sheet; frame n is code 0x20 + n, placed at its own
// offset from the pen and the line top, advancing by its offset + width + 1 (a code without a frame advances 4). Lines are 18 pixels
// apart; no shadow.
// Console font (import::ConsoleFont, from the player's BIOS): 16 x 15 cells at a 16-pixel advance, ink (165, 165, 173) over a grey 74
// copy one pixel down and right.
//   auto font = import::load_ps1_font(vfs);   // null when the disc's executable is not there
//   int i = font->index_for(U'A');  font->level(i, x, y);  font->advance(i);
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "import/console_font.h"
#include "import/text.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::import {

class Ps1Font {
 public:
  enum class Build { Japan, Usa, UsaMenu, Console };
  static constexpr int kRows = 11;
  static constexpr int kLineHeight = 13;
  static constexpr int kMenuLineHeight = 18;
  static constexpr int kConsoleLineHeight = 16;
  // Widest glyph of either build (pixels); the atlas cell is this wide.
  static constexpr int kMaxWidth = 16;
  // File offsets of the glyph table in each build's main executable.
  static constexpr size_t kJapanOffset = 0x46F0C;
  static constexpr size_t kUsaOffset = 0x46FD0;
  static constexpr int kUsaGlyphs = 191;

  // Japanese build: glyphs from the executable, code points through the JpGlyphs table. `menu`: in the menus' shades.
  bool load_japan(ByteSpan exe, bool menu = false);
  // USA build: glyphs from the executable, pen advance from the width record.
  bool load_usa(ByteSpan exe, ByteSpan widths);
  // USA menus: `pictures` = menu/menutim.prd (TIM "usfo", the glyph picture), `menutest` = menu/menutest.prd (sheet "usfo", whose
  // parts address the picture from its top-left).
  bool load_usa_menu(ByteSpan pictures, ByteSpan menutest);
  // The console font.
  bool load_console(std::shared_ptr<const ConsoleFont> font);

  Build build() const { return build_; }
  int count() const { return count_; }
  // Every glyph picture fits a cell_w() x cell_h() cell, drawn at (left, top) from the pen and the line top.
  int cell_w() const { return cell_w_; }
  int cell_h() const { return cell_h_; }
  int left(int index) const;
  int top(int index) const;
  // Brightness of a glyph pixel: 0 = transparent, else the texel's grey (255 for the 1-bit fonts).
  u8 level(int index, int x, int y) const;
  // Colour of every pixel of a 1-bit glyph, 0x00BBGGRR: the grey of level() in each channel, the console font's own ink; white for
  // the menu sheet, whose pixels have their own greys.
  u32 ink() const;
  int line_height() const;
  // The 1-bit fonts are drawn over a dark copy one pixel down and right, in this grey; the menu sheet is not.
  bool shadow() const { return build_ != Build::UsaMenu; }
  u8 shadow_level() const { return shadow_level_; }
  // Glyph of a code point, -1 when the font has none. Characters the font lacks are tried in a substitute form (full-width <->
  // ASCII, typographic quotes and dashes, accented capitals drawn with the build's generic accent).
  int index_for(char32_t cp) const;
  // Rows of a 1-bit glyph (kRows u16, bit 15 = leftmost pixel); nullptr when out of range or for the menu sheet.
  const u16* rows(int index) const;
  // Pen advance in pixels.
  int advance(int index) const;
  // USA build: code point of glyph `index` (single-byte code 0x20 + index); U+E000 + the code for a
  // symbol with no Unicode form; 0 when out of range.
  static char32_t usa_char(int index);

 private:
  int direct(char32_t cp) const;
  Build build_ = Build::Japan;
  int count_ = 0;
  int cell_w_ = kMaxWidth, cell_h_ = kRows;
  // Grey of a 1-bit glyph pixel and of its shadow.
  u8 fill_level_ = 255, shadow_level_ = 0;
  std::vector<u16> rows_;
  std::vector<u8> widths_;
  // Menu sheet: cell_w_ x cell_h_ levels per glyph and its offset from the pen / line top.
  std::vector<u8> levels_;
  std::vector<s8> left_, top_;
  // USA build: code point -> glyph (the first glyph wins).
  std::unordered_map<char32_t, int> usa_index_;
  JpGlyphs jp_;
  std::shared_ptr<const ConsoleFont> console_;
};

// The font of the PS1 disc the VFS serves: the USA build when disc record 2059 is the 256-byte width table, else the Japanese
// build. Null when the executable is missing or too short.
std::shared_ptr<const Ps1Font> load_ps1_font(const vfs::Vfs& vfs);
// The font of the disc's menus: the USA menu sheet (menu/menutim.prd and menu/menutest.prd), else the 1-bit font of load_ps1_font
// in the JP menus' shades. On the JP disc the console font (console_font()) replaces that 1-bit font when the player has a BIOS.
std::shared_ptr<const Ps1Font> load_ps1_menu_font(const vfs::Vfs& vfs);
// The font of the screens that draw the console font on both builds (name entry): console_font(), else load_ps1_font.
std::shared_ptr<const Ps1Font> load_ps1_console_font(const vfs::Vfs& vfs);

}  // namespace oc::import
