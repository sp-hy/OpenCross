#pragma once
// StickNav: the left stick as menu directions with d-pad press semantics - one edge each time it crosses the threshold.
#include <SDL3/SDL_gamepad.h>

#include "platform/window.h"

namespace oc::platform {

class StickNav {
 public:
  // Call once per frame, before any early return. Up / Down / Left / Right are true only on the frame the stick enters that direction.
  void poll(const Input& in) {
    const float x = in.pad_axis[SDL_GAMEPAD_AXIS_LEFTX], y = in.pad_axis[SDL_GAMEPAD_AXIS_LEFTY];
    const bool now[4] = {y < -kThreshold, y > kThreshold, x < -kThreshold, x > kThreshold};
    bool* edge[4] = {&up, &down, &left, &right};
    for (int i = 0; i < 4; ++i) {
      *edge[i] = now[i] && !held_[i];
      held_[i] = now[i];
    }
  }
  bool up = false, down = false, left = false, right = false;

 private:
  static constexpr float kThreshold = 0.6f;
  bool held_[4]{};
};

}  // namespace oc::platform
