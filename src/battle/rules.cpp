#include "battle/rules.h"

#include "data/tables.h"

#include <algorithm>
#include <cmath>

namespace oc::battle {

int sum_rolls(Rng& rng, int count, int power, int variance) {
  int total = 0;
  for (int i = 0; i <= count; ++i) total += power * 100 - power * rng.rnd(0, variance);
  return total;
}

// ------------------------------------------------------------------------------------------------ colours
// Colour points: 1 for the attacker's innate colour, 3 for the attack's colour; opposite pairs cancel; each field-ring
// colour adds to its own points when the opposite has none, else eats the opposite's; the target's colour subtracts
// 1 + its ring count. The attack colour's points index kColourMul (negative -> 500 per mille).
int colour_multiplier(int innate, int attack, const std::array<int, 3>& ring, int target) {
  int points[7] = {0, 0, 0, 0, 0, 0, 0};
  if (innate >= 0 && innate <= kNoColour) points[innate] = 1;
  if (attack < 0 || attack > kNoColour) attack = kNoColour;
  points[attack] += 3;
  for (int i = 0; i < 6; ++i) {
    const int opposite = kOpp[static_cast<size_t>(i)];
    if (i < opposite) {
      const int cancelled = std::min(points[i], points[opposite]);
      points[i] -= cancelled;
      points[opposite] -= cancelled;
    }
  }
  int field[6] = {0, 0, 0, 0, 0, 0};
  for (int colour : ring)
    if (colour >= 0 && colour < 6) ++field[colour];
  for (int i = 0; i < 6; ++i) {
    if (field[i] <= 0) continue;
    const int opposite = kOpp[static_cast<size_t>(i)];
    if (points[opposite] == 0 && points[i] != 0) points[i] += field[i];
    else points[opposite] -= std::min(field[i], points[opposite]);
  }
  if (target >= 0 && target < 6) points[target] -= 1 + field[target];
  int idx = points[attack];
  if (idx < 0) return 500;
  if (idx > 9) idx = 9;
  return kColourMul[static_cast<size_t>(idx)];
}

int ring_colour_of(u8 colour_byte) {
  const u8 mask = static_cast<u8>(colour_byte & 0xFC);
  return mask == 0xFC ? kRingAllColours : data::colour_index_of_bit(mask);
}

void ring_push(std::array<int, 3>& ring, int colour) {
  ring[0] = ring[1];
  ring[1] = ring[2];
  ring[2] = colour;
}

int ring_count(const std::array<int, 3>& ring, int colour) {
  int count = 0;
  for (int slot : ring)
    if (slot == colour) ++count;
  return count;
}

// ------------------------------------------------------------------------------------------------ hit chance
// base * Hit% - evade (halved vs a dizzy target), floor 0; a PC's chain counter pulls the chance toward kChainTarget by
// kChainPct per hit; cap 99, then the always-hit / dark / sleeping-target overrides.
int hit_chance(const ChanceIn& in) {
  int evade = ceil100(in.evd, in.evd_mod);
  if (in.attacker_hero_set) evade = evade * 125 / 100;
  if (in.target_dizzy) evade /= 2;
  int chance = in.base * in.hit_mod / 100 - evade;
  if (chance < 0) chance = 0;
  const size_t strength = static_cast<size_t>(std::clamp(in.strength, 0, 2));
  if (in.pc_attacker) {
    for (int i = 0; i < in.hits; ++i) {
      chance += (kChainTarget[strength] - chance) * kChainPct[strength] / 100;
      if (chance >= 99) {
        chance = 99;
        break;
      }
    }
  }
  if (chance > 99) chance = 99;
  if (in.always_hit) chance = 99;
  if (in.dark) chance /= 2;
  if (in.target_asleep) chance = 100;
  return chance;
}

bool roll_hit(Rng& rng, int chance) { return !(chance * 10 < rng.rnd(0, 1000)); }

// ------------------------------------------------------------------------------------------------ damage
// raw = (Sum(ATK+1, wp, wpVar) - Sum(DEF+1, arm, armVar)) * mult / 20000, floor (wp - arm) / 2, negative -> rnd(0,3),
// x3/4 when resisted, then the colour multiplier; the crit roll rnd(afraid ? 5 : 0, heavy ? 15 : 14) >= 15 (7 with the hero set; or the slayer
// mask) adds the weapon class' kCritBonus percent; finally the opposite-colour x3/2, guard /2 and gear x125 / x75 steps.
DamageOut weapon_damage(Rng& rng, const WeaponIn& in) {
  DamageOut out;
  const int arm = in.arm_soft ? in.arm * 300 / 400 : in.arm;
  const int attack_sum = sum_rolls(rng, in.atk + 1, in.wp, in.wp_var);
  const int defence_sum = sum_rolls(rng, in.def + 1, arm, in.arm_var);
  int raw = static_cast<int>(static_cast<long long>(attack_sum - defence_sum) * in.mult / 20000);
  raw = std::max(raw, (in.wp - arm) / 2);
  if (raw < 0) raw = rng.rnd(0, 3);
  if (in.resisted) raw = raw * 3 / 4;
  const int coloured = raw * in.colour_mille / 1000;

  const int lo = in.target_afraid ? 5 : 0;
  const int hi = in.strength == 2 ? 15 : 14;
  const int crit_roll = rng.rnd(lo, hi);
  out.crit = crit_roll >= (in.hero_set ? 7 : 15) || in.slayer;
  const int bonus = out.crit ? kCritBonus[static_cast<size_t>(in.weapon_class & 15)] : 0;
  int dmg = coloured * (100 + bonus) / 100;
  if (in.opposite_colour) dmg = dmg * 300 / 200;
  if (in.guarding) dmg /= 2;
  if (in.gear_up) dmg = dmg * 125 / 100;
  if (in.gear_down) dmg = dmg * 75 / 100;
  out.value = dmg;
  out.code = dmg == 0 ? 1 : 0;
  return out;
}

// Raw damage per `source` (see ElementIn), then the colour multiplier, the variance roll rnd(0,5) percent (50 on a
// weakness exploit), the target's MDef% / shield / innate-absorb steps, and the opposite-colour and gear steps.
DamageOut element_damage(Rng& rng, const ElementIn& in) {
  DamageOut out;
  const int elem_colour = in.elem_colour;
  int effect = 0, variance_roll = 0;
  if (in.source == 33) {
    effect = in.fixed;
  } else {
    int raw = 0;
    if (in.source == 32) {
      const int arm = in.arm_soft ? in.arm_term * 300 / 400 : in.arm_term;
      const int attack_sum = sum_rolls(rng, in.atk + 1, in.power, in.var);
      const int defence_sum = sum_rolls(rng, in.def + 1, arm, in.arm_var);
      raw = static_cast<int>(static_cast<long long>(attack_sum - defence_sum) * in.slot_mille / 400000);
      raw = std::max(raw, (in.wp_term - arm) / 4);
      if (raw < 0) raw = 1;
    } else {
      const int mdef_term = in.mdef_soft ? in.mdef_term * 300 / 400 : in.mdef_term;
      const int attack_sum = sum_rolls(rng, in.mag + 1 + in.mag_term, in.power, in.var);
      const int defence_sum = sum_rolls(rng, in.mdef + 1, mdef_term, in.mdef_var);
      raw = static_cast<int>(static_cast<long long>(attack_sum - defence_sum) * in.slot_mille / 400000);
      raw = std::max(raw, (in.power - mdef_term) / 4);
      if (raw < 0) raw = rng.rnd(0, 3);
      if (in.atk_matt_pct > 100) raw = raw * 120 / 100;
      else if (in.atk_matt_pct < 100) raw = raw * 80 / 100;
    }
    effect = raw * in.colour_mille / 1000;
    variance_roll = rng.rnd(0, 5);
    if (in.exploit) variance_roll = 50;
    if (in.tgt_mdef_pct < 100) effect = effect * 120 / 100;
    else if (in.tgt_mdef_pct > 100) effect = effect * 80 / 100;
    if (in.shield_colour >= 0 && elem_colour >= 0 && elem_colour < 6) {
      if (in.shield_colour == kOpp[static_cast<size_t>(elem_colour)]) effect = effect * 120 / 100;
      else if (in.shield_colour == elem_colour) effect = effect * 80 / 100;
    }
    if (in.armour_boost_colour >= 0 && elem_colour == in.armour_boost_colour) effect = effect * 150 / 100;
  }
  int dmg = std::max(0, effect * (100 + variance_roll) / 100);
  if (in.opposite_colour) dmg = dmg * 300 / 200;
  if (in.gear_up) dmg = dmg * 125 / 100;
  if (in.gear_down) dmg = dmg * 75 / 100;
  out.value = dmg;
  out.code = dmg == 0 ? 1 : 0;
  if (in.target_immune) {
    out.value = -1;
    out.code = 2;
  } else if (in.target_absorb) {
    out.code = 3;
  }
  return out;
}

int heal_amount(Rng& rng, int mag, int power, int slot_mille, int colour_mille) {
  long long sum = 0;
  for (int i = 0; i <= mag + 1; ++i) sum += power * 100 - rng.rnd(0, power);
  const int base = std::max(1, static_cast<int>(sum / 400));
  const long long heal = static_cast<long long>(base) * slot_mille / 1000 * colour_mille / 1000;
  return static_cast<int>(std::min<long long>(heal, 999));
}

int slot_mille(int row, int level) {
  if (level <= 0) return 1000;
  const int native = level - 1;
  const int distance = row > native ? row - native : native - row;
  return 1000 + (row > native ? 125 : -125) * distance;
}

// ------------------------------------------------------------------------------------------------ statuses
bool status_roll(Rng& rng, int chance, bool weak) {
  if (weak) chance *= 2;
  return rng.rnd(1, 99) <= chance;
}

int status_dur_mult(int idx) { return (idx == 19 || idx == 38 || idx == 39) ? 1 : 3; }
int status_timer(int idx, int dur_stat, int dur_mod) { return status_dur_mult(idx) * (dur_stat + dur_mod); }

// ------------------------------------------------------------------------------------------------ stamina
int weapon_stamina_cost(int strength, bool fatigued) {
  const int cost = (strength + 1) * 10;
  return fatigued ? cost * 500 / 400 : cost;
}

int recovery_step(int rate, int mod, int steps, bool flu) {
  const int recovered = (rate + mod) * steps;
  return flu ? recovered * 300 / 400 : recovered;
}

// ------------------------------------------------------------------------------------------------ growth
int growth_roll(Rng& rng, int range, int star) {
  const long long den = 101 - star;
  const long long roll = rng.rnd(1, 999);
  const long long scaled = (roll * range * 1000) / den;
  long long doubled = scaled * 2;
  if (doubled % 1000000 > 499999) doubled += 1000000;
  return static_cast<int>(doubled / 1000000);
}

int next_pending_hp(Rng& rng, int max_hp, int lv99_hp, int star) {
  if (max_hp >= lv99_hp) return 0;
  int delta = growth_roll(rng, lv99_hp - max_hp, star) + 2;
  if (max_hp + delta > lv99_hp) delta = lv99_hp - max_hp;
  return delta;
}

int next_pending_stat(Rng& rng, int cur, int lv99, int star) {
  if (cur >= lv99) return 0;
  int delta = growth_roll(rng, lv99 - cur, star);
  if (cur + delta > lv99) delta = lv99 - cur;
  return delta;
}

int minor_growth(int snap, int pending, int cur, int pending_tenths, int progress, int cap) {
  if (cap <= 0) return 0;
  const double aimed = pending * pending_tenths / 10.0;
  const double gain = 0.5 * ((snap + aimed) - cur) * std::sqrt(static_cast<double>(progress) / cap);
  const int rounded = static_cast<int>(std::floor(gain + 0.5));
  return rounded < 0 ? 0 : rounded;
}

int full_minor_growth(int snap, int pending, int cur, int pending_tenths) {
  const double gain = (snap + pending * pending_tenths / 10.0) - cur;
  const int rounded = static_cast<int>(std::floor(gain + 0.5));
  return rounded < 0 ? 0 : rounded;
}

// ------------------------------------------------------------------------------------------------ victory
int gold_reward(int base_total, bool bonus) { return bonus ? base_total * 125 / 100 : base_total; }

int drop_roll(Rng& rng, int drop_base) {
  const int roll = rng.rnd(0, 99);
  if (roll < drop_base / 20) return 1;
  if (roll < drop_base / 2) return 0;
  return -1;
}

// ------------------------------------------------------------------------------------------------ encounters
int encounter_variant(Rng& rng, const std::vector<std::array<u16, 2>>& bands) {
  const int roll = rng.rnd(0, 2549);
  for (size_t i = 0; i < bands.size() && i < 8; ++i)
    if (bands[i][0] <= roll && roll < bands[i][1]) return 7 - static_cast<int>(i);
  return 0;
}

int encounter_scene(Rng& rng, const data::GameTables& tables, int battle_id) {
  const int variant = battle_id < 0x200 ? encounter_variant(rng, tables.variant_bands) : 0;
  return tables.scene_for(battle_id, variant);
}

}  // namespace oc::battle
