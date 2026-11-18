#include "game/battle/battle_results.h"
#include "render/camera.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <vector>

#include "core/log.h"
#include "data/char_name.h"
#include "game/battle/hud_sheets.h"
#include "import/battle_hud.h"
#include "import/ps1_font.h"
#include "import/ps1_text.h"
#include "import/tim.h"
#include "platform/keymap.h"
#include "render/text_batch.h"
#include "render/texture.h"
#include "ui/localized_path.h"
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
constexpr float kGoldRight = 256.5f;
// Display lines the wallpaper and the fades cover.
constexpr float kScreenLines = 224.0f;
// Headline face scale (the face's top at the headline's y).
constexpr float kHeadScale = 0.8f;
// Wallpaper tile (btend.prd 'bg', HD battle_37.tim).
constexpr int kTileW = 32, kTileH = 32;

// Stat columns of a left row (panel-relative label x, right edge of the digits, y): STR / HIT / MAG on y = 32, DEF / EVD / MDEF on
// y = 44 (label parts 12 / 10 / 27 and 0 / 8 / 2 of levu frame 2).
struct StatCol {
  int label_x, digits_right, y;
};
// Order: ATK DEF HIT EVD MAG MDEF.
constexpr StatCol kStatCols[6] = {{12, 46, 32}, {7, 46, 44}, {56, 94, 32}, {56, 94, 44}, {104, 144, 32}, {104, 144, 44}};
// UP markers (sheet levu frame 1) at X0 + kUpX[i] - 8 / y0 + kUpY[i] for i = HP, ATK, DEF, HIT, EVD, MAG, MDEF.
constexpr int kUpX[7] = {20, 20, 20, 68, 68, 116, 116}, kUpY[7] = {12, 24, 36, 24, 36, 24, 36};
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
// Item kind (category low nibble) of the forge materials.
constexpr int kMaterialKind = 4;
// Spoils item names wider than this (PS1 units) are drawn scaled down to it, both ways, about the point kNarrowAnchor lines below
// the name's y.
constexpr float kItemNameMaxW = 102.0f, kNarrowAnchor = 18.0f;

// Disc mode: the PS1's own screen. Sheet types of battle/btend.prd (import::kResultsSheetNames; the colour plates are types 0..0x14 by
// colour index).
constexpr int kSheetGrid = 0x18, kSheetCursor = 0x1C, kSheetFont = 0x20, kSheetHeadline = 0x24, kSheetItemIcon = 0x28, kSheetPanels = 0x2C,
              kSheetCount = 0x38;
// Row cells from the row origin: the six stats' two digit slots (ATK DEF HIT EVD MAG MDEF; x - 6 + 8 j) and the UP markers (kUpX - 8,
// kUpY). The gold digits (seven slots) from x 0xB8 (USA 0xD4) on the panel's line + 0x90, the separators at 0xC0 / 0xE0 (USA 0xDC / 0xFC).
constexpr int kDigitX[6] = {40, 40, 88, 88, 136, 136}, kDigitY[6] = {32, 44, 32, 44, 32, 44};
constexpr int kGoldX[7] = {0, 16, 24, 32, 48, 56, 64};
// Heal question: its eight lines at (0xA8 + x, 0x32 + y); the cursor at (0xA8, 0x72 + 16 row) for the three choices.
constexpr int kHealX[8] = {4, 4, 4, 8, 8, 8, 8, 8}, kHealY[8] = {10, 26, 42, 62, 78, 94, 110, 126};
constexpr int kHealCursorRow[3] = {0, 2, 4};
// The results messages (battle/data/exdat.dat): 1..10 the digits, 0x0B.. the seven stat-up labels, 0x12.. the heal question's eight lines,
// the star page's Received, arrow, slash, "and friends" (alone: 0x24), the two growth lines (0x20 / 0x21 at 99 stars), the healed and
// spoils headers.
constexpr int kMsgDigit0 = 1, kMsgStatUp = 0x0B, kMsgHeal = 0x12, kMsgReceived = 0x1A, kMsgArrow = 0x1B, kMsgSlash = 0x1C, kMsgFriends = 0x1D,
              kMsgGrowth = 0x1E, kMsgGrowth99 = 0x20, kMsgHealed = 0x22, kMsgSpoils = 0x23, kMsgAlone = 0x24;

// The nine decimal digits of a value, most significant first; the leading zeros before the last digit are -1.
std::array<int, 9> split_digits(u32 value) {
  std::array<int, 9> digits{};
  for (int i = 8; i >= 0; --i) {
    digits[static_cast<size_t>(i)] = static_cast<int>(value % 10);
    value /= 10;
  }
  for (size_t i = 0; i < 8 && digits[i] == 0; ++i) digits[i] = -1;
  return digits;
}

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
  // Sampler choice: filtered by [Video] Filter*, filtered + repeating (wallpaper), always nearest (the white pixel, the disc art), always
  // nearest + repeating (the disc's wallpaper).
  enum Kind { kLin, kRep, kFixed, kFixedRep };
  struct Tex {
    render::Texture tex;
    render::TexRef set;
    float w = 1, h = 1;
    Kind kind = kLin;
    bool ok() const { return static_cast<bool>(set); }
  };
  struct Row {
    bool present = false;
    // The portrait strip's number: the body up to Lynx's, else the character (data::menu_form).
    int portrait = 0;
    std::u32string name;
    int hp = 0, max_hp = 0;
    std::array<int, 7> stat{}, gain{};
    // shown: this member's PowerUp page was reached (stats + UP markers switch to the new values).
    bool has_gain = false, shown = false;
    std::array<u16, data::kGridCells> cells{};
    int gauge = -1;
    // KO (status 0x80): no gains (unless the shared-growth cheat is on), not healed.
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

  // Disc mode (no hd archive): the PS1's screen, drawn from battle/btend.prd's sheets at the PS1's coordinates (the semi-transparent
  // texels and the others in two atlases), the wallpaper tile, the portraits battle/face/pNN.tim, the messages of battle/data/exdat.dat
  // in the PS1 font.
  bool disc = false, usa = false;
  std::vector<std::vector<SheetFrame>> sheet_frames;
  Tex sheet_semi, sheet_opaque, disc_wall;
  std::vector<std::u32string> messages;
  // Item id -> icon (sheet "sico" frame icon + 1).
  std::vector<u8> item_icons;
  import::JpGlyphs glyph_codes;
  std::shared_ptr<const import::Ps1Font> ps1_font;

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
    if (kind == kFixedRep) return repeat_nearest;
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
    load_png(hd_up, ui::localized_path(*vfs, "hd/battle/btend/up.png", lang));
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
  void load_art() {
    if (disc) load_disc_art();
    else load_hd_art();
  }
  void load_disc_art() {
    auto prd = vfs->read("cdrom/battle/btend.prd");
    std::optional<import::ResultsArt> art;
    if (prd) art = import::load_results_art(*prd);
    if (!art) {
      log::warn("battle results: the disc's results sheets (btend.prd) are missing");
      return;
    }
    SheetAtlas atlas = pack_sheets(art->sheets);
    sheet_frames = std::move(atlas.frames);
    make_tex(sheet_semi, static_cast<u32>(atlas.width), static_cast<u32>(atlas.height), atlas.semi.data(), kFixed);
    make_tex(sheet_opaque, static_cast<u32>(atlas.width), static_cast<u32>(atlas.height), atlas.opaque.data(), kFixed);
    if (art->wall.w > 0) {
      std::vector<u8> wall = art->wall.rgba;
      for (size_t i = 3; i < wall.size(); i += 4) wall[i] = wall[i] ? 255 : 0;
      make_tex(disc_wall, static_cast<u32>(art->wall.w), static_cast<u32>(art->wall.h), wall.data(), kFixedRep);
    }
    ps1_font = import::load_ps1_font(*vfs);
    usa = ps1_font && ps1_font->build() == import::Ps1Font::Build::Usa;
    messages.clear();
    item_icons.clear();
    import::Ps1Text source;
    if (auto exdat = vfs->read("cdrom/battle/data/exdat.dat"); exdat && source.open(*vfs)) {
      for (const Bytes& message : import::load_exit_messages(*exdat, !usa)) messages.push_back(import::utf8_to_u32(source.markup(message)));
      item_icons = import::load_exit_item_icons(*exdat);
    }
    if (messages.empty()) log::warn("battle results: the disc's results messages (exdat.dat) are missing");
  }
  Tex* face(int portrait_id) {
    char key[80];
    std::snprintf(key, sizeof key, "%s/p%02d", disc ? "disc" : hd ? portrait_set.c_str() : "1999", portrait_id);
    if (auto it = hd_face.find(key); it != hd_face.end()) return it->second->ok() ? it->second.get() : nullptr;
    auto tex = std::make_unique<Tex>();
    if (disc) {
      if (auto raw = vfs->read(std::format("cdrom/battle/face/p{:02}.tim", portrait_id))) load_tim(*tex, *raw, kFixed);
    } else {
      load_png(*tex, std::string("hd/battle/face/") + key + ".png");
    }
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
  // draw scale (offX / offY * scale) is added to the PS1 position. `exact`: render::TextStyle::exact.
  void txt(int slot, const std::u32string& text_u32, float x, float y, u32 colour, float ps1_px, int align = 0, bool exact = false) {
    if (text_u32.empty()) return;
    const float sc = scale_for(ps1_px);
    const float w = measure(slot, text_u32, sc);
    if (align == 1) x -= w * 0.5f;
    if (align == 2) x -= w;
    const ui::font::SlotInfo& si = ui::font::slot(slot);
    const ui::font::Ink ink{colour, rgba(33, 33, 33, static_cast<int>(colour >> 24)), true, false, exact};
    fonts.draw(slot, text_u32, (x + shift) * SX + si.off_x * sc + ox / zoom, y * SY + si.off_y * sc + oy / zoom, ink, sc, ui::font::kLeft, zoom);
  }
  // Names and numbers drawn as 'osak' glyphs: kb_70 text.
  void big(const std::u32string& text_u32, float x, float y, u32 colour, int align = 0) { txt(kDigits, text_u32, x, y, colour, kBigPx, align); }
  void num(int value, float right, float y, u32 colour = kColDigit) { big(import::utf8_to_u32(std::to_string(value)), right, y, colour, 2); }

  void draw_wall() {
    if (hd_wall.ok()) quad(hd_wall, edge_x0, 0, edge_w, kScreenLines, 0xFFFFFFFFu, edge_x0 / kTileW, 0, (edge_x0 + edge_w) / kTileW, kScreenLines / kTileH);
  }
  void draw_faces() {
    // Three 32 x 224 strips at x = 0x10 + 0x28 s, drawn first (under the rows) at half brightness.
    for (int slot = 0; slot < battle::kPcSlots; ++slot) {
      const Row& row = rows[static_cast<size_t>(slot)];
      if (!row.present) continue;
      if (Tex* portrait = face(row.portrait)) quad(*portrait, static_cast<float>(kFaceX0 + kFaceDX * slot), 0, kFaceW, kFaceH, rgba(128, 128, 128));
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
    txt(label_slot(), U"/", x0 + 54.75f, y0 + 21, kColText, kMidPx);
    const int max_hp = after ? row.max_hp : row.max_hp - row.gain[0];
    num(std::clamp(row.hp, 0, 9999), x0 + 54, y0 + 20);
    num(max_hp, x0 + 82, y0 + 20);
    // six stats (two rows of three)
    for (int k = 0; k < 6; ++k) {
      const StatCol& col = kStatCols[k];
      const int value = after ? row.stat[static_cast<size_t>(k + 1)] : row.stat[static_cast<size_t>(k + 1)] - row.gain[static_cast<size_t>(k + 1)];
      txt(label_slot(), stat_label(k + 1), x0 + static_cast<float>(col.label_x) - (lang != import::Lang::Jp && k == 0 ? 5.0f : 0.0f), y0 + static_cast<float>(col.y), kColText, kLabelPx);
      num(std::clamp(value, 0, 99), x0 + static_cast<float>(col.digits_right), y0 + static_cast<float>(col.y));
    }
    // "UP" marker (levu frame 1 = btend/up.png 16 x 16) on every stat with a non-zero gain (a stat pulled back to its Lv 99 value too),
    // once the member's page was shown:
    // sprite at X0 + kUpX[i] - 8, y0 + kUpY[i] with i = HP, ATK, DEF, HIT, EVD, MAG, MDEF
    if (after)
      for (int i = 0; i < 7; ++i)
        if (row.gain[static_cast<size_t>(i)] != 0) draw_up(x0 + static_cast<float>(kUpX[i] - 8), y0 + static_cast<float>(kUpY[i]));
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
    // Headline (viva frame 5 / 6) right-aligned at (0x134, 0x10), hidden while sliding out. Drawn with the ch_100 face at 0.8
    // (same file in both art modes) from btlexit.txt 0x10005 / 6: text 0xD5C7B8 over an outline 0x604E41, both added to the
    // picture at half strength.
    if (phase == Phase::Exit) return;
    const float x = static_cast<float>(kHeadX), y = static_cast<float>(kHeadY);
    if (const render::FaceId face_ch = fonts.face(kHead); face_ch != render::kNoFace) {
      const std::u32string headline = label(spoils ? 0x10006 : 0x10005);
      const float sc = kHeadScale;
      const float w = measure(kHead, headline, sc);
      render::TextStyle style;
      style.backdrop_color = rgba(0x60, 0x4E, 0x41);
      // The outline pass covers the glyph too, so the fill adds only the difference.
      style.shadow = false;
      const render::BlendMode old_blend = sprites->blend();
      sprites->set_blend(render::BlendMode::AddAlpha);
      text.draw(face_ch, headline, cx(x - w), cy(y), rgba(0xD5 - 0x60, 0xC7 - 0x4E, 0xB8 - 0x41, 0x80), sc * zoom, style);
      sprites->set_blend(old_blend);
      return;
    }
  }
  void draw_panel() {
    const float px = static_cast<float>(kPanelX), py = static_cast<float>(kPanelY) + slide;
    quad(hd_panel, px - 16, py, kPanelW, kPanelH);
    // gold line: osak digits ending before the "G" part (0x30, panel +88 - 7), then 獲得 / "Received" (part 0x32, +96 - 7)
    num(std::clamp(gold, 0, 9999999), kGoldRight, py + 144, kColDigit);
    txt(label_slot(), U"G", px + 81, py + 145, kColText, kMidPx);
    txt(label_slot(), label(26), px + 89, py + 145, kColText, kMidPx);
    if (phase == Phase::Spoils) draw_spoils(px, py);
    else if (phase == Phase::Heal) draw_heal_menu(px, py);
    else if (phase == Phase::Healed) draw_healed(px, py);
    else if (star_page) draw_star_page(px, py);
    else draw_powerup(px, py);
  }
  void draw_powerup(float px, float py) {
    // Member name at (0xA5, 0x42); one line per stat with a non-zero gain: label (key 11 + k) at (0xC0, 0x52 + 0xE n), the gain (no
    // sign) left-aligned at 0xF7. The digits are those of the gain as an unsigned 32-bit number, so a negative gain shows the last
    // three digits of 2^32 + gain, leading zeros included.
    // Nobody grew: only the slots with a gain are drawn, so the blank page keeps its headline and shows no name and no lines.
    if (cur >= static_cast<int>(gain_list.size())) return;
    const int slot = gain_list[static_cast<size_t>(cur)];
    const Row& row = rows[static_cast<size_t>(slot)];
    txt(label_slot(), row.name, px - 11, py + 16, kColName, kPanelPx);
    int line = 0;
    for (int k = 0; k < 7; ++k) {
      if (row.gain[static_cast<size_t>(k)] == 0) continue;
      const float y = py + 32 + 14.0f * static_cast<float>(line++);
      txt(label_slot(), label(11 + static_cast<u64>(k)), px + 16, y, kColText, kPanelPx);
      // the gain digits are 7.3 lines tall = the kb font at 0.5, not the 0.35 of the row digits
      const int gain = row.gain[static_cast<size_t>(k)];
      const std::string digits = gain > 0 ? std::to_string(gain) : std::format("{:03}", static_cast<u32>(gain) % 1000u);
      txt(kDigits, import::utf8_to_u32(digits), px + 71, y, kColDigit, kPanelPx);
    }
  }
  void draw_star_page(float px, float py) {
    // Pulsing star (levu 4 / btend/star.png) at (0xB4, 0x3C); "Received" (key 26) at (0xCC, 0x41); "->" (27) at (0xD8, 0x51), the
    // star-up counter at 0xE4, "/" (28) at 0xFC and the star level at 0x108 (y 0x51; the numbers in the label face, left-aligned); the
    // growth message (key 30, or 32 at star 99) at (0xB4, 0x92) and (0xB4, 0xA2). No name line.
    const float brightness = static_cast<float>(pulse) / 128.0f;
    quad(hd_star, px + 4, py + 10, 20, 20, tint(0xFFFFFFFFu, brightness));
    txt(label_slot(), label(26), px + 28, py + 16, kColText, kPanelPx);
    txt(label_slot(), label(27), px + 40, py + 32, kColText, kPanelPx);
    txt(label_slot(), import::utf8_to_u32(std::to_string(counter)), px + 52, py + 32, kColText, kPanelPx);
    txt(label_slot(), label(28), px + 76, py + 32, kColText, kPanelPx);
    txt(label_slot(), import::utf8_to_u32(std::to_string(star)), px + 88, py + 32, kColText, kPanelPx);
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
  // second line 16 lines below; the cursor 'curs' frame 1 / 2 (alternating every 16 frames) at (0xA8 / 0xA6, 0x72 + 0x20 * cursor). Non-English Latin texts of the choices
  // are drawn at x 0.85.
  void draw_heal_menu(float /*px*/, float py) {
    const float dy = py - static_cast<float>(kPanelY);
    lines(label(18), 0xAC, 0x3C + dy, kPanelPx);
    const bool small = lang != import::Lang::En && lang != import::Lang::Jp;
    for (int i = 0; i < 3; ++i) lines(label(21 + 2 * static_cast<u64>(i)), 0xB0, 0x70 + 0x20 * static_cast<float>(i) + dy, small ? kPanelPx * 0.85f : kPanelPx);
    if (hd_cursor.ok()) {
      const bool second = ((age >> 4) & 1) != 0;
      const float y = 0x72 + 0x20 * static_cast<float>(cursor) + dy;
      if (!second) quad(hd_cursor, 0xA8, y, 9, 8, 0xFFFFFFFFu, 0, 0, 0.5625f, 1);
      else quad(hd_cursor, 0xA6, y, 7, 8, 0xFFFFFFFFu, 0.5625f, 0, 1, 1);
    }
  }
  // Healed page: header key 34 at (0xC0, 0x3E); one row per healing element used, y = 0x4E + 0x12 n: the colour plate (menu_pg +B colour, sheet frame 3) at
  // (0xAC, row - 1), the element name centred on 0xE8, "x" at 0x118 and the number of uses (tens at 0x11E, ones at 0x11E or 0x124).
  void draw_healed(float px, float py) {
    txt(label_slot(), label(34), px + 16, py + 12, kColText, kPanelPx);
    int line = 0;
    for (const Healer& healer : used) {
      const float y = py + 28 + 18.0f * static_cast<float>(line++);
      const int ci = plate_of(healer.id);
      if (ci >= 0) quad(hd_plate[static_cast<size_t>(ci)], px - 4, y, 120, 16);
      txt(label_slot(), import::utf8_to_u32(tables->element_name(healer.id)), 0xE8, y + 3, kColText, kPanelPx, 1);
      txt(label_slot(), U"x", 0x118, y + 6, kColText, kLabelPx);
      txt(kDigits, import::utf8_to_u32(std::to_string(healer.count)), 0x11E - 1, y + 6, kColDigit, kLabelPx);
    }
  }
  void draw_spoils(float px, float py) {
    // Header (key 35) at (0xBA, 0x3E); row n at y = 0x4E + 0x12 n, in drop order: item icon (sico kind + 1 / EquipIcon cell) at 0xB0
    // and the name left-aligned at (0xC2, row + 2); element: the colour plate (sheet colour << 2, frame 3 / battle_08_NN_03) at 0xAC
    // and the name centred on x = 0xE8 at row + 4. Names keep their sysmes prefix glyph (alpha consumable, gamma trap, delta
    // material), which the fonts draw as the kind's icon left of the name. A material has no item icon of its own (its sico frame is empty).
    // A long item name is narrowed to kItemNameMaxW; element names are drawn full size.
    txt(label_slot(), label(35), px + 10, py + 12, kColText, kPanelPx);
    int line = 0;
    for (const Drop& drop : drops) {
      if (line >= 6) break;
      const float y = py + 28 + 18.0f * static_cast<float>(line++);
      if (drop.element) {
        const int ci = std::clamp(drop.colour, 0, 5);
        quad(hd_plate[static_cast<size_t>(ci)], px - 4, y + 1, 120, 16);
        txt(label_slot(), drop.name, 0xE8, y + 4, kColName, kPanelPx, 1);
      } else {
        if (drop.kind != kMaterialKind) {
          const float u0 = 0.0075f + 0.2f * static_cast<float>(std::clamp(drop.kind, 0, 4));
          quad(hd_equip, px, y, 16, 16, 0xFFFFFFFFu, u0, 0.0075f, u0 + 0.185f, 0.9925f);
        }
        const float w = measure(label_slot(), drop.name, scale_for(kPanelPx));
        const bool narrow = w > kItemNameMaxW;
        const float px_h = narrow ? kPanelPx * kItemNameMaxW / w : kPanelPx;
        txt(label_slot(), drop.name, 0xC2, y + 2 + kNarrowAnchor * (1.0f - px_h / kPanelPx), kColName, px_h, 0, narrow);
      }
    }
    if (drops.empty()) txt(label_slot(), U"-", 0xE8, py + 31, kColText, kPanelPx, 1);
  }

  // ---- disc mode drawing (PS1 coordinates) ----------------------------------------------------------------------------------
  // A sheet frame at PS1 (x, y) in colour word `colour` (texel x colour / 0x80, up to twice as bright) and mode 0 (opaque) or 1..4
  // (ABR mode - 1: (B + F) / 2, B + F, B - F, B + F / 4), which as on the PS1 blends only the texels with the semi-transparency bit.
  void sheet(int type, int frame, float x, float y, u32 colour = 0x808080, int mode = 0) {
    const size_t index = static_cast<size_t>(type / 4);
    if (type < 0 || index >= sheet_frames.size() || frame < 1 || static_cast<size_t>(frame) > sheet_frames[index].size()) return;
    const SheetFrame& picture = sheet_frames[index][static_cast<size_t>(frame - 1)];
    if (picture.w <= 0) return;
    const float px = cx(x + static_cast<float>(picture.x)), py = cy(y + static_cast<float>(picture.y));
    const float pw = static_cast<float>(picture.w) * SX * zoom, ph = static_cast<float>(picture.h) * SY * zoom;
    // Per channel the colour's factor (0x80 = 1) split into the part up to 1 and the brightening above it.
    auto tint = [&](float divide, bool over) {
      auto channel = [&](int shift) {
        const float factor = static_cast<float>((colour >> shift) & 0xFF) / 128.0f / divide;
        return static_cast<int>(std::lround(255.0f * std::clamp(over ? factor - 1.0f : factor, 0.0f, 1.0f)));
      };
      return rgba(channel(0), channel(8), channel(16));
    };
    const bool bright = ((colour & 0xFF) > 0x80) || (((colour >> 8) & 0xFF) > 0x80) || (((colour >> 16) & 0xFF) > 0x80);
    auto draw = [&](const Tex& tex, u32 col, render::BlendMode blend) {
      if (!tex.ok()) return;
      sprites->set_blend(blend);
      sprites->draw(tex.set, px, py, pw, ph, picture.u0, picture.v0, picture.u1, picture.v1, col);
    };
    switch (mode == 0 ? -1 : (mode - 1) & 3) {
      case 0: draw(sheet_semi, (tint(1, false) & 0x00FFFFFFu) | 0x80000000u, render::BlendMode::Alpha); break;
      case 1: draw(sheet_semi, tint(1, false), render::BlendMode::Add); break;
      case 2: draw(sheet_semi, tint(1, false), render::BlendMode::Subtract); break;
      case 3: draw(sheet_semi, tint(4, false), render::BlendMode::Add); break;
      default: draw(sheet_semi, tint(1, false), render::BlendMode::Alpha); break;
    }
    draw(sheet_opaque, tint(1, false), render::BlendMode::Alpha);
    if (bright) draw(sheet_opaque, tint(1, true), render::BlendMode::Add);
    sprites->set_blend(render::BlendMode::Alpha);
  }
  float by_build(int usa_value, int japan_value) const { return static_cast<float>(usa ? usa_value : japan_value); }
  size_t font_frames() const { return static_cast<size_t>(kSheetFont / 4) < sheet_frames.size() ? sheet_frames[kSheetFont / 4].size() : 0; }
  std::u32string message(int index) const { return index >= 0 && static_cast<size_t>(index) < messages.size() ? messages[static_cast<size_t>(index)] : std::u32string(); }
  // PS1 font text from PS1 (x, y): white with the black shadow one pixel down and right.
  void ps1_text(const std::u32string& text_u32, float x, float y) {
    if (text_u32.empty()) return;
    const ui::font::Ink ink{rgba(255, 255, 255), rgba(0, 0, 0), true, false};
    fonts.draw(ui::font::kBn, text_u32, (x + shift) * SX + ox / zoom, y * SY + oy / zoom, ink, SX / render::TextBatch::kPs1PixelScale, ui::font::kLeft, zoom);
  }
  float ps1_width(const std::u32string& text_u32) { return fonts.measure(ui::font::kBn, text_u32, SX / render::TextBatch::kPs1PixelScale) / SX; }
  // The digits of `value` as digit messages 12 px apart from x; `slots` = the value's last digits drawn, the leading blank ones left out.
  float ps1_digits(u32 value, int slots, float x, float y) {
    const std::array<int, 9> digits = split_digits(value);
    for (size_t i = 9 - static_cast<size_t>(slots); i < 9; ++i)
      if (digits[i] >= 0) ps1_text(message(kMsgDigit0 + digits[i]), x, y), x += 12;
    return x;
  }
  // A name in the battle font from (x, y): at most six glyphs, 9 px apart (USA: the PS1 font's advance of each character).
  void sheet_name(const std::u32string& name, float x, float y) {
    int count = 0;
    for (char32_t ch : name) {
      if (count++ >= 6) break;
      if (const int frame = font_frame(ch, glyph_codes, font_frames())) sheet(kSheetFont, frame, x, y);
      x += usa && ps1_font ? static_cast<float>(ps1_font->advance(ps1_font->index_for(ch))) : 9.0f;
    }
  }
  void disc_wall_draw() {
    if (disc_wall.ok()) quad(disc_wall, edge_x0, 0, edge_w, 216, 0xFFFFFFFFu, edge_x0 / disc_wall.w, 0, (edge_x0 + edge_w) / disc_wall.w, 216.0f / disc_wall.h);
  }
  // The portrait strips (32 x 224 at x 0x10 + 0x28 slot), every texel semi-transparent: half wallpaper, half portrait.
  void disc_faces() {
    for (int slot = 0; slot < battle::kPcSlots; ++slot) {
      const Row& row = rows[static_cast<size_t>(slot)];
      if (!row.present) continue;
      if (Tex* portrait = face(row.portrait)) quad(*portrait, static_cast<float>(kFaceX0 + kFaceDX * slot), 0, kFaceW, kFaceH, rgba(255, 255, 255, 128));
    }
  }
  // A party row at (8, 0x20 + 60 slot) (the slide moves it right; the right panel moves up by as much): the frame with its labels (levu frame 2), the name, HP current / max
  // (the current value's leading zero digits left out, the maximum's last three digits), the six stats (two digits), the UP markers once
  // the member's page was shown, the grid map (a gg cell per occupied cell, bright up to the gauge level, half blended).
  void disc_row(int slot) {
    const Row& row = rows[static_cast<size_t>(slot)];
    const float x0 = static_cast<float>(kRowX) + slide, y0 = static_cast<float>(kRowY0 + kRowPitch * slot);
    sheet(kSheetPanels, 2, x0, y0);
    sheet_name(row.name, x0 + 12, y0 + 8);
    const bool after = row.shown;
    const std::array<int, 9> hp = split_digits(static_cast<u32>(std::clamp(row.hp, 0, 999999)));
    bool started = false;
    for (int j = 0; j < 3; ++j) {
      const int digit = hp[static_cast<size_t>(6 + j)];
      if (digit <= 0 && !started && j != 2) continue;
      started = true;
      sheet(kSheetFont, std::max(digit, 0) + 101, x0 + 0x20 + 8.0f * static_cast<float>(j), y0 + 0x14);
    }
    const std::array<int, 9> top = split_digits(static_cast<u32>(std::max(after ? row.max_hp : row.max_hp - row.gain[0], 0)));
    for (int j = 0, k = 0; j < 3; ++j)
      if (const int digit = top[static_cast<size_t>(6 + j)]; digit >= 0) sheet(kSheetFont, digit + 101, x0 + 0x40 + 8.0f * static_cast<float>(k++), y0 + 0x14);
    for (int k = 0; k < 6; ++k) {
      const int value = after ? row.stat[static_cast<size_t>(k + 1)] : row.stat[static_cast<size_t>(k + 1)] - row.gain[static_cast<size_t>(k + 1)];
      const std::array<int, 9> digits = split_digits(static_cast<u32>(std::clamp(value, 0, 99)));
      for (int j = 0; j < 2; ++j) {
        const int digit = digits[static_cast<size_t>(7 + j)];
        if (digit < 0 && j == 0) continue;
        sheet(kSheetFont, std::max(digit, 0) + 101, x0 + static_cast<float>(kDigitX[k] + 8 * j - 6), y0 + static_cast<float>(kDigitY[k]));
      }
    }
    if (after)
      for (int i = 0; i < 7; ++i)
        if (row.gain[static_cast<size_t>(i)] != 0) sheet(kSheetPanels, 1, x0 + static_cast<float>(kUpX[i] - 8), y0 + static_cast<float>(kUpY[i]), 0x808080, 4);
    for (int level = 0; level < data::kGridRows; ++level)
      for (int cell = 0; cell < data::kGridCols; ++cell) {
        if ((row.cells[static_cast<size_t>(level * data::kGridCols + cell)] & 0x3FFF) == 0) continue;
        sheet(kSheetGrid, level <= row.gauge ? 3 : 2, x0 + 0x66 + 4.0f * static_cast<float>(level), y0 + 6 + 2.0f * static_cast<float>(cell), 0xFFFFFF, 1);
      }
  }
  // The right panel (levu frame 3 at (0xB0, 0x32), sliding in y), the gold line on every page and the page; the headline (viva frame 5
  // PowerUp / 6 Spoils at (0x134, 0x10), added at colour 0x606060) until the exit.
  void disc_panel() {
    const float panel_y = static_cast<float>(kPanelY) - slide, dy = -slide;
    sheet(kSheetPanels, 3, static_cast<float>(kPanelX), panel_y);
    const std::array<int, 9> gold_digits = split_digits(static_cast<u32>(std::clamp(gold, 0, 9999999)));
    const float gold_x = by_build(0xD4, 0xB8);
    bool started = false;
    for (int j = 0; j < 7; ++j) {
      const int digit = gold_digits[static_cast<size_t>(2 + j)];
      if (digit <= 0 && !started && j != 6) continue;
      started = true;
      sheet(kSheetFont, std::max(digit, 0) + 101, gold_x + static_cast<float>(kGoldX[j]), panel_y + 0x90);
    }
    if (gold_digits[2] >= 0) sheet(kSheetFont, 138, by_build(0xDC, 0xC0), panel_y + 0x90);
    if (gold_digits[5] >= 0) sheet(kSheetFont, 138, by_build(0xFC, 0xE0), panel_y + 0x90);
    if (phase == Phase::Spoils) disc_spoils(dy);
    else if (phase == Phase::Heal) disc_heal_menu(dy);
    else if (phase == Phase::Healed) disc_healed(dy);
    else if (star_page) disc_star_page(dy);
    else disc_powerup(dy);
    if (phase != Phase::Exit) sheet(kSheetHeadline, phase == Phase::Spoils ? 6 : 5, 0x134, 0x10, 0x606060, 2);
  }
  // The member's name at (0xB4, 0x42); a line per grown stat from y 0x52, 14 apart: its label at 0xC0 and the gain's last three digits
  // (unsigned) as digit messages from 0xF0, 12 apart.
  void disc_powerup(float dy) {
    if (cur >= static_cast<int>(gain_list.size())) return;
    const Row& row = rows[static_cast<size_t>(gain_list[static_cast<size_t>(cur)])];
    ps1_text(row.name, 0xB4, 0x42 + dy);
    float y = 0x52 + dy;
    for (int k = 0; k < 7; ++k) {
      const int gain = row.gain[static_cast<size_t>(k)];
      if (gain == 0) continue;
      ps1_text(message(kMsgStatUp + k), 0xC0, y);
      ps1_digits(static_cast<u32>(gain), 3, 0xF0, y);
      y += 14;
    }
  }
  // The pulsing star (levu frame 4, added at a quarter), Received, the arrow, the star-up counter, the slash and the star level (digits
  // 12 apart, the tens left out below 10), the leader's name and "and friends" (alone: blanks) after it, the two growth lines.
  // Japanese: star (0xB4, 0x3C), Received 0xCC, arrow 0xD8, counter 0xE4, slash 0xFC, stars 0x108, name and lines from 0xB4. USA: star
  // (0xEC, 0x3C), Received 0xB4, arrow 0xD8, counter tens 0xE8 and units 0xF4, slash 0x100, stars 0x108, name and lines from 0xAC.
  void disc_star_page(float dy) {
    const u32 level = static_cast<u32>(pulse);
    sheet(kSheetPanels, 4, by_build(0xEC, 0xB4), 0x3C + dy, level | (level << 8) | (level << 16), 4);
    ps1_text(message(kMsgReceived), by_build(0xB4, 0xCC), 0x42 + dy);
    const float line = 0x52 + dy;
    ps1_text(message(kMsgArrow), 0xD8, line);
    ps1_text(message(kMsgSlash), by_build(0x100, 0xFC), line);
    const std::array<int, 9> count = split_digits(static_cast<u32>(std::clamp(counter, 0, 99)));
    const std::array<int, 9> stars = split_digits(static_cast<u32>(std::clamp(star, 0, 99)));
    if (count[7] >= 0) ps1_text(message(kMsgDigit0 + count[7]), by_build(0xE8, 0xE4), line);
    ps1_text(message(kMsgDigit0 + count[8]), by_build(0xF4, count[7] >= 0 ? 0xF0 : 0xE4), line);
    if (stars[7] >= 0) ps1_text(message(kMsgDigit0 + stars[7]), 0x108, line);
    ps1_text(message(kMsgDigit0 + stars[8]), static_cast<float>(stars[7] >= 0 ? 0x114 : 0x108), line);
    const float left = by_build(0xAC, 0xB4);
    const int lead = first_present();
    const std::u32string name = lead >= 0 ? rows[static_cast<size_t>(lead)].name : std::u32string();
    ps1_text(name, left, 0x82 + dy);
    const bool friends = rows[1].present;
    const float after_name = usa ? left + ps1_width(name) : left + 12.0f * (static_cast<float>(name.size()) - 6.0f);
    ps1_text(message(friends ? kMsgFriends : kMsgAlone), after_name, 0x82 + dy);
    for (int i = 0; i < 2; ++i) ps1_text(message((star >= 99 ? kMsgGrowth99 : kMsgGrowth) + i), left, 0x92 + 16.0f * static_cast<float>(i) + dy);
  }
  // The question and the three choices (eight message lines) and the blinking cursor (curs frame 1 / 2, 16 frames each).
  void disc_heal_menu(float dy) {
    for (int i = 0; i < 8; ++i) ps1_text(message(kMsgHeal + i), 0xA8 + static_cast<float>(kHealX[i]), 0x32 + static_cast<float>(kHealY[i]) + dy);
    sheet(kSheetCursor, ((age >> 4) & 1) + 1, 0xA8, 0x72 + 16.0f * static_cast<float>(kHealCursorRow[std::clamp(cursor, 0, 2)]) + dy);
  }
  // The header at (0xC0, 0x3E); per healing element used, 18 apart: its colour plate (frame 3 at (0xAC, 0x4E)), the name centred on
  // 0xE8 at y + 2, the "x" (elef frame 10) at (0x118, y + 4) and the count (elef frame digit + 11, tens at 0x11E, units 6 further).
  void disc_healed(float dy) {
    ps1_text(message(kMsgHealed), 0xC0, 0x3E + dy);
    float y = 0x4E + dy;
    for (const Healer& healer : used) {
      const int ci = plate_of(healer.id);
      if (ci >= 0) sheet(ci * 4, 3, 0xAC, y);
      const std::u32string name = strip_icon(import::utf8_to_u32(tables->element_name(healer.id)));
      ps1_text(name, 0xE8 - ps1_width(name) * 0.5f, y + 2);
      sheet(kSheetCount, 10, 0x118, y + 4);
      const std::array<int, 9> digits = split_digits(static_cast<u32>(std::clamp(healer.count, 0, 99)));
      if (digits[7] >= 0) sheet(kSheetCount, digits[7] + 11, 0x11E, y + 4);
      sheet(kSheetCount, digits[8] + 11, static_cast<float>(digits[7] >= 0 ? 0x124 : 0x11E), y + 4);
      y += 18;
    }
  }
  // The header at (0xBA, 0x3E); per drop, 18 apart from y 0x4E: an item's icon (sico frame kind + 1) at 0xB0 and its name at (0xC2,
  // y + 2); an element's colour plate (frame 3) at (0xAC, y + 1) and its name centred on 0xE8 at y + 3.
  void disc_spoils(float dy) {
    ps1_text(message(kMsgSpoils), 0xBA, 0x3E + dy);
    float y = 0x4E + dy;
    int line = 0;
    for (const Drop& drop : drops) {
      if (line++ >= 6) break;
      if (drop.element) {
        sheet(std::clamp(drop.colour, 0, 5) * 4, 3, 0xAC, y + 1);
        ps1_text(drop.name, 0xE8 - ps1_width(drop.name) * 0.5f, y + 3);
      } else {
        const int icon = static_cast<size_t>(drop.id) < item_icons.size() ? item_icons[static_cast<size_t>(drop.id)] : 0;
        sheet(kSheetItemIcon, icon + 1, 0xB0, y);
        ps1_text(drop.name, 0xC2, y + 2);
      }
      y += 18;
    }
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
  // The member's gains are applied to the live values - max HP and current HP both rise by the HP gain (cap 999). A knocked-out
  // member (only with the shared-growth cheat) stays at 0 HP.
  void show_member(int slot) {
    Row& row = rows[static_cast<size_t>(slot)];
    if (row.shown) return;
    row.shown = true;
    if (!row.ko) row.hp = std::min(row.hp + row.gain[0], 999);
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
      // Exit: fade to black over 0x1E frames while the panels slide out. The exit sound (0x41) replaces the confirm sound.
      play(0x41);
      phase = Phase::Exit;
      pt = 0;
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
      // The screen stays black after the fade; the victory music plays on until the field returns.
      if (pt >= kFieldReturn) {
        phase = Phase::Done;
        if (on_exit) on_exit();
      }
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
  impl.disc = !vfs.has_archive("hd");
  // Disc mode draws the PS1 font one font pixel per PS1 pixel of the 320 x 216 screen.
  if (impl.disc) impl.text.set_pixel_aspect(SY / SX);
  impl.fonts.init(vfs, impl.text);
  impl.fonts.set_classic(!impl.hd);
  impl.load_art();
  return true;
}

void BattleResults::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  for (Impl::Tex* tex : {&impl.white, &impl.hd_row, &impl.hd_panel, &impl.hd_up, &impl.hd_star, &impl.hd_gg_on, &impl.hd_gg_off, &impl.hd_equip, &impl.hd_wall, &impl.hd_cursor, &impl.sheet_semi, &impl.sheet_opaque, &impl.disc_wall}) impl.free_tex(*tex);
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
    for (Impl::Tex* tex : {&impl.hd_row, &impl.hd_panel, &impl.hd_up, &impl.hd_star, &impl.hd_gg_on, &impl.hd_gg_off, &impl.hd_equip, &impl.hd_wall, &impl.hd_cursor, &impl.sheet_semi, &impl.sheet_opaque, &impl.disc_wall}) impl.free_tex(*tex);
    for (Impl::Tex& tex : impl.hd_plate) impl.free_tex(tex);
    for (auto& [key, tex] : impl.hd_face) impl.free_tex(*tex);
    impl.hd_face.clear();
    impl.text.destroy();
    impl.text.init(*impl.ctx, *impl.sprites);
    if (impl.disc) impl.text.set_pixel_aspect(SY / SX);
    impl.fonts.init(*impl.vfs, impl.text);
    impl.load_art();
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
  for (Impl::Tex* tex : {&impl.hd_row, &impl.hd_panel, &impl.hd_up, &impl.hd_star, &impl.hd_gg_on, &impl.hd_gg_off, &impl.hd_equip, &impl.hd_wall, &impl.hd_cursor, &impl.sheet_semi, &impl.sheet_opaque, &impl.disc_wall}) rebind(*tex);
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
  // Result codes 1 / 3 / 4, scene 0x1DD and monster-party fights show no screen; a flight or a scripted code 5+ shows it without rewards (they are a victory's).
  const battle::Outcome outcome = finished.outcome();
  const int code = finished.result_code();
  if (!impl.ctx || code == 1 || code == 3 || code == 4 || scene_id == 0x1DD || finished.monster_party()) return false;
  if (outcome != battle::Outcome::Victory && outcome != battle::Outcome::Fled && outcome != battle::Outcome::Scripted) return false;
  impl.muted = scene_id == 0x1D8 && code != 2;
  const battle::Rewards& rewards = finished.rewards();
  impl.rows = {};
  impl.gain_list.clear();
  for (size_t i = 0; i < finished.party().size() && i < battle::kPcSlots; ++i) {
    const battle::PartyMember& member = finished.party()[i];
    const battle::Unit unit = finished.exit_unit(static_cast<int>(i));
    Impl::Row& row = impl.rows[i];
    row.present = unit.present;
    if (!row.present) continue;
    row.portrait = data::menu_form(member.char_id, member.kind >= 0 ? member.kind : member.char_id);
    row.name = import::utf8_to_u32(member.name);
    row.hp = unit.hp;
    row.max_hp = member.stat[0];
    row.stat = member.stat;
    row.gain = rewards.gains[i];
    row.cells = member.cells;
    row.gauge = unit.gauge;
    row.ko = !unit.conscious();
    row.unit_cells = unit.cells;
    for (size_t cell = 0; cell < row.open.size(); ++cell) row.open[cell] = unit.usable[cell] && !unit.used[cell];
    for (int gain : row.gain) row.has_gain = row.has_gain || gain != 0;
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
  // Debug: show the star-up page as after a boss fight (the rewards are not changed).
  if (std::getenv("OC_RESULTS_STAR")) impl.star_up = true;
  impl.star = finished.growth().star;
  impl.counter = finished.growth().counter;
  impl.drops.clear();
  std::vector<u16> words = rewards.drops;
  // Debug: replace the drops with these drop words (comma list; bit 15 = element).
  if (const char* list = std::getenv("OC_RESULTS_DROPS")) {
    words.clear();
    for (const char* cursor = list; *cursor;) {
      if (const u16 word = static_cast<u16>(std::strtol(cursor, nullptr, 0))) words.push_back(word);
      while (*cursor && *cursor != ',') ++cursor;
      if (*cursor == ',') ++cursor;
    }
  }
  for (u16 word : words) {
    Impl::Drop drop;
    drop.element = (word & 0x8000) != 0;
    if (drop.element) {
      drop.id = word & 0x7F;
      drop.name = import::utf8_to_u32(impl.tables->element_name(drop.id));
      if (static_cast<size_t>(drop.id) < impl.tables->elements.size()) drop.colour = data::colour_index_of_bit(impl.tables->elements[static_cast<size_t>(drop.id)].colour_bit());
    } else {
      drop.id = word;
      drop.name = import::utf8_to_u32(impl.tables->item_name(word));
      if (const data::ItemInfo* info = impl.tables->item(word)) drop.kind = info->kind();
    }
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
  const auto elements = std::count_if(impl.drops.begin(), impl.drops.end(), [](const Impl::Drop& drop) { return drop.element; });
  log::info("battle results: gold {} items {} elements {} gains for {} members{}", impl.gold, impl.drops.size() - static_cast<size_t>(elements), elements, impl.gain_list.size(),
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
  // RDE's canvas over the field and battle (render::overlay_fit), flatter than wide: the batch draws in a view whose y unit is sy / sx
  // pixels, so one zoom serves both axes, and the text snaps to that grid. The PS1 screen played from the discs keeps the widened
  // 4:3 frame.
  float flat = 1.0f;
  if (impl.disc) {
    const render::Letterbox lb = render::Letterbox::wide(vw, vh, impl.canvas_cols);
    impl.zoom = lb.scale / SX, impl.ox = lb.ox, impl.oy = lb.oy;
    impl.edge_x0 = lb.x0, impl.edge_w = lb.w;
  } else {
    const render::OverlayFit of = render::overlay_fit(vw, vh);
    flat = of.sy / of.sx;
    impl.zoom = of.sx, impl.ox = of.ox, impl.oy = of.oy / flat;
    const float cols = vw / (SX * of.sx);
    impl.edge_x0 = impl.canvas_cols > 0.0f ? -(cols - 320.0f) * 0.5f : 0.0f;
    impl.edge_w = impl.canvas_cols > 0.0f ? cols : 320.0f;
  }
  struct View {
    render::SpriteBatch* sprites;
    render::TextBatch* batch;
    float vw, vh;
    ~View() {
      sprites->set_view(vw, vh);
      batch->set_pixel_grid(0.0f, 0.0f);
    }
  } view{impl.sprites, &impl.text, vw, vh};
  if (impl.draws++ == 0 || impl.pt == 0)
    log::debug("battle results: phase {} at age {} (hd {}, disc {}, art: wall {} row {} panel {})", static_cast<int>(impl.phase), impl.age, impl.hd, impl.disc,
               impl.disc ? impl.disc_wall.ok() : impl.hd_wall.ok(), impl.disc ? impl.sheet_opaque.ok() : impl.hd_row.ok(), impl.hd_panel.ok());
  impl.sprites->begin(frame, vw, vh, resume);
  if (flat != 1.0f) {
    impl.sprites->set_view(vw, vh / flat);
    impl.text.set_pixel_grid(1.0f, flat);
  }
  impl.shift = 0;
  if (impl.disc) {
    impl.disc_wall_draw();
    impl.shift = -impl.layout_cols;
    impl.disc_faces();
    for (int slot = 0; slot < battle::kPcSlots; ++slot)
      if (impl.rows[static_cast<size_t>(slot)].present) impl.disc_row(slot);
    impl.shift = impl.layout_cols;
    impl.disc_panel();
  } else {
    impl.draw_wall();
    impl.shift = -impl.layout_cols;
    impl.draw_faces();
    for (int slot = 0; slot < battle::kPcSlots; ++slot)
      if (impl.rows[static_cast<size_t>(slot)].present) impl.draw_row(slot);
    impl.shift = impl.layout_cols;
    impl.draw_panel();
    impl.draw_headline(impl.phase == Phase::Spoils);
  }
  impl.shift = 0;
  // fades: from black over the first 0x3C frames, to black over the exit's 0x1E frames
  if (impl.age < kFadeIn) impl.rect(impl.edge_x0, 0, impl.edge_w, kScreenLines, rgba(0, 0, 0, 255 - 255 * impl.age / kFadeIn));
  if (impl.phase == Phase::Exit) impl.rect(impl.edge_x0, 0, impl.edge_w, kScreenLines, rgba(0, 0, 0, std::min(255, 255 * impl.pt / kExit)));
  impl.sprites->end();
}

void BattleResults::set_wide_columns(float canvas_columns, float layout_columns) {
  impl_->canvas_cols = std::max(0.0f, canvas_columns);
  impl_->layout_cols = std::clamp(layout_columns, 0.0f, impl_->canvas_cols);
}
render::TextBatch* BattleResults::text_batch() { return &impl_->text; }
void BattleResults::abort() { impl_->phase = Phase::Idle; }
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
