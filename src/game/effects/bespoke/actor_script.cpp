// Scripted model motion: the overlays that only steer actor objects (see actor_script.h).
//   19helf  (19helf.prg)   P = S + (E - S) * e / D, scale 0x1000 -> 0 (shrink, op 0x100) or 0 -> 0x1000 (grow, 0x101)
//   golem                  scale V = s2 + (s3 - s2) * e / D, 0x1000 -> 0x200 (0x100) and 0x200 -> 0x1000 (0x101), no move
//   shine                  colour bytes c = c0 + (c1 - c0) * e / D, end colour = the element colour
//   opthedo                target snapped to the caster, moves S/16 units per tick along its facing, colour (e * 128) / D, then hidden
//   kamedes                colour c = 128 - 128 e / D (fade to black), then Y -= 4.0 per tick for D ticks, Y = 0 at the end
//   41grav  (Gravity Blow) states 0x180 (pull), 0x181 / 0x182 (flight, spin), 0x183 (fall)
//   43acce  (Freefall)     states 0x180 (hop), 0x181 (fall), 0x182 (accelerating fall), 0x183 (landing)
//   22torune (Tornado)     the frame task mirrors the sequence's position / rotation / scale words into the actor; on the landing
//                          condition it aims the actor (yaw = -1024 - atan2) and spawns "hit1" / "hit2".
//   kamei                  see kamei_port.cpp (morph draw, stage swap, form swap)
// State-only overlays (they set flags / spawn stock effects; nothing is drawn by the overlay): k_monk soidash 3voice bboy 27earth icerance
// enrou hebi shock - see StateOnly below.
#include "game/effects/bespoke/actor_script.h"

namespace oc::effects {

using util::add;
using util::lerp;
using util::mul;
using util::opnd;
using util::sub;

float mesh_height(const FxMesh* mesh) {
  if (!mesh || mesh->verts.empty()) return 0.0f;
  float lo = mesh->verts[0][1], hi = lo;
  for (const auto& vertex : mesh->verts) {
    lo = std::min(lo, vertex[1]);
    hi = std::max(hi, vertex[1]);
  }
  return hi - lo;
}

float world_unit(const FxContext& ctx) {
  // The PS1 figure is about 1070 units high.
  const float height = mesh_height(ctx.caster.mesh ? ctx.caster.mesh : ctx.target.mesh);
  return height > 1.0f ? std::max(0.05f, height / 1070.0f) : 0.25f;
}

// ---- ActorScript ----------------------------------------------------------------------------------------------------------------
void ActorScript::start(const FxContext& ctx) {
  ctx_ = ctx;
  rng_.state = ctx.seed ? ctx.seed : 1;
  D_ = ctx.duration > 0 ? std::min(ctx.duration, 4000) : default_duration();
  wu_ = world_unit(ctx);
  ground_ = ctx.target.pos[1];
  if (ctx.target.mesh)
    for (const auto& vertex : ctx.target.mesh->verts) ground_ = std::max(ground_, ctx.target.pos[1] + vertex[1]);
  tick_ = 0;
  done_ = false;
  for (int i = 0; i < 2; ++i) {
    const FxActor& actor = i == 0 ? ctx.caster : ctx.target;
    ActorState& state = a_[i];
    state = ActorState{};
    state.home = state.pos = actor.pos;
    state.present = actor.mesh != nullptr;
    if (takes_over(i) && state.present && ctx.hooks.set_visible) ctx.hooks.set_visible(i, false);
  }
  begin();
}

void ActorScript::push_poses() const {
  if (!ctx_.hooks.actor_pose) return;
  for (int i = 0; i < 2; ++i)
    if (takes_over(i) && a_[i].present) ctx_.hooks.actor_pose(i, a_[i].pos, turns_to_rad(a_[i].rot[1]));
}

void ActorScript::tick() {
  if (done_) return;
  ++tick_;
  step();
  if (!done_) push_poses();
}

void ActorScript::finish() {
  if (done_) return;
  done_ = true;
  for (int i = 0; i < 2; ++i) {
    if (!takes_over(i) || !a_[i].present) continue;
    if (ctx_.hooks.actor_pose) ctx_.hooks.actor_pose(i, a_[i].pos, turns_to_rad(a_[i].rot[1]));
    if (ctx_.hooks.set_visible) ctx_.hooks.set_visible(i, true);
  }
}

void ActorScript::draw(std::vector<FxPrim>& out) const {
  if (done_) return;
  for (int i = 0; i < 2; ++i) {
    const FxMesh* mesh = i == 0 ? ctx_.caster.mesh : ctx_.target.mesh;
    if (mesh && takes_over(i) && a_[i].present) draw_actor(i, out);
  }
  extra(out);
}

void ActorScript::draw_actor(int side, std::vector<FxPrim>& out) const {
  const FxMesh& mesh = *(side == 0 ? ctx_.caster.mesh : ctx_.target.mesh);
  const ActorState& actor = a_[side];
  const Mat3 rotation = Mat3::from_angles(static_cast<int>(std::lround(actor.rot[0])), static_cast<int>(std::lround(actor.rot[1])),
                                          static_cast<int>(std::lround(actor.rot[2])));
  std::vector<Vec3> world(mesh.verts.size());
  std::vector<std::array<u8, 3>> colours(mesh.verts.size());
  for (size_t k = 0; k < world.size(); ++k) {
    world[k] = add(actor.pos, rotation.apply(mul(mesh.verts[k], actor.scale)));
    for (size_t channel = 0; channel < 3; ++channel)
      colours[k][channel] = util::clamp8(fl(mesh.colours[k][channel]) * actor.tint[channel]);
  }
  util::emit_mesh(ctx_.view, mesh, world, colours, 0, out, static_cast<u32>(side) * 5000u);
}

namespace {

// ---- 19helf / golem: scale (and move) the target list over D ticks ---------------------------------------------------------------------
struct ScaleParams {
  const char* name;
  bool move;   // 19helf also moves the object to / from the end point E
  float low;   // scale of the shrunk state: 19helf 0, golem 0x200 / 0x1000
};

class ScaleMove : public ActorScript {
 public:
  explicit ScaleMove(const ScaleParams& params) : params_(params) {}

 protected:
  int default_duration() const override { return 30; }
  void begin() override {
    // E: the caster's position
    end_ = ctx_.caster.pos;
    from_ = a_[1].pos;
    phase_ = 0;
    elapsed_ = 0;
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x100 && op != 0x101) return false;
    D_ = clampi(opnd(args, count, 0, D_), 1, 600);
    phase_ = op == 0x100 ? 0 : 1;
    from_ = a_[1].pos;
    elapsed_ = 0;
    chained_ = false;
    idle_ = false;
    return true;
  }
  void step() override {
    if (idle_) return;
    // e runs 0..D inclusive: D + 1 updates per object, the last one snaps to the end value and frees the task
    const float progress = fl(std::min(elapsed_, D_)) / fl(D_);
    ActorState& actor = a_[1];
    if (phase_ == 0) {
      actor.scale = 1.0f + (params_.low - 1.0f) * progress;
      if (params_.move) actor.pos = lerp(from_, end_, progress);
    } else {
      actor.scale = params_.low + (1.0f - params_.low) * progress;
      if (params_.move) actor.pos = lerp(from_, actor.home, progress);
    }
    if (++elapsed_ > D_) {
      if (phase_ == 0 && chained_) {
        phase_ = 1;
        elapsed_ = 0;
        from_ = actor.pos;
      } else if (chained_) {
        finish();
      } else {
        // Driven by the sequence: hold the final state until the next op.
        idle_ = true;
      }
    }
  }

 private:
  ScaleParams params_;
  Vec3 end_{0, 0, 0}, from_{0, 0, 0};
  int phase_ = 0, elapsed_ = 0;
  bool chained_ = true, idle_ = false;
};

// ---- shine: colour ramp of the caster / target objects -------------------------------------------------------------------------------
class Shine : public ActorScript {
 protected:
  int default_duration() const override { return 40; }
  bool takes_over(int) const override { return true; }
  void begin() override {
    // End colour = the element colour.
    for (int i = 0; i < 3; ++i) end_[i] = fl(ctx_.colour[i]) / 128.0f;
  }
  bool command(int op, const int* args, int count) override {
    if (op != 0x100 && op != 0x101) return false;
    D_ = clampi(opnd(args, count, 0, D_), 1, 600);
    // 0x100 = list-2 record 0, 0x101 = record 1.
    side_ = op == 0x100 ? 0 : 1;
    if (count > 3)
      for (int i = 0; i < 3; ++i) end_[i] = fl(clampi(args[1 + i], 0, 255)) / 128.0f;
    elapsed_ = 0;
    solo_ = true;
    return true;
  }
  void step() override {
    // channel = start + (end - start) * e / D, e = 0..D, snapped to end at D + 1
    const float progress = fl(std::min(elapsed_, D_)) / fl(D_);
    for (int side = 0; side < 2; ++side) {
      if (solo_ && side != side_) continue;
      for (int channel = 0; channel < 3; ++channel) a_[side].tint[channel] = 1.0f + (end_[channel] - 1.0f) * progress;
    }
    if (++elapsed_ > D_ && !solo_) {
      for (int side = 0; side < 2; ++side)
        if (ctx_.hooks.actor_tint && a_[side].present)
          ctx_.hooks.actor_tint(side, static_cast<int>(a_[side].tint[0] * 128), static_cast<int>(a_[side].tint[1] * 128),
                                static_cast<int>(a_[side].tint[2] * 128));
      finish();
    }
  }

 private:
  float end_[3] = {1, 1, 1};
  int elapsed_ = 0, side_ = 0;
  bool solo_ = false;
};

// ---- opthedo: the target is cloned onto the caster, drifts along its facing and fades in -------------------------------------------------
class Opthedo : public ActorScript {
 protected:
  int default_duration() const override { return 45; }
  bool takes_over(int side) const override { return side == 1; }
  void begin() override {
    // Speed S from the first operand (default 160); the clone starts at the caster's position.
    speed_ = fl(ctx_.arg[0] != 0 ? ctx_.arg[0] : 160);
    a_[1].pos = ctx_.caster.pos;
    Vec3 delta = sub(ctx_.target.pos, ctx_.caster.pos);
    delta[1] = 0;
    const float length = util::len(delta);
    // Facing: the caster's yaw is not part of the context, so use caster -> target.
    dir_ = length > 1e-3f ? mul(delta, 1.0f / length) : Vec3{0, 0, 1};
    a_[1].rot[1] = fl(ratan2(static_cast<int>(dir_[0] * 4096), static_cast<int>(dir_[2] * 4096)));
    elapsed_ = 0;
  }
  void step() override {
    // v = (0, 0, S) rotated by the facing; X += v.x * 256 * dt, Z += v.z * 256 * dt in 16.12 => S / 16 units per tick
    const float step_len = speed_ / 16.0f * wu_;
    a_[1].pos = add(a_[1].pos, mul(dir_, step_len));
    ++elapsed_;
    // Expired: hide the clone.
    if (elapsed_ > D_) {
      a_[1].present = false;
      finish();
      return;
    }
    // Colour bytes (e * 128) / D = 0..0x80.
    const float shade = fl((elapsed_ * 128) / D_) / 128.0f;
    for (float& channel : a_[1].tint) channel = shade;
  }

 private:
  float speed_ = 160;
  Vec3 dir_{0, 0, 1};
  int elapsed_ = 0;
};

// ---- kamedes: fade to black, then slide up ---------------------------------------------------------------------------------------------
class Kamedes : public ActorScript {
 protected:
  int default_duration() const override { return 40; }
  void begin() override { elapsed_ = 0; phase_ = 1; }
  void step() override {
    ++elapsed_;
    if (phase_ == 1) {
      // Expiry hands over to the slide-up phase.
      if (elapsed_ > D_) {
        phase_ = 2;
        elapsed_ = 0;
        return;
      }
      // Colour c = (128 - (128 * e) / D) & 0xFF.
      const float shade = fl(128 - (128 * elapsed_) / D_) / 128.0f;
      for (float& channel : a_[1].tint) channel = shade;
    } else {
      // Expiry: back to the home height.
      if (elapsed_ > D_) {
        a_[1].pos[1] = a_[1].home[1];
        if (ctx_.hooks.actor_tint && a_[1].present) ctx_.hooks.actor_tint(1, 0, 0, 0);
        finish();
        return;
      }
      // Rises 4.0 units (0x4000 in 16.12) per tick.
      a_[1].pos[1] -= 4.0f * wu_;
    }
  }

 private:
  int elapsed_ = 0, phase_ = 1;
};

// ---- 41grav: Gravity Blow -------------------------------------------------------------------------------------------------------------
class Grav41 : public ActorScript {
 protected:
  int default_duration() const override { return 20; }
  void begin() override {
    saved_ = a_[1].pos;
    // B = the caster; T = a point above the caster / target pair.
    base_ = ctx_.caster.pos;
    tgt_ = add(mul(add(ctx_.caster.pos, ctx_.target.pos), 0.5f), Vec3{0, -500.0f * wu_, 0});
    enter(0x180, -200);
  }
  bool command(int op, const int* args, int count) override {
    if (op < 0x180 || op > 0x183) return false;
    auto_ = false;
    enter(op, op == 0x180 ? clampi(opnd(args, count, 0, -200), -4096, 4096) : clampi(opnd(args, count, 0, 24), 0, 2000));
    return true;
  }
  void step() override {
    ++state_ticks_;
    ActorState& actor = a_[1];
    switch (state_) {
      case 0x180:  // v =((A - B) * k) >> 12 (raw 16.12) => A += (A - B) * k / 4096 per tick
        for (size_t i = 0; i < 3; ++i) actor.pos[i] += (actor.pos[i] - base_[i]) * fl(param_) / 4096.0f;
        if (auto_ && state_ticks_ >= D_) enter(0x181, 24);
        break;
      case 0x181:  // flight toward T at speed k, spin (+10, +30, +40) per tick
      case 0x182: {  // second flight: spin (+50, +100, +140) per tick, Y floored at -10000
        Vec3 delta = sub(tgt_, actor.pos);
        const float length = util::len(delta);
        if (length > 1e-3f) actor.pos = add(actor.pos, mul(delta, std::min(fl(param_) * wu_, length) / length));
        const float sp[2][3] = {{10, 30, 40}, {50, 100, 140}};
        for (size_t i = 0; i < 3; ++i) actor.rot[i] += sp[state_ - 0x181][i];
        if (state_ == 0x182) actor.pos[1] = std::max(actor.pos[1], -10000.0f * wu_);
        if (auto_ && state_ticks_ >= D_) enter(state_ == 0x181 ? 0x182 : 0x183, param_);
        break;
      }
      case 0x183:  // A.y += s per tick, s += 30 from 400; lands at y > 0 (9 ticks)
        actor.pos[1] += fall_ * wu_;
        fall_ += 30;
        if (actor.pos[1] > 0) {
          actor.pos[1] = 0;
          // No handler: the landing effect is attached.
          state_ = 0x184;
          state_ticks_ = 0;
          spawn("impact");
        }
        break;
      default:  // 0x184: hold, then end
        if (!auto_ || ++hold_ > 12) finish();
        break;
    }
  }

 private:
  void enter(int state, int param) {
    state_ = state;
    state_ticks_ = 0;
    param_ = param;
    ActorState& actor = a_[1];
    // A.y = -4096, fall speed 400; x and z restored from the saved pose
    if (state == 0x183) {
      actor.pos = {saved_[0], -4096.0f * wu_, saved_[2]};
      actor.rot[0] = actor.rot[2] = 0;
      fall_ = 400;
    }
  }
  Vec3 saved_{0, 0, 0}, base_{0, 0, 0}, tgt_{0, 0, 0};
  int state_ = 0x180, state_ticks_ = 0, param_ = 0, hold_ = 0;
  float fall_ = 400;
  bool auto_ = true;
};

// ---- 43acce: Freefall ------------------------------------------------------------------------------------------------------------------
class Acce43 : public ActorScript {
 protected:
  int default_duration() const override { return 14; }
  void begin() override { enter(0x180, 48); }
  bool command(int op, const int* args, int count) override {
    if (op < 0x180 || op > 0x183) return false;
    auto_ = false;
    enter(op, clampi(opnd(args, count, 0, 48), 0, 4000));
    return true;
  }
  void step() override {
    ++state_ticks_;
    ActorState& actor = a_[1];
    switch (state_) {
      case 0x180:  // y += -V; V -= 5 per tick; y > 0 => y = 0, V = 0 (a hop that lands again)
        actor.pos[1] += -velocity_ * wu_;
        if (actor.pos[1] > 0) {
          actor.pos[1] = 0;
          velocity_ = 0;
          if (auto_ && ++hold_ > 3) enter(0x181, 520);
        } else {
          velocity_ -= 5;
        }
        break;
      case 0x181:  // constant fall: V = -k, y starts at -15247 (0xFC471000)
        actor.pos[1] += -velocity_ * wu_;
        if (auto_ && state_ticks_ >= D_) enter(0x182, 520);
        break;
      case 0x182:  // accelerating fall: V -= 10 per tick; lands at y > 0
        actor.pos[1] += -velocity_ * wu_;
        if (actor.pos[1] > 0) {
          actor.pos[1] = 0;
          velocity_ = 0;
          enter(0x183, 0);
        } else {
          velocity_ -= 10;
        }
        break;
      default:  // 0x183: landing cue, hold
        if (!auto_ || ++hold_ > 10) finish();
        break;
    }
  }
  void extra(std::vector<FxPrim>& out) const override {
    // The five emitter objects (kaz- a01- jim- rn1- rn0-) follow the actor at +/- V; shown as speed streaks above it.
    const float sp = std::abs(velocity_);
    if (sp < 1.0f || state_ == 0x183) return;
    const Vec3& pos = a_[1].pos;
    for (int i = -2; i <= 2; ++i) {
      Vec3 top = add(pos, Vec3{fl(i) * 22.0f * wu_ * 4.0f, -(60.0f + sp * 3.0f) * wu_, 0});
      Vec3 bot = add(pos, Vec3{fl(i) * 22.0f * wu_ * 4.0f, -40.0f * wu_, 0});
      FxPrim line;
      line.kind = FxPrim::Kind::Line;
      float z0, z1;
      if (!fx_project(ctx_.view, top, line.v[0].x, line.v[0].y, z0) || !fx_project(ctx_.view, bot, line.v[1].x, line.v[1].y, z1)) continue;
      line.v[0].r = 40; line.v[0].g = 60; line.v[0].b = 120;
      line.v[1].r = 255; line.v[1].g = 255; line.v[1].b = 255;
      line.blend = 2;
      line.slot = fx_slot_of_depth(z1);
      line.order = static_cast<u32>(10000 + i + 2);
      out.push_back(line);
    }
  }

 private:
  void enter(int state, int param) {
    state_ = state;
    state_ticks_ = 0;
    hold_ = 0;
    ActorState& actor = a_[1];
    // 0x180: V = k; 0x181: V = -k and A.y = -15247; 0x182: V = -k.
    if (state == 0x180) velocity_ = fl(param);
    if (state == 0x181) { velocity_ = -fl(param); actor.pos[1] = -15247.0f * wu_; }
    if (state == 0x182) velocity_ = -fl(param);
    if (state == 0x183) spawn("impact");
  }
  int state_ = 0x180, state_ticks_ = 0, hold_ = 0;
  float velocity_ = 0;
  bool auto_ = true;
};

// ---- 22torune: Tornado -----------------------------------------------------------------------------------------------------------------
class Torune : public ActorScript {
 protected:
  int default_duration() const override { return 36; }
  bool takes_over(int side) const override { return side == 0; }
  void begin() override {
    Vec3 delta = sub(ctx_.caster.pos, ctx_.target.pos);
    delta[1] = 0;
    const float length = util::len(delta);
    const Vec3 dir = length > 1e-3f ? mul(delta, 1.0f / length) : Vec3{-1, 0, 0};
    // Lands in front of the target.
    land_ = add(ctx_.target.pos, mul(dir, 220.0f * wu_));
    land_[1] = a_[0].home[1];
    height_ = 500.0f * wu_;
  }
  void step() override {
    ActorState& actor = a_[0];
    ++elapsed_;
    const int len = phase_ == 0 ? D_ : std::max(2, D_ / 2);
    const float progress = std::min(1.0f, fl(elapsed_) / fl(len));
    const float arc = 4.0f * progress * (1.0f - progress);
    if (phase_ == 0) {
      // Jump with two turns of spin.
      actor.pos = lerp(actor.home, land_, progress);
      actor.pos[1] = actor.home[1] - height_ * arc;
      actor.rot[1] = fl(static_cast<int>(8192.0f * progress));
    } else if (phase_ == 1) {
      // Bounce in place, a quarter of the height.
      actor.pos = land_;
      actor.pos[1] = land_[1] - height_ * 0.25f * arc;
    } else {
      // Hop back.
      actor.pos = lerp(land_, actor.home, progress);
      actor.pos[1] = actor.home[1] - height_ * 0.3f * arc;
    }
    if (elapsed_ >= len) {
      elapsed_ = 0;
      if (phase_ == 0) {
        // Landing branch: yaw = -1024 - atan2(S0.z - z, S0.x - x); x/z rot = 0; scale 1.0; spawn "hit1" and "hit2"
        const Vec3 delta = sub(ctx_.target.pos, land_);
        actor.rot[0] = actor.rot[2] = 0;
        actor.rot[1] = fl(-1024 - ratan2(static_cast<int>(delta[2]), static_cast<int>(delta[0])));
        actor.scale = 1.0f;
        spawn("hit1");
        spawn("hit2");
      }
      if (++phase_ > 2) finish();
    }
  }

 private:
  Vec3 land_{0, 0, 0};
  float height_ = 0;
  int phase_ = 0, elapsed_ = 0;
};

// ---- state-only overlays ---------------------------------------------------------------------------------------------------------------------
struct StateParams {
  const char* name;
  int duration;
  const char* spawn;  // stock effect the overlay starts, if any
  const char* what;   // what the engine does with the flags the overlay writes (documentation)
};

class StateOnly : public ActorScript {
 public:
  explicit StateOnly(const StateParams& params) : params_(params) {}

 protected:
  int default_duration() const override { return params_.duration; }
  bool takes_over(int) const override { return false; }
  void begin() override {
    if (params_.spawn && *params_.spawn) spawn(params_.spawn);
    if (std::string_view(params_.name) == "27earth") {
      // A half-turn flag on the actor turns the scene: positions rotated about the origin, x' = (z f1 + x f2) >> 12, z' = (z f2 - x f1) >> 12
      // (f1 = sin, f2 = cos of the angle). ctx.arg[0] != 0 stands for the flag.
      if (ctx_.arg[0] != 0) turn_ = 0x800;
    }
    if (turn_ != 0 && ctx_.hooks.actor_pose)
      for (int i = 0; i < 2; ++i) {
        const Vec3& pos = i == 0 ? ctx_.caster.pos : ctx_.target.pos;
        const float f1 = fl(rsin(turn_)) / 4096.0f, f2 = fl(rcos(turn_)) / 4096.0f;
        ctx_.hooks.actor_pose(i, {pos[2] * f1 + pos[0] * f2, pos[1], pos[2] * f2 - pos[0] * f1}, turns_to_rad(fl(turn_)));
      }
  }
  bool command(int op, const int* args, int count) override {
    bool acted = false;
    // 3voice: the sequence tag is the word at args 2 / 3 (none = nothing starts), the acting-list entry args 4.
    if (std::string_view(params_.name) == "3voice" && op == 0x100) {
      acted = true;
      if (count >= 5 && (args[2] != 0 || args[3] != 0) && ctx_.hooks.start_for_caster) {
        const char tag[4] = {static_cast<char>(args[2] & 0xFF), static_cast<char>((args[2] >> 8) & 0xFF), static_cast<char>(args[3] & 0xFF), static_cast<char>((args[3] >> 8) & 0xFF)};
        ctx_.hooks.start_for_caster(std::string_view(tag, 4), args[4]);
      }
    }
    // shock: 0x100 marks the "kill generator" tasks; 0x101 hides every actor except the caster (the target).
    if (std::string_view(params_.name) == "shock" && op == 0x101) {
      acted = true;
      if (ctx_.hooks.set_visible) {
        ctx_.hooks.set_visible(1, false);
        hidden_ = true;
      }
    }
    return acted;
  }
  void step() override {
    if (tick_ >= D_) {
      if (hidden_ && ctx_.hooks.set_visible) ctx_.hooks.set_visible(1, true);
      finish();
    }
  }

 private:
  StateParams params_;
  int turn_ = 0;
  bool hidden_ = false;
};

constexpr StateParams kStateOnly[] = {
    // k_monk: sets per-part visibility bits of the monster model (0x102 sets all 8 part bits).
    {"k_monk", 40, "", "per-part visibility bits of the monster model"},
    // soidash: 0x100 clears / 0x101 sets bit 0x200 of the object flags on every object (show / hide).
    {"soidash", 40, "", "show / hide flag 0x200 on every actor"},
    // 3voice "CASTER_CHANGE": 0x100 starts the type-0x10 sequence its operand names, bound to an acting-list entry.
    {"3voice", 40, "caster_change", "spawns a linked child and sets the caster's change flag"},
    // bboy: 0x100 sets scale 0xC00 and the position of each record; a callback moves the "bway" child.
    {"bboy", 60, "bway", "scale 0xC00 + positions; drives the 'bway' child object"},
    // 27earth (Earthquake): rotates the sub-records of every type-10 object by 0x800 when the caster's flag is set.
    {"27earth", 60, "tget", "turns the scene by half a turn when the caster's flag is set; the quake itself is the prd's emitters"},
    // icerance: moves the object tagged "1" by p[i] -= (v[i] << S) per tick (see IceLance below).
    // enrou: spawns the "thre" (type 0x25) child; 0x100 flags the emitter tasks to stop.
    {"enrou", 60, "thre", "ZETUEN KILL GENERATER: spawns 'thre' and flags emitter tasks to stop"},
    // hebi: creates the generator child and frees it once it reports done.
    {"hebi", 60, "zetuen_generater", "ZETUEN KILL GENERATER: creates a child generator, frees it when it reports done"},
    // shock: 0x100 flags the kill tasks, 0x101 hides every non-caster actor.
    {"shock", 40, "", "flags the emitter kill tasks; 0x101 hides non-caster actors"},
};

// ---- icerance: the lance object is moved by p[i] -= (v[i] << S) * dt; drawn here as an ice spike flying caster -> target ----------------
class IceLance : public ActorScript {
 protected:
  int default_duration() const override { return 36; }
  bool takes_over(int) const override { return false; }
  void begin() override {
    pos_ = add(ctx_.caster.pos, Vec3{0, -150.0f * wu_, 0});
    const Vec3 goal = add(ctx_.target.pos, Vec3{0, -100.0f * wu_, 0});
    // Stored negated: p -= v.
    velocity_ = mul(sub(goal, pos_), -1.0f / fl(D_));
    start_ = pos_;
  }
  void step() override {
    pos_ = sub(pos_, velocity_);
    if (tick_ >= D_) finish();
  }
  void extra(std::vector<FxPrim>& out) const override {
    // Visual proxy: a four-sided spike along the flight direction (the mesh of the lance is object "1" of the prd).
    Vec3 dir = mul(velocity_, -1.0f);
    const float dir_length = util::len(dir);
    if (dir_length < 1e-3f) return;
    dir = mul(dir, 1.0f / dir_length);
    const float len = 150.0f * wu_, wd = 28.0f * wu_;
    const Vec3 tip = add(pos_, mul(dir, len)), tail = sub(pos_, mul(dir, len * 0.3f));
    const Vec3 side[4] = {{wd, 0, 0}, {0, wd, 0}, {-wd, 0, 0}, {0, -wd, 0}};
    for (size_t k = 0; k < 4; ++k) {
      FxPrim tri;
      tri.kind = FxPrim::Kind::Tri;
      const Vec3 pts[3] = {tip, add(tail, side[k]), add(tail, side[(k + 1) % 4])};
      float zs = 0;
      bool ok = true;
      for (size_t i = 0; i < 3 && ok; ++i) {
        float z;
        ok = fx_project(ctx_.view, pts[i], tri.v[i].x, tri.v[i].y, z);
        zs += z;
        tri.v[i].r = i == 0 ? 235 : 90;
        tri.v[i].g = i == 0 ? 250 : 170;
        tri.v[i].b = 255;
      }
      if (!ok) continue;
      tri.blend = 0;
      tri.slot = fx_slot_of_depth(zs / 3.0f);
      tri.order = static_cast<u32>(k);
      out.push_back(tri);
    }
  }

 private:
  Vec3 pos_{0, 0, 0}, velocity_{0, 0, 0}, start_{0, 0, 0};
};

}  // namespace

void register_actor_scripts(BespokeRegistry& registry) {
  static constexpr ScaleParams kScale[] = {{"19helf", true, 0.0f}, {"golem", false, 512.0f / 4096.0f}};
  for (const ScaleParams& params : kScale)
    registry.add({params.name, "scripted-motion", "ScaleMove", [params] { return std::make_unique<ScaleMove>(params); }});
  registry.add({"shine", "scripted-motion", "Shine", [] { return std::make_unique<Shine>(); }});
  registry.add({"opthedo", "scripted-motion", "Opthedo", [] { return std::make_unique<Opthedo>(); }});
  registry.add({"kamedes", "scripted-motion", "Kamedes", [] { return std::make_unique<Kamedes>(); }});
  registry.add({"41grav", "scripted-motion", "Grav41", [] { return std::make_unique<Grav41>(); }});
  registry.add({"43acce", "scripted-motion", "Acce43", [] { return std::make_unique<Acce43>(); }});
  registry.add({"22torune", "scripted-motion", "Torune", [] { return std::make_unique<Torune>(); }});
  registry.add({"icerance", "scripted-motion", "IceLance", [] { return std::make_unique<IceLance>(); }});
  for (const StateParams& params : kStateOnly)
    registry.add({params.name, "scripted-motion", "StateOnly", [params] { return std::make_unique<StateOnly>(params); }});
}

}  // namespace oc::effects
