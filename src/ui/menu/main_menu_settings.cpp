#include "ui/menu/main_menu_impl.h"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cstdio>

#include "platform/pad_remap.h"
#include "ui/menu/menu_common.h"
#include "ui/ps1_window.h"
#include "ui/window_frame.h"

namespace oc::ui {

namespace {

// Customize rows in the overlay's order: the option bit of the options word and the menutest.txt rows of the label and its two choices.
// In order: Sound (Stereo / Monaural), Window Frame (Default / Custom), Cursor Position (Default / Memorized), Accuracy Selection
// (Manual / Auto), Controller (Default / Custom), [button] Pressed (Run / Walk), Vibration Function (Off / On), Left Stick Sensitivity
// (Digital / Analog).
struct SettingRow {
  u16 mask;
  u32 label, left, right;
};
const SettingRow kSettings[8] = {
    {0x04, 0x801F4A7C, 0x801F4A8E, 0x801F4AA0},
    {0x08, 0x801F4AB2, 0x801F4AC4, 0x801F4AD6},
    {0x01, 0x801F4AE8, 0x801F4AFA, 0x801F4B0C},
    {0x02, 0x801F4B1E, 0x801F4B30, 0x801F4B42},
    {0x100, 0x801F4B54, 0x801F4B66, 0x801F4B78},
    {0x200, 0x801F4BD2, 0x801F4BE4, 0x801F4BF6},
    {0x400, 0x801F4B8A, 0x801F4BAE, 0x801F4BC0},
    {0x800, 0x801F4C08, 0x801F4C2C, 0x801F4C3E}};

// The assignment page's texts are the menutest.txt rows from this address on, 0x12 bytes apart; a text id is its row + 5.
constexpr u32 kPadTextBase = 0x801F4C98, kPadTextStep = 0x12;
constexpr int kPadFirstId = 5;
constexpr u32 kPadCaption = 0x801F4B54;
constexpr float kPadCaptionWidth = 56.0f, kPadTailGap = 5.0f, kPadRowTextPs1 = 0x54, kPadRowTextHd = 0x2D;
constexpr int kPadIdTitleTail = 0x1E, kPadIdQuit = 6, kPadIdDefault = 7, kPadIdStick = 8;
constexpr int kPadIdRun = 0xF, kPadIdWalk = 0x10, kPadIdWalkSelect = 0x11, kPadIdRunSelect = 0x12;
// RDE's rows hold at most 10 cursor positions (8 buttons and the two sticks); the original's analog pad had 12.
constexpr int kPadLastRowAnalog = 9, kPadLastRowDigital = 7;

struct PadRow {
  int id, tail;
};
const PadRow kFieldRows[8] = {{0xA, 0}, {kPadIdRun, 0}, {0xB, 0}, {0xC, 0}, {0x1F, 0x21}, {0x20, 0x21}, {0x1C, 0}, {0x1D, 0}};
const PadRow kBattleRows[8] = {{0x14, 0}, {0x15, 0}, {0x16, 0x17}, {0x19, 0x1A}, {0x16, 0x18}, {0x16, 0x17}, {0x1C, 0}, {0x1D, 0}};
const int kFieldSticks[2] = {kPadIdWalkSelect, 0xE};
const int kBattleSticks[2] = {0x1B, 0xD};

}  // namespace

// Customize: Left or Right flips the row's option bit on every row, pad or not. OK on "Window Frame: Custom" opens the skin picker,
// which steps the skin byte 0..14 (wrapping) to the next skin whose bit in the owned-frames mask is set; only OK closes it.
void MainMenu::Impl::update_settings(const Nav& nav, const platform::Input& in) {
  if (pad_state != 0) {
    update_pad_page(nav, in);
    return;
  }
  if (skin_pick) {
    auto step_skin = [&](int direction) {
      // An empty mask would loop forever below.
      if ((gs->slot.frames & 0x7FFF) == 0) return;
      int skin = gs->skin;
      do skin = wrap(skin + direction, 15);
      while (((gs->slot.frames >> skin) & 1) == 0);
      gs->skin = static_cast<u8>(skin);
      sfx(1);
    };
    if (nav.left) step_skin(-1);
    if (nav.right) step_skin(1);
    if (nav.ok) skin_pick = false, sfx(8);
    return;
  }
  if (nav.up) set_cur = wrap(set_cur - 1, 8), sfx(1);
  if (nav.down) set_cur = wrap(set_cur + 1, 8), sfx(1);
  const SettingRow& row = kSettings[set_cur];
  if (nav.cancel) {
    gs->store_window_frame();
    back_to_main(4);
    sfx(6);
    return;
  }
  if (nav.left || nav.right) gs->set_option(row.mask, !gs->option(row.mask)), sfx(1);
  if (nav.ok) {
    if (set_cur == 1 && gs->option(opt::kCustomWindow)) skin_pick = true, sfx(8);
    else if (set_cur == 4 && gs->option(opt::kControllerEdit)) pad_state = 2, sfx(8);
  }
}

// ---- settings (state 0): option panel frame 0x40 at (48,44); rows y = 60 + 24 i; label x 72; choices x 278 / 376
// with the 82x20 highlight quad (0xA0,0x60,0) behind the chosen one; hand cursor at (40, 60 + 24 row). The rows follow the
// overlay's mask table {4, 8, 1, 2, 0x100, 0x200, 0x400, 0x800}.
void MainMenu::Impl::build_settings() {
  if (pad_state != 0) {
    build_pad_page();
    return;
  }
  Frame(64, 48, 44);
  // "Customize" in the frame's top notch.
  notch_caption(48, 44, 5);
  for (int i = 0; i < 8; ++i) {
    const SettingRow& setting = kSettings[i];
    const float y = 60.0f + 24.0f * static_cast<float>(i);
    const bool right = gs->option(setting.mask);
    const u32 chosen = ps1_colour(0x004048);
    const std::u32string label = utf32(menu_str(setting.label, "?"));
    TP(i == 5 ? 100.0f : 72.0f, y, label, kText, 0.9f);
    // Button glyph of function 1 at (0x48, 0xB0), 24 x 24.
    if (i == 5) btn(72, 176, 1);
    // Chosen-choice highlight, drawn before the choices.
    rect(PX(right ? 375.0f : 277.0f), PY(y - 2), PX(82), PY(20), ps1_colour(0x0060A0));
    TP(278, y, utf32(menu_str(setting.left, "-")), !right ? chosen : kText, 0.9f);
    TP(376, y, utf32(menu_str(setting.right, "-")), right ? chosen : kText, 0.9f);
  }
  if (skin_pick) {
    skin_preview();
  } else {
    tri(40, 60.0f + 24.0f * static_cast<float>(set_cur), true);
  }
}

// State 1 (ui/window_frame.h): the skin of the chosen frame as a real
// window (7 x 4 opaque fill grid, corner / edge pieces, page leaf bottom-right), "Custom Window Frame" on one line near the top (y 0x132) and the skin
// name (y 0x162, one menutest.txt row per skin byte) centred below it, both white (0x808080) with the menu font's shadow; the side pointers
// (frames 3 / 1) at (0x5E - a, 0x152) and (a + 0x1AE, 0x152).
void MainMenu::Impl::skin_preview() {
  const int skin_byte = gs->skin;
  // Config byte -> texture index.
  const int idx = skin_byte == 0 ? 3 : skin_byte <= 3 ? skin_byte - 1 : skin_byte <= 14 ? skin_byte : 15;
  static constexpr int kNn[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};
  const int nn = kNn[idx];
  char fn[48];
  std::snprintf(fn, sizeof fn, "hd/window/w%02d.png", nn);
  // Classic art + Ps1ResMenus: the PS1 sheet cdrom/map/window/wN.tim (128 x 96; wNN.png is its 4x redraw with the same layout).
  Tex* p1 = ps1_screen() ? texs.ps1("ps1:w" + std::to_string(nn), [&] { return ps1win::skin(*vfs, nn); }) : nullptr;
  const SkinWindow window = build_skin_window(idx, (frames / 6) % 5);
  float x0 = 1e9f, x1 = -1e9f;
  // The wNN.png skin is drawn in both art modes.
  if (Tex* tex = p1 ? p1 : texs.load(fn)) {
    for (const WinQuad& quad : window.quads) {
      Img(tex, quad.x, quad.y, quad.w, quad.h, quad.u0, quad.v0, quad.u0 + quad.du, quad.v0 + quad.dv);
      if (quad.w < 100) x0 = std::min(x0, quad.x), x1 = std::max(x1, quad.x + quad.w);
    }
    Img(tex, window.leaf.x, window.leaf.y, window.leaf.w, window.leaf.h, window.leaf.u0, window.leaf.v0, window.leaf.u0 + window.leaf.du,
        window.leaf.v0 + window.leaf.dv);
  }
  if (x0 > x1) x0 = window.left, x1 = window.right;
  // Window centre in PS1 px.
  const float cx = (x0 + x1) * 0.5f / xscale;
  std::u32string title = utf32(menu_str(0x801F4AD6, "Custom"));
  if (!jp()) title += U" ";
  title += utf32(menu_str(0x801F4AB2, "Window Frame"));
  const std::u32string nm = utf32(menu_str(0x801F4EA4 + 0x14u * static_cast<u32>(gs->skin), "?"));
  const float rel = fit_rel(title, 170.0f, 0.85f);
  TP(cx - twp(title, rel) * 0.5f, 306, title, kWhite, rel);
  TP(cx - twp(nm, 0.85f) * 0.5f, 354, nm, kWhite, 0.85f);
  const float drift = static_cast<float>(((frames % 23) * 0x3000) >> 16);
  pointer(94.0f - drift, 338, true);
  pointer(430.0f + drift, 338, false);
}

// The assignment page (the original's states 2 and 3). Left / Right switch between the field page and the battle page. A button
// pressed on a row hands that button's function to the row: the two buttons swap functions. On the stick rows Circle swaps the sticks.
// Select puts the page's defaults back, Start alone leaves. The page reads the pad as it is, without its own assignment.
void MainMenu::Impl::update_pad_page(const Nav& nav, const platform::Input& in) {
  const bool l2_edge = nav.l2 && !pad_l2, r2_edge = nav.r2 && !pad_r2;
  pad_l2 = nav.l2, pad_r2 = nav.r2;
  const bool start = in.key_pressed[platform::kStartKey] || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_START]);
  const bool select = in.key_pressed[platform::kSelectKey] || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_BACK]);
  const bool pressed[platform::kMappedButtons] = {nav.ok, nav.cancel, nav.sq, nav.tri, nav.l1, nav.r1, l2_edge, r2_edge};
  bool other = nav.up || nav.down || nav.left || nav.right;
  for (bool button : pressed) other = other || button;
  if (nav.left || nav.right) pad_state = pad_state == 2 ? 3 : 2;
  if (start && !other) {
    pad_state = 0;
    sfx(8);
    return;
  }
  const size_t pair = static_cast<size_t>(pad_state - 2);
  platform::ButtonMap map{gs->slot.button_cfg[pair], gs->slot.button_cfg[pair + 2], true};
  if (select) {
    platform::map_reset(map);
    sfx(1);
  }
  const int last_row = pad_present ? kPadLastRowAnalog : kPadLastRowDigital;
  pad_cur = std::min(pad_cur, last_row);
  if (nav.up) pad_cur = pad_cur == 0 ? last_row : pad_cur - 1, sfx(1);
  if (nav.down) pad_cur = pad_cur == last_row ? 0 : pad_cur + 1, sfx(1);
  if (pad_cur < platform::kMappedButtons) {
    int button = -1;
    for (int i = 0; i < platform::kMappedButtons; ++i)
      if (pressed[i]) button = i;
    if (button >= 0) {
      sfx(1);
      for (int function = 0; function < platform::kMappedButtons; ++function)
        if (platform::map_source(map, function) == button) {
          platform::map_swap(map, function, pad_cur, false);
          break;
        }
    }
  } else if (nav.ok) {
    sfx(1);
    if (pad_cur < 10) platform::map_swap(map, 0, 1, true);
    else platform::map_swap(map, 2, 3, true);
  }
  gs->slot.button_cfg[pair] = map.face;
  gs->slot.button_cfg[pair + 2] = map.aux;
}

// A page text at PS1 (x, y) with its {ButtonN} / {ButtonLeft} / {ButtonRight} glyphs drawn inline.
void MainMenu::Impl::pad_page_text(float x, float y, const std::u32string& str, u32 colour) {
  constexpr float kRel = 1.0f, kGlyph = 22.0f;
  float pen = x;
  size_t from = 0;
  while (from < str.size()) {
    const size_t open = str.find(U'{', from), close = open == std::u32string::npos ? open : str.find(U'}', open);
    const std::u32string chunk = str.substr(from, close == std::u32string::npos ? std::u32string::npos : open - from);
    if (!chunk.empty()) {
      TP(pen, y, chunk, colour, kRel);
      pen += twp(chunk, kRel);
    }
    if (close == std::u32string::npos) break;
    const std::u32string token = str.substr(open + 1, close - open - 1);
    const int cell = token == U"Button1" ? 8 : token == U"Button2" ? 9 : token == U"ButtonLeft" ? 1 : token == U"ButtonRight" ? 2 : -1;
    if (cell >= 0) {
      if (Tex* tex = texs.load("ui/gamepad.png")) {
        const int col = cell % 13, row = (cell / 13) * 2 + 1;
        Img(tex, PX(pen), PY(y - 5.0f), PX(kGlyph), PY(kGlyph), static_cast<float>(col) / 13.0f, static_cast<float>(row) / 4.0f,
            static_cast<float>(col + 1) / 13.0f, static_cast<float>(row + 1) / 4.0f);
      }
      pen += kGlyph;
    }
    from = close + 1;
  }
}

// Page layout in PS1 px: header frame 0x43 at (0x20, 0x18), the eight rows in frame 0x41 at (0x20, 0x50) (glyph of the button now on
// the function at x + 0x10, text at x + 0x54, 24 px apart), the stick rows in frame 0x42 at (0x20, 0x132), Quit / Default in frame 0x50
// at (0x178, 0x132). The cursor sits at x 0x10 on the text line of its row.
void MainMenu::Impl::build_pad_page() {
  constexpr float kHeaderX = 0x20, kHeaderY = 0x18, kRowsX = 0x20, kRowsY = 0x50, kSticksY = 0x132, kFootX = 0x178, kLine = 0x18;
  const int which = pad_state - 2;
  const platform::ButtonMap map{gs->slot.button_cfg[static_cast<size_t>(which)], gs->slot.button_cfg[static_cast<size_t>(which) + 2], true};
  const bool walks = gs->option(opt::kButtonWalks);
  auto text_of = [&](int id) { return utf32(menu_str(kPadTextBase + kPadTextStep * static_cast<u32>(id - kPadFirstId), "")); };
  Frame(0x43, kHeaderX, kHeaderY);
  const std::u32string title = text_of(which == 0 ? kPadFirstId : 0x13);
  TP(kHeaderX + 0x20, kHeaderY + 0x12, title, kText, 1.0f);
  if (const std::u32string tail = text_of(kPadIdTitleTail); !tail.empty()) TP(kHeaderX + 0x20 + twp(title, 1.0f), kHeaderY + 0x12, tail, kText, 1.0f);
  const float drift = static_cast<float>(((frames % 23) * 0x3000) >> 16);
  pointer(kHeaderX + 0x10 - drift, kHeaderY + 0x16, true);
  pointer(kHeaderX + drift + 0x130, kHeaderY + 0x16, false);
  Frame(0x41, kRowsX, kRowsY);
  {
    const std::u32string caption = utf32(menu_str(kPadCaption, "Controller"));
    float scale = 0.31f;
    text->prepare(face_gn, caption);
    const float width = text->measure(face_gn, caption, scale) / xscale;
    if (width > kPadCaptionWidth) scale *= kPadCaptionWidth / width;
    HT(face_gn, kRowsX + 9.7f, kRowsY + 0.5f, caption, kText, scale);
  }
  const float row_text_x = ps1_screen() ? kPadRowTextPs1 : kPadRowTextHd;
  const PadRow* rows = which == 0 ? kFieldRows : kBattleRows;
  for (int i = 0; i < platform::kMappedButtons; ++i) {
    PadRow row = rows[i];
    if (which == 0 && i == 1 && walks) row.id = kPadIdWalk;
    const float line = kRowsY + 0x14 + kLine * static_cast<float>(i);
    glyph_button(kRowsX + 0x10, kRowsY + 0x10 + kLine * static_cast<float>(i), platform::map_source(map, i) & 7, kWhite, true);
    const std::u32string label = text_of(row.id);
    pad_page_text(kRowsX + row_text_x, line, label, kText);
    if (row.tail == 0) continue;
    const std::u32string tail = text_of(row.tail);
    if (which == 0) pad_page_text(kRowsX + row_text_x + 0x60, line, tail, kText);
    else pad_page_text(kRowsX + row_text_x + twp(label, 1.0f) + kPadTailGap, line, tail, kText);
  }
  const u32 sticks_colour = pad_present ? kWhite : ps1_colour(0x404040);
  const u32 text_colour = pad_present ? kText : ps1_colour(0x404040);
  Frame(0x42, kRowsX, kSticksY, sticks_colour);
  for (int i = 0; i < 2; ++i) {
    const float line = kSticksY + 0x14 + kLine * static_cast<float>(i);
    const int stick = static_cast<int>((map.aux >> (4 * i)) & 0xF);
    int function = (which == 0 ? kFieldSticks : kBattleSticks)[i];
    if (which == 0 && i == 0 && walks) function = kPadIdRunSelect;
    TP(kRowsX + 0x10, line, text_of(kPadIdStick + std::min(stick, 1)), text_colour, 1.0f);
    TP(kRowsX + 0x84, line, text_of(function), text_colour, 1.0f);
  }
  Frame(0x50, kFootX, kSticksY);
  glyph_button(kFootX + 0xC, kSticksY + 8, 8, kWhite, true);
  TP(kFootX + 0x18, kSticksY + 0x28, text_of(kPadIdQuit), kText, 1.0f);
  glyph_button(kFootX + 0xC, kSticksY + 0x38, 9, kWhite, true);
  TP(kFootX + 0x18, kSticksY + 0x58, text_of(kPadIdDefault), kText, 1.0f);
  const float cursor_y = pad_cur < platform::kMappedButtons ? kRowsY + kLine * static_cast<float>(pad_cur) + 0x14
                                                           : kSticksY + kLine * static_cast<float>(pad_cur - platform::kMappedButtons) + 0x14;
  tri(0x10, cursor_y, true);
}

}  // namespace oc::ui
