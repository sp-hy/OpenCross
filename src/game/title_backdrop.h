#pragma once
// The title scene's underwater 3D backdrop: the field script of `opening` runs D3 PLAY_EFFECT "tit0".."tit3";
// those are type-0x10 sequences of the global `title.prd` (a stage-like drp: type-0x02 scenery meshes, 0x0A emitters that place them,
// 0x0C camera shots, child sequences) whose textures are the VRAM uploads of `title.ltd`. This class plays them: effect player
// (emitters + sequences) + battle camera (shots); the mesh objects are drawn like a battle stage (render::StagePipeline: GPU
// projection with near clipping, per-pixel depth, OT bias as depth bias, bilinear sampling in HD mode) and the sprite
// objects through the particle batch, depth tested against them, into the field pass. The model objects take the [Shaders] character shading
// and, with [Enhancements] Lighting, cast their shadows straight down onto the sea floor under them.
#include <array>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "assets/battle_stage.h"
#include "assets/room_effects.h"
#include "game/battle/battle_camera.h"
#include "game/battle/battle_pose.h"
#include "game/effects/emitter_system.h"
#include "game/effects/gpu_effects.h"
#include "game/effects/sequence_player.h"
#include "render/mesh_pipeline.h"
#include "render/particles.h"
#include "render/shadow_pipeline.h"
#include "render/sprite_batch.h"
#include "render/stage_pipeline.h"
#include "render/texture.h"
#include "vfs/vfs.h"

namespace oc::game {

// The title scene's mesh asset (cdrom/title.prd + title.ltd -> StageAsset, tex_group "title") without any GPU: the backdrop's own load and the
// texture dump tool (`opencross occ dump-textures title`). nullopt when the files are missing.
std::optional<assets::StageAsset> load_title_stage(const vfs::Vfs& vfs);

// The title's sprite-type-20 models (the type-0x0B records of title.prd: manta rays `ed9a`, turtle `ed9b`), each on the 128 x 256 page its
// record name binds, cut from `stage`'s VRAM atlas; source "@room_model" (texture dump / replace as room-model tiles). No GPU: the backdrop's
// own load and the texture dump. A record whose name is no binding or whose page is not in the atlas is skipped with a warning.
struct TitleModel {
  std::string name;
  // The binding the name decodes to: tpage (halfwords, rows) and CLUT.
  int tpage_x = 0, tpage_y = 0, clut_x = 0, clut_y = 0;
  assets::ModelAsset asset;
};
std::vector<TitleModel> load_title_models(ByteSpan prd, const assets::StageAsset& stage);

class TitleBackdrop {
 public:
  TitleBackdrop() = default;
  ~TitleBackdrop() { shutdown(); }
  TitleBackdrop(const TitleBackdrop&) = delete;
  TitleBackdrop& operator=(const TitleBackdrop&) = delete;

  // Loads title.prd / title.ltd and uploads the textures. False (and the backdrop stays inert) when a file is missing.
  bool init(const vfs::Vfs& vfs, render::GpuContext& ctx, render::SpriteBatch& sprites, render::MeshPipeline& meshes);
  void shutdown();
  bool ready() const { return ready_; }

  // D3 PLAY_EFFECT: restarts the scene with sequence `name` (tit0..tit3). False if the sequence does not exist.
  bool play(std::string_view name);
  // D7 STOP_EFFECT / leaving the title room.
  void stop();
  bool running() const { return running_; }

  // One 60 Hz simulation tick.
  void tick();
  // Draws into the open frame (the caller's field pass, before the room layers). `linear` = bilinear texture sampling (HD art
  // mode, like the battle stage). No-op while not running.
  void draw(const render::Frame& frame, float view_w, float view_h, float alpha, bool linear);
  // [Shaders] the character shading of the model objects.
  void set_character_shading(const render::CharacterShading& shading) { shading_ = shading; }
  // [Enhancements] Lighting: the model objects cast their shadows on the sea floor.
  void set_cast_shadows(bool on) { cast_shadows_ = on; }

  // Debug: the number of live emitter objects / the camera shot (log lines, tests).
  u32 live_objects() const;
  std::string shot() const { return camera_.shot_name(); }

 private:
  bool build_run(std::string_view first);
  effects::ViewParams view(float alpha) const;
  // A model object's model -> world matrix (the mesh pipeline's world, y up; see draw()).
  static render::Mat4 model_matrix(const effects::EmitterSystem::MeshInstance& mi);
  // [Enhancements] Lighting: finds the floor under every model object of `live` and draws their shadows into the mask (before any 3D of the
  // frame: the pass suspends it). Fills shadow_floors_.
  void cast_shadows(const render::Frame& frame, const effects::ViewParams& vp, float alpha, const std::vector<effects::EmitterSystem::MeshInstance>& live);
  // Inside the scenery pass, after the scenery: lays each shadow of shadow_floors_ on its floor.
  void draw_shadow_floors(const render::Mat4& proj, const effects::ViewParams& vp);

  bool ready_ = false, running_ = false;
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  Bytes prd_, lib_bytes_;
  assets::RoomEffects fx_, lib_;
  std::shared_ptr<assets::StageAsset> meshes_;
  effects::EffectBank bank_;
  std::unique_ptr<effects::EmitterSystem> em_;
  std::unique_ptr<effects::SequencePlayer> seq_;
  CamBank cams_;
  BattleCamera camera_;
  effects::GpuEffects gpu_;
  render::ParticleBatch particles_;
  render::StagePipeline stage_pipe_;
  std::unique_ptr<render::StageMesh> stage_mesh_;
  // texture_registry::generation() the title's object images were composed for.
  unsigned tile_gen_ = 0;
  std::vector<render::StageInstance> inst_;
  // Mesh name -> index into meshes_->parts.
  std::map<std::string, int> parts_;
  // Sprite type 20 objects: the drp's type-0x0B records are Mdl models (manta rays `ed9a`, turtle `ed9b`) drawn with the skinned
  // mesh pipeline, posed from their own motion clip.
  struct ModelRes {
    assets::ModelAsset asset;
    std::unique_ptr<render::MeshModel> gpu;
    UnitPose pose;
    int clip = -1;
  };
  bool load_models(render::MeshPipeline& meshes);
  // The model object's pose at the tick alpha into palette_.
  void pose_model(ModelRes& res, const effects::EmitterSystem::MeshInstance& mi, float alpha);
  // The mod overlay changed: the models' texture replacements again, from their own pages.
  void reload_model_textures();
  struct VramBlock {
    // Halfwords / rows of one title.ltd image upload.
    int x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<VramBlock> ltd_blocks_;
  render::MeshPipeline* mesh_pipe_ = nullptr;
  std::map<std::string, std::unique_ptr<ModelRes>> models_;
  std::vector<render::Mat4> palette_;
  bool models_logged_ = false;
  render::CharacterShading shading_;
  bool cast_shadows_ = false, shadow_pipe_failed_ = false;
  render::ShadowPipeline shadow_pipe_;
  // One shadow's floor: the plane y = height + slope_x (x' - x) + slope_z (z' - z) (world, +y down) under the model at (x, z); the shadow covers
  // x, z within `half` of the model, and its mask is centred on shadow_centre_.
  struct ShadowFloor {
    float x = 0, z = 0, height = 0, slope_x = 0, slope_z = 0, half = 0;
  };
  std::vector<ShadowFloor> shadow_floors_;
  std::array<float, 2> shadow_centre_{};
  // The floor triangles of this frame, three world corners (+y down) each.
  std::vector<std::array<float, 3>> floor_corners_;
  u64 ticks_ = 0;
};

}  // namespace oc::game
