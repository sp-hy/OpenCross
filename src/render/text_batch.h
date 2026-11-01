#pragma once
// Text drawing on top of SpriteBatch: DirectXTK spritefont faces (RDE HD fonts) and the PS1 1-bit bitmap font.
//
// Each face owns one RGBA8 glyph atlas generated from the requested glyphs (the source spritefonts are up to 190 MB,
// so they are never uploaded whole). RDE spritefonts pack masks in the colour channels: R = glyph fill,
// B = fill + outline + shadow; the fill is drawn in the text colour and the B mask in a dark backdrop colour.
// The PS1 bitmap font is drawn with nearest sampling; its backdrop is a one-pixel drop shadow.
//
// Call prepare() for every string before the frame it appears in (it may rebuild an atlas);
// draw calls between SpriteBatch::begin() and end() never touch the GPU resources.
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "import/spritefont.h"
#include "import/text.h"
#include "render/sprite_batch.h"

namespace oc::render {

using FaceId = int;
constexpr FaceId kNoFace = -1;

struct TextStyle {
  // Outline/shadow behind the glyph.
  bool backdrop = true;
  // Alpha is replaced by the text colour's alpha.
  u32 backdrop_color = pack_rgba(34, 34, 34, 255);
  // false: the backdrop is the outline ring only (RDE colour helpers H2 / H3 / H5 pass Shadow = 0, text_core 3.1). Needs a face
  // added with SpriteFontOptions::outline_mask; other faces keep the fill + outline + shadow mask.
  bool shadow = true;
};

struct SpriteFontOptions {
  // RDE's original-graphics rule for the small_font_* files (text_core 1.1, glyph lambda 0x593CC0: with GraphicsMode == 0 every
  // glyph source rect is drawn one texel narrower, subrect.right - 1; advance and measure use the full rect).
  bool narrow = false;
  // Also build the G (fill + outline) mask atlas for TextStyle::shadow == false.
  bool outline_mask = false;
  // Pixel fonts (the small_font_* files): source texels per font pixel (70 / 16 = 4.375 for the 70-size faces), 0 = off. Glyphs whose
  // masks are blocks of that pitch are decoded to one texel per font pixel and drawn with nearest sampling, every font pixel as a
  // whole number of screen pixels (round(pitch * scale), at least 1), the glyph centred on its exact RDE box and the line's glyphs
  // sharing one baseline. Pen advance and measure are unchanged (RDE's float layout). Other glyphs draw the source rect (linear).
  float pixel_pitch = 0.0f;
};

class TextBatch {
 public:
  TextBatch() = default;
  ~TextBatch() { destroy(); }
  TextBatch(const TextBatch&) = delete;
  TextBatch& operator=(const TextBatch&) = delete;

  bool init(GpuContext& ctx, SpriteBatch& sprites);
  void destroy();

  // Spritefont face. `scale` in draw calls multiplies the font's own pixel size (RDE draws its 70 pt faces at 0.5).
  // `extra` joins the first atlas build (ASCII + extra in one rebuild instead of two).
  FaceId add_spritefont(std::shared_ptr<const import::SpriteFont> font, const SpriteFontOptions& opt = {}, std::u32string_view extra = {});
  // PS1 bitmap font (needs JpGlyphs::load_bitmaps). `scale` is a pixel multiplier (2 = 22 px glyphs).
  FaceId add_bitmap_font(std::shared_ptr<const import::JpGlyphs> glyphs);

  // Makes sure every code point of `text` has an atlas entry. Not callable between begin() and end().
  void prepare(FaceId face, std::u32string_view text);

  // Pen advance of one glyph, line pitch, and the widest line of a LF separated string (pixels at `scale`).
  float advance(FaceId face, char32_t ch, float scale) const;
  // The sprite font's measure of one character (import::SpriteFont::measure); 0 for a bitmap face.
  float glyph_measure(FaceId face, char32_t ch) const;
  float line_height(FaceId face, float scale) const;
  float measure(FaceId face, std::u32string_view text, float scale) const;

  // Pixel grid of the target being drawn into: target pixels per view unit on x / y (0 = view units are pixels, the default). Set by
  // ui::LowRes while a screen draws into its PS1-resolution target: pixel-font glyphs whose font pixel is
  // within 10 % of a whole number of target pixels are drawn nearest on that grid, all others (RDE's menu scales: 0.55 .. 0.9 target
  // px per font pixel) at RDE's exact size through the box (area) filter; every line's baseline on a target row.
  void set_pixel_grid(float gx, float gy) { grid_x_ = gx, grid_y_ = gy; }
  bool pixel_grid() const { return grid_x_ > 0 && grid_y_ > 0; }
  SpriteBatch* sprites() const { return sprites_; }

  enum class Pass { Backdrop, Fill };
  // One glyph at pen position (x, y = top of the line box); move the pen with advance().
  void draw_glyph(FaceId face, char32_t ch, float x, float y, u32 color, float scale, Pass pass,
                  const TextStyle& style = {});
  // A LF separated string (backdrop pass, then fill pass).
  void draw(FaceId face, std::u32string_view text, float x, float y, u32 color, float scale,
            const TextStyle& style = {});

  // ---- Layout audit (debug aid; on when the environment has OC_TEXT_AUDIT=1, or by audit_enable). Records the filled glyph
  // boxes of every string ("run": one draw() call, or the glyphs after audit_run()) in view pixels and reports
  //   overlap  - a glyph of one string covers a glyph of another string,
  //   missing  - a character the face has no glyph for (drawn as the face's default character, '#'),
  //   edge     - a glyph box crosses the edge of a registered obstacle (audit_obstacle: frames, dots),
  //   offscreen- a glyph outside the view.
  // Frames are separated by audit_report() or, when nobody calls it (the game), by a pause of 6 ms between glyphs, the frame's first
  // glyph drawn again at its spot or the frame's first string drawn again; then every new issue is logged once ("text-audit: ...").
  // Strings starting within 4 px of each other are not compared (overprints, or two frames of a changing screen that ran together at
  // an uncapped frame rate). Costs nothing when off.
  void audit_enable(bool on);
  bool audit_enabled() const { return audit_on_; }
  void audit_run();
  void audit_obstacle(float x, float y, float w, float h, std::string_view label);
  void audit_view(float w, float h) { audit_vw_ = w, audit_vh_ = h; }
  void audit_clear();
  // Logs every issue of the recorded frame (repeats included) under `tag` and a one-line summary; returns the issue count. From the
  // first call on, the caller frames the recording with audit_clear() / audit_report() (the automatic split stops).
  int audit_report(std::string_view tag);

 private:
  struct AuditGlyph {
    float x0, y0, x1, y1;
    char32_t ch;
  };
  struct AuditRun {
    std::u32string text;
    std::vector<AuditGlyph> glyphs;
    int missing = 0;
  };
  struct AuditObstacle {
    float x0, y0, x1, y1;
    std::string label;
  };
  void audit_glyph(const import::SpriteFont* font, char32_t ch, float x0, float y0, float x1, float y1);
  void audit_auto_frame();
  void audit_flush_auto();
  std::vector<std::string> audit_issues() const;
  bool audit_on_ = false;
  // OC_TEXT_AUDIT=2: also log a sample frame every 300 frames.
  bool audit_verbose_ = false;
  int audit_frames_ = 0;
  // audit_report() is in use: no automatic frame split.
  bool audit_manual_ = false;
  std::vector<AuditRun> audit_runs_;
  std::vector<AuditObstacle> audit_obstacles_;
  bool audit_new_run_ = true;
  float audit_vw_ = 0, audit_vh_ = 0;
  // First glyph of the recorded frame (automatic frame split).
  char32_t audit_first_ch_ = 0;
  float audit_first_x_ = 0, audit_first_y_ = 0;
  // Seconds of the last recorded glyph (automatic frame split).
  double audit_last_ = 0;
  // Issues already logged by the automatic mode.
  std::vector<std::string> audit_seen_;

 private:
  struct Entry {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    // Glyph size in source pixels.
    float w = 0, h = 0;
    float off_x = 0, off_y = 0;
    // Total pen advance in source pixels.
    float adv = 0;
    bool has_quad = false;
    // Pixel-grid decode (SpriteFontOptions::pixel_pitch): nx x ny font pixels at (pu0, pv0)..(pu1, pv1) of the atlas.
    bool px = false;
    int nx = 0, ny = 0;
    // Font pixels from the glyph's top edge to the face baseline.
    int up = 0;
    float pu0 = 0, pv0 = 0, pu1 = 0, pv1 = 0;
  };
  struct Atlas {
    Texture tex;
    TexRef set;
    // Same texture, nearest sampling (pixel-grid glyphs).
    TexRef set_nearest;
  };
  struct Face {
    bool bitmap = false;
    SpriteFontOptions opt;
    std::shared_ptr<const import::SpriteFont> font;
    std::shared_ptr<const import::JpGlyphs> jp;
    std::vector<char32_t> wanted;
    std::unordered_map<char32_t, Entry> entries;
    std::unique_ptr<Atlas> fill;
    // Spritefont only: B mask (fill + outline + shadow).
    std::unique_ptr<Atlas> back;
    // Spritefont with opt.outline_mask: G mask (fill + outline).
    std::unique_ptr<Atlas> outline;
    // Pixel fonts: baseline in font pixels below the pen line (from 'A'), 0 = per glyph.
    float baseline = 0.0f;
    bool baseline_set = false;
  };

  Face* face(FaceId id) {
    return id >= 0 && id < static_cast<int>(faces_.size()) ? &faces_[static_cast<size_t>(id)] : nullptr;
  }
  const Face* face(FaceId id) const {
    return id >= 0 && id < static_cast<int>(faces_.size()) ? &faces_[static_cast<size_t>(id)] : nullptr;
  }
  const Entry* entry(const Face& font_face, char32_t ch) const;
  bool rebuild_spritefont(Face& font_face);
  bool build_bitmap_atlas(Face& font_face);
  std::unique_ptr<Atlas> make_atlas(u32 w, u32 h, const std::vector<u8>& rgba, const Sampler& sampler, bool with_nearest = false, UploadBatch* batch = nullptr);

  GpuContext* ctx_ = nullptr;
  SpriteBatch* sprites_ = nullptr;
  float grid_x_ = 0, grid_y_ = 0;
  Sampler linear_, nearest_;
  std::vector<Face> faces_;
};

}  // namespace oc::render
