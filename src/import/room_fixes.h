#pragma once
// RDE's corrections to individual rooms' backgrounds, kept as data and applied by the room importer before the layers are composed,
// so the renderer has no room-specific code: a tile depth remap. Also the tile groups RDE draws as screen-fixed sprites instead of
// their tiles (HD art).
#include <span>
#include <string_view>
#include <vector>

#include "assets/room_scene.h"
#include "core/types.h"

namespace oc::import {

// Every tile of `room` at depth `from` gets depth `to`; the room's last tile is left as it is.
struct RoomBgFix {
  std::string_view room;
  u16 from = 0, to = 0;
};

// Every row, grouped by room.
std::span<const RoomBgFix> room_bg_fixes();

// Applies the rows of `room` to its parsed layers. Returns the number of rows applied.
int apply_room_bg_fixes(std::string_view room, std::vector<assets::RoomLayer>& layers, std::span<const RoomBgFix> rows = room_bg_fixes());

// `groups` tile groups from `first_group` that RDE never draws as tiles: while group first_group + i is visible (BG_GROUP_ON / OFF)
// it draws cell first_cell + i * cell_step of `image` (cells of cell_w x cell_h screen pixels side by side, the image at 4x) with its
// top-left at screen (x + i * x_step, y), fixed to the screen. HD art only; classic art keeps the PS1 tiles.
struct RoomGroupSprite {
  std::string_view room;
  u8 first_group = 0, groups = 0;
  std::string_view image;
  u8 cell_w = 0, cell_h = 0, first_cell = 0, cell_step = 0;
  s16 x = 0, x_step = 0, y = 0;
};

// The rows of `room`.
std::vector<RoomGroupSprite> room_group_sprites(std::string_view room);

}  // namespace oc::import
