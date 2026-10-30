// BattleView: the battle HUD and menus in both art modes - the party status panels, the command box (top menu and the HIT% chain menu),
// the element list with its slide machine and cast header, target selection (cursor, info window, camera hint), the Dopplegang creature
// list, the results panel and the debug auto player.
//
// Layout: the HUD is drawn on a 1280 x 960 canvas; PS1 draw coordinates map to canvas x * kPs1Px (4 per column) and y * kRowScale (960 / 216 per
// line). Battle text goes through btext(), which picks the font slot, multiplier and shrink rule of a PS1 text style.
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"
#include "game/battle/status_plates.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <format>

#include "import/text.h"
#include "platform/keymap.h"
#include "render/texture.h"

namespace oc::game {

using namespace bv;

namespace {
// Canvas y per PS1 HUD line. Reference lines measured on a 236-line frame (the HUD layer stretched by 236 / 216) are scaled
// to the PS1 row pitch: PS1 line = reference line * kRefLineY.
constexpr float kRowScale = kCanvasH / 216.0f;
constexpr float kRefLineY = 216.0f / 236.0f;
inline float ref_y(float ref_line) { return ref_line * kRefLineY * kRowScale; }

// Stamina labels per language (JP has no translation row: the key itself in the JP font).
const char32_t* kStaLabel[6] = {U"Sta.", U"End.", U"Vig.", U"Asd.", U"Vit.", U"Sta."};
const char32_t* kOutLabel[6] = {U"Out.-", U"End.-", U"Vig.-", U"Asd.-", U"Vit.-", U"Out.－"};
// The innate label per language and its x nudge (y - 1 outside JP).
const char32_t* kInnateLabel[6] = {U"INNATE:", U"INNÉE :", U"INNATO:", U"NATURELL:", U"AFINIDAD:", U"属性:"};
constexpr float kInnateDx[6] = {-3, -1, -4, -16, -11, 11};
// The innate dot colour words in data::colour_name order (green white black red yellow blue).
constexpr u32 kInnateDot[6] = {0x00FF00, 0xFFFFFF, 0x733152, 0x0000FF, 0x00FFFF, 0xFF0000};
// The top command menu rows per language.
const char32_t* kCommand[6][4] = {{U"Attack", U"Element", U"Defend", U"Run Away"},
                                  {U"Attaquer", U"Élément", U"Défendre", U"Fuir"},
                                  {U"Attacco", U"Elemento", U"Difesa", U"Fuga"},
                                  {U"Angreifen", U"Element", U"Verteidigen", U"Fliehen"},
                                  {U"Atacar", U"Elementos", U"Defenderse", U"Escapar"},
                                  {U"攻撃", U"エレメント", U"防御", U"逃走"}};
const char32_t* kCommandCaptionJp[4] = {U"Attack", U"Element", U"Defend", U"RunAway"};
const char32_t* kHitCaption[6] = {U"HIT%", U"%RÉUS.", U"PREC.", U"TRF%", U"%ACIERTO", U"命中率"};
const char32_t* kCamNormal[6] = {U"NORMAL", U"NORMALE", U"NORMALE", U"NORMAL", U"NORMAL", U"NORMAL"};
const char32_t* kCamDynamic[6] = {U"DYNAMIC", U"DYNAMIQUE", U"DINAMICA", U"DYNAMISCH", U"DINÁMICO", U"DYNAMIC"};

// The HD text path remaps two PS1 colour words: dim 0x404040 -> 0x565656, white 0x808080 -> 0x8A8A8A.
u32 remap_text_colour(u32 colour) { return colour == 0x404040 ? 0x565656 : colour == 0x808080 ? 0x8A8A8A : colour; }

std::u32string digits_u32(int value) {
  std::string digits = std::to_string(value);
  return std::u32string(digits.begin(), digits.end());
}

// The HP digit colour word: orange at HP <= max / 4, red-orange at HP <= max / 8, else white.
u32 hp_colour(int hp, int max_hp) { return remap_text_colour(hp * 8 <= max_hp ? 0x3080u : hp * 4 <= max_hp ? 0x4080u : 0x808080u); }

// Element-list slide tables: x of the plates per step and the plate frame per step (3 = the steady plate, 4..10 = the slide frames). A 0 in
// the x table ends a run. Runs: 0 = out to the right (x 8 -> 331, 18 steps), 20 = in from the right (331 -> 8, 18), 40 = in from the left
// (-154 -> 8, 10), 60 = out to the left (8 -> -154, 10).
constexpr short kSlideX[72] = {8,    27,   46,   65,   84,   103,  122,  141,  160,  179,  198,  217, 236, 255, 274, 293, 312, 331, 0,    0,    331,  312, 293, 274,
                               255,  236,  217,  198,  179,  160,  141,  122,  103,  84,   65,   46,  27,  8,   0,   0,   -154, -136, -118, -100, -82, -64, -46, -28,
                               -10,  8,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,   8,   -10, -28, -46, -64, -82,  -100, -118, -136, -154, 0,  0};
constexpr unsigned char kSlideFrame[80] = {3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 3, 3, 3, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                           3, 3, 3, 3, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

// Element name plates: the plate set is the element colour (0..5 in data::colour_name order, 6 = the rainbow set for an all-colour element);
// -1 = no plate. The sprite index into eplate_ = used set offset + colour * 10 + frame - 1 (frames 1..3 steady, 4..10 the slide).
int plate_colour_of(const data::ElemRec& elem) {
  const int colour = data::colour_index_of_bit(elem.colour_bit());
  return colour >= 0 && colour < 6 ? colour : (elem.colour_bit() & 0xFC) == 0xFC ? 6 : -1;
}
size_t plate_index(bool used, int plate_colour, int frame) { return static_cast<size_t>((used ? 70 : 0) + plate_colour * 10 + frame - 1); }

// ---- element menu navigation ----
// A cell is a menu entry when it holds an element and is not locked / empty (`usable`); used cells stay selectable (greyed), the confirm
// refuses them.
bool elem_valid(const battle::Unit& unit, size_t elem_count, int level, int slot) {
  const int idx = level * data::kGridCols + slot;
  const int id = unit.cells[static_cast<size_t>(idx)] & 0x3FFF;
  return id != 0 && static_cast<size_t>(id) < elem_count && unit.usable[static_cast<size_t>(idx)];
}
// A level can be picked when it is not above the level gauge and holds a valid cell.
bool elem_level_ok(const battle::Unit& unit, size_t elem_count, int level) {
  if (level < 0 || level >= data::kGridRows || level > unit.gauge) return false;
  for (int slot = 0; slot < data::kGridCols; ++slot)
    if (elem_valid(unit, elem_count, level, slot)) return true;
  return false;
}
// The highest pickable level at or below the gauge, -1 if none.
int elem_open_level(const battle::Unit& unit, size_t elem_count) {
  for (int level = std::min(unit.gauge, data::kGridRows - 1); level >= 0; --level)
    if (elem_level_ok(unit, elem_count, level)) return level;
  return -1;
}
// From `slot` the first valid slot upward without wrap, else the first valid one below it.
int elem_near_slot(const battle::Unit& unit, size_t elem_count, int level, int slot) {
  for (int next = std::max(slot, 0); next < data::kGridCols; ++next)
    if (elem_valid(unit, elem_count, level, next)) return next;
  for (int next = std::min(slot, data::kGridCols) - 1; next >= 0; --next)
    if (elem_valid(unit, elem_count, level, next)) return next;
  return -1;
}
// The next / previous valid slot of the level over all ten slots with wrap (the cell itself after a full turn).
int elem_step_slot(const battle::Unit& unit, size_t elem_count, int level, int slot, int dir) {
  for (int k = 1; k <= data::kGridCols; ++k) {
    const int next = ((slot + dir * k) % data::kGridCols + data::kGridCols) % data::kGridCols;
    if (elem_valid(unit, elem_count, level, next)) return next;
  }
  return -1;
}
// The next / previous pickable level with wrap, -1 when there is none.
int elem_step_level(const battle::Unit& unit, size_t elem_count, int level, int dir) {
  for (int k = 1; k <= data::kGridRows; ++k) {
    const int next = ((level + dir * k) % data::kGridRows + data::kGridRows) % data::kGridRows;
    if (elem_level_ok(unit, elem_count, next)) return next;
  }
  return -1;
}
// The cell the menu opens on: the highest pickable level, slot 5 or the nearest valid one; -1 = the menu does not open.
int elem_open_cell(const battle::Unit& unit, size_t elem_count) {
  const int level = elem_open_level(unit, elem_count);
  if (level < 0) return -1;
  const int slot = elem_near_slot(unit, elem_count, level, 5);
  return slot < 0 ? -1 : level * data::kGridCols + slot;
}
// The list / header text: the name plus "%+d" of the cell's offset from the element's native level (clamped to 7; none for level-less
// elements).
std::string elem_label(const std::string& name, const data::ElemRec& elem, int row) {
  if (elem.level() == 0) return name;
  const int delta = row - (elem.level() - 1);
  if (delta == 0) return name;
  return name + std::format("{:+d}", std::min(std::abs(delta), 7) * (delta > 0 ? 1 : -1));
}
}  // namespace

// ------------------------------------------------------------------------------------------------------- battle text
// One battle text entry at PS1 draw coordinates (x, y). `flags` = style | 0x10000 (JP font) | 0x20000 (non-JP font); without either bit the
// language decides. The style picks the font slot and the size multiplier; the canvas position is x * kPs1Px + the slot's x offset and
// y * kRowScale + the slot's y offset, both offsets scaled; then the style's own rule applies: a line loop (styles 1, 3, 0x83, 0xB8), a
// shrink to a width budget that moves the baseline by the lost height (0x80 54 px, 0x81 102 px centred, 0x85 86 px centred, 0x86 180 canvas
// px, 0x84 256 px or centred inside them). Style 0xB8 (the Dopplegang list) shrinks only its first entry, to 578 canvas px.
void BattleView::btext(int flags, const std::u32string& text, float x, float y, u32 colour) {
  namespace ft = ui::font;
  if (text.empty()) return;
  const int style = flags & 0xFF;
  const int lang = std::clamp(static_cast<int>(lang_), 0, 5);
  const bool jp = (flags & 0x10000) ? true : (flags & 0x20000) ? false : lang == 5;
  int slot = jp ? ft::kBattleJp : ft::kBattle;
  switch (style) {
    case 1: case 8: case 9: case 0x81: case 0x83: case 0x84: slot = jp ? ft::kKb : ft::kBn; break;
    case 0xA: slot = ft::kCh; break;
    case 0xB: slot = ft::kKb; break;
    default: break;
  }
  float mul = 1.0f;
  switch (style) {
    case 1: mul = jp ? 1.0f : 0.70f; break;
    case 3: case 9: case 0xB: case 0x86: mul = 0.65f; break;
    case 5: case 8: case 0x82: mul = 0.85f; break;
    default: break;
  }
  float first_entry_dy = 0;
  if (style == 0xB8) {
    if (x == 0x40 && y == 0x38) {
      fonts_.prepare(slot, text);
      const float width = fonts_.measure(slot, text, 1.0f);
      if (width > 578.0f) mul = 578.0f / width, first_entry_dy = text.size() < 9 ? 1.0f : 2.0f;
    }
    slot = jp ? ft::kKb : ft::kBn;
  }
  const ft::SlotInfo& slot_info = ft::slot(slot);
  fonts_.prepare(slot, text);
  float scale = slot_info.scale * mul;
  float canvas_x = x * kPs1Px + slot_info.off_x * scale, canvas_y = (y + first_entry_dy) * kRowScale + slot_info.off_y * scale;
  const float baseline_shrink = jp ? 32.0f : 28.0f;
  // Font em in px.
  const float em = static_cast<float>(slot_info.size) * 96.0f / 72.0f;
  auto push = [&](std::u32string segment, float px, float py, const ft::Ink& ink, float text_scale, int align) {
    add_text_op(slot, std::move(segment), px, py, ink, text_scale, align);
  };
  auto lines = [&](float pitch, const ft::Ink& ink) {
    size_t begin = 0;
    for (float line_y = canvas_y;; line_y += pitch) {
      const size_t end = text.find(U'\n', begin);
      push(text.substr(begin, end == std::u32string::npos ? std::u32string::npos : end - begin), canvas_x, line_y, ink, scale, 0);
      if (end == std::u32string::npos) break;
      begin = end + 1;
    }
  };
  switch (style) {
    case 1:
      if (!jp) canvas_y += 4;
      lines(13 * kRowScale, ft::h2(colour, 31, 1));
      break;
    case 2: push(text, canvas_x, canvas_y, ft::h1(colour, 31, 5), scale, 0); break;
    case 3:
      if (text.find(U'\n') != std::u32string::npos) canvas_y -= 4 * kRowScale;
      lines(7 * kRowScale, ft::h2(colour, 31, 1));
      break;
    case 4: push(text, canvas_x, canvas_y, ft::h2(colour, 0x18, 2), scale, 0); break;
    case 8: case 9: push(text, canvas_x, canvas_y, ft::h1(colour, 31, 1), scale, 0); break;
    case 0xA: push(text, canvas_x, canvas_y, {render::pack_rgba(232, 232, 232, 128), render::pack_rgba(33, 33, 33, 128), false, true}, scale, 2); break;
    case 0xB: push(text, canvas_x, canvas_y, ft::h1(colour, 0x10, 1), scale, 0); break;
    case 0x80: {
      const float width_px = fonts_.measure(slot, text, scale) / kPs1Px;
      if (width_px > 54.0f) {
        scale *= 54.0f / width_px;
        canvas_y += baseline_shrink - baseline_shrink * 54.0f / width_px;
      }
      push(text, canvas_x, canvas_y, ft::h1(colour, 31, 4), scale, 0);
      break;
    }
    case 0x81: {
      const float width = fonts_.measure(slot, text, scale);
      const float shrink = width > 0 ? std::min(1.0f, 102.0f * kPs1Px / width) : 1.0f;
      scale *= shrink;
      canvas_y += (1.0f - shrink) * em * 0.5f;
      push(text, canvas_x, canvas_y, ft::h1(colour, 31, 1), scale, 1);
      break;
    }
    case 0x85: {
      const float width = fonts_.measure(slot, text, scale);
      if (width > 86.0f * kPs1Px) {
        const float shrink = 86.0f * kPs1Px / width;
        scale *= shrink;
        canvas_y += (1.0f - shrink) * baseline_shrink * 0.5f;
      }
      push(text, canvas_x, canvas_y, ft::h1(colour, 31, 1), scale, 1);
      break;
    }
    case 0x86: {
      const float width = fonts_.measure(slot, text, scale);
      const float shrink = width > 0 ? std::min(1.0f, 180.0f / width) : 1.0f;
      canvas_y += (1.0f - shrink) * em * 0.25f;
      scale *= shrink;
      push(text, canvas_x, canvas_y, ft::h2(colour, 31, 4), scale, 0);
      break;
    }
    case 0x83: case 0xB8: lines(16 * kRowScale, ft::h1(colour, 31, 4)); break;
    case 0x84: {
      const float width_px = std::floor(fonts_.measure(slot, text, scale) / kPs1Px);
      if (width_px > 256.0f) {
        const float shrink = 256.0f / width_px;
        canvas_y += (1.0f - shrink) * em * 0.5f;
        scale *= shrink;
      } else {
        canvas_x += (256.0f - width_px) * 0.5f * kPs1Px;
      }
      push(text, canvas_x, canvas_y, ft::h1(colour, 31, 1), scale, 0);
      break;
    }
    default: push(text, canvas_x, canvas_y, ft::h3(colour, style == 0x82 ? 0x319F : 0x6319, 3), scale, 0); break;
  }
}

std::u32string BattleView::xlat(const char* jp, const char32_t* fallback) {
  if (const import::TextTable* table = lang_tables_.table("cmnmes"))
    if (const std::string* row = table->get(std::string_view(jp), lang_); row && !row->empty()) return import::utf8_to_u32(*row);
  return lang_ == import::Lang::Jp ? import::utf8_to_u32(jp) : std::u32string(fallback);
}

// ---------------------------------------------------------------------------------------------- waku art preparation
// The party panel art (the subtractive base and the flash frame, 64 x 48 PS1 px at 4x) is drawn through a bilinear sampler, and the files
// have two defects that would show: their transparent texels carry RGB 255 (base) or 0 (frame), which a bilinear edge sample mixes in (the base
// is a subtract draw whose source alpha is no blend factor: a black rim round the panel), and the two chamfered corners are a binary staircase.
// Base (frame = false): the mask is eroded one texel, the chamfer line of each corner is fitted from the mask and rebuilt with 4 x 4
// supersampled coverage one texel inside the line, and RGB := grey * coverage (premultiplied, empty = 0). Frame (frame = true): the empty
// texels take the nearest frame colour (two dilation passes) and the staircase band along each chamfer gets a premultiplied 3 x 3 tent blur.
void battle_waku_prepare(render::DecodedImage& img, bool frame) {
  const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
  if (w < 32 || h < 32 || img.rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4) return;
  auto at = [&](int x, int y) { return (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4; };
  auto ix = [&](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x); };
  std::vector<u8> opaque(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
  auto opaque_at = [&](int x, int y) -> bool { return x >= 0 && y >= 0 && x < w && y < h && opaque[ix(x, y)] != 0; };
  int x0 = w, x1 = -1, y0 = h, y1 = -1;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (img.rgba[at(x, y) + 3] > 127) {
        opaque[ix(x, y)] = 1;
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      }
  if (x1 < x0 + 16 || y1 < y0 + 16) return;
  // Fitted chamfer lines x = offset + slope * y (texel units), one per corner whose end rows are shorter than the box.
  struct Diag {
    bool ok = false, right = false, bottom = false;
    double offset = 0, slope = 0, norm = 1, sign = 1;
  };
  Diag diags[4];
  const double cx = 0.5 * (x0 + x1 + 1), cy = 0.5 * (y0 + y1 + 1);
  for (int k = 0; k < 4; ++k) {
    const bool right = (k & 1) != 0, bottom = (k & 2) != 0;
    double sum_y = 0, sum_x = 0, sum_yy = 0, sum_xy = 0;
    int count = 0;
    for (int row = 0; row < (y1 - y0) / 2; ++row) {
      const int y = bottom ? y1 - row : y0 + row;
      // The row's boundary on the corner's side, a texel edge.
      int edge_x = -1;
      if (right) {
        for (int x = x1; x >= x0; --x)
          if (opaque_at(x, y)) { edge_x = x + 1; break; }
      } else {
        for (int x = x0; x <= x1; ++x)
          if (opaque_at(x, y)) { edge_x = x; break; }
      }
      // The full width again: the diagonal is over.
      if (edge_x < 0 || (right ? edge_x >= x1 + 1 : edge_x <= x0)) break;
      const double yc = y + 0.5;
      sum_x += edge_x, sum_y += yc, sum_yy += yc * yc, sum_xy += edge_x * yc;
      ++count;
    }
    if (count < 8) continue;
    const double den = count * sum_yy - sum_y * sum_y;
    if (std::fabs(den) < 1e-6) continue;
    Diag& diag = diags[k];
    diag.slope = (count * sum_xy - sum_x * sum_y) / den;
    diag.offset = (sum_x - diag.slope * sum_y) / count;
    diag.norm = std::sqrt(1.0 + diag.slope * diag.slope);
    diag.right = right;
    diag.bottom = bottom;
    diag.sign = (cx - diag.slope * cy - diag.offset) >= 0 ? 1.0 : -1.0;
    diag.ok = true;
  }
  // Signed distance to the diagonal, > 0 on the panel's side.
  auto dist = [&](const Diag& diag, double x, double y) { return diag.sign * (x - diag.slope * y - diag.offset) / diag.norm; };
  auto in_quadrant = [&](const Diag& diag, int x, int y) {
    return (diag.right ? x + 0.5 >= cx : x + 0.5 < cx) && (diag.bottom ? y + 0.5 >= cy : y + 0.5 < cy);
  };

  if (!frame) {
    std::vector<float> coverage(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (opaque_at(x, y) && opaque_at(x - 1, y) && opaque_at(x + 1, y) && opaque_at(x, y - 1) && opaque_at(x, y + 1)) coverage[ix(x, y)] = 1.0f;
    for (const Diag& diag : diags) {
      if (!diag.ok) continue;
      for (int y = y0 + 1; y < y1; ++y)
        for (int x = x0 + 1; x < x1; ++x) {
          if (!in_quadrant(diag, x, y) || dist(diag, x + 0.5, y + 0.5) > 14.0) continue;
          int hit = 0;
          for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
              if (dist(diag, x + (i + 0.5) / 4.0, y + (j + 0.5) / 4.0) >= 1.0) ++hit;
          coverage[ix(x, y)] = static_cast<float>(hit) / 16.0f;
        }
    }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        u8* texel = &img.rgba[at(x, y)];
        const float cover = coverage[ix(x, y)];
        for (int k = 0; k < 3; ++k) texel[k] = static_cast<u8>(std::lround((opaque_at(x, y) ? static_cast<float>(texel[k]) : 153.0f) * cover));
        texel[3] = cover > 0.0f ? static_cast<u8>(std::max(1L, std::lround(cover * 255.0f))) : 0;
      }
    return;
  }

  std::vector<u8> good = opaque;
  for (int pass = 0; pass < 2; ++pass) {
    const std::vector<u8> src = img.rgba, good_src = good;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        if (good_src[ix(x, y)]) continue;
        int sum[3] = {0, 0, 0}, count = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h || !good_src[ix(xx, yy)]) continue;
            for (int k = 0; k < 3; ++k) sum[k] += src[at(xx, yy) + static_cast<size_t>(k)];
            ++count;
          }
        if (count == 0) continue;
        for (int k = 0; k < 3; ++k) img.rgba[at(x, y) + static_cast<size_t>(k)] = static_cast<u8>(sum[k] / count);
        good[ix(x, y)] = 1;
      }
  }
  const std::vector<u8> src = img.rgba;
  static const int kTent[3][3] = {{1, 2, 1}, {2, 4, 2}, {1, 2, 1}};
  // The staircase texels: a 3 x 3 neighbourhood of the alpha mask that is neither constant down its columns (a horizontal edge) nor along its
  // rows (a vertical edge), plus one texel round them; straight bands stay crisp.
  std::vector<u8> stair(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
  for (int y = 1; y < h - 1; ++y)
    for (int x = 1; x < w - 1; ++x) {
      bool vert = true, horz = true;
      for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
          if (opaque_at(x - 1 + i, y - 1 + j) != opaque_at(x - 1 + i, y - 1)) vert = false;
          if (opaque_at(x - 1 + i, y - 1 + j) != opaque_at(x - 1, y - 1 + j)) horz = false;
        }
      if (!vert && !horz) stair[ix(x, y)] = 1;
    }
  const std::vector<u8> stair0 = stair;
  for (int y = 1; y < h - 1; ++y)
    for (int x = 1; x < w - 1; ++x)
      if (!stair0[ix(x, y)])
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
            if (stair0[ix(x + dx, y + dy)]) stair[ix(x, y)] = 1;
  for (const Diag& diag : diags) {
    if (!diag.ok) continue;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        if (!in_quadrant(diag, x, y) || !stair[ix(x, y)] || std::fabs(dist(diag, x + 0.5, y + 0.5)) > 14.0) continue;
        double alpha_sum = 0, rgb_sum[3] = {0, 0, 0};
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
            const double weight = kTent[dy + 1][dx + 1] * (src[at(xx, yy) + 3] / 255.0);
            alpha_sum += weight;
            for (int k = 0; k < 3; ++k) rgb_sum[k] += weight * src[at(xx, yy) + static_cast<size_t>(k)];
          }
        u8* texel = &img.rgba[at(x, y)];
        if (alpha_sum > 0.0)
          for (int k = 0; k < 3; ++k) texel[k] = static_cast<u8>(std::lround(rgb_sum[k] / alpha_sum));
        texel[3] = static_cast<u8>(std::lround(std::min(1.0, alpha_sum / 16.0) * 255.0));
      }
  }
}

// ------------------------------------------------------------------------------------------------------ party panels
// One status panel per present party member at PS1 (x, 164), x from a table by party size, the same art in both modes:
//  * waku: the subtractive base under the frame (the active frame blinks while the PC is choosing), 64 x 48 px at (x, 163);
//  * name (style 0x80, shrunk to 54 px) at (x + 4, y + 2); the element level digit (style 4) at (x + 9, y + 12) and "Lv" (style 5) at
//    (x + 4, y + 18) in green;
//  * HP: current right-aligned in three 8 px slots from x + 4, "/" at x + 28, max from x + 36, at y + 26 (style 4), the digits in the HP
//    colour;
//  * stamina: the "Sta." label and tens / "." / units digits at x + 42 / 47 / 52 (a hundreds digit at x + 30 shifts them 5 right), style 5;
//    negative stamina shows "Out.-" and the magnitude in style 0x82 (red). The text row sits at y + 37 (JP) / y + 37.8 so it does not cut into
//    the bar: a black 52 x 4 frame at (x + 8, y + 35) with a 50 x 2 track, fill = stamina * 100 / 140 px (stamina capped at 70), red below
//    1.0, yellow to 3.9, cyan from 4.0;
//  * grid: one 5 x 3 cell sprite per occupied unlocked cell at (x + 18 + 4 level, y + 6 + 2 slot): the gauge level blinks between the two
//    cell sprites, levels below it take the first, above it the second;
//  * status icons: one 6 x 5 icon per status group the unit's status word meets, packed left to right from (x + 6, y - 8).
void BattleView::build_party_panels() {
  const battle::Battle& battle = *battle_;
  int count = 0;
  for (int i = 0; i < battle::kPcSlots; ++i) count += battle.unit(i).present ? 1 : 0;
  if (count == 0) return;
  static const float kPanelX[3][3] = {{172, 0, 0}, {140, 205, 0}, {108, 172, 236}};
  const float py = 164;
  constexpr u32 kLvGreen = 0x408040;
  const int lang = std::clamp(static_cast<int>(lang_), 0, 5);
  const bool jp = lang == 5;
  int placed = 0;
  for (int i = 0; i < battle::kPcSlots; ++i) {
    const battle::Unit& unit = battle.unit(i);
    if (!unit.present) continue;
    const float px = kPanelX[std::min(count, 3) - 1][std::min(placed++, 2)];
    const Visual& visual = vis_[static_cast<size_t>(i)];
    const bool choosing = i == active_pc_ && (ui_ == Ui::Top || ui_ == Ui::Chain || ui_ == Ui::Element || ui_ == Ui::Target || ui_ == Ui::Dopple);
    const bool blink = choosing && (tick_count_ & 0x1F) > 15;
    const float waku_x = px * kPs1Px, waku_y = (py - 1) * kRowScale, waku_w = 64 * kPs1Px, waku_h = 48 * kRowScale;
    add_img(hud_[kHWakuBase], waku_x, waku_y, waku_w, waku_h, 0, 0, 1, 1, 0xFFFFFFFFu, 3);
    add_img(blink ? hud_[kHWakuAct] : hud_[kHWaku], waku_x, waku_y, waku_w, waku_h);
    btext(0x80, import::utf8_to_u32(unit.name), px + 4, py + 2, 0x8A8A8A);
    btext(4, digits_u32(std::clamp(unit.gauge + 1, 0, 9)), px + 9, py + 12, kLvGreen);
    btext(5, U"Lv", px + 4 - (lang == 1 || lang == 2 ? 1.0f : 0.0f), py + 18, kLvGreen);

    const int hp = std::clamp(static_cast<int>(std::lround(visual.hp_disp)), 0, 999);
    const u32 hp_col = hp_colour(hp, unit.max_hp);
    const std::u32string cur_digits = digits_u32(hp), max_digits = digits_u32(std::clamp(unit.max_hp, 0, 999));
    for (size_t k = 0; k < cur_digits.size() && k < 3; ++k)
      btext(4, cur_digits.substr(k, 1), px + 4 + 8.0f * static_cast<float>(3 - cur_digits.size() + k), py + 26, hp_col);
    btext(4, U"/", px + 28, py + 26, 0x8A8A8A);
    for (size_t k = 0; k < max_digits.size() && k < 3; ++k) btext(4, max_digits.substr(k, 1), px + 36 + 8.0f * static_cast<float>(k), py + 26, 0x8A8A8A);

    const bool out = unit.stamina < 0;
    const int stamina = std::min(std::abs(unit.stamina), 999);
    const int bar = std::min(out ? 0 : unit.stamina, 70);
    const float bar_x = (px + 8) * kPs1Px, bar_y = (py + 35) * kRowScale;
    add_rect(bar_x, bar_y, 52 * kPs1Px, 4 * kRowScale, col(0, 0, 0));
    add_rect(bar_x + kPs1Px, bar_y + kRowScale, 50 * kPs1Px, 2 * kRowScale, col(0, 0, 40));
    const u32 bar_colour = bar / 10 == 0 ? col(255, 0, 0) : bar / 10 < 4 ? col(255, 255, 0) : col(0, 255, 255);
    if (bar > 0) add_rect(bar_x + kPs1Px, bar_y + kRowScale, static_cast<float>(bar * 100 / 140) * kPs1Px, 2 * kRowScale, bar_colour);
    const float text_y = jp ? py + 37 : py + 37.8f;
    btext(out ? (jp ? 0x10082 : 0x82) : (jp ? 0x10005 : 5), out ? kOutLabel[lang] : kStaLabel[lang], out ? px + 22 : px + 26, text_y, 0x8A8A8A);
    const int digit_style = out ? 0x82 : 5;
    float digit_x = px + 42;
    if (stamina >= 100) {
      btext(digit_style, digits_u32(stamina / 100), px + 30, text_y, 0x8A8A8A);
      digit_x += 5;
    }
    btext(digit_style, digits_u32((stamina / 10) % 10), digit_x, text_y, 0x8A8A8A);
    btext(digit_style, U".", digit_x + 5, text_y, 0x8A8A8A);
    btext(digit_style, digits_u32(stamina % 10), digit_x + 10, text_y, 0x8A8A8A);

    for (int row = 0; row < data::kGridRows; ++row)
      for (int slot = 0; slot < data::kGridCols; ++slot) {
        const size_t idx = static_cast<size_t>(row * data::kGridCols + slot);
        if ((unit.cells[idx] & 0x3FFF) == 0 || !unit.usable[idx]) continue;
        const int gauge_state = unit.gauge < 0 ? 2 : row == unit.gauge ? 1 : row < unit.gauge ? 3 : 2;
        const bool first = gauge_state == 3 || (gauge_state == 1 && (tick_count_ & 0x3F) < 32);
        const float cell_x = (px + 18 + 4.0f * static_cast<float>(row)) * kPs1Px, cell_y = (py + 6 + 2.0f * static_cast<float>(slot)) * kRowScale;
        add_img(first ? hud_[kHGg1] : hud_[kHGg2], cell_x, cell_y, 5 * kPs1Px, 3 * kRowScale);
      }
    static constexpr u32 kIconMask[6] = {0x00B00008u, 0x00000B01u, 0xB0000020u, 0x0B000010u, 0x0000F000u, 0x000F0000u};
    const u32 status = unit.lo & 0xBBBFFB39u;
    int icon_count = 0;
    for (int k = 0; k < 6 && status; ++k) {
      if (!(status & kIconMask[k])) continue;
      const float u0 = 0.0075f + 0.3f * static_cast<float>(k % 3), v0 = 0.0075f + 0.3125f * static_cast<float>(k / 3);
      add_img(hud_[kHStatus], (px + 6 + 8.0f * static_cast<float>(icon_count++)) * kPs1Px, (py - 8) * kRowScale, 6 * kPs1Px, 5 * kRowScale, u0, v0, u0 + 0.285f, v0 + 0.2975f);
    }
  }
}

// Status plates (status_plates.h), the same art in both modes: the colour triangle is a 3 x 6 PS1 px cell of battle_34.png's bottom row
// (12 x 24 texels from texel row 40), the name the status's cmnmes text in style 2 (the JP font in Japanese), colour 0x757575.
void BattleView::build_status_plates(int unit_index, bool right) {
  if (unit_index < 0 || unit_index >= battle::kUnits) return;
  const bool jp = lang_ == import::Lang::Jp;
  const size_t first = op_count();
  for (const bstat::Placed& placed : bstat::place(battle_->unit(unit_index).lo, right)) {
    const bstat::Plate& plate = bstat::kPlates[static_cast<size_t>(placed.plate)];
    const float u0 = (12.0f * static_cast<float>(plate.colour) + 0.5f) / 80.0f, u1 = u0 + 11.0f / 80.0f;
    add_img(hud_[kHStatus], static_cast<float>(placed.x) * kPs1Px, static_cast<float>(placed.y + 1) * kRowScale, 3 * kPs1Px, 6 * kRowScale, u0,
            40.5f / 64.0f, u1, 63.5f / 64.0f);
    btext(jp ? 0x10002 : 2, xlat(plate.jp, plate.en), static_cast<float>(placed.x + 4), static_cast<float>(placed.y), 0x757575);
  }
  if (right) set_group(first, 1);
}

// One tick of the element list's slide machine. Opening starts "in from the right";
// a level change (Right: dir 1) first slides the shown plates out (left for Right, right for Left), then loads the new level's plates and slides them in from
// the opposite side. Every input of the menu waits for state 0. Returns whether the menu takes input this tick.
bool BattleView::elem_slide_step(int level) {
  // Just opened.
  if (elem_shown_ < 0) {
    elem_shown_ = level;
    elem_dir_ = 1;
    elem_age_ = 0;
    elem_state_ = 1;
    elem_idx_ = 20;
  } else if (elem_state_ == 0 && level != elem_shown_) {
    elem_state_ = 3;
    elem_idx_ = elem_dir_ ? 60 : 0;
  } else if (elem_state_ == 4) {
    elem_shown_ = level;
    elem_state_ = 1;
    elem_idx_ = elem_dir_ ? 20 : 40;
  }
  if (elem_state_ == 1 || elem_state_ == 3) {
    elem_x_ = kSlideX[elem_idx_];
    ++elem_idx_;
    if (kSlideX[elem_idx_] == 0) elem_state_ = elem_state_ == 1 ? 0 : 4;
  }
  elem_age_ = std::min(elem_age_ + 1, 8);
  return elem_state_ == 0;
}

// ---------------------------------------------------------------------------------------------------------- targeting
// Element record byte 4 (battle::target_spec): 0x40 party, 0x80 enemies, 0xC0 either side (the ring spans all nine units), 0x10 = the whole range,
// 0x04 = the caster only, 0x01 = knocked-out units only.
// Tgt::Ally means a party-only ring, Tgt::Enemy anything else; target_candidates reads the element itself.
BattleView::Tgt BattleView::target_mode(int cell) const {
  const int id = battle_->cell_element(active_pc_, cell);
  if (id <= 0 || static_cast<size_t>(id) >= tables_->elements.size()) return Tgt::Enemy;
  const data::ElemRec& elem = tables_->elements[static_cast<size_t>(id)];
  const battle::TargetSpec ts = battle::target_spec(elem);
  const bool ally = ts.party && !ts.enemies;
  const bool all = ts.all;
  return ally ? (all ? Tgt::AllyAll : Tgt::Ally) : (all ? Tgt::EnemyAll : Tgt::Enemy);
}

std::vector<int> BattleView::target_candidates() const {
  std::vector<int> out;
  // The ring: the units of the element's sides (Attack: the enemies), living ones, or only the knocked-out ones for bit 0x01; bit 0x04 pins the
  // cursor to the caster. Walked left to right on screen (the retail ring is sorted by view angle).
  battle::TargetSpec ts;
  if (pending_cmd_.kind == battle::Command::Kind::Element && pending_cmd_.cell >= 0) {
    const int id = battle_->cell_element(pending_cmd_.actor, pending_cmd_.cell);
    if (id > 0 && static_cast<size_t>(id) < tables_->elements.size()) ts = battle::target_spec(tables_->elements[static_cast<size_t>(id)]);
  }
  for (int i = ts.party ? 0 : battle::kPcSlots; i < (ts.enemies ? battle::kUnits : battle::kPcSlots); ++i) {
    const battle::Unit& unit = battle_->unit(i);
    if (!unit.present) continue;
    // Off-stage units (ops 34 / 35 / 5B) are never on the ring.
    if (battle_->untargetable(i)) continue;
    if (ts.self_only && i != pending_cmd_.actor) continue;
    if (ts.ko_only ? unit.alive() : !unit.alive()) continue;
    out.push_back(i);
  }
  {
    std::sort(out.begin(), out.end(), [&](int lhs, int rhs) {
      float ax = 0, ay = 0, bx = 0, by = 0;
      cam_.project(vis_[static_cast<size_t>(lhs)].x, 0, vis_[static_cast<size_t>(lhs)].z, ax, ay);
      cam_.project(vis_[static_cast<size_t>(rhs)].x, 0, vis_[static_cast<size_t>(rhs)].z, bx, by);
      return ax < bx;
    });
  }
  return out;
}

void BattleView::enter_target(Tgt mode) {
  tgt_mode_ = mode;
  targets_ = target_candidates();
  target_cursor_ = 0;
  if (targets_.empty()) {
    ui_ = Ui::Top;
    return;
  }
  {
    // Where the cursor starts, by unit id (the ring itself is walked in screen order afterwards). Enemy rings open on the caster's last
    // confirmed target (3 = the first enemy at battle start; a KO'd or gone one passes the cursor on to the next id, wrapping to 3); a
    // both-sides ring (0xC0) opens on the first living enemy, or on unit 0 when pgdat +4 has bit 0x02; a party ring (0x40) opens on the first
    // eligible party id (NOT the caster); bit 0x04 pins the caster; bit 0x01 starts on the first knocked-out party member.
    battle::TargetSpec ts;
    u8 raw4 = 0x80;
    if (pending_cmd_.kind == battle::Command::Kind::Element && pending_cmd_.cell >= 0) {
      const int id = battle_->cell_element(active_pc_, pending_cmd_.cell);
      if (id > 0 && static_cast<size_t>(id) < tables_->elements.size()) {
        ts = battle::target_spec(tables_->elements[static_cast<size_t>(id)]);
        raw4 = tables_->elements[static_cast<size_t>(id)].b(4);
      }
    }
    std::vector<int> order;
    if (ts.self_only) {
      order = {active_pc_};
    } else if (ts.party && ts.enemies) {
      const int first = (raw4 & 0x02) != 0 ? 0 : battle::kPcSlots;
      for (int i = 0; i < battle::kUnits; ++i) order.push_back((first + i) % battle::kUnits);
    } else if (ts.party) {
      for (int i = 0; i < battle::kPcSlots; ++i) order.push_back(i);
    } else {
      const int last = active_pc_ >= 0 && active_pc_ < battle::kPcSlots ? tgt_mem_[active_pc_] : battle::kPcSlots;
      const int enemy_count = battle::kUnits - battle::kPcSlots, from = std::clamp(last, battle::kPcSlots, battle::kUnits - 1) - battle::kPcSlots;
      for (int i = 0; i < enemy_count; ++i) order.push_back(battle::kPcSlots + (from + i) % enemy_count);
    }
    for (const int id : order) {
      const auto it = std::find(targets_.begin(), targets_.end(), id);
      if (it != targets_.end()) {
        target_cursor_ = static_cast<int>(it - targets_.begin());
        break;
      }
    }
  }
  ui_ = Ui::Target;
  // The command setup rewrote the acting list the menu camera's `zen0` / DYNAMIC shots are framed on; a cast leaves the pack's last shot (framed
  // on the caster) and its 7A/86 unit behind, so the previous PC's frame must not survive.
  script_actor_ = -1;
  anchor_actors_ = {active_pc_};
}

// ------------------------------------------------------------------------------------------------------------- input
void BattleView::auto_pick() {
  if (auto_delay_ > 0) {
    --auto_delay_;
    return;
  }
  if (active_pc_ < 0) return;
  // Rotation: keep one PC while it can still open its menu and chain Weak -> Strong -> Fierce on one target (the single attacks of
  // one PC on one target keep the chain accuracy bonus); when its stamina is spent, the ready PC with the most stamina takes over.
  const bool keep = auto_pc_ == active_pc_;
  if (!keep) {
    int best = active_pc_;
    for (int i = 0; i < battle::kPcSlots; ++i)
      if (battle_->can_act(i) && battle_->unit(i).stamina > battle_->unit(best).stamina) best = i;
    active_pc_ = best;
    auto_pc_ = best;
    auto_step_ = 0;
  }
  const battle::Unit& unit = battle_->unit(active_pc_);
  if (auto_target_ < battle::kPcSlots || !battle_->unit(auto_target_).alive() || battle_->untargetable(auto_target_)) {
    auto_target_ = -1;
    // Finish the weakest enemy first.
    int lowest = 1 << 30;
    for (int i = battle::kPcSlots; i < battle::kUnits; ++i)
      if (battle_->unit(i).alive() && !battle_->untargetable(i) && battle_->unit(i).hp < lowest) lowest = battle_->unit(i).hp, auto_target_ = i;
  }
  int hp = 0, mx = 0;
  for (int i = 0; i < battle::kPcSlots; ++i)
    if (battle_->unit(i).present) hp += std::max(0, battle_->unit(i).hp), mx += battle_->unit(i).max_hp;
  battle::Command cmd;
  cmd.actor = active_pc_;
  cmd.target = auto_target_;
  // A weak hit must be payable to open the HIT% menu.
  const int weak_payable = battle_->chain_cost(0, unit) <= unit.stamina ? 0 : -1;
  if (auto_target_ < 0) {
    cmd.kind = battle::Command::Kind::Defend;
  } else if (weak_payable < 0) {
    cmd.kind = battle::Command::Kind::Defend;
    auto_pc_ = -1;
  } else if (hp * 100 < mx * 12 && unit.stamina < 30 && !std::getenv("OC_BATTLE_NORUN")) {
    // Last resort; OC_BATTLE_NORUN=1 fights to the end instead.
    cmd.kind = battle::Command::Kind::Run;
  } else {
    // Auto-battle: Attack on the remembered target, then the HIT% menu runs auto_chain() for every hit until the stamina is spent.
    if (active_pc_ < battle::kPcSlots) tgt_mem_[active_pc_] = auto_target_;
    auto_delay_ = 20;
    begin_engage(active_pc_, auto_target_);
    return;
  }
  auto_delay_ = 20;
  execute(cmd);
}

// The HIT% menu with Auto on: the pick (strongest hit above 79 % whose stamina need is met, else weak) every time the menu opens, until
// weak cannot be paid (stamina < 10, or a dead target: update() then leaves the menu); then the PC returns and auto_pick() rotates.
void BattleView::auto_chain() {
  if (auto_delay_ > 0) {
    --auto_delay_;
    return;
  }
  const int hit_level = battle_->auto_hit_level(active_pc_, engaged_target_, true);
  auto_delay_ = 12;
  if (hit_level < 0) {
    auto_pc_ = -1;
    begin_disengage();
    ui_ = Ui::Wait;
    return;
  }
  chain_pick(hit_level);
}

void BattleView::handle_input(const platform::Input& in) {
  auto key = [&](int scancode) { return in.key_pressed[static_cast<size_t>(scancode)]; };
  auto pad = [&](int button) { return in.pad_pressed[static_cast<size_t>(button)]; };
  // Camera debug overlay: F9 in the game, F3 too in ocbattleview.
  if (key(SDL_SCANCODE_F9) || (dbg_.overlay_key && key(SDL_SCANCODE_F3))) dbg_.overlay = !dbg_.overlay;
  {
    // Pad input selects pad glyphs, any key press keyboard glyphs (keyboard wins when both happen in one frame).
    bool any_pad = false, any_key = false;
    for (bool pressed : in.pad_pressed) any_pad = any_pad || pressed;
    for (bool pressed : in.key_pressed) any_key = any_key || pressed;
    if (any_pad) kb_mode_ = false;
    if (any_key) kb_mode_ = true;
  }
  using platform::Action;
  auto act = [&](Action action) { return platform::action_pressed(in, action); };
  const bool ok = act(Action::Ok) || pad(SDL_GAMEPAD_BUTTON_SOUTH);
  const bool cancel = act(Action::Cancel) || pad(SDL_GAMEPAD_BUTTON_EAST);
  const bool up = act(Action::Up) || pad(SDL_GAMEPAD_BUTTON_DPAD_UP);
  const bool down = act(Action::Down) || pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
  const bool left = act(Action::Left) || pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
  const bool right = act(Action::Right) || pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  // In the top command menu Right / Square / R1 = next PC, Left / L1 = previous PC.
  const int swap = (act(Action::SelectRight) || act(Action::Item) || pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || pad(SDL_GAMEPAD_BUTTON_WEST) || right ? 1 : 0) -
                   (act(Action::SelectLeft) || pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || left ? 1 : 0);
  // PS1 Triangle: camera mode in target selection.
  const bool triangle = act(Action::Menu) || pad(SDL_GAMEPAD_BUTTON_NORTH);
  // A running overlay reads pad 0's new presses.
  if (ok || auto_confirm_) fx_pad_edge_ |= 0x20;

  // The cast header float steps once per tick while the Element command is in target selection / waiting for its action (cast_header.h).
  if (hdr_on_) {
    if (ui_ == Ui::Target || ui_ == Ui::Wait || ui_ == Ui::Dopple) hdr_.step(60 + 15 * (hdr_cell_ % data::kGridCols));
    // Back in the list or a menu: the float restarts at the next confirm.
    else hdr_on_ = false;
  }
  if (ui_ == Ui::Intro) return;
  // A battle-script message window takes the buttons.
  if (smsg_input(ok || cancel || auto_confirm_)) return;
  if (ui_ == Ui::Result) {
    if ((ok || auto_confirm_) && result_age_ > 0.4f) {
      audio_.stop_music();
    audio_.stop_sounds();
      finished_ = true;
    }
    return;
  }
  if (ui_ == Ui::Wait) return;
  // Op AF: no command input until the script unlocks the menu.
  if (ui_ == Ui::Top && battle_->menu_locked()) return;

  // The acting PC: first one with an open menu; Q / E (or the shoulders) switch between ready PCs in the top menu.
  auto ready = [&](int pc_slot) { return battle_->can_act(pc_slot); };
  if (active_pc_ < 0 || !ready(active_pc_)) {
    // The slot after the last actor first, the actor itself last.
    active_pc_ = next_pc_after(last_actor_);
    if (active_pc_ < 0) {
      // While the menu is disabled (nobody can act) the chime is armed ...
      menu_chime_ = true;
      return;
    }
    ui_ = Ui::Top;
    chain_.clear();
    // ... and SFX 0x16 plays once when the menu is enabled again (battle start ~1.4 s in, after a stamina wait). A
    // weak hit that leaves the PC engaged or kills the target re-opens the menu without it.
    if (menu_chime_) {
      menu_chime_ = false;
      audio_.play_menu(0x16);
    }
  }
  if (auto_ && ui_ == Ui::Top) {
    auto_pick();
    return;
  }
  if (auto_ && ui_ == Ui::Chain) {
    auto_chain();
    return;
  }
  if (debug_ui_ != 0 && debug_ui_ != 7 && ui_ == Ui::Top) {
    // Open every grid row.
    if (debug_ui_ != 2) battle_->unit_mut(active_pc_).gauge = battle_->unit(active_pc_).gauge_max;
    pending_cmd_ = {};
    pending_cmd_.actor = active_pc_;
    // Run away.
    if (debug_ui_ == 5) {
      debug_ui_ = 0;
      pending_cmd_.kind = battle::Command::Kind::Run;
      execute(pending_cmd_);
      return;
    }
    // Knock out the whole party.
    if (debug_ui_ == 6) {
      debug_ui_ = 0;
      for (int i = 0; i < battle::kPcSlots; ++i) {
        battle::Unit& pu = battle_->unit_mut(i);
        pu.hp = 0;
        pu.lo |= battle::st::kKO;
      }
      battle_->check_end();
      collect_events();
      return;
    }
    // Cast the first usable damage element of any ready PC (else the first usable one) at once.
    if (debug_ui_ == 4) {
      int pick = -1, pc = active_pc_;
      for (int slot = 0; slot < battle::kPcSlots && pick < 0; ++slot) {
        if (!battle_->can_act(slot)) continue;
        battle_->unit_mut(slot).gauge = battle_->unit(slot).gauge_max;
        for (int cell = 0; cell < data::kGridCells && pick < 0; ++cell)
          if (battle_->element_usable(slot, cell)) {
            const int id = battle_->unit(slot).cells[static_cast<size_t>(cell)] & 0x3FFF;
            if (id > 0 && static_cast<size_t>(id) < tables_->elements.size() && tables_->elements[static_cast<size_t>(id)].mode() == 0) {
              pick = cell;
              pc = slot;
            }
          }
      }
      active_pc_ = pc;
      pending_cmd_.actor = pc;
      for (int cell = 0; cell < data::kGridCells && pick < 0; ++cell)
        if (battle_->element_usable(pc, cell)) pick = cell;
      debug_ui_ = 0;
      if (pick >= 0) {
        pending_cmd_.kind = battle::Command::Kind::Element;
        pending_cmd_.cell = pick;
        tgt_mode_ = target_mode(pick);
        targets_ = target_candidates();
        if (!targets_.empty()) {
          pending_cmd_.target = targets_.front();
          execute(pending_cmd_);
        }
      }
      return;
    }
    // The creature list of the first PC that has element 238 on its grid (ocbattleview --ui 8 --party 26).
    if (debug_ui_ == 8) {
      pending_cmd_.kind = battle::Command::Kind::Element;
      for (int cell = 0; cell < data::kGridCells; ++cell)
        if ((battle_->unit(active_pc_).cells[static_cast<size_t>(cell)] & 0x3FFF) == 0xEE) {
          cell_ = pending_cmd_.cell = cell;
          break;
        }
      debug_ui_ = 0;
      // As the cell confirm: the banner floats above the list.
      hdr_on_ = true, hdr_cell_ = cell_, hdr_.reset(0);
      open_dopple();
      return;
    }
    if (debug_ui_ == 2) {
      pending_cmd_.kind = battle::Command::Kind::Attack;
      chain_.clear();
      chain_cursor_ = 0;
      enter_target(Tgt::Enemy);
    } else {
      pending_cmd_.kind = battle::Command::Kind::Element;
      cell_ = std::max(0, elem_open_cell(battle_->unit(active_pc_), tables_->elements.size()));
      ui_ = Ui::Element;
      if (debug_ui_ == 3) {
        pending_cmd_.cell = cell_;
        hdr_on_ = true, hdr_cell_ = cell_, hdr_.reset(0);
        enter_target(target_mode(cell_));
      }
    }
    debug_ui_ = 0;
    return;
  }
  // Switching PC plays SFX 0x16, restarts the idle script and restores the cursor memory.
  if (ui_ == Ui::Top && swap != 0) switch_active_pc(swap);
  // The next opening slides in again.
  if (ui_ != Ui::Element) elem_shown_ = -1, elem_state_ = 0;
  const battle::Unit& unit = battle_->unit(active_pc_);
  auto step = [](int& cursor, int count, int dir) { cursor = count > 0 ? ((cursor + dir) % count + count) % count : 0; };
  const int vdir = (down ? 1 : 0) - (up ? 1 : 0);
  const int hdir = (right ? 1 : 0) - (left ? 1 : 0);

  switch (ui_) {
    case Ui::Top:
      step(top_cursor_, 4, vdir);
      // Menu SFX 1 is the cursor tick.
      if (vdir != 0) audio_.play_menu(1);
      if (!ok) break;
      if (cursor_memory_ && active_pc_ >= 0 && active_pc_ < battle::kPcSlots) top_mem_[active_pc_] = top_cursor_;
      // Attack / Element confirmed (the element row may still be refused with 0x18 below).
      if (top_cursor_ <= 1) audio_.play_menu(8);
      pending_cmd_ = {};
      pending_cmd_.actor = active_pc_;
      if (top_cursor_ == 0) {
        pending_cmd_.kind = battle::Command::Kind::Attack;
        chain_.clear();
        // Target first, the PC runs up to it, then the hit percentages.
        enter_target(Tgt::Enemy);
      } else if (top_cursor_ == 1) {
        pending_cmd_.kind = battle::Command::Kind::Element;
        cell_ = elem_open_cell(unit, tables_->elements.size());
        // The row is dim (gauge below level 1 / nothing selectable): the menu does not open.
        if (cell_ < 0 || unit.stamina <= 0) {
          cell_ = 0;
          audio_.play_menu(0x18);
          break;
        }
        ui_ = Ui::Element;
      } else if (top_cursor_ == 2) {
        pending_cmd_.kind = battle::Command::Kind::Defend;
        execute(pending_cmd_);
      } else {
        pending_cmd_.kind = battle::Command::Kind::Run;
        execute(pending_cmd_);
      }
      break;
    // 1 / 2 / 3 hit (percentages) + Element, with the PC standing at the enemy; each pick executes at once.
    case Ui::Chain: {
      step(chain_cursor_, 4, vdir);
      if (vdir != 0) audio_.play_menu(1);
      // Leave: the PC returns, back to the command menu.
      if (cancel) {
        audio_.play_menu(6);
        begin_disengage();
        ui_ = Ui::Wait;
        break;
      }
      if (!ok) break;
      if (chain_cursor_ == 3) {
        const int first = elem_open_cell(unit, tables_->elements.size());
        // Dim row: nothing selectable.
        if (first < 0 || unit.stamina <= 0) {
          audio_.play_menu(0x18);
          break;
        }
        audio_.play_menu(8);
        pending_cmd_ = {};
        pending_cmd_.actor = active_pc_;
        pending_cmd_.kind = battle::Command::Kind::Element;
        cell_ = first;
        ui_ = Ui::Element;
      } else if (battle_->chain_cost(chain_cursor_, unit) <= unit.stamina) {
        // A hit row plays SFX 0x27 - row.
        audio_.play_menu(0x27 - chain_cursor_);
        chain_pick(chain_cursor_);
      } else {
        audio_.play_menu(0x18);
      }
      break;
    }
    case Ui::Element: {
      // The plates are sliding: no input.
      if (!elem_slide_step(cell_ / data::kGridCols)) break;
      // Left / Right = previous / next pickable level (wrap, levels above the gauge or without a valid cell are skipped),
      // the slot index is kept and snapped to the nearest valid slot (up first); Up / Down = previous / next valid slot over the ten slots with
      // wrap (used cells are valid); confirm on a used cell is refused with SFX 0x18.
      const size_t nel = tables_->elements.size();
      int level = cell_ / data::kGridCols, slot = cell_ % data::kGridCols;
      if (hdir != 0) {
        const int nl = elem_step_level(unit, nel, level, hdir);
        if (nl >= 0) {
          level = nl;
          slot = std::max(0, elem_near_slot(unit, nel, level, slot));
        }
      } else if (vdir != 0) {
        const int ns = elem_step_slot(unit, nel, level, slot, vdir);
        if (ns >= 0) slot = ns;
      }
      // The grid cursor tick.
      if (level * data::kGridCols + slot != cell_) audio_.play_menu(1);
      cell_ = level * data::kGridCols + slot;
      if (cancel) {
        audio_.play_menu(6);
        ui_ = engaged_ ? Ui::Chain : Ui::Top;
        break;
      }
      if (ok) {
        if (elem_valid(unit, nel, level, slot) && battle_->element_usable(active_pc_, cell_)) {
          // The cell confirmed.
          audio_.play_menu(0x28);
          pending_cmd_.cell = cell_;
          // Element 0xEE opens the creature list, no target ring.
          if ((unit.cells[static_cast<size_t>(cell_)] & 0x3FFF) == 0xEE) {
            // The header float runs as for any element: the element-name
            // banner floats to (8, 22) above the open list.
            hdr_on_ = true, hdr_cell_ = cell_, hdr_.reset((tick_count_ >> 2) & 3);
            open_dopple();
            break;
          }
          // The header float takes over the drawing of the plate (starting from the list's pulse index).
          hdr_on_ = true;
          hdr_cell_ = cell_;
          hdr_.reset((tick_count_ >> 2) & 3);
          enter_target(target_mode(cell_));
        } else {
          audio_.play_menu(0x18);
        }
      }
      break;
    }
    // The cursor (col, row, first row) walks the 2 x 6 window, empty cells too
    // (confirm refuses an empty entry), and the window scrolls one row:
    case Ui::Dopple: {
      //  Right: col 0 -> 1; col 1 -> col 0 of the next row; past row 5 -> col 0, row 5 and the scroll step below.
      //  Down: row + 1; past row 5 -> row 5 and the scroll step.
      //  scroll step: if (named - 11) / 2 < top + 1 nothing more; else top + 1, at most dop_max_.
      //    `named` counts the finished inline names, so once the names are in the gate is (names - 11) / 2 = the last row that
      //    leaves no empty row: max top = (n + 1 - 11) / 2 for n creatures.
      //  Left: col 1 -> 0; col 0 -> col 1 and the Up step.  Up: row - 1; at row 0 top - 1; at top 0 nothing.
      // Example (32 pool bits): Down x 18 from the top: row 0..5, then top 1..11 one per press, top stays 11 = (33 - 11) / 2;
      // Right at (1, 5) top 11 -> (0, 5); Up walks back row 5..0 then top 11..0. With all 256 bits the byte count wraps to 0: no scroll.
      dop_type_step();
      const int entry_count = static_cast<int>(dop_list_.size());
      const int before = dop_top_ * 2 + dop_cur_;
      int col = dop_cur_ & 1, row = dop_cur_ >> 1;
      auto scroll_down = [&] {
        row = 5;
        if ((dop_named_ - 11) / 2 < dop_top_ + 1) return;
        dop_top_ = std::min(dop_top_ + 1, dop_max_);
      };
      auto step_up = [&] {
        if (row > 0) --row;
        else if (dop_top_ > 0) --dop_top_;
      };
      if (hdir > 0) {
        if (col == 0) {
          col = 1;
        } else {
          col = 0;
          if (row < 5) ++row;
          else scroll_down();
        }
      } else if (vdir > 0) {
        if (row < 5) ++row;
        else scroll_down();
      } else if (hdir < 0) {
        if (col == 1) col = 0;
        else col = 1, step_up();
      } else if (vdir < 0) {
        step_up();
      }
      dop_cur_ = row * 2 + col;
      if (dop_top_ * 2 + dop_cur_ != before) audio_.play_menu(1);
      const int pick = dop_top_ * 2 + dop_cur_;
      // Cancel: back to the element list.
      if (cancel) {
        audio_.play_menu(6);
        ui_ = Ui::Element;
        break;
      }
      if (!ok) break;
      // Confirm: an entry (not an empty cell) is taken unless it is the character's own form while the character already wears it.
      // A pool slot past the 220-row creature table (forged saves only) is refused here as well.
      if (pick >= entry_count || (pick == 0 && unit.form < 0) || (dop_list_[static_cast<size_t>(pick)] >= static_cast<int>(battle::kDoppleTable.size()))) {
        audio_.play_menu(0x18);
        break;
      }
      audio_.play_menu(0x28);
      pending_form_ = dop_list_[static_cast<size_t>(pick)];
      pending_form_unit_ = active_pc_;
      // The header floats since the cell confirm and keeps floating.
      if (!hdr_on_) hdr_on_ = true, hdr_cell_ = cell_, hdr_.reset((tick_count_ >> 2) & 3);
      // The caster only.
      pending_cmd_.target = active_pc_;
      execute(pending_cmd_);
      break;
    }
    case Ui::Target:
      if (tgt_mode_ == Tgt::Enemy || tgt_mode_ == Tgt::Ally) {
        const int before = target_cursor_;
        step(target_cursor_, static_cast<int>(targets_.size()), vdir + hdir);
        // The target cursor tick.
        if (target_cursor_ != before) audio_.play_menu(1);
      }
      // Triangle in the attack / element target selection flips the camera mode (NORMAL <-> DYNAMIC), ends the running camera shot and plays
      // SFX 0x16; the menu camera glides to the new mode's shot.
      if (triangle) {
        cam_dynamic_ = !cam_dynamic_;
        camera_.kill_shot();
        audio_.play_menu(0x16);
      }
      if (cancel) {
        audio_.play_menu(6);
        hdr_on_ = false;
        ui_ = pending_cmd_.kind == battle::Command::Kind::Element ? Ui::Element : Ui::Top;
        pending_cmd_.chain.clear();
        break;
      }
      if (ok && !targets_.empty()) {
        audio_.play_menu(0x28);
        pending_cmd_.target = targets_[static_cast<size_t>(target_cursor_)];
        // The caster remembers its confirmed target.
        if (active_pc_ >= 0 && active_pc_ < battle::kPcSlots) tgt_mem_[active_pc_] = pending_cmd_.target;
        if (pending_cmd_.kind == battle::Command::Kind::Attack) begin_engage(active_pc_, pending_cmd_.target);
        else {
          if (engaged_) engaged_after_cast_ = true;
          execute(pending_cmd_);
        }
      }
      break;
    default: break;
  }
}

// ------------------------------------------------------------------------------------------------------------ menus
// Run every time the HIT% menu is (re)opened and its percentages are recomputed: with the
// option "Accuracy Selection" = Auto the cursor jumps to the
// strongest hit k = 2, 1, 0 whose chance is above 79 % and whose stamina need is met: 10 * (k + 1), or when fatigued
// (status 0x2000) 35 / 22 / 10 (1000 / 40 + 10, 500 / 40 + 10, 10). No such strength: the cursor stays where it is.
// With the option on Manual nothing happens. The rule lives in Battle::auto_hit_level.
void BattleView::chain_auto_cursor() {
  if (!auto_accuracy_) return;
  const battle::Unit& unit = battle_->unit(active_pc_);
  const int hit_level = battle_->auto_hit_level(active_pc_, engaged_target_, false);
  if (hit_level >= 0) {
    chain_cursor_ = hit_level;
    return;
  }
  // Never rest on a row that cannot be paid.
  while (chain_cursor_ > 0 && battle_->chain_cost(chain_cursor_, unit) > unit.stamina) --chain_cursor_;
}

// The PS1 battle window (the HD 'win1' skin = battle_04.tim, 64 x 16 PS1 px,
// drawn at 4 HD px per PS1 px): around an interior rect the eight skin pieces of 8 px (u 0..16 top / bottom edge halves, 16..24 left, 24..32 right,
// 32..40 / 40..48 the corner pairs, v 0..8 upper / 8..16 lower) and the interior tile (u 48..64). A primitive with the semi-transparency flag is drawn
// subtractive (ABR 2, 0x40 in the tpage word) only on the texels with the STP bit (the grey 156 fill and edge bodies: B - 156); the others (the 131
// grey frame line and the 222 / 205 / 205 highlight) are opaque. Outer rect in canvas units.
void BattleView::add_skin_box(float x, float y, float w, float h) {
  // Shared edge coordinates (the neighbouring quads use the very same floats, no rounding seam) and UVs inset by half a texel (the sampler is linear: a quad
  // must not blend in the texels of the next tile, which are the opaque frame lines / the other STP class and showed as thin gaps in the fill).
  const float ex = 8 * kPs1Px, ey = 8 * kRowScale, x0 = x, x1 = x + ex, x2 = x + w - ex, x3 = x + w, y0 = y, y1 = y + ey, y2 = y + h - ey, y3 = y + h;
  struct Piece {
    float x, y, w, h;
    // Texels / 256, 64.
    float u0, v0, u1, v1;
  };
  constexpr float hu = 0.5f / 256.0f, hv = 0.5f / 64.0f;
  auto pc = [&](float ax, float ay, float bx, float by, float tu0, float tv0, float tu1, float tv1) {
    return Piece{ax, ay, bx - ax, by - ay, tu0 + hu, tv0 + hv, tu1 - hu, tv1 - hv};
  };
  const Piece pieces[9] = {pc(x1, y1, x2, y2, 192 / 256.f, 0, 1, 1),
                           pc(x1, y0, x2, y1, 0, 0, 64 / 256.f, 0.5f),
                           pc(x1, y2, x2, y3, 0, 0.5f, 64 / 256.f, 1),
                           pc(x0, y1, x1, y2, 64 / 256.f, 0, 96 / 256.f, 1),
                           pc(x2, y1, x3, y2, 96 / 256.f, 0, 128 / 256.f, 1),
                           pc(x0, y0, x1, y1, 128 / 256.f, 0, 160 / 256.f, 0.5f),
                           pc(x2, y0, x3, y1, 160 / 256.f, 0, 192 / 256.f, 0.5f),
                           pc(x0, y2, x1, y3, 128 / 256.f, 0.5f, 160 / 256.f, 1),
                           pc(x2, y2, x3, y3, 160 / 256.f, 0.5f, 192 / 256.f, 1)};
  // The interior is drawn as the same subtractive grey as the status panels' base (waku-99: B - 153) from a flat white texture, whatever classes the skin
  // texture's texels fall in (a replaced battle_04 without the STP bit would otherwise make the fill opaque).
  add_img(white_, pieces[0].x, pieces[0].y, pieces[0].w, pieces[0].h, 0, 0, 1, 1, 0xFF999999u, 3);
  for (size_t i = 1; i < 9; ++i) add_img(hud_[kHSkinSemi], pieces[i].x, pieces[i].y, pieces[i].w, pieces[i].h, pieces[i].u0, pieces[i].v0, pieces[i].u1, pieces[i].v1, 0xFFFFFFFFu, 3);
  for (size_t i = 1; i < 9; ++i) add_img(hud_[kHSkinOpaque], pieces[i].x, pieces[i].y, pieces[i].w, pieces[i].h, pieces[i].u0, pieces[i].v0, pieces[i].u1, pieces[i].v1);
}

void BattleView::build_command_ui() {
  const battle::Battle& battle_state = *battle_;
  const int li = std::clamp(static_cast<int>(lang_), 0, 5);
  const bool jp = li == 5;
  // While an element plays, its name plate stays on screen at the top-left with the tech title
  // (style 0x80081 = 0x81 on layer 1, centred, colour 0x757575, y = plate + 3).
  // Header plate art: the element colour's battle_08_<T>_3 plate (the same 120 x 16 sprite as the list's idle
  // plates) at PS1 (8, 22), or - for an item-like action (mode 5) - the battle_08_93 plate whatever the colour.
  auto header_plate = [&](const data::ElemRec& er, float y) {
    if (er.mode() == 5) {
      add_img(eplate_item_, 8 * kPs1Px, y * kRowScale, 120 * kPs1Px, 16 * kRowScale);
      return;
    }
    const int ci = data::colour_index_of_bit(er.colour_bit());
    const int pc = ci >= 0 && ci < 6 ? ci : (er.colour_bit() & 0xFC) == 0xFC ? 6 : -1;
    // The 140-plate table: unused set, frame 3 (steady).
    if (pc >= 0) add_img(eplate_[static_cast<size_t>(pc * 10 + 2)], 8 * kPs1Px, y * kRowScale, 120 * kPs1Px, 16 * kRowScale);
  };
  const bool act_hdr = act_.phase != Act::Phase::None && !act_.ev.empty() && act_.ev.front().kind == battle::ActionEvent::Kind::Element && act_.actor >= 0;
  // Any unit: the action covers PCs and enemies. An element with record byte 0 bit 0x80 (death casts, form changes, boss moves such as the
  // Dragon God's dive) shows no header; one without a colour byte shows no title.
  if (act_hdr) {
    // The action start hands the same plate over, fixed at (8, 22).
    hdr_on_ = false;
    const int id = act_.ev.front().element;
    if (id > 0 && static_cast<size_t>(id) < tables_->elements.size() && !(tables_->elements[static_cast<size_t>(id)].b(0) & 0x80)) {
      header_plate(tables_->elements[static_cast<size_t>(id)], 22);
      // Header: title text x 0x44, y = plate + 3: name + "%+d".
      if (battle_state.element_colour(id) & 0xFC)
        btext(0x81, import::utf8_to_u32(elem_label(tables_->element_name(id), tables_->elements[static_cast<size_t>(id)], act_.ev.front().row)),
              0x44, 22 + 3, 0x757575);
    }
  } else if (hdr_on_ && hdr_cell_ >= 0 && hdr_cell_ < data::kGridCells && active_pc_ >= 0) {
    // Cast header float: from the confirm in the list the chosen plate floats to (8, 22) and stays through the
    // target ring; in state 0 the other plates of the level fold away.
    const battle::Unit& cu = battle_state.unit(active_pc_);
    const int id = battle_state.cell_element(active_pc_, hdr_cell_);
    if (id > 0 && static_cast<size_t>(id) < tables_->elements.size()) {
      const int row_y = 60 + 15 * (hdr_cell_ % data::kGridCols);
      const float y = static_cast<float>(hdr_.plate_y(row_y));
      if (hdr_.state == 0 && hdr_.fold_frame() > 0) {
        for (const int idx : level_slots(hdr_cell_ / data::kGridCols)) {
          if (idx == hdr_cell_) continue;
          const data::ElemRec& oe = tables_->elements[static_cast<size_t>(battle_state.cell_element(active_pc_, idx))];
          const int oci = data::colour_index_of_bit(oe.colour_bit());
          const int opc = oci >= 0 && oci < 6 ? oci : (oe.colour_bit() & 0xFC) == 0xFC ? 6 : -1;
          const bool used = cu.used[static_cast<size_t>(idx)] || battle_state.element_sealed(active_pc_, idx);
          if (opc >= 0)
            add_img(eplate_[static_cast<size_t>((used ? 70 : 0) + opc * 10 + hdr_.fold_frame() - 1)], 8 * kPs1Px, (60.0f + 15.0f * static_cast<float>(idx % data::kGridCols)) * kRowScale, 120 * kPs1Px, 16 * kRowScale);
        }
      }
      header_plate(tables_->elements[static_cast<size_t>(id)], y);
      btext(0x81, import::utf8_to_u32(elem_label(tables_->element_name(id), tables_->elements[static_cast<size_t>(id)], hdr_cell_ / data::kGridCols)), 0x44, y + 3, 0x757575);
    }
  }
  if (active_pc_ >= 0 && ui_ == Ui::Dopple) build_dopple_ui();
  if (active_pc_ < 0 || !(ui_ == Ui::Top || ui_ == Ui::Chain || ui_ == Ui::Element || ui_ == Ui::Target)) return;
  const battle::Unit& unit = battle_state.unit(active_pc_);
  // Command window (white-bordered black box at PS1 (28, 177), 68 x 54; rows 11 px apart, red diamond cursor). The box is the PS1's
  // own line box in both art modes. Top: Attack / Element / Defend / Run Away (sheet 0x04 g1..4,
  // XLAT 0x10002 -> style 2, gn_70 x 0.425). Engaged (the PC stands at its target): "1-(92%)" "2-(79%)" "3-(69%)" + Element, with the "HIT%"
  // caption; a strength the stamina cannot pay is dimmed.
  const size_t cmd0 = op_count();
  if (ui_ == Ui::Top || ui_ == Ui::Chain) {
    // A pickable level exists (used cells count).
    const bool any_element = elem_open_level(unit, tables_->elements.size()) >= 0;
    // Row colours (PS1 colour words 0x808080 white / 0x8080 yellow / 0x404040 dim, remapped for the HD text):
    //  top menu: Element dim when the gauge is below 0 or no cell is usable; the cursor row is not recoloured.
    //  chain menu: by stamina / 10 and row -> dim when the row's cost exceeds the whole stamina points, YELLOW for the row whose
    //  cost equals them (the last affordable hit), white below; fatigued (status 0x2000): thresholds 1.2 / 2.5 / 3.7 (dim below, yellow within
    //  +1.2, white above). Element: dim when gauge < 0 / stamina 0 / no usable cell.
    constexpr u32 kDim = 0x404040, kYellowWord = 0x008080, kWhiteWord = 0x808080;
    const float bx = 28 * kPs1Px, by = ref_y(177), bw = 68 * kPs1Px;
    // Interior 52 x 36 + the 8 px skin frame; 0.8 row shorter than the nominal 52 so the top and bottom padding round the text are equal (0.67 row pitch).
    add_skin_box(bx, by, bw, 51.2f * kRowScale);
    const int cur = ui_ == Ui::Top ? top_cursor_ : chain_cursor_;
    const bool fatigued = (unit.lo & battle::st::kFatigued) != 0;
    // The translated text clears bit 16; JP keeps the JP font.
    const int cmd_flags = jp ? 0x10002 : 2;
    for (int k = 0; k < 4; ++k) {
      // PS1 line of the row (+1.1: top pad = bottom pad with the 0.8 row shorter box).
      const float yl = (182 + 11.0f * static_cast<float>(k)) * kRefLineY + 1.1f;
      const bool sel = k == cur;
      u32 row_colour = kWhiteWord;
      std::u32string label;
      if (ui_ == Ui::Top) {
        label = kCommand[li][k];
        if (k == 1 && (!any_element || unit.gauge < 0)) row_colour = kDim;
      } else if (k < 3) {
        const int pct = battle_state.hit_chance_for(active_pc_, engaged_target_, k);
        label = import::utf8_to_u32(std::format("{}-({}%)", k + 1, pct));
        if (fatigued) {
          static const int kThr[3] = {12, 25, 37};
          row_colour = unit.stamina < kThr[k] ? kDim : unit.stamina < kThr[k] + 12 ? kYellowWord : kWhiteWord;
        } else {
          const int pts = std::clamp(unit.stamina / 10, 0, 7), cost = k + 1;
          row_colour = cost > pts ? kDim : cost == pts ? kYellowWord : kWhiteWord;
        }
      } else {
        label = kCommand[li][1];
        if (!any_element || unit.gauge < 0 || unit.stamina <= 0) row_colour = kDim;
      }
      // Red diamond cursor: battle_11.png {0, 0, 0.5625, 1}, 9 x 8 PS1 px.
      if (sel) {
        const float cx = 37 * kPs1Px, cy = (yl * kRowScale) + ref_y(5);
        add_img(hud_[kHDiamond], cx - 4.5f * kPs1Px, cy - 4 * kRowScale, 9 * kPs1Px, 8 * kRowScale, 0, 0, 0.5625f, 1);
      }
      btext(cmd_flags, label, 44, yl, remap_text_colour(row_colour));
    }
    if (ui_ == Ui::Chain) {
      // "HIT%" caption: battle-osak-010 (50 x 9 PS1 px leader line, drawn at (x, y - 8)) with the caption
      // (命中率 / "HIT%" per language, flags 0x10002, colour 0x757575) above it.
      add_img(hud_[kHOsak], 82 * kPs1Px, ref_y(170), 62 * kPs1Px, ref_y(11));
      btext(cmd_flags, kHitCaption[li], 93, 165 * kRefLineY, 0x757575);
    }
  }
  // WideHudEdges: the command box (PS1 x 28, HIT% caption to 144) moves 21 px further out so its left margin equals the field-effect ring's (7 PS1 px).
  set_group(cmd0, -1, -21 * kPs1Px);
  // Top menu: the command under the cursor as a headline (sheet 0x00 g1..4, style 0xA) right-aligned at PS1 (300, 22); Japanese shows the
  // English words. The right group.
  if (ui_ == Ui::Top) {
    const size_t caption0 = op_count();
    btext(0xA, jp ? kCommandCaptionJp[top_cursor_ & 3] : kCommand[li][top_cursor_ & 3], 300, 22, 0);
    set_group(caption0, 1);
    build_status_plates(active_pc_, true);
  }
  if (ui_ == Ui::Element) build_element_ui();
  if (ui_ == Ui::Target) build_target_ui();
}

// Target selection, identical in both art modes:
//  * cursor: for every unit of the target list a camera-facing quad at the model's attach point 14, the four constant vertices
//    (-192, -704, -192) (192, -704, -192) (192, -320, -192) (-192, -320, -192) (y up = negative, z toward the camera) through the perspective
//    projection: a 384 x 384 square from 320 to 704 units above the point, 192 units closer to the eye, sized by the perspective (H / z); textured
//    with the battle_21.dds strips 0 / 2 / 1 / 3 for (frame counter >> 2) & 3;
//  * info window: waku-03 (96 x 48, subtractive) at (6, 163); the target label: the unit name in
//    style 0x85 (centred on x 0x37, shrunk to 86 px, colour 0x8A8A8A) or XLAT(敵全体 / 味方全体) in 0x10085 colour 0x959595; single target:
//    "HP" (10, 0xB3) style 0x10004 and "/" (0x37, 0xBB) style 4, the four "?" glyphs of each unknown value at x 0x1F + 6 k / 0x3F + 6 k, y 0xBB
//    (style 2), the sex glyph (at (0x1E, 0xC5): XLAT(♂ / ♀) has no rows, so 0x10002 keeps the JP font, x - 12,
//    y + 1), XLAT("属性:") at (0x30, 0xC6) with the language nudge, and the innate "●" (colour dot at (0x48, 0xC5), x + 7, y + 2, style 0xB);
//  * camera hint: at (0xF6, 0xE) the "NORMAL" / "DYNAMIC" text at (+27, +3) (flags 0x20003, colour 0x757575) and the camera icon at (+15, 0),
//    plus the button glyph of the camera key (12 x 12 at (0xF6, 0xE)): the pad icon, or in keyboard mode the key cap of the bound key from
//    ui/keyboard_<l>.png.
void BattleView::build_target_ui(bool cam_hint) {
  const battle::Battle& battle_state = *battle_;
  if (targets_.empty()) return;
  // The name + HP block shows when the ring holds ONE living unit,
  // so a whole-range element on the last enemy shows that enemy; otherwise the side label picked by the element's target byte & 0xC0.
  const bool all = (tgt_mode_ == Tgt::EnemyAll || tgt_mode_ == Tgt::AllyAll) && targets_.size() > 1;
  bool both_sides = false;
  if (pending_cmd_.kind == battle::Command::Kind::Element && pending_cmd_.cell >= 0 && pending_cmd_.actor >= 0) {
    const int eid = battle_state.cell_element(pending_cmd_.actor, pending_cmd_.cell);
    if (eid > 0 && static_cast<size_t>(eid) < tables_->elements.size()) {
      const battle::TargetSpec sp = battle::target_spec(tables_->elements[static_cast<size_t>(eid)]);
      both_sides = sp.party && sp.enemies;
    }
  }
  const int li = std::clamp(static_cast<int>(lang_), 0, 5);
  const bool jp = li == 5;
  // The DYNAMIC target camera draws no target cursor unless the element takes the whole range (record byte 4 bit 0x10); element targeting
  // then shows no status plates either (Attack targeting keeps them).
  const bool dynamic_bare = cam_dynamic_ && tgt_mode_ != Tgt::EnemyAll && tgt_mode_ != Tgt::AllyAll;
  static const int kStrip[4] = {0, 2, 1, 3};
  const int strip = kStrip[(tick_count_ >> 2) & 3];
  for (size_t k = 0; k < targets_.size() && !dynamic_bare; ++k) {
    if (!all && static_cast<int>(k) != target_cursor_) continue;
    const int target = targets_[k];
    const Visual& visual = vis_[static_cast<size_t>(target)];
    const std::array<float, 3> world = unit_world(target, 1.0f);
    std::array<float, 3> point{world[0], world[1] + visual.height, world[2]};
    float mx = 0, my = 0, mz = 0;
    // Model space (y down) -> world, as anchor_frame().
    if (visual.has_model && visual.pose.attach_now(14, 1.0f, mx, my, mz)) {
      const float yaw = std::atan2(-visual.face_x, -visual.face_z), cs = std::cos(yaw), sn = std::sin(yaw);
      point = {world[0] + mx * cs + mz * sn, world[1] - my, world[2] - mx * sn + mz * cs};
    }
    const std::array<float, 3> to_eye{point[0] - cam_.eye[0], point[1] - cam_.eye[1], point[2] - cam_.eye[2]};
    const float zc = dot(cam_.fwd, to_eye) - 192.0f;
    if (zc < 1.0f) continue;
    // PS1 px per unit at the quad's depth.
    const float kk = cam_.h / zc;
    // GTE screen (offset (160, 112)); the quad is a HUD sprite, so it maps like
    // every HUD sprite: canvas x * kPs1Px, y * kRowScale (960 / 216), not through the picture's 224-row frame.
    const float cx = cam_.ox + cam_.h * dot(cam_.right, to_eye) / zc, cy = cam_.oy + cam_.h * dot(cam_.down, to_eye) / zc;
    // The quad sits on the 3D picture, so it takes the picture's own mapping (Cam::project: columns at kPs1Px, rows through the field crop / stretch),
    // not the 216-row HUD mapping: the same mapping puts it on the enemy in widescreen (x < 0 or > 1280 for the extra columns) and in 4:3.
    constexpr float kPicY = (render::kLayerH - 2.0f * render::kFieldCrop) / render::kFieldRows * (kCanvasH / render::kLayerH);
    constexpr float kPicOy = render::kFieldCrop * (kCanvasH / render::kLayerH);
    const float qx = (cx - 192.0f * kk) * kPs1Px, qy = kPicOy + (cy - 704.0f * kk) * kPicY;
    const float qw = 384.0f * kk * kPs1Px, qh = 384.0f * kk * kPicY;
    add_img(hud_[kHCursor], qx, qy, qw, qh, static_cast<float>(strip) * 0.25f + 0.0078125f, 0.04f, static_cast<float>(strip) * 0.25f + 0.25f, 0.96f);
    // Rides a unit of the 4:3 picture: never moved by the HUD edge groups.
    set_group(op_count() - 1, 2);
  }
  // info window
  add_img(hud_[kHWaku03], 6 * kPs1Px, 163 * kRowScale, 96 * kPs1Px, 48 * kRowScale, 0, 0, 1, 1, 0xFFFFFFFFu, 1);
  const int tu_i = targets_[static_cast<size_t>(std::clamp(target_cursor_, 0, static_cast<int>(targets_.size()) - 1))];
  const battle::Unit& tu = battle_state.unit(tu_i);
  // Side labels: 0xC0 = 敵・味方全体, 0x80 = 敵全体, 0x40 = 味方全体; translated: All Enemies/Allies | All Enemies | All Allies.
  if (all)
    btext(jp ? 0x10085 : 0x85,
          both_sides ? xlat("敵・味方全体", U"All Enemies/Allies") : tgt_mode_ == Tgt::AllyAll ? xlat("味方全体", U"All Allies") : xlat("敵全体", U"All Enemies"), 0x37,
          0xA9, 0x959595);
  else {
    // Script op 7D 1 shows the enemy's letter: "-A" outside JP, "・" + the full-width letter in JP.
    std::u32string nm = import::utf8_to_u32(tu.name);
    if (!tu.pc && tu.letter != 0 && brain_.vm() && brain_.vm()->menu_entry100()) {
      const char32_t letter = static_cast<char32_t>(U'A' + tu.letter - 1);
      nm += jp ? std::u32string(U"・") + static_cast<char32_t>(letter - U'A' + U'Ａ') : std::u32string(U"-") + letter;
    }
    btext(0x85, nm, 0x37, 0xA9, 0x8A8A8A);
  }
  // The HP / sex / innate block needs status lo 0x400, the HP digits status lo 0x400000. Every unit has 0x400,
  // so the block is always drawn; 0x400000 (HP known) is a PC's from the start and an enemy's only from its
  // record or after an Infoscope (element 66) landed. Known HP: current right-aligned in six 8 px slots from x 5,
  // max left-aligned from 0x3F, all in the HP colour (<= 1/4 max 0x4080, <= 1/8 max 0x3080, else 0x808080); else "????". The sex glyph is group
  // `U+0x14 >> 14` (1 male, 2 female; rolled at spawn when the record has none); a second dot at x 0x50 shows the colour of the colour shield (merged hi
  // & 0x3F, U+1B4).
  if (!all && (tu.pc || (tu.lo & 0x400u))) {
    btext(jp ? 0x10004 : 4, xlat("ＨＰ", U"HP"), 10, 0xB3, 0x8A8A8A);
    // HP known: every PC, an enemy only once an Infoscope landed on it.
    if (tu.pc || (tu.lo & 0x400000u)) {
      const int hp = std::clamp(tu.hp, 0, 99999), mhp = std::clamp(tu.max_hp, 0, 99999);
      const u32 hpc = hp_colour(hp, mhp);
      btext(4, U"/", 0x37, 0xBB, hpc);
      const std::u32string cur = digits_u32(hp), mx = digits_u32(mhp);
      for (size_t k = 0; k < cur.size(); ++k) btext(4, cur.substr(k, 1), 5 + 8.0f * static_cast<float>(6 - cur.size() + k), 0xBB, hpc);
      for (size_t k = 0; k < mx.size(); ++k) btext(4, mx.substr(k, 1), 0x3F + 8.0f * static_cast<float>(k), 0xBB, hpc);
    } else {
      btext(4, U"/", 0x37, 0xBB, 0x8A8A8A);
      // HP unknown until an Infoscope landed.
      for (int k = 0; k < 4; ++k) {
        btext(2, U"?", 0x1F + 6.0f * static_cast<float>(k), 0xBB, 0x8A8A8A);
        btext(2, U"?", 0x3F + 6.0f * static_cast<float>(k), 0xBB, 0x8A8A8A);
      }
    }
    const int sex = tu.gender >> 14;
    if (sex == 1 || sex == 2) btext(0x10002, sex == 2 ? U"♀" : U"♂", 0x1E - 12, 0xC5 + 1, 0x8A8A8A);
    btext(jp ? 0x10002 : 2, kInnateLabel[li], 0x30 + kInnateDx[li], static_cast<float>(0xC6) - (jp ? 0.0f : 1.0f), 0x8A8A8A);
    // The innate byte's colour mask is bits 2..7 (bit 1 = absorbs its own colour).
    const int ci = data::colour_index_of_bit(static_cast<u8>(tu.innate & 0xFC));
    // Style 0xB: the colour word is not remapped.
    if (ci >= 0 && ci < 6) btext(0xB, U"●", 0x48 + 7, 0xC5 + 2, kInnateDot[ci]);
    const int cs = data::colour_index_of_bit(static_cast<u8>((tu.hi & 0x3Fu) << 2));
    // The colour shield's dot uses the same +7 / +2 nudge.
    if (cs >= 0 && cs < 6) btext(0xB, U"●", 0x50 + 7, 0xC5 + 2, kInnateDot[cs]);
  }
  // Camera mode hint and the cursor unit's status plates: not shown under the Dopplegang list. An element's ring shows the plates only while
  // it holds one unit and the cursor is drawn (not under the DYNAMIC camera, above).
  if (!cam_hint) return;
  if (pending_cmd_.kind != battle::Command::Kind::Element || (!all && !dynamic_bare)) build_status_plates(tu_i, false);
  const size_t hint0 = op_count();
  {
    const float hx = 0xF6, hy = 0xE;
    // Keyboard: the key cap of the Triangle (Menu) key.
    if (kb_mode_) {
      const int cell = platform::keyboard_cell(platform::action_vk(platform::Action::Menu));
      add_img(hud_[kHKeys], hx * kPs1Px, hy * kRowScale, 12 * kPs1Px, 12 * kRowScale, static_cast<float>(cell & 15) / 16.0f, static_cast<float>(cell >> 4) / 7.0f,
              static_cast<float>((cell & 15) + 1) / 16.0f, static_cast<float>((cell >> 4) + 1) / 7.0f);
    } else {
      // Pad: Triangle = Y (gamepad.png column 10, row 1).
      add_img(hud_[kHPad], hx * kPs1Px, hy * kRowScale, 12 * kPs1Px, 12 * kRowScale, 10.0f / 13.0f, 0.25f, 11.0f / 13.0f, 0.5f);
    }
    add_img(hud_[kHCam], (hx + 15) * kPs1Px, hy * kRowScale, 12 * kPs1Px, 12 * kRowScale);
    btext(0x20003, cam_dynamic_ ? kCamDynamic[li] : kCamNormal[li], hx + 27, hy + 3, 0x757575);
  }
  // The top-right hint: the right group.
  set_group(hint0, 1);
}

std::vector<int> BattleView::level_slots(int level) const {
  std::vector<int> out;
  if (active_pc_ < 0 || level < 0 || level >= data::kGridRows) return out;
  const battle::Unit& unit = battle_->unit(active_pc_);
  for (int slot = 0; slot < data::kGridCols; ++slot) {
    const int idx = level * data::kGridCols + slot;
    const int id = unit.cells[static_cast<size_t>(idx)] & 0x3FFF;
    if (id != 0 && static_cast<size_t>(id) < tables_->elements.size() && unit.usable[static_cast<size_t>(idx)]) out.push_back(idx);
  }
  return out;
}

// The battle element menu (PS1 px), the same art in both modes:
//  * grid minimap at (160 + 15 * level, 118 + 7.5 * slot) in measured lines, 13 x 6 cells = menuelem_N plates
//    in the element colour; the selected level's column blinks to the white plate, levels above the gauge are semi-transparent;
//  * the level's elements as name plates (battle-win4-large base + frame, 114 x 14 at x 11, one per valid slot at PS1 y 60 + 15 * slot, no
//    scrolling; coloured ends in the element colour; the selected slot's plate is the bright one); the names are tech-title entries (style
//    0x80081: 0x81 centred on the plate, y + 3; colour 0x757575, used cells 0x404040) with a "%+d" level-offset suffix;
//  * the description window at (160, 10) 152 x 52: the three sysmes 0x1A rows of the element (target type, description; style 1,
//    x 0xAB (JP 0xBB), y = row * 13 + 0x10, colour 0x757575).
//  No message window and no command box while it is open.
void BattleView::build_element_ui() {
  const battle::Battle& battle_state = *battle_;
  const battle::Unit& unit = battle_state.unit(active_pc_);
  const int sel_lvl = cell_ / data::kGridCols;
  // The plates on screen belong to the level the slide machine shows (elem_shown_: the old level while it slides out); the text, stock counts and the
  // selected highlight appear only once they rest (the slide machine is in state 0 and the shown level is the selected one).
  const int shown_lvl = elem_shown_ >= 0 ? elem_shown_ : sel_lvl;
  const bool rest = elem_state_ == 0 && shown_lvl == sel_lvl;
  const std::vector<int> slots = level_slots(shown_lvl);
  // Grid minimap: every cell of the battle copy as a menuelem plate at (160 + 15 level, 72 + 7 slot); the selected level's whole column
  // blinks to the white plate while (frame & 0x1f) > 0xf, levels above the gauge are drawn semi-transparent.
  const bool blink = (tick_count_ & 0x1f) > 0xf;
  // The minimap and the description window (PS1 x >= 158) form the right group.
  // One more level column appears per frame after opening, up to 8.
  for (int level = 0; level < std::min(data::kGridRows, elem_age_); ++level)
    for (int slot = 0; slot < data::kGridCols; ++slot) {
      const int idx = level * data::kGridCols + slot;
      const int id = unit.cells[static_cast<size_t>(idx)] & 0x3FFF;
      if (id == 0 || static_cast<size_t>(id) >= tables_->elements.size() || !unit.usable[static_cast<size_t>(idx)]) continue;
      const data::ElemRec& elem = tables_->elements[static_cast<size_t>(battle_state.cell_element(active_pc_, idx))];
      const int ci = blink && level == sel_lvl ? 4 : data::colour_index_of_bit(elem.colour_bit());
      const u8 a = level > unit.gauge ? 128 : 255;
      const float x = (160 + 15.0f * static_cast<float>(level)) * kPs1Px, y = ref_y(118 + 7.5f * static_cast<float>(slot));
      add_plate(ci, x, y, 13 * kPs1Px, ref_y(6), col(255, 255, 255, a));
      set_group(op_count() - 1, 1);
    }
  // Name list: one plate per valid slot of the level at PS1 (11, 60 + 15 slot); the selected slot's plate is the
  // bright one; names are the tech-title entries with the level offset suffix; greyed (0x404040) when the cell is used.
  for (const int idx : slots) {
    const int slot = idx % data::kGridCols;
    // A tech that a party member's tech completes shows (and casts) the double / triple tech instead.
    const int id = battle_state.cell_element(active_pc_, idx);
    const data::ElemRec& elem = tables_->elements[static_cast<size_t>(id)];
    const int ci = data::colour_index_of_bit(elem.colour_bit());
    // Used or colour-sealed = the greyed plate set + 0x404040 text.
    const bool used = unit.used[static_cast<size_t>(idx)] || battle_state.element_sealed(active_pc_, idx);
    const bool sel = idx == cell_ && rest;
    // PS1 line of the plate.
    const float row_y = 60.0f + 15.0f * static_cast<float>(slot);
    // The plate is the battle_08_<T>_<n>.png sprite (used cells: battle_76; 120 x 16 PS1 px at (8, row_y)): the element colour (+ 7 when the cell
    // is greyed) names the sprite set, the frame n: the selected plate pulses 1, 2, 3, 2 every 4 ticks, the others stay on 3.
    // Every colour (0xFC) uses the rainbow set.
    const int pc = ci >= 0 && ci < 6 ? ci : (elem.colour_bit() & 0xFC) == 0xFC ? 6 : -1;
    if (pc >= 0) {
      static const int kPulse[4] = {1, 2, 3, 2};
      // While sliding every plate (the selected one too) takes the slide table's frame at the step just taken, 4..10 = the unfolding frames.
      const int frame = sel ? kPulse[(tick_count_ >> 2) & 3] : elem_state_ == 1 || elem_state_ == 3 ? kSlideFrame[elem_idx_] : 3;
      add_img(eplate_[static_cast<size_t>((used ? 70 : 0) + pc * 10 + frame - 1)], static_cast<float>(elem_x_) * kPs1Px, row_y * kRowScale, 120 * kPs1Px, 16 * kRowScale);
    }
    if (!rest) continue;
    // The name's leading icon code (Greek alpha..delta) is drawn by the fonts as the list icon (alpha = the boxed "x1" of consumables).
    btext(0x81, import::utf8_to_u32(elem_label(tables_->element_name(id), elem, idx / data::kGridCols)), 11 + 57, row_y + 3, used ? 0x404040 : 0x757575);
    // Consumable stock (a cell without a counter, e.g. a debug party, shows none): "x" and the units digit as style-9 glyph text at
    // (x + 0x72 / 0x78, 63 + 15 slot + 2), 0x757575.
    if ((elem.flags0() & 0x10) && (unit.qty[static_cast<size_t>(idx)] > 0 || unit.used[static_cast<size_t>(idx)])) {
      btext(9, U"x", 122, row_y + 5, 0x757575);
      btext(9, digits_u32(std::clamp(static_cast<int>(unit.qty[static_cast<size_t>(idx)]), 0, 9)), 128, row_y + 5, 0x757575);
    }
  }
  // description window
  const size_t desc0 = op_count();
  if (!slots.empty()) {
    const int id = battle_state.cell_element(active_pc_, cell_);
    {
      // Window at (0x136, 10): X = x - 0x98 + 4 (JP: 16 px further right, the sprite keeps its 152 x 56 size), Y = y - 4 -> subtractive
      // base + frame; the JP rows sit at x 0xBB, 16 right of the others.
      const float dw = 152.0f, dx = (lang_ == import::Lang::Jp ? 310.0f + 16.0f : 310.0f) - dw + 4.0f;
      add_img(hud_[kHWin4Base], dx * kPs1Px, 6 * kRowScale, dw * kPs1Px, 56 * kRowScale, 0, 0, 1, 1, 0xFFFFFFFFu, 3);
      add_img(hud_[kHWin4Frame], dx * kPs1Px, 6 * kRowScale, dw * kPs1Px, 56 * kRowScale);
    }
    for (int row = 0; row < 3; ++row) {
      const std::string* text = lang_tables_.sysmes((0x1Au << 16) | static_cast<u32>(id * 3 + row), lang_);
      if (text && !text->empty()) btext(1, import::utf8_to_u32(*text), lang_ == import::Lang::Jp ? 187.0f : 171.0f, static_cast<float>(row * 13 + 0x10), 0x757575);
    }
  }
  set_group(desc0, 1);
}

// Dopplegang's creature list: a 208 x 96 window at PS1 (0x38, 0x34),
// two columns, six rows, the first entry the character's own form, then one entry per pool slot (ascending) named by sysmes table 0x19.
// The entries and the cursor:
//  * entry s (0..11 on screen) at x = 0x40 + (s & 1) * 0x66, y = 0x38 + (s >> 1) * 16, style 0xB8 (the default battle font slot, inline segments);
//  * colour: the first entry of the first page (the character) 0x008000 = green, every creature 0x808080 - the cursor entry is NOT recoloured;
//  * cursor: at (0x36 + col * 0x68, 0x3A + row * 16), battle_11.png halves (9 x 8 / 7 x 8, top-left
//    at the point), the two halves alternate every 16 frames;
//  * the frame = interior + 8 px skin;
//  * scroll triangles: when the scroll maximum != 0 and the byte creature count > 11, flat red (0xFF, 0, 0)
//    triangles (0x109, 0x38) (0x114, 0x38) (0x10F, 0x3E) pointing down at the top right and (0x10F, 0x90) (0x114, 0x96) (0x109, 0x96)
//    pointing up at the bottom right; top row 0: only the top one, top row max - 1 (the last): only the bottom one, else both.
//    (32 pool bits): the small red down-triangle at the window's top-right corner at top 0, both from top 1, only the bottom one at top 11; none with
//    all 256 bits (the count wraps to 0);
//  * the target info window + target cursor of the caster; the camera hint is skipped. Example: the caster's name, HP 999 / 999,
//    the female sign and INNATE green under the list.
std::u32string BattleView::dop_name(int index) {
  const battle::Battle& battle_state = *battle_;
  const int entry = dop_list_.at(static_cast<size_t>(index));
  std::string nm;
  if (entry < 0) {
    nm = static_cast<size_t>(active_pc_) < battle_state.party().size() ? battle_state.party()[static_cast<size_t>(active_pc_)].name : battle_state.unit(active_pc_).name;
  } else {
    // sysmes[0x190000 + low byte].
    const std::string* text = lang_tables_.sysmes((0x19u << 16) | static_cast<u32>(entry & 0xFF), lang_);
    nm = text && !text->empty() ? *text : tables_->enemy_name(entry);
  }
  return import::utf8_to_u32(nm);
}

// The list text is typed out: each pass uploads at most 4 glyphs (two passes every other frame); a finished inline name bumps the named
// count, the scroll gate. 33 names are all in after about 27 frames. The glyphs counted here are our names' code points.
void BattleView::dop_type_step() {
  const int name_count = static_cast<int>(dop_len_.size());
  for (int pass = 0; pass < ((tick_count_ & 1) ? 2 : 1); ++pass) {
    int glyphs = 0;
    while (glyphs < 4 && dop_typing_ < name_count) {
      if (dop_left_ > 0) {
        --dop_left_, ++glyphs;
      } else {
        ++dop_named_;
        if (++dop_typing_ < name_count) dop_left_ = dop_len_[static_cast<size_t>(dop_typing_)];
      }
    }
  }
}

void BattleView::build_dopple_ui() {
  constexpr float wx = 0x38, wy = 0x34, ww = 0xd0, wh = 0x60;
  // The rect is the INTERIOR; add_skin_box puts the 8 px skin outside it (222 x 110 PS1 px outer for the 208 x 96 rect, as the script
  // message window; 6 rows x 16 px fill the interior exactly).
  add_skin_box((wx - 8.0f) * kPs1Px, (wy - 8.0f) * kRowScale, (ww + 16.0f) * kPs1Px, (wh + 16.0f) * kRowScale);
  const int entry_count = static_cast<int>(dop_list_.size());
  const int cc = dop_cur_ & 1, row = dop_cur_ >> 1;
  if (((tick_count_ >> 4) & 1) == 0)
    add_img(hud_[kHDiamond], (0x36 + 0x68 * static_cast<float>(cc)) * kPs1Px, (0x3A + 16 * static_cast<float>(row)) * kRowScale, 9 * kPs1Px, 8 * kRowScale, 0, 0, 0.5625f, 1);
  else
    add_img(hud_[kHDiamond], (0x36 + 0x68 * static_cast<float>(cc)) * kPs1Px, (0x3A + 16 * static_cast<float>(row)) * kRowScale, 7 * kPs1Px, 8 * kRowScale, 0.5625f, 0, 0.4375f, 1);
  for (int screen_slot = 0; screen_slot < 12; ++screen_slot) {
    const int index = dop_top_ * 2 + screen_slot;
    if (index >= entry_count) break;
    const bool own = dop_top_ == 0 && screen_slot == 0;
    btext(0xB8, dop_name(index), static_cast<float>(0x40 + (screen_slot & 1) * 0x66), static_cast<float>(0x38 + (screen_slot >> 1) * 16), own ? 0x008000u : 0x808080u);
  }
  // The flat triangles as one-canvas-row spans (PS1 x from the edges' interpolation).
  auto tri = [&](float y0, float y1, bool down) {
    const float ty = std::floor(y0 * kRowScale), by = std::ceil(y1 * kRowScale);
    for (float cy = ty; cy < by; cy += 1.0f) {
      // frac: 0 at y0 .. 1 at y1; width: the base at y0 (down) or at y1 (up).
      const float frac = (cy + 0.5f - y0 * kRowScale) / ((y1 - y0) * kRowScale);
      const float width = down ? 1.0f - frac : frac;
      const float left = 0x10F - (0x10F - 0x109) * width, right = 0x10F + (0x114 - 0x10F) * width;
      if (right > left) add_rect(left * kPs1Px, cy, (right - left) * kPs1Px, 1.0f, col(255, 0, 0));
    }
  };
  // The creature count is a byte.
  const int n8 = (entry_count - 1) & 0xFF;
  if (dop_max_ != 0 && n8 > 11) {
    const bool last = dop_top_ == dop_max_ - 1;
    if (!last) tri(0x38, 0x3E, true);
    if (last || dop_top_ != 0) tri(0x90, 0x96, false);
  }
  if (!targets_.empty()) build_target_ui(false);
}

// ------------------------------------------------------------------------------------------------------------- results
void BattleView::build_result_ui() {
  const battle::Battle& battle_state = *battle_;
  const battle::Rewards& rewards = battle_state.rewards();
  const battle::Outcome outcome = battle_state.outcome();
  if (outcome == battle::Outcome::Victory) {
    const float x = 110, y = 60, w = 1060, h = 640;
    add_panel(x, y, w, h);
    add_rect(x + 8, y + 8, w - 16, h - 16, col(0, 0, 24, 170));
    add_text("Victory!", x + w / 2, y + 22, kYellow, 1, 1.3f);
    // left: gold + drops
    float ly = y + 120;
    add_text(std::format("Gold  {}", rewards.gold), x + 50, ly, kWhite);
    ly += 56;
    for (u16 it : rewards.items) {
      add_text("Item  " + tables_->item_name(it), x + 50, ly, kWhite, 0, 0.8f);
      ly += 42;
    }
    for (u16 el : rewards.elements) {
      add_text("Element  " + tables_->element_name(el), x + 50, ly, kGreen, 0, 0.8f);
      ly += 42;
    }
    if (rewards.items.empty() && rewards.elements.empty()) add_text("No drops", x + 50, ly, kGrey, 0, 0.8f);
    if (rewards.star_up) add_text("Star level up!", x + w / 2, y + 90, col(255, 200, 80), 1, 0.9f);
    // right / lower: stat growth per PC (battle::Battle::settle -> compute_rewards: minor growth, or the pending gains of a boss battle)
    static const char* kStat[7] = {"HP", "STR", "DEF", "HIT", "EVD", "MAG", "MDF"};
    const float tx = x + 430, ty = y + 120;
    add_text(rewards.star_up ? "Growth (star up)" : "Growth", tx, ty - 44, kGrey, 0, 0.7f);
    for (int k = 0; k < 7; ++k) add_text(kStat[k], tx + 190 + static_cast<float>(k) * 80, ty - 44, kGrey, 1, 0.65f);
    float py = ty;
    for (size_t i = 0; i < battle_state.party().size() && i < battle::kPcSlots; ++i) {
      const battle::PartyMember& member = battle_state.party()[i];
      const bool alive = battle_state.unit(static_cast<int>(i)).alive();
      add_text(member.name, tx, py, alive ? kWhite : kGrey, 0, 0.85f);
      for (int k = 0; k < 7; ++k) {
        const int gain = rewards.gains[i][static_cast<size_t>(k)];
        const float cx = tx + 190 + static_cast<float>(k) * 80;
        add_text(std::format("{}", member.stat[static_cast<size_t>(k)]), cx, py, kWhite, 1, 0.7f);
        add_text(gain > 0 ? std::format("+{}", gain) : std::string("-"), cx, py + 34, gain > 0 ? kGreen : col(110, 110, 120), 1, 0.6f);
      }
      py += 100;
    }
    add_text(std::format("Star {}   progress {}", battle_state.growth().star, battle_state.growth().progress), x + 50, y + h - 100, kGrey, 0, 0.65f);
    add_text("Press Enter", x + w / 2, y + h - 56, kGrey, 1, 0.8f);
  } else {
    // Defeat: the game-over flow is outside the battle overlays; a stub panel.
    add_rect(0, 0, kCanvasW, kCanvasH, col(0, 0, 0, 255));
    add_text(outcome == battle::Outcome::Wiped ? "GAME OVER" : "Battle stalled", kCanvasW / 2, 380, kRed, 1, 1.6f);
    add_text("The party was wiped out.", kCanvasW / 2, 480, kWhite, 1, 0.9f);
    add_text("Press Enter", kCanvasW / 2, 580, kGrey, 1, 0.8f);
  }
}

}  // namespace oc::game
