#pragma once
// The retail battle formulas (hit chance, damage, heal, statuses, stamina, growth, loot) as pure functions. Integer arithmetic is C int with truncating division;
// every random term goes through the injected Rng so a test can script the rolls.
#include <array>
#include <vector>

#include "battle/rng.h"

namespace oc::data {
struct GameTables;
}

namespace oc::battle {

// ---- status words (merged lo / hi words of a unit)
namespace st {
constexpr u32 kPoison = 0x1, kFlu = 0x20, kKO = 0x80, kAfraid = 0x100, kFatigued = 0x2000, kAsleep = 0x4000,
              kDark = 0x20000, kStaminaDrain = 0x40000, kConfused = 0x100000, kDizzy = 0x2000000,
              kDefeated = 0x20000000, kInstantDeath = 0x80000000;
// Absorb / immune come in a weapon pair (0x2000 / 0x8000: Negate Physical) and an element pair (0x4000 / 0x10000: Negate Magic).
constexpr u32 kHiAlwaysHit = 0x1000, kHiWeaponAbsorb = 0x2000, kHiAbsorb = 0x4000, kHiWeaponImmune = 0x8000, kHiImmune = 0x10000, kHiRegen = 0x80000;
// Lo bits kept after a battle.
constexpr u32 kPersist = 0x39;
// Inflicted bits a PC keeps when a hit knocks it out; a poison KO keeps the inflicted lo bits 1..15 and the whole hi word.
constexpr u32 kKoKeepLo = 0x400480u, kKoKeepHi = 0x1FE00000u, kPoisonKoKeepLo = 0xFFFEu;
// Bits whose infliction starts a timer.
constexpr u32 kTimedLo = 0xBBBFFB00u;
constexpr u32 kTimedHi = 0xE003FFFFu;
// Element hi-status bits (element record byte 0x1C) that may be applied.
constexpr u32 kHiApplyMask = 0xE007FFFFu;
// Lo bits that keep a PC's command menu closed.
constexpr u32 kCannotMenuLo = 0x20304080u;
}  // namespace st

// ---- helpers
// value * pct / 100, rounded up.
inline int ceil100(int value, int pct) {
  const int product = value * pct;
  int quotient = product / 100;
  if (product % 100) ++quotient;
  return quotient;
}
// Sum(count, power, variance) = sum_{i=0..count} (power*100 - power*rnd(0,variance)): count+1 terms, one roll each.
int sum_rolls(Rng& rng, int count, int power, int variance);

// ---- colours and the field ring
constexpr int kNoColour = 6;
// Opposite of each colour: green<->yellow, white<->black, red<->blue.
constexpr std::array<int, 6> kOpp = {4, 2, 1, 5, 0, 3};
constexpr std::array<int, 10> kColourMul = {665, 750, 875, 1000, 1125, 1250, 1375, 1500, 1500, 1500};
// Colour multiplier (per mille) for one attack. `innate` = attacker innate colour (0..5 or kNoColour), `attack` = colour of
// the attack (weapon: attacker's colour, element: pgdat colour), `ring` the three field-colour slots (0..5, kRingAllColours or -1),
// `target` = target colour (0..5 or -1).
int colour_multiplier(int innate, int attack, const std::array<int, 3>& ring, int target);
// A ring slot holding every colour at once (the Chrono Cross): drawn as the rainbow cell, counted as no colour by the multiplier.
constexpr int kRingAllColours = 6;
// The ring slot an element colour byte enters as: one colour bit -> 0..5, all six bits (0xFC) -> kRingAllColours, else -1 (none).
int ring_colour_of(u8 colour_byte);
// Field-colour FIFO: the new colour enters, the oldest leaves. `colour` -1 = none.
void ring_push(std::array<int, 3>& ring, int colour);
int ring_count(const std::array<int, 3>& ring, int colour);

// ---- physical hit chance
constexpr std::array<int, 3> kChainTarget = {102, 102, 103};
constexpr std::array<int, 3> kChainPct = {30, 25, 20};
constexpr std::array<int, 3> kChainAdd = {1, 3, 7};
inline int hit_base(int stat, int wp_acc) { return stat / 3 + (wp_acc > 99 ? 99 : wp_acc) * 200 / 300; }
struct ChanceIn {
  // hit_base of the strength's stat; attacker Hit%; target EVD total; target Evd%.
  int base = 0;
  int hit_mod = 100;
  int evd = 0;
  int evd_mod = 100;
  // The attacker wears the hero set: the target's evade x125/100.
  bool attacker_hero_set = false;
  bool target_dizzy = false;
  // The chain bonus is PC-only.
  bool pc_attacker = true;
  // Chain counter.
  int hits = 0;
  int strength = 0;
  // Attacker has st::kHiAlwaysHit / st::kDark.
  bool always_hit = false;
  bool dark = false;
  bool target_asleep = false;
};
// Percent, 0..100.
int hit_chance(const ChanceIn& in);
// Miss iff chance*10 < rnd(0,1000).
bool roll_hit(Rng& rng, int chance);

// ---- damage
struct DamageOut {
  int value = 0;
  // 0 normal, 1 "no damage", 2 miss/immune, 3 heal/absorb.
  int code = 0;
  bool crit = false;
};
constexpr std::array<int, 16> kCritBonus = {50, 100, 80, 100, 50, 100, 100, 100, 100, 100, 100, 80, 100, 100, 100, 100};
constexpr std::array<int, 3> kPcStrMul = {10, 25, 40};
constexpr std::array<int, 3> kEnemyStrMul = {10, 12, 15};

struct WeaponIn {
  // wp is the attacker's weapon term (weapon through Att%, doubled by the hero set); arm is already ceil100(armour * Def%).
  int atk = 0, wp = 0, wp_var = 0;
  int def = 0, arm = 4, arm_var = 0;
  // Target lo & 0x4008: arm x300/400.
  bool arm_soft = false;
  // kPcStrMul / kEnemyStrMul by strength; for a weapon tech, the element record's power.
  int mult = 10;
  int colour_mille = 1125;
  // 0 weak, 1 medium, 2 heavy.
  int strength = 0;
  int weapon_class = 0;
  // The attacker wears the hero set: the crit roll needs 7 instead of 15.
  bool hero_set = false;
  bool target_afraid = false, slayer = false;
  // Attack colour == kOpp[target colour].
  bool opposite_colour = false;
  bool guarding = false;
  // x125/100 attacker gear, x75/100 target gear.
  bool gear_up = false, gear_down = false;
  // The target's gear guard mask meets the attacker's hi word: raw x3/4.
  bool resisted = false;
};
DamageOut weapon_damage(Rng& rng, const WeaponIn& in);

struct ElementIn {
  // mag_term = ceil100((1 + gear) * MAtt%); power and var come from the element record (bytes 8 and 0xC);
  // mdef_term = ceil100((1 + gear) * MDef%) with the target's variance mdef_var.
  int mag = 0, mag_term = 1;
  int power = 0, var = 0;
  int mdef = 0, mdef_term = 5, mdef_var = 0;
  // Target lo & 8: mdef term x300/400.
  bool mdef_soft = false;
  int slot_mille = 1000;
  int atk_matt_pct = 100, tgt_mdef_pct = 100;
  int colour_mille = 1000;
  // Colour index of the element.
  int elem_colour = kNoColour;
  // Target hi & 0x3F shield colour (-1 none).
  int shield_colour = -1;
  int target_colour = -1;
  // The element colour the target's armour colour byte raises x150/100 (-1 none).
  int armour_boost_colour = -1;
  // Weakness mask hit: the variance roll becomes 50.
  bool exploit = false;
  bool opposite_colour = false;
  bool gear_up = false, gear_down = false;
  // Target has st::kHiAbsorb / st::kHiImmune.
  bool target_absorb = false;
  bool target_immune = false;
  // Raw-damage source: 0 = MAG vs MDEF (modes 0 / 15 / 37); 32 = ATK vs DEF (ATK + 2 terms of power against DEF + 2 terms of
  // the target armour term, / 400000 by the slot, floor (wp_term - arm_term) / 4, negative -> 1, no MAtt% step); 33 = `fixed` taken as the
  // post-colour value with a variance roll of 0 and no MDef% / shield / colour-absorb stage.
  int source = 0;
  int atk = 0, wp_term = 0, def = 0, arm_term = 0, arm_var = 0;
  // Target lo & 0x4008: arm_term x300/400.
  bool arm_soft = false;
  int fixed = 0;
};
DamageOut element_damage(Rng& rng, const ElementIn& in);

// Heal (modes 3, 7, 14): max(1, sum_{i=0..mag+1} (power*100 - rnd(0,power)) / 400) through the slot multiplier, capped at 999. A heal never
// takes the colour multiplier or the field state. `power` = pgdat +0xA.
int heal_amount(Rng& rng, int mag, int power, int slot_mille);
// Slot modifier per mille: 1000 +/- 125*n, n = |row-(level-1)| (0 if the element has no level), + when placed above.
int slot_mille(int row, int level);

// ---- statuses
// Chance doubled when weak; lands on rnd(1,99) <= chance.
bool status_roll(Rng& rng, int chance, bool weak);
// 3, except idx 19, 38, 39 -> 1 (idx = bit + 32*set).
int status_dur_mult(int idx);
// DUR * (dur_stat + dur_mod).
int status_timer(int idx, int dur_stat, int dur_mod);
inline bool status_is_timed(int set, int bit) {
  return ((set == 0 ? st::kTimedLo : st::kTimedHi) >> bit & 1u) != 0;
}

// ---- stamina
inline int cap_stamina(int stamina) { return stamina > 70 ? 70 : stamina; }
// (strength + 1) * 10, x500/400 when fatigued.
int weapon_stamina_cost(int strength, bool fatigued);
// (rate + mod) * steps, x300/400 with flu.
int recovery_step(int rate, int mod, int steps, bool flu);
constexpr int kElementCost = 70;

// ---- growth (star levels)
// One exdat star row {n, b, s}: `battles` fills the star in `step`s, `pending_tenths` = the share of the pending delta the minor growth aims at.
struct StarRow {
  int battles = 8, pending_tenths = 7, step = 1;
};
// Next pending delta over `range`: round(rnd(1,999) / 1000 * range * 2 / (101 - star)), computed in integers.
int growth_roll(Rng& rng, int range, int star);
int next_pending_hp(Rng& rng, int max_hp, int lv99_hp, int star);
int next_pending_stat(Rng& rng, int cur, int lv99, int star);
// Minor post-battle gain for one stat: round_half_up(0.5*((snap+d)-cur)*sqrt(P/S)), d = pending*pending_tenths/10, floor 0.
int minor_growth(int snap, int pending, int cur, int pending_tenths, int progress, int progress_cap);
// The whole way to the minor-growth target in one step: round_half_up((snap+d)-cur), floor 0 (the instant-growth cheat).
int full_minor_growth(int snap, int pending, int cur, int pending_tenths);
inline int progress_cap(const StarRow& row) { return static_cast<short>(row.step * row.battles); }

// ---- victory
// x125/100 with the gear bonus.
int gold_reward(int base_total, bool bonus);
// Drop roll for one defeated enemy: returns the chosen row slot index (1 rare, 0 common) or -1.
int drop_roll(Rng& rng, int drop_base);

// ---- encounters
// Enemy-group variant of an encounter: one rnd(0, 2549); band i ({lo, hi}: the rolls in [lo, hi)) gives variant 7 - i, no band 0.
int encounter_variant(Rng& rng, const std::vector<std::array<u16, 2>>& bands);
// Scene of a field battle: an encounter id (< 0x200, field op A3) rolls its variant into symtbl; a fixed scene (id >= 0x200, op C9)
// draws nothing.
int encounter_scene(Rng& rng, const data::GameTables& tables, int battle_id);

}  // namespace oc::battle
