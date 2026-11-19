// BattleView in disc mode: the PS1's own battle HUD art (import::HudArt, the battle.prd sheets) packed into atlases, and the HUD pieces
// drawn from it at the PS1's coordinates: the field ring with its label, trap block and star count, the party status panels and the
// target info window.
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"
#include "game/battle/hud_rules.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "core/log.h"
#include "import/tim.h"
#include "platform/config.h"

namespace oc::game {

using namespace bv;

namespace {

constexpr float kRowScale = kCanvasH / 216.0f;

// Sheet types (import::kHudSheetNames).
constexpr int kSheetFont = 0x04, kSheetGrid = 0x08, kSheetDigits = 0x10, kSheetPanel = 0x74, kSheetStatus = 0x78,
              kSheetRing = 0x80;

// The nine decimal digits of a value, most significant first; the leading zeros before the last digit are -1.
std::array<int, 9> split_digits(int value) {
  std::array<int, 9> digits{};
  unsigned rest = static_cast<unsigned>(std::max(value, 0));
  for (int i = 8; i >= 0; --i) {
    digits[static_cast<size_t>(i)] = static_cast<int>(rest % 10);
    rest /= 10;
  }
  for (size_t i = 0; i < 8 && digits[i] == 0; ++i) digits[i] = -1;
  return digits;
}

// `frame` scaled up by `factor` (nearest), keeping only the texels `keep` accepts (by alpha), made opaque.
template <class Keep>
render::DecodedImage upscaled(const import::HudFrame& frame, int factor, Keep keep) {
  render::DecodedImage img;
  img.width = static_cast<u32>(frame.w * factor);
  img.height = static_cast<u32>(frame.h * factor);
  img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 0);
  for (u32 y = 0; y < img.height; ++y)
    for (u32 x = 0; x < img.width; ++x) {
      const u8* texel = &frame.rgba[(static_cast<size_t>(y / static_cast<u32>(factor)) * static_cast<size_t>(frame.w) + x / static_cast<u32>(factor)) * 4];
      if (!keep(texel[3])) continue;
      u8* out = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
      out[0] = texel[0], out[1] = texel[1], out[2] = texel[2], out[3] = 255;
    }
  return img;
}

}  // namespace

void BattleView::load_ps1_hud(const vfs::Vfs& vfs) {
  sheet_frames_.clear();
  auto prd = vfs.read("cdrom/battle/battle.prd");
  auto prg = vfs.read("cdrom/prog/battle.prg");
  // The USA build's font width record is there only on its discs.
  auto widths = vfs.read("cdrom/#record/02059");
  const bool usa = widths && widths->size() == 256;
  std::optional<import::HudArt> art;
  if (prd) art = import::load_battle_hud(*prd, prg ? ByteSpan(*prg) : ByteSpan{}, usa);
  if (!art) {
    log::warn("battle: the disc's HUD sheets (battle.prd) are missing");
    return;
  }
  name_advance_ = art->name_advance;
  SheetAtlas atlas = pack_sheets(art->sheets);
  sheet_frames_ = std::move(atlas.frames);
  for (auto [tex, rgba] : {std::pair{&sheet_semi_, &atlas.semi}, std::pair{&sheet_opaque_, &atlas.opaque}}) {
    if (!tex->tex.create(*ctx_, static_cast<u32>(atlas.width), static_cast<u32>(atlas.height), render::TextureFormat::Rgba8, rgba->data())) continue;
    tex->set = render::TexRef(tex->tex, nearest_);
    tex->w = static_cast<float>(atlas.width);
    tex->h = static_cast<float>(atlas.height);
  }
  auto upload = [&](Tex& tex, const render::DecodedImage& img) {
    if (img.width == 0 || !tex.tex.create(*ctx_, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data())) return;
    tex.set = render::TexRef(tex.tex, nearest_);
    tex.w = static_cast<float>(img.width);
    tex.h = static_cast<float>(img.height);
  };
  // The art the shared layout draws as whole pictures, at RDE's 4 texels per PS1 pixel: the window skin (its semi-transparent and opaque
  // texels apart), the damage digits and the four target cursor frames side by side.
  constexpr int kHd = 4;
  if (art->skin.w > 0) {
    upload(hud_[kHSkinSemi], upscaled(art->skin, kHd, [](u8 a) { return a == import::kStpAlpha; }));
    upload(hud_[kHSkinOpaque], upscaled(art->skin, kHd, [](u8 a) { return a != 0 && a != import::kStpAlpha; }));
  }
  if (art->digits.w > 0) {
    const render::DecodedImage digits = upscaled(art->digits, kHd, [](u8 a) { return a != 0; });
    upload(hud_[kHDigits], digits);
    digit_x_ = column_ink_runs(digits.rgba, digits.width, digits.height);
    // The disc's "miss!" has its letters apart: every run after the ten digits belongs to that one glyph.
    if (digit_x_.size() > 11) {
      digit_x_[10].second = digit_x_.back().second;
      digit_x_.resize(11);
    }
    if (digit_x_.size() != 11) digit_x_.clear();
  }
  constexpr int kCursorFrames = 4;
  import::HudFrame cursor;
  for (int frame = 1; frame <= kCursorFrames; ++frame)
    if (const import::HudFrame* part = art->frame(0x14, frame)) cursor.w = std::max(cursor.w, part->w), cursor.h = std::max(cursor.h, part->h);
  if (cursor.w > 0) {
    const int cell_w = cursor.w;
    cursor.w *= kCursorFrames;
    cursor.rgba.assign(static_cast<size_t>(cursor.w) * static_cast<size_t>(cursor.h) * 4, 0);
    for (int frame = 1; frame <= kCursorFrames; ++frame)
      if (const import::HudFrame* part = art->frame(0x14, frame))
        for (int y = 0; y < part->h; ++y)
          std::copy_n(&part->rgba[static_cast<size_t>(y) * static_cast<size_t>(part->w) * 4], static_cast<size_t>(part->w) * 4,
                      &cursor.rgba[(static_cast<size_t>(y) * static_cast<size_t>(cursor.w) + static_cast<size_t>((frame - 1) * cell_w)) * 4]);
    upload(hud_[kHCursor], upscaled(cursor, kHd, [](u8 a) { return a != 0; }));
  }
}

void BattleView::add_sheet(int type, int frame, float x, float y, u32 colour, int mode) {
  const size_t sheet = static_cast<size_t>(type / 4);
  if (type < 0 || sheet >= sheet_frames_.size() || frame < 1 || static_cast<size_t>(frame) > sheet_frames_[sheet].size()) return;
  const SheetFrame& f = sheet_frames_[sheet][static_cast<size_t>(frame - 1)];
  if (f.w <= 0) return;
  // Texel x colour / 0x80.
  auto channel = [&](int shift, int divide) { return static_cast<u8>(std::min(255u, ((colour >> shift) & 0xFFu) * 2u / static_cast<u32>(divide))); };
  const u32 tint = col(channel(0, 1), channel(8, 1), channel(16, 1));
  const float cx = (x + static_cast<float>(f.x)) * kPs1Px, cy = (y + static_cast<float>(f.y)) * kRowScale;
  const float cw = static_cast<float>(f.w) * kPs1Px, ch = static_cast<float>(f.h) * kRowScale;
  switch (mode == 0 ? -1 : (mode - 1) & 3) {
    case 0: add_img(sheet_semi_, cx, cy, cw, ch, f.u0, f.v0, f.u1, f.v1, (tint & 0x00FFFFFFu) | 0x80000000u); break;
    case 1: add_img(sheet_semi_, cx, cy, cw, ch, f.u0, f.v0, f.u1, f.v1, tint, 2); break;
    case 2: add_img(sheet_semi_, cx, cy, cw, ch, f.u0, f.v0, f.u1, f.v1, tint, 3); break;
    case 3: add_img(sheet_semi_, cx, cy, cw, ch, f.u0, f.v0, f.u1, f.v1, col(channel(0, 4), channel(8, 4), channel(16, 4)), 2); break;
    default: add_img(sheet_semi_, cx, cy, cw, ch, f.u0, f.v0, f.u1, f.v1, tint); break;
  }
  add_img(sheet_opaque_, cx, cy, cw, ch, f.u0, f.v0, f.u1, f.v1, tint);
}

void BattleView::add_sheet_at(int type, int frame, float left, float top, u32 colour, int mode) {
  const size_t sheet = static_cast<size_t>(type / 4);
  if (type < 0 || sheet >= sheet_frames_.size() || frame < 1 || static_cast<size_t>(frame) > sheet_frames_[sheet].size()) return;
  const SheetFrame& f = sheet_frames_[sheet][static_cast<size_t>(frame - 1)];
  add_sheet(type, frame, left - static_cast<float>(f.x), top - static_cast<float>(f.y), colour, mode);
}

void BattleView::add_sheet_cut(int type, int frame, int dx, int dy, int w, int h, float left, float top) {
  const size_t sheet = static_cast<size_t>(type / 4);
  if (type < 0 || sheet >= sheet_frames_.size() || frame < 1 || static_cast<size_t>(frame) > sheet_frames_[sheet].size()) return;
  const SheetFrame& picture = sheet_frames_[sheet][static_cast<size_t>(frame - 1)];
  const int cut_x = std::max(dx - picture.x, 0), cut_y = std::max(dy - picture.y, 0);
  const int cut_w = std::min(w, picture.w - cut_x), cut_h = std::min(h, picture.h - cut_y);
  if (cut_w <= 0 || cut_h <= 0) return;
  const float du = (picture.u1 - picture.u0) / static_cast<float>(picture.w), dv = (picture.v1 - picture.v0) / static_cast<float>(picture.h);
  const float u0 = picture.u0 + du * static_cast<float>(cut_x), v0 = picture.v0 + dv * static_cast<float>(cut_y);
  const float u1 = u0 + du * static_cast<float>(cut_w), v1 = v0 + dv * static_cast<float>(cut_h);
  for (const Tex* tex : {&sheet_semi_, &sheet_opaque_})
    add_img(*tex, left * kPs1Px, top * kRowScale, static_cast<float>(cut_w) * kPs1Px, static_cast<float>(cut_h) * kRowScale, u0, v0, u1, v1, col(255, 255, 255));
}

int BattleView::sheet_glyph(char32_t ch) const {
  constexpr size_t kFont = kSheetFont / 4;
  return font_frame(ch, glyph_codes_, kFont < sheet_frames_.size() ? sheet_frames_[kFont].size() : 0);
}

float BattleView::add_sheet_name(std::u32string_view text, float x, float y, u32 colour, int max_glyphs) {
  int count = 0;
  for (char32_t ch : text) {
    if (count++ >= max_glyphs) break;
    const int frame = sheet_glyph(ch);
    if (frame > 0) add_sheet(kSheetFont, frame, x, y, colour);
    x += static_cast<float>(name_advance_[static_cast<size_t>(std::clamp(frame, 0, static_cast<int>(name_advance_.size()) - 1))]);
  }
  return x;
}

// The ring outline with the FIELD EFFECT label (sheet 0x80 frame 1) and one crescent per cell (cell k = frame k + 1 of its colour's sheet),
// all added over the picture from PS1 (8, 24); black is the white sheet subtracted at half strength. Under it the element-trap block
// (frame 2: the labels, then the party's mark or NO and the enemies' ? or NO) and, while the ring is one colour, the star count.
void BattleView::ps1_field_ring() {
  constexpr float kX = 8, kY = 24;
  // By colour index (green white black red yellow blue), then the all-colours sheet.
  static constexpr std::array<int, 7> kRingSheet = {0x54, 0x58, 0x58, 0x60, 0x64, 0x68, 0x6C};
  rebuild_ring();
  add_sheet(kSheetRing, 1, kX, kY, 0x808080, 2);
  for (int cell = 0; cell < 12; ++cell) {
    const int colour = ring_cells_[static_cast<size_t>(cell)];
    const int sheet = colour == battle::kRingAllColours ? kRingSheet[6] : colour >= 0 && colour < 6 ? kRingSheet[static_cast<size_t>(colour)] : -1;
    if (sheet < 0) continue;
    const bool black = colour == 2;
    add_sheet(sheet, cell + 1, kX, kY, black ? 0x404040 : 0x808080, black ? 3 : 2);
  }
  const battle::TrapHud trap = presented_trap();
  if (trap.shown) {
    // Trap mark frames by colour index.
    static constexpr std::array<int, 6> kTrapMark = {4, 7, 8, 3, 6, 5};
    add_sheet(kSheetRing, 2, kX, kY, 0x808080, 2);
    add_sheet(kSheetRing, trap.enemy != 0 ? 10 : 11, kX, kY, 0x808080, 2);
    int mark = 9;
    if (trap.pc > 0 && static_cast<size_t>(trap.pc) < tables_->elements.size()) {
      const int colour = data::colour_index_of_bit(static_cast<u8>(tables_->elements[static_cast<size_t>(trap.pc)].b(5) & 0xFC));
      mark = colour >= 0 && colour < 6 ? kTrapMark[static_cast<size_t>(colour)] : 0;
    }
    if (mark > 0) add_sheet(kSheetRing, mark, kX, kY, 0x808080, 2);
  }
  int shared = -1;
  for (size_t slot = 0; slot < 3; ++slot) {
    const int colour = ring_cells_[slot * 4];
    if (colour < 0) continue;
    if (shared >= 0 && colour != shared) return;
    shared = colour;
  }
  const int current = std::clamp(versus_ ? battle_->star_counter(versus_side_) : battle_->growth().counter, 0, 99);
  const int total = std::clamp(versus_ ? versus_stars_ : battle_->growth().star, 0, 99);
  add_sheet(kSheetDigits, 38, 0x3B, 0x1B, 0x808080, 2);
  const std::array<int, 9> cur = split_digits(current), all = split_digits(total);
  if (cur[7] >= 0) add_sheet(kSheetDigits, cur[7] + 15, 0x45, 0x1C, 0x808080, 2);
  add_sheet(kSheetDigits, cur[8] + 15, 0x49, 0x1C, 0x808080, 2);
  add_sheet(kSheetDigits, 39, 0x4E, 0x1B, 0x808080, 2);
  if (all[7] >= 0) add_sheet(kSheetDigits, all[7] + 15, 0x54, 0x1C, 0x808080, 2);
  add_sheet(kSheetDigits, all[8] + 15, all[7] >= 0 ? 88.0f : 84.0f, 0x1C, 0x808080, 2);
}

// One status panel per present party member at PS1 (x, 164), x by party size, as the PS1 draws it from its sheets:
//  * the panel (sheet 0x74 frame 1, the active one frame 2 while the PC chooses and (tick & 0x1F) > 15) at (x, y + 1), its grey body
//    subtracted; a colour status adds its corner (frames 4..9) there;
//  * the name in the battle font from (x + 4, y + 2), at most six glyphs;
//  * the level digit (sheet 0x10, digit + 1) at (x + 9, y + 12) and "Lv" (frame 13) at (x + 4, y + 18), colour 0x408040;
//  * HP in the HP colour: the current value's last three digits in 8 px slots from x + 4 (its leading zeros left out), "/" (frame 11) at
//    x + 28 and the maximum from x + 36, at y + 26;
//  * the stamina bar (rectangles, bhud::stamina_fill) at (x + 8, y + 35), under the stamina line;
//  * stamina at y + 37: "Sta." (frame 14) at x + 34, the tens digit (frame digit + 15) at x + 42, "." (frame 25) at x + 47 and the units
//    at x + 52, all 5 px further right with a hundreds digit at x + 30; below zero "Out.-" (frame 26) and the red digits (12 frames on);
//  * the element grid (sheet 0x08, half blended): a 5 x 3 cell per occupied unlocked cell at (x + 18 + 4 level, y + 6 + 2 slot), frame 3
//    below the gauge level, 2 above it, the gauge level itself alternating between them;
//  * status icons (sheet 0x78 frames 1..6) left to right from (x + 6, y - 6).
void BattleView::ps1_party_panels() {
  int count = 0;
  for (int i = 0; i < battle::kPcSlots; ++i) count += bunit(i).present ? 1 : 0;
  if (count == 0) return;
  static const float kPanelX[3][3] = {{172, 0, 0}, {140, 205, 0}, {108, 172, 236}};
  static constexpr std::array<int, 6> kColourCorner = {5, 8, 9, 4, 7, 6};
  static constexpr u32 kIconMask[6] = {0x00B00008u, 0x00000B01u, 0xB0000020u, 0x0B000010u, 0x0000F000u, 0x000F0000u};
  constexpr float py = 164;
  constexpr u32 kLevelColour = 0x408040, kWhiteWord = 0x808080;
  int placed = 0;
  for (int i = 0; i < battle::kPcSlots; ++i) {
    const battle::Unit& unit = bunit(i);
    if (!unit.present) continue;
    const float px = kPanelX[std::min(count, 3) - 1][std::min(placed++, 2)];
    const Visual& visual = vis_[static_cast<size_t>(i)];
    const bool choosing = i == active_pc_ && (ui_ == Ui::Top || ui_ == Ui::Chain || ui_ == Ui::Element || ui_ == Ui::Target || ui_ == Ui::Dopple || ui_ == Ui::Swap);
    add_sheet(kSheetPanel, choosing && (tick_count_ & 0x1F) > 15 ? 2 : 1, px, py + 1, kWhiteWord, 3);
    if (unit.hi & 0x3Fu) add_sheet(kSheetPanel, kColourCorner[static_cast<size_t>(std::countr_zero(unit.hi & 0x3Fu))], px, py + 1);
    add_sheet_name(import::utf8_to_u32(unit.name), px + 4, py + 2);
    add_sheet(kSheetDigits, std::clamp(unit.gauge + 1, 0, 9) + 1, px + 9, py + 12, kLevelColour);
    add_sheet(kSheetDigits, 13, px + 4, py + 18, kLevelColour);

    const int hp = std::clamp(static_cast<int>(std::lround(visual.hp_disp)), 0, 99999);
    const u32 hp_colour = hp * 8 <= unit.max_hp ? 0x3080u : hp * 4 <= unit.max_hp ? 0x4080u : kWhiteWord;
    const std::array<int, 9> cur = split_digits(hp), top = split_digits(std::clamp(unit.max_hp, 0, 99999));
    bool started = false;
    for (int k = 0; k < 3; ++k) {
      const int digit = cur[static_cast<size_t>(6 + k)];
      if (digit <= 0 && !started && k != 2) continue;
      started = true;
      add_sheet(kSheetDigits, std::max(digit, 0) + 1, px + 4 + 8.0f * static_cast<float>(k), py + 26, hp_colour);
    }
    float max_x = px + 36;
    for (int k = 0; k < 3; ++k) {
      const int digit = top[static_cast<size_t>(6 + k)];
      if (digit < 0) continue;
      add_sheet(kSheetDigits, digit + 1, max_x, py + 26, hp_colour);
      max_x += 8;
    }
    add_sheet(kSheetDigits, 11, px + 28, py + 26, hp_colour);

    const float bar_x = (px + 8) * kPs1Px, bar_y = (py + 35) * kRowScale;
    add_rect(bar_x, bar_y, 52 * kPs1Px, 4 * kRowScale, col(0, 0, 0));
    add_rect(bar_x + kPs1Px, bar_y + kRowScale, 50 * kPs1Px, 2 * kRowScale, col(0, 0, 0));
    const bhud::StaminaFill fill = bhud::stamina_fill(unit.stamina);
    const u32 bar_colour = fill.colour == bhud::BarColour::Red ? col(255, 0, 0) : fill.colour == bhud::BarColour::Yellow ? col(255, 255, 0) : col(0, 255, 255);
    add_rect(bar_x + kPs1Px, bar_y + kRowScale, static_cast<float>(fill.width) * kPs1Px, 2 * kRowScale, bar_colour);
    const bool out = unit.stamina < 0;
    const int frame_shift = out ? 12 : 0;
    const std::array<int, 9> sta = split_digits(std::min(std::abs(unit.stamina), 999));
    const float sta_y = py + 37;
    const float shift = sta[6] >= 0 ? 5.0f : 0.0f;
    if (sta[6] >= 0) add_sheet(kSheetDigits, sta[6] + frame_shift + 15, px + 30, sta_y);
    add_sheet(kSheetDigits, std::max(sta[7], 0) + frame_shift + 15, px + 42 + shift, sta_y);
    add_sheet(kSheetDigits, frame_shift + 25, px + 47 + shift, sta_y);
    add_sheet(kSheetDigits, sta[8] + frame_shift + 15, px + 52 + shift, sta_y);
    add_sheet(kSheetDigits, out ? 26 : 14, px + 34, sta_y);

    for (int row = 0; row < data::kGridRows; ++row)
      for (int slot = 0; slot < data::kGridCols; ++slot) {
        const size_t idx = static_cast<size_t>(row * data::kGridCols + slot);
        if ((unit.cells[idx] & 0x3FFF) == 0 || !unit.usable[idx]) continue;
        const int frame = unit.gauge < 0 || row > unit.gauge ? 2 : row < unit.gauge || (tick_count_ & 0x3F) < 32 ? 3 : 2;
        add_sheet(kSheetGrid, frame, px + 18 + 4.0f * static_cast<float>(row), py + 6 + 2.0f * static_cast<float>(slot), kWhiteWord, 1);
      }
    const u32 status = visual.status_shown & 0xBBBFFB39u;
    int icon_count = 0;
    for (int k = 0; k < 6 && status; ++k)
      if (status & kIconMask[k]) add_sheet(kSheetStatus, k + 1, px + 6 + 8.0f * static_cast<float>(icon_count++), py - 6);
  }
}

// The target info window (sheet 0x74 frame 3 at (6, 165), its body subtracted): the name (or the side label) in the battle font on
// line 0xA9, the Japanese one centred on x 0x37 (at most 11 glyphs 10 px apart from (9 - n) x 5 + 10, the letter after "・"), the USA one
// from x 10 (at most 15 glyphs at their widths, the letter after a space); "HP" (sheet 0x10 frame 12) at (10, 0xB3) and "/"
// (frame 11) at (0x37, 0xBB); a known HP in the HP colour, the current value's last six digits in 8 px slots from x 5 (leading zeros left
// out) and the maximum from 0x3F, else "?" (battle font frame 5) four times from 0x1F and from 0x3F, 6 px apart; from sheet 0x7C the
// sex mark (frame 1 / 2) at (0x1E, 0xC5), "INNATE:" (frame 3) at (0x30, 0xC6) and the innate colour's dot at (0x48, 0xC5), a colour
// shield's dot at 0x50.
void BattleView::ps1_target_info(int unit, const std::u32string& side) {
  constexpr int kSheetInfo = 0x7C;
  // Dot frames by colour index (green white black red yellow blue).
  static constexpr std::array<int, 6> kDot = {6, 8, 9, 4, 7, 5};
  add_sheet(kSheetPanel, 3, 6, 0xA5, 0x808080, 3);
  const battle::Unit& tu = bunit(unit);
  const bool jp = lang_ == import::Lang::Jp;
  std::u32string name = side;
  if (side.empty()) {
    name = import::utf8_to_u32(tu.name);
    if (!tu.pc && tu.letter != 0 && brain_.vm() && brain_.vm()->menu_entry100()) {
      const char32_t letter = static_cast<char32_t>(U'A' + tu.letter - 1);
      name += jp ? std::u32string(U"・") + static_cast<char32_t>(letter - U'A' + U'Ａ') : std::u32string(U" ") + letter;
    }
  }
  if (jp) {
    if (name.size() > 11) name.resize(11);
    float x = static_cast<float>((9 - static_cast<int>(name.size())) * 5);
    for (char32_t ch : name) {
      x += 10;
      if (const int frame = sheet_glyph(ch)) add_sheet(kSheetFont, frame, x, 0xA9);
    }
  } else {
    add_sheet_name(name, 10, 0xA9, 0x808080, 15);
  }
  if (!side.empty() || !(tu.pc || (tu.lo & 0x400u))) return;
  add_sheet(kSheetDigits, 12, 10, 0xB3);
  const bool hp_bar = enemy_hp_bar_on(unit, false);
  if (hp_bar) add_enemy_hp_bar(unit, 0x1E, 0xB5, 68);
  if (versus_ ? unit < battle::kPcSlots : (tu.pc || (tu.lo & 0x400000u) || (hp_bar && platform::cheats().enemy_hp_numbers))) {
    const u32 hp_colour = tu.hp * 8 <= tu.max_hp ? 0x3080u : tu.hp * 4 <= tu.max_hp ? 0x4080u : 0x808080u;
    add_sheet(kSheetDigits, 11, 0x37, 0xBB, hp_colour);
    const std::array<int, 9> cur = split_digits(std::clamp(tu.hp, 0, 999999)), top = split_digits(std::clamp(tu.max_hp, 0, 999999));
    bool started = false;
    for (int k = 0; k < 6; ++k) {
      const int digit = cur[static_cast<size_t>(3 + k)];
      if (digit <= 0 && !started && k != 5) continue;
      started = true;
      add_sheet(kSheetDigits, std::max(digit, 0) + 1, 5 + 8.0f * static_cast<float>(k), 0xBB, hp_colour);
    }
    float x = 0x3F;
    for (int k = 0; k < 6; ++k) {
      const int digit = top[static_cast<size_t>(3 + k)];
      if (digit < 0) continue;
      add_sheet(kSheetDigits, digit + 1, x, 0xBB, hp_colour);
      x += 8;
    }
  } else {
    add_sheet(kSheetDigits, 11, 0x37, 0xBB);
    for (int k = 0; k < 4; ++k) {
      add_sheet(kSheetFont, 5, 0x1F + 6.0f * static_cast<float>(k), 0xBB);
      add_sheet(kSheetFont, 5, 0x3F + 6.0f * static_cast<float>(k), 0xBB);
    }
  }
  add_sheet(kSheetInfo, tu.gender >> 14, 0x1E, 0xC5);
  add_sheet(kSheetInfo, 3, 0x30, 0xC6);
  const int innate = data::colour_index_of_bit(static_cast<u8>(tu.innate & 0xFC));
  if (innate >= 0 && innate < 6) add_sheet(kSheetInfo, kDot[static_cast<size_t>(innate)], 0x48, 0xC5);
  const int shield = data::colour_index_of_bit(static_cast<u8>((tu.hi & 0x3Fu) << 2));
  if (shield >= 0 && shield < 6) add_sheet(kSheetInfo, kDot[static_cast<size_t>(shield)], 0x50, 0xC5);
}

}  // namespace oc::game
