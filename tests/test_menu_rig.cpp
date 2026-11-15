// The menu-model rig (game/menu_rig.h) on hand-built skeletons: a field rig and a half-size menu rig with an extra hand joint and other
// rest angles. Checks the pairing, the scale and pelvis ratio, that the field rest pose gives the menu rest pose, that a field joint's turn
// and the root's movement carry over, that a piece hung from another joint on each rig swings with the joint that carries it, that a
// joint without mesh the clips carry far keeps its own rest place at the field rest pose, that an object held by the hand on one rig and by the forearm on the other still pairs, that a rig
// whose limbs point elsewhere is refused, how a room's copy of a character is told from another model, and the hand-made pairings (taken as
// given when they fit the two skeletons; every table entry has one entry per menu joint), a joint a map marks not drawn, the body
// fingerprint that tells a story NPC's field model, and the per-character tunings (limbs taking the field limbs' directions, a body turning
// about the field joints' places, a foot drawn on the field foot and kept above its lowest point, a piece hanging plumb from its menu
// parent and tipped forward off the floor).
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

void test_swing() {
  // Field: a pendant hangs from the arm. Menu: the same pendant hangs from the spine beside the arm. The pendant keeps its menu offset from
  // the arm and swings with the arm, the joint that carries it; its own turn only spins it in place.
  assets::ModelAsset field = field_rig();
  field.skeleton.joints.push_back(joint(2, {0, 0, 0}, {0, 0, 50}));
  assets::ModelAsset menu = menu_rig();
  menu.skeleton.joints.push_back(joint(1, {0, 0, 0}, {50, 0, 25}));
  const std::vector<s8> map{0, 1, 2, -1, 3, 4, 5};
  const game::MenuRig rig = game::match_menu_rig(field, menu, map);
  CHECK(rig.usable && rig.swing_about.size() == 7 && rig.swing_about[6] == 2);
  const std::vector<render::Mat4> rest = rest_palette(menu);
  assets::ModelAsset spun = field;
  spun.skeleton.joints[5].rest_rot_deg = {90, 0, 0};
  std::vector<render::Mat4> out;
  game::menu_rig_palette(rig, rest_palette(spun), out);
  for (int row = 0; row < 3; ++row) CHECK(near(out[6].at(row, 3), rest[6].at(row, 3)));
  assets::ModelAsset swung = field;
  swung.skeleton.joints[2].rest_rot_deg = {0, 90, 0};
  game::menu_rig_palette(rig, rest_palette(swung), out);
  const render::Mat4 turn = render::mat4_from_rt(assets::rotation_zyx({0, 90, 0}), {0, 0, 0});
  for (int row = 0; row < 3; ++row)
    CHECK(near(out[6].at(row, 3) - out[2].at(row, 3), turn.at(row, 2) * 25.0f));
}

void test_travel_without_mesh() {
  // A field leg the clips carry far from its rest offset, on a menu hip that sits higher than the field one and carries no mesh: at the
  // field rest pose it stays on its own rest place (nothing to line up with the field piece), and both legs still match.
  assets::ModelAsset field = field_rig();
  assets::AnimationClip clip;
  clip.frame_count = 1;
  clip.tracks.resize(field.skeleton.joints.size());
  clip.tracks[3].trans.push_back({0.0f, {60, 300, 0}});
  field.clips.push_back(clip);
  assets::ModelAsset menu = menu_rig();
  menu.skeleton.joints[4].rest_trans = {30, -60, 0};
  menu.skeleton.joints[5].rest_trans = {0, 310, 0};
  const game::MenuRig rig = game::match_menu_rig(field, menu);
  CHECK(rig.usable && rig.placed_from.size() == 6 && rig.placed_from[4] < 0);
  std::vector<render::Mat4> out;
  game::menu_rig_palette(rig, rest_palette(field), out);
  const std::vector<render::Mat4> rest = rest_palette(menu);
  for (size_t j = 0; j < out.size() && j < rest.size(); ++j) CHECK(same(out[j], rest[j]));
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

assets::SkinVertex vertex_on(u16 joint, assets::Vec3 bind, assets::Vec3 local) {
  assets::SkinVertex out;
  out.joint = {joint, joint};
  out.bind_pos = bind;
  out.local[0] = local;
  return out;
}

void test_tuning() {
  // The table: Poshul's entry, none for a body without one.
  const game::RigTuning* poshul = game::menu_rig_tuning(0x0C);
  CHECK(poshul && poshul->align_limbs && poshul->field_body && game::menu_rig_map(0x0C).size() == 32);
  CHECK(!game::menu_rig_tuning(0x03));
  // align_limbs: the menu shin points forward at rest, the field shin down. The menu shin takes the field shin's direction, at rest and when
  // the field leg turns 90 degrees about x.
  assets::ModelAsset menu = menu_rig();
  menu.skeleton.joints[5].rest_trans = {0, 0, -250};
  const std::vector<s8> map{0, 1, 2, -1, 3, 4};
  game::MenuRig rig = game::match_menu_rig(field_rig(), menu, map);
  game::tune_menu_rig(rig, field_rig(), menu, {-1, true, false, {-1, -1, -1, -1}});
  std::vector<render::Mat4> out;
  game::menu_rig_palette(rig, rest_palette(field_rig()), out);
  CHECK(near(out[5].at(0, 3) - out[4].at(0, 3), 0.0f) && near(out[5].at(1, 3) - out[4].at(1, 3), 250.0f) && near(out[5].at(2, 3) - out[4].at(2, 3), 0.0f));
  assets::ModelAsset kicked = field_rig();
  kicked.skeleton.joints[3].rest_rot_deg = {90, 0, 0};
  game::menu_rig_palette(rig, rest_palette(kicked), out);
  const std::vector<render::Mat4> field_out = rest_palette(kicked);
  for (int row = 0; row < 3; ++row) CHECK(near((out[5].at(row, 3) - out[4].at(row, 3)) * 2.0f, field_out[4].at(row, 3) - field_out[3].at(row, 3)));
  // field_body: a head vertex on the spine on both rigs (scale 2), the menu spine 30 units behind the field one. The spine turns about the
  // field spine's place, and the root moves as far as the field root does.
  assets::ModelAsset field = field_rig();
  field.meshes.resize(1);
  field.meshes[0].pool = {vertex_on(1, {0, -750, 0}, {0, -50, 0})};
  menu = menu_rig();
  menu.skeleton.joints[0].rest_trans = {0, -240, 0};
  menu.skeleton.joints[1].rest_trans = {0, -110, 30};
  menu.meshes.resize(1);
  menu.meshes[0].pool = {vertex_on(1, {0, -375, 30}, {0, -25, 0})};
  rig = game::match_menu_rig(field, menu, map);
  CHECK(near(rig.scale, 2.0f) && near(rig.root_motion, 0.48f, 1e-3f));
  game::tune_menu_rig(rig, field, menu, {-1, false, true, {-1, -1, -1, -1}});
  CHECK(near(rig.root_motion, 0.5f, 1e-3f));
  assets::ModelAsset turned = field;
  turned.skeleton.joints[1].rest_rot_deg = {0, 90, 0};
  game::menu_rig_palette(rig, rest_palette(turned), out);
  const render::Mat4 turn = render::mat4_from_rt(assets::rotation_zyx({0, 90, 0}), {0, 0, 0});
  for (int row = 0; row < 3; ++row) CHECK(near(out[1].at(row, 3), (row == 1 ? -350.0f : 0.0f) + turn.at(row, 2) * 30.0f));
  // on_field: the menu foot is drawn on the field foot, the two pieces' centres together; a taller menu foot is lifted to the field foot's
  // lowest point.
  field = field_rig();
  field.meshes.resize(1);
  field.meshes[0].pool = {vertex_on(4, {60, 0, -20}, {0, 0, -20}), vertex_on(4, {60, 0, 20}, {0, 0, 20}), vertex_on(4, {90, 0, 0}, {30, 0, 0})};
  menu = menu_rig();
  menu.skeleton.joints[4].rest_trans = {40, 0, 0};
  menu.meshes.resize(1);
  menu.meshes[0].pool = {vertex_on(5, {40, -6, -10}, {0, -6, -10}), vertex_on(5, {40, -6, 10}, {0, -6, 10}), vertex_on(5, {55, -6, 0}, {15, -6, 0})};
  rig = game::match_menu_rig(field, menu, map);
  game::tune_menu_rig(rig, field, menu, {-1, false, false, {5, -1, -1, -1}});
  CHECK(rig.on_field.size() == 6 && rig.on_field[5]);
  game::menu_rig_palette(rig, rest_palette(field), out);
  CHECK(near((out[5].at(0, 3) + 5.0f) * rig.scale, 70.0f, 0.5f) && near((out[5].at(1, 3) - 6.0f) * rig.scale, 0.0f, 0.5f) && near(out[5].at(2, 3), 0.0f));
  menu.meshes[0].pool.push_back(vertex_on(5, {40, 20, 0}, {0, 20, 0}));
  rig = game::match_menu_rig(field, menu, map);
  game::tune_menu_rig(rig, field, menu, {-1, false, false, {5, -1, -1, -1}});
  game::menu_rig_palette(rig, rest_palette(field), out);
  CHECK(near((out[5].at(1, 3) + 20.0f) * rig.scale, 0.0f, 0.5f));
}

// Where the point at `local` in joint `index`'s frame lies under `palette`.
assets::Vec3 placed_at(const std::vector<render::Mat4>& palette, size_t index, assets::Vec3 local) {
  const render::Mat4& frame = palette[index];
  assets::Vec3 out{};
  for (int row = 0; row < 3; ++row)
    out[static_cast<size_t>(row)] = frame.at(row, 0) * local[0] + frame.at(row, 1) * local[1] + frame.at(row, 2) * local[2] + frame.at(row, 3);
  return out;
}

void test_hang() {
  // A hanging piece (Poshul's collar tag): the menu arm hangs from the spine, 50 units long, while its field partner turns 90 degrees about z.
  // It keeps its rest orientation and its place on the spine; the spine's own turn carries it but does not tilt it.
  assets::ModelAsset field = field_rig();
  field.meshes.resize(1);
  field.meshes[0].pool = {vertex_on(2, {100, -600, 0}, {0, 100, 0})};
  assets::ModelAsset menu = menu_rig();
  menu.meshes.resize(1);
  menu.meshes[0].pool = {vertex_on(2, {50, -300, 0}, {0, 50, 0})};
  const std::vector<s8> map{0, 1, 2, -1, 3, 4};
  const game::RigTuning tuning{-1, false, false, {-1, -1, -1, -1}, {2, -1}};
  game::MenuRig rig = game::match_menu_rig(field, menu, map);
  game::tune_menu_rig(rig, field, menu, tuning);
  CHECK(rig.hang.size() == 6 && rig.hang[2] && !rig.hang[1]);
  std::vector<render::Mat4> rest_out, out;
  game::menu_rig_palette(rig, rest_palette(field), rest_out);
  assets::ModelAsset swung = field;
  swung.skeleton.joints[2].rest_rot_deg = {0, 0, 90};
  game::menu_rig_palette(rig, rest_palette(swung), out);
  CHECK(same(out[2], rest_out[2]));
  assets::ModelAsset bent = field;
  bent.skeleton.joints[1].rest_rot_deg = {0, 0, 90};
  game::menu_rig_palette(rig, rest_palette(bent), out);
  const float reach = std::hypot(out[2].at(0, 3) - out[1].at(0, 3), out[2].at(1, 3) - out[1].at(1, 3), out[2].at(2, 3) - out[1].at(2, 3));
  CHECK(near(reach, 50.0f) && !near(out[2].at(0, 3), rest_out[2].at(0, 3), 1.0f));
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) CHECK(near(out[2].at(row, col), rest_out[2].at(row, col), 1e-3f));
  // A piece long enough to reach 50 units below the floor at rest is tipped forward (-z) until its lowest point is on the floor.
  menu.meshes[0].pool = {vertex_on(2, {50, 50, 0}, {0, 400, 0})};
  rig = game::match_menu_rig(field, menu, map);
  game::tune_menu_rig(rig, field, menu, tuning);
  game::menu_rig_palette(rig, rest_palette(field), out);
  const assets::Vec3 tip = placed_at(out, 2, {0, 400, 0});
  CHECK(near(tip[1], 0.0f, 0.5f) && tip[2] < -100.0f);
}

}  // namespace

int main() {
  test_match();
  test_rest_pose();
  test_transfer();
  test_swing();
  test_travel_without_mesh();
  test_beside();
  test_refused();
  test_same_model();
  test_map();
  test_hidden();
  test_npc_bodies();
  test_tuning();
  test_hang();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("menu rig: all checks passed\n");
  return 0;
}
