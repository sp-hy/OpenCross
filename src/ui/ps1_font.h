#pragma once
// Text in both art modes, the way RDE draws it.
//
// RDE never draws the PS1 glyph rasteriser's output in any language or art mode: every string goes through its own SpriteFont
// text lists. "Original graphics" (config GraphicsMode = 0) only swaps the font files: the loader reads
// data\font\small_font_<face>.spritefont instead of large_font_<face> and draws every glyph one texel narrower. The small_ files
// are pixel fonts (one font pixel = about 4.4 texels, fill / outline ring / drop-shadow masks) - the classic look of the PS1 English
// release; slots, scales and colours are identical in both modes, and so is the layout: the small kb file's glyphs are laid out with
// the large kb file's metrics (pen advance, offsets, measure; the other small files keep their own).
//
// FontSet = the 14 RDE font slots for one render::TextBatch, loaded in the current art mode on first use.
//   ui::font::FontSet fonts;  fonts.init(vfs, text);  fonts.set_classic(original_art);
//   fonts.prepare(font::kBn, s);                                  // outside SpriteBatch begin()/end() (may rebuild an atlas)
//   fonts.draw(font::kBn, s, x, y, font::h1(0x808080, 31, 4), font::slot(font::kBn).scale, font::kLeft, k);   // canvas units
//   float w = fonts.measure(font::kBn, s, scale);                 // DirectXTK MeasureString.x * scale
#include <array>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include "import/spritefont.h"
#include "render/text_batch.h"
#include "vfs/vfs.h"

namespace oc::ui::font {

// RDE font slots. Aliases as loaded: 1, 3, 8, 11 = kt_70; 2, 12 = gn_70; 10, 13 = gn_italic_70.
enum Slot : int {
  kKb = 0,         // kb_70: default JP font, numbers on several screens
  kKtName = 1,     // kt_70: name entry JP style 5
  kBattle = 2,     // gn_70 at 0.425: battle default (non-JP)
  kBattleJp = 3,   // kt_70: battle default (JP)
  kBn = 4,         // bn_70: default non-JP font (menus, shops, labels)
  kCi = 5,         // ci_70: menutest numbers (style 0x80 / 0x82)
  kCur = 6,        // cur_70: the single glyph U+25B6
  kCh = 7,         // ch_100: headline (style 0xA)
  kKt8 = 8,        // kt_70 (unused by the lists)
  kIk = 9,         // ik_70: JP variant of several styles
  kBattleItalic = 10,
  kKtMsg = 11,     // kt_70: JP message windows, PC UI in JP
  kGn = 12,        // gn_70: non-JP message windows
  kGnItalic = 13,  // gn_italic_70: italic message glyphs
  kSlotCount = 14
};

// Font table: {offX, offY, scale, size}. List text is placed at X = x * sx + offX * scale, Y = y * sy + offY * scale;
// message-window glyphs use no offset (scale 0.5).
struct SlotInfo {
  float off_x, off_y, scale;
  int size;
  // File face after aliasing ("bn_70", ...).
  const char* face;
};
const SlotInfo& slot(int slot_index);

// Font archive member of a slot in one art mode: "font/large_font_bn_70.spritefont" / "font/small_font_bn_70.spritefont".
std::string member(int slot_index, bool classic);

// Parsed spritefont, shared process-wide while any FontSet / screen holds it (kb / kt are ~190 MB each). Sets the loader's default
// character ('#' for kb kt bn gn gn_italic ik; none for ci cur ch).
std::shared_ptr<const import::SpriteFont> load(const vfs::Vfs& vfs, const std::string& member);
// Only the glyph metrics and line spacing of a spritefont (no pixels), for laying out another face's glyphs; shared like load().
std::shared_ptr<const import::SpriteFont> load_metrics(const vfs::Vfs& vfs, const std::string& member);

// ---- colours. Colour words are PS1 0x00BBGGRR; 0x80 per channel = 1.0 at k 31.
// outline doubles as the shadow colour where the helper draws a shadow; shadow = false for H2 / H3 / H5;
// additive (H5): both colours are added with their alpha, nothing is darkened.
struct Ink {
  u32 normal = render::pack_rgba(233, 233, 233);
  u32 outline = render::pack_rgba(33, 33, 33);
  bool shadow = true;
  bool additive = false;
  // render::TextStyle::exact.
  bool exact = false;
};
// col * k: per channel min(1, byte * (k & 31) / 3968).
u32 col_k(u32 colour, int brightness_k, u8 alpha = 255);
// c16 (BGR555) (x) col: per channel min(1, c5 * byte / 3968).
u32 c16_col(u32 colour, u32 c16, u8 alpha = 255);
// Colour helpers H1..H4: normal = col * ka (H1 / H2) or c16 (x) col (H3 / H4), outline = col * kb; H1 / H4 draw a shadow.
inline Ink h1(u32 colour, int ka, int kb) { return {col_k(colour, ka), col_k(colour, kb), true, false}; }
inline Ink h2(u32 colour, int ka, int kb) { return {col_k(colour, ka), col_k(colour, kb), false, false}; }
inline Ink h3(u32 colour, u32 c16, int kb) { return {c16_col(colour, c16), col_k(colour, kb), false, false}; }
inline Ink h4(u32 colour, u32 c16, int kb) { return {c16_col(colour, c16), col_k(colour, kb), true, false}; }
// Field message window glyphs: Normal = colour - 0.085, Outline = Shadow = 0.13.
Ink message(float r, float g, float b, float a = 1.0f);

enum Align : int { kLeft = 0, kCentre = 1, kRight = 2 };

class FontSet {
 public:
  FontSet() = default;
  FontSet(const FontSet&) = delete;
  FontSet& operator=(const FontSet&) = delete;

  void init(const vfs::Vfs& vfs, render::TextBatch& text);
  // Art mode: false = HD (large_ files), true = original / classic art (small_ files, glyphs one texel narrower).
  void set_classic(bool classic) { classic_ = classic; }
  bool classic() const { return classic_; }

  // Face of a slot in the current (or the given) mode; loads the file on first use. kNoFace if the file is missing.
  render::FaceId face(int slot_index) { return face(slot_index, classic_); }
  render::FaceId face(int slot_index, bool classic);
  // Makes the glyphs of `text` drawable (not between SpriteBatch begin() and end()).
  void prepare(int slot_index, std::u32string_view text);
  // Reads the slot's font file on a worker thread (the JP faces are ~190 MB); get() / prepare() then take the result instead of reading.
  void prefetch(int slot_index);
  // True when get() / prepare() of the slot will not wait for a prefetch (nothing pending, or the worker is done).
  bool ready(int slot_index);

  // DirectXTK MeasureString(text).x * scale: right edge of the last non-blank glyph, LF starts a new line.
  float measure(int slot_index, std::u32string_view text, float scale);

  // Core draw: pen origin (x, y) in canvas units (1280 x 960), `scale` = slot scale * the list multiplier, align
  // 0 left / 1 centre / 2 right on x (by the measured width). `k` = view pixels per canvas unit. LF advances by lineSpacing.
  // Does not add the slot offX / offY. Between SpriteBatch begin() and end().
  void draw(int slot_index, std::u32string_view text, float x, float y, const Ink& ink, float scale, int align, float k);

 private:
  struct Face {
    render::FaceId id = render::kNoFace;
    std::shared_ptr<const import::SpriteFont> font;
    // render::SpriteFontOptions::layout (the small kb file only).
    std::shared_ptr<const import::SpriteFont> layout;
    bool tried = false;
    std::future<std::shared_ptr<const import::SpriteFont>> pending, pending_layout;
  };
  Face* get(int slot_index, bool classic);
  // The glyph whose metrics place `ch` (the face's layout file when it has the character, else the face's own).
  static const import::SfGlyph* metrics(const Face& face, char32_t ch);
  // Text for the next face build (prepare()).
  std::u32string first_extra_;

  // Files: kb kt gn bn ci cur ch ik gn_italic; aliased slots share one TextBatch face.
  static constexpr int kFiles = 9;
  const vfs::Vfs* vfs_ = nullptr;
  render::TextBatch* text_ = nullptr;
  bool classic_ = false;
  // Indexed [classic * kFiles + file].
  std::array<Face, 2 * kFiles> faces_{};
};

}  // namespace oc::ui::font
