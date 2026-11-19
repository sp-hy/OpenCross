// glTF export: the JSON reader / writer and the GLB container round trip, then a small skinned model (three joints, a vertex split over two
// joints, an attach point, two clips and one empty clip) and a rigid weapon part written as .glb and read back: node and accessor counts,
// joint and clip names, the skin, the embedded image, and the poses at every keyframe against the engine's skinning; and the export job
// without game files. Then the import of such files as a model replacement: the round trip, the refusal of a file whose joints are not the
// model's, and the rebinding of a hand-made file (four weights, a scaled joint, a mesh node without a skin). No game files needed.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/model.h"
#include "game/model_export.h"
#include "import/gltf.h"
#include "import/gltf_model.h"

using namespace oc;
namespace gl = import::gltf;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

assets::Vec3 transform(const assets::Vec3& rot_deg, const assets::Vec3& trans, const assets::Vec3& point) {
  const assets::Mat3 rot = assets::rotation_zyx(rot_deg);
  assets::Vec3 out{};
  for (size_t row = 0; row < 3; ++row) out[row] = rot[row * 3] * point[0] + rot[row * 3 + 1] * point[1] + rot[row * 3 + 2] * point[2] + trans[row];
  return out;
}

assets::Joint joint(int parent, const assets::Vec3& rot, const assets::Vec3& trans) {
  assets::Joint out;
  out.parent = parent;
  out.rest_rot_deg = rot;
  out.rest_quat = assets::quat_zyx(rot);
  out.rest_trans = trans;
  return out;
}

assets::JointTrack constant(const assets::Joint& source) { return {{{0, source.rest_rot_deg}}, {{0, source.rest_trans}}}; }

assets::ModelAsset small_model() {
  assets::ModelAsset asset;
  asset.kind = "battle";
  asset.source = "battle/player/test/test";
  asset.skeleton.joints = {joint(-1, {0, 0, 0}, {0, -500, 0}), joint(0, {10, 20, 30}, {100, 0, 0}), joint(1, {0, 0, 45}, {0, 200, 0})};
  const auto& joints = asset.skeleton.joints;
  assets::Mesh mesh;
  // Pool: one point per joint, and one split between joints 1 and 2 whose two local positions agree in the rest pose.
  const assets::Vec3 on_two{20, 30, 40};
  const assets::Vec3 on_one = transform(joints[2].rest_rot_deg, joints[2].rest_trans, on_two);
  auto world = [&](size_t j, const assets::Vec3& point) {
    assets::Vec3 out = point;
    for (int k = static_cast<int>(j); k >= 0; k = joints[static_cast<size_t>(k)].parent)
      out = transform(joints[static_cast<size_t>(k)].rest_rot_deg, joints[static_cast<size_t>(k)].rest_trans, out);
    return out;
  };
  for (u16 j = 0; j < 3; ++j) {
    assets::SkinVertex vertex;
    vertex.source_index = j;
    vertex.joint = {j, 0};
    vertex.local[0] = {10.0f * j, 5, -7};
    vertex.bind_pos = world(j, vertex.local[0]);
    vertex.normal = {0, -127, 0};
    mesh.pool.push_back(vertex);
  }
  assets::SkinVertex split;
  split.source_index = 3;
  split.joint = {1, 2};
  split.weight = {0.25f, 0.75f};
  split.local = {on_one, on_two};
  split.bind_pos = world(1, on_one);
  split.normal = {127, 0, 0};
  split.normal_joint = 1;
  mesh.pool.push_back(split);
  for (u32 i = 0; i < 4; ++i) mesh.vertices.push_back({i, {0.125f + 0.25f * static_cast<float>(i), 0.5f}});
  mesh.indices = {0, 1, 2, 2, 1, 3};
  mesh.tri_quad = {1, 2};
  mesh.materials = {assets::Material{0, {255, 255, 255}}};
  mesh.submeshes = {{0, 0, 6}};
  asset.meshes.push_back(mesh);
  import::Image texture;
  texture.width = texture.height = 4;
  texture.pixels.assign(4 * 4 * 4, 200);
  asset.textures.push_back(texture);
  asset.main_texture_count = 1;
  asset.attach.points[1] = {{0, -40, 0}, 2, true};

  assets::AnimationClip stand;
  stand.name = "s4_00";
  stand.source = "s4";
  stand.index = 0;
  stand.frame_count = 2;
  for (const assets::Joint& source : joints) stand.tracks.push_back(constant(source));
  stand.tracks[1].rot = {{0, {10, 20, 30}}, {1 / assets::kModelAnimFps, {15, 25, 35}}};
  asset.clips.push_back(stand);

  assets::AnimationClip empty;
  empty.source = "s4";
  empty.index = 1;
  asset.clips.push_back(empty);

  // External clip 3 = id 0x83, with a 200 degree step (the long way round, cut into smaller samples) and a moving root.
  assets::AnimationClip swing;
  swing.name = "at0_03";
  swing.source = "at0";
  swing.index = 3;
  swing.frame_count = 3;
  for (const assets::Joint& source : joints) swing.tracks.push_back(constant(source));
  swing.tracks[2].rot = {{0, {0, 0, 45}}, {1 / assets::kModelAnimFps, {200, 0, 45}}, {2 / assets::kModelAnimFps, {210, 10, 45}}};
  swing.tracks[0].trans = {{0, {0, -500, 0}}, {1 / assets::kModelAnimFps, {50, -480, 0}}, {2 / assets::kModelAnimFps, {90, -450, 10}}};
  asset.clips.push_back(swing);
  return asset;
}

void test_json() {
  const auto value = gl::parse_json(R"({"a":[1,2.5,-3e2,true,null],"b":"x\"é😀","c":{}})");
  check(value.has_value(), "json parses");
  if (!value) return;
  check((*value)["a"].size() == 5 && (*value)["a"][1].number() == 2.5 && (*value)["a"][2].number() == -300, "json numbers");
  check((*value)["a"][3].flag() && (*value)["a"][4].is_null(), "json literals");
  check((*value)["b"].text() == "x\"\xC3\xA9\xF0\x9F\x98\x80", "json string escapes");
  check(gl::write_json(*value) == std::string(R"({"a":[1,2.5,-300,true,null],"b":"x\")") + "\xC3\xA9\xF0\x9F\x98\x80" + R"(","c":{}})", "json writes back");
  check(gl::write_json(gl::Value(0.1f)) == "0.1", "float written in shortest form");
  check(!gl::parse_json("{\"a\":1,}") && !gl::parse_json("[1 2]") && !gl::parse_json(""), "malformed json rejected");
  gl::Value doc = gl::Value::object();
  doc.set("asset", gl::Value::object()).set("version", "2.0");
  const Bytes bin{1, 2, 3, 4, 5};
  const Bytes glb = gl::write_glb(doc, bin);
  check(glb.size() % 4 == 0, "glb padded");
  const auto read = gl::read_glb(glb);
  check(read && read->json["asset"]["version"].text() == "2.0" && read->bin.size() == 8 && read->bin[4] == 5, "glb round trip");
  Bytes broken = glb;
  broken[0] = 'x';
  check(!gl::read_glb(broken), "bad magic rejected");
}

void test_model() {
  const assets::ModelAsset asset = small_model();
  game::GlbStats stats;
  const Bytes glb = game::model_to_glb(asset, false, &stats);
  check(stats.vertices == 4 && stats.triangles == 2 && stats.joints == 3 && stats.clips == 2 && stats.images == 1, "export stats");
  const auto read = gl::read_glb(glb);
  check(read.has_value(), "model glb reads");
  if (!read) return;
  const gl::Value& json = read->json;
  check(json["asset"]["version"].text() == "2.0", "asset version");
  // root, three joints, the attach point, the mesh node.
  check(json["nodes"].size() == 6, "node count");
  check(json["nodes"][0]["name"].text() == "root" && json["nodes"][1]["name"].text() == "joint_00" && json["nodes"][3]["name"].text() == "joint_02",
        "node names");
  check(json["nodes"][0]["children"].size() == 1 && json["nodes"][2]["children"].size() == 1, "joint hierarchy");
  check(json["skins"].size() == 1 && json["skins"][0]["joints"].size() == 3, "skin joints");
  const gl::Value& primitive = json["meshes"][0]["primitives"][0];
  check(json["accessors"][primitive["attributes"]["POSITION"].index()]["count"].index() == 4, "position count");
  check(json["accessors"][primitive["indices"].index()]["count"].index() == 6, "index count");
  check(!primitive["attributes"].has("COLOR_0"), "no vertex colours on neutral corners");
  check(json["animations"].size() == 2 && json["animations"][0]["name"].text() == "clip_00" && json["animations"][1]["name"].text() == "clip_83",
        "clip names");
  check(json["animations"][1]["extras"]["clip"].index() == 0x83 && json["animations"][1]["extras"]["source"].text() == "at0", "clip extras");
  check(json["images"].size() == 1 && json["images"][0]["mimeType"].text() == "image/png", "embedded image");
  const ByteSpan png = gl::buffer_view(*read, json["images"][0]["bufferView"].index());
  check(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G', "png bytes");
  const auto weights = gl::read_accessor(*read, primitive["attributes"]["WEIGHTS_0"].index());
  check(weights && weights->size() == 16 && std::fabs((*weights)[12] - 0.25) < 1e-6 && std::fabs((*weights)[13] - 0.75) < 1e-6, "two-joint weights");

  const game::GlbCheck result = game::check_glb(asset, glb);
  if (!result.ok()) std::printf("check: %s\n", result.error.c_str());
  check(result.ok(), "check passes");
  check(result.poses == 5, "every keyframe compared");
  check(result.joint_error < 1e-2 && result.vertex_error < 1e-2 && result.split_error < 1e-2, "poses match the engine");
  std::printf("small model: %zu bytes, joint error %.6f, vertex error %.6f / %.6f\n", glb.size(), result.joint_error, result.vertex_error, result.split_error);

  assets::WeaponPart part;
  part.texture = asset.textures[0];
  part.mesh = asset.meshes[0];
  part.mesh.skinned = false;
  for (assets::MeshVertex& vertex : part.mesh.vertices) vertex.rgb = {64, 128, 200};
  const assets::ModelAsset weapon = game::weapon_part_asset(part, "battle/wepon/test/part0");
  const Bytes weapon_glb = game::model_to_glb(weapon, true, &stats);
  const auto weapon_read = gl::read_glb(weapon_glb);
  check(weapon_read && !weapon_read->json.has("skins") && !weapon_read->json.has("animations"), "weapon is a rigid mesh");
  if (weapon_read) {
    const gl::Value& attributes = weapon_read->json["meshes"][0]["primitives"][0]["attributes"];
    const auto colour = gl::read_accessor(*weapon_read, attributes["COLOR_0"].index());
    check(colour && colour->size() == 16 && std::fabs((*colour)[0] - 128 / 255.0) < 1e-6 && (*colour)[1] == 1.0, "weapon corner colours");
    check(weapon_read->json["nodes"][0]["children"].size() == 1, "weapon mesh under the root");
  }
  check(game::check_glb(weapon, weapon_glb).ok(), "weapon check passes");
}

// The Mods tab's job on a folder without game files: the worker finishes at once with a summary and writes nothing.
void test_job() {
  game::ModelExportJob job;
  check(job.start("no such game folder"), "job starts");
  job.join();
  check(!job.running() && job.progress().finished && job.progress().files == 0, "job finishes");
  check(job.progress().summary().starts_with("model export: game files not found"), "job summary");
}

// The document of `glb` changed by `change`, written back with its binary chunk.
Bytes edited(const Bytes& glb, const std::function<void(gl::Value&)>& change) {
  const auto file = gl::read_glb(glb);
  if (!file) return {};
  gl::Value json = file->json;
  change(json);
  return gl::write_glb(json, file->bin);
}

// `list` with item `index` changed by `change`.
gl::Value rebuilt(const gl::Value& list, size_t index, const std::function<void(gl::Value&)>& change) {
  gl::Value out = gl::Value::array();
  for (size_t i = 0; i < list.size(); ++i) {
    gl::Value item = list[i];
    if (i == index) change(item);
    out.push(std::move(item));
  }
  return out;
}

size_t node_named(const gl::Value& json, std::string_view name) {
  for (size_t i = 0; i < json["nodes"].size(); ++i)
    if (json["nodes"][i]["name"].text() == name) return i;
  return static_cast<size_t>(-1);
}

std::string import_error(const Bytes& glb, const assets::Skeleton& skeleton) {
  std::string error;
  return import::load_gltf_model(glb, skeleton, error) ? std::string() : error;
}

// The exported small model read back as a replacement: the same faces, quads, joints and positions.
void test_import_round_trip() {
  const assets::ModelAsset asset = small_model();
  const Bytes glb = game::model_to_glb(asset, false);
  std::string error;
  const auto model = import::load_gltf_model(glb, asset.skeleton, error);
  check(model.has_value() && error.empty(), "exported model imports");
  if (!model || model->meshes.size() != 1 || model->meshes[0].pool.size() != 4) {
    check(false, "imported mesh shape");
    return;
  }
  const assets::Mesh& mesh = model->meshes[0];
  const assets::Mesh& source = asset.meshes[0];
  check(mesh.indices == source.indices && mesh.tri_quad == source.tri_quad && mesh.quad_count == 1, "faces and quad kept");
  check(mesh.materials.size() == 1 && mesh.materials[0].texture == 0 && model->images.size() == 1 && model->images[0].name == "clut0" &&
            !model->images[0].bytes.empty(),
        "material and embedded image");
  bool same = true;
  for (size_t i = 0; i < 3; ++i)
    for (size_t axis = 0; axis < 3; ++axis) same = same && std::fabs(mesh.pool[i].local[0][axis] - source.pool[i].local[0][axis]) < 1e-3f;
  check(same, "one-joint vertices land on the model's positions");
  check(std::abs(mesh.pool[0].normal[1] + 127) <= 1 && mesh.pool[0].normal_joint == 0, "normal back in its joint's space");
  const assets::SkinVertex& split = mesh.pool[3];
  check(split.joint == std::array<u16, 2>{1, 2} && std::fabs(split.weight[0] - 0.25f) < 1e-5f && split.normal_joint == 1,
        "two-joint vertex keeps its joints in order, normal on the second");
  check(model->dropped == 0 && model->vertices == 4 && model->triangles == 2, "counts");
  const game::ImportCheck back = game::check_import(asset, glb);
  check(back.ok() && back.faces == 0 && back.corners > 0 && back.vertex_error < 1e-3 && back.split_error < 1e-2, "import check over every pose");
}

// A file whose joints are not the model's is refused with the reason.
void test_import_refusal() {
  const assets::ModelAsset asset = small_model();
  const Bytes glb = game::model_to_glb(asset, false);
  auto rename = [&](std::string_view from, std::string to) {
    return edited(glb, [&](gl::Value& json) {
      json.set("nodes", rebuilt(json["nodes"], node_named(json, from), [&](gl::Value& node) { node.set("name", to); }));
    });
  };
  check(import_error(rename("joint_02", "bone"), asset.skeleton) == "joint_02 is missing (the model has 3 joints)", "missing joint refused");
  check(import_error(rename("joint_01", "joint_00"), asset.skeleton) == "two nodes are called joint_00", "doubled joint refused");
  check(import_error(rename("attach_01", "joint_07"), asset.skeleton) == "joint_07 is not a joint of the model (it has joint_00 to joint_02)",
        "extra joint refused");
  const Bytes moved = edited(glb, [&](gl::Value& json) {
    const size_t one = node_named(json, "joint_01"), two = node_named(json, "joint_02"), root = node_named(json, "root");
    gl::Value nodes = rebuilt(json["nodes"], one, [&](gl::Value& node) {
      gl::Value children = gl::Value::array();
      for (const gl::Value& child : node["children"].items())
        if (child.index() != two) children.push(child);
      node.set("children", std::move(children));
    });
    json.set("nodes", rebuilt(nodes, root, [&](gl::Value& node) {
      gl::Value children = node["children"];
      children.push(two);
      node.set("children", std::move(children));
    }));
  });
  check(import_error(moved, asset.skeleton) == "joint_02 hangs under no joint, the model's under joint_01", "re-parented joint refused");
  const Bytes foreign = edited(glb, [&](gl::Value& json) {
    json.set("skins", rebuilt(json["skins"], 0, [&](gl::Value& skin) {
      skin.set("joints", rebuilt(skin["joints"], 0, [](gl::Value& joint) { joint = gl::Value(0); }));
    }));
  });
  check(import_error(foreign, asset.skeleton) == "skin joint 'root' is not a joint_NN node", "skin on another node refused");
  assets::Skeleton longer = asset.skeleton;
  longer.joints.push_back(longer.joints[2]);
  check(import_error(glb, longer) == "joint_03 is missing (the model has 4 joints)", "file for a smaller skeleton refused");
  check(import_error(Bytes{1, 2, 3}, asset.skeleton) == "not a GLB file", "not a GLB");
  check(import::joint_number("joint_07") == 7 && import::joint_number("joint_") < 0 && import::joint_number("joint_7a") < 0 &&
            import::joint_number("Joint_07") < 0,
        "joint names");
}

// A hand-made file: three joints with identity rest transforms (joint_01 scaled by `scale`), one vertex on three joints, one on joint_01,
// one on joint_02, and a rigid mesh node "prop" under joint_02. No inverse bind matrices, no normals.
Bytes hand_made(double scale) {
  Bytes bin;
  gl::Value views = gl::Value::array(), accessors = gl::Value::array();
  auto add = [&](const void* data, size_t size, int component, size_t count, const char* type) {
    const auto* bytes = static_cast<const u8*>(data);
    gl::Value view = gl::Value::object();
    view.set("buffer", 0);
    view.set("byteOffset", bin.size());
    view.set("byteLength", size);
    bin.insert(bin.end(), bytes, bytes + size);
    while (bin.size() % 4) bin.push_back(0);
    views.push(std::move(view));
    gl::Value accessor = gl::Value::object();
    accessor.set("bufferView", views.size() - 1);
    accessor.set("componentType", component);
    accessor.set("count", count);
    accessor.set("type", type);
    accessors.push(std::move(accessor));
    return accessors.size() - 1;
  };
  const float positions[] = {0.1f, 0, 0, 0, 0.1f, 0, 0, 0, 0.1f};
  const u8 joints[] = {0, 1, 2, 0, 1, 0, 0, 0, 2, 0, 0, 0};
  const float weights[] = {0.5f, 0.3f, 0.2f, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  const size_t position = add(positions, sizeof(positions), gl::kFloat, 3, "VEC3");
  const size_t joint = add(joints, sizeof(joints), gl::kUnsignedByte, 3, "VEC4");
  const size_t weight = add(weights, sizeof(weights), gl::kFloat, 3, "VEC4");
  auto mesh = [&](bool skinned) {
    gl::Value attributes = gl::Value::object();
    attributes.set("POSITION", position);
    if (skinned) {
      attributes.set("JOINTS_0", joint);
      attributes.set("WEIGHTS_0", weight);
    }
    gl::Value primitive = gl::Value::object();
    primitive.set("attributes", std::move(attributes));
    gl::Value primitives = gl::Value::array();
    primitives.push(std::move(primitive));
    gl::Value out = gl::Value::object();
    out.set("primitives", std::move(primitives));
    return out;
  };
  auto node = [](const char* name, std::initializer_list<int> children) {
    gl::Value out = gl::Value::object();
    out.set("name", name);
    if (children.size()) {
      gl::Value list = gl::Value::array();
      for (const int child : children) list.push(child);
      out.set("children", std::move(list));
    }
    return out;
  };
  gl::Value nodes = gl::Value::array();
  nodes.push(node("joint_00", {1}));
  gl::Value scaled = node("joint_01", {2});
  gl::Value factors = gl::Value::array();
  for (int i = 0; i < 3; ++i) factors.push(scale);
  scaled.set("scale", std::move(factors));
  nodes.push(std::move(scaled));
  nodes.push(node("joint_02", {4}));
  gl::Value skinned = node("mesh_00", {});
  skinned.set("mesh", 0);
  skinned.set("skin", 0);
  nodes.push(std::move(skinned));
  gl::Value prop = node("prop", {});
  prop.set("mesh", 1);
  nodes.push(std::move(prop));
  gl::Value meshes = gl::Value::array();
  meshes.push(mesh(true));
  meshes.push(mesh(false));
  gl::Value skin = gl::Value::object();
  gl::Value skin_joints = gl::Value::array();
  for (int i = 0; i < 3; ++i) skin_joints.push(i);
  skin.set("joints", std::move(skin_joints));
  gl::Value skins = gl::Value::array();
  skins.push(std::move(skin));
  gl::Value buffer = gl::Value::object();
  buffer.set("byteLength", bin.size());
  gl::Value buffers = gl::Value::array();
  buffers.push(std::move(buffer));
  gl::Value json = gl::Value::object();
  json.set("asset", gl::Value::object()).set("version", "2.0");
  json.set("nodes", std::move(nodes));
  json.set("meshes", std::move(meshes));
  json.set("skins", std::move(skins));
  json.set("accessors", std::move(accessors));
  json.set("bufferViews", std::move(views));
  json.set("buffers", std::move(buffers));
  return gl::write_glb(json, bin);
}

// Four weights become the engine's two (the lightest dropped); the file's rest pose is what the model draws at rest, a scaled joint included;
// a mesh node without a skin rides its nearest joint.
void test_import_rebinding() {
  const assets::ModelAsset asset = small_model();
  const std::vector<gl::Affine> rest = import::rest_world(asset.skeleton);
  std::string error;
  const auto model = import::load_gltf_model(hand_made(2.0), asset.skeleton, error);
  check(model.has_value() && error.empty() && model->meshes.size() == 1 && model->meshes[0].pool.size() == 6 && model->triangles == 2,
        "hand-made file imports");
  if (!model || model->meshes.size() != 1 || model->meshes[0].pool.size() != 6) return;
  const assets::Mesh& mesh = model->meshes[0];
  auto at_rest = [&](const assets::SkinVertex& vertex) {
    gl::Vec3d out{};
    for (size_t k = 0; k < 2; ++k) {
      const gl::Vec3d moved = gl::apply_point(rest[vertex.joint[k]], {vertex.local[k][0], vertex.local[k][1], vertex.local[k][2]});
      for (size_t axis = 0; axis < 3; ++axis) out[axis] += vertex.weight[k] * moved[axis];
    }
    return out;
  };
  auto near = [](const gl::Vec3d& lhs, const gl::Vec3d& rhs) {
    return std::fabs(lhs[0] - rhs[0]) < 1e-3 && std::fabs(lhs[1] - rhs[1]) < 1e-3 && std::fabs(lhs[2] - rhs[2]) < 1e-3;
  };
  const assets::SkinVertex& three = mesh.pool[0];
  check(model->dropped == 1 && three.joint == std::array<u16, 2>{0, 1} && std::fabs(three.weight[0] - 0.625f) < 1e-5f &&
            std::fabs(three.weight[1] - 0.375f) < 1e-5f,
        "three joints become the two heaviest");
  check(near(at_rest(three), {150, 0, 0}), "the kept joints pose the vertex at its rest position (half on joint_00, half on the scaled joints)");
  check(near(at_rest(mesh.pool[1]), {0, -200, 0}) && mesh.pool[1].joint[0] == 1, "a scaled joint scales its vertices");
  check(near(at_rest(mesh.pool[3]), {200, 0, 0}) && mesh.pool[3].joint[0] == 2 && mesh.pool[3].weight[0] == 1.0f, "a mesh without a skin rides its joint");
  check(mesh.pool[0].normal != std::array<s8, 3>{0, 0, 0}, "missing normals filled from the faces");
  const auto plain = import::load_gltf_model(hand_made(1.0), asset.skeleton, error);
  check(plain && near(at_rest(plain->meshes[0].pool[1]), {0, -100, 0}), "unscaled joint");
}

}  // namespace

int main() {
  test_json();
  test_model();
  test_job();
  test_import_round_trip();
  test_import_refusal();
  test_import_rebinding();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("gltf export: all passed\n");
  return 0;
}
