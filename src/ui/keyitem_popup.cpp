#include "ui/keyitem_popup.h"
#include "render/camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

#include "core/log.h"
#include "import/text.h"
#include "ui/lowres.h"
#include "ui/ps1_font.h"
#include "ui/ps1_window.h"

namespace oc::ui {

namespace {
constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;

// ---- key-item list
// Scroll runs 0..kMaxScroll row pairs. Input / hand start from the 15th active frame (kInputFrame); a scroll step lasts kScrollFrames.
constexpr int kSlots = 64, kPairs = kSlots / 2, kRows = 6, kMaxScroll = kPairs - kRows;
constexpr int kInputFrame = 15;
constexpr int kScrollFrames = 7;
// Panel constants (canvas units).
constexpr float kTW = 68.25f, kTH = 73.5f, kPX = 125.0f, kRest = 495.0f;

// Per-skin numbers, skin = texture index 0..15 (w01..w15, w17).
// kT / kB: top / bottom edge y factor; bdv, bv: B[i] dv / v0; b0v: B0 v0; r24v: R24 v0;
// lk, lu, ldu, ldv: left edge x factor and uv; cx, cu, cdu, cv, cdv: right edge middle pieces.
struct ListSkin {
  float kT, kB, bdv, bv, b0v, r24v;
  float lk, lu, ldu, ldv;
  float cx, cu, cdu, cv, cdv;
  float grid_alpha;
};
constexpr ListSkin kListSkins[16] = {
    {1.05f, .198f, .165f, .1665f, .408f, .408f, 1.02f, .126f, .125f, .161f, 1126.2f, .126f, .1225f, .17067f, .160f, .935f},
    {1.05f, .198f, .165f, .1665f, .4077f, .4077f, 1.0f, .126f, .125f, .161f, 1135.0f, .1275f, .125f, .16667f, .160f, .92f},
    {1.05f, .09f, .170f, .1580f, .4180f, .4210f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, .92f},
    {1.05f, .199f, .165f, .1665f, .408f, .408f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, .88f},
    {1.05f, .175f, .161f, .1665f, .408f, .408f, 1.0f, .12615f, .125f, .165f, 1128.5f, .126f, .1225f, .1699f, .1568f, .92f},
    {1.05f, .198f, .165f, .1665f, .4075f, .4075f, 1.0f, .126f, .125f, .161f, 1127.5f, .14f, .125f, .17067f, .160f, .92f},
    {1.05f, .185f, .165f, .1665f, .408f, .4195f, .99f, .1265f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, .92f},
    {1.05f, .198f, .165f, .1665f, .4077f, .4077f, 1.0f, .126f, .125f, .161f, 1133.5f, .126f, .1275f, .169f, .1608f, 1.0f},
    {1.04f, .203f, .166f, .1665f, .408f, .408f, 1.01f, .125f, .125f, .161f, 1134.5f, .1253f, .124f, .17067f, .160f, 1.0f},
    {1.04f, .202f, .166f, .1654f, .408f, .408f, 1.0f, .126f, .124f, .161f, 1135.0f, .126f, .125f, .17067f, .160f, 1.0f},
    {1.05f, .198f, .163f, .1665f, .408f, .408f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, 1.0f},
    {1.0345f, .21f, .163f, .1665f, .4077f, .4050f, 1.0f, .126f, .125f, .161f, 1134.0f, .125f, .124f, .17067f, .160f, 1.0f},
    {1.05f, .198f, .164f, .1665f, .408f, .408f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, 1.0f},
    {1.05f, .194f, .165f, .1665f, .408f, .408f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, .92f},
    {1.05f, .198f, .165f, .1665f, .408f, .408f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, .92f},
    {1.05f, .198f, .166f, .1665f, .408f, .408f, 1.0f, .126f, .125f, .161f, 1134.0f, .126f, .125f, .17067f, .160f, 1.0f}};

// Panel y of active frame `frame` (1 = first): the end-of-frame update moves it 19 px (frames 1..19) or 38 px (frame 20 on) and snaps
// it to 495 once it is below 523.5: 960, 941, ... 599, 561, 523, 495.
float slide_y(int frame) {
  float y = 960.0f;
  for (int i = 1; i < frame; ++i) {
    if (y < 523.5f) return kRest;
    y -= i >= 20 ? 38.0f : 19.0f;
  }
  return y;
}
}  // namespace

struct KeyItemPopup::Impl {
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch* text = nullptr;
  render::Sampler linear, nearest;
  render::Texture white;
  render::TexRef white_set = {};
  // Fonts of the current art mode: gn_70 for the list rows, kt_70 for Japanese rows.
  font::FontSet fonts;
  // Classic art + [Video] Ps1ResMenus.
  LowRes lowres;
  render::FaceId face() { return fonts.face(font::kGn); }
  render::FaceId row_face() {
    const render::FaceId jp_face = jp ? fonts.face(font::kKtMsg) : render::kNoFace;
    return jp_face != render::kNoFace ? jp_face : face();
  }
  struct Pic {
    render::Texture tex;
    render::TexRef set = {};
    float w = 0, h = 0;
  };
  bool open = false;
  // Ticks (60 Hz) since the list opened.
  float ticks = 0;

  bool hd = true, jp = false;
  import::Lang lang = import::Lang::En;
  import::LangTables lt;
  // Texture index (option byte 0 -> 3 = w04).
  int skin = 3;
  std::unique_ptr<Pic> skin_tex;
  // Texture index + 16 * (original art) of skin_tex, and the mod overlay generation (vfs::Vfs::generation) it was read through.
  int skin_loaded = -1;
  std::uint32_t skin_generation = 0;
  // Field display order: key item code (id - 1) per position.
  std::array<u8, kSlots> order{};
  bool order_from_disc = false;
  // id 0 = empty.
  std::array<ListEntry, kSlots> slots;
  // Hand column / row and the top visible row pair.
  int col = 0, row = 0, top = 0;
  // Scroll step in progress (1 = down: top++ at the end, 2 = up: top-- done at the start).
  int scroll_dir = 0, scroll_t = 0;
  // Row-text scroll offset (moves toward 0 by 10 per frame).
  float text_off = 0;
  // 1 = text moving down (top pair fades), 2 = moving up (bottom pair fades).
  int text_dir = 0;
  int used = 0;

  std::unique_ptr<Pic> load(const char* member) {
    std::unique_ptr<Pic> pic;
    render::DecodedImage img;
    if (auto raw = vfs->read(member); raw && render::decode_image(*raw, img)) {
      pic = std::make_unique<Pic>();
      if (pic->tex.create(*ctx, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data())) {
        pic->set = render::TexRef(pic->tex, linear);
        pic->w = static_cast<float>(img.width), pic->h = static_cast<float>(img.height);
      } else {
        pic.reset();
      }
    }
    return pic;
  }
  // The panel's skin sheet (as DialogueBox::load_skin): HD hd/window/wNN.png (bilinear); original art the PS1
  // sheet cdrom/map/window/wN.tim (from cdrom.dat, nearest) - the same layout at 1/4 size, so the panel's normalised UVs address the
  // same pieces.
  Pic* skin_pic() {
    const int key = std::clamp(skin, 0, 15) + (hd ? 0 : 16);
    if (skin_loaded == key && skin_generation == vfs->generation()) return skin_tex.get();
    static constexpr int kNn[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};
    const int sheet = kNn[std::clamp(skin, 0, 15)];
    if (hd) {
      char path[32];
      std::snprintf(path, sizeof path, "hd/window/w%02d.png", sheet);
      skin_tex = load(path);
    } else {
      skin_tex = from_rgba(ps1win::skin(*vfs, sheet));
      if (!skin_tex) log::warn("keyitem list: PS1 window skin w{}.tim unavailable", sheet);
    }
    skin_loaded = key;
    skin_generation = vfs->generation();
    return skin_tex.get();
  }
  std::unique_ptr<Pic> from_rgba(const ps1::Rgba& img) {
    std::unique_ptr<Pic> pic;
    if (!img) return pic;
    pic = std::make_unique<Pic>();
    if (!pic->tex.create(*ctx, img.w, img.h, render::TextureFormat::Rgba8, img.px.data())) return nullptr;
    pic->set = render::TexRef(pic->tex, nearest);
    pic->w = static_cast<float>(img.w), pic->h = static_cast<float>(img.h);
    return pic;
  }
  int cursor() const { return (top + row) * 2 + col; }
};

KeyItemPopup::KeyItemPopup() : impl_(std::make_unique<Impl>()) {}
KeyItemPopup::~KeyItemPopup() { shutdown(); }

bool KeyItemPopup::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text) {
  Impl& impl = *impl_;
  impl.ctx = &ctx, impl.vfs = &vfs, impl.sprites = &sprites, impl.text = &text;
  if (!impl.linear.create(ctx, true) || !impl.nearest.create(ctx, false)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!impl.white.create(ctx, 1, 1, render::TextureFormat::Rgba8, px)) return false;
  impl.white_set = render::TexRef(impl.white, impl.nearest);
  impl.fonts.init(vfs, text);
  impl.fonts.set_classic(!impl.hd);
  impl.lt.init(&vfs);
  // Display order of the field list: 64 key-item codes from a table in field.prg.
  for (int i = 0; i < kSlots; ++i) impl.order[static_cast<size_t>(i)] = static_cast<u8>(i);
  if (auto prg = vfs.read("cdrom/prog/field.prg"); prg && prg->size() >= 0x3E128 + kSlots) {
    std::array<bool, kSlots> seen{};
    bool ok = true;
    for (int i = 0; i < kSlots; ++i) {
      const u8 code = (*prg)[0x3E128 + static_cast<size_t>(i)];
      if (code >= kSlots || seen[code]) ok = false;
      else seen[code] = true;
    }
    if (ok) {
      for (int i = 0; i < kSlots; ++i) impl.order[static_cast<size_t>(i)] = (*prg)[0x3E128 + static_cast<size_t>(i)];
      impl.order_from_disc = true;
    }
  }
  if (!impl.order_from_disc) log::warn("keyitem list: field.prg order table unavailable; ascending order");
  return impl.fonts.face(font::kGn, false) != render::kNoFace;
}

void KeyItemPopup::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  impl.skin_tex.reset();
  impl.skin_loaded = -1;
  impl.lowres.shutdown();
  impl.white_set = {};
  impl.white.destroy();
  impl.linear.destroy();
  impl.nearest.destroy();
  impl.ctx = nullptr;
}

void KeyItemPopup::set_style(bool hd, int skin_byte, bool jp) {
  Impl& impl = *impl_;
  impl.hd = hd;
  impl.jp = jp;
  // As DialogueBox::set_skin_byte: 0 -> 3, 1 -> 0, 2 -> 1, 3 -> 2, 4..14 -> same, else 15.
  impl.skin = skin_byte == 0 ? 3 : skin_byte >= 1 && skin_byte <= 3 ? skin_byte - 1 : skin_byte <= 14 ? skin_byte : 15;
  impl.fonts.set_classic(!hd);
  // Load / swap the skin texture now, outside the frame's draw.
  if (impl.ctx) impl.skin_pic();
}

void KeyItemPopup::set_language(import::Lang lang) { impl_->lang = lang; }

void KeyItemPopup::open_list(std::vector<ListEntry> owned) {
  Impl& impl = *impl_;
  // Row names in the text language: sysmes 0x18 | id (the key-item name table; the caller's names are the English fallback).
  for (auto& entry : owned)
    if (const std::string* name = impl.lt.sysmes((0x18u << 16) | static_cast<u32>(entry.id), impl.lang); name && !name->empty())
      entry.name = import::utf8_to_u32(*name);
  impl.slots.fill(ListEntry{});
  int placed = 0;
  for (const u8 code : impl.order)
    for (auto& entry : owned)
      if (entry.id == code + 1 && placed < kSlots) impl.slots[static_cast<size_t>(placed++)] = std::move(entry);
  impl.col = impl.row = impl.top = 0;
  impl.scroll_dir = impl.scroll_t = 0;
  impl.text_off = 0;
  impl.text_dir = 0;
  impl.used = 0;
  // The list has no slide-out: closing removes it at once.
  impl.open = true;
  impl.ticks = 0;
}

// List input (one edge per frame). Left at column 0 / right at column 1 continue on the previous / next row; the bottom
// (top) row scrolls the list by one row pair while the scroll is not at its end; there is no wrap.
int KeyItemPopup::list_input(int dx, int dy, bool confirm, bool cancel) {
  Impl& impl = *impl_;
  if (!impl.open) return 0;
  if (impl.ticks < kInputFrame - 1 || impl.scroll_dir != 0) return 0;
  if (cancel) {
    impl.open = false;
    return 6;
  }
  if (confirm) {
    const ListEntry& entry = impl.slots[static_cast<size_t>(impl.cursor())];
    if (entry.id == 0 || !entry.enabled) return 0x18;
    impl.used = entry.id;
    impl.open = false;
    return 8;
  }
  auto down = [&]() -> int {
    if (impl.row == kRows - 1) {
      if (impl.top >= kMaxScroll) return 0;
      impl.scroll_dir = 1, impl.scroll_t = 0;
      return 1;
    }
    ++impl.row;
    return 1;
  };
  auto up = [&]() -> int {
    if (impl.row == 0) {
      if (impl.top == 0) return 0;
      --impl.top;
      impl.text_off = -60.0f, impl.text_dir = 1;
      impl.scroll_dir = 2, impl.scroll_t = 0;
      return 1;
    }
    --impl.row;
    return 1;
  };
  if (dx < 0) {
    if (impl.col != 0) {
      impl.col = 0;
      return 1;
    }
    if (impl.row == 0 && impl.top == 0) return 0;
    impl.col = 1;
    up();
    return 1;
  }
  if (dx > 0) {
    if (impl.col != 1) {
      impl.col = 1;
      return 1;
    }
    if (impl.row == kRows - 1 && impl.top >= kMaxScroll) return 0;
    impl.col = 0;
    down();
    return 1;
  }
  if (dy > 0) return down();
  if (dy < 0) return up();
  return 0;
}

int KeyItemPopup::take_used() {
  const int used = impl_->used;
  impl_->used = 0;
  return used;
}
bool KeyItemPopup::list_mode() const { return impl_->open; }
int KeyItemPopup::selected_id() const { return impl_->slots[static_cast<size_t>(impl_->cursor())].id; }
void KeyItemPopup::finish_intro() {
  Impl& impl = *impl_;
  if (impl.open) impl.ticks = std::max(impl.ticks, 30.0f);
}
bool KeyItemPopup::active() const { return impl_->open; }

void KeyItemPopup::update(float dt) {
  Impl& impl = *impl_;
  if (!impl.open) return;
  const float before = impl.ticks;
  impl.ticks += dt * 60.0f;
  {
    // Whole frames elapsed this call: the scroll step and the text scroll advance per frame.
    const int frames = static_cast<int>(std::floor(impl.ticks)) - static_cast<int>(std::floor(before));
    for (int step = 0; step < frames; ++step) {
      if (impl.scroll_dir != 0 && ++impl.scroll_t >= kScrollFrames) {
        if (impl.scroll_dir == 1) {
          ++impl.top;
          impl.text_off = 60.0f, impl.text_dir = 2;
        }
        impl.scroll_dir = 0;
      }
      if (impl.text_off > 0) impl.text_off = std::max(0.0f, impl.text_off - 10.0f);
      if (impl.text_off < 0) impl.text_off = std::min(0.0f, impl.text_off + 10.0f);
      if (impl.text_off == 0) impl.text_dir = 0;
    }
  }
}

void KeyItemPopup::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  if (!impl.open || !impl.ctx) return;
  // Classic art with [Video] Ps1ResMenus (any art mode): the list is the field's window 4, drawn into the field buffer
  // (320 x 224, 216 rows shown; field windows map as y * 960 / 216): a 320 x 216 target over the field picture.
  render::Frame lf;
  if (ps1_menus()) {
    if (!impl.lowres.ready()) impl.lowres.init(*impl.ctx, *impl.sprites);
    if (impl.lowres.begin(frame, kFieldWin, impl.text, lf)) {
      draw_view(lf, kCanvasW, kCanvasH, resume);
      impl.lowres.end(frame, vw, vh);
      return;
    }
  }
  draw_view(frame, vw, vh, resume);
}

void KeyItemPopup::draw_view(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  const render::CanvasFit cf = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
  const float zoom = cf.s, ox = cf.ox, oy = cf.oy;
  {
    // Active frame, 1 = first.
    const int active_frame = static_cast<int>(impl.ticks) + 1;
    const int first = impl.top * 2;
    // Row texts first (prepare() must run outside begin / end).
    std::array<std::u32string, 12> names;
    const render::FaceId face = impl.row_face();
    for (int i = 0; i < 12; ++i) {
      const ListEntry& entry = impl.slots[static_cast<size_t>(first + i)];
      if (entry.id == 0) continue;
      names[static_cast<size_t>(i)] = entry.name;
      impl.text->prepare(face, names[static_cast<size_t>(i)]);
    }
    // May wait for the GPU: before begin().
    Impl::Pic* sk = impl.skin_pic();
    impl.sprites->begin(frame, vw, vh, resume);
    {
      // ---- Panel: all pieces from the window skin texture. Both art modes draw it; original art samples the PS1 sheet
      // (skin_pic) and the small_ font.
      const int skin_index = std::clamp(impl.skin, 0, 15);
      const ListSkin& skin_numbers = kListSkins[skin_index];
      const float panel_y = slide_y(active_frame);
      const bool narrow = skin_index == 8 || skin_index == 9 || skin_index == 11;
      const float cdu = narrow ? .3120f : .3125f, cdv = narrow ? .4130f : .4140f;
      const float rdu = narrow ? .3103f : .3125f;
      const float bcdv = skin_index == 10 ? .4132f : cdv;
      const float r0x = skin_index == 0 ? 1067.5f : skin_index == 4 ? 1056.0f : 1060.0f;
      const float off = skin_index == 0 ? -1.0f : 0.0f;
      const float T0x = kPX - kTW * 1.28f * (skin_index == 0 ? 1.013f : 1.0f), T0y = panel_y - kTW * 1.35f + off;
      // y of grid cell (5, 0).
      const float G5 = panel_y + 5.0f * kTW;
      const float B0y = (kTH * 0.5f + G5) - kTW * 0.95f;
      const float R24y = B0y + (skin_index == 6 ? 6.0f : 0.0f);
      auto piece = [&](float x, float y, float w, float h, float u0, float v0, float du, float dv, float a = 1.0f) {
        if (sk && sk->set)
          impl.sprites->draw(sk->set, ox + x * zoom, oy + y * zoom, w * zoom, h * zoom, u0, v0, u0 + du, v0 + dv,
                             render::pack_rgba(255, 255, 255, static_cast<u8>(255.0f * std::clamp(a, 0.0f, 1.0f))));
      };
      // Colour / alpha are stored after each piece's draw (static init 0): nothing is visible on the first active frame.
      const float pa = active_frame <= 1 ? 0.0f : 1.0f;
      // Step 1: fill grid, 15 columns (x <= R0.x + TH) x 6 rows, column-major.
      for (int column = 0; column < 21; ++column) {
        const float x = kPX + kTW * static_cast<float>(column);
        if (x > r0x + kTH) continue;
        for (int grid_row = 0; grid_row < kRows; ++grid_row)
          piece(x, panel_y + kTW * static_cast<float>(grid_row), kTW, kTW, (column & 1) ? 0.125f : 0.0f, (grid_row & 1) ? 0.49925f : 0.335f, 0.1232f,
                0.165f, pa * skin_numbers.grid_alpha);
      }
      // Step 4: top / bottom edges i = 1.. while x <= R0.x.
      for (int i = 1; i < 20; ++i) {
        const float x = T0x + kTH * 1.5f * 0.98f - kTW / 1.03f + kTW + kTH * static_cast<float>(i);
        if (x > r0x) break;
        const bool cut = skin_index == 10 && i == 12;
        const float w = cut ? 35.0f : kTH, du = cut ? 0.0615f : 0.123f;
        piece(x, T0y + kTH * 1.475f - kTW * skin_numbers.kT, w, kTH, 0, 0, du, 0.16667f, pa);
        piece(x, (kTH / 2.05f + G5) + kTW * skin_numbers.kB, w, kTH, 0, skin_numbers.bv, du, skin_numbers.bdv, pa);
      }
      // Step 5: left edge (col before the draw, alpha after).
      for (int i = 0; i < 18; ++i) {
        const float y = T0y + kTW * 2.68f + kTH * 0.999f * static_cast<float>(i);
        if (kTH * 0.2f + y - 15.0f > B0y) break;
        const bool cut = skin_index == 14 && i == 3;
        piece(kPX - kTW * 0.735f * skin_numbers.lk, y, kTH, cut ? 7.0f : kTH, skin_numbers.lu, 0, skin_numbers.ldu, cut ? 0.01607f : skin_numbers.ldv, pa);
      }
      // Steps 6/7: left corners; step 8: right corners and the right edge.
      piece(T0x, T0y, kTH * 2.5f, kTH * 2.5f, .3125f, 0, cdu, cdv, pa);
      piece(T0x, B0y, kTH * 2.5f, kTH * 2.5f, .3125f, skin_numbers.b0v, cdu, bcdv, pa);
      const float r0u = skin_index == 0 || skin_index == 5 ? .65f : skin_index == 6 ? .64f : .625f,
                  r24u = skin_index == 0 || skin_index == 5 ? .65f : .625f;
      piece(r0x, T0y, kTH * 2.5f, kTH * 2.5f, r0u, 0, rdu, cdv, pa);
      for (int i = 1; i < 24; ++i) {
        const float y = T0y + kTH * static_cast<float>(i) + kTW * 1.615f + off;
        if (y > R24y) break;
        const bool cut = skin_index == 14 && i == 4;
        piece(skin_numbers.cx, y, kTH, cut ? 7.0f : kTH, skin_numbers.cu, skin_numbers.cv, skin_numbers.cdu, cut ? 0.016f : skin_numbers.cdv, pa);
      }
      piece(r0x, R24y, kTH * 2.5f, kTH * 2.5f, r24u, skin_numbers.r24v, rdu, bcdv, pa);
      // Step 9: the hand (the skin's hand / leaf quad), 70 x 70 at x 158 / 635, y 514 + 60 * row; u0 0 / .125 / .25 / .375 / .375
      // in 6-frame steps from the 15th active frame on (drawn at its final y while the panel still slides).
      if (active_frame >= kInputFrame) {
        const int phase = ((active_frame - kInputFrame) / 6) % 5;
        const float u0 = 0.125f * static_cast<float>(std::min(phase, 3));
        piece(impl.col ? 635.0f : 158.0f, 514.0f + 60.0f * static_cast<float>(impl.row), 70.0f, 70.0f, u0, 0.83333f, 0.124f, 0.16667f);
      }
      // Row text: gn_70 at 0.5, x 225 / 705 (JP 250 / 730), row top = base + 60 * (pair - top);
      // base follows the slide (Y + 22, JP Y + 7) and rests at 517 (502); the pair entering during a scroll is drawn at alpha 0.05.
      // Normal colour bright - 0.085 (0.915; refused rows 0.415), outline / shadow 0.13.
      const float dy = impl.jp ? 7.0f : 22.0f;
      const float base = (active_frame <= 22 ? panel_y : active_frame == 23 ? 523.0f : kRest) + dy;
      render::TextStyle st;
      st.backdrop_color = render::pack_rgba(33, 33, 33, 255);
      for (int i = 0; i < 12; ++i) {
        const ListEntry& entry = impl.slots[static_cast<size_t>(first + i)];
        if (entry.id == 0) continue;
        float a = 1.0f;
        if (impl.text_dir == 1 && i < 2) a = 0.05f;
        if (impl.text_dir == 2 && i >= 10) a = 0.05f;
        const u8 shade = static_cast<u8>(255.0f * (entry.enabled ? 0.915f : 0.415f));
        const float x = (i & 1) ? (impl.jp ? 730.0f : 705.0f) : (impl.jp ? 250.0f : 225.0f);
        const float y = base + 60.0f * static_cast<float>(i >> 1) + impl.text_off;
        impl.text->draw(face, names[static_cast<size_t>(i)], ox + x * zoom, oy + y * zoom,
                        render::pack_rgba(shade, shade, shade, static_cast<u8>(255.0f * a)), 0.5f * zoom, st);
      }
    }
    impl.sprites->end();
  }
}

}  // namespace oc::ui
