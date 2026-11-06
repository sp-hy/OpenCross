#include "import/room_fixes.h"

#include <algorithm>
#include <vector>

namespace oc::import {

namespace {

constexpr RoomBgFix kFixes[] = {
    {"tel_b12i", 0x13B, 0x141},
};

// The dragon-feeding counters: the held food (groups 4..6), the ones (7..16) and tens (17..26) digit of the fed count.
constexpr RoomGroupSprite kGroupSprites[] = {
    {"jac_b07i", 4, 3, "hd/map/jac_b07i/esa.png", 17, 23, 0, 0, 0x11B, -0x15, 2},
    {"jac_b07i", 7, 10, "hd/map/jac_b07i/number.png", 20, 22, 0, 1, 0x32, 0, 6},
    {"jac_b07i", 17, 10, "hd/map/jac_b07i/number.png", 20, 22, 0, 1, 0x1C, 0, 6},
};

}  // namespace

std::span<const RoomBgFix> room_bg_fixes() { return kFixes; }

std::vector<RoomGroupSprite> room_group_sprites(std::string_view room) {
  std::vector<RoomGroupSprite> rows;
  for (const RoomGroupSprite& row : kGroupSprites)
    if (row.room == room) rows.push_back(row);
  return rows;
}

int apply_room_bg_fixes(std::string_view room, std::vector<assets::RoomLayer>& layers, std::span<const RoomBgFix> rows) {
  int applied = 0;
  u32 last_tile = 0;
  for (const auto& layer : layers)
    for (const auto& tile : layer.tiles) last_tile = std::max(last_tile, tile.index);
  for (const RoomBgFix& row : rows) {
    if (row.room != room) continue;
    ++applied;
    for (auto& layer : layers)
      for (auto& tile : layer.tiles)
        if (tile.z == row.from && tile.index != last_tile) tile.z = row.to;
  }
  return applied;
}

}  // namespace oc::import
