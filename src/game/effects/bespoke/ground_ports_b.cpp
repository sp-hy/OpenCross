// Ground overlays of the battle stage, second part: 42hevi (Gravitonne), 11boru (Volcano) and 25upli (Uplift). They share the
// GroundFx base (ground_base.h) with the other ground overlays.
#include <algorithm>
#include <array>
#include <memory>
#include <string>

#include "game/effects/bespoke/ground_base.h"
#include "game/effects/fx_math.h"

namespace oc::effects {

namespace {

using namespace ground_detail;

// ---- 42hevi (Gravitonne) -------------------------------------------------------------------------------------------------------------------
// Init: 'suri' and 'sur2' baked mode 2 at (0, 0) (sur2 = the colour source, never drawn); stock ground off; 'yuka' fired (its mesh object
// draws 'suri': drawn here, colour = the stage ring's colour every frame); the 'hole' op-0x48 callback: the target model
// at the particle (pos >> 4, y scale from the particle, shadow off) and a cone dent in 'suri': per vertex d = SquareRoot0(dx^2 + dz^2) from the
// particle, t = R - d with R = min(0x800, model size / 6), if t > 0: y = max(y, py t / R). Frame: the ground with the rect {-0x1400,
// 0x1400, 0x1400, -0x1400} (every cell skipped: 'suri' replaces the floor), suri colours = sur2 colours - (y >> 2) per corner (floor 0).
// Op 0x103 (D = s16 @+4) per target a task: pos = cur + (home - cur) t / D, y scale s0 + (0x1000 - s0) t / D, shadow on; and
// every suri vertex y = (y * 0x6E) / 128 (rounded toward zero) per task per tick; past D: home, 0x1000, end. Exit: stock ground on, models home.
class Hevi42 : public GroundFx {
 public:
  ~Hevi42() override {
    for (int i = 0; i < target_count(); ++i)
      if (FxModel* model = target(i)) {
        model->scale[1] = 0x1000;
        model->pos = model->home;
        flag_set(model, 0x200);
      }
  }

 protected:
  void init() override {
    load_meshes();
    suri_ = mesh("suri");
    sur2_ = mesh("sur2");
    bake_mesh(suri_, 2, 0, 0);
    bake_mesh(sur2_, 2, 0, 0);
    set_skip(true);
    own_emitter_meshes({"suri"});
    if (!em()) return;
    em()->fire("yuka");
    // The op-0x48 record's callback REPLACES the object's draw function and never calls the stock draw (unlike Ice Lance's), so the 'hole'
    // sprite (4096 wide, under the floor) is never drawn. It is a draw-replacement, not a floor draw-order matter: no OT calibration is
    // involved. The flag is set before any per-target clone (`sado`) copies the header.
    em()->set_draw_dropped("hole", true);
    em()->set_object_hook("hole", [this](EmitterSystem::ObjectRef& object) {
      // The model is the one the particle's own emitter names - the op-0x39 entry of its header (bytes +2 / +3 = selector, joint); the
      // sequence starts one 'hole' header per target with selector 0xD + k (k >= 1; 2 / 0xD = target 0, battle_fx.cpp actor_basis). The
      // sequence cursor is NOT the particle's target: reading it would move every target onto the last particle.
      std::array<int, 4> sel{};
      int idx = 0;
      if (object.owner >= 0 && em() && em()->op_value(static_cast<size_t>(object.owner), 0x39, sel) && sel[0] >= 0xE && sel[0] <= 0x15) idx = sel[0] - 0xD;
      FxModel* model = target(idx);
      const int px = object.pos[0] >> 16, py = object.pos[1] >> 16, pz = object.pos[2] >> 16;
      int radius = 0x800;
      if (model) {
        flag_clear(model, 0x200);
        model->pos = {static_cast<float>(object.pos[0]) / 65536.0f, static_cast<float>(object.pos[1]) / 65536.0f, static_cast<float>(object.pos[2]) / 65536.0f};
        model->scale[1] = object.scale[1];
        model->steer = true;
        // The selector's model size = the load-time x/z extent scale (f(SquareRoot0(ex^2 + ez^2) / 0x23C), 4.12,
        // FxModel::fx_scale_h) / 6, capped at 0x800.
        radius = std::min(0x800, model->fx_scale_h / 6);
      }
      if (!suri_ || radius <= 0) return;
      for (auto& vertex : suri_->verts) {
        const int dx = vertex[0] - px, dz = vertex[2] - pz;
        const int depth = radius - ps1_square_root0(dx * dx + dz * dz);
        if (depth > 0) {
          const int dent = trunc_div(py * depth, radius);
          if (vertex[1] < dent) vertex[1] = s16c(dent);
        }
      }
    }, true);
  }
  bool command(int op, const int* args, int arg_count) override {
    if (op != 0x103) return false;
    for (int i = 0; i < target_count(); ++i) {
      auto task = std::make_shared<Task>();
      task->model = target(i);
      task->duration = arg(args, arg_count, 4);
      task->s0 = task->model->scale[1];
      flag_set(task->model, 0x200);
      task->step(suri_);
      add_task([task, this] { return task->step(suri_); });
    }
    return true;
  }
  void frame() override {
    draw_ground(-0x1400, 0x1400, 0x1400, -0x1400);
    if (suri_ && sur2_ && suri_->prims.size() == sur2_->prims.size())
      for (size_t i = 0; i < suri_->prims.size(); ++i) {
        PackPrim& prim = suri_->prims[i];
        for (int k = 0; k < prim.corners; ++k) {
          const int darken = suri_->verts[prim.idx[static_cast<size_t>(k)]][1] >> 2;
          for (int channel = 0; channel < 3; ++channel) prim.rgb[k][channel] = static_cast<u8>(std::max(0, sur2_->prims[i].rgb[k][channel] - darken));
        }
      }
    const FxGround* stage_ground = fx_ground();
    draw_emitter_meshes(stage_ground ? stage_ground->rgb : nullptr);
    static const bool trace = std::getenv("OC_BESPOKE_TRACE") != nullptr;
    if (trace && suri_ && (frames_++ % 10) == 0) {
      int ymin = 0, ymax = 0, nz = 0, sum = 0;
      for (const auto& vertex : suri_->verts) {
        ymin = std::min<int>(ymin, vertex[1]);
        ymax = std::max<int>(ymax, vertex[1]);
        nz += vertex[1] != 0;
        sum += vertex[1];
      }
      log::debug("bespoke 42hevi suri: nv {} ymin {} ymax {} nonzero {} sum {}", suri_->verts.size(), ymin, ymax, nz, sum);
    }
  }

 private:
  int frames_ = 0;
  struct Task {
    FxModel* model = nullptr;
    int tick = 0, duration = 1, s0 = 0x1000;
    bool live = true;
    bool step(PackMesh* suri) {
      if (!live) return false;
      int sc = s0 + trunc_div((0x1000 - s0) * tick, duration);
      Vec3 pos = model->pos;
      for (size_t k = 0; k < 3; ++k) pos[k] = static_cast<float>(static_cast<int>(pos[k]) + trunc_div((static_cast<int>(model->home[k]) - static_cast<int>(pos[k])) * tick, duration));
      tick += 1;
      if (duration < tick) {
        pos = model->home;
        sc = 0x1000;
        live = false;
      }
      model->scale[1] = sc;
      model->pos = pos;
      model->steer = true;
      if (suri)
        for (auto& vertex : suri->verts) {
          const int y = vertex[1] * 0x6E;
          vertex[1] = s16c((y < 0 ? y + 0x7F : y) >> 7);
        }
      return live;
    }
  };
  PackMesh *suri_ = nullptr, *sur2_ = nullptr;
};

// ---- 11boru (Volcano) ----------------------------------------------------------------------------------------------------------------------
// Init: per target the saved block fields, drift (-x >> 8, -z >> 8) (20.12 per tick: the hole centre is still 0 when it is taken); hole
// centre (ox, yc, oz) = the targets' bounding-box middle, x / z floored to 0x400 and + 0x400; stock ground off; bakes at (ox, oz): m00..m09 mode
// 1 (each with its rolls: rot r % 200 - 100, rate r % 100 - 50, vy -(r % 100) - 50), waku, hahe, hm00 hm01 hm0a hm0b hm1a hm1b mode 1, m10..m19
// and wak1 mode 2; emitters '1'..'6' at (ox, yc, oz), '7' '8' at (ox, -5000, oz). Frame (tick += 1, T = 16 tick; mesh colours = the ground colour):
// phase 0 (128 ticks): piece i = m0i morphed toward m1i by w = 2T, rotated by rot_i T >> 11 about its centroid, at (ox, yc, oz); waku -> wak1
// at (ox, yc, oz) with the last w; the ground with the hole {ox - 0x800, oz + 0x800, ox + 0x800, oz - 0x800}; then SetAnim(target, 6, 0).
// Phase 1 (128 ticks): rot_i += rate_i (from the init roll, so the piece carries on from its phase-0 end), y_i += vy_i, m1i drawn at (ox, y_i, oz) about its centroid; targets rise by vy_i, drift, turn by
// rate_i; wak1 at (ox, yc, oz); the ground with the hole. Phase 2 (127 ticks): targets hang at y = -8400 + 800 i, x / z / rotation restored,
// stock ground on, no meshes; SetAnim(target, 8, 0). Phase 3: the targets fall by 25 d per tick; landing: y = 0, 'k00<i>' at the saved
// position started, 'dead' entry 0x80 variant 7. Exit: the saved block fields back.
class Boru11 : public GroundFx {
 public:
  ~Boru11() override {
    for (Item& it : items_) {
      it.model->pos = it.home;
      for (int k = 0; k < 3; ++k) it.model->rot[k] = it.home_rot[k];
    }
  }

 protected:
  void init() override {
    seed_rng();
    load_meshes();
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < target_count() && i < 10; ++i) {
      FxModel* model = target(i);
      Item it;
      it.model = model;
      it.home = model->pos;
      for (int k = 0; k < 3; ++k) it.home_rot[k] = model->rot[k];
      // (0 - x) >> 8 in 20.12 units: below a world unit per tick.
      it.drift[0] = (-static_cast<int>(model->pos[0])) >> 8;
      it.drift[1] = (-static_cast<int>(model->pos[2])) >> 8;
      for (size_t k = 0; k < 3; ++k) lo[k] = std::min(lo[k], model->pos[k]), hi[k] = std::max(hi[k], model->pos[k]);
      items_.push_back(it);
    }
    if (!items_.empty()) {
      ox_ = ((trunc_div(static_cast<int>(lo[0]) + static_cast<int>(hi[0]), 2) >> 10) * 0x400) + 0x400;
      yc_ = trunc_div(static_cast<int>(lo[1]) + static_cast<int>(hi[1]), 2);
      oz_ = ((trunc_div(static_cast<int>(lo[2]) + static_cast<int>(hi[2]), 2) >> 10) * 0x400) + 0x400;
    }
    set_skip(true);
    for (int i = 0; i < 10; ++i) {
      Piece& piece = pieces_[static_cast<size_t>(i)];
      piece.m0 = mesh(std::string("m0") + static_cast<char>('0' + i));
      if (!piece.m0) continue;
      bake_mesh(piece.m0, 1, ox_, oz_);
      piece.centre = piece.m0->centroid();
      for (int k = 0; k < 3; ++k) piece.rot[k] = rand() % 200 - 100;
      for (int k = 0; k < 3; ++k) piece.rate[k] = rand() % 100 - 0x32;
      piece.vy = -(rand() % 100) - 0x32;
    }
    waku_ = mesh("waku");
    bake_mesh(waku_, 1, ox_, oz_);
    std::set<std::string> em_meshes;
    for (const char* nm : {"hahe", "hm00", "hm01", "hm0a", "hm0b", "hm1a", "hm1b"})
      if (bake_mesh(mesh(nm), 1, ox_, oz_)) em_meshes.insert(nm);
    for (int i = 0; i < 10; ++i) {
      pieces_[static_cast<size_t>(i)].m1 = mesh(std::string("m1") + static_cast<char>('0' + i));
      bake_mesh(pieces_[static_cast<size_t>(i)].m1, 2, ox_, oz_);
    }
    wak1_ = mesh("wak1");
    bake_mesh(wak1_, 2, ox_, oz_);
    own_emitter_meshes(em_meshes);
    if (std::getenv("OC_BESPOKE_TRACE")) {
      log::debug("bespoke 11boru init: centre {} {} {} targets {} ground rgb {} {} {}", ox_, yc_, oz_, items_.size(), fx_ground() ? fx_ground()->rgb[0] : -1,
                 fx_ground() ? fx_ground()->rgb[1] : -1, fx_ground() ? fx_ground()->rgb[2] : -1);
      for (size_t i = 0; i < items_.size(); ++i)
        log::debug("  target {} home {:.0f} {:.0f} {:.0f} drift {} {} rot0 {} {} {} rate {} {} {} vy {}", i, items_[i].home[0], items_[i].home[1], items_[i].home[2],
                   items_[i].drift[0], items_[i].drift[1], pieces_[i].rot[0], pieces_[i].rot[1], pieces_[i].rot[2], pieces_[i].rate[0], pieces_[i].rate[1],
                   pieces_[i].rate[2], pieces_[i].vy);
      auto dump = [](const char* nm, const PackMesh* dumped) {
        if (!dumped) return;
        std::string text;
        for (size_t k = 0; k < std::min<size_t>(6, dumped->prims.size()); ++k) {
          const PackPrim& prim = dumped->prims[k];
          text += " [" + std::to_string(prim.code) + ":";
          for (int corner = 0; corner < prim.corners; ++corner) text += " " + std::to_string(prim.rgb[corner][0]) + "," + std::to_string(prim.rgb[corner][1]) + "," + std::to_string(prim.rgb[corner][2]);
          text += "]";
        }
        log::debug("  bake {} verts {} prims {} first packets{}", nm, dumped->verts.size(), dumped->prims.size(), text);
      };
      dump("m00", pieces_[0].m0);
      dump("m10", pieces_[0].m1);
      dump("waku", waku_);
      dump("wak1", wak1_);
    }
    if (!em()) return;
    for (const char* emitter_name : {"1", "2", "3", "4", "5", "6"}) em()->set_position(emitter_name, ox_, yc_, oz_);
    em()->set_position("7", ox_, static_cast<s16>(0xEC78), oz_);
    em()->set_position("8", ox_, static_cast<s16>(0xEC78), oz_);
  }
  void frame() override {
    ++tick_;
    const int ticks16 = tick_ * 16;
    const FxGround* stage_ground = fx_ground();
    u8 rgb[3] = {128, 128, 128};
    if (stage_ground)
      for (int channel = 0; channel < 3; ++channel) rgb[channel] = stage_ground->rgb[channel];
    auto base_draw = [&](MeshDraw& draw) {
      for (int channel = 0; channel < 3; ++channel) draw.rgb[channel] = rgb[channel];
      draw.bias = 100;
    };
    const Vec3 centre{static_cast<float>(ox_), static_cast<float>(yc_), static_cast<float>(oz_)};
    // Phase 0 draws with the morph builder: the mesh colour as in phase 1, vertices AND packet colours lerped m0 -> m1, the primitives
    // opaque with the group code. m0i holds the ground's greys, m1i (240, 8, 8) where the lava mesh had its own colour and the ground greys
    // elsewhere - the morph is also a grey -> red colour ramp.
    auto morph_draw = [&](MeshDraw& draw) {
      base_draw(draw);
      draw.opaque = true;
    };
    switch (phase_) {
      case 0: {
        const int weight = s16c(ticks16 * 2);
        for (Piece& piece : pieces_) {
          if (!piece.m0) continue;
          MeshDraw draw;
          morph_draw(draw);
          draw.centre = piece.centre;
          for (int k = 0; k < 3; ++k) draw.rot[k] = s16c((piece.rot[k] * ticks16) >> 11);
          draw.offset = centre;
          draw.morph = piece.m1;
          draw.morph_w = weight;
          draw_mesh(*piece.m0, draw);
        }
        if (waku_) {
          MeshDraw draw;
          morph_draw(draw);
          draw.offset = centre;
          draw.morph = wak1_;
          draw.morph_w = weight;
          draw_mesh(*waku_, draw);
        }
        draw_ground(ox_ - 0x800, oz_ + 0x800, ox_ + 0x800, oz_ - 0x800);
        if (ticks16 > 0x7FF) {
          tick_ = 0, phase_ = 1;
          for (Item& it : items_) set_anim(it.model, 6, 0);
        }
        break;
      }
      case 1:
        for (size_t i = 0; i < 10; ++i) {
          Piece& piece = pieces_[i];
          for (int k = 0; k < 3; ++k) piece.rot[k] = s16c(piece.rot[k] + piece.rate[k]);
          piece.yoff += piece.vy;
          if (!piece.m1) continue;
          MeshDraw draw;
          base_draw(draw);
          draw.centre = piece.centre;
          for (int k = 0; k < 3; ++k) draw.rot[k] = piece.rot[k];
          draw.offset = {static_cast<float>(ox_), static_cast<float>(piece.yoff), static_cast<float>(oz_)};
          draw_mesh(*piece.m1, draw);
        }
        for (size_t i = 0; i < items_.size(); ++i) {
          Item& it = items_[i];
          const Piece& piece = pieces_[i];
          it.model->pos[1] += static_cast<float>(piece.vy);
          it.model->pos[0] += static_cast<float>(it.drift[0]) / 4096.0f;
          it.model->pos[2] += static_cast<float>(it.drift[1]) / 4096.0f;
          for (int k = 0; k < 3; ++k) it.model->rot[k] = s16c(it.model->rot[k] + piece.rate[k]);
          it.model->steer = true;
        }
        if (wak1_) {
          MeshDraw draw;
          base_draw(draw);
          draw.offset = centre;
          draw_mesh(*wak1_, draw);
        }
        draw_ground(ox_ - 0x800, oz_ + 0x800, ox_ + 0x800, oz_ - 0x800);
        if (ticks16 > 0x7FF) {
          tick_ = 0, phase_ = 2;
          if (std::getenv("OC_BESPOKE_TRACE"))
            for (size_t i = 0; i < 10; ++i) log::debug("bespoke 11boru phase 1 end: piece {} rot {} {} {}", i, pieces_[i].rot[0], pieces_[i].rot[1], pieces_[i].rot[2]);
        }
        break;
      case 2:
        for (size_t i = 0; i < items_.size(); ++i) {
          Item& it = items_[i];
          it.model->pos = {it.home[0], static_cast<float>(-0x20D0000 / 4096 + static_cast<int>(i) * (0x320000 / 4096)), it.home[2]};
          for (int k = 0; k < 3; ++k) it.model->rot[k] = it.home_rot[k];
          it.model->steer = true;
        }
        set_skip(false);
        if (ticks16 > 0x7E7) {
          tick_ = 0, phase_ = 3;
          for (Item& it : items_) set_anim(it.model, 8, 0);
        }
        break;
      case 3:
        for (size_t i = 0; i < items_.size(); ++i) {
          Item& it = items_[i];
          if (it.landed || it.model->pos[1] >= 0) continue;
          it.model->pos[1] += static_cast<float>(tick_ * 0x19000) / 4096.0f;
          it.model->steer = true;
          if (it.model->pos[1] >= 0) {
            it.model->pos[1] = 0;
            it.landed = true;
            if (em()) {
              const std::string landing_name = std::string("k00") + static_cast<char>('0' + static_cast<int>(i));
              em()->set_position(landing_name, static_cast<int>(it.home[0]), static_cast<int>(it.home[1]), static_cast<int>(it.home[2]));
              em()->start(landing_name);
            }
            run_script(it.model, "dead", 0x80, 7);
          }
        }
        break;
      default: break;
    }
    draw_emitter_meshes();
  }
  bool done() const override { return phase_ >= 3; }

 private:
  struct Item {
    FxModel* model = nullptr;
    Vec3 home{0, 0, 0};
    int home_rot[3] = {0, 0, 0};
    int drift[2] = {0, 0};
    bool landed = false;
  };
  struct Piece {
    PackMesh *m0 = nullptr, *m1 = nullptr;
    std::array<int, 3> centre{};
    int rot[3] = {0, 0, 0}, rate[3] = {0, 0, 0};
    int vy = 0, yoff = 0;
  };
  std::vector<Item> items_;
  std::array<Piece, 10> pieces_{};
  PackMesh *waku_ = nullptr, *wak1_ = nullptr;
  int ox_ = 0, yc_ = 0, oz_ = 0, tick_ = 0, phase_ = 0;
};

// ---- 25upli (Uplift) -----------------------------------------------------------------------------------------------------------------------
// Init: ox = 0x400, oz = floor_0x400(((rcos(a) - rsin(a)) * -800 >> 12) + caster z) (a = caster yaw), the static origin (0x400, 0, oz); stock
// ground off; 81zu..85zu baked mode 2 and hah3 mode 1 at (0x400, oz); the hole = the one cell {0, oz + 0x400, 0x400, oz}; '1500' '1600' at
// the block. Ops 0x180 step the state 0 -> 6 (1: vy = -0xC8000, acc 0x7000; 2: vy = 0xA000; 3: fly to target 0's bone 0 (+0x200, -0x200) in
// 40 ticks, arc of 1500 from 0xB00 by 0x39; 4: the arc's height kept, fall 0x190000 + 600; 5: four pieces apart with spin rolls (r % 20 - 10,
// r % 30 - 15, r % 30 - 15) and 'hkem' 'brak' 'ligt' started). Frame: piece colours = the ground colour; pieces drawn per state (0: at the
// block, bias 100; 1: shaking rotation r % 0x28 x3 about (-0x200, 0x400, 0x200); 2: lifting (bias 100); 3 hover / 4 arc / 5 fall (bias 0);
// 6: each piece flies, spins rate * t about its centroid, lands in a 'kem0' burst and is hidden); 85zu (the pit) drawn two-sided at the
// static origin with bias 0x8C in states 2-6; 'kem3' 'kona' 'sg01' 'boro' at (ox - 0x200, oy + 0x400, oz + 0x200) (the stored oy); the
// ground with the hole every frame. Exit: stock ground on.
class Upli25 : public GroundFx {
 protected:
  void init() override {
    seed_rng();
    load_meshes();
    if (const FxModel* caster_model = caster(0)) {
      const int yaw = caster_model->rot[1];
      const int block_z = (((rcos(yaw) - rsin(yaw)) * -800) >> 12) + static_cast<int>(caster_model->pos[2]);
      oz_ = (static_cast<s16>(block_z) >> 10) << 10;
      ox_ = 0x400;
    }
    oy_ = 0;
    oz0_ = oz_;
    set_skip(true);
    for (int i = 0; i < 4; ++i) {
      pieces_[static_cast<size_t>(i)] = mesh(std::string(1, '8') + static_cast<char>('1' + i) + "zu");
      if (bake_mesh(pieces_[static_cast<size_t>(i)], 2, 0x400, oz0_)) {
        const auto centroid = pieces_[static_cast<size_t>(i)]->centroid();
        for (size_t k = 0; k < 3; ++k) centres_[i][k] = centroid[k];
      }
    }
    pit_ = mesh("85zu");
    bake_mesh(pit_, 2, 0x400, oz0_);
    if (bake_mesh(mesh("hah3"), 1, 0x400, oz0_)) own_emitter_meshes({"hah3"});
    if (em()) {
      em()->set_position("1500", ox_, oy_, oz_);
      em()->set_position("1600", ox_, oy_, oz_);
    }
  }
  bool command(int op, const int* args, int arg_count) override {
    (void)args;
    (void)arg_count;
    if (op != 0x180) return false;
    advance();
    return true;
  }
  void frame() override {
    ++timer_;
    const FxGround* stage_ground = fx_ground();
    u8 rgb[3] = {128, 128, 128};
    if (stage_ground)
      for (int channel = 0; channel < 3; ++channel) rgb[channel] = stage_ground->rgb[channel];
    const std::array<int, 3> kShake{-0x200, 0x400, 0x200};
    auto piece_draw = [&](int i, const std::array<int, 3>& centre, const int rot[3], const Vec3& offset, int bias) {
      if (!pieces_[static_cast<size_t>(i)] || hidden_[i]) return;
      MeshDraw draw;
      draw.centre = centre;
      for (int k = 0; k < 3; ++k) draw.rot[k] = rot[k];
      draw.offset = offset;
      draw.bias = bias;
      for (int k = 0; k < 3; ++k) draw.rgb[k] = rgb[k];
      draw_mesh(*pieces_[static_cast<size_t>(i)], draw);
    };
    const int zero[3] = {0, 0, 0};
    auto block = [&]() { return Vec3{static_cast<float>(ox_), static_cast<float>(oy_), static_cast<float>(oz_)}; };
    switch (state_) {
      case 0:
        // Centre = offset and no rotation: a pure translation.
        for (int i = 0; i < 4; ++i) piece_draw(i, {ox_, oy_, oz_}, zero, block(), 100);
        break;
      case 1: {
        const int rot[3] = {rand() % 0x28, rand() % 0x28, rand() % 0x28};
        for (int i = 0; i < 4; ++i) piece_draw(i, kShake, rot, block(), 100);
        break;
      }
      case 2:
        if (vy_ < 0) vy_ += acc_;
        oy_ = s16c(oy_ + (vy_ >> 12));
        for (int i = 0; i < 4; ++i) piece_draw(i, kShake, zero, block(), 100);
        break;
      case 3:
        if (timer_ > 0x28) timer_ = 0, vy_ = -vy_;
        oy_ = s16c(oy_ + (vy_ >> 12));
        for (int i = 0; i < 4; ++i) piece_draw(i, kShake, zero, block(), 0);
        break;
      case 4: {
        ang_ += angv_;
        ox_ = s16c(ox_ + (vx_ >> 12));
        oz_ = s16c(oz_ + (vz_ >> 12));
        const Vec3 arc_pos{static_cast<float>(ox_), static_cast<float>(oy_ + ((rsin(ang_) * -0x5DC) >> 12)), static_cast<float>(oz_)};
        for (int i = 0; i < 4; ++i) piece_draw(i, kShake, zero, arc_pos, 0);
        break;
      }
      case 5:
        oy_ = s16c(oy_ + (ang_ >> 12));
        if (static_cast<s16>(target_y_) < static_cast<s16>(oy_)) advance();
        else ang_ += angv_;
        if (state_ == 5)
          for (int i = 0; i < 4; ++i) piece_draw(i, kShake, zero, block(), 0);
        else
          for (int i = 0; i < 4; ++i) {
            const int rot[3] = {0, 0, 0};
            piece_draw(i, centres_[i], rot, {static_cast<float>(pos_[i][0]), static_cast<float>(pos_[i][1]), static_cast<float>(pos_[i][2])}, 0);
          }
        break;
      case 6:
        for (int i = 0; i < 4; ++i) {
          for (int k = 0; k < 3; ++k) pos_[i][k] = s16c(pos_[i][k] + (vel_[i][k] >> 12));
          if (pos_[i][1] > 0) {
            if (em()) {
              pos_[i][1] = 0;
              em()->set_position("kem0", pos_[i][0], 0, pos_[i][2]);
              em()->fire("kem0");
            }
            hidden_[i] = true;
          }
          vel_[i][1] += 0x3000;
          const int rot[3] = {s16c(spin_[i][0] * timer_), s16c(spin_[i][1] * timer_), s16c(spin_[i][2] * timer_)};
          piece_draw(i, centres_[i], rot, {static_cast<float>(pos_[i][0]), static_cast<float>(pos_[i][1]), static_cast<float>(pos_[i][2])}, 0);
        }
        break;
      default: break;
    }
    // The pit: two-sided, opaque, in the ground colour, at the static origin.
    if (state_ >= 2 && pit_) {
      MeshDraw draw;
      draw.offset = {static_cast<float>(0x400), 0.0f, static_cast<float>(oz0_)};
      draw.bias = 0x8C;
      draw.two_sided = true;
      draw.opaque = true;
      for (int k = 0; k < 3; ++k) draw.rgb[k] = rgb[k];
      draw_mesh(*pit_, draw);
    }
    if (em())
      for (const char* emitter_name : {"kem3", "kona", "sg01", "boro"}) em()->set_position(emitter_name, ox_ - 0x200, oy_ + 0x400, oz_ + 0x200);
    draw_ground(0, oz0_ + 0x400, 0x400, oz0_);
    draw_emitter_meshes();
  }
  bool done() const override { return state_ >= 6; }

 private:
  void advance() {
    switch (state_) {
      case 1:
        vy_ = -0xC8000;
        acc_ = 0x7000;
        vx_ = vz_ = 0;
        break;
      case 2:
        vy_ = 0xA000;
        vx_ = vz_ = 0;
        break;
      case 3:
        if (const FxModel* target_model = target(0)) {
          // Bone 0's world matrix translation (the root joint of the posed model), stored as bone - (0x200, 0x400, -0x200); here the
          // host's posed joint 0 through the model placement (joint_world). Without a posed model: the waist (half the height) stands in.
          Vec3 bone{target_model->pos[0], target_model->pos[1] - target_model->height * 0.5f, target_model->pos[2]};
          const bool posed = joint_world(target_model, 0, bone);
          const int tx = static_cast<int>(bone[0]), ty = static_cast<int>(bone[1]), tz = static_cast<int>(bone[2]);
          if (std::getenv("OC_BESPOKE_TRACE"))
            log::debug("bespoke 25upli state 3: target 0 pos {:.0f} {:.0f} {:.0f} rot {} bone0 {} {} {} ({})", target_model->pos[0], target_model->pos[1], target_model->pos[2], target_model->rot[1], tx, ty,
                       tz, posed ? "posed joint 0" : "waist stand-in");
          target_y_ = ty - 0x400;
          vx_ = trunc_div((ox_ - tx - 0x200) * -0x1000, 0x28);
          vz_ = trunc_div((oz_ - tz + 0x200) * -0x1000, 0x28);
          angv_ = 0x39;
          ang_ = 0xB00;
          oy_ = s16c(oy_ + ((rsin(0xB00) * 0x5DC) >> 12));
        }
        break;
      case 4:
        oy_ = s16c(oy_ + ((rsin(ang_) * -0x5DC) >> 12));
        ang_ = 0x190000;
        angv_ = 600;
        break;
      case 5:
        for (int i = 0; i < 4; ++i) {
          pos_[i][0] = ox_, pos_[i][1] = oy_, pos_[i][2] = oz_;
          vel_[i][0] = (centres_[i][0] + 0x200) * 0x80;
          vel_[i][1] = 0;
          vel_[i][2] = (centres_[i][2] - 0x200) * 0x80;
          spin_[i][0] = rand() % 20 - 10;
          spin_[i][1] = rand() % 30 - 15;
          spin_[i][2] = rand() % 30 - 15;
        }
        if (em())
          for (const char* emitter_name : {"hkem", "brak", "ligt"}) em()->start(emitter_name);
        break;
      default: break;
    }
    ++state_;
    timer_ = 0;
    if (std::getenv("OC_BESPOKE_TRACE"))
      log::debug("bespoke 25upli -> state {} (tick {}): block {} {} {} vy {} ang {} target {} {} {} oz0 {}", state_, ticks_, ox_, oy_, oz_, vy_, ang_,
                 target_y_, vx_, vz_, oz0_);
  }
  std::array<PackMesh*, 4> pieces_{};
  PackMesh* pit_ = nullptr;
  bool hidden_[4] = {false, false, false, false};
  int state_ = 0, timer_ = 0, ox_ = 0, oy_ = 0, oz_ = 0, oz0_ = 0;
  int vy_ = 0, acc_ = 0, vx_ = 0, vz_ = 0, ang_ = 0, angv_ = 0, target_y_ = 0;
  std::array<int, 3> centres_[4] = {};
  int pos_[4][3] = {}, vel_[4][3] = {}, spin_[4][3] = {};
};

}  // namespace

void register_ground_ports_b(BespokeRegistry& registry) {
  registry.add({"42hevi", "ground", "Hevi42", [] { return std::make_unique<Hevi42>(); }});
  registry.add({"11boru", "ground", "Boru11", [] { return std::make_unique<Boru11>(); }});
  registry.add({"25upli", "ground", "Upli25", [] { return std::make_unique<Upli25>(); }});
}

}  // namespace oc::effects
