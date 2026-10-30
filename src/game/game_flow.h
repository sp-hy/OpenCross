#pragma once
// Game flow above the field: Title -> NameEntry -> Opening -> Field, and Title -> SlotSelect (Continue) -> Field.
//
// FlowMachine is the pure state machine (no GPU): events in, FlowAction out, plus the in-memory ui::GameState it creates
// (new game defaults, or a loaded slot). GameFlow binds it to the screens (ui::TitleScreen, ui::NameEntry, ui::SlotList)
// and draws them. The host (App::apply_flow_action) only has to act on the FlowAction.
//
// The new game start was found in the title room's script (opening.bin = room 8, tmp/dis/opening.txt): after "New Game"
// the script runs  OPEN_MENU (title list), RENAME_CHARACTER #0 (name entry, character 0 = Serge), ADD_TO_PARTY #0, then
// MAP_JUMP_DC #22, #0  = room 22 `opn_a01i`, ROOM_PARAM 0.
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "audio/mixer.h"
#include "game/party_editor.h"
#include "data/tables.h"
#include "import/text.h"
#include "ui/menu/game_state.h"
#include "ui/title/name_codec.h"
#include "ui/keymap_screen.h"
#include "ui/title/name_entry.h"
#include "ui/title/boot_logo.h"
#include "ui/title/slot_list.h"
#include "ui/title/title_screen.h"

namespace oc::game {

// The title scene (room 8 and 1), and room 22: the first room after the name entry.
constexpr const char* kTitleRoom = "opening";
constexpr const char* kNewGameRoom = "opn_a01i";
constexpr int kNewGameRoomId = 22;
// Movie row a02koryu: the title script plays it behind MAP_JUMP_DC #22. ROOM_PARAM of that jump is 0.
constexpr int kNewGameMovieRow = 2;
constexpr int kNewGameParam = 0;

enum class FlowState { Title, SlotSelect, NameEntry, Opening, Field };
const char* flow_state_name(FlowState state);
// "[Debug] Flow=" values: field (default, no flow), title, name, slots, opening.
bool parse_flow_start(std::string_view text, std::optional<FlowState>& out, bool& enabled);

// What the host (App::apply_flow_action) must do after a flow event. All fields default to "nothing".
// room: map file stem. room_id: Continue's saved location word; the host re-resolves it on the saved disc
// (FieldWorld::room_stem_for_id). param: ROOM_PARAM. restore_position: Continue puts the leader at (x, z) after load_room.
// state_changed: GameFlow::game_state() holds a new game / loaded slot to apply to the field. open_settings: show the existing
// options window. stop_music: stop the title music before the field starts its own. movie_row: >= 0 plays this movie-table row
// (skippable) first; load_room runs when it ends.
struct FlowAction {
  bool load_room = false;
  std::string room;
  int room_id = -1;
  int param = 0;
  bool restore_position = false;
  s16 x = 0, z = 0;
  bool state_changed = false;
  bool open_settings = false;
  bool quit = false;
  bool stop_music = false;
  int movie_row = -1;
};

class FlowMachine {
 public:
  FlowMachine();

  void set_tables(const data::GameTables* tables, const ui::GameFontCodec* codec);
  void set_save_available(bool has) { has_saves_ = has; }
  FlowState state() const { return state_; }
  void start(FlowState state) { state_ = state; }
  bool has_saves() const { return has_saves_; }

  // ---- events
  // Title -> NameEntry / SlotSelect; Settings / Quit actions.
  FlowAction on_title(ui::TitleChoice choice);
  // NameEntry -> Opening (new game state + opn_a01i).
  FlowAction on_name(std::u32string_view name);
  // SlotSelect -> Field (loaded state + saved room).
  FlowAction on_slot(const ui::SlotEntry& entry);
  // any state -> Field: Save/Load > Load in the in-game menu (same restore as Continue).
  FlowAction on_load(const ui::SlotEntry& entry);
  // SlotSelect -> Title.
  FlowAction on_slot_cancel();
  // New Game+ list. New Game+: the state becomes ngplus::new_game_plus_slot(cleared) (fresh game + the
  // playinit carry-over); the ending room's script then names Serge and builds the dream party. The custom names stay (playinit carries
  // every name; RDE keeps its host name map).
  void new_game_plus(const save::SaveSlot& cleared);
  // Continue+: the slot loads like Continue with the cleared game carried in (ngplus::continue_plus: playinit + the Time Egg). The custom
  // names stay those of the cleared game (RDE's load hook skips its ext_save names while the title variant is 2).
  FlowAction on_continue_plus(const ui::SlotEntry& entry, const save::SaveSlot& cleared);
  // Opening -> Field once the room is no longer an opn_ room.
  FlowAction on_field(std::string_view room);

  // ---- state
  ui::GameState& game_state() { return gs_; }
  const ui::GameState& game_state() const { return gs_; }
  // Names typed in name entry (UTF-32); the save record only holds what the game font can show (ext_save.dat holds all).
  const std::map<int, std::u32string>& custom_names() const { return custom_names_; }
  void set_custom_name(int char_id, std::u32string name);
  // ext_save.dat names of the slot about to load (SaveDat::read_ext; already empty when the record's language differs): on_load applies them over the record's.
  void set_ext_names(std::map<int, std::u32string> names) { ext_names_ = std::move(names); }
  ui::MenuRules& rules() { return rules_; }

  // Replaces the state with a fresh game named `name` (the dev start without the title, and New Game after name entry).
  // `opening_party`: Serge + Kid + the title script's random member (New Game); false = Serge alone (dev start into a field room).
  void new_game(std::u32string_view name, bool opening_party = true);

  // The state of a fresh game: Serge only (character 0), his char.bin stats and gear, gold 0, room 22.
  // Serge, Kid and the title script's random third member (`seed` 0 = unseeded).
  static save::SaveSlot new_game_slot(const data::GameTables* tables, const ui::GameFontCodec* codec, std::u32string_view name, u32 seed = 0,
                                      bool opening_party = true);
  void set_seed(u32 seed) { seed_ = seed; }
  // The title's vibration page: the next new_game / new_game_plus starts with options bit 0x400 (Vibration Function) = `on`.
  void set_vibration(bool on) { vibration_ = on; }
  // Language of the host: a save record of a never-renamed character holds the JP default name (kana); outside JP the game font
  // cannot show it, so such a record counts as unset (same rule as FieldWorld::install_name_hooks). Re-filters the loaded names.
  void set_language(import::Lang lang);
  static bool usable_name(const std::u32string& name, import::Lang lang);

 private:
  FlowState state_ = FlowState::Title;
  u32 seed_ = 0;
  bool vibration_ = false;
  import::Lang lang_ = import::Lang::En;
  bool has_saves_ = false;
  const data::GameTables* tables_ = nullptr;
  const ui::GameFontCodec* codec_ = nullptr;
  ui::MenuRules rules_;
  ui::GameState gs_;
  std::map<int, std::u32string> custom_names_;
  std::map<int, std::u32string> ext_names_;
};

class GameFlow {
 public:
  GameFlow();
  ~GameFlow();
  GameFlow(const GameFlow&) = delete;
  GameFlow& operator=(const GameFlow&) = delete;

  // `tables` (optional) gives the new-game stats; it must outlive the flow. `mixer` may be null.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, const data::GameTables* tables,
            import::Lang lang, audio::Mixer* mixer);
  void shutdown();

  void set_art_mode(ui::ArtMode mode);
  void set_language(import::Lang lang);
  // -1 = silent title (default; the title script plays none).
  void set_title_song(int song) { title_song_ = song; }
  // Save file for Continue (not owned; may be null). Re-read on every entry to Title.
  // The memory cards and the OpenCross save page (either may be null).
  void set_save_dat(const save::SaveDat* dat, const save::OcSaves* page = nullptr) { dat_ = dat, page_ = page; }
  void set_sfx(std::function<void(int)> fn);

  // Title scene: with a field world behind it (set_title_scene(true), app/app_boot.cpp) entering Title asks the host to run the title room
  // (opening.bin, ROOM_PARAM 0: logo FMV, then the pulsing logo scene) and the title screen is drawn over it as a menu.
  void set_title_scene(bool on) { scene_enabled_ = on; }
  // The title room's logo FMV is over (or skipped): the title screen may now read buttons / open the menu.
  void set_title_logo_ready(bool on) { logo_ready_ = on; }
  bool title_logo_ready() const { return logo_ready_; }
  static bool any_button(const platform::Input& in);
  // The title room's 3D scene is drawn / run only for the title screen itself; the Continue slot list (SlotSelect) replaces it with the static
  // title-menu picture (bg 0xB9..0xBB, the same bg0 / bg1 the menu phase shows).
  bool scene_mode() const {
    return scene_enabled_ && machine_.state() == FlowState::Title && !rename_active_ && !party_active_;
  }
  bool take_scene_request() {
    const bool requested = scene_request_;
    scene_request_ = false;
    return requested;
  }
  // Enters a flow state (Title also restarts the fade-in). Opening / Field only set the state.
  void start(FlowState state);
  // One frame of input. `dt` in seconds. Returns what the host must do.
  FlowAction update(const platform::Input& in, float dt);
  // In-game menu Save/Load > Load: replaces the state with the slot and returns the room to enter (like Continue). The host applies it.
  // Now fades the screen to black first (kLoadFade); the restore itself comes out of tick_fade() when the fade-out is over.
  FlowAction load_slot(const ui::SlotEntry& entry);
  // Screen fades around a load (title Continue slot chosen, in-game Save/Load > Load): fade out, load, fade in. The host calls
  // tick_fade(dt) first thing every simulation block (returns the load action once the fade-out has finished; while fading_out() the
  // host must not run the game) and draw_overlay() last in every frame (the black veil over whatever was drawn).
  FlowAction tick_fade(float dt);
  bool fading_out() const { return load_pending_ || open_pending_; }
  // Field main menu (Tab / Start): the field fades to black and the screen holds black, the host opens the menu when
  // take_open_ready() reports it; the menu then shows at full brightness (no fade-in).
  void begin_menu_open_fade() { open_pending_ = true; }
  bool take_open_ready() {
    const bool ready = open_ready_;
    open_ready_ = false;
    if (ready) veil_ = 0.0f;
    return ready;
  }
  void draw_overlay(const render::Frame& frame, float view_w, float view_h);
  // The boot logo (ui::BootLogo): once per process, before anything of the game runs. The host calls
  // begin_boot_logo() at start-up, and while boot_logo_active() runs no game ticks: tick_boot_logo(ticks, skip) per frame and
  // draw_boot_logo() instead of the scene. Art: ui.dat ui/logo.png (Japanese: notice.png, notice2.png, logo.png) in both art modes.
  void begin_boot_logo();
  bool boot_logo_active() const { return boot_logo_.active(); }
  void tick_boot_logo(int ticks, bool skip);
  void draw_boot_logo(const render::Frame& frame, float vw, float vh);
  // The host tells the flow where the field is (for Opening -> Field).
  FlowAction note_field(std::string_view room);

  // Name entry for a character during play (field scripts FE 4A / FE 84): owns the screen until the name is confirmed, then the
  // typed name is stored in the machine's custom names (and the state's name record). rename_finished() reports it once.
  // `guide`: FE 4A shows the user guide first when the field's variable 6 (MAP_ID) is 0 or 8 - the title room's New Game and
  // New Game+.
  void begin_rename(int char_id, bool guide = false);
  bool rename_active() const { return rename_active_; }
  // The user guide page on screen (New Game or the FE 4A rename): 1 controller, 2 keyboard, 0 none.
  int guide_page() const { return keymap_active_ || (rename_active_ && rename_guide_) ? keymap_.page() : 0; }
  bool take_rename_finished() {
    const bool finished = rename_finished_;
    rename_finished_ = false;
    return finished;
  }

  // FE 84 + FE 61 (launcher mode 0x30, title variant 2): the New Game+ title list over the ending room. `cleared` = the game the player
  // continued (the playinit buffer keeps it). Owns the screen until a choice: New Game+ (the state is replaced at once; the host
  // restores the field variables and lets the script go on) or Continue+ (the slot list in variant 2, then a load through tick_fade()).
  // The list has no cancel; the Continue+ slot list's cancel returns to it.
  void begin_ng_plus(const save::SaveSlot& cleared);
  bool ng_plus_active() const { return ngp_active_; }
  enum class NgPlusResult { None, NewGamePlus, ContinuePlus };
  NgPlusResult take_ng_plus_result() {
    const NgPlusResult result = ngp_result_;
    ngp_result_ = NgPlusResult::None;
    return result;
  }

  // FE 5B party screen (PartyEditor over the state's roster / party); owns the screen until it is left, then the state's party is written.
  void begin_party();
  bool party_active() const { return party_active_; }
  bool take_party_finished() {
    const bool finished = party_finished_;
    party_finished_ = false;
    return finished;
  }

  // True while the flow draws the whole screen (Title, SlotSelect, NameEntry, a rename); draw() then replaces the field.
  bool owns_screen() const;
  void draw(const render::Frame& frame, float view_w, float view_h, bool resume = false);
  void draw_party(ui::DrawList& dl);
  void draw_veil(const render::Frame& frame, float vw, float vh, float alpha);

  FlowState state() const { return machine_.state(); }
  FlowMachine& machine() { return machine_; }
  ui::GameState& game_state() { return machine_.game_state(); }
  ui::TitleScreen& title() { return title_; }
  ui::NameEntry& name_entry() { return name_; }
  ui::SlotList& slot_list() { return slots_; }
  const save::SaveDat* save_dat() const { return dat_; }

 private:
  void enter_title(bool fresh = true);
  void enter_slots();
  void enter_name();
  // New Game: the user guide (controls / key map) pages, then enter_name().
  void enter_keymap();
  FlowAction apply(FlowAction action);
  void begin_load_fade(const ui::SlotEntry& entry, bool from_slot_list);

  FlowMachine machine_;
  ui::TitleScreen title_;
  ui::NameEntry name_;
  ui::KeyMapScreen keymap_;
  // The guide is up (state NameEntry, name entry not begun yet).
  bool keymap_active_ = false;
  ui::SlotList slots_;
  ui::UiKit kit_;
  std::unique_ptr<ui::DrawList> list_;
  ui::NavPoller nav_;
  std::unique_ptr<ui::GameFontCodec> codec_;
  import::LangTables lang_tables_;
  import::Lang lang_ = import::Lang::En;
  ui::ArtMode mode_ = ui::ArtMode::Hd;
  audio::Mixer* mixer_ = nullptr;
  const save::SaveDat* dat_ = nullptr;
  const save::OcSaves* page_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  int title_song_ = -1;
  bool ready_ = false;
  bool scene_enabled_ = false, scene_request_ = false, logo_ready_ = true;
  bool rename_active_ = false, rename_finished_ = false;
  // begin_rename(.., guide): the user guide pages are up before the name entry.
  bool rename_guide_ = false;
  bool party_active_ = false, party_finished_ = false;
  // New Game+ list / its Continue+ slot list / a Continue+ load fading.
  bool ngp_active_ = false, ngp_slots_ = false, ngp_load_ = false;
  NgPlusResult ngp_result_ = NgPlusResult::None;
  save::SaveSlot ngp_cleared_;
  // Menu-open fade (see begin_menu_open_fade).
  bool open_pending_ = false, open_ready_ = false;
  float open_hold_ = 0.0f;
  // A load waits for the fade-out to finish.
  bool load_pending_ = false, load_from_slots_ = false;
  ui::SlotEntry load_entry_;
  // Black veil alpha: rises while load_pending_, falls (fade-in) after the load.
  float veil_ = 0.0f;
  ui::BootLogo boot_logo_;
  render::TexRef boot_images_[3] = {};
  PartyEditor party_editor_;
  std::u32string display_name(int char_id);
  int rename_char_ = 0;
  std::function<void(int)> sfx_;
};

}  // namespace oc::game
