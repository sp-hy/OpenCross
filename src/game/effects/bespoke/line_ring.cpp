// Line rings: malchera (Cat's Cradle, eid 10), spark (String Phone, eid 25), monfish (Hook & Sinker, eid 225),
// oomono2 (Big Catch, eid 412). Ports of the PS1 code: one 60-point chain per slot (12 slots, group * 6 + i), hung from a caster attach point,
// drawn as 59 grey LineG2 packets. The four share the code but for the mode-1 motion, the colour constants and the operand use.
//   0x180 (life @+4, group @+8, count @+0xA (spark: the number of targets), joint @+0xC): P = attach point slot[group].joint of caster 0
//         (the STALE joint of slot index `group`, not group * 6); then for every slot of the group: timer = rest = g = mode = 0, life, joint,
//         all 60 points at P, velocities 0.
//   0x181 (frames F @+4, group @+8, offset @+0xA, rest @+0xC, mode @+0xE, joint
//         @+0x10): per slot g = offset - rand % 20, joint, mode; rest := @+0xC (monfish / oomono2 only when > 0). monfish / oomono2: tcount = 0,
//         frames = F; mode 1: dlt = (T - pt0) / F with T = target 0's attach 0x10 (oomono2: caster[-1]'s), monfish's slot 0 rest = (SquareRoot0(
//         |T - caster's joint|^2) + 400) / 60. spark: t = 0; mode != 0: the flight from S = caster attach 7 to T = target[slot] attach 0x10, v =
//         (T - S) 0x1000 / F, rest = |T - S| / 0x78.
//   frame: per slot timer += 1 (life < timer: the group's count := 0 after this slot), A = caster 0's attach `joint`,
//         mode 0 = HANG (n = 58..0: pos.y += g plus the velocity, PIN_NEXT to fixed length rest; monfish / oomono2 keep vel = (new - old) >> 1;
//         then pt59 = A), mode 1 per overlay (below), then DRAW.
//   DRAW: acc = (rand % 10) * 10 + BASE, 59 LineG2 (code 0x52, ABR 1 additive with dither), colours (acc >> 4)
//         -> (acc1 >> 4) grey, acc1 = acc -+ STEP flipping every 13 lines; OT entry ((SZa >> 2) + (SZb >> 2)) / 2 >> shift. BASE / STEP = 0x500 /
//         0x40 (malchera spark), 0x300 / 0x15 (monfish oomono2).
// spark's impact helper (gated by a battle script word, which fires 'hit1' / 't000') is `impact()`. Not reproduced: oomono2's loader
// (the 'act0' model from LTD_DATA / OBJ_DATA) - its caster[-1] (the pseudo-actor of op 0x3F) is read as emitter slot A's position.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "game/effects/bespoke/mesh_tech.h"
#include "game/effects/fx_math.h"

namespace oc::effects {
namespace {

enum class Ring { Malchera, Spark, Monfish, Oomono2 };
struct RingParams {
  const char* name;
  Ring kind;
  int base, step;
};
constexpr int kPts = 60;

int trunc_div(int numerator, int denominator) { return denominator == 0 ? 0 : numerator / denominator; }

class LineRing : public MeshTech {
 public:
  explicit LineRing(const RingParams& params) : params_(params) {}

 protected:
  bool rope() const { return params_.kind == Ring::Monfish || params_.kind == Ring::Oomono2; }
  void init() override {
    rng_.state = ctx_.seed ? ctx_.seed : 1;
    if (params_.kind == Ring::Oomono2) {
      // The loader tail runs the pack's `main` atk script on every acting model, which also lands the hit (its React), then starts 'line'.
      for (int i = 0; ctx_.models && i < static_cast<int>(ctx_.models->casters.size()); ++i) start_caster_anim(caster(i));
      // 'line' waits for the first frame: the module's sequence player is live then.
      pending_line_ = true;
    } else {
      start_caster_anim(caster(0));
    }
  }
  bool command(int op, const int* args, int arg_count) override {
    if (std::getenv("OC_LINE_DBG")) std::fprintf(stderr, "LINEOP %X a4=%d a8=%d aA=%d aC=%d aE=%d a10=%d\n", op, arg(args, arg_count, 4), arg(args, arg_count, 8), arg(args, arg_count, 0xA), arg(args, arg_count, 0xC), arg(args, arg_count, 0xE), arg(args, arg_count, 0x10));
    if (op == 0x180) {
      const int group = std::clamp(arg(args, arg_count, 8), 0, 1);
      count_[group] = params_.kind == Ring::Spark ? target_count() : arg(args, arg_count, 0xA);
      count_[group] = std::clamp(count_[group], 0, 6);
      nslots_ = count_[group];
      int origin[3] = {0, 0, 0};
      // The stale joint of slot index `group`.
      attach(caster(0), slots_[static_cast<size_t>(group)].joint, origin);
      for (int i = 0; i < count_[group]; ++i) {
        Slot& slot = slots_[static_cast<size_t>(group * 6 + i)];
        slot.timer = slot.rest = slot.gravity = slot.mode = 0;
        slot.life = arg(args, arg_count, 4);
        slot.joint = arg(args, arg_count, 0xC);
        for (Pt& point : slot.pt) point = {{origin[0], origin[1], origin[2]}, {0, 0, 0}};
      }
    } else if (op == 0x181) {
      const int group = std::clamp(arg(args, arg_count, 8), 0, 1), duration = arg(args, arg_count, 4), rest = arg(args, arg_count, 0xC), mode = arg(args, arg_count, 0xE), joint = arg(args, arg_count, 0x10);
      for (int i = 0; i < count_[group]; ++i) {
        Slot& slot = slots_[static_cast<size_t>(group * 6 + i)];
        slot.gravity = arg(args, arg_count, 0xA) - rng_.next() % 20;
        slot.joint = joint;
        slot.mode = mode;
        if (!rope() || rest > 0) slot.rest = rest;
        if (params_.kind == Ring::Spark) {
          slot.flight_step = 0;
          if (mode != 0) flight_setup(slot, group * 6 + i, duration);
        }
      }
      if (rope()) {
        tcount_ = 0, frames_ = duration;
        if (mode == 1) {
          int far_point[3] = {0, 0, 0};
          far_end(far_point);
          Slot& first_slot = slots_[static_cast<size_t>(group * 6)];
          for (int k = 0; k < 3; ++k) dlt_[k] = trunc_div(far_point[k] - first_slot.pt[0].pos[static_cast<size_t>(k)], duration);
          if (params_.kind == Ring::Monfish) {
            int joint_pos[3] = {0, 0, 0};
            attach(caster(0), joint, joint_pos);
            const int dx = far_point[0] - joint_pos[0], dy = far_point[1] - joint_pos[1], dz = far_point[2] - joint_pos[2];
            first_slot.rest = (ps1_square_root0(dx * dx + dy * dy + dz * dz) + 400) / 0x3C;
          }
        }
      }
    } else {
      return false;
    }
    return true;
  }
  void frame() override {
    // Starts 'line' (and 'mis2' on a miss); without a sequence player there is nothing to start.
    if (pending_line_) {
      pending_line_ = false;
      if (ctx_.sequences) {
        ctx_.sequences->start("line");
        const FxModel* actor = caster(0);
        if (actor && actor->hit == 2) ctx_.sequences->start("mis2");
      }
    }
    lines_.clear();
    for (int group = 0; group < 2; ++group)
      for (int i = 0; i < count_[group]; ++i) {
        const int si = group * 6 + i;
        Slot& slot = slots_[static_cast<size_t>(si)];
        ++slot.timer;
        // The later slots of the group never run again.
        if (slot.life < slot.timer) count_[group] = 0;
        int anchor[3] = {0, 0, 0};
        attach(caster(0), slot.joint, anchor);
        if (slot.mode == 0) {
          hang(slot, anchor);
        } else if (slot.mode == 1) {
          if (params_.kind == Ring::Malchera) malchera_mode1(slot);
          else if (params_.kind == Ring::Spark) spark_mode1(slot, si, anchor);
          else rope_mode1(slot, anchor);
        }
        record_draw(slot);
      }
  }
  void draw(std::vector<FxPrim>& out) const override {
    const size_t n0 = out.size();
    u32 order = 0;
    for (const Line& line : lines_) {
      FxPrim prim;
      prim.kind = FxPrim::Kind::Line;
      float za, zb;
      if (!fx_project(ctx_.view, {static_cast<float>(line.from[0]), static_cast<float>(line.from[1]), static_cast<float>(line.from[2])}, prim.v[0].x, prim.v[0].y, za)) continue;
      if (!fx_project(ctx_.view, {static_cast<float>(line.to[0]), static_cast<float>(line.to[1]), static_cast<float>(line.to[2])}, prim.v[1].x, prim.v[1].y, zb)) continue;
      prim.v[0].r = prim.v[0].g = prim.v[0].b = line.ca;
      prim.v[1].r = prim.v[1].g = prim.v[1].b = line.cb;
      // DR_MODE 0xE1000220: ABR 1 (B + F).
      prim.blend = 2;
      prim.slot = std::clamp((((static_cast<int>(za) >> 2) + (static_cast<int>(zb) >> 2)) >> 1) >> 2, 1, 0xFFF);
      prim.order = order++;
      out.push_back(prim);
    }
    last_prims_ = out.size() - n0;
  }
  bool done() const override { return !pending_line_ && count_[0] == 0 && count_[1] == 0; }

 private:
  struct Pt {
    std::array<int, 3> pos{}, vel{};
  };
  struct Slot {
    int joint = 0, timer = 0, life = 0, rest = 0, gravity = 0, mode = 0;
    // spark's flight
    int flight_step = 0, start[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    std::array<Pt, kPts> pt{};
  };
  struct Line {
    int from[3], to[3];
    u8 ca, cb;
  };

  static int s16i(int value) { return static_cast<s16>(value); }
  static int dist2(const Pt& lhs, const Pt& rhs) {
    int sum = 0;
    for (size_t k = 0; k < 3; ++k) {
      const int delta = s16i(lhs.pos[k] - rhs.pos[k]);
      sum += delta * delta;
    }
    return sum;
  }
  // PIN_PREV / PIN_NEXT (monfish): pt[n] put at exactly `rest` from `other` (no clamp: a fixed length).
  static void pin(Pt& pt, const Pt& other, int rest) {
    const int length = ps1_square_root0(dist2(pt, other));
    const int scale = length == 0 ? 0 : (rest << 12) / length;
    for (size_t k = 0; k < 3; ++k) pt.pos[k] = s16i(other.pos[k] + s16i((s16i(pt.pos[k] - other.pos[k]) * scale) >> 12));
  }
  // INTEG: monfish / oomono2 integrate their velocity; malchera / spark run the same code with velocity 0 (y += gravity).
  void integ(Pt& pt, int gravity) const {
    pt.pos[1] = s16i(pt.pos[1] + gravity);
    if (rope()) {
      pt.pos[0] = s16i(pt.pos[0] + s16i(pt.vel[0]));
      pt.pos[2] = s16i(pt.pos[2] + s16i(pt.vel[2]));
      pt.pos[1] = s16i(pt.pos[1] + s16i(pt.vel[1]));
    }
  }
  static void vel_of(Pt& pt, const std::array<int, 3>& old) {
    for (size_t k = 0; k < 3; ++k) pt.vel[k] = s16i(pt.pos[k] - old[k]) >> 1;
  }
  void hang(Slot& slot, const int anchor[3]) const {
    for (int link = kPts - 2; link >= 0; --link) {
      Pt& point = slot.pt[static_cast<size_t>(link)];
      const auto old = point.pos;
      integ(point, slot.gravity);
      pin(point, slot.pt[static_cast<size_t>(link + 1)], slot.rest);
      if (rope()) vel_of(point, old);
    }
    slot.pt[kPts - 1].pos = {anchor[0], anchor[1], anchor[2]};
  }
  // The tail rises by gravity, z jitter (rand % 5 - 2) * 10, the chain follows (y += gravity / 5).
  void malchera_mode1(Slot& slot) {
    const int jitter = (rng_.next() % 5 - 2) * 10;
    Pt& tail = slot.pt[kPts - 1];
    tail.pos[1] = s16i(tail.pos[1] + slot.gravity);
    tail.pos[2] = s16i(tail.pos[2] + jitter);
    for (int link = kPts - 2; link >= 0; --link) {
      Pt& point = slot.pt[static_cast<size_t>(link)];
      point.pos[1] = s16i(point.pos[1] + slot.gravity / 5);
      pin(point, slot.pt[static_cast<size_t>(link + 1)], slot.rest);
    }
  }
  void rope_mode1(Slot& slot, const int anchor[3]) {
    slot.pt[kPts - 1].pos = {anchor[0], anchor[1], anchor[2]};
    ++tcount_;
    if (std::getenv("OC_LINE_DBG")) { int dbg_far[3] = {0,0,0}; far_end(dbg_far); std::fprintf(stderr, "LINEDBG t=%d/%d T=%d %d %d pt0=%d %d %d A=%d %d %d\n", tcount_, frames_, dbg_far[0], dbg_far[1], dbg_far[2], slot.pt[0].pos[0], slot.pt[0].pos[1], slot.pt[0].pos[2], anchor[0], anchor[1], anchor[2]); }
    if (tcount_ < frames_) {
      for (size_t k = 0; k < 3; ++k) slot.pt[0].pos[k] = s16i(slot.pt[0].pos[k] + dlt_[k]);
    } else {
      int far_point[3] = {0, 0, 0};
      far_end(far_point);
      slot.pt[0].pos = {far_point[0], far_point[1], far_point[2]};
      if (std::getenv("OC_LINE_DBG")) std::fprintf(stderr, "LINEDBG t=%d T=%d %d %d A=%d %d %d\n", tcount_, far_point[0], far_point[1], far_point[2], anchor[0], anchor[1], anchor[2]);
    }
    for (int link = 1; link <= kPts - 3; ++link) {
      Pt& point = slot.pt[static_cast<size_t>(link)];
      const auto old = point.pos;
      integ(point, slot.gravity);
      pin(point, slot.pt[static_cast<size_t>(link - 1)], slot.rest);
      vel_of(point, old);
    }
    for (int link = kPts - 2; link >= 1; --link) {
      Pt& point = slot.pt[static_cast<size_t>(link)];
      integ(point, slot.gravity);
      pin(point, slot.pt[static_cast<size_t>(link + 1)], slot.rest);
    }
  }
  void flight_setup(Slot& slot, int si, int duration) {
    int far_point[3] = {0, 0, 0};
    attach(target(si), 0x10, far_point);
    attach(caster(0), 7, slot.start);
    for (int k = 0; k < 3; ++k) slot.vel[k] = trunc_div((far_point[k] - slot.start[k]) * 0x1000, duration);
    const int dx = far_point[0] - slot.start[0], dy = far_point[1] - slot.start[1], dz = far_point[2] - slot.start[2];
    slot.flight_step = 0;
    slot.rest = ps1_square_root0(dx * dx + dy * dy + dz * dz) / 0x78;
  }
  // pt0 = head, then n = 1..58: y += gravity, x / y / z += the signed top byte of drift * n * 5, PIN_PREV.
  void pull(Slot& slot, int gravity, const int head[3]) const {
    slot.pt[0].pos = {head[0], head[1], head[2]};
    for (int link = 1; link <= kPts - 2; ++link) {
      Pt& point = slot.pt[static_cast<size_t>(link)];
      point.pos[1] = s16i(point.pos[1] + gravity);
      for (size_t k = 0; k < 3; ++k) point.pos[k] = s16i(point.pos[k] + static_cast<s8>(static_cast<u32>(drift_[k] * link * 5) >> 24));
      pin(point, slot.pt[static_cast<size_t>(link - 1)], slot.rest);
    }
  }
  void spark_mode1(Slot& slot, int si, const int anchor[3]) {
    slot.pt[kPts - 1].pos = {anchor[0], anchor[1], anchor[2]};
    if (slot.flight_step < 0x10) {
      ++slot.flight_step;
      for (int k = 0; k < 3; ++k) drift_[k] = slot.vel[k];
      for (int k = 0; k < 3; ++k) slot.vel[k] += (slot.vel[k] * 200) >> 12;
      int head[3];
      for (int k = 0; k < 3; ++k) head[k] = slot.start[k] + s16i((slot.vel[k] * slot.flight_step) >> 12);
      slot.gravity = 0;
      pull(slot, 0, head);
    } else {
      // Impact runs while the battle script gate word is set.
      if (ctx_.hooks.script_gate && ctx_.hooks.script_gate() != 0) impact(slot, si);
      ++slot.flight_step;
      int head[3] = {0, 0, 0};
      attach(target(si), 0x10, head);
      pull(slot, slot.gravity, head);
      for (int link = kPts - 2; link >= 1; --link) pin(slot.pt[static_cast<size_t>(link)], slot.pt[static_cast<size_t>(link + 1)], slot.rest);
      slot.pt[0].pos = {head[0], head[1], head[2]};
    }
  }
  // The gate is set by the pack's caster script (its atk entry writes global word 0 = 0, then 1). Per slot after the flight
  // hand = chain point 59, tip = point 0, delta = (tip - hand) 32 / 40 (s16); the slot counter += 1; state 0 -> emitter 't000':
  // op-0x0A VEL := delta, op-08 position := hand, `repeat` firings (+0xE), state 1; counter > 0x28 and state < 2 -> the type-0x10 entry 'hit1' starts (slot > 0: a clone serving target `slot`), the LAST slot clears
  // the gate word, state 2.
  void impact(Slot& slot, int si) {
    const Pt& hand = slot.pt[kPts - 1];
    const Pt& tip = slot.pt[0];
    int delta[3];
    for (size_t k = 0; k < 3; ++k) delta[k] = s16i(((tip.pos[k] - hand.pos[k]) * 0x20) / 0x28);
    const int counter = (hcnt_[static_cast<size_t>(si)] += 1);
    if (counter > 0x28 && hstate_[static_cast<size_t>(si)] < 2) {
      if (ctx_.sequences) ctx_.sequences->start("hit1", si);
      if (si == nslots_ - 1 && ctx_.hooks.clear_script_gate) ctx_.hooks.clear_script_gate();
      hstate_[static_cast<size_t>(si)] = 2;
    }
    if (hstate_[static_cast<size_t>(si)] == 0 && em()) {
      const int handle = em()->find("t000");
      if (handle >= 0) {
        em()->patch_op(static_cast<size_t>(handle), 0x0A, {delta[0], delta[1], delta[2], 0});
        em()->set_position("t000", hand.pos[0], hand.pos[1], hand.pos[2]);
        for (int i = 0; i < em()->header_def(static_cast<size_t>(handle)).repeat; ++i) em()->fire("t000");
      }
      hstate_[static_cast<size_t>(si)] = 1;
    }
  }
  // monfish: target 0's attach 0x10; oomono2: attach 0x10 of caster[-1] = the `act0` pseudo-actor model (a host without a
  // pseudo-actor: emitter slot A's position)
  void far_end(int out[3]) const {
    if (params_.kind == Ring::Monfish) {
      attach(target(0), 0x10, out);
      return;
    }
    Vec3 point{};
    if (ctx_.models && ctx_.models->pseudo_attach && ctx_.models->pseudo_attach(0x10, point)) {
      for (size_t k = 0; k < 3; ++k) out[k] = static_cast<s16>(std::lround(point[k]));
      return;
    }
    if (em() && em()->slot_position(0, point))
      for (size_t k = 0; k < 3; ++k) out[k] = static_cast<s16>(std::lround(point[k]));
  }
  void record_draw(const Slot& slot) {
    int acc = (rng_.next() % 10) * 10 + params_.base;
    bool flag = true;
    int cnt = 0;
    for (int k = 0; k < kPts - 1; ++k) {
      const int acc1 = flag ? acc - params_.step : acc + params_.step;
      if (cnt > 12) flag = !flag, cnt = 0;
      ++cnt;
      Line line;
      for (size_t axis = 0; axis < 3; ++axis) line.from[axis] = slot.pt[static_cast<size_t>(k)].pos[axis], line.to[axis] = slot.pt[static_cast<size_t>(k + 1)].pos[axis];
      line.ca = static_cast<u8>((acc >> 4) & 0xFF);
      line.cb = static_cast<u8>((acc1 >> 4) & 0xFF);
      lines_.push_back(line);
      acc = acc1;
    }
  }

  RingParams params_;
  bool pending_line_ = false;
  std::array<Slot, 12> slots_{};
  int count_[2] = {0, 0};
  int nslots_ = 0;
  // spark's impact helper: state / counter per slot.
  std::array<int, 12> hstate_{}, hcnt_{};
  int tcount_ = 0, frames_ = 1, dlt_[3] = {0, 0, 0}, drift_[3] = {0, 0, 0};
  Rng rng_{};
  std::vector<Line> lines_;
};

constexpr RingParams kParams[] = {
    {"malchera", Ring::Malchera, 0x500, 0x40},
    {"spark", Ring::Spark, 0x500, 0x40},
    {"monfish", Ring::Monfish, 0x300, 0x15},
    {"oomono2", Ring::Oomono2, 0x300, 0x15},
};

}  // namespace

void register_line_rings(BespokeRegistry& registry) {
  for (const RingParams& params : kParams) registry.add({params.name, "line-ring", "LineRing", [params] { return std::make_unique<LineRing>(params); }});
}

}  // namespace oc::effects
