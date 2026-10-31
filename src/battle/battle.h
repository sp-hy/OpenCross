#pragma once
// Headless battle core: state, turn system, command application, status ticks, enemy AI hook, victory and growth.
// No rendering; everything observable is in the Unit array, the log, the ActionEvent list and the result.
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "battle/dopple_table.h"
#include "battle/rng.h"
#include "battle/rules.h"
#include "data/tables.h"

namespace oc::battle {

constexpr int kPcSlots = 3, kEnemySlots = 6, kUnits = 9;
inline bool is_pc_slot(int unit) { return unit >= 0 && unit < kPcSlots; }

// Derived equipment totals. The flat defaults below are used only when the gear table is not loaded (synthetic test tables).
struct Equip {
  int wp = 16, wp_acc = 60, wp_var = 20, wp_class = 0;
  // Equipped weapon item id (presentation: weapon pack part = gear tier & 0xF).
  int weapon_id = 0;
  int arm = 4, arm_var = 20;
  // mag_gear is added to the base 1 of the MAG term; mdef_gear to the base 1 of the MDEF term (natural armour 4 -> term 5).
  int mag_gear = 0;
  int mdef_gear = 4;
  int evd_gear = 0;
  // OR of record flags & 0xF: 1 HP%, 2 gold x125/100 after battle, 4 deals x125/100, 8 takes x75/100.
  u16 gear_flags = 0;
  // Weapon mask: crit when (slayer & target hi) != 0.
  u32 slayer = 0;
  // Armour / accessory masks and permanent gear status.
  u32 resist_lo = 0, resist_hi = 0, status_lo = 0, status_hi = 0;
  // Armour mask | flag-0x40 accessory masks: weapon raw x3/4 vs the attacker's hi word.
  u32 guard_hi = 0;
  // The armour record's colour byte (+3): bits 2..7 a colour mask, bit 1 = elements of that colour are absorbed (the colour plates).
  u8 armour_colour = 0;
  // Accessory ids (0xF9..0xFB let the hi 0xE0000000 statuses land).
  std::array<u16, 3> acc{};
  int stamina_mod = 0, start_gauge = 0;
  std::vector<int> hp_pct;
};
// Builds the totals from item ids (weapon, armour, 3 accessories).
Equip make_equip(const data::GameTables& tables, int weapon, int armour, const std::array<u16, 3>& acc);

// Persistent per-character state that survives a battle (save record subset + the star-growth fields).
struct PartyMember {
  int char_id = 0;
  std::string name;
  // Order: maxHP, ATK, DEF, HIT, EVD, MAG, MDEF. snap is the growth snapshot, pending the pending star-up deltas.
  std::array<int, 7> stat{};
  int hp = 0;
  std::array<int, 7> snap{};
  std::array<int, 7> pending{};
  std::array<int, 7> lv99{};
  // Colour bit.
  u8 innate = 0;
  int growth_col = 0, dur = 10, rate = 10, hit_med = 50, hit_heavy = 20;
  std::array<u16, data::kGridCells> cells{};
  std::array<bool, data::kGridCells> unlocked{};
  // Consumable stacks: copies held by the cell, 0 = not a stack; the battle counts it down.
  std::array<u8, data::kGridCells> qty{};
  Equip eq;
  // Poison / burn / sprain / flu carried between battles.
  u32 lo_persist = 0;
  // The record's model / kind byte: the battle model loaded at battle start and Pip's form 0x2B..0x30. -1 = the character id.
  int kind = -1;
  // Pip's form change happened in this battle: kind, growth column and the tech cells go back into the save record.
  bool evolved = false;
};

// Pip's forms. Kind byte 0x2B base, 0x2C / 0x2D first forms, 0x2E..0x30 second.
inline bool pip_form(int kind) { return kind >= 0x2B && kind <= 0x30; }
// Per form three tech element ids (0x11F..0x12D).
inline constexpr std::array<u16, 18> kPipTechs = {0x11F, 0x120, 0x120, 0x121, 0x122, 0x122, 0x123, 0x124, 0x124,
                                                  0x125, 0x126, 0x127, 0x128, 0x129, 0x12A, 0x12B, 0x12C, 0x12D};
// The growth column of each form (= the char.bin records 43..48).
inline constexpr std::array<u8, 6> kPipGrowthCol = {3, 3, 3, 4, 5, 6};
// The grid cells a form change rewrites: (row 2, col 7), (4, 7), (6, 7) = levels 3 / 5 / 7; the third only for a second form (> 0x2D).
inline constexpr std::array<int, 3> kPipTechCells = {27, 47, 67};
inline bool pip_tech(int id) {
  for (u16 tech : kPipTechs)
    if (tech == id) return true;
  return false;
}

struct GrowthState {
  int star = 0, progress = 0, counter = 0;
};

// `star` selects the grid unlocks (el_mask rows 0..star). STUB: cells that hold an element in the first two rows are
// treated as the starting kit (unlocked) in addition to the el_mask unlocks.
PartyMember make_party_member(const data::GameTables& tables, int char_id, int star = 0, const std::string& set = "char");
void unlock_grid(PartyMember& member, const data::GameTables& tables, int star);

struct Unit {
  bool present = false, pc = false;
  // Char id / monster id.
  int src_id = 0;
  std::string name;
  int max_hp = 0, hp = 0;
  int atk = 0, def = 0, hit = 0, evd = 0, mag = 0, mdef = 0;
  int hit_med = 0, hit_heavy = 0;
  u8 innate = 0;
  int dur = 10, rate = 10, ap = 8;
  int stamina = 70, countdown = 0, gauge = -1, gauge_max = 7;
  // Merged status words = permanent | inflicted.
  u32 lo = 0, hi = 0;
  // Innate + gear statuses, OR-ed into the merged words. Enemies copy them from the monster record (whose hi word also holds the monster
  // type bits 0x00200000..0x10000000 the weapon slayer masks and the element exploit test read) and get lo 0x400 at spawn; PCs get the
  // gear's permanent statuses.
  u32 perm_lo = 0, perm_hi = 0;
  // Status weakness masks: the chance of these bits is doubled.
  u32 weak_lo = 0, weak_hi = 0;
  // 0x4000 male, 0x8000 female; enemies rolled at spawn when 0.
  u16 gender = 0;
  // Status skip (immunity) masks = record masks | armour masks | flag-0x40 accessory masks.
  u32 skip_lo = 0, skip_hi = 0;
  // Hit base per strength written by a script; -1 = derived (hit_base).
  std::array<int, 3> hit_ovr{-1, -1, -1};
  // Status index (bit + 32*set) -> remaining.
  std::map<int, int> timers;
  // Percent modifiers in the order Att Hit Def Evd MAtt MDef.
  std::array<int, 6> mod{100, 100, 100, 100, 100, 100};
  std::array<u16, data::kGridCells> cells{};
  std::array<bool, data::kGridCells> usable{};
  std::array<bool, data::kGridCells> used{};
  // The double / triple tech the last element menu build put into the cell, 0 = the cell shows its own element.
  std::array<u16, data::kGridCells> combo_word{};
  // The cell's colour-seal flag as the last refresh left it (Battle::open_element_menu, Battle::refresh_seal_flags): a double / triple tech
  // judges its partners by this flag, not by the live status word.
  std::array<bool, data::kGridCells> seal_flag{};
  // Copies left of a consumable stack cell (PartyMember::qty); 0 = single / none.
  std::array<u8, data::kGridCells> qty{};
  Equip eq;
  // The PC wore the hero set at battle start (Hero's Blade + two Hero's Shield / Medal); a Dopplegang form keeps it.
  bool hero_set = false;
  bool guard = false;
  // PC "defended" flag: the PC's own menu stays closed until another PC has used a menu or time has advanced; cleared by the PC's next
  // menu and on KO.
  bool defending = false;
  // Model shade set by script op 85 (presentation only).
  u8 shade = 0;
  // Remembered command-menu cursor of a PC, set by script op 7B (reset value 3).
  std::array<u8, 2> menu_cursor{3, 3};
  int gold = 0, drop_row = 0, drop_base = 0;
  // Control block word mode 41 fills with script variable 45 at the cast; script op 0x9F reads it.
  int ctrl12 = 0;
  // Already pilfered (mode 30, one steal per enemy).
  bool stolen = false;
  // Enemy name suffix 1..6 = A..F (0 = none).
  u8 letter = 0;
  // Dopplegang: the monster record (= pool slot) the PC has become; -1 = the character's own form.
  int form = -1;
  // The kind byte: PCs = the record's (Pip's form), enemies = the monster record's; scripts read it (field 0x002D).
  int kind = 0;
  bool alive() const { return present && !(lo & (st::kKO | st::kDefeated)); }
  bool can_open_menu() const { return pc && alive() && stamina >= 10 && !(lo & st::kCannotMenuLo) && !defending; }
};

struct Rewards {
  int gold = 0;
  std::vector<u16> items;
  std::vector<u16> elements;
  // Pilfered words (bit 15 = element id, else item id; mode 30): added to the stock at battle end, on a victory only.
  std::vector<u16> stolen;
  // Enemy casts caught by the party's element trap (mode 5): added to the element stock whatever the outcome.
  std::vector<u16> trapped;
  // Grid cells the enemy side's element trap destroyed: (party slot, grid cell row * 10 + col). The retail game writes the save record at
  // once (cell &= 0xC000, whatever the outcome); the host does it when it applies the battle result.
  std::vector<std::pair<int, int>> lost_cells;
  bool star_up = false;
  // Stat deltas applied by growth: per party slot, and per BattleOptions::bench entry (star-up only).
  std::array<std::array<int, 7>, kPcSlots> gains{};
  std::vector<std::array<int, 7>> bench_gains;
};

// The HUD's element-trap block: shown or not, and the element id each side has trapped (0 none).
struct TrapHud {
  bool shown = false;
  int pc = 0, enemy = 0;
  bool operator==(const TrapHud&) const = default;
};

// One resolved effect on one target, in the order the rules produced it (the presentation replays them).
// `action` numbers the player command / enemy turn they belong to; hit_index counts the events of one actor inside one action.
struct ActionEvent {
  enum class Kind { Weapon, Element, Defend, Run } kind = Kind::Weapon;
  int action = 0, hit_index = 0;
  // Unit indices (target -1 for Defend / Run).
  int actor = -1, target = -1;
  // Weapon: 0 weak, 1 medium, 2 heavy.
  int strength = 0;
  // Element: pgdat id; grid row in `row`.
  int element = 0;
  int row = 0;
  // HP lost (code 0) or gained (code 3).
  int value = 0;
  // 0 hit, 1 no damage, 2 miss, 3 heal / absorb.
  int code = 0;
  // ko: the hit knocked the target out.
  bool crit = false, ko = false;
  // The KO is a scripted death (op 5C): the unit stays standing until its parked death cast has run.
  bool ko_owed = false;
  // Pip's form change (element mode 45): the new kind byte 0x2C..0x30; the view swaps the model at the pack's change op.
  int evolve_form = -1;
  // Big Catch (element mode 39): the battle model number of the hooked creature, 0 when the roll failed; -1 = not a mode-39 cast.
  int creature = -1;
  // Part of a scripted death's cast (run_death_plans): the view ends the dying unit's death_owed when the group ends.
  bool death_cast = false;
  // A presentation-only element (mode 50, the death casts): the forced miss shows no number.
  bool silent = false;
  // Part of an all-targets element.
  bool all_targets = false;
  // The cast was caught in the other side's element trap: it fizzles and its own effect never starts.
  bool caught = false;
  // Pilfer (mode 30): message row of the result window, 0 item / 1 element stolen, 2 failed, 3 nothing to steal; -1 = not a steal.
  int steal_msg = -1;
  // The stolen word (bit 15 = element id, else item id); steal_msg 0 / 1.
  int steal_word = 0;
  // The message row of the action-end system window: the element record's row of an element whose mode landed on this target, or 6 ("Frozen -- can't move!")
  // when Frozen newly landed on a target left alive; on a Run event, 5 ('Dropped the stolen goods!') when pilfered loot was pending.
  int sys_msg = -1;
  int hp_before = 0, hp_after = 0;
  // The field ring before this event's cast changed it (the view shows the ring of the action it presents).
  std::array<int, 3> ring{-1, -1, -1};
  // The cast repaints the whole ring when it ends (modes 6 / 37 / 47) instead of pushing its colour.
  bool ring_repaint = false;
  // The element-trap block while this event's action runs, and as it stood before the action began.
  TrapHud trap, trap_before;
  // The colour this cast entered in the colour history (0..5, 6 = colourless, -1 = none) and the history's first seven entries after it
  // (colour + 1, 0 = empty): the Chrono Cross colour row shows them when the action ends.
  int seq_colour = -1;
  std::array<u8, 7> colour_seq{};
  // The log line the rules wrote for this effect (shown when the hit is presented).
  std::string text;
  // Scripted action: Event::seq of the queue_attack that made it; the view starts the hit once its script cursor passed it.
  int script_seq = -1;
  // A monster cast's caster-group list, unit numbers, 0xFF = none.
  std::array<u8, 6> group{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
};

// Result code: 0 victory, 1 wiped, 2 fled, 3+ forced by a battle script (op 83; Battle::result_code()).
enum class Outcome { Ongoing = -1, Victory = 0, Wiped = 1, Fled = 2, Scripted = 3, Stalled = 9 };

// Who an element can be aimed at: the pgdat byte +4. Bit 0x40 = the cursor ring covers the party (units 0..2), bit 0x80 = the enemies
// (3..8), 0xC0 = all nine; 0x10 = the whole range at once; 0x04 = the cursor cannot leave the caster; 0x01 = only knocked-out units
// qualify (Revive). With neither side bit the ring is the enemies.
struct TargetSpec {
  bool party = false, enemies = true;
  bool all = false, self_only = false, ko_only = false;
};
inline TargetSpec target_spec(const data::ElemRec& rec) {
  const u8 mask = rec.b(4);
  TargetSpec spec;
  spec.party = (mask & 0x40) != 0;
  spec.enemies = (mask & 0x40) == 0 || (mask & 0x80) != 0;
  spec.all = (mask & 0x10) != 0;
  spec.self_only = (mask & 0x04) != 0;
  spec.ko_only = (mask & 0x01) != 0;
  return spec;
}

struct Command {
  enum class Kind { Attack, Element, Defend, Run } kind = Kind::Defend;
  // PC slot 0..2.
  int actor = 0;
  // Attack: strengths (0 weak, 1 medium, 2 heavy), max 4.
  std::vector<int> chain;
  // Element: grid cell 0..79.
  int cell = -1;
  // Unit index; -1 = automatic.
  int target = -1;
};

// A double / triple tech a PC's tech cell turns into (Battle::combo_at): the casting PCs in the order of the tech record's
// character bytes (+0x14..+0x16) and the grid cell each of them gives up, -1 = no entry.
struct ComboTech {
  int element = 0;
  std::array<int, 3> units{-1, -1, -1};
  std::array<int, 3> cells{-1, -1, -1};
  // The cell each party slot gives up, -1 = the slot does not take part.
  std::array<int, 3> slot_cells{-1, -1, -1};
};

struct EnemyAction {
  enum class Kind { Weapon, Element, None } kind = Kind::Weapon;
  int strength = 0;
  // Element record id.
  int element = 0;
  // Unit index.
  int target = 3;
  // An element action's target units as a bit mask (bit = unit index): one cast aimed at all of them. 0 = `target` alone.
  u32 targets = 0;
  // Scripted plans: pac::Event::seq of the op 67 that queued it (ActionEvent::script_seq), -1 = not from a script.
  int seq = -1;
  // Op 67's group list (QueuedAttack::group).
  std::array<u8, 6> group{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
};

class Battle;
// Enemy decision hook. `choose` is the one-action fallback (RandomBrain); the other hooks let a scripted brain
// (pac_vm.h ScriptedBrain) run the .pac slots at the moments the retail scheduler runs them.
class EnemyBrain {
 public:
  virtual ~EnemyBrain() = default;
  virtual EnemyAction choose(const Battle& battle, int unit, Rng& rng) = 0;
  // After the units exist, before the first enemy turn (slot 0 init).
  virtual void begin_battle(Battle&, Rng&) {}
  // The unit may act (enemy slot 2; confused / charmed PC slot 2). Return true with the queued actions in `out`
  // (possibly none = the script passes); false = no script, fall back to `choose` / the built-in behaviour.
  virtual bool plan_turn(Battle&, int /*unit*/, Rng&, std::vector<EnemyAction>& /*out*/) { return false; }
  // The scene script variable at that byte offset (mode 41 reads var 45 at 0xDE); mode 45 zeroes the colour-point vars 4..0xE.
  virtual int script_var(int /*byte_off*/) const { return 0; }
  virtual void set_script_var(int /*byte_off*/, int /*value*/) {}
  // Slot 3 (enemy units): the kill script runs once the unit's HP is 0. What it queues with op 67 (the death element of a boss with a
  // scripted death, op 5C) is executed like any queued action - by the dying unit itself - and returned in `out`; the unit stays owed
  // (death_owed) until the script's A6.
  virtual void on_killed(Battle&, int /*unit*/, Rng&, std::vector<EnemyAction>& /*out*/) {}
  // Slot 4: run once per unit of the finished action's target list. Attacks the script queues (op 67) are executed like any queued
  // action (counter-attacks); return them in `out`.
  virtual void on_attacked(Battle&, int /*unit*/, Rng&, std::vector<EnemyAction>& /*out*/) {}
  // The attacks that scripts queued from their idle loops (a hidden unit's arrival cast) since the last call: executed like any queued action by
  // the unit that queued them. Appended to `out` as (unit, actions).
  virtual void take_idle_plans(Battle&, std::vector<std::pair<int, std::vector<EnemyAction>>>& /*out*/) {}
  // Any attack about to resolve (kind 0 weapon / 1 element; id = weapon class or element id): scripts read it as vars 0x20..0x36.
  virtual void on_attack(Battle&, int /*actor*/, int /*kind*/, int /*id*/, int /*target*/) {}
  // The first seven entries of the cast colour history (colour + 1, 0 = empty).
  virtual std::array<u8, 7> colour_sequence() const { return {}; }
  // Script control of the end of battle.
  // Op 81 off suspends the victory / defeat test.
  virtual bool end_check_enabled() const { return true; }
  // Op 83: pending result code, ends the battle at the next test.
  virtual int forced_result() const { return -1; }
  // Op 7C: no drops.
  virtual bool no_drops() const { return false; }
  // Op 8C: marked defeated without dying.
  virtual bool defeated(int /*unit*/) const { return false; }
  // Op AF: the player command phase does nothing while set.
  virtual bool menu_locked() const { return false; }
  // Ops 34 / 35 / the scene seed: off-stage (no victory block, no time-step countdown).
  virtual bool hidden(int /*unit*/) const { return false; }
  // Ops 34 / 35 / 5B / the seed: the cursor ring skips it; 5B alone does not hide.
  virtual bool untargetable(int unit) const { return hidden(unit); }
  // Op 5C with 0 (cleared by the kill script's A6): the unit's defeat is presented by its own kill script. The KO lands, but the battle
  // goes on while the unit is still in the alive mask (until A6), hidden or not.
  virtual bool death_owed(int /*unit*/) const { return false; }
};

// "Random valid attack": weapon strength weighted 60/30/10, 40% element from the unit's grid when it has a usable one.
class RandomBrain final : public EnemyBrain {
 public:
  EnemyAction choose(const Battle& battle, int unit, Rng& rng) override;
};

struct BattleOptions {
  bool ng_plus = false;
  // Gear gold bonus (x125/100).
  bool gold_bonus = false;
  // Stage index (background table row for the field ring); -1: the scene record's background.
  int battlefield = -1;
  // In: current star/progress; out: after the battle.
  GrowthState growth;
  // Characters outside the party (the star-up touches all 44). On a star-up they receive their old pending deltas (clamped to Lv99),
  // then everybody (party + bench, in char id order) rolls the next pending deltas.
  std::vector<PartyMember> bench;
  // The Dopplegang pool (the save's 16 words, also battle script vars 0x60..0x7F): 256 bits, bit n of word w = pool slot w * 16 + n.
  std::array<u16, 16> dopple_mask{};
  // Script variable 0x176: Van's piggy-bank savings in hundreds of G (element mode 46 adds it to the power).
  int piggy_bank = 0;
  // A monster-party scene (scene record byte 0x0F bit 0, the Grand Slam): party slot i fights as monster record monster_party[i] (the
  // Grand Slam picker's pool slots); -1 leaves the slot empty.
  std::array<int, kPcSlots> monster_party{-1, -1, -1};
};

class Battle {
 public:
  // Script op AF.
  bool menu_locked() const { return brain_.menu_locked(); }
  Battle(const data::GameTables& tables, Rng& rng, EnemyBrain& brain) : tables_(tables), rng_(rng), brain_(brain) {}

  // Builds the 3 party units (party slot i = unit i) and the scene's enemies, then runs enemy turns / time until a PC can
  // act. Returns false if the scene id is invalid.
  bool start(std::vector<PartyMember> party, int scene, const BattleOptions& opt = {});

  // ---- views
  const Unit& unit(int index) const { return units_[static_cast<size_t>(index)]; }
  Unit& unit_mut(int index) { return units_[static_cast<size_t>(index)]; }
  const std::array<int, 3>& ring() const { return ring_; }
  // Dev: replaces the field ring (colour indices 0..5, kRingAllColours or -1 per slot).
  void set_ring(const std::array<int, 3>& ring) { ring_ = ring; }
  // The element-trap block once the running action has ended: a trap set by it shows, a trap that sprang during it hides the whole block
  // (even while the other side's trap stays armed).
  TrapHud trap_hud() const { return {trap_state_ == 1 || trap_state_ == 2, trap_[0], trap_[1]}; }
  Outcome outcome() const { return outcome_; }
  // The Outcome value, or the script's code for Outcome::Scripted.
  int result_code() const { return result_code_; }
  const Rewards& rewards() const { return rewards_; }
  const std::vector<PartyMember>& party() const { return party_; }
  const GrowthState& growth() const { return growth_; }
  // After a star-up: stats / snap / pending updated.
  const std::vector<PartyMember>& bench() const { return bench_; }
  int scene() const { return scene_; }
  // The party slots fight as monsters (BattleOptions::monster_party): nothing of the fight goes back to the party records.
  bool monster_party() const { return monster_party_; }
  int actions() const { return actions_; }
  // Hidden / off-stage units (script ops 34 / 35 / 5B): never on the cursor ring, rejected by `validate`, drawn by nothing.
  bool hidden(int unit) const { return brain_.hidden(unit); }
  bool untargetable(int unit) const { return brain_.untargetable(unit); }
  // The units an all-target cast (pgdat +4 bit 0x10) may reach: seeded from the scene record (the enemies the scene keeps off-stage until a
  // script reveals them), ops 34 / 35 / 5B with arg 0 set a unit's bit; it is ANDed into the enemy-side masks (the party side is never masked).
  u32 cast_mask() const { return cast_mask_; }
  // ---- Dopplegang pool (the live mask is the save's)
  const std::array<u16, 16>& dopple_mask() const { return dopple_mask_; }
  bool dopple_has(int slot) const { return slot >= 0 && slot < 256 && (dopple_mask_[static_cast<size_t>(slot >> 4)] >> (slot & 15) & 1); }
  // The slots in the pick menu's order (ascending slot).
  std::vector<int> dopple_pool() const;
  // Script op 65 on an enemy unit (the start of its kill script): when the PC whose command came last (script var 0x50) is character 26
  // (Sprigg) or wears accessory 0xFC, and the enemy's monster record allows it, the pool gets the enemy's slot bit. True when a bit was set.
  bool dopple_credit(int enemy_unit, int last_pc);
  // Dopplegang's unit swap. form >= 0: PC unit `unit` takes monster record `form` (stats, innate colour, gear, status words, the monster's
  // element grid, its name); the character's own record is kept for the way back; HP and max HP stay, inflicted statuses and timers are gone,
  // AP 8, gauge -1. form < 0: back to the character (the saved record, with the HP of now). False when nothing changed.
  bool transform(int unit, int form);
  // Pip's evolution: script op A7 sets the pending form element (0x80..0x85); while it is set every PC turn is the unit's script slot 2,
  // which casts it; its pgdat mode 45 applies the form: evolve().
  int evo_pending() const { return evo_pending_; }
  void set_evo_pending(int element) { evo_pending_ = element; }
  void evolve(int unit, int form);
  // Element colour (pgdat +5) as the battle sees it: Pip's 18 techs take Pip's innate colour, Vary Innate its last caster's.
  u8 element_colour(int id) const;
  // The colour an element cast enters in the colour history: 0..5 (an element without a colour byte enters as 0), 6 for the all-colour
  // element, -1 for none (record byte 0 bit 0x80: death casts, form changes). PC and enemy casts alike.
  int seq_colour(int element_id) const;
  // A party member's grid holds the Chrono Cross element: the HUD shows the colour history as a row of seven dots.
  bool colour_row_shown() const { return colour_row_; }
  void set_cast_bit(int unit, bool on) {
    if (unit >= 0 && unit < kUnits) cast_mask_ = on ? cast_mask_ | (1u << unit) : cast_mask_ & ~(1u << unit);
  }
  const data::GameTables& tables() const { return tables_; }
  std::vector<std::string> drain_log() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
  }
  const std::vector<std::string>& log() const { return log_; }
  std::vector<ActionEvent> drain_events() {
    std::vector<ActionEvent> out;
    out.swap(events_);
    return out;
  }
  const std::vector<ActionEvent>& events() const { return events_; }
  // Diagnostics: stubbed / unimplemented rule paths that were hit, key -> count (e.g. "element mode 32", "pac field read 6004").
  const std::map<std::string, int>& stub_hits() const { return stubs_; }
  void note_stub(const std::string& key) { ++stubs_[key]; }

  // ---- queries
  // No PC gets its command menu while Pip's form element is pending: every PC turn is its script slot 2 then.
  bool can_act(int pc) const { return outcome_ == Outcome::Ongoing && unit(pc).can_open_menu() && evo_pending_ == 0; }
  // Element cell usable now (gauge, flags, stamina, status).
  bool element_usable(int pc, int cell) const;
  // Colour-sealed cell: the PC's merged lo has the seal bit of the cell's colour (green 0x800, white 0x8000, black 0x80000, red 0x800000,
  // yellow 0x8000000, blue 0x80000000). Sealed cells stay on the menu, greyed, and refuse the confirm.
  bool element_sealed(int pc, int cell) const;
  // A double / triple tech the cell offers now (`ComboTech`, element 0 = none).
  ComboTech combo_at(int pc, int cell) const;
  // Builds the element menu of a PC (the host calls it when the grid opens; an Element command does it first): every PC's menu table starts
  // from its own cells, each double / triple tech the PC's cells offer is written into its cells and its partners' cells, and the PC's own
  // colour-seal flags are judged again by what its cells show now.
  void open_element_menu(int pc);
  // Judges the colour-seal flag of every cell of a PC by the element its cell shows (a double / triple tech's colour where the last menu
  // build put one there); rainbow cells keep their flag.
  void refresh_seal_flags(int pc);
  // The element the cell casts now: the double / triple tech combo_at offers, else the cell's own id.
  int cell_element(int pc, int cell) const;
  int chain_cost(int strength, const Unit& unit) const { return weapon_stamina_cost(strength, (unit.lo & st::kFatigued) != 0); }
  bool validate(const Command& cmd, std::string* why = nullptr) const;
  // Percent, using the current chain counter.
  int hit_chance_for(int actor, int target, int strength) const;
  // Auto-battle accuracy rule: the strongest hit k = 2, 1, 0 whose chance is above 79 % and whose stamina need is met (30 / 20 / 10,
  // fatigued 35 / 22 / 10); none -> -1, or weak when `fallback_weak` and it can be paid (the cursor rests on row 0).
  int auto_hit_level(int actor, int target, bool fallback_weak = true) const;
  int alive_count(bool pcs) const;

  // ---- commands
  // False if invalid (nothing happens).
  bool apply(const Command& cmd, std::string* why = nullptr);

  // ---- primitives (public so tests and tools can drive single actions; they do not run the scheduler)
  void do_weapon_attack(int actor, int target, int strength);
  void note_summon_kill(int unit_index);
  // `push_ring` = false for the 2nd..nth target of an all-targets cast (the colour enters the field ring once per cast).
  void do_element(int actor, int element_id, int row, int target, bool push_ring = true);
  // The PCs other than `actor` and those in `excluded` (a mask of PC slots) recover stamina; the enemy countdowns run `steps`.
  void step_after_pc_action(int actor, int steps, bool weapon, u32 excluded = 0);
  void post_action(int actor, int poison_amount, int timer_steps);
  void time_advance();
  // Enemy turns + time advance until a PC can act or the battle ends.
  void settle();
  void check_end();
  // Debug tools: after a primitive above was called from outside the scheduler (Debug tab cast): slot-4 reactions, then the end test.
  void finish_external_action();
  // Debug tools (Debug tab): damage never takes a unit of that side below 1 HP and no instant KO lands on it. Rules stay untouched when off.
  void set_invincible(bool party, bool enemies) { inv_party_ = party, inv_enemies_ = enemies; }
  bool invincible(int unit) const { return unit < kPcSlots ? (inv_party_ || cheats_.god_mode || cheats_.boost) : inv_enemies_; }
  // Cheats tab: the rules' bypasses, all off by default; the host pushes the live set every update.
  struct Cheats {
    bool god_mode = false, boost = false, max_stamina = false, free_elements = false, always_hit = false, always_loot = false, instant_growth = false;
  };
  void set_cheats(const Cheats& cheats) { cheats_ = cheats; }
  const Cheats& cheats() const { return cheats_; }

  // Dev casts (no script queued them): the caster group the next events carry.
  void set_group_hint(const std::array<u8, 6>& group) { cur_group_ = group; }
  // The running / last begun action (ActionEvent::action of its events).
  int action_id() const { return action_id_; }
  // A killed enemy whose kill script (slot 3) queued a death cast that has not been executed yet (the unit still owes its death presentation).
  bool death_pending(int unit) const { return unit >= 0 && unit < kUnits && death_pending_[static_cast<size_t>(unit)]; }

 private:
  void say(std::string line) { log_.push_back(std::move(line)); }
  void begin_action() {
    ++action_id_;
    hit_counter_ = 0;
    trap_state_ = trap_hud().shown ? 1 : 0;
    trap_before_ = trap_hud();
  }
  void reset_chain() { chain_actor_ = chain_target_ = -1; chain_hits_ = 0; }
  void note_event(ActionEvent event) {
    if ((event.kind == ActionEvent::Kind::Weapon || event.kind == ActionEvent::Kind::Element) && event.target >= 0 &&
        std::find(attacked_.begin(), attacked_.end(), event.target) == attacked_.end())
      attacked_.push_back(event.target);
    event.action = action_id_;
    event.script_seq = cur_script_seq_;
    event.group = cur_group_;
    event.hit_index = hit_counter_++;
    event.ring = ring_;
    event.trap = {trap_state_ == 1 || trap_state_ == 3 || trap_state_ == 4, trap_[0], trap_[1]};
    event.trap_before = trap_before_;
    events_.push_back(event);
  }
  WeaponIn weapon_in(const Unit& attacker, const Unit& target_unit, int mult, int strength) const;
  // The element's colour is sealed on the PC.
  bool colour_sealed(int pc, int id) const;
  // The cell of `pc` that can join a double / triple tech with partner tech `tech`, -1 = none. A PC's seal flag, not its live status, decides.
  int combo_partner_cell(int pc, int tech) const;
  void load_monster_record(Unit& unit, const data::StatRec& rec, u32 perm_lo_bits) const;
  // Single target, the whole side (element record byte 4 bit 0x10), or the units of `targets` (EnemyAction::targets) when it names more
  // than one: one cast in every case, so the field ring takes its colour once.
  void cast_element(int actor, int element_id, int row, int target, u32 targets = 0);
  void damage_unit(int actor, int target, int value, int code);
  void kill(int unit);
  // Runs the kill script (slot 3) of a dead enemy once and parks the death cast it queued.
  void run_kill_script(int unit);
  // Every enemy whose HP is 0 or that is KO'd without its kill script having run (a script wrote its HP to 0) gets that script now.
  void scan_kills();
  // True when any roll landed.
  bool apply_status(int target, const data::ElemRec& rec);
  bool apply_status_bits(int target, u32 lo, u32 hi, int chance, u8 colour_bit = 0);
  bool apply_status_bits(int target, u32 mask, int set, int chance) { return apply_status_bits(target, set == 0 ? mask : 0, set == 0 ? 0 : mask, chance); }
  // Per bit, no roll.
  bool force_status_bits(int target, u32 lo);
  void set_status_bit(int target, int set, int bit);
  void enemy_turn(int unit);
  // The queued death casts of killed units (slot 3), run by run_reactions.
  void run_death_plans();
  // The casts the scripts' idle loops queued, run by run_death_plans.
  void run_idle_plans();
  // Slot 4 of every unit of the last action's target list, queued counter-attacks executed.
  void run_reactions();
  void confused_turn(int unit);
  // Returns {poison step, timer steps}.
  std::pair<int, int> exec_action(int unit, const EnemyAction& action);
  void run_enemy_action(int unit, const EnemyAction& action);
  bool run_plan(int unit, const std::vector<EnemyAction>& plan, bool set_countdown);
  void finish(Outcome outcome, int code = -1);
  void compute_rewards();
  // The innate byte's colour mask is bits 2..7; bit 1 makes the unit absorb elements of its own colour.
  int colour_of(const Unit& unit) const { return data::colour_index_of_bit(static_cast<u8>(unit.innate & 0xFC)); }
  // A multi-caster tech's participants, unit numbers, -1 = none: a PC's double / triple tech lists the party slots holding the
  // character ids of element record +0x14..+0x16 (in that order); a monster's group tech (+0x17 = the count) the caster group.
  std::array<int, 6> tech_participants(int actor, const data::ElemRec& element) const;
  // The field ring at the end of a cast: a repaint (modes 6 / 37 / 47) replaces all three slots, else the cast's colour enters as the newest
  // slot; an element with record byte 0 bit 0x80 and an element trap (mode 5) leave the ring as it was.
  void finish_ring(const data::ElemRec& element, int colour);
  void init_pc(int slot, const PartyMember& member);
  void init_enemy(int slot, int monster_id, const BattleOptions& opt);
  void init_party_monster(int slot, int monster_id, const BattleOptions& opt);
  void unlock_monster_grid(Unit& unit) const;
  int next_ready_enemy();
  void god_filter(int target, DamageOut& dmg) const;
  // Max stamina / battle boost refills, run after every action and hit.
  void apply_cheat_state();

  const data::GameTables& tables_;
  Rng& rng_;
  EnemyBrain& brain_;
  std::array<Unit, kUnits> units_{};
  std::array<int, 3> ring_{-1, -1, -1};
  std::vector<PartyMember> party_, bench_;
  std::vector<std::string> log_;
  std::vector<ActionEvent> events_;
  std::map<std::string, int> stubs_;
  // Target list of the running action, in order (slot-4 launches).
  std::vector<int> attacked_;
  // Slot-3 plans of units killed during the running action (kill()), executed once it has ended (run_reactions): unit, queued attacks.
  std::vector<std::pair<int, std::vector<EnemyAction>>> death_plans_;
  // Casts queued by idle loops, not yet executed: unit, queued attacks.
  std::vector<std::pair<int, std::vector<EnemyAction>>> idle_plans_;
  // run_death_plans is executing a plan (ActionEvent::death_cast).
  bool in_death_cast_ = false;
  // A kill script queued attacks that have not run yet: the unit still owes its death (check_end).
  std::array<bool, kUnits> death_pending_{};
  // The unit's kill script has run since it last had HP.
  std::array<bool, kUnits> kill_script_run_{};
  // By colour index: the enemies (one bit per unit) killed during a summon of that colour. summon_colour_ = the current action's, -1 = not a summon.
  std::array<u16, 6> summon_kills_{};
  int summon_colour_ = -1;
  int action_id_ = 0, hit_counter_ = 0;
  // The executing scripted entry's / dev cast's caster group (ActionEvent::group).
  std::array<u8, 6> cur_group_{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  // run_plan: the scripted entry being executed.
  int cur_script_seq_ = -1;
  bool cast_all_ = false;
  // Set by set_invincible.
  bool inv_party_ = false, inv_enemies_ = false;
  Cheats cheats_;
  Outcome outcome_ = Outcome::Ongoing;
  int result_code_ = -1;
  Rewards rewards_;
  GrowthState growth_;
  BattleOptions opt_;
  int scene_ = 0, actions_ = 0, cursor_ = 0;
  bool boss_ = false, monster_party_ = false;
  std::array<u16, 16> dopple_mask_{};
  // The PC's record before its first Dopplegang form.
  std::array<Unit, kPcSlots> form_backup_{};
  // Big Catch (mode 39) per-cast state: 0 not rolled, 1 the roll failed, 2 a creature was picked (reset at the end of a cast); the creature's
  // battle model number (0 = the value the command cleared it to) and pool slot.
  int creature_state_ = 0, creature_model_ = 0, creature_slot_ = 0;
  // Pip's pending form element (cleared at battle init and by mode 45).
  int evo_pending_ = 0;
  // The colour kPipTechs take (0 = no Pip in the party).
  u8 pip_tech_colour_ = 0;
  // See cast_mask().
  u32 cast_mask_ = 0x1FF;
  int chain_actor_ = -1, chain_target_ = -1, chain_hits_ = 0;
  // Timed field state (mode 6): 1 = elements x1/2, 2 = x3/2.
  int field_state_ = 0;
  // Mode 5: element id the PC side / the enemy side has trapped (0 none).
  std::array<int, 2> trap_{};
  // The element-trap block: 0 hidden, 1 shown, 2 a trap set by the running action (shown once it ends), 3 / 4 the PC / enemy side's trap
  // sprang during the running action (shown until it ends, then the block hides). trap_before_ = the block as the running action began.
  int trap_state_ = 0;
  TrapHud trap_before_;
  // The running cast repaints the ring (modes 6 / 37 / 47) with ring_repaint_ when it ends instead of pushing its colour; the targets of the
  // cast all see the ring as it was.
  bool ring_repainted_ = false;
  std::array<int, 3> ring_repaint_{-1, -1, -1};
  // Vary Innate's colour: the element takes its caster's innate colour mask when it is cast and keeps it for the rest of the battle (0 = its own).
  u8 vary_innate_colour_ = 0;
  bool colour_row_ = false;
  // The last cast hit the other side's element trap.
  bool fizzled_ = false;
};

// Simple deterministic player policy for `ocbattle sim`: heal when someone is below 40%, else the strongest
// usable damage element, else a weak->medium->heavy chain that fits the stamina budget.
Command auto_command(const Battle& battle, int pc);

}  // namespace oc::battle
