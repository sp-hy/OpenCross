#include "render/stage_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cmath>
#include <cstddef>

#include "core/log.h"
#include "core/parallel.h"
#include "render/mesh_pipeline.h"  // bleed_transparent
#include "render/ps1_jitter.h"
#include "render/stage_tiles.h"
#include "render/texture_registry.h"
#include "render/shaders.h"

namespace oc::render {

namespace {

// Uniforms pushed to both stages:
//   clip    camera space -> clip, already multiplied with the instance transform
//   fogc    depth-cue far colour (colour bytes, 0x80 = neutral) in rgb, w = fog slope A (p = A + B / z, 0 = no fog)
//   color   object colour / 128 (rgb), w = blend factor (0.25 for B+F/4, else 1)
//   params  x = OT bias, y = depth mode (0 packet slot, 1 fixed slot, 2 per-fragment z) + 4 * uv-scroll flag (+ 8 HD depth cue, + 16 EdgeBlend
//           strip, + 32 the texture is a picture copy: 15-bit black texels are transparent), z = output alpha (0.5 for
//           the average blend), w = fog offset B
// The camera z of a point is the w of its clip position (the projection's last row is (0, 0, 1, 0)).
//   uvw     x/y = uv phase (texels), z = off_u + 256 * off_v + 65536 * (1 bilinear wrap + 2 EdgeBlend tiling), w = win_w + 256 * win_h
#include "stage.vert.inc"
#include "stage_affine.vert.inc"
#include "stage.frag.inc"
#include "stage_affine.frag.inc"

struct PushData {
  Mat4 clip;
  float fogc[4];
  float color[4];
  float params[4];
  float uvw[4];
  // [Video] Ps1Jitter (render/ps1_jitter.h): x / y PS1 pixels per NDC unit, z = on. Pushes 144 bytes (every desktop GPU allows 256).
  float jit[4] = {ps1_jitter().px, ps1_jitter().py, ps1_jitter().on ? 1.0f : 0.0f, 0.0f};
  // [Enhancements] SpellLights: jit[3] = 1 on the opaque scenery draws; pinv = projection P00, P02, P11, P12; light i = camera-space position + radius,
  // colour x intensity. 256 bytes in all (the limit every desktop GPU offers).
  float pinv[4] = {0, 0, 0, 0};
  float lpos[StagePipeline::kLights][4] = {};
  float lcol[StagePipeline::kLights][4] = {};
};
static_assert(sizeof(PushData) == 256, "uniforms: 128 + the jitter vec4 + the SpellLights block");
static_assert(sizeof(assets::StageVertex) == 15 * sizeof(float), "StageVertex must be tightly packed");

// View depth (StagePipeline::set_view_depth): jit[3] = 2 makes the fragment shader write the camera z instead of the picture.
void push_uniforms(SDL_GPUCommandBuffer* command_buffer, PushData data, bool view_depth) {
  if (view_depth) data.jit[3] = 2.0f;
  SDL_PushGPUVertexUniformData(command_buffer, 0, &data, sizeof data);
  SDL_PushGPUFragmentUniformData(command_buffer, 0, &data, sizeof data);
}

void bind_vertex_buffer(SDL_GPURenderPass* render_pass, SDL_GPUBuffer* buffer) {
  const SDL_GPUBufferBinding binding{buffer, 0};
  SDL_BindGPUVertexBuffers(render_pass, 0, &binding, 1);
}

void bind_index_buffer(SDL_GPURenderPass* render_pass, SDL_GPUBuffer* buffer) {
  const SDL_GPUBufferBinding binding{buffer, 0};
  SDL_BindGPUIndexBuffer(render_pass, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

void bind_texture(GpuContext& ctx, SDL_GPURenderPass* render_pass, const TexRef& texture) {
  const SDL_GPUTextureSamplerBinding binding{texture.texture, ctx.filtered(texture.sampler)};
  SDL_BindGPUFragmentSamplers(render_pass, 0, &binding, 1);
}

// Everything of one instance's per-draw uniforms except the clip matrix and the blend factor / alpha.
void fill_push(PushData& pd, const StageInstance& in, const StagePipeline::DepthCue& cue, float factor, float alpha, bool smooth_fog = false, bool linear = false) {
  for (size_t channel = 0; channel < 3; ++channel) pd.color[channel] = static_cast<float>(in.rgb[channel]) / 128.0f;
  pd.color[3] = factor;
  pd.fogc[0] = cue.r;
  pd.fogc[1] = cue.g;
  pd.fogc[2] = cue.b;
  pd.fogc[3] = 0.0f;
  pd.params[3] = 0.0f;
  // Depth cue: p = far * (z - near) / ((far - near) * z) = A + B / z.
  if (in.depth_cue && cue.active && cue.far > cue.near + 1.0f) {
    pd.fogc[3] = cue.far / (cue.far - cue.near);
    pd.params[3] = -cue.far * cue.near / (cue.far - cue.near);
  }
  pd.params[0] = static_cast<float>(in.bias);
  pd.params[1] = (in.fixed_slot ? 1.0f : 0.0f) + (in.uv_scroll ? 4.0f : 0.0f) + (smooth_fog ? 8.0f : 0.0f);
  pd.params[2] = alpha;
  pd.uvw[0] = in.uv_phase[0];
  pd.uvw[1] = in.uv_phase[1];
  // Scroll flags: 1 = the bilinear footprint wraps inside the window, 2 = [Enhancements] EdgeBlend: a packet corner on the window's last texel
  // reaches the window end, so the next packet's first texel continues the tiling.
  const int scroll_flags = (linear ? 1 : 0) + (edge_blend() ? 2 : 0);
  pd.uvw[2] = static_cast<float>((in.uv_window[0] & 255) + 256 * (in.uv_window[1] & 255) + 65536 * scroll_flags);
  pd.uvw[3] = static_cast<float>(std::clamp(in.uv_window[2], 0, 255) + 256 * std::clamp(in.uv_window[3], 0, 255));
}

// Fills the SpellLights block of one draw's per-draw uniforms; `blend` = the pipeline blend index (only the opaque draws are lit).
void fill_lights(PushData& pd, const Mat4& proj, int blend, const StagePipeline::SceneLights& lights) {
  // Only opaque and average-blend (ABR 0) draws are lit; additive / subtractive layers are not.
  if ((lights.count == 0 && !lights.amb_on) || blend > 1) return;
  pd.jit[3] = 1.0f;
  pd.pinv[0] = proj.at(0, 0);
  pd.pinv[1] = proj.at(0, 2);
  pd.pinv[2] = proj.at(1, 1);
  pd.pinv[3] = proj.at(1, 2);
  for (int i = 0; i < lights.count; ++i)
    for (size_t channel = 0; channel < 4; ++channel) pd.lpos[i][channel] = lights.pos[i][channel], pd.lcol[i][channel] = lights.col[i][channel];
  // lcol[i].w = the scene ambient channel i (the shader multiplies the stage colour by it).
  for (size_t i = 0; i < 3; ++i) pd.lcol[i][3] = lights.amb[i];
}

// A packet is drawn when NCLIP(v0, v1, v2) > 0, i.e. the screen-space (y down) cross product is positive: a PS1 front face is a
// *clockwise* triangle in the pipeline's winding, so keeping it means culling the "front" bit.
constexpr SDL_GPUCullMode kCull[3] = {SDL_GPU_CULLMODE_FRONT, SDL_GPU_CULLMODE_BACK, SDL_GPU_CULLMODE_NONE};
constexpr Blend kBlend[4] = {Blend::None, Blend::Alpha, Blend::Additive, Blend::Subtract};

// Blend state of one draw: object blend (op 06 bits 0-2) wins; an opaque object keeps the packets' own semi bit + tpage abr.
// Returns the pipeline blend index 0..3 and the colour factor / output alpha.
int draw_blend(const StageInstance& in, const StageMesh::Sub& sub, float& factor, float& alpha) {
  factor = 1.0f;
  alpha = 1.0f;
  // Blend 5-7 are add + subtract passes; the add pass carries the look.
  int mode = in.blend >= 5 ? 2 : in.blend;
  // Extended objects (op 35) take the same abr as the plain path: they decrement the blend (`blend > 0 -> p = blend - 1`, semi flag
  // on) and finish the packet tpage with `(tpage & 0xFF9F) | (p * 0x20)`, so blend 2 = B + F and blend 4 = B + F/4. Nothing else in the
  // ext path scales the colour (packet corner x object colour >> 7). OC_STAGE_EXT_QUARTER=1 selects an alternative look in which a
  // scrolling blend-2 ext object adds a quarter.
  static const bool quarter = std::getenv("OC_STAGE_EXT_QUARTER") != nullptr;
  if (quarter && in.ext && !in.ext_morph && in.blend == 2) mode = 4;
  if (mode == 0 && sub.semi) mode = sub.abr + 1;
  switch (mode) {
    case 1: alpha = 0.5f; return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: factor = 0.25f; return 2;
    default: return 0;
  }
}

// The PS1 packet builders transform the corners with RTPT / RTPS and
// draw the packet only when the GTE error summary (FLAG bit 31) stays clear; the GPU additionally skips a triangle whose vertices are more
// than 1023 apart horizontally or 511 vertically. Camera space = M * v + t as in StageInstance::cam; returns false when the whole packet
// is dropped, `tri_ok[0/1]` = the triangles (0,1,2) / (2,1,3) survive the GPU size rule.
bool corners_pass(const std::array<float, 12>& cam, const std::array<float, 3>* const* pos, int corners, float proj_h, float ox, float oy, bool tri_ok[2],
                  const CullFrame& cf = CullFrame{}) {
  float sx[4], sy[4];
  for (int k = 0; k < corners; ++k) {
    const std::array<float, 3>& point = *pos[k];
    float cam_pos[3];
    for (int row = 0; row < 3; ++row) cam_pos[row] = cam[static_cast<size_t>(row * 3)] * point[0] + cam[static_cast<size_t>(row * 3 + 1)] * point[1] + cam[static_cast<size_t>(row * 3 + 2)] * point[2] + cam[static_cast<size_t>(9 + row)];
    // GTE error flags: IR1 / IR2 saturate (24 / 23), SZ3 saturates (18), division overflow (17: H >= 2 * SZ3).
    if (std::fabs(cam_pos[0]) > 32767.0f || std::fabs(cam_pos[1]) > 32767.0f) return false;
    if (cam_pos[2] < 0.0f || cam_pos[2] > 65535.0f) return false;
    if (proj_h >= 2.0f * cam_pos[2]) return false;
    sx[k] = ox + cam_pos[0] * proj_h / cam_pos[2];
    sy[k] = oy + cam_pos[1] * proj_h / cam_pos[2];
    // SX2 / SY2 saturate (GTE flags 14 / 13).
    if (!cull_screen_ok(cf, sx[k], sy[k])) return false;
  }
  auto size_ok = [&](int first, int second, int third) {
    const float x0 = std::min({sx[first], sx[second], sx[third]}), x1 = std::max({sx[first], sx[second], sx[third]});
    const float y0 = std::min({sy[first], sy[second], sy[third]}), y1 = std::max({sy[first], sy[second], sy[third]});
    return cull_span_ok(cf, x1 - x0, y1 - y0);
  };
  tri_ok[0] = size_ok(0, 1, 2);
  tri_ok[1] = corners == 4 && size_ok(2, 1, 3);
  return true;
}

// An op-37 morph with a usable target part (same vertex count).
bool rebuilt_morph(const StageInstance& in, const assets::StageAsset* asset) {
  if (in.morph_part < 0 || !asset || static_cast<size_t>(in.morph_part) >= asset->parts.size() || static_cast<size_t>(in.part) >= asset->parts.size()) return false;
  return asset->parts[static_cast<size_t>(in.part)].vertices.size() == asset->parts[static_cast<size_t>(in.morph_part)].vertices.size();
}

// A screen copy that one of the part's sub-meshes samples.
bool rebuilt_screen(const StageInstance& in, const assets::StageAsset* asset) {
  if (!in.screen || !asset || static_cast<size_t>(in.part) >= asset->parts.size()) return false;
  const auto& subs = asset->parts[static_cast<size_t>(in.part)].submeshes;
  int dx = 0, dy = 0;
  return std::any_of(subs.begin(), subs.end(), [&](const assets::StageSubMesh& sub) { return in.screen_offset(sub.direct_page, dx, dy); });
}

bool packet_passes(const StageInstance& in, const StageMesh& mesh, const StageMesh::Packet& pk, float proj_h, float ox, float oy, bool tri_ok[2], const CullFrame& cf) {
  const std::array<float, 3>* pos[4] = {};
  for (int k = 0; k < pk.corners; ++k) pos[k] = &mesh.cpu_pos[pk.corner[k]];
  return corners_pass(in.cam, pos, pk.corners, proj_h, ox, oy, tri_ok, cf);
}

// Camera z of a PS1-space point under an instance transform.
float cam_z(const std::array<float, 12>& cam, const std::array<float, 3>& point) { return cam[6] * point[0] + cam[7] * point[1] + cam[8] * point[2] + cam[11]; }

// The ground object's cell of a ring quad: the corner sums of (x + 0x1400) and (z - 0x1400) as s16, >> 2, rounded
// toward zero, >> 10 -> cx, cz (cz <= 0); index cx - 10 * cz. -1 outside the 10 x 10 grid.
int ground_cell(const std::array<float, 3>* const* pos) {
  int sx = 0, sz = 0;
  for (int k = 0; k < 4; ++k) {
    sx += static_cast<s16>(static_cast<int>(std::lround((*pos[k])[0])) + 0x1400);
    sz += static_cast<s16>(static_cast<int>(std::lround((*pos[k])[2])) - 0x1400);
  }
  int ax = sx >> 2, az = sz >> 2;
  if (ax < 0) ax += 0x3FF;
  if (az < 0) az += 0x3FF;
  const int cx = ax >> 10, cz = az >> 10;
  const int idx = cx - 10 * cz;
  return cx >= 0 && cx < 10 && cz <= 0 && cz > -10 ? idx : -1;
}

}  // namespace

bool StagePipeline::init(GpuContext& ctx) {
  destroy();
  ctx_ = &ctx;

  vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, stage_vert, ShaderDesc{0, 0, 1}, "stage.vert");
  fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, stage_frag, ShaderDesc{1, 0, 1}, "stage.frag");
  if (!vs_ || !fs_) return false;
  // The PS1 GPU interpolates texture coordinates and Gouraud colours linearly in screen space (no perspective correction). The battle ground
  // cells are the only stage polygons big and near enough for the difference to show (the PS1 even splits the near ones in four, draw_cpu);
  // they are drawn with `noperspective` varyings. The scenery keeps the perspective-correct path (its packets are small).
  vs_affine_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, stage_affine_vert, ShaderDesc{0, 0, 1}, "stage_affine.vert");
  fs_affine_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, stage_affine_frag, ShaderDesc{1, 0, 1}, "stage_affine.frag");
  if (!vs_affine_ || !fs_affine_) return false;
  if (!nearest_.create(ctx, false) || !linear_.create(ctx, true)) return false;
  if (!dyn_vertices_.create(ctx, sizeof(assets::StageVertex) * kDynVertices, SDL_GPU_BUFFERUSAGE_VERTEX) ||
      !dyn_indices_.create(ctx, sizeof(u32) * kDynIndices, SDL_GPU_BUFFERUSAGE_INDEX))
    return false;

  const VertexAttribute attributes[] = {
      {0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<u32>(offsetof(assets::StageVertex, pos))},
      {1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, static_cast<u32>(offsetof(assets::StageVertex, uv))},
      {2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<u32>(offsetof(assets::StageVertex, color))},
      {3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, static_cast<u32>(offsetof(assets::StageVertex, uv_raw))},
      {4, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, static_cast<u32>(offsetof(assets::StageVertex, uv_scale))},
      {5, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<u32>(offsetof(assets::StageVertex, centre))},
  };
  PipelineDesc pd;
  pd.vertex = vs_;
  pd.fragment = fs_;
  pd.vertex_stride = sizeof(assets::StageVertex);
  pd.attributes = attributes;
  pd.depth_test = true;
  for (int cull_mode = 0; cull_mode < 3; ++cull_mode)
    for (int blend_mode = 0; blend_mode < 4; ++blend_mode) {
      pd.cull = kCull[cull_mode];
      pd.blend = kBlend[blend_mode];
      pd.depth_write = blend_mode == 0;
      if (!pipes_[cull_mode][blend_mode].create(ctx, pd)) return false;
      pd.vertex = vs_affine_;
      pd.fragment = fs_affine_;
      if (!pipes_affine_[cull_mode][blend_mode].create(ctx, pd)) return false;
      pd.vertex = vs_;
      pd.fragment = fs_;
    }
  pd.blend = Blend::None;
  pd.depth_write = true;
  for (int cull_mode = 0; cull_mode < 3; ++cull_mode) {
    pd.cull = kCull[cull_mode];
    if (!pipes_view_depth_[cull_mode].create(ctx, pd)) return false;
  }
  const u8 px[4] = {255, 255, 255, 255};
  dyn_white_ = std::make_unique<Texture>();
  if (!dyn_white_->create(ctx, 1, 1, TextureFormat::Rgba8, px)) return false;
  dyn_white_set_ = TexRef(*dyn_white_, nearest_);
  return true;
}

void StagePipeline::destroy() {
  for (auto& row : pipes_)
    for (Pipeline& pipeline : row) pipeline.destroy();
  for (auto& row : pipes_affine_)
    for (Pipeline& pipeline : row) pipeline.destroy();
  for (Pipeline& pipeline : pipes_view_depth_) pipeline.destroy();
  dyn_white_set_ = TexRef();
  dyn_white_.reset();
  dyn_vertices_.destroy();
  dyn_indices_.destroy();
  dyn_vertex_memory_ = nullptr;
  dyn_index_memory_ = nullptr;
  nearest_.destroy();
  linear_.destroy();
  if (ctx_) {
    for (SDL_GPUShader* shader : {vs_, fs_, vs_affine_, fs_affine_})
      if (shader) SDL_ReleaseGPUShader(ctx_->device(), shader);
  }
  vs_ = fs_ = nullptr;
  vs_affine_ = fs_affine_ = nullptr;
  active_ = false;
  ctx_ = nullptr;
}

TexRef StagePipeline::make_stage_set(const Texture& tex, bool linear) {
  return TexRef(tex, linear ? linear_ : nearest_);
}

void StagePipeline::write_vertices(const void* data, u32 count, u32 first) {
  std::memcpy(dyn_vertex_memory_ + size_t(first) * sizeof(assets::StageVertex), data, size_t(count) * sizeof(assets::StageVertex));
  dyn_vertices_.mark_used((first + count) * sizeof(assets::StageVertex));
}

void StagePipeline::write_indices(const u32* data, u32 count, u32 first) {
  std::memcpy(dyn_index_memory_ + size_t(first) * sizeof(u32), data, size_t(count) * sizeof(u32));
  dyn_indices_.mark_used((first + count) * sizeof(u32));
}

std::unique_ptr<StageMesh> StagePipeline::create(const assets::StageAsset& stage) {
  StageCreateJob job = create_begin();
  job.group.clear();
  while (!create_textures(job, stage, 1e9)) {
  }
  return create_finish(job, stage);
}

// The same creation in slices (the battle load): create_begin(), create_textures() until true (a few textures per call, each blocks on its
// own staging copy), create_finish() (white texel, packets, the two static buffers).
StageCreateJob StagePipeline::create_begin() {
  StageCreateJob job;
  job.mesh = std::make_unique<StageMesh>();
  job.group = scene_group_;
  return job;
}

bool StagePipeline::create_textures(StageCreateJob& job, const assets::StageAsset& stage, double budget_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  StageMesh* mesh = job.mesh.get();
  auto make_set = [&](const Texture& tex, bool linear) { return make_stage_set(tex, linear); };
  // One queue wait for the textures of this call.
  UploadBatch batch;
  // The atlases first (effect polygons, the ground cells' dynamic draws and the effect meshes sample them), then the baked per-object textures
  // (assets::StageObjectTexture), which are what the dump / replace names: StageMesh::sets = [atlases..., objects..., white].
  const size_t atlases = stage.textures.size();
  for (; job.next < atlases + stage.object_textures.size(); ++job.next) {
    // Made and uploaded by the worker.
    if (job.next < job.prepared.size() && job.prepared[job.next]) {
      mesh->sets.push_back(make_set(*job.prepared[job.next], false));
      mesh->sets_linear.push_back(make_set(*job.prepared[job.next], true));
      mesh->textures.push_back(std::move(job.prepared[job.next]));
      continue;
    }
    if (job.next > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= budget_ms) {
      batch.flush(*ctx_);
      return false;
    }
    const bool is_object = job.next >= atlases;
    const assets::StageObjectTexture* obj = is_object ? &stage.object_textures[job.next - atlases] : nullptr;
    const assets::StageTexture* atlas = is_object ? nullptr : &stage.textures[job.next];
    auto tex = std::make_unique<Texture>();
    const import::Image& img = is_object ? obj->image : atlas->image;
    if (img.width == 0 || img.height == 0 || img.indexed || img.pixels.size() < size_t(img.width) * img.height * 4) {
      log::warn("stage: texture skipped (not RGBA)");
      mesh->textures.push_back(nullptr);
      mesh->sets.push_back(TexRef());
      mesh->sets_linear.push_back(TexRef());
      continue;
    }
    // Transparent texels (colour 0) take their opaque neighbours' colour so the bilinear filter of HD mode does not draw dark
    // fringes around cut-outs (palm fronds, coral); nearest sampling discards them anyway.
    // Dump / replace through the texture registry (named by the asset's group, or the pipeline's scene group): a replacement PNG of any
    // size is fine - the stage UVs are normalised (uv_scale = 1 / atlas size is applied to the packet texels, so a bigger atlas keeps them).
    u32 tw = img.width, th = img.height;
    std::vector<u8> rgba_src;
    const std::string& group = stage.tex_group.empty() ? job.group : stage.tex_group;
    // Baked assets (battle stage, title, room meshes) dump / replace TILES at load (render/stage_tiles.h: apply_tile_textures), their object images arrive
    // here already composed. Assets that are not baked (effect meshes) keep the atlas keys (content-named).
    const bool named = stage.object_textures.empty() && !is_object && !group.empty() && is_content_named(group.substr(0, group.find('/')));
    if (named && texture_registry::active()) {
      const TextureKey key = stage_group_key(group, static_cast<int>(job.next), atlas->mode, atlas->clut_x, atlas->clut_y, tw, th, img.pixels.data());
      texture_registry::dump(key, tw, th, img.pixels.data());
      rgba_src.assign(img.pixels.begin(), img.pixels.begin() + static_cast<std::ptrdiff_t>(size_t(tw) * th * 4));
      if (!texture_registry::replace(key, tw, th, rgba_src)) rgba_src.clear();
    }
    const std::vector<u8> bled = rgba_src.empty() ? bleed_transparent(img.pixels, img.width, img.height) : bleed_transparent(rgba_src, tw, th);
    if (!tex->create(*ctx_, tw, th, TextureFormat::Rgba8, bled.data(), &batch)) {
      log::warn("stage: texture upload failed");
      mesh->textures.push_back(nullptr);
      mesh->sets.push_back(TexRef());
      mesh->sets_linear.push_back(TexRef());
      continue;
    }
    mesh->sets.push_back(make_set(*tex, false));
    mesh->sets_linear.push_back(make_set(*tex, true));
    mesh->textures.push_back(std::move(tex));
  }
  batch.flush(*ctx_);
  return true;
}

StageTextures prepare_stage_textures(GpuContext& ctx, const assets::StageAsset& stage) {
  StageTextures out;
  if (stage.object_textures.empty()) return out;
  const size_t atlases = stage.textures.size();
  const auto image = [&](size_t i) -> const import::Image& { return i >= atlases ? stage.object_textures[i - atlases].image : stage.textures[i].image; };
  std::vector<std::vector<u8>> bled(atlases + stage.object_textures.size());
  parallel_for(bled.size(), [&](size_t i) {
    const import::Image& img = image(i);
    if (img.width && img.height && !img.indexed && img.pixels.size() >= size_t(img.width) * img.height * 4) bled[i] = bleed_transparent(img.pixels, img.width, img.height);
  });
  out.resize(bled.size());
  for (size_t i = 0; i < bled.size(); ++i) {
    if (bled[i].empty()) continue;
    auto tex = std::make_unique<Texture>();
    if (tex->create_blocking(ctx, image(i).width, image(i).height, TextureFormat::Rgba8, bled[i].data())) out[i] = std::move(tex);
    bled[i] = {};
  }
  return out;
}

bool StagePipeline::reload_textures(StageMesh& mesh, const assets::StageAsset& stage) {
  const size_t atlases = stage.textures.size();
  if (!ctx_ || mesh.sets.size() != atlases + stage.object_textures.size() + 1 || mesh.sets_linear.size() != mesh.sets.size() || mesh.textures.size() + 1 != mesh.sets.size() ||
      mesh.parts.size() != stage.parts.size())
    return false;
  std::vector<assets::StageVertex> verts;
  for (const assets::StagePart& part : stage.parts) verts.insert(verts.end(), part.vertices.begin(), part.vertices.end());
  if (verts.size() * sizeof(assets::StageVertex) != mesh.vertices.size()) return false;
  UploadBatch batch;
  for (size_t k = 0; k < stage.object_textures.size(); ++k) {
    const size_t i = atlases + k;
    const import::Image& img = stage.object_textures[k].image;
    if (img.width == 0 || img.height == 0 || img.indexed || img.pixels.size() < size_t(img.width) * img.height * 4) continue;
    auto tex = std::make_unique<Texture>();
    const std::vector<u8> bled = bleed_transparent(img.pixels, img.width, img.height);
    if (!tex->create(*ctx_, img.width, img.height, TextureFormat::Rgba8, bled.data(), &batch)) continue;
    mesh.sets[i] = make_stage_set(*tex, false);
    mesh.sets_linear[i] = make_stage_set(*tex, true);
    // The old texture is released here; SDL keeps it alive until the GPU is done with it.
    mesh.textures[i] = std::move(tex);
  }
  batch.flush(*ctx_);
  return mesh.vertices.create(*ctx_, verts.data(), static_cast<u32>(verts.size() * sizeof(assets::StageVertex)), SDL_GPU_BUFFERUSAGE_VERTEX);
}

std::unique_ptr<StageMesh> StagePipeline::create_finish(StageCreateJob& job, const assets::StageAsset& stage) {
  std::unique_ptr<StageMesh> mesh = std::move(job.mesh);
  auto make_set = [&](const Texture& tex, bool linear) { return make_stage_set(tex, linear); };
  const u8 px[4] = {255, 255, 255, 255};
  mesh->white = std::make_unique<Texture>();
  if (!mesh->white->create(*ctx_, 1, 1, TextureFormat::Rgba8, px)) return nullptr;
  mesh->sets.push_back(make_set(*mesh->white, false));
  mesh->sets_linear.push_back(make_set(*mesh->white, false));
  const int white_index = static_cast<int>(mesh->sets.size()) - 1;

  std::vector<assets::StageVertex> verts;
  std::vector<u32> idx;
  for (const assets::StagePart& part : stage.parts) {
    const u32 base = static_cast<u32>(verts.size());
    verts.insert(verts.end(), part.vertices.begin(), part.vertices.end());
    // cpu_pos is in PS1 file space, y down.
    for (const assets::StageVertex& vertex : part.vertices) mesh->cpu_pos.push_back({vertex.pos[0], -vertex.pos[1], vertex.pos[2]});
    const u32 first = static_cast<u32>(idx.size());
    for (u32 i : part.indices) idx.push_back(base + i);
    StageMesh::Part mesh_part;
    mesh_part.first_sub = static_cast<u32>(mesh->subs.size());
    mesh_part.first_vertex = base;
    mesh_part.vertex_count = static_cast<u32>(part.vertices.size());
    mesh_part.ground = part.ground;
    for (const assets::StageSubMesh& sm : part.submeshes) {
      StageMesh::Sub sub;
      sub.first_index = first + sm.first_index;
      sub.index_count = sm.index_count;
      sub.semi = sm.semi;
      sub.abr = sm.abr;
      // Packets of this run: a quad was emitted as (c0,c1,c2) (c2,c1,c3) with one centroid for both triangles.
      sub.first_packet = static_cast<u32>(mesh->packets.size());
      for (u32 i = sub.first_index, end = sub.first_index + sub.index_count; i < end;) {
        StageMesh::Packet pk;
        pk.first_index = i;
        pk.corner[0] = idx[i];
        pk.corner[1] = idx[i + 1];
        pk.corner[2] = idx[i + 2];
        const bool quad = i + 6 <= end && idx[i + 3] == idx[i + 2] && idx[i + 4] == idx[i + 1] && idx[i + 5] != idx[i] &&
                          verts[idx[i + 5]].centre == verts[idx[i]].centre;
        if (quad) pk.corner[3] = idx[i + 5];
        pk.corners = quad ? 4 : 3;
        if (part.ground) {
          pk.ground_drop = sm.texture < 0;
          if (quad) {
            const std::array<float, 3>* cp[4] = {&mesh->cpu_pos[pk.corner[0]], &mesh->cpu_pos[pk.corner[1]], &mesh->cpu_pos[pk.corner[2]],
                                                 &mesh->cpu_pos[pk.corner[3]]};
            pk.cell = static_cast<s8>(ground_cell(cp));
          }
        }
        mesh->packets.push_back(pk);
        i += quad ? 6 : 3;
      }
      sub.packet_count = static_cast<u32>(mesh->packets.size()) - sub.first_packet;
      const int tex = sm.object_texture >= 0 ? static_cast<int>(stage.textures.size()) + sm.object_texture : sm.texture;
      sub.texture = tex >= 0 && tex < white_index && mesh->sets[static_cast<size_t>(tex)] ? tex : white_index;
      mesh->subs.push_back(sub);
    }
    mesh_part.sub_count = static_cast<u32>(mesh->subs.size()) - mesh_part.first_sub;
    mesh->parts.push_back(mesh_part);
  }
  if (verts.empty() || idx.empty()) {
    log::warn("stage: no geometry");
    return nullptr;
  }
  mesh->cpu_indices = idx;
  if (!mesh->vertices.create(*ctx_, verts.data(), static_cast<u32>(verts.size() * sizeof(assets::StageVertex)), SDL_GPU_BUFFERUSAGE_VERTEX) ||
      !mesh->indices.create(*ctx_, idx.data(), static_cast<u32>(idx.size() * sizeof(u32)), SDL_GPU_BUFFERUSAGE_INDEX))
    return nullptr;
  return mesh;
}

Mat4 StagePipeline::projection(const Letterbox& lb, float proj_h, float ox, float oy) {
  const float ax = 2.0f * lb.scale * proj_h / lb.vw;
  // scale_y = scale except in the field / battle picture frame (Letterbox::field).
  const float ay = 2.0f * lb.scale_y * proj_h / lb.vh;
  const float cx = (2.0f * (lb.scale * ox + lb.ox)) / lb.vw - 1.0f;
  const float cy = (2.0f * (lb.scale_y * oy + lb.oy)) / lb.vh - 1.0f;
  Mat4 proj;
  proj.at(0, 0) = ax;
  proj.at(0, 2) = cx;
  proj.at(0, 3) = 0;
  proj.at(1, 1) = ay;
  proj.at(1, 2) = cy;
  proj.at(1, 3) = 0;
  proj.at(2, 2) = 0.5f;
  proj.at(2, 3) = 0;
  proj.at(3, 2) = 1.0f;
  proj.at(3, 3) = 0;
  return proj;
}

const Pipeline* StagePipeline::pipeline(int cull, int blend, bool affine) const {
  cull = std::clamp(cull, 0, 2);
  if (view_depth_) return blend == 0 ? &pipes_view_depth_[cull] : nullptr;
  return &(affine ? pipes_affine_ : pipes_)[cull][std::clamp(blend, 0, 3)];
}

void StagePipeline::begin(const Frame& frame, Rect scissor) {
  active_ = true;
  extent_ = frame.extent;
  // A new frame: its ring buffer is free again.
  if (frame.index != frame_index_) dyn_vused_ = dyn_iused_ = 0;
  frame_index_ = frame.index;
  dyn_vertex_memory_ = static_cast<u8*>(dyn_vertices_.begin(frame));
  dyn_index_memory_ = static_cast<u8*>(dyn_indices_.begin(frame));
  gte_ = false;
  cue_ = DepthCue{};
  // SpellLights are per pass (StageScene::draw sets them after begin()).
  lights_ = SceneLights{};
  rejected_ = 0;
  ctx_->set_scissor(scissor);
}

// [Enhancements] SpellLights. The set is in the render world (y up); `cam` is the PS1-world -> camera transform of StageInstance::cam (the view's
// rot / trans, PS1 axes y down), so a light goes through (x, -y, z). The strongest kLights lights are kept (LightSet is sorted by strength).
void StagePipeline::set_lights(const LightSet& light_set, const std::array<float, 12>& cam) {
  lights_ = SceneLights{};
  for (int i = 0; i < light_set.count && lights_.count < kLights; ++i) {
    const PointLight& light = light_set.light[static_cast<size_t>(i)];
    const float world_pos[3] = {light.pos[0], -light.pos[1], light.pos[2]};
    float* cam_pos = lights_.pos[lights_.count];
    for (size_t row = 0; row < 3; ++row) cam_pos[row] = cam[row * 3] * world_pos[0] + cam[row * 3 + 1] * world_pos[1] + cam[row * 3 + 2] * world_pos[2] + cam[9 + row];
    cam_pos[3] = light.linear ? -light.radius : light.radius;
    for (size_t channel = 0; channel < 3; ++channel) lights_.col[lights_.count][channel] = light.rgb[channel];
    ++lights_.count;
  }
  for (size_t channel = 0; channel < 3; ++channel) lights_.amb[channel] = light_set.ambient[channel];
  lights_.amb_on = light_set.ambient_on();
}

void StagePipeline::set_gte(float proj_h, float ox, float oy) {
  static const bool no_reject = std::getenv("OC_STAGE_NOREJECT") != nullptr;
  // The debug switch draws every packet the GPU would clip instead; with a free camera / wide picture only the GPU near-plane clip applies (camera.h).
  gte_ = !no_reject && !cull_bypass() && !wide_picture();
  gte_h_ = proj_h;
  gte_ox_ = ox;
  gte_oy_ = oy;
  // The real visible frame; the PS1 constants at e = 0.
  gte_cf_ = cull_frame(wide_extra());
}

void StagePipeline::draw(const StageMesh& mesh, const Mat4& proj, std::span<const StageInstance> instances, bool linear,
                         const assets::StageAsset* asset) {
  if (!active_ || instances.empty()) return;
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  linear_draw_ = linear && mesh.sets_linear.size() == mesh.sets.size();
  const std::vector<TexRef>& sets = linear_draw_ ? mesh.sets_linear : mesh.sets;
  // Morphing instances (op 37) and the ones sampling a screen copy are drawn last from per-frame copies of their vertices (see below); the
  // static pass skips them.
  std::vector<const StageInstance*> morphing, grounds;
  struct Item {
    const StageInstance* in;
    const StageMesh::Sub* sub;
    int blend;
    float factor, alpha, depth;
  };
  std::vector<Item> opaque, semi;
  for (const StageInstance& in : instances) {
    if (in.part < 0 || static_cast<size_t>(in.part) >= mesh.parts.size()) continue;
    if (rebuilt_morph(in, asset) || rebuilt_screen(in, asset)) {
      morphing.push_back(&in);
      continue;
    }
    if ((in.ground || (in.warp && *in.warp)) && asset && static_cast<size_t>(in.part) < asset->parts.size()) {
      grounds.push_back(&in);
      continue;
    }
    const StageMesh::Part& mesh_part = mesh.parts[static_cast<size_t>(in.part)];
    for (u32 sub_index = mesh_part.first_sub; sub_index < mesh_part.first_sub + mesh_part.sub_count; ++sub_index) {
      Item it{&in, &mesh.subs[sub_index], 0, 1.0f, 1.0f, 0.0f};
      it.blend = draw_blend(in, *it.sub, it.factor, it.alpha);
      it.depth = in.fixed_slot ? static_cast<float>(in.bias) : in.cam[11] / 16.0f + static_cast<float>(in.bias);
      (it.blend == 0 ? opaque : semi).push_back(it);
    }
  }
  // Opaque faces grouped by texture (fewer binds); semi-transparent ones back to front like the ordering table.
  std::stable_sort(opaque.begin(), opaque.end(), [](const Item& lhs, const Item& rhs) { return lhs.sub->texture < rhs.sub->texture; });
  std::stable_sort(semi.begin(), semi.end(), [](const Item& lhs, const Item& rhs) { return lhs.depth > rhs.depth; });

  bind_vertex_buffer(render_pass, mesh.vertices.handle());
  bind_index_buffer(render_pass, mesh.indices.handle());
  const Pipeline* bound = nullptr;
  int bound_tex = -1;
  const StageInstance* bound_in = nullptr;
  int bound_blend = -1;
  float bound_factor = 0, bound_alpha = 0;
  std::vector<u32> kept;
  auto run = [&](const std::vector<Item>& items) {
    for (const Item& it : items) {
      const Pipeline* pl = pipeline(it.in->cull, it.blend, ps1_jitter().on);
      if (!pl) continue;
      if (pl != bound) {
        SDL_BindGPUGraphicsPipeline(render_pass, pl->handle());
        bound = pl;
        bound_tex = -1;
        bound_in = nullptr;
      }
      if (it.sub->texture != bound_tex) {
        bind_texture(*ctx_, render_pass, sets[static_cast<size_t>(it.sub->texture)]);
        bound_tex = it.sub->texture;
      }
      if (it.in != bound_in || it.blend != bound_blend || it.factor != bound_factor || it.alpha != bound_alpha) {
        const StageInstance& in = *it.in;
        Mat4 cam;
        for (int row = 0; row < 3; ++row) {
          for (int col = 0; col < 3; ++col) cam.at(row, col) = in.cam[static_cast<size_t>(row * 3 + col)];
          cam.at(row, 3) = in.cam[static_cast<size_t>(9 + row)];
        }
        PushData pd;
        pd.clip = proj * cam;
        fill_push(pd, in, cue_, it.factor, it.alpha, smooth_fog_, linear_draw_);
        fill_lights(pd, proj, it.blend, lights_);
        push_uniforms(command_buffer, pd, view_depth_);
        bound_in = it.in;
        bound_blend = it.blend;
        bound_factor = it.factor;
        bound_alpha = it.alpha;
      }
      if (!gte_) {
        SDL_DrawGPUIndexedPrimitives(render_pass, it.sub->index_count, 1, it.sub->first_index, 0, 0);
        continue;
      }
      // Per-packet rejection: the surviving triangles go through the dynamic index ring (vertices stay in the static buffer).
      kept.clear();
      for (u32 packet_index = it.sub->first_packet; packet_index < it.sub->first_packet + it.sub->packet_count; ++packet_index) {
        const StageMesh::Packet& pk = mesh.packets[packet_index];
        bool tri_ok[2] = {false, false};
        if (!packet_passes(*it.in, mesh, pk, gte_h_, gte_ox_, gte_oy_, tri_ok, gte_cf_)) {
          ++rejected_;
          continue;
        }
        if (!tri_ok[0] && !tri_ok[1]) {
          ++rejected_;
          continue;
        }
        for (u32 tri = 0; tri < (pk.corners == 4 ? 2u : 1u); ++tri)
          if (tri_ok[tri])
            for (u32 k = 0; k < 3; ++k) kept.push_back(mesh.cpu_indices[pk.first_index + tri * 3 + k]);
      }
      if (kept.empty()) continue;
      if (dyn_iused_ + kept.size() > kDynIndices) {
        log::warn("stage: index ring overflow ({} + {})", dyn_iused_, kept.size());
        continue;
      }
      write_indices(kept.data(), static_cast<u32>(kept.size()), dyn_iused_);
      SDL_DrawGPUIndexedPrimitives(render_pass, static_cast<u32>(kept.size()), 1, dyn_iused_, 0, 0);
      dyn_iused_ += static_cast<u32>(kept.size());
    }
  };
  if (gte_) bind_index_buffer(render_pass, dyn_indices_.handle());
  run(opaque);
  // [Enhancements] EdgeBlend: the static instances' seam strips over their opaque faces, before the semi-transparent layers.
  if (edge_blend() && asset && !view_depth_) {
    bool any = false;
    std::vector<u8> kept_pk;
    for (const StageInstance& in : instances) {
      if (in.part < 0 || static_cast<size_t>(in.part) >= mesh.parts.size() || static_cast<size_t>(in.part) >= asset->parts.size()) continue;
      if (std::find(morphing.begin(), morphing.end(), &in) != morphing.end() || std::find(grounds.begin(), grounds.end(), &in) != grounds.end()) continue;
      const assets::StagePart& ap = asset->parts[static_cast<size_t>(in.part)];
      const StageMesh::Part& mp = mesh.parts[static_cast<size_t>(in.part)];
      if (ap.seams.empty() || mp.sub_count == 0 || mp.vertex_count != ap.vertices.size()) continue;
      const std::vector<u8>* kp = nullptr;
      // The strips of the packets the GTE rules dropped stay away too.
      if (gte_) {
        const u32 q0 = mesh.subs[mp.first_sub].first_packet, q1 = mesh.subs[mp.first_sub + mp.sub_count - 1].first_packet + mesh.subs[mp.first_sub + mp.sub_count - 1].packet_count;
        kept_pk.assign(q1 - q0, 0);
        for (u32 packet_index = q0; packet_index < q1; ++packet_index) {
          bool tri_ok[2] = {false, false};
          kept_pk[packet_index - q0] = packet_passes(in, mesh, mesh.packets[packet_index], gte_h_, gte_ox_, gte_oy_, tri_ok, gte_cf_) && (tri_ok[0] || tri_ok[1]) ? 1 : 0;
        }
        kp = &kept_pk;
      }
      Mat4 cam;
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) cam.at(row, col) = in.cam[static_cast<size_t>(row * 3 + col)];
        cam.at(row, 3) = in.cam[static_cast<size_t>(9 + row)];
      }
      draw_seams(mesh, ap, mp, proj, cam, in, sets, ap.vertices, kp, ps1_jitter().on);
      any = true;
    }
    // Back to the static vertex buffer; the run() binding cache is stale.
    if (any) {
      bind_vertex_buffer(render_pass, mesh.vertices.handle());
      bound = nullptr;
      bound_tex = -1;
      bound_in = nullptr;
    }
  }
  run(semi);
  if (gte_) bind_index_buffer(render_pass, mesh.indices.handle());

  for (const StageInstance* pin : grounds) draw_cpu(mesh, *asset, proj, *pin, sets);

  // Op 37 morph: vertices = lerp(A, B, t); a screen-copy instance: the UVs of its screen sub-meshes address the copy. Rebuilt per frame in the
  // dynamic ring buffer and drawn with the part's own sub-meshes (textures, blend) through the same per-draw uniforms as a static instance.
  for (const StageInstance* pin : morphing) {
    const StageInstance& in = *pin;
    const assets::StagePart& from_part = asset->parts[static_cast<size_t>(in.part)];
    const u32 nv = static_cast<u32>(from_part.vertices.size()), ni = static_cast<u32>(from_part.indices.size());
    if (dyn_vused_ + nv > kDynVertices || dyn_iused_ + ni > kDynIndices) {
      log::warn("stage: rebuilt geometry overflow ({} vertices, {} indices)", nv, ni);
      continue;
    }
    std::vector<assets::StageVertex> blended(from_part.vertices.begin(), from_part.vertices.end());
    if (rebuilt_morph(in, asset)) {
      const assets::StagePart& to_part = asset->parts[static_cast<size_t>(in.morph_part)];
      const float morph_t = std::clamp(in.morph_t, 0.0f, 1.0f);
      for (u32 i = 0; i < nv; ++i) {
        const assets::StageVertex& tv = to_part.vertices[i];
        for (size_t k = 0; k < 3; ++k) {
          blended[i].pos[k] += (tv.pos[k] - blended[i].pos[k]) * morph_t;
          blended[i].centre[k] += (tv.centre[k] - blended[i].centre[k]) * morph_t;
        }
      }
    }
    // The packet texel u, v of a page at (dx, dy) in the copy addresses copy pixel (dx + u, dy + v); the texel centre is half a pixel in.
    const bool screen = rebuilt_screen(in, asset);
    if (screen)
      for (const assets::StageSubMesh& asub : from_part.submeshes) {
        int dx = 0, dy = 0;
        if (!in.screen_offset(asub.direct_page, dx, dy)) continue;
        for (u32 k = asub.first_index; k < asub.first_index + asub.index_count && k < ni; ++k) {
          assets::StageVertex& vertex = blended[from_part.indices[k]];
          const float copy_x = static_cast<float>(dx) + vertex.uv_raw[0] + 0.5f, copy_y = static_cast<float>(dy) + vertex.uv_raw[1] + 0.5f;
          vertex.uv = {copy_x * in.screen_uv[0] + in.screen_uv[2], 1.0f - (copy_y * in.screen_uv[1] + in.screen_uv[3])};
        }
      }
    write_vertices(blended.data(), nv, dyn_vused_);
    write_indices(from_part.indices.data(), ni, dyn_iused_);
    bind_vertex_buffer(render_pass, dyn_vertices_.handle());
    bind_index_buffer(render_pass, dyn_indices_.handle());
    const StageMesh::Part& mp = mesh.parts[static_cast<size_t>(in.part)];
    Mat4 cam;
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) cam.at(row, col) = in.cam[static_cast<size_t>(row * 3 + col)];
      cam.at(row, 3) = in.cam[static_cast<size_t>(9 + row)];
    }
    for (u32 sub_index = 0; sub_index < mp.sub_count && sub_index < from_part.submeshes.size(); ++sub_index) {
      const StageMesh::Sub& sub = mesh.subs[mp.first_sub + sub_index];
      const assets::StageSubMesh& asub = from_part.submeshes[sub_index];
      float factor = 1.0f, alpha = 1.0f;
      const int blend = draw_blend(in, sub, factor, alpha);
      const Pipeline* pl = pipeline(in.cull, blend, ps1_jitter().on);
      if (!pl) continue;
      SDL_BindGPUGraphicsPipeline(render_pass, pl->handle());
      // A screen sub-mesh samples the copy at its rebuilt UVs, never through the op-36 window.
      int dx = 0, dy = 0;
      const bool on_screen = screen && in.screen_offset(asub.direct_page, dx, dy);
      StageInstance plain = in;
      plain.uv_scroll = plain.uv_scroll && !on_screen;
      bind_texture(*ctx_, render_pass, on_screen ? in.screen : sets[static_cast<size_t>(sub.texture)]);
      PushData pd;
      pd.clip = proj * cam;
      fill_push(pd, plain, cue_, factor, alpha, smooth_fog_, linear_draw_);
      if (on_screen) pd.params[1] += in.screen_opaque ? 96.0f : 32.0f;
      fill_lights(pd, proj, blend, lights_);
      push_uniforms(command_buffer, pd, view_depth_);
      SDL_DrawGPUIndexedPrimitives(render_pass, asub.index_count, 1, dyn_iused_ + asub.first_index, static_cast<int32_t>(dyn_vused_), 0);
    }
    dyn_vused_ += nv;
    dyn_iused_ += ni;
    if (edge_blend() && !from_part.seams.empty() && !screen) draw_seams(mesh, from_part, mp, proj, cam, in, sets, blended, nullptr, ps1_jitter().on);
    // restore the static buffers for any later static draw in this pass
    bind_vertex_buffer(render_pass, mesh.vertices.handle());
    bind_index_buffer(render_pass, mesh.indices.handle());
  }
}

// CPU-built instances: the battle ground (the grid cells plus the textured rim triangles:
// every cell's corners carry the farthest corner as their packet centre, i.e. the cell's OT slot = max SZ >> 2 >> z-shift + bias; hidden
// cells and the untextured packets left out) and the warped instances (each vertex to world space, warped, drawn
// through the view). The part's vertices are copied into the dynamic ring buffer.
void StagePipeline::draw_cpu(const StageMesh& mesh, const assets::StageAsset& asset, const Mat4& proj, const StageInstance& in,
                             const std::vector<TexRef>& sets) {
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  const StageMesh::Part& mp = mesh.parts[static_cast<size_t>(in.part)];
  const assets::StagePart& ap = asset.parts[static_cast<size_t>(in.part)];
  const u32 base = mp.first_vertex, nv = mp.vertex_count;
  if (nv != ap.vertices.size() || nv == 0) return;
  std::vector<assets::StageVertex> vv(ap.vertices.begin(), ap.vertices.end());
  // ps1: PS1-space position (y down) under the transform `xf`.
  std::vector<std::array<float, 3>> ps1(nv);
  const bool warp = in.warp && *in.warp;
  const std::array<float, 12>& xf = warp ? in.warp_view : in.cam;
  for (u32 i = 0; i < nv; ++i) {
    std::array<float, 3> point = mesh.cpu_pos[base + i];
    // Warped: camera space by the instance, back to world with the inverse view rotation, then warped.
    if (warp) {
      float cam_pos[3];
      for (int row = 0; row < 3; ++row)
        cam_pos[row] = in.cam[static_cast<size_t>(row * 3)] * point[0] + in.cam[static_cast<size_t>(row * 3 + 1)] * point[1] + in.cam[static_cast<size_t>(row * 3 + 2)] * point[2] +
                       in.cam[static_cast<size_t>(9 + row)] - xf[static_cast<size_t>(9 + row)];
      for (size_t k = 0; k < 3; ++k) point[k] = xf[k] * cam_pos[0] + xf[3 + k] * cam_pos[1] + xf[6 + k] * cam_pos[2];
      (*in.warp)(point);
      vv[i].pos = {point[0], -point[1], point[2]};
    }
    ps1[i] = point;
  }
  std::vector<u32> ix;
  std::vector<std::pair<u32, u32>> runs(mp.sub_count, {0u, 0u});
  // fills: the split cells' edge triangles, per sub-mesh (fill_runs index into fills).
  std::vector<u32> fills;
  std::vector<std::pair<u32, u32>> fill_runs(mp.sub_count, {0u, 0u});
  const u32 part_q0 = mesh.subs[mp.first_sub].first_packet;
  // kept: per packet, for the seam strips.
  std::vector<u8> kept(mesh.subs[mp.first_sub + mp.sub_count - 1].first_packet + mesh.subs[mp.first_sub + mp.sub_count - 1].packet_count - part_q0, 0);
  for (u32 sub_index = 0; sub_index < mp.sub_count; ++sub_index) {
    const StageMesh::Sub& sub = mesh.subs[mp.first_sub + sub_index];
    runs[sub_index].first = static_cast<u32>(ix.size());
    fill_runs[sub_index].first = static_cast<u32>(fills.size());
    for (u32 packet_index = sub.first_packet; packet_index < sub.first_packet + sub.packet_count; ++packet_index) {
      const StageMesh::Packet& pk = mesh.packets[packet_index];
      if (in.ground && pk.ground_drop) continue;
      bool tri_ok[2] = {true, pk.corners == 4};
      // The packet centre moves with its corners.
      if (warp) {
        std::array<float, 3> centre{0, 0, 0};
        for (int k = 0; k < pk.corners; ++k)
          for (size_t axis = 0; axis < 3; ++axis) centre[axis] += vv[pk.corner[k] - base].pos[axis] / static_cast<float>(pk.corners);
        for (int k = 0; k < pk.corners; ++k) vv[pk.corner[k] - base].centre = centre;
      }
      if (in.ground && pk.cell >= 0) {
        if (in.hide.test(static_cast<size_t>(pk.cell))) continue;
        // Ground cells: NCLIP is the only test besides the slot range ((max z >> 2) >> 2 must stay 0..4095); no GTE flag test.
        int far = 0;
        float zmax = -1e30f;
        for (int k = 0; k < pk.corners; ++k) {
          const float z = cam_z(xf, ps1[pk.corner[k] - base]);
          if (z > zmax) zmax = z, far = k;
        }
        if (zmax < 0.0f || zmax >= 65536.0f) {
          ++rejected_;
          continue;
        }
        const std::array<float, 3> centre = vv[pk.corner[far] - base].pos;
        for (int k = 0; k < pk.corners; ++k) vv[pk.corner[k] - base].centre = centre;
        // [Video] Ps1Jitter (affine texturing): near cells (ordering-table slot (max SZ >> 2) >> 2 below 0x200, i.e. the farthest corner nearer
        // than 8192 units) are drawn as four GT4 sub-quads whose new corners average the parent's positions, texel coordinates and colours (the
        // midpoint bytes `(a + b) >> 1`, the same clut / tpage, all four at the parent's slot). This halves the affine warp of the big near quads
        // and only changes that warp, so perspective-correct drawing keeps the whole cell. The sub-quads keep the parent's far corner as their depth.
        // Each snapped edge midpoint can leave the straight edge of an unsplit neighbour, so four GT3 triangles (corner, edge midpoint, corner)
        // along the outer edges fill that gap: no NCLIP test, the parent's slot, drawn under the sub-quads.
        if (ps1_jitter().on && pk.corners == 4 && zmax / 16.0f < 512.0f) {
          const u32 c0 = pk.corner[0] - base, c1 = pk.corner[1] - base, c2 = pk.corner[2] - base, c3 = pk.corner[3] - base;
          auto mid = [&](u32 index_a, u32 index_b) {
            assets::StageVertex middle = vv[index_a];
            const assets::StageVertex& vb = vv[index_b];
            for (size_t i = 0; i < 3; ++i) middle.pos[i] = (middle.pos[i] + vb.pos[i]) * 0.5f, middle.color[i] = (middle.color[i] + vb.color[i]) * 0.5f;
            for (size_t i = 0; i < 2; ++i) middle.uv[i] = (middle.uv[i] + vb.uv[i]) * 0.5f, middle.uv_raw[i] = (middle.uv_raw[i] + vb.uv_raw[i]) * 0.5f;
            vv.push_back(middle);
            return static_cast<u32>(vv.size() - 1);
          };
          const u32 m01 = mid(c0, c1), m23 = mid(c2, c3), m02 = mid(c0, c2), m13 = mid(c1, c3), mc = mid(m01, m23);
          const u32 quads[4][4] = {{c0, m01, m02, mc}, {m01, c1, mc, m13}, {m02, mc, c2, m23}, {mc, m13, m23, c3}};
          for (const u32* sq : quads) {
            ix.insert(ix.end(), {sq[0], sq[1], sq[2], sq[2], sq[1], sq[3]});
          }
          fills.insert(fills.end(), {c0, m02, c2, c1, c3, m13, c0, c1, m01, c2, c3, m23});
          kept[packet_index - part_q0] = 1;
          continue;
        }
      } else if (gte_) {
        const std::array<float, 3>* pos[4] = {};
        for (int k = 0; k < pk.corners; ++k) pos[k] = &ps1[pk.corner[k] - base];
        if (!corners_pass(xf, pos, pk.corners, gte_h_, gte_ox_, gte_oy_, tri_ok, gte_cf_)) {
          ++rejected_;
          continue;
        }
      }
      for (u32 tri = 0; tri < (pk.corners == 4 ? 2u : 1u); ++tri)
        if (tri_ok[tri])
          for (u32 k = 0; k < 3; ++k) ix.push_back(mesh.cpu_indices[pk.first_index + tri * 3 + k] - base);
      kept[packet_index - part_q0] = tri_ok[0] || tri_ok[1] ? 1 : 0;
    }
    runs[sub_index].second = static_cast<u32>(ix.size()) - runs[sub_index].first;
    fill_runs[sub_index].second = static_cast<u32>(fills.size()) - fill_runs[sub_index].first;
  }
  if (ix.empty()) return;
  const u32 fill_base = static_cast<u32>(ix.size());
  ix.insert(ix.end(), fills.begin(), fills.end());
  // nvv: nv plus the near-cell sub-quad corners.
  const u32 nvv = static_cast<u32>(vv.size());
  if (dyn_vused_ + nvv > kDynVertices || dyn_iused_ + ix.size() > kDynIndices) {
    log::warn("stage: CPU geometry overflow ({} vertices, {} indices)", nvv, ix.size());
    return;
  }
  write_vertices(vv.data(), nvv, dyn_vused_);
  write_indices(ix.data(), static_cast<u32>(ix.size()), dyn_iused_);
  bind_vertex_buffer(render_pass, dyn_vertices_.handle());
  bind_index_buffer(render_pass, dyn_indices_.handle());
  Mat4 cam;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) cam.at(row, col) = xf[static_cast<size_t>(row * 3 + col)];
    cam.at(row, 3) = xf[static_cast<size_t>(9 + row)];
  }
  for (u32 sub_index = 0; sub_index < mp.sub_count; ++sub_index) {
    if (runs[sub_index].second == 0) continue;
    const StageMesh::Sub& sub = mesh.subs[mp.first_sub + sub_index];
    float factor = 1.0f, alpha = 1.0f;
    int blend = 0;
    if (in.ground) {
      // Semi-transparent with the tpage ABR bits cleared: B/2 + F/2.
      blend = in.blend != 0 ? 1 : 0;
      alpha = blend == 1 ? 0.5f : 1.0f;
    } else {
      blend = draw_blend(in, sub, factor, alpha);
    }
    const Pipeline* fill_pipe = pipeline(2, blend, ps1_jitter().on);
    const Pipeline* pipe = pipeline(in.cull, blend, ps1_jitter().on);
    if (!pipe) continue;
    bind_texture(*ctx_, render_pass, sets[static_cast<size_t>(sub.texture)]);
    PushData pd;
    pd.clip = proj * cam;
    fill_push(pd, in, cue_, factor, alpha, smooth_fog_, linear_draw_);
    fill_lights(pd, proj, blend, lights_);
    if (fill_runs[sub_index].second > 0) {
      SDL_BindGPUGraphicsPipeline(render_pass, fill_pipe->handle());
      push_uniforms(command_buffer, pd, view_depth_);
      SDL_DrawGPUIndexedPrimitives(render_pass, fill_runs[sub_index].second, 1, dyn_iused_ + fill_base + fill_runs[sub_index].first, static_cast<int32_t>(dyn_vused_), 0);
    }
    SDL_BindGPUGraphicsPipeline(render_pass, pipe->handle());
    push_uniforms(command_buffer, pd, view_depth_);
    SDL_DrawGPUIndexedPrimitives(render_pass, runs[sub_index].second, 1, dyn_iused_ + runs[sub_index].first, static_cast<int32_t>(dyn_vused_), 0);
  }
  dyn_vused_ += nvv;
  dyn_iused_ += static_cast<u32>(ix.size());
  // [Enhancements] EdgeBlend: the kept cells' seam strips at the cells' own depth (vv carries the far-corner centres and the warp)
  if (edge_blend() && !ap.seams.empty() && (!in.ground || in.blend == 0)) draw_seams(mesh, ap, mp, proj, cam, in, sets, vv, &kept, ps1_jitter().on);
  bind_vertex_buffer(render_pass, mesh.vertices.handle());
  bind_index_buffer(render_pass, mesh.indices.handle());
}

void StagePipeline::draw_seams(const StageMesh& mesh, const assets::StagePart& part, const StageMesh::Part& mp, const Mat4& proj, const Mat4& cam,
                               const StageInstance& in, const std::vector<TexRef>& sets, const std::vector<assets::StageVertex>& vv,
                               const std::vector<u8>* kept, bool affine) {
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  if (!edge_blend() || view_depth_ || part.seams.empty() || mp.sub_count == 0 || vv.size() < part.vertices.size()) return;
  // packet of each index position of the part (the seams name their triangle's first index)
  const u32 part_i0 = mesh.subs[mp.first_sub].first_index, part_q0 = mesh.subs[mp.first_sub].first_packet;
  const StageMesh::Sub& last = mesh.subs[mp.first_sub + mp.sub_count - 1];
  std::vector<u32> pk_of(last.first_index + last.index_count - part_i0, ~0u);
  for (u32 packet_index = part_q0; packet_index < last.first_packet + last.packet_count; ++packet_index) {
    const StageMesh::Packet& pk = mesh.packets[packet_index];
    for (u32 i = pk.first_index, end = pk.first_index + (pk.corners == 4 ? 6u : 3u); i < end && i - part_i0 < pk_of.size(); ++i) pk_of[i - part_i0] = packet_index - part_q0;
  }
  // Only the opaque sub-meshes get strips.
  std::vector<u8> opaque(mp.sub_count, 0);
  for (u32 sub_index = 0; sub_index < mp.sub_count; ++sub_index) {
    float factor = 1.0f, alpha = 1.0f;
    opaque[sub_index] = (in.ground ? in.blend == 0 : draw_blend(in, mesh.subs[mp.first_sub + sub_index], factor, alpha) == 0) ? 1 : 0;
  }
  std::vector<std::vector<assets::StageVertex>> per_sub(mp.sub_count);
  u32 total = 0;
  for (const assets::StageSeam& sm : part.seams) {
    if (sm.submesh < 0 || static_cast<u32>(sm.submesh) >= mp.sub_count || !opaque[static_cast<size_t>(sm.submesh)]) continue;
    if (sm.index_pos >= pk_of.size() || pk_of[sm.index_pos] == ~0u) continue;
    if (kept && (pk_of[sm.index_pos] >= kept->size() || !(*kept)[pk_of[sm.index_pos]])) continue;
    bool ok = true;
    for (u32 corner = 0; corner < sm.corners; ++corner) ok = ok && sm.v[corner] < vv.size();
    if (!ok) continue;
    std::vector<assets::StageVertex>& out = per_sub[static_cast<size_t>(sm.submesh)];
    static constexpr u32 kQuad[6] = {0, 1, 2, 2, 1, 3};
    const u32 vertex_count = sm.corners == 4 ? 6u : 3u;
    for (u32 k = 0; k < vertex_count; ++k) {
      const u32 corner = kQuad[k];
      assets::StageVertex vertex = vv[sm.v[corner]];
      vertex.uv = sm.uv[corner];
      // The fragment shader's fade: distance from the edge in bands (stage.frag, seam mode: alpha 1/2 at 0, 0 at 1).
      vertex.uv_raw = {sm.g[corner], 1.0f};
      out.push_back(vertex);
    }
    total += vertex_count;
  }
  if (total == 0) return;
  if (dyn_vused_ + total > kDynVertices) {
    log::warn("stage: seam strips overflow ({} + {} vertices)", dyn_vused_, total);
    return;
  }
  bind_vertex_buffer(render_pass, dyn_vertices_.handle());
  // Alpha blend, depth tested (LEQUAL: the strip's slot = its face's).
  const Pipeline& pl = (affine ? pipes_affine_ : pipes_)[std::clamp(in.cull, 0, 2)][1];
  SDL_BindGPUGraphicsPipeline(render_pass, pl.handle());
  PushData pd;
  pd.clip = proj * cam;
  fill_push(pd, in, cue_, 1.0f, 1.0f, smooth_fog_);
  // +16 = seam mode (no scroll: scrolling objects have no seams).
  pd.params[1] = (in.fixed_slot ? 1.0f : 0.0f) + (smooth_fog_ ? 8.0f : 0.0f) + 16.0f;
  pd.uvw[0] = pd.uvw[1] = 0.0f;
  fill_lights(pd, proj, 1, lights_);
  push_uniforms(command_buffer, pd, view_depth_);
  u32 at = dyn_vused_;
  for (u32 sub_index = 0; sub_index < mp.sub_count; ++sub_index) {
    const std::vector<assets::StageVertex>& out = per_sub[sub_index];
    if (out.empty()) continue;
    write_vertices(out.data(), static_cast<u32>(out.size()), at);
    bind_texture(*ctx_, render_pass, sets[static_cast<size_t>(mesh.subs[mp.first_sub + sub_index].texture)]);
    SDL_DrawGPUPrimitives(render_pass, static_cast<u32>(out.size()), 1, at, 0);
    at += static_cast<u32>(out.size());
  }
  dyn_vused_ = at;
}

void StagePipeline::draw_prims(const StageMesh& mesh, const assets::StageAsset& asset, const Mat4& proj, const std::array<float, 12>& cam,
                               std::span<const StagePrim> prims, bool linear) {
  if (!active_ || prims.empty()) return;
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  const std::vector<TexRef>& sets = linear && mesh.sets_linear.size() == mesh.sets.size() ? mesh.sets_linear : mesh.sets;
  const int white_index = static_cast<int>(mesh.sets.size()) - 1;
  struct Item {
    u32 first_vertex;
    int texture, blend, bias;
    bool two_sided, depth_cue;
    float depth;
  };
  std::vector<assets::StageVertex> vv;
  std::vector<Item> items;
  vv.reserve(prims.size() * 4);
  for (const StagePrim& prim : prims) {
    const int corner_count = prim.n == 4 ? 4 : 3;
    bool tri_ok[2] = {true, corner_count == 4};
    // The PS1 packet builders' rejection rules.
    if (gte_) {
      const std::array<float, 3>* pos[4] = {&prim.pos[0], &prim.pos[1], &prim.pos[2], &prim.pos[3]};
      if (!corners_pass(cam, pos, corner_count, gte_h_, gte_ox_, gte_oy_, tri_ok, gte_cf_) || (!tri_ok[0] && !tri_ok[1])) {
        ++rejected_;
        continue;
      }
    }
    int tex = -1, ppw = 2, tpx = 0, tpy = 256;
    std::array<int, 2> cl_dbg{0, 0};
    if (prim.textured) {
      const assets::StageTpage tp = assets::decode_tpage(prim.tpage);
      const auto cl = assets::decode_clut(prim.clut);
      cl_dbg = cl;
      tex = asset.find_texture(tp.mode, cl[0], cl[1]);
      if (tex >= 0 && (static_cast<size_t>(tex) + 1 >= sets.size() || !sets[static_cast<size_t>(tex)])) tex = -1;
      ppw = tp.mode == 0 ? 4 : 2;
      tpx = tp.x;
      tpy = tp.y;
    }
    const float atlas_width = static_cast<float>(asset.atlas_width_hw * ppw);
    // Tiles (render/stage_tiles.h): a packet whose texels lie in ONE rect that stage faces sample is drawn from the composed (possibly replaced) object texture
    // that holds the tile - UV = composed texel * scale - so the overlays' cells match the normal ground. Anything else keeps the raw atlas.
    bool tiled = false;
    float tu[4] = {}, tv[4] = {}, tsx = 0, tsy = 0;
    if (tex >= 0 && !asset.object_textures.empty()) {
      bool ok = true;
      int rect = -1, obj = -1;
      for (int k = 0; k < corner_count && ok; ++k) {
        TileTexel texel;
        const int x = (tpx - asset.atlas_x0_hw) * ppw + prim.uv[static_cast<size_t>(k)][0], y = (tpy - 256) + prim.uv[static_cast<size_t>(k)][1];
        if (!tile_lookup(asset, tex, x, y, texel) || (k > 0 && texel.rect != rect)) {
          ok = false;
          break;
        }
        rect = texel.rect;
        obj = texel.object;
        const import::Image& im = asset.object_textures[static_cast<size_t>(obj)].image;
        tu[k] = texel.px / static_cast<float>(im.width);
        tv[k] = 1.0f - texel.py / static_cast<float>(im.height);
        tsx = texel.fx / static_cast<float>(im.width);
        tsy = texel.fy / static_cast<float>(im.height);
      }
      const int oi = ok ? static_cast<int>(asset.textures.size()) + obj : -1;
      if (ok && oi >= 0 && static_cast<size_t>(oi) + 1 < sets.size() && sets[static_cast<size_t>(oi)]) {
        tex = oi;
        tiled = true;
      } else if (++raw_prims_ == 1 || (raw_prims_ & (raw_prims_ - 1)) == 0) {
        log::info("stage: {} effect polygon(s) sample texels no stage face uses: drawn from the raw atlas (first: mode {} CLUT {},{} page {},{})", raw_prims_, tpx >= 0 ? (ppw == 4 ? 0 : 1) : 0,
                  cl_dbg[0], cl_dbg[1], tpx, tpy);
      }
    }
    // cen: asset space (y up), like StageVertex::centre.
    std::array<float, 3> cen{0, 0, 0};
    for (int k = 0; k < corner_count; ++k) {
      cen[0] += prim.pos[static_cast<size_t>(k)][0] / static_cast<float>(corner_count);
      cen[1] -= prim.pos[static_cast<size_t>(k)][1] / static_cast<float>(corner_count);
      cen[2] += prim.pos[static_cast<size_t>(k)][2] / static_cast<float>(corner_count);
    }
    Item it{static_cast<u32>(vv.size()), tex >= 0 ? tex : white_index, std::clamp(prim.blend, 0, 4), prim.bias, prim.two_sided, prim.depth_cue,
            cam_z(cam, {cen[0], -cen[1], cen[2]})};
    for (int k = 0; k < 4; ++k) {
      const size_t ks = static_cast<size_t>(k < corner_count ? k : 2);
      assets::StageVertex vertex;
      vertex.pos = {prim.pos[ks][0], -prim.pos[ks][1], prim.pos[ks][2]};
      vertex.centre = cen;
      // Textured corners modulate (0x80 = 1.0), untextured ones are raw colours.
      const float cs = tex >= 0 ? 128.0f : 255.0f;
      vertex.color = {prim.rgb[ks][0] / cs, prim.rgb[ks][1] / cs, prim.rgb[ks][2] / cs};
      if (tiled) {
        vertex.uv = {tu[ks], tv[ks]};
        vertex.uv_raw = {static_cast<float>(prim.uv[ks][0]), static_cast<float>(prim.uv[ks][1])};
        vertex.uv_scale = {tsx, tsy};
      } else if (tex >= 0) {
        const int u = (tpx - asset.atlas_x0_hw) * ppw + prim.uv[ks][0], w = (tpy - 256) + prim.uv[ks][1];
        vertex.uv = {(static_cast<float>(u) + 0.5f) / atlas_width, 1.0f - (static_cast<float>(w) + 0.5f) / 256.0f};
        vertex.uv_raw = {static_cast<float>(prim.uv[ks][0]), static_cast<float>(prim.uv[ks][1])};
        vertex.uv_scale = {1.0f / atlas_width, 1.0f / 256.0f};
      } else {
        vertex.uv = {0.5f, 0.5f};
      }
      vv.push_back(vertex);
    }
    // The GPU size rule may drop one triangle of a quad: collapse it (corner 3 onto corner 2 / corner 0 onto corner 1).
    if (corner_count == 4 && !tri_ok[1]) vv[it.first_vertex + 3] = vv[it.first_vertex + 2];
    if (!tri_ok[0]) vv[it.first_vertex] = vv[it.first_vertex + 1];
    if (corner_count == 3) vv[it.first_vertex + 3] = vv[it.first_vertex + 2];
    items.push_back(it);
  }
  if (items.empty()) return;
  if (dyn_vused_ + vv.size() > kDynVertices || dyn_iused_ + items.size() * 6 > kDynIndices) {
    log::warn("stage: effect polygon overflow ({} polygons)", items.size());
    return;
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& lhs, const Item& rhs) {
    if ((lhs.blend == 0) != (rhs.blend == 0)) return lhs.blend == 0;
    return lhs.blend != 0 && lhs.depth > rhs.depth;
  });
  std::vector<u32> ix;
  ix.reserve(items.size() * 6);
  for (const Item& it : items)
    for (u32 k : {0u, 1u, 2u, 2u, 1u, 3u}) ix.push_back(it.first_vertex + k);
  write_vertices(vv.data(), static_cast<u32>(vv.size()), dyn_vused_);
  write_indices(ix.data(), static_cast<u32>(ix.size()), dyn_iused_);
  bind_vertex_buffer(render_pass, dyn_vertices_.handle());
  bind_index_buffer(render_pass, dyn_indices_.handle());
  Mat4 cam_matrix;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) cam_matrix.at(row, col) = cam[static_cast<size_t>(row * 3 + col)];
    cam_matrix.at(row, 3) = cam[static_cast<size_t>(9 + row)];
  }
  // Per-draw uniforms: neutral object colour, the item's bias, and the depth cue per vertex for the items that take it.
  StageInstance neutral;
  size_t i = 0;
  while (i < items.size()) {
    size_t j = i + 1;
    while (j < items.size() && items[j].texture == items[i].texture && items[j].blend == items[i].blend && items[j].bias == items[i].bias &&
           items[j].two_sided == items[i].two_sided && items[j].depth_cue == items[i].depth_cue)
      ++j;
    const Item& it = items[i];
    const StageMesh::Sub sub;
    float factor = 1.0f, alpha = 1.0f;
    neutral.blend = it.blend;
    neutral.bias = it.bias;
    neutral.depth_cue = it.depth_cue;
    const int blend_index = draw_blend(neutral, sub, factor, alpha);
    const Pipeline* pl = pipeline(it.two_sided ? 2 : 0, blend_index, ps1_jitter().on);
    if (!pl) {
      i = j;
      continue;
    }
    SDL_BindGPUGraphicsPipeline(render_pass, pl->handle());
    bind_texture(*ctx_, render_pass, sets[static_cast<size_t>(it.texture)]);
    PushData pd;
    pd.clip = proj * cam_matrix;
    fill_push(pd, neutral, cue_, factor, alpha, it.depth_cue);
    push_uniforms(command_buffer, pd, view_depth_);
    SDL_DrawGPUIndexedPrimitives(render_pass, static_cast<u32>((j - i) * 6), 1, dyn_iused_ + static_cast<u32>(i * 6), static_cast<int32_t>(dyn_vused_), 0);
    i = j;
  }
  dyn_vused_ += static_cast<u32>(vv.size());
  dyn_iused_ += static_cast<u32>(ix.size());
  bind_vertex_buffer(render_pass, mesh.vertices.handle());
  bind_index_buffer(render_pass, mesh.indices.handle());
}

void StagePipeline::draw_dynamic(const Mat4& proj, const std::array<float, 12>& cam, std::span<const assets::StageVertex> vertices,
                                 std::span<const u32> indices, int blend, int bias) {
  const Pipeline* pl = pipeline(2, blend, ps1_jitter().on);
  if (!active_ || !pl || vertices.empty() || indices.empty()) return;
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  if (dyn_vused_ + vertices.size() > kDynVertices || dyn_iused_ + indices.size() > kDynIndices) {
    log::warn("stage: dynamic geometry overflow ({} + {} vertices, {} + {} indices)", dyn_vused_, vertices.size(), dyn_iused_, indices.size());
    return;
  }
  write_vertices(vertices.data(), static_cast<u32>(vertices.size()), dyn_vused_);
  write_indices(indices.data(), static_cast<u32>(indices.size()), dyn_iused_);
  SDL_BindGPUGraphicsPipeline(render_pass, pl->handle());
  bind_texture(*ctx_, render_pass, dyn_white_set_);
  bind_vertex_buffer(render_pass, dyn_vertices_.handle());
  bind_index_buffer(render_pass, dyn_indices_.handle());
  Mat4 cam_matrix;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) cam_matrix.at(row, col) = cam[static_cast<size_t>(row * 3 + col)];
    cam_matrix.at(row, 3) = cam[static_cast<size_t>(9 + row)];
  }
  PushData pd;
  pd.clip = proj * cam_matrix;
  pd.fogc[0] = pd.fogc[1] = pd.fogc[2] = pd.fogc[3] = 0.0f;
  pd.color[0] = pd.color[1] = pd.color[2] = 1.0f;
  pd.color[3] = 1.0f;
  pd.params[0] = static_cast<float>(bias);
  // Per-fragment depth: the hulls are small and lie on the floor they are biased against.
  pd.params[1] = 2.0f;
  pd.params[2] = 1.0f;
  pd.params[3] = 0.0f;
  pd.uvw[0] = pd.uvw[1] = pd.uvw[2] = pd.uvw[3] = 0.0f;
  push_uniforms(command_buffer, pd, view_depth_);
  SDL_DrawGPUIndexedPrimitives(render_pass, static_cast<u32>(indices.size()), 1, dyn_iused_, static_cast<int32_t>(dyn_vused_), 0);
  dyn_vused_ += static_cast<u32>(vertices.size());
  dyn_iused_ += static_cast<u32>(indices.size());
}

void StagePipeline::end() {
  if (!active_) return;
  ctx_->set_scissor(Rect{0, 0, extent_.width, extent_.height});
  active_ = false;
}

}  // namespace oc::render
