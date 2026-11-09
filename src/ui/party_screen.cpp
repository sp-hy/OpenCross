#include "ui/party_screen.h"
#include "render/camera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/log.h"
#include "data/char_name.h"
#include "game/party_editor.h"
#include "import/tim.h"
#include "ui/localized_path.h"
#include "ui/lowres.h"
#include "ui/menu/game_state.h"
#include "ui/ps1_font.h"
#include "ui/title/nav.h"

namespace oc::ui {

namespace {

constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f;
// The 640-wide screen on the canvas: x * 2, y * 960 / 216.
constexpr float kSx = 2.0f, kSy = kCanvasH / 216.0f;

// Face names by portrait byte: hd/face/<set>/<name>.png.
const char* const kFaceNames[92] = {
    "00_seljyu", "01_kid",      "02_gill",     "03_ishit",   "04_srash",   "05_jakotu",  "06_ridel",    "07_karsh",    "08_zoa",
    "09_mal",    "10_col",      "11_luchi",    "12_poshul",  "13_raz",     "14_zappa",   "15_orcha",    "16_radius",   "17_cap",
    "18_oba",    "19_glen",     "20_rena",     "21_miki",    "22_harler",  "23_janes",   "24_ryu",      "25_star",     "26_spriggan",
    "27_lucky",  "28_kabuo",    "29_rablue",   "30_jilbelt", "31_skarl",   "32_kinoko",  "33_ningyo",   "34_mell",     "35_rea",
    "36_banklif", "37_sunef",   "38_miko",     "39_doc",     "40_gyadaran", "41_yusha",  "42_olha",     "43_C13base",  "44_C13a",
    "45_C13b",   "46_C13aa",    "47_C13ab",    "48_C13bb",   "49_yama",    "50_glen",    "51_yama",     "52_chi_kid",  "53_renkin",
    "54_dowa2",  "55_dario",    "56_dark",     "57_seljyu2", "58_green",   "59_white",   "60_black",    "61_blue",     "62_red",
    "63_yellow", "64_yama2",    "65_chrono",   "66_c_dario", "67_c_glen",  "68_c_karsh", "69_waduki",   "70_migel",    "71_garai",
    "72_marji",  "73_fio",      "74_raz_ane",  "75_l_bros",  "76_l_friend", "77_maburle", "78_galdorb", "79_wife",     "80_ban_rich",
    "81_ban_bin", "82_jigoku",  "kao00",       "kao00",      "kao00",      "kao00",      "kao00",       "kao00",       "kao00",
    "l_zappa",   "kid_2"};

// The PS1 colour word of an entry (0x00BBGGRR, 0x80 = 1.0) as a canvas colour.
u32 c80(u32 colour, u8 a = 255) {
  auto channel = [&](int shift) { return static_cast<u8>(std::min(255u, ((colour >> shift) & 0xFF) * 255u / 0x80)); };
  return render::pack_rgba(channel(0), channel(8), channel(16), a);
}
// Saturating per-channel subtract (the commit fade).
u32 sub(u32 colour, int amount) {
  auto channel = [&](int shift) { return static_cast<u32>(std::max(0, static_cast<int>((colour >> shift) & 0xFF) - amount)) << shift; };
  return channel(0) | channel(8) | channel(16);
}

// Party swap's reserve (OpenCross): the Current Party window becomes a panel of two rows in the space of the help text. Its top
// (backdrop rows kPartyTop..kPartyTopEnd, outer line at 153, label gap up to x kPartyGapEnd) is raised by kPanelShift, its side lines run
// to kPanelBottom, and its top drawn upside down there is the bottom edge, whose label gap holds "Reserve" (kReserveLabelDrop below
// Current Party's label position, as far as the flipped outer line). The party row moves up with the top; the reserve row is at kReserveY.
constexpr float kStatusBottom = 110, kPartyTop = 150, kPartyTopEnd = 163, kPartyBottom = 200, kPartyGapEnd = 203;
constexpr float kPanelShift = -37, kReserveY = 164, kPanelBottom = 197, kReserveLabelDrop = 51;
const char32_t* const kReserveLabel[6] = {U"Reserve", U"Réserve", U"Riserva", U"Reserve", U"Reserva", U"控え"};

struct Rec {
  int hp = 0, max_hp = 0;
  // The six stats in the screen's order.
  std::array<int, 6> st{};
  int innate = 0, portrait = 0, body = 0;
};

}  // namespace

struct PartyScreen::Impl {
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch* text = nullptr;
  render::Sampler linear, nearest;
  render::Texture white_tex;
  render::TexRef white = {};
  import::Lang lang = import::Lang::En;
  import::LangTables lt;
  bool hd = true;
  std::string portrait_set = "2021";
  const std::map<int, std::u32string>* custom = nullptr;
  // Font slots: 0 kb_70, 4 bn_70, 9 ik_70 (large_ / small_ by art mode).
  font::FontSet fonts;
  // Classic art + [Video] Ps1ResUi.
  LowRes lowres;

  struct Tex {
    render::Texture tex;
    render::TexRef set = {};
    float w = 0, h = 0;
  };
  // By path (HD pngs and decoded PS1 TIMs).
  std::map<std::string, std::unique_ptr<Tex>> texs;

  // Disc tables read from the menu overlay: label sprite positions (the first three entries) and element colours.
  struct Sprt {
    int x = 0, y = 0;
  };
  std::array<Sprt, 3> spr{};
  std::array<u8, 24> elem_rgb{};
  bool tables_ok = false;

  // State
  Kind kind = Kind::Party;
  game::PartyEditor ed;
  game::ReliefEditor rel;
  NavPoller nav;
  std::array<Rec, 44> recs{};
  enum class Phase { Idle, Run, Close, Finished } phase = Phase::Idle;
  // frame: frames since open (fade-in: k = 0xE0 - 0x20 (frame - 1) for frames 1..7);
  // close_frame: frames of the closing loop.
  int frame = 0;
  int close_frame = 0;
  float acc = 0;
  bool finished_flag = false;
  std::array<u8, 3> out{0xFF, 0xFF, 0xFF};
  u8 relief_out = 0xFF;
  // Party swap's reserve slots (0 = none) and the slots to write back.
  int reserve_size = 0;
  std::vector<int> reserve_out;

  const game::PartyGrid& grid() const { return kind == Kind::Relief ? static_cast<const game::PartyGrid&>(rel) : ed; }
  bool reserve_shown() const { return kind == Kind::Party && !ed.reserve().empty(); }
  // " reserve a b c" (-1 = empty) when the screen has a reserve.
  std::string reserve_text() const {
    std::string listed;
    if (!ed.reserve().empty()) {
      listed = " reserve";
      for (int id : ed.reserve()) listed += " " + std::to_string(id);
    }
    return listed;
  }

  // ---------------------------------------------------------------- resources
  Tex* store(const std::string& key, u32 w, u32 h, const void* rgba, bool lin) {
    auto created = std::make_unique<Tex>();
    if (!rgba || !created->tex.create(*ctx, w, h, render::TextureFormat::Rgba8, rgba)) {
      log::warn("party screen: {} unavailable", key);
      texs[key] = nullptr;
      return nullptr;
    }
    created->set = render::TexRef(created->tex, lin ? linear : nearest);
    created->w = static_cast<float>(w), created->h = static_cast<float>(h);
    Tex* result = created.get();
    texs[key] = std::move(created);
    return result;
  }
  Tex* png(const std::string& member) {
    // The cache key makes a mod switch load the file again.
    const std::string key = vfs->cache_key(member);
    if (auto it = texs.find(key); it != texs.end()) return it->second.get();
    render::DecodedImage img;
    if (auto raw = vfs->read(member); raw && render::decode_image(*raw, img)) return store(key, img.width, img.height, img.rgba.data(), true);
    return store(key, 0, 0, nullptr, true);
  }
  // A PS1 TIM from the disc (never the cdrom2 overrides), nearest sampled.
  Tex* tim(const std::string& member) {
    if (auto it = texs.find(member); it != texs.end()) return it->second.get();
    if (auto raw = vfs->read_disc(member))
      if (auto parsed = import::parse_tim(ByteSpan(raw->data(), raw->size())))
        if (auto dec = import::decode_tim(*parsed)) {
          const import::Image rgba = import::to_rgba(*dec);
          return store(member, rgba.width, rgba.height, rgba.pixels.data(), false);
        }
    return store(member, 0, 0, nullptr, false);
  }
  // Face folder: "1999" in original art; in HD the [Game] Portraits choice.
  std::string face_set() const { return hd ? portrait_set : std::string("1999"); }
  // Portrait of a character by its portrait byte (0x0E -> 0x5A).
  Tex* face(int character) {
    if (character < 0 || character >= 44) return nullptr;
    int pb = recs[static_cast<size_t>(character)].portrait;
    if (pb < 0 || pb >= 92) return nullptr;
    if (pb == 0x0E) pb = 0x5A;
    // Classic art + [Video] Ps1ResUi: the PS1 face TIM of the same picture (cdrom/face/<stem>.tim,
    // 64 x 64, from cdrom.dat; hd/face/1999/NN_<stem>.png is its 4x redraw). The disc has one such TIM for every
    // portrait byte 0..91 (0x0E -> 0x5A l_zappa).
    if (!hd && ps1_menus()) {
      const std::string nm = kFaceNames[pb];
      const std::string stem = nm.size() > 3 && nm[2] == '_' ? nm.substr(3) : nm;
      if (Tex* disc_face = tim("cdrom/face/" + stem + ".tim"); disc_face && disc_face->set) return disc_face;
    }
    return png("hd/face/" + face_set() + "/" + kFaceNames[pb] + ".png");
  }

  void load_tables() {
    auto prg = vfs->read_disc("cdrom/prog/memmenu.prg");
    tables_ok = false;
    if (!prg || prg->size() < 0x1F00) return;
    const Bytes& image = *prg;
    auto rd16 = [&](size_t offset) { return static_cast<int>(static_cast<s16>(image[offset] | (image[offset + 1] << 8))); };
    auto at = [](u32 address) { return static_cast<size_t>(address - 0x801EE000u); };
    // Sprite records: {x, y, w, h, u, v, -, -, clut x, clut y, colour}.
    for (size_t i = 0; i < spr.size(); ++i) {
      const size_t offset = at(0x801F1DEC) + i * 0x16;
      spr[i] = {rd16(offset), rd16(offset + 2)};
    }
    for (size_t i = 0; i < elem_rgb.size(); ++i) elem_rgb[i] = image[at(0x801F1ECC) + i];
    // Sanity check: label sprite 3 is 176 wide.
    tables_ok = rd16(at(0x801F1DEC) + 3 * 0x16 + 4) == 176;
    if (!tables_ok) log::warn("party screen: memmenu.prg tables do not match");
  }

  // ---------------------------------------------------------------- text
  bool jp() const { return lang == import::Lang::Jp; }
  // The "def" font slot of the lists.
  int def_slot() const { return jp() ? font::kKb : font::kBn; }
  std::u32string to_u32(const std::string& utf8) const { return import::utf8_to_u32(utf8); }
  std::u32string mm(int key) {
    if (const import::TextTable* table = lt.table("memmenu"))
      if (const std::string* found = table->get(static_cast<u64>(key), lang); found) return to_u32(*found);
    return {};
  }
  // cmnmes translation: the JP key's translation; `found` = false keeps the JP text (drawn in the JP font).
  std::u32string cm(const std::string& key, bool* found) {
    if (const import::TextTable* table = lt.table("cmnmes"))
      if (const std::string* translated = table->get(key, lang); translated && !translated->empty()) {
        *found = true;
        return to_u32(*translated);
      }
    *found = false;
    return to_u32(key);
  }
  std::u32string name(int character) {
    if (character < 0 || character >= 44) return {};
    return data::character_name(character, recs[static_cast<size_t>(character)].body, data::NameContext::Menu, false, {custom, &lt, lang});
  }

  // One text entry: canvas X = x * 2 + offX * scale, Y = y * 960/216 + offY * scale, scale = slot scale * mul.
  // Normal = c16 (x) colour, outline = colour * kB / 31; H4 (`shadow`) draws the drop shadow in the outline colour, H3 none.
  float zoom = 1, ox = 0, oy = 0;
  // Widescreen: the backdrop and fades cover PS1 x edge_x0 .. edge_x0 + edge_w (the window's widened frame). While `grouped`, everything
  // left of the gap between the windows (status, current party) moves `spread` PS1 px left and the rest (member grid) as far right;
  // [Video] WideHud.
  static constexpr float kGroupSplit = 300.0f;
  float canvas_cols = 0, layout_cols = 0, edge_x0 = 0, edge_w = 640, spread = 0;
  bool grouped = false;
  float group_dx(float x) const { return !grouped ? 0.0f : x < kGroupSplit ? -spread : spread; }
  // draw() runs the screen twice: a preparing pass (loads every texture, prepares every glyph: neither may happen between the sprite
  // batch's begin / end) and the drawing pass.
  bool prep = false;
  void draw_text(int slot, float mul, float x, float y, const std::u32string& str, u32 col, u16 c16, int kB, bool shadow, float dy_canvas = 0) {
    if (str.empty()) return;
    if (prep) return fonts.prepare(slot, str);
    const render::FaceId face_id = fonts.face(slot);
    if (face_id == render::kNoFace) return;
    const font::SlotInfo& si = font::slot(slot);
    const float scale = si.scale * mul;
    const float canvas_x = (x + group_dx(x)) * kSx + si.off_x * scale, canvas_y = y * kSy + si.off_y * scale + dy_canvas;
    const font::Ink ink = shadow ? font::h4(col, c16, kB) : font::h3(col, c16, kB);
    render::TextStyle st;
    st.backdrop = true;
    st.backdrop_color = ink.outline;
    st.shadow = ink.shadow;
    text->draw(face_id, str, ox + canvas_x * zoom, oy + canvas_y * zoom, ink.normal, scale * zoom, st);
  }

  // ---------------------------------------------------------------- sprites
  // A quad in PS1 px of the 640-wide screen; `clip` (PS1 px, x0 y0 x1 y1) cuts it and its texture coordinates (the PS1
  // draw area). u0..v1 normalised.
  struct Clip {
    float x0 = -1e9f, y0 = -1e9f, x1 = 1e9f, y1 = 1e9f;
  };
  void draw_quad(render::TexRef set, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 colour, const Clip& cl = {},
                 float ys = kSy, float yoff = 0) {
    if (prep || !set || w <= 0 || h <= 0) return;
    float left = std::max(x, cl.x0), top = std::max(y, cl.y0), right = std::min(x + w, cl.x1), bottom = std::min(y + h, cl.y1);
    if (left >= right || top >= bottom) return;
    if (grouped && left < kGroupSplit && right > kGroupSplit) {
      Clip half = cl;
      half.x1 = kGroupSplit;
      draw_quad(set, x, y, w, h, u0, v0, u1, v1, colour, half, ys, yoff);
      half = cl;
      half.x0 = kGroupSplit;
      draw_quad(set, x, y, w, h, u0, v0, u1, v1, colour, half, ys, yoff);
      return;
    }
    const float dx = group_dx(left);
    const float a0 = u0 + (u1 - u0) * (left - x) / w, a1 = u0 + (u1 - u0) * (right - x) / w;
    const float b0 = v0 + (v1 - v0) * (top - y) / h, b1 = v0 + (v1 - v0) * (bottom - y) / h;
    sprites->draw(set, ox + (left + dx) * kSx * zoom, oy + (y * kSy + yoff + (top - y) * kSy) * zoom, (right - left) * kSx * zoom,
                  (bottom - top) * ys * zoom, a0, b0, a1, b1, colour);
  }
  // Portrait 64 x 32 PS1 px, drawn 4.0 canvas px per PS1 row and 7 px lower for portrait bytes 0..0x31.
  void portrait(int character, float x, float y, u32 col, const Clip& cl = {}) {
    Tex* face_tex = face(character);
    if (!face_tex) return;
    const int pb = recs[static_cast<size_t>(character)].portrait == 0x0E ? 0x5A : recs[static_cast<size_t>(character)].portrait;
    if (pb < 0x32) draw_quad(face_tex->set, x, y, 64, 32, 0, 0, 1, 1, col, cl, 4.0f, 7.0f);
    else draw_quad(face_tex->set, x, y, 64, 32, 0, 0, 1, 1, col, cl);
  }

  void draw_screen();
};

PartyScreen::PartyScreen() : impl_(std::make_unique<Impl>()) {}
PartyScreen::~PartyScreen() { shutdown(); }

bool PartyScreen::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text, import::Lang lang) {
  Impl& impl = *impl_;
  impl.ctx = &ctx, impl.vfs = &vfs, impl.sprites = &sprites, impl.text = &text, impl.lang = lang;
  impl.lt.init(&vfs);
  if (!impl.linear.create(ctx, true) || !impl.nearest.create(ctx, false)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!impl.white_tex.create(ctx, 1, 1, render::TextureFormat::Rgba8, px)) return false;
  impl.white = render::TexRef(impl.white_tex, impl.nearest);
  impl.fonts.init(vfs, text);
  impl.fonts.set_classic(!impl.hd);
  impl.load_tables();
  return impl.fonts.face(font::kBn, false) != render::kNoFace;
}

void PartyScreen::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  impl.texs.clear();
  impl.lowres.shutdown();
  impl.white = {};
  impl.white_tex.destroy();
  impl.linear.destroy();
  impl.nearest.destroy();
  impl.ctx = nullptr;
}

void PartyScreen::set_hd(bool hd) {
  impl_->hd = hd;
  impl_->fonts.set_classic(!hd);
}
void PartyScreen::set_portrait_set(const std::string& set) { impl_->portrait_set = set == "1999" ? "1999" : "2021"; }
void PartyScreen::set_custom_names(const std::map<int, std::u32string>* names) { impl_->custom = names; }

namespace {
// The grid = recruited and not excluded characters, ascending ids.
std::vector<int> grid_of(const save::SaveSlot& sl) {
  std::vector<int> grid;
  for (int character = 0; character < 44; ++character) {
    const bool excl = ((sl.unk_e1b8[static_cast<size_t>(2 + (character >> 5))] >> (character & 31)) & 1) != 0;
    if (sl.recruited(character) && !excl) grid.push_back(character);
  }
  return grid;
}
}  // namespace

void PartyScreen::open(const GameState& gs) {
  Impl& impl = *impl_;
  const save::SaveSlot& sl = gs.slot;
  const std::vector<int> grid = grid_of(sl);
  const std::array<u8, 3> party = sl.party();
  impl.kind = Kind::Party;
  impl.ed.begin(grid, {party[0] == 0xFF ? -1 : party[0], party[1] == 0xFF ? -1 : party[1], party[2] == 0xFF ? -1 : party[2]},
                {sl.unk_e1b8[0], sl.unk_e1b8[1]}, impl.reserve_size > 0 ? gs.reserve(impl.reserve_size) : std::vector<int>{});
  impl.reserve_out = impl.ed.reserve();
  for (int character = 0; character < 44; ++character) {
    const save::SaveChar& ch = sl.chars[static_cast<size_t>(character)];
    Rec& rec = impl.recs[static_cast<size_t>(character)];
    rec.hp = ch.cur_hp, rec.max_hp = ch.max_hp;
    rec.st = {ch.atk, ch.hit, ch.def, ch.evd, ch.mag, ch.mdef};
    rec.innate = gs.innate(character);
    rec.portrait = ch.model_id;
    rec.body = data::character_body(ch.model_id, [&](int var) { return sl.var(var); });
  }
  impl.phase = Impl::Phase::Run;
  impl.frame = 0, impl.close_frame = 0, impl.acc = 0;
  impl.finished_flag = false;
  impl.nav = NavPoller{};
  std::string ids;
  for (int character : grid) ids += std::to_string(character) + " ";
  log::info("party screen: opened, party {} {} {}, grid {}, lock {:08X} {:08X}, excluded {:08X} {:08X}", party[0], party[1], party[2], ids,
            sl.unk_e1b8[0], sl.unk_e1b8[1], sl.unk_e1b8[2], sl.unk_e1b8[3]);
}

void PartyScreen::open_relief(const GameState& gs) {
  // Records, fades, the grid.
  open(gs);
  Impl& impl = *impl_;
  const save::SaveSlot& sl = gs.slot;
  int relief_member = sl.head[kReliefHead];
  // A relief member that is no longer recruited (id < 0x2C with its recruited bit clear) -> 0xFF.
  if (relief_member >= 0x2C || !sl.recruited(relief_member)) relief_member = -1;
  impl.kind = Kind::Relief;
  impl.rel.begin(grid_of(sl), relief_member);
  impl.relief_out = relief_member < 0 ? 0xFF : static_cast<u8>(relief_member);
  log::info("party screen: relief screen opened, relief member {}", relief_member);
}

void PartyScreen::set_wide_columns(float canvas_columns, float layout_columns) {
  impl_->canvas_cols = std::max(0.0f, canvas_columns);
  impl_->layout_cols = std::clamp(layout_columns, 0.0f, impl_->canvas_cols);
}
bool PartyScreen::active() const { return impl_->phase != Impl::Phase::Idle; }
PartyScreen::Kind PartyScreen::kind() const { return impl_->kind; }

int PartyScreen::update(const platform::Input& in, float dt) {
  Impl& impl = *impl_;
  if (impl.phase == Impl::Phase::Idle || impl.phase == Impl::Phase::Finished) return 0;
  const Nav nav = impl.nav.poll(in, dt);
  impl.acc += dt * 60.0f;
  int frames = static_cast<int>(impl.acc);
  impl.acc -= static_cast<float>(frames);
  int sfx = 0;
  if (impl.phase == Impl::Phase::Run) {
    const bool relief = impl.kind == Kind::Relief;
    for (int i = 0; i < frames; ++i) {
      ++impl.frame;
      if (relief) impl.rel.tick();
      else impl.ed.tick();
    }
    using Key = game::PartyKey;
    auto press = [&](Key key) { return relief ? impl.rel.press(key) : impl.ed.press(key); };
    // Keys are handled in the order right, left, up, down, confirm, cancel.
    if (nav.right) sfx = press(Key::Right);
    if (nav.left) sfx = press(Key::Left);
    if (nav.up) sfx = press(Key::Up);
    if (nav.down) sfx = press(Key::Down);
    if (nav.confirm) sfx = press(Key::Confirm);
    else if (nav.cancel) sfx = press(Key::Cancel);
    if (sfx) log::info("party screen: sfx {:#x}, {}", sfx, describe());
    if (relief ? impl.rel.done() : impl.ed.done()) {
      impl.phase = Impl::Phase::Close;
      impl.close_frame = 0;
      if (relief) {
        impl.relief_out = impl.rel.relief() < 0 ? 0xFF : static_cast<u8>(impl.rel.relief());
        log::info("party screen: relief screen closed, relief member {}", impl.rel.relief());
      } else {
        const std::vector<int> result = impl.ed.result();
        impl.out = {0xFF, 0xFF, 0xFF};
        for (size_t i = 0; i < result.size() && i < 3; ++i) impl.out[i] = static_cast<u8>(result[i]);
        impl.reserve_out = impl.ed.reserve();
        log::info("party screen: closed, party {} {} {}{}", impl.out[0], impl.out[1], impl.out[2], impl.reserve_text());
      }
    }
  } else if (impl.phase == Impl::Phase::Close) {
    impl.close_frame += frames;
    // The closing loop runs while the counter < 0x41.
    if (impl.close_frame >= 0x40) {
      impl.phase = Impl::Phase::Finished;
      impl.finished_flag = true;
    }
  }
  return sfx;
}

bool PartyScreen::take_finished() {
  Impl& impl = *impl_;
  if (!impl.finished_flag) return false;
  impl.finished_flag = false;
  impl.phase = Impl::Phase::Idle;
  return true;
}
std::array<u8, 3> PartyScreen::result() const { return impl_->out; }
u8 PartyScreen::relief_result() const { return impl_->relief_out; }
std::vector<int> PartyScreen::reserve_result() const { return impl_->reserve_out; }
void PartyScreen::set_reserve_size(int size) { impl_->reserve_size = std::clamp(size, 0, 41); }
void PartyScreen::set_return_to_slot(bool on) { impl_->ed.set_return_to_slot(on); }

std::string PartyScreen::describe() const {
  const Impl& impl = *impl_;
  char buffer[96];
  if (impl.kind == Kind::Relief) {
    std::snprintf(buffer, sizeof buffer, "relief col %d row %d scroll %d member %d", impl.rel.column(), impl.rel.row(), impl.rel.scroll(),
                  impl.rel.relief());
    return buffer;
  }
  const auto& party_slots = impl.ed.slots();
  std::snprintf(buffer, sizeof buffer, "%s slot %d col %d row %d scroll %d party %d %d %d",
                impl.ed.mode() == game::PartyEditor::Mode::Slots ? "slots" : "grid", impl.ed.slot(), impl.ed.column(), impl.ed.row(), impl.ed.scroll(),
                party_slots[0], party_slots[1], party_slots[2]);
  return buffer + impl.reserve_text();
}

// ---------------------------------------------------------------------------------------------------------------- the screen
void PartyScreen::Impl::draw_screen() {
  const bool relief = kind == Kind::Relief;
  const game::PartyGrid& party_grid = grid();
  const bool grid_mode = relief || ed.mode() == game::PartyEditor::Mode::Pick;
  // Commit fade: fade = 0xE0, 0xC0 .. 0x20 on the first 7 frames, subtracted from every sprite / text colour (style 0x0B
  // twice); closing: -8 per frame on the last published lists.
  const int fade = phase == Phase::Close ? 8 * close_frame : (frame <= 7 ? 0xE0 - 0x20 * std::max(0, frame - 1) : 0);
  auto col = [&](u32 colour) { return c80(sub(colour, fade)); };
  const bool reserve_on = reserve_shown();
  const Clip grid_clip{0, 26, 640, 26 + 172};
  const int ys = party_grid.yscroll();
  // The party row's y (raised into the shared panel while there is a reserve) and the reserve row's.
  const float party_y = reserve_on ? 0xA5 + kPanelShift : 0xA5;
  const int lang_index = static_cast<int>(lang);
  const std::u32string reserve_label = kReserveLabel[std::clamp(lang_index, 0, 5)];
  const int reserve_font = jp() ? font::kKb : font::kBn;

  // The PS1 picture under the lists: the background sprite 640 x 220 at (0, 0), bg.tim through the texture window (0, 0, 64, 32).
  // The windows (w17.tim) are cdrom2's blank sheet in both art modes: nothing. PS1 fades:
  // a subtracted TILE of colour 0xE0 .. 0x20 on the first 7 frames; the closing loop subtracts 8 per frame from the double-buffered picture.
  // Widescreen: the tiles continue over the widened frame.
  grouped = false;
  if (Tex* bg = tim("cdrom/map/etc/bg.tim")) {
    const Clip edges{edge_x0, -1e9f, edge_x0 + edge_w, 1e9f};
    const int first = static_cast<int>(std::floor(edge_x0 / 64.0f)) * 64;
    for (int y = 0; y < 220; y += 32)
      for (int x = first; x < edge_x0 + edge_w; x += 64) {
        const float h = static_cast<float>(std::min(32, 220 - y));
        draw_quad(bg->set, static_cast<float>(x), static_cast<float>(y), 64, h, 0, 0, 1, h / 32.0f, 0xFFFFFFFFu, edges);
      }
  }
  if (!prep) {
    int kp = 0;
    if (phase == Phase::Close) kp = std::min(255, 8 * ((close_frame + 1) / 2));
    else if (frame <= 7) kp = 0xE0 - 0x20 * std::max(0, frame - 1);
    if (kp > 0) {
      sprites->set_blend(render::BlendMode::Subtract);
      draw_quad(white, edge_x0, 0, edge_w, 224, 0, 0, 1, 1, render::pack_rgba(static_cast<u8>(kp), static_cast<u8>(kp), static_cast<u8>(kp)));
      sprites->set_blend(render::BlendMode::Alpha);
    }
  }

  // Main layer: backdrop, grid cells (visible rows), then the party slots (Party) or the fixed slot 0 + silhouette and icon (Relief).
  grouped = true;
  if (Tex* bg = png(localized_path(*vfs, relief ? "hd/menu/shapes/memmenu2.png" : "hd/menu/shapes/memmenu.png", lang))) {
    if (!reserve_on) {
      draw_quad(bg->set, 0, 0, 640, 216, 0, 0, 1, 1, col(0x808080));
    } else {
      // Backdrop rows src_y0..src_y1 of the x range x0..x1 drawn over dst_y0..dst_y1 (src_y0 > src_y1 flips them).
      auto rows = [&](float x0, float x1, float src_y0, float src_y1, float dst_y0, float dst_y1) {
        draw_quad(bg->set, x0, dst_y0, x1 - x0, dst_y1 - dst_y0, x0 / 640, src_y0 / 216, x1 / 640, src_y1 / 216, col(0x808080));
      };
      rows(kGroupSplit, 640, 0, 216, 0, 216);
      rows(0, kGroupSplit, 0, kStatusBottom, 0, kStatusBottom);
      // The shared panel: the Current Party window's top raised, its side lines over both rows, its top flipped as the bottom edge.
      rows(0, kGroupSplit, kPartyTop, kPartyTopEnd, kPartyTop + kPanelShift, kPartyTopEnd + kPanelShift);
      rows(0, kGroupSplit, kPartyTopEnd + 2, kPartyBottom, kPartyTopEnd + kPanelShift, kPanelBottom);
      rows(0, kGroupSplit, kPartyTopEnd, kPartyTop, kPanelBottom, kPanelBottom + (kPartyTopEnd - kPartyTop));
      // The flipped edge's label gap is Current Party's: the part "Reserve" does not cover takes line from right of the gap.
      if (prep) fonts.prepare(reserve_font, reserve_label);
      const float label_end = static_cast<float>(spr[1].x) + fonts.measure(reserve_font, reserve_label, font::slot(reserve_font).scale) / kSx + 4;
      if (label_end < kPartyGapEnd)
        draw_quad(bg->set, label_end, kPanelBottom, kPartyGapEnd - label_end, kPartyTopEnd - kPartyTop, kPartyGapEnd / 640, kPartyTopEnd / 216,
                  (2 * kPartyGapEnd - label_end) / 640, kPartyTop / 216, col(0x808080));
    }
  }
  const int roster_size = static_cast<int>(party_grid.roster().size());
  for (int i = 0; i < roster_size; ++i) {
    const int grid_row = i >> 2;
    if (grid_row < party_grid.scroll() - 1 || grid_row > party_grid.scroll() + 5) continue;
    const bool cur = grid_mode && i == party_grid.cursor();
    portrait(party_grid.cell(i), static_cast<float>((i & 3) * 0x44 + 0x14C), static_cast<float>(ys + grid_row * 0x22 + 0x1C),
             col(cur ? 0x000040 : 0x404040), grid_clip);
  }
  constexpr int kReliefSlot = 2;
  // The reserve slots shown: kReserveShown from the row's scroll, under the party slots' columns.
  const int reserve_first = ed.reserve_scroll();
  const int reserve_last = std::min(static_cast<int>(ed.reserve().size()), reserve_first + game::PartyEditor::kReserveShown);
  if (relief) {
    // Record 0 at (0x40, 0xA5), then the relief member as a black silhouette.
    portrait(0, 0x40, 0xA5, col(0x808080));
    if (rel.relief() >= 0) portrait(rel.relief(), kReliefSlot * 0x44 + 0x40, 0xA5, col(0x000000));
    // The arrow (memmenu2_cur.png, 8 cells of 16 x 16) at (0x84 + (t & 0x1F), 0xAD), n = (~(t >> 3)) & 3.
    if (Tex* ic = png("hd/menu/memmenu2_cur.png")) {
      const int tick = frame, cell = (~(tick >> 3)) & 3;
      draw_quad(ic->set, static_cast<float>(0x84 + (tick & 0x1F)), 0xAD, 16, 16, static_cast<float>(cell) / 8.0f, 0, static_cast<float>(cell + 1) / 8.0f, 1,
                col(0x808080));
    }
  } else {
    for (int member_slot = 0; member_slot < 3; ++member_slot) {
      const int character = ed.slots()[static_cast<size_t>(member_slot)];
      if (character < 0) continue;
      portrait(character, static_cast<float>(member_slot * 0x44 + 0x40), party_y, col(ed.slot() == member_slot ? 0x000000 : 0x404040));
    }
    if (reserve_on)
      for (int index = reserve_first; index < reserve_last; ++index)
        if (const int character = ed.reserve()[static_cast<size_t>(index)]; character >= 0)
          portrait(character, static_cast<float>((index - reserve_first) * 0x44 + 0x40), kReserveY, col(ed.slot() == 3 + index ? 0x000000 : 0x404040));
  }
  // Top layer: lifted grid portrait + grid cursor (grid mode), lifted slot portrait + slot cursor.
  Tex* cur = png("hd/menu/memmenu_cur.png");
  auto cursor = [&](float x, float y, const Clip& cl) {
    if (cur) draw_quad(cur->set, x - 3, y - 2, 69, 35, 0, 0, 1, 1, col(0x808080), cl);
  };
  if (grid_mode) {
    const int i = party_grid.cursor(), character = party_grid.cell(i);
    const bool lift = relief ? character >= 0
                             : character >= 0 && ((!ed.in_party(character) && !ed.in_reserve(character)) || ed.at(ed.slot()) == character);
    if (lift)
      portrait(character, static_cast<float>((i & 3) * 0x44 + 0x14A), static_cast<float>(ys + (i >> 2) * 0x22 + 0x1B), col(0x808080), grid_clip);
    cursor(static_cast<float>((i & 3) * 0x44 + 0x14C), static_cast<float>(ys + (i >> 2) * 0x22 + 0x1C), grid_clip);
  }
  if (relief) {
    if (rel.relief() >= 0) portrait(rel.relief(), kReliefSlot * 0x44 + 0x3E, 0xA4, col(0x808080));
    cursor(kReliefSlot * 0x44 + 0x40, 0xA5, grid_clip);
  } else {
    const bool in_reserve = ed.slot() >= 3;
    const float x = static_cast<float>((in_reserve ? ed.slot() - 3 - reserve_first : ed.slot()) * 0x44 + 0x40);
    const float y = in_reserve ? kReserveY : party_y;
    if (const int character = ed.at(ed.slot()); character >= 0) portrait(character, x - 2, y - 1, col(0x808080));
    cursor(x, y, {});
    // Arrows (the Relief screen's, cell 0; mirrored on the left) when the reserve row scrolls past slots on either side.
    if (Tex* arrow = reserve_on ? png("hd/menu/memmenu2_cur.png") : nullptr) {
      constexpr float kArrow = 10;
      if (reserve_first > 0) draw_quad(arrow->set, 0x34, kReserveY + 11, kArrow, kArrow, 1.0f / 8.0f, 0, 0, 1, col(0x808080));
      if (reserve_last < static_cast<int>(ed.reserve().size()))
        draw_quad(arrow->set, 0x108, kReserveY + 11, kArrow, kArrow, 0, 0, 1.0f / 8.0f, 1, col(0x808080));
    }
  }

  // Text in queue order: help, labels, stat labels, "●", numbers, name header.
  // Help text: key 6, or 5 in grid mode (Relief is always in grid mode). One line per row, def slot, mul 1.2,
  // H4(col, 0x6319, 4), pitch 12 * sy; first line at y 111 (FR / IT) or 120. The shared reserve panel takes its place.
  const std::u32string help = reserve_on ? std::u32string() : mm(6 - (grid_mode ? 1 : 0));
  if (!help.empty()) {
    float y = (lang_index == 1 || lang_index == 3) ? 111.0f : 120.0f;
    size_t line_start = 0;
    for (;;) {
      const size_t line_end = help.find(U'\n', line_start);
      draw_text(def_slot(), 1.2f, 0x3E, y,
                help.substr(line_start, line_end == std::u32string::npos ? std::u32string::npos : line_end - line_start), sub(0x808080, fade), 0x6319,
                4, true);
      if (line_end == std::u32string::npos) break;
      line_start = line_end + 1, y += 12;
    }
  }
  // Labels 0..2 (style 6, forced non-JP: bn_70, mul 1.0, H3(col, 0x6319, 6)): Party at the positions read from the disc tables,
  // Relief "Status" (0x3E, 0x0B) and "Member" (0x14A, 0x0B) (no "Current Party").
  if (relief) {
    draw_text(font::kBn, 1.0f, 0x3E, 0x0B, mm(0), sub(0x808080, fade), 0x6319, 6, false);
    draw_text(font::kBn, 1.0f, 0x14A, 0x0B, mm(2), sub(0x808080, fade), 0x6319, 6, false);
  } else {
    for (int id = 0; id < 3; ++id)
      draw_text(font::kBn, 1.0f, static_cast<float>(spr[static_cast<size_t>(id)].x),
                static_cast<float>(spr[static_cast<size_t>(id)].y) + (id == 1 && reserve_on ? kPanelShift : 0.0f), mm(id), sub(0x808080, fade), 0x6319, 6,
                false);
    // "Reserve" in the flipped bottom edge's gap: Current Party's label moved as far as the edge's outer line.
    if (reserve_on) {
      draw_text(reserve_font, 1.0f, static_cast<float>(spr[1].x), static_cast<float>(spr[1].y) + kReserveLabelDrop, reserve_label, sub(0x808080, fade),
                0x6319, 6, false);
    }
  }
  // Label 3: the stat labels, translated through cmnmes; all H3.
  auto stat = [&](float x, float y, int style, const std::string& key) {
    bool found = false;
    const std::u32string label = cm(key, &found);
    const bool jp_font = jp() || !found;
    if (jp() && style != 0x81) y += 2;
    const int slot = style == 0x81 ? (jp_font ? font::kKb : font::kBn) : (jp_font ? font::kIk : font::kBn);
    draw_text(slot, style == 0x81 ? 1.0f : style == 0x82 ? 0.9f : 0.8f, x, y, label, sub(0x808080, fade), 0x6319, 6, false);
  };
  const float xl = jp() ? 62.0f : 60.0f;
  stat(0x3C, 0x2E, 0x81, "ＨＰ");
  stat(0xA0, 0x2E, 0x81, "／");
  stat(xl, 0x39, 0x82, "腕力");
  stat(0xAA, 0x39, 0x82, "正確さ");
  stat(xl, 0x45, 0x82, "耐久力");
  stat(0xAA, 0x45, 0x82, "身軽さ");
  stat(xl, 0x51, 0x82, "魔力");
  stat(0xAA, 0x51, 0x82, "抗魔");
  stat(0x3C, 0x5F, 0x83, "先天属性");
  const int shown = relief ? rel.shown() : ed.shown();
  // "●" in the innate element's PS1 colour, style 0x0B (slot 0, mul 0.65), darkened twice by the fade.
  if (shown >= 0) {
    int bit = 0;
    for (int bits = recs[static_cast<size_t>(shown)].innate >> 2; bit < 6 && !(bits & 1); ++bit) bits >>= 1;
    const u32 element_colour = elem_rgb[static_cast<size_t>(bit * 3)] | (elem_rgb[static_cast<size_t>(bit * 3 + 1)] << 8) |
                               (elem_rgb[static_cast<size_t>(bit * 3 + 2)] << 16);
    draw_text(font::kKb, 0.65f, static_cast<float>(0x8E + (lang_index == 3 ? 0xD : 0xA)), static_cast<float>(0x5F + (jp() ? 1 : 0)), U"●",
              sub(sub(element_colour, fade), fade), 0x6319, 6, false);
  }
  // Numbers: 3 digits (style 0x84) for HP / max HP, 2 (0x85) for the stats; one-digit values 7 units right; right column + 10
  // (+ 6 in FR / IT). Slot 0 (kb_70) in every language, H3.
  const Rec* shown_rec = shown >= 0 ? &recs[static_cast<size_t>(shown)] : nullptr;
  auto num = [&](int value, int field, int x, int y) {
    const int width = field < 2 ? 3 : 2;
    std::string digits = std::to_string(value);
    int dx = value < 10 ? 7 : 0;
    if (x == 0xE6 && (y == 0x39 || y == 0x45 || y == 0x51)) dx += 0xA + ((lang_index == 1 || lang_index == 2) ? 6 : 0);
    else if (!(x == 0x82 && (y == 0x39 || y == 0x45 || y == 0x51))) dx = 0;
    if (static_cast<int>(digits.size()) < width) digits = std::string(static_cast<size_t>(width) - digits.size(), ' ') + digits;
    else if (static_cast<int>(digits.size()) > width) digits = digits.substr(digits.size() - static_cast<size_t>(width));
    draw_text(font::kKb, field < 2 ? 1.0f : 0.9f, static_cast<float>(x + dx), static_cast<float>(y), to_u32(digits), sub(0x808080, fade), 0x6319, 6,
              false);
  };
  static constexpr int kPos[8][2] = {{0x72, 0x2E}, {0xBA, 0x2E}, {0x82, 0x39}, {0xE6, 0x39}, {0x82, 0x45}, {0xE6, 0x45}, {0x82, 0x51}, {0xE6, 0x51}};
  for (int field = 0; field < 8; ++field) {
    const int value = !shown_rec ? 0 : field == 0 ? shown_rec->hp : field == 1 ? shown_rec->max_hp : shown_rec->st[static_cast<size_t>(field - 2)];
    num(value, field, kPos[field][0], kPos[field][1]);
  }
  // Name header for the slot's member (Party slot mode) or the cursor cell (grid mode, not while scrolling).
  // Style 0x80: def slot, mul 1.2, baseline shrink M 210 (k 56, JP 64), H4(0x696969, 0x6318, 7).
  const int named = grid_mode ? (party_grid.scrolling() ? -1 : party_grid.cell(party_grid.cursor())) : ed.at(ed.slot());
  if (named >= 0) {
    const int slot = def_slot();
    const std::u32string header = name(named);
    if (prep) fonts.prepare(slot, header);
    float mul = 1.2f, dy = 0;
    const float wp = fonts.measure(slot, header, font::slot(slot).scale * mul) / kSx;
    if (wp > 210.0f) {
      const float kk = jp() ? 64.0f : 56.0f;
      mul *= 210.0f / wp;
      dy = kk - kk * 210.0f / wp;
    }
    draw_text(slot, mul, 0x44, 0x1C, header, sub(0x696969, fade), 0x6318, 7, true, dy);
  }
}

void PartyScreen::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  if (impl.phase == Impl::Phase::Idle || !impl.ctx) return;
  // Classic art with [Video] Ps1ResUi (any art mode; the PS1 face TIMs stay classic-only, Impl::face): the screen into a
  // 640 x 224 target (216 rows shown = y * 960 / 216), scaled up nearest.
  // Widescreen: the target grows with the canvas (640 + 4 e texels) and is shown in the window rectangle of the widened 240-row frame.
  render::Frame lf;
  if (ps1_menus()) {
    if (!impl.lowres.ready()) impl.lowres.init(*impl.ctx, *impl.sprites);
    const float canvas_w = kCanvasW + 8.0f * impl.canvas_cols;
    Ps1Display display = kMemmenu;
    display.w = static_cast<u32>(std::lround(static_cast<float>(display.w) * canvas_w / kCanvasW));
    if (impl.lowres.begin(frame, display, impl.text, lf, canvas_w, kCanvasH)) {
      draw_view(lf, canvas_w, kCanvasH, resume);
      if (impl.canvas_cols > 0.0f) {
        const render::Letterbox letterbox = render::Letterbox::wide(vw, vh, impl.canvas_cols);
        const render::Letterbox::Rect target_rect{letterbox.left(), letterbox.oy, letterbox.width(), render::kLayerH * letterbox.scale};
        impl.lowres.end(frame, vw, vh, &target_rect);
      } else {
        impl.lowres.end(frame, vw, vh);
      }
      return;
    }
  }
  draw_view(frame, vw, vh, resume);
}

void PartyScreen::draw_view(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  // One layer column = 2 PS1 px of this screen.
  const render::Letterbox letterbox = render::Letterbox::wide(vw, vh, impl.canvas_cols);
  impl.zoom = letterbox.scale / (2.0f * kSx), impl.ox = letterbox.ox, impl.oy = letterbox.oy;
  impl.edge_x0 = 2.0f * letterbox.x0, impl.edge_w = 2.0f * letterbox.w;
  impl.spread = 2.0f * impl.layout_cols;
  // Resources and glyphs before begin() (they may upload / wait for the GPU).
  for (int character : impl.grid().roster()) impl.face(character);
  impl.face(0);
  impl.prep = true;
  impl.draw_screen();
  impl.prep = false;
  impl.sprites->begin(frame, vw, vh, resume);
  // The screen is cleared before the menu.
  impl.sprites->draw(impl.white, 0, 0, vw, vh, 0, 0, 1, 1, render::pack_rgba(0, 0, 0));
  impl.draw_screen();
  impl.sprites->end();
}

}  // namespace oc::ui
