#include "import/room_cards.h"

namespace oc::import {

namespace {

// The ending room's THE END screen (entrance 2, after the final movie): the "Fin" signature over the PS1's own picture (groups 0, 1),
// taken down by the key press's 64-tick fade to black.
constexpr RoomCard kCards[] = {
    {9, 2, "hd/map/Fin/fin.png", 975.0f, 830.0f, 1130.0f, 930.0f, 0x3u, 0.0125, 0.015625, 64},
};

}  // namespace

std::span<const RoomCard> room_cards() { return kCards; }

const RoomCard* room_card(int map_id, int entrance, std::span<const RoomCard> rows) {
  for (const RoomCard& row : rows)
    if (row.map_id == map_id && row.entrance == entrance) return &row;
  return nullptr;
}

}  // namespace oc::import
