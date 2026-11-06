#pragma once
// Pose of one battle model: plays the clips the animation-sequence scripts select (battle_seq.h SetAnim: id < 0x80 = the model's own
// s4 clip, id >= 0x80 = clip id-0x80 of its at0.prd) with the engine blend (the previous clip keeps running while the new one fades
// in over `blend` ticks) and builds the joint palette for MeshPipeline. 15 keyframes per second = 0.25 per 60 Hz tick.
#include <algorithm>
#include <vector>

#include "assets/model.h"
#include "game/head_turn.h"
#include "render/mat4.h"

namespace oc::game {

class UnitPose {
 public:
  void set_model(const assets::ModelAsset* asset);
  bool has_model() const { return asset_ != nullptr; }
  // The external clip set in force (SetAnim 0x80 + k): null = the model's own at0 clips, else `clips` (an effect pack's type-0x19 'tech'
  // entry hung on the model). The clips must outlive the setting; a pose left on one of
  // them after the set is dropped shows the rest pose until the next SetAnim.
  void set_ext_clips(const std::vector<assets::AnimationClip>* clips) { ext_clips_ = clips; }
  const std::vector<assets::AnimationClip>* ext_clips() const { return ext_clips_; }

  // Asset clip index of a script clip id, -1 if the model has none.
  int clip_for(int id) const;
  // Keyframe count (1 when unknown).
  int frames_of(int id) const;
  // SetAnim: `blend` = ticks of cross-fade (0 = cut).
  // `run_source`: SetAnim flag 0x8000 = the old clip keeps running during the fade; else it stays frozen at the
  // keyframe it had.
  // `mode` = (SetAnim flags & 0x6000) >> 13: 1 (flag 0x2000) = the eased blend
  // (w += (1 - w) / 4 per tick, the hit reactions' `a200`), 0 / 2 / 3 = linear 1 / blend ticks.
  void play(int id, int blend, bool run_source = false, int mode = 0);
  // SetAnim of a clip by its index into ModelAsset::clips (the overlays' attached motion streams, effects::FxModels::PackDrive: the
  // clips a .mtc added past the model's own carry no script id).
  void play_clip(int clip, int blend, bool run_source = false, int mode = 0);
  int current_clip() const { return clip_; }
  // The SetAnim blend mode (flags 0x6000 >> 13) as the last SetAnim left it.
  int blend_mode() const { return mode_; }
  // The SetAnim run-source flag (0x8000) as the last SetAnim left it.
  bool run_source() const { return run_source_; }
  // Speed in 1/256 keyframe per tick: 64 = 0.25.
  void set_speed(int s64) { speed_ = static_cast<float>(s64) / 256.0f; }
  void set_loop(bool on) { loop_ = on; }
  void tick();
  // Only the element tasks (rotation / scale ramps, rates): they run on a hidden model too, unlike the motion step.
  void tick_elements();
  // A hidden / off-stage model skips the motion step and the joint-matrix pass, so its joint matrices stay what the last visible pass
  // left (a SetAnim in between only changes the clip word).
  // hold(true) snapshots the palette once; every attach / joint read (palette()) then returns the snapshot until hold(false).
  void hold(bool on);
  bool held() const { return held_; }
  int current_id() const { return id_; }
  // A non-looping clip has reached its last keyframe.
  bool at_end() const;

  // Joint world matrices at `alpha` between the last two ticks.
  void palette(float alpha, std::vector<render::Mat4>& out) const;
  // Joint world matrices of asset clip `clip` (index into ModelAsset::clips, -1 = rest pose) at keyframe `frame` (wrapped into
  // the clip's length): the effect engine's model objects drive their motion themselves.
  void palette_at(int clip, float frame, std::vector<render::Mat4>& out) const;
  // Position (model units, y down) of attach point `idx` in the rest pose; false when unused / no model.
  bool attach_rest(int idx, float& x, float& y, float& z) const;
  // Same for the current (blended) pose at `alpha`; model space, y down.
  // `raw`: an all-zero record counts too (the point is then the joint's origin).
  bool attach_now(int idx, float alpha, float& x, float& y, float& z, bool raw = false) const;
  // attach_now, but an all-zero record (file slot never filled) is what the engine still reads: the joint matrix of the record's joint
  // (joint 0) applied to its position (0), i.e. joint 0's origin.
  bool attach_or_root(int idx, float alpha, float& x, float& y, float& z) const;
  // Rotation part (row-major 3 x 3, model space, y down) of the joint matrix attach point `idx` names: the orientation an effect object
  // with flag bit 7 inherits. False when unused.
  bool attach_rot_now(int idx, float alpha, float (&rot)[9]) const;
  // Attach point `idx` on the CURRENT clip at keyframe `frame` (no blend, no alpha): the weapon-trail sampler poses the model at a
  // frame of its clip and reads the points; model space, y down.
  bool attach_at(int idx, float frame, float& x, float& y, float& z) const;
  // The same on asset clip `clip` (dev: which clip frame a retail reach reading corresponds to, OC_APPROACH_TRACE).
  bool attach_in_clip(int idx, int clip, float frame, float& x, float& y, float& z) const;
  // Element overrides of the anmseq ops 0x6A-0x6D / 0x8F / 0x90: per joint, a rotation and a scale; a frozen joint is no longer driven
  // by the clip. `deg` = degrees.
  // joint_motion: op 0x6B (on) / 0x6A (off: the joint keeps the rotation it has, a later 6C / 6D turns it).
  void joint_motion(int joint, bool on);
  // Op 0x6C: the (frozen) joint's rotation walks the short way round to `deg` over ticks (< 1: at once).
  void joint_rot_to(int joint, const assets::Vec3& deg, int ticks);
  // Op 0x8C: motion off, the old z saved, z walks to z_deg (x, y stay) over ticks.
  void joint_tilt(int joint, float z_deg, int ticks);
  // Op 0x8D: z walks back to the saved value over ticks, the clip drives the joint again at the end.
  void joint_tilt_back(int joint, int ticks);
  // Op 0x6D: the (frozen) joint turns by this every tick.
  void joint_rot_rate(int joint, const assets::Vec3& deg_per_tick);
  // Op 0x8F: the joint matrix scale ramps to `scale` (1 = 4096) over ticks (< 1: at once).
  void joint_scale_to(int joint, const assets::Vec3& scale, int ticks);
  // Op 0x90: the scale ramp of the joint ends where it is.
  void joint_scale_stop(int joint);
  void clear_joint_overrides() { ov_.clear(); }
  // All but the joints whose bit is set in `keep`. The SCALE words stay on every joint: op 0x8F writes them and nothing at a script cut /
  // pack end rewrites them (e.g. the Bomber's wick joints stay 0 through the whole attack).
  void clear_joint_overrides(unsigned long long keep) {
    for (size_t j = 0; j < ov_.size(); ++j)
      if (j >= 64 || !((keep >> j) & 1ull)) {
        JointOv cleared{};
        cleared.scale = ov_[j].scale, cleared.sto = ov_[j].sto, cleared.sstep = ov_[j].sstep, cleared.sleft = ov_[j].sleft;
        ov_[j] = cleared;
      }
  }
  bool has_joint_overrides() const { return !ov_.empty(); }
  // Dev log (OC_FX_UNITS): the op-0x8F x scale word / 4096.
  float joint_scale(size_t joint) const { return joint < ov_.size() ? ov_[joint].scale[0] : 1.0f; }
  size_t joint_override_count() const { return ov_.size(); }
  float frame_cur() const { return t_cur_; }
  float frame_prev() const { return t_prev_; }
  // Keyframes per tick.
  float step_per_tick() const { return speed_; }
  bool joint_now(int point, float alpha, float& x, float& y, float& z) const;

  // Head turn (anmseq ops 0x88 / 0x89, the head-turn callback of head_turn.h on the joint of attach point 1). head_look: the head follows
  // `target` (model space, y down) at `rate` (1..0xFFF; else the old rate is kept), starting from the model's forward axis when it is
  // not turning yet. head_target: a new target, nothing else. head_release: the target becomes the point 0x1000 straight ahead of attach
  // point 1 (model -z) and the callback goes once the head has settled. One step per tick().
  void head_look(const float target[3], int rate);
  void head_target(const float target[3]) { std::copy(target, target + 3, head_target_); }
  void head_release();
  bool head_active() const { return head_.active; }
  // Current head angles (4096 per turn): ax about the head joint's x, az the nod.
  float head_ax() const { return head_.ax; }
  float head_az() const { return head_.az; }

 private:
  struct Local {
    assets::Vec3 rot, trans;
  };
  void sample(int clip, float frame, std::vector<Local>& out) const;
  float length(int clip) const;
  // Clip index space: 0.. = ModelAsset::clips, kExtBase + k = clip k of the external set in force; null when out of range / dropped.
  static constexpr int kExtBase = 0x10000;
  const assets::AnimationClip* clip_at(int clip) const;

  struct JointOv {
    bool frozen = false;
    assets::Vec3 rot{0, 0, 0}, rate{0, 0, 0}, rto{0, 0, 0}, rstep{0, 0, 0};
    int rleft = 0;
    float saved_z = 0;
    // Op 0x8D: clip motion back on when the rotation task ends.
    bool resume = false;
    assets::Vec3 scale{1, 1, 1}, sto{1, 1, 1}, sstep{0, 0, 0};
    int sleft = 0;
  };
  JointOv* ov(int joint, bool create);
  // The joint of attach point 1, -1 without one.
  int head_joint() const;
  // The head frame (rotation of the head joint's parent composite x its rest local, scale removed) and attach point 1, model space.
  bool head_frame(float rot[9], float face[3]) const;
  void head_step();
  HeadTurn head_;
  float head_target_[3]{};
  // Indexed by joint; empty until an op asks.
  std::vector<JointOv> ov_;

  const assets::ModelAsset* asset_ = nullptr;
  const std::vector<assets::AnimationClip>* ext_clips_ = nullptr;
  int id_ = -1;
  int clip_ = -1, from_ = -1;
  float t_prev_ = 0, t_cur_ = 0, f_prev_ = 0, f_cur_ = 0;
  // Blend weight of the current clip.
  float w_prev_ = 1, w_cur_ = 1, w_step_ = 1;
  float speed_ = 0.25f;
  bool loop_ = true;
  // mode_: SetAnim flags 0x6000 >> 13 (1 = eased blend weight).
  // run_source_: advance the cross-fade source clip (SetAnim flag 0x8000).
  // held_: hold() froze the joint matrices.
  int mode_ = 0;
  bool run_source_ = false;
  bool held_ = false;
  std::vector<render::Mat4> held_pal_;
};

}  // namespace oc::game
