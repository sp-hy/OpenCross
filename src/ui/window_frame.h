// Skinned window frame of the Settings "Custom Window Frame" preview.
// Pure geometry: the quads (canvas units, 1280 x 960) of one window of a skin sheet wNN.png / wN.tim - the 7 x 4 fill grid, the corner
// pieces, the top / bottom / left / right edge strips and the page leaf - so the menu can draw them through its own sprite path and a
// unit test can check the layout. Same piece model as the message window (DialogueBox::draw_tiles) and the key-item list panel.
// build_skin_rect lays a skin's pieces around any rectangle the PS1's way (the party screens' windows).
#pragma once

#include <vector>

namespace oc::ui {

struct WinQuad {
  // Position and size in canvas units, then the normalised sheet uv.
  float x = 0, y = 0, w = 0, h = 0;
  float u0 = 0, v0 = 0, du = 0, dv = 0;
  // A tile of the fill halves (build_skin_rect), which the PS1 draws semi-transparent.
  bool fill = false;
};

// quads are in draw order: fill grid, corners + top / bottom edges, left edge, right corners + right edge.
// leaf is the page leaf (close triangle), bottom-right; its u0 depends on the phase given to build().
// left / top / right / bottom bound the fill grid (canvas).
struct SkinWindow {
  std::vector<WinQuad> quads;
  WinQuad leaf;
  float left = 0, top = 0, right = 0, bottom = 0;
};

// `skin` = texture index 0..15 (w01..w15, w17; byte 0 -> 3, 1..3 -> 0..2, 4..14 -> same). `leaf_phase` 0..4 (u0 .5 + .125 min(phase, 3)).
SkinWindow build_skin_window(int skin, int leaf_phase);

// Rest x of the grid's first column per skin.
float skin_window_x(int skin);

// A window of a PS1 skin sheet (wN.tim, 128 x 96 texels) around any rectangle, laid out as the PS1's window builder lays one: ten pieces
// (two fill halves, the four corners, the right, left, bottom and top edges) about the centre (cx, cy), `w` x `h` texels, every texel
// `x_scale` screen pixels wide and one high (the 640-wide screens draw the sheet twice as wide). Edge and fill pieces repeat their
// tile of the sheet, so such a piece comes back as one quad per run of whole tiles. A texel drawn two pixels wide starts one pixel early
// (the PS1 rounds a pixel's texture coordinate to the nearest texel), so the quads hold half texels at a piece's ends. Quads in screen
// pixels, uv normalised to the sheet, in draw order. The right corners and the right edge start one texel left on the 640-wide screens
// (`shift_right`); the field's message windows, one texel per pixel, start them on their own texel.
std::vector<WinQuad> build_skin_rect(float cx, float cy, int w, int h, float x_scale, bool shift_right = true);

}  // namespace oc::ui
