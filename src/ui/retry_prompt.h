#pragma once
// The choice after a wipe that would end the game (Quality of life's Retry lost battles): a field message window centred on the black the
// battle faded to, in the player's window skin and the message font of the art mode, a heading line and three options under the
// field's choice hand. Up / down move the hand (menu sound 1), confirm takes the option (8); there is no cancel.
//
//   RetryPrompt prompt;  prompt.init(ctx, vfs, sprites);
//   prompt.open(hd, skin_byte, lang);  per frame: sfx = prompt.input(dy, confirm);  prompt.update(dt);
//   in a sprite pass: prompt.draw(vw, vh);  then prompt.take_choice() once the pick is made.
#include <array>
#include <memory>
#include <string>

#include "import/text.h"
#include "render/sprite_batch.h"
#include "ui/dialogue_box.h"
#include "vfs/vfs.h"

namespace oc::ui {

class RetryPrompt {
 public:
  enum class Choice { None, Retry, RetryAfterChanges, GiveUp };

  // The window's text in `lang`: the heading, then Retry, Retry after changes and Give up.
  static std::array<std::u32string, 4> lines(import::Lang lang);

  void init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites);
  // Opens with the hand on Retry. The window is built on the first opening and again when the language changes.
  void open(bool hd, int skin_byte, import::Lang lang);
  bool active() const { return open_; }
  // One frame of input edges; returns the menu sound to play (0 none, 1 cursor, 8 confirm).
  int input(int dy, bool confirm);
  void update(float dt);
  // Inside the caller's sprite pass.
  void draw(float vw, float vh);
  // The option taken, once (None until then); taking it closes the window.
  Choice take_choice();

 private:
  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  std::unique_ptr<DialogueDemo> box_;
  import::Lang box_lang_ = import::Lang::En;
  bool open_ = false;
  Choice choice_ = Choice::None;
};

}  // namespace oc::ui
