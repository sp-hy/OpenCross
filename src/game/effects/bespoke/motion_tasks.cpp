// Scripted-motion stand-ins of the overlays that share one per-actor task interpreter: mido2, uzumaki, 3mix (orbits) and kuzira3
// (burst). The module owns the two actors' poses: it hides the originals, animates copies and draws their meshes (FxHooks::actor_pose
// is called as well for a host that moves its own actors).
//   Orbit  (op 0x100 {D, spin0, spin_accel, drop_accel, drop0}): per tick angle += spin / 16 (4096 = one turn), spin += spin_accel,
//          y -= drop_speed * ydrop, drop_speed += drop_accel; x / z = radius * (cos, sin)(angle) about the centre (the midpoint of the two
//          actors, or the caster for uzumaki). Radius: the distance to the centre, doubled for uzumaki, and decaying with t / D for 3mix.
//          Staggered variants spin the second actor twice as fast the other way.
//   Burst  (kuzira3): the actors start at the origin and fly out on a fan of velocities while their brightness fades 0x400 -> 0x1000.
//   Return (op 0x101 {D}, or automatically after D ticks): every actor lerps back to its home position and brightness over lerp_dur ticks.
#include <algorithm>
#include <cmath>

#include "game/effects/bespoke/bespoke.h"
#include "game/effects/bespoke/bespoke_util.h"

namespace oc::effects {
namespace {

using util::add;
using util::mul;
using util::opnd;
using util::sub;

enum class Kind { Orbit, Burst };
enum class RMode { Const, Double, Decay };

// stagger = the second actor spins at -spin0 * 2; ydrop = multiplier of the vertical drop (3mix 64, else 1).
struct MotionParams {
  const char* name;
  Kind kind;
  RMode rmode;
  bool stagger;
  float ydrop;
  bool centre_on_caster;
};

class MotionTasks : public BespokeEffect {
 public:
  explicit MotionTasks(const MotionParams& params) : params_(params) {}

  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    duration_ = ctx.duration > 0 ? ctx.duration : 90;
    for (int i = 0; i < 2; ++i) {
      const FxActor& actor = i == 0 ? ctx.caster : ctx.target;
      obj_[static_cast<size_t>(i)].home = actor.pos;
      obj_[static_cast<size_t>(i)].pos = actor.pos;
      obj_[static_cast<size_t>(i)].present = actor.mesh != nullptr;
      if (ctx.hooks.set_visible && actor.mesh) ctx.hooks.set_visible(i, false);
    }
    centre_ = params_.centre_on_caster ? ctx.caster.pos : mul(add(ctx.caster.pos, ctx.target.pos), 0.5f);
    const int start_args[5] = {duration_, 0x600, 4, params_.kind == Kind::Burst ? -12 : 0, params_.kind == Kind::Burst ? 40 : 0};
    command(0x100, start_args, 5);
    lerp_dur_ = std::max(8, duration_ / 2);
  }

  bool command(int op, const int* args, int nargs) override {
    if (op == 0x100) {
      duration_ = std::max(1, opnd(args, nargs, 0, duration_));
      const float spin0 = static_cast<float>(opnd(args, nargs, 1, 0x600));
      const float spin_accel = static_cast<float>(opnd(args, nargs, 2, 4));
      const float drop_accel = static_cast<float>(opnd(args, nargs, 3, 0)) * 0.0025f;
      const float drop0 = static_cast<float>(opnd(args, nargs, 4, 0)) * 0.1f;
      phase_ = 0;
      elapsed_ = 0;
      for (int i = 0; i < kObjects; ++i) {
        Obj& obj = obj_[static_cast<size_t>(i)];
        obj.pos = obj.home;
        obj.spin = params_.stagger ? -spin0 * static_cast<float>(i + 1) : spin0;
        obj.spin_accel = params_.stagger ? -spin_accel : spin_accel;
        obj.drop_accel = drop_accel;
        obj.drop_speed = drop0;
        obj.bright = 1.0f;
        const Vec3 offset = sub(obj.home, centre_);
        obj.angle = std::atan2(offset[2], offset[0]);
        const float dist = std::sqrt(offset[0] * offset[0] + offset[2] * offset[2]);
        const float origin_dist = std::sqrt(obj.home[0] * obj.home[0] + obj.home[2] * obj.home[2]);
        obj.radius = params_.rmode == RMode::Double ? dist * 2.0f : params_.rmode == RMode::Decay ? std::max(origin_dist, dist) : dist;
        obj.radius0 = obj.radius;
        if (params_.kind == Kind::Burst) {
          const float step = 6.2831853f / static_cast<float>(kObjects + 1);
          obj.pos = {0, 0, 0};
          obj.vel = {std::cos(static_cast<float>(i + 1) * step) * 4.0f, 0.0f, std::sin(static_cast<float>(i + 1) * step) * 4.0f};
        }
      }
    } else if (op == 0x101) {
      lerp_dur_ = std::max(1, opnd(args, nargs, 0, lerp_dur_));
      phase_ = 1;
      elapsed_ = 0;
      for (auto& obj : obj_) obj.bright0 = obj.bright;
    } else {
      return false;
    }
    return true;
  }

  void tick() override {
    if (phase_ == 0) {
      for (size_t i = 0; i < 2; ++i) {
        Obj& obj = obj_[i];
        if (!obj.present) continue;
        if (params_.kind == Kind::Orbit) {
          obj.angle += (obj.spin / 16.0f) * 6.2831853f / 4096.0f;
          obj.spin += obj.spin_accel;
          obj.pos[1] -= obj.drop_speed * params_.ydrop * 0.02f;
          obj.drop_speed += obj.drop_accel;
          float radius = obj.radius;
          if (params_.rmode == RMode::Decay) radius = obj.radius0 * (1.0f - static_cast<float>(elapsed_) / static_cast<float>(duration_));
          obj.pos[0] = radius * std::cos(obj.angle) + centre_[0];
          obj.pos[2] = radius * std::sin(obj.angle) + centre_[2];
        } else {
          obj.pos[0] += obj.vel[0];
          obj.pos[2] += obj.vel[2];
          obj.pos[1] -= obj.drop_speed * 0.1f;
          obj.drop_speed += obj.drop_accel;
          obj.bright = (0x400 + (0x1000 - 0x400) * static_cast<float>(elapsed_) / static_cast<float>(duration_)) / 4096.0f;
        }
      }
      if (++elapsed_ > duration_) {
        phase_ = 1;
        elapsed_ = 0;
        for (auto& obj : obj_) obj.bright0 = obj.bright;
      }
    } else if (phase_ == 1) {
      for (size_t i = 0; i < 2; ++i) {
        Obj& obj = obj_[i];
        if (!obj.present) continue;
        const float frac = static_cast<float>(elapsed_) / static_cast<float>(lerp_dur_);
        obj.pos = add(obj.pos, mul(sub(obj.home, obj.pos), frac));
        obj.bright = obj.bright0 + (1.0f - obj.bright0) * frac;
      }
      if (++elapsed_ > lerp_dur_) {
        phase_ = 2;
        for (auto& obj : obj_) {
          obj.pos = obj.home;
          obj.bright = 1.0f;
        }
      }
    }
    for (size_t i = 0; i < 2; ++i)
      if (obj_[i].present && ctx_.hooks.actor_pose) ctx_.hooks.actor_pose(static_cast<int>(i), obj_[i].pos, 0.0f);
  }

  void draw(std::vector<FxPrim>& out) const override {
    u32 order = 0;
    for (size_t i = 0; i < 2; ++i) {
      const FxActor& actor = i == 0 ? ctx_.caster : ctx_.target;
      const Obj& obj = obj_[i];
      if (!actor.mesh || !obj.present) continue;
      std::vector<Vec3> world(actor.mesh->verts.size());
      std::vector<std::array<u8, 3>> colours(actor.mesh->verts.size());
      for (size_t k = 0; k < world.size(); ++k) {
        world[k] = add(obj.pos, actor.mesh->verts[k]);
        for (size_t channel = 0; channel < 3; ++channel)
          colours[k][channel] = util::clamp8(static_cast<float>(actor.mesh->colours[k][channel]) * obj.bright);
      }
      util::emit_mesh(ctx_.view, *actor.mesh, world, colours, obj.bright < 0.98f ? 2 : 0, out, order);
      order += 2000;
    }
  }

  bool done() const override { return phase_ >= 2; }

 private:
  static constexpr int kObjects = 2;
  struct Obj {
    Vec3 home{0, 0, 0}, pos{0, 0, 0}, vel{0, 0, 0};
    float angle = 0, spin = 0, spin_accel = 0, drop_accel = 0, drop_speed = 0, radius = 0, radius0 = 0, bright = 1, bright0 = 1;
    bool present = false;
  };
  MotionParams params_;
  Obj obj_[2];
  Vec3 centre_{0, 0, 0};
  int duration_ = 90, elapsed_ = 0, phase_ = 0, lerp_dur_ = 45;
};

constexpr MotionParams kParams[] = {
    {"mido2", Kind::Orbit, RMode::Const, false, 1.0f, false},
    {"uzumaki", Kind::Orbit, RMode::Double, true, 1.0f, true},
    {"3mix", Kind::Orbit, RMode::Decay, true, 64.0f, false},
    {"kuzira3", Kind::Burst, RMode::Const, false, 1.0f, false},
};

}  // namespace

void register_motion_tasks(BespokeRegistry& registry) {
  for (const MotionParams& params : kParams)
    registry.add({params.name, "scripted-motion", "MotionTasks", [params] { return std::make_unique<MotionTasks>(params); }});
}

}  // namespace oc::effects
