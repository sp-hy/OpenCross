#pragma once
// Field script VM: runs the original room event bytecode.
//
// Model: every entity has 8 script slot records (pc, priority class, script id, hold, wait counter, ...), a 4 entry
// call stack and a few flag words. tick() gives every entity, in index order, at most 8 instructions (plus the budget
// bonuses some ops grant); the record with the lowest class value runs, the others stay suspended. When every record
// of an entity is idle the default UPDATE script (slot 1) restarts. load() zeroes the room-local variables and runs
// each entity's INIT script (slot 0) to completion before the first tick. All world access goes through FieldHost.
//
// Variables are 16-bit slots addressed by byte offset (ids 0x000..0x7FF): slots 0..0x1FF are global and saved with the
// game, 0x200.. are room-local. Reads apply the room's signedness mask.
#include <array>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "core/types.h"
#include "script/field_host.h"
#include "script/field_script.h"

namespace oc::script {

constexpr u16 kRecIdle = 0xF;
constexpr int kRecordsPerEntity = 8;
constexpr int kCallDepth = 4;
// Dialogue result byte of an entity whose last window took no confirmed choice.
constexpr u8 kNoChoice = 0xFF;
constexpr int kVarSlots = 0x400;
constexpr int kGlobalVarSlots = 0x200;

// Well-known variable ids (byte offsets into the variable array).
namespace var {
constexpr u16 kScenario = 0x00, kRoomParam = 0x02, kChoiceResult = 0x04, kMapId = 0x06, kPrevMapId = 0x08,
              kBattleResult = 0x32, kCurrentFrame = 0x44;
}

// Bits of Entity::flags38 / flags270 / the global flag word that the VM itself interprets (others are just stored).
namespace flag38 {
constexpr u32 kSelected = 0x1, kNoAction = 0x8, kNoCollision = 0x4, kInactive = 0x10, kNo3dObject = 0x4000, kOffScreen = 0x8000;
}
namespace flag270 {
constexpr u32 kInputLock = 0x1, kTouchTrigger = 0x40, kActionRunning = 0x80, kMoving = 0x10000;
}
namespace gflag {
constexpr u32 kCameraBusy = 0x1, kInputLock = 0x2, kFormation = 0x8, kFormationHold = 0x10, kPartyOverride = 0x40, kKeyListHold = 0x2000, kPauseAllowed = 0x4000, kForceRun = 0x8000,
              kKeepCustomName = 0x10000, kMenuUnderLock = 0x80000;
}

// One script slot record of an entity. `state` is the priority class (the lowest runs): 0xF idle, 7 the default UPDATE
// loop, 4 ON_COLLISION, 3 ON_ACTION, 2 the touch trigger. `id` is the script slot number (0xFF none). `hold` marks a
// child spawned by a blocking RUN_SCRIPT (08 / 09): the record is not reused until the parent saw it finish. `wait` is
// the WAIT (2D) countdown, `flag47` the per-record slot flag op 47 sets, `phase` the handshake state of the blocking ops
// (08 / 09, battles, menus) and `child_ent` / `child_rec` the child record a blocking 08 / 09 waits for.
// `stuck`, `loops` and `last_start_pc` feed the softlock watchdog only: ticks the record yielded on the same instruction,
// and ticks its dispatch started at the same pc (a polling loop `3A / IF_JUMP / JUMP` never changes its start pc).
struct ScriptRecord {
  u32 pc = 0;
  u16 state = kRecIdle;
  u16 id = 0xFF;
  u16 hold = 0;
  u16 wait = 0;
  u16 flag47 = 0;
  u16 phase = 0;
  s16 child_ent = -1;
  s8 child_rec = -1;
  u32 stuck = 0, loops = 0, last_start_pc = 0xFFFFFFFFu;
};

// Script-visible state of one room entity. Besides the records, call stack and flag words it carries the plain values
// scripts store and the host reads back: `axis` (27 28 29 AA), `word_b8` (2A), the trigger sizes (8B 8C), `v10c`,
// `v10e`, `v110` (B0 / B1), `v114` (curve turn rate of the walks, FE 2D), `v116` (footstep walker id, FE 40), `v24e`
// (model turn rate per tick, A5 / CF; 0 = the model never turns) with its saved copy `v240`, `monster_ordinal` (the
// entity's bit in the room's defeated-monster mask, D1 / D2), `head_turn_rate` (FE 3C / FE 62), `array25a` (footstep
// sounds, FE 5C), `last_choice` (the dialogue result byte E2 publishes: kNoChoice when the entity opens a window, the
// confirmed line once a choice is confirmed), `timer_acc` (B1) and `scale` (82 / FE 0D, 0x200 per axis = 0.125).
// flags270 starts with bit 23 set: no footprints until FE 2C 0. `last_op` / `last_pc` are for debug views only.
struct Entity {
  std::array<ScriptRecord, kRecordsPerEntity> rec{};
  std::array<u32, kCallDepth> stack{};
  u8 depth = 0;
  u8 cur = 0;
  u32 flags38 = 0, flags270 = 0x800000, flags274 = 0;
  u16 axis[3] = {0, 0, 0};
  s32 word_b8 = 0;
  u16 trigger_radius = 0, trigger_height = 0, rect_w = 0, rect_h = 0;
  u16 monster_ordinal = 0;
  u16 v10c = 0, v10e = 0, v110 = 0, v114 = 0x80, v116 = 0xFFFF, v24e = 0x80, v240 = 0x80, head_turn_rate = 0xE00;
  u16 array25a[8] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
  u8 last_choice = 0;
  s32 timer_acc = 0;
  u32 scale[3] = {0x200, 0x200, 0x200};
  u16 last_op = 0;
  u32 last_pc = 0;
};

// Counters for the soak tools: `invalid_pc` scripts ended because the pc left the code area, `init_overruns` INIT
// scripts that hit the step cap, `call_overflows` CALLs with the 4-entry stack full (the script stalls), `unimplemented`
// table opcodes without a handler, `unknown_ops` opcodes not in the table (key -> hits; each advances 1 byte, 2 for FE),
// `uncertain_ops` '?' confidence opcodes that ran, `op_hits` per opcode (FE page at +0x100).
struct VmStats {
  u64 ticks = 0;
  u64 instructions = 0;
  u64 invalid_pc = 0;
  u64 init_overruns = 0;
  u64 call_overflows = 0;
  u64 unimplemented = 0;
  std::map<u16, u64> unknown_ops;
  std::map<u16, u64> uncertain_ops;
  std::array<u32, 0x200> op_hits{};
};

class FieldVM {
 public:
  using RandFn = std::function<u32()>;
  // The script ops use the low 15 bits of each draw (the game's rand() range).
  static constexpr u32 kRandMask = 0x7FFF;
  // `rng` replaces the default deterministic generator (the game's rand() step, fixed seed).
  explicit FieldVM(FieldHost& host, RandFn rng = {});

  // Loads a room: replaces the module, resets entity state, zeroes the room-local variables (globals persist),
  // sets the engine variable 0x46 = 1, flags entities without ON_ACTION / ON_COLLISION scripts, then runs INIT
  // (slot 0) of entity 0, 1, 2, ... to completion.
  // `flag_table` is the room's attr -> flag-word table; ops 75-78 edit it in place (byte lanes) and the walk mesh blocks
  // by it. It is not part of the save: it is reloaded with every room.
  void load(FieldScriptModule module, std::span<const u32> flag_table = {});
  const std::array<u32, 256>& flag_table() const { return flag_table_; }
  u32 flag_table_writes() const { return flag_writes_; }

  // One logic tick: every entity runs, in index order, up to 8 instructions of its current script.
  void tick();

  // Trigger tests of the field loop. Each returns true when the script was started: ON_ACTION (slot 2, class 3; refused
  // while input is locked), ON_COLLISION (slot 3, class 4) and the touch trigger (slot 4, class 2, entities with the
  // touch-trigger flag).
  bool trigger_action(int ent);
  bool trigger_collision(int ent);
  bool trigger_touch(int ent);
  // Starts script `slot` of `ent` with priority class `cls` unless it is already live or empty / no record is free.
  bool start_script(int ent, int slot, int cls);

  // ---- state access --------------------------------------------------------------------------------------------------
  const FieldScriptModule& module() const { return mod_; }
  size_t entity_count() const { return ents_.size(); }
  Entity& entity(int ent) { return ents_[static_cast<size_t>(ent)]; }
  const Entity& entity(int ent) const { return ents_[static_cast<size_t>(ent)]; }
  // The effect actor lists the field scripts fill with FE 08 / FE 09 / FE 0A (slot index 0..5): the entity index in slot
  // `slot` of table 0 / 1 / 2, or -1 when the slot is empty. FE 09 fills the caster list, FE 0A the target list the
  // room effect basis selectors read.
  int ent_ref(int table, int slot) const {
    if (table < 0 || table > 2 || slot < 0 || slot >= 6) return -1;
    const u8 entity = ent_ref_tables_[table][static_cast<size_t>(slot)];
    return entity == 0xFF ? -1 : static_cast<int>(entity);
  }

  s32 var(u16 id) const;
  void set_var(u16 id, s32 value);
  // The water level table op 66 fills (default 10).
  s16 water_level(size_t slot) const { return slot < table_9fdc_.size() ? table_9fdc_[slot] : s16(10); }
  // The room register that 5A and the movie-end jump of DC store in PREVIOUS_MAP_ID; the New Game+ launcher sets it to 8.
  void set_room_register(int room) { room_reg_ = room; }
  // A room change is committed: PREVIOUS_MAP_ID = the room register, then the register names the next room.
  void note_room_change(int room);
  // `bit_ref` = (id << 4) | bit, as the bit opcodes 71 / 72 / 73 encode it.
  bool var_bit(u32 bit_ref) const;
  std::span<u16> variables() { return vars_; }
  u32 global_flags() const { return gflags_; }
  void set_global_flags(u32 flags) { gflags_ = flags; }
  // Seeds a global the host keeps elsewhere (the saved window frames) without notifying the host; `global` reads the values
  // the set_global ops wrote.
  void seed_global(Global which, s32 value) { globals_[static_cast<size_t>(which)] = value; }
  s32 global(Global which) const { return globals_[static_cast<size_t>(which)]; }
  // The FE 4F slot table: entity index in slot `slot`, -1 when empty.
  int slot_entity(size_t slot) const { return slot < slot_ent_.size() && slot_ent_[slot] != 0xFF ? slot_ent_[slot] : -1; }
  // FE 50 ran since the room load (the world-map Continue).
  bool world_continue() const { return world_continue_; }
  // The entity whose script is executing (valid inside host calls).
  int current_entity() const { return ent_; }
  const VmStats& stats() const { return stats_; }
  VmStats& stats() { return stats_; }

  // ---- softlock watchdog ---------------------------------------------------------------------------------------------------
  // `id` / `cls` are the script slot number and priority class of the record, `pc` the code offset of the instruction
  // it waits on, `key` its opcode key (0xFExx on the extended page). `strict` = it yields on the same instruction every
  // tick; false = a polling loop (same start pc, instructions in between). `holds_lock` = the entity holds the input lock.
  struct StuckInfo {
    int ent = 0, rec = 0;
    u16 id = 0, cls = 0;
    u32 pc = 0;
    u16 key = 0;
    u32 ticks = 0;
    bool strict = true;
    bool holds_lock = false;
  };
  // Records that have waited at least `min_ticks` (strict waits of any class; polling loops only of class < 7 or lock holders).
  std::vector<StuckInfo> stuck_records(u32 min_ticks) const;
  // Every live record of every entity but the idle UPDATE loops, one line each (the report of a long input lock).
  std::string describe_records() const;
  // Some record of class < 7 (ON_ACTION, ON_COLLISION, touch, or an async script started with a class) is still live.
  bool triggered_busy() const {
    for (const auto& entity : ents_)
      for (const auto& record : entity.rec)
        if (record.state < 7) return true;
    return false;
  }
  static std::string describe(const StuckInfo& info);

 private:
  FieldHost& host_;
  RandFn rng_;
  FieldScriptModule mod_;
  std::vector<Entity> ents_;
  std::array<u16, kVarSlots> vars_{};
  // The room register: the host's MAP_ID write at a room load / Continue (-1 = none yet: variable 6 is used), 8 after
  // FE 2F (set while variable 6 ends 0). 5A and DC's movie-end jump store it in PREVIOUS_MAP_ID.
  int room_reg_ = -1;
  int room_register() const;
  u32 gflags_ = 0;
  std::array<s32, static_cast<size_t>(Global::Count)> globals_{};
  u32 flag_writes_ = 0;
  // Tables the ops fill: the flag table (75..78, 4 byte lanes per word), the water levels (66), the pair table (AF), the
  // slot entities (FE 4F) and the three effect actor lists (FE 08 / 09 / 0A).
  std::array<u32, 256> flag_table_{};
  std::array<s16, 256> table_9fdc_{};
  std::array<std::pair<s32, s32>, 256> pair_table_{};
  std::array<u8, 8> slot_ent_{};
  std::array<u8, 256> ent_ref_tables_[3]{};
  VmStats stats_;
  std::set<u16> logged_;

  // Execution context of the record currently running. `base_` is the offset of the opcode (main page) or sub-opcode
  // (FE page); operands are addressed from it. `world_continue_` = FE 50 ran since the room load: at the end of INIT the FE 4F
  // slot entities are restored from variables 0x300.. and the room entry keeps the saved scroll. `ended_` = the record was released / reset: stop the dispatcher loop.
  int ent_ = 0;
  Entity* self_ = nullptr;
  ScriptRecord* rec_ = nullptr;
  u32 base_ = 0;
  bool yield_ = false;
  bool world_continue_ = false;
  bool ended_ = false;
  bool run_mode_ = false;
  int budget_ = 8;
  // The character the last accepted 7B put into the party (-1 none): the 7C after it re-runs that character's entity.
  int staged_char_ = -1;

  // Operand access relative to base_: raw bytes / words, the U form (literal when bit 15 is set, else a variable) and
  // the S form (literal when `mask` is set in the mode byte at `mode_off`, else a variable).
  u8 byte_at(u32 offset) const { return mod_.code_byte(base_ + offset); }
  u16 word_at(u32 offset) const { return static_cast<u16>(byte_at(offset) | (byte_at(offset + 1) << 8)); }
  s32 imm_or_var(u16 raw) const;
  s32 operand_u(u32 offset) const { return imm_or_var(word_at(offset)); }
  s32 operand_s(u32 offset, u32 mode_off, u32 mask) const;
  s32 read_var(u32 id) const;
  void write_var(u32 id, s32 value) { vars_[(id >> 1) & (kVarSlots - 1)] = static_cast<u16>(value); }
  bool var_unsigned(u32 id) const;
  // Character reference (entity index, party slot, character id or self) -> entity index, -1 when absent.
  int resolve_character(u8 char_ref) const;
  int entity_operand(u32 offset) const { return resolve_character(byte_at(offset)); }
  bool has_model(int ent) const { return ent >= 0 && !(ents_[static_cast<size_t>(ent)].flags38 & flag38::kNo3dObject); }

  enum class Spawn { None, Spawned, Deduped, NoRecord };
  Spawn spawn(int target, int id, int cls, bool hold, int* rec_index);
  int pick_record(int ent) const;
  void dispatch(int ent, int rec_index, bool run_mode);
  void run_entity(int ent);
  void run_init(int ent);
  void relink_party_entity(int char_id);
  u32 load_flags(size_t ent) const;
  void release();
  void forget_child();
  void exec(u8 op);
  bool exec_core(u8 op);
  bool exec_scene(u8 op);
  bool exec_game(u8 op);
  void exec_ext(u8 sub);
  void note_unknown(u16 key);
  bool menu_request(MenuKind kind, int arg);
  void alu(u8 op);
  void set_flag(u32& word, int bit, bool on) const { word = on ? word | (1u << bit) : word & ~(1u << bit); }
  void set_global_bit(int bit, bool on) { set_flag(gflags_, bit, on); }
  void turn_flag_13(Entity& entity, bool on);
  void opaque(u16 key, u32 first_operand, u32 count);
  void set_global(Global which, s32 value);
  s32 isqrt_dist(s32 dx, s32 dy, s32 dz) const;
  // Character id in party slot `slot`, 0xFF when the slot is empty (the word the original party table holds).
  s32 party_table(int slot) const {
    const int char_id = host_.party_member(slot);
    return char_id < 0 ? 0xFF : char_id;
  }
  StuckInfo stuck_info(size_t ent, size_t rec, u32 ticks, bool strict) const;

  // Instruction stepping. advance(n): the next instruction is n bytes from base_. park(): the record yields for good (a
  // parked ON_ACTION record is done for the player, so its action-running bit clears). jump_or_advance: branch to the
  // J operand at `jump_offset` or skip the `length`-byte instruction. wait_or_advance / gate helpers of the wait forms.
  void advance(u32 length) { rec_->pc = base_ + length; }
  void park();
  void jump_or_advance(bool jump, u32 jump_offset, u32 length);
  void wait_or_advance(bool busy, u32 length);
  bool system_gate();
  bool busy_gate();
  void begin_motion(const MotionRequest& request, bool free_height);
  void head_turn(HeadOp op, int ent, int other, s32 x, s32 y, s32 z, int rate);
};

}  // namespace oc::script
