#pragma once
// RDE pause screen: the frozen game picture under a magenta colour filter, a slanted bar "PAUSE" and below it the option row
// "Vibration Function : On" with the small left / right arrows at the bar's right end; on a map with a world name (script var 0x50)
// once the scenario has reached 0x34 (Viper Manor, the first talk with Melchior) a second row "World Names : On" with the arrows
// turned to up / down. Left / right flip Vibration, up / down flip World Names (no cursor). Text = lang.dat ui_pausemenu.txt (ids
// 0 PAUSE, 1 Vibration Function, 2 World Names, 3 Off, 4 On), bar art = ui.dat ui/pause.png, font ui_font_gnb (HD) in both art modes,
// in Japanese kb_70 (the gnb face has no kana or kanji).
// Played from the PS1 discs (no ui archive; the PS1 has no pause screen) the same rows are PS1-font text in one PS1 menu window
// (menutim.prd "wind" frame over a translucent black fill), centred on the 320 x 240 picture, the build's own words (no lang.dat).
//   p.open(vibration, world_row, world_names);   per frame: sfx = p.input(dx, dy, toggle, close);   p.draw(...)
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "import/text.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "render/texture.h"
#include "ui/ps1_font.h"
#include "vfs/vfs.h"

namespace oc::ui {

class PauseScreen {
 public:
  PauseScreen() = default;
  ~PauseScreen() { shutdown(); }
  PauseScreen(const PauseScreen&) = delete;
  PauseScreen& operator=(const PauseScreen&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text);
  void shutdown();
  void set_language(import::Lang lang) { lang_ = lang; }

  void open(bool vibration, bool world_row = false, bool world_names = true);
  void close() { open_ = false; }
  bool active() const { return open_; }
  bool vibration() const { return vibration_; }
  // The World Names value (meaningful only while the row is shown; RDE writes it back only then).
  bool world_row() const { return world_row_; }
  bool world_names() const { return world_names_; }
  // The filter colour of this frame: a counter 0..479 advances once per frame (60 per second here, kept across openings as RDE's
  // global counter is), ramp = counter < 240 ? counter : 480 - counter, t = ramp / 240 * 0.53, colour = (1 - t, 0.47, 0.47 + t).
  void tint(float (&rgb)[3]);

  // One frame of input (edges): dx or `toggle` flips Vibration, dy flips World Names (when its row is shown), `close` leaves.
  // Returns the menu sound effect id to play (0 none, 1 cursor / toggle, 6 cancel).
  int input(int dx, int dy, bool toggle, bool close);
  // Draws over the frame (inside no begin / end); `resume` continues the frame's sprite pass as the other panels do.
  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);

 private:
  // A spritefont face of the pause text, `scale` = canvas units per font texel, `ink_mid` = the font-texel offset from the pen's
  // line top to the middle of the capital P's glyph box; `spacing` = canvas units added after every glyph; `arrow_gap` / `title_pad`
  // = canvas units from the text's pen end to the arrow pair / to the title bar's right end.
  struct Face {
    std::shared_ptr<const import::SpriteFont> font;
    render::FaceId id = render::kNoFace;
    float scale = 1.0f, ink_mid = 0.0f, spacing = 0.0f, arrow_gap = 0.0f, title_pad = 0.0f;
  };
  // Row text `id` of ui_pausemenu, else the English / Japanese words.
  std::u32string ui_text(int id);
  // gnb sized so that its P fills the bar as RDE's does; kb_70 for Japanese, read on its first use.
  bool make_face(Face& face, const std::string& member, std::u32string_view extra, float spacing, float arrow_gap, float title_pad);
  const Face& face();
  void draw_ps1(const render::Frame& frame, float vw, float vh, bool resume, const std::u32string& title, const std::u32string& line,
                const std::u32string& line2);

  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::TextBatch* text_ = nullptr;
  render::Sampler linear_, nearest_;
  render::Texture white_, bar_, wind_;
  render::TexRef white_set_ = {}, bar_set_ = {}, wind_set_ = {};
  // The disc look: PS1 font + window frame instead of the RDE bars.
  bool ps1_ = false;
  float wind_w_ = 0, wind_h_ = 0;
  float bar_w_ = 0, bar_h_ = 0;
  // The disc look's PS1 font.
  render::FaceId face_ = render::kNoFace;
  // ui/ui_font_gnb.spritefont (the bold condensed face) and kb_70 for Japanese.
  Face latin_, jp_;
  bool jp_tried_ = false;
  import::LangTables lt_;
  import::Lang lang_ = import::Lang::En;
  bool open_ = false, vibration_ = true, world_row_ = false, world_names_ = true;
  // Frames.
  double counter_ = 0;
  std::chrono::steady_clock::time_point last_{};
};

}  // namespace oc::ui
