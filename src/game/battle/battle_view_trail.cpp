// BattleView: the weapon-trail ribbons of the anmseq scripts (ops 0x23 0x24 0x25 0x46 0x5B 0x85 0x86) and the per-unit model state those
// scripts leave behind (show / hide, part words, colour and fades, scale, eye blink, back-colour and light ramps).
//
// Trail geometry: a sampler per (unit, slot) poses the model at this tick's clip positions, one step per quarter frame, and takes the two attach
// points in world space. Every new node pair with the two before it makes a uniform quadratic B-spline, evaluated at t = 0 and t = 1/2 for both
// ends, which starts two draw segments. A segment owns the unit's trail colours from its start, fades them per tick by the signed rates (it dies
// once both colours are clamped on every channel) and is drawn to the previous segment as two Gouraud triangles (prevB, thisA, prevA) and
// (thisB, thisA, prevB): first with B - F, then with the unit's trail mode (ABR = mode - 1; mode 3 = the subtraction only).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/battle/battle_view.h"

namespace oc::game {

namespace {
using V3 = std::array<float, 3>;
// Spline parameter 1/2 (0x1000 = 1).
constexpr int kSplineHalf = 0x800;
// The ramp tasks step colours in 12.4 fixed point.
constexpr int kColourFixedShift = 4;

// One draw segment: the two spline ends and their fading colours.
struct TrailSegment {
  V3 pos_a{}, pos_b{};
  std::array<int, 3> colour_a{}, colour_b{};
};
// One sampler task: attach points `attach_a` / `attach_b` of `unit`, sampled while active; its segments outlive the sampler until faded.
struct Ribbon {
  int unit = 0, slot = 0, attach_a = 0, attach_b = 0;
  bool active = true;
  // The last three sampled node pairs.
  std::vector<std::array<V3, 2>> nodes;
  std::vector<TrailSegment> segs;
};
}  // namespace

struct BattleView::TrailState {
  struct UnitCfg {
    std::array<int, 3> colour_a{255, 255, 255}, colour_b{255, 255, 255};
    std::array<int, 3> fade{0, 0, 0};
    int mode = 2;
  };
  std::array<UnitCfg, battle::kUnits> cfg{};
  std::vector<Ribbon> ribbons;
  struct Quad {
    // prevA, prevB, thisA, thisB.
    V3 pos[4];
    int colour[4][3];
    int mode;
  };
  // The quads of the last tick.
  std::vector<Quad> quads;
  // Back-colour ramp (op 0x64): one per unit, a new op replaces it; 12.4 fixed-point values with linear steps.
  struct BkRamp {
    int unit = 0, left = 0;
    int cur[3]{}, step[3]{}, to[3]{};
  };
  std::vector<BkRamp> bk;
  // Light ramp (op 0x5E): the slot's intensity walks to the target; `light_count` = the live ramps per slot (only the newest one runs).
  struct LightRamp {
    int slot = 0, left = 0, step = 0;
  };
  std::vector<LightRamp> lights;
  std::array<int, 3> light_count{};
  // The model words a script writes: hidden, semi-transparency, colour, the shading-entry flags, the scale and the running fades.
  struct UnitModel {
    bool hidden = false;
    // Ops 0x7A / 0x79: the attached weapon objects hidden / shown; `weapon_set` = a script wrote the word (else the model's default stands).
    bool weapon_hidden[2] = {false, false};
    bool weapon_set[2] = {false, false};
    u32 part_hide = 0, part_show = 0;
    bool tinted = false;
    int rgb[3]{128, 128, 128};
    int abr = -1;
    struct Fade {
      bool on = false;
      // Op 0x53: a plain colour ramp, not the op 0x8E flash draw.
      bool plain = false;
      int left = 0, cur[3]{}, step[3]{}, to[3]{};
    } fade;
    // Op 0x83: the model scale (4096 = 1.0), a ramp of `scale_left` ticks.
    float scale[3] = {1, 1, 1};
    int scale_left = 0;
    float scale_step[3]{}, scale_to[3] = {1, 1, 1};
    // Op 0x91: alternates two parts; state 0 = part_a hidden and part_b shown after the first toggle.
    struct Blink {
      int part_a = 0, part_b = 0, ticks_a = 0, ticks_b = 0, state = 0, timer = 0;
    };
    std::vector<Blink> blinks;
    // Op 0xA0: per shading entry colour bytes (0x80 = neutral), a ramp of `left` ticks, and the semi-transparency flag + ABR.
    struct PartCol {
      bool on = false, semi = false;
      int abr = 0, left = 0, cur[3]{}, step[3]{}, to[3]{};
    };
    std::array<PartCol, 32> pcol{};
  };
  std::array<UnitModel, battle::kUnits> model{};
  // The model state at rest = what the spawn script (S5 entry 8) left in its first tick: the shading-entry words (ops 0x1B / 0x1C / 0xA0) and the
  // part blink tasks (op 0x91). A cut script falls back to this, not to an all-shown model (anm_unit_reset).
  std::array<UnitModel, battle::kUnits> rest{};
  // Party eye blink (ops 0x72 / 0x73 / 0x4F): the blink flag, the shown frame and the countdown in ticks. Not part of UnitModel: a cut script
  // does not clear it (the next idle script re-arms it).
  struct Eye {
    bool on = false;
    // The unit was KO when last looked at (the blink flag is cleared at the KO and set again on revive).
    bool ko = false;
    // 0 open, 1 half, 2 closed.
    int frame = 0;
    int counter = 0;
  };
  std::array<Eye, battle::kUnits> eye{};
};

// A 12.4 fixed-point colour ramp over `ticks` (>= 1) from `from` to `to`: cur / step in fixed point, the target kept for the last tick.
static void start_colour_ramp(int ticks, const s16 (&from)[3], const s16 (&to)[3], int (&cur)[3], int (&step)[3], int (&target)[3]) {
  for (size_t k = 0; k < 3; ++k) {
    cur[k] = from[k] << kColourFixedShift;
    target[k] = to[k];
    step[k] = ((to[k] - from[k]) * 16) / ticks;
  }
}

// The anmseq events that write a unit's trail configuration / samplers, model words, light slots and back colour.
void BattleView::trail_event(int unit, const bseq::Event& event) {
  if (unit < 0 || unit >= battle::kUnits) return;
  using K = bseq::Event::Kind;
  if (!trail_) trail_ = std::make_shared<TrailState>();
  static const bool trace = std::getenv("OC_ANM_TRACE") != nullptr;
  if (trace) log::info("trail: tick {} unit {} event {} a {} b {} c {}", tick_count_, unit, static_cast<int>(event.kind), event.a, event.b, event.c);
  TrailState& state = *trail_;
  const size_t unit_index = static_cast<size_t>(unit);
  TrailState::UnitCfg& cfg = state.cfg[unit_index];
  TrailState::UnitModel& model = state.model[unit_index];
  const auto party_eyes = [&] { return battle_->unit(unit).pc && battle_->unit(unit).form < 0 && !vis_[unit_index].ko_shown; };
  switch (event.kind) {
    case K::TrailColors:
      for (size_t k = 0; k < 3; ++k) cfg.colour_a[k] = event.off[k], cfg.colour_b[k] = event.ang[k];
      break;
    case K::TrailMode: cfg.mode = event.a; break;
    case K::TrailFade:
      for (size_t k = 0; k < 3; ++k) cfg.fade[k] = event.off[k];
      break;
    case K::TrailOn: {
      // One live sampler per slot.
      for (Ribbon& ribbon : state.ribbons)
        if (ribbon.unit == unit && ribbon.slot == event.a) ribbon.active = false;
      Ribbon ribbon;
      ribbon.unit = unit, ribbon.slot = event.a, ribbon.attach_a = event.b, ribbon.attach_b = event.c;
      state.ribbons.push_back(std::move(ribbon));
      break;
    }
    case K::TrailOff:
      for (Ribbon& ribbon : state.ribbons)
        if (ribbon.unit == unit && ribbon.slot == event.a) ribbon.active = false;
      break;
    case K::ModelShow:
      model.hidden = event.a == 0;
      // The battle-script ghost ops and the anmseq show / hide ops write the same model bit: a show a pack plays on a script-ghosted unit draws
      // it until the action ends, a later hide takes it off again.
      if (ghosted(unit)) entry_shown_[unit_index] = event.a != 0;
      if (event.a != 0) fx_hide({unit}, 1, false);
      break;
    case K::WeaponShow:
      for (int slot = 0; slot < 2; ++slot)
        if (event.b < 0 || event.b == slot) model.weapon_hidden[slot] = event.a == 0, model.weapon_set[slot] = true;
      break;
    // On: only a party member in its own form that is not KO; off: always, with the eyes open again.
    case K::Blink: {
      TrailState::Eye& eye = state.eye[unit_index];
      if (event.a) {
        if (party_eyes()) eye.on = true;
      } else {
        eye.on = false;
        eye.frame = 0;
      }
      break;
    }
    case K::Eye:
      if (party_eyes()) state.eye[unit_index].frame = std::clamp(event.a, 0, 2);
      break;
    case K::LightAbs:
      if (event.a >= 0 && event.a <= 2)
        for (size_t k = 0; k < 3; ++k) fx_lights_.slot[static_cast<size_t>(event.a)].pos[k] = static_cast<s32>(event.off[k]);
      break;
    case K::PartShow: {
      if (event.b < 0 || event.b >= 32) break;
      const u32 bit = 1u << event.b;
      if (event.a) model.part_show |= bit, model.part_hide &= ~bit;
      else model.part_hide |= bit, model.part_show &= ~bit;
      break;
    }
    case K::ModelColor:
      model.tinted = true;
      for (size_t k = 0; k < 3; ++k) model.rgb[k] = event.off[k];
      break;
    case K::ModelAbr: model.abr = event.a == 0 ? -1 : event.a - 1; break;
    case K::ModelScale:  // op 0x83: a new ramp replaces the running one (the packs wait for one before the next)
      for (size_t k = 0; k < 3; ++k) model.scale_to[k] = static_cast<float>(event.off[k]) / 4096.0f;
      if (event.d < 1) {
        for (size_t k = 0; k < 3; ++k) model.scale[k] = model.scale_to[k];
        model.scale_left = 0;
      } else {
        for (size_t k = 0; k < 3; ++k) model.scale_step[k] = (model.scale_to[k] - model.scale[k]) / static_cast<float>(event.d);
        model.scale_left = event.d;
      }
      break;
    case K::ModelFade:  // op 0x53: a plain model colour ramp plus the semi-transparency flag with ABR mode - 1 (mode 0 clears it)
      model.fade = {};
      model.tinted = true;
      model.abr = event.b == 0 ? -1 : event.b - 1;
      if (event.a < 1) {
        for (size_t k = 0; k < 3; ++k) model.rgb[k] = event.ang[k];
        break;
      }
      model.fade.on = model.fade.plain = true;
      model.fade.left = event.a;
      start_colour_ramp(event.a, event.off, event.ang, model.fade.cur, model.fade.step, model.fade.to);
      for (size_t k = 0; k < 3; ++k) model.rgb[k] = event.off[k];
      break;
    case K::PartScale: case K::PartScaleStop: case K::ElemMotion: case K::ElemRot: case K::ElemRotRate: case K::ElemTilt: case K::ElemTiltBack: {
      // element ops = joint overrides of the pose
      Visual& visual = vis_[unit_index];
      if (!visual.has_model) break;
      const float k360 = 360.0f / 4096.0f;
      if ((event.kind == K::ElemTilt || event.kind == K::ElemTiltBack) && std::getenv("OC_FXDBG"))
        log::debug("fxdbg: unit {} PitchDown {} element {} z -> {} (4096 = turn) over {} ticks", unit, event.kind == K::ElemTilt ? "8C" : "8D", event.a, event.off[2], event.d);
      if (event.kind == K::PartScale) visual.pose.joint_scale_to(event.a, {event.off[0] / 4096.0f, event.off[1] / 4096.0f, event.off[2] / 4096.0f}, event.d);
      else if (event.kind == K::PartScaleStop) visual.pose.joint_scale_stop(event.a);
      else if (event.kind == K::ElemMotion) visual.pose.joint_motion(event.a, event.b != 0);
      else if (event.kind == K::ElemTilt) visual.pose.joint_tilt(event.a, event.off[2] * k360, event.d);
      else if (event.kind == K::ElemTiltBack) visual.pose.joint_tilt_back(event.a, event.d);
      else if (event.kind == K::ElemRot) visual.pose.joint_rot_to(event.a, {event.off[0] * k360, event.off[1] * k360, event.off[2] * k360}, event.d);
      else visual.pose.joint_rot_rate(event.a, {event.off[0] * k360, event.off[1] * k360, event.off[2] * k360});
      break;
    }
    case K::PartColor: {
      if (event.a < 0 || event.a >= 32) break;
      TrailState::UnitModel::PartCol& part = model.pcol[static_cast<size_t>(event.a)];
      part.on = true;
      part.semi = event.b != 0;
      part.abr = event.b > 0 ? event.b - 1 : 0;
      if (event.d < 1) {
        for (size_t k = 0; k < 3; ++k) part.cur[k] = event.ang[k] << kColourFixedShift, part.to[k] = event.ang[k];
        part.left = 0;
      } else {
        part.left = event.d;
        start_colour_ramp(event.d, event.off, event.ang, part.cur, part.step, part.to);
      }
      break;
    }
    case K::PartBlink:  // op 0x91: a task per part pair; an older one of the same pair is freed first
      std::erase_if(model.blinks, [&](const TrailState::UnitModel::Blink& blink) { return blink.part_a == event.a && blink.part_b == event.b; });
      model.blinks.push_back({event.a, event.b, event.c, event.d, 0, 0});
      break;
    case K::PartBlinkStop:
      std::erase_if(model.blinks, [&](const TrailState::UnitModel::Blink& blink) { return blink.part_a == event.a && blink.part_b == event.b; });
      break;
    case K::ColorRamp: {
      // Op 0x8E: the soft fade of the model colour, drawn with the flash routine while it runs.
      if (event.b != 0) {
        model.fade = {};
        model.tinted = true;
        if (event.a < 1) {
          for (size_t k = 0; k < 3; ++k) model.rgb[k] = event.ang[k];
          break;
        }
        model.fade.on = true;
        model.fade.left = event.a;
        start_colour_ramp(event.a, event.off, event.ang, model.fade.cur, model.fade.step, model.fade.to);
        for (size_t k = 0; k < 3; ++k) model.rgb[k] = event.off[k];
        break;
      }
      std::erase_if(state.bk, [&](const TrailState::BkRamp& ramp) { return ramp.unit == unit; });
      if (event.a < 1) {
        for (size_t k = 0; k < 3; ++k) fx_ambient_.cur[k] = event.ang[k];
        fx_ambient_.ramping = false;
        break;
      }
      TrailState::BkRamp ramp;
      ramp.unit = unit, ramp.left = event.a;
      start_colour_ramp(event.a, event.off, event.ang, ramp.cur, ramp.step, ramp.to);
      state.bk.push_back(ramp);
      break;
    }
    // Light slot := an attach point of the unit running the script.
    case K::LightPos: {
      effects::Vec3 point{};
      if (event.c < 0 || event.c > 2 || !fx_attach_unit(unit, event.b, point)) break;
      for (size_t k = 0; k < 3; ++k) fx_lights_.slot[static_cast<size_t>(event.c)].pos[k] = static_cast<s32>(point[k]);
      break;
    }
    case K::LightSet: {
      if (event.a < 0 || event.a > 2) break;
      effects::LightTable::Slot& slot = fx_lights_.slot[static_cast<size_t>(event.a)];
      for (size_t k = 0; k < 3; ++k) slot.rgb[k] = static_cast<u8>(event.off[k]);
      slot.falloff = event.b;
      slot.intensity = event.c;
      TrailState::LightRamp ramp;
      ramp.slot = event.a;
      ramp.left = std::max(1, static_cast<int>(event.ang[0]));
      ramp.step = (event.d - event.c) / ramp.left;
      state.lights.push_back(ramp);
      ++state.light_count[static_cast<size_t>(event.a)];
      fx_lit_lists(true);
      break;
    }
    default: break;
  }
}

// The uniform quadratic B-spline of three nodes at t (0x1000 = 1), per component, floored like the fixed-point original.
static V3 spline(const V3& p0, const V3& p1, const V3& p2, int param) {
  const int inv = 0x1000 - param;
  const int w0 = (inv * inv * 8) >> 12, w1 = ((inv * param) >> 8) + 0x8000, w2 = (param * param * 8) >> 12;
  V3 out{};
  for (size_t k = 0; k < 3; ++k)
    out[k] = std::floor((p0[k] * static_cast<float>(w0) + p1[k] * static_cast<float>(w1) + p2[k] * static_cast<float>(w2)) / 65536.0f);
  return out;
}

// colour += rate per channel, clamped to 0..255; true when every channel hit a bound.
static bool fade_colour(std::array<int, 3>& colour, const std::array<int, 3>& rate) {
  int hit = 0;
  for (size_t k = 0; k < 3; ++k) {
    int value = colour[k] + rate[k];
    if (value < 0) value = 0, hit |= 1 << k;
    else if (value > 255) value = 255, hit |= 1 << k;
    colour[k] = value;
  }
  return hit == 7;
}

// One tick of every script-started task: the party eye blink, the back-colour ramps, the model scale / part colour / part blink / colour fade
// tasks, the light ramps, then the trail samplers (new nodes, segments) and the segment fades that produce this tick's quads.
void BattleView::trail_tick() {
  if (!trail_) return;
  TrailState& state = *trail_;
  for (int unit = 0; unit < battle::kPcSlots; ++unit) {
    TrailState::Eye& eye = state.eye[static_cast<size_t>(unit)];
    const bool ko = battle_->unit(unit).pc && battle_->unit(unit).form < 0 && vis_[static_cast<size_t>(unit)].ko_shown;
    if (ko != eye.ko) {
      eye.ko = ko;
      if (ko) eye.on = false, eye.frame = 2;
    }
    if (!eye.on) continue;
    if ((eye.counter -= 1) > 0) continue;
    static const bool trace = std::getenv("OC_ANM_TRACE") != nullptr;
    if (trace) log::info("trail: tick {} unit {} eye frame {} -> toggle", tick_count_, unit, eye.frame);
    // Eyes open: shut them for 6 ticks.
    if (eye.frame == 0) {
      eye.counter = 6;
      eye.frame = 2;
    } else {
      // Open them for (rand % 60) * (rand % 6 + 4) ticks.
      const int open_base = std::rand() % 60;
      const int open_mul = std::rand() % 6 + 4;
      eye.counter = open_base * open_mul;
      eye.frame = 0;
    }
  }
  for (TrailState::BkRamp& ramp : state.bk) {
    if (--ramp.left <= 0) {
      for (size_t k = 0; k < 3; ++k) fx_ambient_.cur[k] = ramp.to[k];
    } else {
      for (size_t k = 0; k < 3; ++k) {
        ramp.cur[k] += ramp.step[k];
        fx_ambient_.cur[k] = ramp.cur[k] >> kColourFixedShift;
      }
    }
    fx_ambient_.ramping = false;
  }
  std::erase_if(state.bk, [](const TrailState::BkRamp& ramp) { return ramp.left <= 0; });
  for (TrailState::UnitModel& model : state.model) {
    if (model.scale_left > 0) {
      for (size_t k = 0; k < 3; ++k) model.scale[k] += model.scale_step[k];
      if (--model.scale_left == 0)
        for (size_t k = 0; k < 3; ++k) model.scale[k] = model.scale_to[k];
    }
    for (TrailState::UnitModel::PartCol& part : model.pcol) {
      if (part.left <= 0) continue;
      for (size_t k = 0; k < 3; ++k) part.cur[k] = std::clamp(part.cur[k] + part.step[k], 0, 0xFF0);
      if (--part.left == 0)
        for (size_t k = 0; k < 3; ++k) part.cur[k] = part.to[k] << kColourFixedShift;
    }
    // When the timer runs below 0 the shown part flips.
    for (TrailState::UnitModel::Blink& blink : model.blinks) {
      if (--blink.timer >= 0) continue;
      if (blink.state == 0) {
        blink.state = 1;
        blink.timer += blink.ticks_b;
        if (blink.part_a >= 0 && blink.part_a < 32) model.part_hide |= 1u << blink.part_a, model.part_show &= ~(1u << blink.part_a);
        if (blink.part_b >= 0 && blink.part_b < 32) model.part_show |= 1u << blink.part_b, model.part_hide &= ~(1u << blink.part_b);
      } else {
        blink.state = 0;
        blink.timer += blink.ticks_a;
        if (blink.part_b >= 0 && blink.part_b < 32) model.part_hide |= 1u << blink.part_b, model.part_show &= ~(1u << blink.part_b);
        if (blink.part_a >= 0 && blink.part_a < 32) model.part_show |= 1u << blink.part_a, model.part_hide &= ~(1u << blink.part_a);
      }
      // Never more than one flip per tick.
      if (blink.timer < 0) blink.timer = blink.state == 0 ? blink.ticks_a : blink.ticks_b;
    }
    if (!model.fade.on) continue;
    for (size_t k = 0; k < 3; ++k) {
      model.fade.cur[k] = std::clamp(model.fade.cur[k] + model.fade.step[k], 0, 0xFF0);
      model.rgb[k] = model.fade.cur[k] >> kColourFixedShift;
    }
    if (--model.fade.left <= 0) {
      for (size_t k = 0; k < 3; ++k) model.rgb[k] = model.fade.to[k];
      const bool plain = model.fade.plain;
      model.fade.on = false;
      model.fade.plain = false;
      // The op 0x8E fade ends semi-transparent drawing; op 0x53 leaves its mode.
      if (!plain) model.abr = -1;
    }
  }
  for (TrailState::LightRamp& ramp : state.lights) {
    int& count = state.light_count[static_cast<size_t>(ramp.slot)];
    // A newer ramp of this slot running ends this one.
    if (count != 1) {
      --count;
      ramp.left = 0;
      continue;
    }
    effects::LightTable::Slot& slot = fx_lights_.slot[static_cast<size_t>(ramp.slot)];
    slot.intensity = std::max(0, slot.intensity + ramp.step);
    if (--ramp.left <= 0) {
      --count;
      fx_lit_lists(false);
    }
  }
  std::erase_if(state.lights, [](const TrailState::LightRamp& ramp) { return ramp.left <= 0; });
  state.quads.clear();
  for (Ribbon& ribbon : state.ribbons) {
    const TrailState::UnitCfg& cfg = state.cfg[static_cast<size_t>(ribbon.unit)];
    // Nodes at the clip positions of this tick, one per quarter frame.
    if (ribbon.active) {
      const Visual& visual = vis_[static_cast<size_t>(ribbon.unit)];
      if (!visual.has_model || !battle_->unit(ribbon.unit).present) {
        ribbon.active = false;
      } else {
        const float cur = visual.pose.frame_cur(), prev = visual.pose.frame_prev();
        int samples = static_cast<int>(std::lround(visual.pose.step_per_tick() * 4.0f));
        if (ribbon.nodes.empty()) samples = 1;
        else if (cur == prev) samples = 0;
        render::Mat4 flip;
        flip.at(1, 1) = -1.0f;
        const render::Mat4 model = render::mat4_translation(visual.x, visual.y, visual.z) * render::mat4_rotation_y(std::atan2(-visual.face_x, -visual.face_z)) * flip;
        const auto world = [&](int attach, float frame, V3& out) {
          float x, y, z;
          if (!visual.pose.attach_at(attach, frame, x, y, z)) return false;
          V3 world_pos{};
          for (int k = 0; k < 3; ++k) world_pos[static_cast<size_t>(k)] = model.at(k, 0) * x + model.at(k, 1) * y + model.at(k, 2) * z + model.at(k, 3);
          out = {world_pos[0], -world_pos[1], world_pos[2]};
          return true;
        };
        for (int i = 0; i < samples; ++i) {
          std::array<V3, 2> node{};
          const float frame = cur - 0.25f * static_cast<float>(samples - 1 - i);
          if (!world(ribbon.attach_a, frame, node[0]) || !world(ribbon.attach_b, frame, node[1])) {
            ribbon.active = false;
            break;
          }
          ribbon.nodes.push_back(node);
          if (ribbon.nodes.size() > 3) ribbon.nodes.erase(ribbon.nodes.begin());
          if (ribbon.nodes.size() == 3) {
            for (const int spline_t : {0, kSplineHalf}) {
              TrailSegment seg;
              seg.pos_a = spline(ribbon.nodes[0][0], ribbon.nodes[1][0], ribbon.nodes[2][0], spline_t);
              seg.pos_b = spline(ribbon.nodes[0][1], ribbon.nodes[1][1], ribbon.nodes[2][1], spline_t);
              seg.colour_a = cfg.colour_a, seg.colour_b = cfg.colour_b;
              ribbon.segs.push_back(seg);
            }
          }
        }
      }
    }
    // per segment: fade, draw the quad to the previous segment, die when both colours are used up
    const TrailSegment* prev = nullptr;
    std::vector<TrailSegment> keep;
    keep.reserve(ribbon.segs.size());
    for (TrailSegment& seg : ribbon.segs) {
      const bool done_a = fade_colour(seg.colour_a, cfg.fade), done_b = fade_colour(seg.colour_b, cfg.fade);
      if (prev) {
        TrailState::Quad quad{};
        quad.pos[0] = prev->pos_a, quad.pos[1] = prev->pos_b, quad.pos[2] = seg.pos_a, quad.pos[3] = seg.pos_b;
        for (size_t k = 0; k < 3; ++k)
          quad.colour[0][k] = prev->colour_a[k], quad.colour[1][k] = prev->colour_b[k], quad.colour[2][k] = seg.colour_a[k], quad.colour[3][k] = seg.colour_b[k];
        quad.mode = cfg.mode;
        state.quads.push_back(quad);
      }
      if (!(done_a && done_b)) keep.push_back(seg);
      prev = &seg;
    }
    ribbon.segs = std::move(keep);
  }
  std::erase_if(state.ribbons, [](const Ribbon& ribbon) { return !ribbon.active && ribbon.segs.empty(); });
}

// Each trail quad as two Gouraud triangles (prevB, thisA, prevA) and (thisB, thisA, prevB) through the particle batch, at the OT depth of the
// triangle's mean z: first B - F, then the unit's mode (ABR = mode - 1; 1 average, 2 add, 3 subtract only, 4 add / 4).
void BattleView::trail_draw(const render::Frame& frame, const render::Letterbox& lb) {
  if (!trail_ || trail_->quads.empty() || !white_.set) return;
  const effects::ViewParams view = fx_view();
  particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), fx_particle_pass_);
  fx_particle_pass_ = true;
  struct ScreenPoint {
    float x, y, z;
  };
  const auto project = [&](const V3& world, ScreenPoint& out) {
    const effects::Vec3 cam = view.rot.apply({world[0], world[1], world[2]});
    const float z = cam[2] + view.trans[2];
    if (z < 1.0f) return false;
    out.x = lb.ox + (view.ox + view.h * (cam[0] + view.trans[0]) / z) * lb.scale;
    out.y = lb.oy + (view.oy + view.h * (cam[1] + view.trans[1]) / z) * lb.scale_y;
    out.z = z;
    return true;
  };
  // Quad corners of the two triangles, drawn as degenerate quads (a, b, c, c).
  static const int kTri[2][3] = {{1, 2, 0}, {3, 2, 1}};
  const float u[4] = {0, 1, 0, 1}, v[4] = {0, 0, 1, 1};
  for (const TrailState::Quad& quad : trail_->quads) {
    ScreenPoint screen[4];
    bool ok = true;
    for (int k = 0; k < 4; ++k) ok = ok && project(quad.pos[k], screen[k]);
    if (!ok) continue;
    for (const auto& tri : kTri) {
      float x[4], y[4];
      u8 rgb[4][3];
      for (int k = 0; k < 4; ++k) {
        const int corner = tri[std::min(k, 2)];
        x[k] = screen[corner].x, y[k] = screen[corner].y;
        // The particle shader doubles the modulation.
        for (int ch = 0; ch < 3; ++ch) rgb[k][ch] = static_cast<u8>(std::clamp(quad.colour[corner][ch], 0, 255) / 2);
      }
      const float z = (screen[tri[0]].z + screen[tri[1]].z + screen[tri[2]].z) / 3.0f;
      const float depth = std::clamp(static_cast<float>(static_cast<int>(z) >> 4), 0.0f, 4095.0f) / render::kDepthRange;
      if (quad.mode != 3) particles_.quad(white_.set, 3, x, y, u, v, rgb, depth);
      particles_.quad(white_.set, std::clamp(quad.mode, 1, 4), x, y, u, v, rgb, depth);
    }
  }
  particles_.end();
}

bool BattleView::anm_hidden(int unit) const {
  return trail_ && unit >= 0 && unit < battle::kUnits && trail_->model[static_cast<size_t>(unit)].hidden;
}

// Dev unit log (OC_FX_UNITS): the model colour, semi-transparency and eye-blink state the anmseq scripts left on a unit.
void BattleView::anm_unit_probe(int unit, bool& semi, float rgb[3], bool& blink) const {
  semi = false;
  blink = trail_ && unit >= 0 && unit < battle::kUnits && trail_->eye[static_cast<size_t>(unit)].on;
  for (int k = 0; k < 3; ++k) rgb[k] = 128.0f;
  if (!trail_ || unit < 0 || unit >= battle::kUnits) return;
  const TrailState::UnitModel& model = trail_->model[static_cast<size_t>(unit)];
  if (model.tinted)
    for (int k = 0; k < 3; ++k) rgb[k] = static_cast<float>(model.rgb[k]);
  semi = (model.fade.on && !model.fade.plain) || model.abr >= 0;
}

// Dev unit log: the hidden shading entries as drawn (`base` = the model data's own hide flags, then the script hides / shows).
unsigned BattleView::anm_part_hide(int unit, unsigned base) const {
  if (!trail_ || unit < 0 || unit >= battle::kUnits) return base;
  const TrailState::UnitModel& model = trail_->model[static_cast<size_t>(unit)];
  return (base | model.part_hide) & ~model.part_show;
}

// The hide word of the attached-object slot: what a script wrote, else what the model's S5 entry 8 (the battle-start script) hides first.
bool BattleView::anm_weapon_hidden(int unit, int slot) const {
  if (unit < 0 || unit >= battle::kUnits || slot < 0 || slot > 1) return false;
  if (trail_ && trail_->model[static_cast<size_t>(unit)].weapon_set[slot]) return trail_->model[static_cast<size_t>(unit)].weapon_hidden[slot];
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  return visual.scripts && visual.scripts->has_s5 && ((visual.scripts->s5.initial_weapon_hide() >> slot) & 1) != 0;
}

// A script that is stopped before its end never runs the ops that undo its model state (semi-transparency, colour, hidden parts, model hide) or
// its trail-off, so the unit falls back to its rest state (the spawn script's part words). What stays regardless: the attached-object hide
// words (plain model words), the spawn script's element tasks. With keep_colour (the script ran out): the model colour it set, its hide bit, and
// a monster it left hidden at colour 0 keeps its KO fade at 0 (the fade ramps from the current colour, so nothing shows). With keep_parts
// (nothing was cut): the shading-entry words and the part tasks that write them.
void BattleView::anm_unit_reset(int unit, bool keep_colour, bool keep_parts) {
  if (!trail_ || unit < 0 || unit >= battle::kUnits) return;
  TrailState::UnitModel& model = trail_->model[static_cast<size_t>(unit)];
  const TrailState::UnitModel keep = model;
  model = trail_->rest[static_cast<size_t>(unit)];
  if (keep_colour && keep.tinted && !keep.fade.on) {
    model.tinted = true;
    for (int k = 0; k < 3; ++k) model.rgb[k] = keep.rgb[k];
  }
  if (keep_colour) model.hidden = keep.hidden;
  if (keep_colour && model.tinted && model.hidden && model.rgb[0] == 0 && model.rgb[1] == 0 && model.rgb[2] == 0 && !battle_->unit(unit).pc)
    vis_[static_cast<size_t>(unit)].fade = vis_[static_cast<size_t>(unit)].fade_prev = 0.0f;
  for (int slot = 0; slot < 2; ++slot) model.weapon_hidden[slot] = keep.weapon_hidden[slot], model.weapon_set[slot] = keep.weapon_set[slot];
  if (keep_parts) model.part_hide = keep.part_hide, model.part_show = keep.part_show, model.blinks = keep.blinks, model.pcol = keep.pcol;
  vis_[static_cast<size_t>(unit)].pose.clear_joint_overrides(vis_[static_cast<size_t>(unit)].spawn_joints);
  for (Ribbon& ribbon : trail_->ribbons)
    if (ribbon.unit == unit) ribbon.active = false;
  if (std::getenv("OC_FXDBG")) log::debug("fxdbg: unit {} script cut: model state reset (parts {})", unit, keep_parts ? "kept" : "cleared");
}

void BattleView::anm_set_hidden(int unit, bool hidden) {
  if (unit < 0 || unit >= battle::kUnits) return;
  if (!trail_) trail_ = std::make_shared<TrailState>();
  trail_->model[static_cast<size_t>(unit)].hidden = hidden;
}

void BattleView::anm_eye_arm(int unit) {
  if (unit < 0 || unit >= battle::kPcSlots) return;
  if (!trail_) trail_ = std::make_shared<TrailState>();
  if (battle_->unit(unit).pc && battle_->unit(unit).form < 0 && !vis_[static_cast<size_t>(unit)].ko_shown) trail_->eye[static_cast<size_t>(unit)].on = true;
}

// The spawn script's part words. The battle start plays S5 entry 8 at once; its first tick (the ops before its first wait) hides / shows
// shading entries and arms part blinks that last the whole battle. This first-tick pass at the model bind records those words as the `rest`
// state a cut script returns to; the whole entry 8 plays from the battle start separately (a form change rebinds and repeats the pass).
void BattleView::anm_spawn_parts(int unit) {
  if (unit < 0 || unit >= battle::kUnits) return;
  if (!trail_) trail_ = std::make_shared<TrailState>();
  const size_t unit_index = static_cast<size_t>(unit);
  trail_->model[unit_index] = trail_->rest[unit_index] = TrailState::UnitModel{};
  const Visual& visual = vis_[unit_index];
  if (!visual.has_model || !visual.scripts || !visual.scripts->has_s5 || brain_.hidden(unit)) return;
  bseq::Vm vm;
  if (!vm.start(&visual.scripts->s5, 8, 0)) return;
  bseq::Ctx ctx;
  fill_ctx(unit, ctx);
  std::vector<bseq::Event> events;
  vm.tick(ctx, events);
  using K = bseq::Event::Kind;
  for (const bseq::Event& event : events)
    if (event.kind == K::PartShow || event.kind == K::PartBlink || event.kind == K::PartBlinkStop || event.kind == K::PartColor) trail_event(unit, event);
  const TrailState::UnitModel& model = trail_->model[unit_index];
  TrailState::UnitModel& rest = trail_->rest[unit_index];
  rest.part_hide = model.part_hide, rest.part_show = model.part_show, rest.blinks = model.blinks, rest.pcol = model.pcol;
  if (model.part_hide || model.part_show || !model.blinks.empty())
    log::debug("battle: unit {} spawn script (S5 entry 8) part words: hide {:x} show {:x} blinks {}", unit, model.part_hide, model.part_show, model.blinks.size());
}

// The model state the scripts left on a unit, applied to its draw: false = the model is hidden.
bool BattleView::anm_unit_apply(int unit, render::MeshPipeline::DrawParams& dp) const {
  if (!trail_ || unit < 0 || unit >= battle::kUnits) return true;
  const TrailState::UnitModel& model = trail_->model[static_cast<size_t>(unit)];
  if (model.hidden) return false;
  dp.eye_frame = trail_->eye[static_cast<size_t>(unit)].frame;
  dp.hidden_meshes = (dp.hidden_meshes | model.part_hide) & ~model.part_show;
  if (model.tinted)
    for (size_t k = 0; k < 3; ++k) dp.tint[k] *= static_cast<float>(model.rgb[k]) / 128.0f;
  // The flash draw: B - F (white CLUT) then B + F, both with the model colour.
  if (model.fade.on && !model.fade.plain) dp.flash = true;
  else if (model.abr >= 0) dp.model_abr = model.abr;
  for (size_t k = 0; k < 32; ++k) {
    const TrailState::UnitModel::PartCol& part = model.pcol[k];
    if (!part.on) continue;
    dp.part_mask |= 1u << k;
    for (size_t ch = 0; ch < 3; ++ch) dp.part_tint[k][ch] = static_cast<float>(part.cur[ch] >> kColourFixedShift) / 128.0f;
    if (part.semi) dp.part_semi |= 1u << k, dp.part_abr[k] = part.abr;
  }
  // Scaled in the model's own axes, before the y mirror.
  if (model.scale[0] != 1.0f || model.scale[1] != 1.0f || model.scale[2] != 1.0f) {
    render::Mat4 scale;
    scale.at(0, 0) = model.scale[0], scale.at(1, 1) = model.scale[1], scale.at(2, 2) = model.scale[2];
    dp.model = dp.model * scale;
  }
  return true;
}

void BattleView::anm_scale_point(int unit, float& x, float& y, float& z) const {
  if (!trail_ || unit < 0 || unit >= battle::kUnits) return;
  const TrailState::UnitModel& model = trail_->model[static_cast<size_t>(unit)];
  x *= model.scale[0], y *= model.scale[1], z *= model.scale[2];
}

void BattleView::trail_eye_reset(int unit) {
  if (trail_ && unit >= 0 && unit < battle::kPcSlots) trail_->eye[static_cast<size_t>(unit)] = {};
}

}  // namespace oc::game
