// BattleView showcase ([Debug] Showcase): every playable character in its battle stance on the stage, evenly spaced in a staggered grid, while
// the battle's own units are hidden. Each model loops its ready-to-strike clip (the HIT% stance, clip 2) with the part / weapon words its spawn script sets on its first tick.
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
namespace oc::game {

using namespace bv;

namespace {
constexpr int kShowcaseChars = 44;
constexpr int kShowcaseColumns = 9;
// World units between neighbours along a row (z) and between rows (x); odd rows shift by half a column so the back rows show between the front.
constexpr float kShowcaseColumnStep = 950.0f, kShowcaseRowStep = 2000.0f;
}  // namespace

void BattleView::showcase_load() {
  showcase_.clear();
  showcase_.reserve(kShowcaseChars);
  const int rows = (kShowcaseChars + kShowcaseColumns - 1) / kShowcaseColumns;
  for (int char_id = 0; char_id < kShowcaseChars; ++char_id) {
    const std::string stem = ModelCache::player_model(*vfs_, char_id);
    std::shared_ptr<ModelHandle> handle = load_model_for(stem);
    if (!handle || !handle->gpu) {
      log::warn("showcase: character {} ({}) has no battle model", char_id, stem);
      continue;
    }
    ShowcaseModel& model = showcase_.emplace_back();
    model.model = handle;
    model.pose.set_model(&handle->asset);
    model.pose.play(model.pose.clip_for(2) >= 0 ? 2 : 1, 0);
    float ext = 0;
    for (const assets::Mesh& mesh : handle->asset.meshes)
      for (const assets::SkinVertex& sv : mesh.pool) ext = std::max(ext, std::fabs(sv.bind_pos[1]));
    model.height = ext > 50 ? ext : 1000.0f;
    if (float hx = 0, hy = 0, hz = 0; model.pose.attach_now(14, 1.0f, hx, hy, hz) && -hy > 50.0f) model.height = -hy;

    if (std::shared_ptr<ModelScripts> scripts = load_scripts_for(stem); scripts && scripts->has_s5) {
      model.weapon_hide = scripts->s5.initial_weapon_hide();
      bseq::Vm vm;
      if (vm.start(&scripts->s5, 8, 0)) {
        bseq::Ctx ctx;
        ctx.rand = [] { return 0; };
        std::vector<bseq::Event> events;
        vm.tick(ctx, events);
        for (const bseq::Event& event : events) {
          if (event.kind != bseq::Event::Kind::PartShow || event.b < 0 || event.b >= 32) continue;
          const u32 bit = 1u << event.b;
          if (event.a) model.part_show |= bit, model.part_hide &= ~bit;
          else model.part_hide |= bit, model.part_show &= ~bit;
        }
      }
    }

    const battle::PartyMember member = battle::make_party_member(*tables_, char_id, 0, "char");
    if (const data::GearRec* gear = tables_->gear_rec(member.eq.weapon_id)) {
      model.weapon = load_weapon(char_id, gear->tier() & 0xF);
      const auto& points = handle->asset.attach.points;
      const assets::MountPoint& mount = handle->asset.attach.mounts[0];
      model.weapon_attach = mount.used ? mount.attach : points[0x12].used ? 0x12 : points[7].used ? 7 : 15;
    }

    const int index = static_cast<int>(showcase_.size()) - 1;
    const int row = index / kShowcaseColumns, column = index % kShowcaseColumns;
    // Row 0 stands furthest back (largest x): the short last row ends up in front.
    model.x = (static_cast<float>(rows - 1) * 0.5f - static_cast<float>(row)) * kShowcaseRowStep;
    model.z = (static_cast<float>(column) - static_cast<float>(kShowcaseColumns - 1) * 0.5f) * kShowcaseColumnStep +
              (row % 2 ? kShowcaseColumnStep * 0.5f : 0.0f);
  }
  log::info("showcase: {} characters on stage", showcase_.size());
}

void BattleView::showcase_tick() {
  for (ShowcaseModel& model : showcase_) model.pose.tick();
}

void BattleView::showcase_shadows(std::vector<StageScene::ShadowCaster>& casters) {
  for (size_t i = 0; i < showcase_.size(); ++i) {
    const ShowcaseModel& model = showcase_[i];
    StageScene::ShadowCaster caster;
    caster.owner = StageScene::CasterOwner::Showcase;
    caster.index = static_cast<int>(i);
    caster.x = model.x;
    caster.z = model.z;
    caster.yaw = std::atan2(-kShowcaseFaceX, -kShowcaseFaceZ);
    caster.height = model.height;
    caster.model = &model.model->asset;
    model.pose.palette(alpha_, palette_);
    for (const render::Mat4& joint : palette_) {
      const float jx = joint.at(0, 3), jz = joint.at(2, 3);
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
    casters.push_back(caster);
  }
}

void BattleView::showcase_draw(const render::Mat4& view_proj, const render::Mat4& view) {
  render::Mat4 flip;
  flip.at(1, 1) = -1.0f;
  for (const ShowcaseModel& model : showcase_) {
    const render::Mat4 model_mat = render::mat4_translation(model.x, 0.0f, model.z) * render::mat4_rotation_y(std::atan2(-kShowcaseFaceX, -kShowcaseFaceZ)) * flip;
    model.pose.palette(alpha_, palette_);
    render::MeshPipeline::DrawParams dp;
    dp.view_proj = view_proj;
    dp.view = view;
    dp.model = model_mat;
    dp.palette = palette_;
    dp.linear = linear_filter_ < 0 ? hd_ : linear_filter_ != 0;
    dp.hd = hd_;
    if (spell_lights_shown_ && spell_.set.active()) dp.lights = &spell_.set;
    unit_ot_faces(model.model->asset, dp);
    shading_params(model.model->asset, dp);
    dp.hidden_meshes = (dp.hidden_meshes | model.part_hide) & ~model.part_show;
    meshes_->draw(*model.model->gpu, dp);

    if (!model.weapon || !model.weapon->gpu || model.weapon_attach < 0 || (model.weapon_hide & 1)) continue;
    const assets::AttachPoint& point = model.model->asset.attach.points[static_cast<size_t>(model.weapon_attach)];
    if (point.joint >= palette_.size()) continue;
    const assets::MountPoint& mount = model.model->asset.attach.mounts[0];
    render::MeshPipeline::DrawParams wd = dp;
    wd.part_mask = wd.part_semi = 0;
    wd.near_meshes = 0;
    wd.light.on = false;
    const std::array<float, 3> light = stage_scene_.light();
    float to_light[3] = {light[0] - model.x, light[1], light[2] - model.z};
    if (std::fabs(to_light[0]) > 16384.0f || std::fabs(to_light[1]) > 16384.0f || std::fabs(to_light[2]) > 16384.0f) to_light[0] = 0.0f, to_light[1] = 1.0f, to_light[2] = 0.0f;
    const float len = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] + to_light[2] * to_light[2]);
    wd.light.spec = len > 0.0f;
    for (size_t k = 0; k < 3; ++k) {
      wd.light.spec_dir[k] = len > 0.0f ? to_light[k] / len : 0.0f;
      wd.light.spec_eye[k] = cam_.eye[k];
    }
    assets::Vec3 offset = point.pos, deg{0, 0, 0};
    if (mount.used)
      for (size_t k = 0; k < 3; ++k) {
        offset[k] += mount.pos[k];
        deg[k] = mount.rot[k] * (360.0f / 4096.0f);
      }
    const render::Mat4 weapon_mat = palette_[point.joint] * render::mat4_from_rt(assets::rotation_zyx(deg), offset);
    wd.palette = std::span<const render::Mat4>(&weapon_mat, 1);
    meshes_->draw(*model.weapon->gpu, wd);
  }
}

}  // namespace oc::game
