#pragma once
// Model replacements: an active mod's models\<classic|hd>\<member stem>.glb (vfs::Overlay::models) replaces the meshes and textures of that one
// model on the model's own skeleton (import/gltf_model.h). The skeleton, the clips, attach points, mounts, shading entries, extent and
// everything the scripts read stay the model's. A file that does not fit is refused: logged, listed among its mod's issues
// (vfs::report_file_problem), and the model is drawn as shipped.
// A character's all-uses file, models\<classic|hd>\character\<name>.glb, is made on its Status-screen model's skeleton and stands in where
// a use has no file of its own: the Status screen, the field (through the HD-models-in-field rig, game/menu_rig.h) and battle, where its
// meshes are bound once onto the battle model's skeleton (battle_joint_map, rebind_meshes) so the battle clips drive them.
#include <span>
#include <string>
#include <vector>

#include "assets/model.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

// Replaces `asset`'s meshes with the active mods' "<hd ? hd : classic>/<stem>.glb" when there is one (`stem`: the member stem the export
// names the file by, "menu/player/<name>" for a Status-screen model). True when replaced.
bool apply_model_mod(const vfs::Vfs& files, assets::ModelAsset& asset, const std::string& stem, bool hd);

// The stem of character `name`'s all-uses file (`name`: the Status-screen model's file name, menu_model_stem).
std::string character_model_stem(const std::string& name);
// Whether an active mod has character `name`'s all-uses file for the art set.
bool has_character_model(const vfs::Vfs& files, const std::string& name, bool hd);

// The battle joint each Status-model joint is carried by. Two skeletons with the same joints and parents pair joint for joint; otherwise
// the HD-models-in-field matcher pairs them (match_menu_rig, the battle model in the field model's place) and a joint without a partner
// rides its nearest paired ancestor's. Empty with `error` set when the matcher finds no usable pairing.
std::vector<int> battle_joint_map(const assets::ModelAsset& battle, const assets::ModelAsset& menu, std::string& error);
// `meshes`, skinned on `from`, bound onto `to`: each vertex keeps its rest-pose place and each of its joints k becomes joint_map[k], with
// its place and normal in that joint's rest frame (two joints that become one are merged).
std::vector<assets::Mesh> rebind_meshes(std::vector<assets::Mesh> meshes, const assets::Skeleton& from, const assets::Skeleton& to,
                                        std::span<const int> joint_map);
// Battle model `battle` drawn with `menu`'s meshes and textures (a Status-screen model wearing its all-uses file), bound onto the battle
// skeleton; the battle model keeps its skeleton, clips, attach points, mounts, shading entries and extent. False when `menu` wears no
// file or the two skeletons do not pair (then reported against character `name`'s file).
bool wear_character_model(const vfs::Vfs& files, assets::ModelAsset& battle, const assets::ModelAsset& menu, const std::string& name, bool hd);

}  // namespace oc::game
