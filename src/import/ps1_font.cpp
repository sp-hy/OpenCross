#include "import/ps1_font.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "import/container.h"
#include "import/tim.h"
#include "vfs/vfs.h"

namespace oc::import {

namespace {

// USA glyph table: glyph i is the single-byte code 0x20 + i. 0 = a symbol with no Unicode form
// (usa_char gives U+E000 + the code). Several codes repeat a character; the first one is drawn for it, except the comma, which
// the text writes as 0xDB.
constexpr char32_t kUsaChars[Ps1Font::kUsaGlyphs] = {
    U' ', U'(', U')', U'-', U',', U'+', U'.', U'/', U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7',
    U'8', U'9', U'A', U'B', U'C', U'D', U'E', U'F', U'G', U'H', U'I', U'J', U'K', U'L', U'M', U'N',
    U'O', U'P', U'Q', U'R', U'S', U'T', U'U', U'V', U'W', U'X', U'Y', U'Z', U'a', U'b', U'c', U'd',
    U'e', U'f', U'g', U'h', U'i', U'j', U'k', U'l', U'm', U'n', U'o', U'p', U'q', U'r', U's', U't',
    U'u', U'v', U'w', U'x', U'y', U'z', U'=', U'!', U'"', U'$', U'%', U'&', U'*', U':', U'?', 0x2013,
    U'[', U']', 0xFF61, 0x0027, U'#', 0x3000, 0x2190, 0x2191, 0x2193, 0x2192, 0xFF0B, 0xFF08, 0xFF09, 0xFF0E, 0x201C, 0x201D,
    0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x00C0, 0x00C4, 0x00C7, 0x00C8, 0x00CB, 0x00D2, 0x00D6, 0x00D9, 0x00DC, 0x00E0, 0x00E2, 0x00E1,
    0x00E4, 0x00E7, 0x00E8, 0x00EA, 0x00E9, 0x00EB, 0x00EC, 0x00EE, 0x00ED, 0x00EF, 0x00F9, 0x00FB, 0x00FA, 0x00FC, 0x00F2, 0x00F3,
    0x00F4, 0x00F6, 0x00D1, 0x00F1, 0x266A, 0x266B, 0x00CF, 0x00A1, U'M', U'W', U'm', U'w', 0, 0x2009, 0x2009, 0x2009,
    0x2009, 0x2009, 0x2009, 0x2009, 0x300E, 0x300F, 0x00D7, U'&', 0x2606, 0x25A1, 0x25B3, 0x203B, U'<', U'>', U'~', 0x2026,
    U'?', U'!', 0x30FB, 0x300C, 0x300D, U'#', 0x2212, U'/', 0x3001, 0x2015, 0x2025, 0xFF3F, 0x25CB, 0x2019, U':', U'-',
    U'0', U'1', U'2', U'3', U'4', U'5', U'6', U'7', U'8', U'9', U'%', U',', U';', U'<', U'>'};
constexpr int kUsaComma = 0xDB - 0x20;
// The menu sheet draws the comma code 0xDB with the frame of code 0x24.
constexpr int kUsaCommaFrame = 0x24 - 0x20;

// The USA menu sheet orders its glyphs as the field font up to 0x83 (0x6F is an underscore); from 0x84 on its own order. 0 = no
// frame.
constexpr int kUsaMenuFirstOwn = 0x84 - 0x20;
constexpr char32_t kUsaMenuChars[] = {
    0,      0x00C0, 0x00C4, 0x00C7, 0x00C8, 0x00CB, 0x00D2, 0x00D6, 0x00D9, 0x00DC, 0x00E0, 0x00E1, 0x00E2, 0x00E4, 0x00E7, 0x00E8,
    0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00F2, 0x00F3, 0x00F4, 0x00F6, 0x00D1,
    0x00F1, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x0152, 0x0153, 0x00A1, 0x00BF, 0,      0,      0,      0,      0,      0,      0,
    0,      0,      0x00D7, U'&',   0x2605, 0x25A1, 0x25B3, 0x203B, U'<',   U'>',   U'~',   0x25CB};
constexpr int kUsaMenuUnderscore = 0x6F - 0x20;
// Pen advance of a code with no frame (the blank, the unused codes).
constexpr int kUsaMenuBlank = 4;
// The JP menus draw the 1-bit glyphs in grey 165 over a grey 74 shadow (5-bit levels 20 and 9).
constexpr u8 kJapanMenuFill = 165, kJapanMenuShadow = 74;
// The console font's ink (5-bit levels 20, 20, 21) and its pen advance.
constexpr u32 kConsoleInk = 165 | 165 << 8 | 173 << 16;
constexpr int kConsoleAdvance = 16;

u16 le16(ByteSpan bytes, size_t at) { return at + 2 <= bytes.size() ? static_cast<u16>(bytes[at] | (bytes[at + 1] << 8)) : 0; }

const DrpEntry* drp_entry(const Drp& drp, std::string_view name) {
  for (const DrpEntry& entry : drp.entries)
    if (entry.name == name) return &entry;
  return nullptr;
}

// Second choices for a character the font lacks.
struct Substitute {
  char32_t cp;
  char32_t alt[3];
};
constexpr Substitute kSubstitutes[] = {
    {U'\'', {0x2019, 0xFF07, 0}},  {0x2018, {0x2019, 0xFF07, U'\''}}, {0x2019, {0xFF07, U'\'', 0}},
    {U'"', {0x201C, 0xFF02, 0}},   {0x201C, {U'"', 0xFF02, 0}},       {0x201D, {U'"', 0xFF02, 0}},
    {U',', {0xFF0C, 0x3001, 0}},   {U';', {0xFF1B, 0, 0}},            {U'~', {0xFF5E, 0x301C, 0}},
    {U'_', {0xFF3F, 0, 0}},        {0x2026, {0x22EF, 0x2025, 0}},     {0x2010, {U'-', 0x30FC, 0}},
    {0x2013, {0x30FC, 0xFF0D, U'-'}}, {0x2014, {0x2013, 0x30FC, 0xFF0D}}, {0xA0, {U' ', 0, 0}},
    {0xC1, {0xC0, 0, 0}},          {0xC2, {0xC0, 0, 0}},              {0xC9, {0xC8, 0, 0}},
    {0xCA, {0xC8, 0, 0}},          {0xD3, {0xD2, 0, 0}},              {0xD4, {0xD2, 0, 0}},
    {0xDA, {0xD9, 0, 0}},          {0xDB, {0xD9, 0, 0}},              {0x3002, {0xFF61, 0, 0}},
    {0xFF61, {0x3002, 0, 0}}};

}  // namespace

char32_t Ps1Font::usa_char(int index) {
  if (index < 0 || index >= kUsaGlyphs) return 0;
  return kUsaChars[index] ? kUsaChars[index] : static_cast<char32_t>(0xE020 + index);
}

bool Ps1Font::load_japan(ByteSpan exe, bool menu) {
  if (!jp_.load_bitmaps(exe)) return false;
  build_ = Build::Japan;
  fill_level_ = menu ? kJapanMenuFill : 255;
  shadow_level_ = menu ? kJapanMenuShadow : 0;
  rows_.assign(jp_.rows(0), jp_.rows(0) + static_cast<size_t>(kJpGlyphSlots) * kRows);
  count_ = kJpGlyphSlots;
  cell_w_ = kMaxWidth, cell_h_ = kRows;
  widths_.clear();
  return true;
}

bool Ps1Font::load_usa(ByteSpan exe, ByteSpan widths) {
  if (widths.size() < static_cast<size_t>(kUsaGlyphs) || exe.size() < kUsaOffset + static_cast<size_t>(kUsaGlyphs) * kRows * 2) return false;
  build_ = Build::Usa;
  rows_.resize(static_cast<size_t>(kUsaGlyphs) * kRows);
  for (size_t i = 0; i < rows_.size(); ++i) rows_[i] = static_cast<u16>(exe[kUsaOffset + 2 * i] | (exe[kUsaOffset + 2 * i + 1] << 8));
  count_ = kUsaGlyphs;
  cell_w_ = kMaxWidth, cell_h_ = kRows;
  widths_.assign(widths.begin(), widths.begin() + kUsaGlyphs);
  usa_index_.clear();
  for (int i = 0; i < kUsaGlyphs; ++i) usa_index_.emplace(usa_char(i), i);
  usa_index_[U','] = kUsaComma;
  return true;
}

// The sheet is a type-3 sprite sheet: u16 frame count (bit 15 set) at +2, u16 frame offsets from +0xC; a frame is a u16 part count,
// two padding bytes and 12-byte parts (s16 x, y; u8 u, v; u16 CLUT; u8 w, h; u16 flags). Every glyph frame has one part, its (u, v)
// a texel of the glyph picture.
bool Ps1Font::load_usa_menu(ByteSpan pictures, ByteSpan menutest) {
  const auto art = parse_drp(pictures);
  const auto menu = parse_drp(menutest);
  const DrpEntry* picture_entry = art ? drp_entry(*art, "usfo") : nullptr;
  const DrpEntry* sheet_entry = menu ? drp_entry(*menu, "usfo") : nullptr;
  if (!picture_entry || !sheet_entry) return false;
  const auto tim = parse_tim(picture_entry->data);
  if (!tim || tim->bpp != 4 || !tim->has_clut || tim->clut.data.size() < 16) return false;
  const ByteSpan sheet = sheet_entry->data;
  const int frames = le16(sheet, 2) & 0x7FFF;
  struct Part {
    int x, y, u, v, w, h;
  };
  std::vector<Part> parts(static_cast<size_t>(kUsaGlyphs), Part{0, 0, 0, 0, 0, 0});
  int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
  for (int frame = 1; frame <= frames && frame < kUsaGlyphs; ++frame) {
    const size_t base = le16(sheet, 0xC + 2 * static_cast<size_t>(frame - 1));
    if (le16(sheet, base) < 1 || base + 16 > sheet.size()) continue;
    const size_t at = base + 4;
    Part& part = parts[static_cast<size_t>(frame)];
    part = {static_cast<s16>(le16(sheet, at)), static_cast<s16>(le16(sheet, at + 2)), sheet[at + 4], sheet[at + 5], sheet[at + 8], sheet[at + 9]};
    min_x = std::min(min_x, part.x), min_y = std::min(min_y, part.y);
    max_x = std::max(max_x, part.x + part.w), max_y = std::max(max_y, part.y + part.h);
  }
  build_ = Build::UsaMenu;
  count_ = kUsaGlyphs;
  cell_w_ = max_x - min_x, cell_h_ = max_y - min_y;
  rows_.clear();
  widths_.assign(static_cast<size_t>(kUsaGlyphs), static_cast<u8>(kUsaMenuBlank));
  levels_.assign(static_cast<size_t>(kUsaGlyphs * cell_w_ * cell_h_), 0);
  left_.assign(static_cast<size_t>(kUsaGlyphs), static_cast<s8>(min_x));
  top_.assign(static_cast<size_t>(kUsaGlyphs), static_cast<s8>(min_y));
  const int picture_w = tim->image.w * 4, picture_h = tim->image.h;
  for (int index = 0; index < kUsaGlyphs; ++index) {
    const Part& part = parts[static_cast<size_t>(index == kUsaComma ? kUsaCommaFrame : index)];
    if (part.w == 0) continue;
    widths_[static_cast<size_t>(index)] = static_cast<u8>(part.x + part.w + 1);
    u8* cell = &levels_[static_cast<size_t>(index * cell_w_ * cell_h_)];
    for (int y = 0; y < part.h; ++y)
      for (int x = 0; x < part.w; ++x) {
        const int u = part.u + x, v = part.v + y;
        if (u >= picture_w || v >= picture_h) continue;
        const u16 word = tim->image.data[static_cast<size_t>(v * tim->image.w + u / 4)];
        const int texel = word >> (4 * (u & 3)) & 15;
        if (!texel) continue;
        const u16 colour = tim->clut.data[static_cast<size_t>(texel)];
        const int red = colour & 31;
        cell[(part.y - min_y + y) * cell_w_ + part.x - min_x + x] = static_cast<u8>(std::max(1, red << 3 | red >> 2));
      }
  }
  usa_index_.clear();
  for (int i = 0; i < kUsaGlyphs; ++i) {
    const char32_t cp = i == kUsaMenuUnderscore ? U'_' : i < kUsaMenuFirstOwn ? usa_char(i)
                        : i - kUsaMenuFirstOwn < static_cast<int>(std::size(kUsaMenuChars)) ? kUsaMenuChars[i - kUsaMenuFirstOwn] : 0;
    if (cp && parts[static_cast<size_t>(i)].w) usa_index_.emplace(cp, i);
  }
  usa_index_[U' '] = 0;
  usa_index_[U','] = kUsaComma;
  return true;
}

bool Ps1Font::load_console(std::shared_ptr<const ConsoleFont> font) {
  if (!font) return false;
  build_ = Build::Console;
  console_ = std::move(font);
  count_ = ConsoleFont::cells();
  cell_w_ = ConsoleFont::kCellW, cell_h_ = ConsoleFont::kCellH;
  fill_level_ = kJapanMenuFill, shadow_level_ = kJapanMenuShadow;
  rows_.clear();
  widths_.clear();
  levels_.clear();
  return true;
}

int Ps1Font::left(int index) const {
  return build_ == Build::UsaMenu && index >= 0 && index < count_ ? left_[static_cast<size_t>(index)] : 0;
}

int Ps1Font::top(int index) const {
  return build_ == Build::UsaMenu && index >= 0 && index < count_ ? top_[static_cast<size_t>(index)] : 0;
}

u8 Ps1Font::level(int index, int x, int y) const {
  if (index < 0 || index >= count_ || x < 0 || y < 0 || x >= cell_w_ || y >= cell_h_) return 0;
  if (build_ == Build::UsaMenu) return levels_[static_cast<size_t>((index * cell_h_ + y) * cell_w_ + x)];
  if (build_ == Build::Console) return console_->pixel(index, x, y) ? fill_level_ : 0;
  return rows_[static_cast<size_t>(index) * kRows + static_cast<size_t>(y)] & (0x8000 >> x) ? fill_level_ : 0;
}

u32 Ps1Font::ink() const {
  if (build_ == Build::Console) return kConsoleInk;
  const u32 grey = build_ == Build::UsaMenu ? 255 : fill_level_;
  return grey | grey << 8 | grey << 16;
}

int Ps1Font::line_height() const {
  switch (build_) {
    case Build::Japan:
    case Build::Usa:
      return kLineHeight;
    case Build::UsaMenu:
      return kMenuLineHeight;
    case Build::Console:
      return kConsoleLineHeight;
  }
  return kLineHeight;
}

int Ps1Font::direct(char32_t cp) const {
  if (build_ == Build::Japan) return jp_.index_for(cp);
  if (build_ == Build::Console) return ConsoleFont::cell_of(cp == U' ' ? 0x3000 : cp);
  auto it = usa_index_.find(cp);
  return it == usa_index_.end() ? -1 : it->second;
}

int Ps1Font::index_for(char32_t cp) const {
  if (count_ == 0) return -1;
  if (int index = direct(cp); index >= 0) return index;
  for (const Substitute& sub : kSubstitutes)
    if (sub.cp == cp)
      for (char32_t alt : sub.alt)
        if (alt)
          if (int index = direct(alt); index >= 0) return index;
  if (cp >= 0x21 && cp <= 0x7E) return direct(cp + 0xFEE0);
  if (cp >= 0xFF01 && cp <= 0xFF5E) return direct(cp - 0xFEE0);
  if (cp == 0x3000) return direct(U' ');
  return -1;
}

const u16* Ps1Font::rows(int index) const {
  if (index < 0 || index >= count() || rows_.empty()) return nullptr;
  return &rows_[static_cast<size_t>(index) * kRows];
}

int Ps1Font::advance(int index) const {
  if (build_ == Build::Console) return kConsoleAdvance;
  if (build_ != Build::Japan) return index >= 0 && index < static_cast<int>(widths_.size()) ? widths_[static_cast<size_t>(index)] : 0;
  return JpGlyphs::advance(index);
}

std::shared_ptr<const Ps1Font> load_ps1_menu_font(const vfs::Vfs& vfs) {
  auto pictures = vfs.read_disc("cdrom/menu/menutim.prd");
  auto menutest = vfs.read_disc("cdrom/menu/menutest.prd");
  auto font = std::make_shared<Ps1Font>();
  if (pictures && menutest && font->load_usa_menu(*pictures, *menutest)) return font;
  auto exe = vfs.read_disc("cdrom/prog/slps_023.64");
  auto widths = vfs.read_disc("cdrom/#record/02059");
  const bool japan = exe && !(widths && widths->size() == 256);
  if (japan && font->load_console(console_font())) return font;
  if (japan && font->load_japan(*exe, true)) return font;
  return load_ps1_font(vfs);
}

std::shared_ptr<const Ps1Font> load_ps1_console_font(const vfs::Vfs& vfs) {
  auto font = std::make_shared<Ps1Font>();
  if (font->load_console(console_font())) return font;
  return load_ps1_font(vfs);
}

std::shared_ptr<const Ps1Font> load_ps1_font(const vfs::Vfs& vfs) {
  auto exe = vfs.read_disc("cdrom/prog/slps_023.64");
  if (!exe) return nullptr;
  auto font = std::make_shared<Ps1Font>();
  auto widths = vfs.read_disc("cdrom/#record/02059");
  if (widths && widths->size() == 256 && font->load_usa(*exe, *widths)) return font;
  if (font->load_japan(*exe)) return font;
  return nullptr;
}

}  // namespace oc::import
