#pragma once
// A 1280x960 layout canvas fitted into the view, and a recorded draw list: screens append rects / images / text in
// canvas units, then flush() prepares the glyph atlases, runs one SpriteBatch begin()/end() pair and replays the list.
#include <memory>
#include <string>
#include <vector>

#include "import/text.h"
#include "render/camera.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "ui/dialogue_box.h"
#include "ui/lowres.h"
#include "ui/ps1_font.h"
#include "vfs/vfs.h"

namespace oc::ui {

// Canvas width; kCanvasHeight (960) is in dialogue_box.h.
constexpr float kCanvasW = 1280.0f;

struct Canvas {
  float scale = 1, ox = 0, oy = 0, vw = kCanvasW, vh = kCanvasHeight;
  // The shared 4:3 fit of the canvas into a vw x vh view (render::canvas_fit).
  static Canvas fit(float vw, float vh) {
    Canvas canvas;
    const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasW, kCanvasHeight);
    canvas.scale = fit.s;
    canvas.ox = fit.ox;
    canvas.oy = fit.oy;
    canvas.vw = vw;
    canvas.vh = vh;
    return canvas;
  }
};

class DrawList {
 public:
  explicit DrawList(render::TextBatch& text) : text_(&text) {}
  void rect(render::TexRef white, float x, float y, float w, float h, u32 color) { image(white, x, y, w, h, color); }
  void image(render::TexRef set, float x, float y, float w, float h, u32 color = 0xFFFFFFFFu, float u0 = 0, float v0 = 0,
             float u1 = 1, float v1 = 1) {
    Op op;
    op.set = set;
    op.x = x, op.y = y, op.w = w, op.h = h, op.color = color, op.u0 = u0, op.v0 = v0, op.u1 = u1, op.v1 = v1;
    ops_.push_back(std::move(op));
  }
  // Text with its top-left at (x, y). align: 0 left, 1 centre on x, 2 right edge at x.
  void text(render::FaceId face, std::u32string str, float x, float y, u32 color, float scale, int align = 0,
            bool backdrop = true);
  // Text with a colour helper ink (font::h1..h4: fill, outline / shadow colour, shadow on / off). Same placement rules.
  void text(render::FaceId face, std::u32string str, float x, float y, const font::Ink& ink, float scale, int align = 0);
  float measure(render::FaceId face, std::u32string_view str, float scale) const { return text_->measure(face, str, scale); }
  void clear() { ops_.clear(); }
  // [Video] Ps1ResUi, any art mode: the next flush() replays the list into `lowres`'s target of the PS1 display `display` and
  // scales it up into the canvas rectangle (one request per flush; screens set it while they record).
  void set_lowres(LowRes* lowres, const Ps1Display* display) { lowres_ = lowres, display_ = display; }
  void flush(render::SpriteBatch& sprites, const render::Frame& frame, const Canvas& canvas, bool resume);

 private:
  void replay(render::SpriteBatch& sprites, const render::Frame& frame, const Canvas& canvas, bool resume);
  LowRes* lowres_ = nullptr;
  const Ps1Display* display_ = nullptr;
  enum class Kind { Image, Text };
  struct Op {
    Kind kind = Kind::Image;
    render::TexRef set = {};
    float x = 0, y = 0, w = 0, h = 0, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    u32 color = 0xFFFFFFFFu;
    render::FaceId face = render::kNoFace;
    std::u32string str;
    float scale = 1;
    render::TextStyle style;
  };
  render::TextBatch* text_;
  std::vector<Op> ops_;
};

// Fonts, white texture and samplers shared by the title / name entry / slot screens.
class UiKit {
 public:
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites);
  void shutdown();

  render::TextBatch& text() { return text_; }
  render::TexRef white() const { return white_set_; }
  // PS1-resolution pass of the title / name entry / Continue screens ([Video] Ps1ResUi, any art mode).
  LowRes& lowres() { return lowres_; }
  // The screen's DrawList request for that pass: on when the switch is on (any art mode; the PS1 textures are the screen's own classic-only choice).
  void request_lowres(DrawList& dl, const Ps1Display& display);
  render::SpriteBatch& sprites() { return *sprites_; }
  const vfs::Vfs& vfs() const { return *vfs_; }
  // PS1 glyph table: only the text codec uses it (the PS1 1-bit font is never drawn).
  const import::JpGlyphs& glyphs() const { return *jp_; }

  // Font slots in the current art mode (original art = the small_font_* files, same layout / scale / colour).
  // The screens call set_classic() with their art mode before they record text.
  void set_classic(bool classic) { fonts_.set_classic(classic); }
  bool classic() const { return fonts_.classic(); }
  font::FontSet& fonts() { return fonts_; }
  // bn_70 (slot 4): the default non-JP list font; falls back to gn.
  render::FaceId face_bn() {
    const render::FaceId face_id = fonts_.face(font::kBn);
    return face_id != render::kNoFace ? face_id : face(fonts_.classic() ? ArtMode::Original : ArtMode::Hd);
  }
  // kb_70 (slot 0): the number / JP font; kNoFace when missing.
  render::FaceId face_kb() { return fonts_.face(font::kKb); }
  bool has_bn() { return fonts_.face(font::kBn) != render::kNoFace; }
  // gn_70 (slot 12, the message font) of an art mode: large_ for HD, small_ for original art. `scale` keeps the HD scale in both modes.
  render::FaceId face(ArtMode mode) { return fonts_.face(font::kGn, mode == ArtMode::Original); }
  float scale(ArtMode, float hd_scale, float) const { return hd_scale; }

  // Uploads PNG bytes as a sampled image; returns its set (empty on failure). Freed by shutdown().
  render::TexRef upload_png(ByteSpan png, float* w = nullptr, float* h = nullptr);
  render::TexRef upload_rgba(u32 w, u32 h, const void* rgba, bool linear);
  // Re-uploads the pixels of an image from upload_rgba() (same size) in place (no new texture).
  bool update_rgba(render::TexRef set, const void* rgba);

 private:
  struct Img {
    render::Texture tex;
    render::TexRef set = {};
  };
  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::TextBatch text_;
  LowRes lowres_;
  render::Sampler linear_, nearest_;
  render::Texture white_;
  render::TexRef white_set_ = {};
  std::shared_ptr<import::JpGlyphs> jp_;
  font::FontSet fonts_;
  std::vector<std::unique_ptr<Img>> images_;
};

}  // namespace oc::ui
