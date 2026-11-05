// The left stick as the d-pad (platform::stick_to_dpad): half-way tilt holds the direction and presses it once. The idle mouse pointer rule
// (platform::pointer_hidden). The Controller: Custom assignment (platform::PadRemap, map_source, map_swap, glyph_action). The field's pad lock after a menu screen or the key-item list (game::FieldInput::apply_lock).
#include <SDL3/SDL_gamepad.h>

#include <array>
#include <cstdio>

#include "game/field_input.h"
#include "platform/pad_remap.h"
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

void test_pointer_hidden() {
  constexpr u64 kIdle = platform::kPointerIdleMs;
  CHECK(platform::pointer_hidden(true, false, kIdle));
  CHECK(!platform::pointer_hidden(true, false, kIdle - 1));
  CHECK(!platform::pointer_hidden(true, true, kIdle));
  CHECK(!platform::pointer_hidden(false, false, kIdle));
}

void test_pad_remap() {
  using platform::Action;
  platform::ButtonMap map;
  CHECK(platform::map_source(map, 3) == 3);
  map.custom = true;
  CHECK(platform::map_source(map, 3) == 3 && platform::map_source(map, 7) == 7);
  platform::map_swap(map, 0, 2, false);
  CHECK(map.face == 0x76543012u && platform::map_source(map, 0) == 2 && platform::map_source(map, 2) == 0);
  platform::map_swap(map, 0, 1, true);
  CHECK(map.aux == 0x3201u);
  platform::map_reset(map);
  CHECK(map.face == platform::kDefaultFace && map.aux == platform::kDefaultAux);

  // Not custom: the frame is left alone.
  platform::PadRemap remap;
  platform::Input input;
  input.pad_connected = true;
  input.pad_down[SDL_GAMEPAD_BUTTON_WEST] = input.pad_pressed[SDL_GAMEPAD_BUTTON_WEST] = true;
  platform::ButtonMap plain;
  remap.apply(input, plain);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_WEST) && !down(input, SDL_GAMEPAD_BUTTON_SOUTH));

  // Confirm on Square and Square on Confirm: the pad buttons and the keys of the two actions trade places.
  platform::ButtonMap swapped;
  swapped.custom = true;
  platform::map_swap(swapped, 0, 2, false);
  input = platform::Input();
  input.pad_connected = true;
  input.pad_down[SDL_GAMEPAD_BUTTON_WEST] = input.pad_pressed[SDL_GAMEPAD_BUTTON_WEST] = true;
  const int ok_key = platform::action_keys(Action::Ok)[0], item_key = platform::action_keys(Action::Item)[0];
  input.key_down[static_cast<size_t>(item_key)] = true;
  remap.apply(input, swapped);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_SOUTH) && pressed(input, SDL_GAMEPAD_BUTTON_SOUTH) && !down(input, SDL_GAMEPAD_BUTTON_WEST));
  CHECK(input.key_down[static_cast<size_t>(ok_key)] && !input.key_down[static_cast<size_t>(item_key)]);

  // L2 as Confirm: the trigger past half way presses the button once; a button as R2 fills the trigger axis.
  platform::ButtonMap triggers;
  triggers.custom = true;
  platform::map_swap(triggers, 0, 6, false);
  platform::map_swap(triggers, 3, 7, false);
  input = platform::Input();
  input.pad_connected = true;
  input.pad_axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 1.0f;
  input.pad_down[SDL_GAMEPAD_BUTTON_NORTH] = input.pad_pressed[SDL_GAMEPAD_BUTTON_NORTH] = true;
  remap.apply(input, triggers);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_SOUTH) && pressed(input, SDL_GAMEPAD_BUTTON_SOUTH));
  CHECK(input.pad_axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] == 0.0f && input.pad_axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] == 1.0f);
  input = platform::Input();
  input.pad_connected = true;
  input.pad_axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 1.0f;
  remap.apply(input, triggers);
  CHECK(down(input, SDL_GAMEPAD_BUTTON_SOUTH) && !pressed(input, SDL_GAMEPAD_BUTTON_SOUTH));

  // The aux word swaps the sticks and the stick clicks.
  platform::ButtonMap sticks;
  sticks.custom = true;
  platform::map_swap(sticks, 0, 1, true);
  input = platform::Input();
  input.pad_axis[SDL_GAMEPAD_AXIS_LEFTX] = 0.5f;
  input.pad_axis[SDL_GAMEPAD_AXIS_RIGHTY] = -0.25f;
  input.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK] = true;
  remap.apply(input, sticks);
  CHECK(input.pad_axis[SDL_GAMEPAD_AXIS_RIGHTX] == 0.5f && input.pad_axis[SDL_GAMEPAD_AXIS_LEFTY] == -0.25f);
  CHECK(input.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK] && !input.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_STICK]);

  // Glyphs follow the active map: the Confirm glyph is the one of the button now on it.
  platform::set_active_map(swapped);
  CHECK(platform::glyph_action(Action::Ok) == Action::Item && platform::glyph_action(Action::Item) == Action::Ok);
  CHECK(platform::glyph_action(Action::Up) == Action::Up);
  platform::set_active_map(platform::ButtonMap());
  CHECK(platform::glyph_action(Action::Ok) == Action::Ok);
}
// A key-item list lock (8 field frames = 16 ticks) and the room-load lock: a held direction and a confirm press read as released for 16 ticks, then come back.
void test_field_lock() {
  game::FieldInput field;
  platform::Input input = stick(0.0f, 0.0f);
  input.pad_down[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
  input.pad_pressed[SDL_GAMEPAD_BUTTON_SOUTH] = input.pad_down[SDL_GAMEPAD_BUTTON_SOUTH] = true;
  field.lock = 16;
  field.menu_wait = 24;
  for (int tick = 0; tick < 16; ++tick) {
    game::FieldInput::Edges edges = field.sample(input, 1);
    field.apply_lock(edges, 1);
    CHECK(!edges.confirm && field.held_mask == 0 && field.pad_word == 0 && field.latched_keys() == 0);
  }
  CHECK(field.lock == 0 && field.menu_wait == 8);
  game::FieldInput::Edges edges = field.sample(input, 1);
  field.apply_lock(edges, 1);
  CHECK(edges.confirm && field.held_mask == 8 && field.pad_word != 0);
  // A room load locks the pad (Select included) for 24 field frames = 48 ticks.
  game::FieldInput room;
  room.reset();
  input.pad_pressed[SDL_GAMEPAD_BUTTON_BACK] = input.pad_down[SDL_GAMEPAD_BUTTON_BACK] = true;
  int locked = 0;
  for (int tick = 0; tick < 60; ++tick) {
    edges = room.sample(input, 1);
    room.apply_lock(edges, 1);
    if (!edges.select && !edges.confirm && room.held_mask == 0) ++locked;
  }
  CHECK(locked == 48);
  // A field movie ends with Left still held: 24 field frames locked (FieldWorld::lock_input(24)), the walk resumes after.
  room.lock = 24 * 2;
  input.pad_pressed[SDL_GAMEPAD_BUTTON_BACK] = input.pad_down[SDL_GAMEPAD_BUTTON_BACK] = false;
  int first_walk = -1;
  for (int tick = 0; tick < 60 && first_walk < 0; ++tick) {
    edges = room.sample(input, 1);
    room.apply_lock(edges, 1);
    if (room.held_mask != 0) first_walk = tick;
  }
  CHECK(first_walk == 48);
}
}  // namespace

int main() {
  test_stick_to_dpad();
  test_pad_remap();
  test_pointer_hidden();
  test_field_lock();
  std::printf("input: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
