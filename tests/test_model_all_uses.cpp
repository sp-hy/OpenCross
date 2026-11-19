// A character's all-uses model file (game/model_mods.h). Without game files: the battle joint map of two skeletons with the same joints is
// joint for joint, and binding a mesh onto another skeleton keeps every vertex's rest place and normal, merging two joints that become one.
// With the game install (SKIP without one): the priority of the files for Serge's Status-screen, field and battle models (a per-use file
// wins its use, the all-uses file the uses without one, the shipped model the rest), the battle model wearing the all-uses file keeping its
// own skeleton, clips, attach points, mounts and extent, and a file made for another character refused everywhere.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "assets/model.h"
#include "game/battle/model_table.h"
#include "game/menu_rig.h"
#include "game/model_export.h"
#include "game/model_files.h"
#include "game/model_mods.h"
#include "import/gltf_model.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

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

assets::Joint joint(int parent, const assets::Vec3& rot, const assets::Vec3& trans) {
  assets::Joint out;
  out.parent = parent;
  out.rest_rot_deg = rot;
  out.rest_quat = assets::quat_zyx(rot);
  out.rest_trans = trans;
  return out;
}

// A vertex's rest place on `skeleton`: its first joint's rest transform applied to its local place k.
gl::Vec3d rest_place(const assets::Skeleton& skeleton, const assets::SkinVertex& vertex, size_t k) {
  const std::vector<gl::Affine> rest = import::rest_world(skeleton);
  return gl::apply_point(rest[vertex.joint[k]], {vertex.local[k][0], vertex.local[k][1], vertex.local[k][2]});
}

bool near(const gl::Vec3d& lhs, const gl::Vec3d& rhs, double tolerance) {
  return std::fabs(lhs[0] - rhs[0]) <= tolerance && std::fabs(lhs[1] - rhs[1]) <= tolerance && std::fabs(lhs[2] - rhs[2]) <= tolerance;
}

void check_rebind() {
  assets::ModelAsset battle, menu;
  battle.skeleton.joints = {joint(-1, {0, 0, 0}, {0, -500, 0}), joint(0, {0, 0, 90}, {100, 0, 0}), joint(1, {0, 0, 0}, {0, 200, 0})};
  menu.skeleton.joints = {joint(-1, {0, 0, 0}, {0, -480, 0}), joint(0, {30, 0, 0}, {120, 0, 0}), joint(1, {0, 45, 0}, {0, 150, 0})};
  std::string error;
  const std::vector<int> map = game::battle_joint_map(battle, menu, error);
  check(map == std::vector<int>{0, 1, 2} && error.empty(), "the same joints and parents pair joint for joint");

  assets::Mesh mesh;
  assets::SkinVertex one;
  one.joint = {2, 2};
  one.local = {assets::Vec3{10, 20, 30}, assets::Vec3{10, 20, 30}};
  one.normal = {0, 0, 127};
  assets::SkinVertex two;
  two.joint = {1, 2};
  two.weight = {0.25f, 0.75f};
  two.local = {assets::Vec3{5, 0, 0}, assets::Vec3{0, -7, 3}};
  two.normal = {127, 0, 0};
  two.normal_joint = 1;
  mesh.pool = {one, two};
  const std::vector<assets::Mesh> moved = game::rebind_meshes({mesh}, menu.skeleton, battle.skeleton, map);
  check(moved.size() == 1 && moved[0].pool.size() == 2, "every vertex is kept");
  if (moved.size() != 1 || moved[0].pool.size() != 2) return;
  for (size_t i = 0; i < 2; ++i)
    for (size_t k = 0; k < 2; ++k)
      check(near(rest_place(battle.skeleton, moved[0].pool[i], k), rest_place(menu.skeleton, mesh.pool[i], k), 1e-3),
            "a vertex keeps its rest place on the battle skeleton");
  const std::vector<gl::Affine> menu_rest = import::rest_world(menu.skeleton), battle_rest = import::rest_world(battle.skeleton);
  const gl::Vec3d before = gl::apply_vector(menu_rest[2], {0, 0, 127});
  const gl::Vec3d after = gl::apply_vector(battle_rest[moved[0].pool[0].joint[moved[0].pool[0].normal_joint]],
                                           {static_cast<double>(moved[0].pool[0].normal[0]), static_cast<double>(moved[0].pool[0].normal[1]),
                                            static_cast<double>(moved[0].pool[0].normal[2])});
  check(near(before, after, 1.5), "a normal keeps its rest direction");

  // Menu joints 1 and 2 both carried by battle joint 1: the two-joint vertex becomes one joint at its blended rest place.
  const std::vector<int> folded{0, 1, 1};
  const std::vector<assets::Mesh> merged = game::rebind_meshes({mesh}, menu.skeleton, battle.skeleton, folded);
  const assets::SkinVertex& vertex = merged[0].pool[1];
  check(vertex.joint[0] == 1 && vertex.joint[1] == 1 && vertex.weight[0] == 1.0f && vertex.weight[1] == 0.0f && vertex.normal_joint == 0,
        "two joints that become one are merged");
  gl::Vec3d blend{};
  for (size_t axis = 0; axis < 3; ++axis)
    blend[axis] = 0.25 * rest_place(menu.skeleton, mesh.pool[1], 0)[axis] + 0.75 * rest_place(menu.skeleton, mesh.pool[1], 1)[axis];
  check(near(rest_place(battle.skeleton, vertex, 0), blend, 1e-3), "a merged vertex sits at its blended rest place");
}

bool write_file(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

bool same_attach(const assets::ModelAsset& lhs, const assets::ModelAsset& rhs) {
  for (size_t i = 0; i < lhs.attach.points.size(); ++i)
    if (lhs.attach.points[i].joint != rhs.attach.points[i].joint || lhs.attach.points[i].pos != rhs.attach.points[i].pos) return false;
  for (size_t i = 0; i < lhs.attach.mounts.size(); ++i)
    if (lhs.attach.mounts[i].attach != rhs.attach.mounts[i].attach || lhs.attach.mounts[i].pos != rhs.attach.mounts[i].pos ||
        lhs.attach.mounts[i].rot != rhs.attach.mounts[i].rot)
      return false;
  return true;
}

int vertices(const assets::ModelAsset& asset) {
  int count = 0;
  for (const assets::Mesh& mesh : asset.meshes) count += static_cast<int>(mesh.pool.size());
  return count;
}

void check_priority(vfs::Vfs& files, const std::filesystem::path& dir) {
  constexpr int kSerge = 0, kKid = 1;
  const std::string name = game::menu_model_stem(kSerge), battle_stem(game::kBattleModels[kSerge]);
  files.set_hd_mode(true);
  std::string err;
  const auto shipped_menu = game::read_menu_model(files, kSerge, true, &err);
  const auto shipped_battle = game::read_model_files(files, battle_stem, &err);
  const auto kid_menu = game::read_menu_model(files, kKid, true, &err);
  check(shipped_menu && shipped_battle && kid_menu, "the shipped models read");
  if (!shipped_menu || !shipped_battle || !kid_menu) return;
  const std::filesystem::path all = dir / "all.glb", menu = dir / "menu.glb", battle = dir / "battle.glb", other = dir / "kid.glb";
  check(write_file(all, game::model_to_glb(*shipped_menu, true)) && write_file(menu, game::model_to_glb(*shipped_menu, true)) &&
            write_file(battle, game::model_to_glb(*shipped_battle, true)) && write_file(other, game::model_to_glb(*kid_menu, true)),
        "the test files are written");
  auto label = [](const std::filesystem::path& path) { return vfs::OverlayFile{path, {}}.label(); };
  const std::string all_key = "hd/" + game::character_model_stem(name), menu_key = "hd/menu/player/" + name, battle_key = "hd/" + battle_stem;
  auto mount = [&](std::vector<std::pair<std::string, std::filesystem::path>> models) {
    vfs::Overlay overlay;
    for (auto& [key, path] : models) overlay.models[key] = {path, {}};
    files.set_overlay(std::move(overlay));
  };
  auto status = [&] { return game::read_menu_model(files, kSerge, true, &err); };
  auto fight = [&] { return game::read_model_files(files, battle_stem, &err); };

  mount({{all_key, all}});
  check(game::has_character_model(files, name, true) && !game::has_character_model(files, name, false), "the all-uses file is found for its art set");
  auto drawn = status();
  check(drawn && drawn->mesh_mod == label(all), "the Status screen wears the all-uses file");
  auto worn = fight();
  check(worn && worn->mesh_mod == label(all), "battle wears the all-uses file");
  if (worn && drawn) {
    check(vertices(*worn) == vertices(*drawn) && worn->meshes.size() >= drawn->meshes.size(), "battle draws the Status-screen meshes");
    check(game::same_skeleton(worn->skeleton, shipped_battle->skeleton) && worn->clips.size() == shipped_battle->clips.size() &&
              worn->extent == shipped_battle->extent && same_attach(*worn, *shipped_battle) && worn->anim_seq == shipped_battle->anim_seq,
          "battle keeps its skeleton, clips, attach points, mounts and extent");
    check(worn->textures.size() == drawn->textures.size() && worn->character.empty(), "battle draws the Status-screen textures");
  }
  const auto field = game::read_model_files(files, "fieldobj/player/" + name + "/" + name, &err);
  check(field && field->mesh_mod.empty(), "the field model itself keeps its meshes (the field dresses it in the Status-screen model)");
  const auto all_only = game::read_menu_model(files, kSerge, true, &err, game::MenuModelMods::AllUses);
  check(all_only && all_only->mesh_mod == label(all), "the all-uses read takes the all-uses file");

  mount({{all_key, all}, {menu_key, menu}});
  drawn = status();
  check(drawn && drawn->mesh_mod == label(menu), "a Status-screen file wins the Status screen");
  const auto field_body = game::read_menu_model(files, kSerge, true, &err, game::MenuModelMods::AllUses);
  check(field_body && field_body->mesh_mod == label(all), "the all-uses read skips the Status-screen file");
  worn = fight();
  check(worn && worn->mesh_mod == label(all), "battle keeps the all-uses file beside a Status-screen file");

  mount({{all_key, all}, {battle_key, battle}});
  worn = fight();
  check(worn && worn->mesh_mod == label(battle), "a battle file wins battle");
  drawn = status();
  check(drawn && drawn->mesh_mod == label(all), "the Status screen keeps the all-uses file beside a battle file");

  mount({{all_key, other}});
  drawn = status();
  worn = fight();
  check(drawn && drawn->mesh_mod.empty() && worn && worn->mesh_mod.empty() && vertices(*worn) == vertices(*shipped_battle),
        "another character's file is refused, the shipped models drawn");

  files.set_hd_mode(false);
  mount({{all_key, all}});
  worn = fight();
  check(worn && worn->mesh_mod.empty(), "an hd file is not used in the original art");
  files.set_overlay({});
}

}  // namespace

int main() {
  check_rebind();
  const char* configured = std::getenv("OPENCROSS_GAMEDIR");
  const auto game_dir = platform::find_game_dir(configured ? configured : "");
  vfs::Vfs files;
  if (game_dir.empty() || !files.open(game_dir)) {
    std::printf("SKIP: no game install (priority checks)\n");
  } else {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("oc_all_uses_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    check_priority(files, dir);
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("all-uses model checks passed\n");
  return 0;
}
