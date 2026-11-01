#pragma once
// Battle stage runtime: the stage's type 0x0A object scripts run in the generic emitter engine (effects::EmitterSystem), and the
// mesh objects they spawn are drawn as StagePipeline instances with the object's matrix, colour, blend, OT bias and cull side.
// This is how the PS1 draws a battlefield: there is no
// separate "stage renderer"; scenery, sky domes and the ring are persistent objects (LIFE 0) of emitters named far / near / ring,
// and the intro bugs / smoke are ordinary particles of the same scripts.
//
// Unit drop shadows: the footprint rectangle of the unit's model
// (x/z bounds padded by 0x80) plus the point where the stage light casts the model's top onto the floor form a convex hull;
// its corners are rounded and the hull is filled with Gouraud rings drawn *subtractively* (B - F), dark at the centre and
// zero at the rim. The light is the stage's `sun0` object position, by default a point 3072 units above the
// arena centre, so shadows point away from the centre. The shadow fades with the unit's height above the floor.
// [Enhancements] Lighting replaces them with the units' real shapes on the floor (render/shadow_pipeline.h) over a faint contact hull.
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/battle_stage.h"
#include "assets/model.h"
#include "assets/room_effects.h"
#include "core/types.h"
#include "game/battle/stage_key_light.h"
#include "game/effects/bespoke/bespoke.h"
#include "game/effects/emitter_system.h"
#include "game/effects/gpu_effects.h"
#include "render/camera.h"
#include "render/gpu_context.h"
#include "render/particles.h"
#include "render/shadow_pipeline.h"
#include "render/sprite_batch.h"
#include "render/stage_pipeline.h"

namespace oc::game {

class StageScene {
 public:
  StageScene() = default;
  ~StageScene() { clear(); }
  StageScene(const StageScene&) = delete;
  StageScene& operator=(const StageScene&) = delete;

  // Loads the stage drp (+ optional ctd), uploads the meshes and starts the object scripts. `OC_STAGE_ONLY=a,b` keeps only the
  // named parts (debug). Returns false (with `err`) when the stage does not parse; the view then runs without scenery.
  // `sprites` + `libs` (optional) enable the type-3 sprite sheets of the object scripts (smoke / dust / sparks: `smok`, `kona`, `tam4` ... live in
  // battle.prd's resident packs and effect/particle.prd): the libraries are searched after the stage's own entries.
  bool load(render::GpuContext& ctx, render::StagePipeline& pipe, ByteSpan prd, ByteSpan ctd, std::string* err = nullptr,
            render::SpriteBatch* sprites = nullptr, std::vector<const assets::RoomEffects*> libs = {});
  // The same load in phases: load_begin(), then load_step(budget_ms) every frame until it stops returning More / Wait (a phase is never split,
  // the sheet atlases are). `prd` / `ctd` must outlive the calls. Failed: the scene is cleared and `err` set. The stage decode (with the mods'
  // tile replacements) runs on a worker thread: Wait = it is still running, call again next frame; a budget >= 1e6 ms waits for it instead.
  enum class LoadResult { More, Wait, Done, Failed };
  void load_begin(render::GpuContext& ctx, render::StagePipeline& pipe, ByteSpan prd, ByteSpan ctd, render::SpriteBatch* sprites = nullptr,
                  std::vector<const assets::RoomEffects*> libs = {});
  LoadResult load_step(double budget_ms, std::string* err = nullptr);
  // The position shift the stage's own emitters run with: 3 (default: field / title), 5 in battle.
  // Takes effect at the next load().
  void set_pos_shift(int shift) { pos_shift_ = shift; }
  void clear();
  bool ready() const { return mesh_ != nullptr; }

  // One 60 Hz tick of the object scripts.
  void tick();
  // Sequence op 0x22: starts an emitter by name.
  bool start_emitter(std::string_view name);

  // Model-local x/z bounds of the unit's joints (the joint positions of the posed skeleton, not the mesh vertices;
  // model space, feet at the origin).
  struct Footprint {
    float min_x = -256, max_x = 256, min_z = -256, max_z = 256;
  };
  // A unit that casts a drop shadow this frame. Positions are stage world units (y up); `yaw` is the model yaw in radians as
  // used for drawing (BattleView: rotation_y(atan2(-face_x, -face_z))); `height` the model's extent above the feet; `y` the
  // height of the feet above the floor (0 on the ground); `fp` the joint bounds of the current pose (`has_fp`), else `model`
  // supplies them from the bind pose (cached). `owner` + `index`: what casts it (the view's unit, its pseudo-actor, its showcase model).
  enum class CasterOwner { Unit, Pseudo, Showcase };
  struct ShadowCaster {
    float x = 0, z = 0, y = 0, yaw = 0, height = 1000;
    Footprint fp;
    bool has_fp = false;
    const assets::ModelAsset* model = nullptr;
    CasterOwner owner = CasterOwner::Unit;
    int index = 0;
  };
  void set_shadow_casters(std::vector<ShadowCaster> casters) { casters_ = std::move(casters); }

  // Draws the sprite-sheet objects (type 3) of the scene in ordering-table order, depth tested against what was drawn before: call after the
  // units, before the spell effects (`scissor` = the 320 x 224 draw area).
  void draw_sprites(const render::Frame& frame, render::Rect scissor, const effects::ViewParams& view, const render::Letterbox& lb, float alpha);
  size_t drawn_sprites() const { return sprites_drawn_; }

  // Draws the live mesh objects, then the unit shadows. `view` is the battle camera (effects::ViewParams: rotation rows right /
  // down / forward, translation, H, GTE offset), `lb` the mapping of the PS1 screen (BattleView::picture() or a 320 x 224 target), `alpha` the interpolation factor of the tick,
  // `linear` bilinear sampling (HD art mode).
  void draw(render::StagePipeline& pipe, const render::Frame& frame, render::Rect scissor, const effects::ViewParams& view, const render::Letterbox& lb,
            float alpha, bool linear = false);

  // GTE depth cue of the stage objects: `active` = the fog flag (sequence op 0x24 sets it, 0x6C clears
  // it), the colour / range are the far colour and SetFogNearFar state (effects op 0x2D). Objects with flag bit 17 blend toward the far colour.
  void set_depth_cue(const render::StagePipeline::DepthCue& cue) { cue_ = cue; }
  // [Enhancements] SpellLights: the lights of the running effect (render world), applied to the opaque scenery of the next draw(); empty = off.
  void set_spell_lights(const render::LightSet& lights) { spell_lights_ = lights; }
  // [Enhancements] Lighting: the next draw() lays the mask of `shadows` on the floor over `rect` (ShadowPipeline::Floor::rect) and keeps only a faint
  // contact hull under each unit; null = the PS1's drop shadows.
  void set_floor_shadow(render::ShadowPipeline* shadows, const std::array<float, 4>& rect) {
    floor_shadow_ = shadows;
    floor_rect_ = rect;
  }
  const KeyLight& key_light() const { return key_light_; }
  // Packets dropped by the GTE / GPU rules in the last draw().
  size_t rejected_packets() const { return rejected_; }

  const assets::StageAsset* asset() const { return asset_.get(); }
  size_t live_objects() const { return em_ ? em_->live_objects() : 0; }
  // The stage's emitter engine (pack ops 0x26 / 0x27 / 0x36..0x3D / 0x5F / 0x60 switch its near / far / ring / roof / bti0 layers).
  effects::EmitterSystem* emitters() { return em_.get(); }
  size_t drawn_instances() const { return drawn_; }
  // Stage light position in world units (y up): the `sun0` object's position or the default (0, 3072, 0).
  std::array<float, 3> light() const { return light_; }

  // ---- The battle ground (the effect overlays reach it through effects::fx_ground()) ----
  // The 10 x 10 cell records built from the stage's 'ring' mesh, present = the stage has a ring.
  const effects::FxGround& ground() const { return ground_; }
  effects::FxGround& ground() { return ground_; }
  // StageAsset texture (atlas) a cell samples: its tpage colour mode + CLUT (-1 = none). Texel (u, v) of the cell's page lies at atlas
  // x = (tpage x - asset()->atlas_x0_hw) * (4 at 4bpp, 2 at 8bpp) + u, y = v.
  int ground_cell_texture(int cell) const;
  // Draws the ground this tick without the cells x >= rect[0] && x < rect[2] && z > rect[3] && z <= rect[1] (s16 rect as the
  // overlays pass it). Cleared with clear_ground_frame(); the overlays rebuild it every tick.
  void hide_ground_rect(const std::array<s16, 4>& rect) { ground_.draws.push_back(rect); }
  void clear_ground_frame() {
    ground_.draws.clear();
    ground_.prims.clear();
  }
  // Skip: the stock ground draw leaves the ground out.
  void set_ground_skip(bool skip) { ground_.skip_stock = skip; }
  // The ground's ordering-table bias (default 100).
  int ground_bias() const { return ground_.bias; }
  void set_ground_bias(int bias) { ground_.bias = bias; }
  // Sequence ops 0x6D (target given) / 0x6E (target 100) over `ticks`: one step at once and one per tick.
  void ground_bias_ramp(int ticks, int target);
  // The overlay was freed: skip off, bias 100, ramp and per-tick lists cleared.
  void ground_overlay_end();

  // Shadow geometry of one caster in stage world space (y up, the floor at y = 0), as StagePipeline vertices (colour = the
  // subtracted grey / 255 x `grey_scale`) and triangle indices appended to the vectors. Exposed for tools / tests.
  static void build_shadow(const ShadowCaster& caster, const Footprint& fp, const std::array<float, 3>& light, std::vector<assets::StageVertex>& verts,
                           std::vector<u32>& indices, float grey_scale = 1.0f);

 private:
  const Footprint& footprint(const assets::ModelAsset* model);

  // The worker's part of the load: the decoded, baked and tile-replaced stage and its textures.
  struct Prepared {
    std::optional<assets::StageAsset> stage;
    std::string error;
    // False under OC_NO_BAKE (no tile library: no live tile replacement).
    bool baked = false;
    // texture_registry::generation() before the replacements were read.
    unsigned generation = 0;
    render::StageTextures textures;
  };
  static Prepared prepare(render::GpuContext& ctx, const Bytes& prd, const Bytes& ctd, const std::string& group);
  struct LoadState {
    bool on = false;
    int phase = 0;
    std::future<Prepared> prepared;
    // Prepared::textures until the mesh job takes them.
    render::StageTextures textures;
    render::StagePipeline* pipe = nullptr;
    std::unique_ptr<render::StageCreateJob> mesh_job;
    ByteSpan prd, ctd;
    render::SpriteBatch* sprites = nullptr;
    std::vector<const assets::RoomEffects*> libs;
  } ld_;
  std::shared_ptr<assets::StageAsset> asset_;
  assets::RoomEffects fx_;
  effects::EffectBank bank_;
  std::unique_ptr<effects::EmitterSystem> em_;
  std::unique_ptr<render::StageMesh> mesh_;
  // Mesh name -> part: sprite type 14 (drp type 0x02) / type 19 (drp type 0x12 ring).
  std::map<std::string, int> parts_, ground_parts_;
  std::vector<render::StageInstance> inst_;
  std::vector<ShadowCaster> casters_;
  std::map<const assets::ModelAsset*, Footprint> footprints_;
  std::vector<assets::StageVertex> shadow_verts_;
  std::vector<u32> shadow_idx_;
  std::array<float, 3> light_{0, 3072, 0};
  KeyLight key_light_;
  render::ShadowPipeline* floor_shadow_ = nullptr;
  std::array<float, 4> floor_rect_{};
  render::GpuContext* ctx_ = nullptr;
  size_t drawn_ = 0, rejected_ = 0, sprites_drawn_ = 0;
  effects::GpuEffects gpu_;
  render::ParticleBatch particles_;
  bool sprites_ok_ = false, particles_ready_ = false;
  int pos_shift_ = 3;
  render::StagePipeline::DepthCue cue_;
  render::LightSet spell_lights_;
  effects::FxGround ground_;
  int ground_part_ = -1;
  // tile_group_: texture group of the loaded stage ("battle_stage/bg_NN"), for live tile replacement.
  // tile_gen_: texture_registry::generation() the object images were composed for.
  // ground_rgb_: the ring object's colour (copied into the ground every frame).
  std::string tile_group_;
  unsigned tile_gen_ = 0;
  std::array<u8, 3> ground_rgb_{128, 128, 128};
  struct BiasRamp {
    bool on = false;
    int elapsed = 0, ticks = 0, from = 100, to = 100;
  } ramp_;
  void ramp_step();
  std::vector<render::StagePrim> prims_;
  // ground_.warp in the pipeline's vertex type.
  std::function<void(std::array<float, 3>&)> warp_fn_;
  // The emitters whose live objects carry the 14bar heat wave's mark.
  std::vector<std::string> marked_emitters_;
  bool warp_objects_marked_ = false;
};

}  // namespace oc::game
