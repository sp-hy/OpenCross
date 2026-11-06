// FieldWorld: the entity script ops - per-entity updates, positions and headings, attach points, entity effects, push and attach
// stepping, and the extra animation packs.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "audio/voice.h"
#include "core/log.h"
#include "game/field_world_internal.h"
#include "import/model.h"

namespace oc::game {

using namespace field_internal;

namespace {

// SET_POSITION steps up to this many units per call are interpolated, larger ones are warps.
constexpr float kGlideMax = 150.0f;

}  // namespace

void FieldWorld::update_entity(int ent) {
  if (ent >= 0 && ent < static_cast<int>(ents_.size())) ents_[static_cast<size_t>(ent)].updating = true;
}

void FieldWorld::set_position_xz(int ent, s32 x, s32 z, int level) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  // A script that calls SET_POSITION every tick (flyers: the bats) moves the actor a few units per tick: the render interpolation must
  // glide over that step; only a real jump (first placement, a warp) resets the interpolation source.
  const ActorPose& c0 = ent_state.actor.current();
  const bool jump = !ent_state.placed || std::hypot(static_cast<float>(x) - c0.x, static_cast<float>(z) - c0.z) > kGlideMax;
  // Op 25's last operand is the floor level: the entity lands on the level-th walk-mesh floor under (x, z), as an entrance record's
  // 4th word does - not on the floor nearest its old height (gal_b14f's Kid, `25 .. #-396, #-563, #1`, stands on the beach far
  // below the jetty). With no floor under (x, z) the height becomes 0, not the height the entity had: exit boxes placed past the
  // walk-mesh border must stay in the vertical range of the player walking into them.
  if (const WalkMesh& walk = view_.walk(); !walk.empty()) {
    const int tri = walk.find_triangle_level(static_cast<float>(x), static_cast<float>(z), level);
    ent_state.actor.set_height(tri >= 0 ? walk.height(tri, static_cast<float>(x), static_cast<float>(z)) : 0.0f, jump);
  }
  ent_state.actor.set_position(view_.walk(), static_cast<float>(x), static_cast<float>(z), jump, false);
  ent_state.placed = true;
}

void FieldWorld::set_position_xyz(int ent, s32 x, s32 y, s32 z) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  const ActorPose& c0 = ent_state.actor.current();
  const bool jump = !ent_state.placed || std::hypot(static_cast<float>(x) - c0.x, static_cast<float>(z) - c0.z) > kGlideMax ||
                    std::fabs(static_cast<float>(y) - c0.y) > kGlideMax;
  ent_state.actor.set_position(view_.walk(), static_cast<float>(x), static_cast<float>(z), jump, true);
  ent_state.actor.set_height(static_cast<float>(y), jump);
  ent_state.placed = true;
  log::debug("field: tick {} ent {} SET_POSITION_XYZ {},{},{}", tick_count_, ent, x, z, y);
}

script::Vec3 FieldWorld::get_position(int ent) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return {};
  const ActorPose& pose = ents_[static_cast<size_t>(ent)].actor.current();
  return {static_cast<s32>(std::lround(pose.x)), static_cast<s32>(std::lround(pose.y)), static_cast<s32>(std::lround(pose.z))};
}

void FieldWorld::set_facing(int ent, int angle) {
  if (ent >= 0 && ent < static_cast<int>(ents_.size())) ents_[static_cast<size_t>(ent)].actor.set_facing(to_internal(angle));
}

int FieldWorld::get_facing(int ent) {
  return ent >= 0 && ent < static_cast<int>(ents_.size()) ? to_script(ents_[static_cast<size_t>(ent)].actor.facing()) : 0;
}

void FieldWorld::turn_to_entity(int ent, int target) {
  if (ent < 0 || target < 0 || ent >= static_cast<int>(ents_.size()) || target >= static_cast<int>(ents_.size())) return;
  const ActorPose& pose = ents_[static_cast<size_t>(ent)].actor.current();
  const ActorPose& target_pose = ents_[static_cast<size_t>(target)].actor.current();
  if (std::fabs(target_pose.x - pose.x) < 0.5f && std::fabs(target_pose.z - pose.z) < 0.5f) return;
  ents_[static_cast<size_t>(ent)].actor.set_facing(bearing(target_pose.x - pose.x, target_pose.z - pose.z));
}

void FieldWorld::face_point(int ent, s32 x, s32 z) {
  // CA: faces the point, only when the slot flag (op 47) is 0 and the turn rate is not 0.
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  const script::Entity& script_ent = vm_.entity(ent);
  if (script_ent.rec[script_ent.cur].flag47 != 0 || script_ent.v24e == 0) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  const ActorPose& pose = ent_state.actor.current();
  const float dx = static_cast<float>(x) - pose.x, dz = static_cast<float>(z) - pose.z;
  if (std::fabs(dx) < 0.5f && std::fabs(dz) < 0.5f) return;
  ent_state.actor.set_facing(bearing(dx, dz));
}

bool FieldWorld::rotation_busy(int ent) {
  // WAIT_ROTATION CB / CC: a pure angle test (shown facing != target facing), so it also waits through the tick or two before a
  // turn-rate-0 entity with the self-paced-turn flag gets its |turn| / 0x30 rate. With turn rate 0 and that flag clear the
  // yaw never moves and the engine waits for ever; that one case ends at once here (logged).
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  // CB: an entity standing on a tile with the slide / stair attribute (0xC000) does not wait at all - its facing is rewritten
  // from the plane normal every tick, so the script's SET_ROTATION target could never be reached (the world map's entity-4
  // rotations of wld_a00 / wld_b02 would poll for 860 ticks).
  if (ent_state.placed && !view_.walk().empty() && (walk_flags(ent) & 0xC000u)) return false;
  if (!ent_state.actor.turning()) return false;
  const script::Entity& script_ent = vm_.entity(ent);
  if (script_ent.v24e == 0 && !(script_ent.flags270 & 0x2000u)) {
    log::debug("field: tick {} ent {} WAIT_ROTATION with turn rate 0 and no scripted-turn bit: the engine would hang, ended", tick_count_, ent);
    return false;
  }
  return true;
}

void FieldWorld::set_animation(int ent, int anim) {
  if (ent >= 0 && ent < static_cast<int>(ents_.size())) ents_[static_cast<size_t>(ent)].actor.play(anim);
}

void FieldWorld::play_animation(int ent, int anim) { set_animation(ent, anim); }

void FieldWorld::set_walk_animation(int ent, int mode, int anim) {
  if (ent >= 0 && ent < static_cast<int>(ents_.size())) ents_[static_cast<size_t>(ent)].actor.set_anim_slot(mode, anim);
}

bool FieldWorld::animation_busy(int ent) {
  return ent >= 0 && ent < static_cast<int>(ents_.size()) && ents_[static_cast<size_t>(ent)].actor.anim_busy();
}

void FieldWorld::set_move_speed(int ent, int speed) {
  if (ent >= 0 && ent < static_cast<int>(ents_.size())) vm_.entity(ent).v10c = static_cast<u16>(speed << 4);
}

void FieldWorld::start_motion(int ent, const script::MotionRequest& req) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  Motion motion;
  motion.active = true;
  motion.kind = req.kind;
  motion.target_ent = req.target;
  const ActorPose& cur = ent_state.actor.current();
  motion.sx = cur.x;
  motion.sy = cur.y;
  motion.sz = cur.z;
  using K = script::MotionKind;
  switch (req.kind) {
    case K::Walk:
    case K::Walk2:
      motion.tx = static_cast<float>(req.x) + (req.mode == 2 ? cur.x : 0);
      motion.tz = static_cast<float>(req.z) + (req.mode == 2 ? cur.z : 0);
      motion.ty = static_cast<float>(req.y);
      motion.target_y = true;
      motion.walking = true;
      // 40's last operand U is the slot timer (3F stores nothing and runs on 0xFFFF). The wait form
      // steps while far and ends the wait, without a snap, on the step after the timer reached 0: U + 1 steps in all.
      // des_a03f's `40 .. #1` creeps two steps per run of the script instead of walking to the target.
      if (req.kind == K::Walk2 && req.params[0] > 0 && req.params[0] != 0x7FFF) motion.frames = req.params[0] + 1;
      break;
    case K::WalkXZ:
    case K::WalkXZ2:
      motion.tx = static_cast<float>(req.x) + (req.mode == 2 ? cur.x : 0);
      motion.tz = static_cast<float>(req.z) + (req.mode == 2 ? cur.z : 0);
      motion.walking = true;
      // 43: the slot timer U@7 ends the wait while the entity is still far (0x7FFF / 0 = none); 42 stores 0xFFFF (never).
      if (req.kind == K::WalkXZ2 && req.params[0] > 0 && req.params[0] != 0x7FFF) motion.frames = req.params[0];
      break;
    case K::FollowEnt:
    case K::FollowEnt3D:
    case K::FollowEntTimed:
      motion.walking = true;
      if (req.kind == K::FollowEntTimed) motion.frames = req.params[0] > 0 && req.params[0] != 0x7FFF ? req.params[0] : 0;
      break;
    case K::FollowEnt3DTimed:
      // B9's slot timer U is tested before it is decremented and the wait form steps first: U + 1 steps while far.
      motion.walking = true;
      motion.frames = req.params[0] + 1;
      break;
    case K::Move:
    case K::Move3D:
      motion.heading = to_internal(req.params[0]);
      // The slot counter (BA U@6, 46 U@4) is tested before it is decremented and the wait form steps first: N + 1 steps.
      motion.frames = req.kind == K::Move ? std::max(0, req.params[1]) + 1 : std::max(0, req.params[2]) + 1;
      motion.walking = true;
      break;
    case K::Tween:
    case K::Jump: {
      // MOVE_TWEEN 5B and JUMP_MOVE 5C start forms: a ballistic hop to (x, z) = S@2 / S@4. 5B lands at its own height S@6 and takes
      // its count from S@8; 5C lands on the walk-mesh floor at the target picked with level U@9 (the entrance floor selector) and takes
      // its count from S@6. Ticks n: mode 0 = the count; mode 1 = distance / count; mode 2: the count is the apex height h (negative =
      // up) and n = sqrt(-h) + sqrt(-h - (landing - y)) (the ticks to rise and to fall under the 2-unit/tick^2 gravity).
      // Ballistic: vy = -n + (landing - y) / n, then each tick y += vy, vy += 2 (Y down), xz += d / (n+1).
      motion.tx = static_cast<float>(req.x);
      motion.tz = static_cast<float>(req.z);
      const int count = req.kind == K::Tween ? req.params[0] : req.y;
      if (req.kind == K::Tween) {
        motion.ty = static_cast<float>(req.y);
      } else {
        const WalkMesh& walk = view_.walk();
        const int tri = walk.empty() ? -1 : walk.find_triangle_level(motion.tx, motion.tz, req.params[0]);
        motion.ty = tri >= 0 ? walk.height(tri, motion.tx, motion.tz) : cur.y;
      }
      motion.target_y = true;
      const float dx = motion.tx - cur.x, dz = motion.tz - cur.z;
      auto isqrt = [](int value) { return value > 0 ? static_cast<int>(std::sqrt(static_cast<double>(value))) : 0; };
      int jump_ticks = count;
      if ((req.mode & 3) == 1) jump_ticks = count != 0 ? static_cast<int>(std::hypot(dx, dz)) / count : 0;
      else if ((req.mode & 3) == 2) {
        const int rem = -count - (static_cast<int>(motion.ty) - static_cast<int>(cur.y));
        jump_ticks = std::abs(isqrt(-count) + (rem < 0 ? -isqrt(-rem) : isqrt(rem)));
      }
      if (jump_ticks <= 0) jump_ticks = 1;
      motion.total = jump_ticks;
      motion.elapsed = 0;
      motion.vy = -static_cast<float>(jump_ticks) + (motion.ty - cur.y) / static_cast<float>(jump_ticks);
      motion.step_x = dx / static_cast<float>(jump_ticks + 1);
      motion.step_z = dz / static_cast<float>(jump_ticks + 1);
      // Facing toward the target unless the slot flag (SET_SLOT_FLAG_126) is set or the turn rate is 0.
      if (vm_.entity(ent).rec[vm_.entity(ent).cur].flag47 == 0 && vm_.entity(ent).v24e != 0 && (dx != 0 || dz != 0))
        ent_state.actor.set_facing(bearing(dx, dz));
      break;
    }
  }
  // Start forms of the walks (3F/40, 42/43, 46, 44/45, B8/B9): the facing becomes the bearing to the target unless the slot flag
  // (op 47) is set or the turn rate is 0, and is saved in the slot for the wait form to re-assert each tick. The model yaw then
  // chases it. A walk that arrives within its
  // first tick (tel_a03f's circling child: 10-unit steps under the 16-unit arrival distance) therefore still turns the entity.
  {
    const script::Entity& script_ent = vm_.entity(ent);
    const bool turn = script_ent.rec[script_ent.cur].flag47 == 0 && script_ent.v24e != 0;
    if (turn) {
      if (req.kind == K::Move || req.kind == K::Move3D) {
        ent_state.actor.set_facing(motion.heading);
      } else if (req.kind == K::Walk || req.kind == K::Walk2 || req.kind == K::WalkXZ || req.kind == K::WalkXZ2) {
        if (std::fabs(motion.tx - cur.x) >= 0.5f || std::fabs(motion.tz - cur.z) >= 0.5f) ent_state.actor.set_facing(bearing(motion.tx - cur.x, motion.tz - cur.z));
      }
    }
    motion.facing = ent_state.actor.facing();
    motion.curve_h = ent_state.actor.shown_facing();
    // The curved-walk heading is per script slot: only the 42 / 43 start form resets it to the shown facing; 44 / 45 and 46
    // curve on from the slot's last value.
    motion.slot = script_ent.cur & 7;
    motion.rec_id = script_ent.rec[static_cast<size_t>(motion.slot)].id;
    if (req.kind == K::WalkXZ || req.kind == K::WalkXZ2) ent_state.curve_h[static_cast<size_t>(motion.slot)] = motion.curve_h;
    if (req.kind == K::Move) {
      // 46's start form also stores a target: position + (heading vector) * S * frames * 2 (in 16ths), i.e. twice as far as the walk
      // goes in its frames; the wait form tests the arrival against it and the curved form steers toward it.
      const float speed = script_ent.v10c ? static_cast<float>(script_ent.v10c) / 16.0f : kWalkUnitsPerTick;
      const float heading_rad = static_cast<float>(motion.heading) * kUnitsToRad;
      const float dist = speed * static_cast<float>(motion.frames) * 2.0f;
      motion.tx = cur.x - std::cos(heading_rad) * dist;
      motion.tz = cur.z + std::sin(heading_rad) * dist;
    }
    if (req.kind == K::Move3D) {
      // BA start form: the same x / z target as 46 and the height target y + sin(pitch) * ... with pitch = U@4:
      // ((rsin(pitch) * S >> 4) * frames * 0x2000) >> 12 in 32-bit arithmetic, truncated to a short and added to y. (The y term lacks
      // the / 4096 of the x / z terms, so any pitch other than 0 sends the target off by thousands of units; the three rooms that use BA -
      // tel_b01f, tel_b23e, tel_b31e - all pass pitch 0.) The wait form walks the straight 3D line toward it with the height free.
      const float speed = script_ent.v10c ? static_cast<float>(script_ent.v10c) / 16.0f : kWalkUnitsPerTick;
      const float heading_rad = static_cast<float>(motion.heading) * kUnitsToRad;
      const int frame_count = std::max(0, req.params[2]);
      const float dist = speed * static_cast<float>(frame_count) * 2.0f;
      motion.tx = cur.x - std::cos(heading_rad) * dist;
      motion.tz = cur.z + std::sin(heading_rad) * dist;
      const int speed_units = script_ent.v10c ? script_ent.v10c : static_cast<int>(kWalkUnitsPerTick * 16.0f);
      const int sn = static_cast<int>(std::lround(std::sin(static_cast<float>(req.params[1] & 0xFFF) * kUnitsToRad) * 4096.0f));
      // The engine adds 0xF to a negative product before the shift: truncation toward zero.
      const int y_step = (sn * speed_units) / 16;
      const s32 y_off = static_cast<s32>(static_cast<u32>(y_step) * static_cast<u32>(frame_count) * 0x2000u);
      const s32 y_sh = (y_off < 0 ? y_off + 0xFFF : y_off) >> 12;
      motion.ty = static_cast<float>(static_cast<s16>(static_cast<s16>(y_sh) + static_cast<s16>(std::lround(cur.y))));
      motion.target_y = true;
    }
  }
  ent_state.motion = motion;
  log::debug("field: tick {} ent {} motion kind {} from {:.0f},{:.0f},{:.0f} to {:.0f},{:.0f},{:.0f} frames {} speed {}", tick_count_, ent,
             static_cast<int>(req.kind), static_cast<double>(motion.sx), static_cast<double>(motion.sz), static_cast<double>(motion.sy),
             static_cast<double>(motion.tx), static_cast<double>(motion.tz), static_cast<double>(motion.ty), motion.frames, vm_.entity(ent).v10c);
}

bool FieldWorld::motion_busy(int ent) {
  return ent >= 0 && ent < static_cast<int>(ents_.size()) && ents_[static_cast<size_t>(ent)].motion.active;
}

bool FieldWorld::motion_wait(int ent) {
  // The wait forms of 3F / 40 and 42 / 43 test the arrival before stepping: within reach (3F: distance
  // <= S/16, one step; 42 / 43: distance <= S/8) the position snaps to the target, the clip becomes the stand slot and pc advances in
  // the same tick, without a yield. Without this every walk lasted one tick more than the engine's, and walks whose target is
  // within reach from the start (the circling child of tel_a03f: one 10-unit WALK_TO_XZ_2 per UPDATE) moved every other tick.
  if (!motion_busy(ent)) return false;
  using K = script::MotionKind;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  Motion& motion = ent_state.motion;
  const script::Entity& script_ent = vm_.entity(ent);
  const bool y_valid = (script_ent.flags38 & 0x40) != 0;
  const float speed = script_ent.v10c ? static_cast<float>(script_ent.v10c) / 16.0f : kWalkUnitsPerTick;
  const ActorPose& cur = ent_state.actor.current();
  const WalkMesh& walk = view_.walk();
  const bool xz_velocity = (motion.kind == K::WalkXZ || motion.kind == K::WalkXZ2) && !motion.direct && !y_valid;
  if (xz_velocity) {
    if (std::hypot(motion.tx - cur.x, motion.tz - cur.z) > 2.0f * speed) return true;
    ent_state.actor.set_position(walk, motion.tx, motion.tz, false, y_valid);
  } else if (motion.kind == K::Walk || motion.kind == K::Walk2 || motion.kind == K::WalkXZ || motion.kind == K::WalkXZ2) {
    const bool three_d = motion.target_y && y_valid;
    const float dy = three_d ? motion.ty - cur.y : 0.0f;
    if (std::sqrt((motion.tx - cur.x) * (motion.tx - cur.x) + (motion.tz - cur.z) * (motion.tz - cur.z) + dy * dy) > speed) return true;
    ent_state.actor.set_position(walk, motion.tx, motion.tz, false, y_valid);
    if (three_d) ent_state.actor.set_height(motion.ty, false);
  } else {
    return true;
  }
  motion.active = false;
  return false;
}

bool FieldWorld::near_and_facing(int ent, int target, int dist, int tolerance) {
  if (ent < 0 || target < 0 || ent >= static_cast<int>(ents_.size()) || target >= static_cast<int>(ents_.size())) return false;
  // D8: the 3D distance must be < U, and the MODEL yaw (what the entity visibly faces, not the heading target) must be within
  // +-tol of the bearing to the target. A robot turning toward the leader "sees" him only once its body has turned.
  const ActorPose& pose = ents_[static_cast<size_t>(ent)].actor.current();
  const ActorPose& target_pose = ents_[static_cast<size_t>(target)].actor.current();
  const float dx = target_pose.x - pose.x, dy = target_pose.y - pose.y, dz = target_pose.z - pose.z;
  const float d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
  int diff = (ents_[static_cast<size_t>(ent)].actor.shown_facing() - bearing(dx, dz)) & 0xFFF;
  if (diff > 0x800) diff = 0x1000 - diff;
  return d3 < static_cast<float>(dist) && diff <= tolerance;
}

bool FieldWorld::region_test(int, int region, bool band) {
  // Ops 55 / 56 test the CONTROLLED entity (set by FE 4C), not the entity whose script runs:
  // the zone-watcher entities that implement most exits (Hydra Marshes: ent 18 / 19 UPDATE) sit at the origin.
  const int ent = player_entity();
  const assets::RoomScene* scene = view_.scene();
  if (!scene || ent < 0 || ent >= static_cast<int>(ents_.size()) || region < 0 || region >= static_cast<int>(scene->zones.size()))
    return false;
  const auto& zone = scene->zones[static_cast<size_t>(region)];
  const ActorPose& pose = ents_[static_cast<size_t>(ent)].actor.current();
  // Point in the XZ quad: the four edge functions agree in sign (the corner order is not normalised here).
  auto side = [&](int i, int j) {
    return (static_cast<float>(zone.corner[j][0] - zone.corner[i][0])) * (pose.z - static_cast<float>(zone.corner[i][2])) -
           (static_cast<float>(zone.corner[j][2] - zone.corner[i][2])) * (pose.x - static_cast<float>(zone.corner[i][0]));
  };
  const float e0 = side(0, 1), e1 = side(1, 2), e2 = side(2, 3), e3 = side(3, 0);
  const bool inside = (e0 >= 0 && e1 >= 0 && e2 >= 0 && e3 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0 && e3 <= 0);
  if (!inside) return false;
  // Op 56 gates first on the height: zone.y < actor.y && actor.y - actor height < zone.y.
  if (band) {
    const float zy = static_cast<float>(zone.corner[0][1]);
    return zy < pose.y && pose.y - ent_height(ent) < zy;
  }
  return true;
}

// 7A: the ATTRIBUTE byte of the walk-mesh triangle the entity stands on (flags = table[attr]). opn_a02i's Cybot 5 idles while the
// leader's tile has attr 79 (the lift / safe strip) and chases otherwise. No triangle -> -1 (never equal).
int FieldWorld::entity_state(int ent) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return -1;
  const int tri = ents_[static_cast<size_t>(ent)].actor.triangle();
  const auto& tris = view_.walk().mesh().triangles;
  return tri >= 0 && static_cast<size_t>(tri) < tris.size() ? tris[static_cast<size_t>(tri)].attr : -1;
}

// Head turns (FE 30..57): the handler stores a world target in the model and calls Actor::head_look_at, or Actor::head_reset for
// FE 32 / FE 56. FE 35 / 49 / 53 / 57 store the point (rcos(a), y, -rsin(a)), the 4.12 trig values as they are (radius 4096 around
// the WORLD origin, not around the actor), a = actor yaw + offset + 0x400 - the engine adds no position.
void FieldWorld::head_op(script::HeadOp op, int ent, int other, s32 x, s32 y, s32 z, int rate) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Actor& actor = ents_[static_cast<size_t>(ent)].actor;
  if (!actor.loaded()) return;
  using H = script::HeadOp;
  log::debug("field: tick {} ent {} head op {} (other {}, {} {} {}, rate {:#x}, head joint {})", tick_count_, ent, static_cast<int>(op), other, x,
             y, z, rate, actor.head_joint());
  switch (op) {
    case H::Point: {
      const float target[3] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
      actor.head_look_at(target, rate);
      break;
    }
    case H::LookAtHead: {
      float target[3];
      if (other < 0 || other >= static_cast<int>(ents_.size()) || !ents_[static_cast<size_t>(other)].actor.attach_world(1, target)) return;
      actor.head_look_at(target, rate);
      break;
    }
    case H::Facing: {
      const int ang = (static_cast<int>(std::lround(actor.current().yaw)) + x + 0x400) & 0xFFF;
      const float radians = static_cast<float>(ang) * (2.0f * 3.14159265358979323846f / 4096.0f);
      const float target[3] = {4096.0f * std::cos(radians), static_cast<float>(y), -4096.0f * std::sin(radians)};
      actor.head_look_at(target, rate);
      break;
    }
    case H::Reset: actor.head_reset(); break;
  }
}

bool FieldWorld::attach_point(int ent, int point, script::Vec3& out) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  float world[3];
  if (!ents_[static_cast<size_t>(ent)].actor.attach_world(point, world)) return false;
  out = {static_cast<s32>(std::lround(world[0])), static_cast<s32>(std::lround(world[1])), static_cast<s32>(std::lround(world[2]))};
  return true;
}

void FieldWorld::entity_effect(u16 key, int ent, std::span<const s32> args) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  // 9D: close the window this entity has up (world-map labels when the party walks away).
  if (key == 0x9D) {
    if (label_ && dialogue_.is_open() && dialogue_owner_ == ent) dialogue_.close();
    for (auto& window : extra_win_)
      if (window.box && window.label && window.box->is_open() && window.owner == ent) window.box->close();
    return;
  }
  // 36 with b != 0: the stand clip is submitted afresh (the previous 3C / A9 pose ends).
  if (key == 0x36) {
    ent_state.actor.clear_play();
    ent_state.actor.play(0xFF);
    return;
  }
  if (key == 0xA0) {
    // A0 = the engine's WANDER, one call per tick. A timer counts down; below zero a new window of (rand & 0x1F) + 0x20 ticks starts
    // and a random bit decides whether it walks. Bit clear: stand clip, done unless the operand is 1. Bit set (or operand 1): walk
    // clip; with a 1/256 chance the preferred turn direction is re-picked; with a 1/16 chance per tick the heading is turned
    // +-0x80 (the direction bit picks the side, 1 in 8 of those the other side), else the heading = the facing; the entity then
    // moves along that heading at its own speed through the wall probe and the slide (the facing follows). This is how
    // opn_a02i's Cybots and every "A0" idler roam.
    if (--ent_state.sway_timer < 0) {
      const u32 random = rand_u32();
      ent_state.sway_bit = random & 1;
      ent_state.sway_timer = static_cast<int>((rand_u32() & 0x1F) + 0x20);
    }
    const bool walk_window = ent_state.sway_bit || (!args.empty() && args[0] == 1);
    if (!walk_window) {
      ent_state.actor.play(ent_state.actor.anim_slot(kSlotStand));
      return;
    }
    ent_state.actor.play(ent_state.actor.anim_slot(kSlotWalk));
    if ((rand_u32() & 0xFF) == 0) ent_state.sway_dir = rand_u32() & 1;
    const int turn = ent_state.sway_dir ? 0x80 : -0x80;
    int heading = ent_state.actor.facing();
    if ((rand_u32() & 0xF) == 0) heading = (heading + (((rand_u32() & 7) == 0) ? -turn : turn)) & 0xFFF;
    const WalkMesh& walk = view_.walk();
    const float heading_rad = static_cast<float>(heading) * kUnitsToRad;
    const float step = static_cast<float>(vm_.entity(ent).v10c ? vm_.entity(ent).v10c : kDefaultSpeed) / 16.0f;
    ent_state.actor.set_facing(heading);
    // A slide re-aims the facing along the wall.
    ent_state.actor.move_on_mesh(walk, -std::cos(heading_rad) * step, std::sin(heading_rad) * step);
    return;
  }
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : 0; };
  switch (key) {
    case 0xFE46:  // offset (x = S@1, z = S@3, y = S@5) from the D6 parent's attach point
      ent_state.attach_off = {static_cast<s16>(arg(0)), static_cast<s16>(arg(2)), static_cast<s16>(arg(1))};
      return;
    case 0xFE47:  // rotation (rx = S@1, rz = S@3, ry = S@5) relative to the parent joint
      ent_state.attach_rot = {static_cast<s16>(arg(0)), static_cast<s16>(arg(2)), static_cast<s16>(arg(1))};
      return;
    case 0xAC:  // n = 0 clears the model's semi-transparency; n >= 1 sets it with blend mode n - 1 (same texture page)
      ent_state.model_abr = arg(0) == 0 ? -1 : ((arg(0) - 1) & 3);
      return;
    case 0xFE45:  // (only when b != 0) white silhouette CLUT row 0xF5 + U for the model; the flash flag = b & 1
      if (arg(1) >= 0) ent_state.flash_row = arg(1);
      ent_state.flash = (arg(0) & 1) != 0;
      return;
    case 0xFE03:  // reflected copy of the model, axis b (0 x, 1 y, 2 z -> 4 / 8 / 0x10), plane S (stored as S << 1)
      ent_state.mirror_axis = arg(0) == 0 ? 4 : arg(0) == 1 ? 8 : arg(0) == 2 ? 0x10 : ent_state.mirror_axis;
      ent_state.mirror_plane2 = arg(1) * 2;
      log::debug("field: tick {} ent {} mirror copy axis {:#x} plane {}", tick_count_, ent, ent_state.mirror_axis, arg(1));
      return;
    case 0x92: {  // semi-transparency bit (M != 0) on both primitives of every tile record of background group key U
      bg_group_abe_[static_cast<u32>(arg(1))] = arg(0) != 0;
      log::debug("field: tick {} background group {} semi-transparency {}", tick_count_, arg(1), arg(0) != 0);
      return;
    }
    case 0xC5:
    case 0xC8: {
      // (src, id, x, z, y, vol, oneshot): C5 = the executing entity's sound (src = the entity), C8 = a fixed point (src -1;
      // operands x, z, y). A record of the same source is reused (its started flag cleared), else appended while fewer than 8.
      const bool fixed = key == 0xC8;
      const int src = fixed ? -1 : ent;
      SoundEmitter* record = nullptr;
      if (!fixed)
        for (auto& emitter : sound_emitters_)
          if (emitter.src == src) record = &emitter;
      if (!record) {
        if (sound_emitters_.size() >= 8) return;
        sound_emitters_.push_back({});
        record = &sound_emitters_.back();
        record->voice = 0x10 + static_cast<int>(sound_emitters_.size() - 1);
      } else if (record->started && mixer_) {
        // The voice of the old sound is keyed off.
        mixer_->evict_sfx(1u << record->voice);
      }
      record->id = arg(0) & 0x1FF;
      record->src = src;
      record->x = fixed ? arg(1) : 0;
      record->z = fixed ? arg(2) : 0;
      record->y = fixed ? arg(3) : 0;
      record->volume = fixed ? arg(4) : arg(1);
      record->oneshot = (fixed ? arg(5) : arg(2)) != 0;
      record->started = false;
      record->ramp = false;
      log::debug("field: tick {} sound emitter id {} src {} volume {} oneshot {}", tick_count_, record->id, src, record->volume, record->oneshot);
      return;
    }
    case 0xFE43: {  // volume ramp (target, frames) of the executing entity's sound record
      for (auto& emitter : sound_emitters_)
        if (emitter.src == ent) {
          emitter.ramp = true;
          emitter.ramp_frames = std::max(1, arg(1));
          emitter.ramp_value = emitter.volume << 16;
          emitter.ramp_step = static_cast<s32>(((static_cast<s64>(arg(0)) << 16) - emitter.ramp_value) / emitter.ramp_frames);
        }
      return;
    }
    default: break;
  }
  stub(std::format("entity_effect({:04X})", key).c_str());
}

void FieldWorld::attach_entity(int ent, int target, int point) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  // D6: parent entity and attach point (the VM sets / clears the attached flag).
  ent_state.attach_to = target;
  ent_state.attach_point = point;
  if (target < 0) ent_state.actor.clear_attached();
}

// An entity whose walk-mesh attribute word has bit 0x800 is pushed every tick by (rcos(d), -rsin(d)) * A * 12 in
// 16.16 (A = push strength, set by B5, room default 5: 3.75 units per tick), d = (attr & 0x700) << 1 (8 directions), on top of its
// own walk; bit 0x1000 halves the move. Not pushed: inactive / free-height entities, entities with the no-push flag, and the
// controlled entity while the input lock is set. The move goes through the walk-mesh collision like any other.
void FieldWorld::step_push() {
  const WalkMesh& walk = view_.walk();
  if (walk.empty() || push_strength_ == 0) return;
  const int player = player_entity();
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    const int tri = ent_state.actor.triangle();
    if (tri < 0 || !ent_state.placed) continue;
    const u32 attr = walk.flags(tri);
    if (!(attr & 0x800u)) continue;
    const script::Entity& script_ent = vm_.entity(i);
    if ((script_ent.flags38 & 0x50u) || (script_ent.flags270 & 0x40000u)) continue;
    if (i == player && (vm_.global_flags() & script::gflag::kInputLock)) continue;
    const float direction = static_cast<float>((attr & 0x700u) << 1) * (2.0f * 3.14159265358979323846f / 4096.0f);
    float strength = static_cast<float>(push_strength_) * 12.0f * 4096.0f / 65536.0f;
    if (attr & 0x1000u) strength *= 0.5f;
    ent_state.actor.move_on_mesh(walk, std::cos(direction) * strength, -std::sin(direction) * strength);
  }
}

// Per frame, for attached entities: the parent model's attach point gives the offset J and its joint's world matrix (R, T); the
// entity matrix becomes R x RotMatrix(attach_rot) (Rx * Ry * Rz) with the translation R (J + attach_off) + T, the model rotation is
// zeroed and the model scale is the entity scale << 3 (the parent joint matrix already carries the parent's model scale). The
// entity's own position is not changed. The original builds it from the parent's joint matrix of the same frame, so the model never
// trails its parent: the joint-space part is kept here and the actor applies the parent's joint of each drawn frame to it.
void FieldWorld::step_attached() {
  const int count = static_cast<int>(ents_.size());
  for (int i = 0; i < count; ++i) {
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    const script::Entity& script_ent = vm_.entity(i);
    if (ent_state.attach_to < 0 || ent_state.attach_to >= count || !(script_ent.flags38 & 0x800000u) || (script_ent.flags38 & 0x1C020u)) {
      ent_state.actor.clear_attached();
      continue;
    }
    // A chain of attachments that leads back to this entity has no frame to draw from.
    int ancestor = ent_state.attach_to;
    for (int depth = 0; depth < count && ancestor >= 0 && ancestor != i; ++depth) ancestor = ents_[static_cast<size_t>(ancestor)].attach_to;
    render::Mat4 jw;
    float joint_offset[3];
    if (ancestor == i || !ents_[static_cast<size_t>(ent_state.attach_to)].actor.attach_joint(ent_state.attach_point, 1.0f, jw, joint_offset)) {
      ent_state.actor.clear_attached();
      continue;
    }
    constexpr float kUnitsToRadians = 2.0f * 3.14159265358979323846f / 4096.0f;
    const float ax = static_cast<float>(ent_state.attach_rot[0]) * kUnitsToRadians, ay = static_cast<float>(ent_state.attach_rot[1]) * kUnitsToRadians,
                az = static_cast<float>(ent_state.attach_rot[2]) * kUnitsToRadians;
    // RotMatrix: Rx(ax) * Ry(ay) * Rz(az), PsyQ signs.
    render::Mat4 rx, ry, rz;
    rx.at(1, 1) = std::cos(ax);
    rx.at(1, 2) = -std::sin(ax);
    rx.at(2, 1) = std::sin(ax);
    rx.at(2, 2) = std::cos(ax);
    ry.at(0, 0) = std::cos(ay);
    ry.at(0, 2) = std::sin(ay);
    ry.at(2, 0) = -std::sin(ay);
    ry.at(2, 2) = std::cos(ay);
    rz.at(0, 0) = std::cos(az);
    rz.at(0, 1) = -std::sin(az);
    rz.at(1, 0) = std::sin(az);
    rz.at(1, 1) = std::cos(az);
    // Joint space: translation J + attach_off, then the rotation and the model scale (entity scale << 3: 0x200 -> 0x1000 = 1.0).
    render::Mat4 local = rx * ry * rz;
    for (int row = 0; row < 3; ++row) local.at(row, 3) = joint_offset[row] + static_cast<float>(ent_state.attach_off[static_cast<size_t>(row)]);
    render::Mat4 scale;
    for (int axis = 0; axis < 3; ++axis) scale.at(axis, axis) = static_cast<float>(static_cast<s16>(script_ent.scale[static_cast<size_t>(axis)])) * 8.0f / 4096.0f;
    ent_state.actor.set_attached(&ents_[static_cast<size_t>(ent_state.attach_to)].actor, ent_state.attach_point, local * scale);
  }
}

// OC_ATTACH_DRIFT: after every tick, each attached model is drawn at render fractions 0 and 0.5 and its origin compared with the
// parent's attach point (+ the FE 46 offset) at the same fraction. The original draws the two from the same frame's joint matrix,
// so any distance is the attached model sliding on its parent; ticks above half a unit are logged.
void FieldWorld::check_attach_drift() const {
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    const Ent& ent_state = ents_[static_cast<size_t>(i)];
    if (!ent_state.actor.attached() || ent_state.attach_to < 0 || ent_state.attach_to >= static_cast<int>(ents_.size())) continue;
    float worst = 0.0f;
    for (const float alpha : {0.0f, 0.5f}) {
      render::Mat4 jw;
      float joint_offset[3];
      if (!ents_[static_cast<size_t>(ent_state.attach_to)].actor.attach_joint(ent_state.attach_point, alpha, jw, joint_offset)) continue;
      const float point[3] = {joint_offset[0] + static_cast<float>(ent_state.attach_off[0]), joint_offset[1] + static_cast<float>(ent_state.attach_off[1]),
                              joint_offset[2] + static_cast<float>(ent_state.attach_off[2])};
      const render::Mat4 drawn = ent_state.actor.model_matrix(alpha);
      float dist2 = 0.0f;
      for (int row = 0; row < 3; ++row) {
        const float expected = jw.at(row, 0) * point[0] + jw.at(row, 1) * point[1] + jw.at(row, 2) * point[2] + jw.at(row, 3);
        dist2 += (drawn.at(row, 3) - expected) * (drawn.at(row, 3) - expected);
      }
      worst = std::max(worst, std::sqrt(dist2));
    }
    if (worst > 0.5f)
      log::info("field: tick {} attach drift: room {} ent {} on ent {} point {}: drawn {:.1f} units off the parent's attach point", tick_count_,
                view_.room_name(), i, ent_state.attach_to, ent_state.attach_point, static_cast<double>(worst));
  }
}

// LOAD_ANIMATION (C1): the entity's ex anim pack becomes file (U & 0xFF) + 0x7D6 (bit 8 clear: the "<char>00" event pack of
// character n) or (U & 0xFF) + 0x795 (bit 8 set: the character's own pack, index = character id); the file index is dirBase(5) + that, the
// names come from cdrom.fnd (fieldobj/ex = 116 consecutive files from 0x795). SET_ANIMATION 0x80 + k then plays clip k of that pack.
// BF / C0 put the default pack back / free it.
static const char* const kEventPackNames[] = {
    "selju00", "kid00", "guill00", "ishito00", "slash00", "jakotu00", "riddle00", "carshu00", "zoah00", "malche00", "colcha00", "luti00",
    "poshul00", "lazz00", "zappa00", "ocha00", "radius00", "farga00", "mama00", "gren00", "lena00", "miki00", "harley00", "jyane00",
    "", "hosi00", "spri00", "dan00", "kabuo00", "rablue00", "jil00", "hone00", "kinoko00", "ilenes00", "mell00", "liia00", "banc00",
    "sunef00", "", "doc00", "gyada00", "pie00", "olha00", "", "", "", "", "", "", "yama00", "gren00"};

void FieldWorld::extra_animation(u16 key, bool start, int ent, int arg) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  if (key == 0xBF) {
    ent_state.actor.hold_default_pack();
    return;
  }
  if (key == 0xC0) {
    ent_state.actor.clear_ex_clips();
    return;
  }
  if (key != 0xC1 || !start) return;
  std::string name;
  // The pack's name is the disc index record of that file (engine file id + 237 = cdrom.fnd record: map id 0x83 = record kMapBase), so
  // every pack resolves, the numbered event packs past the "<char>00" block included (0x40 = selju01: gal_b09i's bed pose, 0x37 =
  // kid06, ...); dummy records load nothing. The name tables below are the fallback when the index is missing.
  if (discs_.loaded()) {
    constexpr int kFidToRecord = DiscIndex::kMapBase - 0x83;
    const std::string& rec = discs_.record(disc_, ((arg & 0x100) ? 0x795 : 0x7D6) + (arg & 0xFF) + kFidToRecord);
    if (const size_t pos = rec.find("fieldobj/ex/"); pos != std::string::npos && rec.ends_with(".bin")) {
      name = rec.substr(pos + 12, rec.size() - pos - 16);
      if (name == "dummy") name.clear();
    }
  } else if (arg & 0x100) {
    if ((arg & 0xFF) < 44) name = kCharFolders[arg & 0xFF];
  } else if ((arg & 0xFF) < static_cast<int>(std::size(kEventPackNames))) {
    name = kEventPackNames[arg & 0xFF];
  }
  // A dummy record loads nothing, but the entity holds a pack all the same.
  if (name.empty()) {
    ent_state.actor.set_ex_clips({});
    return;
  }
  // The pack attaches to the entity's display model whatever the entity is: a party member's character model or
  // an NPC's room model (dem_02's Zoah, a room model, loads zoah00 / zoah and plays its jump clips 0x86..0x89). The clips are decoded
  // against that model's own skeleton.
  const bool pc_model = ent_state.char_id >= 0 && ent_state.char_id < 44 && !model_folder(ent_state.char_model).empty();
  const assets::RoomScene* scene = view_.scene();
  const bool room_mdl = !pc_model && scene && ent_state.model >= 0 && ent_state.model < static_cast<int>(scene->models.size());
  if (!pc_model && !room_mdl) {
    log::debug("field: tick {} ent {} LOAD_ANIMATION {}: entity has no model", tick_count_, ent, name);
    return;
  }
  const std::string cache_key =
      pc_model ? std::format("{}/{}", ent_state.char_model, name) : std::format("room:{}:{}/{}", view_.room_name(), ent_state.model, name);
  auto it = ex_pack_cache_.find(cache_key);
  if (it == ex_pack_cache_.end()) {
    ExPack pack;
    std::optional<std::vector<u8>> obj;
    std::string owner;
    if (pc_model) {
      owner = model_folder(ent_state.char_model);
      obj = files_->read("cdrom/fieldobj/player/" + owner + "/" + owner + ".obj");
    } else {
      owner = std::format("room model {}", ent_state.model);
      obj = scene->models[static_cast<size_t>(ent_state.model)].mdl;
    }
    auto ex = files_->read("cdrom/fieldobj/ex/" + name + ".bin");
    if (obj && ex) {
      std::string err;
      if (auto clips = import::load_ex_clips(ByteSpan(*obj), ByteSpan(*ex), &err)) pack.clips = std::move(*clips);
      if (pack.clips.empty()) log::warn("field: ex pack {} for {}: {}", name, owner, err.empty() ? "no clips" : err);
      // Entry 1 of the pack holds the model's animation sequences.
      pack.seq = import::ex_anim_seq(ByteSpan(*ex));
    } else {
      log::warn("field: ex pack fieldobj/ex/{}.bin for {} not found", name, owner);
    }
    it = ex_pack_cache_.emplace(cache_key, std::move(pack)).first;
  }
  log::debug("field: tick {} ent {} LOAD_ANIMATION {} ({} clips, {} sequences)", tick_count_, ent, name, it->second.clips.size(),
             it->second.seq.size());
  ent_state.actor.set_ex_clips(it->second.clips, it->second.seq);
}
bool FieldWorld::extra_animation_busy() { return false; }

// ------------------------------------------------------------------------------------------------ host: camera
// The pan limits every scroll target is clamped to (the room's, or a script's own limits), narrowed in widescreen so the wider

}  // namespace oc::game
