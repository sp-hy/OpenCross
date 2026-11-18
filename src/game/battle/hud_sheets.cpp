#include "game/battle/hud_sheets.h"

#include <algorithm>

#include "import/tim.h"

namespace oc::game {

namespace {

constexpr int kAtlasW = 1024;

}  // namespace

// Shelf packing in sheet order, one transparent texel between frames.
SheetAtlas pack_sheets(std::span<const std::vector<import::HudFrame>> sheets) {
  SheetAtlas atlas;
  atlas.width = kAtlasW;
  struct Placed {
    int x, y;
  };
  std::vector<Placed> placed;
  int pen_x = 0, pen_y = 0, shelf = 0;
  for (const std::vector<import::HudFrame>& sheet : sheets)
    for (const import::HudFrame& frame : sheet) {
      if (frame.w <= 0 || frame.h <= 0) continue;
      if (pen_x + frame.w + 1 > kAtlasW) pen_x = 0, pen_y += shelf, shelf = 0;
      placed.push_back({pen_x, pen_y});
      pen_x += frame.w + 1;
      shelf = std::max(shelf, frame.h + 1);
    }
  atlas.height = std::max(1, pen_y + shelf);
  atlas.semi.assign(static_cast<size_t>(kAtlasW) * static_cast<size_t>(atlas.height) * 4, 0);
  atlas.opaque.assign(atlas.semi.size(), 0);
  const float atlas_h = static_cast<float>(atlas.height);
  size_t next = 0;
  for (const std::vector<import::HudFrame>& sheet : sheets) {
    std::vector<SheetFrame>& frames = atlas.frames.emplace_back();
    for (const import::HudFrame& frame : sheet) {
      SheetFrame out;
      if (frame.w > 0 && frame.h > 0) {
        const Placed& at = placed[next++];
        out = {frame.x, frame.y, frame.w, frame.h, static_cast<float>(at.x) / kAtlasW, static_cast<float>(at.y) / atlas_h,
               static_cast<float>(at.x + frame.w) / kAtlasW, static_cast<float>(at.y + frame.h) / atlas_h};
        for (int y = 0; y < frame.h; ++y)
          for (int x = 0; x < frame.w; ++x) {
            const u8* texel = &frame.rgba[(static_cast<size_t>(y) * static_cast<size_t>(frame.w) + static_cast<size_t>(x)) * 4];
            if (texel[3] == 0) continue;
            std::vector<u8>& dst = texel[3] == import::kStpAlpha ? atlas.semi : atlas.opaque;
            u8* pixel = &dst[(static_cast<size_t>(at.y + y) * kAtlasW + static_cast<size_t>(at.x + x)) * 4];
            pixel[0] = texel[0], pixel[1] = texel[1], pixel[2] = texel[2], pixel[3] = 255;
          }
      }
      frames.push_back(out);
    }
  }
  return atlas;
}

int font_frame(char32_t ch, const import::JpGlyphs& codes, size_t font_frames) {
  if (ch >= U'0' && ch <= U'9') return static_cast<int>(ch - U'0') + 101;
  if (ch >= U'A' && ch <= U'Z') return static_cast<int>(ch - U'A') + 111;
  if (ch >= U'a' && ch <= U'z') return static_cast<int>(ch - U'a') + 149;
  switch (ch) {
    case U'\'': return 138;
    case U'+': return 139;
    case U'-': return 140;
    case U'/': return 137;
    default: break;
  }
  const int index = codes.index_for(ch);
  const int frame = index + 0x20 - 0x6B;
  return index >= 0 && frame >= 11 && static_cast<size_t>(frame) <= font_frames ? frame : 0;
}

}  // namespace oc::game
