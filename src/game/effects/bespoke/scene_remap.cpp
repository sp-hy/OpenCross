// Scene remaps: overlays that replace the caster / target list getters the sequence ops iterate, swap the stage pack, or write one model
// word. The list overrides go through FxModels::caster_override / target_override (BattleFxRun feeds them to the selectors and the
// per-target loops).
//   seint     Sonja's summon: the caster stands in for every opposite-side target
//   unico     the caster list becomes the unit nearest to caster 0's side point
//   etrap     the two lists swapped
//   bgchng    no shipped sequence reaches its ops: draws nothing
//   miyademo  cycles the backdrop through nine stages
//   hinsi / lasdes4  write target 0's idle-pose word only
#include <memory>

#include "game/effects/bespoke/overlay_base.h"
#include "game/effects/sequence_player.h"
#include "game/effects/fx_math.h"

namespace oc::effects {
namespace {

class Remap : public OverlayFx {
 protected:
  std::vector<int> stock_targets() const { return ctx_.models ? ctx_.models->targets : std::vector<int>{}; }
  std::vector<int> stock_casters() const { return ctx_.models ? ctx_.models->casters : std::vector<int>{}; }
  void set_lists(const std::vector<int>* casters, const std::vector<int>* targets) {
    if (!ctx_.models) return;
    if (casters) ctx_.models->caster_override = true, ctx_.models->caster_list = *casters;
    if (targets) ctx_.models->target_override = true, ctx_.models->target_list = *targets;
  }
  const FxModel* unit_model(int unit) const {
    return ctx_.models && unit >= 0 && static_cast<size_t>(unit) < ctx_.models->units.size() ? &ctx_.models->units[static_cast<size_t>(unit)] : nullptr;
  }
};

// seint: init snapshots the stock target list; the target getter returns copies of caster 0's entry, one per target of the other side (a
// monster target for a PC caster, a PC target for a monster caster), at least one; the caster list is empty for the whole effect. 0x100
// refilters the snapshot: the targets healed by the action (hit == 3) and not dead; 0x101: the targets with hit != 3. The getters are
// restored at exit. The unpacked 'comp' pack's model 'ee97' carries its mesh 'yari' on mount record 0.
class Seint : public Remap {
 protected:
  void init() override {
    if (ctx_.models) ctx_.models->mount_model = "ee97", ctx_.models->mount_mesh = "yari";
    snap_ = stock_targets();
    const std::vector<int> casters = stock_casters();
    caster0_ = casters.empty() ? -1 : casters.front();
    std::vector<int> filtered;
    const FxModel* caster_model = unit_model(caster0_);
    for (int unit : snap_) {
      const FxModel* target_model = unit_model(unit);
      if (caster_model && target_model && caster_model->pc == !target_model->pc) filtered.push_back(caster0_);
    }
    if (filtered.empty() && caster0_ >= 0) filtered.push_back(caster0_);
    const std::vector<int> none;
    set_lists(&none, &filtered);
  }
  bool command(int op, const int* args, int count) override {
    (void)args;
    (void)count;
    if (op != 0x100 && op != 0x101) return false;
    std::vector<int> filtered;
    for (int unit : snap_) {
      const FxModel* target_model = unit_model(unit);
      if (!target_model) continue;
      if (op == 0x100 ? (target_model->hit == 3 && !target_model->dead) : target_model->hit != 3) filtered.push_back(unit);
    }
    set_lists(nullptr, &filtered);
    return true;
  }

 private:
  std::vector<int> snap_;
  int caster0_ = -1;
};

// unico: the reference point R = the side point of caster 0 ((-0x800, 0, 0) when its x < 1, else (0x800, 0, 0)); the caster list becomes
// the one unit (of everyone, in registration order) whose home is nearest to R (SquareRoot0 of the squared distance, strictly less than the
// best so far from 0x7FFF); the target list stays the same targets (as copies). Restored at exit.
std::vector<int> nearest_to_side(const FxModels& models, const FxModel* caster0) {
  const int side_x = caster0 && caster0->pos[0] < 1.0f / 4096.0f ? -0x800 : 0x800;
  int best = 0x7FFF, best_unit = -1;
  for (const FxModel& model : models.units) {
    if (model.unit < 0 || model.hidden) continue;
    const int dx = side_x - static_cast<int>(model.home[0]), dy = -static_cast<int>(model.home[1]), dz = -static_cast<int>(model.home[2]);
    const int distance = ps1_square_root0(dx * dx + dy * dy + dz * dz);
    if (distance < best) best = distance, best_unit = model.unit;
  }
  return best_unit >= 0 ? std::vector<int>{best_unit} : std::vector<int>{};
}
class Unico : public Remap {
 protected:
  void init() override {
    if (!ctx_.models) return;
    const std::vector<int> targets = stock_targets(), casters = nearest_to_side(*ctx_.models, caster(0));
    set_lists(&casters, &targets);
  }
};

// etrap: the two lists swapped for the effect (the target getter returns copies of the casters, the caster getter copies of the targets).
class Etrap : public Remap {
 protected:
  void init() override {
    const std::vector<int> targets = stock_targets(), casters = stock_casters();
    set_lists(&targets, &casters);
  }
};

// bgchng: its ops 0x100 / 0x101 / 0x102 would stream a stage pack over the current one, but no shipped sequence sends them (bg_86..90 /
// bg_99 have no sequence with an op >= 0x100). Draws nothing.
class BgChng : public Remap {};

// miyademo: init rolls n = rand(); 0x100 queues bg_NN for NN = {13, 14, 15, 22, 23, 31, 39, 63, 64}[n % 9], n += 1; 0x101 swaps it in as the
// stage pack and restarts the 'miya' sequence on the first five swaps. The swap goes through FxHooks::set_stage (bg number); the battle
// host does not swap stages, and the sound / camera of the new pack are not played.
class Miyademo : public Remap {
 public:
  bool holds_game_speed() const override { return true; }

 protected:
  void init() override {
    Rng rng;
    rng.state = ctx_.seed ? ctx_.seed : 1;
    roll_ = rng.next();
  }
  bool command(int op, const int* args, int count) override {
    (void)args;
    (void)count;
    static constexpr int kBg[9] = {0x0D, 0x0E, 0x0F, 0x16, 0x17, 0x1F, 0x27, 0x3F, 0x40};
    if (op == 0x100) {
      queued_ = kBg[roll_ % 9];
      ++roll_;
      return true;
    }
    if (op != 0x101) return false;
    if (queued_ >= 0) {
      if (ctx_.hooks.set_stage) ctx_.hooks.set_stage(queued_);
      queued_ = -1;
      if (swaps_++ < 5 && ctx_.sequences) ctx_.sequences->start("miya");
    }
    return true;
  }

 private:
  int roll_ = 0, queued_ = -1, swaps_ = 0;
};

// hinsi / lasdes4: init writes target 0's idle-pose word := 8 (dead) / 0; nothing else.
class PoseWord : public Remap {
 public:
  explicit PoseWord(int pose) : pose_(pose) {}

 protected:
  void init() override {
    const FxModel* model = target(0);
    if (model && ctx_.hooks.set_pose) ctx_.hooks.set_pose(model->unit, pose_);
  }

 private:
  int pose_;
};

}  // namespace

void register_scene_remaps(BespokeRegistry& registry) {
  registry.add({"seint", "scene-remap", "Seint", [] { return std::make_unique<Seint>(); }});
  registry.add({"unico", "scene-remap", "Unico", [] { return std::make_unique<Unico>(); }});
  registry.add({"etrap", "scene-remap", "Etrap", [] { return std::make_unique<Etrap>(); }});
  registry.add({"bgchng", "scene-remap", "BgChng", [] { return std::make_unique<BgChng>(); }});
  registry.add({"miyademo", "scene-remap", "Miyademo", [] { return std::make_unique<Miyademo>(); }});
  registry.add({"hinsi", "shell-only", "PoseWord", [] { return std::make_unique<PoseWord>(8); }});
  registry.add({"lasdes4", "shell-only", "PoseWord", [] { return std::make_unique<PoseWord>(0); }});
}

// Exposed for the trap summon tunico (mesh_techs.cpp), which installs the same pair of getters as unico.
std::vector<int> fx_nearest_to_side(const FxModels& models, const FxModel* caster0) { return nearest_to_side(models, caster0); }

}  // namespace oc::effects
