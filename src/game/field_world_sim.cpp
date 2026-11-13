// FieldWorld: the simulation of motion - input lock and pad heading, turn rates, scripted motions, the player's walk and the
// followers' trail.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <utility>

#include "core/log.h"
#include "game/cheats.h"
#include "game/field_world_internal.h"
#include "game/new_game_plus.h"

namespace oc::game {

using namespace field_internal;

namespace {

// Controls stay locked while the screen is darker than this.
constexpr float kLockFade = 0.3f;
// A scripted XZ walk blocked this long ends (the engine waits for ever).
constexpr int kStuckTicks = 240;

// Angle `cur` stepped toward `target` by `step` the short way round, landing on the target when within one step (4096/turn).
constexpr int turn_toward(int cur, int target, int step) {
  int result = 0;
  if (((cur - target) & 0xFFF) < 0x800) {
    result = cur - step;
    if (((result - target) & 0xFFF) > 0x7FF) result = target;
  } else {
    result = cur + step;
    if (((result - target) & 0xFFF) < 0x801) result = target;
  }
  return result & 0xFFF;
}

// D-pad heading table: index bit0 up, bit1 right, bit2 down, bit3 left. 0x8000 = none.
int dpad_heading(int mask) {
  constexpr int kTable[16] = {0x8000, 0x400, 0x800, 0x600, 0xC00, 0x8000, 0xA00, 0x8000,
                              0x000,  0x200, 0x8000, 0x8000, 0xE00, 0x8000, 0x8000, 0x8000};
  return kTable[mask & 15];
}

}  // namespace

bool FieldWorld::controls_free(bool menu_request) const {
  if (trans_ != Trans::None || fade_ > kLockFade || jump_.active) return false;
  if (modal_window_open()) return false;
  // The menu button is refused while any message window is open, a frameless label included.
  if (menu_request && any_window_open()) return false;
  const u32 gflags = vm_.global_flags();
  if ((gflags & script::gflag::kInputLock) && !(menu_request && (gflags & script::gflag::kMenuUnderLock))) return false;
  // A mod UI that has taken a pick holds the pad (script::ModUi::holds_controls).
  if (vm_.mod_ui().shown && vm_.mod_ui().holds_controls) return false;
  if (panel_.is_open()) return false;
  for (size_t i = 0; i < ents_.size(); ++i) {
    const script::Entity& script_ent = vm_.entity(static_cast<int>(i));
    if (script_ent.flags270 & script::flag270::kActionRunning) return false;
  }
  return true;
}

// Start pauses the field every frame unless the room forbids it: the room script (FE 42 0 clears the flag every room load sets) or
// variable 0x36, the title / ending maps 8 and 9, a room switch, a menu, a movie or a battle on its way. Open windows, choices, a
// running cutscene and the input lock do not count.
bool FieldWorld::pause_allowed() const {
  if (in_init_ || trans_ != Trans::None || jump_.active) return false;
  if (!(vm_.global_flags() & script::gflag::kPauseAllowed) || (vm_.var(0x36) & 0xFF) != 0) return false;
  if (map_id_ == 8 || map_id_ == 9) return false;
  return menu_state_ == MenuState::None && movie_state_ == MovieState::None && handoff_.battle_state == BattleState::None;
}

bool FieldWorld::speed_allowed() const {
  if (in_init_ || trans_ != Trans::None || movie_state_ != MovieState::None || input_.lock > 0) return false;
  return (vm_.var(0x36) & 0xFF) == 0 && map_id_ != 8 && map_id_ != 9;
}

void FieldWorld::update_input_lock() {
  // The lock of an inactive entity (SET_VISIBLE 0) does not count.
  bool locked = false;
  for (size_t i = 0; i < ents_.size(); ++i) {
    const script::Entity& script_ent = vm_.entity(static_cast<int>(i));
    if ((script_ent.flags270 & script::flag270::kInputLock) && !(script_ent.flags38 & script::flag38::kInactive)) locked = true;
  }
  u32 flags = vm_.global_flags();
  flags = locked ? (flags | script::gflag::kInputLock) : (flags & ~script::gflag::kInputLock);
  vm_.set_global_flags(flags);
}


int FieldWorld::pad_heading(bool& run) const {
  // The dash button (+ the "[button] Pressed" option) picks walk / run: run = held XOR option
  // (option clear: the button runs; set: the character runs and the button walks).
  const bool opt_walk_button = gs_ && gs_->option(ui::opt::kButtonWalks);
  run = input_.run_held != opt_walk_button;
  int pad = dpad_heading(static_cast<int>(input_.held_mask));
  if (input_.pad_analog) {
    pad = static_cast<int>(std::lround(std::atan2(input_.pad_y, -input_.pad_x) * kUnitsPerRad)) & 0xFFF;
    // Left Stick Sensitivity = Analog: a stick tilted past 0x7C of 0x7F runs, unless button and option cancel.
    if (gs_ && gs_->option(ui::opt::kAnalogStick) && std::hypot(input_.pad_x, input_.pad_y) * 127.0f > 124.0f) run = !(input_.run_held && opt_walk_button);
  }
  if (pad == 0x8000) return kNoHeading;
  // [Game] CameraRelative: up (heading 0x400, +z with no yaw) goes along the camera's up axis on the floor. A heading h walks
  // (-cos h, sin h) in XZ, so that axis is heading atan2(up_z, -up_x); the room's control yaw is not used.
  if (camera_relative_) {
    float right_x = 0, right_z = 0, up_x = 0, up_z = 0;
    view_.camera().ground_axes(right_x, right_z, up_x, up_z);
    if (up_x != 0.0f || up_z != 0.0f) {
      const int up = static_cast<int>(std::lround(std::atan2(up_z, -up_x) * kUnitsPerRad));
      return (pad + up - 0x400) & 0xFFF;
    }
  }
  return (pad + control_yaw_) & 0xFFF;
}

// Player vs the solid actors. The test point is the player's next position P' = P + V. A solid whose rect /
// circle contains P' blocks the move, raises its press counter (any tick the player does not press against it resets it) and
// - with the FE 25 bit - stops blocking once the counter has reached 0x40 (tested before the increment): a mover stuck against a
// pillar / NPC slides through after 64 ticks.
// Pushable actors (FE 06): the hit hands the actor the player's velocity (:= V) unless
// the actor's own next centre is inside another box actor (or the walk-mesh probe refuses the step); the player stays this tick.
// The actor's rect is built at position + pending velocity, so next tick P' lies outside it: no hit, the actor moves by
// its velocity (the no-hit branch), the player moves by V, and the velocity is cleared - which makes the next tick a hit again. A pushed
// block therefore advances one player step every second tick.

void FieldWorld::step_turn_rates() {
  // Once per rendered frame (every second tick here), before the yaw step: the turn rate and the scripted-turn flag bits.
  // Bit 13 (FE 07 / FE 0C, set in INIT by most characters) makes the turns that the rotation ops request (bit 14:
  // 2E 2F 30 31 32 33 CA) take a fixed time: the turn rate is saved (unless a turn is already running, bit 15) and becomes
  // |turn| / 0x30, so any turn lasts about 0x30 ticks; a turn under 0x30 snaps. Bit 15 marks the turn; when the yaw reaches the facing
  // (or bit 13 is cleared) the turn rate is restored. The walks do not set bit 14: they turn at the plain turn rate (default 0x80 per tick).
  // Turns of 0x140 and more also start the turn clip: SET_WALK_ANIMATION 8 when the model yaw is less than half a turn ahead of
  // the facing, else mode 7, with the anim-done bit set (re-submitted even when it is the id in force). What ends it is the
  // model's animation sequence of that id: Serge's entries 6 / 7 play the turn clip once (loop off), wait for its end and
  // 8 ticks, then start clip 1 (stand) and set var 0.
  // The yaw chase follows the bookkeeping in the engine's frame step, so the yaw holds still while a request waits for its odd tick:
  // the turn starts from the shown facing the request was made at (an exact half turn stays exact and takes the left clip).
  // In the room-entry window a model whose rate is not 0 takes its facing at once instead.
  const bool frame_tick = (tick_count_ & 1u) != 0;
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    script::Entity& script_ent = vm_.entity(i);
    Actor& actor = ents_[static_cast<size_t>(i)].actor;
    if (frame_tick) {
      u32& turn_flags = script_ent.flags270;
      auto restore = [&] {
        turn_flags &= ~0xC000u;
        script_ent.v24e = script_ent.v240;
      };
      if (turn_flags & 0x2000u) {
        if (turn_flags & 0x4000u) {
          if (!(turn_flags & 0x8000u)) script_ent.v240 = script_ent.v24e;
          int diff = (actor.shown_facing() - actor.facing()) & 0xFFF;
          if (diff > 0x7FF) diff = (actor.facing() - actor.shown_facing()) & 0xFFF;
          script_ent.v24e = static_cast<u16>(diff / 0x30);
          if (script_ent.v24e == 0) {
            actor.set_shown_facing(actor.facing());
            restore();
          } else {
            if (diff > 0x13F && !(script_ent.flags38 & 0x4000u)) {
              const int id = actor.anim_slot(((actor.shown_facing() - actor.facing()) & 0xFFF) < 0x800 ? kSlotTurnR : kSlotTurnL);
              if (actor.has_script_clip(id)) {
                actor.play_engine_clip(id, true);
                log::debug("field: tick {} ent {} turn clip {:#x} (turn {:#x})", tick_count_, i, id, diff);
              }
            }
            turn_flags = (turn_flags & ~0x4000u) | 0x8000u;
          }
        } else if ((turn_flags & 0x8000u) && actor.shown_facing() == actor.facing()) {
          restore();
        }
      } else if (turn_flags & 0x8000u) {
        restore();
      }
    }
    const bool request_waiting = (script_ent.flags270 & 0x6000u) == 0x6000u;
    actor.turn_rate = request_waiting ? 0 : static_cast<s16>(script_ent.v24e);
    if (entry_window_ > 0 && actor.turn_rate != 0) actor.set_shown_facing(actor.facing());
  }
}

// The A0 step of the tick, taken after every script has run: the entity walks one step at its own speed along the heading A0
// recorded, through the wall probe and the slide, and faces that heading (a slide re-aims the facing along the wall). A tile with
// flag 0x1000 halves the step. An inactive or free-height entity takes no step, and a step that ends touching the leader is taken
// back (touches_leader). The request is used up either way.
void FieldWorld::step_wander() {
  const WalkMesh& walk = view_.walk();
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    const int heading = std::exchange(ent_state.wander_heading, -1);
    if (heading < 0) continue;
    const script::Entity& script_ent = vm_.entity(i);
    if (script_ent.flags38 & (script::flag38::kInactive | 0x40u)) continue;
    float step = static_cast<float>(script_ent.v10c ? script_ent.v10c : kDefaultSpeed) / 16.0f;
    const int tri = ent_state.actor.triangle();
    if (tri >= 0 && !walk.empty() && (walk.flags(tri) & 0x1000u)) step *= 0.5f;
    const float heading_rad = static_cast<float>(heading) * kUnitsToRad;
    ent_state.actor.set_facing(heading);
    const Actor::Spot before = ent_state.actor.spot();
    ent_state.actor.move_on_mesh(walk, -std::cos(heading_rad) * step, std::sin(heading_rad) * step);
    if (touches_leader(i)) ent_state.actor.restore(before);
  }
}

void FieldWorld::step_motions() {
  using K = script::MotionKind;
  const WalkMesh& walk = view_.walk();
  // class_bits: which attribute walls block the entity.
  for (size_t i = 0; i < ents_.size(); ++i) ents_[i].actor.class_bits = vm_.entity(static_cast<int>(i)).flags38 & 0x600000u;
  step_wander();
  step_turn_rates();
  for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    Motion& motion = ent_state.motion;
    if (!motion.active) continue;
    const script::Entity& script_ent = vm_.entity(i);
    if (!motion.direct) {
      // A script motion moves only on the ticks its wait form runs (the handler sets the velocity or steps the position;
      // the velocity is zeroed after every move).
      // The scheduler runs only the record of the lowest class, so a script that preempts the walking one (the
      // ON_COLLISION class 4 / ON_ACTION class 3 of a trigger) freezes the walk until it ends. A record released
      // meanwhile (STOP / 03 / 04) leaves the motion orphaned: nothing moves the entity any more.
      const script::ScriptRecord& record = script_ent.rec[static_cast<size_t>(motion.slot)];
      if (record.state == script::kRecIdle || record.id != motion.rec_id) {
        motion.active = false;
        continue;
      }
      motion.paused = script_ent.cur != motion.slot;
      if (motion.paused) continue;
    }
    const bool y_valid = (script_ent.flags38 & 0x40) != 0;
    const float speed = script_ent.v10c ? static_cast<float>(script_ent.v10c) / 16.0f : kWalkUnitsPerTick;
    const ActorPose cur = ent_state.actor.current();
    // A formation walk steps in 3D toward its target height and takes no height from the mesh on the way: a straight line may cross a gap.
    const bool free_y = y_valid || motion.direct;
    auto move_xz = [&](float x, float z) { ent_state.actor.set_position(walk, x, z, false, free_y); };
    // Facing while walking: the start form sets the facing (bearing to the target, gated by the slot flag SET_SLOT_FLAG_126 and by
    // a non-zero turn rate; a script can set the flag so an entity slides sideways without rotating) and the wait form
    // re-asserts that saved value every tick. Only the entity follows (44 / 45) re-aim at the moving target each tick, with the same gate.
    const bool turn_to_walk = script_ent.rec[script_ent.cur].flag47 == 0 && script_ent.v24e != 0;
    auto walk_toward = [&](float tx, float tz, float stop, bool reaim) {
      const float dx = tx - cur.x, dz = tz - cur.z;
      const float distance = std::hypot(dx, dz);
      if (distance <= stop + 0.01f) return true;
      const float step = std::min(speed, distance - stop);
      move_xz(cur.x + dx / distance * step, cur.z + dz / distance * step);
      ent_state.actor.set_facing(reaim && turn_to_walk ? bearing(dx, dz) : motion.facing);
      return false;
    };
    // The movement phase adds a velocity walk's step to a free-height entity's position as it is (no wall probe, no slide, the
    // height unchanged); any other entity moves through the wall probe and the slide. A step that ends touching the leader is
    // taken back (touches_leader); that wait is not a stuck walk.
    auto move_by = [&](float vx, float vz, int heading) {
      const Actor::Spot before = ent_state.actor.spot();
      bool moved = true;
      if (y_valid) move_xz(cur.x + vx, cur.z + vz);
      else moved = ent_state.actor.move_on_mesh(walk, vx, vz, heading);
      if (!touches_leader(i)) return moved;
      ent_state.actor.restore(before);
      return true;
    };
    switch (motion.kind) {
      case K::WalkXZ:
      case K::WalkXZ2:
        if (!motion.direct) {
          // WALK_TO_XZ (42 / 43) and MOVE (46) do not write the position: they set the velocity and
          // the actor moves with the wall probe and slide, like the pad (a free-height entity without them). An NPC therefore stops
          // at fences / walls (and keeps pushing until the slot timer of 43 runs out) instead of walking through them; the arrival
          // test is dist > S / 8, then the position snaps to the target. 3F / B8 / B9 / BA (direct position writes) stay unconstrained.
          // The engine aims from the whole-unit position: the offset to the target is an integer vector, its length goes through
          // mover_sqrt and its direction through mover_normalise (4.12), times S (16.16 velocity).
          const int ix = static_cast<int>(motion.tx) - static_cast<int>(std::floor(cur.x));
          const int iz = static_cast<int>(motion.tz) - static_cast<int>(std::floor(cur.z));
          const float dx = static_cast<float>(ix), dz = static_cast<float>(iz);
          const int raw_speed = script_ent.v10c ? script_ent.v10c : kDefaultSpeed;
          if (!(mover_sqrt(ix * ix + iz * iz) * 16 > 2 * raw_speed)) {
            move_xz(motion.tx, motion.tz);
            motion.active = false;
            break;
          }
          ent_state.actor.set_facing(motion.facing);
          const int offset[3] = {ix, 0, iz};
          int unit[3];
          mover_normalise(offset, unit);
          float vx = static_cast<float>(unit[0] * raw_speed) / 65536.0f, vz = static_cast<float>(unit[2] * raw_speed) / 65536.0f;
          if (script_ent.flags270 & 0x2000000u) {
            // Curved walk (FE 2D): the per-slot heading turns toward the bearing to the target by the curve turn rate
            // per tick, the model yaw is written from it and the velocity follows it, so the entity walks an
            // arc instead of a straight line (the model faces the heading).
            int& ch = ent_state.curve_h[static_cast<size_t>(motion.slot)];
            ch = turn_toward(ch, bearing(dx, dz), script_ent.v114);
            ent_state.actor.set_shown_facing(ch);
            const float heading_rad = static_cast<float>(ch) * kUnitsToRad;
            vx = -std::cos(heading_rad) * speed;
            vz = std::sin(heading_rad) * speed;
          }
          if (move_by(vx, vz, motion.facing)) motion.stuck = 0;
          else ++motion.stuck;
          // The slot timer of 43 ends the walk; so does being stuck too long (never leave a script waiting on a wall for ever).
          if (motion.frames > 0 && --motion.frames == 0) motion.active = false;
          if (motion.stuck > kStuckTicks) motion.active = false;
          break;
        }
        [[fallthrough]];
      case K::Walk:
      case K::Walk2: {
        if (motion.target_y && free_y) {
          // WALK_TO with a height on a free-height entity (flyers, lifts) travels the 3D distance at the entity's speed;
          // a pure vertical move (opn_a01i's lift: x, z unchanged) therefore takes distance / speed ticks, not one.
          const float dx = motion.tx - cur.x, dz = motion.tz - cur.z, dy = motion.ty - cur.y;
          const float distance = std::sqrt(dx * dx + dz * dz + dy * dy);
          if (distance <= 0.01f) {
            motion.active = false;
            if (motion.direct) {
              ent_state.actor.set_position(walk, motion.tx, motion.tz, false, false);
              if (motion.final_facing >= 0 && script_ent.rec[static_cast<size_t>(motion.gate_slot & 7)].flag47 == 0 && script_ent.v24e != 0) ent_state.actor.set_facing(to_internal(motion.final_facing));
            }
            break;
          }
          const float ratio = std::min(speed, distance) / distance;
          move_xz(cur.x + dx * ratio, cur.z + dz * ratio);
          ent_state.actor.set_height(cur.y + dy * ratio, false);
          ent_state.actor.set_facing(motion.facing);
          if (speed >= distance) {
            move_xz(motion.tx, motion.tz);
            ent_state.actor.set_height(motion.ty, false);
            if (!motion.direct) motion.active = false;
          } else if (motion.frames > 0 && --motion.frames == 0) {
            // 40's slot timer ran out while still far.
            motion.active = false;
          }
          break;
        }
        if (walk_toward(motion.tx, motion.tz, 0, false)) {
          move_xz(motion.tx, motion.tz);
          motion.active = false;
        } else if (motion.frames > 0 && --motion.frames == 0) {
          // 40's slot timer ran out while still far.
          motion.active = false;
        }
        break;
      }
      case K::FollowEnt:
      case K::FollowEnt3D:
      case K::FollowEntTimed:
      case K::FollowEnt3DTimed: {
        if (motion.target_ent < 0 || motion.target_ent >= static_cast<int>(ents_.size())) {
          motion.active = false;
          break;
        }
        const ActorPose& target_pose = ents_[static_cast<size_t>(motion.target_ent)].actor.current();
        if (motion.kind == K::FollowEnt3D || motion.kind == K::FollowEnt3DTimed) {
          // B8 / B9 wait form: the chase of 44 / 45 in 3D. Faces the target (gated like 42), then while the 3D distance exceeds
          // the same stop distance the position moves straight toward the target's position by S (a direct write: no wall probe,
          // the height free), so a flyer follows a guide entity through the air. At the stop distance it stands.
          const float dx = target_pose.x - cur.x, dy = target_pose.y - cur.y, dz = target_pose.z - cur.z;
          const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
          const float stop = 2.0f * speed + ent_radius(motion.target_ent) / 16.0f + ent_radius(i);
          if (distance <= stop) {
            motion.active = false;
            break;
          }
          if (turn_to_walk && (dx != 0.0f || dz != 0.0f)) ent_state.actor.set_facing(bearing(dx, dz));
          ent_state.actor.set_position(walk, cur.x + dx / distance * speed, cur.z + dz / distance * speed, false, true);
          ent_state.actor.set_height(cur.y + dy / distance * speed, false);
          if (motion.frames > 0 && --motion.frames == 0) motion.active = false;
          break;
        }
        // 44 / 45 wait form: faces the target (gated like 42), then while `16 d > 2 S + r_target + 16 r_self`
        // (S = speed; the target's radius is added unscaled: default stop = 10 + 2.4 + 38 units)
        // it sets the VELOCITY to unit(target - self) * S, and the entity moves through the wall probe and the slide
        // exactly like the pad: a chasing monster slides along fences and never leaves the mesh. At the stop distance
        // it stands (no snap).
        const float dx = target_pose.x - cur.x, dz = target_pose.z - cur.z;
        const float distance = std::hypot(dx, dz);
        const float stop = 2.0f * speed + ent_radius(motion.target_ent) / 16.0f + ent_radius(i);
        const bool done = distance <= stop;
        if (!done) {
          float vx = dx / distance * speed, vz = dz / distance * speed;
          if ((motion.kind == K::FollowEnt || motion.kind == K::FollowEntTimed) && (script_ent.flags270 & 0x2000000u)) {
            // Curved 44 / 45: the slot heading turns toward the target's bearing by the curve turn rate per tick and the
            // velocity follows it. The model yaw is written as the heading itself (no quarter-turn offset, unlike 42 / 43), so the model
            // is shown a quarter turn off the heading before it turns back toward the facing at the turn rate.
            int& ch = ent_state.curve_h[static_cast<size_t>(motion.slot)];
            ch = turn_toward(ch, bearing(dx, dz), script_ent.v114);
            ent_state.actor.set_shown_facing(ch + 0x400);
            const float heading_rad = static_cast<float>(ch) * kUnitsToRad;
            vx = -std::cos(heading_rad) * speed;
            vz = std::sin(heading_rad) * speed;
          }
          // A slide re-aims the facing along the wall.
          ent_state.actor.set_facing(turn_to_walk ? bearing(dx, dz) : motion.facing);
          move_by(vx, vz, -1);
        }
        bool timeout = false;
        if (motion.frames > 0 && --motion.frames == 0) timeout = true;
        if (done || timeout) motion.active = false;
        break;
      }
      case K::Move:
      case K::Move3D: {
        if (motion.kind == K::Move3D) {
          // BA wait form: the facing saved by the start form, then while the 3D distance to the stored target exceeds
          // S / 8 the position moves by the unit vector toward it x S (a direct write, no wall probe; the height stays
          // free), the slot counter (N + 1 steps in all) ending the wait; at the target the wait ends without a step.
          ent_state.actor.set_facing(motion.facing);
          const float dx = motion.tx - cur.x, dy = motion.ty - cur.y, dz = motion.tz - cur.z;
          const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
          if (!(distance > 2.0f * speed)) {
            motion.active = false;
            break;
          }
          ent_state.actor.set_position(walk, cur.x + dx / distance * speed, cur.z + dz / distance * speed, false, true);
          ent_state.actor.set_height(cur.y + dy / distance * speed, false);
          if (--motion.frames <= 0) motion.active = false;
          break;
        }
        // Op 46: a velocity walk (see WalkXZ).
        ent_state.actor.set_facing(motion.facing);
        // Wait form: within S/8 of the stored target (twice the walk's length away, see start_motion) the walk ends without a snap.
        const float dx = motion.tx - cur.x, dz = motion.tz - cur.z;
        if (!(std::hypot(dx, dz) > 2.0f * speed)) {
          motion.active = false;
          break;
        }
        const float heading_rad = static_cast<float>(motion.heading) * kUnitsToRad;
        float vx = -std::cos(heading_rad) * speed, vz = std::sin(heading_rad) * speed;
        if (script_ent.flags270 & 0x2000000u) {
          // Curved 46: as 42 / 43 but from the slot's last heading, and the model yaw is the heading + 0x400:
          // the model is shown facing back along the heading, then turns toward the facing.
          int& ch = ent_state.curve_h[static_cast<size_t>(motion.slot)];
          ch = turn_toward(ch, bearing(dx, dz), script_ent.v114);
          ent_state.actor.set_shown_facing(ch + 0x800);
          const float hc = static_cast<float>(ch) * kUnitsToRad;
          vx = -std::cos(hc) * speed;
          vz = std::sin(hc) * speed;
        }
        move_by(vx, vz, motion.facing);
        if (--motion.frames <= 0) motion.active = false;
        break;
      }
      case K::Tween:
      case K::Jump: {
        // MOVE_TWEEN / JUMP_MOVE wait form, once per tick: n ballistic steps, then one tick that snaps to the target (x, z and the landing
        // height: 5B's own operand, 5C's floor).
        if (motion.elapsed < motion.total) {
          ent_state.actor.set_position(walk, cur.x + motion.step_x, cur.z + motion.step_z, false, true);
          ent_state.actor.set_height(cur.y + motion.vy, false);
          motion.vy += 2.0f;
          // Once the velocity points down the engine switches the clip to the fall slot (SET_WALK_ANIMATION 3; engine default 0)
          // and sets the anim-done bit so a WAIT_ANIMATION on the take-off clip ends.
          const int fall = ent_state.actor.anim_slot(kSlotE6);
          if (motion.vy > 0.0f && ent_state.actor.requested_anim() != fall) ent_state.actor.play_engine_clip(fall);
          ++motion.elapsed;
        } else {
          ent_state.actor.set_position(walk, motion.tx, motion.tz, false, true);
          ent_state.actor.set_height(motion.ty, false);
          motion.active = false;
        }
        break;
      }
    }
  }
}

void FieldWorld::step_player() {
  lead_start_valid_ = false;
  const int player = player_entity();
  if (player < 0) return;
  Ent& pe = ents_[static_cast<size_t>(player)];
  pe.actor.speed = vm_.entity(player).v10c ? vm_.entity(player).v10c : kDefaultSpeed;
  // Cheats: run speed x2; no clip (Actor::tick moves without the walk mesh).
  if (platform::cheats().run_speed) pe.actor.speed *= 2;
  pe.actor.noclip = platform::cheats().no_clip;
  // Script-driven (step_visuals animates it).
  if (pe.motion.active || !pe.placed) return;
  bool run = false;
  int heading = kNoHeading;
  if (controls_free() && !(vm_.global_flags() & script::gflag::kFormationHold)) {
    const bool auto_active = debug_.auto_walk && auto_target_ >= 0 && tick_count_ < static_cast<u64>(debug_.action_tick);
    if (auto_active) {
      const ActorPose& pose = pe.actor.current();
      const ActorPose& target_pose = ents_[static_cast<size_t>(auto_target_)].actor.current();
      // Boxes (exits) are walked into; circles are approached to talking range.
      const float stop = ent_box(auto_target_) ? 0.0f : ent_radius(player) + ent_radius(auto_target_) + kTalkMargin * 0.6f;
      if (debug_.teleport && !auto_teleported_ && !ent_box(auto_target_)) {
        // Test hook: stand next to the target (a free spot on the walk mesh, facing it) instead of walking there.
        auto_teleported_ = true;
        const WalkMesh& walk = view_.walk();
        for (int ring = 0; ring < 4; ++ring)
          for (int k = 0; k < 16; ++k) {
            const float ang = static_cast<float>(k) * 2.0f * kPi / 16.0f, radius = stop - 6.0f + 20.0f * static_cast<float>(ring);
            const float x = target_pose.x + std::sin(ang) * radius, z = target_pose.z - std::cos(ang) * radius;
            const int tri = walk.find_triangle(x, z, target_pose.y);
            if (tri < 0 || walk.blocked(tri, 0)) continue;
            pe.actor.place(walk, tri, x, z, bearing(target_pose.x - x, target_pose.z - z));
            ring = 4;
            break;
          }
      }
      if (std::hypot(target_pose.x - pose.x, target_pose.z - pose.z) > stop) heading = bearing(target_pose.x - pose.x, target_pose.z - pose.z);
    } else {
      heading = pad_heading(run);
      // Flu wobbles the heading; sprain forbids running.
      if (heading != kNoHeading) heading = (heading + status_flu_wobble()) & 0xFFF;
    }
    if (status_no_run()) run = false;
    if (vm_.global_flags() & script::gflag::kForceRun) run = true;
    if (heading != kNoHeading && pe.actor.triangle() >= 0) status_on_move(player, view_.walk().flags(pe.actor.triangle()), run);
  }
  // A scripted move (MOVE / WALK_TO with a heading) can leave the leader off the walk mesh; once the player has the controls
  // back, put it on the nearest walkable spot instead of freezing it there.
  if (controls_free() && heading != kNoHeading && !view_.walk().empty() && !pe.actor.noclip) {
    const WalkMesh& mesh = view_.walk();
    const ActorPose& cp = pe.actor.current();
    if (pe.actor.triangle() < 0 || !mesh.contains(pe.actor.triangle(), cp.x, cp.z)) {
      int tri = mesh.find_triangle(cp.x, cp.z, cp.y);
      float px = cp.x, pz = cp.z;
      if (tri < 0 && mesh.nearest_vertex(cp.x, cp.z, 0, &tri) >= 0 && tri >= 0) {
        const auto centroid = mesh.centroid(tri);
        px = centroid[0];
        pz = centroid[1];
      }
      if (tri >= 0) {
        log::debug("field: tick {} player at {:.0f},{:.0f} is off the walk mesh; placed at {:.0f},{:.0f}", tick_count_, static_cast<double>(cp.x),
                   static_cast<double>(cp.z), static_cast<double>(px), static_cast<double>(pz));
        pe.actor.place(mesh, tri, px, pz, pe.actor.facing());
      }
    }
  }
  const Actor::Spot before = pe.actor.spot();
  lead_start_ = trail_entry_of(pe.actor);
  lead_start_facing_ = pe.actor.facing();
  lead_start_valid_ = true;
  // Pad control re-selects the clip every tick.
  if (controls_free() && !pe.motion.active) pe.actor.clear_play();
  // Right stick: turn on the spot.
  if (controls_free() && heading == kNoHeading && input_.pad_rstick && !pe.motion.active)
    pe.actor.set_facing(static_cast<int>(std::lround(std::atan2(input_.pad_ry, -input_.pad_rx) * kUnitsPerRad)) & 0xFFF);
  pe.actor.force_run = (vm_.global_flags() & script::gflag::kForceRun) != 0;
  pe.actor.tick(view_.walk(), heading, run);
  pe.motion.walking = false;
  if (heading != kNoHeading) {
    const ActorPose& next = pe.actor.current();
    if (!pe.actor.noclip && blocked_by_solids(player, next.x, next.z, before.x, before.z)) pe.actor.restore(before);
  } else {
    for (Ent& other : ents_) {
      // A tick without a press resets every actor's counter; a pushed actor still takes its velocity (the no-hit branch
      // runs whatever the player does).
      other.press_ticks = 0;
      if (other.pend) apply_push(other);
    }
  }
  // The trail only grows on ticks with a pad heading, and records the spot the controlled entity stood on before this tick's move.
  if (heading != kNoHeading) {
    trail_push(0, lead_start_);
    lead_start_pushed_ = true;
  }
}

FieldWorld::TrailEntry FieldWorld::trail_entry_of(const Actor& actor) const {
  const ActorPose& pose = actor.current();
  return {pose.x, pose.y, pose.z, actor.triangle(), view_.walk().flags(actor.triangle())};
}
void FieldWorld::trail_fill(int ring, const TrailEntry& entry) {
  Trail& trail = trails_[static_cast<size_t>(ring)];
  for (auto& slot : trail.entries) slot = entry;
  trail.head = 0;
}
void FieldWorld::trail_push(int ring, const TrailEntry& entry) {
  Trail& trail = trails_[static_cast<size_t>(ring)];
  trail.entries[static_cast<size_t>(trail.head)] = entry;
  trail.head = (trail.head + 1) & 0x7F;
}
void FieldWorld::reset_trails() {
  const int leader = party_ent_[0];
  if (leader < 0) return;
  // SETUP_PC of slot k < 2 fills ring k with ITS OWN entity's position (follower 1 spawns in the V, and a ring 1 that started at
  // the leader's spot would leave a > 50-unit gap before follower 1's first entries: follower 2 would step back and forth over
  // the fill entry under its feet for ever).
  trail_fill(0, trail_entry_of(ents_[static_cast<size_t>(leader)].actor));
  lead_pad_end_ = trail_entry_of(ents_[static_cast<size_t>(leader)].actor);
  const int f1 = party_ent_[1];
  trail_fill(1, trail_entry_of(ents_[static_cast<size_t>(f1 >= 0 && ents_[static_cast<size_t>(f1)].placed ? f1 : leader)].actor));
}

// Follow rule 1 (leader distance < 80): walk to the point 128 units from the leader on the line through this member, swung 0x80 to the
// side away from the leader's facing, at the member's walk speed. a = atan2(leader - self) (0 when the two coincide), side =
// ((leader facing + a) & 0xFFF) > 0x7FF + k, where k = follower index: at the exact tie (sum 0x800) follower 1 swings one way and
// follower 2 the other, which is what fans the party out into a row when all three share a spot.
// The step is the engine's: the 3D unit vector to the point (the leader's height) times the speed, written straight into the position
// - no probe, no slide, no mesh test. The member keeps facing the leader (engine facing -swung angle) and steps back with the walk
// clip played in reverse for the tick.
// Deviation: a free step that would land off the mesh (or on another floor) is replaced by the probe / slide step, and a
// blocked one by standing - at a cramped entrance the engine's rule parks the member in the wall / water until the leader walks
// away. Rule 3 (the trail replay) stays free.
void FieldWorld::follower_back_off(int slot, bool other_side, int ent_override) {
  const int lead = player_entity();
  const int ent = ent_override >= 0 ? ent_override : party_ent_[static_cast<size_t>(slot)];
  if (lead < 0 || ent < 0) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  ActorPose lp = ents_[static_cast<size_t>(lead)].actor.current();
  int lead_facing = ents_[static_cast<size_t>(lead)].actor.facing();
  if (lead_start_valid_) {
    lp.x = lead_start_.x;
    lp.y = lead_start_.y;
    lp.z = lead_start_.z;
    lead_facing = lead_start_facing_;
  }
  const ActorPose pose = ent_state.actor.current();
  const int ring = slot - 1;
  const int base = vm_.entity(ent).v10c ? vm_.entity(ent).v10c : kDefaultSpeed;
  const float dx = pose.x - lp.x, dz = pose.z - lp.z;
  const bool same = std::hypot(dx, dz) < 0.01f;
  // angle = engine angle to the leader; lfe = engine heading of the leader; other_side: settle_spawned_followers swings
  // follower 2 away from follower 1.
  const int angle = same ? 0 : (-bearing(dx, dz)) & 0xFFF;
  const int lfe = (lead_facing - 0x800) & 0xFFF;
  const int sum = (lfe + angle) & 0xFFF;
  const bool swing = (sum > 0x7FF + ring) != other_side;
  const int ar = (angle + (swing ? 0x80 : -0x80)) & 0xFFF;
  const float ang = -static_cast<float>(ar) * kUnitsToRad;
  const float tx = lp.x - std::cos(ang) * 128.0f, tz = lp.z + std::sin(ang) * 128.0f;
  ent_state.motion.walking = true;
  ent_state.actor.set_facing((0x800 - ar) & 0xFFF);
  ent_state.actor.reverse_clip();
  const float mx = tx - pose.x, my = lp.y - pose.y, mz = tz - pose.z;
  const float flat_dist = std::hypot(mx, mz);
  if (flat_dist >= 0.01f) {
    // Velocity is 16.16: units/tick = speed / 16.
    const float spd = static_cast<float>(base) / 16.0f;
    const float n3 = std::sqrt(mx * mx + my * my + mz * mz);
    const float nx = pose.x + mx / n3 * spd, ny = pose.y + my / n3 * spd, nz = pose.z + mz / n3 * spd;
    const WalkMesh& walk = view_.walk();
    const int tri = walk.empty() ? -1 : walk.find_triangle(nx, nz, ny);
    if (walk.empty() || (tri >= 0 && std::fabs(walk.height(tri, nx, nz) - ny) <= 64.0f)) ent_state.actor.move_free(nx, ny, nz, tri);
    else if (!ent_state.actor.move_on_mesh(walk, mx / flat_dist * spd, mz / flat_dist * spd)) ent_state.motion.walking = false;
  }
  if (ring == 0) trail_push(1, trail_entry_of(ent_state.actor));
}

// Followers spawn on the leader's spot (every member takes the same entrance record) and the engine spreads them with
// rule 1 once their UPDATE_CHARACTER runs. A scene that starts at once (a dialogue behind a fade) freezes that
// logic first (the party scripts set a room flag that skips UPDATE_CHARACTER), so the party stands inside one another until the
// script walks it out. This runs rule 1 here, before the first frame, for the members still standing on the spawn spot: the same
// walk (3D distance to the leader, the leader's height, mesh-bounded as in follower_back_off) the engine plays over the next ticks; at a
// cramped entrance it may stop short of 80 units.
void FieldWorld::settle_spawned_followers() {
  lead_start_valid_ = false;
  const int lead = party_ent_[0];
  if (lead < 0) return;
  const WalkMesh& walk = view_.walk();
  const ActorPose& lp = ents_[static_cast<size_t>(lead)].actor.current();
  const bool f1_stacked = party_ent_[1] >= 0 && ents_[static_cast<size_t>(party_ent_[1])].spawned_on_leader &&
                          std::hypot(ents_[static_cast<size_t>(party_ent_[1])].actor.current().x - lp.x,
                                     ents_[static_cast<size_t>(party_ent_[1])].actor.current().z - lp.z) <= 1.0f;
  for (int slot = 1; slot < 3; ++slot) {
    const int ent = party_ent_[static_cast<size_t>(slot)];
    if (ent < 0) continue;
    Ent& ent_state = ents_[static_cast<size_t>(ent)];
    if (!ent_state.spawned_on_leader || !ent_state.placed) continue;
    ent_state.spawned_on_leader = false;
    // A script moved it in INIT: its placement stands.
    if (std::hypot(ent_state.actor.current().x - lp.x, ent_state.actor.current().z - lp.z) > 1.0f) continue;
    // Rule 1 sends both followers the same way when the leader does not face exactly 0x800 (the side test only differs at the tie), so
    // two members spawned on one spot would leave together and still overlap each other: the second swings to the other side.
    const bool other_side = slot == 2 && f1_stacked;
    for (int i = 0; i < 80; ++i) {
      const ActorPose pose = ent_state.actor.current();
      const float distance = std::sqrt((pose.x - lp.x) * (pose.x - lp.x) + (pose.y - lp.y) * (pose.y - lp.y) + (pose.z - lp.z) * (pose.z - lp.z));
      if (distance >= 80.0f) break;
      follower_back_off(slot, other_side);
    }
    ent_state.motion.walking = false;
    ent_state.motion.running = false;
    ent_state.actor.snap_facing();
    const ActorPose settled = ent_state.actor.current();
    // No glide from the spawn spot; the height of the triangle it ends on.
    ent_state.actor.set_position(walk, settled.x, settled.z, true, false);
    log::debug("field: follower {} settled at {:.0f},{:.0f} ({:.0f} from the leader)", slot, static_cast<double>(settled.x), static_cast<double>(settled.z),
               static_cast<double>(std::hypot(settled.x - lp.x, settled.z - lp.z)));
  }
  // The rings start at the leader's spot, not with the settling steps.
  reset_trails();
}

// Party followers (non-leader branch). Follower 1 replays the leader's ring, follower 2 replays
// follower 1's ring. They stand still while within 200 units of the member they follow, so they stop spaced behind the leader
// instead of converging on it; a follower closer than 80 units to the leader backs off to 128 units from it.
// The replay step is a direct position write (position += unit(entry - self) * speed in 3D, then the entry's
// tile / flags / normal are copied); followers never go through the mover's probe / slide. The ring is the leader's own walk,
// so the replay stays on the mesh except for the straight cut between two entries at a corner. A mesh-constrained step would
// wedge followers on corners and edges and leave them behind.
// "The leader" is the controlled entity: when a script hands the pad to another entity, ring 0 records that entity's walk, rule 1
// keeps the followers off it and follower 1 follows it; the controlled entity itself never takes the follower branch.
void FieldWorld::step_followers() {
  const int lead = player_entity();
  if (lead < 0) return;
  const ActorPose& lp = ents_[static_cast<size_t>(lead)].actor.current();
  // The follow rules measure against the leader's spot before this tick's pad move.
  ActorPose lead_pose = lp;
  if (lead_start_valid_) {
    lead_pose.x = lead_start_.x;
    lead_pose.y = lead_start_.y;
    lead_pose.z = lead_start_.z;
  }
  if (lead_start_pushed_) lead_pad_end_ = trail_entry_of(ents_[static_cast<size_t>(lead)].actor);
  const bool pad_pushed = lead_start_pushed_;
  lead_start_pushed_ = false;
  if (follow_hold_ && follow_hold_ticks_ > 0) --follow_hold_ticks_;
  // A leader moved by a script (MOVE / WALK_TO, e.g. opn_a01i's run out of the lift) must grow the ring too, or the followers
  // resumed afterwards replay the spawn position and walk back to where the party started. The spot a pad walk ended on is
  // not such a move: the ring keeps the spot before the last pad step. Only the update op grows the ring: a controlled entity that
  // does not run it (a flyer selected with FE 4C so that it fires its guide's collision) leaves the followers standing.
  if (!pad_pushed && ents_[static_cast<size_t>(lead)].updating && std::hypot(lp.x - lead_pad_end_.x, lp.z - lead_pad_end_.z) > 0.5f) {
    const TrailEntry& last = trails_[0].entries[static_cast<size_t>((trails_[0].head - 1) & 0x7F)];
    if (std::hypot(lp.x - last.x, lp.z - last.z) > 0.5f) trail_push(0, trail_entry_of(ents_[static_cast<size_t>(lead)].actor));
  }
  // Likewise follower 1 moved by a script grows ring 1 (follower 2 replays it).
  if (party_ent_[1] >= 0) {
    const Ent& follower = ents_[static_cast<size_t>(party_ent_[1])];
    const ActorPose& fp = follower.actor.current();
    const TrailEntry& last = trails_[1].entries[static_cast<size_t>((trails_[1].head - 1) & 0x7F)];
    if (follower.motion.active && std::hypot(fp.x - last.x, fp.z - last.z) > 0.5f) trail_push(1, trail_entry_of(follower.actor));
  }
  // The walk / run clip plays only while a follower really moves this tick: clear the flags before any early return, or a follower
  // frozen by the script (lift ride, formation lock, dialogue) would keep "walking on the spot".
  for (int slot = 1; slot < 3; ++slot) {
    const int ent = party_ent_[static_cast<size_t>(slot)];
    if (ent < 0 || ent == lead) continue;
    // A member under script motion (WALK_TO / MOVE of a cutscene) keeps its flag: it plays the walk clip while the script moves it.
    if (ents_[static_cast<size_t>(ent)].motion.active) continue;
    ents_[static_cast<size_t>(ent)].motion.walking = false;
    ents_[static_cast<size_t>(ent)].motion.running = false;
  }
  // The party hold (a formation 84 / DF, the end of BE, 86 mode 0; released by 86 mode 1 and the room load): the update op neither
  // follows nor takes the pad.
  if (vm_.global_flags() & script::gflag::kFormationHold) return;
  // The engine measures between whole-unit positions and floors the root: a member 200.9 units away counts as 200.
  auto dist3 = [](const ActorPose& pose, float x, float y, float z) {
    const float dx = std::floor(pose.x) - std::floor(x), dy = std::floor(pose.y) - std::floor(y), dz = std::floor(pose.z) - std::floor(z);
    return std::floor(std::sqrt(dx * dx + dy * dy + dz * dz));
  };
  // One member's turn of the non-leader branch. `slot` picks the ring (k = slot - 1) and the speed step, `prev` is the member it follows.
  auto follow_member = [&](int ent, int slot, int prev) {
    Ent& ent_state = ents_[static_cast<size_t>(ent)];
    // A scripted move in progress: the script owns the member (walk clip, position).
    if (ent == lead || ent_state.motion.active) return;
    ent_state.motion.walking = false;
    ent_state.motion.running = false;
    if (!ent_state.updating) return;
    // The party-follow logic runs every tick and re-selects the clip itself (stand / walk / run), which
    // releases any script animation (an ex clip held after its one-shot) once the member is under normal control again.
    ent_state.actor.clear_play();
    const int ring = slot - 1;
    if (prev < 0) return;
    const ActorPose pose = ent_state.actor.current();
    const ActorPose& pp = prev == lead ? lead_pose : ents_[static_cast<size_t>(prev)].actor.current();
    const float d_lead = dist3(pose, lead_pose.x, lead_pose.y, lead_pose.z);
    const float d_prev = dist3(pose, pp.x, pp.y, pp.z);
    const int base = vm_.entity(ent).v10c ? vm_.entity(ent).v10c : kDefaultSpeed;
    // Too close to the leader: rule 1 (skipped while op BE waits).
    if (d_lead < 80.0f && !follow_hold_) {
      follower_back_off(slot, false, ent);
      return;
    }
    // Within 200 units of the followed member: stand.
    if (d_prev <= 200.0f) return;
    // Newest ring entry within 50 units of this member (100 for the whole scan when the newest entry's tile or the member's own has
    // flags 0xC800); none -> the newest entry.
    const Trail& tr = trails_[static_cast<size_t>(ring)];
    const int idx = (tr.head - 1) & 0x7F;
    int found = idx;
    const float near = ((ent_state.actor.tile_flags() | tr.entries[static_cast<size_t>(idx)].flags) & 0xC800u) ? 100.0f : 50.0f;
    for (int i = 0; i < 0x80; ++i) {
      const TrailEntry& candidate = tr.entries[static_cast<size_t>((tr.head - 1 - i) & 0x7F)];
      if (dist3(pose, candidate.x, candidate.y, candidate.z) < near) {
        found = (tr.head - 1 - i) & 0x7F;
        break;
      }
    }
    float spd = static_cast<float>(base - (ring + 1) * 16);
    bool run = false;
    if (d_prev > 230.0f) {
      spd += static_cast<float>(base >> 1) + 32.0f;
      run = true;
    }
    if (d_prev >= 351.0f) {
      spd = static_cast<float>(base * 3);
      run = true;
    } else if (d_prev < 250.0f) {
      run = false;
    }
    ent_state.motion.walking = true;
    ent_state.motion.running = run;
    // Broken chain guard: the carrot is normally 50 units ahead and recedes as the ring grows; an entry already reached that
    // is not the newest means every newer entry is more than 50 units away (the followed member was teleported / placed by a script),
    // and the engine's full step would swing past it and back for ever. Aim at the newest entry instead (a straight free walk).
    if (found != idx && dist3(pose, tr.entries[static_cast<size_t>(found)].x, tr.entries[static_cast<size_t>(found)].y, tr.entries[static_cast<size_t>(found)].z) <= spd / 16.0f) found = idx;
    const TrailEntry& target = tr.entries[static_cast<size_t>(found)];
    const float dx = target.x - pose.x, dy = target.y - pose.y, dz = target.z - pose.z;
    const float n3 = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (n3 >= 0.01f) {
      // Velocity is 16.16: units/tick = speed / 16; the full step even past the entry (no clamp).
      const float ratio = spd / 16.0f / n3;
      if (std::hypot(dx, dz) >= 0.01f) ent_state.actor.set_facing(bearing(dx, dz));
      ent_state.actor.move_free(pose.x + dx * ratio, pose.y + dy * ratio, pose.z + dz * ratio, target.tri);
    } else {
      ent_state.motion.walking = false;
    }
    {
      const ActorPose& moved = ent_state.actor.current();
      log::debug("field: tick {} follower {} pos {:.1f},{:.1f},{:.1f} tri {} target {:.1f},{:.1f},{:.1f} ({} back) dP {:.0f}", tick_count_, ring,
                 static_cast<double>(moved.x), static_cast<double>(moved.y), static_cast<double>(moved.z), ent_state.actor.triangle(), static_cast<double>(target.x),
                 static_cast<double>(target.y), static_cast<double>(target.z), (tr.head - 1 - found) & 0x7F, static_cast<double>(d_prev));
    }
    if (ring == 0) trail_push(1, trail_entry_of(ent_state.actor));
  };
  for (int slot = 1; slot < 3; ++slot) {
    const int ent = party_ent_[static_cast<size_t>(slot)];
    if (ent < 0 || slot >= static_cast<int>(party_chars_.size())) continue;
    follow_member(ent, slot, slot == 1 ? lead : party_ent_[1]);
  }
  // Op 0A / 0B applies to ANY entity, not only the three party members. A non-party entity whose script runs it
  // (and is not player-controlled) takes the follower branch with ring index 1: it trails follower 1's path like a third
  // follower - rule 1 away from the leader, stand within 200 of follower 1, else replay ring 1.
  if (party_ent_[1] >= 0)
    for (int i = 0; i < static_cast<int>(ents_.size()); ++i) {
      const Ent& other = ents_[static_cast<size_t>(i)];
      if (i == party_ent_[0] || i == party_ent_[1] || i == party_ent_[2] || other.pc_slot >= 0 || !other.placed || !other.updating) continue;
      // Flag bit 0 selects the player-controlled branch.
      if (vm_.entity(i).flags38 & 0x1) continue;
      follow_member(i, 2, party_ent_[1]);
    }
}

}  // namespace oc::game
