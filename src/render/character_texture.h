#pragma once
// The character textures: two images per playable model (model/character/<stem>/, <stem> = its status-screen model's file name) that the
// status screen and battle both draw from in HD art. texture.png is the 128 x 256 page with the eyes open (the dump: 4x, 512 x 1024);
// eyes.png is the battle eye strip (the dump: 4x, 512 x 128): four 32 x 32 page-texel cells, the half-shut left and right eye, then the shut
// left and right eye, pasted over the battle model's two eye rects. Any size works for either file. Battle draws them as its main texture
// and eye strip; the status screen's shut picture is the texture with the shut cells pasted over the same rects.
#include <array>
#include <string>

#include "core/types.h"
#include "import/tim.h"
#include "render/texture_registry.h"

namespace oc::render {

// The page both textures are scaled copies of, and the eye cell size, in page texels.
inline constexpr u32 kCharacterPageWidth = 128, kCharacterPageHeight = 256, kCharacterEyeCell = 32;

// The battle model's two eye rects (top-left corners, page texels); `valid` false = the model has none.
struct CharacterEyes {
  bool valid = false;
  std::array<u16, 2> x{}, y{};
};

TextureKey character_texture_key(const std::string& stem);
TextureKey character_eyes_key(const std::string& stem);

// The status screen's image of a character, as its HD TIM lays it out: the open page, then the open page with the shut cells of `eyes`
// pasted over the eye rects (no eyes or no rects: the open page again). Empty when `open` is not usable.
import::Image character_status_image(const import::Image& open, const import::Image* eyes, const CharacterEyes& rects);

}  // namespace oc::render
