// Scripted model motion: the ActorScript base (see actor_script.h) and the state-only overlay 3voice (it spawns a stock effect and
// draws nothing):
//   3voice  "CASTER_CHANGE": op 0x100 starts the type-0x10 sequence its operand names, bound to an acting-list entry.
#include "game/effects/bespoke/actor_script.h"

namespace oc::effects {

using util::add;
using util::mul;

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

// ---- 3voice: state only ---------------------------------------------------------------------------------------------------------------------------
class StateOnly : public ActorScript {
 protected:
  int default_duration() const override { return 40; }
  bool takes_over(int) const override { return false; }
  void begin() override { spawn("caster_change"); }
  bool command(int op, const int* args, int count) override {
    if (op != 0x100) return false;
    // The sequence tag is the word at args 2 / 3 (none = nothing starts), the acting-list entry args 4.
    if (count >= 5 && (args[2] != 0 || args[3] != 0) && ctx_.hooks.start_for_caster) {
      const char tag[4] = {static_cast<char>(args[2] & 0xFF), static_cast<char>((args[2] >> 8) & 0xFF), static_cast<char>(args[3] & 0xFF), static_cast<char>((args[3] >> 8) & 0xFF)};
      ctx_.hooks.start_for_caster(std::string_view(tag, 4), args[4]);
    }
    return true;
  }
  void step() override {
    if (tick_ >= D_) finish();
  }
};

}  // namespace

void register_actor_scripts(BespokeRegistry& registry) {
  registry.add({"3voice", "scripted-motion", "StateOnly", [] { return std::make_unique<StateOnly>(); }});
}

}  // namespace oc::effects
