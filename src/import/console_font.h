#pragma once
// The console's kanji font, read from the player's own PS1 BIOS image. The disc mode's Japanese menus and name entry draw it.
//
// The font is a run of 16 x 15 one-bit cells, 30 bytes each (row y = bytes 2y and 2y + 1, bit 7 of the first byte the leftmost pixel),
// one cell per defined character of JIS X 0208 rows 1-8 and 16-47 in code order (cell 0 = the full-width space). Nothing of it is
// shipped: a BIOS image is recognised by the structure of that run (find), not by its name or by known bytes.
//   auto font = import::find_console_font({exe_dir, image_dir});   // null when no file there holds the font
//   int cell = import::ConsoleFont::cell_of(U'セ');  font->pixel(cell, x, y);
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "core/types.h"

namespace oc::import {

class ConsoleFont {
 public:
  static constexpr int kCellW = 16, kCellH = 15, kCellBytes = 30;
  // Number of cells.
  static int cells();
  // Cell of a code point, -1 when the font has none (half-width characters included).
  static int cell_of(char32_t cp);
  // Offset of cell 0 in `image` when the image holds the font, nullopt otherwise. The candidates are the places where a blank cell
  // (the space) is followed by inked cells; one is accepted when the cells of every character after the space are inked, with a
  // plausible amount of ink and unlike the cell before them, and each outline shape (white square, circle, diamond, triangles,
  // star) lies inside its filled form with clearly more ink there.
  static std::optional<size_t> find(ByteSpan image);
  // The font of `image`, null when find() rejects it.
  static std::shared_ptr<const ConsoleFont> from_image(ByteSpan image);

  bool pixel(int cell, int x, int y) const;

 private:
  std::vector<u8> bits_;
};

// The first file directly inside one of `dirs` (in that order, names sorted within a folder) that holds the font: only files named
// *.bin / *.rom (any case) of 512 KiB to 1 MiB are read. Null when none does; `file` gets the one used.
std::shared_ptr<const ConsoleFont> find_console_font(const std::vector<std::filesystem::path>& dirs, std::filesystem::path* file = nullptr);

// The font found for this run (null when there is none); the app sets it once the discs are opened.
void set_console_font(std::shared_ptr<const ConsoleFont> font);
std::shared_ptr<const ConsoleFont> console_font();

}  // namespace oc::import
