#include "import/chip2.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace oc::import {

namespace {
// BGR555 -> 0xAABBGGRR with alpha 255, the rounding of bgr555_to_rgba.
u32 expand555(u16 pixel) {
  u32 r = (pixel & 31u) << 3, g = ((pixel >> 5) & 31u) << 3, b = ((pixel >> 10) & 31u) << 3;
  r |= r >> 5;
  g |= g >> 5;
  b |= b >> 5;
  return r | (g << 8) | (b << 16) | 0xFF000000u;
}
// BGR555 -> RGBA lookup (0 stays transparent), built once; the tile loop below is the hot path of an HD room load.
const std::array<u32, 65536>& color_lut() {
  static const std::array<u32, 65536> lut = [] {
    std::array<u32, 65536> table{};
    for (size_t pixel = 1; pixel < table.size(); ++pixel) table[pixel] = expand555(static_cast<u16>(pixel));
    return table;
  }();
  return lut;
}
}  // namespace

std::optional<Chip2> parse_chip2(ByteSpan file, std::string* error) {
  auto fail = [&](std::string message) -> std::optional<Chip2> {
    if (error) *error = std::move(message);
    return std::nullopt;
  };
  constexpr size_t kTableBytes = kChip2TableEntries * 2;
  if (file.size() < kTableBytes) return fail("chip2: smaller than the key table");
  if (file.size() % kChip2TileBytes != 0) return fail("chip2: size is not a multiple of 0x2000");
  Chip2 chip2;
  chip2.tile_count = static_cast<u32>(file.size() / kChip2TileBytes);
  chip2.table.resize(kChip2TableEntries);
  for (size_t i = 0; i < kChip2TableEntries; ++i) chip2.table[i] = static_cast<u16>(file[2 * i] | (file[2 * i + 1] << 8));
  for (size_t i = 0; i < kChip2TableEntries; ++i)
    if (chip2.table[i] != 0 && (chip2.table[i] < kChip2FirstTile || chip2.table[i] >= chip2.tile_count))
      return fail("chip2: table entry " + std::to_string(i) + " = " + std::to_string(chip2.table[i]) + " outside tiles [16, " +
                  std::to_string(chip2.tile_count) + ")");
  const size_t pixel_count = (file.size() - kTableBytes) / 2;
  chip2.pixels.resize(pixel_count);
  for (size_t i = 0; i < pixel_count; ++i)
    chip2.pixels[i] = static_cast<u16>(file[kTableBytes + 2 * i] | (file[kTableBytes + 2 * i + 1] << 8));
  return chip2;
}

// Texel kinds: in the opaque group (blend -1) a texel of 0 is transparent and anything else (0x8000 included) is
// opaque; a semi-transparent tile contributes its STP texels to the image of its ABR and its non-STP non-zero texels to the opaque
// image. A texel of another kind leaves what an earlier tile wrote (RDE keeps the kinds in separate textures). A tile with no HD
// entry is filled by a nearest 4x upscale of the already composed 1x tile area.
std::optional<assets::RoomGroupImage> compose_hd_group(const assets::RoomScene& room, u32 layer, u32 group,
                                                       const Chip2& chip2, u32* tiles_out, u32* miss_out, int blend) {
  if (layer >= room.layers.size()) return std::nullopt;
  const assets::RoomLayer& room_layer = room.layers[layer];
  const assets::RoomGroupImage* area = nullptr;
  const assets::RoomGroupImage* lowres = nullptr;
  for (const auto& image : room_layer.groups) {
    if (image.group != group) continue;
    area = &image;
    if (image.blend == blend) lowres = &image;
  }
  if (!area) return std::nullopt;
  assets::RoomGroupImage out;
  out.blend = blend;
  out.group = area->group;
  out.x = area->x;
  out.y = area->y;
  out.z = area->z;
  out.width = area->width * kChip2Scale;
  out.height = area->height * kChip2Scale;
  out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
  u32 count = 0, miss = 0;
  for (const auto& tile : room_layer.tiles) {
    if (tile.group != group) continue;
    const bool semi = (tile.flags & 0x80) != 0;
    if (blend >= 0 && !(semi && ((tile.flags >> 3) & 3) == blend)) continue;
    ++count;
    const size_t ox = static_cast<size_t>(tile.x - lowres->x) * kChip2Scale, oy = static_cast<size_t>(tile.y - lowres->y) * kChip2Scale;
    const u32 tile_index = chip2.lookup(tile.clut, tile.v, tile.u);
    if (tile_index != 0) {
      const u16* tile_pixels = chip2.tile(tile_index);
      const auto& lut = color_lut();
      for (int j = 0; j < 64; ++j) {
        u8* dst = &out.rgba[((oy + static_cast<size_t>(j)) * out.width + ox) * 4];
        for (int i = 0; i < 64; ++i) {
          const u16 pixel = tile_pixels[j * 64 + i];
          if (pixel == 0) continue;
          if (semi) {
            if (((pixel & 0x8000) != 0) != (blend >= 0)) continue;
          } else if (blend >= 0) {
            continue;
          }
          const u32 rgba = lut[pixel];
          std::memcpy(dst + 4 * i, &rgba, 4);
        }
      }
    } else {
      ++miss;
      if (!lowres) continue;
      const size_t sx = static_cast<size_t>(tile.x - lowres->x), sy = static_cast<size_t>(tile.y - lowres->y);
      for (int j = 0; j < 16 * kChip2Scale; ++j)
        for (int i = 0; i < 16 * kChip2Scale; ++i) {
          const size_t src = ((sy + static_cast<size_t>(j / kChip2Scale)) * lowres->width + sx + static_cast<size_t>(i / kChip2Scale)) * 4;
          if (!lowres->rgba[src + 3]) continue;
          const size_t dst = (oy + static_cast<size_t>(j)) * out.width + ox + static_cast<size_t>(i);
          std::copy_n(&lowres->rgba[src], 4, &out.rgba[dst * 4]);
        }
    }
  }
  if (tiles_out) *tiles_out = count;
  if (miss_out) *miss_out = miss;
  return out;
}

}  // namespace oc::import
