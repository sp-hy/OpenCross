// Checks for the PS1 font (import::Ps1Font) on synthetic executables: glyph lookup and substitutes in both builds, the Japanese 8 / 12
// pixel advance, the USA width table, and a message laid out with the PS1 metrics (pen at the summed advances, 13-pixel lines, the
// disc's own line breaks, no word wrap).
#include <cstdio>
#include <string>

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

// A disc message laid out the way the message window lays the PS1 font out: advance = pen step, no grid, no wrap.
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
  test_layout();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ps1_font: ok\n");
  return 0;
}
