// Checks for the TIM decoder, the VRAM model and the room tile -> VRAM coordinate maths (hand-built data).
#include <cstdio>

#include "import/room.h"
#include "import/tim.h"
#include "game/draw_offset.h"
#include "game/mirror_copy.h"

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

void put16(Bytes& out, u32 value) {
  out.push_back(static_cast<u8>(value));
  out.push_back(static_cast<u8>(value >> 8));
}
void put32(Bytes& out, u32 value) {
  put16(out, value & 0xFFFF);
  put16(out, value >> 16);
}

// TIM with an optional CLUT block (x,y,w,h in halfwords) and an image block.
Bytes make_tim(u32 flags, const std::vector<u16>& clut, u16 cw, u16 ch, u16 ix, u16 iy, u16 iw, u16 ih,
               const std::vector<u16>& img) {
  Bytes tim;
  put32(tim, 0x10);
  put32(tim, flags);
  if (flags & 8) {
    put32(tim, 12 + 2 * static_cast<u32>(clut.size()));
    put16(tim, 256);
    put16(tim, 224);
    put16(tim, cw);
    put16(tim, ch);
    for (u16 entry : clut) put16(tim, entry);
  }
  put32(tim, 12 + 2 * static_cast<u32>(img.size()));
  put16(tim, ix);
  put16(tim, iy);
  put16(tim, iw);
  put16(tim, ih);
  for (u16 texel : img) put16(tim, texel);
  return tim;
}

// BGR555 -> RGBA: 0 is transparent black, STP-only black is opaque, 31 scales to 255 and 16 becomes 16 << 3 | 16 >> 2 = 132.
void test_colour() {
  CHECK(import::bgr555_to_rgba(0) == 0);
  CHECK(import::bgr555_to_rgba(0x8000) == 0xFF000000u);
  CHECK(import::bgr555_to_rgba(31) == (0xFF000000u | 0xFFu));
  CHECK(import::bgr555_to_rgba(31 << 10) == (0xFF000000u | (0xFFu << 16)));
  CHECK(import::bgr555_to_rgba(16 << 5) == (0xFF000000u | (0x84u << 8)));
}

// 4bpp with a 16-entry CLUT (red, green, blue in entries 1..3), 1 halfword wide x 2 high = 4x2 texels; truncated image data
// is refused.
void test_tim4() {
  std::vector<u16> clut(16, 0);
  clut[1] = 31;
  clut[2] = 31 << 5;
  clut[3] = 31 << 10;
  Bytes tim_bytes = make_tim(8, clut, 16, 1, 320, 10, 1, 2, {0x3210, 0x0123});
  auto tim = import::parse_tim(tim_bytes);
  CHECK(tim && tim->bpp == 4 && tim->has_clut);
  auto img = import::decode_tim(*tim);
  CHECK(img && img->indexed && img->width == 4 && img->height == 2);
  if (img) {
    CHECK((img->pixels == std::vector<u8>{0, 1, 2, 3, 3, 2, 1, 0}));
    CHECK(img->palette.size() == 16 && img->palette[0] == 0 && img->palette[1] == 0xFF0000FFu);
    const import::Image rgba = import::to_rgba(*img);
    CHECK(!rgba.indexed && rgba.pixels.size() == 8 * 4);
    CHECK(rgba.pixels[0 * 4 + 3] == 0 && rgba.pixels[1 * 4 + 0] == 255 && rgba.pixels[1 * 4 + 3] == 255);
  }
  CHECK(!import::parse_tim(ByteSpan(tim_bytes).first(tim_bytes.size() - 2)));
}

// 8bpp without a CLUT (background page style): 2 halfwords wide = 4 texels, low byte first; then with a 256-entry CLUT
// (flags 9).
void test_tim8() {
  Bytes tim_bytes = make_tim(1, {}, 0, 0, 64, 256, 2, 1, {0x0201, 0x04FF});
  auto tim = import::parse_tim(tim_bytes);
  CHECK(tim && tim->bpp == 8 && !tim->has_clut && tim->image.x == 64 && tim->image.y == 256);
  auto img = import::decode_tim(*tim);
  CHECK(img && img->width == 4 && img->palette.empty());
  if (img) CHECK((img->pixels == std::vector<u8>{1, 2, 0xFF, 4}));

  std::vector<u16> clut(256, 0);
  clut[7] = 0x7FFF;
  auto tim_clut = import::parse_tim(make_tim(9, clut, 256, 1, 800, 300, 1, 1, {0x0700}));
  CHECK(tim_clut && tim_clut->bpp == 8 && tim_clut->has_clut && tim_clut->clut.w == 256);
  auto img_clut = tim_clut ? import::decode_tim(*tim_clut) : std::nullopt;
  CHECK(img_clut && img_clut->width == 2 && img_clut->pixels[1] == 7 && img_clut->palette[7] == 0xFFFFFFFFu);
}

// 16bpp: two direct-colour texels (transparent, opaque blue); junk and unknown flag bits are refused.
void test_tim16() {
  auto tim = import::parse_tim(make_tim(2, {}, 0, 0, 0, 0, 2, 1, {0x0000, 0x7C00}));
  CHECK(tim && tim->bpp == 16);
  auto img = tim ? import::decode_tim(*tim) : std::nullopt;
  CHECK(img && !img->indexed && img->width == 2 && img->pixels.size() == 8);
  if (img) CHECK(img->pixels[3] == 0 && img->pixels[4 + 2] == 255 && img->pixels[4 + 3] == 255);
  CHECK(!import::parse_tim(Bytes{1, 2, 3}));
  CHECK(!import::parse_tim(make_tim(0x30, {}, 0, 0, 0, 0, 1, 1, {0})));
}

// VRAM model: blit_tim places the image and its CLUT; texel() reads an 8bpp texel as byte (x & 1) of halfword x / 2 and a
// 4bpp texel as nibble (x & 3) of halfword x / 4, an index beyond a short palette reads as 0; blit wraps on both axes; the
// room .bin CLUT block lands at byte offset 0x70000 = row 224, x 0 with a 2048-byte row stride (palette 1 = file bytes
// 512.. = row 225, x 0).
void test_vram() {
  import::Vram vram;
  auto tim = import::parse_tim(make_tim(9, std::vector<u16>(256, 5), 256, 1, 100, 300, 2, 1, {0x2211, 0x4433}));
  CHECK(tim.has_value());
  if (!tim) return;
  vram.blit_tim(*tim);
  CHECK(vram.at(100, 300) == 0x2211 && vram.at(101, 300) == 0x4433);
  CHECK(vram.at(256, 224) == 5 && vram.at(511, 224) == 5 && vram.at(512, 224) == 0);
  CHECK(vram.read_clut(256, 224, 3) == (std::vector<u16>{5, 5, 5}));
  CHECK(vram.texel(200, 300, 8) == 0x11 && vram.texel(201, 300, 8) == 0x22 && vram.texel(203, 300, 8) == 0x44);
  CHECK(vram.texel(400, 300, 4) == 1 && vram.texel(401, 300, 4) == 1 && vram.texel(402, 300, 4) == 2 && vram.texel(407, 300, 4) == 4);
  CHECK(vram.texel(100, 300, 16) == 0x2211);
  const std::vector<u16> short_palette = {0, 9, 8};
  CHECK(vram.texel(200, 300, 8, short_palette) == 0);
  vram.blit(1023, 511, 2, 2, std::vector<u16>{1, 2, 3, 4});
  CHECK(vram.at(1023, 511) == 1 && vram.at(0, 511) == 2 && vram.at(1023, 0) == 3 && vram.at(0, 0) == 4);
  Bytes clut_rows(1024, 0);
  clut_rows[0] = 0x34;
  clut_rows[1] = 0x12;
  clut_rows[512] = 0x78;
  clut_rows[513] = 0x56;
  import::write_room_cluts(vram, clut_rows);
  CHECK(vram.at(0, 224) == 0x1234 && vram.at(0, 225) == 0x5678 && vram.at(256, 224) == 5);
}

// Tile -> VRAM maths: u is a halfword column, v a row within texture space, clut = 8 * palette row; the last tile column /
// row stays inside VRAM; a 16x16 tile reads texels tex_x..tex_x + 15 = halfwords u..u + 7.
void test_tile_maths() {
  const auto origin = import::tile_source(0, 0, 0);
  CHECK(origin.tex_x == 0 && origin.tex_y == 256 && origin.clut_x == 0 && origin.clut_y == 224);
  const auto middle = import::tile_source(448, 240, 24);
  CHECK(middle.tex_x == 896 && middle.tex_y == 496 && middle.clut_y == 227);
  const auto last = import::tile_source(504, 255, 8 * 12);
  CHECK(last.tex_x == 1008 && last.tex_y == 511 && last.clut_y == 236);
  CHECK(last.tex_x + 15 < 2 * import::Vram::kWidth);
  CHECK(import::kClutVramByteOffset / 2048 == static_cast<size_t>(import::kClutVramRow));
  import::Vram vram;
  std::vector<u16> page(64 * 256, 0);
  page[16 * 64 + 8] = 0x0A09;
  vram.blit(0, 256, 64, 256, page);
  const auto source = import::tile_source(8, 16, 0);
  CHECK(vram.texel(source.tex_x, source.tex_y, 8) == 9 && vram.texel(source.tex_x + 1, source.tex_y, 8) == 10);
}

// FE 03 mirror-copy rules: an entity is drawn when state bit 11 is set and bit 26 clear; the axis flag picks the axis;
// reflect() mirrors about the plane half way to the given level; a copy slot behind layer 0 is covered only by layers
// with a smaller z.
void test_mirror_copy() {
  namespace mc = game::mirror_copy;
  CHECK(mc::drawn(0x20800802u) && mc::drawn(0x20800810u) && mc::drawn(0x20800800u));
  CHECK(!mc::drawn(0x20000000u) && !mc::drawn(0x24800800u));
  CHECK(mc::axis_index(4) == 0 && mc::axis_index(8) == 1 && mc::axis_index(0x10) == 2 && mc::axis_index(0) == -1);
  CHECK(mc::reflect(-38, -19) == -19 && mc::reflect(-38, 0) == -38 && mc::reflect(-38, 10) == -48);
  CHECK(mc::slot(400.0f, 3000.0f) == 3400.0f);
}
}  // namespace

// A far layer at 3000 (group 1) and a near one at 2000 (group 0): each layer's offset covers the two slots just nearer than it.
void test_draw_offset() {
  namespace offset = game::draw_offset;
  const offset::LayerDepth pier[] = {{3000, 1}, {2000, 0}};
  CHECK(offset::group_at(pier, 3001) == offset::kGlobal);
  CHECK(offset::group_at(pier, 3000) == offset::kGlobal);
  CHECK(offset::group_at(pier, 2999) == 1);
  CHECK(offset::group_at(pier, 2998) == 1);
  CHECK(offset::group_at(pier, 2997) == offset::kGlobal);
  CHECK(offset::group_at(pier, 2000) == offset::kGlobal);
  CHECK(offset::group_at(pier, 1999) == 0);
  CHECK(offset::group_at(pier, 1997) == offset::kGlobal);
  // Layers of one depth: the first in the list sets its offset last.
  const offset::LayerDepth same[] = {{500, 3}, {500, 2}};
  CHECK(offset::group_at(same, 499) == 3);
  // A layer two slots nearer shares a slot with the far layer's global scroll, which runs after the group offset.
  const offset::LayerDepth close[] = {{502, 3}, {500, 2}};
  CHECK(offset::group_at(close, 499) == offset::kGlobal);
  CHECK(offset::group_at(close, 501) == 3);
  CHECK(offset::group_at({}, 100) == offset::kGlobal);
}

int main() {
  test_colour();
  test_tim4();
  test_tim8();
  test_tim16();
  test_vram();
  test_tile_maths();
  test_mirror_copy();
  test_draw_offset();
  std::printf(failures ? "%d failure(s)\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
