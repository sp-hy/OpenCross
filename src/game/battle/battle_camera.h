#pragma once
// Battle camera: keyframe shots (drp type 0x0C) and the timeline scripts that select them (type 0x10).
//
// Shot: selecting one (other than select mode 1) cuts the pose to its first key; a shot of 2+ keys then runs as a camera task whose progress
// advances by the whole step (0x40000 / duration) >> 6 per 60 Hz tick, u = ease(progress, mode) (0 linear, 1 sin, 2 1-cos, 3 sin half-steps,
// 4 cosine S-curve); eye / target are read at u from the smoothed key tracks and the previous pose chases them by smoothing / 256 per tick
// (w8 >> 16: 0x100 = exact); H and roll are taken as read. Shots with ref bytes are in a unit-relative frame: origin at the unit, yaw of the
// unit; two refs = midpoint frame scaled by distance / 2048. The frame is taken when the shot is selected; flag bit 0 makes it follow the
// units every tick.
//
// Glide: the menu cameras move the pose toward a goal by rate / 256 of the distance per tick (eye, target and H) until the step is under a
// unit; any select ends it.
//
// Timeline: wait counter += delay (the high half of the command word) before executing the next command; consecutive 0x23 commands whose
// delays are 0 form a random choice group; 0x2B / 0x2C screen fades, 0x4C sound by tag, 0x2F starts another timeline. A timeline with header flag bit 0 repeats per target:
// 0x4E marks the loop start (target 0), 0x4D jumps back to it while a next target exists, and the end restarts it the same way. 0x50 stalls
// while the actor's script runs, 0x67 yields one tick; 0x42 (CD idle) and 0x7A (music switch done) never stall here.
//
// Keys: 3+ keys are smoothed by the retail cubic B-spline and stamped by the chord length of the master track; frame anchors use the unit
// position + height and yaw only.
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "assets/battle_stage.h"
#include "core/types.h"

namespace oc::game {

struct CamPose {
  std::array<float, 3> eye{0, 0, 0}, target{0, 0, 1};
  float h = 512, roll_deg = 0;
};

// The view axes of a pose in the y-up stage world. The up vector is world up turned by the roll about the view's horizontal heading
// (rotation (0, yaw, roll) with yaw = atan2(-dx, -dz)), then made orthogonal to the forward axis: the roll is not a turn about the
// view axis, so a view looking steeply up or down tilts the picture by more than the roll angle. A positive roll turns the picture
// clockwise.
struct CamAxes {
  std::array<float, 3> right{1, 0, 0}, down{0, -1, 0}, forward{0, 0, 1};
};
CamAxes cam_axes(const std::array<float, 3>& eye, const std::array<float, 3>& target, float roll_deg);

// One command of a type 0x10 timeline.
struct CamCmd {
  u16 op = 0;
  // delay: ticks to wait after the command (high half of the command word).
  // name: 4-char tag (camera / sound / emitter), may be empty.
  // frames, mode, r, g, b: fade length / mode / colour of ops 0x2B / 0x2C.
  // data: for ops 0x2B / 0x2C and 0x3E-0x40 (the screen copy warp), the bytes after the command word, for the view's hooks.
  u16 delay = 0;
  std::string name;
  s16 frames = 0;
  u8 mode = 0, r = 0, g = 0, b = 0;
  Bytes data;
};
struct CamSeq {
  std::string name;
  // Header word; bit 0 = repeat per target.
  u32 flags = 0;
  std::vector<CamCmd> cmds;
};

class CamBank {
 public:
  // Reads every 0x0C and 0x10 entry of a drp (recursing into nested drps). Later adds do not replace earlier names unless `replace`.
  void add_drp(ByteSpan drp, bool replace = false);
  const assets::StageCamera* camera(const std::string& name) const;
  const CamSeq* sequence(const std::string& name) const;
  // Names missing here are looked up in `parent` (the unit's cam0 bank falls back to the stage / battle.prd bank).
  void set_parent(const CamBank* parent) { parent_ = parent; }
  size_t cameras() const { return cams_.size(); }
  size_t sequences() const { return seqs_.size(); }

 private:
  std::map<std::string, assets::StageCamera> cams_;
  std::map<std::string, CamSeq> seqs_;
  const CamBank* parent_ = nullptr;
};

// `OC_BATTLE_AUDIT=1`: one `AUDIT ...` warning per anomaly of the battle flow; false when the variable is unset.
bool battle_audit();
void battle_audit_note(const std::string& what);

// Unit frame for anchored shots (our world: x/z ground, y up).
struct CamFrame {
  bool valid = false;
  // Origin.
  float x = 0, y = 0, z = 0;
  // yaw: radians, rotation about +Y: local (x, z) -> (x cos + z sin, -x sin + z cos); a unit's frame uses its model yaw (forward = -z).
  // pitch: two-anchor frames: tilt of local +X toward the second point, in the PS1 sense (y down): atan2(dy_ps1, |dxz|).
  // mscale: the anchored model's effect scale / 4096 (single-model anchors; applied by shots with flag 0x20).
  float yaw = 0;
  float pitch = 0;
  float mscale = 1.0f;
};
using AnchorFn = std::function<CamFrame(int ref_type, int attach)>;

struct CamEvents {
  // sounds: the 0x4C tags this tick. fade_target: 0..1 black, -1 = none requested.
  std::vector<std::string> sounds;
  float fade_target = -1;
  int fade_frames = 0;
  std::array<u8, 3> fade_rgb{0, 0, 0};
};

class BattleCamera {
 public:
  void set_bank(const CamBank* bank) { bank_ = bank; }
  const CamBank* bank() const { return bank_; }
  void set_anchor(AnchorFn fn) { anchor_ = std::move(fn); }
  // Timeline op 0x22 (start the stage emitter named by the command's tag): the stage objects live elsewhere (StageScene), so the view hooks it.
  void set_emitter_hook(std::function<void(const std::string&)> fn) { emitter_hook_ = std::move(fn); }
  // Timeline ops 0x3E / 0x3F / 0x40 (the screen copy warp, one global state shared with the effect packs': the view draws it).
  void set_warp_hook(std::function<void(const CamCmd&)> fn) { warp_hook_ = std::move(fn); }
  // Timeline ops 0x2B / 0x2C (the screen colour layers 0 / 1, shared with the effect packs; the view draws them).
  void set_screen_hook(std::function<void(const CamCmd&)> fn) { screen_hook_ = std::move(fn); }
  // Timeline op 0x50: true while the first actor's script still runs (the timeline stalls and re-tests next tick).
  void set_actor_busy_hook(std::function<bool()> fn) { actor_busy_hook_ = std::move(fn); }
  // The per-target repeat (header flag bit 0): true when the target list has an entry `index`.
  void set_target_hook(std::function<bool(int index)> fn) { target_hook_ = std::move(fn); }
  // Set by timeline op 0x73 and cleared by a new BattleCamera: the battle start waits for the intro timeline / its shot before the party intro
  // when it is set, even in a field battle.
  bool intro_hold() const { return intro_hold_; }

  // Selects a shot (cut to its first key unless select mode 1; a task when it has 2+ keys). Returns false when the shot does not exist or a
  // mode-2 shot is dropped.
  bool select(const std::string& name);
  // The pose of a shot's first key placed in its frame now, without selecting it.
  bool key_pose(const std::string& name, CamPose& out) const;
  // Writes the pose directly (the command-menu orbit); `cut` also drops the render interpolation from the previous tick.
  void write_pose(const CamPose& pose, bool cut);
  // Starts a glide from the current pose to `goal` at `rate` / 256 per tick.
  void glide_to(const CamPose& goal, int rate);
  // The pose at the end of the last tick.
  const CamPose& current() const { return cur_; }
  // The view is lifted so the ground under the eye stays 256 units below the eye's view centre line (anchored shots without flag 0x10).
  bool ground_lift() const { return lift_; }
  // Starts a timeline script (several can run at once, like the engine's script tasks); its 0x23 commands call select().
  bool play_sequence(const std::string& name);
  void stop_sequences() { runners_.clear(); spawned_.clear(); }
  bool sequence_running() const { return !runners_.empty() || !spawned_.empty(); }
  // Screen fade to black (to = 1) or back (to = 0) over `frames` ticks.
  void start_fade(float to, int frames, std::array<u8, 3> rgb = {0, 0, 0});
  // A camera task is alive: a multi-key head before its end or any queued task (a single-key shot never creates one).
  bool task_running() const { return (shot_ != nullptr && progress_ < 4096.0f) || !bg_.empty(); }
  // A camera task of the shot `name` is alive (the head before its end or a queued task).
  bool shot_running(const std::string& name) const {
    if (shot_ != nullptr && progress_ < 4096.0f && shot_->name == name) return true;
    return std::any_of(bg_.begin(), bg_.end(), [&](const Task& task) { return task.shot != nullptr && task.shot->name == name; });
  }
  float progress() const { return progress_; }
  // Ends the running shot at its last key (a later mode-2 select is no longer dropped): the confirm cut apr0 / apr1 gives way to the charge shot.
  void finish_shot() { bg_.clear(); if (shot_) progress_ = 4096.0f; }
  // Unlinks every camera task and ends a glide; the pose keeps its last value, so the camera holds where it is until the next select.
  void kill_shot() { bg_.clear(); glide_rate_ = 0; if (shot_) { prev_ = cur_; shot_ = nullptr; } }
  // An effect pack starts: the tasks of the earlier actions keep running until the next select, which unlinks them instead of pushing them
  // back, so they are never resumed after the pack's own shots.
  void retire_tasks() { retired_ = true; }
  // Task birth credit (dev harness, OC_FX_DT): the camera task a pack command creates is run at once with the pass delta, so it starts `ticks`
  // ahead of the select.
  void credit_ticks(int ticks) {
    if (shot_ && ticks > 0 && progress_ < 4096.0f) progress_ = std::min(4096.0f, progress_ + static_cast<float>(ticks * progress_step(*shot_)));
  }
  // The last selected shot's name (single-key shots included).
  const std::string& shot_name() const { return last_name_; }

  void tick(CamEvents& events);
  CamPose pose(float alpha) const;
  // Debug free camera (game/free_cam.h): while set, pose() returns this instead of the shot's pose (the shot timeline keeps running underneath).
  // The host clears it before reading the shot's pose each frame and sets it again when the free camera is on.
  void set_free_pose(const CamPose& pose) { free_ = pose; has_free_ = true; }
  void clear_free_pose() { has_free_ = false; }
  bool is_free() const { return has_free_; }
  // Current fade: 0 clear .. 1 black.
  float fade() const { return fade_cur_; }
  float fade_alpha(float alpha) const { return fade_prev_ + (fade_cur_ - fade_prev_) * alpha; }
  std::array<u8, 3> fade_rgb() const { return fade_rgb_; }
  void reset_fade() { fade_prev_ = fade_cur_ = fade_to_ = 0; fade_left_ = 0; }

  // Easing curve of a shot mode; progress and result in 0..1.
  static float ease(int mode, float progress);
  // Evaluates a shot at u in 0..1 (keys in the shot's own frame, no anchoring).
  static CamPose eval(const assets::StageCamera& shot, float eased);
  // Progress added per tick (of 4096); 0 for a zero duration.
  static int progress_step(const assets::StageCamera& shot) { return shot.duration == 0 ? 0 : (0x40000 / shot.duration) >> 6; }

 private:
  struct Runner {
    const CamSeq* seq = nullptr;
    size_t pos = 0;
    int wait = 0;
    // Per-target repeat: the command after the last 0x4E (else the first) and the target being served.
    size_t loop_pos = 0;
    int target = 0;
    // The end was reached; the next step restarts it for the next target or drops it.
    bool ended = false;
  };
  bool next_target(Runner& runner) const;
  void exec(const CamCmd& cmd, CamEvents& events);
  void run(Runner& runner, CamEvents& events);
  void goal_pose(CamPose& out) const;
  // The shot at `progress` in `frame`; flag bit 0 re-anchors it now. False when it has no usable frame (the pose is the shot's own).
  bool place(const assets::StageCamera& shot, float progress, CamFrame frame, float scale, CamPose& out) const;
  bool make_frame(const assets::StageCamera& shot, CamFrame& frame, float& scale) const;

  const CamBank* bank_ = nullptr;
  AnchorFn anchor_;
  std::function<void(const std::string&)> emitter_hook_;
  std::function<void(const CamCmd&)> warp_hook_;
  std::function<void(const CamCmd&)> screen_hook_;
  std::function<bool()> actor_busy_hook_;
  std::function<bool(int)> target_hook_;
  // Set by timeline op 0x73.
  bool intro_hold_ = false;
  // Set by retire_tasks(), cleared by the next select.
  bool retired_ = false;
  // The camera task list: shot_ is the HEAD (the one that drives the pose registers); bg_ holds the tasks behind it, which keep advancing
  // their own progress. A select of a mode 0 / 3+ shot pushes the running head back (it is not killed), a mode 1 shot queues at the tail;
  // when the head ends it is unlinked and the next task takes over at ITS progress.
  struct Task {
    const assets::StageCamera* shot = nullptr;
    float progress = 0;
    CamFrame frozen;
    float frozen_scale = 1.0f;
  };
  // bg_[0] is the task right behind the head.
  std::vector<Task> bg_;
  const assets::StageCamera* shot_ = nullptr;
  // Head progress, 0..4096.
  float progress_ = 0;
  CamPose prev_, cur_;
  std::string last_name_;
  bool lift_ = false;
  CamPose glide_goal_;
  int glide_rate_ = 0;
  CamPose free_;
  bool has_free_ = false;
  // The frame at the moment the shot was selected; only flag bit 0 shots keep tracking the units.
  CamFrame frozen_;
  float frozen_scale_ = 1.0f;

  std::vector<Runner> runners_;
  // Timelines started by op 0x2F this tick (they join the list after it).
  std::vector<Runner> spawned_;

  float fade_prev_ = 0, fade_cur_ = 0, fade_to_ = 0;
  int fade_left_ = 0;
  std::array<u8, 3> fade_rgb_{0, 0, 0};
};

}  // namespace oc::game
