#include "ui/ps1_font.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "core/log.h"
#include "import/ps1_font.h"

namespace oc::ui::font {

namespace {

// File faces (RDE's load order) and the default character the loader sets for each.
struct File {
  const char* face;
  u32 default_char;
};
constexpr File kFileTable[9] = {{"kb_70", '#'},  {"kt_70", '#'},  {"gn_70", '#'},   {"bn_70", '#'},          {"ci_70", 0},
                            {"cur_70", 0},   {"ch_100", 0},   {"ik_70", '#'},   {"gn_italic_70", '#'}};
// Slot -> file (aliases: 1 3 8 11 = kt, 2 12 = gn, 10 13 = gn_italic).
constexpr int kSlotFile[kSlotCount] = {0, 1, 2, 1, 3, 4, 5, 6, 1, 7, 8, 1, 2, 8};

// RDE's font table: {offX, offY, scale, size, face}.
constexpr SlotInfo kSlots[kSlotCount] = {
    {-80.0f / 7.0f, -180.0f / 7.0f, 0.5f, 70, "kb_70"},
    {-16.333334f, -25.666666f, 3.0f / 7.0f, 70, "kt_70"},
    {-5.7142859f, -8.5714283f, 0.425f, 70, "gn_70"},
    {-16.666666f, -37.5f, 0.34285715f, 70, "kt_70"},
    {-80.0f / 7.0f, -120.0f / 7.0f, 0.5f, 70, "bn_70"},
    {0, 0, 0.6f, 70, "ci_70"},
    {0, 0, 0.6f, 70, "cur_70"},
    {0, 0, 1.0f, 100, "ch_100"},
    {0, 0, 0.5f, 70, "kt_70"},
    {-80.0f / 7.0f, -120.0f / 7.0f, 0.5f, 70, "ik_70"},
    {-5.7142859f, -8.5714283f, 0.425f, 70, "gn_italic_70"},
    {0, 0, 0.5f, 70, "kt_70"},
    {-2.857143f, -4.285714f, 0.5f, 70, "gn_70"},
    {-2.857143f, -4.285714f, 0.5f, 70, "gn_italic_70"}};

u8 to8(float v) { return static_cast<u8>(std::clamp(std::lround(v * 255.0f), 0L, 255L)); }

bool is_blank(char32_t ch) { return ch == U' ' || ch == U'\t' || ch == 0x3000 || ch == 0xA0; }

}  // namespace

const SlotInfo& slot(int slot_index) { return kSlots[std::clamp(slot_index, 0, kSlotCount - 1)]; }

std::string member(int slot_index, bool classic) {
  return std::string("font/") + (classic ? "small" : "large") + "_font_" + kFileTable[kSlotFile[std::clamp(slot_index, 0, kSlotCount - 1)]].face +
         ".spritefont";
}

std::shared_ptr<const import::SpriteFont> load(const vfs::Vfs& vfs, const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, std::weak_ptr<const import::SpriteFont>> cache;
  std::lock_guard<std::mutex> lock(mu);
  if (auto it = cache.find(name); it != cache.end())
    if (auto cached = it->second.lock()) return cached;
  auto raw = vfs.read(name);
  if (!raw) {
    log::warn("font: {} not found", name);
    return nullptr;
  }
  std::string err;
  auto parsed = import::parse_spritefont(std::move(*raw), &err);
  if (!parsed) {
    log::warn("font: {}: {}", name, err);
    return nullptr;
  }
  for (const File& file : kFileTable)
    if (name.find(std::string("_font_") + file.face + ".") != std::string::npos) parsed->default_char = file.default_char;
  auto font = std::make_shared<const import::SpriteFont>(std::move(*parsed));
  cache[name] = font;
  return font;
}

std::shared_ptr<const import::SpriteFont> load_metrics(const vfs::Vfs& vfs, const std::string& name) {
  static std::mutex mu;
  static std::map<std::string, std::weak_ptr<const import::SpriteFont>> cache;
  std::lock_guard<std::mutex> lock(mu);
  if (auto it = cache.find(name); it != cache.end())
    if (auto cached = it->second.lock()) return cached;
  auto raw = vfs.read(name);
  if (!raw) {
    log::warn("font: {} not found", name);
    return nullptr;
  }
  std::string err;
  auto parsed = import::parse_spritefont(std::move(*raw), &err);
  if (!parsed) {
    log::warn("font: {}: {}", name, err);
    return nullptr;
  }
  auto metrics = std::make_shared<import::SpriteFont>();
  metrics->glyphs = std::move(parsed->glyphs);
  metrics->line_spacing = parsed->line_spacing;
  std::shared_ptr<const import::SpriteFont> font = std::move(metrics);
  cache[name] = font;
  return font;
}

u32 col_k(u32 colour, int brightness_k, u8 alpha) {
  const float factor = static_cast<float>(brightness_k & 31) / 3968.0f;
  auto channel = [&](int shift) { return to8(std::min(1.0f, static_cast<float>((colour >> shift) & 0xFF) * factor)); };
  return render::pack_rgba(channel(0), channel(8), channel(16), alpha);
}

u32 c16_col(u32 colour, u32 c16, u8 alpha) {
  auto channel = [&](int shift, int shift16) {
    return to8(std::min(1.0f, static_cast<float>((c16 >> shift16) & 31) * static_cast<float>((colour >> shift) & 0xFF) / 3968.0f));
  };
  return render::pack_rgba(channel(0, 0), channel(8, 5), channel(16, 10), alpha);
}

Ink message(float r, float g, float b, float a) {
  return {render::pack_rgba(to8(r - 0.085f), to8(g - 0.085f), to8(b - 0.085f), to8(a)), render::pack_rgba(33, 33, 33, to8(a)), true,
          false};
}

void FontSet::init(const vfs::Vfs& vfs, render::TextBatch& text) {
  vfs_ = &vfs;
  text_ = &text;
  faces_ = {};
  ps1_ = {};
  if (vfs.has_archive("font")) return;
  ps1_.tried = true;
  if (auto font = import::load_ps1_font(vfs)) ps1_.id = text.add_ps1_font(std::move(font));
  if (ps1_.id == render::kNoFace) log::warn("font: no spritefonts and no PS1 font");
}

const import::SfGlyph* FontSet::metrics(const Face& face, char32_t ch) {
  if (face.layout)
    if (const import::SfGlyph* glyph = face.layout->find(static_cast<u32>(ch))) return glyph;
  return face.font->find_or_default(static_cast<u32>(ch));
}

FontSet::Face* FontSet::get(int slot_index, bool classic) {
  if (slot_index < 0 || slot_index >= kSlotCount || !vfs_ || !text_) return nullptr;
  if (ps1()) return &ps1_;
  const int file = kSlotFile[slot_index];
  Face& face = faces_[static_cast<size_t>((classic ? kFiles : 0) + file)];
  if (!face.tried) {
    face.tried = true;
    if (face.pending.valid()) face.font = face.pending.get();
    else face.font = load(*vfs_, member(slot_index, classic));
    if (classic && file == kSlotFile[kKb])
      face.layout = face.pending_layout.valid() ? face.pending_layout.get() : load_metrics(*vfs_, member(kKb, false));
    if (face.font) {
      render::SpriteFontOptions opt;
      opt.narrow = classic;
      opt.layout = face.layout;
      opt.outline_mask = true;
      // small_ files: pixel fonts with 16 font pixels per point size (70 / 16 = 4.375 texels). ch_100 / cur_70 are the HD files in
      // both sets and fail the per-glyph grid test, so they keep RDE's linear drawing.
      opt.pixel_pitch = classic ? static_cast<float>(kSlots[slot_index].size) / 16.0f : 0.0f;
      face.id = text_->add_spritefont(face.font, opt, first_extra_);
    }
  }
  return face.id == render::kNoFace ? nullptr : &face;
}

render::FaceId FontSet::face(int slot_index, bool classic) {
  Face* face = get(slot_index, classic);
  return face ? face->id : render::kNoFace;
}

void FontSet::prefetch(int slot_index) {
  if (slot_index < 0 || slot_index >= kSlotCount || !vfs_ || !text_ || ps1()) return;
  Face& face = faces_[static_cast<size_t>((classic_ ? kFiles : 0) + kSlotFile[slot_index])];
  if (face.tried || face.pending.valid()) return;
  face.pending = std::async(std::launch::async, [vfs = vfs_, name = member(slot_index, classic_)] { return load(*vfs, name); });
  if (classic_ && kSlotFile[slot_index] == kSlotFile[kKb])
    face.pending_layout = std::async(std::launch::async, [vfs = vfs_] { return load_metrics(*vfs, member(kKb, false)); });
}

bool FontSet::ready(int slot_index) {
  if (slot_index < 0 || slot_index >= kSlotCount) return true;
  const Face& face = faces_[static_cast<size_t>((classic_ ? kFiles : 0) + kSlotFile[slot_index])];
  return face.tried || !face.pending.valid() || face.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// A face built by this call gets `text` into its first atlas.
void FontSet::prepare(int slot_index, std::u32string_view text) {
  first_extra_ = text;
  Face* face = get(slot_index, classic_);
  first_extra_.clear();
  if (face) text_->prepare(face->id, text);
}

// Glyphs the font lacks are skipped (RDE throws for ci / cur / ch); a blank glyph of at most one texel does not extend the width.
float FontSet::measure(int slot_index, std::u32string_view text, float scale) {
  Face* face = get(slot_index, classic_);
  if (!face) return 0;
  if (!face->font) return text_->measure(face->id, text, scale);
  float x = 0, right = 0;
  for (char32_t ch : text) {
    if (ch == U'\n') {
      x = 0;
      continue;
    }
    if (ch == U'\r') continue;
    const import::SfGlyph* glyph = metrics(*face, ch);
    if (!glyph) continue;
    x = std::max(x + glyph->x_offset, 0.0f);
    if (!(is_blank(ch) && glyph->width() <= 1 && glyph->height() <= 1)) right = std::max(right, x + static_cast<float>(glyph->width()));
    x += static_cast<float>(glyph->width()) + glyph->x_advance;
  }
  return right * scale;
}

// TextBatch adds the glyph's x_offset itself, so it is handed the pen before the offset.
void FontSet::draw(int slot_index, std::u32string_view text, float x, float y, const Ink& ink, float scale, int align, float k) {
  Face* face = get(slot_index, classic_);
  if (!face || text.empty()) return;
  text_->audit_run();
  if (align == kCentre) x -= measure(slot_index, text, scale) * 0.5f;
  else if (align == kRight) x -= measure(slot_index, text, scale);
  if (!face->font) {
    // The PS1 font: the fill colour, a black drop shadow where the ink has a shadow.
    render::TextStyle style;
    style.backdrop = ink.shadow;
    text_->draw(face->id, text, x * k, y * k, ink.normal, scale * k, style);
    return;
  }
  render::TextStyle style;
  style.backdrop_color = ink.outline;
  style.shadow = ink.shadow;
  style.exact = ink.exact;
  // Additive (H5): dst + alpha * (normal * fill + outline * (ring - fill)) = the outline colour over the fill + ring mask, then
  // (normal - outline) over the fill mask, both added with the colour's alpha.
  u32 fill_colour = ink.normal;
  render::SpriteBatch* sprites = ink.additive ? text_->sprites() : nullptr;
  const render::BlendMode old_blend = sprites ? sprites->blend() : render::BlendMode::Alpha;
  if (sprites) {
    fill_colour = ink.normal & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8)
      fill_colour |= static_cast<u32>(std::max(0, static_cast<int>((ink.normal >> shift) & 0xFF) - static_cast<int>((ink.outline >> shift) & 0xFF))) << shift;
    sprites->set_blend(render::BlendMode::AddAlpha);
  }
  const float line_spacing = face->font->line_spacing;
  for (render::TextBatch::Pass pass : {render::TextBatch::Pass::Backdrop, render::TextBatch::Pass::Fill}) {
    float px = 0, py = 0;
    for (char32_t ch : text) {
      if (ch == U'\n') {
        px = 0;
        py += line_spacing;
        continue;
      }
      if (ch == U'\r') continue;
      const import::SfGlyph* glyph = metrics(*face, ch);
      if (!glyph) continue;
      px = std::max(px + glyph->x_offset, 0.0f);
      text_->draw_glyph(face->id, ch, (x + (px - glyph->x_offset) * scale) * k, (y + py * scale) * k, fill_colour, scale * k, pass, style);
      px += static_cast<float>(glyph->width()) + glyph->x_advance;
    }
  }
  if (sprites) sprites->set_blend(old_blend);
}

}  // namespace oc::ui::font
