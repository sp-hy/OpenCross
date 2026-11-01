#pragma once
// [Enhancements] Lighting: turns the bright additive primitives of a live battle effect into a few point lights.
//   1. The caller lists LightSource entries (render-world position, colour 0..1, weight) from the effect's additive sprites / meshes
//      (BattleView::fx_spell_light_tick).
//   2. cluster_sources() merges sources closer than `merge_radius` (greedy, strongest first) into at most LightSet::kMax clusters: weighted mean
//      position and colour, summed weight, extent = the weighted RMS distance of the members from the centre.
//   3. LightTracker::update() matches the clusters to the lights of the previous tick (nearest within match_radius), eases position / hue /
//      radius, and gives the level an attack (fast) and a decay (slow) so the lights neither flicker nor snap off.
#include <algorithm>
#include <span>
#include <unordered_map>
#include <vector>

#include "assets/room_effects.h"
#include "render/lights.h"

namespace oc::effects {

// A light-emitting primitive. pos is in render world space (y up); rgb is 0..1+ (any brightness: only the hue is kept, the weight
// carries the brightness); weight = brightness x size (x blend factor); size = world radius of the primitive.
struct LightSource {
  float pos[3] = {0, 0, 0};
  float rgb[3] = {1, 1, 1};
  float weight = 0;
  float size = 0;
};

// Merged sources. rgb = weighted mean of the members' colours; weight = sum of the members' weights; extent = weighted RMS
// distance of the members from `pos`; size = max over the members of (distance from `pos` + the member's own radius).
struct LightCluster {
  float pos[3] = {0, 0, 0};
  float rgb[3] = {1, 1, 1};
  float weight = 0;
  float extent = 0;
  float size = 0;
  int count = 0;
};

// Tuning. merge_radius (world units): sources closer than this share a light; max_sources: the strongest sources considered;
// min_weight: weaker sources are ignored; match_radius: a cluster continues the tracked light within this distance;
// target level = 1 - exp(-level_gain * cluster weight); per tick the level rises by (target - level) * attack and falls by
// (target - level) * decay (a matched light); orphan_decay: a tracked light no cluster continues has level *= this per tick;
// output colour = hue * level * strength; radius = clamp(radius_scale * size + radius_base, radius_min, radius_max).
struct LightParams {
  float merge_radius = 400.0f;
  int max_sources = 64;
  float min_weight = 0.02f;
  float match_radius = 700.0f;
  float level_gain = 0.8f;
  float attack = 0.45f;
  float decay = 0.08f;
  float orphan_decay = 0.90f;
  float pos_ease = 0.5f, hue_ease = 0.4f, radius_ease = 0.3f;
  float strength = 2.2f;
  float radius_scale = 5.0f, radius_base = 600.0f, radius_min = 800.0f, radius_max = 6000.0f;
};

std::vector<LightCluster> cluster_sources(std::vector<LightSource> sources, const LightParams& params = {});

class LightTracker {
 public:
  explicit LightTracker(const LightParams& params = {}) : params_(params) {}
  void reset() { count_ = 0; }
  void update(std::span<const LightCluster> clusters);
  int active() const { return count_; }
  // The lights with level > 0.01, strongest first.
  render::LightSet set() const;

 private:
  struct Tracked {
    float pos[3] = {0, 0, 0};
    // hue: max channel 1.
    float hue[3] = {1, 1, 1};
    float level = 0, radius = 450;
    // The target of this tick's level, 0 when no cluster continued the light.
    float live = 0;
  };
  LightParams params_;
  Tracked tracked_[render::LightSet::kMax];
  int count_ = 0;
};

// Mean colour (0..1) of a sprite's lit texels and the fraction of the sprite they cover. The mean is weighted by chroma x brightness (+ a small floor), so
// the coloured halo decides the hue and near-white texels hardly count.
// bright = mean of the covered texels' largest channel; sat = (max - min) / max of `rgb`.
struct SpriteColour {
  float rgb[3] = {1, 1, 1};
  float coverage = 0;
  float bright = 0;
  float sat = 0;
};
class SpriteColourCache {
 public:
  const SpriteColour& get(const assets::EffectSprite& sprite);
  void clear() { map_.clear(); }

 private:
  std::unordered_map<const assets::EffectSprite*, SpriteColour> map_;
};

// A PS1 battle light slot (effects::LightTable::Slot: world position y DOWN, colour bytes, intensity = the object's scale x, falloff = its angle x) as a
// point light in the render world. The PS1 lights a model with k = intensity - (dist * falloff >> 12) (clamped 0..0x7FFF) and colour bytes << 4
// (4.12), so the strength at distance d is bytes / 256 * (intensity - d * falloff / 4096) / 4096: linear, zero at radius = intensity * 4096 / falloff
// (12000 with falloff 0, and as the cap). Returns false for an unlit slot (intensity <= 0). rgb is capped at 2.2 per channel.
bool ps1_slot_light(const int pos[3], const unsigned char rgb[3], int intensity, int falloff, render::PointLight& out);

// Drops the clusters a PS1 slot light already covers (centre within 0.6 x the slot's radius): the pack's own `ligt` object is the real light of that
// effect, the cluster of its sprites would double it.
std::vector<LightCluster> drop_covered_clusters(std::vector<LightCluster> clusters, std::span<const render::PointLight> slot_lights);

// How much a colour may weigh as a light: 0.25 for white / grey, 1 from a saturation of 0.5 up (a white flash gives a dim white light, a coloured burst a full one).
inline float chroma_scale(const float rgb[3]) {
  const float max_ch = std::max({rgb[0], rgb[1], rgb[2]}), min_ch = std::min({rgb[0], rgb[1], rgb[2]});
  const float sat = max_ch > 1e-4f ? (max_ch - min_ch) / max_ch : 0.0f;
  return 0.25f + 0.75f * std::min(1.0f, sat * 2.0f);
}

}  // namespace oc::effects
