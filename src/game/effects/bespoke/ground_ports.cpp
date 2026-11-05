// Overlays built around the battle stage's ground (battle.prg's type-0x13 object = the stage's drp type-0x12 'ring': a 10 x 10 grid of 0x400
// cells plus the rim triangles). They switch the stock ground draw off (the ground's skip flag),
// draw it themselves with a hole (the stock ground draw with a rectangle), bake the ground's texels / CLUT / colours into their own pack meshes
// (one shared helper below) and draw those meshes, which the host renders with the STAGE's textures (FxGroundPrim).
// Everything reaches the stage through effects::fx_ground() (game::StageScene owns it); without a stage (tools, tests) the model / emitter
// parts still run.
//   wipeoff  Web-surfer      slab task, debris task
//   tekken   Beaten Earth    piece task
//   jitanda  Tantrum / Make Waves
//   2boll    Aqua Ball       particle hooks
//   saraman  Salamander      ops 0x180, 0x181, 0x182, 0x184
//   11boru   Volcano
//   25upli   Uplift          op 0x180
//   42hevi   Gravitonne      op 0x103, 'hole' op-0x48 callback, 'yuka' update
//   22torune Tornado         no private op reaches the overlay (its 0x100 jump handler is never sent): data only
//   theend   Grim Reaper     ground shards: op 0x181 build (added to the summon port)
//   46black  Black Hole      ground copy + warp, 'near' / 'far' draw hook (added to the model port)
//   14bar    Inferno         'near' draw hook / wobble between ops 0x180 and 0x181 (added to the stand-in)
// dt is one 60 Hz tick here, like every other port.
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <set>
#include <string>

#include "game/effects/bespoke/ground_base.h"
#include "game/effects/bespoke/model_draw.h"
#include "game/effects/fx_math.h"
#include "game/effects/sequence_player.h"

namespace oc::effects {

namespace {
FxGround* g_ground = nullptr;
}
FxGround* fx_ground() { return g_ground; }
void set_fx_ground(FxGround* ground) { g_ground = ground; }

namespace {

using namespace ground_detail;

// ---- wipeoff (Web-surfer, eid 24) ---------------------------------------------------------------------------------------------------------
// Init: stock ground off, caster 0 anmseq 0x80. Op 0x100 (D = s16 @+4 = 92): 8 slabs (pack mesh 'base' and its copies bas1..bas7) each baked
// mode 2 at x offset -0x200 i, bias 0x50, a slab task per slab (start, end D and phase from per-slab tables). Frame: while active the ground
// is drawn with the hole {-0x1000, 0xC00, 0, -0xC00} (4 x 6 cells), stock ground off, ticks counted, past D the wipe ends (stock ground on from
// the next frame). Slab (pos 20.4 world * 16, vy -800 +32 per tick): rises until `phase`, shakes 20 ticks (rand % 10 / % 30 / % 10 drawn offset), holds;
// past D it hands over to five debris b0<i><j> (copies of b00j baked mode 2 at its x), angular velocity (0x18 - r % 0x30, 0xF - r % 0x1E,
// 0x1E - r % 0x3C), phase r % 16: they spin and rise 200 / 16 per tick until `phase`, fall 100 / 16 per tick for 40 ticks, then fade (colour
// 0x800 - 13 t per channel >> 4) drawn twice, additive then subtractive, until tick 0x78.
class Wipeoff : public GroundFx {
 protected:
  void init() override {
    seed_rng();
    load_meshes();
    set_skip(true);
    caster_anmseq_0x80();
  }
  bool command(int op, const int* args, int arg_count) override {
    if (op != 0x100) return false;
    duration_ = arg(args, arg_count, 4);
    tick_ = 0;
    static constexpr int kStart[8] = {0, 3, 3, 0, 4, 0, 4, 0}, kPhase[8] = {7, 0x12, 0xC, 0x18, 0x12, 7, 0x1A, 0x14};
    int px = 0;
    for (int i = 0; i < 8; ++i) {
      auto slab_mesh = copy("base");
      if (!slab_mesh) continue;
      bake_mesh(slab_mesh.get(), 2, px >> 4, 0);
      auto slab_state = std::make_shared<Slab>();
      slab_state->mesh = slab_mesh;
      slab_state->centre = slab_mesh->centroid();
      slab_state->start = kStart[i];
      slab_state->end = duration_;
      slab_state->phase = kPhase[i];
      slab_state->idx = i;
      slab_state->pos = {px, 0, 0};
      slab_state->vy = -800;
      add_task([this, slab_state] { return slab(*slab_state); });
      px -= 0x2000;
    }
    active_ = true;
    return true;
  }
  void frame() override {
    if (active_) {
      ++tick_;
      set_skip(true);
      draw_ground(-0x1000, 0x0C00, 0x0000, -0x0C00);
      if (duration_ < tick_) active_ = false;
    } else {
      set_skip(false);
    }
  }

 private:
  struct Slab {
    std::shared_ptr<PackMesh> mesh;
    std::array<int, 3> centre{};
    int tick = 0, start = 0, end = 0, phase = 0, idx = 0, vy = 0;
    std::array<int, 3> pos{};
  };
  struct Debris {
    std::shared_ptr<PackMesh> mesh;
    std::array<int, 3> centre{};
    int tick = 0, end = 0x78, phase = 0;
    std::array<int, 3> pos{}, rot{}, angv{};
    int fade[3] = {0x800, 0x800, 0x800};
  };
  bool slab(Slab& state) {
    const int tick = ++state.tick;
    Vec3 pw{static_cast<float>(state.pos[0] >> 4), static_cast<float>(state.pos[1] >> 4), static_cast<float>(state.pos[2] >> 4)};
    if (tick > state.start) {
      if (tick >= state.phase) {
        if (tick < state.phase + 0x14) {
          pw[0] += static_cast<float>(rand() % 10);
          pw[1] += static_cast<float>(rand() % 0x1E);
          pw[2] += static_cast<float>(rand() % 10);
        }
      } else {
        const int vy_old = state.vy;
        state.vy += 0x20;
        state.pos[1] += vy_old;
        pw = {static_cast<float>(state.pos[0] >> 4), static_cast<float>(state.pos[1] >> 4), static_cast<float>(state.pos[2] >> 4)};
      }
    }
    MeshDraw draw;
    draw.centre = state.centre;
    draw.offset = pw;
    draw.bias = 0x50;
    draw_mesh(*state.mesh, draw);
    if (state.end < tick) {
      for (int j = 0; j < 5; ++j) {
        auto debris_mesh = copy(std::string("b00") + static_cast<char>('0' + j));
        if (!debris_mesh) continue;
        bake_mesh(debris_mesh.get(), 2, state.pos[0] >> 4, 0);
        auto fragment = std::make_shared<Debris>();
        fragment->mesh = debris_mesh;
        fragment->centre = debris_mesh->centroid();
        fragment->pos = state.pos;
        fragment->angv[0] = 0x18 - rand() % 0x30;
        fragment->angv[1] = 0xF - rand() % 0x1E;
        fragment->angv[2] = 0x1E - rand() % 0x3C;
        fragment->phase = rand() % 16;
        add_task([this, fragment] { return debris(*fragment); });
      }
      return false;
    }
    return true;
  }
  bool debris(Debris& fragment) {
    MeshDraw draw;
    draw.centre = fragment.centre;
    for (int k = 0; k < 3; ++k) draw.rot[k] = fragment.rot[static_cast<size_t>(k)];
    draw.offset = {static_cast<float>(fragment.pos[0] >> 4), static_cast<float>(fragment.pos[1] >> 4), static_cast<float>(fragment.pos[2] >> 4)};
    if (fragment.tick < fragment.phase + 0x28) {
      if (fragment.tick < fragment.phase) {
        for (size_t k = 0; k < 3; ++k) fragment.rot[k] += fragment.angv[k];
        fragment.pos[1] += -200;
      } else {
        fragment.pos[1] += 100;
      }
      draw_mesh(*fragment.mesh, draw);
    } else {
      for (int channel = 0; channel < 3; ++channel) {
        fragment.fade[channel] = fragment.fade[channel] > 0 ? fragment.fade[channel] - 0xD : 0;
        draw.rgb[channel] = static_cast<u8>(fragment.fade[channel] >> 4);
      }
      draw.semi = true;
      draw.abr = 1;
      draw_mesh(*fragment.mesh, draw);
      draw.abr = 2;
      draw_mesh(*fragment.mesh, draw);
    }
    ++fragment.tick;
    return !(fragment.end < fragment.tick);
  }
  int duration_ = 92, tick_ = 0;
  bool active_ = false;
};

// ---- tekken (Beaten Earth, eid 284) -------------------------------------------------------------------------------------------------------
// Init: stock ground on, tint = the stage 'ring' emitter's COLOR >> 4, 30 copies b010..b0N0 of rand() % 3 -> b000 / b001 /
// b002, the six h000..h005 baked mode 2 at the table offsets (drawn by their emitters), caster 0 anmseq 0x80. Op 0x100 (D = s16 @+4): from
// the caster's joint 8 cell to target 0's cell (grid gx = (x + 0x1400) >> 9, gz = (z + 0x1400) >> 8) one piece per column, piece i at
// (gx 0x200 - 0x1200, 0, gz 0x100 - 0x1500) baked mode 2 there, z stepping by ((gzT - gzC) << 12) / |dx| (20.12); piece task:
// drawn from tick 4i + 1 to D at that spot, bias 0x32, colour = the tint; on its first frame 'smok' and 'h01<r % 3>' fire there.
class Tekken : public GroundFx {
 protected:
  void init() override {
    seed_rng();
    load_meshes();
    set_skip(false);
    if (const FxGround* stage_ground = fx_ground())
      for (int channel = 0; channel < 3; ++channel) tint_[channel] = stage_ground->ring_colour[channel];
    for (int k = 0; k < 30; ++k) {
      const int pick = rand() % 3;
      if (auto piece_mesh = copy(std::string("b00") + static_cast<char>('0' + pick))) pieces_.push_back(piece_mesh);
      else pieces_.push_back(nullptr);
    }
    static constexpr int kOff[6][2] = {{262, -285}, {-851, 872}, {-705, 725}, {-250, 766}, {969, 109}, {743, 253}};
    std::set<std::string> baked_names;
    for (int j = 0; j < 6; ++j) {
      const std::string name = std::string("h00") + static_cast<char>('0' + j);
      if (bake_mesh(mesh(name), 2, kOff[j][0], kOff[j][1])) baked_names.insert(name);
    }
    own_emitter_meshes(baked_names);
    caster_anmseq_0x80();
  }
  bool command(int op, const int* args, int arg_count) override {
    if (op != 0x100) return false;
    const FxModel* tm = target(0);
    const FxModel* cm = caster(0);
    if (!tm || !cm) return true;
    auto grid = [](int x, int z, int& gx, int& gz) {
      gx = (x + 0x1400) >> 9;
      gz = (z + 0x1400) >> 8;
    };
    int gxT = 0, gzT = 0, gxC = 0, gzC = 0;
    grid(static_cast<int>(tm->pos[0]), static_cast<int>(tm->pos[2]), gxT, gzT);
    int start[3] = {static_cast<int>(cm->pos[0]), 0, static_cast<int>(cm->pos[2])};
    Vec3 point{};
    if (ctx_.models && ctx_.models->attach && ctx_.models->attach(cm->unit, 8, point)) start[0] = static_cast<int>(std::lround(point[0])), start[2] = static_cast<int>(std::lround(point[2]));
    grid(start[0], start[2], gxC, gzC);
    duration_ = arg(args, arg_count, 4);
    tick_ = 0;
    const int dx = gxT - gxC, nn = std::abs(dx), step = dx < 0 ? -1 : 1;
    int zf = gzC << 12, gx = gxC;
    for (int i = 0; i < nn + 1; ++i) {
      std::shared_ptr<PackMesh> piece_mesh;
      if (i == 0) piece_mesh = copy("b000");
      else if (static_cast<size_t>(i - 1) < pieces_.size()) piece_mesh = pieces_[static_cast<size_t>(i - 1)];
      if (piece_mesh) {
        const int ox = gx * 0x200 - 0x1200, oz = (zf >> 12) * 0x100 - 0x1500;
        bake_mesh(piece_mesh.get(), 2, ox, oz);
        auto piece_state = std::make_shared<Piece>();
        piece_state->mesh = piece_mesh;
        piece_state->centre = piece_mesh->centroid();
        piece_state->start = i << 2;
        piece_state->end = duration_;
        piece_state->pos = {static_cast<float>(ox), 0.0f, static_cast<float>(oz)};
        add_task([this, piece_state] { return piece(*piece_state); });
      }
      gx += step;
      // The PS1 divides by zero here (caster and target in one column): one piece.
      if (nn == 0) break;
      zf += trunc_div((gzT - gzC) * 0x1000, nn);
    }
    return true;
  }
  void frame() override {
    draw_emitter_meshes();
    ++tick_;
  }

 private:
  struct Piece {
    std::shared_ptr<PackMesh> mesh;
    std::array<int, 3> centre{};
    int tick = 0, start = 0, end = 0;
    Vec3 pos{0, 0, 0};
    bool fired = false;
  };
  bool piece(Piece& state) {
    const int tick = ++state.tick;
    if (state.start < tick) {
      if (!state.fired && em()) {
        if (em()->set_position("smok", static_cast<int>(state.pos[0]), 0, static_cast<int>(state.pos[2]))) em()->fire("smok"), state.fired = true;
        const std::string hit_name = std::string("h01") + static_cast<char>('0' + rand() % 3);
        if (em()->set_position(hit_name, static_cast<int>(state.pos[0]), 0, static_cast<int>(state.pos[2]))) em()->fire(hit_name), state.fired = true;
      }
      MeshDraw draw;
      draw.centre = state.centre;
      draw.offset = state.pos;
      draw.bias = 0x32;
      for (int channel = 0; channel < 3; ++channel) draw.rgb[channel] = tint_[channel];
      draw_mesh(*state.mesh, draw);
    }
    return !(state.end < tick);
  }
  std::vector<std::shared_ptr<PackMesh>> pieces_;
  u8 tint_[3] = {128, 128, 128};
  int duration_ = 0, tick_ = 0;
};

// ---- the 21 x 21 water / ripple sheet of jitanda and 2boll ---------------------------------------------------------------------------------
// Two int buffers of 21 x 21 (index (x + 0x1400) >> 9) * 21 + ((z + 0x1400) >> 9), border 0); one step per tick: for the interior cells
// n = ((N + S + W + E) >> 1) - old, stored n - (n >> damp) into the other buffer, then the two swap. The mesh 'mdl1' (21 x 21 vertices, 400
// GT4 at 0x200, baked mode 1 at init) is drawn with every vertex y replaced by the field, opaque, both sides, OT + 200 (AVSZ3 of corners 0-2:
// here the packet centroid), no mesh colour / bias.
struct Ripple {
  std::array<std::array<int, 441>, 2> buf{};
  int cur = 0, old = 1;
  static int index(int x, int z) { return ((x + 0x1400) >> 9) * 21 + ((z + 0x1400) >> 9); }
  void step(int damp) {
    const auto& newest = buf[static_cast<size_t>(cur)];
    auto& older = buf[static_cast<size_t>(old)];
    for (int row = 1; row < 20; ++row)
      for (int col = 1; col < 20; ++col) {
        const int sum = newest[static_cast<size_t>((row - 1) * 21 + col)] + newest[static_cast<size_t>((row + 1) * 21 + col)] + newest[static_cast<size_t>(row * 21 + col - 1)] +
                        newest[static_cast<size_t>(row * 21 + col + 1)];
        const int nv = (sum >> 1) - older[static_cast<size_t>(row * 21 + col)];
        older[static_cast<size_t>(row * 21 + col)] = nv - (nv >> damp);
      }
    std::swap(cur, old);
  }
  int sample(int which, int x, int z) const {
    const int i = index(x, z);
    return i >= 0 && i < 441 ? static_cast<s16>(buf[static_cast<size_t>(which)][static_cast<size_t>(i)]) : 0;
  }
};

void draw_sheet(FxGround* stage_ground, const PackMesh* mesh, const Ripple& ripple, int which) {
  if (!stage_ground || !mesh) return;
  std::vector<int> heights(mesh->verts.size());
  for (size_t i = 0; i < heights.size(); ++i) heights[i] = ripple.sample(which, mesh->verts[i][0], mesh->verts[i][2]);
  MeshDraw draw;
  draw.bias = 200;
  draw.two_sided = true;
  draw.heights = &heights;
  draw.depth_cue = true;
  // Only the GT3 / GT4 packets are drawn, opaque whatever their code (the packets are rebuilt with code 0x3C / 0x34)
  PackMesh sheet = *mesh;
  std::erase_if(sheet.prims, [](const PackPrim& prim) { return prim.code != 0x34 && prim.code != 0x3C; });
  for (PackPrim& prim : sheet.prims) prim.semi = false;
  emit_mesh(*stage_ground, sheet, draw);
}

// ---- jitanda (Tantrum eid 141, Make Waves eid 285) ------------------------------------------------------------------------------------------
// Init: targets' drop shadow off; the source cell = caster 0's ((x + 0x1400) >> 9, (z + 0x1400) >> 9); stock ground off;
// 'mdl1' baked mode 1; caster 0 anmseq 0x80. Op 0x180 (amp = s16 @+8, n = s16 @+10): an n x n square of `amp` (set, not added) round the
// source in the newest buffer, rows / columns 1..19 only. Frame: the ground drawn with the rect {-0x1400, 0x1400, 0x1400, -0x1400} (every
// cell skipped: only the rim), one field step (damping >> 4), the sheet drawn from the older buffer, every target's y = the newest buffer
// at its (x, z) (while the event variable 0x80 is 0: nothing sets it). Exit: stock ground on, target y = 0, shadows back on.
class Jitanda : public GroundFx {
 public:
  ~Jitanda() override {
    for (int i = 0; i < target_count(); ++i)
      if (FxModel* model = target(i)) {
        model->pos[1] = 0;
        flag_set(model, 0x200);
      }
  }

 protected:
  void init() override {
    load_meshes();
    for (int i = 0; i < target_count(); ++i) flag_clear(target(i), 0x200);
    if (const FxModel* caster_model = caster(0)) {
      base_row_ = (static_cast<int>(caster_model->pos[0]) + 0x1400) >> 9;
      base_col_ = (static_cast<int>(caster_model->pos[2]) + 0x1400) >> 9;
    }
    set_skip(true);
    bake_mesh(mesh("mdl1"), 1, 0, 0);
    caster_anmseq_0x80();
  }
  bool command(int op, const int* args, int arg_count) override {
    if (op != 0x180) return false;
    const int amp = static_cast<s16>(arg(args, arg_count, 8)), side = static_cast<s16>(arg(args, arg_count, 10));
    if (side <= 0) return true;
    const int half = side >> 1;
    auto& newest = ripple_.buf[static_cast<size_t>(ripple_.cur)];
    for (int i = 0; i < side; ++i)
      for (int j = 0; j < side; ++j) {
        const int row = base_row_ + i - half, col = base_col_ + j - half;
        if (row >= 1 && row <= 19 && col > 0 && col < 20) newest[static_cast<size_t>(row * 21 + col)] = amp;
      }
    return true;
  }
  void frame() override {
    draw_ground(-0x1400, 0x1400, 0x1400, -0x1400);
    ripple_.step(4);
    draw_sheet(ground(), mesh("mdl1"), ripple_, ripple_.old);
    for (int i = 0; i < target_count(); ++i)
      if (FxModel* model = target(i)) {
        model->pos[1] = static_cast<float>(ripple_.buf[static_cast<size_t>(ripple_.cur)][static_cast<size_t>(std::clamp(Ripple::index(static_cast<int>(model->pos[0]), static_cast<int>(model->pos[2])), 0, 440))]);
        model->steer = true;
      }
  }

 private:
  Ripple ripple_;
  int base_row_ = 10, base_col_ = 10;
};

// ---- 2boll (Aqua Ball, eid 33) -------------------------------------------------------------------------------------------------------------
// Init: stock ground off, 'mdl1' baked mode 1, both buffers zero (indices cur 0 / other 1: every step reads one buffer and writes the other). Frame: emitter '2' particles
// above y -0x2EE set their cell to 0x800 (the first one also fires '6' at (x, 0, z) and starts '13' at (x, 0, z) and '3' at (x, -100, z)),
// emitter '3' particles above y -0x15E set 0x80 (before the particle's own step); the ground drawn with the rect
// {-0x1400, 0x1400, 0x1400, -0x1400}; one step (damping >> 2); the sheet drawn from the buffer the step read. Exit: stock ground on.
class Boll2 : public GroundFx {
 protected:
  void init() override {
    load_meshes();
    set_skip(true);
    bake_mesh(mesh("mdl1"), 1, 0, 0);
    ripple_.cur = 0, ripple_.old = 1;
    if (!em()) return;
    em()->set_object_hook("2", [this](EmitterSystem::ObjectRef& object) {
      const int x = object.pos[0] >> 16, y = object.pos[1] >> 16, z = object.pos[2] >> 16;
      if (y <= -0x2EE) return;
      stimulate(x, z, 0x800);
      if (!splashed_) {
        splashed_ = true;
        pending_ = {x, z};
        has_pending_ = true;
      }
    });
    em()->set_object_hook("3", [this](EmitterSystem::ObjectRef& object) {
      if ((object.pos[1] >> 16) > -0x15E) stimulate(object.pos[0] >> 16, object.pos[2] >> 16, 0x80);
    });
  }
  void frame() override {
    // The hooks must not spawn: the first splash's emitters go here, the same tick.
    if (has_pending_ && em()) {
      has_pending_ = false;
      const int x = pending_[0], z = pending_[1];
      em()->set_position("6", x, 0, z);
      em()->fire("6");
      em()->set_position("13", x, 0, z);
      em()->start("13");
      em()->set_position("3", x, -100, z);
      em()->start("3");
    }
    draw_ground(-0x1400, 0x1400, 0x1400, -0x1400);
    ripple_.step(2);
    draw_sheet(ground(), mesh("mdl1"), ripple_, ripple_.old);
  }

 private:
  void stimulate(int x, int z, int amplitude) {
    const int xi = (x + 0x1400) >> 9, zi = (z + 0x1400) >> 9;
    if (xi >= 1 && xi <= 19 && zi > 0 && zi < 20) ripple_.buf[static_cast<size_t>(ripple_.cur)][static_cast<size_t>(xi * 21 + zi)] = amplitude;
  }
  Ripple ripple_;
  bool splashed_ = false, has_pending_ = false;
  std::array<int, 2> pending_{};
};

// ---- saraman (Salamander) ------------------------------------------------------------------------------------------------------------------
// Ground part. Init: stock ground on, hah0..hah3 baked mode 1 at (0, 0). Op 0x180 (cx = s16 @+8, cz = s16 @+10): the hole rect {cx 0x400 -
// 0x800, cz 0x400 + 0x800, cx 0x400 + 0x800, cz 0x400 - 0x800} (4 x 4 cells), P = (cx << 10, cz << 10); per piece m00..m17: centroid c, rot
// 0, thr = SquareRoot0(|c|^2) >> 4, vy = rand() % 20 + 10; 'base' then the pieces baked mode 1 at P. The pit is on from the
// moment the first motion file is in (and 0x181); 0x184: pit off. Frame (t += dt from init): while the pit is on, the stock ground is off, the
// ground drawn with the hole, 'base' at P (bias 100, OT + 200 -> +200 more), each piece at P + (0, vy (t - thr), 0) spinning (0x1E, 0x50, 0x46)
// per tick once t > thr (bias 100 then; 0 before); else the stock ground is on.
// The 'sara' creature (host side FxModels::PackDrive): the pack's type-0x14 model 'sara' (its texture binding is chosen by
// fx_pack_model_binding, the name carries none). Init: position 0, rotation 0, SetAnim(0, 0), hidden, the stream directory set to group 11,
// file 0x2C (the AKAO bank wave0320) requested, every unit's part-flag bit 0x400 cleared, 'init' started and the effect held.
// Frame: t += dt (dt = the frame's tick count). (1) BANK = 0, no CD read pending and t > 0x50: BANK = 1, the bank uploaded (audio, not
// reproduced) and freed, then the loader for file 0 (op 0x183: file 0x2D + NEXT into buffer NEXT, LOADING = 1). (2) LOADING and the read
// finished: NEXT == 0 -> start 'own' (when caster 0 is target 0 and the pack has it, else 'main'), the hold released, 'sara' shown,
// FIRST = 1, PIT = 1; LOADING = 0, READY[NEXT] = 1, NEXT += 1. (3) The model's animation over (a 1-frame clip is over at once) and FIRST:
// K < 3 -> READY[K] ? (the type-2 sound channels resumed if PAUSED; buffer K attached, SetAnim(0x80, 1), for K == 0 one pose step at once,
// shown, buffer K - 1 freed, K += 1) : (the type-2 sound channels suspended, dt = 0 for the rest of the frame, PAUSED = 1); K == 3 and
// buffer 2 still held -> detached, SetAnim(0, 0), the buffer overwritten with 1s and freed, the third party slot released at init reloaded.
// (4) Every frame: position (P.x, 0, P.z) (0 before 0x180), rotation y = 0x400, anim speed dt << 6, the pose step, shadow bit 0x200 cleared,
// and the draw (nothing while the hidden bit is set; under the fog flag each plain face takes the depth cue of its last corner). Ops: 0x181 PIT = 1 (its operands are read by nothing),
// 0x182 clears an unread flag, 0x183 the loader for file NEXT ('main' sends it twice: files 1 and 2), 0x184 hides 'sara' and PIT = 0.
// Exit: stock ground on, the buffers freed. The PS1's dt = 0 stall also held every task and emitter that ran after the overlay in that
// frame (a CD-speed artefact); the host reports a stream after its disc read and a missing file as loaded with no clip, so the wait branch
// does not stall the summon.
class Saraman : public GroundFx {
 public:
  ~Saraman() override {
    if (ctx_.models) ctx_.models->pack_drive.active = false;
  }

 protected:
  void init() override {
    seed_rng();
    load_meshes();
    set_skip(false);
    for (int k = 0; k < 4; ++k) bake_mesh(mesh(std::string("hah") + static_cast<char>('0' + k)), 1, 0, 0);
    for (int i = 0; i < 18; ++i) {
      char piece_name[4] = {'m', static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), 0};
      pieces_[static_cast<size_t>(i)].mesh = mesh(piece_name);
    }
    if (FxModels* ms = ctx_.models) {
      FxModels::PackDrive& drive = ms->pack_drive;
      const bool host = drive.host;
      drive = {};
      drive.host = host;
      drive.active = true;
      drive.name = "sara";
      drive.clip = 0, drive.clip_flags = 0;
      drive.hidden = true;
      drive.speed = 64;
      for (FxModel& unit : ms->units) flag_clear(&unit, 0x400);
      ms->suppress_main = true;
    }
    if (ctx_.sequences) ctx_.sequences->start("init");
    hold_ = true;
    third_slot_.release(ctx_.models);
  }
  bool command(int op, const int* args, int arg_count) override {
    switch (op) {
      case 0x181:  // pit on
        pit_ = true;
        return true;
      case 0x182:  // no effect
        return true;
      case 0x183:  // the loader for file NEXT
        request_file();
        return true;
      case 0x184:  // hide 'sara', pit off
        if (ctx_.models) ctx_.models->pack_drive.hidden = true;
        pit_ = false;
        return true;
      case 0x180: {
        const int cx = static_cast<s16>(arg(args, arg_count, 8)), cz = static_cast<s16>(arg(args, arg_count, 10));
        rect_ = {s16c(s16c(cx * 0x400) - 0x800), s16c(s16c(cz * 0x400) + 0x800), s16c(s16c(cx * 0x400) + 0x800), s16c(s16c(cz * 0x400) - 0x800)};
        px_ = cx << 10;
        pz_ = cz << 10;
        for (Piece& piece : pieces_) {
          if (!piece.mesh) continue;
          piece.centre = piece.mesh->centroid();
          piece.rot = {0, 0, 0};
          piece.thr = ps1_square_root0(piece.centre[0] * piece.centre[0] + piece.centre[1] * piece.centre[1] + piece.centre[2] * piece.centre[2]) >> 4;
          piece.vy = rand() % 20 + 10;
        }
        bake_mesh(mesh("base"), 1, px_, pz_);
        for (Piece& piece : pieces_) bake_mesh(piece.mesh, 1, px_, pz_);
        positioned_ = true;
        return true;
      }
      default: return false;
    }
  }
  void request_file() {
    if (next_ < 0 || next_ >= 3) return;
    held_[static_cast<size_t>(next_)] = true;
    if (ctx_.models) ctx_.models->pack_drive.motion_request = next_;
    loading_ = true;
  }
  // The host reports the stream after the tick it was asked in; without a host there is nothing to wait for.
  bool file_ready(int k) const {
    if (k < 0 || k >= 3) return false;
    if (!ctx_.models) return true;
    return ctx_.models->pack_drive.motion_state[static_cast<size_t>(k)] != 0;
  }
  // The 'sara' creature's part of the frame.
  void creature_frame(int& dt) {
    FxModels* ms = ctx_.models;
    FxModels::PackDrive* drive = ms ? &ms->pack_drive : nullptr;
    // (1)
    if (!bank_ && !loading_ && tick_ > 0x50) {
      bank_ = true;
      request_file();
    }
    // (2)
    if (loading_ && file_ready(next_)) {
      if (next_ == 0) {
        if (ctx_.sequences) {
          const FxModel* c0 = caster(0);
          const FxModel* t0 = target(0);
          ctx_.sequences->start_main(c0 && t0 && c0 == t0);
        }
        hold_ = false;
        if (drive) drive->hidden = false;
        first_ = true;
        pit_ = true;
      }
      loading_ = false;
      ready_[static_cast<size_t>(next_)] = true;
      ++next_;
    }
    const bool anim_over = drive ? drive->at_end : true;
    // (3)
    if (anim_over && first_) {
      if (attached_ < 3) {
        if (!ready_[static_cast<size_t>(attached_)]) {
          dt = 0;
          paused_ = true;
        } else {
          paused_ = false;
          if (drive) {
            drive->motion = attached_;
            drive->clip = 0x80, drive->clip_flags = 1;
            drive->hidden = false;
          }
          if (attached_ > 0) held_[static_cast<size_t>(attached_ - 1)] = false;
          ++attached_;
        }
      } else if (held_[2]) {
        if (drive) {
          drive->motion = -1;
          drive->clip = 0, drive->clip_flags = 0;
        }
        held_[2] = false;
        add_task([this] { return third_slot_.reload_tick(); });
      }
    }
    // (4)
    if (drive) {
      drive->pos = {static_cast<float>(px_), 0.0f, static_cast<float>(pz_)};
      drive->rot[0] = 0, drive->rot[1] = 0x400, drive->rot[2] = 0;
      drive->speed = dt << 6;
    }
  }
  void frame() override {
    int dt = 1;
    tick_ += dt;
    creature_frame(dt);
    if (!pit_) {
      set_skip(false);
      return;
    }
    set_skip(true);
    draw_ground(rect_[0], rect_[1], rect_[2], rect_[3]);
    // Before op 0x180 the PS1 would draw the unbaked meshes at the origin; not drawn here.
    if (!positioned_) return;
    const Vec3 pos{static_cast<float>(px_), 0.0f, static_cast<float>(pz_)};
    if (const PackMesh* base_mesh = mesh("base")) {
      MeshDraw draw;
      draw.offset = pos;
      draw.bias = 100 + 200;
      draw.depth_cue = true;
      draw_mesh(*base_mesh, draw);
    }
    for (Piece& piece : pieces_) {
      if (!piece.mesh) continue;
      Vec3 piece_pos = pos;
      int bias = 200;
      if (piece.thr < tick_) {
        piece_pos[1] = static_cast<float>(piece.vy * (tick_ - piece.thr));
        piece.rot[0] = static_cast<u16>(piece.rot[0] + 0x1E);
        piece.rot[1] = static_cast<u16>(piece.rot[1] + 0x50);
        piece.rot[2] = static_cast<u16>(piece.rot[2] + 0x46);
        bias = 100 + 200;
      }
      MeshDraw draw;
      draw.centre = piece.centre;
      for (int k = 0; k < 3; ++k) draw.rot[k] = piece.rot[static_cast<size_t>(k)];
      draw.offset = piece_pos;
      draw.bias = bias;
      draw.depth_cue = true;
      // The PS1 guards this draw with an uninitialised stack word; always drawn here.
      draw_mesh(*piece.mesh, draw);
    }
  }

  // The posed 'sara' (the stock builders through the shared draw; byte 3 = F as is, the pack model's textures).
  void draw(std::vector<FxPrim>& out) const override {
    const FxModels* ms = ctx_.models;
    if (!ms || !ms->pack_drive.active || ms->pack_drive.hidden || !ms->skin_pack) return;
    FxSkinnedModel sk;
    if (!ms->skin_pack(sk)) return;
    FxModel model;
    for (int k = 0; k < 3; ++k) model.rgb[k] = sk.rgb[k];
    model.flags = 0;
    ModelDrawSpec spec;
    spec.mask_part_flags = false;
    spec.pack_model = true;
    spec.cue = &ms->depth_cue;
    spec.quad_avsz3 = false;
    u32 order = 0;
    model_draw(sk, model, spec, ctx_.view, out, order);
  }
  bool done() const override { return !hold_ && GroundFx::done(); }

 private:
  struct Piece {
    PackMesh* mesh = nullptr;
    std::array<int, 3> centre{}, rot{};
    int thr = 0, vy = 0;
  };
  std::array<Piece, 18> pieces_{};
  std::array<s16, 4> rect_{};
  int px_ = 0, pz_ = 0, tick_ = 0;
  bool pit_ = false, positioned_ = false;
  // Loader state: BANK, LOADING, NEXT, K (attached count), FIRST, PAUSED, READY[3], the held buffers, and the hold on the effect.
  bool bank_ = false, loading_ = false, first_ = false, paused_ = false, hold_ = false;
  int next_ = 0, attached_ = 0;
  std::array<bool, 3> ready_{}, held_{};
  ThirdSlotRelease third_slot_;
};

}  // namespace

void register_ground_ports(BespokeRegistry& registry) {
  registry.add({"wipeoff", "ground", "Wipeoff", [] { return std::make_unique<Wipeoff>(); }});
  registry.add({"tekken", "ground", "Tekken", [] { return std::make_unique<Tekken>(); }});
  registry.add({"jitanda", "ground", "Jitanda", [] { return std::make_unique<Jitanda>(); }});
  registry.add({"2boll", "ground", "Boll2", [] { return std::make_unique<Boll2>(); }});
  registry.add({"saraman", "ground", "Saraman", [] { return std::make_unique<Saraman>(); }});
  register_ground_ports_b(registry);
  register_ground_ports_c(registry);
}

}  // namespace oc::effects
