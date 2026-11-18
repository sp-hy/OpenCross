#include "ui/keymap_screen.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/log.h"
#include "platform/keymap.h"

namespace oc::ui {
namespace {

// Layout width of both pages.
constexpr float kImageW = 1620.0f;
constexpr float kScale = kCanvasW / kImageW;
constexpr float kStep = 52.0f, kHalfStep = 26.0f, kGlyph = 48.0f;

// Page 1 glyphs, RDE's fixed picture of its pad layout (cells of ui/gamepad_assign.png: A 8, B 9, Y 10, X 11, L stick 12, D-pad 13,
// START 14, BACK 15, L3 16, R3 17, LT 22, RT 23).
struct PadGlyph {
  u8 cell;
  float x, y;
};
constexpr PadGlyph kPadGlyphs[] = {
    // In the field: Talk/Confirm, Run/Cancel, Open Key Item Window, Menu.
    {8, 192.0f, 282.5f},
    {9, 192.0f, 376.5f},
    {11, 192.0f, 470.5f},
    {10, 192.0f, 564.5f},
    // In battle: Confirm, Cancel, Choose Character, Change View.
    {8, 192.0f, 700.5f},
    {9, 192.0f, 794.5f},
    {11, 192.0f, 888.5f},
    {10, 192.0f, 982.5f},
    // Move/Select (D-pad, left stick), Pause, Change Lead Character, Low-Speed Mode, High-Speed Mode, No Random Battles, Battle Boosts.
    {13, 916.0f, 230.5f},
    {12, 968.0f, 230.5f},
    {14, 942.0f, 324.5f},
    {15, 942.0f, 418.5f},
    {22, 942.0f, 512.5f},
    {23, 942.0f, 606.5f},
    {16, 942.0f, 700.5f},
    {17, 942.0f, 794.5f},
    // Auto-Battle (L3 + R3), ESC Menu (LT + L3).
    {16, 911.0f, 888.5f},
    {17, 973.0f, 888.5f},
    {22, 911.0f, 982.5f},
    {16, 973.0f, 982.5f},
};

// A stick pushed at least half way counts as a held button, as in RDE.
constexpr float kStickHeld = 0.5f;

u32 alpha_white(float a) { return render::pack_rgba(255, 255, 255, static_cast<u8>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f))); }

}  // namespace

bool KeyMapScreen::begin(UiKit& kit, import::Lang lang) {
  // Page image suffix and keyboard sheet suffix per language.
  static constexpr const char* kPage[6] = {"en", "fr", "it", "du", "es", "jp"};
  static constexpr const char* kKeys[6] = {"e", "f", "i", "g", "s", "j"};
  const int li = std::clamp(static_cast<int>(lang), 0, 5);
  counter_ = 0;
  acc_ = 0;
  next_ = false;
  pad_held_ = false;
  // The guide's pages are RDE's; played from the PS1 discs the screen is skipped.
  if (!kit.vfs().has_archive("hd")) {
    done_ = true;
    return false;
  }
  if (loaded_lang_ != li) {
    auto load = [&](const std::string& path) {
      auto raw = kit.vfs().read(path);
      if (!raw) {
        log::warn("keymap: {} missing", path);
        return render::TexRef{};
      }
      return kit.upload_png(*raw);
    };
    pad_page_ = load(std::string("hd/userguide/steampad_") + kPage[li] + ".png");
    key_page_ = load(std::string("hd/userguide/steamkey_") + kPage[li] + ".png");
    pad_glyphs_ = load("ui/gamepad_assign.png");
    key_glyphs_ = load(std::string("ui/keyboard_") + kKeys[li] + ".png");
    loaded_lang_ = li;
  }
  done_ = !pad_page_ || !key_page_;
  return !done_;
}

void KeyMapScreen::step() {
  if (counter_ == 0) {
    counter_ = 1;
  } else if (counter_ == 15 || counter_ == 30) {
    if (next_) {
      next_ = false;
      ++counter_;
    }
  } else if (counter_ >= 45) {
    counter_ = 0;
    done_ = true;
  } else {
    ++counter_;
  }
}

void KeyMapScreen::update(const platform::Input& in, float dt) {
  if (done_) return;
  // "Next" latch: a key press (anything but the F-key toggles), or the frame the pad goes from some button or stick held to none (the
  // stick clicks, the triggers and Start, the pause / settings button, do not count). A connected pad with nothing held never sets it.
  for (size_t i = 0; i < in.key_pressed.size(); ++i)
    if (in.key_pressed[i] && (i < SDL_SCANCODE_F1 || i > SDL_SCANCODE_F12)) next_ = true;
  bool pad_held = false;
  if (in.pad_connected) {
    for (size_t button = 0; button < in.pad_down.size(); ++button)
      if (in.pad_down[button] && button != SDL_GAMEPAD_BUTTON_LEFT_STICK && button != SDL_GAMEPAD_BUTTON_RIGHT_STICK && button != SDL_GAMEPAD_BUTTON_START)
        pad_held = true;
    for (int axis : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY})
      if (std::fabs(in.pad_axis[static_cast<size_t>(axis)]) >= kStickHeld) pad_held = true;
  }
  if (pad_held_ && !pad_held) next_ = true;
  pad_held_ = pad_held;
  acc_ += std::clamp(dt, 0.0f, 0.25f) * 60.0f;
  while (acc_ >= 1.0f && !done_) {
    acc_ -= 1.0f;
    step();
  }
}

void KeyMapScreen::draw(DrawList& dl, UiKit& kit) const {
  if (done_) return;
  const int tick = counter_;
  float a0 = 0.0f, a1 = 0.0f;
  if (tick < 15) {
    a0 = static_cast<float>(tick) / 15.0f;
  } else if (tick < 30) {
    a0 = 1.0f;
    a1 = static_cast<float>(tick - 15) / 15.0f;
  } else if (tick < 45) {
    a1 = static_cast<float>(45 - tick) / 15.0f;
  }
  dl.rect(kit.white(), 0, 0, kCanvasW, kCanvasHeight, render::pack_rgba(0, 0, 0, 255));
  auto quad = [&](render::TexRef set, float x, float y, float w, float h, float u0, float v0, float du, float dv, float a) {
    dl.image(set, x * kScale, y * kScale, w * kScale, h * kScale, alpha_white(a), u0, v0, u0 + du, v0 + dv);
  };
  auto key_glyph = [&](int vk, float x, float y, float a) {
    const int cell = platform::keyboard_cell(vk);
    if (cell <= 0 || !key_glyphs_) return;
    quad(key_glyphs_, x, y, kGlyph, kGlyph, static_cast<float>(cell & 15) / 16.0f, static_cast<float>(cell >> 4) / 7.0f, 1.0f / 16.0f, 1.0f / 7.0f, a);
  };
  if (a0 > 0.0f) {
    dl.image(pad_page_, 0, 0, kCanvasW, kCanvasHeight, alpha_white(a0));
    if (pad_glyphs_)
      for (const PadGlyph& glyph : kPadGlyphs)
        quad(pad_glyphs_, glyph.x - 4.0f, glyph.y - 4.0f, kGlyph, kGlyph, static_cast<float>(glyph.cell % 14) / 14.0f, static_cast<float>((glyph.cell / 14) * 2 + 1) * 0.25f,
             1.0f / 14.0f, 0.25f, a0);
  }
  if (a1 > 0.0f) {
    dl.image(key_page_, 0, 0, kCanvasW, kCanvasHeight, alpha_white(a1));
    using platform::Action;
    // Move/Select: set 0 = the arrows (y 247.5), set 1 = W S A D (y 301.5), four cells from x 110 (an unbound key moves the row half a step).
    int sets[2][4];
    const Action dirs[4] = {Action::Up, Action::Down, Action::Left, Action::Right};
    for (int set_index = 0; set_index < 2; ++set_index)
      for (int k = 0; k < 4; ++k) sets[set_index][k] = platform::action_vks(dirs[k])[static_cast<size_t>(set_index)];
    for (int set_index = 0; set_index < 2; ++set_index) {
      float x = 110.0f;
      for (int vk : sets[set_index]) x += vk == 0 ? kHalfStep : 0.0f;
      for (int vk : sets[set_index]) {
        if (vk == 0) continue;
        key_glyph(vk, x, set_index == 0 ? 247.5f : 301.5f, a1);
        x += kStep;
      }
    }
    // Two-key rows: both move half a step right when either key is unbound.
    auto pair = [&](Action action, float x, float y) {
      const std::array<int, 2> vks = platform::action_vks(action);
      if (vks[0] == 0 || vks[1] == 0) x += kHalfStep;
      key_glyph(vks[0], x, y, a1);
      key_glyph(vks[1], x + kStep, y, a1);
    };
    // Left column: Talk/Confirm, Run/Cancel, key item window, Menu (both RDE key sets each), then the fixed Start (Pause) and Select
    // (Change Lead Character) keys.
    pair(Action::Ok, 162.0f, 415.5f);
    pair(Action::Cancel, 162.0f, 525.5f);
    pair(Action::Item, 162.0f, 644.5f);
    pair(Action::Menu, 162.0f, 772.5f);
    key_glyph(platform::kStartVk, 188.0f, 891.5f, a1);
    key_glyph(platform::kSelectVk, 188.0f, 1001.5f, a1);
    // Right column: Low- / High-Speed Mode (both sets), No Random Battles F3, Battle Boosts F4, Auto-Battle F5, ESC Menu.
    pair(Action::SlowSpeed, 912.0f, 243.5f);
    pair(Action::FastSpeed, 912.0f, 353.5f);
    key_glyph(0x72, 938.0f, 463.5f, a1);
    key_glyph(0x73, 938.0f, 573.5f, a1);
    key_glyph(0x74, 938.0f, 683.5f, a1);
    key_glyph(0x1B, 938.0f, 793.5f, a1);
  }
}

}  // namespace oc::ui
