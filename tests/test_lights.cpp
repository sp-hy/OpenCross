// [Enhancements] SpellLights: clustering, smoothing, radius and hue rules of synthetic effect light sources, the PS1 slot-light conversion,
// sprite colour extraction and the stacked-light multiplier cap (src/game/effects/light_extract.*).
#include <cmath>
#include <cstdio>

#include "game/effects/light_extract.h"

using namespace oc::effects;

static int g_fail = 0;
#define CHECK(c)                                                           \
  do {                                                                     \
    if (!(c)) {                                                            \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);                     \
      ++g_fail;                                                            \
    }                                                                      \
  } while (0)

static LightSource src(float x, float y, float z, float r, float g, float b, float weight) {
  LightSource source;
  source.pos[0] = x, source.pos[1] = y, source.pos[2] = z;
  source.rgb[0] = r, source.rgb[1] = g, source.rgb[2] = b;
  source.weight = weight;
  return source;
}

int main() {
  // Two groups 1500 units apart (red near the origin, blue far away) -> two clusters, strongest first; each keeps its hue.
  {
    std::vector<LightSource> sources;
    for (int i = 0; i < 5; ++i) sources.push_back(src(static_cast<float>(i * 20), 100, 0, 1.0f, 0.2f, 0.1f, 0.5f));
    for (int i = 0; i < 3; ++i) sources.push_back(src(1500.0f + static_cast<float>(i * 30), 300, 200, 0.1f, 0.3f, 1.0f, 0.3f));
    const auto clusters = cluster_sources(sources);
    CHECK(clusters.size() == 2);
    CHECK(clusters[0].count == 5 && clusters[1].count == 3);
    CHECK(std::fabs(clusters[0].weight - 2.5f) < 1e-4f);
    CHECK(clusters[0].rgb[0] > 0.9f && clusters[0].rgb[2] < 0.2f);
    CHECK(clusters[1].rgb[2] > 0.9f && clusters[1].rgb[0] < 0.2f);
    CHECK(std::fabs(clusters[0].pos[0] - 40.0f) < 1.0f);
    CHECK(clusters[0].extent > 10.0f && clusters[0].extent < 60.0f);
  }
  // Sources inside the merge radius share one light; weak ones are ignored; more than kMax distant sources join the nearest cluster.
  {
    std::vector<LightSource> sources = {src(0, 0, 0, 1, 1, 1, 1.0f), src(300, 0, 0, 1, 1, 1, 1.0f), src(5000, 0, 0, 1, 0, 0, 0.001f)};
    CHECK(cluster_sources(sources).size() == 1);
    sources.clear();
    for (int i = 0; i < 12; ++i) sources.push_back(src(static_cast<float>(i * 1000), 0, 0, 1, 1, 1, 1.0f));
    const auto clusters = cluster_sources(sources);
    CHECK(static_cast<int>(clusters.size()) == oc::render::LightSet::kMax);
    int total = 0;
    for (const auto& cluster : clusters) total += cluster.count;
    CHECK(total == 12);
  }
  // Smoothing: a steady cluster ramps up monotonically below its target (attack) keeping its hue (green stays half of red), then dies away
  // monotonically (decay) when it disappears; a one-tick blip never reaches the full level; a moving cluster is followed, not duplicated.
  {
    LightTracker tracker;
    const std::vector<LightSource> sources = {src(0, 100, 0, 1, 0.5f, 0.1f, 3.0f)};
    const auto clusters = cluster_sources(sources);
    float prev = 0;
    for (int i = 0; i < 20; ++i) {
      tracker.update(clusters);
      const auto set = tracker.set();
      CHECK(set.count == 1);
      const float level = set.light[0].rgb[0];
      CHECK(level >= prev - 1e-6f);
      prev = level;
    }
    const float target = (1.0f - std::exp(-0.8f * 3.0f)) * 2.2f;
    CHECK(prev > 0.95f * target && prev <= target + 1e-4f);
    CHECK(std::fabs(tracker.set().light[0].rgb[1] - 0.5f * prev) < 0.02f);
    for (int i = 0; i < 60; ++i) {
      tracker.update({});
      const float level = tracker.set().count ? tracker.set().light[0].rgb[0] : 0.0f;
      CHECK(level <= prev + 1e-6f);
      prev = level;
    }
    CHECK(tracker.active() == 0);
    tracker.update(clusters);
    CHECK(tracker.set().light[0].rgb[0] < 0.6f * target);
    LightTracker mover;
    for (int i = 0; i < 10; ++i) mover.update(cluster_sources({src(static_cast<float>(i * 100), 0, 0, 1, 1, 1, 2.0f)}));
    CHECK(mover.active() == 1);
    const auto set = mover.set();
    CHECK(set.light[0].pos[0] > 400.0f && set.light[0].pos[0] < 900.0f);
  }
  // Radius: a cluster's size is the largest of its sources' (spread + size) = max(150 + 600, 150 + 300) = 750, its light radius 5 x size + 600,
  // clamped up to 800 for a tiny source.
  {
    LightSource big_near = src(0, 0, 0, 1, 0.4f, 0, 2.0f), big_far = src(300, 0, 0, 1, 0.4f, 0, 2.0f);
    big_near.size = 600, big_far.size = 300;
    const auto clusters = cluster_sources({big_near, big_far});
    CHECK(clusters.size() == 1 && std::fabs(clusters[0].size - 750.0f) < 5.0f);
    LightTracker tracker;
    tracker.update(clusters);
    const auto set = tracker.set();
    CHECK(set.count == 1 && std::fabs(set.light[0].radius - (5.0f * clusters[0].size + 600.0f)) < 1.0f);
    CHECK(set.light[0].rgb[0] > 0.0f && set.light[0].rgb[1] < set.light[0].rgb[0]);
    LightSource tiny = src(0, 0, 0, 1, 1, 1, 0.5f);
    tiny.size = 10;
    LightTracker tiny_tracker;
    tiny_tracker.update(cluster_sources({tiny}));
    CHECK(tiny_tracker.set().light[0].radius == 800.0f);
  }
  // PS1 slot lights: strength = bytes / 256 * (intensity - d * falloff / 4096) / 4096, linear, zero at intensity * 4096 / falloff; y flips to
  // the render world. Intensity 12032 at falloff 3000: red 0.996 x 2.94 capped at 2.2, radius 16428 clamped to 12000; intensity 24576 at
  // falloff 12288 is dark past 8192. A cluster inside 0.6 x the slot radius is dropped, a far one kept; an ambient colour alone makes the set active.
  {
    const int pos[3] = {100, -200, 300};
    const unsigned char rgb[3] = {255, 0, 0};
    oc::render::PointLight light;
    CHECK(!ps1_slot_light(pos, rgb, 0, 100, light));
    CHECK(ps1_slot_light(pos, rgb, 12032, 3000, light));
    CHECK(light.linear && light.pos[1] == 200.0f && light.pos[0] == 100.0f);
    CHECK(light.rgb[0] == 2.2f && light.rgb[1] == 0.0f);
    CHECK(light.radius == 12000.0f);
    CHECK(ps1_slot_light(pos, rgb, 24576, 12288, light) && std::fabs(light.radius - 8192.0f) < 1.0f);
    LightCluster near_cluster, far_cluster;
    near_cluster.pos[0] = 100, near_cluster.pos[1] = 200, near_cluster.pos[2] = 300 + 1000;
    far_cluster.pos[0] = 100 + 7000, far_cluster.pos[1] = 200, far_cluster.pos[2] = 300;
    const std::vector<oc::render::PointLight> slot_lights = {light};
    const auto kept = drop_covered_clusters({near_cluster, far_cluster}, slot_lights);
    CHECK(kept.size() == 1 && kept[0].pos[0] > 5000.0f);
    oc::render::LightSet set;
    CHECK(!set.active() && !set.ambient_on());
    set.ambient = {0.625f, 0.47f, 0.48f};
    CHECK(set.active() && set.empty());
  }
  // Sprite colour: the lit texels decide the hue, transparent ones are ignored; the cache hands back the same entry.
  {
    oc::assets::EffectSprite sprite;
    sprite.w = 2, sprite.h = 2;
    sprite.rgba = {255, 100, 0, 255, 255, 120, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0};
    SpriteColourCache cache;
    const SpriteColour& colour = cache.get(sprite);
    CHECK(std::fabs(colour.coverage - 0.5f) < 1e-4f);
    CHECK(colour.rgb[0] > 0.99f && colour.rgb[1] > 0.38f && colour.rgb[1] < 0.5f && colour.rgb[2] < 0.01f);
    CHECK(&cache.get(sprite) == &colour);
  }
  // Hue: a white core over a blue halo gives a blue light (chroma-weighted mean) whose brightness still counts the white core; a pure white
  // sprite gives a dim one (chroma_scale 0.25: white is quarter weight).
  {
    oc::assets::EffectSprite sprite;
    sprite.w = 2, sprite.h = 2;
    sprite.rgba = {255, 255, 255, 255, 255, 255, 255, 255, 40, 100, 255, 255, 40, 100, 255, 255};
    SpriteColourCache cache;
    const SpriteColour& colour = cache.get(sprite);
    CHECK(colour.rgb[2] > 0.95f && colour.rgb[0] < 0.5f * colour.rgb[2] && colour.sat > 0.5f);
    CHECK(colour.bright > 0.99f);
    CHECK(chroma_scale(colour.rgb) > 0.9f);
    const float white[3] = {1, 1, 1}, orange[3] = {1, 0.5f, 0.1f};
    CHECK(std::fabs(chroma_scale(white) - 0.25f) < 1e-4f && chroma_scale(orange) == 1.0f);
  }
  // Spell shadow: nothing without live light; a dim light leaves the shadow to the sun, a bright one takes it, a bright one out of reach does not.
  {
    const float unit[3] = {0, 500, 0};
    oc::render::LightSet set;
    CHECK(oc::render::spell_shadow(set, unit).level == 0.0f);
    set.light[0] = {{3000, 0, 0}, {2.0f, 2.0f, 2.0f}, 8000.0f, false, 0.0f};
    set.count = 1;
    CHECK(oc::render::spell_shadow(set, unit).level == 0.0f);
    set.light[0].live = 0.1f;
    CHECK(oc::render::spell_shadow_share(oc::render::spell_shadow(set, unit).level) == 0.0f);
    set.light[0].live = 2.0f;
    oc::render::SpellShadow shadow = oc::render::spell_shadow(set, unit);
    CHECK(oc::render::spell_shadow_share(shadow.level) == 1.0f);
    set.light[0].radius = 2000.0f;
    CHECK(oc::render::spell_shadow(set, unit).level == 0.0f);
    // Away from the light, at most the slope limit; a light overhead throws it shorter.
    CHECK(shadow.slope_x < 0.0f && std::fabs(shadow.slope_z) < 1e-5f && std::fabs(shadow.slope_x + oc::render::kMaxShadowSlope) < 1e-4f);
    set.light[0] = {{1500, 3500, 0}, {2.0f, 2.0f, 2.0f}, 8000.0f, false, 2.0f};
    shadow = oc::render::spell_shadow(set, unit);
    CHECK(std::fabs(shadow.slope_x + 0.5f) < 1e-4f);
    // Lights around the unit cancel; one close to its axis barely leans it.
    set.light[0] = {{1500, 500, 0}, {2.0f, 2.0f, 2.0f}, 8000.0f, false, 2.0f};
    set.light[1] = {{-1500, 500, 0}, {2.0f, 2.0f, 2.0f}, 8000.0f, false, 2.0f};
    set.count = 2;
    shadow = oc::render::spell_shadow(set, unit);
    CHECK(shadow.level > 3.0f && std::fabs(shadow.slope_x) < 1e-4f);
    set.light[0] = {{50, 500, 20}, {2.0f, 2.0f, 2.0f}, 8000.0f, false, 2.0f};
    set.count = 1;
    shadow = oc::render::spell_shadow(set, unit);
    CHECK(std::hypot(shadow.slope_x, shadow.slope_z) < 0.1f);
  }
  // Live level: the glow eases down after its cluster is gone, the live level drops at once.
  {
    LightTracker tracker;
    LightCluster cluster;
    cluster.weight = 3.0f;
    cluster.size = 200.0f;
    tracker.update(std::vector<LightCluster>{cluster});
    tracker.update(std::vector<LightCluster>{cluster});
    CHECK(tracker.set().light[0].live > 1.0f);
    tracker.update(std::vector<LightCluster>{});
    const oc::render::LightSet after = tracker.set();
    CHECK(after.count == 1 && after.light[0].rgb[0] > 0.2f && after.light[0].live == 0.0f);
  }
  // Cap: however many lights stack, the multiplier stays below 1 + 0.9; typical bursts sit at 1.2-1.5; the hue (blue / red = 10) survives the knee.
  {
    oc::render::LightSet set;
    for (int i = 0; i < oc::render::LightSet::kMax; ++i) {
      set.light[static_cast<size_t>(set.count++)] = {{0, 0, 0}, {1.5f, 1.5f, 1.5f}, 3000.0f, false};
    }
    const float origin[3] = {0, 0, 0};
    const auto stacked = set.multiplier_at(origin);
    CHECK(stacked[0] < 1.8f && stacked[0] > 1.7f);
    oc::render::LightSet one;
    one.light[0] = {{0, 0, 0}, {0.6f, 0.4f, 0.1f}, 3000.0f, false};
    one.count = 1;
    const auto single = one.multiplier_at(origin);
    CHECK(single[0] > 1.3f && single[0] < 1.45f && single[2] < 1.1f);
    CHECK(oc::render::light_knee(0.5f) < 0.33f && oc::render::light_knee(100.0f) < 0.9f);
    one.light[0].rgb = {0.2f, 0.4f, 2.0f};
    const auto blue = one.multiplier_at(origin);
    CHECK(blue[2] > blue[1] && blue[1] > blue[0] && (blue[2] - 1.0f) / (blue[0] - 1.0f) > 9.0f);
  }
  // Reset between packs: a tracker that lit one cast and decayed is empty; the next cast starts from level 0 again (same first-tick level as the first cast).
  {
    LightTracker tracker;
    const auto clusters = cluster_sources({src(0, 0, 0, 0.2f, 0.4f, 1.0f, 3.0f)});
    tracker.update(clusters);
    const float first = tracker.set().light[0].rgb[2];
    for (int i = 0; i < 40; ++i) tracker.update(clusters);
    for (int i = 0; i < 80; ++i) tracker.update({});
    CHECK(tracker.active() == 0);
    tracker.update(clusters);
    CHECK(std::fabs(tracker.set().light[0].rgb[2] - first) < 1e-5f);
    for (int i = 0; i < 40; ++i) tracker.update(clusters);
    tracker.reset();
    CHECK(tracker.active() == 0 && tracker.set().empty());
  }
  if (g_fail) std::printf("%d failures\n", g_fail);
  else std::printf("test_lights: ok\n");
  return g_fail ? 1 : 0;
}
