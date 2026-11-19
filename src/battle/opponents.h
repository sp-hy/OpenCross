#pragma once
// Which enemy units of a battle the party can fight at all. A unit is kept from the party when it is untargetable once the battle is set up
// (the scene's off-stage mask, then every entity's init script) and no op of the scene's battle script can make it targetable again: a story
// participant or guest that stands in an enemy slot (Nikki beside the Cassowaries, the Malebranche beside the Loch Nests).
#include <vector>

#include "battle/battle.h"
#include "battle/pac_vm.h"
#include "core/types.h"

namespace oc::data {
struct GameTables;
}

namespace oc::battle {

// The units (bit u) an op of `module` can make targetable: 34 0 (the entity's own unit), 35 0 e (entity e's unit), 5B 0 u (unit u, E9 the
// entity's own); a 5B whose unit is read from a variable, or a 34 / 35 / 5B E9 of an entity no unit is bound to, may name any enemy unit.
// `unit_of_entity` gives the unit an entity is bound to (-1 none).
u32 revealable_units(const pac::Module& module, const std::vector<int>& unit_of_entity);

// The enemy units (bit u) of a battle `brain` has just set up (Battle::start) that the party can never target: present, untargetable and
// not in revealable_units. Zero when the scene has no battle script.
u32 kept_from_party(const Battle& battle, pac::ScriptedBrain& brain);

// Whether the battle ends itself on a count: an idle script (slot 1) that counts with op 17 and forces a victory with op 83 (literal 0), so
// the fight is over after so many turns whatever the enemies' HP (the Fort Dragonia dummies).
bool ends_on_count(const pac::Module& module);

// Per scene of the tables, the monster records of its enemy units: those the party can target (`fought`, each record once) and those kept
// from it (`kept`), set up as a field battle with the first three characters of the stock party, and whether the scene ends on a count
// (`ends_on_count`). A scene that does not start lists none.
struct SceneOpponents {
  std::vector<int> fought, kept;
  bool counted_end = false;
};
std::vector<SceneOpponents> scene_opponents(const data::GameTables& tables, const pac::ScriptedBrain::Loader& loader);

}  // namespace oc::battle
