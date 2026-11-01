// Sweep / gate archetypes: the model is re-emitted every tick with its vertices displaced or
// brightness-gated by a front that travels through it.
//   SweepCollapse  tfleaf, tgfrog, tgolem, tunico (summon deformers, identical task), cook / swordbox (BAKYUMU vacuum), 45infe:
//                  a front crosses the model top-down (45infe: bottom-up); behind it each vertex is pulled to the pivot C:
//                  u = (Y + A) * L / (1.5 A) is the tick the front reaches the vertex, s = (t - u) * 0x400 / (L - u),
//                  X' = X + (X - Cx) * F1, Y' = Y + (Y - Cy) * F1 + F2 * K with F1 = rcos(s + 0x400) / 4096, F2 = rcos(2 s + 0x400) / 4096
//                  (the cosine is inferred from the shape of the motion). 45infe uses the linear
//                  X' = X - (X - Cx) * k / D, k = min(c + Y / 8, D).
//   WaveRise       44get: X' = X + sin(phase + (Y << shift)) * amp, phase += 0x80 / tick, amp += rate / tick, the model rises and its rgb is
//                  scaled by (D - t) / D.
//   GateFx         mushi / garadeth (plane or band gate on Y, ops 0x181 / 0x182), hensin / ghost2 / octdeth / ki_deth (sweeping plane, CLUT
//                  grey lerp, white-out), jakotu (expanding ring gate with 0xC8 / 0x80 / 0 levels).
#include <algorithm>
#include <cmath>

#include "game/effects/bespoke/mesh_base.h"

namespace oc::effects {
namespace {

using util::add;
using util::len;
using util::mul;
using util::opnd;
using util::sub;

// ---- SweepCollapse -----------------------------------------------------------------------------------------------------------------
struct SweepParams {
  const char* name;
  bool cosine;     // false = linear (45infe)
  bool top_down;   // false = bottom-first
  float lift;      // K as a fraction of the model height
  bool strip;      // cook / swordbox also run the BAKYUMU screen negative (op 0x101)
  int default_len; // L in ticks
};

class SweepCollapse : public MeshFx {
 public:
  explicit SweepCollapse(const SweepParams& params) : params_(params) {}

  bool command(int op, const int* args, int arg_count) override {
    if (op == 0x100) {
      duration_ = std::max(1, opnd(args, arg_count, 0, duration_));
      if (arg_count > 2) lift_ = static_cast<float>(opnd(args, arg_count, 2, 0));
      return true;
    }
    if (op == 0x101 && params_.strip) {
      strip_.start(opnd(args, arg_count, 0, 10));
      return true;
    }
    return false;
  }
  bool done() const override { return tick_ > duration_ + 4 && !strip_.active(); }

 protected:
  int default_duration() const override { return params_.default_len; }
  void begin() override {
    height_ = std::max(1.0f, ymax_ - ymin_);
    lift_ = params_.lift * height_;
    pivot_ = mesh_ ? mesh_->centre : Vec3{0, 0, 0};
    strip_at_ = params_.strip ? duration_ / 2 : -1;
    step();
  }
  void step() override {
    strip_.tick();
    if (tick_ == strip_at_) strip_.start(10);
    if (!mesh_) return;
    const float span = static_cast<float>(duration_);
    for (size_t i = 0; i < mesh_->verts.size(); ++i) {
      const Vec3& vertex = mesh_->verts[i];
      // -height (top) .. 0 (feet)
      const float yrel = vertex[1] - ymax_;
      float reach;
      if (params_.cosine) reach = (params_.top_down ? (yrel + height_) : (-yrel)) * span / (1.5f * height_);
      else reach = (-yrel) / height_ * 0.5f * span;
      pos_[i] = vertex;
      const float tt = static_cast<float>(tick_);
      if (tt <= reach) continue;
      if (params_.cosine) {
        const int progress = std::clamp(static_cast<int>((tt - reach) * 1024.0f / std::max(1.0f, span - reach)), 0, 1024);
        const float f1 = static_cast<float>(rcos(progress + 0x400)) / 4096.0f, f2 = static_cast<float>(rcos(2 * progress + 0x400)) / 4096.0f;
        pos_[i] = {vertex[0] + (vertex[0] - pivot_[0]) * f1, vertex[1] + (vertex[1] - pivot_[1]) * f1 + f2 * lift_, vertex[2] + (vertex[2] - pivot_[2]) * f1};
      } else {
        const float pull = std::clamp((tt - reach) / span, 0.0f, 1.0f);
        pos_[i] = sub(vertex, mul(sub(vertex, pivot_), pull));
      }
    }
    finish_step();
  }
  void extra(std::vector<FxPrim>& out) const override { strip_.draw(out); }

 private:
  SweepParams params_;
  float height_ = 1, lift_ = 0;
  Vec3 pivot_{0, 0, 0};
  util::NegativeStrip strip_;
  int strip_at_ = -1;
};

// ---- WaveRise (44get) ---------------------------------------------------------------------------------------------------------------
class WaveRise : public MeshFx {
 public:
  bool command(int op, const int* args, int arg_count) override {
    if (op != 0x181) return false;
    shift_ = opnd(args, arg_count, 0, shift_);
    amp_rate_ = static_cast<float>(opnd(args, arg_count, 1, 1));
    return true;
  }
  bool done() const override { return tick_ >= duration_; }

 protected:
  int default_duration() const override { return 80; }
  void begin() override {
    height_ = std::max(1.0f, ymax_ - ymin_);
    step();
  }
  void step() override {
    if (!mesh_) return;
    const float tt = static_cast<float>(std::min(tick_, duration_));
    const float amp = std::min(28.0f, amp_rate_ * tt * 0.6f);
    const float fade = (static_cast<float>(duration_) - tt) / static_cast<float>(duration_);
    for (size_t i = 0; i < mesh_->verts.size(); ++i) {
      const Vec3& vertex = mesh_->verts[i];
      const int ang = static_cast<int>(tt * 128.0f) + (static_cast<int>(vertex[1]) << shift_);
      pos_[i] = {vertex[0] + static_cast<float>(rsin(ang)) / 4096.0f * amp, vertex[1] - tt * height_ * 0.012f, vertex[2]};
      col_[i] = {util::clamp8(static_cast<float>(mesh_->colours[i][0]) * fade), util::clamp8(static_cast<float>(mesh_->colours[i][1]) * fade),
                 util::clamp8(static_cast<float>(mesh_->colours[i][2]) * fade)};
    }
    blend_ = fade > 0.9f ? 0 : 2;
    finish_step();
  }

 private:
  int shift_ = 2;
  float amp_rate_ = 1, height_ = 1;
};

// ---- GateFx ---------------------------------------------------------------------------------------------------------------------------
enum class Gate { PlaneUp, Band, SweepReveal, SweepHide, Ring, Ghost };

struct GateParams {
  const char* name;
  Gate gate;
  bool desaturate;  // octdeth / ki_deth: CLUT lerp to grey first (0x180), then the sweep (0x182)
  bool whiteout;    // ki_deth: every non-zero CLUT entry becomes white for the sweep
  bool sparkles;    // octdeth: 'hika' / 'kona' particles at the front
  int default_len;
};

class GateFx : public MeshFx {
 public:
  explicit GateFx(const GateParams& params) : params_(params) {}

  bool command(int op, const int* args, int arg_count) override {
    // mushi / garadeth mode 1: falling plane at `rate`; mode 2: growing band.
    if (op == 0x181) {
      gate_ = Gate::PlaneUp;
      rate_ = static_cast<float>(opnd(args, arg_count, 0, 1)) * rate_scale_;
      return true;
    }
    if (op == 0x182) {
      gate_ = Gate::Band;
      rate_ = static_cast<float>(opnd(args, arg_count, 0, 1)) * rate_scale_;
      band_t0_ = tick_;
      return true;
    }
    return false;
  }
  bool done() const override { return tick_ > duration_; }

 protected:
  int default_duration() const override { return params_.default_len; }
  void begin() override {
    gate_ = params_.gate;
    height_ = std::max(1.0f, ymax_ - ymin_);
    rate_scale_ = height_ / static_cast<float>(duration_);
    rate_ = rate_scale_;
    if (gate_ == Gate::Ghost) blend_ = 1;
    step();
  }
  void step() override {
    if (!mesh_) return;
    bright_.assign(mesh_->verts.size(), 128);
    const float tt = static_cast<float>(std::min(tick_, duration_));
    const float fr = tt / static_cast<float>(duration_);
    const bool two_phase = params_.desaturate;
    const float sweep_t = two_phase ? std::max(0.0f, (tt - 0.4f * static_cast<float>(duration_)) / (0.6f * static_cast<float>(duration_))) : fr;
    // SweepHide: hides vertices above y = ymin_ + height * sweep (top first).
    front_ = ymax_ - height_ * (1.0f - sweep_t);
    const Vec3 centre = mesh_->centre;
    for (size_t i = 0; i < mesh_->verts.size(); ++i) {
      const Vec3& vertex = mesh_->verts[i];
      int brightness = 128;
      switch (gate_) {
        case Gate::PlaneUp:  // hide where the falling plane has passed (Y > a3c, a3c = -rate * t)
          brightness = vertex[1] - ymax_ > -rate_ * tt ? 0 : 128;
          break;
        case Gate::Band: {  // |Y - centre| < half width, half width growing
          const float hw = rate_ * static_cast<float>(tick_ - band_t0_) * 0.5f;
          brightness = std::fabs(vertex[1] - centre[1]) < hw ? 0 : 128;
          break;
        }
        case Gate::SweepReveal: {  // hensin: the new form materialises from the head down; 24 unit soft edge
          const float edge = ymin_ + height_ * fr;
          brightness = std::clamp(static_cast<int>((edge - vertex[1]) * 128.0f / 24.0f), 0, 128);
          break;
        }
        case Gate::SweepHide: {  // octdeth / ki_deth 0x182
          if (two_phase && sweep_t <= 0) break;
          const float edge = ymin_ + height_ * sweep_t;
          brightness = std::clamp(static_cast<int>((vertex[1] - edge) * 128.0f / 24.0f + 128.0f * 0.0f), 0, 128);
          break;
        }
        case Gate::Ring: {  // jakotu: radius phases 11.7 / tick, hold, 21.5 / tick
          const float dist = len(sub(vertex, centre));
          float radius;
          int phase;
          if (ring_phase(radius, phase)) brightness = 0;
          else if (phase < 2) brightness = dist < radius * 0.5f ? 0xC8 : dist < radius ? 0x80 : 0;
          else brightness = dist < radius * 0.5f ? 0 : dist < radius ? 0x80 : 0xC8;
          break;
        }
        case Gate::Ghost: brightness = 128; break;
      }
      bright_[i] = brightness;
      // Colour work: CLUT grey lerp (octdeth / ki_deth) and white-out.
      std::array<u8, 3> colour = mesh_->colours[i];
      if (params_.desaturate) {
        const float mix = std::min(1.0f, tt / (0.4f * static_cast<float>(duration_)));
        const float avg = (colour[0] + colour[1] + colour[2]) / 3.0f;
        for (int channel = 0; channel < 3; ++channel) colour[static_cast<size_t>(channel)] = util::clamp8(avg * mix + static_cast<float>(colour[static_cast<size_t>(channel)]) * (1 - mix));
        if (params_.whiteout && sweep_t > 0) colour = {255, 255, 255};
      }
      if (gate_ == Gate::Ghost) {
        const float pulse = 0.55f + 0.35f * static_cast<float>(rsin(tick_ * 96)) / 4096.0f;
        for (int channel = 0; channel < 3; ++channel) colour[static_cast<size_t>(channel)] = util::clamp8(static_cast<float>(colour[static_cast<size_t>(channel)]) * pulse * (1.0f - fr * 0.5f));
      }
      col_[i] = colour;
      pos_[i] = vertex;
    }
    finish_step();
    if (params_.sparkles && gate_ == Gate::SweepHide && sweep_t > 0 && sweep_.size() < 128)
      for (int i = 0; i < 2; ++i) {
        const float angle = static_cast<float>(rng_.next() % 4096) * 6.2831853f / 4096.0f, radius = static_cast<float>(rng_.next() % 100) * 0.01f * mesh_->radius * 0.5f;
        sweep_.push_back({add(base_, {centre[0] + std::cos(angle) * radius, ymin_ + height_ * sweep_t, centre[2] + std::sin(angle) * radius}), 0});
      }
    for (auto& sparkle : sweep_) ++sparkle.second;
    sweep_.erase(std::remove_if(sweep_.begin(), sweep_.end(), [](const auto& sparkle) { return sparkle.second > 20; }), sweep_.end());
  }
  void extra(std::vector<FxPrim>& out) const override {
    for (const auto& sparkle : sweep_)
      util::emit_point(ctx_.view, sparkle.first, 3.0f, ctx_.colour[0], ctx_.colour[1], ctx_.colour[2], 2, out);
    if (gate_ == Gate::SweepReveal || (gate_ == Gate::SweepHide && !params_.whiteout)) {
      // glowing cut line along the front
      const float yf = gate_ == Gate::SweepReveal ? ymin_ + height_ * static_cast<float>(std::min(tick_, duration_)) / static_cast<float>(duration_)
                                                  : front_;
      FxPrim line;
      line.kind = FxPrim::Kind::Line;
      line.blend = 2;
      float z0, z1;
      const float half_width = mesh_ ? mesh_->radius * 0.5f : 40.0f;
      if (fx_project(ctx_.view, add(base_, {-half_width, yf, 0}), line.v[0].x, line.v[0].y, z0) && fx_project(ctx_.view, add(base_, {half_width, yf, 0}), line.v[1].x, line.v[1].y, z1)) {
        for (auto& vert : line.v) {
          vert.r = ctx_.colour[0];
          vert.g = ctx_.colour[1];
          vert.b = ctx_.colour[2];
        }
        line.slot = fx_slot_of_depth(z0);
        out.push_back(line);
      }
    }
  }

 private:
  // false when the ring has not started; radius and phase (0 grow, 1 hold, 2 grow faster)
  bool ring_phase(float& radius, int& phase) const {
    const float tt = static_cast<float>(tick_);
    const float r0 = tt * 48000.0f / 4096.0f;
    if (r0 < 881.0f) {
      radius = r0;
      phase = 0;
    } else if (tt < 881.0f * 4096.0f / 48000.0f + 51.0f) {
      radius = 881.0f;
      phase = 1;
    } else {
      radius = (tt - (881.0f * 4096.0f / 48000.0f + 51.0f)) * 88000.0f / 4096.0f;
      phase = 2;
    }
    return false;
  }

  GateParams params_;
  Gate gate_ = Gate::PlaneUp;
  float height_ = 1, rate_ = 1, rate_scale_ = 1, front_ = 0;
  int band_t0_ = 0;
  std::vector<std::pair<Vec3, int>> sweep_;
};

// ---- BlackHole (46black) --------------------------------------------------------------------------------------------------------------
// Per vertex (relative to the hole): L = |v|, s = max(0, T - (L >> sh)); s > D -> the point (0, -200, 0), else
// x' = x - x s / D, z' = z - z s / D, y' = y + (-200 - y) s / D, then a spin about Y by theta = s << k (4096 = turn). Applied to the
// 10x11 ground grid (cells 0x400), the anchor's meshes and the near / far model objects. Ops: 0x180 phase A (timer starts at 0x28),
// 0x181 phase B (k @+8, sh @+0xA, D @+0xC), 0x182 stop. Ground grid and actor meshes are scaled 1/16 here; the 'sin / cos' helper
// mapping is inferred.
class BlackHole : public BespokeEffect {
 public:
  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    collapse_ticks_ = ctx.duration > 0 ? ctx.duration : 50;
    centre_ = ctx.target.pos;
    floor_ = ctx.target.pos[1] + (ctx.target.mesh ? ctx.target.mesh->centre[1] + ctx.target.mesh->radius * 0.6f : 0.0f);
    if (ctx.hooks.set_visible) {
      ctx.hooks.set_visible(0, false);
      ctx.hooks.set_visible(1, false);
    }
    build_grid();
  }
  bool command(int op, const int* args, int arg_count) override {
    if (op == 0x180) {
      tick_ = std::max(tick_, 0) + 40;
      return true;
    }
    if (op == 0x181) {
      spin_shift_ = opnd(args, arg_count, 0, spin_shift_);
      dist_shift_ = std::clamp(opnd(args, arg_count, 1, dist_shift_), 0, 12);
      collapse_ticks_ = std::max(1, opnd(args, arg_count, 2, collapse_ticks_));
      tick_ = 0;
      return true;
    }
    if (op == 0x182) {
      stopped_ = true;
      return true;
    }
    return false;
  }
  void tick() override { ++tick_; }
  void draw(std::vector<FxPrim>& out) const override {
    if (stopped_) return;
    u32 order = 0;
    for (const FxActor* actor : {&ctx_.caster, &ctx_.target}) {
      if (!actor->mesh) continue;
      std::vector<Vec3> warped(actor->mesh->verts.size());
      for (size_t i = 0; i < warped.size(); ++i) warped[i] = warp(add(actor->pos, actor->mesh->verts[i]));
      util::emit_mesh(ctx_.view, *actor->mesh, warped, actor->mesh->colours, 0, out, order);
      order += 2000;
    }
    for (size_t cell_index = 0; cell_index < cells_.size(); ++cell_index) {
      FxPrim prim;
      prim.kind = FxPrim::Kind::Quad;
      float zs = 0;
      bool ok = true;
      for (int k = 0; k < 4 && ok; ++k) {
        float z;
        ok = fx_project(ctx_.view, warp(cells_[cell_index].corners[k]), prim.v[k].x, prim.v[k].y, z);
        zs += z;
        prim.v[k].r = cells_[cell_index].col[0];
        prim.v[k].g = cells_[cell_index].col[1];
        prim.v[k].b = cells_[cell_index].col[2];
      }
      if (!ok) continue;
      prim.slot = std::min(0xFFF, fx_slot_of_depth(zs * 0.25f) + 40);
      prim.order = order + static_cast<u32>(cell_index);
      out.push_back(prim);
    }
    // the hole itself: a dark violet core that grows with the collapse
    const float growth = std::clamp(static_cast<float>(tick_) / static_cast<float>(collapse_ticks_ + 40), 0.0f, 1.0f);
    util::emit_point(ctx_.view, add(centre_, {0, -kHoleY, 0}), 4.0f + 14.0f * growth, 90, 30, 140, 2, out);
  }
  bool done() const override { return stopped_ || tick_ > max_l_ / static_cast<float>(1 << dist_shift_) + static_cast<float>(collapse_ticks_) + 8; }

 private:
  static constexpr float kHoleY = 120.0f;
  struct Cell {
    Vec3 corners[4];
    std::array<u8, 3> col;
  };
  void build_grid() {
    cells_.clear();
    // 0x400 / 16
    const float cell_size = 64.0f;
    max_l_ = 0;
    for (int z = 0; z < 11; ++z)
      for (int x = 0; x < 10; ++x) {
        Cell cell;
        const float x0 = -320.0f + static_cast<float>(x) * cell_size, z0 = -256.0f + static_cast<float>(z) * cell_size;
        const float xs[4] = {x0, x0 + cell_size, x0, x0 + cell_size}, zs[4] = {z0, z0, z0 + cell_size, z0 + cell_size};
        for (int k = 0; k < 4; ++k) {
          cell.corners[k] = add(centre_, {xs[k], floor_ - centre_[1], zs[k]});
          max_l_ = std::max(max_l_, len(sub(cell.corners[k], centre_)));
        }
        const bool dark = (x + z) & 1;
        cell.col = dark ? std::array<u8, 3>{28, 12, 48} : std::array<u8, 3>{56, 24, 84};
        cells_.push_back(cell);
      }
    for (const FxActor* actor : {&ctx_.caster, &ctx_.target})
      if (actor->mesh) max_l_ = std::max(max_l_, len(sub(add(actor->pos, actor->mesh->centre), centre_)) + actor->mesh->radius);
  }
  Vec3 warp(const Vec3& world) const {
    const Vec3 rel = sub(world, centre_);
    const float dist = len(rel);
    const float progress = std::max(0.0f, static_cast<float>(tick_) - dist / static_cast<float>(1 << dist_shift_));
    const float span = static_cast<float>(collapse_ticks_);
    if (progress <= 0) return world;
    Vec3 pulled;
    if (progress > span) pulled = {0, -kHoleY, 0};
    else pulled = {rel[0] - rel[0] * progress / span, rel[1] + (-kHoleY - rel[1]) * progress / span, rel[2] - rel[2] * progress / span};
    const float th = static_cast<float>(static_cast<int>(progress) << spin_shift_) * 6.2831853f / 4096.0f;
    const float cos_th = std::cos(th), sin_th = std::sin(th);
    return add(centre_, {pulled[0] * cos_th + pulled[2] * sin_th, pulled[1], pulled[2] * cos_th - pulled[0] * sin_th});
  }

  Vec3 centre_{0, 0, 0};
  float floor_ = 0, max_l_ = 0;
  int tick_ = 0, collapse_ticks_ = 50, spin_shift_ = 3, dist_shift_ = 2;
  bool stopped_ = false;
  std::vector<Cell> cells_;
};

constexpr SweepParams kSweeps[] = {
    {"tfleaf", true, true, 0.25f, false, 60},
    {"tgfrog", true, true, 0.25f, false, 60},
    {"tgolem", true, true, 0.25f, false, 60},
    {"tunico", true, true, 0.25f, false, 60},
    {"cook", true, true, 0.35f, true, 60},
    {"swordbox", true, false, 0.35f, true, 60},
    {"45infe", false, false, 0.0f, false, 60},
};

constexpr GateParams kGates[] = {
    {"mushi", Gate::PlaneUp, false, false, false, 90},
    {"garadeth", Gate::Band, false, false, false, 90},
    {"hensin", Gate::SweepReveal, false, false, false, 90},
    {"ghost2", Gate::Ghost, false, false, false, 120},
    {"octdeth", Gate::SweepHide, true, false, true, 120},
    {"ki_deth", Gate::SweepHide, true, true, false, 120},
    {"jakotu", Gate::Ring, false, false, false, 200},
};

}  // namespace

void register_mesh_sweeps(BespokeRegistry& registry) {
  for (const SweepParams& params : kSweeps) registry.add({params.name, "mesh-deform", "SweepCollapse", [params] { return std::make_unique<SweepCollapse>(params); }, false});
  registry.add({"44get", "mesh-deform", "WaveRise", [] { return std::make_unique<WaveRise>(); }, false});
  registry.add({"46black", "mesh-deform", "BlackHole", [] { return std::make_unique<BlackHole>(); }, false});
  for (const GateParams& params : kGates) registry.add({params.name, "mesh-deform", "GateFx", [params] { return std::make_unique<GateFx>(params); }, false});
}

}  // namespace oc::effects
