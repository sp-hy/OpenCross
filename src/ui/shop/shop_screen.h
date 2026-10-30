#pragma once
// ShopScreen: the three shop overlays in one widget -- element shop, blacksmith (forge /
// break down / sell) and the trading post. Rules: ui/shop/shop_logic.h. Same look as the main
// menu (1280x960 canvas fitted into the view). One layout in both art modes: original art swaps only the font files
// (small_font_*) and the face folder (hd/face/1999).
//
//   ShopScreen s;  s.init(ctx, vfs, sprites, text, tables, lang);  s.set_rules(&rules);
//   s.open(state, ShopKind::Element, shop_id);  per frame: s.update(input); s.draw(frame, vw, vh [, resume]); until s.closed().
// draw() runs its own SpriteBatch begin()/end() pair (resume = continue the frame's quad buffer after an earlier pair).
#include <functional>
#include <memory>
#include <string>

#include "data/tables.h"
#include "import/text.h"
#include "platform/window.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "ui/dialogue_box.h"  // ArtMode
#include "ui/menu/game_state.h"
#include "ui/shop/shop_logic.h"
#include "vfs/vfs.h"

namespace oc::ui {

using shop::Kind;
using ShopKind = shop::Kind;

class ShopScreen {
 public:
  ShopScreen();
  ~ShopScreen();
  ShopScreen(const ShopScreen&) = delete;
  ShopScreen& operator=(const ShopScreen&) = delete;

  // `tables` must outlive the screen. Re-reads waa_shop.bin into the shop data.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text,
            const data::GameTables& tables, import::Lang lang);
  void shutdown();

  // `rules` must outlive the screen; needed before open().
  void set_rules(const MenuRules* rules);
  void set_mode(ArtMode mode);
  ArtMode mode() const;
  void set_language(import::Lang lang);
  // Sound hook: 1 cursor, 8 confirm, 6 cancel, 0x18 denied, 5 bought/sold, 0xBF forged/broken down, 0x1B traded.
  void set_sfx(std::function<void(int)> fn);

  void open(GameState& state, ShopKind kind, int shop_id);
  void close();
  bool closed() const;
  ShopKind kind() const;
  int shop_id() const;
  // Current state for logs / demo: "top", "category", "list", "quantity", "confirm", "offer".
  std::string state_name() const;

  // ticks = 60 Hz sim ticks this frame (the animations advance by them).
  void update(const platform::Input& in, int ticks = 1);
  // The item last forged with none in the bag before, then 0 until the next.
  int take_first_forged();
  void draw(const render::Frame& frame, float vw, float vh, bool resume = false);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::ui
