// Overlays that only steer the battle models of the caster / target lists: ports of the PS1 code, one class per
// overlay. The models are the host's (FxModels); nothing here draws.
//   41grav  Gravity Blow   ops 0x180-0x183
//   43acce  Freefall       ops 0x180-0x183
//   19helf  Carnivore      ops 0x100 / 0x101
//   golem   Golem summon   ops 0x100 / 0x101 ("PETANKO")
#include <algorithm>
#include <cmath>
#include <memory>

#include "game/effects/bespoke/overlay_base.h"

namespace oc::effects {
namespace {

// MIPS div: truncates toward zero.
int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }

// ---- 41grav --------------------------------------------------------------------------------------------------------------------------
// Target 0 is pushed away from the caster (0x180), flies spinning toward the camera eye (0x181, 0x182: faster spin, y >= -10000), then
// drops from y -4096 onto its home x / z (0x183: speed 400 + 30 per tick) and plays the reaction of the pack's 'dead' table (state 0x184).
class Grav41 : public OverlayFx {
 protected:
  // Remembers the target's pose (restored by 0x183 and at exit).
  void init() override {
    if (FxModel* victim = target(0)) {
      home_ = victim->pos;
      for (int k = 0; k < 3; ++k) home_rot_[k] = victim->rot[k];
    }
  }
  bool command(int op, const int* args, int count) override {
    FxModel* victim = target(0);
    if (!victim) return op >= 0x180 && op <= 0x183;
    switch (op) {
      case 0x180: {  // v = (A - B) * k >> 12 with A = target 0, B = caster 0 (20.12), k = s16 @+8; SetAnim(A, 5, 1)
        const int k = arg(args, count, 8);
        const FxModel* source = caster(0);
        const Vec3 source_pos = source ? source->pos : victim->pos;
        for (size_t i = 0; i < 3; ++i) v_[i] = (victim->pos[i] - source_pos[i]) * static_cast<float>(k) / 4096.0f;
        state_ = 0x180;
        set_anim(victim, 5, 1);
        break;
      }
      case 0x181:  // shadow off, speed = s16 @+8, SetAnim(A, 6, 10)
        flag_clear(victim, 0x200);
        state_ = 0x181;
        speed_ = arg(args, count, 8);
        set_anim(victim, 6, 10);
        break;
      case 0x182:  // SetAnim(A, 8, 10)
        state_ = 0x182;
        set_anim(victim, 8, 10);
        break;
      case 0x183:  // flag 1 cleared, shadow on, y = -4096.0, speed 400, x / z and rotation restored
        flag_clear(victim, 1);
        flag_set(victim, 0x200);
        victim->pos = {home_[0], -4096.0f, home_[2]};
        victim->rot[0] = home_rot_[0], victim->rot[1] = home_rot_[1], victim->rot[2] = home_rot_[2];
        victim->steer = true;
        speed_ = 400;
        state_ = 0x183;
        break;
      default: return false;
    }
    return true;
  }
  void frame() override {
    FxModel* victim = target(0);
    if (!victim) return;
    switch (state_) {
      case 0x180:
        for (size_t i = 0; i < 3; ++i) victim->pos[i] += v_[i];
        victim->steer = true;
        break;
      case 0x181:
      case 0x182: {
        // Toward the camera eye: VectorNormal(eye - A) * speed (4096 = 1.0, the position is 20.12 => speed units per tick)
        const Vec3 eye = fx_eye(ctx_.view);
        Vec3 to_eye{eye[0] - victim->pos[0], eye[1] - victim->pos[1], eye[2] - victim->pos[2]};
        const float length = std::sqrt(to_eye[0] * to_eye[0] + to_eye[1] * to_eye[1] + to_eye[2] * to_eye[2]);
        if (length > 1e-3f)
          for (size_t i = 0; i < 3; ++i) victim->pos[i] += to_eye[i] / length * static_cast<float>(speed_);
        const int spin[2][3] = {{10, 30, 40}, {0x32, 100, 0x8C}};
        for (int k = 0; k < 3; ++k) victim->rot[k] = (victim->rot[k] + spin[state_ - 0x181][k]) & 0xFFF;
        // The clamp compares the RAW 20.12 word: -10000 raw = -2.44 units, so 0x182 pins the target to the floor.
        if (state_ == 0x182 && victim->pos[1] < -10000.0f / 4096.0f) victim->pos[1] = -10000.0f / 4096.0f;
        victim->steer = true;
        break;
      }
      case 0x183: {
        const int step_speed = speed_;
        speed_ += 0x1E;
        victim->pos = {home_[0], victim->pos[1] + static_cast<float>(step_speed), home_[2]};
        victim->steer = true;
        if (victim->pos[1] > 0) {
          victim->pos[1] = 0;
          state_ = 0x184;
          // The last type-0x1A 'dead' entry.
          run_script(victim, "dead", 0x80, 7);
        }
        break;
      }
      default: break;
    }
  }

 private:
  int state_ = 0, speed_ = 0;
  Vec3 v_{0, 0, 0}, home_{0, 0, 0};
  int home_rot_[3] = {0, 0, 0};
};

// ---- 43acce --------------------------------------------------------------------------------------------------------------------------
// Init places five emitters on target 0 ('kaz-' 'a01-' 'jim-' at its position, 'rn1-' at y -895, 'rn0-' at y -1471). 0x180 throws
// it up (v = s16 @+8, y -= v per tick, v -= 5 per tick, landing clamps y = 0, v = 0); 0x181 puts it at y = -15249 falling at
// -s16 @+8 per tick (SetAnim 8, 1); 0x182 falls with v = -s16 @+8, v -= 10 per tick; 0x183 runs the 'dead' reaction.
class Acce43 : public OverlayFx {
 protected:
  void init() override {
    FxModel* victim = target(0);
    if (!victim || !em()) return;
    const int x = static_cast<int>(victim->pos[0]), y = static_cast<int>(victim->pos[1]), z = static_cast<int>(victim->pos[2]);
    em()->set_position("kaz-", x, y, z);
    em()->set_position("a01-", x, y, z);
    em()->set_position("jim-", x, y, z);
    em()->set_position("rn1-", x, static_cast<s16>(0xFC81), z);
    em()->set_position("rn0-", x, static_cast<s16>(0xFA41), z);
  }
  bool command(int op, const int* args, int count) override {
    FxModel* victim = target(0);
    if (!victim) return op >= 0x180 && op <= 0x183;
    switch (op) {
      case 0x180:  // v = s16 @+8, SetAnim(T, 7, 0x1E)
        v_ = arg(args, count, 8);
        set_anim(victim, 7, 0x1E);
        state_ = 0x180;
        break;
      case 0x181:  // v = -s16 @+8, y = -15249, SetAnim(T, 8, 1)
        state_ = 0x181;
        v_ = -arg(args, count, 8);
        victim->pos[1] = static_cast<float>(static_cast<s32>(0xFC471000u)) / 4096.0f;
        victim->steer = true;
        set_anim(victim, 8, 1);
        break;
      case 0x182:  // accelerating fall
        state_ = 0x182;
        v_ = -arg(args, count, 8);
        break;
      case 0x183:  // the last 'dead' table
        run_script(victim, "dead", 0x80, 7);
        break;
      default: return false;
    }
    return true;
  }
  // dt = 1.
  void frame() override {
    FxModel* victim = target(0);
    if (!victim) return;
    if (state_ == 0x181) {
      victim->pos[1] -= static_cast<float>(v_);
      victim->steer = true;
    } else if (state_ == 0x180 || state_ == 0x182) {
      victim->pos[1] -= static_cast<float>(v_);
      victim->steer = true;
      if (victim->pos[1] > 0) {
        victim->pos[1] = 0;
        v_ = 0;
      } else {
        v_ -= state_ == 0x180 ? 5 : 10;
      }
    }
  }

 private:
  int state_ = 0, v_ = 0;
};

// ---- 19helf / golem: per-target interpolation tasks ----------------------------------------------------------------------------------
// 19helf 0x100 (D = s16 @+4): each target slides from its position S to E = the centroid of the target list while its x scale falls
// 0x1000 -> 0 (scale = 0x1000 - (e << 12) / D with e before the step);
// SetAnim(T, 8, 0x20). 0x101 (D = @+4): back from the current position to the list entry's home, x scale (e << 12) / D up to 0x1000.
// Position p = S + (E - S) * e / D in whole units; e += 1 per tick; when e > D the task snaps to the end and frees itself.
class Helf19 : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100 && op != 0x101) return false;
    const int duration = arg(args, count, 4);
    // E =the midpoint of the targets' bounding box per axis ((max + min) / 2 on s16 values), not their mean
    Vec3 centre{0, 0, 0};
    const int target_total = target_count();
    for (size_t k = 0; k < 3; ++k) {
      int lo = 0x7FFF, hi = -0x7FFF;
      for (int i = 0; i < target_total; ++i) {
        const int coord = static_cast<int>(target(i)->pos[k]);
        lo = std::min(lo, coord), hi = std::max(hi, coord);
      }
      if (target_total > 0) centre[k] = static_cast<float>(trunc_div(hi + lo, 2));
    }
    for (int i = 0; i < target_total; ++i) {
      FxModel* victim = target(i);
      auto task = std::make_shared<Task>();
      task->model = victim;
      task->duration = duration;
      task->shrink = op == 0x100;
      for (size_t k = 0; k < 3; ++k) {
        task->start[k] = static_cast<int>(victim->pos[k]);
        task->end[k] = static_cast<int>(op == 0x100 ? centre[k] : victim->home[k]);
      }
      if (op == 0x100) set_anim(victim, 8, 0x20);
      // The handler runs the task once at once.
      step(*task);
      add_task([this, task] { return step(*task); });
    }
    return true;
  }

 private:
  struct Task {
    FxModel* model = nullptr;
    int tick = 0, duration = 1;
    int start[3]{}, end[3]{};
    bool shrink = true;
  };
  static bool step(Task& task) {
    if (task.tick < 0) return false;
    int pos[3];
    for (int i = 0; i < 3; ++i) pos[i] = static_cast<s16>(task.start[i] + trunc_div((task.end[i] - task.start[i]) * task.tick, task.duration));
    int sc = task.shrink ? 0x1000 - trunc_div(task.tick << 12, task.duration) : trunc_div(task.tick << 12, task.duration);
    task.tick += 1;
    bool alive = true;
    if (task.duration < task.tick) {
      for (int i = 0; i < 3; ++i) pos[i] = task.end[i];
      sc = task.shrink ? 0 : 0x1000;
      alive = false;
      task.tick = -1;
    }
    task.model->scale[0] = sc;
    task.model->pos = {static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2])};
    task.model->steer = true;
    return alive;
  }
};

// golem 0x100 (D = s16 @+4): every target loses its shadow, SetAnim(T, 8, 0x20), y scale 0x1000 -> 0x200 over D
// ticks (v = a + (b - a) * e / D, e += 1, snap to b when e > D). 0x101: shadow back, y scale 0x200 -> 0x1000.
class Golem : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100 && op != 0x101) return false;
    const int duration = arg(args, count, 4);
    for (int i = 0; i < target_count(); ++i) {
      FxModel* victim = target(i);
      auto task = std::make_shared<Task>();
      task->model = victim;
      task->duration = duration;
      task->from = op == 0x100 ? 0x1000 : 0x200;
      task->to = op == 0x100 ? 0x200 : 0x1000;
      if (op == 0x100) {
        flag_clear(victim, 0x200);
        set_anim(victim, 8, 0x20);
      } else {
        flag_set(victim, 0x200);
      }
      step(*task);
      add_task([task] { return step(*task); });
    }
    return true;
  }

 private:
  struct Task {
    FxModel* model = nullptr;
    int tick = 0, duration = 0, from = 0x1000, to = 0x1000, value = 0x1000;
    bool live = true;
  };
  static bool step(Task& task) {
    if (!task.live) return false;
    if (task.duration != 0) task.value = task.from + trunc_div((task.to - task.from) * task.tick, task.duration);
    task.tick += 1;
    if (task.duration < task.tick) {
      task.value = task.to;
      task.live = false;
    }
    task.model->scale[1] = task.value;
    task.model->steer = true;
    return task.live;
  }
};

}  // namespace

void register_model_steers(BespokeRegistry& registry) {
  registry.add({"41grav", "model-steer", "Grav41", [] { return std::make_unique<Grav41>(); }});
  registry.add({"43acce", "model-steer", "Acce43", [] { return std::make_unique<Acce43>(); }});
  registry.add({"19helf", "model-steer", "Helf19", [] { return std::make_unique<Helf19>(); }});
  registry.add({"golem", "model-steer", "Golem", [] { return std::make_unique<Golem>(); }});
}

}  // namespace oc::effects
