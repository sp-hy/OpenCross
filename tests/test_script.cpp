// Field script VM tests: hand-assembled snippets run through FieldVM with a NullHost / scripted host.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>

#include "script/field_host.h"
#include "script/field_script.h"
#include "script/field_vm.h"

using namespace oc;
using namespace oc::script;

namespace {
int failures = 0;
#define CHECK(condition)                                              \
  do {                                                                \
    if (!(condition)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                     \
    }                                                                 \
  } while (0)
#define CHECK_EQ(lhs, rhs)                                                                                 \
  do {                                                                                                     \
    const long long lhs_value_ = static_cast<long long>(lhs), rhs_value_ = static_cast<long long>(rhs);    \
    if (lhs_value_ != rhs_value_) {                                                                        \
      std::printf("FAIL %s:%d %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #lhs, #rhs, lhs_value_, rhs_value_); \
      ++failures;                                                                                          \
    }                                                                                                      \
  } while (0)

// V(k) is room-local variable k; I(value) is a U immediate.
constexpr u16 V(int k) { return static_cast<u16>(0x400 + 2 * k); }
constexpr u16 I(int value) { return static_cast<u16>(0x8000 | value); }

// Tiny assembler. Offset 0 holds a stub byte (a pointer of 0 means "unused slot").
struct Asm {
  Bytes code{0xFF};
  u16 here() const { return static_cast<u16>(code.size()); }
  Asm& raw(std::initializer_list<int> values) {
    for (int value : values) code.push_back(static_cast<u8>(value));
    return *this;
  }
  Asm& word(int value) { return raw({value & 0xFF, (value >> 8) & 0xFF}); }
  Asm& stop() { return raw({0x00}); }
  Asm& jump(u16 target) { raw({0x01}); return word(target); }
  Asm& call(u16 target) { raw({0x05}); return word(target); }
  Asm& ret() { return raw({0x0D}); }
  Asm& wait_frame() { return raw({0x3A}); }
  Asm& park() { return raw({0x3B}); }
  Asm& wait(int ticks) { raw({0x2D}); return word(I(ticks)); }
  // alu: V S40 M with a literal operand; alu_v: V S40 M with a variable operand.
  Asm& alu(int op, u16 var, int lit) { raw({op}); word(var); word(lit & 0xFFFF); return raw({0x40}); }
  Asm& alu_v(int op, u16 var, u16 src) { raw({op}); word(var); word(src); return raw({0x00}); }
  Asm& set(u16 var, int lit) { return alu(0x10, var, lit); }
  Asm& add(u16 var, int lit) { return alu(0x13, var, lit); }
  Asm& inc(u16 var) { raw({0x17}); return word(var); }
  Asm& dec(u16 var) { raw({0x18}); return word(var); }
  // IF_JUMP var <cond> literal -> falls through when TRUE, jumps to `target` when FALSE.
  Asm& if_jump(u16 var, int cond, int lit, u16 target) {
    raw({0x02});
    word(var);
    word(lit & 0xFFFF);
    raw({0x40 | cond});
    return word(target);
  }
};

struct EntScripts {
  std::map<int, u16> slot;
};

FieldScriptModule make_module(const Asm& script, const std::vector<EntScripts>& ents, u32 sign_mask0 = 0) {
  FieldScriptModule module;
  module.sign_mask[0] = sign_mask0;
  module.entities.resize(ents.size());
  for (size_t ent = 0; ent < ents.size(); ++ent) {
    module.entities[ent].fill(0);
    for (auto [slot, address] : ents[ent].slot) module.entities[ent][static_cast<size_t>(slot)] = address;
  }
  module.code = script.code;
  module.code_base = static_cast<u32>(0x84 + 0x40 * ents.size());
  return module;
}

// Host whose motion / dialogue polls can be scripted.
class TestHost : public NullHost {
 public:
  int motion_polls_busy = 0;
  int motions_started = 0;
  int dialogue_ticks = 0;
  int opened = 0;
  bool motion_busy(int) override { return motion_polls_busy-- > 0; }
  void start_motion(int, const MotionRequest&) override { ++motions_started; }
  bool dialogue_open() override { return dialogue_ticks-- > 0; }
  bool dialogue_blocked() override { return dialogue_open(); }
  void open_dialogue(int, int, int) override { ++opened; }
};

// DC only records the movie-end jump: no variable, no room register, no map jump, and the 7E behind it is reached in the same run.
// Taking the jump (note_room_change) stores the room register in PREVIOUS_MAP_ID and moves the register to the target.
void test_movie_end_jump() {
  class JumpHost : public NullHost {
   public:
    int movie_room = -1, movie_param = -1, map_jumps = 0, movies = 0;
    void set_movie_jump(int room, int param) override { movie_room = room, movie_param = param; }
    void request_map_jump(int, int) override { ++map_jumps; }
    void play_movie(int, int, bool) override { ++movies; }
  };
  Asm script;
  const u16 init = script.here();
  script.raw({0xDC}).word(I(472)).word(I(0x102));
  script.raw({0x7E}).word(I(18));
  script.stop();
  JumpHost host;
  FieldVM vm(host);
  vm.set_var(var::kMapId, 538);
  vm.set_var(var::kPrevMapId, 78);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(host.movie_room, 472);
  CHECK_EQ(host.movie_param, 0x102);
  CHECK_EQ(host.map_jumps, 0);
  CHECK_EQ(host.movies, 1);
  CHECK_EQ(vm.var(var::kPrevMapId), 78);
  vm.note_room_change(472);
  CHECK_EQ(vm.var(var::kPrevMapId), 538);
  vm.note_room_change(22);
  CHECK_EQ(vm.var(var::kPrevMapId), 472);
}

void test_arithmetic() {
  Asm script;
  const u16 init = script.here();
  // V0: 5 + 7, then op 0x14 with 2 -> 10; times 3 -> 30; op 0x21 with 4 -> 7; inc, inc, dec -> 8.
  script.set(V(0), 5).add(V(0), 7).alu(0x14, V(0), 2);
  script.alu(0x20, V(0), 3);
  script.alu(0x21, V(0), 4);
  script.inc(V(0)).inc(V(0)).dec(V(0));
  // V1: a signed literal -3, times 5 -> -15.
  script.set(V(1), -3);
  script.alu(0x20, V(1), 5);
  // V2: AND then OR -> 0x010F.
  script.set(V(2), 0x0F0F).alu(0x19, V(2), 0x00FF).alu(0x1A, V(2), 0x0100);
  // V3: SET_BIT 4, 1 -> 0x12; CLEAR_BIT 1 -> 0x10.
  script.set(V(3), 0).alu(0x15, V(3), 4).alu(0x15, V(3), 1);
  script.alu(0x16, V(3), 1);
  // V4: a variable operand, 3 + 8.
  script.set(V(4), 3).alu_v(0x13, V(4), V(0));
  script.set(V(5), 4);
  // SHL V U.
  script.raw({0x1C});
  script.word(V(5)).word(I(3));
  // V6: division by zero is guarded.
  script.set(V(6), 1).alu(0x21, V(6), 0);
  script.stop();
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(0)), 8);
  CHECK_EQ(vm.var(V(1)), -15);
  CHECK_EQ(vm.var(V(2)), 0x010F);
  CHECK_EQ(vm.var(V(3)), 0x10);
  CHECK_EQ(vm.var(V(4)), 11);
  CHECK_EQ(vm.var(V(5)), 32);
  CHECK_EQ(vm.var(V(6)), 0);
}

void test_variables_and_signedness() {
  Asm script;
  const u16 init = script.here();
  // Global 0x30 = 77, local 0 = 0xFFFF.
  script.set(V(0), -1).set(0x30, 77);
  script.stop();
  // Local variable 0 is slot 0x200 = mask word 16 (id 0x400 >> 6), bit 0.
  FieldScriptModule signed_m = make_module(script, {{{{0, init}}}});
  NullHost host;
  FieldVM vm(host);
  vm.load(signed_m);
  CHECK_EQ(vm.var(V(0)), -1);
  FieldScriptModule unsigned_m = signed_m;
  unsigned_m.sign_mask[16] = 1;
  // Globals persist, locals are zeroed and rewritten by INIT.
  vm.load(unsigned_m);
  CHECK_EQ(vm.var(V(0)), 0xFFFF);
  CHECK_EQ(vm.var(0x30), 77);
  // Locals vanish on room load, globals persist.
  Asm empty_script;
  empty_script.stop();
  vm.load(make_module(empty_script, {{{}}}));
  CHECK_EQ(vm.var(V(0)), 0);
  CHECK_EQ(vm.var(0x30), 77);
  // The engine sets variable 0x46 = 1 at room load.
  CHECK_EQ(vm.var(0x46), 1);
}

void test_bit_ops() {
  Asm script;
  const u16 init = script.here();
  // 71 / 72 / 73: operand n = (variable id << 4) | bit, literal (M = 0x80).
  const int n1 = (V(1) << 4) | 3, n2 = (V(1) << 4) | 9;
  script.raw({0x71}).word(n1).raw({0x80});
  script.raw({0x71}).word(n2).raw({0x80});
  // Clear bit 3 again.
  script.raw({0x72}).word(n1).raw({0x80});
  // bit 9 set -> falls through (SET V(2) = 1); bit 3 clear -> jumps over (V(3) stays 0)
  const size_t j1 = script.code.size();
  script.raw({0x73}).word(n2).raw({0x80}).word(0);
  script.set(V(2), 1);
  const size_t j2 = script.code.size();
  script.raw({0x73}).word(n1).raw({0x80}).word(0);
  // Skipped by the second test: bit 3 is clear -> jump.
  script.set(V(3), 1);
  const u16 end = script.here();
  script.stop();
  script.code[j2 + 4] = static_cast<u8>(end);
  script.code[j2 + 5] = static_cast<u8>(end >> 8);
  script.code[j1 + 4] = static_cast<u8>(j2);
  script.code[j1 + 5] = static_cast<u8>(j2 >> 8);
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(1)), 1 << 9);
  CHECK(vm.var_bit(static_cast<u32>(n2)));
  CHECK(!vm.var_bit(static_cast<u32>(n1)));
  // Bit set: 73 falls through. Bit clear: 73 jumped.
  CHECK_EQ(vm.var(V(2)), 1);
  CHECK_EQ(vm.var(V(3)), 0);
}

// IF_JUMP: execution falls through when the condition is TRUE and jumps when it is FALSE.
void test_if_jump_polarity() {
  for (int value : {5, 6}) {
    Asm script;
    const u16 init = script.here();
    script.set(V(0), value);
    const size_t at = script.code.size();
    // if (v == 5) { then } else goto else.
    script.if_jump(V(0), 0, 5, 0);
    script.set(V(1), 1);
    const size_t jmp = script.code.size();
    script.jump(0);
    const u16 els = script.here();
    script.set(V(1), 2);
    const u16 end = script.here();
    script.stop();
    script.code[at + 6] = static_cast<u8>(els);
    script.code[at + 7] = static_cast<u8>(els >> 8);
    script.code[jmp + 1] = static_cast<u8>(end);
    script.code[jmp + 2] = static_cast<u8>(end >> 8);
    NullHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{0, init}}}}));
    CHECK_EQ(vm.var(V(1)), value == 5 ? 1 : 2);
  }
  // All conditions: (x=3, y=5): ==F !=T >F <T >=F <=T and: 3&5=1 T
  const bool expect[7] = {false, true, false, true, false, true, true};
  for (int cond = 0; cond < 7; ++cond) {
    Asm script;
    const u16 init = script.here();
    script.set(V(0), 3);
    const size_t at = script.code.size();
    script.if_jump(V(0), cond, 5, 0);
    script.set(V(1), 1);
    const u16 end = script.here();
    script.stop();
    script.code[at + 6] = static_cast<u8>(end);
    script.code[at + 7] = static_cast<u8>(end >> 8);
    NullHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{0, init}}}}));
    CHECK_EQ(vm.var(V(1)), expect[cond] ? 1 : 0);
  }
}

void test_call_return() {
  Asm script;
  const size_t at = script.code.size();
  // main = INIT.
  script.call(0).call(0).stop();
  const u16 sub = script.here();
  script.inc(V(0)).ret();
  script.code[at + 1] = script.code[at + 4] = static_cast<u8>(sub);
  script.code[at + 2] = script.code[at + 5] = static_cast<u8>(sub >> 8);
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, static_cast<u16>(at)}}}}));
  CHECK_EQ(vm.var(V(0)), 2);
  CHECK_EQ(vm.stats().call_overflows, 0u);

  // Recursion: the stack holds 4 return addresses; the 5th CALL stalls the script permanently.
  Asm recursive;
  const u16 rs = recursive.here();
  // The target is patched below.
  recursive.call(0);
  recursive.stop();
  const u16 body = recursive.here();
  recursive.inc(V(0)).call(body).ret();
  recursive.code[rs + 1] = static_cast<u8>(body);
  recursive.code[rs + 2] = static_cast<u8>(body >> 8);
  FieldVM vm2(host);
  vm2.load(make_module(recursive, {{{{0, rs}}}}));
  CHECK_EQ(vm2.var(V(0)), 4);
  CHECK_EQ(vm2.stats().call_overflows, 1u);
  // RETURN at depth 0 behaves like STOP.
  Asm underflow;
  const u16 zs = underflow.here();
  underflow.inc(V(0)).ret().inc(V(0)).stop();
  FieldVM vm3(host);
  vm3.load(make_module(underflow, {{{{0, zs}}}}));
  CHECK_EQ(vm3.var(V(0)), 1);
}

void test_budget_and_default_restart() {
  Asm script;
  const u16 upd = script.here();
  for (int i = 0; i < 20; ++i) script.inc(V(0));
  script.park();
  const u16 upd2 = script.here();
  // The script ends and restarts from the top next tick.
  script.inc(V(1)).stop();
  TestHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, upd}}}, {{{1, upd}}}, {{{1, upd2}}}}));
  vm.tick();
  // Two entities, 8 instructions each; the third entity runs INC then STOP in one go.
  CHECK_EQ(vm.var(V(0)), 16);
  CHECK_EQ(vm.var(V(1)), 1);
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 32);
  vm.tick();
  // Each entity stops at its 20 INCs and parks on 3B.
  CHECK_EQ(vm.var(V(0)), 40);
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 40);
  // Default script restart: STOP -> restarted every tick.
  CHECK_EQ(vm.var(V(1)), 4);
}

void test_wait_scheduling() {
  // WAIT n takes n+1 ticks; SET follows on the next tick.
  {
    Asm script;
    const u16 upd = script.here();
    script.wait(2).set(V(0), 1).park();
    NullHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{1, upd}}}}));
    for (int wait_tick = 1; wait_tick <= 3; ++wait_tick) {
      vm.tick();
      CHECK_EQ(vm.var(V(0)), 0);
    }
    vm.tick();
    CHECK_EQ(vm.var(V(0)), 1);
  }
  // Walk start form, then the wait form polls the host until the move is done.
  {
    Asm script;
    const u16 upd = script.here();
    script.raw({0x3F, 0x00});
    // S80 S40 S20 M (all literals).
    script.word(10).word(20).word(30).raw({0xE0});
    script.raw({0x3F, 0x01});
    script.set(V(0), 1).park();
    TestHost host;
    host.motion_polls_busy = 3;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{1, upd}}}}));
    for (int wait_tick = 1; wait_tick <= 3; ++wait_tick) {
      vm.tick();
      CHECK_EQ(vm.var(V(0)), 0);
    }
    vm.tick();
    CHECK_EQ(vm.var(V(0)), 1);
    CHECK_EQ(host.motions_started, 1);
  }
  // Dialogue: E0 is retried while a window is open, E2 waits until it closes and stores the result.
  {
    Asm script;
    const u16 upd = script.here();
    script.raw({0xE0}).word(7).raw({0x00});
    script.raw({0xE2});
    script.set(V(0), 1).park();
    TestHost host;
    host.dialogue_ticks = 0;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{1, upd}}}}));
    vm.tick();
    CHECK_EQ(host.opened, 1);
    vm.tick();
    CHECK_EQ(vm.var(V(0)), 1);
  }
  // WAIT_FRAME in INIT is a no-op (run-to-completion mode ignores yields).
  {
    Asm script;
    const u16 init = script.here();
    script.wait_frame().wait_frame().set(V(0), 9).stop();
    NullHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{0, init}}}}));
    CHECK_EQ(vm.var(V(0)), 9);
  }
}

void test_priority_preemption() {
  Asm script;
  const u16 upd = script.here();
  const u16 loop = upd;
  script.inc(V(0)).wait_frame().jump(loop);
  const u16 action = script.here();
  script.set(V(1), 1).wait_frame().wait_frame().set(V(1), 2).stop();
  const u16 collision = script.here();
  script.inc(V(2)).stop();
  NullHost host;
  FieldVM vm(host);
  // Entity 0 is the "director" (never triggerable); entity 1 has all scripts; entity 2 only UPDATE.
  vm.load(make_module(script, {{{}}, {{{1, upd}, {2, action}, {3, collision}}}, {{{1, upd}}}}));
  CHECK(!vm.trigger_action(0));
  // Entity 2 has no ON_ACTION script: flagged at load.
  CHECK(!vm.trigger_action(2));
  CHECK(!vm.trigger_collision(2));
  // UPDATE of entities 1 and 2 starts.
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 2);
  CHECK(vm.trigger_action(1));
  // ACTION (class 3) outranks UPDATE (class 7).
  vm.tick();
  CHECK_EQ(vm.var(V(1)), 1);
  vm.tick();
  vm.tick();
  // The action script finished and was released; entity 2 kept running (+3) while entity 1 stayed suspended.
  CHECK_EQ(vm.var(V(1)), 2);
  CHECK_EQ(vm.var(V(0)), 2 + 3);
  vm.tick();
  // Entity 1's UPDATE resumes where it was (+1), entity 2 (+1).
  CHECK_EQ(vm.var(V(0)), 2 + 3 + 2);
  // Already-running script is not started twice; action (3) beats collision (4) when both are pending.
  CHECK(vm.trigger_collision(1));
  CHECK(vm.trigger_action(1));
  CHECK(!vm.trigger_action(1));
  vm.tick();
  CHECK_EQ(vm.var(V(1)), 1);
  // The collision script waits behind the action script, then runs after it ended.
  CHECK_EQ(vm.var(V(2)), 0);
  vm.tick();
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.var(V(2)), 1);
  // Input lock refuses ON_ACTION.
  vm.set_global_flags(vm.global_flags() | gflag::kInputLock);
  CHECK(!vm.trigger_action(1));
}

void test_blocking_run_script() {
  Asm script;
  const u16 upd = script.here();
  // RUN_SCRIPT_BLOCKING THIS, script 5, class 1.
  script.raw({0x09, 0xFB, (1 << 5) | 5});
  script.set(V(0), 1).park();
  const u16 child = script.here();
  script.wait_frame().wait_frame().set(V(1), 1).stop();
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, upd}, {5, child}}}}));
  vm.tick();
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.var(V(1)), 0);
  vm.tick();
  CHECK_EQ(vm.var(V(1)), 1);
  // The parent only continues once the child is idle.
  CHECK_EQ(vm.var(V(0)), 0);
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 1);
}

// 08 RUN_SCRIPT_AND_WAIT waits until the child is the target's running record, not until it ends: the child here
// loops forever (tel_b11i ent 5 script 4 shape) and the parent must still continue.
void test_run_script_and_wait() {
  Asm script;
  const u16 upd = script.here();
  // RUN_SCRIPT_AND_WAIT ent1, script 5, class 1.
  script.raw({0x08, 0x01, (1 << 5) | 5});
  script.set(V(0), 1).park();
  const u16 upd1 = script.here();
  script.park();
  const u16 child = script.here();
  script.inc(V(1)).wait_frame().jump(child);
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, upd}}}, {{{1, upd1}, {5, child}}}}));
  for (int i = 0; i < 4; ++i) vm.tick();
  // The child runs (and keeps running); the parent went on.
  CHECK(vm.var(V(1)) >= 2);
  CHECK_EQ(vm.var(V(0)), 1);
}

// 99 reads the party table, whose empty slots hold 0xFF (tel_b02f painter: `$room[8] == #255`).
// E2 copies the confirmed choice *line*; a plain message leaves the previous value.
class ChoiceHost : public NullHost {
 public:
  int result = -1;
  int party_member(int slot) override { return slot == 0 ? 0 : slot == 1 ? 1 : -1; }
  int dialogue_result() override { return result; }
};

void test_party_slot_and_choice_result() {
  Asm script;
  const u16 init = script.here();
  // GET_PARTY_CHARACTER V0, slot 2; then V1, slot 1; FIND_PARTY_SLOT V2, Kid.
  script.raw({0x99}).word(V(0)).word(I(2));
  script.raw({0x99}).word(V(1)).word(I(1));
  script.raw({0x98}).word(V(2)).word(I(1));
  script.stop();
  const u16 upd = script.here();
  script.raw({0xE2});
  script.alu_v(0x10, V(3), 0x04);
  script.raw({0xE2});
  script.alu_v(0x10, V(4), 0x04);
  script.park();
  ChoiceHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}, {1, upd}}}}));
  CHECK_EQ(vm.var(V(0)), 255);
  CHECK_EQ(vm.var(V(1)), 1);
  CHECK_EQ(vm.var(V(2)), 1);
  // "No thanks" = line 3 of painter message 37.
  host.result = 3;
  vm.tick();
  // The next window has no choice.
  host.result = -1;
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.var(V(3)), 3);
  CHECK_EQ(vm.var(V(4)), 3);
}

// E3 / E4 with a "none" speaker (an empty party slot) skip themselves and the wait that follows (pc += 6 / pc += 8)
// instead of opening the window: hid_b04f's plant asks "{PARTY2%}: Are ya alright?" through E3 FD, which a two-member party must not hear.
void test_dialogue_none_speaker() {
  Asm script;
  const u16 init = script.here();
  // E3 PC2 (empty slot), msg 1, flags 8; E2.
  script.raw({0xE3, 0xFD, 0x01, 0x00, 0x08, 0xE2});
  script.set(V(0), 7);
  // E4 PC2: skips 8 bytes (the E2 and the SET below are not part of it).
  script.raw({0xE4, 0xFD, 0x02, 0x00, 0x08, 0, 0, 0});
  script.set(V(1), 9);
  // Entity 0 exists: the window opens.
  script.raw({0xE3, 0x00, 0x03, 0x00, 0x08, 0xE2});
  script.stop();
  TestHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(0)), 7);
  CHECK_EQ(vm.var(V(1)), 9);
  CHECK_EQ(host.opened, 1);
}

// Host with a real party table: SETUP_PC binds by character reference, 7B fills the first empty slot.
class PartyHost : public NullHost {
 public:
  std::vector<int> party{0};
  std::vector<int> setup_calls;
  std::vector<int> placed_on_leader;
  int party_member(int slot) override { return slot < static_cast<int>(party.size()) ? party[static_cast<size_t>(slot)] : -1; }
  bool party_add(int char_id) override {
    if (party.size() >= 3) return false;
    party.push_back(char_id);
    return true;
  }
  void setup_pc(int ent, int, int) override { setup_calls.push_back(ent); }
  void place_on_controlled(int ent) override { placed_on_leader.push_back(ent); }
  void place_followers() override { ++followers_placed; }
  int followers_placed = 0;
};

// 7C after a 7B re-runs the INIT of the first entity whose SETUP_PC names the new member through the party table as it is now
// (an `0E FE` that found slot 1 empty at the room load), clears the flags the failed SETUP_PC left, puts it on the controlled
// entity and lets the running script go on after the 7C.
void test_commit_relinks_new_member() {
  Asm script;
  const u16 noop = script.here();
  script.stop();
  const u16 upd = script.here();
  script.raw({0x7B}).word(I(1)).raw({0x7C});
  script.set(V(0), 5);
  script.park();
  const u16 lead = script.here();
  script.raw({0x0E, 0xFD, 0x80}).stop();
  const u16 second = script.here();
  script.raw({0x0E, 0xFE, 0x80}).stop();
  const u16 third = script.here();
  script.raw({0x0E, 0xFF, 0x80}).stop();
  PartyHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, noop}, {1, upd}}}, {{{0, lead}}}, {{{0, second}}}, {{{0, third}}}}));
  CHECK_EQ(host.setup_calls.size(), 3);
  vm.entity(2).flags38 |= flag38::kInactive;
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 5);
  CHECK_EQ(host.setup_calls.size(), 4);
  CHECK_EQ(host.setup_calls.back(), 2);
  CHECK_EQ(host.placed_on_leader.size(), 1);
  CHECK_EQ(host.placed_on_leader.front(), 2);
  CHECK_EQ(host.followers_placed, 1);
  CHECK((vm.entity(2).flags38 & flag38::kInactive) == 0);
  CHECK_EQ(vm.entity(2).rec[0].state, kRecIdle);
}

// C1 / BF resolve their entity operand like any entity reference: `C1 00 FF` loads onto the leader's entity (party slot 0), and
// only a reference that resolves to no entity (an empty party slot) skips the start and stop forms and yields.
class AnimPackHost : public NullHost {
 public:
  std::vector<int> loads;
  int party_entity(int slot) override { return slot == 0 ? 1 : -1; }
  void extra_animation(u16 key, bool start, int ent, int arg) override {
    if (start && key == 0xC1) loads.push_back(ent * 0x1000 + arg);
  }
};

void test_load_animation_resolves_entity() {
  Asm script;
  const u16 noop = script.here();
  script.stop();
  const u16 upd = script.here();
  script.raw({0xC1, 0x00, 0xFF}).word(I(0)).raw({0xC1, 0x01});
  script.set(V(0), 1);
  script.raw({0xC1, 0x00, 0xFE}).word(I(0x105)).raw({0xC1, 0x01});
  script.set(V(1), 1);
  script.park();
  AnimPackHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, noop}, {1, upd}}}, {{{0, noop}}}}));
  for (int i = 0; i < 4; ++i) vm.tick();
  CHECK_EQ(host.loads.size(), 1);
  CHECK_EQ(host.loads.front(), 0x1000);
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(vm.var(V(1)), 1);
}

void test_scenario_and_rng() {
  Asm script;
  const u16 init = script.here();
  // SET_SCENARIO 10; 69 jumps when SCENARIO >= 10 (patched); the SET after it is skipped; GET_SCENARIO; RAND_MOD 7.
  script.raw({0x6C}).word(I(10));
  script.raw({0x69}).word(I(10)).word(0);
  const size_t j = script.code.size() - 2;
  script.set(V(0), 1);
  const u16 target = script.here();
  script.raw({0x6D}).word(V(1));
  script.raw({0x1F}).word(V(2)).word(I(7));
  script.stop();
  script.code[j] = static_cast<u8>(target);
  script.code[j + 1] = static_cast<u8>(target >> 8);
  NullHost host;
  u32 seq = 100;
  FieldVM vm(host, [&] { return seq++; });
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(0)), 0);
  CHECK_EQ(vm.var(V(1)), 10);
  CHECK_EQ(vm.var(0x00), 10);
  // RAND_MOD n gives 0..n inclusive.
  CHECK_EQ(vm.var(V(2)), 100 % 8);
}

// Every table row decodes to a legal length and has a VM handler; linear ops advance exactly their length.
void test_opcode_table() {
  int rows = 0;
  std::vector<std::string> nonlinear;
  for (const OpInfo& op : op_table()) {
    ++rows;
    Asm script;
    // Jumps with zero operands land on a STOP.
    script.code[0] = 0x00;
    const u16 start = script.here();
    if (op.key > 0xFF)
      script.raw({0xFE, op.key & 0xFF});
    else
      script.raw({op.key});
    for (int i = 0; i < 24; ++i) script.raw({0});
    const auto insn = decode_insn(script.code, start);
    CHECK(insn.has_value());
    if (!insn) continue;
    bool len_ok = false;
    for (const char* cursor = op.lengths; *cursor;) {
      char* end = nullptr;
      const long length = std::strtol(cursor, &end, 10);
      if (static_cast<u32>(length) == insn->length) len_ok = true;
      cursor = *end == '/' ? end + 1 : end;
    }
    CHECK(len_ok);
    if (op.key == 0x00) continue;
    NullHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{1, start}}}}));
    vm.tick();
    CHECK_EQ(vm.stats().unimplemented, 0u);
    CHECK(vm.stats().unknown_ops.empty());
    const u32 after = vm.entity(0).rec[0].pc;
    if (after != start + insn->length) nonlinear.push_back(op.name);
  }
  CHECK(rows > 340);
  std::printf("opcode table: %d rows; ops whose first execution does not fall through to pc+len: %zu\n", rows, nonlinear.size());
  for (const auto& name : nonlinear) std::printf(" %s", name.c_str());
  std::printf("\n");
}


// GET_POSITION (48) writes X, Z, Y in operand order, like the published player variables 0x12 / 0x14 / 0x16.
class PosHost : public NullHost {
 public:
  // The position is (x, y, z).
  Vec3 get_position(int) override { return {10, 20, 30}; }
};

void test_get_position_order() {
  Asm script;
  const u16 init = script.here();
  // GET_POSITION ent0, V0, V1, V2.
  script.raw({0x48, 0x00}).word(V(0)).word(V(1)).word(V(2));
  script.stop();
  PosHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  // X, Z, Y in operand order.
  CHECK_EQ(vm.var(V(0)), 10);
  CHECK_EQ(vm.var(V(1)), 30);
  CHECK_EQ(vm.var(V(2)), 20);
}

// CA reads the operands of the walk two bytes on (`CA CB 42 b X Z M ...`): the first call turns toward (X, Z) (absolute for
// b == 0, plus the entity's position for b == 2) and requests the scripted turn (flag bits 13 + 14); the second call, bit 14 still set,
// goes on to the CB.
class FaceHost : public NullHost {
 public:
  int ent = -1;
  s32 x = 0, z = 0;
  int calls = 0;
  Vec3 get_position(int) override { return {1000, 0, 2000}; }
  void face_point(int entity, s32 px, s32 pz) override { ent = entity; x = px; z = pz; ++calls; }
};

void test_ca_faces_the_next_walk() {
  for (int mode : {0, 2}) {
    Asm script;
    const u16 init = script.here();
    // CA, CB, WALK_TO_XZ b, #-67, #-55, #0.
    script.raw({0xCA, 0xCB, 0x42, mode, 0xBD, 0xFF, 0xC9, 0xFF, 0xC0, 0x00, 0x80});
    script.set(V(0), 7).stop();
    FaceHost host;
    FieldVM vm(host);
    // Entity 0 UPDATE (an INIT runs to completion and would not stop at the yield).
    vm.load(make_module(script, {{{{1, init}}}}));
    vm.tick();
    CHECK_EQ(host.calls, 1);
    CHECK_EQ(host.ent, 0);
    CHECK_EQ(host.x, mode == 2 ? 1000 - 67 : -67);
    CHECK_EQ(host.z, mode == 2 ? 2000 - 55 : -55);
    CHECK_EQ(vm.entity(0).flags270 & 0x6000, 0x6000);
    CHECK_EQ(vm.var(V(0)), 0);
    vm.tick();
    // The second call only advances.
    CHECK_EQ(host.calls, 1);
  }
}

// RUN_SCRIPT 07 / 08 / 09 on an inactive entity (an empty party slot, a character not in the party) skip at once.
// The script would never run there, so 09 / 08 would wait for ever.
void test_run_script_inactive_target() {
  Asm script;
  // Ent 0 UPDATE: runs after ent 1's INIT made it inactive.
  const u16 u0 = script.here();
  script.raw({0x09, 0x01, 0x05}).raw({0x08, 0x01, 0x05}).raw({0x07, 0x01, 0x05}).set(V(0), 7).stop();
  // Ent 1 INIT: SET_VISIBLE 0, THIS (marks it inactive).
  const u16 i1 = script.here();
  script.raw({0x36, 0x00, 0xFB}).stop();
  const u16 child = script.here();
  script.wait(100).stop();
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, u0}}}, {{{0, i1}, {5, child}}}}));
  vm.tick();
  CHECK(vm.entity(1).flags38 & flag38::kInactive);
  CHECK_EQ(vm.var(V(0)), 7);
}

// FE 4F registers a slot entity, FE 50 restores the camera (variables 0x330 / 0x332) and, at the end of INIT, the registered entities
// from variables 0x300 + 6k (X, Z, facing) - the world-map Continue.
class RestoreHost : public NullHost {
 public:
  int ent = -1, x = 0, z = 0, face = -1;
  s32 cam_x = -1, cam_y = -1;
  void set_position_xz(int entity, s32 px, s32 pz, int) override { ent = entity; x = px; z = pz; }
  void set_facing(int, int angle) override { face = angle; }
  void set_camera_origin(s32 px, s32 py) override { cam_x = px; cam_y = py; }
};

void test_fe50_restore() {
  Asm script;
  const u16 init = script.here();
  script.set(0x300 + 6, 123).set(0x302 + 6, -45).set(0x304 + 6, 0x400).set(0x330, 77).set(0x332, 88);
  // Slot 1 = entity 1.
  script.raw({0xFE, 0x4F, 0x01, 0x01, 0x80});
  script.raw({0xFE, 0x50}).stop();
  RestoreHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}, {{}}}));
  CHECK_EQ(host.cam_x, 77);
  CHECK_EQ(host.cam_y, 88);
  CHECK(vm.world_continue());
  // The entity restore is done before the first tick (the room entry places the camera on the restored party).
  CHECK_EQ(host.ent, 1);
  CHECK_EQ(host.x, 123);
  CHECK_EQ(host.z, -45);
  CHECK_EQ(host.face, 0x400);
}

// Softlock watchdog bookkeeping: a record that yields on one wait instruction is counted (strict), a polling loop of a class < 7 script is
// counted (loop), WAIT / STOP_EXEC and the idle UPDATE loop are not.
void test_stuck_watchdog() {
  Asm script;
  // Ent 0 UPDATE: WAIT_DIALOG on a window that never closes.
  const u16 waiter = script.here();
  script.raw({0xE2}).stop();
  // Ent 1 UPDATE: a long WAIT.
  const u16 timer = script.here();
  script.wait(5000).stop();
  // Ent 2 UPDATE: the usual idle loop.
  const u16 idle = script.here();
  script.wait_frame().jump(idle);
  // Ent 2 ON_ACTION: polls a variable nobody sets.
  const u16 action = script.here();
  const u16 poll = script.here();
  script.wait_frame().if_jump(V(0), 1, 0, poll).stop();
  TestHost host;
  host.dialogue_ticks = 1 << 30;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, waiter}}}, {{{1, timer}}}, {{{1, idle}, {2, action}}}}));
  vm.trigger_action(2);
  for (int i = 0; i < 100; ++i) vm.tick();
  const auto stuck = vm.stuck_records(50);
  bool found_wait = false, found_poll = false, other = false;
  for (const auto& record : stuck) {
    if (record.ent == 0 && record.key == 0xE2 && record.strict) found_wait = true;
    else if (record.ent == 2 && record.cls == 3 && !record.strict) found_poll = true;
    else other = true;
  }
  if (!found_poll || other)
    for (const auto& record : stuck) std::printf("  stuck: %s\n", FieldVM::describe(record).c_str());
  CHECK(found_wait);
  CHECK(found_poll);
  CHECK(!other);
  CHECK(vm.stuck_records(500).empty());
  CHECK(!FieldVM::describe(stuck[0]).empty());
}
}  // namespace

// Cheats "No encounters": a skipped A3 runs entity 0's slot 2 (RDE's patched "09 00 02") and goes on without request_battle.
void test_skipped_battle() {
  struct SkipHost : TestHost {
    bool skip = true;
    int skipped = 0, requested = 0;
    bool skip_battle(int, bool, int) override { ++skipped; return skip; }
    void request_battle(int, int, int) override { ++requested; }
    bool battle_active() override { return false; }
  };
  for (const bool skip : {true, false}) {
    Asm script;
    const u16 init = script.here();
    script.raw({0xA3}).word(I(5)).word(I(0)).word(I(0));
    script.set(V(0), 1).park();
    const u16 post = script.here();
    script.set(V(1), 1).stop();
    SkipHost host;
    host.skip = skip;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{1, init}, {2, post}}}}));
    for (int tick = 0; tick < 20; ++tick) vm.tick();
    CHECK_EQ(host.skipped, 1);
    CHECK_EQ(host.requested, skip ? 0 : 1);
    // Entity 0's slot 2 ran only for the skipped battle; the script went on past the op.
    CHECK_EQ(vm.var(V(1)), skip ? 1 : 0);
    CHECK_EQ(vm.var(V(0)), 1);
  }
}

// The screen ops (FE 5B party screen, FE 72 save-point main menu) open their screen and step on; the FE 61 after each only waits
// while it is pending or open, and a bare FE 61 opens nothing.
void test_screen_ops_and_fe61() {
  struct MenuHost : TestHost {
    std::vector<MenuKind> requests;
    bool open = false;
    void request_menu(MenuKind kind, int) override {
      requests.push_back(kind);
      open = true;
    }
    bool menu_active() override { return open; }
  };
  for (const int screen_op : {0x5B, 0x72}) {
    Asm script;
    const u16 init = script.here();
    script.raw({0xFE, screen_op, 0xFE, 0x61});
    script.set(V(0), 1);
    script.raw({0xFE, 0x61});
    script.set(V(1), 1).park();
    MenuHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{1, init}}}}));
    for (int tick = 0; tick < 10; ++tick) vm.tick();
    CHECK_EQ(host.requests.size(), 1);
    CHECK(host.requests[0] == (screen_op == 0x5B ? MenuKind::PartyScreen : MenuKind::MainMenu));
    CHECK_EQ(vm.var(V(0)), 0);
    host.open = false;
    for (int tick = 0; tick < 10; ++tick) vm.tick();
    CHECK_EQ(host.requests.size(), 1);
    CHECK_EQ(vm.var(V(0)), 1);
    CHECK_EQ(vm.var(V(1)), 1);
  }
}

int main() {
  test_arithmetic();
  test_movie_end_jump();
  test_skipped_battle();
  test_screen_ops_and_fe61();
  test_variables_and_signedness();
  test_bit_ops();
  test_if_jump_polarity();
  test_call_return();
  test_budget_and_default_restart();
  test_wait_scheduling();
  test_priority_preemption();
  test_blocking_run_script();
  test_run_script_and_wait();
  test_party_slot_and_choice_result();
  test_dialogue_none_speaker();
  test_commit_relinks_new_member();
  test_load_animation_resolves_entity();
  test_scenario_and_rng();
  test_opcode_table();
  test_get_position_order();
  test_ca_faces_the_next_walk();
  test_run_script_inactive_target();
  test_fe50_restore();
  test_stuck_watchdog();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("script tests ok\n");
  return 0;
}
