#include "import/party_menu.h"

namespace oc::import {

namespace {

// Record: s16 x, y, w, h; u8 u, pad; u8 v, pad; s16 du, dv; u16 CLUT x, CLUT y; u8 colour, pad. Seven label records, two bytes of
// padding, three name records, then the element colours.
constexpr size_t kRecord = 0x16, kNamesAt = 7 * kRecord + 2, kColoursAt = kNamesAt + 3 * kRecord + 2;
constexpr size_t kTableSize = kColoursAt + 6 * 3;

int s16_at(ByteSpan data, size_t offset) { return static_cast<s16>(data[offset] | (data[offset + 1] << 8)); }

PartyMenuSprite sprite_at(ByteSpan data, size_t offset) {
  PartyMenuSprite sprite;
  sprite.x = s16_at(data, offset), sprite.y = s16_at(data, offset + 2), sprite.w = s16_at(data, offset + 4), sprite.h = s16_at(data, offset + 6);
  sprite.u = data[offset + 8], sprite.v = data[offset + 10];
  sprite.du = s16_at(data, offset + 12), sprite.dv = s16_at(data, offset + 14);
  const u32 grey = data[offset + 20];
  sprite.colour = grey | grey << 8 | grey << 16;
  return sprite;
}

u32 clut_at(ByteSpan data, size_t offset) { return static_cast<u32>(s16_at(data, offset + 16)) << 16 | static_cast<u16>(s16_at(data, offset + 18)); }

// The labels are drawn 1:1 from one CLUT at full colour; the name quads share another CLUT and their texel height.
bool table_at(ByteSpan data, size_t base) {
  const u32 label_clut = clut_at(data, base), name_clut = clut_at(data, base + kNamesAt);
  for (size_t i = 0; i < 7; ++i) {
    const size_t at = base + i * kRecord;
    const int w = s16_at(data, at + 4), h = s16_at(data, at + 6);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || s16_at(data, at + 12) != w || s16_at(data, at + 14) != h || data[at + 20] != 0x80 ||
        clut_at(data, at) != label_clut)
      return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    const size_t at = base + kNamesAt + i * kRecord;
    if (s16_at(data, at + 4) <= 0 || s16_at(data, at + 6) != s16_at(data, at + 14) || clut_at(data, at) != name_clut || label_clut == name_clut)
      return false;
  }
  return true;
}

}  // namespace

std::optional<PartyMenuLayout> find_party_menu_layout(ByteSpan prg) {
  for (size_t base = 0; base + kTableSize <= prg.size(); base += 4) {
    if (!table_at(prg, base)) continue;
    PartyMenuLayout layout;
    for (size_t i = 0; i < layout.labels.size(); ++i) layout.labels[i] = sprite_at(prg, base + i * kRecord);
    for (size_t i = 0; i < layout.name.size(); ++i) layout.name[i] = sprite_at(prg, base + kNamesAt + i * kRecord);
    for (size_t i = 0; i < layout.element_rgb.size(); ++i)
      for (size_t k = 0; k < 3; ++k) layout.element_rgb[i][k] = prg[base + kColoursAt + i * 3 + k];
    return layout;
  }
  return std::nullopt;
}

}  // namespace oc::import
