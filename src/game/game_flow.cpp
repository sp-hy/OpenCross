#include "game/game_flow.h"

#include <algorithm>
#include <random>

#include "core/log.h"
#include "game/new_game.h"
#include "game/new_game_plus.h"
#include "render/camera.h"
#include "render/sprite_batch.h"
#include "vfs/vfs.h"

namespace oc::game {

const char* flow_state_name(FlowState state) {
  switch (state) {
    case FlowState::Title: return "title";
    case FlowState::SlotSelect: return "slots";
    case FlowState::NameEntry: return "name";
    case FlowState::Opening: return "opening";
    case FlowState::Field: return "field";
  }
  return "?";
}

bool parse_flow_start(std::string_view text, std::optional<FlowState>& out, bool& enabled) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  enabled = true;
  out.reset();
  if (lowered.empty() || lowered == "field" || lowered == "off" || lowered == "none") {
    enabled = false;
    return true;
  }
  if (lowered == "title") out = FlowState::Title;
  else if (lowered == "name" || lowered == "nameentry") out = FlowState::NameEntry;
  else if (lowered == "slots" || lowered == "continue") out = FlowState::SlotSelect;
  else if (lowered == "opening") out = FlowState::Opening;
  else {
    enabled = false;
    return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------ FlowMachine
FlowMachine::FlowMachine() { gs_.rules = &rules_; }

void FlowMachine::set_tables(const data::GameTables* tables, const ui::GameFontCodec* codec) {
  tables_ = tables;
  codec_ = codec;
  rules_.tables = tables;
}

void FlowMachine::set_custom_name(int char_id, std::u32string name) {
  custom_names_[char_id] = std::move(name);
  if (codec_ && char_id >= 0 && char_id < save::kNumChars)
    gs_.slot.chars[static_cast<size_t>(char_id)].name = codec_->pack_record(custom_names_[char_id]);
}

// The title script (opening.bin 0x2C1..0x2D8): ADD_TO_PARTY #0, #1, then RAND_MOD 14 -> ARRAY8_GET of the code table at 0x310 (bytes
// 2A 04 0A 0C 0E 0F 11 14 15 1E 21 22 24 27) -> ADD_TO_PARTY, then MAP_JUMP_DC #22. The opening scene (opn_a01i) needs the third
// member: its first cutscene starts when PC2 has walked in.
static constexpr int kOpeningThird[14] = {42, 4, 10, 12, 14, 15, 17, 20, 21, 30, 33, 34, 36, 39};

save::SaveSlot FlowMachine::new_game_slot(const data::GameTables* tables, const ui::GameFontCodec* codec, std::u32string_view name, u32 seed,
                                       bool opening_party) {
  const u32 rnd = seed ? seed * 2654435761u >> 7 : std::random_device{}();
  // The boot / title state (game/new_game.h): every character record from char20.bin, 20 stars, growth, star unlocks, gold 100,
  // options 2, plus the Time Shifter, frames 0x203, variables 0..0x3CF = 0 and 0x3D0.. from the exe table. The title room's FE 2F
  // rebuilds it when the title is re-entered.
  save::SaveSlot slot = newgame::boot_state(tables, rnd | 1u);
  if (opening_party) {
    // The title script after the name entry: ADD_TO_PARTY #0, #1, then RAND_MOD 14 -> the code table -> ADD_TO_PARTY (recruited too).
    const int third = kOpeningThird[rnd % 14];
    for (int id : {0, 1, third}) slot.set_recruited(id, true);
    slot.set_party({0, 1, static_cast<u8>(third)});
  } else {
    // Dev start straight into a field room (no title, no dream): the state FE 3F leaves (frames 0x203).
    newgame::real_records(tables, slot, rnd | 1u);
    newgame::reset_party(slot);
  }
  slot.head[save::head::kLocation] = static_cast<u8>(kNewGameRoomId & 0xFF);
  slot.head[save::head::kLocation + 1] = static_cast<u8>(kNewGameRoomId >> 8);
  if (codec) slot.chars[0].name = codec->pack_record(name);
  return slot;
}

bool FlowMachine::usable_name(const std::u32string& name, import::Lang lang) {
  if (name.empty() || name.find(U'�') != std::u32string::npos || name.find_first_not_of(U"　 ") == std::u32string::npos) return false;
  if (lang != import::Lang::Jp)
    for (char32_t ch : name)
      // Kana / JP symbols: the non-JP fonts have no glyphs.
      if (ch >= 0x3000) return false;
  return true;
}

void FlowMachine::set_language(import::Lang lang) {
  lang_ = lang;
  for (auto it = custom_names_.begin(); it != custom_names_.end();)
    it = usable_name(it->second, lang_) ? std::next(it) : custom_names_.erase(it);
}

void FlowMachine::new_game(std::u32string_view name, bool opening_party) {
  const std::u32string v = ui::validate_name(name);
  custom_names_.clear();
  custom_names_[0] = v.empty() ? std::u32string(name) : v;
  gs_ = ui::GameState(new_game_slot(tables_, codec_, custom_names_[0], seed_, opening_party), &rules_);
  gs_.set_option(ui::opt::kVibration, vibration_);
}

FlowAction FlowMachine::on_title(ui::TitleChoice choice) {
  FlowAction action;
  if (state_ != FlowState::Title) return action;
  switch (choice) {
    case ui::TitleChoice::NewGame:
      state_ = FlowState::NameEntry;
      action.stop_music = true;
      break;
    case ui::TitleChoice::Continue:
      if (has_saves_) state_ = FlowState::SlotSelect;
      break;
    case ui::TitleChoice::Settings: action.open_settings = true; break;
    case ui::TitleChoice::Quit: action.quit = true; break;
    case ui::TitleChoice::None: break;
  }
  return action;
}

FlowAction FlowMachine::on_name(std::u32string_view name) {
  FlowAction action;
  if (state_ != FlowState::NameEntry) return action;
  const std::u32string validated = ui::validate_name(name);
  if (validated.empty()) return action;
  new_game(validated);
  state_ = FlowState::Opening;
  action.load_room = true;
  action.room = kNewGameRoom;
  action.param = kNewGameParam;
  action.state_changed = true;
  action.stop_music = true;
  // Title script: MAP_JUMP_DC #22, then D4 #2 + WAIT_MOVIE; the jump happens when the script ends.
  action.movie_row = kNewGameMovieRow;
  return action;
}

FlowAction FlowMachine::on_slot(const ui::SlotEntry& entry) {
  if (state_ != FlowState::SlotSelect) return {};
  return on_load(entry);
}

// Starts a saved slot: the title's Continue (SlotSelect) and the in-game menu's Save/Load > Load (any state) both end here.
FlowAction FlowMachine::on_load(const ui::SlotEntry& entry) {
  FlowAction action;
  gs_ = ui::GameState(entry.data, &rules_);
  // Every load grants the Time Shifter key item.
  newgame::ensure_time_shifter(gs_.slot);
  custom_names_.clear();
  if (codec_)
    for (int char_id = 0; char_id < save::kNumChars; ++char_id) {
      // Records of characters that were never named hold blanks / padding (the RDE names live host-side): a name made only of
      // blanks or undecodable codes is "unset" and the default sysmes name applies.
      std::u32string name = ui::trim_blanks(codec_->unpack_record(entry.data.chars[static_cast<size_t>(char_id)].name));
      if (usable_name(name, lang_)) custom_names_[char_id] = std::move(name);
    }
  // ext_save.dat names: what the player typed (the record only holds what the game font can show).
  for (const auto& [char_id, name] : ext_names_)
    if (char_id >= 0 && char_id < save::kNumChars && usable_name(name, lang_)) {
      custom_names_[char_id] = name;
      if (codec_) gs_.slot.chars[static_cast<size_t>(char_id)].name = codec_->pack_record(name);
    }
  ext_names_.clear();
  state_ = FlowState::Field;
  action.state_changed = true;
  action.stop_music = true;
  // The location word is the room id (0xFC/0xFD = the wld_a01 / wld_b01 world map rooms).
  const char* room = import::room_name_for_id(entry.data.location());
  if (room || entry.data.location() > 0) {
    action.load_room = true;
    action.room = room ? room : "";
    action.room_id = entry.data.location();
    // ROOM_PARAM of a loaded slot = its variable 0x3A. Room init copies var 2 into 0x3A and save events overwrite it with the
    // entrance they resume at (gal_b09i: `SET $3A, #4` before the save menu, so a Continue re-enters with param 4 and the wake-up
    // event runs on).
    action.param = static_cast<u16>(entry.data.head[0x188 + 0x3A] | (entry.data.head[0x189 + 0x3A] << 8));
    action.restore_position = true;
    // The leader's position is the engine-written variables 0x12 (X) / 0x14 (Z) of the saved global block (head 0x188 + id); the
    // word pair at head 0x488 is the unrelated variable 0x300 / 0x302 (it lies off the walk mesh).
    const auto var = [&](int id) { return static_cast<s16>(entry.data.head[0x188 + static_cast<size_t>(id)] | (entry.data.head[0x189 + static_cast<size_t>(id)] << 8)); };
    action.x = var(0x12);
    action.z = var(0x14);
    log::info("flow: slot {} room {} ({}) position {}, {}  scenario {}", entry.slot, room ? room : "?", entry.data.location(), action.x, action.z, var(0));
  } else {
    log::warn("flow: slot {} has unknown location {}", entry.slot, entry.data.location());
  }
  return action;
}

void FlowMachine::new_game_plus(const save::SaveSlot& cleared) {
  gs_ = ui::GameState(ngplus::new_game_plus_slot(tables_, cleared), &rules_);
  gs_.set_option(ui::opt::kVibration, vibration_);
  log::info("flow: New Game+: gold {}, stars {}, carry-over bitmap {:08X}:{:03X}", gs_.slot.gold(), gs_.slot.stars_total(), gs_.slot.unk_e1ec,
            gs_.slot.unk_e1f0);
}

FlowAction FlowMachine::on_continue_plus(const ui::SlotEntry& entry, const save::SaveSlot& cleared) {
  ui::SlotEntry plus = entry;
  ngplus::continue_plus(cleared, plus.data, tables_);
  std::map<int, std::u32string> names = custom_names_;
  ext_names_.clear();
  FlowAction action = on_load(plus);
  custom_names_ = std::move(names);
  log::info("flow: Continue+ slot {}: carried in, gold {}", entry.slot, gs_.slot.gold());
  return action;
}

FlowAction FlowMachine::on_slot_cancel() {
  if (state_ == FlowState::SlotSelect) state_ = FlowState::Title;
  return {};
}

FlowAction FlowMachine::on_field(std::string_view room) {
  if (state_ == FlowState::Opening && !room.starts_with("opn_") && !room.empty()) state_ = FlowState::Field;
  return {};
}

// ------------------------------------------------------------------------------------------------ GameFlow
GameFlow::GameFlow() = default;
GameFlow::~GameFlow() { shutdown(); }

bool GameFlow::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, const data::GameTables* tables,
                    import::Lang lang, audio::Mixer* mixer) {
  vfs_ = &vfs;
  lang_ = lang;
  machine_.set_language(lang);
  mixer_ = mixer;
  if (!kit_.init(ctx, vfs, sprites)) return false;
  list_ = std::make_unique<ui::DrawList>(kit_.text());
  codec_ = std::make_unique<ui::GameFontCodec>(kit_.glyphs());
  lang_tables_.init(&vfs);
  machine_.set_tables(tables, codec_.get());
  if (tables) machine_.rules().load_pg(vfs);
  title_.init(kit_, mixer);
  slots_.set_name_lookup([this](int id) { return lang_tables_.default_name(id, lang_); });
  ready_ = true;
  return true;
}

void GameFlow::shutdown() {
  if (!ready_) return;
  ready_ = false;
  title_.shutdown();
  list_.reset();
  kit_.shutdown();
}

void GameFlow::set_art_mode(ui::ArtMode mode) {
  mode_ = mode;
  title_.set_mode(mode);
  slots_.set_mode(mode);
}

void GameFlow::set_language(import::Lang lang) {
  lang_ = lang;
  machine_.set_language(lang);
  title_.set_language(lang);
}

void GameFlow::set_sfx(std::function<void(int)> fn) {
  sfx_ = std::move(fn);
  title_.set_sfx(sfx_);
  name_.set_sfx(sfx_);
  slots_.set_sfx(sfx_);
}

void GameFlow::enter_title(bool fresh) {
  const bool any_save = ui::SlotList::any(dat_, page_);
  machine_.set_save_available(any_save);
  machine_.start(FlowState::Title);
  title_.begin(lang_, mode_, any_save, title_song_, scene_enabled_);
  // The logo FMV plays first.
  if (scene_enabled_ && fresh) scene_request_ = true, logo_ready_ = false;
}

void GameFlow::enter_slots() {
  machine_.start(FlowState::SlotSelect);
  slots_.begin(dat_, page_, lang_, mode_);
}

void GameFlow::enter_keymap() {
  // RDE: the title's start-game op shows the user guide (controls / key map, two pages) before the name entry (keymap_screen.h).
  machine_.start(FlowState::NameEntry);
  keymap_active_ = keymap_.begin(kit_, lang_);
  if (!keymap_active_) enter_name();
}

void GameFlow::enter_name() {
  machine_.start(FlowState::NameEntry);
  keymap_active_ = false;
  ui::NameEntry::Setup setup;
  setup.char_id = 0;
  setup.lang = lang_;
  setup.mode = mode_;
  setup.default_name = lang_tables_.default_name(0, lang_);
  setup.glyphs = &kit_.glyphs();
  name_.begin(setup);
}

void GameFlow::begin_rename(int char_id, bool guide) {
  if (!ready_) return;
  rename_active_ = true;
  rename_finished_ = false;
  rename_guide_ = guide && keymap_.begin(kit_, lang_);
  rename_char_ = char_id;
  ui::NameEntry::Setup setup;
  setup.char_id = char_id;
  setup.lang = lang_;
  setup.mode = mode_;
  setup.default_name = lang_tables_.default_name(char_id, lang_);
  const auto& cn = machine_.custom_names();
  if (auto it = cn.find(char_id); it != cn.end()) setup.initial = it->second;
  setup.glyphs = &kit_.glyphs();
  name_.begin(setup);
}

std::u32string GameFlow::display_name(int char_id) {
  const auto& cn = machine_.custom_names();
  if (auto it = cn.find(char_id); it != cn.end() && !it->second.empty()) return it->second;
  return lang_tables_.default_name(char_id, lang_);
}

void GameFlow::begin_ng_plus(const save::SaveSlot& cleared) {
  if (!ready_) return;
  ngp_cleared_ = cleared;
  ngp_active_ = true;
  ngp_slots_ = ngp_load_ = false;
  ngp_result_ = NgPlusResult::None;
  title_.begin_plus(lang_, mode_);
  log::info("flow: New Game+ title list (variant 2), cleared game flags {}", cleared.summary_flags());
}

void GameFlow::begin_party() {
  if (!ready_) return;
  const ui::GameState& gs = machine_.game_state();
  std::array<int, 3> party_ids{-1, -1, -1};
  const std::vector<int> cur = gs.party();
  for (size_t i = 0; i < cur.size() && i < 3; ++i) party_ids[i] = cur[i];
  party_editor_.begin(gs.roster(), party_ids);
  party_active_ = true;
  party_finished_ = false;
}

void GameFlow::start(FlowState state) {
  if (!ready_) return;
  switch (state) {
    case FlowState::Title: enter_title(); break;
    case FlowState::SlotSelect: enter_slots(); break;
    case FlowState::NameEntry: enter_name(); break;
    case FlowState::Opening:
    case FlowState::Field: machine_.start(state); break;
  }
}

bool GameFlow::any_button(const platform::Input& in) {
  // Not the hotkeys (F-keys, the stick clicks) / the settings key Esc: opening the settings window over the attract demo must not end it.
  // The pad's Start counts on its press (holding it on for the settings window has ended the demo already); Back is the PS1 Select.
  for (int i = 0; i < platform::Input::kButtons; ++i)
    if (in.pad_pressed[static_cast<size_t>(i)] && i != SDL_GAMEPAD_BUTTON_LEFT_STICK && i != SDL_GAMEPAD_BUTTON_RIGHT_STICK) return true;
  for (size_t i = 0; i < in.key_pressed.size(); ++i)
    if (in.key_pressed[i] && (i < SDL_SCANCODE_F1 || i > SDL_SCANCODE_F12) && i != SDL_SCANCODE_ESCAPE) return true;
  return false;
}

// Retail has no explicit load fade in the title/menu code (the field framework fades the screen around the map jump); the length
// is the title's own: 0.5 s (30 frames) each way, the same as the title screen's fades.
namespace {
constexpr float kLoadFade = 0.5f;
// Opening the field menu: a quick fade to black, and the menu comes in faster still.
constexpr float kMenuOutFade = 0.2f, kMenuInFade = 0.1f;
}

void GameFlow::begin_load_fade(const ui::SlotEntry& entry, bool from_slot_list) {
  load_entry_ = entry;
  load_from_slots_ = from_slot_list;
  load_pending_ = true;
  veil_ = std::max(veil_, 0.0f);
}

FlowAction GameFlow::load_slot(const ui::SlotEntry& entry) {
  if (!ready_) return {};
  begin_load_fade(entry, false);
  return {};
}

FlowAction GameFlow::tick_fade(float dt) {
  FlowAction action;
  if (!ready_) return action;
  // A long frame (the room load) must not eat the fade.
  dt = std::clamp(dt, 0.0f, 0.05f);
  if (open_pending_) {
    veil_ = std::min(1.0f, veil_ + dt / kMenuOutFade);
    if (veil_ >= 1.0f) {
      open_pending_ = false;
      open_ready_ = true;
      quick_in_ = true;
      log::info("fade: menu-open fade-out finished");
    }
  } else if (load_pending_) {
    veil_ = std::min(1.0f, veil_ + dt / kLoadFade);
    if (veil_ >= 1.0f) {
      load_pending_ = false;
      // Continue+: the slot plus the cleared game.
      if (ngp_load_) {
        ngp_load_ = ngp_active_ = ngp_slots_ = false;
        ngp_result_ = NgPlusResult::ContinuePlus;
        action = machine_.on_continue_plus(load_entry_, ngp_cleared_);
        load_entry_ = {};
        return action;
      }
      // ext_save.dat: custom names (same language only) + the battle counter.
      if (load_entry_.card == 0 && page_) machine_.set_ext_names(page_->read_ext(load_entry_.slot, static_cast<u32>(lang_)).names);
      else if (dat_) machine_.set_ext_names(dat_->read_ext(load_entry_.card, load_entry_.slot, static_cast<u32>(lang_)).names);
      action = load_from_slots_ ? machine_.on_slot(load_entry_) : machine_.on_load(load_entry_);
      load_entry_ = {};
    }
  } else if (veil_ > 0.0f) {
    veil_ = std::max(0.0f, veil_ - dt / (quick_in_ ? kMenuInFade : kLoadFade));
    if (veil_ <= 0.0f) quick_in_ = false;
  }
  return action;
}

void GameFlow::draw_overlay(const render::Frame& frame, float vw, float vh) {
  if (!ready_ || veil_ <= 0.002f) return;
  // The title / load / menu-open fades cover the whole screen in use (widescreen: the widened frame).
  draw_veil(frame, vw, vh, veil_);
}

FlowAction GameFlow::update(const platform::Input& in, float dt) {
  FlowAction action;
  if (!ready_) return action;
  const ui::Nav nav = nav_.poll(in, dt);
  if (party_active_) {
    using Key = PartyEditor::Key;
    int sfx = 0;
    if (nav.left) sfx = party_editor_.press(Key::Left);
    else if (nav.right) sfx = party_editor_.press(Key::Right);
    else if (nav.up) sfx = party_editor_.press(Key::Up);
    else if (nav.down) sfx = party_editor_.press(Key::Down);
    else if (nav.confirm) sfx = party_editor_.press(Key::Confirm);
    else if (nav.cancel) sfx = party_editor_.press(Key::Cancel);
    if (sfx && sfx_) sfx_(sfx);
    if (party_editor_.done()) {
      std::array<u8, 3> ids{0xFF, 0xFF, 0xFF};
      const std::vector<int> result = party_editor_.result();
      for (size_t i = 0; i < result.size() && i < 3; ++i) ids[i] = static_cast<u8>(result[i]);
      machine_.game_state().slot.set_party(ids);
      party_active_ = false;
      party_finished_ = true;
    }
    return action;
  }
  if (rename_active_) {
    if (rename_guide_) {
      keymap_.update(in, dt);
      if (keymap_.done()) rename_guide_ = false;
      return action;
    }
    name_.update(nav, dt);
    if (name_.done()) {
      machine_.set_custom_name(rename_char_, name_.result());
      rename_active_ = false;
      rename_finished_ = true;
    }
    return action;
  }
  if (ngp_active_) {
    if (load_pending_) return action;
    if (!ngp_slots_) {
      title_.update(nav, dt);
      const ui::TitleChoice choice = title_.take_choice();
      if (choice == ui::TitleChoice::NewGame) {
        // The vibration choice was made on the vibration page before the list returned.
        machine_.set_vibration(title_.vibration());
        machine_.new_game_plus(ngp_cleared_);
        ngp_active_ = false;
        ngp_result_ = NgPlusResult::NewGamePlus;
      } else if (choice == ui::TitleChoice::Continue) {
        ngp_slots_ = true;
        slots_.begin(dat_, page_, lang_, mode_, true);
      }
    } else {
      slots_.update(nav, dt);
      if (slots_.picked() && slots_.selected()) {
        ngp_load_ = true;
        begin_load_fade(*slots_.selected(), false);
      } else if (slots_.cancelled()) {
        ngp_slots_ = false;
        title_.begin_plus(lang_, mode_);
        title_.skip_to_menu();
        title_.select(1);
      }
    }
    return action;
  }
  switch (machine_.state()) {
    case FlowState::Title: {
      // A button during the FMV only skips the movie.
      title_.update(logo_ready_ || !scene_enabled_ ? nav : ui::Nav{}, dt);
      const ui::TitleChoice choice = title_.take_choice();
      if (choice == ui::TitleChoice::None) break;
      if (choice == ui::TitleChoice::NewGame) machine_.set_vibration(title_.vibration());
      action = machine_.on_title(choice);
      // New Game: the user guide pages, then the name entry.
      if (machine_.state() == FlowState::NameEntry) enter_keymap();
      else if (machine_.state() == FlowState::SlotSelect) enter_slots();
      break;
    }
    case FlowState::SlotSelect: {
      if (load_pending_) break;
      slots_.update(nav, dt);
      if (slots_.picked() && slots_.selected()) {
        // Fade to black, then on_slot() (tick_fade).
        begin_load_fade(*slots_.selected(), true);
      } else if (slots_.cancelled()) {
        machine_.on_slot_cancel();
        enter_title(false);
        title_.skip_to_menu();
        title_.select(1);
      }
      break;
    }
    case FlowState::NameEntry:
      if (keymap_active_) {
        keymap_.update(in, dt);
        if (keymap_.done()) enter_name();
        break;
      }
      name_.update(nav, dt);
      if (name_.done()) action = machine_.on_name(name_.result());
      break;
    case FlowState::Opening:
    case FlowState::Field: break;
  }
  if (action.stop_music && mixer_) mixer_->stop();
  return action;
}

FlowAction GameFlow::note_field(std::string_view room) { return machine_.on_field(room); }

bool GameFlow::owns_screen() const {
  const FlowState state = machine_.state();
  return ready_ && (rename_active_ || party_active_ || ngp_active_ || state == FlowState::Title || state == FlowState::SlotSelect || state == FlowState::NameEntry);
}

void GameFlow::draw_party(ui::DrawList& dl) {
  using render::pack_rgba;
  const bool hd = mode_ == ui::ArtMode::Hd;
  const render::FaceId face = kit_.face(mode_);
  const float sc = kit_.scale(mode_, 0.62f, 2.0f);
  const render::TexRef white_tex = kit_.white();
  const u32 panel = hd ? pack_rgba(6, 9, 34, 235) : pack_rgba(0, 0, 96, 235);
  const u32 edge = pack_rgba(230, 230, 255), dim = pack_rgba(150, 150, 190), white = pack_rgba(255, 255, 255), gold = pack_rgba(255, 220, 90);
  dl.rect(white_tex, 0, 0, ui::kCanvasW, ui::kCanvasHeight, pack_rgba(0, 0, 24, 255));
  dl.text(face, U"Current Party", 640, 48, white, sc * 1.2f, 1);
  const bool slots = party_editor_.mode() == PartyEditor::Mode::Slots;
  for (int i = 0; i < 3; ++i) {
    const float x = 190.0f + 340.0f * static_cast<float>(i), y = 120.0f, bw = 300.0f, bh = 130.0f;
    const bool sel = i == party_editor_.slot();
    dl.rect(white_tex, x, y, bw, bh, panel);
    const u32 ec = sel ? (slots ? gold : white) : edge;
    const float thickness = sel ? 6.0f : 3.0f;
    dl.rect(white_tex, x, y, bw, thickness, ec);
    dl.rect(white_tex, x, y + bh - thickness, bw, thickness, ec);
    dl.rect(white_tex, x, y, thickness, bh, ec);
    dl.rect(white_tex, x + bw - thickness, y, thickness, bh, ec);
    const int id = party_editor_.slots()[static_cast<size_t>(i)];
    dl.text(face, id >= 0 ? display_name(id) : std::u32string(U"-"), x + bw * 0.5f, y + 46, id >= 0 ? white : dim, sc, 1);
  }
  const auto& roster = party_editor_.roster();
  const int count = static_cast<int>(roster.size());
  const int rows = (count + PartyEditor::kColumns - 1) / PartyEditor::kColumns, vis = 9;
  const int cur_row = party_editor_.cursor() / PartyEditor::kColumns;
  const int top = std::clamp(cur_row - vis + 1, 0, std::max(0, rows - vis));
  for (int row = top; row < std::min(rows, top + vis); ++row)
    for (int col = 0; col < PartyEditor::kColumns; ++col) {
      const int idx = row * PartyEditor::kColumns + col;
      if (idx >= count) break;
      const int id = roster[static_cast<size_t>(idx)];
      const float x = 130.0f + 270.0f * static_cast<float>(col), y = 310.0f + 66.0f * static_cast<float>(row - top);
      const bool sel = !slots && idx == party_editor_.cursor();
      const auto& ps = party_editor_.slots();
      const bool inparty = std::find(ps.begin(), ps.end(), id) != ps.end();
      dl.rect(white_tex, x, y, 250, 56, sel ? pack_rgba(40, 60, 150, 240) : panel);
      if (sel) {
        dl.rect(white_tex, x, y, 250, 4, gold);
        dl.rect(white_tex, x, y + 52, 250, 4, gold);
      }
      dl.text(face, display_name(id), x + 14, y + 12, inparty ? gold : white, sc, 0);
    }
  dl.text(face, slots ? U"Choose a slot (cancel: done)" : U"Choose a character", 640, 920 - 70, dim, sc * 0.8f, 1);
}

void GameFlow::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  if (!owns_screen()) return;
  list_->clear();
  if (party_active_) {
    draw_party(*list_);
    list_->flush(kit_.sprites(), frame, ui::Canvas::fit(vw, vh), resume);
    return;
  }
  if (rename_active_) {
    if (rename_guide_) keymap_.draw(*list_, kit_);
    else name_.draw(*list_, kit_);
    list_->flush(kit_.sprites(), frame, ui::Canvas::fit(vw, vh), resume);
    return;
  }
  if (ngp_active_) {
    if (ngp_slots_) slots_.draw(*list_, kit_);
    else title_.draw(*list_);
    list_->flush(kit_.sprites(), frame, ui::Canvas::fit(vw, vh), resume);
    if (!ngp_slots_ && title_.fade_alpha() > 0.001f) draw_veil(frame, vw, vh, title_.fade_alpha());
    return;
  }
  switch (machine_.state()) {
    case FlowState::Title: title_.draw(*list_); break;
    case FlowState::SlotSelect: slots_.draw(*list_, kit_); break;
    case FlowState::NameEntry:
      if (keymap_active_) keymap_.draw(*list_, kit_);
      else name_.draw(*list_, kit_);
      break;
    default: break;
  }
  list_->flush(kit_.sprites(), frame, ui::Canvas::fit(vw, vh), resume);
  if (machine_.state() == FlowState::Title && title_.fade_alpha() > 0.001f) draw_veil(frame, vw, vh, title_.fade_alpha());
}

void GameFlow::begin_boot_logo() {
  if (!ready_) return;
  // Japanese loads notice.png, else logo.png; the Japanese sequence swaps in notice2.png at counter 180 and logo.png at 360.
  static const char* const kJa[3] = {"ui/notice.png", "ui/notice2.png", "ui/logo.png"};
  static const char* const kOther[1] = {"ui/logo.png"};
  const bool ja = lang_ == import::Lang::Jp;
  const char* const* names = ja ? kJa : kOther;
  const int count = ja ? 3 : 1;
  for (int i = 0; i < count; ++i) {
    if (auto png = kit_.vfs().read(names[i])) boot_images_[i] = kit_.upload_png(*png);
    if (!boot_images_[i]) {
      log::warn("boot: {} unavailable; no boot logo", names[i]);
      boot_logo_.begin(0);
      return;
    }
  }
  boot_logo_.begin(count);
  log::info("boot: logo screen ({} image{})", count, count > 1 ? "s" : "");
}

void GameFlow::tick_boot_logo(int ticks, bool skip) {
  if (skip) boot_logo_.request_skip();
  for (int i = 0; i < ticks && boot_logo_.active(); ++i) boot_logo_.step();
  if (!boot_logo_.active()) log::info("boot: logo screen over{}", skip ? " (skipped)" : "");
}

void GameFlow::draw_boot_logo(const render::Frame& frame, float vw, float vh) {
  if (!ready_ || !boot_logo_.active()) return;
  // The cleared black frame (widescreen: the whole window).
  draw_veil(frame, vw, vh, 1.0f);
  const render::TexRef img = boot_images_[std::clamp(boot_logo_.image(), 0, 2)];
  if (!img) return;
  // One quad, full height, 4:3 wide, centred (x0 = (W - H * 4 / 3) / 2), vertex colour (1, 1, 1, alpha), linear clamp sampler.
  list_->clear();
  list_->image(img, 0, 0, ui::kCanvasW, ui::kCanvasHeight, render::pack_rgba(255, 255, 255, static_cast<u8>(boot_logo_.alpha() * 255.0f + 0.5f)));
  list_->flush(kit_.sprites(), frame, ui::Canvas::fit(vw, vh), true);
}

// A black veil of opacity `alpha` in window space over the screen in use: the 4:3 fit, or the whole widened frame in widescreen
// (Letterbox::veil_rect; widescreen: the title's 3D sea beside the 4:3 title menu fades with it).
void GameFlow::draw_veil(const render::Frame& frame, float vw, float vh, float alpha) {
  const render::Letterbox::Rect rect = render::Letterbox::veil_rect(vw, vh);
  render::SpriteBatch& sprites = kit_.sprites();
  sprites.begin(frame, vw, vh, true);
  sprites.draw(kit_.white(), rect.x, rect.y, rect.w, rect.h, 0, 0, 1, 1, render::pack_rgba(0, 0, 0, static_cast<u8>(std::min(1.0f, alpha) * 255.0f)));
  sprites.end();
}

}  // namespace oc::game
