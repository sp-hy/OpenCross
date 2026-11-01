// Scripted-motion stand-ins that deform the target's vertices while it is drawn. All maths is in PS1 model units; `psx()` converts the
// dummy-stage vertices back to them and `wu_` scales the result forward again.
//   daimou_d  the model rises by `rate` per tick for D ticks; every vertex with y > -200 is flattened to y = 0 and sheared along the
//             signed red / blue bytes of its lit colour: x += ((y + 200) * red) >> 8, z += ((y + 200) * blue) >> 8
//   godhand   phase += 64 per tick; y1 = y + (sin(phase + (y << shift)) * amp >> 12); z += cos(phase + (y1 << shift)) * amp >> 12
//   14bar     phi += 1 per tick; y += cos(z + 16 phi) * 100 >> 12; z += cos(y + 64 phi) * 200 >> 12; op 0x181 ends the wave
//   38super   a front sweeps outward from the centre C (1.9 model heights above the feet): with h = |v - C| measured once at the start
//             (min, and max' = max + (max - min) / 16), front = min + (max' - min) * t / D, every vertex with h < front is pulled by
//             th = (h - min) * D / (max' - min), angle = (t - th) * 1024 / (D - th) + 1024, v += (v - C) * cos(angle) / 4096
// Operands: godhand 0x100 {D, amp, shift}; the others take D from the context. The frame count drawn is D + 1 (the end test follows
// the draw).
#include <algorithm>

#include "game/effects/bespoke/actor_script.h"

namespace oc::effects {
namespace {

using util::add;
using util::mul;
using util::opnd;
using util::sub;

enum class WarpKind { Daimou, GodHand, Bar14, Super38 };

class MeshWarp : public ActorScript {
 public:
  MeshWarp(WarpKind kind) : kind_(kind) {}

 protected:
  int default_duration() const override {
    switch (kind_) {
      case WarpKind::Daimou: return 60;
      case WarpKind::GodHand: return 90;
      case WarpKind::Bar14: return 150;
      case WarpKind::Super38: return 60;
    }
    return 60;
  }
  bool takes_over(int side) const override { return side == 1; }

  void begin() override {
    amp_ = 40;
    shift_ = 2;
    rate_ = -6;
    if (ctx_.arg[0] != 0 && kind_ == WarpKind::GodHand) amp_ = ctx_.arg[0];
    if (ctx_.arg[1] != 0 && kind_ == WarpKind::GodHand) shift_ = std::clamp(ctx_.arg[1], 0, 6);
    if (ctx_.arg[0] != 0 && kind_ == WarpKind::Daimou) rate_ = std::clamp(ctx_.arg[0], -64, 64);
    const FxMesh* mesh = ctx_.target.mesh;
    centre_y_ = 0;
    if (mesh && !mesh->verts.empty()) {
      float lo = mesh->verts[0][1], hi = lo;
      for (const auto& vertex : mesh->verts) {
        lo = std::min(lo, vertex[1]);
        hi = std::max(hi, vertex[1]);
      }
      centre_y_ = lo - 1.9f * (hi - lo);
      hmin_ = 1e9f;
      hmax_ = 0;
      for (const auto& vertex : mesh->verts) {
        const float metric = hmetric(vertex);
        hmin_ = std::min(hmin_, metric);
        hmax_ = std::max(hmax_, metric);
      }
      hmax_ += (hmax_ - hmin_) / 16.0f;
    }
  }

  bool command(int op, const int* args, int nargs) override {
    if (op == 0x100 && kind_ == WarpKind::GodHand) {
      D_ = std::clamp(opnd(args, nargs, 0, D_), 1, 600);
      amp_ = std::clamp(opnd(args, nargs, 1, amp_), -400, 400);
      shift_ = std::clamp(opnd(args, nargs, 2, shift_), 0, 6);
      tick_ = 0;
      return true;
    }
    if (op == 0x181 && kind_ == WarpKind::Bar14) {
      finish();
      return true;
    }
    return false;
  }

  void step() override {
    if (kind_ == WarpKind::Daimou) a_[1].pos[1] = a_[1].home[1] + fl(rate_ * std::min(tick_, D_)) * wu_;
    if (tick_ > D_) finish();
  }

  void draw_actor(int side, std::vector<FxPrim>& out) const override {
    const FxMesh& mesh = *ctx_.target.mesh;
    const ActorState& actor = a_[side];
    std::vector<Vec3> world(mesh.verts.size());
    for (size_t i = 0; i < world.size(); ++i) world[i] = add(actor.pos, warp(mesh.verts[i], mesh.colours[i]));
    util::emit_mesh(ctx_.view, mesh, world, mesh.colours, 0, out, 5000);
  }

 private:
  float psx(float value) const { return value / wu_; }
  float hmetric(const Vec3& vertex) const { return util::len(Vec3{vertex[0], vertex[1] - centre_y_, vertex[2]}); }

  Vec3 warp(const Vec3& vertex, const std::array<u8, 3>& colour) const {
    const int x = static_cast<int>(psx(vertex[0])), y = static_cast<int>(psx(vertex[1])), z = static_cast<int>(psx(vertex[2]));
    switch (kind_) {
      case WarpKind::Daimou: {
        if (tick_ > D_ || y <= -200) return vertex;
        const int red = static_cast<int8_t>(colour[0]), blue = static_cast<int8_t>(colour[2]);
        return {fl(x + (((y + 200) * red) >> 8)) * wu_, 0.0f, fl(z + (((y + 200) * blue) >> 8)) * wu_};
      }
      case WarpKind::GodHand: {
        const int phase = 64 * tick_;
        const int y1 = y + ((rsin(phase + (y << shift_)) * amp_) >> 12);
        const int z1 = z + ((rcos(phase + (y1 << shift_)) * amp_) >> 12);
        return {vertex[0], fl(y1) * wu_, fl(z1) * wu_};
      }
      case WarpKind::Bar14: {
        const int phi = tick_;
        const int y1 = y + ((rcos(z + 16 * phi) * 100) >> 12);
        const int z1 = z + ((rcos(y + 64 * phi) * 200) >> 12);
        return {vertex[0], fl(y1) * wu_, fl(z1) * wu_};
      }
      case WarpKind::Super38: {
        const float span = hmax_ - hmin_;
        if (span < 1.0f || tick_ > D_) return vertex;
        const float metric = hmetric(vertex), elapsed = fl(tick_), duration = fl(D_);
        const float front = hmin_ + span * elapsed / duration;
        if (metric >= front) return vertex;
        const float reach = (metric - hmin_) * duration / span;
        if (duration - reach < 1.0f) return vertex;
        const int angle = static_cast<int>((elapsed - reach) * 1024.0f / (duration - reach)) + 1024;
        const float pull = fl(rcos(angle)) / 4096.0f;
        const Vec3 from_centre{vertex[0], vertex[1] - centre_y_, vertex[2]};
        return add(vertex, mul(from_centre, pull));
      }
    }
    return vertex;
  }

  WarpKind kind_;
  int amp_ = 40, shift_ = 2, rate_ = -6;
  float centre_y_ = 0, hmin_ = 0, hmax_ = 1;
};

}  // namespace

void register_mesh_warps(BespokeRegistry& registry) {
  registry.add({"daimou_d", "scripted-motion", "MeshWarp", [] { return std::make_unique<MeshWarp>(WarpKind::Daimou); }});
  registry.add({"godhand", "scripted-motion", "MeshWarp", [] { return std::make_unique<MeshWarp>(WarpKind::GodHand); }});
  registry.add({"14bar", "scripted-motion", "MeshWarp", [] { return std::make_unique<MeshWarp>(WarpKind::Bar14); }});
  registry.add({"38super", "scripted-motion", "MeshWarp", [] { return std::make_unique<MeshWarp>(WarpKind::Super38); }});
}

}  // namespace oc::effects
