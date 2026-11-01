#include "render/text_batch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>

#include "core/log.h"

namespace oc::render {

namespace {
constexpr int kBitmapCols = 64;
constexpr int kBitmapCell = 12;
constexpr int kBitmapRows = (import::kJpGlyphSlots + kBitmapCols - 1) / kBitmapCols;
constexpr u32 kMaxAtlasHeight = 4096;
constexpr u32 kAtlasWidth = 2048;
// Pixel fonts: largest relative size error of whole-pixel font pixels (round(pitch) vs RDE's pitch) before the glyph is drawn
// RDE's way instead (exact float size, linear source rect).
constexpr float kPixelSizeTolerance = 0.10f;

// Pixel-grid decode of one small_font_* glyph. The masks are binary blocks of 4 or 5 texels
// (70 / 16 = 4.375 texels per font pixel): every run between two columns (rows) that differ is round(len / pitch) font
// pixels. A run that is not close to a whole number of font pixels (anti-aliased glyphs: Greek letters, the gender signs,
// cur_70, ch_100 - the last two are the HD files) rejects the glyph, which then draws its source rect.
struct PixelGlyph {
  int nx = 0, ny = 0;
  // rgb is nx * ny * 3: R fill, G fill + outline, B fill + outline + shadow.
  std::vector<u8> rgb;
};

bool split_runs(int count, float pitch, const std::function<bool(int, int)>& differs, std::vector<int>& sample, int& cells) {
  sample.clear();
  cells = 0;
  int start = 0;
  for (int i = 1; i <= count; ++i) {
    if (i < count && !differs(i - 1, i)) continue;
    const float len = static_cast<float>(i - start) / pitch;
    const int font_pixels = static_cast<int>(std::lround(len));
    if (font_pixels < 1 || std::fabs(len - static_cast<float>(font_pixels)) > 0.35f) return false;
    // Sample the centre of every font pixel of the run.
    for (int j = 0; j < font_pixels; ++j)
      sample.push_back(std::min(i - 1, start + static_cast<int>((static_cast<float>(j) + 0.5f) * static_cast<float>(i - start) /
                                                                static_cast<float>(font_pixels))));
    cells += font_pixels;
    start = i;
  }
  return cells > 0;
}

bool decode_pixel_glyph(const import::SpriteFont& sf, const import::SfGlyph& glyph, float pitch, PixelGlyph& out) {
  const int w = glyph.width(), h = glyph.height();
  if (pitch <= 1.0f || w <= 1 || h <= 1) return false;
  std::vector<u8> masks(static_cast<size_t>(w) * h * 3, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      u8 texel[4] = {0, 0, 0, 0};
      sf.texel(static_cast<u32>(glyph.left + x), static_cast<u32>(glyph.top + y), texel);
      for (int channel = 0; channel < 3; ++channel) masks[(static_cast<size_t>(y) * w + x) * 3 + channel] = texel[channel];
    }
  auto at = [&](int x, int y, int channel) { return masks[(static_cast<size_t>(y) * w + x) * 3 + channel]; };
  auto col_differs = [&](int col_a, int col_b) {
    for (int y = 0; y < h; ++y)
      for (int channel = 0; channel < 3; ++channel)
        if (at(col_a, y, channel) != at(col_b, y, channel)) return true;
    return false;
  };
  auto row_differs = [&](int row_a, int row_b) {
    for (int x = 0; x < w; ++x)
      for (int channel = 0; channel < 3; ++channel)
        if (at(x, row_a, channel) != at(x, row_b, channel)) return true;
    return false;
  };
  std::vector<int> sx, sy;
  if (!split_runs(w, pitch, col_differs, sx, out.nx) || !split_runs(h, pitch, row_differs, sy, out.ny)) return false;
  out.rgb.assign(static_cast<size_t>(out.nx) * out.ny * 3, 0);
  for (int y = 0; y < out.ny; ++y)
    for (int x = 0; x < out.nx; ++x)
      for (int channel = 0; channel < 3; ++channel)
        out.rgb[(static_cast<size_t>(y) * out.nx + x) * 3 + channel] = at(sx[static_cast<size_t>(x)], sy[static_cast<size_t>(y)], channel);
  return true;
}
}  // namespace

bool TextBatch::init(GpuContext& ctx, SpriteBatch& sprites) {
  ctx_ = &ctx;
  sprites_ = &sprites;
  if (const char* audit_env = std::getenv("OC_TEXT_AUDIT"); audit_env && *audit_env && *audit_env != '0') audit_on_ = true, audit_verbose_ = *audit_env == '2';
  return linear_.create(ctx, true) && nearest_.create(ctx, false);
}

void TextBatch::destroy() {
  for (Face& font_face : faces_) {
    font_face.fill.reset();
    font_face.back.reset();
    font_face.outline.reset();
  }
  faces_.clear();
  linear_.destroy();
  nearest_.destroy();
  ctx_ = nullptr;
  sprites_ = nullptr;
}

std::unique_ptr<TextBatch::Atlas> TextBatch::make_atlas(u32 w, u32 h, const std::vector<u8>& rgba,
                                                        const Sampler& sampler, bool with_nearest, UploadBatch* batch) {
  auto atlas = std::make_unique<Atlas>();
  if (!atlas->tex.create(*ctx_, w, h, TextureFormat::Rgba8, rgba.data(), batch)) return nullptr;
  atlas->set = TexRef(atlas->tex, sampler);
  if (with_nearest) atlas->set_nearest = TexRef(atlas->tex, nearest_);
  return atlas;
}

FaceId TextBatch::add_spritefont(std::shared_ptr<const import::SpriteFont> font, const SpriteFontOptions& opt, std::u32string_view extra) {
  if (!ctx_ || !font) return kNoFace;
  Face new_face;
  new_face.font = std::move(font);
  new_face.opt = opt;
  faces_.push_back(std::move(new_face));
  FaceId id = static_cast<FaceId>(faces_.size() - 1);
  std::u32string ascii;
  for (char32_t ch = 0x20; ch < 0x7F; ++ch) ascii.push_back(ch);
  ascii.push_back(0x25B6);
  ascii.append(extra);
  prepare(id, ascii);
  return id;
}

FaceId TextBatch::add_bitmap_font(std::shared_ptr<const import::JpGlyphs> glyphs) {
  if (!ctx_ || !glyphs || !glyphs->has_bitmaps()) return kNoFace;
  Face new_face;
  new_face.bitmap = true;
  new_face.jp = std::move(glyphs);
  if (!build_bitmap_atlas(new_face)) return kNoFace;
  faces_.push_back(std::move(new_face));
  return static_cast<FaceId>(faces_.size() - 1);
}

bool TextBatch::build_bitmap_atlas(Face& font_face) {
  const u32 w = kBitmapCols * kBitmapCell, h = kBitmapRows * kBitmapCell;
  std::vector<u8> px(static_cast<size_t>(w) * h * 4, 0);
  for (int idx = 0; idx < import::kJpGlyphSlots; ++idx) {
    const u16* rows = font_face.jp->rows(idx);
    if (!rows) continue;
    int cx = (idx % kBitmapCols) * kBitmapCell, cy = (idx / kBitmapCols) * kBitmapCell;
    for (int y = 0; y < import::kJpGlyphSize; ++y) {
      for (int x = 0; x < import::kJpGlyphSize; ++x) {
        if (!(rows[y] & (0x8000 >> x))) continue;
        u8* pixel = &px[(static_cast<size_t>(cy + y) * w + static_cast<size_t>(cx + x)) * 4];
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 255;
      }
    }
  }
  font_face.fill = make_atlas(w, h, px, nearest_);
  return font_face.fill != nullptr;
}

void TextBatch::prepare(FaceId id, std::u32string_view text) {
  Face* font_face = face(id);
  // The bitmap atlas is complete from the start.
  if (!font_face || font_face->bitmap) return;
  bool added = false;
  for (char32_t ch : text) {
    if (ch == U'\n' || font_face->entries.count(ch)) continue;
    if (std::find(font_face->wanted.begin(), font_face->wanted.end(), ch) == font_face->wanted.end()) {
      font_face->wanted.push_back(ch);
      added = true;
    }
  }
  if (added || !font_face->fill) rebuild_spritefont(*font_face);
}

bool TextBatch::rebuild_spritefont(Face& font_face) {
  const auto tmp0 = std::chrono::steady_clock::now();
  const import::SpriteFont& sf = *font_face.font;
  struct Item {
    const import::SfGlyph* glyph = nullptr;
    u32 x = 0, y = 0;
    // px: non-null packs the decoded font pixels instead of the source rect.
    const PixelGlyph* px = nullptr;
    int w() const { return px ? px->nx : glyph->width(); }
    int h() const { return px ? px->ny : glyph->height(); }
  };
  std::vector<Item> items;
  std::unordered_map<const import::SfGlyph*, size_t> seen;
  std::vector<std::pair<char32_t, const import::SfGlyph*>> map;
  for (char32_t ch : font_face.wanted) {
    const import::SfGlyph* glyph = sf.find_or_default(static_cast<u32>(ch));
    map.emplace_back(ch, glyph);
    if (!glyph || glyph->width() <= 0 || glyph->height() <= 0) continue;
    if (seen.emplace(glyph, items.size()).second) items.push_back({glyph, 0, 0, nullptr});
  }
  // Pixel fonts: the decoded grid of every glyph that has one, packed after the source rects (kept for tiny scales).
  std::unordered_map<const import::SfGlyph*, PixelGlyph> pixels;
  std::unordered_map<const import::SfGlyph*, size_t> pixel_item;
  if (font_face.opt.pixel_pitch > 0.0f) {
    const size_t glyph_count = items.size();
    for (size_t i = 0; i < glyph_count; ++i) {
      PixelGlyph pg;
      if (!decode_pixel_glyph(sf, *items[i].glyph, font_face.opt.pixel_pitch, pg)) continue;
      auto it = pixels.emplace(items[i].glyph, std::move(pg)).first;
      pixel_item[items[i].glyph] = items.size();
      items.push_back({items[i].glyph, 0, 0, &it->second});
    }
    // Face baseline from 'A' (all pixel faces have it; glyphs on it end there).
    if (!font_face.baseline_set) {
      font_face.baseline_set = true;
      PixelGlyph pa;
      if (const import::SfGlyph* glyph_a = sf.find(U'A'); glyph_a && decode_pixel_glyph(sf, *glyph_a, font_face.opt.pixel_pitch, pa))
        font_face.baseline = glyph_a->y_offset / font_face.opt.pixel_pitch + static_cast<float>(pa.ny);
    }
  }
  // Shelf packing, tallest first, 1 px transparent padding.
  std::vector<size_t> order(items.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t index_a, size_t index_b) { return items[index_a].h() > items[index_b].h(); });
  u32 x = 0, y = 0, shelf_h = 0, atlas_h = 1;
  bool overflow = false;
  for (size_t i : order) {
    u32 gw = static_cast<u32>(items[i].w()) + 2, gh = static_cast<u32>(items[i].h()) + 2;
    if (x + gw > kAtlasWidth) {
      y += shelf_h;
      x = 0;
      shelf_h = 0;
    }
    if (y + gh > kMaxAtlasHeight) {
      overflow = true;
      // Does not fit: drawn as missing.
      items[i].glyph = nullptr;
      continue;
    }
    items[i].x = x + 1;
    items[i].y = y + 1;
    x += gw;
    shelf_h = std::max(shelf_h, gh);
    atlas_h = std::max(atlas_h, y + gh);
  }
  if (overflow) log::warn("text atlas overflow: some glyphs are missing");
  const u32 aw = kAtlasWidth, ah = atlas_h;
  std::vector<u8> fill(static_cast<size_t>(aw) * ah * 4, 0), back(fill.size(), 0);
  std::vector<u8> outline(font_face.opt.outline_mask ? fill.size() : 0, 0);
  for (const Item& it : items) {
    if (!it.glyph) continue;
    if (it.px) {
      for (int gy = 0; gy < it.px->ny; ++gy)
        for (int gx = 0; gx < it.px->nx; ++gx) {
          const u8* texel = &it.px->rgb[(static_cast<size_t>(gy) * it.px->nx + gx) * 3];
          size_t offset = (static_cast<size_t>(it.y + static_cast<u32>(gy)) * aw + it.x + static_cast<u32>(gx)) * 4;
          fill[offset] = fill[offset + 1] = fill[offset + 2] = back[offset] = back[offset + 1] = back[offset + 2] = 255;
          fill[offset + 3] = texel[0];
          back[offset + 3] = texel[2];
          if (!outline.empty()) {
            outline[offset] = outline[offset + 1] = outline[offset + 2] = 255;
            outline[offset + 3] = texel[1];
          }
        }
      continue;
    }
    for (int gy = 0; gy < it.glyph->height(); ++gy) {
      for (int gx = 0; gx < it.glyph->width(); ++gx) {
        u8 texel[4];
        if (!sf.texel(static_cast<u32>(it.glyph->left + gx), static_cast<u32>(it.glyph->top + gy), texel)) continue;
        size_t offset = (static_cast<size_t>(it.y + static_cast<u32>(gy)) * aw + it.x + static_cast<u32>(gx)) * 4;
        fill[offset] = fill[offset + 1] = fill[offset + 2] = 255;
        fill[offset + 3] = texel[0];
        back[offset] = back[offset + 1] = back[offset + 2] = 255;
        back[offset + 3] = texel[2];
        if (!outline.empty()) {
          outline[offset] = outline[offset + 1] = outline[offset + 2] = 255;
          outline[offset + 3] = texel[1];
        }
      }
    }
  }
  const auto tmp1 = std::chrono::steady_clock::now();
  font_face.entries.clear();
  for (auto& [ch, glyph] : map) {
    Entry glyph_entry;
    if (!glyph) {
      glyph_entry.adv = sf.line_spacing * 0.25f;
      font_face.entries[ch] = glyph_entry;
      continue;
    }
    glyph_entry.adv = static_cast<float>(glyph->width()) + glyph->x_advance + glyph->x_offset;
    glyph_entry.off_x = glyph->x_offset;
    glyph_entry.off_y = glyph->y_offset;
    auto it = seen.find(glyph);
    if (it != seen.end() && items[it->second].glyph) {
      const Item& item = items[it->second];
      // Drawn width (narrow: right - 1).
      const int dw = font_face.opt.narrow && glyph->width() > 1 ? glyph->width() - 1 : glyph->width();
      glyph_entry.has_quad = true;
      glyph_entry.w = static_cast<float>(dw);
      glyph_entry.h = static_cast<float>(glyph->height());
      glyph_entry.u0 = static_cast<float>(item.x) / static_cast<float>(aw);
      glyph_entry.v0 = static_cast<float>(item.y) / static_cast<float>(ah);
      glyph_entry.u1 = static_cast<float>(item.x + static_cast<u32>(dw)) / static_cast<float>(aw);
      glyph_entry.v1 = static_cast<float>(item.y + static_cast<u32>(glyph->height())) / static_cast<float>(ah);
    }
    if (auto pit = pixel_item.find(glyph); pit != pixel_item.end() && items[pit->second].glyph) {
      const Item& item = items[pit->second];
      glyph_entry.px = true;
      glyph_entry.nx = item.px->nx;
      glyph_entry.ny = item.px->ny;
      // Font pixels below the pen line.
      const float top = glyph->y_offset / font_face.opt.pixel_pitch;
      glyph_entry.up = font_face.baseline > 0.0f ? static_cast<int>(std::lround(font_face.baseline - top)) : glyph_entry.ny;
      glyph_entry.pu0 = static_cast<float>(item.x) / static_cast<float>(aw);
      glyph_entry.pv0 = static_cast<float>(item.y) / static_cast<float>(ah);
      glyph_entry.pu1 = static_cast<float>(item.x + static_cast<u32>(glyph_entry.nx)) / static_cast<float>(aw);
      glyph_entry.pv1 = static_cast<float>(item.y + static_cast<u32>(glyph_entry.ny)) / static_cast<float>(ah);
    }
    font_face.entries[ch] = glyph_entry;
  }
  font_face.fill.reset();
  font_face.back.reset();
  font_face.outline.reset();
  const bool nearest = !pixels.empty();
  const auto tmp2 = std::chrono::steady_clock::now();
  // The 2-3 atlases share one queue wait.
  UploadBatch batch;
  font_face.fill = make_atlas(aw, ah, fill, linear_, nearest, &batch);
  font_face.back = make_atlas(aw, ah, back, linear_, nearest, &batch);
  if (!outline.empty()) font_face.outline = make_atlas(aw, ah, outline, linear_, nearest, &batch);
  batch.flush(*ctx_);
  { const auto tmp3 = std::chrono::steady_clock::now(); auto ms_between = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); }; log::debug("TMP rebuild: pack+fill {:.1f} entries+idle {:.1f} atlases {:.1f} ({}x{} glyphs {})", ms_between(tmp0, tmp1), ms_between(tmp1, tmp2), ms_between(tmp2, tmp3), aw, ah, font_face.wanted.size()); }
  return font_face.fill && font_face.back;
}

const TextBatch::Entry* TextBatch::entry(const Face& font_face, char32_t ch) const {
  auto it = font_face.entries.find(ch);
  return it == font_face.entries.end() ? nullptr : &it->second;
}

float TextBatch::advance(FaceId id, char32_t ch, float scale) const {
  const Face* font_face = face(id);
  if (!font_face) return 0;
  if (font_face->bitmap) {
    int idx = font_face->jp->index_for(ch);
    return static_cast<float>(idx < 0 ? 12 : import::JpGlyphs::advance(idx)) * scale;
  }
  const Entry* glyph_entry = entry(*font_face, ch);
  if (!glyph_entry) return font_face->font->line_spacing * 0.25f * scale;
  return glyph_entry->adv * scale;
}

float TextBatch::glyph_measure(FaceId id, char32_t ch) const {
  const Face* font_face = face(id);
  return font_face && !font_face->bitmap && font_face->font ? font_face->font->measure(static_cast<u32>(ch)) : 0.0f;
}

float TextBatch::line_height(FaceId id, float scale) const {
  const Face* font_face = face(id);
  if (!font_face) return 0;
  return (font_face->bitmap ? 13.0f : font_face->font->line_spacing) * scale;
}

float TextBatch::measure(FaceId id, std::u32string_view text, float scale) const {
  float widest = 0, cur = 0;
  for (char32_t ch : text) {
    if (ch == U'\n') {
      widest = std::max(widest, cur);
      cur = 0;
    } else {
      cur += advance(id, ch, scale);
    }
  }
  return std::max(widest, cur);
}

void TextBatch::draw_glyph(FaceId id, char32_t ch, float x, float y, u32 color, float scale, Pass pass,
                           const TextStyle& style) {
  Face* font_face = face(id);
  if (!font_face || !sprites_) return;
  if (pass == Pass::Backdrop && !style.backdrop) return;
  const u32 alpha = color >> 24;
  const u32 back_color = (style.backdrop_color & 0x00FFFFFFu) | (alpha << 24);
  if (font_face->bitmap) {
    int idx = font_face->jp->index_for(ch);
    if (idx < 0 || !font_face->fill || ch == U' ' || ch == 0x3000) return;
    const float cu = 1.0f / static_cast<float>(kBitmapCols), cv = 1.0f / static_cast<float>(kBitmapRows);
    const float frac = static_cast<float>(import::kJpGlyphSize) / static_cast<float>(kBitmapCell);
    float u0 = static_cast<float>(idx % kBitmapCols) * cu, v0 = static_cast<float>(idx / kBitmapCols) * cv;
    float sz = static_cast<float>(import::kJpGlyphSize) * scale;
    float dx = pass == Pass::Backdrop ? scale : 0.0f;
    sprites_->draw(font_face->fill->set, std::floor(x + dx), std::floor(y + dx), sz, sz, u0, v0, u0 + cu * frac, v0 + cv * frac,
                   pass == Pass::Backdrop ? back_color : color);
    if (audit_on_ && pass == Pass::Fill) audit_glyph(nullptr, ch, std::floor(x), std::floor(y), std::floor(x) + sz, std::floor(y) + sz);
    return;
  }
  const Entry* glyph_entry = entry(*font_face, ch);
  if (!glyph_entry || !glyph_entry->has_quad) {
    // Nothing drawn (blank, or missing).
    if (audit_on_ && pass == Pass::Fill) audit_glyph(font_face->font.get(), ch, x, y, x, y);
    return;
  }
  Atlas* atlas = pass == Pass::Fill ? font_face->fill.get() : !style.shadow && font_face->outline ? font_face->outline.get() : font_face->back.get();
  if (!atlas) return;
  const u32 col = pass == Pass::Backdrop ? back_color : color;
  if (grid_x_ > 0 && grid_y_ > 0) {
    // PS1-resolution target: view units are canvas units, the target has grid_x_ / grid_y_ pixels per unit. RDE's
    // layout (pen, advance, glyph box) is kept; the glyph is drawn through the box filter (4 x 4 taps over each target pixel), which
    // equals nearest sampling where a font pixel covers whole target pixels.
    const SpriteFilter old = sprites_->filter();
    sprites_->set_filter(SpriteFilter::Box);
    // pv: view units per font pixel.
    const float pv = font_face->opt.pixel_pitch * scale;
    if (glyph_entry->px && pv > 0.0f) {
      // px, py: target pixels per font pixel.
      const float px = pv * grid_x_, py = pv * grid_y_;
      const float nx = std::round(px), ny = std::round(py);
      const bool whole = nx >= 1.0f && ny >= 1.0f && std::fabs(nx - px) <= kPixelSizeTolerance * px && std::fabs(ny - py) <= kPixelSizeTolerance * py;
      const float cx = x + glyph_entry->off_x * scale + static_cast<float>(glyph_entry->nx) * pv * 0.5f;
      const float base = font_face->baseline > 0.0f ? y + font_face->baseline * pv : y + (glyph_entry->off_y / font_face->opt.pixel_pitch + static_cast<float>(glyph_entry->ny)) * pv;
      // The line's baseline on a target row.
      const float base_row = std::round(base * grid_y_);
      float x0, y0, w, h;
      // Whole: whole target pixels per font pixel, the glyph's left edge on a target column.
      if (whole) {
        const float wt = static_cast<float>(glyph_entry->nx) * nx, ht = static_cast<float>(glyph_entry->ny) * ny;
        x0 = std::round(cx * grid_x_ - wt * 0.5f) / grid_x_;
        y0 = (base_row - static_cast<float>(glyph_entry->up) * ny) / grid_y_;
        w = wt / grid_x_, h = ht / grid_y_;
      } else {
        // RDE's exact glyph size, area-filtered.
        w = static_cast<float>(glyph_entry->nx) * pv, h = static_cast<float>(glyph_entry->ny) * pv;
        x0 = cx - w * 0.5f;
        y0 = base_row / grid_y_ - static_cast<float>(glyph_entry->up) * pv;
      }
      sprites_->draw(atlas->set_nearest ? atlas->set_nearest : atlas->set, x0, y0, w, h, glyph_entry->pu0, glyph_entry->pv0, glyph_entry->pu1, glyph_entry->pv1, col);
      if (audit_on_ && pass == Pass::Fill) audit_glyph(font_face->font.get(), ch, x0, y0, x0 + w, y0 + h);
    } else {
      sprites_->draw(atlas->set, x + glyph_entry->off_x * scale, y + glyph_entry->off_y * scale, glyph_entry->w * scale, glyph_entry->h * scale, glyph_entry->u0, glyph_entry->v0, glyph_entry->u1, glyph_entry->v1, col);
      if (audit_on_ && pass == Pass::Fill)
        audit_glyph(font_face->font.get(), ch, x + glyph_entry->off_x * scale, y + glyph_entry->off_y * scale, x + (glyph_entry->off_x + glyph_entry->w) * scale, y + (glyph_entry->off_y + glyph_entry->h) * scale);
    }
    sprites_->set_filter(old);
    return;
  }
  // pitch: screen pixels per font pixel at RDE's scale.
  const float pitch = font_face->opt.pixel_pitch * scale;
  const float whole_px = std::max(1.0f, std::round(pitch));
  // Whole pixels only where they keep RDE's size (whole_px = round(pitch) would draw 0.35 menu text 31 % too big
  // with the same advance - "letters too close", JP "too big" - and 0.3125 text 27 % too small); otherwise RDE's own exact draw.
  if (glyph_entry->px && pitch >= 0.75f && std::fabs(whole_px - pitch) <= kPixelSizeTolerance * pitch && atlas->set_nearest) {
    // Whole screen pixels per font pixel: whole_px = round(pitch). The glyph keeps its exact RDE centre on x; on y it
    // hangs from the face baseline, snapped once per pen line, so every glyph of a line shares the baseline row.
    const float w = static_cast<float>(glyph_entry->nx) * whole_px, h = static_cast<float>(glyph_entry->ny) * whole_px;
    const float cx = x + glyph_entry->off_x * scale + static_cast<float>(glyph_entry->nx) * pitch * 0.5f;
    const float base = font_face->baseline > 0.0f ? y + font_face->baseline * pitch : y + (glyph_entry->off_y / font_face->opt.pixel_pitch + static_cast<float>(glyph_entry->ny)) * pitch;
    const float top = std::round(base) - static_cast<float>(glyph_entry->up) * whole_px;
    sprites_->draw(atlas->set_nearest, std::round(cx - w * 0.5f), top, w, h, glyph_entry->pu0, glyph_entry->pv0, glyph_entry->pu1, glyph_entry->pv1, col);
    if (audit_on_ && pass == Pass::Fill) audit_glyph(font_face->font.get(), ch, std::round(cx - w * 0.5f), top, std::round(cx - w * 0.5f) + w, top + h);
    return;
  }
  sprites_->draw(atlas->set, x + glyph_entry->off_x * scale, y + glyph_entry->off_y * scale, glyph_entry->w * scale, glyph_entry->h * scale, glyph_entry->u0, glyph_entry->v0, glyph_entry->u1,
                 glyph_entry->v1, pass == Pass::Backdrop ? back_color : color);
  if (audit_on_ && pass == Pass::Fill)
    audit_glyph(font_face->font.get(), ch, x + glyph_entry->off_x * scale, y + glyph_entry->off_y * scale, x + (glyph_entry->off_x + glyph_entry->w) * scale, y + (glyph_entry->off_y + glyph_entry->h) * scale);
}

void TextBatch::draw(FaceId id, std::u32string_view text, float x, float y, u32 color, float scale,
                     const TextStyle& style) {
  if (audit_on_) audit_run();
  // One filter switch per string, not per glyph.
  const bool grid = grid_x_ > 0 && grid_y_ > 0 && sprites_;
  const SpriteFilter old = sprites_ ? sprites_->filter() : SpriteFilter::Sampler;
  if (grid) sprites_->set_filter(SpriteFilter::Box);
  struct Restore {
    SpriteBatch* batch;
    SpriteFilter filter;
    ~Restore() {
      if (batch) batch->set_filter(filter);
    }
  } restore{grid ? sprites_ : nullptr, old};
  for (Pass pass : {Pass::Backdrop, Pass::Fill}) {
    if (pass == Pass::Backdrop && !style.backdrop) continue;
    float px = x, py = y;
    for (char32_t ch : text) {
      if (ch == U'\n') {
        px = x;
        py += line_height(id, scale);
        continue;
      }
      draw_glyph(id, ch, px, py, color, scale, pass, style);
      px += advance(id, ch, scale);
    }
  }
}


// ---- layout audit -----------------------------------------------------------------------------------------------------
namespace {
double audit_now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string audit_utf8(std::u32string_view text) {
  std::string out;
  for (char32_t ch : text) {
    if (ch == U'\n') {
      out += "|";
      continue;
    }
    if (ch < 0x80) {
      out += static_cast<char>(ch);
    } else if (ch < 0x800) {
      out += static_cast<char>(0xC0 | (ch >> 6));
      out += static_cast<char>(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
      out += static_cast<char>(0xE0 | (ch >> 12));
      out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (ch & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (ch >> 18));
      out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (ch & 0x3F));
    }
  }
  return out;
}
bool audit_blank(char32_t ch) { return ch == U' ' || ch == U'\t' || ch == 0x3000 || ch == 0xA0 || ch == U'\r' || ch == U'\n'; }
}  // namespace

void TextBatch::audit_enable(bool on) {
  audit_on_ = on;
  audit_clear();
}

void TextBatch::audit_run() {
  if (!audit_on_) return;
  audit_auto_frame();
  audit_new_run_ = true;
}

void TextBatch::audit_obstacle(float x, float y, float w, float h, std::string_view label) {
  if (!audit_on_) return;
  audit_auto_frame();
  audit_obstacles_.push_back({x, y, x + w, y + h, std::string(label)});
}

void TextBatch::audit_clear() {
  audit_runs_.clear();
  audit_obstacles_.clear();
  audit_new_run_ = true;
}

void TextBatch::audit_glyph(const import::SpriteFont* font, char32_t ch, float x0, float y0, float x1, float y1) {
  audit_auto_frame();
  if (audit_new_run_ || audit_runs_.empty()) {
    // Automatic mode: a string starting with the same glyph at the same spot as the frame's first string = the next frame (uncapped
    // frame rates leave no pause between frames).
    if (!audit_manual_ && !audit_runs_.empty() && ch == audit_first_ch_ && x0 == audit_first_x_ && y0 == audit_first_y_) {
      audit_last_ = 0;
      audit_flush_auto();
    } else if (!audit_manual_ && audit_runs_.size() >= 2 && audit_runs_.back().text == audit_runs_.front().text) {
      // ... or the frame's first string drawn again (it moved: a scrolling / sliding list): it starts the next frame.
      AuditRun again = std::move(audit_runs_.back());
      audit_runs_.pop_back();
      audit_flush_auto();
      audit_runs_.push_back(std::move(again));
      if (const AuditRun& first_run = audit_runs_.front(); !first_run.glyphs.empty())
        audit_first_ch_ = first_run.glyphs.front().ch, audit_first_x_ = first_run.glyphs.front().x0, audit_first_y_ = first_run.glyphs.front().y0;
    }
    if (audit_runs_.empty()) audit_first_ch_ = ch, audit_first_x_ = x0, audit_first_y_ = y0;
    audit_runs_.emplace_back();
    audit_new_run_ = false;
  }
  AuditRun& run = audit_runs_.back();
  run.text += ch;
  if (font && !audit_blank(ch) && !font->find(static_cast<u32>(ch))) ++run.missing;
  if (x1 > x0 && y1 > y0 && !audit_blank(ch)) run.glyphs.push_back({x0, y0, x1, y1, ch});
}

void TextBatch::audit_flush_auto() {
  // OC_TEXT_AUDIT=2: a sample of what is being checked.
  if (audit_verbose_ && ++audit_frames_ % 300 == 1) {
    std::string sample;
    for (const AuditRun& run : audit_runs_) sample += " \"" + audit_utf8(run.text) + "\"";
    log::info("text-audit frame {}: {} strings:{}", audit_frames_, audit_runs_.size(), sample.substr(0, 400));
  }
  for (std::string& issue : audit_issues())
    if (std::find(audit_seen_.begin(), audit_seen_.end(), issue) == audit_seen_.end()) {
      log::info("text-audit: {}", issue);
      audit_seen_.push_back(std::move(issue));
    }
  audit_clear();
}

void TextBatch::audit_auto_frame() {
  if (audit_manual_) return;
  const double now = audit_now();
  if (audit_last_ > 0 && now - audit_last_ > 0.006 && (!audit_runs_.empty() || !audit_obstacles_.empty())) audit_flush_auto();
  audit_last_ = now;
}

std::vector<std::string> TextBatch::audit_issues() const {
  std::vector<std::string> out;
  // Glyph boxes include the outline / shadow margin of the spritefont cells: compare the inner 70 % (15 % inset per side).
  auto inner = [](const AuditGlyph& glyph) {
    const float ix = (glyph.x1 - glyph.x0) * 0.15f, iy = (glyph.y1 - glyph.y0) * 0.15f;
    return AuditGlyph{glyph.x0 + ix, glyph.y0 + iy, glyph.x1 - ix, glyph.y1 - iy, glyph.ch};
  };
  auto box = [](const AuditRun& run) {
    AuditGlyph bounds{1e9f, 1e9f, -1e9f, -1e9f, 0};
    for (const AuditGlyph& glyph : run.glyphs) {
      bounds.x0 = std::min(bounds.x0, glyph.x0);
      bounds.y0 = std::min(bounds.y0, glyph.y0);
      bounds.x1 = std::max(bounds.x1, glyph.x1);
      bounds.y1 = std::max(bounds.y1, glyph.y1);
    }
    return bounds;
  };
  for (size_t i = 0; i < audit_runs_.size(); ++i) {
    const AuditRun& run = audit_runs_[i];
    if (run.missing) out.push_back(std::format("missing {} glyph(s) in \"{}\"", run.missing, audit_utf8(run.text)));
    const AuditGlyph run_box = box(run);
    if (!run.glyphs.empty() && audit_vw_ > 0 && (run_box.x0 < -1 || run_box.y0 < -1 || run_box.x1 > audit_vw_ + 1 || run_box.y1 > audit_vh_ + 1))
      out.push_back(std::format("offscreen \"{}\" box ({:.0f},{:.0f})-({:.0f},{:.0f})", audit_utf8(run.text), run_box.x0, run_box.y0, run_box.x1, run_box.y1));
    for (size_t j = i + 1; j < audit_runs_.size(); ++j) {
      const AuditRun& other = audit_runs_[j];
      const AuditGlyph other_box = box(other);
      // Two strings starting at (almost) one spot: a deliberate overprint (shadow / emboss / blink copy) or, in the automatic mode, the
      // same slot of two frames that ran together (uncapped frame rate, the screen changed state) - not a layout overlap.
      if (std::fabs(other_box.x0 - run_box.x0) < 4 && std::fabs(other_box.y0 - run_box.y0) < 4) continue;
      float worst = 0;
      for (const AuditGlyph& g0 : run.glyphs)
        for (const AuditGlyph& g1 : other.glyphs) {
          const AuditGlyph inner_a = inner(g0), inner_b = inner(g1);
          const float w = std::min(inner_a.x1, inner_b.x1) - std::max(inner_a.x0, inner_b.x0), h = std::min(inner_a.y1, inner_b.y1) - std::max(inner_a.y0, inner_b.y0);
          if (w > 0 && h > 0) worst = std::max(worst, w * h);
        }
      if (worst >= 4.0f)
        out.push_back(std::format("overlap \"{}\" ({:.0f},{:.0f})-({:.0f},{:.0f}) x \"{}\" ({:.0f},{:.0f})-({:.0f},{:.0f}) area {:.0f}",
                                  audit_utf8(run.text), run_box.x0, run_box.y0, run_box.x1, run_box.y1, audit_utf8(other.text), other_box.x0, other_box.y0, other_box.x1, other_box.y1, worst));
    }
    for (const AuditObstacle& obstacle : audit_obstacles_) {
      // A glyph crosses the obstacle edge when part of its inner box is inside and part outside.
      for (const AuditGlyph& g0 : run.glyphs) {
        const AuditGlyph inner_box = inner(g0);
        const float w = std::min(inner_box.x1, obstacle.x1) - std::max(inner_box.x0, obstacle.x0), h = std::min(inner_box.y1, obstacle.y1) - std::max(inner_box.y0, obstacle.y0);
        if (w <= 0 || h <= 0) continue;
        if (inner_box.x0 >= obstacle.x0 && inner_box.x1 <= obstacle.x1 && inner_box.y0 >= obstacle.y0 && inner_box.y1 <= obstacle.y1) continue;
        out.push_back(std::format("edge \"{}\" ({:.0f},{:.0f})-({:.0f},{:.0f}) crosses {} ({:.0f},{:.0f})-({:.0f},{:.0f})", audit_utf8(run.text),
                                  run_box.x0, run_box.y0, run_box.x1, run_box.y1, obstacle.label, obstacle.x0, obstacle.y0, obstacle.x1, obstacle.y1));
        break;
      }
    }
  }
  return out;
}

int TextBatch::audit_report(std::string_view tag) {
  if (!audit_on_) return 0;
  const std::vector<std::string> issues = audit_issues();
  for (const std::string& issue : issues) log::info("text-audit [{}]: {}", tag, issue);
  // OC_TEXT_AUDIT=2 also lists every string with its ink box (canvas px): a text check of what a screen draws, no image needed.
  if (const char* audit_env = std::getenv("OC_TEXT_AUDIT"); audit_env && std::string_view(audit_env) == "2")
    for (const AuditRun& run : audit_runs_) {
      if (run.glyphs.empty()) {
        log::info("text-audit [{}]: list \"{}\" (no glyphs)", tag, audit_utf8(run.text));
        continue;
      }
      float x0 = run.glyphs[0].x0, y0 = run.glyphs[0].y0, x1 = run.glyphs[0].x1, y1 = run.glyphs[0].y1;
      for (const AuditGlyph& glyph : run.glyphs) x0 = std::min(x0, glyph.x0), y0 = std::min(y0, glyph.y0), x1 = std::max(x1, glyph.x1), y1 = std::max(y1, glyph.y1);
      log::info("text-audit [{}]: list \"{}\" ({:.0f},{:.0f})-({:.0f},{:.0f})", tag, audit_utf8(run.text), x0, y0, x1, y1);
    }
  log::info("text-audit [{}]: {} strings, {} issue(s)", tag, audit_runs_.size(), issues.size());
  audit_clear();
  audit_manual_ = true;
  audit_last_ = 0;
  return static_cast<int>(issues.size());
}

}  // namespace oc::render
