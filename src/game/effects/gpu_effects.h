#pragma once
// GPU glue for the effect player: uploads every sprite sheet of an EffectBank as an atlas texture and draws the sorted
// DrawQuads of EmitterSystem::collect_quads() through render::ParticleBatch (call between the room pass and the HUD, after the
// layers / actors wrote their depth). Not exercised by the headless tests; the CPU twin is SoftCanvas.
#include <memory>
#include <unordered_map>
#include <vector>

#include "game/effects/emitter_system.h"
#include "render/camera.h"
#include "render/particles.h"
#include "render/sprite_batch.h"
#include "render/texture.h"
#include "render/texture_registry.h"

namespace oc::effects {

// One effect sprite sheet as the single atlas image the GPU gets (frames stacked vertically, width = the widest sprite) and its texture
// registry key (effects/<sheet name>/<content tag>.png; render/texture_registry.h). Returns false for a sheet without sprites.
bool sheet_atlas_image(const assets::SpriteSheet& sheet, u32& w, u32& h, std::vector<u8>& rgba);
render::TextureKey effect_sheet_key(const assets::SpriteSheet& sheet, u32 w, u32 h, const u8* rgba);

class GpuEffects {
 public:
  GpuEffects() = default;
  ~GpuEffects() { clear(); }
  GpuEffects(const GpuEffects&) = delete;
  GpuEffects& operator=(const GpuEffects&) = delete;

  bool init(render::GpuContext& ctx, render::SpriteBatch& sprites);
  // Builds one atlas per sheet of the bank (sprites stacked vertically, unmirrored RGBA).
  // `which` splits a bank's atlases between instances: a battle uploads the resident library sheets (battle.prd, the particle library;
  // hundreds of atlases) once into a LibrariesOnly instance that every effect pack's SkipLibraries instance reaches through set_shared().
  enum class Which { All, LibrariesOnly, SkipLibraries };
  bool load(const EffectBank& bank, Which which = Which::All);
  // The same upload in slices: load_begin() clears and makes the white texel, then load_more() uploads whole sheets until about
  // `budget_ms` of wall time is used (at least one sheet per call) and sets `done` when the bank is exhausted.
  bool load_begin();
  bool load_more(const EffectBank& bank, Which which, double budget_ms, bool& done);
  void set_shared(std::shared_ptr<const GpuEffects> shared) { shared_ = std::move(shared); }
  // HD art: a sheet with sprites that carry an HD image (EffectSprite::hd_rgba) is uploaded as a 4x atlas, those sprites from their HD
  // image and the others scaled up (nearest). Set before load.
  void set_hd(bool on) { hd_ = on; }
  void clear();

  // `quads` must be in draw order (sort_draw_order). Coordinates are layer space (320x240) mapped through `letterbox`.
  void draw(render::ParticleBatch& batch, const std::vector<DrawQuad>& quads, const render::Letterbox& letterbox) const;
  // One quad, for callers that merge other primitives into the same batch. An untextured quad (sprite type 15 'flat' / 'line') draws the
  // white texel with the colour halved, because the particle shader doubles the modulation (white * colour / 0x80); 'line' is Gouraud
  // from the near end (corners 0 / 2, r g b) to the far end (corners 1 / 3, r1 g1 b1). A sprite this instance did not upload is
  // forwarded to the shared instance.
  void draw_quad(render::ParticleBatch& batch, const DrawQuad& quad, const render::Letterbox& letterbox) const;
  // [Video] FilterEffects: bilinear (true) or nearest effect sprites; global (one field effect player), applies from the next draw.
  static void set_linear(bool on) { linear_ = on; }

 private:
  // One sprite inside its atlas: the atlas bound with the nearest and the bilinear sampler (empty when the
  // atlas has no bilinear variant: nearest only) and the sprite's UV window as fractions of the atlas.
  struct Entry {
    render::TexRef set;
    render::TexRef set_lin;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  };
  struct Atlas {
    render::Texture tex;
    render::TexRef set;
    render::TexRef set_lin;
  };
  static inline bool linear_ = false;
  bool hd_ = false;
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::Sampler sampler_;
  render::Sampler sampler_lin_;
  std::vector<std::unique_ptr<Atlas>> atlases_;
  // 1x1 white for the untextured built-in sprites.
  std::unique_ptr<Atlas> white_;
  std::unordered_map<const assets::EffectSprite*, Entry> entries_;
  // load_more() cursor.
  size_t cur_pack_ = 0, cur_sheet_ = 0;
  // Sprites not found in entries_ are looked up here.
  std::shared_ptr<const GpuEffects> shared_;
};

}  // namespace oc::effects
