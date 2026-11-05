#include "script/field_vm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "core/log.h"

namespace oc::script {

namespace {

// INIT scripts run "to completion" (budget 0xFFFF); the step cap protects against parked or looping ones. kCallCap is
// the number of handler calls one dispatcher invocation may make.
constexpr int kStepCapInit = 65535;
constexpr int kCallCap = 0x41;

u32 default_rng_step(u32& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace

FieldVM::FieldVM(FieldHost& host, RandFn rng) : host_(host), rng_(std::move(rng)) {
  if (!rng_) {
    auto state = std::make_shared<u32>(0x2545F491u);
    rng_ = [state] { return default_rng_step(*state); };
  }
}

// ------------------------------------------------------------------------------------------------ variables
bool FieldVM::var_unsigned(u32 id) const { return (mod_.sign_mask[(id >> 6) & 31] >> ((id >> 1) & 31)) & 1; }

s32 FieldVM::read_var(u32 id) const {
  const u16 raw = vars_[(id >> 1) & (kVarSlots - 1)];
  return var_unsigned(id) ? static_cast<s32>(raw) : static_cast<s32>(static_cast<s16>(raw));
}

s32 FieldVM::var(u16 id) const { return read_var(id); }
void FieldVM::set_var(u16 id, s32 value) {
  write_var(id, value);
  if ((id & ~1u) == var::kMapId) room_reg_ = value;
}

int FieldVM::room_register() const { return room_reg_ >= 0 ? room_reg_ : read_var(var::kMapId); }
void FieldVM::note_room_change(int room) {
  write_var(var::kPrevMapId, room_register());
  room_reg_ = room;
}
bool FieldVM::var_bit(u32 bit_ref) const { return (vars_[(bit_ref >> 5) & (kVarSlots - 1)] >> (bit_ref & 0xF)) & 1; }
void FieldVM::set_global(Global which, s32 value) {
  globals_[static_cast<size_t>(which)] = value;
  host_.set_global(which, value);
}

s32 FieldVM::imm_or_var(u16 raw) const { return raw & 0x8000 ? raw & 0x7FFF : read_var(raw); }

s32 FieldVM::operand_s(u32 offset, u32 mode_off, u32 mask) const {
  const u16 raw = word_at(offset);
  return byte_at(mode_off) & mask ? static_cast<s16>(raw) : read_var(raw);
}

int FieldVM::resolve_character(u8 char_ref) const {
  const int count = static_cast<int>(ents_.size());
  int entity = -1;
  if (char_ref < 0x80) {
    entity = char_ref;
  } else if (char_ref == 0xFB) {
    entity = ent_;
  } else if (char_ref == 0xFF || char_ref == 0xFE || char_ref == 0xFD) {
    entity = host_.party_entity(char_ref == 0xFF ? 0 : char_ref == 0xFE ? 1 : 2);
  } else if (char_ref != 0xFC) {
    entity = host_.character_entity(char_ref - 0x80);
  }
  return entity >= 0 && entity < count ? entity : -1;
}

s32 FieldVM::isqrt_dist(s32 dx, s32 dy, s32 dz) const {
  const double squared = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy + static_cast<double>(dz) * dz;
  return static_cast<s32>(std::sqrt(squared));
}

void FieldVM::note_unknown(u16 key) {
  ++stats_.unknown_ops[key];
  if (logged_.insert(key).second) log::warn("field script: opcode {:04X} is not in the table (skipped)", key);
}

// ------------------------------------------------------------------------------------------------ scheduling
void FieldVM::forget_child() {
  rec_->phase = 0;
  rec_->child_ent = -1;
  rec_->child_rec = -1;
}

void FieldVM::release() {
  rec_->state = kRecIdle;
  if (rec_->id == 2) self_->flags270 &= ~flag270::kActionRunning;
  rec_->id = 0xFF;
  rec_->wait = 0;
  forget_child();
  ended_ = true;
}

int FieldVM::pick_record(int ent) const {
  const Entity& entity = ents_[static_cast<size_t>(ent)];
  int best = -1;
  u16 best_state = 0x10;
  for (int i = 0; i < kRecordsPerEntity; ++i) {
    const ScriptRecord& record = entity.rec[static_cast<size_t>(i)];
    // A record waiting for a live child on the same entity would starve the child when classes tie: skip it.
    if (record.state != kRecIdle && record.phase == 1 && record.child_ent == ent && record.child_rec >= 0 &&
        entity.rec[static_cast<size_t>(record.child_rec)].state != kRecIdle)
      continue;
    if (record.state <= best_state) {
      best = i;
      best_state = record.state;
    }
  }
  return best < 0 ? 0 : best;
}

// The slot flag (flag47) lives in the record array and survives a record being reused.
FieldVM::Spawn FieldVM::spawn(int target, int id, int cls, bool hold, int* rec_index) {
  if (target < 0 || target >= static_cast<int>(ents_.size()) || id < 0 || id >= kScriptSlots) return Spawn::None;
  const u16 ptr = mod_.script_ptr(static_cast<size_t>(target), static_cast<size_t>(id));
  if (ptr == 0) return Spawn::None;
  Entity& entity = ents_[static_cast<size_t>(target)];
  for (const auto& record : entity.rec)
    if (record.state != kRecIdle && record.id == id) return Spawn::Deduped;
  for (int i = 0; i < kRecordsPerEntity; ++i) {
    ScriptRecord& record = entity.rec[static_cast<size_t>(i)];
    if (record.state == kRecIdle && record.hold == 0) {
      const u16 keep47 = record.flag47;
      record = ScriptRecord{};
      record.flag47 = keep47;
      record.pc = ptr;
      record.state = static_cast<u16>(cls);
      record.id = static_cast<u16>(id);
      record.hold = hold ? 1 : 0;
      if (rec_index) *rec_index = i;
      return Spawn::Spawned;
    }
  }
  return Spawn::NoRecord;
}

bool FieldVM::start_script(int ent, int slot, int cls) {
  int rec_index = -1;
  return spawn(ent, slot, cls, false, &rec_index) == Spawn::Spawned;
}

bool FieldVM::trigger_action(int ent) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  Entity& entity = ents_[static_cast<size_t>(ent)];
  if ((entity.flags38 & (flag38::kInactive | flag38::kNoAction)) || (gflags_ & gflag::kInputLock)) return false;
  if (!start_script(ent, 2, 3)) return false;
  entity.flags270 |= flag270::kActionRunning;
  return true;
}

bool FieldVM::trigger_collision(int ent) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  if (ents_[static_cast<size_t>(ent)].flags38 & (flag38::kInactive | flag38::kNoCollision)) return false;
  return start_script(ent, 3, 4);
}

bool FieldVM::trigger_touch(int ent) {
  if (ent < 0 || ent >= static_cast<int>(ents_.size())) return false;
  const Entity& entity = ents_[static_cast<size_t>(ent)];
  if ((entity.flags38 & flag38::kInactive) || !(entity.flags270 & flag270::kTouchTrigger)) return false;
  return start_script(ent, 4, 2);
}

// Runs the entity's lowest-class live record; when none is live the UPDATE script (slot 1) restarts in record 0.
void FieldVM::run_entity(int ent) {
  Entity& entity = ents_[static_cast<size_t>(ent)];
  if (entity.flags38 & flag38::kInactive) return;
  if ((gflags_ & gflag::kFormation) && host_.party_slot_of_entity(ent) >= 0) return;
  int rec_index = pick_record(ent);
  if (entity.rec[static_cast<size_t>(rec_index)].state == kRecIdle) {
    const u16 ptr = mod_.script_ptr(static_cast<size_t>(ent), 1);
    if (ptr == 0) return;
    const u16 keep47 = entity.rec[0].flag47;
    entity.rec[0] = ScriptRecord{};
    entity.rec[0].flag47 = keep47;
    entity.rec[0].pc = ptr;
    entity.rec[0].state = 7;
    entity.rec[0].id = 1;
    rec_index = 0;
  }
  // The action flag only covers the wait for the ON_ACTION record: it drops as soon as that record is picked to run. An entity that
  // goes inactive inside its own ON_ACTION (a chest hiding itself) is never run again and must not keep the controls taken.
  if (entity.rec[static_cast<size_t>(rec_index)].id == 2) entity.flags270 &= ~flag270::kActionRunning;
  dispatch(ent, rec_index, false);
}

// Code offset 0 is the pre-script table when the code starts with 0xFF, so a zero INIT pointer means "no script" there.
void FieldVM::run_init(int ent) {
  Entity& entity = ents_[static_cast<size_t>(ent)];
  u32 ptr = mod_.script_ptr(static_cast<size_t>(ent), 0);
  if (ptr == 0 && (mod_.code.empty() || mod_.code[0] == 0xFF)) return;
  entity.rec[0] = ScriptRecord{};
  entity.rec[0].pc = ptr;
  entity.rec[0].state = 0;
  entity.rec[0].id = 0;
  dispatch(ent, 0, true);
}

// 7C: a party entity whose SETUP_PC found no character at the room load (its slot was still empty) stays out of play. Once a
// character joins, the first entity whose INIT opens with SETUP_PC naming that character (FD / FE / FF read as the party slots
// are now) gets its room-load flags back and its INIT re-run to completion in the middle of the running script, then stands on
// the controlled entity's spot, facing its way.
void FieldVM::relink_party_entity(int char_id) {
  if (char_id < 0) return;
  for (int ent = 0; ent < static_cast<int>(ents_.size()); ++ent) {
    const u16 ptr = mod_.script_ptr(static_cast<size_t>(ent), 0);
    if (mod_.code_byte(ptr) != 0x0E) continue;
    const u8 ref = mod_.code_byte(ptr + 1);
    const s32 named = ref >= 0xFD ? party_table(ref - 0xFD) : ref < 0x2C ? ref : 0xFF;
    if (named != char_id) continue;
    Entity& target = ents_[static_cast<size_t>(ent)];
    const ScriptRecord kept_record = target.rec[0];
    const u8 kept_cur = target.cur;
    const int kept_ent = ent_;
    Entity* const kept_self = self_;
    ScriptRecord* const kept_rec = rec_;
    const u32 kept_base = base_;
    const bool kept_yield = yield_, kept_ended = ended_, kept_run_mode = run_mode_;
    const int kept_budget = budget_;
    target.flags38 = load_flags(static_cast<size_t>(ent));
    run_init(ent);
    target.rec[0] = kept_record;
    target.cur = kept_cur;
    ent_ = kept_ent;
    self_ = kept_self;
    rec_ = kept_rec;
    base_ = kept_base;
    yield_ = kept_yield;
    ended_ = kept_ended;
    run_mode_ = kept_run_mode;
    budget_ = kept_budget;
    host_.place_on_controlled(ent);
    return;
  }
}

// Executes one record: up to `budget_` instructions (8 plus the bonuses ops grant) and kCallCap handler calls per tick,
// or to completion in run mode (INIT). A record that yields or ends stops the loop. Afterwards the watchdog counters
// are updated: an instruction that waits in place counts as `stuck`, a dispatch that starts at the same pc as the
// previous one as a `loop`; parking and timer ops (WAIT, STOP_EXEC, UPDATE_CHARACTER_STOP, FE 16) wait by design.
void FieldVM::dispatch(int ent, int rec_index, bool run_mode) {
  ent_ = ent;
  self_ = &ents_[static_cast<size_t>(ent)];
  self_->cur = static_cast<u8>(rec_index);
  rec_ = &self_->rec[static_cast<size_t>(rec_index)];
  run_mode_ = run_mode;
  budget_ = 8;
  ended_ = false;
  int steps = 0, calls = 0;
  const u32 start_pc = rec_->pc;
  for (;;) {
    if (run_mode) {
      if (steps >= kStepCapInit) {
        ++stats_.init_overruns;
        release();
        break;
      }
    } else if (steps >= budget_ || calls >= kCallCap) {
      break;
    }
    const u32 pc = rec_->pc;
    if (pc >= mod_.code.size()) {
      ++stats_.invalid_pc;
      release();
      break;
    }
    const u8 op = mod_.code[pc];
    if (op == 0) {
      release();
      break;
    }
    yield_ = false;
    base_ = pc;
    exec(op);
    ++steps;
    ++calls;
    ++stats_.instructions;
    if (ended_) break;
    if (yield_ && !run_mode) break;
  }
  if (run_mode && !ended_ && rec_->state != kRecIdle) release();
  if (!run_mode && !ended_ && rec_->state != kRecIdle) {
    const u8 op0 = mod_.code_byte(start_pc);
    const bool timer = op0 == 0x2D || op0 == 0x3B || op0 == 0x0B || (op0 == 0xFE && mod_.code_byte(start_pc + 1) == 0x16);
    if (timer) {
      rec_->stuck = rec_->loops = 0;
    } else {
      if (yield_ && rec_->pc == start_pc && steps == 1) ++rec_->stuck; else rec_->stuck = 0;
      if (start_pc == rec_->last_start_pc) ++rec_->loops; else rec_->loops = 0;
    }
    rec_->last_start_pc = start_pc;
  }
}

namespace {
u16 key_at(const FieldScriptModule& module, u32 pc) {
  const u8 op = module.code_byte(pc);
  return op == 0xFE ? static_cast<u16>(kExtPage | module.code_byte(pc + 1)) : op;
}
}  // namespace

FieldVM::StuckInfo FieldVM::stuck_info(size_t ent, size_t rec, u32 ticks, bool strict) const {
  const Entity& entity = ents_[ent];
  const ScriptRecord& record = entity.rec[rec];
  StuckInfo info;
  info.ent = static_cast<int>(ent);
  info.rec = static_cast<int>(rec);
  info.id = record.id;
  info.cls = record.state;
  info.pc = record.pc;
  info.key = key_at(mod_, record.pc);
  info.ticks = ticks;
  info.strict = strict;
  info.holds_lock = (entity.flags270 & flag270::kInputLock) != 0;
  return info;
}

std::vector<FieldVM::StuckInfo> FieldVM::stuck_records(u32 min_ticks) const {
  std::vector<StuckInfo> out;
  for (size_t ent = 0; ent < ents_.size(); ++ent) {
    const Entity& entity = ents_[ent];
    for (size_t rec = 0; rec < entity.rec.size(); ++rec) {
      const ScriptRecord& record = entity.rec[rec];
      if (record.state == kRecIdle) continue;
      const bool locks = (entity.flags270 & flag270::kInputLock) != 0;
      const bool strict = record.stuck >= min_ticks;
      const bool loop = record.loops >= min_ticks && (record.state < 7 || locks);
      if (!strict && !loop) continue;
      out.push_back(stuck_info(ent, rec, strict ? record.stuck : record.loops, strict));
    }
  }
  return out;
}

std::string FieldVM::describe(const StuckInfo& info) {
  const OpInfo* op = find_op(info.key);
  char buf[200];
  std::snprintf(buf, sizeof buf, "ent %d record %d (script %u, class %u) %s at pc %04X op %s%02X %s, %u ticks%s", info.ent, info.rec,
                info.id, info.cls, info.strict ? "waits" : "polls", info.pc, info.key > 0xFF ? "FE " : "", info.key & 0xFF,
                op ? op->name : "?", info.ticks, info.holds_lock ? ", holds the input lock" : "");
  return buf;
}

std::string FieldVM::describe_records() const {
  std::string out;
  for (size_t ent = 0; ent < ents_.size(); ++ent) {
    const Entity& entity = ents_[ent];
    for (size_t rec = 0; rec < entity.rec.size(); ++rec) {
      const ScriptRecord& record = entity.rec[rec];
      if (record.state == kRecIdle) continue;
      const bool idle_update_loop = record.state == 7 && record.id == 1 && !(entity.flags270 & flag270::kInputLock);
      if (idle_update_loop) continue;
      out += "  " + describe(stuck_info(ent, rec, std::max(record.stuck, record.loops), record.stuck >= record.loops)) + "\n";
    }
  }
  return out;
}

void FieldVM::tick() {
  ++stats_.ticks;
  for (int ent = 0; ent < static_cast<int>(ents_.size()); ++ent) run_entity(ent);
  host_.end_of_tick(*this);
}

// The FE 29 sound slot register is the one register the room init clears; the water levels of the first
// three slots default to 10. Every room starts with the pause allowed (FE 42 0 forbids it).
// An entity with no ON_ACTION or ON_COLLISION script (or entity 0) starts with the matching no-trigger flag.
u32 FieldVM::load_flags(size_t ent) const {
  u32 flags = 0;
  for (size_t slot : {size_t{2}, size_t{3}}) {
    const u16 ptr = mod_.script_ptr(ent, slot);
    if (ptr == 0 || mod_.code_byte(ptr) == 0 || ent == 0) flags |= slot == 2 ? flag38::kNoAction : flag38::kNoCollision;
  }
  return flags;
}

void FieldVM::load(FieldScriptModule module, std::span<const u32> flag_table) {
  mod_ = std::move(module);
  ents_.assign(mod_.entity_count(), Entity{});
  std::fill(vars_.begin() + kGlobalVarSlots, vars_.end(), u16{0});
  vars_[0x46 >> 1] = 1;
  gflags_ = gflag::kPauseAllowed;
  world_continue_ = false;
  globals_[static_cast<size_t>(Global::KeepSfxSlots)] = 0;
  flag_table_.fill(0);
  std::copy_n(flag_table.begin(), std::min<size_t>(flag_table.size(), flag_table_.size()), flag_table_.begin());
  flag_writes_ = 0;
  table_9fdc_.fill(0);
  for (size_t i = 0; i < 3; ++i) table_9fdc_[i] = 10;
  slot_ent_.fill(0xFF);
  staged_char_ = -1;
  for (auto& table : ent_ref_tables_) table.fill(0xFF);
  for (size_t ent = 0; ent < ents_.size(); ++ent) ents_[ent].flags38 |= load_flags(ent);
  for (int ent = 0; ent < static_cast<int>(ents_.size()); ++ent) run_init(ent);
  // An FE 50 in INIT (the world-map Continue) puts the FE 4F slot entities back on the positions and facings saved in variables
  // 0x300 + 6 * slot as soon as INIT is over.
  if (!world_continue_) return;
  for (size_t slot = 0; slot < slot_ent_.size(); ++slot) {
    const int entity = slot_entity(slot);
    if (entity < 0 || entity >= static_cast<int>(ents_.size())) continue;
    const u32 var_id = static_cast<u32>(0x300 + 6 * slot);
    host_.set_position_xz(entity, read_var(var_id), read_var(var_id + 2), 0);
    host_.set_facing(entity, read_var(var_id + 4) & 0xFFF);
  }
}

// ------------------------------------------------------------------------------------------------ helpers
bool FieldVM::menu_request(MenuKind kind, int arg) {
  if (rec_->phase == 0) {
    host_.request_menu(kind, arg);
    rec_->phase = 1;
    yield_ = true;
    return false;
  }
  if (host_.menu_active()) {
    yield_ = true;
    return false;
  }
  rec_->phase = 0;
  return true;
}

void FieldVM::turn_flag_13(Entity& entity, bool on) {
  set_flag(entity.flags270, 13, on);
  if (!on) {
    if (entity.flags270 & 0x8000) entity.v24e = entity.v240;
    entity.flags270 &= ~0xC000u;
  }
}

void FieldVM::opaque(u16 key, u32 first_operand, u32 count) {
  std::array<u8, 24> tmp{};
  count = std::min<u32>(count, static_cast<u32>(tmp.size()));
  for (u32 i = 0; i < count; ++i) tmp[i] = byte_at(first_operand + i);
  host_.opaque_op(key, std::span<const u8>(tmp.data(), count));
}

void FieldVM::park() {
  yield_ = true;
  if (rec_->id == 2) self_->flags270 &= ~flag270::kActionRunning;
  if (run_mode_) ended_ = true;
}

void FieldVM::jump_or_advance(bool jump, u32 jump_offset, u32 length) {
  if (jump)
    rec_->pc = word_at(jump_offset);
  else
    advance(length);
}

void FieldVM::wait_or_advance(bool busy, u32 length) {
  if (busy)
    yield_ = true;
  else
    advance(length);
}

bool FieldVM::system_gate() {
  if (host_.system_ready()) return true;
  yield_ = true;
  return false;
}

void FieldVM::begin_motion(const MotionRequest& request, bool free_height) {
  host_.start_motion(ent_, request);
  self_->flags270 |= flag270::kMoving;
  if (free_height) self_->flags38 |= 0x40;
}

// Head turns need a 3D object on the turning entity (and on the looked-at one); the rate is `rate_entity`'s.
void FieldVM::head_turn(HeadOp op, int ent, int other, s32 x, s32 y, s32 z, int rate_entity) {
  if (!has_model(ent) || (op == HeadOp::LookAtHead && !has_model(other))) return;
  host_.head_op(op, ent, other, x, y, z, ents_[static_cast<size_t>(rate_entity)].head_turn_rate);
}

// The arithmetic ops 10 13 14 15 16 19 1A 1B 20 21: variable op= S operand. Division by zero (and INT32_MIN / -1) gives 0.
void FieldVM::alu(u8 op) {
  const u32 vid = word_at(1);
  const s32 cur = read_var(vid);
  const s32 val = operand_s(3, 5, 0x40);
  const u32 ucur = static_cast<u32>(cur), uval = static_cast<u32>(val);
  s32 res = cur;
  switch (op) {
    case 0x10: res = val; break;
    case 0x13: res = static_cast<s32>(ucur + uval); break;
    case 0x14: res = static_cast<s32>(ucur - uval); break;
    case 0x15: res = static_cast<s32>(ucur | (1u << (val & 31))); break;
    case 0x16: res = static_cast<s32>(ucur & ~(1u << (val & 31))); break;
    case 0x19: res = cur & val; break;
    case 0x1A: res = cur | val; break;
    case 0x1B: res = cur ^ val; break;
    case 0x20: res = static_cast<s32>(ucur * uval); break;
    case 0x21: res = (val == 0 || (cur == INT32_MIN && val == -1)) ? 0 : cur / val; break;
    default: break;
  }
  write_var(vid, res);
  advance(6);
}

// ------------------------------------------------------------------------------------------------ main page
// One instruction of the main opcode page. Every case ends with advance(length), a jump, a yield (the instruction is
// retried next tick) or park(). Operand offsets count from the opcode byte.
void FieldVM::exec(u8 op) {
  u32& pc = rec_->pc;
  ++stats_.op_hits[op];
  self_->last_pc = pc;
  self_->last_op = op;
  if (op == 0xFE) {
    base_ = pc + 1;
    const u8 sub = mod_.code_byte(pc + 1);
    ++stats_.op_hits[0x100 + sub];
    self_->last_op = static_cast<u16>(kExtPage | sub);
    exec_ext(sub);
    return;
  }
  if (const OpInfo* info = find_op(op); info && info->conf == '?') ++stats_.uncertain_ops[op];
  if (exec_core(op) || exec_scene(op) || exec_game(op)) return;
  const OpInfo* info = find_op(op);
  if (!info) {
    note_unknown(op);
    advance(1);
  } else {
    ++stats_.unimplemented;
    if (logged_.insert(op).second) log::warn("field script: opcode {:02X} ({}) has no handler", op, info->name);
    advance(std::max(1, std::atoi(info->lengths)));
  }
}

}  // namespace oc::script
