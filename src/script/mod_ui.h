#pragma once
// Mod UI (feature 2): building blocks a mod's field script puts together into its own screens over the
// room - rectangles, lines, ovals, images, text windows (room messages in the game's window skin, framed or frameless) and selectable items -
// plus a cursor and a blocking pick that moves it over the items with the D-pad (ops FE C1-FE CB, FE CE, FE CF). The field world draws it
// (FieldWorld::draw_mod_ui, the text windows through its own message windows); the state belongs to the room's VM, so a room change
// clears it.
#include <array>
#include <utility>

#include "core/types.h"

namespace oc::script {

struct ModUi {
  // Items are selectable only below kItems: a pick result carries the item in its low 6 bits.
  static constexpr int kElements = 256, kItems = 64, kPalette = 32;
  // The screen the coordinates are in: a script's message window position space (x 0..320, y 0..216 over the whole dialogue canvas).
  static constexpr float kScreenW = 320.0f, kScreenH = 216.0f;
  enum Kind : u8 { kNone = 0, kRect, kLine, kText, kItem, kOval, kImage };
  // UI_RECT / UI_OVAL / UI_IMAGE flags (an oval's outline is a band `width` pixels wide, inside its edge; images take only translucent).
  static constexpr u8 kOutline = 1, kTranslucent = 2;
  // Level 2, rects / ovals / images: the opacity breathes (100 % .. 55 % and back, once a second; a cursor ring).
  static constexpr u8 kPulse = 4;
  // Level 2, rects / ovals / images: the size breathes with the same clock (1 pixel out and in about the centre).
  static constexpr u8 kBeat = 8;
  // UI_TEXT flags: a framed window in the player's window skin (else frameless).
  static constexpr u8 kFramed = 1;
  // UI_ITEM flags: the cursor skips it.
  static constexpr u8 kDisabled = 1;
  enum CursorStyle : u8 { kCursorOutline = 0, kCursorHighlight = 1, kCursorNone = 2 };
  // UI_SET fields.
  enum Field : u8 { kFieldVisible = 0, kFieldEnabled = 1, kFieldColour = 2, kFieldX = 3, kFieldY = 4, kFieldMsg = 5, kFieldFlags = 6, kFieldW = 7, kFieldH = 8, kFieldWidth = 9,
                  kFieldAngle = 10, kFieldAlpha = 11, kFieldUp = 12, kFieldRight = 13, kFieldDown = 14, kFieldLeft = 15 };
  // Pick results besides an item id (0..kItems - 1): "the cursor moved" (| item), cancel, and the extra buttons (L1 / R1: level 2).
  static constexpr s32 kPickMoved = 0x40, kPickCancel = 0x80, kPickTriangle = 0x81, kPickSquare = 0x82, kPickL1 = 0x83, kPickR1 = 0x84;

  // Coordinates are in the kScreenW x kScreenH screen, a text window's (x, y) being its message window position. A rect / item is (x, y, w, h);
  // a line runs from (x, y) to (w, h); an oval is centred on (x, y) with radii (w, h); a text window's text starts at (x, y); an image fills
  // (x, y, w, h). `colour` is a palette index (an image's tint: its pixels times the colour). `msg` is a text window's message, or an image's
  // number n: the file hd/modui/<n>.png that a mod adds. `revision` is the `serial` of the element's last change (a text window is shown
  // again when it differs). `width`: an outlined oval's band.
  struct Element {
    u8 kind = kNone;
    s16 x = 0, y = 0, w = 0, h = 0;
    u8 colour = 0, flags = 0;
    s16 msg = -1;
    bool visible = true;
    u32 revision = 0;
    u8 width = 1;
    // An image turned about its rect's centre (4096 = a full turn, clockwise on screen); the element's opacity (0 = the default:
    // opaque, or 5 / 8 when translucent).
    s16 angle = 0;
    u8 alpha = 0;
    // An item's D-pad neighbours (up, right, down, left): the item that way, its own id = that way doesn't move, -1 = the nearest
    // item that way (step).
    std::array<s16, 4> nav{-1, -1, -1, -1};
  };

  // A held D-pad direction steps again after kRepeatDelay ticks, then every kRepeatEvery ticks (as a menu cursor does).
  static constexpr int kRepeatDelay = 18, kRepeatEvery = 6;

  bool shown = false;
  std::array<Element, kElements> elements{};
  int cursor = -1;
  u8 cursor_style = kCursorOutline;
  std::array<std::array<u8, 3>, kPalette> palette = default_palette();
  // UI_PICK's key state: the keys held when it last looked (a key counts again only once released; all at first, so the press that
  // opened the screen is not taken) and the ticks the held direction has been down.
  u16 pick_held = 0xFFFF;
  int pick_repeat = 0;
  // A pick has run since the UI was shown: the shown UI holds the field's controls (the player does not walk while the cursor
  // moves). A UI that never picks (a status readout) leaves them free.
  bool holds_controls = false;
  // Counts every element change for the life of the VM; clear() keeps it, so an element placed after a clear never repeats a revision
  // a window already shows.
  u32 serial = 0;

  // 0 white, 1 red, 2 orange, 3 purple, 4 yellow, 5 green, 6 blue, 7 cyan, 8 grey, 9 black, 10 dark blue, 11 dark grey, 12 light grey;
  // the rest white.
  static std::array<std::array<u8, 3>, kPalette> default_palette();
  // Everything off and the default palette (UI_CLEAR, a room load); `serial` runs on.
  void clear();
  // A visible, enabled item (ids 0..kItems - 1).
  bool selectable(int id) const;
  // The selectable item nearest to the cursor's item in that direction (0 up, 1 right, 2 down, 3 left), by item centres: the smallest
  // `along + 2 * |across|` over the items strictly that way; -1 when there is none. An item's own neighbour that way (UI_SET 12..15) comes
  // first when it can be picked.
  int step(int dir) const;
  // The centre of an element (a line's midpoint, an oval's centre).
  std::pair<int, int> centre(int id) const;
  // UI_SET; false for an unknown field or id.
  bool set(int id, int field, int value);
};

}  // namespace oc::script
