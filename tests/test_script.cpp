// Field script VM tests: hand-assembled snippets run through FieldVM with a NullHost / scripted host.
#include <array>
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
    void play_movie(int, int) override { ++movies; }
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

// A7 and AB only step over their operands (the debug monitor is off in the retail game); FE 4D sets the Vibration option.
void test_debug_calls_and_vibration() {
  class OptionHost : public NullHost {
   public:
    int opaque = 0, vibration = -1;
    void opaque_op(u16, std::span<const u8>) override { ++opaque; }
    void set_global(Global which, s32 value) override {
      if (which == Global::Vibration) vibration = value;
    }
  };
  Asm script;
  const u16 init = script.here();
  script.raw({0xA7}).word(I(1)).word(I(2)).word(I(3)).word(I(4));
  script.raw({0xAB}).word(I(5)).word(I(6));
  script.raw({0xFE, 0x4D, 0x01});
  script.stop();
  OptionHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(host.opaque, 0);
  CHECK_EQ(host.vibration, 1);
}

// 7E and D4 take the table row from the low byte of their first operand; D4's movie flags are its whole second operand, 7E's are 0.
void test_movie_operands() {
  class MovieHost : public NullHost {
   public:
    std::vector<int> rows, flags;
    void play_movie(int index, int movie_flags) override {
      rows.push_back(index);
      flags.push_back(movie_flags);
    }
  };
  Asm script;
  const u16 init = script.here();
  script.raw({0xD4}).word(I(0x106)).word(I(0x20B));
  script.raw({0x7E}).word(I(0x105));
  script.stop();
  MovieHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(host.rows.size(), 2);
  if (host.rows.size() != 2) return;
  CHECK_EQ(host.rows[0], 6);
  CHECK_EQ(host.flags[0], 0x20B);
  CHECK_EQ(host.rows[1], 5);
  CHECK_EQ(host.flags[1], 0);
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
  // V6: a zero divisor counts as 1, the variable keeps its value; V7: the quotient truncates toward zero.
  script.set(V(6), 9).alu(0x21, V(6), 0);
  script.set(V(7), -7).alu(0x21, V(7), 2);
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
  CHECK_EQ(vm.var(V(6)), 9);
  CHECK_EQ(vm.var(V(7)), -3);
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

// IF_JUMP with two variables: the right value is extended by the left variable's signedness. V(0) is signed, V(1) unsigned.
void test_if_jump_mixed_signedness() {
  struct Case {
    u16 lhs, rhs;
    int cond;
    bool taken;
  };
  // 5 > V(1) = 0xFFFF read as -1: true. 5 < V(0) = 0xFFFF read as 65535: true.
  for (const Case& test : {Case{V(0), V(1), 2, true}, Case{V(1), V(0), 3, true}}) {
    Asm script;
    const u16 init = script.here();
    script.set(V(0), 5).set(V(1), -1);
    if (test.lhs == V(1)) script.set(V(0), -1).set(V(1), 5);
    const size_t at = script.code.size();
    script.raw({0x02}).word(test.lhs).word(test.rhs).raw({test.cond}).word(0);
    script.set(V(2), 1);
    const u16 end = script.here();
    script.stop();
    script.code[at + 6] = static_cast<u8>(end);
    script.code[at + 7] = static_cast<u8>(end >> 8);
    FieldScriptModule module = make_module(script, {{{{0, init}}}});
    module.sign_mask[16] = 2;
    NullHost host;
    FieldVM vm(host);
    vm.load(module);
    CHECK_EQ(vm.var(V(2)), test.taken ? 1 : 0);
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

// A host music cue replaces WAIT's own counter (it holds until the cue reports the end, with the WAIT's tick count passed on), and
// holds op 86 before it runs, passing the code word 4 bytes after the op.
void test_music_cues() {
  class CueHost : public NullHost {
   public:
    int wait_calls = 0, wait_ticks = -1, holds_left = 3, hold_op = -1;
    u32 hold_word = 0;
    int music_wait(int ticks) override {
      wait_ticks = ticks;
      return ++wait_calls >= 5 ? 1 : 0;
    }
    bool music_hold(int op, u32 word) override {
      hold_op = op;
      hold_word = word;
      return holds_left-- > 0;
    }
  };
  Asm script;
  const u16 upd = script.here();
  script.wait(345).set(V(0), 1);
  script.raw({0x86, 0x00, 0x06, 0x06, 0x2B, 0x01, 0x36, 0x00, 0xFB}).set(V(1), 1).park();
  CueHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, upd}}}}));
  for (int tick = 1; tick <= 4; ++tick) vm.tick();
  CHECK_EQ(vm.var(V(0)), 0);
  CHECK_EQ(host.wait_ticks, 345);
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(host.hold_op, 0x86);
  CHECK_EQ(host.hold_word, 0x0036012B);
  CHECK_EQ(vm.var(V(1)), 0);
  for (int tick = 0; tick < 4; ++tick) vm.tick();
  CHECK_EQ(vm.var(V(1)), 1);
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

// E2 of an entity culled off screen: its window closes and the record ends, so the op after E2 does not run; a window opened as
// kept (op 62 flag 0x400) is waited for as usual, as is the window of an entity on screen.
class CulledHost : public NullHost {
 public:
  bool open = true, kept = false;
  int drops = 0;
  bool dialogue_open() override { return open; }
  bool drop_culled_dialogue() override {
    if (kept) return false;
    ++drops;
    open = false;
    return true;
  }
};

void test_e2_culled_speaker() {
  for (const int kind : {0, 1, 2}) {
    Asm script;
    const u16 init = script.here();
    script.stop();
    const u16 upd = script.here();
    script.raw({0xE2});
    script.set(V(0), 1);
    script.park();
    CulledHost host;
    host.kept = kind == 1;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{0, init}, {1, upd}}}}));
    if (kind != 2) vm.entity(0).flags38 |= flag38::kOffScreen;
    vm.tick();
    CHECK_EQ(vm.var(V(0)), 0);
    CHECK_EQ(host.drops, kind == 0 ? 1 : 0);
    CHECK_EQ(host.open, kind != 0);
    if (kind == 0) continue;
    host.open = false;
    vm.tick();
    vm.tick();
    CHECK_EQ(vm.var(V(0)), 1);
  }
}

// 99 reads the party table, whose empty slots hold 0xFF (tel_b02f painter: `$room[8] == #255`).
// E0 resets the entity's dialogue result byte to 0xFF; the host writes the confirmed choice *line* into it while the window is
// still open, and E2 copies the byte once the window is gone, so a plain message yields 0xFF.
class ChoiceHost : public NullHost {
 public:
  bool open = false;
  int party_member(int slot) override { return slot == 0 ? 0 : slot == 1 ? 1 : -1; }
  void open_dialogue(int, int, int) override { open = true; }
  bool dialogue_open() override { return open; }
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
  script.raw({0xE0}).word(37).raw({0x00});
  script.raw({0xE2});
  script.alu_v(0x10, V(3), 0x04);
  script.raw({0xE0}).word(7).raw({0x00});
  script.raw({0xE2});
  script.alu_v(0x10, V(4), 0x04);
  script.park();
  ChoiceHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}, {1, upd}}}}));
  CHECK_EQ(vm.var(V(0)), 255);
  CHECK_EQ(vm.var(V(1)), 1);
  CHECK_EQ(vm.var(V(2)), 1);
  vm.tick();
  CHECK_EQ(vm.entity(0).last_choice, 0xFF);
  // "No thanks" = line 3 of painter message 37, confirmed: the host stores it, then the window closes.
  vm.entity(0).last_choice = 3;
  host.open = false;
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.var(V(3)), 3);
  CHECK(host.open);
  // The next window has no choice.
  host.open = false;
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.var(V(4)), 255);
}

// 04 resets the running entity's records but keeps its call depth; the call depth and action bit it clears are entity 8's.
void test_kill_all_scripts() {
  Asm script;
  script.stop();
  const u16 sub = script.here();
  script.raw({0x04});
  const u16 upd = script.here();
  script.call(sub);
  script.park();
  NullHost host;
  FieldVM vm(host);
  std::vector<EntScripts> ents(9);
  ents[0].slot = {{1, upd}};
  vm.load(make_module(script, ents));
  vm.entity(8).depth = 2;
  vm.entity(8).flags270 |= flag270::kActionRunning;
  vm.tick();
  CHECK_EQ(vm.entity(0).depth, 1);
  CHECK_EQ(vm.entity(8).depth, 0);
  CHECK((vm.entity(8).flags270 & flag270::kActionRunning) == 0);
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

// FREE_EXTRA_ANIMATION (C0) waits without advancing while the entity's pack is still in use (a pack clip in force or a cross-fade
// running), then frees it.
class PackInUseHost : public NullHost {
 public:
  bool in_use = true;
  std::vector<int> frees;
  bool extra_pack_in_use(int ent) override { return in_use && ent == 1; }
  void extra_animation(u16 key, bool, int ent, int) override {
    if (key == 0xC0) frees.push_back(ent);
  }
};

void test_free_animation_waits_for_pack() {
  Asm script;
  const u16 noop = script.here();
  script.stop();
  const u16 upd = script.here();
  script.raw({0xC0, 0x01});
  script.set(V(0), 1);
  script.park();
  PackInUseHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, noop}, {1, upd}}}, {{{0, noop}}}}));
  for (int i = 0; i < 3; ++i) vm.tick();
  CHECK_EQ(host.frees.size(), 0);
  CHECK_EQ(vm.var(V(0)), 0);
  host.in_use = false;
  vm.tick();
  CHECK_EQ(host.frees.size(), 1);
  CHECK_EQ(host.frees.front(), 1);
  CHECK_EQ(vm.var(V(0)), 1);
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
  // RAND_MOD n scales the 15-bit draw: draw * (n + 1) >> 15.
  CHECK_EQ(vm.var(V(2)), 100 * 8 >> 15);
}

// RAND keeps the low 15 bits of a draw; RAND_MOD n = draw * (n + 1) >> 15, so n is the inclusive maximum and the top draw gives n.
void test_rand_range() {
  Asm script;
  const u16 init = script.here();
  script.raw({0x1E}).word(V(0));
  script.raw({0x1F}).word(V(1)).word(I(7));
  script.raw({0x1F}).word(V(2)).word(I(7));
  script.raw({0x1F}).word(V(3)).word(I(100));
  script.raw({0x1F}).word(V(4)).word(I(0));
  script.stop();
  NullHost host;
  const std::vector<u32> draws{0xFFFFFFFFu, 0x4000u, 0x7FFFu, 0x3FFFu, 0x7FFFu};
  size_t next = 0;
  FieldVM vm(host, [&] { return draws[next++ % draws.size()]; });
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(0)), 0x7FFF);
  CHECK_EQ(vm.var(V(1)), 4);
  CHECK_EQ(vm.var(V(2)), 7);
  CHECK_EQ(vm.var(V(3)), 0x3FFF * 101 >> 15);
  CHECK_EQ(vm.var(V(4)), 0);
}

// The default generator is the game's rand() step: state * 0x41C64E6D + 0x3039, bits 16..30. Every 32-bit state that gives the
// first draw is stepped through the next three; one must reproduce all of them.
void test_default_rand() {
  Asm script;
  const u16 init = script.here();
  for (int k = 0; k < 4; ++k) script.raw({0x1E}).word(V(k));
  script.stop();
  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  for (int k = 0; k < 4; ++k) CHECK(vm.var(V(k)) >= 0 && vm.var(V(k)) <= 0x7FFF);
  const u32 first = static_cast<u32>(vm.var(V(0)));
  bool chained = false;
  for (u32 low = 0; low < 0x20000 && !chained; ++low) {
    u32 state = (low >> 16 << 31) | (first << 16) | (low & 0xFFFF);
    chained = true;
    for (int k = 1; k < 4 && chained; ++k) {
      state = state * 0x41C64E6Du + 0x3039u;
      chained = static_cast<s32>((state >> 16) & 0x7FFF) == vm.var(V(k));
    }
  }
  CHECK(chained);
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
    if (is_extension_op(op.key)) script.raw({std::atoi(op.lengths) - 3});
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

// Extension range: the size byte frames every instruction, so ops a build does not know and payload bytes after an op's own
// operands are stepped over; a payload too short for its operands is skipped without running.
void test_extension_ops() {
  Asm script;
  const u16 init = script.here();
  script.set(V(2), 5).set(V(3), 9);
  const u16 known = script.here();
  script.raw({0xFE, 0xC0, 4}).word(V(0)).word(I(0));
  const u16 unknown = script.here();
  script.raw({0xFE, 0xFE, 3, 0x11, 0x22, 0x33});
  const u16 longer = script.here();
  script.raw({0xFE, 0xC0, 6}).word(V(1)).word(I(0)).word(0xBEEF);
  const u16 short_payload = script.here();
  script.raw({0xFE, 0xC0, 2}).word(V(2));
  script.raw({0xFE, 0xC0, 4}).word(V(3)).word(I(0x7FFF));
  script.set(V(4), 1);
  script.stop();

  const auto get = decode_insn(script.code, known);
  CHECK(get.has_value());
  if (get) {
    CHECK_EQ(get->length, 7);
    CHECK(std::string(get->info->name) == "GET_FEATURE_LEVEL");
    CHECK_EQ(get->ops.size(), 2);
    if (get->ops.size() == 2) {
      CHECK(get->ops[0].kind == OperandKind::Var);
      CHECK(get->ops[1].kind == OperandKind::Imm);
    }
  }
  const auto other = decode_insn(script.code, unknown);
  CHECK(other.has_value());
  if (other) {
    CHECK_EQ(other->length, 6);
    CHECK_EQ(other->info->conf, '?');
    CHECK(other->ops.empty());
  }
  const auto extra = decode_insn(script.code, longer);
  CHECK(extra.has_value());
  if (extra) {
    CHECK_EQ(extra->length, 9);
    CHECK_EQ(extra->ops.size(), 2);
  }
  std::string error;
  CHECK(!decode_insn(script.code, short_payload, &error).has_value());
  CHECK(!error.empty());
  const Bytes truncated{0xFE, 0xC0, 9, 0x00, 0x04};
  CHECK(!decode_insn(truncated, 0).has_value());

  NullHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(vm.var(V(1)), 1);
  CHECK_EQ(vm.var(V(2)), 5);
  CHECK_EQ(vm.var(V(3)), 0);
  CHECK_EQ(vm.var(V(4)), 1);
  CHECK_EQ(vm.stats().unknown_ops.size(), 2);
}

// FE D0 MODEL_SOURCE hands the host (room model, kind, id, sub), each operand an immediate or a variable; feature 5 is level 1.
class ModelSourceHost : public NullHost {
 public:
  std::vector<std::array<int, 4>> calls;
  void set_model_source(int model, int kind, int id, int sub) override { calls.push_back({model, kind, id, sub}); }
};

void test_model_source_op() {
  Asm script;
  const u16 init = script.here();
  script.set(V(1), 0x13);
  script.set(V(3), 4);
  const u16 op_at = script.here();
  // The character in var 1, room 0x10's model (var 3), battle model 70, then no source: the room's own model again.
  script.raw({0xFE, 0xD0, 8}).word(I(5)).word(I(0)).word(V(1)).word(I(0));
  script.raw({0xFE, 0xD0, 8}).word(I(6)).word(I(2)).word(I(0x10)).word(V(3));
  script.raw({0xFE, 0xD0, 8}).word(I(7)).word(I(3)).word(I(70)).word(I(0));
  script.raw({0xFE, 0xD0, 8}).word(I(5)).word(I(0xFF)).word(I(0)).word(I(0));
  script.raw({0xFE, 0xC0, 4}).word(V(2)).word(I(5));
  script.stop();
  const auto op = decode_insn(script.code, op_at);
  CHECK(op.has_value());
  if (op) {
    CHECK_EQ(op->length, 11);
    CHECK(std::string(op->info->name) == "MODEL_SOURCE");
  }
  ModelSourceHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(host.calls.size(), 4);
  if (host.calls.size() == 4) {
    CHECK((host.calls[0] == std::array<int, 4>{5, 0, 0x13, 0}));
    CHECK((host.calls[1] == std::array<int, 4>{6, 2, 0x10, 4}));
    CHECK((host.calls[2] == std::array<int, 4>{7, 3, 70, 0}));
    CHECK((host.calls[3] == std::array<int, 4>{5, 0xFF, 0, 0}));
  }
  CHECK_EQ(vm.var(V(2)), 1);
  CHECK(vm.stats().unknown_ops.empty());
}


// The mod UI ops (FE C1-FE CB, feature 2): the elements a room's scripts build, GETPOS, SET, the item step and the blocking pick.
class KeyHost : public NullHost {
 public:
  u16 latch = 0, held = 0;
  u16 key_latch() override { return latch; }
  void clear_key_latch() override { latch = 0; }
  u16 keys_held() override { return held; }
};

// Party swap's reserve ops: FE CC writes a slot, FE CD reads one (0xFF = empty).
class ReserveHost : public NullHost {
 public:
  std::vector<int> reserve;
  int reserve_member(int slot) override { return slot < static_cast<int>(reserve.size()) ? reserve[static_cast<size_t>(slot)] : -1; }
  void set_reserve_member(int slot, int char_id) override {
    if (static_cast<int>(reserve.size()) <= slot) reserve.resize(static_cast<size_t>(slot) + 1, -1);
    reserve[static_cast<size_t>(slot)] = char_id >= 0x2C ? -1 : char_id;
  }
};

void test_reserve_ops() {
  Asm script;
  const u16 init = script.here();
  script.raw({0xFE, 0xCC, 4}).word(I(2)).word(I(7));
  script.raw({0xFE, 0xCD, 4}).word(V(0)).word(I(2));
  script.raw({0xFE, 0xCD, 4}).word(V(1)).word(I(0));
  script.stop();
  ReserveHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK(host.reserve == (std::vector<int>{-1, -1, 7}));
  CHECK_EQ(vm.var(V(0)), 7);
  CHECK_EQ(vm.var(V(1)), 0xFF);
}

void test_mod_ui_ops() {
  // UI_CLEAR keeps the change counter, so an element placed after it never repeats a revision a window shows.
  {
    ModUi cleared;
    cleared.serial = 5;
    cleared.clear();
    CHECK_EQ(cleared.serial, 5u);
  }
  Asm script;
  const auto ext = [&](int sub, std::initializer_list<int> words) {
    script.raw({0xFE, sub, static_cast<int>(2 * words.size())});
    for (int value : words) script.word(value);
  };
  const u16 init = script.here();
  ext(0xC1, {});
  // A panel, a framed text window, a line; items 10 (start), 11 to its right, 12 above, 13 further right but disabled.
  ext(0xC3, {I(0), I(10), I(20), I(300), I(200), I(10), I(2)});
  ext(0xC5, {I(1), I(7), I(40), I(30), I(1)});
  ext(0xC4, {I(2), I(50), I(100), I(100), I(100), I(11)});
  ext(0xC6, {I(10), I(45), I(95), I(10), I(10), I(0)});
  ext(0xC6, {I(11), I(95), I(95), I(10), I(10), I(0)});
  ext(0xC6, {I(12), I(45), I(35), I(10), I(10), I(0)});
  ext(0xC6, {I(13), I(145), I(95), I(10), I(10), I(1)});
  ext(0xC7, {I(10), I(0)});
  ext(0xCB, {I(20), I(1), I(2), I(3)});
  ext(0xC2, {I(1)});
  ext(0xC9, {I(11), V(5), V(6)});
  // SET: the panel's colour; the text's message (its revision moves on).
  ext(0xCA, {I(0), I(2), I(20)});
  ext(0xCA, {I(1), I(5), I(8)});
  script.stop();
  const u16 update = script.here();
  ext(0xC8, {V(0)});
  script.inc(V(1));
  script.park();

  const std::initializer_list<std::pair<int, s32>> presses = {{0x2000, 0x4B}, {0x1000, 0x4C}, {0x20, 10}, {0x40, 0x80}, {0x10, 0x81}, {0x80, 0x82},
                                                              {0x04, 0x83}, {0x08, 0x84}};
  for (const auto& [press, want] : presses) {
    KeyHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{{{0, init}, {1, update}}}}));
    const ModUi& ui = vm.mod_ui();
    CHECK(ui.shown && ui.cursor == 10 && ui.cursor_style == ModUi::kCursorOutline);
    CHECK(ui.elements[0].kind == ModUi::kRect && ui.elements[0].flags == ModUi::kTranslucent && ui.elements[0].colour == 20);
    CHECK(ui.elements[1].kind == ModUi::kText && ui.elements[1].msg == 8 && ui.elements[1].flags == ModUi::kFramed && ui.elements[1].revision == 9);
    CHECK(ui.elements[2].kind == ModUi::kLine && ui.elements[2].w == 100);
    CHECK((ui.palette[20] == std::array<u8, 3>{1, 2, 3}));
    CHECK_EQ(vm.var(V(5)), 100);
    CHECK_EQ(vm.var(V(6)), 100);
    CHECK(ui.selectable(11) && !ui.selectable(13) && !ui.selectable(0));
    CHECK_EQ(ui.step(1), 11);
    CHECK_EQ(ui.step(0), 12);
    CHECK_EQ(ui.step(3), -1);
    // A press made before the pick started is not a pick (its first tick clears the latch); no press: it waits.
    host.latch = 0x20;
    vm.tick();
    CHECK_EQ(host.latch, 0);
    vm.tick();
    CHECK_EQ(vm.var(V(1)), 0);
    host.latch = static_cast<u16>(press);
    vm.tick();
    CHECK_EQ(vm.var(V(0)), want);
    CHECK_EQ(host.latch, 0);
    // The pick ended the turn: the next instruction runs next tick.
    CHECK_EQ(vm.var(V(1)), 0);
    vm.tick();
    CHECK_EQ(vm.var(V(1)), 1);
    if (want & ModUi::kPickMoved && want < ModUi::kPickCancel) CHECK_EQ(vm.mod_ui().cursor, want & 0x3F);
    CHECK(vm.mod_ui().holds_controls);
  }
  {
    // Held keys: the Circle that opened the screen is no pick; a held Right moves once, then again only after the repeat delay and
    // then every kRepeatEvery ticks (V(1) counts the moves of a pick loop); hiding the UI gives the controls back.
    Asm looped;
    const auto lext = [&](int sub, std::initializer_list<int> words) {
      looped.raw({0xFE, sub, static_cast<int>(2 * words.size())});
      for (int value : words) looped.word(value);
    };
    const u16 init2 = looped.here();
    for (int id = 0; id < 4; ++id) lext(0xC6, {I(id), I(40 * id), I(0), I(10), I(10), I(0)});
    lext(0xC7, {I(0), I(0)});
    lext(0xC2, {I(1)});
    looped.stop();
    const u16 loop = looped.here();
    lext(0xC8, {V(0)});
    // IF_JUMP jumps when the condition is false: on 0x80 (cancel) past the INC and the JUMP (8 + 3 + 3 bytes) to UI_SHOW 0.
    looped.if_jump(V(0), 1, 0x80, static_cast<u16>(looped.here() + 14));
    looped.inc(V(1));
    looped.jump(loop);
    lext(0xC2, {I(0)});
    looped.park();
    KeyHost host;
    FieldVM vm(host);
    vm.load(make_module(looped, {{{{0, init2}, {1, loop}}}}));
    host.held = host.latch = 0x20;
    for (int tick = 0; tick < 4; ++tick) vm.tick();
    CHECK_EQ(vm.var(V(1)), 0);
    host.held = 0;
    vm.tick();
    host.held = host.latch = 0x2000;
    vm.tick();
    vm.tick();
    CHECK_EQ(vm.var(V(1)), 1);
    for (int tick = 0; tick < ModUi::kRepeatDelay - 4; ++tick) vm.tick();
    CHECK_EQ(vm.var(V(1)), 1);
    for (int tick = 0; tick < 12; ++tick) vm.tick();
    CHECK(vm.var(V(1)) >= 2 && vm.var(V(1)) <= 3);
    CHECK(vm.mod_ui().holds_controls);
    host.held = 0;
    vm.tick();
    host.latch = 0x40;
    for (int tick = 0; tick < 3; ++tick) vm.tick();
    CHECK(!vm.mod_ui().shown && !vm.mod_ui().holds_controls);
  }
  {
    // UI_PICK's repeat operands: delay 30, no repeat - a held Right moves once however long it is held.
    Asm looped;
    const auto lext = [&](int sub, std::initializer_list<int> words) {
      looped.raw({0xFE, sub, static_cast<int>(2 * words.size())});
      for (int value : words) looped.word(value);
    };
    const u16 init2 = looped.here();
    for (int id = 0; id < 4; ++id) lext(0xC6, {I(id), I(40 * id), I(0), I(10), I(10), I(0)});
    lext(0xC7, {I(0), I(0)});
    lext(0xC2, {I(1)});
    looped.stop();
    const u16 loop = looped.here();
    lext(0xC8, {V(0), I(30), I(0)});
    looped.inc(V(1));
    looped.jump(loop);
    KeyHost host;
    FieldVM vm(host);
    vm.load(make_module(looped, {{{{0, init2}, {1, loop}}}}));
    for (int tick = 0; tick < 4; ++tick) vm.tick();
    host.held = host.latch = 0x2000;
    for (int tick = 0; tick < 3; ++tick) vm.tick();
    CHECK_EQ(vm.var(V(1)), 1);
    for (int tick = 0; tick < 90; ++tick) vm.tick();
    CHECK_EQ(vm.var(V(1)), 1);
    CHECK_EQ(vm.mod_ui().cursor, 1);
  }
  // SET enabled / visible change what the cursor can reach; a new room's VM starts with nothing shown.
  ModUi ui;
  ui.elements[1] = {ModUi::kItem, 0, 0, 10, 10, 0, 0};
  ui.elements[2] = {ModUi::kItem, 40, 0, 10, 10, 0, ModUi::kDisabled};
  ui.cursor = 1;
  CHECK_EQ(ui.step(1), -1);
  CHECK(ui.set(2, ModUi::kFieldEnabled, 1));
  CHECK_EQ(ui.step(1), 2);
  CHECK(ui.set(2, ModUi::kFieldVisible, 0));
  CHECK_EQ(ui.step(1), -1);
  CHECK(!ui.set(2, 99, 0));
  // Level 2: an image's angle wraps at a full turn; alpha is kept 0..255.
  CHECK(ui.set(2, ModUi::kFieldAngle, 4096 + 1024));
  CHECK_EQ(ui.elements[2].angle, 1024);
  CHECK(ui.set(2, ModUi::kFieldAlpha, 300));
  CHECK_EQ(ui.elements[2].alpha, 255);
  // Level 2: an item's own neighbours: one that way, its own id (no move), any other value = the nearest item that way.
  CHECK(ui.set(2, ModUi::kFieldVisible, 1));
  ui.elements[3] = {ModUi::kItem, 0, 40, 10, 10, 0, 0};
  CHECK_EQ(ui.step(2), 3);
  CHECK(ui.set(1, ModUi::kFieldDown, 2));
  CHECK_EQ(ui.step(2), 2);
  CHECK(ui.set(1, ModUi::kFieldDown, 1));
  CHECK_EQ(ui.step(2), -1);
  CHECK(ui.set(1, ModUi::kFieldDown, 255));
  CHECK_EQ(ui.step(2), 3);
  CHECK(ui.set(1, ModUi::kFieldRight, 3));
  CHECK(ui.set(3, ModUi::kFieldVisible, 0));
  CHECK_EQ(ui.step(1), 2);
  KeyHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  vm.load(make_module(script, {{{{0, update}}}}));
  CHECK(!vm.mod_ui().shown && vm.mod_ui().elements[0].kind == ModUi::kNone);
  {
    // Ovals (level 2): ids up to 255; a band's width (0 reads as 1); UI_SET width; GETPOS gives the centre. Items past 63 are not
    // selectable (a pick result has 6 bits for the item). An image (level 2): its rect, tint, flags and number; UI_SET 5 renumbers it.
    Asm ovals;
    const auto oext = [&](int sub, std::initializer_list<int> words) {
      ovals.raw({0xFE, sub, static_cast<int>(2 * words.size())});
      for (int value : words) ovals.word(value);
    };
    const u16 start = ovals.here();
    oext(0xCE, {I(100), I(282), I(124), I(26), I(26), I(6), I(0), I(0)});
    oext(0xCE, {I(101), I(250), I(124), I(58), I(44), I(5), I(ModUi::kOutline | ModUi::kTranslucent), I(4)});
    oext(0xCA, {I(101), I(ModUi::kFieldWidth), I(0)});
    oext(0xC9, {I(101), V(0), V(1)});
    oext(0xC6, {I(70), I(0), I(0), I(10), I(10), I(0)});
    oext(0xC0, {V(2), I(2)});
    oext(0xCF, {I(102), I(10), I(20), I(24), I(24), I(8), I(ModUi::kTranslucent), I(7)});
    oext(0xCA, {I(102), I(ModUi::kFieldMsg), I(12)});
    oext(0xCE, {I(255), I(1), I(2), I(3), I(4), I(5), I(0), I(1)});
    ovals.stop();
    FieldVM ovm(host);
    ovm.load(make_module(ovals, {{{{0, start}}}}));
    const ModUi& oui = ovm.mod_ui();
    CHECK(oui.elements[100].kind == ModUi::kOval && oui.elements[100].w == 26 && oui.elements[100].width == 1 && oui.elements[100].colour == 6);
    CHECK(oui.elements[101].kind == ModUi::kOval && oui.elements[101].h == 44 && oui.elements[101].flags == (ModUi::kOutline | ModUi::kTranslucent));
    CHECK_EQ(oui.elements[101].width, 1);
    CHECK_EQ(ovm.var(V(0)), 250);
    CHECK_EQ(ovm.var(V(1)), 124);
    CHECK(oui.elements[70].kind == ModUi::kItem && !oui.selectable(70));
    CHECK_EQ(ovm.var(V(2)), 2);
    const ModUi::Element& image = oui.elements[102];
    CHECK(image.kind == ModUi::kImage && image.x == 10 && image.w == 24 && image.colour == 8 && image.flags == ModUi::kTranslucent);
    CHECK_EQ(image.msg, 12);
    CHECK_EQ(oui.centre(102).first, 22);
    CHECK(oui.elements[255].kind == ModUi::kOval && oui.elements[255].x == 1);
  }
}

// The entrance table (0xFF, then 8-byte records up to the first script) keeps its records when the first script does not start on a
// record boundary: two records, three spare bytes, then the code.
void test_entrance_table_unaligned() {
  Bytes blob(0x84 + 0x40, 0);
  blob[0x80] = 1;
  const u16 first = 1 + 2 * 8 + 3;
  blob[0x84] = first & 0xFF;
  blob[0x85] = first >> 8;
  const s16 records[2][4] = {{1125, 2754, 1024, 1}, {-50, 300, 0, 0}};
  blob.push_back(0xFF);
  for (const auto& record : records)
    for (s16 word : record) {
      blob.push_back(static_cast<u8>(word & 0xFF));
      blob.push_back(static_cast<u8>((word >> 8) & 0xFF));
    }
  blob.insert(blob.end(), {0x00, 0x00, 0x00, 0x00});
  const auto module = FieldScriptModule::parse(blob);
  CHECK(module.has_value());
  if (!module) return;
  CHECK_EQ(module->positions.size(), 2);
  if (module->positions.size() != 2) return;
  CHECK_EQ(module->positions[0].a, 1125);
  CHECK_EQ(module->positions[0].b, 2754);
  CHECK_EQ(module->positions[0].c, 1024);
  CHECK_EQ(module->positions[0].level, 1);
  CHECK_EQ(module->positions[1].a, -50);
  CHECK_EQ(module->positions[1].b, 300);
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

// FE 2F clears the variables but keeps DEBUG_MODE (variable 0x34): the host's reset already sees the kept value.
void test_fe2f_keeps_debug_mode() {
  struct ResetHost : NullHost {
    const FieldVM* vm = nullptr;
    s32 seen = -1;
    void field_reset() override { seen = vm->var(0x34); }
  };
  Asm script;
  const u16 init = script.here();
  script.raw({0xFE, 0x2F}).stop();
  ResetHost host;
  FieldVM vm(host);
  host.vm = &vm;
  vm.set_var(0x34, 1);
  vm.set_var(0x40, 7);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(host.seen, 1);
  CHECK_EQ(vm.var(0x34), 1);
  CHECK_EQ(vm.var(0x40), 0);
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

// 3C (model entity), A9 (target resolves) and FE 78 (song started) each add 1 to the tick budget: behind a WAIT_FRAME, the op and
// 8 INCs run in one tick; 3C on an entity without a 3D model (every entity at load, until 0E / 0F / D1 give it one) adds nothing.
// While the room-entry window is open 3C / A9 wait.
void test_budget_bonus_and_entry_window() {
  struct WindowHost : TestHost {
    int window_ticks = 0;
    bool entry_window() override { return window_ticks > 0; }
  };
  const std::vector<std::vector<int>> bonus_ops = {{0x3C, 0x04, 0x80}, {0xA9, 0x01, 0x04, 0x80}, {0xFE, 0x78, 0x01, 0x80, 0x7F, 0x80}};
  for (size_t variant = 0; variant < bonus_ops.size() + 1; ++variant) {
    const bool no_model = variant == bonus_ops.size();
    Asm script;
    const u16 upd = script.here();
    script.wait_frame();
    for (int byte : bonus_ops[no_model ? 0 : variant]) script.raw({byte});
    for (int i = 0; i < 8; ++i) script.inc(V(0));
    script.park();
    TestHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{}, {{{1, upd}}}}));
    CHECK(vm.entity(1).flags38 & flag38::kNo3dObject);
    if (!no_model) vm.entity(1).flags38 &= ~flag38::kNo3dObject;
    vm.tick();
    vm.tick();
    CHECK_EQ(vm.var(V(0)), no_model ? 7 : 8);
  }
  for (const int op : {0x3C, 0xA9}) {
    Asm script;
    const u16 upd = script.here();
    script.inc(V(0));
    if (op == 0x3C)
      script.raw({0x3C, 0x04, 0x80});
    else
      script.raw({0xA9, 0x01, 0x04, 0x80});
    script.inc(V(1)).park();
    WindowHost host;
    host.window_ticks = 3;
    FieldVM vm(host);
    vm.load(make_module(script, {{}, {{{1, upd}}}}));
    for (int tick = 0; tick < 3; ++tick, --host.window_ticks) {
      vm.tick();
      CHECK_EQ(vm.var(V(0)), 1);
      CHECK_EQ(vm.var(V(1)), 0);
    }
    vm.tick();
    CHECK_EQ(vm.var(V(1)), 1);
  }
}

// Two entities reach A3 on the same tick: the first one's request holds the system gate, so the second waits, requests its own
// battle once the first has ended, and goes on only after its own fight; the first goes on (its FE 10) before the second fight.
void test_second_battle_waits() {
  struct BattleHost : TestHost {
    std::vector<int> requests;
    bool pending = false;
    bool system_ready() override { return !pending; }
    void request_battle(int battle_id, int, int) override {
      requests.push_back(battle_id);
      pending = true;
    }
    bool battle_active() override { return pending; }
  };
  Asm script;
  const u16 first = script.here();
  script.raw({0xA3}).word(I(5)).word(I(0)).word(I(0)).raw({0xFE, 0x10});
  script.set(V(0), 1).park();
  const u16 second = script.here();
  script.raw({0xA3}).word(I(6)).word(I(0)).word(I(0)).raw({0xFE, 0x10});
  script.set(V(1), 1).park();
  BattleHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{}, {{{1, first}}}, {{{1, second}}}}));
  for (int tick = 0; tick < 10; ++tick) vm.tick();
  CHECK_EQ(host.requests.size(), 1);
  CHECK_EQ(host.requests[0], 5);
  CHECK_EQ(vm.var(V(0)), 0);
  CHECK_EQ(vm.var(V(1)), 0);
  host.pending = false;
  for (int tick = 0; tick < 10; ++tick) vm.tick();
  CHECK_EQ(host.requests.size(), 2);
  CHECK_EQ(host.requests[1], 6);
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(vm.var(V(1)), 0);
  host.pending = false;
  for (int tick = 0; tick < 10; ++tick) vm.tick();
  CHECK_EQ(host.requests.size(), 2);
  CHECK_EQ(vm.var(V(1)), 1);
}

// The launcher ops (here FE 72) also wait while a message window is open; FE 5B waits for the system gate alone.
void test_launcher_gate_waits_for_windows() {
  struct WindowMenuHost : TestHost {
    int requests = 0;
    bool window = true;
    bool window_open() override { return window; }
    void request_menu(MenuKind, int) override { ++requests; }
  };
  for (const int screen_op : {0x72, 0x5B}) {
    Asm script;
    const u16 upd = script.here();
    script.raw({0xFE, screen_op});
    script.set(V(0), 1).park();
    WindowMenuHost host;
    FieldVM vm(host);
    vm.load(make_module(script, {{}, {{{1, upd}}}}));
    for (int tick = 0; tick < 5; ++tick) vm.tick();
    CHECK_EQ(host.requests, screen_op == 0x5B ? 1 : 0);
    host.window = false;
    for (int tick = 0; tick < 5; ++tick) vm.tick();
    CHECK_EQ(host.requests, 1);
    CHECK_EQ(vm.var(V(0)), 1);
  }
}

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

// FE 3B writes the status byte of the referenced character; a reference that resolves to nobody (an empty party slot) leaves the
// variable as it was.
void test_status_read_skips_nobody() {
  class StatusHost : public NullHost {
   public:
    int char_status(int char_id) override { return char_id == 0xFD ? 0x109 : -1; }
  };
  Asm script;
  const u16 init = script.here();
  script.set(V(0), 0x1234).set(V(1), 0x1234);
  script.raw({0xFE, 0x3B}).word(V(0)).word(I(0xFF));
  script.raw({0xFE, 0x3B}).word(V(1)).word(I(0xFD));
  script.stop();
  StatusHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, init}}}}));
  CHECK_EQ(vm.var(V(0)), 0x1234);
  CHECK_EQ(vm.var(V(1)), 9);
}

// A screen holds the system gate from its request until it has closed: a second entity's screen op on the same tick waits in place
// and opens its own screen once the first one has closed.
void test_screen_ops_queue_behind_a_pending_screen() {
  struct QueueHost : TestHost {
    std::vector<MenuKind> requests;
    bool open = false;
    bool system_ready() override { return !open; }
    void request_menu(MenuKind kind, int) override {
      requests.push_back(kind);
      open = true;
    }
    bool menu_active() override { return open; }
  };
  Asm script;
  const u16 party = script.here();
  script.raw({0xFE, 0x5B, 0xFE, 0x61});
  script.set(V(0), 1).park();
  const u16 name = script.here();
  script.raw({0xFE, 0x4A}).word(I(1)).raw({0xFE, 0x61});
  script.set(V(1), 1).park();
  QueueHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{}, {{{1, party}}}, {{{1, name}}}}));
  for (int tick = 0; tick < 5; ++tick) vm.tick();
  CHECK_EQ(host.requests.size(), 1);
  CHECK(host.requests[0] == MenuKind::PartyScreen);
  CHECK_EQ(vm.var(V(0)), 0);
  host.open = false;
  vm.tick();
  CHECK_EQ(host.requests.size(), 2);
  CHECK(host.requests[1] == MenuKind::NameEntry);
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(vm.var(V(1)), 0);
  host.open = false;
  for (int tick = 0; tick < 5; ++tick) vm.tick();
  CHECK_EQ(host.requests.size(), 2);
  CHECK_EQ(vm.var(V(1)), 1);
}

// 07 / 08 / 09 wait in place while the system gate holds: nothing is spawned, and a 09 whose child has ended goes on only once the
// gate opens again.
void test_run_script_waits_for_gate() {
  struct GateHost : NullHost {
    bool busy = false;
    bool system_ready() override { return !busy; }
    bool busy_clear() override { return !busy; }
  };
  Asm script;
  const u16 parent = script.here();
  script.raw({0x07, 0x02, 0x05});
  script.set(V(0), 1);
  script.raw({0x09, 0x02, 0x06});
  script.set(V(1), 1).park();
  const u16 async_child = script.here();
  script.set(V(2), 1).stop();
  const u16 blocking_child = script.here();
  script.wait(3).stop();
  GateHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{}, {{{1, parent}}}, {{{5, async_child}, {6, blocking_child}}}}));
  host.busy = true;
  for (int tick = 0; tick < 5; ++tick) vm.tick();
  CHECK_EQ(vm.var(V(0)), 0);
  CHECK_EQ(vm.var(V(2)), 0);
  host.busy = false;
  vm.tick();
  CHECK_EQ(vm.var(V(0)), 1);
  host.busy = true;
  for (int tick = 0; tick < 10; ++tick) vm.tick();
  CHECK_EQ(vm.var(V(2)), 1);
  CHECK_EQ(vm.var(V(1)), 0);
  host.busy = false;
  for (int tick = 0; tick < 3; ++tick) vm.tick();
  CHECK_EQ(vm.var(V(1)), 1);
}

// A song change holds the full system gate (here a second PLAY_SONG waits) but not the gate of 07 / 08 / 09: the child is spawned
// while the song changes.
void test_song_change_holds_gate_but_not_run_script() {
  struct SongHost : NullHost {
    bool song_change = true;
    bool system_ready() override { return !song_change; }
  };
  Asm script;
  const u16 parent = script.here();
  script.raw({0x07, 0x02, 0x05});
  script.set(V(0), 1);
  script.raw({0x88}).word(I(5));
  script.set(V(1), 1).park();
  const u16 child = script.here();
  script.set(V(2), 1).stop();
  SongHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{}, {{{1, parent}}}, {{{5, child}}}}));
  for (int tick = 0; tick < 5; ++tick) vm.tick();
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(vm.var(V(2)), 1);
  CHECK_EQ(vm.var(V(1)), 0);
  host.song_change = false;
  vm.tick();
  CHECK_EQ(vm.var(V(1)), 1);
}

// 7B and 7D wait in place while a party model swap (D9 .. DA) is pending.
void test_party_ops_wait_for_model_swap() {
  struct SwapHost : PartyHost {
    bool swap = true;
    int removed = 0;
    bool model_swap_pending() override { return swap; }
    void party_remove(int) override { ++removed; }
  };
  Asm script;
  const u16 add = script.here();
  script.raw({0x7B}).word(I(1)).raw({0x7C});
  script.set(V(0), 1).park();
  const u16 remove = script.here();
  script.raw({0x7D, 0xFE});
  script.set(V(1), 1).park();
  SwapHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{}, {{{1, add}}}, {{{1, remove}}}}));
  for (int tick = 0; tick < 5; ++tick) vm.tick();
  CHECK_EQ(host.party.size(), 1);
  CHECK_EQ(host.removed, 0);
  CHECK_EQ(vm.var(V(0)), 0);
  CHECK_EQ(vm.var(V(1)), 0);
  host.swap = false;
  vm.tick();
  CHECK_EQ(host.party.size(), 2);
  CHECK_EQ(host.removed, 1);
  CHECK_EQ(vm.var(V(0)), 1);
  CHECK_EQ(vm.var(V(1)), 1);
}

// MOVE_TWEEN (5B) frees the entity's height; JUMP_MOVE (5C) starts the same hop but leaves the free-height flag alone, so the
// actor stays grounded for the wander step, the push and the tile flags after it lands.
void test_hop_free_height_flag() {
  constexpr u32 kFreeHeight = 0x40;
  Asm script;
  const u16 tween = script.here();
  script.raw({0x5B, 0x00}).word(100).word(200).word(-30).word(10).raw({0xF0});
  script.stop();
  const u16 jump = script.here();
  script.raw({0x5C, 0x00}).word(100).word(200).word(10).raw({0xE0}).word(I(0));
  script.stop();
  TestHost host;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{0, tween}}}, {{{0, jump}}}}));
  CHECK_EQ(host.motions_started, 2);
  CHECK((vm.entity(0).flags38 & kFreeHeight) != 0);
  CHECK((vm.entity(1).flags38 & kFreeHeight) == 0);
}

// WALK_TO (3F) sets the free-height flag on its wait form too, not only when it starts: the field clears the flag of the party
// entities before every script pass, so a party member walking a 3F must get it back on every tick the wait form runs.
void test_walk_to_wait_form_keeps_free_height() {
  constexpr u32 kFreeHeight = 0x40;
  Asm script;
  const u16 walk = script.here();
  script.raw({0x3F, 0x00}).word(10).word(20).word(-5).raw({0xE0}).raw({0x3F, 0x01});
  script.stop();
  TestHost host;
  host.motion_polls_busy = 3;
  FieldVM vm(host);
  vm.load(make_module(script, {{{{1, walk}}}}));
  vm.tick();
  CHECK_EQ(host.motions_started, 1);
  CHECK((vm.entity(0).flags38 & kFreeHeight) != 0);
  vm.entity(0).flags38 &= ~kFreeHeight;
  vm.tick();
  CHECK_EQ(host.motions_started, 1);
  CHECK((vm.entity(0).flags38 & kFreeHeight) != 0);
}

int main() {
  test_status_read_skips_nobody();
  test_arithmetic();
  test_movie_end_jump();
  test_movie_operands();
  test_debug_calls_and_vibration();
  test_skipped_battle();
  test_budget_bonus_and_entry_window();
  test_second_battle_waits();
  test_launcher_gate_waits_for_windows();
  test_screen_ops_and_fe61();
  test_screen_ops_queue_behind_a_pending_screen();
  test_run_script_waits_for_gate();
  test_song_change_holds_gate_but_not_run_script();
  test_party_ops_wait_for_model_swap();
  test_hop_free_height_flag();
  test_walk_to_wait_form_keeps_free_height();
  test_variables_and_signedness();
  test_bit_ops();
  test_if_jump_polarity();
  test_if_jump_mixed_signedness();
  test_call_return();
  test_budget_and_default_restart();
  test_wait_scheduling();
  test_music_cues();
  test_priority_preemption();
  test_blocking_run_script();
  test_run_script_and_wait();
  test_party_slot_and_choice_result();
  test_kill_all_scripts();
  test_e2_culled_speaker();
  test_dialogue_none_speaker();
  test_commit_relinks_new_member();
  test_load_animation_resolves_entity();
  test_free_animation_waits_for_pack();
  test_scenario_and_rng();
  test_rand_range();
  test_default_rand();
  test_opcode_table();
  test_extension_ops();
  test_mod_ui_ops();
  test_reserve_ops();
  test_model_source_op();
  test_entrance_table_unaligned();
  test_get_position_order();
  test_ca_faces_the_next_walk();
  test_run_script_inactive_target();
  test_fe50_restore();
  test_fe2f_keeps_debug_mode();
  test_stuck_watchdog();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("script tests ok\n");
  return 0;
}
