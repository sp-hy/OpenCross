#pragma once
// RDE HD field backgrounds (.chip2).
// File = u16 key_table[65536] then 64x64 BGR555 tiles, one per original 16x16 tile (4x per axis), tile index >= 16.
#include <optional>
#include <string>
#include <vector>

#include "assets/room_scene.h"
#include "core/types.h"

namespace oc::import {

constexpr int kChip2Scale = 4;
constexpr size_t kChip2TableEntries = 65536;
// 0x2000 bytes per tile.
constexpr size_t kChip2TileBytes = 64 * 64 * 2;
// The table occupies the first 16 tile slots.
constexpr size_t kChip2FirstTile = 16;

struct Chip2 {
  std::vector<u16> table;   // kChip2TableEntries entries; 0 = no HD tile
  std::vector<u16> pixels;  // whole file after the table: (tile_count - 16) tiles of 4096 u16
  u32 tile_count = 0;       // file size / 0x2000 (valid tile indices are 16 .. tile_count-1)

  // Table key from a tile's texture source: ((clut_row * 16) + (v >> 4)) * 128 + (u >> 3). `clut` is the record byte
  // (palette row * 8), `u` the VRAM halfword column, `v` the source row.
  static u32 key(u16 u, u8 v, u8 clut) { return ((static_cast<u32>(clut >> 3) * 16u + (v >> 4)) * 128u) + (u >> 3); }
  // Tile index for a texture source, or 0 when there is no HD tile.
  u32 lookup(u8 clut, u8 v, u16 u) const { return table[key(u, v, clut)]; }
  // 4096 BGR555 texels (row-major 64x64) of tile `index` (16 <= index < tile_count).
  const u16* tile(u32 index) const { return &pixels[(static_cast<size_t>(index) - kChip2FirstTile) * 4096]; }
};

// Validates: size is a multiple of 0x2000 and >= 0x20000, every non-zero table entry is a valid tile index.
std::optional<Chip2> parse_chip2(ByteSpan file, std::string* error = nullptr);

// Composes one (layer, group) of `room` at 4x. Same bbox as the 1x group image; the result has x, y in 1x room
// coordinates (as the 1x image) and width/height multiplied by 4. Tiles with no chip2 entry are filled from the 1x
// image (nearest, 4x). `tiles`/`miss` (optional) receive the tile count and the number that fell back.
// `blend` selects the image kind (RoomGroupImage::blend); the HD tiles decide which texels belong to it, so the kind may hold
// texels that its 1x image lacks. nullopt when the layer/group does not exist.
std::optional<assets::RoomGroupImage> compose_hd_group(const assets::RoomScene& room, u32 layer, u32 group,
                                                       const Chip2& chip2, u32* tiles = nullptr, u32* miss = nullptr,
                                                       int blend = -1);

}  // namespace oc::import
