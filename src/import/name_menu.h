#pragma once
// The name entry's own text on the PS1 discs (prog/namemenu.prg): the prompt of each build and the USA build's letter grid and length
// limit. The overlay keeps its strings as Shift-JIS strips of five full-width characters (the grid rows, drawn in the console's font)
// and, on the USA build, the prompt as ASCII for the menu sheet. A build is recognised by the overlay's size.
//   if (auto text = import::load_name_menu(vfs)) draw(text->prompt);
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::import {

inline constexpr int kNameGridSize = 10;
// A grid row of the USA build; 0 = a blank cell (it types a space).
using NameGridRow = std::array<char32_t, kNameGridSize>;

struct NameMenuText {
  // USA build: 10 rows of 10 cells (rows 0..2 A-Z, 3..5 a-z, 6 the digits, 7..9 blank). Empty for the Japanese build, whose kana pages
  // follow its glyph table.
  std::vector<NameGridRow> grid;
  // The prompt line under the name.
  std::u32string prompt;
  // Japanese build: Viper's rank, drawn after his name.
  std::u32string rank;
  // USA build: the longest name for every character; 0 for the Japanese build (6, Viper 4).
  int max_length = 0;
  // The names the screen refuses (after trailing blanks are dropped; case-sensitive): USA "Crono" and "Chrono" as prefixes (a name
  // that begins with one), Japanese "クロノ" as the whole name.
  std::vector<std::u32string> reserved;
  bool reserved_prefix = false;
};

// `prg` = prog/namemenu.prg of either build. Null when the file is not a known build's.
std::shared_ptr<const NameMenuText> parse_name_menu(ByteSpan prg);
// The text of the disc the VFS serves; null without the overlay.
std::shared_ptr<const NameMenuText> load_name_menu(const vfs::Vfs& vfs);

}  // namespace oc::import
