#include "ui/menu/main_menu.h"
#include "ui/menu/main_menu_impl.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

#include "core/log.h"
#include "data/char_name.h"
#include "platform/keymap.h"
#include "ui/lowres.h"
#include "ui/menu/menu_common.h"
#include "ui/ps1_font.h"

namespace oc::ui {

const char* menu_screen_name(MenuScreen screen) {
  switch (screen) {
    case MenuScreen::Main: return "main";
    case MenuScreen::Status: return "status";
    case MenuScreen::ElementChoice: return "element_choice";
    case MenuScreen::ElementUse: return "element_use";
    case MenuScreen::ElementGrid: return "element_grid";
    case MenuScreen::Equip: return "equip";
    case MenuScreen::KeyItems: return "key_items";
    case MenuScreen::Settings: return "settings";
    case MenuScreen::Save: return "save";
    case MenuScreen::OpenCross: return "opencross";
    case MenuScreen::OcPage: return "oc_page";
  }
  return "?";
}

// ===================================================== setup
bool MainMenu::Impl::init(render::GpuContext& context, const vfs::Vfs& files, render::SpriteBatch& sprite_batch, render::TextBatch& text_batch,
          const data::GameTables& game_tables, import::Lang language) {
  ctx = &context;
  vfs = &files;
  sprites = &sprite_batch;
  text = &text_batch;
  tables = &game_tables;
  lang = language;
  lt.init(&files);
  if (!texs.init(context, files, sprite_batch, "menu")) return false;
  lowres.init(context, sprite_batch);
  fonts.init(files, text_batch, true);
  select_fonts();
  if (texs.hd()) cursor_tex = texs.load("hd/menu/menutest_cur.png");
  ready = face_gn != render::kNoFace;
  if (!ready) log::error("menu: no usable font");
  return ready;
}

void MainMenu::Impl::select_fonts() {
  const int art_mode = static_cast<int>(mode);
  if (art_mode == fonts_mode || !vfs) return;
  fonts_mode = art_mode;
  const bool classic = mode != ArtMode::Hd;
  fonts.set_classic(classic);
  face_gn = fonts.face(font::kBn);
  face_kb = fonts.face(font::kKb);
  face_ik = lang == import::Lang::Jp ? fonts.face(font::kIk) : render::kNoFace;
  if (!fonts.ps1()) gn = font::load(*vfs, font::member(font::kBn, classic));
}

void MainMenu::Impl::shutdown() {
  texs.shutdown();
  cursor_tex = nullptr;
  lowres.shutdown();
  ctx = nullptr;
}

// ===================================================== text lookups
std::string MainMenu::Impl::menu_str(u32 addr, const char* fallback) {
  if (const std::string* row = lt.menu_text(addr, lang); row && !trim(*row).empty()) return trim(*row);
  return fallback;
}

int MainMenu::Impl::body(int character) const {
  return data::character_body(gs->slot.chars[static_cast<size_t>(character)].model_id, [this](int var) { return gs->slot.var(var); });
}

std::u32string MainMenu::Impl::char_name(int id) {
  if (!gs || id < 0 || id >= save::kNumChars) return default_char_name(id);
  const std::u32string name = data::character_name(id, body(id), data::NameContext::Menu, false, {custom_names, &lt, lang});
  return name.empty() ? default_char_name(id) : name;
}

std::u32string MainMenu::Impl::default_char_name(int id) {
  auto it = name_cache.find(id);
  if (it != name_cache.end()) return it->second;
  std::u32string name = lt.default_name(id, lang);
  if (name.empty()) name = utf32(tables->char_name(id));
  return name_cache[id] = name;
}

std::u32string MainMenu::Impl::item_name(int id) {
  if (id <= 0) return utf32("-");
  std::u32string name = lt.item_name(id, lang);
  return name.empty() ? utf32(tables->item_name(id)) : name;
}

std::u32string MainMenu::Impl::element_name(int id) {
  if (const std::string* row = lt.sysmes((0x17u << 16) | static_cast<u32>(id), lang); row && !row->empty()) return utf32(*row);
  return utf32(tables->element_name(id));
}

std::u32string MainMenu::Impl::key_item_name(int id) {
  if (const std::string* row = lt.sysmes((0x18u << 16) | static_cast<u32>(id), lang); row) return utf32(*row);
  return utf32(id < static_cast<int>(tables->key_item_names.size()) ? tables->key_item_names[static_cast<size_t>(id)] : "?");
}

std::string MainMenu::Impl::table_row(std::string_view table, int key) {
  if (const import::TextTable* rows = lt.table(table))
    if (const std::string* row = rows->get(static_cast<u64>(key), lang)) return *row;
  return {};
}

int MainMenu::Impl::cur_char() const {
  const std::vector<int> members = party();
  return members.empty() ? 0 : members[static_cast<size_t>(member) % members.size()];
}

void MainMenu::Impl::cycle_member(int step) {
  const std::vector<int> members = party();
  if (members.size() < 2) return;
  member = (member + step + static_cast<int>(members.size())) % static_cast<int>(members.size());
  sfx(1);
}

void MainMenu::Impl::sfx(int id) {
  if (sfx_fn) sfx_fn(id);
}

void MainMenu::Impl::say(const std::string& message_text, int duration_frames) {
  msg = message_text;
  msg_frames = duration_frames;
}

// ===================================================== input
Nav MainMenu::Impl::poll(const platform::Input& in) {
  Nav nav;
  using platform::Action;
  auto key_down = [&](Action action) { return platform::action_down(in, action); };
  auto key_pressed = [&](Action action) { return platform::action_pressed(in, action); };
  auto pad_down = [&](int button) { return in.pad_down[static_cast<size_t>(button)]; };
  auto pad_pressed = [&](int button) { return in.pad_pressed[static_cast<size_t>(button)]; };
  const bool dir[4] = {key_down(Action::Up) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_UP), key_down(Action::Down) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_DOWN),
                       key_down(Action::Left) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_LEFT), key_down(Action::Right) || pad_down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)};
  bool fire[4]{};
  repeat.poll(dir, fire);
  nav.up = fire[0], nav.down = fire[1], nav.left = fire[2], nav.right = fire[3];
  nav.held_up = dir[0], nav.held_down = dir[1], nav.held_left = dir[2], nav.held_right = dir[3];
  nav.l2 = key_down(Action::SlowSpeed) || in.pad_axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] >= 0.5f;
  nav.r2 = key_down(Action::FastSpeed) || in.pad_axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] >= 0.5f;
  nav.ok = key_pressed(Action::Ok) || pad_pressed(SDL_GAMEPAD_BUTTON_SOUTH);
  nav.cancel = key_pressed(Action::Cancel) || pad_pressed(SDL_GAMEPAD_BUTTON_EAST);
  nav.tri = key_pressed(Action::Menu) || pad_pressed(SDL_GAMEPAD_BUTTON_NORTH);
  nav.sq = key_pressed(Action::Item) || pad_pressed(SDL_GAMEPAD_BUTTON_WEST);
  nav.l1 = key_pressed(Action::SelectLeft) || pad_pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
  nav.r1 = key_pressed(Action::SelectRight) || pad_pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
  nav.page_up = in.key_pressed[SDL_SCANCODE_PAGEUP];
  nav.page_down = in.key_pressed[SDL_SCANCODE_PAGEDOWN];
  nav.home = in.key_pressed[SDL_SCANCODE_HOME];
  nav.end = in.key_pressed[SDL_SCANCODE_END];
  return nav;
}

// ===================================================== open / navigation
void MainMenu::Impl::open(GameState& state, bool allow_save) {
  gs = &state;
  saved = false;
  can_save = allow_save;
  save_only = false;
  limited = false;
  closed = false;
  screen = MenuScreen::Main;
  sub = 0;
  main_cur = gs->option(opt::kCursorMemory) ? mem_cur : 0;
  // A remembered OpenCross row that is gone now: the row in its place, Save/Load.
  if (main_cur == kOcRow && !oc_row()) main_cur = 5;
  back_to_oc = false;
  member = pick_cur = 0;
  roster = Roster::Off;
  roster_anim = roster_top = roster_row = roster_no = 0;
  roster_ask = false;
  stand_in = -1;
  focus = 0;
  prompt = GridPrompt::None;
  msg.clear();
  frames = 0;
  transition = Transition::None, transition_t = 0;
  hand_list = -1, hand_settled = 0, hand_step = 0, hand_angle = table_angle(0, 0);
  repeat.reset();
}

// Opens a screen: from the main list, the Use / Allocate list or an OpenCross list through the open transition, else at once (the lists
// themselves switch at once).
void MainMenu::Impl::enter(MenuScreen target) {
  const bool from_list = screen == MenuScreen::Main || screen == MenuScreen::ElementChoice || screen == MenuScreen::OpenCross;
  if (from_list && target != MenuScreen::ElementChoice && target != MenuScreen::OpenCross) {
    transition = Transition::Open, transition_t = 0, open_target = target;
    portraits_away = target == MenuScreen::Status || target == MenuScreen::ElementGrid || target == MenuScreen::Equip;
    return;
  }
  show(target);
}

// Switches to a screen with its cursors at their start positions.
void MainMenu::Impl::show(MenuScreen target) {
  screen = target;
  sub = 0;
  msg.clear();
  switch (target) {
    case MenuScreen::Status: profile = true, restart_model(); break;
    case MenuScreen::ElementChoice: choice_cur = 0; break;
    case MenuScreen::ElementUse: use = UseFlow{}; break;
    case MenuScreen::ElementGrid: grid_reset(); break;
    case MenuScreen::Equip: eslot = 0, eq_cur = eq_scroll = 0, eq_list = false; break;
    case MenuScreen::KeyItems: ki_cur = ki_scroll = 0; break;
    case MenuScreen::Settings: set_cur = 0, skin_pick = false, pad_state = 0; break;
    case MenuScreen::Save: save_cur = 0, load_cur = 1, top_cur = can_save ? 0 : 1, save_phase = 0, save_card = 1, load_mode = false, save_confirm = false, load_previews(); break;
    case MenuScreen::OpenCross: oc_cur = oc_page_cur = 0, oc_group.reset(); break;
    case MenuScreen::OcPage: enter_page(); break;
    default: break;
  }
}

// Returns to the main list: from a screen through the back transition, from the Use / Allocate or an OpenCross list at once.
void MainMenu::Impl::back_to_main(int cursor, bool to_pick) {
  back_to_oc = false;
  if (screen != MenuScreen::Main && screen != MenuScreen::ElementChoice && screen != MenuScreen::OpenCross) {
    transition = Transition::Back, transition_t = 0, back_cursor = cursor, back_to_pick = to_pick;
    return;
  }
  land_on_main(cursor, to_pick);
}

// A screen opened from the roster returns to the roster (same row), not to the main list; an OpenCross page returns to the OpenCross list
// it was opened from.
void MainMenu::Impl::land_on_main(int cursor, bool to_pick) {
  screen = back_to_oc ? MenuScreen::OpenCross : MenuScreen::Main;
  back_to_oc = false;
  sub = 0;
  main_cur = cursor;
  if (to_pick) sub = 1, pick_cur = member;
  if (stand_in >= 0) sub = 1, roster = Roster::On, roster_ask = false;
  stand_in = -1;
  msg.clear();
}

// ===================================================== update
// The menu loop runs once per 60 Hz vsync: the animations (pointer roll, cursor pulse, needle) count sim ticks, not frames.
// The first 4 ticks after opening ignore input. Pad input selects the pad button glyphs, any key event the keyboard key caps.
void MainMenu::Impl::update(const platform::Input& in, int ticks) {
  if (closed || !gs || !ready) return;
  frames += std::max(ticks, 0);
  {
    bool any_pad = false, any_key = false;
    for (bool pressed : in.pad_pressed) any_pad = any_pad || pressed;
    for (bool pressed : in.key_pressed) any_key = any_key || pressed;
    if (any_pad) kb_mode = false;
    if (any_key) kb_mode = true;
  }
  if (msg_frames > 0 && --msg_frames == 0) msg.clear();
  if (chime_frames > 0 && (chime_frames -= std::max(ticks, 0)) <= 0) {
    chime_frames = 0;
    end_saving();
  }
  roster_anim = std::max(0, roster_anim - std::max(ticks, 0));
  if (transition != Transition::None) {
    transition_t += std::max(ticks, 0);
    if (transition_t > kTransitionTicks) {
      const Transition done = transition;
      transition = Transition::None;
      if (done == Transition::Back) land_on_main(back_cursor, back_to_pick);
      else show(open_target);
    }
    return;
  }
  const Nav nav = poll(in);
  if (screen == MenuScreen::Status && model_view) step_model_camera(nav, std::max(ticks, 0));
  if (frames < 4) return;
  switch (screen) {
    case MenuScreen::Main: update_main(nav); break;
    case MenuScreen::Status: update_status(nav); break;
    case MenuScreen::ElementChoice: update_choice(nav); break;
    case MenuScreen::ElementUse: update_use(nav); break;
    case MenuScreen::ElementGrid: update_grid(nav); break;
    case MenuScreen::Equip: update_equip(nav); break;
    case MenuScreen::KeyItems: update_keyitems(nav); break;
    case MenuScreen::Settings: update_settings(nav, in); break;
    case MenuScreen::Save: update_save(nav); break;
    case MenuScreen::OpenCross: update_oc_list(nav); break;
    case MenuScreen::OcPage: update_page(nav); break;
  }
  // the needle eases once per frame on the screens that draw a dial (main list, Use / Allocate toggle, the OpenCross lists, Save / Load list)
  if (screen == MenuScreen::ElementChoice || allocate_pick()) hand_tick(1, choice_cur, table_angle(1, choice_cur));
  else if (screen == MenuScreen::OpenCross) hand_tick(oc_group ? 5 : 4, oc_slot(oc_list_cur()), table_angle(0, oc_slot(oc_list_cur())));
  else if (screen == MenuScreen::Main) hand_tick(0, main_pos(main_cur), table_angle(0, main_pos(main_cur)));
  else if (screen == MenuScreen::Save) hand_tick(3, top_cur, save_list_angle(top_cur));
}

// Needle angles per list (4096 = 2 pi): 0 main (by slot; slot 6 = the seventh row a shown OpenCross row adds, pointing at the ring where
// that row's leader meets it), 1 Use / Allocate, 2 popup, 3 Save / Load. The OpenCross lists (4 the groups, 5 a group's pages) point like
// the main list's slots.
int MainMenu::Impl::table_angle(int list, int sel) {
  static const int kAngle[4][7] = {{-576, -768, -960, -1104, -1248, -1434, -1600},
                                   {-576, -960, 0, 0, 0, 0, 0},
                                   {-1104, -1248, -1434, 0, 0, 0, 0},
                                   {-768, -1104, 0, 0, 0, 0, 0}};
  return kAngle[std::clamp(list, 0, 3)][std::clamp(sel, 0, 6)];
}

// When the list changes the needle snaps to entry 0's angle; while the selection differs from the settled one,
// angle += ((target - angle) / 10) * step with step counting 0..10 once per frame (integer division: 10 %, 20 % ... of the remaining
// way, the 11th frame lands exactly), then the selection settles. A selection settled earlier is drawn at its exact angle.
void MainMenu::Impl::hand_tick(int list, int sel, int target) {
  if (list != hand_list) hand_angle = list == 3 ? save_list_angle(0) : list >= 4 ? table_angle(0, oc_slot(0)) : table_angle(list, 0), hand_list = list;
  if (sel == hand_settled) {
    hand_angle = target;
    return;
  }
  hand_angle += ((target - hand_angle) / 10) * hand_step;
  if (hand_step < 10) ++hand_step;
  else hand_step = 0, hand_settled = sel;
}

void MainMenu::Impl::scroll_to(int cur, int& scroll, int rows) {
  if (cur < scroll) scroll = cur;
  if (cur >= scroll + rows) scroll = cur - rows + 1;
}

// ================================================================================================================
MainMenu::MainMenu() : impl_(std::make_unique<Impl>()) {}
MainMenu::~MainMenu() { shutdown(); }

bool MainMenu::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text,
                    const data::GameTables& tables, import::Lang lang) {
  return impl_->init(ctx, vfs, sprites, text, tables, lang);
}
void MainMenu::shutdown() {
  if (impl_ && impl_->ctx) impl_->shutdown();
}
void MainMenu::set_model_view(MenuModelView* view) { impl_->model_view = view; }
void MainMenu::set_rules(const MenuRules* rules) { impl_->rules = rules; }
void MainMenu::set_custom_names(const std::map<int, std::u32string>* names) { impl_->custom_names = names; }
void MainMenu::set_saved_name_lookup(SavedNameLookup lookup) { impl_->saved_name = std::move(lookup); }
void MainMenu::set_mode(ArtMode mode) { impl_->mode = mode; }
ArtMode MainMenu::mode() const { return impl_->mode; }
void MainMenu::set_language(import::Lang lang) {
  impl_->lang = lang;
  impl_->name_cache.clear();
  // JP adds the ik_70 face.
  impl_->fonts_mode = -1;
}
void MainMenu::set_portrait_set(const std::string& set) { impl_->portrait_set = set == "1999" ? "1999" : "2021"; }
void MainMenu::set_save_target(save::SaveDat* dat, std::filesystem::path out, save::OcSaves* page) {
  impl_->save_dat = dat;
  impl_->page = page;
  impl_->save_out = std::move(out);
}
void MainMenu::set_sfx(std::function<void(int)> fn) { impl_->sfx_fn = std::move(fn); }
void MainMenu::set_sfx_layer(std::function<void(int, u32)> fn) { impl_->sfx_layer_fn = std::move(fn); }
void MainMenu::set_save_locked(bool on) { impl_->save_locked = on; }
void MainMenu::set_wide_columns(float canvas_columns, float layout_columns) {
  impl_->canvas_cols = std::max(0.0f, canvas_columns);
  impl_->wide_cols = std::clamp(layout_columns, 0.0f, impl_->canvas_cols);
}
bool MainMenu::save_locked() const { return impl_->save_locked; }
void MainMenu::set_oc_page(OcPage page, bool on) {
  const u32 bit = 1u << static_cast<int>(page);
  impl_->oc_pages_on = on ? impl_->oc_pages_on | bit : impl_->oc_pages_on & ~bit;
}
void MainMenu::set_collection(const save::Collection* collection) { impl_->collection = collection; }
void MainMenu::set_records_show_all(bool on) { impl_->records_show_all = on; }
void MainMenu::set_grid_presets(save::Collection* collection, std::function<void()> changed) {
  impl_->preset_store = collection;
  impl_->preset_changed = std::move(changed);
}
void MainMenu::set_enemy_view(EnemyModelView* view) { impl_->enemy_view = view; }
void MainMenu::set_spell_view(SpellView* view) { impl_->spell_view = view; }
void MainMenu::set_enemy_order(const data::BestiaryOrder* order) { impl_->enemy_order = order; }
void MainMenu::set_records_info(const RecordsInfo* info) { impl_->records_info = info; }
void MainMenu::set_records_hints(int level) { impl_->records_hints = level; }
void MainMenu::set_dialogue_log_view(DialogueLogView* view) { impl_->log_view = view; }
void MainMenu::set_extras(const ExtrasCatalog* catalog, ExtrasHost* host) {
  impl_->extras_catalog = catalog;
  impl_->extras_host = host;
}
void MainMenu::set_extras_unlock_all(bool on) { impl_->extras_unlock_all = on; }
void MainMenu::set_achievements(const AchievementList* list) { impl_->achievements = list; }
void MainMenu::set_achievement_settings(bool on, bool hide_notices) {
  impl_->achievements_on = on;
  impl_->hide_achievement_notices = hide_notices;
}
void MainMenu::set_oc_options_host(std::function<void(const OcOptions&)> changed, std::function<void()> reset) {
  impl_->oc_options_changed = std::move(changed);
  impl_->oc_reset = std::move(reset);
}
void MainMenu::open(GameState& state, bool can_save) { impl_->open(state, can_save); }
void MainMenu::open_limited(GameState& state) {
  Impl& impl = *impl_;
  impl.open(state, false);
  impl.limited = true;
  if (!impl.main_row_open(impl.main_cur)) impl.main_cur = 1;
}
void MainMenu::open_save_list(GameState& state) {
  Impl& impl = *impl_;
  impl.open(state, true);
  impl.show(MenuScreen::Save);
  impl.save_only = true;
  impl.save_phase = 2;
  impl.save_card = impl.page && !impl.page->empty() ? 0 : 1;
}
void MainMenu::close() { impl_->closed = true; }
bool MainMenu::closed() const { return impl_->closed; }
bool MainMenu::saved() const { return impl_->saved; }
MenuScreen MainMenu::screen() const { return impl_->screen; }
bool MainMenu::camera_takes_triggers() const {
  return !impl_->closed && impl_->screen == MenuScreen::Status && impl_->model_view && impl_->model_cam_manual;
}
bool MainMenu::extras_shown() const {
  return !impl_->closed && impl_->screen == MenuScreen::OcPage && impl_->shown_def().enter == &Impl::enter_extras;
}
bool MainMenu::pad_page_open() const { return !impl_->closed && impl_->screen == MenuScreen::Settings && impl_->pad_state != 0; }
std::optional<MainMenu::LoadRequest> MainMenu::take_load_request() {
  Impl& impl = *impl_;
  if (!impl.load_pending) return std::nullopt;
  impl.load_pending = false;
  if (impl.load_card == 0) {
    const save::OcSaves::File* file = impl.page ? impl.page->file(impl.load_slot) : nullptr;
    if (!file) return std::nullopt;
    LoadRequest request;
    request.card = 0;
    request.slot = impl.load_slot;
    request.data = file->data;
    return request;
  }
  const save::MemCard* card = impl.save_dat ? impl.save_dat->card(impl.load_card) : nullptr;
  if (!card) return std::nullopt;
  try {
    LoadRequest request;
    request.card = impl.load_card;
    request.slot = impl.load_slot;
    request.block = impl.load_block;
    request.data = save::read_slot(*card, impl.load_block);
    return request;
  } catch (const std::exception& error) {
    log::warn("menu: load slot {}: {}", impl.load_slot, error.what());
    return std::nullopt;
  }
}
void MainMenu::update(const platform::Input& in, int ticks) {
  impl_->pad_present = in.pad_connected;
  impl_->update(in, ticks);
}
void MainMenu::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  impl.draw(frame, vw, vh, resume);
  // The DialogueLog page: the log window over the menu's picture, at window resolution.
  if (!impl.closed && impl.log_view && impl.screen == MenuScreen::OcPage && impl.shown_def().page == OcPage::DialogueLog) impl.log_view->draw(frame, vw, vh);
}

void MainMenu::debug_show(MenuScreen screen, int variant) {
  Impl& impl = *impl_;
  if (!impl.gs) return;
  impl.member = 0;
  impl.sub = 0;
  impl.roster = Impl::Roster::Off;
  impl.roster_ask = false;
  impl.stand_in = -1;
  impl.save_only = false;
  impl.msg.clear();
  impl.show(screen);
  impl.frames = 10;
  impl.model_clock = {};
  if (screen == MenuScreen::ElementGrid) {
    impl.list_cur = std::min(2, static_cast<int>(impl.grid_list().size()) - 1);
    // Variant 1: the listed element picked, waiting for a cell; 2: the Auto Allocate question (list rows switched by Right); 3: the
    // Remove All question; 4: Reorder.
    if (variant == 1) impl.pick_element = true, impl.to_grid(impl.cell_row, 0, false);
    if (variant == 2) impl.list_cur = 0, impl.label_page = 1, impl.prompt = GridPrompt::AutoAllocate;
    if (variant == 3) impl.list_cur = 0, impl.prompt = GridPrompt::RemoveAll, impl.prompt_cur = 1;
    if (variant == 4) impl.list_cur = 0, impl.prompt = GridPrompt::Sort;
    impl.grid_unlock = 0;
  }
  if (screen == MenuScreen::ElementUse && variant >= 1) {
    impl.use.target = true;
    impl.use.roster = variant == 2;
    impl.use.roster_row = 2;
    impl.use.stand_in = variant == 2 ? 2 : -1;
    impl.stand_in = impl.use.stand_in;
  }
  // Equip variant 1: the weapon list on row 1; 2: the first accessory slot's list on row 1.
  if (screen == MenuScreen::Equip && variant >= 1) impl.eq_list = true, impl.eq_cur = 1, impl.eslot = variant == 2 ? 2 : 0;
  if (screen == MenuScreen::Main && variant == 1) impl.sub = 1;
  // Variants 2 / 3 of Main: the roster from the Status pick, and from the Equip pick with the remove-all prompt.
  if (screen == MenuScreen::Main && (variant == 2 || variant == 3)) {
    impl.sub = 1;
    impl.pick_for = variant == 3 ? 2 : 0;
    impl.main_cur = impl.pick_for;
    impl.roster = Impl::Roster::On;
    impl.roster_ask = variant == 3;
  }
  if (screen == MenuScreen::Status && variant == 1) impl.profile = false;
  // Variant 1 of Settings: the Custom Window Frame picker.
  if (screen == MenuScreen::Settings && variant == 1) impl.set_cur = 1, impl.skin_pick = true;
  // Variants 2 / 3 of Settings: the Controller assignment page, field pair and battle pair.
  if (screen == MenuScreen::Settings && (variant == 2 || variant == 3)) {
    impl.gs->set_option(opt::kControllerEdit, true);
    impl.set_cur = 4, impl.pad_state = variant;
  }
  if (screen == MenuScreen::Save && variant != 4) impl.can_save = true;
  if (screen == MenuScreen::Save && variant >= 1 && variant <= 3) {
    impl.save_phase = 1;
    impl.load_mode = variant == 2;
    for (int k = 1; k < 15; ++k)
      if (impl.prev[static_cast<size_t>(k)].used) {
        if (impl.load_mode) impl.load_cur = k;
        else if (variant == 3) impl.save_cur = k - 1;
        break;
      }
    impl.save_confirm = variant == 3;
    impl.confirm_cur = 0;
  }
  if (screen == MenuScreen::Save && variant == 4) impl.can_save = false, impl.top_cur = 1;
  if (screen == MenuScreen::OpenCross) impl.main_cur = Impl::kOcRow, impl.oc_cur = std::clamp(variant, 0, std::max(0, static_cast<int>(impl.oc_labels().size()) - 1));
  if (screen == MenuScreen::OcPage) {
    impl.oc_shown = std::clamp(variant, 0, Impl::kOcPageCount - 1);
    impl.enter_page();
  }
}

}  // namespace oc::ui
