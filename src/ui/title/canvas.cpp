#include "ui/title/canvas.h"

#include <cmath>

#include "core/log.h"
#include "render/texture.h"
#include "ui/menu/ps1_art.h"

namespace oc::ui {

void DrawList::text(render::FaceId face, std::u32string str, float x, float y, u32 color, float scale, int align, bool backdrop) {
  if (face == render::kNoFace || str.empty()) return;
  text_->prepare(face, str);
  if (align != 0) {
    const float width = text_->measure(face, str, scale);
    x -= align == 1 ? width * 0.5f : width;
  }
  Op op;
  op.kind = Kind::Text;
  op.face = face;
  op.str = std::move(str);
  op.x = x, op.y = y, op.color = color, op.scale = scale;
  op.style.backdrop = backdrop;
  ops_.push_back(std::move(op));
}

void DrawList::text(render::FaceId face, std::u32string str, float x, float y, const font::Ink& ink, float scale, int align) {
  if (face == render::kNoFace || str.empty()) return;
  text(face, std::move(str), x, y, ink.normal, scale, align, true);
  Op& op = ops_.back();
  op.style.backdrop_color = ink.outline;
  op.style.shadow = ink.shadow;
}

// With a pending low-res request the list is replayed into the PS1-sized target with the canvas itself as the view space, then the
// target is scaled up into the window; otherwise it is replayed straight into the frame.
void DrawList::flush(render::SpriteBatch& sprites, const render::Frame& frame, const Canvas& canvas, bool resume) {
  LowRes* lowres = lowres_;
  const Ps1Display* display = display_;
  lowres_ = nullptr, display_ = nullptr;
  render::Frame target_frame;
  if (lowres && display && canvas.cols > 0.0f) {
    Canvas target;
    target.ox = 4.0f * canvas.cols;
    target.vw = kCanvasW + 2.0f * target.ox;
    Ps1Display widened = *display;
    widened.w = static_cast<u32>(std::lround(static_cast<float>(widened.w) * target.vw / kCanvasW));
    if (lowres->begin(frame, widened, text_, target_frame, target.vw, kCanvasHeight)) {
      replay(sprites, target_frame, target, resume);
      const render::Letterbox letterbox = render::Letterbox::wide(canvas.vw, canvas.vh, canvas.cols);
      const render::Letterbox::Rect target_rect{letterbox.left(), letterbox.oy, letterbox.width(), render::kLayerH * letterbox.scale};
      lowres->end(frame, canvas.vw, canvas.vh, &target_rect);
      return;
    }
  } else if (lowres && display && lowres->begin(frame, *display, text_, target_frame)) {
    replay(sprites, target_frame, Canvas{}, resume);
    lowres->end(frame, canvas.vw, canvas.vh);
    return;
  }
  replay(sprites, frame, canvas, resume);
}

void UiKit::request_lowres(DrawList& dl, const Ps1Display& display) {
  dl.set_lowres(ps1_menus() && lowres_.ready() ? &lowres_ : nullptr, &display);
}

void DrawList::replay(render::SpriteBatch& sprites, const render::Frame& frame, const Canvas& canvas, bool resume) {
  if (canvas.clip) frame.set_scissor(ps1::picture_clip(frame, canvas.vw, canvas.vh, kCanvasW, kCanvasHeight));
  sprites.begin(frame, canvas.vw, canvas.vh, resume);
  for (const Op& op : ops_) {
    switch (op.kind) {
      case Kind::Image:
        if (op.set)
          sprites.draw(op.set, canvas.ox + op.x * canvas.scale, canvas.oy + op.y * canvas.scale, op.w * canvas.scale, op.h * canvas.scale, op.u0,
                       op.v0, op.u1, op.v1, op.color);
        break;
      case Kind::Text:
        text_->draw(op.face, op.str, canvas.ox + op.x * canvas.scale, canvas.oy + op.y * canvas.scale, op.color, op.scale * canvas.scale, op.style);
        break;
    }
  }
  sprites.end();
  if (canvas.clip) frame.set_scissor(frame.full());
  ops_.clear();
}

bool UiKit::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites) {
  ctx_ = &ctx;
  vfs_ = &vfs;
  sprites_ = &sprites;
  if (!linear_.create(ctx, true) || !nearest_.create(ctx, false)) return false;
  const u8 white[4] = {255, 255, 255, 255};
  if (!white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, white)) return false;
  white_set_ = render::TexRef(white_, nearest_);
  lowres_.init(ctx, sprites);
  if (!text_.init(ctx, sprites)) return false;
  // The JP glyph table backs the name codec; the PS1 font face (disc mode) is the FontSet's: the title menus draw the disc's menu font.
  jp_ = std::make_shared<import::JpGlyphs>();
  if (auto exe = vfs.read("cdrom/prog/slps_023.64"); !exe || !jp_->load_bitmaps(*exe)) log::warn("ui: PS1 font bitmaps unavailable");
  fonts_.init(vfs, text_, true);
  if (fonts_.face(font::kGn, false) == render::kNoFace) {
    log::error("ui: no usable font (font/large_font_gn_70.spritefont)");
    return false;
  }
  return static_cast<bool>(white_set_);
}

void UiKit::shutdown() {
  images_.clear();
  white_set_ = {};
  lowres_.shutdown();
  text_.destroy();
  // Drop the face ids (the TextBatch is gone).
  fonts_.reset();
  white_.destroy();
  linear_.destroy();
  nearest_.destroy();
  ctx_ = nullptr;
}

render::TexRef UiKit::upload_rgba(u32 w, u32 h, const void* rgba, bool linear) {
  auto img = std::make_unique<Img>();
  if (!img->tex.create(*ctx_, w, h, render::TextureFormat::Rgba8, rgba)) return {};
  img->set = render::TexRef(img->tex, linear ? linear_ : nearest_);
  if (!img->set) return {};
  const render::TexRef set = img->set;
  images_.push_back(std::move(img));
  return set;
}

bool UiKit::update_rgba(render::TexRef set, const void* rgba) {
  for (auto& img : images_)
    if (img->set == set) return img->tex.update(*ctx_, rgba);
  return false;
}

render::TexRef UiKit::upload_png(ByteSpan png, float* w, float* h) {
  render::DecodedImage decoded;
  if (!render::decode_image(png, decoded)) return {};
  if (w) *w = static_cast<float>(decoded.width);
  if (h) *h = static_cast<float>(decoded.height);
  return upload_rgba(decoded.width, decoded.height, decoded.rgba.data(), true);
}

}  // namespace oc::ui
