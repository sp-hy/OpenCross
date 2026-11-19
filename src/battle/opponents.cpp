#include "battle/opponents.h"

#include <algorithm>

#include "battle/rng.h"
#include "data/tables.h"

namespace oc::battle {

namespace {

constexpr u32 kEnemyUnits = ((1u << kUnits) - 1) & ~((1u << kPcSlots) - 1);
constexpr int kSelf = 0xE9;

// The unit a constant unit operand names (E0..E8, E9 the entity's own unit); -1 for none.
int named_unit(int operand, int own) {
  operand &= 0xFF;
  if (operand == kSelf) return own;
  if (operand >= 0xE0) operand -= 0xE0;
  return operand < kUnits ? operand : -1;
}

}  // namespace

u32 revealable_units(const pac::Module& module, const std::vector<int>& unit_of_entity) {
  const auto unit_bit = [](int unit) { return unit >= 0 && unit < kUnits ? 1u << unit : kEnemyUnits; };
  const auto entity_unit = [&](int entity) {
    return entity >= 0 && static_cast<size_t>(entity) < unit_of_entity.size() ? unit_of_entity[static_cast<size_t>(entity)] : -1;
  };
  u32 out = 0;
  for (const pac::ScriptExtent& script : module.scripts()) {
    const int own = entity_unit(script.entity);
    for (u32 pc = script.start; pc < script.end;) {
      const u8 op = module.at(pc);
      const int length = pac::op_info(op).len;
      if (length == 0 || pc + static_cast<u32>(length) > script.end) break;
      const auto byte = [&](int offset) { return static_cast<int>(module.at(pc + static_cast<u32>(offset))); };
      if (op == 0x34 && byte(1) == 0) out |= unit_bit(own);
      if (op == 0x35 && byte(1) == 0) out |= unit_bit(entity_unit(byte(2)));
      if (op == 0x5B && byte(1) == 0) {
        const int word = byte(2) | byte(3) << 8;
        out |= word & 0x8000 ? unit_bit(named_unit(word & 0x7FFF, own)) : kEnemyUnits;
      }
      pc += static_cast<u32>(length);
    }
  }
  return out;
}

bool ends_on_count(const pac::Module& module) {
  for (const pac::ScriptExtent& script : module.scripts()) {
    if (script.slot != 1) continue;
    bool counts = false, wins = false;
    for (u32 pc = script.start; pc < script.end;) {
      const u8 op = module.at(pc);
      const int length = pac::op_info(op).len;
      if (length == 0 || pc + static_cast<u32>(length) > script.end) break;
      if (op == 0x17) counts = true;
      if (op == 0x83 && module.at(pc + 1) == 0x00 && module.at(pc + 2) == 0x80) wins = true;
      pc += static_cast<u32>(length);
    }
    if (counts && wins) return true;
  }
  return false;
}

u32 kept_from_party(const Battle& battle, pac::ScriptedBrain& brain) {
  if (!brain.active()) return 0;
  const pac::Vm& vm = *brain.vm();
  std::vector<int> unit_of_entity;
  for (int entity = 0; entity < vm.module().entities; ++entity) {
    const int unit = vm.unit_of_entity(entity);
    unit_of_entity.push_back(unit >= 0 && unit < kUnits ? unit : -1);
  }
  const u32 revealable = revealable_units(vm.module(), unit_of_entity);
  u32 out = 0;
  for (int unit = kPcSlots; unit < kUnits; ++unit)
    if (battle.unit(unit).present && battle.untargetable(unit) && !(revealable >> unit & 1u)) out |= 1u << unit;
  return out;
}

std::vector<SceneOpponents> scene_opponents(const data::GameTables& tables, const pac::ScriptedBrain::Loader& loader) {
  std::vector<SceneOpponents> out(tables.scenes.size());
  std::vector<PartyMember> party;
  for (const int id : {0, 1, 2}) party.push_back(make_party_member(tables, id));
  for (size_t scene = 0; scene < tables.scenes.size(); ++scene) {
    GameRng rng(1, tables.rng_table);
    pac::ScriptedBrain brain(loader);
    Battle battle(tables, rng, brain);
    if (!battle.start(party, static_cast<int>(scene))) continue;
    const u32 kept = kept_from_party(battle, brain);
    SceneOpponents& entry = out[scene];
    entry.counted_end = brain.active() && ends_on_count(brain.vm()->module());
    for (int unit = kPcSlots; unit < kUnits; ++unit) {
      if (!battle.unit(unit).present) continue;
      std::vector<int>& list = kept >> unit & 1u ? entry.kept : entry.fought;
      const int record = battle.unit(unit).src_id;
      if (std::find(list.begin(), list.end(), record) == list.end()) list.push_back(record);
    }
  }
  return out;
}

}  // namespace oc::battle
