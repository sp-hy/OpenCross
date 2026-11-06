// Screen-band overlays: canon, hip, jigoku, xgiri, ghost1, sky (screen negatives). Nothing is drawn until the script's op 0x100
// (operand = ticks) starts the countdown; while it runs the frame is redrawn as 5 columns of 64 px under a white quad, i.e. the
// backdrop is inverted. sky adds op 0x101 (ticks, end rgb): the target's tint ramps from 0x80 to the end colour over the ticks.
#include <algorithm>

#include "game/effects/bespoke/bespoke.h"
#include "game/effects/bespoke/bespoke_util.h"

namespace oc::effects {
namespace {

using util::opnd;

// default_ticks = the countdown when the context carries no operand; tint_op = op 0x101 is the actor tint fade (sky).
struct WipeParams {
  const char* name;
  int default_ticks;
  bool tint_op;
};

class BandWipe : public BespokeEffect {
 public:
  explicit BandWipe(const WipeParams& params) : params_(params) {}

  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    ticks_ = ctx.duration > 0 ? ctx.duration : params_.default_ticks;
    strip_.height = 240;
  }

  bool command(int op, const int* args, int nargs) override {
    if (op == 0x100) {
      strip_.start(opnd(args, nargs, 0, ticks_));
      return true;
    }
    if (op == 0x101 && params_.tint_op) {
      tint_dur_ = std::max(1, opnd(args, nargs, 0, 30));
      tint_t_ = 0;
      for (int channel = 0; channel < 3; ++channel) tint_end_[channel] = opnd(args, nargs, 1 + channel, 128);
      return true;
    }
    return false;
  }

  void tick() override {
    strip_.tick();
    if (tint_t_ >= 0) {
      const float frac = static_cast<float>(tint_t_) / static_cast<float>(tint_dur_);
      if (ctx_.hooks.actor_tint)
        ctx_.hooks.actor_tint(1, static_cast<int>(128 + (tint_end_[0] - 128) * frac), static_cast<int>(128 + (tint_end_[1] - 128) * frac),
                              static_cast<int>(128 + (tint_end_[2] - 128) * frac));
      if (++tint_t_ > tint_dur_) tint_t_ = -1;
    }
  }

  void draw(std::vector<FxPrim>& out) const override { strip_.draw(out); }

  bool done() const override { return !strip_.active() && tint_t_ < 0; }

 private:
  WipeParams params_;
  int ticks_ = 30;
  util::NegativeStrip strip_;
  int tint_t_ = -1, tint_dur_ = 1, tint_end_[3] = {128, 128, 128};
};

constexpr WipeParams kParams[] = {
    {"canon", 30, false},
    {"jigoku", 45, false},
    {"hip", 20, false},
    {"xgiri", 24, false},
    {"ghost1", 36, false},
    {"sky", 40, true},
};

}  // namespace

void register_band_wipes(BespokeRegistry& registry) {
  for (const WipeParams& params : kParams)
    registry.add({params.name, "band-wipe", "BandWipe/Negative", [params] { return std::make_unique<BandWipe>(params); }});
}

}  // namespace oc::effects
