#pragma once
// Menu navigation from the platform input snapshot: edges plus key repeat for held directions.
#include <SDL3/SDL.h>

#include "platform/keymap.h"
#include "platform/window.h"

namespace oc::ui {

struct Nav {
  bool up = false, down = false, left = false, right = false;
  bool confirm = false, cancel = false, l1 = false, r1 = false, any = false;
  // start: Space (RDE's keyboard Start) / pad Start.
  bool start = false;
};

class NavPoller {
 public:
  // dt in seconds. Directions repeat after 0.4 s at 0.08 s intervals.
  Nav poll(const platform::Input& in, float dt) {
    Nav nav;
    using platform::Action;
    auto edge = [&](Action action) { return platform::action_pressed(in, action); };
    auto held = [&](Action action) { return platform::action_down(in, action); };
    auto pad_edge = [&](int button) { return in.pad_connected && in.pad_pressed[static_cast<size_t>(button)]; };
    auto pad_held = [&](int button) { return in.pad_connected && in.pad_down[static_cast<size_t>(button)]; };
    // The left stick counts as the d-pad once pushed past 0.6, as in the in-game menus.
    const float axis_x = in.pad_connected ? in.pad_axis[SDL_GAMEPAD_AXIS_LEFTX] : 0.0f, axis_y = in.pad_connected ? in.pad_axis[SDL_GAMEPAD_AXIS_LEFTY] : 0.0f;
    const bool up = held(Action::Up) || pad_held(SDL_GAMEPAD_BUTTON_DPAD_UP) || axis_y < -0.6f;
    const bool down = held(Action::Down) || pad_held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis_y > 0.6f;
    const bool left = held(Action::Left) || pad_held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis_x < -0.6f;
    const bool right = held(Action::Right) || pad_held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis_x > 0.6f;
    nav.up = step(0, up, dt);
    nav.down = step(1, down, dt);
    nav.left = step(2, left, dt);
    nav.right = step(3, right, dt);
    nav.confirm = edge(Action::Ok) || pad_edge(SDL_GAMEPAD_BUTTON_SOUTH);
    nav.cancel = edge(Action::Cancel) || pad_edge(SDL_GAMEPAD_BUTTON_EAST);
    nav.l1 = edge(Action::SelectLeft) || pad_edge(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    nav.r1 = edge(Action::SelectRight) || pad_edge(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    nav.start = in.key_pressed[platform::kStartKey] || pad_edge(SDL_GAMEPAD_BUTTON_START);
    nav.any = nav.confirm || nav.cancel || nav.l1 || nav.r1;
    for (int i = 0; i < platform::Input::kKeys && !nav.any; ++i)
      if (in.key_pressed[static_cast<size_t>(i)] && i != SDL_SCANCODE_ESCAPE && (i < SDL_SCANCODE_F1 || i > SDL_SCANCODE_F12))
        nav.any = true;
    // The stick clicks are RDE's toggles.
    for (int i = 0; i < platform::Input::kButtons && !nav.any; ++i)
      if (in.pad_pressed[static_cast<size_t>(i)] && i != SDL_GAMEPAD_BUTTON_LEFT_STICK && i != SDL_GAMEPAD_BUTTON_RIGHT_STICK) nav.any = true;
    return nav;
  }

 private:
  // One direction's edge-or-repeat: true on the press and then once per 0.08 s step after 0.4 s held.
  bool step(int direction, bool down, float dt) {
    if (!down) {
      held_[direction] = 0;
      return false;
    }
    if (held_[direction] == 0) {
      held_[direction] = 1e-6f;
      return true;
    }
    const float before = held_[direction];
    held_[direction] += dt;
    if (held_[direction] < 0.4f) return false;
    const float repeat_now = (held_[direction] - 0.4f) / 0.08f, repeat_before = before < 0.4f ? -1.0f : (before - 0.4f) / 0.08f;
    return static_cast<int>(repeat_now) != static_cast<int>(repeat_before);
  }
  float held_[4] = {0, 0, 0, 0};
};

}  // namespace oc::ui
