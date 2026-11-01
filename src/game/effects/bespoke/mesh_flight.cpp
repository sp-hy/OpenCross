// Piece-flight stand-ins: the target mesh's faces are grouped into pieces that are released one after another and fly off.
//   FaceFlight   saraman (pieces inward with spin), wipeoff (8 columns released left to right, a burst of sparks when a piece dies),
//                drg_deth (faces reached by a rising attractor shatter upward, spinning about z), fnoise (an expanding ring releases the
//                faces, which fall with drag and fade), theend (pieces sucked toward the caster, release staggered along x), lasbos
//                (pieces shrink along a rising spiral). A piece is a group of faces; its release tick comes from the trigger, its motion
//                from the motion kind, both parameterised per overlay in kFlights. Ops 0x181 / 0x183 add an operand gravity (x 0.01).
//   GhostCopies  tumaru: every 4 ticks a fading, drifting, slowly spinning semi-transparent copy of the model (up to 6 alive, 10 ticks each).
//   DustSweep    tekken: 8 smoke columns from the caster to the target, column i puffing at tick 4 i + 2.
//   CellReveal   rakugaki: the target's screen rectangle as 16 x 16 cells appearing in a shuffled order (1024 random swaps), then a
//                top-down sweep erases them.
// Timings and strengths without an operand are chosen to match the described structure; the operand-driven ones arrive through command().
#include <algorithm>
#include <cmath>

#include "game/effects/bespoke/mesh_base.h"

namespace oc::effects {
namespace {

using util::add;
using util::len;
using util::mul;
using util::opnd;
using util::sub;

enum class Trigger { Time, Column, Attractor, Ring, Suck };
enum class Motion { Ballistic, Rise, Fall, Suck, Orbit };

// gx / gy / gz = piece grid (all 0 = one piece per face); delay = base release tick; per_unit = ticks per unit of distance (Time / Ring),
// per column (Column) or per fraction of the x extent (Suck); speed = horizontal velocity per unit of offset from the axis (negative =
// inward); spin in radians per tick about spin_axis (0 x, 1 y, 2 z, -1 random per piece); life = ticks after the release before the piece
// is gone (0 = fades by itself / stays); fade = additive with a fading colour; burst = sparks when a piece dies.
struct FlightParams {
  const char* name;
  Trigger trig;
  Motion mot;
  int gx, gy, gz;
  float delay;
  float per_unit;
  float speed;
  float gravity;
  float spin;
  int spin_axis;
  int life;
  bool fade;
  bool burst;
};

Vec3 rot_axis(const Vec3& vec, int axis, float angle) {
  const float cos_a = std::cos(angle), sin_a = std::sin(angle);
  switch (axis) {
    case 0: return {vec[0], vec[1] * cos_a - vec[2] * sin_a, vec[1] * sin_a + vec[2] * cos_a};
    case 1: return {vec[0] * cos_a + vec[2] * sin_a, vec[1], -vec[0] * sin_a + vec[2] * cos_a};
    default: return {vec[0] * cos_a - vec[1] * sin_a, vec[0] * sin_a + vec[1] * cos_a, vec[2]};
  }
}

class FaceFlight : public MeshFx {
 public:
  explicit FaceFlight(const FlightParams& params) : params_(params) {}

  bool command(int op, const int* args, int nargs) override {
    if (op != 0x181 && op != 0x183) return false;
    gravity_ = static_cast<float>(opnd(args, nargs, 0, 0)) * 0.01f + params_.gravity;
    return true;
  }
  bool done() const override { return tick_ > duration_; }

 protected:
  int default_duration() const override { return 150; }
  // Copies the mesh with one vertex set per face (so the pieces can part), groups the faces into pieces and gives every piece its release
  // tick, spin axis and spin rate.
  void begin() override {
    if (!mesh_) return;
    own_.verts.clear();
    own_.colours.clear();
    own_.tris.clear();
    for (const auto& tri : mesh_->tris) {
      const u16 base = static_cast<u16>(own_.verts.size());
      for (int k = 0; k < 3; ++k) {
        own_.verts.push_back(mesh_->verts[tri[static_cast<size_t>(k)]]);
        own_.colours.push_back(mesh_->colours[tri[static_cast<size_t>(k)]]);
      }
      own_.tris.push_back({base, static_cast<u16>(base + 1), static_cast<u16>(base + 2)});
    }
    own_.finish();
    mesh_ = &own_;
    pos_ = own_.verts;
    col_ = own_.colours;
    world_.assign(pos_.size(), {0, 0, 0});
    Vec3 lo = own_.verts[0], hi = own_.verts[0];
    for (const auto& vertex : own_.verts)
      for (size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], vertex[k]);
        hi[k] = std::max(hi[k], vertex[k]);
      }
    lo_ = lo;
    hi_ = hi;
    gravity_ = params_.gravity;
    const bool per_face = params_.gx == 0 && params_.gy == 0 && params_.gz == 0;
    const size_t face_count = own_.tris.size();
    face_group_.resize(face_count);
    std::vector<Vec3> centroid_sum;
    std::vector<int> member_count;
    std::vector<int> column_of;
    for (size_t face = 0; face < face_count; ++face) {
      Vec3 centre = centroid(face);
      int group_index;
      int column = 0;
      if (per_face) group_index = static_cast<int>(face);
      else {
        auto cell = [&](size_t axis, int cells) {
          if (cells <= 1) return 0;
          return std::clamp(static_cast<int>((centre[axis] - lo[axis]) / std::max(1e-3f, hi[axis] - lo[axis]) * static_cast<float>(cells)), 0, cells - 1);
        };
        const int ix = cell(0, std::max(1, params_.gx)), iy = cell(1, std::max(1, params_.gy)), iz = cell(2, std::max(1, params_.gz));
        group_index = ix + std::max(1, params_.gx) * (iy + std::max(1, params_.gy) * iz);
        column = ix;
      }
      if (static_cast<size_t>(group_index) >= groups_.size()) {
        groups_.resize(static_cast<size_t>(group_index) + 1);
        centroid_sum.resize(groups_.size());
        member_count.resize(groups_.size());
        column_of.resize(groups_.size());
      }
      centroid_sum[static_cast<size_t>(group_index)] = add(centroid_sum[static_cast<size_t>(group_index)], centre);
      ++member_count[static_cast<size_t>(group_index)];
      column_of[static_cast<size_t>(group_index)] = column;
      face_group_[face] = group_index;
    }
    const Vec3 centre = own_.centre;
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
      Group& group = groups_[gi];
      if (!member_count[gi]) {
        group.release = 1e9f;
        group.skip = true;
        continue;
      }
      group.c0 = mul(centroid_sum[gi], 1.0f / static_cast<float>(member_count[gi]));
      const float dist = len(sub(group.c0, centre));
      switch (params_.trig) {
        case Trigger::Time: group.release = params_.delay + params_.per_unit * dist; break;
        case Trigger::Column: group.release = params_.delay + params_.per_unit * static_cast<float>(column_of[gi]); break;
        case Trigger::Attractor: group.release = 1e9f; break;
        case Trigger::Ring: group.release = params_.delay + dist / 17.6f; break;
        case Trigger::Suck: group.release = params_.delay + params_.per_unit * (hi[0] - group.c0[0]) / std::max(1e-3f, hi[0] - lo[0]); break;
      }
      group.axis = params_.spin_axis >= 0 ? params_.spin_axis : static_cast<int>(rng_.next() % 3);
      group.spin = params_.spin * (0.6f + static_cast<float>(rng_.next() % 80) * 0.01f);
      group.pos = group.c0;
    }
    step();
  }

  void step() override {
    if (!mesh_) return;
    const Vec3 centre = own_.centre;
    const float now = static_cast<float>(tick_);
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
      Group& group = groups_[gi];
      if (!group.active) {
        bool go = now >= group.release;
        if (params_.trig == Trigger::Attractor) {
          const float frac = std::min(1.0f, now / static_cast<float>(duration_) * 1.6f);
          const Vec3 attractor{centre[0], lo_[1] + (hi_[1] - lo_[1]) * frac, centre[2]};
          go = len(sub(group.c0, attractor)) < 0.35f * own_.radius;
        }
        if (go && !group.skip) {
          group.active = true;
          group.age = 0;
          init_velocity(group, centre);
        }
      } else if (!group.dead) {
        ++group.age;
        group.vel[1] += gravity_;
        if (params_.mot == Motion::Fall) group.vel = mul(group.vel, 0.985f);
        if (params_.mot == Motion::Orbit) {
          const float orbit_ticks = 60.0f, progress = std::min(1.0f, static_cast<float>(group.age) / orbit_ticks);
          const Vec3 off = sub(group.c0, centre);
          const float radius = std::sqrt(off[0] * off[0] + off[2] * off[2]) * (1.0f - progress);
          const float angle = std::atan2(off[2], off[0]) + 0.12f * static_cast<float>(group.age);
          group.pos = {centre[0] + radius * std::cos(angle), group.c0[1] - 0.9f * static_cast<float>(group.age) - 0.01f * group.age * group.age,
                       centre[2] + radius * std::sin(angle)};
        } else {
          group.pos = add(group.pos, group.vel);
        }
        if (params_.life > 0 && group.age > params_.life) {
          group.dead = true;
          if (params_.burst) spawn_burst(add(base_, group.pos));
        }
        if (params_.fade && params_.life == 0 && group.age > 50) group.dead = true;
      }
    }
    for (size_t face = 0; face < own_.tris.size(); ++face) {
      const Group& group = groups_[static_cast<size_t>(face_group_[face])];
      for (size_t k = 0; k < 3; ++k) {
        const size_t vi = own_.tris[face][k];
        const Vec3& vertex = own_.verts[vi];
        if (!group.active) {
          pos_[vi] = vertex;
          col_[vi] = own_.colours[vi];
          continue;
        }
        if (group.dead) {
          pos_[vi] = {0, 1e6f, 0};
          continue;
        }
        const Vec3 rel = sub(vertex, group.c0);
        pos_[vi] = add(group.pos, rot_axis(rel, group.axis, group.spin * static_cast<float>(group.age)));
        float fade = 1.0f;
        if (params_.fade)
          fade = params_.life > 0 ? 1.0f - static_cast<float>(group.age) / static_cast<float>(params_.life) : 1.0f - static_cast<float>(group.age) / 50.0f;
        col_[vi] = {util::clamp8(static_cast<float>(own_.colours[vi][0]) * fade), util::clamp8(static_cast<float>(own_.colours[vi][1]) * fade),
                    util::clamp8(static_cast<float>(own_.colours[vi][2]) * fade)};
      }
    }
    blend_ = params_.fade ? 2 : 0;
    finish_step();
    for (auto& spark : sparks_) {
      spark.pos = add(spark.pos, spark.vel);
      ++spark.age;
    }
    sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [](const Spark& spark) { return spark.age > 20; }), sparks_.end());
  }

  void extra(std::vector<FxPrim>& out) const override {
    for (const auto& spark : sparks_)
      util::emit_point(ctx_.view, spark.pos, 3.0f * (1.0f - static_cast<float>(spark.age) / 20.0f) + 0.5f, ctx_.colour[0], ctx_.colour[1], ctx_.colour[2], 2,
                       out);
  }

 private:
  struct Group {
    Vec3 c0{0, 0, 0}, pos{0, 0, 0}, vel{0, 0, 0};
    float release = 1e9f, spin = 0;
    int axis = 1, age = 0;
    bool active = false, dead = false, skip = false;
  };
  struct Spark {
    Vec3 pos, vel;
    int age = 0;
  };

  Vec3 centroid(size_t face) const {
    Vec3 sum{0, 0, 0};
    for (size_t k = 0; k < 3; ++k) sum = add(sum, own_.verts[own_.tris[face][k]]);
    return mul(sum, 1.0f / 3.0f);
  }
  void init_velocity(Group& group, const Vec3& centre) {
    const Vec3 off = sub(group.c0, centre);
    switch (params_.mot) {
      case Motion::Ballistic:
        group.vel = {off[0] * params_.speed, -0.4f - static_cast<float>(rng_.next() % 10) * 0.05f, off[2] * params_.speed};
        break;
      case Motion::Rise: group.vel = {0, -(5.0f + static_cast<float>(rng_.next() % 5)) * 0.35f, 0}; break;
      case Motion::Fall: group.vel = {off[0] * 0.02f, 0, off[2] * 0.02f}; break;
      case Motion::Suck: {
        const Vec3 goal = sub(add(ctx_.caster.pos, centre), base_);
        group.vel = mul(sub(goal, group.c0), 1.0f / 40.0f);
        break;
      }
      case Motion::Orbit: group.vel = {0, 0, 0}; break;
    }
  }
  void spawn_burst(const Vec3& world) {
    for (int i = 0; i < 5 && sparks_.size() < 64; ++i)
      sparks_.push_back({world, {static_cast<float>(static_cast<int>(rng_.next() % 24) - 12) * 0.25f, -static_cast<float>(rng_.next() % 15) * 0.2f,
                                 static_cast<float>(static_cast<int>(rng_.next() % 30) - 15) * 0.25f}, 0});
  }

  FlightParams params_;
  FxMesh own_;
  std::vector<Group> groups_;
  std::vector<int> face_group_;
  std::vector<Spark> sparks_;
  Vec3 lo_{0, 0, 0}, hi_{0, 0, 0};
  float gravity_ = 0;
};

// tumaru: the model stays; every 4 ticks a ghost copy is added that drifts, spins about y with an accelerating angle and fades from
// 0x90 by 14 per tick of age, drawn half-transparent at 1.7 x the mesh colour.
class GhostCopies : public MeshFx {
 public:
  bool done() const override { return tick_ > duration_ + 12; }

 protected:
  int default_duration() const override { return 90; }
  bool hides_actor() const override { return false; }
  void begin() override { step(); }
  void step() override {
    if (!mesh_) return;
    pos_ = mesh_->verts;
    col_ = mesh_->colours;
    finish_step();
    if (tick_ <= duration_ && tick_ % 4 == 0 && ghosts_.size() < 6) {
      Ghost ghost;
      ghost.drift = {static_cast<float>(static_cast<int>(rng_.next() % 9) - 4) * 1.2f, -static_cast<float>(rng_.next() % 5) * 0.8f,
                     static_cast<float>(static_cast<int>(rng_.next() % 9) - 4) * 1.2f};
      ghost.rot = static_cast<float>(static_cast<int>(rng_.next() % 3) - 1) * 3.0f * 0.0015f * 4.0f;
      ghosts_.push_back(ghost);
    }
    for (auto& ghost : ghosts_) ++ghost.age;
    ghosts_.erase(std::remove_if(ghosts_.begin(), ghosts_.end(), [](const Ghost& ghost) { return ghost.age > 10; }), ghosts_.end());
  }
  void extra(std::vector<FxPrim>& out) const override {
    if (!mesh_) return;
    std::vector<Vec3> world(mesh_->verts.size());
    std::vector<std::array<u8, 3>> colours(mesh_->verts.size());
    u32 order = 1000;
    for (const auto& ghost : ghosts_) {
      const float angle = ghost.rot * static_cast<float>(ghost.age) * static_cast<float>(ghost.age);
      const float level = std::max(0.0f, (144.0f - 14.0f * static_cast<float>(ghost.age)) / 255.0f);
      for (size_t i = 0; i < world.size(); ++i) {
        const Vec3 rotated = rot_axis(sub(mesh_->verts[i], mesh_->centre), 1, angle);
        world[i] = add(base_, add(add(mesh_->centre, rotated), mul(ghost.drift, static_cast<float>(ghost.age))));
        colours[i] = {util::clamp8(static_cast<float>(mesh_->colours[i][0]) * level * 1.7f), util::clamp8(static_cast<float>(mesh_->colours[i][1]) * level * 1.7f),
                      util::clamp8(static_cast<float>(mesh_->colours[i][2]) * level * 1.7f)};
      }
      util::emit_mesh(ctx_.view, *mesh_, world, colours, 1, out, order);
      order += 1000;
    }
  }

 private:
  struct Ghost {
    Vec3 drift{0, 0, 0};
    float rot = 0;
    int age = 0;
  };
  std::vector<Ghost> ghosts_;
};

// tekken: 8 columns between the caster and the target, each puffing two grey smoke points at tick 4 i + 2 that drift up and fade over
// 24 ticks.
class DustSweep : public BespokeEffect {
 public:
  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    rng_.state = ctx.seed ? ctx.seed : 1;
    duration_ = ctx.duration > 0 ? ctx.duration : 70;
  }
  bool command(int op, const int* args, int nargs) override {
    if (op != 0x100) return false;
    duration_ = std::max(8, opnd(args, nargs, 0, duration_));
    return true;
  }
  void tick() override {
    ++tick_;
    const Vec3 start = ctx_.caster.pos, end = ctx_.target.pos;
    for (int i = 0; i < 8; ++i) {
      if (tick_ == (i << 2) + 2)
        for (int k = 0; k < 2; ++k) {
          const float frac = static_cast<float>(i) / 7.0f;
          Puff puff;
          puff.pos = add(add(start, mul(sub(end, start), frac)), {0, 60.0f, static_cast<float>(static_cast<int>(rng_.next() % 20) - 10)});
          puff.vel = {static_cast<float>(static_cast<int>(rng_.next() % 9) - 4) * 0.3f, -0.8f - static_cast<float>(rng_.next() % 8) * 0.1f,
                      static_cast<float>(static_cast<int>(rng_.next() % 9) - 4) * 0.3f};
          puffs_.push_back(puff);
        }
    }
    for (auto& puff : puffs_) {
      puff.pos = add(puff.pos, puff.vel);
      ++puff.age;
    }
    puffs_.erase(std::remove_if(puffs_.begin(), puffs_.end(), [](const Puff& puff) { return puff.age > 24; }), puffs_.end());
  }
  void draw(std::vector<FxPrim>& out) const override {
    for (const auto& puff : puffs_) {
      const float fade = 1.0f - static_cast<float>(puff.age) / 24.0f;
      const u8 grey = util::clamp8(150.0f * fade);
      util::emit_point(ctx_.view, puff.pos, 6.0f + static_cast<float>(puff.age) * 0.5f, grey, grey, grey, 1, out);
    }
  }
  bool done() const override { return tick_ > duration_ && puffs_.empty(); }

 private:
  struct Puff {
    Vec3 pos, vel;
    int age = 0;
  };
  Rng rng_;
  int tick_ = 0, duration_ = 70;
  std::vector<Puff> puffs_;
};

// rakugaki: the target's projected bounding rectangle split into 16 x 16 cells, shown in a shuffled order over the duration in the
// mesh's mean colour (jittered per cell), then erased top-down over half the duration.
class CellReveal : public BespokeEffect {
 public:
  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    rng_.state = ctx.seed ? ctx.seed : 1;
    duration_ = ctx.duration > 0 ? ctx.duration : 60;
    order_cells_.resize(256);
    for (size_t i = 0; i < 256; ++i) order_cells_[i] = static_cast<u8>(i);
    for (int i = 0; i < 1024; ++i) std::swap(order_cells_[rng_.next() % 256], order_cells_[rng_.next() % 256]);
    x0_ = 110;
    y0_ = 40;
    x1_ = 210;
    y1_ = 200;
    if (ctx.target.mesh) {
      float lx = 1e9f, ly = 1e9f, hx = -1e9f, hy = -1e9f;
      for (const auto& vertex : ctx.target.mesh->verts) {
        float sx, sy, z;
        if (!fx_project(ctx.view, add(ctx.target.pos, vertex), sx, sy, z)) continue;
        lx = std::min(lx, sx);
        hx = std::max(hx, sx);
        ly = std::min(ly, sy);
        hy = std::max(hy, sy);
      }
      if (hx > lx && hy > ly) {
        x0_ = lx;
        x1_ = hx;
        y0_ = ly;
        y1_ = hy;
      }
      avg_ = {128, 128, 128};
      float sum[3] = {0, 0, 0};
      for (const auto& colour : ctx.target.mesh->colours)
        for (size_t k = 0; k < 3; ++k) sum[k] += colour[k];
      const float count = static_cast<float>(std::max<size_t>(1, ctx.target.mesh->colours.size()));
      for (size_t k = 0; k < 3; ++k) avg_[k] = util::clamp8(sum[k] / count);
    }
  }
  void tick() override { ++tick_; }
  void draw(std::vector<FxPrim>& out) const override {
    const int shown = std::min(256, tick_ * 256 / std::max(1, duration_));
    const float erase = tick_ > duration_ ? static_cast<float>(tick_ - duration_) / static_cast<float>(std::max(1, duration_ / 2)) : 0.0f;
    const float cw = (x1_ - x0_) / 16.0f, ch = (y1_ - y0_) / 16.0f;
    for (int i = 0; i < shown; ++i) {
      const int cell = order_cells_[static_cast<size_t>(i)];
      const int cx = cell & 15, cy = cell >> 4;
      if (static_cast<float>(cy) / 16.0f < erase) continue;
      const int jitter = (cell * 37) % 40 - 20;
      out.push_back(util::rect_prim(x0_ + static_cast<float>(cx) * cw, y0_ + static_cast<float>(cy) * ch, x0_ + static_cast<float>(cx + 1) * cw,
                                    y0_ + static_cast<float>(cy + 1) * ch, util::clamp8i(avg_[0] + jitter), util::clamp8i(avg_[1] + jitter),
                                    util::clamp8i(avg_[2] + jitter), 0, 8));
    }
  }
  bool done() const override { return tick_ > duration_ + duration_ / 2 + 1; }

 private:
  Rng rng_;
  int tick_ = 0, duration_ = 60;
  std::vector<u8> order_cells_;
  float x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;
  std::array<u8, 3> avg_{128, 128, 128};
};

//                          name        trigger           motion            gx gy gz delay  per_unit speed   gravity spin  axis life fade burst
constexpr FlightParams kFlights[] = {
    {"saraman", Trigger::Time, Motion::Ballistic, 3, 3, 2, 20.0f, 0.0625f, -0.05f, 0.30f, 0.12f, -1, 70, true, false},
    {"wipeoff", Trigger::Column, Motion::Ballistic, 8, 1, 1, 10.0f, 4.0f, 0.0f, 0.50f, 0.0f, 1, 45, false, true},
    {"drg_deth", Trigger::Attractor, Motion::Rise, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.10f, 2, 60, true, false},
    {"fnoise", Trigger::Ring, Motion::Fall, 0, 0, 0, 10.0f, 0.0f, 0.0f, 0.35f, 0.0f, 1, 0, true, false},
    {"theend", Trigger::Suck, Motion::Suck, 4, 6, 2, 8.0f, 50.0f, 0.0f, 0.0f, 0.20f, -1, 50, false, false},
    {"lasbos", Trigger::Time, Motion::Orbit, 2, 2, 2, 10.0f, 0.15f, 0.0f, 0.05f, 0.0f, 1, 60, true, false},
};

}  // namespace

void register_mesh_flights(BespokeRegistry& registry) {
  for (const FlightParams& params : kFlights)
    registry.add({params.name, "mesh-deform", "FaceFlight", [params] { return std::make_unique<FaceFlight>(params); }, false});
  registry.add({"tumaru", "mesh-deform", "GhostCopies", [] { return std::make_unique<GhostCopies>(); }, false});
  registry.add({"tekken", "mesh-deform", "DustSweep", [] { return std::make_unique<DustSweep>(); }, false});
  registry.add({"rakugaki", "mesh-deform", "CellReveal", [] { return std::make_unique<CellReveal>(); }, false});
}

}  // namespace oc::effects
