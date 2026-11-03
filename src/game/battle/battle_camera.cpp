// Battle camera: shot records (drp type 0x0C) parsed into key lists, timeline scripts (type 0x10) parsed into command lists, and the
// camera task list that evaluates them: B-spline smoothing of the keys, chord-length time stamps, eased progress, unit-anchored frames
// and the per-tick chase of the pose registers.
#include "game/battle/battle_camera.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "import/container.h"

namespace oc::game {

namespace {

constexpr float kPi = 3.14159265358979f;

u32 rd32(ByteSpan data, size_t offset) {
  return offset + 4 <= data.size() ? static_cast<u32>(data[offset]) | data[offset + 1] << 8 | data[offset + 2] << 16 | static_cast<u32>(data[offset + 3]) << 24 : 0u;
}
u16 rd16(ByteSpan data, size_t offset) { return offset + 2 <= data.size() ? static_cast<u16>(data[offset] | data[offset + 1] << 8) : u16{0}; }

std::string name4(ByteSpan data, size_t offset) {
  std::string out;
  for (size_t i = 0; i < 4 && offset + i < data.size(); ++i)
    if (data[offset + i]) out.push_back(static_cast<char>(data[offset + i]));
  return out;
}

// A type 0x0C shot: u32 flags, u16 duration, u16 ease mode, u32 w8 (low half = master track, high half = smoothing), u8 ref[4], u32 key count,
// then 20-byte keys (u32 w0, s16 eye xyz, s16 H, s16 target xyz, s16 roll in 1024ths of a turn). PS1 y is negated into the stage's y-up world.
bool parse_camera(const import::DrpEntry& entry, assets::StageCamera& cam) {
  const ByteSpan data = entry.data;
  if (data.size() < 20) return false;
  const u32 count = rd32(data, 16);
  if (count == 0 || 20ull + 20ull * count > data.size()) return false;
  cam = {};
  cam.name = entry.name;
  cam.flags = rd32(data, 0);
  cam.duration = rd16(data, 4);
  cam.mode = rd16(data, 6);
  cam.w8 = rd32(data, 8);
  for (size_t i = 0; i < 4; ++i) cam.ref[i] = data[12 + i];
  auto coord = [&](size_t off) { return static_cast<float>(static_cast<s16>(rd16(data, off))); };
  for (u32 i = 0; i < count; ++i) {
    const size_t off = 20 + 20ull * i;
    assets::StageCameraKey key;
    key.w0 = rd32(data, off);
    key.eye = {coord(off + 4), -coord(off + 6), coord(off + 8)};
    key.projection = coord(off + 10);
    key.target = {coord(off + 12), -coord(off + 14), coord(off + 16)};
    key.roll_raw = static_cast<s16>(rd16(data, off + 18));
    key.roll_deg = static_cast<float>(key.roll_raw) * 360.0f / 1024.0f;
    cam.keys.push_back(key);
  }
  return true;
}

// A type 0x10 timeline: u32 header flags, commands from +0x14, each a u32 word (op = bits 0-8, length in bytes = bits 9-14, delay = high half) followed by its
// operands; a 4-char tag at +0xC when the command is 16 bytes or longer; the fade ops 0x2B / 0x2C carry frames, mode and colour. 0x21 ends it.
bool parse_seq(const import::DrpEntry& entry, CamSeq& seq) {
  const ByteSpan data = entry.data;
  seq = {};
  seq.name = entry.name;
  seq.flags = rd32(data, 0);
  size_t offset = 0x14;
  while (offset + 4 <= data.size()) {
    const u32 word = rd32(data, offset);
    const u32 op = word & 0x1FF, len = (word >> 9) & 0x3F;
    if (len < 4 || offset + len > data.size()) return false;
    CamCmd cmd;
    cmd.op = static_cast<u16>(op);
    cmd.delay = static_cast<u16>(word >> 16);
    if (len >= 16) {
      const std::string tag = name4(data, offset + 0xC);
      bool printable = !tag.empty();
      for (char ch : tag) printable = printable && ch >= 0x20 && ch < 0x7F;
      if (printable) cmd.name = tag;
    }
    if ((op >= 0x3E && op <= 0x40) || op == 0x2B || op == 0x2C)
      cmd.data.assign(data.begin() + static_cast<std::ptrdiff_t>(offset + 4), data.begin() + static_cast<std::ptrdiff_t>(offset + len));
    if (op == 0x2B || op == 0x2C) {
      cmd.frames = static_cast<s16>(rd16(data, offset + 4));
      cmd.mode = data[offset + 8];
      cmd.r = data[offset + 9];
      cmd.g = data[offset + 10];
      cmd.b = data[offset + 11];
    }
    seq.cmds.push_back(std::move(cmd));
    offset += len;
    if (op == 0x21) break;
  }
  return !seq.cmds.empty();
}

// The battle loop runs one game frame per kFrameTicks 60 Hz ticks; a per-frame fraction of the distance left, as the same motion per tick.
constexpr float kFrameTicks = 2.0f;
float per_tick(float per_frame) {
  if (per_frame >= 1.0f) return 1.0f;
  return 1.0f - std::pow(1.0f - std::max(0.0f, per_frame), 1.0f / kFrameTicks);
}

}  // namespace

bool battle_audit() {
  static const bool on = std::getenv("OC_BATTLE_AUDIT") != nullptr;
  return on;
}
void battle_audit_note(const std::string& what) {
  if (battle_audit()) log::warn("AUDIT {}", what);
}

void CamBank::add_drp(ByteSpan drp, bool replace) {
  auto parsed = import::parse_drp(drp);
  if (!parsed) return;
  for (const import::DrpEntry& entry : parsed->entries) {
    if (entry.type == 0x01 && import::is_drp(entry.data)) {
      add_drp(entry.data, replace);
    } else if (entry.type == 0x0C) {
      assets::StageCamera cam;
      if (parse_camera(entry, cam) && (replace || !cams_.count(entry.name))) cams_[entry.name] = std::move(cam);
    } else if (entry.type == 0x10) {
      CamSeq seq;
      if (parse_seq(entry, seq) && (replace || !seqs_.count(entry.name))) seqs_[entry.name] = std::move(seq);
    }
  }
}

const assets::StageCamera* CamBank::camera(const std::string& name) const {
  auto it = cams_.find(name);
  if (it != cams_.end()) return &it->second;
  return parent_ ? parent_->camera(name) : nullptr;
}
const CamSeq* CamBank::sequence(const std::string& name) const {
  auto it = seqs_.find(name);
  if (it != seqs_.end()) return &it->second;
  return parent_ ? parent_->sequence(name) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------- shots
// Progress 0..1 of a shot -> eased 0..1: 0 linear, 1 sine ease-out, 2 cosine ease-in, 3 sine half-steps, 4 cosine S-curve.
float BattleCamera::ease(int mode, float progress) {
  progress = std::clamp(progress, 0.0f, 1.0f);
  switch (mode) {
    case 1: return std::sin(progress * kPi * 0.5f);
    case 2: return 1.0f - std::cos(progress * kPi * 0.5f);
    case 3:
      if (progress <= 0.5f) return 0.5f * std::sin(progress * kPi);
      return 1.0f - 0.5f * std::sin(progress * kPi);
    case 4: return 0.5f - 0.5f * std::cos(progress * kPi);
    default: return progress;
  }
}

// Key smoothing. A shot's keys form three tracks (eye, target, H + roll); each is smoothed by a uniform cubic B-spline: every window of four
// consecutive keys yields 8 points through the 4.14 weights below (row 0 / 1 / 2 / 3 / 4 = first / second / middle / second-last / last window,
// each row summing to 0x4000), then the last key is appended raw. 2 keys stay a plain line; 3 and 4 keys are padded with the last key to 5.
// Every point of a track is then stamped with its cumulative chord length / total * 0x1000, the master track (w8 low half: 1 = target,
// 2 = H / roll, else eye) lends its stamps to the other two, and eval() interpolates linearly in that stamp time at the eased progress.
// The curve approximates the interior keys rather than passing through them.
namespace {
constexpr int kBasis[5][8][4] = {
    {{16384, 0, 0, 0}, {10976, 5048, 354, 5}, {6912, 8128, 1301, 42}, {4000, 9576, 2664, 144}, {2048, 9728, 4266, 341}, {864, 8920, 5933, 666}, {256, 7488, 7488, 1152}, {32, 5768, 8754, 1829}},
    {{4096, 9557, 2730, 0}, {2744, 9768, 3866, 5}, {1728, 9450, 5162, 42}, {1000, 8717, 6522, 144}, {512, 7679, 7850, 341}, {216, 6450, 9050, 666}, {64, 5141, 10026, 1152}, {8, 3863, 10682, 1829}},
    {{2730, 10922, 2730, 0}, {1829, 10682, 3866, 5}, {1152, 10026, 5162, 42}, {666, 9050, 6522, 144}, {341, 7850, 7850, 341}, {144, 6522, 9050, 666}, {42, 5162, 10026, 1152}, {5, 3866, 10682, 1829}},
    {{2730, 10922, 2730, 0}, {1829, 10682, 3863, 8}, {1152, 10026, 5141, 64}, {666, 9050, 6450, 216}, {341, 7850, 7679, 512}, {144, 6522, 8717, 1000}, {42, 5162, 9450, 1728}, {5, 3866, 9768, 2744}},
    {{2730, 9557, 4096, 0}, {1829, 8754, 5768, 32}, {1152, 7488, 7488, 256}, {666, 5933, 8920, 864}, {341, 4266, 9728, 2048}, {144, 2664, 9576, 4000}, {42, 1301, 8128, 6912}, {5, 354, 5048, 10976}}};
using Point3 = std::array<float, 3>;

std::vector<Point3> spline_track(std::vector<Point3> points) {
  size_t count = points.size();
  if (count <= 2) return points;
  if (count == 3) {
    points.push_back(points[2]);
    points.push_back(points[2]);
  } else if (count == 4) {
    points.push_back(points[3]);
  }
  count = points.size();
  std::vector<Point3> out;
  for (size_t window = 0; window + 4 <= count; ++window) {
    const int row = window == 0 ? 0 : window + 4 == count ? 4 : window == 1 ? 1 : (window + 5 == count && count > 6) ? 3 : 2;
    for (int step = 0; step < 8; ++step) {
      Point3 point{0, 0, 0};
      for (int k = 0; k < 4; ++k)
        for (size_t axis = 0; axis < 3; ++axis) point[axis] += points[window + static_cast<size_t>(k)][axis] * static_cast<float>(kBasis[row][step][k]) / 16384.0f;
      out.push_back(point);
    }
  }
  out.push_back(points.back());
  return out;
}

// The chord-length stamps 0..4096 of a track (a degenerate track stamps 0 everywhere and 4096 on its last point).
std::vector<float> chord_times(const std::vector<Point3>& track) {
  std::vector<float> stamps(track.size(), 0.0f);
  std::vector<float> segment(track.size(), 0.0f);
  float total = 0;
  for (size_t i = 1; i < track.size(); ++i) {
    float dist2 = 0;
    for (size_t axis = 0; axis < 3; ++axis) dist2 += (track[i][axis] - track[i - 1][axis]) * (track[i][axis] - track[i - 1][axis]);
    segment[i] = std::sqrt(dist2);
    total += segment[i];
  }
  float acc = 0;
  for (size_t i = 1; i < track.size(); ++i) {
    if (total <= 0.0f) {
      stamps[i] = i + 1 == track.size() ? 4096.0f : 0.0f;
      continue;
    }
    acc += segment[i];
    stamps[i] = acc * 4096.0f / total;
  }
  return stamps;
}
}  // namespace

CamPose BattleCamera::eval(const assets::StageCamera& shot, float eased) {
  CamPose pose;
  const auto& keys = shot.keys;
  auto pose_of_key = [](const assets::StageCameraKey& key) {
    CamPose out;
    out.eye = key.eye;
    out.target = key.target;
    out.h = key.projection;
    out.roll_deg = key.roll_deg;
    return out;
  };
  if (keys.empty()) return pose;
  if (keys.size() == 1 || eased <= 0) return pose_of_key(keys.front());
  std::vector<Point3> eye, target, h_roll;
  for (const auto& key : keys) {
    eye.push_back(key.eye);
    target.push_back(key.target);
    h_roll.push_back({key.projection, static_cast<float>(key.roll_raw), 0.0f});
  }
  eye = spline_track(std::move(eye));
  target = spline_track(std::move(target));
  h_roll = spline_track(std::move(h_roll));
  const u32 master = shot.w8 & 0xFFFFu;
  const std::vector<float> stamps = chord_times(master == 1 ? target : master == 2 ? h_roll : eye);
  // The engine evaluates at 0..0xFFF of 0x1000 (the task pins its progress at 0xFFF), so the last stamp is never reached exactly.
  const float time = std::min(eased, 4095.0f / 4096.0f) * 4096.0f;
  size_t seg = 0;
  while (seg + 2 < stamps.size() && time >= stamps[seg + 1]) ++seg;
  float frac = 0;
  if (time >= stamps.back()) {
    seg = stamps.size() - 2;
    frac = 1.0f;
  } else if (stamps[seg + 1] > stamps[seg]) {
    frac = std::clamp((time - stamps[seg]) / (stamps[seg + 1] - stamps[seg]), 0.0f, 1.0f);
  }
  auto lerp_axis = [&](const std::vector<Point3>& track, size_t axis) { return track[seg][axis] + (track[seg + 1][axis] - track[seg][axis]) * frac; };
  for (size_t axis = 0; axis < 3; ++axis) {
    pose.eye[axis] = lerp_axis(eye, axis);
    pose.target[axis] = lerp_axis(target, axis);
  }
  pose.h = lerp_axis(h_roll, 0);
  pose.roll_deg = lerp_axis(h_roll, 1) * 360.0f / 1024.0f;
  return pose;
}

// Selecting a shot. A shot whose flags bits 1-3 are 2 is dropped while any camera task is alive. Every other select ends a glide, records the
// name and, unless its mode is 1, cuts the pose to its first key placed in its frame. A shot of 2+ keys then becomes a task: mode 1 queues it
// at the tail of the list (the head when the list is empty, still without touching the pose), any other mode pushes the running head back (it
// ends or resumes on its own progress) and takes the head. A single-key shot never creates a task and leaves the list as it is.
bool BattleCamera::select(const std::string& name) {
  if (!bank_) {
    battle_audit_note("camera select '" + name + "': no camera bank");
    return false;
  }
  const assets::StageCamera* shot = bank_->camera(name);
  if (!shot || shot->keys.empty()) {
    battle_audit_note("camera select '" + name + "': shot missing from the bank");
    return false;
  }
  const unsigned select_mode = (shot->flags >> 1) & 7u;
  if (select_mode == 2u && task_running()) {
    log::debug("camera: select '{}' dropped (mode-2 shot while '{}' runs)", name, shot_ ? shot_->name : bg_.front().shot->name);
    return false;
  }
  glide_rate_ = 0;
  last_name_ = name;
  CamFrame frame;
  float scale = 1.0f;
  make_frame(*shot, frame, scale);
  CamPose goal;
  const bool placed = place(*shot, 0.0f, frame, scale, goal);
  if (std::getenv("OC_CAM_KEYS"))
    for (const auto& key : shot->keys)
      log::debug("camera:   key eye {:.0f} {:.0f} {:.0f} tgt {:.0f} {:.0f} {:.0f} H {:.0f} roll {:.1f}", key.eye[0], key.eye[1], key.eye[2], key.target[0], key.target[1], key.target[2], key.projection, key.roll_deg);
  {
    const auto& first_key = shot->keys.front();
    log::debug("camera: select '{}' ref {}/{}/{}/{} flags {:x} dur {} mode {} w8 {:x} keys {} key0 eye {:.0f} {:.0f} {:.0f} tgt {:.0f} {:.0f} {:.0f} H {:.0f}; frame {} ({:.0f} {:.0f} {:.0f} yaw {:.2f} pitch {:.2f} x{:.2f} m{:.3f}) -> eye {:.0f} {:.0f} {:.0f} tgt {:.0f} {:.0f} {:.0f}",
               name, shot->ref[0], shot->ref[1], shot->ref[2], shot->ref[3], shot->flags, shot->duration, shot->mode, shot->w8, shot->keys.size(), first_key.eye[0], first_key.eye[1], first_key.eye[2], first_key.target[0],
               first_key.target[1], first_key.target[2], first_key.projection, frame.valid ? 1 : 0, frame.x, frame.y, frame.z, frame.yaw, frame.pitch, scale, frame.mscale, goal.eye[0], goal.eye[1],
               goal.eye[2], goal.target[0], goal.target[1], goal.target[2]);
  }
  if (select_mode != 1u && (placed || !shot->ref[0])) {
    prev_ = cur_ = goal;
    lift_ = false;
  }
  if (retired_) {
    retired_ = false;
    kill_shot();
  }
  if (shot->keys.size() < 2) return true;
  const bool list_alive = task_running();
  if (select_mode == 1u && list_alive) {
    Task task;
    task.shot = shot;
    task.frozen = frame;
    task.frozen_scale = scale;
    bg_.push_back(task);
    log::debug("camera: select '{}' queued behind '{}' (mode 1)", name, shot_ ? shot_->name : bg_.front().shot->name);
    return true;
  }
  if (shot_ && progress_ < 4096.0f) {
    Task task;
    task.shot = shot_;
    task.progress = progress_;
    task.frozen = frozen_;
    task.frozen_scale = frozen_scale_;
    bg_.insert(bg_.begin(), task);
  }
  shot_ = shot;
  progress_ = 0;
  frozen_ = frame;
  frozen_scale_ = scale;
  return true;
}

bool BattleCamera::key_pose(const std::string& name, CamPose& out) const {
  const assets::StageCamera* shot = bank_ ? bank_->camera(name) : nullptr;
  if (!shot || shot->keys.empty()) return false;
  CamFrame frame;
  float scale = 1.0f;
  make_frame(*shot, frame, scale);
  return place(*shot, 0.0f, frame, scale, out) || !shot->ref[0];
}

void BattleCamera::write_pose(const CamPose& pose, bool cut) {
  glide_rate_ = 0;
  lift_ = false;
  cur_ = pose;
  if (cut) prev_ = pose;
}

void BattleCamera::glide_to(const CamPose& goal, int rate) {
  glide_goal_ = goal;
  glide_rate_ = rate;
  cur_.roll_deg = 0.0f;
}

bool BattleCamera::play_sequence(const std::string& name) {
  if (!bank_) return false;
  const CamSeq* seq = bank_->sequence(name);
  if (!seq) {
    battle_audit_note("camera play_sequence '" + name + "': timeline missing from the bank");
    return false;
  }
  for (const CamCmd& cmd : seq->cmds) log::debug("camera: sequence {} op {:#x} delay {} name {} frames {} mode {} rgb {} {} {}", name, cmd.op, cmd.delay, cmd.name, cmd.frames, cmd.mode, cmd.r, cmd.g, cmd.b);
  Runner runner;
  runner.seq = seq;
  runners_.push_back(runner);
  return true;
}

void BattleCamera::start_fade(float to, int frames, std::array<u8, 3> rgb) {
  fade_to_ = to;
  fade_left_ = std::max(1, frames);
  fade_rgb_ = rgb;
}

// The frame of a shot's ref bytes (type, attach, type, attach). One anchor: that unit's frame. Two anchors: the origin is their midpoint, local
// +X points from the first toward the second (yaw) and is pitched toward it (dy in the PS1 sense, y down), and the x / z axes are scaled by
// distance / (2048 cos pitch); a second anchor without a frame counts as the origin. The model scale of flag 0x20 is defined for single-anchor
// frames only.
bool BattleCamera::make_frame(const assets::StageCamera& shot, CamFrame& frame, float& scale) const {
  scale = 1.0f;
  frame = {};
  const int type0 = shot.ref[0], attach0 = shot.ref[1], type1 = shot.ref[2], attach1 = shot.ref[3];
  if (!type0 || !anchor_) return false;
  const CamFrame frame0 = anchor_(type0, attach0);
  if (!frame0.valid) {
    battle_audit_note("camera anchor type " + std::to_string(type0) + " attach " + std::to_string(attach0) + " of the selected shot is invalid (no such unit)");
    return false;
  }
  frame = frame0;
  if (type1) {
    CamFrame frame1 = anchor_(type1, attach1);
    // A missing second anchor is the world origin.
    if (!frame1.valid) {
      battle_audit_note("camera second anchor type " + std::to_string(type1) + " attach " + std::to_string(attach1) + " is invalid: the origin stands in");
      frame1 = {};
    }
    frame.x = (frame0.x + frame1.x) * 0.5f;
    frame.y = (frame0.y + frame1.y) * 0.5f;
    frame.z = (frame0.z + frame1.z) * 0.5f;
    const float dx = frame1.x - frame0.x, dz = frame1.z - frame0.z, dy = -(frame1.y - frame0.y);
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    frame.yaw = -std::atan2(dz, dx);
    frame.pitch = std::atan2(dy, horizontal);
    frame.mscale = 1.0f;
    scale = horizontal / 2048.0f / std::max(0.05f, std::cos(frame.pitch));
  }
  if (!(shot.flags & 0x20u)) frame.mscale = 1.0f;
  return true;
}

// The shot's pose at its current progress, placed into its frame: the local eye / target are scaled (x, z by the two-anchor distance scale,
// all three by the model scale), rotated by Ry * Rz of the frame in the PS1 sense (y down) and translated to the frame origin in our y-up world.
void BattleCamera::goal_pose(CamPose& out) const {
  if (shot_) place(*shot_, progress_, frozen_, frozen_scale_, out);
}

bool BattleCamera::place(const assets::StageCamera& shot, float progress, CamFrame frame, float scale, CamPose& out) const {
  const float eased = ease(shot.mode, std::min(progress, 4096.0f) / 4096.0f);
  out = eval(shot, eased);
  if (shot.flags & 1u) {
    if (!make_frame(shot, frame, scale)) return false;
  } else if (!frame.valid) {
    return false;
  }
  const float cos_yaw = std::cos(frame.yaw), sin_yaw = std::sin(frame.yaw);
  const float cos_pitch = std::cos(frame.pitch), sin_pitch = std::sin(frame.pitch);
  auto to_world = [&](std::array<float, 3>& point) {
    const float x = point[0] * scale * frame.mscale, z = point[2] * scale * frame.mscale, y = -point[1] * frame.mscale;
    const float x_pitched = x * cos_pitch - y * sin_pitch, y_pitched = x * sin_pitch + y * cos_pitch;
    point = {frame.x + x_pitched * cos_yaw + z * sin_yaw, frame.y - y_pitched, frame.z - x_pitched * sin_yaw + z * cos_yaw};
  };
  to_world(out.eye);
  to_world(out.target);
  return true;
}

// One timeline command other than the shot picks (0x23) and the end (0x21), which run() handles itself.
void BattleCamera::exec(const CamCmd& cmd, CamEvents& events) {
  switch (cmd.op) {
    case 0x22:  // start a stage emitter by name
      if (!cmd.name.empty() && emitter_hook_) emitter_hook_(cmd.name);
      break;
    case 0x2B:  // screen colour layer: fade in (from the colour to the scene)
    case 0x2C: {  // screen colour layer: fade out
      if (screen_hook_) {
        screen_hook_(cmd);
        break;
      }
      // Without a view the layers collapse to one fade value: colour 0 draws nothing; ABR mode 3 subtracts the colour (a fade to black, 255 = all
      // the way); other modes blend toward the colour; a black mode-0 fade out clears the layer. Intro scripts open with 0x2B on black.
      const bool sub = cmd.mode == 3, clear = !sub && cmd.op == 0x2C && cmd.r == 0 && cmd.g == 0 && cmd.b == 0;
      events.fade_target = sub ? static_cast<float>(std::max({cmd.r, cmd.g, cmd.b})) / 255.0f : clear ? 0.0f : cmd.op == 0x2C ? 1.0f : 0.0f;
      events.fade_frames = std::max<int>(1, cmd.frames);
      events.fade_rgb = sub || clear ? std::array<u8, 3>{0, 0, 0} : std::array<u8, 3>{cmd.r, cmd.g, cmd.b};
      fade_rgb_ = events.fade_rgb;
      if (!sub && cmd.op == 0x2B && fade_cur_ < 0.5f && fade_left_ == 0) fade_prev_ = fade_cur_ = 1.0f;
      fade_to_ = events.fade_target;
      fade_left_ = events.fade_frames;
      break;
    }
    case 0x2F:  // start another timeline of the bank; it steps from the next tick on
      if (const CamSeq* child = bank_ && !cmd.name.empty() ? bank_->sequence(cmd.name) : nullptr) {
        Runner runner;
        runner.seq = child;
        spawned_.push_back(runner);
      }
      break;
    case 0x3E: case 0x3F: case 0x40:  // the screen copy warp ("frame afterimage"), drawn by the view
      if (warp_hook_) warp_hook_(cmd);
      break;
    case 0x4C:  // sound by tag
      if (!cmd.name.empty()) events.sounds.push_back(cmd.name);
      break;
    case 0x73:  // intro hold: the party intro waits for this timeline's shot
      intro_hold_ = true;
      break;
    default: break;
  }
}

bool BattleCamera::next_target(Runner& runner) const {
  if (!(runner.seq->flags & 1u) || !target_hook_ || !target_hook_(runner.target + 1)) return false;
  ++runner.target;
  return true;
}

// One tick of a timeline: the wait counter takes each command's delay before the next one executes; consecutive 0x23 commands (and the
// non-0x23 commands between them) up to the first non-zero delay form one group, of which one pick is selected at random.
// A jump back for the next target (0x4D, or the step after the end) and a stall end the tick with the wait reset, the step's overshoot dropped:
// a jump runs its loop body on the next tick, a stall (0x50) or a yield (0x67) waits exactly one tick whatever its delay.
void BattleCamera::run(Runner& runner, CamEvents& events) {
  --runner.wait;
  if (runner.wait > 0) return;
  const int overshoot = -runner.wait;
  runner.wait = 0;
  if (runner.ended) {
    runner.ended = false;
    if (next_target(runner)) runner.pos = runner.loop_pos;
    else runner.seq = nullptr;
    return;
  }
  while (runner.seq && runner.wait == 0) {
    if (runner.pos >= runner.seq->cmds.size()) {
      runner.seq = nullptr;
      break;
    }
    const CamCmd& cmd = runner.seq->cmds[runner.pos];
    runner.wait += cmd.delay;
    if (cmd.op == 0x21) {
      // A per-target timeline is restarted or dropped by the next step.
      if (runner.seq->flags & 1u) {
        runner.ended = true;
        return;
      }
      runner.seq = nullptr;
      break;
    }
    if (cmd.op == 0x4E) {
      runner.loop_pos = runner.pos + 1;
      runner.target = 0;
      ++runner.pos;
      continue;
    }
    if (cmd.op == 0x4D && next_target(runner)) {
      runner.pos = runner.loop_pos;
      runner.wait = 0;
      return;
    }
    if (cmd.op == 0x50 && actor_busy_hook_ && actor_busy_hook_()) {
      runner.wait = 1;
      return;
    }
    if (cmd.op == 0x67) {
      ++runner.pos;
      runner.wait = 1;
      return;
    }
    if (cmd.op == 0x23) {
      std::vector<const CamCmd*> picks;
      size_t end = runner.pos;
      for (; end < runner.seq->cmds.size(); ++end) {
        const CamCmd& member = runner.seq->cmds[end];
        if (member.op == 0x23) picks.push_back(&member);
        else if (member.op != 0x21) exec(member, events);
        if (member.delay != 0 || member.op == 0x21) break;
      }
      if (end >= runner.seq->cmds.size()) end = runner.seq->cmds.size() - 1;
      if (end != runner.pos) runner.wait += runner.seq->cmds[end].delay;
      if (!picks.empty()) {
        const CamCmd* pick = picks[static_cast<size_t>(std::rand()) % picks.size()];
        if (const char* want = std::getenv("OC_CAM_PICK"))
          for (const CamCmd* candidate : picks)
            if (candidate->name == want) pick = candidate;
        select(pick->name);
      }
      runner.pos = end + 1;
    } else {
      exec(cmd, events);
      ++runner.pos;
    }
  }
  runner.wait -= overshoot;
}

// One 60 Hz tick: the screen fade, every timeline, a glide, then the shot list. Every task advances its progress step; the tasks behind the head are
// unlinked silently once past the end, the head's eye / target chase its goal by smoothing / 256 (w8 high half) per game frame, then an anchored shot without
// flag bit 4 clamps the eye to the ground and flag bit 6 pulls the eye in to 6144 from the world origin. When the head ends the next task of
// the list resumes at its own progress; an ended head with no task behind it holds the pose registers.
void BattleCamera::tick(CamEvents& events) {
  fade_prev_ = fade_cur_;
  if (fade_left_ > 0) {
    fade_cur_ += (fade_to_ - fade_cur_) / static_cast<float>(fade_left_);
    --fade_left_;
  }

  for (size_t i = 0; i < runners_.size(); ++i) run(runners_[i], events);
  std::erase_if(runners_, [](const Runner& runner) { return runner.seq == nullptr; });
  runners_.insert(runners_.end(), spawned_.begin(), spawned_.end());
  spawned_.clear();

  prev_ = cur_;
  // The glide steps once per game frame (rate / 256 of the distance left); it ends when such a step times the rate is under 256 for both
  // eye and target. Spread over the 60 Hz ticks of a frame.
  if (glide_rate_ > 0) {
    const float frame_rate = static_cast<float>(glide_rate_) / 256.0f, rate = per_tick(frame_rate);
    float eye_left = 0, target_left = 0;
    for (size_t i = 0; i < 3; ++i) {
      cur_.eye[i] += (glide_goal_.eye[i] - cur_.eye[i]) * rate;
      cur_.target[i] += (glide_goal_.target[i] - cur_.target[i]) * rate;
      eye_left += (glide_goal_.eye[i] - cur_.eye[i]) * (glide_goal_.eye[i] - cur_.eye[i]);
      target_left += (glide_goal_.target[i] - cur_.target[i]) * (glide_goal_.target[i] - cur_.target[i]);
    }
    cur_.h += (glide_goal_.h - cur_.h) * rate;
    lift_ = false;
    const float rate_units = static_cast<float>(glide_rate_);
    if (std::sqrt(eye_left) * frame_rate * rate_units < 256.0f && std::sqrt(target_left) * frame_rate * rate_units < 256.0f) glide_rate_ = 0;
  }

  if (shot_) {
    for (Task& task : bg_) task.progress = std::min(4096.0f, task.progress + static_cast<float>(progress_step(*task.shot)));
    std::erase_if(bg_, [](const Task& task) { return task.progress >= 4096.0f; });
    const bool was_running = progress_ < 4096.0f;
    if (!was_running) return;
    progress_ = std::min(4096.0f, progress_ + static_cast<float>(progress_step(*shot_)));
    CamPose goal;
    goal_pose(goal);
    // The chase runs once per game frame: smoothing / 256 per frame, spread over its 60 Hz ticks (0x100 = exact).
    const float smoothing = per_tick(std::min(1.0f, static_cast<float>((shot_->w8 >> 16) & 0xFFFF) / 256.0f));
    for (size_t i = 0; i < 3; ++i) {
      cur_.eye[i] += (goal.eye[i] - cur_.eye[i]) * smoothing;
      cur_.target[i] += (goal.target[i] - cur_.target[i]) * smoothing;
    }
    cur_.h = goal.h;
    cur_.roll_deg = goal.roll_deg;
    lift_ = !(shot_->flags & 0x10u) && shot_->ref[0] != 0;
    if (lift_ && cur_.eye[1] < 0) cur_.eye[1] = 0;
    if (shot_->flags & 0x40u) {
      const float radius = std::sqrt(cur_.eye[0] * cur_.eye[0] + cur_.eye[2] * cur_.eye[2]);
      if (radius > 6144.0f) {
        cur_.eye[0] *= 6144.0f / radius;
        cur_.eye[2] *= 6144.0f / radius;
      }
    }
    if (was_running && progress_ >= 4096.0f && !bg_.empty()) {
      Task next = bg_.front();
      bg_.erase(bg_.begin());
      shot_ = next.shot;
      progress_ = next.progress;
      frozen_ = next.frozen;
      frozen_scale_ = next.frozen_scale;
    }
    static const bool trace = std::getenv("OC_CAM_TRACE") != nullptr;
    if (trace)
      log::debug("camera: pose '{}' p {:.0f} eye {:.0f} {:.0f} {:.0f} tgt {:.0f} {:.0f} {:.0f} H {:.0f}", shot_->name, progress_, cur_.eye[0], cur_.eye[1], cur_.eye[2],
                 cur_.target[0], cur_.target[1], cur_.target[2], cur_.h);
  }
}

CamPose BattleCamera::pose(float alpha) const {
  if (has_free_) return free_;
  CamPose pose;
  for (size_t i = 0; i < 3; ++i) {
    pose.eye[i] = prev_.eye[i] + (cur_.eye[i] - prev_.eye[i]) * alpha;
    pose.target[i] = prev_.target[i] + (cur_.target[i] - prev_.target[i]) * alpha;
  }
  pose.h = prev_.h + (cur_.h - prev_.h) * alpha;
  pose.roll_deg = prev_.roll_deg + (cur_.roll_deg - prev_.roll_deg) * alpha;
  return pose;
}

}  // namespace oc::game
