#pragma once
// The battle HUD art of the PS1 discs: the sprite sheets of battle/battle.prd (nested pack "tana") that the battle draws its panels,
// field ring, menus, labels and digits from, each frame composed into one picture.
//
// The battle names 47 sheets by a fixed list; sheet n is "type" 4 n (kHudSheetNames). A frame is drawn with its origin at a screen point;
// its parts lie at their own offsets from it, the last part underneath. Texels keep the semi-transparency bit: a frame drawn with a
// blend mode blends only those texels and draws the others opaque.
//   auto hud = import::load_battle_hud(prd, prg, usa);
//   const import::HudFrame* f = hud->frame(0x74, 1);   // the party panel
#include <array>
#include <optional>
#include <vector>

#include "core/types.h"

namespace oc::import {

inline constexpr int kHudSheets = 47;
extern const char* const kHudSheetNames[kHudSheets];

// One composed frame: (x, y) = the top-left of the picture relative to the frame origin; rgba w * h, alpha 0 transparent, kStpAlpha
// (tim.h) a texel with the semi-transparency bit, 255 the others.
struct HudFrame {
  int x = 0, y = 0, w = 0, h = 0;
  std::vector<u8> rgba;
};

struct HudArt {
  // [type / 4][frame - 1]; a sheet missing from the file has no frames.
  std::array<std::vector<HudFrame>, kHudSheets> sheets;
  // The battle window skin (TIM "win1", 64 x 16), rgba as HudFrame.
  HudFrame skin;
  // The damage digit sheet (TIM "numf", 144 x 15; frame k = digit k - 1, 11 = "miss!" left to right), rgba as HudFrame.
  HudFrame digits;
  // Pen advance of a battle-font glyph (sheet 0x04 frame) in a party panel name, by frame number.
  std::array<u8, 512> name_advance{};

  const HudFrame* frame(int type, int frame_number) const;
};

// `prd` = battle/battle.prd, `prg` = prog/battle.prg of the same build; `usa` = the USA build (its name glyphs advance by a width table
// of battle.prg, the Japanese ones by 9 pixels). Null when the file holds no "tana" pack.
std::optional<HudArt> load_battle_hud(ByteSpan prd, ByteSpan prg, bool usa);

}  // namespace oc::import
