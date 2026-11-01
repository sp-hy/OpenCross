#include "render/sprite_batch.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "render/shaders.h"

namespace oc::render {

namespace {

#include "sprite.vert.inc"

// `filter` (SpriteBatch::set_filter): 0 = the texture's sampler; 1 = point: the centre of the texel under the fragment (nearest
// filtering with any sampler); 2 = box: the average of 4 x 4 point taps spread over the pixel's footprint in the texture (an area
// filter: every texel the pixel covers contributes by its covered share, whether the texture is minified or magnified).
#include "sprite.frag.inc"

}  // namespace

bool create_quad_index_buffer(GpuContext& ctx, Buffer& buffer, u32 quad_count) {
  std::vector<u16> indices(static_cast<size_t>(quad_count) * 6);
  for (u32 quad = 0; quad < quad_count; ++quad) {
    const u16 base = static_cast<u16>(quad * 4);
    u16* quad_indices = &indices[static_cast<size_t>(quad) * 6];
    quad_indices[0] = base;
    quad_indices[1] = static_cast<u16>(base + 1);
    quad_indices[2] = static_cast<u16>(base + 2);
    quad_indices[3] = static_cast<u16>(base + 2);
    quad_indices[4] = static_cast<u16>(base + 1);
    quad_indices[5] = static_cast<u16>(base + 3);
  }
  return buffer.create(ctx, indices.data(), static_cast<u32>(indices.size() * sizeof(u16)), SDL_GPU_BUFFERUSAGE_INDEX);
}

bool SpriteBatch::init(GpuContext& ctx) {
  destroy();
  ctx_ = &ctx;

  vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, sprite_vert, ShaderDesc{0, 0, 1}, "sprite.vert");
  fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, sprite_frag, ShaderDesc{1, 0, 1}, "sprite.frag");
  if (!vs_ || !fs_) return false;

  const VertexAttribute attributes[] = {
      {0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, x)},
      {1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteVertex, u)},
      {2, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(SpriteVertex, color)},
      {3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(SpriteVertex, z)},
  };
  if (const char* env = std::getenv("OC_LOWRES_LOG"); env && *env && *env != '0') log_on_ = true;
  PipelineDesc pd;
  pd.vertex = vs_;
  pd.fragment = fs_;
  pd.vertex_stride = sizeof(SpriteVertex);
  pd.attributes = attributes;
  pd.blend = Blend::Alpha;
  if (!pipeline_.create(ctx, pd)) return false;
  pd.depth_test = true;
  pd.depth_write = true;
  if (!pipeline_depth_.create(ctx, pd)) return false;
  pd.depth_test = false;
  pd.depth_write = false;
  pd.blend = Blend::Multiply;
  if (!pipeline_mul_.create(ctx, pd)) return false;
  pd.blend = Blend::Additive;
  if (!pipeline_add_.create(ctx, pd)) return false;
  pd.blend = Blend::Subtract;
  if (!pipeline_sub_.create(ctx, pd)) return false;
  pd.depth_test = true;
  pd.blend = Blend::Multiply;
  if (!pipeline_mul_depth_.create(ctx, pd)) return false;
  pd.blend = Blend::Additive;
  if (!pipeline_add_depth_.create(ctx, pd)) return false;
  pd.blend = Blend::Subtract;
  if (!pipeline_sub_depth_.create(ctx, pd)) return false;
  pd.blend = Blend::Alpha;
  if (!pipeline_avg_depth_.create(ctx, pd)) return false;
  pd.depth_test = false;
  pd.blend = Blend::Premultiplied;
  if (!pipeline_pre_.create(ctx, pd)) return false;
  pd.blend = Blend::AdditiveAlpha;
  if (!pipeline_add_alpha_.create(ctx, pd)) return false;

  if (!create_quad_index_buffer(ctx, index_buffer_, kMaxQuads)) return false;
  return vertices_.create(ctx, sizeof(SpriteVertex) * 4 * kMaxQuads, SDL_GPU_BUFFERUSAGE_VERTEX);
}

void SpriteBatch::destroy() {
  vertices_.destroy();
  index_buffer_.destroy();
  pipeline_.destroy();
  pipeline_depth_.destroy();
  pipeline_mul_.destroy();
  pipeline_add_.destroy();
  pipeline_sub_.destroy();
  pipeline_mul_depth_.destroy();
  pipeline_add_depth_.destroy();
  pipeline_sub_depth_.destroy();
  pipeline_avg_depth_.destroy();
  pipeline_pre_.destroy();
  pipeline_add_alpha_.destroy();
  if (ctx_) {
    if (vs_) SDL_ReleaseGPUShader(ctx_->device(), vs_);
    if (fs_) SDL_ReleaseGPUShader(ctx_->device(), fs_);
  }
  vs_ = fs_ = nullptr;
  ctx_ = nullptr;
}

// A pass of a new frame starts the quad buffer over even when it asks to resume (the frame's first pass may be a resumed one when an
// earlier pass of that frame drew nothing): a changed serial = a new frame.
void SpriteBatch::begin(const Frame& frame, float view_width, float view_height, bool resume) {
  vertex_data_ = static_cast<SpriteVertex*>(vertices_.begin(frame));
  if (!resume || frame.serial != last_serial_) quad_count_ = 0;
  last_serial_ = frame.serial;
  flushed_quads_ = quad_count_;
  current_ = {};
  depth_mode_ = false;
  mode_ = BlendMode::Alpha;
  view_transform_[0] = 2.0f / view_width;
  view_transform_[1] = 2.0f / view_height;
  view_transform_[2] = -1.0f;
  view_transform_[3] = -1.0f;
  bind_pipeline();
  SDL_GPUBufferBinding vertex_binding{vertices_.handle(), 0};
  SDL_BindGPUVertexBuffers(ctx_->pass(), 0, &vertex_binding, 1);
  SDL_GPUBufferBinding index_binding{index_buffer_.handle(), 0};
  SDL_BindGPUIndexBuffer(ctx_->pass(), &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
}

void SpriteBatch::flush() {
  if (quad_count_ == flushed_quads_ || !current_) return;
  vertices_.mark_used(quad_count_ * 4 * sizeof(SpriteVertex));
  const SDL_GPUTextureSamplerBinding binding{current_.texture, current_.sampler};
  SDL_BindGPUFragmentSamplers(ctx_->pass(), 0, &binding, 1);
  SDL_DrawGPUIndexedPrimitives(ctx_->pass(), (quad_count_ - flushed_quads_) * 6, 1, 0, static_cast<s32>(flushed_quads_ * 4), 0);
  flushed_quads_ = quad_count_;
}

void SpriteBatch::draw(TexRef texture, float x, float y, float w, float h, float u0, float v0, float u1,
                       float v1, u32 color) {
  quad(false, texture, x, y, w, h, 0.0f, u0, v0, u1, v1, color);
}

void SpriteBatch::draw_depth(TexRef texture, float x, float y, float w, float h, float depth, float u0,
                             float v0, float u1, float v1, u32 color) {
  quad(true, texture, x, y, w, h, depth, u0, v0, u1, v1, color);
}

// quad() switches the pipeline / texture state and takes one slot; its corners are then rewritten with the free ones.
void SpriteBatch::draw_quad_depth(TexRef texture, const float (&xy)[8], const float (&uv)[8], float depth, u32 color) {
  if (quad_count_ >= kMaxQuads) return;
  quad(true, texture, 0, 0, 0, 0, depth, 0, 0, 0, 0, color);
  if (quad_count_ == 0) return;
  SpriteVertex* corners = vertex_data_ + static_cast<size_t>(quad_count_ - 1) * 4;
  for (size_t i = 0; i < 4; ++i) corners[i] = {xy[i * 2], xy[i * 2 + 1], uv[i * 2], uv[i * 2 + 1], color, depth};
}

void SpriteBatch::draw_quad(TexRef texture, const float (&xy)[8], const float (&uv)[8], u32 color) {
  if (quad_count_ >= kMaxQuads) return;
  quad(false, texture, 0, 0, 0, 0, 0, 0, 0, 0, 0, color);
  if (quad_count_ == 0) return;
  SpriteVertex* corners = vertex_data_ + static_cast<size_t>(quad_count_ - 1) * 4;
  for (size_t i = 0; i < 4; ++i) corners[i] = {xy[i * 2], xy[i * 2 + 1], uv[i * 2], uv[i * 2 + 1], color, 0.0f};
}

void SpriteBatch::quad(bool depth, TexRef texture, float x, float y, float w, float h, float z, float u0, float v0,
                       float u1, float v1, u32 color) {
  if (quad_count_ >= kMaxQuads) {
    if (!overflow_logged_) log::warn("SpriteBatch: more than {} quads in a frame, dropping the rest", kMaxQuads);
    overflow_logged_ = true;
    return;
  }
  if (depth != depth_mode_) {
    flush();
    depth_mode_ = depth;
    bind_pipeline();
  }
  if (texture != current_) {
    flush();
    current_ = texture;
    if (log_on_ && !log_tag_.empty()) {
      const std::string key = log_tag_ + "#" + std::to_string(reinterpret_cast<uintptr_t>(texture.texture));
      if (logged_.insert(key).second) log::info("{}: texture {} x {}", log_tag_, texture.width, texture.height);
    }
  }
  SpriteVertex* corners = vertex_data_ + static_cast<size_t>(quad_count_) * 4;
  corners[0] = {x, y, u0, v0, color, z};
  corners[1] = {x + w, y, u1, v0, color, z};
  corners[2] = {x, y + h, u0, v1, color, z};
  corners[3] = {x + w, y + h, u1, v1, color, z};
  ++quad_count_;
}

void SpriteBatch::end() {
  flush();
  vertex_data_ = nullptr;
}

void SpriteBatch::bind_pipeline() {
  const Pipeline& pipeline = mode_ == BlendMode::Multiply   ? (depth_mode_ ? pipeline_mul_depth_ : pipeline_mul_)
                             : mode_ == BlendMode::Add      ? (depth_mode_ ? pipeline_add_depth_ : pipeline_add_)
                             : mode_ == BlendMode::Subtract ? (depth_mode_ ? pipeline_sub_depth_ : pipeline_sub_)
                             : mode_ == BlendMode::Average  ? (depth_mode_ ? pipeline_avg_depth_ : pipeline_)
                             : mode_ == BlendMode::Premultiplied ? pipeline_pre_
                             : mode_ == BlendMode::AddAlpha ? pipeline_add_alpha_
                             : depth_mode_                  ? pipeline_depth_
                                                            : pipeline_;
  SDL_BindGPUGraphicsPipeline(ctx_->pass(), pipeline.handle());
  SDL_PushGPUVertexUniformData(ctx_->cmd(), 0, view_transform_, sizeof view_transform_);
  push_filter();
}

void SpriteBatch::push_filter() {
  if (!vertex_data_) return;
  // Second word: a premultiplied composite keeps texels with alpha 0 and colour > 0 (light added with an additive blend).
  const s32 filter_block[4] = {static_cast<s32>(filter_), mode_ == BlendMode::Premultiplied ? 1 : 0, 0, 0};
  SDL_PushGPUFragmentUniformData(ctx_->cmd(), 0, filter_block, sizeof filter_block);
}

void SpriteBatch::set_filter(SpriteFilter filter) {
  if (filter == filter_) return;
  flush();
  filter_ = filter;
  push_filter();
}

void SpriteBatch::set_log_tag(const char* tag) { log_tag_ = tag ? tag : ""; }

void SpriteBatch::set_blend(BlendMode mode) {
  if (mode == mode_) return;
  flush();
  mode_ = mode;
  bind_pipeline();
}

}  // namespace oc::render
