#pragma once
// 2D textured-quad batcher. Coordinates are in a caller-chosen virtual space with the origin at the top-left
// (y grows downwards); begin() takes the size of that space. Consecutive quads with the same texture share a draw.
#include <string>
#include <unordered_set>

#include "render/buffer.h"
#include "render/pipeline.h"
#include "render/texture.h"

namespace oc::render {

struct SpriteVertex {
  float x, y, u, v;
  // Bytes R,G,B,A in memory.
  u32 color;
  // Depth-buffer value (0 near .. 1 far); only used by draw_depth() quads.
  float z;
};

constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) {
  return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}

// Alpha is the normal mode; Multiply (dst *= src colour) and Add (dst += src colour) are for full-screen tints; Subtract: dst -= src colour
// (PS1 ABR 2); Average: (dst + src) / 2 = alpha blend at 0.5, depth-tested, never written (PS1 ABR 0); Premultiplied: dst = src + dst *
// (1 - src alpha) (an off-screen UI target composited over the frame).
enum class BlendMode { Alpha, Multiply, Add, Subtract, Average, Premultiplied, AddAlpha };

// Linear = bilinear whatever the texture's sampler is (four point taps, for the per-category texture filter options).
enum class SpriteFilter { Sampler = 0, Point = 1, Box = 2, Linear = 3 };

// Index buffer of `quad_count` quads of four vertices each, two triangles (0, 1, 2) (2, 1, 3) per quad, 16-bit indices.
bool create_quad_index_buffer(GpuContext& ctx, Buffer& buffer, u32 quad_count);

class SpriteBatch {
 public:
  // Per frame.
  static constexpr u32 kMaxQuads = 4096;

  SpriteBatch() = default;
  ~SpriteBatch() { destroy(); }
  SpriteBatch(const SpriteBatch&) = delete;
  SpriteBatch& operator=(const SpriteBatch&) = delete;

  bool init(GpuContext& ctx);
  void destroy();

  // `resume` continues the frame's quad buffer after an earlier begin()/end() pair (e.g. a UI pass after the 3D pass).
  void begin(const Frame& frame, float view_width, float view_height, bool resume = false);
  void draw(TexRef texture, float x, float y, float w, float h, float u0 = 0.0f, float v0 = 0.0f,
            float u1 = 1.0f, float v1 = 1.0f, u32 color = 0xFFFFFFFFu);
  // Same as draw(), but the quad is depth tested and written at `depth` (0..1, larger = farther) and texels with
  // alpha 0 are discarded, so later 3D draws (MeshPipeline) sort against it.
  void draw_depth(TexRef texture, float x, float y, float w, float h, float depth, float u0 = 0.0f,
                  float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f, u32 color = 0xFFFFFFFFu);
  // A depth-tested quad with four free corners (PS1 POLY_FT4 order: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right; drawn
  // as the triangles 0-1-2 and 2-1-3 with affine UVs, like the GPU). xy / uv: 4 pairs. Depth writes follow the blend as in draw_depth().
  void draw_quad_depth(TexRef texture, const float (&xy)[8], const float (&uv)[8], float depth, u32 color = 0xFFFFFFFFu);
  // The same free quad without the depth test (UI art drawn turned or mirrored).
  void draw_quad(TexRef texture, const float (&xy)[8], const float (&uv)[8], u32 color = 0xFFFFFFFFu);
  // Switches the blend for the following quads (reset to Alpha by begin()).
  void set_blend(BlendMode mode);
  BlendMode blend() const { return mode_; }
  // Filtering of the following quads whatever the texture's sampler is (persists across begin() / end() until switched back):
  // Point = the centre of the texel under the pixel (classic art drawn into a PS1-resolution target: every texture nearest);
  // Box = the average of 4 x 4 point taps over the pixel's footprint (an area filter; the pixel-font glyphs in such a target).
  void set_filter(SpriteFilter filter);
  SpriteFilter filter() const { return filter_; }
  // Debug (env OC_LOWRES_LOG=1): while a tag is set, the first draw of every texture logs "<tag>: texture W x H" once per tag.
  void set_log_tag(const char* tag);
  void end();

 private:
  void flush();
  void bind_pipeline();
  void quad(bool depth, TexRef texture, float x, float y, float w, float h, float z, float u0, float v0, float u1,
            float v1, u32 color);

  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  // One pipeline per blend; pipeline_depth_ writes depth, the other *_depth_ ones only test it.
  Pipeline pipeline_;
  Pipeline pipeline_depth_;
  Pipeline pipeline_mul_, pipeline_add_, pipeline_sub_, pipeline_mul_depth_, pipeline_add_depth_, pipeline_sub_depth_, pipeline_avg_depth_;
  Pipeline pipeline_pre_, pipeline_add_alpha_;
  BlendMode mode_ = BlendMode::Alpha;
  SpriteFilter filter_ = SpriteFilter::Sampler;
  void push_filter();
  std::string log_tag_;
  std::unordered_set<std::string> logged_;
  bool log_on_ = false;
  Buffer index_buffer_;
  StreamBuffer vertices_;

  SpriteVertex* vertex_data_ = nullptr;
  // Scale and offset of the virtual space, pushed to the vertex stage with every pipeline bind.
  float view_transform_[4]{};
  TexRef current_;
  u32 quad_count_ = 0;
  u32 flushed_quads_ = 0;
  // Frame::serial of the last begin() (a change = a new frame).
  u64 last_serial_ = ~0ull;
  bool depth_mode_ = false;
  bool overflow_logged_ = false;
};

}  // namespace oc::render
