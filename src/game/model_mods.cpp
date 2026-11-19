#include "game/model_mods.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "game/menu_rig.h"
#include "import/gltf_model.h"
#include "import/tim.h"
#include "render/texture.h"
#include "render/texture_registry.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

namespace oc::game {

namespace {

// The texture an exported image name stands for: "clut<k>" = textures[k], "overlay<k>" = textures[main_texture_count + k]; -1 otherwise.
int named_texture(const assets::ModelAsset& asset, std::string_view name) {
  auto number = [&](std::string_view prefix) {
    if (name.size() <= prefix.size() || name.size() > prefix.size() + 3 || !name.starts_with(prefix)) return -1;
    int value = 0;
    for (const char ch : name.substr(prefix.size())) {
      if (ch < '0' || ch > '9') return -1;
      value = value * 10 + (ch - '0');
    }
    return value;
  };
  if (const int clut = number("clut"); clut >= 0) return clut < static_cast<int>(asset.main_texture_count) ? clut : -1;
  if (const int overlay = number("overlay"); overlay >= 0) {
    const size_t index = asset.main_texture_count + static_cast<size_t>(overlay);
    return index < asset.textures.size() ? static_cast<int>(index) : -1;
  }
  return -1;
}

// The file's image is the texture as the export writes it (render::model_file_pixels).
bool same_as(const import::Image& texture, const render::DecodedImage& image) {
  if (texture.indexed || texture.width != image.width || texture.height != image.height || texture.pixels.size() < image.rgba.size()) return false;
  return render::model_file_pixels(texture.width, texture.height, texture.pixels.data()) == image.rgba;
}

// The bytes of an image file next to the .glb: beside the loose file, or the member beside it in a packed mod.
std::optional<Bytes> read_beside(const vfs::Vfs& files, const vfs::OverlayFile& glb, const std::string& uri) {
  const std::filesystem::path rel = std::filesystem::path(std::u8string(uri.begin(), uri.end())).lexically_normal();
  if (rel.empty() || rel.is_absolute() || rel.has_root_name() || *rel.begin() == "..") return std::nullopt;
  if (glb.member.empty()) return files.read_mod_file({glb.file.parent_path() / rel, {}});
  const size_t slash = glb.member.find_last_of('/');
  const std::u8string text = rel.generic_u8string();
  const std::string member = (slash == std::string::npos ? std::string() : glb.member.substr(0, slash + 1)) + std::string(text.begin(), text.end());
  return files.read_mod_file({glb.file, vfs::normalise_path(member)});
}

}  // namespace

bool apply_model_mod(const vfs::Vfs& files, assets::ModelAsset& asset, const std::string& stem, bool hd) {
  if (!asset.mesh_mod.empty()) return false;
  const std::optional<vfs::OverlayFile> file = files.model_file((hd ? "hd/" : "classic/") + vfs::normalise_path(stem));
  if (!file) return false;
  const std::string label = file->label();
  auto refuse = [&](const std::string& reason) {
    log::warn("model mod: {} not used for {}: {}", label, stem, reason);
    vfs::report_file_problem(*file, "not used, " + reason);
    return false;
  };
  const std::optional<Bytes> bytes = files.read_mod_file(*file);
  if (!bytes) return refuse("the file cannot be read");
  std::string error;
  std::optional<import::GltfModel> model = import::load_gltf_model(*bytes, asset.skeleton, error);
  if (!model) return refuse(error);

  // Each image: the texture it is named after when the export's name fits (drawn from the file only when it differs from it), else a
  // texture of its own after the model's.
  std::vector<import::Image> textures = asset.textures, mods = asset.mod_textures;
  mods.resize(textures.size());
  std::vector<int> texture_of(model->images.size(), -1);
  std::vector<u8> claimed(textures.size(), 0);
  for (size_t i = 0; i < model->images.size(); ++i) {
    const import::GltfImage& entry = model->images[i];
    const std::optional<Bytes> encoded = entry.uri.empty() ? std::optional<Bytes>(entry.bytes) : read_beside(files, *file, entry.uri);
    render::DecodedImage image;
    if (!encoded || !render::decode_image(*encoded, image) || !image.width || !image.height)
      return refuse(std::format("image {} cannot be read", entry.name.empty() ? entry.uri : entry.name));
    const int named = named_texture(asset, entry.name);
    if (named >= 0 && !claimed[static_cast<size_t>(named)]) {
      const size_t index = static_cast<size_t>(named);
      claimed[index] = 1;
      texture_of[i] = named;
      if (same_as(asset.texture_for(index, hd), image) || same_as(asset.textures[index], image)) continue;
      const import::Image& original = asset.has_hd(index) && !asset.hd_textures[index].indexed ? asset.hd_textures[index] : asset.textures[index];
      render::model_restore_stp(original.width, original.height, original.pixels.data(), image.width, image.height, image.rgba);
      if (entry.blend && std::none_of(image.rgba.begin(), image.rgba.end(), [](u8 value) { return value == import::kStpAlpha; }))
        for (size_t k = 3; k < image.rgba.size(); k += 4)
          if (image.rgba[k]) image.rgba[k] = import::kStpAlpha;
      mods[index] = {image.width, image.height, false, std::move(image.rgba), {}};
      continue;
    }
    for (size_t k = 3; k < image.rgba.size(); k += 4) image.rgba[k] = image.rgba[k] < 0x80 ? u8{0} : entry.blend ? import::kStpAlpha : u8{0xFF};
    import::Image own{image.width, image.height, false, std::move(image.rgba), {}};
    textures.push_back(own);
    mods.push_back(std::move(own));
    texture_of[i] = static_cast<int>(textures.size() - 1);
  }

  for (assets::Mesh& mesh : model->meshes)
    for (assets::Material& material : mesh.materials)
      if (material.texture >= 0) material.texture = texture_of[static_cast<size_t>(material.texture)];
  const size_t constructs = std::max(asset.meshes.size(), model->meshes.size());
  model->meshes.resize(constructs);
  asset.meshes = std::move(model->meshes);
  asset.textures = std::move(textures);
  asset.mod_textures = std::move(mods);
  asset.mesh_mod = label;
  log::info("model mod: {} replaces {} ({}): {} vertices, {} triangles, {} image(s){}", label, stem, hd ? "hd" : "classic", model->vertices,
            model->triangles, model->images.size(),
            model->dropped ? std::format(", {} vertices keep their two heaviest joints", model->dropped) : std::string());
  return true;
}

std::string character_model_stem(const std::string& name) { return "character/" + name; }

bool has_character_model(const vfs::Vfs& files, const std::string& name, bool hd) {
  return !name.empty() && files.model_file((hd ? "hd/" : "classic/") + vfs::normalise_path(character_model_stem(name))).has_value();
}

std::vector<int> battle_joint_map(const assets::ModelAsset& battle, const assets::ModelAsset& menu, std::string& error) {
  const std::vector<assets::Joint>& to = battle.skeleton.joints;
  const std::vector<assets::Joint>& from = menu.skeleton.joints;
  std::vector<int> map(from.size(), -1);
  const bool same_joints =
      from.size() == to.size() && std::ranges::equal(from, to, [](const assets::Joint& lhs, const assets::Joint& rhs) { return lhs.parent == rhs.parent; });
  if (same_joints) {
    for (size_t j = 0; j < map.size(); ++j) map[j] = static_cast<int>(j);
    return map;
  }
  const MenuRig rig = match_menu_rig(battle, menu);
  if (!rig.usable || rig.field_joint.size() != from.size()) {
    error = std::format("the battle model's skeleton ({} joints) does not pair with the Status-screen model's ({} joints)", to.size(), from.size());
    return {};
  }
  for (size_t j = 0; j < map.size(); ++j) {
    const int parent = from[j].parent;
    map[j] = rig.field_joint[j] >= 0 ? rig.field_joint[j] : parent >= 0 && static_cast<size_t>(parent) < j ? map[static_cast<size_t>(parent)] : 0;
  }
  return map;
}

std::vector<assets::Mesh> rebind_meshes(std::vector<assets::Mesh> meshes, const assets::Skeleton& from, const assets::Skeleton& to,
                                        std::span<const int> joint_map) {
  namespace gl = import::gltf;
  const std::vector<gl::Affine> from_rest = import::rest_world(from), to_rest = import::rest_world(to);
  std::vector<gl::Affine> to_inverse;
  to_inverse.reserve(to_rest.size());
  for (const gl::Affine& rest : to_rest) to_inverse.push_back(gl::inverse(rest));
  auto frame = [&](size_t joint) { return joint < from_rest.size() ? from_rest[joint] : gl::Affine{}; };
  auto target = [&](size_t joint) {
    const int mapped = joint < joint_map.size() ? joint_map[joint] : -1;
    return mapped >= 0 && static_cast<size_t>(mapped) < to_inverse.size() ? static_cast<size_t>(mapped) : size_t{0};
  };
  for (assets::Mesh& mesh : meshes) {
    if (!mesh.skinned) continue;
    for (assets::SkinVertex& vertex : mesh.pool) {
      std::array<gl::Vec3d, 2> place{};
      for (size_t k = 0; k < 2; ++k)
        place[k] = gl::apply_point(frame(vertex.joint[k]), {vertex.local[k][0], vertex.local[k][1], vertex.local[k][2]});
      const gl::Vec3d normal =
          gl::apply_vector(frame(vertex.joint[vertex.normal_joint]), {static_cast<double>(vertex.normal[0]), static_cast<double>(vertex.normal[1]),
                                                                          static_cast<double>(vertex.normal[2])});
      std::array<size_t, 2> joint{target(vertex.joint[0]), target(vertex.joint[1])};
      if (vertex.weight[1] > 0.0f && joint[0] == joint[1]) {
        for (size_t axis = 0; axis < 3; ++axis) place[0][axis] = vertex.weight[0] * place[0][axis] + vertex.weight[1] * place[1][axis];
        vertex.weight = {1.0f, 0.0f};
      }
      if (vertex.weight[1] <= 0.0f) joint[1] = joint[0], place[1] = place[0];
      for (size_t k = 0; k < 2; ++k) {
        vertex.joint[k] = static_cast<u16>(joint[k]);
        const gl::Vec3d local = gl::apply_point(to_inverse[joint[k]], place[k]);
        for (size_t axis = 0; axis < 3; ++axis) vertex.local[k][axis] = static_cast<float>(local[axis]);
      }
      vertex.normal_joint = vertex.weight[1] > 0.0f ? 1 : 0;
      const gl::Vec3d turned = gl::apply_vector(to_inverse[joint[vertex.normal_joint]], normal);
      for (size_t axis = 0; axis < 3; ++axis) vertex.normal[axis] = static_cast<s8>(std::clamp(std::lround(turned[axis]), -127l, 127l));
    }
  }
  return meshes;
}

bool wear_character_model(const vfs::Vfs& files, assets::ModelAsset& battle, const assets::ModelAsset& menu, const std::string& name, bool hd) {
  if (menu.mesh_mod.empty() || !battle.mesh_mod.empty()) return false;
  std::string error;
  const std::vector<int> map = battle_joint_map(battle, menu, error);
  if (map.empty()) {
    log::warn("model mod: {} not used for {}: {}", menu.mesh_mod, battle.source, error);
    if (const auto file = files.model_file((hd ? "hd/" : "classic/") + vfs::normalise_path(character_model_stem(name))))
      vfs::report_file_problem(*file, "not used for " + battle.source + ", " + error);
    return false;
  }
  const bool same = same_skeleton(battle.skeleton, menu.skeleton);
  std::vector<assets::Mesh> meshes = same ? menu.meshes : rebind_meshes(menu.meshes, menu.skeleton, battle.skeleton, map);
  meshes.resize(std::max(battle.meshes.size(), meshes.size()));
  battle.meshes = std::move(meshes);
  battle.textures = menu.textures;
  battle.main_texture_count = menu.main_texture_count;
  battle.hd_textures = menu.hd_textures;
  battle.hd_scale = menu.hd_scale;
  battle.hd_palette = menu.hd_palette;
  battle.mod_textures = menu.mod_textures;
  battle.eye = menu.eye;
  battle.character.clear();
  battle.mesh_mod = menu.mesh_mod;
  log::info("model mod: {} replaces {} ({}): {}", menu.mesh_mod, battle.source, hd ? "hd" : "classic",
            same ? std::string("the Status-screen model's skeleton is the battle model's")
                 : std::format("the Status-screen model's {} joints bound onto the battle model's {}", map.size(), battle.skeleton.joints.size()));
  return true;
}

}  // namespace oc::game
