#pragma once
// Small helpers shared by the BattleView translation units (internal).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include "core/types.h"
#include "render/sprite_batch.h"

namespace oc::game::bv {

constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
// PS1 screen pixel (320x240) -> HUD canvas unit.
constexpr float kPs1Px = 4.0f;
constexpr float kPi = 3.14159265358979f;
// The `fade` quad's clock t = kStartFadeLead + kStartFadeRate * battle tick (t is already ~27 at the first battle frame and advances ~0.65 per
// frame), removed at battle tick kStartFadeEnd.
constexpr int kStartFadeEnd = 91;
constexpr float kStartFadeLead = 27.0f, kStartFadeRate = 0.65f;

struct Rgb {
  u8 r, g, b;
};
// menu/menuelem plate index (1-based file number) per field colour (data::colour_name order): green 3, white 6, black 4, red 5, yellow 7, blue 2.
constexpr int kPlateOfColour[6] = {3, 6, 4, 5, 7, 2};

inline u32 col(u8 r, u8 g, u8 b, u8 a = 255) { return render::pack_rgba(r, g, b, a); }
inline const u32 kWhite = render::pack_rgba(255, 255, 255), kYellow = render::pack_rgba(255, 230, 90), kGrey = render::pack_rgba(150, 150, 160),
                 kRed = render::pack_rgba(255, 90, 90), kGreen = render::pack_rgba(120, 255, 140);

inline float lerp(float from, float to, float amount) { return from + (to - from) * amount; }

// The end yaw (radians, unwrapped from `from`) of a RotateT to the 12-bit yaw `to`: the 12-bit difference taken as signed, so an exact half
// turn goes the negative way.
inline float rotate_end(float from, int to) {
  const int current = static_cast<int>(std::lround(from * (4096.0f / (2.0f * kPi)))) & 0xFFF;
  int diff = (to - current) & 0xFFF;
  if (diff >= 0x800) diff -= 0x1000;
  return from + static_cast<float>(diff) * (2.0f * kPi / 4096.0f);
}

inline std::array<float, 3> cross(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs) {
  return {lhs[1] * rhs[2] - lhs[2] * rhs[1], lhs[2] * rhs[0] - lhs[0] * rhs[2], lhs[0] * rhs[1] - lhs[1] * rhs[0]};
}
inline float dot(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs) { return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2]; }
inline std::array<float, 3> normalize(std::array<float, 3> vec, const std::array<float, 3>& fallback) {
  const float length = std::sqrt(dot(vec, vec));
  if (length < 1e-5f) return fallback;
  for (float& component : vec) component /= length;
  return vec;
}

// apr0 / apr1 at random (apr2 / apr3 when `dynamic`); `OC_CAM_PICK=aprN` forces one.
inline std::string apr_pick(bool dynamic) {
  if (const char* want = std::getenv("OC_CAM_PICK"); want && want[0] == 'a' && want[1] == 'p' && want[2] == 'r') return want;
  return std::format("apr{}", (dynamic ? 2 : 0) + (std::rand() & 1));
}

}  // namespace oc::game::bv
