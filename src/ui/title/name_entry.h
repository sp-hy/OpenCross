#pragma once
// Name entry screen.
//   Both art modes: RDE keyboard, 10 x 14 Latin grid, up to 12
//   characters; JP: the kana pages (katakana / hiragana / alphanumeric), 10 x 10 grid, 6 characters (Viper 4). Original art swaps
//   only the font files (small_font_*) and the portrait strip folder (hd/menu/playtim/1999).
//   Played from the PS1 discs: the disc's own screen at the PS1 points; the USA build has one 10 x 10 page (letters and digits) and
//   6 characters, its text from the disc (import::load_name_menu); grid, name and the JP prompt in the console font's 16-pixel cells.
// The logic (cursor, buffer, validation) is plain data driven by press(); update()/draw() add input mapping and rendering.
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "import/name_menu.h"
#include "import/ps1_font.h"
#include "ui/title/canvas.h"
#include "ui/title/name_codec.h"
#include "ui/title/nav.h"

namespace oc::ui {

enum class NameKey { Up, Down, Left, Right, Confirm, Backspace, PosLeft, PosRight };

class NameEntry {
 public:
  struct Setup {
    // char_id: the character being named (0 = Serge). default_name: the language's name. initial: the custom name (empty = none).
    // The screen opens with, and "Default" restores, initial when set, else default_name. glyphs: needed for the PS1 pages.
    int char_id = 0;
    import::Lang lang = import::Lang::En;
    ArtMode mode = ArtMode::Hd;
    std::u32string default_name;
    std::u32string initial;
    const import::JpGlyphs* glyphs = nullptr;
    // Played from the PS1 discs: the disc's name-entry text; null on RDE.
    std::shared_ptr<const import::NameMenuText> disc;
  };
  enum class Button { Katakana, Hiragana, Alnum, Confirm, Default };

  void begin(const Setup& setup);
  // Sound ids: 1 cursor, 8 confirm, 0x18 denied.
  void set_sfx(std::function<void(int)> fn) { sfx_ = std::move(fn); }

  // ---- logic
  void press(NameKey key);
  bool done() const { return done_; }
  // result(): valid once done(): trimmed, never empty / reserved. buffer(): always max_len() characters, blank padded.
  const std::u32string& result() const { return result_; }
  const std::u32string& buffer() const { return buf_; }
  int cursor_pos() const { return pos_; }
  int max_len() const { return max_; }
  bool in_buttons() const { return in_buttons_; }
  int row() const { return row_; }
  int col() const { return col_; }
  int rows() const { return ps1_pages() || usa_grid() ? kPs1Rows : kLatinRows; }
  int cols() const { return ps1_pages() || usa_grid() ? kPs1Cols : kLatinCols; }
  // The character at a grid cell; 0 = blank cell.
  char32_t cell(int grid_row, int grid_col) const;
  Button button() const { return buttons_[static_cast<size_t>(button_)]; }
  const std::vector<Button>& buttons() const { return buttons_; }
  int page() const { return static_cast<int>(page_); }
  bool ps1_pages() const { return lang_ == import::Lang::Jp; }
  // The USA disc's single page.
  bool usa_grid() const { return disc_ && !disc_->grid.empty(); }
  float reject_flash() const { return flash_; }

  // ---- io
  void update(const Nav& nav, float dt);
  // `reach`: canvas units the parchment continues beyond each side of the 4:3 canvas (Canvas::reach; widescreen). The layout stays on
  // the 4:3 canvas.
  void draw(DrawList& dl, UiKit& kit, float reach = 0.0f) const;

 private:
  struct Pic {
    render::TexRef set = {};
    float w = 0, h = 0;
  };
  // A cached hd.dat PNG (freed with the UiKit).
  Pic pic(UiKit& kit, const std::string& path) const;
  mutable std::map<std::string, Pic> pics_;
  // Played from the discs: the grid cells, the name with its slot lines and the prompt, at the PS1 points (canvas = 2.5 x, 2.2222 y).
  void draw_disc_text(DrawList& dl, UiKit& kit) const;
  // A character in a 16 x 16 cell of the console font, the stand-in the disc's own 1-bit font in its greys; cached.
  Pic cell_pic(UiKit& kit, char32_t ch) const;
  mutable std::shared_ptr<const import::Ps1Font> cell_font_, menu_font_;
  void insert(char32_t c);
  void set_name(const std::u32string& s);
  void play(int id) {
    if (sfx_) sfx_(id);
  }

  int char_id_ = 0;
  import::Lang lang_ = import::Lang::En;
  ArtMode mode_ = ArtMode::Hd;
  std::shared_ptr<const import::NameMenuText> disc_;
  std::u32string default_;
  std::u32string buf_, result_;
  int max_ = 12, pos_ = 0;
  bool done_ = false;
  bool in_buttons_ = false;
  int row_ = 0, col_ = 0, button_ = 0;
  Ps1Page page_ = Ps1Page::Alnum;
  std::vector<Button> buttons_;
  std::array<Ps1Grid, 3> pages_{};
  bool have_pages_ = false;
  float flash_ = 0;
  float time_ = 0;
  // Dial needle easing (the same rule as the menu lists: angle += ((target - angle) / 10) * t, t = 0..10 per
  // frame, then the focused button settles). draw() computes the target for the focused button; update() steps toward it.
  mutable int hand_target_ = 0, hand_angle_ = 0, hand_settled_ = 0, hand_t_ = 0, hand_pic_angle_ = 0;
  mutable bool hand_init_ = false;
  std::function<void(int)> sfx_;
};

}  // namespace oc::ui
