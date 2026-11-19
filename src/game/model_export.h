#pragma once
// Model export to glTF 2.0 binary (.glb): field, battle and Status-screen models with their skeleton, skinning, textures and every
// animation clip, and the weapon pack parts as rigid meshes. Works on the decoded assets only (no GPU), so the "Export all models" job
// (ModelExportJob, started by the Mods tab) runs on a worker thread; `occ export-models` runs it blocking. It reads the shipped
// archives through its own Vfs (no mods). Joints keep the model's own rest transforms under one "root" node that turns the model
// into glTF's +y up axes; joint k is node "joint_<kk>", clip id n is animation "clip_<nn>", so a file maps back onto the game's skeleton.
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "assets/model.h"
#include "core/types.h"
#include "import/gltf_model.h"

namespace oc::game {

using import::kGltfScale;

// The id the game's scripts start a clip with: an internal (S4) clip by its index, an external one (at0, ex) as 0x80 + its index.
u32 clip_id(const assets::AnimationClip& clip);
// The animation name of a clip id: "clip_" and two (or more) hex digits.
std::string clip_name(u32 id);

struct GlbStats {
  u32 vertices = 0, triangles = 0, joints = 0, clips = 0, images = 0;
};
// The .glb of a model. `hd`: a texture with an HD image is written as that image. Rigid meshes (mesh.skinned false) get no skin.
Bytes model_to_glb(const assets::ModelAsset& asset, bool hd, GlbStats* stats = nullptr);
// A weapon pack part as a model: its rigid mesh and its texture, no skeleton.
assets::ModelAsset weapon_part_asset(const assets::WeaponPart& part, const std::string& source);

struct GlbCheck {
  std::string error;
  GlbStats stats;
  // Poses compared (keyframes over every clip) and the largest differences, in model units: joint origins, and the skinned
  // corners against the engine's skinning (per-influence local positions), split into one-joint corners and two-joint corners (the file
  // has one bind position where the model has a local position per joint; the two agree to a few units in the rest pose).
  u32 poses = 0;
  double joint_error = 0, vertex_error = 0, split_error = 0;
  // Share of the triangles whose counter-clockwise side (glTF's front face) faces the way their vertex normals point.
  double facing = 0;
  bool ok() const { return error.empty(); }
};
// Reads `glb` back and checks it against `asset`: accessor counts, joints and clips; then at every keyframe of every clip the joint
// matrices from the file's nodes and animation samples against the engine's pose, and the skinned corners.
GlbCheck check_glb(const assets::ModelAsset& asset, ByteSpan glb);

struct ImportCheck {
  std::string error;
  // Corners compared (corners x keyframes over every clip) and the largest distance of an imported corner from the model's, in model units:
  // one-joint corners and two-joint corners apart. `faces`: constructs whose triangles, quad halves or materials differ.
  u64 corners = 0;
  double vertex_error = 0, split_error = 0;
  u32 faces = 0;
  bool ok() const { return error.empty(); }
};
// Reads `glb` back as a mod's model replacement of `asset` (import::load_gltf_model) and compares the result with the model at every keyframe
// of every clip.
ImportCheck check_import(const assets::ModelAsset& asset, ByteSpan glb);

// kExportClassic / kExportHd: the art sets (none = both). kExportVerify: every file is read back with check_glb, and every skinned model
// also with check_import.
enum ModelExportSet : unsigned {
  kExportField = 1, kExportBattle = 2, kExportMenu = 4, kExportWeapons = 8,
  kExportAll = kExportField | kExportBattle | kExportMenu | kExportWeapons,
  kExportClassic = 16, kExportHd = 32, kExportVerify = 64
};

struct ModelExportProgress {
  std::atomic<u32> done{0}, total{0};
  // Files written so far and their size.
  std::atomic<u32> files{0};
  std::atomic<u64> bytes{0};
  std::atomic<bool> running{false}, finished{false}, cancel{false};

  std::string phase() const;
  void set_phase(std::string name);
  // One line, available after finished.
  std::string summary() const;
  void set_summary(std::string line);

 private:
  mutable std::mutex mutex_;
  std::string phase_, summary_;
};

// Blocking. Writes <out_dir>/<classic|hd>/<member stem>.glb (menu/player/<name>.glb, battle/wepon/<pack>/part<n>.glb). `only` not empty:
// just the models whose stem contains it. Returns 0 when the game files open (models that fail to load are skipped and counted).
int run_model_export(const std::filesystem::path& game_dir, const std::filesystem::path& out_dir, unsigned what, ModelExportProgress& progress,
                     const std::string& only = {});

class ModelExportJob {
 public:
  ~ModelExportJob() {
    request_cancel();
    join();
  }
  // Into <game_dir>/dump/models. False when already running.
  bool start(const std::filesystem::path& game_dir, unsigned what = kExportAll);
  void request_cancel() { progress_.cancel = true; }
  bool running() const { return progress_.running; }
  ModelExportProgress& progress() { return progress_; }
  void join();

 private:
  ModelExportProgress progress_;
  std::thread thread_;
};

}  // namespace oc::game
