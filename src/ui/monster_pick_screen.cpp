#include "ui/monster_pick_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "core/log.h"
#include "import/container.h"
#include "import/tim.h"
#include "render/camera.h"
#include "ui/ps1_font.h"
#include "ui/title/nav.h"

namespace oc::ui {

namespace {

// The backdrop sprite: 640 x 220 at (0, 0) through a 64 x 64 texture window.
constexpr float kBackdropW = 640, kBackdropH = 220, kTile = 64;
// Picks: names start at x 64 + 96 slot - 6 * (Japanese glyph count) + a per-name nudge, y 100; the cursor at x 14 + 96 slot, y 98.
constexpr float kPickNameX = 64, kPickY = 100, kPickCursorX = 14, kPickCursorY = 98, kColumnPitch = 96;
// List: the window (16, 128, 288, 78); names at x 28 + 96 col, y 128 + 13 row + 2; the cursor at x 16 + 96 col, y 128 + 13 row - 2.
constexpr float kListX = 16, kListY = 128, kListW = 288, kListH = 78, kListNameX = 28, kListTextDy = 2;
// Cursor: 8 x 8 px, drawn 4 px below its point; 5 frames of 8 menu frames (the counter wraps at 40).
constexpr float kCursorSize = 8, kCursorDy = 4;
constexpr int kCursorFrames = 5, kCursorPeriod = 8;
// Half the PS1 glyph pitch (12 px) and the glyph limit of the names: the pick names are placed by the Japanese name's width.
constexpr float kHalfGlyph = 6;
constexpr int kNameGlyphs = 8;
// Canvas units per PS1 pixel (the 1280 x 960 font canvas over the 320 x 240 screen).
constexpr float kCanvasPerPx = 4;
// Colours (0x00BBGGRR, 0x80 = 1.0).
constexpr u32 kWhite = 0x757575, kRed = 0x000075, kOrange = 0x285085;

// Per-language name styles: a few long names (by pool slot) are drawn smaller and lower. Styles 0x7D..0x8E replace the slot's
// scale and add dy (PS1 px); every other name keeps the slot's scale.
struct NameStyle {
  u8 id, style;
};
constexpr NameStyle kStylesEn[] = {{0x20, 0x7D}, {0x5E, 0x7E}};
constexpr NameStyle kStylesFr[] = {{0x04, 0x7F}, {0x0B, 0x80}, {0x1F, 0x82}, {0x20, 0x7D}, {0x5D, 0x81}, {0x5E, 0x7E}, {0xA5, 0x83}};
constexpr NameStyle kStylesIt[] = {{0x1A, 0x84}, {0x1F, 0x82}, {0x20, 0x7D}, {0x51, 0x87}, {0x5D, 0x81}, {0x5E, 0x7E}, {0x71, 0x85}, {0x98, 0x86}};
constexpr NameStyle kStylesDe[] = {{0x20, 0x7D}, {0x5E, 0x7E}, {0x98, 0x86}};
constexpr NameStyle kStylesEs[] = {{0x01, 0x88}, {0x03, 0x8B}, {0x1F, 0x82}, {0x20, 0x7D}, {0x28, 0x8C}, {0x29, 0x89}, {0x30, 0x8A},
                                   {0x3F, 0x8D}, {0x47, 0x8E}, {0x5D, 0x81}, {0x5E, 0x7E}, {0x71, 0x85}, {0x98, 0x86}};
constexpr u8 kFirstStyle = 0x7D;
// {scale, dy} of styles 0x7D..0x8E per language (en fr it de es jp).
struct StyleScale {
  float scale, dy;
};
constexpr StyleScale kStyleScale[18][6] = {
    {{.466f, 1}, {.43f, 1}, {.39f, 1}, {.44f, 1}, {.40f, 1}, {.5f, 0}},   {{.466f, 0}, {.385f, 1}, {.40f, 1}, {.42f, 1}, {.36f, 1}, {.5f, 0}},
    {{.5f, 0}, {.42f, 1}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}},        {{.5f, 0}, {.43f, 1}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}},
    {{.5f, 0}, {.41f, 1}, {.41f, 1}, {.5f, 0}, {.37f, 1}, {.5f, 0}},      {{.5f, 0}, {.47f, 1}, {.41f, 1}, {.5f, 0}, {.41f, 1}, {.5f, 0}},
    {{.5f, 0}, {.37f, 1}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}},        {{.5f, 0}, {.5f, 0}, {.43f, 1}, {.5f, 0}, {.5f, 0}, {.5f, 0}},
    {{.5f, 0}, {.5f, 0}, {.43f, 1}, {.5f, 0}, {.40f, 1}, {.5f, 0}},       {{.5f, 0}, {.5f, 0}, {.40f, 1}, {.44f, 1}, {.45f, 1}, {.5f, 0}},
    {{.5f, 0}, {.5f, 0}, {.47f, 1}, {.5f, 0}, {.5f, 0}, {.5f, 0}},        {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.45f, 1}, {.5f, 0}},
    {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.41f, 1}, {.5f, 0}},        {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.46f, 1}, {.5f, 0}},
    {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.45f, 1}, {.5f, 0}},        {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.39f, 2}, {.5f, 0}},
    {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.44f, 1}, {.5f, 0}},        {{.5f, 0}, {.5f, 0}, {.5f, 0}, {.5f, 0}, {.48f, 0}, {.5f, 0}},
};
// Non-Japanese pick names: x nudge (PS1 px) by pool slot.
constexpr u8 kNudgeMinus12[] = {0x02, 0x12, 0x18, 0x1A, 0x2F, 0x35, 0x36, 0x38, 0x41, 0x6C, 0x7C, 0x8E, 0xB2, 0xD7, 0xD8, 0xD9};
constexpr u8 kNudgePlus6[] = {0x03, 0x20, 0x27, 0x31, 0x4D, 0x56, 0x5C, 0x5E, 0x70, 0x8F};
constexpr u8 kNudgeMinus6[] = {0x0A, 0x0B, 0x0E, 0x13, 0x14, 0x1F, 0x29, 0x2A, 0x3D, 0x47, 0x5D, 0x78, 0x79, 0x88, 0x99, 0xAC, 0xB5};
constexpr u8 kNudgeMinus18[] = {0x0D, 0x17, 0x24, 0x46, 0x51, 0x71, 0x89, 0x8A, 0x8D, 0x95, 0xA4, 0xA5, 0xA6, 0xB7, 0xD6};
constexpr u8 kNudgeMinus24 = 0x7E;

template <size_t N>
bool listed(const u8 (&ids)[N], u8 id) {
  return std::find(std::begin(ids), std::end(ids), id) != std::end(ids);
}

float pick_nudge(u8 id) {
  if (listed(kNudgeMinus12, id)) return -12.0f;
  if (listed(kNudgePlus6, id)) return 6.0f;
  if (listed(kNudgeMinus6, id)) return -6.0f;
  if (listed(kNudgeMinus18, id)) return -18.0f;
  return id == kNudgeMinus24 ? -24.0f : 0.0f;
}

// The style of a name in language `lang_i` (0 = no style: the slot's own scale).
u8 name_style(int lang_i, u8 id) {
  auto find = [id](const auto& table) -> u8 {
    for (const NameStyle& entry : table)
      if (entry.id == id) return entry.style;
    return 0;
  };
  switch (lang_i) {
    case 0: return find(kStylesEn);
    case 1: return find(kStylesFr);
    case 2: return find(kStylesIt);
    case 3: return find(kStylesDe);
    case 4: return find(kStylesEs);
    default: return 0;
  }
}
// Monster names: sysmes category 0x19, index = pool slot (= monster record).
constexpr u32 kNameCategory = 0x19u << 16;

}  // namespace

struct MonsterPickScreen::Impl {
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch* text = nullptr;
  render::Sampler linear, nearest;
  render::Texture white_tex, tile_tex, cursor_tex;
  render::TexRef white = {}, tile = {}, cursor = {};
  import::LangTables lt;
  import::Lang lang = import::Lang::En;
  font::FontSet fonts;
  bool hd = true;
  MonsterModelView* models = nullptr;
  Backdrop backdrop;

  MonsterPicker picker;
  NavPoller nav;
  bool open = false, finished = false;
  float acc = 0;
  u32 tick = 0;
  std::array<int, 3> out{-1, -1, -1};
  // PS1 -> window mapping of the current draw.
  render::Letterbox lb;
  bool prep = false;

  bool jp() const { return lang == import::Lang::Jp; }
  int slot() const { return jp() ? font::kKb : font::kBn; }
  int lang_index() const { return std::clamp(static_cast<int>(lang), 0, 5); }
  float wx(float x) const { return lb.ox + x * lb.scale; }
  float wy(float y) const { return lb.oy + y * lb.scale_y; }

  std::u32string name(int pool_slot) {
    if (pool_slot < 0) return {};
    if (const std::string* found = lt.sysmes(kNameCategory | static_cast<u32>(pool_slot), lang); found) return import::utf8_to_u32(*found);
    return {};
  }
  int glyphs(int pool_slot) {
    if (pool_slot < 0) return 0;
    const std::string* found = lt.sysmes(kNameCategory | static_cast<u32>(pool_slot), import::Lang::Jp);
    return found ? std::min(kNameGlyphs, static_cast<int>(import::utf8_to_u32(*found).size())) : 0;
  }

  bool make(render::Texture& texture, render::TexRef& ref, u32 w, u32 h, const void* rgba, bool lin) {
    if (!rgba || !texture.create(*ctx, w, h, render::TextureFormat::Rgba8, rgba)) return false;
    ref = render::TexRef(texture, lin ? linear : nearest);
    return true;
  }
  // The backdrop tile: the fourth TIM of the field's system texture pack.
  void load_tile() {
    if (auto raw = vfs->read_disc("cdrom/map/etc/systim.bin"))
      if (auto cpt = import::parse_cpt(ByteSpan(raw->data(), raw->size())); cpt && cpt->entries.size() > 4)
        if (auto tim = import::parse_tim(cpt->entries[4].data))
          if (auto decoded = import::decode_tim(*tim)) {
            const import::Image rgba = import::to_rgba(*decoded);
            if (make(tile_tex, tile, rgba.width, rgba.height, rgba.pixels.data(), false)) return;
          }
    log::warn("monster picker: map/etc/systim.bin backdrop tile unavailable");
  }
  void load_cursor() {
    render::DecodedImage img;
    if (auto raw = vfs->read("hd/menu/monmenu_cur.png"); raw && render::decode_image(*raw, img) &&
                                                          make(cursor_tex, cursor, img.width, img.height, img.rgba.data(), true))
      return;
    log::warn("monster picker: hd/menu/monmenu_cur.png unavailable");
  }

  void quad(render::TexRef set, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 colour = 0xFFFFFFFFu) {
    if (prep || !set) return;
    sprites->draw(set, wx(x), wy(y), w * lb.scale, h * lb.scale_y, u0, v0, u1, v1, colour);
  }
  // A creature name at (x, y), left-aligned, in the name style of its pool slot.
  void label(int pool_slot, float x, float y, u32 colour) {
    const std::u32string str = name(pool_slot);
    if (str.empty()) return;
    if (prep) return fonts.prepare(slot(), str);
    const font::SlotInfo& info = font::slot(slot());
    float scale = info.scale;
    if (const u8 style = name_style(lang_index(), static_cast<u8>(pool_slot)); style) {
      const StyleScale& entry = kStyleScale[style - kFirstStyle][lang_index()];
      scale = entry.scale, y += entry.dy;
    }
    const float k = lb.scale / kCanvasPerPx;
    const float cx = wx(x) / k + info.off_x * scale, cy = wy(y) / k + info.off_y * scale;
    fonts.draw(slot(), str, cx, cy, font::h1(colour, 31, 4), scale, font::kLeft, k);
  }
  void draw_cursor(float x, float y) {
    const int frame = static_cast<int>(tick % (kCursorFrames * kCursorPeriod)) / kCursorPeriod;
    // monmenu_cur.png: the right-pointing frames in the left column (32 x 48 cells of an 80 x 240 sheet).
    const float u1 = 32.0f / 80.0f, v0 = static_cast<float>(frame) / kCursorFrames, v1 = static_cast<float>(frame + 1) / kCursorFrames;
    quad(cursor, x, y + kCursorDy, kCursorSize, kCursorSize, 0, v0, u1, v1);
  }

  void draw_back(float vw, float vh) {
    if (prep) return;
    sprites->draw(white, 0, 0, vw, vh, 0, 0, 1, 1, render::pack_rgba(0, 0, 0));
    for (float y = 0; y < kBackdropH; y += kTile)
      for (float x = 0; x < kBackdropW; x += kTile) {
        const float h = std::min(kTile, kBackdropH - y);
        quad(tile, x, y, kTile, h, 0, 0, 1, h / kTile);
      }
  }
  // The picks' names and the cursor.
  void draw_picks() {
    for (int pick_slot = 0; pick_slot < MonsterPicker::kPicks; ++pick_slot) {
      const int pool_slot = picker.pick_slot(pick_slot);
      if (pool_slot < 0) continue;
      const float nudge = jp() ? 0.0f : pick_nudge(static_cast<u8>(pool_slot));
      const float x = kPickNameX + kColumnPitch * static_cast<float>(pick_slot) - kHalfGlyph * static_cast<float>(glyphs(pool_slot)) + nudge;
      label(pool_slot, x, kPickY, pick_slot == picker.slot() ? kOrange : kWhite);
    }
    if (picker.mode() == MonsterPicker::Mode::Slots) {
      draw_cursor(kPickCursorX + kColumnPitch * static_cast<float>(picker.slot()), kPickCursorY);
    } else {
      draw_cursor(kListX + kColumnPitch * static_cast<float>(picker.column()), kListY + MonsterPicker::kRowPitch * static_cast<float>(picker.row()) - 2);
    }
  }
  // The visible rows (and the one scrolling in) of the list.
  void draw_list() {
    const int first = picker.top() * MonsterPicker::kColumns;
    const float offset = static_cast<float>(picker.list_offset());
    for (int position = first; position < first + (MonsterPicker::kVisibleRows + 1) * MonsterPicker::kColumns; ++position) {
      const int pool_slot = picker.entry(position);
      if (pool_slot < 0) continue;
      const int row = position / MonsterPicker::kColumns, col = position % MonsterPicker::kColumns;
      u32 colour = picker.taken(position) ? kRed : kWhite;
      if (picker.mode() == MonsterPicker::Mode::List && position == picker.cursor()) colour = kOrange;
      label(pool_slot, kListNameX + kColumnPitch * static_cast<float>(col),
            kListY + MonsterPicker::kRowPitch * static_cast<float>(row) + offset + kListTextDy, colour);
    }
  }
};

MonsterPickScreen::MonsterPickScreen() : impl_(std::make_unique<Impl>()) {}
MonsterPickScreen::~MonsterPickScreen() { shutdown(); }

bool MonsterPickScreen::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text,
                             import::Lang lang) {
  Impl& impl = *impl_;
  impl.ctx = &ctx, impl.vfs = &vfs, impl.sprites = &sprites, impl.text = &text, impl.lang = lang;
  impl.lt.init(&vfs);
  if (!impl.linear.create(ctx, true) || !impl.nearest.create(ctx, false)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!impl.make(impl.white_tex, impl.white, 1, 1, px, false)) return false;
  impl.fonts.init(vfs, text);
  impl.fonts.set_classic(!impl.hd);
  impl.load_tile();
  impl.load_cursor();
  return true;
}

void MonsterPickScreen::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  impl.white = impl.tile = impl.cursor = {};
  impl.white_tex.destroy();
  impl.tile_tex.destroy();
  impl.cursor_tex.destroy();
  impl.linear.destroy();
  impl.nearest.destroy();
  impl.ctx = nullptr;
}

void MonsterPickScreen::set_hd(bool hd) {
  impl_->hd = hd;
  impl_->fonts.set_classic(!hd);
}
void MonsterPickScreen::set_language(import::Lang lang) { impl_->lang = lang; }
void MonsterPickScreen::set_model_view(MonsterModelView* view) { impl_->models = view; }
void MonsterPickScreen::set_backdrop(Backdrop backdrop) { impl_->backdrop = std::move(backdrop); }

void MonsterPickScreen::open(const std::array<u16, 16>& pool, int round, MonsterPicker::Table& table) {
  Impl& impl = *impl_;
  impl.picker.begin(pool, round, table);
  impl.open = true;
  impl.finished = false;
  impl.acc = 0;
  impl.tick = 0;
  impl.nav = NavPoller{};
  impl.out = {-1, -1, -1};
  log::info("monster picker: round {} opened, picks {} {} {}", round, impl.picker.pick_slot(0), impl.picker.pick_slot(1), impl.picker.pick_slot(2));
}

bool MonsterPickScreen::active() const { return impl_->open; }

int MonsterPickScreen::update(const platform::Input& in, float dt) {
  Impl& impl = *impl_;
  if (!impl.open || impl.finished) return 0;
  const Nav nav = impl.nav.poll(in, dt);
  impl.acc += dt * 60.0f;
  const int frames = static_cast<int>(impl.acc);
  impl.acc -= static_cast<float>(frames);
  u16 repeat = 0, pressed = 0;
  if (nav.left) repeat |= MonsterPicker::kLeft;
  if (nav.right) repeat |= MonsterPicker::kRight;
  if (nav.up) repeat |= MonsterPicker::kUp;
  if (nav.down) repeat |= MonsterPicker::kDown;
  if (nav.confirm) pressed |= MonsterPicker::kConfirm;
  if (nav.cancel) pressed |= MonsterPicker::kCancel;
  int sfx = 0;
  for (int i = 0; i < frames && !impl.picker.done(); ++i) {
    ++impl.tick;
    if (const int played = impl.picker.frame(i == 0 ? repeat : 0, i == 0 ? pressed : 0)) sfx = played;
  }
  if (impl.picker.done()) {
    impl.finished = true;
    for (int k = 0; k < MonsterPicker::kPicks; ++k) impl.out[static_cast<size_t>(k)] = impl.picker.pick_slot(k);
    log::info("monster picker: round {} closed, party {} {} {}", impl.picker.round(), impl.out[0], impl.out[1], impl.out[2]);
  }
  return sfx;
}

bool MonsterPickScreen::take_finished() {
  Impl& impl = *impl_;
  if (!impl.finished) return false;
  impl.finished = false;
  impl.open = false;
  return true;
}

std::array<int, 3> MonsterPickScreen::result() const { return impl_->out; }

std::string MonsterPickScreen::describe() const {
  const MonsterPicker& picker = impl_->picker;
  char buffer[128];
  std::snprintf(buffer, sizeof buffer, "%s slot %d col %d row %d top %d picks %d %d %d", picker.mode() == MonsterPicker::Mode::Slots ? "slots" : "list",
                picker.slot(), picker.column(), picker.row(), picker.top(), picker.pick_slot(0), picker.pick_slot(1), picker.pick_slot(2));
  return buffer;
}

void MonsterPickScreen::draw(const render::Frame& frame, float vw, float vh) {
  Impl& impl = *impl_;
  if (!impl.open || !impl.ctx) return;
  impl.lb = render::Letterbox::field(vw, vh);
  // Glyphs first (preparing may rebuild an atlas, never inside a pass).
  impl.prep = true;
  impl.draw_picks();
  impl.draw_list();
  impl.prep = false;

  impl.sprites->begin(frame, vw, vh, true);
  impl.draw_back(vw, vh);
  if (impl.backdrop) impl.backdrop(*impl.sprites, vw, vh);
  impl.sprites->end();

  if (impl.models) {
    MonsterModelSlots slots;
    for (int k = 0; k < MonsterPicker::kPicks; ++k) slots.pool_slot[static_cast<size_t>(k)] = impl.picker.pick_slot(k);
    slots.origin_x = impl.lb.ox, slots.origin_y = impl.lb.oy, slots.unit_x = impl.lb.scale, slots.unit_y = impl.lb.scale_y;
    slots.tick = impl.tick;
    slots.hd = impl.hd;
    impl.models->draw(frame, vw, vh, slots);
  }

  impl.sprites->begin(frame, vw, vh, true);
  impl.draw_picks();
  impl.sprites->end();
  // The list window clips its rows.
  const render::Rect window{static_cast<s32>(std::floor(impl.wx(kListX))), static_cast<s32>(std::floor(impl.wy(kListY))),
                            static_cast<u32>(std::ceil(kListW * impl.lb.scale)), static_cast<u32>(std::ceil(kListH * impl.lb.scale_y))};
  frame.set_scissor(window);
  impl.sprites->begin(frame, vw, vh, true);
  impl.draw_list();
  impl.sprites->end();
  frame.set_scissor(frame.full());
}

}  // namespace oc::ui
