#include "import/spritefont.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

#include "core/span_reader.h"

namespace oc::import {

namespace {
size_t bytes_per_texel(u32 format) {
  switch (format) {
    case kDxgiR8G8B8A8: return 4;
    case kDxgiR8: return 1;
    case kDxgiB5G6R5: return 2;
    case kDxgiB8G8R8A8: return 4;
    case kDxgiB4G4R4A4: return 2;
    default: return 0;
  }
}

bool glyph_before(const SfGlyph& lhs, const SfGlyph& rhs) { return lhs.ch < rhs.ch; }

float read_f32(SpanReader& reader) {
  const u32 bits = reader.u32_();
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
}  // namespace

bool SpriteFont::format_supported(u32 format) { return bytes_per_texel(format) != 0; }

const SfGlyph* SpriteFont::find(u32 ch) const {
  auto it = std::lower_bound(glyphs.begin(), glyphs.end(), ch, [](const SfGlyph& glyph, u32 code) { return glyph.ch < code; });
  return it != glyphs.end() && it->ch == ch ? &*it : nullptr;
}

const SfGlyph* SpriteFont::find_or_default(u32 ch) const {
  if (const SfGlyph* glyph = find(ch)) return glyph;
  return default_char ? find(default_char) : nullptr;
}

float SpriteFont::measure(u32 ch) const {
  const SfGlyph* glyph = find_or_default(ch);
  if (!glyph || (std::iswspace(static_cast<wint_t>(ch)) && glyph->width() <= 1 && glyph->height() <= 1)) return 0.0f;
  return std::max(0.0f, glyph->x_offset) + static_cast<float>(glyph->width());
}

bool SpriteFont::texel(u32 x, u32 y, u8 out[4]) const {
  const size_t bytes = bytes_per_texel(format);
  if (!bytes || x >= tex_width || y >= rows) return false;
  const size_t offset = pixel_offset + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * bytes;
  if (offset + bytes > file.size()) return false;
  const u8* texel = &file[offset];
  switch (format) {
    case kDxgiR8G8B8A8: std::memcpy(out, texel, 4); return true;
    case kDxgiR8: out[0] = out[1] = out[2] = 255; out[3] = texel[0]; return true;
    case kDxgiB5G6R5: {
      const u16 packed = static_cast<u16>(texel[0] | (texel[1] << 8));
      out[0] = static_cast<u8>(((packed >> 11) & 31) * 255 / 31);
      out[1] = static_cast<u8>(((packed >> 5) & 63) * 255 / 63);
      out[2] = static_cast<u8>((packed & 31) * 255 / 31);
      out[3] = 255;
      return true;
    }
    case kDxgiB8G8R8A8: out[0] = texel[2]; out[1] = texel[1]; out[2] = texel[0]; out[3] = texel[3]; return true;
    case kDxgiB4G4R4A4: {
      // Bits 12-15 A, 8-11 R, 4-7 G, 0-3 B.
      const u16 packed = static_cast<u16>(texel[0] | (texel[1] << 8));
      out[0] = static_cast<u8>(((packed >> 8) & 15) * 17);
      out[1] = static_cast<u8>(((packed >> 4) & 15) * 17);
      out[2] = static_cast<u8>((packed & 15) * 17);
      out[3] = static_cast<u8>(((packed >> 12) & 15) * 17);
      return true;
    }
    default: return false;
  }
}

std::optional<SpriteFont> parse_spritefont(Bytes&& file, std::string* error) {
  auto fail = [&](const char* message) -> std::optional<SpriteFont> {
    if (error) *error = message;
    return std::nullopt;
  };
  try {
    SpanReader reader(file);
    if (file.size() < 12 || std::memcmp(file.data(), "DXTKfont", 8) != 0) return fail("not a DXTKfont file");
    reader.skip(8);
    const u32 count = reader.u32_();
    if (static_cast<u64>(count) * 32 + 28 > reader.remaining()) return fail("glyph table past end of file");
    SpriteFont font;
    font.glyphs.resize(count);
    for (SfGlyph& glyph : font.glyphs) {
      glyph.ch = reader.u32_();
      glyph.left = reader.s32_();
      glyph.top = reader.s32_();
      glyph.right = reader.s32_();
      glyph.bottom = reader.s32_();
      glyph.x_offset = read_f32(reader);
      glyph.y_offset = read_f32(reader);
      glyph.x_advance = read_f32(reader);
    }
    font.line_spacing = read_f32(reader);
    font.default_char = reader.u32_();
    font.tex_width = reader.u32_();
    font.tex_height = reader.u32_();
    font.format = reader.u32_();
    font.stride = reader.u32_();
    font.rows = reader.u32_();
    if (!SpriteFont::format_supported(font.format)) return fail("unsupported DXGI format");
    if (static_cast<u64>(font.stride) * font.rows > reader.remaining()) return fail("pixel block past end of file");
    if (font.stride < font.tex_width * bytes_per_texel(font.format)) return fail("stride smaller than a texture row");
    font.pixel_offset = reader.pos();
    if (!std::is_sorted(font.glyphs.begin(), font.glyphs.end(), glyph_before))
      std::stable_sort(font.glyphs.begin(), font.glyphs.end(), glyph_before);
    font.file = std::move(file);
    return font;
  } catch (const std::exception&) {
    return fail("truncated spritefont");
  }
}

}  // namespace oc::import
