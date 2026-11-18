// Settings window-skin preview geometry (canvas 1280 x 960); the per-skin uv numbers are shared with the key-item list panel
// (keyitem_popup.cpp ListSkin) because both are filled from the same piece records.
#include "ui/window_frame.h"

#include <algorithm>
#include <cmath>

namespace oc::ui {
namespace {

// Bottom edge dv / v0, B0 v0, R24 v0; left edge; right edge middle pieces.
struct SkinUv {
  float bdv, bv, b0v, r24v;
  float lu, ldu, ldv;
  float cu, cdu, cv, cdv;
};
constexpr SkinUv kUv[16] = {
    {.165f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .1225f, .17067f, .160f},
    {.165f, .1665f, .4077f, .4077f, .126f, .125f, .161f, .1275f, .125f, .16667f, .160f},
    {.170f, .1580f, .4180f, .4210f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.165f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.161f, .1665f, .408f, .408f, .12615f, .125f, .165f, .126f, .1225f, .1699f, .1568f},
    {.165f, .1665f, .4075f, .4075f, .126f, .125f, .161f, .14f, .125f, .17067f, .160f},
    {.165f, .1665f, .408f, .4195f, .1265f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.165f, .1665f, .4077f, .4077f, .126f, .125f, .161f, .126f, .1275f, .169f, .1608f},
    {.166f, .1665f, .408f, .408f, .125f, .125f, .161f, .1253f, .124f, .17067f, .160f},
    {.166f, .1654f, .408f, .408f, .126f, .124f, .161f, .126f, .125f, .17067f, .160f},
    {.163f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.163f, .1665f, .4077f, .4050f, .126f, .125f, .161f, .125f, .124f, .17067f, .160f},
    {.164f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.165f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.165f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f},
    {.166f, .1665f, .408f, .408f, .126f, .125f, .161f, .126f, .125f, .17067f, .160f}};

constexpr float kRest[16] = {434, 429, 430, 429.5f, 431, 436, 432, 429, 429, 429, 432, 430, 430, 429.5f, 429, 430};
constexpr float kPitch = 64.5f, kTile = 65.0f, kEdge = 70.0f, kCorner = 175.0f, kTop = 640.0f, kLastRow = kTop + 3 * kPitch;

// The PS1 window builder's pieces: the sheet rectangle (u, v, w, h), the point of the rectangle that sits at the piece's base position
// (ox, oy; the position is (u - ox, v - oy) from the window's top-left corner minus half its size), and how the piece follows the window.
enum class PieceKind { TopLeft, TopRight, BottomLeft, BottomRight, TopEdge, BottomEdge, LeftEdge, RightEdge, FillLeft, FillRight };
struct SkinPiece {
  int u, v, w, h, ox, oy;
  PieceKind kind;
};
constexpr SkinPiece kRectPieces[10] = {
    {0, 0, 16, 16, 0, 16, PieceKind::TopEdge},       {0, 16, 16, 16, 0, 16, PieceKind::BottomEdge},  {16, 0, 16, 16, 32, 0, PieceKind::LeftEdge},
    {16, 16, 16, 16, 16, 16, PieceKind::RightEdge},  {32, 0, 48, 40, 64, 24, PieceKind::TopLeft},    {80, 0, 48, 40, 96, 24, PieceKind::TopRight},
    {32, 40, 48, 40, 64, 56, PieceKind::BottomLeft}, {80, 40, 48, 40, 96, 56, PieceKind::BottomRight}, {0, 32, 32, 32, 0, 32, PieceKind::FillLeft},
    {0, 32, 32, 32, 0, 32, PieceKind::FillRight}};
constexpr float kSheetW = 128.0f, kSheetH = 96.0f;

// One run of a piece along one axis: pixels p0..p1 from the piece's start show sheet texels t0..t1.
struct Span {
  float p0, p1, t0, t1;
};
// The runs of `count` texels from `first` on, `scale` pixels per texel. Through a tile window (`tiled`) the sheet coordinate wraps inside
// origin .. origin + size. The PS1 rounds the texture coordinate of a pixel to the nearest texel, so texel k covers the pixels from
// ceil(scale (k - 1/2)) on: two pixels wide it starts one pixel early, the piece's first texel shows half and the texel after the last one
// fills the piece's last half texel.
std::vector<Span> texel_spans(int first, int count, int origin, int size, bool tiled, float scale) {
  auto sheet = [&](int k) { return tiled ? origin + (first + k) % size : first + k; };
  auto edge = [&](int k) { return std::ceil(scale * (static_cast<float>(k) - 0.5f)); };
  const float end = scale * static_cast<float>(count);
  std::vector<Span> spans;
  for (int k = 0; k <= count;) {
    int last = k;
    while (last < count && sheet(last + 1) == sheet(last) + 1) ++last;
    const float p0 = std::max(0.0f, edge(k)), p1 = std::min(end, edge(last + 1));
    if (p1 > p0)
      spans.push_back({p0, p1, static_cast<float>(sheet(k)) + (p0 - edge(k)) / scale, static_cast<float>(sheet(k)) + (p1 - edge(k)) / scale});
    k = last + 1;
  }
  return spans;
}

}  // namespace

std::vector<WinQuad> build_skin_rect(float cx, float cy, int w, int h, float x_scale, bool shift_right) {
  std::vector<WinQuad> quads;
  const int fill_w = (w + 8) / 2;
  const int right_shift = shift_right ? -1 : 0;
  // The pieces are linked to the front of one draw list in table order, so the last one is drawn first.
  for (int index = 9; index >= 0; --index) {
    const SkinPiece& piece = kRectPieces[index];
    // dx, dy: added to the base position; extra_w / extra_h: added to the sheet rectangle's size; shift: added to its u.
    int dx = 0, dy = -4, extra_w = 0, extra_h = 0, shift = 0;
    bool tiled = true;
    switch (piece.kind) {
      case PieceKind::TopLeft: tiled = false; break;
      case PieceKind::TopRight: tiled = false, shift = right_shift, dx = w; break;
      case PieceKind::BottomLeft: tiled = false, dy = h + 4; break;
      case PieceKind::BottomRight: tiled = false, shift = right_shift, dx = w, dy = h + 4; break;
      case PieceKind::TopEdge: extra_w = w - piece.w - 32, dx = 16; break;
      case PieceKind::BottomEdge: extra_w = w - piece.w - 32, dx = 16, dy = h + 4; break;
      case PieceKind::LeftEdge: extra_h = h - piece.h - 24, dy = 12; break;
      case PieceKind::RightEdge: extra_h = h - piece.h - 24, dy = 12, shift = right_shift, dx = w; break;
      case PieceKind::FillLeft: extra_w = fill_w - piece.w, extra_h = h - piece.h + 16, dx = -4, dy = -8; break;
      case PieceKind::FillRight: extra_w = fill_w - piece.w, extra_h = h - piece.h + 16, shift = fill_w, dx = fill_w - 4, dy = -8; break;
    }
    const int x = dx + piece.u - piece.ox - w / 2, y = dy + piece.v - piece.oy - h / 2;
    const int width = piece.w + extra_w, height = piece.h + extra_h;
    if (width <= 0 || height <= 0) continue;
    const int u0 = (piece.u + shift) & 0xFF;
    const std::vector<Span> columns = texel_spans(u0, width, piece.u, piece.w, tiled, x_scale);
    const std::vector<Span> rows = texel_spans(piece.v, height, piece.v, piece.h, tiled, 1.0f);
    const bool fill = piece.kind == PieceKind::FillLeft || piece.kind == PieceKind::FillRight;
    for (const Span& row : rows)
      for (const Span& col : columns)
        quads.push_back({cx + x_scale * static_cast<float>(x) + col.p0, cy + static_cast<float>(y) + row.p0, col.p1 - col.p0, row.p1 - row.p0, col.t0 / kSheetW,
                         row.t0 / kSheetH, (col.t1 - col.t0) / kSheetW, (row.t1 - row.t0) / kSheetH, fill});
  }
  return quads;
}

float skin_window_x(int skin) { return kRest[std::clamp(skin, 0, 15)]; }

// Piece order: the 7 x 4 fill grid (column-major, 65-unit tiles at a 64.5 pitch), the two left corners with the top / bottom edge
// pieces interleaved (1..4, skin 0: 1..5), the left edge strip, then the right corners (edge slot 5) with the right edge strip (slot 6).
// The per-skin offsets below are the sheet-specific nudges of RDE's piece records; the leaf is the 70 x 70 page-turn quad.
SkinWindow build_skin_window(int skin, int leaf_phase) {
  const int skin_idx = std::clamp(skin, 0, 15);
  const SkinUv& uv = kUv[skin_idx];
  const float rest_x = kRest[skin_idx];
  SkinWindow window;
  auto quad = [&](float x, float y, float sw, float sh, float u0, float v0, float du, float dv) { window.quads.push_back({x, y, sw, sh, u0, v0, du, dv}); };

  for (int col = 0; col < 7; ++col)
    for (int row = 0; row < 4; ++row)
      quad(rest_x + kPitch * static_cast<float>(col), kTop + kPitch * static_cast<float>(row), kTile, kTile, (col & 1) ? 0.125f : 0.0f,
           (row & 1) ? 0.49925f : 0.335f, 0.1233f, 0.165f);
  window.left = rest_x;
  window.top = kTop;
  window.right = rest_x + kPitch * 6 + kTile;
  window.bottom = kLastRow + kTile;

  const bool narrow = skin_idx == 8 || skin_idx == 9 || skin_idx == 11;
  const float cdu = narrow ? .3120f : .3125f, cdv = narrow ? .4130f : .4140f, rdu = narrow ? .3103f : .3125f;
  const float bcdv = skin_idx == 10 ? .4132f : cdv;
  const float T0x = rest_x - 65.0f * 1.28f * (skin_idx == 0 ? 1.013f : 1.0f);
  const float T0y = kTop - 65.0f * 1.35f + (skin_idx == 0 ? -1.0f : 0.0f);
  float B0y = kLastRow + kEdge * 0.5f;
  switch (skin_idx) {
    case 0: B0y -= 65.0f * 0.745f; break;
    case 1:
    case 2: B0y -= 65.0f * 0.95f; break;
    case 4: B0y -= 65.0f; break;
    case 5: B0y -= 65.0f * 0.7f + 20.0f; break;
    case 6: B0y -= 65.0f * 0.7f + 15.0f; break;
    default: B0y -= 65.0f * 0.7f; break;
  }
  float bottom_edge_y = kLastRow + kEdge / 2.05f;
  switch (skin_idx) {
    case 0: bottom_edge_y += 65.0f * 0.4f; break;
    case 1: bottom_edge_y += 65.0f * 0.2f; break;
    case 2: bottom_edge_y += 65.0f * 0.12f; break;
    case 4: bottom_edge_y += 65.0f * 0.14f; break;
    case 5: bottom_edge_y += 65.0f * 0.45f - 20.0f; break;
    case 6: bottom_edge_y += 65.0f * 0.45f - 15.0f; break;
    case 8: bottom_edge_y += 65.0f * 0.443f - 2.5f; break;
    case 9: bottom_edge_y += 65.0f * 0.439f - 0.1f; break;
    case 11: bottom_edge_y += 65.0f * 0.444f; break;
    default: bottom_edge_y += 65.0f * 0.45f; break;
  }
  const float edge_x0 = T0x + kEdge * 1.5f * 0.98f - 65.0f / 1.03f + 65.0f;
  const float top_edge_y = T0y + kEdge * 1.475f - 65.0f * (skin_idx == 9 ? 1.054f : 1.047f);

  quad(T0x, T0y, kCorner, kCorner, .3125f, 0, cdu, cdv);
  quad(T0x, B0y, kCorner, kCorner, .3125f, uv.b0v, cdu, bcdv);
  for (int i = 1; i <= (skin_idx == 0 ? 5 : 4); ++i) {
    quad(edge_x0 + kEdge * static_cast<float>(i), top_edge_y, kEdge, kEdge, 0, 0, 0.123f, 0.16667f);
    quad(edge_x0 + kEdge * static_cast<float>(i), bottom_edge_y, kEdge, kEdge, 0, uv.bv, 0.123f, uv.bdv);
  }
  const float left_x = rest_x - 65.0f * 0.73f * (skin_idx == 0 ? 1.03f : 1.0f);
  for (int j = 0; j < 18; ++j) {
    const float y = static_cast<float>(j) * kEdge * 0.999f + T0y + 65.0f * 2.68f;
    if (kEdge * 0.2f + y - 15.0f > B0y) break;
    quad(left_x, y, kEdge, kEdge, uv.lu, 0, uv.ldu, uv.ldv);
  }
  float R0x = edge_x0 + kEdge * 5;
  if (skin_idx == 0) R0x *= 1.008f;
  if (skin_idx == 4) R0x *= 0.997f;
  float right_edge_x = edge_x0 + kEdge * 6;
  switch (skin_idx) {
    case 0: right_edge_x = R0x + 56.5f; break;
    case 4: right_edge_x -= 4.2f; break;
    case 5: right_edge_x -= 5.5f; break;
    case 9:
    case 11: right_edge_x += 1.0f; break;
    case 10: right_edge_x += 0.5f; break;
    default: break;
  }
  const float r0u = skin_idx == 0 || skin_idx == 5 ? .65f : skin_idx == 6 ? .64f : .625f, r24u = skin_idx == 0 || skin_idx == 5 ? .65f : .625f;
  const float R24y = B0y + (skin_idx == 6 ? 4.5f : 0.0f);
  quad(R0x, T0y, kCorner, kCorner, r0u, 0, rdu, cdv);
  for (int i = 1; i < 24; ++i) {
    const float y = T0y + 65.0f * 1.615f + kEdge * static_cast<float>(i) + (skin_idx == 0 ? -1.0f : 0.0f);
    if (y > R24y) break;
    quad(right_edge_x, y, kEdge, kEdge, uv.cu, uv.cv, uv.cdu, uv.cdv);
  }
  quad(R0x, R24y, kCorner, kCorner, r24u, uv.r24v, rdu, bcdv);

  window.leaf = {rest_x + 380.0f, 825.0f, kEdge, kEdge, 0.5f + 0.125f * static_cast<float>(std::clamp(leaf_phase, 0, 3)), 0.83333f, 0.124f, 0.16667f};
  return window;
}

}  // namespace oc::ui
