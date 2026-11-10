#pragma once
// Remote parties: other players' parties shown in the local room (multiplayer presence). Purely cosmetic: they are not entities of the
// room, run no script, collide with nothing and touch no game state. A party is its leader, whose received poses are shown a few ticks
// late and interpolated (RemoteMotion), and up to two followers moved by the field's follow rules (follow_step) over the paths the
// members ahead of them walked (FollowRing), starting from the party V. The models come from the field through a loader; a party is
// drawn once its leader's model is ready.
#include <array>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "game/actor.h"
#include "game/room_lighting.h"
#include "game/walk_mesh.h"
#include "net/presence.h"

namespace oc::game {

// One received leader pose and the local field tick it arrived on.
struct RemoteSample {
  u64 tick = 0;
  float x = 0, y = 0, z = 0;
  // Facing, 4096 per turn.
  int facing = 0;
  int clip = 0;
  bool moving = false;
};

// A remote leader between its received poses, shown kDelayTicks late so that a pose usually has a successor to blend toward. A pose
// farther than kSnapUnits from the one before (a room entry, a teleport) is shown at once.
class RemoteMotion {
 public:
  static constexpr u64 kDelayTicks = 9;
  static constexpr float kSnapUnits = 400.0f;
  static constexpr size_t kMaxSamples = 32;
  void push(const RemoteSample& sample);
  // The pose on local tick `now`: position and facing (the short way round) interpolated between the samples around now - kDelayTicks;
  // before the first sample the first one, after the last one the last one.
  RemoteSample at(u64 now) const;
  bool empty() const { return samples_.empty(); }
  // True once after a sample was shown at once (the first one, or a snap).
  bool take_snap() {
    const bool snap = snapped_;
    snapped_ = false;
    return snap;
  }

 private:
  std::vector<RemoteSample> samples_;
  bool snapped_ = false;
};

// The path a member walked, newest point last, at most kPoints of it: what the member behind it replays (ring 0 the leader's, ring 1
// follower 1's).
class FollowRing {
 public:
  static constexpr size_t kPoints = 0x80;
  void reset(float x, float y, float z);
  // A point is added when (x, y, z) is more than half a unit from the newest one.
  void add(float x, float y, float z);
  const std::vector<std::array<float, 3>>& points() const { return points_; }

 private:
  std::vector<std::array<float, 3>> points_;
};

// One tick of the field's follow rules for a remote follower at `self`: back off when nearer than 80 units to the leader, stand while
// within 200 of the member it follows, else replay `ring` (the followed member's path) at the follower speed of `ring_index` (0 for
// slot 1, 1 for slot 2), running past 230 units and catching up at three times the base speed from 351.
struct FollowerStep {
  float x = 0, y = 0, z = 0;
  int facing = 0;
  bool moving = false;
  bool run = false;
};
FollowerStep follow_step(const std::array<float, 3>& self, int facing, const std::array<float, 3>& leader, int leader_facing,
                         const std::array<float, 3>& followed, const FollowRing& ring, int ring_index);
// The spot of party slot 1 or 2 in the party V behind a leader at (x, z) with facing `facing`: 128 units away, an eighth of a turn to
// either side of straight behind.
std::array<float, 2> v_spot(int slot, float x, float z, int facing);

// Facing (4096 per turn) of a move by (dx, dz), the field's own convention.
int remote_facing(float dx, float dz);
// The indices of the `count` smallest `distances` (ties to the lower index), ascending.
std::vector<size_t> nearest_indices(std::span<const float> distances, int count);

class RemoteParties {
 public:
  // The field model of a model byte: the handle once it is ready, null while it loads; `failed` when it never will be.
  using ModelLoader = std::function<std::shared_ptr<ModelHandle>(int model_byte, bool& failed)>;
  // A party that arrives or jumps is drawn once its spot has held kSettleTicks without another jump and its player has had the controls
  // since, or after kEventWaitTicks in an event: a room's entrance plays out unseen and the party appears where it ends up.
  static constexpr u32 kSettleTicks = 30;
  static constexpr u32 kEventWaitTicks = 300;
  struct Member {
    Actor actor;
    int model = -1;
    bool failed = false;
    bool placed = false;
    ActorLight light;
    bool light_init = false;
    int shadow_grey = 0x40;
  };
  struct Party {
    net::RemoteParty info;
    RemoteMotion motion;
    std::array<FollowRing, 2> rings;
    std::array<Member, 3> members;
    // The last pose_seq taken into the motion.
    u32 seen_seq = 0;
    bool seen = false;
    // Among the parties drawn this tick (shown, settled, leader model ready, one of the nearest).
    bool visible = false;
    // The leader moved on the last tick.
    bool moving = false;
    // Ticks since the party was last placed afresh (its arrival, a snap), up to kEventWaitTicks, and whether its player has had the
    // controls (state Field or Menu) since then.
    u32 settled = 0;
    bool free_since = false;
  };
  static bool settled_in(const Party& party) {
    return party.settled >= kSettleTicks && (party.free_since || party.settled >= kEventWaitTicks);
  }

  // The full list of remote parties in the room on local tick `now`: new pids are added, vanished ones dropped, a new pose_seq is a
  // new sample.
  void set(std::span<const net::RemoteParty> parties, u64 now);
  // One field tick: motion, followers, animation and light; `target` (x, z) is the camera's target, which picks the nearest parties.
  void tick(u64 now, const WalkMesh& walk, const RoomLighting& lighting, const ModelLoader& loader, float target_x, float target_z,
            const net::RemoteDrawOptions& options);
  // The room changed or the art was rebuilt: every member is placed and its model asked for again.
  void reset_room();
  void clear() { parties_.clear(); }
  bool empty() const { return parties_.empty(); }
  std::vector<Party>& parties() { return parties_; }
  const std::vector<Party>& parties() const { return parties_; }
  // The model bytes some member shows or waits for.
  std::vector<int> wanted_models() const;

 private:
  void step_party(Party& party, u64 now, const WalkMesh& walk, const RoomLighting& lighting, const ModelLoader& loader);
  std::vector<Party> parties_;
};

}  // namespace oc::game
