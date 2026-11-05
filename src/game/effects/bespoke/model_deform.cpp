// Overlays that skin the target model themselves and draw it deformed: ports of the PS1 code. The host skins the
// posed mesh and calls the copy's deform function on every skinned world vertex (PS1 axes, the s16 values the GTE wrote); the overlay's
// per-vertex formula is reproduced exactly.
//   44get    Hell Soul   ops 0x180 / 0x181 / 0x182, skin + wave
//   45infe   Hell-Bound  ops 0x180 / 0x181 / 0x182, skin + collapse
//   38super  Ultranova   op 0x100 (per-target task)
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "game/effects/bespoke/bespoke_util.h"
#include "game/effects/bespoke/mesh_tech.h"
#include "game/effects/bespoke/model_draw.h"
#include "game/effects/bespoke/overlay_base.h"
#include "game/effects/fx_math.h"

namespace oc::effects {
namespace {

int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }
s16 s16v(float value) { return static_cast<s16>(std::lround(value)); }

// 44get: 0x180 (dur @+4, speed @+8): a semi-transparent additive copy of target 0 rises by speed * t * 0x80 (20.12) per tick while
// t < dur (t stops at dur; -704 * 80 * 128 >> 12 = -1760 units); the real model stays. 0x181 (dur @+4, amp @+0xC, shift @+0xE, amp rate
// @+0x10): the copy keeps rising 5 units per tick (t * -0x5000), its colour falls linearly to black ((c * rem * 0x80 / dur) >> 7) and every
// skinned vertex sways x += (rsin(phase + (y << shift)) * amp) >> 12, phase += 0x80 and amp += rate per tick; at t = dur the mode ends.
// Copy: part flag bit 1 (semi-transparent) with tpage ABR 1 (B + F).
class Get44 : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x180) {
      mode_ = 0x180;
      speed_ = arg(args, count, 8);
      duration_ = arg(args, count, 4);
    } else if (op == 0x181) {
      mode_ = 0x181;
      amp_ = arg(args, count, 0xC);
      shift_ = arg(args, count, 0xE);
      amp_rate_ = arg(args, count, 0x10);
      duration_ = arg(args, count, 4);
    } else if (op == 0x182) {
      mode_ = 0x182;
      duration_ = arg(args, count, 4);
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* victim = target(0);
    if (!victim) return;
    victim->copies.clear();
    if (mode_ == 0x180) {
      if (t180_ < duration_) ++t180_;
      FxModel::Copy copy;
      copy.offset = {0, static_cast<float>((speed_ * t180_ * 0x80) >> 12), 0};
      copy.abr = 1;
      victim->copies.push_back(std::move(copy));
    } else if (mode_ == 0x181) {
      const int tt = t181_ + 1, rem = duration_ - tt;
      if (duration_ <= tt) {
        mode_ = 0;
        return;
      }
      phase_ += 0x80;
      amp_ += amp_rate_;
      t181_ = tt;
      FxModel::Copy copy;
      for (int k = 0; k < 3; ++k) copy.rgb[k] = trunc_div(victim->rgb[k] * rem * 0x80, duration_) >> 7;
      copy.offset = {0, static_cast<float>((speed_ * t180_ * 0x80 + tt * -0x5000) >> 12), 0};
      copy.abr = 1;
      const int phase = phase_, amp = amp_, shift = shift_;
      copy.deform = [phase, amp, shift](Vec3& vertex) {
        const s16 y = s16v(vertex[1]);
        vertex[0] = static_cast<float>(static_cast<s16>(s16v(vertex[0]) + ((rsin(phase + (y << shift)) * amp) >> 12)));
      };
      victim->copies.push_back(std::move(copy));
    }
  }
  bool done() const override { return mode_ != 0x180 && mode_ != 0x181; }

 private:
  int mode_ = 0, speed_ = 0, duration_ = 1, amp_ = 0, shift_ = 0, amp_rate_ = 0, phase_ = 0, t180_ = 0, t181_ = 0;
};

// 45infe: init keeps target 0's position P. 0x180 (dur @+4, yoff @+8): the overlay draws target 0 itself, every vertex lifted by yoff (the
// normal draw is hidden after its own draw). 0x181 (dur @+4): every other unit is hidden; each vertex with -(y >> 3) < t
// collapses toward P: k = min(t + (y >> 3), dur), v -= (v + (0, yoff, 0) - P) * k / dur, then y += yoff. 0x182: everybody shown again,
// target 0 plays the 'dead' reaction.
class Infe45 : public OverlayFx {
 protected:
  void init() override {
    if (const FxModel* victim = target(0))
      for (int k = 0; k < 3; ++k) pivot_[k] = static_cast<int>(victim->pos[static_cast<size_t>(k)]);
  }
  bool command(int op, const int* args, int count) override {
    FxModel* victim = target(0);
    if (op == 0x180) {
      mode_ = 0x180;
      tick_ = 0;
      yoff_ = arg(args, count, 8);
      duration_ = arg(args, count, 4);
    } else if (op == 0x181) {
      if (ctx_.models)
        for (FxModel& unit_model : ctx_.models->units)
          if (&unit_model != victim && unit_model.unit >= 0) unit_model.hidden = true;
      mode_ = 0x181;
      tick_ = 0;
      duration_ = arg(args, count, 4);
    } else if (op == 0x182) {
      if (ctx_.models)
        for (FxModel& unit_model : ctx_.models->units) unit_model.hidden = false;
      if (victim) {
        victim->hidden = false;
        victim->copies.clear();
        run_script(victim, "dead", 0x80, 7);
      }
      mode_ = 0x182;
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    FxModel* victim = target(0);
    if (!victim) return;
    victim->copies.clear();
    victim->hidden = false;
    if (mode_ != 0x180 && mode_ != 0x181) return;
    ++tick_;
    FxModel::Copy copy;
    const int mode = mode_, tt = tick_, dur = std::max(1, duration_), yoff = yoff_, px = pivot_[0], py = pivot_[1], pz = pivot_[2];
    copy.deform = [mode, tt, dur, yoff, px, py, pz](Vec3& vertex) {
      const int x = s16v(vertex[0]), y = s16v(vertex[1]), z = s16v(vertex[2]);
      int dx = 0, dy = 0, dz = 0;
      if (mode == 0x181) {
        const int y_band = static_cast<s16>(y) >> 3;
        if (-y_band < tt) {
          const int strength = std::min(tt + y_band, dur);
          dx = trunc_div((x - px) * strength, dur);
          dy = trunc_div((y + yoff - py) * strength, dur);
          dz = trunc_div((z - pz) * strength, dur);
        }
      }
      vertex = {static_cast<float>(static_cast<s16>(x - dx)), static_cast<float>(static_cast<s16>(yoff + (y - dy))), static_cast<float>(static_cast<s16>(z - dz))};
    };
    victim->copies.push_back(std::move(copy));
    victim->hidden = true;
  }
  bool done() const override { return mode_ != 0x180 && mode_ != 0x181; }

 private:
  int mode_ = 0, tick_ = 0, duration_ = 1, yoff_ = 0;
  int pivot_[3] = {0, 0, 0};
};

// 38super: 0x100 (dur @+4) starts one task per target: on its first tick it measures the skinned vertices' distances r from the centre
// C = (0, -1728, 0) (min, and max' = max + (max - min) / 16); then each tick t += 1 (the task ends when t > dur), the normal draw is hidden
// (never shown again by the overlay) and the overlay draws the model with every vertex inside the front min + (max' - min) * t / dur
// pulled in: rn = (r - min) * dur / (max' - min), a = (t - rn) * 0x400 / (dur - rn) + 0x400, v += (v - C) * rcos(a) / 4096 - a collapse
// that sweeps outward from the centre and ends at C when t = dur.
class Super38 : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100) return false;
    for (int i = 0; i < target_count(); ++i) {
      auto task = std::make_shared<Task>();
      task->model = target(i);
      task->dur = arg(args, count, 4);
      add_task([task] { return step(*task); });
    }
    return true;
  }

 private:
  static constexpr int kCx = 0, kCy = -1728, kCz = 0;
  struct Task {
    FxModel* model = nullptr;
    int tick = 0, dur = 1;
    // Measured on the first draw (the PS1 measures the vertex buffer on the first tick): min, max, measured flag.
    std::shared_ptr<int[]> mm = std::shared_ptr<int[]>(new int[3]{0x7FFF, 0, 0});
  };
  static int radius(const Vec3& vertex, int& dx, int& dy, int& dz) {
    dx = s16v(vertex[0]) - kCx, dy = s16v(vertex[1]) - kCy, dz = s16v(vertex[2]) - kCz;
    return static_cast<int>(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + static_cast<double>(dz) * dz));
  }
  static bool step(Task& task) {
    task.tick += 1;
    task.model->copies.clear();
    // The task frees itself, no draw (the model stays hidden).
    if (task.dur < task.tick) return false;
    task.model->hidden = true;
    FxModel::Copy copy;
    const int tick = task.tick, dur = std::max(1, task.dur);
    auto mm = task.mm;
    copy.deform = [tick, dur, mm](Vec3& vertex) {
      int dx, dy, dz;
      const int dist = radius(vertex, dx, dy, dz);
      // First pass: measure.
      if (!mm[2]) {
        mm[0] = std::min(mm[0], dist);
        mm[1] = std::max(mm[1], dist);
        return;
      }
      const int mn = mm[0], mx = mm[1];
      if (mx <= mn) return;
      const int front = mn + trunc_div((mx - mn) * tick, dur);
      if (dist >= front) return;
      const int rn = trunc_div((dist - mn) * dur, mx - mn);
      if (dur - rn == 0) return;
      const int angle = trunc_div((tick - rn) * 0x400, dur - rn) + 0x400;
      const int cos_angle = rcos(angle);
      vertex = {static_cast<float>(static_cast<s16>(s16v(vertex[0]) + trunc_div(dx * cos_angle, 0x1000))),
                static_cast<float>(static_cast<s16>(s16v(vertex[1]) + trunc_div(dy * cos_angle, 0x1000))),
                static_cast<float>(static_cast<s16>(s16v(vertex[2]) + trunc_div(dz * cos_angle, 0x1000)))};
    };
    finish_measure(task);
    task.model->copies.push_back(std::move(copy));
    return true;
  }
  // The measuring pass runs during the first frame's draw; from the second tick on the measured range is final (max' = max + (max - min) / 16).
  static void finish_measure(Task& task) {
    if (task.tick >= 2 && !task.mm[2]) {
      const int mn = task.mm[0], mx = task.mm[1];
      // max' = max + (max - min) / 16 (((max - min) + 15 if negative) >> 4).
      task.mm[1] = mx + ((mx - mn) >> 4);
      task.mm[2] = 1;
    }
  }
};

// 46black: the overlay draws the warped models itself every frame (the stock draw is hidden). Which models: when target 0 is a PC
// every unit without the monster bit (and without the flag-8 bit), else the target list. 0x180: model
// timer on at 40 (+1 per tick), SetAnim(target, 5, 0x1E) on every target; 0x181: the ground / scenery timer (stage ring + 'near' / 'far'
// meshes, not done here); 0x182: both timers off (identity). Warp of a skinned world vertex (models: shifts 4 / 5, D = 112):
// r = |v|, t' = max(0, t - (r >> 5)); t' > D -> (0, -200, 0); else X = x + (-x / D) t', Y = y + ((-200 - y) / D) t', Z = z + (-z / D) t' and
// (X, Z) turned about y by a = t' << 4: x' = Z sin a + X cos a, z' = Z cos a - X sin a (>> 12 each). Everything spirals into (0, -200, 0).
class Black46 : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    (void)args;
    (void)count;
    if (op == 0x180) {
      on_ = true;
      tick_ = 0x28;
      for (int i = 0; i < target_count(); ++i) set_anim(target(i), 5, 0x1E);
    } else if (op == 0x182) {
      on_ = false;
      tick_ = 0;
    } else {
      return op == 0x181;
    }
    return true;
  }
  void frame() override {
    if (!ctx_.models) return;
    if (on_) ++tick_;
    const FxModel* t0 = target(0);
    std::vector<FxModel*> warp;
    if (t0 && t0->pc) {
      for (FxModel& unit_model : ctx_.models->units)
        if (unit_model.unit >= 0 && unit_model.pc) warp.push_back(&unit_model);
    } else {
      for (int i = 0; i < target_count(); ++i) warp.push_back(target(i));
    }
    for (FxModel& unit_model : ctx_.models->units) unit_model.copies.clear();
    const int tick = tick_;
    for (FxModel* warped : warp) {
      if (!warped) continue;
      // Every tick the model is shown, the overlay draws it itself, and it is hidden again: the hidden bit is SET after every tick from the
      // very first one, long before op 0x180. Until the warp starts (t <= 0, t' = 0 everywhere)
      // that private draw equals the stock one, which the host keeps drawing: the bit is mirrored in `flags` only.
      warped->hidden = tick > 0;
      if (tick <= 0) {
        warped->flags = static_cast<u16>(warped->flags | 0x1);
        continue;
      }
      FxModel::Copy copy;
      copy.deform = [tick](Vec3& vertex) { warp_vertex(vertex, tick, 4, 5, 112); };
      warped->copies.push_back(std::move(copy));
    }
  }
  bool done() const override { return !on_; }

 public:
  static void warp_vertex(Vec3& vertex, int tick, int sh_angle, int sh_dist, int divisor) {
    const int x = s16v(vertex[0]), y = s16v(vertex[1]), z = s16v(vertex[2]);
    const int dist = static_cast<int>(std::sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y + static_cast<double>(z) * z));
    const int local_tick = std::max(0, tick - (dist >> sh_dist));
    if (local_tick > divisor) {
      vertex = {0, -200, 0};
      return;
    }
    const int new_x = x + trunc_div(-x, divisor) * local_tick;
    const int new_y = static_cast<s16>(y + static_cast<s16>(trunc_div(static_cast<s16>(-200 - y), divisor)) * static_cast<s16>(local_tick));
    const int new_z = z + trunc_div(-z, divisor) * local_tick;
    const int ang = local_tick << sh_angle;
    const int sn = rsin(ang), cs = rcos(ang);
    vertex = {static_cast<float>(static_cast<s16>(static_cast<s16>((new_z * sn) >> 12) + static_cast<s16>((new_x * cs) >> 12))), static_cast<float>(new_y),
              static_cast<float>(static_cast<s16>(static_cast<s16>((new_z * cs) >> 12) - static_cast<s16>((new_x * sn) >> 12)))};
  }

 private:
  bool on_ = false;
  int tick_ = 0;
};

// cook / swordbox (byte-identical but for labels): the 'spot' emitter's op-0x48 callback replaces its draw and stores the
// particle's position as the centre C. 0x100 (dur @+4, K @+8): target 0 is re-skinned and drawn by the overlay (stock draw hidden) while
// t <= dur: H = the model's height (y up is negative, so H > 0), h2 = H / 2, a1 = H + h2, thr = a1 t / dur - a1 - y_M + h2; every skinned vertex with
// y < thr: tau = (y + H + y_M) dur / a1, ang = (t - tau) 1024 / (dur - tau), v += (v - C) rcos(ang + 0x400) / 4096 and y += rcos(2 ang + 0x400) K / 4096 -
// the front falls from the head past the feet and the target is sucked into C behind it, with a bulge. At t > dur the model is put at C and stays hidden until its reaction script shows it and jumps it home. 0x101 = the
// NEGAPOSI screen negative for u16 @+4 ticks. The deformed model goes through the stock draw, so its env-mapped and semi-transparent parts stay.
class Vacuum : public MeshTech {
 protected:
  void init() override {
    if (!em()) return;
    em()->set_object_hook("spot", [this](EmitterSystem::ObjectRef& object) {
      for (int k = 0; k < 3; ++k) centre_[k] = static_cast<s16>(object.pos[k] >> 16);
    }, true);
    em()->set_draw_suppressed("spot", true);  // cook's spot is a mesh
    em()->set_draw_dropped("spot", true);     // swordbox's is a sprite
  }
  bool command(int op, const int* args, int count) override {
    if (op == 0x101) nega_.start(arg(args, count, 4));
    if (op != 0x100) return op == 0x101;
    FxModel* victim = target(0);
    if (!victim) return true;
    auto task = std::make_shared<Task>();
    task->model = victim;
    task->dur = std::max(1, arg(args, count, 4));
    task->bulge = arg(args, count, 8);
    task->height = static_cast<int>(victim->height);
    task->centre = centre_;
    task_ = task;
    add_task([task] { return task->step(); });
    return true;
  }

  void frame() override { nega_.tick(); }
  void draw(std::vector<FxPrim>& out) const override {
    nega_.draw(out);
    if (!task_ || !task_->deform) return;
    ModelDrawSpec spec;
    spec.mask_part_flags = false;
    spec.tri_far_z = true;
    spec.env_uv = ModelDrawSpec::EnvUv::Half;
    spec.env_tpage = 0x95;
    spec.env_clut = 0x3BC0;
    spec.face_hook = [this](DrawVert* corners, int count, int, int, int) {
      for (int k = 0; k < count; ++k) task_->deform(corners[k]);
      return true;
    };
    u32 order = 0;
    draw_model(task_->model, spec, out, order);
  }
  bool done() const override { return OverlayFx::done() && !nega_.active(); }

 private:
  struct Task {
    FxModel* model = nullptr;
    int tick = 0, dur = 1, bulge = 0, height = 0;
    const int* centre = nullptr;
    std::function<void(DrawVert&)> deform;
    bool step() {
      tick += 1;
      if (dur < tick) {
        deform = nullptr;
        model->pos = {static_cast<float>(centre[0]), static_cast<float>(centre[1]), static_cast<float>(centre[2])};
        model->release = true;
        model->hidden = true;
        return false;
      }
      const int half_height = height / 2, a1 = height + half_height, my = static_cast<int>(model->pos[1]);
      const int thr = trunc_div(a1 * tick, dur) - a1 - my + half_height;
      const int tt = tick, duration = dur, bulge_k = bulge, model_height = height, cx = centre[0], cy = centre[1], cz = centre[2];
      deform = [=](DrawVert& vertex) {
        const int x = vertex.x, y = vertex.y, z = vertex.z;
        if (y >= thr) return;
        const int tau = trunc_div((y + model_height + my) * duration, a1);
        if (duration - tau == 0) return;
        const int ang = trunc_div((tt - tau) * 1024, duration - tau);
        const int c1 = rcos(ang + 0x400);
        vertex.x = static_cast<s16>(x + trunc_div((x - cx) * c1, 4096));
        vertex.y = static_cast<s16>(y + trunc_div((y - cy) * c1, 4096) + trunc_div(rcos(2 * ang + 0x400) * bulge_k, 4096));
        vertex.z = static_cast<s16>(z + trunc_div((z - cz) * c1, 4096));
      };
      model->hidden = true;
      return true;
    }
  };
  int centre_[3] = {0, 0, 0};
  std::shared_ptr<Task> task_;
  util::NegativeStrip nega_;
};

}  // namespace

// ---- The overlays' private model draw (model_draw.h) ------------------------------------------------------------
namespace {

int sat16(int value) { return std::clamp(value, -32768, 32767); }

void env_uv_of(const ModelDrawSpec& spec, const DrawVert& vertex, u8 uv[2]) {
  switch (spec.env_uv) {
    case ModelDrawSpec::EnvUv::Xor80:  // jakotu / fnoise: (char)byte - 0x80
      uv[0] = static_cast<u8>((vertex.nx ^ 0x80) & 0xFF);
      uv[1] = static_cast<u8>((vertex.ny ^ 0x80) & 0xFF);
      break;
    case ModelDrawSpec::EnvUv::Half:  // hensin / ghost2: (nx >> 1) + 0x40, (ny >> 1) - 0x40
      uv[0] = static_cast<u8>(((vertex.nx >> 1) + 0x40) & 0xFF);
      uv[1] = static_cast<u8>(((vertex.ny >> 1) - 0x40) & 0xFF);
      break;
    case ModelDrawSpec::EnvUv::Custom:
      uv[0] = uv[1] = 0;
      if (spec.env_uv_fn) spec.env_uv_fn(vertex, uv);
      break;
  }
}

struct PartCtx {
  u8 rgb[3] = {128, 128, 128};
  u8 byte3 = 0;
  // Stock tpage ABR = (F >> 2) & 3.
  int abr_base = 0;
};

PartCtx part_ctx(const FxSkinnedModel& sk, const FxModel& model, const ModelDrawSpec& spec, int part) {
  PartCtx pc;
  model_colour_word(sk, model, spec, part, pc.rgb, pc.byte3);
  const u8 part_flags = part >= 0 && part < 16 ? sk.shading[static_cast<size_t>(part)][3] : 0;
  pc.abr_base = (part_flags >> 2) & 3;
  return pc;
}

float nclip(const float* x, const float* y, int v0, int v1, int v2) { return (x[v1] - x[v0]) * (y[v2] - y[v0]) - (x[v2] - x[v0]) * (y[v1] - y[v0]); }

// One GT3 / GT4 (corners in GPU Z order) through the builder: projection, NCLIP, OT index, the one or two packets.
void emit_poly(const DrawVert* corners, int count, int unit, int tex, int tex_w, const PartCtx& pc, const ModelDrawSpec& spec, const ViewParams& view,
               std::vector<FxPrim>& out, u32& order) {
  float sx[4], sy[4], sz[4];
  for (int k = 0; k < count; ++k) {
    // FLAG < 0.
    if (!fx_project(view, {static_cast<float>(corners[k].x), static_cast<float>(corners[k].y), static_cast<float>(corners[k].z)}, sx[k], sy[k], sz[k])) return;
  }
  float z;
  if (count == 3) {
    if (spec.cull && nclip(sx, sy, 0, 1, 2) <= 0.0f) return;
    z = (pc.byte3 & 0x10) ? (spec.tri_far_z ? std::max({sz[0], sz[1], sz[2]}) : std::min({sz[0], sz[1], sz[2]})) : (sz[0] + sz[1] + sz[2]) / 3.0f;
  } else {
    if (spec.cull && !(nclip(sx, sy, 0, 1, 2) > 0.0f || nclip(sx, sy, 1, 2, 3) < 0.0f)) return;
    z = (pc.byte3 & 0x10) ? std::min({sz[1], sz[2], sz[3]}) : spec.quad_avsz3 ? (sz[1] + sz[2] + sz[3]) / 3.0f : (sz[0] + sz[1] + sz[2] + sz[3]) / 4.0f;
  }
  int slot = static_cast<int>(z) >> 4;
  // (otz >> (14 - shift)) != 0: quads only.
  if (count == 4 && slot >= 0x1000) return;
  slot = std::clamp(slot + (count == 3 ? spec.tri_bias : spec.quad_bias), 1, 0xFFF);
  const bool semi = spec.force_abr >= 0 || (pc.byte3 & 0x02) != 0;
  const int abr_base = spec.force_abr >= 0 ? (spec.force_abr & 3) : pc.abr_base;
  auto base_prim = [&](bool colour_fn) {
    FxPrim prim;
    prim.kind = count == 3 ? FxPrim::Kind::Tri : FxPrim::Kind::Quad;
    prim.src = spec.pack_model ? FxPrim::TexSrc::PackModel : FxPrim::TexSrc::Model;
    prim.unit = unit;
    prim.tex = tex;
    prim.slot = slot;
    prim.blend = semi ? abr_base + 1 : 0;
    prim.white = spec.white;
    for (int k = 0; k < count; ++k) {
      u8 rgb[3] = {pc.rgb[0], pc.rgb[1], pc.rgb[2]};
      if (colour_fn && spec.colour) spec.colour(corners[k], rgb);
      if (spec.cue && !(pc.byte3 & 0x60)) {
        const float amount = spec.cue->amount(sz[count - 1]);
        for (int channel = 0; channel < 3; ++channel) rgb[channel] = static_cast<u8>(std::lround(rgb[channel] + (spec.cue->rgb[channel] - rgb[channel]) * amount));
      }
      prim.v[k] = {sx[k], sy[k], rgb[0], rgb[1], rgb[2]};
      prim.u[k] = corners[k].u;
      prim.w[k] = corners[k].w;
    }
    return prim;
  };
  FxPrim p1 = base_prim(true);
  if ((pc.byte3 & 0x60) && (pc.byte3 & 0x80)) {
    // Env-mapped with the model's OWN texture (shading flag 0x80 with 0x20 / 0x40; the stock unit draw's rule, mesh.vert):
    // u = (nx >> 1) + 0x40 inside the face's 128-texel half of the page, v = (ny >> 1) + 0xC0. These parts must not be skipped, or
    // Bunyip's (kamei_sp) body drops out of the kamedes ghost pair at the fade start.
    if (tex_w <= 0) return;
    for (int k = 0; k < count; ++k) {
      const float base = std::floor(corners[k].u * static_cast<float>(tex_w) / 128.0f) * 128.0f;
      p1.u[k] = (static_cast<float>((corners[k].nx >> 1) + 0x40) + base) / static_cast<float>(tex_w);
      p1.w[k] = static_cast<float>((corners[k].ny >> 1) + 0xC0) / 256.0f;
    }
  } else if (pc.byte3 & 0x60) {
    p1.src = FxPrim::TexSrc::Vram;
    p1.white = 0;
    p1.tpage = static_cast<u16>(((pc.byte3 & 0xC) << 3) | spec.env_tpage);
    p1.clut = spec.env_clut;
    const int abr_env = (pc.byte3 >> 2) & 3;
    for (int k = 0; k < count; ++k) {
      u8 uv[2];
      env_uv_of(spec, corners[k], uv);
      p1.u[k] = uv[0];
      p1.w[k] = uv[1];
    }
    if (pc.byte3 & 0x40) {
      // SetSemiTrans(P1, 1); AddPrim(P1); P2 = the textured model packet, added after (drawn first).
      p1.blend = abr_env + 1;
      p1.order = order++;
      out.push_back(p1);
      FxPrim p2 = base_prim(false);
      p2.order = order++;
      out.push_back(p2);
      return;
    }
    p1.blend = semi ? abr_env + 1 : 0;
  }
  p1.order = order++;
  out.push_back(p1);
}

// Edge test + intersection of hensin / ghost2 at plane P (y): crossing when one end is >= P and one <= P and the ends
// differ in y; t = ((P - a.y) << 12) / (b.y - a.y), every field LoadAverageShort12(a, b, 0x1000 - t, t).
bool plane_cross(const DrawVert& from, const DrawVert& to, int plane_y, DrawVert& out) {
  if (!((plane_y <= from.y || plane_y <= to.y) && (from.y <= plane_y || to.y <= plane_y))) return false;
  if (from.y == to.y) return false;
  const int frac = ((plane_y - from.y) * 4096) / (to.y - from.y);
  const int wa = 0x1000 - frac, wb = frac;
  out.x = sat16((from.x * wa + to.x * wb) >> 12);
  out.y = sat16((from.y * wa + to.y * wb) >> 12);
  out.z = sat16((from.z * wa + to.z * wb) >> 12);
  out.nx = (from.nx * wa + to.nx * wb) >> 12;
  out.ny = (from.ny * wa + to.ny * wb) >> 12;
  out.nz = (from.nz * wa + to.nz * wb) >> 12;
  out.u = (from.u * static_cast<float>(wa) + to.u * static_cast<float>(wb)) / 4096.0f;
  out.w = (from.w * static_cast<float>(wa) + to.w * static_cast<float>(wb)) / 4096.0f;
  out.idx = -1;
  return true;
}

// The walk of both cutters: perimeter v0 v1 v2 (quads v0 v1 v3 v2); push v[k] into the current list, on a crossing push the intersection, switch
// lists and push it into the other one too (at most 5 entries per list).
struct ClipLists {
  DrawVert lists[2][5];
  int counts[2] = {0, 0};
  DrawVert inter[2];
  int ninter = 0;
};
void plane_walk(const DrawVert* corners, int count, int plane_y, ClipLists& cl) {
  static constexpr int kTri[3] = {0, 1, 2}, kQuad[4] = {0, 1, 3, 2};
  const int* ring = count == 3 ? kTri : kQuad;
  int cur = 0;
  auto push = [&](int list, const DrawVert& vertex) {
    if (cl.counts[list] < 5) cl.lists[list][cl.counts[list]++] = vertex;
  };
  for (int k = 0; k < count; ++k) {
    const DrawVert& from = corners[ring[k]];
    const DrawVert& to = corners[ring[(k + 1) % count]];
    push(cur, from);
    DrawVert crossing;
    if (plane_cross(from, to, plane_y, crossing)) {
      if (cl.ninter < 2) cl.inter[cl.ninter] = crossing;
      ++cl.ninter;
      push(cur, crossing);
      cur ^= 1;
      push(cur, crossing);
    }
  }
}

}  // namespace

bool project_poly(const ViewParams& view, const DrawVert* corners, int count, int cull, ProjPoly& out) {
  out.n = count;
  for (int k = 0; k < count; ++k)
    if (!fx_project(view, {static_cast<float>(corners[k].x), static_cast<float>(corners[k].y), static_cast<float>(corners[k].z)}, out.x[k], out.y[k], out.z[k]))
      return false;
  if (cull == 0) return true;
  if (count == 3) return nclip(out.x, out.y, 0, 1, 2) > 0.0f;
  const float n1 = nclip(out.x, out.y, 0, 1, 2), n2 = nclip(out.x, out.y, 1, 2, 3);
  return cull == 2 ? (n1 > 0.0f || n2 <= 0.0f) : (n1 > 0.0f || n2 < 0.0f);
}

void push_poly(const ProjPoly& poly, const DrawVert* corners, int unit, int tex, const u8 (*rgb)[3], int blend, u8 white, int slot, u32& order,
               std::vector<FxPrim>& out, int vram_tpage, u16 vram_clut, const u8 (*uv)[2]) {
  FxPrim prim;
  prim.kind = poly.n == 3 ? FxPrim::Kind::Tri : FxPrim::Kind::Quad;
  if (vram_tpage >= 0) {
    prim.src = FxPrim::TexSrc::Vram;
    prim.tpage = static_cast<u16>(vram_tpage);
    prim.clut = vram_clut;
  } else {
    prim.src = FxPrim::TexSrc::Model;
    prim.unit = unit;
    prim.tex = tex;
    prim.white = white;
  }
  prim.blend = blend;
  prim.slot = std::clamp(slot, 1, 0xFFF);
  for (int k = 0; k < poly.n; ++k) {
    prim.v[k] = {poly.x[k], poly.y[k], rgb[k][0], rgb[k][1], rgb[k][2]};
    prim.u[k] = uv ? static_cast<float>(uv[k][0]) : corners[k].u;
    prim.w[k] = uv ? static_cast<float>(uv[k][1]) : corners[k].w;
  }
  prim.order = order++;
  out.push_back(prim);
}

DrawVert draw_vert(const FxSkinnedModel& sk, const FxSkinFace& face, int corner) {
  DrawVert vert;
  const u32 vi = face.v[corner];
  if (vi < sk.verts.size()) {
    vert.x = static_cast<s16>(std::lround(sk.verts[vi][0]));
    vert.y = static_cast<s16>(std::lround(sk.verts[vi][1]));
    vert.z = static_cast<s16>(std::lround(sk.verts[vi][2]));
  }
  if (vi < sk.normals.size()) vert.nx = sk.normals[vi][0], vert.ny = sk.normals[vi][1], vert.nz = sk.normals[vi][2];
  vert.u = face.u[corner];
  vert.w = face.w[corner];
  vert.idx = static_cast<int>(vi);
  return vert;
}

void model_colour_word(const FxSkinnedModel& sk, const FxModel& model, const ModelDrawSpec& spec, int part, u8 rgb[3], u8& byte3) {
  const std::array<u8, 4> sh = part >= 0 && part < 16 ? sk.shading[static_cast<size_t>(part)] : std::array<u8, 4>{128, 128, 128, 0};
  // Truncated to a byte, no clamp.
  for (int k = 0; k < 3; ++k) rgb[k] = static_cast<u8>((sh[static_cast<size_t>(k)] * model.rgb[k]) >> 7);
  const u8 part_flags = sh[3];
  u8 flags_out = spec.mask_part_flags ? static_cast<u8>((part_flags & 0xF2) | 0x04) : part_flags;
  if (spec.flags & 0x002) flags_out |= 0x02;
  if (spec.flags & 0x100) flags_out |= 0x01;
  if (spec.flags & 0x020) flags_out |= 0x20;
  if (spec.flags & 0x040) flags_out |= 0x40;
  if (spec.flags & 0x080) flags_out |= 0x80;
  byte3 = flags_out;
}

void model_draw(const FxSkinnedModel& sk, const FxModel& model, const ModelDrawSpec& spec, const ViewParams& view, std::vector<FxPrim>& out, u32& order) {
  std::array<PartCtx, 16> pcs;
  std::array<int, 16> quads{};
  for (int i = 0; i < 16; ++i) pcs[static_cast<size_t>(i)] = part_ctx(sk, model, spec, i);
  int face = -1;
  for (const FxSkinFace& skin_face : sk.faces) {
    ++face;
    if (skin_face.part < 0 || skin_face.part >= sk.parts || skin_face.part >= 16) continue;
    const PartCtx& pc = pcs[static_cast<size_t>(skin_face.part)];
    // Part skipped.
    if (pc.byte3 & 0x01) continue;
    DrawVert corners[4];
    for (int k = 0; k < skin_face.n; ++k) corners[k] = draw_vert(sk, skin_face, k);
    const int qi = skin_face.n == 4 ? quads[static_cast<size_t>(skin_face.part)]++ : -1;
    const int tw = skin_face.tex >= 0 && static_cast<size_t>(skin_face.tex) < sk.tex_width.size() ? sk.tex_width[static_cast<size_t>(skin_face.tex)] : 0;
    if (spec.face_hook && !spec.face_hook(corners, skin_face.n, face, qi, skin_face.part)) continue;
    if (!spec.clip) {
      emit_poly(corners, skin_face.n, model.unit, skin_face.tex, tw, pc, spec, view, out, order);
      continue;
    }
    const int plane_y = spec.plane;
    bool all_le = true, all_gt = true;
    for (int k = 0; k < skin_face.n; ++k) all_le = all_le && corners[k].y <= plane_y, all_gt = all_gt && corners[k].y > plane_y;
    // Whole polygon on one side (slot 0 = y <= P, slot 1 = y > P).
    if (all_le || all_gt) {
      if (!spec.skip[all_le ? 0 : 1]) emit_poly(corners, skin_face.n, model.unit, skin_face.tex, tw, pc, spec, view, out, order);
      continue;
    }
    ClipLists cl;
    plane_walk(corners, skin_face.n, plane_y, cl);
    for (int side_list = 0; side_list < 2; ++side_list) {
      const int cnt = cl.counts[side_list];
      if (cnt < 3) continue;
      const DrawVert* list = cl.lists[side_list];
      const int side = (list[0].y >= plane_y - 2 && list[1].y >= plane_y - 2 && list[2].y >= plane_y - 2) ? 1 : 0;
      if (spec.skip[side]) continue;
      if (cnt == 3) {
        emit_poly(list, 3, model.unit, skin_face.tex, tw, pc, spec, view, out, order);
      } else if (cnt == 4) {
        const DrawVert quad[4] = {list[0], list[1], list[3], list[2]};
        emit_poly(quad, 4, model.unit, skin_face.tex, tw, pc, spec, view, out, order);
      } else {
        emit_poly(list, 3, model.unit, skin_face.tex, tw, pc, spec, view, out, order);
        const DrawVert quad[4] = {list[0], list[2], list[4], list[3]};
        emit_poly(quad, 4, model.unit, skin_face.tex, tw, pc, spec, view, out, order);
      }
    }
  }
}

void model_plane_cuts(const FxSkinnedModel& sk, const ModelDrawSpec& spec, const std::function<void(const PlaneCut&)>& on_cut) {
  for (const FxSkinFace& skin_face : sk.faces) {
    if (skin_face.part < 0 || skin_face.part >= sk.parts || skin_face.part >= 16) continue;
    const u8 part_flags = sk.shading[static_cast<size_t>(skin_face.part)][3];
    // The spawn pass masks the colour word with 0xF2FFFFFF before the model flags are ORed in.
    u8 flags_out = static_cast<u8>(part_flags & 0xF2);
    if (spec.flags & 0x100) flags_out |= 0x01;
    if (flags_out & 0x01) continue;
    DrawVert corners[4];
    for (int k = 0; k < skin_face.n; ++k) corners[k] = draw_vert(sk, skin_face, k);
    bool all_le = true, all_gt = true;
    for (int k = 0; k < skin_face.n; ++k) all_le = all_le && corners[k].y <= spec.plane, all_gt = all_gt && corners[k].y > spec.plane;
    if (all_le || all_gt) continue;
    ClipLists cl;
    plane_walk(corners, skin_face.n, spec.plane, cl);
    if (cl.ninter > 1) on_cut({cl.inter[0], cl.inter[1], skin_face.tex, skin_face.part});
  }
}

void register_model_deforms(BespokeRegistry& registry) {
  registry.add({"cook", "model-deform", "Vacuum", [] { return std::make_unique<Vacuum>(); }});
  registry.add({"swordbox", "model-deform", "Vacuum", [] { return std::make_unique<Vacuum>(); }});
  registry.add({"46black", "model-deform", "Black46", [] { return std::make_unique<Black46>(); }});
  registry.add({"44get", "model-deform", "Get44", [] { return std::make_unique<Get44>(); }});
  registry.add({"45infe", "model-deform", "Infe45", [] { return std::make_unique<Infe45>(); }});
  registry.add({"38super", "model-deform", "Super38", [] { return std::make_unique<Super38>(); }});
}

}  // namespace oc::effects
