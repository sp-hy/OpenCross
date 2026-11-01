#include "game/effects/light_extract.h"

#include <algorithm>
#include <cmath>

namespace oc::effects {

namespace {
float dist3(const float from[3], const float to[3]) {
  const float x = from[0] - to[0], y = from[1] - to[1], z = from[2] - to[2];
  return std::sqrt(x * x + y * y + z * z);
}
}  // namespace

// Greedy clustering, strongest source first: a source joins the nearest cluster centre within merge_radius, else opens a new cluster
// while fewer than LightSet::kMax exist, else joins the nearest cluster whatever the distance. The clusters are weighted means of
// position and colour; `extent` is the weighted RMS distance of the members from the centre, `size` the farthest member reach.
std::vector<LightCluster> cluster_sources(std::vector<LightSource> sources, const LightParams& params) {
  sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const LightSource& source) { return !(source.weight >= params.min_weight); }),
                sources.end());
  std::sort(sources.begin(), sources.end(), [](const LightSource& lhs, const LightSource& rhs) { return lhs.weight > rhs.weight; });
  if (static_cast<int>(sources.size()) > params.max_sources) sources.resize(static_cast<size_t>(params.max_sources));
  struct Accumulator {
    LightCluster cluster;
    // Weighted sums of position, colour, |position|^2.
    double pos_sum[3] = {0, 0, 0}, rgb_sum[3] = {0, 0, 0}, sq_sum = 0;
    std::vector<const LightSource*> members;
  };
  std::vector<Accumulator> clusters;
  auto centre = [](const Accumulator& acc, float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = acc.cluster.weight > 0 ? static_cast<float>(acc.pos_sum[k] / acc.cluster.weight) : 0.0f;
  };
  for (const LightSource& source : sources) {
    int best = -1;
    float best_dist = params.merge_radius;
    for (size_t i = 0; i < clusters.size(); ++i) {
      float centre_pos[3];
      centre(clusters[i], centre_pos);
      const float dist = dist3(centre_pos, source.pos);
      if (dist <= best_dist) {
        best_dist = dist;
        best = static_cast<int>(i);
      }
    }
    if (best < 0 && static_cast<int>(clusters.size()) >= render::LightSet::kMax) {
      float nearest = 1e30f;
      for (size_t i = 0; i < clusters.size(); ++i) {
        float centre_pos[3];
        centre(clusters[i], centre_pos);
        const float dist = dist3(centre_pos, source.pos);
        if (dist < nearest) {
          nearest = dist;
          best = static_cast<int>(i);
        }
      }
    }
    if (best < 0) {
      clusters.emplace_back();
      best = static_cast<int>(clusters.size()) - 1;
    }
    Accumulator& acc = clusters[static_cast<size_t>(best)];
    acc.cluster.weight += source.weight;
    acc.cluster.count += 1;
    acc.members.push_back(&source);
    for (int k = 0; k < 3; ++k) {
      acc.pos_sum[k] += static_cast<double>(source.pos[k]) * source.weight;
      acc.rgb_sum[k] += static_cast<double>(source.rgb[k]) * source.weight;
      acc.sq_sum += static_cast<double>(source.pos[k]) * source.pos[k] * source.weight;
    }
  }
  std::vector<LightCluster> out;
  for (Accumulator& acc : clusters) {
    LightCluster cluster = acc.cluster;
    centre(acc, cluster.pos);
    for (int k = 0; k < 3; ++k) cluster.rgb[k] = static_cast<float>(acc.rgb_sum[k] / cluster.weight);
    const double variance = acc.sq_sum / cluster.weight - (static_cast<double>(cluster.pos[0]) * cluster.pos[0] + static_cast<double>(cluster.pos[1]) * cluster.pos[1] +
                                                           static_cast<double>(cluster.pos[2]) * cluster.pos[2]);
    cluster.extent = static_cast<float>(std::sqrt(std::max(variance, 0.0)));
    for (const LightSource* member : acc.members) cluster.size = std::max(cluster.size, dist3(member->pos, cluster.pos) + member->size);
    out.push_back(cluster);
  }
  std::sort(out.begin(), out.end(), [](const LightCluster& lhs, const LightCluster& rhs) { return lhs.weight > rhs.weight; });
  return out;
}

// Each cluster continues the nearest unmatched tracked light within match_radius (eased position / hue / radius), else takes a free
// slot, else replaces the weakest unmatched light when it is stronger. The level rises with `attack` and falls with `decay`; lights no
// cluster continued decay by orphan_decay and are dropped below 0.01 (stable compaction).
void LightTracker::update(std::span<const LightCluster> clusters) {
  bool matched[render::LightSet::kMax] = {};
  for (int i = 0; i < count_; ++i) tracked_[i].live = 0.0f;
  for (const LightCluster& cluster : clusters) {
    const float max_channel = std::max({cluster.rgb[0], cluster.rgb[1], cluster.rgb[2], 1e-4f});
    const float hue[3] = {cluster.rgb[0] / max_channel, cluster.rgb[1] / max_channel, cluster.rgb[2] / max_channel};
    const float target = 1.0f - std::exp(-params_.level_gain * cluster.weight);
    const float radius = std::clamp(params_.radius_scale * cluster.size + params_.radius_base, params_.radius_min, params_.radius_max);
    int best = -1;
    float best_dist = params_.match_radius;
    for (int i = 0; i < count_; ++i) {
      if (matched[i]) continue;
      const float dist = dist3(tracked_[i].pos, cluster.pos);
      if (dist <= best_dist) {
        best_dist = dist;
        best = i;
      }
    }
    if (best < 0) {
      if (count_ < render::LightSet::kMax) {
        best = count_++;
      } else {
        int weakest = -1;
        for (int i = 0; i < count_; ++i)
          if (!matched[i] && (weakest < 0 || tracked_[i].level < tracked_[weakest].level)) weakest = i;
        if (weakest < 0 || tracked_[weakest].level >= target) continue;
        best = weakest;
      }
      Tracked& light = tracked_[best];
      light = Tracked{};
      for (int k = 0; k < 3; ++k) {
        light.pos[k] = cluster.pos[k];
        light.hue[k] = hue[k];
      }
      light.radius = radius;
      light.level = 0;
    } else {
      Tracked& light = tracked_[best];
      for (int k = 0; k < 3; ++k) {
        light.pos[k] += (cluster.pos[k] - light.pos[k]) * params_.pos_ease;
        light.hue[k] += (hue[k] - light.hue[k]) * params_.hue_ease;
      }
      light.radius += (radius - light.radius) * params_.radius_ease;
    }
    Tracked& light = tracked_[best];
    light.level += (target - light.level) * (target > light.level ? params_.attack : params_.decay);
    light.live = target;
    matched[best] = true;
  }
  for (int i = 0; i < count_; ++i)
    if (!matched[i]) tracked_[i].level *= params_.orphan_decay;
  int write = 0;
  for (int i = 0; i < count_; ++i)
    if (tracked_[i].level >= 0.01f) tracked_[write++] = tracked_[i];
  count_ = write;
}

render::LightSet LightTracker::set() const {
  render::LightSet lights;
  int idx[render::LightSet::kMax];
  for (int i = 0; i < count_; ++i) idx[i] = i;
  std::sort(idx, idx + count_, [&](int lhs, int rhs) { return tracked_[lhs].level > tracked_[rhs].level; });
  for (int j = 0; j < count_; ++j) {
    const Tracked& tracked = tracked_[idx[j]];
    render::PointLight& light = lights.light[static_cast<size_t>(lights.count++)];
    for (size_t k = 0; k < 3; ++k) {
      light.pos[k] = tracked.pos[k];
      light.rgb[k] = tracked.hue[k] * tracked.level * params_.strength;
    }
    light.radius = tracked.radius;
    light.live = tracked.live * params_.strength;
  }
  return lights;
}

// The texel weight is chroma x brightness plus a small floor, so a coloured halo beats a white core.
const SpriteColour& SpriteColourCache::get(const assets::EffectSprite& sprite) {
  auto found = map_.find(&sprite);
  if (found != map_.end()) return found->second;
  SpriteColour result;
  double sum[3] = {0, 0, 0}, wsum = 0, bsum = 0;
  size_t covered = 0;
  const size_t texel_count = sprite.rgba.size() / 4;
  for (size_t i = 0; i < texel_count; ++i) {
    const unsigned char* texel = &sprite.rgba[i * 4];
    if (texel[3] == 0) continue;
    const float rgb[3] = {texel[0] / 255.0f, texel[1] / 255.0f, texel[2] / 255.0f};
    const float max_ch = std::max({rgb[0], rgb[1], rgb[2]}), min_ch = std::min({rgb[0], rgb[1], rgb[2]});
    ++covered;
    bsum += max_ch;
    const double weight = static_cast<double>(max_ch - min_ch) * max_ch + 0.02 * max_ch + 0.005;
    for (int k = 0; k < 3; ++k) sum[k] += rgb[k] * weight;
    wsum += weight;
  }
  if (covered && wsum > 0) {
    for (int k = 0; k < 3; ++k) result.rgb[k] = static_cast<float>(sum[k] / wsum);
    result.coverage = static_cast<float>(covered) / static_cast<float>(texel_count);
    result.bright = static_cast<float>(bsum / static_cast<double>(covered));
    const float max_ch = std::max({result.rgb[0], result.rgb[1], result.rgb[2]}), min_ch = std::min({result.rgb[0], result.rgb[1], result.rgb[2]});
    result.sat = max_ch > 1e-4f ? (max_ch - min_ch) / max_ch : 0.0f;
  } else {
    result.rgb[0] = result.rgb[1] = result.rgb[2] = 0;
  }
  return map_.emplace(&sprite, result).first->second;
}

bool ps1_slot_light(const int pos[3], const unsigned char rgb[3], int intensity, int falloff, render::PointLight& out) {
  if (intensity <= 0) return false;
  const float gain = static_cast<float>(std::min(intensity, 0x7FFF)) / 4096.0f;
  out = render::PointLight{};
  out.pos = {static_cast<float>(pos[0]), -static_cast<float>(pos[1]), static_cast<float>(pos[2])};
  for (size_t ch = 0; ch < 3; ++ch) out.rgb[ch] = std::min(static_cast<float>(rgb[ch]) / 256.0f * gain, 2.2f);
  out.radius = falloff > 0 ? std::clamp(static_cast<float>(intensity) * 4096.0f / static_cast<float>(falloff), 400.0f, 12000.0f) : 12000.0f;
  out.linear = true;
  return out.rgb[0] > 0.0f || out.rgb[1] > 0.0f || out.rgb[2] > 0.0f;
}

std::vector<LightCluster> drop_covered_clusters(std::vector<LightCluster> clusters, std::span<const render::PointLight> slot_lights) {
  clusters.erase(std::remove_if(clusters.begin(), clusters.end(),
                                [&](const LightCluster& cluster) {
                                  for (const render::PointLight& light : slot_lights)
                                    if (dist3(cluster.pos, light.pos.data()) < 0.6f * light.radius) return true;
                                  return false;
                                }),
                 clusters.end());
  return clusters;
}

}  // namespace oc::effects
