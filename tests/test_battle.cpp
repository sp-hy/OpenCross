// Battle core checks: formulas with hand-worked numbers, stamina accounting over a sample turn
// sequence, field-colour FIFO, growth caps, status durations. Uses synthetic tables, no game files.
#include <memory>
#include <cstdio>
#include <cstring>
#include <utility>

#include "battle/battle.h"
#include "battle/pac_vm.h"
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
  CHECK_EQ(heal_amount(lh, 7, 18, 1000, 1000), 40);
  Lo lh2(Lo::Fallback::Lo);
  // Capped at 999.
  CHECK_EQ(heal_amount(lh2, 200, 18, 1000, 1500), 999);
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
  CHECK_EQ(recovery_step(10, 0, 2, true), 15);
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
  // minor growth: 0.5*((snap+pending*b/10)-cur)*sqrt(P/S)
  // 0.5*14*1 = 7; 0.5*14*0.5 = 3.5 -> 4; already above the target: floor 0.
  CHECK_EQ(minor_growth(50, 20, 50, 7, 8, 8), 7);
  CHECK_EQ(minor_growth(50, 20, 50, 7, 2, 8), 4);
  CHECK_EQ(minor_growth(50, 20, 70, 7, 8, 8), 0);
  CHECK_EQ(minor_growth(50, 20, 50, 7, 8, 0), 0);
  CHECK_EQ(progress_cap({8, 7, 1}), 8);
  CHECK_EQ(progress_cap({10, 9, 64}), 640);
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

// An enemy's gauge tops out at the deepest row with an element cell that lacks the lock bit 0x4000 (none: -1, so landed hits leave
// it at -1).
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
    CHECK_EQ(battle.unit(3).gauge, locked ? -1 : 2);
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
  // The defender's own menu stays closed while the others can still act ...
  CHECK(battle.unit(1).defending && !battle.can_act(1));
  CHECK(battle.can_act(0) && battle.can_act(2));
  // ... and reopens once another PC has used a menu (the check does not look at the flag).
  Command d0;
  d0.kind = Command::Kind::Defend;
  d0.actor = 0;
  CHECK(battle.apply(d0));
  CHECK(!battle.unit(1).defending && battle.can_act(1));
  CHECK(battle.unit(0).defending && !battle.can_act(0));
  // or when time advances (the lone defender case runs the time step)
  battle.time_advance();
  CHECK(!battle.unit(0).defending && battle.can_act(0));
  battle.unit_mut(0).guard = battle.unit_mut(1).guard = false;

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
  battle.post_action(0, 1, 100);
  CHECK((battle.unit(1).lo & st::kAfraid) == 0 && battle.unit(1).timers.empty());

  // poison: 1 per defend step on every living unit, PCs can die, enemies stay >= 1
  battle.unit_mut(2).lo |= st::kPoison;
  battle.unit_mut(3).lo |= st::kPoison;
  const int hp2 = battle.unit(2).hp;
  battle.post_action(0, 7, 1);
  CHECK_EQ(battle.unit(2).hp, hp2 - 7);
  battle.unit_mut(3).hp = 3;
  battle.post_action(0, 7, 1);
  CHECK_EQ(battle.unit(3).hp, 1);
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
  CHECK((battle.unit(1).lo & 0xFFFFFF3Fu & ~st::kPoison) == 0);
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
}

void test_big_catch() {
  // Mode 39 (Korcha's Big Catch): roll rnd(0, 16) (0 = the line breaks: no creature, the cast misses, the record keeps model 0), then rnd(0, 255) until
  // the slot's pool bit is set; model = kDoppleTable[slot].model; the slot term = max(10, (10000 - model) / 200) * 100 replaces the grid slot term
  // (slot 26 = model 86 -> 4900).
  struct Out {
    int creature = -9, code = -9, value = -9;
    size_t events = 0;
  };
  const auto run = [](const std::array<u16, 16>& mask, const std::vector<int>& rolls, int row = 0) {
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
    // mode 6: never clears the status flag -> the row always shows
    Fixture fixture;
    elem(fixture, 6, 54)->raw[0xE] = 1;
    fixture.battle.do_element(3, 4, 0, 0);
    CHECK(!fixture.battle.events().empty());
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
  const size_t before = battle.events().size();
  battle.settle();
  CHECK(battle.events().size() > before);
  if (battle.events().size() > before) {
    const ActionEvent& caught = battle.events()[before];
    CHECK_EQ(caught.element, 2);
    CHECK_EQ(caught.code, 2);
    CHECK(caught.trap == (TrapHud{true, 2, 0}));
    CHECK(caught.trap_before == (TrapHud{true, 2, 0}));
  }
  CHECK(battle.trap_hud() == TrapHud{});
  CHECK_EQ(battle.rewards().trapped.size(), size_t{1});
}

void test_pilfer() {
  // Mode 30: the chance byte = the heavy attack's HIT% of the command box (72 for Kid and
  // Serge whatever their growth byte), doubled (byte arithmetic) for the opposite gender (PC 0 vs the male enemy); rnd(0,100) >= chance fails (code 2);
  // rnd(0,99) < 10 takes slot +6 (rare) else +4; one steal per enemy; both slots empty -> nothing.
  const auto chance_of = [](const Fixture& fixture) { return static_cast<int>(static_cast<u8>(fixture.battle.hit_chance_for(0, 3, 2) * 2)); };
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
  // non-boss victory with pending deltas: ATK 9, snap 9, pending 20, b=7 -> 0.5*(9+14-9)*sqrt(1/8) = 2.47 -> 2
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
    CHECK_EQ(battle.rewards().gains[0][1], 2);
    CHECK_EQ(battle.party()[0].stat[1], 11);
    // The snapshot only moves on a star-up.
    CHECK_EQ(battle.party()[0].snap[1], 9);
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
}

// ------------------------------------------------------------------------------------------------ battle script VM
// A hand-assembled .pac: 5 entities (0 global, 1-3 PCs, 4 = monster unit 3). Entity 4: slot 0 binds unit 3, slot 2 =
//   set v406=3+value; sub v406,3; [2B v408 = target code `select`]; dialogue 7; 66; 87; 67 type (0 = weapon) strength / element v406 target
//   mask 1 (PC1); A4; halt.
Bytes assemble_pac(int type = 0, int value = 0, int select = -1) {
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
  emit({0x67, type, 0x00, 0x06, 0x04, 0x01, 0x00, 0xA0});
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
    int shade_unit = -1, shade = -1, cursor = -1;
    u32 alive_mask() const override { return 0xF; }
    int read_field(int, u16) override { return 0; }
    void write_field(int, u16, int) override {}
    int rnd(int lo, int) override { return lo; }
    // The tag 0x65636166 spells "face".
    bool effect_busy(int pool, u32 tag) const override { return busy && pool == 1 && tag == 0x65636166; }
    void set_shade(int unit, int value) override { shade_unit = unit, shade = value; }
    void set_menu_cursor(int value) override { cursor = value; }
  };
  CHECK_EQ(pac::op_info(0xAB).conf, 'C');
  CHECK_EQ(pac::op_info(0xC5).conf, 'I');
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
    // engine flags, scripted-death flag, shade, menu cursor
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
    CHECK_EQ(host.shade_unit, 3);
    CHECK_EQ(host.shade, 5);
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
  }
}

}  // namespace

// Op 81 suspends the end test, an op-83 code ends the fight first (Outcome::Scripted, result_code), the
// party test comes before the enemy test (a mutual KO is a defeat); slot-4 counter-attacks are executed.
struct CtlBrain final : EnemyBrain {
  bool end_on = true;
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
    if (unit < kPcSlots || reactions > 0) return;
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
  CHECK(battle.party()[0].evolved && battle.party()[0].kind == 0x2D && battle.party()[0].growth_col == 3);
  CHECK_EQ(battle.element_colour(0x123), 0x04);
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
    CHECK_EQ(fixture.battle.unit(0).lo & 0x400u, 0);
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

int main() {
  test_gear();
  test_rng();
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
  test_stamina_sequence();
  test_action_events();
  test_status_and_poison();
  test_saints_by_target_slot();
  test_purify_cures();
  test_turn_colour_status();
  test_status_message();
  test_field_ring_writes();
  test_queued_cast_targets();
  test_trap_hud();
  test_pilfer();
  test_forced_victory_pays_defeated_only();
  test_victory_and_growth();
  test_determinism();
  test_pac_vm();
  test_scripted_brain();
  test_scripted_heal_on_ko_pc();
  test_pac_script_stream();
  test_pac_init_order();
  test_pac_countdown_restore();
  test_scripted_battle_reentry();
  test_hp_known_status_applies();
  test_blue_seal_is_not_death();
  test_anti_colour_seals();
  test_pac_resolved_ops();
  test_end_rules();
  test_scripted_hp_write_runs_kill_script();
  test_hidden_units();
  test_hidden_pair_reveal_one();
  test_debug_flags();
  test_god_mode();
  test_dopple_pool_and_form();
  test_auto_hit_level();
  test_element_modes_revenge_heal_piggy_hero();
  test_ps1_cast_colours();
  test_hero_set();
  test_weapon_tech_record_colour();
  test_weapon_tech_participants();
  test_combo_techs();
  test_combo_real_tables();
  test_run_drops_stolen_loot();
  test_stat_mode_hero_gate_and_heal();
  test_dopple_form_gauge_cap();
  test_stolen_loot_needs_victory();
  test_monster_party();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("battle tests ok\n");
  return 0;
}
