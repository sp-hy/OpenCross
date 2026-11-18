#pragma once
// The status plate column of the battle menu: one name plate per status of a unit's status word (lo) that the column shows, in a fixed
// order. Each plate is a colour triangle and the status name. Plate n of the column sits at y = 50 + 12 (n % 9) in column n / 9; the right
// column (the active PC's, top menu) starts at x 248 and continues at 210, the left one (the target cursor unit's) at 16 and 54.
#include <array>
#include <bit>
#include <vector>

#include "core/types.h"

namespace oc::game::bstat {

// Triangle colours: the bottom row of battle_34.png, in this order.
enum Colour { kRed, kGreen, kBlue, kYellow, kWhite, kBlack };

struct Plate {
  u32 bit;
  // cmnmes key and the English fallback.
  const char* jp;
  const char32_t* en;
  Colour colour;
};

inline constexpr std::array<Plate, 12> kPlates = {{
    {0x00000001u, "毒", U"Poison", kGreen},
    {0x00000008u, "やけど", U"Burns", kRed},
    {0x00000010u, "ねんざ", U"Sprain", kYellow},
    {0x00000020u, "風邪", U"Flu", kBlue},
    {0x00000100u, "臆病", U"Afraid", kGreen},
    {0x00002000u, "過労", U"Fatigued", kWhite},
    {0x00004000u, "睡眠", U"Asleep", kWhite},
    {0x00020000u, "暗闇", U"Darkness", kBlack},
    {0x00040000u, "呪い", U"Cursed", kBlack},
    {0x00100000u, "混乱", U"Confused", kRed},
    {0x02000000u, "めまい", U"Dizzy", kYellow},
    {0x20000000u, "凍結", U"Frozen", kBlue},
}};

struct Placed {
  // Index into kPlates; PS1 position of the plate (triangle at (x, y + 1), name at (x + 4, y)).
  int plate;
  int x, y;
};

inline std::vector<Placed> place(u32 lo, bool right) {
  static constexpr int kColumnX[2][2] = {{16, 54}, {248, 210}};
  std::vector<Placed> out;
  for (size_t i = 0; i < kPlates.size(); ++i) {
    if (!(lo & kPlates[i].bit)) continue;
    const int n = static_cast<int>(out.size());
    out.push_back({static_cast<int>(i), kColumnX[right ? 1 : 0][n / 9], 50 + 12 * (n % 9)});
  }
  return out;
}

// Played from the PS1 discs: the plate's frame of the battle status sheet (0x78): 7 + the bit's rank among the bits that have one.
inline int sheet_frame(u32 bit) { return 7 + std::popcount(0x33377339u & (bit - 1)); }

}  // namespace oc::game::bstat
