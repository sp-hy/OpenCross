#include "import/name_menu.h"

#include <algorithm>

#include "import/ps1_text.h"
#include "vfs/vfs.h"

namespace oc::import {

namespace {

// Where each build keeps its strings. USA: the grid strips (the left half's rows 0..6, then the right half's, 12 bytes apart), the
// ASCII prompt, the length limit (u32) and the two refused name prefixes in field-font codes. Japanese: the prompt, drawn as two strips
// side by side, Viper's rank and the refused name in Shift-JIS.
struct NameMenuBuild {
  size_t size;
  size_t left_strips, right_strips, prompt, max_length;
  size_t prompt_tail, rank;
  size_t reserved[2];
};
constexpr int kGridStrips = 7;
constexpr size_t kStripStride = 12;
constexpr NameMenuBuild kBuilds[] = {
    {15512, 0x367C, 0x36DC, 0x3760, 0x3A28, 0, 0, {0x3750, 0x3758}},
    {18696, 0, 0, 0x34C4, 0, 0x34D8, 0x34E0, {0x3508, 0}},
};

// A NUL-terminated string of field-font codes: digits 0x28.., capitals 0x32.., small letters 0x4C..; empty on any other code.
std::u32string glyph_string(ByteSpan prg, size_t at) {
  std::u32string out;
  for (size_t i = at; i < prg.size() && prg[i]; ++i) {
    const u8 code = prg[i];
    if (code >= 0x28 && code < 0x32) out.push_back(U'0' + (code - 0x28));
    else if (code >= 0x32 && code < 0x4C) out.push_back(U'A' + (code - 0x32));
    else if (code >= 0x4C && code < 0x66) out.push_back(U'a' + (code - 0x4C));
    else return {};
  }
  return out;
}

// A Shift-JIS string at `at` up to its NUL, full-width forms kept; empty when a pair is not a known character.
std::u32string sjis_string(ByteSpan prg, size_t at) {
  std::u32string out;
  for (size_t i = at; i + 1 < prg.size() && prg[i]; i += 2) {
    const char32_t cp = sjis_to_unicode(static_cast<u16>(prg[i] << 8 | prg[i + 1]));
    if (!cp) return {};
    out.push_back(cp);
  }
  return out;
}

// A grid strip: five cells, full-width letters and digits as ASCII, the ideographic space a blank cell.
bool grid_strip(ByteSpan prg, size_t at, NameGridRow& row, int first) {
  const std::u32string strip = sjis_string(prg, at);
  if (strip.size() != 5) return false;
  for (size_t i = 0; i < strip.size(); ++i) {
    const char32_t cp = strip[i];
    if (cp == U'　') row[static_cast<size_t>(first) + i] = 0;
    else if (cp >= U'！' && cp <= U'～') row[static_cast<size_t>(first) + i] = cp - 0xFEE0;
    else return false;
  }
  return true;
}

}  // namespace

std::shared_ptr<const NameMenuText> parse_name_menu(ByteSpan prg) {
  for (const NameMenuBuild& build : kBuilds) {
    if (prg.size() != build.size) continue;
    auto text = std::make_shared<NameMenuText>();
    if (build.left_strips) {
      text->grid.assign(kNameGridSize, NameGridRow{});
      for (int row = 0; row < kGridStrips; ++row) {
        const size_t step = static_cast<size_t>(row) * kStripStride;
        if (!grid_strip(prg, build.left_strips + step, text->grid[static_cast<size_t>(row)], 0) ||
            !grid_strip(prg, build.right_strips + step, text->grid[static_cast<size_t>(row)], 5))
          return nullptr;
      }
      for (size_t i = build.prompt; i < prg.size() && prg[i]; ++i) text->prompt.push_back(prg[i]);
      for (size_t i = 0; i < 4; ++i) text->max_length |= prg[build.max_length + i] << (8 * i);
      for (size_t at : build.reserved) text->reserved.push_back(glyph_string(prg, at));
      text->reserved_prefix = true;
    } else {
      text->prompt = sjis_string(prg, build.prompt) + sjis_string(prg, build.prompt_tail);
      text->rank = sjis_string(prg, build.rank);
      text->reserved.push_back(sjis_string(prg, build.reserved[0]));
    }
    if (text->prompt.empty() || std::any_of(text->reserved.begin(), text->reserved.end(), [](const auto& name) { return name.empty(); }))
      return nullptr;
    return text;
  }
  return nullptr;
}

std::shared_ptr<const NameMenuText> load_name_menu(const vfs::Vfs& vfs) {
  const auto prg = vfs.read_disc("cdrom/prog/namemenu.prg");
  return prg ? parse_name_menu(*prg) : nullptr;
}

}  // namespace oc::import
