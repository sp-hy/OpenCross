#include "render/character_texture.h"

#include <algorithm>
#include <cstring>

namespace oc::render {

namespace {

bool usable(const import::Image& image) {
  return !image.indexed && image.width && image.height && image.pixels.size() >= static_cast<size_t>(image.width) * image.height * 4;
}

// Column of the shut cell of eye k in the strip, page texels.
constexpr u32 shut_cell_x(u32 k) { return (2 + k) * kCharacterEyeCell; }

}  // namespace

TextureKey character_texture_key(const std::string& stem) { return {"model", "character/" + stem, -1, "texture"}; }

TextureKey character_eyes_key(const std::string& stem) { return {"model", "character/" + stem, -1, "eyes"}; }

import::Image character_status_image(const import::Image& open, const import::Image* eyes, const CharacterEyes& rects) {
  if (!usable(open)) return {};
  import::Image out;
  out.width = open.width * 2, out.height = open.height;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (u32 row = 0; row < open.height; ++row)
    for (u32 half = 0; half < 2; ++half)
      std::memcpy(&out.pixels[(static_cast<size_t>(row) * out.width + half * open.width) * 4], &open.pixels[static_cast<size_t>(row) * open.width * 4],
                  static_cast<size_t>(open.width) * 4);
  if (!eyes || !usable(*eyes) || !rects.valid) return out;
  // Pixels per page texel of the open page and of the strip (4 cells wide, one high).
  const double page_x = static_cast<double>(open.width) / kCharacterPageWidth, page_y = static_cast<double>(open.height) / kCharacterPageHeight;
  const double strip_x = eyes->width / (4.0 * kCharacterEyeCell), strip_y = static_cast<double>(eyes->height) / kCharacterEyeCell;
  for (u32 k = 0; k < 2; ++k) {
    const u32 x0 = static_cast<u32>(rects.x[k] * page_x), x1 = std::min(open.width, static_cast<u32>((rects.x[k] + kCharacterEyeCell) * page_x));
    const u32 y0 = static_cast<u32>(rects.y[k] * page_y), y1 = std::min(open.height, static_cast<u32>((rects.y[k] + kCharacterEyeCell) * page_y));
    for (u32 py = y0; py < y1; ++py)
      for (u32 px = x0; px < x1; ++px) {
        const double u = shut_cell_x(k) + ((px + 0.5) / page_x - rects.x[k]), v = (py + 0.5) / page_y - rects.y[k];
        const u32 sx = std::min(eyes->width - 1, static_cast<u32>(u * strip_x)), sy = std::min(eyes->height - 1, static_cast<u32>(v * strip_y));
        std::memcpy(&out.pixels[(static_cast<size_t>(py) * out.width + open.width + px) * 4], &eyes->pixels[(static_cast<size_t>(sy) * eyes->width + sx) * 4], 4);
      }
  }
  return out;
}

}  // namespace oc::render
