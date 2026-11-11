// Battle core checks: formulas with hand-worked numbers, stamina accounting over a sample turn
// sequence, field-colour FIFO, growth caps, status durations. Uses synthetic tables, no game files.
#include <memory>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include "data/char_name.h"
#include "data/char_name.h"
#include "battle/battle.h"
#include "battle/pac_vm.h"
#include "battle/versus.h"
#include "game/game_session.h"
#include "ui/menu/game_state.h"

using namespace oc;
using namespace oc::battle;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)
#define CHECK_EQ(lhs, rhs)                                                                             \
  do {                                                                                             \
    const long long va_ = (lhs), vb_ = (rhs);                                                      \
    if (va_ != vb_) {                                                                              \
      std::printf("FAIL %s:%d %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #lhs, #rhs, va_, vb_); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

using Lo = ScriptedRng;

// ------------------------------------------------------------------------------------------------ rng
void test_rng() {
  GameRng seeded(7);
  CHECK_EQ(seeded.rnd(5, 5), 5);
  CHECK_EQ(seeded.rnd(0, 0), 0);
  CHECK_EQ(seeded.rnd(0xFFFF, 3), 0xFFFF);
  // small ranges walk the permutation table one entry per call
  std::vector<u8> tab(256);
  for (int i = 0; i < 256; ++i) tab[(size_t)i] = static_cast<u8>((i * 7 + 3) & 255);
  GameRng table_rng(99, tab);
  int out[6];
  for (int& roll : out) roll = table_rng.rnd(0, 255);
  bool consistent = false;
  for (int start = 0; start < 256 && !consistent; ++start) {
    bool ok = true;
    for (int k = 0; k < 6; ++k) ok = ok && tab[(size_t)((start + 1 + k) & 255)] == out[k];
    consistent = ok;
  }
  CHECK(consistent);
  // lo + v % (hi-lo+1)
  GameRng first_rng(5, tab), second_rng(5, tab);
  const int first_roll = first_rng.rnd(0, 255);
  CHECK_EQ(second_rng.rnd(10, 19), 10 + first_roll % 10);
  // determinism
  GameRng c1(1234), c2(1234);
  bool same = true;
  for (int i = 0; i < 50; ++i) same = same && c1.rnd(0, 99) == c2.rnd(0, 99) && c1.rnd(0, 1000) == c2.rnd(0, 1000);
  CHECK(same);
}

// ------------------------------------------------------------------------------------------------ formulas
void test_formulas() {
  // 105 -> 1 + remainder.
  CHECK_EQ(ceil100(7, 15), 2);
  CHECK_EQ(ceil100(10, 10), 1);
  CHECK_EQ(ceil100(0, 77), 0);

  // Sum(n,p,v): n+1 terms of p*100 - p*roll
  Lo lo(Lo::Fallback::Lo), hi(Lo::Fallback::Hi);
  CHECK_EQ(sum_rolls(lo, 9, 12, 20), 10 * 1200);
  CHECK_EQ(sum_rolls(hi, 9, 12, 20), 10 * (1200 - 240));

  // Aqua Beam (power 12, variance 20), Serge MAG 7 vs Beach Bum MDEF 0, slot 1000, neutral colour:
  //   A = (7+1+1 -> 10 terms) * 12 * 80 = 9600, D = 2 terms * 5 * 80 = 800, raw = 8800*1000/400000 = 22
  ElementIn element;
  element.mag = 7, element.power = 12, element.var = 20, element.mdef = 0, element.mdef_var = 20, element.slot_mille = 1000, element.colour_mille = 1000;
  // Raw 22, then r=5 (hi roll) -> 22*105/100.
  DamageOut damage = element_damage(hi, element);
  CHECK_EQ(damage.value, 23);
  CHECK_EQ(damage.code, 0);
  // all-low rolls (f=0 effective): A=(10)*1200=12000, D=2*500=1000 -> 11000*1000/400000 = 27
  Lo lo2(Lo::Fallback::Lo);
  CHECK_EQ(element_damage(lo2, element).value, 27);
  // slot +125*2 (placed two rows above): 11000*1250/400000 = 34
  Lo lo3(Lo::Fallback::Lo);
  element.slot_mille = 1250;
  CHECK_EQ(element_damage(lo3, element).value, 34);
  element.slot_mille = 1000;
  // colour 875 (-> 22*875/1000) and r roll of 5 -> *105/100
  // 10 A terms, 2 D terms, r=5.
  Lo seq({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5}, Lo::Fallback::Lo);
  element.colour_mille = 875;
  // 23*105/100 = 24.
  CHECK_EQ(element_damage(seq, element).value, 27 * 875 / 1000 * 105 / 100);
  // weakness r=50, opposite colour x300/200
  Lo lo4(Lo::Fallback::Lo);
  element.colour_mille = 1000;
  element.exploit = true;
  CHECK_EQ(element_damage(lo4, element).value, 27 * 150 / 100);
  element.exploit = false;
  element.opposite_colour = true;
  Lo lo5(Lo::Fallback::Lo);
  CHECK_EQ(element_damage(lo5, element).value, 27 * 300 / 200);
  element.opposite_colour = false;
  element.target_absorb = true;
  Lo lo6(Lo::Fallback::Lo);
  CHECK_EQ(element_damage(lo6, element).code, 3);
  element.target_absorb = false;
  element.target_immune = true;
  Lo lo7(Lo::Fallback::Lo);
  CHECK_EQ(element_damage(lo7, element).value, -1);

  // Weapon: ATK 9 wp 16 (no variance) vs DEF 0 arm 4: A=11*1600, D=2*400 -> (17600-800)*10/20000 = 8, x1125/1000 = 9
  WeaponIn weapon;
  weapon.atk = 9, weapon.wp = 16, weapon.def = 0, weapon.arm = 4, weapon.mult = 10, weapon.colour_mille = 1125;
  Lo lw(Lo::Fallback::Lo);
  damage = weapon_damage(lw, weapon);
  CHECK_EQ(damage.value, 9);
  CHECK(!damage.crit);
  // heavy: (16800)*40/20000 = 33 -> *1125/1000 = 37; crit roll r=15 -> +CRIT[0]=50% -> 55
  weapon.mult = 40, weapon.strength = 2;
  Lo lc(Lo::Fallback::Lo);
  // 11 A terms + 2 D terms.
  for (int i = 0; i < 13; ++i) lc.push(0);
  lc.push(15);
  damage = weapon_damage(lc, weapon);
  CHECK(damage.crit);
  CHECK_EQ(damage.value, 37 * 150 / 100);
  // minimum: raw never below (wp-arm)/2 even when armour dominates
  WeaponIn tough;
  tough.atk = 0, tough.wp = 16, tough.def = 99, tough.arm = 4, tough.mult = 10;
  Lo lt(Lo::Fallback::Lo);
  CHECK_EQ(weapon_damage(lt, tough).value, (16 - 4) / 2);
  // guard halves; opposite colour x1.5
  WeaponIn g2 = weapon;
  g2.mult = 10, g2.strength = 0, g2.guarding = true;
  Lo lg(Lo::Fallback::Lo);
  CHECK_EQ(weapon_damage(lg, g2).value, 9 / 2);

  // Heal (Cure, p=18): Serge MAG 7 -> 9 terms * 1800 / 400 = 40
  Lo lh(Lo::Fallback::Lo);
  CHECK_EQ(heal_amount(lh, 7, 18, 1000), 40);
  Lo lh2(Lo::Fallback::Lo);
  // Capped at 999.
  CHECK_EQ(heal_amount(lh2, 200, 18, 1250), 999);
  CHECK_EQ(slot_mille(0, 3), 750);
  CHECK_EQ(slot_mille(4, 3), 1250);
  CHECK_EQ(slot_mille(2, 3), 1000);
  CHECK_EQ(slot_mille(5, 0), 1000);

  // Status
  Lo ls({40}, Lo::Fallback::Lo);
  // Doubled to 40, roll 40 <= 40.
  CHECK(status_roll(ls, 20, true));
  Lo ls2({41}, Lo::Fallback::Lo);
  CHECK(!status_roll(ls2, 20, true));
  // Duration = 3 x stat.
  CHECK_EQ(status_timer(8, 13, 0), 39);
  CHECK_EQ(status_timer(19, 13, 0), 13);
  CHECK_EQ(status_timer(38, 13, 2), 15);
  CHECK_EQ(status_timer(39, 5, 0), 5);

  // Stamina
  CHECK_EQ(weapon_stamina_cost(0, false), 10);
  CHECK_EQ(weapon_stamina_cost(2, false), 30);
  CHECK_EQ(weapon_stamina_cost(2, true), 37);
  CHECK_EQ(recovery_step(10, 0, 2, false), 20);
  CHECK_EQ(recovery_step(10, 0, 2, true), 14);
  CHECK_EQ(recovery_step(10, 0, 3, true), 21);
  CHECK_EQ(recovery_step(12, 0, 3, true), 27);
  CHECK_EQ(cap_stamina(95), 70);

  // Victory
  CHECK_EQ(gold_reward(100, true), 125);
  CHECK_EQ(gold_reward(100, false), 100);
  Lo d0({0}), d7({7}), d74({74}), d75({75});
  CHECK_EQ(drop_roll(d0, 150), 1);
  CHECK_EQ(drop_roll(d7, 150), 0);
  CHECK_EQ(drop_roll(d74, 150), 0);
  CHECK_EQ(drop_roll(d75, 150), -1);
}

// The encounter variant roll: rnd(0, 2549) against the retail bands (variant 0 = 1270..2549, 7 = 0..9), and a C9 scene draws nothing.
void test_encounter_variant() {
  data::GameTables tables;
  tables.variant_bands = {{0, 10}, {10, 30}, {30, 70}, {70, 150}, {150, 310}, {310, 630}, {630, 1270}, {1270, 2550}};
  tables.symtbl.assign(16, 0);
  for (int k = 0; k < 8; ++k) tables.symtbl[static_cast<size_t>(8 + k)] = static_cast<u16>(100 + k);
  const std::pair<int, int> cases[] = {{0, 7}, {9, 7}, {10, 6}, {69, 5}, {70, 4}, {309, 3}, {310, 2}, {1269, 1}, {1270, 0}, {2549, 0}};
  for (const auto& [roll, variant] : cases) {
    Lo rng({roll});
    CHECK_EQ(encounter_variant(rng, tables.variant_bands), variant);
    Lo id_rng({roll});
    CHECK_EQ(encounter_scene(id_rng, tables, 1), 100 + variant);
  }
  // A fixed scene (field op C9: id = scene + 0x200) keeps its scene and leaves the stream alone.
  Lo fixed_rng({0});
  CHECK_EQ(encounter_scene(fixed_rng, tables, 0x200 + 42), 42);
  CHECK_EQ(fixed_rng.calls, 0u);
  // No band table: always variant 0.
  Lo bare_rng({5});
  CHECK_EQ(encounter_variant(bare_rng, {}), 0);
  // A seeded game generator repeats its group.
  GameRng first(1234), second(1234);
  CHECK_EQ(encounter_scene(first, tables, 1), encounter_scene(second, tables, 1));
}

void test_hit_chance() {
  // 85/3 + 60*200/300.
  CHECK_EQ(hit_base(85, 60), 68);
  ChanceIn chance;
  chance.base = 68, chance.evd = 8;
  CHECK_EQ(hit_chance(chance), 60);
  // Weak chain: 60 + (102-60)*30/100.
  chance.hits = 1;
  CHECK_EQ(hit_chance(chance), 72);
  // 72 -> 81 -> 87 -> 91.
  chance.hits = 4;
  CHECK_EQ(hit_chance(chance), 91);
  chance.hits = 200;
  CHECK_EQ(hit_chance(chance), 99);
  chance.hits = 0;
  chance.dark = true;
  CHECK_EQ(hit_chance(chance), 30);
  chance.dark = false;
  chance.always_hit = true;
  CHECK_EQ(hit_chance(chance), 99);
  chance.always_hit = false;
  chance.target_asleep = true;
  CHECK_EQ(hit_chance(chance), 100);
  chance.target_asleep = false;
  chance.pc_attacker = false;
  // Enemies never chain.
  chance.hits = 5;
  CHECK_EQ(hit_chance(chance), 60);
  // Dizzy halves the evade: 8 -> 4.
  chance.target_dizzy = true;
  CHECK_EQ(hit_chance(chance), 64);
  chance.base = 5;
  chance.target_dizzy = false;
  // Clamped at 0.
  CHECK_EQ(hit_chance(chance), 0);
  Lo miss({1000}), hit({0});
  // 600 < 1000 misses; 0 < 0 is false -> hit; 600 < 600 false -> hit.
  CHECK(!roll_hit(miss, 60));
  CHECK(roll_hit(hit, 0));
  Lo edge({600});
  CHECK(roll_hit(edge, 60));
  // Only the chain pull stops at 99: the PS1's weak hit of Serge (hit base 101) on a target without evade rolls against 101 and never
  // misses; with one chain point it is 99.
  ChanceIn high;
  high.base = 101;
  CHECK_EQ(hit_chance(high), 101);
  Lo top({1000});
  CHECK(roll_hit(top, 101));
  high.hits = 1;
  CHECK_EQ(hit_chance(high), 99);
  high.hits = 0, high.dark = true;
  CHECK_EQ(hit_chance(high), 50);
}

void test_colour_and_ring() {
  const std::array<int, 3> none{-1, -1, -1};
  // white attacker: weapon 4 -> 1125, foreign colour element 3 -> 1000, own colour element 4 -> 1125
  CHECK_EQ(colour_multiplier(1, 1, none, -1), 1125);
  CHECK_EQ(colour_multiplier(1, 5, none, -1), 1000);
  CHECK_EQ(colour_multiplier(5, 5, none, -1), 1125);
  // each same-colour ring slot adds 1, each opposite slot removes 1
  CHECK_EQ(colour_multiplier(5, 5, {5, -1, -1}, -1), 1250);
  CHECK_EQ(colour_multiplier(5, 5, {5, 5, 5}, -1), 1500);
  // Red is opposite of blue; three of them give 4 - 3 = 1.
  CHECK_EQ(colour_multiplier(5, 5, {3, -1, -1}, -1), 1000);
  CHECK_EQ(colour_multiplier(5, 5, {3, 3, 3}, -1), 750);
  // target of the same colour: -1
  CHECK_EQ(colour_multiplier(5, 5, none, 5), 1000);
  // attacker colour opposite to the element cancels: pool white 1 vs black 3 -> black 2 -> 875
  CHECK_EQ(colour_multiplier(1, 2, none, -1), 875);
  // colourless
  CHECK_EQ(colour_multiplier(kNoColour, kNoColour, none, -1), 1125);
  // negative index -> 500: blue element, opposite red ring x3, target blue with ring... pool 3 - 3 - (1+0)
  CHECK_EQ(colour_multiplier(kNoColour, 5, {3, 3, 3}, 5), 500);

  // FIFO: new colour enters at the newest end, the oldest leaves
  std::array<int, 3> ring{0, 1, 2};
  ring_push(ring, 5);
  CHECK(ring[0] == 1 && ring[1] == 2 && ring[2] == 5);
  ring_push(ring, 5);
  ring_push(ring, 4);
  CHECK(ring[0] == 5 && ring[1] == 5 && ring[2] == 4);
  CHECK_EQ(ring_count(ring, 5), 2);
  ring_push(ring, -1);
  CHECK(ring[0] == 5 && ring[1] == 4 && ring[2] == -1);
}

void test_growth() {
  Lo lo(Lo::Fallback::Lo), hi(Lo::Fallback::Hi);
  // The low roll is r=1; the high roll r=999 -> round(0.999*800*2/101) = 16.
  CHECK_EQ(growth_roll(lo, 800, 0), 0);
  CHECK_EQ(growth_roll(hi, 800, 0), 16);
  // HP: R + 2, never past Lv99
  Lo h1(Lo::Fallback::Hi);
  CHECK_EQ(next_pending_hp(h1, 840, 850, 0), 2);
  Lo h2(Lo::Fallback::Hi);
  // Clamped at the cap.
  CHECK_EQ(next_pending_hp(h2, 849, 850, 0), 1);
  Lo h3(Lo::Fallback::Hi);
  CHECK_EQ(next_pending_hp(h3, 850, 850, 0), 0);
  Lo h4(Lo::Fallback::Hi);
  CHECK_EQ(next_pending_stat(h4, 88, 88, 5), 0);
  Lo h5(Lo::Fallback::Hi);
  CHECK(next_pending_stat(h5, 10, 88, 98) <= 78);
  // minor growth: v = ((snap + K) - cur) * sqrt(P/S) * 0.5, K the aim of x = pending * tenths / 10 (ceil(x) - 1 above 2).
  // x 14 -> K 13: v 6.5 -> 7; v 3.25 -> 4; already above the target: floor 0.
  CHECK_EQ(minor_growth(50, 20, 50, 7, 8, 8), 7);
  CHECK_EQ(minor_growth(50, 20, 50, 7, 2, 8), 4);
  CHECK_EQ(minor_growth(50, 20, 70, 7, 8, 8), 0);
  CHECK_EQ(minor_growth(50, 20, 50, 7, 8, 0), 0);
  // x 14.8 -> K 14, v 2.12 -> 3 (round half up gives 2); x 24 -> K 23, v exactly 11 -> 11 (round half up of 11.5 gives 12).
  CHECK_EQ(minor_growth(62, 4, 64, 37, 1, 8), 3);
  CHECK_EQ(minor_growth(204, 8, 205, 30, 8, 8), 11);
  // x 2 -> K 2: v 1.5 -> 2, v 1.0 -> 0, v 1.25 -> 1, v 0.5 -> 1, v 0.125 -> 0.
  CHECK_EQ(minor_growth(10, 2, 9, 10, 4, 4), 2);
  CHECK_EQ(minor_growth(10, 2, 10, 10, 4, 4), 0);
  CHECK_EQ(minor_growth(10, 2, 7, 10, 1, 4), 1);
  CHECK_EQ(minor_growth(10, 2, 10, 10, 1, 4), 1);
  CHECK_EQ(minor_growth(10, 2, 10, 10, 1, 64), 0);
  // One PS1 victory per party (star row: tenths 7, progress 6 of cap 30), every stat's gain as the original computed it.
  struct Gain {
    int snap, pending, cur, gain;
  };
  for (const Gain& g : {Gain{1023, 16, 1023, 3}, Gain{14, 1, 1, 4}, Gain{9, 1, 127, 0}, Gain{85, 0, 127, 0}, Gain{8, 0, 0, 2}, Gain{10, 1, 1, 3},
                        Gain{1023, 12, 1023, 2}, Gain{7, 1, 127, 0}, Gain{90, 0, 127, 0}, Gain{10, 0, 0, 3}, Gain{6, 0, 1, 1}, Gain{8, 0, 127, 0},
                        Gain{55, 8, 55, 1}, Gain{5, 0, 5, 0}, Gain{4, 1, 4, 0}, Gain{85, 0, 85, 0}, Gain{8, 0, 8, 0}, Gain{6, 0, 6, 0}})
    CHECK_EQ(minor_growth(g.snap, g.pending, g.cur, 7, 6, 30), g.gain);
  // The aims the PS1 computed: pending 16 / 12 / 8 / 1 at tenths 7 -> 11 / 8 / 5 / 1.
  CHECK(full_minor_growth(0, 16, 0, 7) == 11 && full_minor_growth(0, 12, 0, 7) == 8 && full_minor_growth(0, 8, 0, 7) == 5 && full_minor_growth(0, 1, 0, 7) == 1);
  // The full step uses the same aim: (50 + 13) - 50 = 13; x 10 -> K 9, 6 left from 53; above the target: 0.
  CHECK_EQ(full_minor_growth(50, 20, 50, 7), 13);
  CHECK_EQ(full_minor_growth(50, 20, 53, 5), 6);
  CHECK_EQ(full_minor_growth(50, 20, 70, 7), 0);
  // The Lv 99 limit of a party gain (PS1, Serge at 1023 max HP and DEF / MDEF 127 against Lv 99 850 / 78 / 43: his non-zero HP gain
  // became -173, the zero gains left DEF and MDEF above their Lv 99 values).
  CHECK_EQ(limit_gain(3, 1023, 850), -173);
  CHECK_EQ(limit_gain(0, 127, 78), 0);
  CHECK_EQ(limit_gain(4, 1, 88), 4);
  CHECK_EQ(limit_gain(5, 86, 88), 2);
  CHECK_EQ(limit_gain(2, 88, 88), 0);
  CHECK_EQ(progress_cap({8, 7, 1}), 8);
  CHECK_EQ(progress_cap({10, 9, 64}), 640);
  // A star-up history with every roll 999: ATK 10 toward 810 has the star-0 pending 16, so one star-up gives 26; no star-up, the record.
  const std::array<int, 7> base{100, 10, 10, 10, 10, 10, 10}, lv99{1200, 810, 60, 95, 25, 52, 43};
  CHECK(star_growth(base, lv99, 0, hi) == base);
  CHECK_EQ(star_growth(base, lv99, 1, hi)[1], 26);
  // The top history never falls with more stars and never passes Lv 99 (max HP also not 999); a stat already above Lv 99 is never lowered.
  std::array<int, 7> previous = base;
  for (int star = 0; star < 100; ++star) {
    const std::array<int, 7> best = star_growth(base, lv99, star, hi);
    for (size_t k = 0; k < 7; ++k) {
      CHECK(best[k] >= previous[k]);
      CHECK(best[k] <= (k == 0 ? 999 : lv99[k]));
    }
    previous = best;
  }
  CHECK_EQ(star_growth({100, 10, 70, 10, 10, 10, 10}, lv99, 5, hi)[2], 70);
  // The typical record (the mean of histories) sits between the starting stats and the top history, well below the top: the rolls
  // average about half of 999.
  GameRng history_rng(7);
  const std::array<int, 7> typical = typical_star_growth(base, lv99, 9, history_rng, 256);
  const std::array<int, 7> top = star_growth(base, lv99, 9, hi);
  for (size_t k = 0; k < 7; ++k) CHECK(typical[k] >= base[k] && typical[k] <= top[k]);
  CHECK(typical[1] - base[1] < (top[1] - base[1]) * 3 / 4);
  CHECK(typical[1] - base[1] > (top[1] - base[1]) / 4);
}

// ------------------------------------------------------------------------------------------------ battle fixtures
data::GameTables make_tables() {
  data::GameTables tables;
  tables.elements.resize(4);
  // Element 1 is Aqua Beam; element 2 is a status-only element: afraid (lo 0x100) 100%.
  auto& aq = tables.elements[1];
  aq.raw[0] = 1, aq.raw[1] = 1, aq.raw[5] = 0x80, aq.raw[6] = 1, aq.raw[7] = 0, aq.raw[8] = 12, aq.raw[0xC] = 20;
  auto& afraid = tables.elements[2];
  afraid.raw[7] = 1, afraid.raw[5] = 0x20, afraid.raw[6] = 1, afraid.raw[0xA] = 100, afraid.raw[0x19] = 0x01;
  auto& cure = tables.elements[3];
  cure.raw[5] = 0x80, cure.raw[6] = 1, cure.raw[7] = 3, cure.raw[0xA] = 18;
  tables.enemies.resize(1);
  auto& enemy = tables.enemies[0];
  // 9999 HP.
  enemy.raw[0x16] = 0x0F, enemy.raw[0x17] = 0x27;
  enemy.raw[0x15] = 0x40;
  enemy.raw[0x18] = 4, enemy.raw[0x19] = 0, enemy.raw[0x1A] = 100, enemy.raw[0x1B] = 0, enemy.raw[0x1C] = 4, enemy.raw[0x1D] = 0;
  enemy.raw[0x1E] = 13, enemy.raw[0x1F] = 10, enemy.raw[0x2E] = 100, enemy.raw[0xA2] = 25, enemy.raw[0x2C] = 150;
  enemy.raw[1] = 0x80;
  tables.scenes.resize(1);
  auto& scene = tables.scenes[0];
  scene.raw.fill(0);
  scene.raw[0] = 0, scene.raw[1] = 0xFF, scene.raw[2] = 0xFF, scene.raw[3] = 1;
  tables.growth.assign(100, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  tables.star_rows.assign(100, {8, 7, 1, 0});
  tables.drops.resize(1);
  tables.drops[0].slot = {0x0005, 0x8069, 0, 0};
  return tables;
}

PartyMember make_member(int id, const char* name, int hp = 50) {
  PartyMember member;
  member.char_id = id;
  member.name = name;
  member.stat = {hp, 9, 5, 85, 8, 7, 7};
  member.hp = hp;
  member.snap = member.stat;
  member.lv99 = {850, 88, 78, 95, 25, 52, 43};
  member.innate = 0x08;
  member.dur = 13;
  member.cells[0] = 0x4001;
  member.unlocked[0] = true;
  // Cell 10 holds Cure, row 1.
  member.cells[10] = 0x4003;
  member.unlocked[10] = true;
  // An element in row 7 lets the gauge reach level 8.
  member.cells[70] = 0x4001;
  member.unlocked[70] = true;
  return member;
}

struct NullBrain final : EnemyBrain {
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::None;
    return action;
  }
};

struct Fixture {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  NullBrain brain;
  Battle battle{tables, rng, brain};
  bool started = false;
  Fixture() {
    // The enemy ap roll is rnd(0, ap>>2); its countdown is rnd(0, ap).
    rng.push(0);
    rng.push(100);
    started = battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0);
  }
};

void test_setup() {
  Fixture fixture;
  CHECK(fixture.started);
  CHECK(fixture.battle.unit(0).present && fixture.battle.unit(1).present && fixture.battle.unit(2).present && fixture.battle.unit(3).present && !fixture.battle.unit(4).present);
  CHECK_EQ(fixture.battle.unit(0).stamina, 70);
  CHECK_EQ(fixture.battle.unit(0).gauge, -1);
  CHECK_EQ(fixture.battle.unit(0).countdown, 8);
  // Male +5%.
  CHECK_EQ(fixture.battle.unit(3).max_hp, 9999 * 0x69 / 100);
  CHECK_EQ(fixture.battle.unit(3).ap, 100);
  CHECK_EQ(fixture.battle.unit(3).countdown, 100);
  CHECK_EQ(fixture.battle.outcome() == Outcome::Ongoing, true);
  // Party slot i takes unit i whatever the character ids.
  Fixture other_fixture;
  ScriptedRng rng(Lo::Fallback::Lo);
  rng.push(0);
  rng.push(100);
  NullBrain nb;
  Battle b2(other_fixture.tables, rng, nb);
  CHECK(b2.start({make_member(2, "C"), make_member(0, "A"), make_member(1, "B")}, 0));
  CHECK(b2.unit(0).name == "C" && b2.unit(1).name == "A" && b2.unit(2).name == "B");
  CHECK(!b2.start({}, 0));
  CHECK(!b2.start({make_member(0, "A")}, 5));
}

// The gauge cap is the deepest grid row with an unlocked cell holding an element: an empty open cell below does not count, and a
// member without any element has no gauge.
void test_gauge_cap() {
  data::GameTables tables = make_tables();
  ScriptedRng rng(Lo::Fallback::Lo);
  rng.push(0);
  rng.push(100);
  NullBrain brain;
  Battle battle(tables, rng, brain);
  PartyMember capped = make_member(0, "A");
  capped.cells[70] = 0x4000;
  PartyMember bare = make_member(2, "C");
  bare.unlocked.fill(false);
  CHECK(battle.start({capped, make_member(1, "B"), bare}, 0));
  CHECK_EQ(battle.unit(0).gauge_max, 1);
  CHECK_EQ(battle.unit(1).gauge_max, 7);
  CHECK_EQ(battle.unit(2).gauge_max, -1);
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {0, 1};
  CHECK(battle.apply(attack));
  CHECK_EQ(battle.unit(0).gauge, 1);
}

// An enemy's gauge tops out at the deepest row with an element cell that lacks the lock bit 0x4000 (none: -1). Its landed hits never
// charge it, whatever the cap: only the party's end-of-action step moves a gauge.
void test_enemy_gauge_cap() {
  for (const bool locked : {false, true}) {
    data::GameTables tables = make_tables();
    auto& enemy = tables.enemies[0];
    // Row 0 column 0 and row 2 column 0 exist; the first holds element 1 unlocked, the second element 1 with or without the lock bit.
    enemy.raw[0x30] = 1, enemy.raw[0x34] = 1;
    enemy.raw[0x40] = 0x01, enemy.raw[0x41] = 0x40;
    enemy.raw[0x42] = 0x01, enemy.raw[0x43] = locked ? 0x40 : 0x00;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain brain;
    Battle battle(tables, rng, brain);
    CHECK(battle.start({make_member(0, "A")}, 0));
    CHECK_EQ(battle.unit(3).gauge_max, locked ? -1 : 2);
    CHECK_EQ(battle.unit(3).gauge, -1);
    battle.do_weapon_attack(3, 0, 2);
    CHECK_EQ(battle.unit(3).gauge, -1);
  }
}

// The field ring comes from the btbgdat row of the stage: the battlefield operand when it names a row, else the scene's background
// (the same fallback the stage loader uses).
void test_ring_battlefield_fallback() {
  data::GameTables tables = make_tables();
  tables.btbgdat = {{0, 1, 2}, {3, 4, 5}};
  for (const auto& [field, expected] : {std::pair{1, std::array<int, 3>{3, 4, 5}}, std::pair{-1, std::array<int, 3>{0, 1, 2}},
                                        std::pair{200, std::array<int, 3>{0, 1, 2}}}) {
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain brain;
    Battle battle(tables, rng, brain);
    BattleOptions options;
    options.battlefield = field;
    CHECK(battle.start({make_member(0, "A")}, 0, options));
    CHECK(battle.ring() == expected);
  }
}

// While Pip's form change is pending no PC can act, so the scheduler runs time on (instead of returning to a host that cannot open a
// menu) until a PC's countdown gives it its scripted turn, which casts the form element and clears the pending state.
void test_settle_with_pending_evolution() {
  struct EvolvingBrain final : EnemyBrain {
    int pc_turns = 0;
    EnemyAction choose(const Battle&, int, Rng&) override {
      EnemyAction action;
      action.kind = EnemyAction::Kind::None;
      return action;
    }
    bool plan_turn(Battle& battle, int unit, Rng&, std::vector<EnemyAction>&) override {
      if (!is_pc_slot(unit)) return false;
      ++pc_turns;
      battle.set_evo_pending(0);
      return true;
    }
  };
  data::GameTables tables = make_tables();
  ScriptedRng rng(Lo::Fallback::Lo);
  rng.push(0);
  rng.push(100);
  EvolvingBrain brain;
  Battle battle(tables, rng, brain);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B")}, 0));
  CHECK(battle.can_act(0));
  battle.set_evo_pending(0x80);
  CHECK(!battle.can_act(0) && !battle.can_act(1));
  battle.settle();
  CHECK_EQ(battle.evo_pending(), 0);
  CHECK(brain.pc_turns >= 1);
  CHECK(battle.can_act(0) || battle.can_act(1));
  CHECK(battle.outcome() == Outcome::Ongoing);
}

void test_stamina_sequence() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  battle.unit_mut(1).stamina = 30;
  battle.unit_mut(2).stamina = 0;
  // PC0: weak then medium chain: 70 -> 60 -> 40
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {0, 1};
  std::string why;
  CHECK(battle.apply(attack, &why));
  CHECK_EQ(battle.unit(0).stamina, 40);
  // Recovery is +1 step, then +2 steps.
  CHECK_EQ(battle.unit(1).stamina, 30 + 10 + 20);
  CHECK_EQ(battle.unit(2).stamina, 0 + 10 + 20);
  CHECK_EQ(battle.unit(3).countdown, 100 - 1 - 2);
  CHECK_EQ(battle.unit(0).gauge, 2);
  CHECK(battle.unit(3).hp < battle.unit(3).max_hp);

  // a chain that does not fit the stamina budget is refused and changes nothing
  Command big;
  big.kind = Command::Kind::Attack;
  big.actor = 0;
  big.chain = {2, 2};
  CHECK(!battle.apply(big, &why));
  CHECK_EQ(battle.unit(0).stamina, 40);

  // defend: +1.0 for the actor (cap 70), 1 step for the rest, guard flag set
  Command defend;
  defend.kind = Command::Kind::Defend;
  defend.actor = 1;
  CHECK(battle.apply(defend));
  // 60 + 10 capped at 70.
  CHECK_EQ(battle.unit(1).stamina, 70);
  CHECK_EQ(battle.unit(0).stamina, 50);
  CHECK(battle.unit(1).guard);
  CHECK_EQ(battle.unit(3).countdown, 96);
  // The guard does not close the defender's menu (an L1 / R1 switch may land on it) and outlasts the other PCs' commands ...
  CHECK(battle.can_act(1) && battle.can_act(0) && battle.can_act(2));
  Command d0;
  d0.kind = Command::Kind::Defend;
  d0.actor = 0;
  CHECK(battle.apply(d0));
  CHECK(battle.unit(0).guard && battle.unit(1).guard);
  CHECK_EQ(battle.unit(3).countdown, 95);
  // ... until the PC's command menu opens.
  battle.open_menu(0);
  CHECK(!battle.unit(0).guard && battle.unit(1).guard);
  // A Defend that leaves no PC free to take the menu (the others guarding or unable) runs one time step: the enemy's countdown reaches
  // 0 and it acts (countdown back to ap = 100 with the Lo rng) before the menu passes on.
  battle.unit_mut(2).stamina = -100;
  CHECK(battle.apply(d0));
  CHECK_EQ(battle.unit(3).countdown, 100);
  CHECK(battle.unit(0).guard && battle.unit(1).guard);
  battle.open_menu(0);
  battle.open_menu(1);
  CHECK(!battle.unit(0).guard && !battle.unit(1).guard);

  // element: flat 70 stamina whatever the level, gauge cost row+1 (row 0 -> 1); others recover 1 step
  battle.unit_mut(0).stamina = 70;
  battle.unit_mut(1).stamina = 20;
  CHECK(battle.element_usable(0, 0));
  // a colour seal (merged lo & the colour's mask; Aqua Beam is blue = 0x80000000) greys the cell and refuses it; a green seal leaves it alone
  battle.unit_mut(0).lo |= 0x800u;
  CHECK(!battle.element_sealed(0, 0) && battle.element_usable(0, 0));
  battle.unit_mut(0).lo |= 0x80000000u;
  CHECK(battle.element_sealed(0, 0) && !battle.element_usable(0, 0));
  battle.unit_mut(0).lo &= ~(0x800u | 0x80000000u);
  CHECK(!battle.element_sealed(0, 0) && battle.element_usable(0, 0));
  Command element;
  element.kind = Command::Kind::Element;
  element.actor = 0;
  element.cell = 0;
  CHECK(battle.apply(element, &why));
  CHECK_EQ(battle.unit(0).stamina, 0);
  // Gauge 2 - 1.
  CHECK_EQ(battle.unit(0).gauge, 1);
  CHECK_EQ(battle.unit(1).stamina, 30);
  // Used this battle, and stamina is gone.
  CHECK(!battle.element_usable(0, 0));
  CHECK(!battle.can_act(0));
  // field colour FIFO took Aqua Beam's blue
  CHECK_EQ(battle.ring()[2], 5);
  CHECK_EQ(battle.ring()[0], -1);

  // nobody able to act -> time advances by the smallest enemy countdown and the enemy then acts
  battle.unit_mut(0).stamina = 0;
  battle.unit_mut(1).stamina = 0;
  battle.unit_mut(2).stamina = 0;
  battle.unit_mut(3).countdown = 30;
  battle.settle();
  // Recovery is +30 * rate 10, capped. The enemy acted (Null) and its countdown reset to ap + rnd(0, ap>>2) = 100 with the Lo rng.
  CHECK_EQ(battle.unit(0).stamina, 70);
  CHECK_EQ(battle.unit(3).countdown, 100);
  CHECK(battle.can_act(0));
}

// ActionEvents (presentation feed): one event per hit with actor / target / strength, HP before / after, the rules log line.
void test_action_events() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {0, 1};
  CHECK(battle.apply(attack));
  const auto ev = battle.drain_events();
  CHECK(!ev.empty() && ev.size() >= 2);
  if (ev.size() >= 2) {
    CHECK(ev[0].kind == ActionEvent::Kind::Weapon && ev[0].actor == 0 && ev[0].target == 3 && ev[0].strength == 0);
    CHECK(ev[1].strength == 1 && ev[1].action == ev[0].action && ev[1].hit_index == ev[0].hit_index + 1);
    CHECK(ev[0].hp_before >= ev[0].hp_after && !ev[0].text.empty());
    if (ev[0].code == 0) CHECK_EQ(ev[0].hp_before - ev[0].hp_after, ev[0].value);
  }
  CHECK(battle.drain_events().empty());
  Command defend;
  defend.kind = Command::Kind::Defend;
  defend.actor = 1;
  CHECK(battle.apply(defend));
  const auto de = battle.drain_events();
  CHECK(!de.empty() && de.front().kind == ActionEvent::Kind::Defend && de.front().actor == 1);
  CHECK(de.front().action > (ev.empty() ? 0 : ev.front().action));
}

// Op 96's colour gate: a PC's weapon hit on an enemy lands with result 0 (hit code 0, the number shows "0") and leaves the HP alone.
void test_colour_gate() {
  struct GateBrain final : EnemyBrain {
    EnemyAction choose(const Battle&, int, Rng&) override {
      EnemyAction action;
      action.kind = EnemyAction::Kind::None;
      return action;
    }
    int colour_gate() const override { return 4; }
  };
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  GateBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {0};
  CHECK(battle.apply(attack));
  const auto ev = battle.drain_events();
  CHECK(!ev.empty());
  if (!ev.empty() && ev[0].code != 2) {
    CHECK(ev[0].gated);
    CHECK_EQ(ev[0].value, 0);
    CHECK_EQ(ev[0].code, 0);
    CHECK_EQ(ev[0].hp_before, ev[0].hp_after);
  }
}

void test_status_and_poison() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  // status-only element 2 (afraid, 100%) on PC1 from PC0: timer = 3 x status-duration stat
  battle.do_element(0, 2, 0, 1);
  CHECK((battle.unit(1).lo & st::kAfraid) != 0);
  CHECK_EQ(battle.unit(1).timers.at(8), 3 * 13);
  // Five steps of timer decay.
  battle.post_action(0, 1, 5);
  CHECK_EQ(battle.unit(1).timers.at(8), 3 * 13 - 5);
  // An enemy-led action counts but steps no timer.
  const int actions0 = battle.actions();
  battle.post_action(3, 1, 5);
  CHECK_EQ(battle.unit(1).timers.at(8), 3 * 13 - 5);
  CHECK_EQ(battle.actions(), actions0 + 1);
  battle.post_action(0, 1, 100);
  CHECK((battle.unit(1).lo & st::kAfraid) == 0 && battle.unit(1).timers.empty());
  // The target's gear duration modifier joins its duration stat: 3 x (13 - 2).
  battle.unit_mut(1).eq.dur_mod = -2;
  battle.do_element(0, 2, 0, 1);
  CHECK_EQ(battle.unit(1).timers.at(8), 3 * 11);
  battle.post_action(0, 1, 100);

  // poison: 1 per defend step on every living unit, PCs can die; an enemy landing on exactly 0 stays at 1, below 0 it keeps the negative HP
  // and stands
  battle.unit_mut(2).lo |= st::kPoison;
  battle.unit_mut(3).lo |= st::kPoison;
  const int hp2 = battle.unit(2).hp;
  battle.post_action(0, 7, 1);
  CHECK_EQ(battle.unit(2).hp, hp2 - 7);
  battle.post_action(3, 7, 1);
  CHECK_EQ(battle.unit(2).hp, hp2 - 7);
  battle.unit_mut(3).hp = 7;
  battle.post_action(0, 7, 1);
  CHECK_EQ(battle.unit(3).hp, 1);
  battle.unit_mut(3).hp = 3;
  battle.post_action(0, 7, 1);
  CHECK_EQ(battle.unit(3).hp, -4);
  CHECK(battle.unit(3).standing() && battle.outcome() == Outcome::Ongoing);
  battle.unit_mut(3).hp = 1;
  battle.unit_mut(2).hp = 5;
  battle.post_action(0, 7, 1);
  CHECK(!battle.unit(2).alive() && battle.unit(2).hp == 0);

  // heal element never overheals and costs nothing here
  battle.unit_mut(1).hp = 10;
  battle.do_element(0, 3, 1, 1);
  CHECK(battle.unit(1).hp > 10 && battle.unit(1).hp <= battle.unit(1).max_hp);
}

// Element 4 = Saints (mode 14, power 110, heal power 200), 5 = Purify (mode 1, +0xE bit 0, lo 0xFFFFFF3F), 6 = Turn Red (mode 1, hi 0x08).
void add_support_elements(data::GameTables& tables) {
  tables.elements.resize(7);
  auto& saints = tables.elements[4];
  saints.raw[0] = 0x41, saints.raw[4] = 0xD0, saints.raw[5] = 0x08, saints.raw[6] = 1, saints.raw[7] = 14, saints.raw[8] = 110, saints.raw[0xA] = 200;
  auto& purify = tables.elements[5];
  purify.raw[0] = 0x04, purify.raw[1] = 1, purify.raw[4] = 0x40, purify.raw[5] = 0x08, purify.raw[6] = 1, purify.raw[7] = 1, purify.raw[0xA] = 100;
  purify.raw[0xE] = 1, purify.raw[0x18] = 0x3F, purify.raw[0x19] = 0xFF, purify.raw[0x1A] = 0xFF, purify.raw[0x1B] = 0xFF;
  auto& turn_red = tables.elements[6];
  turn_red.raw[0] = 0x08, turn_red.raw[1] = 1, turn_red.raw[4] = 0xC0, turn_red.raw[5] = 0x20, turn_red.raw[6] = 1, turn_red.raw[7] = 1;
  turn_red.raw[0xA] = 100, turn_red.raw[0x1C] = 0x08;
}

void test_enemy_element_value_shift() {
  // An enemy's element action halves the result of ids 150..301 and quarters a summon's (record byte 0 bit 0x40), shown and applied; a
  // lower id, a PC caster and scene 0x1D9 keep the full value. The element: mode 33, 40 % of the target's HP, colourless.
  struct Hit {
    int loss = -1, shown = -1;
  };
  const auto cast = [](int caster, int id, u8 flags, int scene_index) {
    data::GameTables tables = make_tables();
    tables.elements.resize(303);
    data::ElemRec& element = tables.elements[static_cast<size_t>(id)];
    element.raw[0] = flags, element.raw[6] = 1, element.raw[7] = 33, element.raw[8] = 40;
    tables.scenes.resize(static_cast<size_t>(scene_index) + 1);
    tables.scenes[static_cast<size_t>(scene_index)] = tables.scenes[0];
    ScriptedRng rng{Lo::Fallback::Lo};
    NullBrain brain;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    battle.start({make_member(0, "A", 800), make_member(1, "B"), make_member(2, "C")}, scene_index);
    const int target = is_pc_slot(caster) ? 3 : 0;
    battle.unit_mut(target).hp = 800;
    battle.do_element(caster, id, 0, target);
    Hit hit;
    hit.loss = 800 - battle.unit(target).hp;
    if (!battle.events().empty()) hit.shown = battle.events().back().value;
    return hit;
  };
  const Hit full = cast(3, 149, 0x01, 0);
  CHECK(full.loss >= 8);
  CHECK_EQ(full.shown, full.loss);
  const Hit half = cast(3, 150, 0x01, 0), half_top = cast(3, 301, 0x01, 0);
  CHECK_EQ(half.loss, full.loss >> 1);
  CHECK_EQ(half.shown, half.loss);
  CHECK_EQ(half_top.loss, full.loss >> 1);
  CHECK_EQ(cast(3, 302, 0x01, 0).loss, full.loss);
  const Hit quarter = cast(3, 41, 0x41, 0);
  CHECK_EQ(quarter.loss, full.loss >> 2);
  CHECK_EQ(quarter.shown, quarter.loss);
  CHECK_EQ(cast(3, 182, 0x01, 0x1D9).loss, full.loss);
  CHECK_EQ(cast(0, 182, 0x01, 0).loss, cast(0, 149, 0x01, 0).loss);
}

void test_element_mode40_half_hp() {
  // Mode 40 (Toxic Burst): the hit pass's status roll lands, then the target and the caster each lose the target's HP / 2. A zero roll keeps
  // code 1 even on an absorber; a non-zero roll on an absorber heals both by the half; an immune target is a miss and nothing reaches the
  // caster.
  struct Out {
    int target_hp = 0, caster_hp = 0, code = -1;
    u32 lo = 0;
  };
  const auto cast = [](int power, u32 target_hi, int caster_hp, bool caster_ko) {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    data::ElemRec& burst = fixture.tables.elements[4];
    burst.raw[0] = 1, burst.raw[4] = 0x80, burst.raw[5] = 0x04, burst.raw[7] = 40, burst.raw[8] = static_cast<u8>(power), burst.raw[0xA] = 100;
    burst.raw[0x18] = static_cast<u8>(st::kPoison);
    Battle& battle = fixture.battle;
    Unit& pc = battle.unit_mut(1);
    pc.max_hp = 900, pc.hp = 601;
    pc.hi |= target_hi;
    Unit& enemy = battle.unit_mut(3);
    enemy.hp = caster_hp;
    if (caster_ko) enemy.lo |= st::kKO;
    battle.do_element(3, 4, 0, 1);
    Out out;
    out.target_hp = battle.unit(1).hp, out.caster_hp = battle.unit(3).hp, out.lo = battle.unit(1).lo;
    if (!battle.events().empty()) out.code = battle.events().back().code;
    return out;
  };
  const Out plain = cast(0, 0, 2000, false);
  CHECK_EQ(plain.target_hp, 301);
  CHECK((plain.lo & st::kPoison) != 0);
  CHECK_EQ(plain.caster_hp, 1700);
  CHECK_EQ(plain.code, 1);
  const Out absorbed_zero = cast(0, st::kHiAbsorb, 2000, false);
  CHECK_EQ(absorbed_zero.target_hp, 301);
  CHECK_EQ(absorbed_zero.code, 1);
  const Out absorbed = cast(20, st::kHiAbsorb, 1500, false);
  CHECK_EQ(absorbed.target_hp, 900);
  CHECK_EQ(absorbed.code, 3);
  CHECK_EQ(absorbed.caster_hp, 1800);
  CHECK_EQ(cast(20, 0, 2000, false).target_hp, 301);
  const Out immune = cast(0, st::kHiImmune, 2000, false);
  CHECK_EQ(immune.target_hp, 601);
  CHECK((immune.lo & st::kPoison) == 0);
  CHECK_EQ(immune.caster_hp, 2000);
  CHECK_EQ(immune.code, 2);
  CHECK_EQ(cast(0, 0, 64, false).caster_hp, 1);
  CHECK_EQ(cast(0, 0, 0, true).caster_hp, 0);
}

void test_ko_drops_pc_statuses() {
  // A hit that knocks a PC out keeps only its permanent statuses and the inflicted lo 0x400480 / hi 0x1FE00000, drops the other timers and
  // leaves the stat modifiers; an enemy keeps everything.
  Fixture fixture;
  fixture.tables.elements.resize(5);
  data::ElemRec& finisher = fixture.tables.elements[4];
  finisher.raw[6] = 1, finisher.raw[7] = 33, finisher.raw[8] = 100;
  Battle& battle = fixture.battle;
  Unit& pc = battle.unit_mut(1);
  pc.perm_lo |= 0x8;
  pc.lo |= st::kPoison | 0x8 | st::kAfraid | 0x400000;
  pc.hi |= 0x40 | 0x200000;
  pc.timers[0] = 9, pc.timers[8] = 9, pc.timers[32 + 6] = 9, pc.timers[32 + 21] = 9;
  pc.mod[0] = 150;
  pc.hp = 1;
  battle.do_element(3, 4, 0, 1);
  CHECK(!battle.unit(1).alive());
  CHECK_EQ(battle.unit(1).lo, battle.unit(1).perm_lo | 0x400000u | st::kKO);
  CHECK_EQ(battle.unit(1).hi, battle.unit(1).perm_hi | 0x200000u);
  CHECK(battle.unit(1).timers.size() == 1 && battle.unit(1).timers.count(32 + 21) == 1);
  CHECK_EQ(battle.unit(1).mod[0], 150);
  battle.unit_mut(3).lo |= st::kPoison;
  battle.unit_mut(3).hp = 1;
  battle.do_element(0, 4, 0, 3);
  CHECK(!battle.unit(3).alive());
  CHECK((battle.unit(3).lo & st::kPoison) != 0);
}

void test_stamina_statuses_on_heavy_hit() {
  // A fatigued PC's heavy hit on a cursed enemy, the other PCs at rate 10: the fatigued cost is 37, a PC with flu recovers 3 x (10 x 3/4)
  // = 21 (the factor truncates per step), the cursed target loses 10 stamina, poison takes 3 HP from every poisoned unit and the timed
  // statuses step by 3. With timers of 3 both statuses run out after the hit, which still pays and drains as before.
  for (const int timer : {20, 3}) {
    Fixture fixture;
    Battle& battle = fixture.battle;
    Unit& actor = battle.unit_mut(0);
    actor.lo |= st::kFatigued | st::kPoison;
    actor.timers[13] = timer;
    Unit& flu = battle.unit_mut(1);
    flu.lo |= st::kFlu;
    flu.rate = 10;
    flu.stamina = 0;
    Unit& plain = battle.unit_mut(2);
    plain.lo |= st::kPoison;
    plain.rate = 10;
    plain.stamina = 0;
    Unit& enemy = battle.unit_mut(3);
    enemy.lo |= st::kCursed;
    enemy.timers[18] = timer;
    enemy.stamina = 0;
    const int actor_hp = actor.hp, plain_hp = plain.hp, enemy_hp = enemy.hp;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.target = 3;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK_EQ(battle.unit(0).stamina, 33);
    CHECK_EQ(battle.unit(1).stamina, 21);
    CHECK_EQ(battle.unit(2).stamina, 30);
    CHECK_EQ(battle.unit(3).stamina, -10);
    CHECK_EQ(battle.unit(0).hp, actor_hp - 3);
    CHECK_EQ(battle.unit(2).hp, plain_hp - 3);
    CHECK(battle.unit(3).hp < enemy_hp);
    if (timer == 20) {
      CHECK_EQ(battle.unit(0).timers.at(13), 17);
      CHECK_EQ(battle.unit(3).timers.at(18), 17);
    } else {
      CHECK((battle.unit(0).lo & st::kFatigued) == 0 && !battle.unit(0).timers.contains(13));
      CHECK((battle.unit(3).lo & st::kCursed) == 0 && !battle.unit(3).timers.contains(18));
    }
  }
}

void test_poison_ko() {
  // Poison taking a PC to 0 keeps the inflicted lo bits 1..15 (poison and bits 16..31 go) and the hi word, and empties the stamina.
  Fixture fixture;
  Battle& battle = fixture.battle;
  Unit& pc = battle.unit_mut(1);
  pc.lo |= st::kPoison | st::kFlu | 0x04000000;
  pc.hi |= 0x40;
  pc.hp = 3;
  battle.post_action(0, 7, 1);
  CHECK(!battle.unit(1).alive());
  CHECK_EQ(battle.unit(1).hp, 0);
  CHECK_EQ(battle.unit(1).lo, battle.unit(1).perm_lo | st::kFlu | st::kKO);
  CHECK((battle.unit(1).hi & 0x40) != 0);
  CHECK_EQ(battle.unit(1).stamina, 0);
}

void test_revive_resets_stamina_and_countdown() {
  // Mode 4 brings a KO'd unit back with max HP x pct / 100 (15 % from an enemy caster), stamina 70 and countdown AP + rnd(0, AP >> 2).
  Fixture fixture;
  fixture.tables.elements.resize(5);
  data::ElemRec& revive = fixture.tables.elements[4];
  revive.raw[4] = 0x41, revive.raw[6] = 1, revive.raw[7] = 4;
  Battle& battle = fixture.battle;
  battle.unit_mut(1).hp = 0;
  battle.unit_mut(1).lo |= st::kKO;
  battle.unit_mut(1).stamina = 3;
  battle.unit_mut(1).countdown = 0;
  fixture.rng.push(2);
  battle.do_element(3, 4, 0, 1);
  CHECK(battle.unit(1).alive());
  CHECK_EQ(battle.unit(1).hp, battle.unit(1).max_hp * 15 / 100);
  CHECK_EQ(battle.unit(1).stamina, 70);
  CHECK_EQ(battle.unit(1).countdown, battle.unit(1).ap + 2);
}

// A consumable stack at 0 copies stays in its cell and is greyed (used) whenever the element menu opens, a recharged one too; the PS1 sets
// the used bit of a Tablet cell whose save count is 0 and lists it.
void test_empty_stack_greyed() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  Unit& pc = battle.unit_mut(0);
  pc.cells[2] = 1, pc.usable[2] = true, pc.stack[2] = true, pc.qty[2] = 0;
  pc.cells[3] = 1, pc.usable[3] = true, pc.stack[3] = true, pc.qty[3] = 2;
  battle.open_element_menu(0);
  CHECK(battle.unit(0).used[2] && battle.unit(0).usable[2]);
  CHECK(!battle.unit(0).used[3]);
  battle.unit_mut(0).used[2] = false;
  battle.open_element_menu(0);
  CHECK(battle.unit(0).used[2]);
}

void test_revive_to_zero_hp() {
  // A revive whose max HP x pct / 100 rounds to 0 leaves the unit up at 0 HP with the KO bit cleared, still a revive target.
  Fixture fixture;
  fixture.tables.elements.resize(5);
  data::ElemRec& revive = fixture.tables.elements[4];
  revive.raw[4] = 0x41, revive.raw[6] = 1, revive.raw[7] = 4;
  Battle& battle = fixture.battle;
  battle.unit_mut(1).max_hp = 6;
  battle.unit_mut(1).hp = 0;
  battle.unit_mut(1).lo |= st::kKO;
  fixture.rng.push(0);
  battle.do_element(3, 4, 0, 1);
  CHECK(battle.unit(1).alive());
  CHECK_EQ(battle.unit(1).hp, 0);
  battle.unit_mut(1).max_hp = 100;
  fixture.rng.push(0);
  battle.do_element(3, 4, 0, 1);
  CHECK_EQ(battle.unit(1).hp, 15);
}

void test_heal_ignores_ring_colour() {
  // A heal takes neither the colour multiplier nor the field state: Cure (blue) heals the same on a blue, a red and an empty ring.
  const auto heal = [](const std::array<int, 3>& ring) {
    Fixture fixture;
    Battle& battle = fixture.battle;
    battle.set_ring(ring);
    battle.unit_mut(1).hp = 1;
    battle.do_element(0, 3, 0, 1);
    return battle.unit(1).hp - 1;
  };
  const int plain = heal({-1, -1, -1});
  CHECK(plain > 0);
  CHECK_EQ(heal({5, 5, 5}), plain);
  CHECK_EQ(heal({3, 3, 3}), plain);
}

void test_saints_by_target_slot() {
  // Mode 14 goes by the target's slot, not the caster's side: an enemy slot takes the mode 0 hit, a party slot the heal (plus the
  // target's power x 10 stamina).
  for (const int caster : {0, 3}) {
    Fixture fixture;
    add_support_elements(fixture.tables);
    Battle& battle = fixture.battle;
    battle.unit_mut(1).hp = 10;
    battle.unit_mut(1).stamina = 0;
    const int enemy_hp = battle.unit(3).hp;
    battle.do_element(caster, 4, 0, 3);
    battle.do_element(caster, 4, 0, 1);
    CHECK(battle.unit(3).hp < enemy_hp);
    CHECK(battle.unit(1).hp > 10);
    CHECK_EQ(battle.unit(1).stamina, 70);
    const auto& ev = battle.events();
    CHECK(ev.size() >= 2);
    if (ev.size() >= 2) {
      CHECK(ev[ev.size() - 2].target == 3 && ev[ev.size() - 2].code == 0);
      CHECK(ev.back().target == 1 && ev.back().code == 3);
    }
  }
}

void test_purify_cures() {
  // Mode 1 with +0xE bit 0 removes the record's statuses and their timers; a permanent status stays.
  Fixture fixture;
  add_support_elements(fixture.tables);
  Battle& battle = fixture.battle;
  battle.do_element(0, 2, 0, 1);
  CHECK((battle.unit(1).lo & st::kAfraid) != 0 && battle.unit(1).timers.count(8) == 1);
  battle.unit_mut(1).lo |= st::kPoison;
  battle.unit_mut(1).perm_lo |= st::kPoison;
  battle.do_element(0, 5, 0, 1);
  CHECK((battle.unit(1).lo & st::kAfraid) == 0 && battle.unit(1).timers.count(8) == 0);
  CHECK((battle.unit(1).lo & st::kPoison) != 0);
  CHECK((battle.unit(1).lo & 0xFFFFFF3Fu & ~st::kPoison & ~st::kPartyShown) == 0);
  // On a clean target nothing is inflicted.
  const u32 clean = battle.unit(2).lo;
  battle.do_element(0, 5, 0, 2);
  CHECK_EQ(battle.unit(2).lo, clean);
}

void test_turn_colour_status() {
  // Turn Red gives the target the red colour status (hi bit 3), replacing any other; that unit's weapon hits are red, so against the
  // blue enemy they hit harder than the same unit's white hits.
  const auto hit = [](bool keep_status) {
    Fixture fixture;
    add_support_elements(fixture.tables);
    Battle& battle = fixture.battle;
    battle.unit_mut(0).hi |= 0x01;
    battle.do_element(1, 6, 0, 0);
    CHECK_EQ(battle.unit(0).hi & 0x3Fu, 0x08u);
    if (!keep_status) battle.unit_mut(0).hi &= ~0x3Fu;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {0};
    CHECK(battle.apply(attack));
    for (const ActionEvent& ev : battle.drain_events())
      if (ev.kind == ActionEvent::Kind::Weapon) return ev.value;
    return -1;
  };
  const int red = hit(true), white = hit(false);
  CHECK(white > 0);
  CHECK(red > white);
  // Cast again on a target that already has that colour, it clears the colour and leaves its timer running; a third cast sets it again.
  Fixture fixture;
  add_support_elements(fixture.tables);
  Battle& battle = fixture.battle;
  battle.do_element(1, 6, 0, 0);
  CHECK_EQ(battle.unit(0).hi & 0x3Fu, 0x08u);
  CHECK_EQ(battle.unit(0).timers.count(35), 1u);
  battle.do_element(1, 6, 0, 0);
  CHECK_EQ(battle.unit(0).hi & 0x3Fu, 0u);
  CHECK_EQ(battle.unit(0).timers.count(35), 1u);
  battle.do_element(1, 6, 0, 0);
  CHECK_EQ(battle.unit(0).hi & 0x3Fu, 0x08u);
}

void test_big_catch() {
  // Mode 39 (Korcha's Big Catch): roll rnd(0, 16) (0 = the line breaks: no creature, the cast misses, the record keeps model 0), then rnd(0, 255) until
  // the slot's pool bit is set; model = kDoppleTable[slot].model; the slot term = max(10, (10000 - model) / 200) * 100 replaces the grid slot term
  // (slot 26 = model 86 -> 4900).
  struct Out {
    int creature = -9, code = -9, value = -9;
    size_t events = 0;
  };
  const auto run = [](const std::array<u16, 16>& mask, const std::vector<int>& rolls, int row = 0, int target_hp = 0) {
    data::GameTables tables = make_tables();
    tables.elements.resize(5);
    auto& element = tables.elements[4];
    element.raw[0] = 1, element.raw[1] = 1, element.raw[5] = 0x80, element.raw[6] = 1, element.raw[7] = 39, element.raw[8] = 14;
    ScriptedRng rng{Lo::Fallback::Lo};
    NullBrain brain;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    BattleOptions opt;
    opt.dopple_mask = mask;
    battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0, opt);
    for (const int roll : rolls) rng.push(roll);
    if (target_hp > 0) battle.unit_mut(3).hp = target_hp;
    battle.do_element(0, 4, row, 3);
    Out out;
    out.events = battle.events().size();
    if (!battle.events().empty()) out.creature = battle.events().back().creature, out.code = battle.events().back().code, out.value = battle.events().back().value;
    return out;
  };
  std::array<u16, 16> pool{};
  pool[26 >> 4] |= static_cast<u16>(1u << (26 & 15));
  pool[40 >> 4] |= static_cast<u16>(1u << (40 & 15));
  {
    // a picked creature: the loop skips slot 5 (bit clear), takes 26 = model 86
    const Out out = run(pool, {1, 5, 26});
    CHECK_EQ(out.creature, 86);
    CHECK(out.code == 0 || out.code == 1);
  }
  {
    // an overkill shows the damage rolled, not the HP the target had left
    const Out full = run(pool, {1, 26});
    const Out overkill = run(pool, {1, 26}, 0, 1);
    CHECK(full.value > 1);
    CHECK_EQ(overkill.value, full.value);
  }
  {
    // the pick runs through the roll order: slot 40 = model 98
    const Out out = run(pool, {16, 40});
    CHECK_EQ(out.creature, 98);
  }
  {
    // the 1 in 17 roll fails: the record keeps 0 (Serge's model), the hit is a miss
    const Out out = run(pool, {0});
    CHECK_EQ(out.creature, 0);
    CHECK_EQ(out.code, 2);
    CHECK_EQ(out.value, 0);
  }
  {
    // an empty pool (an endless loop in retail) counts as a broken line
    const Out out = run({}, {7, 26});
    CHECK_EQ(out.creature, 0);
    CHECK_EQ(out.code, 2);
  }
  {
    // the grid row does not matter (the slot term is replaced): a row-3 cast of the same rolls deals what a row-0 cast does
    const Out row0 = run(pool, {1, 26}, 0), row3 = run(pool, {1, 26}, 3);
    CHECK_EQ(row0.value, row3.value);
  }
}

// The HIT% box figure leaves out the sleeping-target, dizzy and hero terms the roll uses, and a miss keeps the chain counter.
void test_hit_box_figure() {
  Fixture fixture;
  Unit& enemy = fixture.battle.unit_mut(3);
  enemy.evd = 20;
  enemy.lo |= st::kAsleep;
  const int fresh = fixture.battle.hit_chance_shown_for(0, 3, 0);
  CHECK_EQ(fixture.battle.hit_chance_for(0, 3, 0), 100);
  CHECK(fresh < 100);
  enemy.lo = (enemy.lo & ~st::kAsleep) | st::kDizzy;
  CHECK(fixture.battle.hit_chance_for(0, 3, 0) > fresh);
  CHECK_EQ(fixture.battle.hit_chance_shown_for(0, 3, 0), fresh);
  enemy.lo &= ~st::kDizzy;
  fixture.battle.do_weapon_attack(0, 3, 0);
  const int chained = fixture.battle.hit_chance_shown_for(0, 3, 0);
  CHECK(chained > fresh);
  fixture.rng.push(1000);
  fixture.battle.do_weapon_attack(0, 3, 0);
  CHECK_EQ(fixture.battle.events().back().code, 2);
  CHECK_EQ(fixture.battle.hit_chance_shown_for(0, 3, 0), chained);
  // The PS1 (Serge in the room-53 battle, hit bases 101 / 75 / 65, target raw EVD 30, total EVD 40): box 69 / 45 / 35; the heavy roll
  // 25, 45 against a dizzy target, 100 against a sleeping one.
  const int bases[3] = {101, 75, 65}, box[3] = {69, 45, 35};
  for (int k = 0; k < 3; ++k) {
    ChanceIn shown;
    shown.base = bases[k], shown.evd = 30, shown.strength = k;
    CHECK_EQ(hit_chance_shown(shown), box[k]);
  }
  ChanceIn heavy;
  heavy.base = 65, heavy.evd = 40, heavy.strength = 2;
  CHECK_EQ(hit_chance(heavy), 25);
  heavy.target_dizzy = true;
  CHECK_EQ(hit_chance(heavy), 45);
  heavy.target_dizzy = false, heavy.target_asleep = true;
  CHECK_EQ(hit_chance(heavy), 100);
}

// Darkness halves a weapon hit's chance when the active PC has it, whoever attacks: the PS1 (scene 6, Serge alone against two Komodo
// Pups, total EVD 40 poked on every unit) rolls a pup's weak hit on Serge at 59, at 39 = (99 - 40 / 2) / 2 once Serge is dark and dizzy,
// and at 99 against an EVD 0 Serge while both pups are dark.
void test_darkness_active_pc() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  Unit& pc = battle.unit_mut(0);
  Unit& enemy = battle.unit_mut(3);
  const int plain = battle.hit_chance_for(3, 0, 0), other = battle.hit_chance_for(3, 1, 0), own = battle.hit_chance_for(0, 3, 0);
  CHECK(plain > 1 && other > 1 && own > 1);
  CHECK_EQ(battle.active_pc(), 0);
  enemy.lo |= st::kDark;
  CHECK_EQ(battle.hit_chance_for(3, 0, 0), plain);
  enemy.lo &= ~st::kDark;
  pc.lo |= st::kDark;
  CHECK_EQ(battle.hit_chance_for(3, 0, 0), plain / 2);
  CHECK_EQ(battle.hit_chance_for(3, 1, 0), other / 2);
  CHECK_EQ(battle.hit_chance_for(0, 3, 0), own / 2);
  // Another PC's menu makes it the active PC: unit 0's darkness then counts for no hit, its own included.
  battle.open_menu(1);
  CHECK_EQ(battle.active_pc(), 1);
  CHECK_EQ(battle.hit_chance_for(3, 0, 0), plain);
  CHECK_EQ(battle.hit_chance_for(0, 3, 0), own);
  // A PC's command makes it the active PC too; op 2C leaves none.
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.target = 3;
  attack.chain = {0};
  fixture.rng.push(0);
  CHECK(battle.apply(attack));
  CHECK_EQ(battle.active_pc(), 0);
  battle.set_active_pc(-1);
  CHECK_EQ(battle.hit_chance_for(3, 0, 0), plain);
}

void test_status_message() {
  // pgdat +0x13 = the btmes row of the action-end system window, queued when the mode's status flag is set on a target;
  // Frozen newly landed on a target left alive = row 6 instead. Diminish (83) = mode 6 +0xE 1, row 54.
  const auto elem = [](Fixture& fixture, u8 mode, u8 row) {
    fixture.tables.elements.resize(5);
    fixture.tables.elements[4] = data::ElemRec{};
    fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = mode, fixture.tables.elements[4].raw[0x13] = row;
    return &fixture.tables.elements[4];
  };
  {
    // mode 6: the first cast starts the field state and shows the row; a second one while the state runs clears the status flag
    Fixture fixture;
    elem(fixture, 6, 54)->raw[0xE] = 1;
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK(!fixture.battle.events().empty());
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, 54);
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, -1);
  }
  {
    // mode 5 on a trap the side already holds and mode 12 below row 2 clear the status flag as well
    Fixture fixture;
    elem(fixture, 5, 54)->raw[0xE] = 9;
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, 54);
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, -1);
    elem(fixture, 12, 54);
    fixture.battle.do_element(3, 4, 1, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, -1);
    fixture.battle.do_element(3, 4, 2, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, 54);
  }
  {
    // mode 7: the status flag = the status roll (Goo: 20 %); failed -> code 2, no stat change, no window; landed -> power % and the row
    Fixture fixture;
    data::ElemRec* element = elem(fixture, 7, 37);
    // The word at +0x1C = 0x200 selects EVD%.
    element->raw[8] = 80, element->raw[0xA] = 20, element->raw[0x1D] = 0x02;
    const int mod0 = fixture.battle.unit(0).mod[3];
    fixture.rng.push(99);
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) {
      CHECK_EQ(fixture.battle.events().back().sys_msg, -1);
      CHECK_EQ(fixture.battle.events().back().code, 2);
    }
    CHECK_EQ(fixture.battle.unit(0).mod[3], mod0);
    fixture.rng.push(1);
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, 37);
    CHECK_EQ(fixture.battle.unit(0).mod[3], 80);
  }
  {
    // no row: no window; Frozen landing (lo 0x20000000) on a living target: row 6
    Fixture fixture;
    data::ElemRec* element = elem(fixture, 1, 0);
    element->raw[0xA] = 100, element->raw[0x1B] = 0x20;
    fixture.rng.push(1);
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, 6);
    CHECK((fixture.battle.unit(0).lo & 0x20000000u) != 0);
    element->raw[0x1B] = 0;
    fixture.battle.do_element(3, 4, 0, 0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().sys_msg, -1);
  }
}

void test_status_mode_codes() {
  // Mode 1: code 2 when the status roll fails, else 0. Mode 36 writes no code of its own: the target shows the code its last action left
  // (a caster's own slot reads 0xFF), and only a failed gate clears the landed flag (no record +0x13 row).
  Fixture fixture;
  Battle& battle = fixture.battle;
  fixture.tables.elements.resize(6);
  data::ElemRec& sleep = fixture.tables.elements[4];
  sleep = data::ElemRec{};
  sleep.raw[6] = 1, sleep.raw[7] = 1, sleep.raw[0xA] = 30, sleep.raw[0x19] = 0x40;
  data::ElemRec& heart = fixture.tables.elements[5];
  heart = data::ElemRec{};
  heart.raw[6] = 1, heart.raw[7] = 36, heart.raw[0xA] = 80, heart.raw[0x13] = 9;
  const auto last = [&]() { return battle.events().empty() ? ActionEvent{} : battle.events().back(); };
  fixture.rng.push(99);
  battle.do_element(3, 4, 0, 0);
  CHECK_EQ(last().code, 2);
  CHECK_EQ(battle.unit(0).result_code, 2);
  CHECK_EQ(battle.unit(3).result_code, 0xFF);
  fixture.rng.push(1);
  battle.do_element(3, 4, 0, 1);
  CHECK_EQ(last().code, 0);
  CHECK((battle.unit(1).lo & st::kAsleep) != 0);
  fixture.rng.push(0);
  battle.do_element(3, 5, 0, 0);
  CHECK_EQ(last().code, 2);
  CHECK_EQ(last().sys_msg, 9);
  fixture.rng.push(99);
  battle.do_element(3, 5, 0, 2);
  CHECK_EQ(last().code, 0);
  CHECK_EQ(last().sys_msg, -1);
  fixture.rng.push(0);
  battle.do_element(3, 5, 0, 3);
  CHECK_EQ(last().code, 0xFF);
}

struct AttackBrain final : EnemyBrain {
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.target = 0;
    return action;
  }
};

void test_frozen_turns() {
  // Frozen landing adds 3 to the countdown; the frozen unit stays out of alive() but its countdown runs, and at 0 an enemy takes its turn:
  // its first action ends the freeze. A frozen PC at 0 gets its script turn instead of the countdown reset; it passes.
  data::GameTables tables = make_tables();
  tables.elements.resize(5);
  data::ElemRec& ice = tables.elements[4];
  ice = data::ElemRec{};
  ice.raw[6] = 1, ice.raw[7] = 1, ice.raw[0xA] = 100, ice.raw[0x1B] = 0x20;
  // Two enemies, so the frozen one alone does not end the fight.
  tables.scenes[0].raw[3] = 2;
  ScriptedRng rng{Lo::Fallback::Lo};
  AttackBrain brain;
  Battle battle{tables, rng, brain};
  for (int k = 0; k < 2; ++k) {
    rng.push(0);
    rng.push(100);
  }
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  // The PS1 capture: Ice Blast on an enemy at countdown 1 -> 3 after the cast's own step, then 2 and 0 after a weak and a medium hit.
  battle.unit_mut(3).countdown = 1;
  rng.push(1);
  battle.do_element(0, 4, 0, 3);
  const Unit& enemy = battle.unit(3);
  CHECK((enemy.lo & st::kDefeated) != 0);
  CHECK(enemy.freeze_pending);
  CHECK_EQ(enemy.countdown, 4);
  CHECK(!enemy.alive() && enemy.in_turn_order());
  // The frozen unit's timers step with the party's actions like any unit's.
  const int frozen_timer = enemy.timers.at(29);
  battle.post_action(0, 1, 1);
  CHECK_EQ(enemy.timers.at(29), frozen_timer - 1);
  battle.step_after_pc_action(0, 1, false);
  CHECK_EQ(enemy.countdown, 3);
  battle.step_after_pc_action(0, 1, true);
  CHECK_EQ(enemy.countdown, 2);
  battle.step_after_pc_action(0, 2, true);
  CHECK_EQ(enemy.countdown, 0);
  CHECK((enemy.lo & st::kDefeated) != 0);
  const size_t events0 = battle.events().size();
  battle.settle();
  CHECK((enemy.lo & st::kDefeated) == 0);
  CHECK(!enemy.freeze_pending);
  CHECK(battle.events().size() > events0);
  if (battle.events().size() > events0) {
    CHECK_EQ(battle.events()[events0].actor, 3);
    CHECK(battle.events()[events0].kind == ActionEvent::Kind::Weapon);
  }
  rng.push(1);
  battle.do_element(3, 4, 0, 1);
  CHECK((battle.unit(1).lo & st::kDefeated) != 0);
  battle.unit_mut(1).countdown = 0;
  battle.unit_mut(3).countdown = 50;
  battle.unit_mut(4).countdown = 50;
  const int timer = battle.unit(1).timers.at(29);
  battle.settle();
  CHECK_EQ(battle.unit(1).countdown, battle.unit(1).ap);
  CHECK((battle.unit(1).lo & st::kDefeated) != 0);
  // The PS1: the frozen PC's turn passes (its script queues nothing), which steps none of its timers.
  CHECK_EQ(battle.unit(1).timers.at(29), timer);
}

void test_field_ring_writes() {
  const auto elem = [](Fixture& fixture, u8 flags, u8 mode, u8 colour) {
    fixture.tables.elements.resize(5);
    data::ElemRec& element = fixture.tables.elements[4];
    element = data::ElemRec{};
    element.raw[0] = flags, element.raw[5] = colour, element.raw[6] = 1, element.raw[7] = mode, element.raw[8] = 12, element.raw[0xC] = 20;
    return &element;
  };
  {
    // record byte 0 bit 0x80 (death casts, form changes) and element traps (mode 5) leave the ring; any other cast pushes its colour
    Fixture fixture;
    const std::array<int, 3> ring0 = fixture.battle.ring();
    elem(fixture, 0x81, 50, 0x20);
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK(fixture.battle.ring() == ring0);
    elem(fixture, 0x01, 5, 0x20)->raw[0xE] = 7;
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK(fixture.battle.ring() == ring0);
    elem(fixture, 0x01, 0, 0x20);
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK_EQ(fixture.battle.ring()[2], 3);
  }
  {
    // the ring slot of a colour byte: one bit -> its colour, all six (the Chrono Cross) -> the all-colours slot, none -> no colour
    CHECK_EQ(ring_colour_of(0x04), 0);
    CHECK_EQ(ring_colour_of(0x80), 5);
    CHECK_EQ(ring_colour_of(0x22), 3);
    CHECK_EQ(ring_colour_of(0xFC), kRingAllColours);
    CHECK_EQ(ring_colour_of(0x00), -1);
    CHECK_EQ(ring_colour_of(0x0C), -1);
    // an all-colour cast enters the ring as the all-colours slot (enemy caster or PC alike), which no colour multiplier counts
    Fixture fixture;
    elem(fixture, 0x01, 0, 0xFC);
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK_EQ(fixture.battle.ring()[2], kRingAllColours);
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.ring()[1], kRingAllColours);
    CHECK_EQ(fixture.battle.ring()[2], kRingAllColours);
    CHECK_EQ(colour_multiplier(3, 3, {kRingAllColours, kRingAllColours, kRingAllColours}, -1), colour_multiplier(3, 3, {-1, -1, -1}, -1));
    // a colourless enemy skill leaves an empty slot
    elem(fixture, 0x01, 0, 0x00);
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK_EQ(fixture.battle.ring()[2], -1);
  }
  {
    // the colour history takes every element cast (PC or enemy): all colours 0xFC = 6, no colour byte = 0, record byte 0 bit 0x80 = none
    Fixture fixture;
    elem(fixture, 0x01, 0, 0x20);
    CHECK_EQ(fixture.battle.seq_colour(4), 3);
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK(!fixture.battle.events().empty());
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().seq_colour, 3);
    elem(fixture, 0x01, 0, 0xFC);
    CHECK_EQ(fixture.battle.seq_colour(4), 6);
    elem(fixture, 0x01, 0, 0x00);
    CHECK_EQ(fixture.battle.seq_colour(4), 0);
    elem(fixture, 0x81, 50, 0x20);
    CHECK_EQ(fixture.battle.seq_colour(4), -1);
  }
  {
    // a colour field (mode 6, +0xE & 0xFC) repaints all three slots when the cast ends: every target of the cast still sees the old ring
    Fixture fixture;
    const std::array<int, 3> ring0 = fixture.battle.ring();
    elem(fixture, 0x08, 6, 0x80)->raw[0xE] = 0x80;
    fixture.battle.do_element(3, 4, 0, 0, false);
    fixture.battle.do_element(3, 4, 0, 1, false);
    CHECK(fixture.battle.ring() == ring0);
    fixture.battle.do_element(3, 4, 0, 2, true);
    CHECK(fixture.battle.ring() == (std::array<int, 3>{5, 5, 5}));
    for (const ActionEvent& event : fixture.battle.events()) CHECK(event.ring == ring0);
  }
  {
    // mode 47 draws rnd(0, 5) until a colour some party slot lacks (innate byte >> 2 as the colour mask), repaints the whole ring with it and
    // deals no damage
    Fixture fixture;
    elem(fixture, 0x01, 47, 0x00);
    for (int pc = 0; pc < kPcSlots; ++pc) fixture.battle.unit_mut(pc).innate = 0x1C;
    for (const int value : {0, 2, 1, 4, 5}) fixture.rng.push(value);
    const int hp0 = fixture.battle.unit(0).hp;
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK(fixture.battle.ring() == (std::array<int, 3>{4, 4, 4}));
    CHECK_EQ(fixture.battle.unit(0).hp, hp0);
    CHECK_EQ(fixture.rng.rnd(0, 5), 5);
  }
  {
    // innate byte bit 1: the colour mask is bits 2..7 and an element of that colour is absorbed
    Fixture fixture;
    elem(fixture, 0x01, 0, 0x20);
    fixture.battle.unit_mut(3).innate = 0x22;
    fixture.battle.unit_mut(3).hp = 100;
    fixture.battle.do_element(0, 4, 0, 3);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().code, 3);
    fixture.battle.unit_mut(3).innate = 0x20;
    fixture.battle.do_element(0, 4, 0, 3);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().code, 0);
    // the armour colour byte with bit 1 (the colour plates) absorbs the same way
    fixture.battle.unit_mut(3).eq.armour_colour = 0x22;
    fixture.battle.do_element(0, 4, 0, 3);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().code, 3);
  }
  {
    // absorb / immune: hi 0x4000 / 0x10000 for elements, 0x2000 / 0x8000 for weapons
    Fixture fixture;
    elem(fixture, 0x01, 0, 0x20);
    fixture.battle.unit_mut(3).hp = 100;
    fixture.battle.unit_mut(3).hi |= 0x2000;
    fixture.battle.do_element(0, 4, 0, 3);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().code, 0);
    fixture.battle.unit_mut(3).hi = (fixture.battle.unit(3).hi & ~0x2000u) | 0x4000u;
    fixture.battle.do_element(0, 4, 0, 3);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().code, 3);
    fixture.battle.unit_mut(3).hi = (fixture.battle.unit(3).hi & ~0x4000u) | 0x8000u;
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_weapon_attack(0, 3, 0);
    CHECK_EQ(fixture.battle.unit(3).hp, hp0);
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().code, 2);
  }
}

// The enemy casts one element at unit 0 on every turn.
struct CastBrain final : EnemyBrain {
  int element = 0;
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::Element;
    action.element = element;
    action.target = 0;
    return action;
  }
};

// A battle-script queue entry (op 67) aimed at several units: an element entry is one cast at all of them (each unit hit once, the field
// ring takes the colour once), a weapon entry attacks each unit in turn.
void test_queued_cast_targets() {
  struct QueueBrain final : EnemyBrain {
    std::vector<pac::QueuedAttack> queued;
    bool done = false;
    EnemyAction choose(const Battle&, int, Rng&) override {
      EnemyAction action;
      action.kind = EnemyAction::Kind::None;
      return action;
    }
    bool plan_turn(Battle& battle, int unit, Rng&, std::vector<EnemyAction>& out) override {
      if (is_pc_slot(unit) || done) return false;
      done = true;
      pac::ScriptedBrain::to_actions(battle, queued, out);
      return true;
    }
  };
  pac::QueuedAttack weapon;
  weapon.type = 0;
  weapon.value = 1;
  weapon.mask = 0x5;
  std::vector<EnemyAction> actions;
  data::GameTables plan_tables = make_tables();
  {
    ScriptedRng rng(Lo::Fallback::Lo);
    NullBrain brain;
    Battle battle(plan_tables, rng, brain);
    pac::QueuedAttack cast;
    cast.type = 1;
    cast.value = 1;
    cast.mask = 0x7;
    pac::ScriptedBrain::to_actions(battle, {cast, weapon}, actions);
  }
  CHECK_EQ(actions.size(), size_t{3});
  if (actions.size() == 3) {
    CHECK(actions[0].kind == EnemyAction::Kind::Element && actions[0].target == 0 && actions[0].targets == 0x7u);
    CHECK(actions[1].kind == EnemyAction::Kind::Weapon && actions[1].target == 0);
    CHECK(actions[2].kind == EnemyAction::Kind::Weapon && actions[2].target == 2);
  }
  // Element 1 aims at one unit, element 4 at its whole side: both hit each PC once and push the ring once.
  for (const int element_id : {1, 4}) {
    data::GameTables tables = make_tables();
    tables.elements.resize(5);
    tables.elements[4] = tables.elements[1];
    tables.elements[4].raw[4] = 0x10;
    ScriptedRng rng(Lo::Fallback::Lo);
    QueueBrain brain;
    pac::QueuedAttack cast;
    cast.type = 1;
    cast.value = element_id;
    cast.mask = 0x7;
    brain.queued = {cast};
    Battle battle(tables, rng, brain);
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A", 999), make_member(1, "B", 999), make_member(2, "C", 999)}, 0));
    for (int pc = 0; pc < kPcSlots; ++pc) battle.unit_mut(pc).stamina = 0;
    battle.unit_mut(3).countdown = 30;
    battle.settle();
    CHECK(brain.done);
    std::array<int, kPcSlots> hits{};
    for (const ActionEvent& event : battle.events())
      if (event.kind == ActionEvent::Kind::Element && event.actor == 3 && is_pc_slot(event.target)) ++hits[static_cast<size_t>(event.target)];
    CHECK(hits == (std::array<int, kPcSlots>{1, 1, 1}));
    CHECK(battle.ring() == (std::array<int, 3>{-1, -1, 5}));
  }
  // One turn queuing two element entries and two weapon entries: each element is its own action, so the second cast starts with the
  // first one's colour already in the ring; the weapon entries after them share one action.
  {
    data::GameTables tables = make_tables();
    tables.elements.resize(5);
    tables.elements[4] = tables.elements[1];
    tables.elements[4].raw[5] = 0x20;
    ScriptedRng rng(Lo::Fallback::Lo);
    QueueBrain brain;
    pac::QueuedAttack blue;
    blue.type = 1;
    blue.value = 1;
    blue.mask = 0x1;
    pac::QueuedAttack red = blue;
    red.value = 4;
    pac::QueuedAttack hit = weapon;
    hit.mask = 0x1;
    brain.queued = {blue, red, hit, hit};
    Battle battle(tables, rng, brain);
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A", 999), make_member(1, "B", 999), make_member(2, "C", 999)}, 0));
    for (int pc = 0; pc < kPcSlots; ++pc) battle.unit_mut(pc).stamina = 0;
    battle.unit_mut(3).countdown = 30;
    battle.settle();
    std::vector<ActionEvent> turn;
    for (const ActionEvent& event : battle.events())
      if (event.actor == 3 && event.kind != ActionEvent::Kind::Defend) turn.push_back(event);
    CHECK_EQ(turn.size(), size_t{4});
    if (turn.size() == 4) {
      CHECK(turn[0].element == 1 && turn[1].element == 4);
      CHECK(turn[1].action != turn[0].action && turn[2].action != turn[1].action && turn[3].action == turn[2].action);
      CHECK(turn[0].ring == (std::array<int, 3>{-1, -1, -1}));
      CHECK(turn[1].ring == (std::array<int, 3>{-1, -1, 5}));
    }
    CHECK(battle.ring() == (std::array<int, 3>{-1, 5, 3}));
  }
}

// The element-trap HUD block: hidden while the trap cast runs, shown from its end; the action that springs the trap still shows it, and
// the block is hidden once that action has ended.
void test_trap_hud() {
  data::GameTables tables = make_tables();
  tables.elements.resize(5);
  data::ElemRec& trap_rec = tables.elements[4];
  trap_rec.raw[5] = 0x20, trap_rec.raw[6] = 1, trap_rec.raw[7] = 5, trap_rec.raw[8] = 12, trap_rec.raw[0xC] = 20, trap_rec.raw[0xE] = 2;
  ScriptedRng rng{Lo::Fallback::Lo};
  CastBrain brain;
  brain.element = 2;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  PartyMember trapper = make_member(0, "A");
  trapper.cells[0] = 0x4004;
  CHECK(battle.start({trapper, make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(battle.trap_hud() == TrapHud{});
  battle.unit_mut(0).gauge = 2;
  Command cast;
  cast.kind = Command::Kind::Element;
  cast.actor = 0;
  cast.cell = 0;
  std::string why;
  CHECK(battle.apply(cast, &why));
  CHECK(!battle.events().empty());
  if (!battle.events().empty()) {
    CHECK(battle.events().back().trap == (TrapHud{false, 2, 0}));
    CHECK(battle.events().back().trap_before == TrapHud{});
  }
  CHECK(battle.trap_hud() == (TrapHud{true, 2, 0}));
  // the enemy casts the trapped element: caught, the block shows through that action, then hides
  battle.unit_mut(0).stamina = 0;
  battle.unit_mut(1).stamina = 0;
  battle.unit_mut(2).stamina = 0;
  battle.unit_mut(3).countdown = 30;
  battle.set_ring({0, 1, 2});
  const size_t before = battle.events().size();
  battle.settle();
  CHECK(battle.events().size() > before);
  if (battle.events().size() > before) {
    const ActionEvent& caught = battle.events()[before];
    CHECK_EQ(caught.element, 2);
    CHECK_EQ(caught.code, 2);
    CHECK(caught.trap == (TrapHud{true, 2, 0}));
    CHECK(caught.trap_before == (TrapHud{true, 2, 0}));
    // no number, and the caught red cast still enters the ring
    CHECK(caught.caught && caught.silent);
    CHECK(caught.ring == (std::array<int, 3>{0, 1, 2}));
  }
  CHECK(battle.ring() == (std::array<int, 3>{1, 2, 3}));
  CHECK(battle.trap_hud() == TrapHud{});
  CHECK_EQ(battle.rewards().trapped.size(), size_t{1});
}

// The trap tests only an element with a colour (record byte 5 bits 0xFC): a colourless element is not caught and the trap stays set,
// for a party cast and an enemy cast alike; the two low bits of byte 5 do not count as a colour.
void test_trap_needs_colour() {
  data::GameTables tables = make_tables();
  tables.elements.resize(5);
  data::ElemRec& colourless = tables.elements[4];
  colourless = tables.elements[1];
  colourless.raw[5] = 0x03;
  ScriptedRng rng{Lo::Fallback::Lo};
  CastBrain brain;
  Battle battle{tables, rng, brain};
  PartyMember caster = make_member(0, "A");
  caster.cells[0] = 0x4004;
  CHECK(battle.start({caster, make_member(1, "B"), make_member(2, "C")}, 0));
  battle.set_trap(true, 4);
  CHECK(!battle.catch_in_trap(0, 4, 0, 3));
  CHECK_EQ(battle.trap_hud().enemy, 4);
  battle.set_trap(false, 4);
  CHECK(!battle.catch_in_trap(3, 4, 0, 0));
  CHECK_EQ(battle.trap_hud().pc, 4);
  CHECK(battle.rewards().trapped.empty());
  // the uncaught colourless cast runs and pushes an empty slot into the field ring
  battle.set_ring({0, 1, 2});
  battle.set_trap(true, 4);
  const size_t before = battle.events().size();
  battle.unit_mut(0).gauge = 2;
  Command cast;
  cast.kind = Command::Kind::Element;
  cast.actor = 0;
  cast.cell = 0;
  std::string why;
  CHECK(battle.apply(cast, &why));
  CHECK(battle.events().size() > before);
  if (battle.events().size() > before) CHECK(!battle.events()[before].caught);
  CHECK(battle.ring() == (std::array<int, 3>{1, 2, -1}));
  CHECK_EQ(battle.trap_hud().enemy, 4);
  battle.set_trap(true, 2);
  CHECK(battle.catch_in_trap(0, 2, 0, 3));
  CHECK_EQ(battle.trap_hud().enemy, 0);
  battle.set_trap(false, 2);
  CHECK(battle.catch_in_trap(3, 2, 0, 0));
  CHECK_EQ(battle.trap_hud().pc, 0);
  CHECK_EQ(battle.rewards().trapped.size(), size_t{1});
}

void test_pilfer() {
  // Mode 30: the chance byte = the caster's raw heavy-hit byte on a fresh target (20 here), doubled (byte arithmetic) for the opposite
  // gender (PC 0 vs the male enemy); rnd(0,100) >= chance fails (code 2); rnd(0,99) < 10 takes slot +6 (rare) else +4; one steal per
  // enemy; both slots empty -> nothing.
  const auto chance_of = [](const Fixture& fixture) { return static_cast<int>(static_cast<u8>(fixture.battle.unit(0).hit_heavy * 2)); };
  {
    Fixture fixture;
    CHECK_EQ(chance_of(fixture), 40);
  }
  {
    // The chance bytes the PS1 rolled against for Kid (heavy-hit byte 20, weapon accuracy 89: heavy HIT% 65) on a female target with no
    // evade and, poked, a male one: a fresh target 20 / 40; after one weak hit on it (chain counter 1) the box figure 72 / 144.
    struct Case {
      bool chained, male;
      int chance;
    };
    for (const Case& c : {Case{false, false, 20}, Case{false, true, 40}, Case{true, false, 72}, Case{true, true, 144}}) {
      // rnd(0, 100) never reaches a chance above 100.
      for (const int roll : {std::min(c.chance - 1, 100), c.chance}) {
        if (roll > 100) continue;
        Fixture fixture;
        fixture.tables.elements.resize(5);
        fixture.tables.elements[4].raw[5] = 0x80, fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = 30;
        fixture.tables.drops[0].slot = {0x0005, 0x8069, 0x0007, 0x8061};
        Unit& kid = fixture.battle.unit_mut(0);
        kid.gender = 0x8000, kid.hit_heavy = 20, kid.eq.wp_acc = 89;
        Unit& enemy = fixture.battle.unit_mut(3);
        enemy.gender = c.male ? 0x4000 : 0x8000, enemy.evd = 0;
        if (c.chained) fixture.battle.do_weapon_attack(0, 3, 0);
        fixture.rng.push(roll);
        fixture.rng.push(50);
        fixture.battle.do_element(0, 4, 0, 3);
        CHECK_EQ(fixture.battle.unit(3).stolen, roll < c.chance);
      }
    }
  }
  {
    // The PS1 after a weak hit and a failed menu Pilfer: the cast leaves the chain counter at 1, so a Pilfer right after still reads 72
    // (a chain step would give 78); the next turn starts the chain at 0 and the Pilfer reads the raw byte 20.
    for (const bool next_turn : {false, true}) {
      const int chance = next_turn ? 20 : 72;
      for (const int roll : {chance - 1, chance}) {
        Fixture fixture;
        fixture.tables.elements.resize(5);
        fixture.tables.elements[4].raw[5] = 0x80, fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = 30;
        fixture.tables.drops[0].slot = {0x0005, 0x8069, 0x0007, 0x8061};
        Unit& kid = fixture.battle.unit_mut(0);
        kid.gender = 0x8000, kid.hit_heavy = 20, kid.eq.wp_acc = 89;
        fixture.battle.unit_mut(3).gender = 0x8000, fixture.battle.unit_mut(3).evd = 0;
        fixture.battle.do_weapon_attack(0, 3, 0);
        fixture.rng.push(100);
        fixture.battle.do_element(0, 4, 0, 3);
        CHECK(!fixture.battle.unit(3).stolen);
        if (next_turn) fixture.battle.finish_external_action();
        fixture.rng.push(roll);
        fixture.rng.push(50);
        fixture.battle.do_element(0, 4, 0, 3);
        CHECK_EQ(fixture.battle.unit(3).stolen, roll < chance);
      }
    }
  }
  {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    fixture.tables.elements[4].raw[5] = 0x80, fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = 30;
    fixture.tables.drops[0].slot = {0x0005, 0x8069, 0x0007, 0x8061};
    const int chance = chance_of(fixture);
    // The roll passes the chance; 5 < 10 picks the rare slot.
    fixture.rng.push(chance - 1);
    fixture.rng.push(5);
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK(fixture.battle.unit(3).stolen);
    CHECK_EQ(fixture.battle.rewards().stolen.size(), 1u);
    if (!fixture.battle.rewards().stolen.empty()) CHECK_EQ(fixture.battle.rewards().stolen[0], 0x8061);
    // the event carries the result window's row (btmes 1 = element) and the word, code 0
    CHECK(!fixture.battle.events().empty());
    if (!fixture.battle.events().empty()) {
      CHECK_EQ(fixture.battle.events().back().steal_msg, 1);
      CHECK_EQ(fixture.battle.events().back().steal_word, 0x8061);
      CHECK_EQ(fixture.battle.events().back().code, 0);
    }
    // A second try on the same enemy: nothing left, no roll.
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.rewards().stolen.size(), 1u);
    // btmes 3 'Nothing to steal!', the MISS result code.
    if (!fixture.battle.events().empty()) {
      CHECK_EQ(fixture.battle.events().back().steal_msg, 3);
      CHECK_EQ(fixture.battle.events().back().code, 2);
    }
  }
  {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    fixture.tables.elements[4].raw[5] = 0x80, fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = 30;
    fixture.tables.drops[0].slot = {0x0005, 0x8069, 0x0007, 0x8061};
    // A roll >= the chance: the steal fails, the enemy keeps its loot.
    fixture.rng.push(chance_of(fixture));
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK(!fixture.battle.unit(3).stolen && fixture.battle.rewards().stolen.empty());
    // btmes 2 'Attempt at thievery failed!'.
    if (!fixture.battle.events().empty()) {
      CHECK_EQ(fixture.battle.events().back().steal_msg, 2);
      CHECK_EQ(fixture.battle.events().back().code, 2);
    }
    fixture.rng.push(0);
    // 10 is not < 10: the common slot.
    fixture.rng.push(10);
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.rewards().stolen.size(), 1u);
    if (!fixture.battle.rewards().stolen.empty()) CHECK_EQ(fixture.battle.rewards().stolen[0], 0x0007);
    // btmes 0 = an item.
    if (!fixture.battle.events().empty()) {
      CHECK_EQ(fixture.battle.events().back().steal_msg, 0);
      CHECK_EQ(fixture.battle.events().back().steal_word, 0x0007);
    }
  }
  {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    fixture.tables.elements[4].raw[5] = 0x80, fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = 30;
    // An empty steal row.
    fixture.tables.drops[0].slot = {0x0005, 0x8069, 0, 0};
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK(!fixture.battle.unit(3).stolen && fixture.battle.rewards().stolen.empty());
    if (!fixture.battle.events().empty()) CHECK_EQ(fixture.battle.events().back().steal_msg, 3);
  }
}

// A script-forced victory (op 83 result 0) pays gold and drops for the units in the defeated mask only (the Boxer Boys right pick):
// the holder marked by op 8C, not the boxer still standing.
struct ForcedWinBrain final : EnemyBrain {
  EnemyAction choose(const Battle&, int, Rng&) override { return {}; }
  int forced_result() const override { return 0; }
  bool defeated(int unit) const override { return unit == 3; }
};

void test_forced_victory_pays_defeated_only() {
  data::GameTables tables = make_tables();
  // Two enemies of group 0 (slots 3 and 4).
  tables.scenes[0].raw[3] = 2;
  ScriptedRng rng{Lo::Fallback::Lo};
  ForcedWinBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(battle.unit(3).present && battle.unit(4).present);
  battle.finish_external_action();
  CHECK(battle.outcome() == Outcome::Victory);
  CHECK_EQ(battle.result_code(), 0);
  // Unit 3 only (unit 4 still stands, not in the mask).
  CHECK_EQ(battle.rewards().gold, 25);
  CHECK_EQ(battle.rewards().elements.size() + battle.rewards().items.size(), 1u);
}

void test_victory_and_growth() {
  // normal victory: gold and the common drop slot (roll 0 < 150/20 -> rare slot 1 = element 0x69)
  {
    Fixture fixture;
    fixture.battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    // crit/damage rolls fall back to Lo; the drop roll is the last call -> script it by pushing after the fight
    CHECK(fixture.battle.apply(attack));
    CHECK(fixture.battle.outcome() == Outcome::Victory);
    CHECK_EQ(fixture.battle.rewards().gold, 25);
    // Lo -> r=0 -> rare slot 1.
    CHECK(fixture.battle.rewards().elements.size() + fixture.battle.rewards().items.size() == 1);
    CHECK_EQ(fixture.battle.rewards().elements[0], 0x69);
    // minor growth with progress: P = min(0 + 1, 8) = 1 of 8, pending 0 -> nothing to grow towards
    CHECK_EQ(fixture.battle.growth().progress, 1);
    CHECK_EQ(fixture.battle.rewards().gains[0][1], 0);
  }
  // boss victory: pending is granted in full but never past Lv99; star + counter advance; progress resets
  {
    Fixture fixture;
    fixture.tables.scenes[0].raw[0x1A] = 0x80;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(fixture.tables, rng, nb);
    auto member = make_member(0, "A");
    member.pending = {20, 100, 3, 0, 0, 0, 0};
    // ATK 9 + 100 pending -> capped at 60.
    member.lv99 = {850, 60, 78, 95, 25, 52, 43};
    CHECK(battle.start({member}, 0));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK(battle.rewards().star_up);
    CHECK_EQ(battle.rewards().gains[0][0], 20);
    CHECK_EQ(battle.rewards().gains[0][1], 51);
    CHECK_EQ(battle.party()[0].stat[1], 60);
    CHECK_EQ(battle.party()[0].stat[2], 8);
    CHECK_EQ(battle.growth().star, 1);
    CHECK_EQ(battle.growth().counter, 1);
    CHECK_EQ(battle.growth().progress, 0);
    CHECK(battle.party()[0].snap == battle.party()[0].stat);
    // At the Lv99 cap: nothing more to roll.
    CHECK(battle.party()[0].pending[1] == 0);
  }
  // non-boss victory with pending deltas: ATK 9, snap 9, pending 20, b=7: aim 13, (9+13-9)*sqrt(1/8)*0.5 = 2.30 -> 3
  {
    Fixture f2;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(f2.tables, rng, nb);
    auto member = make_member(0, "A");
    member.pending = {0, 20, 0, 0, 0, 0, 0};
    member.lv99 = {850, 60, 78, 95, 25, 52, 43};
    CHECK(battle.start({member}, 0));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK(!battle.rewards().star_up);
    CHECK_EQ(battle.rewards().gains[0][1], 3);
    CHECK_EQ(battle.party()[0].stat[1], 12);
    // The snapshot only moves on a star-up.
    CHECK_EQ(battle.party()[0].snap[1], 9);
  }
  // instant growth: one normal victory takes ATK the whole way to its target 9 + 13 = 22, not half of it
  {
    Fixture fixture;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(fixture.tables, rng, nb);
    battle.set_cheats({.instant_growth = true});
    auto member = make_member(0, "A");
    member.pending = {0, 20, 0, 0, 0, 0, 0};
    member.lv99 = {850, 60, 78, 95, 25, 52, 43};
    CHECK(battle.start({member}, 0));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK_EQ(battle.growth().progress, 8);
    CHECK_EQ(battle.rewards().gains[0][1], 13);
    CHECK_EQ(battle.party()[0].stat[1], 22);
  }
  // max growth: a member that missed every earlier star (still at its starting stats at star 6) is raised to a typical record of
  // star 6 by one normal win, and the raise shows as the fight's gain; a member already above it keeps its stats
  for (const bool behind : {true, false}) {
    Fixture fixture;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(fixture.tables, rng, nb);
    battle.set_cheats({.instant_growth = true});
    auto member = make_member(0, "A");
    member.lv99 = {850, 60, 78, 95, 25, 52, 43};
    member.pending = {};
    member.base = member.stat;
    if (!behind) member.stat = member.snap = member.lv99;
    const std::array<int, 7> before = member.stat;
    BattleOptions opt;
    opt.growth.star = 6;
    CHECK(battle.start({member}, 0, opt));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    for (size_t k = 0; k < 7; ++k) {
      const int after = battle.party()[0].stat[k];
      CHECK(after >= before[k] && after <= member.lv99[k]);
      CHECK_EQ(battle.rewards().gains[0][k], after - before[k]);
      if (!behind) CHECK_EQ(after, before[k]);
    }
    if (behind) CHECK(battle.party()[0].stat[0] > before[0] && battle.party()[0].stat[1] > before[1]);
  }
  // shared growth: without it a KO'd member and the bench get nothing from a normal win; with it (and instant growth) they reach the target
  for (const bool shared : {false, true}) {
    Fixture fixture;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(fixture.tables, rng, nb);
    battle.set_cheats({.instant_growth = true, .shared_growth = shared});
    auto lead = make_member(0, "A"), ko = make_member(1, "B"), bench = make_member(9, "Z");
    for (auto* member : {&lead, &ko, &bench}) {
      member->pending = {0, 20, 0, 0, 0, 0, 0};
      member->lv99 = {850, 60, 78, 95, 25, 52, 43};
    }
    ko.hp = 0;
    BattleOptions opt;
    opt.bench = {bench};
    CHECK(battle.start({lead, ko}, 0, opt));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK_EQ(battle.party()[0].stat[1], 22);
    CHECK_EQ(battle.rewards().gains[1][1], shared ? 13 : 0);
    CHECK_EQ(battle.party()[1].stat[1], shared ? 22 : 9);
    CHECK_EQ(battle.bench()[0].stat[1], shared ? 22 : 9);
    CHECK_EQ(battle.rewards().bench_gains.size(), shared ? 1u : 0u);
    // The host writes the shared growth back only for a recruited character; one not yet recruited keeps its record.
    for (const bool recruited : {false, true}) {
      ui::GameState state;
      state.slot.chars[9].atk = 9;
      state.slot.set_recruited(9, recruited);
      game::apply_battle_result(state, fixture.tables, battle);
      CHECK_EQ(state.slot.chars[9].atk, shared && recruited ? 22 : 9);
    }
  }
  // star-up: KO'd member gets no gain and 1 HP but does receive next pending; bench (non-party) member gets its old pending
  {
    Fixture f2;
    f2.tables.scenes[0].raw[0x1A] = 0x80;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(f2.tables, rng, nb);
    auto lead = make_member(0, "A"), ko = make_member(1, "B"), bench = make_member(9, "Z");
    for (auto* member : {&lead, &ko, &bench}) member->lv99 = {850, 60, 78, 95, 25, 52, 43};
    lead.pending = {10, 3, 0, 0, 0, 0, 0};
    ko.pending = {10, 3, 0, 0, 0, 0, 0};
    ko.hp = 0;
    bench.pending = {7, 4, 0, 0, 0, 0, 0};
    BattleOptions opt;
    opt.bench = {bench};
    CHECK(battle.start({lead, ko}, 0, opt));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK(battle.rewards().star_up);
    CHECK_EQ(battle.rewards().gains[0][0], 10);
    // KO'd: nothing. KO'd members come back with exactly 1 HP; survivors keep their current HP (no refill on exit).
    CHECK_EQ(battle.rewards().gains[1][0], 0);
    CHECK_EQ(battle.rewards().gains[1][1], 0);
    CHECK_EQ(battle.party()[1].hp, 1);
    CHECK_EQ(battle.party()[0].hp, 50);
    CHECK_EQ(battle.bench().size(), 1u);
    CHECK_EQ(battle.rewards().bench_gains[0][0], 7);
    CHECK_EQ(battle.bench()[0].stat[0], 57);
    CHECK_EQ(battle.bench()[0].stat[1], 13);
    for (const PartyMember* member : {&battle.party()[0], &battle.party()[1], &battle.bench()[0]}) {
      CHECK(member->snap == member->stat);
      // Fresh HP pending (roll + 2) for all three, KO'd and bench included.
      CHECK(member->pending[0] >= 2);
    }
  }
  // a member still frozen when the fight ends is not knocked out: it takes the minor growth and the star-up gains
  for (const bool boss : {false, true}) {
    Fixture f2;
    if (boss) f2.tables.scenes[0].raw[0x1A] = 0x80;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(f2.tables, rng, nb);
    auto lead = make_member(0, "A"), frozen = make_member(1, "B");
    for (auto* member : {&lead, &frozen}) member->pending = {10, 20, 0, 0, 0, 0, 0};
    CHECK(battle.start({lead, frozen}, 0));
    battle.unit_mut(1).lo |= st::kDefeated;
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK(battle.unit(1).lo & st::kDefeated);
    CHECK_EQ(battle.rewards().gains[1][1], battle.rewards().gains[0][1]);
    CHECK_EQ(battle.rewards().gains[1][1], boss ? 20 : 3);
  }
  // the star-up that reaches 99: the party takes its pending gains, the bench keeps its stats, nobody is rolled new deltas
  {
    Fixture f2;
    f2.tables.scenes[0].raw[0x1A] = 0x80;
    ScriptedRng rng(Lo::Fallback::Lo);
    rng.push(0);
    rng.push(100);
    NullBrain nb;
    Battle battle(f2.tables, rng, nb);
    auto lead = make_member(0, "A"), bench = make_member(9, "Z");
    lead.pending = {10, 3, 0, 0, 0, 0, 0};
    bench.pending = {7, 4, 0, 0, 0, 0, 0};
    BattleOptions opt;
    opt.bench = {bench};
    opt.growth.star = 98;
    CHECK(battle.start({lead}, 0, opt));
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK(battle.rewards().star_up);
    CHECK_EQ(battle.growth().star, 99);
    CHECK_EQ(battle.growth().counter, 1);
    CHECK_EQ(battle.rewards().gains[0][0], 10);
    CHECK_EQ(battle.party()[0].stat[0], 60);
    CHECK(battle.party()[0].pending == lead.pending);
    CHECK(battle.party()[0].snap == lead.snap);
    CHECK_EQ(battle.rewards().bench_gains[0][0], 0);
    CHECK(battle.bench()[0].stat == bench.stat);
    CHECK(battle.bench()[0].pending == bench.pending);
  }
  // defeat and run
  {
    Fixture fixture;
    Command run;
    run.kind = Command::Kind::Run;
    run.actor = 0;
    CHECK(fixture.battle.apply(run));
    CHECK(fixture.battle.outcome() == Outcome::Fled);
    CHECK(!fixture.battle.apply(run));
  }
}

void test_determinism() {
  data::GameTables tables = make_tables();
  tables.enemies[0].raw[0x16] = 200;
  tables.enemies[0].raw[0x17] = 0;
  auto run = [&](u32 seed) {
    GameRng rng(seed);
    RandomBrain brain;
    Battle battle(tables, rng, brain);
    battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0);
    int cmds = 0;
    while (battle.outcome() == Outcome::Ongoing && cmds < 500) {
      int pc = -1;
      for (int i = 0; i < 3 && pc < 0; ++i)
        if (battle.can_act(i)) pc = i;
      if (pc < 0) break;
      if (!battle.apply(auto_command(battle, pc))) break;
      ++cmds;
    }
    std::string log_text;
    for (const auto& line : battle.log()) log_text += line + "\n";
    return std::make_pair(static_cast<int>(battle.outcome()), log_text);
  };
  const auto first = run(42), second = run(42), other_seed = run(43);
  CHECK(first.first == 0 && first.second == second.second);
  CHECK(other_seed.first == 0);
}

void test_gear() {
  data::GameTables tables = make_tables();
  tables.gear.resize(256);
  auto set = [&](int id, std::initializer_list<u8> bytes) {
    size_t i = 0;
    for (u8 byte : bytes) tables.gear[static_cast<size_t>(id)].raw[i++] = byte;
  };
  // Sea Swallow (0x20), Ivory Mail (0x96), Power Glove (0xC8, an offence accessory), Knee Pad (0xD1, a defence accessory).
  set(0x20, {0, 1, 0x10, 0, 8, 0, 0x59, 0x14});
  set(0x96, {0, 0, 0, 0, 3, 1, 0, 5});
  set(0xC8, {0, 0, 0, 0, 1, 0, 0, 0, 0x20});
  set(0xD1, {0, 0, 0, 0, 1, 0, 0, 0, 0x40});
  Equip equip = make_equip(tables, 0x20, 0x96, {0xC8, 0xD1, 0});
  CHECK_EQ(equip.wp, 9);
  CHECK_EQ(equip.wp_acc, 0x59);
  CHECK_EQ(equip.wp_var, 0x14);
  CHECK_EQ(equip.arm, 4);
  CHECK_EQ(equip.arm_var, 5);
  CHECK_EQ(equip.mdef_gear, 1);
  CHECK_EQ(equip.wp_class, 1);
  CHECK_EQ(hit_base(85, equip.wp_acc), 85 / 3 + 89 * 200 / 300);
  // Without a gear table the flat defaults apply.
  CHECK_EQ(make_equip(make_tables(), 0x20, 0x96, {0, 0, 0}).wp, 16);
  // The accessory ids are kept whether or not the weapon and armour resolve (the Dopplegang 0xFC and 0xF9..0xFB tests read them).
  const Equip unresolved = make_equip(tables, 0x1000, 0x1000, {0xFC, 0xF9, 0});
  CHECK(unresolved.acc[0] == 0xFC && unresolved.acc[1] == 0xF9);
  CHECK_EQ(make_equip(make_tables(), 0x20, 0x96, {0xFB, 0, 0}).acc[0], 0xFB);
  // Flag-0x80 accessories: +5 adds to the stamina modifier, +4 to the duration modifier (below 0x50 added, else v & 0x7F taken off):
  // Resistance Ring 0x81 = -1, Resistance Belt 0x82 = -2, a made-up 0x03 = +3.
  set(0xE4, {0, 0, 0, 0, 0x81, 0, 0, 0, 0x80});
  set(0xE5, {0, 0, 0, 0, 0x82, 0, 0, 0, 0x80});
  set(0xE6, {0, 0, 0, 0, 0x03, 2, 0, 0, 0x80});
  CHECK_EQ(make_equip(tables, 0x20, 0x96, {0xE4, 0, 0}).dur_mod, -1);
  CHECK_EQ(make_equip(tables, 0x20, 0x96, {0xE5, 0, 0}).dur_mod, -2);
  const Equip mixed = make_equip(tables, 0x20, 0x96, {0xE4, 0xE5, 0xE6});
  CHECK(mixed.dur_mod == 0 && mixed.stamina_mod == 2);
}

// The PS1 capture of Turn Red (hi 0x08, 100 %) cast by Serge (duration stat 10) on himself: the timer after the cast's own step is 29
// with no flag-0x80 accessory, 26 with the Resistance Ring (+4 = 0x81) and 23 with the Resistance Belt (0x82).
void test_gear_duration_modifier() {
  for (const auto& [acc, timer] : {std::pair{0, 29}, std::pair{0xE4, 26}, std::pair{0xE5, 23}}) {
    data::GameTables tables = make_tables();
    tables.gear.resize(256);
    tables.gear[0x20].raw[4] = 8;
    tables.gear[0xE4].raw[4] = 0x81, tables.gear[0xE4].raw[8] = 0x80;
    tables.gear[0xE5].raw[4] = 0x82, tables.gear[0xE5].raw[8] = 0x80;
    tables.elements.resize(5);
    auto& turn_red = tables.elements[4];
    turn_red.raw[5] = 0x20, turn_red.raw[6] = 1, turn_red.raw[7] = 1, turn_red.raw[0xA] = 100, turn_red.raw[0x1C] = 0x08;
    ScriptedRng rng{Lo::Fallback::Lo};
    NullBrain brain;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    PartyMember serge = make_member(0, "Serge");
    serge.dur = 10;
    serge.eq = make_equip(tables, 0x20, 0x96, {static_cast<u16>(acc), 0, 0});
    CHECK(battle.start({serge, make_member(1, "B"), make_member(2, "C")}, 0));
    battle.do_element(0, 4, 0, 0);
    battle.post_action(0, 0, 1);
    CHECK(battle.unit(0).timers.count(35) == 1 && battle.unit(0).timers.at(35) == timer);
  }
}

// ------------------------------------------------------------------------------------------------ battle script VM
// A hand-assembled .pac: 5 entities (0 global, 1-3 PCs, 4 = monster unit 3). Entity 4: slot 0 binds unit 3, slot 2 =
//   set v406=3+value; sub v406,3; [2B v408 = target code `select`]; dialogue 7; 66; 87; 67 type (0 = weapon) strength / element v406 target
//   mask 1 (PC1); A4; halt. With `pick` the 67 is replaced by 55 (element v408, target mask v40A).
Bytes assemble_pac(int type = 0, int value = 0, int select = -1, bool pick = false) {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  const int idle = 1, init4 = 2;
  // rel 0: entity 0 slot 0; rel 1: idle loop (slot 1 of every entity); rel 2: bind unit 3, halt.
  emit({0x00});
  emit({0x00});
  emit({0x0E, 0xE3, 0x80, 0x00});
  const int slot2 = static_cast<int>(code.size());
  // set 0x406 = 3 + value; sub 0x406 -= 3; dialogue 7; begin action, clear queue;
  // queue: type imm, strength / element var 0x406, mask imm 1; end turn, halt.
  emit({0x10, 0x06, 0x04, 3 + value, 0x00, 0x40});
  emit({0x14, 0x06, 0x04, 0x03, 0x00, 0x40});
  if (select >= 0) emit({0x2B, 0x08, 0x04, select, 0x00, 0x40});
  emit({0xE0, 0x07, 0x00, 0x00});
  emit({0x66, 0x87});
  if (pick) emit({0x55, 0x08, 0x04, 0x0A, 0x04});
  else emit({0x67, type, 0x00, 0x06, 0x04, 0x01, 0x00, 0xA0});
  emit({0xA4, 0x00});
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  // Slot 1 of every entity is the idle loop.
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, idle);
  put16(0x94 + 4 * 0x40 + 0, init4);
  put16(0x94 + 4 * 0x40 + 4, slot2);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

// alive covers PCs 0-2 and enemy unit 3 (roster: also the KO'd PCs; ko: the units whose status word reads KO); rnd() returns the queued
// rolls, then lo; libc_rand() gives op 1F: 7 % (B + 1).
struct FakeHost final : pac::Host {
  u32 alive = 0xF, roster = 0xF, ko = 0;
  std::vector<int> rolls;
  int rnd_calls = 0;
  u32 alive_mask() const override { return alive; }
  u32 roster_mask() const override { return roster; }
  int read_field(int unit, u16 code) override { return code == 0xB0 && (ko >> unit & 1u) ? static_cast<int>(st::kKO) : 0; }
  void write_field(int, u16, int) override {}
  int rnd(int lo, int) override {
    const size_t next = static_cast<size_t>(rnd_calls++);
    return next < rolls.size() ? rolls[next] : lo;
  }
  int libc_rand() override { return 7; }
};

void test_pac_vm() {
  CHECK_EQ(pac::op_info(0x67).len, 8);
  CHECK_EQ(pac::op_info(0x02).len, 8);
  CHECK_EQ(pac::op_info(0xA4).conf, 'C');
  pac::Module module;
  std::string err;
  CHECK(module.parse(assemble_pac(), &err));
  CHECK_EQ(module.entities, 5);
  FakeHost host;
  pac::Vm vm(module, host);
  // The first tick runs init.
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.entity_of_unit(3), 4);
  CHECK(vm.has_slot(3, 2));
  CHECK(!vm.has_slot(3, 4));
  // A4 blocks until the executor has consumed the queue
  CHECK(vm.launch(3, 2));
  CHECK_EQ(vm.lock(), 3);
  vm.tick();
  CHECK_EQ(vm.queue(3).size(), 1u);
  CHECK_EQ(vm.frame_count(4), 2);
  vm.tick();
  // Still waiting.
  CHECK_EQ(vm.frame_count(4), 2);
  vm.queue(3).clear();
  vm.tick();
  // A4 passed; halt returned to the idle loop.
  CHECK_EQ(vm.frame_count(4), 1);
  CHECK_EQ(vm.lock(), pac::kNoUnit);
  CHECK_EQ(vm.var(0x406), 0);
  const auto ev = vm.drain_events();
  CHECK(!ev.empty() && ev[0].kind == pac::Event::Kind::Dialogue && ev[0].a == 7);

  std::vector<pac::QueuedAttack> queued;
  CHECK(vm.run_slot(3, 2, queued));
  CHECK_EQ(queued.size(), 1u);
  CHECK_EQ(queued[0].type, 0);
  // A weak attack on PC1.
  CHECK_EQ(queued[0].value, 0);
  CHECK_EQ(queued[0].mask, 1u);
  // dead PC1 in the mask: nothing alive in it -> falls back to PC1 anyway (types 0/5)
  host.alive = 0xE;
  queued.clear();
  CHECK(vm.run_slot(3, 2, queued));
  CHECK_EQ(queued[0].mask, 1u);

  // An element (type 1) keeps a KO'd PC that is still in the roster (Revive on the fallen PC); only a mask with no roster unit falls
  // back to the caster.
  pac::Module element_module;
  CHECK(element_module.parse(assemble_pac(1), &err));
  FakeHost element_host;
  pac::Vm element_vm(element_module, element_host);
  element_vm.tick();
  element_vm.tick();
  element_host.alive = 0xE;
  queued.clear();
  CHECK(element_vm.run_slot(3, 2, queued));
  CHECK_EQ(queued.size(), 1u);
  CHECK_EQ(queued[0].type, 1);
  CHECK_EQ(queued[0].mask, 1u);
  element_host.roster = 0xE;
  queued.clear();
  CHECK(element_vm.run_slot(3, 2, queued));
  CHECK_EQ(queued[0].mask, 1u << 3);

  // Target code 0x1B: the first PC present at battle start that is KO'd, else 0.
  pac::Module select_module;
  CHECK(select_module.parse(assemble_pac(0, 0, 0x1B), &err));
  FakeHost select_host;
  pac::Vm select_vm(select_module, select_host);
  select_vm.tick();
  select_vm.tick();
  queued.clear();
  CHECK(select_vm.run_slot(3, 2, queued));
  CHECK_EQ(select_vm.var(0x408), 0);
  select_host.ko = 0x6;
  select_host.alive = 0x9;
  queued.clear();
  CHECK(select_vm.run_slot(3, 2, queued));
  CHECK_EQ(select_vm.var(0x408), 2);

  // Target code 0x00 redraws rnd(0, 2) until it hits a targetable PC: rolls 1, 2, 1, 0 with only PC 0 alive = 4 draws, PC 0. With no PC
  // alive the draws go on until every PC was drawn once and the pick gives PC 0's bit.
  pac::Module random_module;
  CHECK(random_module.parse(assemble_pac(0, 0, 0x00), &err));
  FakeHost random_host;
  pac::Vm random_vm(random_module, random_host);
  random_vm.tick();
  random_vm.tick();
  random_host.alive = 0x9;
  random_host.rolls = {1, 2, 1, 0};
  random_host.rnd_calls = 0;
  queued.clear();
  CHECK(random_vm.run_slot(3, 2, queued));
  CHECK_EQ(random_vm.var(0x408), 1);
  CHECK_EQ(random_host.rnd_calls, 4);
  random_host.alive = 0x8;
  random_host.rolls = {2, 2, 1, 0};
  random_host.rnd_calls = 0;
  queued.clear();
  CHECK(random_vm.run_slot(3, 2, queued));
  CHECK_EQ(random_vm.var(0x408), 1);
  CHECK_EQ(random_host.rnd_calls, 4);
}

// Op 96 stores the colour gate; an enemy's A6 clears its slot-3 flag and drops the entity after the pass (its halt still runs); a revive
// puts it back, so the kill script can run again.
void test_pac_kill_and_revive() {
  Bytes code = {0x00, 0x00, 0x0E, 0xE3, 0x80, 0x96, 0x04, 0x80, 0x00, 0xA6, 0x00};
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, 1);
  put16(0x94 + 4 * 0x40 + 0, 2);
  put16(0x94 + 4 * 0x40 + 6, 9);
  image.insert(image.end(), code.begin(), code.end());
  pac::Module module;
  std::string err;
  CHECK(module.parse(image, &err));
  FakeHost host;
  pac::Vm vm(module, host);
  vm.tick();
  vm.tick();
  CHECK_EQ(vm.colour_gate(), 4);
  CHECK_EQ(vm.entity_of_unit(3), 4);
  CHECK(vm.launch(3, 3));
  CHECK(!vm.launch(3, 3));
  vm.tick();
  CHECK(!vm.scripted_death(3));
  CHECK_EQ(vm.entity_of_unit(3), -1);
  CHECK(!vm.launch(3, 3));
  vm.revive(3);
  CHECK_EQ(vm.entity_of_unit(3), 4);
  CHECK_EQ(vm.frame_count(4), 1);
  CHECK(vm.launch(3, 3));
}

void test_scripted_brain() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_pac());
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(brain.active());
  battle.drain_log();
  const int hp0 = battle.unit(0).hp;
  // The enemy may act.
  battle.unit_mut(3).countdown = 0;
  battle.settle();
  bool attacked = false;
  for (const auto& line : battle.drain_log())
    if (line.find("Beach") != std::string::npos || line.find("weak attack on A") != std::string::npos) attacked = true;
  CHECK(attacked);
  // The weak attack landed on PC1 (rolls are all-low).
  CHECK(battle.unit(0).hp < hp0);
  CHECK(battle.unit(1).hp == 50 && battle.unit(2).hp == 50);
  CHECK_EQ(battle.actions(), 1);
  CHECK(battle.unit(3).countdown > 0);
  CHECK_EQ(brain.stats().turns, 1);
  bool dlg = false;
  for (const auto& event : brain.drain_events()) dlg = dlg || (event.kind == pac::Event::Kind::Dialogue && event.a == 7);
  CHECK(dlg);
}

// A monster's op 55 draws a cell of its opened grid and marks it used, but leaves its gauge as it is (PS1: a monster with unlocked
// cells and its gauge set to 2 cast from row 0 and kept 2).
void test_enemy_pick_keeps_gauge() {
  data::GameTables tables = make_tables();
  tables.elements[1].raw[0x14] = 0xFF;
  auto& enemy = tables.enemies[0];
  enemy.raw[0x30] = 1, enemy.raw[0x34] = 1;
  enemy.raw[0x40] = 0x01, enemy.raw[0x41] = 0x00;
  enemy.raw[0x42] = 0x01, enemy.raw[0x43] = 0x00;
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_pac(0, 0, -1, true));
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK_EQ(battle.unit(3).gauge_max, 2);
  battle.unit_mut(3).gauge = 2;
  battle.unit_mut(3).countdown = 0;
  battle.settle();
  CHECK(battle.unit(3).used[0]);
  CHECK_EQ(battle.unit(3).gauge, 2);
}

// A scripted heal queued on a KO'd PC still aims at that PC (the roster mask keeps it) and has no effect: HP stays 0, the KO stays.
void test_scripted_heal_on_ko_pc() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_pac(1, 3));
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  battle.unit_mut(0).hp = 0;
  battle.unit_mut(0).lo |= st::kKO;
  const int enemy_hp = battle.unit(3).hp;
  battle.drain_log();
  battle.unit_mut(3).countdown = 0;
  battle.settle();
  bool aimed = false;
  for (const auto& line : battle.drain_log())
    if (line.find("on A: A is down") != std::string::npos) aimed = true;
  CHECK(aimed);
  CHECK_EQ(battle.unit(0).hp, 0);
  CHECK((battle.unit(0).lo & st::kKO) != 0);
  CHECK_EQ(battle.unit(3).hp, enemy_hp);
  CHECK(battle.outcome() == Outcome::Ongoing);
}

// The script stream. Entity 4 (unit 3) slot 2 = `1F v408 = rand % 2; E0 1; E2; 66; 87; queue weapon -> PC1; 2D 30 (60 ticks); E0 2; E2; A4;
// set v84 = 1`; entity 0's idle loop (slot 1) = `if v84 == 1 { E0 9; E2; v84 = 0 }`. The run records everything on virtual time.
Bytes assemble_stream_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  // rel 0: entity 0 slot 0; rel 1: idle loop; rel 2: bind unit 3, halt.
  emit({0x00});
  emit({0x00});
  emit({0x0E, 0xE3, 0x80, 0x00});
  const int slot2 = static_cast<int>(code.size());
  // v408 = rand() % (1 + 1); window 1, wait for it; begin action; queue weak weapon, mask PC1; delay 30 = 60 ticks; window 2, wait for
  // it; end turn (waits for the executor); ghost_self (evt409's right pick ends with it; it runs after the A4 wait); v84 = 1, halt.
  emit({0x1F, 0x08, 0x04, 0x01, 0x80});
  emit({0xE0, 0x01, 0x00, 0x00, 0xE2});
  emit({0x66, 0x87});
  emit({0x67, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xE0});
  emit({0x2D, 0x1E, 0x80});
  emit({0xE0, 0x02, 0x00, 0x00, 0xE2});
  emit({0xA4});
  emit({0x34, 0x01});
  emit({0x10, 0x84, 0x00, 0x01, 0x00, 0x40, 0x00});
  const int idle0 = static_cast<int>(code.size());
  const int skip = idle0 + 8 + 4 + 1 + 6;
  // if v84 == 1 ...; window 9, wait; v84 = 0; (skip) halt.
  emit({0x02, 0x84, 0x00, 0x01, 0x00, 0x40, skip & 0xFF, skip >> 8});
  emit({0xE0, 0x09, 0x00, 0x00, 0xE2});
  emit({0x10, 0x84, 0x00, 0x00, 0x00, 0x40});
  emit({0x00});
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  // Slot 1 of every entity is the idle loop.
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, 1);
  put16(0x94 + 0 * 0x40 + 2, idle0);
  put16(0x94 + 4 * 0x40 + 0, 2);
  put16(0x94 + 4 * 0x40 + 4, slot2);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

void test_pac_script_stream() {
  pac::Module module;
  std::string err;
  CHECK(module.parse(assemble_stream_pac(), &err));
  FakeHost host;
  pac::Vm vm(module, host);
  vm.tick();
  vm.tick();
  vm.drain_events();
  std::vector<pac::QueuedAttack> queued;
  vm.mark();
  CHECK(vm.run_slot(3, 2, queued));
  // Entity 0's idle loop answers v84 == 1 with window 9, then everything is back at rest.
  CHECK(vm.settle());
  CHECK(vm.quiescent());
  // Op 1F: libc_rand() (7) % 2, not the table roll.
  CHECK_EQ(vm.var(0x408), 1);
  CHECK_EQ(vm.var(0x84), 0);
  const auto ev = vm.drain_events();
  using Kind = pac::Event::Kind;
  std::vector<Kind> kinds;
  for (const auto& event : ev) kinds.push_back(event.kind);
  const std::vector<Kind> want{Kind::Mark, Kind::Dialogue, Kind::WaitDialogue, Kind::Barrier, Kind::Queue, Kind::Dialogue, Kind::WaitDialogue, Kind::Barrier, Kind::Ghost, Kind::Dialogue, Kind::WaitDialogue};
  CHECK(kinds == want);
  if (kinds != want) return;
  for (size_t i = 1; i < ev.size(); ++i) CHECK(ev[i].seq > ev[i - 1].seq && ev[i].vt >= ev[i - 1].vt);
  CHECK_EQ(ev[1].a, 1);
  CHECK_EQ(ev[5].a, 2);
  CHECK_EQ(ev[9].a, 9);
  // The Ghost event follows the A4 Barrier (the view hides the unit only then).
  CHECK(ev[8].unit == 3 && ev[8].a == 1 && ev[8].seq > ev[7].seq && ev[8].vt >= ev[7].vt);
  // The 2D delay (2 * 30 ticks) is between the queued attack and window 2.
  CHECK(ev[5].vt >= ev[4].vt + 60);
  // The reaction of entity 0 follows the slot's A4.
  CHECK(ev[9].vt >= ev[7].vt);
  CHECK_EQ(queued.size(), 1u);
  // The queued entry carries its Queue event's ordinal.
  CHECK_EQ(queued[0].seq, ev[4].seq);
}

// evt241 (the Tragedienne contestants) in miniature: entity 4's init counts v400 to 40 in a loop (with or without a 59 in it), then sets
// v80 = 1; entity 0's idle loop answers v80 == 1 with window 11 and v80 = 2.
Bytes assemble_init_order_pac(bool hint) {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  emit({0x00});
  emit({0x00});
  const int idle0 = static_cast<int>(code.size());
  const int idle0_end = idle0 + 8 + 4 + 6;
  emit({0x02, 0x80, 0x00, 0x01, 0x00, 0x40, idle0_end & 0xFF, idle0_end >> 8});
  emit({0xE0, 0x0B, 0x00, 0x00});
  emit({0x10, 0x80, 0x00, 0x02, 0x00, 0x40});
  emit({0x00});
  const int init4 = static_cast<int>(code.size());
  emit({0x0E, 0xE3, 0x80});
  emit({0x10, 0x00, 0x04, 0x00, 0x00, 0x40});
  const int loop = static_cast<int>(code.size());
  if (hint) emit({0x59});
  emit({0x17, 0x00, 0x04});
  emit({0x02, 0x00, 0x04, 0x28, 0x00, 0x44, loop & 0xFF, loop >> 8});
  emit({0x10, 0x80, 0x00, 0x01, 0x00, 0x40});
  emit({0x00});
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, 1);
  put16(0x94 + 0 * 0x40 + 2, idle0);
  put16(0x94 + 4 * 0x40 + 0, init4);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

void test_pac_init_order() {
  std::string err;
  // Op 59 reloads the tick's op budget: the 120-op init loop ends within the first tick.
  pac::Module hint_module;
  CHECK(hint_module.parse(assemble_init_order_pac(true), &err));
  FakeHost hint_host;
  pac::Vm hint_vm(hint_module, hint_host);
  hint_vm.tick();
  CHECK_EQ(hint_vm.var(0x400), 40);
  CHECK_EQ(hint_vm.var(0x80), 1);

  // Without 59 the 80-op loop runs out of budget (0x40) and sets v80 in the second tick, after entity 0's idle loop has tested it: the
  // settle pass goes on for one more tick, and the window comes (it settled with v80 == 1 and no window before).
  pac::Module module;
  CHECK(module.parse(assemble_init_order_pac(false), &err));
  FakeHost host;
  pac::Vm vm(module, host);
  vm.tick();
  CHECK_EQ(vm.var(0x80), 0);
  CHECK(vm.settle());
  CHECK_EQ(vm.var(0x80), 2);
  bool window = false;
  for (const auto& event : vm.drain_events()) window = window || (event.kind == pac::Event::Kind::Dialogue && event.a == 0x0B);
  CHECK(window);
}

// The contestants' countdown save / restore in miniature: entities 4..7 (units 3..6) each read their own countdown (field 0x5000) into
// the SAME var vCE and write 10; entity 7 then sets v80 = 1. Entity 0's idle loop answers v80 == 1 by writing vCE, vD0, vD2, vD4 back to
// units 3..6 and v80 = 2. Unit 3 so gets the LAST unit's drawn countdown and units 4..6 get 0.
Bytes assemble_countdown_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  emit({0x00});
  emit({0x00});
  const int idle0 = static_cast<int>(code.size());
  const int idle0_end = idle0 + 8 + 4 * 8 + 6;
  emit({0x02, 0x80, 0x00, 0x01, 0x00, 0x40, idle0_end & 0xFF, idle0_end >> 8});
  emit({0x76, 0xCE, 0x00, 0xE3, 0x00, 0x00, 0x50, 0x60});
  emit({0x76, 0xD0, 0x00, 0xE4, 0x00, 0x00, 0x50, 0x60});
  emit({0x76, 0xD2, 0x00, 0xE5, 0x00, 0x00, 0x50, 0x60});
  emit({0x76, 0xD4, 0x00, 0xE6, 0x00, 0x00, 0x50, 0x60});
  emit({0x10, 0x80, 0x00, 0x02, 0x00, 0x40});
  emit({0x00});
  std::array<int, 4> init{};
  for (int k = 0; k < 4; ++k) {
    init[static_cast<size_t>(k)] = static_cast<int>(code.size());
    emit({0x0E, 0xE3 + k, 0x80});
    emit({0x75, 0xCE, 0x00, 0xE9, 0x00, 0x00, 0x50, 0x60});
    emit({0x10, 0x4A, 0x04, 0x0A, 0x00, 0x40});
    emit({0x76, 0x4A, 0x04, 0xE9, 0x00, 0x00, 0x50, 0x60});
    if (k == 3) emit({0x10, 0x80, 0x00, 0x01, 0x00, 0x40});
    emit({0x00});
  }
  const size_t ec = 8, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, 1);
  put16(0x94 + 0 * 0x40 + 2, idle0);
  for (size_t k = 0; k < 4; ++k) put16(0x94 + (4 + k) * 0x40 + 0, init[k]);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

// Units at countdown 0 act in slot order; a unit still counting down is overtaken by the command menu (no time step while a PC can
// open its menu). The last unit drew 9: units 4..6 act, unit 3 keeps 9 and the menu comes after three actions. It drew 0: all four act.
void test_pac_countdown_restore() {
  for (const int last_draw : {9, 0}) {
    data::GameTables tables = make_tables();
    tables.scenes[0].raw[3] = 4;
    ScriptedRng rng{Lo::Fallback::Lo};
    pac::ScriptedBrain brain;
    brain.set_module(0, assemble_countdown_pac());
    Battle battle{tables, rng, brain};
    for (const int draw : {30, 40, 50, last_draw}) {
      rng.push(0);
      rng.push(draw);
    }
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    CHECK(brain.active());
    CHECK_EQ(brain.script_var(0x80), 2);
    CHECK_EQ(brain.script_var(0xCE), last_draw);
    CHECK_EQ(battle.actions(), last_draw == 0 ? 4 : 3);
    CHECK_EQ(battle.unit(3).countdown, last_draw == 0 ? 100 : 9);
    for (int unit = 4; unit < 7; ++unit) CHECK_EQ(battle.unit(unit).countdown, 100);
  }
}

// Op 76 writes the byte stats 0x18..0x1D (ATK, DEF, HIT, EVD, MAG, MDEF) as the PS1 stores them: one byte, or two with field bit 0x4000.
// Entity 4's init (unit 3): ATK = 50; DEF / HIT = 0x0807 as one u16 store; EVD = 0x1FF as a byte store (the high byte is dropped);
// MDEF = 0x0C2A as a u16 store (the high byte lands in the status-duration stat).
Bytes assemble_stat_write_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  emit({0x00});
  const int init4 = static_cast<int>(code.size());
  emit({0x0E, 0xE3, 0x80});
  emit({0x76, 50, 0x00, 0xE9, 0x00, 0x18, 0x00, 0xE0});
  emit({0x76, 0x07, 0x08, 0xE9, 0x00, 0x19, 0x40, 0xE0});
  emit({0x76, 0xFF, 0x01, 0xE9, 0x00, 0x1B, 0x00, 0xE0});
  emit({0x76, 0x2A, 0x0C, 0xE9, 0x00, 0x1D, 0x40, 0xE0});
  emit({0x00});
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, 0);
  put16(0x94 + 4 * 0x40 + 0, init4);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

void test_pac_stat_write() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_stat_write_pac());
  Battle battle{tables, rng, brain};
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(brain.active());
  const Unit& unit = battle.unit(3);
  CHECK_EQ(unit.atk, 50);
  CHECK_EQ(unit.def, 7);
  CHECK_EQ(unit.hit, 8);
  CHECK_EQ(unit.evd, 0xFF);
  CHECK_EQ(unit.mdef, 0x2A);
  CHECK_EQ(unit.dur, 0x0C);
}

// A scripted battle left by running away starts from a clean script state when it is entered again. evt409 (Boxer Boys) in
// miniature: entity 0 slot 0 = `v84 = 0; AF 1 (menu lock); halt`; slot 1 = `if v84 == 0 { 2C; 2D 60; E0 0; E2; inc v84; AF 0; 38 }` (the window
// "Which of us do you choose?"). The first entry shows the window, the PC runs, the same ScriptedBrain starts the scene again: the VM, its
// variables, the lock and the idle-loop progress are rebuilt, so the window is requested again with the same event order.
Bytes assemble_window_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  // rel 0 (entity 0 slot 0): v84 = 0; menu lock on; halt.
  emit({0x10, 0x84, 0x00, 0x00, 0x00, 0x40});
  emit({0xAF, 0x01});
  emit({0x00});
  const int idle0 = static_cast<int>(code.size());
  const int skip = idle0 + 8 + 1 + 3 + 4 + 1 + 3 + 2 + 1;
  // if v84 == 0 ...; begin cut scene; delay 60; window 0; wait for it; inc v84; unlock; end cut scene; (skip) halt.
  emit({0x02, 0x84, 0x00, 0x00, 0x00, 0x40, skip & 0xFF, skip >> 8});
  emit({0x2C});
  emit({0x2D, 0x3C, 0x80});
  emit({0xE0, 0x00, 0x00, 0x00});
  emit({0xE2});
  emit({0x17, 0x84, 0x00});
  emit({0xAF, 0x00});
  emit({0x38});
  emit({0x00});
  const size_t ec = 1, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  // Slot 1 is the idle loop.
  put16(0x94 + 2, idle0);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

void test_scripted_battle_reentry() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_window_pac());
  using Kind = pac::Event::Kind;
  auto kinds_of = [](const std::vector<pac::Event>& ev) {
    std::vector<int> kinds;
    for (const auto& event : ev) kinds.push_back(static_cast<int>(event.kind));
    return kinds;
  };
  std::vector<std::vector<int>> runs;
  int windows[2] = {-1, -1};
  for (int round = 0; round < 2; ++round) {
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    CHECK(brain.active());
    const auto ev = brain.drain_events();
    runs.push_back(kinds_of(ev));
    for (const auto& event : ev)
      if (event.kind == Kind::Dialogue) windows[round] = event.a;
    // "Which of us do you choose?" is requested in this entry, and the script ran ahead through its window: AF 0 is back.
    CHECK_EQ(windows[round], 0);
    CHECK(!battle.menu_locked());
    CHECK_EQ(brain.vm()->var(0x84), 1);
    // the PC runs away with the window's variables mid-way: nothing of it may reach the next entry
    brain.vm()->set_var(0x84, 3);
    Command run;
    run.kind = Command::Kind::Run;
    run.actor = 0;
    std::string why;
    CHECK(battle.apply(run, &why));
    CHECK(battle.outcome() == Outcome::Fled);
  }
  CHECK(runs[0] == runs[1]);
  bool has_cut = false;
  for (int kind : runs[0]) has_cut = has_cut || kind == static_cast<int>(Kind::Cutscene);
  CHECK(has_cut);
}

void test_blue_seal_is_not_death() {
  Fixture fixture;
  CHECK(fixture.started);
  // Status lo bit 31 = blue seal (there is no instant-death rule).
  fixture.tables.elements[2].raw[0x19] = 0, fixture.tables.elements[2].raw[0x1B] = 0x80;
  fixture.battle.do_element(3, 2, 0, 0);
  CHECK(fixture.battle.unit(0).alive());
  CHECK((fixture.battle.unit(0).lo & 0x80000000u) != 0);
}

// Infoscope (element 66) carries status lo 0x400000 = "HP known" (the target's lo word goes 0x400 -> 0x400400 at the cast start and stays).
void test_hp_known_status_applies() {
  Fixture fixture;
  CHECK(fixture.started);
  fixture.tables.elements[2].raw[0x19] = 0, fixture.tables.elements[2].raw[0x1A] = 0x40;
  fixture.battle.do_element(3, 2, 0, 0);
  CHECK((fixture.battle.unit(0).lo & 0x400000u) != 0);
}

// ---- script ops: colour sequence (AB/AE), engine flags (81 AF 39 7D 5C 85) and effect waits (C5)
// Same layout as assemble_pac(); entity 4 (unit 3) slot 2 = `body`.
Bytes assemble_slot2(const Bytes& body) {
  // The stub is: 0 entity 0 slot 0; 1 idle; 2 bind unit 3, halt.
  Bytes code{0x00, 0x00, 0x0E, 0xE3, 0x80, 0x00};
  const int slot2 = static_cast<int>(code.size());
  code.insert(code.end(), body.begin(), body.end());
  code.push_back(0x00);
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity) image[0x94 + entity * 0x40 + 2] = 1;
  image[0x94 + 4 * 0x40 + 0] = 2;
  image[0x94 + 4 * 0x40 + 4] = static_cast<u8>(slot2);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

void test_pac_resolved_ops() {
  struct ResolvedHost final : pac::Host {
    bool busy = true;
    int facing_unit = -1, facing = -1, cursor = -1;
    u32 alive_mask() const override { return 0xF; }
    int read_field(int, u16) override { return 0; }
    void write_field(int, u16, int) override {}
    int rnd(int lo, int) override { return lo; }
    // The tag 0x65636166 spells "face".
    bool effect_busy(int pool, u32 tag) const override { return busy && pool == 1 && tag == 0x65636166; }
    void set_home_facing(int unit, int value) override { facing_unit = unit, facing = value; }
    void set_menu_cursor(int value) override { cursor = value; }
  };
  CHECK_EQ(pac::op_info(0xAB).conf, 'C');
  CHECK_EQ(pac::op_info(0xC5).conf, 'C');
  std::vector<pac::QueuedAttack> queued;

  {
    // AE / AB follow the player's cast colours (yellow red green blue black white, then the colourless closer)
    pac::Module module;
    // v406 = match; v408 = next colour, direct order; v40A = next colour, alternate order.
    const Bytes body = {0xAE, 0x06, 0x04, 0x00, 0x00,
                        0xAB, 0x08, 0x04, 0x01, 0x80,
                        0xAB, 0x0A, 0x04, 0x00, 0x80};
    CHECK(module.parse(assemble_slot2(body)));
    ResolvedHost host;
    pac::Vm vm(module, host);
    vm.tick(), vm.tick();
    for (int colour : {4, 3, 0}) vm.note_cast_colour(colour);
    CHECK(vm.run_slot(3, 2, queued));
    CHECK_EQ(vm.var(0x406), 0);
    // Blue is next; the alternate order maps blue (5) through its table -> 4 - 1.
    CHECK_EQ(vm.var(0x408), 5);
    CHECK_EQ(vm.var(0x40A), 3);
    for (int colour : {5, 2, 1, 6}) vm.note_cast_colour(colour);
    CHECK(vm.run_slot(3, 2, queued));
    CHECK_EQ(vm.var(0x406), 1);
    // The sequence is complete.
    CHECK_EQ(vm.var(0x408), 6);
    CHECK(vm.sfx_muted());
    CHECK_EQ(vm.var(0x52), 6);
    // A new sequence restarts the history.
    vm.note_cast_colour(2);
    CHECK_EQ(vm.colour_history()[0], 3);
    CHECK_EQ(vm.colour_history()[1], 0);
  }
  {
    // engine flags, scripted-death flag, home facing, menu cursor
    pac::Module module;
    const Bytes body = {0x81, 0x00, 0xAF, 0x01, 0x39, 0x00, 0x7D, 0x01, 0x5C, 0x00, 0xE3, 0x80,
                        0x85, 0x05, 0x80, 0xE3, 0x80, 0x7B, 0x02, 0x80};
    CHECK(module.parse(assemble_slot2(body)));
    ResolvedHost host;
    pac::Vm vm(module, host);
    vm.tick(), vm.tick();
    CHECK(vm.end_check_enabled() && vm.ui_enabled() && !vm.menu_locked() && !vm.menu_entry100() && !vm.scripted_death(3));
    CHECK(vm.run_slot(3, 2, queued));
    CHECK(!vm.end_check_enabled() && !vm.ui_enabled() && vm.menu_locked() && vm.menu_entry100());
    CHECK(vm.scripted_death(3));
    CHECK_EQ(host.facing_unit, 3);
    CHECK_EQ(host.facing, 5);
    CHECK_EQ(host.cursor, 2);
  }
  {
    // C5 blocks while the host reports a live effect object with that tag
    pac::Module module;
    // Wait on "face"; inc v406.
    const Bytes body = {0xC5, 'f', 'a', 'c', 'e', 0x17, 0x06, 0x04};
    CHECK(module.parse(assemble_slot2(body)));
    ResolvedHost host;
    pac::Vm vm(module, host);
    vm.tick(), vm.tick();
    CHECK(vm.launch(3, 2));
    for (int i = 0; i < 5; ++i) vm.tick();
    CHECK_EQ(vm.var(0x406), 0);
    host.busy = false;
    vm.tick();
    CHECK_EQ(vm.var(0x406), 1);
    // Passing, the op records the wait for the presentation (pool 1, the tag).
    bool waited = false;
    for (const pac::Event& event : vm.drain_events()) waited = waited || (event.kind == pac::Event::Kind::EffectWait && event.a == 1 && event.b == 0x65636166);
    CHECK(waited);
  }
  {
    // 2F on the ring [empty, empty, green]: green 1, the largest group 2 (the empty slots), the empty slots 2; ring vars keep their old
    // value for an empty slot. 83 always writes the pending result: 0 forces a victory, 0x80 clears it.
    struct RingHost final : pac::Host {
      std::array<int, 3> ring{-1, -1, 0};
      u32 alive_mask() const override { return 0xF; }
      int read_field(int, u16) override { return 0; }
      void write_field(int, u16, int) override {}
      int rnd(int lo, int) override { return lo; }
      int ring_colour(int slot) const override { return ring[static_cast<size_t>(slot)]; }
    };
    const Bytes body = {0x2F, 0x06, 0x04, 0x00, 0x80, 0x2F, 0x08, 0x04, 0x06, 0x80, 0x2F, 0x0A, 0x04, 0x08, 0x80, 0x83, 0x00, 0x80};
    pac::Module module;
    CHECK(module.parse(assemble_slot2(body)));
    RingHost host;
    pac::Vm vm(module, host);
    vm.set_var(0x38, 4);
    vm.tick(), vm.tick();
    CHECK(vm.run_slot(3, 2, queued));
    CHECK_EQ(vm.var(0x406), 1);
    CHECK_EQ(vm.var(0x408), 2);
    CHECK_EQ(vm.var(0x40A), 2);
    CHECK_EQ(vm.forced_result(), 0);
    CHECK_EQ(vm.var(0x38), 4);
    CHECK_EQ(vm.var(0x3C), 0);
    host.ring = {-1, kRingAllColours, kRingAllColours};
    const Bytes clear = {0x2F, 0x08, 0x04, 0x06, 0x80, 0x83, 0x00, 0x80, 0x83, 0x80, 0x80};
    pac::Module clear_module;
    CHECK(clear_module.parse(assemble_slot2(clear)));
    pac::Vm clear_vm(clear_module, host);
    clear_vm.tick(), clear_vm.tick();
    CHECK(clear_vm.run_slot(3, 2, queued));
    CHECK_EQ(clear_vm.var(0x408), 2);
    CHECK_EQ(clear_vm.var(0x3A), kRingAllColours);
    CHECK_EQ(clear_vm.forced_result(), -1);
  }
  {
    // B0 -200 lifts by the step -20 and blocks 20 ticks; B1 lands with the step B0 left; 84 forces the low-HP bit of the unit.
    pac::Module module;
    const Bytes body = {0xB0, 0x38, 0xFF, 0x80, 0x17, 0x06, 0x04, 0xB1, 0x00, 0xB1, 0x01, 0x17, 0x08, 0x04, 0x84, 0xE9, 0x80};
    CHECK(module.parse(assemble_slot2(body)));
    ResolvedHost host;
    host.busy = false;
    pac::Vm vm(module, host);
    vm.tick(), vm.tick();
    vm.drain_events();
    CHECK(vm.launch(3, 2));
    for (int i = 0; i < pac::kLiftTicks; ++i) vm.tick();
    CHECK_EQ(vm.var(0x406), 0);
    vm.tick();
    CHECK_EQ(vm.var(0x406), 1);
    for (int i = 0; i < pac::kLiftTicks; ++i) vm.tick();
    CHECK_EQ(vm.var(0x408), 1);
    std::vector<pac::Event> lifts;
    bool low_hp = false;
    for (const pac::Event& event : vm.drain_events()) {
      if (event.kind == pac::Event::Kind::Lift) lifts.push_back(event);
      low_hp = low_hp || (event.kind == pac::Event::Kind::QuarterHp && event.unit == 3);
    }
    CHECK_EQ(lifts.size(), 2u);
    CHECK(lifts.size() == 2 && lifts[0].unit == 3 && lifts[0].a == -20 && lifts[0].b == 1 && lifts[1].a == -20 && lifts[1].b == -1);
    CHECK(lifts.size() == 2 && lifts[1].vt - lifts[0].vt == static_cast<u32>(pac::kLiftTicks));
    CHECK(low_hp);
  }
  {
    // A B1 with no B0 before it moves by step 0 (it only waits).
    pac::Module module;
    const Bytes body = {0xB1, 0x00, 0xB1, 0x01};
    CHECK(module.parse(assemble_slot2(body)));
    ResolvedHost host;
    pac::Vm vm(module, host);
    vm.tick(), vm.tick();
    vm.drain_events();
    CHECK(vm.launch(3, 2));
    vm.tick();
    bool still = false;
    for (const pac::Event& event : vm.drain_events()) still = still || (event.kind == pac::Event::Kind::Lift && event.a == 0 && event.b == -1);
    CHECK(still);
  }
}

// Op 67 type 6 casts its element into the trap whatever the trap holds: the element lands nothing, the enemy side's trap empties and the
// trap block hides.
void test_scripted_trap_fizzle() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_pac(6, 1));
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  battle.set_trap(true, 2);
  const int hp0 = battle.unit(0).hp;
  battle.drain_log();
  battle.unit_mut(3).countdown = 0;
  battle.settle();
  bool fired = false;
  for (const auto& line : battle.drain_log()) fired = fired || line.find("the element trap fires") != std::string::npos;
  CHECK(fired);
  CHECK_EQ(battle.unit(0).hp, hp0);
  CHECK_EQ(battle.trap_hud().enemy, 0);
  CHECK(!battle.trap_hud().shown);
}

// The Radius tutorial (battle 386): a lethal hit on its first enemy slot leaves it 1 HP and no KO; any other battle kills.
void test_tutorial_enemy_undying() {
  for (const int scene : {0, Battle::kUndyingScene}) {
    data::GameTables tables = make_tables();
    tables.scenes.resize(static_cast<size_t>(Battle::kUndyingScene) + 1, tables.scenes[0]);
    ScriptedRng rng{Lo::Fallback::Lo};
    NullBrain brain;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A")}, scene));
    battle.unit_mut(3).hp = 1;
    battle.do_weapon_attack(0, 3, 2);
    const bool undying = scene == Battle::kUndyingScene;
    CHECK_EQ(battle.unit(3).hp, undying ? 1 : 0);
    CHECK_EQ(battle.unit(3).alive(), undying);
  }
}

}  // namespace

// Op 81 suspends the end test, an op-83 code ends the fight first (Outcome::Scripted, result_code), the
// party test comes before the enemy test (a mutual KO is a defeat); slot-4 counter-attacks are executed.
struct CtlBrain final : EnemyBrain {
  bool end_on = true, once = true;
  int forced = -1;
  int reactions = 0;
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::None;
    return action;
  }
  bool end_check_enabled() const override { return end_on; }
  int forced_result() const override { return forced; }
  void on_attacked(Battle&, int unit, Rng&, std::vector<EnemyAction>& out) override {
    if (unit < kPcSlots || (once && reactions > 0)) return;
    ++reactions;
    EnemyAction action;
    action.kind = EnemyAction::Kind::Weapon;
    action.strength = 0;
    action.target = 0;
    out.push_back(action);
  }
};

// evt084 (Portalgheist): entity 0's init ghosts units 3 / 4 (`5B 1 e3` / `e4`), the scene record byte +0x1B = 3 keeps them out of the all-target
// masks; the door's attacked script later reveals them (`35 0 4`, `5B 0 e3`, which set the mask bit again). Off-stage units
// are never on the cursor ring, so a single-target command on one is refused.
Bytes assemble_ghost_pac(bool reveal, bool second = false) {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  // The door's pair (evt084: `5B 1 e3` / `5B 1 e4`): unit 4 stays a ghost.
  if (second) emit({0x5B, 0x01, 0xE4, 0x80});
  // rel 0 (entity 0 slot 0): ghost_unit (flag) unit 3, halt.
  emit({0x5B, reveal ? 0x00 : 0x01, 0xE3, 0x80, 0x00});
  const int idle = static_cast<int>(code.size());
  emit({0x00});
  const int init4 = static_cast<int>(code.size());
  // Entity 4: bind unit 3, halt.
  emit({0x0E, 0xE3, 0x80, 0x00});
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity) put16(0x94 + entity * 0x40 + 2, idle);
  put16(0x94 + 4 * 0x40 + 0, init4);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

// An arrival: the idle loop arms itself (var 0x40), queues a self-cast, waits on it with A4, then reveals its unit with `34 0`.
Bytes assemble_idle_cast_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  // rel 0: a lone halt, every other entity's idle loop.
  emit({0x00});
  const int init4 = static_cast<int>(code.size());
  // Entity 4: bind unit 3, var 0x40 := 1, halt.
  emit({0x0E, 0xE3, 0x80, 0x10, 0x40, 0x00, 0x01, 0x00, 0x40, 0x00});
  const int idle4 = static_cast<int>(code.size());
  // Var 0x40 == 1: begin_action, clear_queue, queue_attack element 1 on self, A4, var 0x40 := 2, ghost_self 0.
  const int end = idle4 + 8 + 1 + 1 + 8 + 1 + 6 + 2;
  emit({0x02, 0x40, 0x00, 0x01, 0x00, 0x40, end & 0xFF, end >> 8});
  emit({0x66, 0x87, 0x67, 0x01, 0x00, 0x01, 0x00, 0x20, 0x00, 0xE0, 0xA4});
  emit({0x10, 0x40, 0x00, 0x02, 0x00, 0x40, 0x34, 0x00, 0x00});
  const size_t ec = 5, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  put16(0x94 + 4 * 0x40 + 0, init4);
  put16(0x94 + 4 * 0x40 + 2, idle4);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

// Pip's form change: the PC entity's slot 1 runs A7 on its own unit,
// the pending form element blocks every PC menu, Pip's next turn (slot 2) casts it on himself, mode 45 applies the form.
Bytes assemble_pip_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  const int idle = static_cast<int>(code.size());
  emit({0x00});
  const int init1 = static_cast<int>(code.size());
  // Entity 1 slot 0: bind unit 0, halt.
  emit({0x0E, 0xE0, 0x80, 0x00});
  const int tick1 = static_cast<int>(code.size());
  // set v9E = E0 (own unit); A7 vA0 v9E.
  emit({0x10, 0x9E, 0x00, 0xE0, 0x00, 0x40});
  emit({0xA7, 0xA0, 0x00, 0x9E, 0x00});
  emit({0x00});
  const int turn1 = static_cast<int>(code.size());
  emit({0x66, 0x87});
  // Queue element vA0 on unit 0 (type / target immediate).
  emit({0x67, 0x01, 0x00, 0xA0, 0x00, 0x01, 0x00, 0xA0});
  emit({0xA4, 0x00});
  const size_t ec = 2, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity)
    for (int slot = 0; slot < 5; ++slot) put16(0x94 + entity * 0x40 + static_cast<size_t>(slot) * 2, idle);
  put16(0x94 + 0x40 + 0, init1);
  put16(0x94 + 0x40 + 2, tick1);
  put16(0x94 + 0x40 + 4, turn1);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

void test_pip_evolution() {
  data::GameTables tables = make_tables();
  tables.elements.resize(0x130);
  for (int id = 0x80; id <= 0x85; ++id) {
    auto& raw = tables.elements[static_cast<size_t>(id)].raw;
    // pgdat 0x80..0x85: mode 45, power = the form.
    raw[0] = 0x81, raw[1] = 2, raw[4] = 0x44, raw[5] = 0x08, raw[7] = 0x2D;
  }
  tables.elements[0x81].raw[8] = 0x2D;
  for (int id = 0x11F; id <= 0x12D; ++id) tables.elements[static_cast<size_t>(id)].raw[5] = 0x08, tables.elements[static_cast<size_t>(id)].raw[6] = 1;
  auto pip = [](int kind) {
    PartyMember member = make_member(43, "Pip");
    member.kind = kind;
    member.cells[27] = 0xC11F, member.cells[47] = 0xC120, member.cells[67] = 0xC121;
    member.unlocked[27] = member.unlocked[47] = member.unlocked[67] = true;
    return member;
  };
  {
    // At battle start the techs follow the current form (0x11F / 0x121 -> first, 0x120 -> second); their colour = his innate
    Fixture fx;
    Battle battle{tables, fx.rng, fx.brain};
    fx.rng.push(0);
    fx.rng.push(100);
    CHECK(battle.start({pip(0x2C), make_member(1, "B"), make_member(2, "C")}, 0));
    CHECK_EQ(battle.unit(0).kind, 0x2C);
    CHECK_EQ(battle.unit(0).cells[27], 0xC121);
    CHECK_EQ(battle.unit(0).cells[47], 0xC122);
    CHECK_EQ(battle.unit(0).cells[67], 0xC121);
    CHECK_EQ(battle.element_colour(0x121), 0x08);
    CHECK_EQ(battle.unit(1).kind, 1);
  }
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_pip_pac());
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({pip(0x2B), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(brain.active());
  // Green points: A = 130 >= 125 -> 0x81 (form 0x2D). Slot 1 runs A7.
  brain.vm()->set_var(0x04, 130);
  brain.vm()->tick();
  CHECK_EQ(battle.evo_pending(), 0x81);
  CHECK_EQ(brain.vm()->var(0xA0), 0x81);
  // No command menu while the form change is pending.
  CHECK(!battle.can_act(0) && !battle.can_act(1));
  // Spent cells before the change: the rewritten tech cells become usable again, the others stay spent.
  for (int cell : {0, 27, 47, 67}) battle.unit_mut(0).used[static_cast<size_t>(cell)] = true;
  battle.settle();
  CHECK_EQ(battle.evo_pending(), 0);
  CHECK_EQ(battle.unit(0).kind, 0x2D);
  // Green had the most points.
  CHECK_EQ(battle.unit(0).innate, 0x04);
  CHECK_EQ(brain.vm()->var(0x04), 0);
  CHECK_EQ(battle.unit(0).cells[27], 0xC123);
  CHECK_EQ(battle.unit(0).cells[47], 0xC124);
  // The third tech only for a second form.
  CHECK_EQ(battle.unit(0).cells[67], 0xC121);
  CHECK(!battle.unit(0).used[27] && !battle.unit(0).used[47]);
  CHECK(battle.unit(0).used[67] && battle.unit(0).used[0]);
  CHECK(battle.party()[0].evolved && battle.party()[0].kind == 0x2D && battle.party()[0].growth_col == 3);
  CHECK_EQ(battle.element_colour(0x123), 0x04);
  // A second form rewrites the third cell too; an empty tech cell stays empty and keeps its used bit.
  battle.unit_mut(0).used[67] = true;
  battle.unit_mut(0).cells[47] = 0;
  battle.unit_mut(0).used[47] = true;
  battle.evolve(0, 0x2E);
  CHECK_EQ(battle.unit(0).cells[67], 0xC127);
  CHECK(!battle.unit(0).used[67]);
  CHECK_EQ(battle.unit(0).cells[47], 0);
  CHECK(battle.unit(0).used[47]);
}

// The PC entities' idle loops as the scenes have them: every PC entity writes its own unit into one shared global (v9C) each tick, and
// the entity of a Pip whose form can still change passes it to A7. Entity 1 = Pip (unit 0), entity 2 = the member after him (unit 1).
Bytes assemble_shared_unit_pac() {
  Bytes code;
  auto emit = [&](std::initializer_list<int> bytes) {
    for (int byte : bytes) code.push_back(static_cast<u8>(byte));
  };
  const int idle = static_cast<int>(code.size());
  emit({0x00});
  const int init1 = static_cast<int>(code.size());
  emit({0x0E, 0xE0, 0x80, 0x00});
  const int init2 = static_cast<int>(code.size());
  emit({0x0E, 0xE1, 0x80, 0x00});
  const int tick1 = static_cast<int>(code.size());
  // v9C = E0; v9E = 0; A7 v9E v9C.
  emit({0x59, 0x10, 0x9C, 0x00, 0xE0, 0x00, 0x40, 0x10, 0x9E, 0x00, 0x00, 0x00, 0x40});
  emit({0xA7, 0x9E, 0x00, 0x9C, 0x00});
  emit({0x00});
  const int tick2 = static_cast<int>(code.size());
  // v9C = E1.
  emit({0x59, 0x10, 0x9C, 0x00, 0xE1, 0x00, 0x40, 0x00});
  const size_t ec = 3, base = 0x94 + ec * 0x40;
  Bytes image(base, 0);
  auto put32 = [&](size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) image[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
  };
  auto put16 = [&](size_t offset, int value) {
    image[offset] = static_cast<u8>(value);
    image[offset + 1] = static_cast<u8>(value >> 8);
  };
  put32(0, 2), put32(4, 0x10), put32(8, static_cast<u32>(base + code.size())), put32(0xC, static_cast<u32>(base + code.size() + 8));
  put32(0x90, static_cast<u32>(ec));
  for (size_t entity = 0; entity < ec; ++entity)
    for (int slot = 0; slot < 5; ++slot) put16(0x94 + entity * 0x40 + static_cast<size_t>(slot) * 2, idle);
  put16(0x94 + 0x40 + 0, init1);
  put16(0x94 + 0x40 + 2, tick1);
  put16(0x94 + 0x80 + 0, init2);
  put16(0x94 + 0x80 + 2, tick2);
  image.insert(image.end(), code.begin(), code.end());
  return image;
}

// A Pip that can still change form, in a party slot before another member: the later entity's write of the shared unit global does not
// keep the settle pass going (Pip's entity wrote the global itself before A7 read it), so no slot run costs the 1500-tick cap and the
// view's script stream gets no 1500-tick gap after every action.
void test_pip_idle_loop_settles() {
  data::GameTables tables = make_tables();
  for (const int kind : {0x2B, 0x2C, 0x2D}) {
    ScriptedRng rng{Lo::Fallback::Lo};
    pac::ScriptedBrain brain;
    brain.set_module(0, assemble_shared_unit_pac());
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    PartyMember pip = make_member(43, "Pip");
    pip.kind = kind;
    CHECK(battle.start({pip, make_member(1, "B"), make_member(2, "C")}, 0));
    CHECK(brain.active());
    CHECK(brain.vm()->vtime() < 10);
    const u32 before = brain.vm()->vtime();
    CHECK(brain.vm()->settle());
    CHECK(brain.vm()->vtime() - before < 10);
    CHECK_EQ(battle.evo_pending(), 0);
    CHECK_EQ(brain.vm()->var(0x9C), 0xE1);
  }
}

// Pip's colour points: script vars 0x00..0x0E start as BattleOptions::saved_vars and come back out of saved_vars(); every element action adds 1
// to the point of its record colour while Pip is in the party (2 when Pip casts it), nothing without him or for an action without one colour.
void test_pip_colour_points() {
  const std::array<u16, 8> saved{0x11, 0x3F, 0, 0, 0, 0, 0, 5};
  auto cast = [&](int pip_slot, int caster, u8 colour) {
    data::GameTables tables = make_tables();
    tables.elements[1].raw[5] = colour;
    ScriptedRng rng{Lo::Fallback::Lo};
    NullBrain brain;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    std::vector<PartyMember> party{make_member(0, "A"), make_member(1, "B"), make_member(2, "C")};
    if (pip_slot >= 0) party[static_cast<size_t>(pip_slot)] = make_member(kPipCharId, "Pip");
    BattleOptions opt;
    opt.saved_vars = saved;
    CHECK(battle.start(party, 0, opt));
    battle.unit_mut(caster).gauge = 2;
    Command command;
    command.kind = Command::Kind::Element;
    command.actor = caster;
    command.cell = 0;
    command.target = 3;
    std::string why;
    CHECK(battle.apply(command, &why));
    return battle.saved_vars();
  };
  std::array<u16, 8> want = saved;
  CHECK(cast(-1, 0, 0x80) == want);
  want[7] = 6;
  CHECK(cast(1, 0, 0x80) == want);
  want[7] = 7;
  CHECK(cast(1, 1, 0x80) == want);
  want = saved;
  want[2] = 1;
  CHECK(cast(2, 0, 0x04) == want);
  CHECK(cast(1, 0, 0xFC) == saved);

  // An enemy's cast counts too.
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  CastBrain brain;
  brain.element = 1;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(kPipCharId, "Pip"), make_member(2, "C")}, 0));
  for (int i = 0; i < kPcSlots; ++i) battle.unit_mut(i).stamina = 0;
  battle.settle();
  int enemy_casts = 0;
  for (const ActionEvent& event : battle.events())
    if (event.kind == ActionEvent::Kind::Element && event.actor == 3) ++enemy_casts;
  CHECK(enemy_casts > 0);
  CHECK_EQ(battle.saved_vars()[7], enemy_casts);

  // With a scene script the points are its variables.
  pac::ScriptedBrain scripted;
  scripted.set_module(0, assemble_shared_unit_pac());
  ScriptedRng scripted_rng{Lo::Fallback::Lo};
  Battle scripted_battle{tables, scripted_rng, scripted};
  scripted_rng.push(0);
  scripted_rng.push(100);
  BattleOptions opt;
  opt.saved_vars = saved;
  CHECK(scripted_battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0, opt));
  CHECK_EQ(scripted.vm()->var(0x02), 0x3F);
  CHECK_EQ(scripted.vm()->var(0x0E), 5);
  scripted.vm()->set_var(0x04, 9);
  CHECK_EQ(scripted_battle.saved_vars()[2], 9);
}

void test_hidden_units() {
  for (int reveal = 0; reveal < 2; ++reveal) {
    data::GameTables tables = make_tables();
    // Two of the monster: unit 3 (off-stage) and unit 4 (on stage - a side of hidden units only is a victory); unit 3 is off-stage from init.
    tables.scenes[0].raw[3] = 2;
    tables.scenes[0].raw[0x1B] = 1;
    ScriptedRng rng{Lo::Fallback::Lo};
    pac::ScriptedBrain brain;
    brain.set_module(0, assemble_ghost_pac(reveal != 0));
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    CHECK(brain.active());
    // 5B writes the targetable flag only: the spawn seed's hide / model hide stay (only ops 34 / 35 clear them).
    CHECK(battle.hidden(3));
    CHECK_EQ(battle.untargetable(3), reveal == 0);
    CHECK(!battle.hidden(4) && battle.unit(4).present);
    // The init `5B 0` sets the bit again, `5B 1` leaves the scene's mask.
    CHECK_EQ(battle.cast_mask(), reveal ? 0x1FFu : 0x1F7u);
    Command attack;
    attack.actor = 0;
    attack.kind = Command::Kind::Attack;
    attack.chain = {0};
    attack.target = 3;
    std::string why;
    CHECK_EQ(battle.validate(attack, &why), reveal != 0);
    if (!reveal) CHECK(why == "the target is off-stage");
    // The automatic target skips the off-stage unit.
    attack.target = -1;
    const int hp3 = battle.unit(3).hp, hp4 = battle.unit(4).hp;
    CHECK(battle.apply(attack));
    battle.settle();
    CHECK(reveal ? battle.unit(3).hp < hp3 || battle.unit(4).hp < hp4 : battle.unit(3).hp == hp3 && battle.unit(4).hp < hp4);
    battle.set_cast_bit(3, true);
    CHECK_EQ(battle.cast_mask(), 0x1FFu);
  }
}

// Portalgheist's pair: one summon reveals ONE of the two ghosts (`5B 0 e3`), the other stays off-stage and out of the cast mask until its own op.
void test_hidden_pair_reveal_one() {
  data::GameTables tables = make_tables();
  // Units 3, 4, 5 (5 on stage: the door); units 3 and 4 are off-stage from init.
  tables.scenes[0].raw[3] = 3;
  tables.scenes[0].raw[0x1B] = 3;
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_ghost_pac(true, true));
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(!battle.untargetable(3) && battle.untargetable(4) && !battle.untargetable(5));
  // The seed hide is untouched by the 5B ops.
  CHECK(battle.hidden(3) && battle.hidden(4) && !battle.hidden(5));
  CHECK((battle.cast_mask() & (1u << 3)) != 0 && (battle.cast_mask() & (1u << 4)) == 0);
  Command attack;
  attack.actor = 0;
  attack.kind = Command::Kind::Attack;
  attack.chain = {0};
  attack.target = 4;
  std::string why;
  CHECK(!battle.validate(attack, &why) && why == "the target is off-stage");
  attack.target = 3;
  CHECK(battle.validate(attack, &why));
}

// A unit a script reveals while an all-target cast is resolving (the last Komodo Pup's kill script brings in Mama Komodo) is not hit by it: the
// cast's targets are fixed when it starts.
struct RevealBrain final : EnemyBrain {
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::None;
    return action;
  }
  void on_killed(Battle& battle, int, Rng&, std::vector<EnemyAction>&) override { battle.set_cast_bit(4, true); }
};

void test_reveal_during_all_target_cast() {
  data::GameTables tables = make_tables();
  // Three enemies, the second kept out of the all-target casts until the first one's kill script reveals it.
  tables.scenes[0].raw[3] = 3;
  tables.scenes[0].raw[0x1B] = 2;
  tables.elements.resize(5);
  auto& sweep = tables.elements[4];
  sweep.raw[4] = 0x90, sweep.raw[5] = 0x80, sweep.raw[6] = 1, sweep.raw[8] = 12;
  RevealBrain brain;
  ScriptedRng rng(Lo::Fallback::Lo);
  rng.push(0);
  rng.push(100);
  Battle battle(tables, rng, brain);
  PartyMember caster = make_member(0, "A");
  caster.cells[0] = 0x4004;
  CHECK(battle.start({caster, make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(battle.unit(4).present && battle.unit(5).present);
  CHECK_EQ(battle.cast_mask() & (1u << 4), 0u);
  battle.unit_mut(3).hp = 1;
  battle.unit_mut(4).hp = battle.unit_mut(4).max_hp = 9999;
  battle.unit_mut(5).hp = battle.unit_mut(5).max_hp = 9999;
  battle.unit_mut(0).gauge = 2;
  Command cast;
  cast.kind = Command::Kind::Element;
  cast.actor = 0;
  cast.cell = 0;
  cast.target = 3;
  std::string why;
  CHECK(battle.apply(cast, &why));
  CHECK(!battle.unit(3).alive());
  CHECK(battle.cast_mask() & (1u << 4));
  CHECK_EQ(battle.unit(4).hp, 9999);
  CHECK(battle.unit(5).hp < 9999);
}

// A weapon with class bit 7 (rods, boomerangs): the PC's heavy hit is one hit on every enemy (one stamina cost, chain step and gauge step),
// each target with its own roll; a miss sticks, so every later target misses too and the hit adds nothing. Weak and medium hits, and a
// heavy hit with another weapon, strike the aimed unit alone.
void test_weapon_sweep() {
  data::GameTables tables = make_tables();
  tables.scenes[0].raw[3] = 3;
  tables.gear.resize(256);
  tables.gear[0x30].raw[1] = 0x83, tables.gear[0x30].raw[2] = 0x10, tables.gear[0x30].raw[4] = 8, tables.gear[0x30].raw[6] = 95;
  tables.gear[0x2D].raw[1] = 0x03, tables.gear[0x2D].raw[2] = 0x12, tables.gear[0x2D].raw[4] = 16, tables.gear[0x2D].raw[6] = 95;
  CHECK(make_equip(tables, 0x30, 0, {}).wp_sweeps);
  CHECK_EQ(make_equip(tables, 0x30, 0, {}).wp_class, 3);
  CHECK(!make_equip(tables, 0x2D, 0, {}).wp_sweeps);
  struct Run {
    std::vector<ActionEvent> hits;
    std::array<int, 3> hp{};
    int stamina = 0, gauge = 0;
  };
  // Every roll comes out at its top, so a hit roll lands only on a sleeping target (chance 100); `asleep` = those enemies (bit = unit).
  const auto run = [&](int strength, bool sweeps, u32 asleep) {
    ScriptedRng rng(ScriptedRng::Fallback::Hi);
    rng.push(0);
    rng.push(100);
    NullBrain brain;
    Battle battle(tables, rng, brain);
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    for (int unit = 3; unit < 6; ++unit) {
      battle.unit_mut(unit).hp = battle.unit_mut(unit).max_hp = 9999;
      if (asleep >> unit & 1u) battle.unit_mut(unit).lo |= st::kAsleep;
    }
    battle.unit_mut(0).eq.wp_sweeps = sweeps;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.target = 4;
    attack.chain = {strength};
    CHECK(battle.apply(attack));
    Run out;
    for (const ActionEvent& event : battle.events())
      if (event.kind == ActionEvent::Kind::Weapon && event.actor == 0) out.hits.push_back(event);
    for (int unit = 3; unit < 6; ++unit) out.hp[static_cast<size_t>(unit - 3)] = battle.unit(unit).hp;
    out.stamina = battle.unit(0).stamina;
    out.gauge = battle.unit(0).gauge;
    return out;
  };
  // Every target hit: one event per enemy in ascending unit order, one hit (shared hit_index, the aim recorded), one cost, gauge -1 + 3.
  const Run all = run(2, true, 0x38);
  CHECK_EQ(all.hits.size(), 3u);
  for (size_t k = 0; k < all.hits.size(); ++k) {
    CHECK_EQ(all.hits[k].target, static_cast<int>(3 + k));
    CHECK_EQ(all.hits[k].aim, 4);
    CHECK_EQ(all.hits[k].hit_index, all.hits[0].hit_index);
    CHECK_EQ(all.hits[k].code, 0);
    CHECK(all.hp[k] < 9999);
  }
  CHECK_EQ(all.stamina, 40);
  CHECK_EQ(all.gauge, 2);
  // Unit 3 is missed: units 4 and 5 miss after it although their own rolls land, and the gauge does not move.
  const Run sticky = run(2, true, 0x30);
  CHECK_EQ(sticky.hits.size(), 3u);
  for (const ActionEvent& event : sticky.hits) CHECK_EQ(event.code, 2);
  CHECK(sticky.hp == (std::array<int, 3>{9999, 9999, 9999}));
  CHECK_EQ(sticky.stamina, 40);
  CHECK_EQ(sticky.gauge, -1);
  // The last target missed: the earlier hits stay, the gauge does not move.
  const Run last = run(2, true, 0x18);
  CHECK(last.hits.size() == 3u && last.hits[0].code == 0 && last.hits[1].code == 0 && last.hits[2].code == 2);
  CHECK(last.hp[0] < 9999 && last.hp[1] < 9999 && last.hp[2] == 9999);
  CHECK_EQ(last.gauge, -1);
  // Weak and medium hits, and a heavy hit with a weapon without the bit: the aimed unit alone.
  for (const Run& single : {run(0, true, 0x38), run(1, true, 0x38), run(2, false, 0x38)}) {
    CHECK_EQ(single.hits.size(), 1u);
    CHECK(!single.hits.empty() && single.hits[0].target == 4 && single.hits[0].aim == -1);
    CHECK(single.hp[0] == 9999 && single.hp[1] < 9999 && single.hp[2] == 9999);
  }
}

// An attack queued by an idle loop is not lost: the unit casts it as an action of its own after the action that woke the loop, and the script
// goes on past its A4 to the reveal.
void test_idle_loop_cast() {
  data::GameTables tables = make_tables();
  tables.scenes[0].raw[3] = 2;
  tables.scenes[0].raw[0x1B] = 1;
  ScriptedRng rng{Lo::Fallback::Lo};
  pac::ScriptedBrain brain;
  brain.set_module(0, assemble_idle_cast_pac());
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  CHECK(battle.cast_mask() & (1u << 3));
  battle.unit_mut(3).hp = battle.unit_mut(3).max_hp = 9999;
  battle.do_weapon_attack(0, 4, 0);
  battle.finish_external_action();
  int casts = 0;
  for (const ActionEvent& event : battle.events())
    if (event.kind == ActionEvent::Kind::Element && event.actor == 3 && event.element == 1 && event.target == 3) ++casts;
  CHECK_EQ(casts, 1);
  CHECK(brain.vm() && brain.vm()->quiescent());
}

void test_end_rules() {
  data::GameTables tables = make_tables();
  auto fresh = [&](CtlBrain& brain, ScriptedRng& rng) {
    rng.push(0);
    rng.push(100);
    auto battle = std::make_unique<Battle>(tables, rng, brain);
    CHECK(battle->start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    return battle;
  };
  {
    // mutual KO -> wiped
    CtlBrain br;
    ScriptedRng rng(Lo::Fallback::Lo);
    auto battle = fresh(br, rng);
    for (int i = 0; i < 4; ++i) battle->unit_mut(i).lo |= st::kKO, battle->unit_mut(i).hp = 0;
    battle->check_end();
    CHECK(battle->outcome() == Outcome::Wiped);
    CHECK_EQ(battle->result_code(), 1);
  }
  {
    // end test off: nothing ends; forced code ends with that code once it is back on
    CtlBrain br;
    ScriptedRng rng(Lo::Fallback::Lo);
    auto battle = fresh(br, rng);
    br.end_on = false;
    battle->unit_mut(3).lo |= st::kKO;
    battle->unit_mut(3).hp = 0;
    battle->check_end();
    CHECK(battle->outcome() == Outcome::Ongoing);
    br.end_on = true;
    br.forced = 3;
    battle->check_end();
    CHECK(battle->outcome() == Outcome::Scripted);
    CHECK_EQ(battle->result_code(), 3);
  }
  {
    // a counter-attack queued by slot 4 is executed after the PC's command
    CtlBrain br;
    ScriptedRng rng(Lo::Fallback::Lo);
    auto battle = fresh(br, rng);
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {0};
    CHECK(battle->apply(attack));
    CHECK_EQ(br.reactions, 1);
    bool countered = false;
    for (const ActionEvent& event : battle->drain_events()) countered = countered || (event.actor == 3 && event.target == 0 && event.kind == ActionEvent::Kind::Weapon);
    CHECK(countered);
  }
  {
    // the counter is enemy-led, so it starts no further slot 4
    CtlBrain br;
    br.once = false;
    ScriptedRng rng(Lo::Fallback::Lo);
    auto battle = fresh(br, rng);
    battle->unit_mut(3).hp = battle->unit_mut(3).max_hp = 9999;
    battle->do_weapon_attack(0, 3, 0);
    battle->finish_external_action();
    CHECK_EQ(br.reactions, 1);
  }
  {
    // an enemy-led hit and a PC action whose targets include a PC start no slot 4
    CtlBrain br;
    br.once = false;
    ScriptedRng rng(Lo::Fallback::Lo);
    auto battle = fresh(br, rng);
    battle->unit_mut(3).hp = battle->unit_mut(3).max_hp = 9999;
    battle->do_weapon_attack(3, 0, 0);
    battle->finish_external_action();
    CHECK_EQ(br.reactions, 0);
    battle->do_weapon_attack(0, 1, 0);
    battle->do_weapon_attack(0, 3, 0);
    battle->finish_external_action();
    CHECK_EQ(br.reactions, 0);
  }
}

// A battle script that writes an enemy's HP to 0 (the final boss's attacked script after the Chrono Cross order) still starts its kill script
// once, and the code that script forces ends the fight.
struct HpWriteBrain final : EnemyBrain {
  int kills = 0, forced = -1;
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::None;
    return action;
  }
  int forced_result() const override { return forced; }
  void on_attacked(Battle& battle, int unit, Rng&, std::vector<EnemyAction>&) override {
    if (unit < kPcSlots) return;
    battle.unit_mut(unit).hp = 0;
    battle.unit_mut(unit).lo |= st::kKO;
  }
  void on_killed(Battle&, int, Rng&, std::vector<EnemyAction>&) override {
    ++kills;
    forced = 4;
  }
};

void test_scripted_hp_write_runs_kill_script() {
  data::GameTables tables = make_tables();
  HpWriteBrain brain;
  ScriptedRng rng(Lo::Fallback::Lo);
  rng.push(0);
  rng.push(100);
  Battle battle(tables, rng, brain);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  battle.unit_mut(3).hp = battle.unit_mut(3).max_hp = 9999;
  battle.do_weapon_attack(0, 3, 0);
  battle.finish_external_action();
  CHECK_EQ(brain.kills, 1);
  CHECK(battle.outcome() == Outcome::Scripted);
  CHECK_EQ(battle.result_code(), 4);
  battle.finish_external_action();
  CHECK_EQ(brain.kills, 1);
}

// A kill script that queues a death cast and then forces a result (the Time Devourer's slot 3: 67, A4, 83): the code waits for the cast,
// also when the killing blow is a weapon command, whose end test runs right after the hit.
struct DeathCastBrain final : EnemyBrain {
  int forced = -1;
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::None;
    return action;
  }
  int forced_result() const override { return forced; }
  void on_killed(Battle&, int, Rng&, std::vector<EnemyAction>& out) override {
    EnemyAction cast;
    cast.kind = EnemyAction::Kind::Weapon;
    cast.target = 0;
    out.push_back(cast);
    forced = 3;
  }
};

void test_forced_result_waits_for_death_cast() {
  data::GameTables tables = make_tables();
  DeathCastBrain brain;
  ScriptedRng rng(Lo::Fallback::Lo);
  rng.push(0);
  rng.push(100);
  Battle battle(tables, rng, brain);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  battle.unit_mut(3).hp = 1;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.target = 3;
  attack.chain = {0};
  CHECK(battle.apply(attack));
  CHECK(battle.outcome() == Outcome::Scripted);
  CHECK_EQ(battle.result_code(), 3);
  bool cast = false;
  for (const ActionEvent& event : battle.events()) cast = cast || (event.death_cast && event.actor == 3);
  CHECK(cast);
}

// Debug tools: invincible sides and the end test after a primitive called outside the scheduler (Debug tab cast).
void test_debug_flags() {
  {
    // invincible enemies: the lethal hit leaves the enemy at 1 HP, the battle goes on
    Fixture fixture;
    fixture.battle.set_invincible(false, true);
    fixture.battle.unit_mut(3).hp = 1;
    fixture.battle.do_weapon_attack(0, 3, 2);
    fixture.battle.finish_external_action();
    CHECK(fixture.battle.unit(3).alive());
    CHECK_EQ(fixture.battle.unit(3).hp, 1);
    CHECK(fixture.battle.outcome() == Outcome::Ongoing);
    // Off again: the same hit kills, and the end test runs without another command.
    fixture.battle.set_invincible(false, false);
    fixture.battle.do_weapon_attack(0, 3, 2);
    CHECK(!fixture.battle.unit(3).alive());
    fixture.battle.finish_external_action();
    CHECK(fixture.battle.outcome() == Outcome::Victory);
  }
  {
    // invincible party: an enemy hit cannot take a PC below 1 HP; the enemies stay mortal
    Fixture fixture;
    fixture.battle.set_invincible(true, false);
    fixture.battle.unit_mut(0).hp = 1;
    fixture.battle.do_weapon_attack(3, 0, 2);
    CHECK(fixture.battle.unit(0).alive());
    CHECK_EQ(fixture.battle.unit(0).hp, 1);
    CHECK(fixture.battle.invincible(0) && !fixture.battle.invincible(3));
    fixture.battle.unit_mut(3).hp = 1;
    fixture.battle.do_weapon_attack(0, 3, 2);
    CHECK(!fixture.battle.unit(3).alive());
  }
}

// Cheats God mode = hits on a PC do no damage (value 0, no pop), HP kept full, KO revived, negative statuses cleared; no stamina / gauge.
void test_god_mode() {
  Fixture fixture;
  Battle::Cheats cheats;
  cheats.god_mode = true;
  fixture.battle.set_cheats(cheats);
  fixture.battle.unit_mut(0).stamina = 20;
  // A negative status of the 0xAA9EE939 set.
  fixture.battle.unit_mut(0).lo |= 0x8u;
  for (int k = 0; k < 6; ++k) fixture.battle.do_weapon_attack(3, 0, 2);
  CHECK_EQ(fixture.battle.unit(0).hp, fixture.battle.unit(0).max_hp);
  CHECK(fixture.battle.unit(0).alive());
  for (const auto& event : fixture.battle.events())
    if (event.target == 0 && event.kind == ActionEvent::Kind::Weapon) CHECK_EQ(event.value, 0);
  fixture.battle.finish_external_action();
  CHECK((fixture.battle.unit(0).lo & 0xAA9EE939u) == 0);
  // Stamina is Battle boost / Max stamina's job.
  CHECK_EQ(fixture.battle.unit(0).stamina, 20);
  fixture.battle.unit_mut(1).hp = 0;
  fixture.battle.unit_mut(1).lo |= st::kKO;
  fixture.battle.finish_external_action();
  CHECK(fixture.battle.unit(1).alive());
  CHECK_EQ(fixture.battle.unit(1).hp, fixture.battle.unit(1).max_hp);
  // Frozen (lo 0x20000000) is in the cure mask: a frozen PC, which is not alive(), is freed as well.
  fixture.battle.unit_mut(2).lo |= st::kDefeated;
  fixture.battle.unit_mut(2).timers[29] = 5;
  fixture.battle.finish_external_action();
  CHECK(fixture.battle.unit(2).alive());
  CHECK(fixture.battle.unit(2).timers.count(29) == 0);
}

// Battle boost (RDE): a hit's damage on a PC is cancelled into the miss result (code 2, no value; nothing clamped), the hit strips the
// standing PCs' inflicted negative statuses (innate ones stay), the gauge is held full; HP and stamina are never refilled.
void test_battle_boost() {
  Fixture fixture;
  Battle::Cheats cheats;
  cheats.boost = true;
  fixture.battle.set_cheats(cheats);
  Unit& pc = fixture.battle.unit_mut(0);
  pc.hp = pc.max_hp / 3;
  pc.stamina = 20;
  pc.lo |= 0x8u | 0x20u;
  pc.perm_lo |= 0x20u;
  fixture.battle.unit_mut(1).lo |= 0x1u;
  const int hp0 = pc.hp;
  for (int k = 0; k < 6; ++k) fixture.battle.do_weapon_attack(3, 0, 2);
  CHECK_EQ(fixture.battle.unit(0).hp, hp0);
  CHECK(fixture.battle.unit(0).alive());
  int hits = 0;
  for (const ActionEvent& event : fixture.battle.events())
    if (event.target == 0 && event.kind == ActionEvent::Kind::Weapon && event.code != 2) ++hits;
  CHECK_EQ(hits, 0);
  CHECK((fixture.battle.unit(0).lo & 0x28u) == 0x20u);
  CHECK((fixture.battle.unit(1).lo & 0x1u) == 0);
  fixture.battle.finish_external_action();
  CHECK_EQ(fixture.battle.unit(0).hp, hp0);
  CHECK_EQ(fixture.battle.unit(0).stamina, 20);
  CHECK_EQ(fixture.battle.unit(0).gauge, fixture.battle.unit(0).gauge_max);
  // A knocked-out PC stays down: boost revives nobody and refills no HP.
  fixture.battle.unit_mut(1).hp = 0;
  fixture.battle.unit_mut(1).lo |= st::kKO;
  fixture.battle.finish_external_action();
  CHECK(!fixture.battle.unit(1).alive());
  CHECK_EQ(fixture.battle.unit(1).hp, 0);
  // The enemies still take damage.
  const int enemy_hp = fixture.battle.unit(3).hp;
  fixture.battle.do_weapon_attack(0, 3, 2);
  CHECK(fixture.battle.unit(3).hp < enemy_hp);
}

// RDE auto-battle's hit pick: the strongest affordable hit above 79 %, weak otherwise, -1 when weak cannot be paid.
void test_auto_hit_level() {
  Fixture fixture;
  fixture.battle.unit_mut(0).lo &= ~st::kFatigued;
  fixture.battle.unit_mut(0).stamina = 70;
  const int top = fixture.battle.auto_hit_level(0, 3, true);
  CHECK(top >= 0 && top <= 2);
  fixture.battle.unit_mut(0).stamina = 25;
  CHECK(fixture.battle.auto_hit_level(0, 3, true) <= 1);
  fixture.battle.unit_mut(0).stamina = 10;
  CHECK_EQ(fixture.battle.auto_hit_level(0, 3, true), 0);
  fixture.battle.unit_mut(0).stamina = 9;
  CHECK_EQ(fixture.battle.auto_hit_level(0, 3, true), -1);
}

// A frozen enemy stays on the target ring: an Attack aimed at it lands, and the scripts' unit mask keeps it.
void test_frozen_enemy_targetable() {
  Fixture fixture;
  Unit& enemy = fixture.battle.unit_mut(3);
  enemy.lo |= st::kDefeated;
  const int hp0 = enemy.hp;
  CHECK(enemy.standing() && !enemy.alive());
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.target = 3;
  attack.chain = {0};
  CHECK(fixture.battle.validate(attack));
  CHECK(fixture.battle.apply(attack));
  CHECK(fixture.battle.unit(3).hp < hp0);
  fixture.battle.unit_mut(3).hp = 0;
  CHECK(!fixture.battle.unit(3).standing());
}

// The auto pick reads the raw HIT stat minus EVD for a fresh PC / target pair, then (after a confirmed hit there) the HIT% box figure.
void test_auto_hit_level_fresh_pair() {
  Fixture fixture;
  Unit& pc = fixture.battle.unit_mut(0);
  pc.lo &= ~st::kFatigued;
  pc.stamina = 70;
  pc.hit = pc.hit_med = pc.hit_heavy = 50;
  pc.hit_ovr = {120, 120, 120};
  fixture.battle.unit_mut(3).evd = 0;
  CHECK_EQ(fixture.battle.hit_figure_fresh(0, 3, 2), 50);
  CHECK_EQ(fixture.battle.auto_hit_level(0, 3, true), 0);
  pc.hit_heavy = 90;
  CHECK_EQ(fixture.battle.auto_hit_level(0, 3, true), 2);
  pc.hit_heavy = 50;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.target = 3;
  attack.chain = {0};
  CHECK(fixture.battle.apply(attack));
  CHECK_EQ(fixture.battle.auto_hit_level(0, 3, true), 2);
}

// The Dopplegang pool (the kill credit; the mask is the save's) and the unit swap.
void test_dopple_pool_and_form() {
  Fixture fixture;
  // Pool slot 21: word 1, bit 5. Level 0, column 0 holds element 1.
  fixture.tables.enemies[0].raw[0x2B] = 21;
  fixture.tables.enemies[0].raw[0xA6] = 0;
  fixture.tables.enemies[0].raw[0x30] = 1;
  fixture.tables.enemies[0].raw[0x40] = 1;
  Battle battle{fixture.tables, fixture.rng, fixture.brain};
  fixture.rng.push(0);
  fixture.rng.push(100);
  BattleOptions options;
  std::array<u16, 16> mask{};
  mask[0] = 0x0003;
  options.dopple_mask = mask;
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(26, "Sprigg")}, 0, options));
  CHECK(battle.dopple_has(0) && battle.dopple_has(1) && !battle.dopple_has(21));
  CHECK_EQ(battle.dopple_pool().size(), 2u);
  // The last PC is not Sprigg and wears no 0xFC, so there is no credit; slot 2 = character 26 earns it.
  CHECK(!battle.dopple_credit(3, 0));
  CHECK(battle.dopple_credit(3, 2));
  CHECK(battle.dopple_has(21) && battle.dopple_mask()[1] == 0x20);
  // Already in the pool.
  CHECK(!battle.dopple_credit(3, 2));
  battle.unit_mut(2).eq.acc[1] = 0xFC;
  battle.unit_mut(2).src_id = 5;
  fixture.tables.enemies[0].raw[0x2B] = 7;
  // Accessory 0xFC earns credit as well.
  CHECK(battle.dopple_credit(3, 2));
  CHECK(battle.dopple_has(7));
  fixture.tables.enemies[0].raw[0xA6] = 0x80;
  fixture.tables.enemies[0].raw[0x2B] = 9;
  // Bit 7 of the enemy flag byte marks it not doppelgangable.
  CHECK(!battle.dopple_credit(3, 2) && !battle.dopple_has(9));
  // the swap
  battle.unit_mut(2).src_id = 26;
  battle.unit_mut(2).hp = 33;
  const int atk0 = battle.unit(2).atk, max0 = battle.unit(2).max_hp;
  CHECK(battle.transform(2, 0));
  CHECK_EQ(battle.unit(2).form, 0);
  CHECK_EQ(battle.unit(2).atk, 4);
  CHECK_EQ(battle.unit(2).hp, 33);
  CHECK_EQ(battle.unit(2).max_hp, max0);
  CHECK((battle.unit(2).cells[0] & 0x3FFF) == 1 && battle.unit(2).usable[0]);
  CHECK(battle.unit(2).pc && battle.unit(2).countdown == 8 && battle.unit(2).gauge == -1);
  battle.unit_mut(2).hp = 20;
  CHECK(battle.transform(2, -1));
  CHECK_EQ(battle.unit(2).form, -1);
  CHECK_EQ(battle.unit(2).atk, atk0);
  CHECK_EQ(battle.unit(2).hp, 20);
  CHECK(!battle.transform(2, -1));
}

// A PC still in a creature form when the fight ends leaves it as itself. Captured on the PS1 and RDE (room 53, Serge + Sprigg at 300 HP,
// form 0x43, form HP set to 123): the record keeps 123 HP and Sprigg's own stats. With Pip in the party and his form change after
// hers, RDE gives the record the unit from before the form, HP 300 included (the PS1 kept the creature's stats and kind there).
void test_dopple_exit_unit() {
  const auto run = [](bool pip_changes) {
    Fixture fixture;
    Battle battle{fixture.tables, fixture.rng, fixture.brain};
    fixture.rng.push(0);
    fixture.rng.push(100);
    CHECK(battle.start({make_member(0, "Serge"), make_member(26, "Sprigg", 300), make_member(43, "Pip")}, 0));
    battle.unit_mut(1).lo |= 0x01;
    const int atk0 = battle.unit(1).atk;
    CHECK(battle.transform(1, 0));
    CHECK(battle.unit(1).atk != atk0);
    battle.unit_mut(1).hp = 123;
    if (pip_changes) battle.evolve(2, 0x2C);
    const Unit own = battle.exit_unit(1);
    CHECK_EQ(own.form, -1);
    CHECK_EQ(own.atk, atk0);
    CHECK_EQ(own.lo & 0x01, 0x01u);
    battle.unit_mut(3).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.chain = {2};
    CHECK(battle.apply(attack));
    CHECK(battle.outcome() == Outcome::Victory);
    CHECK_EQ(battle.party()[1].lo_persist & 0x01, 0x01u);
    return battle.party()[1].hp;
  };
  CHECK_EQ(run(false), 123);
  CHECK_EQ(run(true), 300);
}

// A PC starts from the whole status byte of its record (PS1, room 53: record bits 0x39, 0x02, 0x04 and 0x40 each showed in the
// unit's lo word at the first command menu); the record keeps only lo & 0x39 after the fight.
void test_record_status_carry() {
  Fixture fixture;
  Battle battle{fixture.tables, fixture.rng, fixture.brain};
  PartyMember serge = make_member(0, "Serge");
  serge.lo_persist = 0x7F;
  CHECK(battle.start({serge}, 0));
  CHECK_EQ(battle.unit(0).lo & 0xFF, 0x7Fu);
  battle.unit_mut(3).hp = 1;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {2};
  CHECK(battle.apply(attack));
  CHECK(battle.outcome() == Outcome::Victory);
  CHECK_EQ(battle.party()[0].lo_persist, 0x39u);
}

// The way back brings the saved unit back, its stamina too, with the HP of now, countdown 8 and the max HP of the last unit that changed
// form from a character body, which the record keeps. Captured on the PS1 (room 53, Sprigg at 300 HP + Pip at 85 + Serge; form 0x43, her
// HP held at 123, the saved unit's stamina set to 30 and hers to 50, then the way back): 123 / 300 and stamina 30; with Pip's form change
// before the way back 123 / 85 and stamina 30 (RDE 123 / 85 too), the record 135 / 135 after the growth.
void test_dopple_way_back_after_pip_form() {
  for (const bool pip_changes : {true, false}) {
    Fixture fixture;
    Battle battle{fixture.tables, fixture.rng, fixture.brain};
    fixture.rng.push(0);
    fixture.rng.push(100);
    CHECK(battle.start({make_member(26, "Sprigg", 300), make_member(43, "Pip", 85), make_member(0, "Serge")}, 0));
    battle.unit_mut(0).lo |= 0x01;
    battle.unit_mut(0).stamina = 30;
    battle.unit_mut(0).used[0] = true;
    const Unit saved = battle.unit(0);
    CHECK(battle.transform(0, 0));
    if (pip_changes) battle.evolve(1, 0x2C);
    battle.unit_mut(0).hp = 123;
    battle.unit_mut(0).stamina = 50;
    CHECK(battle.transform(0, -1));
    const Unit& unit = battle.unit(0);
    CHECK_EQ(unit.form, -1);
    CHECK_EQ(unit.hp, 123);
    CHECK_EQ(unit.stamina, 30);
    CHECK(unit.countdown == 8 && unit.gauge == -1 && unit.ap == 8);
    CHECK(unit.atk == saved.atk && unit.def == saved.def && unit.mag == saved.mag && unit.mdef == saved.mdef);
    CHECK_EQ(unit.lo, saved.lo);
    CHECK(unit.cells == saved.cells && unit.used[0]);
    CHECK_EQ(unit.max_hp, pip_changes ? 85 : 300);
    CHECK_EQ(battle.party()[0].stat[0], pip_changes ? 85 : 300);
  }
}

// Gear HP% raises the battle-start HP, not the max HP. Captured on the PS1 and RDE (room 53, Pip at 85 / 85 wearing Earring of Hope, 15%):
// his unit starts at 97 / 85; his form change then hands max HP 85 to Sprigg's way back (PS1 123 / 85). A creature form taken after Pip's
// form change gets that max HP as well (PS1 and RDE 123 / 85), and the fight's end gives Sprigg her whole saved unit back (RDE 300 / 300).
void test_gear_hp_pct_and_forms() {
  data::GameTables tables = make_tables();
  tables.gear.resize(256);
  tables.gear[0xE1].raw[4] = 15, tables.gear[0xE1].raw[8] = 0x01;
  ScriptedRng rng{Lo::Fallback::Lo};
  NullBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  PartyMember pip = make_member(43, "Pip", 85);
  pip.eq = make_equip(tables, 0, 0, {0xE1, 0, 0});
  PartyMember hurt = make_member(1, "B", 50);
  hurt.hp = 40;
  hurt.eq = make_equip(tables, 0, 0, {0xE1, 0xE1, 0});
  CHECK(battle.start({make_member(26, "Sprigg", 300), pip, hurt}, 0));
  CHECK(battle.unit(1).hp == 97 && battle.unit(1).max_hp == 85);
  // Sequential per accessory: 40 -> 46 -> 52.
  CHECK(battle.unit(2).hp == 52 && battle.unit(2).max_hp == 50);
  CHECK(battle.transform(0, 0));
  battle.evolve(1, 0x2C);
  battle.unit_mut(0).hp = 123;
  CHECK(battle.transform(0, 0));
  CHECK(battle.unit(0).form == 0 && battle.unit(0).hp == 123 && battle.unit(0).max_hp == 85);
  const Unit own = battle.exit_unit(0);
  CHECK(own.form == -1 && own.hp == 300 && own.max_hp == 300);
  CHECK(battle.transform(0, -1));
  CHECK(battle.unit(0).hp == 123 && battle.unit(0).max_hp == 85 && battle.party()[0].stat[0] == 85);
}

// A creature form grows with the stars left above its threshold (record +0xA6 & 0x7F) by the +0xA7 selectors: the PS1 turns
// Scorpiod's ATK DEF MAG MDEF 30 5 10 0 (threshold 13, selectors 0x7A) into 34 9 19 2 with 50 stars left; at the threshold nothing grows.
void test_dopple_form_star_growth() {
  const auto form_stats = [](int stars_left) {
    Fixture fixture;
    data::StatRec& rec = fixture.tables.enemies[0];
    rec.raw[0x18] = 30, rec.raw[0x19] = 5, rec.raw[0x1A] = 100, rec.raw[0x1B] = 0, rec.raw[0x1C] = 10, rec.raw[0x1D] = 0;
    rec.raw[0xA6] = 0x0D, rec.raw[0xA7] = 0x7A;
    Battle battle{fixture.tables, fixture.rng, fixture.brain};
    fixture.rng.push(0);
    fixture.rng.push(100);
    BattleOptions options;
    options.growth.counter = stars_left;
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(26, "Sprigg")}, 0, options));
    CHECK(battle.transform(0, 0));
    const Unit& unit = battle.unit(0);
    return std::array<int, 5>{unit.atk, unit.def, unit.hit, unit.mag, unit.mdef};
  };
  CHECK((form_stats(50) == std::array<int, 5>{34, 9, 100, 19, 2}));
  CHECK((form_stats(13) == std::array<int, 5>{30, 5, 100, 10, 0}));
}

// Every Anti-X record (mode 1, pgdat +4 0x80, +5 the element's own colour, +0x18 the seal word) lands its seal word on the target's lo (Anti-Blue
// cast at an enemy turns its lo 0x400 into 0x80000400 at the action start), and the merged lo & the colour's mask then
// seals exactly the grid cells of that colour: element_sealed true, element_usable false, the other colours untouched.
void test_anti_colour_seals() {
  // pgdat: id, own colour byte, seal word: Anti-Red 50 / Anti-Blue 56 / Anti-Yellow 62 / Anti-Green 68 / Anti-Black 74 / Anti-White 80
  struct Anti { int colour_byte; u32 seal; } const antis[6] = {{0x80, 0x800000u}, {0x20, 0x80000000u}, {0x04, 0x08000000u}, {0x40, 0x800u}, {0x08, 0x80000u}, {0x10, 0x8000u}};
  // Cell colours in order: green white black red yellow blue (the seal masks are 0x800, 0x8000, 0x80000, 0x800000, 0x8000000, 0x80000000).
  const u8 cell_colour[6] = {0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
  const u32 seal_of[6] = {0x800u, 0x8000u, 0x80000u, 0x800000u, 0x8000000u, 0x80000000u};
  for (const Anti& anti : antis) {
    Fixture fixture;
    CHECK(fixture.started);
    fixture.tables.elements.resize(12);
    // Elements 4..9: one plain element per colour, placed in Serge's grid cells 1..6.
    for (int cell = 0; cell < 6; ++cell) {
      fixture.tables.elements[static_cast<size_t>(4 + cell)].raw[5] = cell_colour[cell];
      fixture.tables.elements[static_cast<size_t>(4 + cell)].raw[6] = 1;
      fixture.tables.elements[static_cast<size_t>(4 + cell)].raw[7] = 0;
      Unit& unit = fixture.battle.unit_mut(0);
      unit.cells[static_cast<size_t>(1 + cell)] = static_cast<u16>(4 + cell);
      unit.usable[static_cast<size_t>(1 + cell)] = true;
    }
    // The Anti-X record as pgdat has it.
    data::ElemRec& record = fixture.tables.elements[10];
    record.raw[4] = 0x80, record.raw[5] = static_cast<u8>(anti.colour_byte), record.raw[6] = 3, record.raw[7] = 1, record.raw[0xA] = 100;
    record.raw[0x18] = static_cast<u8>(anti.seal), record.raw[0x19] = static_cast<u8>(anti.seal >> 8), record.raw[0x1A] = static_cast<u8>(anti.seal >> 16), record.raw[0x1B] = static_cast<u8>(anti.seal >> 24);
    fixture.battle.unit_mut(0).stamina = 70;
    fixture.battle.unit_mut(0).gauge = 7;
    // An enemy casts it at Serge.
    fixture.battle.do_element(3, 10, 0, 0);
    CHECK((fixture.battle.unit(0).lo & anti.seal) == anti.seal);
    CHECK(fixture.battle.unit(0).alive());
    for (int cell = 0; cell < 6; ++cell) {
      const bool sealed = seal_of[cell] == anti.seal;
      CHECK(fixture.battle.element_sealed(0, 1 + cell) == sealed);
      CHECK(fixture.battle.element_usable(0, 1 + cell) == !sealed);
    }
  }
}

// Element modes 13 (Revenge), 43 (flat heal), 46 (Piggy Boink), 51 (hero-set weapon tech).
void test_element_modes_revenge_heal_piggy_hero() {
  const auto add_element = [](data::GameTables& tables, size_t id, int mode, int power, int chance, int arg) {
    if (tables.elements.size() <= id) tables.elements.resize(id + 1);
    auto& element = tables.elements[id];
    element.raw[5] = 0x80, element.raw[6] = 1, element.raw[7] = static_cast<u8>(mode), element.raw[8] = static_cast<u8>(power);
    element.raw[0xA] = static_cast<u8>(chance), element.raw[0xE] = static_cast<u8>(arg);
  };
  {
    // Revenge: the caster's inflicted negative statuses land on the target and leave the caster with their timers; a permanent one stays.
    Fixture fixture;
    add_element(fixture.tables, 4, 13, 0, 100, 0);
    Unit& caster = fixture.battle.unit_mut(3);
    caster.lo = st::kPoison | st::kAfraid | 0x400;
    caster.perm_lo = 0x400;
    caster.timers[0] = 9;
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK_EQ(fixture.battle.unit(0).lo & (st::kPoison | st::kAfraid), st::kPoison | st::kAfraid);
    CHECK_EQ(fixture.battle.unit(0).lo & ~fixture.battle.unit(0).perm_lo & 0x400u, 0);
    CHECK_EQ(fixture.battle.unit(3).lo, 0x400);
    CHECK(fixture.battle.unit(3).timers.count(0) == 0);
    CHECK_EQ(fixture.battle.events().back().code, 0);
  }
  {
    // The gate rnd(1, 99) <= +0xA fails: a miss, nothing moves.
    Fixture fixture;
    add_element(fixture.tables, 4, 13, 0, 50, 0);
    fixture.battle.unit_mut(3).lo = st::kPoison;
    fixture.rng.push(51);
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK_EQ(fixture.battle.unit(3).lo, st::kPoison);
    CHECK_EQ(fixture.battle.unit(0).lo & st::kPoison, 0);
    CHECK_EQ(fixture.battle.events().back().code, 2);
  }
  {
    // Mode 43 heals the flat +0xA, x125/100 with the caster's damage-up gear.
    Fixture fixture;
    add_element(fixture.tables, 4, 43, 0, 30, 0);
    fixture.battle.unit_mut(0).hp = 5;
    fixture.battle.do_element(1, 4, 0, 0);
    CHECK_EQ(fixture.battle.unit(0).hp, 35);
    CHECK_EQ(fixture.battle.events().back().code, 3);
    fixture.battle.unit_mut(0).hp = 5;
    fixture.battle.unit_mut(1).eq.gear_flags = data::gearflag::kDmgUp;
    fixture.battle.do_element(1, 4, 0, 0);
    CHECK_EQ(fixture.battle.unit(0).hp, 42);
  }
  {
    // Piggy Boink: ATK / DEF damage with power + the savings; the hit wakes the target.
    const auto piggy_damage = [&](int savings, bool& woke) {
      data::GameTables tables = make_tables();
      add_element(tables, 4, 46, 30, 0, 0);
      ScriptedRng rng{Lo::Fallback::Lo};
      NullBrain brain;
      Battle battle{tables, rng, brain};
      rng.push(0);
      rng.push(100);
      BattleOptions opt;
      opt.piggy_bank = savings;
      battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0, opt);
      battle.unit_mut(3).lo |= st::kAsleep;
      const int hp0 = battle.unit(3).hp;
      battle.do_element(0, 4, 0, 3);
      woke = (battle.unit(3).lo & st::kAsleep) == 0;
      return hp0 - battle.unit(3).hp;
    };
    bool woke = false;
    const int plain = piggy_damage(0, woke);
    CHECK(plain > 0);
    CHECK(woke);
    CHECK(piggy_damage(7, woke) > plain);
  }
  {
    // Slap of Cyrus: a miss without the hero set; with weapon 0x3D and two of the hero accessories it is the weapon tech.
    Fixture fixture;
    add_element(fixture.tables, 4, 51, 10, 0, 1);
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.unit(3).hp, hp0);
    CHECK_EQ(fixture.battle.events().back().code, 2);
    Unit& hero = fixture.battle.unit_mut(0);
    hero.hero_set = true;
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK(fixture.battle.unit(3).hp < hp0);
    hero.hero_set = false;
    const int hp1 = fixture.battle.unit(3).hp;
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.unit(3).hp, hp1);
  }
}

// A sprained attacker (lo 0x10) has its weapon term (after the hero doubling) and its MAG term cut to x300/400: weapon 16 hits as 12,
// 32 (the hero set) as 24, and MAG gear 3 (term 4) casts as gear 2 (term 3).
void test_sprained_attacker() {
  const auto weak_hit = [](bool sprained, bool hero, int wp) {
    Fixture fixture;
    Unit& attacker = fixture.battle.unit_mut(0);
    if (sprained) attacker.lo |= st::kSprain;
    attacker.hero_set = hero;
    attacker.eq.wp = wp;
    fixture.battle.unit_mut(3).innate = 0;
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_weapon_attack(0, 3, 0);
    return hp0 - fixture.battle.unit(3).hp;
  };
  CHECK(weak_hit(true, false, 16) < weak_hit(false, false, 16));
  CHECK_EQ(weak_hit(true, false, 16), weak_hit(false, false, 12));
  CHECK_EQ(weak_hit(true, true, 16), weak_hit(false, false, 24));
  const auto aqua_beam = [](bool sprained, int mag_gear) {
    Fixture fixture;
    Unit& caster = fixture.battle.unit_mut(0);
    if (sprained) caster.lo |= st::kSprain;
    caster.eq.mag_gear = mag_gear;
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_element(0, 1, 0, 3);
    return hp0 - fixture.battle.unit(3).hp;
  };
  CHECK(aqua_beam(true, 3) < aqua_beam(false, 3));
  CHECK_EQ(aqua_beam(true, 3), aqua_beam(false, 2));
}

// The colour multipliers of four PS1 enemy casts, from the inputs the PS1 held at the cast (attacker innate = attack colour, the ring
// as it stood, target white): Multistab 1250 (68 damage from a raw 54), Gnaw Bones 1000, Slash All 1000, element 23 1125.
void test_ps1_cast_colours() {
  CHECK_EQ(colour_multiplier(5, 5, {5, 0, 4}, 1), 1250);
  CHECK_EQ(54 * 1250 / 1000 * 102 / 100, 68);
  CHECK_EQ(colour_multiplier(3, 3, {5, 0, 4}, 1), 1000);
  CHECK_EQ(colour_multiplier(1, 1, {0, 4, 1}, 1), 1000);
  CHECK_EQ(colour_multiplier(4, 4, {5, 0, 4}, 1), 1125);
}

// The hero set (weapon 0x3D with two of 0xC0 / 0xC1) doubles the wearer's weapon term, lets its crit roll succeed from 7, and raises
// the evade its targets oppose to its hits by x125/100.
void test_hero_set() {
  ChanceIn chance;
  chance.base = 68, chance.evd = 8;
  chance.attacker_hero_set = true;
  CHECK_EQ(hit_chance(chance), 58);
  WeaponIn weapon;
  weapon.atk = 9, weapon.wp = 16, weapon.def = 0, weapon.arm = 4, weapon.mult = 10, weapon.colour_mille = 1000;
  for (const bool hero : {false, true}) {
    weapon.hero_set = hero;
    Lo rolls(Lo::Fallback::Lo);
    for (int i = 0; i < 13; ++i) rolls.push(0);
    rolls.push(7);
    CHECK_EQ(weapon_damage(rolls, weapon).crit, hero);
  }
  // A weak hit with every roll low: ATK 9, weapon 16 (32 doubled) against DEF 0, armour 4: raw 8 (17), x1125/1000 for the white attacker.
  const auto weak_hit = [](bool hero) {
    Fixture fixture;
    fixture.battle.unit_mut(0).hero_set = hero;
    fixture.battle.unit_mut(3).innate = 0;
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_weapon_attack(0, 3, 0);
    return hp0 - fixture.battle.unit(3).hp;
  };
  CHECK_EQ(weak_hit(false), 9);
  CHECK_EQ(weak_hit(true), 19);
  // The flag is fixed from the equipment at battle start; a Dopplegang form (the monster's gear) keeps it, and so does the way back.
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  NullBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  PartyMember wearer = make_member(0, "A");
  wearer.eq.weapon_id = 0x3D;
  wearer.eq.acc = {0xC1, 0, 0xC0};
  PartyMember half = make_member(1, "B");
  half.eq.weapon_id = 0x3D;
  half.eq.acc = {0xC1, 0, 0};
  CHECK(battle.start({wearer, half}, 0));
  CHECK(battle.unit(0).hero_set && !battle.unit(1).hero_set);
  CHECK(battle.transform(0, 0));
  CHECK(battle.unit(0).eq.weapon_id != 0x3D && battle.unit(0).hero_set);
  CHECK(battle.transform(0, -1));
  CHECK(battle.unit(0).hero_set);
}

// A weapon tech (mode 29) is an element command: its hits take the record's colour, not the caster's innate one. The white caster's blue
// tech hits a red target as its opposite colour (x3/2); a white one does not.
void test_weapon_tech_record_colour() {
  const auto tech_damage = [](u8 colour_bit) {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    auto& record = fixture.tables.elements[4];
    record.raw[5] = colour_bit, record.raw[6] = 1, record.raw[7] = 29, record.raw[8] = 20, record.raw[0xE] = 1;
    fixture.battle.unit_mut(0).innate = 0x08;
    fixture.battle.unit_mut(3).innate = 0x20;
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_element(0, 4, 0, 3);
    return hp0 - fixture.battle.unit(3).hp;
  };
  const int white = tech_damage(0x08), blue = tech_damage(0x80);
  CHECK(white > 0);
  CHECK(blue > white);
}

// An element that can be sealed (id 0x12D at most, record byte 1 bit 0) cast by a caster sealed in its colour becomes the forced miss with
// the seal pack of the colour (190 + colour, 1 green .. 6 blue); the ring still takes the colour. An action's enemy target has its cells'
// seal flags judged like a PC's.
void test_sealed_caster_fizzles() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  battle.unit_mut(3).lo |= 0x80000000u;
  const int hp0 = battle.unit(0).hp;
  battle.drain_events();
  battle.do_element(3, 1, 0, 0);
  CHECK_EQ(battle.unit(0).hp, hp0);
  const auto ev = battle.drain_events();
  CHECK(!ev.empty());
  if (!ev.empty()) {
    CHECK_EQ(ev.front().fx_eid, 196);
    CHECK_EQ(ev.front().code, 2);
    CHECK(ev.front().silent);
    CHECK_EQ(ev.front().sys_msg, -1);
  }
  CHECK_EQ(battle.ring()[2], 5);
  // A record without byte 1 bit 0 is cast as it is.
  fixture.tables.elements[1].raw[1] = 0;
  battle.do_element(3, 1, 0, 0);
  CHECK(battle.unit(0).hp < hp0);
  const auto plain = battle.drain_events();
  CHECK(!plain.empty() && plain.front().fx_eid == -1);
  fixture.tables.elements[1].raw[1] = 1;
  // The enemy's blue cell is flagged once the enemy is an action's target; the flag clears when the seal is gone and it is a target again.
  Unit& enemy = battle.unit_mut(3);
  enemy.cells[5] = 1, enemy.usable[5] = true;
  enemy.cells[6] = 3, enemy.usable[6] = true;
  CHECK(!battle.unit(3).seal_flag[5]);
  battle.do_weapon_attack(0, 3, 0);
  CHECK(battle.unit(3).seal_flag[5]);
  CHECK(battle.unit(3).seal_flag[6]);
  battle.unit_mut(3).lo &= ~0x80000000u;
  battle.do_weapon_attack(0, 3, 0);
  CHECK(!battle.unit(3).seal_flag[5]);
}

// Every party action lowers all three PC countdowns (the actor's too) by its steps, every enemy action by 3; a confused PC whose countdown
// runs out takes its turn, paying the weak hit's stamina.
struct WeakHitBrain final : EnemyBrain {
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.target = 1;
    return action;
  }
};

void test_pc_countdowns_run_with_actions() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  WeakHitBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {0, 1};
  CHECK(battle.apply(attack));
  CHECK_EQ(battle.unit(0).countdown, 8 - 1 - 2);
  CHECK_EQ(battle.unit(1).countdown, 5);
  CHECK_EQ(battle.unit(2).countdown, 5);
  // The enemy acts at 0: the PCs lose 3 more.
  battle.unit_mut(3).countdown = 0;
  battle.settle();
  CHECK_EQ(battle.unit(0).countdown, 2);
  CHECK_EQ(battle.unit(2).countdown, 2);
  // PC 2 confused: the next weak hit takes it to 1, a medium one to 0, and its turn comes before the menu returns.
  battle.unit_mut(2).lo |= st::kConfused;
  battle.unit_mut(2).stamina = 70;
  battle.unit_mut(1).stamina = 70;
  Command next;
  next.kind = Command::Kind::Attack;
  next.actor = 1;
  next.chain = {1};
  CHECK(battle.apply(next));
  CHECK(battle.unit(2).stamina < 70);
  CHECK(battle.unit(2).countdown > 0);
}

// An enemy turn that interrupts a PC's open weapon turn keeps the PC's chain when it hits another PC (the PS1 keeps the counter at 1 after
// a weak hit and two enemy hits on Kid); a hit on the chain's PC, or an enemy turn after the PC's turn has ended, resets it.
struct TargetBrain final : EnemyBrain {
  int target = 1;
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.target = target;
    return action;
  }
};

void test_chain_across_interrupting_enemy_turn() {
  const auto chance_after = [](int enemy_target, int pc_stamina) {
    data::GameTables tables = make_tables();
    ScriptedRng rng{Lo::Fallback::Lo};
    TargetBrain brain;
    brain.target = enemy_target;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    battle.unit_mut(0).stamina = pc_stamina;
    battle.unit_mut(3).countdown = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = 0;
    attack.target = 3;
    attack.chain = {0};
    CHECK(battle.apply(attack));
    CHECK_EQ(battle.unit(3).countdown != 0, true);
    return battle.hit_chance_for(0, 3, 0);
  };
  Fixture fixture;
  const int fresh = fixture.battle.hit_chance_for(0, 3, 0);
  CHECK(chance_after(1, 70) > fresh);
  CHECK_EQ(chance_after(0, 70), fresh);
  CHECK_EQ(chance_after(1, 10), fresh);
}

// A landed weapon hit wakes the target and ends its confusion, timers included.
void test_weapon_hit_ends_confusion() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  Unit& target = battle.unit_mut(0);
  target.lo |= st::kAsleep | st::kConfused;
  target.timers[14] = 9, target.timers[20] = 9;
  battle.do_weapon_attack(3, 0, 0);
  CHECK_EQ(battle.unit(0).lo & (st::kAsleep | st::kConfused), 0u);
  CHECK(!battle.unit(0).timers.count(14) && !battle.unit(0).timers.count(20));
}

// Weapon techs: no crit roll (the hero set's threshold included; only the slayer mask), each hit wakes the target unless the record's lo
// mask names the status, and a drainer heals by the total after the second gear step.
void test_weapon_tech_wake_crit_drain() {
  WeaponIn input;
  input.atk = 9, input.wp = 16, input.def = 0, input.arm = 4, input.mult = 20, input.colour_mille = 1000, input.strength = 1;
  input.hero_set = true;
  Lo rolled(Lo::Fallback::Hi);
  CHECK(weapon_damage(rolled, input).crit);
  input.crit_roll = false;
  Lo none(Lo::Fallback::Hi);
  CHECK(!weapon_damage(none, input).crit);
  input.slayer = true;
  Lo slayer(Lo::Fallback::Hi);
  CHECK(weapon_damage(slayer, input).crit);

  Fixture fixture;
  Battle& battle = fixture.battle;
  fixture.tables.elements.resize(5);
  auto& record = fixture.tables.elements[4];
  record.raw[5] = 0x08, record.raw[6] = 1, record.raw[7] = 29, record.raw[8] = 20, record.raw[0xE] = 2;
  record.raw[0x1A] = 0x10;
  Unit& enemy = battle.unit_mut(3);
  enemy.lo |= st::kAsleep | st::kConfused;
  Unit& caster = battle.unit_mut(0);
  caster.hi |= st::kHiDrain;
  caster.hp = 10;
  const int hp0 = battle.unit(3).hp;
  battle.do_element(0, 4, 0, 3);
  const int dealt = hp0 - battle.unit(3).hp;
  CHECK(dealt > 0);
  CHECK_EQ(battle.unit(3).lo & st::kAsleep, 0u);
  CHECK(battle.unit(3).lo & st::kConfused);
  CHECK_EQ(battle.unit(0).hp, std::min(battle.unit(0).max_hp, 10 + dealt));
}

// A PC script that queues nothing for a confused PC (its confusion only permanent), enemies that hit unit 0 and note every unit's
// countdown on their first turn.
struct ConfusedScriptBrain final : EnemyBrain {
  std::array<int, kUnits> first_turn_countdowns{};
  bool seen = false;
  bool plan_turn(Battle&, int unit, Rng&, std::vector<EnemyAction>& out) override {
    out.clear();
    return unit < kPcSlots;
  }
  EnemyAction choose(const Battle& battle, int, Rng&) override {
    if (!seen) {
      for (int i = 0; i < kUnits; ++i) first_turn_countdowns[static_cast<size_t>(i)] = battle.unit(i).countdown;
      seen = true;
    }
    EnemyAction action;
    action.target = 0;
    return action;
  }
};

// A confused PC's script queues one entry (`kind`, a heavy weapon hit or element 1) on the enemy; the enemies wait.
struct ScriptedPcBrain final : EnemyBrain {
  EnemyAction::Kind kind = EnemyAction::Kind::Weapon;
  bool plan_turn(Battle&, int unit, Rng&, std::vector<EnemyAction>& out) override {
    out.clear();
    if (unit >= kPcSlots) return false;
    EnemyAction action;
    action.kind = kind;
    action.strength = 2;
    action.element = 1;
    action.target = 3;
    out.push_back(action);
    return true;
  }
  EnemyAction choose(const Battle&, int, Rng&) override {
    EnemyAction action;
    action.kind = EnemyAction::Kind::None;
    return action;
  }
};

// A PC's scripted weapon entry is upkept as one weak hit whatever its strength (stamina 10, poison 1); a scripted element entry as the
// menu's element command (stamina 70, poison 7).
void test_scripted_pc_entry_upkeep() {
  for (const EnemyAction::Kind kind : {EnemyAction::Kind::Weapon, EnemyAction::Kind::Element}) {
    data::GameTables tables = make_tables();
    ScriptedRng rng{Lo::Fallback::Lo};
    ScriptedPcBrain brain;
    brain.kind = kind;
    Battle battle{tables, rng, brain};
    rng.push(0);
    rng.push(100);
    CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
    for (int i = 0; i < 3; ++i) battle.unit_mut(i).lo |= 1u;
    battle.unit_mut(0).lo |= st::kConfused;
    battle.unit_mut(0).stamina = 70;
    battle.unit_mut(0).countdown = 0;
    battle.unit_mut(3).countdown = 200;
    const int hp1 = battle.unit(1).hp;
    battle.settle();
    const bool weapon = kind == EnemyAction::Kind::Weapon;
    CHECK_EQ(battle.unit(1).hp, hp1 - (weapon ? 1 : 7));
    CHECK_EQ(battle.unit(0).stamina, weapon ? 60 : 0);
  }
}

// Serge alone and confused (permanently) with enemies at 17 and 1: no PC can act, so time runs once as the menu finds nobody and again in
// the scheduler pass, which takes every countdown to 0 before anyone acts; his empty script turn makes no attack and ends with the A4
// countdown; time keeps running every pass, so the enemies' fresh countdowns are wiped and they act in turn.
void test_time_runs_while_nobody_can_act() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  ConfusedScriptBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0));
  battle.unit_mut(4) = battle.unit(3);
  battle.unit_mut(1).lo |= st::kKO;
  battle.unit_mut(2).lo |= st::kKO;
  Unit& serge = battle.unit_mut(0);
  serge.lo |= st::kConfused, serge.perm_lo |= st::kConfused;
  serge.stamina = 40;
  battle.unit_mut(3).countdown = 17;
  battle.unit_mut(4).countdown = 1;
  battle.drain_events();
  battle.settle();
  CHECK(brain.seen);
  CHECK_EQ(brain.first_turn_countdowns[0], 0);
  CHECK_EQ(brain.first_turn_countdowns[3], 0xFFFF);
  CHECK_EQ(brain.first_turn_countdowns[4], 0);
  int weapon_by_serge = 0, enemy_turns = 0;
  for (const ActionEvent& ev : battle.drain_events()) {
    if (ev.kind == ActionEvent::Kind::Weapon && ev.actor == 0) ++weapon_by_serge;
    if (ev.kind == ActionEvent::Kind::Weapon && ev.actor >= kPcSlots) ++enemy_turns;
  }
  CHECK_EQ(weapon_by_serge, 0);
  CHECK(enemy_turns > 1);
  CHECK(battle.outcome() == Outcome::Wiped);
}

// Weapon techs on a weapon-immune target: every hit is -1 and the result a miss, yet the sum is taken off the HP, so three hits raise it
// by 3 past its max (one hit changes nothing); a weapon absorber is healed by the sum, up to its max.
void test_weapon_tech_immune_absorb() {
  const auto cast = [](int hits, u32 hi, int hp) {
    auto fixture = std::make_unique<Fixture>();
    fixture->tables.elements.resize(5);
    auto& record = fixture->tables.elements[4];
    record.raw[5] = 0x08, record.raw[6] = 1, record.raw[7] = 29, record.raw[8] = 20, record.raw[0xE] = static_cast<u8>(hits);
    Unit& enemy = fixture->battle.unit_mut(3);
    enemy.hi |= hi;
    enemy.hp = hp;
    fixture->battle.drain_events();
    fixture->battle.do_element(0, 4, 0, 3);
    const auto ev = fixture->battle.drain_events();
    return std::pair<int, int>{fixture->battle.unit(3).hp, ev.empty() ? -1 : ev.front().code};
  };
  Fixture probe;
  const int max_hp = probe.battle.unit(3).max_hp;
  CHECK((cast(3, st::kHiWeaponImmune, max_hp) == std::pair<int, int>{max_hp + 3, 2}));
  CHECK((cast(1, st::kHiWeaponImmune, 100) == std::pair<int, int>{100, 2}));
  const auto [absorbed_hp, absorbed_code] = cast(3, st::kHiWeaponAbsorb, 10);
  CHECK(absorbed_hp > 10);
  CHECK_EQ(absorbed_code, 3);
  CHECK_EQ(cast(3, st::kHiWeaponAbsorb, max_hp).first, max_hp);
}

// Hi status 0x20000: the unit's weapon hits and elements cost no stamina.
void test_no_stamina_cost_status() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  battle.unit_mut(0).hi |= st::kHiNoStaminaCost;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {0, 1};
  CHECK(battle.apply(attack));
  CHECK_EQ(battle.unit(0).stamina, 70);
  Command element;
  element.kind = Command::Kind::Element;
  element.actor = 0;
  element.cell = 0;
  CHECK(battle.apply(element));
  CHECK_EQ(battle.unit(0).stamina, 70);
}

// The HIT% visit budget: the stamina, plus 10 per step left on the no-stamina-cost timer while the unit has the status. Under the status a
// hit starts while the stamina covers its unfatigued cost.
void test_hit_budget() {
  Fixture fixture;
  Battle& battle = fixture.battle;
  CHECK_EQ(battle.hit_budget(0), 70);
  Unit& pc = battle.unit_mut(0);
  pc.hi |= st::kHiNoStaminaCost;
  pc.timers[32 + 17] = 9;
  CHECK_EQ(battle.hit_budget(0), 160);
  battle.unit_mut(3).max_hp = battle.unit_mut(3).hp = 9999;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  // Weak, medium, then heavy hits: the status runs out on the fourth hit, which is still free; then stamina is paid.
  const int strengths[] = {0, 1, 2, 2, 2, 2, 0};
  const int stamina_after[] = {70, 70, 70, 70, 40, 10, 0};
  const int timer_after[] = {8, 6, 3, 0, 0, 0, 0};
  int budget = battle.hit_budget(0);
  for (int i = 0; i < 7; ++i) {
    const int cost = battle.chain_cost(strengths[i], battle.unit(0));
    CHECK(cost <= budget);
    budget -= cost;
    attack.chain = {strengths[i]};
    CHECK(battle.apply(attack));
    CHECK_EQ(battle.unit(0).stamina, stamina_after[i]);
    const auto timer = battle.unit(0).timers.find(32 + 17);
    CHECK_EQ(timer == battle.unit(0).timers.end() ? 0 : timer->second, timer_after[i]);
  }
  CHECK_EQ(budget, 0);
  CHECK(!(battle.unit(0).hi & st::kHiNoStaminaCost));
  battle.unit_mut(0).hi |= st::kHiNoStaminaCost;
  battle.unit_mut(0).stamina = 20;
  attack.chain = {2};
  std::string why;
  CHECK(!battle.validate(attack, &why) && why == "not enough stamina for the chain");
  attack.chain = {1};
  battle.unit_mut(0).lo |= st::kFatigued;
  CHECK(battle.validate(attack));
}

// A double tech's weapon hits are dealt one each by its participants (the party slots of the character ids in record +0x14..+0x16), not
// twice by the caster; a single tech (+0x14 = FF) hits +0xE times with the caster.
void test_weapon_tech_participants() {
  const auto tech_damage = [](bool double_tech, int caster_atk) {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    auto& record = fixture.tables.elements[4];
    record.raw[5] = 0x08, record.raw[6] = 1, record.raw[7] = 29, record.raw[8] = 20, record.raw[0xE] = 2;
    record.raw[0x14] = double_tech ? 1 : 0xFF, record.raw[0x15] = double_tech ? 2 : 0xFF, record.raw[0x16] = 0xFF, record.raw[0x17] = 0xFF;
    fixture.battle.unit_mut(0).atk = caster_atk;
    fixture.battle.unit_mut(1).atk = 60;
    fixture.battle.unit_mut(2).atk = 60;
    const int hp0 = fixture.battle.unit(3).hp;
    fixture.battle.do_element(0, 4, 0, 3);
    return hp0 - fixture.battle.unit(3).hp;
  };
  CHECK(tech_damage(false, 9) > 0);
  CHECK(tech_damage(false, 90) > tech_damage(false, 9));
  CHECK_EQ(tech_damage(true, 9), tech_damage(true, 90));
  CHECK(tech_damage(true, 9) > tech_damage(false, 9));
}

// Double / triple techs: a tech whose partner tech a party member holds (open, unused, not sealed, gauge at its row, 1.0 stamina) turns into
// the double tech; the cast uses up every caster's cell and 7.0 stamina, and caster entry k takes party slot k's gauge minus its own cell
// level (Serge's X-Strike with Glenn in slot 2 leaves Glenn with Kid's gauge - 3).
void test_combo_techs() {
  constexpr int kDash = 0xA0, kGash = 0xD9, kXStrike = 0x9F, kLumi = 0xA1, kPartnerA = 0xC9, kPartnerB = 0xDE, kTriple = 0x89;
  const auto setup = [](Fixture& fixture) {
    auto& elements = fixture.tables.elements;
    elements.resize(0x100);
    const auto tech = [&](int id, int colour, int partner, int result) {
      auto& record = elements[static_cast<size_t>(id)];
      record.raw[1] = 2, record.raw[4] = 0x80, record.raw[5] = static_cast<u8>(colour), record.raw[6] = 3, record.raw[8] = 20, record.raw[0xC] = 20;
      record.raw[0x14] = record.raw[0x15] = record.raw[0x16] = record.raw[0x17] = 0xFF;
      record.raw[0x20] = static_cast<u8>(partner), record.raw[0x26] = static_cast<u8>(result);
    };
    tech(kDash, 0x08, kGash, kXStrike);
    tech(kGash, 0x04, kDash, kXStrike);
    tech(kXStrike, 0x20, 0, 0);
    elements[kXStrike].raw[0x14] = 0, elements[kXStrike].raw[0x15] = 2;
    tech(kLumi, 0x08, 0, 0);
    elements[kLumi].raw[0x22] = kPartnerA, elements[kLumi].raw[0x24] = kPartnerB, elements[kLumi].raw[0x28] = kTriple;
    tech(kPartnerA, 0x04, 0, 0);
    tech(kPartnerB, 0x80, 0, 0);
    tech(kTriple, 0x08, 0, 0);
    elements[kTriple].raw[0x14] = 0, elements[kTriple].raw[0x15] = 1, elements[kTriple].raw[0x16] = 2;
    const auto place = [&](int unit, int cell, int id) {
      fixture.battle.unit_mut(unit).cells[static_cast<size_t>(cell)] = static_cast<u16>(id);
      fixture.battle.unit_mut(unit).usable[static_cast<size_t>(cell)] = true;
    };
    place(0, 27, kDash);
    place(2, 27, kGash);
    place(0, 47, kLumi);
    place(1, 45, kPartnerA);
    place(2, 46, kPartnerB);
    for (int unit = 0; unit < 3; ++unit) fixture.battle.unit_mut(unit).gauge_max = 7;
  };
  const auto with = [&](int gauge0, int gauge1, int gauge2, int stamina2) {
    auto fixture = std::make_unique<Fixture>();
    setup(*fixture);
    fixture->battle.unit_mut(0).gauge = gauge0, fixture->battle.unit_mut(1).gauge = gauge1, fixture->battle.unit_mut(2).gauge = gauge2;
    fixture->battle.unit_mut(2).stamina = stamina2;
    return fixture;
  };
  CHECK_EQ(with(4, 6, 2, 70)->battle.cell_element(0, 27), kXStrike);
  CHECK_EQ(with(4, 6, 2, 70)->battle.cell_element(2, 27), kXStrike);
  // Glenn's gauge below his cell's row, or under 1.0 stamina: the plain tech.
  CHECK_EQ(with(4, 6, 1, 70)->battle.cell_element(0, 27), kDash);
  CHECK_EQ(with(4, 6, 2, 9)->battle.cell_element(0, 27), kDash);
  {
    auto fixture = with(4, 6, 2, 70);
    fixture->battle.unit_mut(2).used[27] = true;
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
    fixture->battle.unit_mut(2).used[27] = false;
    fixture->battle.unit_mut(2).lo |= st::kKO;
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
  }
  {
    // The cast: Serge 4 -> 1, Glenn takes Kid's 3 - 3 = 0; both pay 7.0 (Glenn goes negative); Kid recovers and keeps his gauge.
    auto fixture = with(4, 3, 7, 40);
    fixture->battle.unit_mut(1).stamina = 30;
    const ComboTech combo = fixture->battle.combo_at(0, 27);
    CHECK_EQ(combo.element, kXStrike);
    CHECK(combo.units[0] == 0 && combo.cells[0] == 27 && combo.units[1] == 2 && combo.cells[1] == 27 && combo.units[2] == -1);
    Command command;
    command.kind = Command::Kind::Element, command.actor = 0, command.cell = 27, command.target = 3;
    CHECK(fixture->battle.apply(command));
    const Battle& battle = fixture->battle;
    CHECK_EQ(battle.unit(0).gauge, 1);
    CHECK_EQ(battle.unit(0).stamina, 0);
    CHECK(battle.unit(0).used[27]);
    CHECK_EQ(battle.unit(2).gauge, 0);
    CHECK_EQ(battle.unit(2).stamina, -30);
    CHECK(battle.unit(2).used[27]);
    CHECK_EQ(battle.unit(1).gauge, 3);
    CHECK(battle.unit(1).stamina > 30);
    bool cast = false;
    for (const ActionEvent& event : battle.events())
      if (event.kind == ActionEvent::Kind::Element && event.element == kXStrike) cast = event.actor == 0 && event.group[0] == 0 && event.group[1] == 2 && event.group[2] == 0xFF;
    CHECK(cast);
  }
  {
    // Triple: each of the other two slots holds one of the two partner techs.
    auto fixture = with(5, 5, 5, 70);
    const ComboTech combo = fixture->battle.combo_at(0, 47);
    CHECK_EQ(combo.element, kTriple);
    CHECK(combo.cells[0] == 47 && combo.cells[1] == 45 && combo.cells[2] == 46);
    fixture->battle.unit_mut(1).usable[45] = false;
    CHECK_EQ(fixture->battle.cell_element(0, 47), kLumi);
  }
}

// The double and triple techs of the game's own tables (ids, colours, partner and result words, participants). A partner's colour seal is
// the flag its cell carries: it is judged when a menu build or an action last touched the PC, by the element the cell showed then, not by
// the live status word.
void test_combo_real_tables() {
  constexpr int kDash = 0xA0, kGash = 0xD9, kXStrike = 0x9F, kLumi = 0xA1, kC9 = 0xC9, kDE = 0xDE, kLumiResult = 0x89;
  constexpr int kT93 = 0x93, kTA2 = 0xA2, kTA4 = 0xA4, kTripleB = 0x88;
  constexpr u32 kGreenSeal = 0x800, kRedSeal = 0x800000;
  const auto place = [](Fixture& fixture, int unit, int cell, int id) {
    fixture.battle.unit_mut(unit).cells[static_cast<size_t>(cell)] = static_cast<u16>(id);
    fixture.battle.unit_mut(unit).usable[static_cast<size_t>(cell)] = true;
  };
  const auto build = [&](auto&& arrange) {
    auto fixture = std::make_unique<Fixture>();
    auto& elements = fixture->tables.elements;
    elements.resize(0x100);
    const auto tech = [&](int id, int colour, int partner, int result) {
      auto& record = elements[static_cast<size_t>(id)];
      record.raw[1] = 2, record.raw[4] = 0x80, record.raw[5] = static_cast<u8>(colour), record.raw[6] = 3, record.raw[8] = 20, record.raw[0xC] = 20;
      record.raw[0x14] = record.raw[0x15] = record.raw[0x16] = record.raw[0x17] = 0xFF;
      record.raw[0x20] = static_cast<u8>(partner), record.raw[0x26] = static_cast<u8>(result);
    };
    const auto triple = [&](int id, int colour, int first, int second, int result) {
      tech(id, colour, 0, 0);
      elements[static_cast<size_t>(id)].raw[0x22] = static_cast<u8>(first);
      elements[static_cast<size_t>(id)].raw[0x24] = static_cast<u8>(second);
      elements[static_cast<size_t>(id)].raw[0x28] = static_cast<u8>(result);
    };
    const auto result = [&](int id, int colour) {
      tech(id, colour, 0, 0);
      elements[static_cast<size_t>(id)].raw[0x14] = 0, elements[static_cast<size_t>(id)].raw[0x15] = 1, elements[static_cast<size_t>(id)].raw[0x16] = 2;
    };
    tech(kDash, 0x08, kGash, kXStrike);
    tech(kGash, 0x04, kDash, kXStrike);
    result(kXStrike, 0x20);
    elements[kXStrike].raw[0x16] = 0xFF;
    triple(kLumi, 0x08, kC9, kDE, kLumiResult);
    triple(kC9, 0x04, kLumi, kDE, kLumiResult);
    triple(kDE, 0x80, kLumi, kC9, kLumiResult);
    result(kLumiResult, 0x08);
    triple(kT93, 0x80, kTA2, kTA4, kTripleB);
    triple(kTA2, 0x08, kT93, kTA4, kTripleB);
    triple(kTA4, 0x20, kT93, kTA2, kTripleB);
    result(kTripleB, 0x80);
    for (int unit = 0; unit < 3; ++unit) {
      fixture->battle.unit_mut(unit).gauge_max = 7;
      fixture->battle.unit_mut(unit).gauge = 7;
    }
    arrange(*fixture);
    return fixture;
  };
  // The Luminaire trio and the 0x88 trio work from every member's menu.
  for (int active = 0; active < 3; ++active) {
    auto fixture = build([&](Fixture& f) {
      place(f, 0, 47, kLumi), place(f, 1, 45, kC9), place(f, 2, 46, kDE);
      place(f, 0, 60, kTA2), place(f, 1, 61, kT93), place(f, 2, 62, kTA4);
    });
    const int cells[3] = {47, 45, 46}, cells_b[3] = {60, 61, 62};
    CHECK_EQ(fixture->battle.cell_element(active, cells[active]), kLumiResult);
    CHECK_EQ(fixture->battle.cell_element(active, cells_b[active]), kTripleB);
    const ComboTech combo = fixture->battle.combo_at(active, cells[active]);
    CHECK(combo.slot_cells[0] == 47 && combo.slot_cells[1] == 45 && combo.slot_cells[2] == 46);
  }
  // Both partner slots may hold the same partner tech: each only needs one of the two.
  {
    auto fixture = build([&](Fixture& f) { place(f, 0, 47, kLumi), place(f, 1, 45, kC9), place(f, 2, 46, kC9); });
    CHECK_EQ(fixture->battle.cell_element(0, 47), kLumiResult);
  }
  // A slot that holds both partner techs does not stand in for the empty one.
  {
    auto fixture = build([&](Fixture& f) { place(f, 0, 47, kLumi), place(f, 1, 45, kC9), place(f, 1, 55, kDE); });
    CHECK_EQ(fixture->battle.cell_element(0, 47), kLumi);
  }
  // A slot that holds both gives up the cell of the second partner tech.
  {
    auto fixture = build([&](Fixture& f) { place(f, 0, 47, kLumi), place(f, 1, 45, kC9), place(f, 1, 46, kDE), place(f, 2, 50, kDE); });
    const ComboTech combo = fixture->battle.combo_at(0, 47);
    CHECK_EQ(combo.element, kLumiResult);
    CHECK(combo.slot_cells[1] == 46 && combo.slot_cells[2] == 50);
  }
  // Double tech: when both partner slots qualify, the first one in slot order joins.
  {
    auto fixture = build([&](Fixture& f) { place(f, 0, 27, kDash), place(f, 1, 27, kGash), place(f, 2, 30, kGash); });
    const ComboTech combo = fixture->battle.combo_at(0, 27);
    CHECK(combo.element == kXStrike && combo.slot_cells[1] == 27 && combo.slot_cells[2] == -1);
  }
  // Only the first cell that holds the partner tech is looked at: when it is used, a later copy does not help.
  {
    auto fixture = build([&](Fixture& f) { place(f, 0, 27, kDash), place(f, 1, 20, kGash), place(f, 1, 27, kGash); });
    fixture->battle.unit_mut(1).used[20] = true;
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
  }
  // Seals: Serge holds Dash & Slash (white), Glenn holds Dash & Gash (green), the result X-Strike is red.
  const auto duo = [&](Fixture& f) { place(f, 0, 27, kDash), place(f, 1, 27, kGash); };
  {
    // Sealed in its own colour with nothing shown over the cell: the flag is set and the partner stays out.
    auto fixture = build(duo);
    fixture->battle.unit_mut(1).lo |= kGreenSeal;
    fixture->battle.refresh_seal_flags(1);
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
  }
  {
    // Sealed in the colour of the result only: judged by its own tech's colour, it still joins.
    auto fixture = build(duo);
    fixture->battle.unit_mut(1).lo |= kRedSeal;
    fixture->battle.refresh_seal_flags(1);
    CHECK_EQ(fixture->battle.cell_element(0, 27), kXStrike);
  }
  {
    // The menu built the double tech into the partner's cell first: a seal that lands now is judged by the result's colour, so a green seal
    // leaves the flag clear and the partner still joins at the next menu; a red one sets it.
    auto fixture = build(duo);
    fixture->battle.open_element_menu(0);
    fixture->battle.unit_mut(1).lo |= kGreenSeal;
    fixture->battle.refresh_seal_flags(1);
    fixture->battle.open_element_menu(0);
    CHECK_EQ(fixture->battle.cell_element(0, 27), kXStrike);
    fixture->battle.unit_mut(1).lo |= kRedSeal;
    fixture->battle.refresh_seal_flags(1);
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
  }
  {
    // A seal that runs out does not clear the flag; the partner's own menu build does.
    auto fixture = build(duo);
    fixture->battle.unit_mut(1).lo |= kGreenSeal;
    fixture->battle.refresh_seal_flags(1);
    fixture->battle.unit_mut(1).lo &= ~kGreenSeal;
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
    fixture->battle.open_element_menu(1);
    CHECK_EQ(fixture->battle.cell_element(0, 27), kXStrike);
  }
  {
    // The partner's own build judges its cell by the double tech: red-sealed, its flag is set and it stays out of the next build of the other.
    auto fixture = build(duo);
    fixture->battle.unit_mut(1).lo |= kRedSeal;
    fixture->battle.open_element_menu(1);
    CHECK(fixture->battle.unit(1).seal_flag[27]);
    CHECK_EQ(fixture->battle.cell_element(0, 27), kDash);
  }
}

// Run Away with pilfered loot pending drops the loot: the Run event carries system window row 5 and nothing stolen is left to keep.
void test_run_drops_stolen_loot() {
  for (const bool stole : {false, true}) {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    fixture.tables.elements[4].raw[7] = 30;
    fixture.tables.drops[0].slot = {0x0005, 0x8069, 0x0007, 0x8061};
    fixture.battle.unit_mut(3).drop_row = 0;
    fixture.battle.set_cheats({.always_loot = true});
    if (stole) fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.rewards().stolen.size(), stole ? 1u : 0u);
    Command run;
    run.kind = Command::Kind::Run;
    run.actor = 0;
    CHECK(fixture.battle.apply(run));
    CHECK(fixture.battle.outcome() == Outcome::Fled);
    CHECK(fixture.battle.rewards().stolen.empty());
    CHECK(fixture.battle.events().back().kind == ActionEvent::Kind::Run);
    CHECK_EQ(fixture.battle.events().back().sys_msg, stole ? 5 : -1);
  }
}

// Mode 7 with record +0xE != 0 changes the stats only of a target wearing the hero set (Medalsome); +0xC != 0 then heals by the heal
// formula with +0xC as its power, landed or not. A target with hi 0x2000000 takes the heal as damage.
void test_stat_mode_hero_gate_and_heal() {
  const auto cast = [](bool hero, u32 hi) {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    auto& record = fixture.tables.elements[4];
    record.raw[5] = 0x80, record.raw[6] = 1, record.raw[7] = 7, record.raw[8] = 120, record.raw[0xA] = 100, record.raw[0xC] = 25,
    record.raw[0xE] = 1, record.raw[0x1C] = 0xC0, record.raw[0x1D] = 0x0F;
    Unit& target = fixture.battle.unit_mut(0);
    target.hp = 5;
    target.hi |= hi;
    target.hero_set = hero;
    fixture.battle.do_element(1, 4, 0, 0);
    return std::make_pair(fixture.battle.unit(0), fixture.battle.events().back().code);
  };
  const auto [plain, plain_code] = cast(false, 0);
  CHECK_EQ(plain.mod[0], 100);
  CHECK(plain.hp > 5);
  CHECK_EQ(plain_code, 3);
  const auto [hero, hero_code] = cast(true, 0);
  CHECK_EQ(hero.mod[0], 120);
  CHECK_EQ(hero.mod[5], 120);
  CHECK_EQ(hero.hp, plain.hp);
  const auto [hurt, hurt_code] = cast(false, 0x2000000u);
  CHECK_EQ(hurt.hp, 0);
  CHECK_EQ(hurt_code, 0);
}

// A Dopplegang form's grid loses its lock bits and the gauge tops out at the form's deepest element row.
void test_dopple_form_gauge_cap() {
  Fixture fixture;
  auto& monster = fixture.tables.enemies[0];
  monster.raw[0x30] = 1, monster.raw[0x34] = 1;
  monster.raw[0x40] = 0x01, monster.raw[0x41] = 0x40;
  monster.raw[0x42] = 0x01, monster.raw[0x43] = 0x40;
  CHECK_EQ(fixture.battle.unit(0).gauge_max, 7);
  CHECK(fixture.battle.transform(0, 0));
  CHECK_EQ(fixture.battle.unit(0).gauge_max, 2);
  CHECK_EQ(fixture.battle.unit(0).cells[20], 1);
  CHECK_EQ(fixture.battle.unit(0).gauge, -1);
  CHECK(fixture.battle.transform(0, -1));
  CHECK_EQ(fixture.battle.unit(0).gauge_max, 7);
}

// A monster-party scene (scene byte 0x0F bit 0) puts the Grand Slam picks into the party slots: the record under the slot's character id
// (HP capped at 999), the grid cells of rows 1..7 opened (row 0 keeps its lock, so its element stays unusable), on the enemies as well; a
// slot without a pick stays empty; the fight pays nothing and leaves the party records alone. Without picks the party fights as itself.
void test_monster_party() {
  data::GameTables tables = make_tables();
  tables.scenes[0].raw[0x0F] = 1;
  auto& monster = tables.enemies[0];
  monster.raw[0x30] = 1, monster.raw[0x34] = 1;
  monster.raw[0x40] = 0x01, monster.raw[0x41] = 0x40;
  monster.raw[0x42] = 0x03, monster.raw[0x43] = 0x40;
  ScriptedRng rng{Lo::Fallback::Lo};
  ForcedWinBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  BattleOptions options;
  options.monster_party = {0, -1, 0};
  CHECK(battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0, options));
  CHECK(battle.monster_party());
  const Unit& first = battle.unit(0);
  CHECK(first.present && first.pc && first.form == 0 && first.src_id == 0);
  CHECK_EQ(first.max_hp, 999);
  CHECK_EQ(first.hp, 999);
  CHECK_EQ(first.atk, 4);
  CHECK_EQ(first.cells[0], 0x4001);
  CHECK(!first.usable[0]);
  CHECK_EQ(first.cells[20], 3);
  CHECK(first.usable[20]);
  CHECK_EQ(first.gauge_max, 2);
  CHECK(first.ap == 8 && first.countdown == 8 && first.stamina == 70 && first.gauge == -1);
  CHECK(!battle.unit(1).present);
  CHECK(battle.unit(2).present && battle.unit(2).form == 0 && battle.unit(2).src_id == 2);
  CHECK_EQ(battle.unit(3).cells[20], 3);
  battle.finish_external_action();
  CHECK(battle.outcome() == Outcome::Victory);
  CHECK_EQ(battle.rewards().gold, 0);
  CHECK_EQ(battle.party()[0].hp, 50);

  ScriptedRng plain_rng{Lo::Fallback::Lo};
  NullBrain plain_brain;
  Battle plain{tables, plain_rng, plain_brain};
  plain_rng.push(0);
  plain_rng.push(100);
  CHECK(plain.start({make_member(0, "A")}, 0));
  CHECK(!plain.monster_party());
  CHECK(plain.unit(0).form == -1 && plain.unit(0).name == "A");
  CHECK_EQ(plain.unit(3).cells[20], 0x4003);
}

// Pilfered loot reaches the stock on a victory only.
void test_lynx_duel_bodies() {
  data::GameTables tables = make_tables();
  tables.scenes.resize(0x1DE);
  tables.scenes[0x1DD] = tables.scenes[0];
  tables.scenes[0x1DD].raw[0] = 0, tables.scenes[0x1DD].raw[1] = 0xFF, tables.scenes[0x1DD].raw[2] = 0xFF;
  tables.scenes[0x1DD].raw[3] = 3;
  auto& records = tables.char_sets["char"].rec;
  const auto set_gender = [&](int char_id, u16 word) {
    records[static_cast<size_t>(char_id)].raw[0x14] = static_cast<u8>(word);
    records[static_cast<size_t>(char_id)].raw[0x15] = static_cast<u8>(word >> 8);
  };
  set_gender(0, 0x4012), set_gender(1, 0x8034), set_gender(2, 0x4056), set_gender(0x2B, 0x8078);
  PartyMember serge = make_member(0, "A"), other = make_member(2, "C");
  serge.eq.weapon_id = 0x20;
  other.eq.weapon_id = 0x30;
  other.innate = 3;
  BattleOptions options;
  options.duel_third = 2;
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    ForcedWinBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start({serge, other}, 0x1DD, options));
    // The leader fights alone.
    CHECK(battle.unit(0).present);
    CHECK(!battle.unit(1).present);
    CHECK_EQ(battle.unit(3).body, 0);
    CHECK_EQ(battle.unit(3).kind, 0);
    CHECK_EQ(static_cast<int>(battle.unit(3).eq.weapon_id), 0x20);
    CHECK_EQ(static_cast<int>(battle.unit(3).gender), 0x4012);
    CHECK_EQ(battle.unit(3).max_hp, 9999 * 0x69 / 100);
    // Kid is not in the party: the default weapon, and her record's gender word scales the HP.
    CHECK_EQ(battle.unit(4).body, 1);
    CHECK_EQ(battle.unit(4).kind, 1);
    CHECK_EQ(static_cast<int>(battle.unit(4).eq.weapon_id), 0x29);
    CHECK_EQ(static_cast<int>(battle.unit(4).gender), 0x8034);
    CHECK_EQ(battle.unit(4).max_hp, 9999 * 0x5F / 100);
    CHECK_EQ(battle.unit(5).body, 2);
    CHECK_EQ(battle.unit(5).kind, 2);
    CHECK_EQ(static_cast<int>(battle.unit(5).eq.weapon_id), 0x30);
    CHECK_EQ(static_cast<int>(battle.unit(5).gender), 0x4056);
    CHECK_EQ(static_cast<int>(battle.unit(5).innate), 3);
  }
  {
    // The dummies read the character they became, party member 0 Lynx; a body past the named ids reads nothing.
    std::vector<std::string> names(0x32);
    names[0] = "Lynx-body", names[1] = "Kid-name", names[2] = "Third-name", names[0x31] = "Lynx-name";
    BattleOptions named = options;
    named.duel_names = names;
    ScriptedRng rng{Lo::Fallback::Lo};
    ForcedWinBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start({serge, other}, 0x1DD, named));
    CHECK(battle.unit(0).name == "Lynx-name");
    CHECK(battle.unit(3).name == "Lynx-body");
    CHECK(battle.unit(4).name == "Kid-name");
    CHECK(battle.unit(5).name == "Third-name");
  }
  {
    // The relief member stood in for Serge in the fight before: Serge's body gets the default weapon, hers the member's.
    PartyMember relief = make_member(1, "K");
    relief.eq.weapon_id = 0x33;
    options.bench = {relief};
    options.duel_relief = 1;
    ScriptedRng rng{Lo::Fallback::Lo};
    ForcedWinBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start({serge, other}, 0x1DD, options));
    CHECK_EQ(static_cast<int>(battle.unit(0).src_id), 0);
    CHECK_EQ(static_cast<int>(battle.unit(3).eq.weapon_id), 0x22);
    CHECK_EQ(static_cast<int>(battle.unit(4).eq.weapon_id), 0x33);
    CHECK_EQ(static_cast<int>(battle.unit(5).eq.weapon_id), 0x30);
    options.bench.clear();
    options.duel_relief = -1;
  }
  {
    // A third that is not in the party leaves the enemy as it is.
    options.duel_third = 7;
    ScriptedRng rng{Lo::Fallback::Lo};
    ForcedWinBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start({serge, other}, 0x1DD, options));
    CHECK_EQ(battle.unit(5).body, -1);
    CHECK_EQ(battle.unit(5).kind, static_cast<int>(tables.enemies[0].model()));
    CHECK_EQ(static_cast<int>(battle.unit(5).innate), static_cast<int>(tables.enemies[0].innate()));
    CHECK_EQ(static_cast<int>(battle.unit(5).gender), static_cast<int>(tables.enemies[0].rd16(0x14)));
  }
  {
    // Pip keeps his form: the search goes by his character id, the kind comes from his record.
    PartyMember pip = make_member(0x2B, "P");
    pip.kind = 0x2D;
    pip.innate = 4;
    pip.eq.weapon_id = 0x31;
    options.duel_third = 0x2B;
    ScriptedRng rng{Lo::Fallback::Lo};
    ForcedWinBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start({serge, pip}, 0x1DD, options));
    CHECK_EQ(battle.unit(5).body, 0x2D);
    CHECK_EQ(battle.unit(5).kind, 0x2D);
    CHECK_EQ(static_cast<int>(battle.unit(5).eq.weapon_id), 0x31);
    CHECK_EQ(static_cast<int>(battle.unit(5).gender), 0x8078);
    CHECK_EQ(static_cast<int>(battle.unit(5).innate), 4);
  }
  // Any other scene keeps its enemies.
  ScriptedRng plain_rng{Lo::Fallback::Lo};
  ForcedWinBrain plain_brain;
  Battle plain{tables, plain_rng, plain_brain};
  CHECK(plain.start({serge, other}, 0));
  CHECK_EQ(plain.unit(3).body, -1);
}

void test_enemy_names_follow_the_language() {
  data::GameTables tables = make_tables();
  tables.enemy_names = {"Beach Bum"};
  PartyMember serge = make_member(0, "Serge");
  BattleOptions options;
  options.enemy_names = {"Strandpenner"};
  ScriptedRng rng{Lo::Fallback::Lo};
  ForcedWinBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(battle.start({serge}, 0, options));
  CHECK(battle.unit(3).name == "Strandpenner");
  // No row for the language: the table's name.
  ScriptedRng plain_rng{Lo::Fallback::Lo};
  ForcedWinBrain plain_brain;
  Battle plain{tables, plain_rng, plain_brain};
  CHECK(plain.start({serge}, 0));
  CHECK(plain.unit(3).name == "Beach Bum");
}

void test_unnamed_kid_label() {
  data::GameTables tables = make_tables();
  tables.scenes.resize(data::kUnnamedKidScene + 1);
  tables.scenes[data::kUnnamedKidScene] = tables.scenes[0];
  PartyMember serge = make_member(0, "Serge"), kid = make_member(data::kKid, "Kid");
  BattleOptions options;
  options.unnamed_kid_label = data::unnamed_kid_label(import::Lang::De);
  ScriptedRng rng{Lo::Fallback::Lo};
  ForcedWinBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(battle.start({serge, kid}, data::kUnnamedKidScene, options));
  CHECK(battle.unit(0).name == "Serge");
  CHECK(battle.unit(1).name == "Mädchen");
  CHECK(battle.party()[1].name == "Mädchen");
  // Every other scene keeps her name, and so does a run with no label.
  ScriptedRng other_rng{Lo::Fallback::Lo};
  ForcedWinBrain other_brain;
  Battle other{tables, other_rng, other_brain};
  CHECK(other.start({serge, kid}, 0, options));
  CHECK(other.unit(1).name == "Kid");
  ScriptedRng plain_rng{Lo::Fallback::Lo};
  ForcedWinBrain plain_brain;
  Battle plain{tables, plain_rng, plain_brain};
  CHECK(plain.start({serge, kid}, data::kUnnamedKidScene));
  CHECK(plain.unit(1).name == "Kid");
  CHECK(std::string(data::unnamed_kid_label(import::Lang::En)) == "Girl");
  CHECK(std::string(data::unnamed_kid_label(import::Lang::Jp)) == "旅の娘");
}

// Party swap (BattleOptions::party_swap): a PC's Swap command trades its slot with a reserve member; the newcomer arrives at 0 stamina,
// the leaver's battle state waits on the bench and comes back with it. The saved party stays as it was unless the host keeps the lineup
// (game::keep_swapped_lineup: locked characters return to their slots, the Relief Charm's stand-in is Serge).
void test_party_swap() {
  data::GameTables tables = make_tables();
  auto make_battle = [&](Battle& battle, ScriptedRng& rng, BattleOptions opt) {
    rng.push(0);
    rng.push(100);
    auto spare = make_member(9, "D", 40), knocked_out = make_member(10, "E", 40), outside = make_member(11, "F", 40);
    knocked_out.hp = 0;
    opt.bench = {spare, knocked_out, outside};
    return battle.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0, opt);
  };
  BattleOptions opt;
  opt.party_swap = true;
  opt.swap_reserve = {9, 10};
  // Off: no candidates, the command is refused.
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    NullBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(make_battle(battle, rng, {}));
    CHECK(!battle.swap_enabled() && battle.swap_candidates(0).empty());
    Command swap;
    swap.kind = Command::Kind::Swap;
    swap.swap_in = 9;
    CHECK(!battle.apply(swap));
  }
  ScriptedRng rng{Lo::Fallback::Lo};
  NullBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(make_battle(battle, rng, opt));
  // The reserve minus the knocked-out member.
  CHECK(battle.swap_candidates(0) == std::vector<int>{9});
  CHECK(battle.swap_candidates(1) == std::vector<int>{9});
  Command swap;
  swap.kind = Command::Kind::Swap;
  swap.actor = 0;
  for (const int refused : {10, 11, 1}) {
    swap.swap_in = refused;
    CHECK(!battle.apply(swap));
  }
  battle.unit_mut(0).hp = 30;
  battle.unit_mut(0).lo |= st::kPoison | st::kAfraid;
  swap.swap_in = 9;
  CHECK(battle.apply(swap));
  CHECK(battle.unit(0).src_id == 9 && battle.unit(0).name == "D" && battle.unit(0).hp == 40);
  CHECK_EQ(battle.unit(0).stamina, 0);
  CHECK(battle.party()[0].char_id == 9 && battle.bench()[0].char_id == 0);
  CHECK_EQ(battle.bench()[0].hp, 30);
  CHECK_EQ(battle.bench()[0].lo_persist, st::kPoison);
  CHECK(battle.swapped_out() == std::vector<int>{0});
  CHECK_EQ(battle.swaps(), 1);
  CHECK(!battle.events().empty() && battle.events().back().kind == ActionEvent::Kind::Swap);
  CHECK(battle.events().back().actor == 0 && battle.events().back().swap_out == 0 && battle.events().back().swap_in == 9);
  // The benched character can come back although it is not in the reserve (it fought in this battle).
  battle.unit_mut(0).stamina = 70;
  CHECK(battle.swap_candidates(0) == std::vector<int>{0});
  opt.swap_reserve = {0, 9};
  {
    ScriptedRng back_rng{Lo::Fallback::Lo};
    NullBrain back_brain;
    Battle back{tables, back_rng, back_brain};
    CHECK(make_battle(back, back_rng, opt));
    back.unit_mut(0).lo |= st::kAfraid;
    swap.swap_in = 9;
    CHECK(back.apply(swap));
    back.unit_mut(0).stamina = 70;
    CHECK(back.swap_candidates(0) == std::vector<int>{0});
    swap.swap_in = 0;
    CHECK(back.apply(swap));
    CHECK(back.unit(0).src_id == 0 && (back.unit(0).lo & st::kAfraid) && back.unit(0).stamina == 0);
    CHECK(back.swapped_out() == std::vector<int>{9});
  }
  // After the battle (fled here): the leaver's record is written, the saved party is not.
  Command run;
  run.kind = Command::Kind::Run;
  run.actor = 1;
  CHECK(battle.apply(run));
  ui::GameState state;
  state.slot.set_party({0, 1, 2});
  for (const int char_id : {0, 1, 2, 9}) state.slot.chars[static_cast<size_t>(char_id)].max_hp = 50;
  game::apply_battle_result(state, tables, battle);
  CHECK(state.slot.party() == (std::array<u8, 3>{0, 1, 2}));
  CHECK_EQ(state.slot.chars[0].cur_hp, 30);
  CHECK_EQ(state.slot.chars[0].status, st::kPoison);
  // Keep lineup: the ended lineup, unless a locked character is on the bench (it takes its slot back).
  {
    ui::GameState kept = state;
    CHECK(game::keep_swapped_lineup(kept, battle, -1));
    CHECK(kept.slot.party() == (std::array<u8, 3>{9, 1, 2}));
    ui::GameState locked = state;
    locked.slot.unk_e1b8 = {1u, 0u, 0u, 0u};
    CHECK(!game::keep_swapped_lineup(locked, battle, -1));
    CHECK(locked.slot.party() == (std::array<u8, 3>{0, 1, 2}));
    // With reserve slots, Serge takes the slot Guile (9) came from.
    ui::GameState reserved = state;
    for (const int char_id : {0, 1, 2, 5, 9}) reserved.slot.set_recruited(char_id, true);
    // Never set: the first available characters outside the party.
    CHECK(reserved.reserve(3) == (std::vector<int>{5, 9, -1}));
    reserved.slot.set_reserve({5, 9, -1});
    CHECK(game::keep_swapped_lineup(reserved, battle, -1, 3));
    CHECK(reserved.slot.party() == (std::array<u8, 3>{9, 1, 2}) && reserved.slot.reserve() == (std::vector<int>{5, 0, -1}));
  }
  // The Relief Charm: character 2 stood in for Serge (saved party 0 1 x) and stayed on the field, so the kept lineup holds Serge.
  {
    ui::GameState relief = state;
    relief.slot.set_party({1, 0, 0xFF});
    CHECK(game::keep_swapped_lineup(relief, battle, 2));
    CHECK(relief.slot.party() == (std::array<u8, 3>{9, 1, 0}));
  }
  // A stand-in (character 3) that swapped out: Serge takes back his saved slot.
  {
    ui::GameState relief = state;
    relief.slot.set_party({1, 0, 0xFF});
    CHECK(game::keep_swapped_lineup(relief, battle, 3));
    CHECK(relief.slot.party() == (std::array<u8, 3>{9, 0, 2}));
  }
  // Nothing swapped: nothing to keep. A monster-party scene has no swap.
  {
    ScriptedRng idle_rng{Lo::Fallback::Lo};
    NullBrain idle_brain;
    Battle idle{tables, idle_rng, idle_brain};
    CHECK(make_battle(idle, idle_rng, opt));
    ui::GameState untouched = state;
    CHECK(!game::keep_swapped_lineup(untouched, idle, -1));
  }
  {
    data::GameTables monster_tables = make_tables();
    monster_tables.scenes[0].raw[0x0F] = 1;
    ScriptedRng monster_rng{Lo::Fallback::Lo};
    NullBrain monster_brain;
    Battle monster{monster_tables, monster_rng, monster_brain};
    BattleOptions monster_opt = opt;
    monster_opt.monster_party = {0, 0, 0};
    monster_rng.push(0);
    monster_rng.push(100);
    CHECK(monster.start({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}, 0, monster_opt));
    CHECK(!monster.swap_enabled() && monster.swap_candidates(0).empty());
  }
}

void test_target_cursor_order() {
  const auto order = [](u8 mask, int caster, int last_enemy) {
    data::ElemRec record;
    record.raw[4] = mask;
    return battle::target_cursor_order(battle::target_spec(record), caster, last_enemy);
  };
  // A consumable (0x40) and a party element open on unit 0, not on the caster or an enemy.
  CHECK(order(0x40, 2, 5)[0] == 0 && order(0x40, 2, 5)[2] == 2 && order(0x40, 2, 5)[3] == -1);
  CHECK(order(0x41, 1, 5)[0] == 0);
  // Both sides: the enemies first, unless bit 0x02 asks for the party (the Cure line).
  CHECK(order(0xC0, 1, 5)[0] == 3 && order(0xC0, 1, 5)[6] == 0 && order(0xC0, 1, 5)[8] == 2);
  CHECK(order(0xC2, 1, 5)[0] == 0 && order(0xC2, 1, 5)[8] == 8);
  // Self only pins the caster.
  CHECK(order(0x44, 2, 5)[0] == 2 && order(0x44, 2, 5)[1] == -1);
  // An enemy ring resumes on the last enemy and wraps within the enemies; a party id is no memory.
  CHECK(order(0x80, 0, 5)[0] == 5 && order(0x80, 0, 5)[3] == 8 && order(0x80, 0, 5)[4] == 3 && order(0x80, 0, 5)[5] == 4);
  CHECK(order(0x80, 0, 1)[0] == 3);
}

void test_stolen_loot_needs_victory() {
  for (const bool win : {false, true}) {
    Fixture fixture;
    fixture.tables.elements.resize(5);
    fixture.tables.elements[4].raw[5] = 0x80, fixture.tables.elements[4].raw[6] = 1, fixture.tables.elements[4].raw[7] = 30;
    fixture.tables.drops[0].slot = {0x0005, 0x8069, 0x0007, 0};
    fixture.rng.push(0);
    fixture.rng.push(50);
    fixture.battle.do_element(0, 4, 0, 3);
    CHECK_EQ(fixture.battle.rewards().stolen.size(), 1u);
    Command command;
    command.actor = 0;
    if (win) {
      fixture.battle.unit_mut(3).hp = 1;
      command.kind = Command::Kind::Attack;
      command.chain = {2};
    } else {
      command.kind = Command::Kind::Run;
    }
    CHECK(fixture.battle.apply(command));
    CHECK(fixture.battle.outcome() == (win ? Outcome::Victory : Outcome::Fled));
    ui::GameState state;
    game::apply_battle_result(state, fixture.tables, fixture.battle);
    CHECK_EQ(state.item_count(0x0007), win ? 1 : 0);
  }
}

// After a fight every consumable stack cell below 5 copies takes the missing copies from the element stock; a stack that ran out with no
// spares keeps its element at 0 copies.
void test_stack_restock() {
  Fixture fixture;
  fixture.tables.elements.resize(110);
  fixture.tables.elements[105].raw[0] = 0x10;
  fixture.tables.elements[1].raw[0] = 0x01;
  ui::GameState state;
  save::SaveChar& serge = state.slot.chars[0];
  serge.grid[0] = 105;
  serge.equipped_qty[0] = 3;
  serge.grid[1] = 105;
  serge.equipped_qty[1] = 0;
  serge.grid[2] = 105;
  serge.equipped_qty[2] = 5;
  serge.grid[3] = 1;
  serge.equipped_qty[3] = 2;
  for (int i = 0; i < 4; ++i) CHECK(state.add_element(105));
  game::apply_battle_result(state, fixture.tables, fixture.battle);
  CHECK_EQ(serge.equipped_qty[0], 5);
  CHECK_EQ(serge.equipped_qty[1], 2);
  CHECK_EQ(serge.equipped_qty[2], 5);
  CHECK_EQ(serge.equipped_qty[3], 2);
  CHECK_EQ(state.element_count(105), 0);
  game::apply_battle_result(state, fixture.tables, fixture.battle);
  CHECK(serge.grid[1] == 105 && serge.equipped_qty[1] == 2);
  serge.equipped_qty[1] = 0;
  game::apply_battle_result(state, fixture.tables, fixture.battle);
  CHECK(serge.grid[1] == 105 && serge.equipped_qty[1] == 0);
}

// A PC summon (record byte 0 bit 0x40) is usable only while every non-empty ring slot holds its colour and a star is left; the cast
// spends one star, and the stars left go back to the save whatever the outcome. An all-empty ring keeps the last summon colour.
void test_summon_gate() {
  data::GameTables tables = make_tables();
  tables.elements.resize(5);
  data::ElemRec& summon = tables.elements[4];
  summon.raw[0] = 0x41, summon.raw[1] = 1, summon.raw[5] = 0x80, summon.raw[6] = 1, summon.raw[8] = 12, summon.raw[0xC] = 20;
  ScriptedRng rng{Lo::Fallback::Lo};
  NullBrain brain;
  Battle battle{tables, rng, brain};
  rng.push(0);
  rng.push(100);
  PartyMember caller = make_member(0, "A");
  caller.cells[0] = 0x4004;
  BattleOptions options;
  options.growth.star = 12;
  options.growth.counter = 1;
  CHECK(battle.start({caller, make_member(1, "B"), make_member(2, "C")}, 0, options));
  battle.unit_mut(0).gauge = 2;
  battle.unit_mut(0).stamina = 70;
  battle.set_ring({5, 3, 5});
  CHECK_EQ(battle.ring_summon_colour(), -1);
  CHECK(battle.summon_blocked(0, 0) && battle.element_greyed(0, 0) && !battle.element_usable(0, 0));
  battle.set_ring({3, 3, 3});
  CHECK(battle.summon_blocked(0, 0) && !battle.element_usable(0, 0));
  battle.set_ring({-1, 5, 5});
  CHECK_EQ(battle.ring_summon_colour(), 5);
  CHECK(!battle.summon_blocked(0, 0) && !battle.element_greyed(0, 0) && battle.element_usable(0, 0));
  battle.set_ring({-1, -1, -1});
  CHECK_EQ(battle.ring_summon_colour(), 5);
  CHECK(battle.element_usable(0, 0));
  // An ordinary element of the same colour is not gated.
  CHECK(!battle.summon_blocked(0, 10));
  Command cast;
  cast.kind = Command::Kind::Element;
  cast.actor = 0;
  cast.cell = 0;
  std::string why;
  CHECK(battle.apply(cast, &why));
  CHECK_EQ(battle.growth().counter, 0);
  battle.unit_mut(0).used[0] = false;
  battle.unit_mut(0).stamina = 70;
  battle.unit_mut(0).gauge = 2;
  battle.set_ring({5, 5, 5});
  CHECK(battle.summon_blocked(0, 0) && !battle.element_usable(0, 0));
  Command run;
  run.kind = Command::Kind::Run;
  run.actor = 0;
  CHECK(battle.apply(run));
  ui::GameState state;
  state.slot.head[save::head::kStarsTotal] = 12;
  state.slot.head[save::head::kStarsCurrent] = 1;
  game::apply_battle_result(state, tables, battle);
  CHECK_EQ(state.slot.stars_current(), 0);
  CHECK_EQ(state.slot.head[save::head::kStarsTotal], 12);
}

// Every record has the 48-cell shape 03FC x3, 03F8, 01F0, 01E0 x3 (row 0 = columns 2..9, row 4 = columns 4..8) and growth column 0.
void give_every_record_a_grid(data::GameTables& tables) {
  auto& char_set = tables.char_sets["char"];
  const u16 masks[8] = {0x3FC, 0x3FC, 0x3FC, 0x3F8, 0x1F0, 0x1E0, 0x1E0, 0x1E0};
  for (auto& rec : char_set.rec)
    for (int row = 0; row < 8; ++row) {
      rec.raw[static_cast<size_t>(0x30 + 2 * row)] = static_cast<u8>(masks[row]);
      rec.raw[static_cast<size_t>(0x31 + 2 * row)] = static_cast<u8>(masks[row] >> 8);
    }
}

// A boss win at star 2 opens the el_mask cells of stars 0..3 in all 44 records' unlock masks: in an empty cell, in a character who
// is neither in the party nor recruited, never in a cell the grid does not have, never a later star's cell.
void test_star_up_grid_unlocks() {
  data::GameTables tables = make_tables();
  give_every_record_a_grid(tables);
  tables.scenes[0].raw[0x1A] = 0x80;
  tables.growth[1][0] = 0x07;
  tables.growth[2][0] = 0x11;
  tables.growth[3][0] = 0x2A;
  tables.growth[4][0] = 0x08;
  ui::MenuRules rules;
  rules.tables = &tables;
  ui::GameState state;
  state.rules = &rules;
  state.slot.head[save::head::kStarsTotal] = state.slot.head[save::head::kStarsCurrent] = 2;
  for (auto& rec : state.slot.chars) rec.unlocked = {0x40, 0x40, 0x40, 0, 0, 0, 0, 0};
  ScriptedRng rng{Lo::Fallback::Lo};
  rng.push(0);
  rng.push(100);
  NullBrain brain;
  Battle battle{tables, rng, brain};
  BattleOptions options;
  options.growth.star = 2;
  CHECK(battle.start({make_member(0, "A")}, 0, options));
  battle.unit_mut(3).hp = 1;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {2};
  CHECK(battle.apply(attack));
  CHECK(battle.outcome() == Outcome::Victory);
  CHECK(game::apply_battle_result(state, tables, battle).star_up);
  CHECK_EQ(state.slot.head[save::head::kStarsTotal], 3);
  for (const int char_id : {0, 5, 43}) {
    const auto& unlocked = state.slot.chars[static_cast<size_t>(char_id)].unlocked;
    CHECK_EQ(unlocked[0], 0xC0);
    CHECK_EQ(unlocked[1], 0xC0);
    CHECK_EQ(unlocked[2], 0x40);
    CHECK_EQ(unlocked[4], 0);
    CHECK(state.cell_available(char_id, 0, 7) && state.cell_element(char_id, 0, 7) == 0);
  }
}

// Scene 0x1D8 saves the party with no persistent status unless it fled; any other scene keeps the poison.
void test_scene_1d8_clears_status() {
  for (const int scene : {0, 0x1D8})
    for (const bool flee : {false, true}) {
      data::GameTables tables = make_tables();
      tables.scenes.resize(0x1D9, tables.scenes[0]);
      ScriptedRng rng{Lo::Fallback::Lo};
      rng.push(0);
      rng.push(100);
      NullBrain brain;
      Battle battle{tables, rng, brain};
      CHECK(battle.start({make_member(0, "A"), make_member(1, "B")}, scene));
      battle.unit_mut(1).lo |= st::kPoison;
      Command command;
      command.actor = 0;
      if (flee) {
        command.kind = Command::Kind::Run;
      } else {
        battle.unit_mut(3).hp = 1;
        command.kind = Command::Kind::Attack;
        command.chain = {2};
      }
      CHECK(battle.apply(command));
      CHECK(battle.outcome() == (flee ? Outcome::Fled : Outcome::Victory));
      ui::GameState state;
      game::apply_battle_result(state, tables, battle);
      CHECK_EQ(static_cast<u32>(state.slot.chars[1].status), scene == 0x1D8 && !flee ? 0u : st::kPoison);
    }
}

// Pip in a form grows toward the form's Lv 99 record (RDE): a boss win with Pip as Divine Beast (0x30), his stats at record 43's
// Lv 99 values, gives him his whole pending (30 HP, 3 / 5 / 5 / 5 / 9 / 6) up to the form's targets and rolls the next HP pending
// against the form's 800 HP. RDE: 750 -> 780 HP, ATK / DEF / HIT / EVD / MAG / MDEF 83 / 75 / 95 / 30 / 49 / 41, next HP pending 2.
void test_pip_form_growth() {
  data::GameTables tables = make_tables();
  give_every_record_a_grid(tables);
  tables.scenes[0].raw[0x1A] = 0x80;
  auto& char_set = tables.char_sets["char"];
  char_set.lv99[43] = {750, 80, 70, 90, 25, 40, 35};
  char_set.lv99[0x30] = {800, 84, 80, 99, 35, 58, 46};
  ui::MenuRules rules;
  rules.tables = &tables;
  ui::GameState state;
  state.rules = &rules;
  state.slot.head[save::head::kStarsTotal] = 50;
  save::SaveChar& pip = state.slot.chars[43];
  pip.model_id = 0x30;
  pip.max_hp = pip.cur_hp = pip.hp_extra = 750;
  pip.atk = 80, pip.def = 70, pip.hit = 90, pip.evd = 25, pip.mag = 40, pip.mdef = 35;
  pip.unk_be = 30;
  pip.bonus = {80, 3, 90, 5, 70, 5, 25, 5, 40, 9, 35, 6};
  const PartyMember member = game::party_member_from_state(tables, state, 43);
  CHECK(member.lv99 == (std::array<int, 7>{800, 84, 80, 99, 35, 58, 46}));
  pip.model_id = 0x2B;
  CHECK(game::party_member_from_state(tables, state, 43).lv99 == (std::array<int, 7>{750, 80, 70, 90, 25, 40, 35}));
  ScriptedRng rng{Lo::Fallback::Lo};
  rng.push(0);
  rng.push(100);
  NullBrain brain;
  Battle battle{tables, rng, brain};
  BattleOptions options;
  options.growth.star = 50;
  CHECK(battle.start({member}, 0, options));
  battle.unit_mut(3).hp = 1;
  Command attack;
  attack.kind = Command::Kind::Attack;
  attack.actor = 0;
  attack.chain = {2};
  CHECK(battle.apply(attack));
  CHECK(battle.outcome() == Outcome::Victory);
  CHECK(battle.rewards().star_up);
  const PartyMember& grown = battle.party()[0];
  CHECK(grown.stat == (std::array<int, 7>{780, 83, 75, 95, 30, 49, 41}));
  CHECK(grown.snap == grown.stat);
  CHECK(grown.pending[0] >= 2 && grown.pending[0] <= 20);
  // A form change in the fight moves the targets to the new form at once.
  char_set.lv99[0x2E] = {950, 92, 90, 95, 25, 48, 38};
  battle.evolve(0, 0x2E);
  CHECK(battle.party()[0].lv99 == (std::array<int, 7>{950, 92, 90, 95, 25, 48, 38}));
}

// A fight that changes Pip's form at 99 stars ends with the new form's Lv 99 stats and its full grid (RDE: Pip 0x2B -> 0x2C at 500 HP,
// 7 / 7 / 85 / 20 / 8 / 7, masks 0x40 x3: after the results 780 HP, 84 / 80 / 90 / 25 / 44 / 36, masks f8 f8 f8 f0 e0 c0 0 0, pending kept).
void test_pip_form_settles_at_99() {
  data::GameTables tables = make_tables();
  give_every_record_a_grid(tables);
  auto& char_set = tables.char_sets["char"];
  char_set.lv99[0x2C] = {780, 84, 80, 90, 25, 44, 36};
  tables.growth[5][0] = 0x13;
  ui::GameState state;
  auto reset = [&](int stars) {
    state.slot.head[save::head::kStarsTotal] = static_cast<u8>(stars);
    save::SaveChar& pip = state.slot.chars[43];
    pip.model_id = 0x2C;
    pip.max_hp = pip.hp_extra = 500, pip.cur_hp = 450;
    pip.atk = 7, pip.def = 7, pip.hit = 85, pip.evd = 20, pip.mag = 8, pip.mdef = 7;
    pip.unk_be = 8;
    pip.bonus = {7, 0, 85, 0, 7, 0, 20, 0, 8, 1, 7, 0};
    pip.unlocked = {};
  };
  reset(98);
  game::settle_pip_form(state, tables, 0x2B);
  CHECK_EQ(state.slot.chars[43].max_hp, 500);
  reset(99);
  game::settle_pip_form(state, tables, 0x2C);
  CHECK_EQ(state.slot.chars[43].max_hp, 500);
  game::settle_pip_form(state, tables, 0x2B);
  const save::SaveChar& pip = state.slot.chars[43];
  CHECK(pip.max_hp == 780 && pip.cur_hp == 730 && pip.hp_extra == 780 && pip.unk_be == 8);
  CHECK(pip.atk == 84 && pip.def == 80 && pip.hit == 90 && pip.evd == 25 && pip.mag == 44 && pip.mdef == 36);
  CHECK(pip.bonus == (std::array<u8, 12>{84, 0, 90, 0, 80, 0, 25, 0, 44, 1, 36, 0}));
  CHECK(pip.unlocked == (std::array<u16, 8>{0x40, 0x240, 0x40, 0, 0, 0, 0, 0}));
}

// The battle opens a saved cell exactly when its unlock mask bit is set, as the menu does: a filled cell of an earlier star whose bit is
// clear stays shut, a filled cell in rows 0..1 is not opened by default.
void test_battle_grid_follows_the_masks() {
  data::GameTables tables = make_tables();
  give_every_record_a_grid(tables);
  tables.growth[1][0] = 0x07;
  ui::MenuRules rules;
  rules.tables = &tables;
  ui::GameState state;
  state.rules = &rules;
  state.slot.head[save::head::kStarsTotal] = 3;
  save::SaveChar& serge = state.slot.chars[0];
  serge.max_hp = serge.cur_hp = 100;
  serge.unlocked = {0x40, 0x40, 0x40, 0, 0, 0, 0, 0};
  serge.grid[3] = 1;
  serge.grid[4] = 1;
  serge.grid[5] = 1;
  const PartyMember member = game::party_member_from_state(tables, state, 0);
  CHECK(member.unlocked[6]);
  CHECK(!member.unlocked[5]);
  CHECK(!member.unlocked[7]);
  serge.unlocked[0] |= 0x80;
  CHECK(game::party_member_from_state(tables, state, 0).unlocked[7]);
}

// A challenge's party card: the Status screen's numbers (record plus gear, capped at 999) per member, at most 3 members, and the
// elements in open cells (a tech or a shut cell does not count).
void test_party_card() {
  data::GameTables tables = make_tables();
  give_every_record_a_grid(tables);
  tables.gear.resize(0x60);
  tables.gear[0x50].raw[4] = 20;
  tables.gear[0x50].raw[5] = 3;
  tables.gear[0x51].raw[4] = 6;
  tables.gear[0x51].raw[5] = 2;
  ui::MenuRules rules;
  rules.tables = &tables;
  ui::GameState state;
  state.rules = &rules;
  state.slot.head[save::head::kStarsTotal] = 7;
  save::SaveChar& serge = state.slot.chars[0];
  serge.max_hp = 320, serge.atk = 15, serge.def = 12, serge.mag = 9, serge.mdef = 8;
  serge.weapon = 0x50;
  serge.armour = 0x51;
  serge.unlocked = {0xFC, 0, 0, 0, 0, 0, 0, 0};
  serge.grid[0] = 1;
  serge.grid[1] = 2;
  serge.grid[2] = 0x80;
  serge.grid[10] = 3;
  save::SaveChar& kid = state.slot.chars[1];
  kid.max_hp = 1200, kid.atk = 127;
  const data::CharNames names;
  const net::PartyCard card = game::party_card(state, {0, 1, 2, 3}, names);
  CHECK_EQ(card.star, 7);
  CHECK_EQ(card.members.size(), 3);
  if (card.members.size() != 3) return;
  const net::CardMember& lead = card.members[0];
  CHECK(lead.char_id == 0 && lead.hp == 320 && lead.atk == 35 && lead.mag == 12 && lead.def == 18 && lead.mdef == 10);
  CHECK_EQ(lead.elements, 2);
  CHECK(card.members[1].char_id == 1 && card.members[1].hp == 999 && card.members[1].atk == 127 && card.members[1].elements == 0);
  CHECK_EQ(card.members[2].char_id, 2);
  ui::GameState bare;
  CHECK(game::party_card(bare, {0}, names).members.empty());
}

// ------------------------------------------------------------------------------------------------ versus
VersusSide versus_side(std::vector<PartyMember> party) {
  VersusSide side;
  side.party = std::move(party);
  return side;
}

Command versus_command(Command::Kind kind, int actor) {
  Command cmd;
  cmd.kind = kind;
  cmd.actor = actor;
  return cmd;
}

Command versus_attack(int actor, std::vector<int> chain, int target) {
  Command cmd = versus_command(Command::Kind::Attack, actor);
  cmd.chain = std::move(chain);
  cmd.target = target;
  return cmd;
}

// Side A in units 0..2 at the opening stamina, side B in 3..5 at 70, 6..8 empty; only the turn side acts; End Turn passes the turn.
void test_versus_start_and_turns() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(!battle.start_versus(versus_side({}), versus_side({make_member(3, "D")}), -1));
  CHECK(battle.start_versus(versus_side({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}),
                            versus_side({make_member(3, "D"), make_member(4, "E"), make_member(5, "F")}), -1));
  CHECK(battle.versus());
  for (int unit = 0; unit < kUnits; ++unit) CHECK_EQ(battle.unit(unit).present && battle.unit(unit).pc, unit < kPlayerUnits);
  CHECK_EQ(battle.unit(0).stamina, kVersusOpeningStamina);
  CHECK_EQ(battle.unit(2).stamina, 35);
  CHECK_EQ(battle.unit(3).stamina, 70);
  CHECK_EQ(battle.turn_side(), 0);
  CHECK(battle.can_act(0) && !battle.can_act(3));
  CHECK(!battle.apply(versus_command(Command::Kind::Defend, 3)));
  CHECK(!battle.apply(versus_command(Command::Kind::Focus, 4)));
  CHECK(!battle.apply(versus_command(Command::Kind::EndTurn, 3)));
  // An engage names a standing unit of the other side and changes nothing.
  {
    Command engage = versus_command(Command::Kind::Engage, 0);
    engage.target = 1;
    CHECK(!battle.apply(engage));
    engage.target = 4;
    const u64 before = battle.state_hash();
    CHECK(battle.apply(engage));
    CHECK_EQ(battle.state_hash(), before);
    engage.actor = 3;
    engage.target = 0;
    CHECK(!battle.apply(engage));
  }
  const int actions = battle.actions();
  CHECK(battle.apply(versus_command(Command::Kind::Focus, 1)));
  CHECK_EQ(battle.active_pc(), 1);
  CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)));
  CHECK_EQ(battle.actions(), actions);
  CHECK_EQ(battle.turn_side(), 1);
  CHECK(!battle.can_act(0) && battle.can_act(3));
  CHECK(battle.apply(versus_command(Command::Kind::Defend, 4)));
  CHECK_EQ(battle.turn_side(), 1);
  // Normal battles have no turns.
  Fixture fixture;
  CHECK(!fixture.battle.apply(versus_command(Command::Kind::EndTurn, 0)));
}

// Each action of one side counts for every character of the other side as a teammate's: weak / medium / heavy 1 / 2 / 3 steps, a cast
// 1 (its target excluded), each step the rate, x3/4 with flu, capped at 70. The side whose turn it is recovers nothing from its own actions.
void test_versus_recovery() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(battle.start_versus(versus_side({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}),
                            versus_side({make_member(3, "D"), make_member(4, "E"), make_member(5, "F")}), -1));
  for (int unit = 3; unit < 6; ++unit) battle.unit_mut(unit).stamina = 0;
  battle.unit_mut(5).lo |= st::kFlu;
  CHECK(battle.apply(versus_attack(0, {2}, 3)));
  CHECK_EQ(battle.unit(0).stamina, 5);
  CHECK_EQ(battle.unit(1).stamina, 35);
  CHECK_EQ(battle.unit(2).stamina, 35);
  CHECK_EQ(battle.unit(3).stamina, 30);
  CHECK_EQ(battle.unit(4).stamina, 30);
  CHECK_EQ(battle.unit(5).stamina, 21);
  battle.unit_mut(1).gauge = 0;
  Command cast = versus_command(Command::Kind::Element, 1);
  cast.cell = 0;
  cast.target = 4;
  CHECK(battle.apply(cast));
  CHECK_EQ(battle.unit(1).stamina, -35);
  CHECK_EQ(battle.unit(2).stamina, 35);
  CHECK_EQ(battle.unit(0).stamina, 5);
  CHECK_EQ(battle.unit(3).stamina, 40);
  CHECK_EQ(battle.unit(4).stamina, 30);
  CHECK_EQ(battle.unit(5).stamina, 28);
  // Side B's actions in its turn recover side A, not side B.
  CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 2)));
  CHECK(battle.apply(versus_attack(3, {0}, 0)));
  CHECK_EQ(battle.unit(3).stamina, 30);
  CHECK_EQ(battle.unit(4).stamina, 30);
  CHECK_EQ(battle.unit(2).stamina, 45);
}

// A versus Defend gives its +10 and the guard, then the character is done until its side's next turn; a turn in which every character is
// done or exhausted ends by itself.
void test_versus_defend_ends_turn() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(battle.start_versus(versus_side({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}),
                            versus_side({make_member(3, "D"), make_member(4, "E")}), -1));
  const u64 before = battle.state_hash();
  CHECK(battle.apply(versus_command(Command::Kind::Defend, 0)));
  CHECK(battle.state_hash() != before);
  CHECK_EQ(battle.unit(0).stamina, 45);
  CHECK(battle.unit(0).guard && battle.done(0) && !battle.can_act(0));
  CHECK(!battle.apply(versus_command(Command::Kind::Defend, 0)));
  CHECK(!battle.apply(versus_command(Command::Kind::Focus, 0)));
  CHECK(!battle.apply(versus_attack(0, {0}, 3)));
  CHECK_EQ(battle.turn_side(), 0);
  // Unit 1 defends, unit 2 runs out of stamina: the turn passes by itself.
  CHECK(battle.apply(versus_command(Command::Kind::Defend, 1)));
  battle.unit_mut(2).stamina = 10;
  CHECK(battle.apply(versus_attack(2, {0}, 3)));
  CHECK_EQ(battle.turn_side(), 1);
  CHECK(battle.done(0) && !battle.can_act(0));
  CHECK(battle.apply(versus_command(Command::Kind::Defend, 3)));
  CHECK(battle.apply(versus_command(Command::Kind::Defend, 4)));
  CHECK_EQ(battle.turn_side(), 0);
  CHECK(!battle.done(0) && battle.can_act(0) && battle.done(3) && !battle.can_act(3));
  // The guard lasts until the character's menu comes up again.
  CHECK(battle.unit(0).guard);
  CHECK(battle.apply(versus_command(Command::Kind::Focus, 0)));
  CHECK(!battle.unit(0).guard);
}

// In a versus battle hi status 0x20000 waives nothing: weapon hits and elements cost stamina, and the HIT% budget is the stamina alone.
void test_versus_no_stamina_waiver() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(battle.start_versus(versus_side({make_member(0, "A"), make_member(1, "B")}), versus_side({make_member(3, "D"), make_member(4, "E")}), -1,
                            70));
  for (int unit : {0, 1}) {
    battle.unit_mut(unit).hi |= st::kHiNoStaminaCost;
    battle.unit_mut(unit).timers[32 + 17] = 9;
  }
  CHECK(!battle.costs_waived(battle.unit(0)));
  CHECK_EQ(battle.hit_budget(0), 70);
  CHECK(battle.apply(versus_attack(0, {0, 1}, 3)));
  CHECK_EQ(battle.unit(0).stamina, 70 - battle.chain_cost(0, battle.unit(0)) - battle.chain_cost(1, battle.unit(0)));
  battle.unit_mut(1).gauge = 0;
  Command cast = versus_command(Command::Kind::Element, 1);
  cast.cell = 0;
  cast.target = 3;
  CHECK(battle.apply(cast));
  CHECK_EQ(battle.unit(1).stamina, 0);
  battle.unit_mut(0).stamina = 10;
  std::string why;
  CHECK(!battle.validate(versus_attack(0, {2}, 3), &why) && why == "not enough stamina for the chain");
  // A normal battle still waives the costs.
  Fixture fixture;
  fixture.battle.unit_mut(0).hi |= st::kHiNoStaminaCost;
  CHECK(fixture.battle.costs_waived(fixture.battle.unit(0)));
}

// A side whose turn starts with nobody able to act advances time with the step of the other side's countdowns; one that time cannot help
// passes the turn back; a side that runs out during its turn passes it.
void test_versus_stuck_side() {
  data::GameTables tables = make_tables();
  const auto make_battle = [&](Battle& battle) {
    return battle.start_versus(versus_side({make_member(0, "A"), make_member(1, "B"), make_member(2, "C")}),
                               versus_side({make_member(3, "D"), make_member(4, "E"), make_member(5, "F")}), -1);
  };
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    VersusBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(make_battle(battle));
    for (int unit = 3; unit < 6; ++unit) battle.unit_mut(unit).stamina = 0, battle.unit_mut(unit).countdown = 8;
    battle.unit_mut(0).countdown = 5, battle.unit_mut(1).countdown = 7, battle.unit_mut(2).countdown = 6;
    CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)));
    CHECK_EQ(battle.turn_side(), 1);
    CHECK_EQ(battle.unit(3).stamina, 50);
    CHECK_EQ(battle.unit(5).stamina, 50);
    CHECK_EQ(battle.unit(0).stamina, 35);
    // Unit 0 reached 0 and took its AP again; the others ran the step down.
    CHECK_EQ(battle.unit(0).countdown, 8);
    CHECK_EQ(battle.unit(1).countdown, 2);
    CHECK_EQ(battle.unit(4).countdown, 3);
  }
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    VersusBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(make_battle(battle));
    for (int unit = 3; unit < 6; ++unit) battle.unit_mut(unit).lo |= st::kAsleep;
    CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)));
    CHECK_EQ(battle.turn_side(), 0);
    CHECK_EQ(battle.unit(3).stamina, 70);
  }
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    VersusBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(make_battle(battle));
    battle.unit_mut(0).stamina = 10;
    battle.unit_mut(1).stamina = battle.unit_mut(2).stamina = -50;
    CHECK(battle.apply(versus_attack(0, {0}, 3)));
    CHECK_EQ(battle.turn_side(), 1);
  }
}

// Three turns in a row ended by a timeout forfeit the battle; a turn ended otherwise clears the run.
void test_versus_timeouts() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  CHECK(battle.start_versus(versus_side({make_member(0, "A")}), versus_side({make_member(3, "D")}), -1));
  Command timeout_a = versus_command(Command::Kind::EndTurn, 0), end_b = versus_command(Command::Kind::EndTurn, 3);
  timeout_a.timeout = true;
  CHECK(battle.apply(timeout_a) && battle.apply(end_b));
  CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)) && battle.apply(end_b));
  CHECK_EQ(battle.timeouts(0), 0);
  for (int turn = 0; turn < kVersusTimeoutLimit - 1; ++turn) CHECK(battle.apply(timeout_a) && battle.apply(end_b));
  CHECK_EQ(battle.timeouts(0), 2);
  CHECK(battle.apply(timeout_a));
  CHECK(battle.outcome() == Outcome::Wiped && battle.versus_end() == VersusEnd::Timeouts);
  CHECK_EQ(battle.winner(), 1);
}

// The last characters of both sides going down on the same action is a draw; a Run is the commanding side's surrender.
void test_versus_draw_and_surrender() {
  data::GameTables tables = make_tables();
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    VersusBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start_versus(versus_side({make_member(0, "A")}), versus_side({make_member(3, "D", 1)}), -1));
    battle.unit_mut(0).hp = 1;
    battle.unit_mut(0).lo |= st::kPoison;
    CHECK(battle.apply(versus_attack(0, {0}, 3)));
    CHECK(battle.outcome() == Outcome::Draw && battle.versus_end() == VersusEnd::Draw);
    CHECK_EQ(battle.winner(), -1);
  }
  {
    ScriptedRng rng{Lo::Fallback::Lo};
    VersusBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start_versus(versus_side({make_member(0, "A")}), versus_side({make_member(3, "D", 1)}), -1));
    CHECK(battle.apply(versus_attack(0, {0}, 3)));
    CHECK(battle.outcome() == Outcome::Victory && battle.versus_end() == VersusEnd::Knockout);
    CHECK_EQ(battle.winner(), 0);
    CHECK(battle.rewards().gold == 0 && battle.rewards().items.empty() && !battle.rewards().star_up);
  }
  for (const int side : {0, 1}) {
    ScriptedRng rng{Lo::Fallback::Lo};
    VersusBrain brain;
    Battle battle{tables, rng, brain};
    CHECK(battle.start_versus(versus_side({make_member(0, "A")}), versus_side({make_member(3, "D")}), -1));
    if (side == 1) CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)));
    CHECK(battle.apply(versus_command(Command::Kind::Run, side_first(side))));
    CHECK(battle.outcome() == (side == 0 ? Outcome::Wiped : Outcome::Victory) && battle.versus_end() == VersusEnd::Surrender);
    CHECK_EQ(battle.winner(), 1 - side);
  }
}

// The same party on both sides and mirrored commands give mirrored results (no opening handicap, every roll the lowest).
void test_versus_mirror() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  const std::vector<PartyMember> party{make_member(0, "A"), make_member(1, "B"), make_member(2, "C")};
  CHECK(battle.start_versus(versus_side(party), versus_side(party), -1, 70));
  CHECK(battle.apply(versus_command(Command::Kind::Focus, 0)));
  CHECK(battle.apply(versus_attack(0, {0, 1}, 3)));
  const Unit after_a = battle.unit(0), hit_b = battle.unit(3);
  CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)));
  CHECK(battle.apply(versus_command(Command::Kind::Focus, 3)));
  CHECK(battle.apply(versus_attack(3, {0, 1}, 0)));
  CHECK_EQ(battle.unit(3).stamina, after_a.stamina);
  CHECK_EQ(battle.unit(3).gauge, after_a.gauge);
  CHECK_EQ(battle.unit(0).hp, hit_b.hp);
  CHECK(battle.unit(0).hp < 50);
  for (int unit = 0; unit < kPcSlots; ++unit) {
    const Unit& own = battle.unit(unit);
    const Unit& mirror = battle.unit(unit + kPcSlots);
    CHECK(own.hp == mirror.hp && own.gauge == mirror.gauge && own.lo == mirror.lo && own.hi == mirror.hi && own.used == mirror.used);
  }
}

// A side's element trap waits for the other side's casts only, and a caught cast loses the caster's cell.
void test_versus_traps() {
  data::GameTables tables = make_tables();
  tables.elements.resize(5);
  auto& trap = tables.elements[4];
  trap.raw[4] = 0x44, trap.raw[5] = 0x04, trap.raw[6] = 1, trap.raw[7] = 5, trap.raw[0xE] = 1;
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  PartyMember trapper = make_member(0, "A");
  trapper.cells[1] = 0x4004;
  trapper.unlocked[1] = true;
  CHECK(battle.start_versus(versus_side({trapper, make_member(1, "B")}), versus_side({make_member(3, "D")}), -1));
  battle.unit_mut(0).gauge = battle.unit_mut(1).gauge = 0;
  Command set_trap = versus_command(Command::Kind::Element, 0);
  set_trap.cell = 1;
  CHECK(battle.apply(set_trap));
  CHECK_EQ(battle.trap_hud().pc, 1);
  // The side's own cast of that element is not caught.
  Command own_cast = versus_command(Command::Kind::Element, 1);
  own_cast.cell = 0;
  own_cast.target = 3;
  CHECK(battle.apply(own_cast));
  CHECK(battle.unit(3).hp < 50);
  CHECK_EQ(battle.trap_hud().pc, 1);
  CHECK_EQ(battle.turn_side(), 1);
  battle.unit_mut(3).gauge = 0;
  Command cast = versus_command(Command::Kind::Element, 3);
  cast.cell = 0;
  cast.target = 0;
  CHECK(battle.apply(cast));
  CHECK_EQ(battle.unit(0).hp, 50);
  CHECK_EQ(battle.trap_hud().pc, 0);
  CHECK_EQ(battle.unit(3).cells[0] & 0x3FFF, 0);
  CHECK_EQ(battle.party(1)[0].cells[0] & 0x3FFF, 0);
  CHECK(battle.rewards().trapped.empty());
}

// A versus side's double techs take partners from that side only, and the gauge rule reads that side's party slots.
void test_versus_combo_techs() {
  constexpr int kDash = 0xA0, kGash = 0xD9, kXStrike = 0x9F;
  data::GameTables tables = make_tables();
  tables.elements.resize(0x100);
  const auto tech = [&](int id, int colour, int partner, int result) {
    auto& record = tables.elements[static_cast<size_t>(id)];
    record.raw[1] = 2, record.raw[4] = 0x80, record.raw[5] = static_cast<u8>(colour), record.raw[6] = 3, record.raw[8] = 20, record.raw[0xC] = 20;
    record.raw[0x14] = record.raw[0x15] = record.raw[0x16] = record.raw[0x17] = 0xFF;
    record.raw[0x20] = static_cast<u8>(partner), record.raw[0x26] = static_cast<u8>(result);
  };
  tech(kDash, 0x08, kGash, kXStrike);
  tech(kGash, 0x04, kDash, kXStrike);
  tech(kXStrike, 0x20, 0, 0);
  tables.elements[kXStrike].raw[0x14] = 0, tables.elements[kXStrike].raw[0x15] = 2;
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  const std::vector<PartyMember> party{make_member(0, "A"), make_member(1, "B"), make_member(2, "C")};
  CHECK(battle.start_versus(versus_side(party), versus_side(party), -1));
  const auto place = [&](int unit, int cell, int id) {
    battle.unit_mut(unit).cells[static_cast<size_t>(cell)] = static_cast<u16>(id);
    battle.unit_mut(unit).usable[static_cast<size_t>(cell)] = true;
  };
  place(2, 27, kGash);
  place(3, 27, kDash);
  for (int unit = 0; unit < kPlayerUnits; ++unit) battle.unit_mut(unit).gauge_max = 7, battle.unit_mut(unit).gauge = 7;
  CHECK_EQ(battle.cell_element(3, 27), kDash);
  place(5, 27, kGash);
  const ComboTech combo = battle.combo_at(3, 27);
  CHECK_EQ(combo.element, kXStrike);
  CHECK(combo.units[0] == 3 && combo.units[1] == 5 && combo.slot_cells[0] == 27 && combo.slot_cells[2] == 27 && combo.slot_cells[1] == -1);
  CHECK(battle.apply(versus_command(Command::Kind::EndTurn, 0)));
  battle.unit_mut(3).gauge = 4, battle.unit_mut(4).gauge = 3;
  Command cast = versus_command(Command::Kind::Element, 3);
  cast.cell = 27;
  cast.target = 0;
  CHECK(battle.apply(cast));
  CHECK_EQ(battle.unit(3).gauge, 1);
  CHECK_EQ(battle.unit(5).gauge, 0);
  CHECK(battle.unit(3).used[27] && battle.unit(5).used[27] && !battle.unit(2).used[27]);
  CHECK(battle.unit(0).hp < 50);
}

// The exchange format carries every field; a damaged or foreign string decodes to nothing; the battle built from a decoded side is the
// same battle.
void test_versus_side_codec() {
  data::GameTables tables = make_tables();
  VersusSide side = versus_side({make_member(0, "Serge"), make_member(1, "Kid")});
  side.stars = 7;
  side.dopple[3] = 0x8001;
  PartyMember& member = side.party[1];
  member.own_name = "Kidd";
  member.hp = 12;
  member.kind = 0x2C;
  member.evolved = true;
  member.lo_persist = 0x21;
  member.qty[10] = 4;
  member.stack[10] = true;
  member.pending = {-3, 1, 2, 3, 4, 5, 6};
  member.eq.hp_pct = {10, 25};
  member.eq.status_hi = 0xE0000001u;
  member.eq.acc = {0xF9, 0, 0xFC};
  member.eq.dur_mod = -4;
  const std::string bytes = encode_versus_side(side);
  const std::optional<VersusSide> decoded = decode_versus_side(bytes);
  CHECK(decoded.has_value());
  if (!decoded) return;
  CHECK(encode_versus_side(*decoded) == bytes);
  CHECK(decoded->stars == 7 && decoded->dopple[3] == 0x8001 && decoded->party.size() == 2);
  const PartyMember& back = decoded->party[1];
  CHECK(back.name == "Kid" && back.own_name == "Kidd" && back.hp == 12 && back.kind == 0x2C && back.evolved && back.lo_persist == 0x21);
  CHECK(back.qty[10] == 4 && back.stack[10] && back.unlocked == member.unlocked && back.cells == member.cells && back.pending == member.pending);
  CHECK(back.eq.hp_pct == member.eq.hp_pct && back.eq.status_hi == member.eq.status_hi && back.eq.acc == member.eq.acc && back.eq.dur_mod == -4);
  CHECK(!decode_versus_side(bytes.substr(0, bytes.size() - 1)));
  CHECK(!decode_versus_side(bytes + '\0'));
  CHECK(!decode_versus_side(std::string(1, static_cast<char>(kVersusSideFormat + 1)) + bytes.substr(1)));
  CHECK(!decode_versus_side(encode_versus_side(versus_side({member, member, member, member}))));
  CHECK(!decode_versus_side(""));
  ScriptedRng rng_one{Lo::Fallback::Lo}, rng_two{Lo::Fallback::Lo};
  VersusBrain brain_one, brain_two;
  Battle one{tables, rng_one, brain_one}, two{tables, rng_two, brain_two};
  CHECK(one.start_versus(side, side, -1) && two.start_versus(*decoded, *decoded, -1));
  CHECK_EQ(one.state_hash(), two.state_hash());
}

// The state hash follows the rules state and the generator, not the names; the cheats stay off in a versus battle.
void test_versus_state_hash() {
  data::GameTables tables = make_tables();
  ScriptedRng rng_one{Lo::Fallback::Lo}, rng_two{Lo::Fallback::Lo};
  VersusBrain brain_one, brain_two;
  Battle one{tables, rng_one, brain_one}, two{tables, rng_two, brain_two};
  CHECK(one.start_versus(versus_side({make_member(0, "A")}), versus_side({make_member(3, "D")}), -1));
  CHECK(two.start_versus(versus_side({make_member(0, "Other")}), versus_side({make_member(3, "Names")}), -1));
  CHECK_EQ(one.state_hash(), two.state_hash());
  Battle::Cheats cheats;
  cheats.god_mode = cheats.max_stamina = true;
  one.set_cheats(cheats);
  one.set_invincible(true, true);
  CHECK_EQ(one.state_hash(), two.state_hash());
  CHECK(one.apply(versus_attack(0, {0}, 3)));
  CHECK(one.state_hash() != two.state_hash());
  CHECK(two.apply(versus_attack(0, {0}, 3)));
  CHECK_EQ(one.state_hash(), two.state_hash());
  rng_two.rnd(0, 5);
  CHECK(one.state_hash() != two.state_hash());
}

// The battlefield comes from the ordinary scenes' backgrounds, the same one for the same seed.
void test_versus_battlefield() {
  data::GameTables tables = make_tables();
  CHECK_EQ(versus_battlefield(tables, 1), 0);
  const auto scene = [](int bg, bool boss, bool enemies) {
    data::SceneRec rec;
    rec.raw.fill(0);
    rec.raw[0] = enemies ? 0 : 0xFF, rec.raw[1] = rec.raw[2] = 0xFF, rec.raw[3] = enemies ? 1 : 0;
    rec.raw[0x1A] = static_cast<u8>(bg | (boss ? 0x80 : 0));
    return rec;
  };
  tables.scenes = {scene(5, false, true), scene(9, true, true), scene(5, false, true), scene(110, false, true), scene(7, false, false), scene(2, false, true)};
  bool seen_2 = false, seen_5 = false;
  for (u32 seed = 0; seed < 64; ++seed) {
    const int field = versus_battlefield(tables, seed);
    CHECK(field == 2 || field == 5);
    CHECK_EQ(versus_battlefield(tables, seed), field);
    seen_2 = seen_2 || field == 2, seen_5 = seen_5 || field == 5;
  }
  CHECK(seen_2 && seen_5);
  tables.scenes = {scene(9, true, true)};
  CHECK_EQ(versus_battlefield(tables, 3), -1);
  const std::string fingerprint = data::battle_data_fingerprint(tables);
  CHECK(fingerprint.size() == 16 && fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(data::battle_data_fingerprint(tables) == fingerprint);
  tables.elements[1].raw[8] = 13;
  CHECK(data::battle_data_fingerprint(tables) != fingerprint);
  tables.enemy_names = {"renamed"};
  tables.elements[1].raw[8] = 12;
  CHECK(data::battle_data_fingerprint(tables) == fingerprint);
}

// A versus command belongs to the acting side: while a Dopplegang pick is open only the pick comes, even when the cast spent its side's
// turn, and then the turn side's commands.
void test_versus_pick_owner() {
  data::GameTables tables = make_tables();
  tables.elements.resize(kDopplegangElement + 1);
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  PartyMember caster = make_member(0, "A");
  caster.cells[0] = static_cast<u16>(0x4000 | kDopplegangElement);
  CHECK(battle.start_versus(versus_side({caster}), versus_side({make_member(3, "D"), make_member(4, "E")}), -1, 70));
  battle.unit_mut(0).gauge = 0;
  std::string why;
  CHECK(!battle.apply(versus_command(Command::Kind::Defend, 3), &why) && why == "not this side's turn");
  Command cast = versus_command(Command::Kind::Element, 0);
  cast.cell = 0;
  CHECK(battle.apply(cast, &why));
  CHECK_EQ(battle.form_owed(), 0);
  CHECK_EQ(battle.turn_side(), 1);
  CHECK_EQ(battle.acting_side(), 0);
  for (const Command::Kind kind : {Command::Kind::Run, Command::Kind::EndTurn, Command::Kind::Defend, Command::Kind::Focus}) {
    CHECK(!battle.apply(versus_command(kind, 3), &why));
    CHECK(why == "a Dopplegang pick is open");
  }
  Command other_pick = versus_command(Command::Kind::Form, 3);
  other_pick.form = -1;
  CHECK(!battle.apply(other_pick));
  Command pick = versus_command(Command::Kind::Form, 0);
  pick.form = -1;
  CHECK(battle.apply(pick));
  CHECK_EQ(battle.form_owed(), -1);
  CHECK(!battle.apply(versus_command(Command::Kind::Run, 0), &why) && why == "not this side's turn");
  CHECK(battle.apply(versus_command(Command::Kind::Defend, 3)));
}

// A refused Element command of a versus battle leaves the units as they were (its element menu opening included), so both games stay in
// step whichever of them refused it.
void test_versus_refused_element() {
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  PartyMember member = make_member(0, "A");
  member.cells[5] = 0x4001;
  member.unlocked[5] = member.stack[5] = true;
  CHECK(battle.start_versus(versus_side({member}), versus_side({make_member(3, "D")}), -1));
  Command cast = versus_command(Command::Kind::Element, 0);
  cast.cell = 0;
  CHECK(!battle.apply(cast));
  CHECK(!battle.unit(0).used[5]);
  CHECK(battle.apply(versus_command(Command::Kind::ElementMenu, 0)));
  CHECK(battle.unit(0).used[5]);
}

// Numbers no record holds neither hang nor overflow the rules: a unit's numbers are held to their ranges and the roll sums are cut.
void test_versus_huge_numbers() {
  constexpr int kHuge = std::numeric_limits<int>::max();
  ScriptedRng sums{Lo::Fallback::Lo};
  CHECK_EQ(sum_rolls(sums, kHuge, 1, 0), 0x10000 * 100);
  CHECK_EQ(heal_amount(sums, kHuge, 255, 1000), 999);
  data::GameTables tables = make_tables();
  ScriptedRng rng{Lo::Fallback::Lo};
  VersusBrain brain;
  Battle battle{tables, rng, brain};
  PartyMember giant = make_member(0, "A");
  giant.stat.fill(kHuge);
  giant.hp = kHuge;
  giant.rate = giant.dur = giant.hit_med = giant.hit_heavy = kHuge;
  giant.eq.stamina_mod = giant.eq.start_gauge = kHuge;
  giant.eq.hp_pct = {kHuge};
  PartyMember wall = giant;
  wall.char_id = 3;
  CHECK(battle.start_versus(versus_side({giant}), versus_side({wall}), -1, 70));
  CHECK_EQ(battle.unit(0).max_hp, 30000);
  CHECK_EQ(battle.unit(0).hp, 999);
  CHECK_EQ(battle.unit(0).def, 9999);
  CHECK(battle.apply(versus_attack(0, {2}, 3)));
}

// The record set of the legality tests: characters 0 to 5 with a starting kit (Aqua Beam in cell 0, an empty cell 1, the level 2 tech 0x80 in
// cell 10 and an empty cell 11), weapon-less.
data::GameTables make_versus_tables() {
  data::GameTables tables = make_tables();
  tables.elements[1].raw[0x12] = 0x00;
  tables.elements.resize(0x81);
  tables.elements[0x80].raw[1] = 0x02, tables.elements[0x80].raw[6] = 2;
  data::CharSet set;
  for (int id = 0; id < 6; ++id) {
    data::StatRec& rec = set.rec[static_cast<size_t>(id)];
    rec.raw[0x01] = 0x08, rec.raw[0x16] = 50, rec.raw[0x18] = 9, rec.raw[0x19] = 5, rec.raw[0x1A] = 85, rec.raw[0x1B] = 8, rec.raw[0x1C] = 7,
    rec.raw[0x1D] = 7, rec.raw[0x1E] = 13, rec.raw[0x1F] = 10, rec.raw[0xA4] = 50, rec.raw[0xA5] = 20;
    rec.raw[0x30] = 0x03, rec.raw[0x32] = 0x03;
    rec.raw[0x40] = 0x01, rec.raw[0x41] = 0x40, rec.raw[0x43] = 0x40, rec.raw[0x44] = 0x80, rec.raw[0x45] = 0xC0, rec.raw[0x47] = 0x40;
    set.lv99[static_cast<size_t>(id)] = {850, 88, 78, 95, 25, 52, 43};
  }
  tables.char_sets["char"] = set;
  return tables;
}

// A received side is refused for anything a real game cannot send; a side built from the records passes.
void test_versus_side_legality() {
  const data::GameTables tables = make_versus_tables();
  VersusSide legal;
  legal.party = {make_party_member(tables, 0, 0), make_party_member(tables, 1, 0)};
  legal.stars = 5;
  legal.dopple[0] = 1;
  std::string why;
  CHECK(legal_versus_side(tables, legal, &why));
  CHECK_EQ(legal.party[0].cells[10], 0xC080);
  const auto refused = [&](const std::function<void(VersusSide&)>& change, const char* reason) {
    VersusSide side = legal;
    change(side);
    std::string got;
    const bool ok = legal_versus_side(tables, side, &got);
    if (ok || got.find(reason) == std::string::npos) std::printf("legality: expected '%s', got '%s'\n", reason, ok ? "legal" : got.c_str());
    return !ok && got.find(reason) != std::string::npos;
  };
  VersusSide moved = legal;
  moved.party[0].cells[1] = 0x4002;
  moved.party[0].cells[10] = 0x4000;
  moved.party[0].cells[11] = 0xC080;
  moved.party[0].innate = 0x20;
  CHECK(legal_versus_side(tables, moved, &why));
  CHECK(refused([](VersusSide& side) { side.party.clear(); }, "not 1 to 3"));
  CHECK(refused([](VersusSide& side) { side.party[1].char_id = 0; }, "a character twice"));
  CHECK(refused([](VersusSide& side) { side.party[0].char_id = 44; }, "not a playable character"));
  CHECK(refused([](VersusSide& side) { side.party[0].name = "A\nB"; }, "control characters"));
  CHECK(refused([](VersusSide& side) { side.party[0].own_name = "\xC0\x80"; }, "control characters"));
  CHECK(refused([](VersusSide& side) { side.party[0].kind = 0x2C; }, "a body"));
  CHECK(refused([](VersusSide& side) { side.party[0].evolved = true; }, "a form change"));
  CHECK(refused([](VersusSide& side) { side.party[0].dur = 99; }, "record constants"));
  CHECK(refused([](VersusSide& side) { side.party[0].innate = 0x03; }, "no colour"));
  CHECK(refused([](VersusSide& side) { side.party[0].base[1] = 99; }, "growth record"));
  CHECK(refused([](VersusSide& side) { side.party[0].stat[1] = 128; }, "a stat past"));
  CHECK(refused([](VersusSide& side) { side.party[0].stat[0] = 0; }, "a stat past"));
  CHECK(refused([](VersusSide& side) { side.party[0].pending[2] = 500; }, "growth words"));
  CHECK(refused([](VersusSide& side) { side.party[0].hp = 51; }, "HP past"));
  CHECK(refused([](VersusSide& side) { side.party[0].lo_persist = 0x2; }, "a status"));
  CHECK(refused([](VersusSide& side) { side.party[0].eq.wp = 999; }, "gear"));
  CHECK(refused([](VersusSide& side) { side.party[0].eq.status_hi = st::kHiImmune; }, "gear"));
  CHECK(refused([](VersusSide& side) { side.party[0].eq.weapon_id = 0x3D; }, "gear"));
  CHECK(refused([](VersusSide& side) { side.party[0].cells[1] = 0x4005; }, "placement window"));
  CHECK(refused([](VersusSide& side) { side.party[0].cells[1] = 0xC080; }, "a tech no player holds there"));
  CHECK(refused([](VersusSide& side) { side.party[0].cells[10] = 0x4001; }, "placement window"));
  CHECK(refused([](VersusSide& side) { side.party[0].cells[2] = 0x4001; }, "does not have"));
  CHECK(refused([](VersusSide& side) { side.party[0].cells[11] = 0x4001; }, "placement window"));
  CHECK(refused([](VersusSide& side) { side.party[0].qty[0] = 3; }, "no stack"));
  CHECK(refused([](VersusSide& side) { side.party[0].stack[0] = true; }, "does not stack"));
  CHECK(refused([](VersusSide& side) { side.stars = 100; }, "stars"));
  CHECK(refused([](VersusSide& side) { side.dopple[14] = 1 << 6; }, "pool"));
}

int main() {
  test_stack_restock();
  test_gear();
  test_rng();
  test_target_cursor_order();
  test_formulas();
  test_encounter_variant();
  test_hit_chance();
  test_colour_and_ring();
  test_growth();
  test_setup();
  test_gauge_cap();
  test_enemy_gauge_cap();
  test_ring_battlefield_fallback();
  test_settle_with_pending_evolution();
  test_pip_idle_loop_settles();
  test_pip_colour_points();
  test_stamina_sequence();
  test_action_events();
  test_status_and_poison();
  test_enemy_element_value_shift();
  test_element_mode40_half_hp();
  test_ko_drops_pc_statuses();
  test_stamina_statuses_on_heavy_hit();
  test_poison_ko();
  test_revive_resets_stamina_and_countdown();
  test_revive_to_zero_hp();
  test_empty_stack_greyed();
  test_heal_ignores_ring_colour();
  test_saints_by_target_slot();
  test_purify_cures();
  test_turn_colour_status();
  test_hit_box_figure();
  test_darkness_active_pc();
  test_status_message();
  test_status_mode_codes();
  test_frozen_turns();
  test_field_ring_writes();
  test_queued_cast_targets();
  test_trap_hud();
  test_summon_gate();
  test_trap_needs_colour();
  test_pilfer();
  test_forced_victory_pays_defeated_only();
  test_victory_and_growth();
  test_determinism();
  test_pac_vm();
  test_pac_kill_and_revive();
  test_colour_gate();
  test_scripted_brain();
  test_enemy_pick_keeps_gauge();
  test_scripted_heal_on_ko_pc();
  test_pac_script_stream();
  test_pac_init_order();
  test_pac_countdown_restore();
  test_pac_stat_write();
  test_scripted_battle_reentry();
  test_hp_known_status_applies();
  test_blue_seal_is_not_death();
  test_anti_colour_seals();
  test_pac_resolved_ops();
  test_scripted_trap_fizzle();
  test_tutorial_enemy_undying();
  test_end_rules();
  test_scripted_hp_write_runs_kill_script();
  test_forced_result_waits_for_death_cast();
  test_hidden_units();
  test_hidden_pair_reveal_one();
  test_reveal_during_all_target_cast();
  test_weapon_sweep();
  test_idle_loop_cast();
  test_debug_flags();
  test_god_mode();
  test_battle_boost();
  test_dopple_pool_and_form();
  test_dopple_exit_unit();
  test_record_status_carry();
  test_dopple_way_back_after_pip_form();
  test_gear_hp_pct_and_forms();
  test_dopple_form_star_growth();
  test_auto_hit_level();
  test_auto_hit_level_fresh_pair();
  test_frozen_enemy_targetable();
  test_element_modes_revenge_heal_piggy_hero();
  test_ps1_cast_colours();
  test_hero_set();
  test_sprained_attacker();
  test_gear_duration_modifier();
  test_weapon_tech_record_colour();
  test_weapon_tech_participants();
  test_sealed_caster_fizzles();
  test_pc_countdowns_run_with_actions();
  test_chain_across_interrupting_enemy_turn();
  test_weapon_hit_ends_confusion();
  test_weapon_tech_wake_crit_drain();
  test_no_stamina_cost_status();
  test_hit_budget();
  test_time_runs_while_nobody_can_act();
  test_scripted_pc_entry_upkeep();
  test_weapon_tech_immune_absorb();
  test_combo_techs();
  test_combo_real_tables();
  test_run_drops_stolen_loot();
  test_stat_mode_hero_gate_and_heal();
  test_dopple_form_gauge_cap();
  test_stolen_loot_needs_victory();
  test_star_up_grid_unlocks();
  test_scene_1d8_clears_status();
  test_pip_form_growth();
  test_pip_form_settles_at_99();
  test_battle_grid_follows_the_masks();
  test_party_card();
  test_monster_party();
  test_lynx_duel_bodies();
  test_unnamed_kid_label();
  test_enemy_names_follow_the_language();
  test_party_swap();
  test_versus_start_and_turns();
  test_versus_recovery();
  test_versus_defend_ends_turn();
  test_versus_no_stamina_waiver();
  test_versus_stuck_side();
  test_versus_timeouts();
  test_versus_draw_and_surrender();
  test_versus_mirror();
  test_versus_traps();
  test_versus_combo_techs();
  test_versus_side_codec();
  test_versus_state_hash();
  test_versus_battlefield();
  test_versus_pick_owner();
  test_versus_refused_element();
  test_versus_huge_numbers();
  test_versus_side_legality();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("battle tests ok\n");
  return 0;
}
