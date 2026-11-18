#pragma once
// The Grand Slam monster picker screen (field FE 75 + FE 61; rules: ui::MonsterPicker), a field-sized 320 x 240 screen drawn in the field
// picture's framing (render::Letterbox::field):
//   the tiled backdrop sprite (map/etc/systim.bin entry 4, 64 x 64, over 640 x 220), the room's own picker layers (background group 1:
//   the three boxes and the list window; the host draws them), the three picks' battle models in the boxes (MonsterModelView), the picks'
//   names centred under them, the 3 x 6 list of pool creatures in the list window (13 px rows, clipped, scrolling) and the cursor
//   (hd/menu/monmenu_cur.png, 5 frames of 8 menu frames) at the slot or at the list cell.
// Text is RDE's: the battle font (gn_70 / kt_70, large_ or small_ files by art mode), white 0x808080, taken entries red 0x000080, the
// selected slot and the list cursor's entry orange 0x285080; the cursor art is the same in both art modes. Played from the PS1 discs the
// names are the PS1 font at one font pixel per pixel over their black copy, white / red / orange as the PS1's sprite colours, and the
// cursor is the PS1's picture (map/etc/systim.bin entry 2).
//
//   s.open(pool, round, table); per frame: if (int sfx = s.update(input, dt)) play(sfx); s.draw(frame, vw, vh);
//   when s.take_finished(): s.result() = the pool slots of the three picks (-1 none); the table holds the round's list positions.
#include <array>
#include <functional>
#include <memory>
#include <string>

#include "core/types.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "ui/monster_pick.h"
#include "vfs/vfs.h"

namespace oc::ui {

// The three boxes' creatures for the host's model drawer (oc_ui cannot reach the battle models; ui/menu/picker_model.h draws them).
struct MonsterModelSlots {
  // Pool slot (= monster record) per box, -1 = none.
  std::array<int, 3> pool_slot{-1, -1, -1};
  // Window pixels of the screen point (0, 0) and of one PS1 pixel.
  float origin_x = 0, origin_y = 0, unit_x = 1, unit_y = 1;
  // 60 Hz frames since the screen opened.
  u32 tick = 0;
  bool hd = true;
};

class MonsterModelView {
 public:
  virtual ~MonsterModelView() = default;
  // Called with no SpriteBatch pass open.
  virtual void draw(const render::Frame& frame, float vw, float vh, const MonsterModelSlots& slots) = 0;
};

class MonsterPickScreen {
 public:
  // Draws the room's background layers (the picker's frames) into the open SpriteBatch pass.
  using Backdrop = std::function<void(render::SpriteBatch& sprites, float vw, float vh)>;

  MonsterPickScreen();
  ~MonsterPickScreen();
  MonsterPickScreen(const MonsterPickScreen&) = delete;
  MonsterPickScreen& operator=(const MonsterPickScreen&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text, import::Lang lang);
  void shutdown();
  // HD = the large_ font files and bilinear sprites; original = the small_ files.
  void set_hd(bool hd);
  void set_language(import::Lang lang);
  void set_model_view(MonsterModelView* view);
  void set_backdrop(Backdrop backdrop);

  void open(const std::array<u16, 16>& pool, int round, MonsterPicker::Table& table);
  bool active() const;
  // One update: input edges + key repeat, `dt` seconds of 60 Hz frames. Returns the sound effect to play (0 none).
  int update(const platform::Input& in, float dt);
  // True once, after the picks were written.
  bool take_finished();
  std::array<int, 3> result() const;

  void draw(const render::Frame& frame, float vw, float vh);

  // Routes: "mode slot col row top picks a b c" for assertions.
  std::string describe() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::ui
