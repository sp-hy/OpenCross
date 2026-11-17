#include "import/ps1_font.h"

#include <algorithm>

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

bool Ps1Font::load_japan(ByteSpan exe) {
  if (!jp_.load_bitmaps(exe)) return false;
  build_ = Build::Japan;
  rows_.assign(jp_.rows(0), jp_.rows(0) + static_cast<size_t>(kJpGlyphSlots) * kRows);
  widths_.clear();
  return true;
}

bool Ps1Font::load_usa(ByteSpan exe, ByteSpan widths) {
  if (widths.size() < static_cast<size_t>(kUsaGlyphs) || exe.size() < kUsaOffset + static_cast<size_t>(kUsaGlyphs) * kRows * 2) return false;
  build_ = Build::Usa;
  rows_.resize(static_cast<size_t>(kUsaGlyphs) * kRows);
  for (size_t i = 0; i < rows_.size(); ++i) rows_[i] = static_cast<u16>(exe[kUsaOffset + 2 * i] | (exe[kUsaOffset + 2 * i + 1] << 8));
  widths_.assign(widths.begin(), widths.begin() + kUsaGlyphs);
  usa_index_.clear();
  for (int i = 0; i < kUsaGlyphs; ++i) usa_index_.emplace(usa_char(i), i);
  usa_index_[U','] = kUsaComma;
  return true;
}

int Ps1Font::direct(char32_t cp) const {
  if (build_ == Build::Japan) return jp_.index_for(cp);
  auto it = usa_index_.find(cp);
  return it == usa_index_.end() ? -1 : it->second;
}

int Ps1Font::index_for(char32_t cp) const {
  if (rows_.empty()) return -1;
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
  if (index < 0 || index >= count()) return nullptr;
  return &rows_[static_cast<size_t>(index) * kRows];
}

int Ps1Font::advance(int index) const {
  if (build_ == Build::Usa) return index >= 0 && index < static_cast<int>(widths_.size()) ? widths_[static_cast<size_t>(index)] : 0;
  return JpGlyphs::advance(index);
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
