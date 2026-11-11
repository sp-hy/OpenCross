// BattleView: camera and effect anchor points (reference points, anchor frames, model scale) and the table audit.
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>


namespace oc::game {

using namespace bv;

// -------------------------------------------------------------------------------------------------------------- camera
// The reference point of the pack's pseudo-actor model (slot A, see anchor_frame case 8): the same attach rule as a unit's (see unit_ref_point),
// on its own position / pose / facing.
bool BattleView::pa_ref_point(int attach, std::array<float, 3>& point, float& yaw) const {
  if ((!pa_.on && !pa_.registered) || !pa_.pose || !pa_.asset) return false;
  yaw = std::atan2(-pa_.face_x, -pa_.face_z);
  point = {pa_.x, attach == 0x22 ? 0.0f : pa_.y, pa_.z};
  const float height = static_cast<float>(pa_.asset->extent[1]);
  if (attach == 1) {
    point[1] += height;
  } else if (attach == 0x23) {
    point[1] += height * 0.5f;
  } else if (attach >= 2 && attach != 0x22) {
    float mx = 0, my = 0, mz = 0;
    // Model space (y down) -> world: yaw about Y, y mirrored.
    if (pa_.pose->attach_or_root(attach - 2, 1.0f, mx, my, mz)) {
      mx *= pa_.scale[0], my *= pa_.scale[1], mz *= pa_.scale[2];
      const float cs = std::cos(yaw), sn = std::sin(yaw);
      point = {pa_.x + mx * cs + mz * sn, pa_.y - my, pa_.z - mx * sn + mz * cs};
    }
  }
  return true;
}

// The reference point of a unit for attach code `attach`: 0 = ground position, 1 = position + the model height word, 0x22 = ground (y 0),
// 0x23 = position + height / 2, >= 2 otherwise = attach point `attach - 2` of the animated model (points 15 / 16 / 17 = weapon / hit point / ...).
// The frame rotation is only the unit yaw. An all-zero attach record is joint 0's origin = the model position (the Bomber's point 9 = joint 0 =
// its position; a 55 % height fallback would put an anchor 284 units too high). Only a pose without that joint keeps the fallback.
bool BattleView::unit_ref_point(int unit, int attach, std::array<float, 3>& point, float& yaw) const {
  if (unit < 0 || unit >= battle::kUnits || !bunit(unit).present) return false;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  // The model's own yaw (forward = local -Z), as the model matrix in draw(); a model an overlay steers has the yaw the overlay gave it.
  yaw = std::atan2(-visual.face_x, -visual.face_z);
  if (const effects::FxModels* models = fx_ ? fx_->bespoke_models() : nullptr; models && static_cast<size_t>(unit) < models->units.size() && models->units[static_cast<size_t>(unit)].steer)
    yaw = static_cast<float>(models->units[static_cast<size_t>(unit)].rot[1] & 0xFFF) * (2.0f * kPi / 4096.0f);
  // The model block position: the home / script position, or where an overlay put it.
  const std::array<float, 3> world = unit_world(unit, 1.0f);
  point = {world[0], attach == 0x22 ? 0.0f : world[1], world[2]};
  if (attach == 1) {
    point[1] += visual.height;
  } else if (attach == 0x23) {
    point[1] += visual.height * 0.5f;
  } else if (attach >= 2 && attach != 0x22) {
    float mx = 0, my = 0, mz = 0;
    // Model space (y down) -> world: yaw about Y, y mirrored.
    if (visual.has_model && visual.pose.attach_now(attach - 2, 1.0f, mx, my, mz, true)) {
      // A hidden model skips its joint pass: the joint matrices carry the placement of the last visible pass, so an attach-point frame stays
      // where the model stood then (the Bulbs' q001 camera stays 1024 units under the live position until the sprout un-hides).
      const bool held = visual.pose.held();
      const float point_yaw = held ? std::atan2(-visual.hold_fx, -visual.hold_fz) : yaw;
      const float cs = std::cos(point_yaw), sn = std::sin(point_yaw);
      const float bx = held ? visual.hold_x : world[0], by = held ? visual.hold_y : world[1], bz = held ? visual.hold_z : world[2];
      point = {bx + mx * cs + mz * sn, by - my, bz - mx * sn + mz * cs};
    } else {
      point[1] += visual.height * 0.55f;
    }
  }
  return true;
}

CamFrame BattleView::anchor_frame(int type, int attach) const {
  CamFrame frame;
  auto unit_point = [&](int unit, std::array<float, 3>& point, float& yaw) { return unit_ref_point(unit, attach, point, yaw); };
  auto unit_frame = [&](int unit) {
    std::array<float, 3> point{};
    float yaw = 0;
    if (!unit_point(unit, point, yaw)) return;
    frame.valid = true;
    frame.x = point[0];
    frame.y = point[1];
    frame.z = point[2];
    frame.yaw = yaw;
    // Shots with flag 0x20 scale the frame by the model's scale.
    frame.mscale = model_scale(unit);
  };
  // Types 3 / 4 / 5 / 0x16 / 0x17: the midpoint of the bounding box of the points ((max + min) / 2 per axis, integer division), yaw = the
  // arithmetic mean of the 12-bit model yaws / n; type 5 uses yaw 0. `home`: the units' home positions instead of their attach points.
  auto centroid = [&](const std::vector<int>& units, bool mean_yaw = true, bool home = false) {
    std::array<int, 3> lo{32767, 32767, 32767}, hi{-32767, -32767, -32767};
    int ang_sum = 0, count = 0;
    for (int unit : units) {
      std::array<float, 3> point{};
      float yaw = 0;
      if (!unit_point(unit, point, yaw)) continue;
      if (home) point = {vis_[static_cast<size_t>(unit)].home_x, 0.0f, vis_[static_cast<size_t>(unit)].home_z};
      for (size_t k = 0; k < 3; ++k) {
        // PS1 y is down.
        const int coord = static_cast<int>(k == 1 ? -point[k] : point[k]);
        lo[k] = std::min(lo[k], coord);
        hi[k] = std::max(hi[k], coord);
      }
      ang_sum += static_cast<int>(std::lround(yaw / (2.0f * kPi) * 4096.0f)) & 0xFFF;
      ++count;
    }
    if (count == 0) return;
    frame.valid = true;
    frame.x = static_cast<float>((hi[0] + lo[0]) / 2);
    frame.y = -static_cast<float>((hi[1] + lo[1]) / 2);
    frame.z = static_cast<float>((hi[2] + lo[2]) / 2);
    frame.yaw = mean_yaw ? static_cast<float>(ang_sum / count) * (2.0f * kPi / 4096.0f) : 0.0f;
  };
  // The two lists: the acting list and the target list, both rewritten by the command setup (units in ascending slot order) and kept until the
  // next one: the running action's actor / targets, the script's 7A / 86 unit, the target selection's cursor, else the last ones. A missing
  // entry is the world frame (an identity matrix at the origin).
  // The element menu makes the active PC the acting list.
  if (ui_ == Ui::Element && active_pc_ >= 0 && act_.phase == Act::Phase::None) {
    anchor_actors_ = {active_pc_};
    anchor_actors_set_ = true;
  // Op 0x7A / 0x86 rewrote the acting list after the command setup (reset by begin_group): it wins over the last action's list.
  } else if (script_actor_ >= 0) {
    anchor_actors_ = {script_actor_};
    anchor_actors_set_ = true;
    if (act_.phase != Act::Phase::None || act_.actor >= 0) {
      anchor_targets_ = act_.targets;
      anchor_targets_set_ = true;
    }
  } else if (act_.phase != Act::Phase::None || act_.actor >= 0) {
    // The acting list in list order (henshin [4, 3]).
    anchor_actors_ = act_.casters.empty() ? std::vector<int>{act_.actor} : act_.casters;
    anchor_targets_ = act_.targets;
    anchor_actors_set_ = anchor_targets_set_ = true;
  } else if (ui_ == Ui::Target && active_pc_ >= 0 && !targets_.empty()) {
    anchor_actors_ = {active_pc_};
    anchor_targets_ = {targets_[static_cast<size_t>(std::clamp(target_cursor_, 0, static_cast<int>(targets_.size()) - 1))]};
    anchor_actors_set_ = anchor_targets_set_ = true;
  }
  std::vector<int> actors = anchor_actors_, tgts = anchor_targets_;
  // The target list is written in ascending slot order; the acting list keeps the script's 8D slot order.
  std::sort(tgts.begin(), tgts.end());
  tgts.erase(std::unique(tgts.begin(), tgts.end()), tgts.end());
  // A running overlay that replaced the list getters (etrap swaps the two lists) frames the shots by its lists too.
  const effects::FxModels* models = fx_ ? fx_->bespoke_models() : nullptr;
  const bool targets_overridden = models && models->target_override;
  if (models && models->caster_override) actors = models->caster_list;
  if (targets_overridden) tgts = models->target_list;
  const int tgt = tgts.empty() ? -1 : tgts.front();
  auto world = [&] {
    frame.valid = true;
    frame.x = frame.y = frame.z = frame.yaw = 0.0f;
  };
  // Pseudo-actor slot A / B: the particle an effect registered with op 0x3F / 0x40; its frame is the particle's position and the yaw
  // -atan2(vel z, vel x); a sprite's model bound there gives its own yaw and keeps its last place after the particle is freed. No such particle: no frame
  // (the identity matrix at the origin).
  auto slot_frame = [&](int slot) {
    effects::Vec3 pos{}, vel{};
    if (!fx_ || !fx_->slot_object(slot, pos, vel)) {
      // Slot A also holds a MODEL: the pack loader registers every type-0x0B Mdl named 'act0' as slot A when the pack loads; case 8 then
      // frames that model like a unit. (Pip's evolution base2m.prd: the act0 model stands at Pip's position from the pack load on; cst3 / cst4
      // (ref 8) frame it.)
      std::array<float, 3> ref{};
      float yaw = 0;
      if (slot == 0 && pa_ref_point(attach, ref, yaw)) {
        frame.valid = true;
        frame.x = ref[0], frame.y = ref[1], frame.z = ref[2];
        frame.yaw = yaw;
        return;
      }
      return world();
    }
    frame.valid = true;
    frame.x = pos[0];
    // PS1 world y is down.
    frame.y = -pos[1];
    frame.z = pos[2];
    int model_yaw = 0;
    frame.yaw = fx_->slot_model_yaw(slot, model_yaw) ? static_cast<float>(model_yaw) * (2.0f * kPi / 4096.0f) : -std::atan2(vel[2], vel[0]);
  };
  auto nth = [&](const std::vector<int>& list, int index) { return index >= 0 && index < static_cast<int>(list.size()) ? list[static_cast<size_t>(index)] : -1; };
  auto frame_of = [&](int unit) {
    if (unit < 0) return world();
    unit_frame(unit);
  };
  auto centroid_of = [&](const std::vector<int>& list, bool ground, bool home = false) {
    if (list.empty()) return world();
    centroid(list, true, home);
    if (ground) frame.y = 0.0f;
  };
  switch (type) {
    case 1: case 9: frame_of(nth(actors, 0)); break;
    case 10: case 11: frame_of(nth(actors, type - 9)); break;
    // Type 8 / 0xC: the pseudo-actor registered by the pack's ops 0x3F / 0x40.
    case 8: case 12: slot_frame(type == 8 ? 0 : 1); break;
    case 2: case 13: frame_of(nth(tgts, 0)); break;
    case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: frame_of(nth(tgts, type - 13)); break;
    case 3: centroid_of(actors, false); break;
    case 4: centroid_of(tgts, false); break;
    // The targets' ground centroid, y 0: 0x16 of their attach points, 0x17 of their home positions.
    case 22: case 23: centroid_of(tgts, true, type == 23); break;
    case 5: {
      std::vector<int> all;
      for (int i = 0; i < battle::kUnits; ++i) all.push_back(i);
      // Yaw 0.
      centroid(all, false);
      break;
    }
    // A side frame: origin (-0x800, 0, 0) turned 180 degrees while the unit stands at x <= 0, else (+0x800, 0, 0).
    case 25: case 26: {
      const int unit = type == 25 ? nth(actors, 0) : nth(tgts, 0);
      if (unit < 0 || unit >= battle::kUnits) break;
      frame.valid = true;
      frame.y = frame.z = 0.0f;
      const bool left = vis_[static_cast<size_t>(unit)].x < 1.0f;
      frame.x = left ? -2048.0f : 2048.0f;
      frame.yaw = left ? kPi : 0.0f;
      break;
    }
    case 7: case 24: world(); break;
    // 0, 6 and beyond: no frame (the matrix is left alone).
    default: break;
  }
  if (battle_audit()) {
    const bool actor_ref = type == 1 || type == 3 || (type >= 8 && type <= 11) || type == 25, tgt_ref = type == 2 || type == 4 || type == 12 || (type >= 13 && type <= 23) || type == 26;
    if (!frame.valid && type >= 1 && type != 6) {
      battle_audit_note(std::format("anchor type {} attach {} gives no frame (phase {})", type, attach, static_cast<int>(act_.phase)));
    // Before the first command setup the lists are in their initial state: empty, world frame (the Radius tutorial's opening shot ev01 asks type 2 there).
    } else if (actor_ref && actors.empty() && anchor_actors_set_) {
      battle_audit_note(std::format("anchor type {} (acting list) asked while the list is empty: world frame", type));
    } else if (tgt_ref && tgts.empty() && anchor_targets_set_) {
      battle_audit_note(std::format("anchor type {} (target list) asked while the list is empty: world frame", type));
    } else if (tgt_ref && !targets_overridden && act_.phase != Act::Phase::None && !act_.ev.empty() && tgt >= 0) {
      bool known = false;
      for (const battle::ActionEvent& event : act_.ev) known = known || event.target == tgt;
      if (!known) battle_audit_note(std::format("anchor type {} on unit {} which is no target of action {}", type, tgt, act_.ev.front().action));
    }
  }
  return frame;
}

// The model scale = clamp(((ey << 12) / 0x42B - 0x1000) / 2 + 0x1000, 0x300, 0x8000 past 0x10000), ey = the model's S1 height extent
// (the same value the effect player gets as FxUnit::fx_scale in battle_view_fx.cpp).
float BattleView::model_scale(int unit) const {
  if (unit < 0 || unit >= battle::kUnits) return 1.0f;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!visual.has_model || !visual.model) return 1.0f;
  const int scale = ((visual.model->asset.extent[1] << 12) / 0x42B - 0x1000) / 2 + 0x1000;
  return static_cast<float>(scale > 0x10000 ? 0x8000 : (scale < 0x300 ? 0x300 : scale)) / 4096.0f;
}

// OC_BATTLE_AUDIT=1: checks that need no hook inside a phase.
void BattleView::audit_tables() {
  audit_.tables_done = true;
  // =2: every distinct (target byte, mode) pair with its first element.
  if (const char* mode = std::getenv("OC_BATTLE_AUDIT"); mode && mode[0] == '2') {
    std::map<std::pair<int, int>, std::pair<int, int>> seen;
    for (size_t id = 1; id < tables_->elements.size(); ++id) {
      const data::ElemRec& elem = tables_->elements[id];
      if (elem.empty()) continue;
      auto& entry = seen[{elem.b(4), elem.mode()}];
      entry.first = entry.second == 0 ? static_cast<int>(id) : entry.first;
      ++entry.second;
    }
    for (const auto& [key, value] : seen)
      battle_audit_note(std::format("target byte {:#04x} mode {:2}: {} elements, first {} '{}'", key.first, key.second, value.second, value.first, tables_->element_name(value.first)));
  }
}

void BattleView::audit_tick() {
  if (!audit_.tables_done) audit_tables();
  // A queued group that cannot start (script message / intro holding it) for ever.
  if (audit_.group_stalled(act_.phase == Act::Phase::None && !queue_.empty() && (ui_ == Ui::Intro || script_gated(queue_.front().front()))))
    battle_audit_note(std::format("{} queued group(s) wait 900 ticks (ui {} script stream {} message window {})", queue_.size(), static_cast<int>(ui_), script_stream_.size(), smsg_.open));
  // A phase that sits on one value (ticks reset by start_hit etc. are fine; this catches a stuck state machine).
  if (audit_.ui_wait_stuck(act_.phase == Act::Phase::None && ui_ == Ui::Wait && queue_.empty() && pending_.empty() && !engage_arrive_ && finish_ == Finish::None && !script_busy() && !battle_->menu_locked()))
    battle_audit_note("UI waits for an action that never came (ui Wait, no group, no pending reaction) for 300 ticks");
}

}  // namespace oc::game
