#include "game/menu_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
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
// A leading joint passes between its two field joints' turns over this share of its piece's arm.
constexpr float kLeadBlend = 0.25f;
// An outer carried joint passes between its carried place and its parent's place over this share of its rest arm from the carrier.
constexpr float kOuterBlend = 0.25f;
// A field joint with fewer vertices than this has no face of its own: no piece a menu piece could be laid onto.
constexpr int kMinPieceVertices = 3;
constexpr int kSubtreeCap = 12;
// Bones shorter than this share of the height have no direction worth comparing.
constexpr float kMinBone = 0.03f;
constexpr float kDegrees = 180.0f / 3.14159265358979323846f;

// Props modelled into a body joint's piece (match_body_rig splits them off onto a joint of their own, the body's last, for its map to
// hide or pair): Orcha's wok, on its shoulder strap's joint; Pierre's shield, on his left forearm's joint.
struct HiddenShell {
  int body;
  int joint;
};
constexpr HiddenShell kHiddenShells[] = {
    {0x0F, 13},
    {0x29, 16},
};

// Development: OC_RIG_OVERRIDE names a text file whose lines stand in for the tables' entries of a detailed body (battle model number in
// hex), so a pairing can be tried without a rebuild. One entry a line:
//   map <body> <field joint per menu joint: a number, -1 = rides its parent, H = not drawn> | map <body> auto (the automatic match)
//   tuning <body> <align_limbs> <field_body> <on_field x 4> <hang x 2> <stance> [<keep> up to 8] | tuning <body> none
//   carry <body> [<menu joint> <carrier>]... (up to 4 pairs) | lay <body> <menu joint>... (up to 6) | posture <body> <0/1>
//   rest <body> [<menu joint> <turn x y z, degrees> <offset x y z>]... (up to 4) | floor <body> <menu joint>... (up to 4)
//   lead <body> [<menu joint> <field joint> <field joint>]... (up to 4) | outer <body> <menu joint>... (up to 4)
//   aim <body> <menu joint>... (up to 4)
//     (all of these are laid over the body's tuning, the file's or the table's)
//   props <body> [<field joint> <menu joint x 4>]...   (no numbers: no held prop)
//   shells <body> [<menu joint>]...                    (no numbers: none)
struct RigOverrides {
  std::map<int, std::vector<s8>> maps;
  std::map<int, std::vector<s8>> carry, lay, floor, lead, outer, aim;
  std::map<int, std::vector<int>> rest;
  std::map<int, bool> posture;
  std::map<int, RigTuning> tunings;
  std::map<int, bool> untuned;
  std::map<int, std::vector<HeldProp>> props;
  std::map<int, std::vector<HiddenShell>> shells;
};

const RigOverrides& rig_overrides() {
  static const RigOverrides loaded = [] {
    RigOverrides out;
    const char* path = std::getenv("OC_RIG_OVERRIDE");
    if (!path) return out;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
      std::istringstream words(line);
      std::string kind, body_text;
      if (!(words >> kind >> body_text) || kind[0] == '#') continue;
      const int body = static_cast<int>(std::strtol(body_text.c_str(), nullptr, 16));
      std::vector<std::string> rest;
      for (std::string word; words >> word;) rest.push_back(word);
      auto number = [&](size_t index) { return index < rest.size() ? static_cast<int>(std::strtol(rest[index].c_str(), nullptr, 10)) : -1; };
      if (kind == "map") {
        std::vector<s8>& map = out.maps[body];
        for (size_t i = 0; i < rest.size() && rest[0] != "auto"; ++i) map.push_back(rest[i] == "H" ? kHiddenJoint : static_cast<s8>(number(i)));
      } else if (kind == "tuning") {
        out.untuned[body] = !rest.empty() && rest[0] == "none";
        if (!out.untuned[body])
          out.tunings[body] = RigTuning{body, number(0) > 0, number(1) > 0,
                                        {static_cast<s8>(number(2)), static_cast<s8>(number(3)), static_cast<s8>(number(4)), static_cast<s8>(number(5))},
                                        {static_cast<s8>(number(6)), static_cast<s8>(number(7))}, number(8) > 0,
                                        {static_cast<s8>(number(9)), static_cast<s8>(number(10)), static_cast<s8>(number(11)), static_cast<s8>(number(12)),
                                         static_cast<s8>(number(13)), static_cast<s8>(number(14)), static_cast<s8>(number(15)), static_cast<s8>(number(16))}};
      } else if (kind == "aim" || kind == "carry" || kind == "lay" || kind == "floor" || kind == "lead" || kind == "outer") {
        std::vector<s8>& joints = kind == "aim"     ? out.aim[body]
                                  : kind == "carry" ? out.carry[body]
                                  : kind == "lay"   ? out.lay[body]
                                  : kind == "floor" ? out.floor[body]
                                  : kind == "lead"  ? out.lead[body]
                                                    : out.outer[body];
        for (size_t i = 0; i < rest.size(); ++i) joints.push_back(static_cast<s8>(number(i)));
      } else if (kind == "rest") {
        for (size_t i = 0; i < rest.size(); ++i) out.rest[body].push_back(number(i));
      } else if (kind == "posture") {
        out.posture[body] = number(0) > 0;
      } else if (kind == "props") {
        std::vector<HeldProp>& props = out.props[body];
        for (size_t i = 0; i + 4 < rest.size(); i += 5)
          props.push_back({body, number(i), {static_cast<s8>(number(i + 1)), static_cast<s8>(number(i + 2)), static_cast<s8>(number(i + 3)), static_cast<s8>(number(i + 4))}});
      } else if (kind == "shells") {
        std::vector<HiddenShell>& shells = out.shells[body];
        for (size_t i = 0; i < rest.size(); ++i) shells.push_back({body, number(i)});
      }
    }
    return out;
  }();
  return loaded;
}

// The joints of detailed body `body` whose free shells are props the field model does not carry.
std::vector<HiddenShell> hidden_shells(int body) {
  if (const auto found = rig_overrides().shells.find(body); found != rig_overrides().shells.end()) return found->second;
  std::vector<HiddenShell> out;
  for (const HiddenShell& shell : kHiddenShells)
    if (shell.body == body) out.push_back(shell);
  return out;
}

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

// The field model's head: the joint of attach point 1 (the head turn's). A room's own model carries no attach points (all name the root):
// then the joint whose mesh lies highest in the rest pose. -1 without either.
int head_joint(const assets::ModelAsset& model, const Shape& shape) {
  if (model.attach.points.size() > 1 && model.attach.points[1].joint > 0 && static_cast<size_t>(model.attach.points[1].joint) < shape.parent.size())
    return model.attach.points[1].joint;
  int head = -1;
  for (size_t j = 0; j < shape.parent.size(); ++j)
    if (shape.vertices[j] > 0 && (head < 0 || shape.mesh_centre[j][1] < shape.mesh_centre[static_cast<size_t>(head)][1])) head = static_cast<int>(j);
  return head;
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
// limb (a prop held in a hand, such as Marcy's shield) keep riding it.
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
// A rigid piece's rest-pose points: their centre and principal axes, the columns of `axes`, longest first, each of the first two pointing to
// the side the points reach furthest (the sign of the third moment along it: a rifle's stock against its barrel), the third completing a
// right-handed frame. Not valid for fewer than three points.
struct PieceFrame {
  Vec3 centre{};
  Mat3 axes{1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool valid = false;
};

PieceFrame piece_frame(const std::vector<Vec3>& points) {
  PieceFrame frame;
  if (points.size() < 3) return frame;
  for (const Vec3& point : points)
    for (size_t axis = 0; axis < 3; ++axis) frame.centre[axis] += point[axis] / static_cast<float>(points.size());
  // Covariance, then its eigenvectors by cyclic Jacobi rotations (symmetric 3 x 3).
  double cov[3][3] = {};
  for (const Vec3& point : points)
    for (size_t row = 0; row < 3; ++row)
      for (size_t col = 0; col < 3; ++col) cov[row][col] += (point[row] - frame.centre[row]) * (point[col] - frame.centre[col]);
  double vec[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 32; ++sweep)
    for (size_t i = 0; i < 2; ++i)
      for (size_t j = i + 1; j < 3; ++j) {
        if (std::fabs(cov[i][j]) < 1e-9) continue;
        const double theta = (cov[j][j] - cov[i][i]) / (2.0 * cov[i][j]);
        const double tangent = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double cosine = 1.0 / std::sqrt(tangent * tangent + 1.0), sine = tangent * cosine;
        for (size_t k = 0; k < 3; ++k) {
          const double left = cov[k][i], right = cov[k][j];
          cov[k][i] = cosine * left - sine * right;
          cov[k][j] = sine * left + cosine * right;
        }
        for (size_t k = 0; k < 3; ++k) {
          const double upper = cov[i][k], lower = cov[j][k];
          cov[i][k] = cosine * upper - sine * lower;
          cov[j][k] = sine * upper + cosine * lower;
        }
        for (size_t k = 0; k < 3; ++k) {
          const double left = vec[k][i], right = vec[k][j];
          vec[k][i] = cosine * left - sine * right;
          vec[k][j] = sine * left + cosine * right;
        }
      }
  std::array<size_t, 3> order{0, 1, 2};
  std::ranges::sort(order, [&](size_t lhs, size_t rhs) { return cov[lhs][lhs] > cov[rhs][rhs]; });
  Vec3 axis_of[3];
  for (size_t i = 0; i < 2; ++i) {
    double third = 0;
    for (const Vec3& point : points) {
      double along = 0;
      for (size_t k = 0; k < 3; ++k) along += (point[k] - frame.centre[k]) * vec[k][order[i]];
      third += along * along * along;
    }
    for (size_t k = 0; k < 3; ++k) axis_of[i][k] = static_cast<float>(third < 0 ? -vec[k][order[i]] : vec[k][order[i]]);
  }
  axis_of[2] = {axis_of[0][1] * axis_of[1][2] - axis_of[0][2] * axis_of[1][1], axis_of[0][2] * axis_of[1][0] - axis_of[0][0] * axis_of[1][2],
                axis_of[0][0] * axis_of[1][1] - axis_of[0][1] * axis_of[1][0]};
  for (size_t row = 0; row < 3; ++row)
    for (size_t col = 0; col < 3; ++col) frame.axes[row * 3 + col] = axis_of[col][row];
  frame.valid = true;
  return frame;
}

// The turn laying one rigid piece onto another: principal axes onto principal axes, of the four ways to point them (each pair of axes
// flipped, or none) the one whose laid-on points, scaled to the other piece's length, lie nearest the other piece's points both ways.
// The axes' own directions (piece_frame) need not agree between a detailed mesh and a coarse one of the same object.
Mat3 lay_piece(const std::vector<Vec3>& menu_points, const PieceFrame& menu_frame, const std::vector<Vec3>& field_points,
               const PieceFrame& field_frame) {
  auto length = [](const std::vector<Vec3>& points, const PieceFrame& frame) {
    float low = 0.0f, high = 0.0f;
    for (const Vec3& point : points) {
      float along = 0.0f;
      for (size_t k = 0; k < 3; ++k) along += (point[k] - frame.centre[k]) * frame.axes[k * 3];
      low = std::min(low, along), high = std::max(high, along);
    }
    return high - low;
  };
  const float menu_length = length(menu_points, menu_frame), field_length = length(field_points, field_frame);
  const float ratio = menu_length > 0.0f ? field_length / menu_length : 1.0f;
  auto nearest = [](const Vec3& point, const std::vector<Vec3>& others) {
    float best = 1e30f;
    for (const Vec3& other : others) best = std::min(best, distance(point, other));
    return best;
  };
  constexpr float kFlips[4][3] = {{1, 1, 1}, {-1, -1, 1}, {-1, 1, -1}, {1, -1, -1}};
  Mat3 best_turn{1, 0, 0, 0, 1, 0, 0, 0, 1};
  float best_cost = 1e30f;
  for (const auto& flip : kFlips) {
    Mat3 flipped = field_frame.axes;
    for (size_t row = 0; row < 3; ++row)
      for (size_t col = 0; col < 3; ++col) flipped[row * 3 + col] *= flip[col];
    const Mat3 turn = multiply(flipped, transpose(menu_frame.axes));
    std::vector<Vec3> laid;
    for (const Vec3& point : menu_points) {
      Vec3 arm;
      for (size_t k = 0; k < 3; ++k) arm[k] = (point[k] - menu_frame.centre[k]) * ratio;
      const Vec3 turned = rotate(turn, arm);
      laid.push_back({field_frame.centre[0] + turned[0], field_frame.centre[1] + turned[1], field_frame.centre[2] + turned[2]});
    }
    float cost = 0.0f;
    for (const Vec3& point : laid) cost += nearest(point, field_points) / static_cast<float>(laid.size());
    for (const Vec3& point : field_points) cost += nearest(point, laid) / static_cast<float>(field_points.size());
    if (cost < best_cost) best_cost = cost, best_turn = turn;
  }
  return best_turn;
}

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

u64 body_fingerprint(const assets::ModelAsset& model) {
  constexpr u64 kFnvOffset = 0xCBF29CE484222325ull, kFnvPrime = 0x100000001B3ull;
  u64 hash = kFnvOffset;
  const auto mix = [&hash](s64 value) {
    for (int shift = 0; shift < 64; shift += 8) hash = (hash ^ ((static_cast<u64>(value) >> shift) & 0xFF)) * kFnvPrime;
  };
  mix(static_cast<s64>(model.skeleton.joints.size()));
  for (const auto& joint : model.skeleton.joints) mix(joint.parent);
  mix(static_cast<s64>(model.meshes.size()));
  for (const auto& mesh : model.meshes) {
    mix(static_cast<s64>(mesh.pool.size()));
    for (const auto& vertex : mesh.pool)
      for (const float axis : vertex.bind_pos) mix(std::isfinite(axis) ? std::lround(axis) : 0);
  }
  return hash;
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

namespace {

// Copies the field piece on `field_joint` (its faces, their materials and textures, the 4x images too) onto `menu` as a construct of a
// new root joint placed at the piece's centre with no rest turn, the vertices divided by `scale` into menu units. The joint's index, -1
// when the piece has no faces or no construct is left.
int graft_field_piece(assets::ModelAsset& menu, const assets::ModelAsset& field, int field_joint, float scale) {
  const auto joint = static_cast<u16>(menu.skeleton.joints.size());
  const Mat3 rest_turn = rest_pose(field.skeleton).rot[static_cast<size_t>(field_joint)];
  Vec3 centre{};
  int count = 0;
  for (const auto& mesh : field.meshes)
    for (const auto& vertex : mesh.pool)
      if (vertex.joint[0] == field_joint) {
        for (size_t axis = 0; axis < 3; ++axis) centre[axis] += vertex.bind_pos[axis] / scale;
        ++count;
      }
  if (count == 0) return -1;
  for (float& axis : centre) axis /= static_cast<float>(count);
  std::vector<int> texture_of(field.textures.size(), -1);
  bool grafted = false;
  for (size_t construct = 0; construct < field.meshes.size(); ++construct) {
    const assets::Mesh& source = field.meshes[construct];
    assets::Mesh piece;
    piece.skinned = true;
    piece.trans = source.trans;
    std::vector<int> pool_of(source.pool.size(), -1), vertex_of(source.vertices.size(), -1);
    for (size_t material = 0; material < source.materials.size(); ++material) piece.materials.push_back(source.materials[material]);
    for (const assets::SubMesh& part : source.submeshes) {
      assets::SubMesh out{part.material, static_cast<u32>(piece.indices.size()), 0, part.not_gouraud_textured};
      for (u32 i = part.first_index; i + 2 < part.first_index + part.index_count && i + 2 < source.indices.size(); i += 3) {
        bool inside = true;
        for (u32 corner = 0; corner < 3; ++corner) inside = inside && source.pool[source.vertices[source.indices[i + corner]].pool].joint[0] == field_joint;
        if (!inside) continue;
        for (u32 corner = 0; corner < 3; ++corner) {
          const u32 index = source.indices[i + corner];
          if (vertex_of[index] < 0) {
            const u32 pool = source.vertices[index].pool;
            if (pool_of[pool] < 0) {
              assets::SkinVertex vertex = source.pool[pool];
              for (size_t axis = 0; axis < 3; ++axis) vertex.bind_pos[axis] /= scale;
              vertex.joint = {joint, joint};
              vertex.weight = {1.0f, 0.0f};
              for (size_t axis = 0; axis < 3; ++axis) vertex.local[0][axis] = vertex.bind_pos[axis] - centre[axis];
              vertex.local[1] = Vec3{};
              const Vec3 normal = rotate(rest_turn, Vec3{static_cast<float>(vertex.normal[0]), static_cast<float>(vertex.normal[1]),
                                                         static_cast<float>(vertex.normal[2])});
              for (size_t axis = 0; axis < 3; ++axis) vertex.normal[axis] = static_cast<s8>(std::clamp(std::lround(normal[axis]), -127l, 127l));
              vertex.normal_joint = 0;
              pool_of[pool] = static_cast<int>(piece.pool.size());
              piece.pool.push_back(vertex);
            }
            assets::MeshVertex corner_vertex = source.vertices[index];
            corner_vertex.pool = static_cast<u32>(pool_of[pool]);
            vertex_of[index] = static_cast<int>(piece.vertices.size());
            piece.vertices.push_back(corner_vertex);
          }
          piece.indices.push_back(static_cast<u32>(vertex_of[index]));
        }
        piece.tri_quad.push_back(i / 3 < source.tri_quad.size() ? source.tri_quad[i / 3] : u8{0});
      }
      out.index_count = static_cast<u32>(piece.indices.size()) - out.first_index;
      if (out.index_count > 0) piece.submeshes.push_back(out);
    }
    if (piece.indices.empty() || menu.meshes.size() >= menu.shading.size()) continue;
    for (assets::Material& material : piece.materials) {
      if (material.texture < 0 || static_cast<size_t>(material.texture) >= field.textures.size()) continue;
      const auto texture = static_cast<size_t>(material.texture);
      if (texture_of[texture] < 0) {
        menu.hd_textures.resize(menu.textures.size());
        texture_of[texture] = static_cast<int>(menu.textures.size());
        menu.textures.push_back(field.textures[texture]);
        menu.hd_textures.push_back(field.has_hd(texture) ? field.hd_textures[texture] : import::Image{});
      }
      material.texture = texture_of[texture];
    }
    menu.shading[menu.meshes.size()] = field.shading[construct];
    menu.meshes.push_back(std::move(piece));
    grafted = true;
  }
  if (!grafted) return -1;
  assets::Joint added;
  added.parent = -1;
  added.rest_trans = centre;
  menu.skeleton.joints.push_back(added);
  for (assets::AnimationClip& clip : menu.clips)
    if (clip.tracks.size() + 1 == menu.skeleton.joints.size()) clip.tracks.emplace_back();
  return joint;
}

// A prop modelled into a body joint's piece (a shield on the forearm's joint): the shells of `joint`'s mesh that touch no other joint's
// mesh (every vertex bound to `joint` alone, no face shared with another joint's vertices) are moved onto a new joint under `joint`, so
// they can be left undrawn while the rest of the piece is drawn. Nothing is moved unless the piece also has mesh joined to its
// neighbours. Returns the new joint, -1 for none.
int split_free_shells(assets::ModelAsset& menu, int joint) {
  const auto count = static_cast<u16>(menu.skeleton.joints.size());
  const auto own = static_cast<u16>(joint);
  // Per mesh, per pool vertex: the shell it belongs to (a vertex of it), and per shell whether it reaches another joint's mesh.
  std::vector<std::vector<u32>> shells(menu.meshes.size());
  std::vector<std::vector<bool>> reaches(menu.meshes.size());
  bool held = false, loose = false;
  for (size_t index = 0; index < menu.meshes.size(); ++index) {
    const assets::Mesh& mesh = menu.meshes[index];
    std::vector<u32>& shell = shells[index];
    shell.resize(mesh.pool.size());
    for (u32 i = 0; i < shell.size(); ++i) shell[i] = i;
    auto root = [&](u32 vertex) {
      while (shell[vertex] != vertex) vertex = shell[vertex] = shell[shell[vertex]];
      return vertex;
    };
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
      for (size_t corner = 1; corner < 3; ++corner) shell[root(mesh.vertices[mesh.indices[i + corner]].pool)] = root(mesh.vertices[mesh.indices[i]].pool);
    for (u32 i = 0; i < shell.size(); ++i) shell[i] = root(i);
    reaches[index].assign(mesh.pool.size(), false);
    std::vector<bool> faced(mesh.pool.size(), false);
    for (const u32 corner : mesh.indices) faced[shell[mesh.vertices[corner].pool]] = true;
    for (u32 i = 0; i < mesh.pool.size(); ++i) {
      const assets::SkinVertex& vertex = mesh.pool[i];
      if (!faced[shell[i]] || vertex.joint[0] != own || (vertex.weight[1] > 0.0f && vertex.joint[1] != own)) reaches[index][shell[i]] = true;
    }
    for (u32 i = 0; i < mesh.pool.size(); ++i)
      if (mesh.pool[i].joint[0] == own && faced[shell[i]]) (reaches[index][shell[i]] ? held : loose) = true;
  }
  if (!held || !loose) return -1;
  for (size_t index = 0; index < menu.meshes.size(); ++index)
    for (u32 i = 0; i < menu.meshes[index].pool.size(); ++i) {
      if (reaches[index][shells[index][i]]) continue;
      assets::SkinVertex& vertex = menu.meshes[index].pool[i];
      vertex.joint = {count, count};
      vertex.weight = {1.0f, 0.0f};
      vertex.local[1] = vertex.local[0];
      vertex.normal_joint = 0;
    }
  // The new joint rests exactly on `joint`, so the moved vertices keep their place in its frame.
  assets::Joint added;
  added.parent = joint;
  menu.skeleton.joints.push_back(added);
  for (assets::AnimationClip& clip : menu.clips)
    if (clip.tracks.size() + 1 == menu.skeleton.joints.size()) clip.tracks.emplace_back();
  return count;
}

// The turn taking unit direction `from` onto unit direction `to` by the shortest way (none when they point opposite ways).
Mat3 turn_between(const Vec3& from, const Vec3& to) {
  const Vec3 axis{from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2], from[0] * to[1] - from[1] * to[0]};
  const float cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
  if (cosine < -0.999f) return Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
  const Mat3 cross{0, -axis[2], axis[1], axis[2], 0, -axis[0], -axis[1], axis[0], 0};
  const Mat3 square = multiply(cross, cross);
  Mat3 out{1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (size_t k = 0; k < 9; ++k) out[k] += cross[k] + square[k] / (1.0f + cosine);
  return out;
}

// A menu joint without a field partner keeps its rest angle and offset to its parent, unless the menu model's first pose clip (the Status
// screen's) carries it further than kTravelShare of the menu skeleton's height `height`: a held object rests at the model origin and only
// the clips put it in the hand, so it takes the clip's angle and offset. A joint the clip turns or shifts a little keeps its rest values
// (a skirt panel the Status stance flares out). `stance`: the clip's values whatever the distance (RigTuning::stance).
void ride_on_parent(MenuRig& rig, const assets::ModelAsset& menu_model, size_t j, float height, bool stance = false) {
  const assets::Joint& joint = menu_model.skeleton.joints[j];
  rig.menu_local[j] = assets::rotation_zyx(joint.rest_rot_deg);
  rig.menu_offset[j] = joint.rest_trans;
  for (const assets::AnimationClip& clip : menu_model.clips) {
    if (clip.source != "s4" || clip.index != 0 || clip.frame_count == 0 || clip.tracks.size() != menu_model.skeleton.joints.size()) continue;
    const bool carried = !clip.tracks[j].trans.empty() && distance(clip.tracks[j].trans[0].v, joint.rest_trans) > kTravelShare * height;
    if (!carried && !stance) return;
    if (!clip.tracks[j].trans.empty()) rig.menu_offset[j] = clip.tracks[j].trans[0].v;
    if (!clip.tracks[j].rot.empty()) rig.menu_local[j] = assets::rotation_zyx(clip.tracks[j].rot[0].v);
    return;
  }
}

// Per joint: its piece (the vertices bound first to it or to a joint below it) shares a face or a blended vertex with the mesh of a joint
// outside it, so the joint's move stretches the mesh between the two rather than carrying a piece of its own.
std::vector<bool> joined_pieces(const assets::ModelAsset& model) {
  const size_t count = model.skeleton.joints.size();
  std::vector<bool> joined(count, false);
  auto below = [&](size_t joint, size_t top) {
    int at = static_cast<int>(joint);
    while (at >= 0 && static_cast<size_t>(at) != top) at = model.skeleton.joints[static_cast<size_t>(at)].parent;
    return at >= 0;
  };
  auto join = [&](size_t lhs, size_t rhs) {
    if (lhs >= count || rhs >= count || lhs == rhs) return;
    if (!below(rhs, lhs)) joined[lhs] = true;
    if (!below(lhs, rhs)) joined[rhs] = true;
  };
  for (const assets::Mesh& mesh : model.meshes) {
    for (const assets::SkinVertex& vertex : mesh.pool)
      if (vertex.weight[1] > 0.0f) join(vertex.joint[0], vertex.joint[1]);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
      for (size_t corner = 0; corner < 3; ++corner)
        join(mesh.pool[mesh.vertices[mesh.indices[i + corner]].pool].joint[0], mesh.pool[mesh.vertices[mesh.indices[i + (corner + 1) % 3]].pool].joint[0]);
  }
  return joined;
}

// The rotation nearest to a blend of two rotations: its first two rows made unit and perpendicular, the third their cross product.
Mat3 orthonormal(const Mat3& blend) {
  Vec3 first{blend[0], blend[1], blend[2]}, second{blend[3], blend[4], blend[5]};
  const float first_length = std::max(1e-6f, std::hypot(first[0], first[1], first[2]));
  for (float& axis : first) axis /= first_length;
  const float along = first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
  for (size_t axis = 0; axis < 3; ++axis) second[axis] -= along * first[axis];
  const float second_length = std::max(1e-6f, std::hypot(second[0], second[1], second[2]));
  for (float& axis : second) axis /= second_length;
  return {first[0],  first[1],  first[2],  second[0], second[1], second[2], first[1] * second[2] - first[2] * second[1],
          first[2] * second[0] - first[0] * second[2], first[0] * second[1] - first[1] * second[0]};
}

// Every joint of `joint`'s subtree.
std::vector<bool> subtree(const assets::Skeleton& skeleton, size_t joint) {
  std::vector<bool> under(skeleton.joints.size(), false);
  under[joint] = true;
  for (size_t k = joint + 1; k < under.size(); ++k) {
    const int parent = skeleton.joints[k].parent;
    under[k] = parent >= 0 && under[static_cast<size_t>(parent)];
  }
  return under;
}

}  // namespace

void tune_menu_rig(MenuRig& rig, const assets::ModelAsset& field_model, const assets::ModelAsset& menu_model, const RigTuning& tuning) {
  const RestPose field_pose = rest_pose(field_model.skeleton), menu_pose = rest_pose(menu_model.skeleton);
  const Shape field_shape = shape_of(field_model, field_pose);
  const size_t field_count = field_pose.pos.size(), menu_count = rig.field_joint.size();
  if (menu_pose.pos.size() != menu_count || rig.menu_rest.size() != menu_count || rig.field_rest.size() != field_count) return;
  std::vector<bool> body_chain(field_count, false);
  for (int at = head_joint(field_model, field_shape); at >= 0; at = field_shape.parent[static_cast<size_t>(at)]) body_chain[static_cast<size_t>(at)] = true;
  auto nearest_paired = [&](size_t joint) {
    int above = menu_model.skeleton.joints[joint].parent;
    while (above >= 0 && rig.field_joint[static_cast<size_t>(above)] < 0) above = menu_model.skeleton.joints[static_cast<size_t>(above)].parent;
    return above;
  };
  if (tuning.align_limbs) {
    // The bone of a paired joint off the body chain: to the first menu joint below it paired with a child of its field joint.
    for (size_t j = 1; j < menu_count; ++j) {
      const int source = rig.field_joint[j];
      if (source <= 0 || body_chain[static_cast<size_t>(source)]) continue;
      for (size_t k = j + 1; k < menu_count; ++k) {
        const int child = rig.field_joint[k];
        if (child < 0 || field_shape.parent[static_cast<size_t>(child)] != source || nearest_paired(k) != static_cast<int>(j)) continue;
        Vec3 field_bone, menu_bone;
        for (size_t axis = 0; axis < 3; ++axis) {
          field_bone[axis] = (field_pose.pos[static_cast<size_t>(child)][axis] - field_pose.pos[static_cast<size_t>(source)][axis]) / field_pose.height;
          menu_bone[axis] = (menu_pose.pos[k][axis] - menu_pose.pos[j][axis]) / menu_pose.height;
        }
        const float field_length = std::hypot(field_bone[0], field_bone[1], field_bone[2]);
        const float menu_length = std::hypot(menu_bone[0], menu_bone[1], menu_bone[2]);
        if (field_length < kMinBone || menu_length < kMinBone) continue;
        for (size_t axis = 0; axis < 3; ++axis) field_bone[axis] /= field_length, menu_bone[axis] /= menu_length;
        rig.menu_rest[j] = multiply(turn_between(menu_bone, field_bone), rig.menu_rest[j]);
        break;
      }
    }
  }
  // Leaf pieces laid along their field pieces: the joint-to-mesh-centre line of each, in the rest poses.
  for (const s8 joint : tuning.lay) {
    if (joint <= 0 || static_cast<size_t>(joint) >= menu_count || rig.field_joint[static_cast<size_t>(joint)] < 0) continue;
    const auto j = static_cast<size_t>(joint);
    const auto source = static_cast<size_t>(rig.field_joint[j]);
    const std::vector<bool> menu_piece = subtree(menu_model.skeleton, j), field_piece = subtree(field_model.skeleton, source);
    auto reach = [](const assets::ModelAsset& model, const std::vector<bool>& piece, const Vec3& from, Vec3& out) {
      int vertices = 0;
      out = Vec3{};
      for (const assets::Mesh& mesh : model.meshes)
        for (const assets::SkinVertex& vertex : mesh.pool) {
          if (vertex.joint[0] >= piece.size() || !piece[vertex.joint[0]]) continue;
          for (size_t axis = 0; axis < 3; ++axis) out[axis] += vertex.bind_pos[axis] - from[axis];
          ++vertices;
        }
      const float length = std::hypot(out[0], out[1], out[2]);
      if (vertices == 0 || length <= 0.0f) return false;
      for (float& axis : out) axis /= length;
      return true;
    };
    Vec3 menu_line, field_line;
    if (!reach(menu_model, menu_piece, menu_pose.pos[j], menu_line) || !reach(field_model, field_piece, field_pose.pos[source], field_line)) continue;
    rig.menu_rest[j] = multiply(turn_between(menu_line, field_line), rig.menu_rest[j]);
  }
  // The body chain measured from the field stand pose: the first keyframe of clip 1, built in model space.
  if (tuning.posture) {
    const assets::AnimationClip* stand = nullptr;
    for (const assets::AnimationClip& clip : field_model.clips)
      if (!stand && clip.source == "s4" && clip.index == 1 && clip.frame_count > 0 && clip.tracks.size() == field_count) stand = &clip;
    std::vector<Mat3> standing(field_count);
    for (size_t i = 0; stand && i < field_count; ++i) {
      const assets::Joint& joint = field_model.skeleton.joints[i];
      const Mat3 local = assets::rotation_zyx(stand->tracks[i].rot.empty() ? joint.rest_rot_deg : stand->tracks[i].rot[0].v);
      standing[i] = joint.parent >= 0 ? multiply(standing[static_cast<size_t>(joint.parent)], local) : local;
    }
    const int head = head_joint(field_model, field_shape);
    for (size_t i = 0; stand && i < field_count; ++i)
      if (body_chain[i] && static_cast<int>(i) != head) rig.field_rest[i] = standing[i];
  }
  if (tuning.field_body) {
    rig.pivot_shift.assign(menu_count, Vec3{});
    for (size_t j = 1; j < menu_count; ++j) {
      const int source = rig.field_joint[j];
      const int parent = menu_model.skeleton.joints[j].parent;
      if (source <= 0 || !body_chain[static_cast<size_t>(source)] || parent < 0 || rig.field_joint[static_cast<size_t>(parent)] < 0) continue;
      for (size_t axis = 0; axis < 3; ++axis) rig.pivot_shift[j][axis] = field_pose.pos[static_cast<size_t>(source)][axis] / rig.scale - menu_pose.pos[j][axis];
    }
    rig.root_motion = 1.0f / rig.scale;
  }
  rig.on_field.assign(menu_count, false);
  rig.menu_sole.assign(menu_count, {});
  rig.field_sole.assign(menu_count, {});
  if (rig.mesh_shift.size() != menu_count) rig.mesh_shift.assign(menu_count, Vec3{});
  for (const s8 joint : tuning.on_field) {
    if (joint <= 0 || static_cast<size_t>(joint) >= menu_count || rig.field_joint[static_cast<size_t>(joint)] < 0) continue;
    const auto j = static_cast<size_t>(joint);
    const auto source = static_cast<size_t>(rig.field_joint[j]);
    // The menu piece in the field rest pose (its rest turn and the alignment above), from its joint.
    const Mat3 lay = multiply(rig.menu_rest[j], transpose(menu_pose.rot[j]));
    const std::vector<bool> menu_piece = subtree(menu_model.skeleton, j), field_piece = subtree(field_model.skeleton, source);
    Vec3 menu_centre{}, field_centre{};
    for (const assets::Mesh& mesh : menu_model.meshes)
      for (const assets::SkinVertex& vertex : mesh.pool) {
        if (vertex.joint[0] >= menu_count || !menu_piece[vertex.joint[0]]) continue;
        Vec3 arm;
        for (size_t axis = 0; axis < 3; ++axis) arm[axis] = vertex.bind_pos[axis] - menu_pose.pos[j][axis];
        rig.menu_sole[j].push_back(rotate(lay, arm));
        for (size_t axis = 0; axis < 3; ++axis) menu_centre[axis] += rig.menu_sole[j].back()[axis];
      }
    for (const assets::Mesh& mesh : field_model.meshes)
      for (const assets::SkinVertex& vertex : mesh.pool) {
        if (vertex.joint[0] >= field_count || !field_piece[vertex.joint[0]]) continue;
        rig.field_sole[j].push_back({vertex.joint[0], vertex.local[0]});
        for (size_t axis = 0; axis < 3; ++axis) field_centre[axis] += vertex.bind_pos[axis] - field_pose.pos[source][axis];
      }
    if (rig.menu_sole[j].empty() || rig.field_sole[j].empty()) continue;
    rig.on_field[j] = true;
    for (size_t axis = 0; axis < 3; ++axis)
      rig.mesh_shift[j][axis] = field_centre[axis] / static_cast<float>(rig.field_sole[j].size()) / rig.scale -
                                menu_centre[axis] / static_cast<float>(rig.menu_sole[j].size());
  }
  rig.carried_by.assign(menu_count, -1);
  for (const auto& pair : tuning.carry)
    if (pair[0] > 0 && static_cast<size_t>(pair[0]) < menu_count && pair[1] >= 0 && pair[1] < pair[0]) rig.carried_by[static_cast<size_t>(pair[0])] = pair[1];
  for (const s8 joint : tuning.keep) {
    if (joint <= 0 || static_cast<size_t>(joint) >= menu_count) continue;
    const auto j = static_cast<size_t>(joint);
    if (j < rig.placed_from.size()) rig.placed_from[j] = -1;
    if (j < rig.swing_about.size()) rig.swing_about[j] = -1;
    if (j < rig.moved.size()) rig.moved[j] = false;
  }
  if (tuning.stance)
    for (size_t j = 1; j < menu_count; ++j)
      if (rig.field_joint[j] < 0) ride_on_parent(rig, menu_model, j, menu_pose.height, true);
  // Hanging pieces: placed by their menu parent, none of the paired joints' placement rules; menu_sole and field_sole for the reach limit.
  // Rest fixes: laid over the angle and offset to the parent of a joint without a partner, over the model-space rest turn of a paired one.
  for (const RigRest& fix : tuning.rest) {
    if (fix.joint <= 0 || static_cast<size_t>(fix.joint) >= menu_count) continue;
    const auto j = static_cast<size_t>(fix.joint);
    const Mat3 turn = assets::rotation_zyx({static_cast<float>(fix.turn[0]), static_cast<float>(fix.turn[1]), static_cast<float>(fix.turn[2])});
    if (rig.field_joint[j] >= 0) {
      rig.menu_rest[j] = multiply(turn, rig.menu_rest[j]);
      continue;
    }
    rig.menu_local[j] = multiply(turn, rig.menu_local[j]);
    for (size_t axis = 0; axis < 3; ++axis) rig.menu_offset[j][axis] += static_cast<float>(fix.offset[axis]);
  }
  // A piece's vertices from its joint, with what hangs under it (rest pose, model space).
  auto piece_arms = [&](size_t j) {
    std::vector<Vec3> arms;
    const std::vector<bool> piece = subtree(menu_model.skeleton, j);
    for (const assets::Mesh& mesh : menu_model.meshes)
      for (const assets::SkinVertex& vertex : mesh.pool) {
        if (vertex.joint[0] >= menu_count || !piece[vertex.joint[0]]) continue;
        Vec3 arm;
        for (size_t axis = 0; axis < 3; ++axis) arm[axis] = vertex.bind_pos[axis] - menu_pose.pos[j][axis];
        arms.push_back(arm);
      }
    return arms;
  };
  rig.floored.assign(menu_count, false);
  for (const s8 joint : tuning.floor) {
    if (joint <= 0 || static_cast<size_t>(joint) >= menu_count) continue;
    rig.menu_sole[static_cast<size_t>(joint)] = piece_arms(static_cast<size_t>(joint));
    rig.floored[static_cast<size_t>(joint)] = !rig.menu_sole[static_cast<size_t>(joint)].empty();
  }
  rig.lead.assign(menu_count, {-1, -1});
  rig.lead_arm.assign(menu_count, Vec3{});
  rig.lead_back.assign(menu_count, false);
  for (const auto& entry : tuning.lead) {
    if (entry[0] <= 0 || static_cast<size_t>(entry[0]) >= menu_count || entry[1] < 0 || entry[2] < 0 || static_cast<size_t>(entry[1]) >= field_count ||
        static_cast<size_t>(entry[2]) >= field_count)
      continue;
    const auto j = static_cast<size_t>(entry[0]);
    const std::vector<Vec3> arms = piece_arms(j);
    if (arms.empty()) continue;
    for (const Vec3& arm : arms)
      for (size_t axis = 0; axis < 3; ++axis) rig.lead_arm[j][axis] += arm[axis] / static_cast<float>(arms.size());
    rig.lead[j] = {entry[1], entry[2]};
    rig.lead_back[j] = menu_pose.pos[j][2] + rig.lead_arm[j][2] > menu_pose.pos[0][2];
  }
  rig.outer.assign(menu_count, false);
  for (const s8 joint : tuning.outer)
    if (joint > 0 && static_cast<size_t>(joint) < menu_count) rig.outer[static_cast<size_t>(joint)] = true;
  rig.aimed.assign(menu_count, false);
  rig.aim_point.assign(menu_count, Vec3{});
  rig.aim_rest.assign(menu_count, Vec3{});
  rig.aim_arm.assign(menu_count, Vec3{});
  for (const s8 joint : tuning.aim) {
    if (joint <= 0 || static_cast<size_t>(joint) >= menu_count || rig.field_joint[static_cast<size_t>(joint)] < 0) continue;
    const auto j = static_cast<size_t>(joint);
    const auto source = static_cast<size_t>(rig.field_joint[j]);
    Vec3 centre{}, local{};
    int vertices = 0;
    for (const assets::Mesh& mesh : field_model.meshes)
      for (const assets::SkinVertex& vertex : mesh.pool) {
        if (vertex.joint[0] != source) continue;
        for (size_t axis = 0; axis < 3; ++axis) centre[axis] += vertex.bind_pos[axis], local[axis] += vertex.local[0][axis];
        ++vertices;
      }
    const std::vector<Vec3> arms = piece_arms(j);
    Vec3 mean{};
    float reach = 0.0f;
    for (const Vec3& arm : arms) {
      for (size_t axis = 0; axis < 3; ++axis) mean[axis] += arm[axis] / static_cast<float>(arms.size());
      reach = std::max(reach, std::hypot(arm[0], arm[1], arm[2]));
    }
    const float length = std::hypot(mean[0], mean[1], mean[2]);
    if (vertices == 0 || length <= 0.0f) continue;
    for (size_t axis = 0; axis < 3; ++axis) {
      rig.aim_point[j][axis] = local[axis] / static_cast<float>(vertices);
      rig.aim_rest[j][axis] = centre[axis] / static_cast<float>(vertices) / rig.scale - menu_pose.pos[j][axis];
      rig.aim_arm[j][axis] = mean[axis] * reach / length;
    }
    rig.aimed[j] = true;
    if (j < rig.placed_from.size()) rig.placed_from[j] = -1;
    if (j < rig.swing_about.size()) rig.swing_about[j] = -1;
    if (j < rig.moved.size()) rig.moved[j] = false;
  }
  rig.hang.assign(menu_count, false);
  for (const s8 joint : tuning.hang) {
    if (joint <= 0 || static_cast<size_t>(joint) >= menu_count || rig.field_joint[static_cast<size_t>(joint)] < 0 || rig.menu_parent[static_cast<size_t>(joint)] < 0) continue;
    const auto j = static_cast<size_t>(joint);
    const std::vector<bool> menu_piece = subtree(menu_model.skeleton, j);
    const std::vector<bool> field_piece = subtree(field_model.skeleton, static_cast<size_t>(rig.field_joint[j]));
    rig.menu_sole[j].clear();
    rig.field_sole[j].clear();
    for (const assets::Mesh& mesh : menu_model.meshes)
      for (const assets::SkinVertex& vertex : mesh.pool) {
        if (vertex.joint[0] >= menu_count || !menu_piece[vertex.joint[0]]) continue;
        Vec3 arm;
        for (size_t axis = 0; axis < 3; ++axis) arm[axis] = vertex.bind_pos[axis] - menu_pose.pos[j][axis];
        rig.menu_sole[j].push_back(arm);
      }
    for (const assets::Mesh& mesh : field_model.meshes)
      for (const assets::SkinVertex& vertex : mesh.pool)
        if (vertex.joint[0] < field_count && field_piece[vertex.joint[0]]) rig.field_sole[j].push_back({vertex.joint[0], vertex.local[0]});
    rig.hang[j] = true;
    if (j < rig.placed_from.size()) rig.placed_from[j] = -1;
    if (j < rig.swing_about.size()) rig.swing_about[j] = -1;
    if (j < rig.moved.size()) rig.moved[j] = false;
    if (j < rig.pivot_shift.size()) rig.pivot_shift[j] = Vec3{};
  }
}

MenuRig match_body_rig(const assets::ModelAsset& field_model, assets::ModelAsset& menu_model, int body) {
  // A prop modelled into a body joint's piece gets a joint of its own first, so that the map can hide or pair it.
  for (const HiddenShell& shell : hidden_shells(body))
    if (shell.joint > 0 && static_cast<size_t>(shell.joint) < menu_model.skeleton.joints.size()) split_free_shells(menu_model, shell.joint);
  const auto map = menu_rig_map(body);
  MenuRig rig = map.empty() ? match_menu_rig(field_model, menu_model) : match_menu_rig(field_model, menu_model, map);
  if (const RigTuning* tuning = menu_rig_tuning(body)) tune_menu_rig(rig, field_model, menu_model, *tuning);
  const size_t menu_count = rig.field_joint.size(), field_count = field_model.skeleton.joints.size();
  if (rig.field_rest.size() != field_count || rig.menu_rest.size() != menu_count) return rig;
  rig.held_from.assign(menu_count, -1);
  rig.held_driver.assign(menu_count, -1);
  rig.held_anchor.assign(menu_count, -1);
  rig.held_turn.assign(menu_count, Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
  rig.held_arm.assign(menu_count, Vec3{});
  rig.held_reach.assign(menu_count, Vec3{});
  // A prop the detailed body lacks: the field piece itself, grafted on as a joint of its own.
  std::vector<int> grafted(menu_rig_props(body).size(), -1);
  for (size_t index = 0; index < grafted.size(); ++index) {
    const HeldProp& prop = menu_rig_props(body)[index];
    if (prop.menu_joints[0] >= 0 || prop.field_joint < 0 || static_cast<size_t>(prop.field_joint) >= field_count) continue;
    const int joint = graft_field_piece(menu_model, field_model, prop.field_joint, rig.scale);
    if (joint < 0) continue;
    grafted[index] = joint;
    const Vec3 place = menu_model.skeleton.joints[static_cast<size_t>(joint)].rest_trans;
    rig.field_joint.push_back(-1);
    rig.menu_parent.push_back(-1);
    rig.menu_rest.push_back(Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
    rig.menu_local.push_back(Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
    rig.menu_offset.push_back(place);
    rig.menu_rest_pos.push_back(place);
    rig.placed_from.push_back(-1);
    rig.anchor.push_back(-1);
    rig.swing_about.push_back(-1);
    rig.moved.push_back(false);
    rig.mesh_shift.push_back(Vec3{});
    rig.hidden.push_back(false);
    rig.held_from.push_back(-1);
    rig.held_driver.push_back(-1);
    rig.held_anchor.push_back(-1);
    rig.held_turn.push_back(Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
    rig.held_arm.push_back(Vec3{});
    rig.held_reach.push_back(Vec3{});
  }
  const size_t body_count = rig.field_joint.size();
  const RestPose field_pose = rest_pose(field_model.skeleton), menu_pose = rest_pose(menu_model.skeleton);
  for (size_t index = 0; index < grafted.size(); ++index) {
    const HeldProp& prop = menu_rig_props(body)[index];
    if (prop.field_joint < 0 || static_cast<size_t>(prop.field_joint) >= field_count || (prop.menu_joints[0] < 0 && grafted[index] < 0)) continue;
    std::vector<bool> piece(body_count, false);
    if (grafted[index] >= 0) piece[static_cast<size_t>(grafted[index])] = true;
    for (const s8 joint : prop.menu_joints)
      if (joint >= 0 && static_cast<size_t>(joint) < menu_count) piece[static_cast<size_t>(joint)] = true;
    std::vector<Vec3> menu_points, field_points;
    for (const auto& mesh : menu_model.meshes)
      for (const auto& vertex : mesh.pool)
        if (vertex.joint[0] < body_count && piece[vertex.joint[0]]) menu_points.push_back(vertex.bind_pos);
    for (const auto& mesh : field_model.meshes)
      for (const auto& vertex : mesh.pool)
        if (vertex.joint[0] == static_cast<u16>(prop.field_joint)) field_points.push_back(vertex.bind_pos);
    const PieceFrame menu_frame = piece_frame(menu_points), field_frame = piece_frame(field_points);
    if (!menu_frame.valid || !field_frame.valid) continue;
    const auto first = static_cast<size_t>(grafted[index] >= 0 ? menu_count : prop.menu_joints[0]);
    int driver = -1, anchor = -1;
    for (int at = field_model.skeleton.joints[static_cast<size_t>(prop.field_joint)].parent; at >= 0 && anchor < 0;
         at = field_model.skeleton.joints[static_cast<size_t>(at)].parent)
      for (size_t k = 0; k < first && anchor < 0; ++k)
        if (rig.field_joint[k] == at) driver = at, anchor = static_cast<int>(k);
    if (anchor < 0) continue;
    const Mat3 turn = lay_piece(menu_points, menu_frame, field_points, field_frame);
    Vec3 reach;
    for (size_t axis = 0; axis < 3; ++axis) reach[axis] = field_frame.centre[axis] - field_pose.pos[static_cast<size_t>(prop.field_joint)][axis];
    for (size_t j = 0; j < body_count; ++j) {
      if (!piece[j]) continue;
      Vec3 from_centre;
      for (size_t axis = 0; axis < 3; ++axis) from_centre[axis] = menu_pose.pos[j][axis] - menu_frame.centre[axis];
      rig.held_from[j] = prop.field_joint;
      rig.held_driver[j] = driver;
      rig.held_anchor[j] = anchor;
      rig.held_turn[j] = turn;
      rig.held_arm[j] = rotate(turn, from_centre);
      rig.held_reach[j] = reach;
    }
    // The field prop's joint drives the prop only: a finger the matcher paired with it rides its hand.
    for (size_t j = 1; j < body_count; ++j) {
      if (piece[j] || rig.field_joint[j] != prop.field_joint) continue;
      rig.field_joint[j] = -1;
      rig.placed_from[j] = rig.swing_about[j] = -1;
      rig.moved[j] = false;
      ride_on_parent(rig, menu_model, j, menu_pose.height);
    }
  }
  // Shown and hidden with a field construct: the one holding every vertex of the joint's field joint (a held prop's field piece), the
  // parent's for a joint without a partner.
  std::vector<int> construct_of(field_count, -1);
  std::vector<bool> spread(field_count, false);
  for (size_t construct = 0; construct < field_model.meshes.size(); ++construct)
    for (const assets::SkinVertex& vertex : field_model.meshes[construct].pool) {
      if (vertex.joint[0] >= field_count) continue;
      spread[vertex.joint[0]] = spread[vertex.joint[0]] || (construct_of[vertex.joint[0]] >= 0 && construct_of[vertex.joint[0]] != static_cast<int>(construct));
      construct_of[vertex.joint[0]] = static_cast<int>(construct);
    }
  rig.shown_with.assign(body_count, -1);
  for (size_t j = 1; j < body_count; ++j) {
    const int source = rig.held_from[j] >= 0 ? rig.held_from[j] : rig.field_joint[j];
    const int parent = rig.menu_parent[j];
    if (source >= 0) rig.shown_with[j] = spread[static_cast<size_t>(source)] ? -1 : construct_of[static_cast<size_t>(source)];
    else if (parent >= 0) rig.shown_with[j] = rig.shown_with[static_cast<size_t>(parent)];
  }
  return rig;
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
  rig.hidden.assign(menu_count, false);
  if (plan == MenuRigPlan::Map) {
    for (size_t j = 0; map_fits && j < menu_count; ++j) {
      map_fits = map[j] >= kHiddenJoint && map[j] < static_cast<int>(field_count);
      rig.hidden[j] = map[j] == kHiddenJoint || (menu.joints[j].parent >= 0 && rig.hidden[static_cast<size_t>(menu.joints[j].parent)]);
      rig.field_joint[j] = std::max<int>(map[j], -1);
    }
    if (!map_fits) return rig;
  } else {
    rig.field_joint = pair_joints(field_shape, menu_shape, plan, separate_branches);
    lift_to_mesh(rig.field_joint, menu_shape);
    if (const int head = head_joint(field_model, field_shape); head >= 0) {
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
  const std::vector<bool> field_joined = joined_pieces(field_model), menu_joined = joined_pieces(menu_model);
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
  // The body's own chain, the head (head_joint) down to the root, keeps the menu skeleton's offsets even where a clip carries it far (a
  // fall drops the pelvis to the floor): the two roots need not sit at the same point of the body (one rig's at the feet, the other's at the
  // hips), so the field offset from the root would place the pelvis twice as high.
  const int field_head = head_joint(field_model, field_shape);
  std::vector<bool> body_chain(field_count, false);
  for (int at = field_head; at >= 0 && static_cast<size_t>(at) < field_count; at = field_shape.parent[static_cast<size_t>(at)])
    body_chain[static_cast<size_t>(at)] = true;
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
    if (below && travels && followers[static_cast<size_t>(source)] == 1 && !body_chain[static_cast<size_t>(source)] && menu_shape.skinned[j] &&
        field_shape.vertices[static_cast<size_t>(source)] >= kMinPieceVertices) {
      rig.placed_from[j] = driver;
      rig.anchor[j] = anchor;
    } else if (below && !travels && field_shape.parent[static_cast<size_t>(source)] != driver) {
      int pivot = anchor;
      for (int at = field_shape.parent[static_cast<size_t>(source)]; at >= 0 && at != driver && pivot == anchor; at = field_shape.parent[static_cast<size_t>(at)])
        if (partner[static_cast<size_t>(at)] >= 0 && partner[static_cast<size_t>(at)] < static_cast<int>(j)) pivot = partner[static_cast<size_t>(at)];
      rig.swing_about[j] = pivot;
    } else if (travel[static_cast<size_t>(source)] > kMinTravel) {
      // A field joint whose mesh is joined to its neighbours stretches that mesh as it moves; a menu piece free of the mesh would come off
      // the body with it (a tuft of leaves the field model draws taller), so it keeps its own offset.
      rig.moved[j] = menu_joined[j] || !field_joined[static_cast<size_t>(source)];
    }
  }
  rig.menu_local.resize(menu_count);
  rig.menu_offset.resize(menu_count);
  rig.menu_parent = menu_shape.parent;
  // A menu joint without a field partner is placed on its parent as the menu model's first pose (clip s4 0, the Status screen's) has it:
  // a held object (Marcy's shield) rests at the model origin and only the clips put it in the hand.
  for (size_t j = 0; j < menu_count; ++j) {
    rig.menu_local[j] = assets::rotation_zyx(menu.joints[j].rest_rot_deg);
    rig.menu_offset[j] = menu.joints[j].rest_trans;
    if (rig.field_joint[j] < 0) ride_on_parent(rig, menu_model, j, menu_pose.height);
  }
  rig.field_root_rest = field_pose.pos[0];
  // As tall as the field model, feet to the top of the body (body_top; the head is head_joint and the menu joint that follows it).
  rig.scale = kBattleModelFieldScale;
  if (field_head >= 0) {
    const int head = field_head;
    const int menu_head = partner[static_cast<size_t>(head)];
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

void menu_rig_palette(const MenuRig& rig, std::span<const render::Mat4> field_palette, std::vector<render::Mat4>& out, u32 hidden_constructs) {
  const size_t count = rig.field_joint.size();
  out.resize(count);
  std::vector<Mat3> turned(count);
  std::vector<Vec3> placed(count), collapsed(count);
  std::vector<bool> gone(count, false);
  // The joint's matrix: a piece that is not drawn collapses onto its joint, or onto the topmost joint above it that is not drawn.
  auto finish = [&](size_t j) {
    const int parent = rig.menu_parent[j];
    const int construct = j < rig.shown_with.size() ? rig.shown_with[j] : -1;
    gone[j] = (j < rig.hidden.size() && rig.hidden[j]) || (construct >= 0 && construct < 32 && (hidden_constructs >> construct & 1u) != 0);
    collapsed[j] = gone[j] && parent >= 0 && gone[static_cast<size_t>(parent)] ? collapsed[static_cast<size_t>(parent)] : placed[j];
    out[j] = render::mat4_from_rt(gone[j] ? Mat3{} : turned[j], collapsed[j]);
  };
  auto rotation_of = [](const render::Mat4& matrix) {
    return Mat3{matrix.at(0, 0), matrix.at(0, 1), matrix.at(0, 2), matrix.at(1, 0), matrix.at(1, 1), matrix.at(1, 2), matrix.at(2, 0), matrix.at(2, 1), matrix.at(2, 2)};
  };
  auto position_of = [](const render::Mat4& matrix) { return Vec3{matrix.at(0, 3), matrix.at(1, 3), matrix.at(2, 3)}; };
  for (size_t j = 0; j < count; ++j) {
    const int parent = rig.menu_parent[j];
    const int source = rig.field_joint[j];
    const int held = j < rig.held_from.size() ? rig.held_from[j] : -1;
    if (held >= 0 && static_cast<size_t>(held) < field_palette.size() && static_cast<size_t>(rig.held_driver[j]) < field_palette.size() &&
        static_cast<size_t>(rig.held_anchor[j]) < j) {
      const Mat3 turn = multiply(rotation_of(field_palette[static_cast<size_t>(held)]), transpose(rig.field_rest[static_cast<size_t>(held)]));
      turned[j] = multiply(multiply(turn, rig.held_turn[j]), rig.menu_rest[j]);
      const Vec3 arm = rotate(turn, rig.held_arm[j]), reach = rotate(turn, rig.held_reach[j]);
      const Vec3 prop = position_of(field_palette[static_cast<size_t>(held)]), driver = position_of(field_palette[static_cast<size_t>(rig.held_driver[j])]);
      for (size_t axis = 0; axis < 3; ++axis)
        placed[j][axis] = placed[static_cast<size_t>(rig.held_anchor[j])][axis] + arm[axis] + (reach[axis] + prop[axis] - driver[axis]) / rig.scale;
      finish(j);
      continue;
    }
    if (source >= 0 && static_cast<size_t>(source) < field_palette.size()) {
      const size_t field_index = static_cast<size_t>(source);
      Mat3 turn = multiply(rotation_of(field_palette[field_index]), transpose(rig.field_rest[field_index]));
      // Of two leading field joints, the one that swings the piece further forward (or back, for a piece behind the root); the turn
      // passes from one to the other over kLeadBlend of the piece's arm, so the piece does not jump as the two cross.
      if (j < rig.lead.size() && rig.lead[j][0] >= 0 && static_cast<size_t>(rig.lead[j][0]) < field_palette.size() &&
          static_cast<size_t>(rig.lead[j][1]) < field_palette.size()) {
        Mat3 turns[2];
        float reach[2];
        for (size_t side = 0; side < 2; ++side) {
          const auto leader = static_cast<size_t>(rig.lead[j][side]);
          turns[side] = multiply(rotation_of(field_palette[leader]), transpose(rig.field_rest[leader]));
          reach[side] = rotate(turns[side], rig.lead_arm[j])[2] * (rig.lead_back[j] ? -1.0f : 1.0f);
        }
        const float band = kLeadBlend * std::max(1.0f, std::hypot(rig.lead_arm[j][0], rig.lead_arm[j][1], rig.lead_arm[j][2]));
        const float share = std::clamp((reach[0] - reach[1]) / band + 0.5f, 0.0f, 1.0f);
        const float eased = share * share * (3.0f - 2.0f * share);
        for (size_t k = 0; k < 9; ++k) turn[k] = turns[0][k] + (turns[1][k] - turns[0][k]) * eased;
        turn = orthonormal(turn);
      }
      turned[j] = multiply(turn, rig.menu_rest[j]);
    } else {
      turned[j] = parent >= 0 ? multiply(turned[static_cast<size_t>(parent)], rig.menu_local[j]) : rig.menu_local[j];
    }
    // A hanging piece keeps its rest orientation to the root: it hangs plumb from the place its parent carries it to.
    const bool hanging = j > 0 && j < rig.hang.size() && rig.hang[j];
    if (hanging) turned[j] = multiply(multiply(turned[0], transpose(rig.menu_rest[0])), rig.menu_rest[j]);
    const int from = j < rig.placed_from.size() ? rig.placed_from[j] : -1;
    const int swing = j < rig.swing_about.size() ? rig.swing_about[j] : -1;
    const bool field_ok = source >= 0 && static_cast<size_t>(source) < field_palette.size();
    if (j < rig.on_field.size() && rig.on_field[j] && field_ok) {
      const Mat3 turn = multiply(rotation_of(field_palette[static_cast<size_t>(source)]), transpose(rig.field_rest[static_cast<size_t>(source)]));
      const Vec3 shift = rotate(turn, rig.mesh_shift[j]);
      for (size_t axis = 0; axis < 3; ++axis)
        placed[j][axis] = field_palette[static_cast<size_t>(source)].at(static_cast<int>(axis), 3) / rig.scale + shift[axis];
      // Never lower than the field piece (model y points down): a longer menu foot tipped down would reach through the floor.
      float field_low = -1e30f, menu_low = -1e30f;
      for (const MenuRigPoint& point : rig.field_sole[j]) {
        if (static_cast<size_t>(point.joint) >= field_palette.size()) continue;
        const render::Mat4& frame = field_palette[static_cast<size_t>(point.joint)];
        field_low = std::max(field_low, (frame.at(1, 0) * point.local[0] + frame.at(1, 1) * point.local[1] + frame.at(1, 2) * point.local[2] + frame.at(1, 3)) / rig.scale);
      }
      for (const Vec3& point : rig.menu_sole[j]) menu_low = std::max(menu_low, placed[j][1] + rotate(turn, point)[1]);
      if (field_low > -1e30f && menu_low > field_low) placed[j][1] -= menu_low - field_low;
    } else if (j < rig.carried_by.size() && rig.carried_by[j] >= 0) {
      const auto carrier = static_cast<size_t>(rig.carried_by[j]);
      Vec3 arm;
      for (size_t axis = 0; axis < 3; ++axis) arm[axis] = rig.menu_rest_pos[j][axis] - rig.menu_rest_pos[carrier][axis];
      const Vec3 carried = rotate(multiply(turned[carrier], transpose(rig.menu_rest[carrier])), arm);
      for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] = placed[carrier][axis] + carried[axis];
      // An outer joint never falls behind its own parent's place: of the two, the one further out from the carrier the way the carried
      // place lies from it (seen from above), passing from one to the other over kOuterBlend of the rest arm's length so that the piece
      // does not jump as they cross.
      if (j < rig.outer.size() && rig.outer[j] && parent >= 0) {
        const Vec3 hung = rotate(turned[static_cast<size_t>(parent)], rig.menu_offset[j]);
        Vec3 own;
        for (size_t axis = 0; axis < 3; ++axis) own[axis] = placed[static_cast<size_t>(parent)][axis] + hung[axis];
        const float carried_reach = std::hypot(carried[0], carried[2]);
        const float own_reach = carried_reach > 0.0f
                                    ? ((own[0] - placed[carrier][0]) * carried[0] + (own[2] - placed[carrier][2]) * carried[2]) / carried_reach
                                    : std::hypot(own[0] - placed[carrier][0], own[2] - placed[carrier][2]);
        const float band = kOuterBlend * std::max(1.0f, std::hypot(arm[0], arm[1], arm[2]));
        const float share = std::clamp((own_reach - carried_reach) / band + 0.5f, 0.0f, 1.0f);
        const float eased = share * share * (3.0f - 2.0f * share);
        for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] += (own[axis] - placed[j][axis]) * eased;
      }
      // A carried joint whose field joint is translated takes that joint's move away from its rest offset as well.
      const int above = field_ok && j < rig.moved.size() && rig.moved[j] ? rig.field_parent[static_cast<size_t>(source)] : -1;
      if (above >= 0 && static_cast<size_t>(above) < field_palette.size()) {
        const render::Mat4& frame = field_palette[static_cast<size_t>(above)];
        const Vec3 rest = rotate(rotation_of(frame), rig.field_offset[static_cast<size_t>(source)]);
        for (size_t axis = 0; axis < 3; ++axis)
          placed[j][axis] += (field_palette[static_cast<size_t>(source)].at(static_cast<int>(axis), 3) - frame.at(static_cast<int>(axis), 3) - rest[axis]) / rig.scale;
      }
    } else if (parent >= 0 && from >= 0 && field_ok && static_cast<size_t>(from) < field_palette.size()) {
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
      const int carrier = rig.field_parent[static_cast<size_t>(source)];
      const size_t frame = carrier >= 0 && static_cast<size_t>(carrier) < field_palette.size() ? static_cast<size_t>(carrier) : static_cast<size_t>(source);
      const Mat3 turn = multiply(rotation_of(field_palette[frame]), transpose(rig.field_rest[frame]));
      Vec3 arm;
      for (size_t axis = 0; axis < 3; ++axis) arm[axis] = rig.menu_rest_pos[j][axis] - rig.menu_rest_pos[pivot][axis];
      const Vec3 swung = rotate(turn, arm);
      for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] = placed[pivot][axis] + swung[axis];
    } else if (parent >= 0) {
      const Vec3 offset = rotate(turned[static_cast<size_t>(parent)], rig.menu_offset[j]);
      for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] = placed[static_cast<size_t>(parent)][axis] + offset[axis];
      // Turned about its field joint's place rather than its own: the parent carries that place, the joint's own turn swings the rest.
      if (j < rig.pivot_shift.size() && field_ok) {
        const Mat3 own = multiply(turned[j], transpose(rig.menu_rest[j]));
        const Mat3 above = multiply(turned[static_cast<size_t>(parent)], transpose(rig.menu_rest[static_cast<size_t>(parent)]));
        const Vec3 from_above = rotate(above, rig.pivot_shift[j]), from_own = rotate(own, rig.pivot_shift[j]);
        for (size_t axis = 0; axis < 3; ++axis) placed[j][axis] += from_above[axis] - from_own[axis];
      }
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
    // An aimed piece: turned from the way its carrier holds it so that its far end moves as its field joint's vertices have moved.
    if (j < rig.aimed.size() && rig.aimed[j] && field_ok && parent >= 0) {
      const auto base = static_cast<size_t>(j < rig.carried_by.size() && rig.carried_by[j] >= 0 ? rig.carried_by[j] : parent);
      const Mat3 carry = multiply(turned[base], transpose(rig.menu_rest[base]));
      const render::Mat4& frame = field_palette[static_cast<size_t>(source)];
      const Vec3 expected = rotate(carry, rig.aim_rest[j]);
      Vec3 arm = rotate(carry, rig.aim_arm[j]), reach;
      for (size_t axis = 0; axis < 3; ++axis) {
        const int row = static_cast<int>(axis);
        const float centre = frame.at(row, 0) * rig.aim_point[j][0] + frame.at(row, 1) * rig.aim_point[j][1] + frame.at(row, 2) * rig.aim_point[j][2] + frame.at(row, 3);
        reach[axis] = arm[axis] + centre / rig.scale - placed[j][axis] - expected[axis];
      }
      const float arm_length = std::hypot(arm[0], arm[1], arm[2]), reach_length = std::hypot(reach[0], reach[1], reach[2]);
      if (arm_length > 0.0f && reach_length > 0.0f) {
        for (size_t axis = 0; axis < 3; ++axis) arm[axis] /= arm_length, reach[axis] /= reach_length;
        turned[j] = multiply(multiply(turn_between(arm, reach), carry), rig.menu_rest[j]);
      }
    }
    // A floored piece is lifted off the floor (model y = 0, y pointing down).
    if (j < rig.floored.size() && rig.floored[j]) {
      const Mat3 turn = multiply(turned[j], transpose(rig.menu_rest[j]));
      float low = -1e30f;
      for (const Vec3& arm : rig.menu_sole[j]) low = std::max(low, placed[j][1] + rotate(turn, arm)[1]);
      if (low > 0.0f) placed[j][1] -= low;
    }
    // A hanging piece reaches no lower than the floor (model y = 0, y pointing down) or, where it reaches lower, its field piece: tipped
    // forward about the root's sideways axis through its joint, by the least angle that lifts its lowest point that high.
    if (hanging && field_ok && !rig.menu_sole[j].empty()) {
      float field_low = 0.0f;
      for (const MenuRigPoint& point : rig.field_sole[j]) {
        if (static_cast<size_t>(point.joint) >= field_palette.size()) continue;
        const render::Mat4& frame = field_palette[static_cast<size_t>(point.joint)];
        field_low = std::max(field_low, (frame.at(1, 0) * point.local[0] + frame.at(1, 1) * point.local[1] + frame.at(1, 2) * point.local[2] + frame.at(1, 3)) / rig.scale);
      }
      const Mat3 carry = multiply(turned[j], transpose(rig.menu_rest[j]));
      auto tipped = [&](float angle) { return multiply(carry, Mat3{1, 0, 0, 0, std::cos(angle), std::sin(angle), 0, -std::sin(angle), std::cos(angle)}); };
      auto lowest = [&](float angle) {
        const Mat3 tip = tipped(angle);
        float low = -1e30f;
        for (const Vec3& arm : rig.menu_sole[j]) low = std::max(low, placed[j][1] + rotate(tip, arm)[1]);
        return low;
      };
      if (lowest(0.0f) > field_low) {
        float fits = 1.5707963f, misses = 0.0f;
        for (int step = 0; step < 16; ++step) {
          const float mid = 0.5f * (fits + misses);
          (lowest(mid) > field_low ? misses : fits) = mid;
        }
        turned[j] = multiply(tipped(fits), rig.menu_rest[j]);
      }
    }
    finish(j);
  }
}

namespace {

// Hand-made pairings, for the characters whose two skeletons the matcher cannot pair (one rig's pelvis or head sits a joint higher, limbs
// carry other helper joints, props hang off other joints) or pairs wrongly in a way no general rule catches (a hand left unpaired under a
// veil, a coat panel taking a cane, panels paired on one side only). -1: the menu joint rides on its parent (hair and other pieces the field rig has no
// joint for). Field joints that share one point (Marcy's skirt flaps, Razzly's wings) are told apart by where their vertices lie.
// Riddel: field joint per menu joint.
// The sleeves (one piece with the forearm) turn with the field hand joints, which carry the field's bell sleeves.
constexpr s8 kRiddelMap[] = {0, 1, 2, 3, -1, -1, -1, -1, -1, -1, -1, -1, 5, 6, 7, 7, 8, 9, 11, 11, -1, 12, 13, 15, 15, -1, 23, 24, 25, 26, 29, 28,
                             16, 17, 18, 19, 20, 21};
// Marcy: field joint per menu joint.
constexpr s8 kMarcyMap[] = {0, 1, 2, 3, -1, -1, 5, 4, 6, 7, 8, 9, -1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 25, 25, 22, 22, 23, 24, 22, 22};
// Razzly: field joint per menu joint.
constexpr s8 kRazzlyMap[] = {0, 1, 2, 3, -1, -1, -1, 11, 12, 13, 14, 7, 8, 9, 10, 4, 5, 6, 6, 15, 16, 17, 18, -1, 19, 20, 21, 22, -1, -1, 26, 24, 25, 23, 23};
// Harle: field joint per menu joint.
// Every joint of the two bell ribbons follows the field's one ribbon joint.
constexpr s8 kHarleMap[] = {0, 1, 2, 3, -1, -1, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15, 16, 16, 16, 16, 16, 16, -1, -1, 17, 18, 19, 20, -1, 21,
                            22, 23, 24, -1};
// Janice: field joint per menu joint.
// The drum on her forearm (in the field she carries none) is not drawn.
constexpr s8 kJaniceMap[] = {0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, kHiddenJoint, 10, 13, 14, 15, 16, 17, -1, 15, 18, 18, 19, 20, 21, 22, 23, 24,
                             25, 26, 27};
// NeoFio: field joint per menu joint.
constexpr s8 kNeoFioMap[] = {0, 1, 2, 3, 6, 5, -1, -1, -1, -1, 4, 7, 8, 9, 10, -1, -1, -1, -1, -1, -1, 11, 12, 13, 14, -1, -1, -1, -1, -1, -1, 15, 16, 17, 18, 19, 20, 21, 22};
// Irenes: field joint per menu joint.
constexpr s8 kIrenesMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, -1, -1, -1, 8, 9, 10, 11, -1, -1, -1, -1, 12, 3, -1, -1, -1, -1, 13, 15, 16, 17, 18, 19, 20, 21, 23, 22};
// Leah: field joint per menu joint.
// The tail's end rides its root (the field tail's second joint does not turn the way the tail points).
constexpr s8 kLeahMap[] = {0, 1, 2, -1, 3, 12, -1, 4, 5, 6, 7, 8, 9, 10, 11, -1, 17, 18, 19, 20, 13, 14, 15, 16, 21, 22, -1, -1, -1, -1, -1, -1, -1,
                           -1};
// Van: field joint per menu joint.
constexpr s8 kVanMap[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, 8, 9, 10, 11, -1, 21, -1, 12, 13, 14, 15, -1, 16, 17, 18, 19, -1, 24, 20, 22, 23, -1, -1, 23, 22};
// Sneff: field joint per menu joint.
constexpr s8 kSneffMap[] = {0, 1, 2, 3, -1, -1, 5, 6, 7, 8, -1, -1, 9, 10, 11, 12, -1, -1, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};

// Poshul: field joint per menu joint; the legs joint for joint (hip, thigh, foot, toe on both rigs: the matcher pairs the field thigh with
// the battle foot), the collar's tag on the field collar's ring (hung from the chest by her RigTuning).
// The ears' bases ride the head and their tips follow the field ears (the field ear joint carries the tip only).
constexpr s8 kPoshulMap[] = {0, 1, 2, 3, -1, -1, -1, 6, -1, 5, -1, 7, 8, 9, 10, 11, 12, 13, 14, 4, 17, 18, 19, 20, 21, 22, 23, 24, 16, -1, 15, -1};

// Leena: field joint per menu joint.
constexpr s8 kLeenaMap[] = {0, 1, 2, 3, -1, -1, 5, 6, 4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 23, 24, 25, 26, 27, 27, 27, 20, 20, 20, 22, 21, -1, 29, 28, -1};
// Turnip: field joint per menu joint.
// The cape's top follows the field cape.
constexpr s8 kTurnipMap[] = {0, 1, 2, 3, 3, 4, -1, -1, -1, 15, 17, 18, 15, 16, 13, 14, 15, 16, 17, 13, 5, 6, 7, 8, 9, 10, 11, 12, -1, 27, 19, 20,
                             21, 22, -1, 23, 24, 25, 26, -1};
// Pierre: field joint per menu joint.
constexpr s8 kPierreMap[] = {0, 1, 2, 3, -1, -1, -1, 5, -1, -1, 4, -1, 7, 6, 12, 13, 14, 15, 8, 9, 10, 11, -1, -1, 16, 17, 18, 19, 20, 21, 22, 23, 26, 27, 24, 25, -1, -1, -1, kHiddenJoint};

// Glenn (both field models share one skeleton, both menu models too): field joint per menu joint.
constexpr s8 kGlennMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 12, 13, 14, 15, -1, 8, 9, 10, 11, -1, 23, 24, 25, 26, 19, 20, 21, 22, 16, 17};

// Radius: field joint per menu joint.
// The robe's side and lower panels swing with the thighs and the feet's chain; its front panel is on the pelvis, hung plumb by his RigTuning.
constexpr s8 kRadiusMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 17, 19, 13, 15, 1, -1, 17, 19, 13, 15,
                             -1, -1};
// Fargo: field joint per menu joint.
constexpr s8 kFargoMap[] = {0, 1, 2, 3, -1, -1, 4, -1, 5, 6, 7, 8, 9, 10, 11, 12, -1, -1, -1, -1, -1, 13, 14, 15, 16, 17, 18, 19, 20, -1};
// Macha: field joint per menu joint.
constexpr s8 kMachaMap[] = {0, 1, 2, 3, -1, -1, 8, 9, 10, 11, 4, 5, 6, 7, -1, 12, 13, 14, 15, 16, 17, 18, 19, 21, 21, 20, 20, -1, -1, -1, -1, 21, -1, -1, 21, 21};
// Miki: field joint per menu joint.
// Both front hair locks turn with the field's one centre lock.
constexpr s8 kMikiMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 7, 8, 9, 10, 11, 12, 13, 14, -1, 15, 16, 17, 18, 19, 20, -1, 21, 22, 23, 24, 25, 26,
                           27, 28};
// Greco: field joint per menu joint.
// Every robe panel follows the thigh or shin its neighbour does (two rode the pelvis and split from the rest in a crouch).
constexpr s8 kGrecoMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 13, 14, 17, 18, 17, 23, 20, 21, 13, 14, 17, 18};
// Mel: field joint per menu joint.
constexpr s8 kMelMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 19, 15, 19, 15};
// Steena: field joint per menu joint.
// The ponytail's arch turns with the field ponytail like the tail below it.
constexpr s8 kSteenaMap[] = {0, 1, 2, -1, -1, -1, 3, -1, -1, 4, 4, 5, 6, 11, 12, 13, 14, 7, 8, 9, 10, 20, 21, 22, 23, 24, 15, 16, 17, 18, 19};
// Doc: field joint per menu joint.
constexpr s8 kDocMap[] = {0, 1, 2, 3, 4, 6, 8, 9, 10, 11, -1, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 25, 25, 24, 24, 21, 17, 26, 24, 24, 24, 24};
// Pip's flying form (tu_mm): field joint per menu joint.
// Bow and bell as in the other forms.
constexpr s8 kTuMmMap[] = {0, 1, 2, 3, 4, 5, -1, -1, -1, 18, 16, 17, 6, 7, 8, 11, 12, 13, 14, 15, 10, 9, 20, 20, -1, 19, 19, -1};
// Lynx's body (worn by Serge): field joint per menu joint.
// The lower coat's front side panels (children of the shins on the menu rig) follow the field coat's front hem joint and his RigTuning
// places them; the back side panels swing with the shins they hang from, which keeps them behind the trailing leg.
constexpr s8 kLynxMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1, 13, 14, 13, 14, 15, 16, 17, 18, 25, 17, -1, -1, 19, 20, 21, 22, 25, 21, -1, -1,
                           24, 25, 23, 26};

// Solt's battle model: field joint per battle joint (the matcher pairs the thighs with skirt panels); the side panels of the skirt swing
// with the thighs.
// The skirt's front and back hem follow the field skirt's corners; the empty joint under the hand shares the front corner so that the hem
// takes the corner's move rather than its place.
constexpr s8 kSoltMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 12, 13, 14, 15, 16, 17, 18, 19, 17, -1, 13, -1, -1, 20, -1, 21};
// Peppor's battle model: field joint per battle joint.
// The skirt's hem joints follow the field hem joints; its waist-level joints ride the pelvis (paired with the hem they swung the whole
// panel on a long arm).
constexpr s8 kPepporMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1, 12, 13, 14, 15, 16, 17, 18, 19, -1, 22, -1, 23, 21, -1, 20, -1};

// Dario's battle model: the matcher's pairing, with the sword (in the field he carries none) not drawn.
// Both halves of the cape follow the field cape's joints by height; the front hip plates turn with the thighs.
constexpr s8 kDarioMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, kHiddenJoint, 12, -1, -1, 13, 14, -1, -1, 13, 14, 15, 16, 17, 18, -1, 19, 20, 21,
                            22, -1, -1, 20, 16};

// Held props: Radius's cane, the Acacia Dragoon rider's lance and the Fossicker's shovel (their detailed bodies have none: the field piece
// is grafted on); the
// Porre soldiers' rifle (battle joints 17..19),
// aimed in the battle stance, held across the body or slung in the field.
constexpr HeldProp kHeldProps[] = {
    {0x10, 20, {-1, -1, -1, -1}},
    {134, 18, {-1, -1, -1, -1}},
    {140, 9, {17, 18, 19, -1}},
    {141, 9, {17, 18, 19, -1}},
    {166, 28, {-1, -1, -1, -1}},
};

// Nikki: the matcher's pairing, with the guitar at his hip (in the field he carries none) not drawn.
constexpr s8 kNikkiMap[] = {0, 1, 2, 3, -1, -1, -1, -1, 4, 5, 6, 7, -1, 8, 9, 10, 11, -1, 12, 13, 14, 15, 16, 17, 18, 19, kHiddenJoint, -1, -1};
// Orcha: the matcher's pairing. The wok on his back and its shoulder strap are one menu joint (13, at the strap's top); split off
// (kHiddenShells) onto the last joint, they follow the field wok, and the few vertices left on joint 13 ride the chest.
constexpr s8 kOrchaMap[] = {0, 1, 2, 3, 4, 9, 10, 11, 12, 5, 6, 7, 8, -1, 18, 19, 20, 21, 14, 15, 16, 17, 25, -1, 24, 23, 22, 13};
// Korcha: the matcher's pairing, with the oar (in the field he carries none) not drawn.
constexpr s8 kKorchaMap[] = {0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9, 10, 11, 12, kHiddenJoint, -1, -1, -1, -1, 13, 14, 15, 16, 17, 18, 19, 20};

// Robo Ducky's battle model: the matcher's pairing. The wheel under the body (the field model walks on its feet) rides the body; its
// RigTuning tucks it into its well, which would show as a hole in the belly without it.
constexpr s8 kRoboDuckyMap[] = {0, 1, 2, 3, 4, 5, 6, -1, 7, -1, 8, 9, 10, 11, -1};

// The Spearfisher's battle model: the matcher's pairing, with the trident (the field model carries none) not drawn.
constexpr s8 kSpearfisherMap[] = {0,  1,  2,  2,  -1, 3,  -1, 4,  5,  6,  7,  -1, kHiddenJoint, -1, 8,
                                  9,  10, 11, -1, 12, 13, 14, 15, 16, 17, 18, 19, 20,           21};

// The Fossicker's battle model: field joint per battle joint (the matcher crosses the upper and lower left arms); the tool only the
// battle model holds (upper right arm) not drawn.
constexpr s8 kFossickerMap[] = {0,  1,  2,  -1, -1, -1, 3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, kHiddenJoint, -1, -1,
                                -1, -1, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, -1, 30,           31, 32, 33, 34};

// Viper: the matcher's pairing, with the coat's front panels on the thighs like its side panels (the coat opens with the legs) and their
// lower halves hung plumb by his RigTuning.
constexpr s8 kViperMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 17, -1, 17, 17, 13, -1, -1, -1, 13, 13};

// Sprigg: field joint per menu joint. The field cloak is five flaps at waist height, its upper part rigid with the chest: the menu cloak's
// upper joints ride the chest and its lower joints follow the flaps.
constexpr s8 kSpriggMap[] = {0, 1, 2, 3, 4, 9, 10, 11, 12, 5, 6, 7, 8, -1, 13, -1, 15, -1, 17, -1, 16, -1, 14, -1, -1, -1, 16, -1, 17, 23, 24, 25,
                             26, 27, 18, 19, 20, 21, 22};

// Skelly: the matcher's pairing, with the neck ruff riding the chest (paired with the field head it pitched with every nod).
constexpr s8 kSkellyMap[] = {0, 1, 2, -1, 3, -1, 4, -1, -1, -1, -1, 6, 7, 8, 9, -1, -1, -1, -1, 10, 11, 12, 13, -1, -1, -1, -1, 18, 19, 20, 21, 14,
                             15, 16, 17};

// Pip's forms (tumaru, tu_p, tu_m, tu_pp, tu_pm; tu_mm below): the matcher's pairing, with the bow riding the chest and the bell on the field
// bell's joint (the field clips spin that small bell freely; a bow paired with it swung up over the mouth), hung plumb by the RigTuning.
constexpr s8 kPipMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -1, -1, -1, 18, 16, 17};

constexpr s8 kPipPMap[] = {0, 1, 2, 3, 5, 4, 16, 17, -1, -1, -1, 18, 6, 7, 8, 11, 12, 13, 14, 15, 10, -1, 9, -1, -1, -1, -1, -1};

constexpr s8 kPipMMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, 18, 16, 17, 19, 20, -1, -1, -1, -1};

constexpr s8 kPipPpMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 11, 12, 9, 10, -1, -1, -1, -1, -1, -1, -1, 22, 20, 21, 18, 19, 23, -1,
                            -1, -1, 24, -1, -1, -1};

constexpr s8 kPipPmMap[] = {0, 1, 2, 3, 5, 4, 16, 17, -1, -1, -1, 18, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 22, 20, 21, 19};

// The Acacia Dragoon private and sergeant (one rig): the pelvis on the field pelvis, the skirt's upper panels on the thighs and its hem
// joints on the field skirt pieces (all unpaired, the skirt was a rigid bell the legs came through).
constexpr s8 kDragoonMap[] = {0, 1, 1, 2, -1, 4, 5, 6, 7, 8, 9, 10, 11, -1, 3, 12, 13, 14, 15, 18, 19, 20, 21, 13, 17, 13, 16, 19, 22, 19, 23};

// The Taurminator's battle model: the head and neck on the field head and neck (the matcher gave them to a mane tuft); jaw, horns and
// mane ride.
constexpr s8 kTaurminatorMap[] = {0, 1, 2, 3, 4, -1, -1, -1, -1, -1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, -1, -1, -1, -1, -1, -1, -1,
                                  19, 20, 21, 22, 23, 24, 25, 26, 27, 28, -1, 29, -1, 30, 31};

// The Polis Police robot's battle model: both arms joint for joint (the matcher paired one arm a joint low).
constexpr s8 kPolisRobotMap[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, 8, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13,
                                 14, 15, 16, -1, -1, 17, 18, 19, 20, 21, -1, -1};

// Dark Serge's battle model: the matcher's pairing, with the cape's second half on the field cape's joints like the first.
constexpr s8 kDarkSergeMap[] = {0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11, -1, 12, 13, 12, 13, 14, 15, 16, 17, -1, -1, -1, -1, 18, 19, 20, 21,
                                -1, -1, -1, -1, -1, -1, -1, -1};

// Per-body adjustments (RigTuning), by battle model.
// Limbs aligned: Kid, Guile, Janice, Irenes (their feet meet the field model's floor line better) and the Acacia Dragoons (arms held as the
// field arms are). Poshul: the field rest pose holds her hind legs folded flat at foot level, her body turns about a point behind the menu
// model's, her hind feet and front paws hang free of the body mesh, her collar's tag hangs from the collar on her chest while the field
// collar and its ring ride the head, and her ear tips are laid as the field ears hang. Pink Pip: his root sits higher in the body than
// the field root, so his body follows the field body.
// Drawn on the field piece: Orcha's cleavers (also laid as the field cleavers hang) and his wok, Leena's back hem panels, the Dragoons'
// front and side hems,
// Turnip's forearms and hands and Pip's feet (the field limbs lengthen by moving their joints), Van's bag (hung from the chest on the menu
// rig, from the pelvis on the field rig), Sneff's upper arms.
// Hung plumb: the lower front coat panels of Viper and Lynx, Radius's front robe panel, Steena's bare feet (the field ankle helpers do
// not flatten in a kneel), the bell of every Pip form.
// Kept at their own offset, turning only: Riddel's hem and sleeves, Miki's hair locks, Steena's ponytail, Pierre's sash, the hem joints
// of Solt and the Dragoons, Dario's cape top and hip plates, Lynx's centre front strip and the tips of his cape halves (two vertices
// each, which the field cape's move drew out and in).
// Carried from another joint: Lynx's front side coat panels from the pelvis (they are children of the shins), Macha's upper front skirt
// panels from the thighs and kept (moved with the field hem they flew to chest height in a jump).
// Laid as the field piece hangs: the ribbons of Harle and Glenn, Orlha's twin tails.
// Lifted off the floor: the hems of Lynx's coat and Radius's robe in a crouch. Following the leading one of two field joints: the centre
// skirt panels of the Dragoons (leading thigh in front, trailing behind), Solt's centre hems, the mid ring of Leena's skirt front (whose
// front hems are carried from it, so each half is one sheet). Kept out with their own parent: Lynx's front panels, in front of the knee.
// Aimed at the field hem: Lynx's centre back strip (the field hem joints tip far over as he walks; the strip swings out as far as the hem).
// Rest turns: Viper's arms (modelled held out), Skelly's ruff (modelled level, the field's is tilted), Fargo's scarf tails (laid against
// chest and back), the mounted Dragoon's lower jaw (closed); a rest offset tucks Robo Ducky's wheel into its well.
// Status stance for the pieces without a field partner: Miki's veils. Standing posture from the field stand pose: Dark Serge (the field
// model stands swayed against its own rest pose).
constexpr RigTuning kRigTunings[] = {
    {0x01, true, false, {-1, -1, -1, -1}, {-1, -1}},
    {0x02, true, false, {-1, -1, -1, -1}, {-1, -1}},
    {0x05, false, false, {-1, -1, -1, -1}, {25, 31}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{7, {0, 0, 12}, {0, 0, 0}}, {8, {0, 0, 12}, {0, 0, 0}}, {11, {0, 0, -12}, {0, 0, 0}}, {12, {0, 0, -12}, {0, 0, 0}}}}},
    {0x06, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {18, 23, 30, 31, 36, 37, -1, -1}},
    {0x0C, true, true, {13, 17, 22, 26}, {19, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}}, {7, 9, -1, -1, -1, -1}},
    {0x0F, false, false, {24, 25, 27, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {24, 25, -1, -1, -1, -1}},
    {0x10, false, false, {-1, -1, -1, -1}, {26, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{}, {}, {}, {}}}, {32, 26, 23, 25}},
    {0x11, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{19, {0, 0, -8}, {0, 0, 0}}, {17, {0, 0, 10}, {0, 0, 0}}, {}, {}}}},
    {0x12, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {31, 34, -1, -1, -1, -1, -1, -1}, {{{31, 20}, {34, 16}, {-1, -1}, {-1, -1}}}},
    {0x13, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}}, {6, 7, 8, 9, -1, -1}},
    {0x14, false, false, {31, 34, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{32, 29}, {35, 26}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{}, {}, {}, {}}}, {-1, -1, -1, -1}, {{{29, 17, 27}, {26, 24, 20}, {-1, -1, -1}, {-1, -1, -1}}}},
    {0x15, false, false, {-1, -1, -1, -1}, {-1, -1}, true, {9, 10, 11, 12, -1, -1, -1, -1}},
    {0x16, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}}, {18, 19, 20, 21, 22, 23}},
    {0x17, true, false, {-1, -1, -1, -1}, {-1, -1}},
    {0x1C, false, false, {22, 23, 26, 27}, {-1, -1}},
    {0x1F, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{3, {0, 0, -10}, {0, 0, 0}}, {}, {}, {}}}},
    {0x21, true, false, {-1, -1, -1, -1}, {-1, -1}},
    {0x24, false, false, {14, -1, -1, -1}, {-1, -1}},
    {0x25, false, false, {7, 13, -1, -1}, {-1, -1}},
    {0x26, false, false, {-1, -1, -1, -1}, {24, 29}, false, {9, 10, -1, -1, -1, -1, -1, -1}},
    {0x29, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {32, 33, 34, 35, -1, -1, -1, -1}},
    {0x2A, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}}, {10, 11, 13, 14, -1, -1}},
    {0x2B, false, false, {-1, -1, -1, -1}, {19, -1}},
    {0x2C, false, true, {20, 22, -1, -1}, {11, -1}},
    {0x2D, false, false, {-1, -1, -1, -1}, {23, -1}},
    {0x2E, false, false, {14, 15, 16, 17}, {25, -1}},
    {0x2F, false, false, {-1, -1, -1, -1}, {11, -1}},
    {0x30, false, false, {-1, -1, -1, -1}, {9, -1}},
    {0x31, false, false, {-1, -1, -1, -1}, {21, 29}, false, {33, 34, 14, 16, -1, -1, -1, -1}, {{{21, 1}, {29, 1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{}, {}, {}, {}}}, {33, 35, -1, -1}, {{{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}}, {21, 29, -1, -1},
      {35, 36, -1, -1}},
    {0x32, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}}, {6, 7, 8, 9, -1, -1}},
    {71, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {26, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{}, {}, {}, {}}}, {-1, -1, -1, -1}, {{{28, 21, 22}, {26, 20, 23}, {-1, -1, -1}, {-1, -1, -1}}}},
    {91, true, false, {24, 30, -1, -1}, {-1, -1}, false, {26, 28, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{}, {}, {}, {}}}, {-1, -1, -1, -1}, {{{23, 13, 19}, {27, 13, 19}, {-1, -1, -1}, {-1, -1, -1}}}},
    {92, true, false, {24, 30, -1, -1}, {-1, -1}, false, {26, 28, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{}, {}, {}, {}}}, {-1, -1, -1, -1}, {{{23, 13, 19}, {27, 13, 19}, {-1, -1, -1}, {-1, -1, -1}}}},
    {134, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{6, {-20, 0, 0}, {0, 0, 0}}, {}, {}, {}}}},
    {147, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}},
      {-1, -1, -1, -1, -1, -1}, false, {{{14, {0, 0, 0}, {0, -400, 0}}, {}, {}, {}}}},
    {215, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {13, 33, 34, -1, -1, -1, -1, -1}},
    {217, false, false, {-1, -1, -1, -1}, {-1, -1}, false, {-1, -1, -1, -1, -1, -1, -1, -1}, {{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}}, {-1, -1, -1, -1, -1, -1}, true},
};

struct RigMap {
  int body;
  std::span<const s8> map;
};
constexpr RigMap kRigMaps[] = {
    {0x04, kNikkiMap},
    {0x06, kRiddelMap},
    {0x09, kMarcyMap},
    {0x0A, kKorchaMap},
    {0x0C, kPoshulMap},
    {0x0D, kRazzlyMap},
    {0x0F, kOrchaMap},
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
    {71, kSoltMap},
    {72, kPepporMap},
    {215, kDarioMap},
    {147, kRoboDuckyMap},
    {156, kSpearfisherMap},
    {166, kFossickerMap},
    {0x05, kViperMap},
    {0x1A, kSpriggMap},
    {0x1F, kSkellyMap},
    {0x2B, kPipMap},
    {0x2C, kPipPMap},
    {0x2D, kPipMMap},
    {0x2E, kPipPpMap},
    {0x2F, kPipPmMap},
    {91, kDragoonMap},
    {92, kDragoonMap},
    {174, kTaurminatorMap},
    {186, kPolisRobotMap},
    {217, kDarkSergeMap},
};

// Story NPCs and field enemies whose battle model is more detailed than the field model every room carries a copy of: field body
// fingerprint, battle model. The others with a battle model of their own (Garai, Miguel, the Sage, Ozzie, Slash, Flea, the Highwayman, the
// dragons and most other enemies) already wear it in the field.
constexpr NpcBody kNpcBodies[] = {
    // Solt (adk_yst).
    {0x429BF698DCC7FB04ull, 71},
    // Peppor (adk_krn).
    {0x99981075FEB15405ull, 72},
    // Acacia Dragoon private (adk_sp): the two field meshes, the second also its own field model file.
    {0x41D635593F5D3126ull, 91},
    {0x242F487538306113ull, 91},
    // Acacia Dragoon sergeant (adk_zako).
    {0xFE08794C29442551ull, 92},
    // Porre private (paresol).
    {0xE5EA5C39CDEA9926ull, 140},
    // Porre sergeant (parecap).
    {0x98250B363110C1DEull, 141},
    // Polis Police robot (robo_g).
    {0xC66ACEA1B1C230C7ull, 186},
    // Dario (dario).
    {0x0C1726D954D0CD20ull, 215},
    // Dark Serge (d_serge).
    {0x189B095F942DA422ull, 217},
    // Field enemies.
    // King Moaman (skalf).
    {0xFE724B69ACD6F1F8ull, 88},
    // Man-Of-War and Man-At-Arms (yoroi; yoroi2 is the same mesh and texture).
    {0x498A2E7954DA074Eull, 98},
    // Snob Goblin (gobu).
    {0x454F802A14957937ull, 110},
    // Crossbones (bairetta).
    {0x224DA4C8E6BE81FBull, 117},
    // Cat Burglar (tiger).
    {0x9F886DD7BA524EA2ull, 123},
    // Acacia Dragoon rider on its mount (adk_ryu).
    {0x111C0C1F852AD396ull, 134},
    // Total Chaos (k_monk).
    {0x5920EFBE3A257570ull, 139},
    // Airframe (skyf).
    {0x3BB12C7912D927F0ull, 144},
    // Robo Ducky (torimeca).
    {0x5007CCDAB88E7EFCull, 147},
    // Spearfisher (snow).
    {0xB64B9CE0296FCB9Bull, 156},
    // Rockroach (anda).
    {0x845C27B61F024DC6ull, 164},
    // Fossicker (iwao).
    {0x90296C11CDEF0C94ull, 166},
    // Lagoonate (toka).
    {0x50AC440344F3FD9Cull, 169},
    // Taurminator (leo).
    {0xC040782CE196EEE0ull, 174},
    // Pterodact (putera).
    {0x10C7A4FFEB977D33ull, 179},
    // Gizmotoid (mark).
    {0x9B92274DA53C5471ull, 187},
    // Aero-Guard (hikou).
    {0x77A342592D6BEE62ull, 188},
    // Gurgoyle (galgol).
    {0x796334492C27A4E6ull, 196},
    // Terra Terror (bannin).
    {0x0C4467665DFE87EBull, 199},
};

}  // namespace

std::span<const NpcBody> npc_bodies() { return kNpcBodies; }

int npc_body(u64 fingerprint) {
  for (const NpcBody& entry : kNpcBodies)
    if (entry.fingerprint == fingerprint) return entry.battle_model;
  return -1;
}

std::span<const HeldProp> menu_rig_props(int body) {
  if (const auto found = rig_overrides().props.find(body); found != rig_overrides().props.end()) return found->second;
  const auto first = std::ranges::find(kHeldProps, body, &HeldProp::body);
  const auto last = std::find_if(first, std::end(kHeldProps), [body](const HeldProp& prop) { return prop.body != body; });
  return {first, last};
}

const RigTuning* menu_rig_tuning(int body) {
  const RigOverrides& overrides = rig_overrides();
  const RigTuning* base = nullptr;
  if (const auto found = overrides.untuned.find(body); found != overrides.untuned.end()) {
    base = found->second ? nullptr : &overrides.tunings.at(body);
  } else if (const auto entry = std::ranges::find(kRigTunings, body, &RigTuning::body); entry != std::end(kRigTunings)) {
    base = &*entry;
  }
  const auto carry = overrides.carry.find(body), lay = overrides.lay.find(body), floor = overrides.floor.find(body), lead = overrides.lead.find(body);
  const auto outer = overrides.outer.find(body), aim = overrides.aim.find(body);
  const auto rest = overrides.rest.find(body);
  const auto posture = overrides.posture.find(body);
  if (carry == overrides.carry.end() && lay == overrides.lay.end() && posture == overrides.posture.end() && floor == overrides.floor.end() &&
      lead == overrides.lead.end() && outer == overrides.outer.end() && aim == overrides.aim.end() && rest == overrides.rest.end())
    return base;
  static std::map<int, RigTuning> merged;
  const auto [entry, added] = merged.try_emplace(body);
  if (added) {
    entry->second = base ? *base : RigTuning{body, false, false, {-1, -1, -1, -1}, {-1, -1}};
    if (carry != overrides.carry.end())
      for (size_t i = 0; i + 1 < carry->second.size() && i / 2 < entry->second.carry.size(); i += 2) entry->second.carry[i / 2] = {carry->second[i], carry->second[i + 1]};
    if (lay != overrides.lay.end())
      for (size_t i = 0; i < lay->second.size() && i < entry->second.lay.size(); ++i) entry->second.lay[i] = lay->second[i];
    if (posture != overrides.posture.end()) entry->second.posture = posture->second;
    if (floor != overrides.floor.end())
      for (size_t i = 0; i < floor->second.size() && i < entry->second.floor.size(); ++i) entry->second.floor[i] = floor->second[i];
    if (outer != overrides.outer.end())
      for (size_t i = 0; i < outer->second.size() && i < entry->second.outer.size(); ++i) entry->second.outer[i] = outer->second[i];
    if (aim != overrides.aim.end())
      for (size_t i = 0; i < aim->second.size() && i < entry->second.aim.size(); ++i) entry->second.aim[i] = aim->second[i];
    if (lead != overrides.lead.end())
      for (size_t i = 0; i + 2 < lead->second.size() && i / 3 < entry->second.lead.size(); i += 3)
        entry->second.lead[i / 3] = {lead->second[i], lead->second[i + 1], lead->second[i + 2]};
    if (rest != overrides.rest.end())
      for (size_t i = 0; i + 6 < rest->second.size() && i / 7 < entry->second.rest.size(); i += 7) {
        RigRest& fix = entry->second.rest[i / 7];
        fix.joint = static_cast<s8>(rest->second[i]);
        for (size_t axis = 0; axis < 3; ++axis) {
          fix.turn[axis] = static_cast<s16>(rest->second[i + 1 + axis]);
          fix.offset[axis] = static_cast<s16>(rest->second[i + 4 + axis]);
        }
      }
  }
  return &entry->second;
}

std::span<const s8> menu_rig_map(int body) {
  if (const auto found = rig_overrides().maps.find(body); found != rig_overrides().maps.end()) return found->second;
  for (const RigMap& entry : kRigMaps)
    if (entry.body == body) return entry.map;
  return {};
}

}  // namespace oc::game
