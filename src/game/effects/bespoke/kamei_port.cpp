// kamei: Bunyip's transformation. The shell form (caster 0) plays its `ex00` clip, then an env-mapped dome mesh morphs over it (`bod0` ->
// `bod1` -> `bod2`, 129 ticks per step) while the stage pack is swapped by ops 0x100 / 0x101 / 0x102; at the end the shell is hidden for
// good and the second enemy (the new form) is shown and plays its `ex01` clip.
//   init     caster 0 shows its part 3 and starts `ex00` (SetAnim 0x80 flags 1); the dome's env texture (a 64 x 128 window of the pack's
//            `okat` TIM at (576, 384)) is copied to (320, 384) with its CLUT row (256, 237) -> (0, 239) inside the pack VRAM, and the
//            model builders' env page is armed for the parts whose shading flag has bit 0x80.
//   state 0  wait for the ex00 clip: (frames - 1) * 4 ticks (60 keyframes = 236 ticks)
//   state 1  part 3 hidden every tick, bod0 -> bod1 by R = counter * 0x20 until R > 0x1000
//   state 2  bod1 -> bod2 likewise; at its end the new form gets `ex01` (SetAnim 0x80 flags 1)
//   state 3  depth cue off, env page off, caster hidden (and kept hidden after the effect), the new form shown until its clip ends ->
//            SetAnim(1, 0x1E)
//   state 4  hold
// Every tick the env texture scrolls one halfword: ctr = (ctr + 1) & 0x3F, (576 + ctr, 384, 64 - ctr, 128) -> (320, 384) and (576, 384, ctr,
// 128) -> (384 - ctr, 384).
// The morph draw: per packet the corners are the two meshes' vertices lerped ((a (0x1000 - R) + b R) >> 12), NCLIP(v0, v1, v2) > 0 only, as
// GT4 / GT3 on the env page (tpage 0xF5 = page (320, 256) 8 bit, CLUT 0x3BC0) with UVs from the corner's vertex normal turned by the actor's
// rotation (no camera): u = (vx >> 6) + 0x40, v = (vy >> 6) + 0xC0. Vertex normals: the face normals cross(P1 - P0, P2 - P0) >> 6 normalised,
// summed into every corner and the corner normalised again after each face. The draw is lit: every corner colour is the GTE normal colour of
// the first mesh's vertex normal (colour 0x80) under the battle's light slots at the shell's position and the back colour (lit_colour).
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

#include "game/effects/bespoke/overlay_base.h"
#include "game/effects/fx_math.h"
#include "import/battle_stage.h"
#include "import/container.h"

namespace oc::effects {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// The wait on the clip a model plays: armed with (frames - 1) * 4 when the count is below 1, false until the count (minus dt per call) runs
// out; the call that finds it <= 0 returns true and leaves it there (the next call would arm it again).
struct ClipWait {
  int left = 0;
  bool step(int frames, int dt = 1) {
    if (left < 1) {
      const int ticks = (frames - 1) * 4;
      if (ticks <= 0) return true;
      left = ticks;
      return left <= 0;
    }
    left -= dt;
    return left <= 0;
  }
};

struct DomeMesh {
  std::vector<std::array<s16, 3>> verts;
  std::vector<import::StageFace> faces;
  std::vector<std::array<int, 3>> vnorm;  // vertex normals (4.12, length 4096)
};

// v * 4096 / |v|, 0 for the zero vector.
std::array<int, 3> normal4096(double x, double y, double z) {
  const double len = std::sqrt(x * x + y * y + z * z);
  if (len < 1e-9) return {0, 0, 0};
  return {static_cast<int>(std::lround(x * 4096.0 / len)), static_cast<int>(std::lround(y * 4096.0 / len)), static_cast<int>(std::lround(z * 4096.0 / len))};
}

// Per face in file order, the face normal cross(B - A, C - A) >> 6 normalised with A = idx1, B = idx0, C = idx3 (quads) / idx2 (triangles);
// it is added to every corner and each corner is normalised again at once.
void build_normals(DomeMesh& mesh) {
  mesh.vnorm.assign(mesh.verts.size(), {0, 0, 0});
  for (const import::StageFace& face : mesh.faces) {
    const size_t corner_a = face.idx[1], corner_b = face.idx[0], corner_c = face.n == 4 ? face.idx[3] : face.idx[2];
    if (corner_a >= mesh.verts.size() || corner_b >= mesh.verts.size() || corner_c >= mesh.verts.size()) continue;
    const double ux = mesh.verts[corner_b][0] - mesh.verts[corner_a][0], uy = mesh.verts[corner_b][1] - mesh.verts[corner_a][1],
                 uz = mesh.verts[corner_b][2] - mesh.verts[corner_a][2];
    const double vx = mesh.verts[corner_c][0] - mesh.verts[corner_a][0], vy = mesh.verts[corner_c][1] - mesh.verts[corner_a][1],
                 vz = mesh.verts[corner_c][2] - mesh.verts[corner_a][2];
    const auto face_normal =
        normal4096(std::trunc((uy * vz - uz * vy) / 64.0), std::trunc((uz * vx - ux * vz) / 64.0), std::trunc((ux * vy - uy * vx) / 64.0));
    for (int k = 0; k < face.n; ++k) {
      const size_t vi = face.idx[static_cast<size_t>(k)];
      if (vi >= mesh.vnorm.size()) continue;
      auto& normal = mesh.vnorm[vi];
      normal = normal4096(normal[0] + face_normal[0], normal[1] + face_normal[1], normal[2] + face_normal[2]);
    }
  }
}

class Kamei : public OverlayFx {
 protected:
  void init() override {
    load_meshes();
    FxModel* shell = caster(0);
    if (!shell) return;
    shell->part_show |= 1u << 3;
    shell->part_hide &= ~(1u << 3);
    shell->clip_pack = "ex00";
    set_anim(shell, 0x80, 1);
    // Unknown until the host has started the clip.
    shell->clip_frames = 0;
    if (ctx_.models) {
      ctx_.models->env_page = true;
      ctx_.models->vram_moves.push_back({576, 384, 64, 128, 320, 384, false});
      ctx_.models->vram_moves.push_back({256, 237, 256, 1, 0, 239, false});
    }
  }
  bool command(int op, const int*, int) override {
    // 0x100: the new stage's .ctd streamed while the stage layers are off; the host's set_stage loads both files at 0x102.
    if (op == 0x100) return true;
    // 0x101: the stage pack's objects freed, the new .prd read over them.
    if (op == 0x101) {
      if (ctx_.hooks.set_stage) ctx_.hooks.set_stage(-1);
      return true;
    }
    // 0x102: the resources registered, the stage's objects started.
    if (op == 0x102) {
      if (ctx_.hooks.set_stage) ctx_.hooks.set_stage(100);
      return true;
    }
    return false;
  }
  void frame() override {
    FxModel* shell = caster(0);
    if (!shell || !ctx_.models) return;
    ++counter_;
    int morph = counter_ * 0x20;
    switch (state_) {
      case 0:  // the ex00 clip plays out
        // The host has not started the clip yet: the retail wait is armed on the first tick.
        if (shell->clip_frames == 0) {
          ++late_;
          break;
        }
        // The first call arms the wait (and says "not yet"): (frames - 1) * 4 ticks, minus the ticks the host handshake cost.
        if (!armed0_) {
          armed0_ = true;
          wait0_.left = std::max(1, (shell->clip_frames - 1) * 4 - late_);
          break;
        }
        if (wait0_.step(shell->clip_frames)) {
          state_ = 1;
          counter_ = 0;
        }
        break;
      case 1:  // bod0 -> bod1 over the shell with its part 3 hidden
        shell->part_hide |= 1u << 3;
        shell->part_show &= ~(1u << 3);
        if (morph > 0x1000) {
          morph = 0x1000;
          counter_ = 0;
          state_ = 2;
        }
        pose(*shell, 0, morph);
        break;
      case 2:  // bod1 -> bod2, then the new form starts its clip
        if (morph > 0x1000) {
          counter_ = 0;
          state_ = 3;
          if (FxModel* new_form = ctx_.models->everybody(1)) {
            new_form->clip_pack = "ex01";
            set_anim(new_form, 0x80, 1);
            new_form->clip_frames = 0;
          }
          morph = 0x1000;
        }
        pose(*shell, 1, morph);
        break;
      case 3: {  // the shell gives way to the new form until its ex01 clip ends
        if (ctx_.hooks.fog_off) ctx_.hooks.fog_off();
        ctx_.models->env_page = false;
        FxModel* new_form = ctx_.models->everybody(1);
        shell->hidden = true;
        draw_a_ = -1;
        if (!concealed_) {
          shell->conceal = true;
          concealed_ = true;
        }
        if (!new_form) {
          state_ = 4;
          break;
        }
        // The retail place / rotation copy is a no-op: both forms stand on the same spot.
        new_form->hidden = false;
        new_form->reveal = true;
        // The ex01 clip is not started yet.
        if (new_form->clip_frames == 0) break;
        if (wait1_.step(new_form->clip_frames)) {
          set_anim(new_form, 1, 0x1E);
          state_ = 4;
          counter_ = 0;
        }
        break;
      }
      default:  // state 4: hold
        break;
    }
    scroll_ = (scroll_ + 1) & 0x3F;
    ctx_.models->vram_moves.push_back({576 + scroll_, 384, 64 - scroll_, 128, 320, 384, false});
    if (scroll_ != 0) ctx_.models->vram_moves.push_back({576, 384, scroll_, 128, 384 - scroll_, 384, false});
  }
  void draw(std::vector<FxPrim>& out) const override {
    if (draw_a_ < 0) return;
    const DomeMesh& mesh_a = meshes_[static_cast<size_t>(draw_a_)];
    const DomeMesh& mesh_b = meshes_[static_cast<size_t>(draw_a_) + 1];
    if (mesh_a.verts.empty() || mesh_a.verts.size() != mesh_b.verts.size()) return;
    const int weight = std::clamp(draw_r_, 0, 0x1000);
    const Mat3& pose_matrix = draw_m_;
    u32 order = 0;
    const size_t first = out.size();
    std::vector<Vec3> world(mesh_a.verts.size());
    for (size_t i = 0; i < mesh_a.verts.size(); ++i) {
      Vec3 vertex;
      for (size_t k = 0; k < 3; ++k) vertex[k] = static_cast<float>((mesh_a.verts[i][k] * (0x1000 - weight) + mesh_b.verts[i][k] * weight) >> 12);
      const Vec3 rotated = pose_matrix.apply(vertex);
      for (size_t k = 0; k < 3; ++k) world[i][k] = rotated[k] + draw_pos_[k];
    }
    std::vector<std::array<u8, 2>> uvs(mesh_a.verts.size());
    for (size_t i = 0; i < mesh_a.verts.size(); ++i) {
      std::array<int, 3> normal;
      for (size_t k = 0; k < 3; ++k) normal[k] = (mesh_a.vnorm[i][k] * (0x1000 - weight) + mesh_b.vnorm[i][k] * weight) >> 12;
      uvs[i] = env_uv(pose_matrix, normal);
    }
    LightMatrices lights;
    std::array<float, 3> back{};
    const bool lit = ctx_.hooks.lights && ctx_.hooks.lights(draw_pos_, lights, back);
    std::vector<std::array<u8, 3>> colours(mesh_a.verts.size(), {0x80, 0x80, 0x80});
    if (lit)
      for (size_t i = 0; i < mesh_a.verts.size(); ++i) colours[i] = lit_colour(pose_matrix, mesh_a.vnorm[i], lights, back);
    for (const import::StageFace& face : mesh_a.faces) {
      FxPrim prim;
      prim.kind = face.n == 3 ? FxPrim::Kind::Tri : FxPrim::Kind::Quad;
      prim.src = FxPrim::TexSrc::Vram;
      prim.tpage = 0xF5;
      prim.clut = 0x3BC0;
      float sx[4], sy[4], depth_sum = 0;
      bool ok = true;
      for (int k = 0; k < face.n && ok; ++k) {
        const size_t vi = face.idx[static_cast<size_t>(k)];
        if (vi >= world.size()) {
          ok = false;
          break;
        }
        float z;
        if (!fx_project(ctx_.view, world[vi], sx[k], sy[k], z)) {
          ok = false;
          break;
        }
        depth_sum += z;
        prim.v[k] = {sx[k], sy[k], colours[vi][0], colours[vi][1], colours[vi][2]};
        prim.u[k] = uvs[vi][0];
        prim.w[k] = uvs[vi][1];
      }
      if (!ok) continue;
      if (!((sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]) > 0.0f)) continue;
      prim.blend = 0;
      prim.slot = std::clamp(static_cast<int>(depth_sum / static_cast<float>(face.n)) >> 4, 1, 0xFFF);
      prim.order = order++;
      out.push_back(prim);
    }
    last_prims_ = out.size() - first;
  }

 private:
  // The pose the morph is drawn at this tick: the retail matrix is the actor's rotation with its position as translation. Here the unit is
  // drawn through the host's placement (translate * home yaw * y mirror * rel) and the effect world is that placement with y down, so the
  // same chain is applied to the mesh: R_yaw(home) * rel (fx_model_pose), where rel = Ry(-home ry) * RotMatrix(rot): the dome sits on the
  // shell exactly where part 3 of the model does.
  void pose(const FxModel& actor, int mesh, int morph) {
    draw_a_ = mesh;
    draw_r_ = morph;
    const FxModelPose model_pose = fx_model_pose(actor, 1.0f);
    const float yaw = static_cast<float>(actor.home_rot[1] & 0xFFF) * kTwoPi / 4096.0f;
    const float cos_yaw = std::cos(yaw), sin_yaw = std::sin(yaw);
    Mat3 ry;
    ry.m[0] = cos_yaw, ry.m[1] = 0, ry.m[2] = sin_yaw, ry.m[3] = 0, ry.m[4] = 1, ry.m[5] = 0, ry.m[6] = -sin_yaw, ry.m[7] = 0, ry.m[8] = cos_yaw;
    draw_m_ = ry * model_pose.rel;
    for (size_t k = 0; k < 3; ++k) draw_pos_[k] = actor.pos[k];
  }
  // The normal turned by the rotation matrix only, normalised to length 4096, then u = (vx >> 6) + 0x40 and v = (vy >> 6) + 0xC0 as the
  // two bytes of the packet's UV halfword (the window u 0..127, v 128..255 of the env page).
  static std::array<u8, 2> env_uv(const Mat3& pose_matrix, const std::array<int, 3>& normal) {
    const Vec3 rotated = pose_matrix.apply({static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2])});
    const auto unit = normal4096(rotated[0], rotated[1], rotated[2]);
    const int u = (unit[0] >> 6) + 0x40, v = (unit[1] >> 6) + 0xC0;
    const int halfword = ((v << 8) | u) & 0xFFFF;
    return {static_cast<u8>(halfword & 0xFF), static_cast<u8>(halfword >> 8)};
  }
  // The corner colour of the lit draw (GTE normal colour): the light rows are built for the shell's position with the light vectors negated
  // and turned into model space by the actor's rotation, so per light c = max(0, -dir . (R n)) with the first mesh's normal n; colour =
  // 0x80 * (back + sum light colour * c), clamped to 255.
  static std::array<u8, 3> lit_colour(const Mat3& pose_matrix, const std::array<int, 3>& normal, const LightMatrices& lights, const std::array<float, 3>& back) {
    const Vec3 turned = pose_matrix.apply({static_cast<float>(normal[0]) / 4096.0f, static_cast<float>(normal[1]) / 4096.0f, static_cast<float>(normal[2]) / 4096.0f});
    std::array<float, 3> shade = back;
    for (size_t i = 0; i < 3; ++i) {
      const float facing = std::max(0.0f, -(lights.dir[i][0] * turned[0] + lights.dir[i][1] * turned[1] + lights.dir[i][2] * turned[2]));
      for (size_t k = 0; k < 3; ++k) shade[k] += lights.rgb[i][k] * facing;
    }
    std::array<u8, 3> out;
    for (size_t k = 0; k < 3; ++k) out[k] = static_cast<u8>(std::clamp(static_cast<int>(128.0f * shade[k]), 0, 255));
    return out;
  }
  void load_meshes() {
    if (ctx_.pack_prd.empty()) return;
    auto drp = import::parse_drp(ctx_.pack_prd);
    if (!drp) return;
    static constexpr const char* kNames[3] = {"bod0", "bod1", "bod2"};
    for (int i = 0; i < 3; ++i)
      for (const auto& entry : drp->entries)
        if (entry.type == 0x02 && entry.name == kNames[i])
          if (auto mesh = import::parse_stage_mesh(entry.data)) {
            meshes_[static_cast<size_t>(i)].verts = mesh->verts;
            meshes_[static_cast<size_t>(i)].faces = mesh->faces;
            build_normals(meshes_[static_cast<size_t>(i)]);
          }
  }

  std::array<DomeMesh, 3> meshes_;
  int state_ = 0, counter_ = 0, scroll_ = 0, late_ = 0;
  bool armed0_ = false, concealed_ = false;
  ClipWait wait0_, wait1_;
  int draw_a_ = -1, draw_r_ = 0;
  Mat3 draw_m_;
  std::array<float, 3> draw_pos_{0, 0, 0};
};

}  // namespace

void register_kamei_port(BespokeRegistry& registry) {
  registry.add({"kamei", "scripted-motion", "Kamei", [] { return std::make_unique<Kamei>(); }});
}

}  // namespace oc::effects
