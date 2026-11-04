// FieldWorld: the room's particle effects. The emitter system and the sequence player are built per room from the room's own
// effect pack plus the shared library (effect/particle.prd, with its nested encounter pack); this file holds their anchors into the
// entity table (actor bases, animation-driven bone followers), the sequence screen colour and post-pass state, the scripted sound
// emitters, the actor wakes / footprints, and the depth-sorted draw of the effect quads and mesh objects.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include "core/log.h"
#include "game/effects/strip_smooth.h"
#include "game/field_world_internal.h"

namespace oc::game {

using namespace field_internal;

// ------------------------------------------------------------------------------------------------ named effects (script ops)
// 9B / 9C / B6 / E7 drive the room's emitters by name, D3 / D7 its sequences. The host may tap an op first (the title scene's global
// sequences the room has no entry for). The name "0000" means every emitter for 9C and E7 alike (all stopped, all their objects
// freed) and every sequence for D7 (the 'loop' one too; the screen shake and the picture post pass end with them); B6 and D3 look
// it up as an ordinary name, which no pack has.
void FieldWorld::named_effect(script::NamedOp op, std::string_view name, int ent) {
  std::string effect_name(name.substr(0, 4));
  while (!effect_name.empty() && effect_name.back() == 0) effect_name.pop_back();
  if (effect_tap_ && effect_tap_(op, effect_name)) return;
  if (!emitters_) return;
  const bool every = effect_name == "0000";
  switch (op) {
    case script::NamedOp::AttachToEntity5: emitters_->attach(effect_name, ent); break;
    case script::NamedOp::ModeZero:
      if (every) emitters_->kill(effect_name);
      else emitters_->stop(effect_name);
      break;
    case script::NamedOp::ModeOne: emitters_->kill(effect_name); break;
    case script::NamedOp::Activate: emitters_->start(effect_name); break;
    case script::NamedOp::PlayEffect:
      if (sequences_) sequences_->start(effect_name);
      break;
    case script::NamedOp::StopEffect:
      if (!sequences_) break;
      if (every) {
        sequences_->stop_all();
        seq_post_.on = false;
      } else {
        sequences_->stop(effect_name);
      }
      break;
  }
}

// ------------------------------------------------------------------------------------------------ bone followers (anim-sequence op 1D)
// The anchor of an emitter started by a model's animation sequence is a matrix built from the attach point: mode 0 RotMatrix(angles),
// 1 RotMatrix(angles + the model rotation), 2 RotMatrix(angles) x the joint matrix; scaled by the model scale; translation = the attach
// point's world position + the offset. With flag 0x80 it is rebuilt every tick; the follower lives for the emitter header's duration.
bool FieldWorld::bone_basis(BoneFollower& follower) {
  if (!valid_ent(follower.ent)) return false;
  const Actor& actor = ents_[static_cast<size_t>(follower.ent)].actor;
  render::Mat4 joint_world;
  float offset[3];
  if (!actor.attach_joint(follower.point, 1.0f, joint_world, offset)) return false;
  effects::Basis basis;
  const script::Entity& vm_ent = vm_.entity(follower.ent);
  int ax = follower.ang[0], ay = follower.ang[1], az = follower.ang[2];
  if ((follower.mode & 0x7F) == 1) {
    ax += vm_ent.axis[0];
    ay += vm_ent.axis[1];
    az += vm_ent.axis[2];
  }
  basis.m = effects::Mat3::from_angles(ax, ay, az);
  if ((follower.mode & 0x7F) == 2) {
    effects::Mat3 joint;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col) joint.m[static_cast<size_t>(row * 3 + col)] = joint_world.at(row, col);
    basis.m = basis.m * joint;
  }
  basis.m = basis.m.scaled_columns(static_cast<float>(static_cast<s16>(vm_ent.scale[0])) * 8.0f / 4096.0f,
                                   static_cast<float>(static_cast<s16>(vm_ent.scale[1])) * 8.0f / 4096.0f,
                                   static_cast<float>(static_cast<s16>(vm_ent.scale[2])) * 8.0f / 4096.0f);
  for (int row = 0; row < 3; ++row)
    basis.t[static_cast<size_t>(row)] = joint_world.at(row, 0) * offset[0] + joint_world.at(row, 1) * offset[1] +
                                        joint_world.at(row, 2) * offset[2] + joint_world.at(row, 3) +
                                        static_cast<float>(follower.off[static_cast<size_t>(row)]);
  follower.basis = basis;
  return true;
}

// Starts the emitters the models' animation sequences asked for this tick, then re-places the live followers. An expired follower is
// no longer re-placed but stays as a frozen basis until the room ends (the emitter keeps pointing at its last matrix).
void FieldWorld::step_bone_emitters() {
  for (int ent = 0; ent < static_cast<int>(ents_.size()); ++ent) {
    for (auto& request : ents_[static_cast<size_t>(ent)].actor.take_bone_emitters()) {
      if (!emitters_) continue;
      BoneFollower follower;
      follower.ent = ent;
      follower.point = request.point;
      follower.mode = request.flags;
      follower.follow = (request.flags & 0x80) != 0;
      follower.off = request.off;
      follower.ang = request.ang;
      if (!bone_basis(follower)) continue;
      const int id = next_bone_anchor_++;
      int duration = 0;
      const int headers = emitters_->attach_all(request.name, id, duration);
      follower.life = duration;
      if (headers > 0) bone_followers_[id] = follower;
      log::debug("field: tick {} ent {} anim op 1D emitter {} on point {} ({} headers, life {})", tick_count_, ent, request.name, request.point,
                 headers, duration);
    }
  }
  for (auto& [id, follower] : bone_followers_) {
    if (follower.follow) bone_basis(follower);
    if (follower.life > 0 && --follower.life == 0) follower.follow = false;
  }
}

// ------------------------------------------------------------------------------------------------ sound emitters (C5 / C8 / FE 43)
// Every tick: the volume ramps advance; each record's distance from the leader is taken (an inactive source stops its voice); the
// nearest four records own a voice. Volume = 0x7F within 0x100 units, (0x900 - d) >> 4 up to 0x900, 0 beyond, times the record
// volume >> 7; pan from the sound's screen x. A record whose sound runs gets the new pan and volume; an idle one starts (not started
// yet), restarts (not one-shot) or is freed (one-shot). Records beyond the nearest four are keyed off.
void FieldWorld::step_sound_emitters() {
  if (sound_emitters_.empty()) return;
  const int player = player_entity();
  if (!valid_ent(player)) return;
  const ActorPose& leader = ents_[static_cast<size_t>(player)].actor.current();
  struct Near {
    float distance;
    size_t index;
    float x, y, z;
  };
  std::vector<Near> order;
  order.reserve(sound_emitters_.size());
  for (size_t index = 0; index < sound_emitters_.size(); ++index) {
    SoundEmitter& record = sound_emitters_[index];
    if (record.ramp) {
      record.ramp_value += record.ramp_step;
      record.volume = std::clamp(record.ramp_value >> 16, 0, 0x7F);
      if (--record.ramp_frames <= 0) record.ramp = false;
    }
    float x = static_cast<float>(record.x), y = static_cast<float>(record.y), z = static_cast<float>(record.z);
    if (valid_ent(record.src)) {
      if (vm_.entity(record.src).flags38 & script::flag38::kInactive) {
        if (record.started && mixer_) mixer_->evict_sfx(1u << record.voice);
        record.started = false;
        continue;
      }
      const ActorPose& source = ents_[static_cast<size_t>(record.src)].actor.current();
      x = source.x, y = source.y, z = source.z;
    }
    order.push_back({std::sqrt((x - leader.x) * (x - leader.x) + (y - leader.y) * (y - leader.y) + (z - leader.z) * (z - leader.z)), index, x, y, z});
  }
  std::stable_sort(order.begin(), order.end(), [](const Near& lhs, const Near& rhs) { return lhs.distance < rhs.distance; });
  for (size_t rank = 0; rank < order.size(); ++rank) {
    SoundEmitter& record = sound_emitters_[order[rank].index];
    if (rank >= 4 || record.id == 0xFF) {
      if (record.started && mixer_) mixer_->evict_sfx(1u << record.voice);
      record.started = false;
      continue;
    }
    const float distance = order[rank].distance;
    const int near_volume = distance <= 256.0f ? 0x7F : distance < 0x900 ? (0x900 - static_cast<int>(distance)) >> 4 : 0;
    const int vol = std::clamp(record.volume * near_volume >> 7, 0, 0x7F);
    int pan8 = 0x80;
    render::FieldCamera::Projected projected;
    if (view_.camera().project({order[rank].x, order[rank].y, order[rank].z}, projected))
      pan8 = std::clamp(((static_cast<int>(projected.sx) + camera_.cam.x) * 0x6666 >> 16) + 0x40, 0, 0xFF);
    const int pan = pan8 >> 1;
    const u32 mask = 1u << record.voice;
    audio::Sfx& out = sfx_out();
    if (out.field_active(mask)) {
      out.set_field_mods(mask, vol, pan);
    } else if (!record.started || !record.oneshot) {
      out.play_field(record.id, vol, pan, mask);
      record.started = true;
    } else {
      record.id = 0xFF;
      record.volume = 0;
      record.started = false;
    }
  }
}

// ------------------------------------------------------------------------------------------------ effect system per room
void FieldWorld::clear_effects() {
  sequences_.reset();
  emitters_.reset();
  lights_ = effects::LightTable{};
  bk_ = {};
  bank_ = effects::EffectBank{};
  gpu_fx_.clear();
  room_meshes_.clear();
  seq_post_ = {};
  seq_dim_ = {};
}

// Emitter op 0x39 selector `selector`, point `point`, through the field's getters: 1 / 9 -> caster[0], 10 / 11 -> caster[selector - 9],
// 2 / 13 -> target[0], 14..21 -> target[selector - 13] (byte tables of entity ids the scripts fill with FE 09 / FE 0A), 12 -> the
// battle-intro anchor. The basis is a yaw-only rotation at the model's position, or at attach point `point` - 2 for point >= 2
// (other than 0x22 / 0x23).
bool FieldWorld::effects_actor_basis(int selector, int point, effects::Basis& out) const {
  int ent = -1;
  if (selector == 12) ent = battle_intro_anchor();
  else if (selector == 1 || selector == 9) ent = vm_.ent_ref(1, 0);
  else if (selector == 10 || selector == 11) ent = vm_.ent_ref(1, selector - 9);
  else if (selector == 2 || selector == 13) ent = vm_.ent_ref(2, 0);
  else if (selector >= 14 && selector <= 21) ent = vm_.ent_ref(2, selector - 13);
  else return false;
  if (!valid_ent(ent)) return false;
  const Ent& entity = ents_[static_cast<size_t>(ent)];
  const ActorPose& pose = entity.actor.current();
  out = effects::Basis{};
  out.m = effects::Mat3::from_angles(0, static_cast<int>(std::lround(pose.yaw)), 0);
  out.t = {pose.x, pose.y, pose.z};
  if (point >= 2 && point != 0x22 && point != 0x23) {
    float world[3];
    if (entity.actor.attach_world(point - 2, world)) out.t = {world[0], world[1], world[2]};
  }
  return true;
}

// Sequence ops 2B / 2C: any running task of the slot is freed; a new one starts from the current colour when the colour or the mode
// differs, and runs once at once.
void FieldWorld::seq_dim_set(size_t slot, int r, int g, int b, int mode, int ticks) {
  SeqDim& dim = seq_dim_[slot];
  dim.task = false;
  const std::array<int, 3> target = {r & 0xFF, g & 0xFF, b & 0xFF};
  if (dim.cur == target && dim.mode == mode) return;
  dim.start = dim.cur;
  dim.target = target;
  dim.elapsed = 0;
  dim.ticks = ticks;
  dim.mode = mode;
  dim.task = true;
  seq_dim_step(dim);
}

// Per tick: colour = start + (target - start) * t / ticks (C division per channel), t += 1; past `ticks` the colour is the target and
// the task ends.
void FieldWorld::seq_dim_step(SeqDim& dim) {
  if (!dim.task) return;
  if (dim.ticks != 0)
    for (size_t channel = 0; channel < 3; ++channel)
      dim.cur[channel] = (dim.start[channel] + (dim.target[channel] - dim.start[channel]) * dim.elapsed / dim.ticks) & 0xFF;
  ++dim.elapsed;
  if (dim.elapsed >static_cast<s16>(dim.ticks)) {
    dim.cur = dim.target;
    dim.task = false;
  }
}

// Back colour ramp (sequence op 0x65, a pack's type-0x24 entry with 0 ticks): cur = from + (to - from) * t / ticks per byte channel,
// t += 1, past `ticks` the target.
void FieldWorld::bk_set(int r, int g, int b, int ticks) {
  bk_.from = bk_.cur;
  bk_.to = {r & 0xFF, g & 0xFF, b & 0xFF};
  bk_.elapsed = 0;
  bk_.ticks = ticks;
  bk_.ramping = true;
  bk_step();
}

void FieldWorld::bk_step() {
  if (!bk_.ramping) return;
  if (bk_.ticks != 0)
    for (size_t channel = 0; channel < 3; ++channel)
      bk_.cur[channel] = (bk_.from[channel] + ((bk_.to[channel] - bk_.from[channel]) * bk_.elapsed) / bk_.ticks) & 0xFF;
  ++bk_.elapsed;
  if (bk_.ticks < bk_.elapsed) {
    bk_.cur = bk_.to;
    bk_.ramping = false;
  }
}

// Sequence ops 3E / 3F / 40: a transformed copy of the whole picture blended over it. 3E switches it on with the parameters reset
// (rotation / translation / offset 0, colour 0x808080, ABR 0), 3F off, 40 ramps from the current values to the targets in the command
// (data bytes after the 4 header bytes: +4 ticks, +8 rot xyz, +0xE trans xyz, +0x14 offset xy, +0x18 colour rgb + ABR mode) with the
// integer lerp `start + (target - start) * t / ticks`, the target once t > ticks.
void FieldWorld::seq_post_op(const assets::SeqCmd& cmd) {
  auto s16_at = [&](size_t at) {
    return at + 1 < cmd.data.size() ? static_cast<int>(static_cast<std::int16_t>(cmd.data[at] | (cmd.data[at + 1] << 8))) : 0;
  };
  if (cmd.op == 0x3E) {
    seq_post_ = {};
    seq_post_.on = true;
  } else if (cmd.op == 0x3F) {
    seq_post_.on = false;
  } else if (cmd.data.size() >= 24) {
    seq_post_.start = seq_post_.cur;
    seq_post_.rgb_start = seq_post_.rgb;
    const size_t at[8] = {4, 6, 8, 10, 12, 14, 16, 18};
    for (size_t i = 0; i < 8; ++i) seq_post_.target[i] = s16_at(at[i]);
    for (size_t i = 0; i < 3; ++i) seq_post_.rgb_target[i] = cmd.data[20 + i];
    seq_post_.abr = cmd.data[23];
    seq_post_.ticks = s16_at(0);
    seq_post_.elapsed = 0;
    seq_post_.ramp = true;
    seq_post_step();
  }
}

void FieldWorld::seq_post_step() {
  SeqPost& post = seq_post_;
  if (!post.ramp) return;
  if (post.ticks != 0) {
    for (size_t i = 0; i < 8; ++i) post.cur[i] = post.start[i] + (post.target[i] - post.start[i]) * post.elapsed / post.ticks;
    for (size_t i = 0; i < 3; ++i) post.rgb[i] = post.rgb_start[i] + (post.rgb_target[i] - post.rgb_start[i]) * post.elapsed / post.ticks;
  }
  if (++post.elapsed >post.ticks) {
    post.cur = post.target;
    post.rgb = post.rgb_target;
    post.ramp = false;
  }
}

// Builds the emitter system and the sequence player of the loaded room: the room's effect pack, the shared library (actor wakes /
// footprints) and the encounter pack, with the hooks that place emitters on entities. An entity anchor is the model's rotation times
// its scale at its position (a wake offset shrinks and turns with the figure); a bone anchor is the follower's basis.
void FieldWorld::build_effects() {
  clear_effects();
  effects_logged_ = false;
  const assets::RoomScene* scene = view_.scene();
  if (!scene || (scene->effects.emitters.empty() && scene->effects.sequences.empty() && !lib_loaded_)) return;
  bank_.add(&scene->effects);
  if (auto meshes = view_.effect_meshes()) room_meshes_.set(std::move(*meshes), ctx_, sprites_);
  if (lib_loaded_) bank_.add(&lib_fx_);
  if (lib_loaded_) bank_.add(&fenc_fx_);
  effects::EmitterSystem::Config cfg;
  cfg.seed = seed_ ? seed_ * 0x9E3779B1u + static_cast<u32>(room_id_ < 0 ? 0 : room_id_) * 7919u + 1u : 1u;
  emitters_ = std::make_unique<effects::EmitterSystem>(bank_, cfg);
  // The field draws every second refresh; its effects advance once per drawn frame with that frame's 2 ticks.
  emitters_->set_frame_ticks(2);
  lights_ = effects::LightTable{};
  emitters_->set_lights(&lights_);
  effects::ActorHooks hooks;
  auto anchor_frame = [this](int ent, const ActorPose& pose) {
    const Ent& entity = ents_[static_cast<size_t>(ent)];
    effects::Basis basis;
    const int pitch = static_cast<int>(std::lround(entity.actor.pitch)), roll = static_cast<int>(std::lround(entity.actor.roll));
    const int yaw = static_cast<int>(std::lround(pose.yaw)), local_yaw = static_cast<int>(std::lround(entity.actor.local_yaw));
    effects::Mat3 rotation = entity.actor.tilt_world ? effects::Mat3::from_angles(pitch, yaw, roll)
                                                     : effects::Mat3::from_angles(0, yaw, 0) * effects::Mat3::from_angles(pitch, local_yaw, roll);
    basis.m = rotation.scaled_columns(entity.actor.scale * entity.actor.axis_scale[0], entity.actor.scale * entity.actor.axis_scale[1],
                                      entity.actor.scale * entity.actor.axis_scale[2]);
    basis.t = {pose.x, pose.y, pose.z};
    return basis;
  };
  hooks.anchor_basis = [this, anchor_frame](int ent, effects::Basis& out) {
    if (ent >= kBoneAnchorBase) {
      const auto it = bone_followers_.find(ent);
      if (it == bone_followers_.end()) return false;
      out = it->second.basis;
      return true;
    }
    if (!valid_ent(ent)) return false;
    out = anchor_frame(ent, ents_[static_cast<size_t>(ent)].actor.current());
    return true;
  };
  hooks.anchor_basis_at = [this, anchor_frame](int ent, float alpha, effects::Basis& out) {
    if (ent < 0 || ent >= kBoneAnchorBase) return false;
    if (ent >= static_cast<int>(ents_.size())) return false;
    out = anchor_frame(ent, ents_[static_cast<size_t>(ent)].actor.pose(alpha));
    return true;
  };
  hooks.actor_basis = [this](int selector, int point, int, int, effects::Basis& out, int&) { return effects_actor_basis(selector, point, out); };
  emitters_->set_hooks(std::move(hooks));
  emitters_->register_all();
  sequences_ = std::make_unique<effects::SequencePlayer>(bank_, *emitters_, cfg.seed);
  sequences_->set_sfx(sfx_ ? sfx_ : &log_sfx_);
  effects::SequenceHooks seq_hooks;
  seq_hooks.ambient = [this](int r, int g, int b, int ticks) { bk_set(r, g, b, ticks); };
  for (const auto& blob : scene->effects.blobs)
    if (blob.type == 0x24 && blob.data.size() >= 3) bk_set(blob.data[0], blob.data[1], blob.data[2], 0);
  seq_hooks.other = [this](const assets::SeqCmd& cmd) {
    if (cmd.op == 0x3E || cmd.op == 0x3F || cmd.op == 0x40) seq_post_op(cmd);
    if ((cmd.op == 0x2B || cmd.op == 0x2C) && cmd.data.size() >= 8)
      seq_dim_set(cmd.op == 0x2B ? 0 : 1, cmd.data[5], cmd.data[6], cmd.data[7], cmd.data[4],
                  static_cast<s16>(static_cast<u16>(cmd.data[0] | (cmd.data[1] << 8))));
  };
  sequences_->set_hooks(std::move(seq_hooks));
  // Every room entry starts the pack's repeating sequence 'loop' when it has one (ambient birds, smoke, bubbles).
  sequences_->start("loop");
  if (particles_ok_ && effects_gpu_) gpu_fx_.load(bank_);
  log::info("field: {} effects: {} emitters, {} sequences, {} sheets", scene->name, scene->effects.emitters.size(),
            scene->effects.sequences.size(), scene->effects.sheets.size());
}

// ------------------------------------------------------------------------------------------------ wakes and footprints
// Per actor that is drawn: on a water triangle the level is the op-66 table entry of its class; unless the FE 2A bit is set, water
// deeper than 0x28 under the feet spawns the ring mz01 at the surface every 4th 30 Hz frame while the walk / run clip plays and every
// 16th otherwise; shallower water spawns mz02 splashes and dry ground asi1 footprints (unless the FE 2C bit is set) at the feet: one
// per foot event bit 0-3 of the model (attach points 12, 13, 7, 8 = ankles, then hands for a quadruped). Bits 4 / 5 of the foot events
// are the footstep sounds, played first.
void FieldWorld::step_wakes() {
  static constexpr int kFootPoints[4] = {12, 13, 7, 8};
  for (size_t index = 0; index < ents_.size(); ++index) {
    Ent& entity = ents_[index];
    const int ent = static_cast<int>(index);
    const int feet = entity.actor.take_foot_events();
    play_footstep(ent, feet);
    if (!emitters_ || !entity.actor.loaded() || entity.actor.triangle() < 0) continue;
    const script::Entity& vm_ent = vm_.entity(ent);
    if (vm_ent.flags38 & 0x10C020) continue;
    const int slot = RoomLighting::water_slot(walk_flags(ent));
    const ActorPose& pose = entity.actor.current();
    const int y = static_cast<int>(std::lround(pose.y));
    const int yaw = (entity.actor.shown_facing() + 0x400) & 0xFFF;
    auto at_feet = [&](const char* name) {
      for (int foot = 0; foot < 4; ++foot) {
        if (!(feet & (1 << foot))) continue;
        float world[3];
        if (!entity.actor.attach_world(kFootPoints[foot], world)) continue;
        effects::Vec3 normal{};
        const bool ok = emitters_->spawn_at(name, static_cast<int>(std::lround(world[0])), y, static_cast<int>(std::lround(world[2])), yaw, &normal);
        log::debug("field: tick {} ent {} foot {} {} at {:.0f},{},{:.0f} (actor {:.0f},{:.0f}) normal {:.2f},{:.2f},{:.2f}{}", tick_count_, index,
                   foot, name, static_cast<double>(world[0]), y, static_cast<double>(world[2]), static_cast<double>(pose.x),
                   static_cast<double>(pose.z), static_cast<double>(normal[0]), static_cast<double>(normal[1]), static_cast<double>(normal[2]),
                   ok ? "" : " (refused)");
      }
    };
    if (slot < 0) {
      if (!(vm_ent.flags270 & 0x800000u)) at_feet("asi1");
      continue;
    }
    if (vm_ent.flags270 & 0x400000u) continue;
    const int level = vm_.water_level(static_cast<size_t>(slot));
    const int anim = entity.actor.submitted_anim();
    const int mask = anim == entity.actor.anim_slot(kSlotRun) || anim == entity.actor.anim_slot(kSlotWalk) ? 3 : 15;
    const bool frame_tick = (tick_count_ & 1u) != 0;
    if (level - y < -0x28) {
      if (frame_tick && (entity.wake_count & mask) == 0) {
        effects::Vec3 normal{};
        const bool ok = emitters_->spawn_at("mz01", static_cast<int>(std::lround(pose.x)), level, static_cast<int>(std::lround(pose.z)), yaw, &normal);
        if (!ok) log::warn("field: water ripple mz01 refused (ent {})", index);
        else log::debug("field: tick {} ent {} mz01 at {:.0f},{},{:.0f} yaw {} normal {:.2f},{:.2f},{:.2f}", tick_count_, index,
                        static_cast<double>(pose.x), level, static_cast<double>(pose.z), yaw, static_cast<double>(normal[0]),
                        static_cast<double>(normal[1]), static_cast<double>(normal[2]));
      }
    } else {
      at_feet("mz02");
    }
    if (!frame_tick) continue;
    if (entity.wake_count == 0)
      log::debug("field: tick {} ent {} in water class {} level {} feet y {}: {}", tick_count_, index, slot + 1, level, y,
                 level - y < -0x28 ? "ripples (mz01)" : "shallow (mz02 foot splashes)");
    ++entity.wake_count;
  }
}

// ------------------------------------------------------------------------------------------------ drawing
// Depth-tested particle quads of the room's emitters plus the sprite-type-14 mesh objects (the room's own textured packet meshes and
// the encounter's untextured ones, Gouraud-shaded from their vertex colours times the object colour / 128). Everything is merged into
// one painter's order: farther ordering-table slot first, inside a slot the later-submitted primitive first. Only the content inside
// the current draw slab (the 8F split) is drawn. What is filed under a layer's draw offset scrolls with that layer, not with the camera.
void FieldWorld::draw_effects(const render::Frame& frame, float vw, float vh, float alpha) {
  alpha = interp_alpha(alpha);
  if (!emitters_ || !particles_ok_ || !effects_gpu_ || !view_.loaded() || !view_.scene()) return;
  effects::ViewParams vp = effects::ViewParams::from_room(view_.scene()->camera);
  view_.scroll_at(alpha, vp.scroll_x, vp.scroll_y);
  std::vector<effects::DrawQuad> quads;
  emitters_->collect_quads(vp, alpha, quads);
  if (slab_min_ > -1e8f || slab_max_ < 1e8f)
    quads.erase(std::remove_if(quads.begin(), quads.end(), [&](const effects::DrawQuad& quad) { return !in_slab(static_cast<float>(quad.slot)); }),
                quads.end());
  for (effects::DrawQuad& quad : quads)
    if (const float shift = view_.slot_parallax(quad.slot) - 1.0f; shift != 0.0f)
      for (int corner = 0; corner < 4; ++corner) {
        quad.x[corner] += vp.scroll_x * shift;
        quad.y[corner] += vp.scroll_y * shift;
      }
  effects::sort_draw_order(quads);
  struct Tri {
    float x[4], y[4];
    u8 rgb[4][3];
    int blend, slot;
    u32 order;
    float u[4] = {0, 1, 0, 1}, v[4] = {0, 0, 1, 1};
    render::TexRef set;
  };
  std::vector<Tri> tris;
  const render::Letterbox lb = view_.letterbox(vw, vh);
  if ((fenc_meshes_ || !room_meshes_.empty()) && white_set_) {
    std::vector<effects::EmitterSystem::MeshInstance> instances;
    emitters_->collect_meshes(vp, alpha, instances);
    u32 order = static_cast<u32>(quads.size()) + 1000000u;
    for (const auto& instance : instances) {
      if (!instance.name || instance.sprite_type != 14) continue;
      if (room_meshes_.has(*instance.name)) {
        std::vector<effects::RoomMeshTri> room_tris;
        room_meshes_.project(instance, vp, lb, alpha, room_tris);
        for (const effects::RoomMeshTri& room_tri : room_tris) {
          if (!in_slab(static_cast<float>(room_tri.slot))) continue;
          Tri tri{};
          std::copy(std::begin(room_tri.x), std::end(room_tri.x), tri.x);
          std::copy(std::begin(room_tri.y), std::end(room_tri.y), tri.y);
          std::copy(std::begin(room_tri.u), std::end(room_tri.u), tri.u);
          std::copy(std::begin(room_tri.v), std::end(room_tri.v), tri.v);
          for (int corner = 0; corner < 4; ++corner)
            for (int channel = 0; channel < 3; ++channel) tri.rgb[corner][channel] = room_tri.rgb[corner][channel];
          tri.blend = room_tri.blend;
          tri.slot = room_tri.slot;
          tri.set = room_tri.set;
          tri.order = order++;
          tris.push_back(tri);
        }
        continue;
      }
      if (!fenc_meshes_) continue;
      const assets::StagePart* part = nullptr;
      for (const auto& candidate : fenc_meshes_->parts)
        if (candidate.name == *instance.name) part = &candidate;
      if (!part || instance.off_screen(part->cull_points, vp)) continue;
      std::vector<effects::SmoothVert> verts;
      for (u32 k = 0; k + 2 < part->indices.size(); k += 3)
        for (u32 corner = 0; corner < 3; ++corner) {
          const assets::StageVertex& stage_vert = part->vertices[part->indices[k + corner]];
          effects::SmoothVert vert{};
          vert.p[0] = stage_vert.pos[0], vert.p[1] = -stage_vert.pos[1], vert.p[2] = stage_vert.pos[2];
          for (int channel = 0; channel < 3; ++channel) vert.c[channel] = stage_vert.color[channel] * static_cast<float>(instance.rgb[channel]);
          verts.push_back(vert);
        }
      auto project = [&](const float point[3], float xy[2], float* z_out) {
        const effects::Vec3 delta = instance.mat.apply({point[0], point[1], point[2]});
        const float cx = instance.centre[0] + delta[0], cy = instance.centre[1] + delta[1], cz = instance.centre[2] + delta[2];
        if (cz < 1.0f) return false;
        xy[0] = lb.ox + (vp.ox + vp.h * cx / cz + vp.scroll_x) * lb.scale;
        xy[1] = lb.oy + (vp.oy + vp.h * cy / cz + vp.scroll_y) * lb.scale_y;
        if (z_out) *z_out = cz;
        return true;
      };
      if (smooth_rings_)
        effects::smooth_triangle_rings(verts, 6.0f, [&](const float point[3], float xy[2]) { return project(point, xy, nullptr); });
      for (size_t k = 0; k + 2 < verts.size(); k += 3) {
        Tri tri{};
        float z_sum = 0;
        bool ok = true;
        for (u32 corner = 0; corner < 3 && ok; ++corner) {
          float xy[2], cz = 0;
          if (!project(verts[k + corner].p, xy, &cz)) {
            ok = false;
            break;
          }
          tri.x[corner] = xy[0];
          tri.y[corner] = xy[1];
          z_sum += cz;
          for (int channel = 0; channel < 3; ++channel) tri.rgb[corner][channel] = static_cast<u8>(std::clamp(verts[k + corner].c[channel], 0.0f, 255.0f));
        }
        if (!ok) continue;
        tri.x[3] = tri.x[2];
        tri.y[3] = tri.y[2];
        for (int channel = 0; channel < 3; ++channel) tri.rgb[3][channel] = tri.rgb[2][channel];
        tri.blend = instance.blend >= 5 ? 2 : instance.blend;
        const int slot = instance.fixed_slot ? instance.bias : (static_cast<int>(z_sum / 3.0f) >> 4) + instance.bias;
        tri.slot = std::clamp(slot, 1, 0xFFF);
        if (!in_slab(static_cast<float>(tri.slot))) continue;
        tri.order = order++;
        tris.push_back(tri);
      }
    }
  }
  for (Tri& tri : tris)
    if (const float shift = view_.slot_parallax(tri.slot) - 1.0f; shift != 0.0f)
      for (int corner = 0; corner < 4; ++corner) {
        tri.x[corner] += vp.scroll_x * shift * lb.scale;
        tri.y[corner] += vp.scroll_y * shift * lb.scale_y;
      }
  if (static const bool fx_log = std::getenv("OC_FIELD_FX_LOG") != nullptr; fx_log && tick_count_ % 60 == 0) {
    const effects::EmitterStats stats = emitters_->stats();
    log::info("field fx: tick {} {} quads, {} live, {} spawned, {} unresolved sprites", tick_count_, quads.size(), stats.live, stats.spawned,
              stats.unresolved_sprites);
  }
  static u64 fx_logged_tick = ~0ull;
  const bool fx_tick_log = std::getenv("OC_FIELD_FX_LOG") != nullptr && fx_logged_tick != tick_count_;
  if (fx_tick_log) fx_logged_tick = tick_count_;
  if (fx_tick_log && !tris.empty() && tick_count_ % 4 == 0) {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (const Tri& tri : tris)
      for (int corner = 0; corner < 3; ++corner) {
        x0 = std::min(x0, tri.x[corner]);
        x1 = std::max(x1, tri.x[corner]);
        y0 = std::min(y0, tri.y[corner]);
        y1 = std::max(y1, tri.y[corner]);
      }
    log::info("field fx: tick {} {} mesh triangles, screen x {:.0f}..{:.0f} y {:.0f}..{:.0f}, slot {} rgb {} {} {} blend {}", tick_count_, tris.size(),
              x0, x1, y0, y1, tris[0].slot, tris[0].rgb[0][0], tris[0].rgb[0][1], tris[0].rgb[0][2], tris[0].blend);
  }
  if (fx_tick_log && std::getenv("OC_FIELD_FX_LOG")[0] == '2') {
    std::string lives;
    effects::EmitterSystem::ObjectView object;
    for (size_t i = 0; i < emitters_->live_objects(); ++i)
      if (emitters_->object_view(i, object)) lives += std::format(" {}:{}", object.emitter ? *object.emitter : std::string("?"), object.life);
    log::info("field fx: tick {} objects{}", tick_count_, lives);
  }
  if (fx_tick_log && seq_dim_shown() && tick_count_ % 4 == 0)
    log::info("field fx: tick {} sequence dim {} {} {} mode {}", tick_count_, seq_dim_[0].cur[0], seq_dim_[0].cur[1], seq_dim_[0].cur[2],
              seq_dim_[0].mode);
  if (quads.empty() && tris.empty()) return;
  if (!effects_logged_) {
    effects_logged_ = true;
    const effects::EmitterStats stats = emitters_->stats();
    log::info("field: {} effects on screen: {} quads from {} live objects ({} emitters running)", view_.room_name(), quads.size(), stats.live,
              stats.running);
  }
  particles_.begin(frame, vw, vh);
  if (tris.empty()) {
    gpu_fx_.draw(particles_, quads, lb);
  } else {
    struct Item {
      int slot;
      u32 order;
      bool tri;
      u32 idx;
    };
    std::vector<Item> items;
    items.reserve(quads.size() + tris.size());
    for (u32 i = 0; i < quads.size(); ++i) items.push_back({quads[i].slot, quads[i].order, false, i});
    for (u32 i = 0; i < tris.size(); ++i) items.push_back({tris[i].slot, tris[i].order, true, i});
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& lhs, const Item& rhs) { return lhs.slot != rhs.slot ? lhs.slot > rhs.slot : lhs.order > rhs.order; });
    for (const Item& item : items) {
      if (!item.tri) {
        gpu_fx_.draw_quad(particles_, quads[item.idx], lb);
        continue;
      }
      const Tri& tri = tris[item.idx];
      particles_.quad(tri.set ? tri.set : white_set_, tri.blend, tri.x, tri.y, tri.u, tri.v, tri.rgb,
                      static_cast<float>(tri.slot) / render::kDepthRange);
    }
  }
  particles_.end();
}

}  // namespace oc::game
