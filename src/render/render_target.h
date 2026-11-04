#pragma once
// Off-screen colour + depth target for a low-resolution pass (classic art: the field drawn at the PS1's 320x224 and scaled up with
// nearest sampling). The colour image has the frame's colour format and the depth image the context's depth format, so every pipeline
// built for the main pass draws into it unchanged. Recorded with GpuContext::begin_target / end_target.
#include <vector>

#include "render/gpu_context.h"
#include "render/texture.h"

namespace oc::render {

// Field screen ripple (script ops 8F / 90). The PS1 field engine rebuilds it every frame at ordering-table index p8 (>= 7): a draw offset
// (buffer origin), 15 VRAM copies of the 64x16 blocks of buffer rows 176..223 to x 0x340, ONE copy of buffer rows 32..223 to rows 0..191,
// then opaque textured quads (colour 0x80, 15-bit texture of the buffer itself) as a 20 x 17 grid of 16 x 16 cells, drawn in this order:
// rows 13..0 (sampling the moved rows 16r.., drawn at 16r + 32: the copy is the feedback guard of the bottom-up order), then rows 16..14
// (sampling the block copies, drawn at 176..223, bottom pinned at 224). Grid line x of column c (1..19) = 16c + rcos(phase_x + c * step_x)
// * amp_x >> 12 (column 0 at 0, the right edge at 321); line y of row r = 16r + 32 + rcos(phase_y + r * step_y) * amp_y >> 12 for rows
// 1..12 (row 0 pinned at 32), rows 14..16 at 16r - 48 + rcos(phase_y + (r - 3) * step_y) * amp_y >> 12. Buffer rows 0..31 are never
// redrawn: they keep the moved copy (rows 32..63). Everything already in the frame at OT index > p8 is warped; what is drawn later
// (nearer) is not. The lines here take the exact cosine of fractional values instead of the table cosine truncated to whole pixels, so
// they move by sub-pixel amounts.
struct ScreenWarpParams {
  // p1 / p2: pixels.
  float amp_x = 0, amp_y = 0;
  // p3 / p4: phase step per column / row (4096 = one turn).
  float step_x = 0, step_y = 0;
  // Advanced by p5 / p6 every tick.
  float phase_x = 0, phase_y = 0;
};
// One quad in PS1 POLY_FT4 corner order (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right), in buffer pixels of the 320 x 224 field
// frame: xy = destination, uv = source texel coordinates in the frame as it was before the warp.
struct WarpQuad {
  float xy[8];
  float uv[8];
};
// The warped frame in draw order: the two quads of the moved base (rows 0..191 <- 32..223, rows 192..223 unchanged), then the grid cells in
// the PS1's execution order (later quads overwrite earlier ones; all opaque). `extra` = widescreen layer columns shown beyond each side of
// the 320 (render/camera.h wide_extra()): the grid continues over them in 16-px columns, edges pinned at -extra / 321 + extra.
void build_screen_warp(const ScreenWarpParams& params, std::vector<WarpQuad>& out, int extra = 0);

class RenderTarget {
 public:
  RenderTarget() = default;
  ~RenderTarget() { destroy(); }
  RenderTarget(const RenderTarget&) = delete;
  RenderTarget& operator=(const RenderTarget&) = delete;

  // `multisampled`: drawn with the context's MSAA (GpuContext::set_msaa) into a multisampled image that is resolved into color().
  bool create(GpuContext& ctx, u32 width, u32 height, bool multisampled = false);
  void destroy();
  // Matches the multisampled images to the context's MSAA (begin_target calls it); color() and the TexRefs of it stay valid.
  bool prepare_samples(GpuContext& ctx);
  // Frees the multisampled images (MSAA changed); prepare_samples makes them again.
  void release_samples();

  bool valid() const { return color_ != nullptr; }
  u32 width() const { return width_; }
  u32 height() const { return height_; }
  SDL_GPUTexture* color() const { return color_; }
  // Null when single-sampled.
  SDL_GPUTexture* msaa_color() const { return msaa_color_; }
  SDL_GPUTexture* depth() const { return depth_; }
  SDL_GPUSampleCount sample_count() const { return samples_; }
  // The colour image as sampled after GpuContext::end_target.
  TexRef ref(const Sampler& sampler) const { return {color_, sampler.handle(), width_, height_}; }

 private:
  GpuContext* ctx_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUTexture* color_ = nullptr;
  SDL_GPUTexture* msaa_color_ = nullptr;
  SDL_GPUTexture* depth_ = nullptr;
  u32 width_ = 0, height_ = 0;
  bool multisampled_ = false;
  SDL_GPUSampleCount samples_ = SDL_GPU_SAMPLECOUNT_1;
};

}  // namespace oc::render
