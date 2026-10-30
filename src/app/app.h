#pragma once
// The OpenCross application: everything the process owns, from the boot sequence to the main loop and the frame composition.
// One translation unit per concern:
//   app_boot.cpp      init(): config -> window -> VFS / mods -> GPU -> audio -> routes -> flow / saves -> menus -> field -> battle -> title
//   app_sim.cpp       advance(): one block of simulation (flow, pause, movies, menus, the field), the art / audio switches
//   app_battle.cpp    the field -> battle hand-off and back
//   app_route.cpp     scripted routes, input replay, the room sweep
//   app_debug.cpp     the Esc overlay's Debug tab
//   app_run.cpp       run(): the main loop (hotkeys, per-frame settings, mode dispatch, the options window's changes)
//   app_draw.cpp      draw_frame(): the frame composition (scene target, Ps1Res passes, 8F warp, overlays, pillarboxing)
//   app_feedback.cpp  the encounter feed-back / damage-jolt pass over the scene target
// The members stand in the order the objects are created; destruction runs in reverse, so every GPU resource goes before ctx and
// the voice player before the mixer.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "audio/sfx.h"
#include "audio/voice.h"
#include "data/tables.h"
#include "game/autosave.h"
#include "game/battle/battle_results.h"
#include "game/battle/battle_view.h"
#include "game/battle/texture_dump.h"
#include "game/cheats.h"
#include "game/achievements.h"
#include "game/debug_tools.h"
#include "platform/steam.h"
#include "game/field_world.h"
#include "game/game_flow.h"
#include "game/route_script.h"
#include "game/title_backdrop.h"
#include "import/text.h"
#include "movie/player.h"
#include "platform/config.h"
#include "platform/updater.h"
#include "platform/window.h"
#include "render/crt_pass.h"
#include "render/gpu_context.h"
#include "render/imgui_layer.h"
#include "render/mesh_pipeline.h"
#include "render/render_target.h"
#include "render/sprite_batch.h"
#include "render/text_batch.h"
#include "render/texture.h"
#include "save/cc_save.h"
#include "save/oc_saves.h"
#include "ui/keyitem_popup.h"
#include "ui/lowres.h"
#include "ui/menu/main_menu.h"
#include "ui/menu/picker_model.h"
#include "ui/menu/status_model.h"
#include "ui/monster_pick_screen.h"
#include "ui/party_screen.h"
#include "ui/feature_icons.h"
#include "ui/pause_screen.h"
#include "ui/world_name.h"
#include "ui/shop/shop_screen.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

namespace oc::app {

// Everything the BattleView needs from a field encounter request. Built when the load begins (the hold) and reused by the take-over.
struct FieldBattleArgs {
  game::FieldWorld::BattleRequest rq;
  int scene = -1;
  std::vector<battle::PartyMember> party;
  game::BattleView::StartOptions bopt;
};

// What one frame's composition shares between its passes (app_draw.cpp, app_feedback.cpp).
struct SceneFrame {
  // The window frame and its size.
  render::Frame frame;
  float vw = 0, vh = 0;
  bool field_shown = false, battle_shown = false, title_scene = false, title_sea = false, title_lowres = false;
  bool field_lowres = false, results_up = false, battle_3d = false, battle_lowres = false;
  // OC_NO_WARP (0 = off, 1 = unwarped, 2 = the deep part only).
  int warp_debug = 0;
  // The frame the 3D pictures are drawn into (the window or the scene target) and its size.
  render::Frame pf;
  float pw = 0, ph = 0;
  // [Video] RenderScale.
  float rscale = 1.0f;
  // The scene target of this frame when scene_scaled (scene_rt).
  bool scene_scaled = false;
  render::RenderTarget* srt = nullptr;
  render::TexRef* sset = nullptr;
  game::FieldWorld::JoltView jolt;
  game::FieldWorld::WaveView wave;
  bool wave_hd = false;
};

// The picture rectangle of one feed-back pass (app_feedback.cpp).
struct FeedbackRect;

class App {
 public:
  App() = default;
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // Boots the game. Returns 0 when it may run, else the process exit code.
  int init(int argc, char** argv);
  // The main loop; returns the process exit code.
  int run();

 private:
  // ---- app_boot.cpp -----------------------------------------------------------------------------------------------------------
  void init_config(int argc, char** argv);
  bool init_window();
  void init_vfs();
  bool init_render();
  void init_audio();
  bool init_routes();
  void init_flow();
  void init_saves();
  void init_menus();
  void init_field();
  void init_battle();
  void init_title();
  void init_start_state();

  // ---- app_sim.cpp ------------------------------------------------------------------------------------------------------------
  static bool confirm_edge(const platform::Input& in);
  void apply_audio_config();
  void apply_art_mode();
  void end_movie();
  void start_movie(const game::FieldWorld::MovieRequest& rq);
  void open_main_menu(bool script);
  void handle_script_menu(const game::FieldWorld::MenuRequest& rq);
  void apply_flow_action(const game::FlowAction& action);
  bool pause_modal(const platform::Input& in);
  void open_pause();
  void advance(const platform::Input& raw, double dt, int ticks);
  bool field_visible() const { return world_ok && world.view().loaded(); }
  // First open: the title room runs ~20 ticks before its script starts the logo FMV; nothing of it may be presented until the movie
  // takes over (or it was skipped / failed, which marks the logo ready).
  bool title_hold() const {
    return flow_ready && flow.scene_mode() && !flow.title_logo_ready() && !movie_open && (!field_visible() || world.tick_count() - title_scene_tick0 < 150);
  }

  // ---- app_battle.cpp ---------------------------------------------------------------------------------------------------------
  FieldBattleArgs make_field_battle_args(const game::FieldWorld::BattleRequest& rq);
  void field_battle_load_step();
  void start_field_battle();
  void finish_field_battle(bool after_results = false);
  void scene_battle();

  // ---- app_route.cpp ----------------------------------------------------------------------------------------------------------
  void init_route_state();
  void fill_route_state();
  void route_request(const game::RouteOut& out);
  void route_frame();
  void replay_frame();
  // Returns false when the sweep is over (the main loop ends).
  bool sweep_frame();

  // ---- app_debug.cpp ----------------------------------------------------------------------------------------------------------
  game::DebugTools::Ctx debug_tools_ctx();
  // One frame of Steam, then the game's achievement state for `ticks` ticks of play.
  void update_achievements(int ticks);
  void restart_game();
  void init_debug_tab();
  void debug_tools_frame();

  // ---- app_run.cpp ------------------------------------------------------------------------------------------------------------
  void log_frametime();
  void frame_hotkeys();
  void settings_pad_filter();
  // RDE's speed toggles do nothing in the Radius tutorial (its field map or its battle).
  bool speed_locked() const;
  void rde_toggle_keys();
  void frame_settings();
  // Returns false when the run is over (the sweep's last room).
  bool simulate();
  void field_frame();
  void apply_options_change(const render::OptionsChange& change);

  // ---- app_draw.cpp -----------------------------------------------------------------------------------------------------------
  void draw_frame(const render::Frame& frame);
  void apply_frame_options();
  void draw_scene(SceneFrame& sf);
  void begin_scene_target(SceneFrame& sf);
  void draw_battle_picture(const SceneFrame& sf);
  void draw_title_lowres(const SceneFrame& sf);
  void draw_field_lowres(const SceneFrame& sf);
  void draw_field_full(const SceneFrame& sf);
  void draw_battle_ui(const SceneFrame& sf);
  void draw_field_ui(const SceneFrame& sf);
  void draw_title_menu(const SceneFrame& sf);
  void field_clip(const SceneFrame& sf, bool on);
  bool ensure_target(render::RenderTarget& rt, render::TexRef& set, u32 w, u32 h, const render::Sampler& smp, bool multisampled = false);
  void draw_field_slab(const SceneFrame& sf, const render::Frame& slab_frame, float fw, float fh, bool clip);
  void draw_warp(const SceneFrame& sf, render::TexRef set, const render::Letterbox& dst, const render::Letterbox& src, float sw, float sh);
  void show_lowres(const SceneFrame& sf, render::TexRef set, const render::Letterbox& lb, float lw, float lh);
  template <typename Draw>
  bool ps1_pass_43(const render::Frame& frame, float vw, float vh, render::TextBatch* tb, Draw&& draw);
  template <typename Draw>
  bool ps1_pass_wide(const render::Frame& frame, float vw, float vh, render::TextBatch* tb, Draw&& draw);

  // ---- app_feedback.cpp -------------------------------------------------------------------------------------------------------
  void composite_scene(const SceneFrame& sf);
  void feedback_pass(const SceneFrame& sf, render::TexRef& outset, bool& fb_done);
  void feedback_hold_pass(const SceneFrame& sf, const FeedbackRect& rect, const game::FieldWorld::TransitionStrips& ts);
  void feedback_field_pass(const SceneFrame& sf, const FeedbackRect& rect);
  void jolt_paste(const SceneFrame& sf);
  void keep_jolt_picture(const SceneFrame& sf);
  bool ensure_transition_targets(u32 w, u32 h);
  void scene_extent(float w, float h, u32& tw, u32& th) const;
  void draw_scene_copy(const SceneFrame& sf, const render::Frame& target, const render::TexRef& scene);

  // ---- the session ------------------------------------------------------------------------------------------------------------
  std::filesystem::path exe;
  std::filesystem::path config_file;
  bool own_ini = true;
  std::string log_path;
  bool log_forced = false;
  platform::Config config;
  // No ini was found: the defaults were written and [Game] Language follows the OS once the window has initialised SDL.
  bool first_launch = false;
  std::filesystem::path game_dir;
  std::filesystem::path save_dir;

  platform::Window window;
  vfs::Vfs files;
  vfs::ModManager mods;
  game::TextureDumpJob texture_dump_job;

  // ---- rendering --------------------------------------------------------------------------------------------------------------
  render::GpuContext ctx;
  render::SpriteBatch sprites;
  render::MeshPipeline meshes;
  render::Sampler linear;
  render::Texture background;
  render::ImGuiLayer ui;
  render::CrtPass crt_pass;
  bool crt_ok = false;
  // Set when the Mods tab changed the mod list: re-apply the art mode after the overlay window.
  bool mods_dirty = false;
  platform::Updater updater;
  platform::SteamClient steam;
  bool steam_allowed = false;
  bool steam_wanted = false;
  game::Achievements achievements;
  // API names earned this run; routes read them.
  std::vector<std::string> earned_achievements;
  bool restart_for_update = false;
  render::TexRef background_set;
  // Classic art, [Video] Ps1Res: the field picture is drawn into the PS1 field buffer size (320 x 224) and scaled up with nearest
  // sampling into RDE's field frame, so models / effects have the pixel size of the 1x backdrops.
  render::RenderTarget field_rt;
  render::Sampler nearest;
  render::TexRef field_rt_set;
  // 8F screen ripple: the frame drawn so far is re-drawn warped. Classic PS1-resolution art: the deep part goes into field_rt, the
  // warp + near part into wave_lo_rt (320 x 224); otherwise the deep part into wave_rt (window size), warped into the frame.
  render::RenderTarget wave_lo_rt, wave_rt;
  render::TexRef wave_lo_set, wave_set;
  // [Video] RenderScale: the window-resolution 3D pictures at the scale x the window size (created on first use, follows the size).
  render::RenderTarget scene_rt;
  render::TexRef scene_set;
  // Damage-floor blur (FieldWorld::jolt_view): while it runs the field picture (scene_rt) is also kept in one of two window-sized
  // targets, and the other one (the previous picture) is blended over it at 50 %. jolt_rt[1 - jolt_cur] holds the previous picture when
  // jolt_have is set. Window-sized, whatever the render scale: the transitions allocate nothing of the scene target's size.
  render::RenderTarget jolt_rt[2];
  render::TexRef jolt_set[2] = {};
  // The window size ensure_transition_targets last ran for ahead of a transition.
  u32 transition_w = 0, transition_h = 0;
  int jolt_cur = 0;
  bool jolt_have = false;
  int jolt_last_n = -1;
  // Encounter feed-back pass (sequence ops 3E / 40): the picture of every tick is 50 % blended with the zoomed PREVIOUS OUTPUT, kept
  // here (two window-sized targets, ping-pong). fb_flip_next: the next game tick writes the other target (false inside a strip update:
  // its anchor stays the previous output). fb_have: fb_rt[1 - fb_cur] holds the previous output.
  render::RenderTarget fb_rt[2];
  render::TexRef fb_set[2] = {};
  int fb_cur = 0, fb_last_n = -1;
  bool fb_flip_next = true;
  bool fb_have = false;
  // The hold after the battle-start white clear shows no fresh field picture: the strips keep sampling the last pre-white output.
  render::RenderTarget fb_frozen;
  render::TexRef fb_frozen_set;
  bool fb_frozen_have = false;

  import::Lang lang = import::Lang::En;
  bool sweeping = false;

  // ---- audio ------------------------------------------------------------------------------------------------------------------
  audio::Mixer mixer;
  std::unique_ptr<audio::VoicePlayer> voice_player;
  audio::LogSfx log_sfx;
  audio::SfxBank sfx_bank;
  audio::AkaoSfx akao_sfx{&mixer, &sfx_bank};
  bool sfx_ok = false;
  audio::Sfx* sfx_sink = nullptr;
  std::function<void(int)> ui_sfx;

  // ---- scripted input ---------------------------------------------------------------------------------------------------------
  game::Route route;
  game::InputReplayer replayer;
  game::InputRecorder recorder;
  bool routing = false;
  bool replaying = false;
  std::filesystem::path shots_dir;
  std::optional<game::FlowState> flow_start;
  bool flow_enabled = true;
  std::string dev_room;

  // ---- game data, flow, saves -------------------------------------------------------------------------------------------------
  data::GameTables tables;
  bool tables_ok = false;
  game::GameFlow flow;
  bool flow_ready = false;
  save::SaveDat save_dat;
  bool have_save_dat = false;
  // The OpenCross save page (folder OpenCross next to save.dat).
  save::OcSaves oc_saves;
  std::filesystem::path save_path;
  save::ExtHost ext_host;
  game::Autosave autosave;

  // ---- menus and screens ------------------------------------------------------------------------------------------------------
  render::TextBatch menu_text;
  // [Video] Ps1Res: the PS1-resolution target of the battle HUD / results / pause screen / field windows (any art mode).
  ui::LowRes ps1_ui;
  ui::MainMenu menu;
  ui::ShopScreen shop;
  ui::StatusModelView status_model{meshes, files};
  ui::KeyItemPopup key_popup;
  ui::PauseScreen pause_screen;
  bool pause_ok = false;
  // RDE's feature icons and the hotkeys / pad buttons of its play-time toggles.
  ui::FeatureIcons feature_icons;
  bool feature_icons_ok = false;
  game::RdeHotkeys rde_hotkeys;
  // Pad buttons / axes held while the settings window was open: hidden from the game until let go.
  std::array<bool, platform::Input::kButtons> settings_held_buttons{};
  std::array<bool, platform::Input::kAxes> settings_held_axes{};
  // The pad's Start press that may still open the settings window (held long enough) or the pause screen (released before), and when it
  // went down; start_tap = this frame released it early (the pause screen's Start).
  bool start_held = false, start_tap = false;
  // The left stick's d-pad directions of the previous advance() (platform::stick_to_dpad).
  std::array<bool, 4> stick_dpad{};
  u64 start_down_ms = 0;
  game::SlowFieldCycle slow_field_cycle;
  // RDE's HD "Home / Another World" name over the world maps.
  ui::WorldName world_name;
  bool world_name_ok = false;
  ui::PartyScreen party_screen;
  bool party_ok = false;
  // The Grand Slam monster picker (FE 75 + FE 61) and its creature models.
  ui::MonsterPickScreen monster_picker;
  ui::PickerModelView picker_models{meshes, files};
  bool monster_picker_ok = false;
  bool menu_ok = false, shop_ok = false, key_popup_ok = false;

  // ---- the field, battles, the title ------------------------------------------------------------------------------------------
  game::FieldWorld world;
  bool world_ok = false;
  game::BattleView battle;
  // battle_ok: [Debug] Battle, the battle view runs alone; battle_ready: BattleView initialised (tables loaded); battle_in_field: a
  // field encounter is being fought.
  bool battle_ok = false;
  bool battle_ready = false;
  bool battle_in_field = false;
  game::BattleResults results;
  bool results_ok = false;
  // results_shown: the standalone [Debug] Battle opens the screen once; battle_scene: scene of the running field encounter.
  bool results_shown = false;
  int battle_scene = -1;
  game::TitleBackdrop title_bd;
  // The field tick the title scene began at.
  u64 title_scene_tick0 = 0;
  // Routes count sequence starts (an art switch must not restart the title sequence).
  u64 title_plays = 0;
  std::string title_seq;
  size_t sweep_first = 0;
  std::vector<std::string> sweep_rooms;

  // ---- the main loop ----------------------------------------------------------------------------------------------------------
  platform::Input input;
  platform::FrameTimer timer;
  platform::FixedStep stepper;
  double frame_dt = 0.0;
  platform::FixedStep::Result step{0, 0.0f};
  bool screenshot_pending = false;
  u64 screenshot_tick = 0;
  const char* screenshot_name = "screenshot.png";
  size_t sweep_idx = 0, sweep_failed = 0, sweep_layer_problems = 0, sweep_npc_rooms = 0;
  bool exit_when_idle = false;
  int exit_countdown = 0;
  u64 frame_count = 0;
  bool quit_requested = false;
  bool restart_requested = false;
  // [Video] Widescreen: the extra layer columns of this frame's picture, and the columns the HUD / menus spread out by.
  float wide_extra = 0.0f;
  float hud_cols = 0.0f;

  // ---- overlays driven from the field: FMV, main menu, shops ------------------------------------------------------------------
  movie::Player movie_player;
  bool movie_open = false, movie_skippable = false;
  // The last movie frame is held and faded into the next scene once the field has run a few ticks (or entered another room).
  std::string ghost_room;
  u64 ghost_tick0 = 0;
  bool menu_from_script = false, shop_open = false;
  int battle_flags = 0;
  // The growth progress counter is not in the save, kept for the session.
  int growth_progress = 0;
  u32 battle_counter = 0;
  // Routes count field battles started (an art switch must not restart a battle).
  u64 battle_starts = 0;
  // The Debug tab's battle / room / party / cast / script tools; using one disables saving for the session.
  game::DebugTools debug_tools;
  std::optional<FieldBattleArgs> fb_args;
  bool fb_prefetched = false;
  // battle_warmed: BattleView::warm_up is done; fb_req_frames: frames since the encounter request (the load starts a few frames later).
  bool battle_warmed = false;
  int fb_req_frames = 0;
  // A flow action whose room load waits for its movie (New Game: a02koryu, then opn_a01i).
  game::FlowAction deferred_flow;
  bool deferred_flow_set = false;

  // ---- routes -----------------------------------------------------------------------------------------------------------------
  game::RouteState rstate;
  u64 route_tick = 0;
  int exit_code = 0;
  bool route_finished = false;

  // ---- debugging --------------------------------------------------------------------------------------------------------------
  // A field room is shown this frame (the Debug tab's field sections).
  bool debug_field = false;
  platform::FrameLimiter limiter;
  // OC_FRAMETIME logs the wall time of main-loop iterations over 20 ms while a field -> battle hand-over is running; a value starting
  // with 2 logs every iteration.
  bool frametime_log = false;
  std::chrono::steady_clock::time_point ft_prev;
  u64 ft_frame = 0;
};

// A 4:3 screen (results, pause) drawn on the 1280 x 960 canvas into a 320 x 240 target and scaled up nearest into the canvas rectangle.
template <typename Draw>
bool App::ps1_pass_43(const render::Frame& frame, float vw, float vh, render::TextBatch* tb, Draw&& draw) {
  static constexpr ui::Ps1Display kD{320, 240, "lowres 4:3 320x240"};
  if (!ps1_ui.ready()) ps1_ui.init(ctx, sprites);
  render::Frame lf;
  if (!ps1_ui.begin(frame, kD, tb, lf)) return false;
  draw(lf);
  ps1_ui.end(frame, vw, vh);
  return true;
}

// The widescreen picture frame's UI (battle HUD, field windows): a (320 + 2 e) x 240 target, e = render::wide_extra() columns per side
// (the 240-row frame: picture rows 2..238 line up with the field / battle picture), scaled up nearest into Letterbox::wide(vw, vh, e)'s
// rectangle. `draw` gets the target's frame, its pixel size and a Letterbox::identity(1280 + 8 e, 960, 4) for canvas-unit drawers.
template <typename Draw>
bool App::ps1_pass_wide(const render::Frame& frame, float vw, float vh, render::TextBatch* tb, Draw&& draw) {
  const float extra = render::wide_extra();
  const u32 tw = static_cast<u32>(std::lround(render::kLayerW + 2.0f * extra));
  char tag[40];
  std::snprintf(tag, sizeof tag, "lowres ui %ux240", tw);
  // Ps1Display keeps the pointer (OC_LOWRES_LOG): one string per width.
  static std::string tag_store;
  tag_store = tag;
  const ui::Ps1Display display{tw, 240, tag_store.c_str()};
  if (!ps1_ui.ready()) ps1_ui.init(ctx, sprites);
  const float cw = 1280.0f + 8.0f * extra;
  render::Frame lf;
  if (!ps1_ui.begin(frame, display, tb, lf, cw, 960.0f)) return false;
  draw(lf, static_cast<float>(tw), 240.0f, render::Letterbox::identity(cw, 960.0f, 4.0f));
  const render::Letterbox wide = render::Letterbox::wide(vw, vh, extra);
  const render::Letterbox::Rect dst{wide.left(), wide.oy, wide.width(), render::kLayerH * wide.scale};
  ps1_ui.end(frame, vw, vh, &dst);
  return true;
}

}  // namespace oc::app
