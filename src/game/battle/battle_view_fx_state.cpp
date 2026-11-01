// BattleView: effect ambient colour, screen colour, op dispatch, warp, stage swap, slow motion and the per-tick update.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include "core/log.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"
#include "game/encounter_strips.h"

namespace oc::game {

using namespace bv;

// Op 0x65 (starting a ramp kills the running one first, so one ramp at a time): from the current
// back colour to the bytes @+8..+10 over s16 @+4 ticks. Each frame with ticks elapsed: cur = from + (to - from) * t / ticks (byte
// arithmetic, ticks != 0), t += elapsed, past `ticks` the target and the task ends; each step sets the back colour. The op runs
// the task once at once (t = 0 -> `from`; ticks 0 -> the target). A pack's type-0x24 entry comes here with ticks 0.
void BattleView::fx_ambient_set(int r, int g, int b, int ticks) {
  FxAmbient& ambient = fx_ambient_;
  const int to[3] = {r & 0xFF, g & 0xFF, b & 0xFF};
  for (size_t k = 0; k < 3; ++k) {
    ambient.from[k] = ambient.cur[k];
    ambient.to[k] = to[k];
  }
  ambient.elapsed = 0;
  ambient.ticks = ticks;
  ambient.ramping = true;
  // The stage multiplier: from its current value (128 before the first ramp) to the target.
  for (size_t k = 0; k < 3; ++k) {
    ambient.sfrom[k] = ambient.touched ? ambient.scur[k] : 128.0f;
    ambient.sto[k] = static_cast<float>(to[k]);
  }
  ambient.touched = true;
  fx_ambient_tick();
}

void BattleView::fx_ambient_tick() {
  FxAmbient& ambient = fx_ambient_;
  if (!ambient.ramping) return;
  if (ambient.ticks != 0)
    for (size_t k = 0; k < 3; ++k) ambient.cur[k] = (ambient.from[k] + ((ambient.to[k] - ambient.from[k]) * ambient.elapsed) / ambient.ticks) & 0xFF;
  for (size_t k = 0; k < 3; ++k)
    ambient.scur[k] = ambient.ticks != 0 ? ambient.sfrom[k] + (ambient.sto[k] - ambient.sfrom[k]) * static_cast<float>(ambient.elapsed) / static_cast<float>(ambient.ticks) : ambient.sto[k];
  ambient.elapsed += 1;
  if (ambient.ticks < ambient.elapsed) {
    for (size_t k = 0; k < 3; ++k) ambient.cur[k] = ambient.to[k], ambient.scur[k] = ambient.sto[k];
    ambient.ramping = false;
  }
}

// The stage / unit multiplier walks back to neutral (128) at a fixed rate, then forgets the last ramp; the unit back colour is untouched.
void BattleView::fx_ambient_release() {
  constexpr float kReleaseStep = 128.0f / 30.0f;
  FxAmbient& ambient = fx_ambient_;
  if (!ambient.touched || ambient.ramping) return;
  bool neutral = true;
  for (size_t k = 0; k < 3; ++k) {
    const float delta = 128.0f - ambient.scur[k];
    ambient.scur[k] += std::clamp(delta, -kReleaseStep, kReleaseStep);
    neutral = neutral && ambient.scur[k] == 128.0f;
  }
  if (neutral) fx_ambient_stage_reset();
}

void BattleView::fx_ambient_stage_reset() {
  FxAmbient& ambient = fx_ambient_;
  ambient.touched = false;
  for (size_t k = 0; k < 3; ++k) ambient.scur[k] = ambient.sfrom[k] = ambient.sto[k] = 128.0f;
}

// Before a unit's draw the light / colour matrices are rebuilt from the light table for the unit's attach
// point 9 (waist), and the lit packer (world-space normals) runs when the unit's lit flag is set.
// The table is in PS1 world units (y down) and this world is its mirror (model = T * yaw * flip), so the waist is
// mirrored into PS1 space for the distance / direction and the rows come back mirrored.
void BattleView::fx_light_params(int unit, const render::Mat4& model, render::MeshPipeline::DrawParams& dp) const {
  if (unit < 0 || unit >= battle::kUnits) return;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  float ax = 0, ay = 0, az = 0;
  if (!visual.has_model || !visual.pose.attach_now(9, alpha_, ax, ay, az)) return;
  float waist[3];
  for (int row = 0; row < 3; ++row) waist[row] = model.at(row, 0) * ax + model.at(row, 1) * ay + model.at(row, 2) * az + model.at(row, 3);
  const effects::LightMatrices lm = effects::light_matrices(fx_lights_, {waist[0], -waist[1], waist[2]}, false);
  dp.light.on = true;
  for (size_t k = 0; k < 3; ++k) dp.light.bk[k] = static_cast<float>(fx_ambient_.cur[k]) / 256.0f;
  for (size_t i = 0; i < 3; ++i)
    for (size_t k = 0; k < 3; ++k) {
      dp.light.dir[i][k] = k == 1 ? -lm.dir[i][k] : lm.dir[i][k];
      dp.light.rgb[i][k] = lm.rgb[i][k];
    }
}

// Ops 0x2B (layer 0) / 0x2C (layer 1): ticks s16 @+4, mode byte @+8 (0xFF = keep), colour bytes @+9..+11. A new
// colour or mode replaces the layer's task with one that ramps from the current colour (colour = start +
// (target - start) * t / ticks, run once at once; past `ticks` the target).
void BattleView::fx_screen_colour(int layer, const assets::SeqCmd& cmd) {
  if (cmd.data.size() < 8 || layer < 0 || layer > 1) return;
  ScreenColour& screen = fx_screen_[static_cast<size_t>(layer)];
  const int ticks = static_cast<s16>(cmd.data[0] | (cmd.data[1] << 8));
  const int mode = cmd.data[4];
  const float to[3] = {static_cast<float>(cmd.data[5]), static_cast<float>(cmd.data[6]), static_cast<float>(cmd.data[7])};
  // The old task of the layer is freed.
  screen.ramping = false;
  if (to[0] == screen.cur[0] && to[1] == screen.cur[1] && to[2] == screen.cur[2] && (mode == screen.mode || mode == 0xFF)) return;
  for (size_t k = 0; k < 3; ++k) {
    screen.from[k] = screen.cur[k];
    screen.to[k] = to[k];
  }
  if (mode != 0xFF) screen.mode = mode;
  screen.ticks = ticks;
  screen.elapsed = 0;
  screen.ramping = true;
  log::debug("battle fx: screen colour layer {} -> {} {} {} mode {} over {} ticks", layer, to[0], to[1], to[2], screen.mode, ticks);
  // The ramp runs once at once.
  fx_screen_tick_layer(screen);
}

void BattleView::fx_screen_tick_layer(ScreenColour& screen) {
  if (!screen.ramping) return;
  if (screen.ticks != 0)
    for (size_t k = 0; k < 3; ++k) screen.cur[k] = screen.from[k] + std::trunc((screen.to[k] - screen.from[k]) * static_cast<float>(screen.elapsed) / static_cast<float>(screen.ticks));
  ++screen.elapsed;
  if (screen.elapsed > screen.ticks) {
    for (size_t k = 0; k < 3; ++k) screen.cur[k] = screen.to[k];
    screen.ramping = false;
  }
}

void BattleView::fx_screen_tick() {
  for (ScreenColour& screen : fx_screen_) fx_screen_tick_layer(screen);
  // Op 0x52 quads: colour = from + (to - from) t / n, freed at t >= n.
  for (ScreenColour& screen : anm_screen_) fx_screen_tick_layer(screen);
  std::erase_if(anm_screen_, [](const ScreenColour& screen) { return !screen.ramping; });
  // Op 0x58 model colour ramps (the same lerp).
  for (FxUnitState& state : fx_unit_) {
    if (!state.ramping) continue;
    if (state.ticks != 0)
      for (size_t k = 0; k < 3; ++k) state.rgb[k] = state.from[k] + std::trunc((state.to[k] - state.from[k]) * static_cast<float>(state.elapsed) / static_cast<float>(state.ticks));
    ++state.elapsed;
    if (state.elapsed > state.ticks) {
      for (size_t k = 0; k < 3; ++k) state.rgb[k] = state.to[k];
      state.ramping = false;
    }
    state.tinted = true;
  }
}

void BattleView::fx_other(const assets::SeqCmd& cmd) {
  auto s16_at = [&](size_t off) { return off >= 4 && off - 4 + 2 <= cmd.data.size() ? static_cast<int>(static_cast<s16>(cmd.data[off - 4] | (cmd.data[off - 3] << 8))) : 0; };
  auto target_at = [&](int index) { return index >= 0 && static_cast<size_t>(index) < act_.targets.size() ? act_.targets[static_cast<size_t>(index)] : -1; };
  switch (cmd.op) {
    case 0x30: fx_hide(fx_units(0), 1, false); break;
    case 0x31: fx_hide(fx_units(0), 1, true); break;
    case 0x32: fx_set_hidden(true, false); break;
    case 0x33: fx_set_hidden(true, true); break;
    case 0x34: fx_set_hidden(false, false); break;
    case 0x35: fx_set_hidden(false, true); break;
    case 0x5B: fx_hide(fx_units(1), 1, false); break;
    case 0x5C: fx_hide(fx_units(1), 1, true); break;
    case 0x5D: fx_hide(fx_units(2), 1, false); break;
    case 0x5E: fx_hide(fx_units(2), 1, true); break;
    // The lit flag (the lit packer), not a hide bit: targets (0x61 on / 0x62 off), casters (0x63 / 0x64), everyone (0x68 / 0x69).
    case 0x61: case 0x62: case 0x63: case 0x64: case 0x68: case 0x69:
      for (int unit : fx_units(cmd.op == 0x61 || cmd.op == 0x62 ? 1 : cmd.op == 0x63 || cmd.op == 0x64 ? 2 : 0))
        if (unit >= 0 && unit < battle::kUnits) {
          fx_lit_[static_cast<size_t>(unit)] = cmd.op == 0x61 || cmd.op == 0x63 || cmd.op == 0x68;
          log::debug("battle fx: op {:02X} unit {} lit {} (ambient {} {} {}, light intensities {} {} {})", cmd.op, unit, fx_lit_[static_cast<size_t>(unit)],
                     fx_ambient_.cur[0], fx_ambient_.cur[1], fx_ambient_.cur[2], fx_lights_.slot[0].intensity, fx_lights_.slot[1].intensity,
                     fx_lights_.slot[2].intensity);
        }
      break;
    // The three light slots' intensity := 0 (position / colour kept).
    case 0x66:
      for (auto& slot : fx_lights_.slot) slot.intensity = 0;
      break;
    // Music volume fade: to s16 @+8 over s16 @+4 ticks (the cast's own duck and restore use the same fade).
    case 0x54: audio_.fade_music(s16_at(8), s16_at(4)); break;
    // Slow motion: with s16 @+10 == 0 the slow-motion period = byte @+8, else as 0x79.
    case 0x56:
      if (cmd.data.size() >= 8 && s16_at(10) == 0) {
        fx_slow_x_ = cmd.data[4];
        fx_slow_phase_ = 0;
        break;
      }
      [[fallthrough]];
    // Slow-motion count = byte @+8.
    case 0x79:
      if (cmd.data.size() >= 5) fx_slow_n_ = cmd.data[4];
      break;
    case 0x3E: case 0x3F: case 0x40: fx_warp_op(cmd); break;
    // CallDamage's number spawn ("damv" / "gurv" / "gaiv" by the result type) for the first acting unit, i.e. the caster's own pending HP
    // change, with the offset digit layout.
    case 0x6B:
      for (size_t i = 0; i < act_.ev.size(); ++i)
        if (act_.ev[i].target == act_.actor && !fx_fired_[i]) {
          // The offset layout of BattleView::spawn_pop.
          pop_caster_layout_ = true;
          fx_fire(i);
          pop_caster_layout_ = false;
          break;
        }
      break;
    // Stage layers (every emitter of the name; 'bti0' only while op 0x7B's gate is set): 0x26 near / far / roof / ring on, 0x27 the four
    // off, 0x36 / 0x37 far on / off, 0x38 / 0x39 near, 0x3A / 0x3B ring, 0x3C / 0x3D roof, 0x5F bti0 on (any duration), 0x60 bti0 off.
    case 0x26: case 0x27:
      for (const char* layer : {"near", "far", "roof", "ring"}) fx_stage_layer(layer, cmd.op == 0x26, false);
      break;
    case 0x36: case 0x37: fx_stage_layer("far", cmd.op == 0x36, false); break;
    case 0x38: case 0x39: fx_stage_layer("near", cmd.op == 0x38, false); break;
    case 0x3A: case 0x3B: fx_stage_layer("ring", cmd.op == 0x3A, false); break;
    case 0x3C: case 0x3D: fx_stage_layer("roof", cmd.op == 0x3C, false); break;
    case 0x7B: fx_bti_gate_ = true; break;
    case 0x5F: case 0x60:
      if (fx_bti_gate_) {
        if (cmd.op == 0x5F || !keep_intro_layer_) fx_stage_layer("bti0", cmd.op == 0x5F, true);
        if (cmd.op == 0x60) fx_bti_gate_ = false;
      }
      break;
    case 0x2B: fx_screen_colour(0, cmd); break;
    case 0x2C: fx_screen_colour(1, cmd); break;
    // Depth cue on, every actor on the depth-cued renderer.
    case 0x24:
      fx_fog_on_ = true;
      fx_unit_fog_.fill(true);
      break;
    // Depth cue off.
    case 0x6C:
      fx_fog_on_ = false;
      fx_unit_fog_.fill(false);
      break;
    // The ground's draw-order bias: 0x6D ramps it to s16 @+8 over s16 @+4 ticks, 0x6E back to 100.
    case 0x6D: stage_scene_.ground_bias_ramp(s16_at(4), s16_at(8)); break;
    case 0x6E: stage_scene_.ground_bias_ramp(s16_at(4), 100); break;
    // Everyone / the targets / the casters back on the plain renderer.
    case 0x49: case 0x4A: case 0x4B:
      for (int unit : fx_units(cmd.op == 0x49 ? 0 : cmd.op == 0x4A ? 1 : 2))
        if (unit >= 0 && unit < battle::kUnits) fx_unit_fog_[static_cast<size_t>(unit)] = false;
      break;
    // Model colour ramp on target (byte @+0xB + the loop cursor): colour bytes @+8..+10 over s16 @+4 ticks from the model's colour.
    case 0x58: {
      if (cmd.data.size() < 8) break;
      const int unit = target_at(cmd.data[7] + (fx_ ? fx_->cursor() : 0));
      if (unit < 0) break;
      FxUnitState& state = fx_unit_[static_cast<size_t>(unit)];
      for (size_t k = 0; k < 3; ++k) {
        state.from[k] = state.rgb[k];
        state.to[k] = static_cast<float>(cmd.data[4 + k]);
      }
      state.ticks = s16_at(4);
      // A task born in a pass is run at once with the pass delta.
      state.elapsed = std::min(state.ticks, fx_task_credit());
      state.ramping = state.tinted = true;
      break;
    }
    // Target s16 @+10: SetAnim(s16 @+8, blend 0x20) with the loop flag s16 @+0xC (0 = clamp on the last keyframe, e.g. kamedeth's 'deat'
    // clip plays once over ~465 ticks).
    case 0x59: {
      const int unit = target_at(s16_at(10));
      if (unit >= 0 && vis_[static_cast<size_t>(unit)].has_model) {
        Visual& target_visual = vis_[static_cast<size_t>(unit)];
        target_visual.pose.play(s16_at(8), 0x20);
        target_visual.pose.set_loop(s16_at(0xC) != 0);
      }
      break;
    }
    // Targets' drop shadow off (0x74) / on (0x75).
    case 0x74: case 0x75:
      for (int unit : fx_units(1))
        if (unit >= 0 && unit < battle::kUnits) shadow_off_[static_cast<size_t>(unit)] = cmd.op == 0x74;
      break;
    // Targets and casters turn sideways: yaw -0x400 when x < 1, else 0x400; facing = (-sin yaw, cos yaw).
    case 0x76:
      for (int which : {1, 2})
        for (int unit : fx_units(which)) {
          if (unit < 0 || unit >= battle::kUnits) continue;
          Visual& visual = vis_[static_cast<size_t>(unit)];
          const float yaw = (visual.x < 1.0f ? -1024.0f : 1024.0f) * (2.0f * kPi / 4096.0f);
          visual.face_x = visual.pface_x = -std::sin(yaw);
          visual.face_z = visual.pface_z = -std::cos(yaw);
        }
      break;
    // Depth-cue ramp: frames s16@+4, fog near / far s16@+8 / +10, far colour bytes @+12..14 (0x80 = neutral).
    case 0x2D:
      if (cmd.data.size() >= 11) {
        const int frames = static_cast<s16>(cmd.data[0] | (cmd.data[1] << 8));
        // The ramp runs in the stored values; the GTE itself keeps the last pair with far - near >= 100 (fx_fog_tick). E.g. the ramp
        // (16384, 20480) -> (296, 296) fogs the whole scene to the far colour; volcano ramps the colour 128 -> 0 at (9684, 30504).
        Fog to = fog_raw_;
        to.near = static_cast<float>(static_cast<s16>(cmd.data[4] | (cmd.data[5] << 8)));
        to.far = static_cast<float>(static_cast<s16>(cmd.data[6] | (cmd.data[7] << 8)));
        to.r = static_cast<float>(cmd.data[8]);
        to.g = static_cast<float>(cmd.data[9]);
        to.b = static_cast<float>(cmd.data[10]);
        fog_from_ = fog_raw_;
        fog_to_ = to;
        fog_total_ = fog_left_ = std::max(1, frames);
        // The fog task's birth steps: 2 steps ahead of the op-time start.
        for (int k = std::min(2, fx_task_credit()); k > 0 && fog_left_ > 1; --k) fx_fog_tick();
        log::debug("battle fx: fog ramp {} frames -> rgb {} {} {} near {} far {}", frames, to.r, to.g, to.b, to.near, to.far);
      }
      break;
    case 0x43: case 0x44: case 0x45: case 0x46: case 0x4F: case 0x57: fx_actor_script(cmd); break;
    // Not presented: BattleFxRun::presented() lists what is.
    default: break;
  }
}

// ---- screen copy warp (ops 0x3E / 0x3F / 0x40) ----------------------------------------------------------------------------------------
// 0x3E: warp on and the state reset (colour 0x808080, angles, translation, offset 0, ABR 0). 0x3F: off. 0x40: a ramp
// over s16 @+4 ticks from the current state to angles s16 @+8 / +10 / +12, translation s16 @+0xE / +0x10 / +0x12, offset
// s16 @+0x14 / +0x16, colour bytes @+0x18..+0x1A; ABR = byte @+0x1B at once. Value = start + (target - start) * t / ticks, past `ticks` the
// target (run once at once). Reset with the effect system and at a pack start.
void BattleView::fx_warp_op(const assets::SeqCmd& cmd) {
  auto s16_at = [&](size_t off) { return off >= 4 && off - 4 + 2 <= cmd.data.size() ? static_cast<float>(static_cast<s16>(cmd.data[off - 4] | (cmd.data[off - 3] << 8))) : 0.0f; };
  Warp& warp = fx_warp_;
  if (cmd.op == 0x3E) {
    warp = Warp{};
    warp.on = true;
    return;
  }
  if (cmd.op == 0x3F) {
    warp.on = false;
    return;
  }
  if (cmd.data.size() < 24) return;
  Warp::Ramp& ramp = warp.ramp;
  const float cur[11] = {warp.ang[0], warp.ang[1], warp.ang[2], warp.tr[0], warp.tr[1], warp.tr[2], warp.off[0], warp.off[1], warp.rgb[0], warp.rgb[1], warp.rgb[2]};
  const float to[11] = {s16_at(8), s16_at(10), s16_at(12), s16_at(14), s16_at(16), s16_at(18), s16_at(20), s16_at(22),
                        static_cast<float>(cmd.data[20]), static_cast<float>(cmd.data[21]), static_cast<float>(cmd.data[22])};
  for (int k = 0; k < 11; ++k) {
    ramp.from[k] = cur[k];
    ramp.to[k] = to[k];
  }
  warp.abr = cmd.data[23] & 3;
  log::debug("battle fx: warp ramp {} ticks -> angles {} {} {} move {} {} {} offset {} {} rgb {} {} {} abr {}", static_cast<int>(s16_at(4)), to[0], to[1], to[2],
             to[3], to[4], to[5], to[6], to[7], to[8], to[9], to[10], warp.abr);
  ramp.ticks = static_cast<int>(s16_at(4));
  ramp.elapsed = 0;
  ramp.on = true;
  // The ramp runs once at once.
  fx_warp_tick();
}

void BattleView::fx_warp_tick() {
  Warp& warp = fx_warp_;
  Warp::Ramp& ramp = warp.ramp;
  if (!ramp.on) return;
  float values[11];
  for (int k = 0; k < 11; ++k) values[k] = ramp.ticks != 0 ? ramp.from[k] + std::trunc((ramp.to[k] - ramp.from[k]) * static_cast<float>(ramp.elapsed) / static_cast<float>(ramp.ticks)) : ramp.from[k];
  ++ramp.elapsed;
  if (ramp.elapsed > ramp.ticks) {
    for (int k = 0; k < 11; ++k) values[k] = ramp.to[k];
    ramp.on = false;
  }
  for (int k = 0; k < 3; ++k) {
    warp.ang[k] = values[k];
    warp.tr[k] = values[3 + k];
    warp.rgb[k] = values[8 + k];
  }
  warp.off[0] = values[6];
  warp.off[1] = values[7];
}

bool BattleView::fx_warp_begin(const render::Frame& frame) {
  // The bracket is off: the next one starts without a previous output.
  if (!fx_warp_.on && !rde_hand_) {
    fx_acc_have_ = false;
    fx_acc_tick_ = -1;
    hand_carry_ = {};
  }
  if ((!fx_warp_.on && !rde_hand_) || !ctx_ || frame.extent.width == 0 || frame.extent.height == 0) return false;
  if (!fx_warp_ensure(frame.extent.width, frame.extent.height)) return false;
  ctx_->begin_target(fx_warp_rt_, frame, fx_warp_frame_);
  return true;
}

bool BattleView::fx_warp_ensure(u32 w, u32 h) {
  if (fx_warp_rt_.valid() && fx_warp_rt_.width() == w && fx_warp_rt_.height() == h) return true;
  if (fx_warp_rt_.valid()) {
    fx_warp_set_ = {};
    fx_warp_rt_.destroy();
  }
  if (!fx_warp_rt_.create(*ctx_, w, h)) return false;
  fx_warp_set_ = fx_warp_rt_.ref(nearest_);
  if (!fx_warp_set_) {
    fx_warp_rt_.destroy();
    return false;
  }
  return true;
}

// The overlay's camera cross-fade: a new serial cuts the second camera to key 0 of its shot (in the battle camera's bank); it then steps once per
// tick. `promote` hands the shot to the battle camera at the second camera's progress (the first camera task ends, the next one in the list is
// the head now: a second camera replaced without a stop, else the running one), `stop` ends the second camera.
void BattleView::fx_cross_tick() {
  effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  if (!ms) {
    fx_cross_.on = fx_cross_.next_on = false;
    return;
  }
  effects::FxModels::CrossView& view = ms->cross;
  // A stop that lands in the tick of a replacement belongs to the camera being replaced.
  bool replaced = false;
  if (view.serial != fx_cross_.serial) {
    if (fx_cross_.on) {
      replaced = true;
      fx_cross_.next = fx_cross_.cam;
      fx_cross_.next_shot = fx_cross_.shot;
      fx_cross_.next_ticks = fx_cross_.ticks;
      fx_cross_.next_on = true;
    }
    fx_cross_.serial = view.serial;
    fx_cross_.cam = BattleCamera{};
    fx_cross_.cam.set_bank(camera_.bank());
    fx_cross_.cam.set_anchor([this](int unit, int point) { return anchor_frame(unit, point); });
    fx_cross_.on = fx_cross_.cam.select(view.shot);
    fx_cross_.shot = view.shot;
    fx_cross_.ticks = 0;
    log::debug("battle fx: cross-fade shot '{}' {} at tick {}", view.shot, fx_cross_.on ? "selected" : "missing", tick_count_);
  } else if (fx_cross_.on) {
    CamEvents events;
    fx_cross_.cam.tick(events);
    ++fx_cross_.ticks;
  }
  if (fx_cross_.next_on) {
    CamEvents events;
    fx_cross_.next.tick(events);
    ++fx_cross_.next_ticks;
  }
  if (view.promote) {
    view.promote = false;
    if (fx_cross_.next_on) {
      camera_.kill_shot();
      if (camera_.select(fx_cross_.next_shot)) camera_.credit_ticks(fx_cross_.next_ticks);
      fx_cross_.next_on = false;
    } else {
      if (fx_cross_.on) {
        camera_.kill_shot();
        if (camera_.select(fx_cross_.shot)) camera_.credit_ticks(fx_cross_.ticks);
      }
      fx_cross_.on = false;
    }
  }
  if (view.stop) {
    view.stop = false;
    (replaced ? fx_cross_.next_on : fx_cross_.on) = false;
  }
  fx_cross_.alpha = view.alpha;
  fx_cross_.fill = view.fill;
}

// The picture of a cross-fade: the battle camera's view with `fill` subtracted from every channel, then the second camera's view (its own pass
// into fx_cross_rt_ over black) added times alpha / 128, over the 320 x 224 draw area.
bool BattleView::fx_cross_draw(const render::Frame& frame, const render::Letterbox& lb, bool sprites_resume) {
  if (!fx_cross_.on || !ctx_ || !white_.set || frame.extent.width == 0 || frame.extent.height == 0) return false;
  const u32 w = frame.extent.width, h = frame.extent.height;
  if (!fx_cross_rt_.valid() || fx_cross_rt_.width() != w || fx_cross_rt_.height() != h) {
    fx_cross_set_ = {};
    fx_cross_rt_.destroy();
    if (!fx_cross_rt_.create(*ctx_, w, h)) return false;
    fx_cross_set_ = fx_cross_rt_.ref(nearest_);
    if (!fx_cross_set_) {
      fx_cross_rt_.destroy();
      return false;
    }
  }
  draw_scene_3d(frame, lb, sprites_resume);
  ctx_->begin_target(fx_cross_rt_, frame, fx_cross_frame_);
  draw_camera_ = &fx_cross_.cam;
  draw_scene_3d(fx_cross_frame_, lb, true);
  draw_camera_ = nullptr;
  ctx_->end_target(fx_cross_rt_);
  apply_camera(alpha_);
  const float fw = static_cast<float>(w), fh = static_cast<float>(h);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<u32>(std::min(fw - x0, std::round(lb.width()))),
                             static_cast<u32>(std::min(fh - y0, std::round(kBufferRows * lb.scale_y)))};
  const float qx[4] = {0, fw, 0, fw}, qy[4] = {0, 0, fh, fh}, qu[4] = {0, 1, 0, 1}, qv[4] = {0, 0, 1, 1};
  frame.set_scissor(scissor);
  particles_.begin(frame, fw, fh, true);
  // Untextured PS1 colour c is c / 255 of full; the batch's modulation is 0x80 = 1.0.
  const u8 fill = static_cast<u8>(fx_cross_.fill * 128 / 255);
  if (fill > 0) particles_.quad(white_.set, 3, qx, qy, qu, qv, fill, fill, fill, 0.0f);
  const u8 alpha = static_cast<u8>(std::clamp(fx_cross_.alpha, 0, 128));
  if (alpha > 0) particles_.quad(fx_cross_set_, 2, qx, qy, qu, qv, alpha, alpha, alpha, 0.0f);
  particles_.end();
  frame.set_scissor(frame.full());
  fx_particle_pass_ = true;
  return true;
}

// Nothing is drawn while the colour is black. Mode 0 = opaque, 1..4 = ABR mode - 1 (the batch's blend numbering is ABR + 1); the PS1 quad is 352 x 272 from the
// draw area's corner, i.e. the whole visible picture.
void BattleView::fx_tint_draw(const render::Frame& frame, const render::Letterbox& lb) {
  const effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  if (!ms || !white_.set) return;
  const effects::FxModels::Tint& tint = ms->tint;
  if (tint.rgb[0] == 0 && tint.rgb[1] == 0 && tint.rgb[2] == 0) return;
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const float x1 = std::min(fw, x0 + 352.0f * lb.scale), y1 = std::min(fh, y0 + 272.0f * lb.scale_y);
  const float qx[4] = {x0, x1, x0, x1}, qy[4] = {y0, y0, y1, y1}, qu[4] = {0, 1, 0, 1}, qv[4] = {0, 0, 1, 1};
  auto channel = [](u8 value) { return static_cast<u8>(value * 128 / 255); };
  particles_.begin(frame, fw, fh, true);
  particles_.quad(white_.set, std::clamp(tint.mode, 0, 4), qx, qy, qu, qv, channel(tint.rgb[0]), channel(tint.rgb[1]), channel(tint.rgb[2]), 0.0f);
  particles_.end();
}

// The frame copy's target: the frame's size divided by the whole factor that keeps at most kCopyPixelsPerPs1 pixels per PS1 pixel.
bool BattleView::fx_copy_ensure(const render::Frame& frame, const render::Letterbox& lb) {
  if (!ctx_ || frame.extent.width == 0 || frame.extent.height == 0) return false;
  const u32 divisor = static_cast<u32>(std::max(1.0f, std::floor(lb.scale / kCopyPixelsPerPs1)));
  const u32 w = (frame.extent.width + divisor - 1) / divisor, h = (frame.extent.height + divisor - 1) / divisor;
  if (fx_copy_rt_.valid() && fx_copy_rt_.width() == w && fx_copy_rt_.height() == h && fx_copy_set_) return true;
  fx_copy_set_ = {};
  fx_copy_rt_.destroy();
  if (!fx_copy_rt_.create(*ctx_, w, h)) return false;
  fx_copy_set_ = fx_copy_rt_.ref(linear_);
  return static_cast<bool>(fx_copy_set_);
}

// The two feed-back targets (frame size, bilinear: the zoom resamples the previous output). False = no feed-back (the strips then sample the scene).
bool BattleView::fx_acc_ensure(const render::Frame& frame) {
  for (int i = 0; i < 2; ++i)
    if (!fx_acc_ensure_one(i, frame.extent.width, frame.extent.height)) return false;
  return true;
}

bool BattleView::fx_acc_ensure_one(int i, u32 w, u32 h) {
  if (fx_acc_rt_[i].valid() && fx_acc_rt_[i].width() == w && fx_acc_rt_[i].height() == h && fx_acc_set_[i]) return true;
  fx_acc_set_[i] = {};
  fx_acc_have_ = false;
  fx_acc_rt_[i].destroy();
  if (!fx_acc_rt_[i].create(*ctx_, w, h)) return false;
  fx_acc_set_[i] = fx_acc_rt_[i].ref(linear_);
  return static_cast<bool>(fx_acc_set_[i]);
}

// Makes one missing warp / feed-back target of the battle picture's size per call (each is a full-size colour + depth image): run behind
// the encounter's white hold, the hand-over's first frame then allocates nothing. True when they are all there (or one cannot be made).
bool BattleView::prepare_fx_targets(u32 w, u32 h) {
  if (!ctx_ || w == 0 || h == 0) return true;
  const auto matches = [&](const render::RenderTarget& rt) { return rt.valid() && rt.width() == w && rt.height() == h; };
  if (!matches(fx_warp_rt_)) return !fx_warp_ensure(w, h);
  for (int i = 0; i < 2; ++i)
    if (!matches(fx_acc_rt_[i])) return !fx_acc_ensure_one(i, w, h);
  return true;
}

void BattleView::fx_warp_end(const render::Frame& frame, const render::Letterbox& lb) {
  ctx_->end_target(fx_warp_rt_);
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                             static_cast<u32>(std::min(fw - x0, std::round(lb.width()))), static_cast<u32>(std::min(fh - y0, std::round(kBufferRows * lb.scale_y)))};
  const float qx[4] = {0, fw, 0, fw}, qy[4] = {0, 0, fh, fh}, qu[4] = {0, 1, 0, 1}, qv[4] = {0, 0, 1, 1};
  // Feed-back: this tick's finished picture plus the strips go into fx_acc_rt_[cur]; the strips sample the previous game frame's output (the
  // scene itself on the very first step). Then the target is copied to the frame.
  const bool acc = fx_acc_ensure(frame);
  render::TexRef src = fx_warp_set_;
  render::Frame af = frame;
  if (acc) {
    if (fx_acc_tick_ != tick_count_) {
      // One pass per game tick; in the hand-over the anchor (previous update's output) stays for the 4 ticks of an update and the other target is
      // rewritten, so the live scene keeps moving while the feed-back advances at the update's rate.
      if (!rde_hand_ || fx_acc_adv_ || fx_acc_tick_ < 0) {
        fx_acc_cur_ = 1 - fx_acc_cur_;
        fx_acc_have_ = fx_acc_tick_ >= 0;
      }
      fx_acc_tick_ = tick_count_;
    }
    if (fx_acc_have_) src = fx_acc_set_[1 - fx_acc_cur_];
    // The first battle frame's 'previous output' is the field's last one (the strips never stopped).
    else if (hand_carry_) src = hand_carry_;
    if (fx_acc_have_ && !rde_hand_) hand_carry_ = {};
    ctx_->begin_target(fx_acc_rt_[fx_acc_cur_], frame, af);
    af.set_scissor(scissor);
  } else {
    frame.set_scissor(scissor);
  }
  particles_.begin(acc ? af : frame, fw, fh, fx_particle_pass_);
  fx_particle_pass_ = true;
  // The hand-over fades to black (the smear alone, the live scene no longer injected), then the battle fades in from black.
  const int ht = std::max(0, white_ticks_);
  // The carried smear's weight.
  const float hg = rde_hand_ ? encstrips::hand_smear(ht) : 1.0f;
  // The battle scene's weight (fades in under the smear's last ticks).
  const float bg = rde_hand_ ? encstrips::hand_battle(ht) : 1.0f;
  const bool hand_strips = rde_hand_ && acc && ht < encstrips::kBlackAt;
  // While the hand-over strips run, the battle scene is NOT part of the picture at all; the scene appears only in the fade-in from black below.
  // The base under the strips is the previous output itself, decaying by kHandKeep per tick (the PS1 draw buffer keeps its old content where
  // nothing is drawn), not black: over its own previous output the centre recedes while the frame stays full, and the darkening ramp
  // still takes it to black.
  // The strip pass of the hand-over.
  if (hand_strips) {
    // Once the battle shows through (the cross-fade), the smear holds its level instead of decaying further.
    const u8 kb = static_cast<u8>(128.0f * (ht >= encstrips::kDarkFrom ? 1.0f : kHandKeep) + 0.5f);
    particles_.quad(src, 0, qx, qy, qu, qv, kb, kb, kb, 0.0f);
    particles_.end();
    sprites_->begin(af, fw, fh, true);
    // The previous output (the field's last one on the first step) is the strips: ONE rectangle (the four 80-wide strips side by side form the whole
    // picture), scaled about the GTE offset by the bracket's zoom 4096 / (4096 + tz) per update, drawn over the finished scene with the blend weight
    // kHandKeep. Where the shrunk copy does not reach nothing is drawn: the live scene shows there (never a fill).
    const int mclk = rde_m0_ + tick_count_;
    // Tick j of an update draws the update's pass with the partial factors e = (j + 1) / 4 from the anchor.
    // The battle presents at 60 Hz (kHoldPassTicks = 1): every tick is a real pass, so ramps 2 / 3 of the bracket (tz +131 -> 0, zoom
    // 0.969 -> 1.0 per pass over ~50 passes) visibly pull the carried smear back toward the centre while it darkens.
    constexpr int kN = encstrips::kHoldPassTicks;
    const int uj = std::max(0, mclk - 1) % kN;
    const float ue = static_cast<float>(uj + 1) / static_cast<float>(kN);
    const encstrips::Params bp = encstrips::main_bracket(mclk - uj);
    const float zoom = bp.on ? std::pow(bp.scale(), ue) : 1.0f;
    // ABR 0: (B + F) / 2 (the decay sits in the base, kHandKeep).
    const u8 a8 = static_cast<u8>(std::pow(0.5f, ue) * 255.0f + 0.5f);
    fx_acc_adv_ = uj == kN - 1;
    const float rx0 = static_cast<float>(scissor.x), ry0 = static_cast<float>(scissor.y);
    const float rw = static_cast<float>(scissor.w), rh = static_cast<float>(scissor.h);
    const float cx = lb.ox + 160.0f * lb.scale, cy = lb.oy + 120.0f * lb.scale_y;
    sprites_->draw(src, cx + (rx0 - cx) * zoom, cy + (ry0 - cy) * zoom, rw * zoom, rh * zoom, rx0 / fw, ry0 / fh, (rx0 + rw) / fw, (ry0 + rh) / fh,
                   render::pack_rgba(255, 255, 255, a8));
    sprites_->end();
    ctx_->end_target(fx_acc_rt_[fx_acc_cur_]);
    frame.set_scissor(scissor);
    // Composite: the battle scene (opaque, times its fade-in weight; black before kBattleFrom) and the smear added over it times its own weight.
    const u8 pg = static_cast<u8>(128.0f * hg + 0.5f), sg = static_cast<u8>(128.0f * bg + 0.5f);
    particles_.begin(frame, fw, fh, true);
    particles_.quad(fx_warp_set_, 0, qx, qy, qu, qv, sg, sg, sg, 0.0f);
    particles_.quad(fx_acc_set_[fx_acc_cur_], 2, qx, qy, qu, qv, pg, pg, pg, 0.0f);
    particles_.end();
    frame.set_scissor(frame.full());
    return;
  }
  // The scene as drawn (opaque copy of the whole frame).
  particles_.quad(fx_warp_set_, 0, qx, qy, qu, qv, 128, 128, 128, 0.0f);
  // The battle fading in (no strips any more): the scene copy times its fade-in weight.
  if (rde_hand_ && acc) {
    particles_.end();
    ctx_->end_target(fx_acc_rt_[fx_acc_cur_]);
    frame.set_scissor(scissor);
    const u8 pg = static_cast<u8>(128.0f * bg + 0.5f);
    particles_.begin(frame, fw, fh, true);
    particles_.quad(fx_acc_set_[fx_acc_cur_], 0, qx, qy, qu, qv, pg, pg, pg, 0.0f);
    particles_.end();
    frame.set_scissor(frame.full());
    return;
  }
  // Strips of 0x50 pixels over the draw area (widescreen: its visible columns), full height.
  const Warp& warp = fx_warp_;
  const effects::Mat3 rot = effects::Mat3::from_angles(static_cast<int>(warp.ang[0]), static_cast<int>(warp.ang[1]), static_cast<int>(warp.ang[2]));
  const float cx = kGeomOffsetX, cy = kGeomOffsetY, hh = static_cast<float>(kBufferRows);
  auto project = [&](float sx, float sy, float& ox, float& oy) {
    const effects::Vec3 pos = rot.apply({(sx - cx) * 4.0f, (sy - cy) * 4.0f, 0.0f});
    const float z = std::max(1.0f, pos[2] + warp.tr[2] + 4096.0f);
    const float lx = cx + warp.off[0] + 1024.0f * (pos[0] + warp.tr[0]) / z, ly = cy + warp.off[1] + 1024.0f * (pos[1] + warp.tr[1]) / z;
    ox = lb.ox + lx * lb.scale;
    oy = lb.oy + ly * lb.scale_y;
  };
  const float left = (lb.left() - lb.ox) / lb.scale, right = left + lb.width() / lb.scale;
  const u8 r = static_cast<u8>(std::clamp(warp.rgb[0], 0.0f, 255.0f)), g = static_cast<u8>(std::clamp(warp.rgb[1], 0.0f, 255.0f)),
           b = static_cast<u8>(std::clamp(warp.rgb[2], 0.0f, 255.0f));
  for (float sx = std::floor(left / 80.0f) * 80.0f; sx < right; sx += 80.0f) {
    const float xs[2] = {sx, sx + 80.0f}, ys[2] = {0.0f, hh};
    float x[4], y[4], u[4], v[4];
    for (int k = 0; k < 4; ++k) {
      const float px = xs[k & 1], py = ys[k >> 1];
      project(px, py, x[k], y[k]);
      // The strip samples its own pixels of the source picture.
      u[k] = (lb.ox + px * lb.scale) / fw;
      v[k] = (lb.oy + py * lb.scale_y) / fh;
    }
    particles_.quad(src, warp.abr + 1, x, y, u, v, r, g, b, 0.0f);
  }
  particles_.end();
  if (acc) {
    ctx_->end_target(fx_acc_rt_[fx_acc_cur_]);
    frame.set_scissor(scissor);
    particles_.begin(frame, fw, fh, true);
    particles_.quad(fx_acc_set_[fx_acc_cur_], 0, qx, qy, qu, qv, 128, 128, 128, 0.0f);
    particles_.end();
  }
  frame.set_scissor(frame.full());
}

// An overlay replaces the battle stage (kamei: op 0x101 frees the stage pack's objects and reads the .prd of slot 28 = battle/bg/kamei over it;
// op 0x102 registers the new resources and starts the stage's objects). The new stage stays for the rest of the battle (scene 431,
// Bunyip's second form fights on the kamei field). -1 = free (nothing drawn: the black the pack shows between the ops), 0..99 = bg_NN,
// 100 = battle/bg/kamei, effects::kStageSlot + e = the files of effect slot e (lasbos.prg: the Time Devourer's arenas bg_86..bg_90 / bg_99).
void BattleView::fx_swap_stage(int id) {
  stage_scene_.clear();
  fx_lens_wait_ = 0;
  if (id < 0) {
    log::debug("battle fx: stage freed at tick {}", tick_count_);
    return;
  }
  // kStageSlot + e (lasbos 0x101 / 0x102): the .prd / .ctd of effect slot e (the disc-2 index fills bg_86..bg_99).
  std::string base = id >= 100 ? "battle/bg/kamei" : std::format("battle/bg/bg_{:02d}", id), ctd_path = base + ".ctd";
  if (id >= effects::kStageSlot) {
    const effects::EffectSlot* slot = fxlib_.slot(id - effects::kStageSlot);
    if (!slot || slot->prd.size() <= 4) {
      log::warn("battle fx: stage slot {} has no .prd", id - effects::kStageSlot);
      return;
    }
    base = slot->prd.substr(0, slot->prd.size() - 4);
    ctd_path = slot->ctd;
  }
  const auto prd = vfs_->read("cdrom/" + base + ".prd");
  if (!prd) {
    log::warn("battle fx: stage {} not found", base);
    return;
  }
  const Bytes ctd = ctd_path.empty() ? Bytes{} : vfs_->read("cdrom/" + ctd_path).value_or(Bytes{});
  std::vector<const assets::RoomEffects*> sheet_libs;
  if (fxlib_ready_) {
    if (const auto res = fxlib_.resident())
      for (const auto& resident_pack : res->packs) sheet_libs.push_back(resident_pack.get());
    sheet_libs.push_back(&fxlib_.particle_lib());
  }
  // Every emitter in battle uses position shift 5.
  stage_scene_.set_pos_shift(5);
  stage_pipe_.set_scene_group("battle_stage/" + std::filesystem::path(base).filename().string());
  std::string err;
  if (!stage_scene_.load(*ctx_, stage_pipe_, *prd, ctd, &err, sprites_, std::move(sheet_libs)))
    log::warn("battle fx: stage {}: {}", base, err);
  // The new stage's cameras / timelines take over (0x102 registers its resources; the old stage's were freed at 0x101): after the
  // lasbos4 swap there is ONE stage zen0, the new stage's, ahead of battle.prd's - the battle view frames the new arena from its own zen0.
  // Names only the old stage had stay in the bank.
  bank_.add_drp(*prd, true);
  log::info("battle fx: stage {} loaded at tick {}", base, tick_count_);
}

// Switched live during the command phase: the `bti0` layer restarts (every emitter, as the outro does) or stops as at the intro's end.
void BattleView::set_keep_intro_layer(bool on) {
  if (on == keep_intro_layer_) return;
  keep_intro_layer_ = on;
  if (intro_layer_cut_ && finish_ == Finish::None) fx_stage_layer("bti0", on, on);
}

// A stage layer switched by an effect (ops 0x26 / 0x27 / 0x36-0x3D / 0x5F / 0x60): the stage's emitters of that name. The stage scene owns
// them (stage_scene.*); EmitterSystem::layer_off / layer_on switch every emitter of that name.
void BattleView::fx_stage_layer(std::string_view name, bool on, bool any_duration) {
  effects::EmitterSystem* em = stage_scene_.emitters();
  const int emitter_count = em ? (on ? em->layer_on(name, any_duration) : em->layer_off(name)) : 0;
  log::debug("battle fx: stage layer '{}' {}{} ({} emitter(s))", name, on ? "on" : "off", any_duration ? " (any duration)" : "", emitter_count);
}

// The PS1 frame pacing (a VSync at the end of each frame, T ticks per frame):
// e = vsyncs since the last frame (1..10). m = fx_slow_n_ >= 2 (op 0x79, op 0x56 with s16 @+10 != 0): the frame is padded to a multiple
// of m vsyncs and T = that multiple / m - here one game tick per m video frames. Else k = fx_slow_x_ != 0 (op 0x56 with s16 @+10 == 0):
// d = e - k; d < 1 -> T = 1 and 1 - d vsyncs are owed, spent as a wait (n >= 2 = n vsyncs since the last
// call, 1 -> VSync(0)); else T = d + 1. With one vsync per video frame here: k = 1 changes nothing; k >= 2: a frame measuring e = 1 pays
// k vsyncs, the next one measures e = k, pays 1: 2 game ticks per k + 1 video frames (k = 2: 2 / 3, k = 3: 1 / 2), assuming a PS1
// frame costs one vsync. fx_slow_phase_ (0x56 only) = pending wait frames | (vsyncs counted since the last tick << 8).
bool BattleView::fx_slow_skip() {
  if (fx_slow_n_ >= 2) {
    fx_slow_phase_ = (fx_slow_phase_ + 1) % fx_slow_n_;
    return fx_slow_phase_ != 0;
  }
  if (fx_slow_x_ <= 0) {
    fx_slow_phase_ = 0;
    return false;
  }
  int wait = fx_slow_phase_ & 0xFF, vsyncs = std::max(1, fx_slow_phase_ >> 8);
  // The wait owed by the last game frame is still running.
  if (wait > 0) {
    fx_slow_phase_ = (wait - 1) | ((vsyncs + 1) << 8);
    return true;
  }
  const int surplus = std::min(vsyncs, 10) - fx_slow_x_;
  // The vsyncs owed.
  const int pay = surplus < 1 ? 1 - surplus : 0;
  fx_slow_phase_ = std::max(0, pay - 1) | (1 << 8);
  return false;
}

// 60 Hz presentation of a slowed pack: the frames fx_slow_skip swallows run no tick, so every interpolated thing (camera
// eye / target, unit path, pose, effects) would hold the newer tick's state and move in 1 / n-frame steps (chgtar, element 337, the Bomber's
// own hop, sets slow motion of 8 / 4 / 2 via ops 0x79 / 0x56). The PS1 draws once per padded frame; the port spreads the last tick
// step over the gap instead (the same one-tick-late interpolation as at normal speed): m path = fx_slow_n_ frames, k path = the frames
// already waited (e - 1) plus the wait frames still owed (fx_slow_phase_ & 0xFF). Draw only: the ticks see tick_alpha_.
float BattleView::fx_slow_alpha(float alpha) const {
  int since = 0, gap = 1;
  if (fx_slow_n_ >= 2) {
    since = fx_slow_phase_;
    gap = fx_slow_n_;
  } else if (fx_slow_x_ > 0) {
    const int vsyncs = std::max(1, fx_slow_phase_ >> 8);
    since = vsyncs - 1;
    gap = vsyncs + (fx_slow_phase_ & 0xFF);
  }
  if (gap <= 1) return alpha;
  return std::clamp((static_cast<float>(since) + alpha) / static_cast<float>(gap), 0.0f, 1.0f);
}

void BattleView::fx_tick() {
  // OC_FXDBG: log what can draw a second copy, and the positions of the acting / hit units.
  if (static const bool dbg = std::getenv("OC_FXDBG") != nullptr; dbg && (fx_ || fx_warp_.on))
    log::debug("fxdbg: tick {} pack {} warp {} seq {} shot {}", tick_count_, fx_ != nullptr, fx_warp_.on, camera_.sequence_running(), camera_.task_running());
  if (static const bool dbg2 = std::getenv("OC_FXDBG") != nullptr; dbg2 && act_.actor >= 0 && tick_count_ % 5 == 0) {
    std::string text;
    for (int unit : {act_.actor, act_.targets.empty() ? -1 : act_.targets.front()})
      if (unit >= 0) text += std::format(" u{} ({:.0f},{:.0f}) y {:.0f} face ({:.2f},{:.2f}) kind {} done {}", unit, vis_[static_cast<size_t>(unit)].x, vis_[static_cast<size_t>(unit)].z, vis_[static_cast<size_t>(unit)].y,
                                         vis_[static_cast<size_t>(unit)].face_x, vis_[static_cast<size_t>(unit)].face_z, static_cast<int>(vis_[static_cast<size_t>(unit)].script_kind),
                                         vis_[static_cast<size_t>(unit)].script_done);
    log::debug("fxdbg: tick {} units{}", tick_count_, text);
  }
  // In the command flow: no shot and no timeline running (and no pack) ends the warp.
  if (!fx_ && fx_warp_.on && !camera_.sequence_running() && !camera_.task_running()) fx_warp_.on = false;
  if (fx_lens_wait_ > 0 && --fx_lens_wait_ == 0)
    if (effects::EmitterSystem* se = stage_scene_.emitters()) se->lens_on();
  // The idle camera handler (per frame while no shot / timeline runs), in its command-orbit branch, switches the depth cue
  // flag (op 0x24 .. 0x6C) off and puts every actor back on the plain renderer. A pack that ends with the flag on (Magma Bomb: op 0x24,
  // the far colour ramped to (80, 80, 80) with near / far 0 / 0 = the last valid pair 137 / 378) would otherwise leave the whole field fogged dark.
  // The fog values stay (only the flag is reset); the next pack start clears the flag too.
  if (!fx_ && fx_fog_on_ && act_.actor < 0 && ui_ != Ui::Wait && ui_ != Ui::Intro && ui_ != Ui::Result) {
    fx_state_log("command input opens");
    fx_fog_on_ = false;
    fx_unit_fog_.fill(false);
  }
  // Once the menu opens with no shot and no timeline alive (and the one-shot flag still set from the battle start), both screen colour
  // layers ramp to 0 over 10 ticks, mode kept; once per battle. Without this an additive pale yellow (bg_41 `pli0` t55 `2B f35 m2`) stayed
  // over the whole battle.
  if (fx_screen_menu_clear_ && ui_ != Ui::Wait && ui_ != Ui::Intro && ui_ != Ui::Result && !camera_.sequence_running() && !camera_.task_running()) {
    fx_screen_menu_clear_ = false;
    assets::SeqCmd cmd;
    // Ticks 10 (s16 @+4), mode 0xFF = keep (@+8), colour 0 (@+9..11).
    cmd.data = {10, 0, 0, 0, 0xFF, 0, 0, 0};
    fx_screen_colour(0, cmd);
    fx_screen_colour(1, cmd);
  }
  fx_fog_tick();
  fx_screen_tick();
  fx_warp_tick();
  fx_ambient_tick();
  fx_spell_light_tick();
  {
    // The ghost flag clears when the action ends.
    const bool presenting = act_.phase != Act::Phase::None || !queue_.empty();
    for (int i = 0; i < battle::kUnits; ++i) {
      const size_t unit_index = static_cast<size_t>(i);
      // A pack-hidden model (kamei's shell form) is the script's again once the action is over.
      if (fx_conceal_[unit_index] && !presenting) fx_conceal_[unit_index] = false;
      if (brain_.hidden(i)) {
        ghost_view_[unit_index] = true;
        // The script's ghost op ran and its action is over: the normal rules draw the unit.
      } else if (ghost_view_[unit_index] && !presenting) {
        ghost_view_[unit_index] = false;
        entry_shown_[unit_index] = false;
      }
    }
  }
  if (!fx_) return;
  // The overlays skin models in their ticks too (spawned shards).
  if (effects::FxModels* ms = fx_->bespoke_models(); ms && !ms->skin) {
    ms->skin = [this](int unit, effects::FxSkinnedModel& out) { return fx_skin_unit(unit, out); };
    ms->attach = [this](int unit, int point, effects::Vec3& out) { return fx_attach_unit(unit, point, out); };
    ms->pseudo_attach = [this](int point, effects::Vec3& out) { return pa_attach(point, out); };
    ms->model_read_ticks = [this](int unit) { return fx_model_read_ticks(unit); };
    ms->skin_pack = [this](effects::FxSkinnedModel& out) { return fx_skin_pack(out); };
    // FxModels::PackDrive requests are consumed here (fx_pack_drive_tick).
    ms->pack_drive.host = true;
  }
  if (effects::FxModels* ms = fx_->bespoke_models()) ms->pad_edge = fx_pad_edge_;
  fx_pad_edge_ = 0;
  fx_->tick(fx_scene());
  // The hand-over runs at the top of a later pass than the op 0x41 (kHandOverDelay).
  if (fx_charge_ && fx_ready_) ++fx_hand_wait_;
  fx_model_requests();
  // The pack's pseudo-actor: its script, mover and pose (battle_view_pseudo.cpp); then the overlay's write of its hidden bit (ghost2).
  pa_tick();
  if (effects::FxModels* ms = fx_->bespoke_models(); ms && ms->pseudo_hide >= 0) {
    pa_.hidden = ms->pseudo_hide != 0;
    ms->pseudo_hide = -1;
  }
  fx_vram_apply();
  fx_cross_tick();
  // darkfeit / lasdes3 0x102 / 0x103.
  if (effects::FxModels* ms = fx_->bespoke_models(); ms && ms->env_clut_set) fx_textures_.stock_env_k = ms->env_clut, ms->env_clut_set = false;
  fx_grey_apply(false);
  // Per frame, while an amplitude is set, the display shifts by (rand16 * A >> 16) + A / 2 on each axis (A = the ramped byte; the
  // y base +10 of the screen offset is the normal centring, already in the picture's crop).
  const int ax = fx_->shake_x(), ay = fx_->shake_y();
  if (ax || ay) {
    const u32 hash = static_cast<u32>(tick_count_) * 2654435761u;
    fx_shake_x_ = ax ? static_cast<int>(((hash >> 8) & 0xFFFFu) * static_cast<u32>(ax) >> 16) + ax / 2 : 0;
    fx_shake_y_ = ay ? static_cast<int>(((hash >> 17 | hash << 15) & 0xFFFFu) * static_cast<u32>(ay) >> 16) + ay / 2 : 0;
  } else {
    fx_shake_x_ = fx_shake_y_ = 0;
  }
  if (fx_->done()) fx_end();
}
}  // namespace oc::game
