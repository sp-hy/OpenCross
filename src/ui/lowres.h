#pragma once
// PS1-resolution UI pass ([Video] Ps1ResUi, any art mode). A screen that is drawn on the 1280 x 960 canvas is drawn instead into an
// off-screen target of its PS1 overlay's display size (menutest 512 x 448, the title / save list 640 x 448, ...): the SpriteBatch
// view space is the canvas itself (so the screen's layout code runs unchanged with vw = 1280, vh = 960), every texture is
// point-sampled, the pixel-font text is drawn on the target's pixel grid (TextBatch set_pixel_grid), and the target is then scaled
// up nearest into the 4:3 canvas rectangle of the window, composited over whatever the frame already holds (premultiplied alpha:
// the target is cleared transparent). Subtract and Multiply quads (window bases, B - F and B x F) cannot be held by a transparent target:
// they change only its colour, and are drawn again into two more targets of the same size (subtracted amounts, cleared black; multiplied
// factors, cleared white) that are applied to the frame first, so the picture under a window is darkened as at window resolution.
//
//   LowRes lr;  lr.init(ctx, sprites);                                   (shutdown() before the device goes)
//   per frame:  render::Frame lf;
//               if (lr.begin(frame, kMenutest, &text, lf)) { draw(lf, kCanvasW, kCanvasH, true); lr.end(frame, vw, vh); }
//               else draw(frame, vw, vh, resume);
// Must be called outside SpriteBatch begin() / end() pairs; every begin() inside the pass must pass resume = true (the frame's quad
// buffer is shared with what was drawn before).
#include "core/types.h"
#include "render/camera.h"
#include "render/render_target.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "render/texture.h"

namespace oc::ui {

// Display sizes of the PS1 overlays (the draw / display environments each overlay sets up; the height is the number of rows RDE
// maps onto the 960-unit canvas, which can be fewer than the overlay's buffer holds). `tag` names the pass in the texture log.
struct Ps1Display {
  u32 w, h;
  const char* tag;
};
inline constexpr Ps1Display kMenutest{512, 448, "lowres menutest 512x448"};
inline constexpr Ps1Display kMenuWide{640, 448, "lowres menutest 640x448"};
inline constexpr Ps1Display kShop{512, 432, "lowres shopmenu 512x432"};
inline constexpr Ps1Display kNameMenu{512, 432, "lowres namemenu 512x432"};
inline constexpr Ps1Display kMemmenu{640, 216, "lowres memmenu 640x216"};
inline constexpr Ps1Display kFieldWin{320, 216, "lowres field 320x216"};

// Global switch, set by the host from [Video] Ps1ResUi. Screens run the pass in EVERY art mode
// when it is on; only the PS1 disc textures stay classic-only (mode == Original && ps1_menus()).
void set_ps1_menus(bool on);
bool ps1_menus();

class LowRes {
 public:
  LowRes() = default;
  ~LowRes() { shutdown(); }
  LowRes(const LowRes&) = delete;
  LowRes& operator=(const LowRes&) = delete;

  bool init(render::GpuContext& ctx, render::SpriteBatch& sprites);
  void shutdown();
  bool ready() const { return ctx_ != nullptr; }

  // Starts the pass into a display.w x display.h target (created / resized on demand), cleared transparent; `out` = the target's
  // frame. `text` (optional) gets the target's pixel grid until end(). Returns false (nothing started) when the target cannot be made.
  // `canvas_w / h` = the canvas units the target spans (default: the whole 1280 x 960 canvas; a widened or partial pass gives its own).
  bool begin(const render::Frame& frame, const Ps1Display& display, render::TextBatch* text, render::Frame& out, float canvas_w = 1280.0f,
             float canvas_h = 960.0f);
  // Ends the pass and draws the target, nearest, into `dst` (window x, y, w, h), or without one into the canvas rectangle of the view
  // (render::canvas_fit: the 4:3 area centred in vw x vh at the shared zoom).
  void end(const render::Frame& frame, float vw, float vh, const render::Letterbox::Rect* dst = nullptr);

 private:
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::TextBatch* text_ = nullptr;
  // Draws the recorded quads of `mode` into `layer` (cleared to `clear`) with the blend `as`.
  void draw_layer(const render::Frame& frame, render::RenderTarget& layer, render::TexRef& set, render::BlendMode mode, render::BlendMode as,
                  const float (&clear)[4]);
  render::RenderTarget rt_;
  render::RenderTarget sub_rt_, mul_rt_;
  render::Sampler nearest_;
  render::TexRef set_ = {};
  render::TexRef sub_set_ = {}, mul_set_ = {};
  render::SpriteLayerCapture capture_;
  render::SpriteFilter old_filter_ = render::SpriteFilter::Sampler;
  bool active_ = false;
};

}  // namespace oc::ui
