// Boss and monster deaths: ports of the PS1 overlays that draw the dying model themselves (model_draw.h).
//   garadeth / mushi   eids 374 / 391   two-pass silhouette (additive own CLUT + subtractive white CLUT), garadeth with a rising kill line
//   ki_deth            eid 388          grey CLUT fade, then the six 'hik*' particles eat vertices while quads fall, both sides, two passes
//   drg_deth           eids 404 521 522 spheres between attach points break faces into rising spinning shards; 0x182 breaks all
//   octdeth            eid 373          grey CLUT fade, then a plane sweeps down from y = -1500 and only the part below it is drawn
//   daimou_d           eids 386 387     the body sinks and melts into a puddle along its normals, two passes
//   F-B dissolves      sfin bossde dorublue skadeth kroto dorured objcrash darkfeit lasdes3 lasdes1: a growing hole (fade by the
//                      distance to a centre) with 'kie0' / 'kie1' fired at fading faces or the faces cloned into particles (shards)
//   lasdes2            eid 532          the Time Devourer's end: the dissolve plus camera cross-fades, music changes, screen tint, eye frames
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

#include "game/effects/bespoke/bespoke_util.h"
#include "game/effects/bespoke/mesh_tech.h"
#include "game/effects/fx_math.h"

namespace oc::effects {
namespace {

int trunc_div(int dividend, int divisor) { return divisor == 0 ? 0 : dividend / divisor; }
// Square0 per axis, SquareRoot0 of the sum.
int dist3(int dx, int dy, int dz) { return ps1_square_root0(dx * dx + dy * dy + dz * dz); }

// ---- garadeth / mushi -----------------------------------------------------------------------------------------------------------------
// garadeth and mushi are the same code. M = caster 0. 0x180 (D = s16 @+4): the part flag bit 1 (semi) is set; for D ticks the overlay
// draws M twice and hides the stock pass (then M stays hidden): pass 1 with the model's tpage ABR 1 and its own CLUT, pass 2 ABR 2 (B - F)
// with CLUT 0x3B10 = the white rows (0x100, 0xEC / 0xED) loaded at init (garadeth: 256 x 0xFFFF, index 0 included; mushi: the model's own
// row, every non-zero entry -> 0xFFFF). Pass 2 is added later, so it runs first in each OT entry: the silhouette is blacked out, then the
// texture added. garadeth's vertex colour: f = 0 where T < y (0x181, speed @+8: T -= speed per tick from 0, a kill line rising from the
// feet) or, in state 2 (0x182, speed @+8: W += speed), where |cz - z| < W (cz = M's z at init); colour = f x CW >> 7, Gouraud. mushi
// draws flat CW through the stock hook (no shadow call, ops 0x181 / 0x182 store values nobody reads).
class GaraDeth : public MeshTech {
 public:
  explicit GaraDeth(bool mushi) : mushi_(mushi) {}

 protected:
  void init() override {
    if (const FxModel* model = caster(0)) cz_ = static_cast<int>(model->pos[2]);
  }
  bool command(int op, const int* args, int count) override {
    if (op == 0x180) {
      duration_ = arg(args, count, 4), mode_ = 0x180, tick_ = 0, state_ = 0;
    } else if (op == 0x181 && !mushi_) {
      kill_y_ = 0, state_ = 1, sp1_ = arg(args, count, 8);
    } else if (op == 0x182 && !mushi_) {
      width_ = 0, state_ = 2, sp2_ = arg(args, count, 8);
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* model = caster(0);
    if (!model || mode_ != 0x180) return;
    if (duration_ < ++tick_) {
      // The model is gone: its hidden flag stays set.
      mode_ = 0;
      model->custom_draw = model->own_shadow = false;
      return;
    }
    if (state_ == 1) kill_y_ -= sp1_;
    if (state_ == 2) width_ += sp2_;
    // garadeth draws its own shadow; mushi goes through the stock hook (shadow only with part flag 0x200).
    own_draw(model, true, !mushi_);
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = caster(0);
    if (!model || mode_ != 0x180) return;
    ModelDrawSpec draw_spec;
    draw_spec.flags = 0x2;
    draw_spec.force_abr = 1;
    if (!mushi_) {
      const int kill_y = kill_y_, width = width_, state = state_, cz = cz_;
      draw_spec.colour = [kill_y, width, state, cz](const DrawVert& v, u8 rgb[3]) {
        int factor = kill_y < v.y ? 0 : 0x80;
        if (state == 2 && std::abs(cz - v.z) < width) factor = 0;
        for (int k = 0; k < 3; ++k) rgb[k] = static_cast<u8>((factor * rgb[k]) >> 7);
      };
    }
    u32 order = 0;
    const size_t n0 = out.size();
    FxSkinnedModel sk;
    if (!skin(model, sk)) return;
    model_draw(sk, *model, draw_spec, ctx_.view, out, order);
    draw_spec.force_abr = 2;
    draw_spec.white = mushi_ ? 1 : 2;
    model_draw(sk, *model, draw_spec, ctx_.view, out, order);
    last_prims_ = out.size() - n0;
  }

 private:
  bool mushi_;
  int mode_ = 0, tick_ = 0, duration_ = 0, state_ = 0, kill_y_ = 0, width_ = 0, sp1_ = 0, sp2_ = 0, cz_ = 0;
};

// ---- ki_deth --------------------------------------------------------------------------------------------------------------------------
// M = caster 0. 0x180 (D = s16 @+4): the animation is reset (SetAnim(M, 0x80, 0x1E)); the model's two CLUT rows fade to grey over D ticks (g =
// (R + G + B) / 3 x k, k = t 0x1000 / D, channel' = (g + c (0x1000 - k)) >> 12, | 0x8000; drawn by the stock pass). 0x182 (D = s16 @+4)
// replaces the mode (the grey stays where it was), 0x181 restores the rows. Mode 0x182: the fall counter grows 10 per tick after t 0x8C;
// quads 16..63 of every part (and quads > 7 of part 1) move down by it, every quad corner is clamped to y <= 0; a vertex closer than 300 to
// one of the six 'hik0'..'hik5' particles (the last live one of each) is eaten for good (colour 0); every face is drawn twice, both
// sides: packet A semi ABR 1 own CLUT (OT + 1 for quads), packet B ABR 2 with the white CLUT at OT + 1. At the end the model
// stays hidden and its height offset is reset to 0.
class KiDeth : public MeshTech {
 protected:
  bool command(int op, const int* args, int count) override {
    FxModel* model = caster(0);
    if (op == 0x180) {
      duration_ = arg(args, count, 4), mode_ = 0x180, tick_ = 0;
      set_anim(model, 0x80, 0x1E);
      if (model) model->hidden = false;
    } else if (op == 0x181) {
      if (model) model->grey = -1;
    } else if (op == 0x182) {
      duration_ = arg(args, count, 4), mode_ = 0x182, tick_ = 0;
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* model = caster(0);
    if (!model) return;
    // The six points: the last live particle of each emitter wins; a point without one keeps its value.
    if (em())
      for (int k = 0; k < 6; ++k) {
        const std::string name = "hik" + std::string(1, static_cast<char>('0' + k));
        em()->for_each_object(name, [&](EmitterSystem::ObjectRef& object) {
          for (int axis = 0; axis < 3; ++axis) pt_[k][axis] = static_cast<s16>(object.pos[axis] >> 16);
        });
      }
    if (mode_ == 0x180) {
      if (duration_ < ++tick_) mode_ = 0;
      else model->grey = trunc_div(tick_ * 0x1000, duration_), model->grey_stp = true;
    } else if (mode_ == 0x182) {
      if (duration_ < ++tick_) {
        mode_ = 0;
        model->move_to({model->pos[0], 0.0f, model->pos[2]});
        model->custom_draw = false;
        return;
      }
      if (tick_ > 0x8C) fall_ += 10;
      eat();
      own_draw(model, true, false);
    }
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = caster(0);
    if (!model || mode_ != 0x182) return;
    FxSkinnedModel sk;
    if (!skin(model, sk)) return;
    ModelDrawSpec draw_spec = spec();
    const std::vector<u8>* eaten = &eaten_;
    draw_spec.colour = [eaten](const DrawVert& v, u8 rgb[3]) {
      if (v.idx >= 0 && static_cast<size_t>(v.idx) < eaten->size() && (*eaten)[static_cast<size_t>(v.idx)]) rgb[0] = rgb[1] = rgb[2] = 0;
    };
    u32 order = 0;
    const size_t n0 = out.size();
    draw_spec.force_abr = 1, draw_spec.quad_bias = 1;
    model_draw(sk, *model, draw_spec, ctx_.view, out, order);
    draw_spec.force_abr = 2, draw_spec.white = 1, draw_spec.tri_bias = 1, draw_spec.quad_bias = 1;
    model_draw(sk, *model, draw_spec, ctx_.view, out, order);
    last_prims_ = out.size() - n0;
  }

 private:
  ModelDrawSpec spec() const {
    ModelDrawSpec draw_spec;
    draw_spec.flags = 0x2;
    draw_spec.cull = false;
    const int fall = fall_;
    draw_spec.face_hook = [fall](DrawVert* corners, int corner_count, int, int quad_index, int part) {
      if (corner_count != 4) return true;
      if ((part == 1 && quad_index > 7) || static_cast<unsigned>(quad_index - 0x10) < 0x30u)
        for (int k = 0; k < 4; ++k) corners[k].y = static_cast<s16>(corners[k].y + fall);
      for (int k = 0; k < 4; ++k) corners[k].y = std::min(corners[k].y, 0);
      return true;
    };
    return draw_spec;
  }
  // The hook's test, run on every drawn corner: a vertex within 300 (r^2 < 90000, s16 differences) of any point is eaten for good.
  void eat() {
    FxSkinnedModel sk;
    if (!skin(caster(0), sk)) return;
    if (eaten_.size() < sk.verts.size()) eaten_.resize(sk.verts.size(), 0);
    ModelDrawSpec draw_spec = spec();
    std::array<int, 16> quads{};
    int face_index = -1;
    for (const FxSkinFace& face : sk.faces) {
      ++face_index;
      if (face.part < 0 || face.part >= 16) continue;
      DrawVert corners[4];
      for (int k = 0; k < face.n; ++k) corners[k] = draw_vert(sk, face, k);
      const int quad_index = face.n == 4 ? quads[static_cast<size_t>(face.part)]++ : -1;
      draw_spec.face_hook(corners, face.n, face_index, quad_index, face.part);
      for (int k = 0; k < face.n; ++k) {
        if (corners[k].idx < 0 || eaten_[static_cast<size_t>(corners[k].idx)]) continue;
        for (const auto& point : pt_) {
          const int dx = static_cast<s16>(corners[k].x - point[0]), dy = static_cast<s16>(corners[k].y - point[1]),
                    dz = static_cast<s16>(corners[k].z - point[2]);
          if (dx * dx + dy * dy + dz * dz < 90000) eaten_[static_cast<size_t>(corners[k].idx)] = 1;
        }
      }
    }
  }

  int mode_ = 0, tick_ = 0, duration_ = 0, fall_ = 0;
  std::array<std::array<int, 3>, 6> pt_{};
  std::vector<u8> eaten_;
};

// ---- drg_deth -------------------------------------------------------------------------------------------------------------------------
// M = caster 0, drawn by the overlay every frame (no shadow): flat CW faces, both sides, OT + 1, quad corners
// clamped to y <= 0, a face broken once stays gone. 0x183 (D @+4, attach A @+8, B @+0xA, radius @+0xC): a sphere in the first of 5 free
// slots moves from attach A to B over D ticks (pos = pa + (pb - pa) t / D, then t += 1, freed when D < t). A face whose FIRST corner is
// inside a sphere (or every face after 0x182 sets the flag) breaks: it is still drawn this frame, then a shard task takes it:
// v = (0, -(rand % 5) - 5, 0), every tick turned about z round its centroid by 128 (ticks << 7) and moved by v, drawn FT3 grey 0x80 opaque both
// sides with the face's texture, for 60 (0x3C) ticks. Quad shards are drawn with xy3 never written (degenerate: invisible on the PS1)
// and are not drawn here.
class DrgDeth : public MeshTech {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x182) {
      all_ = true;
    } else if (op == 0x183) {
      for (Sphere& sphere : sph_)
        if (sphere.dur == 0) {
          sphere = {};
          sphere.dur = arg(args, count, 4), sphere.attach_a = arg(args, count, 8), sphere.attach_b = arg(args, count, 0xA);
          const int radius = arg(args, count, 0xC);
          sphere.r2 = radius * radius;
          break;
        }
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* model = caster(0);
    if (!model) return;
    for (Sphere& sphere : sph_) {
      if (sphere.dur == 0) continue;
      int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
      attach(model, sphere.attach_a, pa);
      attach(model, sphere.attach_b, pb);
      for (int k = 0; k < 3; ++k) sphere.pos[k] = pa[k] + trunc_div((pb[k] - pa[k]) * sphere.elapsed, sphere.dur);
      const int t_old = sphere.elapsed;
      sphere.elapsed += 1;
      if (sphere.dur < t_old + 1) sphere.dur = 0;
    }
    // Shard tasks: age, spin, rise; freed past 60 ticks.
    for (Shard& shard : shards_) {
      if (++shard.age > 0x3C) {
        shard.dead = true;
        continue;
      }
      int centroid[3];
      for (int k = 0; k < 3; ++k) centroid[k] = static_cast<s16>(shard.verts[0][k] + shard.verts[1][k] + shard.verts[2][k]) / 3;
      const int cs = rcos(128), sn = rsin(128);
      for (auto& vert : shard.verts) {
        const int dx = vert[0] - centroid[0], dy = vert[1] - centroid[1];
        vert[0] = static_cast<s16>(((dx * cs - dy * sn) >> 12) + centroid[0]);
        vert[1] = static_cast<s16>(((dx * sn + dy * cs) >> 12) + centroid[1]);
        vert[1] = static_cast<s16>(vert[1] + shard.vy);
      }
    }
    shards_.erase(std::remove_if(shards_.begin(), shards_.end(), [](const Shard& shard) { return shard.dead; }), shards_.end());
    // The face pass: draw list + breaks.
    polys_.clear();
    FxSkinnedModel sk;
    if (skin(model, sk)) {
      if (broken_.size() < sk.faces.size()) broken_.resize(sk.faces.size(), 0);
      Rng& rng = rng_;
      ModelDrawSpec cw;
      cw.mask_part_flags = true;
      for (size_t fi = 0; fi < sk.faces.size(); ++fi) {
        const FxSkinFace& face = sk.faces[fi];
        if (broken_[fi] || face.part < 0 || face.part >= 16) continue;
        Poly poly;
        poly.count = face.n, poly.tex = face.tex;
        u8 b3;
        model_colour_word(sk, *model, cw, face.part, poly.rgb, b3);
        if (b3 & 1) continue;
        poly.semi = (b3 & 2) != 0;
        poly.abr = (sk.shading[static_cast<size_t>(face.part)][3] >> 2) & 3;
        for (int k = 0; k < face.n; ++k) {
          poly.corners[k] = draw_vert(sk, face, k);
          if (face.n == 4) poly.corners[k].y = std::min(poly.corners[k].y, 0);
        }
        polys_.push_back(poly);
        if (eat(poly.corners[0])) {
          broken_[fi] = 1;
          if (face.n == 3) {
            Shard shard;
            for (int k = 0; k < 3; ++k)
              shard.verts[k] = {poly.corners[k].x, poly.corners[k].y, poly.corners[k].z}, shard.u[k] = poly.corners[k].u,
              shard.w[k] = poly.corners[k].w;
            shard.tex = face.tex;
            shard.vy = -(rng.next() % 5) - 5;
            shards_.push_back(shard);
          } else {
            // The quad shard rolls its velocity too (drawn degenerate).
            (void)(rng.next() % 5);
          }
        }
      }
    }
    own_draw(model, true, false);
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = caster(0);
    if (!model) return;
    const size_t n0 = out.size();
    u32 order = 0;
    for (const Poly& poly : polys_) {
      ProjPoly pp;
      if (!project_poly(ctx_.view, poly.corners, poly.count, 0, pp)) continue;
      const float z = poly.count == 3 ? (pp.z[0] + pp.z[1] + pp.z[2]) / 3.0f : (pp.z[1] + pp.z[2] + pp.z[3]) / 3.0f;
      const int slot = (static_cast<int>(z) >> 4) + 1;
      if (poly.count == 4 && slot - 1 >= 0x1000) continue;
      const u8 rgb[4][3] = {{poly.rgb[0], poly.rgb[1], poly.rgb[2]}, {poly.rgb[0], poly.rgb[1], poly.rgb[2]}, {poly.rgb[0], poly.rgb[1], poly.rgb[2]}, {poly.rgb[0], poly.rgb[1], poly.rgb[2]}};
      push_poly(pp, poly.corners, model->unit, poly.tex, rgb, poly.semi ? poly.abr + 1 : 0, 0, slot, order, out);
    }
    for (const Shard& shard : shards_) {
      DrawVert corners[3];
      for (int k = 0; k < 3; ++k)
        corners[k].x = shard.verts[k][0], corners[k].y = shard.verts[k][1], corners[k].z = shard.verts[k][2], corners[k].u = shard.u[k],
        corners[k].w = shard.w[k];
      ProjPoly pp;
      if (!project_poly(ctx_.view, corners, 3, 0, pp)) continue;
      const u8 rgb[4][3] = {{0x80, 0x80, 0x80}, {0x80, 0x80, 0x80}, {0x80, 0x80, 0x80}, {0x80, 0x80, 0x80}};
      push_poly(pp, corners, model->unit, shard.tex, rgb, 0, 0, static_cast<int>((pp.z[0] + pp.z[1] + pp.z[2]) / 3.0f) >> 4, order, out);
    }
    last_prims_ = out.size() - n0;
  }
  bool done() const override { return shards_.empty(); }

 private:
  struct Sphere {
    int elapsed = 0, dur = 0, r2 = 0, attach_a = 0, attach_b = 0;
    int pos[3] = {0, 0, 0};
  };
  struct Poly {
    DrawVert corners[4];
    int count = 3, tex = 0, abr = 0;
    bool semi = false;
    u8 rgb[3] = {128, 128, 128};
  };
  struct Shard {
    std::array<std::array<int, 3>, 3> verts{};
    float u[3] = {}, w[3] = {};
    int tex = 0, vy = 0, age = 0;
    bool dead = false;
  };
  bool eat(const DrawVert& v0) const {
    if (all_) return true;
    for (const Sphere& sphere : sph_) {
      if (sphere.dur == 0) continue;
      const int dx = static_cast<s16>(v0.x - sphere.pos[0]), dy = static_cast<s16>(v0.y - sphere.pos[1]), dz = static_cast<s16>(v0.z - sphere.pos[2]);
      if (dx * dx + dy * dy + dz * dz < sphere.r2) return true;
    }
    return false;
  }

  bool all_ = false;
  std::array<Sphere, 5> sph_{};
  std::vector<u8> broken_;
  std::vector<Poly> polys_;
  std::vector<Shard> shards_;
  Rng rng_{};
};

// ---- octdeth --------------------------------------------------------------------------------------------------------------------------
// M = caster 0. 0x180 (D @+4): SetAnim(M, 0x80, 0x1E); the CLUT rows fade to grey over D ticks without STP (an entry ending 0 turns
// transparent), then stay grey; 0x181 restores them. 0x182 (D @+4): L =-1500 + (1500 / (D - 15)) t; while t <= D the overlay
// draws M (normal speed, unit shadow) clipped at y = L keeping y > L (live clipper, GT3 max-SZ for part flag 0x10) and puts the
// emitters 'hika' 'kona' 'kemu' 'kem1' at (x + 400, L, z); afterwards M stays hidden.
class OctDeth : public MeshTech {
 protected:
  bool command(int op, const int* args, int count) override {
    FxModel* model = caster(0);
    if (op == 0x180) {
      duration_ = arg(args, count, 4), mode_ = 0x180, tick_ = 0;
      set_anim(model, 0x80, 0x1E);
      if (model) model->hidden = false;
    } else if (op == 0x181) {
      if (model) model->grey = -1;
    } else if (op == 0x182) {
      mode_ = 0x182, tick_ = 0, duration_ = arg(args, count, 4);
      step_ = trunc_div(1500, duration_ - 15);
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* model = caster(0);
    if (!model) return;
    if (mode_ == 0x180) {
      if (++tick_ <= duration_) model->grey = trunc_div(tick_ << 12, duration_), model->grey_stp = false;
      else mode_ = 0;
    } else if (mode_ == 0x182) {
      if (++tick_ <= duration_) {
        plane_y_ = -1500 + step_ * tick_;
        model->anim_speed = 0x40;
        own_draw(model, true, true);
        if (em())
          for (const char* emitter_name : {"hika", "kona", "kemu", "kem1"})
            em()->set_position(emitter_name, static_cast<int>(model->pos[0]) + 400, static_cast<s16>(plane_y_), static_cast<int>(model->pos[2]));
      } else {
        mode_ = 0;
        model->custom_draw = model->own_shadow = false;
      }
    }
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = caster(0);
    if (!model || mode_ != 0x182) return;
    ModelDrawSpec draw_spec;
    draw_spec.tri_far_z = true;
    draw_spec.clip = true;
    draw_spec.plane = plane_y_;
    draw_spec.skip[0] = true, draw_spec.skip[1] = false;
    u32 order = 0;
    const size_t n0 = out.size();
    draw_model(model, draw_spec, out, order);
    last_prims_ = out.size() - n0;
  }

 private:
  int mode_ = 0, tick_ = 0, duration_ = 0, step_ = 0, plane_y_ = -1500;
};

// ---- daimou_d -------------------------------------------------------------------------------------------------------------------------
// M = caster 0 (init shows it). 0x180 (D @+4, S @+8, mode @+10): the white CLUT rows (0x100, 0xEC / 0xED) from M's own row
// (non-zero entries -> 0xFFFF). Frame: for D ticks M sinks S units per tick, its skinning (the stock skinning without the normal clamp)
// moves every world vertex below y = -200: h = y + 200, x += h nx >> 8, z += h nz >> 8, y = 0
// - the body melts into a puddle spreading along its normals; drawn by the stock model draw (flat CW, culled), mode 0 twice (ABR 1 own CLUT,
// then ABR 2 white CLUT), else once semi-transparent; the stock pass is hidden. At the end the height offset is reset and M stays hidden.
class Daimou : public MeshTech {
 protected:
  void init() override {
    if (FxModel* model = caster(0)) model->hidden = false;
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x180) return false;
    state_ = 0x180, tick_ = 0, duration_ = arg(args, count, 4), sink_speed_ = arg(args, count, 8), mode_ = arg(args, count, 10);
    return true;
  }
  void frame() override {
    FxModel* model = caster(0);
    if (!model || state_ != 0x180) return;
    if (duration_ < ++tick_) {
      state_ = 0;
      model->move_to({model->pos[0], 0.0f, model->pos[2]});
      model->custom_draw = false;
      return;
    }
    model->move_to({model->pos[0], model->pos[1] + static_cast<float>(sink_speed_), model->pos[2]});
    own_draw(model, true, false);
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = caster(0);
    if (!model || state_ != 0x180) return;
    ModelDrawSpec draw_spec;
    draw_spec.flags = 0x2;
    draw_spec.face_hook = [](DrawVert* corners, int corner_count, int, int, int) {
      for (int k = 0; k < corner_count; ++k)
        if (corners[k].y > -200) {
          const int height = corners[k].y + 200;
          corners[k].x = static_cast<s16>(corners[k].x + ((height * corners[k].nx) >> 8));
          corners[k].y = 0;
          corners[k].z = static_cast<s16>(corners[k].z + ((height * corners[k].nz) >> 8));
        }
      return true;
    };
    u32 order = 0;
    const size_t n0 = out.size();
    FxSkinnedModel sk;
    if (!skin(model, sk)) return;
    if (mode_ == 0) {
      draw_spec.force_abr = 1;
      model_draw(sk, *model, draw_spec, ctx_.view, out, order);
      draw_spec.force_abr = 2, draw_spec.white = 1;
      model_draw(sk, *model, draw_spec, ctx_.view, out, order);
    } else {
      // The model's own tpage ABR.
      draw_spec.force_abr = 0;
      model_draw(sk, *model, draw_spec, ctx_.view, out, order);
    }
    last_prims_ = out.size() - n0;
  }

 private:
  int state_ = 0, tick_ = 0, duration_ = 0, sink_speed_ = 0, mode_ = 0;
};

// ---- F-B dissolves --------------------------------------------------------------------------------------------------------------------
// One dissolve task per op 0x100 (D @+4, a @+8, b @+10, y @+12, cnt = byte @+14 (objcrash: the word), p6 = byte @+15, z offset @+16, x offset
// @+18) on M = target 0: R = a t / D + b (kroto: a - a t / D + b), band B = b. Every frame of the task the overlay skins M and hides
// the stock pass for good (skadeth shows it again at the task end); every vertex gets c from its distance d to the centre C: out-fade (e = d -
// R) or kroto's in-fade (e = R - d): c = e < 0 ? max(0, e + B) 0x80 / B : 0x80 (skadeth clamps e + B >= 0x10); faces with every c 0 are
// skipped, faces whose corners are all 0x80 (bossde dorublue skadeth: corner 0 only) are one opaque packet, the others two: semi ABR 1 with the
// own CLUT plus a copy ABR 2 with the white CLUT (non-zero entries | 0x7FFF), the copy drawn first. Culling NCLIP, quads drawn
// when NCLIP(v0,v1,v2) > 0 or NCLIP(v1,v2,v3) <= 0; OT = SZ of the last corner >> 2 >> shift. Emission (E1): faces drawn partial with corner
// 0's c < 0x40 give their centroid, then 'kie0' fires at random centroids (2 per frame; dorublue cnt per tick plus 'kie1' with probability 1 /
// (p6 + 1)). E2 (kroto dorured objcrash darkfeit lasdes3): cnt times a random partial face that was not taken yet becomes a 'kie0' particle
// carrying the face (corners relative to its centroid) drawn by the replaced callback: FT3 / FT4 with the particle's colour, both sides,
// ABR 1 own CLUT + ABR 2 white CLUT, OT + the particle's bias; 'kie1' fires there with probability 1 / (p6 + 1); lasdes3 gives the particle the
// velocity ((Mx - cx) << 13) / D, ((Mz - cz) << 13) / D (D from op 0x106). Op 0x101 = the screen negative for w@4 ticks; op 0x59 (all but
// sfin bossde skadeth) is replaced: target w@10 gets SetAnim(w@8, 0x20) (+0x22 / +0x2A loop and speed not reproduced).
enum class Fade { Out, In, Ska };
enum class Centre { HalfHeight, AbsY, Attach9, CasterAttach9, RelY, RelYOff };
enum class Emit { None, E1, E1Cnt, E2 };
struct FbParams {
  const char* name;
  Fade fade;
  bool bright_all;  // the full-bright test on every corner (else corner 0)
  bool two_pass;
  Centre centre;
  Emit emit;
  bool kie1;
  bool op59;
  bool word_cnt;    // objcrash: cnt = the word @+14
  // Velocity toward the model, part flag bit 1 semi on full-bright faces, ops 0x104..0x106.
  bool lasdes3;
  // Ops 0x102 / 0x103: the env page's CLUT row fade (darkfeit, lasdes3; FxModels::env_clut).
  bool env_clut;
  // lasdes2: corners take the model colour (FxSkinnedModel::rgb) times the fade, e < 0 ? max(0, e + B) colour / B : colour; a face whose last
  // corner fades is one packet, semi-transparent with the part's ABR (flag bits 2-3); 'kie0' fires when corner 0's red byte < 0x30; a face is
  // skipped when every red byte is 0.
  bool model_colour = false;
};

class Dissolve : public MeshTech {
 public:
  explicit Dissolve(const FbParams& params) : params_(params) {}

 protected:
  bool command(int op, const int* args, int count) override {
    bool acted = true;
    if (op == 0x100) {
      Task task;
      task.dur = arg(args, count, 4), task.reach = arg(args, count, 8), task.band = arg(args, count, 10), task.y = arg(args, count, 12);
      const int w14 = arg(args, count, 14);
      task.cnt = params_.word_cnt ? w14 : (w14 & 0xFF);
      task.p6 = (w14 >> 8) & 0xFF;
      task.zo = arg(args, count, 16), task.xo = arg(args, count, 18);
      tasks_d_.push_back(task);
    } else if (op == 0x101) {
      nega_.start(arg(args, count, 4));
    } else if (op == 0x102 && params_.centre == Centre::CasterAttach9) {
      burst(args, count);
    } else if (params_.lasdes3 && (op == 0x104 || op == 0x105)) {
      if (FxModel* model = target(0)) {
        const int part = arg(args, count, 8);
        if (part >= 0 && part < 32) {
          const u32 bit = 1u << part;
          if (op == 0x104) model->part_hide |= bit, model->part_show &= ~bit;
          else model->part_hide &= ~bit, model->part_show |= bit;
        }
      }
    } else if (params_.lasdes3 && op == 0x106) {
      velocity_div_ = arg(args, count, 8);
    } else {
      acted = false;
    }
    // Op 0x102 starts the CLUT fade task (t = 0, D = s16 @+4); op 0x103 restores the saved row (0, 239, 256, 1).
    if (params_.env_clut && op == 0x102) {
      clut_t_ = 0, clut_d_ = std::max(1, arg(args, count, 4));
      acted = true;
    } else if (params_.env_clut && op == 0x103) {
      if (ctx_.models) ctx_.models->env_clut = -1, ctx_.models->env_clut_set = true;
      acted = true;
    }
    return acted;
  }
  bool stock_op(int op, const int* args, int count) override {
    if (op != 0x59 || !params_.op59) return false;
    if (FxModel* model = target(arg(args, count, 10))) set_anim(model, arg(args, count, 8), 0x20);
    return true;
  }
  void frame() override {
    nega_.tick();
    polys_.clear();
    // t += the frame delta (1 per tick); k = 0 and the task freed once D < t, the row is written either way.
    if (clut_d_ > 0 && ctx_.models) {
      ++clut_t_;
      const int level = clut_d_ < clut_t_ ? 0 : 0x100 - (clut_t_ << 8) / clut_d_;
      if (clut_d_ < clut_t_) clut_d_ = 0;
      ctx_.models->env_clut = level, ctx_.models->env_clut_set = true;
    }
    FxModel* model = target(0);
    if (!model) return;
    for (size_t i = 0; i < tasks_d_.size();) {
      Task& task = tasks_d_[i];
      if (task.dur < ++task.elapsed) {
        if (params_.fade == Fade::Ska) model->hidden = false, model->custom_draw = false;
        tasks_d_.erase(tasks_d_.begin() + static_cast<std::ptrdiff_t>(i));
        continue;
      }
      pass(*model, task);
      ++i;
    }
  }
  void draw(std::vector<FxPrim>& out) const override {
    const size_t n0 = out.size();
    u32 order = 0;
    const FxModel* model = target(0);
    if (model)
      for (const Poly& poly : polys_) {
        ProjPoly pp;
        if (!project_poly(ctx_.view, poly.corners, poly.count, 2, pp)) continue;
        const int slot = static_cast<int>(pp.z[poly.count - 1]) >> 4;
        if (poly.mode == 0) {
          push_poly(pp, poly.corners, model->unit, poly.tex, poly.rgb, poly.semi ? poly.semi_blend : 0, 0, slot, order, out);
        } else {
          push_poly(pp, poly.corners, model->unit, poly.tex, poly.rgb, poly.mode == 1 ? 2 : 0, 0, slot, order, out);
          // The copy: ABR 2 white CLUT, drawn first.
          if (poly.mode == 1) push_poly(pp, poly.corners, model->unit, poly.tex, poly.rgb, 3, 1, slot, order, out);
        }
      }
    draw_shards(out, order);
    if (nega_.active()) nega_.draw(out);
    last_prims_ = out.size() - n0;
  }
  bool done() const override { return tasks_d_.empty() && !nega_.active(); }

 private:
  struct Task {
    int elapsed = 0, dur = 0, reach = 0, band = 0, y = 0, cnt = 0, p6 = 0, zo = 0, xo = 0;
  };
  struct Poly {
    DrawVert corners[4];
    // mode: 0 one opaque packet (semi with lasdes3's part bit 1), 1 two packets, 2 one packet (skadeth).
    int count = 3, tex = 0, mode = 0;
    bool semi = false;
    int semi_blend = 2;
    u8 rgb[4][3] = {};
  };
  struct ShardPoly {
    int count = 3, tex = 0;
    int offsets[4][3] = {};
    float u[4] = {}, w[4] = {};
  };

  void centre_of(const FxModel& model, const Task& task, int centre[3]) const {
    const int x = static_cast<int>(model.pos[0]), y = static_cast<int>(model.pos[1]), z = static_cast<int>(model.pos[2]);
    centre[0] = x, centre[1] = y, centre[2] = z;
    switch (params_.centre) {
      case Centre::HalfHeight: centre[1] = y + (-static_cast<int>(model.height)) / 2; break;
      case Centre::AbsY: centre[1] = task.y; break;
      case Centre::Attach9: attach(&model, 9, centre); break;
      case Centre::CasterAttach9: attach(caster(0), 9, centre); break;
      case Centre::RelY: centre[1] = y + task.y; break;
      case Centre::RelYOff: centre[0] = x + task.xo, centre[1] = y + task.y, centre[2] = z + task.zo; break;
    }
  }
  int fade(int distance, int reach, int band) const {
    const int excess = params_.fade == Fade::In ? reach - distance : distance - reach;
    if (excess >= 0) return 0x80;
    int level = std::max(0, excess + band);
    if (params_.fade == Fade::Ska) level = std::max(level, 0x10);
    return trunc_div(level * 128, band);
  }
  void pass(FxModel& model, const Task& task) {
    FxSkinnedModel sk;
    if (!skin(&model, sk)) return;
    own_draw(&model, true, false);
    const int reach = params_.fade == Fade::In ? task.reach - trunc_div(task.reach * task.elapsed, task.dur) + task.band
                                              : trunc_div(task.reach * task.elapsed, task.dur) + task.band,
              band = task.band;
    int centre[3];
    centre_of(model, task, centre);
    std::vector<std::array<int, 3>> cents;
    std::vector<ShardPoly> recs;
    for (const FxSkinFace& face : sk.faces) {
      if (face.part < 0 || face.part >= 16) continue;
      // The F-B draws do not skip parts by flag bit 0.
      const u8 part_flags = sk.shading[static_cast<size_t>(face.part)][3];
      Poly poly;
      poly.count = face.n, poly.tex = face.tex;
      if (params_.model_colour) {
        bool fading = false, any_red = false;
        for (int k = 0; k < face.n; ++k) {
          poly.corners[k] = draw_vert(sk, face, k);
          const int excess = dist3(poly.corners[k].x - centre[0], poly.corners[k].y - centre[1], poly.corners[k].z - centre[2]) - reach;
          fading = excess < 0;
          for (int ch = 0; ch < 3; ++ch)
            poly.rgb[k][ch] = static_cast<u8>(fading ? trunc_div(std::max(0, excess + band) * sk.rgb[ch], band) : sk.rgb[ch]);
          any_red = any_red || poly.rgb[k][0] != 0;
        }
        if (!any_red) continue;
        ProjPoly pp;
        if (!project_poly(ctx_.view, poly.corners, poly.count, 2, pp)) continue;
        poly.mode = 0;
        poly.semi = fading || (part_flags & 2);
        poly.semi_blend = ((part_flags >> 2) & 3) + 1;
        if (fading && poly.rgb[0][0] < 0x30) {
          int sum[3] = {0, 0, 0};
          for (int k = 0; k < face.n; ++k) sum[0] += poly.corners[k].x, sum[1] += poly.corners[k].y, sum[2] += poly.corners[k].z;
          std::array<int, 3> centroid{};
          for (int k = 0; k < 3; ++k) centroid[static_cast<size_t>(k)] = face.n == 3 ? static_cast<s16>(sum[k]) / 3 : trunc_div(sum[k], 4);
          cents.push_back(centroid);
        }
        polys_.push_back(poly);
        continue;
      }
      bool any = false;
      for (int k = 0; k < face.n; ++k) {
        poly.corners[k] = draw_vert(sk, face, k);
        const int shade = fade(dist3(poly.corners[k].x - centre[0], poly.corners[k].y - centre[1], poly.corners[k].z - centre[2]), reach, band);
        poly.rgb[k][0] = poly.rgb[k][1] = poly.rgb[k][2] = static_cast<u8>(shade);
        any = any || shade != 0;
      }
      if (!any) continue;
      ProjPoly pp;
      // Culled: neither drawn nor emitting.
      if (!project_poly(ctx_.view, poly.corners, poly.count, 2, pp)) continue;
      bool bright = poly.rgb[0][0] == 0x80;
      if (params_.bright_all)
        for (int k = 1; k < face.n; ++k) bright = bright && poly.rgb[k][0] == 0x80;
      if (bright) {
        poly.mode = 0;
        poly.semi = params_.lasdes3 && (part_flags & 2);
      } else {
        poly.mode = params_.two_pass ? 1 : 2;
        if (poly.rgb[0][0] < 0x40) {
          int sum[3] = {0, 0, 0};
          for (int k = 0; k < face.n; ++k) sum[0] += poly.corners[k].x, sum[1] += poly.corners[k].y, sum[2] += poly.corners[k].z;
          std::array<int, 3> centroid{};
          for (int k = 0; k < 3; ++k) centroid[static_cast<size_t>(k)] = face.n == 3 ? static_cast<s16>(sum[k]) / 3 : trunc_div(sum[k], 4);
          if (params_.emit == Emit::E2) {
            ShardPoly record;
            record.count = face.n, record.tex = face.tex;
            for (int k = 0; k < face.n; ++k) {
              record.offsets[k][0] = poly.corners[k].x - centroid[0], record.offsets[k][1] = poly.corners[k].y - centroid[1],
              record.offsets[k][2] = poly.corners[k].z - centroid[2];
              record.u[k] = poly.corners[k].u, record.w[k] = poly.corners[k].w;
            }
            recs.push_back(record);
          }
          cents.push_back(centroid);
        }
      }
      polys_.push_back(poly);
    }
    emit(model, task, cents, recs);
  }
  void emit(const FxModel& model, const Task& task, const std::vector<std::array<int, 3>>& cents, const std::vector<ShardPoly>& recs) {
    if (!em() || cents.empty()) return;
    Rng& rng = rng_;
    if (params_.emit == Emit::E1) {
      for (int k = 0; k < 2; ++k) {
        const auto& centroid = cents[static_cast<size_t>(rng.next() % static_cast<int>(cents.size()))];
        em()->fire_at("kie0", centroid[0], centroid[1], centroid[2]);
      }
    } else if (params_.emit == Emit::E1Cnt) {
      for (int k = 0; k < task.cnt; ++k) {
        const auto& centroid = cents[static_cast<size_t>(rng.next() % static_cast<int>(cents.size()))];
        em()->fire_at("kie0", centroid[0], centroid[1], centroid[2]);
        if (params_.kie1 && rng.next() % (task.p6 + 1) == 0) em()->fire_at("kie1", centroid[0], centroid[1], centroid[2]);
      }
    } else if (params_.emit == Emit::E2 && !recs.empty()) {
      std::vector<u8> used(recs.size(), 0);
      for (int k = 0; k < task.cnt; ++k) {
        const size_t i = static_cast<size_t>(rng.next() % static_cast<int>(recs.size()));
        if (used[i]) continue;
        used[i] = 1;
        const auto& centroid = cents[i];
        const int tag = static_cast<int>(shard_polys_.size());
        std::function<void(EmitterSystem::ObjectRef&)> init;
        if (params_.lasdes3 && velocity_div_ != 0) {
          const int vx = ((static_cast<int>(model.pos[0]) - centroid[0]) << 13) / velocity_div_,
                    vz = ((static_cast<int>(model.pos[2]) - centroid[2]) << 13) / velocity_div_;
          init = [vx, vz](EmitterSystem::ObjectRef& object) {
            if (object.vel) object.vel[0] = vx, object.vel[2] = vz;
          };
        }
        if (!em()->fire_at("kie0", centroid[0], centroid[1], centroid[2], tag, init)) break;
        shard_polys_.push_back(recs[i]);
        if (params_.kie1 && rng.next() % (task.p6 + 1) == 0) em()->fire_at("kie1", centroid[0], centroid[1], centroid[2]);
      }
    }
  }
  void draw_shards(std::vector<FxPrim>& out, u32& order) const {
    if (!em() || shard_polys_.empty()) return;
    const FxModel* model = target(0);
    if (!model) return;
    std::vector<EmitterSystem::TaggedInstance> instances;
    em()->collect_tagged(ctx_.view, 1.0f, instances);
    for (const auto& instance : instances) {
      if (instance.tag < 0 || static_cast<size_t>(instance.tag) >= shard_polys_.size()) continue;
      const ShardPoly& record = shard_polys_[static_cast<size_t>(instance.tag)];
      ProjPoly pp;
      pp.n = record.count;
      bool ok = true;
      for (int k = 0; k < record.count && ok; ++k) {
        const Vec3 offset = instance.mat.apply(
            {static_cast<float>(record.offsets[k][0]), static_cast<float>(record.offsets[k][1]), static_cast<float>(record.offsets[k][2])});
        const float cx = instance.centre[0] + offset[0], cy = instance.centre[1] + offset[1], cz = instance.centre[2] + offset[2];
        if (cz < 32.0f) ok = false;
        else {
          pp.x[k] = ctx_.view.ox + ctx_.view.h * cx / cz + ctx_.view.scroll_x;
          pp.y[k] = ctx_.view.oy + ctx_.view.h * cy / cz + ctx_.view.scroll_y;
          pp.z[k] = cz;
        }
      }
      if (!ok) continue;
      DrawVert corners[4];
      for (int k = 0; k < record.count; ++k) corners[k].u = record.u[k], corners[k].w = record.w[k];
      const u8 rgb[4][3] = {{instance.rgb[0], instance.rgb[1], instance.rgb[2]}, {instance.rgb[0], instance.rgb[1], instance.rgb[2]},
                            {instance.rgb[0], instance.rgb[1], instance.rgb[2]}, {instance.rgb[0], instance.rgb[1], instance.rgb[2]}};
      const int slot = std::max(0, (static_cast<int>(pp.z[record.count - 1]) >> 4) + instance.bias);
      push_poly(pp, corners, model->unit, record.tex, rgb, 2, 0, slot, order, out);
      push_poly(pp, corners, model->unit, record.tex, rgb, 3, 1, slot, order, out);
    }
  }
  // skadeth 0x102: the emitter named by the 4 bytes @+8 fires once at the centroid of every front face of caster 0, which is hidden.
  void burst(const int* args, int count) {
    FxModel* model = caster(0);
    if (!model || !em()) return;
    const int w8 = arg(args, count, 8), w10 = arg(args, count, 10);
    const char emitter_name[5] = {static_cast<char>(w8 & 0xFF), static_cast<char>((w8 >> 8) & 0xFF), static_cast<char>(w10 & 0xFF),
                                  static_cast<char>((w10 >> 8) & 0xFF), 0};
    FxSkinnedModel sk;
    if (!skin(model, sk)) return;
    model->hidden = true;
    for (const FxSkinFace& face : sk.faces) {
      DrawVert corners[4];
      for (int k = 0; k < face.n; ++k) corners[k] = draw_vert(sk, face, k);
      ProjPoly pp;
      if (!project_poly(ctx_.view, corners, face.n, 1, pp)) continue;
      int sum[3] = {0, 0, 0};
      for (int k = 0; k < face.n; ++k) sum[0] += corners[k].x, sum[1] += corners[k].y, sum[2] += corners[k].z;
      int centroid[3];
      for (int k = 0; k < 3; ++k) centroid[k] = face.n == 3 ? static_cast<s16>(sum[k]) / 3 : trunc_div(sum[k], 4);
      if (!em()->fire_at(emitter_name, centroid[0], centroid[1], centroid[2])) break;
    }
  }

  FbParams params_;
  std::vector<Task> tasks_d_;
  std::vector<Poly> polys_;
  std::vector<ShardPoly> shard_polys_;
  util::NegativeStrip nega_;
  Rng rng_{};
  int velocity_div_ = 0;
  // The 0x102 task (none while clut_d_ == 0).
  int clut_t_ = 0, clut_d_ = 0;
};

constexpr FbParams kFb[] = {
    {"sfin", Fade::Out, true, true, Centre::HalfHeight, Emit::E1, false, false, false, false, false},
    {"bossde", Fade::Out, false, true, Centre::HalfHeight, Emit::E1, false, false, false, false, false},
    {"dorublue", Fade::Out, false, true, Centre::AbsY, Emit::E1Cnt, true, true, false, false, false},
    {"skadeth", Fade::Ska, false, false, Centre::CasterAttach9, Emit::E1, false, false, false, false, false},
    {"kroto", Fade::In, true, true, Centre::Attach9, Emit::E2, true, true, false, false, false},
    {"dorured", Fade::Out, true, true, Centre::RelY, Emit::E2, true, true, false, false, false},
    {"objcrash", Fade::Out, true, true, Centre::RelY, Emit::E2, false, true, true, false, false},
    {"darkfeit", Fade::Out, true, true, Centre::RelY, Emit::E2, true, true, false, false, true},
    {"lasdes3", Fade::Out, true, true, Centre::RelYOff, Emit::E2, true, true, false, true, true},
};

// ---- lasdes2 (the Time Devourer's end) -------------------------------------------------------------------------------------------------
// The F-B dissolve of lasdes3 (op 0x100) drawn in the model colour (FbParams::model_colour) with the cnt-per-tick emission ('kie0' / 'kie1'), its
// private ops renumbered: 0x107 / 0x108 part hide /
// show on target 0, 0x109 / 0x10A the env CLUT fade / restore, op 0x59 replaced. The scene ops (D = s16 @+4; every task runs once in the tick that
// starts it, then once per tick, and ends once t > D):
//   0x101 / 0x103  the music changes to song 0x3B / to song s16 @+8
//   0x104          a second camera plays the pack's shot named @+8 and its view fades in: alpha = 0x80 t / D (the fill is reset)
//   0x105          the second view fades out (alpha = 0x80 - 0x80 t / D), then the second camera ends
//   0x106          the battle camera's view fades out (fill = 0xFF t / D, subtracted), then the battle camera ends and the second takes over
//   0x10B          target 0 is drawn by the stock pass again
//   0x10C          rectangle s16 @+8 of a seven-entry table (16 x 32 halfwords in the pack's VRAM) is copied to (0x3C4, 0x32): the pack model's eyes
//   0x10D          the screen tint (rgb bytes @+9..@+11, mode byte @+8) moves from the current colour to the new one: c = c0 + (c1 - c0) t / D
//                  with t before the step; a tint already moving is dropped; nothing starts when colour and mode are unchanged
constexpr FbParams kLasdes2Fb = {"lasdes2", Fade::Out, true, true, Centre::RelYOff, Emit::E1Cnt, true, true, false, true, true, true};

class Lasdes2 : public Dissolve {
 public:
  Lasdes2() : Dissolve(kLasdes2Fb) {}

 protected:
  // The idle-pose word only: the idle ran out on the pose element 341 left, which the target holds through the fall.
  void init() override {
    const FxModel* model = target(0);
    if (model && ctx_.hooks.set_pose) ctx_.hooks.set_pose(model->unit, 8);
  }
  bool command(int op, const int* args, int count) override {
    const int duration = arg(args, count, 4);
    switch (op) {
      case 0x100: Dissolve::command(0x100, args, count); break;
      case 0x107: Dissolve::command(0x104, args, count); break;
      case 0x108: Dissolve::command(0x105, args, count); break;
      case 0x109: Dissolve::command(0x102, args, count); break;
      case 0x10A: Dissolve::command(0x103, args, count); break;
      case 0x101: song(0x3B); break;
      // After the song change: the sound channels come back and pausing is allowed again; the audio host needs neither.
      case 0x102: break;
      case 0x103: song(arg(args, count, 8)); break;
      case 0x104: cross_in(args, count, duration); break;
      case 0x105: cross_ramp(duration, true); break;
      case 0x106: cross_ramp(duration, false); break;
      case 0x10B:
        if (FxModel* model = target(0)) model->hidden = model->custom_draw = false;
        break;
      case 0x10C: eyes(arg(args, count, 8)); break;
      case 0x10D: {
        const int w8 = arg(args, count, 8), w10 = arg(args, count, 10);
        tint(static_cast<u8>(w8 >> 8), static_cast<u8>(w10), static_cast<u8>(w10 >> 8), w8 & 0xFF, duration);
        break;
      }
      default: return false;
    }
    return true;
  }
  bool done() const override { return Dissolve::done() && tasks_.empty(); }

 private:
  void song(int id) const {
    if (ctx_.hooks.play_song) ctx_.hooks.play_song(id);
  }
  void cross_in(const int* args, int count, int duration) {
    if (!ctx_.models) return;
    const int w8 = arg(args, count, 8), w10 = arg(args, count, 10);
    const char name[5] = {static_cast<char>(w8 & 0xFF), static_cast<char>((w8 >> 8) & 0xFF), static_cast<char>(w10 & 0xFF), static_cast<char>((w10 >> 8) & 0xFF), 0};
    FxModels::CrossView& cross = ctx_.models->cross;
    cross.shot = name;
    ++cross.serial;
    cross.alpha = cross.fill = 0;
    const int serial = cross.serial;
    ++alpha_task_;
    const int task = alpha_task_;
    add_task([this, serial, task, duration, elapsed = 0]() mutable {
      FxModels::CrossView& view = ctx_.models->cross;
      if (task != alpha_task_ || view.serial != serial) return false;
      if (duration < ++elapsed) {
        view.alpha = 0x80;
        return false;
      }
      view.alpha = (elapsed << 7) / duration;
      return true;
    });
  }
  // fade_out: 0x105 (the second view leaves); else 0x106 (the battle camera's view leaves).
  void cross_ramp(int duration, bool fade_out) {
    if (!ctx_.models) return;
    add_task([this, duration, fade_out, elapsed = 0]() mutable {
      FxModels::CrossView& view = ctx_.models->cross;
      if (duration < ++elapsed) {
        if (fade_out) view.alpha = 0, view.stop = true;
        else view.fill = 0xFF, view.promote = true;
        return false;
      }
      if (fade_out) view.alpha = 0x80 - (elapsed << 7) / duration;
      else view.fill = elapsed * 0xFF / duration;
      return true;
    });
  }
  void eyes(int index) const {
    // The overlay's table: x, y, w, h in halfwords; entry 7 is empty.
    static constexpr int kEyes[7][2] = {{320, 256}, {336, 256}, {352, 256}, {368, 256}, {320, 288}, {304, 256}, {288, 256}};
    if (!ctx_.models || index < 0 || index >= 7) return;
    ctx_.models->vram_moves.push_back({kEyes[index][0], kEyes[index][1], 16, 32, 0x3C4, 0x32, false});
  }
  void tint(u8 r, u8 g, u8 b, int mode, int duration) {
    if (!ctx_.models) return;
    FxModels::Tint& now = ctx_.models->tint;
    if (now.rgb[0] == r && now.rgb[1] == g && now.rgb[2] == b && now.mode == mode) return;
    now.mode = mode;
    ++tint_task_;
    const int task = tint_task_;
    const std::array<u8, 3> from{now.rgb[0], now.rgb[1], now.rgb[2]}, to{r, g, b};
    add_task([this, task, from, to, duration, elapsed = 0]() mutable {
      if (task != tint_task_) return false;
      FxModels::Tint& colour = ctx_.models->tint;
      std::array<u8, 3> rgb = from;
      if (duration != 0)
        for (size_t k = 0; k < 3; ++k) rgb[k] = static_cast<u8>(from[k] + (to[k] - from[k]) * elapsed / duration);
      const bool over = duration < ++elapsed;
      if (over) rgb = to;
      for (size_t k = 0; k < 3; ++k) colour.rgb[k] = rgb[k];
      return !over;
    });
  }

  int alpha_task_ = 0, tint_task_ = 0;
};

// ---- lasdes1 (NEGAPOSI) ---------------------------------------------------------------------------------------------------------------
// M = target 0, drawn by the overlay every frame (no shadow, the stock pass hidden): the skinned vertices are waved (rates != 0):
// x += A rsin(px + (x << 1)) >> 12, y += A rsin(py + (z << 1)) >> 12, z += A rsin(pz + (x << 1)) >> 12, the phases advancing by the rates per
// tick; every vertex takes its colour from a table reset to M's colour each frame and darkened by the particles of the emitters named '---*'
// (their op-0x48 callback: radius R = scale / 6; a vertex at d < R gets k = (d - R) 128 / R + 128 and, when k is below its current value,
// colour = (particle colour >> 4) k >> 7). Faces with corner 0 at 0x80 are one opaque packet, others the two-pass pair, all-zero faces skipped.
// 0x100 = the screen negative (w@4 ticks), 0x101 (D @+4, rates @+8 / +10 / +12, amplitude @+14) lerps rates and amplitude over D ticks.
class Lasdes1 : public MeshTech {
 protected:
  void init() override {
    if (!em() || !ctx_.pack_fx) return;
    for (size_t i = 0; i < em()->header_count(); ++i) {
      if (!own_header(i)) continue;
      const std::string& header_name = em()->header_def(i).name;
      if (header_name.size() >= 3 && header_name.compare(0, 3, "---") == 0 &&
          std::find(names_.begin(), names_.end(), header_name) == names_.end())
        names_.push_back(header_name);
    }
    for (const std::string& name : names_)
      em()->set_object_hook(name, [this](EmitterSystem::ObjectRef& object) { paint(object); }, true);
  }
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      nega_.start(arg(args, count, 4));
    } else if (op == 0x101) {
      lerp_ = {};
      lerp_.on = true;
      lerp_.dur = arg(args, count, 4);
      for (int k = 0; k < 3; ++k) lerp_.from[k] = rate_[k], lerp_.to[k] = arg(args, count, 8 + 2 * k);
      lerp_.from[3] = amplitude_, lerp_.to[3] = arg(args, count, 14);
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    nega_.tick();
    if (lerp_.on) {
      const bool end = lerp_.dur < ++lerp_.elapsed;
      for (int k = 0; k < 4; ++k) {
        const int value = end ? lerp_.to[k] : lerp_.from[k] + trunc_div((lerp_.to[k] - lerp_.from[k]) * lerp_.elapsed, lerp_.dur);
        if (k < 3) rate_[k] = value;
        else amplitude_ = value;
      }
      if (end) lerp_.on = false;
    }
    FxModel* model = target(0);
    if (!model) return;
    own_draw(model, true, false);
    // On release the model is shown again.
    model->unhide_at_end = true;
    // The vertex table of this frame (the particles' callbacks of the next tick paint it).
    FxSkinnedModel sk;
    if (skin(model, sk)) {
      verts_.resize(sk.verts.size());
      for (size_t i = 0; i < sk.verts.size(); ++i) {
        int vertex[3];
        for (int k = 0; k < 3; ++k) vertex[k] = static_cast<s16>(std::lround(sk.verts[i][static_cast<size_t>(k)]));
        wave(vertex);
        verts_[i] = {vertex[0], vertex[1], vertex[2]};
      }
    }
    colours_.assign(verts_.size(), {static_cast<u8>(model->rgb[0]), static_cast<u8>(model->rgb[1]), static_cast<u8>(model->rgb[2])});
    for (int k = 0; k < 3; ++k) phase_[k] = static_cast<s16>(phase_[k] + rate_[k]);
    painted_ = colours_;
  }
  void draw(std::vector<FxPrim>& out) const override {
    const size_t n0 = out.size();
    u32 order = 0;
    const FxModel* model = target(0);
    FxSkinnedModel sk;
    if (model && skin(model, sk))
      for (const FxSkinFace& face : sk.faces) {
        DrawVert corners[4];
        u8 rgb[4][3];
        bool any = false;
        for (int k = 0; k < face.n; ++k) {
          corners[k] = draw_vert(sk, face, k);
          const size_t vi = static_cast<size_t>(std::max(0, corners[k].idx));
          if (vi < verts_.size()) corners[k].x = verts_[vi][0], corners[k].y = verts_[vi][1], corners[k].z = verts_[vi][2];
          const std::array<u8, 3> col = vi < shown_.size() ? shown_[vi] : std::array<u8, 3>{0x80, 0x80, 0x80};
          for (int ch = 0; ch < 3; ++ch) rgb[k][ch] = col[static_cast<size_t>(ch)];
          any = any || col[0] || col[1] || col[2];
        }
        if (!any) continue;
        ProjPoly pp;
        if (!project_poly(ctx_.view, corners, face.n, 2, pp)) continue;
        const int slot = static_cast<int>(pp.z[face.n - 1]) >> 4;
        if (rgb[0][0] == 0x80) {
          push_poly(pp, corners, model->unit, face.tex, rgb, 0, 0, slot, order, out);
        } else {
          push_poly(pp, corners, model->unit, face.tex, rgb, 2, 0, slot, order, out);
          push_poly(pp, corners, model->unit, face.tex, rgb, 3, 1, slot, order, out);
        }
      }
    if (nega_.active()) nega_.draw(out);
    last_prims_ = out.size() - n0;
  }
  void tick() override {
    // The colours the particles painted during this tick's emitter step.
    shown_ = painted_;
    OverlayFx::tick();
  }
  bool done() const override { return !nega_.active() && !lerp_.on; }

 private:
  void wave(int vertex[3]) const {
    if (!(rate_[0] | rate_[1] | rate_[2])) return;
    const int x = vertex[0], z = vertex[2];
    vertex[0] = static_cast<s16>(x + ((amplitude_ * rsin(phase_[0] + (x << 1))) >> 12));
    vertex[1] = static_cast<s16>(vertex[1] + ((amplitude_ * rsin(phase_[1] + (z << 1))) >> 12));
    vertex[2] = static_cast<s16>(z + ((amplitude_ * rsin(phase_[2] + (x << 1))) >> 12));
  }
  void paint(EmitterSystem::ObjectRef& object) {
    if (!object.pos || !object.scale || !object.col) return;
    const int radius = object.scale[0] / 6;
    if (radius == 0) return;
    const int px = object.pos[0] >> 16, py = object.pos[1] >> 16, pz = object.pos[2] >> 16;
    for (size_t i = 0; i < verts_.size() && i < painted_.size(); ++i) {
      const int distance = dist3(verts_[i][0] - px, verts_[i][1] - py, verts_[i][2] - pz), inside = distance - radius;
      if (inside >= 0) continue;
      const int level = trunc_div(inside * 128, radius) + 128;
      if (level < painted_[i][0])
        for (int ch = 0; ch < 3; ++ch)
          painted_[i][static_cast<size_t>(ch)] = static_cast<u8>(trunc_div((static_cast<s16>(object.col[ch]) >> 4) * level, 128));
    }
  }

  struct Lerp {
    bool on = false;
    int elapsed = 0, dur = 0;
    int from[4] = {}, to[4] = {};
  } lerp_;
  std::vector<std::string> names_;
  std::vector<std::array<int, 3>> verts_;
  std::vector<std::array<u8, 3>> colours_, painted_, shown_;
  int rate_[3] = {0, 0, 0}, phase_[3] = {0, 0, 0}, amplitude_ = 0;
  util::NegativeStrip nega_;
};

}  // namespace

void register_death_ports(BespokeRegistry& registry) {
  registry.add({"garadeth", "death", "GaraDeth", [] { return std::make_unique<GaraDeth>(false); }});
  registry.add({"mushi", "death", "GaraDeth/mushi", [] { return std::make_unique<GaraDeth>(true); }});
  registry.add({"ki_deth", "death", "KiDeth", [] { return std::make_unique<KiDeth>(); }});
  registry.add({"drg_deth", "death", "DrgDeth", [] { return std::make_unique<DrgDeth>(); }});
  registry.add({"octdeth", "death", "OctDeth", [] { return std::make_unique<OctDeth>(); }});
  registry.add({"daimou_d", "death", "Daimou", [] { return std::make_unique<Daimou>(); }});
  for (const FbParams& params : kFb)
    registry.add({params.name, "death-dissolve", "Dissolve", [params] { return std::make_unique<Dissolve>(params); }});
  registry.add({"lasdes1", "death-dissolve", "Lasdes1", [] { return std::make_unique<Lasdes1>(); }});
  registry.add({"lasdes2", "death-dissolve", "Lasdes2", [] { return std::make_unique<Lasdes2>(); }});
}

}  // namespace oc::effects
