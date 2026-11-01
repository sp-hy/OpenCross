#pragma once
// [Enhancements] Lighting: real shadows of the battle units on the floor. Every model is flattened onto the floor plane along each light, into
// a mask that looks straight down on the arena: the red channel for the stage's sun, green for the unit's spell shadow (render::spell_shadow). Max blending
// makes overlaps harmless.
//
// The field has no one floor: begin_tiles() gives every actor its own tile of the mask instead, a light value to multiply the backdrop by, around its feet at
// its own floor height. The caller lays the tile on the floor the way the PS1 shadow quad lies (game::ActorShadows). The floor pass then multiplies the finished floor by what the shadows leave of its light (shaders/shadow_floor.frag).
#include <array>
#include <span>

#include "render/buffer.h"
#include "render/lights.h"
#include "render/mat4.h"
#include "render/mesh_pipeline.h"
#include "render/pipeline.h"
#include "render/render_target.h"
#include "render/texture.h"

namespace oc::render {

class ShadowPipeline {
 public:
  // The sun, the spell lights.
  static constexpr int kChannels = 2;
  static constexpr u32 kMaskSize = 2048;
  // The mask covers x and z in +-kReach (the arena is +-5120).
  static constexpr float kReach = 6144.0f;
  // Model draws per frame (units, weapons).
  static constexpr u32 kMaxCasters = 48;
  // begin_tiles(): tiles per side, so kTileGrid * kTileGrid >= kMaxCasters.
  static constexpr int kTileGrid = 8;

  ShadowPipeline() = default;
  ~ShadowPipeline() { destroy(); }
  ShadowPipeline(const ShadowPipeline&) = delete;
  ShadowPipeline& operator=(const ShadowPipeline&) = delete;

  bool init(GpuContext& ctx);
  void destroy();
  bool ready() const { return mask_.valid(); }

  // Suspends the rendering in progress (its depth is cleared on the way back: call before anything in it needs depth).
  void begin_mask(const Frame& frame);
  // The same for tiles: multipliers, 1 = no shadow. Choose each with place_tile() before its cast().
  void begin_tiles(const Frame& frame);
  // Tile `index` covers x, z within +-half of (x, z); the floor there is at floor_y (y down, as the field world). Grow and fade are in field units.
  void place_tile(int index, float x, float z, float floor_y, float half);
  // The tile's centre in the uv of tiles(), and half its span.
  static std::array<float, 3> tile_uv(int index);
  // After end_mask(): blurs tiles 0 .. half.size() - 1, each `half[i]` units either way of its actor, into tiles().
  void soften_tiles(const Frame& frame, std::span<const float> half);
  TexRef tiles() const { return soft_.ref(linear_); }
  // One model's shadow from one light of the mask: the direction toward the light (y > 0) and how much shadow it throws, 0..1.
  struct Cast {
    std::array<float, 3> dir{0, 1, 0};
    float strength = 0;
  };
  // Bit k of `hidden_meshes`: mesh k casts none; of `untextured_meshes`: mesh k has no usable cut-out.
  void cast(const MeshModel& model, const Mat4& model_mat, std::span<const Mat4> palette, u32 hidden_meshes, u32 untextured_meshes, const std::array<Cast, kChannels>& casts);
  void end_mask();

  struct Floor {
    // PS1 file space -> clip, as StagePipeline::draw_dynamic builds it.
    Mat4 clip;
    // x0, z0, x1, z1 of the quad: where a shadow can fall.
    std::array<float, 4> rect{-kReach, -kReach, kReach, kReach};
    // Share of the sunlight the sun's shadow removes.
    float strength = 0.5f;
    // Depth in ordering-table slots, in front of the floor it lies on: past the whole-packet depth of a stage's coplanar floor layers.
    float bias = -64.0f;
    const LightSet* lights = nullptr;
  };
  // Inside the frame's rendering (the stage pass), after the scenery.
  void draw_floor(const Floor& floor);

 private:
  // Where a pass's shadows go; see shaders/shadow_cast.vert.
  struct Place {
    std::array<float, 4> plane{0, 0, 0, 1};
    std::array<float, 4> tile{0, 0, 1, 2400};
    float inverse_span = 1.0f / kReach;
    float grow = 26.0f;
    std::array<float, 3> tone{0, 1, 0.4f};
    // Every channel of the mask, not only the light's.
    bool all_channels = false;
  };
  void begin_pass(const Frame& frame, const Pipeline& pipeline, const float (&clear)[4]);

  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* cast_vs_ = nullptr;
  SDL_GPUShader* cast_fs_ = nullptr;
  SDL_GPUShader* floor_vs_ = nullptr;
  SDL_GPUShader* floor_fs_ = nullptr;
  SDL_GPUShader* blur_vs_ = nullptr;
  SDL_GPUShader* blur_fs_ = nullptr;
  Pipeline cast_pipe_, tile_pipe_, floor_pipe_, blur_pipe_;
  RenderTarget mask_, soft_;
  Sampler linear_;
  StreamBuffer slots_;
  void* slots_memory_ = nullptr;
  u32 slot_ = 0;
  u64 serial_ = 0;
  Place place_;
  Frame mask_frame_;
  bool in_mask_ = false;
};

}  // namespace oc::render
