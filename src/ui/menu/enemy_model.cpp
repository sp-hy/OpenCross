#include "ui/menu/enemy_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>

#include "core/log.h"
#include "game/battle/battle_model_cache.h"
#include "game/battle/battle_seq.h"
#include "game/battle/model_table.h"
#include "render/mat4.h"

namespace oc::ui {

namespace {
// The stand-by clip at the battle's rate (half a keyframe per model tick), a model tick being 1/30 s.
constexpr int kClip = 1, kSpeed = 128;
constexpr float kTicksPerSecond = 30.0f;
// The turn per model tick (4096 = a turn), as the Status screen's turning character.
constexpr float kTurn = 8.0f;
// The projection-plane distance (PS1 px) and the share of the box the creature may reach.
constexpr float kProjection = 256.0f, kFill = 0.95f;
constexpr float kPi = 3.14159265358979323846f;
// At most this many model ticks are caught up in one draw, and this many models are kept.
constexpr int kMaxSteps = 8;
constexpr size_t kKept = 8;

// Every skinned pool vertex of the meshes of `asset` not set in `hidden`, posed by `palette` (joints past its end are skipped).
void add_points(const assets::ModelAsset& asset, std::span<const render::Mat4> palette, u32 hidden, std::vector<std::array<float, 3>>& out) {
  for (size_t index = 0; index < asset.meshes.size(); ++index) {
    if (index < 32 && ((hidden >> index) & 1)) continue;
    for (const assets::SkinVertex& vertex : asset.meshes[index].pool) {
      std::array<float, 3> point{0, 0, 0};
      for (size_t k = 0; k < 2; ++k) {
        if (vertex.weight[k] == 0.0f || vertex.joint[k] >= palette.size()) continue;
        const render::Mat4& joint = palette[vertex.joint[k]];
        const assets::Vec3& local = vertex.local[k];
        for (int axis = 0; axis < 3; ++axis)
          point[static_cast<size_t>(axis)] += vertex.weight[k] * (joint.at(axis, 0) * local[0] + joint.at(axis, 1) * local[1] + joint.at(axis, 2) * local[2] + joint.at(axis, 3));
      }
      out.push_back(point);
    }
  }
}
}  // namespace

bool EnemyModelLibrary::weapon_matrix(const Entry& held, size_t weapon_slot, const std::vector<render::Mat4>& palette, render::Mat4& out) {
  const assets::ModelAsset& asset = held.body->asset;
  const assets::MountPoint& mount = asset.attach.mounts[weapon_slot];
  const int attach = weapon_slot == 0 ? held.weapon_attach : mount.used ? static_cast<int>(mount.attach) : -1;
  if (attach < 0 || ((held.weapon_hide >> weapon_slot) & 1)) return false;
  const assets::AttachPoint& point = asset.attach.points[static_cast<size_t>(attach)];
  if (point.joint >= palette.size()) return false;
  assets::Vec3 offset = point.pos, degrees{0, 0, 0};
  if (mount.used)
    for (size_t k = 0; k < 3; ++k) offset[k] += mount.pos[k], degrees[k] = mount.rot[k] * (360.0f / 4096.0f);
  out = palette[point.joint] * render::mat4_from_rt(assets::rotation_zyx(degrees), offset);
  return true;
}

EnemyModelLibrary::Entry* EnemyModelLibrary::entry(int model, int weapon_part) {
  const auto found = std::find_if(cache_.begin(), cache_.end(),
                                  [&](const std::unique_ptr<Entry>& held) { return held->model == model && held->weapon_part == weapon_part; });
  if (found != cache_.end()) {
    cache_.splice(cache_.begin(), cache_, found);
    return cache_.front().get();
  }
  if (model < 0 || static_cast<size_t>(model) >= game::kBattleModels.size() || game::kBattleModels[static_cast<size_t>(model)].empty()) return nullptr;
  std::string err;
  auto asset = game::ModelCache::read_model(vfs_, std::string(game::kBattleModels[static_cast<size_t>(model)]), &err);
  if (!asset) {
    log::warn("bestiary page: model {}: {}", game::kBattleModels[static_cast<size_t>(model)], err);
    return nullptr;
  }
  auto held = std::make_unique<Entry>();
  held->model = model;
  held->weapon_part = weapon_part;
  held->body = game::Actor::make_resolved_model(meshes_, std::move(*asset));
  const std::string stem(game::kBattleModels[static_cast<size_t>(model)]);
  // The spawn script's first tick (S5 entry 8 up to its first wait): the attached-object hide words and the parts it hides / shows.
  game::bseq::Table spawn;
  if (const auto obj = vfs_.read("cdrom/" + stem + ".obj"); obj && game::bseq::load_model_table(*obj, spawn)) {
    held->weapon_hide = spawn.initial_weapon_hide();
    game::bseq::Vm vm;
    if (vm.start(&spawn, 8, 0)) {
      game::bseq::Ctx ctx;
      ctx.rand = [] { return 0; };
      std::vector<game::bseq::Event> events;
      const game::bseq::Vm::Shared shared = game::bseq::Vm::shared();
      vm.tick(ctx, events);
      game::bseq::Vm::restore(shared);
      for (const game::bseq::Event& event : events) {
        if (event.kind != game::bseq::Event::Kind::PartShow || event.b < 0 || event.b >= 32) continue;
        const u32 bit = 1u << event.b;
        if (event.a) held->part_show |= bit, held->part_hide &= ~bit;
        else held->part_hide |= bit, held->part_show &= ~bit;
      }
    }
  }
  if (const std::string bin = game::ModelCache::weapon_bin(model); weapon_part >= 0 && !bin.empty() && held->body && (held->weapon_hide & 3) != 3) {
    held->weapon = game::ModelCache::build_weapon(vfs_, meshes_, bin, weapon_part);
    const auto& points = held->body->asset.attach.points;
    const assets::MountPoint& mount = held->body->asset.attach.mounts[0];
    if (held->weapon) held->weapon_attach = mount.used ? mount.attach : points[0x12].used ? 0x12 : points[7].used ? 7 : 15;
  }
  held->pose.set_model(&held->body->asset);
  held->pose.set_speed(kSpeed);
  held->pose.set_loop(true);
  held->pose.play(kClip, 0);
  // The bounds over the whole stand-by clip, every keyframe and the half keyframes between them: every skinned pool vertex of the parts
  // drawn and of the weapon on the slots it is drawn on (model y points down).
  render::MeshPipeline::DrawParams parts;
  game::model_shading_entries(held->body->asset, parts);
  const u32 hidden = (parts.hidden_meshes | held->part_hide) & ~held->part_show;
  std::vector<std::array<float, 3>> points;
  const int clip = held->pose.clip_for(kClip), frames = held->pose.frames_of(kClip);
  for (int half = 0; half < 2 * frames; ++half) {
    held->pose.palette_at(clip, 0.5f * static_cast<float>(half), palette_);
    add_points(held->body->asset, palette_, hidden, points);
    render::Mat4 weapon_mat;
    for (size_t weapon_slot = 0; weapon_slot < 2 && held->weapon; ++weapon_slot)
      if (weapon_matrix(*held, weapon_slot, palette_, weapon_mat)) add_points(held->weapon->asset, std::span<const render::Mat4>(&weapon_mat, 1), 0, points);
  }
  if (!points.empty()) {
    float low[3], high[3];
    std::fill(std::begin(low), std::end(low), std::numeric_limits<float>::max());
    std::fill(std::begin(high), std::end(high), std::numeric_limits<float>::lowest());
    for (const auto& point : points)
      for (size_t axis = 0; axis < 3; ++axis) low[axis] = std::min(low[axis], point[axis]), high[axis] = std::max(high[axis], point[axis]);
    for (size_t axis = 0; axis < 3; ++axis) held->centre[axis] = 0.5f * (low[axis] + high[axis]);
    // Each point as its height off the centre and its distance from the turning axis; only the points no other point exceeds in both
    // are kept, highest first.
    std::vector<std::array<float, 2>> reach;
    reach.reserve(points.size());
    for (const auto& point : points)
      reach.push_back({std::abs(point[1] - held->centre[1]), std::hypot(point[0] - held->centre[0], point[2] - held->centre[2])});
    std::sort(reach.begin(), reach.end(), [](const auto& first, const auto& second) { return first[0] != second[0] ? first[0] > second[0] : first[1] > second[1]; });
    for (const auto& point : reach) {
      if (!held->reach.empty() && point[1] <= held->reach.back()[1]) continue;
      held->reach.push_back(point);
    }
  }
  cache_.push_front(std::move(held));
  if (cache_.size() > kKept) cache_.pop_back();
  return cache_.front().get();
}

void EnemyModelLibrary::draw(const render::Frame& frame, float vw, float vh, const EnemyModelSlot& slot) {
  // A texture mod switched on / off: the models are built again.
  if (const u32 generation = vfs_.generation(); generation != generation_) {
    generation_ = generation;
    cache_.clear();
  }
  Entry* shown = entry(slot.model, slot.weapon_part);
  if (!shown || !shown->body || !shown->body->gpu) return;
  if (slot.model != shown_ || slot.weapon_part != shown_weapon_) {
    shown_ = slot.model;
    shown_weapon_ = slot.weapon_part;
    shown->pose.play(kClip, 0);
    accum_ = 0;
  }
  accum_ += std::max(slot.dt, 0.0f) * kTicksPerSecond;
  for (int step = 0; accum_ >= 1.0f && step < kMaxSteps; ++step, accum_ -= 1.0f) {
    shown->pose.tick();
    yaw_ = std::fmod(yaw_ + kTurn, 4096.0f);
  }
  accum_ = std::min(accum_, 1.0f);
  shown->pose.palette(accum_, palette_);
  // The creature is square in window pixels: one projection scale on both axes, the PS1 pixel's height.
  const float scale_px = slot.unit_y * kProjection;
  const float distance = turning_fit_distance(shown->reach, scale_px, 0.5f * kFill * slot.box_w * slot.unit_x, 0.5f * kFill * slot.box_h * slot.unit_y);
  const float centre_x = slot.box_x + 0.5f * slot.box_w, centre_y = slot.box_y + 0.5f * slot.box_h;
  render::Mat4 proj;
  proj.at(0, 0) = 2.0f * scale_px / vw;
  proj.at(0, 2) = 2.0f * (slot.origin_x + slot.unit_x * centre_x) / vw - 1.0f;
  proj.at(1, 1) = 2.0f * scale_px / vh;
  proj.at(1, 2) = 2.0f * (slot.origin_y + slot.unit_y * centre_y) / vh - 1.0f;
  proj.at(2, 2) = 0.5f;
  proj.at(3, 2) = 1.0f;
  proj.at(3, 3) = 0.0f;
  const float turn = 2.0f * kPi * (yaw_ + kTurn * accum_) / 4096.0f;
  const render::Mat4 view = render::mat4_translation(0, -shown->centre[1], distance) * render::mat4_rotation_y(turn);
  render::MeshPipeline::DrawParams dp;
  dp.view_proj = proj * view;
  dp.view = view;
  dp.model = render::mat4_translation(-shown->centre[0], 0, -shown->centre[2]);
  dp.palette = palette_;
  dp.linear = dp.hd = slot.hd;
  const assets::ModelAsset& asset = shown->body->asset;
  game::model_ot_faces(asset, dp);
  game::model_shading_entries(asset, dp);
  dp.hidden_meshes = (dp.hidden_meshes | shown->part_hide) & ~shown->part_show;
  if (slot.silhouette) std::fill(std::begin(dp.tint), std::end(dp.tint), 0.0f);
  // The menu is drawn over the field in the same pass: start from a clean depth buffer.
  frame.clear_depth();
  meshes_.begin(frame, frame.full());
  meshes_.draw(*shown->body->gpu, dp);
  // Both attached-object slots carry the weapon at their attach point's joint, moved and turned by their mount record; slot 1 only with a
  // mount record of its own.
  for (size_t weapon_slot = 0; weapon_slot < 2 && shown->weapon && shown->weapon->gpu; ++weapon_slot) {
    render::Mat4 weapon_mat;
    if (!weapon_matrix(*shown, weapon_slot, palette_, weapon_mat)) continue;
    render::MeshPipeline::DrawParams wd = dp;
    wd.part_mask = wd.part_semi = 0;
    wd.near_meshes = wd.env_meshes = 0;
    wd.palette = std::span<const render::Mat4>(&weapon_mat, 1);
    meshes_.draw(*shown->weapon->gpu, wd);
  }
  meshes_.end();
}

}  // namespace oc::ui
