#include "render/mesh_pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "core/log.h"
#include "render/camera.h"
#include "render/model_tiles.h"
#include "render/ps1_jitter.h"
#include "render/shaders.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

namespace oc::render {
// The fragment shader alpha-tests at 0.5 and the actors are sampled bilinearly: a transparent texel keeps its (usually white / black) colour and
// bleeds it into the opaque edge, which shows as pale slivers along cut-out borders (Glenn's arms). Give every fully transparent texel the mean
// colour of its opaque neighbours (two passes) so the filtered edge keeps the model colour. Alpha is untouched.
std::vector<u8> bleed_transparent(const std::vector<u8>& src, u32 w, u32 h) {
  std::vector<u8> out(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(src.size(), size_t(w) * h * 4)));
  if (out.size() < size_t(w) * h * 4) return out;
  std::vector<u8> known(size_t(w) * h);
  // Only the transparent texels are visited; a pass reads the previous pass's colours and applies its fills at the end.
  std::vector<size_t> unknown;
  for (size_t i = 0; i < known.size(); ++i) {
    known[i] = out[i * 4 + 3] != 0;
    if (!known[i]) unknown.push_back(i);
  }
  struct Fill {
    size_t index;
    u8 rgb[3];
  };
  std::vector<Fill> fills;
  for (int pass = 0; pass < 2 && !unknown.empty(); ++pass) {
    fills.clear();
    for (size_t i : unknown) {
      const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
      int neighbours = 0, r = 0, g = 0, b = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= int(w) || ny >= int(h)) continue;
          const size_t j = size_t(ny) * w + size_t(nx);
          if (!known[j]) continue;
          r += out[j * 4], g += out[j * 4 + 1], b += out[j * 4 + 2];
          ++neighbours;
        }
      if (neighbours == 0) continue;
      fills.push_back({i, {static_cast<u8>(r / neighbours), static_cast<u8>(g / neighbours), static_cast<u8>(b / neighbours)}});
    }
    for (const Fill& fill : fills) {
      std::memcpy(&out[fill.index * 4], fill.rgb, 3);
      known[fill.index] = 1;
    }
    std::erase_if(unknown, [&](size_t i) { return known[i] != 0; });
  }
  return out;
}

namespace {

#include "mesh.vert.inc"
#include "mesh.frag.inc"

struct FrameUniforms {
  Mat4 view_proj, view, model;
  // std430 vec4 after the three mat4s.
  float slab[4];
  // DrawParams::Light: back colour (+ on flag), 3 light matrix rows, 3 light colours, specular light dir (+ on), eye.
  float lit[9][4];
  // [Enhancements] EnhancedLighting / SpellLights (see the vertex shader).
  float el[18][4];
  Mat4 palette[MeshPipeline::kMaxJoints];
};
static_assert(offsetof(FrameUniforms, palette) == 3 * sizeof(Mat4) + 16 + 9 * 16 + 18 * 16);
// Lighting key light: colour multiplier swing = +-0.35 (key strength * dot(n, key) * 0.5).
constexpr float kKeyStrength = 0.70f;

// 8 x vec4 of draw state followed by the slot number in x of a ninth vec4.
constexpr u32 kPushFloats = 36;

// Faces meeting at a corner at a larger angle than this keep their own normal there (a hard edge).
constexpr float kHardEdgeDegrees = 60.0f;

// The smoothed normal of every corner of the rigid meshes' unindexed triangles (SkinnedVertex::smooth). `source[i]` = the vertex of
// `rigid_vertex` that flat corner i came from. Corners match by their model-space position.
void smooth_rigid_normals(std::vector<SkinnedVertex>& flat, const std::vector<u8>& rigid_vertex, const std::vector<u32>& source) {
  using Key = std::array<float, 3>;
  const size_t tris = flat.size() / 3;
  std::vector<std::array<float, 3>> face(tris);
  std::map<Key, std::vector<size_t>> at;
  for (size_t tri = 0; tri < tris; ++tri) {
    if (!rigid_vertex[source[tri * 3]]) continue;
    const float* p0 = flat[tri * 3].pos[0][0];
    const float* p1 = flat[tri * 3 + 1].pos[0][0];
    const float* p2 = flat[tri * 3 + 2].pos[0][0];
    const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    face[tri] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    for (size_t k = 0; k < 3; ++k) {
      const float* p = flat[tri * 3 + k].pos[0][0];
      at[Key{p[0], p[1], p[2]}].push_back(tri);
    }
  }
  auto unit = [](const std::array<float, 3>& v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return length > 0.0f ? std::array<float, 3>{v[0] / length, v[1] / length, v[2] / length} : std::array<float, 3>{0, 0, 0};
  };
  const float min_cos = std::cos(kHardEdgeDegrees * 3.14159265f / 180.0f);
  for (size_t tri = 0; tri < tris; ++tri) {
    if (!rigid_vertex[source[tri * 3]]) continue;
    const std::array<float, 3> own = unit(face[tri]);
    if (own[0] == 0.0f && own[1] == 0.0f && own[2] == 0.0f) continue;
    for (size_t k = 0; k < 3; ++k) {
      SkinnedVertex& corner = flat[tri * 3 + k];
      const float* p = corner.pos[0][0];
      std::array<float, 3> sum{0, 0, 0};
      for (size_t other : at[Key{p[0], p[1], p[2]}]) {
        const std::array<float, 3> n = unit(face[other]);
        if (n[0] * own[0] + n[1] * own[1] + n[2] * own[2] < min_cos) continue;
        for (size_t c = 0; c < 3; ++c) sum[c] += face[other][c];
      }
      const std::array<float, 3> n = unit(sum);
      corner.smooth[0] = n[0], corner.smooth[1] = n[1], corner.smooth[2] = n[2], corner.smooth[3] = 1.0f;
    }
  }
}

}  // namespace

bool MeshPipeline::init(GpuContext& ctx) {
  destroy();
  ctx_ = &ctx;

  vs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_VERTEX, mesh_vert, ShaderDesc{0, 1, 1}, "mesh.vert");
  fs_ = create_shader(ctx, SDL_GPU_SHADERSTAGE_FRAGMENT, mesh_frag, ShaderDesc{1, 0, 1}, "mesh.frag");
  if (!vs_ || !fs_) return false;
  // Debug override OC_MESH_CULL: 0 off, -1 inverted.
  if (const char* cull_env = std::getenv("OC_MESH_CULL")) cull_ = cull_env[0] == '0' ? 0.0f : cull_env[0] == '-' ? -1.0f : 1.0f;
  if (!nearest_.create(ctx, false) || !linear_.create(ctx, true)) return false;

  // 16 attributes: 8 joint-local positions, the joints and weights of the 4 corners packed by two, uv + quad info,
  // the VDDM normal + its joint (lit path), the corner colour, the smoothed normal.
  VertexAttribute attributes[16];
  for (u32 corner = 0; corner < 4; ++corner)
    for (u32 i = 0; i < 2; ++i)
      attributes[corner * 2 + i] = {corner * 2 + i, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
                                    static_cast<u32>(offsetof(SkinnedVertex, pos) + (corner * 2 + i) * 3 * sizeof(float))};
  attributes[8] = {8, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, static_cast<u32>(offsetof(SkinnedVertex, joint))};
  attributes[9] = {9, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, static_cast<u32>(offsetof(SkinnedVertex, joint) + 4 * sizeof(u16))};
  attributes[10] = {10, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, weight))};
  attributes[11] = {11, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, weight) + 4 * sizeof(float))};
  attributes[12] = {12, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, uv))};
  attributes[13] = {13, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, normal))};
  attributes[14] = {14, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, rgb))};
  attributes[15] = {15, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, static_cast<u32>(offsetof(SkinnedVertex, smooth))};
  static_assert(offsetof(SkinnedVertex, quad) == offsetof(SkinnedVertex, uv) + 2 * sizeof(float) &&
                offsetof(SkinnedVertex, corner) == offsetof(SkinnedVertex, uv) + 3 * sizeof(float));
  PipelineDesc pd;
  pd.vertex = vs_;
  pd.fragment = fs_;
  pd.vertex_stride = sizeof(SkinnedVertex);
  pd.attributes = attributes;
  pd.cull = SDL_GPU_CULLMODE_NONE;
  pd.blend = Blend::None;
  pd.depth_test = true;
  pd.depth_write = true;
  if (!pipeline_.create(ctx, pd)) return false;
  pd.depth_write = false;
  pd.blend = Blend::Additive;
  if (!pipeline_add_.create(ctx, pd)) return false;
  pd.blend = Blend::Subtract;
  if (!pipeline_sub_.create(ctx, pd)) return false;
  // ABR 0: alpha blend with output alpha 0.5.
  pd.blend = Blend::Alpha;
  if (!pipeline_avg_.create(ctx, pd)) return false;
  // Depth-only: alpha blend of an alpha-0 output, so the frame keeps its colour and only the depth is written.
  pd.depth_write = true;
  pd.blend = Blend::Alpha;
  if (!pipeline_depth_only_.create(ctx, pd)) return false;

  return slots_.create(ctx, sizeof(FrameUniforms) * kMaxDrawsPerFrame, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
}

void MeshPipeline::destroy() {
  slots_.destroy();
  pipeline_.destroy();
  pipeline_add_.destroy();
  pipeline_sub_.destroy();
  pipeline_avg_.destroy();
  pipeline_depth_only_.destroy();
  nearest_.destroy();
  linear_.destroy();
  if (ctx_) {
    if (vs_) SDL_ReleaseGPUShader(ctx_->device(), vs_);
    if (fs_) SDL_ReleaseGPUShader(ctx_->device(), fs_);
  }
  vs_ = fs_ = nullptr;
  slots_memory_ = nullptr;
  active_ = false;
  ctx_ = nullptr;
}

void resolve_model_textures(assets::ModelAsset& asset) {
  if (asset.source == "@room_model") {
    if (const vfs::Vfs* vfs = texture_registry::bound_vfs()) resolve_room_model_textures(asset, *vfs);
    return;
  }
  if (asset.source.empty() || !texture_registry::active()) return;
  for (size_t i = 0; i < asset.textures.size(); ++i) {
    const import::Image& img = asset.textures[i];
    if (img.indexed || !img.width || !img.height || img.pixels.size() < size_t(img.width) * img.height * 4) continue;
    const import::Image& best = asset.has_hd(i) && !asset.hd_textures[i].indexed ? asset.hd_textures[i] : img;
    const TextureKey key = model_texture_key(asset.source, i, asset.main_texture_count, img.width, img.height, img.pixels.data());
    if (texture_registry::settings().dump) texture_registry::dump(key, best.width, best.height, model_file_pixels(best.width, best.height, best.pixels.data()).data());
    import::Image mod;
    mod.width = img.width, mod.height = img.height;
    mod.pixels.assign(img.pixels.begin(), img.pixels.begin() + static_cast<std::ptrdiff_t>(size_t(img.width) * img.height * 4));
    if (!texture_registry::replace(key, mod.width, mod.height, mod.pixels)) continue;
    model_restore_stp(best.width, best.height, best.pixels.data(), mod.width, mod.height, mod.pixels);
    asset.mod_textures.resize(asset.textures.size());
    asset.mod_textures[i] = std::move(mod);
  }
}

namespace {

// The 4x image of texture i, uploaded only where no mod replaces the texture (assets::ModelAsset::drawn_texture).
const import::Image* hd_image(const assets::ModelAsset& asset, size_t i) {
  if (asset.has_mod(i) || !asset.has_hd(i)) return nullptr;
  const import::Image& hd = asset.hd_textures[i];
  return !hd.indexed && hd.pixels.size() >= size_t(hd.width) * hd.height * 4 ? &hd : nullptr;
}

}  // namespace

ModelUploads prepare_model_uploads(const assets::ModelAsset& asset) {
  ModelUploads out;
  out.main.resize(asset.textures.size());
  out.hd.resize(asset.textures.size());
  for (size_t i = 0; i < asset.textures.size(); ++i) {
    const import::Image& img = asset.textures[i];
    if (img.width == 0 || img.height == 0 || img.indexed) continue;
    const import::Image& drawn = asset.drawn_texture(i, false);
    out.main[i] = bleed_transparent(drawn.pixels, drawn.width, drawn.height);
    if (const import::Image* hd = hd_image(asset, i)) out.hd[i] = bleed_transparent(hd->pixels, hd->width, hd->height);
  }
  if (asset.eye.valid && !asset.textures.empty()) {
    const import::Image& drawn = asset.drawn_texture(0, false);
    const import::Image* hd = hd_image(asset, 0);
    for (int frame_number = 1; frame_number <= 2; ++frame_number) {
      const std::vector<u8> frame = asset.drawn_eye_frame(frame_number, false);
      out.eye.push_back(frame.empty() ? frame : bleed_transparent(frame, drawn.width, drawn.height));
      const std::vector<u8> hd_frame = hd ? asset.drawn_eye_frame(frame_number, true) : std::vector<u8>{};
      out.hd_eye.push_back(hd_frame.empty() ? hd_frame : bleed_transparent(hd_frame, hd->width, hd->height));
    }
  }
  return out;
}

std::unique_ptr<MeshModel> MeshPipeline::create_model(const assets::ModelAsset& asset, const ModelUploads* uploads) {
  const ModelUploads made = uploads ? ModelUploads{} : prepare_model_uploads(asset);
  const ModelUploads& up = uploads ? *uploads : made;
  auto model = std::make_unique<MeshModel>();
  model->joint_count = std::min<u32>(static_cast<u32>(asset.skeleton.joints.size()), kMaxJoints);

  // Textures (RGBA8) with one nearest and one linear sampling reference each (plus the optional 4x variants).
  auto make_sets = [&](const Texture& tex, std::vector<TexRef>& nearest, std::vector<TexRef>& linear_sets) {
    nearest.push_back(TexRef(tex, nearest_));
    linear_sets.push_back(TexRef(tex, linear_));
  };
  model->textures.resize(asset.textures.size());
  for (auto& texture : model->textures) texture = std::make_unique<Texture>();
  model->hd_textures.resize(asset.textures.size());
  for (size_t i = 0; i < asset.textures.size(); ++i) {
    const import::Image& img = asset.textures[i];
    if (img.width == 0 || img.height == 0 || img.indexed) {
      log::warn("mesh: texture {} is not RGBA, skipped", i);
      model->sets_nearest.push_back(TexRef());
      model->sets_linear.push_back(TexRef());
      model->hd_sets_nearest.push_back(TexRef());
      model->hd_sets_linear.push_back(TexRef());
      continue;
    }
    const import::Image& drawn = asset.drawn_texture(i, false);
    if (!model->textures[i]->create(*ctx_, drawn.width, drawn.height, TextureFormat::Rgba8, up.main[i].data())) return nullptr;
    make_sets(*model->textures[i], model->sets_nearest, model->sets_linear);
    if (const import::Image* hd = hd_image(asset, i)) {
      model->hd_textures[i] = std::make_unique<Texture>();
      if (!model->hd_textures[i]->create(*ctx_, hd->width, hd->height, TextureFormat::Rgba8, up.hd[i].data())) return nullptr;
      make_sets(*model->hd_textures[i], model->hd_sets_nearest, model->hd_sets_linear);
    } else {
      model->hd_sets_nearest.push_back(TexRef());
      model->hd_sets_linear.push_back(TexRef());
    }
  }

  // Eye-blink variants (frames 1, 2 of the .ltc strip pasted over the eye rects of texture 0): extra textures after the asset's own, so the
  // draw of texture 0 can swap to them without a texture upload (the PS1 copies the strip into VRAM).
  if (asset.eye.valid && !asset.textures.empty() && model->textures[0]) {
    const size_t base_index = model->textures.size();
    const import::Image& drawn = asset.drawn_texture(0, false);
    const import::Image* hd = hd_image(asset, 0);
    if (up.eye.size() == 2 && !up.eye[0].empty() && !up.eye[1].empty()) {
      for (size_t frame_index = 0; frame_index < 2; ++frame_index) {
        auto tex = std::make_unique<Texture>();
        if (!tex->create(*ctx_, drawn.width, drawn.height, TextureFormat::Rgba8, up.eye[frame_index].data())) return nullptr;
        make_sets(*tex, model->sets_nearest, model->sets_linear);
        model->textures.push_back(std::move(tex));
        std::unique_ptr<Texture> hd_tex;
        if (hd && !up.hd_eye[frame_index].empty()) {
          hd_tex = std::make_unique<Texture>();
          if (!hd_tex->create(*ctx_, hd->width, hd->height, TextureFormat::Rgba8, up.hd_eye[frame_index].data())) return nullptr;
          make_sets(*hd_tex, model->hd_sets_nearest, model->hd_sets_linear);
        } else {
          model->hd_sets_nearest.push_back(TexRef());
          model->hd_sets_linear.push_back(TexRef());
        }
        model->hd_textures.push_back(std::move(hd_tex));
      }
      model->eye_base = static_cast<int>(base_index);
    }
  }

  // Concatenate all meshes into one vertex and one index buffer (corner 0 of each vertex only; the rest is filled when unindexing).
  std::vector<SkinnedVertex> verts;
  std::vector<u32> idx;
  // Per vertex of verts: it belongs to a rigid mesh.
  std::vector<u8> rigid_vertex;
  // Per triangle of idx: assets::Mesh::tri_quad.
  std::vector<u8> tri_quad;
  const u16 max_joint = static_cast<u16>(kMaxJoints - 1);
  for (size_t mesh_index = 0; mesh_index < asset.meshes.size(); ++mesh_index) {
    const assets::Mesh& mesh = asset.meshes[mesh_index];
    const u32 base = static_cast<u32>(verts.size());
    for (const assets::MeshVertex& mv : mesh.vertices) {
      const assets::SkinVertex& sv = mesh.pool[mv.pool];
      SkinnedVertex vertex{};
      const bool rigid = !mesh.skinned;
      for (size_t k = 0; k < 3; ++k) {
        vertex.pos[0][0][k] = rigid ? sv.bind_pos[k] : sv.local[0][k];
        vertex.pos[0][1][k] = rigid ? 0.0f : sv.local[1][k];
      }
      vertex.uv[0] = mv.uv[0];
      vertex.uv[1] = mv.uv[1];
      for (size_t k = 0; k < 3; ++k) vertex.rgb[k] = static_cast<float>(mv.rgb[k]) / 128.0f;
      vertex.joint[0][0] = rigid ? u16{0} : std::min(sv.joint[0], max_joint);
      vertex.joint[0][1] = rigid ? u16{0} : std::min(sv.joint[1], max_joint);
      vertex.weight[0][0] = rigid ? 1.0f : sv.weight[0];
      vertex.weight[0][1] = rigid ? 0.0f : sv.weight[1];
      // VDDM normal: n << 5 in 4.12 = n / 128; rotated by the vertex's own joint, or J2 for a 16-byte vertex. A rigid mesh's (weapon meshes:
      // built at import) turns with joint 0.
      for (size_t k = 0; k < 3; ++k) vertex.normal[k] = static_cast<float>(sv.normal[k]) / 128.0f;
      vertex.normal[3] = static_cast<float>(vertex.joint[0][sv.normal_joint & 1]);
      verts.push_back(vertex);
      rigid_vertex.push_back(rigid ? u8{1} : u8{0});
    }
    const u32 first_of_mesh = static_cast<u32>(idx.size());
    for (u32 i : mesh.indices) idx.push_back(base + i);
    const size_t tris = mesh.indices.size() / 3;
    for (size_t tri = 0; tri < tris; ++tri) tri_quad.push_back(mesh.tri_quad.size() == tris ? mesh.tri_quad[tri] : u8{0});
    for (const assets::SubMesh& sm : mesh.submeshes) {
      MeshModel::Draw model_draw;
      model_draw.first_index = first_of_mesh + sm.first_index;
      model_draw.index_count = sm.index_count;
      model_draw.trans = mesh.trans;
      model_draw.mesh = static_cast<u16>(mesh_index);
      // Mdl constructs and rigid weapon meshes alike are one-sided.
      model_draw.one_sided = true;
      const assets::Material& mat = mesh.materials[sm.material];
      model_draw.texture = mat.texture;
      if (model_draw.texture >= static_cast<int>(model->sets_nearest.size())) model_draw.texture = -1;
      for (size_t channel = 0; channel < 3; ++channel) model_draw.color[channel] = static_cast<float>(mat.color[channel]) / 255.0f;
      model->draws.push_back(model_draw);
    }
  }
  if (verts.empty() || idx.empty()) {
    log::warn("mesh: model has no geometry");
    return nullptr;
  }
  // Unindexed triangles: every corner carries the skinning inputs of its triangle's other two corners and, for a quad half, of the quad's
  // fourth corner (per-face back-face test, ordering-table slot and water test in the vertex shader). The index buffer becomes 0..n-1, so
  // the Draw ranges stay valid. Quad halves are consecutive: (v0,v1,v2) then (v2,v1,v3); the partner of the first is v3 (the second
  // triangle's corner 2), of the second v0 (the first triangle's corner 0).
  {
    auto copy_corner = [](SkinnedVertex& dst, size_t corner, const SkinnedVertex& src) {
      std::memcpy(dst.pos[corner], src.pos[0], sizeof dst.pos[corner]);
      std::memcpy(dst.joint[corner], src.joint[0], sizeof dst.joint[corner]);
      std::memcpy(dst.weight[corner], src.weight[0], sizeof dst.weight[corner]);
    };
    const size_t flat_count = idx.size() - idx.size() % 3;
    std::vector<SkinnedVertex> flat(flat_count);
    for (size_t i = 0; i < flat_count; ++i) {
      const size_t tri_start = i - i % 3, corner_in_tri = i % 3;
      SkinnedVertex vertex = verts[idx[i]];
      copy_corner(vertex, 1, verts[idx[tri_start + (corner_in_tri + 1) % 3]]);
      copy_corner(vertex, 2, verts[idx[tri_start + (corner_in_tri + 2) % 3]]);
      u8 role = tri_quad[tri_start / 3];
      // Unpaired: a plain triangle.
      if (role == 1 && (tri_start / 3 + 1 >= tri_quad.size() || tri_quad[tri_start / 3 + 1] != 2)) role = 0;
      if (role == 2 && (tri_start < 3 || tri_quad[tri_start / 3 - 1] != 1)) role = 0;
      if (role == 1) copy_corner(vertex, 3, verts[idx[tri_start + 5]]);
      else if (role == 2) copy_corner(vertex, 3, verts[idx[tri_start - 3]]);
      else copy_corner(vertex, 3, vertex);
      vertex.quad = static_cast<float>(role);
      vertex.corner = static_cast<float>(corner_in_tri);
      flat[i] = vertex;
    }
    smooth_rigid_normals(flat, rigid_vertex, idx);
    verts.swap(flat);
    idx.resize(flat_count);
    for (size_t i = 0; i < flat_count; ++i) idx[i] = static_cast<u32>(i);
  }
  if (!model->vertices.create(*ctx_, verts.data(), static_cast<u32>(verts.size() * sizeof(SkinnedVertex)), SDL_GPU_BUFFERUSAGE_VERTEX) ||
      !model->indices.create(*ctx_, idx.data(), static_cast<u32>(idx.size() * sizeof(u32)), SDL_GPU_BUFFERUSAGE_INDEX))
    return nullptr;
  return model;
}

void MeshPipeline::begin(const Frame& frame, Rect scissor) {
  extent_ = frame.extent;
  // The uniform slots are per frame, not per begin(): a second pass of the same frame (the submerged faces before the layers, the rest after
  // them) must not overwrite the first pass's slots, which the GPU reads only when the command buffer runs (the hydra's submerged faces were
  // drawn with the first dry actor's matrix and skeleton).
  if (frame.serial != serial_ || frame.serial == 0) slot_ = 0;
  serial_ = frame.serial;
  slots_memory_ = slots_.begin(frame);
  active_ = true;
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_BindGPUGraphicsPipeline(render_pass, pipeline_.handle());
  SDL_GPUBuffer* slots_buffer = slots_.handle();
  SDL_BindGPUVertexStorageBuffers(render_pass, 0, &slots_buffer, 1);
  ctx_->set_scissor(scissor);
}

void MeshPipeline::draw(const MeshModel& model, const DrawParams& params) {
  if (!active_) return;
  if (slot_ >= kMaxDrawsPerFrame) {
    log::warn("MeshPipeline: more than {} draws in a frame", kMaxDrawsPerFrame);
    return;
  }
  const u32 slot = slot_++;
  auto* uniforms = reinterpret_cast<FrameUniforms*>(static_cast<u8*>(slots_memory_) + slot * sizeof(FrameUniforms));
  uniforms->view_proj = params.view_proj;
  uniforms->view = params.view;
  uniforms->model = params.model;
  uniforms->slab[0] = params.slot_min;
  uniforms->slab[1] = params.slot_max;
  uniforms->slab[2] = uniforms->slab[3] = 0.0f;
  if (ps1_jitter().on) uniforms->slab[2] = ps1_jitter().px, uniforms->slab[3] = ps1_jitter().py;
  std::memset(uniforms->lit, 0, sizeof uniforms->lit);
  // PS1 lit path (DrawParams::Light): BK, the light matrix rows, the colour matrix columns.
  if (params.light.on) {
    for (size_t channel = 0; channel < 3; ++channel) uniforms->lit[0][channel] = params.light.bk[channel];
    uniforms->lit[0][3] = 1.0f;
    for (size_t i = 0; i < 3; ++i)
      for (size_t channel = 0; channel < 3; ++channel) {
        uniforms->lit[1 + i][channel] = params.light.dir[i][channel];
        uniforms->lit[4 + i][channel] = params.light.rgb[i][channel];
      }
  }
  std::memset(uniforms->el, 0, sizeof uniforms->el);
  // [Enhancements] SmoothReflections (mesh.vert env UVs).
  uniforms->el[0][3] = smooth_reflections_ ? 1.0f : 0.0f;
  const LightSet* light_set = params.lights && params.lights->active() ? params.lights : nullptr;
  // [Enhancements] Lighting.
  if (enhanced_ || light_set) {
    if (light_set && light_set->ambient_on()) {
      for (size_t channel = 0; channel < 3; ++channel) uniforms->el[17][channel] = light_set->ambient[channel];
      uniforms->el[17][3] = 1.0f;
    }
    uniforms->el[0][0] = enhanced_ && !params.solid ? 1.0f : 0.0f;
    uniforms->el[0][1] = kKeyStrength;
    uniforms->el[0][2] = light_set ? static_cast<float>(light_set->count) : 0.0f;
    for (int i = 0; light_set && i < light_set->count; ++i) {
      const PointLight& light = light_set->light[static_cast<size_t>(i)];
      for (size_t channel = 0; channel < 3; ++channel) {
        uniforms->el[1 + 2 * i][channel] = light.pos[channel];
        uniforms->el[2 + 2 * i][channel] = light.rgb[channel];
      }
      uniforms->el[2 + 2 * i][3] = light.linear ? 1.0f : 0.0f;
      uniforms->el[1 + 2 * i][3] = light.radius;
    }
  }
  // Weapon specular: light direction + on flag, eye position.
  if (params.light.spec) {
    for (size_t channel = 0; channel < 3; ++channel) {
      uniforms->lit[7][channel] = params.light.spec_dir[channel];
      uniforms->lit[8][channel] = params.light.spec_eye[channel];
    }
    uniforms->lit[7][3] = 1.0f;
  }
  const size_t joint_count = std::min<size_t>(params.palette.size(), kMaxJoints);
  if (joint_count) std::memcpy(uniforms->palette, params.palette.data(), joint_count * sizeof(Mat4));
  for (size_t i = joint_count; i < kMaxJoints; ++i) uniforms->palette[i] = Mat4{};

  slots_.mark_used((slot + 1) * sizeof(FrameUniforms));
  SDL_GPURenderPass* render_pass = ctx_->pass();
  SDL_GPUCommandBuffer* command_buffer = ctx_->cmd();
  const SDL_GPUBufferBinding vertex_binding{model.vertices.handle(), 0};
  SDL_BindGPUVertexBuffers(render_pass, 0, &vertex_binding, 1);
  const SDL_GPUBufferBinding index_binding{model.indices.handle(), 0};
  SDL_BindGPUIndexBuffer(render_pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  const auto& sets = params.linear ? model.sets_linear : model.sets_nearest;
  const auto& hd_sets = params.linear ? model.hd_sets_linear : model.hd_sets_nearest;
  // Flat-colour faces still need some texture bound.
  TexRef fallback;
  for (const TexRef& ref : sets)
    if (ref) {
      fallback = ref;
      break;
    }
  // Passes: Solid (every visible mesh; a semi-transparent mesh only with its texels without STP, which the PS1 draws opaque), Semi (the
  // semi-transparent meshes' STP texels with the construct's ABR), and in field ordering-table mode with water (WetFaces::Mixed) DepthOnly +
  // WetAdd for the submerged faces: their pushed-back depth first, then added to the frame where they are the nearest thing (PS1: drawn far
  // back first, the semi-transparent water layer over them). DryOnly / WetOnly draw one side of that split with Solid + Semi.
  enum class Kind { Solid, Semi, DepthOnly, WetAdd };
  struct Pass {
    Kind kind;
    // ot2.x face filter: 0 all, 1 dry only, 2 submerged only.
    float faces;
    // FE 45: the white CLUT.
    bool white = false;
    // Semi: this ABR for every construct (AC model ABR / FE 45 routine), -1 = the construct's own.
    int abr = -1;
    // The second, env-mapped primitive of the flag-0x40 parts (DrawParams::stock_env_over): only those parts, the env page.
    bool env = false;
  };
  const bool wet = params.water_level < 1e8f;
  const bool split_wet = params.ot_faces && wet;
  using WF = DrawParams::WetFaces;
  if (params.wet_faces == WF::WetOnly && !split_wet) return;
  // AC: every construct semi-transparent with the model's ABR. FE 45 (routine 4): every construct twice, white with ABR 2 then normal with
  // ABR 1 (the AC flag does not matter there: the routine sets the code and the ABR itself).
  // The flash routine applies only to an actor off the water triangles (the water routine wins there).
  const bool flash = params.flash && !split_wet;
  const bool all_semi = flash || (params.model_abr >= 0 && params.model_abr <= 3);
  const int model_abr = flash ? 1 : all_semi ? params.model_abr : -1;
  Pass passes[8];
  int npass = 0;
  if (params.solid) {
    passes[npass++] = {Kind::Solid, 0.0f};
  } else if (!split_wet) {
    if (flash) {
      // The PS1 draws each polygon white (B - F) and then normal (B + F) before the next one, back to front, so a nearer polygon's
      // subtraction removes what the farther ones added. The depth of the nearest blended surface first: both blended passes then touch
      // only that surface (max(B - light, 0) + texel * light per pixel) instead of summing every layer of the model.
      passes[npass++] = {Kind::DepthOnly, 0.0f};
      passes[npass++] = {Kind::Solid, 0.0f, true};
      passes[npass++] = {Kind::Semi, 0.0f, true, 2};
    }
    passes[npass++] = {Kind::Solid, 0.0f};
    passes[npass++] = {Kind::Semi, 0.0f, false, model_abr};
    // Drawn after the model primitive (the PS1 files it one OT slot nearer).
    if (params.stock_env_over && params.stock_env_set) {
      passes[npass++] = {Kind::Solid, 0.0f, false, -1, true};
      passes[npass++] = {Kind::Semi, 0.0f, false, -1, true};
    }
  } else if (params.wet_faces == WF::Mixed) {
    passes[npass++] = {Kind::Solid, 1.0f};
    passes[npass++] = {Kind::Semi, 0.0f, false, model_abr};
    passes[npass++] = {Kind::DepthOnly, 2.0f};
    passes[npass++] = {Kind::WetAdd, 2.0f};
  } else {
    const float faces = params.wet_faces == WF::DryOnly ? 1.0f : 2.0f;
    passes[npass++] = {Kind::Solid, faces};
    passes[npass++] = {Kind::Semi, faces, false, model_abr};
  }
  SDL_GPUGraphicsPipeline* bound = pipeline_.handle();
  for (int pi = 0; pi < npass; ++pi) {
    const Pass& pass = passes[pi];
    for (const MeshModel::Draw& model_draw : model.draws) {
      if (model_draw.trans == 0xFF) continue;
      // Hidden by the shading entry's flag bit 0.
      if (model_draw.mesh < 32 && ((params.hidden_meshes >> model_draw.mesh) & 1u)) continue;
      const bool part_semi = model_draw.mesh < 32 && ((params.part_semi >> model_draw.mesh) & 1u) != 0;
      const bool env_over = model_draw.mesh < 32 && ((params.stock_env_over >> model_draw.mesh) & 1u) && static_cast<bool>(params.stock_env_set);
      if (pass.env && !env_over) continue;
      const bool semi_mesh = !params.solid && (pass.env || all_semi || part_semi || (model_draw.trans >= 1 && model_draw.trans <= 4));
      if (pass.kind == Kind::Semi && !semi_mesh) continue;
      const Pipeline* pipe = &pipeline_;
      float blend_scale = 1.0f, out_alpha = 1.0f;
      if (pass.kind == Kind::DepthOnly) pipe = &pipeline_depth_only_;
      else if (pass.kind == Kind::WetAdd) pipe = &pipeline_add_;
      else if (pass.kind == Kind::Semi) {
        // PS1 ABR: 0 = (B + F) / 2, 1 = B + F, 2 = B - F, 3 = B + F / 4.
        switch (pass.env ? params.stock_env_abr[model_draw.mesh] : pass.abr >= 0 ? pass.abr : part_semi ? params.part_abr[model_draw.mesh] : model_draw.trans - 1) {
          case 0: pipe = &pipeline_avg_; out_alpha = 0.5f; break;
          case 2: pipe = &pipeline_sub_; break;
          case 3: pipe = &pipeline_add_; blend_scale = 0.25f; break;
          default: pipe = &pipeline_add_; break;
        }
      }
      if (pipe->handle() != bound) {
        bound = pipe->handle();
        SDL_BindGPUGraphicsPipeline(render_pass, bound);
      }
      // STP texel filter: the Semi pass blends the STP texels only (the flash routine's depth pass covers the same texels); every other pass
      // draws a semi-transparent mesh's non-STP texels.
      const float stp_filter = pass.kind == Kind::Semi || (pass.kind == Kind::DepthOnly && flash) ? 2.0f : semi_mesh ? 1.0f : 0.0f;
      const int ti = (params.eye_frame >= 1 && params.eye_frame <= 2 && model_draw.texture == 0 && model.eye_base >= 0) ? model.eye_base + params.eye_frame - 1 : model_draw.texture;
      TexRef tex = ti >= 0 ? sets[static_cast<size_t>(ti)] : fallback;
      if (params.hd && ti >= 0 && static_cast<size_t>(ti) < hd_sets.size() && hd_sets[static_cast<size_t>(ti)])
        tex = hd_sets[static_cast<size_t>(ti)];
      // env-mapped part: pc.fog.w carries the texture width (> 1.5); the vertex shader replaces the corner UVs (mesh.vert)
      const bool env = model_draw.mesh < 32 && ((params.env_meshes >> model_draw.mesh) & 1u) && ti >= 0 && static_cast<size_t>(ti) < model.textures.size() && model.textures[static_cast<size_t>(ti)];
      const float env_w = env ? (params.env_set ? 256.0f : static_cast<float>(model.textures[static_cast<size_t>(ti)]->width())) : 1.0f;
      float env_w2 = env_w;
      // kamei's env page override.
      if (env && params.env_set) tex = params.env_set;
      if (params.stock_env_set && model_draw.mesh < 32 && (pass.env || ((params.stock_env_replace >> model_draw.mesh) & 1u))) {
        // The stock env page, UVs unshifted (pc.fog.w < 0 = |w| texels wide, no half-page base).
        tex = params.stock_env_set;
        env_w2 = -256.0f;
      }
      if (!tex) continue;
      const SDL_GPUTextureSamplerBinding texture_binding{tex.texture, ctx_->filtered(tex.sampler)};
      SDL_BindGPUFragmentSamplers(render_pass, 0, &texture_binding, 1);
      const bool near_sort = model_draw.mesh < 32 && ((params.near_meshes >> model_draw.mesh) & 1u);
      float tr = params.tint[0], tg = params.tint[1], tb = params.tint[2];
      if (model_draw.mesh < 32 && ((params.part_mask >> model_draw.mesh) & 1u)) tr *= params.part_tint[model_draw.mesh][0], tg *= params.part_tint[model_draw.mesh][1], tb *= params.part_tint[model_draw.mesh][2];
      if (params.solid) tr = tg = tb = 1.0f;
      const float* flat_rgb = params.solid ? params.solid_rgb : model_draw.color;
      const float cull = model_draw.one_sided ? (params.mirror ? -cull_ : cull_) : 0.0f;
      const float push[kPushFloats] = {flat_rgb[0], flat_rgb[1], flat_rgb[2], cull, params.solid || model_draw.texture < 0 ? 1.0f : 0.0f,
                                       1.0f / (kActorDepthDivisor * kDepthRange), wet ? 1.0f : 0.0f, wet ? params.water_level : 0.0f,
                                       tr * blend_scale, tg * blend_scale, tb * blend_scale, blend_scale,
                                       params.water_tint[0], params.water_tint[1], params.water_tint[2], 1.0f,
                                       params.fog_color[0], params.fog_color[1], params.fog_color[2], env_w2,
                                       params.fog_near, params.fog_far, stp_filter, pass.white ? 1.0f : 0.0f,
                                       params.ot_faces ? (smooth_water_line_ ? 2.0f : 1.0f) : 0.0f, near_sort ? 1.0f : 0.0f, params.ot_bias, params.water_bias,
                                       pass.faces, pass.kind == Kind::DepthOnly ? 1.0f : 0.0f, 1.0f / kDepthRange, out_alpha,
                                       static_cast<float>(slot), 0.0f, 0.0f, 0.0f};
      SDL_PushGPUVertexUniformData(command_buffer, 0, push, sizeof push);
      SDL_PushGPUFragmentUniformData(command_buffer, 0, push, sizeof push);
      SDL_DrawGPUIndexedPrimitives(render_pass, model_draw.index_count, 1, model_draw.first_index, 0, 0);
    }
  }
  SDL_BindGPUGraphicsPipeline(render_pass, pipeline_.handle());
}

void MeshPipeline::end() {
  if (!active_) return;
  ctx_->set_scissor(Rect{0, 0, extent_.width, extent_.height});
  active_ = false;
}

}  // namespace oc::render
