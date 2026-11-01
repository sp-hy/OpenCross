#pragma once
// Shared base of the battle-stage ground overlays: the pack's type-0x02 meshes in packet form, the ground bake, the mesh draw through the
// stage (FxGround) and GroundFx, the common base class of every ground overlay. The overlays live in ground_ports*.cpp, each registered by
// its own registrar.
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/effects/bespoke/bespoke.h"
#include "game/effects/bespoke/overlay_base.h"
#include "import/battle_stage.h"
#include "import/container.h"

namespace oc::effects {

void register_ground_ports_b(BespokeRegistry& registry);
void register_ground_ports_c(BespokeRegistry& registry);

namespace ground_detail {

inline int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }
inline s16 s16c(int value) { return static_cast<s16>(value); }

// ---- the pack's type-0x02 meshes in packet form (the overlays copy, bake and draw them packet by packet) ---------------------------------
struct PackPrim {
  // Group type: 0x24 FT3, 0x2C FT4, 0x34 GT3, 0x3C GT4 (0x20 / 0x28 / 0x30 / 0x38 untextured).
  u8 code = 0x3C;
  u8 corners = 4;
  // Packet code bit 1.
  bool semi = false;
  // Corner k <-> idx[k] (textured triangles in the builders' idx1, idx2, idx0 order: import::StageFace).
  std::array<u16, 4> idx{};
  u8 rgb[4][3] = {};
  u8 uv[4][2] = {};
  u16 clut = 0, tpage = 0;
  bool textured() const { return (code & 4) != 0; }
  bool gouraud() const { return (code & 0x10) != 0; }
};
struct PackMesh {
  std::string name;
  // PS1 file space (y down).
  std::vector<std::array<s16, 3>> verts;
  std::vector<PackPrim> prims;
  // The truncating integer mean of the vertex array.
  std::array<int, 3> centroid() const {
    long long sums[3] = {0, 0, 0};
    for (const auto& vertex : verts)
      for (size_t k = 0; k < 3; ++k) sums[k] += vertex[k];
    const long long count = std::max<long long>(1, static_cast<long long>(verts.size()));
    return {static_cast<int>(sums[0] / count), static_cast<int>(sums[1] / count), static_cast<int>(sums[2] / count)};
  }
};
using PackMeshes = std::map<std::string, PackMesh>;

// Every type-0x02 entry of the pack drp, by name (the first of a name wins).
inline PackMeshes load_pack_meshes(ByteSpan prd) {
  PackMeshes out;
  if (prd.empty()) return out;
  auto drp = import::parse_drp(prd);
  if (!drp) return out;
  for (const auto& entry : drp->entries) {
    if (entry.type != 0x02 || out.count(entry.name)) continue;
    auto stage_mesh = import::parse_stage_mesh(entry.data);
    if (!stage_mesh) continue;
    PackMesh pm;
    pm.name = entry.name;
    pm.verts = stage_mesh->verts;
    for (const import::StageFace& face : stage_mesh->faces) {
      PackPrim prim;
      prim.code = face.code;
      prim.corners = face.n;
      prim.semi = face.semi;
      prim.idx = face.idx;
      for (size_t k = 0; k < 4; ++k) {
        for (size_t channel = 0; channel < 3; ++channel) prim.rgb[k][channel] = face.rgb[k][channel];
        prim.uv[k][0] = face.uv[k][0];
        prim.uv[k][1] = face.uv[k][1];
      }
      prim.clut = face.clut;
      prim.tpage = face.tpage;
      pm.prims.push_back(prim);
    }
    out.emplace(entry.name, std::move(pm));
  }
  return out;
}

// ---- the ground bake (shared by wipeoff, jitanda, 2boll, saraman, 11boru, 25upli, 42hevi, tekken) --------------------------------------
// Per textured packet (FT3 / FT4 / GT3 / GT4; other codes untouched): X' = s16(x + 0x1400 + offx), Z' = s16(z - 0x1400 + offz) per corner; the
// cell (cx, cz) of the corner sums (triangles: / 0xC00 truncated, quads: (sum + 0xFFF if negative) >> 12); record cx - 10 cz (no clamp on the
// PS1). Per corner u = (X' - 0x400 cx) * 4, v = (0x400 cz - Z') * 4 (12-bit fractions), the uv = bilinear of the cell's corner bytes (left
// edge c00 -> c01 and right edge c10 -> c11 along v, then left -> right along u, lerp(a, b, t) = a + ((b - a) t >> 12)), the colour the same
// way, written when mode == 1 or (mode == 2 and the packet colour is still exactly 0x808080; a flat packet has one colour slot, so its first
// corner sticks in mode 2 and its last in mode 1); then clut := cell +2, tpage := cell +6.
inline void bake(PackMesh& mesh, const FxGround& ground, int mode, int offx, int offz) {
  for (PackPrim& prim : mesh.prims) {
    if (prim.code != 0x24 && prim.code != 0x2C && prim.code != 0x34 && prim.code != 0x3C) continue;
    int corner_x[4] = {}, corner_z[4] = {}, sx = 0, sz = 0;
    for (int k = 0; k < prim.corners; ++k) {
      const auto& vertex = mesh.verts[prim.idx[static_cast<size_t>(k)]];
      corner_x[k] = s16c(vertex[0] + 0x1400 + offx);
      corner_z[k] = s16c(vertex[2] - 0x1400 + offz);
      sx += corner_x[k];
      sz += corner_z[k];
    }
    int cx = 0, cz = 0;
    if (prim.corners == 3) {
      cx = sx / 0xC00;
      cz = sz / 0xC00;
    } else {
      cx = (sx < 0 ? sx + 0xFFF : sx) >> 12;
      cz = (sz < 0 ? sz + 0xFFF : sz) >> 12;
    }
    const int cell = cx - 10 * cz;
    // The PS1 reads past the table; no shipped mesh does.
    if (cell < 0 || cell >= 100 || !ground.cells[static_cast<size_t>(cell)].present) continue;
    const FxGroundCell& ground_cell = ground.cells[static_cast<size_t>(cell)];
    auto lerp = [](int from, int to, int fraction) { return from + (((to - from) * fraction) >> 12); };
    for (int k = 0; k < prim.corners; ++k) {
      const int u = (corner_x[k] - cx * 0x400) * 4, v = (cz * 0x400 - corner_z[k]) * 4;
      for (int channel = 0; channel < 2; ++channel) {
        const int left = lerp(ground_cell.uv[0][channel], ground_cell.uv[2][channel], v), right = lerp(ground_cell.uv[1][channel], ground_cell.uv[3][channel], v);
        prim.uv[k][channel] = static_cast<u8>(lerp(left, right, u) & 0xFF);
      }
      u8* slot = prim.gouraud() ? prim.rgb[k] : prim.rgb[0];
      if (mode == 1 || (mode == 2 && slot[0] == 0x80 && slot[1] == 0x80 && slot[2] == 0x80))
        for (int channel = 0; channel < 3; ++channel) {
          const int left = lerp(ground_cell.rgb[0][channel], ground_cell.rgb[2][channel], v), right = lerp(ground_cell.rgb[1][channel], ground_cell.rgb[3][channel], v);
          slot[channel] = static_cast<u8>(lerp(left, right, u) & 0xFF);
        }
    }
    if (!prim.gouraud())
      for (int k = 1; k < 4; ++k)
        for (int channel = 0; channel < 3; ++channel) prim.rgb[k][channel] = prim.rgb[0][channel];
    prim.clut = ground_cell.clut;
    prim.tpage = ground_cell.tpage;
  }
}

// ---- drawing a pack mesh through the stage ---------------------------------------------------------------------------------------------
// v' = View * (RotMatrix(rot) (v - centre) + centre + offset); the builders multiply each corner colour by the mesh colour (0x80 = 1.0, saturating),
// blend = the mesh semi flag with its ABR or else the packet's own semi bit with the tpage ABR, OT bias from the mesh,
// NCLIP > 0 faces only (`two_sided` for the overlays' private GT4 drawers).
struct MeshDraw {
  std::array<int, 3> centre{0, 0, 0};
  int rot[3] = {0, 0, 0};
  Vec3 offset{0, 0, 0};
  int bias = 0;
  u8 rgb[3] = {128, 128, 128};
  bool semi = false;
  int abr = 0;
  bool two_sided = false;
  // Morph: vertices lerped toward `morph` by morph_w / 4096, and the packet colours too, per corner (cA (0x1000 - w) + cB w) >> 12.
  const PackMesh* morph = nullptr;
  int morph_w = 0;
  // The morph builders force the primitive code to the group type (0x3C / 0x34: never semi) and take the tpage ABR from the call (0):
  // the packets' own semi bits are not read. The stock draw keeps them.
  bool opaque = false;
  // jitanda / 2boll: y of each vertex replaced (index = vertex).
  const std::vector<int>* heights = nullptr;
  // The overlays' private fog builders (FxGroundPrim::depth_cue).
  bool depth_cue = false;
};

inline void emit_prim(FxGround& ground, const PackMesh& mesh, const PackPrim& source, const MeshDraw& draw, const std::array<Vec3, 4>& world, bool camera_space) {
  FxGroundPrim prim;
  prim.n = source.corners;
  prim.camera_space = camera_space;
  for (int k = 0; k < source.corners; ++k) {
    prim.p[k] = world[static_cast<size_t>(k)];
    prim.uv[k][0] = source.uv[k][0];
    prim.uv[k][1] = source.uv[k][1];
    for (int channel = 0; channel < 3; ++channel) prim.rgb[k][channel] = static_cast<u8>(std::min(255, source.rgb[k][channel] * draw.rgb[channel] / 128));
  }
  prim.clut = source.clut;
  prim.tpage = source.tpage;
  prim.textured = source.textured();
  prim.blend = draw.opaque ? 0 : draw.semi ? (draw.abr & 3) + 1 : source.semi && source.textured() ? ((source.tpage >> 5) & 3) + 1 : 0;
  prim.bias = draw.bias;
  prim.two_sided = draw.two_sided;
  prim.depth_cue = draw.depth_cue;
  (void)mesh;
  ground.prims.push_back(prim);
}

inline void emit_mesh(FxGround& ground, const PackMesh& mesh, const MeshDraw& draw) {
  const Mat3 rotation = Mat3::from_angles(draw.rot[0], draw.rot[1], draw.rot[2]);
  const bool morph = draw.morph && draw.morph->verts.size() == mesh.verts.size();
  // The morph reads packet i of both lists in lockstep; the weight is capped at 0x1000.
  const bool morph_rgb = morph && draw.morph->prims.size() == mesh.prims.size();
  const int mw = std::clamp(draw.morph_w, 0, 0x1000);
  for (size_t pi = 0; pi < mesh.prims.size(); ++pi) {
    PackPrim prim = mesh.prims[pi];
    // Colour = mesh colour * ((colour_a * (0x1000 - weight) + colour_b * weight) >> 12) >> 7 per corner (the mesh colour applied below).
    if (morph_rgb) {
      const PackPrim& morph_prim = draw.morph->prims[pi];
      for (int k = 0; k < 4; ++k)
        for (int channel = 0; channel < 3; ++channel) prim.rgb[k][channel] = static_cast<u8>((prim.rgb[k][channel] * (0x1000 - mw) + morph_prim.rgb[k][channel] * mw) >> 12);
    }
    std::array<Vec3, 4> world{};
    for (int k = 0; k < prim.corners; ++k) {
      const size_t vi = prim.idx[static_cast<size_t>(k)];
      std::array<int, 3> vertex{mesh.verts[vi][0], mesh.verts[vi][1], mesh.verts[vi][2]};
      if (morph)
        for (size_t axis = 0; axis < 3; ++axis) vertex[axis] = (vertex[axis] * (4096 - mw) + draw.morph->verts[vi][axis] * mw) >> 12;
      if (draw.heights && vi < draw.heights->size()) vertex[1] = (*draw.heights)[vi];
      const Vec3 rotated = rotation.apply({static_cast<float>(vertex[0] - draw.centre[0]), static_cast<float>(vertex[1] - draw.centre[1]), static_cast<float>(vertex[2] - draw.centre[2])});
      for (size_t axis = 0; axis < 3; ++axis) world[static_cast<size_t>(k)][axis] = rotated[axis] + static_cast<float>(draw.centre[axis]) + draw.offset[axis];
    }
    emit_prim(ground, mesh, prim, draw, world, false);
  }
}

// ---- base of the ground overlays ---------------------------------------------------------------------------------------------------------
class GroundFx : public OverlayFx {
 public:
  // Every exit (cmd 2) gives the stock ground back and the overlay's draws end with it.
  ~GroundFx() override {
    if (FxGround* stage_ground = fx_ground()) {
      if (owns_skip_) stage_ground->skip_stock = false;
      stage_ground->draws.clear();
      stage_ground->prims.clear();
      if (owns_warp_) {
        stage_ground->warp = nullptr;
        stage_ground->warp_emitters.clear();
        stage_ground->draw_bias_add = 0;
      }
    }
  }
  void tick() override {
    // The overlay redraws its ground / meshes every tick.
    if (FxGround* stage_ground = fx_ground()) {
      stage_ground->draws.clear();
      stage_ground->prims.clear();
    }
    OverlayFx::tick();
    if (const FxGround* stage_ground = fx_ground()) last_prims_ = stage_ground->prims.size();
  }

 protected:
  FxGround* ground() const {
    FxGround* stage_ground = fx_ground();
    return stage_ground && stage_ground->present ? stage_ground : nullptr;
  }
  // Switches the stock ground draw off / on.
  void set_skip(bool on) {
    owns_skip_ = true;
    if (FxGround* stage_ground = ground()) stage_ground->skip_stock = on;
  }
  // Draws the ground with a hole rect as the overlays store it: s16 {x0, z_top, x1, z_bottom}.
  void draw_ground(int x0, int z1, int x1, int z0) {
    if (FxGround* stage_ground = ground()) stage_ground->draws.push_back({s16c(x0), s16c(z1), s16c(x1), s16c(z0)});
  }
  void draw_mesh(const PackMesh& mesh_to_draw, const MeshDraw& draw) {
    if (FxGround* stage_ground = ground()) emit_mesh(*stage_ground, mesh_to_draw, draw);
  }
  void load_meshes() { meshes_ = load_pack_meshes(ctx_.pack_prd); }
  PackMesh* mesh(const std::string& name) {
    auto it = meshes_.find(name);
    return it == meshes_.end() ? nullptr : &it->second;
  }
  // A private copy of a pack mesh (the overlays memcpy a mesh resource and register it under a new name before baking it).
  std::shared_ptr<PackMesh> copy(const std::string& name) {
    const PackMesh* source = mesh(name);
    return source ? std::make_shared<PackMesh>(*source) : nullptr;
  }
  bool bake_mesh(PackMesh* target_mesh, int mode, int offx, int offz) {
    const FxGround* stage_ground = ground();
    if (!target_mesh || !stage_ground) return false;
    bake(*target_mesh, *stage_ground, mode, offx, offz);
    return true;
  }
  // Baked meshes drawn by the pack's emitters (tekken h00n, 11boru hahe / hm**, 42hevi suri via 'yuka'): the overlay changed the mesh
  // resource the stock draw reads, so its emitter objects are drawn here with the baked packets (the host's own draw of those
  // emitters is suppressed). Transform / colour / blend / bias are the objects' own (EmitterSystem::MeshInstance).
  void own_emitter_meshes(const std::set<std::string>& names) {
    emitter_meshes_ = names;
    if (!em()) return;
    for (size_t i = 0; i < em()->header_count(); ++i) {
      if (!own_header(i)) continue;
      for (const auto& op : em()->header_def(i).ops)
        if (op.op == 0x05)
          if (auto* sprite = std::get_if<assets::FxSprite>(&op.arg); sprite && sprite->type == 14 && names.count(sprite->name)) em()->set_draw_suppressed(em()->header_def(i).name, true);
    }
  }
  void draw_emitter_meshes(const u8* colour_override = nullptr) {
    FxGround* stage_ground = ground();
    if (!stage_ground || !em() || emitter_meshes_.empty()) return;
    std::vector<EmitterSystem::MeshInstance> mis;
    em()->collect_meshes(ctx_.view, 1.0f, mis, true);
    for (const auto& mi : mis) {
      if (mi.sprite_type != 14 || !mi.name || !emitter_meshes_.count(*mi.name) || !own_header(static_cast<size_t>(std::max(0, mi.owner)))) continue;
      const PackMesh* source = mesh(*mi.name);
      if (!source) continue;
      PackMesh blended;
      // Object +0x94 bit 1 / +0x98 / +0x9c: vertices (and packet colours) lerped toward the target.
      if (morph_w_ >= 0 && morph_to_ && *mi.name == morph_from_) {
        blended = *source;
        const int weight = std::clamp(morph_w_, 0, 0x1000);
        if (morph_to_->verts.size() == source->verts.size())
          for (size_t i = 0; i < blended.verts.size(); ++i)
            for (size_t axis = 0; axis < 3; ++axis) blended.verts[i][axis] = s16c((source->verts[i][axis] * (0x1000 - weight) + morph_to_->verts[i][axis] * weight) >> 12);
        if (morph_to_->prims.size() == source->prims.size())
          for (size_t i = 0; i < blended.prims.size(); ++i)
            for (int k = 0; k < 4; ++k)
              for (int channel = 0; channel < 3; ++channel)
                blended.prims[i].rgb[k][channel] = static_cast<u8>((source->prims[i].rgb[k][channel] * (0x1000 - weight) + morph_to_->prims[i].rgb[k][channel] * weight) >> 12);
        source = &blended;
      }
      MeshDraw draw;
      for (int channel = 0; channel < 3; ++channel) draw.rgb[channel] = colour_override ? colour_override[channel] : mi.rgb[channel];
      const int blend = mi.blend >= 5 ? 2 : mi.blend;
      draw.semi = blend != 0;
      draw.abr = blend - 1;
      draw.bias = mi.bias;
      for (const PackPrim& prim : source->prims) {
        std::array<Vec3, 4> world{};
        for (int k = 0; k < prim.corners; ++k) {
          const auto& vertex = source->verts[prim.idx[static_cast<size_t>(k)]];
          const Vec3 transformed = mi.mat.apply({static_cast<float>(vertex[0]), static_cast<float>(vertex[1]), static_cast<float>(vertex[2])});
          for (size_t axis = 0; axis < 3; ++axis) world[static_cast<size_t>(k)][axis] = transformed[axis] + mi.centre[axis];
        }
        emit_prim(*stage_ground, *source, prim, draw, world, true);
      }
    }
  }
  // The pack's external table entry 0 on caster 0 (the variant byte is the actor's own, taken as 0 like the host's ops 0x43 / 0x45).
  void caster_anmseq_0x80() { run_script(caster(0), "", 0x80, 0); }

  PackMeshes meshes_;
  std::set<std::string> emitter_meshes_;
  // A morph the overlay set on the objects drawing mesh `morph_from_` (lasbos' 'yuka' object: morph flag, target mesh and weight).
  std::string morph_from_;
  const PackMesh* morph_to_ = nullptr;
  int morph_w_ = -1;  // -1 = no morph
  bool owns_skip_ = false, owns_warp_ = false;
  Rng rng_;
  int rand() { return rng_.next(); }
  void seed_rng() { rng_.state = ctx_.seed ? ctx_.seed : 1; }
};

}  // namespace ground_detail
}  // namespace oc::effects
