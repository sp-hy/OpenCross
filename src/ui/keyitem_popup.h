#pragma once
// Key-item list panel of the field.
//
// There is no pickup panel: receiving a key item (BB RECEIVE_ITEM, b == 0, id 0x401..0x440) only sets
// the bit; the room script itself shows the "<name> received <item>!" message window, plays sfx 283 or not, and waits.
//
// Key-item list (F / pad North): the field's window 4. 64 slots in the field's display order,
//    2 columns x 6 visible rows, scroll 0..26 row pairs. Both art modes use the same panel (window
//    skin tiles, slide up to y 495, the skin's hand cursor, names in gn_70 at 0.5): HD samples hd/window/wNN.png with
//    the large_ font, original art the PS1 sheet cdrom/map/window/wN.tim (as the dialogue window) with the small_ font.
//      p.set_style(hd, skin_byte, jp);  p.open_list(owned);
//      per frame: sfx = p.list_input(dx, dy, confirm, cancel);  if (int k = p.take_used()) ...;  p.update(dt, false);  p.draw(...)
#include <memory>
#include <string>
#include <vector>

#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "import/text.h"
#include "render/texture.h"
#include "vfs/vfs.h"

namespace oc::ui {

class KeyItemPopup {
 public:
  KeyItemPopup();
  ~KeyItemPopup();
  KeyItemPopup(const KeyItemPopup&) = delete;
  KeyItemPopup& operator=(const KeyItemPopup&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text);
  void shutdown();

  // ---- the field key-item list
  // id: key item 1..64. name: sysmes 0x180000 | id. enabled = false: drawn dimmed (bright 0.5) and refused on
  // confirm (Tele-Porter / Relief Charm / Smith Spirit while script vars 0x46 / 0x3C are not both 1).
  struct ListEntry {
    int id = 0;
    std::u32string name;
    bool enabled = true;
  };
  // hd: art mode (skin sheet + font files); skin_byte: window-type option byte (0..14; 0 = default texture w04); jp: Japanese text offsets.
  void set_style(bool hd, int skin_byte, bool jp = false);
  // Text language of the rows: open_list() replaces each entry's name by sysmes 0x18 | id in that language (lang.dat) when it exists.
  void set_language(import::Lang lang);
  // `owned` in any order; the list packs them in the field's display order from slot 0 on, the remaining slots stay empty.
  void open_list(std::vector<ListEntry> owned);
  // One frame of list input (edges). dx / dy: -1, 0, 1. Returns the sound effect id to play (0 none, 1 cursor, 6 cancel,
  // 8 use, 0x18 refused). A confirmed usable item closes the list and is returned once by take_used(); a confirmed disabled
  // entry keeps the list open and is returned once by take_refused().
  int list_input(int dx, int dy, bool confirm, bool cancel);
  int take_used();
  int take_refused();
  bool list_mode() const;
  // Item under the hand (0 = empty slot).
  int selected_id() const;

  // Jump to the fully shown state (screenshots).
  void finish_intro();
  // True until the list is closed.
  bool active() const;
  // dt seconds (the list closes through list_input).
  void update(float dt);
  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);

 private:
  void draw_view(const render::Frame& frame, float vw, float vh, bool resume);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::ui
