#pragma once
// Title screen. Phases: fade in -> "Press any button" blink -> menu -> fade out.
//   Both art modes: hd/title/title.png (1280x960 logo splash), or the PS1 menu background
//   cdrom/menu/title/bg0.tim + bg1.tim (640x448); original art draws the text with the small_font_* files.
// Menu: New Game / Continue / Settings / Quit (variant 2: New Game + / Continue +). New Game and Continue fade to black before they are
// reported. New Game (+) first opens the vibration page: "On" / "Off" + the footer "Vibration Function"; confirm reports New Game
// with vibration() = the choice, cancel returns to page 0.
#include <functional>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "ui/title/canvas.h"
#include "ui/title/nav.h"

namespace oc::ui {

enum class TitleChoice { None, NewGame, Continue, Settings, Quit };

class TitleScreen {
 public:
  enum class Phase { FadeIn, Prompt, Menu, FadeOut };

  // Loads the art. `mixer` may be null (silent). `song` < 0 = no title music.
  bool init(UiKit& kit, audio::Mixer* mixer);
  void shutdown();

  // `backdrop`: a 3D scene (the title room) is drawn underneath: no fade-in, no opaque art, the screen starts at the prompt.
  void begin(import::Lang lang, ArtMode mode, bool has_saves, int song = -1, bool backdrop = false);
  // Title variant 2 (started from the ending room): the list "New Game +" / "Continue +", straight into the menu (no prompt), no cancel.
  void begin_plus(import::Lang lang, ArtMode mode);
  int variant() const { return variant_; }
  void set_mode(ArtMode mode) { mode_ = mode; }
  void set_language(import::Lang lang) { lang_ = lang; }
  void set_sfx(std::function<void(int)> fn) { sfx_ = std::move(fn); }

  void update(const Nav& nav, float dt);
  // Returns the pending choice once (New Game / Continue / Quit after the fade, Settings at once) and clears it.
  TitleChoice take_choice() {
    TitleChoice taken = choice_;
    choice_ = TitleChoice::None;
    return taken;
  }
  void draw(DrawList& dl) const;

  Phase phase() const { return phase_; }
  int selected() const { return sel_; }
  int item_count() const { return page_ == 1 || variant_ == 2 ? 2 : 4; }
  // page: 0 options, 1 the vibration page. vibration: the page's choice of the last New Game (On = true). fade_alpha: 1 = black.
  int page() const { return page_; }
  bool vibration() const { return vibration_; }
  float fade_alpha() const { return fade_; }
  // Test / screenshot hooks.
  void skip_to_menu() {
    phase_ = Phase::Menu;
    fade_ = 0;
    to_menu_ = menu_in_ = to_prompt_ = false;
    page_ = 0;
  }
  void select(int i) { sel_ = i; }
  bool backdrop() const { return backdrop_; }
  // Back to the prompt (the title room was reloaded after an attract demo).
  void restart_prompt() {
    phase_ = Phase::Prompt;
    fade_ = 0;
    to_menu_ = menu_in_ = to_prompt_ = false;
    choice_ = pending_ = TitleChoice::None;
    page_ = 0;
  }

 private:
  void play(int id) {
    if (sfx_) sfx_(id);
  }
  // The "press start" chime: global sound 62, the one the original plays.
  void play_chime() { play(62); }
  void start_song(int song);
  std::u32string label(int i) const;
  bool enabled(int i) const { return page_ == 1 || variant_ == 2 || i != 1 || has_saves_; }

  UiKit* kit_ = nullptr;
  audio::Mixer* mixer_ = nullptr;
  render::TexRef logo_ = {};
  render::TexRef bg_[2] = {{}, {}};
  // The option underline sprites: the blanked label atlases (map/etc/openfont.tim for variant 1, menu/title/cmptitle.tim for
  // variant 2; only the index-14 underline pixels are left), 256 x 325.
  render::TexRef bars_[2] = {{}, {}};
  float bars_h_[2] = {0, 0};
  import::Lang lang_ = import::Lang::En;
  ArtMode mode_ = ArtMode::Hd;
  Phase phase_ = Phase::FadeIn;
  TitleChoice choice_ = TitleChoice::None, pending_ = TitleChoice::None;
  bool has_saves_ = false;
  bool backdrop_ = false;
  // Transitions: to_menu_ = Prompt -> Menu, fading the title to black; menu_in_ = the menu still fading in from that black;
  // to_prompt_ = menu cancelled, fading to black before the prompt returns.
  bool to_menu_ = false;
  bool menu_in_ = false;
  bool to_prompt_ = false;
  int sel_ = 0;
  // variant_: 1 New Game / Continue (+ Settings / Quit), 2 New Game+ / Continue+. page_: 0 the option list, 1 the vibration page.
  int variant_ = 1;
  int page_ = 0;
  bool vibration_ = false;
  float fade_ = 1.0f;
  float time_ = 0;
  std::function<void(int)> sfx_;
};

}  // namespace oc::ui
