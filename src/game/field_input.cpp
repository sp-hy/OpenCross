// FieldInput: the per-frame sampling of keyboard and pad.
#include "game/field_input.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>

#include "platform/keymap.h"

namespace oc::game {

namespace {

// PS1-style key word (op 50 / 51 masks): bit 2 L1, 3 R1, 4 triangle, 5 circle (confirm), 6 cross, 7 square, 8 select, 11 start, 12 up,
// 13 right, 14 down, 15 left.
constexpr u16 kKeyL1 = 0x04, kKeyR1 = 0x08, kKeyTriangle = 0x10, kKeyCircle = 0x20, kKeyCross = 0x40, kKeySquare = 0x80, kKeySelect = 0x100,
              kKeyStart = 0x800, kKeyUp = 0x1000, kKeyRight = 0x2000, kKeyDown = 0x4000, kKeyLeft = 0x8000;
// A held direction repeats once it has been down for more than this many ticks, then every tick.
constexpr int kRepeatDelay = 21;
// After a room load the pad reads as released for 24 field frames (2 ticks each).
constexpr int kRoomLoadLock = 48;
using platform::kSelectKey;
using platform::kStartKey;

}  // namespace

void FieldInput::reset() {
  held_mask = 0;
  held_ticks = {};
  action_pending = false;
  input_mask = 0xFFFF;
  lock = kRoomLoadLock;
}

FieldInput::Edges FieldInput::sample(const platform::Input& in, int ticks) {
  using platform::Action;
  int mask = 0;
  if (platform::action_down(in, Action::Up)) mask |= 1;
  if (platform::action_down(in, Action::Right)) mask |= 2;
  if (platform::action_down(in, Action::Down)) mask |= 4;
  if (platform::action_down(in, Action::Left)) mask |= 8;
  // Cancel (Cross) held = run.
  run_held = platform::action_down(in, Action::Cancel);
  bool confirm = platform::action_pressed(in, Action::Ok), cancel = platform::action_pressed(in, Action::Cancel);
  bool up = platform::action_pressed(in, Action::Up), down = platform::action_pressed(in, Action::Down);
  bool left = platform::action_pressed(in, Action::Left), right = platform::action_pressed(in, Action::Right);
  pad_analog = false;
  pad_rstick = false;
  if (in.pad_connected) {
    if (in.pad_down[SDL_GAMEPAD_BUTTON_DPAD_UP]) mask |= 1;
    if (in.pad_down[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]) mask |= 2;
    if (in.pad_down[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) mask |= 4;
    if (in.pad_down[SDL_GAMEPAD_BUTTON_DPAD_LEFT]) mask |= 8;
    // Pad: B (PS1 cross) = dash, X = key items, Y = menu, Start = pause.
    run_held = run_held || in.pad_down[SDL_GAMEPAD_BUTTON_EAST];
    confirm = confirm || in.pad_pressed[SDL_GAMEPAD_BUTTON_SOUTH];
    cancel = cancel || in.pad_pressed[SDL_GAMEPAD_BUTTON_EAST] || in.pad_pressed[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
    // In the field the stick clicks act as the two functions: L3 confirms, R3 runs and cancels (after the Controller assignment).
    run_held = run_held || in.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
    confirm = confirm || in.pad_pressed[SDL_GAMEPAD_BUTTON_LEFT_STICK];
    up = up || in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_UP];
    down = down || in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN];
    left = left || in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_LEFT];
    right = right || in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_RIGHT];
    // ay: up positive. Outside the 0x4C..0xB4 dead zone the stick counts as analog.
    const float ax = in.pad_axis[SDL_GAMEPAD_AXIS_LEFTX], ay = -in.pad_axis[SDL_GAMEPAD_AXIS_LEFTY];
    if (std::fabs(ax) * 127.0f > 52.0f || std::fabs(ay) * 127.0f > 52.0f) {
      pad_analog = true;
      pad_x = ax;
      pad_y = ay;
    }
    // Analog mode: the right stick outside the same dead zone sets the leader's facing target (no camera yaw), turning him
    // on the spot; a walking tick overwrites it with the move heading.
    const float rx = in.pad_axis[SDL_GAMEPAD_AXIS_RIGHTX], ry = -in.pad_axis[SDL_GAMEPAD_AXIS_RIGHTY];
    if (std::fabs(rx) * 127.0f > 52.0f || std::fabs(ry) * 127.0f > 52.0f) {
      pad_rstick = true;
      pad_rx = rx;
      pad_ry = ry;
    }
  }
  held_mask = static_cast<u32>(mask);
  u32 repeat = 0;
  for (int dir = 0; dir < 4; ++dir) {
    int& held = held_ticks[static_cast<size_t>(dir)];
    held = (mask >> dir & 1) ? held + std::max(ticks, 1) : 0;
    if (held > kRepeatDelay) repeat |= 1u << dir;
  }
  pad_word = 0;
  if (mask & 1) pad_word |= kKeyUp;
  if (mask & 2) pad_word |= kKeyRight;
  if (mask & 4) pad_word |= kKeyDown;
  if (mask & 8) pad_word |= kKeyLeft;
  // The script-visible key word (ops 50 / 51: the world map's "enter the location" test reads the circle bit) takes the pad too.
  const bool pad_a = in.pad_connected && (in.pad_down[SDL_GAMEPAD_BUTTON_SOUTH] || in.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK]);
  const bool pad_b = in.pad_connected && (in.pad_down[SDL_GAMEPAD_BUTTON_EAST] || in.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_STICK]);
  if (platform::action_down(in, Action::Ok) || pad_a) pad_word |= kKeyCircle;
  if (platform::action_down(in, Action::Cancel) || pad_b) pad_word |= kKeyCross;
  if (in.key_down[kStartKey] || (in.pad_connected && in.pad_down[SDL_GAMEPAD_BUTTON_START])) pad_word |= kKeyStart;
  if (in.key_down[kSelectKey] || (in.pad_connected && in.pad_down[SDL_GAMEPAD_BUTTON_BACK])) pad_word |= kKeySelect;
  // The other face and shoulder buttons, as on the PS1 (the mod UI pick reads them; the menus keep their own actions).
  const auto pad = [&](SDL_GamepadButton button) { return in.pad_connected && in.pad_down[button]; };
  if (platform::action_down(in, Action::Menu) || pad(SDL_GAMEPAD_BUTTON_NORTH)) pad_word |= kKeyTriangle;
  if (platform::action_down(in, Action::Item) || pad(SDL_GAMEPAD_BUTTON_WEST)) pad_word |= kKeySquare;
  if (platform::action_down(in, Action::SelectLeft) || pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pad_word |= kKeyL1;
  if (platform::action_down(in, Action::SelectRight) || pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pad_word |= kKeyR1;
  key_latch |= static_cast<u16>(pad_word & ~0u);
  const bool select = (in.key_pressed[kSelectKey] || (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_BACK])) && (input_mask & kKeySelect);
  return {confirm, up, down, left, right, repeat, select, cancel};
}

void FieldInput::apply_lock(Edges& edges, int ticks) {
  menu_wait = std::max(0, menu_wait - ticks);
  if (lock <= 0) return;
  lock = std::max(0, lock - std::max(ticks, 1));
  edges = {};
  held_mask = 0;
  held_ticks = {};
  run_held = false;
  pad_word = 0;
  key_latch = 0;
  pad_analog = pad_rstick = false;
}

}  // namespace oc::game
