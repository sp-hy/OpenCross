// The disc-mode title bar builder (ui::title_blank_bars): every row of a 32-row label cell becomes its most common index, the
// selected half keeps its cursor left of the plain cell's lettering, and bad input gives an empty image; and the label builder on
// top of it (ui::title_disc_label).
#include <algorithm>
#include <cstdio>

#include "ui/title/title_screen.h"

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

constexpr u32 kW = 16;
constexpr u8 kClear = 15, kBack = 1, kInk = 2, kBar = 14, kCursor = 4;

u8& at(import::Image& img, u32 x, u32 y) { return img.pixels[static_cast<size_t>(y) * img.width + x]; }
u8 at(const import::Image& img, u32 x, u32 y) { return img.pixels[static_cast<size_t>(y) * img.width + x]; }

// Three cells: 0 = plain label, 1 = the same label selected (cursor at columns 2..4), 2 = filler. Rows 0..29 are background with
// lettering at columns 8..12 on rows 5..10; rows 30..31 are the underline, ending at column 13 (columns 14..15 transparent).
import::Image atlas() {
  import::Image img;
  img.width = kW;
  img.height = 96;
  img.indexed = true;
  img.pixels.assign(static_cast<size_t>(img.width) * img.height, kClear);
  for (u32 cell = 0; cell < 3; ++cell) {
    for (u32 row = 0; row < 32; ++row)
      for (u32 x = 0; x < kW; ++x) at(img, x, cell * 32 + row) = row < 30 ? kBack : x < 14 ? kBar : kClear;
    if (cell == 2) continue;
    for (u32 row = 5; row <= 10; ++row)
      for (u32 x = 8; x <= 12; ++x) at(img, x, cell * 32 + row) = kInk;
  }
  for (u32 row = 6; row <= 9; ++row)
    for (u32 x = 2; x <= 4; ++x) at(img, x, 32 + row) = kCursor;
  return img;
}

// A labelled atlas as the discs have it: two 32-wide cells (plain, selected) on the clear index, a first letter at columns 8..12 on
// rows 5..10 with a dark corner, a second one at 14..15, the underline on rows 30..31 up to column 18 (three past the lettering).
constexpr u8 kEdge = 3, kSelInk = 5;
constexpr u32 kWhite = 0xFFFFFFFFu, kOrange = 0xFF4080F0u, kDark = 0xFF282828u, kBarColour = 0xFFC0C0C0u;
import::Image labelled() {
  import::Image img;
  img.width = 32;
  img.height = 64;
  img.indexed = true;
  img.palette.assign(16, 0u);
  img.palette[kInk] = kWhite, img.palette[kSelInk] = kOrange, img.palette[kEdge] = kDark, img.palette[kBar] = kBarColour;
  img.pixels.assign(static_cast<size_t>(img.width) * img.height, kClear);
  for (u32 cell = 0; cell < 2; ++cell) {
    const u8 ink = cell == 0 ? kInk : kSelInk;
    for (u32 row = 5; row <= 10; ++row)
      for (u32 x = 8; x <= 12; ++x) at(img, x, cell * 32 + row) = ink;
    for (u32 row = 7; row <= 10; ++row)
      for (u32 x = 14; x <= 15; ++x) at(img, x, cell * 32 + row) = ink;
    at(img, 8, cell * 32 + 5) = kEdge;
    for (u32 row = 30; row < 32; ++row)
      for (u32 x = 0; x <= 18; ++x) at(img, x, cell * 32 + row) = kBar;
  }
  return img;
}

u32 rgba_at(const import::Image& img, u32 x, u32 y) {
  const u8* px = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
  return static_cast<u32>(px[0]) | static_cast<u32>(px[1]) << 8 | static_cast<u32>(px[2]) << 16 | static_cast<u32>(px[3]) << 24;
}

// The disc label builder (ui::title_disc_label): a 2-wide, 6-row first character fills the art's first letter rows from its column,
// in the cell's most common lettering colour, and the underline is cut three columns after the drawn lettering.
void test_disc_label() {
  ui::TitleInk ink;
  ink.width = 4, ink.height = 6;
  ink.first_top = 0, ink.first_bottom = 5;
  ink.cover.assign(24, 0);
  for (u32 row = 0; row < 6; ++row) ink.cover[row * 4] = ink.cover[row * 4 + 1] = 255;
  const import::Image out = ui::title_disc_label(labelled(), 0, 1, ink);
  CHECK(!out.indexed && out.width == 32 && out.height == 64 && out.pixels.size() == 32u * 64u * 4u);
  if (out.pixels.size() != 32u * 64u * 4u) return;
  CHECK(rgba_at(out, 9, 8) == kWhite);
  CHECK(rgba_at(out, 9, 40) == kOrange);
  CHECK(rgba_at(out, 20, 8) >> 24 == 0);
  CHECK(rgba_at(out, 8, 2) >> 24 == 0);
  int right = -1;
  for (u32 x = 0; x < 32; ++x)
    for (u32 row = 0; row < 30; ++row)
      if (rgba_at(out, x, row) >> 24) right = std::max(right, static_cast<int>(x));
  CHECK(right >= 9 && right <= 12);
  CHECK(rgba_at(out, 0, 30) == kBarColour && rgba_at(out, static_cast<u32>(right + 3), 31) == kBarColour);
  CHECK(rgba_at(out, static_cast<u32>(right + 4), 30) >> 24 == 0);
  CHECK(ui::title_disc_label(labelled(), 0, 1, ui::TitleInk{}).pixels.empty());
}

}  // namespace

int main() {
  const import::Image src = atlas();
  int label_x = -1;
  const import::Image out = ui::title_blank_bars(src, 0, 1, &label_x);
  CHECK(out.indexed && out.width == kW && out.height == 64 && out.pixels.size() == kW * 64);
  CHECK(label_x == 8);
  bool plain_ok = true, selected_ok = true;
  for (u32 row = 0; row < 32; ++row)
    for (u32 x = 0; x < kW; ++x) {
      const u8 want = row < 30 ? kBack : kBar;
      plain_ok = plain_ok && at(out, x, row) == want;
      const bool cursor = x >= 2 && x <= 4 && row >= 6 && row <= 9;
      selected_ok = selected_ok && at(out, x, 32 + row) == (cursor ? kCursor : want);
    }
  CHECK(plain_ok);
  CHECK(selected_ok);

  // A cell without lettering: no column leaves the row's index except the underline's transparent end.
  label_x = -1;
  const import::Image filler = ui::title_blank_bars(src, 2, 2, &label_x);
  CHECK(filler.height == 64 && label_x == 14);

  import::Image rgba = src;
  rgba.indexed = false;
  CHECK(ui::title_blank_bars(rgba, 0, 1).pixels.empty());
  CHECK(ui::title_blank_bars(src, 0, 3).pixels.empty());
  CHECK(ui::title_blank_bars(src, -1, 1).pixels.empty());
  test_disc_label();

  if (failures) return 1;
  std::printf("title_labels: ok\n");
  return 0;
}
