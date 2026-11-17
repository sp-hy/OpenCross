#pragma once
// PS1 MDEC "version 2" frames (the movie streams' video): an 8-byte header (MDEC length, 0x3800, quantiser, version 2), then 16 x 16
// macroblocks in column order, each six 8 x 8 blocks (Cr, Cb, four Y) coded as a 10-bit DC value and MPEG-1 intra run / level codes.
// Decoded with the MPEG-1 default intra matrix and an 8 x 8 inverse DCT to full-range BT.601 YCbCr 4:2:0, then RGB.
#include <vector>

#include "core/types.h"

namespace oc::import {

// Decodes one frame of `width` x `height` pixels (multiples of 16) to RGBA (alpha 255). False on a malformed or truncated frame.
bool mdec_decode(ByteSpan frame, u32 width, u32 height, std::vector<u8>& rgba);

}  // namespace oc::import
