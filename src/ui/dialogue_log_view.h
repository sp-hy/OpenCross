#pragma once
// The dialogue log window ([Game] OpenCrossMenu): the logged messages as one scrolling list in the field message window's style - the
// window skin's frame around the list, each message with its portrait in the message font, a loaded game as a divider line, the option
// picked in a choice in the window's highlight colour, and in the title row the Clear log prompt. Shown over the field (its key or pad
// chord; the field stands still) and as the DialogueLog page of the OpenCross menu row. Laid out on the message window's 1280 x 960 canvas.
//
//   DialogueLogView view;  view.init(ctx, vfs, sprites);  view.set_log(&log);
//   view.open();  per frame: view.input(nav) (a sound id), view.take_replay(), view.draw(frame, vw, vh)  until it is closed.
#include <memory>
#include <string>
#include <vector>

#include "import/text.h"
#include "render/sprite_batch.h"
#include "save/dialogue_log.h"
#include "ui/dialogue_box.h"
#include "vfs/vfs.h"

namespace oc::ui {

class DialogueLogView {
 public:
  DialogueLogView();
  ~DialogueLogView();
  DialogueLogView(const DialogueLogView&) = delete;
  DialogueLogView& operator=(const DialogueLogView&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites);
  void shutdown();
  // The log shown; must outlive the view.
  void set_log(const save::DialogueLog* log);
  // The art mode (skin sheet, portraits and fonts), the window skin byte of the game options (0 = the default skin), the HD portrait set
  // ("2021" / "1999") and the language of the window's own words.
  void set_style(ArtMode mode, int skin_byte, const std::string& portrait_set, import::Lang lang);
  // The Clear log prompt's button glyph: the key cap of the bound key (the keyboard was used last), else the pad glyph.
  void set_key_caps(bool keyboard);

  // Opens on the newest entry, scrolled to the end of the list.
  void open();
  void close();
  bool active() const;

  // One frame of input: `move` entries (negative = up), `page` screens of entries, `first` / `last` jump to the ends, `replay` (confirm)
  // plays the voice files of the selected entry again (take_replay), `clear` (the key-item button) asks whether to clear the log. While
  // that question is up, `move` or `page` go between Yes and No, confirm answers and `cancel` is No; the window ignores `cancel` otherwise.
  // Returns the sound to play (1 cursor, 8 the question opened or Yes, 6 cancelled, 0 none).
  struct Nav {
    int move = 0, page = 0;
    bool first = false, last = false, replay = false, clear = false, cancel = false;
  };
  int input(const Nav& nav);
  // The Clear log question is up (the host leaves Cancel to it).
  bool asking() const;
  // The voice files the last replay asked for, in page order (empty: none asked), and then nothing.
  std::vector<std::string> take_replay();
  // True once after the Clear log question was answered Yes: the host clears the log.
  bool take_clear();

  // The window over the frame: its own SpriteBatch pass (resume = after an earlier pass of the frame).
  void draw(const render::Frame& frame, float vw, float vh, bool resume = true);

  // The entries listed and the selected one (an index into them), for route checks.
  size_t entry_count() const;
  int cursor() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::ui
