#include "ui/pause_screen.h"

#include <cmath>

#include "core/log.h"
#include "render/camera.h"

namespace oc::ui {

namespace {
constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
// ui/pause.png (170 x 83): the slanted bar (outline, fill, drop shadow) in y 0..59, a left and a right arrow below it. kCap = the
// source columns kept unstretched at each end of the bar; the arrows are x0, y0, x1, y1 in source pixels.
constexpr float kSrcW = 170.0f, kBarH = 60.0f, kCap = 38.0f;
constexpr float kLeftArrow[4] = {1, 61, 13, 83}, kRightArrow[4] = {18, 61, 31, 83};
// The pair of arrows as one strip; the World Names row draws it turned a quarter clockwise (left -> up, right -> down).
constexpr float kArrowStrip[4] = {0, 61, 31, 83};
// The art is drawn 1:1 at 1080p: kArt canvas units per art pixel (canvas 960 high). kTitleY / kRowY are the two bar centres.
constexpr float kArt = 960.0f / 1080.0f;
constexpr float kTitleY = 0.424f * kCanvasH, kRowY = 0.538f * kCanvasH;
// The World Names row sits 89 canvas units below the Vibration row.
constexpr float kRow2Y = kRowY + 89.0f;
}  // namespace

bool PauseScreen::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text) {
  ctx_ = &ctx, vfs_ = &vfs, sprites_ = &sprites, text_ = &text;
  if (!linear_.create(ctx, true) || !nearest_.create(ctx, false)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, px)) return false;
  white_set_ = render::TexRef(white_, nearest_);
  render::DecodedImage img;
  // The art is drawn as it is (a plain sprite draw, no recolouring): white 4 px outline + white arrows, translucent-black fill
  // (alpha 175), lavender-grey 4 px drop shadow; the text is white.
  if (auto raw = vfs.read("ui/pause.png"); raw && render::decode_image(*raw, img) && img.width == 170 && img.height == 83) {
    if (bar_.create(ctx, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data())) {
      bar_set_ = render::TexRef(bar_, linear_);
      bar_w_ = static_cast<float>(img.width), bar_h_ = static_cast<float>(img.height);
    }
  }
  if (!bar_set_) log::warn("pause: ui/pause.png unavailable; plain bars");
  // RDE uses the _small font file only in original graphics mode with the small UI; the HD file serves both modes here.
  font_ = font::load(vfs, "ui/ui_font_gnb.spritefont");
  if (font_) {
    face_ = text.add_spritefont(font_, {}, U"PAUSE");
    if (const import::SfGlyph* glyph = font_->find('P'); glyph && glyph->height() > 0)
      font_scale_ = 0.55f * kBarH * kArt / (static_cast<float>(glyph->height()) * 1.15f);
  } else {
    log::warn("pause: ui/ui_font_gnb.spritefont unavailable");
  }
  lt_.init(&vfs);
  return true;
}

void PauseScreen::shutdown() {
  if (!ctx_) return;
  bar_set_ = {};
  white_set_ = {};
  bar_.destroy();
  white_.destroy();
  linear_.destroy();
  nearest_.destroy();
  ctx_ = nullptr;
}

void PauseScreen::open(bool vibration, bool world_row, bool world_names) {
  vibration_ = vibration;
  world_row_ = world_row;
  world_names_ = world_names;
  open_ = true;
  last_ = std::chrono::steady_clock::now();
}

void PauseScreen::tint(float (&rgb)[3]) {
  const auto now = std::chrono::steady_clock::now();
  counter_ = std::fmod(counter_ + std::chrono::duration<double>(now - last_).count() * 60.0, 480.0);
  last_ = now;
  const int count = static_cast<int>(counter_);
  const int ramp = count > 0xEF ? 480 - count : count;
  const float shift = static_cast<float>(ramp) / 240.0f * 0.53f;
  rgb[0] = 1.0f - shift, rgb[1] = 0.47f, rgb[2] = 0.47f + shift;
}

int PauseScreen::input(int dx, int dy, bool toggle, bool close) {
  if (!open_) return 0;
  if (close) {
    open_ = false;
    return 6;
  }
  int sfx = 0;
  if (dx != 0 || toggle) {
    vibration_ = !vibration_;
    sfx = 1;
  }
  if (dy != 0 && world_row_) {
    world_names_ = !world_names_;
    sfx = 1;
  }
  return sfx;
}

std::u32string PauseScreen::ui_text(int id, const char32_t* fallback) {
  if (const import::TextTable* table = lt_.table("ui_pausemenu"))
    if (const std::string* str = table->get(static_cast<u64>(id), lang_); str && !str->empty()) return import::utf8_to_u32(*str);
  return fallback;
}

// The title bar is the art at its natural size; the option bar is sized to its text plus the two arrows tucked inside its right end
// (only the middle slice of the art stretches). The purple ramp over the frozen picture is the full-screen shader pass's ramp mode, applied by the
// host before this draw.
void PauseScreen::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  if (!open_ || !ctx_) return;
  const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
  const float zoom = fit.s, ox = fit.ox, oy = fit.oy;
  const std::u32string title = ui_text(0, U"PAUSE");
  const std::u32string line = ui_text(1, U"Vibration Function") + U" : " + ui_text(vibration_ ? 4 : 3, vibration_ ? U"On" : U"Off");
  const std::u32string line2 = ui_text(2, U"World Names") + U" : " + ui_text(world_names_ ? 4 : 3, world_names_ ? U"On" : U"Off");
  if (face_ != render::kNoFace) {
    text_->prepare(face_, title);
    text_->prepare(face_, line);
    if (world_row_) text_->prepare(face_, line2);
  }

  sprites_->begin(frame, vw, vh, resume);

  // A slanted bar centred at (cx, cy) (canvas units), `art_scale` = view scale of the art, total width `w` (end caps unstretched).
  auto bar = [&](float cx, float cy, float w, float art_scale, u8 alpha) {
    const float x = ox + (cx - w * 0.5f) * zoom, y = oy + (cy - kBarH * art_scale * 0.5f) * zoom;
    const float h = kBarH * art_scale * zoom, cap = kCap * art_scale * zoom, width = w * zoom;
    const u32 col = render::pack_rgba(255, 255, 255, alpha);
    if (!bar_set_) {
      sprites_->draw(white_set_, x, y, width, h, 0, 0, 1, 1, col);
      return;
    }
    const float v1 = kBarH / bar_h_;
    sprites_->draw(bar_set_, x, y, cap, h, 0, 0, kCap / bar_w_, v1, col);
    sprites_->draw(bar_set_, x + cap, y, width - 2 * cap, h, kCap / bar_w_, 0, (kSrcW - kCap) / bar_w_, v1, col);
    sprites_->draw(bar_set_, x + width - cap, y, cap, h, (kSrcW - kCap) / bar_w_, 0, 1.0f, v1, col);
  };
  auto arrow = [&](const float (&src)[4], float cx, float cy, float art_scale, u32 col) {
    if (!bar_set_) return;
    const float w = (src[2] - src[0]) * art_scale * zoom, h = (src[3] - src[1]) * art_scale * zoom;
    sprites_->draw(bar_set_, ox + cx * zoom - w * 0.5f, oy + cy * zoom - h * 0.5f, w, h, src[0] / bar_w_, src[1] / bar_h_, src[2] / bar_w_,
                   src[3] / bar_h_, col);
  };
  render::TextStyle style;
  style.backdrop_color = render::pack_rgba(30, 30, 40, 255);
  const u32 text_colour = render::pack_rgba(255, 255, 255);
  const float text_scale = font_scale_ * zoom;
  auto text_w = [&](const std::u32string& str) { return face_ == render::kNoFace ? 0.0f : text_->measure(face_, str, text_scale) / zoom; };
  // Text centred at (cx, cy) in canvas units, vertically by its line box.
  auto centred = [&](const std::u32string& str, float cx, float cy) {
    if (face_ == render::kNoFace) return;
    const float w = text_->measure(face_, str, text_scale), lh = text_->line_height(face_, text_scale);
    text_->draw(face_, str, ox + cx * zoom - w * 0.5f, oy + cy * zoom - lh * 0.5f, text_colour, text_scale, style);
  };

  const float cx = kCanvasW * 0.5f;
  bar(cx, kTitleY, kSrcW * kArt, kArt, 255);
  centred(title, cx, kTitleY);
  // An option row: the bar sized to its text plus the arrows tucked inside its right end.
  auto option_row = [&](const std::u32string& str, float cy, bool vertical) {
    const float tw = text_w(str), arrows = 40.0f * kArt;
    const float w = tw + arrows + 2.0f * kCap * kArt * 1.0f;
    bar(cx, cy, w, kArt, 255);
    const float inner_right = cx + w * 0.5f - kCap * kArt * 0.85f;
    centred(str, inner_right - arrows - tw * 0.5f - 4.0f * kArt, cy);
    if (!vertical) {
      arrow(kLeftArrow, inner_right - 22.0f * kArt, cy - 1.0f, kArt, 0xFFFFFFFFu);
      arrow(kRightArrow, inner_right - 6.0f * kArt, cy - 1.0f, kArt, 0xFFFFFFFFu);
    } else if (bar_set_) {
      // The strip turned clockwise: its left edge becomes the top.
      const float sw = (kArrowStrip[3] - kArrowStrip[1]) * kArt * zoom, sh = (kArrowStrip[2] - kArrowStrip[0]) * kArt * zoom;
      const float x0 = ox + (inner_right - 14.0f * kArt) * zoom - sw * 0.5f, y0 = oy + (cy - 1.0f) * zoom - sh * 0.5f;
      const float u0 = kArrowStrip[0] / bar_w_, v0 = kArrowStrip[1] / bar_h_, u1 = kArrowStrip[2] / bar_w_, v1 = kArrowStrip[3] / bar_h_;
      const float xy[8] = {x0, y0, x0 + sw, y0, x0, y0 + sh, x0 + sw, y0 + sh};
      const float uv[8] = {u0, v1, u0, v0, u1, v1, u1, v0};
      sprites_->draw_quad(bar_set_, xy, uv);
    }
  };
  option_row(line, kRowY, false);
  if (world_row_) option_row(line2, kRow2Y, true);
  sprites_->end();
}

}  // namespace oc::ui
