#pragma once
// [Shaders] full-screen pass. The whole frame up to the ImGui overlay is drawn into an off-screen target of the frame's size (begin), then
// drawn to the frame through the screen shader (end); the overlay and the toasts follow on the frame itself, so they are not filtered. The
// shader applies, in order: the main filter (CRT or composite video) and the colour grade (a built-in look or a mod's LUT).
// The pause screen uses the same pass for its monochrome ramp, which replaces everything else.
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "render/pipeline.h"
#include "render/render_target.h"
#include "render/texture.h"

namespace oc::render {

struct ScreenSettings {
  enum class Filter : u8 { Off, Crt, Composite };
  enum class Mask : u8 { Classic, ApertureGrille, ShadowMask };
  // Built-in grades; Lut = the LUT `lut` (ScreenPass::lut_index).
  enum class Grade : u8 { Off, Film, Warm, Cold, Faded, Lut };
  Filter filter = Filter::Off;
  Mask mask = Mask::Classic;
  bool curvature = false;
  bool glow = false;
  Grade grade = Grade::Off;
  int lut = -1;
  bool active() const { return filter != Filter::Off || grade != Grade::Off; }
};

class ScreenPass {
 public:
  ScreenPass() = default;
  ~ScreenPass() { destroy(); }
  ScreenPass(const ScreenPass&) = delete;
  ScreenPass& operator=(const ScreenPass&) = delete;

  bool init(GpuContext& ctx);
  void destroy();

  // Redirects the frame's drawing into the pass's target (created / resized to the frame's extent). False = could not (draw normally).
  bool begin(GpuContext& ctx, const Frame& frame);
  // Ends the redirect and draws the target to the frame through the screen shader.
  void end(GpuContext& ctx, const Frame& frame);
  bool active() const { return active_; }
  // The next end() draws the monochrome ramp (the pause screen filter) instead: out = L * rgb with L the luminance of the picture (RDE
  // cycles that constant colour while the pause menu is up).
  void set_ramp(bool on, const float (&rgb)[3]) {
    ramp_ = on;
    for (int i = 0; i < 3; ++i) ramp_rgb_[i] = rgb[i];
  }
  // The look of the next end(). `ps1_pixel` = frame pixels per PS1 picture pixel (the composite filter and the CRT glow work in
  // those units); `tick` animates the composite dot crawl.
  void set_settings(const ScreenSettings& settings, float ps1_pixel, u32 tick) {
    settings_ = settings;
    ps1_pixel_ = ps1_pixel;
    tick_ = tick;
  }

  // Colour-grade LUTs: an image of N x N squares side by side (width N * N, height N; red across a square, green down, blue square by
  // square). add_lut adds or replaces one by name (false = not that shape or no texture); clear_luts drops them all.
  int lut_index(std::string_view name) const;
  bool add_lut(const std::string& name, u32 width, u32 height, const u8* rgba);
  void clear_luts() { luts_.clear(); }

 private:
  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  TexRef set_;
  Pipeline pipeline_;
  Sampler sampler_;
  RenderTarget target_;
  bool active_ = false;
  bool ramp_ = false;
  float ramp_rgb_[3] = {1, 1, 1};
  ScreenSettings settings_;
  float ps1_pixel_ = 1.0f;
  u32 tick_ = 0;
  struct Lut {
    std::string name;
    u32 size = 0;
    std::unique_ptr<Texture> texture;
  };
  std::vector<Lut> luts_;
};

}  // namespace oc::render
