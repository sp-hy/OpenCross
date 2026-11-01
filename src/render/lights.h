#pragma once
// Scene lights of the optional [Enhancements] EnhancedLighting / SpellLights. Plain data, no GPU types: the battle view derives a LightSet
// from the live spell effect each tick (effects/light_extract.h) and hands it to the mesh pipeline (units), the stage pipeline (floor
// and scenery) and the shadow pipeline (unit shadows on the floor). With both enhancements off the pipelines receive no set and their output is the PS1 look, bit for bit.
#include <algorithm>
#include <array>
#include <cmath>

namespace oc::render {

// One point light in the RENDER world (the stage convention: y up, PS1 world units). `rgb` is the light colour already multiplied by its
// intensity (0..1 per channel); the contribution to a surface point at distance d is rgb * (1 - d / radius)^2 (0 beyond the radius), added to the
// colour multiplier the surface already has. `linear` selects the PS1's own falloff (a battle light slot: intensity - distance * falloff,
// linear to zero at `radius`) instead of the smooth (1 - x^2)^2.
struct PointLight {
  std::array<float, 3> pos{};
  std::array<float, 3> rgb{};
  float radius = 0;
  bool linear = false;
  // The peak of the effect's light this very tick, 0 once it is gone: `rgb` eases down so the glow does not snap off, which is too slow for a shadow.
  float live = 0;
};

// Soft knee of the total light term: with m = the largest channel of the summed lights L, the multiplier is 1 + L / (1 + m / kLightKnee)
// (the same scale on all three channels, so the hue is kept), i.e. any number of stacked lights stays below 1 + kLightKnee = 1.8; m = 0.5 -> +0.32,
// m = 1 -> +0.47. The shaders carry the same constant (0.8).
constexpr float kLightKnee = 0.8f;
inline float light_knee(float level) { return level <= 0.0f ? level : level / (1.0f + level / kLightKnee); }

// 0 at and beyond the radius, 1 at the light, by the light's falloff.
inline float light_falloff(const PointLight& source, const float point[3]) {
  const float dx = point[0] - source.pos[0], dy = point[1] - source.pos[1], dz = point[2] - source.pos[2];
  const float x = std::sqrt(dx * dx + dy * dy + dz * dz) / std::max(source.radius, 1.0f);
  return source.linear ? std::clamp(1.0f - x, 0.0f, 1.0f) : std::clamp(1.0f - x * x, 0.0f, 1.0f);
}

struct LightSet {
  static constexpr int kMax = 8;
  std::array<PointLight, kMax> light{};
  int count = 0;
  // Global ambient multiplier of the battle scene (the PS1 back colour / 0x80 while a pack ramps it, 1 = neutral): scales the stage
  // and the units that are not on the PS1 lit path. Lights come first (the PS1 slot lights, then the clustered spell glow).
  std::array<float, 3> ambient{1.0f, 1.0f, 1.0f};
  bool empty() const { return count == 0; }
  bool ambient_on() const { return ambient[0] != 1.0f || ambient[1] != 1.0f || ambient[2] != 1.0f; }
  bool active() const { return count > 0 || ambient_on(); }
  // The colour multiplier (1 + knee(sum of the lights)) a surface at `point` gets per channel, as the shaders compute it (facing 1): smooth
  // (1 - x^2)^2 or the PS1 slot's linear falloff. The knee of the largest channel is applied to all three so the hue survives the
  // compression. For tests and the OC_LIGHT_TRACE log.
  std::array<float, 3> multiplier_at(const float point[3]) const {
    std::array<float, 3> sum{0, 0, 0};
    for (int i = 0; i < count; ++i) {
      const PointLight& source = light[static_cast<size_t>(i)];
      const float atten = light_falloff(source, point);
      for (size_t channel = 0; channel < 3; ++channel) sum[channel] += source.rgb[channel] * atten;
    }
    const float peak = std::max({sum[0], sum[1], sum[2]});
    const float knee = 1.0f / (1.0f + peak / kLightKnee);
    for (size_t channel = 0; channel < 3; ++channel) sum[channel] = 1.0f + sum[channel] * knee;
    return sum;
  }
};

// The shadow the spell lights give a unit whose middle is at `point`. level = the summed live light on it. slope x, z = how far the shadow falls per unit of
// height: the lights' mean, weighted by level, so lights around the unit cancel and its shadow gathers under it instead of swinging with them. A light
// within kNearShadowLight of the unit's axis leans the shadow less the closer it is. The slope is at most that of an elevation of 40 degrees, since many
// spell lights sit at floor level and a true point light would throw the shadow off the screen.
constexpr float kMaxShadowSlope = 1.19f;
constexpr float kNearShadowLight = 1000.0f;
struct SpellShadow {
  float level = 0, slope_x = 0, slope_z = 0;
};
inline SpellShadow spell_shadow(const LightSet& set, const float point[3]) {
  SpellShadow out;
  for (int i = 0; i < set.count; ++i) {
    const PointLight& source = set.light[static_cast<size_t>(i)];
    const float level = source.live * light_falloff(source, point);
    if (level <= 0.0f) continue;
    out.level += level;
    const float dx = source.pos[0] - point[0], dy = source.pos[1] - point[1], dz = source.pos[2] - point[2];
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    if (horizontal < 1.0f) continue;
    const float slope = (dy > 0.0f ? std::min(horizontal / dy, kMaxShadowSlope) : kMaxShadowSlope) * std::min(horizontal / kNearShadowLight, 1.0f);
    out.slope_x -= level * slope * dx / horizontal;
    out.slope_z -= level * slope * dz / horizontal;
  }
  if (out.level > 0.0f) out.slope_x /= out.level, out.slope_z /= out.level;
  return out;
}
// The part of a unit's shadow that is the spell's, the rest being the sun's. The sun counts as a light of level kSunShadowLevel, and the spell takes over as
// its share of the light climbs from 0.3 to 0.6.
constexpr float kSunShadowLevel = 0.5f;
inline float spell_shadow_share(float level) {
  const float rise = std::clamp((level / (level + kSunShadowLevel) - 0.3f) / 0.3f, 0.0f, 1.0f);
  return rise * rise * (3.0f - 2.0f * rise);
}

}  // namespace oc::render
