#pragma once
// Status-screen 3D model: the member's MENU model (cdrom menu/player/<stem>.ome: cpt entry 1 = the eye-block positions, entry 2 = the
// nearest camera distance in ASCII, entry 3 = Mdl, entry 4 = the closed-eye TIM, entry 5 = Atim pair; HD 4x texture
// hd/menu/player/<stem>.tim) in its pose clip with the eye blink, the equipped weapon (the model's weapon pack, part = weapon record tier)
// hung on the model's mount records like a battle model, its flat floor shadow ([Enhancements] Lighting: the real shadow of its shape, from
// the same direction) and the status effects of menu/stef.prd. Drawn with
// MeshPipeline between the menu's parchment and the rest of its sprites (frames, text, swirls, key caps and legend go over it), through the
// menu's own camera and projection (MenuModelSlot).
#include <map>
#include <memory>
#include <random>
#include <string>

#include "assets/room_effects.h"
#include "game/actor.h"
#include "game/battle/battle_pose.h"
#include "game/effects/emitter_system.h"
#include "game/effects/gpu_effects.h"
#include "render/particles.h"
#include "render/shadow_pipeline.h"
#include "render/sprite_batch.h"
#include "render/texture.h"
#include "ui/menu/menu_model.h"
#include "vfs/vfs.h"

namespace oc::ui {

class StatusModelView : public MenuModelView {
 public:
  StatusModelView(render::MeshPipeline& meshes, const vfs::Vfs& vfs) : meshes_(meshes), vfs_(vfs) {}
  void draw(const render::Frame& frame, float vw, float vh, const MenuModelSlot& slot) override;
  int min_distance(int model) override;
  // [Enhancements] Lighting.
  void set_cast_shadows(bool on) { cast_shadows_ = on; }
  // [Shaders] the character shaders, on the model and its weapon.
  void set_character_shading(const render::CharacterShading& shading) { shading_ = shading; }
  // Drops the GPU models and effect textures before the pipeline is destroyed.
  void release();

 private:
  struct Entry {
    std::shared_ptr<game::ModelHandle> body;
    // The same model with the closed-eye texture page.
    std::shared_ptr<game::ModelHandle> body_shut;
    game::UnitPose pose;
    std::shared_ptr<game::ModelHandle> weapon;
    int weapon_model = -1, weapon_part = -2;
    // Interpolation between the last two menu ticks (0..1).
    float accum = 0;
    // Menu ticks since the screen (re)started the model.
    int ticks = 0;
    // The second of the tick's two pose steps is still due: it is taken half-way to the next tick.
    bool pose_half_due = false;
    int pose_id = -1;
    u32 session = 0;
  };
  // Cached per (menu model, art mode).
  Entry* entry(int model, bool hd_art);
  void set_weapon(Entry& shown, int weapon_model, int part);
  // One menu frame of the eye blink.
  void step_blink();
  // The status effects: menu/stef.prd restarted for a new session, the effects of the absent statuses stopped.
  void start_effects(const render::Frame& frame, u32 status);
  // `trans` = the camera translation (feet offset, pan, distance) the model is drawn with.
  void draw_effects(const render::Frame& frame, float vw, float vh, const MenuModelSlot& slot, const effects::Mat3& rotation, const effects::Vec3& trans,
                    float alpha);
  // The pipeline is made on first use: false = none, keep the flat shadow.
  bool shadow_pipe_ready(const render::Frame& frame);
  render::MeshPipeline& meshes_;
  render::ShadowPipeline shadow_pipe_;
  bool cast_shadows_ = false, shadow_pipe_failed_ = false;
  render::CharacterShading shading_;
  const vfs::Vfs& vfs_;
  std::map<int, std::unique_ptr<Entry>> cache_;  // key = model * 2 + hd_art
  std::map<std::string, std::shared_ptr<game::ModelHandle>> weapons_;  // "<pack>#<part>"
  // vfs::Vfs::generation() the caches were built for.
  u32 generation_ = 0;
  std::map<int, int> min_distance_;
  std::vector<render::Mat4> palette_;
  // The menu's stock environment page (an 8 bpp read of the menu's own frame buffer).
  render::Texture env_page_;
  render::Sampler env_sampler_;
  bool env_page_tried_ = false;
  // The menu tick of the previous draw.
  u32 last_tick_ = 0;
  bool eyes_shut_ = false;
  int blink_count_ = 0;
  std::minstd_rand blink_rng_;
  // Status effects.
  assets::RoomEffects effect_pack_;
  bool effect_pack_loaded_ = false;
  effects::EffectBank effect_bank_;
  std::unique_ptr<effects::EmitterSystem> emitters_;
  effects::GpuEffects effect_gpu_;
  render::SpriteBatch effect_sprites_;
  render::ParticleBatch particles_;
  bool effects_ready_ = false;
  u32 effect_session_ = 0;
  const assets::ModelAsset* effect_model_ = nullptr;
  std::vector<render::Mat4> effect_palette_;
};

}  // namespace oc::ui
