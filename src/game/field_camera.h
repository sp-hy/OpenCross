#pragma once
// FieldCamera: the field scroll state - the scroll position, the scripted camera target and its per-tick step, and the pan limits - plus
// the rules that move it and clamp it to the room. It holds no room or entity data of its own: the caller passes the room view, the
// script global flags and the poses of the entities to aim at.
#include <utility>

#include "core/types.h"
#include "game/actor.h"
#include "game/room_view.h"

namespace oc::game {

struct FieldCamera {
  // The wait forms of the scripted camera moves (5D, 5E, 61).
  enum class Wait { Position, Actor, Follow };

  struct Camera {
    // scroll position
    int x = 0, y = 0;
    // A script owns the camera target (5D / 5E / 61): the scroll eases toward `tx, ty` instead of an entity.
    bool scripted = false;
    // scripted target and its per-tick step, 16.16 fixed point
    s32 tx = 0, ty = 0, dx = 0, dy = 0;
    // ticks left of the scripted move; below 0 the move is over
    int frames = -1;
    bool has_limits = false;
    RoomView::Limits limits;
  };

  Camera cam;
  float camera_y_offset = 0;

  void reset();
  // the active pan limits, widened for widescreen
  RoomView::Limits scroll_limits(const RoomView& view) const;
  int clamp_x(const RoomView& view, u32 global_flags, int value) const;
  int clamp_y(const RoomView& view, u32 global_flags, int value) const;
  // The scroll that centres `pose` raised by `y_offset`, not clamped; false when there is no such entity.
  bool aim(const RoomView& view, const ActorPose* pose, float y_offset, int& tx, int& ty) const;
  void set_position(s32 x, s32 y, int frames);
  // `follow_pose` / `override_follow` as for advance: a move of 0 frames hands the camera back at once and runs one extra ease.
  void to_actor(const RoomView& view, u32 global_flags, const ActorPose* follow_pose, bool override_follow, int frames);
  void follow(const RoomView& view, u32 global_flags, const ActorPose* pose, int frames);
  // One tick of a wait form; true when the move is over. Position re-targets `x, y` (the start form's operands read again).
  bool wait(Wait kind, s32 x, s32 y, const RoomView& view, u32 global_flags, const ActorPose* follow_pose, bool override_follow);
  void set_limits(s32 x0, s32 x1, s32 y0, s32 y1);
  std::pair<s32, s32> position() const { return {cam.x, cam.y}; }
  void set_origin(s32 x, s32 y);
  // Room entry: the ease run to the player's spot, without the display step.
  void snap(const RoomView& view, u32 global_flags, const ActorPose* player_pose);
  // One tick of the scroll ease, before the shake offset. `follow_pose` = the controlled entity (`override_follow`: a script chose it,
  // which takes precedence over a scripted target) or the party leader.
  void advance(const RoomView& view, u32 global_flags, const ActorPose* follow_pose, bool override_follow);

 private:
  void start_move(int to_x, int to_y, int frames);
};

}  // namespace oc::game
