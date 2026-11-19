#pragma once
// A model's glTF form, both ways of the layout the model export writes (docs/formats/mods.md 7.1): its axes and scale, and the import of a
// mod's .glb that replaces a model's meshes on the model's own skeleton. The file's joint nodes "joint_NN" are matched to the skeleton's joints
// by name; its meshes are skinned in the file's rest pose and rebound to the skeleton's rest pose in the engine's two-joint form. Images stay
// encoded (the caller decodes them); the file's animations are not read.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/model.h"
#include "core/types.h"
#include "import/gltf.h"

namespace oc::import {

// glTF space = (x, -y, -z) of the model space (+y down) times this: about 1 m per 1000 units.
constexpr float kGltfScale = 0.001f;
// The export's "root" node transform: model space -> glTF space.
gltf::Affine model_to_gltf();
// Each joint's rest transform in model space.
std::vector<gltf::Affine> rest_world(const assets::Skeleton& skeleton);

// The joint number of node name "joint_NN" (decimal digits), -1 for any other name.
int joint_number(std::string_view name);
// Node k of the file's joint for each skeleton joint; empty with `error` set when the file's joint nodes are not the skeleton's: a joint
// missing, one too many, one named twice, or one hanging under another joint than the skeleton's parent (its nearest joint ancestor).
std::vector<size_t> match_joints(const gltf::Value& json, const assets::Skeleton& skeleton, std::string& error);

// One image of the file: embedded (`bytes`) or a file next to it (`uri`, relative). `name` is the glTF image name ("clut0", "overlay1" in an
// exported file); `blend`: a BLEND material draws it.
struct GltfImage {
  std::string name;
  Bytes bytes;
  std::string uri;
  bool blend = false;
};

// The replacement meshes, one per construct (node "mesh_NN" = construct NN, other mesh nodes go to construct 0): Material::texture indexes
// `images`. `dropped` counts the vertices that had more than two joints (the lightest left out).
struct GltfModel {
  std::vector<assets::Mesh> meshes;
  std::vector<GltfImage> images;
  u32 vertices = 0, triangles = 0, dropped = 0;
};

// The meshes of `glb` on `skeleton`; nullopt with `error` set when the file cannot be used.
std::optional<GltfModel> load_gltf_model(ByteSpan glb, const assets::Skeleton& skeleton, std::string& error);

}  // namespace oc::import
