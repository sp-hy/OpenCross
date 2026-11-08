#include "render/screen_pass.h"

#include "render/shaders.h"

namespace oc::render {

namespace {

#include "screen.vert.inc"
#include "screen.frag.inc"

// Push data of screen.frag: (size, ramp, filter), ramp colour, (mask, curvature, glow, grade), (tick, PS1 pixel, LUT size, LUT on).
constexpr u32 kPushFloats = 16;

}  // namespace

bool ScreenPass::init(GpuContext& ctx) {
  destroy();
  ctx_ = &ctx;
  vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, screen_vert, ShaderDesc{0, 0, 0}, "screen.vert");
  fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, screen_frag, ShaderDesc{2, 0, 1}, "screen.frag");
  if (!vs_ || !fs_ || !sampler_.create(ctx, true)) return false;
  PipelineDesc pd;
  pd.vertex = vs_;
  pd.fragment = fs_;
  pd.vertex_stride = 0;
  pd.blend = Blend::None;
  return pipeline_.create(ctx, pd);
}

void ScreenPass::destroy() {
  active_ = false;
  pipeline_.destroy();
  target_.destroy();
  luts_.clear();
  sampler_.destroy();
  if (ctx_) {
    if (vs_) SDL_ReleaseGPUShader(ctx_->device(), vs_);
    if (fs_) SDL_ReleaseGPUShader(ctx_->device(), fs_);
  }
  vs_ = fs_ = nullptr;
  set_ = {};
  ctx_ = nullptr;
}

bool ScreenPass::begin(GpuContext& ctx, const Frame& frame) {
  active_ = false;
  if (!pipeline_.handle()) return false;
  if (!target_.valid() || target_.width() != frame.extent.width || target_.height() != frame.extent.height) {
    target_.destroy();
    if (!target_.create(ctx, frame.extent.width, frame.extent.height, true)) return false;
    set_ = target_.ref(sampler_);
  }
  Frame inner;
  ctx.begin_target(target_, frame, inner);
  active_ = true;
  return true;
}

void ScreenPass::end(GpuContext& ctx, const Frame& frame) {
  if (!active_) return;
  active_ = false;
  ctx.end_target(target_);
  const Lut* lut = settings_.grade == ScreenSettings::Grade::Lut && settings_.lut >= 0 && settings_.lut < static_cast<int>(luts_.size())
                       ? &luts_[static_cast<size_t>(settings_.lut)]
                       : nullptr;
  const ScreenSettings::Grade grade = settings_.grade == ScreenSettings::Grade::Lut && !lut ? ScreenSettings::Grade::Off : settings_.grade;
  const float push[kPushFloats] = {static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), ramp_ ? 1.0f : 0.0f,
                                   static_cast<float>(settings_.filter),
                                   ramp_rgb_[0], ramp_rgb_[1], ramp_rgb_[2], 1.0f,
                                   static_cast<float>(settings_.mask), settings_.curvature ? 1.0f : 0.0f, settings_.glow ? 1.0f : 0.0f,
                                   static_cast<float>(grade),
                                   static_cast<float>(tick_ % 4096u), ps1_pixel_, lut ? static_cast<float>(lut->size) : 0.0f, lut ? 1.0f : 0.0f};
  SDL_BindGPUGraphicsPipeline(ctx.pass(), pipeline_.handle());
  const SDL_GPUTextureSamplerBinding bindings[2] = {{set_.texture, set_.sampler}, {lut ? lut->texture->handle() : set_.texture, set_.sampler}};
  SDL_BindGPUFragmentSamplers(ctx.pass(), 0, bindings, 2);
  SDL_PushGPUFragmentUniformData(ctx.cmd(), 0, push, sizeof push);
  SDL_DrawGPUPrimitives(ctx.pass(), 3, 1, 0, 0);
}

int ScreenPass::lut_index(std::string_view name) const {
  for (size_t i = 0; i < luts_.size(); ++i)
    if (luts_[i].name == name) return static_cast<int>(i);
  return -1;
}

bool ScreenPass::add_lut(const std::string& name, u32 width, u32 height, const u8* rgba) {
  if (!ctx_ || height < 2 || width != height * height) return false;
  auto texture = std::make_unique<Texture>();
  if (!texture->create(*ctx_, width, height, TextureFormat::Rgba8, rgba)) return false;
  const int existing = lut_index(name);
  if (existing >= 0) luts_[static_cast<size_t>(existing)] = {name, height, std::move(texture)};
  else luts_.push_back({name, height, std::move(texture)});
  return true;
}

}  // namespace oc::render
