#pragma once
// RDE's HD room cards, kept as data: a picture RDE draws in HD art over the screen of one room entrance, from the room's load until
// a fade to black of a given length starts, while it hides some of the room's own background groups. The engine shows whichever
// row matches the room and entrance it loads, so it has no room-specific code.
#include <span>
#include <string_view>

#include "core/types.h"

namespace oc::import {

struct RoomCard {
  int map_id = 0;
  int entrance = 0;
  // hd.dat path of the picture.
  std::string_view image;
  // Placement on the 1280 x 960 canvas, from the picture's left edge.
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  // Background tile groups (bit n = group n) hidden while the card is up.
  u32 hidden_groups = 0;
  // Opacity change per 60 Hz frame while it fades in / out.
  double fade_in = 0, fade_out = 0;
  // A fade to black over exactly this many ticks takes the card down; a later fade back does not bring it again.
  int dismiss_fade = 0;
};

std::span<const RoomCard> room_cards();

// The card of a room entrance, or nullptr.
const RoomCard* room_card(int map_id, int entrance, std::span<const RoomCard> rows = room_cards());

}  // namespace oc::import
