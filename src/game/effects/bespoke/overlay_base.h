#pragma once
// Base of the overlay ports: the overlays read and write the battle model blocks of the caster /
// target lists (FxModels), patch the pack's emitters (EmitterSystem overlay hooks) and run per-object frame tasks.
// Operands: the private sequence ops hand the overlay the command record; `arg(a, n, off)` reads the s16 at
// record offset `off` (the host passes the s16 words from record +4 on).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "assets/room_effects.h"
#include "core/log.h"
#include "game/effects/bespoke/bespoke.h"
#include "game/effects/emitter_system.h"

namespace oc::effects {

// The biggest summons take the third party slot's memory: from the overlay's init the unit is off the stage, whatever the sequence's hide ops
// say, until the overlay reloads its model. The reload (the disc read of the unit's model files plus the swap stages) holds the effect's end;
// the unit then comes back with a new model block, even while the sequence still hides the party, and plays its spawn script. An empty slot
// lends nothing.
constexpr int kPartyModelReadTicks = 20;  // a mid-sized model, without a host to ask
constexpr int kPartyModelSwapTicks = 6;

class ThirdSlotRelease {
 public:
  void release(FxModels* models) {
    constexpr size_t kThirdSlot = 2;
    if (!models || models->units.size() <= kThirdSlot) return;
    FxModel& model = models->units[kThirdSlot];
    if (!model.pc || model.hidden) return;
    model.hidden = model.unloaded = true;
    unit_ = &model;
    models_ = models;
  }
  // One tick of the reload as a frame task: true until the unit is back.
  bool reload_tick() {
    if (!unit_) return false;
    if (reload_left_ < 0) reload_left_ = (models_->model_read_ticks ? models_->model_read_ticks(unit_->unit) : kPartyModelReadTicks) + kPartyModelSwapTicks;
    if (reload_left_-- > 0) return true;
    unit_->hidden = unit_->unloaded = false, unit_->rebuilt = true;
    unit_ = nullptr;
    return false;
  }

 private:
  FxModels* models_ = nullptr;
  FxModel* unit_ = nullptr;
  int reload_left_ = -1;
};

class OverlayFx : public BespokeEffect {
 public:
  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    init();
  }
  void tick() override {
    frame();
    // Frame tasks run until they return false (which frees them).
    for (size_t i = 0; i < tasks_.size();) {
      if (tasks_[i]()) ++i;
      else tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    trace();
  }
  void draw(std::vector<FxPrim>& out) const override { (void)out; }
  // The overlay never ends the effect itself: the sequence does. Pending frame tasks keep it alive.
  bool done() const override { return tasks_.empty(); }

 protected:
  virtual void init() {}
  // cmd 1
  virtual void frame() {}

  static int arg(const int* args, int count, int off) {
    const int i = (off - 4) / 2;
    return i >= 0 && i < count ? args[i] : 0;
  }
  FxModel* target(int i) const { return ctx_.models ? ctx_.models->target(i) : nullptr; }
  FxModel* caster(int i) const { return ctx_.models ? ctx_.models->caster(i) : nullptr; }
  int target_count() const { return ctx_.models ? static_cast<int>(ctx_.models->targets.size()) : 0; }
  EmitterSystem* em() const { return ctx_.emitters; }
  // Header i belongs to the pack itself (the overlays iterate the resources of their owner, not the particle library).
  bool own_header(size_t i) const {
    if (!em() || !ctx_.pack_fx || ctx_.pack_fx->emitters.empty()) return false;
    const assets::Emitter* def = &em()->header_def(i);
    return def >= ctx_.pack_fx->emitters.data() && def < ctx_.pack_fx->emitters.data() + ctx_.pack_fx->emitters.size();
  }
  void add_task(std::function<bool()> task) { tasks_.push_back(std::move(task)); }
  // SetAnim / anmseq requests (consumed by the host after the tick).
  static void set_anim(FxModel* model, int clip, int flags) {
    if (model) model->clip = clip, model->clip_flags = flags;
  }
  static void run_script(FxModel* model, const std::string& table, int entry, int variant) {
    if (model) model->script_table = table, model->script_entry = entry, model->script_variant = variant;
  }
  static void flag_set(FxModel* model, u16 bits) {
    if (model) model->flags = static_cast<u16>(model->flags | bits);
  }
  static void flag_clear(FxModel* model, u16 bits) {
    if (model) model->flags = static_cast<u16>(model->flags & ~bits);
  }
  // The world matrix of the joint that attach point k names; the overlays read its translation =
  // that joint's origin ("bone k"). PS1 world units, y down: the host's posed joint (model space) through the model's own placement,
  // the rotation matrix with the scale, plus the position. False when the unit has no posed model / the point is unused.
  static bool joint_world(const FxModel* model, int joint, Vec3& out) {
    Vec3 local{0, 0, 0};
    if (!model || !model->joint_local || !model->joint_local(joint, local)) return false;
    const Mat3 rotation = Mat3::from_angles(model->rot[0], model->rot[1], model->rot[2]);
    for (size_t k = 0; k < 3; ++k) local[k] *= static_cast<float>(model->scale[k]) / 4096.0f;
    const Vec3 world = rotation.apply(local);
    for (size_t k = 0; k < 3; ++k) out[k] = model->pos[k] + world[k];
    return true;
  }

  // OC_BESPOKE_TRACE=1: every 10 ticks one line per steered model (position, rotation, scale, colour, flags) and the task count.
  void trace() {
    static const bool on = std::getenv("OC_BESPOKE_TRACE") != nullptr;
    if (!on || (ticks_++ % 10) != 0 || !ctx_.models) return;
    log::debug("bespoke {} tick {}: tasks {} prims {}", label(), ticks_ - 1, tasks_.size(), last_prims_);
    for (const FxModel& model : ctx_.models->units)
      if (model.steer || model.tinted || !model.copies.empty())
        log::debug("  unit {} pos {:.0f} {:.0f} {:.0f} rot {} {} {} scale {} {} {} rgb {} {} {} flags {:04X} hidden {} copies {}", model.unit, model.pos[0],
                   model.pos[1], model.pos[2], model.rot[0], model.rot[1], model.rot[2], model.scale[0], model.scale[1], model.scale[2], model.rgb[0],
                   model.rgb[1], model.rgb[2], model.flags, model.hidden, model.copies.size());
  }

  std::vector<std::function<bool()>> tasks_;
  int ticks_ = 0;
  mutable size_t last_prims_ = 0;  // set by drawing ports
};

}  // namespace oc::effects
