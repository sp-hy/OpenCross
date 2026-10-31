#include "battle/pac_vm.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace oc::battle::pac {

// ------------------------------------------------------------------------------------------------ opcode table
const OpInfo& op_info(u8 code) {
  static const std::array<OpInfo, 256> tab = [] {
    std::array<OpInfo, 256> table{};
    for (size_t i = 0; i < table.size(); ++i) table[i].code = static_cast<u8>(i);
#define PAC_OP(c, l, n, cf) table[c] = OpInfo{static_cast<u8>(c), static_cast<u8>(l), n, cf};
#include "battle/pac_opcodes.inc"
#undef PAC_OP
    return table;
  }();
  return tab[code];
}

// ------------------------------------------------------------------------------------------------ module
namespace {
u32 rd32(const Bytes& bytes, size_t offset) { return offset + 4 <= bytes.size() ? static_cast<u32>(bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16)) | (static_cast<u32>(bytes[offset + 3]) << 24) : 0u; }
u16 rd16(const Bytes& bytes, size_t offset) { return offset + 2 <= bytes.size() ? static_cast<u16>(bytes[offset] | (bytes[offset + 1] << 8)) : u16{0}; }
}  // namespace

bool Module::parse(Bytes data, std::string* err) {
  auto fail = [&](const char* message) {
    if (err) *err = message;
    return false;
  };
  bytes = std::move(data);
  if (bytes.size() < 0x94) return fail("pac too small");
  const u32 ec = rd32(bytes, 0x90);
  if (ec == 0 || ec > 64) return fail("bad entity count");
  entities = static_cast<int>(ec);
  base = 0x94 + static_cast<size_t>(ec) * 0x40;
  end = rd32(bytes, 8);
  if (end > bytes.size() || end < base) return fail("bad script block end");
  ptr.assign(ec, {});
  for (size_t entity = 0; entity < ec; ++entity)
    for (size_t i = 0; i < kSlots; ++i) ptr[entity][i] = rd16(bytes, 0x94 + entity * 0x40 + 2 * i);
  return true;
}

std::vector<ScriptExtent> Module::scripts() const {
  struct Pointer {
    int entity, slot;
    u32 offset;
  };
  std::vector<Pointer> pointers;
  std::set<u32> uniq;
  for (int entity = 0; entity < entities; ++entity)
    for (int slot = 0; slot < kSlots; ++slot) {
      const u16 offset = ptr[static_cast<size_t>(entity)][static_cast<size_t>(slot)];
      if (offset) {
        pointers.push_back({entity, slot, offset});
        uniq.insert(offset);
      }
    }
  std::map<u32, u32> ext;
  for (auto it = uniq.begin(); it != uniq.end(); ++it) {
    auto nx = std::next(it);
    ext[*it] = nx != uniq.end() ? *nx : static_cast<u32>(code_size());
  }
  std::vector<ScriptExtent> out;
  if (!uniq.empty() && *uniq.begin() > 0) out.push_back({0, 0, 0, *uniq.begin()});
  for (const Pointer& pointer : pointers) out.push_back({pointer.entity, pointer.slot, pointer.offset, ext[pointer.offset]});
  return out;
}

// ------------------------------------------------------------------------------------------------ vm
Vm::Vm(const Module& module, Host& host) : mod_(module), host_(host), vars_(0x8000, 0), read_tick_(0x8000, 0), read_entity_(0x8000, 0) {
  ctx_.resize(static_cast<size_t>(module.entities));
  for (int entity = 0; entity < module.entities; ++entity) {
    Ctx& ctx = ctx_[static_cast<size_t>(entity)];
    const u16 p0 = module.ptr[static_cast<size_t>(entity)][0];
    Frame frame;
    frame.slot = 0;
    frame.mark = 0;
    if (entity == 0 || p0 != 0) {
      frame.pc = p0;
    } else {
      // No init script: go straight to the idle loop.
      const u16 p1 = module.ptr[static_cast<size_t>(entity)][1];
      frame.pc = p1 ? p1 : u16{0xFFFF};
      frame.slot = 1;
      frame.mark = 7;
    }
    ctx.frames.push_back(frame);
  }
  // last attack type / element / target: global (0x20) then per PC (0x26 step 6)
  for (int off : {0x20, 0x26, 0x2C, 0x32}) {
    set_var(off, 0xFF);
    set_var(off + 2, 0xFFFF);
    set_var(off + 4, 0);
  }
}

// Pose / Anim requests of a slot-1 loop repeat every tick by design (3D / 58 under `cond var == x`); the identical repeat is dropped (the real ops
// are idempotent for the model: re-starting the running clip changes nothing).
int Vm::emit(Event::Kind kind, int unit, int arg_a, int arg_b) {
  if (cur_slot_ == 1 && (kind == Event::Kind::Pose || kind == Event::Kind::Anim)) {
    auto& last = idle_last_[{cur_ent_, static_cast<int>(kind)}];
    const std::array<int, 3> now{unit, arg_a, arg_b};
    if (last == now) return -1;
    last = now;
  }
  Event ev;
  ev.kind = kind;
  ev.unit = unit;
  ev.a = arg_a;
  ev.b = arg_b;
  ev.vt = vtime_;
  ev.seq = seq_++;
  events_.push_back(ev);
  return ev.seq;
}

u32 Vm::targetable() const {
  u32 mask = host_.alive_mask();
  for (int unit = 0; unit < kUnits; ++unit)
    if (untarget_[static_cast<size_t>(unit)]) mask &= ~(1u << unit);
  return mask;
}

void Vm::note_cast_colour(int colour) {
  if (creset_)
    for (u8& entry : chist_) entry = 0;
  chist_[static_cast<size_t>(ccur_)] = static_cast<u8>(colour + 1);
  if (ccur_ == 7)
    for (size_t i = 0; i < 7; ++i) chist_[i] = chist_[i + 1];
  else
    ++ccur_;
  creset_ = false;
  if (colour == 6) {
    ccur_ = 0;
    creset_ = true;
  }
  set_var(0x52, colour);
}

void Vm::refresh_reserved() {
  for (int k = 0; k < 3; ++k) {
    set_var(0x38 + 2 * k, host_.ring_colour(k));
    set_var(0x3E + 2 * k, host_.pc_char_id(k));
  }
  for (int unit = 3; unit < kUnits; ++unit) set_var(0x44 + 2 * (unit - 3), host_.monster_id(unit));
  const int timer = host_.timer_frames();
  set_var(0x54, timer & 0xFFFF);
  set_var(0x56, (timer >> 16) & 0xFFFF);
}

int Vm::entity_of_unit(int unit) const {
  for (int entity = 1; entity < mod_.entities; ++entity) {
    const Ctx& ctx = ctx_[static_cast<size_t>(entity)];
    if (!ctx.dropped && ctx.unit == unit) return entity;
  }
  return -1;
}

bool Vm::has_slot(int unit, int slot) const {
  const int entity = entity_of_unit(unit);
  return entity >= 0 && slot >= 0 && slot < kSlots && mod_.ptr[static_cast<size_t>(entity)][static_cast<size_t>(slot)] != 0;
}

bool Vm::launch(int unit, int slot) {
  const int entity = entity_of_unit(unit);
  if (entity < 0 || slot < 0 || slot >= kSlots) return false;
  Ctx& ctx = ctx_[static_cast<size_t>(entity)];
  const u16 offset = mod_.ptr[static_cast<size_t>(entity)][static_cast<size_t>(slot)];
  if (!offset || static_cast<int>(ctx.frames.size()) >= kMaxFrames) return false;
  if ((slot == 2 || slot == 4) && lock_ != kNoUnit && lock_ != unit) return false;
  switch (slot) {
    case 2:
      if (ctx.s2) return false;
      ctx.s2 = true;
      lock_ = unit;
      break;
    case 3:
      if (ctx.s3) return false;
      ctx.s3 = true;
      break;
    case 4:
      if (ctx.s4 || ctx.s3) return false;
      ctx.s4 = true;
      lock_ = unit;
      break;
    default: break;
  }
  Frame frame;
  frame.pc = offset;
  frame.slot = static_cast<u8>(slot);
  frame.mark = slot == 2 ? 2 : slot == 4 ? 1 : 0;
  ctx.frames.push_back(frame);
  // Var 0x50 is NOT written per script run: the engine writes it once, at the PC's command confirm (see ScriptedBrain::on_attack).
  return true;
}

bool Vm::run_slot(int unit, int slot, std::vector<QueuedAttack>& out, int max_ticks) {
  if (!launch(unit, slot)) return false;
  const int entity = entity_of_unit(unit);
  if (entity < 0) return false;
  Ctx& ctx = ctx_[static_cast<size_t>(entity)];
  const size_t depth = ctx.frames.size();
  auto& pending = queue_[static_cast<size_t>(unit)];
  auto collect = [&] {
    out.insert(out.end(), pending.begin(), pending.end());
    pending.clear();
  };
  bool done = false;
  for (int tick_index = 0; tick_index < max_ticks && !done; ++tick_index) {
    tick();
    if (!pending.empty() && ctx.a4_wait) collect();
    done = ctx.frames.size() < depth || ctx.dropped;
  }
  collect();
  if (!done) {
    // Runaway script: unwind.
    ctx.frames.resize(depth - 1);
    ctx.s2 = ctx.s4 = false;
    if (lock_ == unit) lock_ = kNoUnit;
    return false;
  }
  if (lock_ == unit) lock_ = kNoUnit;
  return true;
}

void Vm::tick() {
  ++vtime_;
  stale_read_ = false;
  refresh_reserved();
  for (int entity = 0; entity < mod_.entities; ++entity)
    if (!ctx_[static_cast<size_t>(entity)].dropped) interpret(entity);
}

bool Vm::quiescent() const {
  for (int entity = 0; entity < mod_.entities; ++entity) {
    const Ctx& ctx = ctx_[static_cast<size_t>(entity)];
    if (ctx.dropped) continue;
    const u16 p1 = mod_.ptr[static_cast<size_t>(entity)][1];
    if (ctx.frames.size() != 1 || ctx.delay_armed || ctx.frames[0].pc != (p1 ? p1 : u16{0xFFFF})) return false;
  }
  return true;
}

bool Vm::settle(int max_ticks) {
  for (int tick_index = 0; tick_index < max_ticks; ++tick_index) {
    tick();
    // An idle-loop frame (slot 1, e.g. Mama Komodo's arrival: 66, 67 self-cast 0x140, A4, ghost_self 0) queues an attack that no slot run
    // drains, and its A4 would wait for it forever. The entries move to the idle list for the executor to play.
    for (int entity = 0; entity < mod_.entities; ++entity) {
      Ctx& ctx = ctx_[static_cast<size_t>(entity)];
      if (!ctx.a4_wait || ctx.dropped || ctx.unit < 0 || ctx.unit >= kUnits) continue;
      std::vector<QueuedAttack>& pending = queue_[static_cast<size_t>(ctx.unit)];
      for (const QueuedAttack& attack : pending) idle_attacks_.emplace_back(ctx.unit, attack);
      pending.clear();
    }
    if (quiescent() && !stale_read_) return true;
  }
  return false;
}

void Vm::do_halt(int entity) {
  Ctx& ctx = ctx_[static_cast<size_t>(entity)];
  if (ctx.frames.size() > 1) {
    const Frame top = ctx.frames.back();
    ctx.frames.pop_back();
    if (top.slot == 2) ctx.s2 = false;
    if (top.slot == 4) ctx.s4 = false;
    if ((top.slot == 2 || top.slot == 4) && lock_ == ctx.unit) lock_ = kNoUnit;
  }
  Frame frame;
  frame.pc = mod_.ptr[static_cast<size_t>(entity)][1] ? mod_.ptr[static_cast<size_t>(entity)][1] : u16{0xFFFF};
  frame.slot = 1;
  frame.mark = 7;
  ctx.frames[0] = frame;
  ctx.a4_wait = false;
}

void Vm::interpret(int entity) {
  Ctx& ctx = ctx_[static_cast<size_t>(entity)];
  budget_ = 0x40;
  // Ops 59 / 67 reload the budget, so a loop through one of them only ends at a halt, a block or a stop; retail has no other limit, the
  // step cap only keeps a runaway script from hanging the engine.
  int steps = 0;
  while (budget_ > 0 && !ctx.dropped && ++steps <= 0x10000) {
    --budget_;
    Frame& frame = ctx.frames.back();
    if (frame.pc == 0xFFFF) break;
    if (frame.pc >= mod_.code_size()) {
      invalid_.insert(-1);
      ctx.dropped = true;
      break;
    }
    const u8 op = mod_.at(frame.pc);
    const OpInfo& oi = op_info(op);
    if (oi.len == 0 || frame.pc + oi.len > mod_.code_size()) {
      invalid_.insert(op);
      // The real handler stalls forever; nothing can come of it.
      ctx.dropped = true;
      break;
    }
    int adv = oi.len;
    cur_ent_ = entity;
    cur_slot_ = frame.slot;
    ++ops_;
    ++op_counts_[op];
    if (oi.conf == 'U') unimplemented_.insert(op);
    const Step result = exec(entity, frame, op, adv);
    for (const auto& [index, before] : writes_)
      if (vars_[index] != before && read_tick_[index] == vtime_ && read_entity_[index] < entity) stale_read_ = true;
    writes_.clear();
    if (op == 0x59 || op == 0x67) budget_ = 0x1F;
    if (result == Step::Next) ctx.frames.back().pc = static_cast<u16>(ctx.frames.back().pc + adv);
    else if (result == Step::Block || result == Step::Stop) break;
  }
}

// ---- targets
int Vm::target_mask(int entity, int code) {
  const u32 alive = host_.alive_mask(), tg = targetable();
  const u32 pcs = 0x7, enemies = 0x1F8;
  const int self = self_unit(entity);
  // A random pick draws rnd(first, last) until the unit is targetable and not `exclude`; once every unit of the range was drawn without
  // success it gives unit 0's bit.
  auto pick = [&](int first, int last, int exclude) -> int {
    u32 range = 0, tried = 0;
    for (int unit = first; unit <= last; ++unit) range |= 1u << unit;
    while ((tried & range) != range) {
      const int unit = host_.rnd(first, last);
      if ((tg >> unit & 1u) && unit != exclude) return 1 << unit;
      tried |= 1u << unit;
    }
    return 1;
  };
  auto group_of = [&](int unit) {
    for (int k = 1; k < mod_.entities; ++k)
      if (ctx_[static_cast<size_t>(k)].unit == unit && !ctx_[static_cast<size_t>(k)].dropped) return ctx_[static_cast<size_t>(k)].group;
    return 0xFF;
  };
  // The group picks draw rnd(0, 9) until the unit was present at battle start, is alive (the untargetable flag is not tested) and passes
  // the group test; with no such unit retail never returns, here the pick gives 0 without drawing.
  auto group_pick = [&](auto&& accept) -> int {
    const u32 present = host_.present_mask();
    u32 valid = 0;
    for (int unit = 0; unit < kUnits; ++unit)
      if ((present >> unit & 1u) && (alive >> unit & 1u) && accept(unit)) valid |= 1u << unit;
    if (!valid) return 0;
    for (;;) {
      const int unit = host_.rnd(0, 9);
      if (unit < kUnits && (valid >> unit & 1u)) return 1 << unit;
    }
  };
  // The first unit of one side present at battle start that is KO'd now (0x17 enemies, 0x1B PCs), else 0.
  auto first_down = [&](u32 side) -> int {
    const u32 present = host_.present_mask();
    for (int unit = 0; unit < kUnits; ++unit)
      if ((side >> unit & 1u) && (present >> unit & 1u) && (host_.read_field(unit, 0xB0) & st::kKO)) return 1 << unit;
    return 0;
  };
  const int lo = code & 0xFF;
  if (lo == 0x1A || lo >= 0x50) {
    const int id = (code + 0x80) & 0xFF;
    for (int i = 0; i < kPcSlots; ++i)
      if ((tg >> i & 1u) && host_.pc_char_id(i) == id) return 1 << i;
    return 1;
  }
  switch (lo) {
    case 0x00: return pick(0, kPcSlots - 1, -1);
    case 0x10: return pick(kPcSlots, kUnits - 1, -1);
    case 0x01: return static_cast<int>(alive & pcs);
    case 0x24: return static_cast<int>(alive & enemies);
    case 0x18: return pick(0, kUnits - 1, self);
    case 0x19: return pick(kPcSlots, kUnits - 1, self);
    case 0x25: {
      const bool charmed = self < kUnits && (host_.read_field(self, 0x40B2) & 0x10) != 0;
      return charmed ? pick(0, kUnits - 1, self) : pick(0, kPcSlots - 1, -1);
    }
    case 0x17: return first_down(enemies);
    case 0x1B: return first_down(pcs);
    case 0x1C: {
      const int mine = self < kUnits ? group_of(self) : 0xFF;
      return group_pick([&](int unit) { return group_of(unit) != mine; });
    }
    default:
      if (lo >= 0x1D && lo <= 0x23) return group_pick([&](int unit) { return group_of(unit) == lo - 0x1D; });
      return 0;
  }
}

std::optional<std::array<u8, 6>> Vm::probe_group(int element) const {
  for (const ScriptExtent& extent : mod_.scripts()) {
    int last8d = -1;
    // The vars as the straight-line assignments of this script (e.g. `set v, self; sub v, 0xE0` before its 8D) leave them.
    std::vector<u16> local_vars = vars_;
    for (u32 pc = extent.start; pc < extent.end;) {
      const u8 op = mod_.at(pc);
      const int len = op_info(op).len;
      if (len <= 0) break;
      auto r16 = [&](int off) { return static_cast<u16>(mod_.at(pc + static_cast<u32>(off)) | (mod_.at(pc + static_cast<u32>(off) + 1) << 8)); };
      auto vr = [&](u16 raw) { return static_cast<int>(static_cast<s16>(local_vars[static_cast<size_t>(raw & 0xFFFE) >> 1])); };
      if (op == 0x10 || op == 0x13 || op == 0x14) {
        const u8 sp = mod_.at(pc + static_cast<u32>(len - 1));
        const u16 raw1 = r16(3);
        const int rhs = (sp >> 6) & 1 ? static_cast<int>(static_cast<s16>(raw1)) : vr(raw1);
        u16& dest = local_vars[static_cast<size_t>(r16(1) & 0xFFFE) >> 1];
        dest = static_cast<u16>(op == 0x10 ? rhs : op == 0x13 ? static_cast<s16>(dest) + rhs : static_cast<s16>(dest) - rhs);
      }
      if (op == 0x8D) last8d = static_cast<int>(pc);
      if (op == 0x67 && last8d >= 0) {
        const u8 spec = mod_.at(pc + static_cast<u32>(len - 1));
        auto operand_s = [&](int k) { const u16 raw = r16(1 + 2 * k); return (spec >> (7 - k)) & 1 ? static_cast<int>(static_cast<s16>(raw)) : vr(raw); };
        if ((operand_s(0) & 0xFF) == 2 && operand_s(1) == element) {
          const u32 p8 = static_cast<u32>(last8d);
          std::array<u8, 6> out{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
          int count = 0;
          for (int k = 0; k < 6; ++k) {
            const u16 raw = static_cast<u16>(mod_.at(p8 + 1 + 2 * static_cast<u32>(k)) | (mod_.at(p8 + 2 + 2 * static_cast<u32>(k)) << 8));
            const int value = raw & 0x8000 ? raw & 0x7FFF : vr(raw);
            if ((value & 0xFFFF) != 0xFF) out[static_cast<size_t>(count++)] = static_cast<u8>(value);
          }
          return out;
        }
      }
      pc += static_cast<u32>(len);
    }
  }
  return std::nullopt;
}

// ---- one instruction
Vm::Step Vm::exec(int entity, Frame& frame, u8 op, int& adv) {
  Ctx& ctx = ctx_[static_cast<size_t>(entity)];
  const u32 pc = frame.pc;
  const int len = op_info(op).len;
  // Dev: trace every executed op.
  static const bool pac_trace = std::getenv("OC_PAC_TRACE") != nullptr;
  if (pac_trace) std::fprintf(stderr, "pac: ent %d unit %d slot %d pc %04X op %02X\n", entity, ctx.unit, static_cast<int>(frame.slot), pc, op);
  const u8 spec = mod_.at(pc + static_cast<u32>(op == 0x02 ? 5 : len - 1));
  auto r16 = [&](int off) -> u16 { return static_cast<u16>(mod_.at(pc + static_cast<u32>(off)) | (mod_.at(pc + static_cast<u32>(off) + 1) << 8)); };
  auto vr = [&](u16 raw) -> int {
    const size_t index = static_cast<size_t>(raw & 0xFFFE) >> 1;
    if (read_tick_[index] != vtime_ || read_entity_[index] > entity) {
      read_tick_[index] = vtime_;
      read_entity_[index] = static_cast<u8>(entity);
    }
    return static_cast<s16>(vars_[index]);
  };
  // S-mode operand k.
  auto operand_s = [&](int k) -> int {
    const u16 raw = r16(1 + 2 * k);
    return (spec >> (7 - k)) & 1 ? static_cast<s16>(raw) : vr(raw);
  };
  // W-mode operand.
  auto operand_w = [&](int off) -> int {
    const u16 raw = r16(off);
    return raw & 0x8000 ? raw & 0x7FFF : vr(raw);
  };
  auto dst = [&](int off) -> u16& {
    const size_t index = static_cast<size_t>(r16(off) & 0xFFFE) >> 1;
    writes_.emplace_back(index, vars_[index]);
    return vars_[index];
  };
  auto b8 = [&](int off) -> int { return mod_.at(pc + static_cast<u32>(off)); };
  const int self = ctx.unit;
  // E0..E8 -> 0..8, E9 -> self; plain 0..8 accepted.
  auto unit_of = [&](int operand) -> int {
    operand &= 0xFF;
    if (operand == 0xE9) return self;
    if (operand >= 0xE0) return operand - 0xE0;
    return operand;
  };
  auto valid = [](int unit) { return unit >= 0 && unit < kUnits; };
  auto& myq = valid(self) ? queue_[static_cast<size_t>(self)] : scratch_;
  auto release = [&] {
    if (lock_ == self) lock_ = kNoUnit;
  };
  auto store = [&](u16& dest, int value) { dest = static_cast<u16>(value); };
  auto binop = [&](auto fn) {
    u16& dest = dst(1);
    store(dest, fn(static_cast<s16>(dest), operand_s(1)));
  };

  switch (op) {
    case 0x00: do_halt(entity); return Step::Stop;
    case 0x01: frame.pc = r16(1); return Step::Jump;
    case 0x02: {
      const int lhs = operand_s(0), rhs = operand_s(1);
      bool taken = false;
      switch (spec & 15) {
        case 0: taken = lhs == rhs; break;
        case 1: taken = lhs != rhs; break;
        case 2: taken = lhs > rhs; break;
        case 3: taken = lhs < rhs; break;
        case 4: taken = lhs >= rhs; break;
        case 5: taken = lhs <= rhs; break;
        case 6: taken = ((lhs & rhs) & 0xFFFF) != 0; break;
        case 7: taken = lhs != rhs; break;
        case 8: taken = lhs != 0 || rhs != 0; break;
        case 9: taken = ((lhs >> (rhs & 15)) & 1) != 0; break;
        case 10: taken = ((lhs >> (rhs & 15)) & 1) == 0; break;
        default: break;
      }
      if (taken) return Step::Next;
      frame.pc = r16(6);
      return Step::Jump;
    }
    case 0x05:
      if (frame.depth >= frame.ret.size()) {
        ctx.dropped = true;
        invalid_.insert(-2);
        return Step::Stop;
      }
      frame.ret[frame.depth++] = static_cast<u16>(pc + 3);
      frame.pc = r16(1);
      return Step::Jump;
    case 0x0D:
      if (frame.depth == 0) {
        do_halt(entity);
        return Step::Stop;
      }
      frame.pc = frame.ret[--frame.depth];
      return Step::Jump;
    case 0x0E: {
      const int unit = operand_w(1) - 0xE0;
      if (unit >= 0 && unit < kUnits && (host_.alive_mask() >> unit & 1u)) {
        ctx.unit = unit;
        return Step::Next;
      }
      ctx.unit = 9;
      ctx.dropped = true;
      return Step::Stop;
    }
    case 0x10: binop([](int, int rhs) { return rhs; }); break;
    case 0x11: dst(1) = 1; break;
    case 0x12: dst(1) = 0; break;
    case 0x13: binop([](int lhs, int rhs) { return lhs + rhs; }); break;
    case 0x14: binop([](int lhs, int rhs) { return lhs - rhs; }); break;
    case 0x15: binop([](int lhs, int rhs) { return lhs | (1 << (rhs & 15)); }); break;
    case 0x16: binop([](int lhs, int rhs) { return lhs & ~(1 << (rhs & 15)); }); break;
    case 0x17: ++dst(1); break;
    case 0x18: --dst(1); break;
    case 0x19: binop([](int lhs, int rhs) { return lhs & rhs; }); break;
    case 0x1A: binop([](int lhs, int rhs) { return lhs | rhs; }); break;
    case 0x1B: binop([](int lhs, int rhs) { return lhs ^ rhs; }); break;
    case 0x1C: {
      u16& dest = dst(1);
      store(dest, static_cast<int>(static_cast<u32>(dest) << (operand_w(3) & 15)));
      break;
    }
    case 0x1D: {
      u16& dest = dst(1);
      store(dest, static_cast<s16>(dest) >> (operand_w(3) & 15));
      break;
    }
    case 0x1E: store(dst(1), host_.rnd(0, 0x7FFF)); break;
    case 0x1F: {
      // (rand() & 0xFFFF) % (B + 1), with the libc generator.
      const int modulus = operand_w(3) + 1;
      store(dst(1), modulus > 0 ? (host_.libc_rand() & 0xFFFF) % modulus : 0);
      break;
    }
    case 0x20:
    case 0x21: {
      // A immediate: nothing to write.
      if ((spec >> 7) & 1) break;
      u16& dest = dst(1);
      const int lhs = static_cast<s16>(dest), rhs = operand_s(1);
      store(dest, op == 0x20 ? lhs * rhs : (rhs != 0 ? lhs / rhs : 0));
      break;
    }
    case 0x2A:
      if (!myq.empty() || !host_.action_ready()) {
        ctx.a4_wait = true;
        return Step::Block;
      }
      ctx.a4_wait = false;
      ctx.s4 = false;
      emit(Event::Kind::Barrier, self, op, host_.action_serial());
      release();
      break;
    case 0x2B: store(dst(1), target_mask(entity, operand_s(1))); break;
    case 0x2C:
      if (!host_.action_ready()) return Step::Block;
      emit(Event::Kind::Barrier, self, op, host_.action_serial());
      emit(Event::Kind::Cutscene, self, 1);
      break;
    case 0x2D: {
      if (skip_delays_) break;
      if (!ctx.delay_armed) {
        ctx.delay_armed = true;
        ctx.delay = 2 * operand_w(1);
        return Step::Block;
      }
      if (--ctx.delay > 0) return Step::Block;
      ctx.delay_armed = false;
      break;
    }
    case 0x2F: {
      u16& dest = dst(1);
      const int col = operand_w(3);
      int cnt[6]{};
      for (int k = 0; k < 3; ++k) {
        const int rc = host_.ring_colour(k);
        if (rc >= 0 && rc < 6) ++cnt[rc];
      }
      store(dest, col >= 0 && col < 6 ? cnt[col] : *std::max_element(cnt, cnt + 6));
      break;
    }
    case 0x34:
      if (valid(self)) ghost_[static_cast<size_t>(self)] = untarget_[static_cast<size_t>(self)] = b8(1) != 0;
      if (valid(self) && b8(1) == 0) host_.reveal(self);
      emit(Event::Kind::Ghost, self, b8(1) != 0);
      break;
    case 0x35: {
      const int en = b8(2);
      const int unit = en >= 0 && en < mod_.entities ? ctx_[static_cast<size_t>(en)].unit : kNoUnit;
      if (valid(unit)) ghost_[static_cast<size_t>(unit)] = untarget_[static_cast<size_t>(unit)] = b8(1) != 0;
      if (valid(unit) && b8(1) == 0) host_.reveal(unit);
      emit(Event::Kind::Ghost, unit, b8(1) != 0);
      break;
    }
    case 0x5B: {
      // ONLY the untargetable flag (= f != 0) and the all-target cast bit (f == 0: set, else cleared) change. The hidden flag
      // (what count_enemies / read_flag / the victory test read) and the model's visibility are not touched (a Bomber that starts off-stage
      // stays off-stage even though its idle script runs `5B 0 e4` every tick).
      const int unit = unit_of(operand_w(2));
      if (valid(unit)) untarget_[static_cast<size_t>(unit)] = b8(1) != 0;
      if (valid(unit)) { if (b8(1) == 0) host_.reveal(unit); else host_.conceal(unit); }
      break;
    }
    case 0x38:
      emit(Event::Kind::Cutscene, self, 0);
      release();
      break;
    case 0x3D: emit(Event::Kind::Pose, self, operand_w(1)); break;
    case 0x55: {
      const PickResult pick = host_.pick_element(self, -1);
      store(dst(1), pick.element);
      store(dst(3), static_cast<int>(pick.mask));
      break;
    }
    case 0x8F: {
      const PickResult pick = host_.pick_element(unit_of(operand_w(5)), 6);
      store(dst(1), pick.element);
      store(dst(3), static_cast<int>(pick.mask));
      break;
    }
    case 0x56: {
      const int unit = unit_of(operand_w(3));
      store(dst(1), valid(unit) && ghost_[static_cast<size_t>(unit)]);
      break;
    }
    case 0x57:
    case 0x80: {
      // Only units that are not off-stage count.
      u32 mask = host_.alive_mask() & (op == 0x57 ? 0x1F8u : 0x7u);
      for (int unit = 0; unit < kUnits; ++unit)
        if (ghost_[static_cast<size_t>(unit)]) mask &= ~(1u << unit);
      store(dst(1), std::popcount(mask));
      break;
    }
    // 58: start animation a & 0xFF7F; bit 0x80 = only when the model runs no animation stream. 5A: entity b's unit, the
    // word as it is (>= 0x80 = the external table), no gate.
    case 0x58: emit(Event::Kind::Anim, self, operand_w(1) & 0xFF7F, (operand_w(1) & 0x80) != 0); break;
    case 0x5A: {
      const int en = b8(1);
      emit(Event::Kind::Anim, en >= 0 && en < mod_.entities ? ctx_[static_cast<size_t>(en)].unit : kNoUnit, operand_w(2), 0);
      break;
    }
    case 0x59: break;
    case 0x62: emit(Event::Kind::DialogPos, -1, operand_w(1), operand_w(3)); break;
    case 0x65:
    case 0x66:
      if (!host_.action_ready()) return Step::Block;
      if (op == 0x65 && valid(self)) host_.begin_scripted(self, var(0x50));
      emit(Event::Kind::Barrier, self, op, host_.action_serial());
      break;
    case 0x67: {
      if (!valid(self)) break;
      QueuedAttack qa;
      qa.type = operand_s(0) & 0xFF;
      qa.value = operand_s(1);
      u32 mask = static_cast<u32>(operand_s(2)) & 0xFFFFu & host_.roster_mask();
      if (qa.type == 0 || qa.type == 5) {
        if (qa.value < 0 || qa.value > 2) qa.value = 0;
        if (!mask) mask = 1;
      } else if (qa.type == 1 || qa.type == 2 || qa.type == 3 || qa.type == 6) {
        if (!mask) mask = 1u << self;
      }
      qa.mask = mask;
      // Type 2: group[k] = low byte of var 0x10 + 2k (the 8D list); the executor's group setup reads it.
      if (qa.type == 2)
        for (int k = 0; k < 6; ++k) grp_[static_cast<size_t>(k)] = static_cast<u8>(var(0x10 + 2 * k));
      qa.group = grp_;
      if (myq.size() < 4) {
        qa.seq = emit(Event::Kind::Queue, self, qa.type, qa.value);
        myq.push_back(qa);
      }
      break;
    }
    case 0x75: {
      const int unit = unit_of(operand_s(1));
      store(dst(1), valid(unit) ? host_.read_field(unit, static_cast<u16>(operand_s(2))) : 0);
      break;
    }
    case 0x76: {
      const int unit = unit_of(operand_s(1));
      if (valid(unit)) host_.write_field(unit, static_cast<u16>(operand_s(2)), operand_s(0));
      break;
    }
    case 0x77: break;
    case 0x7A: emit(Event::Kind::Camera, self); break;
    case 0x7C:
      no_drops_ = true;
      emit(Event::Kind::NoDrops);
      break;
    case 0x83:  // pending result = (s8)(v - 0x80); the end check ends the battle while it is negative and restores the code (- 0x80)
      if (static_cast<s8>(static_cast<u8>(operand_w(1) - 0x80)) < 0) forced_result_ = operand_w(1) & 0xFF;
      emit(Event::Kind::SetResult, -1, operand_w(1));
      break;
    case 0x84: emit(Event::Kind::QuarterHp, unit_of(operand_w(1))); break;
    case 0x86: {
      // Arg 0xE9 = the script's own unit; else the entity table slot (arg + 0x21) & 0xFF, whose unit byte is
      // the unit the camera follows (arg E0 + u -> entity u + 1).
      const int arg = operand_w(1);
      const int en = (arg + 0x21) & 0xFF;
      emit(Event::Kind::Camera, (arg & 0xFFFF) == 0xE9 ? self : (en < mod_.entities ? ctx_[static_cast<size_t>(en)].unit : kNoUnit));
      break;
    }
    case 0x87: myq.clear(); break;
    case 0x89: {
      const int sec = host_.timer_frames() / 60;
      store(dst(1), sec >= operand_w(3) + operand_w(5) * 3600);
      break;
    }
    case 0x8C: defeated_mask_ |= static_cast<u32>(operand_w(1)) & 0xFFFF; emit(Event::Kind::Defeated, -1, operand_w(1)); break;
    case 0x8D: {
      std::array<int, 6> values{};
      int count = 0;
      for (int k = 0; k < 6; ++k) {
        const int operand = operand_w(1 + 2 * k);
        if (operand != 0xFF) values[static_cast<size_t>(count++)] = operand;
      }
      for (int k = 0; k < 6; ++k) set_var(0x10 + 2 * k, k < count ? values[static_cast<size_t>(k)] : 0xFF);
      break;
    }
    case 0x8E: ctx.group = operand_w(1); break;
    case 0x96: emit(Event::Kind::Invuln, -1, operand_w(1)); break;
    case 0xA4:
      if (!myq.empty() || !host_.action_ready()) {
        ctx.a4_wait = true;
        return Step::Block;
      }
      ctx.a4_wait = false;
      ctx.s2 = false;
      emit(Event::Kind::Barrier, self, op, host_.action_serial());
      release();
      break;
    case 0xA6:  // an enemy's kill script ends: alive mask bit cleared, unit added to the defeated mask, op 5C's flag cleared
      if (valid(self) && self >= kPcSlots) scripted_death_[static_cast<size_t>(self)] = false;
      emit(Event::Kind::Killed, self);
      ctx.s4 = false;
      release();
      break;
    case 0xA7: {  // Pip's evolution check `A7 dst unit`: var dst = the form element 0x80..0x85 or 0 (also the host's pending form when not 0); the unit
                  // = operand 2 (a variable holding E0..E8; the PC entities pass their own unit), its kind byte (field 0x2D)
      const int green = vr(0x04), white = vr(0x06), black = vr(0x08), red = vr(0x0A), yellow = vr(0x0C), blue = vr(0x0E);
      const int light_total = green + white + blue, dark_total = black + red + yellow;
      const int who = unit_of(vr(r16(3)));
      const int kind = valid(who) ? host_.read_field(who, 0x2D) & 0xFF : 0;
      int res = 0;
      if (kind == 0x2B) res = dark_total >= light_total ? (dark_total >= 100 ? 0x80 : 0) : (light_total >= 125 ? 0x81 : 0);
      else if (kind == 0x2C) res = dark_total >= light_total ? (dark_total >= 300 ? 0x82 : 0) : (light_total >= 200 ? 0x83 : 0);
      else if (kind == 0x2D) res = dark_total >= light_total ? (dark_total >= 200 ? 0x84 : 0) : (light_total >= 300 ? 0x85 : 0);
      store(dst(1), res);
      if (res != 0) host_.set_evo_pending(res);
      break;
    }
    // B0 / B1: lift / land tween, no model here.
    case 0xB0:
    case 0xB1: break;
    case 0xE0: emit(Event::Kind::Dialogue, self, r16(1)); break;
    case 0xE2: emit(Event::Kind::WaitDialogue, self); break;
    case 0x39: flag_ui_ = b8(1) != 0; emit(Event::Kind::EngineFlag, -1, op, b8(1)); break;
    case 0x7D: flag_831_ = b8(1) != 0; emit(Event::Kind::EngineFlag, -1, op, b8(1)); break;
    case 0x81: flag_end_ = b8(1) != 0; emit(Event::Kind::EngineFlag, -1, op, b8(1)); break;
    case 0xAF: flag_menu_lock_ = b8(1) != 0; emit(Event::Kind::EngineFlag, -1, op, b8(1)); break;
    case 0x5C: {
      const int unit = unit_of(operand_w(2));
      const bool owed = b8(1) == 0;
      // The word is stored every time; a slot-1 idle loop (e.g. `5C 0 e3; halt`) runs it each pass, so only a change is an event
      const bool changed = !valid(unit) || scripted_death_[static_cast<size_t>(unit)] != owed;
      if (valid(unit)) scripted_death_[static_cast<size_t>(unit)] = owed;
      if (changed) emit(Event::Kind::ScriptedDeath, unit, owed);
      break;
    }
    case 0x7B:
      host_.set_menu_cursor(operand_w(1));
      emit(Event::Kind::MenuCursor, -1, operand_w(1));
      break;
    case 0x85: {
      const int unit = unit_of(operand_w(3)), shade = operand_w(1);
      host_.set_shade(unit, shade);
      emit(Event::Kind::Shade, unit, shade);
      break;
    }
    case 0x91: emit(Event::Kind::SyncPos, unit_of(operand_w(1))); break;
    case 0xC0:
    case 0xC2: {
      const u32 tag = r16(1) | (static_cast<u32>(r16(3)) << 16);
      emit(Event::Kind::EffectStart, -1, op == 0xC2, static_cast<int>(tag));
      break;
    }
    case 0xC1:
    case 0xC3: emit(Event::Kind::EffectStop, -1, op == 0xC3); break;
    case 0xC4:
    case 0xC5: {
      const u32 tag = r16(1) | (static_cast<u32>(r16(3)) << 16);
      if (host_.effect_busy(op == 0xC5, tag)) return Step::Block;
      break;
    }
    case 0xAE: {  // d = (colour history[0..6] == sequence 5,4,1,6,3,2,7 [colour+1: yellow red green blue black white, colourless]); a match mutes battle sfx
      static constexpr u8 kSeq[7] = {5, 4, 1, 6, 3, 2, 7};
      bool same = true;
      for (int i = 0; i < 7; ++i) same = same && chist_[static_cast<size_t>(i)] == kSeq[i];
      store(dst(1), same);
      if (same) {
        sfx_muted_ = true;
        emit(Event::Kind::SfxMute);
      }
      break;
    }
    case 0xAB: {  // d = the colour (0..5, 6 = the closing colourless cast) the sequence still needs next; B == 0 maps it through the alternate order
      static constexpr u8 kSeq[7] = {5, 4, 1, 6, 3, 2, 7}, kAlt[8] = {5, 3, 2, 6, 1, 4, 7, 0};
      int start = 6;
      while (start >= 0 && chist_[static_cast<size_t>(start)] != kSeq[0]) --start;
      int next = 4;
      if (start == 6) next = kSeq[1] - 1;
      else if (start >= 0) {
        int j = 1, k = start + 1;
        bool ok = true;
        for (; k < 7; ++k, ++j) {
          if (chist_[static_cast<size_t>(k)] != kSeq[j]) {
            next = chist_[static_cast<size_t>(k)] == 0 ? kSeq[j] - 1 : 4;
            ok = false;
            break;
          }
        }
        if (ok) next = j == 7 ? 6 : kSeq[j] - 1;
      }
      if (operand_w(3) == 0) next = kAlt[std::clamp(next, 0, 7)] - 1;
      store(dst(1), next);
      break;
    }
    // 'U' and default-skip ops advance by their length.
    default: break;
  }
  (void)adv;
  return Step::Next;
}


// ------------------------------------------------------------------------------------------------ battle binding
class ScriptedBrain::BattleHost final : public Host {
 public:
  Battle* battle = nullptr;
  Rng* rng = nullptr;
  std::set<int> unknown;

  u32 alive_mask() const override {
    u32 mask = 0;
    for (int i = 0; i < kUnits; ++i)
      if (battle->unit(i).alive()) mask |= 1u << i;
    return mask;
  }
  u32 roster_mask() const override {
    u32 mask = 0;
    for (int i = 0; i < kUnits; ++i) {
      const Unit& state = battle->unit(i);
      if (state.present && (state.pc || state.alive() || battle->death_pending(i))) mask |= 1u << i;
    }
    return mask;
  }
  u32 present_mask() const override {
    u32 mask = 0;
    for (int i = 0; i < kUnits; ++i)
      if (battle->unit(i).present) mask |= 1u << i;
    return mask;
  }
  int rnd(int lo, int hi) override { return rng->rnd(lo, hi); }
  int libc_rand() override { return rng->libc_rand(); }
  int action_serial() const override { return battle->action_id(); }
  void begin_scripted(int unit, int last_pc) override { battle->dopple_credit(unit, last_pc); }
  int pc_char_id(int pc) const override { return battle->unit(pc).present && battle->unit(pc).pc ? battle->unit(pc).src_id : 0xFF; }
  int monster_id(int unit) const override { return battle->unit(unit).present && !battle->unit(unit).pc ? battle->unit(unit).src_id : 0xFF; }
  int ring_colour(int slot) const override {
    const int colour = battle->ring()[static_cast<size_t>(slot)];
    return colour < 0 || colour > 5 ? 6 : colour;
  }
  int timer_frames() const override { return battle->actions() * 180; }
  void set_menu_cursor(int value) override {
    for (int i = 0; i < kPcSlots; ++i) battle->unit_mut(i).menu_cursor = {static_cast<u8>(value), static_cast<u8>(value)};
  }
  void set_shade(int unit, int value) override {
    if (unit >= 0 && unit < kUnits) battle->unit_mut(unit).shade = static_cast<u8>(value);
  }
  void reveal(int unit) override { battle->set_cast_bit(unit, true); }
  void conceal(int unit) override { battle->set_cast_bit(unit, false); }

  void set_evo_pending(int element) override { battle->set_evo_pending(element); }
  int read_field(int unit, u16 code) override {
    const Unit& state = battle->unit(unit);
    const int off = code & 0x1FF;
    const int base = code & 0x1000 ? 1 : code & 0x2000 ? 2 : 0;
    if (base == 1) {
      switch (off) {
        case 0: return state.countdown;
        case 2: return state.gauge;
        case 0x32: return state.stamina;
        default: break;
      }
    } else if (base == 2) {
      // The derived block: hit bases, weapon / armour / EVD / MAG / MDEF terms and the skip (status immunity) masks.
      switch (off) {
        case 0: case 1: case 2: {
          const int strength = off;
          const int stat = strength == 0 ? state.hit : strength == 1 ? state.hit_med : state.hit_heavy;
          return state.hit_ovr[static_cast<size_t>(strength)] >= 0 ? state.hit_ovr[static_cast<size_t>(strength)] : hit_base(stat, state.eq.wp_acc);
        }
        case 4: return state.eq.wp;
        case 6: return state.eq.arm;
        case 8: return state.evd + state.eq.evd_gear;
        case 0xA: return 1 + state.eq.mag_gear;
        case 0xC: return 1 + state.eq.mdef_gear;
        case 0x10: return static_cast<int>(state.skip_lo & 0xFFFF);
        case 0x12: return static_cast<int>(state.skip_lo >> 16);
        case 0x14: return static_cast<int>(state.skip_hi & 0xFFFF);
        case 0x16: return static_cast<int>(state.skip_hi >> 16);
        default: break;
      }
    } else if (base == 0) {
      switch (off) {
        case 0x01: return state.innate;
        case 0x14: return state.gender;
        case 0xA8: return static_cast<int>(state.perm_lo & 0xFFFF);
        case 0xAA: return static_cast<int>(state.perm_lo >> 16);
        case 0xAC: return static_cast<int>(state.perm_hi & 0xFFFF);
        case 0xAE: return static_cast<int>(state.perm_hi >> 16);
        case 0x16: return state.max_hp;
        case 0x18: return state.atk;
        case 0x19: return state.def;
        case 0x1A: return state.hit;
        case 0x1B: return state.evd;
        case 0x1C: return state.mag;
        case 0x1D: return state.mdef;
        case 0x20: return state.src_id;
        case 0x2E: return state.ap;
        case 0xA2: return state.gold;
        case 0xA0: return state.drop_row;
        case 0xB0:
        case 0x1B0: return static_cast<int>(state.lo & 0xFFFF);
        case 0xB2:
        case 0x1B2: return static_cast<int>(state.lo >> 16);
        case 0xB4:
        case 0x1B4: return static_cast<int>(state.hi & 0xFFFF);
        case 0xB6:
        case 0x1B6: return static_cast<int>(state.hi >> 16);
        case 0xB8: return state.hp;
        case 0x16C: return state.countdown;
        case 0x16E: return state.gauge;
        case 0x19E: return state.stamina;
        case 0x19B:
        case 0x1BF: return state.shade;
        // The kind byte (Pip's form: the PC entities test 0x2B..0x2D before A7).
        case 0x2D: return state.kind;
        default: break;
      }
    }
    unknown.insert(code);
    battle->note_stub(std::format("pac field read {:04X}", code));
    return 0;
  }

  void write_field(int unit, u16 code, int value) override {
    Unit& state = battle->unit_mut(unit);
    const int off = code & 0x1FF;
    const int base = code & 0x1000 ? 1 : code & 0x2000 ? 2 : 0;
    auto set_half = [](u32& word, bool hi, int val) {
      word = hi ? (word & 0xFFFFu) | (static_cast<u32>(val & 0xFFFF) << 16) : (word & 0xFFFF0000u) | static_cast<u32>(val & 0xFFFF);
    };
    // Permanent status halves: the merged words keep their inflicted bits and take the new permanent ones.
    auto set_perm = [&](bool hi_word, bool upper, int val) {
      u32& perm = hi_word ? state.perm_hi : state.perm_lo;
      u32& merged = hi_word ? state.hi : state.lo;
      const u32 old = perm;
      set_half(perm, upper, val);
      merged = (merged & ~old) | perm;
    };
    if (base == 2) {
      switch (off) {
        case 0: case 1: case 2: state.hit_ovr[static_cast<size_t>(off)] = value & 0xFF; return;
        case 4: state.eq.wp = value; return;
        case 6: state.eq.arm = value; return;
        case 8: state.eq.evd_gear = value - state.evd; return;
        case 0xA: state.eq.mag_gear = value - 1; return;
        case 0xC: state.eq.mdef_gear = value - 1; return;
        case 0x10: set_half(state.skip_lo, false, value); return;
        case 0x12: set_half(state.skip_lo, true, value); return;
        case 0x14: set_half(state.skip_hi, false, value); return;
        case 0x16: set_half(state.skip_hi, true, value); return;
        default: break;
      }
    }
    if (base == 1) {
      if (off == 0) {
        state.countdown = value & 0xFFFF;
        return;
      }
      if (off == 2) {
        state.gauge = std::clamp(value, -1, state.gauge_max);
        return;
      }
      if (off == 0x32) {
        state.stamina = value;
        return;
      }
    } else if (base == 0) {
      switch (off) {
        // Op 76 is a raw byte / u16 store: no clamp of HP against max HP in either order.
        // Time Devourer 471 / 472 write HP 30000 BEFORE max HP 30000, so HP must not be clamped to the old max HP.
        case 0x16:
          state.max_hp = std::max(1, value & 0xFFFF);
          return;
        case 0xB8:
          state.hp = value & 0xFFFF;
          if (state.hp == 0 && !state.pc) state.lo |= st::kKO;
          return;
        case 0xA2: state.gold = value; return;
        case 0x01: state.innate = static_cast<u8>(value); return;
        case 0x14: state.gender = static_cast<u16>(value); return;
        case 0xA8: set_perm(false, false, value); return;
        case 0xAA: set_perm(false, true, value); return;
        case 0xAC: set_perm(true, false, value); return;
        case 0xAE: set_perm(true, true, value); return;
        case 0x2E: state.ap = std::max(0, value); return;
        case 0xB0:
        case 0x1B0: set_half(state.lo, false, value); return;
        case 0xB2:
        case 0x1B2: set_half(state.lo, true, value); return;
        case 0xB4:
        case 0x1B4: set_half(state.hi, false, value); return;
        case 0xB6:
        case 0x1B6: set_half(state.hi, true, value); return;
        case 0x16C: state.countdown = value & 0xFFFF; return;
        case 0x16E: state.gauge = std::clamp(value, -1, state.gauge_max); return;
        case 0x19E: state.stamina = value; return;
        case 0x19B:
        case 0x1BF: state.shade = static_cast<u8>(value); return;
        default: break;
      }
    }
    unknown.insert(0x10000 | code);
    battle->note_stub(std::format("pac field write {:04X}", code));
  }

  PickResult pick_element(int unit, int limit) override {
    PickResult result;
    if (unit < 0 || unit >= kUnits) return result;
    Unit& state = battle->unit_mut(unit);
    // Ops 0x55 / 0x8F draw a cell of rows 0..gauge (0x8F: rows 0..limit) with rnd(0, cells - 1) until one is free (usable, unused, no
    // tech flag) and holds an element id < 0x7E whose record has byte 0x14 == 0xFF, (byte 0 & 0x44) == 0 and (byte 4 & 0x41) == 0, and a
    // target is found for it; a free cell that fails is closed and the draw goes on. Techs from id 126 up (Big Catch 192, Dopplegang 238,
    // the enemy-only techs ...) are only ever cast by an explicit queue_attack.
    const int rows = std::clamp((limit < 0 ? state.gauge : limit) + 1, 0, data::kGridRows);
    const int cells = rows * data::kGridCols;
    std::array<bool, data::kGridCells> open{};
    int left = 0;
    for (int cell = 0; cell < cells; ++cell) {
      const auto index = static_cast<size_t>(cell);
      open[index] = state.usable[index] && !state.used[index] && !(state.cells[index] & 0xC000);
      left += open[index];
    }
    const auto& elements = battle->tables().elements;
    while (left > 0) {
      const auto cell = static_cast<size_t>(rng->rnd(0, cells - 1));
      if (!open[cell]) continue;
      const int id = state.cells[cell] & 0x3FFF;
      if (id < 0x7E && id < static_cast<int>(elements.size())) {
        const auto& rec = elements[static_cast<size_t>(id)];
        if (rec.raw[0x14] == 0xFF && (rec.raw[0] & 0x44) == 0 && (rec.raw[4] & 0x41) == 0) {
          const u32 mask = element_target(rec.raw[4], limit >= 0, unit);
          if (mask != 0xFFFF) {
            state.used[cell] = true;
            if (limit < 0) state.gauge = std::max(-1, state.gauge - (static_cast<int>(cell) / data::kGridCols + 1));
            result.element = id;
            result.mask = mask;
            return result;
          }
        }
      }
      open[cell] = false;
      --left;
    }
    return result;
  }

  // The target of a picked element (record byte 4): bit 0x10 = every alive unit of the far side (op 0x8F: the PCs, op 0x55: the enemy
  // slots); else rnd(0, 2) (0x8F) / rnd(0, 8) (0x55) until an alive, targetable unit other than the caster comes up; 0xFFFF once every
  // unit was drawn without success.
  u32 element_target(u8 spec, bool pcs_only, int caster) {
    const u32 alive = alive_mask();
    if (spec & 0x10) return alive & (pcs_only ? 0x7u : 0x1F8u);
    const int last = pcs_only ? kPcSlots - 1 : kUnits - 1;
    u32 range = 0, tried = 0;
    for (int i = 0; i <= last; ++i) range |= 1u << i;
    while ((tried & range) != range) {
      const int target = rng->rnd(0, last);
      if ((alive >> target & 1u) && target != caster && !battle->untargetable(target)) return 1u << target;
      tried |= 1u << target;
    }
    return 0xFFFF;
  }
};

ScriptedBrain::ScriptedBrain() = default;
ScriptedBrain::ScriptedBrain(Loader loader) : loader_(std::move(loader)) {}
ScriptedBrain::~ScriptedBrain() = default;

EnemyAction ScriptedBrain::choose(const Battle& battle, int unit, Rng& rng) { return fallback_.choose(battle, unit, rng); }

const std::set<int>& ScriptedBrain::unknown_fields() const { return host_ ? host_->unknown : no_fields_; }

std::vector<Event> ScriptedBrain::drain_events() {
  if (vm_) {
    auto drained = vm_->drain_events();
    events_.insert(events_.end(), drained.begin(), drained.end());
  }
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

void ScriptedBrain::begin_battle(Battle& battle, Rng& rng) {
  vm_.reset();
  host_.reset();
  module_.reset();
  events_.clear();
  stats_ = {};
  last_cast_ = {-1, -1, -1};
  if (!enabled_) return;
  Bytes data;
  if (auto it = forced_.find(battle.scene()); it != forced_.end()) data = it->second;
  else if (loader_) {
    if (auto loaded = loader_(battle.scene())) data = std::move(*loaded);
  }
  if (data.empty()) return;
  module_ = std::make_unique<Module>();
  if (!module_->parse(std::move(data))) {
    module_.reset();
    return;
  }
  host_ = std::make_unique<BattleHost>();
  host_->battle = &battle;
  host_->rng = &rng;
  vm_ = std::make_unique<Vm>(*module_, *host_);
  // The script vars 0x60..0x7F start as the save's Dopplegang pool (scripts read it through the vars; the live mask is the Battle's)
  for (int word = 0; word < 16; ++word) vm_->set_var(0x60 + 2 * word, battle.dopple_mask()[static_cast<size_t>(word)]);
  {
    const auto& sc = battle.tables().scenes[static_cast<size_t>(battle.scene())];
    u32 hide = 0, owed = 0;
    for (int unit = kPcSlots; unit < kUnits; ++unit) {
      if (!battle.unit(unit).present) continue;
      hide |= (static_cast<u32>(sc.raw[0x1B]) << 3 & (1u << unit));
      owed |= (static_cast<u32>(sc.raw[0x10]) << 3 & (1u << unit));
    }
    // The scene record's bytes +0x1B / +0x10 put the listed enemy slots off-stage / owing their death from the first frame.
    vm_->seed_scene(hide, owed);
  }
  // Slot 0 of every entity.
  vm_->tick();
  vm_->mark();
  // The idle loops run on virtual time (2D delays count, e.g. `2C, delay 60, E0 0` message): the events carry their ticks.
  vm_->settle();
}

// One slot run on virtual time, then the settle pass (the other entities' slot-1 loops react to what the slot did).
bool ScriptedBrain::run(int unit, int slot, std::vector<QueuedAttack>& queued) {
  vm_->mark();
  if (!vm_->run_slot(unit, slot, queued, 30000)) return false;
  vm_->settle();
  return true;
}

bool ScriptedBrain::plan_turn(Battle& battle, int unit, Rng& rng, std::vector<EnemyAction>& out) {
  if (!vm_) return false;
  host_->battle = &battle;
  host_->rng = &rng;
  ++stats_.turns;
  if (!vm_->has_slot(unit, 2)) {
    ++stats_.fallbacks;
    return false;
  }
  // Slot 1 of every entity ("each time the unit may act").
  vm_->tick();
  std::vector<QueuedAttack> queued;
  if (!run(unit, 2, queued)) {
    ++stats_.timeouts;
    return false;
  }
  to_actions(battle, queued, out);
  if (out.empty()) ++stats_.empty_plans;
  return true;
}

// Queue entries (op 67) -> actions: a weapon entry attacks each unit of its target mask in turn; an element entry is one cast aimed at
// every unit of the mask (the field ring and the cast-colour history take it once).
void ScriptedBrain::to_actions(const Battle& battle, const std::vector<QueuedAttack>& queued, std::vector<EnemyAction>& out) {
  for (const QueuedAttack& attack : queued) {
    // Countdown reset: the core does it.
    if (attack.type == 4) continue;
    const u32 mask = attack.mask & ((1u << kUnits) - 1);
    if (!mask) continue;
    EnemyAction ea;
    ea.seq = attack.seq;
    ea.group = attack.group;
    if (attack.type == 1 || attack.type == 2 || attack.type == 3 || attack.type == 6) {
      if (attack.value <= 0 || attack.value >= static_cast<int>(battle.tables().elements.size())) continue;
      ea.kind = EnemyAction::Kind::Element;
      ea.element = attack.value;
      ea.target = std::countr_zero(mask);
      ea.targets = mask;
      out.push_back(ea);
    } else if (attack.type == 0 || attack.type == 5) {
      ea.kind = EnemyAction::Kind::Weapon;
      ea.strength = attack.value;
      for (int unit = 0; unit < kUnits; ++unit) {
        if (!(mask >> unit & 1u)) continue;
        ea.target = unit;
        out.push_back(ea);
      }
    }
  }
}

void ScriptedBrain::take_idle_plans(Battle& battle, std::vector<std::pair<int, std::vector<EnemyAction>>>& out) {
  if (!vm_) return;
  std::vector<std::pair<int, std::vector<QueuedAttack>>> by_unit;
  for (const auto& [unit, attack] : vm_->take_idle_attacks()) {
    if (by_unit.empty() || by_unit.back().first != unit) by_unit.emplace_back(unit, std::vector<QueuedAttack>{});
    by_unit.back().second.push_back(attack);
  }
  for (const auto& [unit, queued] : by_unit) {
    std::vector<EnemyAction> plan;
    to_actions(battle, queued, plan);
    if (!plan.empty()) out.emplace_back(unit, std::move(plan));
  }
}

std::array<u8, 7> ScriptedBrain::colour_sequence() const {
  std::array<u8, 7> sequence{};
  if (vm_) std::copy_n(vm_->colour_history().begin(), sequence.size(), sequence.begin());
  return sequence;
}

void ScriptedBrain::on_attack(Battle& battle, int actor, int kind, int id, int target) {
  if (!vm_) return;
  if (kind == 1 && id > 0) {
    // One history entry per cast (the core calls this once per target).
    // Var 0x52 takes the colour of every element with a colour byte; the history skips record byte 0 bit 0x80 (seq_colour).
    const std::tuple<int, int, int> cast{battle.actions(), actor, id};
    if (cast != last_cast_) {
      last_cast_ = cast;
      const u8 colour_mask = battle.element_colour(id) & 0xFC;
      const int colour = colour_mask == 0xFC ? 6 : data::colour_index_of_bit(colour_mask);
      if (const int entry = battle.seq_colour(id); entry >= 0) vm_->note_cast_colour(entry);
      else if (colour >= 0) vm_->set_var(0x52, colour);
    }
  }
  // Reserved vars: last attack type / value (weapon attack: the hit strength 0..2; element: its id) / target mask; global at 0x20, per PC at
  // 0x26 + 6 * pc.
  for (const int off : {0x20, 0x26 + 6 * actor}) {
    if (off != 0x20 && actor >= kPcSlots) break;
    vm_->set_var(off, kind);
    vm_->set_var(off + 2, id);
    vm_->set_var(off + 4, 1 << target);
  }
  // Var 0x58 (set at the PC command confirm): 0 for a weapon attack, else the element's byte 4 & 0x10 (hits its whole range);
  // the Boxer Boys scripts read it as the "cheater" test.
  // Var 0x50 (a unit index): the PC whose command was confirmed last. Enemy turns never change it, so the Boxer Boys' `bit_set v46, v50` +
  // `queue_attack` ("presents") aims at the PC that attacked.
  if (actor < kPcSlots) vm_->set_var(0x50, actor);
  if (actor < kPcSlots) {
    const bool all = kind == 1 && id > 0 && id < static_cast<int>(battle.tables().elements.size()) && (battle.tables().elements[static_cast<size_t>(id)].b(4) & 0x10);
    vm_->set_var(0x58, all ? 0x10 : 0);
  }
}

void ScriptedBrain::on_killed(Battle& battle, int unit, Rng& rng, std::vector<EnemyAction>& out) {
  if (!vm_) return;
  host_->battle = &battle;
  host_->rng = &rng;
  std::vector<QueuedAttack> queued;
  run(unit, 3, queued);
  // The script queues the death element on the dying unit (op 67, mask 0x10 = itself); the executor runs it.
  to_actions(battle, queued, out);
}

void ScriptedBrain::on_attacked(Battle& battle, int unit, Rng& rng, std::vector<EnemyAction>& out) {
  if (!vm_) return;
  host_->battle = &battle;
  host_->rng = &rng;
  std::vector<QueuedAttack> queued;
  run(unit, 4, queued);
  to_actions(battle, queued, out);
  stats_.reaction_attacks += static_cast<int>(out.size());
}

}  // namespace oc::battle::pac
