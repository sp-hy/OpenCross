#include "game/battle/battle_results.h"
#include "render/camera.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "core/log.h"
#include "import/tim.h"
#include "platform/keymap.h"
#include "render/text_batch.h"
#include "render/texture.h"
#include "ui/ps1_font.h"

namespace oc::game {

namespace {
// The screen is a 320 x 216-line display (sx = 1280/320, sy = 960/216) on the 1280x960 canvas.
constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
constexpr float SX = kCanvasW / 320.0f, SY = kCanvasH / 216.0f;

// Layout in PS1 pixels (part offsets from the btend.prd 'levu' sheet frames 2 / 3).
// Left rows: x = kRowX at rest, y = 0x20 + 60 r; row frame is levu frame 2 (btlexit-levu-002.png).
constexpr int kRowX = 8, kRowY0 = 0x20, kRowPitch = 0x3C;
constexpr int kRowW = 148, kRowH = 58;
// Right panel: levu frame 3 at (0xB0, 0x32), drawn from x - 16 (the frame parts start at dx = -16).
constexpr int kPanelX = 0xB0, kPanelY = 0x32;
constexpr int kPanelW = 144, kPanelH = 160;
// Three 32 x 224 portrait strips at x = 0x10 + 0x28 s.
constexpr int kFaceX0 = 0x10, kFaceDX = 0x28, kFaceW = 32, kFaceH = 224;
// "PowerUp" / "Spoils" headline, right-aligned (viva frames 5 / 6).
constexpr int kHeadX = 0x134, kHeadY = 0x10;
// Gold digits (osak, 8 px pitch; y = 0x32 + 0x90) end 6 px before the "G" (part 0x30 at panel +88 -> text at 0x101).
constexpr int kGoldRight = 0xFB;
// Wallpaper tile (btend.prd 'bg', HD battle_37.tim).
constexpr int kTileW = 32, kTileH = 32;

// Stat columns of a left row (panel-relative; digit slots 8 px apart, right edges): STR / HIT / MAG on y = 32, DEF / EVD / MDEF on y = 44
// (label parts 12 / 10 / 27 and 0 / 8 / 2 of levu frame 2; digit x inferred from the label extents).
struct StatCol {
  int label_x, digits_right, y;
};
// Order: ATK DEF HIT EVD MAG MDEF.
constexpr StatCol kStatCols[6] = {{12, 50, 32}, {7, 50, 44}, {56, 98, 32}, {56, 98, 44}, {104, 146, 32}, {104, 146, 44}};
// Digit cells: x = X0 + {40, 40, 88, 88, 136, 136}[k] + 8 j - 6 (2 cells: the right edges above), y = y0 + {32, 44, 32, 44, 32, 44}[k].
// UP markers (sheet levu frame 1) at X0 + kUpX[i] - 8 / y0 + kUpY[i] for i = HP, ATK, DEF, HIT, EVD, MAG, MDEF;
// the sprite's own part offset is kUpDy.
constexpr int kUpX[7] = {20, 20, 20, 68, 68, 116, 116}, kUpY[7] = {12, 24, 36, 24, 36, 24, 36}, kUpDy = 5;
// Stat label keys in the shared message text.
const char* kStatJp[7] = {"ＨＰ", "腕力", "耐久力", "正確さ", "身軽さ", "魔力", "抗魔"};
const char* kStatEn[7] = {"HP", "STR.", "RES.", "ACC.", "AGI.", "MAG.", "M.RES"};

// Text colours: one tint on every text (labels, digits, panel names, spoils) and a grey on the party rows' name line only. The glyph
// faces are white, so the result is the multiplier itself. ABGR: R 207 G 197 B 197 / grey 165.
constexpr u32 kColText = 0xFFC5C5CFu, kColName = kColText, kColDigit = kColText, kColRowName = 0xFFA5A5A5u;
// Heal menu: healing amount by tier, indexed by the element row's tier (menu_pg +A & 3).
constexpr int kHealAmount[4] = {80, 160, 320, 16};
// Glyph box heights in PS1 lines = 70 px font x scale / sy: stat labels = bn_70 x 0.65 x 0.5; osak glyphs (names / digits) =
// kb_70 x 0.70 x 0.5; "HP", "/", "G", "Received" = bn x 0.70 x 0.5; the right-panel lines (the name, "HP+" labels, star page,
// Spoils header) = bn x 0.5; the row name = bn x 0.70 x 0.5.
constexpr float kLabelPx = 70.0f * 0.325f / SY, kBigPx = 70.0f * 0.35f / SY, kMidPx = 70.0f * 0.35f / SY, kPanelPx = 70.0f * 0.5f / SY;
// Colour index (green white black red yellow blue) -> battle_08_NN file number.
constexpr u32 kHdColourFile[6] = {10, 5, 13, 7, 3, 12};

// sysmes names carry a leading icon code (Greek alpha..delta: element kind / material) that the RDE fonts draw as the list icon; the
// results screen draws the PS1 icon itself (sico / EquipIcon), so the code is dropped from the name.
std::u32string strip_icon(std::u32string name) {
  if (!name.empty() && name[0] >= 0x3B1 && name[0] <= 0x3B4) name.erase(0, 1);
  return name;
}
u32 rgba(int r, int g, int b, int a = 255) { return render::pack_rgba(static_cast<u8>(r), static_cast<u8>(g), static_cast<u8>(b), static_cast<u8>(a)); }
// Scale the colour channels, keep alpha.
u32 tint(u32 colour, float factor) {
  const int r = static_cast<int>(static_cast<float>(colour & 0xFF) * factor), g = static_cast<int>(static_cast<float>((colour >> 8) & 0xFF) * factor),
            b = static_cast<int>(static_cast<float>((colour >> 16) & 0xFF) * factor);
  return (colour & 0xFF000000u) | static_cast<u32>(std::min(255, r)) | (static_cast<u32>(std::min(255, g)) << 8) | (static_cast<u32>(std::min(255, b)) << 16);
}
}  // namespace

struct BattleResults::Impl {
  // Sampler choice: filtered by [Video] Filter*, filtered + repeating (wallpaper), always nearest (the white pixel).
  enum Kind { kLin, kRep, kFixed };
  struct Tex {
    render::Texture tex;
    render::TexRef set;
    float w = 1, h = 1;
    Kind kind = kLin;
    bool ok() const { return static_cast<bool>(set); }
  };
  struct Row {
    bool present = false;
    int char_id = 0;
    std::u32string name;
    int hp = 0, max_hp = 0;
    std::array<int, 7> stat{}, gain{};
    // shown: this member's PowerUp page was reached (stats + UP markers switch to the new values).
    bool has_gain = false, shown = false;
    std::array<u16, data::kGridCells> cells{};
    int gauge = -1;
    // KO (status 0x80): no gains, not healed.
    bool ko = false;
    // The battle unit's grid (heal menu: healing elements in cells that are open now), and which cells are open
    // (unlocked and not used, flags & 0xC1 == 0).
    std::array<u16, data::kGridCells> unit_cells{};
    std::array<bool, data::kGridCells> open{};
  };
  // One distinct healing element of the party's open grid cells: id + number of cells holding it.
  struct Healer {
    int id = 0, count = 0;
  };
  struct Drop {
    bool element = false;
    int id = 0, colour = -1, kind = 0;
    std::u32string name;
  };

  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  const data::GameTables* tables = nullptr;
  import::Lang lang = import::Lang::En;
  import::LangTables lt;
  render::TextBatch text;
  // Font slots: large_font_* (HD) / small_font_* (original art).
  ui::font::FontSet fonts;
  render::Sampler linear, nearest, repeat_linear, repeat_nearest;
  Tex white;
  bool hd = true;
  // [Video] Filter (global).
  bool bilinear = true;
  std::string portrait_set = "2021";
  std::function<void(int)> sfx;
  std::function<void()> on_exit;

  // The HD art, drawn in both art modes: the battle_37.tim wallpaper and the portrait strips
  // hd/battle/face/<dir>/pNN.png with <dir> = 1999 in original art, 2021 in HD.
  Tex hd_row, hd_panel, hd_up, hd_star, hd_gg_on, hd_gg_off, hd_equip, hd_wall, hd_cursor;
  std::array<Tex, 6> hd_plate;
  // Portrait cache, key "<dir>/pNN".
  std::map<std::string, std::unique_ptr<Tex>> hd_face;

  // state
  Phase phase = Phase::Idle;
  // age: frames since open; pt: frames in the phase.
  int age = 0, pt = 0, debounce = 0;
  float acc = 0;
  // Panel offset in PS1 px: 24 at open decaying by 1/8 per frame, growing by 1/8 per frame at exit.
  float slide = 0;
  std::array<Row, battle::kPcSlots> rows{};
  // Slots with any gain, in slot order.
  std::vector<int> gain_list;
  int cur = 0;
  bool star_up = false, star_page = false;
  int star = 0, counter = 0, gold = 0;
  std::vector<Drop> drops;
  // Star-up glyph brightness.
  int pulse = 0x40, pulse_dir = 4;
  bool auto_on = false;
  // Scene 0x1D8 plays none of the screen's sounds unless the party fled.
  bool muted = false;
  int draws = 0;
  // Frames since the current page appeared (auto mode).
  int page_age = 0;
  // heal menu
  // cdrom/battle/data/menu_pg.bin
  std::vector<std::array<u8, 16>> pg;
  std::vector<Healer> healers, used;
  // cursor_mem is kept for the session.
  int cursor = 0, cursor_mem = 0;
  bool cursor_memory = false;
  bool final_ready = false;

  // ---- loading ---------------------------------------------------------------------------------------------------------
  const render::Sampler& sampler_for(Kind kind) const {
    if (kind == kFixed) return nearest;
    if (kind == kRep) return bilinear ? repeat_linear : repeat_nearest;
    return bilinear ? linear : nearest;
  }
  bool make_tex(Tex& tex, u32 w, u32 h, const u8* px, Kind kind) {
    if (w == 0 || h == 0 || !tex.tex.create(*ctx, w, h, render::TextureFormat::Rgba8, px)) return false;
    tex.kind = kind;
    tex.set = render::TexRef(tex.tex, sampler_for(kind));
    tex.w = static_cast<float>(w), tex.h = static_cast<float>(h);
    return tex.ok();
  }
  bool load_png(Tex& tex, const std::string& path, Kind kind = kLin) {
    auto raw = vfs->read(path);
    render::DecodedImage img;
    if (!raw || !render::decode_image(*raw, img)) return false;
    return make_tex(tex, img.width, img.height, img.rgba.data(), kind);
  }
  // Any TIM (PS1 or RDE HD) -> RGBA texture; palette 0 = transparent like the GPU.
  bool load_tim(Tex& tex, ByteSpan bytes, Kind kind) {
    auto tim = import::parse_tim(bytes);
    if (!tim) return false;
    auto img = import::decode_tim(*tim, 0);
    if (!img) return false;
    std::vector<u8> px;
    const u8* src = img->pixels.data();
    if (img->indexed) {
      px.resize(static_cast<size_t>(img->width) * img->height * 4);
      for (size_t i = 0; i < static_cast<size_t>(img->width) * img->height; ++i) {
        const u8 idx = img->pixels[i];
        const u32 colour = idx < img->palette.size() ? img->palette[idx] : 0;
        px[i * 4] = static_cast<u8>(colour), px[i * 4 + 1] = static_cast<u8>(colour >> 8), px[i * 4 + 2] = static_cast<u8>(colour >> 16), px[i * 4 + 3] = static_cast<u8>(colour >> 24);
      }
      src = px.data();
    }
    return make_tex(tex, img->width, img->height, src, kind);
  }
  void free_tex(Tex& tex) {
    tex.set = {};
    tex.tex.destroy();
  }
  void load_hd_art() {
    load_png(hd_row, "hd/battle/shapes/btlexit-levu-002.png");
    load_png(hd_panel, "hd/battle/shapes/btlexit-levu-003.png");
    load_png(hd_up, "hd/battle/btend/up.png");
    load_png(hd_star, "hd/battle/btend/star.png");
    load_png(hd_gg_on, "hd/battle/shapes/battle-gg-1.png");
    load_png(hd_gg_off, "hd/battle/shapes/battle-gg-2.png");
    load_png(hd_equip, "hd/menu/menutim/EquipIcon.png");
    // Sheet 'curs' frames 1 / 2 are the two halves of battle_11.
    load_png(hd_cursor, "hd/battle/battle/battle_11.png");
    for (int i = 0; i < 6; ++i) {
      char path[64];
      std::snprintf(path, sizeof path, "hd/battle/battle/battle_08_%02u_03.png", kHdColourFile[i]);
      load_png(hd_plate[static_cast<size_t>(i)], path);
    }
    if (auto raw = vfs->read("hd/battle/battle/battle_37.tim")) load_tim(hd_wall, *raw, kRep);
    if (!hd_row.ok() || !hd_panel.ok()) log::warn("battle results: HD btlexit panels missing");
  }
  Tex* face(int char_id) {
    char key[80];
    std::snprintf(key, sizeof key, "%s/p%02d", hd ? portrait_set.c_str() : "1999", char_id);
    if (auto it = hd_face.find(key); it != hd_face.end()) return it->second->ok() ? it->second.get() : nullptr;
    auto tex = std::make_unique<Tex>();
    load_png(*tex, std::string("hd/battle/face/") + key + ".png");
    Tex* result = tex->ok() ? tex.get() : nullptr;
    hd_face[key] = std::move(tex);
    return result;
  }

  // ---- text ------------------------------------------------------------------------------------------------------------
  // lang/btlexit.txt (keys 11..17 stat-up, 26 Received, 27 ->, 28 /, 30/32 growth, 35 Spoils, 0x10005/6 headlines).
  std::u32string label(u64 key) {
    if (const import::TextTable* tt = lt.table("btlexit"))
      if (const std::string* text_utf8 = tt->get(key, lang); text_utf8 && !text_utf8->empty()) return import::utf8_to_u32(*text_utf8);
    return {};
  }
  // cmnmes: JP key -> language column; untranslated = the EN word.
  std::u32string stat_label(int stat) {
    if (const import::TextTable* tt = lt.table("cmnmes"))
      if (const std::string* text_utf8 = tt->get(std::string_view(kStatJp[stat]), lang); text_utf8 && !text_utf8->empty()) return import::utf8_to_u32(*text_utf8);
    return import::utf8_to_u32(kStatEn[stat]);
  }
  // Font slots: labels / names use kb_70 for JP and bn_70 otherwise (bn has no kana / kanji), osak digits kb_70, headline ch_100.
  static constexpr int kDigits = ui::font::kKb, kHead = ui::font::kCh;
  int label_slot() const { return lang == import::Lang::Jp ? ui::font::kKb : ui::font::kBn; }
  // Scale for a glyph box of `ps1_px` PS1 lines (70 px faces).
  static float scale_for(float ps1_px) { return ps1_px * SY / 70.0f; }
  void prepare(const std::u32string& text_u32) {
    for (int slot : {label_slot(), kDigits, kHead}) fonts.prepare(slot, text_u32);
  }
  void prepare_all() {
    prepare(U"0123456789+-/ ");
    for (int k = 0; k < 7; ++k) prepare(stat_label(k)), prepare(label(11 + static_cast<u64>(k)));
    for (u64 key : {26ull, 27ull, 28ull, 30ull, 31ull, 32ull, 33ull, 35ull, 0x10005ull, 0x10006ull}) prepare(label(key));
    prepare(U"G");
    for (const Row& row : rows) prepare(row.name);
    for (const Drop& drop : drops) prepare(drop.name);
  }

  // ---- drawing ---------------------------------------------------------------------------------------------------------
  // Widescreen: the wallpaper and fades cover the columns edge_x0 .. edge_x0 + edge_w (the window's widened frame); `shift` = the current
  // group's offset (the portraits and rows -layout_cols, the panel and headline +layout_cols; [Video] WideHud).
  float zoom = 1, ox = 0, oy = 0;
  float canvas_cols = 0, layout_cols = 0, edge_x0 = 0, edge_w = 320, shift = 0;
  float cx(float x) const { return ox + (x + shift) * SX * zoom; }
  float cy(float y) const { return oy + y * SY * zoom; }
  void quad(const Tex& tex, float x, float y, float w, float h, u32 colour = 0xFFFFFFFFu, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    if (!tex.ok()) return;
    sprites->draw(tex.set, cx(x), cy(y), w * SX * zoom, h * SY * zoom, u0, v0, u1, v1, colour);
  }
  void rect(float x, float y, float w, float h, u32 colour) { quad(white, x, y, w, h, colour); }
  float measure(int slot, const std::u32string& text_u32, float sc) { return fonts.measure(slot, text_u32, sc) / SX; }
  // Text at PS1 (x, y): align 0 left, 1 centre, 2 right. `ps1_px` = glyph box height in PS1 lines. The font slot's canvas offset times the
  // draw scale (offX / offY * scale) is added to the PS1 position.
  void txt(int slot, const std::u32string& text_u32, float x, float y, u32 colour, float ps1_px, int align = 0) {
    if (text_u32.empty()) return;
    const float sc = scale_for(ps1_px);
    const float w = measure(slot, text_u32, sc);
    if (align == 1) x -= w * 0.5f;
    if (align == 2) x -= w;
    const ui::font::SlotInfo& si = ui::font::slot(slot);
    const ui::font::Ink ink{colour, rgba(33, 33, 33, static_cast<int>(colour >> 24)), true, false};
    fonts.draw(slot, text_u32, (x + shift) * SX + si.off_x * sc + ox / zoom, y * SY + si.off_y * sc + oy / zoom, ink, sc, ui::font::kLeft, zoom);
  }
  // Names and numbers drawn as 'osak' glyphs: kb_70 text.
  void big(const std::u32string& text_u32, float x, float y, u32 colour, int align = 0) { txt(kDigits, text_u32, x, y, colour, kBigPx, align); }
  void num(int value, float right, float y, u32 colour = kColDigit) { big(import::utf8_to_u32(std::to_string(value)), right, y, colour, 2); }

  void draw_wall() {
    if (hd_wall.ok()) quad(hd_wall, edge_x0, 0, edge_w, 216, 0xFFFFFFFFu, edge_x0 / kTileW, 0, (edge_x0 + edge_w) / kTileW, 216.0f / kTileH);
  }
  void draw_faces() {
    // Three 32 x 224 strips at x = 0x10 + 0x28 s, drawn first (under the rows) at half brightness.
    for (int slot = 0; slot < battle::kPcSlots; ++slot) {
      const Row& row = rows[static_cast<size_t>(slot)];
      if (!row.present) continue;
      if (Tex* portrait = face(row.char_id)) quad(*portrait, static_cast<float>(kFaceX0 + kFaceDX * slot), 0, kFaceW, kFaceH, rgba(128, 128, 128));
    }
  }
  void draw_row(int slot) {
    const Row& row = rows[static_cast<size_t>(slot)];
    const float x0 = static_cast<float>(kRowX) - slide, y0 = static_cast<float>(kRowY0 + kRowPitch * slot);
    quad(hd_row, x0, y0, kRowW, kRowH);
    // name (6 glyphs at X0 + 0xC, y0 + 8)
    txt(label_slot(), row.name, x0 + 12, y0 + 8, kColRowName, kMidPx);
    const bool after = row.shown;
    // HP line: label part 20 at (8, 21), current HP digits at X0 + 0x20 + 8 j, "/" part 28 at (55, 21), max at X0 + 0x40 + 8 j
    txt(label_slot(), stat_label(0), x0 + 8, y0 + 21, kColText, kLabelPx);
    txt(label_slot(), U"/", x0 + 56, y0 + 20, kColText, kMidPx);
    const int max_hp = after ? row.max_hp : row.max_hp - row.gain[0];
    num(std::clamp(row.hp, 0, 9999), x0 + 54, y0 + 20);
    num(max_hp, x0 + 86, y0 + 20);
    // six stats (two rows of three)
    for (int k = 0; k < 6; ++k) {
      const StatCol& col = kStatCols[k];
      const int value = after ? row.stat[static_cast<size_t>(k + 1)] : row.stat[static_cast<size_t>(k + 1)] - row.gain[static_cast<size_t>(k + 1)];
      txt(label_slot(), stat_label(k + 1), x0 + static_cast<float>(col.label_x) - (lang != import::Lang::Jp && k == 0 ? 5.0f : 0.0f), y0 + static_cast<float>(col.y) + 1.0f, kColText, kLabelPx);
      num(std::clamp(value, 0, 99), x0 + static_cast<float>(col.digits_right), y0 + static_cast<float>(col.y));
    }
    // "UP" marker (levu frame 1 = btend/up.png 16 x 16) on every stat that grew, once the member's page was shown:
    // sprite at X0 + kUpX[i] - 8, y0 + kUpY[i] + kUpDy with i = HP, ATK, DEF, HIT, EVD, MAG, MDEF
    if (after)
      for (int i = 0; i < 7; ++i)
        if (row.gain[static_cast<size_t>(i)] > 0) draw_up(x0 + static_cast<float>(kUpX[i] - 8), y0 + static_cast<float>(kUpY[i] + kUpDy));
    // element-grid thumbnail: gg dots at X0 + 0x66 + 4 grid_row, y0 + 6 + 2 grid_col for every occupied cell; bright up to the gauge level
    for (int grid_row = 0; grid_row < data::kGridRows; ++grid_row)
      for (int grid_col = 0; grid_col < data::kGridCols; ++grid_col) {
        const size_t idx = static_cast<size_t>(grid_row * data::kGridCols + grid_col);
        if (idx >= row.cells.size() || (row.cells[idx] & 0x3FFF) == 0) continue;
        const bool on = grid_row <= row.gauge;
        const float dx = x0 + 0x66 + 4.0f * static_cast<float>(grid_row), dy = y0 + 6 + 2.0f * static_cast<float>(grid_col);
        quad(on ? hd_gg_on : hd_gg_off, dx, dy, 5, 3);
      }
  }
  void draw_up(float x, float y) { quad(hd_up, x, y, 16, 16); }
  void draw_headline(bool spoils) {
    // Headline (viva frame 5 / 6) right-aligned at (0x134, 0x10), hidden while sliding out. Drawn with the ch_100 face
    // (same file in both art modes) from btlexit.txt 0x10005 / 6, with a shadow colour 0x414E60 under the 0xB8C7D5 text.
    if (phase == Phase::Exit) return;
    const float x = static_cast<float>(kHeadX), y = static_cast<float>(kHeadY);
    if (const render::FaceId face_ch = fonts.face(kHead); face_ch != render::kNoFace) {
      const std::u32string headline = label(spoils ? 0x10006 : 0x10005);
      // The word is 12.8 PS1 lines tall (ascender to descender) with its ink top 25.7 lines
      // down, i.e. the 100 px face at 0.47, 6.5 lines below the PS1 y.
      const float sc = 0.47f;
      const float w = measure(kHead, headline, sc);
      text.draw(face_ch, headline, cx(x - w) + 2 * zoom, cy(y + 6.5f) + 2 * zoom, rgba(0x60, 0x4E, 0x41), sc * zoom);
      text.draw(face_ch, headline, cx(x - w), cy(y + 6.5f), rgba(0xD5, 0xC7, 0xB8), sc * zoom);
      return;
    }
  }
  void draw_panel() {
    const float px = static_cast<float>(kPanelX), py = static_cast<float>(kPanelY) + slide;
    quad(hd_panel, px - 16, py, kPanelW, kPanelH);
    // gold line: osak digits ending before the "G" part (0x30, panel +88 - 7), then 獲得 / "Received" (part 0x32, +96 - 7)
    num(std::clamp(gold, 0, 9999999), static_cast<float>(kGoldRight), py + 144, kColDigit);
    txt(label_slot(), U"G", px + 81, py + 144, kColText, kMidPx);
    txt(label_slot(), label(26), px + 89, py + 145, kColText, kMidPx);
    if (phase == Phase::Spoils) draw_spoils(px, py);
    else if (phase == Phase::Heal) draw_heal_menu(px, py);
    else if (phase == Phase::Healed) draw_healed(px, py);
    else if (star_page) draw_star_page(px, py);
    else draw_powerup(px, py);
  }
  void draw_powerup(float px, float py) {
    // Member name at (0xB4, 0x42); one line per grown stat: label (key 11 + k) at (0xC0, 0x52 + 0xE n), the gain (3 digit
    // slots at 0xF0 + 0xC j, no sign) right-aligned at 0x108 + 0xC.
    // Nobody grew: only the slots with a gain are drawn, so the blank page keeps its headline and shows no name and no lines.
    if (cur >= static_cast<int>(gain_list.size())) return;
    const int slot = gain_list[static_cast<size_t>(cur)];
    const Row& row = rows[static_cast<size_t>(slot)];
    txt(label_slot(), row.name, px + 4, py + 16, kColName, kPanelPx);
    int line = 0;
    for (int k = 0; k < 7; ++k) {
      if (row.gain[static_cast<size_t>(k)] <= 0) continue;
      const float y = py + 32 + 14.0f * static_cast<float>(line++);
      txt(label_slot(), label(11 + static_cast<u64>(k)), px + 16, y, kColText, kPanelPx);
      // the gain digits are 7.3 lines tall = the kb font at 0.5, not the 0.35 of the row digits
      txt(kDigits, import::utf8_to_u32(std::to_string(row.gain[static_cast<size_t>(k)])), px + 100, y - 1, kColDigit, kPanelPx, 2);
    }
  }
  void draw_star_page(float px, float py) {
    // Pulsing star (levu 4 / btend/star.png) at (0xB4, 0x3C); "Received" (key 26) at (0xCC, 0x42); "->" (27) at (0xD8, 0x52),
    // the star-up counter at 0xE4, "/" (28) at 0xFC, the star level at 0x108 (y 0x52); the leader's name at (0xB4, 0x82); the growth
    // message (key 30, or 32 at star 99) at (0xB4, 0x92) and (0xB4, 0xA2).
    const float brightness = static_cast<float>(pulse) / 128.0f;
    quad(hd_star, px + 4, py + 10, 20, 20, tint(0xFFFFFFFFu, brightness));
    txt(label_slot(), label(26), px + 28, py + 17, kColText, kPanelPx);
    txt(label_slot(), label(27), px + 40, py + 33, kColText, kPanelPx);
    num(counter, px + 52 + 16, py + 32);
    txt(label_slot(), label(28), px + 76, py + 33, kColText, kPanelPx);
    num(star, px + 88 + 16, py + 32);
    const int lead = first_present();
    if (lead >= 0) txt(label_slot(), rows[static_cast<size_t>(lead)].name, px + 4, py + 80, kColName, kPanelPx);
    const std::u32string msg = label(star >= 99 ? 32 : 30);
    // the EN/FR/.. message carries its own line break; JP uses two keys (30 / 31)
    size_t newline = msg.find(U'\n');
    txt(label_slot(), newline == std::u32string::npos ? msg : msg.substr(0, newline), px + 4, py + 96, kColText, kPanelPx);
    std::u32string second = newline == std::u32string::npos ? label(star >= 99 ? 33 : 31) : msg.substr(newline + 1);
    txt(label_slot(), second, px + 4, py + 112, kColText, kPanelPx);
  }
  // Text lines of a message: the 3 / 2 line message sits in its first key and the lines print 16 PS1 lines apart.
  void lines(const std::u32string& message, float x, float y, float px_h) {
    size_t begin = 0;
    for (int i = 0; begin <= message.size(); ++i) {
      size_t end = message.find(U'\n', begin);
      if (end == std::u32string::npos) end = message.size();
      txt(label_slot(), message.substr(begin, end - begin), x, y + 16.0f * static_cast<float>(i), kColText, px_h);
      begin = end + 1;
    }
  }
  // Heal menu: the question (key 18, three lines) at (0xAC, 0x3C) and the three choices (keys 21, 23, 25) at (0xB0, 0x70 / 0x90 / 0xB0), their
  // second line 16 lines below; the cursor 'curs' frame 1 / 2 (blinking every 16 frames) at (0xA8, 0x72 + 0x20 * cursor). Non-English Latin texts of the choices
  // are drawn at x 0.85.
  void draw_heal_menu(float /*px*/, float py) {
    const float dy = py - static_cast<float>(kPanelY);
    lines(label(18), 0xAC, 0x3C + dy + 1, kPanelPx);
    const bool small = lang != import::Lang::En && lang != import::Lang::Jp;
    for (int i = 0; i < 3; ++i) lines(label(21 + 2 * static_cast<u64>(i)), 0xB0, 0x70 + 0x20 * static_cast<float>(i) + dy + 1, small ? kPanelPx * 0.85f : kPanelPx);
    if (hd_cursor.ok()) {
      const bool second = ((age >> 4) & 1) != 0;
      const float y = 0x72 + 0x20 * static_cast<float>(cursor) + dy;
      if (!second) quad(hd_cursor, 0xA8, y, 9, 8, 0xFFFFFFFFu, 0, 0, 0.5625f, 1);
      else quad(hd_cursor, 0xA8, y, 7, 8, 0xFFFFFFFFu, 0.5625f, 0, 1, 1);
    }
  }
  // Healed page: header key 34 at (0xC0, 0x3E); one row per healing element used, y = 0x4E + 0x12 n: the colour plate (menu_pg +B colour, sheet frame 3) at
  // 0xAC, the element name centred on 0xE8, "x" at 0x118 and the number of uses (tens at 0x11E, ones at 0x11E or 0x124).
  void draw_healed(float px, float py) {
    txt(label_slot(), label(34), px + 16, py + 13, kColText, kPanelPx);
    int line = 0;
    for (const Healer& healer : used) {
      const float y = py + 28 + 18.0f * static_cast<float>(line++);
      const int ci = plate_of(healer.id);
      if (ci >= 0) quad(hd_plate[static_cast<size_t>(ci)], px - 4, y + 1, 120, 16);
      txt(label_slot(), strip_icon(import::utf8_to_u32(tables->element_name(healer.id))), 0xE8, y + 3, kColText, kPanelPx, 1);
      txt(label_slot(), U"x", 0x118, y + 6, kColText, kLabelPx);
      txt(kDigits, import::utf8_to_u32(std::to_string(healer.count)), 0x11E - 1, y + 6, kColDigit, kLabelPx);
    }
  }
  void draw_spoils(float px, float py) {
    // Header (key 35) at (0xBA, 0x3E); row n at y = 0x4E + 0x12 n: item icon (sico kind + 1 / EquipIcon cell) at 0xB0 and
    // the name at 0xC2; element: the colour plate (sheet colour << 2, frame 3 / battle_08_NN_03) at 0xAC and the name on it.
    // The names are centred at x = 0xE8.
    txt(label_slot(), label(35), px + 10, py + 13, kColText, kPanelPx);
    int line = 0;
    for (const Drop& drop : drops) {
      if (line >= 6) break;
      const float y = py + 28 + 18.0f * static_cast<float>(line++);
      if (drop.element) {
        const int ci = std::clamp(drop.colour, 0, 5);
        quad(hd_plate[static_cast<size_t>(ci)], px - 4, y + 1, 120, 16);
        txt(label_slot(), drop.name, 0xE8, y + 3, kColName, kPanelPx, 1);
      } else {
        const float u0 = 0.0075f + 0.2f * static_cast<float>(std::clamp(drop.kind, 0, 4));
        quad(hd_equip, px, y, 16, 16, 0xFFFFFFFFu, u0, 0.0075f, u0 + 0.185f, 0.9925f);
        txt(label_slot(), drop.name, 0xE8, y + 3, kColName, kPanelPx, 1);
      }
    }
    if (drops.empty()) txt(label_slot(), U"-", 0xE8, py + 31, kColText, kPanelPx, 1);
  }
  // ---- heal menu logic ------------------------------------------------------------------------------------------
  // menu_pg.bin row of an element (16 bytes): +A & 3 healing tier, 0x80 all party, 0x40 consumable stack; +B 0x40 = healing element,
  // 0x3F colour mask (0x01 green, 0x02 white, 0x04 black, 0x08 red, 0x10 yellow, 0x20 blue = plate sheet order).
  const std::array<u8, 16>* pg_row(int id) const { return id >= 0 && static_cast<size_t>(id) < pg.size() ? &pg[static_cast<size_t>(id)] : nullptr; }
  int plate_of(int id) const {
    const auto* pg_entry = pg_row(id);
    if (!pg_entry) return -1;
    // colour mask -> sheet 1..6
    const int mask = (*pg_entry)[0xB] & 0x3F;
    for (int i = 0; i < 6; ++i)
      if (mask == (1 << i)) return i;
    return -1;
  }
  // Present, not KO'd (status 0x80 clear) and HP < max HP.
  bool hurt(const Row& row) const { return row.present && !row.ko && row.hp < row.max_hp; }
  int hurt_count() const {
    int count = 0;
    for (const Row& row : rows) count += hurt(row) ? 1 : 0;
    return count;
  }
  // The healing elements (menu_pg +B & 0x40, no consumable stack) in the open cells (unlocked, unused, element id < 0x80)
  // of rows 0..gauge of every living member, scanned slot by slot from the highest cell down; merged by id (count = cells); ordered by tier (+A & 3, 2 first) with the
  // single-target ones before the all-party ones when fewer than two members are hurt, after them otherwise.
  // Rows with both menu_pg +B 0x40 and +A 0x40 (consumable stacks) are skipped: no menu_pg row has both bits.
  std::vector<Healer> collect_healers() const {
    std::vector<Healer> uniq;
    for (const Row& row : rows) {
      if (!row.present || row.ko || row.gauge < 0) continue;
      for (int cell = std::min((row.gauge + 1) * data::kGridCols, data::kGridCells) - 1; cell >= 0; --cell) {
        const size_t i = static_cast<size_t>(cell);
        if (!row.open[i] || row.unit_cells[i] == 0 || row.unit_cells[i] >= 0x80) continue;
        const auto* pg_entry = pg_row(row.unit_cells[i]);
        if (!pg_entry || ((*pg_entry)[0xB] & 0x40) == 0 || ((*pg_entry)[0xA] & 0x40) != 0) continue;
        auto it = std::find_if(uniq.begin(), uniq.end(), [&](const Healer& healer) { return healer.id == row.unit_cells[i]; });
        if (it != uniq.end()) ++it->count;
        else uniq.push_back({row.unit_cells[i], 1});
      }
    }
    std::vector<Healer> single[3], all[3], out;
    for (const Healer& healer : uniq) {
      const auto& pg_entry = *pg_row(healer.id);
      const int tier = pg_entry[0xA] & 3;
      // only tiers 2..0 are sorted
      if (tier > 2) continue;
      ((pg_entry[0xA] & 0x80) ? all : single)[tier].push_back(healer);
    }
    auto append = [&](std::vector<Healer>* by_tier) {
      for (int tier = 2; tier >= 0; --tier) out.insert(out.end(), by_tier[tier].begin(), by_tier[tier].end());
    };
    if (hurt_count() < 2) append(single), append(all);
    else append(all), append(single);
    return out;
  }
  // Heal a living member below max HP by `amount`; true when one was healed.
  bool heal_one(int amount, Row& row) {
    if (!row.present || row.ko || row.hp >= row.max_hp) return false;
    row.hp = std::min(row.hp + amount, row.max_hp);
    return true;
  }
  // One use of healing element `id`: all members (flag 0x80) or the first hurt one.
  void heal_use(int id) {
    const auto& pg_entry = *pg_row(id);
    const int amount = kHealAmount[pg_entry[0xA] & 3];
    for (Row& row : rows) {
      const bool done = heal_one(amount, row);
      if (done && (pg_entry[0xA] & 0x80) == 0) return;
    }
  }
  // Both heal choices behave the same (the stock list is empty): each healer in list order, up to `count` uses, until nobody is hurt.
  void heal_apply() {
    used.clear();
    for (size_t i = 0; i < healers.size(); ++i) {
      int left = healers[i].count;
      while (left > 0) {
        --left;
        heal_use(healers[i].id);
        if (hurt_count() == 0) break;
      }
      if (healers[i].count - left > 0) {
        used.push_back({healers[i].id, healers[i].count - left});
        log::info("battle results: heal element {} used {} time(s)", healers[i].id, healers[i].count - left);
      }
      if (hurt_count() == 0) break;
    }
  }
  // The menu is offered when a member is hurt and a healer exists.
  void after_powerups() {
    healers = collect_healers();
    if (hurt_count() > 0 && !healers.empty()) {
      phase = Phase::Heal;
      cursor = cursor_memory ? std::clamp(cursor_mem, 0, 2) : 0;
      log::info("battle results: heal menu offered ({} healing element(s), {} member(s) hurt)", healers.size(), hurt_count());
    } else {
      begin_spoils();
    }
    pt = 0;
  }
  // The member's gains are applied to the live values - max HP and current HP both rise by the HP gain (cap 999).
  void show_member(int slot) {
    Row& row = rows[static_cast<size_t>(slot)];
    if (row.shown) return;
    row.shown = true;
    row.hp = std::min(row.hp + row.gain[0], 999);
  }
  int first_present() const {
    for (int i = 0; i < battle::kPcSlots; ++i)
      if (rows[static_cast<size_t>(i)].present) return i;
    return -1;
  }

  // ---- state -----------------------------------------------------------------------------------------------------------
  void play(int id) {
    if (sfx && !muted) sfx(id);
  }
  void confirm() {
    if (debounce > 0 || age < kSettle) return;
    page_age = 0;
    if (phase == Phase::Heal) {
      play(8);
      // "Do not heal party": straight to Spoils.
      if (cursor == 2) {
        debounce = kDebounce;
        begin_spoils();
        return;
      }
      if (cursor_memory) cursor_mem = cursor;
      // 0 = "Max. healing without consumables", 1 = "Use consumables in stock too" (the stock list is empty in the retail data)
      heal_apply();
      phase = Phase::Healed;
      pt = 0;
      return;
    }
    // Healed: confirm -> Spoils.
    if (phase == Phase::Healed) {
      play(8);
      begin_spoils();
      return;
    }
    debounce = kDebounce;
    if (phase == Phase::PowerUp) {
      play(8);
      // The first member's gains were applied when the screen opened.
      if (star_page) {
        star_page = false;
        return;
      }
      ++cur;
      if (cur >= static_cast<int>(gain_list.size())) after_powerups();
      else show_member(gain_list[static_cast<size_t>(cur)]);
    } else if (phase == Phase::Spoils) {
      play(8);
      // Exit: black fade over 0x1E frames, the panels slide out, 30 frames. No exit sound.
      phase = Phase::Exit;
      pt = 0;
      if (on_exit) on_exit();
    }
  }
  void begin_spoils() {
    phase = Phase::Spoils;
    pt = 0;
  }
  // SFX 6, straight to Spoils.
  void cancel() {
    if (phase != Phase::Heal || debounce > 0 || age < kSettle) return;
    play(6);
    page_age = 0;
    begin_spoils();
  }
  // dir +1 (down) / -1 (up), wrapping over the three entries.
  void move_cursor(int dir) {
    if (phase != Phase::Heal || age < kSettle) return;
    cursor = (cursor + dir + 3) % 3;
    play(1);
  }
  void tick() {
    ++age, ++pt, ++page_age;
    if (debounce > 0) --debounce;
    if (auto_on && page_age >= kAutoHold && (phase == Phase::PowerUp || phase == Phase::Heal || phase == Phase::Healed || phase == Phase::Spoils)) confirm();
    if (phase == Phase::Exit) {
      slide = slide < 1.0f ? 24.0f : slide * 1.125f;
      if (pt >= kExit) phase = Phase::Done;
    } else {
      // settles from 0x18
      slide -= slide / 8.0f;
      if (slide < 0.5f) slide = 0;
      if (age == kSettle && star_up) play(0x14);
    }
    // glyph brightness 0x40..0x80
    pulse += pulse_dir;
    if (pulse >= 0x80) pulse = 0x80, pulse_dir = -4;
    if (pulse <= 0x40) pulse = 0x40, pulse_dir = 4;
  }
};

BattleResults::BattleResults() : impl_(std::make_unique<Impl>()) {}
BattleResults::~BattleResults() { shutdown(); }

bool BattleResults::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, const data::GameTables& tables, import::Lang lang) {
  Impl& impl = *impl_;
  impl.ctx = &ctx, impl.vfs = &vfs, impl.sprites = &sprites, impl.tables = &tables, impl.lang = lang;
  impl.lt.init(&vfs);
  if (!impl.linear.create(ctx, true) || !impl.nearest.create(ctx, false) || !impl.repeat_linear.create(ctx, true, true) || !impl.repeat_nearest.create(ctx, false, true)) return false;
  if (!impl.text.init(ctx, sprites)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!impl.make_tex(impl.white, 1, 1, px, Impl::kFixed)) return false;
  // The element table of the menus.
  if (auto raw = vfs.read("cdrom/battle/data/menu_pg.bin")) {
    impl.pg.resize(raw->size() / 16);
    for (size_t i = 0; i < impl.pg.size(); ++i) std::copy_n(raw->data() + i * 16, 16, impl.pg[i].begin());
  } else {
    log::warn("battle results: menu_pg.bin missing; the heal menu is never offered");
  }
  impl.fonts.init(vfs, impl.text);
  impl.fonts.set_classic(!impl.hd);
  impl.load_hd_art();
  return true;
}

void BattleResults::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  for (Impl::Tex* tex : {&impl.white, &impl.hd_row, &impl.hd_panel, &impl.hd_up, &impl.hd_star, &impl.hd_gg_on, &impl.hd_gg_off, &impl.hd_equip, &impl.hd_wall, &impl.hd_cursor}) impl.free_tex(*tex);
  for (Impl::Tex& tex : impl.hd_plate) impl.free_tex(tex);
  for (auto& [key, tex] : impl.hd_face) impl.free_tex(*tex);
  impl.hd_face.clear();
  impl.text.destroy();
  impl.linear.destroy(), impl.nearest.destroy(), impl.repeat_linear.destroy(), impl.repeat_nearest.destroy();
  impl.ctx = nullptr;
}

void BattleResults::set_art_mode(bool hd, bool force) {
  Impl& impl = *impl_;
  const bool changed = hd != impl.hd || force;
  impl.hd = hd;
  if (changed && impl.ctx) {
    // A pure rendering swap: re-read every image, the font files and the portraits through the (re-pointed) VFS; the screen state is untouched.
    for (Impl::Tex* tex : {&impl.hd_row, &impl.hd_panel, &impl.hd_up, &impl.hd_star, &impl.hd_gg_on, &impl.hd_gg_off, &impl.hd_equip, &impl.hd_wall, &impl.hd_cursor}) impl.free_tex(*tex);
    for (Impl::Tex& tex : impl.hd_plate) impl.free_tex(tex);
    for (auto& [key, tex] : impl.hd_face) impl.free_tex(*tex);
    impl.hd_face.clear();
    impl.text.destroy();
    impl.text.init(*impl.ctx, *impl.sprites);
    impl.fonts.init(*impl.vfs, impl.text);
    impl.load_hd_art();
  }
  // Only the font files and the portrait folder differ between the modes.
  impl.fonts.set_classic(!hd);
  if (impl.phase != Phase::Idle) impl.prepare_all();
}
void BattleResults::set_filter(bool bilinear) {
  Impl& impl = *impl_;
  if (impl.bilinear == bilinear) return;
  impl.bilinear = bilinear;
  if (!impl.ctx) return;
  auto rebind = [&](Impl::Tex& tex) {
    if (!tex.ok()) return;
    tex.set = render::TexRef(tex.tex, impl.sampler_for(tex.kind));
  };
  for (Impl::Tex* tex : {&impl.hd_row, &impl.hd_panel, &impl.hd_up, &impl.hd_star, &impl.hd_gg_on, &impl.hd_gg_off, &impl.hd_equip, &impl.hd_wall, &impl.hd_cursor}) rebind(*tex);
  for (Impl::Tex& tex : impl.hd_plate) rebind(tex);
  for (auto& [key, tex] : impl.hd_face) rebind(*tex);
}
void BattleResults::set_cursor_memory(bool on) { impl_->cursor_memory = on; }
bool BattleResults::take_final_hp(std::array<int, battle::kPcSlots>& hp) {
  Impl& impl = *impl_;
  hp.fill(-1);
  if (!impl.final_ready) return false;
  impl.final_ready = false;
  for (size_t i = 0; i < impl.rows.size(); ++i)
    if (impl.rows[i].present) hp[i] = std::clamp(impl.rows[i].hp, 0, 999);
  return true;
}
int BattleResults::heal_cursor() const { return impl_->phase == Phase::Heal ? impl_->cursor : -1; }
void BattleResults::set_language(import::Lang lang) { impl_->lang = lang; }
void BattleResults::set_portrait_set(const std::string& set) {
  Impl& impl = *impl_;
  const std::string normalised = set == "1999" ? "1999" : "2021";
  if (normalised == impl.portrait_set) return;
  impl.portrait_set = normalised;
  for (auto& [key, tex] : impl.hd_face) impl.free_tex(*tex);
  impl.hd_face.clear();
}
void BattleResults::set_sfx(std::function<void(int)> fn) { impl_->sfx = std::move(fn); }
void BattleResults::set_on_exit(std::function<void()> fn) { impl_->on_exit = std::move(fn); }
void BattleResults::set_auto(bool on) { impl_->auto_on = on; }

bool BattleResults::open(const battle::Battle& finished, int scene_id) {
  Impl& impl = *impl_;
  // Result codes 1 / 3 / 4 and scene 0x1DD show no screen; a flight or a scripted code 5+ shows it without rewards (they are a victory's).
  const battle::Outcome outcome = finished.outcome();
  const int code = finished.result_code();
  if (!impl.ctx || code == 1 || code == 3 || code == 4 || scene_id == 0x1DD) return false;
  if (outcome != battle::Outcome::Victory && outcome != battle::Outcome::Fled && outcome != battle::Outcome::Scripted) return false;
  impl.muted = scene_id == 0x1D8 && code != 2;
  const battle::Rewards& rewards = finished.rewards();
  impl.rows = {};
  impl.gain_list.clear();
  for (size_t i = 0; i < finished.party().size() && i < battle::kPcSlots; ++i) {
    const battle::PartyMember& member = finished.party()[i];
    const battle::Unit& unit = finished.unit(static_cast<int>(i));
    Impl::Row& row = impl.rows[i];
    row.present = unit.present;
    if (!row.present) continue;
    row.char_id = member.char_id;
    row.name = import::utf8_to_u32(member.name);
    row.hp = unit.hp;
    row.max_hp = member.stat[0];
    row.stat = member.stat;
    row.gain = rewards.gains[i];
    row.cells = member.cells;
    row.gauge = unit.gauge;
    row.ko = !unit.alive();
    row.unit_cells = unit.cells;
    for (size_t cell = 0; cell < row.open.size(); ++cell) row.open[cell] = unit.usable[cell] && !unit.used[cell];
    for (int gain : row.gain) row.has_gain = row.has_gain || gain > 0;
    if (row.has_gain) impl.gain_list.push_back(static_cast<int>(i));
  }
  // Debug: start the screen with every living member at this HP (heal menu tests).
  if (const char* hurt = std::getenv("OC_RESULTS_HURT")) {
    const int hp = std::atoi(hurt);
    for (Impl::Row& row : impl.rows)
      if (row.present && !row.ko) row.hp = std::min(row.hp, std::max(1, hp));
  }
  // Debug: put these element ids (comma list) into the first open cells of every living member.
  if (const char* ids = std::getenv("OC_RESULTS_HEALER")) {
    std::vector<int> list;
    for (const char* cursor = ids; *cursor;) {
      list.push_back(std::atoi(cursor));
      while (*cursor && *cursor != ',') ++cursor;
      if (*cursor == ',') ++cursor;
    }
    for (Impl::Row& row : impl.rows) {
      if (!row.present || row.ko) continue;
      row.gauge = std::max(row.gauge, 0);
      for (size_t i = 0; i < list.size() && i < row.open.size(); ++i) row.unit_cells[i] = static_cast<u16>(list[i]), row.open[i] = true;
    }
  }
  impl.gold = rewards.gold;
  impl.star_up = rewards.star_up;
  impl.star = finished.growth().star;
  impl.counter = finished.growth().counter;
  impl.drops.clear();
  for (u16 id : rewards.items) {
    Impl::Drop drop;
    drop.id = id;
    drop.name = strip_icon(import::utf8_to_u32(impl.tables->item_name(id)));
    if (const data::ItemInfo* info = impl.tables->item(id)) drop.kind = info->kind();
    impl.drops.push_back(std::move(drop));
  }
  for (u16 id : rewards.elements) {
    Impl::Drop drop;
    drop.element = true;
    drop.id = id & 0x7F;
    drop.name = strip_icon(import::utf8_to_u32(impl.tables->element_name(drop.id)));
    if (static_cast<size_t>(drop.id) < impl.tables->elements.size()) drop.colour = data::colour_index_of_bit(impl.tables->elements[static_cast<size_t>(drop.id)].colour_bit());
    impl.drops.push_back(std::move(drop));
  }
  impl.phase = Phase::PowerUp;
  impl.age = impl.pt = impl.debounce = 0;
  impl.acc = 0;
  impl.slide = 24.0f;
  impl.cur = 0;
  impl.star_page = impl.star_up;
  impl.healers.clear(), impl.used.clear();
  impl.final_ready = false;
  // The first member's gains are applied even under the star page.
  if (!impl.gain_list.empty()) impl.show_member(impl.gain_list[0]);
  impl.pulse = 0x40, impl.pulse_dir = 4;
  impl.page_age = 0;
  impl.draws = 0;
  impl.prepare_all();
  impl.play(0x47);
  log::info("battle results: gold {} items {} elements {} gains for {} members{}", impl.gold, rewards.items.size(), rewards.elements.size(), impl.gain_list.size(),
            impl.star_up ? " (star up)" : "");
  return true;
}

void BattleResults::update(float dt, const platform::Input& in) {
  Impl& impl = *impl_;
  if (impl.phase == Phase::Idle || impl.phase == Phase::Done) return;
  using platform::Action;
  const bool ok = platform::action_pressed(in, Action::Ok) || in.pad_pressed[SDL_GAMEPAD_BUTTON_SOUTH];
  const bool up = platform::action_pressed(in, Action::Up) || in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_UP];
  const bool down = platform::action_pressed(in, Action::Down) || in.pad_pressed[SDL_GAMEPAD_BUTTON_DPAD_DOWN];
  const bool cancel = platform::action_pressed(in, Action::Cancel) || in.pad_pressed[SDL_GAMEPAD_BUTTON_EAST];
  impl.acc += dt * 60.0f;
  int steps = 0;
  while (impl.acc >= 1.0f && steps++ < 8) {
    impl.tick();
    impl.acc -= 1.0f;
  }
  if (up) impl.move_cursor(-1);
  else if (down) impl.move_cursor(1);
  if (ok) impl.confirm();
  else if (cancel) impl.cancel();
  if (impl.phase == Phase::Done) impl.final_ready = true;
}

void BattleResults::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  if (impl.phase == Phase::Idle || impl.phase == Phase::Done || !impl.ctx) return;
  const render::Letterbox lb = render::Letterbox::wide(vw, vh, impl.canvas_cols);
  impl.zoom = lb.scale / SX, impl.ox = lb.ox, impl.oy = lb.oy;
  impl.edge_x0 = lb.x0, impl.edge_w = lb.w;
  if (impl.draws++ == 0 || impl.pt == 0)
    log::debug("battle results: phase {} at age {} (hd {}, art: wall {} row {} panel {})", static_cast<int>(impl.phase), impl.age, impl.hd, impl.hd_wall.ok(),
               impl.hd_row.ok(), impl.hd_panel.ok());
  impl.sprites->begin(frame, vw, vh, resume);
  impl.shift = 0;
  impl.draw_wall();
  impl.shift = -impl.layout_cols;
  impl.draw_faces();
  for (int slot = 0; slot < battle::kPcSlots; ++slot)
    if (impl.rows[static_cast<size_t>(slot)].present) impl.draw_row(slot);
  impl.shift = impl.layout_cols;
  impl.draw_panel();
  impl.draw_headline(impl.phase == Phase::Spoils);
  impl.shift = 0;
  // fades: from black over the first 0x3C frames, to black over the exit's 0x1E frames
  if (impl.age < kFadeIn) impl.rect(impl.edge_x0, 0, impl.edge_w, 216, rgba(0, 0, 0, 255 - 255 * impl.age / kFadeIn));
  if (impl.phase == Phase::Exit) impl.rect(impl.edge_x0, 0, impl.edge_w, 216, rgba(0, 0, 0, std::min(255, 255 * impl.pt / kExit)));
  impl.sprites->end();
}

void BattleResults::set_wide_columns(float canvas_columns, float layout_columns) {
  impl_->canvas_cols = std::max(0.0f, canvas_columns);
  impl_->layout_cols = std::clamp(layout_columns, 0.0f, impl_->canvas_cols);
}
render::TextBatch* BattleResults::text_batch() { return &impl_->text; }
bool BattleResults::active() const { return impl_->phase != Phase::Idle && impl_->phase != Phase::Done; }
bool BattleResults::done() const { return impl_->phase == Phase::Done; }
void BattleResults::finish_intro() {
  Impl& impl = *impl_;
  if (impl.phase == Phase::PowerUp || impl.phase == Phase::Spoils) {
    impl.age = std::max(impl.age, kFadeIn);
    impl.slide = 0;
  }
}
BattleResults::Phase BattleResults::phase() const { return impl_->phase; }

}  // namespace oc::game
