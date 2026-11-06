#include "render/particles.h"

#include "core/log.h"
#include "render/shaders.h"

namespace oc::render {

namespace {

#include "particle.vert.inc"

// texel * colour / 0x80: the vertex colour byte 0x80 is neutral, so the unorm value is scaled by 2 (up to ~2x brightening).
#include "particle.frag.inc"

constexpr Blend kBlendFor[kParticleBlendModes] = {Blend::None, Blend::Alpha, Blend::Additive, Blend::Subtract, Blend::Additive, Blend::Invert};

}  // namespace

bool ParticleBatch::init(GpuContext& ctx) {
  destroy();
  ctx_ = &ctx;

  vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, particle_vert, ShaderDesc{0, 0, 1}, "particle.vert");
  fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, particle_frag, ShaderDesc{1, 0, 1}, "particle.frag");
  if (!vs_ || !fs_) return false;

  const VertexAttribute attributes[] = {
      {0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, x)},
      {1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, u)},
      {2, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(SpriteVertex, color)},
      {3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(SpriteVertex, z)},
  };
  for (int mode = 0; mode < kParticleBlendModes; ++mode) {
    PipelineDesc pd;
    pd.vertex = vs_;
    pd.fragment = fs_;
    pd.vertex_stride = sizeof(SpriteVertex);
    pd.attributes = attributes;
    pd.blend = kBlendFor[mode];
    pd.depth_test = true;
    pd.depth_write = false;
    if (!pipelines_[static_cast<size_t>(mode)].create(ctx, pd)) return false;
  }

  if (!create_quad_index_buffer(ctx, index_buffer_, kMaxQuads)) return false;
  return vertices_.create(ctx, sizeof(SpriteVertex) * 4 * kMaxQuads, SDL_GPU_BUFFERUSAGE_VERTEX);
}

void ParticleBatch::destroy() {
  vertices_.destroy();
  index_buffer_.destroy();
  for (Pipeline& pipeline : pipelines_) pipeline.destroy();
  if (ctx_) {
    if (vs_) SDL_ReleaseGPUShader(ctx_->device(), vs_);
    if (fs_) SDL_ReleaseGPUShader(ctx_->device(), fs_);
  }
  vs_ = fs_ = nullptr;
  ctx_ = nullptr;
}

// A new frame starts the quad buffer over even when resumed (as SpriteBatch::begin). The view transform is kept in push_ and
// re-pushed on every blend-mode change.
void ParticleBatch::begin(const Frame& frame, float view_width, float view_height, bool resume) {
  vertex_data_ = static_cast<SpriteVertex*>(vertices_.begin(frame));
  if (!resume || frame.serial != last_serial_) quad_count_ = 0;
  last_serial_ = frame.serial;
  flushed_quads_ = quad_count_;
  current_ = {};
  mode_ = -1;
  overflow_logged_ = false;
  SDL_GPUBufferBinding vertex_binding{vertices_.handle(), 0};
  SDL_BindGPUVertexBuffers(ctx_->pass(), 0, &vertex_binding, 1);
  SDL_GPUBufferBinding index_binding{index_buffer_.handle(), 0};
  SDL_BindGPUIndexBuffer(ctx_->pass(), &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
  push_[0] = 2.0f / view_width;
  push_[1] = 2.0f / view_height;
  push_[2] = -1.0f;
  push_[3] = -1.0f;
}

void ParticleBatch::flush() {
  if (quad_count_ == flushed_quads_ || !current_ || mode_ < 0) return;
  vertices_.mark_used(quad_count_ * 4 * sizeof(SpriteVertex));
  const SDL_GPUTextureSamplerBinding binding{current_.texture, current_.sampler};
  SDL_BindGPUFragmentSamplers(ctx_->pass(), 0, &binding, 1);
  SDL_DrawGPUIndexedPrimitives(ctx_->pass(), (quad_count_ - flushed_quads_) * 6, 1, 0, static_cast<s32>(flushed_quads_ * 4), 0);
  flushed_quads_ = quad_count_;
}

void ParticleBatch::quad(TexRef texture, int blend, const float x[4], const float y[4], const float u[4],
                         const float v[4], u8 mod_r, u8 mod_g, u8 mod_b, float depth) {
  if (blend < 0 || blend >= kParticleBlendModes) blend = 0;
  if (quad_count_ >= kMaxQuads) {
    if (!overflow_logged_) log::warn("ParticleBatch: more than {} quads in a frame, dropping the rest", kMaxQuads);
    overflow_logged_ = true;
    return;
  }
  if (blend != mode_) {
    flush();
    mode_ = blend;
    const Pipeline& pipeline = pipelines_[static_cast<size_t>(blend)];
    SDL_BindGPUGraphicsPipeline(ctx_->pass(), pipeline.handle());
    SDL_PushGPUVertexUniformData(ctx_->cmd(), 0, push_, sizeof push_);
    const float mode[4] = {full_precision_ ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(ctx_->cmd(), 0, mode, sizeof mode);
  }
  if (texture != current_) {
    flush();
    current_ = texture;
  }
  // Blend 1 (0.5 B + 0.5 F) goes through alpha 128; blend 4 (B + 0.25 F) through additive blending of a quartered colour.
  u8 r = mod_r, g = mod_g, b = mod_b, a = 255;
  if (blend == 1) a = 128;
  if (blend == 4) {
    r = static_cast<u8>(r / 4);
    g = static_cast<u8>(g / 4);
    b = static_cast<u8>(b / 4);
  }
  const u32 col = pack_rgba(r, g, b, a);
  SpriteVertex* vt = vertex_data_ + static_cast<size_t>(quad_count_) * 4;
  for (int k = 0; k < 4; ++k) vt[k] = {x[k], y[k], u[k], v[k], col, depth};
  ++quad_count_;
}

void ParticleBatch::quad(TexRef texture, int blend, const float x[4], const float y[4], const float u[4], const float v[4],
                         const u8 rgb[4][3], float depth) {
  const u32 first = quad_count_;
  quad(texture, blend, x, y, u, v, rgb[0][0], rgb[0][1], rgb[0][2], depth);
  if (quad_count_ == first) return;
  if (blend < 0 || blend >= kParticleBlendModes) blend = 0;
  SpriteVertex* vt = vertex_data_ + static_cast<size_t>(first) * 4;
  for (int k = 0; k < 4; ++k) {
    u8 r = rgb[k][0], g = rgb[k][1], b = rgb[k][2];
    if (blend == 4) r = static_cast<u8>(r / 4), g = static_cast<u8>(g / 4), b = static_cast<u8>(b / 4);
    vt[k].color = pack_rgba(r, g, b, static_cast<u8>(vt[k].color >> 24));
  }
}

void ParticleBatch::end() {
  flush();
  vertex_data_ = nullptr;
}

}  // namespace oc::render
