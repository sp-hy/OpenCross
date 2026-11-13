#pragma once
// [Enhancements] MenuModelsInField: a character's menu model (the Status screen's mesh, on its battle model's skeleton), or a story NPC's
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
// them, not the limbs and what they hold (Korcha's oar); kBattleModelFieldScale (the field-to-battle height ratio) when that cannot be
// measured. Pose transfer, every frame: a paired menu joint turns as its field joint turns away from its rest orientation, in model space
// (R = R_field * R_field_rest^T * R_menu_rest), so the menu model keeps its own rest posture and takes the field model's movement; an
// unpaired one keeps the angle and offset to its parent that the menu model's first pose clip gives it (its rest values without one: a held
// object such as Marcy's shield rests at the model origin and only the clips put it in the hand). Every joint sits at the menu skeleton's
// own rest offset, so the menu model keeps its proportions, with three exceptions for a paired joint, judged against its anchor (its nearest
// paired ancestor) and the field joint driving that anchor. A field joint the clips carry far (more than kTravelShare of the height and
// more than its own rest offset is long: a prop carried from the hip to the hand, Pip's feet pulled into the body), that hangs below that
// driver and that no other menu joint follows: the menu joint sits at the field joint's offset from the driver, scaled to the menu model,
// from the anchor, shifted so the two pieces' meshes line up. A field joint hanging below that driver through joints of its
// own (a skirt panel hung from the pelvis on the menu rig and from the thigh on the field rig): the menu joint's own rest offset from the
// nearest menu joint paired along that field chain swings with its field joint's turn. Any other translated field joint: the menu offset
// plus the field joint's move away from its rest offset (in model space, at the menu model's size). The menu root turns about the field root's place (scaled) and moves with the
// field root's movement away from its rest position, by the ratio of the two pelvis heights.
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
  // Per menu joint of a held prop (menu_rig_props): the field joint of the field prop whose place it takes, -1 = none; held_driver, the
  // nearest field joint above that one with a menu partner, held_anchor, that partner; held_turn, the turn laying the menu piece onto the
  // field piece at rest (held_arm: the menu joint's rest place from the menu piece's centre, turned, menu units; held_reach: the field piece's
  // centre from its joint at rest, field units).
  std::vector<int> held_from, held_driver, held_anchor;
  std::vector<assets::Mat3> held_turn;
  std::vector<assets::Vec3> held_arm, held_reach;
  std::vector<assets::Vec3> field_offset, menu_rest_pos, mesh_shift;
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
// The rig the field draws detailed body `body` with: menu_rig_map's pairing when there is one, else the automatic match; then each held
// prop of menu_rig_props laid onto its field prop (centres and principal axes of the two pieces' rest meshes, each axis pointing to the
// side its mesh reaches furthest) and from then on carried as the field prop is, from the nearest field joint above it that has a menu
// partner. A grafted piece is added to `menu` (a construct, its textures, a joint) before the menu model is uploaded.
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
// The menu model's joint matrices (menu units) for the field model's joint matrices `field_palette` (field units).
void menu_rig_palette(const MenuRig& rig, std::span<const render::Mat4> field_palette, std::vector<render::Mat4>& out);

}  // namespace oc::game
