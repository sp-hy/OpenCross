#include "ui/shop/shop_screen.h"
#include "render/camera.h"

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <utility>

#include "core/log.h"
#include "platform/config.h"
#include "platform/keymap.h"
#include "ui/lowres.h"
#include "ui/menu/menu_common.h"
#include "ui/menu/ps1_art.h"
#include "ui/ps1_font.h"

namespace oc::ui {

using namespace menudraw;

namespace {

const u32 kText = rgb(240, 240, 240), kGold = rgb(255, 214, 102);

struct Nav {
  bool up = false, down = false, left = false, right = false, ok = false, cancel = false;
  // ok_rep: Circle with auto-repeat (the first press, then every repeat while held).
  bool ok_rep = false;
};

// One drawing command of the frame: a rectangle, an image, or a text drawn through FontSet in a font slot at canvas (x, y).
struct Cmd {
  enum Kind : u8 { Rect, Img, Text } kind = Rect;
  render::TexRef set = {};
  float x = 0, y = 0, w = 0, h = 0, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  u32 color = kWhite;
  std::u32string text;
  float scale = 1;
  int slot = -1;
  font::Ink ink;
  int align = 0;
};

enum class Operation { None, Buy, SellElement, Forge, Break, SellGear };
enum class Stage { Top, Category, List, Quantity, Confirm };

}  // namespace

// ================================================================================================================
struct ShopScreen::Impl {
  int first_forged = 0;
  // ---- services
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch* text = nullptr;
  const data::GameTables* tables = nullptr;
  const MenuRules* rules = nullptr;
  import::Lang lang = import::Lang::En;
  import::LangTables lang_tables;
  // Font slots in the current art mode: the shop list's styles pick bn_70 (slot 4; JP kb_70 slot 0), kb_70 for numbers and ik_70
  // (slot 9) for JP style 7; large_ files in HD art, small_ in original art. Nothing draws the PS1 1-bit font.
  font::FontSet fonts;
  TexCache texs;
  Tex* skin = nullptr;
  Tex* cursor_tex = nullptr;
  // Classic art + [Video] Ps1ResMenus: the shop rendered at the original menu's display size.
  LowRes lowres;
  bool ready = false;
  std::function<void(int)> sfx_fn;
  shop::ShopData shop_data;

  // ---- state
  GameState* gs = nullptr;
  ArtMode mode = ArtMode::Hd;
  ShopKind kind = ShopKind::Element;
  int shop_id = 0;
  bool closed = true;
  Stage stage = Stage::Top;
  Operation operation = Operation::None;
  int top_cur = 0, cat_cur = 0, category = 0;
  // ids: list entries (item / element ids). conf_cur: 0 Yes, 1 No.
  std::vector<int> ids;
  int cur = 0, scroll = 0;
  int qty = 1, qty_max = 1;
  int conf_cur = 1;
  // Trading post: its own three-stage loop (greeting, goods list, Element offer) over the shop's trade.bin row. The goods and offer
  // lists prepare one row string per frame (`prepared`) and refuse input until every row is ready.
  struct Trade {
    // stage: 0 greeting, 1 goods list ("Exchange Rate"), 2 Element offer.
    // elements: offer list (192 entries, 0 = none); element_rows: listed offer rows (at least 11);
    // offered: offered copies per offer-list index.
    // offer_cur: offer cursor row 0..10, kept between visits; offer_top: offer list scroll.
    // scroll_dir: after a scroll 1 = up (prepare row 0), 2 = down (row 10), until the preparation ends.
    // yes_no: 0 Yes, 1 No; confirm_frames: frames since the confirmation opened (Circle needs more than 30).
    int stage = 0;
    shop::TradeRow row;
    std::vector<int> elements;
    int element_rows = 0;
    std::vector<int> offered;
    int goods_cur = 0;
    int offer_cur = 0;
    int offer_top = 0;
    int prepared = 0;
    int scroll_dir = 0;
    bool confirm = false;
    int yes_no = 0;
    int confirm_frames = 0;
  } trade;
  std::string msg;
  u32 msg_col = kGold;
  int msg_frames = 0;
  int frames = 0;
  // Compass needle of the element shop / smith top lists: eased once per frame toward the table angle of the selection (needle_tick).
  struct Needle {
    int angle = 0, settled = 0, step = 0;
    bool ticked = false;
  } needle;
  static constexpr int kNeedleAng[3] = {-960, -1100, -1248};
  NeedleTexture needle_tex;
  // Auto-repeat state for: up, down, left, right, Circle.
  DirectionRepeat<5> repeat;

  // ---- drawing
  float zoom = 1, ox = 0, oy = 0;
  std::vector<Cmd> cmds;

  // ===================================================== setup
  // The PS1-resolution pass runs in every art mode (ps1_menus()); the PS1 disc textures replace the HD files in classic art only.
  bool ps1_on() const { return mode == ArtMode::Original && ps1_menus(); }
  Tex* ps1_tile() {
    return texs.ps1("ps1:tile", [&] { return ps1::parchment(*vfs); });
  }

  bool init(render::GpuContext& context, const vfs::Vfs& files, render::SpriteBatch& sprite_batch, render::TextBatch& text_batch,
            const data::GameTables& game_tables, import::Lang language) {
    ctx = &context;
    vfs = &files;
    sprites = &sprite_batch;
    text = &text_batch;
    tables = &game_tables;
    lang = language;
    lang_tables.init(&files);
    shop_data = shop::ShopData::from_tables(game_tables);
    if (!shop_data.load_waa_shop(files)) log::warn("shop: waa_shop.bin missing; using the table loader rows");
    if (!texs.init(context, files, sprite_batch, "shop")) return false;
    lowres.init(context, sprite_batch);
    fonts.init(files, text_batch);
    fonts.set_classic(mode == ArtMode::Original);
    skin = texs.load("hd/window/w01.png");
    cursor_tex = texs.load("hd/menu/menutest_cur.png");
    ready = fonts.face(font::kBn, false) != render::kNoFace;
    if (!ready) log::error("shop: no usable font");
    return ready;
  }

  void shutdown() {
    texs.shutdown();
    skin = cursor_tex = nullptr;
    lowres.shutdown();
    ctx = nullptr;
  }

  // ===================================================== names
  std::u32string item_name(int id) {
    if (id <= 0) return utf32("-");
    std::u32string name = lang_tables.item_name(id, lang);
    return name.empty() ? utf32(tables->item_name(id)) : name;
  }
  std::u32string element_name(int id) {
    if (const std::string* name = lang_tables.sysmes((0x17u << 16) | static_cast<u32>(id), lang); name && !name->empty()) return utf32(*name);
    return utf32(tables->element_name(id));
  }
  void sfx(int id) {
    if (sfx_fn) sfx_fn(id);
  }
  void say(const std::string& message_text, u32 colour = kGold, int hold_frames = 150) {
    msg = message_text;
    msg_col = colour;
    msg_frames = hold_frames;
  }

  // ===================================================== input
  Nav poll(const platform::Input& in) {
    Nav nav;
    using platform::Action;
    auto key_down = [&](Action action) { return platform::action_down(in, action); };
    auto key_pressed = [&](Action action) { return platform::action_pressed(in, action); };
    auto pad_down = [&](int button) { return in.pad_down[static_cast<size_t>(button)]; };
    auto pad_pressed = [&](int button) { return in.pad_pressed[static_cast<size_t>(button)]; };
    const bool down[5] = {key_down(Action::Up) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_UP),
                          key_down(Action::Down) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                          key_down(Action::Left) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_LEFT),
                          key_down(Action::Right) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT),
                          key_down(Action::Ok) || pad_down(SDL_GAMEPAD_BUTTON_SOUTH)};
    bool fire[5]{};
    repeat.poll(down, fire);
    nav.up = fire[0], nav.down = fire[1], nav.left = fire[2], nav.right = fire[3];
    nav.ok = key_pressed(Action::Ok) || pad_pressed(SDL_GAMEPAD_BUTTON_SOUTH);
    nav.cancel = key_pressed(Action::Cancel) || pad_pressed(SDL_GAMEPAD_BUTTON_EAST);
    nav.ok_rep = nav.ok || fire[4];
    return nav;
  }

  // ===================================================== flow
  void open(GameState& game_state, ShopKind shop_kind, int id) {
    gs = &game_state;
    kind = shop_kind;
    shop_id = id;
    closed = false;
    stage = Stage::Top;
    operation = Operation::None;
    top_cur = cat_cur = category = 0;
    needle = Needle{};
    needle_tex.reset();
    msg.clear();
    msg_frames = 0;
    frames = 0;
    repeat.reset();
    if (kind == ShopKind::Trading) trade_open();
  }

  // Rebuilds `ids` for the current operation; the Cheats tab can widen the buy and forge lists to every element / piece of gear.
  void build_list() {
    ids.clear();
    switch (operation) {
      case Operation::None: break;
      case Operation::Buy:
        ids = shop_data.element_row(shop_id);
        if (platform::cheats().shop_all_elements) ids = shop::element_row_all(*gs, std::move(ids));
        break;
      case Operation::SellElement: ids = shop::element_sell_list(*gs); break;
      case Operation::Forge:
        ids = platform::cheats().shop_all_weapons ? shop::forge_list_all(*gs, shop_data, shop_id, category) : shop::forge_list(*gs, shop_data, shop_id, category);
        break;
      case Operation::Break: ids = shop::breakdown_list(*gs, category); break;
      case Operation::SellGear: ids = shop::gear_sell_list(*gs, category); break;
    }
    cur = std::min(cur, std::max(0, list_size() - 1));
    fix_scroll();
  }
  int list_size() const { return static_cast<int>(ids.size()); }
  void fix_scroll() {
    if (cur < scroll) scroll = cur;
    if (cur >= scroll + kVisibleRows) scroll = cur - kVisibleRows + 1;
    scroll = std::max(0, std::min(scroll, std::max(0, list_size() - kVisibleRows)));
  }
  void move_cursor(int& cursor, int count, int step) {
    if (count <= 0) return;
    cursor = (cursor + step + count) % count;
    sfx(1);
  }

  // Needle easing of the retail shops: a settled selection sits at its exact table angle; while the selection differs from the settled
  // one, angle += ((target - angle) / 10) * step (integer division) with step counting 0..10 once per frame, so the needle covers
  // 10 %, 20 % ... of the remaining way and lands exactly on the 11th frame, when the selection settles.
  void needle_tick(int sel) {
    sel = std::clamp(sel, 0, 2);
    const int target = kNeedleAng[sel];
    needle.ticked = true;
    if (sel == needle.settled) {
      needle.angle = target;
      return;
    }
    needle.angle += ((target - needle.angle) / 10) * needle.step;
    if (needle.step < 10) ++needle.step;
    else needle.step = 0, needle.settled = sel;
  }

  // `ticks` = 60 Hz sim ticks: the retail shop loop runs once per vsync, so the animations count ticks, not rendered frames.
  void update(const platform::Input& in, int ticks) {
    if (closed || !gs || !ready) return;
    frames += std::max(ticks, 0);
    if (msg_frames > 0 && --msg_frames == 0) msg.clear();
    const Nav nav = poll(in);
    if (kind == ShopKind::Trading) return trade_update(nav);
    if (stage == Stage::Top || stage == Stage::Category) needle_tick(stage == Stage::Category && kind == ShopKind::Smith ? cat_cur : top_cur);
    switch (stage) {
      case Stage::Top: update_top(nav); break;
      case Stage::Category: update_category(nav); break;
      case Stage::List: update_list(nav); break;
      case Stage::Quantity: update_quantity(nav); break;
      case Stage::Confirm: update_confirm(nav); break;
    }
  }

  int top_count() const { return kind == ShopKind::Element ? 2 : 3; }

  void update_top(const Nav& nav) {
    if (nav.up) move_cursor(top_cur, top_count(), -1);
    if (nav.down) move_cursor(top_cur, top_count(), 1);
    if (nav.cancel) {
      sfx(6);
      closed = true;
    } else if (nav.ok) {
      sfx(8);
      cur = scroll = 0;
      if (kind == ShopKind::Element) {
        operation = top_cur == 0 ? Operation::Buy : Operation::SellElement;
        build_list();
        if (ids.empty()) return deny("");
        stage = Stage::List;
      } else {
        operation = top_cur == 0 ? Operation::Forge : top_cur == 1 ? Operation::Break : Operation::SellGear;
        stage = Stage::Category;
      }
    }
  }

  void update_category(const Nav& nav) {
    if (nav.up) move_cursor(cat_cur, 3, -1);
    if (nav.down) move_cursor(cat_cur, 3, 1);
    if (nav.cancel) {
      sfx(6);
      stage = Stage::Top;
      operation = Operation::None;
    } else if (nav.ok) {
      category = cat_cur;
      cur = scroll = 0;
      build_list();
      if (ids.empty()) {
        sfx(0x18);
        return deny("");
      }
      sfx(8);
      stage = Stage::List;
    }
  }

  void update_list(const Nav& nav) {
    const int count = list_size();
    if (nav.up) move_cursor(cur, count, -1), fix_scroll();
    if (nav.down) move_cursor(cur, count, 1), fix_scroll();
    if (nav.cancel) {
      sfx(6);
      msg.clear();
      stage = kind == ShopKind::Smith ? Stage::Category : Stage::Top;
      return;
    }
    if (!nav.ok || count == 0) return;
    const int id = ids[static_cast<size_t>(cur)];
    switch (operation) {
      case Operation::None: break;
      case Operation::Buy:
        if (static_cast<u32>(shop::element_price(*gs, id)) > gs->gold()) return deny("");
        qty_max = shop::element_buy_max(*gs, id);
        if (qty_max < 1) return deny("");
        start_quantity();
        break;
      case Operation::SellElement:
        qty_max = shop::element_sell_max(*gs, id);
        start_quantity();
        break;
      case Operation::Forge: {
        const shop::Res result = shop::can_forge(*gs, id, platform::cheats().no_craft_cost);
        if (result == shop::Res::ShortMaterials) return deny(ss(0x801DF8FDu + static_cast<u32>(std::clamp(category, 0, 2))));
        if (result == shop::Res::NoGold) return deny(ss(0x801DF914));
        if (result != shop::Res::Ok) return deny("");
        start_confirm();
        break;
      }
      case Operation::Break:
        if (gs->item_count(id) < 1) return deny(ss(0x801DF985));
        start_confirm();
        break;
      case Operation::SellGear:
        qty_max = gs->item_count(id);
        start_quantity();
        break;
    }
  }
  void deny(const std::string& message_text) {
    sfx(0x18);
    if (!message_text.empty()) say(message_text, kText);
  }
  void start_quantity() {
    sfx(8);
    qty = 1;
    msg.clear();
    stage = Stage::Quantity;
  }
  void start_confirm() {
    sfx(8);
    conf_cur = 1;
    msg.clear();
    stage = Stage::Confirm;
  }

  void update_quantity(const Nav& nav) {
    if (nav.right) qty = std::min(qty_max, qty + 1), sfx(1);
    if (nav.left) qty = std::max(1, qty - 1), sfx(1);
    if (nav.up) qty = std::min(qty_max, qty + 10), sfx(1);
    if (nav.down) qty = std::max(1, qty - 10), sfx(1);
    if (nav.cancel) {
      sfx(6);
      stage = Stage::List;
      return;
    }
    if (!nav.ok) return;
    const int id = ids[static_cast<size_t>(cur)];
    shop::Res result = shop::Res::NoStock;
    if (operation == Operation::Buy) result = shop::buy_element(*gs, id, qty);
    else if (operation == Operation::SellElement) result = shop::sell_element(*gs, id, qty);
    else if (operation == Operation::SellGear) result = shop::sell_gear(*gs, id, qty);
    if (result != shop::Res::Ok) {
      stage = Stage::List;
      return deny(result_text(result));
    }
    sfx(5);
    say(ss(0x801DF4A4), kText);
    after_change();
  }

  // The stock changed: rebuild the list and fall back to the menu above when nothing is left.
  void after_change() {
    build_list();
    stage = Stage::List;
    if (ids.empty()) {
      stage = kind == ShopKind::Smith ? Stage::Category : Stage::Top;
      cur = scroll = 0;
    }
  }

  std::string result_text(shop::Res result) {
    return result == shop::Res::NoGold ? ss(0x801DF914) : result == shop::Res::Equipped ? ss(0x801DF985) : std::string();
  }

  void update_confirm(const Nav& nav) {
    if (nav.left || nav.right || nav.up || nav.down) conf_cur ^= 1, sfx(1);
    if (nav.cancel || (nav.ok && conf_cur == 1)) {
      sfx(6);
      stage = Stage::List;
      return;
    }
    if (!nav.ok) return;
    const int id = ids[static_cast<size_t>(cur)];
    shop::Res result = shop::Res::NoRecipe;
    int gain = 0;
    const bool first_copy = gs->item_count(id) == 0;
    if (operation == Operation::Forge) result = shop::forge(*gs, id, platform::cheats().no_craft_cost);
    else if (operation == Operation::Break) result = shop::breakdown(*gs, id, &gain);
    if (result != shop::Res::Ok) {
      stage = Stage::List;
      return deny(result_text(result));
    }
    if (operation == Operation::Forge && first_copy) first_forged = id;
    sfx(0xBF);
    say(operation == Operation::Forge ? ss(0x801DF92D) : gain > 0 ? ss(0x801DF92E) + "\n" + ss(0x801DF99C) : ss(0x801DF92E), kText);
    after_change();
  }

  // ===================================================== draw primitives
  // The shop overlay draws in a 512 x 432 unit space mapped onto the 1280 x 960 canvas: canvas X = 2.5 x, Y = 960 / 432 y. The frames
  // are hd/menu/shapes/menu-main-NNN (image = 4x the unit size; NNN = the PS1 sheet frame in decimal).
  //   rect / Img              canvas-px rectangle / image quad                SX / SY        unit -> canvas px
  //   Frame(frame, x, y)      panel shape with its top-left at unit (x, y)    RT(style, ...) shop-list text entry at unit (x, y)
  //   ss(key)                 shopmenu.txt row by its string key              cm(key)        cmnmes.txt row by its JP key
  //   fmt(format, value)      one-number snprintf                             full_width     JP full-width digits
  void rect(float x, float y, float w, float h, u32 colour) {
    Cmd cmd;
    cmd.kind = Cmd::Rect;
    cmd.set = texs.white_set();
    cmd.x = x, cmd.y = y, cmd.w = w, cmd.h = h, cmd.color = colour;
    cmds.push_back(std::move(cmd));
  }
  void Img(const Tex* tex, float x, float y, float w, float h, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1, u32 colour = kWhite) {
    if (!tex || !tex->set) return;
    Cmd cmd;
    cmd.kind = Cmd::Img;
    cmd.set = tex->set;
    cmd.x = x, cmd.y = y, cmd.w = w, cmd.h = h, cmd.u0 = u0, cmd.v0 = v0, cmd.u1 = u1, cmd.v1 = v1, cmd.color = colour;
    cmds.push_back(std::move(cmd));
  }
  // Fallback frame when a menu-main shape is missing: a 3 x 3 grid of the w01 window skin around a dark fill.
  void Panel(float x, float y, float w, float h) {
    if (skin && skin->set) {
      rect(x + 10, y + 10, w - 20, h - 20, rgb(6, 9, 34, 238));
      const float sheet_x0 = 224, sheet_y0 = 75, sheet_x1 = 416, sheet_y1 = 245, sheet_corner = 28, corner = 26;
      const float xs[4] = {x, x + corner, x + w - corner, x + w}, ys[4] = {y, y + corner, y + h - corner, y + h};
      const float us[4] = {sheet_x0, sheet_x0 + sheet_corner, sheet_x1 - sheet_corner, sheet_x1};
      const float vs[4] = {sheet_y0, sheet_y0 + sheet_corner, sheet_y1 - sheet_corner, sheet_y1};
      for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
          Img(skin, xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], us[i] / skin->w, vs[j] / skin->h, us[i + 1] / skin->w, vs[j + 1] / skin->h);
    } else {
      rect(x, y, w, h, rgb(6, 9, 34, 238));
    }
  }

  static float SX(float x) { return x * 2.5f; }
  static float SY(float y) { return y * (960.0f / 432.0f); }
  // PS1 colour byte (0x80 = 1.0) -> modulation grey.
  static u32 grey(int value) {
    const int channel = std::min(255, value * 2);
    return rgb(channel, channel, channel);
  }
  // Panel frame with its top-left corner at unit (x, y), in both art modes. Classic art with the PS1 disc textures builds the frame from
  // the disc's "wind" sheet pieces (the shape only supplies the panel size and its caption notch); the interior stays transparent so
  // the parchment shows through.
  void Frame(int frame, float x, float y, u32 colour = kWhite, float* out_w = nullptr, float* out_h = nullptr) {
    Tex* tex = texs.shape(frame, lang);
    const float w = tex ? tex->w / 4.0f : 100, h = tex ? tex->h / 4.0f : 50;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (tex) {
      if (Tex* wind = ps1_on() ? texs.ps1("ps1:wind", [&] { return ps1::menutim(*vfs, "wind"); }) : nullptr) {
        const float gap = ps1::caption_gap_end(frame);
        ps1::frame_pieces(
            [&](float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh) {
              Img(wind, dx, dy, dw, dh, sx / wind->w, sy / wind->h, (sx + sw) / wind->w, (sy + sh) / wind->h, colour);
            },
            SX(x), SY(y), SX(w), SY(h), SX(1), SY(1), gap > 0 ? SX(8) : 0.0f, gap > 0 ? SX(gap - 8) : 0.0f);
        return;
      }
      Img(tex, SX(x), SY(y), SX(w), SY(h), 0, 0, 1, 1, colour);
    } else {
      Panel(SX(x), SY(y), SX(w), SY(h));
    }
  }
  std::string cm(std::string_view key) {
    if (const import::TextTable* table = lang_tables.table("cmnmes"))
      if (const std::string* row = table->get(key, lang); row && !row->empty()) return *row;
    return std::string(key);
  }
  std::string ss(u32 addr) {
    if (const import::TextTable* table = lang_tables.table("shopmenu"))
      if (const std::string* row = table->get(static_cast<u64>(addr), lang)) return *row;
    return {};
  }
  // ---- shop text. Every string is an entry of the shop list: {x, y in the 512 x 432 list space, style, text, PS1 colour
  // 0x00BBGGRR}. Default slot 4 bn_70 (JP: 0 kb_70), multiplier 0.7; flag 0x10000 forces the JP slot, 0x20000 the non-JP one.
  // X = x * 2.5 + offX * scale, Y = int(y + dy) * 960 / 432 + offY * scale (offsets at the scale before any shrink), scale = slot scale
  // * multiplier. Colour helpers by mode: Lines = H1(c, 20, 6), one draw per line at a 16 * sy pitch; H3(c, 0x6319, 3); H4(c, 0x6319, 1);
  // Base (style 0x80) = shrink to 97 units with H1(c, 20, 9); Centre (0x81) = centred, scale * min(1, 136 * 2.5 / w), 0x909090 ->
  // 0x808080; Right (0x82). `align_override` >= 0 replaces the style's alignment (only where OpenCross keeps its own geometry).
  // Language index, 5 = JP.
  int lang_index() const { return static_cast<int>(lang); }
  void RT(int style, float x, float y, const std::u32string& str, u32 col = 0x808080, int align_override = -1) {
    if (str.empty()) return;
    const int lang_i = std::clamp(lang_index(), 0, 5);
    const bool jp = lang_i == 5;
    // Effective language: flag 0x10000 forces JP, 0x20000 non-JP; it picks the default slot, style 7's slot and the baseline-shrink
    // constant. Width rules and per-language tables use the global language.
    const bool jp_effective = (style & 0x10000) ? true : (style & 0x20000) ? false : jp;
    int slot = jp_effective ? font::kKb : font::kBn;
    const int style_id = style & 0xFF;
    float mul = 0.7f, dy = 0;
    enum class Mode { Lines, H3, H4, Base, Centre, Right } colour_mode = Mode::Lines;
    auto per_language = [&](const float (&muls)[6], const float (&dys)[6]) { mul = muls[lang_i], dy = dys[lang_i]; };
    // trunc(width in the JP ik / bn slot at 0.35 / 2.5): the width rules of the shrinking styles.
    auto width_units = [&]() {
      const int width_slot = jp ? font::kIk : font::kBn;
      fonts.prepare(width_slot, str);
      return static_cast<float>(static_cast<int>(fonts.measure(width_slot, str, 0.35f) / 2.5f));
    };
    switch (style_id) {
      // JP numbers
      case 4: mul = 0.625f, colour_mode = Mode::H3; break;
      // small labels ("Lv", "G", "/")
      case 5: mul = 0.55f, colour_mode = Mode::H3; break;
      case 6: mul = 0.6f, colour_mode = Mode::H3; break;
      // compass labels
      case 7: slot = jp_effective ? font::kIk : font::kBn, colour_mode = Mode::H3; break;
      // offer-list "-"
      case 8: mul = 0.625f, colour_mode = Mode::H4; break;
      // offer-list "x"
      case 9: mul = 0.55f, colour_mode = Mode::H4; break;
      // forge confirmation prompt
      case 0x71: {
        static const float kDy[6] = {0, -2, -20, -2, 0, 0};
        dy = kDy[lang_i];
        break;
      }
      // break-down confirmation prompt
      case 0x72: {
        static const float kDy[6] = {0, -16, -16, -16, 0, 0};
        dy = kDy[lang_i];
        break;
      }
      case 0x75: mul = lang_i == 3 ? 0.64f : 0.7f, dy = lang_i == 3 ? 1.0f : 0.0f; break;
      // element target label
      case 0x79: per_language({.61f, .58f, .45f, .53f, .61f, .7f}, {4, 4, 6, 5, 4, 2}); break;
      // innate-only note
      case 0x7A: per_language({.67f, .58f, .58f, .62f, .67f, .7f}, {3, 5, 5, 4, 4, 2}); break;
      // element description
      case 0x7B: case 0xBA: per_language({.67f, .57f, .51f, .65f, .53f, .7f}, {3, 3, 4, 3, 5, 2}); break;
      // total amount
      case 0x7C: slot = font::kKb, mul = 0.625f, colour_mode = Mode::H3; break;
      case 0x80: colour_mode = Mode::Base; break;
      // list names centred on the plate
      case 0x81: colour_mode = Mode::Centre; break;
      // goods names right-aligned
      case 0x82: colour_mode = Mode::Right; break;
      // gear description
      case 0x8F: per_language({.7f, .65f, .68f, .68f, .69f, .7f}, {-12, -10, -11, -11, -12, -10}); break;
      // kb numbers and "/"
      case 0x90: case 0xB0: slot = font::kKb, mul = 0.55f, colour_mode = Mode::H3; break;
      // kb prices / counts
      case 0x91: slot = font::kKb, mul = 0.625f, colour_mode = Mode::H3; break;
      // trading post numbers
      case 0x98: slot = font::kKb; break;
      // offer-list digits
      case 0x99: slot = font::kKb, mul = 0.55f, dy = 1, colour_mode = Mode::H4; break;
      // smith list names: shrink by width
      case 0xAE: {
        const float width = width_units();
        mul = width <= 119 ? 0.7f : width <= 161 ? 0.7f - 0.005f * (width - 119) : 0.7f - 0.00414f * (width - 119);
        break;
      }
      // material names: shrink by width, lowered when shrunk
      case 0xAF: {
        const float width = width_units();
        mul = width <= 75 ? 0.7f : width <= 107 ? 0.7f - 0.0065f * (width - 75) : 0.7f - 0.0055f * (width - 75);
        dy = jp ? 0.0f : width > 107 ? 3.0f : width > 75 ? 2.0f : 0.0f;
        break;
      }
      case 0xB1: slot = font::kKb, mul = 0.55f, dy = 2, colour_mode = Mode::H3; break;
      // trading post colour condition: shrink by width
      case 0xB5: {
        const float width = width_units();
        mul = width <= 152 ? 0.7f : 0.7f - 0.005f * (width - 152);
        break;
      }
      default: break;
    }
    if (colour_mode == Mode::Centre && (col & 0xFFFFFF) == 0x909090) col = 0x808080;
    const font::SlotInfo& slot_info = font::slot(slot);
    float scale = slot_info.scale * mul;
    const float sy = 960.0f / 432.0f;
    const float canvas_x = x * 2.5f + slot_info.off_x * scale;
    float canvas_y = std::floor(y + dy) * sy + slot_info.off_y * scale;
    fonts.prepare(slot, str);
    font::Ink ink = font::h1(col, 20, 6);
    int align = font::kLeft;
    switch (colour_mode) {
      case Mode::Lines: break;
      case Mode::H3: ink = font::h3(col, 0x6319, 3); break;
      case Mode::H4: ink = font::h4(col, 0x6319, 1); break;
      case Mode::Base: {
        ink = font::h1(col, 20, 9);
        const float width = fonts.measure(slot, str, scale) / 2.5f, drop = jp_effective ? 32.0f : 28.0f;
        if (width > 97.0f) {
          scale *= 97.0f / width;
          canvas_y += drop - drop * 97.0f / width;
        }
        break;
      }
      case Mode::Centre: {
        const float width = fonts.measure(slot, str, scale);
        if (width > 0) scale *= std::min(1.0f, 136.0f * 2.5f / width);
        align = font::kCentre;
        break;
      }
      case Mode::Right: align = font::kRight; break;
    }
    if (align_override >= 0) align = align_override;
    auto push = [&](const std::u32string& line_text, float line_y) {
      Cmd cmd;
      cmd.kind = Cmd::Text;
      cmd.text = line_text;
      cmd.slot = slot;
      cmd.ink = ink;
      cmd.align = align;
      cmd.scale = scale;
      cmd.x = canvas_x, cmd.y = line_y;
      cmds.push_back(std::move(cmd));
    };
    if (colour_mode != Mode::Lines) {
      push(str, canvas_y);
      return;
    }
    size_t start = 0;
    for (int line = 0;; ++line) {
      const size_t newline = str.find(U'\n', start);
      std::u32string line_text = str.substr(start, newline == std::u32string::npos ? std::u32string::npos : newline - start);
      std::erase(line_text, U'\r');
      if (!line_text.empty()) push(line_text, canvas_y + 16.0f * sy * static_cast<float>(line));
      if (newline == std::u32string::npos) break;
      start = newline + 1;
    }
  }
  void RT(int style, float x, float y, const std::string& str, u32 col = 0x808080, int align_override = -1) {
    RT(style, x, y, utf32(str), col, align_override);
  }
  // Digit count of the magnitude, at least 1.
  static int digit_count(long long value) {
    int count = 1;
    for (value = value < 0 ? -value : value; value >= 10; value /= 10) ++count;
    return count;
  }
  static std::string fmt(const char* format, int value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, format, value);
    return buf;
  }
  // JP: every printable ASCII character, pad spaces included (0x20 -> U+3000), becomes its full-width form.
  std::u32string full_width(const std::string& str) const {
    std::u32string wide = utf32(str);
    if (lang_index() == 5)
      for (char32_t& ch : wide)
        if (ch == U' ') ch = 0x3000;
        else if (ch == U'"') ch = 0x201D;
        else if (ch == U'\'') ch = 0x2019;
        else if (ch == U'\\') ch = 0xFFE5;
        else if (ch == U'`') ch = 0x2018;
        else if (ch > U' ' && ch < 0x7F) ch += 0xFEE0;
    return wide;
  }
  // The cursor triangle (23 x 11 units): the cursor sprite in HD art, the PS1's flat red triangle with the disc textures; pulsing while
  // `pulse` (choosing), flat dark red otherwise.
  void marker(float x, float y, bool pulse) {
    const u32 colour = cursor_sprite_colour(pulse, frames);
    if (!ps1_on() && cursor_tex && cursor_tex->set) Img(cursor_tex, SX(x), SY(y), SX(23), SY(11), 0, 0, 1, 1, colour);
    else
      for (int i = 0; i < 12; ++i) {
        const float width = 1.0f - std::fabs(static_cast<float>(i) + 0.5f - 6.0f) / 6.0f;
        rect(SX(x), SY(y + static_cast<float>(i)), SX(23.0f * width), SY(1) + 0.6f, colour);
      }
  }
  // Scroll arrow of the "roll" sheet, 9 x 5 units, bobbing 0..4 units away from the list every 4 ticks.
  void scroll_arrow(float x, float y, bool up) {
    const float bob = static_cast<float>((frames / 4) % 5);
    const float arrow_y = up ? y - bob : y + bob;
    if (Tex* tex = texs.roll(ps1_on())) Img(tex, SX(x), SY(arrow_y), SX(9), SY(5), 52.0f / 96, up ? 0.5f : 0.0f, 88.0f / 96, up ? 0.0f : 0.5f);
  }
  // Colour TIM of the element plate per colour index 0..5 (green, white, black, red, yellow, blue).
  static constexpr int kPlateTim[6] = {9, 12, 10, 11, 13, 8};
  // Row plate (184 x 24 units) behind an element name, in the element's lowest colour bit.
  void plate(int id, float x, float y) {
    if (!rules) return;
    const int colour_bits = rules->el_colour(id);
    int colour_slot = 0;
    for (int bit = 7; bit >= 2; --bit)
      if (colour_bits & (1 << bit)) colour_slot = bit - 2;
    if (Tex* tex = texs.plate(kPlateTim[colour_slot], ps1_on())) Img(tex, SX(x), SY(y), SX(184), SY(24), 0, 0, 1, 1, grey(0x70));
  }
  // The dial needle (handle.png turned by `angle`, 4096 = 2 pi) centred on the pivot at unit (pivot_x, pivot_y); see NeedleTexture.
  void Hand(float pivot_x, float pivot_y, int angle) {
    const NeedleTexture::Quad quad = needle_tex.get(texs, *vfs, angle, 2.5f, 960.0f / 432.0f);
    if (!quad.tex) {
      log::warn("shop: hd/menu/handle.png not available");
      return;
    }
    Img(quad.tex, SX(pivot_x) - quad.half_w, SY(pivot_y) - quad.half_h, 2 * quad.half_w, 2 * quad.half_h, 0, 0, 1, 1, rgb(175, 172, 172));
  }

  // Caption and icon in the top notch of a list panel at unit (x, y): the weapon / armour / accessory frames 0x9A..0x9C and 0xA0..0xA2
  // use EquipIcon cells 0..2 with their own caption offsets, every other frame the Elements cell 3. Classic art with the disc textures
  // draws the frame's own header part from the menutim "icon" sheet at its part offset instead of the EquipIcon cell.
  void panel_caption(int frame, float x, float y) {
    int cell = 3;
    float caption_x = 35;
    const char* key = "Elements";
    if (frame >= 0x9A && frame <= 0x9C) cell = frame - 0x9A;
    else if (frame >= 0xA0 && frame <= 0xA2) cell = frame - 0xA0;
    if (cell < 3) {
      static const float kCaptionX[3] = {32, 36, 33};
      static const char* const kKey[3] = {"Weapon", "Armor", "Accessories"};
      caption_x = kCaptionX[cell];
      key = kKey[cell];
    }
    if (Tex* icon_sheet = ps1_on() ? texs.ps1("ps1:icon", [&] { return ps1::menutim(*vfs, "icon"); }) : nullptr) {
      const ps1::IconCell& part = ps1::kEquipIcon[cell];
      Img(icon_sheet, SX(x + static_cast<float>(part.dx)), SY(y + static_cast<float>(part.dy)), SX(static_cast<float>(part.w)), SY(static_cast<float>(part.h)),
          static_cast<float>(part.u) / icon_sheet->w, static_cast<float>(part.v) / icon_sheet->h, static_cast<float>(part.u + part.w) / icon_sheet->w,
          static_cast<float>(part.v + part.h) / icon_sheet->h);
    } else if (Tex* icons = texs.load("hd/menu/menutim/EquipIcon.png")) {
      Img(icons, SX(x + 10), SY(y - 7), SX(26), SY(32), 0.0075f + 0.2f * static_cast<float>(cell), 0.0075f,
          0.0075f + 0.2f * static_cast<float>(cell) + 0.185f, 0.0075f + 0.985f);
    }
    RT(0x20006, x + caption_x, y - 2, cm(key));
  }
  // Row plate by the trading post's colour index 1..6 (0 = none), modulated by the low byte of `colour`.
  void plate_indexed(int colour_index, float x, float y, u32 colour) {
    if (colour_index < 1 || colour_index > 6) return;
    if (Tex* tex = texs.plate(kPlateTim[colour_index - 1], ps1_on())) Img(tex, SX(x), SY(y), SX(184), SY(24), 0, 0, 1, 1, grey(static_cast<int>(colour & 0xFF)));
  }

  // Parchment: 8 x 7 tiles of hd/map/etc/tile.tim (the whole 256 x 256 image is one tile) in both art modes, the disc tile with the
  // PS1 textures or when the HD file is missing; then the two faint compass swirls.
  void backdrop() {
    Tex* tile = ps1_on() ? ps1_tile() : texs.load("hd/map/etc/tile.tim");
    if (!tile) tile = ps1_tile();
    if (tile) {
      const float tile_w = kCanvasW / 8.0f, tile_h = kCanvasH / 7.0f;
      for (int ty = 0; ty < 7; ++ty)
        for (int tx = 0; tx < 8; ++tx) Img(tile, tile_w * static_cast<float>(tx), tile_h * static_cast<float>(ty), tile_w + 1.0f, tile_h + 1.0f, 0, 0, 1, 1);
    } else {
      rect(0, 0, kCanvasW, kCanvasH, rgb(60, 38, 18));
    }
    if (Tex* deco = texs.load("hd/menu/menutim/menutim_02.png")) {
      Img(deco, SX(-56), SY(242), SX(256), SY(298), 0, 0, 1, 1, rgb(150, 150, 170));
      Img(deco, SX(115), SY(-135), SX(204), SY(238), 0, 0, 1, 1, rgb(150, 150, 170));
    }
  }
  // Compass dial of the top lists: the deco disc at (382, 293) 128 x 149, the needle on the pivot (446, 367) aimed at the selected
  // label, the labels (2 for the element shop, 3 for the smith) with leader lines like the main menu's list entries 2..4: along the
  // label, a 12-unit diagonal down-right, then on toward the needle tip.
  void compass(int sel, const std::vector<std::string>& labels, bool bright) {
    static const float kPos[2][3][2] = {{{301, 333}, {292, 349}, {0, 0}}, {{299, 332}, {289, 349}, {279, 365}}};
    const bool two_labels = labels.size() == 2;
    if (Tex* deco = texs.load("hd/menu/menutim/menutim_02.png")) Img(deco, SX(382), SY(293), SX(128), SY(149), 0, 0, 1, 1, bright ? rgb(200, 200, 215) : rgb(100, 100, 108));
    // Without update() (screenshots) the needle sits at the exact table angle.
    const int angle = needle.ticked ? needle.angle : kNeedleAng[std::clamp(sel, 0, 2)];
    Hand(446, 367, angle);
    if (std::getenv("OC_SHOP_LOG"))
      log::info("shop compass: sel {} angle {} theta {:.2f} deg tip dir (canvas) {:.1f} deg", sel, angle, (angle + 1024) * 180.0 / 2048.0,
                std::atan2(-std::sin((angle + 1024) * 3.14159265 / 2048.0) * 960.0 / 432.0, -std::cos((angle + 1024) * 3.14159265 / 2048.0) * 2.5) * 180.0 / 3.14159265);
    const u32 line_colour = rgb(200, 190, 190);
    const float thickness = 2.2f;
    for (size_t i = 0; i < labels.size(); ++i) {
      const int entry = 2 + static_cast<int>(i);
      const float line_x = kPos[two_labels ? 0 : 1][i][0] - 2.8f, line_y = kPos[two_labels ? 0 : 1][i][1] + 12.4f;
      const float bend_x = 383.0f - 12.4f * static_cast<float>(entry), end_x = 387.5f + 1.4f * static_cast<float>(entry - 1);
      rect(SX(line_x), SY(line_y) - thickness * 0.5f, SX(bend_x - line_x), thickness, line_colour);
      for (int step = 0; step <= 24; ++step) {
        const float along = static_cast<float>(step) / 24.0f;
        rect(SX(bend_x + 12.0f * along) - thickness * 0.5f, SY(line_y + 12.0f * along) - thickness * 0.5f, thickness, thickness, line_colour);
      }
      rect(SX(bend_x + 12.0f), SY(line_y + 12.0f) - thickness * 0.5f, SX(end_x - bend_x - 12.0f), thickness, line_colour);
    }
    for (size_t i = 0; i < labels.size(); ++i)
      RT(7, kPos[two_labels ? 0 : 1][i][0], kPos[two_labels ? 0 : 1][i][1], labels[i], static_cast<int>(i) == sel ? 0x004E77u : 0x808080u);
  }
  // Footer: star count (star glyph at x, y + 2), "%2d" counts, PLAY label + time, gold + unit.
  void footer() {
    const bool jp = lang_index() == 5;
    RT(0x10004, 0x1E, 0x186 + 2, U"\u2605");    RT(jp ? 4 : 0x91, 0x2C, 0x188, padded_number(std::min<int>(gs->slot.stars_current(), 99), 2));
    RT(jp ? 5 : 0xB0, 0x42, 0x188, U"/");    RT(jp ? 4 : 0x91, 0x4E, 0x188, padded_number(std::min<int>(gs->slot.stars_total(), 99), 2));
    RT(5, 0x74 - (lang_index() == 4 ? 9.0f : 0.0f), 0x17E, cm("PLAY"));
    const u32 sec = gs->play_seconds();
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "%03u:%02u:%02u", sec / 3600, sec / 60 % 60, sec % 60);
    RT(jp ? 5 : 0x90, 0x94, 0x17E, buffer);
    // Gold x offset by digit count.
    static const int kX7C[9] = {37, 34, 30, 27, 23, 19, 16, 12, 0};
    RT(jp ? 5 : 0x90, 0x7C + static_cast<float>(kX7C[std::min(digit_count(gs->gold()), 9) - 1]), 400, grouped_gold(gs->gold()));
    RT(5, jp ? 0xD4 - 10.0f : 0xD4 - 8.0f, jp ? 400.0f : 401.0f, U"G");
  }
  std::string table_row(std::string_view table, int key) {
    if (const import::TextTable* rows = lang_tables.table(table))
      if (const std::string* row = rows->get(static_cast<u64>(key), lang)) return *row;
    return {};
  }
  // The 3-row typed message on the backdrop at (336, 270 + 16 k).
  // `ov` is a transient result text (thanks / refusal) that replaces the standing prompt.
  std::string ov;
  // Prompt strip at (0x150, 0x10E), one multi-line entry, style 1 (0x71 / 0x72: the smith confirm prompts).
  void message(const std::string& prompt, int style = 1) { RT(style, 0x150, 0x10E, ov.empty() ? prompt : ov); }
  void banner(const std::string& title_text) {
    Frame(119, 276, 132);
    // Per-language x offset (dx0) of the title string for the element shop, trading post and blacksmith.
    static const int kEl[6] = {0x1D, 4, 7, 0x1A, 0, 0x12}, kTr[6] = {0x20, 6, 0x10, 0x18, 0xB, 0xE}, kSm[6] = {-0x17, 1, -1, -10, -9, 4};
    const int lang_idx = std::clamp(lang_index(), 0, 5);
    const int dx0 = kind == ShopKind::Element ? kEl[lang_idx] : kind == ShopKind::Trading ? kTr[lang_idx] : kSm[lang_idx];
    // Strip base: element shop / trading post (0x124, 0x92), 10 cells; blacksmith (0x15C, 0x92), 3 cells.
    RT(1, static_cast<float>((kind == ShopKind::Smith ? 0x15C : 0x124) + dx0), 0x92, title_text);
  }
  std::string title_text() {
    switch (kind) {
      case ShopKind::Element: return ss(0x801DF40C);
      case ShopKind::Smith: return ss(0x801DFAC8);
      case ShopKind::Trading: return ss(0x801DF688);
    }
    return {};
  }
  // Total amount at (0x16E, 0xED): the 11-character grouped string (style 0x7C, dx
  // 0x2A - 4d (JP 0x23 - 4d), dy -4 (JP -3)), then "G" at x + 0x58, dy -2 (JP dx -6). Always colour 0x808080.
  void total_g(long long amount) {
    const u32 clamped = static_cast<u32>(std::max(0LL, amount));
    const bool jp = lang_index() == 5;
    const int digits = clamped == 0 ? 0 : digit_count(clamped);
    RT(0x7C, static_cast<float>(0x16E + (jp ? 0x23 : 0x2A) - 4 * digits), static_cast<float>(0xED + (jp ? -3 : -4)), grouped_gold(clamped));
    RT(5, static_cast<float>(0x16E + 0x58 + (jp ? -6 : 0)), 0xED - 2, U"G");
  }

  void build() {
    cmds.clear();
    ov = msg;
    fonts.set_classic(mode == ArtMode::Original);
    backdrop();
    if (kind == ShopKind::Trading) return build_trading();
    if (stage == Stage::Top || stage == Stage::Category) {
      banner(title_text());
      if (kind == ShopKind::Element) {
        message(stage == Stage::Top ? ss(0x801DF424) : std::string());
        compass(top_cur, {cm("買う"), cm("売る")}, stage == Stage::Top);
      } else if (stage == Stage::Top) {
        message(ss(0x801DF814));
        compass(top_cur, {cm("鍛造"), cm("分解"), cm("下取り")}, true);
      } else {
        static const u32 kForge[3] = {0x801DF835, 0x801DF845, 0x801DF855};
        message(ss(kForge[std::min(2, top_cur)]));
        compass(cat_cur, {cm("武器"), cm("防具"), cm("アクセサリー")}, true);
      }
      footer();
      return;
    }
    if (kind == ShopKind::Element) build_element_list();
    else build_smith_list();
    footer();
  }

  // ---- element shop lists: list panel at (20,32), info panel at (314,70)
  void info_panel_element(int id) {
    Frame(133, 314, 70);
    RT(1, 0x142, 0xA6, ss(0x801DF538));
    RT(1, 0x176, 0x9E, ss(0x801DF540));
    RT(1, 0x176, 0xB0, ss(0x801DF54C));
    if (id <= 0) return;
    // counts "%4d" at (0x1B6, 0xA0 / 0xB2): style 0x90 (JP 5), dx d == 1 ? 0x21 : 0x1E (JP 3 : 0)
    auto count = [&](float y, int value) {
      const int dx = lang_index() == 5 ? (digit_count(value) == 1 ? 3 : 0) : (digit_count(value) == 1 ? 0x21 : 0x1E);
      RT(lang_index() == 5 ? 5 : 0x90, static_cast<float>(0x1B6 + dx), y, fmt("%4d", value));
    };
    count(0xA0, shop::party_copies(*gs, id));
    count(0xB2, gs->element_count(id));
    // description: one entry, sysmes 0x1A row id*3+1 at (0x13E, 0x72), style 0x7B (row id*3+2 is never drawn).
    // Consumables (0x61..0x7D, not JP) use 0xBA = 0x7B plus the "(Consum.)" label at canvas (791, Y) with its own scale.
    const bool consum = id >= 0x61 && id <= 0x7D && lang_index() != 5;
    if (const std::string* description = lang_tables.sysmes((0x1Au << 16) | static_cast<u32>(id * 3 + 1), lang);
        description && !trim(*description).empty())
      RT(consum ? 0xBA : 0x7B, 0x13E, 0x72, *description);
    if (consum) {
      static const char* const kLbl[5] = {"(Consum.)", "(consommable)", "(consumabile)", "(Einweg)", "(consumible)"};
      static const float kY[5] = {214, 219, 219, 217, 216.5f}, kS[5] = {0.335f, 0.3f, 0.3f, 0.306f, 0.33f};
      const int lang_idx = std::clamp(lang_index(), 0, 4);
      const std::u32string label = utf32(kLbl[lang_idx]);
      fonts.prepare(font::kBn, label);
      Cmd cmd;
      cmd.kind = Cmd::Text;
      cmd.text = label;
      cmd.slot = font::kBn;
      cmd.ink = font::h1(0x808080, 20, 6);
      cmd.scale = kS[lang_idx];
      cmd.x = 791, cmd.y = kY[lang_idx];
      cmds.push_back(std::move(cmd));
    }
    const data::ElemRec* record = rules ? rules->elem(id) : nullptr;
    if (!record) return;
    if (rules->el_innate_only(id)) {
      int ci = 0;
      for (int bit_index = 2; bit_index < 8; ++bit_index)
        if (rules->el_colour(id) & (1 << bit_index)) ci = bit_index - 2;
      // Innate-only note at (0x13E, 0x60), style 0x7A.
      RT(0x7A, 0x13E, 0x60, ss(0x801DF5A9 + 12u * static_cast<u32>(ci)));
    }
    // "Lv" label, the level at (0x15E, 0x4E) with dy 3, the "+-" sign (the second sign frame is suppressed), then the
    // element-type text at (0x18E, 0x4E), style 0x79.
    RT(1, 0x13E, 0x4E, ss(0x801DF568));
    RT(1, 0x15E, 0x4E + 3, full_width(fmt("%1d", rules->el_level(id))));
    RT(0x10001, 0x172 - 2, 0x4C + 5, U"\u00B1");
    RT(1, static_cast<float>(0x17A + (lang_index() != 5 ? 2 : 0) + 1), 0x4E + 3, full_width(fmt("%1d", record->raw[0x12] & 0xF)));
    const u8 target = record->raw[4];
    const u32 key = ((target & 0x10) ? 0x801DF589u : 0x801DF581u) + ((target & 0xC0) == 0xC0 ? 2u : (target & 0x80) ? 1u : 0u);
    RT(0x79, 0x18E, 0x4E, ss(key));
  }
  // Quantity window: labels at (0x15E, 0xD4 / 0xE8) style 1; the count:
  // buying "%2d" at (0x19E, 0xD6), dx (JP 10 : 0x11) + (n < 10 ? 4 : 0), style 0x91; selling "%4d" at (0x186, 0xD6), dx 0x2B - 4 x (JP (9 - x)
  // * 4), x = digits; dy -1 (not JP); then the total (no colour change when it exceeds the gold).
  void qty_window(u32 lbl1, u32 lbl2, bool selling, long long total) {
    Frame(134, 342, 200);
    RT(1, 0x15E, 0xD4, ss(lbl1));
    RT(1, 0x15E, 0xE8, ss(lbl2));
    const bool jp = lang_index() == 5;
    const float dy = jp ? 0.0f : -1.0f;
    if (selling) {
      const int x = digit_count(qty);
      RT(jp ? 4 : 0x91, static_cast<float>(0x186 + (jp ? (9 - x) * 4 : 0x2B - 4 * x)), 0xD6 + dy, fmt("%4d", qty));
    } else {
      RT(jp ? 4 : 0x91, static_cast<float>(0x19E + (jp ? 10 : 0x11) + (qty < 10 ? 4 : 0)), 0xD6 + dy, fmt("%2d", qty));
    }
    total_g(total);
  }
  void build_element_list() {
    const bool buying = operation == Operation::Buy;
    Frame(132, 20, 32);
    panel_caption(0x84, 20, 32);
    const int count = static_cast<int>(ids.size());
    int sel_id = (count > 0 && cur < count) ? ids[static_cast<size_t>(cur)] : 0;
    info_panel_element(sel_id);
    if (scroll > 0) scroll_arrow(306, 50, true);
    if (scroll + kVisibleRows < std::max(count, kVisibleRows)) scroll_arrow(306, 307, false);
    for (int row = 0; row < kVisibleRows; ++row) {
      const int index = scroll + row;
      if (index >= count) break;
      const int id = ids[static_cast<size_t>(index)];
      const float y0 = 32.0f + 24.0f * static_cast<float>(row);
      const bool sel = index == cur;
      plate(id, 42, 47.0f + 24.0f * static_cast<float>(row));
      // Name centred on x 0x84 (style 0x81), colour 0x909090 / 0x7A6AA5 when selected.
      RT(0x81, 0x84, 0x34 + 24.0f * static_cast<float>(row), element_name(id), sel ? 0x7A6AA5u : 0x909090u);
      // price "%5d" at (0xE4, 0x34 + 24 r): dx 0xF - 4d, style 0x91 (JP 4)
      const int price = static_cast<int>(buying ? shop::element_price(*gs, id) : shop::element_sell_unit(*gs, id));
      RT(lang_index() == 5 ? 4 : 0x91, static_cast<float>(0xE4 + 0xF - 4 * digit_count(price)), y0 + 20, fmt("%5d", price));
    }
    marker(20, 52.0f + 24.0f * static_cast<float>(cur - scroll), stage == Stage::List);
    if (stage == Stage::Quantity && count > 0) {
      const int id = ids[static_cast<size_t>(cur)];
      long long total = buying ? static_cast<long long>(shop::element_price(*gs, id)) * qty : shop::element_sell_total(*gs, id, qty);
      qty_window(0x801DF558, 0x801DF560, !buying, total);
      message(ss(buying ? 0x801DF4D6 : 0x801DF4E6));
    } else if (stage == Stage::List) {
      message(ss(buying ? 0x801DF4D5 : 0x801DF4E5));
    }
  }

  // ---- blacksmith lists
  // Party strip at (0xE6, 0x13A): the plate frame at (x - 3, y - 8), 270 x 107,
  // then the 44 character records r (11 x 4, 24 px pitch): face (hd/face) in 0x707070,
  // 0 when not recruited, 0x303030 when the cursor item cannot be equipped by r; on a bright face the smithy mark
  // (hd/menu/smithy.png): weapon / armour n = 3 when it is the
  // one worn, else 1 when the item's power is higher than the worn one's, 2 when lower; accessory n = 3 when worn in any of the 3 slots.
  void party_strip(int item) {
    if (Tex* plate_frame = texs.shape(173, lang)) Img(plate_frame, SX(230 - 3), SY(314 - 8), SX(plate_frame->w / 4.0f), SY(plate_frame->h / 4.0f));
    if (!rules || !rules->tables) return;
    const data::GameTables& tb = *rules->tables;
    const data::ItemInfo* ii = item >= 0 && item < static_cast<int>(tb.item_info.size()) ? &tb.item_info[static_cast<size_t>(item)] : nullptr;
    auto power = [&](int id) { return id >= 0 && id < static_cast<int>(tb.gear.size()) ? tb.gear[static_cast<size_t>(id)].power() : 0; };
    Tex* smithy = texs.load("hd/menu/smithy.png");
    for (int character = 0; character < 44; ++character) {
      const float x = 230.0f + 24.0f * static_cast<float>(character % 11), y = 314.0f + 24.0f * static_cast<float>(character / 11);
      const save::SaveChar& ch = gs->slot.chars[static_cast<size_t>(character)];
      int brightness = 0x70;
      if (!gs->slot.recruited(character)) brightness = 0;
      else if (!ii || !ii->can_equip(character)) brightness = 0x30;
      // Faces sheet frame: frame_no 1..0x33 are drawn as faces.
      const int frame_no = (ch.model_id & 0xFF) + 1;
      if (frame_no >= 1 && frame_no <= 0x33)
        if (const Tex* face = face_tex(frame_no - 1)) Img(face, SX(x), SY(y), SX(24), SY(24), 0, 0, 1, 1, grey(brightness));
      if (item <= 0 || brightness != 0x70 || !ii) continue;
      int mark = 0;
      const int ik = ii->cat & 0xF;
      if (ik == 0 || ik == 1) {
        const int worn = ik == 0 ? ch.weapon : ch.armour;
        if (worn == item) mark = 3;
        else if (power(worn) > power(item)) mark = 2;
        else if (power(worn) < power(item)) mark = 1;
      } else if (ik == 2) {
        for (int k = 0; k < 3; ++k)
          if (ch.accessory[static_cast<size_t>(k)] == item) mark = 3;
      }
      if (!mark || !smithy) continue;
      // The three mark rects in smithy.png (480 x 1024): (236, 972, 36, 40), (332, 972, 36, 40), (440, 972, 24, 36).
      static const float kU[3] = {236, 332, 440}, kMw[3] = {36, 36, 24}, kMh[3] = {40, 40, 36};
      const int mark_index = mark - 1;
      Img(smithy, SX(x), SY(y), SX(kMw[mark_index] / 4.0f), SY(kMh[mark_index] / 4.0f), kU[mark_index] / smithy->w, 972.0f / smithy->h,
          (kU[mark_index] + kMw[mark_index]) / smithy->w, (972.0f + kMh[mark_index]) / smithy->h);
    }
  }
  // Face sprites: hd/face/<2021 | 1999>/NN_name.png by art mode. faces_listed: 1 = 2021, 2 = 1999.
  std::map<int, std::string> face_files;
  int faces_listed = 0;
  const Tex* face_tex(int character) {
    const bool classic = mode == ArtMode::Original;
    // Classic + Ps1ResMenus: cdrom/face/<stem>.tim.
    if (ps1_on() && character >= 0 && character < 50)
      if (Tex* disc_face = texs.ps1("ps1:face" + std::to_string(character), [&] { return ps1::face(*vfs, character); })) return disc_face;
    if (faces_listed != (classic ? 2 : 1)) {
      faces_listed = classic ? 2 : 1;
      face_files.clear();
      const std::string pre = classic ? "face/1999/" : "face/2021/";
      for (const std::string& name : vfs->list("hd")) {
        if (name.rfind(pre, 0) != 0) continue;
        const std::string file = name.substr(10);
        if (file.size() > 3 && std::isdigit(static_cast<unsigned char>(file[0])) && std::isdigit(static_cast<unsigned char>(file[1])) && file[2] == '_')
          face_files[(file[0] - '0') * 10 + (file[1] - '0')] = "hd/" + name;
      }
    }
    const auto it = face_files.find(character);
    return it == face_files.end() ? nullptr : texs.load(it->second);
  }
  void build_smith_list() {
    const int row_count = static_cast<int>(ids.size());
    const int item = (row_count > 0 && cur < row_count) ? ids[static_cast<size_t>(cur)] : 0;
    const bool forge_like = operation == Operation::Forge || operation == Operation::Break;
    if (forge_like) {
      Frame(154 + std::clamp(category, 0, 2), 20, 32);
      panel_caption(0x9A + std::clamp(category, 0, 2), 20, 32);
      RT(1, 0x1E, 0x14C, ss(0x801DFAD0));
      RT(1, 0x50, 0x144, ss(0x801DFAD8));
      RT(1, 0x50, 0x154, ss(0x801DFAE4));
      // "%2d" counts at (0xA0, 0x148 / 0x158): style 0x90, dy -2, dx by language and digit count.
      if (item > 0) {
        auto count = [&](float y, int value) {
          const bool one = digit_count(value) == 1;
          const int lang_idx = lang_index(),
                    dx = lang_idx == 5 ? (one ? 4 : 0) : lang_idx == 4 ? (one ? 0x26 : 0x22) : (one ? 0x18 : 0x14);
          RT(lang_idx == 5 ? 5 : 0x90, static_cast<float>(0xA0 + dx), y - 2, fmt("%2d", value));
        };
        count(0x148, shop::equipped_copies(*gs, item));
        count(0x158, gs->item_count(item));
      }
      if (scroll > 0) scroll_arrow(210, 50, true);
      if (scroll + kVisibleRows < std::max(row_count, kVisibleRows)) scroll_arrow(210, 302, false);
    } else {
      Frame(160 + std::clamp(category, 0, 2), 20, 32);
      panel_caption(0xA0 + std::clamp(category, 0, 2), 20, 32);
      if (scroll > 0) scroll_arrow(304, 50, true);
      if (scroll + kVisibleRows < std::max(row_count, kVisibleRows)) scroll_arrow(304, 312, false);
    }
    for (int row = 0; row < kVisibleRows; ++row) {
      const int index = scroll + row;
      if (index >= row_count) break;
      const int id = ids[static_cast<size_t>(index)];
      // Name at (0x44, 0x30 + 24 r), constant 0x808080 (the marker shows the row), style 0xAE.
      RT(0xAE, 0x44, 0x30 + 24.0f * static_cast<float>(row), item_name(id));
      // Sell price "%6d" at (0xC4, 0x32 + 24 r): dx 0x16 - 4d, style 0x91 (JP 4).
      if (!forge_like) {
        const int price = shop::gear_sell_unit(*gs, id);
        RT(lang_index() == 5 ? 4 : 0x91, static_cast<float>(0xC4 + 0x16 - 4 * digit_count(price)), 0x32 + 24.0f * static_cast<float>(row),
           fmt("%6d", price));
      }
    }
    marker(30, 48.0f + 24.0f * static_cast<float>(cur - scroll), stage == Stage::List);
    if (forge_like) {
      // material panel: forge (266x210) / disassemble (266x168) at (220,32)
      Frame(operation == Operation::Forge ? 157 : 159, 220, 32);
      RT(1, operation == Operation::Forge ? 364.0f : 300.0f, 0x1C, ss(operation == Operation::Forge ? 0x801DFAF8 : 0x801DFB0C));
      if (operation == Operation::Forge) {
        RT(1, 0x154, 0xD4, ss(0x801DFAF0));
        // fee: 11-character grouped string at (0x174, 0xD8), style 0x90, dx 0x35 - 4d (JP 0x37 - 4d); "G" (0x1CC, 0xD8) dy +1.
        // The "No crafting cost" cheat makes it free.
        if (item > 0) {
          const u32 fee = platform::cheats().no_craft_cost ? 0u : static_cast<u32>(std::max(0LL, static_cast<long long>(shop::forge_cost(*gs, item))));
          const bool jp = lang_index() == 5;
          RT(jp ? 5 : 0x90, static_cast<float>(0x174 + (jp ? 0x37 : 0x35) - 4 * digit_count(fee)), 0xD8, grouped_gold(fee));
          RT(5, 0x1CC, jp ? 216.0f : 217.0f, U"G");
        }
      }
      if (item > 0) {
        int material_index = 0;
        for (const shop::Mat& material : shop::recipe(*gs, item)) {
          if (material_index >= 32) break;
          // Name at (0xE4 + 0x80 (u & 1), 0x30 + 24 (u >> 1)), style 0xAF (JP 1), colour 0x303080 on a forge shortage;
          // at x + 0x50: need "%2d" (x, y + 2) or "-" (style 5) at (x + 2, y) when disassembling, "/" (x + 0x10, y),
          // have "%2d" (x + 0x18, y + 2); numbers style 0x90 with dx 3 below 10.
          const float x = 228.0f + 128.0f * static_cast<float>(material_index & 1), y = 48.0f + 24.0f * static_cast<float>(material_index >> 1);
          const int have = gs->item_count(material.id);
          const bool shortage = operation == Operation::Forge && have < material.qty && !platform::cheats().no_craft_cost;
          const u32 col = shortage ? 0x303080u : 0x808080u;
          // drop_first removes the material glyph U+03B4.
          RT(lang_index() == 5 ? 1 : 0xAF, x, y, drop_first(item_name(material.id)), col);
          const float xb = x + 0x50;
          const int nst = lang_index() == 5 ? 5 : 0x90;
          if (operation == Operation::Forge)
            RT(nst, xb + (std::min(material.qty, 99) < 10 ? 3.0f : 0.0f), y + 2, fmt("%2d", std::min(material.qty, 99)), col);
          else RT(5, xb + 2, y, U"-", col);
          RT(lang_index() == 5 ? 5 : 0xB0, xb + 0x10, y, U"/", col);
          RT(nst, xb + 0x18 + (std::min(have, 99) < 10 ? 3.0f : 0.0f), y + 2, fmt("%2d", std::min(have, 99)), col);
          ++material_index;
        }
      }
      party_strip(item);
    } else {
      // trade-in: info panel with Stock / No. equipped / No. stockpiled and the item description
      Frame(133, 314, 70);
      RT(1, 0x142, 0xA6, ss(0x801DFAD0));
      RT(1, 0x172, 0x9D, ss(0x801DFAD8));
      RT(1, 0x172, 0xAF, ss(0x801DFAE4));
      // "%2d" counts at (0x1C2, 0xA1 / 0xB3): style 0x90, dy -2, dx by language.
      if (item > 0) {
        auto count = [&](float y, int value) {
          static const int kOne[6] = {13, 19, 5, -12, 0x21, 4}, kMore[6] = {10, 16, 2, -15, 0x1E, 0};
          const int lang_idx = std::clamp(lang_index(), 0, 5), dx = digit_count(value) == 1 ? kOne[lang_idx] : kMore[lang_idx];
          RT(lang_idx == 5 ? 5 : 0x90, static_cast<float>(0x1C2 + dx), y - 2, fmt("%2d", value));
        };
        count(0xA1, shop::equipped_copies(*gs, item));
        count(0xB3, gs->item_count(item));
        // description: one entry waa_spec row item * 3 at (0x142, 0x5E), style 0x8F (the 0x6E / 0x7E slots stay empty)
        const std::string spec = table_row("waa_spec", item * 3);
        if (!trim(spec).empty()) RT(0x8F, 0x142, 0x5E, spec);
      }
    }
    // messages / Yes-No rows / quantity window
    if (stage == Stage::Confirm) {
      if (operation == Operation::Forge) message(ss(0x801DF834), 0x71);
      else message(ss(0x801DF95C), 0x72);
      RT(1, 0x150, 0x10E + 16, ss(0x801DF8A0));
      RT(1, 0x150, 0x10E + 32, ss(0x801DF8AC));
      marker(336 - 4, 286.0f + 16.0f * static_cast<float>(conf_cur), true);
    } else if (stage == Stage::Quantity) {
      qty_window(0x801DFB28, 0x801DFB30, false, item > 0 ? static_cast<long long>(shop::gear_sell_unit(*gs, item)) * qty : 0);
      message(ss(0x801DF859));
    } else if (stage == Stage::List) {
      static const u32 kF[3] = {0x801DF892, 0x801DF893, 0x801DF894}, kD[3] = {0x801DF846, 0x801DF847, 0x801DF848}, kS[3] = {0x801DF856, 0x801DF857, 0x801DF858};
      const int clamped_category = std::clamp(category, 0, 2);
      message(ss(operation == Operation::Forge ? kF[clamped_category] : operation == Operation::Break ? kD[clamped_category] : kS[clamped_category]));
    }
  }

  // ===================================================== trading post
  // Per frame: state 0 greeting (banner + "Welcome!"), 1 goods list ("Exchange Rate" panel + info panel), 2 Element offer
  // (offer list, "levels' worth to go", "Make the exchange?"). Prompts at (0x150, 0x10E). Text goes through the shop list, same
  // path in both art modes.
  // kTradeCol: the colour dot of each colour index (green .. blue).
  static constexpr u32 kTradeCol[6] = {0x008000, 0x808080, 0x000000, 0x000080, 0x008080, 0x800000};
  // Drops the first character (the material glyph U+03B4).
  static std::u32string drop_first(std::u32string str) {
    if (!str.empty()) str.erase(0, 1);
    return str;
  }
  // The goods id / element id under the cursor (0 = none).
  int trade_goods() const { return trade.goods_cur >= 0 && trade.goods_cur < 16 ? trade.row.goods[static_cast<size_t>(trade.goods_cur)] : 0; }
  const shop::TradeSlot& trade_slot() const { return trade.row.slot[static_cast<size_t>(std::clamp(trade.goods_cur, 0, 15))]; }
  int trade_element(int index) const {
    return index >= 0 && index < static_cast<int>(trade.elements.size()) ? trade.elements[static_cast<size_t>(index)] : 0;
  }
  // Opens the shop's trade.bin row at state 0; the cursors start at 0.
  void trade_open() {
    trade = Trade{};
    trade.row = shop::trade_row(shop_data, shop_id);
    trade.offered.assign(192, 0);
    std::string goods_text;
    for (int i = 0; i < 16; ++i)
      if (trade.row.goods[static_cast<size_t>(i)] > 0)
        goods_text += std::format("{}(mask {:#x} lv {}) ", trade.row.goods[static_cast<size_t>(i)], trade.row.slot[static_cast<size_t>(i)].mask,
                                  trade.row.slot[static_cast<size_t>(i)].need);
    log::info("trade: row {} goods {}", shop_id, goods_text);
  }
  void trade_log_stock(const char* when) {
    std::string offer_text;
    for (int k = 0; k < trade.element_rows; ++k)
      if (const int id = trade_element(k); id > 0)
        offer_text += std::format("{}:{}x{} ", id, rules ? rules->el_level(id) : 0, shop::element_stock(*gs, id));
    log::info("trade: {} goods {} stock {}, offer list {}", when, trade_goods(), shop::item_stock(*gs, trade_goods()), offer_text);
  }
  void trade_update(const Nav& nav) {
    // State 0: greeting.
    if (trade.stage == 0) {
      if (nav.ok) {
        sfx(8);
        trade.row = shop::trade_row(shop_data, shop_id);
        trade.prepared = 0;
        trade.stage = 1;
        trade.goods_cur = 0;
        log::info("trade: goods list ({} entries)", trade.row.count);
      } else if (nav.cancel) {
        sfx(6);
        closed = true;
        log::info("trade: closed");
      }
      return;
    }
    // State 1: goods list; one goods name is prepared per frame, then the 3 description lines (0x10..0x12).
    if (trade.stage == 1) {
      if (trade.prepared < 0x10) trade.prepared = trade.row.goods[static_cast<size_t>(trade.prepared)] > 0 ? trade.prepared + 1 : 0x10;
      else if (trade.prepared < 0x13) ++trade.prepared;
      const bool go = trade.prepared > 0xF;
      if (nav.up && go) {
        trade.goods_cur = std::max(0, trade.goods_cur - 1);
        trade.prepared = 0x10;
        sfx(1);
      }
      if (nav.down && go) {
        trade.goods_cur = std::max(0, std::min(trade.row.count - 1, trade.goods_cur + 1));
        trade.prepared = 0x10;
        sfx(1);
      }
      if (nav.ok && go) {
        sfx(8);
        trade.elements = shop::trade_elements(*gs, &trade.element_rows);
        trade.stage = 2;
        trade.confirm = false;
        trade.prepared = trade.offer_top = trade.scroll_dir = 0;
        trade.offered.assign(192, 0);
        trade_log_stock("offer");
      }
      if (nav.cancel) {
        trade.stage = 0;
        sfx(6);
      }
      return;
    }
    // State 2: Element offer; one row name is prepared per frame (after a scroll only the new row 0 / 10).
    if (trade.prepared < 11) {
      const int row = trade.scroll_dir == 1 ? 0 : trade.scroll_dir == 2 ? 10 : trade.prepared;
      trade.prepared = trade_element(trade.offer_top + row) > 0 ? trade.prepared + 1 : 11;
      if (trade.prepared >= 11) trade.scroll_dir = 0;
    }
    const shop::TradeSlot& goods_slot = trade_slot();
    int sum = shop::offer_levels(*gs, trade.elements, trade.offered);
    if (!trade.confirm) {
      const bool go = trade.prepared > 10;
      if (nav.up && go) {
        sfx(1);
        trade.prepared = 11;
        if (--trade.offer_cur < 0) {
          if (trade.offer_top > 0) {
            --trade.offer_top;
            trade.prepared = 0;
            trade.scroll_dir = 1;
          }
          trade.offer_cur = 0;
        }
      }
      if (nav.down && go) {
        sfx(1);
        trade.prepared = 11;
        if (++trade.offer_cur < 11) {
          // No move onto an empty row.
          if (trade_element(trade.offer_top + trade.offer_cur) == 0) --trade.offer_cur;
        } else {
          const int last = trade.element_rows - 11;
          if (trade.offer_top + 1 <= last) {
            ++trade.offer_top;
            trade.prepared = 0;
            trade.scroll_dir = 2;
          }
          trade.offer_cur = 10;
        }
      }
      // Circle with auto-repeat: holding it keeps adding. Sound 8 on every press, also on a refused row.
      if (nav.ok_rep && trade.prepared > 10) {
        sfx(8);
        const int element_index = trade.offer_top + trade.offer_cur, id = trade_element(element_index);
        if (shop::trade_accepts(*gs, goods_slot.mask, id) && sum < goods_slot.need) {
          int& offered_count = trade.offered[static_cast<size_t>(element_index)];
          if (shop::element_stock(*gs, id) != offered_count) ++offered_count;
          sum = shop::offer_levels(*gs, trade.elements, trade.offered);
          if (goods_slot.need <= sum) {
            trade.confirm_frames = 0;
            trade.confirm = true;
            trade.yes_no = 0;
          }
        }
      }
      // Back to the goods list; the offer is cleared on the next visit.
      if (nav.cancel) {
        sfx(6);
        trade.row = shop::trade_row(shop_data, shop_id);
        trade.prepared = 0;
        trade.stage = 1;
      }
      return;
    }
    ++trade.confirm_frames;
    if (nav.up || nav.down) {
      sfx(1);
      trade.yes_no ^= 1;
    }
    if (nav.ok && trade.confirm_frames > 0x1E) {
      if (trade.yes_no == 0) {
        const int goods = trade_goods(), before = shop::item_stock(*gs, goods);
        if (!shop::trade_commit(*gs, trade.elements, trade.offered, goods)) {
          sfx(0x18);
        } else {
          sfx(0x1B);
          log::info("trade: traded {} levels for goods {} (stock {} -> {})", sum, goods, before, shop::item_stock(*gs, goods));
          trade_log_stock("after");
        }
      } else {
        sfx(8);
      }
      std::fill(trade.offered.begin(), trade.offered.end(), 0);
      trade.confirm = false;
    }
    if (nav.cancel) {
      std::fill(trade.offered.begin(), trade.offered.end(), 0);
      sfx(6);
      trade.confirm = false;
    }
  }

  // "Exchange Rate" panel at (0x14, 0x20), one row per entry at y = 0x44 + 18 i: the goods name (first character dropped, ":" added,
  // style 0x82 = right edge at 0x92), the colour column at 0x94 (a colour dot from kTradeCol, or a text for mask 0) and the level
  // sum "%d" at (0xA4, y) (style 0x98).
  void trade_goods_list() {
    Frame(144, 0x14, 0x20);
    RT(1, 0x24, 0x2C, ss(0x801DF784));
    const int entry_count = std::min(trade.row.count, 16);
    for (int i = 0; i < entry_count; ++i) {
      const float y = 0x44 + 18.0f * static_cast<float>(i);
      const shop::TradeSlot& entry = trade.row.slot[static_cast<size_t>(i)];
      if (const int goods = trade.row.goods[static_cast<size_t>(i)]; goods > 0)
        RT(0x82, 0x92, y, drop_first(item_name(goods)) + (lang_index() == 5 ? U"：" : U":"));
      const int colour = shop::colour_index(entry.mask);
      if (colour == 0) RT(1, 0x94, y, ss(0x801DF7C4));
      else RT(0x10001, 0x94, y, U"●", kTradeCol[colour - 1]);
      RT(0x98, 0xA4, y, full_width(fmt("%d", entry.need)));
    }
  }
  // Info panel at (0x13A, 0x46) (description, "No. in Stock" + "%9d"), colour box at (0x146, 200) ("All Element
  // Colors" / "<colour> Elements only!" merged under the 5-cell strip at (0x14E, 0xD2), style 0xB5) and "<need> levels' worth".
  void trade_info() {
    const bool jp = lang_index() == 5;
    const int lang_idx = std::clamp(lang_index(), 0, 5);
    Frame(133, 0x13A, 0x46);
    Frame(145, 0x146, 200);
    const int goods = trade_goods();
    // Description: waa_spec row goods * 3 at (0x142, 0x5E), style 0x8F.
    if (goods > 0)
      if (const std::string spec = table_row("waa_spec", goods * 3); !trim(spec).empty()) RT(0x8F, 0x142, 0x5E, spec);
    // "No. in Stock"
    RT(1, 0x142, 0xA6, ss(0x801DF73C));
    RT(1, 0x1DC, 0xA7, ss(0x801DF735));
    // Stock "%9d" at (0x13A, 0xA7): dx n < 10 ? 0x72 : 0x6E (JP 4 : 0), dy -2.
    const int stock = shop::item_stock(*gs, goods);
    RT(0x98, static_cast<float>(0x13A + (stock < 10 ? (jp ? 4 : 0x72) : (jp ? 0 : 0x6E))), 0xA7 - 2, full_width(fmt("%9d", stock)));
    // Colour condition strip at (0x14E, 0xD2), x shifted per language.
    static const int kDx0[6] = {0x20, 6, 0x10, 0x18, 0xB, 0xE};
    RT(0xB5, static_cast<float>(0x14E + kDx0[lang_idx]), 0xD2, ss(0x801DF689));
    const int colour = shop::colour_index(trade_slot().mask);
    if (colour == 0) {
      RT(0xB5, 0x14E, 0xD2, ss(0x801DF72D));
    } else {
      RT(1, 0x19E, 0xD2, ss(0x801DF79C + 4u * static_cast<u32>(colour - 1)), kTradeCol[colour - 1]);
      RT(0xB5, 0x14E, 0xD2, ss(0x801DF77C + static_cast<u32>(colour)));
    }
    RT(1, 0x14E, 0xE4, ss(0x801DF794));
    // "levels' worth" label at (0x1BE, 0xE4): dx0 (JP 0 : -0x23) + per language, dy +1 (not JP).
    static const int kLw[6] = {-0xC, -0x3A, -0x36, -0xC, -0x36, 0};
    RT(1, static_cast<float>(0x1BE + (jp ? 0 : -0x23) + kLw[lang_idx]), 0xE4 + (jp ? 0.0f : 1.0f), ss(0x801DF774));
    // Need "%5d" at (0x16E, 0xE4): dx (L in 1, 2, 4 ? -0x2A : 0) + (n < 10 ? 4 : 0).
    const int need = trade_slot().need;
    RT(0x98, static_cast<float>(0x16E + ((lang_idx == 1 || lang_idx == 2 || lang_idx == 4) ? -0x2A : 0) + (need < 10 ? 4 : 0)), 0xE4,
       full_width(fmt("%5d", need)));
  }
  // Offer list at (0x14, 0x20); row r at y = 0x38 + 24 r: plate (colour sheet frame 3) at (0x4A, 0x33 + 24 r),
  // "Lv" at 0x32, level "%d" at 0x42, "x" + stock - offered (suji) at 0x100, "-" + offered at 0x118 in 0x7A6AA5, the name
  // centred on 0xA4 (style 0x81). Rows the entry's colour mask refuses are 0x404040.
  void trade_offer_list() {
    Frame(147, 0x14, 0x20);
    panel_caption(0x93, 0x14, 0x20);
    if (trade.offer_top + 11 < trade.element_rows) scroll_arrow(0x132, 0x133, false);
    if (trade.offer_top > 0) scroll_arrow(0x132, 0x32, true);
    const int mask = trade_slot().mask;
    // Two digits; the tens digit only when non-zero.
    auto digits2 = [&](float x, float y, int value, u32 colour) {
      if (value / 10) RT(0x99, x + 8, y + 3, std::string(1, static_cast<char>('0' + value / 10 % 10)), colour);
      RT(0x99, x + 16, y + 3, std::string(1, static_cast<char>('0' + value % 10)), colour);
    };
    for (int row = 0; row < 11; ++row) {
      const int element_index = trade.offer_top + row;
      if (element_index >= trade.element_rows) break;
      const int id = trade_element(element_index);
      if (id <= 0) continue;
      const bool ok = shop::trade_accepts(*gs, mask, id);
      const u32 c12 = ok ? 0x808080u : 0x404040u, c14 = ok ? (row == trade.offer_cur ? 0x7A6AA5u : 0x909090u) : 0x404040u;
      const float y = 0x38 + 24.0f * static_cast<float>(row);
      plate_indexed(shop::colour_index(rules ? rules->el_colour(id) : 0), 0x4A, 0x33 + 24.0f * static_cast<float>(row), c12);
      RT(5, 0x32, y, U"Lv", c12);
      RT(lang_index() == 5 ? 5 : 0x90, 0x42, y, fmt("%d", rules ? rules->el_level(id) : 0), c12);
      const int offered_count = trade.offered[static_cast<size_t>(element_index)];
      RT(9, 0x100, y + 3, U"x", c12);
      digits2(0x100, y, std::clamp(shop::element_stock(*gs, id) - offered_count, 0, 99), c12);
      if (offered_count) {
        RT(8, 0x118, y, U"-", 0x7A6AA5u);
        digits2(0x118, y, offered_count, 0x7A6AA5u);
      }
      RT(0x81, 0xA4, y, element_name(id), c14);
    }
  }
  void build_trading() {
    const int lang_idx = std::clamp(lang_index(), 0, 5);
    if (trade.stage == 0) {
      banner(title_text());
      RT(1, 0x150, 0x10E, ss(0x801DF6DC));
      RT(1, 0x150, 0x11E, ss(0x801DF6B4));
      RT(1, 0x150, 0x12E, ss(0x801DF6C8));
    } else if (trade.stage == 1) {
      trade_goods_list();
      trade_info();
      RT(1, 0x150, 0x10E, ss(0x801DF6EC));
      RT(1, 0x150, 0x11E, ss(0x801DF6F9));
      marker(0x1E, 0x46 + 18.0f * static_cast<float>(trade.goods_cur), true);
    } else {
      trade_offer_list();
      trade_info();
      const int sum = shop::offer_levels(*gs, trade.elements, trade.offered), need = trade_slot().need;
      // Status line at (0x26, 0x14E): dx by language, no style remap.
      static const int kDx[6] = {0x41, 0x41, 0x24, 0x41, 0x24, 0};
      if (sum < need) {
        // "levels' worth to go"
        RT(1, static_cast<float>(0x26 + kDx[lang_idx]), 0x14E, ss(0x801DF760));
        RT(1, 0xB6, 0x14E, ss(0x801DF76C));
        // Remaining "%5d" at (0x66, 0x14E), dy -1.
        const int remaining = need - sum;
        const int dx = lang_idx == 5 ? (remaining < 10 ? 0 : -4) : (remaining < 10 ? 4 : 0) - ((lang_idx == 2 || lang_idx == 4) ? 0x3D : 0x20);
        RT(0x98, static_cast<float>(0x66 + dx), 0x14E - 1, full_width(fmt("%5d", remaining)));
      } else {
        // "Required Lv reached"
        RT(1, static_cast<float>(0x26 + kDx[lang_idx]), 0x14E, ss(0x801DF668));
        RT(1, 0xA6, 0x14E, ss(0x801DF67C));
      }
      if (!trade.confirm) {
        marker(0x14, 0x38 + 24.0f * static_cast<float>(trade.offer_cur), true);
        RT(1, 0x150, 0x10E, ss(0x801DF6A0));
        RT(1, 0x150, 0x11E, ss(0x801DF708));
        RT(1, 0x150, 0x12E, ss(0x801DF718));
      } else {
        marker(0x150, 0x11E + 16.0f * static_cast<float>(trade.yes_no), true);
        // "Make the exchange?", then Yes / No
        RT(1, 0x150, 0x10E, ss(0x801DF6F8));
        RT(1, 0x170, 0x11E, ss(0x801DF7B4));
        RT(1, 0x170, 0x12E, ss(0x801DF7BC));
      }
    }
    footer();
  }

  // Classic art with [Video] Ps1ResMenus: the whole shop is drawn into a target of the original menu's display size
  // (512 x 448 buffer; 432 of its rows map onto the canvas, so one PS1 pixel = one target pixel), scaled up nearest.
  void draw(const render::Frame& frame, float vw, float vh, bool resume) {
    if (closed || !gs || !ready) return;
    render::Frame lf;
    if (ps1_menus() && lowres.begin(frame, kShop, text, lf)) {
      draw_view(lf, kCanvasW, kCanvasH, resume);
      lowres.end(frame, vw, vh);
      return;
    }
    draw_view(frame, vw, vh, resume);
  }
  void draw_view(const render::Frame& frame, float vw, float vh, bool resume) {
    const render::CanvasFit cf = render::canvas_fit(vw, vh, kCanvasW, kCanvasH);
    zoom = cf.s;
    ox = cf.ox;
    oy = cf.oy;
    build();
    sprites->begin(frame, vw, vh, resume);
    sprites->draw(texs.white_set(), 0, 0, vw, vh, 0, 0, 1, 1, rgb(0, 0, 0, 255));
    for (const Cmd& cmd : cmds) {
      if (cmd.kind == Cmd::Text) fonts.draw(cmd.slot, cmd.text, cmd.x + ox / zoom, cmd.y + oy / zoom, cmd.ink, cmd.scale, cmd.align, zoom);
      else sprites->draw(cmd.set, ox + cmd.x * zoom, oy + cmd.y * zoom, cmd.w * zoom, cmd.h * zoom, cmd.u0, cmd.v0, cmd.u1, cmd.v1, cmd.color);
    }
    sprites->end();
  }
};

// ================================================================================================================
ShopScreen::ShopScreen() : impl_(std::make_unique<Impl>()) {}
int ShopScreen::take_first_forged() { return std::exchange(impl_->first_forged, 0); }
ShopScreen::~ShopScreen() { shutdown(); }

bool ShopScreen::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text,
                      const data::GameTables& tables, import::Lang lang) {
  return impl_->init(ctx, vfs, sprites, text, tables, lang);
}
void ShopScreen::shutdown() {
  if (impl_ && impl_->ctx) impl_->shutdown();
}
void ShopScreen::set_rules(const MenuRules* rules) { impl_->rules = rules; }
void ShopScreen::set_mode(ArtMode mode) { impl_->mode = mode; }
ArtMode ShopScreen::mode() const { return impl_->mode; }
void ShopScreen::set_language(import::Lang lang) { impl_->lang = lang; }
void ShopScreen::set_sfx(std::function<void(int)> fn) { impl_->sfx_fn = std::move(fn); }
void ShopScreen::open(GameState& state, ShopKind kind, int shop_id) { impl_->open(state, kind, shop_id); }
void ShopScreen::close() { impl_->closed = true; }
bool ShopScreen::closed() const { return impl_->closed; }
ShopKind ShopScreen::kind() const { return impl_->kind; }
int ShopScreen::shop_id() const { return impl_->shop_id; }
std::string ShopScreen::state_name() const {
  // Trading post states.
  if (impl_->kind == ShopKind::Trading) {
    const auto& trade = impl_->trade;
    return trade.stage == 0 ? "top" : trade.stage == 1 ? "list" : trade.confirm ? "confirm" : "offer";
  }
  static const char* const kNames[] = {"top", "category", "list", "quantity", "confirm"};
  return kNames[static_cast<int>(impl_->stage)];
}
void ShopScreen::update(const platform::Input& in, int ticks) { impl_->update(in, ticks); }
void ShopScreen::draw(const render::Frame& frame, float vw, float vh, bool resume) { impl_->draw(frame, vw, vh, resume); }

}  // namespace oc::ui
