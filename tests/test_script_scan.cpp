// Static room-script scan tests: hand-assembled rooms (a chest, a guarded battle, a pruned branch, a flag set by another entity's
// script) run through script::scan_script, plus the helpers that classify gives and read story ranges and flags.
#include <cstdio>
#include <initializer_list>
#include <map>
#include <vector>

#include "script/field_script.h"
#include "script/script_scan.h"

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

// Room-local variable k and a U immediate.
constexpr u16 V(int k) { return static_cast<u16>(0x400 + 2 * k); }
constexpr u16 I(int value) { return static_cast<u16>(0x8000 | value); }

// Byte assembler; offset 0 holds the pre-script table byte, so no script starts at 0.
struct Asm {
  Bytes code{0xFF};
  u16 here() const { return static_cast<u16>(code.size()); }
  Asm& raw(std::initializer_list<int> values) {
    for (int value : values) code.push_back(static_cast<u8>(value));
    return *this;
  }
  Asm& word(int value) { return raw({value & 0xFF, (value >> 8) & 0xFF}); }
  // Patches the jump word at `at` (an offset returned by a jump emitter) to `target`.
  void patch(u16 at, u16 target) {
    code[at] = static_cast<u8>(target & 0xFF);
    code[at + 1] = static_cast<u8>(target >> 8);
  }
  Asm& stop() { return raw({0x00}); }
  Asm& ret() { return raw({0x0D}); }
  Asm& set(u16 var, int lit) { raw({0x10}); word(var); word(lit & 0xFFFF); return raw({0x40}); }
  Asm& set_bit(u16 var, int bit) { raw({0x15}); word(var); word(bit); return raw({0x40}); }
  Asm& set_var_bit(int bit_ref) { raw({0x71}); word(bit_ref); return raw({0x80}); }
  Asm& give(u16 operand) { raw({0xBB, 0x00}); return word(operand); }
  Asm& item_count(u16 dest, u16 item) { raw({0xD5}); word(dest); return word(item); }
  Asm& battle(int id) { raw({0xA3}); word(I(id)); word(I(0)); return word(I(0)); }
  Asm& battle_scene(int scene) { raw({0xC9}); word(I(scene)); word(I(0)); return word(I(0)); }
  Asm& hide_self() { return raw({0x36, 0x00, 0xFB}); }
  Asm& run_blocking(int entity, int slot) { return raw({0x09, entity, slot}); }
  // Conditional jumps: each returns the offset of its jump word for patch().
  u16 if_jump(u16 var, int cond, int lit) {
    raw({0x02});
    word(var);
    word(lit & 0xFFFF);
    raw({0x40 | cond});
    const u16 at = here();
    word(0);
    return at;
  }
  u16 if_literals(int left, int cond, int right) {
    raw({0x02});
    word(left & 0xFFFF);
    word(right & 0xFFFF);
    raw({0xC0 | cond});
    const u16 at = here();
    word(0);
    return at;
  }
  u16 jump_if_bit_clear(int bit_ref) {
    raw({0x73});
    word(bit_ref);
    raw({0x80});
    const u16 at = here();
    word(0);
    return at;
  }
  u16 jump_if_story_ge(int value) {
    raw({0x69});
    word(I(value));
    const u16 at = here();
    word(0);
    return at;
  }
  u16 jump_if_not_key(int mask) {
    raw({0x50});
    word(mask);
    const u16 at = here();
    word(0);
    return at;
  }
  u16 call() {
    raw({0x05});
    const u16 at = here();
    word(0);
    return at;
  }
};

FieldScriptModule make_module(const Asm& script, const std::vector<std::map<int, u16>>& entities) {
  FieldScriptModule module;
  module.entities.resize(entities.size());
  for (size_t entity = 0; entity < entities.size(); ++entity) {
    module.entities[entity].fill(0);
    for (const auto& [slot, ptr] : entities[entity]) module.entities[entity][static_cast<size_t>(slot)] = ptr;
  }
  module.code = script.code;
  return module;
}

const ScanEvent* find_event(const RoomScanResult& result, EventKind kind, int entity) {
  for (const ScanEvent& event : result.events)
    if (event.kind == kind && event.entity == entity) return &event;
  return nullptr;
}

bool has_cond(const ScanEvent& event, const Cond& cond) {
  for (const Cond& own : event.conds)
    if (own == cond) return true;
  return false;
}

// The chest routine: INIT stores the item in a room variable and hides the chest when its flag is set; ON_ACTION tests the stock
// against 99, gives the item through the variable and sets the flag.
void test_chest() {
  Asm a;
  const u16 init = a.here();
  a.set(V(3), 0x221);
  const u16 to_end = a.jump_if_bit_clear(0x1980);
  a.hide_self();
  a.patch(to_end, a.here());
  a.stop();
  const u16 action = a.here();
  a.item_count(V(4), V(3));
  const u16 to_give = a.if_jump(V(4), 0, 99);
  a.stop();
  a.patch(to_give, a.here());
  a.give(V(3)).set_var_bit(0x1980).stop();
  const RoomScanResult result = scan_script(make_module(a, {{{0, init}, {2, action}}}));
  const ScanEvent* give = find_event(result, EventKind::Give, 0);
  CHECK(give != nullptr);
  if (!give) return;
  CHECK_EQ(give->slot, 2);
  CHECK_EQ(give->values.size(), 1);
  if (!give->values.empty()) CHECK_EQ(give->values[0], 0x221);
  CHECK_EQ(give->operand_var, V(3));
  CHECK_EQ(give->flag.var, 0x198);
  CHECK_EQ(give->flag.bit, 0);
  CHECK(give->flag_at_load);
  CHECK(!give->flag_guarded);
  CHECK(give->capacity_check);
  CHECK(has_cond(*give, Cond{Subject::Var, V(4), Cmp::Ne, 99}));
  CHECK(give_class(give->values.front()) == GiveClass::Element);
  const ScanEvent* set = find_event(result, EventKind::SetFlag, 0);
  CHECK(set != nullptr);
  if (set) CHECK(set->flag == give->flag);
}

// A battle behind a story test and a CALL, a scene battle, and a branch decided by two literals that hides a give.
void test_battles_and_pruning() {
  Asm a;
  const u16 update = a.here();
  const u16 to_skip = a.jump_if_story_ge(40);
  const u16 to_sub = a.call();
  a.battle_scene(3).stop();
  a.patch(to_skip, a.here());
  a.stop();
  a.patch(to_sub, a.here());
  a.battle(5).ret();
  const u16 action = a.here();
  const u16 dead = a.if_literals(1, 0, 0);
  a.give(I(10)).stop();
  a.patch(dead, a.here());
  a.stop();
  const RoomScanResult result = scan_script(make_module(a, {{{1, update}, {2, action}}}));
  int battles = 0;
  for (const ScanEvent& event : result.events) {
    if (event.kind != EventKind::Battle) continue;
    ++battles;
    CHECK(has_cond(event, Cond{Subject::Var, 0, Cmp::Lt, 40}));
    CHECK_EQ(story_range(event.conds).hi, 39);
    CHECK_EQ(event.values.size(), 1);
    if (!event.values.empty()) CHECK(event.values[0] == 5 || event.values[0] == 0x203);
  }
  CHECK_EQ(battles, 2);
  CHECK(find_event(result, EventKind::Give, 0) == nullptr);
}

// A give whose flag is set by a script of another entity, started by RUN_SCRIPT_BLOCKING; a test on a variable the path then
// overwrites is dropped from the event's tests.
void test_flag_in_other_script() {
  Asm a;
  const u16 action = a.here();
  const u16 to_end = a.if_jump(0x90, 0, 1);
  a.set(0x90, 2);
  a.give(I(0x401)).run_blocking(1, 4).stop();
  a.patch(to_end, a.here());
  a.stop();
  const u16 setter = a.here();
  a.set_bit(0x88, 5).stop();
  const RoomScanResult result = scan_script(make_module(a, {{{2, action}}, {{4, setter}}}));
  const ScanEvent* give = find_event(result, EventKind::Give, 0);
  CHECK(give != nullptr);
  if (!give) return;
  CHECK_EQ(give->flag.var, 0x88);
  CHECK_EQ(give->flag.bit, 5);
  CHECK(!give->flag_at_load);
  CHECK(give->conds.empty());
  CHECK(give_class(give->values.front()) == GiveClass::KeyItem);
}

// Code behind a held L1 / R1 (the developers' menus) is not walked; other buttons are ordinary tests.
void test_held_buttons() {
  Asm a;
  const u16 update = a.here();
  const u16 to_circle = a.jump_if_not_key(0x0008);
  a.give(I(10)).stop();
  a.patch(to_circle, a.here());
  const u16 to_end = a.jump_if_not_key(0x0020);
  a.give(I(11)).stop();
  a.patch(to_end, a.here());
  a.stop();
  const RoomScanResult result = scan_script(make_module(a, {{{1, update}}}));
  CHECK_EQ(result.events.size(), 1);
  if (result.events.empty()) return;
  CHECK_EQ(result.events[0].values.front(), 11);
  CHECK(has_cond(result.events[0], Cond{Subject::Key, 0, Cmp::AnyBit, 0x0020}));
  CHECK(has_cond(result.events[0], Cond{Subject::Key, 0, Cmp::NoBit, 0x0008}));
}

void test_helpers() {
  CHECK(give_class(0) == GiveClass::None);
  CHECK(give_class(0x40) == GiveClass::Item);
  CHECK(give_class(0x261) == GiveClass::Element);
  CHECK(give_class(0x400) == GiveClass::None);
  CHECK(give_class(0x43F) == GiveClass::KeyItem);
  CHECK(give_class(0x440) == GiveClass::None);
  CHECK_EQ(give_count(0x261), 5);
  CHECK_EQ(give_count(0x26B), 5);
  CHECK_EQ(give_count(0x26C), 1);
  const StoryRange range = story_range({Cond{Subject::Var, 0, Cmp::Gt, 36}, Cond{Subject::Var, 0, Cmp::Le, 100},
                                        Cond{Subject::Var, 0x88, Cmp::Lt, 5}});
  CHECK_EQ(range.lo, 37);
  CHECK_EQ(range.hi, 100);
  const ScanFlag bit{0x198, 3, 0};
  CHECK(bit.is_set([](int var) { return var == 0x198 ? 0x0008 : 0; }));
  CHECK(!bit.is_set([](int var) { return var == 0x198 ? 0x0004 : 0; }));
  const ScanFlag value{0x152, -1, 9};
  CHECK(value.is_set([](int) { return 9; }));
  CHECK(!ScanFlag{}.is_set([](int) { return 0xFFFF; }));
  CHECK(cond_text(Cond{Subject::Var, 0, Cmp::Ge, 40}) == "story>=40");
  CHECK(cond_text(Cond{Subject::Var, 0x88, Cmp::NoBit, 512}) == "$0088!&512");
  CHECK(flag_text(bit) == "$0198.3");
}

}  // namespace

int main() {
  test_chest();
  test_battles_and_pruning();
  test_flag_in_other_script();
  test_held_buttons();
  test_helpers();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("script_scan: all passed\n");
  return 0;
}
