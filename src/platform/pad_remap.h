#pragma once
// The Customize > Controller: Custom assignment. Eight buttons (Circle, Cross, Square, Triangle, L1, R1, L2, R2: the order of
// platform::Action Ok .. FastSpeed) each trigger the function whose nibble they sit in: face nibble f = the physical button that
// triggers function f. The aux word holds the stick swap (nibble 0 = 1 puts the left stick on the right one) and the click swap
// (nibble 2 other than 2 swaps L3 / R3). Field and menus use the field pair of the save, battle the battle pair.
#include <array>

#include "core/types.h"
#include "platform/keymap.h"
#include "platform/window.h"

namespace oc::platform {

constexpr int kMappedButtons = 8;
constexpr u32 kDefaultFace = 0x76543210u, kDefaultAux = 0x3210u;

struct ButtonMap {
  u32 face = kDefaultFace;
  u32 aux = kDefaultAux;
  bool custom = false;
};

// The physical button (0..7) that triggers `function`; `function` itself when the map is not custom.
int map_source(const ButtonMap& map, int function);
// Swaps the nibbles `a` and `b` of the face word (or of the aux word when `aux`).
void map_swap(ButtonMap& map, int a, int b, bool aux);
// Back to the default face and aux words, whatever `custom` says.
void map_reset(ButtonMap& map);

// The action of button index 0..7 (Ok .. FastSpeed) and the index of an action (-1 for the others).
Action button_action(int index);
int action_button(Action action);

// The ui/gamepad.png cell (13 x 4 atlas) of button index 0..7, 8 = Start, 9 = Select.
int pad_glyph_cell(int index);

// The map the screens read their glyphs through, set once a frame by the app.
void set_active_map(const ButtonMap& map);
const ButtonMap& active_map();
// The action whose button triggers `action` under the active map: the glyph to show for it.
Action glyph_action(Action action);

// Rewrites the keyboard keys, pad buttons and trigger / stick axes of one frame so that function f reads the state of its source
// button. Holds the previous trigger state for the edge.
class PadRemap {
 public:
  void apply(Input& input, const ButtonMap& map);

 private:
  std::array<bool, 2> trigger_held_{};
};

}  // namespace oc::platform
