// 14bar (Inferno) body wobble: deforms the target's vertices while it is drawn; the ground part adds the stage heat wave
// (ground_ports_c.cpp). All maths is in PS1 model units: `psx()` converts the dummy-stage vertices back to them and `wu_` scales the
// result forward again. phi += 1 per tick; y += cos(z + 16 phi) * 100 >> 12; z += cos(y + 64 phi) * 200 >> 12; op 0x181 ends the
// wave. D comes from the context; the frame count drawn is D + 1 (the end test follows the draw).
#include "game/effects/bespoke/actor_script.h"

namespace oc::effects {
namespace {

using util::add;

class MeshWarp : public ActorScript {
 protected:
  int default_duration() const override { return 150; }
  bool takes_over(int side) const override { return side == 1; }

  bool command(int op, const int*, int) override {
    if (op != 0x181) return false;
    finish();
    return true;
  }

  void step() override {
    if (tick_ > D_) finish();
  }

  void draw_actor(int side, std::vector<FxPrim>& out) const override {
    const FxMesh& mesh = *ctx_.target.mesh;
    const ActorState& actor = a_[side];
    std::vector<Vec3> world(mesh.verts.size());
    for (size_t i = 0; i < world.size(); ++i) world[i] = add(actor.pos, warp(mesh.verts[i]));
    util::emit_mesh(ctx_.view, mesh, world, mesh.colours, 0, out, 5000);
  }

 private:
  float psx(float value) const { return value / wu_; }

  Vec3 warp(const Vec3& vertex) const {
    const int y = static_cast<int>(psx(vertex[1])), z = static_cast<int>(psx(vertex[2]));
    const int phi = tick_;
    const int y1 = y + ((rcos(z + 16 * phi) * 100) >> 12);
    const int z1 = z + ((rcos(y + 64 * phi) * 200) >> 12);
    return {vertex[0], fl(y1) * wu_, fl(z1) * wu_};
  }
};

}  // namespace

void register_mesh_warps(BespokeRegistry& registry) {
  registry.add({"14bar", "scripted-motion", "MeshWarp", [] { return std::make_unique<MeshWarp>(); }});
}

}  // namespace oc::effects
