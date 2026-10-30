#pragma once
// Battle presentation: the stage, the party and enemy models, the HUD and the command menu on top of the headless battle::Battle.
//
// Usage:
//   BattleView view;  view.init(ctx, vfs, sprites, meshes, tables, mixer);  view.start(scene_id, {});
//   per frame:        view.update(dt, input);   ... ctx.begin_frame(frame) ...  view.draw(frame);
// update() runs the 60 Hz visual tick (interpolated at draw time). The rules run only when the player confirms a command
// (Battle::apply also runs the enemy turns): the ActionEvents they produce are queued and replayed by the animation-sequence scripts
// of the models (battle_seq.h: approach, attack, hit reaction, return), so HP, numbers and KOs change at the moment the script says.
//
// Space: the stage is y up in model units; skinned models (y down in their own space) are mirrored with S(1,-1,1).
// Camera: BattleCamera (battle_camera.h) plays the stage's intro sequence, then the "zen0" command shot, and the per-action shots;
// drawn into the field's picture frame (320 x 224 buffer, Letterbox::field; see picture()). HUD: 1280x960 canvas scaled into the 4:3 letterbox.
//
// The implementation is split by concern: battle_view.cpp (setup and loading), battle_model_cache.cpp (unit / weapon model loading),
// battle_view_camera.cpp (camera basis, tick), battle_view_hud.cpp (HUD art and the draw-op list), battle_view_draw.cpp (frame drawing),
// battle_view_action.cpp (the real-time command flow), battle_view_script.cpp (the battle-script stream and message window),
// battle_view_units.cpp (unit scripts, movement, hits), battle_view_sequence.cpp (action playback and the finish), battle_view_anchor.cpp
// (camera reference points, audit), battle_view_anm.cpp (anmseq emitters), battle_view_pseudo.cpp (the effect pack's pseudo-actor),
// battle_view_fx*.cpp (element effect packs: scene, cast, state, lights, skin, draw), battle_view_ui.cpp (menus / HUD),
// battle_view_trail.cpp (weapon trails and per-unit model state), battle_transition.cpp (the field -> battle swirl).
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <functional>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/battle_stage.h"
#include "audio/mixer.h"
#include "battle/battle.h"
#include "battle/pac_vm.h"
#include "data/tables.h"
#include "game/actor.h"
#include "game/battle/battle_audio.h"
#include "game/battle/battle_audit.h"
#include "game/battle/battle_camera.h"
#include "game/battle/battle_model_cache.h"
#include "game/battle/battle_numbers.h"
#include "game/battle/battle_spell_lights.h"
#include "game/battle/battle_fx.h"
#include "game/battle/battle_fx_textures.h"
#include "game/battle/cast_header.h"
#include "game/effects/gpu_effects.h"
#include "game/effects/light_extract.h"
#include "render/particles.h"
#include "game/battle/battle_pose.h"
#include "game/battle/battle_seq.h"
#include "platform/window.h"
#include "render/camera.h"
#include "render/mesh_pipeline.h"
#include "render/sprite_batch.h"
#include "render/render_target.h"
#include "render/stage_pipeline.h"
#include "game/battle/stage_scene.h"
#include "import/text.h"
#include "render/text_batch.h"
#include "ui/ps1_font.h"
#include "vfs/vfs.h"

namespace oc::game {

class BattleView {
 public:
  BattleView();
  ~BattleView();
  BattleView(const BattleView&) = delete;
  BattleView& operator=(const BattleView&) = delete;

  // `tables` and `mixer` (optional) must outlive the view. The mixer plays the battle theme (when the field did not keep its music)
  // and the sound effects.
  bool init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::MeshPipeline& meshes,
            const data::GameTables& tables, audio::Mixer* mixer = nullptr);
  // Waits for the GPU; also called by the destructor.
  void shutdown();

  // Loads the scene's stage (battle/bg/bg_NN) and models and starts the rules. `party` empty = Serge, Kid, Harle.
  bool start(int scene_id, std::vector<battle::PartyMember> party = {});
  // What the field hands over with a battle: NG+, the party's growth state and the bench (for a star-up), and the BATTLE opcode flags.
  struct StartOptions {
    bool ng_plus = false;
    bool gold_bonus = false;
    battle::GrowthState growth;
    std::vector<battle::PartyMember> bench;
    // The Dopplegang pool bits.
    std::array<u16, 16> dopple_mask{};
    // Van's piggy-bank savings (script variable 0x176).
    int piggy_bank = 0;
    // The Grand Slam picks: monster records of the party slots in a monster-party scene (battle::BattleOptions::monster_party).
    std::array<int, 3> monster_party{-1, -1, -1};
    // The battle id word: an encounter id < 0x200, or scene + 0x200 for a scene started directly; -1 = scene + 0x200.
    int battle_id = -1;
    // The battlefield operand (stage bg_NN); -1: the scene record's default.
    int battlefield = -1;
    // 1 keep the field music, 2 losable (a wipe is not game over), 4 dragoon disguise (every PC except Poshul / Mojo uses model 0x3B),
    // 0x10 no victory music, 0x20 the field restarts no music after the battle (host side).
    int flags = 0;
  };
  bool start(int scene_id, std::vector<battle::PartyMember> party, const StartOptions& opt);
  // start() in slices. The host calls begin_load() once the field hands over (the swirl / the frozen hold), then load_step() every frame
  // (a time budget in ms; at least one step), and start() with the same scene finishes in a few ms instead of loading everything on the
  // take-over frame. prefetch_fonts() reads the two ~190 MB JP fonts on a worker thread (call it as soon as the encounter is requested).
  bool begin_load(int scene_id, std::vector<battle::PartyMember> party, const StartOptions& opt);
  // True when the load is complete.
  bool load_step(double budget_ms);
  bool prepare_fx_targets(u32 w, u32 h);
  bool load_complete() const { return !load_ || load_->done; }
  bool loading() const { return load_ && !load_->done; }
  void prefetch_fonts();
  // Effects-library init + JP font prefetch on worker threads (host: at title / room load; begin_load repeats it as a fallback).
  void warm_up();
  // [Video] Filter: bilinear or nearest sampling of the battle 3D (stage objects, units, weapons). Until the host calls it the art mode
  // decides (HD bilinear, original nearest).
  void set_texture_filter(bool linear) { linear_filter_ = linear ? 1 : 0; }
  // [Video] WideHud: layer columns per side the HUD edge groups move out by.
  void set_wide_hud_columns(float columns) { wide_hud_cols_ = std::max(0.0f, columns); }
  // [Enhancements] SmoothEffectRings (off under Ps1Res), BattleShadows, SpellLights.
  void set_smooth_rings(bool on) { smooth_rings_ = on; }
  void set_smooth_fog(bool on) { stage_pipe_.set_smooth_fog(on); }
  void set_spell_lights(bool on) { spell_.lights_on = on; }
  // force: rebuild even if `hd` is unchanged (HD <-> Modded). HD: large_font_*; original: small_font_* (the HUD art is RDE's in both modes).
  void set_art_mode(bool hd, bool force = false);
  // Element descriptions (sysmes 0x1A rows) of the element menu.
  void set_language(import::Lang lang) { lang_ = lang; }
  bool set_bgm_remastered(bool on) { return audio_.set_remastered(on); }
  // [Debug] BattleAuto: every turn the first ready PC attacks the first enemy with a fierce hit (chain of one), and the result page and
  // battle-script message windows confirm themselves (headless runs).
  void set_auto(bool on) { auto_ = auto_confirm_ = on; }
  // Cheats "Auto battle" (RDE F5): the command picks only; the results page and message windows stay manual. Call after set_auto each frame.
  void set_auto_commands(bool on) { auto_ = auto_ || on; }
  // Settings rows "Cursor Position" and "Accuracy Selection", pushed by the host every frame.
  void set_menu_options(bool cursor_memory, bool auto_accuracy) { cursor_memory_ = cursor_memory, auto_accuracy_ = auto_accuracy; }
  // Dev: skip the intro and open a menu as soon as a PC can act (1 element grid, 2 attack chain, 3 element grid -> target selection, 4 cast an element, 5 run, 6 wipe the party).
  void debug_ui(int mode) { debug_ui_ = mode; }
  // Dev hooks that config.* can wire ([Debug] BattleCast=<element id>, BattleFx=0/1).
  struct Debug {
    // > 0: skip the intro and let the first PC cast this element on the first enemy as soon as the view ticks.
    int cast_element = 0;
    // false: element casts use the generic path (no effect packs).
    bool effects = true;
    // cast_element: the first enemy casts it on the first PC (instead of PC 0 on the first enemy).
    bool enemy_cast = false;
    // >= 0: skip the intro, the lead PC attacks the first enemy with this strength (0 weak, 1 medium, 2 heavy).
    int weapon = -1;
    // Camera overlay: active shot, eye / target, axis gizmo at each unit (F9 toggles it at run time); overlay_key: F3 toggles it too
    // (ocbattleview only: in the game F3 is RDE's encounters-off key).
    bool overlay = false;
    bool overlay_key = false;
    // Both >= 0 with cast_element: that unit casts it on that unit (Debug tab; an all-targets element hits that side).
    int cast_actor = -1, cast_target = -1;
    // The PC unit that casts cast_element (without cast_actor) and makes the weapon attack.
    int lead = 0;
    // The next start() runs without the scene's battle script (Debug tab: replaced enemies do not match the script's units).
    bool no_script = false;
    // Debug tab: Battle::set_invincible, applied at the start and every tick.
    bool inv_party = false, inv_enemies = false;
  };
  void set_debug(const Debug& debug) { dbg_ = debug; }
  // [Debug] Showcase: the next start() puts every playable character on the stage in its battle stance and hides the battle's units.
  void set_showcase(bool on) { showcase_on_ = on; }
  const Debug& debug() const { return dbg_; }
  // Effect state for tools / tests.
  bool fx_active() const { return fx_ != nullptr; }
  int fx_casts() const { return fx_casts_; }

  void update(float dt, const platform::Input& in);
  // Pause screen over the battle: update() returns at once (no sim ticks, no effect ticks, no UI time), the draw shows the last tick's
  // state at interpolation alpha 1.
  void set_paused(bool on) { paused_ = on; if (on) alpha_ = 1.0f; }
  bool paused() const { return paused_; }
  // RDE: the pad's Select held during a cast hides the HUD, menus and windows (the damage digits stay). Pushed by the host every frame.
  void set_select_held(bool held) { select_held_ = held; }
  // [Enhancements] AutoHideCastHud: the same hiding during every cast, without Select.
  void set_autohide_cast(bool on) { autohide_cast_ = on; }
  // [Enhancements] RetainBattleScenery: the stage's `bti0` layer is not stopped when the intro ends (nor by a pack's op 0x60).
  void set_keep_intro_layer(bool on);
  // Call between ctx.begin_frame() and ctx.end_frame(). `sprites_resume` = another sprite pass already ran this frame.
  // draw() = draw_scene() at window resolution into picture(), then draw_ui().
  void draw(const render::Frame& frame, bool sprites_resume = false);
  // The battle display: the same 320 x 224 buffer / screen as the field with the projection centre at (160, 112), so RDE presents buffer
  // rows 0..223 in the field frame (render::Letterbox::field). kBufferRows = the buffer height, kGeomOffsetY = the projection centre row.
  static constexpr float kBufferRows = render::kFieldRows, kGeomOffsetX = 160.0f, kGeomOffsetY = 112.0f;
  static render::Letterbox picture(float vw, float vh) { return render::Letterbox::field(vw, vh); }
  bool scene_visible() const { return ready_ && battle_ && !trans_run_; }
  int scene_id() const { return scene_id_; }
  // The 3D pass: depth cue, stage, unit shadows, units, effects, mapped by `lb` = picture() of the window or Letterbox::identity(320, 224)
  // for a PS1-resolution target ([Video] Ps1Res; the host scales it up into picture()). Clipped to the 320 x 224 buffer.
  void draw_scene(const render::Frame& frame, const render::Letterbox& lb, bool sprites_resume = false);
  // The 2D pass at window resolution over the scene: screen fade, HUD / menus / text, damage digits, the start white-out.
  // `lb` (optional) = the PS1-resolution pass ([Video] Ps1Res): a Letterbox::identity(1280 + 8 e, 960, 4) over a (320 + 2 e) x 240 target (view = canvas
  // units, canvas x from -4 e), replacing the window's 4:3 fit; `text_batch()` is the batch its pixel grid is set on.
  void draw_ui(const render::Frame& frame, bool sprites_resume = false, const render::Letterbox* lb = nullptr);
  render::TextBatch* text_batch() { return &text_; }

  // Field -> battle transition (battle_transition.cpp). The host calls begin_transition() on the frame the field starts to fade out for a
  // battle: the next finished frame is captured, then spirals / zooms / fades for 0.8 s. update() holds the battle (no ticks) and draw()
  // skips the scene while it runs; the host calls update_transition(dt) every frame and draw_transition() after drawing the field.
  // `carry`: the field's last (smeared) output picture; the battle's first ticks blend out of it.
  // `main_clock`: the field's strip bracket clock at the take-over (FieldWorld::transition_main_clock); the battle continues it (encounter_strips.h).
  void begin_transition(render::GpuContext& ctx, render::TexRef carry = {}, int main_clock = 0);
  void update_transition(float dt);
  void draw_transition(const render::Frame& frame, bool sprites_resume = false);
  bool transition_active() const { return trans_wait_ || trans_run_; }

  bool active() const { return battle_ != nullptr && !finished_; }
  // Result panel confirmed / escaped / game over confirmed.
  bool finished() const { return finished_; }
  // Route vocabulary (`assert battle events`): the order of the script windows and actions since the view started: "w<msg>" = battle script window <msg> opened,
  // "a" = a queued action began (begin_group).
  const std::vector<std::string>& route_events() const { return route_events_; }
  // The host shows the btlexit results screen (BattleResults) after a victory: the view finishes right after the victory pose /
  // camera sequence without its own result panel and leaves the victory music playing (stop_music() ends it when that screen exits).
  void set_external_results(bool on) { external_results_ = on; }
  void stop_music() { audio_.stop_music(); }
  battle::Outcome outcome() const { return battle_ ? battle_->outcome() : battle::Outcome::Ongoing; }
  const battle::Battle* battle() const { return battle_.get(); }
  // Dev tools (ocbattleview --enemyhp).
  battle::Battle* battle_mut() { return battle_.get(); }

 private:
  // ---- camera ------------------------------------------------------------------------------------------------------
  struct Cam {
    std::array<float, 3> eye{0, 0, 0}, right{1, 0, 0}, down{0, 1, 0}, fwd{0, 0, 1};
    float h = 512;
    // Projection centre = the geometry offset + the effect screen shake (a display-window shift).
    float ox = 160.0f, oy = 112.0f;
    render::Mat4 view() const;
    render::Mat4 view_proj(const render::Letterbox& lb) const;
    // -> PS1 screen (GTE: (160, 112) + H * xy / z in the 320 x 224 buffer).
    bool screen(float x, float y, float z, float& sx, float& sy) const;
    // -> 1280x960 canvas, as the picture shows that screen point (picture rows 0..224 = canvas [8, 952]): damage digits, debug gizmos.
    bool project(float x, float y, float z, float& cx, float& cy) const;
  };

  // ---- scene -------------------------------------------------------------------------------------------------------
  // The per-unit presentation state. Positions are world units, y up; `p*` members hold the previous tick's value for draw interpolation.
  struct Visual {
    std::shared_ptr<ModelHandle> model;
    bool has_model = false;
    UnitPose pose;
    std::shared_ptr<ModelScripts> scripts;
    // BattleAudio group of this unit's at0.prd (weapon-class renamed tags).
    std::string sound_key;
    bseq::Vm vm;
    // enum Script.
    int script_kind = 0;
    bool script_done = true;
    // Ring position and the previous tick's.
    float x = 0, z = 0, px = 0, pz = 0;
    // Where the model stood when its joint pass was last skipped (hidden): the frozen joint matrices carry that position.
    float hold_x = 0, hold_y = 0, hold_z = 0;
    // Height above the floor: the jump / PosMoveT arcs of the anmseq scripts.
    float y = 0, py = 0;
    float face_x = 1, face_z = 0, pface_x = 1, pface_z = 0;
    float home_x = 0, home_z = 0, home_fx = 1, home_fz = 0;
    // A scripted move: x / z linear from (fx, fz) to (tx, tz) over `total` ticks; y linear between fy and ty (PosMoveT / MoveT), or the
    // ballistic arc of JumpT when `jump`: y (PS1 down) += vy per tick after a half-tick back step on the first tick, vy += g.
    struct Move {
      bool on = false;
      float fx = 0, fz = 0, tx = 0, tz = 0;
      float fy = 0, ty = 0;
      int total = 0, left = 0;
      bool to_home = false;
      bool jump = false, first = true;
      float vy = 0, g = 0;
    } move;
    // RotateT: the yaw (radians) interpolated from `from` to `to` over `total` ticks.
    struct Turn {
      bool on = false;
      float from = 0, to = 0;
      int total = 0, left = 0;
    } turn;
    // The idle script was started with the guard flag.
    bool guard_shown = false;
    // The running script is the spawn script (S5 entry 8, start_spawn): its fade-in is not cut by an idle restart; idle_owed = an idle
    // restart arrived while it ran, done when it ends.
    bool spawn = false;
    bool idle_owed = false;
    // Elements the spawn script froze / set turning (battle-long tasks a cut script keeps).
    unsigned long long spawn_joints = 0;
    // PC weapon mesh (weapon pack part), drawn rigidly at attach point `weapon_attach`.
    std::shared_ptr<ModelHandle> weapon;
    int weapon_attach = -1;
    // A prism weapon (tier byte 5) loaded at battle start: drawn under three turning coloured lights. A later model swap draws it plain.
    bool weapon_prism = false;
    // The colour byte of the unit's control block (anmseq op 0x58): the innate as of the last model build, rewritten by Vary Innate only. A
    // script's own innate write (Time Devourer's phases) and Pip's new form colour before its model swap leave it as it was.
    u8 attrib = 0;
    // The clips of an effect pack's type-0x19 entry ('tech' / 'tec1'..: a tech's own caster animation) hung on this unit as its external
    // clip set (SetAnim 0x80 + k) while the pack runs; dropped at the pack end (fx_drop_clip_packs).
    std::vector<assets::AnimationClip> tech_clips;
    // Model extent along y (the height word).
    float height = 1000;
    // Horizontal reach of attach points 15 (weapon) / 16 (hit point).
    float reach15 = 0, reach16 = 0;
    // Animated HP shown by the bars, and the HP they chase: set when a hit lands (CallDamage), synced when an action ends.
    float hp_disp = 0;
    int hp_shown = 0;
    // Scripted death: the KO hit landed but the death cast has not ended; scripts see the unit alive.
    bool death_owed = false;
    // The KO is on screen (set by the hit that causes it).
    bool ko_shown = false;
    // Hit flash 1 -> 0, and the enemy fade-out after the KO.
    float flash = 0, flash_prev = 0;
    float fade = 1, fade_prev = 1;
    // The hit this unit is reacting to.
    battle::ActionEvent react_ev;
    bool has_react = false;
    // The idle script's pose word, set by the battle script's pose op: 1 = stand-by, 8 = dead.
    int script_pose = 1;
  };
  enum Script { kNone = 0, kIdle, kApproach, kAttack, kReturn, kReact, kCast, kRelease, kRun, kVictory };
  // One CallDamage number (battle_numbers.h): its digits are view-space particles, fixed on the screen from the spawn on. kind 0 = damage
  // to an enemy, 1 = damage to a PC, 2 = heal, 3 = miss; `code` picks the flight emitter (0 / 2 damv, 1 gurv, 3 gaiv); (ax, ay) = the anchor
  // in PS1 px from the projection centre (0 = the single-target layout); `delay` = the overlap delay of the offset layout (16 per live
  // overlapped number); `end` = ticks until the last digit's flight has ended.
  struct Pop {
    int value = 0, kind = 0;
    float age = 0, age_prev = 0;
    int code = 0;
    int ax = 0, ay = 0;
    int delay = 0;
    int end = 0;
  };
  bnum::Overlap pop_rects_;
  // The caster's own number (anmseq op 0x6B) uses the offset layout.
  bool pop_caster_layout_ = false;
  struct LogLine {
    std::u32string text;
    float ttl = 0;
  };
  enum class Ui { Intro, Top, Chain, Element, Target, Wait, Result, Dopple };
  struct Op;
  float wide_hud_cols_ = 0.0f;

  // One presented action: all events of one player command / enemy turn. `hand` = the attack-hand toggle the atk script reads (flipped at
  // the start of every attack); `targets` = unit indices of the current hit (script target index -> unit); `casters` = the acting list:
  // [actor] or a monster cast's group, index = the script variant; `cast_after_return` = an element cast starts after the return script
  // (S5 entry 3) has run; `hit_wait` = the next hit's queue_attack was not released yet by the script stream (script_gated).
  struct Act {
    enum class Phase { None, Approach, Hit, Return, Cast, Release, Defend, Run, Settle } phase = Phase::None;
    std::vector<battle::ActionEvent> ev;
    size_t hit = 0;
    int actor = -1;
    int ticks = 0;
    int hand = 0;
    std::vector<int> targets;
    std::vector<int> casters;
    bool cast_after_return = false;
    bool hit_wait = false;
  };
  // A reaction scheduled for a later tick (element casts hit one target after the other).
  struct Pending {
    int unit = 0, delay = 0;
    battle::ActionEvent ev;
  };

  // setup
  std::shared_ptr<ModelHandle> load_model_for(const std::string& dir_stem) { return model_cache_.load_model_for(*vfs_, *meshes_, dir_stem); }
  void prefetch_unit_models();
  std::shared_ptr<ModelHandle> load_weapon(int char_id, int part) { return model_cache_.load_weapon(*vfs_, *meshes_, char_id, part); }
  std::shared_ptr<ModelHandle> load_enemy_weapon(int model, int part) { return model_cache_.load_enemy_weapon(*vfs_, *meshes_, model, part); }
  int weapon_digit(const battle::Unit& unit) const { return ModelCache::weapon_digit(tables_, unit); }
  std::shared_ptr<ModelScripts> load_scripts_for(const std::string& dir_stem) { return model_cache_.load_scripts_for(*vfs_, &bank_, dir_stem); }
  std::string unit_model_stem(const battle::Unit& unit) const { return ModelCache::unit_model_stem(*vfs_, tables_, unit); }
  // Model, scripts, sound group, weapon and extents of a unit from its Unit record.
  void bind_unit_model(int unit);
  void place_units();
  void unload_scene();

  // showcase (battle_view_showcase.cpp)
  struct ShowcaseModel {
    std::shared_ptr<ModelHandle> model, weapon;
    int weapon_attach = -1, weapon_hide = 0;
    u32 part_hide = 0, part_show = 0;
    UnitPose pose;
    float x = 0, z = 0, height = 1000;
  };
  // Every model faces -x, the way the party faces the enemies.
  static constexpr float kShowcaseFaceX = -1.0f, kShowcaseFaceZ = 0.0f;
  void showcase_load();
  void showcase_tick();
  void showcase_shadows(std::vector<StageScene::ShadowCaster>& casters);
  void showcase_draw(const render::Mat4& view_proj, const render::Mat4& view);
  bool showcase_on_ = false;
  std::vector<ShowcaseModel> showcase_;
  void apply_camera(float alpha);
  // The command-menu orbit around the active PC at this tick.
  CamPose orbit_pose() const;
  // The DYNAMIC target-selection shot from the active PC toward `target`.
  CamPose dynamic_pose(int target) const;
  // The menu camera, once per tick while no camera task and no timeline is alive.
  void menu_camera();
  // The approach shot of an attack: apr0 / apr1 at random, apr2 / apr3 in the DYNAMIC camera mode.
  void select_approach();
  // The next PC with an open menu in slot order pc+1, pc+2, pc; -1 = none.
  int next_pc_after(int pc) const;

  // frame tick
  void tick();
  void tick_units();
  void tick_camera();
  // Shots / timelines torn down, a random `pli*` started.
  void start_party_intro(const std::string& pli);
  void tick_action();
  // OC_BATTLE_AUDIT=1 (battle_view_anchor.cpp): per-tick flow checks, one `AUDIT` log line per anomaly.
  void audit_tick();
  void audit_tables();
  BattleAudit audit_;
  // The menu state tick_camera() last saw (elm0 is selected when the element menu opens).
  Ui cam_ui_ = Ui::Intro;
  // The acting / target lists the camera anchors read (rewritten by the command setup, persist between actions), and whether each was ever
  // written (audit: the battle-start state is empty by design).
  mutable bool anchor_actors_set_ = false, anchor_targets_set_ = false;
  mutable std::vector<int> anchor_actors_, anchor_targets_;
  void handle_input(const platform::Input& in);

  // actions (battle_view_action.cpp, battle_view_script.cpp, battle_view_units.cpp, battle_view_sequence.cpp, battle_view_anchor.cpp)
  void execute(const battle::Command& command);
  void collect_events();
  void begin_group();
  // Attack + target: the PC runs up to the enemy and stays there; begin_disengage: the engaged PC returns home.
  void begin_engage(int pc, int target);
  void begin_disengage();
  void chain_pick(int strength);
  void start_hit();
  void start_return();
  void start_release();
  void end_group();
  bool start_script(int unit, const bseq::Table* table, int entry, int variant, Script kind);
  void start_idle(int unit);
  // The PC standing at its target between HIT% picks keeps the clip its approach / atk script left (clip 2).
  bool engaged_stance(int unit) const;
  // Model hidden, S5 entry 8 (the spawn script: part words, fade-in) instead of the idle.
  void start_spawn(int unit);
  void start_reaction(int unit, const battle::ActionEvent& event);
  void on_script_event(int unit, const bseq::Event& event);
  // number false: the hit lands (HP, KO) without CallDamage's number / flash.
  void on_damage(int unit, bool number = true);
  void fill_ctx(int unit, bseq::Ctx& ctx) const;
  // The unit an anmseq actor selector names: list 0 = the acting list (the caster; entry k >= 1 of a monster cast's list = the k-th other
  // unit of its group), list 1 = the targets, list 2 = the unit number itself. -1 = none.
  int script_list_unit(int list, int idx) const;
  void dev_group_hint(int actor, int element);
  // SetRot requests (unit, yaw) of this tick, applied after fx_tick().
  std::vector<std::pair<int, int>> pending_rot_;
  void set_rot_now(int unit, int yaw4096);
  int target_unit(int idx) const;
  float approach_distance(int self, int target, int attach = 15) const;
  void begin_move(int unit, float tx, float tz, int ticks, bool home, float ty = 0.0f);
  void begin_jump(int unit, float tx, float tz, float ty, int ticks, int mode = 0);
  // LinkEid: `victim` is shifted every tick so that its attach point `victim_attach` meets `carrier`'s attach point `carrier_attach`;
  // left 0 = until the action ends.
  struct Link {
    int carrier = -1, victim = -1, carrier_attach = 0, victim_attach = 0, left = 0;
  };
  std::vector<Link> links_;
  void tick_links();
  // The unit's position as the effects / cameras / shadows see it: the view's own placement, or where a bespoke overlay put the model
  // (FxModel::steer). `alpha` interpolates between the last two ticks (draw), 1 = the current tick.
  std::array<float, 3> unit_world(int unit, float alpha) const;
  void face_toward(int unit, int target);
  void face_point(int unit, float x, float z);
  void face_cast_targets(bool turn_targets);
  // The group facing of a cast (midpoint / origin) runs after the charge camera is selected (fx_charge_begin).
  bool face_pending_ = false;
  float face_px_ = 0, face_pz_ = 0;
  void sync_shown();
  void begin_finish();
  void tick_finish();
  CamFrame anchor_frame(int type, int attach) const;
  // The camera reference point of one unit for attach code `attach` (world, y up).
  bool unit_ref_point(int unit, int attach, std::array<float, 3>& point, float& yaw) const;
  // CallDamage's number spawn (battle_view_units.cpp).
  void spawn_pop(int unit, int value, int kind, int code);
  // The model's scale word / 4096, 1 without a model.
  float model_scale(int unit) const;
  void play_tag_sound(const std::string& tag, int volume);

  // unit-attached emitters of the anmseq scripts (op 0x1D; battle_view_anm.cpp)
  struct AnmFx;
  void anm_effect(int unit, const bseq::Event& event);
  // Every unit model's S6 DRP headers (resident registry file).
  void anm_add_s6(effects::EffectBank& bank) const;
  // The EmitterSystem + hooks of one anm bank.
  void anm_start_system(AnmFx& anm);
  // Op 0x1D on one bank (false = no attach point).
  bool anm_attach(AnmFx& anm, int unit, const bseq::Event& event, const std::string& name, int src);
  void anm_tick();
  // Dev object log: every anm bank.
  void anm_each_system(const std::function<void(const effects::EmitterSystem&)>& fn) const;
  void anm_draw(const render::Frame& frame, const render::Letterbox& lb);
  void anm_reset();
  std::map<std::string, std::shared_ptr<AnmFx>> anm_;
  int anm_pack_seq_ = 0;

  // weapon trails (ops 0x23-0x25 0x46 0x5B 0x85 0x86; battle_view_trail.cpp)
  struct TrailState;
  void trail_event(int unit, const bseq::Event& event);
  void trail_tick();
  void trail_draw(const render::Frame& frame, const render::Letterbox& lb);
  std::shared_ptr<TrailState> trail_;
  // per-unit model state of the scripts (show / hide, parts, model colour + semi-transparency, colour fade 0x8E): false = the model is hidden
  bool anm_unit_apply(int unit, render::MeshPipeline::DrawParams& dp) const;
  // Battle models are painter-sorted per face like every PS1 model: one ordering-table slot per face = average z >> 4, or the NEAREST
  // corner's z >> 4 for the parts whose shading flag has 0x10 (hair / capes laid over the body).
  static void unit_ot_faces(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp) {
    dp.ot_faces = true;
    for (size_t k = 0; k < asset.shading.size() && k < 32; ++k)
      if (asset.shading[k].flags & 0x10) dp.near_meshes |= 1u << k;
  }
  // The S3 shading entries as the stock model draw reads them: flag bit 0 = part hidden, 0x80 with 0x20 / 0x40 = env-mapped with the model's
  // own page, 0x20 / 0x40 without 0x80 = the stock env page (replace / over, ABR = flags bits 2-3), and the part colour word = model colour x
  // the entry's rgb >> 7 (0x80 neutral). Shared by the unit draw and the pseudo-actor (pa_draw).
  void shading_params(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp);
  // The model scale words applied to a model-space point.
  void anm_scale_point(int unit, float& x, float& y, float& z) const;
  bool anm_hidden(int unit) const;
  // The script-written model state (semi-transparency, colour) for the dev unit log.
  void anm_unit_probe(int unit, bool& semi, float rgb[3], bool& blink) const;
  // Dev unit log: the hidden parts as drawn.
  unsigned anm_part_hide(int unit, unsigned base) const;
  // The hidden bit of the anmseq model state.
  void anm_set_hidden(int unit, bool hidden);
  // The party eye blink bit as op 0x72 sets it.
  void anm_eye_arm(int unit);
  // The part words / blinks of the spawn script's first tick (S5 entry 8), also the state a cut script returns to.
  void anm_spawn_parts(int unit);
  // anmseq op 0x7A hid the attached weapon object `slot` (0 = the drawn weapon).
  bool anm_weapon_hidden(int unit, int slot) const;
  // A script cut short: its model state (semi-transparent / tinted / hidden parts / fade) and trail samplers end with it; keep_parts: the
  // part hide bits stay (nothing was cut).
  void anm_unit_reset(int unit, bool keep_colour = false, bool keep_parts = false);

  // element effects (battle_view_fx*.cpp)
  // start_release: starts the pack of the current element; false = generic path.
  bool fx_begin();
  // Cast start: cast pose, chant, the colour's charge pack "ch0N".
  void fx_charge_begin();
  static bool fx_dt_emulation();
  int fx_hand_delay() const;
  // Pass delta a freshly born pack task gets.
  int fx_task_credit() const;
  // Element pack ready: the charge is killed, the fog reset; fx_charge_done: the charge signalled op 0x41 (or has nothing left to play).
  void fx_charge_end();
  bool fx_charge_done() const;
  void fx_tick();
  // A bespoke overlay's SetAnim / anmseq requests on the models it steers. `at_init` = the requests of the overlay's init, applied in the
  // pack-start pass: a script it starts runs its first step at once (and the pack drive is left to the tick).
  void fx_model_requests(bool at_init = false);
  // FxModels::PackDrive: the overlay-driven pack model (saraman's 'sara').
  void fx_pack_drive_tick(effects::FxModels& ms);
  bool fx_skin_pack_driven(effects::FxModels& ms, effects::FxSkinnedModel& out);
  using FxTriSink = std::function<void(render::TexRef set, const float* x, const float* y, const float* u, const float* v, u8 r, u8 g, u8 b, int blend,
                                       int slot, u32 order)>;
  // FxModel::copies; fx_textures_.model_sets = sprite-batch sets of unit textures drawn by it.
  void fx_model_copies(const effects::ViewParams& view, const render::Letterbox& lb, u32 order, const FxTriSink& sink);
  FxTextureCache fx_textures_;
  // The overlays' private model draws: FxModels::skin, unit model textures and VRAM pages for FxPrim::TexSrc.
  bool fx_skin_unit(int unit, effects::FxSkinnedModel& out);
  // FxModels::attach.
  bool fx_attach_unit(int unit, int point, effects::Vec3& out, bool raw = false);
  void fx_skin_asset(const assets::ModelAsset& asset, const render::Mat4& model_mat, const effects::FxModel* om, effects::FxSkinnedModel& out);
  // FxModels::skin_pack.
  bool fx_skin_pack(effects::FxSkinnedModel& out);
  // TexSrc::PackModel.
  render::TexRef fx_pack_model_texture_set(int tex);
  // The pack model block as its objects last placed it.
  struct FxPackPlace {
    bool valid = false;
    std::string name;
    effects::Vec3 pos{0, 0, 0};
    effects::Mat3 mat;
    int clip = -1;
    bool loop = false;
    float motion = 0;
    int rgb[3] = {128, 128, 128};
  } fx_pack_place_;
  // Model space (PS1 axes) -> world (y up) as drawn.
  render::Mat4 fx_unit_matrix(int unit) const;
  // white: FxPrim::white silhouette variants.
  render::TexRef fx_unit_texture_set(int unit, int tex, u8 white = 0);
  render::TexRef fx_vram_page_set(u16 tpage, u16 clut);
  // The stock env page of the running stage.
  render::TexRef stock_env_page_set();
  // FxModels::vram_moves -> fx_textures_.vram (after each overlay tick).
  void fx_vram_apply();
  // FxModel::grey -> the unit model textures (restore = back to the originals); fx_grey_ per unit: applied k (-1 = original), STP mode.
  void fx_grey_apply(bool restore);
  std::vector<std::pair<int, bool>> fx_grey_;
  // FxModels::pad_edge collected between ticks (confirm = 0x20).
  u16 fx_pad_edge_ = 0;
  // fx_textures_.vram = the running pack's VRAM (FxPack::build_vram), rebuilt per pack.
  bool fx_vram_ready();
  void fx_end();
  // Lands the hit of one ActionEvent of the current act.
  void fx_fire(size_t event_index, bool number = true);
  // anmseq ops 59 / 5A while a pack runs: the target (idx, -1 = all) runs the reaction script (variant 7) of the pack's atk table.
  void fx_react(int idx, int caller);
  // CallDamage from a script while a pack runs: lands that unit's pending hit (true = consumed).
  bool fx_land(int unit);
  void fx_damage(int target_slot);
  void fx_other(const assets::SeqCmd& cmd);
  // Ops 0x43-0x46 / 0x4F / 0x57: anmseq entries on casters / targets; fx_actor_busy: op 0x50.
  void fx_actor_script(const assets::SeqCmd& cmd);
  bool fx_actor_busy() const;
  void fx_fog_tick();
  void fx_fog_reset();
  void fx_state_log(const char* when) const;
  // GTE depth cue over the backdrop / stage, on the picture of `lb`; `resume` = a sprite pass already ran this frame.
  void fx_draw_fog(const render::Frame& frame, const render::Letterbox& lb, bool before_stage, bool resume);
  void fx_set_hidden(bool pc_side_of_caster, bool hidden);
  FxScene fx_scene() const;
  effects::ViewParams fx_view() const;
  void fx_draw(const render::Frame& frame, const render::Letterbox& lb);
  // S6 shots / timelines of the present models (resident registry files).
  const CamBank* resident_cam_bank(const std::string& name, bool sequence) const;
  // OC_FX_OBJECTS / OC_FX_UNITS dev logs (pack or idle).
  void fx_dev_logs();
  // Screen colour layers (ops 0x2B / 0x2C).
  void fx_draw_screen(const render::Frame& frame, const render::Letterbox& lb);
  // GPU side of one pack: sprite sheets + the effect meshes' texture atlases (`sets`: one per atlas, null = skipped; untextured faces use
  // `white_`), the meshes on the GPU for the stage pipeline (`stage`, null without meshes; `parts` = mesh name -> StagePart index) and the
  // sprite type 20 objects: the pack's type-0x0B Mdl records (summon creatures ...), posed by their own motion.
  struct FxGpu {
    std::unique_ptr<effects::GpuEffects> sprites;
    std::vector<std::unique_ptr<render::Texture>> textures;
    std::vector<render::TexRef> sets;
    std::unordered_map<std::string, int> parts;
    std::unique_ptr<render::StageMesh> stage;
    struct Model {
      assets::ModelAsset asset;
      std::unique_ptr<render::MeshModel> gpu;
      UnitPose pose;
      int clip = -1;
      // effects::FxModels::PackDrive: asset clip index of streamed motion file n once decoded (-1 = not loaded, -2 = failed).
      std::array<int, 4> motion_clip{{-1, -1, -1, -1}};
      // Where its two texture pages are cut from the pack's VRAM (page x in halfwords, -1 = the textures are not the VRAM's: HD art).
      int page_x = -1, page_y = 0, clut_x = 0, clut_y = 0;
    };
    std::map<std::string, std::unique_ptr<Model>> models;
    // A model was given its battle model's cdrom2 mesh + texture (fx_hd_counterpart): rebuilt on an art switch.
    bool art_dependent = false;
    FxGpu() = default;
    FxGpu(const FxGpu&) = delete;
    FxGpu& operator=(const FxGpu&) = delete;
  };
  FxGpu* fx_gpu_for(const std::shared_ptr<const FxPack>& pack);
  // battle.prd's meshes (once per scene).
  FxGpu* fx_gpu_resident(const std::shared_ptr<const FxPack>& pack);
  // Sprite type 20: the pack's type-0x0B models.
  void fx_load_models(FxGpu& gpu, const FxPack& pack);
  // The two pages of a pack model cut from `vram` (fx_load_models; again after an overlay's VRAM copy into them: fx_vram_apply).
  static std::vector<import::Image> fx_model_pages(const import::Vram& vram, int page_x, int page_y, int clut_x, int clut_y);
  // HD art: when the pack Mdl `mdl` is a byte copy of a battle model's disc mesh (S1) + skeleton (S2) and its texture page (8 bpp at halfword
  // (img_x, img_y), CLUT row (clut_x, clut_y)) holds that model's disc .ltd / .ltc main image + CLUT row 0, and the model has a cdrom2 (HD)
  // variant: `out_mdl` = mdl with the cdrom2 S1, `out_vram` = vram with the cdrom2 image + CLUT written at the same place. Returns the stem.
  std::string fx_hd_counterpart(ByteSpan mdl, const import::Vram& vram, int img_x, int img_y, int clut_x, int clut_y, Bytes& out_mdl,
                                import::Vram& out_vram);
  // FNV-1a of the disc S1 -> battle model stems with a cdrom2 .obj.
  std::unordered_map<u64, std::vector<std::string>> fx_hd_mesh_index_;
  bool fx_hd_mesh_indexed_ = false;
  void fx_draw_models(const render::Frame& frame, const render::Letterbox& lb, FxGpu& gpu,
                      const std::vector<effects::EmitterSystem::MeshInstance>& inst);
  // A PC's weapon mesh is registered under the two-digit character number ("02" = Guile, "19" = Glenn, "37" = Sneff, "39" = Doc: the names
  // the weapon techs' mesh sprites use). The present PC whose equipped weapon mesh `name` denotes, or -1.
  int fx_weapon_mesh_unit(std::string_view name) const;
  // Mesh sprites (type 14) that resolved to a PC's weapon: drawn rigidly at the object's placement, like the pack models.
  void fx_draw_weapon_meshes(const render::Frame& frame, const render::Letterbox& lb,
                             const std::vector<std::pair<int, const effects::EmitterSystem::MeshInstance*>>& inst);
  // At the pack's registration a type-0x19 entry 'tech' / 'tec1'..'tec5' becomes the external clip set of the caster with that acting-list
  // index, any other name that of every caster and target. Dropped at the pack end.
  void fx_hang_clip_packs(const FxPack& pack);
  // One pack clip entry on one unit.
  bool fx_hang_clip(int eid, const std::string& name, const Bytes& data, int unit);
  // An overlay (kamei / lasbos ops 0x101 / 0x102) replaces the battle stage: -1 frees it, 0..99 bg_NN, 100 battle/bg/kamei, kStageSlot + e slot e.
  void fx_swap_stage(int stage);
  void fx_drop_clip_packs();
  // Effect mesh objects drawn in the last frame (trace).
  size_t fx_mesh_instances_ = 0;
  // unload_scene.
  void fx_reset_scene();

  // ui (battle_view_ui.cpp)
  enum class Tgt { Enemy, EnemyAll, Ally, AllyAll, Self };
  Tgt target_mode(int cell) const;
  void enter_target(Tgt mode);
  std::vector<int> target_candidates() const;
  void build_command_ui();
  void build_element_ui();
  // Dopplegang (element 238): the creature pick list and the form change. open_dopple: element 238 confirmed on the active PC's grid builds
  // the list and opens the window; apply_form: Battle::transform + the unit's model / scripts / idle (form < 0: back to the character);
  // apply_pip_form: Pip's form change (the unit's kind byte is new: rebind its model).
  void open_dopple();
  void build_dopple_ui();
  void apply_form(int unit, int form);
  void apply_pip_form(int unit);
  // Camera case 8 on the pack's act0 model (slot A).
  bool pa_ref_point(int attach, std::array<float, 3>& point, float& yaw) const;
  // battle_view_trail.cpp: the blink state of a unit whose model changed.
  void trail_eye_reset(int unit);
  // The hensin overlay's form phases.
  void fx_form_phase(int phase);
  void build_result_ui();
  void auto_pick();
  void auto_chain();

  // hud
  void build_hud();
  void build_camera_overlay();
  // Plain HUD text (dev overlay, the standalone result / game-over stub): RDE battle font (slot 2) at 0.42 x scale_mul, canvas units.
  void add_text(const std::string& text, float x, float y, u32 color, int align = 0, float scale_mul = 1.0f);
  void add_rect(float x, float y, float w, float h, u32 color);
  void add_panel(float x, float y, float w, float h);
  // The PS1 battle window: black fill, 1 px white line, chamfered top-right / bottom-left.
  void add_box(float x, float y, float w, float h, bool fill = true);
  void add_bar(float x, float y, float w, float h, float frac, u32 fill);
  void add_plate(int colour_index, float x, float y, float w, float h, u32 tint = 0xFFFFFFFFu);
  struct Tex;
  void add_img(const Tex& tex, float x, float y, float w, float h, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1, u32 tint = 0xFFFFFFFFu, int blend = 0);
  // RDE battle text list entry at PS1 draw coordinates (x, y), sx = 4, sy = 960 / 216: `flags` = style | 0x10000 (JP font) | 0x20000 (non-JP
  // font); `colour` = PS1 colour word 0x00BBGGRR. Picks the font slot, multiplier, shrink rule and colour helper of the style. Both art
  // modes: only the font files differ (ui::font::FontSet small_ / large_).
  void btext(int flags, const std::u32string& text, float x, float y, u32 colour);
  // Canvas units.
  void add_text_op(int slot, std::u32string text, float x, float y, const ui::font::Ink& ink, float scale, int align);
  // cmnmes XLAT of a JP key in the current language.
  std::u32string xlat(const char* jp, const char32_t* fallback);
  // HIT% menu cursor after each hit (option "Battle attack select: Auto").
  void chain_auto_cursor();
  size_t op_count() const;
  // WideHudEdges: ops_[from..] -> edge group (-1 left, 1 right, 2 pinned) + extra canvas shift.
  void set_group(size_t from, int grp, float shift = 0.0f);
  // The battle window skin (battle_04.tim 'win1'): outer rect in canvas units.
  void add_skin_box(float x, float y, float w, float h);
  // Target cursor, info window and the camera-mode hint (Ui::Target; the Dopplegang list: no hint).
  void build_target_ui(bool cam_hint = true);
  void build_message_window(const std::vector<std::pair<std::u32string, u32>>& lines);
  void load_hud_art(const vfs::Vfs& vfs);
  void rebuild_ring();
  // The field ring of the presented action (the rules run ahead of the replay) and the cell shift that walks the drawn ring toward it.
  std::array<int, 3> presented_ring() const;
  void step_ring();
  // The drawn ring has reached the presented ring: the next action group waits for the shift.
  bool ring_settled() const;
  void build_party_panels();
  // The status plate column of one unit (status_plates.h): the right column for the active PC, the left one for the target cursor unit.
  void build_status_plates(int unit_index, bool right);
  void build_field_ring();
  // The element-trap block under the field ring, for the presented action (like the ring).
  battle::TrapHud presented_trap() const;
  void build_element_trap();
  void build_colour_row();
  void replay(const render::Frame& frame, const render::Letterbox& lb, float alpha);
  // draw_ui is replaying into the PS1-resolution target (replay derives the picture rectangle from `lb`).
  bool ui_pass_ = false;
  void nine_slice(render::TexRef set, float tw, float th, float x, float y, float w, float h, float corner_src, float corner_dst, u32 color);

  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::MeshPipeline* meshes_ = nullptr;
  const data::GameTables* tables_ = nullptr;
  audio::Mixer* mixer_ = nullptr;
  bool hd_ = true;
  // set_texture_filter: -1 = by art mode.
  int linear_filter_ = -1;
  bool ready_ = false;
  // auto_: command picks; auto_confirm_: pages / windows confirm themselves.
  bool auto_ = false, auto_confirm_ = false;
  bool cursor_memory_ = false, auto_accuracy_ = true;
  // Per-PC last command-menu row (kept with the option) and last confirmed target (3 at battle start): the next ring opens on it.
  int top_mem_[battle::kPcSlots] = {};
  int tgt_mem_[battle::kPcSlots] = {3, 3, 3};
  // Element list slide: state 0 idle, 1 plates sliding in, 3 sliding out, 4 out (the next level's plates load); idx = the index into the
  // x / frame tables, dir = 1 after Right, shown = the level whose plates are on screen (-1 = menu closed), age = ticks in the state.
  int elem_state_ = 0, elem_idx_ = 0, elem_dir_ = 1, elem_shown_ = -1, elem_age_ = 0, elem_x_ = 8;
  // The chosen plate floating to (8, 22) while the target is picked (cast_header.h); hdr_on_ = the command is an Element in target
  // selection / waiting for its action start; hdr_row_ = level of the chosen cell (the %+d label).
  bseq_hdr::FloatHeader hdr_;
  bool hdr_on_ = false;
  int hdr_row_ = 0;
  int hdr_cell_ = -1;
  // One tick of the slide machine; false = the menu takes no input this tick.
  bool elem_slide_step(int level);
  int debug_ui_ = 0;

  render::StagePipeline stage_pipe_;
  // The stage's object scripts + mesh instances (stage_scene.h).
  StageScene stage_scene_;
  Cam cam_;
  float clear_[3] = {0.05f, 0.05f, 0.08f};
  BattleCamera camera_;
  // Stage + battle.prd cameras / sequences.
  CamBank bank_;
  BattleAudio audio_;
  bool boss_scene_ = false, field_battle_ = true;
  // PCs not yet placed while a held intro (timeline op 0x73) runs.
  std::array<bool, battle::kUnits> unplaced_{};
  // Battle id word > 0x1FF: the stage's 'boss' timeline instead of a random bti*.
  bool direct_id_ = true;
  // StartOptions::flags of the running battle.
  int start_flags_ = 0;
  struct LoadJob {
    int scene = -1;
    std::vector<battle::PartyMember> party;
    StartOptions opt;
    int step = 0;
    bool done = false, failed = false, has_prd = false, block = false, wait = false, again = false, stage_begun = false;
    // battle/bg/bg_NN.
    std::string base;
    Bytes prd, ctd;
    double ms = 0;
    std::chrono::steady_clock::time_point t0;
  };
  std::unique_ptr<LoadJob> load_;
  void run_load_step(LoadJob& job);
  bool finish_start();

  // rules
  std::unique_ptr<battle::GameRng> rng_;
  // The Rand op of the spawn scripts (their opening wait): own stream, seeded with the battle seed.
  mutable std::minstd_rand spawn_rng_;
  // The scene's .pac scripts drive the enemies (loader set in init).
  battle::pac::ScriptedBrain brain_;
  std::unique_ptr<battle::Battle> battle_;
  ModelCache model_cache_;
  int scene_id_ = 0;
  std::array<Visual, battle::kUnits> vis_{};
  std::vector<Pop> pops_;
  std::deque<LogLine> log_;

  // action playback
  std::deque<std::vector<battle::ActionEvent>> queue_;
  Act act_;
  std::vector<Pending> pending_;
  // The attack-hand toggle starts at 1 with the battle overlay: the first attack plays hand 0 ("_s00").
  int hand_toggle_ = 1;
  int intro_ticks_ = 0;
  // Ticks the intro timelines / stage emitters run while the battle loads behind the field transition (the `bti*` pan is cut ~88 ticks in).
  static constexpr int kBtlinitLoadTicks = 88;
  // The party intro timeline waiting for the load to end (pli_after_load_, a field battle) or for the bti* intro to end (scripted / held).
  std::string pli_pending_;
  bool pli_after_load_ = false;
  // The stage intro timeline picked at the start, until start_stage_intro runs it.
  std::string intro_pending_;
  bool intro_started_ = false;
  void start_stage_intro(bool preroll);
  enum class Finish { None, Victory, Wiped, Fled } finish_ = Finish::None;
  // A wipe: 14 + 14 idle frames, then the fade to black over 0x3C ticks.
  static constexpr int kWipeIdle = 28, kWipeFade = 0x3C;
  // Running away: `bto0` starts ~62 ticks after the Run confirm (the party runs, then the 14 + 14 teardown frames); kFledFade = the fade of a stage
  // without `bto0`.
  static constexpr int kRunTicks = 34, kOutroDelay = 28, kFledFade = 60, kOutroCap = 600;
  // Running away with pilfered loot: the escape waits kLootDropWait more ticks; the system window (btmes row 5) opens kLootDropMsg ticks into it.
  static constexpr int kLootDropWait = 80, kLootDropMsg = 40;
  bool fled_outro_ = false;
  int finish_ticks_ = 0;
  // Victory music, in ticks: kVictoryThemeFade = from the win to the start of the battle theme's fade to silence; kVictoryThemeFadeTicks =
  // the length of that fade; kVictorySongDelay = from the start of the fade to the victory song.
  static constexpr int kVictoryThemeFade = 43, kVictoryThemeFadeTicks = 16, kVictorySongDelay = 20;
  // The victory song still to start (-1 = none).
  int victory_song_ = -1;

  // ui
  Ui ui_ = Ui::Intro;
  int active_pc_ = -1;
  // SFX 0x16 owed when the command menu opens again after nobody could act.
  bool menu_chime_ = true;
  int top_cursor_ = 0, chain_cursor_ = 0, cell_ = 0, target_cursor_ = 0;
  // The Dopplegang pick list: -1 = the character's own form (entry 0), then the pool slots ascending. dop_cur_ = cursor entry (2 per row),
  // dop_top_ = first visible row (6 rows show), dop_max_ = the first-row clamp 1 + ceil((n8 - 11) / 2) with the BYTE creature count n8 (>= 0),
  // dop_named_ = inline names the text renderer has finished (the scroll gate), dop_typing_ / dop_left_ = the renderer's position: the entry
  // being uploaded and its glyphs still to go, dop_len_ = glyphs per entry (the names as drawn).
  std::vector<int> dop_list_;
  int dop_cur_ = 0, dop_top_ = 0;
  int dop_max_ = 0;
  int dop_named_ = 0;
  int dop_typing_ = 0, dop_left_ = 0;
  std::vector<int> dop_len_;
  // Entry k's name (the caster's, or sysmes 0x19 of the slot).
  std::u32string dop_name(int index);
  // One frame of the text renderer over the list text.
  void dop_type_step();
  // The picked Dopplegang form (-2 none, -1 own), applied by the hensin overlay's swap phase or at the action's end; pending_evolve_unit_ =
  // Pip's form change waiting for the tumaru overlay's swap op (or the action's end).
  int pending_form_ = -2, pending_form_unit_ = -1;
  int pending_evolve_unit_ = -1;
  // active_pc_ stands at engaged_target_ and picks hits from the percentage menu; engaged_after_cast_ = an element was cast from the
  // engaged spot: return afterwards; engage_arrive_ = the approach act is running.
  bool engaged_ = false;
  int engaged_target_ = -1;
  // An enemy action ran while the PC stood at its target: its next attack selects the approach shot again.
  bool engage_interrupted_ = false;
  bool engaged_after_cast_ = false;
  bool engage_arrive_ = false;
  std::vector<int> chain_;
  battle::Command pending_cmd_;
  std::vector<int> targets_;
  Tgt tgt_mode_ = Tgt::Enemy;
  bool finished_ = false;
  std::vector<std::string> route_events_;
  bool paused_ = false;
  // The OC_ROUTE_ELEM / OC_DEV_VAR dev hooks ran.
  bool route_elem_done_ = false;
  bool dev_var_done_ = false;
  // The host shows BattleResults after a victory (set_external_results).
  bool external_results_ = false;
  // Actor and kind of the last command the player confirmed (which PC the menu reopens on).
  int last_actor_ = -1;
  battle::Command::Kind last_kind_ = battle::Command::Kind::Attack;
  // sysmes.txt (element descriptions), bound to the vfs in init().
  import::LangTables lang_tables_;
  import::Lang lang_ = import::Lang::En;
  // Occupied cells of one grid level (row) of the acting PC, slot order.
  std::vector<int> level_slots(int level) const;
  // Menu-time facing, the L1 / R1 PC switch, the DYNAMIC-camera translucency (battle_view_action.cpp).
  void sync_menu_facing();
  // Top command menu: +1 next / -1 previous PC that can act (plays SFX 0x16); false = nobody else.
  bool switch_active_pc(int dir);
  // Every unit but the cursor unit while the DYNAMIC target camera is on.
  bool dynamic_translucent(int unit) const;
  Ui menu_face_ui_ = Ui::Intro;
  int menu_face_pc_ = -2, tgt_face_unit_ = -2;
  // Target selection camera mode: false NORMAL (zen0), true DYNAMIC.
  bool cam_dynamic_ = false;
  // Menu camera state: the PC orbited last tick (-1 = none, the next orbit pose is a cut), the target the DYNAMIC glide aims at (-1 = none),
  // the NORMAL glide to zen0 started for this target menu.
  int orbit_pc_ = -1, dyn_target_ = -1;
  bool menu_glide_ = false;
  // A battle-script cutscene (op 2C) takes the orbit's PC away until a command menu opens again; the begin event takes effect after the
  // next menu camera pass (the camera of the tick the action ended runs before the script resumes).
  bool orbit_blocked_ = false, orbit_block_next_ = false;
  float time_ = 0;
  int auto_delay_ = 0;
  // BattleAuto rotation state.
  int auto_pc_ = -1, auto_target_ = -1, auto_step_ = 0;
  int tick_count_ = 0;
  // Video frames (60 Hz) since the view started, counting the frames fx_slow_skip() swallows (tick_count_ = game ticks, which a pack's slow
  // motion, ops 0x56 / 0x79, makes fewer): the time base of the reference captures (run frames) and of the OC_FX_* dev logs. dev_logged_frame_
  // = the last frame those logs were written for (fx_draw can run more than once per frame).
  int frame_count_ = 0, dev_logged_frame_ = -1;
  float result_age_ = 0;

  // timing
  platform::FixedStep stepper_;
  // The DRAW interpolation between the last two game ticks (fx_slow_alpha: spread over a slowed pack's gap); tick_alpha_ = the stepper's own
  // remainder, alpha_ during the ticks, so game logic that reads alpha_ is unchanged by the slow spread.
  float alpha_ = 0;
  float tick_alpha_ = 0;

  // text + panel art
  render::TextBatch text_;
  // RDE's 14 font slots; classic (original art) = the small_font_* files.
  ui::font::FontSet fonts_;
  // Keyboard glyphs after a key press, pad glyphs after pad input.
  bool kb_mode_ = false;
  struct Tex {
    render::Texture tex;
    render::TexRef set;
    float w = 1, h = 1;
  };
  Tex white_, win_base_, win_frame_;
  // The captured field frame of the transition.
  Tex trans_;
  std::vector<u8> trans_px_;
  u32 trans_w_ = 0, trans_h_ = 0;
  bool trans_wait_ = false, trans_run_ = false;
  float trans_t_ = 0;
  // Battle start after the warp: white fade-in (the miya "fade" emitter), -1 = none.
  int white_ticks_ = -1;
  void free_transition();
  // HUD art: waku panel base / frame / active frame, gauge cells gg-1 / gg-2, field frame, damage digits, cursor frames, the field ring,
  // the HIT% caption leader (osak), pad / key glyph atlases, the camera icon, the element list plate (win4), the target info window
  // (waku-03), the red cursor diamonds (battle_11), the status icons (battle_34), the window skin halves (battle_04 STP / opaque texels) and
  // the script window's page / end cursor (battle_86, 8 x 16 PS1 px cells) and the seven Chrono Cross colour row dots (16 x 16).
  enum HudTex { kHWakuBase, kHWaku, kHWakuAct, kHGg1, kHGg2, kHField, kHDigits, kHCursor, kHRing, kHOsak, kHPad, kHCam, kHWin4Base, kHWin4Frame,
                kHWaku03, kHKeys, kHDiamond, kHStatus, kHSkinSemi, kHSkinOpaque, kHPageCur, kHColourDot, kHCount = kHColourDot + 7 };
  std::array<Tex, kHCount> hud_;
  // 12 cell masks, 208x112 each (elementfield/a01..a12.bmp).
  std::vector<u8> ring_masks_;
  // The drawn ring: 12 cells, 4 per slot (oldest slot first); ring_feed_ holds the cells still to shift in at the newest end, ring_tick_ the
  // tick of the last shift.
  std::array<int, 12> ring_cells_{};
  std::array<int, 12> ring_feed_{};
  int ring_feed_count_ = 0, ring_tick_ = 0;
  bool ring_cells_set_ = false;
  // An action presented since the last shift repainted the whole ring: the next shift feeds all 12 cells.
  bool ring_repaint_seen_ = false;
  // The Chrono Cross colour row as last shown (colour + 1 per dot, 0 = empty).
  std::array<u8, 7> colour_row_{};
  std::array<int, 12> ring_key_{-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2};
  // The all-colours cell's picture, 208x112 RGB (elementfield/battle_88.bmp); empty when missing.
  std::vector<u8> ring_rainbow_;
  // The element-trap block's colour marks (5 x 5 PS1 px, cut from the disc's battle sprite sheet 'etra'), by colour index green .. blue.
  std::array<Tex, 6> trap_marks_;
  // Damage digit ink ranges in battle_33.tim (HD px) + the "miss!" word as entry 10.
  std::vector<std::pair<int, int>> digit_x_;
  // battle_08_93: the item-like (record type 5) cast-header plate (120 x 16).
  Tex eplate_item_;
  // Element-list name plates battle_08_<T>_<n> / battle_76_<T>_<n> (120 x 16): [used * 70 + colour * 10 + frame - 1], frames 1..3 steady, 4..10 the slide.
  std::array<Tex, 140> eplate_;
  // menu/menuelem/menuelem_1..8 (empty, blue, green, black, red, white, yellow, rainbow).
  std::array<Tex, 8> plates_;
  render::Sampler linear_, nearest_;

  // element effects
  Debug dbg_;
  BattleFxLibrary fxlib_;
  bool fxlib_ready_ = false;
  // The effect library's first init (fnd tables, particle.prd, battle.prd resident packs: ~16 ms) on a worker during the swirl.
  std::future<bool> fxlib_job_;
  std::unique_ptr<BattleFxRun> fx_;
  std::shared_ptr<const FxPack> fx_pack_;
  // The pack's pseudo-actor: the type-0x14 `act0` model of the running pack (ghost1's Dragon Rider dragon = the pack's type-0x0B `act0` Mdl;
  // oomono2 / jigoku's creature = model N of file group 0xE) with its own anmseq VM (the S5 entry 0 / the pack's type-0x1A `act0` table), pose,
  // mover and draw. Hidden until its script shows it; bound to the pack's lifetime (fx_end). `registered` = slot A holds the pack's act0 Mdl
  // but no overlay of ours runs its script (camera case 8 only); `own_table` = the model's S5 block (ghost1; the group-0xE class runs
  // FxPack::tables["act0"]); `handle` / `own_pose` = a group 0xE model and its pose; `fm` = the pack's own model (ghost1); `rx` / `rz` = the
  // SetRot x / z rotations (radians), ramped by `xz` (RotateT: 12.12 fixed angles stepped by (diff << 12) / n per tick); `abr` = -1 off, else
  // the ABR mode; `shadow` = the anmseq Shadow op set the drop shadow bit; `height` = model extent (computed on first use): the shadow's grey
  // fades with it.
  struct PseudoActor {
    bool on = false, hidden = true;
    bool registered = false;
    bseq::Vm vm;
    bseq::Table own_table;
    std::shared_ptr<ModelHandle> handle;
    UnitPose own_pose;
    FxGpu::Model* fm = nullptr;
    UnitPose* pose = nullptr;
    const assets::ModelAsset* asset = nullptr;
    const render::MeshModel* gpu = nullptr;
    float x = 0, z = 0, y = 0, px = 0, pz = 0, py = 0;
    float face_x = 0, face_z = -1, pface_x = 0, pface_z = -1;
    float rx = 0, rz = 0;
    struct XzRamp {
      int left = 0;
      int cur[2] = {0, 0}, step[2] = {0, 0};
    } xz;
    Visual::Move move;
    Visual::Turn turn;
    float scale[3] = {1, 1, 1}, scale_to[3] = {1, 1, 1}, scale_step[3] = {0, 0, 0};
    int scale_left = 0;
    int rgb[3] = {128, 128, 128};
    int abr = -1;
    struct Fade {
      bool on = false, plain = false;
      int left = 0, cur[3]{}, step[3]{}, to[3]{};
    } fade;
    bool shadow = false;
    float height = -1;
    int ticks = 0;
  } pa_;
  void pa_begin(const std::shared_ptr<const FxPack>& pack);
  void fx_queue_hits();
  void pa_tick();
  void pa_event(const bseq::Event& event);
  void pa_draw(const render::Frame& frame, const render::Letterbox& lb);
  // Model -> world, as pa_draw.
  render::Mat4 pa_matrix(float alpha) const;
  // FxModels::pseudo_attach (caster[-1] = the act0 model).
  bool pa_attach(int point, effects::Vec3& out);
  FxFrame fx_frame_;
  SpellLights spell_;
  std::map<int, std::unique_ptr<FxGpu>> fx_gpu_;
  // The resident library sheets, uploaded once per scene and shared by every pack's GpuEffects.
  std::shared_ptr<effects::GpuEffects> fx_lib_gpu_;
  const void* fx_lib_key_ = nullptr;
  render::ParticleBatch particles_;
  // The previous bank stays alive: the camera may still point into it.
  std::unique_ptr<CamBank> fx_cam_, fx_cam_old_;
  const CamBank* fx_prev_bank_ = nullptr;
  std::vector<bool> fx_fired_;
  // Units whose anmseq a pack sequence op (0x43 / 0x45 / 0x4F ...) armed during the pack start (fx_->start); the stock hosts' init tail comes
  // BEFORE those ops in retail and its arm is simply overwritten, so it must not re-arm them (fx_begin). fx_starting_ = inside fx_->start.
  std::vector<int> fx_armed_at_start_;
  bool fx_starting_ = false;
  std::array<bool, battle::kUnits> fx_hidden_{};
  // Presentation-side ghost word: the pac VM clears its ghost flag the moment the door script runs (synchronously, before the cast is
  // replayed), but retail's clears when the action ends. `ghost_view_` is set with the VM's flag and cleared only once no action is being
  // presented (fx_tick), so the unit stays undrawn until the pack's own ghost draws it.
  std::array<bool, battle::kUnits> ghost_view_{};
  // FxModel::conceal: a pack hid the model, drawn again only when the action is over.
  std::array<bool, battle::kUnits> fx_conceal_{};
  // battle_view_script.cpp: a set still waiting behind a Barrier in the script stream has not happened yet.
  bool ghosted(int unit) const;
  // A script-ghosted unit (brain_.hidden) a pack's entry has revealed: drawn until the script's ghost op clears it.
  std::array<bool, battle::kUnits> entry_shown_{};
  int fx_casts_ = 0, fx_shake_x_ = 0, fx_shake_y_ = 0;
  bool fx_faded_ = false;
  std::string fx_sound_key_;
  // The casting monster's own camera timeline (S6 'mag<k>') may start: only with a charge instance (colour action).
  bool model_cam_ = false;
  // Ticks since the charge's op 0x41; the pack shows kHandOverDelay ticks later (the hand-over runs at the top of the next 20 Hz pass).
  int fx_hand_wait_ = 0;
  static constexpr int kHandOverDelay = 3;
  // A cast is in its charge; the charge's op 0x41 was seen.
  bool fx_charge_ = false, fx_ready_ = false;
  // Ticks from the command confirm to the charge start.
  static constexpr int kCastLead = 12;
  // GTE depth cue state (battle effects op 0x2D ramps it, a pack start resets it): far colour in vertex-colour units (0x80 = neutral) and the
  // near / far range in world units. fog_raw_ = the stored values (the ramp space); fog_cur_ = what the GTE holds: SetFogNearFar leaves the
  // depth-queue factors untouched unless far - near >= 100, so a ramp to equal near / far freezes the last valid pair and fogs everything.
  struct Fog {
    float r = 128, g = 128, b = 128, near = 16384, far = 20480;
  } fog_cur_, fog_from_, fog_to_, fog_raw_;
  int fog_left_ = 0, fog_total_ = 0;
  // A sprite pass of this frame already ran (the HUD pass resumes it).
  bool fog_pass_ = false;
  // Depth-cue switch: op 0x24 on (and every actor gets the depth-cued renderer), op 0x6C off; off by default (battle start / effect start /
  // reset). Ops 0x49 / 0x4A / 0x4B give all / targets / casters back the plain renderer. Fog ramps (0x2D) show only while it is on.
  bool smooth_rings_ = false;
  bool fx_fog_on_ = false;
  // Ticks until the stage lens flare returns after a pack's end hook.
  int fx_lens_wait_ = 0;
  std::array<bool, battle::kUnits> fx_unit_fog_{};
  // Screen colour layers (sequence ops 0x2B / 0x2C): a full-screen quad (0,0)-(0x160,0x110) in effect-OT slot 5 (layer 0) / 6 (layer 1) while
  // its colour is not black; mode 0 opaque, 1..4 = ABR mode - 1.
  struct ScreenColour {
    float cur[3]{}, from[3]{}, to[3]{};
    int mode = 0, elapsed = 0, ticks = 0;
    bool ramping = false;
  };
  std::array<ScreenColour, 2> fx_screen_{};
  // Set at the battle main loop start; the first command-camera pass with no shot / timeline fades both screen colour layers to 0 over 10
  // ticks and clears it - what ends an intro's held colour (bg_41 `pli0` (255, 226, 156) additive).
  bool fx_screen_menu_clear_ = false;
  // Anmseq op 0x52: one more full-screen quad per op, colour from -> to over `ticks`, drawn with the effects under the HUD; mode 0 opaque,
  // else ABR mode - 1. Several can run at once.
  std::vector<ScreenColour> anm_screen_;
  void fx_screen_colour(int layer, const assets::SeqCmd& cmd);
  void fx_screen_tick();
  static void fx_screen_tick_layer(ScreenColour& screen);
  // Hide bits 0x1 / 0x100 set by the visibility ops; fx_hidden_ = any set.
  std::array<u16, battle::kUnits> fx_hide_bits_{};
  void fx_hide(const std::vector<int>& units, u16 bit, bool on);
  // 0 everyone, 1 the action's targets, 2 its casters.
  std::vector<int> fx_units(int which) const;
  // Per-unit model state set by stock sequence ops: colour (op 0x58; 0x80 = neutral), the shadow bit (ops 0x74 / 0x75), sideways facing
  // (op 0x76: yaw -0x400 / 0x400 by the side's x sign).
  struct FxUnitState {
    float rgb[3]{128, 128, 128}, from[3]{128, 128, 128}, to[3]{128, 128, 128};
    int elapsed = 0, ticks = 0;
    bool ramping = false, tinted = false;
  };
  std::array<FxUnitState, battle::kUnits> fx_unit_{};
  // The unit drop shadow bit is MODEL state that lives for the whole battle: set at model init, anmseq op 0x22 / 0x21 (S5 entry 8 of ki /
  // q_con2 / sakana / fate / tokikui: the unit's appearance) and pack ops 0x74 / 0x75 clear / set it; the effect start hook saves it per unit
  // and the end hook writes the saved bit back. shadow_off_ = the bit is clear; the saved copy starts clear too.
  std::array<bool, battle::kUnits> shadow_off_{}, shadow_saved_off_{};
  // The effect start hook: shadow_saved_off_ := shadow_off_; the end hook: shadow_off_ := shadow_saved_off_.
  void fx_hook_start();
  void fx_hook_end();
  // PS1 lit model path. The lit bit: sequence ops 0x61 / 0x62 (targets), 0x63 / 0x64 (casters), 0x68 / 0x69 (everyone) - not hide bits: the
  // model draw skips a model on the hidden bit only and takes the lit packer on the lit / semi bits. The light slots (written by the packs'
  // type-17 light objects, intensities zeroed by op 0x66) and the back colour (op 0x65 ramp, a pack's type-0x24 entry) are globals on the
  // PS1 and persist across packs within the battle.
  std::array<bool, battle::kUnits> fx_lit_{};
  // The anmseq light ramp walks the acting and target lists, not every unit.
  void fx_lit_lists(bool on) {
    for (const std::vector<int>* list : {&anchor_actors_, &anchor_targets_})
      for (const int unit : *list)
        if (unit >= 0 && unit < battle::kUnits) fx_lit_[static_cast<size_t>(unit)] = on;
  }
  effects::LightTable fx_lights_;
  struct FxAmbient {
    int cur[3]{0, 0, 0}, from[3]{0, 0, 0}, to[3]{0, 0, 0};
    int elapsed = 0, ticks = 0;
    bool ramping = false;
    // [Enhancements] Lighting: the same ramp as a stage multiplier (/ 128). Neutral (128) until a pack's op 0x65 first runs: the PS1 back
    // colour starts at 0 = "no pack lighting", which must not black the stage out, so the first ramp starts from 128 instead of 0. Only op
    // 0x65 moves it (not the anmseq 0x5E flash). Packs that end on a dark back colour (Fire Pillar, Magma Burst) never ramp it back: once
    // no effect run is live the multiplier eases to neutral, and every battle starts neutral.
    bool touched = false;
    float scur[3]{128, 128, 128}, sfrom[3]{128, 128, 128}, sto[3]{128, 128, 128};
  } fx_ambient_;
  void fx_ambient_set(int r, int g, int b, int ticks);
  void fx_ambient_tick();
  void fx_ambient_release();
  void fx_ambient_stage_reset();
  void fx_spell_run_begin();
  // [Enhancements] SpellLights (battle_view_fx_lights.cpp).
  void fx_spell_light_tick();
  void fx_light_params(int unit, const render::Mat4& model, render::MeshPipeline::DrawParams& dp) const;
  // Slow motion (sequence ops 0x56 / 0x79): one game tick per N video frames. 0x79 (and 0x56 with s16 @+10 != 0) sets N = byte @+8 (< 2 =
  // normal); 0x56 with s16 @+10 == 0 sets the catch-up limit X = byte @+8 (used while N < 2), taken here as N = X. Both clear when the next
  // pack starts.
  int fx_slow_n_ = 0, fx_slow_x_ = 0, fx_slow_phase_ = 0;
  // Screen copy warp (sequence ops 0x3E on / 0x3F off / 0x40 ramp; drawn after the scene and the effects): the frame drawn so far is drawn
  // again over itself in 80-pixel strips, each vertex (x - 160, y - 112) * 4 rotated by `ang`, moved by `tr` (+ 0x1000 in z), projected with
  // H 0x400 about (160, 112) + `off`, colour-modulated by `rgb` (0x80 = neutral) and blended semi-transparently with ABR `abr`. Identity = the
  // frame unchanged; the packs zoom (tr z < 0), turn and tint it.
  struct Warp {
    bool on = false;
    float rgb[3]{128, 128, 128}, ang[3]{}, tr[3]{}, off[2]{};
    int abr = 0;
    struct Ramp {
      float from[11]{}, to[11]{};
      int elapsed = 0, ticks = 0;
      bool on = false;
    } ramp;
  } fx_warp_;
  void fx_warp_op(const assets::SeqCmd& cmd);
  void fx_warp_tick();
  // The scene goes into fx_warp_rt_ (false = warp off); fx_warp_end: the copy + the warped strips onto `frame`.
  bool fx_warp_begin(const render::Frame& frame);
  bool fx_warp_ensure(u32 w, u32 h);
  void fx_warp_end(const render::Frame& frame, const render::Letterbox& lb);
  render::RenderTarget fx_warp_rt_;
  render::TexRef fx_warp_set_;
  render::Frame fx_warp_frame_{};
  // An overlay's camera cross-fade (effects::FxModels::CrossView): `cam` is the second camera task; while `on` the scene is drawn twice, the
  // second time from `cam` into fx_cross_rt_, and composited (fx_cross_draw). `ticks` = ticks since its shot was cut to key 0.
  struct Cross {
    BattleCamera cam;
    std::string shot;
    int serial = 0, ticks = 0, alpha = 0, fill = 0;
    bool on = false;
  } fx_cross_;
  void fx_cross_tick();
  bool fx_cross_draw(const render::Frame& frame, const render::Letterbox& lb, bool sprites_resume);
  // The full-screen tint of an overlay (effects::FxModels::Tint), over the finished picture.
  void fx_tint_draw(const render::Frame& frame, const render::Letterbox& lb);
  render::RenderTarget fx_cross_rt_;
  render::TexRef fx_cross_set_;
  render::Frame fx_cross_frame_{};
  // The camera apply_camera() reads (null = camera_): the cross-fade's second pass.
  const BattleCamera* draw_camera_ = nullptr;
  // An overlay's frame copy (FxModels::capture): the picture as drawn up to the request's ordering-table slot, copied every frame while the
  // request stands; the mesh faces on the copy's page sample it (fx_draw). Made at most kCopyPixelsPerPs1 pixels per PS1 pixel.
  static constexpr float kCopyPixelsPerPs1 = 8.0f;
  bool fx_copy_ensure(const render::Frame& frame, const render::Letterbox& lb);
  render::RenderTarget fx_copy_rt_;
  render::TexRef fx_copy_set_;
  // The strips sample the PREVIOUS output (RDE's 4x renderer reads the last presented frame, not this frame's pixels): out(n) = scene(n)
  // blended with the warped out(n - 1). Two persistent targets, one step per game tick.
  render::RenderTarget fx_acc_rt_[2];
  render::TexRef fx_acc_set_[2] = {};
  int fx_acc_cur_ = 0, fx_acc_tick_ = -1;
  bool fx_acc_have_ = false;
  bool fx_acc_ensure(const render::Frame& frame);
  bool fx_acc_ensure_one(int i, u32 w, u32 h);
  // The field's last output (host-owned); the previous output of the hand-over ticks. hand_tick_ = ticks since the battle became visible, -1
  // = no hand-over blend; kHandTicks = length of the field -> battle blend (fx_warp_end).
  render::TexRef hand_carry_;
  static constexpr int kHandTicks = 40;
  int hand_tick_ = -1;
  bool hand_active() const { return hand_tick_ >= 0 && hand_carry_; }
  // The RDE hand-over is the PS1 strip pass, once per 15 Hz update: out = scene, then the previous output over it as one strip rectangle
  // scaled by the bracket's zoom (clock rde_m0_ + tick) about the GTE centre (160, 120), blended at kHandKeep (the base's decay per tick);
  // outside the shrunk copy only the live scene shows. Ends at the `fade` cut (battle tick kStartFadeEnd). fx_acc_adv_ = the last hand-over
  // tick finished an update (the next tick writes the other target).
  bool rde_hand_ = false;
  bool fx_acc_adv_ = true;
  int rde_m0_ = 0;
  static constexpr float kHandKeep = 0.971f;
  // A particle pass already ran this frame (later ones resume its vertex data).
  bool fx_particle_pass_ = false;
  void draw_scene_3d(const render::Frame& frame, const render::Letterbox& lb, bool sprites_resume);
  // True: this 60 Hz tick is not a game tick; fx_slow_alpha: draw alpha over the video frames between two game ticks of a slowed pack.
  bool fx_slow_skip();
  float fx_slow_alpha(float alpha) const;
  // Stage emitters of a name on / off (ops 0x26 / 0x27 / 0x36-0x3D / 0x5F / 0x60).
  void fx_stage_layer(std::string_view name, bool on, bool any_duration);
  // Op 0x7B: the gate ops 0x5F / 0x60 act only while set.
  bool fx_bti_gate_ = false;
  // ---- battle-script presentation requests (pac_vm.h Event; battle_view_script.cpp script_event)
  // Event message window (ops E0 / E2 / 62): the btel_<lang>.csv row (remapped battle id << 16 | message), one {bpage} chunk per page (UTF-8,
  // tags kept), revealed up to its next {wait(n)}. `waits` = {wait(n)} steps done (cumulative over the message), `wait_left` = frames left of
  // the running one; `wend` = frames left before the window closes by itself ({wend(n)}), -1 = none; `unit` = the script's unit
  // ({playwork(0)}); (x, y) = the window rect origin and `ty` the text y (PS1 px); `cells` / `lines` = 12-px cells, 16-px lines; `open` = 0
  // closed, > 0 frames since the open (grows from the centre), -k closing; `item` = {itemwork(0)}: the stolen item / element name (system
  // message window, Pilfer); `sys` = the system message window (text at (32, 144)): closes by itself, after the action. The system plate is a
  // fixed 256 x 10 rect whatever the text with no grow step: the skin box snaps on and off, no outline frames.
  struct ScriptMsg {
    std::vector<std::string> pages;
    size_t page = 0;
    int waits = 0, wait_left = -1;
    int wend = -1;
    int unit = -1;
    float x = 0, y = 0, ty = 0;
    int cells = 1, lines = 1;
    int open = 0;
    std::string item;
    bool sys = false;
    float rect_w() const { return sys ? 256.0f : static_cast<float>(cells * 12 & ~3); }
    float rect_h() const { return sys ? 10.0f : static_cast<float>((lines << 4) & 0xFFF0); }
    int grow_frames(int step) const {
      if (sys) return 1;
      const int half = std::max(static_cast<int>(rect_w()), static_cast<int>(rect_h())) / 2;
      return std::max(1, (half + step - 1) / step);
    }
  };
  // `text` = a btmes row instead of a btel one (sys); item = {itemwork(0)}.
  struct ScriptMsgReq {
    int unit = -1, msg = 0;
    bool has_pos = false;
    int x = 0, y = 0;
    std::string text, item;
    bool sys = false;
  };
  ScriptMsg smsg_;
  // Pilfer's result: shown when the action ends.
  ScriptMsgReq sys_req_;
  bool sys_pending_ = false;
  // Ops 2C .. 38 (script cut scene): the party panels, ring and command box are not drawn.
  bool hud_off_ = false;
  // Op 39 with 0: the per-frame HUD (panels, ring, command box) is not drawn until the script sets it again.
  bool hud_script_off_ = false;
  bool select_held_ = false;
  bool autohide_cast_ = false;
  bool keep_intro_layer_ = false;
  // The intro has ended and the outro has not yet brought the `bti0` layer back.
  bool intro_layer_cut_ = false;
  // Op 62: a window position for the next window only.
  bool smsg_pos_set_ = false;
  int smsg_pos_x_ = 0, smsg_pos_y_ = 0;
  // Ops 7A / 86: entry 0 of the acting list (camera refs of script-started shots), -1 = none.
  int script_actor_ = -1;
  void script_event(const battle::pac::Event& event);
  void smsg_open(const ScriptMsgReq& req);
  void smsg_tick();
  // True while a message window owns the confirm button.
  bool smsg_input(bool ok);
  // Battle-script stream. The scripts run synchronously inside the rules (ScriptedBrain), so everything they said arrives as one ordered
  // list of pac::Event with virtual-time stamps; the view replays it as the retail VM would have run it live: events are released in order,
  // paced by the 2D delays (vt gaps), E0 waits while a window is open, E2 waits until the window has closed (the button), Barrier events
  // (2C / 65 / 66 / A4 / 2A) wait until the action executor (queue_ + act_) has finished everything queued before them, and a queued attack
  // (ActionEvent::script_seq) starts only once its queue_attack event was released. Attacks keep running while later windows open (the
  // executor never looks at window 4). script_gap_ = ticks left before the front event is due (-1 = not looked at yet); script_passed_ =
  // events with seq < this were released (the gate of ActionEvent::script_seq); script_stall_ = ticks the front Barrier has waited (watchdog).
  std::deque<battle::pac::Event> script_stream_;
  u32 script_prev_vt_ = 0;
  int script_gap_ = -1;
  int script_passed_ = 0;
  int script_stall_ = 0;
  void script_pump();
  bool script_barrier_clear(const battle::pac::Event& event) const;
  bool script_gated(const battle::ActionEvent& event) const { return event.script_seq >= 0 && event.script_seq >= script_passed_; }
  bool script_busy() const { return !script_stream_.empty() || smsg_.open != 0; }
  void build_script_msg();
  std::vector<Op> ops_;
  std::vector<render::Mat4> palette_;
};

}  // namespace oc::game
