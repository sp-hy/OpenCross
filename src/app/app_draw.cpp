// The frame composition. The 3D pictures (title sea, field, battle) go into the window or the [Video] RenderScale scene target;
// classic art + [Video] Ps1Res draws them into a 320 (+ widescreen columns) x 224 target scaled up nearest; the 8F screen ripple
// re-draws the deep part warped; then the HUD, dialogue windows, menus, pillarboxing, the screen veils, the CRT pass and the pause
// screen at window resolution.
#include "app/app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/free_cam.h"
#include "render/ps1_jitter.h"
#include "render/stage_pipeline.h"

namespace oc::app {

namespace {

// [Video] Ps1Res: the PS1 buffer size exactly - the render scale does not apply to the PS1-resolution pictures.
void native_extent(float w, float h, u32& tw, u32& th) {
  tw = static_cast<u32>(std::max(1.0f, std::round(w)));
  th = static_cast<u32>(std::max(1.0f, std::round(h)));
}

// Black bars beside the 4:3 picture frame `fit`. The black overlaps the picture's (fractional) edge by up to a pixel: floor / ceil the
// other way left a sub-pixel seam where the scene behind blended through (slivers of the title screen near the black bars).
void pillarbox(render::SpriteBatch& sprites, const render::Frame& frame, const render::Letterbox& fit, float vw) {
  const float height = static_cast<float>(frame.extent.height);
  const float left = std::clamp(std::ceil(fit.ox), 0.0f, vw);
  const float right = std::clamp(std::floor(fit.ox + render::kLayerW * fit.scale), left, vw);
  const u32 black = render::pack_rgba(0, 0, 0);
  sprites.begin(frame, static_cast<float>(frame.extent.width), height, true);
  sprites.draw(frame.ctx->white(), 0.0f, 0.0f, left, height, 0.0f, 0.0f, 1.0f, 1.0f, black);
  sprites.draw(frame.ctx->white(), right, 0.0f, static_cast<float>(frame.extent.width) - right, height, 0.0f, 0.0f, 1.0f, 1.0f, black);
  sprites.end();
}

}  // namespace

void App::draw_frame(const render::Frame& frame) {
  const float vw = static_cast<float>(frame.extent.width);
  const float vh = static_cast<float>(frame.extent.height);
  // The pause screen filters the finished picture through the CRT pass target (ramp mode).
  const bool pause_on = pause_ok && pause_screen.active();
  // The pause screen holds everything under it: the battle view stops (alpha 1), the movie player stops (frame held), and the mixer
  // freezes the music / voice / movie PCM (RDE pauses the sound thread and the movie).
  battle.set_paused(pause_on && (battle_ok || battle_in_field));
  movie_player.set_paused(pause_on && movie_open);
  mixer.hold(pause_on);
  // The whole frame goes through the CRT pass (ended before the overlay).
  const bool crt_on = (config.crt || pause_on) && crt_ok && crt_pass.begin(ctx, frame);
  apply_frame_options();
  const bool boot_logo = flow_ready && flow.boot_logo_active();
  const bool battle_shown = battle_ok || battle_in_field;
  const bool title_scene = flow_ready && flow.scene_mode() && field_visible() && !movie_open;
  // The title room's layers (PS1 logo / glow) and the title menu stay 4:3, centred: composed with 0 extra columns (restored after the
  // frame). The 3D sea under them (TitleBackdrop: Letterbox::wide with screen_extra) and the screen veils keep the window's width.
  // (The attract demo rooms stay widescreen.)
  if (title_scene && world.room_name() == game::kTitleRoom) render::set_wide_extra(0.0f);
  // HD art: RDE replaces the PS1 logo layer and prompt with title.png + its own prompt. Classic
  // (SD) art keeps the disc's own title room layers (PS1 logo / (c) / prompt), so nothing is hidden there.
  // HD art: RDE draws its own world name (draw_field_ui) instead of the world maps' screen-fixed name layers.
  // The world name (a map with var 0x50 set) follows the widescreen edge like RDE's HD name.
  if (world_ok) world.view().set_fixed_to_edge(world.vm().var(0x50) != 0);
  if (world_ok)
    world.view().set_hidden_groups(
        (title_scene && (platform::art_is_hd(config.art_mode) || game::free_cam().hide_overlays()) && world.room_name() == game::kTitleRoom ? 0x7u : 0u) |
        (platform::art_is_hd(config.art_mode) && world_name_ok && world.vm().var(0x50) != 0 ? world.view().fixed_groups() : 0u));
  // A disabled flow ([Debug] Battle / sweep / replay) is never started and would still "own the screen" in its initial Title state (black
  // frame over the battle): only an enabled flow draws.
  const bool flow_screen = flow_ready && flow_enabled && flow.owns_screen() && !movie_open && !title_scene;
  const bool field_shown = field_visible() && !battle_shown && !flow_screen && !movie_open;
  const bool title_held = title_hold();
  if (movie_open) {
    movie_player.draw(frame, vw, vh);
  } else if (boot_logo) {
    flow.draw_boot_logo(frame, vw, vh);
  } else if (title_held) {
    // black frame
  } else if (flow_screen) {
    flow.draw(frame, vw, vh);
  } else {
    SceneFrame sf;
    sf.frame = frame;
    sf.vw = vw;
    sf.vh = vh;
    sf.field_shown = field_shown;
    sf.battle_shown = battle_shown;
    sf.title_scene = title_scene;
    draw_scene(sf);
  }
  // Screen veils go on top of everything drawn: the held last movie frame, the load fades (Letterbox::veil_rect).
  if (!title_held) {
    if (!movie_open) movie_player.draw_held(frame, vw, vh);
    if (flow_ready) flow.draw_overlay(frame, vw, vh);
  }
  // The title's 4:3 override ends with the composition (the ticks use the real value).
  render::set_wide_extra(wide_extra);

  if (crt_on) {
    float tint[3] = {1.0f, 1.0f, 1.0f};
    // The pause screen's colour cycles over 480 frames.
    if (pause_on) pause_screen.tint(tint);
    crt_pass.set_ramp(pause_on, tint);
    crt_pass.end(ctx, frame);
  }
  // RDE's feature icons over the picture, under the pause screen's bars. They belong to play: the boot logo, the movies, the title
  // scene and its slot list / name entry do not show them.
  const bool before_play = movie_open || boot_logo || title_held ||
                           (flow_ready && flow_enabled && flow.state() != game::FlowState::Opening && flow.state() != game::FlowState::Field);
  if (feature_icons_ok && !before_play && !game::free_cam().hide_overlays()) {
    const bool locked = speed_locked();
    const ui::FeatureIcons::Active active{config.cheats.auto_battle, config.cheats.battle_boost, config.cheats.no_encounters,
                                          config.slow_mode && !locked, config.fast_mode && !locked};
    import::Lang icon_lang = import::Lang::En;
    import::parse_lang(config.language, icon_lang);
    feature_icons.draw(frame, vw, vh, active, icon_lang);
  }
  // The pause screen's bars + text go over the filtered picture.
  if (pause_on && !(config.ps1_res && ps1_pass_43(frame, vw, vh, &menu_text, [&](const render::Frame& lf) { pause_screen.draw(lf, 1280.0f, 960.0f, true); })))
    pause_screen.draw(frame, vw, vh, true);
}

// The [Video] / [Enhancements] settings and the game options the drawers read, applied every frame.
void App::apply_frame_options() {
  // [Video] Ps1Res (Esc options; the UI screens read it in original art).
  ui::set_ps1_menus(config.ps1_res);
  // [Video] Filter: the field actors' texture sampling, one global setting (Graphics tab).
  if (world_ok) world.view().linear_filter = config.filter == platform::TexFilter::Bilinear;
  if (results_ok) results.set_filter(config.filter == platform::TexFilter::Bilinear);
  // The filter covers the stage, units and weapons of the battle.
  if (battle_ready) battle.set_texture_filter(config.filter == platform::TexFilter::Bilinear);
  // The [Enhancements] are independent of Ps1Res: SmoothEffectRings, SmoothWaterLine, SmoothReflections (env-mapped parts), Lighting
  // (the key light for field + battle models, and the spell glow), EdgeBlend (the stage pipelines draw the modded tiles' seam strips).
  {
    const bool rings = config.smooth_effect_rings;
    meshes.set_smooth_water_line(config.smooth_water_line);
    meshes.set_smooth_reflections(config.smooth_reflections);
    meshes.set_enhanced_lighting(config.lighting);
    render::set_edge_blend(config.edge_blend);
    if (battle_ready) battle.set_spell_lights(config.lighting);
    if (battle_ready) battle.set_autohide_cast(config.autohide_cast_hud);
    if (battle_ready) battle.set_keep_intro_layer(config.retain_battle_scenery);
    if (world_ok) world.set_smooth_rings(rings);
    if (battle_ready) battle.set_smooth_rings(rings);
  }
  // [Enhancements] BattleShadows (any art mode / filter).
  if (battle_ready) battle.set_smooth_fog(config.battle_shadows);
  if (battle_ready && world_ok)
    battle.set_menu_options(world.game_state().option(ui::opt::kCursorMemory), world.game_state().option(ui::opt::kAutoAttack));
  if (world_ok) mixer.set_mono(world.game_state().option(ui::opt::kMono));
  {  // Vibration Function (an option bit): the sequence tasks' motor bytes of this frame -> the gamepad (platform::Rumble)
    const bool vib = world_ok && world.game_state().option(ui::opt::kVibration);
    window.set_rumble(platform::Rumble::instance().flush(vib, frame_dt));
  }
  if (results_ok && world_ok) results.set_cursor_memory(world.game_state().option(ui::opt::kCursorMemory));
}

// The game picture: title sea, field or battle 3D, then the UI over it.
void App::draw_scene(SceneFrame& sf) {
  const float vw = sf.vw, vh = sf.vh;
  const bool field_shown = sf.field_shown, battle_shown = sf.battle_shown;
  // The title room's 3D sea scene is the backdrop: drawn first, the room layers (PS1 logo / glow, white fade) blend over it.
  // Widescreen: the sea fills the widened frame like the battle 3D (same H and centre (160, 112), more columns; no clamp).
  sf.title_sea = field_shown && title_bd.running() && world.room_name() == game::kTitleRoom;
  sf.title_lowres = sf.title_sea && config.ps1_res;
  // [Video] Ps1Res (any art mode): the whole field picture (layers, shadows, models, effects: one depth space) is drawn into the
  // 320x224 target, then scaled up into the field frame. Not under the title's 3D sea (the layers blend over that backdrop).
  sf.field_lowres = field_shown && !sf.title_sea && field_rt_set && config.ps1_res;
  sf.results_up = battle_shown && results_ok && results.active();
  sf.battle_3d = battle_shown && !sf.results_up && battle.scene_visible();
  sf.battle_lowres = sf.battle_3d && field_rt_set && config.ps1_res;
  {  // [Video] Ps1Jitter: GTE vertex snapping + affine textures in the 3D pipelines; the grid = the PS1 pixels of the picture being drawn
    render::Ps1Jitter& jit = render::g_ps1_jitter;
    jit.on = config.ps1_jitter && (field_shown || sf.battle_3d || sf.title_sea);
    if (sf.field_lowres || sf.battle_lowres) {
      jit.px = render::kLayerW * 0.5f;
      jit.py = render::kFieldRows * 0.5f;
    } else if (sf.title_lowres) {
      jit.px = render::kLayerW * 0.5f + render::screen_extra();
      jit.py = render::kLayerH * 0.5f;
    } else {
      const render::Letterbox lb = field_shown && !sf.title_sea ? render::Letterbox::field(vw, vh) : render::Letterbox::wide(vw, vh, wide_extra);
      jit.px = vw / (2.0f * std::max(lb.scale, 1e-3f));
      jit.py = vh / (2.0f * std::max(lb.scale_y, 1e-3f));
    }
  }
  begin_scene_target(sf);
  if (sf.title_lowres) draw_title_lowres(sf);
  else if (sf.title_sea) title_bd.draw(sf.pf, sf.pw, sf.ph, step.alpha, config.filter == platform::TexFilter::Bilinear);
  // 8F / 90 screen ripple (render::build_screen_warp): what the field files at OT index > wave.ot is drawn first into a target, re-drawn
  // warped into the picture, then the nearer content is drawn over it (FieldWorld::set_draw_slab).
  // Debug (A/B pixel checks): OC_NO_WARP=1 draws the ripple's frame unwarped; =2 (classic PS1-resolution art) shows only its deep part,
  // unwarped (the input of the warp).
  static const int warp_debug = std::getenv("OC_NO_WARP") ? std::atoi(std::getenv("OC_NO_WARP")) : 0;
  static const bool no_warp = warp_debug == 1;
  sf.warp_debug = warp_debug;
  sf.wave = field_shown && !sf.title_sea && !no_warp ? world.wave_view() : game::FieldWorld::WaveView{};
  sf.wave_hd = sf.wave.on && !sf.field_lowres;
  // Submerged actor faces first (opaque, depth written), so the water layer blends over them; the rest after the layers.
  if (field_shown && !sf.field_lowres && !sf.wave_hd)
    world.draw_actors(meshes, sf.pf, step.alpha, render::MeshPipeline::DrawParams::WetFaces::WetOnly);
  // Over the title's widescreen sea the layers clip themselves (their darkening veil reaches past the 4:3 room picture).
  if (field_shown && !sf.field_lowres && !sf.wave_hd && !sf.title_sea) field_clip(sf, true);
  // (The test image below is only drawn when nothing is scaled: pf = frame.) Resumed: the PS1-resolution title sea's quad is already in
  // the batch.
  sprites.begin(sf.pf, sf.pw, sf.ph, true);
  // ui/background.png (the test image) only when no game picture is shown: the field / battle are composed over BLACK;
  // under a field room its 16:9 picture showed through every additive base layer (dem_01, hid_*: all STP, ABR 1).
  if (background_set && !sf.title_sea && !field_shown && !battle_shown) {
    const float scale = render::canvas_fit(vw, vh, static_cast<float>(background.width()), static_cast<float>(background.height())).s;
    const float w = static_cast<float>(background.width()) * scale;
    const float h = static_cast<float>(background.height()) * scale;
    sprites.draw(background_set, (vw - w) * 0.5f, (vh - h) * 0.5f, w, h);
  }
  if (field_shown && !sf.field_lowres && !sf.wave_hd) world.draw_background(sprites, sf.pw, sf.ph, step.alpha);
  sprites.end();
  if (field_shown && !sf.field_lowres && !sf.wave_hd) field_clip(sf, false);
  if (battle_shown) draw_battle_picture(sf);
  if (field_shown && sf.field_lowres) draw_field_lowres(sf);
  if (field_shown) draw_field_full(sf);
  // Render scale: the scene target into the window (the frame is cleared black under every 3D picture: additive = a plain copy).
  if (sf.scene_scaled) composite_scene(sf);
  if (sf.battle_3d) draw_battle_ui(sf);
  if (field_shown) draw_field_ui(sf);
  if (sf.title_scene && world.room_name() == game::kTitleRoom) draw_title_menu(sf);
}

// [Video] RenderScale: the window-resolution 3D pictures - the title sea, the field picture (its layers share the actors' depth space,
// so they go in too), the battle 3D - are drawn into `scene_rt` at the scale x the window size with the same mapping (every drawer
// takes its letterbox from the frame size it is given), then composited into the frame: area-filtered down (SpriteFilter::Box) above
// 1x, bilinear up below. The HUD, menus, dialogue and veils follow at window resolution. The PS1-resolution passes (classic art +
// Ps1Res) scale their own 320 x 224 target instead. The damage-floor blur routes the picture through a target in any case, so the
// previous picture can be pasted over it.
void App::begin_scene_target(SceneFrame& sf) {
  const render::Frame& frame = sf.frame;
  const float vw = sf.vw, vh = sf.vh;
  const float rscale = config.render_scale;
  sf.rscale = rscale;
  // The frame the 3D pictures are drawn into: the window, or scene_rt.
  sf.pf = frame;
  sf.pw = vw;
  sf.ph = vh;
  // The jolt / encounter feed-back passes work on the finished picture: with Ps1Res the PS1-resolution field is scaled up into the target.
  const game::FieldWorld::JoltView jolt = sf.field_shown && !sf.title_sea ? world.jolt_view() : game::FieldWorld::JoltView{};
  sf.jolt = jolt;
  // Made ahead once per window size (a failed creation is not retried every frame; the passes try again when they run).
  if (sf.field_shown && !sf.title_sea && (frame.extent.width != transition_w || frame.extent.height != transition_h)) {
    transition_w = frame.extent.width;
    transition_h = frame.extent.height;
    ensure_transition_targets(transition_w, transition_h);
  }
  if (!jolt.on) {
    jolt_have = false;
    jolt_last_n = -1;
    fb_have = false;
    fb_last_n = -1;
  // A new game tick is a new picture: the old one becomes the "previous".
  } else if (jolt.n != jolt_last_n) {
    jolt_cur = 1 - jolt_cur;
    jolt_have = jolt_last_n >= 0;
    jolt_last_n = jolt.n;
  }
  render::RenderTarget& srt = scene_rt;
  render::TexRef& sset = scene_set;
  sf.srt = &srt;
  sf.sset = &sset;
  bool scene_scaled = jolt.on || (rscale != 1.0f && ((sf.title_sea && !sf.title_lowres) || (sf.field_shown && !sf.field_lowres) || (sf.battle_3d && !sf.battle_lowres)));
  if (scene_scaled) {
    u32 tw = 0, th = 0;
    scene_extent(vw, vh, tw, th);
    scene_scaled = jolt.on || (tw != frame.extent.width || th != frame.extent.height);
    if (scene_scaled && (!srt.valid() || srt.width() != tw || srt.height() != th || !sset)) {
      sset = {};
      if (srt.create(ctx, tw, th, true)) sset = srt.ref(linear);
      if (!sset) {
        log::warn("render scale {}: no {}x{} scene target, drawing at window resolution", rscale, tw, th);
        srt.destroy();
        scene_scaled = false;
      }
    }
    if (scene_scaled) {
      ctx.begin_target(srt, frame, sf.pf);
      sf.pw = static_cast<float>(tw);
      sf.ph = static_cast<float>(th);
    }
  }
  sf.scene_scaled = scene_scaled;
}

// w x h times the render scale, at most 16384 on a side (the usual GPU limit): the scene target's size.
void App::scene_extent(float w, float h, u32& tw, u32& th) const {
  const float fit_scale = std::min({config.render_scale, 16384.0f / std::max(w, 1.0f), 16384.0f / std::max(h, 1.0f)});
  tw = static_cast<u32>(std::max(1.0f, std::round(w * fit_scale)));
  th = static_cast<u32>(std::max(1.0f, std::round(h * fit_scale)));
}

void App::draw_battle_picture(const SceneFrame& sf) {
  const render::Frame& frame = sf.frame;
  const float vw = sf.vw, vh = sf.vh;
  if (sf.results_up) {
    if (!(config.ps1_res && ps1_pass_wide(frame, vw, vh, results.text_batch(),
                                          [&](const render::Frame& lf, float, float, const render::Letterbox& lb) { results.draw(lf, lb.vw, lb.vh, true); })))
      results.draw(frame, vw, vh, true);
  } else if (sf.battle_lowres) {
    // Original art + [Video] Ps1Res: the battle 3D (stage, shadows, units, effects) into the 320 x 224 battle buffer size
    // (the same size as the field's, so the field target is reused) x the render scale, scaled into the picture frame;
    // the HUD, menus and text are drawn afterwards at window resolution.
    // Widescreen: the target is 320 + 2 * wide_extra() columns wide (16:9: 426), the extra columns centred around the 320.
    render::Frame lf;
    u32 tw = 0, th = 0;
    native_extent(render::kLayerW + 2.0f * render::wide_extra(), game::BattleView::kBufferRows, tw, th);
    const float lw = static_cast<float>(tw), lh = static_cast<float>(th);
    ensure_target(field_rt, field_rt_set, tw, th, nearest);
    // One log line per target size (the battle 3D is drawn at the PS1 size x render scale).
    static u32 logged_tw = 0, logged_th = 0;
    if (tw != logged_tw || th != logged_th) log::info("battle 3D: PS1-resolution target {}x{}", logged_tw = tw, logged_th = th);
    ctx.begin_target(field_rt, frame, lf);
    battle.draw_scene(lf, render::Letterbox::identity(lw, lh, lh / game::BattleView::kBufferRows), true);
    ctx.end_target(field_rt);
    show_lowres(sf, field_rt_set, game::BattleView::picture(sf.pw, sf.ph), lw, lh);
  } else if (sf.battle_3d) {
    // (With a render scale this goes to scene_rt; composited below.)
    battle.draw_scene(sf.pf, game::BattleView::picture(sf.pw, sf.ph), true);
  }
}

// [Video] Ps1Res: the title's 3D sea into the (320 + 2 * screen_extra()) x 240 target, scaled up nearest into the widened frame (the
// room layers over it stay at window resolution).
void App::draw_title_lowres(const SceneFrame& sf) {
  const bool linear_tex = config.filter == platform::TexFilter::Bilinear;
  const float extra = render::screen_extra();
  u32 tw = 0, th = 0;
  native_extent(render::kLayerW + 2.0f * extra, render::kLayerH, tw, th);
  if (!ensure_target(field_rt, field_rt_set, tw, th, nearest)) {
    title_bd.draw(sf.pf, sf.pw, sf.ph, step.alpha, linear_tex);
    return;
  }
  const float lw = static_cast<float>(tw), lh = static_cast<float>(th);
  render::Frame lf;
  ctx.begin_target(field_rt, sf.frame, lf);
  title_bd.draw(lf, lw, lh, step.alpha, linear_tex);
  ctx.end_target(field_rt);
  const render::Letterbox lb = render::Letterbox::wide(sf.pw, sf.ph, extra);
  const float bu = 1.0f / (64.0f * lw), bv = 1.0f / (64.0f * lh);
  sprites.begin(sf.pf, sf.pw, sf.ph, true);
  sprites.set_blend(render::BlendMode::Add);
  sprites.draw(field_rt_set, lb.left(), lb.oy, lb.width(), render::kLayerH * lb.scale_y, bu, bv, 1.0f + bu, 1.0f + bv);
  sprites.set_blend(render::BlendMode::Alpha);
  sprites.end();
}

// Classic art + Ps1Res: the field picture into the 320 (+ 2 * wide_extra()) x 224 target (16:9: 426 x 224), layer columns 0..320
// centred (Letterbox::identity); x the render scale (2x: 640 x 448 / 852 x 448).
void App::draw_field_lowres(const SceneFrame& sf) {
  const render::Frame& frame = sf.frame;
  render::Frame lf;
  u32 tw = 0, th = 0;
  native_extent(render::kLayerW + 2.0f * render::wide_extra(), render::kFieldRows, tw, th);
  const float lw = static_cast<float>(tw), lh = static_cast<float>(th), ls = lh / render::kFieldRows;
  ensure_target(field_rt, field_rt_set, tw, th, nearest);
  const bool wave_lo = sf.wave.on && ensure_target(wave_lo_rt, wave_lo_set, tw, th, nearest);
  ctx.begin_target(field_rt, frame, lf);
  world.view().set_target_pass(true, ls);
  // The deep part only; warped below.
  if (wave_lo) world.set_draw_slab(static_cast<float>(sf.wave.ot), 1e9f);
  draw_field_slab(sf, lf, lw, lh, false);
  ctx.end_target(field_rt);
  render::TexRef shown = field_rt_set;
  if (wave_lo && sf.warp_debug != 2) {
    render::Frame wf;
    ctx.begin_target(wave_lo_rt, frame, wf);
    const render::Letterbox id = render::Letterbox::identity(lw, lh, ls);
    sprites.begin(wf, lw, lh, true);
    draw_warp(sf, field_rt_set, id, id, lw, lh);
    sprites.end();
    world.set_draw_slab(-1e9f, static_cast<float>(sf.wave.ot));
    draw_field_slab(sf, wf, lw, lh, false);
    ctx.end_target(wave_lo_rt);
    shown = wave_lo_set;
  }
  world.set_draw_slab(-1e9f, 1e9f);
  world.view().set_target_pass(false);
  show_lowres(sf, shown, world.view().letterbox(sf.pw, sf.ph), lw, lh);
}

// The window-resolution field picture (the layers went in before the battle picture; here the actors, effects and near layers).
void App::draw_field_full(const SceneFrame& sf) {
  const render::Frame& pf = sf.pf;
  const float pw = sf.pw, ph = sf.ph;
  if (sf.wave_hd && ensure_target(wave_rt, wave_set, pf.extent.width, pf.extent.height, linear, true)) {
    // The deep part into a target of the picture frame's size (same letterbox), warped into the picture, then the near part over it.
    render::Frame wf;
    ctx.begin_target(wave_rt, pf, wf);
    world.set_draw_slab(static_cast<float>(sf.wave.ot), 1e9f);
    draw_field_slab(sf, wf, pw, ph, true);
    ctx.end_target(wave_rt);
    const render::Letterbox lb = world.view().letterbox(pw, ph);
    field_clip(sf, true);
    sprites.begin(pf, pw, ph, true);
    draw_warp(sf, wave_set, lb, lb, pw, ph);
    sprites.end();
    field_clip(sf, false);
    world.set_draw_slab(-1e9f, static_cast<float>(sf.wave.ot));
    draw_field_slab(sf, pf, pw, ph, true);
    world.set_draw_slab(-1e9f, 1e9f);
  } else if (sf.wave_hd) {
    // No target: unwarped.
    draw_field_slab(sf, pf, pw, ph, true);
  } else if (!sf.field_lowres) {
    // Scissored to the field picture itself.
    world.draw_actors(meshes, pf, step.alpha);
    field_clip(sf, true);
    world.draw_effects(pf, pw, ph, step.alpha);
    field_clip(sf, false);
    field_clip(sf, true);
    sprites.begin(pf, pw, ph, true);
    world.draw_background_late(sprites, pw, ph, step.alpha);
    sprites.end();
    field_clip(sf, false);
  }
}

// [Video] Ps1Res: the HUD, menus, digits and text in the 320 (+ 2e) x 240 PS1 picture (any art mode), else at window resolution.
void App::draw_battle_ui(const SceneFrame& sf) {
  if (!(config.ps1_res && ps1_pass_wide(sf.frame, sf.vw, sf.vh, battle.text_batch(),
                                        [&](const render::Frame& lf, float, float, const render::Letterbox& lb) { battle.draw_ui(lf, true, &lb); })))
    battle.draw_ui(sf.frame, true);
}

// The field's status icons, dialogue windows, fades, and the full-screen menus over it.
void App::draw_field_ui(const SceneFrame& sf) {
  const render::Frame& frame = sf.frame;
  const float vw = sf.vw, vh = sf.vh;
  // RDE's world name (HD art): under the windows, fades and menus.
  if (world_name_ok) {
    const bool show = platform::art_is_hd(config.art_mode) && !game::free_cam().hide_overlays() && world.vm().var(0x50) != 0 &&
                      world.vm().var(0x52) != 0;
    import::Lang name_lang = import::Lang::En;
    import::parse_lang(config.language, name_lang);
    world_name.draw(frame, vw, vh, world.view().letterbox(vw, vh).left(), show, ui::WorldName::home_map(world.map_id()), name_lang);
  }
  if (world.overlay_needed()) {
    sprites.begin(frame, vw, vh, true);
    // With Ps1Res the status icons and dialogue windows go into the PS1-resolution target below.
    const bool ps1_windows = config.ps1_res;
    world.draw_overlay(sprites, vw, vh, !ps1_windows);
    sprites.end();
    if (ps1_windows &&
        !ps1_pass_wide(frame, vw, vh, nullptr, [&](const render::Frame& lf, float tw, float th, const render::Letterbox&) {
          sprites.begin(lf, tw, th, true);
          world.draw_windows(sprites, tw, th, true);
          sprites.end();
        })) {
      sprites.begin(frame, vw, vh, true);
      world.draw_windows(sprites, vw, vh);
      sprites.end();
    }
    // The room fade / transition veil / captured frame go over the windows (the non-Ps1 order).
    if (ps1_windows) {
      sprites.begin(frame, vw, vh, true);
      world.draw_overlay_tail(sprites, vw, vh);
      sprites.end();
    }
  }
  if (menu_ok && !menu.closed()) menu.draw(frame, vw, vh, true);
  // Widescreen: the shop is 4:3 (its canvas is Letterbox::fit): the wide field beside it is blacked out, i.e. pillarboxed like the title
  // and the flow screens. The main menu and the party screen cover the widened frame themselves.
  if (wide_extra > 0.0f && shop_open) pillarbox(sprites, frame, render::Letterbox::fit(vw, vh), vw);
  if (shop_open) shop.draw(frame, vw, vh, true);
  if (key_popup_ok && key_popup.active()) key_popup.draw(frame, vw, vh, true);
  if (party_ok && party_screen.active()) {
    party_screen.set_hd(platform::art_is_hd(config.art_mode));
    party_screen.draw(frame, vw, vh, true);
  }
  if (monster_picker_ok && monster_picker.active()) monster_picker.draw(frame, vw, vh);
}

// The title screen over the title room. Widescreen: the New Game / Continue menu is the static 4:3 menu picture; the 3D sea beside
// it is blacked out (pillarbox, like the full-screen menus). The splash / prompt phase keeps the sea across the widened frame.
void App::draw_title_menu(const SceneFrame& sf) {
  const auto tph = flow.title().phase();
  if (wide_extra > 0.0f && (tph == ui::TitleScreen::Phase::Menu || tph == ui::TitleScreen::Phase::FadeOut) && !game::free_cam().hide_overlays())
    pillarbox(sprites, sf.frame, render::Letterbox::fit(sf.vw, sf.vh), sf.vw);
  // The Debug free camera's "Hide overlays" also hides the title logo / menu.
  if (!game::free_cam().hide_overlays()) flow.draw(sf.frame, sf.vw, sf.vh, true);
}

// The full-resolution field passes are clipped to the field picture like the original 320x224 draw area (effect quads and shadows
// otherwise spill into RDE's crop band / the letterbox). Widescreen: the visible columns, cut to the room picture (black beyond).
void App::field_clip(const SceneFrame& sf, bool on) {
  render::Rect scissor = sf.pf.full();
  if (on) {
    float sx = 0, sy = 0;
    world.view().scroll_at(step.alpha, sx, sy);
    scissor = world.view().clip_rect(world.view().letterbox(sf.pw, sf.ph), sx, sf.pw, sf.ph, true);
  }
  sf.pf.set_scissor(scissor);
}

bool App::ensure_target(render::RenderTarget& rt, render::TexRef& set, u32 w, u32 h, const render::Sampler& smp, bool multisampled) {
  if (rt.valid() && rt.width() == w && rt.height() == h) {
    set = rt.ref(smp);
    return true;
  }
  set = {};
  if (w == 0 || h == 0 || !rt.create(ctx, w, h, multisampled)) return false;
  set = rt.ref(smp);
  return rt.valid();
}

// The whole field picture of the current slab: submerged faces, layers + shadows, actors, effects (the order of the normal frame).
void App::draw_field_slab(const SceneFrame& sf, const render::Frame& slab_frame, float fw, float fh, bool clip) {
  world.draw_actors(meshes, slab_frame, step.alpha, render::MeshPipeline::DrawParams::WetFaces::WetOnly);
  if (clip) field_clip(sf, true);
  sprites.begin(slab_frame, fw, fh, true);
  world.draw_background(sprites, fw, fh, step.alpha);
  sprites.end();
  if (clip) field_clip(sf, false);
  world.draw_actors(meshes, slab_frame, step.alpha);
  if (clip) field_clip(sf, true);
  world.draw_effects(slab_frame, fw, fh, step.alpha);
  if (clip) field_clip(sf, false);
  // The layers nearer than every actor blend over the models and effects (FieldWorld::draw_background_late).
  if (clip) field_clip(sf, true);
  sprites.begin(slab_frame, fw, fh, true);
  world.draw_background_late(sprites, fw, fh, step.alpha);
  sprites.end();
  if (clip) field_clip(sf, false);
}

// The warp grid: buffer pixels -> `dst` (the picture in the frame drawn into), source texels -> `src` (the picture in the sampled
// target of sw x sh). Opaque copies at depth 1 (never in front of the near content drawn afterwards).
void App::draw_warp(const SceneFrame& sf, render::TexRef set, const render::Letterbox& dst, const render::Letterbox& src, float sw, float sh) {
  static std::vector<render::WarpQuad> quads;
  // Widescreen: over the extra columns too.
  render::build_screen_warp(sf.wave.warp, quads, static_cast<int>(render::wide_extra()));
  for (const render::WarpQuad& quad : quads) {
    float xy[8], uv[8];
    for (int k = 0; k < 4; ++k) {
      xy[2 * k] = dst.ox + quad.xy[2 * k] * dst.scale;
      xy[2 * k + 1] = dst.oy + quad.xy[2 * k + 1] * dst.scale_y;
      uv[2 * k] = (src.ox + quad.uv[2 * k] * src.scale) / sw;
      uv[2 * k + 1] = (src.oy + quad.uv[2 * k + 1] * src.scale_y) / sh;
    }
    sprites.draw_quad_depth(set, xy, uv, 1.0f);
  }
}

// A PS1-resolution target (classic art + Ps1Res, field or battle) shown in the picture frame `lb` of the scene frame (sf.pf): nearest up (the 1/64-texel bias
// keeps pixel centres off exact texel boundaries: 944 / 224 = 59 / 14 puts some rows on a tie, which the quad's two triangles resolved
// differently: one-pixel seams); area-filtered when the render scale makes the target denser than the window. Over the black clear:
// additive = a plain copy that ignores the target's alpha channel.
void App::show_lowres(const SceneFrame& sf, render::TexRef set, const render::Letterbox& lb, float lw, float lh) {
  // ls: target pixels per buffer pixel; dw: scene frame pixels the target's width covers.
  const float ls = lh / render::kFieldRows;
  const float dw = lw / ls * lb.scale;
  const bool denser = lw > dw + 0.5f;
  sprites.begin(sf.pf, sf.pw, sf.ph, true);
  sprites.set_blend(render::BlendMode::Add);
  if (denser) sprites.set_filter(render::SpriteFilter::Box);
  const float bu = denser ? 0.0f : 1.0f / (64.0f * lw), bv = denser ? 0.0f : 1.0f / (64.0f * lh);
  sprites.draw(set, lb.left(), lb.oy, dw, lh / ls * lb.scale_y, bu, bv, 1.0f + bu, 1.0f + bv);
  if (denser) sprites.set_filter(render::SpriteFilter::Sampler);
  sprites.set_blend(render::BlendMode::Alpha);
  sprites.end();
}

}  // namespace oc::app
