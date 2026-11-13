// FieldCamera: scroll clamping, the scripted camera target, the follow ease and the room-entry snap.
#include "game/field_camera.h"

#include <algorithm>
#include <cmath>

namespace oc::game {

namespace {

// Script global-flag bits: GF.1 the input lock, FE 04 disables the camera pan-limit clamp, FE 11 snaps the camera.
constexpr u32 kGfInputLock = 0x2, kGfNoCameraClamp = 0x100, kGfCameraSnap = 0x200;
// Scroll easing: S -= (S - T) / 12 per tick.
constexpr int kEaseDivisor = 12;
constexpr int kEntryEaseIterations = 64;
// A frame count of 0x7FFF makes the move last as many ticks as the distance in pixels.
constexpr int kFramesFromDistance = 0x7FFF;

// ScrollSmoother: a critically damped alpha-beta pair (beta = alpha^2 / (2 - alpha)); the gain of the step-rate average; how far
// behind the scroll the filter aims while moving and the step rate at which that offset is complete; how far the display may trail the
// integer scroll; the jump that is taken at once.
constexpr float kSmoothAlpha = 0.15f;
constexpr float kSmoothBeta = kSmoothAlpha * kSmoothAlpha / (2.0f - kSmoothAlpha);
constexpr float kRateGain = 0.2f;
constexpr float kAimBehind = 0.75f;
constexpr float kFullOffsetRate = 0.25f;
constexpr float kMaxTrail = 2.0f;
constexpr float kSmoothCut = 6.0f;

}  // namespace

// While the scroll moves, the filter aims a little behind it, inside its current whole-pixel step, fading to the scroll itself as the
// step rate falls, so a stopping camera settles forward onto the scroll. The result never passes the scroll in its direction of travel,
// never trails it by more than kMaxTrail (held there, it takes the step rate as its speed) and never moves against that direction: no
// overshoot at a pan limit and no creeping back.
void ScrollSmoother::Axis::step(int to) {
  const int moved = to - last;
  if (moved != 0) dir = moved > 0 ? 1 : -1;
  rate += kRateGain * (static_cast<float>(moved) - rate);
  last = to;
  const float target = static_cast<float>(to), before = pos, predicted = pos + vel;
  if (std::abs(target - predicted) > kSmoothCut) {
    pos = target;
    vel = rate = static_cast<float>(moved);
    return;
  }
  const float aim = target - static_cast<float>(dir) * kAimBehind * std::min(1.0f, std::abs(rate) / kFullOffsetRate);
  const float residual = aim - predicted;
  vel += kSmoothBeta * residual;
  pos = predicted + kSmoothAlpha * residual;
  if (dir == 0) return;
  const float forward = static_cast<float>(dir);
  if ((pos - before) * forward < 0) {
    pos = before;
    vel = 0;
  }
  const float ahead = (pos - target) * forward;
  if (ahead > 0) {
    pos = target;
  } else if (ahead < -kMaxTrail) {
    pos = target - forward * kMaxTrail;
    if (vel * forward < rate * forward) vel = rate;
  }
}

void ScrollSmoother::reset(int to_x, int to_y) {
  ax = {static_cast<float>(to_x), 0, 0, to_x, 0};
  ay = {static_cast<float>(to_y), 0, 0, to_y, 0};
}

void ScrollSmoother::step(int to_x, int to_y) {
  ax.step(to_x);
  ay.step(to_y);
}

void FieldCamera::reset() {
  cam = Camera{};
  camera_y_offset = 0;
  smooth.reset(0, 0);
}

RoomView::Limits FieldCamera::scroll_limits(const RoomView& view) const {
  return RoomView::widen(cam.has_limits ? cam.limits : view.default_limits(), static_cast<int>(render::wide_extra()));
}
int FieldCamera::clamp_x(const RoomView& view, u32 global_flags, int value) const {
  // FE 04 skips every pan-limit clamp.
  if (global_flags & kGfNoCameraClamp) return value;
  const RoomView::Limits limits = scroll_limits(view);
  return std::clamp(value, std::min(limits.x_min, limits.x_max), std::max(limits.x_min, limits.x_max));
}
int FieldCamera::clamp_y(const RoomView& view, u32 global_flags, int value) const {
  if (global_flags & kGfNoCameraClamp) return value;
  const RoomView::Limits limits = scroll_limits(view);
  return std::clamp(value, std::min(limits.y_min, limits.y_max), std::max(limits.y_min, limits.y_max));
}

bool FieldCamera::aim(const RoomView& view, const ActorPose* pose, float y_offset, int& tx, int& ty) const {
  if (!pose) return false;
  render::FieldCamera::Projected pr;
  if (!view.camera().project({pose->x, pose->y + y_offset, pose->z}, pr)) return false;
  // Target = (160 - sx, 120 - sy) on the whole screen pixel the point falls in.
  tx = 160 - static_cast<int>(std::floor(pr.sx));
  ty = 120 - static_cast<int>(std::floor(pr.sy));
  return true;
}

// A timed move starts the scripted target on the current scroll and steps it linearly ((to - from) / frames + 1 per tick, 16.16) for
// `frames` ticks; the scroll itself keeps easing toward it, so it lags behind and stops short where the ease stalls.
void FieldCamera::start_move(int to_x, int to_y, int frames) {
  cam.tx = cam.x * 65536;
  cam.ty = cam.y * 65536;
  if (frames == kFramesFromDistance)
    frames = static_cast<int>(std::sqrt(static_cast<double>((to_x - cam.x) * (to_x - cam.x) + (to_y - cam.y) * (to_y - cam.y))));
  frames = std::max(1, frames);
  cam.dx = (to_x * 65536 - cam.tx) / frames + 1;
  cam.dy = (to_y * 65536 - cam.ty) / frames + 1;
  cam.frames = frames - 1;
}

void FieldCamera::set_position(s32 x, s32 y, int frames) {
  cam.scripted = true;
  if (frames == 0) {
    cam.tx = x * 65536;
    cam.ty = y * 65536;
    cam.frames = -1;
    return;
  }
  start_move(x, y, frames);
}

void FieldCamera::to_actor(const RoomView& view, u32 global_flags, const ActorPose* follow_pose, bool override_follow, int frames) {
  cam.scripted = true;
  if (frames == 0) {
    cam.dx = cam.dy = 0;
    cam.scripted = false;
    advance(view, global_flags, follow_pose, override_follow);
    return;
  }
  int tx = cam.x, ty = cam.y;
  aim(view, follow_pose, camera_y_offset, tx, ty);
  start_move(tx, ty, frames);
}

void FieldCamera::follow(const RoomView& view, u32 global_flags, const ActorPose* pose, int frames) {
  cam.scripted = true;
  int tx, ty;
  if (!aim(view, pose, 0.0f, tx, ty)) return;
  tx = clamp_x(view, global_flags, tx);
  ty = clamp_y(view, global_flags, ty);
  if (frames == 0) {
    cam.tx = tx * 65536;
    cam.ty = ty * 65536;
    return;
  }
  start_move(tx, ty, frames);
}

bool FieldCamera::wait(Wait kind, s32 x, s32 y, const RoomView& view, u32 global_flags, const ActorPose* follow_pose, bool override_follow) {
  if (cam.frames >= 0) {
    cam.tx += cam.dx;
    cam.ty += cam.dy;
    --cam.frames;
    return false;
  }
  cam.dx = cam.dy = 0;
  switch (kind) {
    case Wait::Position:
      cam.tx = x * 65536;
      cam.ty = y * 65536;
      break;
    case Wait::Actor:
      // The camera goes back to following, with one extra ease on the spot.
      cam.scripted = false;
      advance(view, global_flags, follow_pose, override_follow);
      break;
    case Wait::Follow:
      break;
  }
  return true;
}

void FieldCamera::set_limits(s32 x0, s32 x1, s32 y0, s32 y1) {
  cam.has_limits = true;
  cam.limits = {x0, x1, y0, y1};
}

// FE 50: only the scroll position is written from variables 0x330 / 0x332; the follow logic of the field loop
// goes on (the world-map Continue, ROOM_PARAM 6), so the camera target stays as it is.
void FieldCamera::set_origin(s32 x, s32 y) {
  cam.x = x;
  cam.y = y;
}

// Room entry: INIT's end eases the scroll 0x40 times from 0 toward the leader (skipped when FE 50 restored a saved scroll), then the
// start of the field loop runs that ease twice more from wherever the scroll is. From FE 50's scroll the camera so comes to rest on
// the side of the target it was saved on (the ease stalls short of it).
void FieldCamera::snap(const RoomView& view, u32 global_flags, const ActorPose* player_pose, bool keep_scroll) {
  cam.scripted = false;
  cam.frames = -1;
  if (!keep_scroll) cam.x = cam.y = 0;
  int tx = cam.x, ty = cam.y;
  aim(view, player_pose, camera_y_offset, tx, ty);
  tx = clamp_x(view, global_flags, tx);
  ty = clamp_y(view, global_flags, ty);
  const int passes = keep_scroll ? 2 : 3;
  for (int i = 0; i < passes * kEntryEaseIterations; ++i) {
    cam.x -= (cam.x - tx) / kEaseDivisor;
    cam.y -= (cam.y - ty) / kEaseDivisor;
  }
}

// The target, in order: the entity a script made the controlled one; the scripted target; while the input lock is on, the scroll itself
// (the camera holds still through a cutscene unless a script moves it); else the party leader. It is clamped to the pan limits, then the
// scroll eases toward it (C division, so it stalls short of a target less than 12 px away) or, with FE 11, jumps onto it.
void FieldCamera::advance(const RoomView& view, u32 global_flags, const ActorPose* follow_pose, bool override_follow) {
  int tx = cam.x, ty = cam.y;
  if (override_follow)
    aim(view, follow_pose, camera_y_offset, tx, ty);
  else if (cam.scripted) {
    tx = cam.tx >> 16;
    ty = cam.ty >> 16;
  } else if (!(global_flags & kGfInputLock))
    aim(view, follow_pose, camera_y_offset, tx, ty);
  tx = clamp_x(view, global_flags, tx);
  ty = clamp_y(view, global_flags, ty);
  if (global_flags & kGfCameraSnap) {
    cam.x = tx;
    cam.y = ty;
  } else {
    cam.x -= (cam.x - tx) / kEaseDivisor;
    cam.y -= (cam.y - ty) / kEaseDivisor;
  }
}

}  // namespace oc::game
