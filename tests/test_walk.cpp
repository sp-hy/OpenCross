// Walk-mesh helper checks on a hand-built 3-triangle strip: containment, plane height, edge crossing, wall blocking
// by attr flags, boundary stop and wall sliding.
#include <cmath>
#include <cstdio>

#include <memory>
#include <string>

#include "game/actor.h"
#include "game/walk_mesh.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

bool near(float lhs, float rhs, float eps = 1e-2f) { return std::fabs(lhs - rhs) <= eps; }

// Squares [0,100]x[0,100] and [100,200]x[0,100], each split in two triangles; the vertices at x = 200 are higher
// (y = 20) so the right square slopes. Triangle 3 (right-upper) uses attr 1 = wall.
assets::WalkMesh build() {
  assets::WalkMesh mesh;
  mesh.vertices = {{0, 0, 0}, {100, 0, 0}, {100, 0, 100}, {0, 0, 100}, {200, 20, 0}, {200, 20, 100}};
  auto tri = [](u16 v0, u16 v1, u16 v2, s16 a0, s16 a1, s16 a2, s16 attr) {
    assets::WalkTriangle triangle;
    triangle.v = {v0, v1, v2};
    triangle.adj = {a0, a1, a2};
    triangle.attr = attr;
    return triangle;
  };
  // Left square: 0:(v0,v1,v2) 1:(v0,v2,v3). Right square: 2:(v1,v4,v5) 3:(v1,v5,v2).
  // Tri 0's edge v1-v2 borders the right square (tri 3); tri 1's edge v0-v2 is the shared diagonal.
  mesh.triangles.push_back(tri(0, 1, 2, -1, 3, 1, 0));
  mesh.triangles.push_back(tri(0, 2, 3, 0, -1, -1, 0));
  mesh.triangles.push_back(tri(1, 4, 5, -1, -1, 3, 0));
  mesh.triangles.push_back(tri(1, 5, 2, 2, -1, 0, 1));
  return mesh;
}

// Actor script-animation rules on a model-less handle (clips only, no GPU): slots 1-4 loop, other clips and the
// extra clips (0x80+) play once and hold; moving overrides a requested clip; turning chases the facing.
void test_actor_anim() {
  auto handle = std::make_shared<game::ModelHandle>();
  assets::Joint root;
  handle->asset.skeleton.joints.push_back(root);
  auto clip = [&](const char* source, u32 index, u32 frames) {
    assets::AnimationClip anim_clip;
    anim_clip.source = source;
    anim_clip.index = index;
    anim_clip.frame_count = frames;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s_%02u", source, index);
    anim_clip.name = buf;
    anim_clip.tracks.resize(1);
    handle->asset.clips.push_back(anim_clip);
  };
  for (u32 i = 0; i < 6; ++i) clip("s4", i, 8);
  clip("ex", 0, 6);
  game::Actor actor;
  actor.set_model(handle);
  // The stand slot is the default.
  CHECK(actor.clip_name() == "s4_01");

  // One-shot: 7 keyframes at 0.25 per tick = 28 ticks; it then holds its last frame.
  actor.play(5);
  CHECK(actor.anim_busy());
  for (int i = 0; i < 20; ++i) actor.step_visual(false, false);
  CHECK(actor.anim_busy());
  for (int i = 0; i < 12; ++i) actor.step_visual(false, false);
  CHECK(!actor.anim_busy() && actor.clip_name() == "s4_05");

  // A looping slot is never busy.
  actor.play(1);
  for (int i = 0; i < 100; ++i) actor.step_visual(false, false);
  CHECK(!actor.anim_busy() && actor.clip_name() == "s4_01");

  // Extra clip 0: 5 keyframes -> 20 ticks, one-shot; the clip starts at the next step.
  actor.play(0x80);
  actor.step_visual(false, false);
  CHECK(actor.anim_busy() && actor.clip_name() == "ex_00");
  for (int i = 0; i < 25; ++i) actor.step_visual(false, false);
  CHECK(!actor.anim_busy());

  actor.play(5);
  // Walking overrides the request.
  actor.step_visual(true, false);
  CHECK(!actor.anim_busy() && actor.requested_anim() == -1 && actor.clip_name() == "s4_03");
  actor.step_visual(true, true);
  // The run slot.
  CHECK(actor.clip_name() == "s4_02");
  actor.set_anim_slot(game::kSlotStand, 4);
  actor.step_visual(false, false);
  // The switch to the stand clip waits for the running 0x20-tick cross-fade.
  CHECK(actor.clip_name() == "s4_02");
  for (int i = 0; i < 0x20; ++i) actor.step_visual(false, false);
  CHECK(actor.clip_name() == "s4_04");
  // Other clips switch at once.
  actor.step_visual(true, false);
  CHECK(actor.clip_name() == "s4_03");

  // Facing: the model yaw chases the target facing at the turn rate (default 0x80) per tick; a rate of 0 never turns.
  actor.set_facing(0x400);
  CHECK(actor.turning());
  for (int i = 0; i < 7; ++i) actor.step_visual(false, false);
  CHECK(actor.turning());
  // 0x400 takes 8 ticks.
  actor.step_visual(false, false);
  CHECK(!actor.turning());
  actor.turn_rate = 0;
  actor.set_facing(0x800);
  actor.step_visual(false, false);
  // WAIT_ROTATION is a pure angle test: still turning (FieldWorld decides).
  CHECK(actor.turning() && actor.shown_facing() == 0x400);

  // Talk cone: facing straight at the target (difference 0x800 in the engine's formula) is inside, away is not.
  CHECK(game::in_talk_cone(0x800) && game::in_talk_cone(0x600) && !game::in_talk_cone(0) && !game::in_talk_cone(0x400));
}

// Animation-sequence interpreter on Serge-style entries: the sequence starts the clip, sets the loop flag, the foot
// events (var 1) and the anim-done variable (var 0).
void test_anim_seq() {
  auto handle = std::make_shared<game::ModelHandle>();
  handle->asset.skeleton.joints.push_back(assets::Joint{});
  for (u32 i = 0; i < 8; ++i) {
    assets::AnimationClip anim_clip;
    anim_clip.source = "s4";
    anim_clip.index = i;
    anim_clip.frame_count = 8;
    anim_clip.name = "s4_0" + std::to_string(i);
    anim_clip.tracks.resize(1);
    handle->asset.clips.push_back(anim_clip);
  }
  auto entry = [](std::vector<s16> script) {
    std::vector<s16> words = {0x10, 0, 0, 0, 0, 0, 0, 0};
    words.insert(words.end(), script.begin(), script.end());
    return words;
  };
  handle->asset.anim_seq.resize(8);
  // stand: loop, clip 1, done, hold.
  handle->asset.anim_seq[1] = entry({0x17, 0x15, 1, 8, 0x40, 0, 1, 9, 0});
  // run: loop, clip 2; { wait 4; var2 = 0x11; var1 |= var2; wait 4; var2 = 0x22; var1 |= var2; done; jump back to the first wait }
  handle->asset.anim_seq[2] = entry({0x17, 0x15, 2, 8, 9, 4, 0x40, 2, 0x11, 0x77, 1, 2, 9, 4, 0x40, 2, 0x22, 0x77, 1, 2, 0x40, 0, 1, 1, -20, 9, 0});
  // turn: one-shot clip 6, wait for its end, 8 more ticks, back to clip 1 (stand), done
  handle->asset.anim_seq[6] = entry({0x18, 0x15, 6, 8, 0x0A, 9, 8, 0x17, 0x15, 1, 0x10, 0x40, 0, 1, 9, 0});
  game::Actor actor;
  actor.set_model(handle);
  CHECK(actor.has_anim_seq() && actor.clip_name() == "s4_01" && actor.submitted_anim() == 1);

  actor.play(2);
  CHECK(actor.anim_busy());
  int events[16] = {};
  for (int tick = 0; tick < 16; ++tick) {
    actor.step_visual(false, false);
    events[tick] = actor.take_foot_events();
  }
  CHECK(actor.clip_name() == "s4_02");
  // Foot events fire on ticks 5 / 9 / 13; var 0 is set after the first cycle.
  CHECK(events[3] == 0 && events[4] == 0x11 && events[8] == 0x22 && events[12] == 0x11);
  CHECK(!actor.anim_busy());

  actor.play(6);
  actor.step_visual(false, false);
  CHECK(actor.anim_busy() && actor.clip_name() == "s4_06");
  // 28 ticks to the last keyframe + 8.
  for (int tick = 0; tick < 34; ++tick) actor.step_visual(false, false);
  CHECK(actor.anim_busy() && actor.clip_name() == "s4_06");
  for (int tick = 0; tick < 3; ++tick) actor.step_visual(false, false);
  CHECK(!actor.anim_busy() && actor.clip_name() == "s4_01");

  // Playing the id in force does not restart; the anim-done bit stays clear as in the engine.
  actor.play(6);
  actor.step_visual(false, false);
  CHECK(actor.clip_name() == "s4_01" && actor.anim_busy());
  // The scripted-turn clip restarts and sets anim-done.
  actor.play_engine_clip(6, true);
  actor.step_visual(false, false);
  CHECK(actor.clip_name() == "s4_06" && !actor.anim_busy());
}
// D6 attachment: the attached model is drawn from the parent's joint at the drawn render fraction, so it sits on the parent's attach
// point in every frame, also while the parent moves and its display is smoothed over ticks (a move every 2nd tick).
void test_attached_rigid() {
  auto handle = std::make_shared<game::ModelHandle>();
  handle->asset.skeleton.joints.push_back(assets::Joint{});
  handle->asset.attach.points[2].pos = {10.0f, -50.0f, 120.0f};
  handle->asset.attach.points[2].used = true;
  game::Actor parent;
  parent.set_model(handle);
  game::Actor rider;
  render::Mat4 local;
  local.at(0, 3) = 10.0f;
  local.at(1, 3) = -50.0f;
  local.at(2, 3) = 120.0f;
  rider.set_attached(&parent, 2, local);
  const game::WalkMesh empty;
  float z = 0.0f;
  for (int tick = 0; tick < 12; ++tick) {
    parent.begin_tick();
    if (tick % 2 == 0) z += 16.0f;
    parent.set_position(empty, 0.0f, z, false, true);
    parent.end_tick();
    for (const float alpha : {0.0f, 0.5f, 1.0f}) {
      const render::Mat4 joint = parent.model_matrix(alpha);
      const render::Mat4 drawn = rider.model_matrix(alpha);
      for (int row = 0; row < 3; ++row) {
        const float expected = joint.at(row, 0) * 10.0f + joint.at(row, 1) * -50.0f + joint.at(row, 2) * 120.0f + joint.at(row, 3);
        CHECK(near(drawn.at(row, 3), expected));
      }
    }
  }
  rider.clear_attached();
  CHECK(!rider.attached());
}
}  // namespace

int main() {
  test_actor_anim();
  test_attached_rigid();
  test_anim_seq();
  game::WalkMesh wm;
  wm.init(build(), {0, game::kFlagBlockAll});

  CHECK(wm.contains(0, 80, 20));
  CHECK(!wm.contains(0, 20, 80));
  CHECK(wm.contains(1, 20, 80));
  CHECK(wm.find_triangle(150, 10, 0) == 2);
  CHECK(wm.find_triangle(500, 10, 0) == -1);
  // Halfway up the slope.
  CHECK(near(wm.height(2, 150, 10), 10.0f));
  CHECK(near(wm.height(0, 60, 20), 0.0f));
  CHECK(wm.blocked(3, 0) && !wm.blocked(2, 0));

  // Crossing the diagonal from triangle 0 into triangle 1.
  auto moved = wm.move(0, 80, 20, -50, 60, 0);
  CHECK(!moved.blocked && moved.tri == 1 && near(moved.x, 30) && near(moved.z, 80));

  // Moving right from triangle 2 stays inside the right square's open boundary at x = 200: stopped, not crossing.
  moved = wm.move(2, 190, 5, 50, 0, 0);
  CHECK(moved.blocked && moved.x <= 200.0f && moved.tri == 2);

  // From triangle 0 into the wall triangle 3 across edge v1-v2: blocked, slides along the edge (z grows).
  moved = wm.move(0, 90, 10, 30, 40, 0);
  CHECK(moved.blocked && moved.tri != 3 && moved.x <= 100.0f + 1e-2f && moved.z > 10.0f);

  // Nearest vertex ignores blocked triangles: vertex 5 only borders triangles 2 and 3, 2 is fine.
  int tri = -1;
  CHECK(wm.nearest_vertex(10, 10, 0, &tri) == 0 && tri >= 0);

  // Class bits: only 0x600000 bits of the mover count; 0x800000 blocks everyone.
  game::WalkMesh cw;
  cw.init(build(), {0, 0x200000});
  CHECK(!cw.blocked(3, 0) && cw.blocked(3, 0x200000) && !cw.blocked(3, 0x400000) && !cw.blocked(3, 0x1000));

  // move_actor: free move, 8-unit stand-off before the x = 200 boundary, full-speed slide along a wall edge.
  auto step = wm.move_actor(0, 20, 50, 5, 0, 0);
  CHECK(!step.blocked && near(step.x, 25) && near(step.z, 50));
  // The probe at 198 is inside; the -45 degree feeler (z = -0.7) leaves the mesh: a parallel slide, full step.
  step = wm.move_actor(2, 185, 5, 5, 0, 0);
  CHECK(near(step.x, 190) && near(step.z, 5));
  // The probe at 208 leaves the mesh: head-on, cancelled.
  step = wm.move_actor(2, 195, 50, 5, 0, 0);
  CHECK(step.blocked && !step.slid && near(step.x, 195));
  // Wall tri 3 across x = 100: sliding along the edge keeps |move| = 10.
  step = wm.move_actor(0, 88, 50, 7.07f, 7.07f, 0);
  CHECK(step.blocked);
  CHECK(step.slid && near(std::hypot(step.slide_dx, step.slide_dz), 10.0f, 1e-2f) && near(step.slide_dx, 0.0f));

  // The +-45 degree feelers (8 units). Walking +z at x = 96, 4 units from the wall x = 100: the straight probe (96, 62) is
  // free, the feeler at +45 degrees reaches x = 101.7 inside the wall triangle -> the first blocked feeler gives the edge, the move slides along
  // it at full speed (parallel: unchanged) and the result is flagged. Further from the wall (x = 90) nothing is blocked.
  step = wm.move_actor(0, 96, 50, 0, 4, 0);
  CHECK(step.blocked && step.slid && near(step.z, 54) && near(step.x, 96));
  step = wm.move_actor(0, 90, 50, 0, 4, 0);
  CHECK(!step.blocked && !step.slid && near(step.z, 54));

  // Pad walk: S / 16 per tick (5 at the default speed), halved on a tile with flag 0x1000 (the pushed-actor step with it).
  game::WalkMesh slow;
  slow.init(build(), {0x1000, game::kFlagBlockAll});
  game::Actor walker;
  walker.place(slow, 1, 20, 50, 0x800);
  walker.tick(slow, 0x800, false);
  CHECK(near(walker.current().x, 25) && near(walker.pad_step_x(), 5));
  walker.tick(slow, 0x800, false);
  CHECK(near(walker.current().x, 27.5f) && near(walker.pad_step_x(), 2.5f));

  // The mover's coarse square root and unit vector look at the eight leading bits of the sum of squares only: 31 * 31 + 1 = 962
  // counts as 960 (root 30), and (100, 0, 0) comes out 4098 long.
  CHECK(game::mover_sqrt(962) == 30 && game::mover_sqrt(1025) == 32 && game::mover_sqrt(2245) == 47 && game::mover_sqrt(0) == 0);
  const int axis[3] = {100, 0, 0};
  int unit[3];
  game::mover_normalise(axis, unit);
  CHECK(unit[0] == 4098 && unit[1] == 0 && unit[2] == 0);

  // On the slope (y = 0.2 per unit of x) a 5-unit step from x = 180 rises a whole unit of floor height (16 -> 17), so the 3D
  // re-make shortens it to 5 * 5 / |(5, 1)|.
  CHECK(wm.floor_height(2, 180.5f, 50.5f) == 16 && wm.floor_height(2, 185.2f, 50) == 17);
  game::Actor climber;
  climber.place(wm, 2, 180, 50, 0x800);
  climber.tick(wm, 0x800, false);
  CHECK(near(climber.current().x, 184.904f));

  if (failures) return 1;
  std::printf("walk mesh checks passed\n");
  return 0;
}
