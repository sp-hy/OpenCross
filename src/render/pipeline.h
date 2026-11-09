#pragma once
// Graphics pipeline for the frame / target images: formats come from the GpuContext, viewport/scissor are per render pass.
#include <span>
#include <vector>

#include "render/gpu_context.h"

namespace oc::render {

// Multiply: dst = src * dst; Subtract: dst = dst - src; Premultiplied: dst = src + dst * (1 - src alpha) (compositing an off-screen
// target that was drawn with Alpha over a transparent clear: its colour is already multiplied by its coverage); Invert: out = src *
// (1 - dst) (a white source turns the backdrop into its negative); AdditiveAlpha: dst = dst + src * src alpha (straight-alpha add),
// destination alpha kept (a premultiplied composite of the target then adds it over the picture); Max / Min: dst = the larger / smaller of src and dst per channel, alpha too.
// AdditiveKeepAlpha / SubtractKeepAlpha / MultiplyKeepAlpha: the colour of Additive / Subtract / Multiply, destination alpha kept (drawn into
// a transparent off-screen target, they change what the target adds without making it cover the picture).
enum class Blend { None, Alpha, Additive, Multiply, Subtract, Premultiplied, Invert, AdditiveAlpha, Max, Min, AdditiveKeepAlpha, SubtractKeepAlpha, MultiplyKeepAlpha };

struct VertexAttribute {
  u32 location = 0;
  SDL_GPUVertexElementFormat format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
  u32 offset = 0;
};

struct PipelineDesc {
  SDL_GPUShader* vertex = nullptr;
  SDL_GPUShader* fragment = nullptr;
  // 0 = no vertex buffer (a full-screen triangle from gl_VertexIndex).
  u32 vertex_stride = 0;
  std::span<const VertexAttribute> attributes;
  SDL_GPUPrimitiveType topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  SDL_GPUCullMode cull = SDL_GPU_CULLMODE_NONE;
  Blend blend = Blend::None;
  bool depth_test = false;
  bool depth_write = false;
};

class Pipeline {
 public:
  Pipeline() = default;
  ~Pipeline() { destroy(); }
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // The shaders of `desc` must live until destroy(): the multisampled variants are built from them on first use.
  bool create(GpuContext& ctx, const PipelineDesc& desc);
  void destroy();

  // The variant for the sample count of the render pass in progress (GpuContext::pass_sample_count).
  SDL_GPUGraphicsPipeline* handle() const;

 private:
  SDL_GPUGraphicsPipeline* build(SDL_GPUSampleCount samples) const;

  GpuContext* ctx_ = nullptr;
  PipelineDesc desc_;
  std::vector<VertexAttribute> attributes_;
  // Indexed by SDL_GPUSampleCount (1, 2, 4, 8 samples).
  mutable SDL_GPUGraphicsPipeline* variants_[4] = {};
  mutable bool failed_[4] = {};
};

}  // namespace oc::render
