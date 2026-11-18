// Checks for the PS1 font (import::Ps1Font) on synthetic executables: glyph lookup and substitutes in both builds, the Japanese 8 / 12
// pixel advance, the USA width table, the USA menu sheet (glyph pictures, offsets, advances), and a message laid out with the PS1 metrics (pen at the summed advances, 13-pixel lines, the
// disc's own line breaks, no word wrap). The console font (import::ConsoleFont) is found by structure in a fabricated table: its base, the
// rejections, the file search and its use as a Ps1Font.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "import/ps1_font.h"
#include "import/text.h"
#include "ui/typewriter.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// An executable image with a recognisable first row in every glyph: glyph i's row 0 = i, row 10 = 0x8000.
Bytes fake_exe(size_t offset, int glyphs) {
  Bytes exe(offset + static_cast<size_t>(glyphs) * 22, 0);
  for (int i = 0; i < glyphs; ++i) {
    const size_t at = offset + static_cast<size_t>(i) * 22;
    exe[at] = static_cast<u8>(i & 0xFF);
    exe[at + 1] = static_cast<u8>(i >> 8);
    exe[at + 21] = 0x80;
  }
  return exe;
}

void test_japan() {
  import::Ps1Font font;
  CHECK(!font.load_japan(Bytes(16, 0)));
  const Bytes exe = fake_exe(import::Ps1Font::kJapanOffset, import::kJpGlyphSlots);
  CHECK(font.load_japan(exe));
  CHECK(font.build() == import::Ps1Font::Build::Japan);
  CHECK(font.count() == import::kJpGlyphSlots);
  // 'A' is code 0x32 (glyph 18), 'a' 0x4C; a single-byte glyph up to 0x74 advances 8, the rest 12.
  const int a = font.index_for(U'A');
  CHECK(a == 18);
  CHECK(font.rows(a) && font.rows(a)[0] == 18 && font.rows(a)[10] == 0x8000);
  CHECK(font.advance(a) == 8);
  CHECK(font.index_for(U'a') == 0x4C - 0x20);
  const int na = font.index_for(0x30CA);
  CHECK(na == 256 + 14);
  CHECK(font.advance(na) == 12);
  // The JP font has no ASCII comma: the full-width or ideographic one stands in.
  const int comma = font.index_for(U',');
  CHECK(comma >= 0 && (comma == font.index_for(0xFF0C) || comma == font.index_for(0x3001)));
  CHECK(font.rows(-1) == nullptr && font.rows(font.count()) == nullptr);
}

void test_usa() {
  import::Ps1Font font;
  Bytes widths(256, 0);
  for (int i = 0; i < import::Ps1Font::kUsaGlyphs; ++i) widths[static_cast<size_t>(i)] = static_cast<u8>(2 + i % 12);
  const Bytes exe = fake_exe(import::Ps1Font::kUsaOffset, import::Ps1Font::kUsaGlyphs);
  CHECK(!font.load_usa(exe, Bytes(8, 0)));
  CHECK(font.load_usa(exe, widths));
  CHECK(font.build() == import::Ps1Font::Build::Usa);
  CHECK(font.count() == import::Ps1Font::kUsaGlyphs);
  // Same Latin order as the JP single bytes; the rest of the table is the USA build's own.
  CHECK(font.index_for(U'A') == 18 && font.index_for(U'z') == 69);
  CHECK(font.index_for(U'\'') == 83);
  CHECK(font.index_for(0xE9) == 0x94 - 0x20);
  CHECK(font.index_for(0xC8) == 0x87 - 0x20);
  CHECK(font.index_for(0xC8) == font.index_for(0xC9));
  CHECK(font.index_for(0xFF21) == 18);
  CHECK(font.index_for(U'@') < 0);
  CHECK(font.advance(18) == widths[18]);
  CHECK(font.rows(18)[0] == 18);
  CHECK(import::Ps1Font::usa_char(0) == U' ' && import::Ps1Font::usa_char(0xAC - 0x20) == 0xE0AC && import::Ps1Font::usa_char(191) == 0);
  // The text's comma is code 0xDB; a repeated character draws its first code.
  CHECK(font.index_for(U',') == 0xDB - 0x20);
  CHECK(font.index_for(U'!') == 0x67 - 0x20);
  // Every glyph's character finds a glyph of that character.
  for (int i = 0; i < import::Ps1Font::kUsaGlyphs; ++i)
    CHECK(import::Ps1Font::usa_char(font.index_for(import::Ps1Font::usa_char(i))) == import::Ps1Font::usa_char(i));
}

// A drp pack of the given entries (name, type, payload).
Bytes fake_drp(const std::vector<std::tuple<std::string, u8, Bytes>>& entries) {
  Bytes out = {'d', 'r', 'p', 0, 0, 0, 0, 0};
  const u32 count = static_cast<u32>(entries.size()) * 64;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(count >> (8 * i)));
  size_t at = out.size() + 4 * entries.size();
  for (const auto& [name, type, payload] : entries) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(at >> (8 * i)));
    at += 12 + payload.size();
  }
  for (const auto& [name, type, payload] : entries) {
    out.insert(out.end(), 4, 0);
    for (size_t i = 0; i < 4; ++i) out.push_back(i < name.size() ? static_cast<u8>(name[i]) : 0);
    out.push_back(type);
    const u32 length = static_cast<u32>(payload.size()) * 16;
    for (int i = 0; i < 3; ++i) out.push_back(static_cast<u8>(length >> (8 * i)));
    out.insert(out.end(), payload.begin(), payload.end());
  }
  return out;
}

void put16(Bytes& out, u32 value) {
  out.push_back(static_cast<u8>(value));
  out.push_back(static_cast<u8>(value >> 8));
}

// The USA menu sheet: a 16 x 4 texel picture (4bpp, grey ramp CLUT) and a sheet of two frames: '(' (code 0x21) = texels (1, 0)
// 2 x 3 at offset (0, 1), '-' (0x23) = texel (4, 2) 3 x 1. The other codes have no frame.
void test_usa_menu() {
  Bytes tim;
  for (u32 word : {0x10u, 0x08u}) put16(tim, word), put16(tim, word >> 16);
  put16(tim, 12 + 32), put16(tim, 0), put16(tim, 0), put16(tim, 176), put16(tim, 16), put16(tim, 1);
  for (int i = 0; i < 16; ++i) put16(tim, i ? 0x8000u | static_cast<u32>(i * 2) * 0x421u : 0);
  put16(tim, 12 + 32), put16(tim, 0), put16(tim, 0), put16(tim, 0), put16(tim, 4), put16(tim, 4);
  std::vector<u16> texels(16 * 4, 0);
  texels[1] = 15, texels[2] = 8, texels[16 + 1] = 15, texels[32 + 2] = 15, texels[32 + 4] = texels[32 + 5] = texels[32 + 6] = 15;
  for (int row = 0; row < 4; ++row)
    for (int word = 0; word < 4; ++word) {
      u32 value = 0;
      for (int k = 0; k < 4; ++k) value |= static_cast<u32>(texels[static_cast<size_t>(row * 16 + word * 4 + k)]) << (4 * k);
      put16(tim, value);
    }
  Bytes sheet;
  put16(sheet, 0), put16(sheet, 0x8000 | 3);
  sheet.resize(0xC, 0);
  put16(sheet, 0x12), put16(sheet, 0x22), put16(sheet, 0x32);
  auto frame = [&](int x, int y, int u, int v, int w, int h) {
    put16(sheet, w ? 1 : 0), put16(sheet, 0);
    put16(sheet, static_cast<u32>(x)), put16(sheet, static_cast<u32>(y));
    sheet.push_back(static_cast<u8>(u)), sheet.push_back(static_cast<u8>(v));
    put16(sheet, 0), sheet.push_back(static_cast<u8>(w)), sheet.push_back(static_cast<u8>(h)), put16(sheet, 0);
  };
  frame(0, 1, 1, 0, 2, 3);
  frame(0, 0, 0, 0, 0, 0);
  frame(0, 0, 4, 2, 3, 1);
  import::Ps1Font font;
  CHECK(!font.load_usa_menu(fake_drp({{"wind", 4, tim}}), fake_drp({{"usfo", 3, sheet}})));
  CHECK(font.load_usa_menu(fake_drp({{"usfo", 4, tim}}), fake_drp({{"usfo", 3, sheet}})));
  CHECK(font.build() == import::Ps1Font::Build::UsaMenu);
  CHECK(font.line_height() == 18 && !font.shadow() && font.rows(1) == nullptr);
  const int paren = font.index_for(U'(');
  CHECK(paren == 1 && font.advance(paren) == 3);
  CHECK(font.level(paren, 0, 1) == 0xF7 && font.level(paren, 1, 1) > 0 && font.level(paren, 1, 1) < 0xF7 && font.level(paren, 0, 0) == 0);
  CHECK(font.level(paren, 0, 2) == 0xF7 && font.level(paren, 1, 3) == 0xF7);
  CHECK(font.index_for(U'-') == 3 && font.advance(3) == 4 && font.level(3, 2, 0) == 0xF7);
  // No frame: the blank advance; a character with no code is not drawn.
  CHECK(font.index_for(U' ') == 0 && font.advance(0) == 4);
  CHECK(font.index_for(U')') < 0 && font.index_for(U'@') < 0);
}

// A disc message laid out the way the message window lays the PS1 font out: advance = pen step, no grid, no wrap.
// A fabricated console font table at `base` of a 512 KiB image (0xFF before it): every cell but the space carries its own index in
// rows 2-3 and a two-pixel stroke in row 0; the outline shapes are a hollow box and their filled forms the solid box.
Bytes fake_console_image(size_t base) {
  using import::ConsoleFont;
  Bytes image(512 * 1024, 0);
  std::fill(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(base), 0xFF);
  auto cell = [&](int index) { return &image[base + static_cast<size_t>(index) * ConsoleFont::kCellBytes]; };
  for (int index = 1; index < ConsoleFont::cells(); ++index) {
    u8* bytes = cell(index);
    bytes[0] = 0x80, bytes[1] = 0x01;
    bytes[4] = static_cast<u8>(index >> 8), bytes[5] = static_cast<u8>(index);
  }
  for (const auto& [outline, filled] : {std::pair{U'□', U'■'}, {U'○', U'●'}, {U'◇', U'◆'}, {U'△', U'▲'}, {U'▽', U'▼'}, {U'☆', U'★'}}) {
    u8* hollow = cell(ConsoleFont::cell_of(outline));
    u8* solid = cell(ConsoleFont::cell_of(filled));
    std::fill(hollow, hollow + ConsoleFont::kCellBytes, 0);
    std::fill(solid, solid + ConsoleFont::kCellBytes, 0);
    for (int y = 1; y <= 12; ++y) {
      const bool edge = y == 1 || y == 12;
      hollow[2 * y] = edge ? 0x7F : 0x40, hollow[2 * y + 1] = edge ? 0xFE : 0x02;
      solid[2 * y] = 0x7F, solid[2 * y + 1] = 0xFE;
    }
  }
  return image;
}

void test_console() {
  using import::ConsoleFont;
  CHECK(ConsoleFont::cells() == 3489);
  CHECK(ConsoleFont::cell_of(0x3000) == 0);
  CHECK(ConsoleFont::cell_of(U'０') == 147);
  CHECK(ConsoleFont::cell_of(U'ア') == 293);
  CHECK(ConsoleFont::cell_of(U'セ') == 318);
  CHECK(ConsoleFont::cell_of(U'亜') == 524);
  CHECK(ConsoleFont::cell_of(U'腕') == 3488);
  CHECK(ConsoleFont::cell_of(U'～') == ConsoleFont::cell_of(U'〜'));
  CHECK(ConsoleFont::cell_of(U'A') < 0);
  CHECK(ConsoleFont::cell_of(U'丂') < 0);

  constexpr size_t kBase = 0x12345;
  Bytes image = fake_console_image(kBase);
  CHECK(ConsoleFont::find(image) == kBase);
  auto font = ConsoleFont::from_image(image);
  CHECK(font && font->pixel(1, 0, 0) && font->pixel(1, 15, 0) && !font->pixel(1, 1, 0) && !font->pixel(0, 0, 0));
  CHECK(font && font->pixel(ConsoleFont::cell_of(U'■'), 1, 5) && !font->pixel(ConsoleFont::cell_of(U'□'), 3, 5));
  // A header in front moves the table; the base follows it.
  Bytes headered(0x670, 0x11);
  headered.insert(headered.end(), image.begin(), image.end());
  CHECK(ConsoleFont::find(headered) == kBase + 0x670);
  Bytes inked_space = image;
  inked_space[kBase + 6] = 0x10;
  CHECK(!ConsoleFont::find(inked_space));
  Bytes broken_pair = image;
  broken_pair[kBase + static_cast<size_t>(ConsoleFont::cell_of(U'○')) * ConsoleFont::kCellBytes + 28] = 0x80;
  CHECK(!ConsoleFont::find(broken_pair));
  Bytes repeated = image;
  std::copy_n(&image[kBase + 100 * 30], 30, &repeated[kBase + 101 * 30]);
  CHECK(!ConsoleFont::find(repeated));
  CHECK(!ConsoleFont::find(Bytes(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(kBase) + 1000)));
  CHECK(!ConsoleFont::find(Bytes(512 * 1024, 0)));

  // The search reads *.bin / *.rom of a BIOS's size directly in the folders.
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "oc_test_console_font";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "sub");
  auto write = [](const std::filesystem::path& file, const Bytes& bytes) {
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  };
  write(dir / "a.dat", image);
  write(dir / "sub" / "b.bin", image);
  write(dir / "c.BIN", inked_space);
  std::filesystem::path found;
  CHECK(!import::find_console_font({dir}, &found) && found.empty());
  write(dir / "d.Rom", headered);
  CHECK(import::find_console_font({dir}, &found) && found == dir / "d.Rom");
  std::filesystem::remove_all(dir);

  import::Ps1Font ps1;
  CHECK(!ps1.load_console(nullptr));
  CHECK(ps1.load_console(font));
  CHECK(ps1.build() == import::Ps1Font::Build::Console && ps1.cell_w() == 16 && ps1.cell_h() == 15 && ps1.count() == 3489);
  CHECK(ps1.index_for(U'A') == ConsoleFont::cell_of(U'Ａ'));
  CHECK(ps1.index_for(U' ') == 0 && ps1.advance(0) == 16 && ps1.advance(ps1.index_for(U'セ')) == 16);
  CHECK(ps1.level(1, 0, 0) == 165 && ps1.level(1, 1, 0) == 0 && ps1.ink() == (165u | 165u << 8 | 173u << 16));
  CHECK(ps1.shadow() && ps1.shadow_level() == 74);
}

void test_layout() {
  import::Ps1Font font;
  CHECK(font.load_japan(fake_exe(import::Ps1Font::kJapanOffset, import::kJpGlyphSlots)));
  constexpr float kPixel = 4.0f;
  ui::LayoutMetrics metrics;
  metrics.advance = [&](char32_t ch) { return static_cast<float>(font.advance(font.index_for(ch))) * kPixel; };
  metrics.glyph_width = [&](char32_t ch, bool) { return metrics.advance(ch); };
  metrics.line_height = import::Ps1Font::kLineHeight * kPixel;
  import::Message message;
  import::Token text;
  text.kind = import::TokKind::Text;
  text.text = U"AB" + std::u32string(1, 0x30CA) + U" long line that would wrap in the RDE layout but not here";
  message.tokens.push_back(text);
  import::Token newline;
  newline.kind = import::TokKind::Newline;
  message.tokens.push_back(newline);
  text.text = U"C";
  message.tokens.push_back(text);
  const std::vector<ui::TextPage> pages = ui::layout_pages(message, metrics);
  CHECK(pages.size() == 1);
  if (pages.empty()) return;
  const ui::TextPage& page = pages[0];
  CHECK(page.lines == 2);
  CHECK(page.glyphs.size() >= 4);
  if (page.glyphs.size() < 4) return;
  CHECK(page.glyphs[0].x == 0 && page.glyphs[1].x == 8 * kPixel && page.glyphs[2].x == 16 * kPixel && page.glyphs[3].x == 28 * kPixel);
  CHECK(page.glyphs.back().ch == U'C' && page.glyphs.back().x == 0 && page.glyphs.back().y == 13 * kPixel);
}
}  // namespace

int main() {
  test_japan();
  test_usa();
  test_usa_menu();
  test_layout();
  test_console();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ps1_font: ok\n");
  return 0;
}
