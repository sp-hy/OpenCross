#pragma once
// RDE's world name in HD art: hd.dat map/HomeAnother/{home,another}_<lang>.png stretched over (20, 7) .. (330, 207) of the 1280 x 960
// canvas (from the picture's left edge). It shows while the map has a world name (script var 0x50) and the World Names option (var
// 0x52) is on, whatever windows are open (the location labels leave it up), fading in by 0.0625 and out by 0.065 of full opacity per
// 60 Hz frame. The PS1 draws the same name as a screen-fixed layer group of the world-map rooms; the HD art replaces it.
// The same layer holds RDE's room cards (import::room_cards): the card's picture over its canvas rectangle, fading in and out at the
// card's rates while a card is passed; it is drawn over the room fade, which does not dim it.
// Both fades step by the frames of `clock`, the app's 60 Hz frame count (route steps in a route, real time otherwise; fast and slow
// mode do not change it).
//   name.init(ctx, vfs, sprites);   per frame: name.draw(frame, vw, vh, left, show, home, lang, clock);
//                                              name.draw_room_card(frame, vw, vh, left, card, clock);
#include <string_view>

#include "import/room_cards.h"
#include "import/text.h"
#include "render/sprite_batch.h"
#include "render/texture.h"
#include "vfs/vfs.h"

namespace oc::ui {

class WorldName {
 public:
  WorldName() = default;
  ~WorldName() { shutdown(); }
  WorldName(const WorldName&) = delete;
  WorldName& operator=(const WorldName&) = delete;

  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites);
  void shutdown();
  // The Home World maps (every other one is Another World): 0x8F, 0xFA, 0xFC, 0xFE, 0x102, 0x1BB, 0x213.
  static bool home_map(int map);
  // Advances the fade and draws over the frame (inside no begin / end). `left` = window x of the picture's left edge.
  void draw(const render::Frame& frame, float vw, float vh, float left, bool show, bool home, import::Lang lang, double clock);
  // `card` = the card to show, nullptr to fade the last one out.
  void draw_room_card(const render::Frame& frame, float vw, float vh, float left, const import::RoomCard* card, double clock);

 private:
  void load(import::Lang lang);

  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::Sampler linear_;
  // 0 = Home World, 1 = Another World.
  render::Texture art_[2];
  render::TexRef set_[2] = {};
  render::Texture card_art_;
  render::TexRef card_set_ = {};
  std::string_view card_image_;
  const import::RoomCard* shown_ = nullptr;
  double card_alpha_ = 0.0;
  double card_last_ = -1.0;
  int loaded_lang_ = -1;
  double alpha_ = 0.0;
  double last_ = -1.0;
};

}  // namespace oc::ui
