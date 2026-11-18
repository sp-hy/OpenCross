#pragma once
// The PS1 HUD sheets (import::HudArt, import::ResultsArt) as the battle view and the results screen draw them: every frame packed into
// two atlases, one with the texels that carry the semi-transparency bit and one with the others, so a frame drawn with a blend mode blends
// only the first; and the battle font's frame of a character.
//   SheetAtlas atlas = pack_sheets(art.sheets);   // upload atlas.semi / atlas.opaque (atlas.width x atlas.height RGBA)
//   const SheetFrame& f = atlas.frames[type / 4][frame - 1];
#include <span>
#include <vector>

#include "core/types.h"
#include "import/battle_hud.h"
#include "import/text.h"

namespace oc::game {

// A frame's picture: offset from its origin and size in PS1 pixels, and its rectangle in the atlases (UV).
struct SheetFrame {
  int x = 0, y = 0, w = 0, h = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

struct SheetAtlas {
  int width = 0, height = 0;
  std::vector<u8> semi, opaque;
  std::vector<std::vector<SheetFrame>> frames;
};

SheetAtlas pack_sheets(std::span<const std::vector<import::HudFrame>> sheets);

// The battle font sheet ("osak", `font_frames` frames) frame of a character, 0 when it has none: 101.. the digits, 111.. A-Z, 149.. a-z,
// 137 / 138 / 139 / 140 = / ' + -; the kana and kanji follow the Japanese text codes from 0x76 on (frame = code - 0x6B).
int font_frame(char32_t ch, const import::JpGlyphs& codes, size_t font_frames);

}  // namespace oc::game
