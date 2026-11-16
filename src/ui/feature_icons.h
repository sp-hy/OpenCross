#pragma once
// RDE's play-time feature icons: one icon per active toggle from ui.dat ui/exticon_<lang>.png (256 x 256, cells of 64 px: row 0
// encounters off / auto battle / battle boost, row 2 fast / slow), drawn 53 x 53 on the 1280 x 960 canvas. The column starts
// at x 20, bottom edge y 877 and grows upwards 63 units per icon in the order auto battle, battle boost, encounters off, slow, fast.
// When the window leaves more than 75 canvas units of black beside the 4:3 picture the column moves 93 units out into the left bar.
// After an auto-save the "AUTO SAVE" strip (atlas row 1, drawn 212 x 53 at x 83 beside the column's first slot, 80 % opaque) shows for
// a 120-frame count that runs down once per 60 Hz frame, twice while no toggle is on (so 2 s with icons, 1 s without).
//   icons.init(ctx, vfs, sprites);   icons.show_autosave();   per frame: icons.draw(frame, vw, vh, active, lang);
#include <chrono>

#include "import/text.h"
#include "render/sprite_batch.h"
#include "render/texture.h"
#include "vfs/vfs.h"

namespace oc::ui {

class FeatureIcons {
 public:
  struct Active {
    bool auto_battle = false, boost = false, encounters = false, slow = false, fast = false;
  };

  FeatureIcons() = default;
  ~FeatureIcons() { shutdown(); }
  FeatureIcons(const FeatureIcons&) = delete;
  FeatureIcons& operator=(const FeatureIcons&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites);
  void shutdown();
  void show_autosave() { autosave_ = 120.0; }
  // Draws over the finished frame (inside no begin / end). The atlas of `lang` is loaded on first use.
  void draw(const render::Frame& frame, float vw, float vh, const Active& active, import::Lang lang);
  // Toggle icons the last draw put on screen (the AUTO SAVE strip is not counted).
  int shown() const { return shown_; }

 private:
  void load(import::Lang lang);

  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::Sampler linear_;
  render::Texture atlas_;
  render::TexRef atlas_set_ = {};
  int loaded_lang_ = -1;
  int shown_ = 0;
  // The AUTO SAVE strip's remaining 60 Hz frames, run down by the real time between draws.
  double autosave_ = 0.0;
  std::chrono::steady_clock::time_point last_{};
};

}  // namespace oc::ui
