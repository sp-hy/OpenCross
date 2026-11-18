#pragma once
// The layout table of the party screens' overlays (prog/memmenu.prg, the Current Party screen; prog/memmenu2.prg, the Relief Charm
// screen): the sprites they cut from map/etc/fmenu.tim (the three captions, the stat labels, the innate element dot, the two help lines),
// the three quads of the shown member's name and the innate element colours. Both builds carry the same table at different offsets
// (the USA one with wider help lines and one name quad per glyph); it is found by its content.
//   if (auto layout = import::find_party_menu_layout(prg)) draw(layout->labels[0]);   // "Status"
#include <array>
#include <optional>

#include "core/types.h"

namespace oc::import {

// One textured rectangle: screen (x, y, w, h) on the 640-wide screen, texels (u, v, du, dv), the sprite colour (0x80 = 1.0 per channel).
struct PartyMenuSprite {
  int x = 0, y = 0, w = 0, h = 0;
  int u = 0, v = 0, du = 0, dv = 0;
  u32 colour = 0;
};

struct PartyMenuLayout {
  // 0 "Status", 1 "Current Party", 2 "Member", 3 the stat labels, 4 the innate element dot, 5 the help line of the member grid,
  // 6 the help line of the party slots.
  std::array<PartyMenuSprite, 7> labels{};
  // The name: drawn last to first, the last one darker and further right (a shadow), the first two one pixel apart.
  std::array<PartyMenuSprite, 3> name{};
  // Innate element colours (r, g, b; 0x80 = 1.0), one per innate bit of the character record (the lowest set bit picks).
  std::array<std::array<u8, 3>, 6> element_rgb{};
};

std::optional<PartyMenuLayout> find_party_menu_layout(ByteSpan prg);

}  // namespace oc::import
