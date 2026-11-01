#pragma once
// DirectXTK MakeSpriteFont binary reader (font.dat / ui.dat): "DXTKfont", u32 glyph count, 32-byte glyphs, line spacing,
// default character, texture width / height / DXGI format / stride / rows, then the pixel block.
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::import {

struct SfGlyph {
  u32 ch = 0;                     // UTF-16 code unit
  s32 left = 0, top = 0, right = 0, bottom = 0;  // sub-rectangle in texels
  float x_offset = 0, y_offset = 0, x_advance = 0;
  int width() const { return right - left; }
  int height() const { return bottom - top; }
};

// DXGI formats seen in the files (115) plus the other common MakeSpriteFont outputs.
constexpr u32 kDxgiR8G8B8A8 = 28, kDxgiR8 = 61, kDxgiB5G6R5 = 85, kDxgiB8G8R8A8 = 87, kDxgiB4G4R4A4 = 115;

struct SpriteFont {
  std::vector<SfGlyph> glyphs;  // sorted by character
  float line_spacing = 0;
  u32 default_char = 0;         // 0 = none
  u32 tex_width = 0, tex_height = 0, format = 0, stride = 0, rows = 0;
  Bytes file;                   // the whole file (kept so huge atlases are not copied)
  size_t pixel_offset = 0;      // start of the pixel block inside `file`; stride*rows bytes follow (trailing bytes ignored)

  const SfGlyph* find(u32 ch) const;
  // Glyph for `ch`, else the default character's glyph, else nullptr.
  const SfGlyph* find_or_default(u32 ch) const;
  // The file's measure of one character: the glyph box from its x offset, 0 for a blank (a white space with an empty box) or no glyph.
  float measure(u32 ch) const;
  // One texel as 4 channels R,G,B,A (0..255, 4-bit channels are expanded with *17). False if outside the texture or the
  // format is unsupported.
  bool texel(u32 x, u32 y, u8 rgba[4]) const;
  static bool format_supported(u32 format);
};

// Takes ownership of the file bytes. nullopt (and a message) on a malformed file; never throws.
std::optional<SpriteFont> parse_spritefont(Bytes&& file, std::string* error = nullptr);

}  // namespace oc::import
