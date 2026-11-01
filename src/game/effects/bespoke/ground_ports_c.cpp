// Ground overlays of the battle stage, third part: the ground parts added to the ports of other files (theend, 46black, 14bar), lasbos
// (the last boss' summon-in packs) and the data-only 22torune shell. They share the GroundFx base (ground_base.h).
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

#include "game/effects/bespoke/ground_base.h"
#include "game/effects/fx_math.h"

namespace oc::effects {

namespace {

using namespace ground_detail;

// ---- ground parts added to the ports of other files (theend, 46black, 14bar) -------------------------------------------------------------
// The registered module runs unchanged; the ground part runs beside it with the same context and ops (one overlay on the PS1).
class GroundAddon : public BespokeEffect {
 public:
  GroundAddon(std::unique_ptr<BespokeEffect> inner, std::unique_ptr<BespokeEffect> extra) : inner_(std::move(inner)), extra_(std::move(extra)) {}
  void start(const FxContext& ctx) override {
    ctx_ = ctx;
    if (inner_) inner_->start(ctx);
    if (extra_) extra_->start(ctx);
  }
  void tick() override {
    if (inner_) inner_->tick();
    if (extra_) extra_->tick();
  }
  void draw(std::vector<FxPrim>& out) const override {
    if (inner_) inner_->draw(out);
    if (extra_) extra_->draw(out);
  }
  bool done() const override { return (!inner_ || inner_->done()) && (!extra_ || extra_->done()); }
  bool command(int op, const int* args, int arg_count) override {
    const bool inner_acted = inner_ && inner_->command(op, args, arg_count);
    const bool extra_acted = extra_ && extra_->command(op, args, arg_count);
    return inner_acted || extra_acted;
  }
  void set_view(const ViewParams& view) override {
    ctx_.view = view;
    if (inner_) inner_->set_view(view);
    if (extra_) extra_->set_view(view);
  }

 private:
  std::unique_ptr<BespokeEffect> inner_, extra_;
};

// theend (Grim Reaper) ground shards. Op 0x180: P = (s16 @+8, @+10, @+12). Op 0x181 (N = s16 @+4): the stock ground is cut into triangle shards
// (the rim FT3 then GT3 triangles, then two per cell (LT RT LB) (RT LB RB), row-major), colours = ground colour x shard colour
// >> 7; vel = ((P - centroid) / N) << 4 per axis (1/16 units; cells use the cell centre, y 0), acc = floor(16 unit(vel)), start s = (0x1C49 -
// x - |z >> 2|) >> 6 as u16 (rim: the centroid; cells: the cell's left x and top z), axis = rand() % 3; the stock ground is switched off every frame. Frame:
// age += 1; s < age <= s + N: the shard moves by vel >> 4, vel += acc, turns 50 about its centroid on its axis; past s + N it is
// gone; before s it is drawn at home. Drawn both sides, opaque, no bias. Op 0x182: stock ground on, shards stop.
class EndShards : public GroundFx {
 protected:
  void init() override { seed_rng(); }
  bool command(int op, const int* args, int arg_count) override {
    switch (op) {
      case 0x180: target_point_ = {static_cast<s16>(arg(args, arg_count, 8)), static_cast<s16>(arg(args, arg_count, 10)), static_cast<s16>(arg(args, arg_count, 12))}; return true;
      case 0x181:
        flight_ticks_ = arg(args, arg_count, 4);
        build();
        active_ = true;
        return true;
      case 0x182:
        active_ = false;
        set_skip(false);
        return true;
      default: return false;
    }
  }
  void frame() override {
    FxGround* stage_ground = ground();
    if (!active_ || !stage_ground) return;
    for (Shard& shard : shards_) {
      shard.age = static_cast<u16>(shard.age + 1);
      const int age = static_cast<s16>(shard.age);
      if (static_cast<int>(shard.start) < age) {
        if (age > static_cast<int>(shard.start) + static_cast<int>(shard.duration)) continue;
        int step[3];
        for (int axis = 0; axis < 3; ++axis) {
          step[axis] = s16c(shard.vel[axis] >> 4);
          shard.vel[axis] = s16c(shard.vel[axis] + shard.acc[axis]);
        }
        int centroid[3] = {0, 0, 0};
        for (auto& vertex : shard.verts)
          for (int axis = 0; axis < 3; ++axis) vertex[static_cast<size_t>(axis)] = s16c(vertex[static_cast<size_t>(axis)] + step[axis]), centroid[axis] += vertex[static_cast<size_t>(axis)];
        for (int& sum : centroid) sum = s16c(sum) / 3;
        const int sin_turn = rsin(50), cos_turn = rcos(50);
        for (auto& vertex : shard.verts) {
          const int rel[3] = {s16c(vertex[0] - centroid[0]), s16c(vertex[1] - centroid[1]), s16c(vertex[2] - centroid[2])};
          int turned[3] = {rel[0], rel[1], rel[2]};
          if (shard.axis == 0) turned[1] = s16c((rel[1] * cos_turn - rel[2] * sin_turn) >> 12), turned[2] = s16c((rel[1] * sin_turn + rel[2] * cos_turn) >> 12);
          else if (shard.axis == 1) turned[0] = s16c((rel[2] * sin_turn + rel[0] * cos_turn) >> 12), turned[2] = s16c((rel[2] * cos_turn - rel[0] * sin_turn) >> 12);
          else turned[0] = s16c((rel[0] * cos_turn - rel[1] * sin_turn) >> 12), turned[1] = s16c((rel[0] * sin_turn + rel[1] * cos_turn) >> 12);
          for (int axis = 0; axis < 3; ++axis) vertex[static_cast<size_t>(axis)] = s16c(turned[axis] + centroid[axis]);
        }
      }
      FxGroundPrim prim;
      prim.n = 3;
      for (int k = 0; k < 3; ++k) {
        for (int axis = 0; axis < 3; ++axis) prim.p[k][static_cast<size_t>(axis)] = static_cast<float>(shard.verts[static_cast<size_t>(k)][static_cast<size_t>(axis)]);
        prim.uv[k][0] = shard.uv[k][0];
        prim.uv[k][1] = shard.uv[k][1];
        for (int channel = 0; channel < 3; ++channel) prim.rgb[k][channel] = shard.rgb[k][channel];
      }
      prim.clut = shard.clut;
      prim.tpage = shard.tpage;
      prim.two_sided = true;
      stage_ground->prims.push_back(prim);
    }
    set_skip(true);
  }

 private:
  struct Shard {
    std::array<std::array<int, 3>, 3> verts{};
    u8 uv[3][2] = {}, rgb[3][3] = {};
    u16 clut = 0, tpage = 0, start = 0, duration = 0, age = 0;
    int vel[3] = {}, acc[3] = {}, axis = 0;
  };
  void motion(Shard& shard, int cx, int cy, int cz) {
    const int centroid[3] = {cx, cy, cz};
    for (int k = 0; k < 3; ++k) shard.vel[k] = s16c(trunc_div(target_point_[static_cast<size_t>(k)] - centroid[k], flight_ticks_) << 4);
    // VectorNormalSS -> (s8)(unit >> 8): floor(16 x the unit vector)
    const float len = std::sqrt(static_cast<float>(shard.vel[0]) * shard.vel[0] + static_cast<float>(shard.vel[1]) * shard.vel[1] + static_cast<float>(shard.vel[2]) * shard.vel[2]);
    for (int k = 0; k < 3; ++k) {
      const int unit = len > 0 ? static_cast<int>(static_cast<float>(shard.vel[k]) * 4096.0f / len) : 0;
      shard.acc[k] = static_cast<s8>(static_cast<u16>(unit) >> 8);
    }
    shard.duration = static_cast<u16>(flight_ticks_);
  }
  void build() {
    shards_.clear();
    const FxGround* stage_ground = ground();
    // The shards are built only while the stock ground is on.
    if (!stage_ground || (stage_ground->skip_stock)) return;
    for (const FxGroundTri& tri : stage_ground->rim) {
      Shard shard;
      int centroid[3] = {0, 0, 0};
      for (int k = 0; k < 3; ++k) {
        for (int axis = 0; axis < 3; ++axis) shard.verts[static_cast<size_t>(k)][static_cast<size_t>(axis)] = tri.v[static_cast<size_t>(k)][static_cast<size_t>(axis)], centroid[axis] += tri.v[static_cast<size_t>(k)][static_cast<size_t>(axis)];
        shard.uv[k][0] = tri.uv[k][0];
        shard.uv[k][1] = tri.uv[k][1];
        for (int channel = 0; channel < 3; ++channel) shard.rgb[k][channel] = static_cast<u8>((stage_ground->rgb[channel] * tri.rgb[tri.gouraud ? k : 0][channel]) >> 7);
      }
      for (int& sum : centroid) sum /= 3;
      shard.clut = tri.clut;
      shard.tpage = tri.tpage;
      motion(shard, centroid[0], centroid[1], centroid[2]);
      int top = static_cast<s16>(centroid[2]) >> 2;
      if (top >= 0) top = -top;
      shard.start = static_cast<u16>((0x1C49 - centroid[0] + top) >> 6);
      shard.axis = rand() % 3;
      shards_.push_back(shard);
    }
    for (int row = 0; row < 10; ++row)
      for (int col = 0; col < 10; ++col) {
        const FxGroundCell& cell = stage_ground->cells[static_cast<size_t>(10 * row + col)];
        if (!cell.present) continue;
        const int zt = 0x1400 - 0x400 * row, xl = -0x1400 + 0x400 * col, zb = zt - 0x400, xr = xl + 0x400;
        const std::array<int, 3> corner[4] = {{xl, 0, zt}, {xr, 0, zt}, {xl, 0, zb}, {xr, 0, zb}};
        const int ccx = s16c((xl + xr + xl + xr) >> 2), ccz = s16c((zt + zt + zb + zb) >> 2);
        int top = zt >> 2;
        const u16 start = static_cast<u16>((0x1C49 - xl - (top < 0 ? -top : top)) >> 6);
        static constexpr int kTri[2][3] = {{0, 1, 2}, {1, 2, 3}};
        for (const auto& tri : kTri) {
          Shard shard;
          for (int k = 0; k < 3; ++k) {
            const int ci = tri[k];
            shard.verts[static_cast<size_t>(k)] = corner[ci];
            shard.uv[k][0] = cell.uv[ci][0];
            shard.uv[k][1] = cell.uv[ci][1];
            for (int channel = 0; channel < 3; ++channel) shard.rgb[k][channel] = static_cast<u8>((stage_ground->rgb[channel] * cell.rgb[ci][channel]) >> 7);
          }
          shard.clut = cell.clut;
          shard.tpage = cell.tpage;
          motion(shard, ccx, 0, ccz);
          shard.start = start;
          shard.axis = rand() % 3;
          shards_.push_back(shard);
          (void)top;
        }
      }
  }
  std::vector<Shard> shards_;
  std::array<int, 3> target_point_{0, 0, 0};
  int flight_ticks_ = 1;
  bool active_ = false;
};

// 46black (Black Hole) ground / scenery warp. Init: stock ground off, the overlay draws the whole ground itself every frame (no hole, 400 slots
// deeper) and hooks the mesh objects of the stage emitters 'near' and 'far'. Op 0x181 (A = s16 @+8, S = @+10, D = @+12): the clock T starts;
// op 0x182 stops and resets it (W = identity). W(v, T): r =SquareRoot0(|v|^2), t = max(0, T - (r >> S)); t > D: (0, -200, 0);
// else ix = x + (-x / D) t, iz = z + (-z / D) t, iy = s16(y + s16(s16(-200 - y) / D) * t), a = t << A, x' = (iz sin a >> 12) + (ix cos a >> 12),
// z' = (iz cos a >> 12) - (ix sin a >> 12) (s16 terms). The hook covers every 'near' / 'far' mesh object, also those spawned after init.
class BlackWarp : public GroundFx {
 protected:
  void init() override {
    set_skip(true);
    owns_warp_ = true;
    if (FxGround* stage_ground = ground()) {
      stage_ground->warp_emitters = {"near", "far"};
      stage_ground->draw_bias_add = 400;
      stage_ground->warp = [this](Vec3& position) { warp(position); };
    }
  }
  bool command(int op, const int* args, int arg_count) override {
    if (op == 0x181 || op == 0x182) log::debug("bespoke 46black: op {:X} (ground / scenery warp {})", op, op == 0x181 ? "starts" : "reset");
    if (op == 0x181) {
      running_ = true;
      clock_ = 0;
      angle_shift_ = static_cast<s16>(arg(args, arg_count, 8));
      radius_shift_ = static_cast<s16>(arg(args, arg_count, 10));
      span_ = static_cast<s16>(arg(args, arg_count, 12));
      return true;
    }
    if (op == 0x182) {
      running_ = false;
      clock_ = 0;
      return true;
    }
    return false;
  }
  void frame() override {
    if (running_) ++clock_;
    // A zero rect: every cell drawn.
    draw_ground(0, 0, 0, 0);
  }
  bool done() const override { return true; }

 private:
  void warp(Vec3& position) const {
    if (clock_ == 0) return;
    const int x = static_cast<int>(std::lround(position[0])), y = static_cast<int>(std::lround(position[1])), z = static_cast<int>(std::lround(position[2]));
    const int radius = ps1_square_root0(x * x + y * y + z * z);
    int elapsed = clock_ - (radius >> (radius_shift_ & 31));
    if (elapsed <= 0) return;
    if (span_ <= 0) return;
    if (span_ < elapsed) {
      position = {0.0f, -200.0f, 0.0f};
      return;
    }
    const int ix = x + (-x / span_) * elapsed, iz = z + (-z / span_) * elapsed;
    const int iy = s16c(y + s16c(s16c(-200 - y) / span_) * s16c(elapsed));
    const int angle = elapsed << (angle_shift_ & 31);
    position[0] = static_cast<float>(s16c((iz * rsin(angle)) >> 12) + s16c((ix * rcos(angle)) >> 12));
    position[1] = static_cast<float>(iy);
    position[2] = static_cast<float>(s16c((iz * rcos(angle)) >> 12) - s16c((ix * rsin(angle)) >> 12));
  }
  bool running_ = false;
  int clock_ = 0, angle_shift_ = 0, radius_shift_ = 0, span_ = 0;
};

// ---- lasbos (lasbos1..6, the last boss' summon-in packs; Portalgheist's 0x167 = lasbos5, eid 359) -----------------------------------------------
// The floor opens and the targets are sucked up the vortex. Init: the pack mesh 'anar' (the 10 x 10 cell
// floor) baked mode 2 at (0, 0) (packets 0x24 / 0x2C / 0x34 / 0x3C, the same bake as every ground overlay), the vertices of
// 'ana2' (the pit) closer than 0x1000 to the axis pushed out to radius 0x1000 on their own angle (x = rcos a, z = rsin a), the ground object kept
// (the stock draw off) and drawn by the frame with the rect {-0x1400, 0x1400, 0x1400, -0x1400} (every cell skipped: only the
// rim), the pack's 'yuka' emitter stopped and ONE object fired whose update copies the stage 'ring' object's colour every tick (the object
// draws 'anar'). Op 0x103 (D = s16 @+4): a task sets the 'yuka' object's morph (target 'ana2') weight 0x1000 t / D, 0x1000 at t > D and
// ends (the pit opens). Op 0x104: with the object alive its morph restarts (target 'ana2', weight 0), else the object is created; the ground is
// taken back (stock draw off, drawn again) and 'anar' baked again. Op 0x105: the ground draw stops (the stock draw stays off: no floor at all)
// and the 'yuka' object is freed. Op 0x101: like 0x105 plus the stock floor back. THE STAGE SWAP (the same three ops as bgchng /
// kamei): 0x100 (w = s16 @+8, the pack's own eid in all six packs) streams group-11 local file 3 w + 2 = the .ctd of slot w + 1 into VRAM;
// 0x101 frees the stage pack's objects and buffer and reads local file 3 w + 3 = the .prd of slot w + 1 over it; 0x102 registers the new
// resources and starts its objects - the host's
// set_stage(-1) / set_stage(kStageSlot + w + 1). lasbos1..5 / 6 (eid 351..359 / 530) -> bg_86, bg_87, bg_88, bg_89, bg_90 / bg_99: the Time
// Devourer's six arenas, kept for the rest of the battle. Exit (op 2): stock floor back, 'yuka' freed.
// Op 0x106 (D @+4, ang. accel -(@+0xA), vertical accel @+0xC, vertical speed @+0xE, rotation rates @+0x10 / +0x12 / +0x14, SetAnim(8, @+0x16),
// angular speed -(@+0x18) (i + 1)): per target i a task (run once at once): angle a = ratan2(z, x) and radius R0 = SquareRoot0(x^2 + z^2)
// of the model, r = R0 - R0 t / D; per tick a += as >> 4, as += accel, y -= vv 64 / 4096 (world), vv += vertical accel, the three rotations
// += their rates; x = r rcos a / 4096, z = r rsin a / 4096 (the vortex centre is the origin); at t > D r = 0 and the task ends.
// Op 0x107 (D @+4): per target a task: from the current place toward the saved home h: p += (h - p) t / D (integer, per tick); rotation
// x = z = 0, y = the old y (the original stores a local `y` that also serves as the yaw; kept literally for the D ticks); at t > D p = h and
// y rotation = -0x400 - ratan2(cz - z, cx - x) with c = the casters' bounding-box middle (the unit faces the caster). Frame: nothing else.
class Lasbos : public GroundFx {
 protected:
  void init() override {
    load_meshes();
    anar_ = mesh("anar");
    ana2_ = mesh("ana2");
    bake_mesh(anar_, 2, 0, 0);
    if (ana2_)
      for (auto& vertex : ana2_->verts) {
        const int x = vertex[0], z = vertex[2];
        const int angle = ratan2(z, x);
        if (ps1_square_root0(x * x + z * z) < 0x1000) {
          vertex[0] = s16c(rcos(angle));
          vertex[2] = s16c(rsin(angle));
        }
      }
    ground_obj_ = true;
    set_skip(true);
    own_emitter_meshes({"anar"});
    morph_from_ = "anar";
    morph_to_ = ana2_;
    if (em()) {
      em()->stop("yuka");
      yuka_ = em()->fire("yuka");
    }
  }
  bool command(int op, const int* args, int arg_count) override {
    switch (op) {
      // The new stage's .ctd streamed behind the pit's warp; the host's set_stage loads both files at 0x102.
      case 0x100: return true;
      case 0x101:
        if (ground_obj_) {
          ground_obj_ = false;
          set_skip(false);
        }
        free_yuka();
        // The stage is freed and local file 3 w + 3 (= slot w + 1's .prd) read over it; 0x102 starts it.
        stage_slot_ = arg(args, arg_count, 8) + 1;
        if (ctx_.hooks.set_stage) ctx_.hooks.set_stage(-1);
        return true;
      case 0x102:
        if (stage_slot_ > 0 && ctx_.hooks.set_stage) ctx_.hooks.set_stage(kStageSlot + stage_slot_);
        stage_slot_ = 0;
        return true;
      case 0x105:
        ground_obj_ = false;
        free_yuka();
        return true;
      case 0x103: {  // the first run is the next tick
        const int duration = arg(args, arg_count, 4);
        auto ticks = std::make_shared<int>(0);
        add_task([this, duration, ticks] {
          int weight = duration != 0 ? (*ticks * 0x1000) / duration : 0x1000;
          *ticks += 1;
          bool live = true;
          if (duration < *ticks) {
            weight = 0x1000;
            live = false;
          }
          if (yuka_) morph_w_ = weight;
          return live;
        });
        return true;
      }
      case 0x104:
        if (!yuka_ && em()) {
          em()->stop("yuka");
          yuka_ = em()->fire("yuka");
        } else {
          // +0x9e = 0, +0x9c = 0, +0x98 = 'ana2', +0x94 |= 2
          morph_w_ = 0;
        }
        ground_obj_ = true;
        set_skip(true);
        bake_mesh(anar_, 2, 0, 0);
        return true;
      case 0x106: spiral(args, arg_count); return true;
      case 0x107: home(args, arg_count); return true;
      default: return false;
    }
  }
  void frame() override {
    if (ground_obj_) draw_ground(-0x1400, 0x1400, 0x1400, -0x1400);
    if (yuka_) {
      const FxGround* stage_ground = fx_ground();
      draw_emitter_meshes(stage_ground ? stage_ground->rgb : nullptr);
    }
  }

 private:
  void free_yuka() {
    if (!yuka_) return;
    yuka_ = false;
    morph_w_ = -1;
    if (em()) em()->kill("yuka");
  }

  // Op 0x106: the vortex spiral.
  void spiral(const int* args, int arg_count) {
    for (int i = 0; i < target_count(); ++i) {
      FxModel* model = target(i);
      if (!model) continue;
      struct Spiral {
        FxModel* model = nullptr;
        int tick = 0, duration = 0, R0 = 0;
        s16 ang = 0, as = 0, accel = 0;
        int vacc = 0, vv = 0, rv[3] = {0, 0, 0};
      };
      auto st = std::make_shared<Spiral>();
      st->model = model;
      st->duration = arg(args, arg_count, 4);
      const int x = static_cast<int>(std::floor(model->pos[0])), z = static_cast<int>(std::floor(model->pos[2]));
      st->ang = s16c(ratan2(z, x));
      st->R0 = ps1_square_root0(x * x + z * z);
      st->as = s16c(-arg(args, arg_count, 0x18) * (i + 1));
      st->accel = s16c(-arg(args, arg_count, 0xA));
      st->vacc = arg(args, arg_count, 0xC);
      st->vv = arg(args, arg_count, 0xE);
      for (int k = 0; k < 3; ++k) st->rv[k] = arg(args, arg_count, 0x10 + 2 * k);
      set_anim(model, 8, arg(args, arg_count, 0x16));
      auto step = [st]() {
        FxModel* spun = st->model;
        int radius = 0;
        if (st->duration != 0) radius = st->R0 - (st->R0 * st->tick) / st->duration;
        st->tick += 1;
        bool live = true;
        if (st->duration < st->tick) {
          radius = 0;
          live = false;
        }
        st->ang = s16c(st->ang + (st->as >> 4));
        st->as = s16c(st->as + st->accel);
        spun->pos[1] += static_cast<float>(st->vv * -0x40) / 4096.0f;
        st->vv += st->vacc;
        for (int k = 0; k < 3; ++k) spun->rot[k] = s16c(spun->rot[k] + st->rv[k]);
        spun->pos[0] = static_cast<float>(radius * rcos(st->ang)) / 4096.0f;
        spun->pos[2] = static_cast<float>(radius * rsin(st->ang)) / 4096.0f;
        spun->steer = true;
        return live;
      };
      if (step()) add_task(step);
    }
  }
  // Op 0x107: back to the saved home.
  void home(const int* args, int arg_count) {
    for (int i = 0; i < target_count(); ++i) {
      FxModel* model = target(i);
      if (!model) continue;
      auto ticks = std::make_shared<int>(0);
      const int duration = arg(args, arg_count, 4);
      auto step = [this, model, ticks, duration]() {
        const int px = static_cast<int>(std::floor(model->pos[0])), py = static_cast<int>(std::floor(model->pos[1])), pz = static_cast<int>(std::floor(model->pos[2]));
        const int hx = static_cast<int>(model->home[0]), hy = static_cast<int>(model->home[1]), hz = static_cast<int>(model->home[2]);
        // The casters' bounding-box middle.
        int cx = 0, cz = 0;
        if (ctx_.models && !ctx_.models->casters.empty()) {
          int lo[2] = {INT_MAX, INT_MAX}, hi[2] = {INT_MIN, INT_MIN};
          for (int unit_index : ctx_.models->casters) {
            const FxModel& caster_unit = ctx_.models->units[static_cast<size_t>(unit_index)];
            const int place[2] = {static_cast<int>(std::floor(caster_unit.pos[0])), static_cast<int>(std::floor(caster_unit.pos[2]))};
            for (int k = 0; k < 2; ++k) lo[k] = std::min(lo[k], place[k]), hi[k] = std::max(hi[k], place[k]);
          }
          cx = (lo[0] + hi[0]) / 2;
          cz = (lo[1] + hi[1]) / 2;
        }
        const int face = ratan2(cz - pz, cx - px);
        int nx = px, ny = py, nz = pz, roty = py;
        if (duration != 0) {
          nx = px + ((hx - px) * *ticks) / duration;
          ny = py + ((hy - py) * *ticks) / duration;
          nz = pz + ((hz - pz) * *ticks) / duration;
        }
        *ticks += 1;
        bool live = true;
        if (duration < *ticks) {
          nx = hx, ny = hy, nz = hz;
          roty = -0x400 - face;
          live = false;
        }
        model->pos = {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)};
        model->rot[0] = 0;
        model->rot[1] = s16c(roty);
        model->rot[2] = 0;
        model->steer = true;
        return live;
      };
      if (step()) add_task(step);
    }
  }

  PackMesh *anar_ = nullptr, *ana2_ = nullptr;
  // The ground object is kept; the 'yuka' object is alive; the slot whose .prd op 0x101 read (0 = none).
  bool ground_obj_ = false;
  bool yuka_ = false;
  int stage_slot_ = 0;
};

// 14bar (Inferno) heat wave: from op 0x180 to op 0x181 the stage's 'near' mesh objects are drawn with every vertex y += rcos(z + 16 T) 100 >> 12
// and z += rcos(y + 64 T) 200 >> 12 (the original y), T = ticks since the overlay's first frame. It needs no ground: it uses the
// same per-vertex hook of the stage (FxGround::warp, model space of the mesh here, which the hook receives in world space: the scenery
// objects are unrotated / unscaled, so their file space equals world space minus the position). While the wave runs the objects that have
// a blend mode are drawn with mode 4 (B + F/4) whatever their own mode is; opaque ones stay opaque. The op reaches the objects alive at
// that moment: objects the emitter spawns later keep the stock draw.
class BarWave : public GroundFx {
 public:
  ~BarWave() override {
    if (FxGround* stage_ground = fx_ground()) stage_ground->warp_quarter_add = false;
  }

 protected:
  bool command(int op, const int* args, int arg_count) override {
    (void)args;
    (void)arg_count;
    const bool handled = op == 0x180 || op == 0x181;
    FxGround* stage_ground = fx_ground();
    if (!stage_ground) return handled;
    log::debug("bespoke 14bar: op {:X} (heat wave {})", op, op == 0x180 ? "on" : "off");
    if (op == 0x180) {
      owns_warp_ = true;
      stage_ground->warp_quarter_add = true;
      stage_ground->warp_emitters = {"near"};
      stage_ground->warp = [this](Vec3& position) {
        const int y = static_cast<int>(std::lround(position[1])), z = static_cast<int>(std::lround(position[2]));
        position[1] = static_cast<float>(s16c(y + ((rcos(z + clock_ * 0x10) * 100) >> 12)));
        position[2] = static_cast<float>(s16c(z + ((rcos(y + clock_ * 0x40) * 200) >> 12)));
      };
    } else if (op == 0x181) {
      stage_ground->warp = nullptr;
      stage_ground->warp_quarter_add = false;
      stage_ground->warp_emitters.clear();
    }
    return handled;
  }
  // No ground draws: the per-tick lists stay with the stock path.
  void tick() override { ++clock_; }
  bool done() const override { return true; }

 private:
  int clock_ = 0;
};

class Shell : public OverlayFx {};

// Wraps the module registered under `name` (another file's port) with a ground part.
template <class Extra>
void add_ground_part(BespokeRegistry& registry, const char* name, const char* family) {
  const BespokeInfo* prev = registry.find(name);
  std::function<std::unique_ptr<BespokeEffect>()> inner = prev && prev->name == name ? prev->make : nullptr;
  const std::string module = prev && prev->name == name ? prev->module + "+ground" : std::string("ground");
  const bool battle = prev && prev->name == name ? prev->battle : true;
  registry.add({name, prev && prev->name == name ? prev->family : family, module,
         [inner] { return std::make_unique<GroundAddon>(inner ? inner() : nullptr, std::make_unique<Extra>()); }, battle});
}

}  // namespace

void register_ground_ports_c(BespokeRegistry& registry) {
  registry.add({"22torune", "shell", "Shell", [] { return std::make_unique<Shell>(); }});
  registry.add({"lasbos", "ground", "Lasbos", [] { return std::make_unique<Lasbos>(); }});
  add_ground_part<EndShards>(registry, "theend", "summon");
  add_ground_part<BlackWarp>(registry, "46black", "model-deform");
  add_ground_part<BarWave>(registry, "14bar", "scripted-motion");
}

}  // namespace oc::effects
