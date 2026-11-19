#include "game/model_export.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "core/log.h"
#include "game/anim_arc.h"
#include "game/battle/model_table.h"
#include "game/model_files.h"
#include "import/gltf.h"
#include "import/gltf_model.h"
#include "import/model.h"
#include "render/mat4.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace oc::game {

namespace {

namespace gl = import::gltf;
using gl::Value;
using gl::Affine;
using gl::affine_trs;
using gl::Quat;
using gl::Vec3d;

constexpr int kTargetVertices = 34962, kTargetIndices = 34963;
constexpr int kFilterNearest = 9728, kFilterLinear = 9729, kWrapClamp = 33071;
// A keyframe step larger than this on any axis is cut into smaller ones, so that the quaternion interpolation between samples follows
// the engine's per-axis angle interpolation (which takes the long way round between 180 and 270 degrees).
constexpr double kMaxStepDegrees = 60.0;

Vec3d vec3d(const assets::Vec3& v) { return {v[0], v[1], v[2]}; }

Value floats(std::initializer_list<double> values) {
  Value out = Value::array();
  for (const double value : values) out.push(static_cast<float>(value));
  return out;
}

Value column_major(const Affine& xf) {
  Value out = Value::array();
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) out.push(static_cast<float>(row == 3 ? (col == 3 ? 1.0 : 0.0) : xf.at(row, col)));
  return out;
}

void png_sink(void* context, void* data, int size) {
  auto* out = static_cast<Bytes*>(context);
  const auto* bytes = static_cast<const u8*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

Bytes encode_png(const import::Image& image) {
  Bytes out;
  if (image.indexed || !image.width || !image.height || image.pixels.size() < size_t(image.width) * image.height * 4) return out;
  const std::vector<u8> rgba = render::model_file_pixels(image.width, image.height, image.pixels.data());
  if (!stbi_write_png_to_func(png_sink, &out, static_cast<int>(image.width), static_cast<int>(image.height), 4, rgba.data(), static_cast<int>(image.width * 4)))
    out.clear();
  return out;
}

const char* blend_name(u8 trans) {
  static constexpr const char* kNames[] = {"average", "add", "subtract", "add_quarter"};
  return trans >= 1 && trans <= 4 ? kNames[trans - 1] : "none";
}

// Accumulates the binary chunk and the document's top-level arrays.
class Document {
 public:
  Value accessors = Value::array(), views = Value::array(), meshes = Value::array(), materials = Value::array();
  std::vector<Value> nodes;
  Value textures = Value::array(), images = Value::array(), samplers = Value::array(), skins = Value::array(), animations = Value::array();
  Bytes bin;

  size_t view(const void* data, size_t size, int target) {
    while (bin.size() % 4) bin.push_back(0);
    Value entry = Value::object();
    entry.set("buffer", 0);
    entry.set("byteOffset", bin.size());
    entry.set("byteLength", size);
    if (target) entry.set("target", target);
    const auto* bytes = static_cast<const u8*>(data);
    bin.insert(bin.end(), bytes, bytes + size);
    views.push(std::move(entry));
    return views.size() - 1;
  }

  // Float accessor of `values` (count x components); min / max are written when asked (POSITION, animation inputs).
  size_t float_accessor(const std::vector<float>& values, u32 components, const char* type, int target, bool bounds) {
    Value entry = Value::object();
    entry.set("bufferView", view(values.data(), values.size() * sizeof(float), target));
    entry.set("componentType", gl::kFloat);
    entry.set("count", values.size() / components);
    entry.set("type", type);
    if (bounds && !values.empty()) {
      Value low = Value::array(), high = Value::array();
      for (u32 k = 0; k < components; ++k) {
        float lo = values[k], hi = values[k];
        for (size_t i = k; i < values.size(); i += components) lo = std::min(lo, values[i]), hi = std::max(hi, values[i]);
        low.push(lo);
        high.push(hi);
      }
      entry.set("min", std::move(low));
      entry.set("max", std::move(high));
    }
    accessors.push(std::move(entry));
    return accessors.size() - 1;
  }

  template <typename T>
  size_t int_accessor(const std::vector<T>& values, int component_type, u32 components, const char* type, int target, bool normalized) {
    Value entry = Value::object();
    entry.set("bufferView", view(values.data(), values.size() * sizeof(T), target));
    entry.set("componentType", component_type);
    if (normalized) entry.set("normalized", true);
    entry.set("count", values.size() / components);
    entry.set("type", type);
    accessors.push(std::move(entry));
    return accessors.size() - 1;
  }

  size_t node(Value entry) {
    nodes.push_back(std::move(entry));
    return nodes.size() - 1;
  }
};

struct MaterialKey {
  int texture;
  std::array<u8, 3> color;
  u8 trans;
  bool operator<(const MaterialKey& other) const { return std::tie(texture, color, trans) < std::tie(other.texture, other.color, other.trans); }
};

// Builds the whole document of one model.
class Exporter {
 public:
  Exporter(const assets::ModelAsset& asset, bool hd) : asset_(asset), hd_(hd) {}

  Bytes run(GlbStats* stats) {
    const size_t joints = asset_.skeleton.joints.size();
    const bool skinned = joints > 0 && std::ranges::any_of(asset_.meshes, [](const assets::Mesh& mesh) { return mesh.skinned && !mesh.indices.empty(); });
    world_ = import::rest_world(asset_.skeleton);
    const size_t root = doc_.node(root_node());
    std::vector<Value> children(joints + 1, Value::array());
    for (size_t j = 0; j < joints; ++j) {
      const assets::Joint& joint = asset_.skeleton.joints[j];
      Value entry = Value::object();
      entry.set("name", std::format("joint_{:02}", j));
      entry.set("rotation", floats({joint.rest_quat[0], joint.rest_quat[1], joint.rest_quat[2], joint.rest_quat[3]}));
      entry.set("translation", floats({joint.rest_trans[0], joint.rest_trans[1], joint.rest_trans[2]}));
      if (joint.bone >= 0) entry.set("extras", Value::object()).set("bone", joint.bone);
      const size_t index = doc_.node(std::move(entry));
      joint_nodes_.push_back(index);
      const size_t parent = joint.parent >= 0 && static_cast<size_t>(joint.parent) < j ? static_cast<size_t>(joint.parent) + 1 : 0;
      children[parent].push(index);
    }
    add_attach_nodes(children);
    std::vector<size_t> scene_nodes{root};
    for (size_t mesh_index = 0; mesh_index < asset_.meshes.size(); ++mesh_index) {
      const assets::Mesh& mesh = asset_.meshes[mesh_index];
      if (mesh.indices.empty() || mesh.vertices.empty()) continue;
      const bool skin = skinned && mesh.skinned;
      Value entry = Value::object();
      entry.set("name", std::format("mesh_{:02}", mesh_index));
      entry.set("mesh", add_mesh(mesh, mesh_index, skin));
      if (skin) entry.set("skin", 0);
      const size_t index = doc_.node(std::move(entry));
      if (skin) scene_nodes.push_back(index);
      else children[0].push(index);
      stats_.vertices += static_cast<u32>(mesh.vertices.size());
      stats_.triangles += static_cast<u32>(mesh.indices.size() / 3);
    }
    if (children[0].size()) doc_.nodes[root].set("children", std::move(children[0]));
    for (size_t j = 0; j < joints; ++j)
      if (children[j + 1].size()) doc_.nodes[joint_nodes_[j]].set("children", std::move(children[j + 1]));
    if (skinned) add_skin();
    for (const assets::AnimationClip& clip : asset_.clips) add_animation(clip);
    stats_.joints = static_cast<u32>(joints);
    stats_.clips = static_cast<u32>(doc_.animations.size());
    stats_.images = static_cast<u32>(doc_.images.size());
    if (stats) *stats = stats_;
    return gl::write_glb(document(scene_nodes), doc_.bin);
  }

 private:
  Value root_node() const {
    Value entry = Value::object();
    entry.set("name", "root");
    entry.set("rotation", floats({1, 0, 0, 0}));
    entry.set("scale", floats({kGltfScale, kGltfScale, kGltfScale}));
    return entry;
  }

  // The used attach points (attach_<kk>) and weapon mounts (mount_<k>) as empty nodes under their joints.
  void add_attach_nodes(std::vector<Value>& children) {
    const size_t joints = asset_.skeleton.joints.size();
    for (size_t i = 0; i < asset_.attach.points.size(); ++i) {
      const assets::AttachPoint& point = asset_.attach.points[i];
      if (!point.used || point.joint >= joints) continue;
      Value entry = Value::object();
      entry.set("name", std::format("attach_{:02}", i));
      entry.set("translation", floats({point.pos[0], point.pos[1], point.pos[2]}));
      children[point.joint + 1u].push(doc_.node(std::move(entry)));
    }
    for (size_t k = 0; k < asset_.attach.mounts.size(); ++k) {
      const assets::MountPoint& mount = asset_.attach.mounts[k];
      if (!mount.used) continue;
      const assets::AttachPoint& point = asset_.attach.points[mount.attach];
      if (point.joint >= joints) continue;
      constexpr float kTurnDegrees = 360.0f / 4096.0f;
      const auto quat = assets::quat_zyx({mount.rot[0] * kTurnDegrees, mount.rot[1] * kTurnDegrees, mount.rot[2] * kTurnDegrees});
      Value entry = Value::object();
      entry.set("name", std::format("mount_{}", k));
      entry.set("rotation", floats({quat[0], quat[1], quat[2], quat[3]}));
      entry.set("translation", floats({point.pos[0] + mount.pos[0], point.pos[1] + mount.pos[1], point.pos[2] + mount.pos[2]}));
      children[point.joint + 1u].push(doc_.node(std::move(entry)));
    }
  }

  // Texture `index` of the model as a glTF texture (once), -1 when it has no RGBA image.
  int texture(int index) {
    if (index < 0 || static_cast<size_t>(index) >= asset_.textures.size()) return -1;
    if (const auto it = texture_of_.find(index); it != texture_of_.end()) return it->second;
    const import::Image& image = asset_.texture_for(static_cast<size_t>(index), hd_);
    const Bytes png = encode_png(image);
    int out = -1;
    if (!png.empty()) {
      if (!doc_.samplers.size()) {
        Value sampler = Value::object();
        const int filter = hd_ ? kFilterLinear : kFilterNearest;
        sampler.set("magFilter", filter);
        sampler.set("minFilter", filter);
        sampler.set("wrapS", kWrapClamp);
        sampler.set("wrapT", kWrapClamp);
        doc_.samplers.push(std::move(sampler));
      }
      Value entry = Value::object();
      entry.set("name", index < static_cast<int>(asset_.main_texture_count) ? std::format("clut{}", index) : std::format("overlay{}", index - static_cast<int>(asset_.main_texture_count)));
      entry.set("mimeType", "image/png");
      entry.set("bufferView", doc_.view(png.data(), png.size(), 0));
      doc_.images.push(std::move(entry));
      Value tex = Value::object();
      tex.set("sampler", 0);
      tex.set("source", doc_.images.size() - 1);
      doc_.textures.push(std::move(tex));
      out = static_cast<int>(doc_.textures.size() - 1);
    }
    texture_of_[index] = out;
    return out;
  }

  size_t material(const assets::Material& source, u8 trans) {
    const MaterialKey key{source.texture, source.color, trans};
    if (const auto it = material_of_.find(key); it != material_of_.end()) return it->second;
    const int tex = texture(source.texture);
    Value pbr = Value::object();
    const double alpha = trans >= 1 && trans <= 4 ? 0.5 : 1.0;
    if (tex >= 0) {
      pbr.set("baseColorTexture", Value::object()).set("index", tex);
      if (alpha < 1) pbr.set("baseColorFactor", floats({1, 1, 1, alpha}));
    } else {
      pbr.set("baseColorFactor", floats({source.color[0] / 255.0, source.color[1] / 255.0, source.color[2] / 255.0, alpha}));
    }
    pbr.set("metallicFactor", 0);
    pbr.set("roughnessFactor", 1);
    Value entry = Value::object();
    entry.set("name", std::format("material_{:02}", doc_.materials.size()));
    entry.set("pbrMetallicRoughness", std::move(pbr));
    if (alpha < 1) {
      entry.set("alphaMode", "BLEND");
    } else if (tex >= 0) {
      entry.set("alphaMode", "MASK");
      entry.set("alphaCutoff", 0.5);
    }
    entry.set("extensions", Value::object()).set("KHR_materials_unlit", Value::object());
    if (alpha < 1) entry.set("extras", Value::object()).set("blend", blend_name(trans));
    doc_.materials.push(std::move(entry));
    material_of_[key] = doc_.materials.size() - 1;
    return doc_.materials.size() - 1;
  }

  size_t add_mesh(const assets::Mesh& mesh, size_t mesh_index, bool skin) {
    const Affine convert = import::model_to_gltf();
    const size_t count = mesh.vertices.size();
    std::vector<float> position, normal, uv, weight;
    std::vector<u8> joint8, color;
    std::vector<u16> joint16;
    const bool wide_joints = asset_.skeleton.joints.size() > 256;
    const bool colored = std::ranges::any_of(mesh.vertices, [](const assets::MeshVertex& vertex) { return vertex.rgb != std::array<u8, 3>{128, 128, 128}; });
    for (const assets::MeshVertex& vertex : mesh.vertices) {
      const assets::SkinVertex& pool = mesh.pool[std::min<size_t>(vertex.pool, mesh.pool.size() - 1)];
      const Vec3d bind = vec3d(pool.bind_pos);
      const Vec3d pos = skin ? apply_point(convert, bind) : bind;
      Vec3d dir{pool.normal[0] / 127.0, pool.normal[1] / 127.0, pool.normal[2] / 127.0};
      if (skin) {
        const size_t joint = pool.joint[std::min<size_t>(pool.normal_joint, 1)];
        if (joint < world_.size()) dir = apply_vector(world_[joint], dir);
        dir = apply_vector(convert, dir);
      }
      double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      if (len <= 1e-9) {
        dir = {0, skin ? 1.0 : -1.0, 0};
        len = 1;
      }
      for (size_t k = 0; k < 3; ++k) {
        position.push_back(static_cast<float>(pos[k]));
        normal.push_back(static_cast<float>(dir[k] / len));
      }
      uv.push_back(vertex.uv[0]);
      uv.push_back(vertex.uv[1]);
      if (colored)
        for (size_t k = 0; k < 4; ++k) color.push_back(k < 3 ? static_cast<u8>(std::min(255, vertex.rgb[k] * 2)) : u8{255});
      if (skin) {
        for (size_t k = 0; k < 4; ++k) {
          const u16 joint = k < 2 ? pool.joint[k] : u16{0};
          const float w = k < 2 ? pool.weight[k] : 0.0f;
          if (wide_joints) joint16.push_back(w > 0 ? joint : u16{0});
          else joint8.push_back(static_cast<u8>(w > 0 ? joint : 0));
          weight.push_back(w);
        }
      }
    }
    Value attributes = Value::object();
    attributes.set("POSITION", doc_.float_accessor(position, 3, "VEC3", kTargetVertices, true));
    attributes.set("NORMAL", doc_.float_accessor(normal, 3, "VEC3", kTargetVertices, false));
    attributes.set("TEXCOORD_0", doc_.float_accessor(uv, 2, "VEC2", kTargetVertices, false));
    if (colored) attributes.set("COLOR_0", doc_.int_accessor(color, gl::kUnsignedByte, 4, "VEC4", kTargetVertices, true));
    if (skin) {
      attributes.set("JOINTS_0", wide_joints ? doc_.int_accessor(joint16, gl::kUnsignedShort, 4, "VEC4", kTargetVertices, false)
                                             : doc_.int_accessor(joint8, gl::kUnsignedByte, 4, "VEC4", kTargetVertices, false));
      attributes.set("WEIGHTS_0", doc_.float_accessor(weight, 4, "VEC4", kTargetVertices, false));
    }
    Value primitives = Value::array();
    for (const assets::SubMesh& sub : mesh.submeshes) {
      if (!sub.index_count || sub.first_index + sub.index_count > mesh.indices.size()) continue;
      // The game draws a face whose corners run clockwise on screen (y down); glTF's front face runs counter-clockwise.
      std::vector<u32> triangles(mesh.indices.begin() + sub.first_index, mesh.indices.begin() + sub.first_index + sub.index_count);
      for (size_t k = 0; k + 2 < triangles.size(); k += 3) std::swap(triangles[k + 1], triangles[k + 2]);
      size_t indices = 0;
      if (count <= 0xFFFF) indices = doc_.int_accessor(std::vector<u16>(triangles.begin(), triangles.end()), gl::kUnsignedShort, 1, "SCALAR", kTargetIndices, false);
      else indices = doc_.int_accessor(triangles, gl::kUnsignedInt, 1, "SCALAR", kTargetIndices, false);
      Value primitive = Value::object();
      primitive.set("attributes", attributes);
      primitive.set("indices", indices);
      primitive.set("material", material(sub.material < mesh.materials.size() ? mesh.materials[sub.material] : assets::Material{}, mesh.trans));
      Value quads = Value::array();
      for (u32 k = 0; k < sub.index_count / 3; ++k)
        if (const size_t tri = sub.first_index / 3 + k; tri < mesh.tri_quad.size() && mesh.tri_quad[tri] == 1) quads.push(k);
      if (quads.size()) primitive.set("extras", Value::object()).set("quads", std::move(quads));
      primitives.push(std::move(primitive));
    }
    Value entry = Value::object();
    entry.set("name", std::format("construct_{:02}", mesh_index));
    entry.set("primitives", std::move(primitives));
    if (mesh_index < asset_.shading.size()) {
      const assets::ShadingEntry& shading = asset_.shading[mesh_index];
      Value extras = Value::object();
      Value list = Value::array();
      for (const u8 value : {shading.r, shading.g, shading.b, shading.flags}) list.push(static_cast<int>(value));
      extras.set("shading", std::move(list));
      entry.set("extras", std::move(extras));
    }
    doc_.meshes.push(std::move(entry));
    return doc_.meshes.size() - 1;
  }

  void add_skin() {
    const Affine convert = import::model_to_gltf();
    std::vector<float> matrices;
    Value joints = Value::array();
    for (size_t j = 0; j < joint_nodes_.size(); ++j) {
      const Value column = column_major(inverse(convert * world_[j]));
      for (const Value& item : column.items()) matrices.push_back(static_cast<float>(item.number()));
      joints.push(joint_nodes_[j]);
    }
    Value skin = Value::object();
    skin.set("name", "skeleton");
    skin.set("inverseBindMatrices", doc_.float_accessor(matrices, 16, "MAT4", 0, false));
    skin.set("joints", std::move(joints));
    doc_.skins.push(std::move(skin));
  }

  size_t sampler(Value& samplers, size_t input, const std::vector<float>& values, u32 components, const char* type, const char* interpolation) {
    Value entry = Value::object();
    entry.set("input", input);
    entry.set("interpolation", interpolation);
    entry.set("output", doc_.float_accessor(values, components, type, 0, false));
    samplers.push(std::move(entry));
    return samplers.size() - 1;
  }

  void channel(Value& channels, size_t sampler_index, size_t node, const char* path) {
    Value target = Value::object();
    target.set("node", node);
    target.set("path", path);
    Value entry = Value::object();
    entry.set("sampler", sampler_index);
    entry.set("target", std::move(target));
    channels.push(std::move(entry));
  }

  // Rotation samples of one joint track: every keyframe, with extra samples inside a step larger than kMaxStepDegrees on any axis
  // (interpolated per axis as the engine does). Quaternion signs are kept continuous.
  void rotation_samples(const std::vector<assets::Key3>& keys, std::vector<float>& times, std::vector<float>& quats) const {
    auto push = [&](double time, const Vec3d& deg) {
      const auto quat = assets::quat_zyx({static_cast<float>(deg[0]), static_cast<float>(deg[1]), static_cast<float>(deg[2])});
      std::array<float, 4> value = quat;
      if (!quats.empty()) {
        const size_t last = quats.size() - 4;
        const float dot = quats[last] * value[0] + quats[last + 1] * value[1] + quats[last + 2] * value[2] + quats[last + 3] * value[3];
        if (dot < 0)
          for (float& component : value) component = -component;
      }
      times.push_back(static_cast<float>(time));
      quats.insert(quats.end(), value.begin(), value.end());
    };
    for (size_t k = 0; k < keys.size(); ++k) {
      const double time = static_cast<double>(k) / assets::kModelAnimFps;
      if (k + 1 == keys.size()) {
        push(time, vec3d(keys[k].v));
        break;
      }
      std::array<float, 3> from{}, to{};
      double largest = 0;
      for (size_t axis = 0; axis < 3; ++axis) {
        from[axis] = keys[k].v[axis], to[axis] = keys[k + 1].v[axis];
        retail_arc(from[axis], to[axis]);
        largest = std::max(largest, std::fabs(static_cast<double>(to[axis]) - from[axis]));
      }
      const int steps = std::max(1, static_cast<int>(std::ceil(largest / kMaxStepDegrees)));
      push(time, vec3d(keys[k].v));
      for (int step = 1; step < steps; ++step) {
        const double u = static_cast<double>(step) / steps;
        push(time + u / assets::kModelAnimFps, {from[0] + (to[0] - from[0]) * u, from[1] + (to[1] - from[1]) * u, from[2] + (to[2] - from[2]) * u});
      }
    }
  }

  void add_animation(const assets::AnimationClip& clip) {
    const size_t joints = asset_.skeleton.joints.size();
    if (!clip.frame_count || clip.tracks.size() != joints || joints == 0) return;
    std::vector<float> key_times(clip.frame_count);
    for (u32 k = 0; k < clip.frame_count; ++k) key_times[k] = static_cast<float>(k / static_cast<double>(assets::kModelAnimFps));
    const size_t base_input = doc_.float_accessor(key_times, 1, "SCALAR", 0, true);
    std::optional<size_t> zero_input;
    auto single_input = [&] {
      if (!zero_input) zero_input = doc_.float_accessor({0.0f}, 1, "SCALAR", 0, true);
      return *zero_input;
    };
    Value samplers = Value::array(), channels = Value::array();
    for (size_t j = 0; j < joints; ++j) {
      const assets::Joint& joint = asset_.skeleton.joints[j];
      const assets::JointTrack& track = clip.tracks[j];
      if (track.rot.size() == 1 && track.rot[0].v != joint.rest_rot_deg) {
        const auto quat = assets::quat_zyx(track.rot[0].v);
        channel(channels, sampler(samplers, single_input(), {quat[0], quat[1], quat[2], quat[3]}, 4, "VEC4", "STEP"), joint_nodes_[j], "rotation");
      } else if (track.rot.size() > 1) {
        std::vector<float> times, quats;
        rotation_samples(track.rot, times, quats);
        const size_t input = times == key_times ? base_input : doc_.float_accessor(times, 1, "SCALAR", 0, true);
        channel(channels, sampler(samplers, input, quats, 4, "VEC4", "LINEAR"), joint_nodes_[j], "rotation");
      }
      if (track.trans.size() == 1 && track.trans[0].v != joint.rest_trans) {
        const assets::Vec3& value = track.trans[0].v;
        channel(channels, sampler(samplers, single_input(), {value[0], value[1], value[2]}, 3, "VEC3", "STEP"), joint_nodes_[j], "translation");
      } else if (track.trans.size() > 1) {
        std::vector<float> values;
        for (size_t k = 0; k < track.trans.size() && k < clip.frame_count; ++k) values.insert(values.end(), track.trans[k].v.begin(), track.trans[k].v.end());
        const size_t input = track.trans.size() == clip.frame_count ? base_input : doc_.float_accessor(std::vector<float>(key_times.begin(), key_times.begin() + static_cast<std::ptrdiff_t>(values.size() / 3)), 1, "SCALAR", 0, true);
        channel(channels, sampler(samplers, input, values, 3, "VEC3", "LINEAR"), joint_nodes_[j], "translation");
      }
    }
    // A clip that holds the rest pose still needs one channel.
    if (!channels.size()) {
      const auto& rest = asset_.skeleton.joints[0].rest_quat;
      channel(channels, sampler(samplers, single_input(), {rest[0], rest[1], rest[2], rest[3]}, 4, "VEC4", "STEP"), joint_nodes_[0], "rotation");
    }
    const u32 id = clip_id(clip);
    Value extras = Value::object();
    extras.set("clip", id);
    extras.set("source", clip.source);
    extras.set("index", clip.index);
    extras.set("frames", clip.frame_count);
    Value entry = Value::object();
    entry.set("name", clip_name(id));
    entry.set("channels", std::move(channels));
    entry.set("samplers", std::move(samplers));
    entry.set("extras", std::move(extras));
    doc_.animations.push(std::move(entry));
  }

  Value document(const std::vector<size_t>& scene_nodes) {
    Value root = Value::object();
    Value info = Value::object();
    info.set("version", "2.0");
    info.set("generator", "OpenCross");
    Value extras = Value::object();
    extras.set("kind", asset_.kind);
    extras.set("source", asset_.source);
    extras.set("art", hd_ ? "hd" : "classic");
    extras.set("keyframes_per_second", assets::kModelAnimFps);
    info.set("extras", std::move(extras));
    root.set("asset", std::move(info));
    if (doc_.materials.size()) {
      Value used = Value::array();
      used.push("KHR_materials_unlit");
      root.set("extensionsUsed", std::move(used));
    }
    root.set("scene", 0);
    Value scene = Value::object();
    scene.set("name", asset_.source.empty() ? std::string("model") : asset_.source);
    Value list = Value::array();
    for (const size_t node : scene_nodes) list.push(node);
    scene.set("nodes", std::move(list));
    Value scenes = Value::array();
    scenes.push(std::move(scene));
    root.set("scenes", std::move(scenes));
    Value nodes = Value::array();
    for (Value& node : doc_.nodes) nodes.push(std::move(node));
    root.set("nodes", std::move(nodes));
    for (auto& [name, list_value] : std::initializer_list<std::pair<const char*, Value*>>{{"meshes", &doc_.meshes}, {"materials", &doc_.materials},
                                                                                         {"textures", &doc_.textures}, {"images", &doc_.images},
                                                                                         {"samplers", &doc_.samplers}, {"skins", &doc_.skins},
                                                                                         {"animations", &doc_.animations}})
      if (list_value->size()) root.set(name, std::move(*list_value));
    root.set("accessors", std::move(doc_.accessors));
    root.set("bufferViews", std::move(doc_.views));
    Value buffer = Value::object();
    buffer.set("byteLength", (doc_.bin.size() + 3) / 4 * 4);
    Value buffers = Value::array();
    buffers.push(std::move(buffer));
    root.set("buffers", std::move(buffers));
    return root;
  }

  const assets::ModelAsset& asset_;
  bool hd_;
  Document doc_;
  GlbStats stats_;
  std::vector<Affine> world_;
  std::vector<size_t> joint_nodes_;
  std::map<int, int> texture_of_;
  std::map<MaterialKey, size_t> material_of_;
};

}  // namespace

u32 clip_id(const assets::AnimationClip& clip) { return clip.source == "s4" ? clip.index : 0x80 + clip.index; }

std::string clip_name(u32 id) { return std::format("clip_{:02x}", id); }

Bytes model_to_glb(const assets::ModelAsset& asset, bool hd, GlbStats* stats) { return Exporter(asset, hd).run(stats); }

assets::ModelAsset weapon_part_asset(const assets::WeaponPart& part, const std::string& source) {
  assets::ModelAsset asset;
  asset.kind = "weapon";
  asset.source = source;
  asset.meshes.push_back(part.mesh);
  asset.textures.push_back(part.texture);
  asset.main_texture_count = 1;
  return asset;
}

namespace {

std::array<double, 4> read_numbers(const Value& list, std::array<double, 4> fallback) {
  for (size_t i = 0; i < list.size() && i < fallback.size(); ++i) fallback[i] = list[i].number(fallback[i]);
  return fallback;
}

struct Channel {
  size_t node = 0;
  bool rotation = false, step = false;
  std::vector<double> times, values;
};

// Channel value at `time`: the keys around it, linear (quaternions normalised afterwards) or step.
std::array<double, 4> sample(const Channel& channel, double time) {
  const size_t width = channel.rotation ? 4 : 3;
  std::array<double, 4> out{};
  if (channel.times.empty() || channel.values.size() < channel.times.size() * width) return out;
  size_t i = 0;
  while (i + 1 < channel.times.size() && channel.times[i + 1] <= time) ++i;
  double u = 0;
  if (!channel.step && i + 1 < channel.times.size() && time > channel.times[i])
    u = (time - channel.times[i]) / (channel.times[i + 1] - channel.times[i]);
  for (size_t k = 0; k < width; ++k) {
    const double from = channel.values[i * width + k];
    const double to = u > 0 ? channel.values[(i + 1) * width + k] : from;
    out[k] = from + (to - from) * u;
  }
  return out;
}

Affine affine_of(const render::Mat4& mat) {
  Affine out;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col) out.at(row, col) = mat.at(row, col);
  return out;
}

double distance(const Vec3d& lhs, const Vec3d& rhs) {
  return std::sqrt((lhs[0] - rhs[0]) * (lhs[0] - rhs[0]) + (lhs[1] - rhs[1]) * (lhs[1] - rhs[1]) + (lhs[2] - rhs[2]) * (lhs[2] - rhs[2]));
}

}  // namespace

GlbCheck check_glb(const assets::ModelAsset& asset, ByteSpan data) {
  GlbCheck out;
  std::string err;
  const auto glb = gl::read_glb(data, &err);
  if (!glb) {
    out.error = err;
    return out;
  }
  const Value& json = glb->json;
  const Value& nodes = json["nodes"];
  constexpr size_t kNone = static_cast<size_t>(-1);
  std::vector<size_t> parent(nodes.size(), kNone);
  for (size_t i = 0; i < nodes.size(); ++i)
    for (const Value& child : nodes[i]["children"].items())
      if (child.index() < nodes.size()) parent[child.index()] = i;
  auto accessor = [&](const Value& index, u32* components = nullptr) {
    return index.is_number() ? gl::read_accessor(*glb, index.index(), components).value_or(std::vector<double>{}) : std::vector<double>{};
  };
  const Affine from_gltf = inverse(import::model_to_gltf());
  const size_t joints = asset.skeleton.joints.size();

  struct MeshData {
    size_t asset_mesh = 0;
    bool skin = false;
    std::vector<double> position, normal, joint, weight;
    std::vector<std::array<size_t, 3>> triangles;
  };
  std::vector<MeshData> meshes;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Value& node = nodes[i];
    if (!node.has("mesh")) continue;
    const std::string& name = node["name"].text();
    const size_t mesh_index = name.starts_with("mesh_") ? static_cast<size_t>(std::atoi(name.c_str() + 5)) : kNone;
    if (mesh_index >= asset.meshes.size()) {
      out.error = std::format("node {} names no mesh of the model", name);
      return out;
    }
    const Value& primitives = json["meshes"][node["mesh"].index()]["primitives"];
    MeshData mesh;
    mesh.asset_mesh = mesh_index;
    mesh.skin = node.has("skin");
    for (const Value& primitive : primitives.items()) {
      const Value& attributes = primitive["attributes"];
      if (mesh.position.empty()) {
        mesh.position = accessor(attributes["POSITION"]);
        mesh.normal = accessor(attributes["NORMAL"]);
        mesh.joint = accessor(attributes["JOINTS_0"]);
        mesh.weight = accessor(attributes["WEIGHTS_0"]);
      }
      const std::vector<double> indices = accessor(primitive["indices"]);
      for (size_t k = 0; k + 2 < indices.size(); k += 3)
        mesh.triangles.push_back({static_cast<size_t>(indices[k]), static_cast<size_t>(indices[k + 1]), static_cast<size_t>(indices[k + 2])});
    }
    const assets::Mesh& source = asset.meshes[mesh_index];
    const size_t vertex_count = mesh.position.size() / 3;
    if (vertex_count != source.vertices.size() || mesh.triangles.size() != source.indices.size() / 3 ||
        (mesh.skin && (mesh.joint.size() != vertex_count * 4 || mesh.weight.size() != vertex_count * 4))) {
      out.error = std::format("{}: {} vertices / {} triangles, the model has {} / {}", name, vertex_count, mesh.triangles.size(), source.vertices.size(),
                              source.indices.size() / 3);
      return out;
    }
    for (const auto& triangle : mesh.triangles)
      for (const size_t corner : triangle)
        if (corner >= vertex_count) {
          out.error = name + ": index out of range";
          return out;
        }
    out.stats.vertices += static_cast<u32>(vertex_count);
    out.stats.triangles += static_cast<u32>(mesh.triangles.size());
    meshes.push_back(std::move(mesh));
  }
  out.stats.images = static_cast<u32>(json["images"].size());
  out.stats.clips = static_cast<u32>(json["animations"].size());
  size_t expected_clips = 0;
  for (const assets::AnimationClip& clip : asset.clips) expected_clips += clip.frame_count && clip.tracks.size() == joints && joints ? 1 : 0;
  if (out.stats.clips != expected_clips) {
    out.error = std::format("{} animations, the model has {} clips", out.stats.clips, expected_clips);
    return out;
  }

  // Facing: the counter-clockwise normal of each triangle against its corners' normals (both in the file's space).
  size_t front = 0, total = 0;
  for (const MeshData& mesh : meshes) {
    if (mesh.normal.size() != mesh.position.size()) continue;
    for (const auto& triangle : mesh.triangles) {
      Vec3d corner[3], normal{};
      for (size_t k = 0; k < 3; ++k) {
        corner[k] = {mesh.position[triangle[k] * 3], mesh.position[triangle[k] * 3 + 1], mesh.position[triangle[k] * 3 + 2]};
        for (size_t axis = 0; axis < 3; ++axis) normal[axis] += mesh.normal[triangle[k] * 3 + axis];
      }
      const Vec3d e1{corner[1][0] - corner[0][0], corner[1][1] - corner[0][1], corner[1][2] - corner[0][2]};
      const Vec3d e2{corner[2][0] - corner[0][0], corner[2][1] - corner[0][1], corner[2][2] - corner[0][2]};
      const Vec3d cross{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      const double dot = cross[0] * normal[0] + cross[1] * normal[1] + cross[2] * normal[2];
      if (dot == 0) continue;
      ++total;
      front += dot > 0 ? 1 : 0;
    }
  }
  out.facing = total ? static_cast<double>(front) / static_cast<double>(total) : 1.0;

  // Rigid meshes: the vertices are the model's own points.
  for (const MeshData& mesh : meshes) {
    if (mesh.skin) continue;
    const assets::Mesh& source = asset.meshes[mesh.asset_mesh];
    for (size_t i = 0; i < source.vertices.size(); ++i) {
      const Vec3d point{mesh.position[i * 3], mesh.position[i * 3 + 1], mesh.position[i * 3 + 2]};
      out.vertex_error = std::max(out.vertex_error, distance(point, vec3d(source.pool[source.vertices[i].pool].bind_pos)));
    }
  }
  const Value& skin = json["skins"][0];
  if (!skin.is_object()) {
    if (joints && std::ranges::any_of(meshes, [](const MeshData& mesh) { return mesh.skin; })) out.error = "skinned meshes without a skin";
    return out;
  }
  out.stats.joints = static_cast<u32>(skin["joints"].size());
  if (out.stats.joints != joints) {
    out.error = std::format("{} skin joints, the skeleton has {}", out.stats.joints, joints);
    return out;
  }
  std::vector<size_t> joint_node(joints);
  for (size_t j = 0; j < joints; ++j) joint_node[j] = skin["joints"][j].index(0);
  const std::vector<double> ibm_values = accessor(skin["inverseBindMatrices"]);
  if (ibm_values.size() != joints * 16) {
    out.error = "inverse bind matrix count";
    return out;
  }
  std::vector<Affine> ibm(joints);
  for (size_t j = 0; j < joints; ++j)
    for (int col = 0; col < 4; ++col)
      for (int row = 0; row < 3; ++row) ibm[j].at(row, col) = ibm_values[j * 16 + static_cast<size_t>(col * 4 + row)];


  for (const Value& animation : json["animations"].items()) {
    const u32 id = static_cast<u32>(animation["extras"]["clip"].number(-1));
    const auto clip = std::ranges::find_if(asset.clips, [&](const assets::AnimationClip& candidate) { return clip_id(candidate) == id && candidate.frame_count; });
    if (clip == asset.clips.end() || animation["name"].text() != clip_name(id)) {
      out.error = std::format("animation {} matches no clip", animation["name"].text());
      return out;
    }
    std::vector<Channel> channels;
    for (const Value& entry : animation["channels"].items()) {
      const Value& sampler_entry = animation["samplers"][entry["sampler"].index(0)];
      Channel channel;
      channel.node = entry["target"]["node"].index(0);
      channel.rotation = entry["target"]["path"].text() == "rotation";
      channel.step = sampler_entry["interpolation"].text() == "STEP";
      channel.times = accessor(sampler_entry["input"]);
      channel.values = accessor(sampler_entry["output"]);
      channels.push_back(std::move(channel));
    }
    for (u32 frame = 0; frame < clip->frame_count; ++frame) {
      const double time = static_cast<float>(frame / static_cast<double>(assets::kModelAnimFps));
      std::vector<Affine> local(nodes.size());
      for (size_t i = 0; i < nodes.size(); ++i) {
        const auto node_trans = read_numbers(nodes[i]["translation"], {0, 0, 0, 0});
        const auto node_rot = read_numbers(nodes[i]["rotation"], {0, 0, 0, 1});
        const auto node_scale = read_numbers(nodes[i]["scale"], {1, 1, 1, 0});
        Vec3d trans{node_trans[0], node_trans[1], node_trans[2]};
        Quat rot{node_rot[0], node_rot[1], node_rot[2], node_rot[3]};
        for (const Channel& channel : channels) {
          if (channel.node != i) continue;
          const auto value = sample(channel, time);
          if (channel.rotation) rot = {value[0], value[1], value[2], value[3]};
          else trans = {value[0], value[1], value[2]};
        }
        local[i] = affine_trs(trans, rot, {node_scale[0], node_scale[1], node_scale[2]});
      }
      std::vector<Affine> global(nodes.size());
      std::vector<u8> done(nodes.size(), 0);
      auto global_of = [&](auto& self, size_t i) -> const Affine& {
        if (!done[i]) {
          global[i] = parent[i] == kNone ? local[i] : self(self, parent[i]) * local[i];
          done[i] = 1;
        }
        return global[i];
      };
      std::vector<render::Mat4> engine(joints);
      for (size_t j = 0; j < joints; ++j) {
        const assets::Joint& joint = asset.skeleton.joints[j];
        const assets::JointTrack& track = clip->tracks[j];
        const assets::Vec3 rot = track.rot.empty() ? joint.rest_rot_deg : track.rot[std::min<size_t>(frame, track.rot.size() - 1)].v;
        const assets::Vec3 trans = track.trans.empty() ? joint.rest_trans : track.trans[std::min<size_t>(frame, track.trans.size() - 1)].v;
        const render::Mat4 joint_local = render::mat4_from_rt(assets::rotation_zyx(rot), trans);
        engine[j] = joint.parent >= 0 ? engine[static_cast<size_t>(joint.parent)] * joint_local : joint_local;
        const Affine file = from_gltf * global_of(global_of, joint_node[j]);
        out.joint_error = std::max(out.joint_error, distance({file.at(0, 3), file.at(1, 3), file.at(2, 3)}, {engine[j].at(0, 3), engine[j].at(1, 3), engine[j].at(2, 3)}));
      }
      std::vector<Affine> skinning(joints);
      for (size_t j = 0; j < joints; ++j) skinning[j] = global_of(global_of, joint_node[j]) * ibm[j];
      for (const MeshData& mesh : meshes) {
        if (!mesh.skin) continue;
        const assets::Mesh& source = asset.meshes[mesh.asset_mesh];
        for (size_t i = 0; i < source.vertices.size(); ++i) {
          const Vec3d point{mesh.position[i * 3], mesh.position[i * 3 + 1], mesh.position[i * 3 + 2]};
          Vec3d file{};
          for (size_t k = 0; k < 4; ++k) {
            const double w = mesh.weight[i * 4 + k];
            const size_t j = static_cast<size_t>(mesh.joint[i * 4 + k]);
            if (w == 0 || j >= joints) continue;
            const Vec3d moved = apply_point(skinning[j], point);
            for (size_t axis = 0; axis < 3; ++axis) file[axis] += w * moved[axis];
          }
          file = apply_point(from_gltf, file);
          const assets::SkinVertex& pool = source.pool[source.vertices[i].pool];
          Vec3d game{};
          for (size_t k = 0; k < 2; ++k) {
            if (pool.weight[k] == 0 || pool.joint[k] >= joints) continue;
            const Vec3d moved = apply_point(affine_of(engine[pool.joint[k]]), vec3d(pool.local[k]));
            for (size_t axis = 0; axis < 3; ++axis) game[axis] += pool.weight[k] * moved[axis];
          }
          double& error = pool.weight[1] > 0 ? out.split_error : out.vertex_error;
          error = std::max(error, distance(file, game));
        }
      }
      ++out.poses;
    }
  }
  return out;
}

ImportCheck check_import(const assets::ModelAsset& asset, ByteSpan data) {
  ImportCheck out;
  const std::optional<import::GltfModel> model = import::load_gltf_model(data, asset.skeleton, out.error);
  if (!model) return out;
  const size_t joints = asset.skeleton.joints.size();
  for (size_t construct = 0; construct < asset.meshes.size() || construct < model->meshes.size(); ++construct) {
    const assets::Mesh empty;
    const assets::Mesh& source = construct < asset.meshes.size() ? asset.meshes[construct] : empty;
    const assets::Mesh& copy = construct < model->meshes.size() ? model->meshes[construct] : empty;
    if (source.vertices.size() != copy.vertices.size()) {
      out.error = std::format("construct {}: {} corners, the model has {}", construct, copy.vertices.size(), source.vertices.size());
      return out;
    }
    bool same = source.indices == copy.indices && source.tri_quad == copy.tri_quad && source.submeshes.size() == copy.submeshes.size();
    for (size_t k = 0; same && k < source.submeshes.size(); ++k) {
      const assets::SubMesh& lhs = source.submeshes[k];
      const assets::SubMesh& rhs = copy.submeshes[k];
      same = lhs.first_index == rhs.first_index && lhs.index_count == rhs.index_count &&
             source.materials[lhs.material].color == copy.materials[rhs.material].color &&
             (source.materials[lhs.material].texture < 0) == (copy.materials[rhs.material].texture < 0);
    }
    out.faces += same && source.trans == copy.trans ? 0 : 1;
  }
  auto skinned = [&](const assets::Mesh& mesh, size_t corner, const std::vector<render::Mat4>& pose) {
    const assets::SkinVertex& vertex = mesh.pool[mesh.vertices[corner].pool];
    Vec3d at{};
    for (size_t k = 0; k < 2; ++k) {
      if (vertex.weight[k] == 0 || vertex.joint[k] >= joints) continue;
      const Vec3d moved = apply_point(affine_of(pose[vertex.joint[k]]), vec3d(vertex.local[k]));
      for (size_t axis = 0; axis < 3; ++axis) at[axis] += vertex.weight[k] * moved[axis];
    }
    return at;
  };
  for (const assets::AnimationClip& clip : asset.clips) {
    if (!clip.frame_count || clip.tracks.size() != joints) continue;
    std::vector<render::Mat4> pose(joints);
    for (u32 frame = 0; frame < clip.frame_count; ++frame) {
      for (size_t j = 0; j < joints; ++j) {
        const assets::Joint& joint = asset.skeleton.joints[j];
        const assets::JointTrack& track = clip.tracks[j];
        const assets::Vec3 rot = track.rot.empty() ? joint.rest_rot_deg : track.rot[std::min<size_t>(frame, track.rot.size() - 1)].v;
        const assets::Vec3 trans = track.trans.empty() ? joint.rest_trans : track.trans[std::min<size_t>(frame, track.trans.size() - 1)].v;
        const render::Mat4 local = render::mat4_from_rt(assets::rotation_zyx(rot), trans);
        pose[j] = joint.parent >= 0 ? pose[static_cast<size_t>(joint.parent)] * local : local;
      }
      for (size_t construct = 0; construct < asset.meshes.size(); ++construct) {
        const assets::Mesh& source = asset.meshes[construct];
        if (!source.skinned) continue;
        for (size_t corner = 0; corner < source.vertices.size(); ++corner) {
          const double gap = distance(skinned(source, corner, pose), skinned(model->meshes[construct], corner, pose));
          double& error = source.pool[source.vertices[corner].pool].weight[1] > 0 ? out.split_error : out.vertex_error;
          error = std::max(error, gap);
          ++out.corners;
        }
      }
    }
  }
  return out;
}

std::string ModelExportProgress::phase() const {
  std::lock_guard lock(mutex_);
  return phase_;
}
void ModelExportProgress::set_phase(std::string name) {
  std::lock_guard lock(mutex_);
  phase_ = std::move(name);
}
std::string ModelExportProgress::summary() const {
  std::lock_guard lock(mutex_);
  return summary_;
}
void ModelExportProgress::set_summary(std::string line) {
  std::lock_guard lock(mutex_);
  summary_ = std::move(line);
}

namespace {

std::string lower(std::string text) {
  for (char& ch : text) ch = ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

// One thing to export: a model stem (field / battle / menu/player/<name>) or a weapon pack member.
struct ExportItem {
  unsigned kind = 0;
  std::string stem;
};

std::vector<ExportItem> export_items(const vfs::Vfs& files, unsigned what, const std::string& only) {
  std::vector<ExportItem> items;
  auto wanted = [&](const std::string& name) { return only.empty() || name.find(only) != std::string::npos; };
  for (const std::string& stem : model_stems(files)) {
    const unsigned kind = stem.starts_with("fieldobj/") ? kExportField : kExportBattle;
    if ((what & kind) && wanted(stem)) items.push_back({kind, stem});
  }
  if (what & kExportMenu)
    for (int model = 0; model <= kLastMenuModel; ++model) {
      const std::string stem = "menu/player/" + menu_model_stem(model);
      if (files.exists("cdrom/" + stem + ".ome") && wanted(stem)) items.push_back({kExportMenu, stem});
    }
  if (what & kExportWeapons) {
    std::set<std::string> seen;
    for (const std::string& path : files.list("cdrom")) {
      const std::string lowered = lower(path);
      if (lowered.starts_with("battle/wepon/") && lowered.ends_with(".bin") && wanted(lowered) && seen.insert(lowered).second)
        items.push_back({kExportWeapons, lowered});
    }
  }
  return items;
}

// The models of one item with their output names (member stems).
std::vector<std::pair<std::string, assets::ModelAsset>> load_item(const vfs::Vfs& files, const ExportItem& item, bool hd, std::string* err) {
  std::vector<std::pair<std::string, assets::ModelAsset>> out;
  switch (item.kind) {
    case kExportField:
    case kExportBattle:
      if (auto asset = read_model_files(files, item.stem, err)) out.emplace_back(item.stem, std::move(*asset));
      break;
    case kExportMenu: {
      const Bytes ome = files.read("cdrom/" + item.stem + ".ome").value_or(Bytes{});
      const Bytes tim = hd ? files.read("hd/" + item.stem + ".tim").value_or(Bytes{}) : Bytes{};
      if (auto asset = import::load_menu_model(ome, tim, err)) {
        asset->source = item.stem;
        out.emplace_back(item.stem, std::move(*asset));
      }
      break;
    }
    case kExportWeapons: {
      const Bytes raw = files.read("cdrom/" + item.stem).value_or(Bytes{});
      if (auto pack = import::load_weapon_pack(raw, err))
        for (size_t part = 0; part < pack->parts.size(); ++part) {
          const std::string source = weapon_texture_source(item.stem, static_cast<int>(part));
          out.emplace_back(source, weapon_part_asset(pack->parts[part], source));
        }
      break;
    }
    default:
      break;
  }
  return out;
}

bool write_file(const std::filesystem::path& path, const Bytes& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

}  // namespace

int run_model_export(const std::filesystem::path& game_dir, const std::filesystem::path& out_dir, unsigned what, ModelExportProgress& progress,
                     const std::string& only) {
  const auto start_time = std::chrono::steady_clock::now();
  progress.running = true;
  progress.finished = false;
  progress.files = 0;
  progress.bytes = 0;
  vfs::Vfs files;
  const bool opened = files.open(game_dir);
  u32 skipped = 0, checked = 0, check_failed = 0, imports = 0, import_failed = 0;
  double joint_error = 0, vertex_error = 0, split_error = 0, facing = 1, import_error = 0, import_split_error = 0;
  std::vector<bool> arts;
  if (what & kExportClassic || !(what & kExportHd)) arts.push_back(false);
  if (what & kExportHd || !(what & kExportClassic)) arts.push_back(true);
  std::vector<std::vector<ExportItem>> items_of(arts.size());
  progress.done = 0;
  progress.total = 0;
  for (size_t i = 0; i < arts.size() && opened; ++i) {
    files.set_hd_mode(arts[i]);
    items_of[i] = export_items(files, what, only);
    progress.total += static_cast<u32>(items_of[i].size());
  }
  for (size_t i = 0; i < arts.size(); ++i) {
    if (!opened || progress.cancel) break;
    const bool hd = arts[i];
    files.set_hd_mode(hd);
    const std::string art = hd ? "hd" : "classic";
    progress.set_phase(art + " models");
    for (const ExportItem& item : items_of[i]) {
      if (progress.cancel) break;
      try {
        std::string err;
        auto models = load_item(files, item, hd, &err);
        if (models.empty()) {
          ++skipped;
          log::warn("model export: {} ({}): {}", item.stem, art, err.empty() ? "nothing to export" : err);
        }
        for (const auto& [name, asset] : models) {
          GlbStats stats;
          const Bytes glb = model_to_glb(asset, hd, &stats);
          const std::filesystem::path path = out_dir / art / std::filesystem::path(std::u8string(name.begin(), name.end()) + u8".glb");
          if (!write_file(path, glb)) {
            ++skipped;
            log::warn("model export: cannot write {}", path.string());
            continue;
          }
          ++progress.files;
          progress.bytes += glb.size();
          log::debug("model export: {}/{}.glb: {} vertices, {} triangles, {} joints, {} clips, {} images, {} bytes", art, name, stats.vertices,
                     stats.triangles, stats.joints, stats.clips, stats.images, glb.size());
          if (!(what & kExportVerify)) continue;
          const GlbCheck check = check_glb(asset, glb);
          ++checked;
          if (!check.ok()) ++check_failed;
          joint_error = std::max(joint_error, check.joint_error);
          vertex_error = std::max(vertex_error, check.vertex_error);
          facing = std::min(facing, check.facing);
          split_error = std::max(split_error, check.split_error);
          log::info("model check {}/{}: {} vertices, {} triangles, {} joints, {} clips, {} poses, joint error {:.4f}, vertex error {:.4f}, two-joint "
                    "vertex error {:.4f}, facing {:.3f}{}{}",
                    art, name, check.stats.vertices, check.stats.triangles, check.stats.joints, check.stats.clips, check.poses, check.joint_error,
                    check.vertex_error, check.split_error, check.facing, check.ok() ? "" : ": ", check.error);
          if (item.kind == kExportWeapons) continue;
          const ImportCheck back = check_import(asset, glb);
          ++imports;
          if (!back.ok() || back.faces) ++import_failed;
          import_error = std::max(import_error, back.vertex_error);
          import_split_error = std::max(import_split_error, back.split_error);
          log::info("import check {}/{}: {} corner poses, vertex error {:.4f}, two-joint vertex error {:.4f}, {} construct(s) with other faces{}{}", art,
                    name, back.corners, back.vertex_error, back.split_error, back.faces, back.ok() ? "" : ": ", back.error);
        }
      } catch (...) {
        ++skipped;
      }
      ++progress.done;
    }
  }
  files.set_hd_mode(false);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
  std::string summary = std::format("{}: {} file(s), {:.1f} MB, {} item(s) skipped, {} s", !opened ? "model export: game files not found"
                                                                                         : progress.cancel ? "model export cancelled" : "model export done",
                                    progress.files.load(), progress.bytes.load() / 1048576.0, skipped, static_cast<int>(seconds));
  if (checked)
    summary += std::format("; checked {}: {} failed, max joint error {:.4f}, max vertex error {:.4f} (two-joint {:.4f}), min facing {:.3f}", checked,
                           check_failed, joint_error, vertex_error, split_error, facing);
  if (imports)
    summary += std::format("; imported {}: {} failed, max vertex error {:.4f} (two-joint {:.4f})", imports, import_failed, import_error, import_split_error);
  progress.set_summary(summary);
  log::info("{}", summary);
  progress.running = false;
  progress.finished = true;
  return opened ? 0 : 1;
}

bool ModelExportJob::start(const std::filesystem::path& game_dir, unsigned what) {
  if (progress_.running) return false;
  join();
  progress_.cancel = false;
  progress_.running = true;
  progress_.finished = false;
  progress_.done = 0;
  progress_.total = 0;
  thread_ = std::thread([this, game_dir, what] {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    run_model_export(game_dir, game_dir / "dump" / "models", what, progress_);
  });
  return true;
}

void ModelExportJob::join() {
  if (thread_.joinable()) thread_.join();
}

}  // namespace oc::game
