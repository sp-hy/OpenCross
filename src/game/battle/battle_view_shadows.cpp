// BattleView: who casts a shadow this frame, and the [Enhancements] Lighting mask pass.
#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

namespace oc::game {

using namespace bv;

namespace {

// Per tick: the spell taking a shadow over and giving it back, and the shadow turning. Fast enough to keep up with the effect, slow enough not to flicker.
constexpr float kShareEase = 0.3f, kSlopeEase = 0.15f;
// Clearance around a unit's shadow for the blur.
constexpr float kShadowMargin = 640.0f;

}  // namespace

render::Mat4 BattleView::unit_model_matrix(int unit, const effects::FxModel* om) const {
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  const float x = lerp(visual.px, visual.x, alpha_), z = lerp(visual.pz, visual.z, alpha_), y = lerp(visual.py, visual.y, alpha_);
  const float yaw = std::atan2(-lerp(visual.pface_x, visual.face_x, alpha_), -lerp(visual.pface_z, visual.face_z, alpha_));
  render::Mat4 flip;
  flip.at(1, 1) = -1.0f;
  if (om && om->steer) {
    const effects::FxModelPose op = effects::fx_model_pose(*om, alpha_);
    return render::mat4_translation(op.pos[0], op.pos[1], op.pos[2]) * render::mat4_rotation_y(yaw) * flip * render::mat4_from_rt(op.rel.m, {0, 0, 0});
  }
  return render::mat4_translation(x, y, z) * render::mat4_rotation_y(yaw) * flip;
}

bool BattleView::weapon_matrix(int unit, int slot, render::Mat4& out) const {
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!visual.weapon || !visual.weapon->gpu || anm_weapon_hidden(unit, slot)) return false;
  const assets::MountPoint& mount = visual.model->asset.attach.mounts[static_cast<size_t>(slot)];
  const int attach = slot == 0 ? visual.weapon_attach : (mount.used ? static_cast<int>(mount.attach) : -1);
  if (attach < 0) return false;
  const assets::AttachPoint& ap = visual.model->asset.attach.points[static_cast<size_t>(attach)];
  if (ap.joint >= palette_.size()) return false;
  // Slot k = JointMatrix(attach.joint) * [RotMatrixZYX(mount.rot) | attach.pos + mount.pos] (the offset in the joint's
  // space, not turned by the mount rotation; 4096 = a turn). Guile's wand sits on attach 31 = joint 21, a bone the tech clips animate.
  assets::Vec3 off = ap.pos, deg{0, 0, 0};
  if (mount.used)
    for (size_t k = 0; k < 3; ++k) {
      off[k] += mount.pos[k];
      deg[k] = mount.rot[k] * (360.0f / 4096.0f);
    }
  out = palette_[ap.joint] * render::mat4_from_rt(assets::rotation_zyx(deg), off);
  return true;
}

// Every visible unit with a model casts a drop shadow; the stage scene builds the hulls.
void BattleView::collect_shadow_casters() {
  shadow_casters_.clear();
  if (!stage_scene_.ready()) return;
  for (int i = 0; i < battle::kUnits; ++i) {
    const battle::Unit& unit = battle_->unit(i);
    const Visual& visual = vis_[static_cast<size_t>(i)];
    if (showcase_on_ || !unit.present || !visual.has_model || !visual.model->gpu || fx_hidden_[static_cast<size_t>(i)] || unplaced_[static_cast<size_t>(i)] || (ghosted(i) && !entry_shown_[static_cast<size_t>(i)]) || anm_hidden(i)) continue;
    if (!unit.pc && lerp(visual.fade_prev, visual.fade, alpha_) <= 0.0f) continue;
    const effects::FxModel* om = fx_ && fx_->bespoke_models() && static_cast<size_t>(i) < fx_->bespoke_models()->units.size()
                                     ? &fx_->bespoke_models()->units[static_cast<size_t>(i)]
                                     : nullptr;
    // The overlay model's shadow flag (0x200) says whether the unit casts its shadow.
    if (om && (om->hidden || !(om->flags & 0x200)) && !om->own_shadow) continue;
    // The shadow flag is cleared by S5 op 0x22 / pack op 0x74.
    if (shadow_off_[static_cast<size_t>(i)]) continue;
    StageScene::ShadowCaster caster;
    caster.index = i;
    {
      // The shadow stays on the floor under the unit; its grey fades with the height.
      const std::array<float, 3> world = unit_world(i, alpha_);
      caster.x = world[0];
      caster.z = world[2];
      caster.y = std::max(0.0f, world[1]);
    }
    caster.yaw = std::atan2(-lerp(visual.pface_x, visual.face_x, alpha_), -lerp(visual.pface_z, visual.face_z, alpha_));
    caster.height = visual.height;
    caster.model = &visual.model->asset;
    // Footprint = the posed joint positions, from the same palette the draw uses.
    visual.pose.palette(alpha_, palette_);
    float lowest = palette_.empty() ? 0.0f : -palette_[0].at(1, 3);
    for (size_t j = 0; j < palette_.size(); ++j) {
      const float jx = palette_[j].at(0, 3), jz = palette_[j].at(2, 3);
      lowest = std::min(lowest, -palette_[j].at(1, 3));
      if (!caster.has_fp) {
        caster.fp.min_x = caster.fp.max_x = jx;
        caster.fp.min_z = caster.fp.max_z = jz;
        caster.has_fp = true;
      }
      caster.fp.min_x = std::min(caster.fp.min_x, jx);
      caster.fp.max_x = std::max(caster.fp.max_x, jx);
      caster.fp.min_z = std::min(caster.fp.min_z, jz);
      caster.fp.max_z = std::max(caster.fp.max_z, jz);
    }
    caster.clearance = std::max(0.0f, caster.y + lowest);
    shadow_casters_.push_back(caster);
  }
  // The pack's pseudo-actor casts a shadow once the Shadow op set its flag and it is not hidden (or an overlay draws it itself: ghost2).
  const bool pa_own_shadow = fx_ && fx_->bespoke_models() && fx_->bespoke_models()->pseudo_own_shadow;
  if (pa_.on && (!pa_.hidden || pa_own_shadow) && pa_.shadow && pa_.asset && pa_.pose) {
    if (pa_.height < 0) {
      float ext = 0;
      for (const assets::Mesh& mesh : pa_.asset->meshes)
        for (const assets::SkinVertex& sv : mesh.pool) ext = std::max(ext, std::fabs(sv.bind_pos[1]));
      pa_.height = ext > 50 ? ext : 1000.0f;
    }
    StageScene::ShadowCaster caster;
    caster.owner = StageScene::CasterOwner::Pseudo;
    caster.x = lerp(pa_.px, pa_.x, alpha_);
    caster.z = lerp(pa_.pz, pa_.z, alpha_);
    caster.y = std::max(0.0f, lerp(pa_.py, pa_.y, alpha_));
    caster.yaw = std::atan2(-lerp(pa_.pface_x, pa_.face_x, alpha_), -lerp(pa_.pface_z, pa_.face_z, alpha_));
    caster.height = pa_.height * pa_.scale[1];
    caster.model = pa_.asset;
    pa_.pose->palette(alpha_, palette_);
    for (size_t j = 0; j < palette_.size(); ++j) {
      const float jx = palette_[j].at(0, 3) * pa_.scale[0], jz = palette_[j].at(2, 3) * pa_.scale[2];
      if (!caster.has_fp) {
        caster.fp.min_x = caster.fp.max_x = jx;
        caster.fp.min_z = caster.fp.max_z = jz;
        caster.has_fp = true;
      }
      caster.fp.min_x = std::min(caster.fp.min_x, jx);
      caster.fp.max_x = std::max(caster.fp.max_x, jx);
      caster.fp.min_z = std::min(caster.fp.min_z, jz);
      caster.fp.max_z = std::max(caster.fp.max_z, jz);
    }
    shadow_casters_.push_back(caster);
  }
  showcase_shadows(shadow_casters_);
  stage_scene_.set_shadow_casters(shadow_casters_);
}

void BattleView::cast_shadow_pass(const render::Frame& frame) {
  stage_scene_.set_floor_shadow(nullptr, {});
  if (!spell_.lights_on || shadow_pipe_failed_ || !stage_scene_.ready() || shadow_casters_.empty()) return;
  if (!shadow_pipe_.ready() && !shadow_pipe_.init(*ctx_)) {
    log::warn("battle: shadow pipeline failed, cast shadows off");
    shadow_pipe_failed_ = true;
    return;
  }
  const std::array<float, 3> sun = stage_scene_.key_light().dir;
  bool spell_shadows = false;
  const bool step = tick_count_ != shadow_tick_;
  shadow_tick_ = tick_count_;
  shadow_pipe_.begin_mask(frame);
  for (const StageScene::ShadowCaster& caster : shadow_casters_) {
    const float middle[3] = {caster.x, caster.y + caster.height * 0.5f, caster.z};
    std::array<render::ShadowPipeline::Cast, render::ShadowPipeline::kChannels> casts;
    casts[0] = {sun, 1.0f};
    if (caster.owner != StageScene::CasterOwner::Showcase) {
      ShadowLean& lean = shadow_lean_[static_cast<size_t>(caster.owner == StageScene::CasterOwner::Unit ? caster.index : battle::kUnits)];
      const render::SpellShadow target = render::spell_shadow(spell_.set, middle);
      if (step) {
        lean.share += (render::spell_shadow_share(target.level) - lean.share) * kShareEase;
        if (lean.share < 0.01f) lean.share = 0.0f;
        // A new shadow starts where it belongs; one losing its light stays where it was as it fades.
        const float ease = lean.share < 0.02f ? 1.0f : target.level > 0.0f ? kSlopeEase : 0.0f;
        lean.slope_x += (target.slope_x - lean.slope_x) * ease;
        lean.slope_z += (target.slope_z - lean.slope_z) * ease;
      }
      const float len = std::sqrt(lean.slope_x * lean.slope_x + 1.0f + lean.slope_z * lean.slope_z);
      casts[0].strength = 1.0f - lean.share;
      casts[1] = {{-lean.slope_x / len, 1.0f / len, -lean.slope_z / len}, lean.share};
      spell_shadows = spell_shadows || lean.share > 0.0f;
    }
    if (caster.owner == StageScene::CasterOwner::Showcase) {
      const ShowcaseModel& model = showcase_[static_cast<size_t>(caster.index)];
      if (!model.model->gpu) continue;
      render::Mat4 flip;
      flip.at(1, 1) = -1.0f;
      model.pose.palette(alpha_, palette_);
      shadow_pipe_.cast(*model.model->gpu, render::mat4_translation(model.x, 0.0f, model.z) * render::mat4_rotation_y(caster.yaw) * flip, palette_, 0, 0, casts);
    } else if (caster.owner == StageScene::CasterOwner::Pseudo) {
      if (!pa_.gpu || !pa_.pose) continue;
      pa_.pose->palette(alpha_, palette_);
      shadow_pipe_.cast(*pa_.gpu, pa_matrix(alpha_), palette_, 0, 0, casts);
    } else {
      const int unit = caster.index;
      const Visual& visual = vis_[static_cast<size_t>(unit)];
      const effects::FxModel* om = fx_ && fx_->bespoke_models() && static_cast<size_t>(unit) < fx_->bespoke_models()->units.size() ? &fx_->bespoke_models()->units[static_cast<size_t>(unit)] : nullptr;
      render::MeshPipeline::DrawParams parts;
      shading_params(visual.model->asset, parts);
      if (om) parts.hidden_meshes = (parts.hidden_meshes | om->part_hide) & ~om->part_show;
      if (!anm_unit_apply(unit, parts)) continue;
      const render::Mat4 model_mat = unit_model_matrix(unit, om);
      if (!battle_->unit(unit).pc)
        for (render::ShadowPipeline::Cast& cast : casts) cast.strength *= std::clamp(lerp(visual.fade_prev, visual.fade, alpha_), 0.0f, 1.0f);
      visual.pose.palette(alpha_, palette_);
      shadow_pipe_.cast(*visual.model->gpu, model_mat, palette_, parts.hidden_meshes, parts.env_meshes, casts);
      for (int slot = 0; slot < 2; ++slot) {
        render::Mat4 weapon;
        if (weapon_matrix(unit, slot, weapon)) shadow_pipe_.cast(*visual.weapon->gpu, model_mat, std::span<const render::Mat4>(&weapon, 1), 0, 0, casts);
      }
    }
  }
  shadow_pipe_.end_mask();
  // Spell shadows can fall any way; the sun's fall on the side away from it.
  constexpr float kReach = render::ShadowPipeline::kReach;
  std::array<float, 4> rect{-kReach, -kReach, kReach, kReach};
  if (!spell_shadows) {
    rect = {kReach, kReach, -kReach, -kReach};
    for (const StageScene::ShadowCaster& caster : shadow_casters_) {
      const float top = caster.y + caster.height;
      const float tip_x = caster.x - sun[0] / sun[1] * top, tip_z = caster.z - sun[2] / sun[1] * top;
      const float radius = std::max({std::fabs(caster.fp.min_x), std::fabs(caster.fp.max_x), std::fabs(caster.fp.min_z), std::fabs(caster.fp.max_z), caster.height * 0.5f}) + kShadowMargin;
      rect[0] = std::min({rect[0], caster.x - radius, tip_x - radius});
      rect[1] = std::min({rect[1], caster.z - radius, tip_z - radius});
      rect[2] = std::max({rect[2], caster.x + radius, tip_x + radius});
      rect[3] = std::max({rect[3], caster.z + radius, tip_z + radius});
    }
    rect = {std::max(rect[0], -kReach), std::max(rect[1], -kReach), std::min(rect[2], kReach), std::min(rect[3], kReach)};
  }
  stage_scene_.set_floor_shadow(&shadow_pipe_, rect);
}

}  // namespace oc::game
