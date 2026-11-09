#pragma once
// PS1 menu art decoded from the disc (classic art with [Video] Ps1ResUi: the PS1 textures of the menus, shops, name entry, save/load).
// Sources: cdrom/map/etc/systim.bin (parchment tile), cdrom/menu/menutim.prd drp entry "wind"
// (frame pieces, captions, dial lines), cdrom/face/<name>.tim (64x64 avatars). Everything is nearest-sampled by the callers.
#include <string_view>
#include <vector>

#include "core/types.h"
#include "import/text.h"
#include "vfs/vfs.h"

namespace oc::ui::ps1 {

// Original-art text: the PS1 bitmap font (import::JpGlyphs) has no ASCII punctuation (its Latin blocks are digits and letters only),
// so every character it lacks is replaced by the full-width / JP form it does have (':' -> '：', ',' -> '，', '#' -> '＃', quotes,
// '…' -> '⋯', ...). Characters with no such form are kept (the caller's HD-face fallback decides). Spaces and newlines pass through.
std::u32string bitmap_text(const import::JpGlyphs& glyphs, std::u32string text);

struct Rgba {
  u32 w = 0, h = 0;
  std::vector<u8> px;  // w*h*4, colour 0x0000 transparent
  explicit operator bool() const { return w != 0 && h != 0; }
  const u8* at(u32 x, u32 y) const { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
};

// The brown parchment tile: 4bpp 64x64 at VRAM (896, 96) inside systim.bin; the menus draw 8 x 7 of them.
Rgba parchment(const vfs::Vfs& fs);
// One TIM entry of cdrom/menu/menutim.prd by its 4-char drp name ("wind", "suji", ...).
Rgba menutim(const vfs::Vfs& fs, std::string_view entry);
// Frame `frame` (1-based) of the menu's "main" sprite sheet (cdrom/menu/menutest.prd type-3 entry): every part (s16 x, y, u8 u, v, u16 CLUT,
// u8 w, h, u16 flags: texture page in the low byte, bit 9 / 10 mirrored in x / y) cut from the menutim.prd TIM that holds its CLUT and
// texels, the last part first. The image is kSheetFrameW x kSheetFrameH with the frame origin at kSheetFrameOriginX / Y: wide enough for
// the panel frames, whose parts reach 300 px right of and below their origin (the list frame's "Reorder" label part sits at (159, 288)).
inline constexpr int kSheetFrameW = 512, kSheetFrameH = 512, kSheetFrameOriginX = 192, kSheetFrameOriginY = 192;
Rgba sheet_frame(const vfs::Vfs& fs, int frame);
// Face avatar of character id 0..49 (cdrom/face/<name>.tim, 64x64). Empty Rgba if the id has none.
Rgba face(const vfs::Vfs& fs, int char_id);
// Retail portrait strip of character id 0..49 (cdrom/menu/playtim/pNN.tim, 8bpp 128x224: left half = top 224 rows, right half = bottom).
Rgba portrait(const vfs::Vfs& fs, int char_id);
const char* face_name(int char_id);
// (All loaders read the disc image itself, Vfs::read_disc, never the HD override copies.)

// ---- PS1 counterparts of the HD menu widgets (classic art + [Video] Ps1ResUi) ----
// Save-box icon of box `icon` 0..14 (CHRONO k), sheet index icon + 1: menutim.prd "p1ic" .. "pfic" (16 x 16, own CLUT); "p0ic" (index 0) is the white
// square with the red X. The HD art uses the menufaceicon sprites instead.
Rgba slot_icon(const vfs::Vfs& fs, int icon);
// The PS1 quad spans texels 0..15 (u0 0x7C, u1 0x8B).
inline constexpr float kSlotIconUv = 15.0f / 16.0f;
// EquipIcon cell k 0..4: menutim.prd "icon" (4bpp 256 x 66), the header part of the "main" sheet: texel rect and the part's offset
// from the frame origin (PS1 px).
struct IconCell {
  int u, v, w, h, dx, dy;
};
inline constexpr IconCell kEquipIcon[5] = {{0, 0, 25, 32, 10, -7}, {25, 0, 27, 32, 9, -7}, {52, 1, 26, 31, 10, -6}, {78, 1, 25, 31, 10, -6},
                                          {104, 0, 25, 32, 10, -6}};
// Element plate menutim_14_<type>_<variant> (184 x 24): the PS1 colour-sheet frame (gree / whit / kuro / red / yell /
// blue / nije of menutest.prd, frames 1..4) composed into 184 x 24: the "mpla" strip texels (1, 0, 173, 24) at (5, 0)
// under the four corner pieces of the colour TIM `type` (8 mnbl, 9 mngr, 10 mnku, 11 mnre, 12 mnwh, 13 mnye, 38 mnni). variant = 1..4 (file _01 = frame
// 1 = frame 3, _02 = frame 2, _04 = frame 4). Empty when `type` has no colour TIM.
Rgba plate(const vfs::Vfs& fs, int type, int variant);
// Key-item picture NN 1..64: cdrom/menu/keyitem/<name>.tim (8bpp 256 x 256), drawn
// 255 x 255. Empty for NN 54..60 (no file).
Rgba keyitem(const vfs::Vfs& fs, int nn);
inline constexpr float kKeyItemUv = 255.0f / 256.0f;

// PS1 window frame from the "wind" sheet: 3-px lines (dark / light / dark), 8 x 8 corner pieces TL (19,1) TR (37,1) BL (19,10) BR (37,10),
// top edge piece T (28,1) (double line), bottom edge B (28,10), left edge L (15,10, 3 px), right edge = the 3 right columns of TR (42,4). The interior is
// transparent. (ux, uy) = canvas size of one PS1 texel; `tab_x` / `tab_w` (canvas px from the left) cut a gap in the top edge for a caption tab,
// `foot_x` / `foot_w` one in the bottom edge. `piece(dx, dy, dw, dh, sx, sy, sw, sh)` draws a sheet rectangle (texel coordinates) to a
// canvas rectangle.
template <class PieceFn>
void frame_pieces(PieceFn&& piece, float x, float y, float w, float h, float ux, float uy, float tab_x = 0, float tab_w = 0, float foot_x = 0,
                  float foot_w = 0) {
  auto emit = [&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) {
    if (dw > 0 && dh > 0) piece(dx, dy, dw, dh, sx, sy, sw, sh);
  };
  const float cw = 8 * ux, ch = 8 * uy;
  if (tab_w > 0) {
    emit(x + cw, y, tab_x - cw, ch, 31, 1, 1, 8);
    emit(x + tab_x + tab_w, y, w - cw - tab_x - tab_w, ch, 31, 1, 1, 8);
  } else {
    emit(x + cw, y, w - 2 * cw, ch, 31, 1, 1, 8);
  }
  if (foot_w > 0) {
    emit(x + cw, y + h - ch, foot_x - cw, ch, 31, 10, 1, 8);
    emit(x + foot_x + foot_w, y + h - ch, w - cw - foot_x - foot_w, ch, 31, 10, 1, 8);
  } else {
    emit(x + cw, y + h - ch, w - 2 * cw, ch, 31, 10, 1, 8);
  }
  emit(x, y + ch, 3 * ux, h - 2 * ch, 15, 12, 3, 1);
  emit(x + w - 3 * ux, y + ch, 3 * ux, h - 2 * ch, 42, 4, 3, 1);
  emit(x, y, cw, ch, 19, 1, 8, 8);
  emit(x + w - cw, y, cw, ch, 37, 1, 8, 8);
  emit(x, y + h - ch, cw, ch, 19, 10, 8, 8);
  emit(x + w - cw, y + h - ch, cw, ch, 37, 10, 8, 8);
}

// Gap in the bottom edge of a menu frame (HD shape id), PS1 px from the frame's left: the element list frame 53 leaves 130..228 open for
// the Reorder prompt (its sheet frame has bottom-edge parts only at 2..130 and a corner at 228). {0, 0} = none.
struct EdgeGap {
  float start = 0, end = 0;
};
inline EdgeGap bottom_gap(int frame) { return frame == 53 ? EdgeGap{130, 228} : EdgeGap{}; }

// Caption notch of a menu frame (HD shape id): where the gap in the top edge ends, PS1 px from the frame's left (it starts at 8); 0 = none.
// Measured on the alpha of `hd/menu/shapes/menu-main-NNN`; 58 / 59 / 60 (the Equip candidate list) share the widths of
// the Weapons / Armor / Accessories frames 49 / 50 / 51.
inline float caption_gap_end(int frame) {
  switch (frame) {
    case 49: case 58: return 79;
    case 50: case 59: return 66;
    case 51: case 60: case 111: return 90;
    case 53: case 186: return 80;
    case 56: return 91;
    case 61: return 86;
    case 63: return 50;
    case 64: return 61;
    case 82: return 48;
    case 84: return 64;
    default: return 0;
  }
}

}  // namespace oc::ui::ps1
