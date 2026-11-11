#pragma once
// ButtonPanel: RDE's on-screen button picker for a room script that reads the pad through key-latch tests (a code entered with the
// buttons). A field script op (FE D1) runs it just before the script's tests: a Circle, Cross or Triangle press opens the panel and is
// read as Circle. While it is open the script's key latch holds nothing but the button picked on the panel's grid, until the script
// clears the latch. FieldWorld draws it as a message window (`text`, `cursor_glyph`, `lit_glyph`).
#include <string>
#include <utility>

#include "core/types.h"

namespace oc::game {

class ButtonPanel {
 public:
  static constexpr int kRows = 4, kCols = 2;
  // PS1 key-latch bits the panel reads and delivers.
  static constexpr u16 kL1 = 0x04, kR1 = 0x08, kTriangle = 0x10, kCircle = 0x20, kCross = 0x40;
  // Ticks the picked cell stays lit (and the cursor stays on it) before the cursor returns to the first cell.
  static constexpr int kHoldTicks = 60;
  // The panel closes once this many picks have been made since it opened.
  static constexpr int kPicksPerOpen = 4;

  struct Keys {
    bool up = false, down = false, left = false, right = false, confirm = false, cancel = false;
  };

  // FE D1, with the script's key latch: a Circle, Cross or Triangle opens the closed panel and is delivered as Circle.
  void watch(u16 latch);
  // The key latch the script reads: the delivered button while the panel is open or a button waits for the script, else `raw` without
  // the Circle, Cross and Triangle still held from before the panel closed.
  u16 latch(u16 raw) const;
  // The script cleared its key latch: a delivered button has been read.
  void clear_latch() { pending_ = 0; }
  // One field tick (`held`: the keys down now): the cursor (Up / Down rows, Left / Right columns, no wrap), a pick (confirm; not while
  // the last button waits for the script) and Cross (closes; a waiting button is dropped).
  void tick(const Keys& keys, u16 held);
  void close();

  bool is_open() const { return open_; }
  int row() const { return row_; }
  int col() const { return col_; }
  bool holding() const { return hold_ > 0; }
  // Japanese text: the letter column keeps its first mapping.
  void set_japanese(bool jp) { jp_ = jp; }
  // The button a cell delivers now.
  u16 cell_button(int row, int col) const;

  // The window text (four lines of two cells; Japanese spacing with `jp`) and, for the drawing, the page glyph index the cursor stands
  // on and the lit cell.
  std::u32string text() const;
  int cursor_glyph() const;
  // (glyph index, text colour): the picked cell while it is held (colour 2), else the cursor's cell (colour 6).
  std::pair<int, int> lit_glyph() const;

 private:
  int cell_glyph(int row, int col) const;
  bool open_ = false;
  bool jp_ = false;
  int row_ = 0, col_ = 0;
  int picks_ = 0;
  int hold_ = 0;
  u16 pending_ = 0;
  u16 released_ = 0;
};

}  // namespace oc::game
