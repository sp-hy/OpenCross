// BattleView camera: orbit and target shots, per-frame tick and the 3D projection helpers.
#include "game/battle/battle_view.h"
#include "game/free_cam.h"
#include "game/encounter_strips.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
namespace oc::game {

using namespace bv;

namespace {
// Radians added to the orbit start angle (see command_orbit).
constexpr float kOrbitBias = 0.0f;
}  // namespace

// ------------------------------------------------------------------------------------------------------------ camera
render::Mat4 BattleView::Cam::view() const {
  render::Mat4 view_mat;
  for (int col_index = 0; col_index < 3; ++col_index) {
    view_mat.at(0, col_index) = right[static_cast<size_t>(col_index)];
    view_mat.at(1, col_index) = down[static_cast<size_t>(col_index)];
    view_mat.at(2, col_index) = fwd[static_cast<size_t>(col_index)];
  }
  view_mat.at(0, 3) = -dot(right, eye);
  view_mat.at(1, 3) = -dot(down, eye);
  view_mat.at(2, 3) = -dot(fwd, eye);
  return view_mat;
}

// Same off-centre projection as FieldCamera::view_proj: the PS1 battle buffer (320 x 224, GTE offset (160, 112)) mapped
// by `lb` (BattleView::picture() or a 320 x 224 target); the mesh and stage shaders write the real depth.
render::Mat4 BattleView::Cam::view_proj(const render::Letterbox& lb) const {
  const float ax = 2.0f * lb.scale * h / lb.vw;
  const float ay = 2.0f * lb.scale_y * h / lb.vh;
  const float cx = (2.0f * (lb.scale * ox + lb.ox)) / lb.vw - 1.0f;
  const float cy = (2.0f * (lb.scale_y * oy + lb.oy)) / lb.vh - 1.0f;
  render::Mat4 proj;
  proj.at(0, 0) = ax;
  proj.at(0, 2) = cx;
  proj.at(0, 3) = 0;
  proj.at(1, 1) = ay;
  proj.at(1, 2) = cy;
  proj.at(1, 3) = 0;
  proj.at(2, 2) = 0.5f;
  proj.at(2, 3) = 0;
  proj.at(3, 2) = 1.0f;
  proj.at(3, 3) = 0;
  return proj * view();
}

bool BattleView::Cam::screen(float x, float y, float z, float& sx, float& sy) const {
  const std::array<float, 3> delta{x - eye[0], y - eye[1], z - eye[2]};
  const float zc = dot(fwd, delta);
  if (zc < 1.0f) return false;
  sx = ox + h * dot(right, delta) / zc;
  sy = oy + h * dot(down, delta) / zc;
  return true;
}

// The canvas spans the 4:3 letterbox (Letterbox::fit), the picture its rows [kFieldCrop, kLayerH - kFieldCrop] (Letterbox::field).
// Widescreen: the picture's extra columns lie left / right of the canvas at the same scale, so a unit there projects to canvas x < 0 or
// > 1280 and its damage digits / cursor still land on it.
bool BattleView::Cam::project(float x, float y, float z, float& cx, float& cy) const {
  float sx = 0, sy = 0;
  if (!screen(x, y, z, sx, sy)) return false;
  cx = sx * (kCanvasW / render::kLayerW);
  cy = (render::kFieldCrop + sy * (render::kLayerH - 2.0f * render::kFieldCrop) / kBufferRows) * (kCanvasH / render::kLayerH);
  return true;
}

// The command-menu orbit: the eye circles the acting PC at 2400 units, height (model height word + 0x100), the target is the PC at 2/3 of its
// height, H = 0x200. The angle is `(((yaw_byte) + 0x30) & 0x3F) * 0x40 + orbit_clock_` in 4096 = turn, where the unit byte is the unit's yaw
// / 64 (0x10 for a PC facing yaw 1024); eye = (x - cos a * d, y, z + sin a * d).
CamPose BattleView::orbit_pose(int pc) const {
  const Visual& visual = vis_[static_cast<size_t>(pc)];
  // Unit yaw in PS1 units (place_units: face = (-sin yaw, -cos yaw), 4096 = turn), truncated to the unit's yaw byte (yaw / 64).
  float yaw_units = std::atan2(-visual.face_x, -visual.face_z) * (4096.0f / (2.0f * kPi));
  if (yaw_units < 0.0f) yaw_units += 4096.0f;
  const int yaw_byte = static_cast<int>(yaw_units) / 64;
  const int angle_units = ((yaw_byte + 0x30) & 0x3F) * 0x40 + orbit_clock_;
  const float angle = static_cast<float>(angle_units) * (2.0f * kPi / 4096.0f) + kOrbitBias;
  const float dist = 2400.0f;
  CamPose pose;
  pose.eye = {visual.x - std::cos(angle) * dist, visual.height + 256.0f, visual.z + std::sin(angle) * dist};
  pose.target = {visual.x, visual.height * (2.0f / 3.0f), visual.z};
  pose.h = 512.0f;
  pose.roll_deg = 0.0f;
  return pose;
}

// The DYNAMIC target shot: target = the target's anchor 3 (attach point 1), eye = the PC's anchor 2 (attach point 0; when the PC targets
// itself: 0x900 in front of it, looking back); if the eye is closer than 0xA00 to the target it is pushed out to 0x900 along the same
// direction; H = 0x200.
CamPose BattleView::dynamic_pose(int target) const {
  auto point = [&](int unit, int attach) {
    const Visual& visual = vis_[static_cast<size_t>(unit)];
    // A lifted / steered unit carries its anchors with it.
    const std::array<float, 3> world = unit_world(unit, 1.0f);
    std::array<float, 3> out{world[0], world[1] + visual.height * 0.55f, world[2]};
    const float yaw = std::atan2(-visual.face_x, -visual.face_z);
    float mx = 0, my = 0, mz = 0;
    // The S3 record as is (an all-zero one = joint 0).
    if (visual.has_model && visual.pose.attach_now(attach - 2, 1.0f, mx, my, mz, true)) {
      const float cs = std::cos(yaw), sn = std::sin(yaw);
      out = {world[0] + mx * cs + mz * sn, world[1] - my, world[2] - mx * sn + mz * cs};
    }
    return out;
  };
  const Visual& active = vis_[static_cast<size_t>(active_pc_)];
  std::array<float, 3> aim = point(target, 3), eye = point(active_pc_, 2);
  if (target == active_pc_) {
    eye[0] += active.face_x * 2304.0f;
    eye[2] += active.face_z * 2304.0f;
  }
  const float dx = eye[0] - aim[0], dy = eye[1] - aim[1], dz = eye[2] - aim[2];
  if (std::sqrt(dx * dx + dy * dy + dz * dz) < 2560.0f) {
    const float length = std::max(1.0f, std::hypot(dx, dz));
    eye[0] = aim[0] + dx / length * 2304.0f;
    eye[2] = aim[2] + dz / length * 2304.0f;
  }
  CamPose pose;
  pose.eye = eye;
  pose.target = aim;
  pose.h = 512.0f;
  pose.roll_deg = 0.0f;
  return pose;
}

// The menu camera runs only while no camera task and no timeline is alive (a running shot keeps the picture; the command-menu steps end the
// shots, end_camera_tasks). Top menu, and between actions once nothing is queued: the orbit pose round the menu's PC is written every tick (a
// cut when it starts or the PC changes). Between actions that PC is already the one the menu goes to next (menu_pc_after); when no PC can
// act yet the camera keeps its pose. A Defend action leaves the camera to the orbit: the defending PC's guard pose plays while the camera
// circles the next PC. A battle-script cutscene
// takes the orbit's PC away until a command menu opens again, so the pose written last stays. Target selection in NORMAL mode, and any
// all-targets selection: one glide from the pose on screen to `zen0`'s first key at 0x40 / 256 per tick. DYNAMIC mode with a cursor: a glide
// at 0x14 / 256 per tick to the DYNAMIC shot whenever the cursor unit changes. Other menus (element list, waiting) keep what is on screen.
void BattleView::menu_camera() {
  if (ui_ == Ui::Intro || ui_ == Ui::Result || camera_.task_running() || camera_.sequence_running()) {
    orbit_pc_ = -1;
    return;
  }
  const bool defending = act_.phase != Act::Phase::None && !act_.ev.empty() && act_.ev.front().kind == battle::ActionEvent::Kind::Defend;
  const bool idle = active_pc_ >= 0 && (act_.phase == Act::Phase::None || defending) && queue_.empty() && finish_ == Finish::None;
  const bool between = ui_ == Ui::Wait && !orbit_blocked_ && pending_.empty();
  const int orbit_pc = between ? menu_pc_after() : active_pc_;
  const bool cast_held = cast_handover_ > 0 || (cast_hold_pc_ >= 0 && orbit_pc != cast_hold_pc_ && !menu_live());
  // The party swap list keeps the top menu's orbit (the orbit clock runs on, so a held camera would jump when the list closes).
  const bool top = idle && orbit_pc >= 0 && (ui_ == Ui::Top || ui_ == Ui::Swap || between) && !cast_held && vis_[static_cast<size_t>(orbit_pc)].has_model;
  const bool target = idle && !defending && ui_ == Ui::Target && !targets_.empty();
  const bool all_targets = target && (tgt_mode_ == Tgt::EnemyAll || tgt_mode_ == Tgt::AllyAll);
  if (target && (!cam_dynamic_ || all_targets)) {
    orbit_pc_ = -1;
    if (!menu_glide_) {
      CamPose goal;
      if (camera_.key_pose("zen0", goal)) camera_.glide_to(goal, 0x40);
      menu_glide_ = true;
      dyn_target_ = -1;
    }
    return;
  }
  menu_glide_ = false;
  if (top) {
    cast_hold_pc_ = -1;
    camera_.write_pose(orbit_pose(orbit_pc), orbit_pc_ != orbit_pc);
    orbit_pc_ = orbit_pc;
    dyn_target_ = -1;
    return;
  }
  orbit_pc_ = -1;
  if (target) {
    const int tgt = targets_[static_cast<size_t>(std::clamp(target_cursor_, 0, static_cast<int>(targets_.size()) - 1))];
    if (tgt != dyn_target_) {
      camera_.glide_to(dynamic_pose(tgt), 0x14);
      dyn_target_ = tgt;
    }
  }
}

void BattleView::select_approach() {
  if (!camera_.select(apr_pick(cam_dynamic_)) && cam_dynamic_) camera_.select(apr_pick(false));
}

bool BattleView::end_camera_tasks() {
  const bool alive = camera_.task_running() || camera_.sequence_running();
  camera_.kill_shot();
  camera_.stop_sequences();
  fx_warp_.on = false;
  return alive;
}

std::string BattleView::route_camera() const {
  if (camera_.task_running() || camera_.sequence_running()) return "shot";
  if (orbit_pc_ >= 0) return "orbit " + std::to_string(orbit_pc_);
  if (menu_glide_ || dyn_target_ >= 0) return "target";
  return "hold";
}

void BattleView::top_command_camera() {
  if (!end_camera_tasks() || active_pc_ < 0 || !vis_[static_cast<size_t>(active_pc_)].has_model) return;
  camera_.write_pose(orbit_pose(active_pc_), true);
  orbit_pc_ = active_pc_;
}

void BattleView::apply_camera(float alpha) {
  camera_.clear_free_pose();
  const BattleCamera& source = draw_camera_ ? *draw_camera_ : camera_;
  CamPose pose = source.pose(alpha);
  // Debug tab free camera: the pose is the free one (the shot / orbit above only seed it); H = 512 x FovDeg.
  if (FreeCam& free_camera = free_cam(); free_camera.on()) {
    if (free_camera.needs_init()) free_camera.init_from(pose.eye, pose.target);
    pose.eye = free_camera.eye();
    pose.target = free_camera.target();
    pose.h = 512.0f;
    pose.roll_deg = 0.0f;
    camera_.set_free_pose(pose);
  }
  // Dev: the camera as DRAWN (draw alpha, PS1 axes: y down), then every unit's facing in 4096ths of a turn (- = absent).
  if (static const bool trace = std::getenv("OC_DRAW_CAM") != nullptr; trace) {
    std::string yaws;
    for (int i = 0; i < battle::kUnits; ++i) {
      const Visual& visual = vis_[static_cast<size_t>(i)];
      if (!bunit(i).present) {
        yaws += " -";
        continue;
      }
      yaws += std::format(" {}", static_cast<int>(std::lround(std::atan2(-visual.face_x, -visual.face_z) * (4096.0f / (2.0f * kPi)))) & 0xFFF);
    }
    log::debug("draw camera frame {} alpha {:.3f} eye {:.0f} {:.0f} {:.0f} tgt {:.0f} {:.0f} {:.0f} H {:.0f} orbit {} yaw{}", frame_count_, alpha, pose.eye[0], -pose.eye[1],
               pose.eye[2], pose.target[0], -pose.target[1], pose.target[2], pose.h, orbit_clock_, yaws);
  }
  cam_.eye = pose.eye;
  cam_.h = pose.h * render::fov_scale();
  const CamAxes axes = cam_axes(pose.eye, pose.target, pose.roll_deg);
  const std::array<float, 3>& forward = axes.forward;
  const std::array<float, 3>& right = axes.right;
  const std::array<float, 3>& down = axes.down;
  // An anchored shot keeps the ground under the eye at least 256 units below the view centre line: the view is raised along its own up axis.
  if (source.ground_lift() && !free_cam().on()) {
    const float ground_y = dot(down, {0.0f, -pose.eye[1], 0.0f});
    if (ground_y < 256.0f)
      for (size_t i = 0; i < 3; ++i) cam_.eye[i] -= down[i] * (256.0f - ground_y);
  }
  cam_.fwd = forward;
  cam_.right = right;
  cam_.down = down;
  // Effect screen shake: the PS1 shifts the DISPLAY window, not the camera, so the whole 3D picture moves by the
  // shake bytes in buffer pixels (projection centre = GeomOffset - shake).
  cam_.ox = kGeomOffsetX - static_cast<float>(fx_shake_x_);
  cam_.oy = kGeomOffsetY - static_cast<float>(fx_shake_y_);
}

// ------------------------------------------------------------------------------------------------------------ update
void BattleView::tick() {
  ++frame_count_;
  if (act_.phase == Act::Phase::Cast && act_.actor >= 0 && act_.actor < battle::kPcSlots) orbit_held_ = true;
  if (!orbit_held_) orbit_clock_ += 2;
  // Effect slow motion skips the game tick.
  if (fx_slow_skip()) return;
  ++tick_count_;
  cast_glow_tick();
  // The RDE hand-over's clock includes the load before the party intro, so its `fade` cut comes that much later.
  if (white_ticks_ >= 0 && ++white_ticks_ > (rde_hand_ ? encstrips::kLoadHold + kStartFadeEnd : kStartFadeEnd)) {
    white_ticks_ = -1;
    // The `fade` cut: the feed-back of the hand-over ends, the next frame is the clean scene.
    if (rde_hand_) {
      rde_hand_ = false;
      hand_carry_ = {};
      fx_acc_have_ = false;
      fx_acc_tick_ = -1;
    }
  }
  if (hand_tick_ >= 0 && ++hand_tick_ > kHandTicks) hand_tick_ = -1;
  tick_camera();
  stage_scene_.tick();
  tick_units();
  showcase_tick();
  tick_links();
  tick_tex_flips();
  tick_action();
  tick_menu_entry();
  fx_tick();
  for (const auto& [unit, yaw] : pending_rot_) set_rot_now(unit, yaw);
  pending_rot_.clear();
  anm_tick();
  // The battle-script stream (E0 / E2 / delays / executor waits) and its message window.
  script_pump();
  smsg_tick();
  if (wipe_hold_ > 0) --wipe_hold_;
  for (Pop& pop : pops_) {
    pop.age_prev = pop.age;
    pop.age += 1.0f;
  }
  std::erase_if(pops_, [](const Pop& pop) { return pop.age > static_cast<float>(pop.end); });
  pop_rects_.tick();
  if (finish_ != Finish::None) tick_finish();
}

// The stage intro (`bti*` / 'boss') and 'fade'. Without the RDE hand-over the battle starts where the load ends, so the effect tasks' load-time
// ticks run here at once (the subtractive 'fade' flat, COLOR 4090, rate -34 / tick, LIFE 120, blend 3 = B - F, then shows only its last ticks;
// the timelines' early 0x4C sounds are played); under the hand-over they run live.
void BattleView::start_stage_intro(bool preroll) {
  intro_started_ = true;
  if (!intro_pending_.empty()) {
    camera_.play_sequence(intro_pending_);
    log::info("battle: intro timeline {} at tick {} (hand-over tick {}, bracket clock {})", intro_pending_, tick_count_, white_ticks_, rde_m0_ + tick_count_);
    intro_pending_.clear();
  }
  if (field_battle_ && bank_.sequence("fade")) camera_.play_sequence("fade");
  if (!preroll) return;
  for (int i = 0; i < kBtlinitLoadTicks; ++i) {
    CamEvents cam_events;
    camera_.tick(cam_events);
    // The intro's 0x2B / 0x2C layer tasks step with the timelines.
    fx_screen_tick();
    stage_scene_.tick();
    for (const std::string& tag : cam_events.sounds) play_tag_sound(tag, 127);
  }
}

// A random `pli*`: the running shots / timelines (and the warp bracket) go, the party intro starts.
void BattleView::start_party_intro(const std::string& pli) {
  // Every camera task is unlinked first (the pose registers kept); otherwise a `bti*` shot still running would be pushed behind `pli*`'s
  // first multi-key shot and resumed after it.
  camera_.kill_shot();
  camera_.stop_sequences();
  fx_warp_.on = false;
  camera_.play_sequence(pli);
  log::info("battle: party intro timeline {} at tick {} (hand-over tick {}, bracket clock {})", pli, tick_count_, white_ticks_, rde_m0_ + tick_count_);
  // The PCs placed after a held intro get their model now (S5 entry 8 from its start).
  for (int i = 0; i < battle::kPcSlots; ++i)
    if (unplaced_[static_cast<size_t>(i)]) {
      unplaced_[static_cast<size_t>(i)] = false;
      vis_[static_cast<size_t>(i)].pose.play(1, 0);
      start_spawn(i);
    }
}

void BattleView::tick_camera() {
  CamEvents cam_events;
  camera_.tick(cam_events);
  for (const std::string& tag : cam_events.sounds) play_tag_sound(tag, 127);
  // Every time the element menu is (re)entered (from the command box, or back out of the target selection): the acting list := the active PC
  // and the shot `elm0` (unit-framed cast pose shot) is selected, unless the PC is engaged at the enemy.
  if (ui_ == Ui::Element && cam_ui_ != Ui::Element && active_pc_ >= 0 && !engaged_ && act_.phase == Act::Phase::None) {
    anchor_actors_ = {active_pc_};
    camera_.select("elm0");
  }
  // An open command menu gives the orbit its PC back.
  if (ui_ != Ui::Wait && ui_ != Ui::Intro && ui_ != Ui::Result && !hud_off_) orbit_blocked_ = false;
  cam_ui_ = ui_;
  if (ui_ == Ui::Intro) {
    ++intro_ticks_;
    const bool rde_load = rde_hand_ && white_ticks_ >= 0;
    if (!intro_started_ && (!rde_load || white_ticks_ >= encstrips::kIntroStart)) start_stage_intro(!rde_load);
    // The load ends (under the RDE hand-over: kLoadHold ticks after the take-over): the first PC is placed; a field battle's `pli*` starts now,
    // a held intro's after the `bti*` timeline and its shot.
    if (intro_started_ && pli_after_load_ && (!rde_load || white_ticks_ >= encstrips::kLoadHold)) {
      pli_after_load_ = false;
      int first = -1;
      for (int i = 0; i < battle::kPcSlots; ++i)
        if (unplaced_[static_cast<size_t>(i)] && (first < 0 || bunit(i).src_id < bunit(first).src_id)) first = i;
      if (first >= 0) {
        unplaced_[static_cast<size_t>(first)] = false;
        vis_[static_cast<size_t>(first)].pose.play(1, 0);
        start_spawn(first);
      }
      if (field_battle_ && !camera_.intro_hold()) {
        const std::string pli = pli_pending_;
        pli_pending_.clear();
        start_party_intro(pli);
      }
    }
    if (!pli_pending_.empty() && !pli_after_load_ && !camera_.sequence_running() && !camera_.task_running()) {
      const std::string pli = pli_pending_;
      pli_pending_.clear();
      start_party_intro(pli);
    }
    // The command phase starts once no camera shot task and no timeline but 'loop' is alive; then every stage emitter named `bti0` is
    // stopped and its live objects freed - bg_00's sea (nea5 / nea6 / the nea7 sheen), birds and fish exist only during the
    // intro (the `near` / `far` / `roof` / `ring` layers stay). The outro starts them again before 'bto0' (battle_view_sequence.cpp).
    // [Enhancements] RetainBattleScenery leaves the layer running.
    if ((intro_started_ && pli_pending_.empty() && !camera_.sequence_running() && !camera_.task_running() && intro_ticks_ > 8) || intro_ticks_ > 1500 || debug_ui_ != 0 || dbg_.cast_element > 0 ||
        dbg_.weapon >= 0) {
      // A cut intro (dev casts / debug UI / the cap) never reached the party intro: place them now.
      pli_pending_.clear();
      pli_after_load_ = false;
      intro_started_ = true;
      intro_pending_.clear();
      for (int i = 0; i < battle::kPcSlots; ++i)
        if (unplaced_[static_cast<size_t>(i)]) {
          unplaced_[static_cast<size_t>(i)] = false;
          start_spawn(i);
        }
      camera_.stop_sequences();
      // At the end of the intro a cut `pli*` (dev casts / debug UI) must not leave its 3E..40 warp on.
      fx_warp_.on = false;
      intro_layer_cut_ = speed_gate_ = true;
      if (!keep_intro_layer_) fx_stage_layer("bti0", false, false);
      // The camera holds the party intro's last pose until the menu camera takes over.
      camera_.kill_shot();
      camera_.reset_fade();
      // update() moves to Top once nothing is queued; the held script stream (script_pump) starts now.
      ui_ = Ui::Wait;
    }
  }
  menu_camera();
  // A cutscene that began last tick holds the camera from now on: the pass above was the one of the tick the action ended.
  if (orbit_block_next_) orbit_blocked_ = true, orbit_block_next_ = false;
}

}  // namespace oc::game
