#include "game/effects/emitter_internal.h"

#include <algorithm>
#include <cstring>

namespace oc::effects {

using namespace emitter_detail;

// -- header stepper -------------------------------------------------------------------------------------
void EmitterSystem::Impl::step_header(size_t hi) {
  Header& header = headers[hi];
  const assets::Emitter& emitter = *header.def;
  header.frame = static_cast<s16>(header.frame + 1);
  if (header.frame < 0) header.frame = static_cast<s16>(emitter.start + 1);
  auto fire_check = [&]() {
    header.countdown = static_cast<s16>(header.countdown - 1);
    if (header.countdown != -1) return;
    if (emitter.repeat >= 0)
      for (int k = 0; k <= emitter.repeat; ++k) fire(hi, nullptr);
    header.countdown = static_cast<s16>(emitter.period);
    if (emitter.jitter() != 0) header.countdown = static_cast<s16>(header.countdown + 2 * (rng.next() % static_cast<int>(emitter.jitter())));
  };
  if (emitter.start == 0 && emitter.duration == 0) {
    fire_check();
  } else if (header.frame > emitter.start) {
    if (emitter.duration == 0 || header.frame < emitter.start + emitter.duration) fire_check();
    else {
      header.linked = false;
      header.stopped = true;
    }
  }
}

// -- persistent object replay -------------------------------------------------------------
void EmitterSystem::Impl::replay(Object& obj, Header& header, int ticks) {
  header.frame = static_cast<s16>(header.frame + ticks);
  const u32 obj_flags = obj.flags;
  s16 sv_ang[3], sv_scale[3];
  u16 sv_col[3];
  std::memcpy(sv_ang, obj.ang, sizeof sv_ang);
  std::memcpy(sv_scale, obj.scale, sizeof sv_scale);
  std::memcpy(sv_col, obj.col, sizeof sv_col);
  const s16 fr = header.frame;
  for (const auto& op : header.def->ops) {
    switch (op.op) {
      case 0x08:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const auto* pv = header.patched(0x08);
          obj.pos[0] = shl32(pv ? (*pv)[0] : xyz->x, 16); obj.pos[1] = shl32(pv ? (*pv)[1] : xyz->y, 16);
          obj.pos[2] = shl32(pv ? (*pv)[2] : xyz->z, 16);
        }
        break;
      case 0x09:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg))
          for (int i = 0; i < 3; ++i) obj.pos[i] = add32(obj.pos[i], osc_value(osc->axis[static_cast<size_t>(i)], fr));
        break;
      case 0x12:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) { obj.scale[0] = xyz->x; obj.scale[1] = xyz->y; obj.scale[2] = xyz->z; }
        break;
      case 0x14:
      case 0x16:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          s16* tgt = op.op == 0x14 ? obj.scale : obj.srate;
          for (int i = 0; i < 3; ++i) tgt[i] = add16(tgt[i], osc_value(osc->axis[static_cast<size_t>(i)], fr) >> 16);
        }
        break;
      case 0x19:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) { obj.ang[0] = xyz->x; obj.ang[1] = xyz->y; obj.ang[2] = xyz->z; }
        break;
      case 0x1B:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) { obj.arate[0] = xyz->x; obj.arate[1] = xyz->y; obj.arate[2] = xyz->z; }
        break;
      case 0x1C:
      case 0x1F:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          s16* tgt = op.op == 0x1C ? obj.ang : obj.arate;
          for (int i = 0; i < 3; ++i) tgt[i] = add16(tgt[i], osc_value(osc->axis[static_cast<size_t>(i)], fr) >> 16);
        }
        break;
      case 0x20:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) { obj.col[0] = static_cast<u16>(xyz->x); obj.col[1] = static_cast<u16>(xyz->y); obj.col[2] = static_cast<u16>(xyz->z); }
        break;
      case 0x21:
      case 0x23:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          u16* tgt = op.op == 0x21 ? obj.col : obj.crate;
          for (int i = 0; i < 3; ++i) tgt[i] = static_cast<u16>(tgt[i] + (osc_value(osc->axis[static_cast<size_t>(i)], fr) >> 16));
        }
        break;
      case 0x22:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) { obj.crate[0] = static_cast<u16>(xyz->x); obj.crate[1] = static_cast<u16>(xyz->y); obj.crate[2] = static_cast<u16>(xyz->z); }
        break;
      default: break;
    }
  }
  if (obj_flags & kFlagInheritAngle) std::memcpy(obj.ang, sv_ang, sizeof sv_ang);
  if (obj_flags & kFlagInheritScale) std::memcpy(obj.scale, sv_scale, sizeof sv_scale);
  if (obj_flags & kFlagInheritColor) std::memcpy(obj.col, sv_col, sizeof sv_col);
  build_matrix(obj);
}

// -- per-object update (one frame of `ticks` ticks) --------------------------------------------------------
// Each rate term is rate * ticks saturated to s16, then added with wrap-around.
static void apply_rates(Object& obj, int ticks) {
  auto term = [ticks](int rate) { return std::clamp(rate * ticks, -0x8000, 0x7FFF); };
  for (int i = 0; i < 3; ++i) {
    obj.col[i] = static_cast<u16>(obj.col[i] + term(static_cast<s16>(obj.crate[i])));
    obj.ang[i] = add16(obj.ang[i], term(obj.arate[i]));
    obj.scale[i] = add16(obj.scale[i], term(obj.srate[i]));
  }
}

void EmitterSystem::Impl::update(size_t idx, int ticks) {
  for (int i = 0; i < 3; ++i) {
    objs[idx].ppos[i] = add32(objs[idx].pos[i], objs[idx].born_shift[i]);
    objs[idx].born_shift[i] = 0;
    objs[idx].pscale[i] = objs[idx].scale[i];
    objs[idx].pang[i] = objs[idx].ang[i];
    objs[idx].pcol[i] = objs[idx].col[i];
  }
  Object& obj = objs[idx];
  if (obj.life == 0) {
    if (obj.owner >= 0) replay(obj, headers[static_cast<size_t>(obj.owner)], ticks);
  } else {
    const int remaining = static_cast<s16>(obj.life);
    const int nl = static_cast<s16>(obj.life - ticks);
    obj.life = static_cast<u16>(nl);
    if (nl <= 0) {
      // An object with a death child first runs the ticks it had left, so the child starts from the position after them.
      const bool spawns = !(obj.flags & kFlagNoDeathSpawn);
      if (spawns && obj.death >= 0 && remaining > 0) {
        apply_rates(obj, remaining);
        integrate(obj, cfg.pos_shift, remaining);
      }
      die(idx, spawns);
      return;
    }
  }
  apply_rates(obj, ticks);
  // The model's motion advances by ticks * rate / 256 keyframes.
  if (obj.stype == 20) {
    obj.motion_prev = obj.motion;
    obj.motion += static_cast<float>(obj.motion_rate * ticks) / 256.0f;
  }
  integrate(obj, cfg.pos_shift, ticks);
  if ((obj.flags & kFlagFloor) && (obj.flags & kFlagNoDeathSpawn) && shl32(static_cast<s32>(obj.floor_y) << cfg.pos_shift, 16) < obj.pos[1]) {
    // Flag bit 27: dies on the floor hit, with its death spawn.
    die(idx, true);
    return;
  }
  if (obj.flags & kFlagExt) ext_step(obj, ticks);
  build_matrix(obj);
  // Sheet animation: advance the frame every `anim_per` ticks.
  if (obj.anim_per != 0) {
    for (int tick = 0; tick < ticks; ++tick) {
      obj.anim_ctr = static_cast<u8>(obj.anim_ctr - 1);
      if (obj.anim_ctr == 0) {
        obj.anim_ctr = obj.anim_per;
        obj.frame = static_cast<s16>(obj.frame + 1);
      }
    }
    // A frame the sheet does not have resets the frame to 1, so the animation loops.
    if (obj.sheet && (obj.stype == 3 || obj.stype == 21) && obj.frame > 0 && static_cast<size_t>(obj.frame) > obj.sheet->frames.size()) obj.frame = 1;
  }
  if (obj.periodic >= 0) periodic_spawn(idx, ticks);
}

// Extended object block step: sub-word accumulation of the scale / angle rates, UV scroll, morph phase and path ride.
void EmitterSystem::Impl::ext_step(Object& obj, int ticks) {
  if (obj.ext & 0x80000) {
    for (int i = 0; i < 3; ++i) {
      s32 comb = static_cast<s32>((static_cast<u32>(static_cast<u16>(obj.srate[i])) << 16) | static_cast<u16>(obj.sub_s[i]));
      for (int tick = 0; tick < ticks; ++tick) comb = add32(comb, obj.acc_s[i] * 0x20);
      obj.srate[i] = static_cast<s16>(comb >> 16);
      obj.sub_s[i] = static_cast<s16>(comb & 0xFFFF);
    }
  }
  if (obj.ext & 0x100000) {
    for (int i = 0; i < 3; ++i) {
      s32 comb = static_cast<s32>((static_cast<u32>(static_cast<u16>(obj.arate[i])) << 16) | static_cast<u16>(obj.sub_a[i]));
      for (int tick = 0; tick < ticks; ++tick) comb = add32(comb, obj.acc_a[i] * 0x20);
      obj.arate[i] = static_cast<s16>(comb >> 16);
      obj.sub_a[i] = static_cast<s16>(comb & 0xFFFF);
    }
  }
  obj.uv_phase_prev[0] = obj.uv_phase[0];
  obj.uv_phase_prev[1] = obj.uv_phase[1];
  if (obj.ext & 1) {
    obj.uv_phase[0] += obj.uv_rate[0] * ticks;
    obj.uv_phase[1] += obj.uv_rate[1] * ticks;
  }
  obj.morph_phase_prev = obj.morph_phase;
  if (obj.ext & 2) obj.morph_phase = add16(obj.morph_phase, obj.morph_rate * ticks);
  if (obj.path && obj.path_l0 != 0 && obj.life != obj.path_l0) {
    const int mode = (obj.path_mode >= 0 && obj.path_mode <= 7) ? obj.path_mode : 0;
    const int eased = ease(((static_cast<int>(obj.path_l0) - static_cast<int>(static_cast<s16>(obj.life))) * 0x1000) / static_cast<int>(obj.path_l0), mode);
    if (obj.path_live && hooks.actor_basis) {
      int sc = 0;
      bool found = false;
      obj.path_basis = actor_basis(obj.path_sel[0], obj.path_sel[1], obj.path_sel[2], obj.path_sel[3], sc, found);
    }
    const Vec3 point = obj.path_basis.m.apply(obj.path->sample(eased));
    for (int i = 0; i < 3; ++i) {
      const s32 target = shl32(static_cast<s16>(static_cast<int>(point[static_cast<size_t>(i)] + obj.path_basis.t[static_cast<size_t>(i)])), 16);
      // Reached over the frame's ticks, in the pos >> pos_shift domain.
      obj.vel[i] = ((target - obj.pos[i]) >> cfg.pos_shift) / ticks;
    }
  }
}

// The periodic child header runs against this object's heading frame.
void EmitterSystem::Impl::periodic_spawn(size_t idx, int ticks) {
  const size_t ci = static_cast<size_t>(objs[idx].periodic);
  const Object me = objs[idx];
  for (int tick = 0; tick < ticks; ++tick) {
    Header& ch = headers[ci];
    ch.countdown = static_cast<s16>(ch.countdown - 1);
    if (ch.countdown != -1) continue;
    if (depth < kMaxDepth) {
      const int reps = ch.def->repeat;
      for (int k = 0; k <= reps && reps >= 0; ++k) {
        // fire(): the child's op 45 emission runs too.
        const int fired = fire(ci, &me);
        if (fired < 0) break;
        if (objs[static_cast<size_t>(fired)].life == 0) objs[static_cast<size_t>(fired)].alive = false;
      }
    }
    Header& ch2 = headers[ci];
    ch2.countdown = static_cast<s16>(ch2.def->period);
    if (ch2.def->jitter() != 0) ch2.countdown = static_cast<s16>(ch2.countdown + 2 * (rng.next() % static_cast<int>(ch2.def->jitter())));
  }
}

// Anchor frame at a draw: the entity's INTERPOLATED position (the model is drawn at pose(alpha)); a relative object drawn against the
// latest tick position would step once per tick while the anchor's model glides.
bool EmitterSystem::Impl::anchor_at(int id, float alpha, Basis& basis) const {
  if (hooks.anchor_basis_at && hooks.anchor_basis_at(id, alpha, basis)) return true;
  return hooks.anchor_basis && hooks.anchor_basis(id, basis);
}

// -- tick -------------------------------------------------------------------------------------------------------------------
void EmitterSystem::Impl::tick() {
  ++tick_count;
  if (++frame_phase < frame_ticks) return;
  frame_phase = 0;
  step_frame(frame_ticks);
}

// Where the drawn picture sits between the last two frame states: the ticks since the last step plus the tick alpha, over the frame.
float EmitterSystem::Impl::frame_blend(float alpha) const {
  return frame_ticks <= 1 ? alpha : (static_cast<float>(frame_phase) + alpha) / static_cast<float>(frame_ticks);
}

// One frame: the header stepper once per tick of the frame, then one update of every live object with the frame's tick count.
void EmitterSystem::Impl::step_frame(int ticks) {
  for (size_t i = 0; i < headers.size(); ++i)
    for (int tick = 0; tick < ticks && headers[i].linked; ++tick) step_header(i);
  // objects (including those spawned above and during this pass)
  for (size_t i = 0; i < objs.size(); ++i)
    if (objs[i].alive) {
      const Header* hh = objs[i].owner >= 0 && static_cast<size_t>(objs[i].owner) < headers.size() ? &headers[static_cast<size_t>(objs[i].owner)] : nullptr;
      if (hh && hh->hook && !hh->hook_after) {
        EmitterSystem::ObjectRef object_ref = ref(objs[i]);
        hh->hook(object_ref);
      }
      update(i, ticks);
      if (hh && hh->hook && hh->hook_after && objs[i].alive) {
        EmitterSystem::ObjectRef object_ref = ref(objs[i]);
        hh->hook(object_ref);
      }
    }
  // Lights (sprite type 17): every live light writes its slot of the light table once per frame:
  // position = the s16 high halves of the object's position, through the op-39 basis for a parent-relative object
  // (flag bit 15), colour = colour accumulators >> 4, intensity = scale x, falloff = angle x.
  if (lights)
    for (const Object& obj : objs) {
      if (!obj.alive || obj.stype != 17 || obj.light_slot < 0 || obj.light_slot > 2) continue;
      Vec3 wpos{static_cast<float>(obj.pos[0] >> 16), static_cast<float>(obj.pos[1] >> 16), static_cast<float>(obj.pos[2] >> 16)};
      if ((obj.flags & kFlagRelParent) && obj.owner >= 0 && static_cast<size_t>(obj.owner) < headers.size()) {
        const Header& header = headers[static_cast<size_t>(obj.owner)];
        Basis basis;
        bool ok = false;
        if (header.has_anchor_op && header.anchor >= 0 && hooks.anchor_basis) ok = hooks.anchor_basis(header.anchor, basis);
        else if (header.has_basis_op && hooks.actor_basis) {
          int sc = 0;
          ok = hooks.actor_basis(header.basis_sel[0], header.basis_sel[1], header.basis_sel[2], header.basis_sel[3], basis, sc);
        }
        if (ok) {
          const Vec3 world = basis.m.apply(wpos);
          wpos = {world[0] + basis.t[0], world[1] + basis.t[1], world[2] + basis.t[2]};
        }
      }
      LightTable::Slot& light = lights->slot[static_cast<size_t>(obj.light_slot)];
      for (size_t k = 0; k < 3; ++k) {
        light.pos[k] = static_cast<s32>(wpos[k]);
        light.rgb[k] = static_cast<u8>((static_cast<int>(obj.col[k]) >> 4) & 0xFF);
      }
      light.intensity = obj.scale[0];
      light.falloff = obj.ang[0];
    }
  // Freeing a light object (flag bit31, sprite type 17) zeroes that slot's intensity, so the next
  // light finds it free.
  // A slot a live light still holds is rewritten by the loop above and keeps its value.
  if (lights)
    for (const Object& obj : objs) {
      if (obj.alive || obj.stype != 17 || obj.light_slot < 0 || obj.light_slot > 2) continue;
      bool held = false;
      for (const Object& other : objs) held = held || (other.alive && other.stype == 17 && other.light_slot == obj.light_slot);
      if (!held) lights->slot[static_cast<size_t>(obj.light_slot)].intensity = 0;
    }
  // Register slots A / B and compact the list.
  slot_idx[0] = slot_idx[1] = ~size_t{0};
  size_t write = 0;
  for (size_t read = 0; read < objs.size(); ++read) {
    if (!objs[read].alive) continue;
    if (write != read) objs[write] = std::move(objs[read]);
    if (const int slot = objs[write].slot_reg; slot >= 0) {
      slot_idx[slot] = write;
      slot_model[slot] = !(objs[write].flags & 0x80000000u);
      for (int i = 0; i < 3; ++i) slot_pos[slot][i] = objs[write].pos[i], slot_vel[slot][i] = objs[write].vel[i];
      slot_yaw[slot] = objs[write].ang[1];
    }
    ++write;
  }
  objs.resize(write);
  st.live = static_cast<u32>(objs.size());
  u32 run = 0;
  for (const auto& header : headers) run += header.linked ? 1u : 0u;
  st.running = run;
}

// -- quads --------------------------------------------------------------------------------------------------------------------
EmitterSystem::ObjectRef EmitterSystem::Impl::ref(Object& obj) {
  EmitterSystem::ObjectRef object_ref;
  object_ref.pos = obj.pos;
  object_ref.vel = obj.vel;
  object_ref.acc = obj.acc;
  object_ref.scale = obj.scale;
  object_ref.ang = obj.ang;
  object_ref.col = obj.col;
  object_ref.life = &obj.life;
  object_ref.flags = &obj.flags;
  object_ref.owner = obj.owner;
  if (obj.owner >= 0 && static_cast<size_t>(obj.owner) < headers.size()) object_ref.basis_sel = headers[static_cast<size_t>(obj.owner)].basis_sel[0];
  object_ref.shift = cfg.pos_shift;
  return object_ref;
}

}  // namespace oc::effects
