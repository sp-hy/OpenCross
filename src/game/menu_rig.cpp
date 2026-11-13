#include "game/menu_rig.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#include "game/field_model_source.h"

namespace oc::game {

namespace {

// Matching cost weights: the subtree centre, a skipped menu joint (more when it carries mesh: a head or ponytail passed over for the empty
// helper below it), a subtree size difference (sizes counted up to kSubtreeCap), a partner beside the parent's partner instead of under it,
// a joint that carries mesh vertices paired with one that carries none.
constexpr float kCentreWeight = 2.0f, kSkipCost = 0.1f, kSkipMeshCost = 0.3f, kSizeCost = 0.05f, kSideCost = 0.1f, kSkinCost = 0.1f;
// A torso piece without a partner follows the field joint whose own mesh lies within this share of the height of its own mesh
// (follow_nearest_mesh); it must carry at least 1 / kPieceShare of the menu model's vertices.
constexpr float kPieceReach = 0.12f;
constexpr int kPieceShare = 50;
// A field joint whose clips move it further than this (model units) from its rest offset counts as translated; further than this share of
// the field joints' height and further than its own rest offset is long, it travels (a prop carried from the hip to the hand, feet pulled
// into the body).
constexpr float kMinTravel = 1.0f, kTravelShare = 0.1f;
constexpr int kSubtreeCap = 12;
// Bones shorter than this share of the height have no direction worth comparing.
constexpr float kMinBone = 0.03f;
constexpr float kDegrees = 180.0f / 3.14159265358979323846f;

using assets::Mat3;
using assets::Vec3;

Mat3 multiply(const Mat3& lhs, const Mat3& rhs) {
  Mat3 out{};
  for (size_t row = 0; row < 3; ++row)
    for (size_t col = 0; col < 3; ++col)
      for (size_t k = 0; k < 3; ++k) out[row * 3 + col] += lhs[row * 3 + k] * rhs[k * 3 + col];
  return out;
}

Mat3 transpose(const Mat3& matrix) {
  return {matrix[0], matrix[3], matrix[6], matrix[1], matrix[4], matrix[7], matrix[2], matrix[5], matrix[8]};
}

Vec3 rotate(const Mat3& matrix, const Vec3& v) {
  return {matrix[0] * v[0] + matrix[1] * v[1] + matrix[2] * v[2], matrix[3] * v[0] + matrix[4] * v[1] + matrix[5] * v[2],
          matrix[6] * v[0] + matrix[7] * v[1] + matrix[8] * v[2]};
}

float distance(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

// The rest pose in model space: rotation and position of every joint, and the joints' vertical extent.
struct RestPose {
  std::vector<Mat3> rot;
  std::vector<Vec3> pos;
  float height = 1.0f;
};

RestPose rest_pose(const assets::Skeleton& skeleton) {
  RestPose pose;
  const size_t count = skeleton.joints.size();
  pose.rot.resize(count);
  pose.pos.resize(count);
  float low = 0.0f, high = 0.0f;
  for (size_t j = 0; j < count; ++j) {
    const assets::Joint& joint = skeleton.joints[j];
    const Mat3 local = assets::rotation_zyx(joint.rest_rot_deg);
    if (joint.parent < 0) {
      pose.rot[j] = local;
      pose.pos[j] = joint.rest_trans;
    } else {
      const size_t parent = static_cast<size_t>(joint.parent);
      pose.rot[j] = multiply(pose.rot[parent], local);
      const Vec3 offset = rotate(pose.rot[parent], joint.rest_trans);
      for (size_t k = 0; k < 3; ++k) pose.pos[j][k] = pose.pos[parent][k] + offset[k];
    }
    low = j == 0 ? pose.pos[j][1] : std::min(low, pose.pos[j][1]);
    high = j == 0 ? pose.pos[j][1] : std::max(high, pose.pos[j][1]);
  }
  if (high - low > 0.0f) pose.height = high - low;
  return pose;
}

// What the matcher compares, in shares of the height: joint positions, subtree centres, depths, subtree sizes and whether mesh vertices are
// bound to the joint.
struct Shape {
  std::vector<Vec3> pos, centre, mesh_centre;
  std::vector<int> depth, size, parent, vertices;
  std::vector<bool> skinned;
};

Shape shape_of(const assets::ModelAsset& model, const RestPose& pose) {
  const size_t count = model.skeleton.joints.size();
  Shape shape;
  shape.pos.resize(count);
  shape.depth.assign(count, 0);
  shape.size.assign(count, 1);
  shape.parent.resize(count);
  shape.skinned.assign(count, false);
  for (size_t j = 0; j < count; ++j) {
    shape.parent[j] = model.skeleton.joints[j].parent;
    for (size_t k = 0; k < 3; ++k) shape.pos[j][k] = pose.pos[j][k] / pose.height;
    if (shape.parent[j] >= 0) shape.depth[j] = shape.depth[static_cast<size_t>(shape.parent[j])] + 1;
  }
  shape.centre = shape.pos;
  for (size_t j = count; j-- > 1;) {
    if (shape.parent[j] < 0) continue;
    const size_t parent = static_cast<size_t>(shape.parent[j]);
    shape.size[parent] += shape.size[j];
    for (size_t k = 0; k < 3; ++k) shape.centre[parent][k] += shape.centre[j][k];
  }
  for (size_t j = 0; j < count; ++j)
    for (size_t k = 0; k < 3; ++k) shape.centre[j][k] /= static_cast<float>(shape.size[j]);
  shape.mesh_centre = shape.pos;
  shape.vertices.assign(count, 0);
  std::vector<Vec3> sum(count, Vec3{});
  for (const assets::Mesh& mesh : model.meshes)
    for (const assets::SkinVertex& vertex : mesh.pool) {
      if (vertex.joint[0] >= count) continue;
      shape.skinned[vertex.joint[0]] = true;
      ++shape.vertices[vertex.joint[0]];
      for (size_t k = 0; k < 3; ++k) sum[vertex.joint[0]][k] += vertex.bind_pos[k] / pose.height;
    }
  for (size_t j = 0; j < count; ++j)
    if (shape.vertices[j] > 0)
      for (size_t k = 0; k < 3; ++k) shape.mesh_centre[j][k] = sum[j][k] / static_cast<float>(shape.vertices[j]);
  return shape;
}

bool descends_from(const Shape& shape, int joint, int ancestor) {
  for (int at = shape.parent[static_cast<size_t>(joint)]; at >= 0; at = shape.parent[static_cast<size_t>(at)])
    if (at == ancestor) return true;
  return false;
}

// Height of the body's top above the feet (y = 0) in the rest pose: the highest vertex of the head, its chain down to the root and what hangs
// from them (hair, a hat, a collar), not of the limbs: a joint under a paired joint off that chain (an arm, a leg, a prop held in a hand)
// does not count. 0 without such a vertex.
float body_top(const assets::ModelAsset& model, const Shape& shape, int head, const std::vector<bool>& paired) {
  const size_t count = shape.parent.size();
  if (head < 0 || static_cast<size_t>(head) >= count) return 0.0f;
  std::vector<bool> chain(count, false), body(count, false);
  for (int at = head; at >= 0; at = shape.parent[static_cast<size_t>(at)]) chain[static_cast<size_t>(at)] = true;
  for (size_t j = 0; j < count; ++j) {
    int at = static_cast<int>(j);
    while (at >= 0 && !chain[static_cast<size_t>(at)] && !(paired[static_cast<size_t>(at)] && !descends_from(shape, at, head)))
      at = shape.parent[static_cast<size_t>(at)];
    body[j] = at >= 0 && chain[static_cast<size_t>(at)];
  }
  float top = 0.0f;
  for (const assets::Mesh& mesh : model.meshes)
    for (const assets::SkinVertex& vertex : mesh.pool)
      if (vertex.joint[0] < count && body[vertex.joint[0]]) top = std::max(top, -vertex.bind_pos[1]);
  return top;
}

// A field joint paired with an empty menu helper whose nearest mesh-carrying ancestor (past other empty unpaired helpers) went unpaired drives
// that ancestor instead: Kid's head mesh hangs on the joint above the helper the field head was paired with, and would otherwise ride on the
// chest, turned with its twist while the field head turns back to face forward.
void lift_to_mesh(std::vector<int>& field_joint, const Shape& menu_shape) {
  for (size_t k = 0; k < field_joint.size(); ++k) {
    if (field_joint[k] < 0 || menu_shape.skinned[k]) continue;
    int above = menu_shape.parent[k];
    while (above >= 0 && field_joint[static_cast<size_t>(above)] < 0 && !menu_shape.skinned[static_cast<size_t>(above)])
      above = menu_shape.parent[static_cast<size_t>(above)];
    if (above < 0 || field_joint[static_cast<size_t>(above)] >= 0 || !menu_shape.skinned[static_cast<size_t>(above)]) continue;
    field_joint[static_cast<size_t>(above)] = field_joint[k];
    field_joint[k] = -1;
  }
}

// A piece hanging from the torso without a partner (a skirt or coat panel, a cape top, a helmet bubble: an unpaired menu joint with mesh whose
// nearest paired ancestor is the head or one of the head's ancestors) follows the field joint whose own mesh lies nearest to its own, when
// that is within kPieceReach: a skirt panel then swings with the thigh beside it instead of standing stiff on the pelvis. Pieces under a
// limb (a prop held in a hand, such as Korcha's oar) keep riding it.
void follow_nearest_mesh(std::vector<int>& field_joint, const Shape& field_shape, const Shape& menu_shape, int menu_head) {
  if (menu_head < 0) return;
  const size_t count = field_joint.size();
  std::vector<bool> torso(count, false);
  for (int at = menu_head; at >= 0; at = menu_shape.parent[static_cast<size_t>(at)]) torso[static_cast<size_t>(at)] = true;
  int total = 0;
  for (const int vertices : menu_shape.vertices) total += vertices;
  for (size_t j = 0; j < count; ++j) {
    if (field_joint[j] >= 0 || menu_shape.vertices[j] * kPieceShare < total) continue;
    int anchor = menu_shape.parent[j];
    while (anchor >= 0 && field_joint[static_cast<size_t>(anchor)] < 0) anchor = menu_shape.parent[static_cast<size_t>(anchor)];
    if (anchor < 0 || !torso[static_cast<size_t>(anchor)]) continue;
    int nearest = -1;
    float best = kPieceReach;
    for (size_t i = 0; i < field_shape.vertices.size(); ++i) {
      if (field_shape.vertices[i] == 0) continue;
      const float gap = distance(field_shape.mesh_centre[i], menu_shape.mesh_centre[j]);
      if (gap < best) best = gap, nearest = static_cast<int>(i);
    }
    if (nearest >= 0) field_joint[j] = nearest;
  }
}

// The field joint of every menu joint under one strategy (-1 = none).
std::vector<int> pair_joints(const Shape& field_shape, const Shape& menu_shape, MenuRigPlan plan, bool separate_branches) {
  const size_t field_count = field_shape.pos.size(), menu_count = menu_shape.pos.size();
  std::vector<int> partner(field_count, -1);
  std::vector<bool> taken(menu_count, false);
  partner[0] = 0;
  taken[0] = true;
  for (size_t i = 0; i < field_count; ++i) {
    if (partner[i] < 0) continue;
    std::vector<size_t> children;
    for (size_t child = i + 1; child < field_count; ++child)
      if (field_shape.parent[child] == static_cast<int>(i)) children.push_back(child);
    bool room_under = false;
    for (size_t k = 0; k < menu_count; ++k) room_under = room_under || (!taken[k] && descends_from(menu_shape, static_cast<int>(k), partner[i]));
    const bool beside = plan == MenuRigPlan::Beside || (plan == MenuRigPlan::BesideWhenFull && !room_under);
    const int anchor = beside && field_shape.parent[i] >= 0 ? partner[static_cast<size_t>(field_shape.parent[i])] : -1;
    std::vector<std::tuple<float, size_t, size_t>> pairs;
    for (size_t child : children)
      for (size_t k = 0; k < menu_count; ++k) {
        if (taken[k]) continue;
        const bool under = descends_from(menu_shape, static_cast<int>(k), partner[i]);
        if (!under && (anchor < 0 || !descends_from(menu_shape, static_cast<int>(k), anchor))) continue;
        const int base = under ? partner[i] : anchor;
        const int skipped = menu_shape.depth[k] - menu_shape.depth[static_cast<size_t>(base)] - 1;
        int skipped_mesh = 0;
        for (int at = menu_shape.parent[k]; at >= 0 && at != base; at = menu_shape.parent[static_cast<size_t>(at)])
          skipped_mesh += menu_shape.skinned[static_cast<size_t>(at)] ? 1 : 0;
        const int size_gap = std::abs(std::min(menu_shape.size[k] - 1, kSubtreeCap) - std::min(field_shape.size[child] - 1, kSubtreeCap));
        const float cost = distance(menu_shape.pos[k], field_shape.pos[child]) + kCentreWeight * distance(menu_shape.centre[k], field_shape.centre[child]) +
                           kSkipCost * static_cast<float>(skipped) + kSkipMeshCost * static_cast<float>(skipped_mesh) +
                           kSizeCost * static_cast<float>(size_gap) + (under ? 0.0f : kSideCost) +
                           (menu_shape.skinned[k] != field_shape.skinned[child] ? kSkinCost : 0.0f);
        pairs.emplace_back(cost, child, k);
      }
    std::ranges::sort(pairs);
    // With separate_branches (and always for BesideWhenFull) two children never share a branch: the joints a pair skips belong to that
    // pair's limb (a hip piece does not take a forearm deep in the spine's branch).
    std::vector<int> branches;
    for (const auto& [cost, child, k] : pairs) {
      if (partner[child] >= 0 || taken[k]) continue;
      if (separate_branches || plan == MenuRigPlan::BesideWhenFull) {
        const int base = descends_from(menu_shape, static_cast<int>(k), partner[i]) ? partner[i] : anchor;
        int branch = static_cast<int>(k);
        while (menu_shape.parent[static_cast<size_t>(branch)] != base) branch = menu_shape.parent[static_cast<size_t>(branch)];
        if (std::ranges::find(branches, branch) != branches.end()) continue;
        branches.push_back(branch);
      }
      partner[child] = static_cast<int>(k);
      taken[k] = true;
    }
  }
  std::vector<int> field_joint(menu_count, -1);
  for (size_t i = 0; i < field_count; ++i)
    if (partner[i] >= 0) field_joint[static_cast<size_t>(partner[i])] = static_cast<int>(i);
  return field_joint;
}

MenuRig build_rig(const assets::ModelAsset& field_model, const assets::ModelAsset& menu_model, MenuRigPlan plan, std::span<const s8> map,
                  bool separate_branches);

// The better of two matches: usable first, then fewer field joints left over (those with children first), then the bones that agree best.
bool better(const MenuRig& lhs, const MenuRig& rhs) {
  if (lhs.usable != rhs.usable) return lhs.usable;
  if (lhs.unmatched_inner != rhs.unmatched_inner) return lhs.unmatched_inner < rhs.unmatched_inner;
  if (lhs.unmatched != rhs.unmatched) return lhs.unmatched < rhs.unmatched;
  return std::tie(lhs.bone_p90, lhs.bone_max) < std::tie(rhs.bone_p90, rhs.bone_max);
}

}  // namespace

bool same_skeleton(const assets::Skeleton& lhs, const assets::Skeleton& rhs) {
  if (lhs.joints.size() != rhs.joints.size()) return false;
  for (size_t j = 0; j < lhs.joints.size(); ++j) {
    const assets::Joint& a = lhs.joints[j];
    const assets::Joint& b = rhs.joints[j];
    if (a.parent != b.parent || a.rest_rot_deg != b.rest_rot_deg || a.rest_trans != b.rest_trans) return false;
  }
  return true;
}

bool same_mesh(const assets::ModelAsset& lhs, const assets::ModelAsset& rhs) {
  if (lhs.meshes.size() != rhs.meshes.size()) return false;
  for (size_t k = 0; k < lhs.meshes.size(); ++k) {
    const auto& left = lhs.meshes[k].pool;
    const auto& right = rhs.meshes[k].pool;
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
      for (size_t axis = 0; axis < 3; ++axis)
        if (std::fabs(left[i].bind_pos[axis] - right[i].bind_pos[axis]) > 0.5f) return false;
  }
  return true;
}

MenuRig match_menu_rig(const assets::ModelAsset& field_model, const assets::ModelAsset& menu_model) {
  // Siblings in separate branches first; sharing branches only when no strategy gives a usable rig that way.
  MenuRig best;
  for (const bool separate : {true, false}) {
    for (const MenuRigPlan plan : {MenuRigPlan::Under, MenuRigPlan::Beside, MenuRigPlan::BesideWhenFull}) {
      MenuRig other = build_rig(field_model, menu_model, plan, {}, separate);
      if (best.field_joint.empty() || better(other, best)) best = std::move(other);
    }
    if (best.usable) break;
  }
  return best;
}

MenuRig match_menu_rig(const assets::ModelAsset& field_model, const assets::ModelAsset& menu_model, MenuRigPlan plan) {
  return build_rig(field_model, menu_model, plan, {}, true);
}

MenuRig match_menu_rig(const assets::ModelAsset& field_model, const assets::ModelAsset& menu_model, std::span<const s8> map) {
  return build_rig(field_model, menu_model, MenuRigPlan::Map, map, true);
}

namespace {

// A rig from the matcher's pairing under `plan`, or from `map` (plan Map): usable when the map fits the two skeletons.
MenuRig build_rig(const assets::ModelAsset& field_model, const assets::ModelAsset& menu_model, MenuRigPlan plan, std::span<const s8> map,
                  bool separate_branches) {
  const assets::Skeleton& field = field_model.skeleton;
  const assets::Skeleton& menu = menu_model.skeleton;
  MenuRig rig;
  rig.plan = plan;
  const size_t field_count = field.joints.size(), menu_count = menu.joints.size();
  rig.field_joint.assign(menu_count, -1);
  if (field_count == 0 || menu_count == 0 || field.joints[0].parent >= 0 || menu.joints[0].parent >= 0) return rig;
  for (size_t j = 1; j < menu_count; ++j)
    if (menu.joints[j].parent < 0) return rig;
  const RestPose field_pose = rest_pose(field), menu_pose = rest_pose(menu);
  const Shape field_shape = shape_of(field_model, field_pose), menu_shape = shape_of(menu_model, menu_pose);
  bool map_fits = plan == MenuRigPlan::Map && map.size() == menu_count;
  if (plan == MenuRigPlan::Map) {
    for (size_t j = 0; map_fits && j < menu_count; ++j) {
      map_fits = map[j] >= -1 && map[j] < static_cast<int>(field_count);
      rig.field_joint[j] = map[j];
    }
    if (!map_fits) return rig;
  } else {
    rig.field_joint = pair_joints(field_shape, menu_shape, plan, separate_branches);
    lift_to_mesh(rig.field_joint, menu_shape);
    if (field_model.attach.points.size() > 1) {
      const int head = field_model.attach.points[1].joint;
      int menu_head = -1;
      for (size_t k = 0; k < menu_count && menu_head < 0; ++k)
        if (rig.field_joint[k] == head) menu_head = static_cast<int>(k);
      follow_nearest_mesh(rig.field_joint, field_shape, menu_shape, menu_head);
    }
  }
  // A field joint's partner for the quality figures: the first menu joint that follows it (a map may give it several).
  std::vector<int> partner(field_count, -1);
  for (size_t k = 0; k < menu_count; ++k)
    if (rig.field_joint[k] >= 0 && partner[static_cast<size_t>(rig.field_joint[k])] < 0) partner[static_cast<size_t>(rig.field_joint[k])] = static_cast<int>(k);

  std::vector<float> angles;
  float offset_sum = 0.0f;
  int matched = 0;
  for (size_t i = 0; i < field_count; ++i) {
    if (partner[i] < 0) {
      ++rig.unmatched;
      if (field_shape.size[i] > 1) ++rig.unmatched_inner;
      continue;
    }
    const size_t k = static_cast<size_t>(partner[i]);
    offset_sum += distance(field_shape.pos[i], menu_shape.pos[k]);
    ++matched;
    const int parent = field_shape.parent[i];
    if (parent < 0 || partner[static_cast<size_t>(parent)] < 0) continue;
    const size_t menu_parent = static_cast<size_t>(partner[static_cast<size_t>(parent)]);
    Vec3 field_bone, menu_bone;
    for (size_t axis = 0; axis < 3; ++axis) {
      field_bone[axis] = field_shape.pos[i][axis] - field_shape.pos[static_cast<size_t>(parent)][axis];
      menu_bone[axis] = menu_shape.pos[k][axis] - menu_shape.pos[menu_parent][axis];
    }
    const float field_length = std::hypot(field_bone[0], field_bone[1], field_bone[2]);
    const float menu_length = std::hypot(menu_bone[0], menu_bone[1], menu_bone[2]);
    if (field_length < kMinBone || menu_length < kMinBone) continue;
    const float cosine = (field_bone[0] * menu_bone[0] + field_bone[1] * menu_bone[1] + field_bone[2] * menu_bone[2]) / (field_length * menu_length);
    angles.push_back(std::acos(std::clamp(cosine, -1.0f, 1.0f)) * kDegrees);
  }
  if (!angles.empty()) {
    std::ranges::sort(angles);
    rig.bone_max = angles.back();
    rig.bone_p90 = angles[(angles.size() - 1) * 9 / 10];
  }
  rig.mean_offset = matched > 0 ? offset_sum / static_cast<float>(matched) : 0.0f;

  rig.field_rest = field_pose.rot;
  rig.menu_rest = menu_pose.rot;
  // How each paired menu joint is placed (see the top of menu_rig.h): from the field skeleton when its field joint travels and hangs below
  // the field joint driving the menu joint's anchor (its nearest paired ancestor); swung about the nearest shared joint when its field joint
  // hangs below that driver through joints of its own; its own offset plus the field move when its field joint is otherwise translated.
  std::vector<float> travel(field_count, 0.0f);
  for (const assets::AnimationClip& clip : field_model.clips)
    for (size_t i = 1; i < field_count && i < clip.tracks.size(); ++i)
      for (const assets::Key3& key : clip.tracks[i].trans)
        travel[i] = std::max(travel[i], distance(key.v, field.joints[i].rest_trans));
  rig.placed_from.assign(menu_count, -1);
  rig.anchor.assign(menu_count, -1);
  rig.swing_about.assign(menu_count, -1);
  rig.moved.assign(menu_count, false);
  rig.field_parent = field_shape.parent;
  rig.field_offset.resize(field_count);
  for (size_t i = 0; i < field_count; ++i) rig.field_offset[i] = field.joints[i].rest_trans;
  rig.menu_rest_pos = menu_pose.pos;
  std::vector<int> followers(field_count, 0);
  for (const int source : rig.field_joint)
    if (source >= 0) ++followers[static_cast<size_t>(source)];
  for (size_t j = 1; j < menu_count; ++j) {
    const int source = rig.field_joint[j];
    int anchor = menu_shape.parent[j];
    while (anchor >= 0 && rig.field_joint[static_cast<size_t>(anchor)] < 0) anchor = menu_shape.parent[static_cast<size_t>(anchor)];
    const int driver = anchor >= 0 ? rig.field_joint[static_cast<size_t>(anchor)] : -1;
    if (source < 0 || driver < 0 || source == driver) continue;
    const bool below = descends_from(field_shape, source, driver);
    const float reach = travel[static_cast<size_t>(source)];
    const bool travels = reach > kTravelShare * field_pose.height && reach > std::hypot(field.joints[static_cast<size_t>(source)].rest_trans[0],
                                                                                         field.joints[static_cast<size_t>(source)].rest_trans[1],
                                                                                         field.joints[static_cast<size_t>(source)].rest_trans[2]);
    if (below && travels && followers[static_cast<size_t>(source)] == 1) {
      rig.placed_from[j] = driver;
      rig.anchor[j] = anchor;
    } else if (below && !travels && field_shape.parent[static_cast<size_t>(source)] != driver) {
      int pivot = anchor;
      for (int at = field_shape.parent[static_cast<size_t>(source)]; at >= 0 && at != driver && pivot == anchor; at = field_shape.parent[static_cast<size_t>(at)])
        if (partner[static_cast<size_t>(at)] >= 0 && partner[static_cast<size_t>(at)] < static_cast<int>(j)) pivot = partner[static_cast<size_t>(at)];
      rig.swing_about[j] = pivot;
    } else if (travel[static_cast<size_t>(source)] > kMinTravel) {
      rig.moved[j] = true;
    }
  }
  rig.menu_local.resize(menu_count);
  rig.menu_offset.resize(menu_count);
  rig.menu_parent = menu_shape.parent;
  // A menu joint without a field partner is placed on its parent as the menu model's first pose (clip s4 0, the Status screen's) has it:
  // a held object (Marcy's shield) rests at the model origin and only the clips put it in the hand.
  const assets::AnimationClip* pose = nullptr;
  for (const assets::AnimationClip& clip : menu_model.clips)
    if (!pose && clip.source == "s4" && clip.index == 0 && clip.frame_count > 0 && clip.tracks.size() == menu_count) pose = &clip;
  for (size_t j = 0; j < menu_count; ++j) {
    rig.menu_local[j] = assets::rotation_zyx(menu.joints[j].rest_rot_deg);
    rig.menu_offset[j] = menu.joints[j].rest_trans;
    if (!pose || rig.field_joint[j] >= 0) continue;
    if (!pose->tracks[j].rot.empty()) rig.menu_local[j] = assets::rotation_zyx(pose->tracks[j].rot[0].v);
    if (!pose->tracks[j].trans.empty()) rig.menu_offset[j] = pose->tracks[j].trans[0].v;
  }
  rig.field_root_rest = field_pose.pos[0];
  // As tall as the field model, feet to the top of the body (body_top; the head is the head turn's joint, attach point 1, and the menu joint
  // that follows it).
  rig.scale = kBattleModelFieldScale;
  if (field_model.attach.points.size() > 1) {
    const int head = field_model.attach.points[1].joint;
    const int menu_head = head >= 0 && static_cast<size_t>(head) < field_count ? partner[static_cast<size_t>(head)] : -1;
    std::vector<bool> field_paired(field_count), menu_paired(menu_count);
    for (size_t i = 0; i < field_count; ++i) field_paired[i] = partner[i] >= 0;
    for (size_t k = 0; k < menu_count; ++k) menu_paired[k] = rig.field_joint[k] >= 0;
    const float field_top = body_top(field_model, field_shape, head, field_paired);
    const float menu_top = body_top(menu_model, menu_shape, menu_head, menu_paired);
    if (field_top > 0.0f && menu_top > 0.0f) rig.scale = field_top / menu_top;
  }
  // A placed piece lines up by the centre of its own mesh, not its joint: the two rigs need not hang a prop's mesh the same way from its
  // joint (Orcha's cleaver blade hangs further below the menu joint than below the field one).
  rig.mesh_shift.assign(menu_count, Vec3{});
  for (size_t j = 0; j < menu_count; ++j) {
    const int source = rig.field_joint[j];
    if (rig.placed_from[j] < 0 || source < 0 || !menu_shape.skinned[j] || !field_shape.skinned[static_cast<size_t>(source)]) continue;
    for (size_t axis = 0; axis < 3; ++axis) {
      const float field_hang = (field_shape.mesh_centre[static_cast<size_t>(source)][axis] - field_shape.pos[static_cast<size_t>(source)][axis]) * field_pose.height;
      const float menu_hang = (menu_shape.mesh_centre[j][axis] - menu_shape.pos[j][axis]) * menu_pose.height;
      rig.mesh_shift[j][axis] = field_hang / rig.scale - menu_hang;
    }
  }
  const float field_pelvis = -field_pose.pos[0][1], menu_pelvis = -menu_pose.pos[0][1];
  rig.root_motion = field_pelvis > 0.0f && menu_pelvis > 0.0f ? menu_pelvis / field_pelvis : 1.0f / rig.scale;
  rig.usable = plan == MenuRigPlan::Map ? map_fits : rig.unmatched_inner == 0 && rig.bone_p90 <= kMaxBoneAngleP90 && rig.bone_max <= kMaxBoneAngle;
  return rig;
}

}  // namespace

void menu_model_parts(const assets::ModelAsset& menu, render::MeshPipeline::DrawParams& dp) {
  dp.hidden_meshes = dp.near_meshes = dp.env_meshes = dp.part_mask = 0;
  for (size_t k = 0; k < menu.shading.size(); ++k) {
    const assets::ShadingEntry& entry = menu.shading[k];
    if (entry.flags & 1) dp.hidden_meshes |= 1u << k;
    if (entry.flags & 0x10) dp.near_meshes |= 1u << k;
    if ((entry.flags & 0x80) && (entry.flags & 0x60)) dp.env_meshes |= 1u << k;
    if (entry.r != 0x80 || entry.g != 0x80 || entry.b != 0x80) {
      dp.part_mask |= 1u << k;
      dp.part_tint[k][0] = entry.r / 128.0f, dp.part_tint[k][1] = entry.g / 128.0f, dp.part_tint[k][2] = entry.b / 128.0f;
    }
  }
}

void menu_rig_palette(const MenuRig& rig, std::span<const render::Mat4> field_palette, std::vector<render::Mat4>& out) {
  const size_t count = rig.field_joint.size();
  out.resize(count);
  std::vector<Mat3> turned(count);
  std::vector<Vec3> placed(count);
  auto rotation_of = [](const render::Mat4& matrix) {
    return Mat3{matrix.at(0, 0), matrix.at(0, 1), matrix.at(0, 2), matrix.at(1, 0), matrix.at(1, 1), matrix.at(1, 2), matrix.at(2, 0), matrix.at(2, 1), matrix.at(2, 2)};
  };
  for (size_t j = 0; j < count; ++j) {
    const int parent = rig.menu_parent[j];
    const int source = rig.field_joint[j];
    if (source >= 0 && static_cast<size_t>(source) < field_palette.size()) {
      const size_t field_index = static_cast<size_t>(source);
      turned[j] = multiply(multiply(rotation_of(field_palette[field_index]), transpose(rig.field_rest[field_index])), rig.menu_rest[j]);
    } else {
      turned[j] = parent >= 0 ? multiply(turned[static_cast<size_t>(parent)], rig.menu_local[j]) : rig.menu_local[j];
    }
    const int from = j < rig.placed_from.size() ? rig.placed_from[j] : -1;
    const int swing = j < rig.swing_about.size() ? rig.swing_about[j] : -1;
    const bool field_ok = source >= 0 && static_cast<size_t>(source) < field_palette.size();
    if (parent >= 0 && from >= 0 && field_ok && static_cast<size_t>(from) < field_palette.size()) {
      const size_t anchor = static_cast<size_t>(rig.anchor[j]);
      const Mat3 turn = multiply(rotation_of(field_palette[static_cast<size_t>(source)]), transpose(rig.field_rest[static_cast<size_t>(source)]));
      const Vec3 shift = rotate(turn, rig.mesh_shift[j]);
      for (size_t axis = 0; axis < 3; ++axis) {
        const int row = static_cast<int>(axis);
        placed[j][axis] = placed[anchor][axis] + shift[axis] +
                          (field_palette[static_cast<size_t>(source)].at(row, 3) - field_palette[static_cast<size_t>(from)].at(row, 3)) / rig.scale;
      }
    } else if (parent >= 0 && swing >= 0 && field_ok) {
      const size_t pivot = static_cast<size_t>(swing);
      const Mat3 turn = multiply(rotation_of(field_palette[static_cast<size_t>(source)]), transpose(rig.field_rest[static_cast<size_t>(source)]));
      Vec3 arm;
      for (size_t axis = 0; axis < 3; ++axis) arm[axis] = rig.menu_rest_pos[j][axis] - rig.menu_rest_pos[pivot][axis];
      const Vec3 swung = rotate(turn, arm);
      for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] = placed[pivot][axis] + swung[axis];
    } else if (parent >= 0) {
      const Vec3 offset = rotate(turned[static_cast<size_t>(parent)], rig.menu_offset[j]);
      for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] = placed[static_cast<size_t>(parent)][axis] + offset[axis];
      const int field_parent = j < rig.moved.size() && rig.moved[j] ? rig.field_parent[static_cast<size_t>(source)] : -1;
      if (field_parent >= 0 && static_cast<size_t>(field_parent) < field_palette.size() && static_cast<size_t>(source) < field_palette.size()) {
        const render::Mat4& above = field_palette[static_cast<size_t>(field_parent)];
        const Vec3 rest = rotate(rotation_of(above), rig.field_offset[static_cast<size_t>(source)]);
        for (size_t axis = 0; axis < 3; ++axis) {
          const int row = static_cast<int>(axis);
          placed[j][axis] += (field_palette[static_cast<size_t>(source)].at(row, 3) - above.at(row, 3) - rest[axis]) / rig.scale;
        }
      }
    } else {
      // About the field root's place: the two roots need not sit at the same point of the body (Poshul's is near his chest on the field rig).
      Vec3 pivot;
      for (size_t axis = 0; axis < 3; ++axis) pivot[axis] = rig.field_root_rest[axis] / rig.scale;
      Vec3 arm;
      for (size_t axis = 0; axis < 3; ++axis) arm[axis] = rig.menu_offset[j][axis] - pivot[axis];
      const Mat3 turn = source >= 0 && static_cast<size_t>(source) < field_palette.size()
                            ? multiply(rotation_of(field_palette[static_cast<size_t>(source)]), transpose(rig.field_rest[static_cast<size_t>(source)]))
                            : Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
      const Vec3 swung = rotate(turn, arm);
      for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] = pivot[axis] + swung[axis];
      if (!field_palette.empty())
        for (size_t axis = 0; axis < 3; ++axis)
          placed[j][axis] += (field_palette[0].at(static_cast<int>(axis), 3) - rig.field_root_rest[axis]) * rig.root_motion;
    }
    out[j] = render::mat4_from_rt(turned[j], placed[j]);
  }
}

namespace {

// Hand-made pairings, for the characters whose two skeletons the matcher cannot pair (one rig's pelvis or head sits a joint higher, limbs
// carry other helper joints, props hang off other joints) or pairs wrongly in a way no general rule catches (a hand left unpaired under a
// veil, a coat panel taking a cane, panels paired on one side only). -1: the menu joint rides on its parent (hair and other pieces the field rig has no
// joint for). Field joints that share one point (Marcy's skirt flaps, Razzly's wings) are told apart by where their vertices lie.
// Riddel: field joint per menu joint.
constexpr s8 kRiddelMap[] = {0, 1, 2, 3, -1, -1, -1, -1, -1, -1, -1, -1, 5, 6, 7, 7, 8, 9, 10, 11, -1, 12, 13, 14, 15, -1, 23, 24, 25, 26, 29, 28, 16, 17, 18, 19, 20, 21};
// Marcy: field joint per menu joint.
constexpr s8 kMarcyMap[] = {0, 1, 2, 3, -1, -1, 5, 4, 6, 7, 8, 9, -1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 25, 25, 22, 22, 23, 24, 22, 22};
// Razzly: field joint per menu joint.
constexpr s8 kRazzlyMap[] = {0, 1, 2, 3, -1, -1, -1, 11, 12, 13, 14, 7, 8, 9, 10, 4, 5, 6, 6, 15, 16, 17, 18, -1, 19, 20, 21, 22, -1, -1, 26, 24, 25, 23, 23};
// Harle: field joint per menu joint.
constexpr s8 kHarleMap[] = {0, 1, 2, 3, -1, -1, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15, 16, -1, -1, 16, -1, -1, -1, -1, 17, 18, 19, 20, -1, 21, 22, 23, 24, -1};
// Janice: field joint per menu joint.
constexpr s8 kJaniceMap[] = {0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, -1, 10, 13, 14, 15, 16, 17, -1, 15, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
// NeoFio: field joint per menu joint.
constexpr s8 kNeoFioMap[] = {0, 1, 2, 3, 6, 5, -1, -1, -1, -1, 4, 7, 8, 9, 10, -1, -1, -1, -1, -1, -1, 11, 12, 13, 14, -1, -1, -1, -1, -1, -1, 15, 16, 17, 18, 19, 20, 21, 22};
// Irenes: field joint per menu joint.
constexpr s8 kIrenesMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, -1, -1, -1, 8, 9, 10, 11, -1, -1, -1, -1, 12, 3, -1, -1, -1, -1, 13, 15, 16, 17, 18, 19, 20, 21, 23, 22};
// Leah: field joint per menu joint.
constexpr s8 kLeahMap[] = {0, 1, 2, -1, 3, 12, -1, 4, 5, 6, 7, 8, 9, 10, 11, -1, 17, 18, 19, 20, 13, 14, 15, 16, 21, 22, 23, 23, 23, -1, -1, -1, -1, -1};
// Van: field joint per menu joint.
constexpr s8 kVanMap[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, 8, 9, 10, 11, -1, 21, -1, 12, 13, 14, 15, -1, 16, 17, 18, 19, -1, 24, 20, 22, 23, -1, -1, 23, 22};
// Sneff: field joint per menu joint.
constexpr s8 kSneffMap[] = {0, 1, 2, 3, -1, -1, 5, 6, 7, 8, -1, -1, 9, 10, 11, 12, -1, -1, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};

// Leena: field joint per menu joint.
constexpr s8 kLeenaMap[] = {0, 1, 2, 3, -1, -1, 5, 6, 4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 23, 24, 25, 26, 27, 27, 27, 20, 20, 20, 22, 21, -1, 29, 28, -1};
// Turnip: field joint per menu joint.
constexpr s8 kTurnipMap[] = {0, 1, 2, 3, 3, 4, -1, -1, -1, -1, 17, 18, 15, 16, 13, 14, 15, 16, 17, 13, 5, 6, 7, 8, 9, 10, 11, 12, -1, 27, 19, 20, 21, 22, -1, 23, 24, 25, 26, -1};
// Pierre: field joint per menu joint.
constexpr s8 kPierreMap[] = {0, 1, 2, 3, -1, -1, -1, 5, -1, -1, 4, -1, 7, 6, 12, 13, 14, 15, 8, 9, 10, 11, -1, -1, 16, 17, 18, 19, 20, 21, 22, 23, 26, 27, 24, 25, -1, -1, -1};

// Glenn (both field models share one skeleton, both menu models too): field joint per menu joint.
constexpr s8 kGlennMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 12, 13, 14, 15, -1, 8, 9, 10, 11, -1, 23, 24, 25, 26, 19, 20, 21, 22, 16, 17};

// Radius: field joint per menu joint.
constexpr s8 kRadiusMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 17, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1};
// Fargo: field joint per menu joint.
constexpr s8 kFargoMap[] = {0, 1, 2, 3, -1, -1, 4, -1, 5, 6, 7, 8, 9, 10, 11, 12, -1, -1, -1, -1, -1, 13, 14, 15, 16, 17, 18, 19, 20, -1};
// Macha: field joint per menu joint.
constexpr s8 kMachaMap[] = {0, 1, 2, 3, -1, -1, 8, 9, 10, 11, 4, 5, 6, 7, -1, 12, 13, 14, 15, 16, 17, 18, 19, 21, 21, 20, 20, -1, -1, -1, -1, 21, -1, -1, 21, 21};
// Miki: field joint per menu joint.
constexpr s8 kMikiMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, -1, -1, 9, 10, 11, 12, 13, 14, -1, 15, 16, 17, 18, 19, 20, -1, 21, 22, 23, 24, 25, 26, 27, 28};
// Greco: field joint per menu joint.
constexpr s8 kGrecoMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 13, 14, 17, 18, 22, 23, 20, 21, -1, -1, -1, -1};
// Mel: field joint per menu joint.
constexpr s8 kMelMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 19, 15, 19, 15};
// Steena: field joint per menu joint.
constexpr s8 kSteenaMap[] = {0, 1, 2, -1, -1, -1, 3, -1, -1, -1, 4, 5, 6, 11, 12, 13, 14, 7, 8, 9, 10, 20, 21, 22, 23, 24, 15, 16, 17, 18, 19};
// Doc: field joint per menu joint.
constexpr s8 kDocMap[] = {0, 1, 2, 3, 4, 6, 8, 9, 10, 11, -1, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 25, 24, 24, 21, 17, 26, 24, 24, 24, 24};
// Pip's flying form (tu_mm): field joint per menu joint.
constexpr s8 kTuMmMap[] = {0, 1, 2, 3, 4, 5, 18, -1, -1, -1, 16, 17, 6, 7, 8, 11, 12, 13, 14, 15, 10, 9, 20, 20, -1, 19, 19, -1};
// Lynx's body (worn by Serge): field joint per menu joint.
constexpr s8 kLynxMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1, 13, 14, 13, 14, 15, 16, 17, 18, -1, -1, -1, -1, 19, 20, 21, 22, -1, -1, -1, -1, 24, 25, 23, 26};

struct RigMap {
  int model_byte;
  std::span<const s8> map;
};
constexpr RigMap kRigMaps[] = {
    {0x06, kRiddelMap},
    {0x09, kMarcyMap},
    {0x0D, kRazzlyMap},
    {0x10, kRadiusMap},
    {0x11, kFargoMap},
    {0x12, kMachaMap},
    {0x13, kGlennMap},
    {0x14, kLeenaMap},
    {0x15, kMikiMap},
    {0x16, kHarleMap},
    {0x17, kJaniceMap},
    {0x1C, kTurnipMap},
    {0x1D, kNeoFioMap},
    {0x1E, kGrecoMap},
    {0x21, kIrenesMap},
    {0x22, kMelMap},
    {0x23, kLeahMap},
    {0x24, kVanMap},
    {0x25, kSneffMap},
    {0x26, kSteenaMap},
    {0x27, kDocMap},
    {0x29, kPierreMap},
    {0x30, kTuMmMap},
    {0x31, kLynxMap},
    {0x32, kGlennMap},
};

}  // namespace

std::span<const s8> menu_rig_map(int model_byte) {
  for (const RigMap& entry : kRigMaps)
    if (entry.model_byte == model_byte) return entry.map;
  return {};
}

}  // namespace oc::game
