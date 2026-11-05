#include "platform/pad_remap.h"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <utility>

namespace oc::platform {

namespace {

constexpr Action kActions[kMappedButtons] = {Action::Ok,         Action::Cancel,     Action::Item,       Action::Menu,
                                             Action::SelectLeft, Action::SelectRight, Action::SlowSpeed, Action::FastSpeed};
constexpr SDL_GamepadButton kPadButtons[kMappedButtons] = {
    SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_EAST,           SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID};
constexpr SDL_GamepadAxis kTriggers[2] = {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
constexpr float kTriggerPress = 0.5f;

ButtonMap g_active;

struct Source {
  std::array<bool, 2> key_down{}, key_pressed{};
  bool pad_down = false, pad_pressed = false;
  float axis = 0.0f;
};

}  // namespace

int map_source(const ButtonMap& map, int function) {
  if (!map.custom) return function;
  return static_cast<int>((map.face >> ((function & 7) * 4)) & 0xF);
}

void map_swap(ButtonMap& map, int a, int b, bool aux) {
  u32& word = aux ? map.aux : map.face;
  const u32 nibble_a = (word >> (a * 4)) & 0xF, nibble_b = (word >> (b * 4)) & 0xF;
  word &= ~((0xFu << (a * 4)) | (0xFu << (b * 4)));
  word |= (nibble_b << (a * 4)) | (nibble_a << (b * 4));
}

void map_reset(ButtonMap& map) {
  map.face = kDefaultFace;
  map.aux = kDefaultAux;
}

Action button_action(int index) { return kActions[std::clamp(index, 0, kMappedButtons - 1)]; }

int action_button(Action action) {
  for (int i = 0; i < kMappedButtons; ++i)
    if (kActions[i] == action) return i;
  return -1;
}

int pad_glyph_cell(int index) {
  static constexpr int kCells[kMappedButtons + 2] = {8, 9, 0xB, 0xA, 0x17, 0x18, 0x15, 0x16, 13, 14};
  return kCells[std::clamp(index, 0, kMappedButtons + 1)];
}

void set_active_map(const ButtonMap& map) { g_active = map; }

const ButtonMap& active_map() { return g_active; }

Action glyph_action(Action action) {
  const int function = action_button(action);
  if (function < 0) return action;
  return button_action(map_source(g_active, function) & 7);
}

void PadRemap::apply(Input& input, const ButtonMap& map) {
  std::array<bool, 2> trigger_now{};
  for (int i = 0; i < 2; ++i) trigger_now[static_cast<size_t>(i)] = input.pad_axis[kTriggers[i]] >= kTriggerPress;
  if (map.custom) {
    std::array<Source, kMappedButtons> sources;
    for (int i = 0; i < kMappedButtons; ++i) {
      Source& source = sources[static_cast<size_t>(i)];
      const std::array<int, 2> keys = action_keys(kActions[i]);
      for (size_t k = 0; k < 2; ++k)
        if (keys[k] > 0) {
          source.key_down[k] = input.key_down[static_cast<size_t>(keys[k])];
          source.key_pressed[k] = input.key_pressed[static_cast<size_t>(keys[k])];
        }
      if (kPadButtons[i] != SDL_GAMEPAD_BUTTON_INVALID) {
        source.pad_down = input.pad_down[kPadButtons[i]];
        source.pad_pressed = input.pad_pressed[kPadButtons[i]];
        source.axis = source.pad_down ? 1.0f : 0.0f;
      } else {
        const size_t trigger = static_cast<size_t>(i - 6);
        source.axis = input.pad_axis[kTriggers[trigger]];
        source.pad_down = trigger_now[trigger];
        source.pad_pressed = trigger_now[trigger] && !trigger_held_[trigger];
      }
    }
    for (int function = 0; function < kMappedButtons; ++function) {
      const int from = map_source(map, function);
      if (from == function) continue;
      const Source& source = sources[static_cast<size_t>(from & 7)];
      const std::array<int, 2> keys = action_keys(kActions[function]);
      for (size_t k = 0; k < 2; ++k)
        if (keys[k] > 0) {
          input.key_down[static_cast<size_t>(keys[k])] = source.key_down[k];
          input.key_pressed[static_cast<size_t>(keys[k])] = source.key_pressed[k];
        }
      if (kPadButtons[function] != SDL_GAMEPAD_BUTTON_INVALID) {
        input.pad_down[kPadButtons[function]] = source.pad_down;
        input.pad_pressed[kPadButtons[function]] = source.pad_pressed;
      } else {
        input.pad_axis[kTriggers[function - 6]] = source.axis;
      }
    }
    if ((map.aux & 0xF) == 1) {
      std::swap(input.pad_axis[SDL_GAMEPAD_AXIS_LEFTX], input.pad_axis[SDL_GAMEPAD_AXIS_RIGHTX]);
      std::swap(input.pad_axis[SDL_GAMEPAD_AXIS_LEFTY], input.pad_axis[SDL_GAMEPAD_AXIS_RIGHTY]);
    }
    if (((map.aux >> 8) & 0xF) != 2) {
      std::swap(input.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK], input.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_STICK]);
      std::swap(input.pad_pressed[SDL_GAMEPAD_BUTTON_LEFT_STICK], input.pad_pressed[SDL_GAMEPAD_BUTTON_RIGHT_STICK]);
    }
  }
  trigger_held_ = trigger_now;
}

}  // namespace oc::platform
