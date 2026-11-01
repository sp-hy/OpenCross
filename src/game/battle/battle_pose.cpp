#include "game/battle/battle_pose.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"

#include "game/anim_arc.h"

namespace oc::game {

namespace {

assets::Vec3 sample_keys(const std::vector<assets::Key3>& keys, float frame, bool angles, bool loop) {
  if (keys.empty()) return {};
  const size_t key_count = keys.size();
  if (key_count == 1) return keys[0].v;
  float wrapped = frame;
  if (loop) {
    const float count_f = static_cast<float>(key_count);
    wrapped = std::fmod(std::fmod(frame, count_f) + count_f, count_f);
  } else {
    wrapped = std::clamp(frame, 0.0f, static_cast<float>(key_count - 1));
  }
  const size_t k0 = std::min(static_cast<size_t>(wrapped), key_count - 1);
  const size_t k1 = loop ? (k0 + 1) % key_count : std::min(k0 + 1, key_count - 1);
  const float frac = wrapped - static_cast<float>(k0);
  assets::Vec3 result;
  for (size_t i = 0; i < 3; ++i) {
    float from = keys[k0].v[i];
    float to = keys[k1].v[i];
    // The long way round between 180 and 270 degrees, as in the field.
    if (angles) retail_arc(from, to);
    result[i] = from + (to - from) * frac;
  }
  return result;
}

}  // namespace

void UnitPose::set_model(const assets::ModelAsset* asset) {
  asset_ = asset;
  ext_clips_ = nullptr;
  id_ = clip_ = from_ = -1;
  t_prev_ = t_cur_ = f_prev_ = f_cur_ = 0;
  w_prev_ = w_cur_ = 1;
}

const assets::AnimationClip* UnitPose::clip_at(int clip) const {
  if (clip < 0) return nullptr;
  if (clip >= kExtBase) {
    const size_t ext_index = static_cast<size_t>(clip - kExtBase);
    return ext_clips_ && ext_index < ext_clips_->size() ? &(*ext_clips_)[ext_index] : nullptr;
  }
  return asset_ && static_cast<size_t>(clip) < asset_->clips.size() ? &asset_->clips[static_cast<size_t>(clip)] : nullptr;
}

int UnitPose::clip_for(int id) const {
  if (!asset_) return -1;
  const bool ext = id >= 0x80;
  const u32 idx = static_cast<u32>(ext ? id - 0x80 : id);
  // The pack's clips replace at0.
  if (ext && ext_clips_) return idx < ext_clips_->size() && (*ext_clips_)[idx].frame_count > 0 ? kExtBase + static_cast<int>(idx) : -1;
  for (size_t i = 0; i < asset_->clips.size(); ++i) {
    const auto& clip = asset_->clips[i];
    if (clip.index == idx && clip.source == (ext ? "at0" : "s4")) return static_cast<int>(i);
  }
  return -1;
}

int UnitPose::frames_of(int id) const {
  const assets::AnimationClip* clip = clip_at(clip_for(id));
  return clip ? std::max<int>(1, static_cast<int>(clip->frame_count)) : 1;
}

float UnitPose::length(int clip) const {
  const assets::AnimationClip* found = clip_at(clip);
  return found ? static_cast<float>(found->frame_count) : 0.0f;
}

void UnitPose::play(int id, int blend, bool run_source, int mode) {
  const int clip = clip_for(id);
  // A missing clip keeps the current pose.
  if (clip < 0) return;
  play_clip(clip, blend, run_source, mode);
  id_ = id;
}

void UnitPose::play_clip(int clip, int blend, bool run_source, int mode) {
  if (!asset_ || !clip_at(clip)) return;
  // Every SetAnim rewrites the mode, also a cut.
  mode_ = mode & 3;
  if (clip_ >= 0 && blend > 0) {
    from_ = clip_;
    f_prev_ = t_prev_;
    f_cur_ = t_cur_;
    w_prev_ = w_cur_ = 0;
    w_step_ = 1.0f / static_cast<float>(blend);
    run_source_ = run_source;
  } else {
    from_ = -1;
    w_prev_ = w_cur_ = 1;
  }
  clip_ = clip;
  id_ = -1;
  t_prev_ = t_cur_ = 0;
}

void UnitPose::tick() {
  auto adv = [&](int clip, float& prev, float& cur, bool loop) {
    prev = cur;
    if (clip < 0) return;
    const float len = length(clip);
    cur += speed_;
    if (len <= 1.0f) {
      cur = 0;
      return;
    }
    if (loop) {
      if (cur >= len) {
        cur -= len;
        prev -= len;
      }
    } else if (cur > len - 1.0f) {
      cur = len - 1.0f;
      prev = std::min(prev, cur);
    }
  };
  adv(clip_, t_prev_, t_cur_, loop_);
  if (run_source_) adv(from_, f_prev_, f_cur_, true);
  // Else the source clip stays frozen at the keyframe it had at the switch.
  else f_prev_ = f_cur_;
  w_prev_ = w_cur_;
  if (w_cur_ < 1.0f) {
    // Eased blend: w += (0x1000 - w) >> 2 per tick, w = 0x1000 once less than 4 / 4096 is left.
    if (mode_ == 1) {
      const float left = 1.0f - w_cur_;
      w_cur_ = left * 4096.0f < 4.0f ? 1.0f : w_cur_ + std::floor(std::floor(left * 4096.0f) / 4.0f) / 4096.0f;
    } else {
      w_cur_ = std::min(1.0f, w_cur_ + w_step_);
    }
  }
  if (w_cur_ >= 1.0f) from_ = -1;
  tick_elements();
}

void UnitPose::tick_elements() {
  // The element tasks (rotation, scale): one step per tick, exact end.
  for (JointOv& joint_ov : ov_) {
    if (joint_ov.rleft > 0) {
      for (size_t i = 0; i < 3; ++i) joint_ov.rot[i] += joint_ov.rstep[i];
      if (--joint_ov.rleft == 0) {
        joint_ov.rot = joint_ov.rto;
        if (joint_ov.resume) joint_ov.frozen = false, joint_ov.resume = false, joint_ov.rate = {0, 0, 0};
      }
    }
    for (size_t i = 0; i < 3; ++i) joint_ov.rot[i] += joint_ov.rate[i];
    if (joint_ov.sleft > 0) {
      for (size_t i = 0; i < 3; ++i) joint_ov.scale[i] += joint_ov.sstep[i];
      if (--joint_ov.sleft == 0) joint_ov.scale = joint_ov.sto;
    }
  }
  // Dev: OC_FXDBG_POSE=<joint> (default 0) logs that joint's frozen rotation each tick.
  if (static const char* env_joint = std::getenv("OC_FXDBG_POSE"); env_joint && !ov_.empty())
    if (const size_t joint = static_cast<size_t>(std::max(0, std::atoi(env_joint))); joint < ov_.size() && ov_[joint].frozen)
      log::debug("fxdbg: joint {} frozen rot {:.1f} {:.1f} {:.1f}", joint, ov_[joint].rot[0], ov_[joint].rot[1], ov_[joint].rot[2]);
}

UnitPose::JointOv* UnitPose::ov(int joint, bool create) {
  if (!asset_ || joint < 0 || joint >= static_cast<int>(asset_->skeleton.joints.size())) return nullptr;
  if (ov_.empty()) {
    if (!create) return nullptr;
    ov_.resize(asset_->skeleton.joints.size());
  }
  return &ov_[static_cast<size_t>(joint)];
}

void UnitPose::joint_motion(int joint, bool on) {
  JointOv* joint_ov = ov(joint, !on);
  if (!joint_ov) return;
  if (on) {
    joint_ov->frozen = false;
    joint_ov->rate = {0, 0, 0};
    joint_ov->rleft = 0;
    return;
  }
  // The element keeps the value the clip gave it at this tick: the clip's delta over the rest rotation (the matrix is then rest * element, see palette).
  if (!joint_ov->frozen) {
    std::vector<Local> loc;
    sample(clip_, clip_ >= 0 ? t_cur_ : 0.0f, loc);
    if (static_cast<size_t>(joint) < loc.size())
      for (size_t i = 0; i < 3; ++i) joint_ov->rot[i] = loc[static_cast<size_t>(joint)].rot[i] - asset_->skeleton.joints[static_cast<size_t>(joint)].rest_rot_deg[i];
    joint_ov->frozen = true;
  }
}

void UnitPose::joint_rot_to(int joint, const assets::Vec3& deg, int ticks) {
  JointOv* joint_ov = ov(joint, true);
  if (!joint_ov) return;
  if (ticks < 1) {
    joint_ov->rot = deg;
    joint_ov->rleft = 0;
    return;
  }
  // Each axis takes the short way round.
  for (size_t i = 0; i < 3; ++i) {
    float delta = std::fmod(deg[i] - joint_ov->rot[i], 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    joint_ov->rstep[i] = delta / static_cast<float>(ticks);
    joint_ov->rto[i] = joint_ov->rot[i] + delta;
  }
  joint_ov->rleft = ticks;
}

void UnitPose::joint_tilt(int joint, float z_deg, int ticks) {
  joint_motion(joint, false);
  JointOv* joint_ov = ov(joint, true);
  if (!joint_ov) return;
  // The element's z is saved after switching its motion off.
  joint_ov->saved_z = joint_ov->rot[2];
  joint_ov->resume = false;
  joint_rot_to(joint, {joint_ov->rot[0], joint_ov->rot[1], z_deg}, ticks);
}

void UnitPose::joint_tilt_back(int joint, int ticks) {
  JointOv* joint_ov = ov(joint, false);
  if (!joint_ov) return;
  joint_rot_to(joint, {joint_ov->rot[0], joint_ov->rot[1], joint_ov->saved_z}, ticks);
  // No ticks = z set and the motion flag cleared at once.
  if (ticks < 1) joint_ov->frozen = false, joint_ov->rate = {0, 0, 0};
  else joint_ov->resume = true;
}

void UnitPose::joint_rot_rate(int joint, const assets::Vec3& deg_per_tick) {
  if (JointOv* joint_ov = ov(joint, true)) joint_ov->rate = deg_per_tick, joint_ov->rleft = 0;
}

void UnitPose::joint_scale_to(int joint, const assets::Vec3& scale, int ticks) {
  JointOv* joint_ov = ov(joint, true);
  if (!joint_ov) return;
  if (ticks < 1) {
    joint_ov->scale = scale;
    joint_ov->sleft = 0;
    return;
  }
  for (size_t i = 0; i < 3; ++i) joint_ov->sstep[i] = (scale[i] - joint_ov->scale[i]) / static_cast<float>(ticks);
  joint_ov->sto = scale;
  joint_ov->sleft = ticks;
}

void UnitPose::joint_scale_stop(int joint) {
  if (JointOv* joint_ov = ov(joint, false)) joint_ov->sleft = 0;
}

bool UnitPose::at_end() const {
  if (clip_ < 0 || loop_) return false;
  return t_cur_ >= length(clip_) - 1.0f - 1e-3f;
}

void UnitPose::sample(int clip, float frame, std::vector<Local>& out) const {
  const auto& joints = asset_->skeleton.joints;
  out.resize(joints.size());
  const bool loop = loop_;
  const assets::AnimationClip* cl = clip_at(clip);
  for (size_t j = 0; j < joints.size(); ++j) {
    assets::Vec3 rot = joints[j].rest_rot_deg, trans = joints[j].rest_trans;
    if (cl && j < cl->tracks.size()) {
      const auto& tr = cl->tracks[j];
      if (!tr.rot.empty()) rot = sample_keys(tr.rot, frame, true, loop);
      if (!tr.trans.empty()) trans = sample_keys(tr.trans, frame, false, loop);
    }
    out[j] = {rot, trans};
  }
}

void UnitPose::hold(bool on) {
  if (on == held_) return;
  // palette() is the live pose while held_ is still false.
  if (on) palette(1.0f, held_pal_);
  else held_pal_.clear();
  held_ = on;
}

void UnitPose::palette(float alpha, std::vector<render::Mat4>& out) const {
  if (!asset_) {
    out.clear();
    return;
  }
  if (held_) {
    out = held_pal_;
    return;
  }
  const auto& joints = asset_->skeleton.joints;
  std::vector<Local> local;
  auto at = [&](int clip, float prev, float cur) {
    float time = prev + (cur - prev) * alpha;
    if (time < 0) time += length(clip);
    return time;
  };
  sample(clip_, clip_ >= 0 ? at(clip_, t_prev_, t_cur_) : 0.0f, local);
  const float weight = w_prev_ + (w_cur_ - w_prev_) * alpha;
  if (from_ >= 0 && weight < 1.0f) {
    std::vector<Local> old;
    sample(from_, at(from_, f_prev_, f_cur_), old);
    for (size_t j = 0; j < local.size(); ++j)
      for (size_t i = 0; i < 3; ++i) {
        float from = old[j].rot[i];
        float to = local[j].rot[i];
        // The clip blend takes the same long-way-round pre-pass.
        retail_arc(from, to);
        local[j].rot[i] = from + (to - from) * weight;
        local[j].trans[i] = old[j].trans[i] + (local[j].trans[i] - old[j].trans[i]) * weight;
      }
  }
  out.resize(joints.size());
  for (size_t j = 0; j < joints.size(); ++j) {
    render::Mat4 joint_mat;
    if (j < ov_.size()) {
      const JointOv& joint_ov = ov_[j];
      // A frozen joint: R = RotMatrixZYX(rest) * RotMatrixZYX(element); the element holds its own value, not rest + delta.
      if (joint_ov.frozen) {
        joint_mat = render::mat4_from_rt(assets::rotation_zyx(joints[j].rest_rot_deg), {0, 0, 0}) * render::mat4_from_rt(assets::rotation_zyx(joint_ov.rot), {0, 0, 0});
        for (int k = 0; k < 3; ++k) joint_mat.at(k, 3) = local[j].trans[static_cast<size_t>(k)];
      } else {
        joint_mat = render::mat4_from_rt(assets::rotation_zyx(local[j].rot), local[j].trans);
      }
      if (joint_ov.scale[0] != 1.0f || joint_ov.scale[1] != 1.0f || joint_ov.scale[2] != 1.0f) {
        render::Mat4 scale_mat;
        scale_mat.at(0, 0) = joint_ov.scale[0], scale_mat.at(1, 1) = joint_ov.scale[1], scale_mat.at(2, 2) = joint_ov.scale[2];
        joint_mat = joint_mat * scale_mat;
      }
    } else {
      joint_mat = render::mat4_from_rt(assets::rotation_zyx(local[j].rot), local[j].trans);
    }
    const int parent = joints[j].parent;
    out[j] = parent >= 0 ? out[static_cast<size_t>(parent)] * joint_mat : joint_mat;
  }
}

void UnitPose::palette_at(int clip, float frame, std::vector<render::Mat4>& out) const {
  if (!asset_) {
    out.clear();
    return;
  }
  if (!clip_at(clip)) clip = -1;
  if (clip >= 0) {
    const float len = length(clip);
    if (len > 1.0f) frame = std::fmod(frame, len);
    else frame = 0;
    if (frame < 0) frame += len;
  }
  const auto& joints = asset_->skeleton.joints;
  std::vector<Local> local;
  sample(clip, frame, local);
  out.resize(joints.size());
  for (size_t j = 0; j < joints.size(); ++j) {
    const render::Mat4 joint_mat = render::mat4_from_rt(assets::rotation_zyx(local[j].rot), local[j].trans);
    const int parent = joints[j].parent;
    out[j] = parent >= 0 ? out[static_cast<size_t>(parent)] * joint_mat : joint_mat;
  }
}

bool UnitPose::attach_rest(int idx, float& x, float& y, float& z) const {
  if (!asset_ || idx < 0 || idx >= 32) return false;
  const auto& ap = asset_->attach.points[static_cast<size_t>(idx)];
  if (!ap.used) return false;
  const auto& joints = asset_->skeleton.joints;
  std::vector<render::Mat4> pal(joints.size());
  for (size_t j = 0; j < joints.size(); ++j) {
    const render::Mat4 joint_mat = render::mat4_from_rt(assets::rotation_zyx(joints[j].rest_rot_deg), joints[j].rest_trans);
    const int parent = joints[j].parent;
    pal[j] = parent >= 0 ? pal[static_cast<size_t>(parent)] * joint_mat : joint_mat;
  }
  if (ap.joint >= pal.size()) return false;
  const render::Mat4& mat = pal[ap.joint];
  x = mat.at(0, 0) * ap.pos[0] + mat.at(0, 1) * ap.pos[1] + mat.at(0, 2) * ap.pos[2] + mat.at(0, 3);
  y = mat.at(1, 0) * ap.pos[0] + mat.at(1, 1) * ap.pos[1] + mat.at(1, 2) * ap.pos[2] + mat.at(1, 3);
  z = mat.at(2, 0) * ap.pos[0] + mat.at(2, 1) * ap.pos[1] + mat.at(2, 2) * ap.pos[2] + mat.at(2, 3);
  return true;
}

bool UnitPose::attach_now(int idx, float alpha, float& x, float& y, float& z, bool raw) const {
  if (!asset_ || idx < 0 || idx >= 32) return false;
  const auto& ap = asset_->attach.points[static_cast<size_t>(idx)];
  if (!ap.used && !raw) return false;
  std::vector<render::Mat4> pal;
  palette(alpha, pal);
  if (ap.joint >= pal.size()) return false;
  const render::Mat4& mat = pal[ap.joint];
  x = mat.at(0, 0) * ap.pos[0] + mat.at(0, 1) * ap.pos[1] + mat.at(0, 2) * ap.pos[2] + mat.at(0, 3);
  y = mat.at(1, 0) * ap.pos[0] + mat.at(1, 1) * ap.pos[1] + mat.at(1, 2) * ap.pos[2] + mat.at(1, 3);
  z = mat.at(2, 0) * ap.pos[0] + mat.at(2, 1) * ap.pos[1] + mat.at(2, 2) * ap.pos[2] + mat.at(2, 3);
  return true;
}

bool UnitPose::attach_rot_now(int idx, float alpha, float (&rot)[9]) const {
  if (!asset_ || idx < 0 || idx >= 32) return false;
  const auto& ap = asset_->attach.points[static_cast<size_t>(idx)];
  if (!ap.used) return false;
  std::vector<render::Mat4> pal;
  palette(alpha, pal);
  if (ap.joint >= pal.size()) return false;
  const render::Mat4& mat = pal[ap.joint];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) rot[i * 3 + j] = mat.at(i, j);
  return true;
}

bool UnitPose::attach_or_root(int idx, float alpha, float& x, float& y, float& z) const {
  if (attach_now(idx, alpha, x, y, z)) return true;
  if (!asset_ || idx < 0 || idx >= 32 || asset_->attach.points[static_cast<size_t>(idx)].used) return false;
  std::vector<render::Mat4> pal;
  palette(alpha, pal);
  if (pal.empty()) return false;
  x = pal[0].at(0, 3);
  y = pal[0].at(1, 3);
  z = pal[0].at(2, 3);
  return true;
}

bool UnitPose::attach_at(int idx, float frame, float& x, float& y, float& z) const {
  if (!asset_ || idx < 0 || idx >= 32) return false;
  const auto& ap = asset_->attach.points[static_cast<size_t>(idx)];
  if (!ap.used) return false;
  std::vector<render::Mat4> pal;
  palette_at(clip_, frame, pal);
  if (ap.joint >= pal.size()) return false;
  const render::Mat4& mat = pal[ap.joint];
  x = mat.at(0, 0) * ap.pos[0] + mat.at(0, 1) * ap.pos[1] + mat.at(0, 2) * ap.pos[2] + mat.at(0, 3);
  y = mat.at(1, 0) * ap.pos[0] + mat.at(1, 1) * ap.pos[1] + mat.at(1, 2) * ap.pos[2] + mat.at(1, 3);
  z = mat.at(2, 0) * ap.pos[0] + mat.at(2, 1) * ap.pos[1] + mat.at(2, 2) * ap.pos[2] + mat.at(2, 3);
  return true;
}

bool UnitPose::attach_in_clip(int idx, int clip, float frame, float& x, float& y, float& z) const {
  if (!asset_ || idx < 0 || idx >= 32) return false;
  const auto& ap = asset_->attach.points[static_cast<size_t>(idx)];
  if (!ap.used) return false;
  std::vector<render::Mat4> pal;
  palette_at(clip, frame, pal);
  if (ap.joint >= pal.size()) return false;
  const render::Mat4& mat = pal[ap.joint];
  x = mat.at(0, 0) * ap.pos[0] + mat.at(0, 1) * ap.pos[1] + mat.at(0, 2) * ap.pos[2] + mat.at(0, 3);
  y = mat.at(1, 0) * ap.pos[0] + mat.at(1, 1) * ap.pos[1] + mat.at(1, 2) * ap.pos[2] + mat.at(1, 3);
  z = mat.at(2, 0) * ap.pos[0] + mat.at(2, 1) * ap.pos[1] + mat.at(2, 2) * ap.pos[2] + mat.at(2, 3);
  return true;
}

bool UnitPose::joint_now(int point, float alpha, float& x, float& y, float& z) const {
  // The world position of the joint that attach point `point` names (what the overlays call "bone j"): its translation = that joint's origin
  // in the current (blended) pose at `alpha`; the point's own offset is not added. Model space, y down, the model's placement not applied.
  // False when the point is unused / no model.
  if (!asset_ || point < 0 || point >= 32) return false;
  const auto& ap = asset_->attach.points[static_cast<size_t>(point)];
  if (!ap.used) return false;
  std::vector<render::Mat4> pal;
  palette(alpha, pal);
  if (ap.joint >= pal.size()) return false;
  const render::Mat4& mat = pal[ap.joint];
  x = mat.at(0, 3);
  y = mat.at(1, 3);
  z = mat.at(2, 3);
  return true;
}

}  // namespace oc::game
