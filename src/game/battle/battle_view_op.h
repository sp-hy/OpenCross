// Private definition of BattleView::Op, the queued HUD primitive shared by the HUD builders and the replay.
#pragma once
#include "game/battle/battle_view.h"

namespace oc::game {

// One queued HUD primitive in canvas units (built in update(), replayed in draw()).
// slot: Text font slot (ui::font::Slot); align: 0 left, 1 centre, 2 right; plate: index into plates_ (0 = menuelem_1); tex: Img texture;
// icon: an Icon op's element colour index (0..5), drawn centred on (x, y) w wide, its brightness the red byte of color;
// blend: 0 alpha, 1 multiply, 2 add; grp: widescreen HUD edge group, -1 left (ring, command / element / target UI), +1 right (party panels),
// 2 pinned to the 4:3 picture (world-attached), 0 unassigned; shift: extra canvas units added to the group's edge offset (equalises the outer
// margin with the field-effect ring's 7 PS1 px).
struct BattleView::Op {
  enum Kind { Rect, Text, Panel, Plate, Img, Icon } kind = Rect;
  float x = 0, y = 0, w = 0, h = 0;
  u32 color = 0xFFFFFFFFu;
  std::u32string text;
  int slot = 2;
  ui::font::Ink ink;
  float scale = 1;
  int align = 0;
  int plate = 0;
  int icon = 0;
  const Tex* tex = nullptr;
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  int blend = 0;
  int grp = 0;
  float shift = 0;
};

}  // namespace oc::game
