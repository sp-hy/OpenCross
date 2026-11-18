#include "import/battle_hud.h"

#include <algorithm>
#include <span>
#include <string_view>

#include "core/span_reader.h"
#include "import/container.h"
#include "import/lzss.h"
#include "import/room_effects.h"
#include "import/tim.h"

namespace oc::import {

const char* const kHudSheetNames[kHudSheets] = {
    "viva", "osak", "gg",   "curs", "2bnu", "arro", "gree", "grey", "purp", "red",  "yell", "blue", "niji", "xgre", "xwhi", "xbla",
    "xred", "xyel", "xblu", "xnij", "xoff", "egre", "ewhi", "ebla", "ered", "eyel", "eblu", "enij", "win4", "waku", "stat", "colo",
    "etra", "cami", "ugre", "ugr1", "upur", "ured", "uyel", "ublu", "unij", "paus", "elef", "mark", "slow", "elme", "ccba"};

const char* const kResultsSheetNames[kResultsSheets] = {"gree", "grey", "purp", "red",  "yell", "blue", "gg",  "curs",
                                                        "osak", "viva", "sico", "levu", "2bnu", "paus", "elef"};

namespace {

// The USA build's name glyph widths: a byte per battle-font frame from frame 100 on (lower frames advance 4 pixels), in prog/battle.prg.
constexpr size_t kUsaNameAdvanceOffset = 0x35976;
constexpr int kUsaFirstWideFrame = 100;
constexpr int kUsaNarrowAdvance = 4;
constexpr int kJapanNameAdvance = 9;

std::optional<Tim> tim_in(ByteSpan data) {
  if (!is_lzss(data)) return parse_tim(data);
  auto unpacked = lzss_decode(data);
  return unpacked ? parse_tim(*unpacked) : std::nullopt;
}

struct NamedTim {
  std::string_view name;
  HudFrame* out;
};

// Every TIM of a drp and its nested drps onto `vram`, in file order; the TIMs `named` lists are also decoded into their frames.
void load_tims(ByteSpan data, Vram& vram, std::span<const NamedTim> named_tims, int depth = 0) {
  auto drp = parse_drp(data);
  if (!drp) return;
  for (const DrpEntry& entry : drp->entries) {
    if (entry.type == 1 && depth < 4 && is_drp(entry.data)) load_tims(entry.data, vram, named_tims, depth + 1);
    if (entry.type != 4) continue;
    auto tim = tim_in(entry.data);
    if (!tim) continue;
    vram.blit_tim(*tim);
    HudFrame* named = nullptr;
    for (const NamedTim& candidate : named_tims)
      if (entry.name == candidate.name) named = candidate.out;
    if (!named) continue;
    if (auto indexed = decode_tim(*tim, 0, true)) {
      const Image rgba = to_rgba(*indexed);
      named->w = static_cast<int>(rgba.width);
      named->h = static_cast<int>(rgba.height);
      named->rgba = rgba.pixels;
    }
  }
}

// The texel of a sheet part at (i, j) of its rectangle, through its CLUT, with the semi-transparency bit kept.
u32 part_texel(const Vram& vram, const assets::EffectSprite& part, int i, int j) {
  const int row = (part.page_y() + part.v + j) & 511;
  const int tx = part.u + i;
  unsigned index;
  if (part.depth() == 0) index = (vram.at(part.page_x() + (tx >> 2), row) >> ((tx & 3) * 4)) & 15u;
  else index = (vram.at(part.page_x() + (tx >> 1), row) >> ((tx & 1) * 8)) & 255u;
  return bgr555_to_rgba_stp(vram.at(part.clut_x() + static_cast<int>(index), part.clut_y()));
}

HudFrame compose(const Vram& vram, const std::vector<assets::EffectSprite>& parts) {
  HudFrame out;
  if (parts.empty()) return out;
  int x0 = parts[0].x, y0 = parts[0].y, x1 = x0, y1 = y0;
  for (const assets::EffectSprite& part : parts) {
    x0 = std::min<int>(x0, part.x), y0 = std::min<int>(y0, part.y);
    x1 = std::max(x1, part.x + part.w), y1 = std::max(y1, part.y + part.h);
  }
  out.x = x0, out.y = y0, out.w = x1 - x0, out.h = y1 - y0;
  out.rgba.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 4, 0);
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    const assets::EffectSprite& part = *it;
    // Flag bit 11 draws the part as a plain rectangle, which cannot be mirrored.
    const bool quad = (part.flags & 0x800) == 0;
    for (int j = 0; j < part.h; ++j)
      for (int i = 0; i < part.w; ++i) {
        const u32 texel = part_texel(vram, part, quad && part.mirror_x() ? part.w - 1 - i : i, quad && part.mirror_y() ? part.h - 1 - j : j);
        if ((texel >> 24) == 0) continue;
        const size_t at = (static_cast<size_t>(part.y - y0 + j) * static_cast<size_t>(out.w) + static_cast<size_t>(part.x - x0 + i)) * 4;
        store_rgba(&out.rgba[at], texel);
      }
  }
  return out;
}

// The frames of every sheet `names` lists, composed from the "tana" pack over `vram`; the first sheet of a name wins.
void compose_sheets(ByteSpan tana, const Vram& vram, const char* const* names, std::span<std::vector<HudFrame>> out) {
  const assets::RoomEffects pack = load_battle_effects(tana, {}, vram);
  for (const assets::SpriteSheet& sheet : pack.sheets)
    for (size_t n = 0; n < out.size(); ++n) {
      std::vector<HudFrame>& frames = out[n];
      if (!frames.empty() || std::string_view(sheet.name.c_str()) != names[n]) continue;
      for (const std::vector<assets::EffectSprite>& parts : sheet.frames) frames.push_back(compose(vram, parts));
    }
}

const HudFrame* sheet_frame(std::span<const std::vector<HudFrame>> sheets, int type, int frame_number) {
  const size_t sheet = static_cast<size_t>(type / 4);
  if (type < 0 || (type & 3) || sheet >= sheets.size() || frame_number < 1) return nullptr;
  const std::vector<HudFrame>& frames = sheets[sheet];
  if (static_cast<size_t>(frame_number) > frames.size()) return nullptr;
  const HudFrame& found = frames[static_cast<size_t>(frame_number - 1)];
  return found.w > 0 && found.h > 0 ? &found : nullptr;
}

}  // namespace

const HudFrame* HudArt::frame(int type, int frame_number) const { return sheet_frame(sheets, type, frame_number); }

const HudFrame* ResultsArt::frame(int type, int frame_number) const { return sheet_frame(sheets, type, frame_number); }

std::optional<HudArt> load_battle_hud(ByteSpan prd, ByteSpan prg, bool usa) {
  const Bytes tana = nested_pack(prd, "tana");
  if (tana.empty()) return std::nullopt;
  HudArt art;
  Vram vram;
  const NamedTim named[] = {{"win1", &art.skin}, {"numf", &art.digits}};
  load_tims(prd, vram, named);
  compose_sheets(tana, vram, kHudSheetNames, art.sheets);
  for (size_t frame = 0; frame < art.name_advance.size(); ++frame) {
    u8 advance = kJapanNameAdvance;
    if (usa) {
      const size_t at = kUsaNameAdvanceOffset + frame;
      advance = frame < kUsaFirstWideFrame ? kUsaNarrowAdvance : at < prg.size() ? prg[at] : kUsaNarrowAdvance;
    }
    art.name_advance[frame] = advance;
  }
  return art;
}

std::optional<ResultsArt> load_results_art(ByteSpan prd) {
  const Bytes tana = nested_pack(prd, "tana");
  if (tana.empty()) return std::nullopt;
  ResultsArt art;
  Vram vram;
  const NamedTim named[] = {{"bg", &art.wall}};
  load_tims(prd, vram, named);
  compose_sheets(tana, vram, kResultsSheetNames, art.sheets);
  return art;
}

std::optional<Bytes> load_exit_member(ByteSpan exdat, u32 index) {
  const SpanReader pack(exdat);
  if (exdat.size() < 4 + 4 * (static_cast<size_t>(index) + 2) || pack.at<u32>(0) <= index) return std::nullopt;
  const u32 begin = pack.at<u32>(4 + 4 * index), end = pack.at<u32>(4 + 4 * (index + 1));
  if (begin >= end || end > exdat.size()) return std::nullopt;
  return lzss_decode(exdat.subspan(begin, end - begin));
}

std::vector<Bytes> load_exit_messages(ByteSpan exdat, bool pairs) {
  constexpr size_t kEntry = 8;
  std::vector<Bytes> out;
  auto block = load_exit_member(exdat, 3);
  if (!block || block->size() < kEntry) return out;
  const SpanReader table(*block);
  const size_t count = std::min<size_t>(table.at<u16>(0) / kEntry, block->size() / kEntry);
  for (size_t i = 0; i < count; ++i) {
    Bytes text;
    for (size_t at = table.at<u16>(i * kEntry); at < block->size() && (*block)[at] >= 0x20; ++at) {
      text.push_back((*block)[at]);
      if (pairs && (*block)[at] >= 0xF8 && at + 1 < block->size()) text.push_back((*block)[++at]);
    }
    out.push_back(std::move(text));
  }
  return out;
}

std::vector<u8> load_exit_item_icons(ByteSpan exdat) {
  constexpr size_t kItem = 12, kIcon = 10;
  std::vector<u8> out;
  if (auto block = load_exit_member(exdat, 5))
    for (size_t at = 0; at + kItem <= block->size(); at += kItem) out.push_back(static_cast<u8>((*block)[at + kIcon] & 0x0F));
  return out;
}

}  // namespace oc::import
