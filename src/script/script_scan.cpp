// Static room-script scan. Each entity script slot is walked depth first over both sides of every branch, through CALLs (a
// four-entry return stack, as the VM keeps) and gotos. The walk carries the tests that hold on its path; where paths meet, only
// the tests common to all of them are kept (a position is walked again while that set shrinks), so an event's tests are the ones
// that hold on every path to it. It also carries the variables the path set to a literal (kept where paths meet when all agree). A
// branch decided by two literals takes only its live side; a write to a variable drops the tests on it. Item, battle and character
// operands that read a variable are resolved from the path, else from the literal the entity's INIT writes to it, else from every
// literal the room writes to it when the room never writes it a computed value.
#include "script/script_scan.h"

#include <algorithm>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace oc::script {

namespace {

constexpr int kRoomVarBase = 0x400;
constexpr int kChoiceVar = 0x04;
constexpr size_t kStackDepth = 4;
constexpr u32 kWalkCap = 200000;
// Instructions the flag search follows after a give.
constexpr int kFlagSearch = 48;
// Engine scratch variables (room parameter, party, dialogue arguments, results): never a remembered flag.
constexpr int kFirstFlagVar = 0x50;
// Pad bits of L1 and R1 in the field pad word.
constexpr int kShoulderButtons = 0x0004 | 0x0008;
// Character reference of the entity running the script.
constexpr int kThisEntity = 0xFB;
// The stock cap of an item or element.
constexpr int kStackCap = 99;
constexpr int kUndecoded = -1;
constexpr int kUndecodable = -2;
// The story counter and ROOM_PARAM (the entrance a room was entered by), and the largest value a test of them is read against.
constexpr int kStoryVar = 0x00;
constexpr int kEntranceVar = 0x02;
constexpr int kTopValue = 0x7FFF;
// Instructions an INIT evaluation walks before it gives up (and calls the entity active).
constexpr u32 kInitWalkCap = 20000;

struct Const {
  int var;
  int value;
};

// `consts` is sorted by variable.
struct Path {
  u32 pc = 0;
  std::vector<u32> stack;
  std::vector<Cond> conds;
  std::vector<Const> consts;
};

std::vector<Const>::const_iterator const_at(const std::vector<Const>& consts, int var) {
  return std::lower_bound(consts.begin(), consts.end(), var, [](const Const& entry, int key) { return entry.var < key; });
}

std::optional<int> find_const(const std::vector<Const>& consts, int var) {
  const auto found = const_at(consts, var);
  return found != consts.end() && found->var == var ? std::optional<int>(found->value) : std::nullopt;
}

void set_const(std::vector<Const>& consts, int var, std::optional<int> value) {
  const auto found = consts.begin() + (const_at(consts, var) - consts.begin());
  const bool present = found != consts.end() && found->var == var;
  if (value && present) found->value = *value;
  else if (value) consts.insert(found, {var, *value});
  else if (present) consts.erase(found);
}

// Order of (variable, value) entries; a list sorted by variable is sorted by it too.
bool const_before(const Const& left, const Const& right) {
  return left.var < right.var || (left.var == right.var && left.value < right.value);
}

// The entries of two sorted constant lists that agree.
std::vector<Const> common_consts(const std::vector<Const>& lhs, const std::vector<Const>& rhs) {
  std::vector<Const> out;
  std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(out), const_before);
  return out;
}

Cmp negate(Cmp cmp) {
  switch (cmp) {
    case Cmp::Eq: return Cmp::Ne;
    case Cmp::Ne: return Cmp::Eq;
    case Cmp::Gt: return Cmp::Le;
    case Cmp::Lt: return Cmp::Ge;
    case Cmp::Ge: return Cmp::Lt;
    case Cmp::Le: return Cmp::Gt;
    case Cmp::AnyBit: return Cmp::NoBit;
    case Cmp::NoBit: return Cmp::AnyBit;
  }
  return cmp;
}

// x <cmp> y read as y <mirror> x.
Cmp mirror(Cmp cmp) {
  switch (cmp) {
    case Cmp::Gt: return Cmp::Lt;
    case Cmp::Lt: return Cmp::Gt;
    case Cmp::Ge: return Cmp::Le;
    case Cmp::Le: return Cmp::Ge;
    default: return cmp;
  }
}

bool holds(int lhs, Cmp cmp, int rhs) {
  switch (cmp) {
    case Cmp::Eq: return lhs == rhs;
    case Cmp::Ne: return lhs != rhs;
    case Cmp::Gt: return lhs > rhs;
    case Cmp::Lt: return lhs < rhs;
    case Cmp::Ge: return lhs >= rhs;
    case Cmp::Le: return lhs <= rhs;
    case Cmp::AnyBit: return (lhs & rhs) != 0;
    case Cmp::NoBit: return (lhs & rhs) == 0;
  }
  return false;
}

// IF_JUMP condition codes 0..A; nullopt for the two that are not a comparison of the left operand (8: x | y, A: ~x & y).
std::optional<Cmp> if_jump_cmp(int code) {
  switch (code) {
    case 0: return Cmp::Eq;
    case 1:
    case 7: return Cmp::Ne;
    case 2: return Cmp::Gt;
    case 3: return Cmp::Lt;
    case 4: return Cmp::Ge;
    case 5: return Cmp::Le;
    case 6:
    case 9: return Cmp::AnyBit;
    default: return std::nullopt;
  }
}

// A decoded instruction with its value operands (the mode bytes left out) and the variables its V operands name. Kept at a fixed
// address: `values` points into `ins.ops`.
struct Decoded {
  Insn ins;
  std::vector<const Operand*> values;
  std::vector<int> dests;
};

// `is_dest[i]`: operand i is a V (destination variable) operand.
void index_operands(Decoded& decoded, const std::vector<bool>& is_dest) {
  const auto& ops = decoded.ins.ops;
  for (size_t i = 0; i < ops.size(); ++i) {
    if (ops[i].kind != OperandKind::Mode) decoded.values.push_back(&ops[i]);
    if (i < is_dest.size() && is_dest[i]) decoded.dests.push_back(ops[i].value);
  }
}

Subject var_subject(int var) { return var == kChoiceVar ? Subject::Choice : Subject::Var; }

// Successors of a branch: the fall-through side and the jump side, each with the test that holds on it, or a fixed outcome when
// two literals decide the branch. Path constants decide nothing: a loop counter set before its loop would close the exit.
struct Branch {
  std::optional<Cond> fall, jump;
  bool fall_live = true, jump_live = true;
};

Branch branch_of(const Decoded& decoded) {
  Branch branch;
  const Insn& ins = decoded.ins;
  const auto& ops = decoded.values;
  auto both = [&](Subject subject, int id, int fall_value, int jump_value) {
    branch.fall = Cond{subject, id, Cmp::Eq, fall_value};
    branch.jump = Cond{subject, id, Cmp::Eq, jump_value};
  };
  // Scenario ops: the jump side's comparison of variable 0 with U.
  auto scenario = [&](Cmp jump_cmp) {
    if (ops.empty() || ops[0]->kind != OperandKind::Imm) return;
    branch.jump = Cond{Subject::Var, 0, jump_cmp, ops[0]->value};
    branch.fall = Cond{Subject::Var, 0, negate(jump_cmp), ops[0]->value};
  };
  switch (ins.key) {
    case 0x02: {
      const auto cmp = if_jump_cmp(ins.cond);
      if (!cmp || ops.size() < 2) break;
      if (ops[0]->kind == OperandKind::Imm && ops[1]->kind == OperandKind::Imm) {
        branch.fall_live = holds(ops[0]->value, *cmp, ops[1]->value);
        branch.jump_live = !branch.fall_live;
        break;
      }
      if (ops[0]->kind == OperandKind::Var && ops[1]->kind == OperandKind::Imm) {
        branch.fall = Cond{var_subject(ops[0]->value), ops[0]->value, *cmp, ops[1]->value};
      } else if (ops[0]->kind == OperandKind::Imm && ops[1]->kind == OperandKind::Var) {
        branch.fall = Cond{var_subject(ops[1]->value), ops[1]->value, mirror(*cmp), ops[0]->value};
      } else if (ops[0]->kind == OperandKind::Var && ops[1]->kind == OperandKind::Var) {
        branch.fall = Cond{var_subject(ops[0]->value), ops[0]->value, *cmp, ops[1]->value, true};
      }
      if (branch.fall) {
        branch.jump = *branch.fall;
        branch.jump->cmp = negate(branch.fall->cmp);
      }
      break;
    }
    case 0x50:
    case 0x51:
      if (!ops.empty()) {
        branch.fall = Cond{Subject::Key, 0, Cmp::AnyBit, ops[0]->value & 0xFFFF};
        branch.jump = Cond{Subject::Key, 0, Cmp::NoBit, ops[0]->value & 0xFFFF};
        // Held L1 / R1 (op 50, not the key latch of 51 that the code puzzles read) opens the developers' menus (room warps, party
        // and story setters) that nearly every room carries.
        if (ins.key == 0x50 && (ops[0]->value & kShoulderButtons)) branch.fall_live = false;
      }
      break;
    case 0x69: scenario(Cmp::Ge); break;
    case 0x6A: scenario(Cmp::Le); break;
    case 0x6B: scenario(Cmp::Ne); break;
    case 0x73: {
      if (ops.empty() || ops[0]->kind != OperandKind::Imm) break;
      const int bit_ref = ops[0]->value & 0xFFFF;
      const int var = (bit_ref >> 5) * 2, mask = 1 << (bit_ref & 0xF);
      branch.fall = Cond{Subject::Var, var, Cmp::AnyBit, mask};
      branch.jump = Cond{Subject::Var, var, Cmp::NoBit, mask};
      break;
    }
    case 0x94:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) both(Subject::InParty, ops[0]->value, 1, 0);
      break;
    case 0x95:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) both(Subject::Recruited, ops[0]->value, 1, 0);
      break;
    case 0x9A:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) both(Subject::Leader, ops[0]->value, 0, 1);
      break;
    case 0xB4:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) both(Subject::Item, ops[0]->value, 1, 0);
      break;
    case 0xBC: {
      if (ops.size() < 4) break;
      int threshold = 0;
      for (int i = 0; i < 4; ++i) threshold |= (ops[static_cast<size_t>(i)]->value & 0xFF) << (8 * i);
      branch.fall = Cond{Subject::Money, 0, Cmp::Ge, threshold};
      branch.jump = Cond{Subject::Money, 0, Cmp::Lt, threshold};
      break;
    }
    case 0xCD: scenario(Cmp::Gt); break;
    case 0xCE: scenario(Cmp::Lt); break;
    case 0xE5:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) both(Subject::Carryover, ops[0]->value, 1, 0);
      break;
    case 0xE6:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) both(Subject::RecruitedOrCarryover, ops[0]->value, 1, 0);
      break;
    case 0xE8:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) branch.fall = Cond{Subject::Carryover, ops[0]->value, Cmp::Eq, 1};
      break;
    default: break;
  }
  return branch;
}

// The flag a write sets, if it is one: a single bit or a literal value of a global variable past the engine's scratch range.
std::optional<ScanFlag> flag_write(const Decoded& decoded) {
  const Insn& ins = decoded.ins;
  const auto& ops = decoded.values;
  auto global = [](int var) { return var >= kFirstFlagVar && var < kRoomVarBase; };
  switch (ins.key) {
    case 0x10:
      if (ops.size() >= 2 && ops[1]->kind == OperandKind::Imm && global(ops[0]->value)) return ScanFlag{ops[0]->value, -1, ops[1]->value};
      break;
    case 0x11:
      if (!ops.empty() && global(ops[0]->value)) return ScanFlag{ops[0]->value, -1, 1};
      break;
    case 0x15:
      if (ops.size() >= 2 && ops[1]->kind == OperandKind::Imm && global(ops[0]->value) && (ops[1]->value & 31) < 16)
        return ScanFlag{ops[0]->value, ops[1]->value & 31, 0};
      break;
    case 0x1A:
      if (ops.size() >= 2 && ops[1]->kind == OperandKind::Imm && global(ops[0]->value) && (ops[1]->value & 0xFFFF) != 0) {
        const int mask = ops[1]->value & 0xFFFF;
        int bit = 0;
        while (!((mask >> bit) & 1)) ++bit;
        return ScanFlag{ops[0]->value, bit, 0};
      }
      break;
    case 0x71:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) {
        const int bit_ref = ops[0]->value & 0xFFFF;
        const int var = (bit_ref >> 5) * 2;
        if (global(var)) return ScanFlag{var, bit_ref & 0xF, 0};
      }
      break;
    default: break;
  }
  return std::nullopt;
}

// Calls write(var, value) for every variable the instruction writes; value is the literal or known path constant written, nullopt
// for a computed one.
template <typename Write>
void for_each_write(const Decoded& decoded, const std::vector<Const>& consts, Write&& write) {
  const auto& ops = decoded.values;
  switch (decoded.ins.key) {
    case 0x10:
      if (ops.size() >= 2)
        write(ops[0]->value, ops[1]->kind == OperandKind::Imm ? std::optional<int>(ops[1]->value) : find_const(consts, ops[1]->value));
      break;
    case 0x11:
      if (!ops.empty()) write(ops[0]->value, std::optional<int>(1));
      break;
    case 0x12:
      if (!ops.empty()) write(ops[0]->value, std::optional<int>(0));
      break;
    case 0x71:
    case 0x72:
      if (!ops.empty() && ops[0]->kind == OperandKind::Imm) write(((ops[0]->value & 0xFFFF) >> 5) * 2, std::optional<int>());
      break;
    case 0xE1: write(kChoiceVar, std::optional<int>()); break;
    default:
      for (int var : decoded.dests) write(var, std::optional<int>());
      break;
  }
}

int operand_bias(u16 op) { return op == 0xC9 ? 0x200 : 0; }

class Scanner {
 public:
  explicit Scanner(const FieldScriptModule& module) : module_(module), index_(module.code.size(), kUndecoded) {}

  RoomScanResult run() {
    const size_t count = module_.entity_count();
    result_.entities.assign(count, EntityScan{});
    init_literals_.resize(count);
    find_meeting_points();
    for (size_t entity = 0; entity < count; ++entity) {
      std::set<u16> walked;
      for (int slot = 0; slot < kScriptSlots; ++slot) {
        const u16 ptr = module_.script_ptr(entity, static_cast<size_t>(slot));
        if (!ptr || !walked.insert(ptr).second) continue;
        walk(static_cast<int>(entity), slot, ptr);
      }
    }
    resolve();
    for (const auto& [site, conds] : hide_sites_) result_.entities[static_cast<size_t>(site.first)].inactive_when.push_back(conds);
    for (const int entity : reactivated_) result_.entities[static_cast<size_t>(entity)].reactivated = true;
    for (size_t entity = 0; entity < count; ++entity)
      if (const u16 ptr = module_.script_ptr(entity, 0)) result_.entities[entity].inactive_at = inactive_regions(static_cast<int>(entity), ptr);
    result_.written.assign(written_.begin(), written_.end());
    for (ScanEvent& event : result_.events) {
      event.capacity_check = std::any_of(event.conds.begin(), event.conds.end(), [](const Cond& cond) {
        return cond.subject == Subject::Var && cond.id >= kRoomVarBase && !cond.value_var && cond.value == kStackCap &&
               (cond.cmp == Cmp::Ne || cond.cmp == Cmp::Lt);
      });
      if (!event.flag.valid()) continue;
      for (const Cond& cond : event.conds)
        if (tests_flag(cond, event.flag)) event.flag_guarded = true;
      for (const Cond& cond : init_tests_)
        if (tests_flag(cond, event.flag)) event.flag_at_load = true;
    }
    std::sort(result_.events.begin(), result_.events.end(),
              [](const ScanEvent& lhs, const ScanEvent& rhs) { return std::tie(lhs.entity, lhs.offset) < std::tie(rhs.entity, rhs.offset); });
    return std::move(result_);
  }

 private:
  // Marks every position more than one path can reach: script entries, jump and call targets, and the positions after a CALL.
  // Everywhere else a path only arrives from the instruction before, so the walk keeps no state there.
  void find_meeting_points() {
    meeting_.assign(module_.code.size(), false);
    std::vector<bool> reached(module_.code.size(), false);
    std::vector<u32> todo;
    auto mark = [&](u32 off) {
      if (off >= module_.code.size()) return;
      meeting_[off] = true;
      if (!reached[off]) todo.push_back(off);
    };
    for (const auto& scripts : module_.entities)
      for (u16 ptr : scripts)
        if (ptr) mark(ptr);
    while (!todo.empty()) {
      u32 pc = todo.back();
      todo.pop_back();
      while (pc < module_.code.size() && !reached[pc]) {
        reached[pc] = true;
        const Decoded* entry = decoded(pc);
        if (!entry) break;
        const Insn& ins = entry->ins;
        for (const auto& [target, is_call] : ins.targets) mark(target);
        if (ins.key == 0x05) mark(ins.off + ins.length);
        if (ins.flow != Flow::Next) break;
        pc = ins.off + ins.length;
      }
    }
  }

  const Decoded* decoded(u32 off) {
    if (off >= index_.size()) return nullptr;
    if (index_[off] == kUndecoded) {
      auto ins = decode_insn(module_.code, off);
      if (!ins) {
        index_[off] = kUndecodable;
        return nullptr;
      }
      Decoded& added = decoded_.emplace_back();
      added.ins = std::move(*ins);
      index_operands(added, dest_layout(added.ins));
      index_[off] = static_cast<int>(decoded_.size() - 1);
    }
    return index_[off] >= 0 ? &decoded_[static_cast<size_t>(index_[off])] : nullptr;
  }

  // Which operands of the instruction's layout are V operands; the layout depends only on the row and the selector byte after the
  // opcode, so it is resolved once per pair.
  const std::vector<bool>& dest_layout(const Insn& ins) {
    const size_t selector_at = ins.off + (ins.key > 0xFF ? 2 : 1);
    const u8 selector = selector_at < module_.code.size() ? module_.code[selector_at] : u8{0};
    auto [found, added] = dest_layouts_.try_emplace({ins.info->spec, selector});
    if (added)
      for (std::string_view token : resolve_layout(*ins.info, module_.code, ins.off)) found->second.push_back(token == "V");
    return found->second;
  }

  static bool tests_flag(const Cond& cond, const ScanFlag& flag) {
    if (cond.subject != Subject::Var || cond.id != flag.var || cond.value_var) return false;
    if (flag.bit >= 0) return (cond.cmp == Cmp::AnyBit || cond.cmp == Cmp::NoBit) && ((cond.value >> flag.bit) & 1) != 0;
    return cond.cmp != Cmp::AnyBit && cond.cmp != Cmp::NoBit;
  }

  // Per meeting point and return context: the tests and constants that hold on every path walked there so far.
  using MeetState = std::unordered_map<u64, std::pair<std::vector<Cond>, std::vector<Const>>>;

  // A path arriving at a meeting point: true when it brings something new (the first arrival, or the common tests and constants
  // shrank, which the path then carries on with); false when the walk has already been there with no more than it brings.
  static bool meet(MeetState& seen, Path& path) {
    const u64 key = static_cast<u64>(path.pc) | (static_cast<u64>(path.stack.size()) << 16) |
                    (static_cast<u64>(path.stack.empty() ? 0 : path.stack.back()) << 20);
    auto [state, first] = seen.try_emplace(key, path.conds, path.consts);
    if (first) return true;
    auto& [conds, consts] = state->second;
    const auto has_cond = [&](const Cond& cond) { return std::find(path.conds.begin(), path.conds.end(), cond) != path.conds.end(); };
    if (std::all_of(conds.begin(), conds.end(), has_cond) &&
        std::includes(path.consts.begin(), path.consts.end(), consts.begin(), consts.end(), const_before))
      return false;
    std::vector<Cond> common_conds;
    std::copy_if(conds.begin(), conds.end(), std::back_inserter(common_conds), has_cond);
    std::vector<Const> agreed = common_consts(consts, path.consts);
    conds = common_conds;
    consts = agreed;
    path.conds = std::move(common_conds);
    path.consts = std::move(agreed);
    return true;
  }

  void walk(int entity, int slot, u32 start) {
    MeetState seen;
    // Pending paths as a min-heap on the position: walking in code order lets most meeting paths arrive before the code after the
    // meeting point is walked, so few positions are walked twice.
    std::vector<Path> todo;
    const auto later = [](const Path& lhs, const Path& rhs) { return lhs.pc > rhs.pc; };
    auto push = [&](Path&& path) {
      todo.push_back(std::move(path));
      std::push_heap(todo.begin(), todo.end(), later);
    };
    push(Path{start, {}, {}, {}});
    u32 steps = 0;
    while (!todo.empty() && steps < kWalkCap) {
      std::pop_heap(todo.begin(), todo.end(), later);
      Path path = std::move(todo.back());
      todo.pop_back();
      for (;;) {
        if (!todo.empty() && todo.front().pc < path.pc) {
          push(std::move(path));
          break;
        }
        if (path.pc >= meeting_.size() || (meeting_[path.pc] && !meet(seen, path))) break;
        const Decoded* entry = decoded(path.pc);
        if (!entry) break;
        const Insn* ins = &entry->ins;
        ++steps;
        visit(entity, slot, *entry, path);
        const u32 next = ins->off + ins->length;
        if (ins->key == 0x0D) {
          if (path.stack.empty()) break;
          path.pc = path.stack.back();
          path.stack.pop_back();
          continue;
        }
        if (ins->flow == Flow::End) break;
        if (ins->flow == Flow::Goto) {
          if (ins->targets.empty()) break;
          path.pc = ins->targets.front().first;
          continue;
        }
        if (ins->key == 0x05) {
          if (path.stack.size() >= kStackDepth || ins->targets.empty()) break;
          path.stack.push_back(next);
          path.pc = ins->targets.front().first;
          continue;
        }
        if (ins->targets.empty()) {
          path.pc = next;
          continue;
        }
        Branch branch = branch_of(*entry);
        if (branch.jump_live) {
          Path other = path;
          other.pc = ins->targets.front().first;
          add_cond(other, branch.jump);
          push(std::move(other));
        }
        if (!branch.fall_live) break;
        add_cond(path, branch.fall);
        path.pc = next;
      }
    }
    result_.steps += steps;
  }

  void add_cond(Path& path, const std::optional<Cond>& cond) {
    if (!cond) return;
    if (std::find(path.conds.begin(), path.conds.end(), *cond) == path.conds.end()) path.conds.push_back(*cond);
    if (cond->subject == Subject::Var && cond->cmp == Cmp::Eq && !cond->value_var) set_const(path.consts, cond->id, cond->value);
  }

  void visit(int entity, int slot, const Decoded& entry, Path& path) {
    const Insn& ins = entry.ins;
    const auto& ops = entry.values;
    if (slot == 0) {
      EntityScan& own = result_.entities[static_cast<size_t>(entity)];
      if (own.role == EntityRole::None) {
        if (ins.key == 0x0E) own.role = EntityRole::Party;
        if (ins.key == 0x0F) own.role = EntityRole::Npc;
        if (ins.key == 0xD1) own.role = EntityRole::Monster;
      }
      const bool hides_self = ins.key == 0x36 && ins.ops.size() >= 2 && ins.ops[0].value == 0 &&
                              (ins.ops[1].value == kThisEntity || ins.ops[1].value == entity);
      if (hides_self) hide_sites_[{entity, ins.off}] = path.conds;
      if (!ins.targets.empty() && ins.key != 0x05 && ins.flow == Flow::Next) {
        const Branch branch = branch_of(entry);
        for (const auto& cond : {branch.fall, branch.jump})
          if (cond) init_tests_.insert(*cond);
      }
    }
    if (ins.key == 0x36 && ins.ops.size() >= 2 && ins.ops[0].value != 0) {
      const int target = ins.ops[1].value == kThisEntity ? entity : ins.ops[1].value;
      if (target < static_cast<int>(result_.entities.size())) reactivated_.insert(target);
    }
    switch (ins.key) {
      case 0x5A:
      case 0xDC:
        if (ScanEvent* added = add_event(EventKind::Exit, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]))
          if (ops.size() >= 2 && ops[1]->kind == OperandKind::Imm) added->entrance = ins.key == 0x5A ? ops[1]->value & 0xFF : ops[1]->value;
        break;
      case 0x6C: add_event(EventKind::Scenario, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]); break;
      case 0x7B: add_event(EventKind::Join, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]); break;
      case 0x96: add_event(EventKind::Roster, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]); break;
      case 0x97: add_event(EventKind::Unroster, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]); break;
      case 0xA3: add_event(EventKind::Battle, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]); break;
      case 0xBB:
        if (ops.size() >= 2)
          add_event(ops[0]->value == 0 ? EventKind::Give : EventKind::Take, entity, slot, ins, path, ops[1]);
        break;
      case 0xBD:
        if (ops.size() >= 2 && ops[0]->value == 0) add_event(EventKind::Gold, entity, slot, ins, path, ops[1]);
        break;
      case 0xC9: add_event(EventKind::Battle, entity, slot, ins, path, ops.empty() ? nullptr : ops[0]); break;
      default: break;
    }
    if (ins.key == 0x10 && ops.size() >= 2 && ops[0]->value >= kRoomVarBase && ops[1]->kind == OperandKind::Imm)
      literal_sites_[{ops[0]->value, ops[1]->value}].insert(ops[1]->pos);
    if (const auto written = flag_write(entry))
      if (ScanEvent* added = add_event(EventKind::SetFlag, entity, slot, ins, path, nullptr)) added->flag = *written;
    if ((ins.key == 0x71 || ins.key == 0x72) && !ops.empty() && ops[0]->kind != OperandKind::Imm) result_.writes_any = true;
    if (ins.key == 0x65 || ins.key == 0x77 || ins.key == 0xFE48)
      for (const Operand* operand : ops)
        if (operand->kind == OperandKind::Word && operand->value < kRoomVarBase) written_.insert(operand->value & ~1);
    for_each_write(entry, path.consts, [&](int var, std::optional<int> value) {
      if (var >= 0 && var < kRoomVarBase) written_.insert(var & ~1);
      note_literal(room_literals_[var], value);
      if (slot == 0) note_literal(init_literals_[static_cast<size_t>(entity)][var], value);
      set_const(path.consts, var, value);
      std::erase_if(path.conds, [&](const Cond& cond) { return cond.subject == var_subject(var) && cond.id == var; });
    });
  }

  struct Literals {
    std::set<int> values;
    bool computed = false;
  };

  static void note_literal(Literals& literals, std::optional<int> value) {
    if (value) literals.values.insert(*value);
    else literals.computed = true;
  }

  // Records the event at the instruction, or merges this path into the one already recorded there (its tests shrink to the common
  // ones, a newly known operand value is added). Returns the new event, nullptr after a merge.
  ScanEvent* add_event(EventKind kind, int entity, int slot, const Insn& ins, const Path& path, const Operand* operand) {
    const int bias = operand_bias(ins.key);
    const std::optional<int> known =
        operand && operand->kind == OperandKind::Var ? find_const(path.consts, operand->value) : std::optional<int>();
    if (const auto site = event_sites_.find({entity, ins.off}); site != event_sites_.end()) {
      ScanEvent& event = result_.events[site->second];
      std::erase_if(event.conds, [&](const Cond& cond) { return std::find(path.conds.begin(), path.conds.end(), cond) == path.conds.end(); });
      if (known && std::find(event.values.begin(), event.values.end(), *known + bias) == event.values.end()) event.values.push_back(*known + bias);
      return nullptr;
    }
    event_sites_[{entity, ins.off}] = result_.events.size();
    ScanEvent event;
    event.kind = kind;
    event.entity = entity;
    event.slot = slot;
    event.offset = ins.off;
    event.op = ins.key;
    event.conds = path.conds;
    if (operand && operand->kind == OperandKind::Imm) {
      event.values.push_back(operand->value + bias);
      if (kind == EventKind::Give) event.value_sites.push_back(operand->pos);
    }
    if (operand && operand->kind == OperandKind::Var) event.operand_var = operand->value;
    if (known) event.values.push_back(*known + bias);
    if (kind == EventKind::Give) event.flag = flag_after(entity, ins.off + ins.length, path.stack);
    result_.events.push_back(std::move(event));
    return &result_.events.back();
  }

  // The first flag written after `pc`, along the fall-through side of every branch, through gotos, CALLs and the scripts a
  // RUN_SCRIPT starts on an entity of the room.
  ScanFlag flag_after(int entity, u32 pc, std::vector<u32> stack) {
    for (int step = 0; step < kFlagSearch; ++step) {
      const Decoded* entry = decoded(pc);
      if (!entry) break;
      const Insn* ins = &entry->ins;
      if (const auto flag = flag_write(*entry)) return *flag;
      const u32 next = ins->off + ins->length;
      if (ins->key == 0x0D || (ins->flow == Flow::End && !stack.empty())) {
        if (stack.empty()) break;
        pc = stack.back();
        stack.pop_back();
        continue;
      }
      if (ins->flow == Flow::End) break;
      if (ins->flow == Flow::Goto && !ins->targets.empty()) {
        pc = ins->targets.front().first;
        continue;
      }
      if (ins->key == 0x05 && !ins->targets.empty() && stack.size() < kStackDepth) {
        stack.push_back(next);
        pc = ins->targets.front().first;
        continue;
      }
      if ((ins->key == 0x07 || ins->key == 0x08 || ins->key == 0x09) && ins->ops.size() >= 2 && stack.size() < kStackDepth) {
        const int target = ins->ops[0].value == 0xFB ? entity : ins->ops[0].value;
        const u16 ptr = target < 0x80 ? module_.script_ptr(static_cast<size_t>(target), static_cast<size_t>(ins->ops[1].value & 0x1F)) : u16{0};
        if (ptr) {
          stack.push_back(next);
          pc = ptr;
          continue;
        }
      }
      pc = next;
    }
    return {};
  }

  // The INIT's tests on the story counter and the entrance: between two neighbouring candidate values every such test comes out the
  // same, so the INIT is run once per pair of candidates, and the regions where every way it can run leaves the entity inactive are
  // kept.
  std::vector<InactiveRegion> inactive_regions(int entity, u16 start) {
    std::set<int> story_values = {0, kTopValue}, entrance_values = {0, kTopValue};
    std::set<u32> seen;
    std::vector<u32> todo = {start};
    while (!todo.empty()) {
      u32 pc = todo.back();
      todo.pop_back();
      while (seen.insert(pc).second) {
        const Decoded* entry = decoded(pc);
        if (!entry) break;
        const Branch branch = branch_of(*entry);
        for (const auto& cond : {branch.fall, branch.jump}) {
          if (!cond || cond->subject != Subject::Var || cond->value_var || (cond->id != kStoryVar && cond->id != kEntranceVar)) continue;
          std::set<int>& values = cond->id == kStoryVar ? story_values : entrance_values;
          for (const int value : {cond->value - 1, cond->value, cond->value + 1}) values.insert(std::clamp(value, 0, kTopValue));
        }
        for (const auto& [target, is_call] : entry->ins.targets) todo.push_back(target);
        if (entry->ins.flow != Flow::Next) break;
        pc = entry->ins.off + entry->ins.length;
      }
    }
    const std::vector<int> stories(story_values.begin(), story_values.end()), entrances(entrance_values.begin(), entrance_values.end());
    std::vector<InactiveRegion> regions;
    for (size_t i = 0; i < stories.size(); ++i) {
      const int story_hi = i + 1 < stories.size() ? stories[i + 1] - 1 : kTopValue;
      for (size_t j = 0; j < entrances.size(); ++j) {
        if (!init_hides(entity, start, stories[i], entrances[j])) continue;
        const int entrance_hi = j + 1 < entrances.size() ? entrances[j + 1] - 1 : kTopValue;
        if (!regions.empty() && regions.back().story_lo == stories[i] && regions.back().entrance_hi + 1 == entrances[j])
          regions.back().entrance_hi = entrance_hi;
        else
          regions.push_back({stories[i], story_hi, entrances[j], entrance_hi});
      }
    }
    return regions;
  }

  // Whether every way the INIT can run with this story value and entrance ends with the entity inactive: tests on the two are decided
  // until a write to them, every other test is taken both ways.
  bool init_hides(int entity, u16 start, int story, int entrance) {
    struct InitPath {
      u32 pc = 0;
      std::vector<u32> stack;
      bool hidden = false;
      bool story_known = true;
      bool entrance_known = true;
      auto operator<=>(const InitPath&) const = default;
    };
    std::set<InitPath> visited;
    std::vector<InitPath> todo = {{start, {}, false, true, true}};
    bool ended = false;
    u32 steps = 0;
    while (!todo.empty()) {
      InitPath path = std::move(todo.back());
      todo.pop_back();
      for (;;) {
        if (++steps > kInitWalkCap || !visited.insert(path).second) break;
        const Decoded* entry = decoded(path.pc);
        if (!entry) return false;
        const Insn& ins = entry->ins;
        if (ins.key == 0x36 && ins.ops.size() >= 2 && (ins.ops[1].value == kThisEntity || ins.ops[1].value == entity))
          path.hidden = ins.ops[0].value == 0;
        if ((ins.key == 0x71 || ins.key == 0x72) && !entry->values.empty() && entry->values[0]->kind != OperandKind::Imm)
          path.story_known = path.entrance_known = false;
        for_each_write(*entry, {}, [&](int var, std::optional<int>) {
          if (var >> 1 == kStoryVar >> 1) path.story_known = false;
          if (var >> 1 == kEntranceVar >> 1) path.entrance_known = false;
        });
        const u32 next = ins.off + ins.length;
        if (ins.key == 0x0D && !path.stack.empty()) {
          path.pc = path.stack.back();
          path.stack.pop_back();
          continue;
        }
        if (ins.key == 0x0D || ins.flow == Flow::End) {
          if (!path.hidden) return false;
          ended = true;
          break;
        }
        if (ins.flow == Flow::Goto || ins.key == 0x05) {
          if (ins.targets.empty() || (ins.key == 0x05 && path.stack.size() >= kStackDepth)) return false;
          if (ins.key == 0x05) path.stack.push_back(next);
          path.pc = ins.targets.front().first;
          continue;
        }
        if (ins.targets.empty()) {
          path.pc = next;
          continue;
        }
        const Branch branch = branch_of(*entry);
        bool fall = branch.fall_live, jump = branch.jump_live;
        if (const auto& cond = branch.fall; cond && cond->subject == Subject::Var && !cond->value_var) {
          const bool known = (cond->id == kStoryVar && path.story_known) || (cond->id == kEntranceVar && path.entrance_known);
          if (known) {
            const bool holds_now = holds(cond->id == kStoryVar ? story : entrance, cond->cmp, cond->value);
            fall = fall && holds_now;
            jump = jump && !holds_now;
          }
        }
        if (jump) {
          InitPath other = path;
          other.pc = ins.targets.front().first;
          todo.push_back(std::move(other));
        }
        if (!fall) break;
        path.pc = next;
      }
      if (steps > kInitWalkCap) return false;
    }
    return ended;
  }

  void resolve() {
    for (ScanEvent& event : result_.events) {
      if (!event.values.empty() || event.operand_var < 0) continue;
      const int bias = operand_bias(event.op);
      const auto& init = init_literals_[static_cast<size_t>(event.entity)];
      const auto own = init.find(event.operand_var);
      const Literals* source = nullptr;
      if (own != init.end() && !own->second.computed && own->second.values.size() == 1) source = &own->second;
      else if (const auto room = room_literals_.find(event.operand_var); room != room_literals_.end() && !room->second.computed)
        source = &room->second;
      if (source)
        for (int value : source->values) event.values.push_back(value + bias);
    }
    for (ScanEvent& event : result_.events) {
      if (event.kind != EventKind::Give || event.operand_var < kRoomVarBase) continue;
      std::set<u32> sites;
      for (int value : event.values)
        if (const auto found = literal_sites_.find({event.operand_var, value}); found != literal_sites_.end())
          sites.insert(found->second.begin(), found->second.end());
      event.value_sites.assign(sites.begin(), sites.end());
    }
  }

  const FieldScriptModule& module_;
  RoomScanResult result_;
  // Per code offset: the index into decoded_, kUndecoded, or kUndecodable.
  std::vector<int> index_;
  std::deque<Decoded> decoded_;
  std::vector<bool> meeting_;
  std::map<std::pair<const char*, u8>, std::vector<bool>> dest_layouts_;
  std::map<int, Literals> room_literals_;
  std::vector<std::map<int, Literals>> init_literals_;
  // (room variable, literal) -> the code offsets of the literal words SET writes it with.
  std::map<std::pair<int, int>, std::set<u32>> literal_sites_;
  // Every test an INIT of the room makes.
  std::set<Cond> init_tests_;
  // (entity, offset) -> index into result_.events; (entity, offset) of an INIT op making its entity inactive -> the tests there.
  std::map<std::pair<int, u32>, size_t> event_sites_;
  std::map<std::pair<int, u32>, std::vector<Cond>> hide_sites_;
  // Entities some walked 36 makes active; global variables some walked instruction writes (even byte offsets).
  std::set<int> reactivated_;
  std::set<int> written_;
};

const char* cmp_text(Cmp cmp) {
  switch (cmp) {
    case Cmp::Eq: return "==";
    case Cmp::Ne: return "!=";
    case Cmp::Gt: return ">";
    case Cmp::Lt: return "<";
    case Cmp::Ge: return ">=";
    case Cmp::Le: return "<=";
    case Cmp::AnyBit: return "&";
    case Cmp::NoBit: return "!&";
  }
  return "?";
}

}  // namespace

RoomScanResult scan_script(const FieldScriptModule& module) { return Scanner(module).run(); }

GiveClass give_class(int id) {
  if (id <= 0) return GiveClass::None;
  if (id < 0x200) return GiveClass::Item;
  if (id < 0x400) return GiveClass::Element;
  if (id > 0x400 && id < 0x440) return GiveClass::KeyItem;
  return GiveClass::None;
}

int give_count(int id) { return id >= 0x261 && id <= 0x26B ? 5 : 1; }

StoryRange story_range(const std::vector<Cond>& conds) {
  StoryRange range;
  for (const Cond& cond : conds) {
    if (cond.subject != Subject::Var || cond.id != 0 || cond.value_var) continue;
    switch (cond.cmp) {
      case Cmp::Eq:
        range.lo = std::max(range.lo, cond.value);
        range.hi = std::min(range.hi, cond.value);
        break;
      case Cmp::Gt: range.lo = std::max(range.lo, cond.value + 1); break;
      case Cmp::Lt: range.hi = std::min(range.hi, cond.value - 1); break;
      case Cmp::Ge: range.lo = std::max(range.lo, cond.value); break;
      case Cmp::Le: range.hi = std::min(range.hi, cond.value); break;
      default: break;
    }
  }
  return range;
}

std::string cond_text(const Cond& cond) {
  const std::string value = cond.value_var ? std::format("${:04X}", cond.value) : std::to_string(cond.value);
  switch (cond.subject) {
    case Subject::Var:
      if (cond.id == 0) return std::format("story{}{}", cmp_text(cond.cmp), value);
      if (cond.id >= kRoomVarBase) return std::format("room[{}]{}{}", (cond.id - kRoomVarBase) / 2, cmp_text(cond.cmp), value);
      return std::format("${:04X}{}{}", cond.id, cmp_text(cond.cmp), value);
    case Subject::InParty: return std::format("char{}{}", cond.id, cond.value ? " in party" : " not in party");
    case Subject::Recruited: return std::format("char{}{}", cond.id, cond.value ? " recruited" : " not recruited");
    case Subject::Carryover: return std::format("char{}{}", cond.id, cond.value ? " carried over" : " not carried over");
    case Subject::RecruitedOrCarryover:
      return std::format("char{}{}", cond.id, cond.value ? " recruited or carried over" : " neither recruited nor carried over");
    case Subject::Leader: return std::format("char{}{}", cond.id, cond.value ? " leads" : " does not lead");
    case Subject::Item: return std::format("item{}{}", cond.id, cond.value ? " owned" : " missing");
    case Subject::Money: return std::format("gold{}{}", cmp_text(cond.cmp), value);
    case Subject::Choice: return std::format("choice{}{}", cmp_text(cond.cmp), value);
    case Subject::Key: return std::format("keys{}0x{:04X}", cmp_text(cond.cmp), cond.value);
  }
  return {};
}

std::string flag_text(const ScanFlag& flag) {
  if (!flag.valid()) return {};
  if (flag.bit >= 0) return std::format("${:04X}.{}", flag.var, flag.bit);
  return std::format("${:04X}={}", flag.var, flag.value);
}

const char* event_kind_name(EventKind kind) {
  switch (kind) {
    case EventKind::Give: return "give";
    case EventKind::Take: return "take";
    case EventKind::Gold: return "gold";
    case EventKind::Battle: return "battle";
    case EventKind::Join: return "join";
    case EventKind::Roster: return "roster";
    case EventKind::Unroster: return "unroster";
    case EventKind::Scenario: return "story";
    case EventKind::Exit: return "exit";
    case EventKind::SetFlag: return "setflag";
  }
  return "?";
}

}  // namespace oc::script
