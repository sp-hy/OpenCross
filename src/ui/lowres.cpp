#include "ui/lowres.h"

#include "core/log.h"

namespace oc::ui {

namespace {
bool g_ps1_menus = true;
constexpr float kCanvasWidth = 1280.0f, kCanvasHeightUnits = 960.0f;
}  // namespace

void set_ps1_menus(bool on) { g_ps1_menus = on; }
bool ps1_menus() { return g_ps1_menus; }

bool LowRes::init(render::GpuContext& ctx, render::SpriteBatch& sprites) {
  shutdown();
  if (!nearest_.create(ctx, false)) return false;
  ctx_ = &ctx;
  sprites_ = &sprites;
  return true;
}

void LowRes::shutdown() {
  if (!ctx_) return;
  set_ = sub_set_ = mul_set_ = {};
  rt_.destroy();
  sub_rt_.destroy();
  mul_rt_.destroy();
  capture_.clear();
  nearest_.destroy();
  ctx_ = nullptr;
  sprites_ = nullptr;
  text_ = nullptr;
  active_ = false;
}

// The target is recreated when its size changes and cleared transparent so end() can composite it over the frame.
bool LowRes::begin(const render::Frame& frame, const Ps1Display& display, render::TextBatch* text, render::Frame& out, float canvas_w,
                   float canvas_h) {
  if (!ctx_ || active_) return false;
  if (!rt_.valid() || rt_.width() != display.w || rt_.height() != display.h || !set_) {
    if (!rt_.create(*ctx_, display.w, display.h)) return false;
    set_ = rt_.ref(nearest_);
  }
  static const float kClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ctx_->begin_target(rt_, frame, out, kClear);
  old_filter_ = sprites_->filter();
  sprites_->set_filter(render::SpriteFilter::Point);
  sprites_->set_log_tag(display.tag);
  text_ = text;
  if (text_) text_->set_pixel_grid(static_cast<float>(display.w) / canvas_w, static_cast<float>(display.h) / canvas_h);
  capture_.clear();
  sprites_->set_layer_capture(&capture_);
  active_ = true;
  return true;
}

void LowRes::draw_layer(const render::Frame& frame, render::RenderTarget& layer, render::TexRef& set, render::BlendMode mode,
                        render::BlendMode as, const float (&clear)[4]) {
  if (!layer.valid() || layer.width() != rt_.width() || layer.height() != rt_.height() || !set) {
    set = {};
    if (!layer.create(*ctx_, rt_.width(), rt_.height())) return;
    set = layer.ref(nearest_);
  }
  render::Frame lf;
  ctx_->begin_target(layer, frame, lf, clear);
  const float tw = static_cast<float>(rt_.width()), th = static_cast<float>(rt_.height());
  sprites_->begin(lf, tw, th, true);
  sprites_->replay(capture_, mode, as);
  sprites_->end();
  ctx_->end_target(layer);
}

void LowRes::end(const render::Frame& frame, float vw, float vh, const render::Letterbox::Rect* dst) {
  if (!active_) return;
  active_ = false;
  sprites_->set_layer_capture(nullptr);
  ctx_->end_target(rt_);
  // The window bases: subtracted amounts accumulate on black, multiplied factors on white.
  static const float kBlack[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  static const float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const bool subtract = capture_.has(render::BlendMode::Subtract), multiply = capture_.has(render::BlendMode::Multiply);
  if (subtract) draw_layer(frame, sub_rt_, sub_set_, render::BlendMode::Subtract, render::BlendMode::Add, kBlack);
  if (multiply) draw_layer(frame, mul_rt_, mul_set_, render::BlendMode::Multiply, render::BlendMode::Multiply, kWhite);
  sprites_->set_filter(old_filter_);
  sprites_->set_log_tag(nullptr);
  if (text_) text_->set_pixel_grid(0.0f, 0.0f);
  text_ = nullptr;
  // The canvas rectangle of the view (render::canvas_fit: the 4:3 area centred in the window at the shared zoom), unless given.
  const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasWidth, kCanvasHeightUnits);
  const float zoom = fit.s;
  const float ox = dst ? dst->x : fit.ox, oy = dst ? dst->y : fit.oy;
  const float dw = dst ? dst->w : kCanvasWidth * zoom, dh = dst ? dst->h : kCanvasHeightUnits * zoom;
  const float tw = static_cast<float>(rt_.width()), th = static_cast<float>(rt_.height());
  // Nearest upscale with a 1/64-texel uv bias (the same bias as the field's own upscale): window pixel centres never sit on a texel
  // boundary, where the quad's two triangles could round differently.
  const float bu = 1.0f / (64.0f * tw), bv = 1.0f / (64.0f * th);
  sprites_->begin(frame, vw, vh, true);
  if (multiply && mul_set_) {
    sprites_->set_blend(render::BlendMode::Multiply);
    sprites_->draw(mul_set_, ox, oy, dw, dh, bu, bv, 1.0f + bu, 1.0f + bv);
  }
  if (subtract && sub_set_) {
    sprites_->set_blend(render::BlendMode::Subtract);
    sprites_->draw(sub_set_, ox, oy, dw, dh, bu, bv, 1.0f + bu, 1.0f + bv);
  }
  sprites_->set_blend(render::BlendMode::Premultiplied);
  sprites_->draw(set_, ox, oy, dw, dh, bu, bv, 1.0f + bu, 1.0f + bv);
  sprites_->set_blend(render::BlendMode::Alpha);
  sprites_->end();
}

}  // namespace oc::ui
