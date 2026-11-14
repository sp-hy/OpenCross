// BattleView: forwards of the spell-run lighting (battle_spell_lights.h), and the [Shaders] spell rim and cast glow built on the same state.
#include "game/battle/battle_view.h"

#include <algorithm>

#include "data/tables.h"

namespace oc::game {

void BattleView::fx_spell_run_begin() {
  spell_.run_begin(fx_.get(), fx_lights_);
}

void BattleView::fx_spell_light_tick() {
  effects::ViewParams view;
  if (fx_) view = fx_view();
  else fx_ambient_release();
  if (spell_.tick(fx_.get(), view, fx_lights_, fx_ambient_.scur, fx_lit_, tick_count_))
    stage_scene_.set_spell_lights(spell_lights_shown_ ? spell_.set : render::LightSet{});
}

// [Shaders] CastGlow: while an element is charged or released, its casters' glow rises to 1 in 15 ticks; it falls back in 30 once the
// cast is over. Elements without a colour give no glow; the all-colour element glows white.
void BattleView::cast_glow_tick() {
  static constexpr float kColours[6][3] = {{0.35f, 1.0f, 0.4f}, {1.0f, 1.0f, 0.92f}, {0.55f, 0.35f, 0.85f},
                                           {1.0f, 0.35f, 0.2f}, {1.0f, 0.85f, 0.25f}, {0.3f, 0.6f, 1.0f}};
  const bool casting = cast_tint_ && battle_ && (act_.phase == Act::Phase::Cast || act_.phase == Act::Phase::Release) && act_.hit < act_.ev.size() &&
                       act_.ev[act_.hit].kind == battle::ActionEvent::Kind::Element;
  const float* colour = nullptr;
  if (casting) {
    const u8 mask = element_colour(act_.ev[act_.hit].element, act_.actor) & 0xFC;
    static constexpr float kWhite[3] = {1.0f, 1.0f, 1.0f};
    const int index = data::colour_index_of_bit(mask);
    colour = mask == 0xFC ? kWhite : index >= 0 ? kColours[index] : nullptr;
  }
  for (int unit = 0; unit < battle::kUnits; ++unit) {
    const bool lit = colour && std::find(act_.casters.begin(), act_.casters.end(), unit) != act_.casters.end();
    float& glow = cast_glow_[static_cast<size_t>(unit)];
    if (lit) {
      for (size_t channel = 0; channel < 3; ++channel) cast_glow_rgb_[static_cast<size_t>(unit)][channel] = colour[channel];
      glow = std::min(1.0f, glow + 1.0f / 15.0f);
    } else {
      glow = std::max(0.0f, glow - 1.0f / 30.0f);
    }
  }
}

// The key light is the stage's sun. The rim colour follows the summed spell light: white without a spell, the light's hue (its largest channel scaled to 1.5) at full
// strength once the light's largest channel reaches 0.67.
render::CharacterShading BattleView::unit_shading(int unit) const {
  render::CharacterShading shading = character_shading_;
  // The stage's sun, so the shading stays put as the camera moves and agrees with the cast shadows.
  shading.world_key = true;
  const std::array<float, 3> sun = stage_scene_.key_light().dir;
  for (size_t axis = 0; axis < 3; ++axis) shading.key_dir[axis] = sun[axis];
  if (shading.rim && spell_rim_) {
    float sum[3] = {0, 0, 0};
    for (int i = 0; i < spell_.set.count; ++i)
      for (size_t channel = 0; channel < 3; ++channel) sum[channel] += spell_.set.light[static_cast<size_t>(i)].rgb[channel];
    const float peak = std::max({sum[0], sum[1], sum[2]});
    if (peak > 0.0f) {
      const float weight = std::min(1.0f, peak * 1.5f);
      for (size_t channel = 0; channel < 3; ++channel) shading.rim_rgb[channel] = 1.0f + (1.5f * sum[channel] / peak - 1.0f) * weight;
    }
  }
  if (unit >= 0 && unit < battle::kUnits && cast_glow_[static_cast<size_t>(unit)] > 0.0f) {
    shading.glow = cast_glow_[static_cast<size_t>(unit)];
    for (size_t channel = 0; channel < 3; ++channel) shading.glow_rgb[channel] = cast_glow_rgb_[static_cast<size_t>(unit)][channel];
  }
  return shading;
}

}  // namespace oc::game
