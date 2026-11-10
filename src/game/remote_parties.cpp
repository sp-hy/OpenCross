// Remote parties: the leader's delayed interpolation, the party V and the follow rules of the followers, the choice of the nearest parties and the per-tick pose,
// animation and light of every member.
#include "game/remote_parties.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oc::game {

namespace {

constexpr float kUnitsPerRad = 4096.0f / (2.0f * std::numbers::pi_v<float>);
// A member counts as moving when it moved more than this on the tick.
constexpr float kMoveEpsilon = 0.05f;
// A follower takes the floor height under its spot when the floor is within this of the path's height; a V spot whose floor is farther
// from the leader's height than this is not used.
constexpr float kFloorReach = 64.0f;
// The follow rules' distances and the members' base speed (units per tick = speed / 16).
constexpr float kBackOffDistance = 80.0f;
constexpr float kStandDistance = 200.0f;
constexpr float kRunDistance = 230.0f;
constexpr float kCatchUpDistance = 351.0f;
constexpr float kRingNear = 50.0f;
constexpr int kBaseSpeed = 0x50;
constexpr float kVDistance = 128.0f;

float xz_distance(float x0, float z0, float x1, float z1) { return std::hypot(x1 - x0, z1 - z0); }

}  // namespace

int remote_facing(float dx, float dz) { return static_cast<int>(std::lround(std::atan2(dz, -dx) * kUnitsPerRad)) & 0xFFF; }

std::vector<size_t> nearest_indices(std::span<const float> distances, int count) {
  std::vector<size_t> order(distances.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) { return distances[lhs] < distances[rhs]; });
  order.resize(std::min(order.size(), static_cast<size_t>(std::max(0, count))));
  std::sort(order.begin(), order.end());
  return order;
}

// A sample after a stop (the last one did not move, so nothing was sent meanwhile) gets the last pose repeated kDelayTicks before it: the
// move then plays over the delay instead of jumping most of the way at once. A late sample of a walk that never stopped gets no hold.
void RemoteMotion::push(const RemoteSample& sample) {
  if (!samples_.empty()) {
    const RemoteSample& last = samples_.back();
    if (std::hypot(sample.x - last.x, sample.y - last.y, sample.z - last.z) > kSnapUnits) samples_.clear();
  }
  if (samples_.empty()) {
    snapped_ = true;
  } else if (samples_.back().tick == sample.tick) {
    samples_.pop_back();
  } else if (!samples_.back().moving && sample.tick > samples_.back().tick + kDelayTicks) {
    RemoteSample hold = samples_.back();
    hold.tick = sample.tick - kDelayTicks;
    hold.moving = false;
    samples_.push_back(hold);
  }
  samples_.push_back(sample);
  if (samples_.size() > kMaxSamples) samples_.erase(samples_.begin(), samples_.end() - static_cast<std::ptrdiff_t>(kMaxSamples));
}

// The clip and the moving flag of a segment are its end's when that one moves, else its start's (the last steps before a stop walk).
RemoteSample RemoteMotion::at(u64 now) const {
  if (samples_.empty()) return {};
  const u64 shown = now > kDelayTicks ? now - kDelayTicks : 0;
  if (shown <= samples_.front().tick) return samples_.front();
  for (size_t i = 1; i < samples_.size(); ++i) {
    const RemoteSample& to = samples_[i];
    if (shown >= to.tick) continue;
    const RemoteSample& from = samples_[i - 1];
    const float share = static_cast<float>(shown - from.tick) / static_cast<float>(to.tick - from.tick);
    RemoteSample out = to.moving ? to : from;
    out.tick = shown;
    out.x = from.x + (to.x - from.x) * share;
    out.y = from.y + (to.y - from.y) * share;
    out.z = from.z + (to.z - from.z) * share;
    const int turn = ((to.facing - from.facing + 0x800) & 0xFFF) - 0x800;
    out.facing = (from.facing + static_cast<int>(std::lround(static_cast<float>(turn) * share))) & 0xFFF;
    return out;
  }
  return samples_.back();
}

void FollowRing::reset(float x, float y, float z) { points_.assign(1, {x, y, z}); }

void FollowRing::add(float x, float y, float z) {
  if (!points_.empty() && std::hypot(x - points_.back()[0], y - points_.back()[1], z - points_.back()[2]) <= 0.5f) return;
  points_.push_back({x, y, z});
  if (points_.size() > kPoints) points_.erase(points_.begin());
}

// Rule 1 steps straight away from the leader (behind its facing when the two coincide) and faces it; rule 3 steps toward the newest
// point of the ring nearer than kRingNear to the member (none: the newest point), the newest point instead when that one is already
// within the step, the full step even past it.
FollowerStep follow_step(const std::array<float, 3>& self, int facing, const std::array<float, 3>& leader, int leader_facing,
                         const std::array<float, 3>& followed, const FollowRing& ring, int ring_index) {
  FollowerStep out{self[0], self[1], self[2], facing, false, false};
  if (xz_distance(self[0], self[2], leader[0], leader[2]) < kBackOffDistance) {
    float dx = self[0] - leader[0];
    float dz = self[2] - leader[2];
    float length = std::hypot(dx, dz);
    if (length < 1e-3f) {
      const float angle = static_cast<float>(leader_facing & 0xFFF) / kUnitsPerRad;
      dx = std::cos(angle);
      dz = -std::sin(angle);
      length = 1.0f;
    }
    const float step = static_cast<float>(kBaseSpeed) / 16.0f;
    out.x += dx / length * step;
    out.z += dz / length * step;
    out.facing = remote_facing(leader[0] - out.x, leader[2] - out.z);
    out.moving = true;
    return out;
  }
  const float to_followed = xz_distance(self[0], self[2], followed[0], followed[2]);
  const std::vector<std::array<float, 3>>& points = ring.points();
  if (to_followed <= kStandDistance || points.empty()) return out;
  int speed = kBaseSpeed - (ring_index + 1) * 16;
  if (to_followed > kRunDistance) {
    speed += kBaseSpeed / 2 + 32;
    out.run = true;
  }
  if (to_followed >= kCatchUpDistance) speed = 3 * kBaseSpeed;
  const float step = static_cast<float>(speed) / 16.0f;
  size_t pick = points.size() - 1;
  for (size_t i = points.size(); i-- > 0;)
    if (xz_distance(self[0], self[2], points[i][0], points[i][2]) < kRingNear) {
      pick = i;
      break;
    }
  if (pick != points.size() - 1 && xz_distance(self[0], self[2], points[pick][0], points[pick][2]) <= step) pick = points.size() - 1;
  const std::array<float, 3>& target = points[pick];
  const float dx = target[0] - self[0];
  const float dz = target[2] - self[2];
  const float length = std::hypot(dx, dz);
  if (length < 1e-3f) return out;
  out.x += dx / length * step;
  out.z += dz / length * step;
  out.y = target[1];
  out.facing = remote_facing(dx, dz);
  out.moving = true;
  return out;
}

std::array<float, 2> v_spot(int slot, float x, float z, int facing) {
  const int heading = (facing - 0x800 + (slot == 1 ? 0x200 : -0x200)) & 0xFFF;
  const float angle = static_cast<float>(heading) / kUnitsPerRad;
  return {x - std::cos(angle) * kVDistance, z + std::sin(angle) * kVDistance};
}

void RemoteParties::set(std::span<const net::RemoteParty> parties, u64 now) {
  std::vector<Party> next;
  next.reserve(parties.size());
  for (const net::RemoteParty& info : parties) {
    if (std::any_of(next.begin(), next.end(), [&](const Party& party) { return party.info.pid == info.pid; })) continue;
    const auto old = std::find_if(parties_.begin(), parties_.end(), [&](const Party& party) { return party.info.pid == info.pid; });
    Party party = old != parties_.end() ? std::move(*old) : Party{};
    party.info = info;
    for (size_t slot = 0; slot < party.members.size(); ++slot) {
      Member& member = party.members[slot];
      if (member.model == info.models[slot]) continue;
      member = Member{};
      member.model = info.models[slot];
    }
    if (info.has_pose && (!party.seen || info.pose_seq != party.seen_seq)) {
      const net::LeaderPose& pose = info.pose;
      party.motion.push({now, static_cast<float>(pose.x), static_cast<float>(pose.y), static_cast<float>(pose.z), pose.facing & 0xFFF, pose.clip, pose.moving});
      party.seen = true;
      party.seen_seq = info.pose_seq;
    }
    next.push_back(std::move(party));
  }
  parties_ = std::move(next);
}

void RemoteParties::reset_room() {
  for (Party& party : parties_)
    for (Member& member : party.members) {
      Member fresh;
      fresh.model = member.model;
      member = std::move(fresh);
    }
}

std::vector<int> RemoteParties::wanted_models() const {
  std::vector<int> bytes;
  for (const Party& party : parties_)
    for (const Member& member : party.members)
      if (member.model >= 0 && !member.failed) bytes.push_back(member.model);
  return bytes;
}

void RemoteParties::tick(u64 now, const WalkMesh& walk, const RoomLighting& lighting, const ModelLoader& loader, float target_x, float target_z,
                         const net::RemoteDrawOptions& options) {
  std::vector<size_t> candidates;
  std::vector<float> distances;
  for (size_t i = 0; i < parties_.size(); ++i) {
    Party& party = parties_[i];
    step_party(party, now, walk, lighting, loader);
    party.visible = false;
    const Member& lead = party.members[0];
    if (!options.show || !party.seen || !lead.placed || !settled_in(party)) continue;
    candidates.push_back(i);
    distances.push_back(xz_distance(lead.actor.current().x, lead.actor.current().z, target_x, target_z));
  }
  for (const size_t k : nearest_indices(distances, options.max_parties)) parties_[candidates[k]].visible = true;
}

// The leader takes the interpolated pose; it walks or runs, by its clip, while it moves or its pose says it moves (a late pose leaves it
// waiting a few ticks: the walk cycle goes on rather than restart), and stands otherwise and in a battle, a menu, an event or while its player types. A follower is placed
// in the party V (the half V, else the leader's spot, when the V spot has no floor near the leader's height) on its first tick with a
// model and after a snap, then moves by follow_step: slot 1 after the leader over ring 0, slot 2 after slot 1 over ring 1. Placing shows
// the member at once, facing the leader's way. Every member's light eases toward the zone colour of its triangle and its shadow strength
// toward the party actors' 64 (0 on water and stairs), as a party actor's do.
void RemoteParties::step_party(Party& party, u64 now, const WalkMesh& walk, const RoomLighting& lighting, const ModelLoader& loader) {
  if (!party.seen) return;
  for (Member& member : party.members) {
    if (member.model < 0 || member.failed || member.actor.model_handle()) continue;
    bool failed = false;
    if (auto handle = loader(member.model, failed)) member.actor.set_model(std::move(handle));
    member.failed = failed;
  }
  Member& lead = party.members[0];
  const bool snap = party.motion.take_snap();
  if (lead.failed || !lead.actor.loaded()) {
    for (Member& member : party.members) member.placed = false;
    party.settled = 0;
    return;
  }
  const RemoteSample shown = party.motion.at(now);
  const net::PresenceState state = party.info.pose.state;
  const bool still = state != net::PresenceState::Field;
  const bool lead_run = shown.clip == kClipRun;
  const bool regroup = snap || !lead.placed;
  party.settled = regroup ? 0 : std::min(party.settled + 1, kEventWaitTicks);
  if (regroup) party.free_since = false;
  if (state == net::PresenceState::Field || state == net::PresenceState::Menu || state == net::PresenceState::Typing) party.free_since = true;
  if (regroup) party.rings[0].reset(shown.x, shown.y, shown.z);
  else party.rings[0].add(shown.x, shown.y, shown.z);
  const auto triangle = [&](float x, float y, float z) { return walk.empty() ? -1 : walk.find_triangle(x, z, y); };
  const auto update = [&](Member& member, float x, float y, float z, int tri, int facing, bool place, bool moving, bool run) {
    Actor& actor = member.actor;
    if (place) {
      actor.place(walk, tri, x, z, facing);
      actor.set_height(y, true);
      actor.snap_facing();
      member.placed = true;
    }
    actor.begin_tick();
    if (!place) actor.move_free(x, y, z, tri);
    actor.set_facing(facing);
    if (!moving && !actor.auto_idle) actor.seed_rng(static_cast<u32>(now));
    actor.auto_idle = !moving;
    actor.step_visual(moving, run);
    actor.take_foot_events();
    actor.take_bone_emitters();
    actor.end_tick();
    const u32 flags = actor.triangle() >= 0 ? walk.flags(actor.triangle()) : 0u;
    if (!member.light_init) lighting.snap(member.light, flags);
    else lighting.tick(member.light, flags);
    member.light_init = true;
    const int target = (flags & 0x6E000) ? 0 : 0x40;
    member.shadow_grey = member.shadow_grey < target ? std::min(target, member.shadow_grey + 2) : std::max(target, member.shadow_grey - 2);
  };
  const bool lead_place = regroup || !lead.placed;
  const float lead_shift = xz_distance(lead.actor.current().x, lead.actor.current().z, shown.x, shown.z);
  party.moving = !lead_place && !still && (shown.moving || lead_shift > kMoveEpsilon);
  update(lead, shown.x, shown.y, shown.z, triangle(shown.x, shown.y, shown.z), shown.facing, lead_place, party.moving, lead_run);
  const std::array<float, 3> leader{shown.x, shown.y, shown.z};
  for (size_t slot = 1; slot < party.members.size(); ++slot) {
    Member& member = party.members[slot];
    if (member.failed || !member.actor.loaded()) continue;
    const Member& ahead = party.members[slot - 1];
    const bool after_leader = slot == 1 || ahead.failed || !ahead.placed;
    if (regroup || !member.placed) {
      std::array<float, 3> spot = leader;
      int tri = triangle(shown.x, shown.y, shown.z);
      const std::array<float, 2> full = v_spot(static_cast<int>(slot), shown.x, shown.z, shown.facing);
      for (const float share : {1.0f, 0.5f}) {
        const float x = shown.x + (full[0] - shown.x) * share;
        const float z = shown.z + (full[1] - shown.z) * share;
        const int under = triangle(x, shown.y, z);
        if (walk.empty() || (under >= 0 && std::fabs(walk.height(under, x, z) - shown.y) <= kFloorReach)) {
          spot = {x, under >= 0 ? walk.height(under, x, z) : shown.y, z};
          tri = under;
          break;
        }
      }
      update(member, spot[0], spot[1], spot[2], tri, shown.facing, true, false, false);
      if (slot == 1) party.rings[1].reset(spot[0], spot[1], spot[2]);
      continue;
    }
    const ActorPose& now_at = member.actor.current();
    const ActorPose& ahead_at = ahead.actor.current();
    const std::array<float, 3> followed = after_leader ? leader : std::array<float, 3>{ahead_at.x, ahead_at.y, ahead_at.z};
    const FollowRing& ring = after_leader ? party.rings[0] : party.rings[1];
    const FollowerStep step =
        follow_step({now_at.x, now_at.y, now_at.z}, member.actor.facing(), leader, shown.facing, followed, ring, after_leader ? 0 : 1);
    const int tri = triangle(step.x, step.y, step.z);
    float y = step.y;
    if (tri >= 0 && std::fabs(walk.height(tri, step.x, step.z) - step.y) <= kFloorReach) y = walk.height(tri, step.x, step.z);
    update(member, step.x, y, step.z, tri, step.facing, false, step.moving, step.run);
    if (slot == 1 && step.moving) party.rings[1].add(step.x, y, step.z);
  }
}

}  // namespace oc::game
