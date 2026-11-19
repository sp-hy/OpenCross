#pragma once
// Draws an element colour icon (core/element_icon.h) through the sprite batch as flat triangles of a 1 x 1 white texture. Used by every
// drawer of the menus, shops and battle HUD, so the icon looks the same everywhere.
#include <vector>

#include "core/element_icon.h"
#include "render/sprite_batch.h"

namespace oc::render {

// The icon of `colour` (0..5, as data::colour_index_of_bit) filling the square of side `size` centred on (cx, cy) in the batch's current
// virtual space; `pixels` is that size on the screen in device pixels, `light` (0..255) dims it. `white` is any opaque white texture.
inline void draw_element_icon(SpriteBatch& sprites, TexRef white, int colour, float cx, float cy, float size, float pixels, int light = 255) {
  static constexpr float kUv[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
  std::vector<IconTriangle> triangles;
  element_icon(colour, cx, cy, size, pixels, triangles, light);
  for (const IconTriangle& tri : triangles) {
    const float xy[8] = {tri.xy[0], tri.xy[1], tri.xy[2], tri.xy[3], tri.xy[4], tri.xy[5], tri.xy[2], tri.xy[3]};
    sprites.draw_quad(white, xy, kUv, tri.colour);
  }
}

}  // namespace oc::render
