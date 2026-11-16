// Room-effect mesh objects (sprite type 14 in a room .prd): ryu_a27i's `elem` (the opn_a04i crystal column) loads through import::load_stage and projects
// to ordering-table triangles. Needs the game install (SKIP without one).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "game/effects/room_meshes.h"
#include "import/battle_stage.h"
#include "platform/paths.h"
#include "render/stage_tiles.h"
#include "vfs/vfs.h"

using namespace oc;

int main() {
  std::string gd = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
  const auto dir = platform::find_game_dir(gd);
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  // Audit mode: RM_LIST = file with one room .prd stem per line; prints load_stage / validate_stage per prd.
  if (const char* list = std::getenv("RM_LIST")) {
    std::FILE* lf = std::fopen(list, "r");
    char line[256];
    while (lf && std::fgets(line, sizeof line, lf)) {
      std::string stem = line;
      while (!stem.empty() && (stem.back() == 10 || stem.back() == 13)) stem.pop_back();
      if (stem.empty()) continue;
      const auto prd_bytes = files.read("cdrom/map/mapbin/" + stem + ".prd");
      if (!prd_bytes) {
        std::printf("%s,missing\n", stem.c_str());
        continue;
      }
      std::string e2;
      auto s2 = import::load_stage(*prd_bytes, {}, &e2);
      const import::StageValidation validation = import::validate_stage(*prd_bytes, {});
      size_t faces = 0, sub_tex = 0;
      if (s2)
        for (const auto& part : s2->parts)
          for (const auto& sm : part.submeshes) {
            faces += sm.index_count / 3;
            if (sm.texture >= 0) ++sub_tex;
          }
      std::printf("%s,%s,parts=%zu,textures=%zu,textured_submeshes=%zu,tris=%zu,validate_errors=%zu%s%s\n", stem.c_str(), s2 ? "ok" : "FAIL", s2 ? s2->parts.size() : 0,
                  s2 ? s2->textures.size() : 0, sub_tex, faces, validation.errors.size(), validation.errors.empty() ? "" : ",first=", validation.errors.empty() ? "" : validation.errors[0].c_str());
    }
    if (lf) std::fclose(lf);
    return 0;
  }
  // Data check of the NCLIP sign: RM_SIGN = file with room .prd stems; one-sided closed-ish meshes, geometry vs MAC0.
  if (const char* list = std::getenv("RM_SIGN")) {
    std::FILE* lf = std::fopen(list, "r");
    char line[256];
    size_t tot_pos = 0, tot_neg = 0, tot = 0, parts_used = 0;
    while (lf && std::fgets(line, sizeof line, lf)) {
      std::string stem = line;
      while (!stem.empty() && (stem.back() == 10 || stem.back() == 13)) stem.pop_back();
      const auto prd_bytes = stem.empty() ? std::nullopt : files.read("cdrom/map/mapbin/" + stem + ".prd");
      if (!prd_bytes) continue;
      auto s2 = import::load_stage(*prd_bytes, {}, nullptr);
      if (!s2) continue;
      for (const auto& part : s2->parts) {
        if (part.ground || part.indices.size() < 36) continue;
        auto pos = [&](u32 vi) { return std::array<double, 3>{part.vertices[vi].pos[0], -double(part.vertices[vi].pos[1]), part.vertices[vi].pos[2]}; };
        // two-sided meshes (every face present in both windings) cannot tell the sign: skip a part whose faces repeat their corner sets
        std::vector<std::array<long, 9>> keys;
        bool dup = false;
        for (size_t i = 0; i + 2 < part.indices.size() && !dup; i += 3) {
          std::array<std::array<long, 3>, 3> corners;
          for (size_t k = 0; k < 3; ++k)
            for (size_t axis = 0; axis < 3; ++axis) corners[k][axis] = std::lround(pos(part.indices[i + k])[axis]);
          std::sort(corners.begin(), corners.end());
          std::array<long, 9> key;
          for (size_t k = 0; k < 3; ++k)
            for (size_t axis = 0; axis < 3; ++axis) key[k * 3 + axis] = corners[k][axis];
          dup = std::find(keys.begin(), keys.end(), key) != keys.end();
          keys.push_back(key);
        }
        if (dup) continue;
        std::array<double, 3> cm{};
        for (u32 vi = 0; vi < part.vertices.size(); ++vi)
          for (size_t k = 0; k < 3; ++k) cm[k] += pos(vi)[k] / double(part.vertices.size());
        // The view: the part's centroid 6000 in front of the camera (identity orientation), so "outward . position < 0" = front.
        size_t ap = 0, an = 0, face_count = 0;
        for (size_t i = 0; i + 2 < part.indices.size(); i += 3) {
          std::array<double, 3> view[3];
          for (size_t corner = 0; corner < 3; ++corner) view[corner] = pos(part.indices[i + corner]), view[corner][0] -= cm[0], view[corner][1] -= cm[1], view[corner][2] += 6000.0 - cm[2];
          std::array<double, 3> fc{}, oc{};
          for (size_t corner = 0; corner < 3; ++corner)
            for (size_t k = 0; k < 3; ++k) fc[k] += view[corner][k] / 3.0;
          double dotp = 0;
          for (size_t k = 0; k < 3; ++k) dotp += (fc[k] - (k == 2 ? 6000.0 : 0.0)) * fc[k];
          double sx[3], sy[3];
          for (size_t corner = 0; corner < 3; ++corner) sx[corner] = 1000.0 * view[corner][0] / view[corner][2], sy[corner] = 1000.0 * view[corner][1] / view[corner][2];
          const double nclip = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
          if (std::fabs(nclip) < 1e-6) continue;
          ++face_count;
          ap += (nclip > 0) == (dotp < 0);
          an += (nclip < 0) == (dotp < 0);
        }
        if (face_count < 12) continue;
        ++parts_used;
        tot += face_count, tot_pos += ap, tot_neg += an;
        std::printf("%s/%s: %zu faces, MAC0>0 agrees %zu, MAC0<0 agrees %zu\n", stem.c_str(), part.name.c_str(), face_count, ap, an);
      }
    }
    if (lf) std::fclose(lf);
    std::printf("TOTAL %zu one-sided parts, %zu faces: MAC0>0 agrees %zu (%.1f%%), MAC0<0 agrees %zu\n", parts_used, tot, tot_pos, tot ? 100.0 * double(tot_pos) / double(tot) : 0.0, tot_neg);
    return 0;
  }
  const auto prd = files.read("cdrom/map/mapbin/ryu_a27i.prd");
  if (!prd) {
    std::printf("FAIL: ryu_a27i.prd\n");
    return 1;
  }
  std::string err;
  auto st = import::load_stage(*prd, {}, &err);
  if (!st) {
    std::printf("FAIL: load_stage: %s\n", err.c_str());
    return 1;
  }
  int failures = 0;
  bool found = false;
  for (const auto& part : st->parts) {
    std::printf("part %s: %zu vertices, %zu indices, %zu submeshes\n", part.name.c_str(), part.vertices.size(), part.indices.size(), part.submeshes.size());
    if (part.name == "elem") found = true;
  }
  std::printf("textures %zu\n", st->textures.size());
  size_t opaque = 0;
  if (!st->textures.empty()) {
    const auto& im = st->textures[0].image;
    for (size_t i = 3; i < im.pixels.size(); i += 4) opaque += im.pixels[i] != 0;
    std::printf("texture 0: %ux%u, %zu opaque texels\n", im.width, im.height, opaque);
  }
  if (opaque == 0) {
    std::printf("FAIL: elem's texture page (960, 0) is blank\n");
    return 1;
  }
  if (!found) {
    std::printf("FAIL: no elem part\n");
    ++failures;
  }
  // NCLIP: the game keeps a face when MAC0 = (x1-x0)(y2-y0) - (x2-x0)(y1-y0) > 0 on the screen positions (x right, y down) of its packet
  // corners 0,1,2. `elem` is a TWO-SIDED shape (every face is stored in both windings: 20 keep with MAC0 > 0 and 20 with MAC0 < 0, 2
  // are edge-on), so it cannot tell the sign; the sign comes from the RM_SIGN data audit below (one-sided meshes: MAC0 > 0 = outward faces).
  size_t pass_pos = 0, pass_neg = 0, tris_all = 0;
  for (const auto& part : st->parts) {
    if (part.name != "elem") continue;
    auto view = [&](u32 vi) { return std::array<double, 3>{part.vertices[vi].pos[0], -double(part.vertices[vi].pos[1]), part.vertices[vi].pos[2] + 6000.0}; };
    for (size_t i = 0; i + 2 < part.indices.size(); i += 3) {
      std::array<double, 3> corners[3] = {view(part.indices[i]), view(part.indices[i + 1]), view(part.indices[i + 2])};
      double sx[3], sy[3];
      for (int corner = 0; corner < 3; ++corner) sx[corner] = 1035.0 * corners[corner][0] / corners[corner][2], sy[corner] = 1035.0 * corners[corner][1] / corners[corner][2];
      const double nclip = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
      ++tris_all;
      pass_pos += nclip > 0;
      pass_neg += nclip < 0;
    }
  }
  std::printf("NCLIP on elem: %zu faces, MAC0>0 keeps %zu, MAC0<0 keeps %zu\n", tris_all, pass_pos, pass_neg);
  if (pass_pos != pass_neg || pass_pos != 20 || tris_all != 42) {
    std::printf("FAIL: elem is expected two-sided: 42 faces, 20 / 20 by sign\n");
    ++failures;
  }
  effects::RoomMeshes rm;
  rm.set(std::move(*st), nullptr, nullptr);
  const std::string name = "elem";
  effects::EmitterSystem::MeshInstance mi;
  mi.name = &name;
  mi.centre = {0, 0, 6000};
  mi.rgb[0] = 64, mi.rgb[1] = 15, mi.rgb[2] = 72;
  mi.blend = 4;
  mi.bias = 1;
  effects::ViewParams vp;
  vp.h = 1035;
  render::Letterbox lb = render::Letterbox::identity(320, 224, 1.0f);
  std::vector<effects::RoomMeshTri> tris;
  // Without a GPU context the textured faces are projected too (set = null).
  const size_t projected = rm.project(mi, vp, lb, 0.0f, tris);
  std::printf("elem: %zu triangles, slot of the first %d\n", projected, tris.empty() ? -1 : tris[0].slot);
  if (projected == 0) {
    std::printf("FAIL: elem projects to nothing\n");
    ++failures;
  }
  if (projected != pass_pos) {
    std::printf("FAIL: project() keeps %zu faces, MAC0 > 0 keeps %zu\n", projected, pass_pos);
    ++failures;
  }

  // Op 36 UV scroll (ryu_a27i sui6 / sui7: window (64, 64) 64 x 64): a phase of 32 texels pushes the 64..127 packet texels past the window edge at 128, so
  // triangles are cut along it and every piece samples inside the window (u * 128 in [64, 128] on the 128-texel wide 8bpp page atlas).
  {
    effects::EmitterSystem::MeshInstance sm = mi;
    sm.uv_scroll = true;
    sm.uv_window = {64, 64, 64, 64};
    sm.uv_phase = {32.0f, 0.0f};
    std::vector<effects::RoomMeshTri> scrolled;
    const size_t ns = rm.project(sm, vp, lb, 0.0f, scrolled);
    float umin = 1e9f, umax = -1e9f;
    for (const auto& tri : scrolled)
      for (int corner = 0; corner < 3; ++corner) umin = std::min(umin, tri.u[corner] * 128.0f), umax = std::max(umax, tri.u[corner] * 128.0f);
    std::printf("scrolled elem: %zu triangles (%zu unscrolled), u range %.2f..%.2f\n", ns, projected, umin, umax);
    if (ns <= projected || umin < 64.0f || umax > 128.01f) {
      std::printf("FAIL: UV scroll windowing\n");
      ++failures;
    }
  }

  // Texel pick: a baked copy of the asset keeps every corner's packet texel coordinate, the coordinate places the corner's normalised u / v through the
  // face's page origin and texel size, and the texel it names holds the same pixel in the baked object image as in the raw atlas.
  {
    auto raw = import::load_stage(*prd, {}, &err);
    auto baked = import::load_stage(*prd, {}, &err);
    if (raw && baked) {
      render::bake_object_textures(*baked);
      const import::Image* raw_image = nullptr;
      const import::Image* baked_image = nullptr;
      size_t textured_subs = 0;
      for (size_t pi = 0; pi < baked->parts.size(); ++pi) {
        if (baked->parts[pi].name != "elem") continue;
        for (const assets::StageSubMesh& sub : baked->parts[pi].submeshes) {
          if (sub.texture < 0 || sub.object_texture < 0) continue;
          ++textured_subs;
          raw_image = &raw->textures[static_cast<size_t>(sub.texture)].image;
          baked_image = &baked->object_textures[static_cast<size_t>(sub.object_texture)].image;
        }
      }
      const import::Image raw_copy = raw_image ? *raw_image : import::Image{};
      const import::Image baked_copy = baked_image ? *baked_image : import::Image{};
      effects::RoomMeshes raw_meshes, baked_meshes;
      raw_meshes.set(std::move(*raw), nullptr, nullptr);
      baked_meshes.set(std::move(*baked), nullptr, nullptr);
      std::vector<effects::RoomMeshTri> raw_tris, baked_tris;
      raw_meshes.project(mi, vp, lb, 0.0f, raw_tris);
      baked_meshes.project(mi, vp, lb, 0.0f, baked_tris);
      auto pixel = [](const import::Image& image, const effects::RoomMeshTri& tri, int corner) {
        const float texel_x = std::round(tri.texel_u[corner]), texel_y = std::round(tri.texel_v[corner]);
        const long x = std::lround(std::floor((tri.texels[0] + texel_x * tri.texels[2]) * static_cast<float>(image.width)));
        const long y = std::lround(std::floor((tri.texels[1] + texel_y * tri.texels[3]) * static_cast<float>(image.height)));
        std::array<u8, 4> rgba{};
        if (x >= 0 && y >= 0 && x < static_cast<long>(image.width) && y < static_cast<long>(image.height))
          for (size_t ch = 0; ch < 4; ++ch) rgba[ch] = image.pixels[(static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) * 4 + ch];
        return rgba;
      };
      size_t corners = 0, moved = 0, misplaced = 0, other_pixel = 0;
      if (textured_subs == 1 && raw_tris.size() == baked_tris.size() && !raw_copy.indexed && !baked_copy.indexed) {
        for (size_t i = 0; i < raw_tris.size(); ++i) {
          const effects::RoomMeshTri& raw_tri = raw_tris[i];
          const effects::RoomMeshTri& baked_tri = baked_tris[i];
          if (raw_tri.texels[2] <= 0.0f || baked_tri.texels[2] <= 0.0f) continue;
          for (int corner = 0; corner < 3; ++corner) {
            ++corners;
            moved += raw_tri.texel_u[corner] != baked_tri.texel_u[corner] || raw_tri.texel_v[corner] != baked_tri.texel_v[corner];
            for (const effects::RoomMeshTri* tri : {&raw_tri, &baked_tri})
              misplaced += std::fabs(tri->texels[0] + tri->texel_u[corner] * tri->texels[2] - tri->u[corner]) > 1e-4f ||
                           std::fabs(tri->texels[1] + tri->texel_v[corner] * tri->texels[3] - tri->v[corner]) > 1e-4f;
            other_pixel += pixel(raw_copy, raw_tri, corner) != pixel(baked_copy, baked_tri, corner);
          }
        }
      }
      std::printf("texel pick: %zu corners of %zu triangles, %zu moved, %zu misplaced, %zu with another pixel\n", corners, raw_tris.size(), moved, misplaced, other_pixel);
      if (corners == 0 || moved || misplaced || other_pixel) {
        std::printf("FAIL: the baked elem does not name the raw atlas's texels\n");
        ++failures;
      }
    }
  }

  // Op 37 morph (alu_a06f `fis1` -> `fish`): the same vertex count, so the projected corners move with the phase.
  if (const auto fprd = files.read("cdrom/map/mapbin/alu_a06f.prd")) {
    auto fst = import::load_stage(*fprd, {}, &err);
    effects::RoomMeshes fm;
    if (fst) fm.set(std::move(*fst), nullptr, nullptr);
    const std::string from_name = "fis1", to_name = "fish";
    effects::EmitterSystem::MeshInstance mm;
    mm.name = &from_name;
    mm.morph_name = &to_name;
    mm.centre = {0, 0, 6000};
    mm.blend = 2;
    std::vector<effects::RoomMeshTri> t0, t1, th;
    mm.morph_t = mm.morph_t_prev = 0.0f;
    fm.project(mm, vp, lb, 0.0f, t0);
    mm.morph_t = mm.morph_t_prev = 1.0f;
    fm.project(mm, vp, lb, 0.0f, t1);
    mm.morph_t_prev = 0.0f;
    mm.morph_t = 1.0f;
    // The tick alpha interpolates the phase.
    fm.project(mm, vp, lb, 0.5f, th);
    double d01 = 0, d0h = 0;
    for (size_t i = 0; i < std::min({t0.size(), t1.size(), th.size()}); ++i) {
      d01 += std::fabs(t0[i].x[0] - t1[i].x[0]) + std::fabs(t0[i].y[0] - t1[i].y[0]);
      d0h += std::fabs(t0[i].x[0] - th[i].x[0]) + std::fabs(t0[i].y[0] - th[i].y[0]);
    }
    std::printf("morph fis1 -> fish: %zu / %zu / %zu triangles, displacement full %.1f half %.1f\n", t0.size(), t1.size(), th.size(), d01, d0h);
    // The retail NCLIP runs per frame on the morphed vertices, so the number of
    // faces kept changes with the phase; the sweep is printed (a face flips where the morph folds it through edge-on).
    std::printf("morph sweep (phase 0..1 step 0.1), faces kept:");
    for (int step = 0; step <= 10; ++step) {
      std::vector<effects::RoomMeshTri> ts;
      mm.morph_t_prev = mm.morph_t = float(step) / 10.0f;
      fm.project(mm, vp, lb, 0.0f, ts);
      std::printf(" %zu", ts.size());
    }
    std::printf("\n");
    if (t0.empty() || d01 < 1.0 || d0h < 0.3 * d01 || d0h > 0.7 * d01) {
      std::printf("FAIL: morph\n");
      ++failures;
    }
  } else {
    std::printf("SKIP morph: alu_a06f.prd\n");
  }
  return failures ? 1 : 0;
}
