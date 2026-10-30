// The left stick as the d-pad (platform::stick_to_dpad): half-way tilt holds the direction and presses it once.
#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdio>

#include "platform/window.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

platform::Input stick(float x, float y) {
  platform::Input input;
  input.pad_connected = true;
  input.pad_axis[SDL_GAMEPAD_AXIS_LEFTX] = x;
  input.pad_axis[SDL_GAMEPAD_AXIS_LEFTY] = y;
  return input;
}

bool down(const platform::Input& input, SDL_GamepadButton button) { return input.pad_down[static_cast<size_t>(button)]; }
bool pressed(const platform::Input& input, SDL_GamepadButton button) { return input.pad_pressed[static_cast<size_t>(button)]; }

void test_stick_to_dpad() {
  std::array<bool, 4> held{};
  // Below half way: nothing.
  platform::Input input = stick(0.4f, -0.45f);
  platform::stick_to_dpad(input, held);
  CHECK(!down(input, SDL_GAMEPAD_BUTTON_DPAD_UP) && !down(input, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
  // Up past half way: held and pressed on the first call, held only on the next.
  input = stick(0.0f, -0.8f);
  platform::stick_to_dpad(input, held);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_DPAD_UP) && pressed(input, SDL_GAMEPAD_BUTTON_DPAD_UP));
  CHECK(!down(input, SDL_GAMEPAD_BUTTON_DPAD_DOWN));
  input = stick(0.0f, -0.8f);
  platform::stick_to_dpad(input, held);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_DPAD_UP) && !pressed(input, SDL_GAMEPAD_BUTTON_DPAD_UP));
  // Diagonal: both directions; the new one is pressed.
  input = stick(0.7f, -0.7f);
  platform::stick_to_dpad(input, held);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_DPAD_UP) && down(input, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
  CHECK(!pressed(input, SDL_GAMEPAD_BUTTON_DPAD_UP) && pressed(input, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
  // Released, then pushed again: pressed again.
  input = stick(0.0f, 0.0f);
  platform::stick_to_dpad(input, held);
  CHECK(!down(input, SDL_GAMEPAD_BUTTON_DPAD_UP));
  input = stick(-0.9f, 0.0f);
  platform::stick_to_dpad(input, held);
  CHECK(pressed(input, SDL_GAMEPAD_BUTTON_DPAD_LEFT));
  // A real d-pad press is kept.
  input = stick(0.0f, 0.0f);
  input.pad_down[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = input.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true;
  platform::stick_to_dpad(input, held);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_DPAD_DOWN) && pressed(input, SDL_GAMEPAD_BUTTON_DPAD_DOWN));
}
}  // namespace

int main() {
  test_stick_to_dpad();
  std::printf("input: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
