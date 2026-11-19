#pragma once
// Static scan of one room's field script: every place the script gives or takes an item, element, key item or gold, starts a
// battle, adds a character to the party or the roster, sets the story counter or changes the room, with the tests guarding it on
// the path that reaches it and, for a give, the flag the script sets to remember it. Nothing runs: the code is walked from every
// entity script slot along both sides of each branch.
#include <string>
#include <vector>

#include "core/types.h"
#include "script/field_script.h"

namespace oc::script {

// What a guard tests. Var: a script variable (byte offset; variable 0 is the story counter); InParty / Recruited / Carryover /
// RecruitedOrCarryover / Leader: a character id; Item: an item id in the BB numbering (value 1 = owned, 0 = missing); Money: gold;
// Choice: the dialogue choice result (variable 4, kept apart because it is the player's pick, not state); Key: the held pad
// buttons (ops 50 / 51, `value` = the button mask).
enum class Subject : u8 { Var, InParty, Recruited, Carryover, RecruitedOrCarryover, Leader, Item, Money, Choice, Key };
// AnyBit / NoBit: (subject & value) != 0 / == 0.
enum class Cmp : u8 { Eq, Ne, Gt, Lt, Ge, Le, AnyBit, NoBit };

// One test that holds on the path to an event. `value_var` is set when the right side is a variable (then `value` is its id).
struct Cond {
  Subject subject = Subject::Var;
  int id = 0;
  Cmp cmp = Cmp::Eq;
  int value = 0;
  bool value_var = false;
  auto operator<=>(const Cond&) const = default;
};

// A remembered flag: bit `bit` of global variable `var` (byte offset, even), or with bit < 0 the variable set to `value`.
struct ScanFlag {
  int var = -1;
  int bit = -1;
  int value = 0;
  bool valid() const { return var >= 0; }
  // Whether a save's variables have it set; `read_var` returns a variable by byte offset.
  template <typename ReadVar>
  bool is_set(ReadVar read_var) const {
    if (!valid()) return false;
    const int current = read_var(var);
    return bit >= 0 ? ((current >> bit) & 1) != 0 : current == value;
  }
  bool operator==(const ScanFlag&) const = default;
};

// SetFlag: a write of a flag (a single bit, or a literal value, of a global variable past the engine's scratch range).
enum class EventKind : u8 { Give, Take, Gold, Battle, Join, Roster, Unroster, Scenario, Exit, SetFlag };

// One event. `values`: the operand's possible values (item id in the BB numbering, gold, battle id, character id, story value, room
// id); empty when it could not be resolved, `operand_var` then names the variable it reads. `flag`: for gives, the flag set
// after it; for SetFlag, the flag written. `flag_guarded`: a guard on the path tests the give's flag; `flag_at_load`: some
// entity's INIT tests it (the room shows the thing taken or opened when it loads). `conds`: the tests that hold on every path to
// it from the entity's script slots (each slot is walked on its own, so the tests of a script that starts this one through
// RUN_SCRIPT are not included). `capacity_check`: the path compares a room variable with 99 first, the "cannot carry more" test
// of the chest routine. `op`: the opcode key; `offset`: its code-area offset (stable for a given room file). `value_sites`: for a
// give, the code offsets of the literal words its id comes from: its own operand when that is a literal, else every SET of a literal
// to the room variable it reads whose literal is one of `values` (the chest routine stores the id there; its message, stock test and
// give all read it). `entrance`: for an exit, the entrance (ROOM_PARAM) it enters by, -1 when the operand is not a literal.
struct ScanEvent {
  EventKind kind = EventKind::Give;
  int entity = 0;
  int slot = 0;
  u32 offset = 0;
  u16 op = 0;
  std::vector<int> values;
  int operand_var = -1;
  int entrance = -1;
  ScanFlag flag;
  bool flag_guarded = false;
  bool flag_at_load = false;
  bool capacity_check = false;
  std::vector<Cond> conds;
  std::vector<u32> value_sites;
};

// What an entity's INIT makes of it first: a party slot (0E), a room model (0F), a field monster (D1), or nothing.
enum class EntityRole : u8 { None, Party, Npc, Monster };

// Story values [story_lo, story_hi] and entrances [entrance_lo, entrance_hi] (ROOM_PARAM) at which every way the entity's INIT can run
// leaves it inactive.
struct InactiveRegion {
  int story_lo = 0;
  int story_hi = 0;
  int entrance_lo = 0;
  int entrance_hi = 0;
};

// Per entity: its role, the tests on each INIT path on which the entity makes itself inactive (36 with b = 0 on itself), the state
// in which the room loads without it; the story values and entrances at which the INIT certainly does (its tests on variables 0 and 2
// decided, every other test taken both ways); and whether some script of the room makes it active again (36 with b != 0 on it).
struct EntityScan {
  EntityRole role = EntityRole::None;
  std::vector<std::vector<Cond>> inactive_when;
  std::vector<InactiveRegion> inactive_at;
  bool reactivated = false;
};

struct RoomScanResult {
  std::vector<ScanEvent> events;
  std::vector<EntityScan> entities;
  // The global variables (byte offsets below 0x400) the walked scripts may write, sorted; `writes_any`: a bit write whose bit number
  // is a variable, which may land in any of them.
  std::vector<int> written;
  bool writes_any = false;
  // Instructions decoded by the walk (a measure of work, counted once per entity walk).
  u32 steps = 0;
};

// Walks every script of every entity, except the code behind a held L1 / R1 test (op 50): the developers' menus (room warps, party
// and story setters) that nearly every room carries. Deterministic: the events come in entity, then code order.
RoomScanResult scan_script(const FieldScriptModule& module);

// BB numbering: below 0x200 an item, 0x200..0x3FF an element (id - 0x200), 0x401..0x43F a key item (id - 0x400).
enum class GiveClass : u8 { None, Item, Element, KeyItem };
GiveClass give_class(int id);
// Copies a give adds: 5 for the consumable stack elements, else 1.
int give_count(int id);

// The story counter's range the conditions allow: [lo, hi], hi = 0x7FFF when open-ended.
struct StoryRange {
  int lo = 0;
  int hi = 0x7FFF;
};
StoryRange story_range(const std::vector<Cond>& conds);

// "$0088&512", "story>=36", "Kid in party" ... for tool output.
std::string cond_text(const Cond& cond);
std::string flag_text(const ScanFlag& flag);
const char* event_kind_name(EventKind kind);

}  // namespace oc::script
