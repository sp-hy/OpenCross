#include "game/actor.h"
#include "game/anim_arc.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "import/container.h"
#include "import/tim.h"
#include "vfs/vfs.h"

namespace oc::game {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kUnitsToRad = 2.0f * kPi / 4096.0f;

// Wraps a difference of two 4096/turn angles into [-2048, 2048).
float wrap_units(float a) {
  a = std::fmod(a + 2048.0f, 4096.0f);
  if (a < 0) a += 4096.0f;
  return a - 2048.0f;
}

// Interpolates key values of one channel at fractional frame `frame` (looping over the keys). Rotation follows the retail arc rule (retail_arc).
assets::Vec3 sample_keys(const std::vector<assets::Key3>& keys, float frame, bool angles) {
  if (keys.empty()) return {};
  const size_t key_count = keys.size();
  if (key_count == 1) return keys[0].v;
  const float fw = std::fmod(std::fmod(frame, static_cast<float>(key_count)) + static_cast<float>(key_count), static_cast<float>(key_count));
  const size_t k0 = std::min(static_cast<size_t>(fw), key_count - 1);
  const size_t k1 = (k0 + 1) % key_count;
  const float u = fw - static_cast<float>(k0);
  assets::Vec3 value;
  for (size_t i = 0; i < 3; ++i) {
    float from = keys[k0].v[i];
    float to = keys[k1].v[i];
    if (angles) retail_arc(from, to);
    value[i] = from + (to - from) * u;
  }
  return value;
}

// Models WITHOUT an animation-sequence table (room models: S5 = 0): script animation slots 1-4 (stand, run, walk, idle) loop; every
// other clip plays once and holds its last frame (the split is what the scripts' WAIT_ANIMATION needs). Field characters run their
// sequence.
bool script_anim_loops(int id) { return id >= 1 && id <= 4; }

// Guard against a sequence that loops without a wait (the engine would hang).
constexpr int kSeqOpsPerTick = 256;

// Blend-word curve 1: the clip-blend weight (0..0x1000) closes a quarter of the gap
// per tick: gap = 0x1000 - weight; gap < 4 -> 0x1000, else weight += gap >> 2. Returns the weight after `elapsed` ticks (4096 = 1.0).
int exp_blend_weight(int elapsed) {
  int weight = 0;
  for (int i = 0; i < elapsed && weight < 0x1000; ++i) {
    const int gap = 0x1000 - weight;
    weight = gap < 4 ? 0x1000 : weight + (gap >> 2);
  }
  return weight;
}
// Ticks until that weight reaches 0x1000 (the fade length of an exponential blend).
int exp_blend_ticks() {
  int ticks = 0;
  while (exp_blend_weight(ticks) < 0x1000) ++ticks;
  return ticks;
}

}  // namespace

std::shared_ptr<ModelHandle> Actor::make_model(render::MeshPipeline& pipeline, assets::ModelAsset asset) {
  render::resolve_model_textures(asset);
  return make_resolved_model(pipeline, std::move(asset));
}

std::shared_ptr<ModelHandle> Actor::make_resolved_model(render::MeshPipeline& pipeline, assets::ModelAsset asset, const render::ModelUploads* uploads,
                                                       render::ModelTextures* textures) {
  auto handle = std::make_shared<ModelHandle>();
  handle->asset = std::move(asset);
  handle->gpu = pipeline.create_model(handle->asset, uploads, textures);
  if (!handle->gpu) return nullptr;
  return handle;
}

bool Actor::load(render::MeshPipeline& pipeline, assets::ModelAsset asset) {
  auto handle = make_model(pipeline, std::move(asset));
  if (!handle) return false;
  set_model(std::move(handle));
  return true;
}

void Actor::set_model(std::shared_ptr<ModelHandle> model) {
  model_ = std::move(model);
  ex_override_.clear();
  ex_seq_override_.clear();
  reset_playback();
}

void Actor::reset_playback() {
  // Model init (loop flag 1, vars 0, no sequence) plus room start (nothing submitted yet, then the stand clip submitted).
  clip_ = from_clip_ = -1;
  clip_loops_ = true;
  requested_ = -1;
  busy_ = false;
  fade_prev_ = fade_cur_ = fade_len_ = kCrossFadeTicks;
  fade_curve_ = 0;
  seq_ = Seq{};
  vars_.fill(0);
  clip_loop_ = true;
  seq_mode_ = false;
  hidden_parts_ = near_parts_ = 0;
  head_ = Head{};
  // The instance's shading entries start as the file's S3 (flag bit 0 = invisible, 0x10 = nearest-corner sort).
  if (model_)
    for (size_t k = 0; k < model_->asset.shading.size() && k < 16; ++k) {
      const u8 shading_flags = model_->asset.shading[k].flags;
      if (shading_flags & 1) hidden_parts_ = static_cast<u16>(hidden_parts_ | (1u << k));
      if (shading_flags & 0x10) near_parts_ = static_cast<u16>(near_parts_ | (1u << k));
    }
  submitted_ = -1;
  if (model_) {
    submit(slot_anim_[kSlotStand]);
    run_seq();
  }
}

void Actor::stand_on_clip_zero() {
  slot_anim_[kSlotStand] = 0;
  requested_ = 0;
  requested_loops_ = true;
  busy_ = false;
  if (submitted_ == 0) return;
  clip_ = from_clip_ = -1;
  submit(0);
}

bool Actor::swap_model_art(std::shared_ptr<ModelHandle> model) {
  if (!model || !model_ || !model->gpu) return false;
  if (model->asset.skeleton.joints.size() != model_->asset.skeleton.joints.size() || model->asset.clips.size() != model_->asset.clips.size())
    return false;
  model_ = std::move(model);
  return true;
}

const std::string& Actor::clip_name() const {
  return model_ && clip_ >= 0 ? clip_at(clip_).name : empty_;
}

int Actor::find_clip(int s4_index) const {
  if (!model_) return -1;
  const auto& clips = model_->asset.clips;
  for (size_t i = 0; i < clips.size(); ++i)
    if (clips[i].source == "s4" && clips[i].index == static_cast<u32>(s4_index)) return static_cast<int>(i);
  return clips.empty() ? -1 : 0;
}

int Actor::clip_for_script(int script_anim) const {
  if (!model_) return -1;
  if (script_anim >= kExtraAnimBase) {
    const auto& clips = model_->asset.clips;
    for (size_t i = 0; i < clips.size(); ++i)
      if (clips[i].source == "ex" && clips[i].index == static_cast<u32>(script_anim - kExtraAnimBase)) {
        if (ex_override_.empty()) return static_cast<int>(i);
        break;
      }
    // LOAD_ANIMATION replaced the entity's ex pack: ex clip k is clip k of that pack (indices past the model's own clips).
    if (const size_t k = static_cast<size_t>(script_anim - kExtraAnimBase); k < ex_override_.size())
      return static_cast<int>(clips.size() + k);
    return find_clip(kClipStandBy);
  }
  return find_clip(script_anim);
}

void Actor::set_clip(int clip_index) {
  if (clip_index == clip_) return;
  start_clip(clip_index, kCrossFadeTicks);
}

void Actor::start_clip(int clip_index, int fade) {
  // The new clip starts at keyframe 0; the pose blends from the one that was playing (even the same clip) over `fade`
  // ticks (the weight gains 0x1000 / fade per tick). fade 0 cuts. The blend word's bit 0x8000 makes
  // the source keep running; without it the source time never advances, so the source is frozen at the pose it
  // had when the new clip started (a one-shot held at its last keyframe stays there; it must not wrap to keyframe 0 during the fade).
  if (clip_index < 0) return;
  from_runs_ = (fade & 0x8000) != 0;
  // Bits 0x6000 of the blend word select the weight curve: 1 =
  // exponential, 0 / 2 / 3 = linear. No shipped field or battle sequence sets them.
  const int curve = ((fade >> 13) & 3) == 1 ? 1 : 0;
  fade &= 0x1FFF;
  if (clip_ >= 0 && fade > 0) {
    from_clip_ = clip_;
    from_prev_ = from_runs_ ? t_prev_ : t_cur_;
    from_cur_ = t_cur_;
    fade_prev_ = fade_cur_ = 0;
    fade_curve_ = curve;
    fade_len_ = curve == 1 ? exp_blend_ticks() : fade;
  } else {
    fade_curve_ = 0;
    from_clip_ = -1;
    fade_prev_ = fade_cur_ = fade_len_ = std::max(fade, 1);
  }
  clip_ = clip_index;
  t_prev_ = t_cur_ = 0;
}

void Actor::play(int script_anim) {
  // SET_ANIMATION (3C / A9): model var 0 and the anim-done bit are cleared; 0xFF re-submits the current id.
  vars_[kVarAnimDone] = 0;
  if (script_anim == 0xFF) {
    submitted_ = -1;
  } else {
    requested_ = script_anim;
    requested_loops_ = script_anim < kExtraAnimBase && script_anim_loops(script_anim);
  }
  if (requested_ < 0) {
    busy_ = false;
    return;
  }
  if (seq_entry(requested_)) {
    // Busy until the sequence sets var 0 bit 0.
    busy_ = true;
    return;
  }
  const int clip = clip_for_script(requested_);
  // A one-shot already held at its end clears again on the next tick.
  busy_ = clip >= 0 && !requested_loops_ && clip_length(clip) > 1.0f;
}

bool Actor::has_script_clip(int script_anim) const { return seq_entry(script_anim) != nullptr || clip_for_seq(script_anim) >= 0; }

const std::vector<s16>* Actor::seq_entry(int script_anim) const {
  // Ids < 0x80 index the model's own table (S5), 0x80 + k the ex table. Only models with an S5 table run sequences at all.
  if (!has_anim_seq() || script_anim < 0) return nullptr;
  const assets::AnimSeqTable* table = &model_->asset.anim_seq;
  int k = script_anim;
  if (script_anim >= kExtraAnimBase) {
    table = !ex_override_.empty() ? &ex_seq_override_ : &model_->asset.ex_anim_seq;
    k = script_anim & 0x7F;
  }
  if (k >= static_cast<int>(table->size()) || (*table)[static_cast<size_t>(k)].empty()) return nullptr;
  return &(*table)[static_cast<size_t>(k)];
}

int Actor::clip_for_seq(int clip_id) const {
  // Ids < 0x80 = clip of the model's own pack (S4), 0x80 + k = clip k of the ex pack; past the table = error, no change.
  if (!model_ || clip_id < 0) return -1;
  const auto& clips = model_->asset.clips;
  if (clip_id < kExtraAnimBase) {
    for (size_t i = 0; i < clips.size(); ++i)
      if (clips[i].source == "s4" && clips[i].index == static_cast<u32>(clip_id)) return static_cast<int>(i);
    return -1;
  }
  const size_t k = static_cast<size_t>(clip_id - kExtraAnimBase);
  if (!ex_override_.empty()) return k < ex_override_.size() ? static_cast<int>(clips.size() + k) : -1;
  for (size_t i = 0; i < clips.size(); ++i)
    if (clips[i].source == "ex" && clips[i].index == static_cast<u32>(k)) return static_cast<int>(i);
  return -1;
}

void Actor::submit(int script_anim) {
  // A model with a sequence table starts the id's sequence (pc = entry + header[0], counter / call depth 0);
  // the sequence then starts the clip itself (op 15). Otherwise the clip is started with the 0x20 cross-fade.
  submitted_ = script_anim;
  if (const auto* entry = seq_entry(script_anim)) {
    seq_.id = script_anim;
    seq_.pc = (*entry)[0] / 2;
    seq_.counter = 0;
    seq_.sp = 0;
    seq_mode_ = true;
    return;
  }
  seq_.pc = -1;
  seq_mode_ = false;
  const int clip = clip_for_script(script_anim);
  if (clip >= 0) set_clip(clip);
}

void Actor::run_seq() {
  // Runs handlers (opcode = halfword) until one returns 0 (yield; the same op runs again next
  // tick) or the sequence ends (pc = 0). A handler returns its length in halfwords; jumps move pc first.
  if (seq_.pc < 0 || !model_) return;
  const std::vector<s16>* ep = seq_entry(seq_.id);
  if (!ep) {
    seq_.pc = -1;
    return;
  }
  const std::vector<s16>& ops = *ep;
  s16 dummy = 0;
  for (int guard = 0; guard < kSeqOpsPerTick; ++guard) {
    if (seq_.pc < 0 || seq_.pc >= static_cast<int>(ops.size())) {
      seq_.pc = -1;
      return;
    }
    const int pc = seq_.pc;
    auto arg = [&](int k) -> int { return pc + k < static_cast<int>(ops.size()) ? ops[static_cast<size_t>(pc + k)] : 0; };
    // Model variable (arg & 0x7F).
    auto var = [&](int k) -> s16& {
      const int index = arg(k) & 0x7F;
      return index < kModelVarCount ? vars_[static_cast<size_t>(index)] : dummy;
    };
    // A wait of `ticks` on the counter (the engine subtracts the frame's tick count; here one tick per call).
    auto wait_ticks = [&](int ticks) {
      if (seq_.counter < 1) {
        seq_.counter = static_cast<s16>(seq_.counter + ticks);
        return ticks > 0 && seq_.counter <= 0;
      }
      seq_.counter = static_cast<s16>(seq_.counter - 1);
      return seq_.counter <= 0;
    };
    // Wait until the clip's last keyframe, (frames - 1) * 4 ticks from its start, minus `early`.
    auto wait_clip = [&](int early) {
      if (seq_.counter < 1) {
        const int frames = clip_ >= 0 ? static_cast<s16>(static_cast<int>(clip_at(clip_).frame_count) << 8) >> 8 : 0;
        const int remaining = (frames - 1) * 4 - early;
        if (static_cast<s16>(remaining) > 0) {
          seq_.counter = static_cast<s16>(seq_.counter + remaining);
          return seq_.counter <= 0;
        }
        return true;
      }
      seq_.counter = static_cast<s16>(seq_.counter - 1);
      return seq_.counter <= 0;
    };
    auto test = [&](int lhs, int rhs, int cond) {
      switch (cond) {
        case 0: return lhs == rhs;
        case 1: return lhs < rhs;
        case 2: return lhs > rhs;
        case 3: return lhs <= rhs;
        case 4: return lhs >= rhs;
        case 5: return lhs != rhs;
        case 6: return (lhs & rhs) != 0;
        default: return false;
      }
    };
    int adv = 0;
    const int op = static_cast<u16>(ops[static_cast<size_t>(pc)]);
    switch (op) {
      // end
      case 0x00: case 0x3E: case 0x4C: seq_.pc = -1; return;
      // jump: pc + off + 1
      case 0x01: seq_.pc += arg(1); adv = 1; break;
      // call: push pc, pc + off + 1
      case 0x02:
        if (seq_.sp < static_cast<int>(seq_.stack.size())) seq_.stack[static_cast<size_t>(seq_.sp++)] = pc;
        seq_.pc += arg(1);
        adv = 1;
        break;
      // return: past the call
      case 0x03:
        if (seq_.sp > 0) seq_.pc = seq_.stack[static_cast<size_t>(--seq_.sp)];
        adv = 2;
        break;
      // if: off 0 = wait until true
      case 0x04: adv = test(var(1), var(2), arg(3)) ? 5 : arg(4); break;
      case 0x06: adv = test(var(1), var(2), arg(3)) ? 5 : arg(4) + 2; break;
      // jumps
      case 0x05: case 0x07: seq_.pc += arg(1); adv = 2; break;
      // no-op / debug print / rate (unused in the field)
      case 0x08: case 0x3C: case 0x3D: case 0x16: adv = 2; break;
      case 0x32: adv = 4; break;
      case 0x1E: adv = 8; break;
      // wait n ticks (n = 0: hold for ever)
      case 0x09: adv = wait_ticks(arg(1)) ? 2 : 0; break;
      // wait for the clip's end
      case 0x0A: adv = wait_clip(0) ? 1 : 0; break;
      case 0x42: adv = wait_clip(arg(1)) ? 2 : 0; break;
      // play clip, blend word (ticks | 0x8000 source runs)
      case 0x15: start_clip(clip_for_seq(arg(1)), arg(2) & 0xFFFF); adv = 3; break;
      case 0x3F: start_clip(clip_for_seq(var(1)), var(2) & 0xFFFF); adv = 3; break;
      // loop flag on / off
      case 0x17: clip_loop_ = true; adv = 1; break;
      case 0x18: clip_loop_ = false; adv = 1; break;
      // not used in the field
      case 0x19: case 0x1A: adv = 1; break;
      case 0x1B: hidden_parts_ = static_cast<u16>(hidden_parts_ & ~(1u << (arg(1) & 15))); adv = 2; break;
      case 0x1C: hidden_parts_ = static_cast<u16>(hidden_parts_ | (1u << (arg(1) & 15))); adv = 2; break;
      // room emitters `name` on attach point pt (FieldWorld::step_bone_emitters)
      case 0x1D: {
        BoneEmitterRequest r;
        for (int k = 0; k < 4; ++k) {
          const char ch = static_cast<char>((arg(3 + k / 2) >> (8 * (k & 1))) & 0xFF);
          if (ch) r.name.push_back(ch);
        }
        r.flags = arg(1) & 0xFF;
        r.point = arg(2);
        for (int k = 0; k < 3; ++k) {
          r.off[static_cast<size_t>(k)] = static_cast<s16>(arg(5 + k));
          r.ang[static_cast<size_t>(k)] = static_cast<s16>(arg(8 + k));
        }
        bone_emitters_.push_back(std::move(r));
        adv = 11;
        break;
      }
      // shading entry k flag 0x10 (nearest-corner sort) on / off
      case 0x93:
        if (arg(2) != 0) near_parts_ = static_cast<u16>(near_parts_ | (1u << (arg(1) & 15)));
        else near_parts_ = static_cast<u16>(near_parts_ & ~(1u << (arg(1) & 15)));
        adv = 3;
        break;
      // var b = rsin / rcos(immediate angle & 0xFFF), 4.12
      case 0x2D: case 0x2E: {
        const double a = static_cast<double>(arg(1) & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
        var(2) = static_cast<s16>(std::lround((op == 0x2D ? std::sin(a) : std::cos(a)) * 4096.0));
        adv = 3;
        break;
      }
      // var c = ratan2(immediate y, immediate x), 4096 per turn
      case 0x2F:
        var(3) = static_cast<s16>(std::lround(std::atan2(static_cast<double>(arg(2)), static_cast<double>(arg(1))) * (2048.0 / 3.14159265358979323846)));
        adv = 4;
        break;
      // var c = |(vars b, b+1) - (vars a, a+1)| >> 12 (x, z)
      case 0x31: {
        const int var_a = arg(1) & 0x7F, var_b = arg(2) & 0x7F;
        auto var_value = [&](int index) { return index < kModelVarCount ? static_cast<int>(vars_[static_cast<size_t>(index)]) : 0; };
        const s64 dx = (var_value(var_b) - var_value(var_a)) >> 12, dz = (var_value(var_b + 1) - var_value(var_a + 1)) >> 12;
        var(3) = static_cast<s16>(static_cast<s64>(std::sqrt(static_cast<double>(dx * dx + dz * dz))));
        adv = 4;
        break;
      }
      case 0x26: ++var(1); adv = 2; break;
      case 0x27: --var(1); adv = 2; break;
      case 0x28: var(1) = static_cast<s16>(var(1) + var(2)); adv = 3; break;
      case 0x29: var(1) = static_cast<s16>(var(1) - var(2)); adv = 3; break;
      case 0x2A: if (var(2) != 0) var(1) = static_cast<s16>(var(1) / var(2)); adv = 3; break;
      case 0x2B: var(1) = static_cast<s16>(var(1) * var(2)); adv = 3; break;
      case 0x2C: if (var(2) != 0) var(1) = static_cast<s16>(var(1) % var(2)); adv = 3; break;
      case 0x76: var(1) = static_cast<s16>(var(1) & var(2)); adv = 3; break;
      case 0x77: var(1) = static_cast<s16>(var(1) | var(2)); adv = 3; break;
      // set var, immediate
      case 0x40: var(1) = static_cast<s16>(arg(2)); adv = 3; break;
      // copy var
      case 0x4B: var(1) = var(2); adv = 3; break;
      case 0x30: if (arg(1) != 0) var(2) = static_cast<s16>(rand_(0x7FFF) % arg(1)); adv = 3; break;
      default:
        if (!seq_warned_)
          log::warn("actor: anim sequence id {:#x}: opcode {:#x} at {} not implemented, sequence stopped", seq_.id, op, pc);
        seq_warned_ = true;
        seq_.pc = -1;
        return;
    }
    // Yield; the op runs again next tick.
    if (adv == 0) return;
    seq_.pc += adv;
  }
}

void Actor::place(const WalkMesh& walk, int tri, float x, float z, int facing) {
  tri_ = tri;
  cur_.x = x;
  cur_.z = z;
  cur_.y = tri >= 0 ? walk.height(tri, x, z) : 0.0f;
  facing_ = facing & 0xFFF;
  cur_.yaw = static_cast<float>((facing_ - kModelYawBias) & 0xFFF);
  prev_ = cur_;
  moving_ = false;
  reset_smooth();
  reset_playback();
}

void Actor::copy_pose(const Actor& from) {
  cur_.x = from.cur_.x;
  cur_.y = from.cur_.y;
  cur_.z = from.cur_.z;
  cur_.yaw = from.cur_.yaw;
  tri_ = from.tri_;
  tile_flags_ = from.tile_flags_;
  facing_ = from.facing_;
  prev_ = cur_;
  reset_smooth();
}

void Actor::set_position(const WalkMesh& walk, float x, float z, bool teleport, bool free_y) {
  cur_.x = x;
  cur_.z = z;
  if (!walk.empty()) {
    const int tri = walk.find_triangle(x, z, cur_.y);
    if (tri >= 0) {
      tri_ = tri;
      if (!free_y) cur_.y = walk.height(tri, x, z);
    }
  }
  if (teleport) {
    reset_smooth();
    prev_.x = cur_.x;
    prev_.y = cur_.y;
    prev_.z = cur_.z;
  }
}

void Actor::set_height(float y, bool teleport) {
  cur_.y = y;
  if (teleport) {
    prev_.y = y;
    reset_smooth();
  }
}

bool Actor::move_on_mesh(const WalkMesh& walk, float dx, float dz) {
  if (tri_ < 0 || walk.empty()) {
    set_position(walk, cur_.x + dx, cur_.z + dz, false, false);
    return true;
  }
  const float ox = cur_.x, oz = cur_.z;
  slope_step(walk, dx, dz);
  return std::fabs(cur_.x - ox) > 1e-4f || std::fabs(cur_.z - oz) > 1e-4f;
}

// The end of the engine's mover (every actor): after the probe / wall-slide the accepted xz move d is re-made as a 3D move along
// the plane. Its length L is taken in sixteenths of a unit, each axis truncated toward zero, through mover_sqrt (a 2-unit step at
// a slight angle, 31 and 1 sixteenths, is 30 / 16). The direction (d, dy) is normalised (mover_normalise, from d in 1/256 units)
// and scaled by L, where dy is the difference of the whole-unit floor heights (WalkMesh::floor_height) at the end of the move and
// at the start. On a gentle slope dy is 0 or 1 from tick to tick, so a step with a rise is shortened to L * |d| / |(d, 1)|, and a
// ladder (130 units across for 1270 of height) is climbed at L instead of shooting up 10x faster. The position is then found
// again with the new move and the height follows the plane.
bool Actor::slope_step(const WalkMesh& walk, float dx, float dz) {
  const float ox = cur_.x, oz = cur_.z;
  const int otri = tri_;
  auto step = walk.move_actor(otri, ox, oz, dx, dz, class_bits, kProbeAhead);
  const float mdx = step.x - ox, mdz = step.z - oz;
  if (std::hypot(mdx, mdz) > 1e-4f && step.tri >= 0) {
    const int sx = static_cast<int>(mdx * 16.0f), sz = static_cast<int>(mdz * 16.0f);
    const int length = mover_sqrt(sx * sx + sz * sz);
    const int dy = walk.floor_height(step.tri, step.x, step.z) - walk.floor_height(otri, ox, oz);
    // Normalised backwards, as the engine does: the rounding of the shift then falls the same way.
    const int dir[3] = {static_cast<int>(-mdx * 256.0f), dy * 256, static_cast<int>(-mdz * 256.0f)};
    int unit[3];
    mover_normalise(dir, unit);
    const float ndx = static_cast<float>(-length * unit[0]) / 65536.0f, ndz = static_cast<float>(-length * unit[2]) / 65536.0f;
    if (std::fabs(ndx - mdx) > 1e-5f || std::fabs(ndz - mdz) > 1e-5f) {
      // the new move goes through the same mover (wall probe ahead + slide): a plain move() let the actor creep closer to a wall
      // than move_actor allows and wedge in corners on gentle slopes
      const auto m2 = walk.move_actor(otri, ox, oz, ndx, ndz, class_bits, kProbeAhead);
      if (m2.tri >= 0 && !m2.blocked) {
        step.tri = m2.tri;
        step.x = m2.x;
        step.z = m2.z;
      }
    }
  }
  tri_ = step.tri;
  cur_.x = step.x;
  cur_.z = step.z;
  cur_.y = walk.height(tri_, step.x, step.z);
  if (step.slid) facing_ = static_cast<int>(std::lround(std::atan2(step.slide_dz, -step.slide_dx) / kUnitsToRad)) & 0xFFF;
  return step.slid;
}

void Actor::update_ground_tilt(const WalkMesh& walk, bool enabled, bool snap) {
  if (!enabled || tilt_world || tri_ < 0 || walk.empty()) {
    gt_on_ = false;
    return;
  }
  float normal[3];
  float target[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  if (walk.normal_up(tri_, normal) && normal[1] < 0.9999f) {
    // Shortest rotation taking up (0, 1, 0) to the normal: axis a = up x n = (nz, 0, -nx), cos = ny; R = I + K + K^2 / (1 + cos).
    const float ax = normal[2], az = -normal[0], k = 1.0f / (1.0f + normal[1]);
    const float skew[9] = {0, -az, 0, az, 0, -ax, 0, ax, 0};
    float skew_sq[9];
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col)
        skew_sq[row * 3 + col] = skew[row * 3] * skew[col] + skew[row * 3 + 1] * skew[3 + col] + skew[row * 3 + 2] * skew[6 + col];
    for (int i = 0; i < 9; ++i) target[i] = (i % 4 == 0 ? 1.0f : 0.0f) + skew[i] + skew_sq[i] * k;
  }
  if (!gt_on_ || snap) {
    for (int i = 0; i < 9; ++i) gt_[i] = target[i];
  } else {
    // Per-tick step limit on each matrix element.
    constexpr float kStep = 0xAA / 4096.0f;
    for (int i = 0; i < 9; ++i) gt_[i] += std::clamp(target[i] - gt_[i], -kStep, kStep);
  }
  gt_on_ = true;
}

void Actor::set_tile(const WalkMesh& walk, u32 tile_flags) {
  tile_flags_ = tile_flags;
  if (!(tile_flags & 0xC000u)) return;
  float nx, nz;
  if (!walk.normal_xz(tri_, nx, nz) || std::hypot(nx, nz) < 1e-3f) return;
  // engine facing = atan2(-Nz, Nx); in the actor's space (engine + 0x800) that is atan2(Nz, -Nx).
  facing_ = static_cast<int>(std::lround(std::atan2(nz, -nx) / kUnitsToRad)) & 0xFFF;
}

const assets::AnimationClip& Actor::clip_at(int idx) const {
  const auto& clips = model_->asset.clips;
  const size_t i = static_cast<size_t>(idx);
  return i < clips.size() ? clips[i] : ex_override_[i - clips.size()];
}

void Actor::set_ex_clips(std::vector<assets::AnimationClip> clips, assets::AnimSeqTable seq) {
  clear_ex_clips();
  ex_override_ = std::move(clips);
  ex_seq_override_ = std::move(seq);
}

void Actor::clear_ex_clips() {
  if (ex_override_.empty()) return;
  const int clip_count = model_ ? static_cast<int>(model_->asset.clips.size()) : 0;
  const bool in_use = clip_ >= clip_count || from_clip_ >= clip_count || (seq_.pc >= 0 && seq_.id >= kExtraAnimBase);
  ex_override_.clear();
  ex_seq_override_.clear();
  if (in_use && model_) {
    // Put the stand clip back before the pack goes.
    clip_ = from_clip_ = -1;
    requested_ = -1;
    busy_ = false;
    seq_.pc = -1;
    submit(slot_anim_[kSlotStand]);
    run_seq();
  }
}

float Actor::clip_length(int clip) const {
  if (clip < 0 || !model_) return 0;
  return static_cast<float>(clip_at(clip).frame_count);
}

bool Actor::turning() const {
  const float diff = wrap_units(static_cast<float>(facing_ - kModelYawBias) - cur_.yaw);
  return std::fabs(diff) > 0.5f;
}

void Actor::turn_step() {
  // Chase the facing: yaw -> facing - 0x400 by turn_rate per tick, snapping when within one step. The yaw turns down while it is
  // less than half a turn above the target, else up, so an exact half turn goes up.
  // A turn rate of 0 means the engine never touches the yaw.
  if (turn_rate <= 0) return;
  float above = std::fmod(cur_.yaw - static_cast<float>(facing_ - kModelYawBias), 4096.0f);
  if (above < 0) above += 4096.0f;
  const float diff = above < 2048.0f ? -above : 4096.0f - above;
  const float step = static_cast<float>(turn_rate);
  float y = std::fabs(diff) <= step ? cur_.yaw + diff : cur_.yaw + (diff < 0 ? -step : step);
  y = std::fmod(y, 4096.0f);
  cur_.yaw = y < 0 ? y + 4096.0f : y;
}

void Actor::advance_anim(bool loop_current) {
  // Animation: 0.25 keyframe per tick. The cross-fade source keeps playing (looping) only for a blend word with bit 0x8000; otherwise
  // it holds its time. On a stair tile the step follows the whole-unit height instead: forwards while it falls (climbing), backwards
  // while it rises, none while it stays.
  float rate = clip_reverse_ ? -anim_rate : anim_rate;
  if (tile_flags_ & 0xC000u) {
    const int whole_y = static_cast<int>(std::floor(cur_.y));
    rate = whole_y == stair_prev_y_ ? 0.0f : whole_y < stair_prev_y_ ? anim_rate : -anim_rate;
    stair_prev_y_ = whole_y;
  }
  auto advance = [&](int clip, float& tp, float& tc, bool loop) {
    tp = tc;
    if (clip < 0) return;
    const float len = clip_length(clip);
    tc += kKeyframesPerTick * rate;
    if (len <= 0) return;
    if (loop) {
      if (tc >= len) {
        tc -= len;
        tp -= len;
      } else if (tc < 0) {
        tc += len;
        tp += len;
      }
    } else if (rate < 0) {
      if (tc <= 0) {
        tc = 0;
        tp = std::max(tp, tc);
        if (clip == clip_) busy_ = false;
      }
    } else if (tc >= len - 1.0f) {
      tc = std::max(0.0f, len - 1.0f);
      tp = std::min(tp, tc);
      // With a sequence the anim-done bit comes from model var 0 only.
      if (clip == clip_ && !seq_mode_) busy_ = false;
    }
  };
  advance(clip_, t_prev_, t_cur_, loop_current);
  if (from_runs_) advance(from_clip_, from_prev_, from_cur_, true);
  fade_prev_ = fade_cur_;
  if (fade_cur_ < fade_len_) ++fade_cur_;
}

bool Actor::attach_world(int point, float out[3]) const {
  if (!model_ || point < 0 || point >= static_cast<int>(model_->asset.attach.points.size())) return false;
  const assets::AttachPoint& ap = model_->asset.attach.points[static_cast<size_t>(point)];
  std::vector<render::Mat4> pal;
  palette(1.0f, pal);
  if (ap.joint >= pal.size()) return false;
  // A world POSITION: where the joint is drawn, i.e. the render matrix (the model is drawn with yaw + 0x800, vertices and attach points share the
  // model space). Not engine_matrix: that negates x / z of the offset from the actor (right for directions, mirrored for a point away from the
  // axis).
  const render::Mat4 joint_world = model_matrix(1.0f) * pal[ap.joint];
  for (int r = 0; r < 3; ++r)
    out[r] = joint_world.at(r, 0) * ap.pos[0] + joint_world.at(r, 1) * ap.pos[1] + joint_world.at(r, 2) * ap.pos[2] + joint_world.at(r, 3);
  return true;
}

namespace {
// Rotation part of `matrix` with its columns normalised (removes the model scale), row-major.
void unscaled_rotation(const render::Mat4& matrix, float out[9]) {
  for (int col = 0; col < 3; ++col) {
    const float length = std::sqrt(matrix.at(0, col) * matrix.at(0, col) + matrix.at(1, col) * matrix.at(1, col) + matrix.at(2, col) * matrix.at(2, col));
    for (int r = 0; r < 3; ++r) out[r * 3 + col] = length > 0.0f ? matrix.at(r, col) / length : 0.0f;
  }
}
// Normalises vec (a zero vector stays zero).
void normalise3(float vec[3]) {
  const float length = std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
  if (length > 0.0f)
    for (int i = 0; i < 3; ++i) vec[i] /= length;
}
// The angles {ax, 0, az} of RotMatrix that turn +y into the unit vector dir: ax = ratan2(dir.z, dir.y), then with
// w = RotMatrix({-ax, 0, 0}) dir = (dir.x, |(dir.y, dir.z)|, 0): az = -ratan2(w.x, w.y). 4096 per turn.
void head_angles(const float dir[3], float& ax, float& az) {
  constexpr float k = 4096.0f / (2.0f * kPi);
  ax = std::atan2(dir[2], dir[1]) * k;
  az = -std::atan2(dir[0], std::sqrt(dir[1] * dir[1] + dir[2] * dir[2])) * k;
}
// RotMatrix({ax, 0, az}) = Rx(ax) * Rz(az) (PsyQ: z applied first), row-major.
void head_matrix(float ax, float az, float out[9]) {
  const float cx = std::cos(ax * kUnitsToRad), sx = std::sin(ax * kUnitsToRad);
  const float cz = std::cos(az * kUnitsToRad), sz = std::sin(az * kUnitsToRad);
  const float matrix[9] = {cz, -sz, 0, cx * sz, cx * cz, -sx, sx * sz, sx * cz, cx};
  std::copy(matrix, matrix + 9, out);
}
}  // namespace

bool Actor::attach_frame(int point, float rot[9], float pos[3]) const {
  if (!model_ || point < 0 || point >= static_cast<int>(model_->asset.attach.points.size())) return false;
  const assets::AttachPoint& ap = model_->asset.attach.points[static_cast<size_t>(point)];
  std::vector<render::Mat4> pal;
  palette(1.0f, pal);
  if (ap.joint >= pal.size()) return false;
  const render::Mat4 joint_world = model_matrix(1.0f) * pal[ap.joint];
  unscaled_rotation(joint_world, rot);
  for (int r = 0; r < 3; ++r)
    pos[r] = joint_world.at(r, 0) * ap.pos[0] + joint_world.at(r, 1) * ap.pos[1] + joint_world.at(r, 2) * ap.pos[2] + joint_world.at(r, 3);
  return true;
}

void Actor::model_rotation(float rot[9]) const { unscaled_rotation(model_matrix(1.0f), rot); }

render::Mat4 Actor::engine_matrix() const {
  render::Mat4 matrix = model_matrix(1.0f);
  if (attached_) return matrix;
  for (int col = 0; col < 3; ++col) {
    matrix.at(0, col) = -matrix.at(0, col);
    matrix.at(2, col) = -matrix.at(2, col);
  }
  return matrix;
}

bool Actor::attach_joint(int point, float alpha, render::Mat4& joint_world, float offset[3]) const {
  if (!model_ || point < 0 || point >= static_cast<int>(model_->asset.attach.points.size())) return false;
  const assets::AttachPoint& ap = model_->asset.attach.points[static_cast<size_t>(point)];
  std::vector<render::Mat4> pal;
  palette(alpha, pal);
  if (ap.joint >= pal.size()) return false;
  joint_world = model_matrix(alpha) * pal[ap.joint];
  for (int i = 0; i < 3; ++i) offset[i] = ap.pos[static_cast<size_t>(i)];
  return true;
}

void Actor::set_attached(const Actor* parent, int point, const render::Mat4& local) {
  attach_parent_ = parent;
  attach_point_ = point;
  attach_local_ = local;
  attached_ = parent != nullptr;
}

int Actor::head_joint() const {
  // The head joint is attach point 1's joint.
  if (!model_ || model_->asset.attach.points.size() < 2) return -1;
  const int j = model_->asset.attach.points[1].joint;
  return j < static_cast<int>(model_->asset.skeleton.joints.size()) ? j : -1;
}

void Actor::head_look_at(const float target_world[3], int rate) {
  // A rate in 1..0xFFF replaces the stored rate; otherwise the old one is kept.
  if (rate >= 1 && rate <= 0xFFF) head_.rate = rate;
  std::copy(target_world, target_world + 3, head_.target);
  // Already turning: only the target / rate change (the "returning" bit is kept).
  if (head_.active) return;
  const int j = head_joint();
  if (j < 0) return;
  // Not turning yet: the start direction is the model's forward axis (model rotation * entity matrix * (0, 0, 0x1000)) in the
  // frame of the head joint (parent composite x animated local; scale removed, transposed), normalised.
  std::vector<Local> loc;
  locals(1.0f, loc);
  // The head frame uses the rest local.
  loc[static_cast<size_t>(j)] = {model_->asset.skeleton.joints[static_cast<size_t>(j)].rest_rot_deg,
                                 model_->asset.skeleton.joints[static_cast<size_t>(j)].rest_trans};
  std::vector<render::Mat4> pal;
  compose(loc, 1.0f, false, pal);
  const int parent = model_->asset.skeleton.joints[static_cast<size_t>(j)].parent;
  const render::Mat4 local = render::mat4_from_rt(assets::rotation_zyx(loc[static_cast<size_t>(j)].rot), loc[static_cast<size_t>(j)].trans);
  const render::Mat4 eng = engine_matrix();
  const render::Mat4 g = eng * (parent >= 0 ? pal[static_cast<size_t>(parent)] * local : local);
  float gr[9], wr[9];
  unscaled_rotation(g, gr);
  unscaled_rotation(eng, wr);
  // World forward = column z.
  const float fwd[3] = {wr[2], wr[5], wr[8]};
  for (int i = 0; i < 3; ++i) head_.cur[i] = gr[0 * 3 + i] * fwd[0] + gr[1 * 3 + i] * fwd[1] + gr[2 * 3 + i] * fwd[2];
  normalise3(head_.cur);
  head_angles(head_.cur, head_.ax, head_.az);
  head_.pax = head_.ax;
  head_.paz = head_.az;
  head_.active = true;
  head_.returning = false;
  head_.moving = true;
}

void Actor::head_reset() {
  // Only while the head turn is active: target = attach point 1 + (entity matrix * model rotation) (0, 0, -0x1000),
  // i.e. straight ahead of the face, and bit 2 = release once settled.
  if (!head_.active) return;
  if (!model_ || model_->asset.attach.points.size() < 2) return;
  const assets::AttachPoint& ap = model_->asset.attach.points[1];
  std::vector<render::Mat4> pal;
  palette(1.0f, pal);
  if (ap.joint >= pal.size()) return;
  const render::Mat4 eng = engine_matrix();
  const render::Mat4 hm = eng * pal[ap.joint];
  float head[3], wr[9];
  for (int r = 0; r < 3; ++r) head[r] = hm.at(r, 0) * ap.pos[0] + hm.at(r, 1) * ap.pos[1] + hm.at(r, 2) * ap.pos[2] + hm.at(r, 3);
  unscaled_rotation(eng, wr);
  for (int i = 0; i < 3; ++i) head_.target[i] = head[i] - 4096.0f * wr[i * 3 + 2];
  head_.returning = true;
}

void Actor::head_step() {
  // The head joint's callback, run while the local matrices are built, once per tick here (the engine loops once per
  // elapsed tick of the frame):
  //   d = normalise(attach point 1 - target), taken into the head joint's frame M = parent composite x animated local (scale removed);
  //   (tx, tz) = angles(d), (cx, cz) = angles(cur); z range = [max(min(tz, cz), -0x180), min(max(tz, cz), 0x200)];
  //   cur = normalise((cur * rate + d * (0x1000 - rate)) >> 12); (ax, az) = angles(cur), ax clamped to +-0x200, az to the z range;
  //   cur = RotMatrix({ax, 0, az}) (0, 0x1000, 0); the joint's local matrix = RotMatrix({ax, 0, az}) x animated local rotation, the
  //   animated translation kept. A change below 0x40 / 0x1000 on x and z clears the moving bit; with the returning bit set the
  //   callback is then removed (the head follows the animation again).
  head_.pax = head_.ax;
  head_.paz = head_.az;
  if (!head_.active || !model_) return;
  const int j = head_joint();
  if (j < 0) {
    head_ = Head{};
    return;
  }
  std::vector<Local> loc;
  locals(1.0f, loc);
  // The head frame uses the rest local.
  loc[static_cast<size_t>(j)] = {model_->asset.skeleton.joints[static_cast<size_t>(j)].rest_rot_deg,
                                 model_->asset.skeleton.joints[static_cast<size_t>(j)].rest_trans};
  std::vector<render::Mat4> pal;
  compose(loc, 1.0f, true, pal);
  const auto& jt = model_->asset.skeleton.joints[static_cast<size_t>(j)];
  const assets::AttachPoint& ap = model_->asset.attach.points[1];
  const render::Mat4 engine = engine_matrix();
  const render::Mat4 hm = engine * pal[static_cast<size_t>(j)];
  float to_target[3];
  for (int r = 0; r < 3; ++r)
    to_target[r] = hm.at(r, 0) * ap.pos[0] + hm.at(r, 1) * ap.pos[1] + hm.at(r, 2) * ap.pos[2] + hm.at(r, 3) - head_.target[r];
  normalise3(to_target);
  const render::Mat4 local = render::mat4_from_rt(assets::rotation_zyx(loc[static_cast<size_t>(j)].rot), loc[static_cast<size_t>(j)].trans);
  const render::Mat4 g = engine * (jt.parent >= 0 ? pal[static_cast<size_t>(jt.parent)] * local : local);
  float gr[9], head_dir[3];
  unscaled_rotation(g, gr);
  for (int i = 0; i < 3; ++i)
    head_dir[i] = gr[0 * 3 + i] * to_target[0] + gr[1 * 3 + i] * to_target[1] + gr[2 * 3 + i] * to_target[2];
  normalise3(head_dir);
  float tx, tz, cx, cz;
  head_angles(head_dir, tx, tz);
  head_angles(head_.cur, cx, cz);
  const float zlo = std::max(std::min(tz, cz), -384.0f), zhi = std::min(std::max(tz, cz), 512.0f);
  const float old[3] = {head_.cur[0], head_.cur[1], head_.cur[2]};
  const float kc = static_cast<float>(head_.rate) / 4096.0f;
  float blended[3];
  for (int i = 0; i < 3; ++i) blended[i] = head_.cur[i] * kc + head_dir[i] * (1.0f - kc);
  normalise3(blended);
  float ax, az;
  head_angles(blended, ax, az);
  ax = std::clamp(ax, -512.0f, 512.0f);
  // The engine tests the upper bound first.
  if (az > zhi) az = zhi;
  else if (az < zlo) az = zlo;
  float rm[9];
  head_matrix(ax, az, rm);
  head_.cur[0] = rm[1];
  head_.cur[1] = rm[4];
  head_.cur[2] = rm[7];
  head_.ax = ax;
  head_.az = az;
  constexpr float kSettle = 64.0f / 4096.0f;
  if (std::fabs(head_.cur[0] - old[0]) < kSettle && std::fabs(head_.cur[2] - old[2]) < kSettle) {
    head_.moving = false;
    if (head_.returning) head_ = Head{.rate = head_.rate};
  } else {
    head_.moving = true;
  }
}

void Actor::step_visual(bool moving, bool run) {
  moving_ = moving;
  if (!inactive_) pick_clip(moving, run);
  // An off-screen entity neither runs its sequence, nor advances its clip, nor builds the head turn (see set_culled).
  if (culled_) return;
  // The sequence runs once per frame, before the playback step; its var 0 bit 0 ORs into the anim-done bit.
  run_seq();
  if (seq_mode_ && (vars_[kVarAnimDone] & 1)) busy_ = false;
  advance_anim(seq_mode_ ? clip_loop_ : clip_loops_);
  // The head joint's callback runs inside the pose build, after the clip time moved.
  head_step();
}

void Actor::pick_clip(bool moving, bool run) {
  turn_step();
  int want;
  clip_loops_ = true;
  if (moving) {
    idle_timer_ = rand_(0x3FF) + 0x200;
    requested_ = -1;
    busy_ = false;
    want = slot_anim_[run ? kSlotRun : kSlotWalk];
  } else if (requested_ >= 0) {
    want = requested_;
    clip_loops_ = requested_loops_;
  } else {
    int slot = kSlotStand;
    if (auto_idle) {
      if (idle_timer_ < 1) {
        slot = idle_pick_;
        if (idle_alt_ < 1) {
          idle_alt_ = rand_(0x3FF) + 0x100;
          idle_pick_ = (rand_(1) & 1) ? kSlotIdle2 : kSlotStand;
        } else {
          --idle_alt_;
        }
      } else {
        --idle_timer_;
      }
    }
    want = slot_anim_[slot];
    // The stand id becomes the current id: a later SET_WALK_ANIMATION of the stand slot does not change the clip of a standing entity
    // until it walks, is shown again or a script asks for a clip.
    if (!auto_idle && !(tile_flags_ & 0xC000u)) {
      requested_ = want;
      requested_loops_ = true;
    }
  }
  // A stair / ladder tile (0xC000) forces the stairs-down slot (flag 0x8000 set) or the stairs-up slot, moving or standing.
  if ((tile_flags_ & 0xC000u) && requested_ < 0) want = slot_anim_[(tile_flags_ & 0x8000u) ? kSlotStairsDown : kSlotStairsUp];
  // A new id is submitted only when it differs from the last one and, if it is the stand id, only once
  // the running cross-fade is over. That is how short
  // scripted hops (a walk that arrives after a tick or two, then waits) keep the walk clip instead of flicking between walk and stand.
  const bool fading = from_clip_ >= 0 && fade_cur_ < fade_len_;
  const bool defer_stand = want != submitted_ && clip_ >= 0 && fading && want == slot_anim_[kSlotStand];
  if (want >= 0 && want != submitted_ && !defer_stand) submit(want);
}

void Actor::tick(const WalkMesh& walk, int heading, bool run) {
  prev_ = cur_;
  pad_step_[0] = pad_step_[1] = 0.0f;
  const bool moving = heading != kNoHeading;
  // No running on stairs / ladders / slide tiles.
  if ((tile_flags_ & 0xC800u) && !force_run) run = false;
  if (moving && (tri_ >= 0 || noclip)) {
    const float heading_rad = static_cast<float>(heading & 0xFFF) * kUnitsToRad;
    // 16.16 velocity -> units; a tile with flag 0x1000 halves the velocity.
    float step = static_cast<float>(run ? speed * 2 + (speed >> 1) : speed) / 16.0f;
    if (tile_flags_ & 0x1000u) step *= 0.5f;
    // Screen-correct with the script yaw.
    const float dx = -std::cos(heading_rad) * step, dz = std::sin(heading_rad) * step;
    pad_step_[0] = dx;
    pad_step_[1] = dz;
    if (noclip) {
      // Cheats: no wall, no edge; y follows the mesh while a triangle is under the actor.
      set_position(walk, cur_.x + dx, cur_.z + dz, false);
      facing_ = heading & 0xFFF;
    } else if (!slope_step(walk, dx, dz)) {
      facing_ = heading & 0xFFF;
    }
  }
  if (tri_ >= 0) set_tile(walk, walk.flags(tri_));
  step_visual(moving, run);
}

void Actor::end_tick() {
  // A change of the pose spreads over the ticks since the previous change (1..4; a longer rest means a fresh move, span 1). The
  // segment starts where the display was at the end of the previous tick, so it is continuous whatever the cadence.
  auto close = [&](Chan& chan, const float* cur, int count, bool yaw, bool warp) {
    bool moved = false;
    for (int i = 0; i < count; ++i) moved = moved || std::fabs(cur[i] - chan.ref[i]) > 1e-4f;
    if (warp) {
      for (int i = 0; i < count; ++i) chan.from[i] = chan.to[i] = chan.ref[i] = cur[i];
      chan.span = 1;
      chan.age = 0;
      chan.gap = 1000;
      return;
    }
    if (moved) {
      const float blend = std::clamp(static_cast<float>(chan.age + 1) / static_cast<float>(chan.span), 0.0f, 1.0f);
      float start[3]{}, dist2 = 0;
      for (int i = 0; i < count; ++i) {
        start[i] = yaw ? chan.from[i] + wrap_units(chan.to[i] - chan.from[i]) * blend : chan.from[i] + (chan.to[i] - chan.from[i]) * blend;
        const float diff = cur[i] - start[i];
        if (!yaw) dist2 += diff * diff;
      }
      if (!yaw && dist2 > 150.0f * 150.0f)
        for (int i = 0; i < count; ++i) start[i] = cur[i];
      for (int i = 0; i < count; ++i) {
        chan.from[i] = start[i];
        chan.to[i] = cur[i];
      }
      chan.span = chan.gap + 1 <= 4 ? chan.gap + 1 : 1;
      chan.age = 0;
      chan.gap = 0;
    } else {
      ++chan.age;
      chan.gap = std::min(chan.gap + 1, 1000);
    }
    for (int i = 0; i < count; ++i) chan.ref[i] = cur[i];
  };
  const float pos[3] = {cur_.x, cur_.y, cur_.z};
  const float yw[1] = {cur_.yaw};
  close(smooth_pos_, pos, 3, false, smooth_warp_);
  close(smooth_yaw_, yw, 1, true, smooth_warp_);
  smooth_warp_ = false;
  smooth_on_ = true;
}

ActorPose Actor::pose(float alpha) const {
  ActorPose result;
  if (smooth_on_ && !smooth_warp_) {
    const Chan& pos_chan = smooth_pos_;
    const float blend = std::clamp((static_cast<float>(pos_chan.age) + alpha) / static_cast<float>(pos_chan.span), 0.0f, 1.0f);
    result.x = pos_chan.from[0] + (pos_chan.to[0] - pos_chan.from[0]) * blend;
    result.y = pos_chan.from[1] + (pos_chan.to[1] - pos_chan.from[1]) * blend;
    result.z = pos_chan.from[2] + (pos_chan.to[2] - pos_chan.from[2]) * blend;
    const Chan& yaw_chan = smooth_yaw_;
    const float yaw_blend = std::clamp((static_cast<float>(yaw_chan.age) + alpha) / static_cast<float>(yaw_chan.span), 0.0f, 1.0f);
    float y = std::fmod(yaw_chan.from[0] + wrap_units(yaw_chan.to[0] - yaw_chan.from[0]) * yaw_blend, 4096.0f);
    result.yaw = y < 0 ? y + 4096.0f : y;
    return result;
  }
  result.x = prev_.x + (cur_.x - prev_.x) * alpha;
  result.y = prev_.y + (cur_.y - prev_.y) * alpha;
  result.z = prev_.z + (cur_.z - prev_.z) * alpha;
  float y = prev_.yaw + wrap_units(cur_.yaw - prev_.yaw) * alpha;
  y = std::fmod(y, 4096.0f);
  result.yaw = y < 0 ? y + 4096.0f : y;
  return result;
}

Actor::Axes Actor::axes_at(float alpha) const {
  const Axes now = axes_now();
  if (!axes_prev_ok_) return now;
  Axes result = now;
  auto angle = [&](float prev, float cur) {
    const float diff = wrap_units(cur - prev);
    return std::fabs(diff) > kAxisSnapUnits ? cur : prev + diff * alpha;
  };
  result.pitch = angle(axes_prev_.pitch, now.pitch);
  result.local_yaw = angle(axes_prev_.local_yaw, now.local_yaw);
  result.roll = angle(axes_prev_.roll, now.roll);
  for (int i = 0; i < 3; ++i) {
    const float prev = axes_prev_.scale[i], cur = now.scale[i];
    const float lo = std::min(std::fabs(prev), std::fabs(cur)), hi = std::max(std::fabs(prev), std::fabs(cur));
    if (prev * cur > 0.0f && hi <= lo * kAxisSnapRatio) result.scale[i] = prev + (cur - prev) * alpha;
  }
  return result;
}

render::Mat4 Actor::model_matrix(float alpha) const {
  // Attached: the parent's joint at the same render fraction x the joint-space transform.
  if (attached_) {
    render::Mat4 joint_world;
    float joint_offset[3];
    if (attach_parent_->attach_joint(attach_point_, alpha, joint_world, joint_offset)) return joint_world * attach_local_;
  }
  const ActorPose actor_pose = pose(alpha);
  const Axes axes = axes_at(alpha);
  // A model with yaw (h - 0x400) faces heading h; movement is (-cos h, sin h). With this renderer's mat4_rotation_y the
  // model's forward is -Z, which gives the angle yaw + 0x800.
  const float angle = (actor_pose.yaw + static_cast<float>(kModelYawBias) * 2.0f) * kUnitsToRad;
  render::Mat4 rotation = render::mat4_rotation_y(angle);
  if (axes.pitch != 0.0f || axes.roll != 0.0f || (axes.local_yaw != 0.0f && !tilt_world)) {
    // The model matrix is RotMatrix({pitch, yaw, roll}) = pitch about X, yaw about Y, roll about Z
    // (a sword stuck in the ground is pitch 1100 = ~97 deg).
    const float pitch_rad = axes.pitch * kUnitsToRad, local_yaw_rad = axes.local_yaw * kUnitsToRad, roll_rad = axes.roll * kUnitsToRad;
    render::Mat4 rx, ry, rz;
    rx.at(1, 1) = std::cos(pitch_rad);
    rx.at(1, 2) = -std::sin(pitch_rad);
    rx.at(2, 1) = std::sin(pitch_rad);
    rx.at(2, 2) = std::cos(pitch_rad);
    ry.at(0, 0) = std::cos(local_yaw_rad);
    ry.at(0, 2) = std::sin(local_yaw_rad);
    ry.at(2, 0) = -std::sin(local_yaw_rad);
    ry.at(2, 2) = std::cos(local_yaw_rad);
    rz.at(0, 0) = std::cos(roll_rad);
    rz.at(0, 1) = -std::sin(roll_rad);
    rz.at(1, 0) = std::sin(roll_rad);
    rz.at(1, 1) = std::cos(roll_rad);
    // With tilt_world set (op AD 1: the world-map party) the matrix is one RotMatrix({pitch, yaw, roll}) = Rx*Ry*Rz, i.e. the
    // pitch acts in the WORLD frame after the yaw and tilts the model toward the camera whatever it faces; the model's own yaw is
    // not used. With it clear (the stuck sword, a swinging gate) the matrix is Ry(yaw) and RotMatrix({pitch, local yaw, roll}) is
    // the model's own rotation inside it (model frame: the sword stands up for any yaw).
    rotation = tilt_world ? rx * rotation * rz : rotation * rx * ry * rz;
  }
  // Ground-aligned basis path: matrix = tilt * RotY(yaw).
  if (gt_on_) {
    render::Mat4 tilt;
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 3; ++col) tilt.at(row, col) = gt_[row * 3 + col];
    rotation = tilt * rotation;
  }
  render::Mat4 scaling = render::mat4_scale(scale);
  for (int i = 0; i < 3; ++i) scaling.at(i, i) *= axes.scale[i];
  return render::mat4_translation(actor_pose.x, actor_pose.y + y_offset, actor_pose.z) * rotation * scaling;
}

void Actor::sample_local(const assets::AnimationClip& clip, float frame, std::vector<Local>& out) const {
  const auto& joints = model_->asset.skeleton.joints;
  out.resize(joints.size());
  for (size_t j = 0; j < joints.size(); ++j) {
    assets::Vec3 rot = joints[j].rest_rot_deg, trans = joints[j].rest_trans;
    if (j < clip.tracks.size()) {
      const auto& tr = clip.tracks[j];
      if (!tr.rot.empty()) rot = sample_keys(tr.rot, frame, true);
      if (!tr.trans.empty()) trans = sample_keys(tr.trans, frame, false);
    }
    out[j] = {rot, trans};
  }
}

void Actor::palette(float alpha, std::vector<render::Mat4>& out) const {
  if (!model_) {
    out.clear();
    return;
  }
  std::vector<Local> local;
  locals(alpha, local);
  compose(local, alpha, true, out);
}

void Actor::compose(const std::vector<Local>& local, float alpha, bool head, std::vector<render::Mat4>& out) const {
  const auto& joints = model_->asset.skeleton.joints;
  const int hj = head && head_.active ? head_joint() : -1;
  out.resize(joints.size());
  for (size_t j = 0; j < joints.size(); ++j) {
    render::Mat4 joint_local = render::mat4_from_rt(assets::rotation_zyx(local[j].rot), local[j].trans);
    // The head joint: local = RotMatrix({ax, 0, az}) x the REST rotation (taken from the skeleton record, not the clip).
    if (static_cast<int>(j) == hj) {
      joint_local = render::mat4_from_rt(assets::rotation_zyx(joints[j].rest_rot_deg), joints[j].rest_trans);
      float head_rot[9];
      head_matrix(head_.pax + wrap_units(head_.ax - head_.pax) * alpha, head_.paz + wrap_units(head_.az - head_.paz) * alpha, head_rot);
      render::Mat4 hr;
      for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) hr.at(row, col) = head_rot[row * 3 + col];
      const render::Mat4 rest = joint_local;
      joint_local = hr * joint_local;
      for (int row = 0; row < 3; ++row) joint_local.at(row, 3) = rest.at(row, 3);
    }
    const int parent = joints[j].parent;
    out[j] = parent >= 0 ? out[static_cast<size_t>(parent)] * joint_local : joint_local;
  }
}

void Actor::locals(float alpha, std::vector<Local>& local) const {
  if (clip_ < 0) {
    // No clips: rest pose.
    assets::AnimationClip rest;
    sample_local(rest, 0.0f, local);
  } else {
    auto at = [&](int clip, float tp, float tc) {
      float time = tp + (tc - tp) * alpha;
      if (time < 0) time += clip_length(clip);
      if (time >= clip_length(clip) && clip_length(clip) > 0) time -= clip_length(clip);
      return time;
    };
    sample_local(clip_at(clip_), at(clip_, t_prev_, t_cur_), local);
    auto weight = [&](int elapsed) {
      return fade_curve_ == 1 ? static_cast<float>(exp_blend_weight(elapsed)) / 4096.0f
                              : std::clamp(static_cast<float>(elapsed) / static_cast<float>(std::max(fade_len_, 1)), 0.0f, 1.0f);
    };
    const float w0 = weight(fade_prev_);
    const float blend_weight = w0 + (weight(fade_cur_) - w0) * alpha;
    if (from_clip_ >= 0 && blend_weight < 1.0f) {
      std::vector<Local> old;
      sample_local(clip_at(from_clip_), at(from_clip_, from_prev_, from_cur_), old);
      for (size_t j = 0; j < local.size(); ++j)
        for (size_t i = 0; i < 3; ++i) {
          float from = old[j].rot[i], to = local[j].rot[i];
          retail_arc(from, to);
          local[j].rot[i] = from + (to - from) * blend_weight;
          local[j].trans[i] = old[j].trans[i] + (local[j].trans[i] - old[j].trans[i]) * blend_weight;
        }
    }
  }
}

// ---- floor shadows -----------------------------------------------------------------------------------------------------------------

namespace {
// The shadow quad's model-space corners (+-0x200 in x / z, y 0) and its UVs (u 0 / 0x1E, v 0 / 0x1E of the
// 32x32 blob). Corner order 0 1 2 3.
constexpr float kShadowCorner[4][2] = {{512, 512}, {-512, 512}, {512, -512}, {-512, -512}};
constexpr float kShadowUv[4][2] = {{0, 0}, {30, 0}, {0, 30}, {30, 30}};
constexpr float kShadowTexSize = 32.0f;

// Normalises vec to a 4.12 unit vector (zero stays zero).
void normalise(float vec[3]) {
  const float length = std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
  if (length <= 0.0f) return;
  for (int i = 0; i < 3; ++i) vec[i] = vec[i] * 4096.0f / length;
}
}  // namespace

bool ActorShadows::init(const vfs::Vfs& files, render::GpuContext& ctx, render::SpriteBatch&) {
  // map/etc/systim.bin = cpt of system TIMs; TIM 0 is the shadow blob (4bpp, 8 x 32 halfwords, 16-colour CLUT whose every entry has the
  // STP bit: the whole quad is blended).
  auto raw = files.read("cdrom/map/etc/systim.bin");
  if (!raw) {
    log::warn("field: map/etc/systim.bin missing: no actor shadows");
    return false;
  }
  auto cpt = import::parse_cpt(ByteSpan(*raw));
  if (!cpt || cpt->entries.empty()) return false;
  auto tim = import::parse_tim(cpt->entries[0].data);
  if (!tim || tim->bpp != 4 || !tim->has_clut || tim->image.w != 8 || tim->image.h != 32) {
    log::warn("field: systim.bin TIM 0 is not the 32x32 4bpp shadow blob: no actor shadows");
    return false;
  }
  auto img = import::decode_tim(*tim, 0);
  if (!img) return false;
  const import::Image rgba = import::to_rgba(*img);
  if (!tex_.create(ctx, rgba.width, rgba.height, render::TextureFormat::Rgba8, rgba.pixels.data())) return false;
  if (!nearest_.create(ctx, false) || !linear_.create(ctx, true)) return false;
  set_nearest_ = render::TexRef(tex_, nearest_);
  set_linear_ = render::TexRef(tex_, linear_);
  return set_nearest_ && set_linear_;
}

void ActorShadows::shutdown(render::SpriteBatch&) {
  set_nearest_ = set_linear_ = {};
  tex_.destroy();
  nearest_.destroy();
  linear_.destroy();
}

void ActorShadows::floor_normal(const WalkMesh& walk, int tri, float out[3]) {
  out[0] = 0;
  out[1] = 4096;
  out[2] = 0;
  const assets::WalkMesh& mesh = walk.mesh();
  if (tri < 0 || static_cast<size_t>(tri) >= mesh.triangles.size()) return;
  const auto& triangle = mesh.triangles[static_cast<size_t>(tri)];
  if (triangle.v[0] >= mesh.vertices.size() || triangle.v[1] >= mesh.vertices.size() || triangle.v[2] >= mesh.vertices.size()) return;
  const auto& p0 = mesh.vertices[triangle.v[0]];
  const auto& p1 = mesh.vertices[triangle.v[1]];
  const auto& p2 = mesh.vertices[triangle.v[2]];
  float edge_a[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, edge_b[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
  normalise(edge_a);
  normalise(edge_b);
  // Outer product of the two unit edges: a 4.12 vector of length sin(angle), not renormalised.
  out[0] = (edge_a[1] * edge_b[2] - edge_a[2] * edge_b[1]) / 4096.0f;
  out[1] = (edge_a[2] * edge_b[0] - edge_a[0] * edge_b[2]) / 4096.0f;
  out[2] = (edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0]) / 4096.0f;
}

void ActorShadows::draw(render::SpriteBatch& sprites, const render::FieldCamera& camera, const render::Letterbox& lb, float scroll_x,
                        float scroll_y, std::span<const ShadowParams> shadows, bool linear, render::TexRef mask) const {
  const render::TexRef set = linear ? set_linear_ : set_nearest_;
  if (!set || shadows.empty()) return;
  // Subtractive blend, depth tested, never written.
  sprites.set_blend(render::BlendMode::Subtract);
  for (const ShadowParams& shadow : shadows) {
    if (shadow.grey <= 0) continue;
    // A = normalise(cross((0, 0, 1), N)) = norm(-N.y, N.x, 0); B = normalise(cross(A, N)) = norm(A x N). The matrix with
    // the rows A, N, B (then scaled by the actor scale) takes a corner v to the world offset (A.v, N.v, B.v).
    const float* normal = shadow.normal;
    float axis_a[3] = {-normal[1], normal[0], 0};
    normalise(axis_a);
    float axis_b[3] = {axis_a[1] * normal[2] - axis_a[2] * normal[1], axis_a[2] * normal[0] - axis_a[0] * normal[2],
                       axis_a[0] * normal[1] - axis_a[1] * normal[0]};
    normalise(axis_b);
    float xy[8], uv[8];
    float slot_z = 0;
    bool ok = true;
    for (int k = 0; k < 4 && ok; ++k) {
      const float vx = kShadowCorner[k][0] * shadow.scale_x / 4096.0f, vz = kShadowCorner[k][1] * shadow.scale_z / 4096.0f;
      const assets::Vec3 corner = {shadow.x + (axis_a[0] * vx + axis_a[2] * vz) / 4096.0f,
                                   shadow.y + (normal[0] * vx + normal[2] * vz) / 4096.0f,
                                   shadow.z + (axis_b[0] * vx + axis_b[2] * vz) / 4096.0f};
      render::FieldCamera::Projected pr;
      ok = camera.project(corner, pr);
      xy[k * 2] = lb.ox + (pr.sx + scroll_x) * lb.scale;
      xy[k * 2 + 1] = lb.oy + (pr.sy + scroll_y) * lb.scale_y;
      uv[k * 2] = kShadowUv[k][0] / kShadowTexSize;
      uv[k * 2 + 1] = kShadowUv[k][1] / kShadowTexSize;
      // The ordering slot is the depth of corner 2.
      if (k == 2) slot_z = pr.z;
    }
    if (!ok) continue;
    // Ordering index (depth >> 4) + actor layer; +0.5 puts it behind a background layer of the same slot (linked earlier, drawn later).
    const float slot = std::floor(std::max(slot_z, 0.0f) / 16.0f) + shadow.layer + 0.5f;
    // The actor's silhouette: its tile on the eased floor plane and in the blob's slot, so it meets the backdrop as the blob does. Tile x, z are world x, z.
    if (shadow.tile_half > 0.0f && mask) {
      for (int k = 0; k < 4 && ok; ++k) {
        const float side_x = (kShadowCorner[k][0] > 0.0f ? 1.0f : -1.0f) * shadow.tile_half, side_z = (kShadowCorner[k][1] > 0.0f ? 1.0f : -1.0f) * shadow.tile_half;
        render::FieldCamera::Projected pr;
        ok = camera.project({shadow.x + side_x, shadow.y + shadow.tile_slope_x * side_x + shadow.tile_slope_z * side_z, shadow.z + side_z}, pr);
        xy[k * 2] = lb.ox + (pr.sx + scroll_x) * lb.scale;
        xy[k * 2 + 1] = lb.oy + (pr.sy + scroll_y) * lb.scale_y;
        uv[k * 2] = shadow.tile_uv[0] + (side_x > 0.0f ? shadow.tile_uv[2] : -shadow.tile_uv[2]);
        uv[k * 2 + 1] = shadow.tile_uv[1] + (side_z > 0.0f ? -shadow.tile_uv[2] : shadow.tile_uv[2]);
      }
      if (!ok) continue;
      sprites.set_blend(render::BlendMode::Multiply);
      sprites.draw_quad_depth(mask, xy, uv, slot / render::kDepthRange);
      sprites.set_blend(render::BlendMode::Subtract);
      continue;
    }
    const u8 grey = static_cast<u8>(std::lround(std::clamp(static_cast<float>(shadow.grey) / 128.0f, 0.0f, 1.0f) * 255.0f));
    sprites.draw_quad_depth(set, xy, uv, slot / render::kDepthRange, render::pack_rgba(grey, grey, grey, 255));
  }
  sprites.set_blend(render::BlendMode::Alpha);
}

}  // namespace oc::game
