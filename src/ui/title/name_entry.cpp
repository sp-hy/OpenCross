#include "ui/title/name_entry.h"

#include "ui/menu/hand.h"
#include "ui/menu/ps1_art.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "import/tim.h"
#include "render/texture.h"

namespace oc::ui {

namespace {

constexpr int lang_index(import::Lang lang) { return static_cast<int>(lang); }

// Label tables, language order en fr it de es jp.
constexpr const char32_t* kTitle[6] = {U"Name", U"Nom", U"Nome", U"Name", U"Nombre", U"Name"};
constexpr const char32_t* kPrompt[6] = {U"Please enter a name...", U"Veuillez saisir un nom...", U"Inserisci un nome...",
                                        U"Bitte gib einen Namen ein...", U"Introduce un nombre...", U"名前を入力して下さい"};
constexpr const char32_t* kChange[6] = {U"Change Name", U"Changer le nom", U"Cambia nome", U"Namen ändern", U"Cambiar nombre", U"英数字"};
constexpr const char32_t* kConfirm[6] = {U"Confirm", U"Confirmer", U"Conferma", U"Bestätigen", U"Confirmar", U"決定"};
constexpr const char32_t* kDefault[6] = {U"Default", U"Par défaut", U"Predefinito", U"Zurücksetzen", U"Predeterminado", U"初期化"};

constexpr u32 kWhite = render::pack_rgba(255, 255, 255);

}  // namespace

void NameEntry::begin(const Setup& setup) {
  char_id_ = setup.char_id;
  lang_ = setup.lang;
  mode_ = setup.mode;
  // Default restores the name the screen opened with: the custom name when there is one, else the language's name.
  default_ = setup.initial.empty() ? setup.default_name : setup.initial;
  // 12 characters non-JP, JP 6 (Viper 4), in both art modes.
  max_ = max_name_length(char_id_, lang_, true);
  done_ = false;
  result_.clear();
  flash_ = 0;
  time_ = 0;
  hand_init_ = false;
  have_pages_ = false;
  if (ps1_pages() && setup.glyphs) {
    for (int i = 0; i < 3; ++i) pages_[static_cast<size_t>(i)] = ps1_page(static_cast<Ps1Page>(i), *setup.glyphs);
    have_pages_ = true;
  }
  buttons_.clear();
  if (ps1_pages()) buttons_ = {Button::Katakana, Button::Hiragana, Button::Alnum};
  // RDE keyboard: the third button is "Change Name" (focus index 2; confirm returns to the grid).
  else buttons_ = {Button::Alnum};
  buttons_.push_back(Button::Confirm);
  buttons_.push_back(Button::Default);
  page_ = lang_ == import::Lang::Jp ? Ps1Page::Katakana : Ps1Page::Alnum;
  row_ = col_ = 0;
  // The PS1 screen starts on the button column; the RDE keyboard starts in the grid.
  in_buttons_ = ps1_pages();
  button_ = in_buttons_ ? static_cast<int>(page_) : 0;
  set_name(default_);
}

void NameEntry::set_name(const std::u32string& name) {
  const char32_t blank = lang_ == import::Lang::Jp ? U'　' : U' ';
  buf_ = name.substr(0, static_cast<size_t>(max_));
  buf_.resize(static_cast<size_t>(max_), blank);
  pos_ = 0;
}

char32_t NameEntry::cell(int grid_row, int grid_col) const {
  if (grid_row < 0 || grid_col < 0 || grid_row >= rows() || grid_col >= cols()) return 0;
  if (!ps1_pages()) {
    const char32_t ch = latin_rows()[static_cast<size_t>(grid_row)][static_cast<size_t>(grid_col)];
    return ch == U' ' ? 0 : ch;
  }
  return have_pages_ ? pages_[static_cast<size_t>(page_)][static_cast<size_t>(grid_row)][static_cast<size_t>(grid_col)] : 0;
}

void NameEntry::insert(char32_t ch) {
  const char32_t blank = lang_ == import::Lang::Jp ? U'　' : U' ';
  buf_[static_cast<size_t>(pos_)] = ch ? ch : blank;
  if (pos_ < max_ - 1) ++pos_;
}

void NameEntry::press(NameKey key) {
  if (done_) return;
  const int nb = static_cast<int>(buttons_.size());
  auto focus_button = [&](int index) {
    button_ = (index + nb) % nb;
    const Button bt = buttons_[static_cast<size_t>(button_)];
    if (bt == Button::Katakana || bt == Button::Hiragana || bt == Button::Alnum) page_ = static_cast<Ps1Page>(static_cast<int>(bt));
    play(1);
  };
  switch (key) {
    case NameKey::Up:
      if (in_buttons_) focus_button(button_ - 1);
      else if (row_ > 0) --row_, play(1);
      else in_buttons_ = true, play(1);
      break;
    case NameKey::Down:
      if (in_buttons_) focus_button(button_ + 1);
      else if (row_ < rows() - 1) ++row_, play(1);
      else in_buttons_ = true, play(1);
      break;
    case NameKey::Left:
      if (in_buttons_) in_buttons_ = false, col_ = cols() - 1, play(1);
      else if (col_ > 0) --col_, play(1);
      else in_buttons_ = true, play(1);
      break;
    case NameKey::Right:
      if (in_buttons_) break;
      if (col_ < cols() - 1) ++col_, play(1);
      else in_buttons_ = true, play(1);
      break;
    case NameKey::Confirm:
      if (!in_buttons_) {
        insert(cell(row_, col_));
        play(1);
        break;
      }
      switch (buttons_[static_cast<size_t>(button_)]) {
        case Button::Confirm: {
          std::u32string validated = validate_name(buf_);
          if (validated.empty()) {
            flash_ = 0.45f;
            play(0x18);
          } else {
            result_ = std::move(validated);
            done_ = true;
            play(8);
          }
          break;
        }
        case Button::Default: set_name(default_); play(8); break;
        // A tab button: back into the grid.
        default: in_buttons_ = false; play(8); break;
      }
      break;
    case NameKey::Backspace:
      buf_[static_cast<size_t>(pos_)] = lang_ == import::Lang::Jp ? U'　' : U' ';
      if (pos_ > 0) --pos_;
      play(1);
      break;
    // Pad Start accepts the name from anywhere (same as the Confirm button).
    case NameKey::Start:
      in_buttons_ = true;
      for (size_t i = 0; i < buttons_.size(); ++i)
        if (buttons_[i] == Button::Confirm) button_ = static_cast<int>(i);
      press(NameKey::Confirm);
      break;
    case NameKey::PosLeft: pos_ = std::max(0, pos_ - 1); break;
    case NameKey::PosRight: pos_ = std::min(max_ - 1, pos_ + 1); break;
  }
}

void NameEntry::update(const Nav& nav, float dt) {
  time_ += dt;
  flash_ = std::max(0.0f, flash_ - dt);
  // Ease the needle toward the focused button's angle (11 frames), exact once settled.
  if (hand_init_) {
    if (button_ == hand_settled_) {
      hand_angle_ = hand_target_;
    } else {
      hand_angle_ += ((hand_target_ - hand_angle_) / 10) * hand_t_;
      if (hand_t_ < 10) ++hand_t_;
      else hand_t_ = 0, hand_settled_ = button_;
    }
  }
  if (nav.up) press(NameKey::Up);
  if (nav.down) press(NameKey::Down);
  if (nav.left) press(NameKey::Left);
  if (nav.right) press(NameKey::Right);
  if (nav.l1) press(NameKey::PosLeft);
  if (nav.r1) press(NameKey::PosRight);
  if (nav.cancel) press(NameKey::Backspace);
  if (nav.confirm) press(NameKey::Confirm);
  if (nav.start) press(NameKey::Start);
}

void NameEntry::draw(DrawList& dl, UiKit& kit) const {
  // Layout: the overlay uses a 512x432 list space, canvas X = 2.5 x, Y = 2.2222 y. Sizes below are canvas units.
  // Both art modes draw this screen; original art swaps the font files (small_font_*, kit.set_classic) and the playtim folder.
  const bool classic = mode_ == ArtMode::Original;
  kit.set_classic(classic);
  const int li = std::min(lang_index(lang_), 5);
  const bool jp = lang_ == import::Lang::Jp;
  const bool ps1 = ps1_pages();
  const render::FaceId gn_face = kit.face(mode_);
  // non-JP: bn_70 (slot 4, the default list font); JP: kt_70 (slot 1), else gn.
  const render::FaceId kt_face = kit.fonts().face(font::kKtName);
  const render::FaceId hd_face = jp ? (kt_face != render::kNoFace ? kt_face : gn_face) : kit.face_bn();
  const bool bn = !jp && hd_face != gn_face;
  // Text position: X = (x + dx) * sx + font.offX * scale, Y likewise (bn_70: offX -80/7, offY -120/7).
  auto offset_x = [&](float sc) { return bn ? -11.428572f * sc : 0.0f; };
  auto offset_y = [&](float sc) { return bn ? -17.142857f * sc : 0.0f; };
  const render::FaceId cell_face = hd_face;
  // Spec font scale -> gn / kt spritefont scale (0.35 <-> 0.42); bn_70 takes the spec scale as is.
  const float gsc = bn ? 1.0f : 1.2f;
  auto lab_scale = [&](float spec) { return spec * gsc; };
  const render::TexRef white_set = kit.white();
  const u32 normal = render::pack_rgba(165, 165, 165), orange = render::pack_rgba(192, 120, 0), title_c = render::pack_rgba(206, 197, 197);
  auto canvas_x = [](float x) { return 2.5f * x; };
  auto canvas_y = [](float y) { return 2.2222f * y; };

  // ---- backdrop, dimmed portrait strip, decorative swirls (menutim_02.png)
  // The screen tiles the brown parchment (map/etc/tile.tim): 8 x 7 sprites of 64 x 64 texels at u 0, v 128 of the 256 x 256 4bpp
  // image; one 512 x 448 screen. Same art in both modes.
  // hd/map/etc/tile.tim is the 4x PS1 tile; the WHOLE 256 x 256 image is the tile. It is used in both art modes; the disc tile of
  // cdrom/map/etc/systim.bin (64 x 64, nearest) only when the HD file is missing. Drawn 8 x 7 times, no alpha.
  auto ps1_pic = [&](const std::string& key, auto&& make) -> NameEntry::Pic {
    if (auto it = pics_.find(key); it != pics_.end()) return it->second;
    NameEntry::Pic loaded;
    const ps1::Rgba im = make();
    if (im) loaded.w = static_cast<float>(im.w), loaded.h = static_cast<float>(im.h), loaded.set = kit.upload_rgba(im.w, im.h, im.px.data(), false);
    pics_[key] = loaded;
    return loaded;
  };
  // Classic art + [Video] Ps1ResMenus: the screen at the PS1 display (512 x 448 buffer, the 432-row list space -> a 512 x 432
  // target) with the disc tile and the disc portrait strip.
  const bool ps1_art = classic && ps1_menus();
  kit.request_lowres(dl, kNameMenu);
  NameEntry::Pic tile = ps1_art ? NameEntry::Pic{} : pic(kit, "hd/map/etc/tile.tim");
  if (!tile.set) tile = ps1_pic("ps1:tile", [&] { return ps1::parchment(kit.vfs()); });
  if (tile.set) {
    const float tw = kCanvasW / 8.0f, th = kCanvasHeight / 7.0f;
    for (int ty = 0; ty < 7; ++ty)
      for (int tx = 0; tx < 8; ++tx) dl.image(tile.set, tw * static_cast<float>(tx), th * static_cast<float>(ty), tw + 1.0f, th + 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f, 1.0f, 1.0f);
  } else {
    dl.rect(white_set, 0, 0, kCanvasW, kCanvasHeight, render::pack_rgba(60, 38, 18));
  }
  const NameEntry::Pic dial = pic(kit, "hd/menu/menutim/menutim_02.png");
  if (dial.set) {
    const u32 tint = render::pack_rgba(160, 160, 176);
    // Two decorative swirls, deco A and deco B.
    dl.image(dial.set, -140, 537.8f, 640, 662, tint);
    dl.image(dial.set, 287, -300, 512, 529, tint);
  }
  {
    // Portrait strip: hd/menu/playtim/<2021 | 1999>/pNN.png by art mode; the disc strip cdrom/menu/playtim/pNN.tim
    // (8bpp 128 x 224: left half = top rows, right half = bottom) only when the PNG is missing.
    char path[64];
    const int id = std::clamp(char_id_, 0, 49);
    std::snprintf(path, sizeof path, "hd/menu/playtim/%s/p%02d.png", classic ? "1999" : "2021", id);
    NameEntry::Pic strip = ps1_art ? NameEntry::Pic{} : pic(kit, path);
    if (!strip.set) strip = ps1_pic("ps1:portrait" + std::to_string(id), [&] { return ps1::portrait(kit.vfs(), id); });
    // Drawn as two 64x224 halves at (16, 0) and (16, 224), colour 0x404040.
    if (strip.set) {
      const u32 tint = render::pack_rgba(128, 128, 128);
      dl.image(strip.set, 40, 0, 160, canvas_y(224), tint, 0, 0, 0.5f, 1);
      dl.image(strip.set, 40, canvas_y(224), 160, canvas_y(224), tint, 0.5f, 0, 1, 1);
    }
  }
  // ---- grid panel (menu-main-121, 362x256 at (130,32)), name bar (menu-main-118 at (120,304)), prompt bar (menu-main-119 at (56,360))
  auto panel = [&](int frame, float x, float y, float pw, float ph, float tab_x = 0, float tab_w = 0) {
    char path[64];
    std::snprintf(path, sizeof path, "hd/menu/shapes/menu-main-%03d.png", frame);
    const NameEntry::Pic shape = pic(kit, path);
    const float cx = canvas_x(x), cy = canvas_y(y), cw = canvas_x(pw), ch = canvas_y(ph);
    // The panel interior is transparent (the parchment shows through the frame sprites). HD uses the frame sprite; classic +
    // Ps1Res and a missing PNG use the disc's pieces.
    if (shape.set && !ps1_art) {
      dl.image(shape.set, cx, cy, cw, ch);
    } else {
      const NameEntry::Pic wd = ps1_pic("ps1:wind", [&] { return ps1::menutim(kit.vfs(), "wind"); });
      if (wd.set)
        ps1::frame_pieces(
            [&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) {
              dl.image(wd.set, dx, dy, dw, dh, 0xFFFFFFFFu, sx / wd.w, sy / wd.h, (sx + sw) / wd.w, (sy + sh) / wd.h);
            },
            cx, cy, cw, ch, 2.5f, 2.2222f, tab_x, tab_w);
    }
  };
  panel(121, 130, 32, 362, 256);
  // The name bar leaves a gap in the top edge for the "Name" tab (original art).
  panel(118, 120, 304, 128, 40, 14.0f, 76.0f);
  panel(119, 56, 360, 192, 40);
  {
    // Title "Name": the tab label of the name bar, in the notch at the top-left of the panel at (120, 304), left aligned.
    const std::u32string title = kTitle[li];
    const render::FaceId title_face = hd_face;
    const float sc = lab_scale(0.30f);
    dl.text(title_face, title, canvas_x(120) + 18.0f + offset_x(0.30f), canvas_y(304) + 7.0f - 0.5f * kit.text().line_height(title_face, sc), title_c,
            sc);
  }

  // ---- grid cells
  const int nr = rows(), nc = cols();
  for (int grid_row = 0; grid_row < nr; ++grid_row)
    for (int grid_col = 0; grid_col < nc; ++grid_col) {
      const char32_t ch = cell(grid_row, grid_col);
      const bool focus = !in_buttons_ && grid_row == row_ && grid_col == col_;
      if (!ps1) {
        // Cell glyphs are centred.
        const float cx = canvas_x(148.0f + 25.0f * static_cast<float>(grid_col)) + offset_x(0.35f),
                    cy = canvas_y(48.0f + 24.0f * static_cast<float>(grid_row)) + offset_y(0.35f);
        if (ch) dl.text(hd_face, std::u32string(1, ch), cx, cy, focus ? orange : normal, 0.35f * gsc, 1);
      } else {
        const float cx = (grid_col < 5 ? 375.0f + 80.0f * static_cast<float>(grid_col) : 815.0f + 80.0f * static_cast<float>(grid_col - 5)),
                    cy = 111.1f + 53.33f * static_cast<float>(grid_row);
        if (ch) dl.text(cell_face, std::u32string(1, ch), cx + 20.0f, cy, focus ? orange : normal, 0.5f, 1);
      }
    }
  // Cursor sprite (menu-main-120, 32x32) over the cell.
  if (!in_buttons_) {
    const NameEntry::Pic cur = pic(kit, "hd/menu/shapes/menu-main-120.png");
    float x, y;
    if (!ps1) {
      x = 347.5f + 62.5f * static_cast<float>(col_), y = 100.0f + 53.33f * static_cast<float>(row_);
    } else {
      x = 2.5f * (32.0f * static_cast<float>(col_) + 16.0f * static_cast<float>(col_ / 5) + 149.0f), y = 2.2222f * (24.0f * static_cast<float>(row_) + 49.0f);
    }
    if (cur.set) dl.image(cur.set, x, y, 80, 71.1f);
    else dl.rect(white_set, x, y, 80, 71.1f, render::pack_rgba(255, 224, 128, 90));
  }

  // ---- compass dial (canvas (955,651) 320x331) with the needle pointing at the focused label
  if (dial.set) dl.image(dial.set, 955, 651, 320, 331, render::pack_rgba(190, 190, 205));
  // ---- label leader lines (PS1 button strips): an underline below each label, a diagonal step and a run toward the dial
  if (!jp && !ps1) {
    const u32 lc = render::pack_rgba(205, 198, 190, 170);
    static const float kLead[3][6] = {{672, 792, 868, 901, 819, 928}, {653, 827, 835, 868, 856, 972}, {631, 863, 803, 835, 890, 988}};
    for (const auto& lead : kLead) {
      dl.rect(white_set, lead[0], lead[1], lead[2] - lead[0], 2.0f, lc);
      const float dx = lead[3] - lead[2], dy = lead[4] - lead[1];
      const int steps = 12;
      for (int k = 0; k < steps; ++k)
        dl.rect(white_set, lead[2] + dx * static_cast<float>(k) / steps, lead[1] + dy * static_cast<float>(k) / steps, dx / steps + 2.0f, 2.0f, lc);
      dl.rect(white_set, lead[3], lead[4], lead[5] - lead[3], 2.0f, lc);
    }
  }
  // ---- button labels
  struct Lab {
    float x, y;
  };
  // Label text and position per button; the index matches buttons_.
  std::vector<std::pair<std::u32string, Lab>> labs;
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const Button kind = buttons_[i];
    std::u32string label;
    if (kind == Button::Confirm) label = kConfirm[li];
    else if (kind == Button::Default) label = kDefault[li];
    else if (!ps1) label = kChange[li];
    else label = kind == Button::Katakana ? U"カタカナ" : kind == Button::Hiragana ? U"ひらがな" : U"英数字";
    // button_index is the PS1 button index 0..4 (Alnum / "Change Name" = 2, Confirm 3, Default 4).
    const int button_index = static_cast<int>(kind);
    Lab position;
    // The RDE keyboard labels sit 14 canvas px above the baseline.
    if (!jp && !ps1)
      position = {canvas_x(285.0f - 8.0f * static_cast<float>(button_index)) + offset_x(0.365f),
                  canvas_y(316.0f + 16.0f * static_cast<float>(button_index)) + offset_y(0.365f) - 14.0f};
    else position = {770.0f - 22.5f * static_cast<float>(button_index), 704.4f + 35.56f * static_cast<float>(button_index)};
    labs.push_back({std::move(label), position});
  }
  for (size_t i = 0; i < labs.size(); ++i) {
    // The focused button is highlighted whether or not the cursor is in the grid.
    const bool focus = static_cast<int>(i) == button_;
    const bool tab = buttons_[i] == Button::Katakana || buttons_[i] == Button::Hiragana || buttons_[i] == Button::Alnum;
    const bool active_tab = tab && static_cast<int>(buttons_[i]) == static_cast<int>(page_);
    dl.text(hd_face, labs[i].first, labs[i].second.x, labs[i].second.y, focus ? orange : (active_tab && ps1 ? orange : normal), lab_scale(0.365f));
  }
  {
    // The needle (handle.png: 98x14 PS1 px quad, tip at the left) turned about the dial centre (446, 367) so its tip points at the
    // end of the focused button's leader line (HD) / the right end of its label (original); resampled per angle (menu/hand.h).
    NameEntry::Pic hand;
    HandImage hi;
    const float px = 1115.0f, py = 816.0f;
    if (!labs.empty()) {
      const Lab focused_label = labs[static_cast<size_t>(button_)].second;
      const int button_index = static_cast<int>(buttons_[static_cast<size_t>(button_)]);
      float tx, ty;
      if (!jp && !ps1) {
        // End of leader line 0..2 (Change Name / Confirm / Default).
        static const float kEnd[3][2] = {{928, 819}, {972, 835}, {988, 851}};
        const int lead_index = std::clamp(button_index - 2, 0, 2);
        tx = kEnd[lead_index][0], ty = kEnd[lead_index][1];
      } else {
        tx = focused_label.x + 160.0f, ty = focused_label.y + 15.0f;
      }
      hand_target_ = hand_angle_toward(tx - px, ty - py, 2.5f, 2.2222f);
      // First frame: start on the focused button.
      if (!hand_init_) hand_init_ = true, hand_angle_ = hand_target_, hand_settled_ = button_, hand_t_ = 0;
      const int angle = hand_angle_;
      // One hand image, re-uploaded in place when the (eased) angle changes: a texture per angle would be wasteful.
      const std::string key = "hand";
      if (auto it = pics_.find(key); it != pics_.end() && (hand_pic_angle_ == angle || !it->second.set)) {
        hand = it->second;
      } else if (auto raw = kit.vfs().read("hd/menu/handle.png")) {
        render::DecodedImage src;
        if (render::decode_image(*raw, src)) {
          hi = render_hand(src.rgba.data(), src.width, src.height, angle, 2.5f, 2.2222f);
          if (auto jt = pics_.find(key); jt != pics_.end() && jt->second.set) {
            hand = jt->second;
            kit.update_rgba(hand.set, hi.rgba.data());
          } else {
            hand.set = kit.upload_rgba(hi.w, hi.h, hi.rgba.data(), true);
            // Pic.w / .h hold the half extents here.
            hand.w = hi.half_w, hand.h = hi.half_h;
          }
          hand_pic_angle_ = angle;
        }
        pics_[key] = hand;
      }
      if (hand.set) dl.image(hand.set, px - hand.w, py - hand.h, 2 * hand.w, 2 * hand.h, render::pack_rgba(210, 205, 205));
    }
  }

  // ---- name bar contents: glyphs, slot underscores, (JP) the title kanji for Viper
  for (int i = 0; i < max_; ++i) {
    const bool cur = i == pos_;
    const char32_t ch = buf_[static_cast<size_t>(i)];
    const float gx = jp ? 330.0f + 40.0f * static_cast<float>(i) : 322.5f + 25.0f * static_cast<float>(i) + (bn ? offset_x(0.25f) : 0.0f);
    const float dyn = bn ? offset_y(0.25f) : 0.0f;
    if (ch != U' ' && ch != U'　') {
      const float sc = jp ? 0.35f * gsc : 0.25f * gsc;
      dl.text(cell_face, std::u32string(1, ch), gx, (jp ? 708.9f : 711.1f) + dyn, cur ? orange : normal, sc, jp ? 0 : 1);
    }
    const bool blink = cur && std::fmod(time_, 0.9f) < 0.6f;
    if (jp) dl.rect(white_set, gx + 2.5f, 746.7f, 32.5f, 4.4f, cur ? (blink ? orange : render::pack_rgba(120, 75, 0)) : normal);
    else dl.text(hd_face, U"_", gx, 715.6f + dyn, cur ? orange : normal, 0.25f * gsc, 1);
  }
  if (char_id_ == 5 && jp) {
    dl.text(hd_face, U"大", 490, 708.9f, normal, 0.35f * gsc);
    dl.text(hd_face, U"佐", 530, 708.9f, normal, 0.35f * gsc);
  }
  if (flash_ > 0) dl.rect(white_set, 300, 676, 320, 89, render::pack_rgba(220, 40, 40, static_cast<u8>(std::min(1.0f, flash_ / 0.45f) * 150)));
  {
    // Prompt in the prompt bar at (200 (DE 175), 833.3).
    const std::u32string prompt = kPrompt[li];
    const render::FaceId prompt_face = hd_face;
    const float sc = lab_scale(0.35f);
    dl.text(prompt_face, prompt, canvas_x(lang_ == import::Lang::De ? 70.0f : 80.0f) + offset_x(0.35f), canvas_y(375.0f) + offset_y(0.35f), normal, sc);
  }
}

}  // namespace oc::ui

namespace oc::ui {

NameEntry::Pic NameEntry::pic(UiKit& kit, const std::string& path) const {
  if (auto it = pics_.find(path); it != pics_.end()) return it->second;
  Pic loaded;
  if (auto raw = kit.vfs().read(path)) {
    // A PS1 TIM is converted to RGBA (nearest); index 0 / 0x0000 stays transparent.
    if (path.ends_with(".tim")) {
      if (auto tim = import::parse_tim(*raw))
        if (auto img = import::decode_tim(*tim)) {
          const import::Image rgba = import::to_rgba(*img);
          loaded.w = static_cast<float>(rgba.width), loaded.h = static_cast<float>(rgba.height);
          loaded.set = kit.upload_rgba(rgba.width, rgba.height, rgba.pixels.data(), path.starts_with("hd/"));
        }
    } else {
      loaded.set = kit.upload_png(*raw, &loaded.w, &loaded.h);
    }
  }
  pics_[path] = loaded;
  return loaded;
}

}  // namespace oc::ui
