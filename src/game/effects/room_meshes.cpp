#include "game/effects/room_meshes.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "import/tim.h"
#include "render/stage_tiles.h"
#include "render/texture_registry.h"

namespace oc::effects {

void RoomMeshes::clear() {
  sets_.clear();
  textures_.clear();
  parts_.clear();
  asset_ = assets::StageAsset{};
  sampler_.destroy();
}

void RoomMeshes::set(assets::StageAsset meshes, render::GpuContext* ctx, render::SpriteBatch* sprites) {
  clear();
  ctx_ = ctx;
  sprites_ = sprites;
  asset_ = std::move(meshes);
  parts_ = asset_.parts;
  tile_gen_ = render::texture_registry::generation();
  if (!ctx || !sprites || !sampler_.create(*ctx, false)) return;
  upload_textures();
}

// A baked asset (render::bake_object_textures) is drawn from its per-object images (StageSubMesh::object_texture); the atlases are not uploaded.
void RoomMeshes::upload_textures() const {
  const bool baked = !asset_.object_textures.empty();
  const size_t count = baked ? asset_.object_textures.size() : asset_.textures.size();
  for (size_t ti = 0; ti < count; ++ti) {
    import::Image img = baked ? asset_.object_textures[ti].image : asset_.textures[ti].image.indexed ? import::to_rgba(asset_.textures[ti].image) : asset_.textures[ti].image;
    auto tex = std::make_unique<render::Texture>();
    render::TexRef set;
    if (img.width && img.height && img.pixels.size() >= static_cast<size_t>(img.width) * img.height * 4u &&
        tex->create(*ctx_, img.width, img.height, render::TextureFormat::Rgba8, img.pixels.data()))
      set = render::TexRef(*tex, sampler_);
    textures_.push_back(std::move(tex));
    sets_.push_back(set);
  }
}

bool RoomMeshes::texture_rgba(int index, u32& width, u32& height, std::vector<u8>& rgba) const {
  const bool baked = !asset_.object_textures.empty();
  const size_t count = baked ? asset_.object_textures.size() : asset_.textures.size();
  if (index < 0 || static_cast<size_t>(index) >= count) return false;
  const size_t ti = static_cast<size_t>(index);
  import::Image img = baked ? asset_.object_textures[ti].image : asset_.textures[ti].image.indexed ? import::to_rgba(asset_.textures[ti].image) : asset_.textures[ti].image;
  if (!img.width || !img.height || img.pixels.size() < static_cast<size_t>(img.width) * img.height * 4u) return false;
  width = img.width, height = img.height;
  rgba = std::move(img.pixels);
  rgba.resize(static_cast<size_t>(width) * height * 4u);
  return true;
}

// The recompose rewrites the UVs of the parts and the object images, so the sets are re-made.
void RoomMeshes::refresh_tiles() const {
  tile_gen_ = render::texture_registry::generation();
  if (asset_.tiles.empty() || !ctx_ || !sprites_) return;
  render::apply_tile_textures(asset_, "room_mesh");
  parts_ = asset_.parts;
  sets_.clear();
  textures_.clear();
  upload_textures();
}

const assets::StagePart* RoomMeshes::find(const std::string& name) const {
  for (const assets::StagePart& part : parts_)
    if (part.name == name && !part.ground) return &part;
  return nullptr;
}

namespace {

// One corner of a triangle while it is cut along the texture-window wrap lines (op 36): screen position, colour, scrolled texel position,
// 1 / view depth (linear in screen space, so a cut corner's depth stays on the face).
struct WrapVert {
  float x, y, rgb[3], texel[2], inv_z;
};

WrapVert lerp(const WrapVert& from, const WrapVert& to, float frac) {
  WrapVert result;
  result.x = from.x + (to.x - from.x) * frac;
  result.y = from.y + (to.y - from.y) * frac;
  result.inv_z = from.inv_z + (to.inv_z - from.inv_z) * frac;
  for (int k = 0; k < 3; ++k) result.rgb[k] = from.rgb[k] + (to.rgb[k] - from.rgb[k]) * frac;
  for (int k = 0; k < 2; ++k) result.texel[k] = from.texel[k] + (to.texel[k] - from.texel[k]) * frac;
  return result;
}

// Sutherland-Hodgman against texel[axis] >= bound (keep_ge) or texel[axis] <= bound.
std::vector<WrapVert> clip_axis(const std::vector<WrapVert>& in, int axis, float bound, bool keep_ge) {
  std::vector<WrapVert> out;
  const size_t count = in.size();
  auto inside = [&](const WrapVert& vert) { return keep_ge ? vert.texel[axis] >= bound : vert.texel[axis] <= bound; };
  for (size_t i = 0; i < count; ++i) {
    const WrapVert& cur = in[i];
    const WrapVert& next = in[(i + 1) % count];
    const bool cur_in = inside(cur), next_in = inside(next);
    if (cur_in) out.push_back(cur);
    if (cur_in != next_in) out.push_back(lerp(cur, next, (bound - cur.texel[axis]) / (next.texel[axis] - cur.texel[axis])));
  }
  return out;
}

}  // namespace

// Per face: the asset's +y-up vertices go back to the file's +y-down ones, through the object matrix and the view-space centre, then the
// perspective of `view` and the letterbox. Faces with a corner in front of the near plane are dropped. The back-face test is the game's
// NCLIP on the packet's first three transformed corners (screen x right, y down): MAC0 = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0) must be > 0,
// or < 1 with ext bit 4, or anything with ext bit 5 (the ext bits exist only on the extended draw, op 35). A quad is one packet, so its
// second triangle (2,1,3) reuses the first one's verdict; the test runs on the morphed vertices, so a mid-morph face flips like retail's.
// One OT slot per packet: the packet centre's view depth >> 4 (the average corner depth when the asset has no centre) + bias, or the
// fixed slot. Op 37 morph: the vertices and packet centres are lerped toward the named mesh of the same .prd by the eased phase the
// emitter system steps per tick (interpolated by `alpha`); a target with a different vertex count cannot morph, the base mesh is drawn.
// Op 36 scroll: texel + phase wrapped inside the texture window like the GPU's; the wrap is per pixel, so the triangle is cut along the
// window cell lines (Sutherland-Hodgman) and every piece is shifted back into the window. An opaque object keeps the packets' own
// semi-transparency (tpage ABR + 1). Asset UVs are OBJ-style (v up) while the atlas rows are stored top-down, hence v = 1 - uv[1].
size_t RoomMeshes::project(const EmitterSystem::MeshInstance& mesh, const ViewParams& view, const render::Letterbox& letterbox, float alpha,
                           std::vector<RoomMeshTri>& out) const {
  if (!mesh.name) return 0;
  if (tile_gen_ != render::texture_registry::generation()) refresh_tiles();
  const assets::StagePart* part = find(*mesh.name);
  if (!part) return 0;
  const assets::StagePart* target = nullptr;
  float morph_t = 0;
  if (mesh.morph_name)
    if (const assets::StagePart* morph_part = find(*mesh.morph_name); morph_part && morph_part->vertices.size() == part->vertices.size()) {
      target = morph_part;
      morph_t = std::clamp(mesh.morph_t_prev + (mesh.morph_t - mesh.morph_t_prev) * alpha, 0.0f, 1.0f);
    }
  auto lerped = [&](const std::array<float, 3>& from, const std::array<float, 3>& to) {
    std::array<float, 3> result;
    for (size_t k = 0; k < 3; ++k) result[k] = from[k] + (to[k] - from[k]) * morph_t;
    return result;
  };
  auto pos_of = [&](u32 vi) { return target ? lerped(part->vertices[vi].pos, target->vertices[vi].pos) : part->vertices[vi].pos; };
  auto centre_of = [&](u32 vi) { return target ? lerped(part->vertices[vi].centre, target->vertices[vi].centre) : part->vertices[vi].centre; };
  auto view_of = [&](const std::array<float, 3>& pos, float& cx, float& cy, float& cz) {
    const Vec3 rotated = mesh.mat.apply({pos[0], -pos[1], pos[2]});
    cx = mesh.centre[0] + rotated[0];
    cy = mesh.centre[1] + rotated[1];
    cz = mesh.centre[2] + rotated[2];
  };
  const bool scroll = mesh.uv_scroll;
  const float phase[2] = {mesh.uv_phase[0], mesh.uv_phase[1]};
  const float off[2] = {mesh.uv_window[2] > 0 ? static_cast<float>(mesh.uv_window[0] & 255) : 0.0f,
                        mesh.uv_window[3] > 0 ? static_cast<float>(mesh.uv_window[1] & 255) : 0.0f};
  const float win[2] = {mesh.uv_window[2] > 0 ? static_cast<float>(mesh.uv_window[2]) : 256.0f,
                        mesh.uv_window[3] > 0 ? static_cast<float>(mesh.uv_window[3]) : 256.0f};
  // The shader keeps the bilinear footprint inside an explicit window only.
  const bool bound[2] = {mesh.uv_window[2] > 0, mesh.uv_window[3] > 0};

  const int base_blend = mesh.blend >= 5 ? 2 : mesh.blend;
  size_t added = 0;
  for (const assets::StageSubMesh& sm : part->submeshes) {
    int blend = base_blend;
    if (blend == 0 && sm.semi) blend = std::clamp(sm.abr, 0, 3) + 1;
    const int ti = sm.object_texture >= 0 ? sm.object_texture : sm.texture;
    const render::TexRef set = ti >= 0 && static_cast<size_t>(ti) < sets_.size() ? sets_[static_cast<size_t>(ti)] : render::TexRef{};
    // Textured but its upload failed; without a GPU the faces are still projected.
    if (ti >= 0 && !set && sprites_) continue;
    bool prev_valid = false, prev_facing = false;
    u32 prev_idx[3] = {};
    for (u32 k = 0; k + 2 < sm.index_count; k += 3) {
      const u32 idx[3] = {part->indices[sm.first_index + k], part->indices[sm.first_index + k + 1], part->indices[sm.first_index + k + 2]};
      RoomMeshTri tri;
      float cz_sum = 0;
      bool ok = true;
      for (int corner = 0; corner < 3 && ok; ++corner) {
        const assets::StageVertex& vertex = part->vertices[idx[corner]];
        float cx, cy, cz;
        view_of(pos_of(idx[corner]), cx, cy, cz);
        if (cz < 1.0f) {
          ok = false;
          break;
        }
        tri.x[corner] = letterbox.ox + (view.ox + view.h * cx / cz + view.scroll_x) * letterbox.scale;
        tri.y[corner] = letterbox.oy + (view.oy + view.h * cy / cz + view.scroll_y) * letterbox.scale_y;
        tri.z[corner] = cz;
        cz_sum += cz;
        tri.u[corner] = vertex.uv[0];
        tri.v[corner] = 1.0f - vertex.uv[1];
        for (int ch = 0; ch < 3; ++ch)
          tri.rgb[corner][ch] = static_cast<u8>(std::clamp(vertex.color[static_cast<size_t>(ch)] * static_cast<float>(mesh.rgb[ch]), 0.0f, 255.0f));
      }
      if (!ok) continue;
      const bool quad_second = prev_valid && idx[0] == prev_idx[2] && idx[1] == prev_idx[1];
      bool facing;
      if (quad_second) {
        facing = prev_facing;
      } else {
        const float nclip = (tri.x[1] - tri.x[0]) * (tri.y[2] - tri.y[0]) - (tri.x[2] - tri.x[0]) * (tri.y[1] - tri.y[0]);
        const u32 ext = mesh.ext_path ? mesh.ext : 0;
        facing = (ext & 0x20) != 0 || ((ext & 0x10) ? nclip < 1.0f : nclip > 0.0f);
      }
      prev_valid = !quad_second;
      prev_facing = facing;
      prev_idx[0] = idx[0];
      prev_idx[1] = idx[1];
      prev_idx[2] = idx[2];
      if (!facing) continue;
      float packet_z = cz_sum / 3.0f;
      if (const auto centre = centre_of(idx[0]); centre != std::array<float, 3>{}) {
        float cx, cy, cz;
        view_of(centre, cx, cy, cz);
        packet_z = cz;
      }
      tri.slot = std::clamp(mesh.fixed_slot ? mesh.bias : (static_cast<int>(packet_z) >> 4) + mesh.bias, 0, 0xFFF);
      tri.blend = blend;
      tri.set = set;
      tri.tex = ti;
      tri.z[3] = tri.z[2];
      const bool textured = sm.texture >= 0 && part->vertices[idx[0]].uv_scale[0] > 0.0f;
      if (textured) {
        const assets::StageVertex& first = part->vertices[idx[0]];
        tri.texels[2] = first.uv_scale[0];
        tri.texels[3] = first.uv_scale[1];
        tri.texels[0] = first.uv[0] - first.uv_raw[0] * tri.texels[2];
        tri.texels[1] = (1.0f - first.uv[1]) - first.uv_raw[1] * tri.texels[3];
        for (int corner = 0; corner < 3; ++corner) {
          tri.texel_u[corner] = part->vertices[idx[corner]].uv_raw[0];
          tri.texel_v[corner] = part->vertices[idx[corner]].uv_raw[1];
        }
        tri.texel_u[3] = tri.texel_u[2];
        tri.texel_v[3] = tri.texel_v[2];
      }
      if (!scroll || !textured) {
        tri.x[3] = tri.x[2];
        tri.y[3] = tri.y[2];
        tri.u[3] = tri.u[2];
        tri.v[3] = tri.v[2];
        for (int ch = 0; ch < 3; ++ch) tri.rgb[3][ch] = tri.rgb[2][ch];
        out.push_back(tri);
        ++added;
        continue;
      }
      // Scrolled: texel position per corner and the atlas position of the texture page origin, then the pieces inside each window cell.
      std::vector<WrapVert> poly(3);
      float page[3][2];
      for (int corner = 0; corner < 3; ++corner) {
        const assets::StageVertex& vertex = part->vertices[idx[corner]];
        poly[static_cast<size_t>(corner)].x = tri.x[corner];
        poly[static_cast<size_t>(corner)].y = tri.y[corner];
        poly[static_cast<size_t>(corner)].inv_z = 1.0f / tri.z[corner];
        for (int ch = 0; ch < 3; ++ch) poly[static_cast<size_t>(corner)].rgb[ch] = tri.rgb[corner][ch];
        poly[static_cast<size_t>(corner)].texel[0] = vertex.uv_raw[0] + phase[0];
        poly[static_cast<size_t>(corner)].texel[1] = vertex.uv_raw[1] + phase[1];
        page[corner][0] = vertex.uv[0] - vertex.uv_raw[0] * vertex.uv_scale[0];
        page[corner][1] = (1.0f - vertex.uv[1]) - vertex.uv_raw[1] * vertex.uv_scale[1];
      }
      const float scale_u = part->vertices[idx[0]].uv_scale[0], scale_v = part->vertices[idx[0]].uv_scale[1];
      float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
      for (const WrapVert& vert : poly)
        for (int axis = 0; axis < 2; ++axis) {
          lo[axis] = std::min(lo[axis], vert.texel[axis]);
          hi[axis] = std::max(hi[axis], vert.texel[axis]);
        }
      const int cell_first[2] = {static_cast<int>(std::floor((lo[0] - off[0]) / win[0])), static_cast<int>(std::floor((lo[1] - off[1]) / win[1]))};
      const int cell_last[2] = {static_cast<int>(std::floor((hi[0] - off[0]) / win[0])), static_cast<int>(std::floor((hi[1] - off[1]) / win[1]))};
      for (int ky = cell_first[1]; ky <= cell_last[1]; ++ky)
        for (int kx = cell_first[0]; kx <= cell_last[0]; ++kx) {
          std::vector<WrapVert> piece_poly = poly;
          const float cell_origin[2] = {off[0] + static_cast<float>(kx) * win[0], off[1] + static_cast<float>(ky) * win[1]};
          piece_poly = clip_axis(piece_poly, 0, cell_origin[0], true);
          piece_poly = clip_axis(piece_poly, 0, cell_origin[0] + win[0], false);
          piece_poly = clip_axis(piece_poly, 1, cell_origin[1], true);
          piece_poly = clip_axis(piece_poly, 1, cell_origin[1] + win[1], false);
          if (piece_poly.size() < 3) continue;
          // Slivers (a triangle lying exactly on a window line) are skipped by their area.
          float area = 0;
          for (size_t i = 0; i < piece_poly.size(); ++i) {
            const WrapVert& cur = piece_poly[i];
            const WrapVert& next = piece_poly[(i + 1) % piece_poly.size()];
            area += cur.x * next.y - next.x * cur.y;
          }
          if (std::fabs(area) < 1e-4f) continue;
          for (size_t i = 1; i + 1 < piece_poly.size(); ++i) {
            RoomMeshTri piece = tri;
            const WrapVert* corners[3] = {&piece_poly[0], &piece_poly[i], &piece_poly[i + 1]};
            for (int corner = 0; corner < 3; ++corner) {
              piece.x[corner] = corners[corner]->x;
              piece.y[corner] = corners[corner]->y;
              piece.z[corner] = 1.0f / corners[corner]->inv_z;
              for (int ch = 0; ch < 3; ++ch) piece.rgb[corner][ch] = static_cast<u8>(std::clamp(corners[corner]->rgb[ch], 0.0f, 255.0f));
              float texel[2] = {corners[corner]->texel[0] - static_cast<float>(kx) * win[0], corners[corner]->texel[1] - static_cast<float>(ky) * win[1]};
              if (bound[0]) texel[0] = std::clamp(texel[0], off[0] + 0.5f, off[0] + win[0] - 0.5f);
              if (bound[1]) texel[1] = std::clamp(texel[1], off[1] + 0.5f, off[1] + win[1] - 0.5f);
              piece.u[corner] = page[0][0] + texel[0] * scale_u;
              piece.v[corner] = page[0][1] + texel[1] * scale_v;
              piece.texel_u[corner] = texel[0];
              piece.texel_v[corner] = texel[1];
            }
            piece.x[3] = piece.x[2];
            piece.y[3] = piece.y[2];
            piece.z[3] = piece.z[2];
            piece.u[3] = piece.u[2];
            piece.v[3] = piece.v[2];
            piece.texel_u[3] = piece.texel_u[2];
            piece.texel_v[3] = piece.texel_v[2];
            for (int ch = 0; ch < 3; ++ch) piece.rgb[3][ch] = piece.rgb[2][ch];
            out.push_back(piece);
            ++added;
          }
        }
    }
  }
  return added;
}

}  // namespace oc::effects
