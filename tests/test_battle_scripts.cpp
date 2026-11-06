// Battle rules run against the shipped battle scripts and tables, one scenario per check. Needs the game install (OPENCROSS_GAMEDIR or a
// parent of the exe); without it the test reports SKIP and passes.
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "battle/battle.h"
#include "battle/pac_vm.h"
#include "battle/rng.h"
#include "data/tables.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::battle;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (ok) return;
  std::printf("FAIL %s\n", what);
  ++failures;
}

pac::ScriptedBrain::Loader loader(const vfs::Vfs& files) {
  return [&files](int scene) { return files.read(std::format("cdrom/battle/event/evt{:03}.pac", scene)); };
}

int first_pc_ready(const Battle& battle) {
  for (int pc = 0; pc < kPcSlots; ++pc)
    if (battle.can_act(pc)) return pc;
  return -1;
}

// Battle 450 (the Criosphinx, battlefield 43, Serge / Kid / Guile): the first hit on it starts the reaction cutscene (idle loop of entity 0,
// var 0x80 2 -> 3, which writes the boss's countdown to 5). When that hit also ran the boss's countdown out, the boss's turn was already
// claimed: it attacks right after the cutscene, before any PC acts again.
void check_reaction_cutscene_keeps_the_claimed_turn(const data::GameTables& tables, const vfs::Vfs& files) {
  GameRng rng(7, tables.rng_table);
  pac::ScriptedBrain brain(loader(files));
  Battle battle(tables, rng, brain);
  std::vector<PartyMember> party;
  for (const int id : {0, 1, 2}) party.push_back(make_party_member(tables, id));
  BattleOptions options;
  options.battlefield = 43;
  if (!battle.start(party, 450, options)) {
    check(false, "450: start");
    return;
  }
  battle.settle();
  check(brain.script_var(0x80) == 1, "450: the intro cutscene ran");
  const int pc = first_pc_ready(battle);
  check(pc >= 0, "450: a PC can act");
  if (pc < 0) return;
  battle.unit_mut(3).countdown = 1;
  battle.drain_events();
  Command hit;
  hit.kind = Command::Kind::Attack;
  hit.actor = pc;
  hit.chain = {0};
  hit.target = 3;
  check(battle.apply(hit), "450: the weak hit is accepted");
  battle.settle();
  check(brain.script_var(0x80) == 3 && brain.script_var(0x82) == 1, "450: the reaction cutscene ran to its end");
  int first_element = 0;
  for (const ActionEvent& event : battle.drain_events())
    if (!first_element && event.actor == 3 && event.kind == ActionEvent::Kind::Element) first_element = event.element;
  check(first_element == 0x15, "450: the boss attacks (its fight routine, element 0x15 first) before the next PC command");
}

// Battle 1 (room 53's encounter), Sprigg + Pip + Serge: Sprigg takes monster record 3 (kind 0x43), Pip changes form, Sprigg at 123 HP takes the
// way back. Her saved unit comes back with HP 123 and Pip's max HP, as on the PS1 and RDE.
void check_dopple_way_back_after_pip_form(const data::GameTables& tables, const vfs::Vfs& files) {
  GameRng rng(7, tables.rng_table);
  pac::ScriptedBrain brain(loader(files));
  Battle battle(tables, rng, brain);
  std::vector<PartyMember> party;
  for (const int id : {26, 43, 0}) party.push_back(make_party_member(tables, id));
  party[1].kind = 0x2B;
  if (!battle.start(party, 1)) {
    check(false, "dopple: start");
    return;
  }
  const Unit start = battle.unit(0);
  const int pip_max_hp = battle.unit(1).max_hp;
  check(battle.transform(0, 3), "dopple: the creature form");
  battle.evolve(1, 0x2C);
  battle.unit_mut(0).hp = 123;
  check(battle.transform(0, -1), "dopple: the way back");
  const Unit& unit = battle.unit(0);
  std::printf("dopple way back: hp %d/%d stamina %d countdown %d ap %d gauge %d stats %d %d %d %d %d %d lo %#x hi %#x\n", unit.hp, unit.max_hp,
              unit.stamina, unit.countdown, unit.ap, unit.gauge, unit.atk, unit.def, unit.hit, unit.evd, unit.mag, unit.mdef, unit.lo, unit.hi);
  check(unit.hp == 123 && unit.max_hp == pip_max_hp && battle.party()[0].stat[0] == pip_max_hp, "dopple: HP of now, Pip's max HP");
  check(unit.stamina == start.stamina && unit.countdown == 8 && unit.ap == 8 && unit.gauge == -1, "dopple: the saved stamina, countdown 8, AP 8, gauge -1");
  check(unit.atk == start.atk && unit.def == start.def && unit.hit == start.hit && unit.evd == start.evd && unit.mag == start.mag &&
            unit.mdef == start.mdef && unit.lo == start.lo && unit.hi == start.hi && unit.cells == start.cells && unit.kind == start.kind,
        "dopple: the battle-start stats, statuses, cells and kind");
}

// The same fight to its end from the save the PS1 capture continued (star 3, growth progress 0; Sprigg 300 / 300 with the HP growth
// bytes 300 / 8; Pip 85 / 85): after the way back with Pip's max HP 85 the victory grows her record from 85, to 135 as on the PS1 and RDE.
void check_dopple_growth_after_way_back(const data::GameTables& tables, const vfs::Vfs& files) {
  GameRng rng(7, tables.rng_table);
  pac::ScriptedBrain brain(loader(files));
  Battle battle(tables, rng, brain);
  std::vector<PartyMember> party;
  for (const int id : {26, 43, 0}) party.push_back(make_party_member(tables, id, 3));
  party[0].stat[0] = party[0].hp = party[0].snap[0] = 300;
  party[0].pending[0] = 8;
  party[1].stat[0] = party[1].hp = 85;
  party[1].kind = 0x2B;
  BattleOptions options;
  options.growth = {3, 0, 3};
  if (!battle.start(party, 1, options)) {
    check(false, "dopple growth: start");
    return;
  }
  check(battle.transform(0, 3), "dopple growth: the creature form");
  battle.evolve(1, 0x2C);
  battle.unit_mut(0).hp = 123;
  check(battle.transform(0, -1), "dopple growth: the way back");
  for (int tries = 0; tries < 40 && battle.outcome() == Outcome::Ongoing; ++tries) {
    int target = -1;
    for (int unit = kPcSlots; unit < kUnits && target < 0; ++unit)
      if (battle.unit(unit).present && battle.unit(unit).alive()) target = unit;
    const int pc = first_pc_ready(battle);
    if (target < 0 || pc < 0) {
      battle.settle();
      continue;
    }
    battle.unit_mut(target).hp = 1;
    Command attack;
    attack.kind = Command::Kind::Attack;
    attack.actor = pc;
    attack.chain = {0};
    attack.target = target;
    battle.apply(attack);
    battle.settle();
  }
  std::printf("dopple growth: outcome %d, Sprigg's record %d / %d (gain %d)\n", static_cast<int>(battle.outcome()), battle.party()[0].hp,
              battle.party()[0].stat[0], battle.rewards().gains[0][0]);
  check(battle.outcome() == Outcome::Victory, "dopple growth: the fight is won");
  check(battle.party()[0].stat[0] == 135 && battle.rewards().gains[0][0] == 50, "dopple growth: max HP 85 + 50");
}

}  // namespace

int main() {
  const char* env = std::getenv("OPENCROSS_GAMEDIR");
  const auto dir = platform::find_game_dir(env ? env : "");
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  data::GameTables tables;
  std::string err;
  if (!tables.load(files, &err)) {
    std::printf("FAIL tables: %s\n", err.c_str());
    return 1;
  }
  check_reaction_cutscene_keeps_the_claimed_turn(tables, files);
  check_dopple_way_back_after_pip_form(tables, files);
  check_dopple_growth_after_way_back(tables, files);
  if (failures == 0) std::printf("battle_scripts: all passed\n");
  return failures == 0 ? 0 : 1;
}
