#include "ui/pause_screen.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "import/ps1_font.h"
#include "render/camera.h"
#include "ui/menu/ps1_art.h"

namespace oc::ui {

namespace {
constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
// ui/pause.png (170 x 83): the slanted bar (outline, fill, drop shadow) in y 0..59, a left and a right arrow below it. kCap = the
// source columns kept unstretched at each end of the bar; the arrows are x0, y0, x1, y1 in source pixels. The outline is rows 0..55.
constexpr float kSrcW = 170.0f, kBarH = 60.0f, kCap = 38.0f, kOutlineMid = 27.5f;
constexpr float kLeftArrow[4] = {1, 61, 13, 83}, kRightArrow[4] = {18, 61, 31, 83};
// The pair of arrows as one strip; the World Names row draws it turned a quarter clockwise (left -> up, right -> down).
constexpr float kArrowStrip[4] = {0, 61, 31, 83};
// Canvas units (pixels of a 960-high picture) per art pixel: the bars at the picture height / 840, the arrows at height / 1080.
constexpr float kArt = 960.0f / 840.0f, kArrowArt = 960.0f / 1080.0f;
// Middles of the bar outlines: PAUSE, the Vibration row and the World Names row.
constexpr float kTitleMid = 405.5f, kRowMid = 512.5f, kRow2Mid = 603.5f;
// Every bar is sized to its text: the pen starts kTextLeft right of the bar's left edge; the title bar ends the face's title_pad
// after the text's pen end; an option row has the arrow pair the face's arrow_gap after it and ends kArrowRight after the pair. The
// arrows' middle sits kArrowDy below the outline's middle, on the text's baseline; the text's capital P is centred kTextDy below it.
constexpr float kTextLeft = 27.0f, kArrowRight = 8.1f, kArrowDy = 15.5f, kTextDy = 1.0f;
}  // namespace

bool PauseScreen::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text) {
  ctx_ = &ctx, vfs_ = &vfs, sprites_ = &sprites, text_ = &text;
  if (!linear_.create(ctx, true) || !nearest_.create(ctx, false)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, px)) return false;
  white_set_ = render::TexRef(white_, nearest_);
  lt_.init(&vfs);
  if (!vfs.has_archive("ui")) {
    ps1_ = true;
    if (auto font = import::load_ps1_font(vfs)) face_ = text.add_ps1_font(std::move(font));
    const ps1::Rgba wind = ps1::menutim(vfs, "wind");
    if (wind && wind_.create(ctx, wind.w, wind.h, render::TextureFormat::Rgba8, wind.px.data())) {
      wind_set_ = render::TexRef(wind_, nearest_);
      wind_w_ = static_cast<float>(wind.w), wind_h_ = static_cast<float>(wind.h);
    }
    if (face_ == render::kNoFace || !wind_set_) log::warn("pause: PS1 font or menu window art unavailable");
    return true;
  }
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
  if (!make_face(latin_, "ui/ui_font_gnb.spritefont", U"PAUSE", 0.6f, 10.2f, 38.5f)) log::warn("pause: ui/ui_font_gnb.spritefont unavailable");
  return true;
}

void PauseScreen::shutdown() {
  if (!ctx_) return;
  bar_set_ = {};
  white_set_ = {};
  wind_set_ = {};
  bar_.destroy();
  white_.destroy();
  wind_.destroy();
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

std::u32string PauseScreen::ui_text(int id) {
  static constexpr const char32_t* kWords[2][5] = {{U"PAUSE", U"Vibration Function", U"World Names", U"Off", U"On"},
                                                   {U"PAUSE", U"バイブレーション機能", U"ワールド名表示", U"OFF", U"ON"}};
  if (const import::TextTable* table = lt_.table("ui_pausemenu"))
    if (const std::string* str = table->get(static_cast<u64>(id), lang_); str && !str->empty()) return import::utf8_to_u32(*str);
  return kWords[lang_ == import::Lang::Jp ? 1 : 0][id];
}

bool PauseScreen::make_face(Face& face, const std::string& member, std::u32string_view extra, float spacing, float arrow_gap,
                            float title_pad) {
  face.spacing = spacing, face.arrow_gap = arrow_gap, face.title_pad = title_pad;
  face.font = font::load(*vfs_, member);
  if (!face.font) return false;
  face.id = text_->add_spritefont(face.font, {}, extra);
  if (const import::SfGlyph* glyph = face.font->find('P'); glyph && glyph->height() > 0) {
    face.scale = 0.55f * kBarH * kArt / (static_cast<float>(glyph->height()) * 1.15f);
    face.ink_mid = glyph->y_offset + static_cast<float>(glyph->height()) * 0.5f;
  }
  return face.id != render::kNoFace;
}

const PauseScreen::Face& PauseScreen::face() {
  if (lang_ != import::Lang::Jp) return latin_;
  if (!jp_tried_) {
    jp_tried_ = true;
    if (!make_face(jp_, font::member(font::kKb, false), U"PAUSE", 2.3f, 30.6f, 48.3f)) log::warn("pause: kb_70 unavailable; Japanese rows in ui_font_gnb");
  }
  return jp_.id != render::kNoFace ? jp_ : latin_;
}

// Every bar is sized to its text; an option row has the arrows tucked inside its right end (only the middle slice of the art
// stretches). The purple ramp over the frozen picture is the full-screen shader pass's ramp mode, applied by the host before this draw.
void PauseScreen::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  if (!open_ || !ctx_) return;
  const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
  const float zoom = fit.s, ox = fit.ox, oy = fit.oy;
  const std::u32string colon = lang_ == import::Lang::Jp ? U"\uFF1A" : U" : ";
  const std::u32string title = ui_text(0);
  const std::u32string line = ui_text(1) + colon + ui_text(vibration_ ? 4 : 3);
  const std::u32string line2 = ui_text(2) + colon + ui_text(world_names_ ? 4 : 3);
  if (ps1_) {
    draw_ps1(frame, vw, vh, resume, title, line, line2);
    return;
  }
  const Face& font_face = face();
  if (font_face.id != render::kNoFace) {
    text_->prepare(font_face.id, title);
    text_->prepare(font_face.id, line);
    if (world_row_) text_->prepare(font_face.id, line2);
  }

  sprites_->begin(frame, vw, vh, resume);

  // A slanted bar with its outline's middle at `mid` (canvas units), centred across the canvas, total width `w` (end caps unstretched).
  const float cx = kCanvasW * 0.5f;
  auto bar = [&](float mid, float w) {
    const float x = ox + (cx - w * 0.5f) * zoom, y = oy + (mid - kOutlineMid * kArt) * zoom;
    const float h = kBarH * kArt * zoom, cap = kCap * kArt * zoom, width = w * zoom;
    if (!bar_set_) {
      sprites_->draw(white_set_, x, y, width, h, 0, 0, 1, 1, 0xFFFFFFFFu);
      return;
    }
    const float v1 = kBarH / bar_h_;
    sprites_->draw(bar_set_, x, y, cap, h, 0, 0, kCap / bar_w_, v1, 0xFFFFFFFFu);
    sprites_->draw(bar_set_, x + cap, y, width - 2 * cap, h, kCap / bar_w_, 0, (kSrcW - kCap) / bar_w_, v1, 0xFFFFFFFFu);
    sprites_->draw(bar_set_, x + width - cap, y, cap, h, (kSrcW - kCap) / bar_w_, 0, 1.0f, v1, 0xFFFFFFFFu);
  };
  // One arrow of the strip whose left edge is at canvas x0, its middle at canvas y `mid`.
  auto arrow = [&](const float (&src)[4], float x0, float mid) {
    if (!bar_set_) return;
    const float w = (src[2] - src[0]) * kArrowArt * zoom, h = (src[3] - src[1]) * kArrowArt * zoom;
    sprites_->draw(bar_set_, ox + (x0 + (src[0] - kArrowStrip[0]) * kArrowArt) * zoom, oy + mid * zoom - h * 0.5f, w, h, src[0] / bar_w_,
                   src[1] / bar_h_, src[2] / bar_w_, src[3] / bar_h_, 0xFFFFFFFFu);
  };
  render::TextStyle style;
  style.backdrop_color = render::pack_rgba(30, 30, 40, 255);
  const float text_scale = font_face.scale * zoom;
  // The pen's travel over `str`: every glyph's advance plus the face's spacing.
  auto text_w = [&](const std::u32string& str) {
    float width = 0.0f;
    if (font_face.id != render::kNoFace)
      for (const char32_t ch : str) width += text_->advance(font_face.id, ch, text_scale) / zoom + font_face.spacing;
    return width;
  };
  auto text_at = [&](const std::u32string& str, float left, float mid) {
    if (font_face.id == render::kNoFace) return;
    const float y = oy + (mid + kTextDy) * zoom - font_face.ink_mid * text_scale;
    for (const render::TextBatch::Pass pass : {render::TextBatch::Pass::Backdrop, render::TextBatch::Pass::Fill}) {
      float pen = left;
      for (const char32_t ch : str) {
        text_->draw_glyph(font_face.id, ch, ox + pen * zoom, y, 0xFFFFFFFFu, text_scale, pass, style);
        pen += text_->advance(font_face.id, ch, text_scale) / zoom + font_face.spacing;
      }
    }
  };

  const float title_w = kTextLeft + text_w(title) + font_face.title_pad;
  bar(kTitleMid, title_w);
  text_at(title, cx - title_w * 0.5f + kTextLeft, kTitleMid);
  // An option row: the text, then the arrow pair, left / right or (World Names) turned to up / down.
  const float pair_w = (kArrowStrip[2] - kArrowStrip[0]) * kArrowArt;
  auto option_row = [&](const std::u32string& str, float mid, bool vertical) {
    const float tw = text_w(str), w = kTextLeft + tw + font_face.arrow_gap + pair_w + kArrowRight;
    const float left = cx - w * 0.5f, pair_x = left + kTextLeft + tw + font_face.arrow_gap, arrows_mid = mid + kArrowDy;
    bar(mid, w);
    text_at(str, left + kTextLeft, mid);
    if (!vertical) {
      arrow(kLeftArrow, pair_x, arrows_mid);
      arrow(kRightArrow, pair_x, arrows_mid);
    } else if (bar_set_) {
      // The strip turned clockwise about the pair's middle: its left edge becomes the top.
      const float sw = (kArrowStrip[3] - kArrowStrip[1]) * kArrowArt * zoom, sh = pair_w * zoom;
      const float x0 = ox + (pair_x + pair_w * 0.5f) * zoom - sw * 0.5f, y0 = oy + arrows_mid * zoom - sh * 0.5f;
      const float u0 = kArrowStrip[0] / bar_w_, v0 = kArrowStrip[1] / bar_h_, u1 = kArrowStrip[2] / bar_w_, v1 = kArrowStrip[3] / bar_h_;
      const float xy[8] = {x0, y0, x0 + sw, y0, x0, y0 + sh, x0 + sw, y0 + sh};
      const float uv[8] = {u0, v1, u0, v0, u1, v1, u1, v0};
      sprites_->draw_quad(bar_set_, xy, uv);
    }
  };
  option_row(line, kRowMid, false);
  if (world_row_) option_row(line2, kRow2Mid, true);
  sprites_->end();
}

// The disc look, laid out in pixels of the 320 x 240 picture (4 canvas units each): the title line, a 6-pixel gap and the option
// rows at the font's 13-pixel pitch, every line centred, inside a window 14 pixels wider than the widest line on each side and
// 8 pixels taller above and below, its frame the menu window pieces and its inside a translucent black.
void PauseScreen::draw_ps1(const render::Frame& frame, float vw, float vh, bool resume, const std::u32string& title,
                           const std::u32string& line, const std::u32string& line2) {
  constexpr float kPx = 4.0f, kPadX = 14.0f, kPadY = 8.0f, kGap = 6.0f, kPitch = 13.0f, kScale = 0.5f;
  if (face_ == render::kNoFace) return;
  const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
  const float zoom = fit.s, scale = kScale * zoom;
  text_->prepare(face_, title);
  text_->prepare(face_, line);
  if (world_row_) text_->prepare(face_, line2);
  auto width_px = [&](const std::u32string& str) { return std::ceil(text_->measure(face_, str, scale) / (kPx * zoom)); };
  const float text_w = std::max({width_px(title), width_px(line), world_row_ ? width_px(line2) : 0.0f});
  const float win_w = text_w + 2.0f * kPadX, win_h = 2.0f * kPadY + kGap + kPitch * (world_row_ ? 3.0f : 2.0f);
  const float left = std::floor((kCanvasW / kPx - win_w) * 0.5f), top = std::floor((kCanvasH / kPx - win_h) * 0.5f);
  auto vx = [&](float px) { return fit.ox + px * kPx * zoom; };
  auto vy = [&](float px) { return fit.oy + px * kPx * zoom; };

  sprites_->begin(frame, vw, vh, resume);
  sprites_->draw(white_set_, vx(left + 2.0f), vy(top + 2.0f), (win_w - 4.0f) * kPx * zoom, (win_h - 4.0f) * kPx * zoom, 0, 0, 1, 1,
                 render::pack_rgba(0, 0, 0, 160));
  if (wind_set_)
    ps1::frame_pieces(
        [&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) {
          sprites_->draw(wind_set_, dx, dy, dw, dh, sx / wind_w_, sy / wind_h_, (sx + sw) / wind_w_, (sy + sh) / wind_h_);
        },
        vx(left), vy(top), win_w * kPx * zoom, win_h * kPx * zoom, kPx * zoom, kPx * zoom);
  render::TextStyle style;
  style.backdrop_color = render::pack_rgba(0, 0, 0, 255);
  auto centred = [&](const std::u32string& str, float row_top) {
    text_->draw(face_, str, vx(left + std::floor((win_w - width_px(str)) * 0.5f)), vy(row_top), render::pack_rgba(255, 255, 255), scale,
                style);
  };
  centred(title, top + kPadY);
  centred(line, top + kPadY + kPitch + kGap);
  if (world_row_) centred(line2, top + kPadY + 2.0f * kPitch + kGap);
  sprites_->end();
}

}  // namespace oc::ui
