// Technique / monster overlays that steer or re-draw the battle models.
//   shine    (Luminaire, Flamenco)  ops 0x100 / 0x101: caster 0 / 1 colour lerp
//   opthedo                         op 0x100 SetAnim on a caster, op 0x101 a target slides back from the caster and fades in
//   kamedes                         op 0x100: target 0 fades to black as a ghost pair, then stays hidden
//   k_monk                          ops 0x100 / 0x101 / 0x102: part visibility and SetAnim of target 0
//   soidash                         ops 0x100 / 0x101: casters' shadows off / on
//   bboy     (Surf's Up)            the target rides the 'bway' particle; op 0x100 faces / homes the targets
//   shock    (Limelight, Mix & Match, presents)  op 0x100 = kill emitter by name, op 0x101 hides the caster's side
//   uzumaki  (Vortex)               op 0x100 orbit-and-rise around caster 0, op 0x101 return
//   3mix     (Delta Force)          op 0x100 inward spiral around (+-0x800, 0, 0), op 0x101 return
//   godhand  (Gnarly)               op 0x100: target 0 re-skinned with a twisting wave for D ticks
//   tumaru   (Pip's form changes)   op 0x181 = the host's unit swap
// opthedo / kamedes draw the model as a ghost pair (additive own CLUT + subtractive white CLUT, model_draw.h).
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "game/effects/bespoke/mesh_tech.h"
#include "game/effects/fx_math.h"

namespace oc::effects {
namespace {

int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }
float unfixed(int value) { return static_cast<float>(value) / 4096.0f; }
int fixed(float units) { return static_cast<int>(std::lround(units * 4096.0f)); }
u8 lo_byte(int word) { return static_cast<u8>(word & 0xFF); }
u8 hi_byte(int word) { return static_cast<u8>((word >> 8) & 0xFF); }

// ---- shine -------------------------------------------------------------------------------------------------------------------------
// 0x100 (caster 0) / 0x101 (caster 1): D = s16 @+4, end colour = bytes @+8 / +9 / +10; c = c0 + (e - c0) * t / D (old t), t += 1, at t > D
// c = e and the task ends; written to the model colour.
class Shine : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100 && op != 0x101) return false;
    FxModel* model = caster(op == 0x100 ? 0 : 1);
    if (!model) return true;
    auto task = std::make_shared<Task>();
    task->model = model;
    task->dur = arg(args, count, 4);
    const int end_rgb[3] = {lo_byte(arg(args, count, 8)), hi_byte(arg(args, count, 8)), lo_byte(arg(args, count, 10))};
    for (int i = 0; i < 3; ++i) task->start_rgb[i] = model->rgb[i], task->end_rgb[i] = end_rgb[i];
    task->step();
    add_task([task] { return task->step(); });
    return true;
  }

 private:
  struct Task {
    FxModel* model = nullptr;
    int elapsed = 0, dur = 0, start_rgb[3] = {128, 128, 128}, end_rgb[3] = {128, 128, 128};
    bool live = true;
    bool step() {
      if (!live) return false;
      int rgb[3];
      for (int i = 0; i < 3; ++i)
        rgb[i] = dur != 0 ? static_cast<u8>(start_rgb[i] + static_cast<s8>(trunc_div((end_rgb[i] - start_rgb[i]) * elapsed, dur))) : model->rgb[i];
      elapsed += 1;
      if (dur < elapsed) {
        for (int i = 0; i < 3; ++i) rgb[i] = end_rgb[i];
        live = false;
      }
      for (int i = 0; i < 3; ++i) model->rgb[i] = rgb[i];
      model->tinted = true;
      return live;
    }
  };
};

// ---- opthedo / kamedes: the ghost pass pair --------------------------------------------------------------------------------------
// Both draw a model twice through its stock draw hook with the semi-transparent model flag set: pass 1 with the tpage ABR 1 (additive) and
// the model's own CLUT, pass 2 ABR 2 (B - F) with the battle's global white CLUT row (index 0 transparent, 1..255 white; a second all-white
// row is chosen by a unit flag OpenCross does not track). Pass 2 is added after pass 1, so it runs first in each OT entry: the silhouette
// darkened by the colour, then the texture added - a ghost of colour c (both passes use the model colour := c). Then the stock pass is
// hidden. The host sorts a pack's prims so that inside one slot the last submitted prim is drawn last, so pass 2 is submitted first here.
class GhostPair : public MeshTech {
 protected:
  void ghost_draw(const FxModel* model, std::vector<FxPrim>& out, u32& order) const {
    FxSkinnedModel skinned;
    if (!model || !skin(model, skinned)) return;
    ModelDrawSpec spec;
    spec.mask_part_flags = false;
    spec.flags = 0x2;
    spec.force_abr = 2;
    spec.white = 1;
    model_draw(skinned, *model, spec, ctx_.view, out, order);
    spec.force_abr = 1;
    spec.white = 0;
    model_draw(skinned, *model, spec, ctx_.view, out, order);
  }
  // The hook draws the shadow only when the model's drop-shadow flag (0x200) is set.
  static void set_colour(FxModel* model, int colour) {
    for (int k = 0; k < 3; ++k) model->rgb[k] = colour & 0xFF;
    model->tinted = true;
    own_draw(model, true, (model->flags & 0x200) != 0);
  }
};

// opthedo: 0x100: SetAnim(caster(s16 @+10), s16 @+8, 0x20). 0x101 (once per target-loop pass: the loop's target T): T takes caster 0's
// position and rotation; D = @+4, v = @+8; per tick T moves by RotMatrix(T rot) * (0, 0, v) * 0x100 * ticks (20.12) = v / 16 units along its
// facing; while t <= D: colour c = (t << 7) / D (so the ghost fades in from nothing) and the ghost pair; at t > D: anmseq 3 of its own S5
// block, shown again and released to the host, which takes it home from where it stands. Exit: every target stands on its home point with
// yaw 0xC00.
class Opthedo : public GhostPair {
 protected:
  void init() override {
    for (int i = 0; i < target_count(); ++i)
      if (FxModel* model = target(i)) model->exit_yaw = 0xC00;
  }
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      set_anim(caster(arg(args, count, 10)), arg(args, count, 8), 0x20);
    } else if (op == 0x101) {
      FxModel* caster_model = caster(0);
      FxModel* target_model = target(ctx_.models ? ctx_.models->cursor : 0);
      if (!caster_model || !target_model) return true;
      target_model->pos = caster_model->pos;
      for (int k = 0; k < 3; ++k) target_model->rot[k] = caster_model->rot[k];
      target_model->steer = true;
      target_model->face_rot = true;
      auto task = std::make_shared<Task>();
      task->model = target_model;
      task->dur = arg(args, count, 4);
      task->speed = arg(args, count, 8);
      set_colour(target_model, 0);
      live_.push_back(task);
      add_task([task, this] {
        const bool on = task->step();
        if (!on) live_.erase(std::remove(live_.begin(), live_.end(), task), live_.end());
        return on;
      });
    } else {
      return false;
    }
    return true;
  }
  void draw(std::vector<FxPrim>& out) const override {
    u32 order = 0;
    const size_t first = out.size();
    for (const auto& task : live_) ghost_draw(task->model, out, order);
    last_prims_ = out.size() - first;
  }

 private:
  struct Task {
    FxModel* model = nullptr;
    int elapsed = 0, dur = 1, speed = 0;
    // The slide runs along the unit's forward (-sin yaw, -cos yaw).
    bool step() {
      const Mat3 rotation = Mat3::from_angles(model->rot[0], model->rot[1], model->rot[2]);
      const Vec3 offset = rotation.apply({0, 0, static_cast<float>(speed)});
      model->pos[0] += unfixed(static_cast<int>(offset[0]) * 0x100);
      model->pos[2] += unfixed(static_cast<int>(offset[2]) * 0x100);
      model->steer = true;
      elapsed += 1;
      if (dur < elapsed) {
        model->script_entry = 3, model->script_variant = 0, model->script_table.clear();
        model->hidden = false;
        model->custom_draw = model->own_shadow = false;
        model->face_rot = false;
        model->reveal = model->release = true;
        return false;
      }
      set_colour(model, (elapsed << 7) / std::max(1, dur));
      return true;
    }
  };
  std::vector<std::shared_ptr<Task>> live_;
};

// kamedes: 0x100 (D = @+4): target 0 fades out, colour c = 0x80 - (t << 7) / D, through the ghost pair each tick; after D the task frees
// itself and the model stays hidden. Its op-0x59 prefix falls through to the stock op. Init puts target 0's idle-pose word at 8 (dead).
class Kamedes : public GhostPair {
 protected:
  void init() override {
    const FxModel* model = target(0);
    if (model && ctx_.hooks.set_pose) ctx_.hooks.set_pose(model->unit, 8);
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x100) return false;
    FxModel* model = target(0);
    if (!model) return true;
    model_ = model;
    tick_ = 0;
    duration_ = std::max(1, arg(args, count, 4));
    add_task([this] {
      if (duration_ < ++tick_) {
        on_ = false;
        model_->custom_draw = model_->own_shadow = false;
        return false;
      }
      on_ = true;
      set_colour(model_, 0x80 - ((tick_ << 7) / duration_));
      return true;
    });
    return true;
  }
  void draw(std::vector<FxPrim>& out) const override {
    if (!on_) return;
    u32 order = 0;
    const size_t first = out.size();
    ghost_draw(model_, out, order);
    last_prims_ = out.size() - first;
  }

 private:
  FxModel* model_ = nullptr;
  int tick_ = 0, duration_ = 1;
  bool on_ = false;
};

// ---- k_monk ------------------------------------------------------------------------------------------------------------------------
// target 0: 0x100 hides parts 2..5, shows 0, 1, 6, 7 and turns its shadow on; 0x101 SetAnim(M, s16 @+8, s16 @+4); 0x102 hides parts 0..7.
class KMonk : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100 && op != 0x101 && op != 0x102) return false;
    FxModel* model = target(0);
    if (!model) return true;
    if (op == 0x100) {
      model->part_hide = (model->part_hide | 0x3Cu) & ~0xC3u;
      model->part_show = (model->part_show | 0xC3u) & ~0x3Cu;
      flag_set(model, 0x200);
    } else if (op == 0x101) {
      set_anim(model, arg(args, count, 8), arg(args, count, 4));
    } else if (op == 0x102) {
      model->part_hide |= 0xFFu;
      model->part_show &= ~0xFFu;
    }
    return true;
  }
};

// ---- soidash -----------------------------------------------------------------------------------------------------------------------
class Soidash : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    (void)args;
    (void)count;
    if (op != 0x100 && op != 0x101) return false;
    if (!ctx_.models) return true;
    for (size_t i = 0; i < ctx_.models->casters.size(); ++i) (op == 0x100 ? flag_clear : flag_set)(caster(static_cast<int>(i)), 0x200);
    return true;
  }
};

// ---- bboy --------------------------------------------------------------------------------------------------------------------------
// 'bway' gets a replacement draw callback through its op 0x48 record: the sprite is not drawn, the model of its op-0x39 selector (2 = the
// loop's target) is shown and put at the particle (pos >> 4). 0x100: every target faces 0xC00 and stands on its home; the particle's basis is
// that target, so the wave always runs along +x, whichever side the targets are on.
class Bboy : public OverlayFx {
 protected:
  void init() override {
    if (!em()) return;
    em()->set_object_hook("bway", [this](EmitterSystem::ObjectRef& object) {
      // 2 / 0xD = target 0, 0xD + n = target n: each target's clone of the emitter carries its own selector.
      FxModel* model = target(object.basis_sel > 0xD ? object.basis_sel - 0xD : 0);
      if (!model) return;
      model->hidden = false;
      model->reveal = true;
      model->pos = {static_cast<float>(object.pos[0]) / 65536.0f, static_cast<float>(object.pos[1]) / 65536.0f, static_cast<float>(object.pos[2]) / 65536.0f};
      model->steer = true;
      last_ride_[static_cast<size_t>(model->unit)] = ticks_;
    }, true);
    em()->set_draw_dropped("bway", true);
  }
  // A rider whose particle has died stays where it landed, with the yaw of the ride: the unit takes over that placement.
  void frame() override {
    ++ticks_;
    for (int i = 0; i < target_count(); ++i) {
      FxModel* model = target(i);
      int& last = last_ride_[static_cast<size_t>(model->unit)];
      if (last == 0 || ticks_ - last < kRideGapTicks) continue;
      last = 0;
      model->release = true;
    }
  }
  bool command(int op, const int* args, int count) override {
    (void)args;
    (void)count;
    if (op != 0x100) return false;
    for (int i = 0; i < target_count(); ++i) {
      FxModel* model = target(i);
      model->rot[1] = 0xC00;
      model->pos = model->home;
      model->steer = true;
    }
    return true;
  }

 private:
  static constexpr int kRideGapTicks = 4;
  std::array<int, 16> last_ride_{};
  int ticks_ = 1;
};

// ---- shock -------------------------------------------------------------------------------------------------------------------------
// 0x100: the emitter named by the s32 @+8 is stopped and its live particles expire on the next tick; 0x101 hides every other unit of the
// caster's side.
class Shock : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      if (!em()) return true;
      const u32 packed = static_cast<u16>(arg(args, count, 8)) | (static_cast<u32>(static_cast<u16>(arg(args, count, 10))) << 16);
      std::string name;
      for (int i = 0; i < 4 && ((packed >> (8 * i)) & 0xFF); ++i) name.push_back(static_cast<char>((packed >> (8 * i)) & 0xFF));
      if (name.empty()) return true;
      em()->stop(name);
      em()->for_each_object(name, [](EmitterSystem::ObjectRef& object) {
        if (*object.life != 0) *object.life = 1, *object.flags |= 0x40000;
      });
    } else if (op == 0x101) {
      if (!ctx_.models) return true;
      const FxModel* caster_model = caster(0);
      if (!caster_model) return true;
      for (FxModel& model : ctx_.models->units)
        if (&model != caster_model && model.unit >= 0 && model.pc == caster_model->pc) model.hidden = true;
    } else {
      return false;
    }
    return true;
  }
};

// ---- uzumaki / 3mix: orbit tasks + the shared return task ----------------------------------------------------------------------------
// The position converges on the home by cur + (home - cur) t / D; at the end the model faces the casters' point (0, 0xFC00 - yaw, 0), yaw
// taken from where the model stood before that last step. The casters' point is the acting-list selector's: (max + min) / 2 per axis of
// their live positions, not the mean. While running the shipped code writes the model's own position into the rotation words (3mix: only y
// into the yaw) - reproduced.
struct Return {
  FxModel* model = nullptr;
  FxModels* models = nullptr;
  int elapsed = 0, dur = 0;
  bool mix = false, live = true;
  // Without a host the casters' model positions stand in for their live ones.
  Vec3 casters_point() const {
    Vec3 point{0, 0, 0};
    if (models->point && models->point(3, 0, point)) return point;
    if (models->casters.empty()) return point;
    for (size_t k = 0; k < 3; ++k) {
      int lo = 0x7FFF, hi = -0x7FFF;
      for (int unit : models->casters) {
        const int value = static_cast<s16>(static_cast<int>(models->units[static_cast<size_t>(unit)].pos[k]));
        lo = std::min(lo, value), hi = std::max(hi, value);
      }
      point[k] = static_cast<float>((hi + lo) / 2);
    }
    return point;
  }
  // The original runs this task once per 30 Hz pass with t += 2 and faces the model from the pose of its last pass. A copy of the
  // path on that schedule supplies the final yaw, while the drawn model moves at 60 Hz.
  int pass_pos[3] = {0, 0, 0};
  int pass_elapsed = 0, pass_yaw = 0, ticks = 0;
  void step_pass(const Vec3& caster_centre) {
    if (ticks == 0)
      for (int k = 0; k < 3; ++k) pass_pos[k] = static_cast<int>(model->pos[static_cast<size_t>(k)]);
    if (ticks++ % 2 != 0 || dur < pass_elapsed) return;
    pass_yaw = ratan2(static_cast<int>(caster_centre[2]) - pass_pos[2], static_cast<int>(caster_centre[0]) - pass_pos[0]);
    if (dur != 0)
      for (int k = 0; k < 3; ++k)
        pass_pos[k] = static_cast<s16>(pass_pos[k] + trunc_div((static_cast<int>(model->home[static_cast<size_t>(k)]) - pass_pos[k]) * pass_elapsed, dur));
    pass_elapsed += 2;
  }
  bool step() {
    if (!live) return false;
    const Vec3 caster_centre = casters_point();
    step_pass(caster_centre);
    const int mx = static_cast<int>(model->pos[0]), my = static_cast<int>(model->pos[1]), mz = static_cast<int>(model->pos[2]);
    int pos[3] = {mx, my, mz};
    if (dur != 0)
      for (int k = 0; k < 3; ++k) pos[k] = static_cast<s16>(pos[k] + trunc_div((static_cast<int>(model->home[static_cast<size_t>(k)]) - pos[k]) * elapsed, dur));
    elapsed += 1;
    if (dur < elapsed) {
      for (int k = 0; k < 3; ++k) pos[k] = static_cast<int>(model->home[static_cast<size_t>(k)]);
      model->rot[0] = 0, model->rot[1] = (0xFC00 - pass_yaw) & 0xFFF, model->rot[2] = 0;
      live = false;
    } else if (mix) {
      model->rot[0] = 0, model->rot[1] = my & 0xFFF, model->rot[2] = 0;
    } else {
      model->rot[0] = mx & 0xFFF, model->rot[1] = my & 0xFFF, model->rot[2] = mz & 0xFFF;
    }
    model->pos = {static_cast<float>(pos[0]), static_cast<float>(pos[1]), static_cast<float>(pos[2])};
    model->steer = true;
    return live;
  }
};

// Per tick: ang += ang_vel >> 4, ang_vel += ang_acc, the model rises (y -= vy; spiral: y += vy * -0x40), vy += acc, rot += rates, x / z =
// radius cos / sin ang + centre. spiral (3mix): the radius shrinks radius - radius t / D to 0.
struct Orbit {
  FxModel* model = nullptr;
  int elapsed = 0, dur = 0, radius = 0, cx = 0, cz = 0, y = 0, vy = 0, acc = 0;
  s16 ang = 0, ang_vel = 0, ang_acc = 0;
  int rate[3] = {0, 0, 0};
  bool spiral = false, live = true;
  bool step() {
    if (!live) return false;
    int radius_now = radius;
    if (spiral) radius_now = radius - trunc_div(radius * elapsed, std::max(1, dur));
    elapsed += 1;
    if (dur < elapsed) {
      live = false;
      if (spiral) radius_now = 0;
    }
    ang = static_cast<s16>(ang + (ang_vel >> 4));
    ang_vel = static_cast<s16>(ang_vel + ang_acc);
    if (spiral) y += vy * -0x40;
    else y -= vy;
    vy += acc;
    for (int k = 0; k < 3; ++k) model->rot[k] = (model->rot[k] + rate[k]) & 0xFFF;
    model->pos = {unfixed(radius_now * rcos(ang) + cx * 0x1000), unfixed(y), unfixed(radius_now * rsin(ang) + cz * 0x1000)};
    model->steer = true;
    return live;
  }
};

// uzumaki 0x100: centre = caster 0; per target k: angle ratan2(Tz - cz, Tx - cx), w0 = -(s16 @+0x18) * (k + 1), dw = -(s16 @+10), acc = @+12,
// vy = @+14, R = 2 * SquareRoot0(cz - Tz cz - Tz + cx - Tx cx - Tx) (the code's literal expression), rates @+0x10/12/14, SetAnim(T, 8, @+0x16).
// 0x101 (D @+4): return.
class Uzumaki : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      const FxModel* caster_model = caster(0);
      if (!caster_model) return true;
      const int cx = static_cast<s16>(static_cast<int>(caster_model->pos[0])), cz = static_cast<s16>(static_cast<int>(caster_model->pos[2]));
      for (int i = 0; i < target_count(); ++i) {
        FxModel* model = target(i);
        auto orbit = std::make_shared<Orbit>();
        orbit->model = model;
        orbit->dur = arg(args, count, 4);
        const int tx = static_cast<int>(model->pos[0]), tz = static_cast<int>(model->pos[2]);
        orbit->ang = static_cast<s16>(ratan2(tz - cz, tx - cx));
        orbit->ang_vel = static_cast<s16>(-arg(args, count, 0x18) * (i + 1));
        orbit->ang_acc = static_cast<s16>(-arg(args, count, 10));
        orbit->acc = arg(args, count, 12);
        orbit->vy = arg(args, count, 14);
        orbit->radius = 2 * ps1_square_root0(cz - tz * cz - tz + cx - tx * cx - tx);
        orbit->cx = cx, orbit->cz = cz;
        orbit->y = fixed(model->pos[1]);
        for (int j = 0; j < 3; ++j) orbit->rate[j] = arg(args, count, 0x10 + 2 * j);
        set_anim(model, 8, arg(args, count, 0x16));
        orbit->step();
        add_task([orbit] { return orbit->step(); });
      }
    } else if (op == 0x101) {
      add_returns(arg(args, count, 4), false);
    } else {
      return false;
    }
    return true;
  }
  void add_returns(int dur, bool mix) {
    for (int i = 0; i < target_count(); ++i) {
      auto ret = std::make_shared<Return>();
      ret->model = target(i);
      ret->models = ctx_.models;
      ret->dur = dur;
      ret->mix = mix;
      ret->step();
      add_task([ret] { return ret->step(); });
    }
  }
};

// 3mix 0x100: centre = (-0x800 if target 0's x <= 0 else +0x800, 0, 0); R = SquareRoot0(Tz^2 + Tx^2) of the absolute position, angle
// ratan2(Tz - cz, Tx - cx), w0 = -(@+0x18) (k + 1), dw = -(@+10), acc @+12, vy @+14, rates @+0x10.., SetAnim(T, 8, @+0x16); the radius shrinks
// R - R t / D to 0 and y += vy * -0x40. 0x101 (D @+4): the return with rot y = y while running.
class Mix3 : public Uzumaki {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op == 0x100) {
      const FxModel* target0 = target(0);
      if (!target0) return true;
      const int cx = target0->pos[0] <= 0 ? -0x800 : 0x800, cz = 0;
      for (int i = 0; i < target_count(); ++i) {
        FxModel* model = target(i);
        auto orbit = std::make_shared<Orbit>();
        orbit->model = model;
        orbit->spiral = true;
        orbit->dur = arg(args, count, 4);
        const int tx = static_cast<int>(model->pos[0]), tz = static_cast<int>(model->pos[2]);
        orbit->ang = static_cast<s16>(ratan2(tz - cz, tx - cx));
        orbit->ang_vel = static_cast<s16>(-arg(args, count, 0x18) * (i + 1));
        orbit->ang_acc = static_cast<s16>(-arg(args, count, 10));
        orbit->acc = arg(args, count, 12);
        orbit->vy = arg(args, count, 14);
        orbit->radius = ps1_square_root0(tz * tz + tx * tx);
        orbit->cx = cx, orbit->cz = cz;
        orbit->y = fixed(model->pos[1]);
        for (int j = 0; j < 3; ++j) orbit->rate[j] = arg(args, count, 0x10 + 2 * j);
        set_anim(model, 8, arg(args, count, 0x16));
        orbit->step();
        add_task([orbit] { return orbit->step(); });
      }
    } else if (op == 0x101) {
      add_returns(arg(args, count, 4), true);
    } else {
      return false;
    }
    return true;
  }
};

// ---- godhand -----------------------------------------------------------------------------------------------------------------------
// 0x100: D = @+4, A = @+12, S = @+14; for D ticks target 0 is drawn by the overlay (stock draw hidden) with every skinned world vertex
// moved by y' = y + (rsin(P + (y << S)) * A >> 12), z' = z + (rcos(P + (y' << S)) * A >> 12) (y' already updated), P += 0x40 per tick.
class Godhand : public OverlayFx {
 protected:
  bool command(int op, const int* args, int count) override {
    if (op != 0x100) return false;
    on_ = true;
    tick_ = 0;
    duration_ = arg(args, count, 4);
    amp_ = arg(args, count, 12);
    shift_ = arg(args, count, 14);
    return true;
  }
  void frame() override {
    FxModel* model = target(0);
    if (!model) return;
    model->copies.clear();
    if (!on_) {
      model->hidden = false;
      return;
    }
    ++tick_;
    phase_ += 0x40;
    FxModel::Copy copy;
    const int phase = phase_, amp = amp_, shift = shift_;
    copy.deform = [phase, amp, shift](Vec3& vertex) {
      const int y = static_cast<s16>(std::lround(vertex[1])), z = static_cast<s16>(std::lround(vertex[2]));
      const int y1 = static_cast<s16>(y + ((rsin(phase + (y << shift)) * amp) >> 12));
      const int z1 = static_cast<s16>(z + ((rcos(phase + (y1 << shift)) * amp) >> 12));
      vertex[1] = static_cast<float>(y1);
      vertex[2] = static_cast<float>(z1);
    };
    model->copies.push_back(std::move(copy));
    model->hidden = true;
    if (duration_ < tick_) on_ = false;
  }
  bool done() const override { return !on_; }

 private:
  bool on_ = false;
  int tick_ = 0, duration_ = 0, amp_ = 0, shift_ = 0, phase_ = 0;
};

// ---- tumaru (Pip's form changes: base2p base2m p2pp p2pm m2pm m2mm) ---------------------------------------------------------------
// The live overlay draws nothing (the ghost-copy code in it is never called): init starts every caster's external anmseq entry 0x80 with
// its list index (that script starts the 'chng' sequence), hides the pack's own 'act0' actor and shows caster 0 (OpenCross draws no extra
// actor); ops 0x180 / 0x181 only call the host's unit swap of Pip's new form: 0x180 arms it with the unit already hidden by the 'chng' entry,
// so nothing shows and the swap needs no state ahead of 0x181, where the new model is bound.
class Tumaru : public OverlayFx {
 protected:
  void init() override {
    for (int i = 0; FxModel* model = caster(i); ++i) run_script(model, "", 0x80, i);
  }
  bool command(int op, const int* args, int count) override {
    if (op == 0x181 && ctx_.hooks.form_swap) ctx_.hooks.form_swap(2);
    const bool handled = OverlayFx::command(op, args, count);
    return handled || op == 0x180 || op == 0x181;
  }
};

}  // namespace

void register_tech_ports(BespokeRegistry& registry) {
  registry.add({"shine", "model-steer", "Shine", [] { return std::make_unique<Shine>(); }});
  registry.add({"opthedo", "model-steer", "Opthedo", [] { return std::make_unique<Opthedo>(); }});
  registry.add({"kamedes", "model-deform", "Kamedes", [] { return std::make_unique<Kamedes>(); }});
  registry.add({"k_monk", "model-steer", "KMonk", [] { return std::make_unique<KMonk>(); }});
  registry.add({"soidash", "model-steer", "Soidash", [] { return std::make_unique<Soidash>(); }});
  registry.add({"bboy", "emitter-patch", "Bboy", [] { return std::make_unique<Bboy>(); }});
  registry.add({"shock", "emitter-patch", "Shock", [] { return std::make_unique<Shock>(); }});
  registry.add({"uzumaki", "model-steer", "Uzumaki", [] { return std::make_unique<Uzumaki>(); }});
  registry.add({"3mix", "model-steer", "Mix3", [] { return std::make_unique<Mix3>(); }});
  registry.add({"tumaru", "shell", "Tumaru", [] { return std::make_unique<Tumaru>(); }});
  registry.add({"godhand", "model-deform", "Godhand", [] { return std::make_unique<Godhand>(); }});
}

}  // namespace oc::effects
