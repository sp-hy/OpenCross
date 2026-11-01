// SpellLights: spell-run lighting for element / tech effects.
#include "game/battle/battle_spell_lights.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/battle/battle_fx.h"
#include "import/tim.h"

namespace oc::game {

void SpellLights::run_begin(const BattleFxRun* fx, const effects::LightTable& lights) {
  run = fx;
  last_ticks = 0;
  clear_colour_caches();
  slot_base = lights;
  slot_live = {false, false, false};
  amb_dark = false;
}

void SpellLights::clear_colour_caches() {
  sprite_colours.clear();
  mesh_colours.clear();
}

// Enhancement lighting, spell part: once per tick, the running effect's bright additive primitives become point
// lights. Sources, all with blend B + F (factor 1) or B + F/4 (factor 0.25):
//  * sheet sprites (EmitterSystem::collect_quads): colour = mean colour of the sprite's lit texels (SpriteColourCache) x object colour / 128;
//  * mesh sprites (type 14): colour = the mean of the mesh's atlas texels at its vertices' UVs x mean vertex colour x object colour / 128;
//  * bespoke overlay primitives (FxFrame::flats / textured): colour = the corner colours; their world position is un-projected from the screen
//    position at depth z = slot * 16 (an approximation: the OT bias is unknown); full-screen washes (> 60 % of the screen) are not sources.
// brightness = the largest colour channel; weight = brightness x clamp(sqrt(screen area px) / 40, 0.05, 3) x coverage x factor (meshes:
// brightness x clamp(object scale, 0.1, 3) x factor); the source's world radius is its size (sprites: half the quad's world side, meshes: the
// mesh's bounding radius x scale). Light sprites (type 17) and the stage's own emitters give nothing. effects::cluster_sources / LightTracker do
// the rest; the result feeds the units (DrawParams::lights) and the stage (StageScene::set_spell_lights). OC_LIGHT_TRACE=1 logs the lights.
bool SpellLights::tick(BattleFxRun* fx, const effects::ViewParams& view, const effects::LightTable& light_table, const float (&ambient)[3], std::span<const bool> unit_lit, int tick_count) {
  if (!lights_on) {
    if (set.active() || tracker.active()) {
      tracker.reset();
      set = render::LightSet{};
      return true;
    }
    return false;
  }
  // A new effect run (a new BattleFxRun, or its tick counter went back): the per-pack caches (sprite / mesh pointers) and the slot-write tracking restart.
  // The tracker keeps its own decay. This stops a charge pack -> element pack swap from keeping dangling sprite pointers and a previous pack's
  // persistent light slots (intensities the new pack never wrote) from lighting the next cast.
  if (fx && (fx != run || fx->ticks() < last_ticks)) run_begin(fx, light_table);
  if (fx) last_ticks = fx->ticks();
  if (!fx) run = nullptr;
  std::vector<effects::LightSource> src;
  if (fx) {
    // Camera space (PS1 axes) -> render world: P = rot^T (c - trans), then y up.
    auto to_world = [&](const float cam_pos[3], float out[3]) {
      const float delta[3] = {cam_pos[0] - view.trans[0], cam_pos[1] - view.trans[1], cam_pos[2] - view.trans[2]};
      for (size_t k = 0; k < 3; ++k) out[k] = view.rot.m[k] * delta[0] + view.rot.m[3 + k] * delta[1] + view.rot.m[6 + k] * delta[2];
      out[1] = -out[1];
    };
    FxFrame frame;
    fx->collect(view, 1.0f, frame);
    static constexpr int kLoop[4] = {0, 1, 3, 2};
    auto area_of = [&](const float* x, const float* y) {
      float area = 0;
      for (int k = 0; k < 4; ++k) {
        const int from = kLoop[k], to = kLoop[(k + 1) % 4];
        area += x[from] * y[to] - x[to] * y[from];
      }
      return std::fabs(area) * 0.5f;
    };
    auto screen_source = [&](const float* x, const float* y, float z, const float rgb[3], float coverage, float bf) {
      const float area = area_of(x, y);
      // A screen wash (over a third of the screen) or on the camera plane is not a light.
      if (area > (1.0f / 3.0f) * 320.0f * 224.0f || z < 150.0f) return;
      effects::LightSource source;
      const float px = std::sqrt(area);
      z = std::max(z, 200.0f);
      for (size_t k = 0; k < 3; ++k) source.rgb[k] = rgb[k];
      source.weight = std::max({rgb[0], rgb[1], rgb[2]}) * effects::chroma_scale(rgb) * std::clamp(px / 40.0f, 0.05f, 3.0f) * coverage * bf;
      source.size = 0.5f * px * z / std::max(view.h, 1.0f);
      float cx = 0, cy = 0;
      for (int k = 0; k < 4; ++k) cx += x[k] * 0.25f, cy += y[k] * 0.25f;
      const float centre[3] = {(cx - view.ox) * z / view.h, (cy - view.oy) * z / view.h, z};
      to_world(centre, source.pos);
      src.push_back(source);
    };
    for (const effects::DrawQuad& quad : frame.quads) {
      const float bf = quad.blend == 2 ? 1.0f : quad.blend == 4 ? 0.25f : 0.0f;
      if (bf == 0.0f || !quad.sprite || quad.untextured) continue;
      const effects::SpriteColour& sc = sprite_colours.get(*quad.sprite);
      if (sc.coverage <= 0.0f) continue;
      float rgb[3];
      for (size_t k = 0; k < 3; ++k) rgb[k] = std::min(sc.rgb[k] * (k == 0 ? quad.r : k == 1 ? quad.g : quad.b) / 128.0f, 2.0f);
      const float area = area_of(quad.x, quad.y);
      // A screen wash / on the camera plane: never a point light.
      if (area > (1.0f / 3.0f) * 320.0f * 224.0f || quad.cam[2] < 150.0f) continue;
      const float px = std::sqrt(area);
      effects::LightSource source;
      for (size_t k = 0; k < 3; ++k) source.rgb[k] = rgb[k];
      // brightness = the sprite's mean texel peak x the object colour; chroma_scale: white / grey sprites weigh a quarter
      const float mod = std::max({static_cast<float>(quad.r), static_cast<float>(quad.g), static_cast<float>(quad.b)}) / 128.0f;
      source.weight = std::min(sc.bright * mod, 1.5f) * effects::chroma_scale(rgb) * std::clamp(px / 40.0f, 0.05f, 3.0f) * sc.coverage * bf;
      source.size = 0.5f * px * std::max(quad.cam[2], 200.0f) / std::max(view.h, 1.0f);
      to_world(quad.cam, source.pos);
      src.push_back(source);
    }
    for (const effects::FlatQuad& quad : frame.flats) {
      const float bf = quad.blend == 2 ? 1.0f : quad.blend == 4 ? 0.25f : 0.0f;
      if (bf == 0.0f) continue;
      const float rgb[3] = {quad.r / 255.0f, quad.g / 255.0f, quad.b / 255.0f};
      screen_source(quad.x, quad.y, static_cast<float>(quad.slot) * 16.0f, rgb, 1.0f, bf);
    }
    for (const effects::FxPrim& prim : frame.textured) {
      const float bf = prim.blend == 2 ? 1.0f : prim.blend == 4 ? 0.25f : 0.0f;
      if (bf == 0.0f) continue;
      const int corner_count = prim.kind == effects::FxPrim::Kind::Tri ? 3 : 4;
      float x[4], y[4], rgb[3] = {0, 0, 0};
      for (int k = 0; k < 4; ++k) {
        const int corner = k < corner_count ? k : 2;
        x[k] = prim.v[corner].x, y[k] = prim.v[corner].y;
        if (k < corner_count) rgb[0] += prim.v[corner].r / 128.0f / static_cast<float>(corner_count), rgb[1] += prim.v[corner].g / 128.0f / static_cast<float>(corner_count), rgb[2] += prim.v[corner].b / 128.0f / static_cast<float>(corner_count);
      }
      screen_source(x, y, static_cast<float>(prim.slot) * 16.0f, rgb, 0.8f, bf);
    }
    std::vector<effects::EmitterSystem::MeshInstance> meshes;
    fx->collect_meshes(view, 1.0f, meshes);
    for (const auto& mi : meshes) {
      const float bf = mi.blend == 2 ? 1.0f : mi.blend == 4 ? 0.25f : 0.0f;
      if (bf == 0.0f || !mi.name || mi.sprite_type != 14) continue;
      auto it = mesh_colours.find(mi.name);
      if (it == mesh_colours.end()) {
        // rgb, bounding radius (file units).
        std::array<float, 4> mean{1, 1, 1, 0};
        size_t part = 0;
        if (const assets::StageAsset* asset = fx->pack().mesh_asset(*mi.name, part); asset && part < asset->parts.size() && !asset->parts[part].vertices.empty()) {
          double sum[3] = {0, 0, 0}, wsum = 0;
          float rmax = 0;
          for (const assets::StageVertex& vertex : asset->parts[part].vertices) {
            float colour[3] = {vertex.color[0], vertex.color[1], vertex.color[2]};
            // Textured corner: the atlas texel at its UV, x the vertex colour.
            if (vertex.uv_scale[0] > 0.0f)
              for (const assets::StageSubMesh& sm : asset->parts[part].submeshes)
                if (sm.texture >= 0 && static_cast<size_t>(sm.texture) < asset->textures.size()) {
                  const import::Image& im = asset->textures[static_cast<size_t>(sm.texture)].image;
                  if (!im.indexed && im.width && im.height && im.pixels.size() >= size_t(im.width) * im.height * 4) {
                    const u32 px = std::min<u32>(static_cast<u32>(std::max(vertex.uv[0], 0.0f) * static_cast<float>(im.width)), im.width - 1);
                    const u32 py = std::min<u32>(static_cast<u32>(std::max(1.0f - vertex.uv[1], 0.0f) * static_cast<float>(im.height)), im.height - 1);
                    const u8* texel = &im.pixels[(size_t(py) * im.width + px) * 4];
                    if (texel[3] != 0)
                      for (int k = 0; k < 3; ++k) colour[k] *= texel[k] / 255.0f;
                  }
                  break;
                }
            const float mxc = std::max({colour[0], colour[1], colour[2]}), mnc = std::min({colour[0], colour[1], colour[2]});
            // Chroma x brightness (a white core does not decide the hue).
            const float weight = (mxc - mnc) * mxc + 0.02f * mxc + 0.005f;
            for (int k = 0; k < 3; ++k) sum[k] += colour[k] * weight;
            wsum += weight;
            rmax = std::max(rmax, std::sqrt(vertex.pos[0] * vertex.pos[0] + vertex.pos[1] * vertex.pos[1] + vertex.pos[2] * vertex.pos[2]));
          }
          for (size_t k = 0; k < 3; ++k) mean[k] = static_cast<float>(sum[k] / wsum);
          mean[3] = rmax;
        }
        it = mesh_colours.emplace(mi.name, mean).first;
      }
      effects::LightSource source;
      float mx = 0;
      for (size_t k = 0; k < 3; ++k) source.rgb[k] = std::min(it->second[k] * mi.rgb[k] / 128.0f, 2.0f), mx = std::max(mx, source.rgb[k]);
      const float scale = std::sqrt(mi.mat.m[0] * mi.mat.m[0] + mi.mat.m[3] * mi.mat.m[3] + mi.mat.m[6] * mi.mat.m[6]);
      source.weight = std::min(mx, 1.5f) * effects::chroma_scale(source.rgb) * std::clamp(scale, 0.1f, 3.0f) * bf;
      source.size = it->second[3] * scale;
      const float centre[3] = {mi.centre[0], mi.centre[1], mi.centre[2]};
      to_world(centre, source.pos);
      src.push_back(source);
    }
  }
  // The PS1's own light slots (the pack's type-17 `ligt` objects, intensity = scale x, falloff = angle x) while a pack runs: first-class lights, linear
  // falloff; the clusters they cover are dropped (not doubled), the others keep their smoothed glow.
  // Only slots this run has written: a pack's last light stays in the table (a persistent one is never zeroed), so a slot still equal to its value at
  // the run's start belongs to an earlier pack and is ignored.
  // Models are lit only while a pack's lit window is open (ops 0x61 / 0x63 / 0x68 on ... 0x62 / 0x64 / 0x69 off, `unit_lit`), and the same pack ramps the
  // ambient back at that moment: the slot lights follow that window through a gate (rise 0.2, fall 0.15 per tick), so the ending does not keep full-strength lights
  // while the ambient is already back at 1.
  bool any_lit = false;
  for (bool lit : unit_lit) any_lit = any_lit || lit;
  slot_gate += ((fx && any_lit ? 1.0f : 0.0f) - slot_gate) * (fx && any_lit ? 0.2f : 0.15f);
  if (slot_gate < 0.01f) slot_gate = 0.0f;
  // Contrast fade: a pack that darkens the scene (ambient below 0.9) wants its lights seen against that dark; as the ambient ramps back toward 1 all lights
  // fade with ((1 - ambient) / 0.3)^2, so ambient x (1 + lights) never climbs above the mid-spell level at the ending (full lights over a restored ambient did).
  const float amb_mean = (ambient[0] + ambient[1] + ambient[2]) / (3.0f * 128.0f);
  if (fx && amb_mean < 0.9f) amb_dark = true;
  const float contrast = amb_dark ? std::pow(std::clamp((1.0f - amb_mean) / 0.3f, 0.0f, 1.0f), 2.0f) : 1.0f;
  std::vector<render::PointLight> slots;
  if (fx && slot_gate > 0.0f)
    for (size_t i = 0; i < light_table.slot.size(); ++i) {
      const effects::LightTable::Slot& slot = light_table.slot[i];
      const effects::LightTable::Slot& base = slot_base.slot[i];
      if (!slot_live[i] && (slot.intensity != base.intensity || slot.falloff != base.falloff || slot.pos[0] != base.pos[0] || slot.pos[1] != base.pos[1] || slot.pos[2] != base.pos[2] ||
                                   slot.rgb[0] != base.rgb[0] || slot.rgb[1] != base.rgb[1] || slot.rgb[2] != base.rgb[2]))
        slot_live[i] = true;
      render::PointLight light;
      if (slot_live[i] && effects::ps1_slot_light(slot.pos, slot.rgb, slot.intensity, slot.falloff, light)) {
        light.live = any_lit ? std::max({light.rgb[0], light.rgb[1], light.rgb[2]}) : 0.0f;
        for (float& channel : light.rgb) channel *= slot_gate;
        slots.push_back(light);
      }
    }
  tracker.update(effects::drop_covered_clusters(effects::cluster_sources(std::move(src)), slots));
  const render::LightSet glow = tracker.set();
  set = render::LightSet{};
  for (const render::PointLight& light : slots)
    if (set.count < render::LightSet::kMax) set.light[static_cast<size_t>(set.count++)] = light;
  for (int i = 0; i < glow.count && set.count < render::LightSet::kMax; ++i) set.light[static_cast<size_t>(set.count++)] = glow.light[static_cast<size_t>(i)];
  // The contrast fade applies to the whole set (slots and glow): the ending over a restored ambient stays within ~20 % of the mid-spell level.
  if (contrast < 1.0f)
    for (int i = 0; i < set.count; ++i) {
      for (float& channel : set.light[static_cast<size_t>(i)].rgb) channel *= contrast;
      set.light[static_cast<size_t>(i)].live *= contrast;
    }
  // The scene ambient (1 = neutral).
  for (size_t k = 0; k < 3; ++k) set.ambient[k] = std::clamp(ambient[k] / 128.0f, 0.0f, 2.0f);
  static const bool trace = std::getenv("OC_LIGHT_TRACE") != nullptr;
  // Per 5 ticks: the lights, their summed colour and the multiplier at the strongest light.
  if (trace && tick_count % 5 == 0 && !set.empty()) {
    float sum[3] = {0, 0, 0};
    for (int i = 0; i < set.count; ++i)
      for (size_t k = 0; k < 3; ++k) sum[k] += set.light[static_cast<size_t>(i)].rgb[k];
    const auto multiplier = set.multiplier_at(set.light[0].pos.data());
    log::debug("lights: tick {} n {} slots {} sum ({:.2f},{:.2f},{:.2f}) peak multiplier ({:.2f},{:.2f},{:.2f}) ambient {:.2f}", tick_count, set.count, slots.size(),
               sum[0], sum[1], sum[2], multiplier[0], multiplier[1], multiplier[2], set.ambient[0]);
  }
  if (trace && tick_count % 10 == 0 && !set.empty())
    for (int i = 0; i < set.count; ++i) {
      const render::PointLight& light = set.light[static_cast<size_t>(i)];
      log::debug("lights: tick {} #{} pos ({:.0f},{:.0f},{:.0f}) rgb ({:.2f},{:.2f},{:.2f}) radius {:.0f}", tick_count, i, light.pos[0], light.pos[1], light.pos[2], light.rgb[0],
                 light.rgb[1], light.rgb[2], light.radius);
    }
  return true;
}

}  // namespace oc::game
