#include "ui/menu/main_menu_impl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "core/log.h"
#include "platform/keymap.h"
#include "render/camera.h"
#include "ui/lowres.h"
#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"
#include "ui/ps1_font.h"

namespace oc::ui {

namespace {

// Row pitch the canvas-unit text helper centres a line in.
constexpr float kListRowPitch = 46.0f;

const Str6 kTitles[6] = {
    {{"Member's Condition", "Statut du membre", "Condizioni dell'alleato", "Gefährtenstatus", "Estado del miembro", "Member's Condition"}},
    {{"Allocate/Use Elements", "Assigner/Utiliser éléments", "Assegna/Usa Elementi", "Elemente zuteilen/einsetzen", "Asignar/Usar elementos", "Power Elements"}},
    {{"Change Equipment", "Changer d'équipement", "Cambia equipaggiamento", "Ausrüstung ändern", "Cambiar equipo", "Change Equipments"}},
    {{"Key Items", "Objets clefs", "Oggetti importanti", "Wichtige Gegenstände", "Objetos clave", "Key Items"}},
    {{"Configuration", "Configuration", "Configurazione", "Konfiguration", "Configuración", "Customize"}},
    {{"Save/Load Progress", "Sauv./Charger la progression", "Salva o carica i progressi", "Fortschritt speichern/laden", "Guardar/Cargar una partida", "File Operation"}}};

}  // namespace

namespace mainmenu {

MtStyle mt_style(u32 flags, int lang_index) {
  lang_index = std::clamp(lang_index, 0, 5);
  const int effective_lang = (flags & 0x10000) ? 5 : (flags & 0x20000) ? 0 : lang_index;
  const int style = static_cast<int>(flags & 0xFF);
  const int global_slot = lang_index == 5 ? font::kIk : font::kBn;
  MtStyle out{effective_lang == 5 ? font::kKb : font::kBn, 0.7f, 0.0f, 0.0f};
  if (style == 2 || style == 3 || style == 7) out.slot = effective_lang == 5 ? font::kIk : font::kBn;
  static const float kMul64[6] = {0.6f, 0.6f, 0.565f, 0.56f, 0.56f, 0.55f};
  static const float kMul65[6] = {0.625f, 0.625f, 0.585f, 0.625f, 0.625f, 0.625f};
  static const float kMul66[6] = {0.55f, 0.55f, 0.55f, 0.55f, 0.43f, 0.625f};
  static const float kMul67[6] = {0.625f, 0.625f, 0.625f, 0.625f, 0.48f, 0.625f};
  static const float kDx76[6] = {0, -15, -15, 20, -23, -3};
  switch (style) {
    case 2: case 4: case 8: out.mul = 0.625f; break;
    case 5: case 9: out.mul = 0.55f; break;
    case 6: out.mul = 0.6f; break;
    case 7: out.mul = 0.7f; break;
    case 0x64: out.mul = kMul64[lang_index], out.slot = global_slot; break;
    case 0x65: out.mul = kMul65[lang_index], out.slot = global_slot; break;
    case 0x66: out.mul = kMul66[lang_index], out.slot = global_slot; break;
    case 0x67: out.mul = kMul67[lang_index], out.slot = global_slot; break;
    case 0x74: out.mul = lang_index == 3 ? 0.65f : 0.7f, out.slot = global_slot; break;
    case 0x76: out.mul = 0.62f, out.dx = kDx76[lang_index], out.dy = -2; break;
    case 0x77: out.mul = 0.62f, out.dy = lang_index != 5 ? 1.0f : -3.0f; break;
    case 0x78: out.mul = 0.54f, out.slot = font::kKb, out.dy = 2; break;
    case 0x84: out.mul = 0.52f, out.slot = font::kBn; break;
    case 0x90: case 0xAB: out.mul = 0.55f, out.slot = font::kKb; break;
    case 0x91: out.mul = 0.625f, out.slot = font::kKb; break;
    default: break;
  }
  return out;
}

}  // namespace mainmenu

void MainMenu::Impl::emit(Cmd&& cmd) {
  if (cmd.kind != Cmd::Text && cmd.w > 0 && (cmd.x < sprite_clip_left || cmd.x + cmd.w > sprite_clip_right)) {
    const float left = std::max(cmd.x, sprite_clip_left), right = std::min(cmd.x + cmd.w, sprite_clip_right);
    if (right <= left) return;
    const float u_per_x = (cmd.u1 - cmd.u0) / cmd.w, u0 = cmd.u0;
    cmd.u0 = u0 + (left - cmd.x) * u_per_x;
    cmd.u1 = u0 + (right - cmd.x) * u_per_x;
    cmd.x = left, cmd.w = right - left;
  }
  cmd.x += gdx;
  cmds.push_back(std::move(cmd));
}

void MainMenu::Impl::rect(float x, float y, float w, float h, u32 colour) {
  Cmd cmd;
  cmd.kind = Cmd::Rect;
  cmd.set = texs.white_set();
  cmd.x = x, cmd.y = y, cmd.w = w, cmd.h = h, cmd.color = colour;
  emit(std::move(cmd));
}

void MainMenu::Impl::box_outline(float x, float y, float w, float h, float thickness, u32 colour) {
  rect(x, y, w, thickness, colour);
  rect(x, y + h - thickness, w, thickness, colour);
  rect(x, y, thickness, h, colour);
  rect(x + w - thickness, y, thickness, h, colour);
}

void MainMenu::Impl::Img(const Tex* tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 colour) {
  if (!tex || !tex->set) return;
  Cmd cmd;
  cmd.kind = Cmd::Img;
  cmd.set = tex->set;
  cmd.x = x, cmd.y = y, cmd.w = w, cmd.h = h, cmd.u0 = u0, cmd.v0 = v0, cmd.u1 = u1, cmd.v1 = v1, cmd.color = colour;
  emit(std::move(cmd));
}

// Face for a string: the bn label face; strings it has no glyphs for (JP names / kana) use kb_70, the default JP face. Same faces,
// positions and scales in both art modes; only the font files differ.
void MainMenu::Impl::pick_face(const std::u32string& str, float rel, render::FaceId& face, float& scale) const {
  face = face_gn;
  scale = base * rel;
  if (face_kb != render::kNoFace && (jp() || !glyphs_ok(str))) face = face_kb;
}

float MainMenu::Impl::tw(const std::u32string& str, float rel) {
  render::FaceId face;
  float scale;
  pick_face(str, rel, face, scale);
  text->prepare(face, str);
  return text->measure(face, str, scale);
}

void MainMenu::Impl::T(float x, float y, const std::u32string& str, u32 colour, float rel) {
  if (str.empty()) return;
  render::FaceId face;
  float scale;
  pick_face(str, rel, face, scale);
  text->prepare(face, str);
  Cmd cmd;
  cmd.kind = Cmd::Text;
  cmd.text = str;
  cmd.face = face;
  cmd.x = x, cmd.y = y + (kListRowPitch - text->line_height(face, scale)) * 0.5f, cmd.color = colour, cmd.scale = scale;
  emit(std::move(cmd));
}

// Hand / arrow cursor left of a row; with the PS1 disc art the flat red triangle at the sprite's spot.
void MainMenu::Impl::cursor(float x, float y) {
  if (ps1_screen()) return tri(x / xscale, (y + 8.0f) / PY(1.0f), true);
  if (cursor_tex && cursor_tex->set) Img(cursor_tex, x, y + 8, 46, 22, 0, 0, 1, 1, kWhite);
  else {
    rect(x + 6, y + 14, 24, 10, kGold);
    rect(x + 30, y + 10, 8, 18, kGold);
  }
}

// PS1 colour byte (0x80 = 1.0) -> modulation colour.
u32 MainMenu::Impl::grey(int ps1_level) {
  const int level = std::min(255, ps1_level * 2);
  return rgb(level, level, level);
}

// Panel shape `frame` with its top-left corner at PS1 (x, y), transparent interior (the parchment shows); the shape images are 4x the
// PS1 pixel size except the "-large" variants and frame 121 (1:1). With the PS1 disc art the panel is built from the "wind" sheet
// pieces at the shape's size, with the caption notch cut out of the top edge.
void MainMenu::Impl::Frame(int frame, float x, float y, u32 colour, float* out_w, float* out_h, bool large) {
  Tex* tex = texs.shape(frame, lang, large);
  const float div = (large || frame == 121) ? 1.0f : 4.0f;
  const float w = tex ? tex->w / div : 100, h = tex ? tex->h / div : 50;
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  if (!tex) return;
  if (Tex* wind = ps1_screen() ? texs.ps1("ps1:wind", [&] { return ps1::menutim(*vfs, "wind"); }) : nullptr) {
    const float gap = ps1::caption_gap_end(frame);
    const ps1::EdgeGap foot = ps1::bottom_gap(frame);
    ps1::frame_pieces(
        [&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) {
          Img(wind, dx, dy, dw, dh, sx / wind->w, sy / wind->h, (sx + sw) / wind->w, (sy + sh) / wind->h, colour);
        },
        PX(x), PY(y), PX(w), PY(h), PX(1), PY(1), gap > 0 ? PX(8) : 0.0f, gap > 0 ? PX(gap - 8) : 0.0f, PX(foot.start),
        PX(foot.end - foot.start));
  } else {
    Img(tex, PX(x), PY(y), PX(w), PY(h), 0, 0, 1, 1, colour);
  }
  if (text->audit_enabled()) audit_boxes.push_back({PX(x) + gdx, PY(y), PX(w), PY(h), "frame " + std::to_string(frame)});
}

// Text with its top-left at PS1 (x, y), centred in a 16-px row.
void MainMenu::Impl::TP(float x, float y, const std::u32string& str, u32 colour, float rel) {
  if (str.empty()) return;
  render::FaceId face;
  float scale;
  pick_face(str, rel, face, scale);
  text->prepare(face, str);
  Cmd cmd;
  cmd.kind = Cmd::Text;
  cmd.text = str;
  cmd.face = face;
  cmd.x = PX(x), cmd.y = PY(y) + (PY(16) - text->line_height(face, scale)) * 0.5f, cmd.color = colour, cmd.scale = scale;
  emit(std::move(cmd));
}

// PS1 number print in `advance`-px cells: the whole string right-aligned in the field with kb_70 (0.34 / 0.29 / 0.275 of the slot scale
// for 16 / 12 / 8 px cells), in both art modes.
void MainMenu::Impl::Digits(float x, float y, const std::string& number, float advance, u32 colour) {
  const size_t first = number.find_first_not_of(' ');
  if (first == std::string::npos) return;
  HT(face_kb != render::kNoFace ? face_kb : face_gn, x + advance * static_cast<float>(number.size()), y + 1.7f, utf32(number.substr(first)),
     colour, advance >= 16 ? 0.34f : advance >= 12 ? 0.29f : 0.275f, true);
}

// PS1 colour word 0xBBGGRR (0x80 = 1.0).
u32 MainMenu::Impl::ps1_colour(u32 bgr, int a) const {
  return rgb(std::min(255, static_cast<int>(bgr & 0xFF) * 2), std::min(255, static_cast<int>(bgr >> 8 & 0xFF) * 2),
             std::min(255, static_cast<int>(bgr >> 16 & 0xFF) * 2), a);
}

void MainMenu::Impl::message(float x, float y) {
  if (msg.empty()) return;
  T(x, y, msg, kGold, 0.85f);
}

// Portrait strip of a character: hd/menu/playtim/<1999|2021>/pCC.png, or the PS1 strip (8bpp 128 x 224, the same two-half layout).
const Tex* MainMenu::Impl::portrait(int character) {
  if (character < 0 || character > 49) return nullptr;
  if (ps1_screen())
    if (Tex* disc = texs.ps1("ps1:portrait" + std::to_string(character), [&] { return ps1::portrait(*vfs, character); })) return disc;
  char path[64];
  std::snprintf(path, sizeof path, "hd/menu/playtim/%s/p%02d.png", face_set().c_str(), character);
  return texs.load(path);
}

// ===================================================== build: screens
void MainMenu::Impl::build() {
  cmds.clear();
  audit_boxes.clear();
  model_layer = 0;
  group(Centre);
  xscale = screen == MenuScreen::Save ? 2.0f : 2.5f;
  select_fonts();
  background();
  switch (screen) {
    case MenuScreen::Main: build_main(); break;
    case MenuScreen::ElementChoice: build_main(); break;
    case MenuScreen::Status: build_status(); break;
    case MenuScreen::ElementUse: build_use(); break;
    case MenuScreen::ElementGrid: build_grid(); break;
    case MenuScreen::Equip: build_equip(); break;
    case MenuScreen::KeyItems: build_keyitems(); break;
    case MenuScreen::Settings: build_settings(); break;
    case MenuScreen::Save: build_save(); break;
  }
  cmds.insert(cmds.end(), std::make_move_iterator(key_caps.begin()), std::make_move_iterator(key_caps.end()));
  key_caps.clear();
  if (xfade_state == 1 || xfade_state == 2) {
    const int alpha = xfade_state == 1 ? 255 * xfade / xfade_len : 255 * (xfade_len - xfade) / xfade_len;
    group(Centre);
    rect(-canvas_s, 0, kCanvasW + 2.0f * canvas_s, kCanvasH, rgb(0, 0, 0, std::clamp(alpha, 0, 255)));
  }
}

// Face of a font slot (kb / bn / ik); falls back to the label face.
render::FaceId MainMenu::Impl::slot_face(int slot) const {
  const render::FaceId face = slot == font::kKb ? face_kb : slot == font::kIk ? face_ik : face_gn;
  return face != render::kNoFace ? face : face_gn;
}

// Text in menutest style `flags`: ink at the English-calibrated PS1 (x, y), moved by the caller's per-language key offset (PS1 px,
// relative to English) plus the style's canvas nudges relative to English; scale = slot scale * the style's multiplier for the
// language; face = the style's slot (JP: kb / ik).
void MainMenu::Impl::HS(u32 flags, float x, float y, const std::u32string& str, u32 colour, float key_dx, float key_dy, bool right) {
  const MtStyle style = mt_style(flags, lang_index()), english = mt_style(flags, 0);
  HT(slot_face(style.slot), x + key_dx + (style.dx - english.dx) / xscale, y + key_dy + (style.dy - english.dy) * (448.0f / 960.0f), str, colour,
     font::slot(style.slot).scale * style.mul, right);
}

// cmnmes.txt: the shared UI strings keyed by their JP text.
std::string MainMenu::Impl::cm(std::string_view key) {
  if (const import::TextTable* rows = lt.table("cmnmes"))
    if (const std::string* row = rows->get(key, lang); row && !row->empty()) return *row;
  return std::string(key);
}

// ---- parchment backdrop: 8 x 7 tiles of hd/map/etc/tile.tim (the whole 256 x 256 image is one tile) or, with the PS1 disc art, the
// disc's own tile; widescreen continues the grid out to x = -canvas_s .. 1280 + canvas_s with the edge tiles clipped. Then the two faint
// compass swirls (menutim_02).
void MainMenu::Impl::background() {
  if (Tex* tile = texs.ps1_or_hd(ps1_screen(), "ps1:tile", [&] { return ps1::parchment(*vfs); }, "hd/map/etc/tile.tim")) {
    const float tile_w = kCanvasW / 8.0f, tile_h = kCanvasH / 7.0f;
    const int extra = static_cast<int>(std::ceil(canvas_s / tile_w));
    for (int ty = 0; ty < 7; ++ty)
      for (int tx = -extra; tx < 8 + extra; ++tx) {
        const float x0 = tile_w * static_cast<float>(tx), x1 = x0 + tile_w + 1.0f;
        const float clip_x0 = std::max(x0, -canvas_s), clip_x1 = std::min(x1, kCanvasW + canvas_s);
        if (clip_x1 <= clip_x0) continue;
        Img(tile, clip_x0, tile_h * static_cast<float>(ty), clip_x1 - clip_x0, tile_h + 1.0f, (clip_x0 - x0) / (x1 - x0), 0.0f, (clip_x1 - x0) / (x1 - x0), 1.0f);
      }
  } else {
    rect(-canvas_s, 0, kCanvasW + 2.0f * canvas_s, kCanvasH, rgb(60, 38, 18));
  }
  model_layer = cmds.size();
  if (Tex* deco = texs.load("hd/menu/menutim/menutim_02.png")) {
    Img(deco, -140, 537.8f, 640, 662, 0, 0, 1, 1, rgb(160, 160, 176));
    Img(deco, 287, -300, 512, 529, 0, 0, 1, 1, rgb(160, 160, 176));
  }
}

bool MainMenu::Impl::glyphs_ok(const std::u32string& str) const {
  if (!gn) return false;
  for (char32_t ch : str)
    if (ch != U' ' && ch != U'\n' && !gn->find(static_cast<u32>(ch))) return false;
  return true;
}

void MainMenu::Impl::HT(render::FaceId face, float x, float y, const std::u32string& str, u32 colour, float scale, bool right) {
  if (str.empty()) return;
  text->prepare(face, str);
  const bool kb = face == face_kb;
  Cmd cmd;
  cmd.kind = Cmd::Text;
  cmd.text = str;
  cmd.face = face;
  cmd.scale = scale;
  cmd.color = colour;
  cmd.x = right ? PX(x) - text->measure(face, str, scale) - 12.0f * scale : PX(x) - (kb ? 22.5f : 25.4f) * scale;
  cmd.y = PY(y) - (kb ? 32.3f : 25.6f) * scale;
  emit(std::move(cmd));
}

// bn label (kb for JP).
void MainMenu::Impl::HL(float x, float y, const std::u32string& str, u32 colour, float scale, bool right) {
  HT((glyphs_ok(str) && !jp()) || face_kb == render::kNoFace ? face_gn : face_kb, x, y, str, colour, scale, right);
}

// kb number, ink right edge at right_x.
void MainMenu::Impl::HN(float right_x, float y, const std::string& number, u32 colour, float scale) {
  HT(face_kb != render::kNoFace ? face_kb : face_gn, right_x, y, utf32(number), colour, scale, true);
}

// Filled circle, canvas px.
void MainMenu::Impl::Disc(float cx, float cy, float radius, u32 colour) {
  if (text->audit_enabled()) audit_boxes.push_back({cx - radius + gdx, cy - radius, 2 * radius, 2 * radius, "dot"});
  for (float dy = -radius; dy < radius; dy += 1.0f) {
    const float half_w = std::sqrt(std::max(0.0f, radius * radius - (dy + 0.5f) * (dy + 0.5f)));
    rect(cx - half_w, cy + dy, 2 * half_w, 1.2f, colour);
  }
}

// The star "★" as a kb_70 list entry at list (x, y): scale = the slot scale * mul, pen X = x * sx + offX * scale, Y = y * sy + offY *
// scale + dy.
void MainMenu::Impl::StarGlyph(float x, float y, float mul, float dy, u32 colour) {
  if (face_kb == render::kNoFace) return;
  const font::SlotInfo& kb_slot = font::slot(font::kKb);
  const float scale = kb_slot.scale * mul;
  const std::u32string star = U"★";
  text->prepare(face_kb, star);
  Cmd cmd;
  cmd.kind = Cmd::Text;
  cmd.text = star;
  cmd.face = face_kb;
  cmd.scale = scale;
  cmd.color = colour;
  cmd.x = PX(x) + kb_slot.off_x * scale;
  cmd.y = PY(y) + kb_slot.off_y * scale + dy;
  emit(std::move(cmd));
}

// Thin segment of squares, canvas px.
void MainMenu::Impl::Seg(float x0, float y0, float x1, float y1, float thickness, u32 colour) {
  const float length = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
  const int steps = std::max(1, static_cast<int>(length / (thickness * 0.5f)));
  for (int i = 0; i <= steps; ++i) {
    const float along = static_cast<float>(i) / static_cast<float>(steps);
    rect(x0 + (x1 - x0) * along - thickness * 0.5f, y0 + (y1 - y0) * along - thickness * 0.5f, thickness, thickness, colour);
  }
}

// Leader line of a list entry (PS1 px): horizontal from `start_x` to `bend_x`, 12 px diagonally down-right, horizontal on to `end_x`.
void MainMenu::Impl::leader_raw(float start_x, float y, float bend_x, float end_x, u32 colour) {
  const float thickness = 2.2f;
  rect(PX(start_x), PY(y) - thickness * 0.5f, PX(bend_x - start_x), thickness, colour);
  Seg(PX(bend_x), PY(y), PX(bend_x + 12.0f), PY(y + 12.0f), thickness, colour);
  rect(PX(bend_x + 12.0f), PY(y + 12.0f) - thickness * 0.5f, PX(end_x - bend_x - 12.0f), thickness, colour);
}

// Leader line of main-list entry `entry`: from just left of the label's ink to the dial ring.
void MainMenu::Impl::leader(int entry, float x_ink, float cap_y, u32 colour) {
  const float bend_x = 383.0f - 12.4f * static_cast<float>(entry);
  const float end_x = entry == 0 ? 397.0f : 387.5f + 1.4f * static_cast<float>(entry - 1);
  leader_raw(x_ink - 2.8f, cap_y + 12.0f, bend_x, end_x, colour);
}

void MainMenu::Impl::Hand(float pivot_x, float pivot_y, int angle) {
  const NeedleTexture::Quad needle = needle_tex.get(texs, *vfs, angle, 2.5f, 960.0f / 448.0f);
  if (needle.tex) Img(needle.tex, PX(pivot_x) - needle.half_w, PY(pivot_y) - needle.half_h, 2 * needle.half_w, 2 * needle.half_h, 0, 0, 1, 1, rgb(175, 172, 172));
}

// Screen title `title_index` (right-aligned bn, with its underline) and the bottom bar: the star glyph with the star counter / star
// level (both capped 99), the Time label and value, the gold and its "G". `dx` shifts the bar / title (the Status screen sits 1.7 PS1
// px left of the main list). Widescreen: the title goes with the right group, the bar with the left one.
void MainMenu::Impl::bar_and_title(int title_index, bool bar, float dx) {
  group(Right);
  const std::u32string title = utf32(S6(kTitles[std::clamp(title_index, 0, 5)]));
  HT(face_gn, 487.6f + dx, 24.4f, title, kText, 0.26f, true);
  text->prepare(face_gn, title);
  const float title_w = text->measure(face_gn, title, 0.26f) / xscale;
  rect(PX(487.6f + dx - title_w - 1.0f), PY(34.6f), PX(title_w + 2.6f), 2.2f, rgb(200, 190, 190));
  group(Left);
  if (!bar) return;
  const u32 seconds = gs->play_seconds();
  char playtime[24];
  std::snprintf(playtime, sizeof playtime, "%03u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
  const std::string stars_current = std::to_string(std::min<int>(gs->slot.stars_current(), 99));
  const std::string stars_total = std::to_string(std::min<int>(gs->slot.stars_total(), 99));
  if (const std::string shown = stars_current + "/" + stars_total; shown != stars_logged) {
    stars_logged = shown;
    log::info("menu: star counter {} (save 0x5A5 / 0x5A4; progression 0x40 = {})", shown, gs->slot.progression());
  }
  StarGlyph(30.0f + dx, 0x186 + 2, 0.54f, 2.0f);
  HN(52.2f + dx, 394.2f, stars_current, kText, 0.29f);
  HT(face_kb, 66.1f + dx, 394.2f, utf32("/"), kText, 0.29f);
  HN(86.5f + dx, 394.2f, stars_total, kText, 0.29f);
  HS(0x76, 116.3f + dx, 383.9f, utf32(cm("PLAY")));
  HN(195.6f + dx, 383.9f, playtime);
  std::string gold = grouped_gold(gs->gold());
  gold.erase(0, gold.find_first_not_of(' '));
  HN(195.6f + dx, 401.9f, gold);
  HS(0x77, 204.1f + dx + (jp() ? -2.0f : 0.0f), 402.6f, utf32("G"));
}

// Baked-in sheet captions: the PS1 frames carry "HP / STR. / ACC." etc. in the picture; the HD shapes are blank there and the
// cmnmes.txt text is drawn (keyed by the JP caption) at the same spot. `key` = JP key, `fallback` = English fallback.
std::u32string MainMenu::Impl::capt(std::string_view key, const char* fallback) {
  if (const import::TextTable* rows = lt.table("cmnmes"))
    if (const std::string* row = rows->get(key, lang); row && !trim(*row).empty()) return utf32(trim(*row));
  return utf32(fallback);
}

// Caption in the top-left notch of a frame at PS1 (frame_x, frame_y): kind 0 Weapons, 1 Armor, 2 Accessories, 3 Elements, 4 Key
// Items (the EquipIcon cell `kind`, 26 x 32 px, at (X + 9, Y - 6) and the bn label at ink (X + 34, Y + 0.5), scale 0.31), 5 Customize
// (no icon; the label shrinks to the notch). The cmnmes rows of these captions are Latin in every language, JP included; the JP
// column of "Customize" is empty, so the key itself shows there.
void MainMenu::Impl::notch_caption(float frame_x, float frame_y, int kind) {
  static const char* const kKey[7] = {"Weapon", "Armor", "Accessories", "Elements", "Key Items", "Customize", "Profile"};
  static const char* const kEnglish[7] = {"Weapons", "Armor", "Accessories", "Elements", "Key Items", "Customize", "Profile"};
  // Room for the text inside the frame's notch (caption_gap_end - 10).
  static const float kGapWidth[7] = {69, 56, 80, 70, 76, 51, 40};
  kind = std::clamp(kind, 0, 6);
  // Customize and Profile carry no icon.
  if (kind >= 5) {
    const std::u32string caption = capt(kKey[kind], kEnglish[kind]);
    float scale = 0.31f;
    text->prepare(face_gn, caption);
    const float width = text->measure(face_gn, caption, scale) / xscale;
    if (width > kGapWidth[kind]) scale *= kGapWidth[kind] / width;
    HT(face_gn, frame_x + 9.7f, frame_y + 0.5f, caption, kText, scale);
    return;
  }
  if (Tex* sheet = ps1_screen() ? texs.ps1("ps1:icon", [&] { return ps1::menutim(*vfs, "icon"); }) : nullptr) {
    const ps1::IconCell& cell = ps1::kEquipIcon[kind];
    Img(sheet, PX(frame_x + static_cast<float>(cell.dx)), PY(frame_y + static_cast<float>(cell.dy)), PX(static_cast<float>(cell.w)), PY(static_cast<float>(cell.h)),
        static_cast<float>(cell.u) / sheet->w, static_cast<float>(cell.v) / sheet->h, static_cast<float>(cell.u + cell.w) / sheet->w,
        static_cast<float>(cell.v + cell.h) / sheet->h);
  } else if (Tex* icons = texs.load("hd/menu/menutim/EquipIcon.png")) {
    Img(icons, PX(frame_x + 9.0f), PY(frame_y - 6.0f), PX(26), PY(32), 0.0075f + 0.2f * static_cast<float>(kind), 0.0075f,
        0.0075f + 0.2f * static_cast<float>(kind) + 0.185f, 0.985f);
  }
  HT(face_gn, frame_x + 34.0f, frame_y + 0.5f, capt(kKey[kind], kEnglish[kind]), kText, 0.31f);
}

// ---- shared PS1 drawing pieces
// Red triangle (x,y)-(x,y+12)-(x+23,y+6); `active` pulses (R = c, G = B = c - 0x80, c ping-pongs 0x90..0xF1).
void MainMenu::Impl::tri(float x, float y, bool active) {
  // HD art (and classic without Ps1ResMenus): the triangle is replaced by the sprite menutest_cur.png (23 x 11 PS1 px) at (x, y),
  // colour = the triangle colour >> 1 & 0x7F7F7F (flat (0x80,0,0) -> (0x40,0,0); pulse as below).
  if (!ps1_screen() && cursor_tex && cursor_tex->set) {
    u32 sc = rgb(127, 0, 0);
    if (active) {
      const int ph = frames % 96;
      const int pulse = 0x90 + (ph < 48 ? ph * 2 : (95 - ph) * 2);
      sc = rgb(std::min(255, (pulse >> 1) * 255 / 128), std::min(255, ((pulse - 0x80) >> 1) * 255 / 128),
               std::min(255, ((pulse - 0x80) >> 1) * 255 / 128));
    }
    Img(cursor_tex, PX(x), PY(y), PX(23), PY(11), 0, 0, 1, 1, sc);
    return;
  }
  u32 col = rgb(255, 0, 0);
  if (active) {
    const int ph = frames % 96;
    const int pulse = 0x90 + (ph < 48 ? ph * 2 : (95 - ph) * 2);
    const int g = std::min(255, (pulse - 0x80) * 2);
    col = rgb(255, g, g);
  }
  for (int i = 0; i < 12; ++i) {
    const float width_factor = 1.0f - std::fabs(static_cast<float>(i) + 0.5f - 6.0f) / 6.0f;
    rect(PX(x), PY(y + static_cast<float>(i)), PX(23.0f * width_factor), PY(1) + 0.6f, col);
  }
}

// Scroll arrow (menutim_31.png), 9x5 PS1 px. menutim_31.png = 4x menutim.prd "roll" (24 x 10 texels, the "roll" sheet frames 5..8):
// same UVs in classic + Ps1ResMenus.
void MainMenu::Impl::scroll_arrow(float x, float y, bool up) {
  Tex* p1 = ps1_screen() ? texs.ps1("ps1:roll", [&] { return ps1::menutim(*vfs, "roll"); }) : nullptr;
  if (Tex* tex = p1 ? p1 : texs.load("hd/menu/menutim/menutim_31.png"))
    Img(tex, PX(x), PY(y), PX(9), PY(5), 52.0f / 96, up ? 0.5f : 0.0f, 88.0f / 96, up ? 0.0f : 0.5f);
  else
    rect(PX(x), PY(y), PX(9), PY(5), kGold);
}

// Animated side pointer: frame 1 (quad (1,0,5,9)) / frame 3 ((6,0,-5,9): frame 1 mirrored) of the
// menutim "roll" texture (24 x 10; menutim_31.png = 4x), 5 x 9 PS1 px at (x, y). `acc += 0x3000` per frame, reset after passing 0x40000 (23-frame
// period); the caller subtracts / adds `acc >> 16`.
void MainMenu::Impl::pointer(float x, float y, bool left) {
  Tex* p1 = ps1_screen() ? texs.ps1("ps1:roll", [&] { return ps1::menutim(*vfs, "roll"); }) : nullptr;
  if (Tex* tex = p1 ? p1 : texs.load("hd/menu/menutim/menutim_31.png"))
    Img(tex, PX(x), PY(y), PX(5), PY(9), left ? 6.0f / 24 : 1.0f / 24, 0, left ? 1.0f / 24 : 6.0f / 24, 0.9f);
}

// Button glyph of function slot `slot_index`: 24 x 24 PS1 px at (x, y). Gamepad: ui/gamepad.png cell
// i = {8, 9, 0xB, 0xA, 0x17, 0x18, 0x15, 0x16}[slot_index] (13 x 4 atlas: column i % 13, row (i / 13) * 2 + 1); keyboard mode swaps it for the key
// cap of the action's first key (platform/keymap.h; ui/keyboard_<e f i g s j>.png, 16 x 7 cells, moved up 1; the pad glyph at
// (0x10, 0x182 / 0x19A) is moved up 5). The slots are the PS1 pad functions, identity-mapped: 0 OK, 1 Cancel, 2 Square, 3 Triangle, 4 L1, 5 R1.
// Classic art + Ps1ResMenus draws the disc's glyph, frame 0x7C + slot of the menu sheet. Glyphs go to `key_caps`, drawn last.
void MainMenu::Impl::btn(float x, float y, int slot_index, u32 colour) {
  const size_t first = cmds.size();
  static const int pad_i[8] = {8, 9, 0xB, 0xA, 0x17, 0x18, 0x15, 0x16};
  using platform::Action;
  static const Action kAction[6] = {Action::Ok, Action::Cancel, Action::Item, Action::Menu, Action::SelectLeft, Action::SelectRight};
  static const char* const atlas[6] = {"e", "f", "i", "g", "s", "j"};
  slot_index = std::clamp(slot_index, 0, 7);
  if (ps1_screen()) {
    const int frame = 0x7C + slot_index;
    if (Tex* tex = texs.ps1("ps1:sheet" + std::to_string(frame), [&] { return ps1::sheet_frame(*vfs, frame); }))
      Img(tex, PX(x - ps1::kSheetFrameOriginX), PY(y - ps1::kSheetFrameOriginY), PX(ps1::kSheetFrameW), PY(ps1::kSheetFrameH), 0, 0, 1, 1, colour);
  } else if (kb_mode && slot_index < 6) {
    if (Tex* tex = texs.load(std::string("ui/keyboard_") + atlas[std::clamp(lang_index(), 0, 5)] + ".png")) {
      const int cell = platform::keyboard_cell(platform::action_vk(kAction[slot_index]));
      Img(tex, PX(x), PY(y - 1.0f), PX(24), PY(24), static_cast<float>(cell & 15) / 16.0f, static_cast<float>(cell >> 4) / 7.0f,
          static_cast<float>((cell & 15) + 1) / 16.0f, static_cast<float>((cell >> 4) + 1) / 7.0f, colour);
    }
  } else if (Tex* tex = texs.load("ui/gamepad.png")) {
    const int i = pad_i[slot_index], col = i % 13, row = (i / 13) * 2 + 1;
    const float dy = (x == 0x10 && (y == 0x182 || y == 0x19A)) ? -5.0f : 0.0f;
    Img(tex, PX(x), PY(y + dy), PX(24), PY(24), static_cast<float>(col) / 13.0f, static_cast<float>(row) / 4.0f,
        static_cast<float>(col + 1) / 13.0f, static_cast<float>(row + 1) / 4.0f, colour);
  }
  key_caps.insert(key_caps.end(), std::make_move_iterator(cmds.begin() + static_cast<std::ptrdiff_t>(first)), std::make_move_iterator(cmds.end()));
  cmds.resize(first);
}

void MainMenu::Impl::portrait_strip(int character, float x, u32 col, float dy) {
  if (const Tex* tex = portrait(character)) {
    Img(tex, PX(x), PY(dy), PX(64), PY(224), 0, 0, 0.5f, 1, col);
    Img(tex, PX(x), PY(224 + dy), PX(64), PY(224), 0.5f, 0, 1, 1, col);
  }
}

// Shrinks `rel` until the text is at most `maxw` PS1 px wide.
float MainMenu::Impl::fit_rel(const std::u32string& str, float maxw, float rel) {
  while (rel > 0.4f && twp(str, rel) > maxw) rel -= 0.04f;
  return rel;
}

void MainMenu::Impl::TPfit(float x, float y, const std::u32string& str, float maxw, u32 colour, float rel) {
  TP(x, y, str, colour, fit_rel(str, maxw, rel));
}

void MainMenu::Impl::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  if (closed || !gs || !ready) return;
  // Widescreen: the canvas covers the window's widened frame; Wide Menus/HUD 4:3 keeps the layout on its centre, 16:9 / full spread it.
  wide_s = 4.0f * wide_cols;
  canvas_s = 4.0f * canvas_cols;
  render::Frame lf;
  if (lowres_on()) {
    Ps1Display display = screen == MenuScreen::Save || screen == MenuScreen::Settings ? kMenuWide : kMenutest;
    display.w = static_cast<u32>(std::lround(static_cast<float>(display.w) * (kCanvasW + 2.0f * canvas_s) / kCanvasW));
    if (lowres.begin(frame, display, text, lf, kCanvasW + 2.0f * canvas_s, kCanvasH)) {
      draw_view(lf, kCanvasW + 2.0f * canvas_s, kCanvasH, resume, true);
      if (canvas_s > 0.0f) {
        const render::Letterbox letterbox = render::Letterbox::wide(vw, vh, canvas_cols);
        const render::Letterbox::Rect target_rect{letterbox.left(), letterbox.oy, letterbox.width(), render::kLayerH * letterbox.scale};
        lowres.end(frame, vw, vh, &target_rect);
      } else {
        lowres.end(frame, vw, vh);
      }
      return;
    }
  }
  draw_view(frame, vw, vh, resume, false);
}

// `in_pass`: vw x vh is the widened canvas itself (zoom 1; canvas x = 0 sits canvas_s in). Otherwise the window: the 4:3 canvas fit, or
// with widescreen the widened frame's zoom (a quarter of the layer-column scale) and origin.
void MainMenu::Impl::draw_view(const render::Frame& frame, float vw, float vh, bool resume, bool in_pass) {
  if (in_pass) {
    zoom = 1.0f, ox = canvas_s, oy = 0.0f;
  } else if (canvas_s > 0.0f) {
    const render::Letterbox letterbox = render::Letterbox::wide(vw, vh, canvas_cols);
    zoom = letterbox.scale * 0.25f, ox = letterbox.ox, oy = letterbox.oy;
  } else {
    const render::CanvasFit cf = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
    zoom = cf.s, ox = cf.ox, oy = cf.oy;
  }
  build();
  auto draw_cmds = [&](size_t first, size_t last) {
    for (size_t i = first; i < last; ++i) {
      const Cmd& cmd = cmds[i];
      if (cmd.kind == Cmd::Text) {
        text->draw(cmd.face, cmd.text, ox + cmd.x * zoom, oy + cmd.y * zoom, cmd.color, cmd.scale * zoom);
      } else {
        sprites->draw(cmd.set, ox + cmd.x * zoom, oy + cmd.y * zoom, cmd.w * zoom, cmd.h * zoom, cmd.u0, cmd.v0, cmd.u1, cmd.v1, cmd.color);
      }
    }
  };
  const bool with_model = model_view && model_wanted && screen == MenuScreen::Status;
  const size_t split = with_model ? std::min(model_layer, cmds.size()) : cmds.size();
  sprites->begin(frame, vw, vh, resume);
  sprites->draw(texs.white_set(), 0, 0, vw, vh, 0, 0, 1, 1, rgb(0, 0, 0, 255));
  draw_cmds(0, split);
  sprites->end();
  if (with_model) {
    const auto now = std::chrono::steady_clock::now();
    float dt = model_clock.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - model_clock).count();
    model_clock = now;
    dt = std::min(dt, 0.1f);
    const int character = cur_char();
    const save::SaveChar& ch = gs->slot.chars[static_cast<size_t>(character)];
    MenuModelSlot slot;
    slot.char_id = character;
    slot.model = model_number(character, kLastMenuModel);
    slot.weapon_model = model_number(character, kLastWeaponModel);
    slot.origin_x = ox, slot.origin_y = oy;
    slot.unit_x = xscale * zoom, slot.unit_y = PY(1) * zoom;
    slot.camera = model_cam;
    slot.session = model_session;
    slot.hd = mode == ArtMode::Hd;
    slot.status = ch.status;
    slot.tick = static_cast<u32>(model_frames / 2);
    // The hurt pose replaces pose 0 below an eighth of the HP or with a status of mask 0x39.
    const bool hurt = ch.cur_hp < ch.max_hp / 8 || (ch.status & 0x39) != 0;
    slot.pose = hurt && model_pose == 0 ? 3 : model_pose;
    if (const int weapon = gs->equipped(character, 0); weapon > 0 && rules && rules->tables)
      if (const data::GearRec* gear = rules->tables->gear_rec(weapon)) slot.weapon_part = gear->tier() & 0xF;
    // Pierre's construct 3 (the Hero's Shield) shows only while he wears one.
    constexpr int kPierre = 41, kHerosShield = 0xC1, kShieldConstruct = 3;
    if (character == kPierre && gs->equipped(character, 2) != kHerosShield && gs->equipped(character, 3) != kHerosShield &&
        gs->equipped(character, 4) != kHerosShield)
      slot.hidden_meshes = 1u << kShieldConstruct;
    slot.dt = dt;
    model_view->draw(frame, vw, vh, slot);
    sprites->begin(frame, vw, vh, true);
    draw_cmds(split, cmds.size());
    sprites->end();
  } else {
    model_clock = {};
  }
  if (text->audit_enabled()) {
    text->audit_view(vw, vh);
    for (const AuditBox& box : audit_boxes) text->audit_obstacle(ox + box.x * zoom, oy + box.y * zoom, box.w * zoom, box.h * zoom, box.label);
  }
  model_wanted = false;
}

}  // namespace oc::ui
