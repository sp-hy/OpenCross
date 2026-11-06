#include "render/render_target.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace oc::render {

namespace {

// One grid line's offset at `angle` (phase + index * step, 4096 = one turn) with amplitude `amp`, in buffer pixels.
float warp_offset(float angle, float amp) {
  return static_cast<float>(std::cos(static_cast<double>(angle) * (6.283185307179586 / 4096.0)) * static_cast<double>(amp));
}

}  // namespace

// The grid is emitted bottom-up (row 13 first) as the PS1 links each row's quads after the previous row's; row r samples the moved rows
// 16r.. (frame rows 16r + 32; rows 12 / 13 read the unmoved rows 192..223), and row 13's column 0 takes dy(13) where the other columns
// take dy(14). One departure from the PS1: the grid continues over the top 32 rows too (rows -2 / -1, row 0 unpinned), so the whole
// picture ripples where the PS1 left rows 0..31 as the unwarped moved copy. Widescreen adds `extra` columns per side in 16-px steps
// continuing the same wave, the outer edges pinned at -extra and 321 + extra (no extra columns: the PS1 grid exactly).
void build_screen_warp(const ScreenWarpParams& params, std::vector<WarpQuad>& out, int extra) {
  out.clear();
  extra = std::max(0, extra);
  const float extra_f = static_cast<float>(extra);
  // Corners 0 (x0, top), 1 (x1, top), 2 (x0, bottom), 3 (x1, bottom); the top / bottom edge heights may differ per corner.
  auto quad = [&](float x0, float x1, float y00, float y01, float y10, float y11, float u0, float u1, float v0) {
    WarpQuad warp_quad;
    const float xy[8] = {x0, y00, x1, y01, x0, y10, x1, y11};
    const float uv[8] = {u0, v0, u1, v0, u0, v0 + 16.0f, u1, v0 + 16.0f};
    std::copy(xy, xy + 8, warp_quad.xy);
    std::copy(uv, uv + 8, warp_quad.uv);
    out.push_back(warp_quad);
  };
  {
    WarpQuad moved_base{{-extra_f, 0, 320 + extra_f, 0, -extra_f, 192, 320 + extra_f, 192},
                        {-extra_f, 32, 320 + extra_f, 32, -extra_f, 224, 320 + extra_f, 224}};
    WarpQuad kept_base{{-extra_f, 192, 320 + extra_f, 192, -extra_f, 224, 320 + extra_f, 224},
                       {-extra_f, 192, 320 + extra_f, 192, -extra_f, 224, 320 + extra_f, 224}};
    out.push_back(moved_base);
    out.push_back(kept_base);
  }
  const int extra_cols = (extra + 15) / 16, cols = 20 + 2 * extra_cols;
  std::vector<float> grid_x(static_cast<size_t>(cols) + 1);
  for (int i = 0; i <= cols; ++i) {
    const int col = i - extra_cols;
    grid_x[static_cast<size_t>(i)] = i == 0      ? -extra_f
                                     : i == cols ? 321.0f + extra_f
                                                 : static_cast<float>(16 * col) + warp_offset(params.phase_x + static_cast<float>(col) * params.step_x, params.amp_x);
  }
  auto u_at = [&](int i) { return i == 0 ? -extra_f : i == cols ? 320.0f + extra_f : static_cast<float>(16 * (i - extra_cols)); };
  auto dy = [&](int row) { return warp_offset(params.phase_y + static_cast<float>(row) * params.step_y, params.amp_y); };
  for (int row = 13; row >= -2; --row) {
    const float v0 = static_cast<float>(row <= 11 ? 16 * row + 32 : 16 * row);
    for (int i = cols - 1; i >= 0; --i) {
      const int col = i - extra_cols;
      float top, bottom;
      if (row == -2) top = 0.0f;
      else if (row <= 12) top = static_cast<float>(16 * row + 32 + dy(row));
      else top = static_cast<float>(240 + (col == 0 ? dy(13) : dy(14)));
      if (row <= 11) bottom = static_cast<float>(16 * (row + 1) + 32 + dy(row + 1));
      else if (row == 12) bottom = static_cast<float>(240 + (col == 0 ? dy(13) : dy(14)));
      else bottom = static_cast<float>(240 + dy(14));
      quad(grid_x[static_cast<size_t>(i)], grid_x[static_cast<size_t>(i) + 1], top, top, bottom, bottom, u_at(i), u_at(i + 1), v0);
    }
  }
  for (int row = 16; row >= 14; --row) {
    const float v0 = static_cast<float>(176 + 16 * (row - 14));
    const float top = static_cast<float>(16 * row - 48 + dy(row - 3));
    const float bottom = row == 16 ? 224.0f : static_cast<float>(16 * (row + 1) - 48 + dy(row - 2));
    for (int i = cols - 1; i >= 0; --i)
      quad(grid_x[static_cast<size_t>(i)], grid_x[static_cast<size_t>(i) + 1], top, top, bottom, bottom, u_at(i), u_at(i + 1), v0);
  }
}


bool RenderTarget::create(GpuContext& ctx, u32 width, u32 height, bool multisampled) {
  destroy();
  ctx_ = &ctx;
  device_ = ctx.device();
  width_ = width;
  height_ = height;
  multisampled_ = multisampled;
  if (multisampled) ctx.track_multisampled(this, true);
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  info.format = ctx.color_format();
  info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  color_ = SDL_CreateGPUTexture(device_, &info);
  if (!color_ || !prepare_samples(ctx)) {
    log::error("render target {}x{}: SDL_CreateGPUTexture: {}", width, height, SDL_GetError());
    destroy();
    return false;
  }
  return true;
}

// A multisampled target that cannot get its images falls back to single-sampled.
bool RenderTarget::prepare_samples(GpuContext& ctx) {
  const SDL_GPUSampleCount wanted = multisampled_ ? ctx.sample_count_for(width_, height_) : SDL_GPU_SAMPLECOUNT_1;
  if (depth_ && wanted == samples_) return true;
  for (const SDL_GPUSampleCount samples : {wanted, SDL_GPU_SAMPLECOUNT_1}) {
    release_samples();
    samples_ = samples;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width_;
    info.height = height_;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = samples;
    if (samples != SDL_GPU_SAMPLECOUNT_1) {
      info.format = ctx.color_format();
      info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
      msaa_color_ = SDL_CreateGPUTexture(device_, &info);
    }
    info.format = ctx.depth_format();
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_ = SDL_CreateGPUTexture(device_, &info);
    if (depth_ && (msaa_color_ || samples == SDL_GPU_SAMPLECOUNT_1)) return true;
    log::error("render target {}x{} ({} samples): SDL_CreateGPUTexture: {}", width_, height_, 1u << static_cast<u32>(samples), SDL_GetError());
  }
  return false;
}

void RenderTarget::release_samples() {
  if (!ctx_) return;
  const u64 bytes = u64{width_} * height_ * 4 * (u64{1} << static_cast<u32>(samples_));
  ctx_->release_texture(msaa_color_, bytes);
  ctx_->release_texture(depth_, bytes);
  msaa_color_ = depth_ = nullptr;
}

void RenderTarget::destroy() {
  if (ctx_ && multisampled_) ctx_->track_multisampled(this, false);
  if (ctx_) ctx_->release_texture(color_, u64{width_} * height_ * 4);
  release_samples();
  color_ = msaa_color_ = depth_ = nullptr;
  samples_ = SDL_GPU_SAMPLECOUNT_1;
  multisampled_ = false;
  device_ = nullptr;
  ctx_ = nullptr;
}

}  // namespace oc::render
