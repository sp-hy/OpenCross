// Synthetic .chip2 (key table + 2 tiles): lookup, validation, the 4x composition of one tile with a 1x fallback, and a
// semi-transparent tile whose HD texels split differently from its 1x texels.
#include <algorithm>
#include <cstdio>

#include "import/chip2.h"

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

void put16(Bytes& out, size_t at, u16 value) {
  out[at] = static_cast<u8>(value);
  out[at + 1] = static_cast<u8>(value >> 8);
}
}  // namespace

int main() {
  // Key table: tile A at (u 0x28, v 0x30, clut row 3) has key (3 * 16 + 3) * 128 + 5 and maps to tile 16; key 7 maps to
  // tile 17, which no room tile uses. Tile 16 is red with texel 0 clear, texel (6, 5) STP black (opaque) and texel
  // (63, 63) pure blue; tile 17 is all green.
  const u32 key_a = import::Chip2::key(0x28, 0x30, 3 * 8);
  CHECK(key_a == (3u * 16 + 3) * 128 + 5);
  Bytes chip2_bytes(0x20000 + 2 * 0x2000, 0);
  put16(chip2_bytes, 2 * key_a, 16);
  put16(chip2_bytes, 2 * 7, 17);
  for (int i = 1; i < 4096; ++i) put16(chip2_bytes, 0x20000 + 2 * static_cast<size_t>(i), 0x001F);
  put16(chip2_bytes, 0x20000 + 2 * (64 * 5 + 6), 0x8000);
  put16(chip2_bytes, 0x20000 + 2 * (64 * 63 + 63), 0x7C00);
  for (int i = 0; i < 4096; ++i) put16(chip2_bytes, 0x22000 + 2 * static_cast<size_t>(i), 0x03E0);

  // Lookup counts only v >> 4 and u >> 3, so (0x3F, 0x2F) is the same 16x16 block as (0x30, 0x28).
  auto chip = import::parse_chip2(chip2_bytes);
  CHECK(chip && chip->tile_count == 18);
  if (!chip) return 1;
  CHECK(chip->lookup(3 * 8, 0x30, 0x28) == 16);
  CHECK(chip->lookup(3 * 8, 0x3F, 0x2F) == 16);
  CHECK(chip->lookup(3 * 8, 0x40, 0x28) == 0);
  CHECK(chip->lookup(0, 0, 56) == 17);
  Bytes truncated(chip2_bytes.begin(), chip2_bytes.end() - 1);
  CHECK(!import::parse_chip2(truncated));
  Bytes out_of_range = chip2_bytes;
  put16(out_of_range, 2 * 9, 99);
  CHECK(!import::parse_chip2(out_of_range));

  // Room: one layer, one group, two tiles. Tile 0 at (16, 32) has an HD tile; tile 1 at (32, 32) misses and falls back
  // to the 1x group image, which holds one opaque green texel at tile 1's local (2, 1).
  assets::RoomScene room;
  assets::RoomLayer layer;
  layer.index = 0;
  assets::RoomTile tile0;
  tile0.x = 16;
  tile0.y = 32;
  tile0.u = 0x28;
  tile0.v = 0x30;
  tile0.clut = 3 * 8;
  tile0.group = 0;
  assets::RoomTile tile1 = tile0;
  tile1.x = 32;
  tile1.u = 0x100;
  tile1.v = 0;
  tile1.clut = 0;
  layer.tiles = {tile0, tile1};
  assets::RoomGroupImage group;
  group.group = 0;
  group.x = 16;
  group.y = 32;
  group.width = 32;
  group.height = 16;
  group.rgba.assign(32 * 16 * 4, 0);
  {
    u8* green_texel = &group.rgba[(1 * 32 + 16 + 2) * 4];
    green_texel[1] = 200;
    green_texel[3] = 255;
  }
  layer.groups.push_back(group);
  room.layers.push_back(layer);

  // Composition at 4x: index 0 transparent, red 31 -> 255, 0x8000 opaque black, blue at the tile's far corner; the
  // fallback texel (2, 1) of tile 1 becomes the 4x block x 64 + 8..11, y 4..7.
  u32 tiles_seen = 0, tiles_missed = 0;
  auto hd = import::compose_hd_group(room, 0, 0, *chip, &tiles_seen, &tiles_missed);
  CHECK(hd.has_value());
  if (!hd) return 1;
  CHECK(tiles_seen == 2 && tiles_missed == 1);
  CHECK(hd->width == 128 && hd->height == 64 && hd->x == 16 && hd->y == 32);
  auto px = [&](size_t x, size_t y) { return &hd->rgba[(y * 128 + x) * 4]; };
  CHECK(px(0, 0)[3] == 0);
  CHECK(px(1, 0)[0] == 255 && px(1, 0)[1] == 0 && px(1, 0)[3] == 255);
  CHECK(px(6, 5)[0] == 0 && px(6, 5)[3] == 255);
  CHECK(px(63, 63)[2] == 255 && px(63, 63)[0] == 0 && px(63, 63)[3] == 255);
  CHECK(px(64 + 8, 4)[1] == 200 && px(64 + 11, 7)[1] == 200 && px(64 + 8, 3)[3] == 0 && px(64 + 12, 4)[3] == 0);
  CHECK(!import::compose_hd_group(room, 0, 5, *chip));

  // A semi-transparent (ABR 1) tile whose 1x texels all carry STP, drawn with HD tile 17 whose texels carry none: the HD texels
  // belong to the opaque kind, which the 1x art leaves empty, and the ABR 1 kind gets none of them.
  assets::RoomScene semi_room;
  assets::RoomLayer semi_layer;
  assets::RoomTile semi_tile;
  semi_tile.x = 0;
  semi_tile.y = 0;
  semi_tile.u = 56;
  semi_tile.flags = 0x80 | (1 << 3);
  semi_layer.tiles = {semi_tile};
  assets::RoomGroupImage additive;
  additive.blend = 1;
  additive.width = 16;
  additive.height = 16;
  additive.rgba.assign(16 * 16 * 4, 0);
  assets::RoomGroupImage opaque = additive;
  opaque.blend = -1;
  semi_layer.groups = {opaque, additive};
  semi_room.layers.push_back(semi_layer);
  auto semi_opaque = import::compose_hd_group(semi_room, 0, 0, *chip, nullptr, nullptr, -1);
  auto semi_additive = import::compose_hd_group(semi_room, 0, 0, *chip, nullptr, nullptr, 1);
  CHECK(semi_opaque && semi_additive);
  if (!semi_opaque || !semi_additive) return 1;
  CHECK(semi_opaque->width == 64 && semi_opaque->blend == -1);
  CHECK(semi_opaque->rgba[1] == 255 && semi_opaque->rgba[3] == 255);
  CHECK(semi_opaque->rgba[(64 * 63 + 63) * 4 + 3] == 255);
  CHECK(std::ranges::none_of(semi_additive->rgba, [](u8 value) { return value != 0; }));
  if (!failures) std::printf("test_chip2: ok\n");
  return failures ? 1 : 0;
}
