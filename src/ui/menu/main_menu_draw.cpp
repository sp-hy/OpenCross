#include "ui/menu/main_menu_impl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "core/element_icon.h"
#include "core/log.h"
#include "platform/keymap.h"
#include "platform/pad_remap.h"
#include "render/camera.h"
#include "render/element_icon_draw.h"
#include "ui/lowres.h"
#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"
#include "ui/ps1_font.h"

namespace oc::ui {

namespace {

// Row pitch the canvas-unit text helper centres a line in.
constexpr float kListRowPitch = 46.0f;
// Canvas units RDE draws the ink-placed text (HT) right of its ink coordinate.
constexpr float kInkDx = 4.0f;

const Str6 kTitles[7] = {
    {{"Member's Condition", "Statut du membre", "Condizioni dell'alleato", "Gefährtenstatus", "Estado del miembro", "Member's Condition"}},
    {{"Allocate/Use Elements", "Assigner/Utiliser éléments", "Assegna/Usa Elementi", "Elemente zuteilen/einsetzen", "Asignar/Usar elementos", "Power Elements"}},
    {{"Change Equipment", "Changer d'équipement", "Cambia equipaggiamento", "Ausrüstung ändern", "Cambiar equipo", "Change Equipments"}},
    {{"Key Items", "Objets clefs", "Oggetti importanti", "Wichtige Gegenstände", "Objetos clave", "Key Items"}},
    {{"Configuration", "Configuration", "Configurazione", "Konfiguration", "Configuración", "Customize"}},
    {{"Save/Load Progress", "Sauv./Charger la progression", "Salva o carica i progressi", "Fortschritt speichern/laden", "Guardar/Cargar una partida", "File Operation"}},
    {{"OpenCross", "OpenCross", "OpenCross", "OpenCross", "OpenCross", "OpenCross"}}};

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
  // The disc's menu font carries its own greys: the neutral text colours draw it unchanged.
  if (cmd.kind == Cmd::Text && ps1_text() && (cmd.color == kText || cmd.color == kName || cmd.color == kCap)) cmd.color = kWhite;
  if ((cmd.kind == Cmd::Rect || cmd.kind == Cmd::Img) && cmd.w > 0 && (cmd.x < sprite_clip_left || cmd.x + cmd.w > sprite_clip_right)) {
    const float left = std::max(cmd.x, sprite_clip_left), right = std::min(cmd.x + cmd.w, sprite_clip_right);
    if (right <= left) return;
    const float u_per_x = (cmd.u1 - cmd.u0) / cmd.w, u0 = cmd.u0;
    cmd.u0 = u0 + (left - cmd.x) * u_per_x;
    cmd.u1 = u0 + (right - cmd.x) * u_per_x;
    cmd.x = left, cmd.w = right - left;
  }
  cmd.x += gdx;
  const auto boost = cmd.kind == Cmd::Text && ps1_text() ? colour_boosts.find(cmd.color) : colour_boosts.end();
  cmds.push_back(std::move(cmd));
  if (boost != colour_boosts.end()) {
    Cmd extra = cmds.back();
    extra.color = boost->second;
    extra.add = true;
    cmds.push_back(std::move(extra));
  }
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
  cmd.add = img_add;
  emit(std::move(cmd));
}

// Face for a string: the bn label face; strings it has no glyphs for (JP names / kana) use kb_70, the default JP face. Same faces,
// positions and scales in both art modes; only the font files differ.
void MainMenu::Impl::pick_face(const std::u32string& str, float rel, render::FaceId& face, float& scale) const {
  face = face_gn;
  scale = ps1_text() ? ps1_scale() : base * rel;
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
// pieces at the shape's size, with the caption notch cut out of the top edge. Played from the discs (no HD art) the panel is the sheet
// frame itself, its lines with its caption words and icons, and its size reaches from the frame origin to the frame's far edges. A
// `height` below the shape's own cuts the panel shorter: its top and bottom 24 px as they are, the straight sides between them stretched
// (not played from the discs, where the sheet frame keeps its size).
void MainMenu::Impl::Frame(int frame, float x, float y, u32 colour, float* out_w, float* out_h, bool large, float height) {
  if (!texs.hd()) {
    // The JP disc's description frame is narrower than the USA disc's and stands 27 pixels further right.
    if (frame == 0x36 && jp()) x += 27;
    Tex* panel = texs.sheet("main", frame);
    if (out_w) *out_w = panel ? panel->ox + panel->w : 100;
    if (out_h) *out_h = panel ? panel->oy + panel->h : 50;
    if (panel) Img(panel, PX(x + panel->ox), PY(y + panel->oy), PX(panel->w), PY(panel->h), 0, 0, 1, 1, colour);
    return;
  }
  Tex* tex = texs.shape(frame, lang, large);
  const float div = (large || frame == 121) ? 1.0f : 4.0f;
  const float w = tex ? tex->w / div : 100, full_h = tex ? tex->h / div : 50;
  const bool cut = height > 48.0f && height < full_h;
  const float h = cut ? height : full_h;
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
  } else if (cut) {
    constexpr float kEnd = 24.0f;
    const float edge = kEnd / full_h;
    Img(tex, PX(x), PY(y), PX(w), PY(kEnd), 0, 0, 1, edge, colour);
    Img(tex, PX(x), PY(y + kEnd), PX(w), PY(h - 2 * kEnd), 0, edge, 1, 1 - edge, colour);
    Img(tex, PX(x), PY(y + h - kEnd), PX(w), PY(kEnd), 0, 1 - edge, 1, 1, colour);
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

// The console font's cells start at the point; the USA menu font's glyph frames hang from it by their own offsets.
void MainMenu::Impl::PT(float x, float y, const std::u32string& str, u32 colour) {
  if (str.empty()) return;
  render::FaceId face;
  float scale;
  pick_face(str, 1.0f, face, scale);
  text->prepare(face, str);
  Cmd cmd;
  cmd.kind = Cmd::Text;
  cmd.text = str;
  cmd.face = face;
  cmd.x = PX(x), cmd.y = PY(y), cmd.color = colour, cmd.scale = ps1_scale();
  emit(std::move(cmd));
}

// PS1 number print in `advance`-px cells: the whole string right-aligned in the field with kb_70 (0.34 / 0.29 / 0.275 of the slot scale
// for 16 / 12 / 8 px cells), in both art modes.
void MainMenu::Impl::Digits(float x, float y, const std::string& number, float advance, u32 colour) {
  const size_t first = number.find_first_not_of(' ');
  if (first == std::string::npos) return;
  // Played from the discs the digits stand on the row's text point.
  HT(face_kb != render::kNoFace ? face_kb : face_gn, x + advance * static_cast<float>(number.size()), y + (ps1_text() ? 0.0f : 1.7f),
     utf32(number.substr(first)), colour, advance >= 16 ? 0.34f : advance >= 12 ? 0.29f : 0.275f, true);
}

// A stock count of a list row in two 16-px cells from x; the USA menu font prints it left-aligned from the field's start.
void MainMenu::Impl::Count(float x, float y, int count, u32 colour) {
  const render::FaceId face = face_kb != render::kNoFace ? face_kb : face_gn;
  if (font_build(face, import::Ps1Font::Build::UsaMenu)) HT(face, x + 2.0f, y, utf32(std::to_string(count)), colour, 0.34f);
  else Digits(x, y, padded_number(count, 2), 16, colour);
}

// PS1 colour word 0xBBGGRR (0x80 = 1.0).
u32 MainMenu::Impl::ps1_colour(u32 bgr, int a) const {
  const int red = static_cast<int>(bgr & 0xFF) * 2, green = static_cast<int>(bgr >> 8 & 0xFF) * 2, blue = static_cast<int>(bgr >> 16 & 0xFF) * 2;
  const u32 colour = rgb(std::min(255, red), std::min(255, green), std::min(255, blue), a);
  if (red > 256 || green > 256 || blue > 256)
    colour_boosts[colour] = rgb(std::clamp(red - 256, 0, 255), std::clamp(green - 256, 0, 255), std::clamp(blue - 256, 0, 255), a);
  return colour;
}

void MainMenu::Impl::message(float x, float y) {
  if (msg.empty()) return;
  T(x, y, msg, kGold, 0.85f);
}

// Portrait strip of a body (model_number): hd/menu/playtim/<1999|2021>/pNN.png, or the PS1 strip (8bpp 128 x 224, the same two-half layout).
const Tex* MainMenu::Impl::portrait(int form) {
  if (form < 0 || form > 49) return nullptr;
  if (ps1_screen())
    if (Tex* disc = texs.ps1("ps1:portrait" + std::to_string(form), [&] { return ps1::portrait(*vfs, form); })) return disc;
  char path[64];
  std::snprintf(path, sizeof path, "hd/menu/playtim/%s/p%02d.png", face_set().c_str(), form);
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
  if (ps1_text()) text->set_pixel_aspect(PY(1.0f) / xscale);
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
    case MenuScreen::OpenCross: build_main(); break;
    case MenuScreen::OcPage: build_page(); break;
  }
  if (!texs.hd()) {
    // The disc's swirls, over the screen as on the PS1 (the portrait strips included): the compass meshes "deco" at (0x48, 0x187) and
    // "dec1" at (0xD9, -0x10), the latter at 0.8, added at a quarter (the PS1's B + F / 4).
    group(Centre);
    img_add = true;
    if (Tex* deco = texs.mesh("deco", ps1::kMeshAspect, 0x1000, 0x808080, true))
      Img(deco, PX(0x48 + deco->ox), PY(0x187 + deco->oy), PX(deco->w), PY(deco->h), 0, 0, 1, 1, shade(kWhite, 0.25f));
    if (Tex* dec1 = texs.mesh("dec1", ps1::kMeshAspect * 0xCCC >> 12, 0xCCC, 0x808080, true))
      Img(dec1, PX(0xD9 + dec1->ox), PY(-0x10 + dec1->oy), PX(dec1->w), PY(dec1->h), 0, 0, 1, 1, shade(kWhite, 0.25f));
    img_add = false;
  }
  cmds.insert(cmds.end(), std::make_move_iterator(key_caps.begin()), std::make_move_iterator(key_caps.end()));
  key_caps.clear();
  if (transition != Transition::None) {
    const int span = transition == Transition::Open ? kOpenFade : kBackFade;
    group(Centre);
    rect(-reach, 0, kCanvasW + 2.0f * reach, kCanvasH, rgb(0, 0, 0, 255 * std::min(transition_t, span) / span));
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
    ps1::parchment_grid([&](float x, float y, float w, float h, float u0, float u1) { Img(tile, x, y, w, h, u0, 0.0f, u1, 1.0f); }, kCanvasW, kCanvasH,
                        -reach, kCanvasW + reach);
  } else {
    rect(-reach, 0, kCanvasW + 2.0f * reach, kCanvasH, rgb(60, 38, 18));
  }
  model_layer = cmds.size();
  // The disc's swirls are drawn over the screen (build()).
  if (!texs.hd()) return;
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
  cmd.color = colour;
  if (ps1_text()) {
    const font::InkBox ink = font::ps1_ink(*text, face, str);
    // On the 640-wide save screen the console font's cells sit 4 pixels left of and 3 above these points.
    const bool wide_console = screen == MenuScreen::Save && font_build(face, import::Ps1Font::Build::Console);
    cmd.scale = ps1_scale();
    cmd.x = PX((right ? x - ink.right : x + font::kPs1InkShift - ink.left) - (wide_console ? 4.0f : 0.0f));
    cmd.y = PY(y - ink.top - (wide_console ? 3.0f : 0.0f));
  } else {
    cmd.scale = scale;
    cmd.x = right ? PX(x) - text->measure(face, str, scale) - 12.0f * scale + kInkDx : PX(x) - (kb ? 22.5f : 25.4f) * scale + kInkDx;
    cmd.y = PY(y) - (kb ? 32.3f : 25.6f) * scale;
  }
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

void MainMenu::Impl::symbol(int colour, float cx, float cy, float size, int light) {
  if (!element_symbols() || colour < 0 || colour >= kElementIconCount) return;
  if (cx - size * 0.5f < sprite_clip_left || cx + size * 0.5f > sprite_clip_right) return;
  Cmd cmd;
  cmd.kind = Cmd::Icon;
  cmd.x = cx, cmd.y = cy, cmd.w = size, cmd.icon = colour, cmd.light = light;
  emit(std::move(cmd));
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

// Leader line of main-list slot `entry`: from just left of the label's ink to the dial ring. The seventh slot (shown with the OpenCross
// row) meets the ring on its lower curve, further right.
void MainMenu::Impl::leader(int entry, float x_ink, float cap_y, u32 colour) {
  constexpr float kSeventhSlotEnd = 402.0f;
  const float bend_x = 383.0f - 12.4f * static_cast<float>(entry);
  const float end_x = entry == 0 ? 397.0f : entry == 6 ? kSeventhSlotEnd : 387.5f + 1.4f * static_cast<float>(entry - 1);
  leader_raw(x_ink - 2.8f, cap_y + 12.0f, bend_x, end_x, colour);
}

void MainMenu::Impl::Hand(float pivot_x, float pivot_y, int angle, float brightness) {
  const NeedleTexture::Quad needle = needle_tex.get(texs, *vfs, angle, 2.5f, 960.0f / 448.0f);
  // The disc's needle carries its own grey.
  const u32 grey_colour = texs.hd() ? rgb(175, 172, 172) : kWhite;
  if (needle.tex) Img(needle.tex, PX(pivot_x) - needle.half_w, PY(pivot_y) - needle.half_h, 2 * needle.half_w, 2 * needle.half_h, 0, 0, 1, 1, shade(grey_colour, brightness));
}

// Screen title `title_index` (right-aligned bn, with its underline) and the bottom bar: the star glyph with the star counter / star
// level (both capped 99), the Time label and value, the gold and its "G". Widescreen: the title goes with the right group, the bar with the left one.
void MainMenu::Impl::Sprite(int frame, float x, float y, u32 colour) {
  if (Tex* tex = texs.sheet("main", frame)) Img(tex, PX(x + tex->ox), PY(y + tex->oy), PX(tex->w), PY(tex->h), 0, 0, 1, 1, colour);
}

void MainMenu::Impl::SpriteNumber(int value, float x, float y, bool big, u32 colour) {
  value = std::max(value, 0);
  do {
    Sprite((big ? 9 : 21) + value % 10, x, y, colour);
    x -= big ? 12.0f : 8.0f;
    value /= 10;
  } while (value > 0);
}

// Played from the discs the title is sheet frame 41 + index at (490, 35) and the bar is sprites: the star (frame 6), the counter in big
// digits around the "/" (19), "Time" (36) and the play time in small digits with ":" (35), the gold with "." (32) between the thousands
// and "G" (33).
void MainMenu::Impl::bar_and_title(int title_index, bool bar) {
  if (!texs.hd()) {
    // The OpenCross title has no sheet frame: it is font text at the HD title's place.
    if (title_index < 6) {
      group(Right);
      Sprite(41 + std::max(title_index, 0), 490, 35);
      group(Left);
    } else {
      screen_title(utf32(S6(kTitles[6])));
    }
    if (!bar) return;
    SpriteNumber(std::min<int>(gs->slot.stars_current(), 99), 56, 392, true);
    Sprite(6, 30, 390);
    Sprite(19, 66, 392);
    SpriteNumber(std::min<int>(gs->slot.stars_total(), 99), 90, 392, true);
    Sprite(36, 116, 382);
    const u32 seconds = gs->play_seconds();
    const int fields[3] = {static_cast<int>(std::min<u32>(seconds / 3600, 999)), static_cast<int>(seconds / 60 % 60), static_cast<int>(seconds % 60)};
    float x = 148;
    for (int field = 0; field < 3; ++field) {
      const int digits = field == 0 ? 3 : 2;
      for (int k = digits - 1; k >= 0; --k, x += 8) Sprite(21 + fields[field] / (k == 2 ? 100 : k == 1 ? 10 : 1) % 10, x, 382);
      if (field < 2) Sprite(35, x, 382), x += 8;
    }
    const std::string gold = grouped_gold(gs->gold());
    float gold_x = 212;
    Sprite(33, gold_x, 400);
    for (auto it = gold.rbegin(); it != gold.rend() && *it != ' '; ++it) {
      gold_x -= 8;
      Sprite(*it == ',' ? 32 : 21 + (*it - '0'), gold_x, 400);
    }
    return;
  }
  screen_title(utf32(S6(kTitles[std::clamp(title_index, 0, 6)])));
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
  StarGlyph(30.0f, 0x186 + 2, 0.54f, 5.0f);
  HN(52.2f, 394.2f, stars_current, kText, 0.29f);
  HT(face_kb, 66.1f, 394.2f, utf32("/"), kText, 0.29f);
  HN(86.5f, 394.2f, stars_total, kText, 0.29f);
  HS(0x76, 116.3f, 383.9f, utf32(cm("PLAY")));
  HN(195.6f, 383.9f, playtime);
  std::string gold = grouped_gold(gs->gold());
  gold.erase(0, gold.find_first_not_of(' '));
  HN(195.6f, 401.9f, gold);
  HS(0x77, 204.1f + (jp() ? -2.0f : 0.0f), 402.6f, utf32("G"));
}

// The screen title, right-aligned at the top right with its underline (right group).
void MainMenu::Impl::screen_title(const std::u32string& title) {
  group(Right);
  HT(face_gn, 487.6f, 24.4f, title, kText, 0.26f, true);
  text->prepare(face_gn, title);
  const float title_w = text->measure(face_gn, title, 0.26f) / xscale;
  rect(PX(487.6f - title_w - 1.0f), PY(34.6f), PX(title_w + 2.6f), 2.2f, rgb(200, 190, 190));
  group(Left);
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
  // Played from the discs the caption and its icon are part of the frame.
  if (!texs.hd()) return;
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
  // HD art (and classic without Ps1ResUi): the triangle is replaced by the sprite menutest_cur.png (23 x 11 PS1 px) at (x, y),
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
// same UVs in classic + Ps1ResUi.
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

// Button glyph of function slot `slot_index`, read through the active Controller assignment (slots 0..3); the glyphs of glyph_button.
// The disc glyphs follow the Japanese build, where Circle confirms; played from the USA discs, where Cross confirms, the confirm and
// cancel glyphs trade places (the Customize run / walk row shows Circle there).
void MainMenu::Impl::btn(float x, float y, int slot_index, u32 colour) {
  slot_index = std::clamp(slot_index, 0, 7);
  if (slot_index < 4) slot_index = platform::map_source(platform::active_map(), slot_index) & 7;
  if (!texs.hd() && !jp() && slot_index < 2) slot_index ^= 1;
  glyph_button(x, y, slot_index, colour);
}

// Glyph of the physical button `button` (0..7 Circle .. R2, 8 Start, 9 Select): 24 x 24 PS1 px at (x, y). Gamepad: ui/gamepad.png cell
// i = pad_glyph_cell(button) (13 x 4 atlas: column i % 13, row (i / 13) * 2 + 1); keyboard mode swaps it for the key
// cap of the action's first key (platform/keymap.h; ui/keyboard_<e f i g s j>.png, 16 x 7 cells, moved up 1; the pad glyph at
// (0x10, 0x182 / 0x19A) is moved up 5). The buttons are 0 OK, 1 Cancel, 2 Square, 3 Triangle, 4 L1, 5 R1, 6 L2, 7 R2.
// Classic art + Ps1ResUi draws the disc's glyph, frame 0x7C + button of the menu sheet (the assignment page: 0x44..0x47 and 0x4A..0x4D; Start / Select 0x48 / 0x49). Glyphs go to `key_caps`, drawn last.
void MainMenu::Impl::glyph_button(float x, float y, int button, u32 colour, bool page_frames) {
  const size_t first = cmds.size();
  static const char* const atlas[6] = {"e", "f", "i", "g", "s", "j"};
  if (ps1_screen()) {
    const int frame = page_frames ? (button < 4 ? 0x44 + button : button < 8 ? 0x46 + button : 0x40 + button) : button < 8 ? 0x7C + button : 0x40 + button;
    if (Tex* tex = texs.ps1("ps1:sheet" + std::to_string(frame), [&] { return ps1::sheet_frame(*vfs, frame); }))
      Img(tex, PX(x - ps1::kSheetFrameOriginX), PY(y - ps1::kSheetFrameOriginY), PX(ps1::kSheetFrameW), PY(ps1::kSheetFrameH), 0, 0, 1, 1, colour);
  } else if (kb_mode) {
    if (Tex* tex = texs.load(std::string("ui/keyboard_") + atlas[std::clamp(lang_index(), 0, 5)] + ".png")) {
      const int cell = platform::keyboard_cell(button < 8 ? platform::action_vk(platform::button_action(button)) : button == 8 ? platform::kStartVk : platform::kSelectVk);
      Img(tex, PX(x), PY(y - 1.0f), PX(24), PY(24), static_cast<float>(cell & 15) / 16.0f, static_cast<float>(cell >> 4) / 7.0f,
          static_cast<float>((cell & 15) + 1) / 16.0f, static_cast<float>((cell >> 4) + 1) / 7.0f, colour);
    }
  } else if (Tex* tex = texs.load("ui/gamepad.png")) {
    const int i = platform::pad_glyph_cell(button), col = i % 13, row = (i / 13) * 2 + 1;
    const float dy = (x == 0x10 && (y == 0x182 || y == 0x19A)) ? -5.0f : 0.0f;
    Img(tex, PX(x), PY(y + dy), PX(24), PY(24), static_cast<float>(col) / 13.0f, static_cast<float>(row) / 4.0f,
        static_cast<float>(col + 1) / 13.0f, static_cast<float>(row + 1) / 4.0f, colour);
  }
  key_caps.insert(key_caps.end(), std::make_move_iterator(cmds.begin() + static_cast<std::ptrdiff_t>(first)), std::make_move_iterator(cmds.end()));
  cmds.resize(first);
}

void MainMenu::Impl::portrait_strip(int character, float x, u32 col, float dy) {
  if (const Tex* tex = portrait(model_number(character))) {
    Img(tex, PX(x), PY(dy), PX(64), PY(224), 0, 0, 0.5f, 1, col);
    Img(tex, PX(x), PY(224 + dy), PX(64), PY(224), 0.5f, 0, 1, 1, col);
  }
}

// Text wider than `maxw` PS1 px is shrunk to exactly that width.
float MainMenu::Impl::fit_rel(const std::u32string& str, float maxw, float rel) {
  if (ps1_text()) return rel;
  const float width = twp(str, rel);
  return width > maxw && width > 0.0f ? rel * maxw / width : rel;
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
    // Played from the discs the Customize screen keeps the 512-wide mode, as its layout does.
    const bool wide = screen == MenuScreen::Save || (screen == MenuScreen::Settings && texs.hd());
    Ps1Display display = wide ? kMenuWide : kMenutest;
    display.w = static_cast<u32>(std::lround(static_cast<float>(display.w) * (kCanvasW + 2.0f * canvas_s) / kCanvasW));
    // The window is cleared first: the target covers the picture only, and the field's own edge columns can lie just outside it.
    sprites->begin(frame, vw, vh, resume);
    sprites->draw(texs.white_set(), 0, 0, vw, vh, 0, 0, 1, 1, rgb(0, 0, 0, 255));
    sprites->end();
    resume = true;
    if (lowres.begin(frame, display, text, lf, kCanvasW + 2.0f * canvas_s, kCanvasH)) {
      // The target's window rectangle: the widened frame's, else the canvas rectangle (LowRes::end's own).
      PassWindow window{&frame, vw, vh, display, {}};
      if (canvas_s > 0.0f) {
        const render::Letterbox letterbox = render::Letterbox::wide(vw, vh, canvas_cols);
        window.rect = {letterbox.left(), letterbox.oy, letterbox.width(), render::kLayerH * letterbox.scale};
      } else {
        const render::CanvasFit fit = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
        window.rect = {fit.ox, fit.oy, kCanvasW * fit.s, kCanvasH * fit.s};
      }
      draw_view(lf, kCanvasW + 2.0f * canvas_s, kCanvasH, resume, true, ps1_menu_models() ? nullptr : &window);
      lowres.end(frame, vw, vh, &window.rect);
      return;
    }
  }
  draw_view(frame, vw, vh, resume, false);
}

// `in_pass`: vw x vh is the widened canvas itself (zoom 1; canvas x = 0 sits canvas_s in). Otherwise the window: RDE's menu canvas
// (render::menu_fit, drawn in a view whose y unit is sy / sx pixels so one zoom serves both axes), or with widescreen the widened
// frame's zoom (a quarter of the layer-column scale) and origin. The 4:3 fit cuts everything after the clear at the canvas's sides.
void MainMenu::Impl::draw_view(const render::Frame& pass_frame, float vw, float vh, bool resume, bool in_pass, const PassWindow* window) {
  render::Frame frame = pass_frame;
  const bool clipped = !in_pass && canvas_s <= 0.0f;
  float flat = 1.0f;
  if (in_pass) {
    zoom = 1.0f, ox = reach = canvas_s, oy = 0.0f;
  } else if (canvas_s > 0.0f) {
    const render::Letterbox letterbox = render::Letterbox::wide(vw, vh, canvas_cols);
    zoom = letterbox.scale * 0.25f, ox = letterbox.ox, oy = letterbox.oy;
    reach = -4.0f * letterbox.x0;
  } else {
    const render::OverlayFit fit = render::menu_fit(vw, vh);
    flat = fit.sy / fit.sx;
    zoom = fit.sx, ox = fit.ox, oy = fit.oy / flat;
    reach = 0.0f;
  }
  struct View {
    render::SpriteBatch* sprites;
    render::TextBatch* batch;
    float vw, vh;
    bool flattened;
    ~View() {
      if (!flattened) return;
      sprites->set_view(vw, vh);
      batch->set_pixel_grid(0.0f, 0.0f);
    }
  } view{sprites, text, vw, vh, flat != 1.0f};
  const auto flatten = [&] {
    if (!view.flattened) return;
    sprites->set_view(vw, vh / flat);
    text->set_pixel_grid(1.0f, flat);
  };
  const render::Rect clip = ps1::columns_clip(frame, vw, ox, kCanvasW * zoom);
  const float old_aspect = text->pixel_aspect();
  build();
  auto draw_cmds = [&](size_t first, size_t last) {
    bool adding = false;
    for (size_t i = first; i < last; ++i) {
      const Cmd& cmd = cmds[i];
      if (cmd.add != adding) sprites->set_blend(cmd.add ? render::BlendMode::Add : render::BlendMode::Alpha), adding = cmd.add;
      if (cmd.kind == Cmd::Text) {
        text->draw(cmd.face, cmd.text, ox + cmd.x * zoom, oy + cmd.y * zoom, cmd.color, cmd.scale * zoom);
      } else if (cmd.kind == Cmd::Icon) {
        render::draw_element_icon(*sprites, texs.white_set(), cmd.icon, ox + cmd.x * zoom, oy + cmd.y * zoom, cmd.w * zoom, cmd.w * zoom, cmd.light);
      } else {
        sprites->draw(cmd.set, ox + cmd.x * zoom, oy + cmd.y * zoom, cmd.w * zoom, cmd.h * zoom, cmd.u0, cmd.v0, cmd.u1, cmd.v1, cmd.color);
      }
    }
    if (adding) sprites->set_blend(render::BlendMode::Alpha);
  };
  const bool with_status = model_view && model_wanted && (screen == MenuScreen::Status || screen == MenuScreen::OcPage);
  const bool with_enemy = enemy_view && enemy_wanted && screen == MenuScreen::OcPage;
  const bool with_model = with_status || with_enemy;
  const size_t split = with_model ? std::min(model_layer, cmds.size()) : cmds.size();
  sprites->begin(frame, vw, vh, resume);
  sprites->draw(texs.white_set(), 0, 0, vw, vh, 0, 0, 1, 1, rgb(0, 0, 0, 255));
  if (clipped) {
    sprites->end();
    frame.set_scissor(clip);
    sprites->begin(frame, vw, vh, true);
  }
  flatten();
  draw_cmds(0, split);
  sprites->end();
  if (with_model) {
    const auto now = std::chrono::steady_clock::now();
    float dt = model_clock.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - model_clock).count();
    model_clock = now;
    dt = std::min(dt, 0.1f);
    float origin_x = ox, origin_y = oy * flat, unit_x = xscale * zoom, unit_y = PY(1) * zoom * flat;
    auto draw_model = [&](const render::Frame& out, float out_w, float out_h) {
      if (with_enemy) {
        EnemyModelSlot slot = enemy_slot;
        slot.origin_x = origin_x, slot.origin_y = origin_y, slot.unit_x = unit_x, slot.unit_y = unit_y;
        slot.hd = mode == ArtMode::Hd;
        slot.dt = dt;
        enemy_view->draw(out, out_w, out_h, slot);
        return;
      }
      if (screen == MenuScreen::OcPage) {
        MenuModelSlot slot = mv_slot;
        slot.origin_x = origin_x, slot.origin_y = origin_y, slot.unit_x = unit_x, slot.unit_y = unit_y;
        slot.hd = mode == ArtMode::Hd;
        slot.dt = dt;
        model_view->draw(out, out_w, out_h, slot);
        return;
      }
      const int character = cur_char();
      const save::SaveChar& ch = gs->slot.chars[static_cast<size_t>(character)];
      MenuModelSlot slot;
      slot.char_id = character;
      slot.model = model_number(character, kLastMenuModel);
      slot.weapon_model = model_number(character);
      slot.origin_x = origin_x, slot.origin_y = origin_y;
      slot.unit_x = unit_x, slot.unit_y = unit_y;
      slot.camera = model_cam;
      slot.camera_prev = model_cam_prev;
      slot.session = model_session;
      slot.hd = mode == ArtMode::Hd;
      slot.status = ch.status;
      slot.tick = static_cast<u32>(model_frames / 2);
      // The hurt pose replaces pose 0 below an eighth of the HP or with a status of mask 0x39.
      const bool hurt = ch.cur_hp < ch.max_hp / 8 || (ch.status & 0x39) != 0;
      slot.pose = hurt && model_pose == 0 ? 3 : model_pose;
      model_gear(character, slot);
      slot.dt = dt;
      model_view->draw(out, out_w, out_h, slot);
    };
    if (window) {
      // The 2D so far goes to the window; the model is drawn there over the target's rectangle, and the pass starts again for the rest.
      lowres.end(*window->frame, window->vw, window->vh, &window->rect);
      const float to_x = window->rect.w / vw, to_y = window->rect.h / vh;
      origin_x = window->rect.x + origin_x * to_x, origin_y = window->rect.y + origin_y * to_y;
      unit_x *= to_x, unit_y *= to_y;
      const render::Frame& out = *window->frame;
      out.set_scissor(ps1::columns_clip(out, window->vw, window->rect.x, window->rect.w));
      draw_model(out, window->vw, window->vh);
      out.set_scissor(out.full());
      if (!lowres.begin(out, window->display, text, frame, vw, vh)) {
        model_wanted = enemy_wanted = false;
        text->set_pixel_aspect(old_aspect);
        return;
      }
    } else {
      draw_model(frame, vw, vh);
    }
    if (clipped) frame.set_scissor(clip);
    sprites->begin(frame, vw, vh, true);
    flatten();
    draw_cmds(split, cmds.size());
    sprites->end();
  } else {
    model_clock = {};
  }
  if (clipped) frame.set_scissor(frame.full());
  if (text->audit_enabled()) {
    text->audit_view(vw, vh);
    for (const AuditBox& box : audit_boxes)
      text->audit_obstacle(ox + box.x * zoom, (oy + box.y * zoom) * flat, box.w * zoom, box.h * zoom * flat, box.label);
  }
  model_wanted = enemy_wanted = false;
  text->set_pixel_aspect(old_aspect);
}

}  // namespace oc::ui
