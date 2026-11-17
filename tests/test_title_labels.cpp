// The disc-mode title bar builder (ui::title_blank_bars): every row of a 32-row label cell becomes its most common index, the
// selected half keeps its cursor left of the plain cell's lettering, and bad input gives an empty image.
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

  if (failures) return 1;
  std::printf("title_labels: ok\n");
  return 0;
}
