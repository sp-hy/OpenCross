#pragma once
// The battle HUD art of the PS1 discs: the sprite sheets of battle/battle.prd (nested pack "tana") that the battle draws its panels,
// field ring, menus, labels and digits from, each frame composed into one picture; the same for the results screen (battle/btend.prd)
// and its messages.
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

// The results screen's art (battle/btend.prd, nested pack "tana"): the sheets the screen names by its own list, sheet n addressed as
// type 4 n (kResultsSheetNames: the colour plates 0x00..0x14, gg 0x18, curs 0x1C, osak 0x20, viva 0x24, sico 0x28, levu 0x2C, 2bnu 0x30,
// paus 0x34, elef 0x38), and the wallpaper tile (TIM "bg").
inline constexpr int kResultsSheets = 15;
extern const char* const kResultsSheetNames[kResultsSheets];

struct ResultsArt {
  std::array<std::vector<HudFrame>, kResultsSheets> sheets;
  HudFrame wall;

  const HudFrame* frame(int type, int frame_number) const;
};

// Null when the file holds no "tana" pack.
std::optional<ResultsArt> load_results_art(ByteSpan prd);

// battle/data/exdat.dat, the results screen's data: a u32 count, then u32 offsets (count + 1), every member LZSS-packed. Member
// `index` unpacked; nullopt when absent.
std::optional<Bytes> load_exit_member(ByteSpan exdat, u32 index);

// The results screen's messages (member 3, entries {u16 offset, u16 cells, u16, u16 width}): each entry's PS1 text bytes up to its first
// control byte (below 0x20). `pairs` = the Japanese charset, whose bytes 0xF8..0xFE take the next byte along. Empty when absent.
std::vector<Bytes> load_exit_messages(ByteSpan exdat, bool pairs);

// The results screen's icon per item id (member 5, 12 bytes per item, byte 10's low nibble: sheet "sico" frame icon + 1). Empty when
// absent.
std::vector<u8> load_exit_item_icons(ByteSpan exdat);

}  // namespace oc::import
