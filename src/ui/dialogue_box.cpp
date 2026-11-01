#include "ui/dialogue_box.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "core/log.h"
#include "platform/keymap.h"
#include "ui/ps1_font.h"
#include "ui/ps1_window.h"

namespace oc::ui {

using render::pack_rgba;

namespace {

// Text colour table: index -> RGB; 0 and 1 white, 5 gray.
u32 text_color(u8 idx, u32 alpha) {
  struct Rgb {
    u8 r, g, b;
  };
  static const Rgb table[7] = {{233, 233, 233}, {233, 233, 233}, {0, 255, 255}, {230, 102, 255},
                               {0, 255, 0},     {106, 106, 106}, {255, 166, 0}};
  const Rgb& entry = table[idx < 7 ? idx : 0];
  return pack_rgba(entry.r, entry.g, entry.b, static_cast<u8>(alpha));
}

// Per-skin tile UVs of the message frame (skin =texture index 0..15 = w01..w15, w17).
struct SkinUv {
  float b0v, bv, bdv, lu, ldu, ldv, r0u, r0du, r24v, ru, rv, rdu, rdv;
};
constexpr SkinUv kSkins[16] = {
    {.404f, .163f, .166f, .126f, .125f, .161f, .653f, .3125f, .404f, .128f, .17067f, .125f, .161f},
    {.406f, .165f, .166f, .126f, .125f, .161f, .634f, .3125f, .406f, .134f, .17067f, .124f, .161f},
    {.420f, .1645f, .166f, .126f, .125f, .161f, .63f, .314f, .420f, .13f, .17067f, .1275f, .161f},
    {.408f, .16799f, .1625f, .126f, .125f, .161f, .625f, .314f, .408f, .125f, .17067f, .1275f, .161f},
    {.408f, .16667f, .1605f, .12615f, .125f, .1645f, .65f, .3125f, .408f, .125f, .17f, .1275f, .155f},
    {.408f, .16666f, .1645f, .126f, .125f, .161f, .653f, .3125f, .408f, .143f, .17067f, .125f, .161f},
    {.408f, .1655f, .166f, .1265f, .125f, .161f, .65f, .3125f, .418f, .143f, .17067f, .125f, .161f},
    {.408f, .1654f, .166f, .12625f, .125f, .161f, .625f, .3125f, .408f, .126f, .17067f, .1275f, .161f},
    {.407f, .166f, .166f, .125f, .125f, .161f, .625f, .3125f, .407f, .1258f, .17067f, .125f, .161f},
    {.408f, .1685f, .162f, .1265f, .124f, .161f, .625f, .3125f, .408f, .1257f, .17067f, .125f, .161f},
    {.408f, .166f, .164f, .126f, .125f, .161f, .625f, .312f, .408f, .126f, .17067f, .124f, .161f},
    {.408f, .16667f, .16465f, .126f, .125f, .161f, .625f, .3125f, .408f, .126f, .169f, .12525f, .161f},
    {.4095f, .1662f, .16495f, .126f, .125f, .161f, .625f, .3125f, .40675f, .125f, .17067f, .125f, .161f},
    {.408f, .16667f, .164f, .126f, .125f, .161f, .625f, .3125f, .408f, .125f, .17067f, .1275f, .161f},
    {.408f, .1655f, .166f, .126f, .125f, .161f, .625f, .3125f, .408f, .125f, .17067f, .1275f, .161f},
    {.408f, .16666f, .166f, .126f, .125f, .161f, .625f, .3125f, .408f, .125f, .17067f, .1275f, .161f}};
constexpr float kTW = 61.75f, kTH = 66.5f, kCorner = 2.5f * kTH;

// Frameless (label) window: inset around the text (canvas units).
constexpr float kPadX = 64, kPadY = 44;
constexpr float kBottomMargin = 36;

// Script variable of the number entry ({input(n)}): read when the message opens, written while the entry takes input.
constexpr int kDigitEntryVar = 0x18;

// Stem of an RDE face name ("00_seljyu" -> "seljyu"): the PS1 face files cdrom/face/<stem>.tim carry the same names.
std::string face_stem(const char* name) {
  std::string stem(name);
  return stem.size() > 3 && stem[2] == '_' ? stem.substr(3) : stem;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- DialogueBox
bool DialogueBox::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites,
                       render::TextBatch& text, const char* skin) {
  ctx_ = &ctx;
  sprites_ = &sprites;
  vfs_ = &vfs;
  text_ = &text;
  if (!linear_.create(ctx, true) || !nearest_.create(ctx, false)) return false;
  const u8 white[4] = {255, 255, 255, 255};
  if (!white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, white)) return false;
  white_set_ = render::TexRef(white_, nearest_);
  (void)skin;
  if (!load_skin(3)) log::warn("dialogue: window skin not found (no frame drawn)");
  return static_cast<bool>(white_set_);
}

// Skin texture index 0..15 (= sheet N 1..15, 17). HD: hd/window/wNN.png (512 x 384, bilinear, 0x5B3500). Original art: the PS1 sheet
// cdrom/map/window/wN.tim (8 bpp 128 x 96, read from cdrom.dat: RDE's cdrom2 copies are blank), nearest. wNN.png is a 4x redraw of
// wN.tim (same layout: per texel alpha agreement 0.98-1.0 over w1 w4 w5 w8 w14 w17), so the frame model's normalised UVs address the
// same pieces in both.
bool DialogueBox::load_skin(int index) {
  if (!vfs_ || !ctx_ || !sprites_) return false;
  static constexpr int kNn[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};
  index = std::clamp(index, 0, 15);
  const bool hd = mode_ == ArtMode::Hd;
  u32 w = 0, h = 0;
  std::vector<u8> px;
  if (hd) {
    char name[32];
    std::snprintf(name, sizeof name, "hd/window/w%02d.png", kNn[index]);
    auto png = vfs_->read(name);
    render::DecodedImage img;
    if (!png || !render::decode_image(*png, img)) return false;
    w = img.width, h = img.height, px = std::move(img.rgba);
  } else {
    ps1::Rgba disc_skin = ps1win::skin(*vfs_, kNn[index]);
    if (!disc_skin) return false;
    w = disc_skin.w, h = disc_skin.h, px = std::move(disc_skin.px);
  }
  skin_set_ = {};
  skin_.destroy();
  if (!skin_.create(*ctx_, w, h, render::TextureFormat::Rgba8, px.data())) return false;
  skin_set_ = render::TexRef(skin_, hd ? linear_ : nearest_);
  skin_w_ = static_cast<float>(w);
  skin_h_ = static_cast<float>(h);
  skin_idx_ = index;
  skin_mode_ = mode_;
  return true;
}

void DialogueBox::set_mode(ArtMode mode) {
  mode_ = mode;
  if (!skin_set_ || skin_mode_ != mode) load_skin(skin_idx_);
  refresh_face();
}

void DialogueBox::set_skin_byte(int config_byte) {
  // Engine table 0x5B3E30: 0 -> 3, 1 -> 0, 2 -> 1, 3 -> 2, 4..14 -> same, else 15.
  const int idx = config_byte == 0 ? 3 : config_byte >= 1 && config_byte <= 3 ? config_byte - 1 : config_byte <= 14 ? config_byte : 15;
  if (idx == skin_idx_ && skin_set_ && skin_mode_ == mode_) return;
  load_skin(idx);
}

void DialogueBox::destroy() {
  white_set_ = {};
  skin_set_ = {};
  faces_.clear();
  face_now_ = nullptr;
  white_.destroy();
  skin_.destroy();
  linear_.destroy();
  nearest_.destroy();
  ctx_ = nullptr;
}

// Face art names by RDE face id (data\hd\face\{2021|1999}\<name>.png, sprite registry ids 0x300 + id).
constexpr const char* kFaceNames[92] = {
    "00_seljyu", "01_kid", "02_gill", "03_ishit", "04_srash", "05_jakotu", "06_ridel", "07_karsh", "08_zoa", "09_mal",
    "10_col", "11_luchi", "12_poshul", "13_raz", "14_zappa", "15_orcha", "16_radius", "17_cap", "18_oba", "19_glen",
    "20_rena", "21_miki", "22_harler", "23_janes", "24_ryu", "25_star", "26_spriggan", "27_lucky", "28_kabuo", "29_rablue",
    "30_jilbelt", "31_skarl", "32_kinoko", "33_ningyo", "34_mell", "35_rea", "36_banklif", "37_sunef", "38_miko", "39_doc",
    "40_gyadaran", "41_yusha", "42_olha", "43_C13base", "44_C13a", "45_C13b", "46_C13aa", "47_C13ab", "48_C13bb", "49_yama",
    "50_glen", "51_yama", "52_chi_kid", "53_renkin", "54_dowa2", "55_dario", "56_dark", "57_seljyu2", "58_green", "59_white",
    "60_black", "61_blue", "62_red", "63_yellow", "64_yama2", "65_chrono", "66_c_dario", "67_c_glen", "68_c_karsh", "69_waduki",
    "70_migel", "71_garai", "72_marji", "73_fio", "74_raz_ane", "75_l_bros", "76_l_friend", "77_maburle", "78_galdorb", "79_wife",
    "80_ban_rich", "81_ban_bin", "82_jigoku", "kao00", "kao00", "kao00", "kao00", "kao00", "kao00", "kao00",
    "l_zappa", "kid_2"};

void DialogueBox::refresh_face() {
  face_now_ = nullptr;
  if (portrait_ < 0 || portrait_ >= 92 || !vfs_ || !ctx_ || !sprites_) return;
  const bool hd = mode_ == ArtMode::Hd;
  const std::string key = vfs_->cache_key((hd ? portrait_set_ : std::string("ps1")) + "/" + kFaceNames[portrait_]);
  if (auto it = faces_.find(key); it != faces_.end()) {
    face_now_ = it->second.get();
    return;
  }
  u32 w = 0, h = 0;
  std::vector<u8> px;
  bool nearest = false;
  // The PS1 face (64 x 64).
  if (!hd) {
    ps1::Rgba disc_face = ps1win::disc_tim(*vfs_, "cdrom/face/" + face_stem(kFaceNames[portrait_]) + ".tim");
    if (disc_face) w = disc_face.w, h = disc_face.h, px = std::move(disc_face.px), nearest = true;
  }
  // HD set, or the original-graphics folder hd/face/1999.
  if (px.empty()) {
    const std::string name = (hd ? portrait_set_ : std::string("1999")) + "/" + kFaceNames[portrait_];
    auto png = vfs_->read("hd/face/" + name + ".png");
    render::DecodedImage img;
    if (!png || !render::decode_image(*png, img)) {
      log::warn("dialogue: portrait hd/face/{}.png unavailable", name);
      faces_[key] = nullptr;
      return;
    }
    w = img.width, h = img.height, px = std::move(img.rgba);
  }
  auto face = std::make_unique<Face>();
  if (!face->tex.create(*ctx_, w, h, render::TextureFormat::Rgba8, px.data())) return;
  face->set = render::TexRef(face->tex, nearest ? nearest_ : linear_);
  face_now_ = face.get();
  faces_[key] = std::move(face);
}

void DialogueBox::set_portrait(int face_id) {
  portrait_ = face_id;
  refresh_face();
}

void DialogueBox::set_portrait_set(std::string set) {
  portrait_set_ = set == "1999" ? "1999" : "2021";
  refresh_face();
}

void DialogueBox::set_choices(std::vector<std::u32string> choices) {
  choices_ = std::move(choices);
  choice_ = 0;
  chosen_ = -1;
  if (text_ && face_ != render::kNoFace)
    for (const auto& choice : choices_) text_->prepare(face_, choice);
}

void DialogueBox::set_line_choice(int first, int last) {
  line_first_ = std::max(0, first);
  line_count_ = std::max(0, last - first + 1);
  choice_ = 0;
  chosen_ = -1;
  // The message is laid out with the choice flag set, which widens the three-space
  // indent of the option lines so the hand fits in front of them. The message is already open here
  // (E0 runs first), so lay it out again; page, revealed count and waits are kept.
  if (!choice_mode_) {
    choice_mode_ = true;
    if (open_ && !tw_.closed() && text_ && face_ != render::kNoFace) tw_.replace_pages(layout(face_, italic_, scale_));
  }
}

void DialogueBox::move_choice(int delta) {
  const int count = line_count_ > 0 ? line_count_ : static_cast<int>(choices_.size());
  if (count <= 0) return;
  choice_ = ((choice_ + delta) % count + count) % count;
}

void DialogueBox::open(const import::Message& flat, render::FaceId face, float text_scale, bool wrap, float wrap_width,
                       render::FaceId italic) {
  flat_ = flat;
  wrap_ = wrap;
  wrap_width_ = wrap_width;
  choice_mode_ = false;
  std::vector<TextPage> pages = layout(face, italic, text_scale);
  int digit_count = 0;
  for (const TextPage& page : pages)
    for (const LayoutGlyph& glyph : page.glyphs) digit_count = std::max(digit_count, glyph.digit + 1);
  digits_.assign(static_cast<size_t>(digit_count), 0);
  digit_cursor_ = std::max(0, digit_count - 1);
  tw_.start(std::move(pages));
  open_ = true;
  has_win_ = false;
  opening_.start(true);
  blink_ = 0;
  chosen_ = -1;
  line_count_ = 0;
  choice_ = 0;
}

void DialogueBox::start_digits(int value) {
  const int count = static_cast<int>(digits_.size());
  int rest = std::abs(value);
  for (int i = count - 1; i >= 0; --i) {
    digits_[static_cast<size_t>(i)] = rest % 10;
    rest /= 10;
  }
  digit_cursor_ = std::max(0, count - 1);
}

bool DialogueBox::digit_entry() const {
  if (digits_.empty() || !open_ || tw_.closed()) return false;
  const TextPage* page = tw_.page();
  if (!page) return false;
  for (size_t i = 0; i < page->glyphs.size(); ++i)
    if (page->glyphs[i].digit >= 0) return tw_.visible() > i;
  return false;
}

void DialogueBox::move_digit(int step, int shift) {
  if (digits_.empty()) return;
  const int count = static_cast<int>(digits_.size());
  int& digit = digits_[static_cast<size_t>(digit_cursor_)];
  digit = ((digit + step) % 10 + 10) % 10;
  digit_cursor_ = ((digit_cursor_ + shift) % count + count) % count;
}

int DialogueBox::digit_value() const {
  int value = 0;
  for (int digit : digits_) value = value * 10 + digit;
  return value;
}

void DialogueBox::swap_faces(render::FaceId face, render::FaceId italic) {
  if (!open_ || tw_.closed()) {
    face_ = face;
    italic_ = italic;
    return;
  }
  // Same message, page, revealed count, waits and choice state.
  tw_.replace_pages(layout(face, italic, scale_));
}

std::vector<TextPage> DialogueBox::layout(render::FaceId face, render::FaceId italic, float text_scale) {
  const import::Message& flat = flat_;
  const bool wrap = wrap_;
  face_ = face;
  italic_ = italic;
  // Every language but JP word-wraps.
  jp_ = !wrap;
  scale_ = text_scale;
  render::TextBatch* batch = text_;
  LayoutMetrics metrics;
  metrics.advance = [batch, face, text_scale](char32_t ch) { return batch->advance(face, ch, text_scale); };
  metrics.line_height = batch->line_height(face, text_scale);
  metrics.max_width = wrap_width_ * (text_scale / 0.5f);
  metrics.wrap_advance = [batch, face, italic, text_scale](char32_t ch, bool slanted) {
    return wrap_glyph_width(batch->glyph_measure(slanted && italic != render::kNoFace ? italic : face, ch), slanted) * (text_scale / 0.5f);
  };
  metrics.wrap = wrap;
  // Choice indent: non-JP only.
  if (choice_mode_ && wrap) {
    metrics.choice_space_first = 2.0f * 70.0f * 0.45f * (text_scale / 0.5f);
    metrics.choice_space = 2.0f * 20.0f * 0.45f * (text_scale / 0.5f);
  }
  line_h_ = metrics.line_height;
  // Make every glyph of the message available before the first frame that draws it.
  std::u32string all;
  for (const auto& tok : flat.tokens)
    if (tok.kind == import::TokKind::Text) all += tok.text;
  all += U"○×△□0123456789";
  batch->prepare(face, all);
  if (italic != render::kNoFace) batch->prepare(italic, all);
  for (const auto& choice : choices_) batch->prepare(face, choice);

  auto pages = layout_pages(flat, metrics);
  text_w_ = 0;
  // The frame's rows come from the text (starting at 1), no minimum height.
  int lines = 1;
  for (const TextPage& page : pages) {
    text_w_ = std::max(text_w_, page.width);
    lines = std::max(lines, page.lines);
  }
  lines_ = lines;
  text_h_ = static_cast<float>(lines) * line_h_;
  return pages;
}

void DialogueBox::update(float dt) {
  if (!open_) return;
  blink_ += dt;
  const float text_dt = opening_.update(dt);
  if (text_dt > 0) tw_.update(text_dt);
}

void DialogueBox::confirm() {
  if (!open_ || opening_.growing()) return;
  // In-window choice: the result is the cursor's *line number* in the message (the cursor starts at the first
  // option line), not the option index.
  // A message ending in {wend(n)} is in Closing while the list is up: the pick still counts.
  if ((tw_.state() == Typewriter::State::WaitClose || tw_.state() == Typewriter::State::Closing) && line_count_ > 0)
    chosen_ = line_first_ + choice_;
  else if (tw_.state() == Typewriter::State::WaitClose && !choices_.empty())
    chosen_ = choice_;
  tw_.confirm();
  // The page cursor animation restarts on every page.
  blink_ = 0;
}

void DialogueBox::rect(float x, float y, float w, float h, u32 color) {
  sprites_->draw(white_set_, x, y, w, h, 0, 0, 1, 1, color);
}

// Frame geometry for a text origin (x, y), widest line `text_width`, `lines` text lines.
DialogueBox::Geo DialogueBox::frame_geo(float x, float y, float text_width, int lines, bool has_portrait, bool portrait_left) const {
  return frame_geo_for(skin_idx_, x, y, text_width, lines, has_portrait, portrait_left);
}

DialogueBox::Geo DialogueBox::frame_geo_for(int skin_idx, float x, float y, float text_width, int lines, bool has_portrait, bool portrait_left) {
  Geo geo;
  geo.skin = skin_idx;
  const float base = x + 10;
  geo.X0 = base - 40;
  geo.Y0 = y - 35;
  geo.rows = lines + 1;
  if (has_portrait && geo.rows < 5) geo.rows = 5;
  const float XRp = base + text_width + (portrait_left ? 0 : 30);
  const float portrait_extra = (has_portrait && !portrait_left) ? 4 * kTW + 10 : 0;
  geo.T0x = geo.X0 - 1.28f * kTW * (skin_idx == 0 ? 1.013f : 1.0f);
  geo.T0y = geo.Y0 - 1.35f * kTW - (skin_idx == 0 ? 1.0f : 0.0f);
  geo.last = geo.Y0 + static_cast<float>(geo.rows - 1) * kTW;
  // skin8_two_rows = skin 8 with two rows (a one-line message): the bottom corner sits lower
  const bool skin8_two_rows = skin_idx == 8 && geo.rows == 2;
  geo.B0y = (skin8_two_rows && !has_portrait) ? geo.last + 0.15f * kTH - 0.17f * kTW
                                              : geo.last + 0.5f * kTH - (skin8_two_rows ? 0.425f : 0.95f) * kTW;
  geo.R0x = XRp + 6 - 110 + 70 + portrait_extra;
  geo.cols = 0;
  while (geo.X0 + static_cast<float>(geo.cols) * kTW <= XRp + 70 && geo.cols < 25) ++geo.cols;
  geo.cols += has_portrait ? 6 : 1;
  return geo;
}

DialogueBox::FrameSize DialogueBox::frame_size(int skin_idx, float text_width, int lines) {
  const Geo geo = frame_geo_for(skin_idx, 0, 0, text_width, std::max(1, lines), false, false);
  FrameSize size;
  size.w = geo.R0x + kCorner - geo.T0x;
  size.h = geo.B0y + kCorner - geo.T0y;
  size.rows = geo.rows;
  size.cols = geo.cols;
  return size;
}

void DialogueBox::draw_tiles(const Geo& geo, float zoom, float alpha, bool has_portrait, bool portrait_left) {
  const SkinUv& uv = kSkins[std::clamp(geo.skin, 0, 15)];
  const bool light = geo.skin <= 5;
  const u32 colF = pack_rgba(255, 255, 255, static_cast<u8>(255.0f * alpha));
  const u32 colG = pack_rgba(255, 255, 255, static_cast<u8>(255.0f * alpha * (light ? 0.85f : 1.0f)));
  auto quad = [&](float x, float y, float w, float h, float u0, float v0, float du, float dv, u32 col, bool mirror = false) {
    if (w <= 0 || h <= 0) return;
    const float ua = mirror ? u0 + du : u0, ub = mirror ? u0 : u0 + du;
    sprites_->draw(skin_set_, x * zoom, y * zoom + yo_, w * zoom, h * zoom, ua, v0, ub, v0 + dv, col);
  };
  const float rightLimit = geo.R0x;
  // Fill grid.
  for (int row = 0; row < geo.rows; ++row)
    for (int col = 0; col < geo.cols; ++col) {
      const float x = geo.X0 + static_cast<float>(col) * kTW, y = geo.Y0 + static_cast<float>(row) * kTW;
      if (x > rightLimit + kTH) continue;
      float w = kTW, h = kTW, du = 0.1232f;
      if (x + kTW > rightLimit + kCorner * 0.5f - (geo.skin == 0 ? 10 : 0)) {
        const float frac = std::clamp((rightLimit + kCorner * 0.5f - (geo.skin == 0 ? 10 : 0) - x) / kTW, 0.0f, 1.0f);
        w = kTW * frac;
        du *= frac;
      }
      if (!has_portrait && geo.skin != 5 && row == geo.rows - 1) {
        const float fv = (kTW - (y + kTW - (geo.B0y + kCorner * 0.5f))) / kTW;
        h = kTW * std::clamp(fv, 0.0f, 1.0f);
      }
      quad(x, y, w, h, (col & 1) ? 0.125f : 0.0f, (row & 1) ? 0.49925f : 0.335f, du, 0.165f, colG);
    }
  // Face.
  if (has_portrait && face_now_ && face_now_->set) {
    const float fy = geo.rows < 6 ? geo.Y0 + 12.5f
                                  : geo.Y0 + static_cast<float>(geo.rows / 2 - 2) * kTW - kTW * ((geo.rows % 2 == 0) ? 0.28f : -0.1f);
    float fx;
    if (portrait_left) fx = geo.X0 + 2;
    else fx = geo.R0x + kCorner * (geo.skin == 0 ? 0.32f : 0.4f) - 280.0f * (geo.skin == 0 ? 0.9f : 1.0f);
    sprites_->draw(face_now_->set, fx * zoom, fy * zoom + yo_, 280 * zoom, 280 * zoom, portrait_left ? 1.0f : 0.0f, 0, portrait_left ? 0.0f : 1.0f, 1,
                   colF);
  }
  // Top / bottom edges.
  const float bk = geo.skin == 2 ? -0.09f : geo.skin == 5 ? -0.18f : geo.skin == 6 ? -0.175f : geo.skin == 9 ? -0.184f : geo.skin == 10 ? -0.167f : -0.187f;
  for (int j = 1; j <= 19; ++j) {
    const float x = static_cast<float>(j) * kTH + 1.5f * 0.98f * kTH + geo.T0x - kTW / 1.03f + kTW;
    if (x > geo.R0x) break;
    quad(x, geo.T0y + 1.475f * kTH - kTW * 1.05f, kTH, kTH, 0, 0, 0.123f, 0.16667f, colF);
    quad(x, geo.last + 0.5f * kTH - kTW * bk, kTH, kTH, 0, uv.bv, 0.123f, uv.bdv, colF);
  }
  // Left edge.
  const float lk = geo.skin == 0 ? 1.02f : geo.skin == 6 ? 0.99f : geo.skin == 8 ? 1.01f : 1.0f;
  for (int i = 0; i < geo.rows; ++i) {
    const float y = geo.T0y + 2.68f * kTW + static_cast<float>(i) * kTH * 0.999f;
    if (!(geo.B0y + 30 > y + 0.2f * kTH)) break;
    quad(geo.X0 - 0.735f * kTW * lk, y, kTH, kTH, uv.lu, 0, uv.ldu, uv.ldv, colF);
  }
  // Corners T0, B0.
  // Skin 8, one text line: the corner sheets' v0 shift.
  const bool f8 = geo.skin == 8 && geo.rows == 2;
  quad(geo.T0x, geo.T0y, kCorner, kCorner, 0.3125f, f8 ? 0.004f : 0.0f, 0.3125f, 0.416f, colF);
  quad(geo.T0x, geo.B0y, kCorner, kCorner, 0.3125f, f8 ? 0.43f : uv.b0v, 0.3125f, 0.416f, colF);
  // Right edge, R0, R24.
  quad(geo.R0x, geo.T0y, kCorner, kCorner, uv.r0u, f8 ? 0.004f : 0.0f, uv.r0du, 0.416f, colF);
  static const float kR[16] = {.32f, .4f, .4f, .4f, .32f, .368f, .407f, .4f, .404f, .403f, .4035f, .404f, .4f, .4f, .4f, .4f};
  for (int i = 1; i < geo.rows; ++i) {
    const float y = geo.T0y + 1.615f * kTW + static_cast<float>(i) * kTH - (geo.skin == 0 ? 1.0f : 0.0f);
    if (y > geo.B0y) break;
    quad(geo.R0x + kCorner * kR[std::clamp(geo.skin, 0, 15)], y, kTH, kTH, uv.ru, uv.rv, uv.rdu, uv.rdv, colF);
  }
  quad(geo.R0x, geo.B0y, kCorner, kCorner, uv.r0u, f8 ? 0.43f : uv.r24v, uv.r0du, 0.416f, colF);
}

int DialogueBox::first_choice_line(const TextPage& page, float line_h) {
  if (line_h <= 0) return -1;
  int best = -1;
  for (size_t i = 0; i + 2 < page.glyphs.size(); ++i) {
    const LayoutGlyph& glyph = page.glyphs[i];
    const int line = static_cast<int>(std::lround(glyph.y / line_h));
    if (best >= 0 && line >= best) break;
    const bool start = i == 0 || static_cast<int>(std::lround(page.glyphs[i - 1].y / line_h)) != line;
    auto sp = [](char32_t ch) { return ch == U' ' || ch == 0x3000; };
    if (start && sp(glyph.ch) && sp(page.glyphs[i + 1].ch) && sp(page.glyphs[i + 2].ch)) {
      best = line;
      break;
    }
  }
  return best;
}

std::vector<int> DialogueBox::guest_choice_lines(const import::Message& ps1) {
  std::vector<int> out(1, -1);
  int newlines = 0, n75 = 0;
  bool at_entry0 = true;
  for (const import::Token& token : ps1.tokens) {
    switch (token.kind) {
      case import::TokKind::Page:
        out.push_back(-1);
        newlines = n75 = 0;
        at_entry0 = true;
        break;
      case import::TokKind::Newline:
        if (!at_entry0) ++newlines;
        at_entry0 = false;
        break;
      case import::TokKind::Text:
        for (char32_t ch : token.text) {
          at_entry0 = false;
          if (ch == 0x3000 && ++n75 == 3) out.back() = newlines;
        }
        break;
      default: at_entry0 = false; break;
    }
  }
  return out;
}

int DialogueBox::map_choice_line(int translated_first, int guest_first, int row) {
  if (translated_first < 0 || guest_first < 0) return row;
  return std::max(0, translated_first - guest_first + row);
}

int DialogueBox::picking_line() const {
  const Typewriter::State st = tw_.state();
  return open_ && line_count_ > 0 && (st == Typewriter::State::WaitClose || st == Typewriter::State::Closing) ? choice_line() : -1;
}

int DialogueBox::choice_line() const {
  const int row = line_first_ + choice_;
  const TextPage* current_page = tw_.page();
  if (!current_page || guest_lines_.empty()) return row;
  // The translation may merge or split pages: the last translated page is the last guest page (the choice list ends the message).
  const size_t pg = tw_.page_index() + 1 >= tw_.page_count() ? guest_lines_.size() - 1 : std::min(tw_.page_index(), guest_lines_.size() - 1);
  return map_choice_line(first_choice_line(*current_page, line_h_), guest_lines_[pg], row);
}

DialogueBox::ChoiceCursor DialogueBox::choice_cursor(const TextPage& page, float line_h, int line, bool jp, int skin_idx) {
  ChoiceCursor cursor;
  if (line_h <= 0) return cursor;
  for (const LayoutGlyph& glyph : page.glyphs)
    if (static_cast<int>(std::lround(glyph.y / line_h)) == line) {
      const bool s13 = skin_idx >= 1 && skin_idx <= 3;
      const float off = jp ? (s13 ? -15.0f : -10.0f) : (s13 ? 0.0f : 5.0f);
      cursor.valid = true;
      cursor.x = glyph.x;
      cursor.y = glyph.y - off;
      break;
    }
  return cursor;
}

void DialogueBox::draw(float vw, float vh, const render::CanvasFit* fit) {
  if (!open_ || tw_.closed() || !sprites_) return;
  const float alpha = opening_.alpha();
  if (alpha * 255.0f < 1.0f) return;
  const render::CanvasFit cf = fit ? *fit : render::canvas_fit(vw, vh);
  const float zoom = cf.s;
  yo_ = cf.oy;
  // The PS1-resolution pass (explicit fit): the view is the target's pixels, so the text snaps to its pixel grid.
  struct Grid {
    render::TextBatch* batch;
    ~Grid() { if (batch) batch->set_pixel_grid(0.0f, 0.0f); }
  } grid{fit && text_ ? text_ : nullptr};
  if (grid.batch) grid.batch->set_pixel_grid(1.0f, 1.0f);
  // Both art modes draw the same frame model; only the skin texture (and the font face chosen by the caller) differs.
  const bool tiled = skin_set_ && skin_mode_ == mode_ && !label_;
  const float cw = vw / zoom;
  const bool has_portrait = portrait_ >= 0 && portrait_on_ && !label_;
  float bx, by, bw, bh, tx, ty;
  Geo geo;
  if (tiled) {
    const int lines = std::max(1, lines_);
    // Text origin: portrait_left (portrait on the left) moves the text right of the face.
    const float xoff = has_portrait && portrait_left_ ? 280.0f : 0.0f;
    Geo g0 = frame_geo(0, 0, text_w_ + xoff, lines, has_portrait, has_portrait && portrait_left_);
    const float minx = g0.T0x, maxx = g0.R0x + kCorner;
    float ox, oy;
    if (has_win_) {
      // Glyph 0 at ((2 x + (portrait left ? 124 : -4)) * 2, 2 y * 960 / 432); the frame is kept inside the
      // canvas (screen clamp).
      ox = (cw - 1280.0f) * 0.5f + 4.0f * static_cast<float>(win_x_) + (has_portrait && portrait_left_ ? 248.0f - xoff : -8.0f);
      oy = 2.0f * static_cast<float>(win_y_) * kCanvasHeight / 432.0f;
      const Geo g1 = frame_geo(ox, oy, text_w_ + xoff, lines, has_portrait, has_portrait && portrait_left_);
      const float frame_left = g1.T0x, frame_right = g1.R0x + kCorner, frame_bottom = g1.B0y + kCorner;
      if (frame_right - frame_left >= cw) ox += (cw - (frame_right + frame_left)) * 0.5f;
      else if (frame_right > cw) ox -= frame_right - cw;
      else if (frame_left < 0) ox -= frame_left;
      if (frame_bottom > kCanvasHeight) oy -= frame_bottom - kCanvasHeight;
    } else {
      ox = (cw - (maxx - minx)) * 0.5f - minx;
      oy = kCanvasHeight - 8.0f - (g0.B0y + kCorner);
    }
    geo = frame_geo(ox, oy, text_w_ + xoff, lines, has_portrait, has_portrait && portrait_left_);
    tx = ox + xoff;
    ty = oy;
    bx = geo.T0x;
    by = geo.T0y;
    bw = geo.R0x + kCorner - geo.T0x;
    bh = geo.B0y + kCorner - geo.T0y;
    draw_tiles(geo, zoom, alpha, has_portrait, has_portrait && portrait_left_);
  } else {
    // Frameless: the world-map label window, or no skin texture could be loaded (nothing is drawn in place of the frame).
    const float padx = kPadX, pady = kPadY;
    bw = std::max(text_w_ + 2 * padx, 520.0f);
    bh = text_h_ + 2 * pady;
    bx = has_win_ && !label_ ? 4.0f * static_cast<float>(win_x_) - padx : (cw - bw) * 0.5f;
    by = label_ ? 4.0f * 0x41 - pady : has_win_ ? 4.0f * static_cast<float>(win_y_) - pady : kCanvasHeight - bh - kBottomMargin;
    tx = label_ ? (cw - text_w_) * 0.5f : bx + padx;
    ty = by + pady;
    // Labels are placed like any window (world-map names sit above their marker).
    if (label_ && has_win_) {
      tx = (cw - 1280.0f) * 0.5f + 4.0f * static_cast<float>(win_x_) - 8.0f;
      ty = 2.0f * static_cast<float>(win_y_) * kCanvasHeight / 432.0f;
    }
  }
  // Choice highlight: the selected option line is drawn in colour 6 (orange) once the choice is up.
  const Typewriter::State st = tw_.state();
  // Closing covers a {wend(n)} list.
  const bool choosing = line_count_ > 0 && (st == Typewriter::State::WaitClose || st == Typewriter::State::Closing);
  const int hl_line = choice_line();

  const TextPage* page = tw_.page();
  if (page) {
    const u32 shown = std::min<u32>(tw_.visible(), static_cast<u32>(page->glyphs.size()));
    const u32 a = static_cast<u32>(255.0f * alpha);
    for (render::TextBatch::Pass pass : {render::TextBatch::Pass::Backdrop, render::TextBatch::Pass::Fill}) {
      for (u32 i = 0; i < shown; ++i) {
        const LayoutGlyph& glyph = page->glyphs[i];
        bool hl = choosing && line_h_ > 0 && static_cast<int>(std::lround(glyph.y / line_h_)) == hl_line;
        char32_t ch = glyph.ch;
        // A number-entry digit shows the entry's value; the cursor digit is orange.
        if (glyph.digit >= 0 && static_cast<size_t>(glyph.digit) < digits_.size()) {
          ch = static_cast<char32_t>(U'0' + digits_[static_cast<size_t>(glyph.digit)]);
          hl = hl || glyph.digit == digit_cursor_;
        }
        const render::FaceId glyph_face = glyph.italic && italic_ != render::kNoFace ? italic_ : face_;
        text_->draw_glyph(glyph_face, ch, (tx + glyph.x) * zoom, (ty + glyph.y) * zoom + yo_, text_color(hl ? 6 : glyph.color, a), scale_ * zoom,
                          pass);
      }
    }
  }

  // Wait cursor (both art modes): the leaf quad of the skin texture (70 x 70, uv (u0, 0.83333) size (0.124, 0.16667),
  // u0 = {.5, .625, .75, .875}[phase], phase 0..4 advancing every 5 frames and wrapping, phase 4 shows the last frame again), at the end of the revealed text
  // of the page, its top 35 above the last line's lower edge.
  if (tiled && tw_.waiting_for_input() && page && !page->glyphs.empty() && line_count_ == 0) {
    const u32 shown = std::min<u32>(tw_.visible(), static_cast<u32>(page->glyphs.size()));
    if (shown > 0) {
      const LayoutGlyph& glyph = page->glyphs[shown - 1];
      const float cx = tx + glyph.x + text_->advance(face_, glyph.ch, scale_) + 8.0f;
      const float cy = ty + glyph.y + line_h_ - 35.0f;
      // phase 0..4 cycles (advanced every period frames, wrapping); phase 4 repeats the last leaf frame.
      const int phase = std::min(3, static_cast<int>(blink_ / (5.0f / 30.0f)) % 5);
      const float u0 = 0.5f + 0.125f * static_cast<float>(phase);
      sprites_->draw(skin_set_, cx * zoom, cy * zoom + yo_, 70.0f * zoom, 70.0f * zoom, u0, 0.83333f, u0 + 0.124f, 1.0f,
                     pack_rgba(255, 255, 255, static_cast<u8>(255.0f * alpha)));
    }
  }

  // In-window choice (field MULTI_CHOICE): the skin's hand (u0 {0, .125, .25,
  // .375}, v0 0.83333, 70 x 70) at the first glyph of the selected line (x = glyph x), y = glyph y - off with
  // off = non-JP: skins 1-3 0, else 5; JP: skins 1-3 -15, else -10.
  // Also drawn in a frameless label window: the hand only needs the skin sheet.
  if (choosing && skin_set_ && skin_mode_ == mode_ && page) {
    const ChoiceCursor cc = choice_cursor(*page, line_h_, hl_line, jp_, skin_idx_);
    if (cc.valid) {
      const int phase = std::min(3, static_cast<int>(blink_ / (5.0f / 30.0f)) % 5);
      const float u0 = 0.125f * static_cast<float>(phase);
      sprites_->draw(skin_set_, (tx + cc.x) * zoom, (ty + cc.y) * zoom + yo_, cc.w * zoom, cc.h * zoom, u0, 0.83333f, u0 + 0.124f, 1.0f,
                     pack_rgba(255, 255, 255, static_cast<u8>(255.0f * alpha)));
    }
  }

  // Choice list stub: a panel above the box once the last page is complete.
  if (!choices_.empty() && st == Typewriter::State::WaitClose) {
    float cwid = 0;
    for (const auto& choice : choices_) cwid = std::max(cwid, text_->measure(face_, choice, scale_));
    float pw = cwid + 2 * 56.0f, ph = static_cast<float>(choices_.size()) * line_h_ + 2 * 28.0f;
    float px = bx + bw - pw, py = by - ph - 10;
    float lxo = 56.0f, lyo = 28.0f;
    if (tiled) {
      const int choice_count = static_cast<int>(choices_.size());
      const Geo g0 = frame_geo(0, 0, cwid + 70.0f, choice_count, false, false);
      const float right = bx + bw;
      const float ox = right - (g0.R0x + kCorner), oy = by + 60.0f - (g0.B0y + kCorner);
      draw_tiles(frame_geo(ox, oy, cwid + 70.0f, choice_count, false, false), zoom, 1.0f, false, false);
      // Pen origin = ox, cursor 40 left of it.
      px = ox - 40.0f;
      py = oy;
      lxo = 40.0f;
      lyo = 0;
    }
    for (render::TextBatch::Pass pass : {render::TextBatch::Pass::Backdrop, render::TextBatch::Pass::Fill}) {
      for (size_t i = 0; i < choices_.size(); ++i) {
        u8 col = static_cast<int>(i) == choice_ ? 6 : 0;
        float lx = px + lxo, ly = py + lyo + static_cast<float>(i) * line_h_;
        float pen = lx;
        for (char32_t ch : choices_[i]) {
          text_->draw_glyph(face_, ch, pen * zoom, ly * zoom + yo_, text_color(col, 255), scale_ * zoom, pass);
          pen += text_->advance(face_, ch, scale_);
        }
      }
    }
    // Cursor next to the selected line: the skin's hand (choice style), as the in-window choice.
    if (tiled) {
      const float u0 = 0.125f * static_cast<float>(std::min(3, static_cast<int>(blink_ / (5.0f / 30.0f)) % 5));
      sprites_->draw(skin_set_, (px + lxo - 40.0f) * zoom, (py + lyo + static_cast<float>(choice_) * line_h_ - 5.0f) * zoom + yo_, 70.0f * zoom,
                     70.0f * zoom, u0, 0.83333f, u0 + 0.124f, 1.0f, pack_rgba(255, 255, 255, 255));
    }
  }
}

// ---------------------------------------------------------------------------------------------- DialogueDemo
struct DialogueDemo::Impl {
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch text;
  DialogueBox box;
  int last_speaker = 0;
  // Message fonts: non-JP gn_70 / gn_italic_70, JP kt_70, drawn at scale 0.5;
  // HD = large_ files, original art = small_ files (ui/ps1_font.h).
  font::FontSet fonts;
  import::Lang lang = import::Lang::En;
  ArtMode mode = ArtMode::Hd;
  bool ready = false;
  std::string status;
  // The room's PS1 message block (the guest line numbering of the E1 choice rows, DialogueBox::guest_choice_lines).
  int guest_room = -1;
  Bytes guest_bytes;
  std::optional<import::DialogueBlock> guest_block;
  import::JpGlyphs guest_glyphs;
  // Plain text of the open message (Text tokens, newlines as spaces).
  std::string plain;
  bool status_on = true;
  bool classic() const { return mode == ArtMode::Original; }
  // Message faces (0x43A060 glyph loop): JP kt_70, else gn_70 with gn_italic_70 for italic glyphs.
  render::FaceId msg_face() {
    const bool jp = lang == import::Lang::Jp;
    render::FaceId face = fonts.face(jp ? font::kKtMsg : font::kGn, classic());
    if (face == render::kNoFace) face = fonts.face(font::kGn, classic());
    if (face == render::kNoFace) face = fonts.face(font::kGn, false);
    return face;
  }
  render::FaceId italic_face() { return lang == import::Lang::Jp ? render::kNoFace : fonts.face(font::kGnItalic, classic()); }
};

DialogueDemo::DialogueDemo() : impl_(std::make_unique<Impl>()) {}
DialogueDemo::~DialogueDemo() { shutdown(); }

bool DialogueDemo::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, import::Lang lang) {
  Impl& impl = *impl_;
  impl.ctx = &ctx;
  impl.vfs = &vfs;
  impl.sprites = &sprites;
  impl.lang = lang;
  tables_.init(&vfs);
  if (!impl.text.init(ctx, sprites)) return false;
  if (!impl.box.init(ctx, vfs, sprites, impl.text)) return false;

  impl.fonts.init(vfs, impl.text);
  impl.ready = impl.fonts.face(font::kGn, false) != render::kNoFace;
  if (!impl.ready) log::error("dialogue: no usable font (font/large_font_gn_70.spritefont)");
  return impl.ready;
}

void DialogueDemo::shutdown() {
  Impl& impl = *impl_;
  impl.box.destroy();
  impl.text.destroy();
  impl.ctx = nullptr;
}

void DialogueDemo::set_mode(ArtMode mode) {
  impl_->mode = mode;
  impl_->box.set_mode(mode);
  show(room_, msg_);
}

void DialogueDemo::set_art_mode(ArtMode mode) {
  Impl& impl = *impl_;
  impl.mode = mode;
  impl.fonts.set_classic(impl.classic());
  // Skin + portrait.
  impl.box.set_mode(mode);
  impl.box.swap_faces(impl.msg_face(), impl.italic_face());
}

void DialogueDemo::set_language(import::Lang lang) {
  impl_->lang = lang;
  show(room_, msg_);
}

void DialogueDemo::show(std::string_view room_name, int msg_index) {
  int id = import::room_id_from_name(room_name);
  if (id < 0) {
    log::warn("dialogue: unknown room '{}'", std::string(room_name));
    return;
  }
  show(id, msg_index);
}

void DialogueDemo::show(int room_id, int msg_index, int speaker, int line_speaker) {
  Impl& impl = *impl_;
  if (!impl.ready) return;
  room_ = room_id;
  msg_ = std::max(0, msg_index);
  show_speaker_ = speaker;
  voice_page_ = -1;
  const import::Lang lang = impl.lang;

  // Text = lang.dat in every language and art mode (RDE never draws the PS1 glyph stream; it only parses it for timing).
  const char* room_name = import::room_name_for_id(room_);
  const int text_speaker = line_speaker >= 0 ? line_speaker : speaker;
  import::Message msg = tables_.field_message(room_, msg_, lang, text_speaker);
  {
    import::LookupInfo li;
    if (!tables_.lookup_field(room_, msg_, lang, text_speaker, &li)) li.speaker = text_speaker;
    impl.last_speaker = li.speaker;
  }
  import::TextContext tctx = tables_.make_context(lang);
  if (party_name) {
    auto base = tctx.party_name;
    tctx.party_name = [this, base](int slot) {
      std::u32string name = party_name(slot);
      return name.empty() && base ? base(slot) : name;
    };
  }
  if (speaker_name) {
    auto base = tctx.speaker_name;
    tctx.speaker_name = [this, base](int sp) {
      std::u32string name = speaker_name(sp);
      return name.empty() && base ? base(sp) : name;
    };
  }
  if (script_var) {
    auto var = [this](int index) { return script_var(0x1A + 2 * index); };
    tctx.script_number = var;
    tctx.script_name = [this, var](int index) {
      const int id = var(index);
      return char_name ? char_name(id) : tables_.default_name(id, language());
    };
    tctx.script_item = [this, var](int index) {
      const int value = var(index);
      const int key = value > 0x400 ? (0x180000 | (value - 0x400)) : value > 0x200 ? (0x170000 | (value - 0x200)) : value;
      return tables_.item_name(key, language());
    };
  }
  import::Message flat = import::flatten(msg, tctx);
  impl.plain.clear();
  for (const import::Token& token : flat.tokens)
    if (token.kind == import::TokKind::Text) impl.plain += import::u32_to_utf8(token.text);
    else if (token.kind == import::TokKind::Newline || token.kind == import::TokKind::Page) impl.plain += ' ';

  // Face (0x43A060 glyph loop): JP kt_70, else gn_70 with gn_italic_70 for italic glyphs; scale 0.5, no font-table offset.
  const bool jp = lang == import::Lang::Jp;
  const bool classic = impl.classic();
  const render::FaceId face = impl.msg_face();
  const render::FaceId italic = impl.italic_face();

  impl.box.set_mode(impl.mode);
  if (script_var && script_set)
    impl.box.set_field_events([this] { return script_var(0x22); }, [this](int value) { script_set(0x22, value); });
  if (impl.guest_room != room_) {
    impl.guest_room = room_;
    impl.guest_block.reset();
    if (room_name)
      if (auto block_bytes = import::read_room_dialogue_block(*impl.vfs, room_name)) {
        impl.guest_bytes = std::move(*block_bytes);
        impl.guest_block = import::parse_dialogue_block(impl.guest_bytes);
      }
  }
  const bool has_guest = impl.guest_block && static_cast<size_t>(msg_) < impl.guest_block->records.size();
  const import::Message guest =
      has_guest ? import::parse_ps1(impl.guest_block->records[static_cast<size_t>(msg_)].bytes, impl.guest_glyphs) : import::Message{};
  impl.box.open(flat, face, 0.5f, !jp, wrap_limit(room_, msg_, lang, disc_line_chars(guest)), italic);
  if (script_var) impl.box.start_digits(script_var(kDigitEntryVar));
  if (has_guest) impl.box.set_guest_choice_lines(DialogueBox::guest_choice_lines(guest));

  impl.status = std::string(room_name ? room_name : "?") + " #" + std::to_string(msg_) + "  " + import::lang_code(lang) +
                (impl.mode == ArtMode::Hd ? "  HD" : "  ORIG") + "   [Enter] next  [PgUp/PgDn] msg  [F5] art  [F6] lang";
  impl.fonts.set_classic(classic);
  impl.fonts.prepare(font::kGn, import::utf8_to_u32(impl.status));
}

void DialogueDemo::update(float dt) {
  impl_->box.update(dt);
  if (script_set && impl_->box.digit_entry()) script_set(kDigitEntryVar, impl_->box.digit_value());
  voice_sync();
}

void DialogueDemo::voice_sync() {
  if (!on_page && !on_close) return;
  if (impl_->box.is_open()) {
    const int pg = impl_->box.page_index();
    if (pg != voice_page_) {
      voice_page_ = pg;
      if (on_page) on_page(room_, msg_, show_speaker_, impl_->last_speaker, pg);
    }
  } else if (voice_page_ >= 0) {
    voice_page_ = -1;
    if (on_close) on_close();
  }
}

void DialogueDemo::draw(float vw, float vh, const render::CanvasFit* fit) {
  Impl& impl = *impl_;
  if (!impl.ready) return;
  const render::CanvasFit cf = fit ? *fit : render::canvas_fit(vw, vh);
  const float zoom = cf.s;
  impl.box.draw(vw, vh, &cf);
  // Status line (top left), debug view only.
  if (impl.status_on)
    impl.fonts.draw(font::kGn, import::utf8_to_u32(impl.status), 16, 12 + cf.oy / zoom, font::message(1, 1, 1), 0.4f, font::kLeft, zoom);
}

}  // namespace oc::ui

namespace oc::ui {

bool DialogueDemo::is_open() const { return impl_->box.is_open(); }
void DialogueDemo::confirm() { impl_->box.confirm(); }
void DialogueDemo::move_choice(int delta) { impl_->box.move_choice(delta); }
void DialogueDemo::set_line_choice(int first, int last) { impl_->box.set_line_choice(first, last); }
int DialogueDemo::chosen() const { return impl_->box.chosen(); }
int DialogueDemo::picking_line() const { return impl_->box.picking_line(); }
int DialogueDemo::progress() const { return impl_->box.page_index() * 0x10000 + static_cast<int>(impl_->box.typewriter().visible()); }
bool DialogueDemo::digit_entry() const { return impl_->box.digit_entry(); }
void DialogueDemo::move_digit(int step, int shift) {
  impl_->box.move_digit(step, shift);
  if (script_set && impl_->box.digit_entry()) script_set(kDigitEntryVar, impl_->box.digit_value());
}
void DialogueDemo::set_portrait(int id) { impl_->box.set_portrait(id); }
void DialogueDemo::set_portrait_flags(bool on, bool left) { impl_->box.set_portrait_flags(on, left); }
void DialogueDemo::set_skin_byte(int config_byte) { impl_->box.set_skin_byte(config_byte); }
int DialogueDemo::speaker() const { return impl_->last_speaker; }
const std::string& DialogueDemo::text() const { return impl_->plain; }
void DialogueDemo::set_portrait_set(const std::string& set) { impl_->box.set_portrait_set(set); }
void DialogueDemo::close() {
  impl_->box.close();
  voice_sync();
}
void DialogueDemo::set_label(bool on) { impl_->box.set_label(on); }
void DialogueDemo::set_ps1_window(int x, int y) { impl_->box.set_ps1_window(x, y); }
void DialogueDemo::skip_grow() { impl_->box.skip_grow(); }
void DialogueDemo::show_status(bool on) { impl_->status_on = on; }
import::Lang DialogueDemo::language() const { return impl_->lang; }

}  // namespace oc::ui
