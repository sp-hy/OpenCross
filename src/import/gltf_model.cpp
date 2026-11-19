#include "import/gltf_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <utility>

namespace oc::import {

namespace gl = gltf;
using gl::Affine;
using gl::Value;
using gl::Vec3d;

namespace {

constexpr size_t kNone = static_cast<size_t>(-1);
// ModelAsset::shading has one entry per construct.
constexpr size_t kMaxConstructs = 16;
constexpr int kModeTriangles = 4;

Affine affine_rt(const assets::Mat3& rot, const assets::Vec3& trans) {
  Affine out;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) out.at(row, col) = rot[static_cast<size_t>(row * 3 + col)];
    out.at(row, 3) = trans[static_cast<size_t>(row)];
  }
  return out;
}

// The parent node of every node (kNone for a scene root); false with `error` set when a node has two parents or the hierarchy loops.
bool node_parents(const Value& nodes, std::vector<size_t>& parent, std::string& error) {
  parent.assign(nodes.size(), kNone);
  for (size_t i = 0; i < nodes.size(); ++i)
    for (const Value& child : nodes[i]["children"].items()) {
      const size_t index = child.index();
      if (index >= nodes.size() || parent[index] != kNone || index == i) {
        error = std::format("node {} has a bad child list", i);
        return false;
      }
      parent[index] = i;
    }
  for (size_t i = 0; i < nodes.size(); ++i) {
    size_t steps = 0;
    for (size_t at = parent[i]; at != kNone; at = parent[at])
      if (++steps > nodes.size()) {
        error = "the node hierarchy loops";
        return false;
      }
  }
  return true;
}

std::string joint_label(int joint) { return joint < 0 ? std::string("no joint") : std::format("joint_{:02}", joint); }

// The ABR + 1 of a BLEND material's extras.blend name (the export's names); average when absent.
u8 blend_trans(std::string_view name) {
  if (name == "add") return 2;
  if (name == "subtract") return 3;
  if (name == "add_quarter") return 4;
  return 1;
}

int base64_digit(char letter) {
  if (letter >= 'A' && letter <= 'Z') return letter - 'A';
  if (letter >= 'a' && letter <= 'z') return letter - 'a' + 26;
  if (letter >= '0' && letter <= '9') return letter - '0' + 52;
  if (letter == '+' || letter == '-') return 62;
  if (letter == '/' || letter == '_') return 63;
  return -1;
}

Bytes base64_decode(std::string_view text) {
  Bytes out;
  u32 bits = 0;
  int count = 0;
  for (const char letter : text) {
    const int digit = base64_digit(letter);
    if (digit < 0) continue;
    bits = (bits << 6) | static_cast<u32>(digit);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(static_cast<u8>(bits >> count));
    }
  }
  return out;
}

// A URI's %XX escapes decoded.
std::string uri_path(std::string_view uri) {
  std::string out;
  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size()) {
      int value = 0;
      bool ok = true;
      for (size_t k = 1; k <= 2; ++k) {
        const char letter = uri[i + k];
        const int digit = letter >= '0' && letter <= '9' ? letter - '0' : letter >= 'a' && letter <= 'f' ? letter - 'a' + 10 : letter >= 'A' && letter <= 'F' ? letter - 'A' + 10 : -1;
        ok = ok && digit >= 0;
        value = value * 16 + digit;
      }
      if (ok) {
        out += static_cast<char>(value);
        i += 2;
        continue;
      }
    }
    out += uri[i];
  }
  return out;
}

Vec3d normalised(const Vec3d& v) {
  const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return len > 1e-12 ? Vec3d{v[0] / len, v[1] / len, v[2] / len} : Vec3d{0, 0, 0};
}

// Inverse transpose of the 3 x 3 part: turns normals.
Affine normal_matrix(const Affine& xf) {
  const Affine inv = gl::inverse(xf);
  Affine out;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) out.at(row, col) = inv.at(col, row);
    out.at(row, 3) = 0;
  }
  return out;
}

class Importer {
 public:
  Importer(const gl::Glb& glb, const assets::Skeleton& skeleton, std::vector<size_t> joint_node, std::string& error)
      : glb_(glb), json_(glb.json), skeleton_(skeleton), joint_node_(std::move(joint_node)), error_(error) {}

  std::optional<GltfModel> run() {
    const Value& nodes = json_["nodes"];
    if (!node_parents(nodes, parent_, error_)) return std::nullopt;
    joint_at_.assign(nodes.size(), -1);
    for (size_t j = 0; j < joint_node_.size(); ++j) joint_at_[joint_node_[j]] = static_cast<int>(j);
    world_.assign(nodes.size(), Affine{});
    std::vector<u8> done(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); ++i) world_of(i, done);
    from_gltf_ = gl::inverse(model_to_gltf());
    for (const Affine& rest : rest_world(skeleton_)) game_inverse_.push_back(gl::inverse(rest));
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].has("mesh") && !add_node(i)) return std::nullopt;
    if (!out_.triangles) {
      error_ = "it has no triangles";
      return std::nullopt;
    }
    for (assets::Mesh& mesh : out_.meshes) fill_missing_normals(mesh);
    return std::move(out_);
  }

 private:
  const Affine& world_of(size_t i, std::vector<u8>& done) {
    if (!done[i]) {
      const Affine local = gl::node_transform(json_["nodes"][i]);
      world_[i] = parent_[i] == kNone ? local : world_of(parent_[i], done) * local;
      done[i] = 1;
    }
    return world_[i];
  }

  std::vector<double> accessor(const Value& index, size_t count, u32 components, const char* what) {
    if (!index.is_number()) return {};
    u32 got = 0;
    std::optional<std::vector<double>> values = gl::read_accessor(glb_, index.index(), &got);
    if (!values || got != components || (count != kNone && values->size() != count * components)) {
      if (error_.empty()) error_ = std::format("its {} accessor {} cannot be read", what, index.index());
      return {};
    }
    return std::move(*values);
  }

  // glTF image `image` as an entry of out_.images (once).
  int image(size_t image_index) {
    if (const auto it = image_of_.find(image_index); it != image_of_.end()) return it->second;
    const Value& entry = json_["images"][image_index];
    GltfImage out;
    out.name = entry["name"].text();
    if (entry.has("bufferView")) {
      const ByteSpan bytes = gl::buffer_view(glb_, entry["bufferView"].index());
      out.bytes.assign(bytes.begin(), bytes.end());
    } else if (const std::string& uri = entry["uri"].text(); uri.starts_with("data:")) {
      const size_t comma = uri.find(',');
      if (comma != std::string::npos) out.bytes = base64_decode(std::string_view(uri).substr(comma + 1));
    } else {
      out.uri = uri_path(uri);
    }
    if (out.bytes.empty() && out.uri.empty()) {
      error_ = std::format("image {} has no data", image_index);
      return -1;
    }
    out_.images.push_back(std::move(out));
    const int index = static_cast<int>(out_.images.size() - 1);
    image_of_[image_index] = index;
    return index;
  }

  struct Look {
    assets::Material material;
    u8 trans = 0;
  };

  // A primitive's material: its base colour texture (or flat colour) and its blending; nullopt (error set) when its image is missing.
  std::optional<Look> look(const Value& primitive) {
    Look out;
    if (!primitive.has("material")) return out;
    const Value& material = json_["materials"][primitive["material"].index()];
    if (!material.is_object()) {
      error_ = std::format("material {} does not exist", primitive["material"].index());
      return std::nullopt;
    }
    const Value& pbr = material["pbrMetallicRoughness"];
    const Value& factor = pbr["baseColorFactor"];
    if (material["alphaMode"].text() == "BLEND") out.trans = blend_trans(material["extras"]["blend"].text());
    if (pbr.has("baseColorTexture")) {
      const Value& texture = json_["textures"][pbr["baseColorTexture"]["index"].index()];
      if (!texture.is_object() || !texture.has("source") || texture["source"].index() >= json_["images"].size()) {
        error_ = std::format("material {} has a texture without an image", primitive["material"].index());
        return std::nullopt;
      }
      out.material.texture = image(texture["source"].index());
      if (out.material.texture < 0) return std::nullopt;
      if (out.trans) out_.images[static_cast<size_t>(out.material.texture)].blend = true;
    } else {
      for (size_t k = 0; k < 3; ++k)
        out.material.color[k] = static_cast<u8>(std::clamp(std::lround(factor[k].number(1) * 255.0), 0l, 255l));
    }
    return out;
  }

  // The vertex transforms of a mesh node: the skin's joints (file transform at rest times inverse bind matrix, in model space) and their
  // skeleton joints, or the node's own transform on its nearest joint ancestor (joint 0 when it has none).
  bool binding(size_t node_index, std::vector<Affine>& transforms, std::vector<u16>& joints) {
    const Value& node = json_["nodes"][node_index];
    if (!node.has("skin")) {
      size_t at = node_index;
      while (at != kNone && joint_at_[at] < 0) at = parent_[at];
      transforms = {from_gltf_ * world_[node_index]};
      joints = {static_cast<u16>(at == kNone ? 0 : joint_at_[at])};
      return true;
    }
    const Value& skin = json_["skins"][node["skin"].index()];
    const size_t count = skin["joints"].size();
    if (!skin.is_object() || !count) {
      error_ = std::format("node {} has no usable skin", node["name"].text());
      return false;
    }
    std::vector<double> ibm;
    if (skin.has("inverseBindMatrices")) {
      ibm = accessor(skin["inverseBindMatrices"], count, 16, "inverse bind matrix");
      if (ibm.empty()) return false;
    }
    for (size_t k = 0; k < count; ++k) {
      const size_t joint_node = skin["joints"][k].index();
      if (joint_node >= joint_at_.size() || joint_at_[joint_node] < 0) {
        const std::string name = joint_node < joint_at_.size() ? json_["nodes"][joint_node]["name"].text() : std::string("?");
        error_ = std::format("skin joint '{}' is not a joint_NN node", name);
        return false;
      }
      Affine bind;
      if (!ibm.empty())
        for (int col = 0; col < 4; ++col)
          for (int row = 0; row < 3; ++row) bind.at(row, col) = ibm[k * 16 + static_cast<size_t>(col * 4 + row)];
      transforms.push_back(from_gltf_ * world_[joint_node] * bind);
      joints.push_back(static_cast<u16>(joint_at_[joint_node]));
    }
    return true;
  }

  // The vertices of one attribute set of a node into `mesh`: pool entries and corners, one each per glTF vertex. `skinned`: JOINTS_n /
  // WEIGHTS_n index `transforms`; otherwise every vertex takes transforms[0]. Returns the first.
  std::optional<u32> add_vertices(assets::Mesh& mesh, const Value& attributes, const std::vector<Affine>& transforms, const std::vector<u16>& joints,
                                  bool skinned) {
    const std::vector<double> position = accessor(attributes["POSITION"], kNone, 3, "POSITION");
    if (position.empty()) {
      if (error_.empty()) error_ = "a primitive has no POSITION";
      return std::nullopt;
    }
    const size_t count = position.size() / 3;
    const std::vector<double> normal = accessor(attributes["NORMAL"], count, 3, "NORMAL");
    const std::vector<double> uv = accessor(attributes["TEXCOORD_0"], count, 2, "TEXCOORD_0");
    u32 color_width = 4;
    std::vector<double> color;
    if (attributes.has("COLOR_0")) {
      color_width = gl::type_components(json_["accessors"][attributes["COLOR_0"].index()]["type"].text());
      color = accessor(attributes["COLOR_0"], count, color_width == 3 ? 3 : 4, "COLOR_0");
    }
    std::array<std::vector<double>, 2> joint_sets, weight_sets;
    if (skinned)
      for (size_t set = 0; set < 2; ++set) {
        const std::string suffix = std::to_string(set);
        if (!attributes.has("JOINTS_" + suffix)) continue;
        joint_sets[set] = accessor(attributes["JOINTS_" + suffix], count, 4, "JOINTS");
        weight_sets[set] = accessor(attributes["WEIGHTS_" + suffix], count, 4, "WEIGHTS");
      }
    if (!error_.empty()) return std::nullopt;
    const u32 first = static_cast<u32>(mesh.pool.size());
    for (size_t i = 0; i < count; ++i) {
      // Influences by skeleton joint (several skin joints may name one), in the file's order.
      std::vector<std::pair<u16, double>> influences;
      Vec3d pos{};
      Affine blend;
      blend.cells.fill(0);
      double total = 0;
      const Vec3d point{position[i * 3], position[i * 3 + 1], position[i * 3 + 2]};
      auto add = [&](size_t k, double w) {
        if (w <= 0 || k >= transforms.size()) return;
        const Vec3d moved = gl::apply_point(transforms[k], point);
        for (size_t axis = 0; axis < 3; ++axis) pos[axis] += w * moved[axis];
        for (size_t cell = 0; cell < blend.cells.size(); ++cell) blend.cells[cell] += w * transforms[k].cells[cell];
        const auto same = std::ranges::find(influences, joints[k], &std::pair<u16, double>::first);
        if (same != influences.end()) same->second += w;
        else influences.emplace_back(joints[k], w);
        total += w;
      };
      for (size_t set = 0; set < 2; ++set)
        for (size_t k = 0; k < 4 && !joint_sets[set].empty() && !weight_sets[set].empty(); ++k)
          add(static_cast<size_t>(joint_sets[set][i * 4 + k]), weight_sets[set][i * 4 + k]);
      if (total <= 0) {
        influences.clear();
        pos = {};
        blend.cells.fill(0);
        total = 0;
        add(0, 1);
      }
      for (double& component : pos) component /= total;
      for (double& cell : blend.cells) cell /= total;
      // The engine keeps two: the lightest go (the two kept stay in the file's order).
      if (influences.size() > 2) ++out_.dropped;
      while (influences.size() > 2) influences.erase(std::ranges::min_element(influences, {}, &std::pair<u16, double>::second));
      const double kept = influences[0].second + (influences.size() > 1 ? influences[1].second : 0.0);
      assets::SkinVertex vertex;
      vertex.source_index = static_cast<u32>(mesh.pool.size());
      for (size_t axis = 0; axis < 3; ++axis) vertex.bind_pos[axis] = static_cast<float>(pos[axis]);
      for (size_t k = 0; k < 2; ++k) {
        const bool own = k < influences.size();
        vertex.joint[k] = influences[own ? k : 0].first;
        vertex.weight[k] = own ? static_cast<float>(influences[k].second / kept) : 0.0f;
        const size_t joint = vertex.joint[k];
        const Vec3d local = joint < game_inverse_.size() ? gl::apply_point(game_inverse_[joint], pos) : pos;
        for (size_t axis = 0; axis < 3; ++axis) vertex.local[k][axis] = static_cast<float>(local[axis]);
      }
      if (!normal.empty()) {
        const Vec3d turned = normalised(gl::apply_vector(normal_matrix(blend), {normal[i * 3], normal[i * 3 + 1], normal[i * 3 + 2]}));
        set_normal(vertex, turned);
      }
      mesh.pool.push_back(vertex);
      assets::MeshVertex corner;
      corner.pool = static_cast<u32>(mesh.pool.size() - 1);
      if (!uv.empty()) corner.uv = {static_cast<float>(uv[i * 2]), static_cast<float>(uv[i * 2 + 1])};
      if (!color.empty())
        for (size_t k = 0; k < 3; ++k)
          corner.rgb[k] = static_cast<u8>(std::clamp(std::lround(color[i * (color_width == 3 ? 3 : 4) + k] * 128.0), 0l, 255l));
      mesh.vertices.push_back(corner);
    }
    return first;
  }

  // `model_normal` (model space) into the space of the joint the skinner turns it with, as an s8 record: the second joint of a two-joint
  // vertex, as the game's 16-byte vertices have it, else the only one.
  void set_normal(assets::SkinVertex& vertex, const Vec3d& model_normal) const {
    vertex.normal_joint = vertex.weight[1] > 0 ? 1 : 0;
    const size_t joint = vertex.joint[vertex.normal_joint];
    const Vec3d local = normalised(joint < game_inverse_.size() ? gl::apply_vector(game_inverse_[joint], model_normal) : model_normal);
    for (size_t axis = 0; axis < 3; ++axis) vertex.normal[axis] = static_cast<s8>(std::clamp(std::lround(local[axis] * 127.0), -127l, 127l));
  }

  bool add_node(size_t node_index) {
    const Value& node = json_["nodes"][node_index];
    const std::string& name = node["name"].text();
    size_t construct = 0;
    if (name.size() > 5 && name.starts_with("mesh_") && std::all_of(name.begin() + 5, name.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
      construct = name.size() - 5 > 3 ? kMaxConstructs : static_cast<size_t>(std::stoi(name.substr(5)));
      if (construct >= kMaxConstructs) {
        error_ = std::format("node {}: a model has at most {} constructs (mesh_00 to mesh_{:02})", name, kMaxConstructs, kMaxConstructs - 1);
        return false;
      }
    }
    const Value& mesh_entry = json_["meshes"][node["mesh"].index()];
    if (!mesh_entry.is_object()) {
      error_ = std::format("node {} names a missing mesh", name);
      return false;
    }
    std::vector<Affine> transforms;
    std::vector<u16> joints;
    if (!binding(node_index, transforms, joints)) return false;
    if (out_.meshes.size() <= construct) out_.meshes.resize(construct + 1);
    assets::Mesh& mesh = out_.meshes[construct];
    // The pool range of each attribute set, once (the export's primitives of a mesh share one).
    std::map<std::string, std::pair<u32, u32>> range_of;
    const bool skinned = node.has("skin");
    for (const Value& primitive : mesh_entry["primitives"].items()) {
      if (primitive["mode"].number(kModeTriangles) != kModeTriangles) continue;
      const Value& attributes = primitive["attributes"];
      const std::string key = gl::write_json(attributes);
      if (!range_of.contains(key)) {
        const std::optional<u32> added = add_vertices(mesh, attributes, transforms, joints, skinned);
        if (!added) return false;
        range_of[key] = {*added, static_cast<u32>(mesh.pool.size()) - *added};
      }
      const auto [first, count] = range_of[key];
      std::vector<double> indices;
      if (primitive.has("indices")) {
        indices = accessor(primitive["indices"], kNone, 1, "index");
        if (!error_.empty()) return false;
      } else {
        for (u32 i = 0; i < count; ++i) indices.push_back(i);
      }
      const std::optional<Look> found = look(primitive);
      if (!found) return false;
      if (found->trans && !mesh.trans) mesh.trans = found->trans;
      u32 slot = 0;
      while (slot < mesh.materials.size() && !(mesh.materials[slot] == found->material)) ++slot;
      if (slot == mesh.materials.size()) mesh.materials.push_back(found->material);
      assets::SubMesh sub;
      sub.material = slot;
      sub.first_index = static_cast<u32>(mesh.indices.size());
      const u32 first_tri = sub.first_index / 3;
      for (size_t k = 0; k + 2 < indices.size(); k += 3) {
        std::array<u32, 3> corner{};
        for (size_t c = 0; c < 3; ++c) {
          const double value = indices[k + c];
          if (value < 0 || value >= count) {
            error_ = std::format("node {}: a triangle index is out of range", name);
            return false;
          }
          corner[c] = first + static_cast<u32>(value);
        }
        // glTF's front face runs counter-clockwise; the engine's faces are wound the other way.
        mesh.indices.insert(mesh.indices.end(), {corner[0], corner[2], corner[1]});
        mesh.tri_quad.push_back(0);
      }
      // extras.quads: the triangles (of this primitive) that start a quad the engine split as (v0, v1, v2), (v2, v1, v3); the PS1 culls
      // a quad as a whole. A pair whose corners are not shared that way stays two triangles.
      for (const Value& quad : primitive["extras"]["quads"].items()) {
        const size_t tri = first_tri + quad.index(mesh.tri_quad.size());
        if (tri + 1 >= mesh.tri_quad.size() || mesh.tri_quad[tri] || mesh.tri_quad[tri + 1]) continue;
        if (mesh.indices[tri * 3 + 3] != mesh.indices[tri * 3 + 2] || mesh.indices[tri * 3 + 4] != mesh.indices[tri * 3 + 1]) continue;
        mesh.tri_quad[tri] = 1, mesh.tri_quad[tri + 1] = 2;
        ++mesh.quad_count;
      }
      sub.index_count = static_cast<u32>(mesh.indices.size()) - sub.first_index;
      out_.triangles += sub.index_count / 3;
      if (sub.index_count) mesh.submeshes.push_back(sub);
    }
    out_.vertices = 0;
    for (const assets::Mesh& each : out_.meshes) out_.vertices += static_cast<u32>(each.pool.size());
    return true;
  }

  // Vertices without a NORMAL get the mean of their triangles' facing.
  void fill_missing_normals(assets::Mesh& mesh) const {
    std::vector<Vec3d> sum(mesh.pool.size(), Vec3d{0, 0, 0});
    std::vector<u8> missing(mesh.pool.size(), 0);
    bool any = false;
    for (size_t i = 0; i < mesh.pool.size(); ++i)
      if (mesh.pool[i].normal == std::array<s8, 3>{0, 0, 0}) missing[i] = 1, any = true;
    if (!any) return;
    for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
      std::array<Vec3d, 3> corner;
      std::array<u32, 3> pool{};
      for (size_t c = 0; c < 3; ++c) {
        pool[c] = mesh.vertices[mesh.indices[k + c]].pool;
        const assets::Vec3& at = mesh.pool[pool[c]].bind_pos;
        corner[c] = {at[0], at[1], at[2]};
      }
      // Engine order (v0, v2, v1 of the file's counter-clockwise face): the file's front side is e2 x e1.
      const Vec3d e1{corner[1][0] - corner[0][0], corner[1][1] - corner[0][1], corner[1][2] - corner[0][2]};
      const Vec3d e2{corner[2][0] - corner[0][0], corner[2][1] - corner[0][1], corner[2][2] - corner[0][2]};
      const Vec3d face{e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};
      for (const u32 at : pool)
        for (size_t axis = 0; axis < 3; ++axis) sum[at][axis] += face[axis];
    }
    for (size_t i = 0; i < mesh.pool.size(); ++i)
      if (missing[i]) set_normal(mesh.pool[i], normalised(sum[i]));
  }

  const gl::Glb& glb_;
  const Value& json_;
  const assets::Skeleton& skeleton_;
  std::vector<size_t> joint_node_;
  std::string& error_;
  std::vector<size_t> parent_;
  std::vector<int> joint_at_;
  std::vector<Affine> world_;
  Affine from_gltf_;
  std::vector<Affine> game_inverse_;
  std::map<size_t, int> image_of_;
  GltfModel out_;
};

}  // namespace

Affine model_to_gltf() { return gl::affine_trs({0, 0, 0}, {1, 0, 0, 0}, {kGltfScale, kGltfScale, kGltfScale}); }

std::vector<Affine> rest_world(const assets::Skeleton& skeleton) {
  std::vector<Affine> world(skeleton.joints.size());
  for (size_t j = 0; j < skeleton.joints.size(); ++j) {
    const assets::Joint& joint = skeleton.joints[j];
    const Affine local = affine_rt(assets::rotation_zyx(joint.rest_rot_deg), joint.rest_trans);
    world[j] = joint.parent >= 0 && static_cast<size_t>(joint.parent) < j ? world[static_cast<size_t>(joint.parent)] * local : local;
  }
  return world;
}

int joint_number(std::string_view name) {
  constexpr std::string_view kPrefix = "joint_";
  if (name.size() <= kPrefix.size() || name.size() > kPrefix.size() + 4 || !name.starts_with(kPrefix)) return -1;
  int number = 0;
  for (const char ch : name.substr(kPrefix.size())) {
    if (ch < '0' || ch > '9') return -1;
    number = number * 10 + (ch - '0');
  }
  return number;
}

std::vector<size_t> match_joints(const Value& json, const assets::Skeleton& skeleton, std::string& error) {
  const Value& nodes = json["nodes"];
  std::vector<size_t> parent;
  if (!node_parents(nodes, parent, error)) return {};
  const size_t count = skeleton.joints.size();
  if (!count) {
    error = "the model has no skeleton";
    return {};
  }
  std::vector<size_t> node_of(count, kNone);
  std::vector<int> joint_at(nodes.size(), -1);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const int joint = joint_number(nodes[i]["name"].text());
    if (joint < 0) continue;
    if (static_cast<size_t>(joint) >= count) {
      error = std::format("{} is not a joint of the model (it has joint_00 to {})", joint_label(joint), joint_label(static_cast<int>(count) - 1));
      return {};
    }
    if (node_of[static_cast<size_t>(joint)] != kNone) {
      error = std::format("two nodes are called {}", joint_label(joint));
      return {};
    }
    node_of[static_cast<size_t>(joint)] = i;
    joint_at[i] = joint;
  }
  for (size_t j = 0; j < count; ++j)
    if (node_of[j] == kNone) {
      error = std::format("{} is missing (the model has {} joints)", joint_label(static_cast<int>(j)), count);
      return {};
    }
  for (size_t j = 0; j < count; ++j) {
    size_t at = parent[node_of[j]];
    while (at != kNone && joint_at[at] < 0) at = parent[at];
    const int found = at == kNone ? -1 : joint_at[at];
    const int expected = skeleton.joints[j].parent >= 0 ? skeleton.joints[j].parent : -1;
    if (found != expected) {
      error = std::format("{} hangs under {}, the model's under {}", joint_label(static_cast<int>(j)), joint_label(found), joint_label(expected));
      return {};
    }
  }
  return node_of;
}

std::optional<GltfModel> load_gltf_model(ByteSpan glb, const assets::Skeleton& skeleton, std::string& error) {
  error.clear();
  const std::optional<gl::Glb> file = gl::read_glb(glb, &error);
  if (!file) return std::nullopt;
  std::vector<size_t> joint_node = match_joints(file->json, skeleton, error);
  if (joint_node.empty()) return std::nullopt;
  return Importer(*file, skeleton, std::move(joint_node), error).run();
}

}  // namespace oc::import
