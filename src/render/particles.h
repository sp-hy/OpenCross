#pragma once
// Particle quad batcher for the effect player (game/effects): arbitrary (rotated / projected) textured quads with the GPU blend
// modes of the emitters and a depth test against the room layers / actors without depth writes. Textures are TexRefs. Coordinates are in the caller's virtual space
// (top-left origin, y down) like SpriteBatch::begin(); `depth` is the shared 0..1 depth-buffer value (OT slot / kDepthRange).
#include <array>

#include "render/buffer.h"
#include "render/pipeline.h"
#include "render/sprite_batch.h"

namespace oc::render {

// GPU blend of an emitter object: 0 opaque modulated, 1 = 0.5B + 0.5F, 2 = B + F, 3 = B - F, 4 = B + 0.25F, 5 = invert the backdrop
// (the NEGAPOSI screen negative).
constexpr int kParticleBlendModes = 6;

class ParticleBatch {
 public:
  // Per frame.
  static constexpr u32 kMaxQuads = 8192;

  ParticleBatch() = default;
  ~ParticleBatch() { destroy(); }
  ParticleBatch(const ParticleBatch&) = delete;
  ParticleBatch& operator=(const ParticleBatch&) = delete;

  bool init(GpuContext& ctx);
  void destroy();

  // `resume`: a later pass of the same frame (e.g. after a render target switch) appends to this frame's vertex data instead of
  // overwriting what the earlier passes recorded (the GPU reads the vertices only when the frame is submitted).
  void begin(const Frame& frame, float view_width, float view_height, bool resume = false);
  // Corners in texture order (u0v0, u1v0, u0v1, u1v1). `modulate` = emitter colour bytes (0x80 neutral).
  void quad(TexRef texture, int blend, const float x[4], const float y[4], const float u[4], const float v[4], u8 mod_r,
            u8 mod_g, u8 mod_b, float depth);
  // The same with one modulation colour per corner (Gouraud: the bespoke overlays' GT3 / GT4 packets).
  void quad(TexRef texture, int blend, const float x[4], const float y[4], const float u[4], const float v[4], const u8 rgb[4][3],
            float depth);
  void end();

  // [Enhancements] FullPrecisionEffects: modulate without the half 5-bit level the PS1 loses to rounding; set once per frame by the host.
  static void set_full_precision(bool on) { full_precision_ = on; }

 private:
  void flush();

  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  std::array<Pipeline, kParticleBlendModes> pipelines_;
  Buffer index_buffer_;
  StreamBuffer vertices_;

  SpriteVertex* vertex_data_ = nullptr;
  TexRef current_;
  int mode_ = -1;
  u32 quad_count_ = 0, flushed_quads_ = 0;
  // Frame::serial of the last begin() (a change = a new frame).
  u64 last_serial_ = ~0ull;
  float push_[4]{};
  bool overflow_logged_ = false;
  static inline bool full_precision_ = false;
};

}  // namespace oc::render
