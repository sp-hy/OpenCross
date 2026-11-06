// Walk-mesh helper checks on a hand-built 3-triangle strip: containment, plane height, edge crossing, wall blocking
// by attr flags, boundary stop and wall sliding; the actor's script-animation rules and drawn blends, the screen ripple grid and the
// frame echo's projection, the dialogue portrait side; the field's item and grid script ops (FE 69 Kid's grid list, the FE 7D strip,
// the key-item range) and the FE 2F reset.
#include <array>
#include <cmath>
#include <cstdio>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game/actor.h"
#include "game/field_world.h"
#include "game/walk_mesh.h"
#include "render/render_target.h"

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

// Clip time on a stair tile follows the whole-unit height: forwards while it falls (climbing), backwards while it rises, still while it
// stays; off the stairs a back-off tick plays the clip backwards. The slope square (y = 0.2 per unit of x) is the stair here.
void test_stair_clip() {
  auto handle = std::make_shared<game::ModelHandle>();
  handle->asset.skeleton.joints.emplace_back();
  for (u32 i = 0; i < 6; ++i) {
    assets::AnimationClip anim_clip;
    anim_clip.frame_count = 64;
    anim_clip.tracks.resize(1);
    handle->asset.clips.push_back(anim_clip);
  }
  game::WalkMesh stairs;
  stairs.init(build(), {0x4000, game::kFlagBlockAll});
  game::Actor actor;
  actor.set_model(handle);
  actor.place(stairs, 2, 152.5f, 50, 0x800);
  // The first stair tick compares against the height of the last stair visit (none yet).
  actor.tick(stairs, game::kNoHeading, false);
  float t = actor.anim_time();
  actor.tick(stairs, game::kNoHeading, false);
  CHECK(near(actor.anim_time(), t));
  // Internal heading 0x800 walks +x: the floor drops away (y grows), so the climb clip runs backwards.
  t = actor.anim_time();
  actor.tick(stairs, 0x800, false);
  CHECK(near(actor.anim_time(), t - game::kKeyframesPerTick));
  t = actor.anim_time();
  actor.tick(stairs, 0, false);
  CHECK(near(actor.anim_time(), t + game::kKeyframesPerTick));
  // Off the stairs the clip runs forwards, and backwards for a tick that asked for it.
  game::WalkMesh flat;
  flat.init(build(), {0, game::kFlagBlockAll});
  game::Actor walker;
  walker.set_model(handle);
  walker.place(flat, 1, 20, 50, 0x800);
  for (int i = 0; i < 4; ++i) walker.step_visual(true, false);
  t = walker.anim_time();
  walker.begin_tick();
  walker.reverse_clip();
  walker.step_visual(true, false);
  CHECK(near(walker.anim_time(), t - game::kKeyframesPerTick));
  walker.begin_tick();
  walker.step_visual(true, false);
  CHECK(near(walker.anim_time(), t));
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
// Field battle hand-over: the snapshot drops every held extra pack and puts its holder on the stand slot whatever it plays (a pack id
// in the stand slot becomes 1); the return submits every current id again from frame 0 (a sequence from its top) with anim-done set.
void test_battle_pack_handover() {
  auto handle = std::make_shared<game::ModelHandle>();
  handle->asset.skeleton.joints.push_back(assets::Joint{});
  auto clip = [](const char* source, u32 index) {
    assets::AnimationClip anim_clip;
    anim_clip.source = source;
    anim_clip.index = index;
    anim_clip.frame_count = 8;
    anim_clip.name = std::string(source) + "_0" + std::to_string(index);
    anim_clip.tracks.resize(1);
    return anim_clip;
  };
  for (u32 i = 0; i < 6; ++i) handle->asset.clips.push_back(clip("s4", i));
  const std::vector<assets::AnimationClip> pack = {clip("pk", 0), clip("pk", 1)};

  // Pack held, a base clip playing: still back to the stand slot.
  game::Actor base_clip;
  base_clip.set_model(handle);
  base_clip.set_ex_clips(pack);
  base_clip.play(4);
  for (int i = 0; i < 6; ++i) base_clip.step_visual(false, false);
  CHECK(base_clip.holds_ex_pack() && base_clip.clip_name() == "s4_04");
  base_clip.drop_ex_pack();
  CHECK(!base_clip.holds_ex_pack() && base_clip.submitted_anim() == 1 && base_clip.clip_name() == "s4_01" && !base_clip.anim_busy());

  // Pack clip playing, stand slot naming a pack clip: the slot becomes 1.
  game::Actor pack_clip;
  pack_clip.set_model(handle);
  pack_clip.set_ex_clips(pack);
  pack_clip.set_anim_slot(game::kSlotStand, 0x81);
  pack_clip.play(0x80);
  pack_clip.step_visual(false, false);
  CHECK(pack_clip.clip_name() == "pk_00");
  pack_clip.drop_ex_pack();
  CHECK(pack_clip.anim_slot(game::kSlotStand) == 1 && pack_clip.clip_name() == "s4_01");

  // No pack: the snapshot leaves it alone; the return restarts its clip at frame 0.
  game::Actor plain;
  plain.set_model(handle);
  plain.set_ex_clips(pack);
  plain.clear_ex_clips();
  plain.play(4);
  for (int i = 0; i < 6; ++i) plain.step_visual(false, false);
  const float before = plain.anim_time();
  plain.drop_ex_pack();
  CHECK(plain.submitted_anim() == 4 && near(plain.anim_time(), before) && before > 0.5f);
  plain.restart_anim();
  CHECK(plain.clip_name() == "s4_04" && near(plain.anim_time(), 0) && !plain.anim_busy());

  // A second LOAD_ANIMATION on a holder also puts the stand clip back first.
  game::Actor reload;
  reload.set_model(handle);
  reload.set_ex_clips(pack);
  reload.play(4);
  reload.step_visual(false, false);
  reload.set_ex_clips(pack);
  CHECK(reload.submitted_anim() == 1 && reload.holds_ex_pack());

  // ANIM_PACK_RESTORE_START (BF) holds the default pack: a base clip played after it goes back to the stand slot at the snapshot.
  game::Actor restored;
  restored.set_model(handle);
  restored.hold_default_pack();
  restored.play(4);
  for (int i = 0; i < 6; ++i) restored.step_visual(false, false);
  CHECK(restored.holds_ex_pack() && restored.clip_name() == "s4_04");
  restored.drop_ex_pack();
  CHECK(!restored.holds_ex_pack() && restored.submitted_anim() == 1 && restored.clip_name() == "s4_01" && near(restored.anim_time(), 0));

  // A model with sequences (the same handle, given a table): the return starts the sequence from its top, which cuts its clip in at frame 0.
  auto& seq_handle = handle;
  auto entry = [](std::vector<s16> script) {
    std::vector<s16> words = {0x10, 0, 0, 0, 0, 0, 0, 0};
    words.insert(words.end(), script.begin(), script.end());
    return words;
  };
  seq_handle->asset.anim_seq.resize(4);
  seq_handle->asset.anim_seq[1] = entry({0x17, 0x15, 1, 8, 0x40, 0, 1, 9, 0});
  seq_handle->asset.anim_seq[3] = entry({0x17, 0x15, 3, 8, 0x40, 0, 1, 9, 0});
  game::Actor seq_actor;
  seq_actor.set_model(seq_handle);
  seq_actor.play(3);
  for (int i = 0; i < 12; ++i) seq_actor.step_visual(false, false);
  CHECK(seq_actor.clip_name() == "s4_03" && seq_actor.anim_time() > 0.5f);
  seq_actor.restart_anim();
  CHECK(seq_actor.clip_name() == "s4_03" && near(seq_actor.anim_time(), 0) && !seq_actor.anim_busy());
}

// A frozen actor (culled off screen, or hidden): the clip time stands still, a clip asked for meanwhile is started but held at its
// first frame, and a one-shot stays busy until the actor runs again, then goes on from where it stood.
void test_frozen_actor() {
  auto handle = std::make_shared<game::ModelHandle>();
  handle->asset.skeleton.joints.push_back(assets::Joint{});
  for (u32 i = 0; i < 6; ++i) {
    assets::AnimationClip anim_clip;
    anim_clip.source = "s4";
    anim_clip.index = i;
    anim_clip.frame_count = 8;
    anim_clip.name = "s4_0" + std::to_string(i);
    anim_clip.tracks.resize(1);
    handle->asset.clips.push_back(anim_clip);
  }
  game::Actor actor;
  actor.set_model(handle);
  actor.play(4);
  for (int i = 0; i < 4; ++i) actor.step_visual(false, false);
  const float before = actor.anim_time();
  actor.set_culled(true);
  for (int i = 0; i < 10; ++i) actor.step_visual(false, false);
  CHECK(actor.clip_name() == "s4_04" && near(actor.anim_time(), before) && before > 0.5f);
  actor.play(5);
  for (int i = 0; i < 10; ++i) actor.step_visual(false, false);
  CHECK(actor.clip_name() == "s4_05" && near(actor.anim_time(), 0) && actor.anim_busy());
  actor.set_culled(false);
  actor.step_visual(false, false);
  CHECK(actor.clip_name() == "s4_05" && actor.anim_time() > 0.1f && actor.anim_time() < 0.5f);
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
// The script-set model axes are drawn blended between ticks: a roll of 0 then 8 draws as 4 at half a tick; a jump past the snap limit,
// a scale reset and the first tick after creation draw the current value.
void test_axes_blend() {
  auto ticked = [](float roll0, float roll1, float scale0, float scale1) {
    game::Actor actor;
    actor.begin_tick();
    actor.roll = roll0;
    actor.axis_scale[0] = scale0;
    actor.begin_tick();
    actor.roll = roll1;
    actor.axis_scale[0] = scale1;
    return actor;
  };
  auto same = [](const render::Mat4& lhs, const render::Mat4& rhs) {
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col)
        if (!near(lhs.at(row, col), rhs.at(row, col), 1e-4f)) return false;
    return true;
  };
  CHECK(same(ticked(0, 8, 0.5f, 0.6f).model_matrix(0.5f), ticked(4, 4, 0.55f, 0.55f).model_matrix(0.5f)));
  CHECK(same(ticked(4090, 6, 1, 1).model_matrix(0.5f), ticked(0, 0, 1, 1).model_matrix(0.5f)));
  CHECK(same(ticked(0, 2000, 1365.0f / 4096, 1.0f / 4096).model_matrix(0.5f), ticked(2000, 2000, 1.0f / 4096, 1.0f / 4096).model_matrix(0.5f)));
  game::Actor fresh;
  fresh.begin_tick();
  fresh.roll = 100;
  CHECK(same(fresh.model_matrix(0.25f), ticked(100, 100, 1, 1).model_matrix(0.25f)));
}
// Screen ripple grid line of column 19 (the first grid quad's left edge): the exact cosine of the phase, and a fractional phase lands
// between its neighbours.
void test_screen_warp() {
  auto line19 = [](float phase) {
    render::ScreenWarpParams params;
    params.amp_x = 4;
    params.step_x = 128;
    params.phase_x = phase;
    std::vector<render::WarpQuad> quads;
    render::build_screen_warp(params, quads, 0);
    return quads.size() > 2 ? quads[2].xy[0] - 304.0f : 1e9f;
  };
  const double angle = (64.0 + 19.0 * 128.0) * (6.283185307179586 / 4096.0);
  CHECK(near(line19(64), static_cast<float>(std::cos(angle) * 4.0), 1e-3f));
  const float lo = line19(64), mid = line19(64.5f), hi = line19(65);
  CHECK((mid - lo) * (hi - mid) > 0.0f);
}

// The FE 14 frame echo's projection: 4196 scales a point by 4196 / 4096 about the centre, half a pass by its square root; a turn
// about z by 1024 (a quarter) maps x onto y; a tilt about y moves the near side outward.
void test_frame_echo() {
  game::FieldWorld::JoltView view;
  view.echo_scale = {4196, 4196, 4096};
  float x = 100, y = -50;
  game::FieldWorld::frame_echo_point(view, 1.0f, x, y);
  CHECK(near(x, 100.0f * 4196 / 4096, 1e-3f) && near(y, -50.0f * 4196 / 4096, 1e-3f));
  x = 100;
  y = 0;
  game::FieldWorld::frame_echo_point(view, 0.5f, x, y);
  CHECK(near(x, 100.0f * std::sqrt(4196.0f / 4096.0f), 1e-3f));
  view.echo_scale = {4096, 4096, 4096};
  view.echo_rot = {0, 0, 1024};
  x = 100;
  y = 0;
  game::FieldWorld::frame_echo_point(view, 1.0f, x, y);
  CHECK(near(x, 0.0f, 1e-3f) && near(y, 100.0f, 1e-3f));
  view.echo_rot = {0, 32, 0};
  float left = -160, right = 160, zero = 0;
  game::FieldWorld::frame_echo_point(view, 1.0f, left, zero);
  zero = 0;
  game::FieldWorld::frame_echo_point(view, 1.0f, right, zero);
  CHECK(right > -left && -left < 160.0f);
}

// Portrait side: heading 0 = left; a portrait yaw of 0x800 turns it right; heading 0x400 stays left (the pad's control yaw is not
// an input); 0xC00 is the last right heading; the window flags 0x800 / 0x1000 force a side.
void test_portrait_side() {
  using game::FieldWorld;
  CHECK(FieldWorld::portrait_on_left(0, 0, 0, 0));
  CHECK(!FieldWorld::portrait_on_left(0, 0x800, 0, 0));
  CHECK(FieldWorld::portrait_on_left(0x400, 0, 0, 0));
  CHECK(!FieldWorld::portrait_on_left(0x401, 0, 0, 0));
  CHECK(!FieldWorld::portrait_on_left(0xC00, 0, 0, 0));
  CHECK(FieldWorld::portrait_on_left(0x200, 0, -0x200, 0));
  CHECK(!FieldWorld::portrait_on_left(0, 0, 0, 0x800));
  CHECK(FieldWorld::portrait_on_left(0x800, 0, 0, 0x1000));
}

void test_give_item_element_count() {
  ui::GameState state;
  game::FieldWorld world;
  world.set_game_state(&state);
  world.give_item(0x200 + 0x61);
  world.give_item(0x200 + 0x6B);
  world.give_item(0x200 + 0x6C);
  world.give_item(0x200 + 0x60);
  CHECK(state.element_count(0x61) == 5);
  CHECK(state.element_count(0x6B) == 5);
  CHECK(state.element_count(0x6C) == 1);
  CHECK(state.element_count(0x60) == 1);
  for (int i = 0; i < 30; ++i) world.give_item(0x200 + 0x65);
  CHECK(state.element_count(0x65) == 99);
}
// Element table stand-in: the consumables 0x61..0x7D keep their copy count in a grid cell.
ui::MenuRules counted_rules() {
  ui::MenuRules rules;
  rules.pg.resize(0x80);
  for (int id = 0x61; id <= 0x7D; ++id) rules.pg[static_cast<size_t>(id)].raw[0xA] = 0x40;
  return rules;
}

// FE 69 on the PS1 reference grid: Kid's cells 4 / 9 / 30 / 31 hold consumables, the others single elements, a tech (21) and a
// fixed cell (34) stay; cell k counts k & 0xF copies. SAVE writes the list into variables 0x334.. / 0x394.., a save and a load
// keep it, and RESTORE reads each count from variable 0x394 + 2k (cell 4 gets cell 8's count, cell 9 cell 19's, cell 30 the
// zero of variable 0x3D0, cell 31 the high nibble of 0x3D2), single elements one copy each.
void test_kid_grid_save_restore() {
  const ui::MenuRules rules = counted_rules();
  ui::GameState state;
  state.rules = &rules;
  save::SaveChar& kid = state.slot.chars[1];
  kid.grid.fill(0x4000);
  for (auto [cell, id] : {std::pair{4, 0x69}, {5, 7}, {9, 0x6A}, {12, 0xA}, {13, 0x40}, {20, 7}, {21, 0xA3}, {30, 0x6B}, {31, 0x6C},
                          {34, 0x40A4}, {40, 0x4005}, {47, 0}})
    kid.grid[static_cast<size_t>(cell)] = static_cast<u16>(id);
  for (size_t k = 0; k < 48; ++k) kid.equipped_qty[k] = static_cast<u8>(k < 44 ? k & 0xF : 5);
  for (int i = 0; i < 10; ++i) state.add_element(0x69);
  game::FieldWorld world;
  world.set_game_state(&state);
  world.vm().set_var(0x3D2, 0x3F);
  std::array<u16, 48> stripped = kid.grid;
  for (u16& cell : stripped)
    if ((cell & 0x1FF) < 0x80) cell &= 0xC000;
  world.table_op(0xFE69, 0, 0);
  CHECK(kid.grid == stripped && kid.grid[21] == 0xA3 && kid.grid[34] == 0x40A4 && kid.grid[40] == 0x4000);
  CHECK(kid.equipped_qty[4] == 0 && kid.equipped_qty[21] == 5);
  CHECK(world.vm().var(0x334 + 2 * 4) == 0x69 && world.vm().var(0x334 + 2 * 31) == 0x6C && world.vm().var(0x334 + 2 * 40) == 5);
  CHECK(world.vm().var(0x334 + 2 * 21) == 0 && world.vm().var(0x334 + 2 * 34) == 0);
  const u16 counts[24] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x04, 0x76,
                          0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x30, 0x54, 0x76, 0x98, 0xBA, 0x55, 0x55};
  for (int j = 0; j < 24; ++j) CHECK(world.vm().var(static_cast<u16>(0x394 + 2 * j)) == counts[j]);
  CHECK(state.element_count(0x69) == 10);
  world.capture_state();

  ui::GameState loaded;
  loaded.rules = &rules;
  loaded.slot = state.slot;
  game::FieldWorld reloaded;
  reloaded.set_game_state(&loaded);
  reloaded.restore_vars();
  reloaded.table_op(0xFE69, 1, 0);
  CHECK(loaded.element_count(0x69) == 18);
  CHECK(loaded.element_count(7) == 2 && loaded.element_count(0xA) == 1 && loaded.element_count(0x40) == 1 && loaded.element_count(5) == 1);
  CHECK(loaded.element_count(0x6A) == 3 && loaded.element_count(0x6B) == 0 && loaded.element_count(0x6C) == 3);
  for (u16 id = 0x334; id < 0x3C4; id += 2) CHECK(reloaded.vm().var(id) == 0);
  CHECK(reloaded.vm().var(0x3D2) == 0x3F);
}

// FE 7D strips every cell, locked ones included (a fresh record has no unlocked row): copies for a consumable, one otherwise, none
// for a consumable with no count; techs stay; the accessories go back to the item stock.
void test_character_strip_locked_cells() {
  const ui::MenuRules rules = counted_rules();
  ui::GameState state;
  state.rules = &rules;
  save::SaveChar& guile = state.slot.chars[2];
  guile.grid[3] = 0xA6, guile.grid[9] = 0x40A7, guile.grid[20] = 0x4007, guile.grid[25] = 0x4069, guile.grid[29] = 0x4061;
  guile.equipped_qty.fill(5);
  guile.equipped_qty[25] = 3, guile.equipped_qty[29] = 0;
  guile.accessory = {0x13, 0, 0x58};
  game::FieldWorld world;
  world.set_game_state(&state);
  world.table_op(0xFE7D, 2, 0);
  CHECK(state.element_count(0x69) == 3 && state.element_count(7) == 1 && state.element_count(0x61) == 0);
  CHECK(guile.grid[20] == 0x4000 && guile.grid[25] == 0x4000 && guile.grid[29] == 0x4000 && guile.grid[3] == 0xA6 && guile.grid[9] == 0x40A7);
  CHECK(guile.equipped_qty[25] == 0 && guile.equipped_qty[9] == 5);
  CHECK(guile.accessory[0] == 0 && guile.accessory[2] == 0 && state.item_count(0x13) == 1 && state.item_count(0x58) == 1);
}

// The script item ops reach key items 1..63: 0x440 is neither given, taken nor counted, even with its bit set.
void test_script_key_item_range() {
  ui::GameState state;
  game::FieldWorld world;
  world.set_game_state(&state);
  world.give_item(0x440);
  CHECK(state.slot.key_items() == 0);
  world.give_item(0x43F);
  CHECK(state.slot.key_items() == 1ull << 62);
  state.slot.set_key_items(state.slot.key_items() | 1ull << 63);
  CHECK(world.item_count(0x440) == 0 && world.item_count(0x43F) == 1 && state.has_key_item(64));
  world.take_item(0x440);
  CHECK(state.slot.key_items() >> 63 == 1);
}

// FE 2F: the boot state over a game in progress; the debug variable and the disc in use stay, and the reset is reported once.
void test_field_reset() {
  ui::GameState state;
  game::FieldWorld world;
  world.set_game_state(&state);
  world.set_disc(2);
  state.set_gold(1796);
  world.vm().set_var(0x34, 1);
  world.vm().set_var(0x100, 7);
  world.vm().set_var(0x42, 1);
  CHECK(!world.take_game_reset());
  world.field_reset();
  CHECK(world.vm().var(0x100) == 0);
  CHECK(world.vm().var(0x34) == 1 && state.slot.var(0x34) == 1);
  CHECK(world.vm().var(0x42) == 1 && state.slot.var(0x42) == 1 && world.disc() == 2);
  CHECK(state.gold() != 1796);
  CHECK(world.take_game_reset());
  CHECK(!world.take_game_reset());
}

// FE 65 form 0 is not a screen: it writes variable 0x42 and switches the disc even while a menu request is pending.
void test_change_disc_with_pending_menu() {
  game::FieldWorld world;
  world.request_menu(script::MenuKind::MainMenu, 0);
  CHECK(world.menu_pending());
  world.request_menu(script::MenuKind::ChangeDisc, 1);
  CHECK(world.vm().var(0x42) == 1);
  CHECK(world.disc() == 2);
  CHECK(world.menu_pending());
}
}  // namespace

int main() {
  test_field_reset();
  test_change_disc_with_pending_menu();
  test_kid_grid_save_restore();
  test_character_strip_locked_cells();
  test_script_key_item_range();
  test_give_item_element_count();
  test_actor_anim();
  test_stair_clip();
  test_attached_rigid();
  test_axes_blend();
  test_screen_warp();
  test_frame_echo();
  test_anim_seq();
  test_battle_pack_handover();
  test_frozen_actor();
  test_portrait_side();
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
  // A push 5.7 degrees off the wall normal is head-on by its own direction; with a heading of 45 degrees (a script walk's start
  // heading) it slides along the wall at full length.
  step = wm.move_actor(2, 195, 50, 5, 0.5f, 0);
  CHECK(step.blocked && !step.slid && near(step.z, 50));
  step = wm.move_actor(2, 195, 50, 5, 0.5f, 0, 8.0f, 0.1963f, 0.70710678f, 0.70710678f);
  CHECK(step.slid && near(step.x, 195) && near(step.z, 50 + std::hypot(5.0f, 0.5f), 1e-2f));
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
