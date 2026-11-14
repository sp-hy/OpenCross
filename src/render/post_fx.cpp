#include "render/post_fx.h"

#include <algorithm>
#include <cmath>

#include "render/shaders.h"

namespace oc::render {

namespace {

#include "screen.vert.inc"
#include "post_fx.frag.inc"

// post_fx.frag's modes.
enum Mode { kSsao = 0, kAoBlur = 1, kDofFilter = 2, kDofBlur = 3, kCombine = 4, kComposite = 5 };

}  // namespace

bool PostFx::init(GpuContext& ctx) {
  if (ctx_) return pipeline_.handle() != nullptr;
  ctx_ = &ctx;
  vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, screen_vert, ShaderDesc{0, 0, 0}, "screen.vert");
  fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, post_fx_frag, ShaderDesc{3, 0, 1}, "post_fx.frag");
  if (!vs_ || !fs_ || !nearest_.create(ctx, false) || !linear_.create(ctx, true)) return false;
  PipelineDesc pd;
  pd.vertex = vs_;
  pd.fragment = fs_;
  pd.vertex_stride = 0;
  pd.blend = Blend::None;
  if (!pipeline_.create(ctx, pd)) return false;
  pd.blend = Blend::Premultiplied;
  return pipeline_over_.create(ctx, pd);
}

void PostFx::destroy() {
  if (!ctx_) return;
  pipeline_.destroy();
  pipeline_over_.destroy();
  depth_.destroy();
  result_.destroy();
  for (RenderTarget& target : ao_) target.destroy();
  for (RenderTarget& target : dof_) target.destroy();
  nearest_.destroy();
  linear_.destroy();
  if (vs_) SDL_ReleaseGPUShader(ctx_->device(), vs_);
  if (fs_) SDL_ReleaseGPUShader(ctx_->device(), fs_);
  vs_ = fs_ = nullptr;
  scene_ = {};
  ctx_ = nullptr;
}

bool PostFx::ensure(RenderTarget& target, u32 width, u32 height) {
  if (target.valid() && target.width() == width && target.height() == height) return true;
  return target.create(*ctx_, width, height);
}

bool PostFx::begin(GpuContext& ctx, const Frame& frame, const Rect& scissor, Frame& depth_frame, Rect& depth_scissor) {
  if (!init(ctx) || frame.extent.width == 0 || frame.extent.height == 0) return false;
  // A picture larger than the window (render scale) is shown at window size: the passes run at that size.
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float scale = std::min({1.0f, static_cast<float>(ctx.extent().width) / fw, static_cast<float>(ctx.extent().height) / fh});
  const u32 width = std::max(1u, static_cast<u32>(std::lround(fw * scale))), height = std::max(1u, static_cast<u32>(std::lround(fh * scale)));
  if (!ensure(depth_, width, height)) return false;
  scene_ = ctx.suspend_rendering(linear_.handle());
  if (!scene_) return false;
  frame_ = frame;
  static constexpr float kFar[4] = {1, 1, 1, 1};
  ctx.begin_target(depth_, frame, depth_frame, kFar);
  const float sx = static_cast<float>(width) / fw, sy = static_cast<float>(height) / fh;
  const s32 x0 = static_cast<s32>(std::floor(static_cast<float>(scissor.x) * sx)), y0 = static_cast<s32>(std::floor(static_cast<float>(scissor.y) * sy));
  const s32 x1 = static_cast<s32>(std::ceil(static_cast<float>(scissor.x + static_cast<s32>(scissor.w)) * sx));
  const s32 y1 = static_cast<s32>(std::ceil(static_cast<float>(scissor.y + static_cast<s32>(scissor.h)) * sy));
  depth_scissor = {x0, y0, static_cast<u32>(std::max(0, x1 - x0)), static_cast<u32>(std::max(0, y1 - y0))};
  return true;
}

void PostFx::pass(GpuContext& ctx, int mode, const TexRef (&textures)[3], const float (&push)[16], bool over) {
  float data[16];
  std::copy(std::begin(push), std::end(push), data);
  data[0] = static_cast<float>(mode);
  SDL_BindGPUGraphicsPipeline(ctx.pass(), (over ? pipeline_over_ : pipeline_).handle());
  SDL_GPUTextureSamplerBinding bindings[3];
  for (size_t i = 0; i < 3; ++i) bindings[i] = {textures[i].texture, textures[i].sampler};
  SDL_BindGPUFragmentSamplers(ctx.pass(), 0, bindings, 3);
  SDL_PushGPUFragmentUniformData(ctx.cmd(), 0, data, sizeof data);
  SDL_DrawGPUPrimitives(ctx.pass(), 3, 1, 0, 0);
}

void PostFx::end(GpuContext& ctx, const Rect& scissor, const PostFxView& view) {
  ctx.end_target(depth_);
  const u32 half_w = std::max(1u, (depth_.width() + 1) / 2), half_h = std::max(1u, (depth_.height() + 1) / 2);
  const TexRef depth = depth_.ref(nearest_);
  const float max_blur = kMaxBlur * static_cast<float>(depth_.height()) * 0.5f * std::clamp(view.dof_amount, 0.0f, 1.0f);
  float push[16] = {0, view.ssao ? 1.0f : 0.0f, view.dof ? 1.0f : 0.0f, max_blur, view.ax, view.cx, view.ay, view.cy,
                    std::max(view.focus, 1.0f), kAoRadius, 0, 0, kFocusNear, kFocusFar, kFocusRamp, kFocusNearRamp};
  auto run = [&](RenderTarget& target, int mode, const TexRef (&textures)[3]) {
    Frame out;
    ctx.begin_target(target, frame_, out);
    pass(ctx, mode, textures, push, false);
    ctx.end_target(target);
  };
  bool ssao = view.ssao && ensure(ao_[0], half_w, half_h) && ensure(ao_[1], half_w, half_h);
  bool dof = view.dof && ensure(dof_[0], half_w, half_h) && ensure(dof_[1], half_w, half_h);
  if (ssao) {
    run(ao_[0], kSsao, {depth, depth, depth});
    push[10] = 1, push[11] = 0;
    run(ao_[1], kAoBlur, {ao_[0].ref(nearest_), depth, depth});
    push[10] = 0, push[11] = 1;
    run(ao_[0], kAoBlur, {ao_[1].ref(nearest_), depth, depth});
  } else {
    for (RenderTarget& target : ao_) target.destroy();
  }
  if (!dof) {
    for (RenderTarget& target : dof_) target.destroy();
  }
  dof = dof && max_blur > 0.01f;
  if (dof) {
    run(dof_[0], kDofFilter, {scene_, depth, depth});
    run(dof_[1], kDofBlur, {dof_[0].ref(linear_), depth, depth});
  }
  push[1] = ssao ? 1.0f : 0.0f;
  push[2] = dof ? 1.0f : 0.0f;
  const TexRef ao = ssao ? ao_[0].ref(nearest_) : depth, blurred = dof ? dof_[1].ref(nearest_) : depth;
  const bool direct = depth_.width() == scene_.width && depth_.height() == scene_.height;
  if (!direct && !ensure(result_, depth_.width(), depth_.height())) ssao = dof = false;
  if (!direct && (ssao || dof)) run(result_, kCombine, {ao, blurred, depth});
  if (direct) result_.destroy();
  ctx.resume_rendering();
  if (!ssao && !dof) return;
  ctx.set_scissor(scissor);
  if (direct) pass(ctx, kCombine, {ao, blurred, depth}, push, true);
  else pass(ctx, kComposite, {result_.ref(linear_), depth, depth}, push, true);
  ctx.set_scissor({0, 0, scene_.width, scene_.height});
}

}  // namespace oc::render
