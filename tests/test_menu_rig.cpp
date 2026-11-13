// The menu-model rig (game/menu_rig.h) on hand-built skeletons: a field rig and a half-size menu rig with an extra hand joint and other
// rest angles. Checks the pairing, the scale and pelvis ratio, that the field rest pose gives the menu rest pose, that a field joint's turn
// and the root's movement carry over, that an object held by the hand on one rig and by the forearm on the other still pairs, that a rig
// whose limbs point elsewhere is refused, how a room's copy of a character is told from another model, and the hand-made pairings (taken as
// given when they fit the two skeletons; every table entry has one entry per menu joint), a joint a map marks not drawn, and the body
// fingerprint that tells a story NPC's field model.
#include <cmath>
#include <cstdio>
#include <vector>

#include "game/field_model_source.h"
#include "game/menu_rig.h"

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

bool near(float lhs, float rhs, float eps = 0.05f) { return std::fabs(lhs - rhs) <= eps; }

assets::Joint joint(int parent, assets::Vec3 rot, assets::Vec3 trans) {
  assets::Joint out;
  out.parent = parent;
  out.rest_rot_deg = rot;
  out.rest_trans = trans;
  return out;
}

// Root (pelvis) -> spine -> arm, root -> leg -> shin. +y is down; the feet are at y = 0.
assets::ModelAsset field_rig() {
  assets::ModelAsset rig;
  rig.skeleton.joints = {joint(-1, {0, 0, 0}, {0, -500, 0}), joint(0, {0, 0, 0}, {0, -200, 0}), joint(1, {0, 0, 0}, {100, 0, 0}),
                joint(0, {0, 0, 0}, {60, 0, 0}), joint(3, {0, 0, 0}, {0, 500, 0})};
  return rig;
}

// The same body at half size, the arm turned 30 degrees about y at rest, and a hand joint (no field partner) under the arm.
assets::ModelAsset menu_rig() {
  assets::ModelAsset rig;
  rig.skeleton.joints = {joint(-1, {0, 0, 0}, {0, -250, 0}), joint(0, {0, 0, 0}, {0, -100, 0}), joint(1, {0, 30, 0}, {50, 0, 0}),
                joint(2, {0, 0, 0}, {20, 0, 0}), joint(0, {0, 0, 0}, {30, 0, 0}), joint(4, {0, 0, 0}, {0, 250, 0})};
  return rig;
}

std::vector<render::Mat4> rest_palette(const assets::ModelAsset& rig) {
  std::vector<render::Mat4> out;
  for (const assets::Joint& item : rig.skeleton.joints) {
    const render::Mat4 local = render::mat4_from_rt(assets::rotation_zyx(item.rest_rot_deg), item.rest_trans);
    out.push_back(item.parent >= 0 ? out[static_cast<size_t>(item.parent)] * local : local);
  }
  return out;
}

bool same(const render::Mat4& lhs, const render::Mat4& rhs) {
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col)
      if (!near(lhs.at(row, col), rhs.at(row, col), col == 3 ? 0.05f : 1e-3f)) return false;
  return true;
}

void test_match() {
  const game::MenuRig rig = game::match_menu_rig(field_rig(), menu_rig());
  CHECK(rig.usable);
  CHECK(rig.unmatched == 0);
  CHECK((rig.field_joint == std::vector<int>{0, 1, 2, -1, 3, 4}));
  CHECK(near(rig.scale, game::kBattleModelFieldScale));
  CHECK(near(rig.root_motion, 0.5f));
}

void test_rest_pose() {
  const game::MenuRig rig = game::match_menu_rig(field_rig(), menu_rig());
  std::vector<render::Mat4> out;
  game::menu_rig_palette(rig, rest_palette(field_rig()), out);
  const std::vector<render::Mat4> rest = rest_palette(menu_rig());
  CHECK(out.size() == rest.size());
  for (size_t j = 0; j < out.size() && j < rest.size(); ++j) CHECK(same(out[j], rest[j]));
}

void test_transfer() {
  const game::MenuRig rig = game::match_menu_rig(field_rig(), menu_rig());
  assets::ModelAsset posed = field_rig();
  // The arm turns 90 degrees about z; the pelvis drops 100 field units.
  posed.skeleton.joints[2].rest_rot_deg = {0, 0, 90};
  posed.skeleton.joints[0].rest_trans[1] += 100.0f;
  std::vector<render::Mat4> out;
  game::menu_rig_palette(rig, rest_palette(posed), out);
  const std::vector<render::Mat4> rest = rest_palette(menu_rig());
  CHECK(near(out[0].at(1, 3), rest[0].at(1, 3) + 50.0f));
  // The arm is the menu rest arm turned by the same 90 degrees in model space: x -> y.
  const render::Mat4 turn = render::mat4_from_rt(assets::rotation_zyx({0, 0, 90}), {0, 0, 0});
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) CHECK(near((turn * rest[2]).at(row, col), out[2].at(row, col), 1e-3f));
  // The hand rides on the arm: 20 units along the arm's turned x axis.
  for (int row = 0; row < 3; ++row) CHECK(near(out[3].at(row, 3) - out[2].at(row, 3), out[2].at(row, 0) * 20.0f));
  // The leg did not turn.
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) CHECK(near(out[5].at(row, col), rest[5].at(row, col), 1e-3f));
}

void test_beside() {
  // Field: the arm ends in a hand holding a rod (hand -> rod). Menu: the rod hangs off the arm next to the hand.
  assets::ModelAsset field = field_rig();
  field.skeleton.joints.push_back(joint(2, {0, 0, 0}, {80, 0, 0}));
  field.skeleton.joints.push_back(joint(5, {0, 0, 0}, {0, 0, 60}));
  assets::ModelAsset menu = menu_rig();
  menu.skeleton.joints[2].rest_rot_deg = {0, 0, 0};
  menu.skeleton.joints[3].rest_trans = {40, 0, 0};
  menu.skeleton.joints.push_back(joint(2, {0, 0, 0}, {40, 0, 30}));
  const game::MenuRig rig = game::match_menu_rig(field, menu);
  CHECK(rig.usable);
  CHECK(rig.unmatched == 0);
  CHECK(rig.field_joint.size() == 7 && rig.field_joint[3] == 5 && rig.field_joint[6] == 6);
  CHECK(rig.plan != game::MenuRigPlan::Under);
}

void test_refused() {
  // The menu legs point up: the bones do not agree, the rig is not used.
  assets::ModelAsset flipped = menu_rig();
  flipped.skeleton.joints[5].rest_trans = {0, -250, 0};
  flipped.skeleton.joints[4].rest_trans = {30, -200, 0};
  CHECK(!game::match_menu_rig(field_rig(), flipped).usable);
}

void test_same_model() {
  assets::ModelAsset copy = field_rig();
  CHECK(game::same_skeleton(field_rig().skeleton, copy.skeleton));
  copy.skeleton.joints[4].rest_trans[1] += 1.0f;
  CHECK(!game::same_skeleton(field_rig().skeleton, copy.skeleton));
  assets::ModelAsset body = field_rig(), other = field_rig();
  body.meshes.resize(1);
  body.meshes[0].pool.resize(2);
  body.meshes[0].pool[1].bind_pos = {0, -900, 0};
  other.meshes = body.meshes;
  CHECK(game::same_mesh(body, other));
  other.meshes[0].pool[1].bind_pos[1] = -880;
  CHECK(!game::same_mesh(body, other));
}

void test_map() {
  // The flipped rig of test_refused, paired by hand: used, joints as given (two menu joints may follow one field joint).
  assets::ModelAsset flipped = menu_rig();
  flipped.skeleton.joints[5].rest_trans = {0, -250, 0};
  const std::vector<s8> map{0, 1, 2, 2, 3, 4};
  const game::MenuRig rig = game::match_menu_rig(field_rig(), flipped, map);
  CHECK(rig.usable && rig.plan == game::MenuRigPlan::Map);
  CHECK((rig.field_joint == std::vector<int>{0, 1, 2, 2, 3, 4}));
  // A map that does not fit: one entry short, or a field joint the model does not have.
  CHECK(!game::match_menu_rig(field_rig(), flipped, std::vector<s8>{0, 1, 2, 2, 3}).usable);
  CHECK(!game::match_menu_rig(field_rig(), flipped, std::vector<s8>{0, 1, 2, 2, 3, 9}).usable);
  CHECK(game::menu_rig_map(0).empty());
  CHECK(game::menu_rig_map(0x24).size() == 34);
}

void test_hidden() {
  // The menu hand marked not drawn: its matrix keeps its place but loses its axes (its vertices collapse onto the joint); the rest is as given.
  const std::vector<s8> map{0, 1, 2, game::kHiddenJoint, 3, 4};
  const game::MenuRig rig = game::match_menu_rig(field_rig(), menu_rig(), map);
  CHECK(rig.usable);
  CHECK((rig.field_joint == std::vector<int>{0, 1, 2, -1, 3, 4}));
  CHECK(rig.hidden.size() == 6 && rig.hidden[3] && !rig.hidden[2]);
  std::vector<render::Mat4> out;
  game::menu_rig_palette(rig, rest_palette(field_rig()), out);
  const std::vector<render::Mat4> rest = rest_palette(menu_rig());
  for (int row = 0; row < 3; ++row) {
    CHECK(near(out[3].at(row, 3), rest[3].at(row, 3)));
    for (int col = 0; col < 3; ++col) CHECK(out[3].at(row, col) == 0.0f);
  }
  CHECK(same(out[2], rest[2]));
}

void test_npc_bodies() {
  // The fingerprint: equal for a copy, different once a vertex or a parent changes.
  assets::ModelAsset body = field_rig();
  body.meshes.resize(1);
  body.meshes[0].pool.resize(2);
  body.meshes[0].pool[1].bind_pos = {0, -900, 0};
  const assets::ModelAsset copy = body;
  CHECK(game::body_fingerprint(body) == game::body_fingerprint(copy));
  assets::ModelAsset moved = body;
  moved.meshes[0].pool[1].bind_pos[1] = -880;
  CHECK(game::body_fingerprint(body) != game::body_fingerprint(moved));
  assets::ModelAsset rewired = body;
  rewired.skeleton.joints[4].parent = 0;
  CHECK(game::body_fingerprint(body) != game::body_fingerprint(rewired));
  // Every table entry names a monster battle model, past the menu models, and is found by its own fingerprint.
  CHECK(!game::npc_bodies().empty());
  for (const game::NpcBody& npc : game::npc_bodies()) {
    CHECK(npc.battle_model > 0x32);
    CHECK(game::npc_body(npc.fingerprint) == npc.battle_model);
  }
  CHECK(game::npc_body(game::body_fingerprint(body)) == -1);
  // Held props: listed per body, none for a body without one; without one match_body_rig is the plain match.
  CHECK(game::menu_rig_props(0).empty());
  for (const game::HeldProp& prop : game::menu_rig_props(140)) CHECK(prop.body == 140 && prop.menu_joints[0] >= 0);
  CHECK(!game::menu_rig_props(140).empty());
  assets::ModelAsset menu = menu_rig();
  const game::MenuRig plain = game::match_menu_rig(field_rig(), menu_rig()), body_rig = game::match_body_rig(field_rig(), menu, 0);
  CHECK(plain.field_joint == body_rig.field_joint);
  for (const int from : body_rig.held_from) CHECK(from == -1);
}

}  // namespace

int main() {
  test_match();
  test_rest_pose();
  test_transfer();
  test_beside();
  test_refused();
  test_same_model();
  test_map();
  test_hidden();
  test_npc_bodies();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("menu rig: all checks passed\n");
  return 0;
}
