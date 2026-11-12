#pragma once
// [Enhancements] MenuModelsInField: a character's menu model (the Status screen's mesh, on its battle model's skeleton) posed by the
// character's field model, whose skeleton has other joints, rest angles and proportions.
//
// Matching, once per model pair: both rest poses are built in model space and scaled to the same height (the joints' vertical extent).
// From the root down, the children of every paired field joint are paired one to one with free menu joints, cheapest pair first; the cost
// is the distance of the two joints plus twice the distance of the centres of their subtrees, plus penalties per menu joint skipped, per
// difference in subtree size, and for a joint carrying mesh vertices paired with one that carries none. Where the menu joint may lie
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
// object such as Marcy's shield rests at the model origin and only the clips put it in the hand). Every joint sits
// at the menu skeleton's own rest offset, so the menu model keeps its proportions; the field root's movement away from its rest position
// moves the menu root by the ratio of the two pelvis heights.
#include <span>
#include <vector>

#include "assets/model.h"
#include "core/types.h"
#include "render/mat4.h"
#include "render/mesh_pipeline.h"

namespace oc::game {

// The 90th-percentile and the worst angle between a paired field bone and its menu bone in the rest poses, in degrees.
constexpr float kMaxBoneAngleP90 = 35.0f, kMaxBoneAngle = 90.0f;

// Map: the hand-made pairing of menu_rig_map.
enum class MenuRigPlan { Under, Beside, BesideWhenFull, Map };

struct MenuRig {
  // Per menu joint: its field joint, -1 = none, and its parent.
  std::vector<int> field_joint, menu_parent;
  // Model-space rest rotations (field joints, menu joints), the menu joints' rest rotation and offset relative to their parent.
  std::vector<assets::Mat3> field_rest, menu_rest, menu_local;
  std::vector<assets::Vec3> menu_offset;
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

// The best of the three strategies.
MenuRig match_menu_rig(const assets::ModelAsset& field, const assets::ModelAsset& menu);
MenuRig match_menu_rig(const assets::ModelAsset& field, const assets::ModelAsset& menu, MenuRigPlan plan);
// A rig from a hand-made pairing (field joint per menu joint, -1 = none; several menu joints may follow one field joint): usable when the
// map has an entry for every menu joint and names field joints the field model has.
MenuRig match_menu_rig(const assets::ModelAsset& field, const assets::ModelAsset& menu, std::span<const s8> map);
// The hand-made pairing for the menu models whose skeleton the matcher cannot pair with their field model's, by character model byte;
// empty for the others.
std::span<const s8> menu_rig_map(int model_byte);
// Same joints, parents, rest angles and offsets: a room's own copy of a character's field model has its field model's skeleton.
bool same_skeleton(const assets::Skeleton& lhs, const assets::Skeleton& rhs);
// Same constructs with the same rest-pose vertices (within half a unit): with the same skeleton, the same character's model. Another
// person may wear a character's skeleton with a mesh of their own.
bool same_mesh(const assets::ModelAsset& lhs, const assets::ModelAsset& rhs);
// The menu model's own part flags (S3: hidden, nearest-corner sort, env-mapped with its own page, part colours) in place of the field model's.
void menu_model_parts(const assets::ModelAsset& menu, render::MeshPipeline::DrawParams& dp);
// The menu model's joint matrices (menu units) for the field model's joint matrices `field_palette` (field units).
void menu_rig_palette(const MenuRig& rig, std::span<const render::Mat4> field_palette, std::vector<render::Mat4>& out);

}  // namespace oc::game
