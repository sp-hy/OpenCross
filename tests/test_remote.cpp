// Remote parties (multiplayer presence) without a GPU: the leader's delayed interpolation (delay, no hold for a late pose, hold after a stop, snap, facing the
// short way round), the party V and the follow rules (stand, walk, run, catch up, back off, the ring), the facing convention, the choice of the
// nearest parties, the arrival settle, and the party list bookkeeping (new and vanished pids, repeated poses, a leader model that fails).
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "game/remote_parties.h"

using namespace oc;
using namespace oc::game;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

bool near(float lhs, float rhs, float eps = 0.5f) { return std::fabs(lhs - rhs) <= eps; }

RemoteSample sample(u64 tick, float x, float z, int facing = 0, bool moving = true) { return {tick, x, 0.0f, z, facing, kClipWalk, moving}; }

void test_motion() {
  RemoteMotion motion;
  CHECK(motion.empty());
  motion.push(sample(100, 0, 0));
  CHECK(motion.take_snap());
  CHECK(!motion.take_snap());
  CHECK(near(motion.at(100).x, 0));
  motion.push(sample(106, 60, 0));
  CHECK(!motion.take_snap());
  // Shown kDelayTicks (9) late: tick 112 shows tick 103, half way.
  CHECK(near(motion.at(109).x, 0));
  CHECK(near(motion.at(112).x, 30));
  CHECK(near(motion.at(115).x, 60));
  CHECK(motion.at(115).moving);
  // A late pose of a walk that never stopped gets no hold: the move blends over the whole gap.
  motion.push(sample(126, 120, 0));
  CHECK(near(motion.at(125).x, 90));
  // A stop, then a pose long after: the stop is held until kDelayTicks before the new pose, so the move plays over the delay.
  motion.push(sample(132, 150, 0, 0, false));
  CHECK(near(motion.at(200).x, 150) && !motion.at(200).moving);
  motion.push(sample(300, 210, 0));
  CHECK(near(motion.at(300).x, 150));
  CHECK(near(motion.at(304).x, 180, 4.0f));
  CHECK(near(motion.at(309).x, 210));
  // Far away: shown at once.
  motion.push(sample(400, 2000, 0));
  CHECK(motion.take_snap());
  CHECK(near(motion.at(400).x, 2000));
  // The facing turns the short way across 0.
  motion.push(sample(406, 2000, 0, 0xFF0));
  motion.push(sample(412, 2000, 0, 0x010));
  CHECK(motion.at(418).facing == 0);
}

void test_facing() {
  // Facing 0 walks toward -x; a quarter turn (0x400) toward +z.
  CHECK(remote_facing(-1, 0) == 0);
  CHECK(remote_facing(0, 1) == 0x400);
  CHECK(remote_facing(1, 0) == 0x800);
}

// The party V, and the follow rules: back off near the leader, stand within 200 of the member followed, else replay its ring at the
// follower speed (walk, run past 230, catch up from 351; slot 2 a step slower).
void test_follow() {
  const int east = remote_facing(1, 0);
  const std::array<float, 2> left = v_spot(1, 0, 0, east);
  const std::array<float, 2> right = v_spot(2, 0, 0, east);
  CHECK(near(left[0], -90.5f) && near(right[0], -90.5f) && near(std::fabs(left[1]), 90.5f) && near(left[1], -right[1]));

  FollowRing ring;
  ring.reset(0, 0, 0);
  for (int x = 5; x <= 300; x += 5) ring.add(static_cast<float>(x), 0, 0);
  ring.add(300.2f, 0, 0);
  CHECK(ring.points().size() == 61);
  const std::array<float, 3> origin{0, 0, 0};
  const auto at = [](float x) { return std::array<float, 3>{x, 0, 0}; };

  FollowerStep step = follow_step(origin, 0, at(150), east, at(150), ring, 0);
  CHECK(!step.moving && step.x == 0.0f);
  step = follow_step(origin, 0, at(220), east, at(220), ring, 0);
  CHECK(step.moving && !step.run && near(step.x, 4.0f) && step.facing == east);
  step = follow_step(origin, 0, at(220), east, at(220), ring, 1);
  CHECK(near(step.x, 3.0f));
  step = follow_step(origin, 0, at(300), east, at(300), ring, 0);
  CHECK(step.run && near(step.x, 8.5f));
  step = follow_step(origin, 0, at(500), east, at(500), ring, 0);
  CHECK(step.run && near(step.x, 15.0f));
  // Near the leader: one step straight away from it, facing it.
  step = follow_step(at(-40), 0, origin, east, origin, ring, 0);
  CHECK(step.moving && near(step.x, -45.0f) && step.facing == east);

  for (int i = 0; i < 200; ++i) ring.add(1000.0f + static_cast<float>(i), 0, 0);
  CHECK(ring.points().size() == FollowRing::kPoints);
}

void test_nearest() {
  const std::vector<float> distances{5, 1, 3, 1};
  CHECK((nearest_indices(distances, 2) == std::vector<size_t>{1, 3}));
  CHECK((nearest_indices(distances, 3) == std::vector<size_t>{1, 2, 3}));
  CHECK(nearest_indices(distances, 0).empty());
  CHECK(nearest_indices(distances, 9).size() == 4);
}

net::RemoteParty party(const char* pid, u32 seq) {
  net::RemoteParty info;
  info.pid = pid;
  info.name = pid;
  info.models = {1, 2, -1};
  info.has_pose = true;
  info.pose_seq = seq;
  info.pose.x = 100;
  return info;
}

void test_parties() {
  RemoteParties parties;
  const std::vector<net::RemoteParty> two{party("a", 1), party("b", 1)};
  parties.set(two, 10);
  CHECK(parties.parties().size() == 2);
  CHECK(parties.wanted_models().size() == 4);
  // A vanished pid is dropped; the same pose_seq adds no sample (no snap pending a second time).
  const std::vector<net::RemoteParty> one{party("b", 1)};
  parties.set(one, 11);
  CHECK(parties.parties().size() == 1 && parties.parties()[0].info.pid == "b");
  CHECK(parties.parties()[0].motion.at(11).x == 100.0f);
  // A leader model that fails hides the party; nothing is asked of the walk mesh or the lighting.
  int asked = 0;
  const RemoteParties::ModelLoader failing = [&](int, bool& failed) {
    ++asked;
    failed = true;
    return std::shared_ptr<ModelHandle>();
  };
  WalkMesh walk;
  RoomLighting lighting;
  parties.tick(12, walk, lighting, failing, 0, 0, {});
  CHECK(asked == 2);
  CHECK(!parties.parties()[0].visible);
  parties.tick(13, walk, lighting, failing, 0, 0, {});
  CHECK(asked == 2);
  CHECK(parties.wanted_models().empty());
  // A new model byte asks again.
  std::vector<net::RemoteParty> changed{party("b", 2)};
  changed[0].models = {3, -1, -1};
  parties.set(changed, 14);
  CHECK(parties.wanted_models() == std::vector<int>{3});
  parties.clear();
  CHECK(parties.empty());
}

// A party that arrives is drawn once its spot has held kSettleTicks with its player free, or after kEventWaitTicks in an event.
void test_settle() {
  RemoteParties::Party arrived;
  arrived.free_since = true;
  arrived.settled = RemoteParties::kSettleTicks - 1;
  CHECK(!RemoteParties::settled_in(arrived));
  arrived.settled = RemoteParties::kSettleTicks;
  CHECK(RemoteParties::settled_in(arrived));
  arrived.free_since = false;
  CHECK(!RemoteParties::settled_in(arrived));
  arrived.settled = RemoteParties::kEventWaitTicks;
  CHECK(RemoteParties::settled_in(arrived));
}

}  // namespace

int main() {
  test_motion();
  test_facing();
  test_follow();
  test_nearest();
  test_parties();
  test_settle();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("remote: all checks passed\n");
  return 0;
}
