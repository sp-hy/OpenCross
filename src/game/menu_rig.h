#pragma once
// HD models in fields: a character's menu model (the Status screen's mesh, on its battle model's skeleton), or a story NPC's
// battle model, posed by the field model, whose skeleton has other joints, rest angles and proportions. "Menu model" below stands for both.
//
// Matching, once per model pair: both rest poses are built in model space and scaled to the same height (the joints' vertical extent).
// From the root down, the children of every paired field joint are paired one to one with free menu joints, cheapest pair first, two
// children never in the same branch (a fallback allows it); the cost is the distance of the two joints plus twice the distance of the centres of their subtrees,
// plus penalties per menu joint skipped (more for one carrying mesh), per difference in subtree size, and for a joint carrying mesh paired
// with one that carries none. A field joint left on an empty helper below an unpaired mesh joint moves up to it; torso pieces left unpaired
// (skirt and coat panels, cape tops) follow the field joint whose mesh lies nearest. Where the menu joint may lie
// depends on the strategy (MenuRigPlan): only under the parent's partner; also beside it, under the grandparent's partner (a held object
// that hangs off the hand on one rig hangs off the forearm on the other), at a cost; or beside it only once nothing under it is free, with
// two siblings never placed in the same branch. All three are tried and the usable one whose bones agree best is kept. A match is usable
// when every field joint with children found a partner and the paired bones point the same way (kMaxBoneAngleP90 / kMaxBoneAngle).
//
// The menu model is drawn as tall as the field model, feet to the top of the body: the head, its chain down to the root and what hangs from
// them, not the limbs and what they hold (Marcy's shield); kBattleModelFieldScale (the field-to-battle height ratio) when that cannot be
// measured. Pose transfer, every frame: a paired menu joint turns as its field joint turns away from its rest orientation, in model space
// (R = R_field * R_field_rest^T * R_menu_rest), so the menu model keeps its own rest posture and takes the field model's movement; an
// unpaired one keeps its rest angle and offset to its parent, or those of the menu model's first pose clip when that clip carries it far
// from its rest offset (a held object such as Marcy's shield rests at the model origin and only the clips put it in the hand; a skirt panel
// the Status stance merely flares out keeps its rest angle). Every joint sits at the menu skeleton's
// own rest offset, so the menu model keeps its proportions, with three exceptions for a paired joint, judged against its anchor (its nearest
// paired ancestor) and the field joint driving that anchor. A field joint the clips carry far (more than kTravelShare of the height and
// more than its own rest offset is long: a prop carried from the hip to the hand, Pip's feet pulled into the body), that hangs below that
// driver, that no other menu joint follows, that carries a face of its own and whose menu joint carries mesh: the menu joint sits at the field joint's offset from the
// driver, scaled to the menu model, from the anchor, shifted so the two pieces' meshes line up. A field joint hanging below that driver
// through joints of its own (a skirt panel hung from the pelvis on the menu rig and from the thigh on the field rig): the menu joint's own
// rest offset from the nearest menu joint paired along that field chain swings as the field joint's parent turns (the joint that carries
// the offset; the field joint's own turn only spins the piece). Any other translated field joint: the menu offset
// plus the field joint's move away from its rest offset (in model space, at the menu model's size), unless that move only stretches the
// field mesh (the field joint's mesh is joined to its neighbours') while the menu piece is free of the rest of its mesh and would come off
// the body. The menu root turns about the field root's place (scaled) and moves with the
// field root's movement away from its rest position, by the ratio of the two pelvis heights. A few characters have a RigTuning that
// changes this: limbs that take the field limbs' directions, a body that turns about the field joints' places, pieces drawn on their
// field pieces. A menu piece is drawn only while the field actor draws the construct of its field piece (MenuRig::shown_with): the field
// sequences show and hide props.
#include <array>
#include <span>
#include <vector>

#include "assets/model.h"
#include "core/types.h"
#include "render/mat4.h"
#include "render/mesh_pipeline.h"

namespace oc::game {

// The 90th-percentile and the worst angle between a paired field bone and its menu bone in the rest poses, in degrees.
constexpr float kMaxBoneAngleP90 = 40.0f, kMaxBoneAngle = 90.0f;

// In a hand-made pairing: the menu joint and what hangs from it are not drawn (its vertices collapse onto the joint), for a prop the field
// model does not carry (Dario's sword).
constexpr s8 kHiddenJoint = -2;

// Map: the hand-made pairing of menu_rig_map.
enum class MenuRigPlan { Under, Beside, BesideWhenFull, Map };

// A field model vertex: its joint and its place in that joint's frame.
struct MenuRigPoint {
  int joint;
  assets::Vec3 local;
};

struct MenuRig {
  // Per menu joint: its field joint, -1 = none, and its parent.
  std::vector<int> field_joint, menu_parent;
  // Model-space rest rotations (field joints, menu joints), the menu joints' rest rotation and offset relative to their parent.
  std::vector<assets::Mat3> field_rest, menu_rest, menu_local;
  std::vector<assets::Vec3> menu_offset;
  // Per menu joint, how it is placed (see the top of this file), -1 / false where it sits at its own offset from its parent: placed_from,
  // the field joint driving its anchor (its nearest paired ancestor), from which its own field joint's offset is taken; swing_about, the menu
  // joint about which its rest offset (menu_rest_pos, model space) swings; moved, its own offset plus its field joint's move away from its
  // rest offset (field_offset, from field_parent).
  // mesh_shift: for a placed joint, how much further its field piece's mesh hangs from its joint than the menu piece's does (rest pose,
  // model space, menu units), turned with the piece.
  std::vector<int> placed_from, anchor, swing_about, field_parent;
  std::vector<bool> moved;
  // Per menu joint: not drawn (kHiddenJoint in the map, or below such a joint).
  std::vector<bool> hidden;
  // Per menu joint: the field construct it is shown and hidden with (the one holding every vertex of its field joint, a held prop's field
  // piece, or its parent's for a joint without a partner), -1 = always drawn.
  std::vector<int> shown_with;
  // Per menu joint, only with a menu_rig_tuning: the menu joint that carries it (RigTuning::carry), -1 = its own parent.
  std::vector<int> carried_by;
  // Per menu joint, only with a menu_rig_tuning: outer (RigTuning::outer); floored (RigTuning::floor: menu_sole holds its piece's
  // vertices); lead, the two field joints of which it follows the one that swings its piece further (RigTuning::lead; -1 = its own field
  // joint), with lead_arm the piece's mesh centre from its joint (rest pose, model space) and lead_back whether further means backwards.
  std::vector<bool> outer, floored, lead_back;
  std::vector<std::array<int, 2>> lead;
  std::vector<assets::Vec3> lead_arm;
  // Per menu joint, only with a menu_rig_tuning: aimed (RigTuning::aim), with aim_point the centre of its field joint's vertices in that
  // joint's frame, and, in the rest pose, model space and menu units, aim_rest that centre from the menu joint and aim_arm the far end of
  // the menu piece from its joint.
  std::vector<bool> aimed;
  std::vector<assets::Vec3> aim_point, aim_rest, aim_arm;
  // Per menu joint of a held prop (menu_rig_props): the field joint of the field prop whose place it takes, -1 = none; held_driver, the
  // nearest field joint above that one with a menu partner, held_anchor, that partner; held_turn, the turn laying the menu piece onto the
  // field piece at rest (held_arm: the menu joint's rest place from the menu piece's centre, turned, menu units; held_reach: the field piece's
  // centre from its joint at rest, field units).
  std::vector<int> held_from, held_driver, held_anchor;
  std::vector<assets::Mat3> held_turn;
  std::vector<assets::Vec3> held_arm, held_reach;
  std::vector<assets::Vec3> field_offset, menu_rest_pos, mesh_shift;
  // Per menu joint, only with a menu_rig_tuning: pivot_shift, for a joint on the body chain of a character whose body follows the field
  // body, its field joint's rest place (menu units) less its own, the place it turns about; on_field, drawn on its field piece (mesh_shift
  // then lines up the two pieces' mesh centres), with menu_sole the menu piece's vertices from its joint (rest pose, model space, menu
  // units) and field_sole the field piece's vertices (field joint, place in that joint's frame), which keep it from reaching lower than the
  // field piece; hang, hangs plumb from where its parent carries it (RigTuning::hang; menu_sole without the alignment, field_sole as above).
  std::vector<assets::Vec3> pivot_shift;
  std::vector<bool> on_field, hang;
  std::vector<std::vector<assets::Vec3>> menu_sole;
  std::vector<std::vector<MenuRigPoint>> field_sole;
  assets::Vec3 field_root_rest{};
  // Field root movement -> menu units.
  float root_motion = 1.0f;
  // Menu units -> field units: the menu model is drawn this much larger.
  float scale = 1.0f;
  // Match quality: field joints without a partner (all, and those with children), bone angles (degrees), mean joint distance (share of
  // the height); the strategy that made it.
  int unmatched = 0, unmatched_inner = 0;
  float bone_p90 = 0.0f, bone_max = 0.0f, mean_offset = 0.0f;
  MenuRigPlan plan = MenuRigPlan::Under;
  bool usable = false;
};

// The best of the three strategies, with siblings in separate branches; without that rule only when no strategy is usable with it.
MenuRig match_menu_rig(const assets::ModelAsset& field, const assets::ModelAsset& menu);
MenuRig match_menu_rig(const assets::ModelAsset& field, const assets::ModelAsset& menu, MenuRigPlan plan);
// A rig from a hand-made pairing (field joint per menu joint, -1 = none, kHiddenJoint = not drawn; several menu joints may follow one field
// joint): usable when the map has an entry for every menu joint and names field joints the field model has.
MenuRig match_menu_rig(const assets::ModelAsset& field, const assets::ModelAsset& menu, std::span<const s8> map);
// The hand-made pairing for the detailed bodies whose skeleton the matcher cannot pair with their field model's, by battle model (the
// character model byte for a menu model); empty for the others.
std::span<const s8> menu_rig_map(int body);
// A prop the detailed body holds otherwise than the field model (the Porre soldiers' rifle, aimed in battle, slung in the field): its menu
// joints (the first is the piece's root, -1 ends the list) take the place of the field prop on `field_joint`. No menu joint (the first -1):
// the detailed body lacks the prop (Radius's cane), and the field piece itself is grafted onto it.
struct HeldProp {
  int body;
  int field_joint;
  std::array<s8, 4> menu_joints;
};
std::span<const HeldProp> menu_rig_props(int body);
// Adjustments of the pose transfer for a detailed body whose field rig the general rules follow badly (match_body_rig applies them).
// align_limbs: each paired joint off the body chain first turns its rest bone (to the first menu joint below it paired with a child of
// its field joint) onto that field bone, so the limb takes the field limb's directions rather than its turns (a field rest pose holding the
// legs far from where the clips hold them). field_body: the body chain (the head down to the root) turns about the field joints' places
// and the root moves as far as the field root does, at the field model's size (a body whose field joints sit elsewhere in the body than the
// menu joints). on_field (-1 = unused): menu pieces that hang free of the body mesh, drawn on their field piece: placed so the two pieces' mesh
// centres meet, turned as the field piece is, and lifted when they would reach lower than the field piece does (a longer foot). hang (-1
// or 0 = unused): paired menu pieces that hang plumb from where their menu parent carries them (their own offset from it), keeping their
// rest orientation to the root, and tipped forward about their joint when they would reach lower than their field piece (a collar tag on
// the chest's collar, where the field collar and its ring ride the head and the clips turn the round ring about itself). stance: every
// joint without a field partner takes the angle and offset of the menu model's first pose clip (the Status screen's) whether or not that
// clip carries it far (cloak and veil pieces that lie better as that stance holds them than at rest). keep (-1 = unused): paired menu
// joints that sit at their own offset from their menu parent whatever their field joint's place does (none of the three placement
// exceptions): they only turn with the field joint (cloth panels whose field joints hang elsewhere on the body). carry (pairs of menu
// joint and carrier, -1 = unused): the joint sits at its rest place from the carrier, an earlier menu joint that is not its parent, and
// that place turns as the carrier turns (a coat panel hung from the shin on the menu rig that belongs to the pelvis's coat on the field
// rig); when its field joint is translated (and the joint is not in keep or hang) it also takes that joint's move away from its rest offset. lay (-1 = unused): paired menu pieces without a paired joint below them, turned once so that the line from the joint to the
// centre of the piece's mesh points as the field piece's does (a ribbon modelled swept back where the field ribbon hangs straight).
// posture: the joints of the body chain below the head (down to the root) take the field joints' turns away from the field stand pose (first
// keyframe of clip 1) instead of the field rest pose (a field model that stands swayed against its own rest pose, on a detailed body
// modelled upright).
// rest (joint -1 = unused): a turn (degrees about x, y, z, composed as the joints' own angles are) and an offset (menu units) laid over a
// joint's rest: for a joint without a field partner, over its angle and offset to its parent (a ruff modelled level where the field's is
// tilted, a wheel drawn tucked into its well); for a paired joint the turn alone, in model space. floor (-1 = unused): joints whose
// piece (with what hangs under it) is lifted whenever its lowest vertex would reach below the floor (a robe panel hung from the pelvis in
// a crouch). lead (menu joint, two field joints; -1 = unused): the joint turns with whichever of the two field joints swings its piece
// further forward (a piece in front of the root) or back (one behind it): a single front skirt panel between two legs follows the leading
// leg. outer (-1 = unused): carried joints that take the place their own parent gives them whenever that lies further out from the
// carrier than the carried place, the way the carried place lies from it, easing from one to the other as they cross (a coat panel that must stay in front of the knee it hangs
// from). aim (-1 = unused): paired joints that sit at their own offset as keep's do and do not take their field joint's turn: each is
// turned, from the way the joint that carries it (its carrier, else its parent) holds it, so that the far end of its piece moves as the
// centre of the field joint's vertices has moved against that carrier (a long coat panel paired with a hem joint that holds two vertices
// and tips far over: the panel swings out as far as the hem does, not as steeply).
struct RigRest {
  s8 joint = -1;
  std::array<s16, 3> turn{};
  std::array<s16, 3> offset{};
};
struct RigTuning {
  int body;
  bool align_limbs, field_body;
  std::array<s8, 4> on_field;
  std::array<s8, 2> hang;
  bool stance = false;
  std::array<s8, 8> keep{-1, -1, -1, -1, -1, -1, -1, -1};
  std::array<std::array<s8, 2>, 4> carry{{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}};
  std::array<s8, 6> lay{-1, -1, -1, -1, -1, -1};
  bool posture = false;
  std::array<RigRest, 4> rest{};
  std::array<s8, 4> floor{-1, -1, -1, -1};
  std::array<std::array<s8, 3>, 4> lead{{{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}};
  std::array<s8, 4> outer{-1, -1, -1, -1};
  std::array<s8, 4> aim{-1, -1, -1, -1};
};
// The entry for detailed body `body`, nullptr for none.
const RigTuning* menu_rig_tuning(int body);
// Applies `tuning` to a matched rig (match_body_rig does for menu_rig_tuning's entry).
void tune_menu_rig(MenuRig& rig, const assets::ModelAsset& field, const assets::ModelAsset& menu, const RigTuning& tuning);
// The rig the field draws detailed body `body` with: menu_rig_map's pairing when there is one, else the automatic match; then each held
// prop of menu_rig_props laid onto its field prop (centres and principal axes of the two pieces' rest meshes, each axis pointing to the
// side its mesh reaches furthest) and from then on carried as the field prop is, from the nearest field joint above it that has a menu
// partner; no other menu joint follows the field prop's joint (a finger the matcher paired with it rides its hand). A grafted piece is
// added to `menu` (a construct, its textures, a joint) before the menu model is uploaded. A prop modelled into a body joint's own piece
// (Pierre's shield on his forearm's joint, Orcha's wok on its strap's joint) is split off first: the shells of that joint's mesh that
// touch no other joint's mesh move onto a new joint of `menu`, the last one, which the body's map then hides or pairs like any other.
// Last, MenuRig::shown_with is filled in.
MenuRig match_body_rig(const assets::ModelAsset& field, assets::ModelAsset& menu, int body);
// Same joints, parents, rest angles and offsets: a room's own copy of a character's field model has its field model's skeleton.
bool same_skeleton(const assets::Skeleton& lhs, const assets::Skeleton& rhs);
// Same constructs with the same rest-pose vertices (within half a unit): with the same skeleton, the same character's model. Another
// person may wear a character's skeleton with a mesh of their own.
bool same_mesh(const assets::ModelAsset& lhs, const assets::ModelAsset& rhs);
// A hash of the joint parents and the constructs' rest-pose vertices (to the unit): every room's copy of one model gives the same value.
u64 body_fingerprint(const assets::ModelAsset& model);
// A story NPC whose battle model (game::kBattleModels index) is drawn for its own field model (a room model, or a field model past the menu
// models), known by that field model's body_fingerprint.
struct NpcBody {
  u64 fingerprint;
  int battle_model;
};
std::span<const NpcBody> npc_bodies();
// npc_bodies' battle model for a field model's fingerprint, -1 for any other model.
int npc_body(u64 fingerprint);
// The menu model's own part flags (S3: hidden, nearest-corner sort, env-mapped with its own page, part colours) in place of the field model's.
void menu_model_parts(const assets::ModelAsset& menu, render::MeshPipeline::DrawParams& dp);
// The menu model's joint matrices (menu units) for the field model's joint matrices `field_palette` (field units). `hidden_constructs`:
// bit k = the field actor does not draw construct k now (its sequences show and hide props); the menu pieces shown with it
// (MenuRig::shown_with) are not drawn either. A piece that is not drawn has its vertices collapsed onto its joint, or onto the topmost
// joint above it that is not drawn.
void menu_rig_palette(const MenuRig& rig, std::span<const render::Mat4> field_palette, std::vector<render::Mat4>& out, u32 hidden_constructs = 0);

}  // namespace oc::game
