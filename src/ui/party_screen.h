#pragma once
// The two party-member screens of the menu overlay:
//   Party  = the 3-slot "Current Party" screen (FE 5B PARTY_SCREEN, the Tele-Porter). Rules: game::PartyEditor.
//   Relief = the Relief Charm screen: pick the relief member. Rules: game::ReliefEditor.
// 640 x 224 PS1 screens (lock-step 60 Hz).
//
// One look in both art modes: the PS1 picture underneath (bg.tim tiled; the w17 windows come from cdrom2's blank
// map/window/w17.tim in both modes, so they are invisible), the mostly transparent HD backdrop
// hd/menu/shapes/memmenu.png / memmenu2.png with the window lines, portraits hd/face/<set>/<name>.png, the cursor
// hd/menu/memmenu_cur.png, memmenu2's arrow hd/menu/memmenu2_cur.png and the PC text (memmenu.txt, cmnmes). Original art swaps
// only the font files (small_font_*) and the face folder (1999).
//
//   s.open(game_state) / s.open_relief(game_state);  per frame: if (int sfx = s.update(input, dt)) play(sfx);  s.draw(frame, vw, vh);
//   when s.take_finished(): Party -> write s.result() as the party (compacted); Relief -> write s.relief_result().
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "render/texture.h"
#include "vfs/vfs.h"

namespace oc::ui {

class GameState;

class PartyScreen {
 public:
  enum class Kind { Party, Relief };

  PartyScreen();
  ~PartyScreen();
  PartyScreen(const PartyScreen&) = delete;
  PartyScreen& operator=(const PartyScreen&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text, import::Lang lang);
  void shutdown();

  // Art mode: HD = the large_ fonts and the [Game] Portraits face set; original = the small_ fonts and hd/face/1999.
  void set_hd(bool hd);
  // "2021" (default) or "1999": the HD art face set.
  void set_portrait_set(const std::string& set);
  // The player's names (may be null).
  void set_custom_names(const std::map<int, std::u32string>* names);

  // [Video] Widescreen: `canvas_columns` = layer columns per side the backdrop extends by; [Video] WideHud: `layout_columns` = the
  // columns the left half (status, current party) and the right half (member grid) move out by.
  void set_wide_columns(float canvas_columns, float layout_columns);
  // Party swap's reserve (OpenCross): `size` reserve slots on the Party screen (0 = the vanilla screen). Up to 3 are portraits in a
  // Reserve window under a shorter member grid, more a scrolling list of names and HP (game::PartyEditor). Applies from the next open().
  void set_reserve_size(int size);
  // The PartyScreen feature's ReturnToSlot: a pick returns to the slot it filled (game::PartyEditor::set_return_to_slot). Applies from the next open().
  void set_return_to_slot(bool on);
  // Snapshot of the party, the grid (recruited and not excluded), the lock bitmap, the reserve and the shown records.
  void open(const GameState& gs);
  // Relief Charm (memmenu2): the same grid; the relief member is save head 0x59F.
  void open_relief(const GameState& gs);
  bool active() const;
  Kind kind() const;
  // One update: input edges + key repeat, `dt` seconds of 60 Hz frames (scroll, fades). Returns the sound effect to play (0 none).
  int update(const platform::Input& in, float dt);
  // True once, after the closing fade: the party to write back (0xFF = empty, compacted) / the relief member (0xFF = none).
  bool take_finished();
  std::array<u8, 3> result() const;
  u8 relief_result() const;
  // The reserve slots to write back (-1 = empty); empty when the screen had no reserve.
  std::vector<int> reserve_result() const;

  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);

  // Routes: the screen state for assertions ("mode slot col row scroll party [reserve ...]" / "relief col row scroll member").
  std::string describe() const;

 private:
  void draw_view(const render::Frame& frame, float vw, float vh, bool resume);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Save head byte of the relief member (the byte after the three party bytes, head 0x59C..0x59E).
inline constexpr size_t kReliefHead = 0x59F;

}  // namespace oc::ui
