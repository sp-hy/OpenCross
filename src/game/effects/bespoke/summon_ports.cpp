// Summon overlays that steer the target models.
//   kuzira3  Blue Whale    op 0x100 throws each target out from the origin, op 0x101 brings them home
//   mido2    Genie         op 0x100 orbits the targets round their centroid, op 0x101 brings them home
//   enrou    Red Wolf      op 0x100 stops an emitter by name and expires its particles
//   hebi     Thunda Snake  op 0x100 the same, op 0x101 brings the third party member back
//   theend   Grim Reaper   op 0x180 sets a point, op 0x181 drifts the targets toward it, op 0x182 restores them
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "game/effects/bespoke/overlay_base.h"
#include "game/effects/fx_math.h"
#include "game/effects/sequence_player.h"

namespace oc::effects {
namespace {

int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }
int fixed(float units) { return static_cast<int>(std::lround(units * 4096.0f)); }
float unfixed(int value) { return static_cast<float>(value) / 4096.0f; }

// Centroid of the caster or target list as the sequence selectors compute it: the midpoint of the bounding box per axis, (max + min) / 2 on
// the integer model positions (C division), not their mean.
Vec3 centroid(FxModels* models, bool casters) {
  Vec3 centre{0, 0, 0};
  if (!models) return centre;
  const auto& list = casters ? models->casters : models->targets;
  if (list.empty()) return centre;
  for (size_t k = 0; k < 3; ++k) {
    int lo = 0x7FFF, hi = -0x7FFF;
    for (int unit : list) {
      const int value = static_cast<int>(models->units[static_cast<size_t>(unit)].pos[k]);
      lo = std::min(lo, value), hi = std::max(hi, value);
    }
    centre[k] = static_cast<float>(trunc_div(hi + lo, 2));
  }
  return centre;
}

// The return task shared by kuzira3 and mido2: the position converges on the list entry's home by cur + (home - cur) * t / dur (re-read
// every tick), the scale goes scale0 -> 0x1000 (kuzira3 only); on the last tick the rotation is (0, 0xFC00 - atan2(caster centroid -
// position), 0). On the other ticks the original writes the model's own (x, y, z) as s16 into the rotation words (a quirk of the code: the
// interpolated rotation is computed and then overwritten) - reproduced.
struct ReturnTask {
  FxModel* model = nullptr;
  FxModels* models = nullptr;
  int elapsed = 0, dur = 0, scale0 = 0x1000;
  bool scale = false, live = true;
  bool step() {
    if (!live) return false;
    const Vec3 caster_centre = centroid(models, true);
    const int mx = static_cast<int>(model->pos[0]), my = static_cast<int>(model->pos[1]), mz = static_cast<int>(model->pos[2]);
    const int yaw_to_casters = ratan2(static_cast<int>(caster_centre[2]) - mz, static_cast<int>(caster_centre[0]) - mx);
    int pos[3] = {mx, my, mz};
    int scale_now = model->scale[0];
    if (dur != 0) {
      for (int k = 0; k < 3; ++k) pos[k] = static_cast<s16>(pos[k] + trunc_div((static_cast<int>(model->home[static_cast<size_t>(k)]) - pos[k]) * elapsed, dur));
      scale_now = scale0 + trunc_div((0x1000 - scale0) * elapsed, dur);
    }
    elapsed += 1;
    if (elapsed > dur) {
      for (int k = 0; k < 3; ++k) pos[k] = static_cast<int>(model->home[static_cast<size_t>(k)]);
      scale_now = 0x1000;
      model->rot[0] = 0, model->rot[1] = (0xFC00 - yaw_to_casters) & 0xFFF, model->rot[2] = 0;
      live = false;
    } else {
      model->rot[0] = static_cast<s16>(mx) & 0xFFF, model->rot[1] = static_cast<s16>(my) & 0xFFF, model->rot[2] = static_cast<s16>(mz) & 0xFFF;
    }
    model->pos = {static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2])};
    if (scale) model->scale[0] = model->scale[1] = model->scale[2] = scale_now;
    model->steer = true;
    return live;
  }
};

// ---- kuzira3 -----------------------------------------------------------------------------------------------------------------------
// 0x100 (sent once per target from the per-target loop: the loop index picks the target): dur @+4, g = @+8 << 4, vy = @+10 << 6, start scale
// @+12, rotation rates @+14/+16/+18. The model is put at the origin, SetAnim(M, 8, 0x20); with N = targets + 1 and step = 0x1000 / N it flies
// out along angle step * index: per tick y -= vy, vy += g, x += rcos << 4, z += rsin << 4 (20.12), rot += rates, scale = (s0 - 0x400) * t /
// dur + 0x400 (0x1000 once t > dur, the task ends). 0x101 (dur @+4): the 0x100 tasks are killed and every target returns.
class Whale : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      const int target_index = ctx_.models ? ctx_.models->cursor : 0;
      FxModel* model = target(target_index);
      if (!model) return true;
      const int slots = target_count() + 1;
      const int angle_step = 0x1000 / slots;
      auto fly = std::make_shared<Fly>();
      fly->model = model;
      fly->dur = arg(args, count, 4);
      fly->gravity = arg(args, count, 8) << 4;
      fly->vy = arg(args, count, 10) << 6;
      fly->vx = rcos(angle_step * target_index) << 4;
      fly->vz = rsin(angle_step * target_index) << 4;
      fly->scale0 = arg(args, count, 12);
      for (int i = 0; i < 3; ++i) fly->rate[i] = arg(args, count, 14 + 2 * i);
      for (int i = 0; i < 3; ++i) fly->pos[i] = 0;
      model->pos = {0, 0, 0};
      model->steer = true;
      set_anim(model, 8, 0x20);
      fly->step();
      fly_.push_back(fly);
      add_task([fly] { return fly->step(); });
    } else if (op == 0x101) {
      for (auto& fly : fly_) fly->live = false;
      fly_.clear();
      for (int i = 0; i < target_count(); ++i) {
        auto ret = std::make_shared<ReturnTask>();
        ret->model = target(i);
        ret->models = ctx_.models;
        ret->dur = arg(args, count, 4);
        ret->scale0 = ret->model->scale[0];
        ret->scale = true;
        ret->step();
        add_task([ret] { return ret->step(); });
      }
    } else {
      return false;
    }
    return true;
  }

 private:
  struct Fly {
    FxModel* model = nullptr;
    int elapsed = 0, dur = 1, gravity = 0, vy = 0, vx = 0, vz = 0, scale0 = 0x1000;
    int rate[3] = {0, 0, 0};
    int pos[3] = {0, 0, 0};  // 20.12
    bool live = true;
    bool step() {
      if (!live) return false;
      int scale_now = dur != 0 ? trunc_div((scale0 - 0x400) * elapsed, dur) + 0x400 : 0x400;
      elapsed += 1;
      if (elapsed > dur) {
        scale_now = 0x1000;
        live = false;
      }
      model->scale[0] = model->scale[1] = model->scale[2] = scale_now;
      pos[1] -= vy;
      vy += gravity;
      pos[0] += vx;
      pos[2] += vz;
      for (int k = 0; k < 3; ++k) model->rot[k] = (model->rot[k] + rate[k]) & 0xFFF;
      model->pos = {unfixed(pos[0]), unfixed(pos[1]), unfixed(pos[2])};
      model->steer = true;
      return live;
    }
  };
  std::vector<std::shared_ptr<Fly>> fly_;
};

// ---- mido2 -------------------------------------------------------------------------------------------------------------------------
// 0x100 (dur @+4, angular velocity @+8, angular acceleration @+10, g @+12, vy @+14, rotation rates @+16/+18/+20): every target orbits the
// target centroid C: angle a0 = -ratan2(C.z - z, C.x - x), radius R = SquareRoot0 of the code's literal expression
// C.z - z * C.z - z + C.x - x * C.x - x (not a distance - reproduced, including the table read a negative value makes); SetAnim(M, 8, 0x20).
// Per tick: a += w >> 4, w += dw, y -= vy, vy += g, rot += rates; x = R rcos(a) + C.x << 12, z = R rsin(a) + C.z << 12.
// 0x101 (dur @+4): the return task without scale.
class Genie : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      const Vec3 centre = centroid(ctx_.models, false);
      const int cx = static_cast<int>(centre[0]), cz = static_cast<int>(centre[2]);
      for (int i = 0; i < target_count(); ++i) {
        FxModel* model = target(i);
        auto orbit = std::make_shared<Orbit>();
        orbit->model = model;
        orbit->dur = arg(args, count, 4);
        const int mx = static_cast<int>(model->pos[0]), mz = static_cast<int>(model->pos[2]);
        orbit->ang = static_cast<s16>(-ratan2(cz - mz, cx - mx));
        orbit->ang_vel = static_cast<s16>(arg(args, count, 8));
        orbit->ang_acc = static_cast<s16>(arg(args, count, 10));
        orbit->gravity = arg(args, count, 12);
        orbit->vy = arg(args, count, 14);
        orbit->radius = ps1_square_root0(cz - mz * cz - mz + cx - mx * cx - mx);
        orbit->cx = cx, orbit->cz = cz;
        orbit->y = fixed(model->pos[1]);
        for (int j = 0; j < 3; ++j) orbit->rate[j] = arg(args, count, 16 + 2 * j);
        set_anim(model, 8, 0x20);
        orbit->step();
        add_task([orbit] { return orbit->step(); });
      }
    } else if (op == 0x101) {
      for (int i = 0; i < target_count(); ++i) {
        auto ret = std::make_shared<ReturnTask>();
        ret->model = target(i);
        ret->models = ctx_.models;
        ret->dur = arg(args, count, 4);
        ret->step();
        add_task([ret] { return ret->step(); });
      }
    } else {
      return false;
    }
    return true;
  }

 private:
  struct Orbit {
    FxModel* model = nullptr;
    int elapsed = 0, dur = 0, gravity = 0, vy = 0, radius = 0, cx = 0, cz = 0, y = 0;
    s16 ang = 0, ang_vel = 0, ang_acc = 0;
    int rate[3] = {0, 0, 0};
    bool live = true;
    bool step() {
      if (!live) return false;
      elapsed += 1;
      // This tick is still applied.
      if (elapsed > dur) live = false;
      ang = static_cast<s16>(ang + (ang_vel >> 4));
      ang_vel = static_cast<s16>(ang_vel + ang_acc);
      y -= vy;
      vy += gravity;
      for (int k = 0; k < 3; ++k) model->rot[k] = (model->rot[k] + rate[k]) & 0xFFF;
      model->pos = {unfixed(radius * rcos(ang) + cx * 0x1000), unfixed(y), unfixed(radius * rsin(ang) + cz * 0x1000)};
      model->steer = true;
      return live;
    }
  };
};

// ---- enrou / hebi (whose init also starts the pack's `init` sequence): op 0x100 stops the emitter named by the s32 @+8 and lets its live particles expire on the next tick ------------------
// (life = 1 and flag 0x40000 on every live object of it).
class KillByName : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100) return false;
    if (!em()) return true;
    const u32 packed = static_cast<u16>(arg(args, count, 8)) | (static_cast<u32>(static_cast<u16>(arg(args, count, 10))) << 16);
    std::string name;
    for (int i = 0; i < 4; ++i) {
      const char ch = static_cast<char>((packed >> (8 * i)) & 0xFF);
      if (!ch) break;
      name.push_back(ch);
    }
    em()->stop(name);
    em()->for_each_object(name, [](EmitterSystem::ObjectRef& object) {
      if (*object.life != 0) {
        *object.life = 1;
        *object.flags |= 0x40000;
      }
    });
    return true;
  }
};

constexpr size_t kSoundGroupFileBytes = 72416;

// The overlay reads the pack's sound group before anything else: the `init` sequence holds the screen meanwhile and `main` starts when the file is in.
// The third party slot is released at init and reloaded at 0x101.
class Hebi : public KillByName {
 protected:
  void init() override {
    if (ctx_.models) ctx_.models->suppress_main = true;
    if (ctx_.sequences) ctx_.sequences->start("init");
    third_slot_.release(ctx_.models);
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x101) return KillByName::command(op, args, count);
    // The pack closes here: the sequences go, 'main' never reaches what follows this op.
    if (ctx_.sequences) ctx_.sequences->stop_all_after_pass();
    if (ctx_.models) ctx_.models->pack_closed = true;
    add_task([this] { return third_slot_.reload_tick(); });
    return true;
  }
  void frame() override {
    if (read_ticks_ == 0) return;
    if (--read_ticks_ > 0 || !ctx_.sequences) return;
    const FxModel* first_caster = caster(0);
    const FxModel* first_target = target(0);
    ctx_.sequences->start_main(first_caster && first_target && first_caster == first_target);
  }

 private:
  int read_ticks_ = disc_read_ticks(kSoundGroupFileBytes);
  ThirdSlotRelease third_slot_;
};

// ---- theend (the target part) ----------------------------------------------------------------------------------------------------------
// 0x180: target point P = s16 @+8/+10/+12. 0x181 (N = @+4): every target gets a start delay (0x1C49 - x) >> 6 and a velocity (P - home) / N;
// while delay < timer < delay + N it moves by velocity << 12 per tick and spins by its three random rates (rand() % 0x28, rolled at init).
// 0x182: positions and rotations restored. The shattering ground is EndShards in ground_ports.cpp.
class TheEnd : public OverlayFx {
 protected:
  void init() override {
    Rng rng;
    rng.state = ctx_.seed ? ctx_.seed : 1;
    for (int i = 0; i < target_count(); ++i) {
      Item item;
      item.model = target(i);
      item.home = item.model->pos;
      for (int k = 0; k < 3; ++k) item.home_rot[k] = item.model->rot[k], item.rate[k] = static_cast<int>(rng.next() % 0x28);
      items_.push_back(item);
    }
  }
  bool command(int op, const int* args, int count) override {
    if (op == 0x180) {
      point_[0] = arg(args, count, 8), point_[1] = arg(args, count, 10), point_[2] = arg(args, count, 12);
    } else if (op == 0x181) {
      on_ = true;
      timer_ = 0;
      const int ticks = std::max(1, arg(args, count, 4));
      duration_ = ticks;
      for (Item& item : items_) {
        item.delay = (0x1C49 - static_cast<int>(item.model->pos[0])) >> 6;
        for (int k = 0; k < 3; ++k) item.vel[k] = trunc_div(point_[k] - static_cast<int>(item.home[static_cast<size_t>(k)]), ticks);
      }
    } else if (op == 0x182) {
      on_ = false;
      for (Item& item : items_) {
        item.model->pos = item.home;
        for (int k = 0; k < 3; ++k) item.model->rot[k] = item.home_rot[k];
        item.model->steer = true;
      }
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    if (!on_) return;
    ++timer_;
    for (Item& item : items_) {
      if (item.delay < timer_ && timer_ < duration_ + item.delay) {
        for (size_t k = 0; k < 3; ++k) item.model->pos[k] += static_cast<float>(item.vel[k]);
        for (int k = 0; k < 3; ++k) item.model->rot[k] = (item.model->rot[k] + item.rate[k]) & 0xFFF;
        item.model->steer = true;
      }
    }
  }
  bool done() const override { return !on_; }

 private:
  struct Item {
    FxModel* model = nullptr;
    Vec3 home{0, 0, 0};
    int home_rot[3] = {0, 0, 0}, rate[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, delay = 0;
  };
  std::vector<Item> items_;
  int point_[3] = {0, 0, 0}, timer_ = 0, duration_ = 1;
  bool on_ = false;
};

}  // namespace

void register_summon_ports(BespokeRegistry& registry) {
  registry.add({"kuzira3", "summon", "Whale", [] { return std::make_unique<Whale>(); }});
  registry.add({"mido2", "summon", "Genie", [] { return std::make_unique<Genie>(); }});
  registry.add({"enrou", "summon", "KillByName", [] { return std::make_unique<KillByName>(); }});
  registry.add({"hebi", "summon", "Hebi", [] { return std::make_unique<Hebi>(); }});
  registry.add({"theend", "summon", "TheEnd", [] { return std::make_unique<TheEnd>(); }});
}

}  // namespace oc::effects
