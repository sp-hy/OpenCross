#pragma once
// RDE's keyboard bindings: two keys per action, its config.ini defaults. Every screen reads its keys through these and shows the key
// caps of the first set.
//
//   action        set 0       set 1        pad (PS1)
//   Up..Right     arrows      W S A D      d-pad
//   Ok            Enter       X            Circle
//   Cancel        Backspace   Left Shift   Cross (also run in the field)
//   Item          F           R            Square (the key-item list in the field)
//   Menu          I           Tab          Triangle (the main menu in the field)
//   SelectLeft    H           Q            L1
//   SelectRight   J           E            R1
//   SlowSpeed     F1          -            L2 (RDE's slow mode)
//   FastSpeed     F2          -            R2 (RDE's fast mode)
// Plus fixed keys outside the sets: Space = Start (the pause screen), K = Select (change the lead character), T = open the multiplayer chat
// (Enter is the first set's Ok).
#include <SDL3/SDL_scancode.h>

#include <array>

#include "platform/window.h"

namespace oc::platform {

constexpr SDL_Scancode kStartKey = SDL_SCANCODE_SPACE, kSelectKey = SDL_SCANCODE_K;
constexpr int kStartVk = 0x20, kSelectVk = 0x4B;
constexpr SDL_Scancode kChatKey = SDL_SCANCODE_T;

enum class Action { Up, Down, Left, Right, Ok, Cancel, Item, Menu, SelectLeft, SelectRight, SlowSpeed, FastSpeed, Count };

// SDL scancodes of both sets (0 = unbound).
std::array<int, 2> action_keys(Action action);
bool action_down(const Input& in, Action action);
bool action_pressed(const Input& in, Action action);
// Windows virtual keys of both sets (0 = unbound); action_vk = the first set's, the key cap RDE draws for the action.
std::array<int, 2> action_vks(Action action);
int action_vk(Action action);
// Cell of a virtual key in ui/keyboard_<l>.png (16 x 7 cells: column cell & 15, row cell >> 4); 0 = no glyph.
int keyboard_cell(int vk);

}  // namespace oc::platform
