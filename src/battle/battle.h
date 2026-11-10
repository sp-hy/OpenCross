#pragma once
// Headless battle core: state, turn system, command application, status ticks, enemy AI hook, victory and growth.
// No rendering; everything observable is in the Unit array, the log, the ActionEvent list and the result.
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "battle/dopple_table.h"
#include "battle/rng.h"
#include "battle/rules.h"
#include "data/tables.h"

namespace oc::battle {

constexpr int kPcSlots = 3, kEnemySlots = 6, kUnits = 9;
// The units players command at most: the party slots, and in a versus battle the second side's units 3..5 too.
constexpr int kPlayerUnits = 2 * kPcSlots;
inline bool is_pc_slot(int unit) { return unit >= 0 && unit < kPcSlots; }
// The side a unit fights on: 0 = units 0..2 (the party, versus side A), 1 = units 3..8 (the enemies; versus side B in 3..5).
inline int side_of(int unit) { return unit < kPcSlots ? 0 : 1; }
inline int side_first(int side) { return side == 0 ? 0 : kPcSlots; }
inline int side_end(int side) { return side == 0 ? kPcSlots : kUnits; }
// Dopplegang: the element whose cast opens the creature pick (Command::Kind::Form).
inline constexpr int kDopplegangElement = 0xEE;

// Derived equipment totals. The flat defaults below are used only when the gear table is not loaded (synthetic test tables).
struct Equip {
  int wp = 16, wp_acc = 60, wp_var = 20, wp_class = 0;
  // The weapon's class bit 7 (rods, boomerangs): a PC's heavy hit with it strikes every enemy (Battle::do_weapon_sweep).
  bool wp_sweeps = false;
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
  // Sum of the signed +4 bytes of the flag-0x80 accessories, added to the wearer's duration stat when a status timer starts.
  int dur_mod = 0;
  std::vector<int> hp_pct;
};
// Builds the totals from item ids (weapon, armour, 3 accessories).
Equip make_equip(const data::GameTables& tables, int weapon, int armour, const std::array<u16, 3>& acc);

// Persistent per-character state that survives a battle (save record subset + the star-growth fields).
struct PartyMember {
  int char_id = 0;
  // name: what the battle shows for the member; own_name: the typed or default name, which the Dopple list shows whatever the body.
  std::string name, own_name;
  // Order: maxHP, ATK, DEF, HIT, EVD, MAG, MDEF. snap is the growth snapshot, pending the pending star-up deltas.
  std::array<int, 7> stat{};
  int hp = 0;
  std::array<int, 7> snap{};
  std::array<int, 7> pending{};
  std::array<int, 7> lv99{};
  // The record's starting stats (the character data's, before any growth): where the max-growth cheat's path begins.
  std::array<int, 7> base{};
  // Colour bit.
  u8 innate = 0;
  int growth_col = 0, dur = 10, rate = 10, hit_med = 50, hit_heavy = 20;
  std::array<u16, data::kGridCells> cells{};
  std::array<bool, data::kGridCells> unlocked{};
  // Consumable stacks: copies held by the cell, 0 = not a stack or none left; the battle counts it down.
  std::array<u8, data::kGridCells> qty{};
  // The cell is a consumable stack the save counts (qty holds its copies, also 0).
  std::array<bool, data::kGridCells> stack{};
  Equip eq;
  // The record's status byte at battle start; after the battle its poison / burn / sprain / flu bits (lo & kPersist).
  u32 lo_persist = 0;
  // The record's model / kind byte: the battle model loaded at battle start and Pip's form 0x2B..0x30. -1 = the character id.
  int kind = -1;
  // Pip's form change happened in this battle: kind, growth column and the tech cells go back into the save record.
  bool evolved = false;
};

// Pip's forms. Kind byte 0x2B base, 0x2C / 0x2D first forms, 0x2E..0x30 second.
inline bool pip_form(int kind) { return kind >= 0x2B && kind <= 0x30; }
inline constexpr int kPipCharId = 43;
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

// `star` selects the grid unlocks (el_mask rows 0..star). Cells that hold an element in the first two rows are
// treated as the starting kit (unlocked) in addition to the el_mask unlocks.
PartyMember make_party_member(const data::GameTables& tables, int char_id, int star = 0, const std::string& set = "char");
// The Lv 99 targets of lv99para.bin record `record` (a character id, or Pip's form 0x2B..0x30), in PartyMember stat order; all 0
// without that record set.
std::array<int, 7> lv99_targets(const data::GameTables& tables, int record, const std::string& set = "char");
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
  // type bits 0x00200000..0x10000000 the weapon slayer masks and the element exploit test read) and get st::kInfoShown at spawn; PCs get
  // the gear's permanent statuses and st::kPartyShown.
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
  // judges its partners by this flag, not by the live status word, and an enemy's element pick (script ops 55 / 8F) skips a flagged cell.
  std::array<bool, data::kGridCells> seal_flag{};
  // Copies left of a consumable stack cell (PartyMember::qty); 0 = single / none.
  std::array<u8, data::kGridCells> qty{};
  // PartyMember::stack: a stack cell with no copies left is greyed (used) whenever the element menu opens.
  std::array<bool, data::kGridCells> stack{};
  Equip eq;
  // The PC wore the hero set at battle start (Hero's Blade + two Hero's Shield / Medal); a Dopplegang form keeps it.
  bool hero_set = false;
  // Set by Defend: weapon damage taken is halved and the idle script holds the block pose. It ends when the PC's command menu opens
  // (Battle::open_menu, also implied by its next command) and on KO.
  bool guard = false;
  // Home facing byte (home yaw = byte << 6, 4096 = 360 deg): the formation facing byte << 3 at battle start, rewritten by script op 85.
  u8 home_facing = 0;
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
  // An enemy drawn as a character: that kind byte (the Lynx duel, scene 0x1DD); -1 = its own model.
  int body = -1;
  // The kind byte: PCs = the record's (Pip's form), enemies = the monster record's; scripts read it (field 0x002D).
  int kind = 0;
  // A battle script wrote the HP to 0: the kill script still runs, but the unit never takes the KO of a resolved hit, so its model is not dead.
  bool hp_written_zero = false;
  // Frozen (lo 0x20000000) landed with its timer: the unit's next action first clears lo 0x60000000 (Battle::thaw_on_action).
  bool freeze_pending = false;
  // The unit's result code slot (0 hit, 1 no damage, 2 miss, 3 heal; 0xFF = it was the last action's caster). It keeps its value between
  // actions: a mode that writes no code (36) shows the one the unit's last action left.
  int result_code = 0;
  bool alive() const { return present && !(lo & (st::kKO | st::kDefeated)); }
  // In the turn order: a frozen unit is out of alive() but its countdown runs and it takes its turn at 0.
  bool in_turn_order() const { return present && !(lo & st::kKO); }
  // The after-battle growth, star-up and results-screen heal test: only the KO bit counts, so a member still frozen at the end grows.
  bool conscious() const { return present && !(lo & st::kKO); }
  // On the target ring and in every script target pick, count and attacked-script test: in the turn order with HP not 0 (a frozen unit
  // stands).
  bool standing() const { return in_turn_order() && hp != 0; }
  bool can_open_menu() const { return pc && alive() && stamina >= 10 && !(lo & st::kCannotMenuLo); }
  // The cell's lock (the original's grid word bit 14): a character's grid keeps the bit in every cell word and holds its lock in the star
  // unlocks (`usable`); a monster grid (an enemy, a creature form, a monster-party slot) holds it in the word.
  bool cell_locked(size_t cell) const { return pc && form < 0 ? !usable[cell] : (cells[cell] & 0x4000) != 0; }
};

struct Rewards {
  int gold = 0;
  std::vector<u16> items;
  std::vector<u16> elements;
  // Pilfered words (bit 15 = element id, else item id; mode 30): added to the stock at battle end, on a victory only.
  std::vector<u16> stolen;
  // Enemy casts caught by the party's element trap (mode 5): added to the element stock whatever the outcome.
  std::vector<u16> trapped;
  // Grid cells the enemy side's element trap destroyed: (character id, grid cell row * 10 + col). The retail game writes the save record
  // at once (cell &= 0xC000, whatever the outcome); the host does it when it applies the battle result.
  std::vector<std::pair<int, int>> lost_cells;
  bool star_up = false;
  // Stat deltas applied by growth: per party slot, and per BattleOptions::bench entry (star-up, or any victory with shared growth).
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
  enum class Kind { Weapon, Element, Defend, Run, Swap } kind = Kind::Weapon;
  int action = 0, hit_index = 0;
  // Unit indices (target -1 for Defend / Run / Swap).
  int actor = -1, target = -1;
  // Swap: the character ids that left and took the actor's slot.
  int swap_out = -1, swap_in = -1;
  // Weapon: 0 weak, 1 medium, 2 heavy.
  int strength = 0;
  // Weapon: the unit a sweep was aimed at (the heavy hit of a rod or boomerang, which strikes every enemy); -1 = a one-target hit. The
  // events of one sweep follow each other in ascending unit order and share hit_index.
  int aim = -1;
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
  // Battle script op 96's colour gate cancelled the result: the hit deals 0 and its number shows "0".
  bool gated = false;
  // The result word the element mode wrote into the caster's control block (anim op 0xA9 reads it): Beta (mode 47) the field colour
  // bit << 2, Shuffle (mode 48) the swapped HP value; -1 = the mode wrote none.
  int caster_word = -1;
  // Part of an all-targets element.
  bool all_targets = false;
  // The cast was caught in the other side's element trap: it fizzles, and the trap pack of its colour plays instead of its own effect.
  bool caught = false;
  // The effect id the cast plays instead of its record's (+0x10): a caster sealed in the element's colour plays 190 + colour (1 green .. 6
  // blue); -1 = its own.
  int fx_eid = -1;
  // Pilfer (mode 30): message row of the result window, 0 item / 1 element stolen, 2 failed, 3 nothing to steal; -1 = not a steal.
  int steal_msg = -1;
  // The stolen word (bit 15 = element id, else item id); steal_msg 0 / 1.
  int steal_word = 0;
  // The message row of the action-end system window: the element record's row of an element whose mode landed on this target, or 6 ("Frozen -- can't move!")
  // when Frozen newly landed on a target left alive; on a Run event, 5 ('Dropped the stolen goods!') when pilfered loot was pending.
  int sys_msg = -1;
  // Frozen (lo 0x20000000) newly landed on this target, whatever its HP.
  bool froze = false;
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

// Result code: 0 victory, 1 wiped, 2 fled, 3+ forced by a battle script (op 83; Battle::result_code()). A versus battle reads from side
// A: Victory = side A won, Wiped = side B won, Draw = both sides went down on the same action.
enum class Outcome { Ongoing = -1, Victory = 0, Wiped = 1, Fled = 2, Scripted = 3, Stalled = 9, Draw = 10 };

// How a versus battle ended: the last characters of a side knocked out, of both sides at once, a Run (surrender), the third timeout in a
// row of one side, a side conceded by the host (a player who stayed away), or nobody could ever act again (a draw).
enum class VersusEnd { None, Knockout, Draw, Surrender, Timeouts, Conceded, Stalled };
// Three turns in a row ended by a timeout forfeit the battle.
inline constexpr int kVersusTimeoutLimit = 3;
// Side A's stamina on its opening turn.
inline constexpr int kVersusOpeningStamina = 35;

// Who an element can be aimed at: the pgdat byte +4. Bit 0x40 = the cursor ring covers the party (units 0..2), bit 0x80 = the enemies
// (3..8), 0xC0 = all nine; 0x10 = the whole range at once; 0x04 = the cursor cannot leave the caster; 0x01 = only knocked-out units
// qualify (Revive); 0x02 = a both-sides ring opens on the party. With neither side bit the ring is the enemies.
struct TargetSpec {
  bool party = false, enemies = true;
  bool all = false, self_only = false, ko_only = false, party_first = false;
};
inline TargetSpec target_spec(const data::ElemRec& rec) {
  const u8 mask = rec.b(4);
  TargetSpec spec;
  spec.party = (mask & 0x40) != 0;
  spec.enemies = (mask & 0x40) == 0 || (mask & 0x80) != 0;
  spec.all = (mask & 0x10) != 0;
  spec.self_only = (mask & 0x04) != 0;
  spec.ko_only = (mask & 0x01) != 0;
  spec.party_first = (mask & 0x02) != 0;
  return spec;
}

// The unit ids a target cursor tries in turn, by the target byte alone, items and elements alike: the caster for a self-only element; a
// both-sides ring from unit 0 with bit 0x02, else from the first enemy; a party ring from unit 0 (not the caster); an enemy ring from
// `last_enemy`, the caster's last enemy target, wrapping within the enemies.
inline std::array<int, 9> target_cursor_order(const TargetSpec& spec, int caster, int last_enemy) {
  std::array<int, 9> order;
  order.fill(-1);
  if (spec.self_only) {
    order[0] = caster;
  } else if (spec.party && spec.enemies) {
    const int first = spec.party_first ? 0 : kPcSlots;
    for (int i = 0; i < kUnits; ++i) order[static_cast<size_t>(i)] = (first + i) % kUnits;
  } else if (spec.party) {
    for (int i = 0; i < kPcSlots; ++i) order[static_cast<size_t>(i)] = i;
  } else {
    const int enemy_count = kUnits - kPcSlots, from = (last_enemy >= kPcSlots && last_enemy < kUnits ? last_enemy : kPcSlots) - kPcSlots;
    for (int i = 0; i < enemy_count; ++i) order[static_cast<size_t>(i)] = kPcSlots + (from + i) % enemy_count;
  }
  return order;
}

// The commands after Swap change no HP and take no time; they carry the menu state the rules read (the guard, the active PC, the element
// menu's tech and seal state, the Dopplegang pick) so that two games in step apply it at the same point of the command stream.
struct Command {
  // EndTurn: a versus side gives the turn to the other side (`actor` = any unit of that side). Focus: the actor's top command menu came up
  // (Battle::open_menu). ElementMenu: its element grid opened (Battle::open_element_menu). Form: the Dopplegang pick of the actor's
  // Dopplegang cast (Battle::transform with `form`).
  enum class Kind { Attack, Element, Defend, Run, Swap, EndTurn, Focus, ElementMenu, Form } kind = Kind::Defend;
  // PC slot 0..2 (versus: 0..5).
  int actor = 0;
  // Swap: the character id that takes the actor's slot (Battle::swap_candidates).
  int swap_in = -1;
  // Attack: strengths (0 weak, 1 medium, 2 heavy), max 4.
  std::vector<int> chain;
  // Element: grid cell 0..79.
  int cell = -1;
  // Unit index; -1 = automatic.
  int target = -1;
  // Form: the pool slot (monster record) to become, -1 = back to the character.
  int form = -1;
  // EndTurn: the turn ran out of time.
  bool timeout = false;
};

// A double / triple tech a PC's tech cell turns into (Battle::combo_at): the casting PCs in the order of the tech record's
// character bytes (+0x14..+0x16) and the grid cell each of them gives up, -1 = no entry.
struct ComboTech {
  int element = 0;
  std::array<int, 3> units{-1, -1, -1};
  std::array<int, 3> cells{-1, -1, -1};
  // The cell each party slot of the caster's side gives up (index = unit - side_first), -1 = the slot does not take part.
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
  // Op 67 type 6: the cast springs the element trap whatever the trap holds (it plays the colour's trap pack and lands nothing).
  bool trap_fizzle = false;
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
  // has_script_vars: a scene script runs, so these are its variables (otherwise the battle keeps the saved ones itself).
  virtual bool has_script_vars() const { return false; }
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
  // Any attack about to resolve (kind 0 weapon / 1 element; id = hit strength or element id; the units it aims at as a mask): scripts read
  // it as vars 0x20..0x36.
  virtual void on_attack(Battle&, int /*actor*/, int /*kind*/, int /*id*/, u32 /*target_mask*/) {}
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
  // Op 96: a PC's hit on an enemy deals 0 unless it is an element of this colour (1 green .. 6 blue); 0 = no gate.
  virtual int colour_gate() const { return 0; }
  // An element revive brought the unit back: its script entity runs again.
  virtual void on_revived(int /*unit*/) {}
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

// The brain of a versus battle: no battle script runs, so every hook keeps its default and no unit is ever handed to it.
class VersusBrain final : public EnemyBrain {
 public:
  EnemyAction choose(const Battle&, int, Rng&) override { return {EnemyAction::Kind::None}; }
};

// One side of a versus battle as both games build it: the party (at most 3, leader first), the summon stars left and the Dopplegang pool.
struct VersusSide {
  std::vector<PartyMember> party;
  int stars = 0;
  std::array<u16, 16> dopple{};
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
  // The save's variables 0x3D0..0x3DE, which are battle script vars 0x00..0x0E (0x04..0x0E: Pip's six colour points).
  std::array<u16, 8> saved_vars{};
  // Script variable 0x176: Van's piggy-bank savings in hundreds of G (element mode 46 adds it to the power).
  int piggy_bank = 0;
  // A monster-party scene (scene record byte 0x0F bit 0, the Grand Slam): party slot i fights as monster record monster_party[i] (the
  // Grand Slam picker's pool slots); -1 leaves the slot empty.
  std::array<int, kPcSlots> monster_party{-1, -1, -1};
  // The Lynx duel (scene 0x1DD): the character the third enemy becomes, -1 = none.
  int duel_third = -1;
  // The duel's names by character id, with Lynx's own name at id 0x31 (game::duel_names): the dummy enemies read the character they became,
  // a Pip form past id 0x2B nothing, and party member 0 reads Lynx. Empty: the record names stay.
  std::vector<std::string> duel_names;
  // The Lynx duel: the relief member, whose place in the list of the fight before the duel was Serge's; -1 = none.
  int duel_relief = -1;
  // Monster names in the game's language, by monster record. Empty or a blank entry: the table's name.
  std::vector<std::string> enemy_names;
  // Kid's name on every battle screen of scene 0x186, results included (data::unnamed_kid_label). Empty: her own name stays.
  std::string unnamed_kid_label;
  // Party swap (an OpenCross feature that mods turn on, off in the game as shipped): a PC's Swap command trades its slot with a bench
  // member. swap_reserve = the character ids that may come in (each must be in `bench`; the host applies the reserve rules).
  bool party_swap = false;
  std::vector<int> swap_reserve;
};

class Battle {
 public:
  // Script op AF.
  bool menu_locked() const { return brain_.menu_locked(); }
  Battle(const data::GameTables& tables, Rng& rng, EnemyBrain& brain) : tables_(tables), rng_(rng), brain_(brain) {}

  // Builds the 3 party units (party slot i = unit i) and the scene's enemies, then runs enemy turns / time until a PC can
  // act. Returns false if the scene id is invalid.
  bool start(std::vector<PartyMember> party, int scene, const BattleOptions& opt = {});

  // ---- versus battles
  // Starts a versus battle: side A's party in units 0..2, side B's in units 3..5 (every member at full HP without statuses), no scene and
  // no battle script (the brain must be a VersusBrain), the field ring of btbgdat row `battlefield` (none when the row does not exist),
  // side A's characters at `opening_stamina`, side B's at 70, and side A's turn. False when a side has no member or more than three.
  // Then the caller feeds every player's commands, in one order both games share, to apply(); Focus / ElementMenu / Form carry the menu
  // state, EndTurn (also on a timeout) passes the turn, and a side that cannot act any more passes it by itself.
  bool start_versus(const VersusSide& side_a, const VersusSide& side_b, int battlefield, int opening_stamina = kVersusOpeningStamina);
  bool versus() const { return versus_; }
  // The side whose turn it is (0 = A, 1 = B).
  int turn_side() const { return turn_side_; }
  // The side whose command comes next: the side that owes a Dopplegang pick (its cast may have ended its turn), else the turn side.
  int acting_side() const { return form_owed_ >= 0 ? side_of(form_owed_) : turn_side_; }
  // The unit whose Dopplegang cast waits for its pick, -1 = none.
  int form_owed() const { return form_owed_; }
  // Turns of that side ended by a timeout in a row.
  int timeouts(int side) const { return timeouts_[static_cast<size_t>(side & 1)]; }
  VersusEnd versus_end() const { return versus_end_; }
  // The side that won, -1 = a draw or still on.
  int winner() const { return versus_ && outcome_ == Outcome::Victory ? 0 : versus_ && outcome_ == Outcome::Wiped ? 1 : -1; }
  // A unit a player commands: a party slot, in a versus battle also side B's units 3..5.
  bool player_unit(int unit) const { return versus_ ? unit >= 0 && unit < kPlayerUnits : is_pc_slot(unit); }
  // Some character of the side could open its command menu, whoever has the turn (a done character of the turn side cannot).
  bool side_ready(int side) const;
  // The host ends the battle against that side (a player who stayed away); not a command, since only one game decides it.
  void concede(int side);
  // The summon stars a side has left (the normal battle's party: side 0, growth().counter).
  int star_counter(int side) const { return versus_ && side == 1 ? stars_b_ : growth_.counter; }
  // Everything the rules decide by (units, ring, traps, turn, counters, the generator's position ...), names and log text left out: two
  // games in step hold the same value after every command.
  u64 state_hash() const;

  // ---- views
  const Unit& unit(int index) const { return units_[static_cast<size_t>(index)]; }
  Unit& unit_mut(int index) { return units_[static_cast<size_t>(index)]; }
  const std::array<int, 3>& ring() const { return ring_; }
  // Dev: replaces the field ring (colour indices 0..5, kRingAllColours or -1 per slot).
  void set_ring(const std::array<int, 3>& ring) {
    ring_ = ring;
    judge_ring_summon_colour();
  }
  // Dev: arms one side's element trap with that element id (0 = none) without showing the trap block, as a write of the trap word alone does.
  void set_trap(bool enemy_side, int element) { trap_[enemy_side ? 1 : 0] = element; }
  // The element-trap block once the running action has ended: a trap set by it shows, a trap that sprang during it hides the whole block
  // (even while the other side's trap stays armed).
  TrapHud trap_hud() const { return {trap_state_ == 1 || trap_state_ == 2, trap_[0], trap_[1]}; }
  Outcome outcome() const { return outcome_; }
  // The Outcome value, or the script's code for Outcome::Scripted.
  int result_code() const { return result_code_; }
  const Rewards& rewards() const { return rewards_; }
  // The party records; versus side B's with side 1.
  const std::vector<PartyMember>& party(int side = 0) const { return versus_ && side == 1 ? party_b_ : party_; }
  const GrowthState& growth() const { return growth_; }
  // After a star-up: stats / snap / pending updated.
  const std::vector<PartyMember>& bench() const { return bench_; }
  int scene() const { return scene_; }
  // The party slots fight as monsters (BattleOptions::monster_party): nothing of the fight goes back to the party records.
  bool monster_party() const { return monster_party_; }
  // ---- party swap (BattleOptions::party_swap)
  // On for this fight: never in a monster-party scene or the Lynx duel.
  bool swap_enabled() const { return opt_.party_swap && !monster_party_ && scene_ != 0x1DD; }
  // The character ids that can take PC slot `pc`'s place now, in bench order: in the reserve or swapped out earlier in this battle, alive
  // (with the HP they left with when they fought earlier in this battle). Empty when the PC cannot act or is in a Dopplegang form. Any character may leave: the party lock
  // (FE 5D) is a rule of the field party, which the battle does not change (game::keep_swapped_lineup).
  std::vector<int> swap_candidates(int pc) const;
  // The battle state of a character that swapped out earlier in this battle and waits on the bench; null when it has not fought.
  const Unit* benched_unit(int char_id) const;
  // The characters that fought and are on the bench now: their records in bench() hold the HP, statuses and consumables to write back.
  const std::vector<int>& swapped_out() const { return swapped_out_; }
  // Swap commands applied in this battle.
  int swaps() const { return swaps_; }
  int actions() const { return actions_; }
  // Hidden / off-stage units (script ops 34 / 35 / 5B): never on the cursor ring, rejected by `validate`, drawn by nothing.
  bool hidden(int unit) const { return brain_.hidden(unit); }
  bool untargetable(int unit) const { return brain_.untargetable(unit); }
  // The units an all-target cast (pgdat +4 bit 0x10) may reach: seeded from the scene record (the enemies the scene keeps off-stage until a
  // script reveals them), ops 34 / 35 / 5B with arg 0 set a unit's bit; it is ANDed into the enemy-side masks (the party side is never masked).
  u32 cast_mask() const { return cast_mask_; }
  // ---- Dopplegang pool (the live mask is the save's; a versus battle keeps one per side)
  const std::array<u16, 16>& dopple_mask(int side = 0) const { return versus_ && side == 1 ? dopple_b_ : dopple_mask_; }
  bool dopple_has(int slot, int side = 0) const {
    return slot >= 0 && slot < 256 && (dopple_mask(side)[static_cast<size_t>(slot >> 4)] >> (slot & 15) & 1);
  }
  // The slots in the pick menu's order (ascending slot).
  std::vector<int> dopple_pool(int side = 0) const;
  // Script op 65 on an enemy unit (the start of its kill script): when the PC whose command came last (script var 0x50) is character 26
  // (Sprigg) or wears accessory 0xFC, and the enemy's monster record allows it, the pool gets the enemy's slot bit. True when a bit was set.
  bool dopple_credit(int enemy_unit, int last_pc);
  // Dopplegang's unit swap. form >= 0: PC unit `unit` takes monster record `form` (stats, innate colour, gear, status words, the monster's
  // element grid, its name); the character's own record is kept for the way back; HP stays, inflicted statuses and timers are gone, AP 8,
  // gauge -1. form < 0: the saved unit comes back (its stamina too) with the HP of now, countdown 8 and the max HP of the last unit to
  // change form from a character body (Pip's once he has changed form after it), which the party record keeps. False when nothing changed.
  bool transform(int unit, int form);
  // The unit a PC leaves the fight with (results screen, party record). A PC still in a creature form gets its own unit back: with the
  // HP of now while it was the last unit to change form from a character body and its creature form the last form change of the fight,
  // else the whole unit as it was before the form (its HP too).
  Unit exit_unit(int unit) const;
  // Pip's evolution: script op A7 sets the pending form element (0x80..0x85); while it is set every PC turn is the unit's script slot 2,
  // which casts it; its pgdat mode 45 applies the form: evolve().
  int evo_pending() const { return evo_pending_; }
  void set_evo_pending(int element) { evo_pending_ = element; }
  void evolve(int unit, int form);
  // Script vars 0x00..0x0E as they stand (BattleOptions::saved_vars at the start): the host writes them back to the save's variables
  // 0x3D0..0x3DE after the fight. Each element action (PC or enemy, a technique, item or summon alike) adds 1 to the colour point of its
  // record colour while Pip is in the party, and 1 more when Pip casts it; an action without a single colour adds nothing.
  std::array<u16, 8> saved_vars() const;
  // Element colour (pgdat +5) as the battle sees it: Pip's 18 techs take Pip's innate colour, Vary Innate its last caster's. A versus
  // battle keeps Pip's colour per side: `side` is the caster's.
  u8 element_colour(int id, int side = 0) const;
  // The colour an element cast enters in the colour history: 0..5 (an element without a colour byte enters as 0), 6 for the all-colour
  // element, -1 for none (record byte 0 bit 0x80: death casts, form changes). PC and enemy casts alike.
  int seq_colour(int element_id, int side = 0) const;
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
  // No PC gets its command menu while Pip's form element is pending: every PC turn is its script slot 2 then. In a versus battle only the
  // side whose turn it is acts, and a character that defended in this turn is done until its side's next turn.
  bool can_act(int pc) const {
    return outcome_ == Outcome::Ongoing && player_unit(pc) && unit(pc).can_open_menu() && evo_pending_ == 0 &&
           (!versus_ || (side_of(pc) == turn_side_ && !done_[static_cast<size_t>(pc)]));
  }
  // Versus: the character defended in its side's current turn.
  bool done(int unit) const { return versus_ && player_unit(unit) && done_[static_cast<size_t>(unit)]; }
  // Element cell usable now (gauge, flags, stamina, status).
  bool element_usable(int pc, int cell) const;
  // Colour-sealed cell: the PC's merged lo has the seal bit of the cell's colour (green 0x800, white 0x8000, black 0x80000, red 0x800000,
  // yellow 0x8000000, blue 0x80000000). Sealed cells stay on the menu, greyed, and refuse the confirm.
  bool element_sealed(int pc, int cell) const;
  // A PC summon (element record byte 0 bit 0x40) the ring forbids: its colour is not the ring's summon colour, or no star is left.
  bool summon_blocked(int pc, int cell) const;
  // The menu greys the cell: colour-sealed or a forbidden summon.
  bool element_greyed(int pc, int cell) const { return element_sealed(pc, cell) || summon_blocked(pc, cell); }
  // The ring's summon colour (0..5, kRingAllColours, -1 = none): the colour every non-empty slot shares, none when two differ; an all-empty
  // ring keeps the last one.
  int ring_summon_colour() const { return ring_summon_colour_; }
  // A double / triple tech the cell offers now (`ComboTech`, element 0 = none).
  ComboTech combo_at(int pc, int cell) const;
  // Builds the element menu of a PC (the host calls it when the grid opens; an Element command does it first): every PC's menu table starts
  // from its own cells, each double / triple tech the PC's cells offer is written into its cells and its partners' cells, and the PC's own
  // colour-seal flags are judged again by what its cells show now.
  void open_element_menu(int pc);
  // Judges the colour-seal flag of every cell of a unit (PC or enemy) by the element its cell shows (a double / triple tech's colour where
  // the last menu build put one there); rainbow cells keep their flag.
  void refresh_seal_flags(int pc);
  // The element the cell casts now: the double / triple tech combo_at offers, else the cell's own id.
  int cell_element(int pc, int cell) const;
  int chain_cost(int strength, const Unit& unit) const { return weapon_stamina_cost(strength, (unit.lo & st::kFatigued) != 0); }
  // The stamina one HIT% menu visit may spend, taken while the top menu runs: the stamina, plus 10 per step left on the timer of hi status
  // kHiNoStaminaCost while it waives the unit's costs (the stamina it will have once the status runs out).
  int hit_budget(int actor) const;
  // Hi status kHiNoStaminaCost waives the unit's weapon and element costs, except in a versus battle, where every action costs stamina.
  bool costs_waived(const Unit& unit) const { return !versus_ && (unit.hi & st::kHiNoStaminaCost) != 0; }
  bool validate(const Command& cmd, std::string* why = nullptr) const;
  // Percent, using the current chain counter (`any_target`: for every target of a sweep, not only the chain's own).
  int hit_chance_for(int actor, int target, int strength, bool any_target = false) const;
  // The HIT% box figure for the same attack (hit_chance_shown: the target's raw EVD, no hero, dizzy or sleeping-target terms).
  int hit_chance_shown_for(int actor, int target, int strength) const;
  // Auto accuracy rule (Accuracy Selection = Auto, and the auto battle): the strongest hit k = 2, 1, 0 whose HIT% figure is above 79 and
  // whose stamina need is met (30 / 20 / 10, fatigued 35 / 22 / 10); none -> -1, or weak when `fallback_weak` and it can be paid (the cursor
  // rests on row 0). The figure is hit_figure_fresh while the actor's last confirmed command was by another PC or at another target, else
  // the HIT% box figure (hit_chance_shown_for).
  int auto_hit_level(int actor, int target, bool fallback_weak = true) const;
  // The HIT% figure a command confirm writes for a fresh actor / target pair, before the box figures are computed: the strength's raw HIT
  // stat minus the target's EVD x EVD% / 100 (no weapon accuracy, Hit%, chain, hero, dizzy or sleep term; it may be negative).
  int hit_figure_fresh(int actor, int target, int strength) const;
  int alive_count(bool pcs) const;

  // ---- commands
  // The host opened the PC's command menu (the menu passed to it, an L1 / R1 switch, a re-entry): its guard ends and it becomes the active PC.
  void open_menu(int pc);
  // The active PC: the PC whose command menu ran last, or whose command or script action (op 67 of a PC's script) was queued last; op 2C
  // clears it (-1) and a battle starts with slot 0. Its darkness halves the chance of every weapon hit, an enemy's included, whatever the
  // attacker's own status.
  void set_active_pc(int pc) { active_pc_ = pc; }
  int active_pc() const { return active_pc_; }
  // False if invalid (nothing happens).
  bool apply(const Command& cmd, std::string* why = nullptr);

  // ---- primitives (public so tests and tools can drive single actions; they do not run the scheduler)
  void do_weapon_attack(int actor, int target, int strength);
  // A PC's heavy hit with a sweeping weapon (Equip::wp_sweeps), aimed at `aim`: one hit pass per enemy the cursor ring reaches.
  void do_weapon_sweep(int actor, int aim, int strength);
  // The hit a PC's command makes: the sweep for a heavy hit with a sweeping weapon, else the one-target attack.
  void do_pc_weapon_hit(int actor, int target, int strength);
  void note_summon_kill(int unit_index);
  // `push_ring` = false for the 2nd..nth target of an all-targets cast (the colour enters the field ring once per cast).
  void do_element(int actor, int element_id, int row, int target, bool push_ring = true);
  // Before every cast: an element the other side's trap holds is caught (it fizzles, the trap clears, one `caught` miss event). True when caught.
  bool catch_in_trap(int actor, int element_id, int row, int target);
  // The PCs other than `actor` and those in `excluded` (a mask of PC slots) recover stamina; the enemy countdowns run `steps`.
  void step_after_pc_action(int actor, int steps, bool weapon, u32 excluded = 0);
  // Ends an action. After a PC-led one (its own, confused or scripted turn, Defend) poison, regen and the status timers step.
  void post_action(int actor, int poison_amount, int timer_steps);
  void time_advance();
  // Enemy turns + time advance until a PC can act or the battle ends.
  void settle();
  void check_end();
  // Debug tools: after a primitive above was called from outside the scheduler (Debug tab cast): slot-4 reactions, then the end test.
  void finish_external_action();
  // Debug tools (Debug tab): damage never takes a unit of that side below 1 HP and no instant KO lands on it. Rules stay untouched when off.
  // Ignored in a versus battle, as the cheats are.
  void set_invincible(bool party, bool enemies) {
    if (!versus_) inv_party_ = party, inv_enemies_ = enemies;
  }
  bool invincible(int unit) const { return unit < kPcSlots ? (inv_party_ || cheats_.god_mode || cheats_.boost) : inv_enemies_; }
  // RDE's rule for the Radius tutorial (battle 386): its first enemy slot never dies. A hit's damage is cut to HP - 1 and a KO does not
  // stay (the tutorial's own idle script rewrites that unit's HP every tick).
  static constexpr int kUndyingScene = 386, kUndyingUnit = 3;
  bool undying(int unit) const { return scene_ == kUndyingScene && unit == kUndyingUnit; }
  // Cheats tab: the rules' bypasses, all off by default; the host pushes the live set every update.
  struct Cheats {
    bool god_mode = false, boost = false, max_stamina = false, free_elements = false, always_hit = false, always_loot = false, instant_growth = false, shared_growth = false;
  };
  void set_cheats(const Cheats& cheats) {
    if (!versus_) cheats_ = cheats;
  }
  const Cheats& cheats() const { return cheats_; }

  // Dev casts (no script queued them): the caster group the next events carry.
  void set_group_hint(const std::array<u8, 6>& group) { cur_group_ = group; }
  // The running / last begun action (ActionEvent::action of its events).
  int action_id() const { return action_id_; }
  // A killed enemy whose kill script (slot 3) queued a death cast that has not been executed yet (the unit still owes its death presentation).
  bool death_pending(int unit) const { return unit >= 0 && unit < kUnits && death_pending_[static_cast<size_t>(unit)]; }
  // Called when an attacked script (slot 4) ends, before the other scripts' idle loops run again: every enemy in the turn order whose
  // countdown is 0 has its turn taken (countdown 0xFFFF). A countdown written after that (a reaction cutscene's) does not cancel the turn.
  void claim_ready_turns();

 private:
  void say(std::string line) { log_.push_back(std::move(line)); }
  // The state start() and start_versus() both begin from.
  void reset_battle_state();
  // The party record a PC unit came from (versus: side B's for units 3..5); null for an enemy or an empty slot.
  PartyMember* record_of(int unit);
  int& stars_left(int side) { return versus_ && side == 1 ? stars_b_ : growth_.counter; }
  // A state command (EndTurn, Focus, ElementMenu, Form): no time passes, no action is counted.
  void apply_state_command(const Command& cmd);
  void finish_versus(Outcome outcome, VersusEnd end);
  // The turn goes to the other side; a turn ended any way but a timeout clears its side's timeout run.
  void pass_turn(bool timeout);
  // A side character waits for nothing but stamina (alive, no status that keeps its menu shut, a recovery rate): time can give it the menu.
  bool time_helps(int side) const;
  // Versus time: the step is the smallest countdown above 0 among the units of the other side (of every side for -1), 255 when there is
  // none; every unit in the turn order counts it down and the characters of `side` (every character for -1) recover step x rate.
  void versus_time_advance(int side);
  // The scheduler of a versus battle (settle).
  void settle_versus();
  // The turn of a unit the scheduler found ready (an enemy, a confused PC, a PC's script slot 2), then the reactions.
  void run_scheduled_turn(int actor);
  void begin_action() {
    ++action_id_;
    hit_counter_ = 0;
    trap_state_ = trap_hud().shown ? 1 : 0;
    trap_before_ = trap_hud();
  }
  void reset_chain() { chain_actor_ = chain_target_ = -1; chain_hits_ = 0; }
  // The start of a weapon attack aimed at `aim` (thaw, the script's attack vars with `target_mask`, a new chain for a new target).
  void weapon_begin(int actor, int aim, int strength, u32 target_mask);
  // One target's pass of a weapon attack: the hit roll, then the damage and its event. `aim` >= 0: a pass of a sweep aimed there;
  // `missed`: an earlier target of the same sweep missed, so this one misses after its roll. Returns whether the hit landed.
  bool weapon_strike(int actor, int target, int strength, int aim, bool missed);
  // A landed attack (every target hit): the chain step and the level gauge.
  void weapon_landed(int actor, int strength);
  // A PC-led weapon / element hit adds its target to the attacked list, kept in ascending unit order; enemy-led actions add nothing.
  void note_event(ActionEvent event) {
    if ((event.kind == ActionEvent::Kind::Weapon || event.kind == ActionEvent::Kind::Element) && is_pc_slot(event.actor) && event.target >= 0) {
      const auto at = std::lower_bound(attacked_.begin(), attacked_.end(), event.target);
      if (at == attacked_.end() || *at != event.target) attacked_.insert(at, event.target);
    }
    event.action = action_id_;
    event.script_seq = cur_script_seq_;
    event.group = cur_group_;
    event.hit_index = hit_counter_++;
    event.ring = ring_;
    event.trap = {trap_state_ == 1 || trap_state_ == 3 || trap_state_ == 4, trap_[0], trap_[1]};
    event.trap_before = trap_before_;
    if ((event.kind == ActionEvent::Kind::Weapon || event.kind == ActionEvent::Kind::Element) && event.target >= 0 && event.target < kUnits)
      units_[static_cast<size_t>(event.target)].result_code = event.code;
    events_.push_back(event);
  }
  WeaponIn weapon_in(const Unit& attacker, const Unit& target_unit, int mult, int strength) const;
  // The element's colour is sealed on the PC.
  bool colour_sealed(int pc, int id) const;
  void judge_ring_summon_colour();
  // The cell of `pc` that can join a double / triple tech with partner tech `tech`, -1 = none. A PC's seal flag, not its live status, decides.
  int combo_partner_cell(int pc, int tech) const;
  void load_monster_record(Unit& unit, const data::StatRec& rec, u32 perm_lo_bits) const;
  // `side`: whose stars left count.
  void scale_creature_form(Unit& unit, const data::StatRec& rec, int side) const;
  void end_chain_after(size_t first_event);
  // Single target, the whole side (element record byte 4 bit 0x10), or the units of `targets` (EnemyAction::targets) when it names more
  // than one: one cast in every case, so the field ring takes its colour once.
  void cast_element(int actor, int element_id, int row, int target, u32 targets = 0);
  // Script var 0x00..0x0E number `index` (saved_vars): the scene script's variable while one runs.
  int saved_var(int index) const;
  void set_saved_var(int index, int value);
  void count_colour_point(int actor, const data::ElemRec& element);
  void damage_unit(int actor, int target, int value, int code);
  // Op 96's gate holds for this hit (element_id -1 = a weapon hit).
  bool colour_gated(int actor, int target, int element_id) const;
  void kill(int unit);
  void poison_kill(int unit);
  // Runs the kill script (slot 3) of a dead enemy once and parks the death cast it queued.
  void run_kill_script(int unit);
  // Every enemy whose HP is 0 or that is KO'd without its kill script having run (a script wrote its HP to 0) gets that script now.
  void scan_kills();
  // True when any roll landed. `side`: the caster's (the colour the element takes).
  bool apply_status(int target, const data::ElemRec& rec, int side);
  bool apply_status_bits(int target, u32 lo, u32 hi, int chance, u8 colour_bit = 0);
  bool apply_status_bits(int target, u32 mask, int set, int chance) { return apply_status_bits(target, set == 0 ? mask : 0, set == 0 ? 0 : mask, chance); }
  // Per bit, no roll.
  bool force_status_bits(int target, u32 lo);
  void set_status_bit(int target, int set, int bit);
  // The unit's action is about to run: a pending freeze ends (lo 0x60000000 and their timers cleared).
  void thaw_on_action(int unit);
  void enemy_turn(int unit);
  // The queued death casts of killed units (slot 3), run by run_reactions.
  void run_death_plans();
  // The casts the scripts' idle loops queued, run by run_death_plans.
  void run_idle_plans();
  // Slot 4 of every unit of the last action's target list, queued counter-attacks executed.
  void run_reactions();
  void confused_turn(int unit);
  // A cast swapped to the forced miss: the colour 1..6 (green .. blue) the caster is sealed in when the element can be sealed (id 0x12D at
  // most, record byte 1 bit 0); 0 = none.
  int seal_swap_colour(int actor, int element_id) const;
  // The three party countdowns run `steps` (PCs in the turn order, floor 0).
  void lower_pc_countdowns(int steps);
  // The stamina an action costs the unit (none while its costs are waived).
  int stamina_cost(const Unit& unit, int cost) const { return costs_waived(unit) ? 0 : cost; }
  // Heals the unit the running action owes a drain heal (drain_).
  void pay_drain();
  // Returns {poison step, timer steps}.
  std::pair<int, int> exec_action(int unit, const EnemyAction& action);
  void run_enemy_action(int unit, const EnemyAction& action);
  void end_scripted_action(int unit, const EnemyAction& action);
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
  // What a dummy enemy of the Lynx duel becomes: its kind, weapon, gender word and colour (-1 = its own).
  struct DuelBody {
    int kind;
    u16 weapon;
    u16 gender;
    int innate;
  };
  std::optional<DuelBody> duel_body(int slot, int monster_id, int third) const;
  std::string enemy_label(int monster_id) const;
  void init_enemy(int slot, int monster_id, const BattleOptions& opt);
  void init_party_monster(int slot, int monster_id, const BattleOptions& opt);
  void unlock_monster_grid(Unit& unit) const;
  int next_ready_unit();
  void god_filter(int target, DamageOut& dmg) const;
  // Max stamina / God mode / battle boost refills, run after every action and hit.
  void apply_cheat_state();
  // Battle boost (RDE): a PC's pending damage is cancelled (the result reads as a miss) and every resolved hit or miss strips the negative
  // statuses of the PCs still standing.
  bool boost_shields(int unit) const { return cheats_.boost && is_pc_slot(unit); }
  void boost_cure();

  const data::GameTables& tables_;
  Rng& rng_;
  EnemyBrain& brain_;
  std::array<Unit, kUnits> units_{};
  std::array<int, 3> ring_{-1, -1, -1};
  // duel_roster_: the whole party of the Lynx duel, of which only the leader stays in party_.
  std::vector<PartyMember> party_, bench_, duel_roster_;
  std::vector<std::string> log_;
  std::vector<ActionEvent> events_;
  std::map<std::string, int> stubs_;
  // Target list of the running PC-led action, ascending (slot-4 launches).
  std::vector<int> attacked_;
  // Slot-3 plans of units killed during the running action (kill()), executed once it has ended (run_reactions): unit, queued attacks.
  std::vector<std::pair<int, std::vector<EnemyAction>>> death_plans_;
  // Casts queued by idle loops, not yet executed: unit, queued attacks.
  std::vector<std::pair<int, std::vector<EnemyAction>>> idle_plans_;
  // run_death_plans is executing a plan (ActionEvent::death_cast).
  bool in_death_cast_ = false;
  // Units whose damage battle boost cancelled during the running hit (bit = unit).
  u32 boost_cancelled_ = 0;
  // A kill script queued attacks that have not run yet: the unit still owes its death (check_end).
  std::array<bool, kUnits> death_pending_{};
  // Turns taken by claim_ready_turns that have not run yet.
  std::array<bool, kUnits> turn_claimed_{};
  // The target whose results op 96's gate cancels while the element mode runs (-1 = none).
  int gated_target_ = -1;
  // The unit's kill script has run since it last had HP.
  std::array<bool, kUnits> kill_script_run_{};
  // By colour index: the enemies (one bit per unit) killed during a summon of that colour. summon_colour_ = the current action's, -1 = not a summon.
  std::array<u16, 6> summon_kills_{};
  int summon_colour_ = -1;
  int ring_summon_colour_ = -1;
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
  std::array<Unit, kPlayerUnits> form_backup_{};
  // Per side (a normal battle uses side 0). A Dopplegang form change into a creature sets form_restore_; the way back and Pip's form change
  // clear it. The max HP and the slot of the last unit that changed form from a character body (a Dopplegang form from the own form, Pip's
  // form change); the way back and a creature form give that max HP to the unit.
  std::array<bool, 2> form_restore_{};
  std::array<int, 2> form_max_hp_{};
  std::array<int, 2> form_body_unit_{-1, -1};
  // Party swap: the battle state of each character that left the field (statuses and timers included), by character id, restored when it
  // comes back; swapped_out_ = those of them still on the bench.
  std::map<int, Unit> benched_units_;
  std::vector<int> swapped_out_;
  int swaps_ = 0;
  // Big Catch (mode 39) per-cast state: 0 not rolled, 1 the roll failed, 2 a creature was picked (reset at the end of a cast); the creature's
  // battle model number (0 = the value the command cleared it to) and pool slot.
  int creature_state_ = 0, creature_model_ = 0, creature_slot_ = 0;
  // Pip's pending form element (cleared at battle init and by mode 45).
  int evo_pending_ = 0;
  // The running cast's seal swap colour (seal_swap_colour), -1 = not judged yet (a lone do_element judges it itself).
  int seal_swap_ = -1;
  // The drain heal the running action owes (hi kHiDrain): {unit, value} of the last target it damaged, unit -1 = none.
  std::pair<int, int> drain_{-1, 0};
  // Script vars 0x00..0x0E while no scene script holds them (saved_vars).
  std::array<u16, 8> saved_vars_{};
  // The colour kPipTechs take (0 = no Pip in the party); per side in a versus battle, else entry 0.
  std::array<u8, 2> pip_tech_colour_{};
  // See cast_mask().
  u32 cast_mask_ = 0x1FF;
  int chain_actor_ = -1, chain_target_ = -1, chain_hits_ = 0;
  // The PC whose weapon turn is open (end_chain_after), -1 = none.
  int chain_owner_ = -1;
  // See set_active_pc().
  int active_pc_ = 0;
  // The PC of the last confirmed Attack or Element command, and each PC's target of its last one (-1 = none yet): auto_hit_level's
  // fresh pair test.
  int confirm_actor_ = -1;
  std::array<int, kPlayerUnits> confirm_target_{-1, -1, -1, -1, -1, -1};
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
  // The next cast is a script's forced trap fizzle (EnemyAction::trap_fizzle).
  bool trap_fizzle_ = false;
  // ---- versus
  bool versus_ = false;
  // Side B's party records, summon stars and Dopplegang pool (side A's are party_, growth_.counter and dopple_mask_).
  std::vector<PartyMember> party_b_;
  int stars_b_ = 0;
  std::array<u16, 16> dopple_b_{};
  int turn_side_ = 0;
  // The turn side has not acted yet in this turn.
  bool turn_fresh_ = true;
  // The turn side's characters that defended in this turn; cleared when their side's turn starts.
  std::array<bool, kPlayerUnits> done_{};
  std::array<int, 2> timeouts_{};
  VersusEnd versus_end_ = VersusEnd::None;
  // The unit whose Dopplegang cast waits for its pick (Command::Kind::Form), -1 = none.
  int form_owed_ = -1;
};

// Simple deterministic player policy for `ocbattle sim`: heal when someone is below 40%, else the strongest
// usable damage element, else a weak->medium->heavy chain that fits the stamina budget.
Command auto_command(const Battle& battle, int pc);

}  // namespace oc::battle
