// The encounter's strip feed-back after the battle initialiser's white clear. The initialiser's embedded `main` sequence is a
// bracket with four ramps; the strip pass draws the previous frame buffer over the new scene at 50 % scaled by 4096 / (4096 + tz),
// tinted by the bracket colour. Both the field (until the battle takes over) and the battle (after) evaluate the same bracket
// here, so the zoom is one continuous curve.
#pragma once

#include <algorithm>
#include <cmath>

namespace oc::game::encstrips {

struct Params {
  // `on`: inside the bracket. `tz`: z translation of the strips. `rgb`: tint (128 = neutral).
  bool on = true;
  float tz = 0.0f;
  float rgb[3] = {128.0f, 128.0f, 128.0f};
  // The strips' zoom about the projection centre.
  float scale() const { return 4096.0f / std::max(64.0f, 4096.0f + tz); }
  float gain(int channel) const { return rgb[channel] / 128.0f; }
};

// `ticks` = game ticks since the first ramp started (10 ticks after the bracket opened). The bracket: ramp 1 over 57 ticks to
// tz -2289 / colour 190 191 179, wait 60; ramp 2 over 62 ticks to tz +131 / neutral, wait 61; ramp 3 over 49 ticks to tz 0 /
// neutral, wait 49; close. Each ramp is the integer lerp from the value when it started; ramp 3 starts one tick before ramp 2
// ends, so from ramp 2's value at its tick 61.
inline Params main_bracket(int ticks) {
  Params params;
  if (ticks < 0) return params;
  if (ticks >= 170) {
    params.on = false;
    return params;
  }
  auto lerp = [](float from, float to, int step, int steps) {
    return from + std::trunc((to - from) * static_cast<float>(step) / static_cast<float>(steps));
  };
  const float tz1 = -2289.0f, rgb1[3] = {190.0f, 191.0f, 179.0f};
  float tz = 0.0f, rgb[3] = {128.0f, 128.0f, 128.0f};
  if (ticks <= 57) {
    tz = lerp(0.0f, tz1, ticks, 57);
    for (int k = 0; k < 3; ++k) rgb[k] = lerp(128.0f, rgb1[k], ticks, 57);
  } else {
    tz = tz1;
    for (int k = 0; k < 3; ++k) rgb[k] = rgb1[k];
  }
  if (ticks >= 60) {
    const int step = std::min(ticks - 60, 62);
    const float z0 = tz;
    tz = lerp(z0, 131.0f, step, 62);
    for (int k = 0; k < 3; ++k) rgb[k] = lerp(rgb[k], 128.0f, step, 62);
  }
  if (ticks >= 121) {
    const float z0 = lerp(tz1, 131.0f, 61, 62);
    float rgb0[3];
    for (int k = 0; k < 3; ++k) rgb0[k] = lerp(rgb1[k], 128.0f, 61, 62);
    const int step = std::min(ticks - 121, 49);
    tz = lerp(z0, 0.0f, step, 49);
    for (int k = 0; k < 3; ++k) rgb[k] = lerp(rgb0[k], 128.0f, step, 49);
  }
  params.tz = tz;
  for (int k = 0; k < 3; ++k) params.rgb[k] = rgb[k];
  return params;
}

// Pass rates: the field presents at 30 Hz, so its bracket runs one strip pass per 2 game ticks; the initialiser's hold and the
// battle present at 60 Hz, one pass per tick. Ticks inside a pass period redraw the period's pass from its anchor with partial
// powers, so the 60 Hz picture stays smooth whatever the period.
constexpr int kFieldPassTicks = 2, kHoldPassTicks = 1;

// The scene weight of the field's hold (the frozen last pre-white output), fitted to the retail luma curve: with the uniform
// feed-back L' = 0.5 S L0 + 0.5 g(m) L (g = the bracket colour gain; the zoom does not change the mean) the weight S is ~1.08
// until game frame 48, then falls off to 0.51 at frame 104. `update_index` = ticks / 4 + 1 (the field's update index).
inline float hold_scene_gain(float update_index) {
  static constexpr float kGf[] = {0.0f, 48.0f, 56.0f, 64.0f, 72.0f, 80.0f, 88.0f, 96.0f, 104.0f};
  static constexpr float kS[] = {1.08f, 1.08f, 0.95f, 0.83f, 0.80f, 0.72f, 0.65f, 0.56f, 0.51f};
  const float gf = 4.0f * (update_index - 1.0f);
  constexpr int count = sizeof(kGf) / sizeof(kGf[0]);
  if (gf <= kGf[0]) return kS[0];
  for (int i = 1; i < count; ++i)
    if (gf <= kGf[i]) return kS[i - 1] + (kS[i] - kS[i - 1]) * (gf - kGf[i - 1]) / (kGf[i] - kGf[i - 1]);
  return kS[count - 1];
}

// The hand-over's own picture (the smear) darkens to nothing while the strips keep zooming out, and the battle fades in UNDER it:
// a cross-fade in which the brightness never dips. `tick` = ticks since the take-over (game frame 95, ~96 frames after the white). RDE keeps
// loading long after the take-over while the battle already runs its stage intro: the smear alone until tick 30, then the stage intro pan fades
// in under it until tick 78 (retail: 126..174 frames after the white); the party intro starts when the first PC's load ends, kLoadHold (retail:
// 210 frames after the white), which cuts the stage intro after about 98 of its ticks.
// The stage intro ('bti*', 'fade') starts kIntroStart ticks after the take-over (retail: ~109 frames after the white; the 'fade' flat then lifts
// the intro pan out of the dark: retail luma 65 / 104 / 132 at 178 / 205 / 229 frames after the white).
constexpr int kIntroStart = 13, kLoadHold = 112;
constexpr int kDarkFrom = 30, kBlackAt = 78, kBattleFrom = 30, kFadeInTicks = 48;
inline float hand_smear(int tick) {
  if (tick <= kDarkFrom) return 1.0f;
  if (tick >= kBlackAt) return 0.0f;
  return 1.0f - static_cast<float>(tick - kDarkFrom) / static_cast<float>(kBlackAt - kDarkFrom);
}
inline float hand_battle(int tick) {
  if (tick <= kBattleFrom) return 0.0f;
  return std::min(1.0f, static_cast<float>(tick - kBattleFrom) / static_cast<float>(kFadeInTicks));
}

}  // namespace oc::game::encstrips
