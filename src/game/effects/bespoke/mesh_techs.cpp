// Mesh-effect techs: overlays that re-skin a model every frame and draw it through their private copy of the model
// draw (model_draw.h), plus their shard tasks. Ports of the PS1 code.
//   jakotu   eid 22  caster 0, ring-coloured env pass over the model, env texture scrolled in VRAM
//   fnoise   eid 20  target 0, frost front (0x180) then the model breaks into spinning shards (0x181)
//   rakugaki eid 21  target 0, white sweep (0x181) then the planar doodle texture revealed tile by tile (0x180)
//   hensin   eid 11  caster 0 erased from the head down by a plane (0x180) with shards along the cut
//   ghost2   eids 302 306 309 310 527  the pack's pseudo-actor rises out of the ground (0x180: drawn clipped at y = 0)
//   miki     eid 17  copies the displayed frame to VRAM (0x1C0, 0x100) every frame
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "game/effects/bespoke/mesh_tech.h"
#include "game/effects/fx_math.h"

namespace oc::effects {
namespace {

int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }

// ---- jakotu ---------------------------------------------------------------------------------------------------------------------------
// Init: M = caster 0; centre = attach point 9 (x unused, y / z = the low halves). Frame: T += 1; from T >= 601
// (0x259) tA += 1 and the ring radius R runs a three-state machine (state 0: R = tA * 48000 >> 12 until R >= 881 (0x371), then state 1 with R
// frozen, tA = 0; state 1: 51 (0x33) ticks; state 2: R = tA * 88000 >> 12, no end). The model is posed at the normal speed, drawn by the
// overlay with part flag 0x40 (env packet = ring colour per vertex on the page 0x97 | ABR, CLUT 0x3AD0, UV
// = normal bytes ^ 0x80, over the base packet) and hidden from the stock pass. Ring colour: d = SquareRoot0(dy^2 + dz^2) with dy =
// cy - y, dz = cz - z; states 0 / 1: d < R/2 -> 0xC8, d < R -> 0x80, else 0; state 2: 0 / 0x80 / 0xC8. Every frame the env texture (VRAM
// x 0x1C0..0x23F, y 0x100..0x1FF) scrolls up 2 rows, cyclically (StoreImage / MoveImage / LoadImage). Exit: the stock draw back.
class Jakotu : public MeshTech {
 protected:
  void init() override { start_caster_anim(caster(0)); }
  void frame() override {
    FxModel* model = caster(0);
    if (!model) return;
    // The centre is taken on the first frame (the host's attach callback is installed after the overlay starts; same pose).
    if (!centred_) {
      int out[3];
      if (attach(model, 9, out)) cy_ = out[1], cz_ = out[2];
      centred_ = true;
    }
    if (++timer_ >= 0x259) {
      ++ring_tick_;
      if (state_ == 0) {
        radius_ = (ring_tick_ * 48000) >> 12;
        if (radius_ >= 0x371) state_ = 1, ring_tick_ = 0;
      } else if (state_ == 2) {
        radius_ = (ring_tick_ * 88000) >> 12;
      } else if (ring_tick_ >= 0x33) {
        state_ = 2, ring_tick_ = 0;
      }
    }
    if (ctx_.models) ctx_.models->vram_moves.push_back({0x1C0, 0x100, 0x80, 0x100, 0, 2, true});
    model->anim_speed = 0x40;
    own_draw(model, true);
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = caster(0);
    if (!model) return;
    ModelDrawSpec spec;
    spec.flags = 0x40;
    spec.env_tpage = 0x97;
    spec.env_clut = 0x3AD0;
    spec.env_uv = ModelDrawSpec::EnvUv::Xor80;
    const int radius = radius_, st = state_, cy = cy_, cz = cz_;
    spec.colour = [radius, st, cy, cz](const DrawVert& vertex, u8 rgb[3]) {
      const int dy = static_cast<s16>(cy - vertex.y), dz = static_cast<s16>(cz - vertex.z);
      const int dist = ps1_square_root0(dy * dy + dz * dz), half = radius >> 1;
      int level = 0;
      if (st == 0 || st == 1) level = dist < half ? 0xC8 : dist < radius ? 0x80 : 0;
      else if (st == 2) level = dist < half ? 0 : dist < radius ? 0x80 : 0xC8;
      rgb[0] = rgb[1] = rgb[2] = static_cast<u8>(level);
    };
    u32 order = 0;
    const size_t n0 = out.size();
    draw_model(model, spec, out, order);
    last_prims_ = out.size() - n0;
  }

 private:
  bool centred_ = false;
  int cy_ = 0, cz_ = 0, timer_ = 0, ring_tick_ = 0, state_ = 0, radius_ = 0;
};

// ---- fnoise ---------------------------------------------------------------------------------------------------------------------------
// Init: M = target 0; centre (cx, 0, cz) = attach point 9's x / z (y forced 0, x unused by the colour). 0x180: state 0x180.
// 0x181 (speed P = s16 @+8, gravity G = s16 @+10): state 0x181; the pose is stepped once, part flag 0x20 is set and one shard is built
// per face from the skinned model (the face's world corners, env UVs = normal bytes - 0x80 on the page 0x97 |
// ABR with CLUT 0x3890 (bit 0x20 makes every part env-mapped), velocity byte per axis = the mean of the corners' normal bytes (tri: sum / 3
// toward 0, quad: sum >> 2) x (rand % 0x400 + 0xC00) >> 12, axis = rand % 3, rand order vx vy vz axis); shard colour 0x64 grey.
// Frame: in 0x180 T += 1, R = T * 72000 >> 12 and the pose is frozen; states 0 / 0x180 draw the model with part flag
// 0x40 (env packet with the frost colour per vertex, d = SquareRoot0(dy^2 + dz^2) to the centre: d < R/2 -> 0x64, d < R -> 0x40, else
// 0; quads AVSZ4) and hide the stock pass. State 0x181: the colour falls 2 per tick (r < 0 -> black), every shard moves by (v * P >> 12) +
// (0, G, 0) and turns 100 / tick about its axis round its centroid, drawn as FT3 / FT4 additive both sides, OT AVSZ >> 2
// clamped; the model is not hidden by the overlay (stock draw).
class Fnoise : public MeshTech {
 protected:
  void init() override { start_caster_anim(caster(0)); }
  bool command(int op, const int* args, int count) override {
    if (op == 0x180) {
      state_ = 0x180;
    } else if (op == 0x181) {
      speed_ = static_cast<u16>(arg(args, count, 8));
      grav_ = static_cast<s16>(arg(args, count, 10));
      state_ = 0x181;
      build_shards();
      rgb_ = {0x64, 0x64, 0x64};
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* model = target(0);
    if (!model) return;
    if (!centred_) {
      int out[3];
      if (attach(model, 9, out)) cx_ = out[0], cz_ = out[2];
      centred_ = true;
    }
    if (state_ == 0x180) radius_ = (++timer_ * 72000) >> 12;
    model->anim_speed = state_ == 0x180 ? 0 : 0x40;
    if (state_ == 0x181) {
      const int red = rgb_[0] - 2;
      if (red < 0) rgb_ = {0, 0, 0};
      else rgb_ = {red, (rgb_[1] - 2) & 0xFF, (rgb_[2] - 2) & 0xFF};
      for (Shard& shard : shards_) step(shard);
      own_draw(model, false);
      // The stock draw stays on during the shatter.
      model->hidden = false;
    } else {
      own_draw(model, true);
    }
  }
  void draw(std::vector<FxPrim>& out) const override {
    const size_t n0 = out.size();
    u32 order = 0;
    if (state_ != 0x181) {
      const FxModel* model = target(0);
      if (!model) return;
      ModelDrawSpec spec;
      spec.flags = 0x40;
      spec.env_tpage = 0x97;
      spec.env_clut = 0x3890;
      // Quads use AVSZ4.
      spec.quad_avsz3 = false;
      const int radius = radius_, cz = cz_;
      // The centre's y is 0, x unused.
      spec.colour = [radius, cz](const DrawVert& vertex, u8 rgb[3]) {
        const int dy = static_cast<s16>(0 - vertex.y), dz = static_cast<s16>(cz - vertex.z);
        const int dist = ps1_square_root0(dy * dy + dz * dz), half = radius >> 1;
        const u8 level = static_cast<u8>(dist < half ? 0x64 : dist < radius ? 0x40 : 0);
        rgb[0] = rgb[1] = rgb[2] = level;
      };
      draw_model(model, spec, out, order);
    } else if (rgb_[0] | rgb_[1] | rgb_[2]) {
      for (const Shard& sh : shards_) draw_shard(sh, out, order);
    }
    last_prims_ = out.size() - n0;
  }

 private:
  struct Shard {
    int corner_count = 3;
    int verts[4][3] = {};
    u8 u[4] = {}, w[4] = {};
    u16 tpage = 0, clut = 0;
    int vel[3] = {0, 0, 0};
    int axis = 0;
  };
  void build_shards() {
    shards_.clear();
    const FxModel* model = target(0);
    FxSkinnedModel sk;
    if (!skin(model, sk)) return;
    Rng rng;
    rng.state = ctx_.seed ? ctx_.seed : 1;
    ModelDrawSpec spec;
    spec.flags = 0x20;
    for (const FxSkinFace& skin_face : sk.faces) {
      if (skin_face.part < 0 || skin_face.part >= sk.parts || skin_face.part >= 16) continue;
      u8 rgb[3], b3;
      model_colour_word(sk, *model, spec, skin_face.part, rgb, b3);
      if (b3 & 0x01) continue;
      Shard sh;
      sh.corner_count = skin_face.n;
      int sum[3] = {0, 0, 0};
      for (int k = 0; k < skin_face.n; ++k) {
        const DrawVert vert = draw_vert(sk, skin_face, k);
        sh.verts[k][0] = vert.x, sh.verts[k][1] = vert.y, sh.verts[k][2] = vert.z;
        sh.u[k] = static_cast<u8>((vert.nx - 0x80) & 0xFF);
        sh.w[k] = static_cast<u8>((vert.ny - 0x80) & 0xFF);
        sum[0] += vert.nx, sum[1] += vert.ny, sum[2] += vert.nz;
      }
      sh.clut = 0x3890;
      sh.tpage = static_cast<u16>(((b3 & 0xC) << 3) | 0x97);
      for (int k = 0; k < 3; ++k) {
        const int mean = skin_face.n == 3 ? sum[k] / 3 : sum[k] >> 2;
        const int jitter = rng.next() % 0x400 + 0xC00;
        sh.vel[k] = static_cast<s8>(static_cast<s8>(mean) * jitter >> 12);
      }
      sh.axis = rng.next() % 3;
      shards_.push_back(sh);
    }
  }
  // One tick of shard motion plus rotation.
  void step(Shard& shard) const {
    const int speed = speed_, gravity = grav_;
    int delta[3];
    for (int k = 0; k < 3; ++k) delta[k] = static_cast<s16>((static_cast<s16>(shard.vel[k]) * speed) >> 12);
    delta[1] = static_cast<s16>(delta[1] + gravity);
    int centroid[3] = {0, 0, 0};
    for (int i = 0; i < shard.corner_count; ++i)
      for (int k = 0; k < 3; ++k) shard.verts[i][k] = static_cast<s16>(shard.verts[i][k] + delta[k]), centroid[k] += shard.verts[i][k];
    for (int k = 0; k < 3; ++k) centroid[k] = shard.corner_count == 3 ? static_cast<s16>(centroid[k]) / 3 : static_cast<s16>(centroid[k]) >> 2;
    const int sn = rsin(100), cs = rcos(100);
    for (int i = 0; i < shard.corner_count; ++i) {
      const int x = shard.verts[i][0] - centroid[0], y = shard.verts[i][1] - centroid[1], z = shard.verts[i][2] - centroid[2];
      int nx = x, ny = y, nz = z;
      if (shard.axis == 0) ny = (y * cs - z * sn) >> 12, nz = (y * sn + z * cs) >> 12;
      else if (shard.axis == 1) nx = (z * sn + x * cs) >> 12, nz = (z * cs - x * sn) >> 12;
      else nx = (x * cs - y * sn) >> 12, ny = (x * sn + y * cs) >> 12;
      shard.verts[i][0] = static_cast<s16>(nx + centroid[0]), shard.verts[i][1] = static_cast<s16>(ny + centroid[1]), shard.verts[i][2] = static_cast<s16>(nz + centroid[2]);
    }
  }
  // FT3 0x26 / FT4 0x2E, no NCLIP.
  void draw_shard(const Shard& shard, std::vector<FxPrim>& out, u32& order) const {
    FxPrim prim;
    prim.kind = shard.corner_count == 3 ? FxPrim::Kind::Tri : FxPrim::Kind::Quad;
    prim.src = FxPrim::TexSrc::Vram;
    prim.tpage = static_cast<u16>((shard.tpage & 0xFF9F) | 0x20);
    prim.clut = shard.clut;
    // ABR 1.
    prim.blend = 2;
    float z = 0;
    for (int k = 0; k < shard.corner_count; ++k) {
      float sx, sy, sz;
      if (!fx_project(ctx_.view, {static_cast<float>(shard.verts[k][0]), static_cast<float>(shard.verts[k][1]), static_cast<float>(shard.verts[k][2])}, sx, sy, sz)) return;
      prim.v[k] = {sx, sy, static_cast<u8>(rgb_[0]), static_cast<u8>(rgb_[1]), static_cast<u8>(rgb_[2])};
      prim.u[k] = shard.u[k];
      prim.w[k] = shard.w[k];
      z += sz;
    }
    prim.slot = std::clamp(static_cast<int>(z / static_cast<float>(shard.corner_count)) >> 4, 1, 0xFFF);
    prim.order = order++;
    out.push_back(prim);
  }

  bool centred_ = false;
  int cx_ = 0, cz_ = 0, timer_ = 0, radius_ = 0, state_ = 0, speed_ = 0, grav_ = 0;
  std::array<int, 3> rgb_{0x64, 0x64, 0x64};
  std::vector<Shard> shards_;
};

// ---- rakugaki -------------------------------------------------------------------------------------------------------------------------
// M = target 0, its pose frozen for the whole effect. 0x181 (D = s16 @+4): y0 = attach point 0xE's world
// y, rate = (-y0 << 12) / D; every frame thr = y0 + (rate * t >> 12) and the model is drawn with part flag 0x40: env packet = 0xFF where the
// vertex y < thr else 0 on the doodle page 0x9B | ABR (VRAM (704, 256), CLUT 0x3B90) with planar UVs u = (z - minZ) * 0x7F / rangeZ,
// v = (y - minY) * 0x7F / rangeY (over the box of every skinned vertex), additive over the base packet; never ends by itself.
// 0x180 (D = s16 @+4): perm[256] = identity shuffled by 1024 swaps of (rand % 256, rand % 256); for D ticks the model is drawn with part flag
// 0x20 only (one opaque packet, colour 0x80, the planar UVs on the doodle page) while each tick one 8 x 8 texel tile perm[n] (n = 0..255, never
// reset) is copied from (0x280 + tx, 0x100 + ty) to (0x2C0 + tx, 0x100 + ty), tx = (i & 15) * 4, ty = (i >> 4) * 8:
// the doodle appears tile by tile. Afterwards the stock draw is back.
class Rakugaki : public MeshTech {
 protected:
  void init() override { start_caster_anim(caster(0)); }
  bool command(int op, const int* args, int count) override {
    if (op == 0x181) {
      state_ = 0x181, tick_ = 0, duration_ = arg(args, count, 4);
      int out[3] = {0, 0, 0};
      attach(target(0), 0xE, out);
      y0_ = out[1];
      rate_ = trunc_div(-y0_ * 4096, duration_);
    } else if (op == 0x180) {
      state_ = 0x180, duration_ = arg(args, count, 4), tick_ = 0;
      for (int i = 0; i < 256; ++i) perm_[static_cast<size_t>(i)] = static_cast<u16>(i);
      Rng rng;
      rng.state = ctx_.seed ? ctx_.seed : 1;
      for (int i = 0; i < 1024; ++i) {
        const int first = rng.next() % 256, second = rng.next() % 256;
        std::swap(perm_[static_cast<size_t>(first)], perm_[static_cast<size_t>(second)]);
      }
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* model = target(0);
    if (!model) return;
    model->anim_speed = 0;
    draw_flags_ = 0;
    if (state_ == 0x180) {
      if (duration_ < ++tick_) state_ = 0;
      if (tc_ < 0x100 && ctx_.models) {
        const int i = perm_[static_cast<size_t>(tc_++)], tx = (i & 15) * 4, ty = (i >> 4) * 8;
        ctx_.models->vram_moves.push_back({0x280 + tx, 0x100 + ty, 4, 8, 0x2C0 | tx, 0x100 + ty, false});
      }
      draw_flags_ = 0x20;
    } else if (state_ == 0x181) {
      thr_ = y0_ + ((rate_ * ++tick_) >> 12);
      draw_flags_ = 0x40;
    }
    own_draw(model, draw_flags_ != 0);
  }
  void draw(std::vector<FxPrim>& out) const override {
    const FxModel* model = target(0);
    if (!model || !draw_flags_) return;
    FxSkinnedModel sk;
    if (!skin(model, sk) || sk.verts.empty()) return;
    int mn[3], mx[3];
    for (int k = 0; k < 3; ++k) mn[k] = mx[k] = static_cast<s16>(std::lround(sk.verts[0][static_cast<size_t>(k)]));
    for (const Vec3& vertex : sk.verts)
      for (int k = 0; k < 3; ++k) {
        const int coord = static_cast<s16>(std::lround(vertex[static_cast<size_t>(k)]));
        mn[k] = std::min(mn[k], coord), mx[k] = std::max(mx[k], coord);
      }
    const int ry = static_cast<s16>(mx[1] - mn[1]), rz = static_cast<s16>(mx[2] - mn[2]), miny = mn[1], minz = mn[2];
    ModelDrawSpec spec;
    spec.flags = static_cast<u16>(draw_flags_);
    spec.env_tpage = 0x9B;
    spec.env_clut = 0x3B90;
    spec.env_uv = ModelDrawSpec::EnvUv::Custom;
    spec.env_uv_fn = [ry, rz, miny, minz](const DrawVert& vertex, u8 uv[2]) {
      uv[0] = static_cast<u8>(trunc_div((vertex.z - minz) * 0x7F, rz));
      uv[1] = static_cast<u8>(trunc_div((vertex.y - miny) * 0x7F, ry));
    };
    const int st = state_ == 0x181 ? 0x181 : 0, thr = thr_;
    spec.colour = [st, thr](const DrawVert& vertex, u8 rgb[3]) {
      const u8 level = static_cast<u8>(st == 0x181 ? (vertex.y < thr ? 0xFF : 0) : 0x80);
      rgb[0] = rgb[1] = rgb[2] = level;
    };
    u32 order = 0;
    const size_t n0 = out.size();
    model_draw(sk, *model, spec, ctx_.view, out, order);
    last_prims_ = out.size() - n0;
  }

 private:
  int state_ = 0, tick_ = 0, duration_ = 1, y0_ = 0, rate_ = 0, thr_ = 0, tc_ = 0, draw_flags_ = 0;
  std::array<u16, 256> perm_{};
};

// ---- ghost2 ---------------------------------------------------------------------------------------------------------------------------
// Init: the pack's 'act0' model (type 0x14, tpage (448, 256), CLUT (256, 226), its own anmseq entry 0) is the host's pseudo-actor; every
// caster plays entry 0 of the pack's 0x1A table ("main"), and the pseudo-actor (acting-list entry -1) is hidden from the stock pass. 0x180
// (D = s16 @+4): for D ticks the overlay draws the pseudo-actor itself clipped at the world plane y = 0, keeping the y <= 0 side (skip side 1;
// byte 3 = the part flag byte, GT3 depth max SZ for flag 0x10), plus its shadow; when the timer passes D it is shown to the stock draw.
// Eids 310 / 527 never send 0x180: the pseudo-actor stays hidden until its own script shows it. The casters are never hidden.
class Ghost2 : public MeshTech {
 protected:
  void init() override {
    for (int i = 0; i < static_cast<int>(ctx_.models ? ctx_.models->casters.size() : 0); ++i) start_caster_anim(caster(i));
    if (ctx_.models) ctx_.models->pseudo_hide = 1;
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x180) return false;
    state_ = 0x180, timer_ = 0, limit_ = arg(args, count, 4);
    return true;
  }
  void frame() override {
    if (!ctx_.models || state_ != 0x180) return;
    if (limit_ < ++timer_) {
      state_ = 0;
      ctx_.models->pseudo_hide = 0;
      ctx_.models->pseudo_own_shadow = false;
      return;
    }
    ctx_.models->pseudo_hide = 1;
    ctx_.models->pseudo_own_shadow = true;
  }
  void draw(std::vector<FxPrim>& out) const override {
    if (state_ != 0x180) return;
    FxModel model;
    model.unit = kPseudoUnit;
    FxSkinnedModel sk;
    if (!skin(&model, sk)) return;
    for (int k = 0; k < 3; ++k) model.rgb[k] = sk.rgb[k];
    ModelDrawSpec spec;
    spec.mask_part_flags = false;
    spec.tri_far_z = true;
    spec.env_uv = ModelDrawSpec::EnvUv::Half;
    spec.env_tpage = 0x95;
    spec.env_clut = 0x3BC0;
    spec.clip = true;
    spec.plane = 0;
    spec.skip[0] = false, spec.skip[1] = true;
    u32 order = 0;
    const size_t n0 = out.size();
    model_draw(sk, model, spec, ctx_.view, out, order);
    last_prims_ = out.size() - n0;
  }

 private:
  int state_ = 0, timer_ = 0, limit_ = 0;
};

// ---- hensin ---------------------------------------------------------------------------------------------------------------------------
// Init: H1 = -(attach point 0xE's world y) of caster 0 (the head top); the loader task brings the new form's model (file group 0xE,
// id * 3 + 2 TIM to (0x1C0, 0x100) / CLUT (0x100, 0xEA), id * 3 + 3 model). 0x180 / 0x181 (D = s16 @+4): state, t = 0. Frame:
// 0x180: t += 1, s = t * H1 / D, the caster's pose frozen, plane P = s - H1; the overlay draws the caster clipped at y = P
// keeping the y > P side (skip side 0; the part from the plane down, so it vanishes from the head down; byte 3 = the part flag byte)
// and runs the cutter (mode 0): every polygon the plane cuts spawns a shard task with the first two intersections, drift
// ((rand % 3 - 1) * 3, -1, (rand % 3 - 1) * 3), counter 0 -> 10. Shard: c += 1 (mode 0; the task is freed at c >= 10 after that
// frame's draw), A = I0, B = I1, C = I0 + (5, 5, 5) turned about their midpoint in the XY plane by c * 256, + drift * c; FT3 (I0's UV, I1's UV
// twice) on the part's texture, grey (144 - 14 c) & 0xFF, additive, both sides, OT AVSZ3. 0x183: the caster shown again, state 0.
// Not reproduced: 0x181 (the new form revealed from the feet up at y >= 100 - s, shards at -s) and 0x182 / 0x183's battle unit replacement:
// OpenCross has no unit swap; the type-0x13 ground objects' skip flag is cleared.
class Hensin : public MeshTech {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op < 0x180 || op > 0x183) return false;
    if (op == 0x180 || op == 0x181) state_ = op, tick_ = 0, duration_ = arg(args, count, 4);
    if (op == 0x181) {
      // The loader brought the new form's model into the overlay's own block; the port's unit swap (the host's
      // Battle::transform + the model rebind) happens HERE instead of at 0x183, so the unit's own model is the new form while 0x181
      // reveals it, and the overlay's clip / cutter work on it like on the old one.
      if (ctx_.hooks.form_swap) ctx_.hooks.form_swap(1);
      measured_ = false;
      // The shard tasks count down.
      mode_ = 1;
    }
    // Op 0x182: nothing is left to do once the swap is through.
    if (op == 0x182 && ctx_.hooks.form_swap) ctx_.hooks.form_swap(2);
    if (op == 0x183) {
      own_draw(caster(0), false);
      state_ = 0;
      if (FxModel* model = caster(0)) {
        // The caster is shown again.
        model->hidden = false;
        model->anim_speed = -1;
      }
      // The engine's own build of the unit's new model (the fallback of a swap that did not happen yet).
      if (ctx_.hooks.form_swap) ctx_.hooks.form_swap(2);
    }
    return true;
  }
  void frame() override {
    FxModel* model = caster(0);
    if (!model) return;
    if (!measured_) {
      int out[3];
      if (attach(model, 0xE, out)) h1_ = -out[1];
      measured_ = true;
    }
    if (state_ == 0x180) {
      ++tick_;
      const int sweep = trunc_div(tick_ * h1_, duration_);
      model->anim_speed = 0;
      plane_ = sweep - h1_;
      mode_ = 0;
      FxSkinnedModel sk;
      if (skin(model, sk)) {
        ModelDrawSpec cs;
        cs.plane = plane_;
        Rng& rng = rng_;
        model_plane_cuts(sk, cs, [this, &rng](const PlaneCut& cut) {
          Shard sh;
          sh.p0 = cut.first, sh.p1 = cut.second, sh.tex = cut.tex;
          sh.drift[0] = (rng.next() % 3 - 1) * 3;
          sh.drift[1] = -1;
          sh.drift[2] = (rng.next() % 3 - 1) * 3;
          sh.counter = 0, sh.limit = 10;
          shards_.push_back(sh);
        });
      }
      own_draw(model, true);
    } else if (state_ == 0x181) {
      // State 0x181: t = tick * H1' / D (H1' = -(attach 0xE y) of the NEW model), the new form posed at speed 0 and drawn clipped at P = 100 - t keeping
      // y > P (revealed from the feet up), the cutter at -t in mode 1 (shards count down from 10).
      ++tick_;
      const int sweep = trunc_div(tick_ * h1_, duration_);
      model->anim_speed = 0;
      plane_ = 100 - sweep;
      mode_ = 1;
      FxSkinnedModel sk;
      if (skin(model, sk)) {
        ModelDrawSpec cs;
        cs.plane = -sweep;
        Rng& rng = rng_;
        model_plane_cuts(sk, cs, [this, &rng](const PlaneCut& cut) {
          Shard sh;
          sh.p0 = cut.first, sh.p1 = cut.second, sh.tex = cut.tex;
          sh.drift[0] = (rng.next() % 3 - 1) * 3;
          sh.drift[1] = -1;
          sh.drift[2] = (rng.next() % 3 - 1) * 3;
          sh.counter = 10, sh.limit = 0;
          shards_.push_back(sh);
        });
      }
      own_draw(model, true);
    }
    // Shard tasks: count with the current global mode, freed after the frame they pass the limit (still drawn that frame)
    for (Shard& sh : shards_) {
      if (sh.dead) continue;
      if (mode_ == 0) sh.done = ++sh.counter >= sh.limit;
      else sh.done = --sh.counter < 0;
    }
  }
  void draw(std::vector<FxPrim>& out) const override {
    const size_t n0 = out.size();
    u32 order = 0;
    const FxModel* model = caster(0);
    if (model && (state_ == 0x180 || state_ == 0x181)) {
      ModelDrawSpec spec;
      spec.mask_part_flags = false;
      spec.tri_far_z = true;
      spec.env_uv = ModelDrawSpec::EnvUv::Half;
      spec.env_tpage = 0x95;
      spec.env_clut = 0x3BC0;
      spec.clip = true;
      spec.plane = plane_;
      spec.skip[0] = true, spec.skip[1] = false;
      draw_model(model, spec, out, order);
    }
    for (const Shard& sh : shards_)
      if (!sh.dead && model) draw_shard(sh, *model, out, order);
    last_prims_ = out.size() - n0;
  }
  bool done() const override { return state_ == 0 && std::all_of(shards_.begin(), shards_.end(), [](const Shard& shard) { return shard.dead || shard.done; }); }
  void tick() override {
    for (Shard& sh : shards_) {
      // Freed after the frame it was last drawn.
      if (sh.done) sh.dead = true;
    }
    shards_.erase(std::remove_if(shards_.begin(), shards_.end(), [](const Shard& shard) { return shard.dead; }), shards_.end());
    OverlayFx::tick();
  }

 private:
  struct Shard {
    DrawVert p0, p1;
    int tex = 0;
    int drift[3] = {0, 0, 0};
    int counter = 0, limit = 10;
    bool done = false, dead = false;
  };
  void draw_shard(const Shard& sh, const FxModel& model, std::vector<FxPrim>& out, u32& order) const {
    const int mid[3] = {(sh.p0.x + sh.p1.x) >> 1, (sh.p0.y + sh.p1.y) >> 1, (sh.p0.z + sh.p1.z) >> 1};
    const int ang = static_cast<s16>(sh.counter << 8);
    const int cs = rcos(ang), sn = rsin(ang);
    const int src[3][3] = {{sh.p0.x, sh.p0.y, sh.p0.z}, {sh.p1.x, sh.p1.y, sh.p1.z}, {sh.p0.x + 5, sh.p0.y + 5, sh.p0.z + 5}};
    FxPrim prim;
    prim.kind = FxPrim::Kind::Tri;
    prim.src = FxPrim::TexSrc::Model;
    prim.unit = model.unit;
    prim.tex = sh.tex;
    // SetSemiTrans(1), tpage ABR 1.
    prim.blend = 2;
    const u8 col = static_cast<u8>((sh.counter * -0xE - 0x70) & 0xFF);
    const float uu[3] = {sh.p0.u, sh.p1.u, sh.p1.u}, ww[3] = {sh.p0.w, sh.p1.w, sh.p1.w};
    float z = 0;
    for (int k = 0; k < 3; ++k) {
      const int dx = src[k][0] - mid[0], dy = src[k][1] - mid[1];
      const int x = static_cast<s16>(((dx * cs - dy * sn) >> 12) + mid[0] + sh.drift[0] * sh.counter);
      const int y = static_cast<s16>(((dx * sn + dy * cs) >> 12) + mid[1] + sh.drift[1] * sh.counter);
      const int zz = static_cast<s16>(src[k][2] + sh.drift[2] * sh.counter);
      float sx, sy, sz;
      if (!fx_project(ctx_.view, {static_cast<float>(x), static_cast<float>(y), static_cast<float>(zz)}, sx, sy, sz)) return;
      prim.v[k] = {sx, sy, col, col, col};
      prim.u[k] = uu[k];
      prim.w[k] = ww[k];
      z += sz;
    }
    prim.slot = std::clamp(static_cast<int>(z / 3.0f) >> 4, 1, 0xFFF);
    prim.order = order++;
    out.push_back(prim);
  }

  bool measured_ = false;
  int h1_ = 0, state_ = 0, tick_ = 0, duration_ = 1, plane_ = 0, mode_ = 0;
  Rng rng_{};
  std::vector<Shard> shards_;
};

// ---- miki -----------------------------------------------------------------------------------------------------------------------------
// Init: caster 0 plays entry 0 of the pack's 0x1A table. Frame: the displayed frame is copied to VRAM
// (0x1C0, 0x100) at OT entry 50 - the page the haze meshes grid / gnr0 / gnr1 of sexy2.prd sample. Draws nothing.
// The capture request is posted every frame (FxModels::capture); the battle host copies the picture and the meshes on that page sample it.
class Miki : public MeshTech {
 protected:
  void init() override { start_caster_anim(caster(0)); }
  void frame() override {
    if (ctx_.models) ctx_.models->capture = {0x1C0, 0x100, 50};
  }
};

// ---- trap summons: tfleaf tgfrog tgolem tunico --------------------------------------------------------------------------------------
// One module (the four tasks are identical but for the animation speed K). 0x100 (D @+4, a @+8, b @+10): a
// task on the pack's own model M (the type-0x14 resource = the drp's type-0x0B Mdl that the sprite-type-20 objects
// draw; they copy their place, angles, scale and colour into it every tick), then the 'trap' sequence starts when the pack has
// one (tgfrog). tfleaf first stops 'wlf2' and kills its particles; tgfrog stops and kills every own emitter and every sequence but
// 'main' and its camera tasks. Each tick (t += 1; the task ends when D < t, M's position := the jar C, M stays hidden):
// M is re-skinned in place, every WORLD vertex above the plane THR = 1.5a t / D - 1.5a + Ym + a/2 (Ym = M's y) is pulled into C with a hop:
// e = (y + a + Ym) D / 1.5a, k = (t - e) 0x400 / (D - e), x += dx rcos(k + 0x400) >> 12 (toward zero), y += dy rcos(k + 0x400) + rcos(2k +
// 0x400) b, z likewise (d = v - C) - and M is drawn by the stock draw, after which the hidden flag hides it from the objects' draws for good.
// C = (0, 0, 0), tgolem (-8960, -1664, -18880). The task's extra pose step (K = 4 / 32 / 64 / 64) advances M's one frame counter by K / 256
// keyframes per tick for t = 1..D on top of the objects' own rate steps (the op-49 rate does the same): FxModels::pack_extra_frames,
// added by the host when it skins the pack model. Not reproduced: tgfrog's camera-task kill.
class Trap : public MeshTech {
 public:
  Trap(int cx, int cy, int cz, const char* stop, bool frog, bool remap = false, int speed = 64) : stop_(stop), frog_(frog), remap_(remap), speed_(speed) {
    centre_[0] = cx, centre_[1] = cy, centre_[2] = cz;
  }

 protected:
  // tunico's init installs unico's getters: the caster list = the unit nearest to caster 0's side point, the targets frozen.
  void init() override {
    if (!remap_ || !ctx_.models) return;
    ctx_.models->caster_list = fx_nearest_to_side(*ctx_.models, caster(0));
    ctx_.models->target_list = ctx_.models->targets;
    ctx_.models->caster_override = ctx_.models->target_override = true;
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x100) return false;
    on_ = true;
    tick_ = 0, duration_ = arg(args, count, 4), reach_ = arg(args, count, 8), bulge_ = arg(args, count, 10);
    if (em() && stop_) em()->kill(stop_);
    if (frog_) {
      for (size_t i = 0; em() && i < em()->header_count(); ++i)
        if (own_header(i)) em()->kill(em()->header_def(i).name);
      if (ctx_.sequences && ctx_.pack_fx)
        for (const auto& sq : ctx_.pack_fx->sequences)
          if (sq.name != "main") ctx_.sequences->stop(sq.name);
    }
    if (ctx_.sequences && ctx_.pack_fx && ctx_.pack_fx->find_sequence("trap")) ctx_.sequences->start("trap");
    return true;
  }
  void frame() override {
    if (!on_) return;
    if (duration_ < static_cast<s16>(++tick_)) {
      on_ = false;
      return;
    }
    if (ctx_.models) {
      ctx_.models->pack_model_hidden = true;
      // The extra pose step.
      ctx_.models->pack_extra_frames += static_cast<float>(speed_) / 256.0f;
    }
  }
  void draw(std::vector<FxPrim>& out) const override {
    if (!on_ || !ctx_.models || !ctx_.models->skin_pack) return;
    FxSkinnedModel sk;
    if (!ctx_.models->skin_pack(sk)) return;
    FxModel model;
    for (int k = 0; k < 3; ++k) model.rgb[k] = sk.rgb[k];
    ModelDrawSpec spec;
    spec.mask_part_flags = false;
    spec.pack_model = true;
    const int reach = reach_, bulge = bulge_, tick = static_cast<s16>(tick_), duration = duration_, half = reach / 2, A15 = reach + half,
              Ym = static_cast<int>(std::lround(sk.pos[1]));
    const int thr = trunc_div(A15 * tick, duration) - A15 + Ym + half;
    const int cx = centre_[0], cy = centre_[1], cz = centre_[2];
    auto shift_round = [](int value) { return (value < 0 ? value + 0xFFF : value) >> 12; };
    spec.face_hook = [=](DrawVert* corners, int count, int, int, int) {
      for (int k = 0; k < count; ++k) {
        DrawVert& vert = corners[k];
        if (vert.y >= thr) continue;
        const int dx = vert.x - cx, dy = vert.y - cy, dz = vert.z - cz;
        const int tau = trunc_div((vert.y + reach + Ym) * duration, A15);
        const int kk = trunc_div((tick - tau) * 0x400, duration - tau);
        const int c1 = rcos(kk + 0x400), c2 = rcos(2 * kk + 0x400);
        vert.x = static_cast<s16>(vert.x + shift_round(dx * c1));
        vert.y = static_cast<s16>(vert.y + shift_round(dy * c1) + shift_round(c2 * bulge));
        vert.z = static_cast<s16>(vert.z + shift_round(dz * c1));
      }
      return true;
    };
    u32 order = 0;
    const size_t n0 = out.size();
    model_draw(sk, model, spec, ctx_.view, out, order);
    last_prims_ = out.size() - n0;
  }
  bool done() const override { return !on_; }

 private:
  const char* stop_;
  bool frog_, remap_;
  // The task's anim speed (K = dt * 4 / 32 / 64 / 64).
  int speed_;
  int centre_[3];
  bool on_ = false;
  int tick_ = 0, duration_ = 0, reach_ = 0, bulge_ = 0;
};

}  // namespace

void register_mesh_techs(BespokeRegistry& registry) {
  registry.add({"jakotu", "mesh-tech", "Jakotu", [] { return std::make_unique<Jakotu>(); }});
  registry.add({"fnoise", "mesh-tech", "Fnoise", [] { return std::make_unique<Fnoise>(); }});
  registry.add({"rakugaki", "mesh-tech", "Rakugaki", [] { return std::make_unique<Rakugaki>(); }});
  registry.add({"ghost2", "mesh-tech", "Ghost2", [] { return std::make_unique<Ghost2>(); }});
  registry.add({"hensin", "mesh-tech", "Hensin", [] { return std::make_unique<Hensin>(); }});
  registry.add({"miki", "mesh-tech", "Miki", [] { return std::make_unique<Miki>(); }});
  registry.add({"tfleaf", "trap", "Trap", [] { return std::make_unique<Trap>(0, 0, 0, "wlf2", false, false, 4); }});
  registry.add({"tgfrog", "trap", "Trap", [] { return std::make_unique<Trap>(0, 0, 0, nullptr, true, false, 32); }});
  registry.add({"tgolem", "trap", "Trap", [] { return std::make_unique<Trap>(-8960, -1664, -18880, nullptr, false); }});
  registry.add({"tunico", "trap", "Trap", [] { return std::make_unique<Trap>(0, 0, 0, nullptr, false, true); }});
}

}  // namespace oc::effects
