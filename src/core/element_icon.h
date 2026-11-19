#pragma once
// Themed icons for the six element colours, drawn as filled triangles (no image files, so they stay sharp at any resolution and in every
// art mode): green leaf, white star, black crescent moon, red flame, yellow lightning bolt, blue water drop. A caller turns the triangles
// of element_icon() into whatever its draw list takes; the shapes are built once in a unit square and only moved and scaled per call.
#include <array>
#include <vector>

#include "core/types.h"

namespace oc {

// Colour index as data::colour_index_of_bit: 0 green, 1 white, 2 black, 3 red, 4 yellow, 5 blue.
inline constexpr int kElementIconCount = 6;
// Below this on-screen size in device pixels the simplified shapes (no inner highlight or vein, a thicker outline) are used.
inline constexpr float kSimpleIconPixels = 20.0f;

// One filled triangle in the caller's units; `colour` holds bytes R, G, B, A in memory.
struct IconTriangle {
  std::array<float, 6> xy;
  u32 colour;
};

// Whether the colour symbols are drawn where the game shows an element colour ([Enhancements] ElementSymbols). Read by every drawer.
void set_element_symbols(bool on);
bool element_symbols();

// Appends the triangles of colour `colour`'s icon, in drawing order (later over earlier), filling the square of side `size` centred on
// (cx, cy). `pixels` is that size on the screen in device pixels. `light` (0..255) scales the colours for a dimmed row. A colour index
// outside 0..5 appends nothing.
void element_icon(int colour, float cx, float cy, float size, float pixels, std::vector<IconTriangle>& out, int light = 255);

// Splits a simple polygon (no self-crossings, either winding) into triangles of vertex indices by ear clipping.
std::vector<std::array<int, 3>> triangulate_polygon(const std::vector<std::array<float, 2>>& points);

}  // namespace oc
