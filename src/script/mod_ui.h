#pragma once
// Mod UI (feature 2): building blocks a mod's field script puts together into its own screens over the
// room - rectangles, lines, text windows (room messages in the game's window skin, framed or frameless) and selectable items - plus a
// cursor and a blocking pick that moves it over the items with the D-pad (ops FE C1-FE CB). The field world draws it
// (FieldWorld::draw_mod_ui, the text windows through its own message windows); the state belongs to the room's VM, so a room change
// clears it.
#include <array>
#include <utility>

#include "core/types.h"

namespace oc::script {

struct ModUi {
  static constexpr int kElements = 64, kPalette = 32;
  enum Kind : u8 { kNone = 0, kRect, kLine, kText, kItem };
  // UI_RECT flags.
  static constexpr u8 kOutline = 1, kTranslucent = 2;
  // UI_TEXT flags: a framed window in the player's window skin (else frameless).
  static constexpr u8 kFramed = 1;
  // UI_ITEM flags: the cursor skips it.
  static constexpr u8 kDisabled = 1;
  enum CursorStyle : u8 { kCursorOutline = 0, kCursorHighlight = 1, kCursorNone = 2 };
  // UI_SET fields.
  enum Field : u8 { kFieldVisible = 0, kFieldEnabled = 1, kFieldColour = 2, kFieldX = 3, kFieldY = 4, kFieldMsg = 5, kFieldFlags = 6, kFieldW = 7, kFieldH = 8 };
  // Pick results besides an item id (0..63): "the cursor moved" (| item), cancel, and the two extra buttons.
  static constexpr s32 kPickMoved = 0x40, kPickCancel = 0x80, kPickTriangle = 0x81, kPickSquare = 0x82;

  // Coordinates are field screen pixels (320 x 224, the dialogue windows' space). A rect / item is (x, y, w, h); a line runs from (x, y)
  // to (w, h); a text window's text starts at (x, y). `colour` is a palette index. `revision` counts the changes a text window must
  // be shown again for (its message, place or frame).
  struct Element {
    u8 kind = kNone;
    s16 x = 0, y = 0, w = 0, h = 0;
    u8 colour = 0, flags = 0;
    s16 msg = -1;
    bool visible = true;
    u16 revision = 0;
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

  // 0 white, 1 red, 2 orange, 3 purple, 4 yellow, 5 green, 6 blue, 7 cyan, 8 grey, 9 black, 10 dark blue, 11 dark grey, 12 light grey;
  // the rest white.
  static std::array<std::array<u8, 3>, kPalette> default_palette();
  // Everything off and the default palette (UI_CLEAR, a room load).
  void clear();
  // A visible, enabled item.
  bool selectable(int id) const;
  // The selectable item nearest to the cursor's item in that direction (0 up, 1 right, 2 down, 3 left), by item centres: the smallest
  // `along + 2 * |across|` over the items strictly that way; -1 when there is none.
  int step(int dir) const;
  // The centre of an element (a line's midpoint).
  std::pair<int, int> centre(int id) const;
  // UI_SET; false for an unknown field or id.
  bool set(int id, int field, int value);
};

}  // namespace oc::script
