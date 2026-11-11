#include "game/button_panel.h"

#include <algorithm>
#include <array>

namespace oc::game {

namespace {

// Cell labels by row: the button symbols, then the letters of the code's hint.
constexpr std::array<char32_t, ButtonPanel::kRows> kSymbols = {U'○', U'×', U'△', U'□'};
constexpr std::array<char32_t, ButtonPanel::kRows> kSymbolsJp = {U'〇', U'×', U'△', U'□'};
constexpr std::array<char32_t, ButtonPanel::kRows> kLetters = {U'L', U'R', U'B', U'Y'};
// Each line: three spaces, the symbol, three spaces, the letter, the line's tail (one space; two in Japanese).
constexpr int kLead = 3, kLineGlyphs = 9, kLineGlyphsJp = 10;
constexpr u16 kOpening = ButtonPanel::kCircle | ButtonPanel::kCross | ButtonPanel::kTriangle;

}  // namespace

u16 ButtonPanel::cell_button(int row, int col) const {
  // Square, B and Y deliver Circle. Outside Japanese, from the second pick on L delivers Circle and R delivers L1.
  const bool later = !jp_ && picks_ >= 1;
  if (col == 0) return row == 1 ? kCross : row == 2 ? kTriangle : kCircle;
  if (row == 0) return later ? kCircle : kL1;
  if (row == 1) return later ? kL1 : kR1;
  return kCircle;
}

void ButtonPanel::watch(u16 latch) {
  if (open_ || pending_ || !(latch & kOpening & ~released_)) return;
  open_ = true;
  row_ = col_ = 0;
  picks_ = 0;
  hold_ = 0;
  pending_ = kCircle;
}

u16 ButtonPanel::latch(u16 raw) const { return open_ || pending_ ? pending_ : static_cast<u16>(raw & ~released_); }

void ButtonPanel::tick(const Keys& keys, u16 held) {
  released_ &= held;
  if (!open_) return;
  if (hold_ > 0) {
    if (--hold_ > 0) return;
    row_ = col_ = 0;
    if (picks_ >= kPicksPerOpen) {
      open_ = false;
      released_ = kOpening;
    }
    return;
  }
  if (keys.cancel) {
    close();
    return;
  }
  if (keys.confirm && !pending_) {
    pending_ = cell_button(row_, col_);
    ++picks_;
    hold_ = kHoldTicks;
    return;
  }
  if (keys.up) row_ = std::max(0, row_ - 1);
  if (keys.down) row_ = std::min(kRows - 1, row_ + 1);
  if (keys.left) col_ = 0;
  if (keys.right) col_ = 1;
}

void ButtonPanel::close() {
  open_ = false;
  released_ = kOpening;
  pending_ = 0;
  hold_ = 0;
}

std::u32string ButtonPanel::text() const {
  const std::u32string space(1, jp_ ? U'　' : U' ');
  std::u32string out;
  for (int row = 0; row < kRows; ++row) {
    if (row > 0) out += U'\n';
    for (int k = 0; k < kLead; ++k) out += space;
    out += (jp_ ? kSymbolsJp : kSymbols)[static_cast<size_t>(row)];
    for (int k = 0; k < kLead; ++k) out += space;
    out += kLetters[static_cast<size_t>(row)];
    out += jp_ ? U"  " : U" ";
  }
  return out;
}

int ButtonPanel::cell_glyph(int row, int col) const {
  return row * (jp_ ? kLineGlyphsJp : kLineGlyphs) + col * (kLead + 1) + kLead;
}

int ButtonPanel::cursor_glyph() const { return cell_glyph(row_, col_) - kLead; }

std::pair<int, int> ButtonPanel::lit_glyph() const { return {cell_glyph(row_, col_), hold_ > 0 ? 2 : 6}; }

}  // namespace oc::game
