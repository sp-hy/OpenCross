#pragma once
// Battle script VM: parses battle/event/evtNNN.pac, runs the per-entity frames/slots,
// and drives enemy decisions through ScriptedBrain. The VM talks to the world only through `Host`, so it can be tested
// without a Battle; `BattleHost` (pac_vm.cpp) binds it to the headless core.
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "battle/battle.h"
#include "core/types.h"

namespace oc::battle::pac {

constexpr int kSlots = 32;
constexpr int kMaxFrames = 32;
constexpr int kNoUnit = 0xFF;

// ---- opcode table (pac_opcodes.inc). len 0 = invalid / default-stall handler.
// conf: C = handler read, I = purpose inferred, U = in no shipped script (not modelled).
struct OpInfo {
  u8 code = 0, len = 0;
  const char* name = "";
  char conf = 'U';
};
const OpInfo& op_info(u8 code);

// ---- module
// start / end are offsets from the script base.
struct ScriptExtent {
  int entity = 0, slot = 0;
  u32 start = 0, end = 0;
};

struct Module {
  // Whole file.
  Bytes bytes;
  int entities = 0;
  // File offsets: script area, end of the script block.
  size_t base = 0, end = 0;
  // [entity][slot], offsets from base (0 = none).
  std::vector<std::array<u16, kSlots>> ptr;

  bool parse(Bytes data, std::string* err = nullptr);
  size_t code_size() const { return end > base ? end - base : 0; }
  u8 at(u32 rel) const { return rel < code_size() ? bytes[base + rel] : u8{0}; }
  // Script extents (entity 0 slot 0 = code before the first pointer is inserted first).
  std::vector<ScriptExtent> scripts() const;
};

// ---- what a script asked for / said
struct QueuedAttack {
  // 0/5 weapon, 1/2/3/6 element, 4 reset countdown.
  int type = 0;
  // Weapon strength or element id.
  int value = 0;
  // Target units (bit u).
  u32 mask = 0;
  // Event::seq of the Queue event the op 67 emitted (the view gates the hit's replay on it).
  int seq = -1;
  // Script vars 0x10.. as they stood at the last type-2 queue.
  std::array<u8, 6> group{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
};

struct Event {
  // Per kind: the emitting op(s) and the meaning of unit / a / b.
  enum class Kind {
    // E0: open text box a.
    Dialogue,
    // E2.
    WaitDialogue,
    // 62: a = x, b = y.
    DialogPos,
    // 58/5A: unit, a = anmseq entry (< 0x80 the model's S5, >= 0x80 the external table), b = 1: only if no stream runs.
    Anim,
    // 3D: unit, a.
    Pose,
    // 86/7A: unit.
    Camera,
    // 83: a = battle result code (3/4).
    SetResult,
    // 7C.
    NoDrops,
    // 8C: a = mask.
    Defeated,
    // 2C/38: a = 1 begin, 0 end.
    Cutscene,
    // 34/35/5B: unit, a = untargetable.
    Ghost,
    // 96: a.
    Invuln,
    // A6 reached for unit.
    Killed,
    // 84: unit.
    QuarterHp,
    // 85: unit, a = shade value.
    Shade,
    // 91: unit (refresh the cached model position).
    SyncPos,
    // 5C: unit, a = flag value (1 = the script owns the death presentation).
    ScriptedDeath,
    // 7B: a = value written to both command-menu cursor bytes of PCs 0-2.
    MenuCursor,
    // 39 / 7D / 81 / AF: a = opcode, b = byte written.
    EngineFlag,
    // C0 / C2: a = pool (0 = C0/C1/C4 list, 1 = C2/C3/C5 list), b = 4-char tag (little-endian u32).
    EffectStart,
    // C1 / C3: a = pool.
    EffectStop,
    // AE matched the colour sequence: battle sound calls are switched off.
    SfxMute,
    // 67: the entry entered the unit's attack queue (a = type, b = value); QueuedAttack::seq == this event's seq.
    Queue,
    // 2C / 65 / 66 / A4 / 2A passed: the real op blocks until the action executor is idle (a = op, b = Battle action id at that moment).
    Barrier,
    // ScriptedBrain: a slot run starts here (vt = its start); only carries the virtual time.
    Mark,
  } kind = Kind::Dialogue;
  int unit = -1, a = 0, b = 0;
  // Virtual script time: Vm::tick() count at emission (a 2D delay of n advances it by 2n). Events are produced by a synchronous run, so
  // the view replays them paced by the vt gaps and blocks on the real waits (E2 window, Barrier executor) itself.
  u32 vt = 0;
  // Monotonic over the whole battle (emission order).
  int seq = 0;
};

// ---- world interface
struct PickResult {
  // 0 = none.
  int element = 0;
  u32 mask = 0xFFFF;
};

class Host {
 public:
  virtual ~Host() = default;
  // Bit u (0..8): unit present and not KO.
  virtual u32 alive_mask() const = 0;
  // Bit u: unit in the battle roster (PCs also while KO'd; enemies until their kill script ends).
  virtual u32 roster_mask() const { return alive_mask(); }
  // Bit u: unit present at battle start (kept after its death).
  virtual u32 present_mask() const { return roster_mask(); }
  virtual int read_field(int unit, u16 code) = 0;
  virtual void write_field(int unit, u16 code, int value) = 0;
  // Op A7: Pip's pending form element.
  virtual void set_evo_pending(int /*element*/) {}
  virtual int rnd(int lo, int hi) = 0;
  virtual int pc_char_id(int /*pc*/) const { return 0xFF; }
  virtual int monster_id(int /*unit*/) const { return 0xFF; }
  // 0..5 colour, 6 empty.
  virtual int ring_colour(int /*slot*/) const { return 6; }
  virtual int timer_frames() const { return 0; }
  // Gate of 66 / 65 / 2C.
  virtual bool action_ready() const { return true; }
  // Op 1F.
  virtual int libc_rand() { return rnd(0, 0x7FFF); }
  // Battle action id now (Event::Barrier.b).
  virtual int action_serial() const { return 0; }
  // Op 65 passed its gate for `unit` (an enemy: the Dopplegang pool credit); `last_pc` = script var 0x50.
  virtual void begin_scripted(int /*unit*/, int /*last_pc*/) {}
  // C4 / C5 block while the engine still has a live effect object in `pool` (tag 0 = any). Headless hosts have none.
  virtual bool effect_busy(int /*pool*/, u32 /*tag*/) const { return false; }
  // 7B / 85: unit state the core does not simulate but keeps for the presentation.
  virtual void set_menu_cursor(int /*v*/) {}
  virtual void set_shade(int /*unit*/, int /*v*/) {}
  // 55 / 8F: pick a usable grid cell of `unit` (levels 0..limit, limit < 0 = the unit's gauge) and its target mask.
  virtual PickResult pick_element(int /*unit*/, int /*limit*/) { return {}; }
  // 34 / 35 / 5B with arg 0: the unit joins the all-target cast masks (Battle::cast_mask).
  virtual void reveal(int /*unit*/) {}
  // 5B with arg != 0: the unit leaves the all-target cast masks.
  virtual void conceal(int /*unit*/) {}
};

class Vm {
 public:
  Vm(const Module& module, Host& host);

  // One interpreter pass over every entity (once per frame).
  void tick();
  // Pushes a slot frame (2 about to act, 3 killed, 4 attacked) on the entity bound to `unit`. False if there is no such
  // script, the entity is not bound, or the slot is already running.
  bool launch(int unit, int slot);
  // launch + tick until the pushed frame has returned. Attacks queued by the script are collected into `out` whenever the
  // script waits for the executor (A4 / 2A) and when it ends. False on timeout or if the slot could not start.
  bool run_slot(int unit, int slot, std::vector<QueuedAttack>& out, int max_ticks = 6000);

  int entity_of_unit(int unit) const;
  int unit_of_entity(int entity) const { return ctx_[static_cast<size_t>(entity)].unit; }
  bool has_slot(int unit, int slot) const;
  int lock() const { return lock_; }
  int frame_count(int entity) const { return static_cast<int>(ctx_[static_cast<size_t>(entity)].frames.size()); }
  std::vector<QueuedAttack>& queue(int unit) { return queue_[static_cast<size_t>(unit)]; }

  // Dev casts (no script queued them): the group list a type-2 queue_attack of `element` would carry = the operands of the 8D set_slots before it (immediates, or
  // the vars as they stand now). nullopt = the module has no such queue_attack.
  std::optional<std::array<u8, 6>> probe_group(int element) const;

  u16 var(int byte_off) const { return vars_[static_cast<size_t>(byte_off & 0xFFFE) >> 1]; }
  void set_var(int byte_off, int value) { vars_[static_cast<size_t>(byte_off & 0xFFFE) >> 1] = static_cast<u16>(value); }

  std::vector<Event> drain_events() {
    std::vector<Event> drained;
    drained.swap(events_);
    return drained;
  }
  // Dialogue boxes (E0 / E2) only emit events; the battle rules resolve a turn synchronously, so nothing waits on the box.
  // Collapses op 2D waits (unit tests that want a script to run through in few ticks).
  void set_skip_delays(bool skip) { skip_delays_ = skip; }
  // Virtual time (ticks of the 2D delay counter) and the settle pass: after a slot run every entity's idle loop (slot 1: the other units'
  // reactions to what the slot just did, e.g. the Boxer Boys' var 0x84 -> dialogue) is ticked until all entities are back at the start of
  // their idle script with nothing armed, or `max_ticks`. False = still busy at the cap. A tick in which an entity changed a var that an
  // earlier entity had already read in that tick does not settle: the reader sees the new value only on the next tick.
  u32 vtime() const { return vtime_; }
  bool quiescent() const;
  bool settle(int max_ticks = 1500);
  // The attacks the idle loops queued during settle passes (unit, entry), oldest first: nothing else collects them, the executor plays
  // them like any queued action (a hidden unit's arrival cast).
  std::vector<std::pair<int, QueuedAttack>> take_idle_attacks() { return std::exchange(idle_attacks_, {}); }
  // Records a Mark event (a slot run starts).
  void mark() { emit(Event::Kind::Mark); }

  // diagnostics: 'U' ops executed (advanced by length), and opcodes with no length that were reached.
  const std::set<int>& unimplemented() const { return unimplemented_; }
  const std::set<int>& invalid() const { return invalid_; }
  u64 ops_executed() const { return ops_; }
  const std::array<u64, 256>& op_counts() const { return op_counts_; }

  // Engine globals the scripts set. Defaults are the values after battle init.
  // 39: gates the per-frame battle UI update.
  bool ui_enabled() const { return flag_ui_; }
  // 81: gates the victory / defeat check.
  bool end_check_enabled() const { return flag_end_; }
  // AF: blocks the player command phase.
  bool menu_locked() const { return flag_menu_lock_; }
  // 7D: shows list entry 100 in the PC menu.
  bool menu_entry100() const { return flag_831_; }
  // 5C.
  bool scripted_death(int unit) const { return unit >= 0 && unit < kUnits && scripted_death_[static_cast<size_t>(unit)]; }
  // AE.
  bool sfx_muted() const { return sfx_muted_; }
  // 83: result code the next end check ends the battle with (-1 none).
  int forced_result() const { return forced_result_; }
  // 7C: the battle end rolls no drops.
  bool no_drops() const { return no_drops_; }
  // 8C: gold + drops are paid for these slots only.
  bool defeated(int unit) const { return unit >= 0 && unit < 16 && (defeated_mask_ >> unit & 1) != 0; }
  // 34 / 35 / spawn seed: off-stage.
  bool hidden(int unit) const { return unit >= 0 && unit < kUnits && ghost_[static_cast<size_t>(unit)]; }
  // Untargetable flag: written together with the hidden flag by ops 34 / 35; op 5B writes ONLY this one.
  bool untargetable(int unit) const { return unit >= 0 && unit < kUnits && untarget_[static_cast<size_t>(unit)]; }
  // Dev (OC_GHOST).
  void set_hidden(int unit, bool on) { if (unit >= 0 && unit < kUnits) ghost_[static_cast<size_t>(unit)] = untarget_[static_cast<size_t>(unit)] = on; }
  // Enemy spawn seeding: the hidden and untargetable flags of the units in hidden_mask are set, and the units in owed_mask start owing
  // their death. Called once before the slot-0 pass: the listed enemy slots start off-stage / owing their death.
  void seed_scene(u32 hidden_mask, u32 owed_mask) {
    for (int unit = 0; unit < kUnits; ++unit) {
      ghost_[static_cast<size_t>(unit)] = untarget_[static_cast<size_t>(unit)] = (hidden_mask >> unit & 1u) != 0;
      scripted_death_[static_cast<size_t>(unit)] = (owed_mask >> unit & 1u) != 0;
    }
  }
  // The cast colour history: called once per element cast (PC or enemy) with colour 0..5, 6 = colourless. Feeds AB / AE and var 0x52.
  void note_cast_colour(int colour);
  const std::array<u8, 8>& colour_history() const { return chist_; }

 private:
  struct Frame {
    u16 pc = 0xFFFF;
    u8 slot = 0, mark = 0xFF, depth = 0;
    std::array<u16, 4> ret{};
  };
  struct Ctx {
    std::vector<Frame> frames;
    int unit = kNoUnit;
    bool dropped = false;
    int group = 0xFF;
    bool s2 = false, s3 = false, s4 = false;
    int delay = 0;
    bool delay_armed = false, a4_wait = false;
  };
  enum class Step { Next, Jump, Block, Stop };

  void refresh_reserved();
  void interpret(int entity);
  Step exec(int entity, Frame& frame, u8 op, int& adv);
  void do_halt(int entity);
  int target_mask(int entity, int code);
  int emit(Event::Kind kind, int unit = -1, int arg_a = 0, int arg_b = 0);
  int self_unit(int entity) const { return ctx_[static_cast<size_t>(entity)].unit; }
  u32 targetable() const;

  const Module& mod_;
  Host& host_;
  std::vector<u16> vars_;
  // Per var: the tick of its last script read and the lowest entity that read it in that tick; the script writes of the op being run
  // (var index, value before); set when a write changed a var an earlier entity read this tick.
  std::vector<u32> read_tick_;
  std::vector<u8> read_entity_;
  std::vector<std::pair<size_t, u16>> writes_;
  bool stale_read_ = false;
  std::vector<Ctx> ctx_;
  std::array<std::vector<QueuedAttack>, kUnits> queue_{};
  // Filled by a type-2 queue_attack, kept until the next.
  std::array<u8, 6> grp_{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  // Hidden (off-stage) and untargetable flags per unit.
  std::array<bool, kUnits> ghost_{};
  std::array<bool, kUnits> untarget_{};
  // Queue of an unbound entity.
  std::vector<QueuedAttack> scratch_;
  std::vector<std::pair<int, QueuedAttack>> idle_attacks_;
  std::vector<Event> events_;
  std::set<int> unimplemented_, invalid_;
  int lock_ = kNoUnit;
  bool skip_delays_ = false;
  u32 vtime_ = 0;
  int seq_ = 0;
  // The entity / slot frame being interpreted (emit's idle-loop dedupe).
  int cur_ent_ = 0, cur_slot_ = 0;
  // (entity, kind) -> last unit / a / b emitted from a slot-1 loop.
  std::map<std::pair<int, int>, std::array<int, 3>> idle_last_;
  int budget_ = 0;
  u64 ops_ = 0;
  std::array<u64, 256> op_counts_{};
  bool flag_ui_ = true, flag_end_ = true, flag_menu_lock_ = false, flag_831_ = false, sfx_muted_ = false, no_drops_ = false;
  int forced_result_ = -1;
  // Op 8C (and A6's own bit is the unit's death, which the rules see as hp 0).
  u32 defeated_mask_ = 0;
  std::array<bool, kUnits> scripted_death_{};
  // Colour + 1 of the last casts, 0 empty; ccur_ is the write cursor.
  std::array<u8, 8> chist_{};
  int ccur_ = 0;
  // The previous cast ended the sequence (colour 6), history restarts.
  bool creset_ = false;
};

// ---- enemy brain driven by the scripts
class ScriptedBrain final : public EnemyBrain {
 public:
  using Loader = std::function<std::optional<Bytes>(int scene)>;
  ScriptedBrain();
  explicit ScriptedBrain(Loader loader);
  ~ScriptedBrain() override;

  void set_loader(Loader loader) { loader_ = std::move(loader); }
  // For tests / hand-assembled scripts.
  void set_module(int scene, Bytes bytes) { forced_[scene] = std::move(bytes); }
  // false: the next begin_battle loads no script (the RandomBrain fallback plans, no end-of-battle control). Debug tab battles with replaced enemies.
  void set_enabled(bool on) { enabled_ = on; }

  // Fallback when no script applies.
  EnemyAction choose(const Battle& battle, int unit, Rng& rng) override;
  void begin_battle(Battle& battle, Rng& rng) override;
  bool plan_turn(Battle& battle, int unit, Rng& rng, std::vector<EnemyAction>& out) override;
  void on_killed(Battle& battle, int unit, Rng& rng, std::vector<EnemyAction>& out) override;
  void on_attacked(Battle& battle, int unit, Rng& rng, std::vector<EnemyAction>& out) override;
  void on_attack(Battle& battle, int actor, int kind, int id, int target) override;
  void take_idle_plans(Battle& battle, std::vector<std::pair<int, std::vector<EnemyAction>>>& out) override;
  std::array<u8, 7> colour_sequence() const override;
  bool end_check_enabled() const override { return !vm_ || vm_->end_check_enabled(); }
  int forced_result() const override { return vm_ ? vm_->forced_result() : -1; }
  bool no_drops() const override { return vm_ && vm_->no_drops(); }
  bool defeated(int unit) const override { return vm_ && vm_->defeated(unit); }
  bool menu_locked() const override { return vm_ && vm_->menu_locked(); }
  bool hidden(int unit) const override { return vm_ && vm_->hidden(unit); }
  bool untargetable(int unit) const override { return vm_ && vm_->untargetable(unit); }
  int script_var(int byte_off) const override { return vm_ ? vm_->var(byte_off) : 0; }
  void set_script_var(int byte_off, int value) override {
    if (vm_) vm_->set_var(byte_off, value);
  }
  bool death_owed(int unit) const override { return vm_ && vm_->scripted_death(unit); }

  bool active() const { return vm_ != nullptr; }
  Vm* vm() { return vm_.get(); }
  std::vector<Event> drain_events();
  // diagnostics for `ocbattle pac-soak`
  // reaction_attacks counts the slot-4 counter-attacks executed.
  struct Stats {
    int turns = 0, empty_plans = 0, fallbacks = 0, timeouts = 0, reaction_attacks = 0;
  };
  const Stats& stats() const { return stats_; }
  const std::set<int>& unknown_fields() const;
  // The executor actions of a turn's queue entries (op 67).
  static void to_actions(const Battle& battle, const std::vector<QueuedAttack>& queued, std::vector<EnemyAction>& out);

 private:
  class BattleHost;
  bool run(int unit, int slot, std::vector<QueuedAttack>& queued);
  Loader loader_;
  bool enabled_ = true;
  std::map<int, Bytes> forced_;
  std::unique_ptr<Module> module_;
  std::unique_ptr<BattleHost> host_;
  std::unique_ptr<Vm> vm_;
  std::vector<Event> events_;
  RandomBrain fallback_;
  Stats stats_;
  std::set<int> no_fields_;
  std::tuple<int, int, int> last_cast_{-1, -1, -1};
};

}  // namespace oc::battle::pac
