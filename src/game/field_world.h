#pragma once
// FieldWorld: the field game mode. Owns the room (RoomView), the entity table (one Ent per script entity: actor, motion,
// flags), the player, the field script VM, the dialogue box, the camera state machine, fades and the room-change flow.
// It implements script::FieldHost, i.e. every world effect a room script can ask for.
//
// Frame use (app/app_run.cpp, app/app_draw.cpp):
//   update(input, ticks)                        once per frame, ticks = 60 Hz steps to simulate (may be 0)
//   draw_background(...)  [SpriteBatch pass]    background layers + floor shadows
//   draw_actors(...)      [3D pass]             skinned models
//   draw_overlay(...)     [SpriteBatch resume]  dialogue window + fade
//
// Entity -> model rules:
//   SETUP_NPC n  -> room model n (.bin subfile 11 + n, texture = the .ctd TIM named by its header), own s4 animations.
//   SETUP_PC ref -> party slot (0xFF leader, 0xFE, 0xFD) -> character -> fieldobj/player/<folder> (+ fieldobj/ex/<folder>.bin).
//   SET_ANIMATION n -> s4 clip n (n < 0x80) or ex clip n - 0x80.
#include <array>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "audio/sfx.h"
#include "assets/battle_stage.h"
#include "assets/room_effects.h"
#include "game/actor.h"
#include "game/disc_index.h"
#include "game/effects/emitter_system.h"
#include "game/effects/gpu_effects.h"
#include "game/effects/room_meshes.h"
#include "game/effects/sequence_player.h"
#include "game/field_battle_handoff.h"
#include "game/field_camera.h"
#include "game/field_input.h"
#include "game/field_watchdog.h"
#include "game/room_lighting.h"
#include "game/room_view.h"
#include "render/render_target.h"
#include "import/text.h"
#include "platform/window.h"
#include "script/field_host.h"
#include "script/field_vm.h"
#include "ui/dialogue_box.h"
#include "ui/menu/game_state.h"

namespace oc::game {

// A script-started movement of one entity (walk / follow / move / tween / jump).
struct Motion {
  bool active = false;
  script::MotionKind kind = script::MotionKind::Walk;
  // target (world units); target_y: ty is meaningful
  float tx = 0, ty = 0, tz = 0;
  bool target_y = false;
  // follow kinds
  int target_ent = -1;
  // remaining ticks for timed kinds (0 = until arrival)
  int frames = 0;
  // tween / jump
  int total = 0, elapsed = 0;
  // start of a tween / jump
  float sx = 0, sy = 0, sz = 0;
  // jump height (unused by JUMP_MOVE, which uses the ballistic step)
  float arc = 0;
  // JUMP_MOVE vertical velocity per tick (+2 per tick = gravity)
  float vy = 0;
  // JUMP_MOVE horizontal step per tick (distance / (frames + 1))
  float step_x = 0, step_z = 0;
  int heading = 0;
  // facing the start form left (bearing to the target, or unchanged when gated): the wait form re-asserts it
  int facing = 0;
  // the shown facing at the start (42 / 43 copy it to Ent::curve_h[slot])
  int curve_h = 0;
  // script slot that started it: index of the curved heading
  int slot = 0;
  // script id held by that record at the start (the motion is orphaned once the record is released)
  int rec_id = -1;
  // this tick another record of the entity ran instead (preempted): no step, no walk clip
  bool paused = false;
  // plays the walk / run clip while it lasts
  bool walking = false;
  // follower AI: run clip
  bool running = false;
  int stuck = 0;
  // position written directly (no wall collision): formation walks of the party
  bool direct = false;
  // formation walks (84 / DF): facing written on arrival, -1 = none
  int final_facing = -1;
  // script slot of the formation op's caller: its flag gates that facing
  int gate_slot = 0;
};

struct Ent {
  Actor actor;
  // room model index, -1 none
  int model = -1;
  // party character (party entities)
  int char_id = -1;
  // party slot 0..2 if SETUP_PC bound it
  int pc_slot = -1;
  // a script (or the spawn) positioned it
  bool placed = false;
  // place_party_spawn put it on the leader's spot (settle_spawned_followers)
  bool spawned_on_leader = false;
  // bind_party_entity set kInactive because its party slot is empty (cleared when the slot is filled again)
  bool slot_inactive = false;
  // SETUP_DIALOG_WINDOW (62) x, y, cols, lines, flags: kept per entity
  std::array<int, 5> win{};
  // ran UPDATE_CHARACTER (0A / 0B) this tick: only then does the party-follow logic move a follower
  bool updating = false;
  // SET_POSITION_XYZ: the height is script-controlled, not snapped to the walk mesh
  bool y_free = false;
  Motion motion;
  // A0 wander: window timer
  int sway_timer = 0;
  // this window walks (set) or stands (clear)
  bool sway_bit = false;
  // the preferred turn direction of the walk, re-picked with a 1/256 chance per tick
  bool sway_dir = false;
  // ENTITY_RGB (A1); 128 = neutral, drawn as a colour multiplier
  std::array<u8, 3> rgb{128, 128, 128};
  int attach_to = -1;
  // attach point of the parent's model (D6 U)
  int attach_point = 0;
  // FE 46: x, y, z offset in the parent joint's space
  std::array<s16, 3> attach_off{};
  // FE 47: rotation angles relative to the parent joint
  std::array<s16, 3> attach_rot{};
  // FE 03: axis (4 / 8 / 0x10 = x / y / z) of the reflected copy, 0 = none
  int mirror_axis = 0;
  // FE 03: plane * 2 (copy axis = plane2 - pos)
  int mirror_plane2 = 0;
  // AC: whole model semi-transparent with ABR n - 1, -1 = off
  int model_abr = -1;
  // FE 45: drawn with the white CLUT
  bool flash = false;
  // FE 45: U (white CLUT row 0xF5 + U)
  int flash_row = 0;
  // party entities: the character's model byte the actor shows
  int char_model = -1;
  // SET_DIALOG_FACE portrait id of this entity (-1 none)
  int face = -1;
  // short description for the debug view
  std::string note;
  // flat light colour, eased per tick (room_lighting.h)
  ActorLight light;
  bool light_init = false;
  // route check: ticks a party follower stood off the walkable mesh
  int off_mesh_ticks = 0;
  // FE 06 pushable: velocity handed over by the player's hit, applied on the next no-hit tick
  bool pend = false;
  float pend_x = 0, pend_z = 0;
  // ticks the controlled entity has pressed against this solid (FE 25 lets it through after 0x40)
  int press_ticks = 0;
  // ticks spent in water (ripple cadence)
  u16 wake_count = 0;
  // shadow strength: starts at 0x40, eases toward (R+G+B)/6, 0 on water / stairs
  int shadow_grey = 0x40;
  // The curved-walk heading of each script slot (FE 2D). 42 / 43 start it at the shown facing; 44 / 45 / 46 keep the
  // slot's last value. Internal angles; 0x800 = the engine's 0.
  std::array<int, 8> curve_h{0x800, 0x800, 0x800, 0x800, 0x800, 0x800, 0x800, 0x800};
  // debug: foot events (model var 1) seen, for the route log
  int foot_events = 0;
  // debug: clip name last logged for this entity
  std::string last_clip;
};

// [Debug] test hooks.
struct FieldDebug {
  // walk the player to a talkable NPC
  bool auto_walk = false;
  // press the action key at this tick
  int action_tick = 200;
  // entity to walk to (-1 = nearest NPC with an ON_ACTION script)
  int target = -1;
  // press confirm every N ticks while a dialogue is open (0 = never)
  int auto_confirm = 0;
  // put the player next to the target instead of walking
  bool teleport = false;
  // open this dialogue message at tick 30 (-1 = off)
  int show_message = -1;
  // ... with lines 0..show_choice as the choice (-1 = plain)
  int show_choice = -1;
  // ... starting at this line (E1 first line)
  int show_choice_first = 0;
  // ... with this portrait id forced (-1 = none)
  int show_face = -1;
};

class FieldWorld final : public script::FieldHost {
 public:
  FieldWorld();
  ~FieldWorld() override;
  FieldWorld(const FieldWorld&) = delete;
  FieldWorld& operator=(const FieldWorld&) = delete;

  bool init(const vfs::Vfs& files, render::GpuContext& ctx, render::SpriteBatch& sprites, render::MeshPipeline& meshes,
            import::Lang lang);
  void shutdown();

  // Loads a room by file stem and runs its INIT scripts. `param` = ROOM_PARAM (selects the entrance record).
  bool load_room(std::string_view name, int param = 0);
  const std::string& room_name() const { return view_.room_name(); }
  const std::vector<std::string>& room_list() const { return view_.rooms(); }

  // force: re-read the art even if `hd` is unchanged (HD <-> Modded, mod list edits)
  void set_art_mode(bool hd, bool force = false);
  void set_language(import::Lang lang);
  // Audio: the BGM mixer and the SFX sink (both optional, not owned). Song ids go through audio/bgm_table.h.
  void set_audio(audio::Mixer* mixer, audio::Sfx* sfx);
  // [Audio] BgmSource: remastered = RDE .sab streams; original = the PS1 AKAO sequence (musicNNN + its bank) on our sequencer.
  // Flow rule: New Game / Continue end the title music (the title room's song must not carry into the next room).
  void stop_music() {
    if (mixer_) mixer_->stop();
    pending_song_ = -1;
    last_song_ = -1;
  }
  // The room's current song from its start, after the mixer was stopped outside the scripts (the title prompt's music fade).
  void restart_song() {
    if (last_song_ >= 0) start_song(last_song_, bgm_volume_);
  }
  void set_bgm_remastered(bool on, bool restart = true);
  bool bgm_remastered() const { return bgm_remastered_; }
  // set: "2021" / "1999"
  void set_portrait_set(const std::string& set) {
    portrait_set_ = set;
    dialogue_.set_portrait_set(set);
    for (auto& window : extra_win_)
      if (window.box) window.box->set_portrait_set(set);
  }
  void set_debug(const FieldDebug& debug) { debug_ = debug; }
  // Seeds every field RNG (NPC idling, the room's effect emitters / sequences). 0 keeps the built-in constants.
  void set_seed(u32 seed);

  // ---- game state -------------------------------------------------------------------------------------------------------
  // The in-memory state (party, roster, stats / HP, inventory, gold, key items, flags) is the single source of truth: this world
  // reads and writes it, it never keeps its own copy. Defaults to a private fresh state until the host passes the shared one.
  void set_game_state(ui::GameState* gs);
  ui::GameState& game_state() { return *gs_; }
  // Typed character names (UTF-32) for the dialogue name placeholders; the map must outlive the world.
  void set_custom_names(const std::map<int, std::u32string>* names);
  void install_name_hooks();
  // Writes the world into the state before a save: global script variables 0x000..0x3FF into the head block, the room id, the
  // leader's position and the party ids.
  void capture_state();
  // Continue: copies the saved variables back into the VM. Call before load_room() (room-local variables stay zero); the entry
  // variables the room load sets (ROOM_PARAM, MAP_ID) win over the saved ones.
  void restore_vars();
  // The Dopplegang pool: 16 u16 = global variables 0x3E0..0x3FE (battle script vars 0x60..0x7F).
  std::array<u16, 16> dopple_mask() const;
  // Sets the VM variables and the head block together (a battle's end).
  void set_dopple_mask(const std::array<u16, 16>& mask);
  // Puts the party leader at (x, z) on the walk mesh once the room is up (Continue). False if there is no leader.
  bool place_leader(int x, int z);
  // Resets the transient world (variables, queued requests) for a new game; keeps the state object.
  void reset_for_new_game();
  // ---- discs: the file index in use. The disc is data-driven: FE 65 and the DiscCheck at every
  // map transition set it from variable 0x42 (= disc - 1); a loaded save takes it from its variable 0x42 too.
  int disc() const { return disc_; }
  void set_disc(int disc);
  // Room file stem of a script room id on the current disc; falls back to the lang.dat table.
  std::string room_stem_for_id(int room_id) const;
  const DiscIndex& disc_index() const { return discs_; }
  // The save gate of the main menu (FE 72 save points and the menu key): variable 0x3C == 1.
  bool can_save() const { return vm_.var(0x3C) == 1; }
  // FE 18 / FE 7F set global flag bit 12: the main menu does not open from the pad (opening, ending, gal_b06i's bed scene...).
  bool menu_key_allowed() const { return !(vm_.global_flags() & (1u << 12)); }

  // ---- scripted menus / shops / movies handed to the host (App::advance) -----------------------------------------------------------
  struct MenuRequest {
    script::MenuKind kind = script::MenuKind::MainMenu;
    int arg = 0;
  };
  bool menu_pending() const { return menu_state_ == MenuState::Requested; }
  MenuRequest take_menu() {
    menu_state_ = MenuState::Open;
    return menu_req_;
  }
  // The host closed the menu / shop / save screen: the blocked script resumes; var 0x4C = result (1 = saved).
  void menu_closed(int result = 0);
  // ---- Grand Slam (FE 75 opens the monster picker, the FE 61 after it waits for it; ui::MonsterPicker) -------------------------------------
  // The session's round table (3 list positions per round, 0xFF = none) and the pool slots of the last picks (the party of a
  // monster-party battle, -1 = none).
  std::array<u8, 9>& grand_slam_table() { return grand_slam_table_; }
  const std::array<int, 3>& monster_party() const { return monster_party_; }
  void set_monster_party(const std::array<int, 3>& picks) { monster_party_ = picks; }
  // While the picker is up the room shows only background group 1 (the picker's frames; group 0 hidden) at scroll (0, 0); closing
  // shows group 0 again, hides group 1 and restores the scroll.
  void set_picker_backdrop(bool on);
  // The room's background layers alone (the picker's backdrop), between SpriteBatch begin() and end().
  void draw_room(render::SpriteBatch& sprites, float vw, float vh);
  struct MovieRequest {
    int index = 0, variant = 0;
    bool skippable = false;
    // movie.dat member base name ("" = nothing to play)
    std::string name;
  };
  // Host tap on the named-object ops (9B 9C B6 E7 D3 D7): called first; true = handled by the host (the title scene's global title.prd
  // sequences "tit0".."tit3" that the room has no .prd entry for).
  // Host hook: a key item (1..64) was newly obtained through give_item.
  void set_on_key_item(std::function<void(int)> cb) { on_key_item_ = std::move(cb); }
  // Field key-item list. Tele-Porter (1), Relief Charm (61) and Smith Spirit (63) are usable only
  // while script vars 0x46 and 0x3C are both 1; every other owned item always is.
  bool key_item_usable(int item) const;
  // A key item chosen in the list: var 0x2A USED_KEY_ITEM = 0x400 + n; Tele-Porter -> party screen (as FE 5B), Smith Spirit -> smith
  // (as FE 68 with shop var 0x4A), any other item is shown to the entities in talk range (ON slot 4 of those that accept an item).
  void use_key_item(int item);
  void set_effect_tap(std::function<bool(script::NamedOp, std::string_view)> tap) { effect_tap_ = std::move(tap); }
  bool movie_pending() const { return movie_state_ == MovieState::Requested; }
  // The host starts the requested movie: the room sequence named after it ("mo1" / "mo2" by the disc, then 'a' + the movie number:
  // controller rumble timed to the movie) starts with it and is stopped when it ends; movie_tick runs the room's sequences one 60 Hz
  // tick while the movie plays.
  MovieRequest take_movie();
  void movie_tick();
  void movie_finished();
  bool movie_active() const { return movie_state_ != MovieState::None; }

  // ---- room effects (.prd emitters / sequences) -------------------------------------------------
  // [3D pass, after draw_actors] depth-tested particle quads of the room's emitters. Draws nothing for rooms without any.
  void draw_effects(const render::Frame& frame, float vw, float vh, float alpha);
  size_t effect_quads() const { return last_quads_; }
  // false = simulate the emitters but never upload / draw them (sweeps)
  void set_effects_gpu(bool on) { effects_gpu_ = on; }
  // The shared state was edited behind the world's back (menu, shop): re-read the party and rebind the party entities.
  void state_edited() {
    sync_party();
    refresh_party_models();
  }
  const effects::EmitterSystem* emitters() const { return emitters_.get(); }

  void update(const platform::Input& in, int ticks);
  void draw_background(render::SpriteBatch& sprites, float vw, float vh, float alpha);
  // [after draw_actors + draw_effects] the semi-transparent layers filed nearer than every actor (the Hydra Marshes water layer): they
  // blend over the models and effects, as in the PS1's ordering table. Draws nothing in rooms without such a layer.
  void draw_background_late(render::SpriteBatch& sprites, float vw, float vh, float alpha);
  // `wet`: WetOnly = only the submerged faces of actors standing in water (draw BEFORE draw_background), DryOnly = everything else
  // (after it); MeshPipeline::DrawParams::WetFaces.
  void draw_actors(render::MeshPipeline& meshes, const render::Frame& frame, float alpha,
                   render::MeshPipeline::DrawParams::WetFaces wet = render::MeshPipeline::DrawParams::WetFaces::DryOnly);
  // `windows` = false leaves the status icons and the dialogue / extra windows to draw_windows() (the PS1-resolution pass, [Video] Ps1Res).
  void draw_overlay(render::SpriteBatch& sprites, float vw, float vh, bool windows = true);
  // Party status icons + the dialogue and extra windows. `ps1` = into the PS1-resolution target (vw x vh = (320 + 2e) x 240 pixels, 1 layer
  // column = 1 pixel, canvas unit = 1/4 pixel); otherwise the window's letterbox / render::canvas_fit.
  void draw_windows(render::SpriteBatch& sprites, float vw, float vh, bool ps1 = false);
  // Ps1Res: after the window pass - transition veil, room fade, captured frame (draw_overlay(windows = false) leaves them out).
  void draw_overlay_tail(render::SpriteBatch& sprites, float vw, float vh);
  void draw_veil_transition(render::SpriteBatch& sprites, float vw, float vh);
  void draw_veil_fade(render::SpriteBatch& sprites, float vw, float vh);
  bool overlay_needed() const { return any_window_open() || fade_ > 0.001f || tint_active() || capture_shown() || seq_dim_shown() || handoff_.in_intro_or_hold(); }

  // ---- 8F / 90 screen ripple (render/render_target.h build_screen_warp) ---------------------------------------------
  // On while the ripple is set; `ot` = the grid's ordering-table index (>= 7): content filed deeper (slot > ot) is warped, the rest is
  // drawn over the warped picture.
  struct WaveView {
    bool on = false;
    int ot = 7;
    render::ScreenWarpParams warp;
  };
  WaveView wave_view() const;
  // ---- damage-floor blur: for 8 frames after a hit the host blends the previous
  // picture over the new one at 50 %, shifted by (tx, ty) PS1 px and scaled about the picture centre.
  struct JoltView {
    bool on = false;
    float tx = 0, ty = 0, scale = 1;
    // 7 .. 0: changes once per game tick (the PS1 displays a new picture then)
    int n = -1;
    // the sequence post pass (3E / 40): the CURRENT picture, blended by `mode` with colour `rgb`
    bool post = false;
    // post: ABR 0 average, 1 add, 2 subtract, 3 add / 4
    int mode = 0;
    std::array<int, 3> rgb{0x80, 0x80, 0x80};
  };
  JoltView jolt_view() const;
  // The ordering-table slab every field drawer honours (layers, shadows, actor faces, effect quads): only content with lo < slot <= hi is
  // drawn. The host draws the deep part (ot, inf], warps it, then the near part (-inf, ot]. Reset to everything after use.
  void set_draw_slab(float lo, float hi);
  // [Enhancements] SmoothEffectRings
  void set_smooth_rings(bool on) { smooth_rings_ = on; }
  // ---- FE 02 captured-frame transitions (kinds 1 / 3 / 4 additive fade of the old frame, 5 / 6 slide) ------
  // The host hands the next presented frame to FieldWorld when capture_wanted() (GpuContext::request_capture), and draws the overlay as usual.
  bool capture_wanted() const { return capture_.want; }
  void set_captured_frame(std::vector<u8>&& rgba, u32 w, u32 h);
  // FE 70 as the A3 / C9 battle handlers copy it: the entity whose attach point 9 (waist) the
  // encounter effect 'enct' (effect/particle.prd > nested pack 'fenc') is placed on (emitter op 0x39 selector 12, effects_actor_basis);
  // the leader when unset.
  int battle_intro_anchor() const { return handoff_.battle_anchor >= 0 ? handoff_.battle_anchor : player_entity(); }

  // ---- introspection (debug UI) ---------------------------------------------------------------------------------
  const script::FieldVM& vm() const { return vm_; }
  script::FieldVM& vm() { return vm_; }
  size_t entity_count() const { return ents_.size(); }
  const Ent& ent(size_t i) const { return ents_[i]; }
  int player_entity() const;
  u64 tick_count() const { return tick_count_; }
  // load_room() calls so far (routes: an art switch must not load the room)
  u64 room_loads() const { return room_loads_; }
  // in-place art swaps of a loaded room (set_art_mode)
  u64 art_swaps() const { return art_swaps_; }
  int room_id() const { return room_id_; }
  // script / save location id
  int map_id() const { return map_id_; }
  // ROOM_PARAM of the last load_room
  int entrance_param() const { return entrance_param_; }
  const RoomView& view() const { return view_; }
  RoomView& view() { return view_; }
  bool dialogue_is_open() const { return any_window_open(); }
  float fade_level() const { return fade_; }
  // Field menu closed: the field comes back from black over `ticks`.
  void fade_in_from_black(int ticks) {
    fade_ = 1.0f;
    fade_target_ = 0.0f;
    fade_step_ = 1.0f / static_cast<float>(ticks);
    fade_add_ = false;
    fade_black_ = false;
    fade_kind_ = 0;
  }
  const std::string& status() const { return status_; }
  int camera_mode() const { return static_cast<int>(camera_.cam_mode); }
  int control_yaw() const { return control_yaw_; }
  const std::map<std::string, u64>& stub_hits() const { return stub_hits_; }
  int auto_target_entity() const { return auto_target_; }
  // controls are free (no dialogue, fade, transition or running action)
  bool idle() const { return controls_free(); }
  // the pause screen may open over the field (message windows, choices and locked controls do not prevent it)
  bool pause_allowed() const;
  // route `assert choiceline`: the message line the hand is on in any open choice window (-1 none)
  int dialogue_choice_line() const {
    int line = dialogue_.is_open() ? dialogue_.picking_line() : -1;
    for (const auto& window : extra_win_)
      if (line < 0 && window.box && window.box->is_open()) line = window.box->picking_line();
    return line;
  }
  bool dialogue_choice_pending() const { return any_window_open() && choice_pending_; }
  // route `wait/assert text`: every open window, space-joined
  std::string dialogue_text() const {
    std::string text = dialogue_.is_open() ? dialogue_.text() : std::string();
    for (const auto& window : extra_win_)
      if (window.box && window.box->is_open()) text += (text.empty() ? "" : " ") + window.box->text();
    return text;
  }
  // route `snap`: the typewriter position of every open window (DialogueDemo::progress), -1 = closed
  std::vector<int> dialogue_progress() const {
    std::vector<int> progress{dialogue_.is_open() ? dialogue_.progress() : -1};
    for (const auto& window : extra_win_) progress.push_back(window.box && window.box->is_open() ? window.box->progress() : -1);
    return progress;
  }
  const std::vector<int>& party_chars() const { return party_chars_; }
  // Route hooks: a character's current HP (what 0), max HP (1) or status bits (2); -1 = no such character. The setter writes 0 / 2.
  int debug_char_stat(int char_id, int what) const;
  void debug_set_char_stat(int char_id, int what, int value);
  // The player-controlled entity's position (false without one).
  bool player_xz(float& x, float& z) const;
  // Highest count of ticks any non-party model entity stood off the walkable mesh in this room (sweep / route check); `ent` = which.
  int worst_npc_off_mesh(int& ent) const {
    int worst = 0;
    for (size_t i = 0; i < ents_.size(); ++i)
      if (ents_[i].pc_slot < 0 && ents_[i].off_mesh_ticks > worst) worst = ents_[i].off_mesh_ticks, ent = static_cast<int>(i);
    return worst;
  }
  bool entity_xz(int ent, float& x, float& z) const;
  // route hook; -1 = controlled entity
  int entity_facing(int ent) const;
  // route hook: the entity's head-turn angles, false = head not turning
  bool entity_head(int ent, int& ax, int& az) const;
  // route hook: (clip index << 12) | keyframe * 4 of the entity's actor, -1 = none
  int entity_clip(int ent, bool source = false) const;
  // route hook: model yaw in script space (= facing once the turn is done)
  int entity_yaw(int ent) const;
  // Route planner (debug automation): waypoints over the walk mesh from (x0, z0) to (x1, z1), A* across triangle adjacency with
  // optional straight-line smoothing (off = plain triangle centroids, safer along walls); empty when either end is off the mesh or unreachable. Solid entities are not considered.
  std::vector<std::array<float, 2>> find_path(float x0, float z0, float x1, float z1, bool smooth = true) const;
  // Sweep check: problems of the room's background layer ops ("" = fine). Every literal group of a BG_LAYER_ON / OFF (80 / 81) / C2
  // reachable from the entity scripts must be a tile group of the room, and after INIT at least one group image must be visible.
  std::string layer_report(std::string* notes = nullptr) const;
  // one line per entity: position, model, flags, trigger size (route "dump")
  std::string dump_entities() const;

  // ---- battle handoff (A3 / C9) ---------------------------------------------------------------------------------------
  // The script op calls request_battle(); the field plays the encounter sequence 'enct' (105 ticks, the field keeps running with the
  // input locked) and battle_ready() turns true when its task has ended. The host (app/app_battle.cpp) runs the BattleView for
  // take_battle() (the battle starts from white) and reports the outcome with end_battle(); the field fades in from black over 0x30
  // ticks and the blocked script resumes with BATTLE_RESULT (var 0x32: 0 victory, 1 party wiped, 2 fled) set.
  using BattleRequest = BattleHandoff::BattleRequest;
  bool battle_ready() const { return handoff_.ready(); }
  // The host loads the battle behind the swirl / the frozen hold: battle_requested() from request_battle() on (font prefetch),
  // battle_loading() while the hold runs (BattleView::load_step each frame), peek_battle() reads the request without taking it.
  bool battle_requested() const { return handoff_.requested(); }
  bool battle_loading() const { return handoff_.loading(); }
  BattleRequest peek_battle() const { return handoff_.battle_req; }
  // the tick before the white: the host keeps the previous output as the hold's picture
  bool transition_prefreeze() const;
  // btlinit's white clear is on screen (4 frames); the host feeds it into the 3E strips' feed-back
  bool transition_white() const;
  using TransitionStrips = BattleHandoff::TransitionStrips;
  TransitionStrips transition_strips() const;
  // `main`'s bracket clock now (the battle continues it from the take-over)
  int transition_main_clock() const;
  BattleRequest take_battle();
  void end_battle(int result_code);
  // BATTLE flag 4: model byte 0x3B on every party member but Poshul / Mojo
  void apply_battle_disguise();
  int battle_result() const { return vm_.var(script::var::kBattleResult); }

  // ---- script::FieldHost ----------------------------------------------------------------------------------------------
  bool system_ready() override;
  void end_of_tick(script::FieldVM& vm) override;
  int party_entity(int slot) override;
  int character_entity(int char_id) override;
  int party_slot_of_entity(int ent) override;
  void setup_pc(int ent, int char_id, int flags) override;
  void setup_npc(int ent, int model) override;
  int load_model(int ent, int model) override;
  bool monster_defeated(int ordinal) override;
  void set_monster_defeated(int ordinal) override;
  void update_entity(int ent) override;
  void set_position_xz(int ent, s32 x, s32 z, int param) override;
  void set_position_xyz(int ent, s32 x, s32 y, s32 z) override;
  script::Vec3 get_position(int ent) override;
  void set_facing(int ent, int angle) override;
  int get_facing(int ent) override;
  void turn_to_entity(int ent, int target) override;
  bool rotation_busy(int ent) override;
  void face_point(int ent, s32 x, s32 z) override;
  void set_animation(int ent, int anim) override;
  void play_animation(int ent, int anim) override;
  void set_walk_animation(int ent, int mode, int anim) override;
  bool animation_busy(int ent) override;
  void set_move_speed(int ent, int speed) override;
  void start_motion(int ent, const script::MotionRequest& req) override;
  bool motion_busy(int ent) override;
  bool motion_wait(int ent) override;
  bool near_and_facing(int ent, int target, int dist, int tolerance) override;
  bool region_test(int ent, int region, bool band) override;
  int entity_state(int ent) override;
  void head_op(script::HeadOp op, int ent, int other, s32 x, s32 y, s32 z, int rate) override;
  bool attach_point(int ent, int point, script::Vec3& out) override;
  void entity_effect(u16 key, int ent, std::span<const s32> args) override;
  void attach_entity(int ent, int target, int offset) override;
  void extra_animation(u16 key, bool start, int ent, int arg) override;
  bool extra_animation_busy() override;
  void camera_set_position(s32 x, s32 y, int frames) override;
  void camera_to_actor(int frames) override;
  void camera_follow(int ent, int frames) override;
  bool camera_busy() override;
  void camera_limits(s32 x0, s32 x1, s32 y0, s32 y1) override;
  void camera_auto_follow(bool on) override;
  std::pair<s32, s32> camera_position() override;
  void set_camera_origin(s32 x, s32 y) override;
  void set_controlled_entity(int ent) override;
  void shake(int x, int y, int ticks) override;
  void formation(u16 key, std::span<const s32> args) override;
  void walk_party_member(int slot, float x, float z, int final_facing = -1, int gate_slot = 0);
  void party_v_spot(int slot, float lx, float lz, int heading, float& x, float& z) const;
  bool formation_done() override;
  bool follow_wait() override;
  void setup_dialog_window(int ent, std::span<const s32> v5) override;
  void set_dialog_face(int face) override;
  bool dialogue_open() override;
  bool dialogue_blocked() override;
  void open_dialogue(int speaker, int message, int flags) override;
  void dialogue_choice(int first, int last) override;
  int dialogue_result() override;
  void bg_layer(int layer, bool on) override;
  void set_layer_opacity(int layer, int r, int g, int b) override;
  bool bg_wave(int mode, int arg) override;
  void bg_wave_start(std::span<const s32> p8) override;
  void set_shadow_colors(std::span<const u8> rgbx16) override;
  void set_screen_rgb(int abr, int r, int g, int b, int frames) override;
  void set_underwater_rgb(int r, int g, int b) override;
  void fade(int mode, int arg) override;
  void draw_sync() override;
  void set_entity_rgb(int ent, int r, int g, int b) override;
  bool play_song(int song) override;
  bool play_song_ex(int song, int volume) override;
  // PLAY_SONG = 127, FE 78 = its own volume
  bool play_song_at(int song, int volume);
  bool song_changing() override;
  void play_sfx(int mode, int id, int volume, int pan, int extra) override;
  void sfx_volume(int slot, int frames, int volume) override;
  void sfx_pan(int slot, int frames, int pan) override;
  void fade_master_volume(int frames, int volume) override;
  void named_effect(script::NamedOp op, std::string_view name, int ent) override;
  void play_movie(int index, int variant, bool skippable) override;
  bool movie_playing() override;
  void request_battle(int battle_id, int arg, int flags) override;
  bool battle_active() override;
  // Cheats: No encounters
  bool skip_battle(int arg, bool c9, int ent) override;
  bool skip_model_swap(int ent) override;
  // the encounter sequence 'enct' runs before a battle (BattleState::Intro)
  bool battle_intro() const;
  void request_map_jump(int room, int param) override;
  void request_menu(script::MenuKind kind, int arg) override;
  bool menu_active() override;
  void field_reset() override;
  int party_member(int slot) override;
  bool in_party(int char_id) override;
  bool party_add(int char_id) override;
  void party_remove(int char_ref) override;
  void party_override(bool enter, int key) override;
  void swap_leader(int char_id) override;
  void roster_add(int char_id) override;
  void roster_remove(int char_id) override;
  int item_count(int id) override;
  void give_item(int id) override;
  void take_item(int id) override;
  s32 money() override;
  void set_money(s32 amount) override;
  int char_life(int char_id) override;
  void change_char_life(script::LifeOp op, int char_id, s32 value) override;
  int char_status(int char_id) override;
  void heal_group(int group) override;
  void change_status(int char_ref, int mask, bool set) override;
  void learn_limit(int level, int char_id, int value) override;
  void set_limit_mode(int mode) override;
  int set_character_model(int char_ref, int value) override;
  bool model_swap_busy() override;
  void model_swap_commit() override;
  int character_model_byte(int char_id) override;
  void refresh_party_models() override;
  void table_op(u16 key, int arg, int index) override;
  u16 keys_held() override;
  u16 key_latch() override;
  void clear_key_latch() override;
  bool predicate(script::Predicate op, int arg) override;
  s32 query(script::Query which) override;
  void set_global(script::Global which, s32 value) override;
  void opaque_op(u16 key, std::span<const u8> operands) override;

 private:
  enum class MenuState { None, Requested, Open };
  enum class MovieState { None, Requested, Playing };
  enum class Trans { None, Out, In };
  using BattleState = BattleHandoff::BattleState;

  struct PendingJump {
    bool active = false;
    int room = 0, param = 0;
  };

  // ---- room / model plumbing
  void clear_room();
  std::shared_ptr<ModelHandle> room_model(int index);
  // set_art_mode on a loaded room: art from the other file set, simulation state untouched
  void swap_art();
  std::shared_ptr<ModelHandle> character_model(int char_id);
  // The CPU half of character_model (files, import, texture mods, bled uploads) for a model byte not loaded yet, on a worker: a party
  // member shown in another body (Viper Manor's uniforms) gets its own model ready for the script that switches it back.
  void prefetch_character_model(int model_byte);
  void drop_character_prefetches();
  void bind_party_entity(int slot);
  void rebind_party();
  void place_party_spawn(int slot);
  // Select in the field / on the world map: the party order rotates one place (the entities stay where they are).
  void rotate_party();

  // ---- simulation
  void sim_tick();
  // turn rates and the scripted-turn state of every entity
  void step_turn_rates();
  void step_motions();
  void step_player();
  void step_followers();
  // follow rule 1 (dL < 80) for party slot 1 / 2: one step toward the point 128 units from the leader (free step while it lands on the
  // mesh, else the probe / slide step, else stand)
  // ent_override: a non-party entity running op 0A
  void follower_back_off(int slot, bool other_side = false, int ent_override = -1);
  // load: followers stacked on the leader's spawn spot walk out by rule 1 before the first frame
  void settle_spawned_followers();
  void step_triggers();
  void step_visuals();
  // the off-screen test of every model entity
  void step_cull();
  void step_camera();
  // water ripples (library emitter mz01) under actors standing in deep water
  void step_wakes();
  // per-actor shadow strength
  void step_shadows();
  void step_fade();
  // Every field entry fades in from black after INIT, whatever INIT's own A8 asked for: kind 2 over 0x20 ticks from white, else
  // over 0x30 from black (a captured kind then sets its own length).
  void begin_entry_fade(int kind);
  void step_transition();
  void update_input_lock();
  bool controls_free() const;
  int pad_heading(bool& run) const;
  bool blocked_by_solids(int player, float x, float z, float old_x, float old_z);
  bool push_target_inside_box(int crate, float vx, float vz) const;
  // the wall-collision step as a probe
  bool push_step_ok(Ent& ent_state, float dx, float dz);
  // the pushed actor takes its pending velocity
  void apply_push(Ent& ent_state);
  float ent_radius(int i) const;
  float ent_height(int i) const;
  bool ent_box(int i) const;
  bool in_box(int i, float px, float pz, float margin) const;
  bool ent_hidden(int i) const;
  // retail draw skip: only the hidden / off-screen flags (an inactive entity keeps the model drawn)
  bool ent_not_drawn(int i) const;
  // The entity's walk flags: the attr flags of its triangle, 0 for a free-height NPC.
  u32 walk_flags(int i) const;
  float ground_y(const Ent& ent_state) const;
  int bearing(float dx, float dz) const;
  void snap_camera_to_player();
  void begin_jump(int room, int param);
  void finish_jump_failed(const std::string& why);
  void stub(const char* what);
  bool tint_active() const;
  // True once a room light (type-17 emitter object) has lit a slot. The PS1 shades FE 64 actors from the slots whatever they hold; with all three
  // at intensity 0 that is black, which no shipped scene intends, so until a light exists the actor keeps its flat zone colour.
  bool lights_active() const { return lights_.slot[0].intensity > 0 || lights_.slot[1].intensity > 0 || lights_.slot[2].intensity > 0; }
  void start_song(int song, int volume, double start_seconds = 0.0);
  u32 rand_u32() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }
  void pick_auto_target();
  std::string describe_ent(int i) const;
  int map_id_for(std::string_view stem);
  void sync_flag_table();
  u32 flag_writes_seen_ = 0;
  // room stem -> script room id on the current disc (lazy)
  std::map<std::string, int> stem_ids_;

  const vfs::Vfs* files_ = nullptr;
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::MeshPipeline* meshes_ = nullptr;
  RoomView view_;
  ui::DialogueDemo dialogue_;
  // The other message windows (the engine keeps 4, one per owning entity; see FieldWorld::open_dialogue), created on first use.
  struct ExtraWindow {
    std::unique_ptr<ui::DialogueDemo> box;
    int owner = -1;
    bool label = false;
  };
  std::array<ExtraWindow, 3> extra_win_;
  // entity -> the window slot (0 = dialogue_, 1.. = extra_win_) of its last message
  std::map<int, int> owner_window_;
  std::string portrait_set_ = "2021";
  ui::DialogueDemo* window_owned_by(int ent);
  ui::DialogueDemo& window_of(int ent);
  bool any_window_open() const;
  bool modal_window_open() const;
  void close_windows();
  script::FieldVM vm_;
  import::Lang lang_ = import::Lang::En;
  bool initialised_ = false;

  std::vector<Ent> ents_;
  // by room model index (null = not loaded / failed)
  std::vector<std::shared_ptr<ModelHandle>> room_models_;
  std::vector<bool> room_model_tried_;
  // by character id, kept across rooms
  std::map<int, std::shared_ptr<ModelHandle>> char_models_;
  struct PrefetchedCharacter {
    assets::ModelAsset asset;
    render::ModelUploads uploads;
    unsigned generation = 0;
  };
  // by model byte: prefetch_character_model's workers, taken by character_model
  std::map<int, std::future<std::optional<PrefetchedCharacter>>> char_prefetch_;
  std::array<int, 3> party_ent_{-1, -1, -1};
  // character id per party slot (cache of gs_->party())
  std::vector<int> party_chars_{0};
  // used until the host passes the shared state
  ui::GameState own_state_;
  ui::GameState* gs_ = &own_state_;
  const std::map<int, std::u32string>* custom_names_ = nullptr;
  // party_chars_ <- the state's party
  void sync_party();
  // state's party <- party_chars_
  void write_party();
  int controlled_override_ = -1;
  // CLEAR_CONTROLLED_ENTITY ran and nothing set a controlled entity since
  bool controlled_cleared_ = false;

  std::string status_;
  // lang.dat room id (dialogue text); map_id_ below is the script / save id on the current disc
  int room_id_ = -1;
  int map_id_ = -1;
  DiscIndex discs_;
  int disc_ = 1;
  int entrance_param_ = 0;
  bool in_init_ = false;
  u64 tick_count_ = 0;
  // ticks after a room load in which clip cross-fades are cut (INIT's first pose is not a visible move)
  u32 entry_cut_ = 0;
  // 2 frames (4 ticks) after a room load in which nothing is culled
  u32 cull_grace_ = 0;
  u64 room_loads_ = 0, art_swaps_ = 0;
  // art mode changed while a window was open: applied when the next window opens
  bool dialogue_mode_pending_ = false;

  // input snapshot
  FieldInput input_;
  // a used key item waits for the next trigger pass
  bool key_item_shown_ = false;
  // FE 69 SAVE: (element id, copies) taken off Kid's grid, returned to the stock by RESTORE
  std::vector<std::pair<int, int>> fe69_saved_;

  // player / control
  int control_yaw_ = 0;
  // FE 21 SETUP_SHAKE: amplitudes in 16.16 screen pixels ramp to the target over ticks + 1 steps; each
  // tick the scroll gets a uniform random offset in [-a/2, a/2].
  struct Shake {
    s32 ax = 0, ay = 0, dx = 0, dy = 0;
    int tx = 0, ty = 0, frames = 0;
    u32 rng = 0x2545F491u;
  } shake_;
  int shake_offset(s32 amp);
  // Sequence op 2E: the display offset of one frame for the integer amplitude `amp`, in [amp / 2, amp).
  int display_shake_offset(int amp);
  bool party_follow_ = true;
  // op BE in progress: follow rule 1 is skipped
  bool follow_hold_ = false;
  // counts down from 0xF0 while the hold is set
  int follow_hold_ticks_ = 0;
  // Follower trails (2 rings x 0x80 entries). Ring 0 = leader, ring 1 = first follower; one entry is
  // appended per tick in which that member actually moves.
  // An entry keeps the position, the tile and its flags.
  struct TrailEntry {
    float x = 0, y = 0, z = 0;
    int tri = -1;
    u32 flags = 0;
  };
  struct Trail {
    std::array<TrailEntry, 0x80> entries{};
    // next write slot
    int head = 0;
  };
  std::array<Trail, 2> trails_;
  // The leader as it stood when the tick began, before the pad move: the scripts (and with them the follow rules) run before
  // the mover, so ring 0 records this spot and the followers measure their distances to it.
  TrailEntry lead_start_;
  int lead_start_facing_ = 0;
  bool lead_start_valid_ = false;
  bool lead_start_pushed_ = false;
  // where the last pad step left the leader (not a scripted move)
  TrailEntry lead_pad_end_;
  TrailEntry trail_entry_of(const Actor& actor) const;
  void trail_fill(int ring, const TrailEntry& entry);
  void trail_push(int ring, const TrailEntry& entry);
  void reset_trails();

  FieldCamera camera_;

  // screen
  render::Texture white_;
  render::Sampler white_sampler_;
  render::TexRef white_set_;
  // black coverage 0..1
  float fade_ = 0;
  float fade_target_ = 0;
  // per tick
  float fade_step_ = 1;
  // fade_ is the slot-0 screen fade (room changes, A8, menu returns): subtractive toward black (ABR 2, B - F), or additive toward
  // white (ABR 1, B + F) for a kind 2 room change.
  bool fade_add_ = false;
  // The last slot-0 fade went to black (a room exit, A8 mode 1); a fade-in clears it. A8 only acts when it flips this.
  bool fade_black_ = false;
  // Effect-sequence screen colour (sequence ops 2B / 2C):
  // a full-screen flat quad of colour `cur`, mode m (0 opaque, else ABR m - 1), linear from the start
  // colour to the target over the given ticks. Only drawn while the colour is not black. 'enct' dims the field with it (mode 3 = B - F).
  struct SeqDim {
    std::array<int, 3> cur{}, start{}, target{};
    int elapsed = 0, ticks = 0, mode = 0;
    bool task = false;
  };
  std::array<SeqDim, 2> seq_dim_{};
  // Sequence ops 3E / 3F / 40 (run last in the frame while active): a transformed copy of
  // the whole picture blended over it. Parameters: rotation, translation (z = zoom), screen offset, colour, ABR mode; op 40
  // ramps them linearly to the targets over `ticks`.
  struct SeqPost {
    bool on = false, ramp = false;
    // rot x y z, trans x y z, offset x y
    std::array<int, 8> cur{}, start{}, target{};
    std::array<int, 3> rgb{0x80, 0x80, 0x80}, rgb_start{}, rgb_target{};
    int abr = 0, elapsed = 0, ticks = 0;
  };
  SeqPost seq_post_;
  void seq_post_op(const assets::SeqCmd& cmd);
  void seq_post_step();
  void seq_dim_set(size_t slot, int r, int g, int b, int mode, int ticks);
  void seq_dim_step(SeqDim& dim);
  bool seq_dim_shown() const { return seq_dim_[0].cur != std::array<int, 3>{} || seq_dim_[1].cur != std::array<int, 3>{}; }
  // The world stands still only during the frozen-capture exit kinds (1, 3..6; no sweeping alpha then). The fade kinds 0 / 2 tick the
  // whole field, so their interpolation pairs advance normally.
  bool exit_frozen() const { return trans_ == Trans::Out && capture_kind(fade_kind_); }
  float interp_alpha(float alpha) const { return exit_frozen() ? 1.0f : alpha; }
  Trans trans_ = Trans::None;
  PendingJump jump_;
  int trans_wait_ = 0;
  // Op 68 SET_SCREEN_RGB abr, r, g, b, frames: a full-screen flat quad of colour (r, g, b) in
  // 0..255, drawn with PS1 semi-transparency mode `abr` (2 = subtract), its colour interpolated linearly from the current value to the
  // target over `frames` ticks (0 = 1).
  struct ScreenFog {
    std::array<float, 3> cur{0, 0, 0}, step{0, 0, 0}, target{0, 0, 0};
    int left = 0;
    int abr = 2;
  } fog_;
  RoomLighting lighting_;
  // op 87: colour of actors standing on water triangles (128 = neutral)
  std::array<int, 3> underwater_rgb_{32, 32, 180};
  // op A4: slot push of submerged model faces; -1 = the room default, layer 0's z
  int water_bias_ = -1;
  // floor shadows
  ActorShadows shadows_;
  bool shadows_ok_ = false;
  // art mode: party actors use their 4x texture
  bool hd_ = true;

  // audio
  audio::Mixer* mixer_ = nullptr;
  audio::Sfx* sfx_ = nullptr;
  audio::LogSfx log_sfx_;
  // The sfx sink in use: the real one, or the logging stand-in when none is attached.
  audio::Sfx& sfx_out();
  // True when `e` indexes an entity of this room.
  bool valid_ent(int ent) const { return ent >= 0 && ent < static_cast<int>(ents_.size()); }
  // The entity's current pose, null when `ent` indexes no entity of this room.
  const ActorPose* pose_of(int ent) const { return valid_ent(ent) ? &ents_[static_cast<size_t>(ent)].actor.current() : nullptr; }
  // FE 22 / FE 23: per C6 slot, the volume and pan the slot's sounds slide to.
  struct SfxSlide {
    float cur = 127, target = 127, step = 0;
    int left = 0;
  };
  std::array<SfxSlide, 8> sfx_vol_, sfx_pan_ = {SfxSlide{64, 64, 0, 0}, SfxSlide{64, 64, 0, 0}, SfxSlide{64, 64, 0, 0}, SfxSlide{64, 64, 0, 0},
                                                SfxSlide{64, 64, 0, 0}, SfxSlide{64, 64, 0, 0}, SfxSlide{64, 64, 0, 0}, SfxSlide{64, 64, 0, 0}};
  void step_sfx_slides();
  bool bgm_remastered_ = true;
  // volume of the next PLAY_SONG (FE 78 sets it)
  int song_volume_ = 127;
  // volume of the playing song (FE 76 reads it; FE 1D / play set it)
  int bgm_volume_ = 127;
  // last song the script asked for (replayed when BgmSource switches back to remastered)
  int last_song_ = -1;
  // song whose start the mixer has not confirmed yet (WAIT_SONG)
  int pending_song_ = -1;
  int pending_ticks_ = 0;
  // FE 29: C6 slots kept across the next room change
  u32 sfx_keep_slots_ = 0;

  // scripted menus / movies
  MenuState menu_state_ = MenuState::None;
  MenuRequest menu_req_;
  // FE 75's round (the stored byte register) and the Grand Slam state.
  int picker_round_ = 0;
  std::array<u8, 9> grand_slam_table_{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::array<int, 3> monster_party_{-1, -1, -1};
  // Scroll in effect before the picker opened.
  int picker_scroll_x_ = 0, picker_scroll_y_ = 0;
  MovieState movie_state_ = MovieState::None;
  std::function<bool(script::NamedOp, std::string_view)> effect_tap_;
  std::function<void(int)> on_key_item_;
  MovieRequest movie_req_;
  // the sequence the playing movie started ("" = none)
  std::string movie_sequence_;

  // room effects
  void build_effects();
  void clear_effects();
  // effect/particle.prd (shared sprite library)
  assets::RoomEffects lib_fx_;
  bool lib_loaded_ = false;
  // effect/particle.prd > nested pack 'fenc' (same registry): the encounter sequences enct / enc2, emitters e00 e01 e04
  // e041 e06 e061 and the meshes enc0 / enc1 (sprite type 14, drawn by draw_effects).
  assets::RoomEffects fenc_fx_;
  std::optional<assets::StageAsset> fenc_meshes_;
  // the room .prd's type-0x02 mesh objects (`elem` ...)
  effects::RoomMeshes room_meshes_;
  // emitter op 0x39 / path selectors through the field getter
  bool effects_actor_basis(int selector, int point, effects::Basis& out) const;
  effects::EffectBank bank_;
  // the three light slots the room's type-17 emitter objects write (FE 64 actors are lit by them)
  effects::LightTable lights_;
  // Back colour of the lit actors: 0 at room init, then the pack's type-0x24 entry and
  // sequence op 0x65 ramps set it; opn_a01i's lift pack sets (159, 160, 115).
  struct BackColor {
    std::array<int, 3> cur{}, from{}, to{};
    int elapsed = 0, ticks = 0;
    bool ramping = false;
  } bk_;
  void bk_set(int r, int g, int b, int ticks);
  void bk_step();
  std::unique_ptr<effects::EmitterSystem> emitters_;
  std::unique_ptr<effects::SequencePlayer> sequences_;
  effects::GpuEffects gpu_fx_;
  render::ParticleBatch particles_;
  bool particles_ok_ = false;
  bool effects_gpu_ = true;
  bool effects_logged_ = false;
  size_t last_quads_ = 0;
  u32 seed_ = 0;

  // battle handoff
  BattleHandoff handoff_;

  // dialogue
  bool choice_pending_ = false;
  bool portrait_left_ = false;
  int flags_hi_for(int flag_byte) const;
  void place_dialogue_window(ui::DialogueDemo& dlg, int speaker, int message, int flag_byte, bool portrait);
  // the open window is a label (non-modal: the player keeps control)
  bool label_ = false;
  // entity whose script opened it
  int dialogue_owner_ = -1;
  int dialogue_face_ = -1;
  int last_dialogue_msg_ = -1;
  int pending_choice_first_ = 0, pending_choice_last_ = -1;

  // debug
  FieldDebug debug_;
  int auto_target_ = -1;
  bool auto_teleported_ = false;
  std::map<std::string, u64> stub_hits_;
  // Softlock watchdog (step_watchdog): a script wait or a player input lock that nothing but the script itself can end, held for
  // `stuck_limit_` ticks, is logged with the entity, script, pc and op. The Debug/Release builds both run it; OC_STUCK_TICKS overrides.
  void step_watchdog();
  FieldWatchdog watchdog_;
  // The entity-side cause of one stuck record, for the watchdog's report.
  std::string describe_stuck(const script::FieldVM::StuckInfo& stuck);
 public:
  // reports of this session (sweep / tests)
  const std::vector<std::string>& stuck_log() const { return watchdog_.stuck_log; }
  void set_stuck_limit(u32 ticks) { watchdog_.stuck_limit = ticks; }
  // Sweep probe: starts ON_ACTION (class 3) of `ent`, as a talk would; false when the script cannot start.
  bool probe_action(int ent) { return vm_.trigger_action(ent); }
  // [Debug] SweepTalk: starts the ON_ACTION and then ON_COLLISION script of every non-party entity of the loaded room, one after the other,
  // presses confirm every 10 ticks and runs `ticks` ticks each (battles are won, movies and menus end at once, a map jump request
  // reloads the room). Scripts that wait for ever are logged by the softlock watchdog (set OC_STUCK_TICKS below `ticks`).
  void sweep_talk(int ticks);
 private:
  // LOAD_ANIMATION packs (clips + sequence table) by char/name
  std::map<std::string, ExPack> ex_pack_cache_;
  // FE 2B / FE 34 footstep globals; reset_room_sound_regs restores the room defaults (the floor types persist across rooms).
  struct FootstepRegs {
    s32 volume = 0x7F, dry_type = 0, water_type = 0, listener_x = 0, listener_y = 0, listener_z = 0, nearest = 0x7FFF, farthest = 0x7FFF;
  } footstep_;
  u32 rng_ = 0x2545F491u;

  // ---- engine state behind script ops (B5 D0 D1 D2 D9 DA DB B2 FE 52 FE 02 FE 1A FE 1B FE 77 C5 C8
  // FE 43 8F 90 AC FE 03 FE 45 92 and anim-sequence op 1D)
  // 8F ramp / phases
  void step_wave();
  // D6-attached entities on their parent's joint (FE 46 / FE 47)
  void step_attached();
  // OC_ATTACH_DRIFT: logs attached models drawn away from their parent's attach point
  void check_attach_drift() const;
  // walk-flag 0x800 terrain push, strength set by B5
  void step_push();
  // anim-sequence op 1D followers
  void step_bone_emitters();
  // C5 / C8 / FE 43 records
  void step_sound_emitters();
  void defeated_enter_room();
  // fieldobj/player folder of a character model byte (file id 2b + 2)
  std::string model_folder(int model_byte) const;
  // character record model byte (D9 / DB)
  int model_byte(int char_id) const;
  // B5; room init 5
  int push_strength_ = 5;
  // FE 02: transition of the next room change; 0 after use
  int exit_kind_ = 0;
  // kind of the running room transition (0 black: subtract, 2 white: add)
  int fade_kind_ = 0;
  // 3 x {room, defeated-monster mask}, most recent first
  struct DefeatedRec {
    int room = -1;
    u32 mask = 0;
  };
  std::array<DefeatedRec, 3> defeated_{};
  // D1 ordinal of the room (zeroed at room init)
  int d1_counter_ = 0;
  // B2: the party tables are saved
  bool party_override_on_ = false;
  std::vector<int> saved_party_chars_;
  std::array<int, 3> saved_party_ent_{-1, -1, -1};
  // room_loads_ when B2 saved the tables (the entity table only means something in that room)
  u64 saved_party_room_ = 0;
  // D9 -> DA: party slot whose model is replaced, -1 none
  int model_swap_slot_ = -1;
  // 8F / 90: screen ripple parameters, 16.16
  struct BgWave {
    bool active = false;
    bool stop_at_end = false;
    std::array<s64, 6> cur{}, step{};
    int frames = 0;
    int ot = 7;
    s32 phase_x = 0, phase_y = 0;
  } wave_;
  // op 92: background group key -> its tiles' semi-transparency bit
  std::map<u32, bool> bg_group_abe_;
  bool smooth_rings_ = false;
  // set_draw_slab: ordering-table slab of the current draw pass
  float slab_min_ = -1e9f, slab_max_ = 1e9f;
  bool in_slab(float slot) const { return slot > slab_min_ && slot <= slab_max_; }
  // FE 02 kinds 1 / 3 / 4 / 5 / 6.
  struct FrameCapture {
    // asked the host for the next presented frame
    bool want = false;
    // the frame arrived
    bool ready = false;
    // drawn over the new room
    bool active = false;
    int kind = 0;
    // ticks of the frozen old picture before the room switch (4 frames)
    int hold = 0;
    // brightness 16.16 (0x800000 = 0x80), step 0x800000 / D per tick
    s32 acc = 0, step = 0;
    // kind 5 (x) / kind 6 (y): +12 per tick
    int slide = 0;
    std::vector<u8> px;
    u32 w = 0, h = 0;
    // px not uploaded yet
    bool dirty = false;
    render::Texture tex;
    render::TexRef set;
  } capture_;
  // During the 4 frozen frames the room itself still shows the captured picture (nothing ticks), so only the new room draws it.
  bool capture_shown() const { return capture_.active; }
  static bool capture_kind(int kind) { return kind == 1 || kind == 3 || kind == 4 || kind == 5 || kind == 6; }
  void step_capture();
  void draw_capture(render::SpriteBatch& sprites, float vw, float vh);
  // op 1D bone-attached emitter follower
  struct BoneFollower {
    int ent = -1, point = 0, mode = 0;
    // flag 0x80: re-placed every tick
    bool follow = false;
    std::array<s16, 3> off{}, ang{};
    // the emitter header's duration
    int life = 0;
    effects::Basis basis;
  };
  // anchor ids above the entity range
  static constexpr int kBoneAnchorBase = 0x1000;
  std::map<int, BoneFollower> bone_followers_;
  int next_bone_anchor_ = kBoneAnchorBase;
  bool bone_basis(BoneFollower& follower);
  // C5 / C8 sound record (8 slots)
  struct SoundEmitter {
    // sound id (0xFF = free; bit 0x100 = global effect00 id)
    int id = 0xFF;
    // source entity (-1 = fixed point)
    int src = -1;
    int x = 0, y = 0, z = 0;
    bool oneshot = false;
    int volume = 0;
    bool started = false;
    int ramp_frames = 0;
    bool ramp = false;
    // 16.16
    s32 ramp_value = 0, ramp_step = 0;
    // voice slot 0x10..0x17
    int voice = 0;
  };
  std::vector<SoundEmitter> sound_emitters_;

  // ---- field status / HP effects and footsteps
  struct FieldStatus {
    // poison drain counter
    int poison_ticks = 0;
    // flu timer and its random extra
    int flu_ticks = 0;
    int flu_next = 0;
    // damage-floor accumulator (walk-mesh flag 0x10000)
    int floor_acc = 0;
    // frames of the damage jolt (screen warp) left
    int jolt = 0;
    // flu heading wobble phase
    u32 flu_phase = 0;
    // footstep channel toggle (mask 0x40 / 0x80)
    int step_voice = 0;
    // own generator for the PS1's rand() calls here, so NPC idling (rng_) keeps its sequence
    u32 rng = 0x6C078965u;
    // debug counters
    u64 floor_hits = 0, poison_hits = 0, steps = 0;
  } fstat_;
  void for_char_targets(int ref, const std::function<void(save::SaveChar&)>& fn);
  // OR of the party members' status words
  int party_status_bits() const;
  // the per-tick status block
  void step_status();
  // damage-floor accumulator while the pad moves the player
  void status_on_move(int ent, u32 tri_flags, bool run);
  // heading offset of the flu wobble (0 without flu)
  int status_flu_wobble();
  // sprain: the player cannot run
  bool status_no_run() const;
  // defaults of the footstep registers
  void reset_room_sound_regs();
  void play_footstep(int ent, int feet);
  // see draw_background_late
  float late_layer_z() const;
  // the jolt's n (7 .. 0) for the frame being drawn, -1 = none
  int jolt_n_ = -1;
  // status icons: PS1 systim.bin TIM 8 (classic), hd/map/etc/system-*.png (HD)
  struct StatusIcon {
    render::Texture tex;
    render::TexRef set;
    // PS1 pixels
    float w = 0, h = 0;
    float u0 = 0, u1 = 1;
  };
  std::array<StatusIcon, 4> icons_ps1_{}, icons_hd_{};
  bool icons_tried_ps1_ = false, icons_tried_hd_ = false;
  render::Sampler icon_sampler_;
  void load_status_icons(bool hd);
  void free_status_icons();
  void draw_status_icons(render::SpriteBatch& sprites, float vw, float vh, bool ps1 = false);
};

}  // namespace oc::game
