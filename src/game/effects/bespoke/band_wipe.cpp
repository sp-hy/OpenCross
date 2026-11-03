// Screen-band overlays: canon, hip, jigoku, xgiri, ghost1, sky (screen negatives), muteki (flare), 155slot (reels). Three styles over
// one parameter struct:
//   Negative  nothing is drawn until the script's op 0x100 (operand = ticks) starts the countdown; while it runs the frame is redrawn as
//             5 columns of 64 px under a white quad, i.e. the backdrop is inverted. sky adds op 0x101 (ticks, end rgb): the target's tint
//             ramps from 0x80 to the end colour over the ticks.
//   Flare     op 0x180 (life, x, y, vx, vy, rgb) adds a drifting flare: 8 additive squares stepping from the flare towards its point-mirrored
//             screen position, plus a full-screen additive tint that falls off with the distance between the two.
//   Reels     three 32 x 192 reels (21 symbols of 64 px) scrolling up; op 0x100 arms them, each pad press (op 0x1FF, or ticks 40 / 70 / 100
//             with auto input) stops the next reel: it decelerates by 0x600 / 65536 px per tick down to 1 px / tick and snaps to a symbol
//             boundary. op 0x101 releases the effect.
#include <algorithm>
#include <cmath>

#include "game/effects/bespoke/bespoke.h"
#include "game/effects/bespoke/bespoke_util.h"

namespace oc::effects {
namespace {

using util::opnd;

enum class WipeStyle { Negative, Flare, Reels };

// default_ticks = the countdown when the context carries no operand; tint_op = op 0x101 is the actor tint fade (sky).
struct WipeParams {
  const char* name;
  WipeStyle style;
  int default_ticks;
  bool tint_op;
};

class BandWipe : public BespokeEffect {
 public:
  explicit BandWipe(const WipeParams& params) : params_(params) {}

  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    ticks_ = ctx.duration > 0 ? ctx.duration : params_.default_ticks;
    switch (params_.style) {
      case WipeStyle::Negative: strip_.height = 240; break;
      case WipeStyle::Flare: {
        const int start_args[8] = {ticks_, 40, 50, 3, 2, ctx.colour[0], ctx.colour[1], ctx.colour[2]};
        command(0x180, start_args, 8);
        break;
      }
      case WipeStyle::Reels:
        if (ctx.auto_input) {
          armed_ = true;
          reel_vel_ = {6.0f, 6.0f, 6.0f};
        }
        break;
    }
  }

  bool command(int op, const int* args, int nargs) override {
    switch (params_.style) {
      case WipeStyle::Negative:
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
        break;
      case WipeStyle::Flare:
        if (op == 0x180) {
          Flare flare;
          flare.life = opnd(args, nargs, 0, 60);
          flare.x = static_cast<float>(opnd(args, nargs, 1, 40));
          flare.y = static_cast<float>(opnd(args, nargs, 2, 50));
          flare.vx = static_cast<float>(opnd(args, nargs, 3, 3));
          flare.vy = static_cast<float>(opnd(args, nargs, 4, 2));
          for (int channel = 0; channel < 3; ++channel) flare.rgb[channel] = opnd(args, nargs, 5 + channel, 255);
          if (flares_.size() < 16) flares_.push_back(flare);
          return true;
        }
        break;
      case WipeStyle::Reels:
        if (op == 0x100) {
          armed_ = true;
          reel_vel_ = {6.0f, 6.0f, 6.0f};
          return true;
        }
        if (op == 0x101) {
          released_ = true;
          return true;
        }
        if (op == 0x1FF) {
          press();
          return true;
        }
        break;
    }
    return false;
  }

  void tick() override {
    ++age_;
    switch (params_.style) {
      case WipeStyle::Negative:
        strip_.tick();
        if (tint_t_ >= 0) {
          const float frac = static_cast<float>(tint_t_) / static_cast<float>(tint_dur_);
          if (ctx_.hooks.actor_tint)
            ctx_.hooks.actor_tint(1, static_cast<int>(128 + (tint_end_[0] - 128) * frac), static_cast<int>(128 + (tint_end_[1] - 128) * frac),
                                  static_cast<int>(128 + (tint_end_[2] - 128) * frac));
          if (++tint_t_ > tint_dur_) tint_t_ = -1;
        }
        break;
      case WipeStyle::Flare:
        for (auto& flare : flares_) {
          ++flare.age;
          flare.x += flare.vx;
          flare.y += flare.vy;
        }
        flares_.erase(std::remove_if(flares_.begin(), flares_.end(), [](const Flare& flare) { return flare.age > flare.life; }), flares_.end());
        break;
      case WipeStyle::Reels: tick_reels(); break;
    }
  }

  void draw(std::vector<FxPrim>& out) const override {
    switch (params_.style) {
      case WipeStyle::Negative: strip_.draw(out); break;
      case WipeStyle::Flare: draw_flares(out); break;
      case WipeStyle::Reels: draw_reels(out); break;
    }
  }

  bool done() const override {
    switch (params_.style) {
      case WipeStyle::Negative: return !strip_.active() && tint_t_ < 0;
      case WipeStyle::Flare: return flares_.empty();
      case WipeStyle::Reels: return released_ || (stopped_ == 3 && age_ > settle_until_);
    }
    return true;
  }

 private:
  struct Flare {
    int life = 60, age = 0;
    float x = 0, y = 0, vx = 0, vy = 0;
    int rgb[3] = {255, 255, 255};
  };

  static constexpr int kFlareFrames = 8;
  void draw_flares(std::vector<FxPrim>& out) const {
    for (const auto& flare : flares_) {
      if (flare.x < 0 || flare.x > 320 || flare.y < 0 || flare.y > 224) continue;
      for (int frame = 1; frame <= kFlareFrames; ++frame) {
        const float frac = static_cast<float>(frame - 1) / static_cast<float>(kFlareFrames);
        const float px = flare.x + (320.0f - 2.0f * flare.x) * frac, py = flare.y + (224.0f - 2.0f * flare.y) * frac;
        const float half = 12.0f - static_cast<float>(frame);
        out.push_back(util::rect_prim(px - half, py - half, px + half, py + half, util::clamp8i(flare.rgb[0]), util::clamp8i(flare.rgb[1]),
                                      util::clamp8i(flare.rgb[2]), 2, 0x10 + frame));
      }
      if (flare.rgb[0] | flare.rgb[1] | flare.rgb[2]) {
        const float dx = 320.0f - 2.0f * flare.x, dy = 224.0f - 2.0f * flare.y;
        const float fall = std::max(0.0f, (153600.0f - (dx * dx + dy * dy)) / 153600.0f);
        out.push_back(util::rect_prim(0, 0, 320, 224, util::clamp8(static_cast<float>(flare.rgb[0]) * fall), util::clamp8(static_cast<float>(flare.rgb[1]) * fall),
                                      util::clamp8(static_cast<float>(flare.rgb[2]) * fall), 2, 0));
      }
    }
  }

  static constexpr int kSymbols = 21, kSymH = 64;
  void press() {
    if (!armed_) return;
    for (size_t i = 0; i < 3; ++i)
      if (!stopping_[i]) {
        stopping_[i] = true;
        return;
      }
  }
  void tick_reels() {
    if (!armed_) return;
    if (ctx_.auto_input && (age_ == 40 || age_ == 70 || age_ == 100)) press();
    for (size_t i = 0; i < 3; ++i) {
      if (reel_done_[i]) continue;
      reel_pos_[i] += reel_vel_[i];
      if (stopping_[i]) {
        reel_vel_[i] = std::max(1.0f, reel_vel_[i] - 0x600 / 65536.0f);
        const float offset = std::fmod(reel_pos_[i], static_cast<float>(kSymH));
        if (reel_vel_[i] <= 1.0f && (offset < 1.0f || offset > kSymH - 1.0f)) {
          reel_pos_[i] = std::round(reel_pos_[i] / kSymH) * kSymH;
          reel_done_[i] = true;
          ++stopped_;
          settle_until_ = age_ + 30;
        }
      }
    }
  }
  void draw_reels(std::vector<FxPrim>& out) const {
    for (size_t i = 0; i < 3; ++i) {
      const float x0 = 112.0f + 34.0f * static_cast<float>(i), y0 = 24.0f;
      out.push_back(util::rect_prim(x0 - 2, y0 - 2, x0 + 34, y0 + 194, 40, 40, 56, 0, 4));
      const float pos = reel_pos_[i];
      const int first = static_cast<int>(std::floor(pos / kSymH));
      for (int row = 0; row <= 3; ++row) {
        const int sym = ((first + row) % kSymbols + kSymbols) % kSymbols;
        const float ty = y0 - std::fmod(pos, static_cast<float>(kSymH)) + static_cast<float>(row * kSymH);
        const float top = std::max(ty, y0), bottom = std::min(ty + kSymH, y0 + 192.0f);
        if (bottom <= top) continue;
        const u8 cr = static_cast<u8>(80 + (sym * 53) % 160), cg = static_cast<u8>(80 + (sym * 97) % 160), cb = static_cast<u8>(80 + (sym * 29) % 160);
        out.push_back(util::rect_prim(x0, top, x0 + 32, bottom - 2, cr, cg, cb, 0, 3));
      }
    }
  }

  WipeParams params_;
  int ticks_ = 30, age_ = 0;
  util::NegativeStrip strip_;
  int tint_t_ = -1, tint_dur_ = 1, tint_end_[3] = {128, 128, 128};
  std::vector<Flare> flares_;
  bool armed_ = false, released_ = false;
  std::array<float, 3> reel_pos_{0, 21, 42}, reel_vel_{0, 0, 0};
  std::array<bool, 3> stopping_{false, false, false}, reel_done_{false, false, false};
  int stopped_ = 0, settle_until_ = 0;
};

constexpr WipeParams kParams[] = {
    {"canon", WipeStyle::Negative, 30, false},
    {"jigoku", WipeStyle::Negative, 45, false},
    {"hip", WipeStyle::Negative, 20, false},
    {"xgiri", WipeStyle::Negative, 24, false},
    {"ghost1", WipeStyle::Negative, 36, false},
    {"sky", WipeStyle::Negative, 40, true},
    {"muteki", WipeStyle::Flare, 60, false},
    {"155slot", WipeStyle::Reels, 0, false},
};

}  // namespace

void register_band_wipes(BespokeRegistry& registry) {
  for (const WipeParams& params : kParams) {
    const char* module = params.style == WipeStyle::Negative ? "BandWipe/Negative" : params.style == WipeStyle::Flare ? "BandWipe/Flare" : "BandWipe/Reels";
    registry.add({params.name, "band-wipe", module, [params] { return std::make_unique<BandWipe>(params); }});
  }
}

}  // namespace oc::effects
