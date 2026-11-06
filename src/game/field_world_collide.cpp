// FieldWorld: positions and bounding boxes of entities, solid and push collision, box triggers, culling and path finding.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <queue>

#include "core/log.h"
#include "game/field_world_internal.h"

namespace oc::game {

using namespace field_internal;

namespace {

// Box triggers test the outer corner set = half extents + 0x20.
constexpr float kBoxMargin = 32.0f;

}  // namespace

bool FieldWorld::player_xz(float& x, float& z) const {
  const int player = player_entity();
  if (player < 0 || player >= static_cast<int>(ents_.size()) || !ents_[static_cast<size_t>(player)].placed) return false;
  x = ents_[static_cast<size_t>(player)].actor.current().x;
  z = ents_[static_cast<size_t>(player)].actor.current().z;
  return true;
}

bool FieldWorld::entity_xz(int ent, float& x, float& z) const {
  // Route-test queries use negative ids: -4000 - n = entity n's script pitch (x) and whether it is drawn (z = 1); -3000 - n = entity
  // n's height (x, world y, down = +) and clip state (z = submitted anim id, + 0x10000 on a stair tile); -2000 - n = entity n's state (x = room model, -1 none; z = bits); -1000 - n = entity n's off-mesh tick
  // count (x); -100 - n = entity n's layer-space screen position; -1 = the controlled entity's layer-space screen position (scroll included).
  if (ent <= -4000) {
    const int target = -4000 - ent;
    if (target >= static_cast<int>(ents_.size())) return false;
    const Ent& ent_state = ents_[static_cast<size_t>(target)];
    x = ent_state.actor.pitch;
    z = ent_state.actor.loaded() && !ent_not_drawn(target) ? 1.0f : 0.0f;
    return true;
  }
  if (ent <= -3000) {
    const int target = -3000 - ent;
    if (target >= static_cast<int>(ents_.size())) return false;
    const Actor& actor = ents_[static_cast<size_t>(target)].actor;
    x = actor.current().y;
    z = static_cast<float>(actor.submitted_anim() + ((actor.tile_flags() & 0xC000u) ? 0x10000 : 0));
    return true;
  }
  if (ent <= -2000) {
    const int target = -2000 - ent;
    if (target >= static_cast<int>(ents_.size())) return false;
    const Ent& ent_state = ents_[static_cast<size_t>(target)];
    int bits = 0;
    if (ent_state.actor.loaded()) bits |= 1;
    if (ent_state.actor.loaded() && !ent_not_drawn(target)) bits |= 2;
    if (ent_state.motion.active) bits |= 4;
    if (ent_state.motion.active && ent_state.motion.paused) bits |= 8;
    if (walk_flags(target) & 0x60000) bits |= 16;
    x = static_cast<float>(ent_state.model);
    z = static_cast<float>(bits);
    return true;
  }
  if (ent <= -1000) {
    const int target = -1000 - ent;
    if (target >= static_cast<int>(ents_.size())) return false;
    x = static_cast<float>(ents_[static_cast<size_t>(target)].off_mesh_ticks);
    z = 0;
    return true;
  }
  if (ent <= -100) {
    const int target = -100 - ent;
    if (target >= static_cast<int>(ents_.size())) return false;
    const ActorPose& pose = ents_[static_cast<size_t>(target)].actor.current();
    render::FieldCamera::Projected pr;
    if (!view_.camera().project({pose.x, pose.y, pose.z}, pr)) return false;
    x = pr.sx + static_cast<float>(view_.scroll_x());
    z = pr.sy + static_cast<float>(view_.scroll_y());
    return true;
  }
  if (ent == -1) {
    const int player = player_entity();
    if (player < 0 || player >= static_cast<int>(ents_.size())) return false;
    const ActorPose& pose = ents_[static_cast<size_t>(player)].actor.current();
    render::FieldCamera::Projected pr;
    if (!view_.camera().project({pose.x, pose.y, pose.z}, pr)) return false;
    x = pr.sx + static_cast<float>(view_.scroll_x());
    z = pr.sy + static_cast<float>(view_.scroll_y());
    return true;
  }
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  x = ents_[static_cast<size_t>(ent)].actor.current().x;
  z = ents_[static_cast<size_t>(ent)].actor.current().z;
  return true;
}

int FieldWorld::entity_facing(int ent) const {
  if (ent == -1) ent = player_entity();
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return 0;
  return to_script(ents_[static_cast<size_t>(ent)].actor.facing());
}

bool FieldWorld::entity_head(int ent, int& ax, int& az) const {
  if (ent == -1) ent = player_entity();
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  const Actor& actor = ents_[static_cast<size_t>(ent)].actor;
  if (!actor.head_active()) return false;
  ax = static_cast<int>(std::lround(actor.head_ax()));
  az = static_cast<int>(std::lround(actor.head_az()));
  return true;
}

int FieldWorld::entity_clip(int ent, bool source) const {
  if (ent == -1) ent = player_entity();
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return -1;
  const Actor& actor = ents_[static_cast<size_t>(ent)].actor;
  if (source) return actor.fade_source();
  if (actor.cur_clip() < 0) return -1;
  return (actor.cur_clip() << 12) | (static_cast<int>(actor.anim_time() * 4.0f + 0.5f) & 0xFFF);
}

int FieldWorld::entity_yaw(int ent) const {
  if (ent == -1) ent = player_entity();
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return 0;
  // Model yaw = facing - 0x400; reported as the facing it shows, so it equals entity_facing once the model has turned.
  return to_script(static_cast<int>(std::lround(ents_[static_cast<size_t>(ent)].actor.current().yaw)) + kModelYawBias);
}


float FieldWorld::ent_radius(int i) const {
  const u16 radius = vm_.entity(i).trigger_radius;
  return radius ? static_cast<float>(radius) : kDefaultRadius;
}
float FieldWorld::ent_height(int i) const {
  const u16 height = vm_.entity(i).trigger_height;
  return height ? static_cast<float>(height) : kDefaultHeight;
}
bool FieldWorld::ent_box(int i) const { return (vm_.entity(i).flags38 & 0x200) != 0; }

bool FieldWorld::in_box(int i, float px, float pz, float margin) const {
  // Oriented rectangle around the entity: half extents (rect_w, rect_h) = along the right / forward axes of its facing
  // (extents default to the radius).
  const script::Entity& script_ent = vm_.entity(i);
  const Actor& actor = ents_[static_cast<size_t>(i)].actor;
  const float hw = (script_ent.rect_w ? static_cast<float>(script_ent.rect_w) : kDefaultRadius) + margin;
  const float hh = (script_ent.rect_h ? static_cast<float>(script_ent.rect_h) : kDefaultRadius) + margin;
  const float facing = static_cast<float>(actor.facing()) * kUnitsToRad;
  const float dx = px - actor.current().x, dz = pz - actor.current().z;
  // Half extent w runs along the x axis at facing 0: with the heading
  // direction (-cos f, sin f) that is the forward axis; h runs along the perpendicular.
  const float lx = -dx * std::cos(facing) + dz * std::sin(facing);
  const float lz = dx * std::sin(facing) + dz * std::cos(facing);
  return std::fabs(lx) <= hw && std::fabs(lz) <= hh;
}

u32 FieldWorld::walk_flags(int i) const {
  // After the move, every entity but the controlled one takes `table[attr of its triangle]`, except that an
  // entity with free height (op 26 / 3F with y) and the FE 19 bit clear gets 0. The controlled entity
  // always takes its triangle's flags. The flags feed the zone light, the shadow and the water rule:
  // a boat placed with op 26 above the water level is dry, so it is not filed behind the background.
  const Ent& ent_state = ents_[static_cast<size_t>(i)];
  const int tri = ent_state.actor.triangle();
  if (tri < 0 || view_.walk().empty()) return 0;
  const script::Entity& script_ent = vm_.entity(i);
  if (i != player_entity() && (script_ent.flags38 & 0xC0) == 0x40 && !(script_ent.flags270 & 0x80000)) return 0;
  return view_.walk().flags(tri);
}

// Not DRAWN: the draw passes skip an actor with flag bit 0x20 (TOGGLE_COLLISION b != 0), 0x4000 or 0x8000 (off screen, step_cull) -
// never bit 0x10. SET_VISIBLE 0 only sets 0x10 = inactive (no script, no anim update, no collision / talk), so the model stays on
// screen frozen. An empty party slot (no model) stays out via slot_inactive.
bool FieldWorld::ent_not_drawn(int i) const {
  const u32 flags = vm_.entity(i).flags38;
  if (flags & (0x20 | script::flag38::kNo3dObject | script::flag38::kOffScreen)) return true;
  return (flags & script::flag38::kInactive) && ents_[static_cast<size_t>(i)].slot_inactive;
}

bool FieldWorld::ent_hidden(int i) const {
  return (vm_.entity(i).flags38 & (script::flag38::kInactive | 0x20 | script::flag38::kNo3dObject)) != 0;
}

float FieldWorld::ground_y(const Ent& ent_state) const {
  const WalkMesh& walk = view_.walk();
  const ActorPose& pose = ent_state.actor.current();
  const int tri = walk.empty() ? -1 : walk.find_triangle(pose.x, pose.z, pose.y);
  return tri >= 0 ? walk.height(tri, pose.x, pose.z) : pose.y;
}


bool FieldWorld::blocked_by_solids(int player, float x, float z, float old_x, float old_z) {
  const float py = ents_[static_cast<size_t>(player)].actor.current().y;
  const float rp = ent_radius(player), hp = ent_height(player);
  bool blocked = false;
  // The walk-mesh probe on the pending velocity of every pushed actor: a step the mesh refuses clears it (the actor then stays).
  for (Ent& pushed : ents_)
    if (pushed.pend && !push_step_ok(pushed, pushed.pend_x, pushed.pend_z)) pushed.pend = false;
  std::vector<char> fresh(ents_.size(), 0);
  for (int i = 1; i < static_cast<int>(ents_.size()); ++i) {
    if (i == player) continue;
    const script::Entity& script_ent = vm_.entity(i);
    Ent& ent_state = ents_[static_cast<size_t>(i)];
    if (!(script_ent.flags38 & 0x100) || (script_ent.flags38 & script::flag38::kInactive) || ent_state.pc_slot >= 0) {
      ent_state.press_ticks = 0;
      continue;
    }
    const float oy = ent_state.actor.current().y;
    if (!(py - hp <= oy && oy - ent_height(i) <= py)) {
      ent_state.press_ticks = 0;
      continue;
    }
    // The actor's centre / rect is at position + velocity.
    const float ox = ent_state.pend ? ent_state.pend_x : 0.0f, oz = ent_state.pend ? ent_state.pend_z : 0.0f;
    bool hit, was;
    // The raw hit test: the player's next position inside the actor.
    bool pressed;
    if (ent_box(i)) {
      hit = pressed = in_box(i, x - ox, z - oz, 0);
      was = in_box(i, old_x - ox, old_z - oz, 0);
      hit = hit && !was;
    } else {
      const float reach = rp + ent_radius(i);
      const ActorPose& other = ent_state.actor.current();
      const float cx = other.x + ox, cz = other.z + oz;
      hit = pressed = (x - cx) * (x - cx) + (z - cz) * (z - cz) < reach * reach;
      was = (old_x - cx) * (old_x - cx) + (old_z - cz) * (old_z - cz) < reach * reach;
      // Already overlapping: only moves that get further away are blocked-free.
      if (hit && was) {
        const float d_new = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        const float d_old = (old_x - cx) * (old_x - cx) + (old_z - cz) * (old_z - cz);
        hit = d_new < d_old;
      }
    }
    if (!pressed) {
      ent_state.press_ticks = 0;
      continue;
    }
    // FE 25: a long-pressed solid lets the player through.
    const bool pass = (script_ent.flags270 & 0x200000u) != 0 && ent_state.press_ticks >= 0x40;
    if (hit && !pass) blocked = true;
    ent_state.press_ticks = std::min(ent_state.press_ticks + 1, 0x7FFF);
    // FE 06: pushable.
    if (hit && (script_ent.flags38 & 0x2000u)) {
      // The pushed actor takes the player's pad velocity as it was before the player's own probe / slide / slope rebuild.
      const Actor& player_actor = ents_[static_cast<size_t>(player)].actor;
      const float push_x = player_actor.pad_step_x(), push_z = player_actor.pad_step_z();
      const bool free = !push_target_inside_box(i, push_x, push_z) && push_step_ok(ent_state, push_x, push_z);
      ent_state.pend = free && (std::fabs(push_x) > 1e-4f || std::fabs(push_z) > 1e-4f);
      ent_state.pend_x = push_x;
      ent_state.pend_z = push_z;
      fresh[static_cast<size_t>(i)] = 1;
    } else if (hit) {
      ent_state.pend = false;
    }
  }
  // The no-hit branch: a pushed actor that the player's next position no longer touches moves by its velocity.
  for (size_t i = 0; i < ents_.size(); ++i)
    if (ents_[i].pend && !fresh[i]) apply_push(ents_[i]);
  return blocked;
}

// The movement phase moves every actor but the controlled one by its velocity, except a solid one whose new spot touches the
// controlled entity: that actor stays where it was for the tick, so a walk waits there until the leader steps away. The reach and
// the height overlap are those of blocked_by_solids; party members and pushable actors (moved by the push) are not held.
bool FieldWorld::touches_leader(int i) const {
  const int player = player_entity();
  if (player < 0 || i == player) return false;
  const u32 flags = vm_.entity(i).flags38;
  const Ent& ent_state = ents_[static_cast<size_t>(i)];
  if (!(flags & 0x100u) || (flags & (script::flag38::kInactive | 0x2000u)) || ent_state.pc_slot >= 0) return false;
  const ActorPose& lead = ents_[static_cast<size_t>(player)].actor.current();
  const ActorPose& pose = ent_state.actor.current();
  if (!(lead.y - ent_height(player) <= pose.y && pose.y - ent_height(i) <= lead.y)) return false;
  if (ent_box(i)) return in_box(i, lead.x, lead.z, 0);
  const float reach = ent_radius(i) + ent_radius(player);
  return (lead.x - pose.x) * (lead.x - pose.x) + (lead.z - pose.z) * (lead.z - pose.z) < reach * reach;
}

// The pushed actor's next centre (position + velocity) inside the rect of another box actor refuses the push.
bool FieldWorld::push_target_inside_box(int crate, float vx, float vz) const {
  const ActorPose& pose = ents_[static_cast<size_t>(crate)].actor.current();
  for (int k = 0; k < static_cast<int>(ents_.size()); ++k) {
    if (k == crate) continue;
    const u32 flags = vm_.entity(k).flags38;
    if ((flags & 0x400050u) || !(flags & 0x200u)) continue;
    if (in_box(k, pose.x + vx, pose.z + vz, 0)) return true;
  }
  return false;
}

// A probe: does the walk mesh (class bits, wall slide) accept the step? The actor is left where it was.
bool FieldWorld::push_step_ok(Ent& ent_state, float dx, float dz) {
  const WalkMesh& walk = view_.walk();
  if (walk.empty()) return true;
  const Actor::Spot before = ent_state.actor.spot();
  const bool moved = ent_state.actor.move_on_mesh(walk, dx, dz);
  ent_state.actor.restore(before);
  return moved;
}

void FieldWorld::apply_push(Ent& ent_state) {
  ent_state.actor.move_on_mesh(view_.walk(), ent_state.pend_x, ent_state.pend_z);
  ent_state.pend = false;
  ent_state.pend_x = ent_state.pend_z = 0.0f;
}

void FieldWorld::pick_auto_target() {
  auto_target_ = -1;
  if (!debug_.auto_walk) return;
  const int player = player_entity();
  if (player < 0) return;
  const ActorPose& pp = ents_[static_cast<size_t>(player)].actor.current();
  if (debug_.target >= 0 && debug_.target < static_cast<int>(ents_.size())) {
    auto_target_ = debug_.target;
    return;
  }
  float best = 1e30f;
  for (int i = 1; i < static_cast<int>(ents_.size()); ++i) {
    const Ent& ent_state = ents_[static_cast<size_t>(i)];
    const script::Entity& script_ent = vm_.entity(i);
    if (i == player || ent_state.pc_slot >= 0 || !ent_state.actor.loaded() || ent_hidden(i)) continue;
    if (script_ent.flags38 & (script::flag38::kNoAction | 0x400012)) continue;
    const u16 ptr = vm_.module().script_ptr(static_cast<size_t>(i), 2);
    if (!ptr || vm_.module().code_byte(ptr) == 0) continue;
    const ActorPose& pose = ent_state.actor.current();
    const float distance = std::hypot(pose.x - pp.x, pose.z - pp.z);
    if (distance < best) {
      best = distance;
      auto_target_ = i;
    }
  }
}


void FieldWorld::step_triggers() {
  const int player = player_entity();
  if (player < 0) return;
  const Ent& pe = ents_[static_cast<size_t>(player)];
  const ActorPose& pp = pe.actor.current();
  const float rp = ent_radius(player), hp = ent_height(player);
  // Entities are tested in index order. A pending action press that the script lock allows takes the talk range for itself: an
  // entity there gets the action or nothing, never the touch event (a failed facing cone or an open modal window included). The
  // first entity that answers takes the press, also when its ON_ACTION cannot start (already running, no free record), so the
  // entities after it see no press and run the touch test; a box entity with flag 0x1000 answers without taking the press.
  const bool locked = (vm_.global_flags() & script::gflag::kInputLock) != 0;
  const bool talk_free = controls_free();
  for (int i = 1; i < static_cast<int>(ents_.size()); ++i) {
    if (i == player) continue;
    const Ent& ent_state = ents_[static_cast<size_t>(i)];
    if (ent_state.pc_slot >= 0) continue;
    const script::Entity& script_ent = vm_.entity(i);
    if (script_ent.flags38 & 0x400012) continue;
    const ActorPose& other = ent_state.actor.current();
    if (!(pp.y - hp <= other.y && other.y <= pp.y + ent_height(i))) continue;
    const float dx = other.x - pp.x, dz = other.z - pp.z;
    bool touch, in_talk_range, facing;
    float d2 = dx * dx + dz * dz;
    const bool cone = in_talk_cone((pe.actor.facing() - bearing(dx, dz) + 0x800) & 0xFFF);
    if (ent_box(i)) {
      // The player point is tested against the outer rectangle (half extents + 0x20; the inner one is the solid
      // body). Inside: the action button talks (a facing cone only for entities with flag 0x1000), otherwise the touch event.
      // Thin exit walls (5..10 wide, on the walk-mesh border) and ON_ACTION doors depend on the margin.
      touch = in_talk_range = in_box(i, pp.x, pp.z, kBoxMargin);
      facing = !(script_ent.flags38 & 0x1000) || cone;
    } else {
      const float ro = ent_radius(i);
      touch = d2 < (rp + kTouchMargin + ro) * (rp + kTouchMargin + ro);
      in_talk_range = d2 < (rp + kTalkMargin + ro) * (rp + kTalkMargin + ro);
      facing = cone;
    }
    // A key item used from the field list starts ON slot 4 (class 2) of every entity in talk range that has the touch-trigger
    // flag, without the facing test; that entity gets neither the action nor the touch event this pass.
    if (key_item_shown_ && (script_ent.flags270 & script::flag270::kTouchTrigger)) {
      const float rt = rp + kTalkMargin + ent_radius(i);
      if (ent_box(i) ? touch : d2 < rt * rt) {
        vm_.trigger_touch(i);
        continue;
      }
    }
    if (in_talk_range && input_.action_pending && !locked && !(script_ent.flags38 & script::flag38::kNoAction)) {
      if (talk_free && facing) {
        if (!ent_box(i) || !(script_ent.flags38 & 0x1000)) input_.action_pending = false;
        vm_.trigger_action(i);
      }
      continue;
    }
    if (touch && !(script_ent.flags38 & script::flag38::kNoCollision)) vm_.trigger_collision(i);
  }
  // The key-item flag lasts one pass.
  key_item_shown_ = false;
}

// Off-screen culling (every tick, before the animation steps): the cull flag is cleared for every entity and set again for a
// model entity whose bounding box misses the 320 x 224 screen, unless the entity is exempt: the FE 01 flag or the
// room-entry window (the first two frames after a room load or a battle return) skip the test, and entities with the never-cull flags are always kept. The test projects
// four points of the box (scale applied, camera rotation, no yaw: the entity's own pitch / roll are ignored here):
// P0 / P1 = (-/+ w/2, 0, 0), P2 = (0, 0, -w/2), P3 = (0, -h, +w/2) around (x, y + height offset, z), where w = max(model extent x, z) and
// h = extent y, and keeps the entity when P0.x + scroll < 321, P1.x + scroll >= 0, P2.y + scroll >= 0 and
// P3.y + scroll < 225. A culled entity's sequence, clip time and head matrices do not run, so
// a clip waits until the entity is on screen again, and a WAIT_ANIMATION on it with it. A hidden entity (EF.5) freezes the same way
// until it is shown again. In widescreen the test widens by the extra columns.
void FieldWorld::step_cull() {
  // Debug: A/B check of the off-screen animation freeze.
  static const bool no_cull = std::getenv("OC_NO_CULL") != nullptr;
  const bool grace = entry_window_ > 0 || no_cull;
  if (entry_window_ > 0) --entry_window_;
  const float ex = std::floor(render::wide_extra());
  const float scx = static_cast<float>(view_.scroll_x()), scy = static_cast<float>(view_.scroll_y());
  for (size_t i = 0; i < ents_.size(); ++i) {
    Ent& ent_state = ents_[i];
    script::Entity& script_ent = vm_.entity(static_cast<int>(i));
    bool culled = false;
    if (ent_state.actor.loaded() && !(script_ent.flags38 & 0x4020u) && !grace && !(script_ent.flags270 & 0x200u) && !(script_ent.flags38 & 0x400000u) &&
        !(script_ent.flags270 & 0x1000000u)) {
      const assets::ModelAsset* asset = ent_state.actor.asset();
      const ActorPose& pose = ent_state.actor.current();
      const float w2 = asset ? static_cast<float>(std::max(asset->extent[0], asset->extent[2])) * 0.5f : 0.0f;
      const float h = asset ? static_cast<float>(asset->extent[1]) : 0.0f;
      const float s0 = static_cast<float>(static_cast<s16>(script_ent.scale[0])) / 4096.0f, s1 = static_cast<float>(static_cast<s16>(script_ent.scale[1])) / 4096.0f,
                  s2 = static_cast<float>(static_cast<s16>(script_ent.scale[2])) / 4096.0f;
      const float by = pose.y + static_cast<float>(static_cast<s16>(script_ent.v110));
      render::FieldCamera::Projected p0, p1, p2, p3;
      const render::FieldCamera& cam = view_.camera();
      if (cam.project({pose.x - w2 * s0, by, pose.z}, p0) && cam.project({pose.x + w2 * s0, by, pose.z}, p1) && cam.project({pose.x, by, pose.z - w2 * s2}, p2) &&
          cam.project({pose.x, by - h * s1, pose.z + w2 * s2}, p3)) {
        const bool on = p0.sx + scx < 321.0f + ex && p1.sx + scx >= -ex && p2.sy + scy >= 0.0f && p3.sy + scy < 225.0f;
        culled = !on;
      }
    }
    // A hidden entity (EF.5) is never tested, and its animation is frozen as a culled one's.
    ent_state.actor.set_culled(culled || (script_ent.flags38 & 0x20u));
    script_ent.flags38 = culled ? (script_ent.flags38 | script::flag38::kOffScreen) : (script_ent.flags38 & ~script::flag38::kOffScreen);
  }
}


std::vector<std::array<float, 2>> FieldWorld::find_path(float x0, float z0, float x1, float z1, bool smooth) const {
  std::vector<std::array<float, 2>> out;
  const WalkMesh& walk = view_.walk();
  if (walk.empty()) return out;
  const int player = player_entity();
  const float y_hint = player >= 0 ? ents_[static_cast<size_t>(player)].actor.current().y : 0.0f;
  const int start = walk.find_triangle(x0, z0, y_hint), goal = walk.find_triangle(x1, z1, y_hint);
  if (start < 0 || goal < 0) {
    log::debug("field: find_path: start tri {} goal tri {} (off the walk mesh)", start, goal);
    return out;
  }
  const auto& tris = walk.mesh().triangles;
  const size_t tri_count = tris.size();
  std::vector<float> dist(tri_count, 1e30f);
  std::vector<int> prev(tri_count, -1);
  std::vector<bool> done(tri_count, false);
  auto cen = [&](int tri) { return walk.centroid(tri); };
  // Triangles under a solid entity (boxes and circles, grown by the player's radius) are not walkable, except near the goal.
  std::vector<bool> solid(tri_count, false);
  if (player >= 0) {
    const float rp = ent_radius(player) + 6.0f;
    for (int i = 1; i < static_cast<int>(ents_.size()); ++i) {
      const script::Entity& script_ent = vm_.entity(i);
      const Ent& ent_state = ents_[static_cast<size_t>(i)];
      if (i == player || ent_state.pc_slot >= 0 || !(script_ent.flags38 & 0x100) || (script_ent.flags38 & script::flag38::kInactive) || ent_hidden(i)) continue;
      const ActorPose& other = ent_state.actor.current();
      if (std::hypot(other.x - x1, other.z - z1) < 150.0f) continue;
      for (size_t tri = 0; tri < tri_count; ++tri) {
        const auto centroid = walk.centroid(static_cast<int>(tri));
        if (ent_box(i) ? in_box(i, centroid[0], centroid[1], rp) : std::hypot(centroid[0] - other.x, centroid[1] - other.z) < ent_radius(i) + rp) solid[tri] = true;
      }
    }
  }
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  dist[static_cast<size_t>(start)] = 0;
  open.push({0.0f, start});
  while (!open.empty()) {
    const int tri = open.top().second;
    open.pop();
    if (done[static_cast<size_t>(tri)]) continue;
    done[static_cast<size_t>(tri)] = true;
    if (tri == goal) break;
    const auto ct = cen(tri);
    for (const s16 neighbour : tris[static_cast<size_t>(tri)].adj) {
      if (neighbour < 0 || static_cast<size_t>(neighbour) >= tri_count || done[static_cast<size_t>(neighbour)] || walk.blocked(neighbour, 0) || solid[static_cast<size_t>(neighbour)]) continue;
      const auto ca = cen(neighbour);
      const float nd = dist[static_cast<size_t>(tri)] + std::hypot(ca[0] - ct[0], ca[1] - ct[1]);
      if (nd < dist[static_cast<size_t>(neighbour)]) {
        dist[static_cast<size_t>(neighbour)] = nd;
        prev[static_cast<size_t>(neighbour)] = tri;
        open.push({nd, neighbour});
      }
    }
  }
  if (!done[static_cast<size_t>(goal)]) {
    size_t reached = 0, blocked = 0, nsolid = 0;
    for (size_t tri = 0; tri < tri_count; ++tri) {
      reached += done[tri];
      blocked += walk.blocked(static_cast<int>(tri), 0);
      nsolid += solid[tri];
    }
    log::debug("field: find_path: goal tri {} unreachable from {} ({} of {} triangles reached, {} blocked, {} under solids)", goal, start, reached, tri_count, blocked, nsolid);
    return out;
  }
  std::vector<int> chain;
  for (int tri = goal; tri >= 0; tri = prev[static_cast<size_t>(tri)]) chain.push_back(tri);
  std::reverse(chain.begin(), chain.end());
  std::vector<std::array<float, 2>> pts{{x0, z0}};
  std::vector<int> pt_tri{start};
  for (size_t i = 1; i + 1 < chain.size(); ++i) {
    pts.push_back(cen(chain[i]));
    pt_tri.push_back(chain[i]);
  }
  pts.push_back({x1, z1});
  pt_tri.push_back(goal);
  // Smoothing: from each kept point jump to the farthest later point that a straight walk reaches.
  if (!smooth) {
    out.assign(pts.begin() + 1, pts.end());
    return out;
  }
  size_t i = 0;
  while (i + 1 < pts.size()) {
    size_t best = i + 1;
    for (size_t j = pts.size() - 1; j > i + 1; --j) {
      const float dx = pts[j][0] - pts[i][0], dz = pts[j][1] - pts[i][1];
      const WalkMesh::Move step = walk.move(pt_tri[i], pts[i][0], pts[i][1], dx, dz, 0);
      if (!step.blocked && std::hypot(step.x - pts[j][0], step.z - pts[j][1]) < 2.0f) {
        best = j;
        break;
      }
    }
    out.push_back(pts[best]);
    i = best;
  }
  return out;
}

}  // namespace oc::game
