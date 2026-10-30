// BattleView HUD: loading the HUD art and building the queued Op list.
#include "game/battle/battle_view.h"
#include "game/free_cam.h"
#include "game/battle/battle_view_util.h"
#include "game/battle/battle_view_op.h"

#include <algorithm>
#include <cstdlib>
#include <format>

#include "core/log.h"
#include "import/container.h"
#include "import/lzss.h"
#include "import/room_effects.h"
#include "import/text.h"
#include "import/tim.h"
namespace oc::game {

using namespace bv;

// ------------------------------------------------------------------------------------------------------------ HUD art
namespace {

// 8-bit palettised BMP -> grey mask ((r+g+b)/3 of the palette entry, like the RDE field ring loader).
bool decode_bmp_mask(ByteSpan data, std::vector<u8>& out, int w, int h) {
  if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') return false;
  auto rd32 = [&](size_t offset) { return static_cast<u32>(data[offset]) | static_cast<u32>(data[offset + 1]) << 8 | static_cast<u32>(data[offset + 2]) << 16 | static_cast<u32>(data[offset + 3]) << 24; };
  const u32 off = rd32(10);
  const int bw = static_cast<int>(rd32(18)), bh = static_cast<int>(rd32(22));
  if (bw != w || std::abs(bh) != h || data[28] != 8) return false;
  const size_t rs = (static_cast<size_t>(w) + 3) & ~size_t{3};
  if (off + rs * static_cast<size_t>(h) > data.size() || 54 + 1024 > data.size()) return false;
  out.assign(static_cast<size_t>(w) * h, 0);
  for (int y = 0; y < h; ++y) {
    const int sy = bh > 0 ? h - 1 - y : y;
    for (int x = 0; x < w; ++x) {
      const u8 idx = data[off + static_cast<size_t>(sy) * rs + static_cast<size_t>(x)];
      const size_t entry = 54 + 4 * static_cast<size_t>(idx);
      out[static_cast<size_t>(y) * w + x] = static_cast<u8>((data[entry] + data[entry + 1] + data[entry + 2]) / 3);
    }
  }
  return true;
}

// 24-bit BMP -> RGB rows, top row first.
bool decode_bmp_rgb(ByteSpan data, std::vector<u8>& out, int w, int h) {
  if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') return false;
  auto rd32 = [&](size_t offset) { return static_cast<u32>(data[offset]) | static_cast<u32>(data[offset + 1]) << 8 | static_cast<u32>(data[offset + 2]) << 16 | static_cast<u32>(data[offset + 3]) << 24; };
  const u32 off = rd32(10);
  const int bw = static_cast<int>(rd32(18)), bh = static_cast<int>(rd32(22));
  if (bw != w || std::abs(bh) != h || data[28] != 24) return false;
  const size_t rs = (static_cast<size_t>(w) * 3 + 3) & ~size_t{3};
  if (off + rs * static_cast<size_t>(h) > data.size()) return false;
  out.assign(static_cast<size_t>(w) * h * 3, 0);
  for (int y = 0; y < h; ++y) {
    const int sy = bh > 0 ? h - 1 - y : y;
    for (int x = 0; x < w; ++x) {
      const size_t from = off + static_cast<size_t>(sy) * rs + static_cast<size_t>(x) * 3, to = (static_cast<size_t>(y) * w + x) * 3;
      out[to] = data[from + 2];
      out[to + 1] = data[from + 1];
      out[to + 2] = data[from];
    }
  }
  return true;
}

void dxt_colors(u16 c0, u16 c1, u8 out[4][4]) {
  auto ex = [](u16 packed, u8* rgb) {
    rgb[0] = static_cast<u8>(((packed >> 11) & 31) * 255 / 31);
    rgb[1] = static_cast<u8>(((packed >> 5) & 63) * 255 / 63);
    rgb[2] = static_cast<u8>((packed & 31) * 255 / 31);
  };
  ex(c0, out[0]);
  ex(c1, out[1]);
  for (int i = 0; i < 3; ++i) {
    out[2][i] = static_cast<u8>((2 * out[0][i] + out[1][i]) / 3);
    out[3][i] = static_cast<u8>((out[0][i] + 2 * out[1][i]) / 3);
  }
}

// DXT5 DDS (top mip) -> RGBA.
bool decode_dds_dxt5(ByteSpan data, render::DecodedImage& img) {
  if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0 || std::memcmp(data.data() + 84, "DXT5", 4) != 0) return false;
  auto rd32 = [&](size_t offset) { return static_cast<u32>(data[offset]) | static_cast<u32>(data[offset + 1]) << 8 | static_cast<u32>(data[offset + 2]) << 16 | static_cast<u32>(data[offset + 3]) << 24; };
  const u32 h = rd32(12), w = rd32(16);
  const u32 bx = (w + 3) / 4, by = (h + 3) / 4;
  if (128 + static_cast<size_t>(bx) * by * 16 > data.size()) return false;
  img.width = w;
  img.height = h;
  img.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
  for (u32 yb = 0; yb < by; ++yb)
    for (u32 xb = 0; xb < bx; ++xb) {
      const u8* block = data.data() + 128 + (static_cast<size_t>(yb) * bx + xb) * 16;
      u8 alpha[8] = {block[0], block[1]};
      if (alpha[0] > alpha[1]) {
        for (int i = 1; i < 7; ++i) alpha[1 + i] = static_cast<u8>(((7 - i) * alpha[0] + i * alpha[1]) / 7);
      } else {
        for (int i = 1; i < 5; ++i) alpha[1 + i] = static_cast<u8>(((5 - i) * alpha[0] + i * alpha[1]) / 5);
        alpha[6] = 0;
        alpha[7] = 255;
      }
      u64 abits = 0;
      for (int i = 0; i < 6; ++i) abits |= static_cast<u64>(block[2 + i]) << (8 * i);
      u8 palette[4][4];
      dxt_colors(static_cast<u16>(block[8] | block[9] << 8), static_cast<u16>(block[10] | block[11] << 8), palette);
      const u32 cb = static_cast<u32>(block[12]) | static_cast<u32>(block[13]) << 8 | static_cast<u32>(block[14]) << 16 | static_cast<u32>(block[15]) << 24;
      for (int py = 0; py < 4; ++py)
        for (int px = 0; px < 4; ++px) {
          const u32 x = xb * 4 + static_cast<u32>(px), y = yb * 4 + static_cast<u32>(py);
          if (x >= w || y >= h) continue;
          const int texel = py * 4 + px;
          u8* dst = &img.rgba[(static_cast<size_t>(y) * w + x) * 4];
          const u8* cc = palette[(cb >> (2 * texel)) & 3];
          dst[0] = cc[0];
          dst[1] = cc[1];
          dst[2] = cc[2];
          dst[3] = alpha[(abits >> (3 * texel)) & 7];
        }
    }
  return true;
}

// Every TIM of a drp and of its nested drps onto `vram`, in registration order.
void blit_drp_tims(ByteSpan data, import::Vram& vram, int depth = 0) {
  auto drp = import::parse_drp(data);
  if (!drp) return;
  for (const auto& entry : drp->entries) {
    if (entry.type == 1 && depth < 4 && import::is_drp(entry.data)) blit_drp_tims(entry.data, vram, depth + 1);
    if (entry.type != 4) continue;
    ByteSpan raw = entry.data;
    Bytes decoded;
    if (import::is_lzss(raw)) {
      auto unpacked = import::lzss_decode(raw);
      if (!unpacked) continue;
      decoded = std::move(*unpacked);
      raw = decoded;
    }
    if (auto tim = import::parse_tim(raw)) vram.blit_tim(*tim);
  }
}

}  // namespace

// Defined in battle_view_ui.cpp: the panel art's edge fixes.
void battle_waku_prepare(render::DecodedImage& img, bool frame);

void BattleView::load_hud_art(const vfs::Vfs& vfs) {
  auto upload = [&](Tex& tex, const render::DecodedImage& img) {
    if (!tex.tex.create(*ctx_, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data())) return false;
    tex.set = render::TexRef(tex.tex, linear_);
    tex.w = static_cast<float>(img.width);
    tex.h = static_cast<float>(img.height);
    return static_cast<bool>(tex.set);
  };
  // prep: 0 / 1 = battle_waku_prepare for the waku base / frame.
  auto png = [&](Tex& tex, const char* path, int prep = -1) {
    auto file = vfs.read(path);
    render::DecodedImage img;
    if (!file || !render::decode_image(*file, img)) {
      log::warn("battle: HUD art {} missing", path);
      return;
    }
    if (prep >= 0) battle_waku_prepare(img, prep == 1);
    if (!upload(tex, img)) log::warn("battle: HUD art {} missing", path);
  };
  png(hud_[kHWakuBase], "hd/battle/shapes/battle-waku-99.png", 0);
  png(hud_[kHWaku], "hd/battle/shapes/battle-waku-01.png");
  png(hud_[kHWakuAct], "hd/battle/shapes/battle-waku-02.png", 1);
  png(hud_[kHGg1], "hd/battle/shapes/battle-gg-1.png");
  png(hud_[kHGg2], "hd/battle/shapes/battle-gg-2.png");
  png(hud_[kHField], "hd/battle/battle/battle_28_01.png");
  // HIT% caption leader line; 13 x 4 button glyphs (the camera hint uses Y); camera icon of the NORMAL / DYNAMIC hint (12 x 12 PS1 px);
  // element list plate (subtractive base + frame, 152 x 56); target info window (subtractive, 96 x 48); item name plate.
  png(hud_[kHOsak], "hd/battle/shapes/battle-osak-010.png");
  png(hud_[kHPad], "ui/gamepad.png");
  png(hud_[kHCam], "hd/menu/menutim/camera.png");
  png(hud_[kHWin4Base], "hd/battle/shapes/battle-win4-large-base.png");
  png(hud_[kHWin4Frame], "hd/battle/shapes/battle-win4-large-frame.png");
  png(hud_[kHWaku03], "hd/battle/shapes/battle-waku-03.png");
  png(eplate_item_, "hd/battle/battle/battle_08_93.png");
  {
    // Element-list name plates, T = {10, 5, 13, 7, 3, 12, 87}; frames 1..3 = the steady ones, 4..10 are the slide-in.
    static const int kT[7] = {10, 5, 13, 7, 3, 12, 87};
    for (int used = 0; used < 2; ++used)
      for (int colour = 0; colour < 7; ++colour)
        for (int frame = 0; frame < 10; ++frame)
          png(eplate_[static_cast<size_t>(used * 70 + colour * 10 + frame)], std::format("hd/battle/battle/battle_{:02d}_{:02d}_{:02d}.png", used ? 76 : 8, kT[colour], frame + 1).c_str());
  }
  // Window skin 'win1': 256 x 64 4bpp = 64 x 16 PS1 px, STP texels subtract, the others are opaque.
  if (auto file = vfs.read("hd/battle/battle/battle_04.tim")) {
    if (auto tim = import::parse_tim(*file))
      if (auto idx = import::decode_tim(*tim, 0, true)) {
        const import::Image rgba = import::to_rgba(*idx);
        render::DecodedImage semi, opq;
        semi.width = opq.width = rgba.width;
        semi.height = opq.height = rgba.height;
        semi.rgba.assign(rgba.pixels.size(), 0);
        opq.rgba.assign(rgba.pixels.size(), 0);
        for (size_t i = 0; i + 3 < rgba.pixels.size(); i += 4) {
          const u8 a = rgba.pixels[i + 3];
          if (a == 0) continue;
          std::vector<u8>& dst = a == import::kStpAlpha ? semi.rgba : opq.rgba;
          dst[i] = rgba.pixels[i];
          dst[i + 1] = rgba.pixels[i + 1];
          dst[i + 2] = rgba.pixels[i + 2];
          dst[i + 3] = 255;
        }
        upload(hud_[kHSkinSemi], semi);
        upload(hud_[kHSkinOpaque], opq);
      }
  }
  png(hud_[kHDiamond], "hd/battle/battle/battle_11.png");
  // Chrono Cross colour row dots, frames 1..7.
  static constexpr std::array<int, 7> kColourDotArt = {98, 96, 95, 100, 99, 101, 97};
  for (size_t frame = 0; frame < kColourDotArt.size(); ++frame)
    png(hud_[kHColourDot + frame], std::format("hd/battle/battle/battle_{}.png", kColourDotArt[frame]).c_str());
  png(hud_[kHPageCur], "hd/battle/battle/battle_86.png");
  // Party-panel status icons: a 3 x 2 grid, 6 x 5 PS1 px each.
  png(hud_[kHStatus], "hd/battle/battle/battle_34.png");
  {
    // Keyboard key glyphs: ui/keyboard_<e,f,i,g,s,j>.png, 16 x 7 cells.
    static const char* kKeyAtlas[6] = {"e", "f", "i", "g", "s", "j"};
    png(hud_[kHKeys], (std::string("ui/keyboard_") + kKeyAtlas[std::clamp(static_cast<int>(lang_), 0, 5)] + ".png").c_str());
  }
  // Target cursor: battle_21.dds (4 frames of 30x23 PS1 px at 4x).
  if (auto file = vfs.read("hd/battle/battle/battle_21.dds")) {
    render::DecodedImage img;
    if (!decode_dds_dxt5(*file, img) || !upload(hud_[kHCursor], img)) log::warn("battle: battle_21.dds not decoded");
  }
  // Damage digits + "miss!": battle_33_<lang>.tim (fr / it / de / es), else battle_33.tim (8bpp indexed 576x60, CLUT row 0) in both art modes. Digits are proportional: cut at the ink ranges.
  static const char* kNumfLang[6] = {"en", "fr", "it", "de", "es", "jp"};
  auto numf = vfs.read(std::format("hd/battle/battle/battle_33_{}.tim", kNumfLang[std::clamp(static_cast<int>(lang_), 0, 5)]));
  if (!numf) numf = vfs.read("hd/battle/battle/battle_33.tim");
  if (auto& numf_file = numf) {
    if (auto tim = import::parse_tim(*numf_file))
      if (auto idx = import::decode_tim(*tim, 0)) {
        const import::Image rgba = import::to_rgba(*idx);
        render::DecodedImage img;
        img.width = rgba.width;
        img.height = rgba.height;
        img.rgba.assign(rgba.pixels.begin(), rgba.pixels.end());
        if (upload(hud_[kHDigits], img)) {
          digit_x_.clear();
          int start = -1;
          for (u32 x = 0; x <= img.width; ++x) {
            bool ink = false;
            if (x < img.width)
              for (u32 y = 0; y < img.height && !ink; ++y) ink = img.rgba[(static_cast<size_t>(y) * img.width + x) * 4 + 3] != 0;
            if (ink && start < 0) start = static_cast<int>(x);
            if (!ink && start >= 0) {
              digit_x_.emplace_back(start, static_cast<int>(x) - 1);
              start = -1;
            }
          }
          if (digit_x_.size() != 11) {
            log::warn("battle: damage digit sheet has {} glyph runs (expected 11)", digit_x_.size());
            digit_x_.clear();
          }
        }
      }
  }
  // Field ring masks.
  ring_masks_.assign(12 * 208 * 112, 0);
  for (int i = 0; i < 12; ++i) {
    std::vector<u8> mask;
    auto file = vfs.read(std::format("hd/battle/elementfield/a{:02}.bmp", i + 1));
    if (!file || !decode_bmp_mask(*file, mask, 208, 112)) {
      log::warn("battle: field ring mask a{:02}.bmp missing", i + 1);
      continue;
    }
    std::copy(mask.begin(), mask.end(), ring_masks_.begin() + static_cast<ptrdiff_t>(i) * 208 * 112);
  }
  ring_rainbow_.clear();
  auto rainbow = vfs.read("hd/battle/elementfield/battle_88.bmp");
  if (!rainbow || !decode_bmp_rgb(*rainbow, ring_rainbow_, 208, 112)) {
    ring_rainbow_.clear();
    log::warn("battle: field ring picture battle_88.bmp missing");
  }
  // Element-trap colour marks, both art modes: the disc's battle sprite sheet 'etra' (battle.prd > nested pack 'tana'), groups 3..8 = red,
  // green, blue, yellow, white, black, one 5 x 5 part each, cut from the battle VRAM.
  for (Tex& mark : trap_marks_) {
    mark.set = {};
    mark.tex.destroy();
  }
  if (auto prd = vfs.read("cdrom/battle/battle.prd")) {
    import::Vram vram;
    blit_drp_tims(*prd, vram);
    const Bytes tana = import::nested_pack(*prd, "tana");
    const assets::RoomEffects pack = import::load_battle_effects(tana, {}, vram);
    static constexpr std::array<int, 6> kTrapMarkGroup = {4, 7, 8, 3, 6, 5};
    for (const assets::SpriteSheet& sheet : pack.sheets) {
      if (std::string_view(sheet.name.c_str()) != "etra") continue;
      for (size_t colour = 0; colour < kTrapMarkGroup.size(); ++colour) {
        const size_t group = static_cast<size_t>(kTrapMarkGroup[colour]) - 1;
        if (group >= sheet.frames.size() || sheet.frames[group].empty()) continue;
        const assets::EffectSprite& part = sheet.frames[group][0];
        if (part.rgba.size() != static_cast<size_t>(part.w) * part.h * 4) continue;
        Tex& mark = trap_marks_[colour];
        if (!mark.tex.create(*ctx_, part.w, part.h, render::TextureFormat::Rgba8, part.rgba.data())) continue;
        mark.set = render::TexRef(mark.tex, nearest_);
        mark.w = static_cast<float>(part.w);
        mark.h = static_cast<float>(part.h);
      }
    }
  }
  if (!trap_marks_[0].set) log::warn("battle: element trap sheet 'etra' missing");
}

// The ring the HUD walks toward: the one before the action being presented (or the next queued one), else the rules' ring once the replay has
// caught up.
std::array<int, 3> BattleView::presented_ring() const {
  if (act_.phase != Act::Phase::None && !act_.ev.empty()) return act_.ev[std::min(act_.hit, act_.ev.size() - 1)].ring;
  if (!queue_.empty() && !queue_.front().empty()) return queue_.front().front().ring;
  return battle_->ring();
}

// The drawn ring is a 12-cell queue, 4 cells per slot. When the presented ring changes, new cells enter at the newest end one per 2 ticks
// while the oldest leave: all 12 after a repaint, else the fewest whole slots that reach the new ring (4 for a cast's colour). The intro and a
// new battle set the cells at once.
void BattleView::step_ring() {
  const std::array<int, 3> target = presented_ring();
  std::array<int, 12> want{};
  for (int cell = 0; cell < 12; ++cell) want[static_cast<size_t>(cell)] = target[static_cast<size_t>(cell / 4)];
  if (!ring_cells_set_ || ui_ == Ui::Intro) {
    ring_cells_ = want;
    ring_key_.fill(-2);
    ring_cells_set_ = true;
    ring_feed_count_ = 0;
    ring_tick_ = tick_count_;
    ring_repaint_seen_ = false;
    return;
  }
  if (act_.phase != Act::Phase::None)
    for (const battle::ActionEvent& event : act_.ev) ring_repaint_seen_ = ring_repaint_seen_ || event.ring_repaint;
  if (ring_feed_count_ == 0 && ring_cells_ != want) {
    int shift = 4;
    if (ring_repaint_seen_) shift = 12;
    for (; shift < 12; shift += 4)
      if (std::equal(ring_cells_.begin() + shift, ring_cells_.end(), want.begin())) break;
    ring_feed_count_ = shift;
    std::copy(want.end() - shift, want.end(), ring_feed_.begin());
    ring_tick_ = tick_count_ - 2;
    ring_repaint_seen_ = false;  } else if (ring_feed_count_ == 0 && act_.phase == Act::Phase::None) {
    ring_repaint_seen_ = false;
  }
  while (ring_feed_count_ > 0 && tick_count_ - ring_tick_ >= 2) {
    std::copy(ring_cells_.begin() + 1, ring_cells_.end(), ring_cells_.begin());
    ring_cells_[11] = ring_feed_[0];
    std::copy(ring_feed_.begin() + 1, ring_feed_.end(), ring_feed_.begin());
    --ring_feed_count_;
    ring_tick_ += 2;
  }
}

bool BattleView::ring_settled() const {
  if (!ring_cells_set_) return true;
  if (ring_feed_count_ > 0) return false;
  const std::array<int, 3> target = presented_ring();
  for (int cell = 0; cell < 12; ++cell)
    if (ring_cells_[static_cast<size_t>(cell)] != target[static_cast<size_t>(cell / 4)]) return false;
  return true;
}

// The field ring: 12 cells filled with their colour at alpha 0x80 like RDE's 52x28 composite (PS1 rect (7,10)).
void BattleView::rebuild_ring() {
  if (ring_cells_ == ring_key_) return;
  ring_key_ = ring_cells_;
  Tex& ring_tex = hud_[kHRing];
  ring_tex.set = {};
  ring_tex.tex.destroy();
  if (ring_masks_.size() != 12u * 208 * 112) return;
  // Cells 0..11 in order, 4 crescent shells per ellipse (a01..a04 outer, a05..a08 middle, a09..a12 inner). A cell with a colour
  // OVERWRITES its mask (m >= 0x80: colour at alpha 0x80; 0 < m < 0x80: rgb += (C - rgb) * m / 128 over a covered texel, C over a clear one,
  // alpha = max(m, alpha)); a cell with no colour (a cast with no colour byte) punches its mask out of what was drawn before. Colours: green,
  // white, black, red, yellow, blue; an all-colours slot takes each texel's colour from battle_88.bmp.
  static const Rgb kRing[6] = {{0, 255, 0}, {255, 255, 255}, {0, 0, 0}, {255, 0, 0}, {255, 255, 0}, {0, 0, 255}};
  std::vector<u8> px(208 * 112 * 4, 0);
  bool any = false;
  for (int cell = 0; cell < 12; ++cell) {
    const int colour = ring_cells_[static_cast<size_t>(cell)];
    const bool rainbow = colour == battle::kRingAllColours && !ring_rainbow_.empty();
    const u8* mask = &ring_masks_[static_cast<size_t>(cell) * 208 * 112];
    if ((colour < 0 || colour >= 6) && !rainbow) {
      if (!any) continue;
      for (int i = 0; i < 208 * 112; ++i) {
        if (mask[i] == 0) continue;
        u8* texel = &px[static_cast<size_t>(i) * 4];
        if (mask[i] >= 0x80) texel[3] = 0;
        else texel[3] = static_cast<u8>(texel[3] * (0x80 - mask[i]) / 0x80);
      }
      continue;
    }
    any = true;
    for (int i = 0; i < 208 * 112; ++i) {
      if (mask[i] == 0) continue;
      u8* texel = &px[static_cast<size_t>(i) * 4];
      Rgb rgb = rainbow ? Rgb{} : kRing[colour];
      if (rainbow) {
        const u8* picture = &ring_rainbow_[static_cast<size_t>(i) * 3];
        rgb.r = picture[0], rgb.g = picture[1], rgb.b = picture[2];
      }
      if (mask[i] >= 0x80) {
        texel[0] = rgb.r, texel[1] = rgb.g, texel[2] = rgb.b, texel[3] = 0x80;
      } else if (texel[3] == 0) {
        texel[0] = rgb.r, texel[1] = rgb.g, texel[2] = rgb.b, texel[3] = mask[i];
      } else {
        texel[0] = static_cast<u8>(texel[0] + (rgb.r - texel[0]) * mask[i] / 128);
        texel[1] = static_cast<u8>(texel[1] + (rgb.g - texel[1]) * mask[i] / 128);
        texel[2] = static_cast<u8>(texel[2] + (rgb.b - texel[2]) * mask[i] / 128);
        texel[3] = std::max<u8>(texel[3], mask[i]);
      }
    }
  }
  if (!ring_tex.tex.create(*ctx_, 208, 112, render::TextureFormat::Rgba8, px.data())) return;
  ring_tex.set = render::TexRef(ring_tex.tex, linear_);
  ring_tex.w = 208;
  ring_tex.h = 112;
}

// -------------------------------------------------------------------------------------------------------------- HUD
void BattleView::add_text(const std::string& text, float x, float y, u32 color, int align, float scale_mul) {
  Op op;
  op.kind = Op::Text;
  op.x = x;
  op.y = y;
  op.text = import::utf8_to_u32(text);
  op.slot = ui::font::kBattle;
  op.ink = {color, col(33, 33, 33, static_cast<u8>((color >> 24) & 0xFF)), true, false};
  op.scale = 0.42f * scale_mul;
  op.align = align;
  fonts_.prepare(op.slot, op.text);
  ops_.push_back(std::move(op));
}

void BattleView::add_text_op(int slot, std::u32string text, float x, float y, const ui::font::Ink& ink, float scale, int align) {
  Op op;
  op.kind = Op::Text;
  op.x = x;
  op.y = y;
  op.text = std::move(text);
  op.slot = slot;
  op.ink = ink;
  op.scale = scale;
  op.align = align;
  ops_.push_back(std::move(op));
}

void BattleView::add_img(const Tex& tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 tint, int blend) {
  if (!tex.set) return;
  Op op;
  op.kind = Op::Img;
  op.tex = &tex;
  op.x = x;
  op.y = y;
  op.w = w;
  op.h = h;
  op.u0 = u0;
  op.v0 = v0;
  op.u1 = u1;
  op.v1 = v1;
  op.color = tint;
  op.blend = blend;
  ops_.push_back(std::move(op));
}

void BattleView::add_rect(float x, float y, float w, float h, u32 color) {
  Op op;
  op.kind = Op::Rect;
  op.x = x;
  op.y = y;
  op.w = w;
  op.h = h;
  op.color = color;
  ops_.push_back(std::move(op));
}

size_t BattleView::op_count() const { return ops_.size(); }

void BattleView::set_group(size_t from, int grp, float shift) {
  for (size_t index = from; index < ops_.size(); ++index) {
    ops_[index].grp = grp;
    ops_[index].shift = shift;
  }
}

void BattleView::add_panel(float x, float y, float w, float h) {
  Op op;
  op.kind = Op::Panel;
  op.x = x;
  op.y = y;
  op.w = w;
  op.h = h;
  ops_.push_back(std::move(op));
}

// The PS1-drawn battle window (command box, target info): both art modes draw it as primitives (no waku shape): near-black fill, a 1 px
// white line with a 6 px chamfer at the top-right and bottom-left corners.
void BattleView::add_box(float x, float y, float w, float h, bool fill_on) {
  // Chamfer in PS1 px.
  constexpr int kC = 6;
  const float chamfer = kC * kPs1Px, line_w = 1 * kPs1Px;
  const u32 fill = col(0, 0, 0, 232), line = kWhite;
  // Fill (non-overlapping pieces: the fill is translucent): the middle band, the top band left of the chamfer, the bottom band right of the
  // chamfer, and per-pixel rows under the top-right diagonal and right of the bottom-left diagonal.
  if (fill_on) {
    add_rect(x, y + chamfer, w, h - 2 * chamfer, fill);
    add_rect(x, y, w - chamfer, chamfer, fill);
    add_rect(x + chamfer, y + h - chamfer, w - chamfer, chamfer, fill);
    for (int k = 0; k < kC; ++k) {
      const float step = static_cast<float>(k) * kPs1Px;
      if (k > 0) add_rect(x + w - chamfer, y + step, step, kPs1Px, fill);
      if (k < kC - 1) add_rect(x + step + kPs1Px, y + h - chamfer + step, static_cast<float>(kC - 1 - k) * kPs1Px, kPs1Px, fill);
    }
  }
  // 1 px line: top, left, right, bottom, then the diagonals in 1 px steps (top-right: (w-c, 0) down to (w, c); bottom-left: (0, h-c) down to (c, h)).
  add_rect(x, y, w - chamfer, line_w, line);
  add_rect(x, y, line_w, h - chamfer, line);
  add_rect(x + w - line_w, y + chamfer, line_w, h - chamfer, line);
  add_rect(x + chamfer, y + h - line_w, w - chamfer, line_w, line);
  for (int k = 0; k < kC; ++k) {
    const float step = static_cast<float>(k) * kPs1Px;
    add_rect(x + w - chamfer + step, y + step, kPs1Px, kPs1Px, line);
    add_rect(x + step, y + h - chamfer + step, kPs1Px, kPs1Px, line);
  }
}

void BattleView::add_plate(int colour_index, float x, float y, float w, float h, u32 tint) {
  Op op;
  op.kind = Op::Plate;
  op.x = x;
  op.y = y;
  op.w = w;
  op.h = h;
  op.color = tint;
  op.plate = colour_index >= 0 && colour_index < 6 ? kPlateOfColour[colour_index] - 1 : 0;
  ops_.push_back(std::move(op));
}

void BattleView::add_bar(float x, float y, float w, float h, float frac, u32 fill) {
  frac = std::clamp(frac, 0.0f, 1.0f);
  add_rect(x - 2, y - 2, w + 4, h + 4, col(0, 0, 0, 200));
  add_rect(x, y, w, h, col(40, 40, 50));
  if (frac > 0) add_rect(x, y, w * frac, h, fill);
}

// Field ring: the composited 52x28 image at PS1 (7, 10) under the white battle_28_01 frame.
void BattleView::build_field_ring() {
  rebuild_ring();
  add_img(hud_[kHRing], 7 * kPs1Px, 10 * kPs1Px, 52 * kPs1Px, 28 * kPs1Px);
  add_img(hud_[kHField], 7 * kPs1Px, 10 * kPs1Px, 52 * kPs1Px, 28 * kPs1Px);
  // Caption: "FIELD EFFECT" per language, flags 0x20086 (style 0x86: gn x 0.65, quarter shrink, H2), colour 0x757575, at PS1 (60, 23) in
  // 240-line units.
  static const char32_t* kCaption[6] = {U"FIELD EFFECT", U"EFFET DE CHAMP", U"EFFETTO DI CAMPO", U"FELDEFFEKT", U"AMBIENTE ELEMENTAL", U"FiELD EFFECT"};
  btext(0x20086, kCaption[std::clamp(static_cast<int>(lang_), 0, 5)], 60, 23 * kPs1Px / (kCanvasH / 216.0f), 0x757575);
  build_element_trap();
}

// The element-trap block of the presented action: the one before it while it is queued, its own while it runs, else the rules' block.
battle::TrapHud BattleView::presented_trap() const {
  if (act_.phase != Act::Phase::None && !act_.ev.empty()) return act_.ev[std::min(act_.hit, act_.ev.size() - 1)].trap;
  if (!queue_.empty() && !queue_.front().empty()) return queue_.front().front().trap_before;
  return battle_->trap_hud();
}

// Element-trap block under the field ring, drawn from PS1 (8, 24) like the ring: "ELEMENT TRAP" at (16, 43); "PLAYER-" at (62, 39) with the
// colour mark of the element the party trapped (or "NO") at (88, 39); "ENEMY-" at (62, 46) with "?" while the enemies hold a trap (or
// "NO") at (88, 46). Texts per language from cmnmes (style 3, colour 0x757575); the two row labels move left 8 px in Italian, 4 in Spanish.
void BattleView::build_element_trap() {
  const battle::TrapHud trap = presented_trap();
  if (!trap.shown) return;
  constexpr int kStyle = 0x20003;
  constexpr u32 kColour = 0x757575;
  float label_x = 62;
  if (lang_ == import::Lang::It) label_x -= 8;
  else if (lang_ == import::Lang::Es) label_x -= 4;
  btext(kStyle, xlat("ELEMENT TRAP", U"ELEMENT TRAP"), 16, 43, kColour);
  btext(kStyle, xlat("PLAYER-", U"PLAYER-"), label_x, 39, kColour);
  btext(kStyle, xlat("ENEMY-", U"ENEMY-"), label_x, 46, kColour);
  if (trap.pc == 0) {
    btext(kStyle, xlat("NO", U"NO"), 88, 39, kColour);
  } else if (trap.pc > 0 && static_cast<size_t>(trap.pc) < tables_->elements.size()) {
    const int colour = data::colour_index_of_bit(static_cast<u8>(tables_->elements[static_cast<size_t>(trap.pc)].b(5) & 0xFC));
    if (colour >= 0) add_img(trap_marks_[static_cast<size_t>(colour)], 88 * kPs1Px, 39 * kCanvasH / 216.0f, 5 * kPs1Px, 5 * kCanvasH / 216.0f);
  }
  btext(kStyle, trap.enemy != 0 ? xlat("?", U"?") : xlat("NO", U"NO"), 88, 46, kColour);
}

// Chrono Cross colour row: while a party grid holds the Chrono Cross, the last seven cast colours as 16 x 16 dots at PS1 (0x68 + 16 i, 0x10).
// A history entry (colour + 1: green white black red yellow blue, 7 colourless) picks the dot frame.
void BattleView::build_colour_row() {
  if (!battle_->colour_row_shown()) return;
  static constexpr std::array<int, 8> kColourDotFrame = {0, 2, 5, 6, 1, 4, 3, 7};
  for (size_t i = 0; i < colour_row_.size(); ++i) {
    const int entry = colour_row_[i];
    if (entry <= 0 || entry >= static_cast<int>(kColourDotFrame.size())) continue;
    const int frame = kColourDotFrame[static_cast<size_t>(entry)];
    add_img(hud_[kHColourDot + static_cast<size_t>(frame - 1)], static_cast<float>(0x68 + 16 * i) * kPs1Px, 0x10 * kPs1Px, 16 * kPs1Px, 16 * kPs1Px);
  }
}

// Party status panels: BattleView::build_party_panels in battle_view_ui.cpp.

// The battle message window (PS1 window slot: text origin (32, 144), 16 px line pitch).
void BattleView::build_message_window(const std::vector<std::pair<std::u32string, u32>>& lines) {
  if (lines.empty()) return;
  // Window slot 1 text: style 0x83 (bn_70, H1 k 4) at the window text origin, 16 PS1 px per line, colour 0x757575.
  float wmax = 0;
  for (const auto& line : lines) {
    fonts_.prepare(ui::font::kBn, line.first);
    wmax = std::max(wmax, fonts_.measure(ui::font::kBn, line.first, ui::font::slot(ui::font::kBn).scale));
  }
  const float line_count = static_cast<float>(lines.size());
  add_panel(24 * kPs1Px, 138 * kPs1Px, std::min(std::max(wmax + 40, 300.0f), 1180.0f), 16 * kPs1Px * line_count + 6 * kPs1Px);
  for (size_t i = 0; i < lines.size(); ++i) {
    const size_t before = ops_.size();
    btext(0x83, lines[i].first, 32, 144 * kPs1Px / (kCanvasH / 216.0f) + 16.0f * static_cast<float>(i), 0x757575);
    // The log line's fade.
    for (size_t index = before; index < ops_.size(); ++index) {
      const u32 alpha = (lines[i].second >> 24) & 0xFF;
      ops_[index].ink.normal = (ops_[index].ink.normal & 0x00FFFFFFu) | (alpha << 24);
      ops_[index].ink.outline = (ops_[index].ink.outline & 0x00FFFFFFu) | (alpha << 24);
    }
  }
}

void BattleView::build_hud() {
  ops_.clear();
  const bool intro = ui_ == Ui::Intro;
  step_ring();

  // The whole HUD disappears the moment the party runs away and stays off through the settle frames after the run act.
  const bool running = act_.phase == Act::Phase::Run || finish_ == Finish::Fled || battle_->outcome() == battle::Outcome::Fled;
  const bool hud_off = hud_off_ || hud_script_off_;
  if (!intro && !running && !hud_off) {
    // The panels, the ring and the cast header are one block gated only by the intro / the pause flag; during a cast the ring, label and panels
    // all stay, and the plate lies over the ring.
    size_t g0 = ops_.size();
    build_field_ring();
    for (; g0 < ops_.size(); ++g0) ops_[g0].grp = -1;
    build_party_panels();
    for (g0 = 0; g0 < ops_.size(); ++g0)
      // Right group: 3-party panels end at PS1 x 300 -> 13 px more puts them 7 px (the ring's margin) from the right edge.
      if (ops_[g0].grp == 0) {
        ops_[g0].grp = 1;
        ops_[g0].shift = 13 * 4.0f;
      }
    build_colour_row();
  }

  if (!hud_off) {
    const size_t g1 = ops_.size();
    build_command_ui();
    for (size_t index = g1; index < ops_.size(); ++index)
      if (ops_[index].grp == 0) ops_[index].grp = -1;
  }

  // Attack results / log: the message window (unless a menu uses it for its own text).
  if (!log_.empty() && !intro && ui_ != Ui::Element && ui_ != Ui::Chain && ui_ != Ui::Result) {
    std::vector<std::pair<std::u32string, u32>> lines;
    const size_t first = log_.size() > 2 ? log_.size() - 2 : 0;
    for (size_t i = first; i < log_.size(); ++i)
      lines.emplace_back(log_[i].text, col(235, 235, 235, static_cast<u8>(255 * std::clamp(log_[i].ttl, 0.0f, 1.0f))));
    build_message_window(lines);
  }

  // The battle-script event window (E0).
  build_script_msg();
  if (ui_ == Ui::Result) build_result_ui();
  // Debug free camera, "Hide overlays", or Select held / [Enhancements] AutoHideCastHud while an element / tech / summon is charged or
  // released: no HUD / menus / windows / results (the F9 text below stays; the damage digits are drawn elsewhere).
  const bool casting = act_.phase == Act::Phase::Cast || act_.phase == Act::Phase::Release;
  if (free_cam().hide_overlays() || ((select_held_ || autohide_cast_) && casting)) ops_.clear();
  if (dbg_.overlay) build_camera_overlay();
}

// [Debug] camera overlay: active shot name, eye / target / H, and an axis gizmo at each unit (X red, Y up green, Z blue, white = facing).
void BattleView::build_camera_overlay() {
  const CamPose pose = camera_.pose(alpha_);
  add_rect(560, 110, 700, 66, col(0, 0, 0, 170));
  add_text(std::format("{}shot {} ({}/4096)  H {:.0f}  roll {:.1f}", camera_.is_free() ? "FREE  " : "", camera_.shot_name(), static_cast<int>(camera_.progress()), pose.h, pose.roll_deg), 568, 114, col(255, 255, 80), 0, 0.55f);
  add_text(std::format("eye {:.0f} {:.0f} {:.0f}  target {:.0f} {:.0f} {:.0f}", pose.eye[0], pose.eye[1], pose.eye[2], pose.target[0], pose.target[1], pose.target[2]), 568, 144, col(255, 255, 80), 0, 0.55f);
  auto line = [&](float x0, float y0, float z0, float x1, float y1, float z1, u32 colour) {
    float ax, ay, bx, by;
    if (!cam_.project(x0, y0, z0, ax, ay) || !cam_.project(x1, y1, z1, bx, by)) return;
    for (int i = 0; i <= 10; ++i) {
      const float frac = static_cast<float>(i) / 10.0f;
      add_rect(ax + (bx - ax) * frac - 3, ay + (by - ay) * frac - 3, 6, 6, colour);
    }
  };
  for (int i = 0; i < battle::kUnits; ++i) {
    if (!battle_->unit(i).present) continue;
    const Visual& visual = vis_[static_cast<size_t>(i)];
    const float axis_len = 500;
    line(visual.x, 0, visual.z, visual.x + axis_len, 0, visual.z, col(255, 60, 60));
    line(visual.x, 0, visual.z, visual.x, axis_len, visual.z, col(60, 255, 60));
    line(visual.x, 0, visual.z, visual.x, 0, visual.z + axis_len, col(70, 110, 255));
    line(visual.x, 0, visual.z, visual.x + visual.face_x * 2 * axis_len, 0, visual.z + visual.face_z * 2 * axis_len, col(255, 255, 255));
    float sx, sy;
    if (cam_.project(visual.x, 0, visual.z, sx, sy)) add_text(std::to_string(i), sx, sy + 6, col(255, 255, 255), 1, 0.5f);
  }
}

// ----------------------------------------------------------------------------------------------------------- drawing
void BattleView::nine_slice(render::TexRef set, float tw, float th, float x, float y, float w, float h, float cs, float cd, u32 color) {
  const float xs[4] = {0, cs, tw - cs, tw}, ys[4] = {0, cs, th - cs, th};
  const float xd[4] = {x, x + cd, x + w - cd, x + w}, yd[4] = {y, y + cd, y + h - cd, y + h};
  for (int j = 0; j < 3; ++j)
    for (int i = 0; i < 3; ++i)
      sprites_->draw(set, xd[i], yd[j], xd[i + 1] - xd[i], yd[j + 1] - yd[j], xs[i] / tw, ys[j] / th, xs[i + 1] / tw, ys[j + 1] / th, color);
}

}  // namespace oc::game
