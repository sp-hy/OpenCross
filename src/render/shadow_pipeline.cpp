#include "render/shadow_pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "core/log.h"
#include "render/shaders.h"

namespace oc::render {

namespace {

#include "shadow_cast.vert.inc"
#include "shadow_cast.frag.inc"
#include "shadow_floor.vert.inc"
#include "shadow_floor.frag.inc"
#include "shadow_blur.vert.inc"
#include "shadow_blur.frag.inc"

struct CasterSlot {
  Mat4 model;
  Mat4 palette[MeshPipeline::kMaxJoints];
};

struct CastPush {
  float light[4];
  float channel[4];
  float misc[4];
  float plane[4];
  float tile[4];
  float tone[4];
};

struct FloorPush {
  Mat4 clip;
  float rect[4];
  float params[4];
  float lpos[3][4];
  float lcol[3][4];
};

}  // namespace

bool ShadowPipeline::init(GpuContext& ctx) {
  destroy();
  ctx_ = &ctx;
  cast_vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, shadow_cast_vert, ShaderDesc{0, 1, 1}, "shadow_cast.vert");
  cast_fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, shadow_cast_frag, ShaderDesc{1, 0, 1}, "shadow_cast.frag");
  floor_vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, shadow_floor_vert, ShaderDesc{0, 0, 1}, "shadow_floor.vert");
  floor_fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, shadow_floor_frag, ShaderDesc{1, 0, 1}, "shadow_floor.frag");
  blur_vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, shadow_blur_vert, ShaderDesc{0, 0, 0}, "shadow_blur.vert");
  blur_fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, shadow_blur_frag, ShaderDesc{1, 0, 1}, "shadow_blur.frag");
  if (!cast_vs_ || !cast_fs_ || !floor_vs_ || !floor_fs_ || !blur_vs_ || !blur_fs_) return false;
  if (!linear_.create(ctx, true)) return false;
  const VertexAttribute attributes[6] = {
      {0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<u32>(offsetof(SkinnedVertex, pos))},
      {1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<u32>(offsetof(SkinnedVertex, pos) + 3 * sizeof(float))},
      {2, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, static_cast<u32>(offsetof(SkinnedVertex, joint))},
      {3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, weight))},
      {4, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, uv))},
      {5, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, normal))},
  };
  PipelineDesc cast;
  cast.vertex = cast_vs_;
  cast.fragment = cast_fs_;
  cast.vertex_stride = sizeof(SkinnedVertex);
  cast.attributes = attributes;
  cast.blend = Blend::Max;
  if (!cast_pipe_.create(ctx, cast)) return false;
  cast.blend = Blend::Min;
  if (!tile_pipe_.create(ctx, cast)) return false;
  PipelineDesc floor;
  floor.vertex = floor_vs_;
  floor.fragment = floor_fs_;
  floor.blend = Blend::Multiply;
  floor.depth_test = true;
  if (!floor_pipe_.create(ctx, floor)) return false;
  PipelineDesc blur;
  blur.vertex = blur_vs_;
  blur.fragment = blur_fs_;
  if (!blur_pipe_.create(ctx, blur)) return false;
  if (!slots_.create(ctx, sizeof(CasterSlot) * kMaxCasters, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)) return false;
  return mask_.create(ctx, kMaskSize, kMaskSize) && soft_.create(ctx, kMaskSize / 2, kMaskSize / 2);
}

void ShadowPipeline::destroy() {
  mask_.destroy();
  soft_.destroy();
  slots_.destroy();
  cast_pipe_.destroy();
  tile_pipe_.destroy();
  floor_pipe_.destroy();
  blur_pipe_.destroy();
  linear_.destroy();
  if (ctx_)
    for (SDL_GPUShader* shader : {cast_vs_, cast_fs_, floor_vs_, floor_fs_, blur_vs_, blur_fs_})
      if (shader) SDL_ReleaseGPUShader(ctx_->device(), shader);
  cast_vs_ = cast_fs_ = floor_vs_ = floor_fs_ = blur_vs_ = blur_fs_ = nullptr;
  slots_memory_ = nullptr;
  in_mask_ = false;
  ctx_ = nullptr;
}

void ShadowPipeline::begin_mask(const Frame& frame) {
  static constexpr float kClear[4] = {0, 0, 0, 0};
  place_ = Place{};
  begin_pass(frame, cast_pipe_, kClear);
}

void ShadowPipeline::begin_tiles(const Frame& frame) {
  static constexpr float kClear[4] = {1, 1, 1, 1};
  begin_pass(frame, tile_pipe_, kClear);
}

std::array<float, 3> ShadowPipeline::tile_uv(int index) {
  const float span = 1.0f / static_cast<float>(kTileGrid);
  return {(static_cast<float>(index % kTileGrid) + 0.5f) * span, (static_cast<float>(index / kTileGrid) + 0.5f) * span, 0.5f * span};
}

void ShadowPipeline::place_tile(int index, float x, float z, float floor_y, float half) {
  const std::array<float, 3> uv = tile_uv(index);
  const u32 tile_pixels = kMaskSize / kTileGrid;
  place_ = Place{};
  place_.plane = {x, floor_y, z, -1.0f};
  place_.tile = {uv[0] * 2.0f - 1.0f, 1.0f - uv[1] * 2.0f, uv[2] * 2.0f, 420.0f};
  place_.inverse_span = 1.0f / half;
  place_.grow = 4.0f;
  place_.tone = {1.0f, -1.0f, 0.45f};
  place_.all_channels = true;
  mask_frame_.set_scissor(Rect{static_cast<int32_t>(static_cast<u32>(index % kTileGrid) * tile_pixels), static_cast<int32_t>(static_cast<u32>(index / kTileGrid) * tile_pixels), tile_pixels, tile_pixels});
}

void ShadowPipeline::soften_tiles(const Frame& frame, std::span<const float> half) {
  // The blur's radius in field units, whatever the tile's span.
  static constexpr float kSoftness = 4.0f;
  static constexpr float kClear[4] = {1, 1, 1, 1};
  Frame soft_frame;
  ctx_->begin_target(soft_, frame, soft_frame, kClear);
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_BindGPUGraphicsPipeline(render_pass, blur_pipe_.handle());
  const TexRef sharp = mask_.ref(linear_);
  const SDL_GPUTextureSamplerBinding texture_binding{sharp.texture, sharp.sampler};
  SDL_BindGPUFragmentSamplers(render_pass, 0, &texture_binding, 1);
  const u32 tile_pixels = soft_.width() / kTileGrid;
  for (size_t i = 0; i < half.size(); ++i) {
    const int index = static_cast<int>(i);
    soft_frame.set_scissor(Rect{static_cast<int32_t>(static_cast<u32>(index % kTileGrid) * tile_pixels), static_cast<int32_t>(static_cast<u32>(index / kTileGrid) * tile_pixels), tile_pixels, tile_pixels});
    const float push[4] = {kSoftness / half[i] * tile_uv(index)[2], 0, 0, 0};
    SDL_PushGPUFragmentUniformData(ctx_->cmd(), 0, push, sizeof push);
    SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);
  }
  ctx_->end_target(soft_);
}

void ShadowPipeline::begin_pass(const Frame& frame, const Pipeline& pipeline, const float (&clear)[4]) {
  if (frame.serial != serial_ || frame.serial == 0) slot_ = 0;
  serial_ = frame.serial;
  ctx_->begin_target(mask_, frame, mask_frame_, clear);
  slots_memory_ = slots_.begin(mask_frame_);
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_BindGPUGraphicsPipeline(render_pass, pipeline.handle());
  SDL_GPUBuffer* slots_buffer = slots_.handle();
  SDL_BindGPUVertexStorageBuffers(render_pass, 0, &slots_buffer, 1);
  in_mask_ = true;
}

void ShadowPipeline::cast(const MeshModel& model, const Mat4& model_mat, std::span<const Mat4> palette, u32 hidden_meshes, u32 untextured_meshes, const std::array<Cast, kChannels>& casts) {
  if (!in_mask_) return;
  if (slot_ >= kMaxCasters) {
    log::warn("ShadowPipeline: more than {} casters in a frame", kMaxCasters);
    return;
  }
  // Untextured faces still need some texture bound.
  TexRef fallback;
  for (const TexRef& ref : model.sets_nearest)
    if (ref) {
      fallback = ref;
      break;
    }
  if (!fallback) return;
  const u32 slot = slot_++;
  auto* caster = reinterpret_cast<CasterSlot*>(static_cast<u8*>(slots_memory_) + slot * sizeof(CasterSlot));
  caster->model = model_mat;
  const size_t joint_count = std::min<size_t>(palette.size(), MeshPipeline::kMaxJoints);
  if (joint_count) std::memcpy(caster->palette, palette.data(), joint_count * sizeof(Mat4));
  for (size_t i = joint_count; i < MeshPipeline::kMaxJoints; ++i) caster->palette[i] = Mat4{};
  slots_.mark_used((slot + 1) * sizeof(CasterSlot));

  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  const SDL_GPUBufferBinding vertex_binding{model.vertices.handle(), 0};
  SDL_BindGPUVertexBuffers(render_pass, 0, &vertex_binding, 1);
  const SDL_GPUBufferBinding index_binding{model.indices.handle(), 0};
  SDL_BindGPUIndexBuffer(render_pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  for (const MeshModel::Draw& draw : model.draws) {
    if (draw.trans == 0xFF) continue;
    if (draw.mesh < 32 && ((hidden_meshes >> draw.mesh) & 1u)) continue;
    const bool has_texture = draw.texture >= 0 && static_cast<size_t>(draw.texture) < model.sets_nearest.size() && model.sets_nearest[static_cast<size_t>(draw.texture)];
    const bool cut_out = has_texture && !(draw.mesh < 32 && ((untextured_meshes >> draw.mesh) & 1u));
    const TexRef tex = cut_out ? model.sets_nearest[static_cast<size_t>(draw.texture)] : fallback;
    const SDL_GPUTextureSamplerBinding texture_binding{tex.texture, ctx_->filtered(tex.sampler)};
    SDL_BindGPUFragmentSamplers(render_pass, 0, &texture_binding, 1);
    for (int channel = 0; channel < kChannels; ++channel) {
      const Cast& light = casts[static_cast<size_t>(channel)];
      if (light.strength <= 0.0f) continue;
      CastPush push{};
      std::memcpy(push.light, light.dir.data(), sizeof light.dir);
      push.light[3] = place_.grow;
      for (int k = 0; k < 4; ++k) push.channel[k] = k == channel || place_.all_channels ? 1.0f : 0.0f;
      push.misc[0] = static_cast<float>(slot);
      push.misc[1] = cut_out ? 0.0f : 1.0f;
      push.misc[2] = light.strength;
      push.misc[3] = place_.inverse_span;
      push.plane[0] = place_.plane[0], push.plane[1] = place_.plane[2], push.plane[2] = place_.plane[1], push.plane[3] = place_.plane[3];
      std::memcpy(push.tile, place_.tile.data(), sizeof push.tile);
      std::memcpy(push.tone, place_.tone.data(), sizeof place_.tone);
      SDL_PushGPUVertexUniformData(command_buffer, 0, &push, sizeof push);
      SDL_PushGPUFragmentUniformData(command_buffer, 0, &push, sizeof push);
      SDL_DrawGPUIndexedPrimitives(render_pass, draw.index_count, 1, draw.first_index, 0, 0);
    }
  }
}

void ShadowPipeline::end_mask() {
  if (!in_mask_) return;
  in_mask_ = false;
  ctx_->end_target(mask_);
}

void ShadowPipeline::draw_floor(const Floor& floor) {
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_BindGPUGraphicsPipeline(render_pass, floor_pipe_.handle());
  const TexRef mask = mask_.ref(linear_);
  const SDL_GPUTextureSamplerBinding texture_binding{mask.texture, mask.sampler};
  SDL_BindGPUFragmentSamplers(render_pass, 0, &texture_binding, 1);
  FloorPush push{};
  push.clip = floor.clip;
  std::memcpy(push.rect, floor.rect.data(), sizeof push.rect);
  push.params[0] = floor.strength;
  push.params[1] = floor.bias;
  push.params[2] = 0.5f / kReach;
  push.params[3] = 1.0f / static_cast<float>(kMaskSize);
  // The lights StagePipeline lights the floor with.
  const int light_count = floor.lights ? std::min(floor.lights->count, 3) : 0;
  for (int i = 0; i < light_count; ++i) {
    const PointLight& light = floor.lights->light[static_cast<size_t>(i)];
    for (size_t k = 0; k < 3; ++k) {
      push.lpos[i][k] = light.pos[k];
      push.lcol[i][k] = light.rgb[k];
    }
    push.lpos[i][3] = light.linear ? -light.radius : light.radius;
  }
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  SDL_PushGPUVertexUniformData(command_buffer, 0, &push, sizeof push);
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &push, sizeof push);
  SDL_DrawGPUPrimitives(render_pass, 6, 1, 0, 0);
}

}  // namespace oc::render
