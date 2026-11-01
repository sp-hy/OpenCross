// BattleView: effect grey fade, end of effect, screen draw and developer logs.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "core/log.h"
#include "import/tim.h"
#include "game/battle/battle_view.h"
#include "game/effects/strip_smooth.h"

namespace oc::game {


// FxModel::grey (ki_deth / octdeth op 0x180): the unit's model textures with every 5-bit colour moved toward its grey (the CLUT rows fade on
// the PS1; the textures carry the CLUT colours texel by texel). HD textures get the same per-texel rule on their 8-bit channels.
void BattleView::fx_grey_apply(bool restore) {
  const effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  if (fx_grey_.size() < static_cast<size_t>(battle::kUnits)) fx_grey_.assign(static_cast<size_t>(battle::kUnits), {-1, true});
  for (int unit = 0; unit < battle::kUnits; ++unit) {
    const effects::FxModel* om = !restore && ms && static_cast<size_t>(unit) < ms->units.size() ? &ms->units[static_cast<size_t>(unit)] : nullptr;
    const int grey = om ? std::clamp(om->grey, -1, 0x1000) : -1;
    const bool stp = om ? om->grey_stp : true;
    auto& cur = fx_grey_[static_cast<size_t>(unit)];
    if (cur.first == grey && (grey < 0 || cur.second == stp)) continue;
    cur = {grey, stp};
    const Visual& visual = vis_[static_cast<size_t>(unit)];
    if (!visual.has_model || !visual.model || !visual.model->gpu) continue;
    const assets::ModelAsset& asset = visual.model->asset;
    render::MeshModel& gpu = *visual.model->gpu;
    auto fade = [&](const import::Image& img, int bits) {
      std::vector<u8> px = img.pixels;
      if (grey >= 0)
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
          // CLUT entry 0x0000: transparent either way.
          if (px[i + 3] == 0) continue;
          const int sh = 8 - bits, inv = 0x1000 - grey;
          int channel[3] = {px[i] >> sh, px[i + 1] >> sh, px[i + 2] >> sh};
          const int mean = ((channel[0] + channel[1] + channel[2]) / 3) * grey;
          for (int ch = 0; ch < 3; ++ch) channel[ch] = (mean + channel[ch] * inv) >> 12;
          for (int ch = 0; ch < 3; ++ch) px[i + static_cast<size_t>(ch)] = static_cast<u8>(bits == 5 ? (channel[ch] << 3) | (channel[ch] >> 2) : channel[ch]);
          // STP: | 0x8000.
          if (stp) px[i + 3] = import::kStpAlpha;
          else px[i + 3] = (channel[0] | channel[1] | channel[2]) ? 255 : 0;
        }
      return render::bleed_transparent(px, img.width, img.height);
    };
    // The drawn images (a texture mod's replacement included; its 8-bit channels take the HD rule).
    for (size_t i = 0; i < asset.textures.size() && i < gpu.textures.size(); ++i) {
      const import::Image& img = asset.drawn_texture(i, false);
      if (gpu.textures[i] && !img.indexed && img.pixels.size() >= static_cast<size_t>(img.width) * img.height * 4u)
        gpu.textures[i]->update(*ctx_, fade(img, asset.has_mod(i) ? 8 : 5).data());
      if (i < gpu.hd_textures.size() && gpu.hd_textures[i] && asset.has_hd(i)) gpu.hd_textures[i]->update(*ctx_, fade(asset.hd_textures[i], 8).data());
    }
    // The eye-blink variants (textures eye_base + 0 / 1 = texture 0 with the shut-eye frames pasted in) fade with the rest of the model.
    if (gpu.eye_base >= 0 && !asset.textures.empty() && !asset.textures[0].indexed)
      for (int frame = 1; frame <= 2; ++frame) {
        const size_t ti = static_cast<size_t>(gpu.eye_base + frame - 1);
        if (ti >= gpu.textures.size() || !gpu.textures[ti]) continue;
        import::Image sd_image = asset.drawn_texture(0, false);
        sd_image.pixels = asset.drawn_eye_frame(frame, false);
        if (sd_image.pixels.empty()) continue;
        gpu.textures[ti]->update(*ctx_, fade(sd_image, asset.has_mod(0) ? 8 : 5).data());
        if (ti < gpu.hd_textures.size() && gpu.hd_textures[ti] && asset.has_hd(0)) {
          import::Image hd_image = asset.hd_textures[0];
          hd_image.pixels = asset.drawn_eye_frame(frame, true);
          if (!hd_image.pixels.empty()) gpu.hd_textures[ti]->update(*ctx_, fade(hd_image, 8).data());
        }
      }
  }
}

// OC_FXDBG: the global effect state a pack leaves behind (fog words / flag, the two screen colours, the warp).
void BattleView::fx_state_log(const char* when) const {
  if (static const bool dbg = std::getenv("OC_FXDBG") != nullptr; !dbg) return;
  log::debug("fxdbg: {} tick {}: fog on {} cur rgb {:.0f} {:.0f} {:.0f} near {:.0f} far {:.0f} left {} | screen0 {:.0f} {:.0f} {:.0f} m{} | screen1 {:.0f} {:.0f} {:.0f} m{} | warp {}",
             when, tick_count_, fx_fog_on_, fog_cur_.r, fog_cur_.g, fog_cur_.b, fog_cur_.near, fog_cur_.far, fog_left_, fx_screen_[0].cur[0], fx_screen_[0].cur[1],
             fx_screen_[0].cur[2], fx_screen_[0].mode, fx_screen_[1].cur[0], fx_screen_[1].cur[1], fx_screen_[1].cur[2], fx_screen_[1].mode, fx_warp_.on);
}

void BattleView::fx_end() {
  if (!fx_) return;
  pa_.on = false;
  pa_.fm = nullptr;
  pa_.pose = nullptr;
  pa_.handle.reset();
  fx_state_log("pack end");
  fx_grey_apply(true);
  fx_cross_.on = fx_cross_.next_on = false;
  // The screen copy warp (the zoom-blur double image) is a global of the effect system: the pack end does not clear it, the next
  // effect start does, and so does the battle's idle camera handler while no shot / timeline runs. A pack
  // that ends (or is cut short) with the warp still on would leave every later frame doubled until some next pack: off at the pack end, unless
  // a stage / battle-script timeline is running that may own it.
  if (fx_warp_.on && !camera_.sequence_running()) {
    if (std::getenv("OC_FXDBG")) log::debug("fxdbg: pack end at tick {} with the screen warp still on: cleared", tick_count_);
    fx_warp_.on = false;
  }
  // The slow motion is cleared when the next pack starts; a pack cut here keeps none.
  fx_slow_n_ = fx_slow_x_ = fx_slow_phase_ = 0;
  int target_index = 0;
  for (size_t i = 0; i < act_.ev.size() && i < fx_fired_.size(); ++i) {
    if (fx_fired_[i]) continue;
    fx_fired_[i] = true;
    if (act_.ev[i].target < 0) continue;
    pending_.push_back({act_.ev[i].target, 1 + 6 * target_index, act_.ev[i]});
    ++target_index;
  }
  // The pack's sprites go away with it (the lights keep decaying from the tracker's own state).
  spell_.clear_colour_caches();
  // A monster the pack faded to colour 0 and hid (kamedes 0x100: c = 0x80 - (t << 7) / D) keeps that colour in its model block: the KO
  // fade (ColorRamp from the CURRENT colour 0, then ModelShow 0) shows nothing, so it must not come back at full colour and fade a second
  // time when the overlay's copy of the block goes away.
  // The same holds for a DEAD monster the overlay left hidden at any colour. The F-B dissolves draw the model themselves and keep the hide
  // bit set every frame of their task (darkfeit; the CLUT fade 0x102 leaves the model colour at 128); nothing clears the bit at the pack
  // end, so FATE (scene 462, death element 518 feit.prd) stays hidden through to the victory instead of being drawn again in its bind pose
  // and then falling with the KO fade.
  if (const effects::FxModels* ms = fx_->bespoke_models())
    for (const effects::FxModel& fx_model : ms->units)
      if (fx_model.unit >= 0 && fx_model.unit < battle::kUnits && fx_model.hidden && !fx_model.unhide_at_end && !battle_->unit(fx_model.unit).pc &&
          ((fx_model.tinted && fx_model.rgb[0] == 0 && fx_model.rgb[1] == 0 && fx_model.rgb[2] == 0) || battle_->unit(fx_model.unit).hp <= 0)) {
        Visual& visual = vis_[static_cast<size_t>(fx_model.unit)];
        visual.fade = visual.fade_prev = 0.0f;
      }
  fx_.reset();
  // The stage flare returns 3 ticks after the end hook.
  fx_lens_wait_ = 3;
  // The shadow flag := the entry's copy (op 0x74's clear undone; an S5 0x22 from before the start stays).
  fx_hook_end();
  // The pack end walks EVERY unit, not just the ones a script ran on: the shadow flag (above), the model copy freed, the idle script restarted.
  // Our scripts can be cut, so their leftover model state (blend, part hides, hidden, trail samplers) goes with it - a unit that kept a
  // second blended copy of itself would show the "doubling" after a spell.
  // The shading entries' hide bits (ops 0x1B / 0x1C) are plain model words the pack end never touches, so a unit whose script ran
  // out or that only runs its idle loop (nothing was cut) keeps them: the Bulbs' S5 entry 8 hides parts 0-3 and each sprout stage (entry 13 ...)
  // shows one more; clearing them on every pack end would draw the whole Bulb under the floor after the next cast.
  for (int i = 0; i < battle::kUnits; ++i) {
    const Visual& visual = vis_[static_cast<size_t>(i)];
    // A running spawn script is not cut: the idle restart waits for its end (start_idle).
    if (visual.spawn && visual.vm.running()) continue;
    // A reaction that took the killing hit is not cut either: a pack waits for its scripts, so the fade at its tail always plays out.
    if (visual.ko_reacted && visual.vm.running()) continue;
    const bool ran_out = !visual.vm.running();
    // A script that ran out keeps its model colour.
    anm_unit_reset(i, ran_out, ran_out || visual.script_kind == kIdle);
    // No KO on the unit, so the idle restart runs: HP a script wrote to 0 shows as the low-HP pose from here on.
    if (visual.zero_hp_actions == 2 && visual.script_kind == kIdle) start_idle(i);
  }
  // The tech clips are dropped; nothing plays an external clip afterwards.
  fx_drop_clip_packs();
  // The ground overlay is freed.
  stage_scene_.ground_overlay_end();
  fx_show_living();
  fx_lit_.fill(false);
  fx_shake_x_ = fx_shake_y_ = 0;
  if (fx_prev_bank_) camera_.set_bank(fx_prev_bank_);
  // The pack closes: its tags leave the list and its sounds are cut.
  audio_.end_pack();
  // The theme returns to 0x7F over 0x60 frames.
  audio_.restore_cast();
  if (fx_faded_) camera_.start_fade(0.0f, 20);
  fx_faded_ = false;
}

// The two screen colour layers (ops 0x2B / 0x2C): a full-screen flat quad (0,0)-(0x160,0x110) while the colour is not black,
// in effect-OT slots 5 / 6, i.e. over the scene and the effects. Mode 0 opaque, 1..4 = the GPU blends of the emitters (ABR mode - 1).
void BattleView::fx_draw_screen(const render::Frame& frame, const render::Letterbox& lb) {
  if (!white_.set) return;
  bool any = !anm_screen_.empty();
  for (const ScreenColour& sc : fx_screen_) any = any || sc.cur[0] > 0 || sc.cur[1] > 0 || sc.cur[2] > 0;
  if (!any) return;
  particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), fx_particle_pass_);
  fx_particle_pass_ = true;
  // Slot 6 (layer 1) first, slot 5 on top; the op 0x52 quads under both.
  for (int layer = static_cast<int>(anm_screen_.size()) + 1; layer >= 0; --layer) {
    const ScreenColour& sc = layer < 2 ? fx_screen_[static_cast<size_t>(layer)] : anm_screen_[static_cast<size_t>(layer - 2)];
    if (sc.cur[0] <= 0 && sc.cur[1] <= 0 && sc.cur[2] <= 0) continue;
    float qx[4] = {0, 352, 0, 352};
    lb.widen_full_width(qx, 4);
    const float qy[4] = {0, 0, 272, 272};
    float x[4], y[4];
    for (int k = 0; k < 4; ++k) {
      x[k] = lb.ox + qx[k] * lb.scale;
      y[k] = lb.oy + qy[k] * lb.scale_y;
    }
    const float u[4] = {0, 1, 0, 1}, v[4] = {0, 0, 1, 1};
    const int blend = std::clamp(sc.mode, 0, 4);
    // The particle shader modulates the white texel by colour / 0x80: the flat colour c is c * 2 there (saturating at 255 like the GPU's c).
    auto ch = [](float value) { return static_cast<u8>(std::clamp(value * 0.5f, 0.0f, 255.0f)); };
    particles_.quad(white_.set, blend, x, y, u, v, ch(sc.cur[0]), ch(sc.cur[1]), ch(sc.cur[2]), static_cast<float>(5 + layer) / render::kDepthRange);
  }
  particles_.end();
}

// Dev logs (OC_FX_OBJECTS / OC_FX_UNITS): shared by the pack path of fx_draw and the idle path (a monster's own anmseq sparks run with no pack).
void BattleView::fx_dev_logs() {
  // OC_FX_OBJECTS=N: every N ticks one line per live object: emitter / sprite, position (world units), velocity (op-0A units), scale, angles,
  // colour accumulators, draw colour, life, bias, flags.
  static const int objects_every = std::getenv("OC_FX_OBJECTS") ? std::max(1, std::atoi(std::getenv("OC_FX_OBJECTS"))) : 0;
  // The dev logs count VIDEO frames (frame_count_): a pack's slow motion swallows game ticks (fx_slow_skip) and
  // tick_count_ then runs behind (Dash & Slash: 60 sequence ticks last 110 frames).
  const bool dev_new_frame = dev_logged_frame_ != frame_count_;
  dev_logged_frame_ = frame_count_;
  if (objects_every && dev_new_frame && frame_count_ % objects_every == 0) {
    // The anmseq banks (op 0x1D hit sparks / trails started by the unit scripts, battle_view_anm.cpp) are objects of the same task table.
    size_t total = fx_ ? fx_->emitters().live_objects() : 0;
    anm_each_system([&](const effects::EmitterSystem& sys) { total += sys.live_objects(); });
    log::debug("fx objects tick {} fxtick {}: {} objects", frame_count_, fx_ ? fx_->ticks() : 0, total);
    // Names are the registry's u32 tag with every non-printable byte shown as '.': a name carrying the 0x7F byte of `hi2<7f>` prints as `hi2.`.
    auto tagged = [](const std::string* name) {
      if (!name) return std::string("?");
      std::string text = *name;
      for (char& ch : text)
        if (ch < 32 || ch > 126) ch = '.';
      return text;
    };
    auto dump = [&](const effects::EmitterSystem& sys) {
    for (size_t i = 0; i < sys.live_objects(); ++i) {
      effects::EmitterSystem::ObjectView object;
      if (!sys.object_view(i, object)) continue;
      log::debug("  em {} spr {} t{} pos {:6.0f} {:6.0f} {:6.0f} vel {:7.0f} {:7.0f} {:7.0f} sc {:5d} {:5d} {:5d} ang {:5d} {:5d} {:5d} col {:4d} {:4d} {:4d} rgb {:3d} {:3d} {:3d} life {:4d} bias {:4d} fr {:2d} fl {:08x}",
                 tagged(object.emitter), tagged(object.sprite), object.stype, object.pos[0], object.pos[1], object.pos[2], object.vel[0] * 1024.0f, object.vel[1] * 1024.0f,
                 object.vel[2] * 1024.0f, object.scale[0], object.scale[1], object.scale[2], object.angle[0], object.angle[1], object.angle[2], object.color[0], object.color[1], object.color[2], object.rgb[0],
                 object.rgb[1], object.rgb[2], object.life, object.bias, object.frame, object.flags);
      // The model block's motion fields: clip, position in 1/256 keyframe, loop.
      if (object.stype == 20)
        log::debug("    model clip {} motion {:.2f} ({} /256) loop {}", object.clip, object.motion, static_cast<int>(object.motion * 256.0f), object.loop);
    }
    };
    if (fx_) dump(fx_->emitters());
    anm_each_system(dump);
  }
  // OC_FX_UNITS=N (dev): every N ticks one header, one line per present unit (the model flag bits we model, colour, position in PS1 world
  // units (y down), yaw as PS1 12 bit, clip + motion position, scale) and one `fx camera` line (eye / target / H in PS1 axes: y down, so
  // ours is negated; the fog pair + colour, the warp globals). `-` = no equivalent on our side.
  static const int units_every = std::getenv("OC_FX_UNITS") ? std::max(1, std::atoi(std::getenv("OC_FX_UNITS"))) : 0;
  // A per-frame stamp for the `fxseq:` lines (their t is task-local).
  if (units_every && dev_new_frame) log::debug("fx tickmark {} {}", frame_count_, fx_ ? fx_->ticks() : 0);
  if (units_every && dev_new_frame && frame_count_ % units_every == 0) {
    log::debug("fx units tick {} fxtick {}", frame_count_, fx_ ? fx_->ticks() : 0);
    for (int unit = 0; unit < battle::kUnits; ++unit) {
      if (!battle_->unit(unit).present) continue;
      const Visual& visual = vis_[static_cast<size_t>(unit)];
      const size_t ui = static_cast<size_t>(unit);
      // Model flags: 0x1 / 0x100 hide bits (fx_hide_bits_), 0x100 lit, 0x200 = shadow ON (set on every unit at rest; ops 0x74 clears it, 0x75 sets it); ghost is the separate column.
      // The state the unit is DRAWN with (battle_view.cpp unit loop), not only the stock-op bits: the model block of an overlay (hidden, flag bits,
      // colour, steered position), the anmseq script state (hide, semi-transparency, colour) and the enemy KO fade (colour -> 0, then hidden).
      const effects::FxModels* lm = fx_ ? fx_->bespoke_models() : nullptr;
      const effects::FxModel* om = lm && ui < lm->units.size() ? &lm->units[ui] : nullptr;
      bool semi = false, blink = false;
      float arg[3];
      anm_unit_probe(unit, semi, arg, blink);
      unsigned fl = fx_hide_bits_[ui] | (fx_lit_[ui] ? 0x100u : 0u) | (shadow_off_[ui] ? 0u : 0x200u);
      if (om) fl = (fl & ~0x200u) | (om->flags & ~0x200u) | ((om->flags & 0x200u) && !shadow_off_[ui] ? 0x200u : 0u);
      const float kofade = battle_->unit(unit).pc ? 1.0f : visual.fade;
      if (fx_hidden_[ui] || (om && om->hidden) || anm_hidden(unit) || kofade <= 0.0f) fl |= 0x1;
      if (semi || (kofade < 1.0f && kofade > 0.0f)) fl |= 0x2;
      // Eye blink armed (ops 0x72 / 0x4F).
      if (blink) fl |= 0x400;
      // SetAnim flags 0x2000 / 0x4000.
      if (visual.has_model) fl |= static_cast<unsigned>(visual.pose.blend_mode() << 13);
      // SetAnim flag 0x8000.
      if (visual.has_model && visual.pose.run_source()) fl |= 0x8000;
      float cl[3];
      for (int k = 0; k < 3; ++k)
        cl[k] = kofade * (fx_unit_[ui].tinted ? fx_unit_[ui].rgb[k] : 128.0f) * (om && om->tinted ? om->rgb[k] / 128.0f : 1.0f) * (arg[k] / 128.0f);
      // PS1 axes (y down).
      std::array<float, 3> pp{visual.x, -visual.y, visual.z};
      if (om && om->steer) pp = {om->pos[0], om->pos[1], om->pos[2]};
      // The posed reach of attach point 15 (GetApproachPos 0x7C reads it).
      float r15 = 0, r15y = 0, r15z = 0, r15x = 0;
      if (visual.has_model && visual.pose.attach_now(15, 1.0f, r15x, r15y, r15z)) r15 = std::hypot(r15x, r15z);
      const bool gh = ghosted(unit) && !entry_shown_[ui];
      int yaw = static_cast<int>(std::lround(std::atan2(-visual.face_x, -visual.face_z) * 4096.0 / 6.283185307179586)) & 0xFFF;
      // An overlay that steers the model owns its rotation too (Gravity Blow's spin).
      if (om && om->steer) yaw = om->rot[1] & 0xFFF;
      // The hidden shading entries as drawn (per part: model data | ops 0x1C / 0x91 hides, minus 0x1B / 0x91 shows).
      unsigned parts = 0;
      if (visual.has_model && visual.model)
        for (size_t k = 0; k < visual.model->asset.shading.size() && k < 32; ++k)
          if (visual.model->asset.shading[k].flags & 1) parts |= 1u << k;
      parts = anm_part_hide(unit, parts);
      // Joints whose op-0x8F scale word is not 4096.
      std::string jsc;
      for (size_t j = 0; visual.has_model && j < visual.pose.joint_override_count(); ++j)
        if (visual.pose.joint_scale(j) != 1.0f) jsc += std::format(" j{}:{:.0f}", j, visual.pose.joint_scale(j) * 4096.0f);
      log::debug("  unit {:2d} flags7e {:04x} ghost {} col {:3.0f} {:3.0f} {:3.0f} pos {:6.0f} {:6.0f} {:6.0f} yaw {:4d} clip {} motion {:.2f} ({}/256) scale - - - r15 {:.0f} parthide {:x}{}{}",
                 unit, fl & 0xFFFF, gh ? 1 : 0, cl[0], cl[1], cl[2], pp[0], pp[1], pp[2], yaw, visual.pose.current_clip(),
                 visual.pose.frame_cur(), static_cast<int>(visual.pose.frame_cur() * 256.0f), r15, parts, jsc.empty() ? "" : " jscale", jsc);
    }
    const CamPose cp = camera_.pose(1.0f);
    log::debug("fx camera tick {} eye {:.0f} {:.0f} {:.0f} tgt {:.0f} {:.0f} {:.0f} H {:.0f} fog {} near {:.0f} far {:.0f} rgb {:.0f} {:.0f} {:.0f} warp on {} ang {:.0f} {:.0f} {:.0f} trans {:.0f} {:.0f} {:.0f} col {:.0f} {:.0f} {:.0f} off {:.0f} {:.0f} abr {} slow {}/{}",
               frame_count_, cp.eye[0], -cp.eye[1], cp.eye[2], cp.target[0], -cp.target[1], cp.target[2], cp.h, fx_fog_on_ ? 1 : 0, fog_raw_.near, fog_raw_.far,
               fog_raw_.r, fog_raw_.g, fog_raw_.b, fx_warp_.on ? 1 : 0, fx_warp_.ang[0], fx_warp_.ang[1], fx_warp_.ang[2], fx_warp_.tr[0], fx_warp_.tr[1],
               fx_warp_.tr[2], fx_warp_.rgb[0], fx_warp_.rgb[1], fx_warp_.rgb[2], fx_warp_.off[0], fx_warp_.off[1], fx_warp_.abr, fx_slow_x_, fx_slow_n_);
  }
}

void BattleView::fx_draw(const render::Frame& frame, const render::Letterbox& lb) {
  if (!fx_) {
    fx_draw_screen(frame, lb);
    fx_dev_logs();
    return;
  }
  const effects::ViewParams view = fx_view();
  if (effects::FxModels* ms = fx_->bespoke_models(); ms && !ms->skin) {
    ms->skin = [this](int unit, effects::FxSkinnedModel& out) { return fx_skin_unit(unit, out); };
    ms->attach = [this](int unit, int point, effects::Vec3& out) { return fx_attach_unit(unit, point, out); };
    ms->pseudo_attach = [this](int point, effects::Vec3& out) { return pa_attach(point, out); };
    ms->skin_pack = [this](effects::FxSkinnedModel& out) { return fx_skin_pack(out); };
    // FxModels::PackDrive requests are consumed here (fx_pack_drive_tick).
    ms->pack_drive.host = true;
  }
  if (effects::FxModels* ms = fx_->bespoke_models()) {
    ms->depth_cue.on = fx_fog_on_;
    ms->depth_cue.near_z = fog_cur_.near, ms->depth_cue.far_z = fog_cur_.far;
    ms->depth_cue.rgb[0] = fog_cur_.r, ms->depth_cue.rgb[1] = fog_cur_.g, ms->depth_cue.rgb[2] = fog_cur_.b;
  }
  fx_->collect(view, alpha_, fx_frame_);
  FxGpu* gpu = fx_gpu_for(fx_pack_);
  // Effect meshes (sprite types 14 / 19): every live mesh object goes through the stage pipeline like the stage's own objects (object
  // colour x Gouraud corners, blend word, OT bias / fixed slot, ext cull sides, op-36 texture-window scroll, op-37 morph). The pack's
  // meshes first, then the resident battle.prd ones.
  struct Tri {
    render::TexRef set;
    float x[4], y[4], u[4], v[4];
    u8 r, g, b;
    int blend, slot;
    u32 order;
    // Per-corner colours (the bespoke overlays' GT3 / GT4 packets).
    bool gouraud;
    u8 colours[4][3];
  };
  std::vector<Tri> tris;
  // Semi-transparent mesh objects (instance blend != 0) take their OT slot in the painter's sort below with the sprite quads and the bespoke
  // packets: a PS1 mesh object is filed in the one ordering table at (z >> 4) + bias, so darkfeit's subtractive 'kie1' / 'ki12' puffs
  // (bias -65) lie on top of FATE's dissolving faces. Opaque mesh objects stay in the depth-written pre-pass.
  struct SemiMesh {
    render::StageInstance in;
    bool own;
  };
  std::vector<SemiMesh> semi_meshes;
  std::vector<effects::FxPrim> env_prims;
  FxGpu* rg = fx_gpu_resident(fx_pack_);
  const assets::StageAsset* own_asset = fx_pack_->meshes.get();
  const assets::StageAsset* res_asset = fx_pack_->resident ? fx_pack_->resident->meshes.get() : nullptr;
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float sx0 = std::clamp(std::round(lb.left()), 0.0f, fw), sy0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect mesh_scissor{static_cast<int32_t>(sx0), static_cast<int32_t>(sy0),
                                  static_cast<u32>(std::min(fw - sx0, std::round(lb.width()))), static_cast<u32>(std::min(fh - sy0, std::round(kBufferRows * lb.scale_y)))};
  const render::Mat4 mesh_proj = render::StagePipeline::projection(lb, view.h, view.ox, view.oy);
  const bool mesh_linear = linear_filter_ < 0 ? hd_ : linear_filter_ != 0;
  render::StagePipeline::DepthCue mesh_cue;
  mesh_cue.active = fx_fog_on_;
  mesh_cue.r = fog_cur_.r, mesh_cue.g = fog_cur_.g, mesh_cue.b = fog_cur_.b, mesh_cue.near = fog_cur_.near, mesh_cue.far = fog_cur_.far;
  // An overlay's frame copy (miki): the mesh faces on direct-colour pages inside the copy's 320 x 224 VRAM rectangle sample the picture as
  // drawn up to the request's ordering-table slot (everything filed deeper). The copy is made in the painter pass below, before its first
  // item at or above that slot, so those faces go through the painter's sort whatever their blend.
  const effects::FxModels* copy_models = fx_->bespoke_models();
  const bool copy_on = copy_models && copy_models->capture.x >= 0 && fx_copy_ensure(frame, lb);
  const int copy_origin = copy_on ? copy_models->capture.x | copy_models->capture.y << 16 : -1;
  const int copy_slot = copy_on ? copy_models->capture.ot : 0;
  bool copy_pending = false;
  {
    std::vector<effects::EmitterSystem::MeshInstance> inst;
    fx_->collect_meshes(view, alpha_, inst);
    std::vector<render::StageInstance> own, res;
    const bool env_page = static_cast<bool>(stock_env_page_set());
    // Mesh sprites naming a PC's weapon ("02" = Guile's wand).
    std::vector<std::pair<int, const effects::EmitterSystem::MeshInstance*>> weapon_inst;
    for (const auto& mi : inst) {
      if (!mi.name || (mi.sprite_type != 14 && mi.sprite_type != 19)) continue;
      FxGpu* gg = nullptr;
      const assets::StageAsset* asset = nullptr;
      std::unordered_map<std::string, int>::const_iterator pit;
      if (gpu && gpu->stage && (pit = gpu->parts.find(*mi.name)) != gpu->parts.end()) gg = gpu, asset = own_asset;
      else if (rg && rg->stage && (pit = rg->parts.find(*mi.name)) != rg->parts.end()) gg = rg, asset = res_asset;
      // Dev: OC_FX_MESH_TRACE logs one line per mesh instance per frame.
      static const bool mesh_trace = std::getenv("OC_FX_MESH_TRACE") != nullptr;
      if (mesh_trace)
        log::debug("fx mesh '{}' {} centre {:.0f} {:.0f} {:.0f} diag {:.2f} {:.2f} {:.2f} rgb {} {} {} blend {} ext {} type {}", *mi.name,
                   gg == gpu ? "own" : (gg ? "res" : "NONE"), mi.centre[0], mi.centre[1], mi.centre[2], mi.mat.m[0], mi.mat.m[4], mi.mat.m[8],
                   mi.rgb[0], mi.rgb[1], mi.rgb[2], mi.blend, mi.ext_path, mi.sprite_type);
      if (!gg) {
        if (const int wu = mi.sprite_type == 14 ? fx_weapon_mesh_unit(*mi.name) : -1; wu >= 0) weapon_inst.emplace_back(wu, &mi);
        continue;
      }
      if (asset && static_cast<size_t>(pit->second) < asset->parts.size() && mi.off_screen(asset->parts[static_cast<size_t>(pit->second)].cull_points, view))
        continue;
      // Ext bit 2: the mesh mirrors the stage. A stage without the picture keeps the packets' own UVs.
      if (env_page && gg == gpu && mi.sprite_type == 14 && mi.ext_path && (mi.ext & 4)) {
        fx_->env_meshes().emit(mi, mi.morph_t_prev + (mi.morph_t - mi.morph_t_prev) * alpha_, view, env_prims);
        continue;
      }
      render::StageInstance in;
      in.part = pit->second;
      for (size_t k = 0; k < 9; ++k) in.cam[k] = mi.mat.m[k];
      for (size_t k = 0; k < 3; ++k) in.cam[9 + k] = mi.centre[k];
      for (size_t k = 0; k < 3; ++k) in.rgb[k] = mi.rgb[k];
      in.blend = mi.blend;
      in.ext = mi.ext_path;
      in.ext_morph = mi.morph_name != nullptr;
      in.bias = mi.bias;
      in.fixed_slot = mi.fixed_slot;
      in.depth_cue = (mi.flags >> 17) & 1;
      if (mi.morph_name)
        if (const auto mt = gg->parts.find(*mi.morph_name); mt != gg->parts.end()) {
          in.morph_part = mt->second;
          in.morph_t = mi.morph_t_prev + (mi.morph_t - mi.morph_t_prev) * alpha_;
        }
      // Ext bit 5: both sides, bit 4: the reverse side.
      if (mi.ext & 0x20) in.cull = 2;
      else if (mi.ext & 0x10) in.cull = 1;
      // Dev: OC_FX_MESH_CULL forces a cull mode.
      static const int dev_cull = [] { const char* env = std::getenv("OC_FX_MESH_CULL"); return env ? std::atoi(env) : -1; }();
      if (dev_cull >= 0) in.cull = dev_cull;
      // Op 36: the scroll phase inside the texture window.
      if (mi.ext & 1) {
        in.uv_scroll = true;
        in.uv_phase = mi.uv_phase;
        if (mi.owner >= 0 && static_cast<size_t>(mi.owner) < fx_->emitters().header_count())
          for (const auto& op : fx_->emitters().header_def(static_cast<size_t>(mi.owner)).ops)
            if (op.op == 0x36)
              if (auto* scroll = std::get_if<assets::FxUvScroll>(&op.arg)) in.uv_window = {scroll->off_u, scroll->off_v, scroll->win_w, scroll->win_h};
      }
      if (copy_on && asset && static_cast<size_t>(in.part) < asset->parts.size()) {
        const auto& subs = asset->parts[static_cast<size_t>(in.part)].submeshes;
        in.screen = fx_copy_set_;
        in.screen_origin = copy_origin;
        in.screen_uv = {lb.scale / fw, lb.scale_y / fh, lb.ox / fw, lb.oy / fh};
        int dx = 0, dy = 0;
        if (std::any_of(subs.begin(), subs.end(), [&](const assets::StageSubMesh& sub) { return in.screen_offset(sub.direct_page, dx, dy); })) {
          copy_pending = true;
          semi_meshes.push_back({in, asset == own_asset});
          continue;
        }
        in.screen = {};
      }
      if (in.blend != 0) semi_meshes.push_back({in, asset == own_asset});
      else (asset == own_asset ? own : res).push_back(in);
    }
    if (!own.empty() || !res.empty()) {
      stage_pipe_.begin(frame, mesh_scissor);
      // The PS1 packet rejection rules, as for the stage objects.
      stage_pipe_.set_gte(view.h, view.ox, view.oy);
      stage_pipe_.set_depth_cue(mesh_cue);
      if (!own.empty()) stage_pipe_.draw(*gpu->stage, mesh_proj, own, mesh_linear, own_asset);
      if (!res.empty()) stage_pipe_.draw(*rg->stage, mesh_proj, res, mesh_linear, res_asset);
      stage_pipe_.end();
      frame.set_scissor(mesh_scissor);
    }
    if (!weapon_inst.empty()) fx_draw_weapon_meshes(frame, lb, weapon_inst);
    fx_mesh_instances_ = own.size() + res.size() + semi_meshes.size() + weapon_inst.size();
    if (gpu) fx_draw_models(frame, lb, *gpu, inst);
    pa_draw(frame, lb);
  }
  static const bool trace = std::getenv("OC_FX_TRACE") != nullptr;
  if (trace && tick_count_ % 10 == 0) {
    const CamPose cp = camera_.pose(1.0f);
    log::debug("fx trace camera '{}' eye {:.0f} {:.0f} {:.0f} target {:.0f} {:.0f} {:.0f} h {:.0f}", camera_.shot_name(), cp.eye[0], cp.eye[1], cp.eye[2], cp.target[0],
               cp.target[1], cp.target[2], cp.h);
  }
  if (trace && tick_count_ % 10 == 0)
    log::debug("fx trace tick {} fxtick {}: quads {} meshes {} tris {} flats {} live {} seq {}", tick_count_, fx_->ticks(), fx_frame_.quads.size(),
               fx_mesh_instances_, tris.size(), fx_frame_.flats.size(), fx_->emitters().live_objects(), fx_->sequences_active());
  fx_dev_logs();
  if (trace && tick_count_ % 10 == 0 && !tris.empty()) {
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (const Tri& tri : tris)
      for (int corner = 0; corner < 3; ++corner) {
        lo[0] = std::min(lo[0], tri.x[corner]);
        hi[0] = std::max(hi[0], tri.x[corner]);
        lo[1] = std::min(lo[1], tri.y[corner]);
        hi[1] = std::max(hi[1], tri.y[corner]);
      }
    log::debug("fx trace tri bounds x {:.0f}..{:.0f} y {:.0f}..{:.0f} first slot {} blend {} rgb {} {} {} set {}", lo[0], hi[0], lo[1], hi[1], tris[0].slot, tris[0].blend,
               tris[0].r, tris[0].g, tris[0].b, static_cast<bool>(tris[0].set));
  }
  // Bespoke overlays' own textured packets (pack mesh textures, the units' model textures, VRAM pages): same
  // painter's sort as the mesh sprites, Gouraud corners.
  {
    // fx_frame_.textured is already in DRAW order (effects::sort_prims: farthest slot first, the later-submitted packet first inside a slot),
    // while the painter's sort below draws the higher `order` first inside a slot: the i-th prim gets top - i, so a pair filed in one OT
    // entry keeps the PS1 order (darkfeit links the ABR 1 face, then its ABR 2 white-CLUT copy, so the copy (B - c) is drawn first and the
    // face (+ texel x c) on it; the other way round every partial face of FATE's dissolve would end black).
    const u32 tex_base = tris.empty() ? (fx_frame_.quads.empty() ? 0 : fx_frame_.quads.back().order + 1) : tris.back().order + 1;
    std::vector<effects::FxPrim> textured_prims = fx_frame_.textured;
    if (!env_prims.empty()) {
      textured_prims.insert(textured_prims.end(), env_prims.begin(), env_prims.end());
      effects::sort_prims(textured_prims);
    }
    const u32 tex_top = tex_base + static_cast<u32>(textured_prims.size());
    u32 tex_i = 0;
    for (const effects::FxPrim& prim : textured_prims) {
      render::TexRef set;
      if (prim.src == effects::FxPrim::TexSrc::Model) {
        set = fx_unit_texture_set(prim.unit, prim.tex, prim.white);
      } else if (prim.src == effects::FxPrim::TexSrc::Vram) {
        set = fx_vram_page_set(prim.tpage, prim.clut);
      } else if (prim.src == effects::FxPrim::TexSrc::Env) {
        set = stock_env_page_set();
      } else if (prim.src == effects::FxPrim::TexSrc::Capture) {
        if (!copy_on) continue;
        set = fx_copy_set_;
        copy_pending = true;
      } else if (prim.src == effects::FxPrim::TexSrc::PackModel) {
        set = fx_pack_model_texture_set(prim.tex);
      } else if (gpu && prim.tex >= 0 && static_cast<size_t>(prim.tex) < gpu->sets.size()) {
        set = gpu->sets[static_cast<size_t>(prim.tex)];
      }
      if (!set) continue;
      Tri tri{};
      tri.set = set;
      tri.gouraud = true;
      const int corner_count = prim.kind == effects::FxPrim::Kind::Tri ? 3 : 4;
      int cr = 0, cg = 0, cb = 0;
      for (int corner = 0; corner < 4; ++corner) {
        const int k = corner < corner_count ? corner : 2;
        tri.x[corner] = lb.ox + prim.v[k].x * lb.scale;
        tri.y[corner] = lb.oy + prim.v[k].y * lb.scale_y;
        const bool page_texels = prim.src == effects::FxPrim::TexSrc::Vram || prim.src == effects::FxPrim::TexSrc::Env;
        tri.u[corner] = page_texels ? prim.u[k] / 256.0f : prim.u[k];
        // Model / PackModel: v as is.
        tri.v[corner] = prim.src == effects::FxPrim::TexSrc::Pack ? 1.0f - prim.w[k] : page_texels ? prim.w[k] / 256.0f : prim.w[k];
        // Capture: layer pixels of the picture, which the copy holds over the whole frame.
        if (prim.src == effects::FxPrim::TexSrc::Capture) {
          tri.u[corner] = (lb.ox + prim.u[k] * lb.scale) / fw;
          tri.v[corner] = (lb.oy + prim.w[k] * lb.scale_y) / fh;
        }
        tri.colours[corner][0] = prim.v[k].r, tri.colours[corner][1] = prim.v[k].g, tri.colours[corner][2] = prim.v[k].b;
        if (corner < corner_count) cr += prim.v[k].r, cg += prim.v[k].g, cb += prim.v[k].b;
      }
      tri.r = static_cast<u8>(cr / corner_count);
      tri.g = static_cast<u8>(cg / corner_count);
      tri.b = static_cast<u8>(cb / corner_count);
      tri.blend = prim.blend;
      tri.slot = std::clamp(prim.slot, 1, 0xFFF);
      tri.order = tex_top - tex_i++;
      tris.push_back(tri);
    }
  }
  u32 copies_base = 0;
  for (const Tri& tri : tris) copies_base = std::max(copies_base, tri.order + 1);
  fx_model_copies(view, lb, copies_base, [&](render::TexRef set, const float* x, const float* y, const float* u,
                                                                              const float* v, u8 r, u8 gg, u8 b, int blend, int slot, u32 order) {
    Tri tri{};
    tri.set = set;
    for (int corner = 0; corner < 4; ++corner) tri.x[corner] = x[corner], tri.y[corner] = y[corner], tri.u[corner] = u[corner], tri.v[corner] = v[corner];
    tri.r = r, tri.g = gg, tri.b = b;
    tri.blend = blend;
    tri.slot = slot;
    tri.order = order;
    tris.push_back(tri);
  });
  // SmoothEffectRings: closed strips of untextured Gouraud quads (spell rings / tubes) get extra segments (strip_smooth.h).
  std::vector<effects::FlatQuad> smooth_flats;
  const std::vector<effects::FlatQuad>* flats = &fx_frame_.flats;
  if (smooth_rings_ && fx_frame_.flats.size() >= 3) {
    std::vector<effects::SmoothQuad> sq;
    // Flats that are not ring candidates (degenerate corners).
    std::vector<size_t> keep;
    for (size_t i = 0; i < fx_frame_.flats.size(); ++i) {
      const effects::FlatQuad& quad = fx_frame_.flats[i];
      // Texture order -> perimeter order.
      static constexpr int kLoop[4] = {0, 1, 3, 2};
      bool distinct = true;
      for (int first = 0; first < 4; ++first)
        for (int second = first + 1; second < 4; ++second) distinct &= quad.x[first] != quad.x[second] || quad.y[first] != quad.y[second];
      if (!distinct) {
        keep.push_back(i);
        continue;
      }
      effects::SmoothQuad smooth;
      smooth.tag = static_cast<int>(i);
      for (int k = 0; k < 4; ++k) {
        const int corner = kLoop[k];
        smooth.v[k].p[0] = quad.x[corner], smooth.v[k].p[1] = quad.y[corner], smooth.v[k].p[2] = 0;
        for (int ch = 0; ch < 3; ++ch) smooth.v[k].c[ch] = quad.c[corner][ch];
      }
      sq.push_back(smooth);
    }
    const float px = lb.scale;
    if (effects::smooth_quad_rings(sq, 6.0f, [px](const float point[3], float xy[2]) {
          xy[0] = point[0] * px, xy[1] = point[1] * px;
          return true;
        })) {
      for (size_t i : keep) smooth_flats.push_back(fx_frame_.flats[i]);
      for (const effects::SmoothQuad& smooth : sq) {
        const effects::FlatQuad& src_flat = fx_frame_.flats[static_cast<size_t>(smooth.tag)];
        effects::FlatQuad flat = src_flat;
        static constexpr int kLoop[4] = {0, 1, 3, 2};
        int mean[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          const int corner = kLoop[k];
          flat.x[corner] = smooth.v[k].p[0], flat.y[corner] = smooth.v[k].p[1];
          for (int ch = 0; ch < 3; ++ch) {
            flat.c[corner][ch] = static_cast<u8>(std::clamp(smooth.v[k].c[ch] + 0.5f, 0.0f, 255.0f));
            mean[ch] += flat.c[corner][ch];
          }
        }
        flat.r = static_cast<u8>(mean[0] / 4), flat.g = static_cast<u8>(mean[1] / 4), flat.b = static_cast<u8>(mean[2] / 4);
        smooth_flats.push_back(flat);
      }
      flats = &smooth_flats;
    }
  }
  if (fx_frame_.quads.empty() && flats->empty() && tris.empty() && semi_meshes.empty()) {
    fx_draw_screen(frame, lb);
    return;
  }
  struct Item {
    int slot;
    u32 order;
    // 0 sprite quad, 1 mesh triangle, 2 bespoke flat quad, 3 semi-transparent mesh object.
    u8 kind;
    u32 idx;
  };
  std::vector<Item> items;
  items.reserve(fx_frame_.quads.size() + tris.size() + flats->size());
  for (u32 i = 0; i < fx_frame_.quads.size(); ++i) items.push_back({fx_frame_.quads[i].slot, fx_frame_.quads[i].order, 0, i});
  for (u32 i = 0; i < tris.size(); ++i) items.push_back({tris[i].slot, tris[i].order, 1, i});
  for (u32 i = 0; i < flats->size(); ++i) items.push_back({(*flats)[i].slot, 0, 2, i});
  // The slot StagePipeline gives the instance: fixed = bias, else centre z / 16 + bias.
  for (u32 i = 0; i < semi_meshes.size(); ++i) {
    const render::StageInstance& in = semi_meshes[i].in;
    const int slot = in.fixed_slot ? in.bias : static_cast<int>(std::floor(in.cam[11] / 16.0f)) + in.bias;
    items.push_back({std::clamp(slot, 0, 0xFFF), 0, 3, i});
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& lhs, const Item& rhs) { return lhs.slot != rhs.slot ? lhs.slot > rhs.slot : lhs.order > rhs.order; });
  particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), fx_particle_pass_);
  fx_particle_pass_ = true;
  // The frame copy: the picture so far into fx_copy_rt_ (the rendering resumes with its depth cleared: what follows is filed nearer and
  // drawn over it, as in the ordering table).
  auto copy_picture = [&] {
    copy_pending = false;
    particles_.end();
    ctx_->copy_color(fx_copy_rt_);
    frame.set_scissor(mesh_scissor);
    particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), true);
  };
  for (size_t ii = 0; ii < items.size(); ++ii) {
    const Item& it = items[ii];
    if (copy_pending && it.slot <= copy_slot) copy_picture();
    // A run of semi-transparent mesh objects: drawn through the stage pipeline between the particle batches.
    if (it.kind == 3) {
      std::vector<render::StageInstance> run_own, run_res;
      size_t jj = ii;
      for (; jj < items.size() && items[jj].kind == 3; ++jj) {
        const SemiMesh& sm = semi_meshes[items[jj].idx];
        (sm.own ? run_own : run_res).push_back(sm.in);
      }
      ii = jj - 1;
      particles_.end();
      stage_pipe_.begin(frame, mesh_scissor);
      stage_pipe_.set_gte(view.h, view.ox, view.oy);
      stage_pipe_.set_depth_cue(mesh_cue);
      if (!run_own.empty() && gpu && gpu->stage) stage_pipe_.draw(*gpu->stage, mesh_proj, run_own, mesh_linear, own_asset);
      if (!run_res.empty() && rg && rg->stage) stage_pipe_.draw(*rg->stage, mesh_proj, run_res, mesh_linear, res_asset);
      stage_pipe_.end();
      frame.set_scissor(mesh_scissor);
      particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), true);
      continue;
    }
    if (it.kind == 0) {
      if (gpu) gpu->sprites->draw_quad(particles_, fx_frame_.quads[it.idx], lb);
    } else if (it.kind == 1) {
      const Tri& tri = tris[it.idx];
      static const bool nodepth = std::getenv("OC_FX_NODEPTH") != nullptr;
      if (tri.gouraud)
        particles_.quad(tri.set, tri.blend, tri.x, tri.y, tri.u, tri.v, tri.colours, nodepth ? 0.0f : static_cast<float>(tri.slot) / render::kDepthRange);
      else
        particles_.quad(tri.set, tri.blend, tri.x, tri.y, tri.u, tri.v, tri.r, tri.g, tri.b, nodepth ? 0.0f : static_cast<float>(tri.slot) / render::kDepthRange);
    } else {
      const effects::FlatQuad& quad = (*flats)[it.idx];
      // Blend 5 = the NEGAPOSI screen negative (a white flat quad, then the frame copy subtracted = 255 - backdrop): white through the
      // particle batch's invert blend (out = src * (1 - dst)), so everything drawn before it in the sort turns negative.
      if (!white_.set) continue;
      float x[4], y[4], qx[4] = {quad.x[0], quad.x[1], quad.x[2], quad.x[3]};
      // Widescreen: a full-width flash / wash covers the extra columns too.
      lb.widen_full_width(qx, 4);
      for (int k = 0; k < 4; ++k) {
        x[k] = lb.ox + qx[k] * lb.scale;
        y[k] = lb.oy + quad.y[k] * lb.scale_y;
      }
      const float u[4] = {0, 1, 0, 1}, v[4] = {0, 0, 1, 1};
      // Per-corner (Gouraud) colours.
      particles_.quad(white_.set, quad.blend, x, y, u, v, quad.c, static_cast<float>(quad.slot) / render::kDepthRange);
    }
  }
  // Nothing at or above the slot: the copy is still made (a face filed deeper samples it next frame, like the VRAM page).
  if (copy_pending) copy_picture();
  particles_.end();
  fx_draw_screen(frame, lb);
}
}  // namespace oc::game
