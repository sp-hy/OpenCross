#pragma once
// The record pages' knowledge (ui/menu/records_info.h) built from the room-script scan: where each character joins, where elements and
// key items are given, and the chests and pickups by area.
//
// Only rooms a game can reach count: the rooms the room changes of the scan lead to from the first room of a new game, without passing
// through the title scene (whose attract demo rooms rebuild a party of their own). A recruitment that needs the character carried over
// from a cleared game (New Game+) is left out; the pages never call a character missed in such a game.
#include <vector>

#include "game/room_scan.h"
#include "ui/menu/records_info.h"

namespace oc::data {
struct GameTables;
}

namespace oc::game {

// The rooms reachable as above, as indices into scan.rooms.
std::vector<bool> reachable_rooms(const ScriptScan& scan);
// Every drop of the story counter the scan cannot rule out: the story sets a game may run while the counter is above the value they
// write, with that counter value, sorted. A game is followed from the first room of a new game (entrance 0, story 0) through the
// rooms' exits and story sets, a state being the room, its entrance, the counter when it was entered and the counter now. An exit or
// set is taken where no test on its path is known to fail and its entity is active. Known are: the story counter; the entrance, in a
// room no script of which writes ROOM_PARAM; a global variable no script and not the engine writes (0); and an entity's INIT making
// it inactive, decided by the counter on entry, for an entity no script makes active again and that is not a party slot. Every other
// test may pass, so the drops found are a superset of the drops a game can make.
std::vector<ui::StoryDrop> story_drops(const ScriptScan& scan);
// `tables` gives the element shops and the enemies' drop rows; the areas are data::area_name of the room stems.
ui::RecordsInfo records_info(const ScriptScan& scan, const data::GameTables& tables);

}  // namespace oc::game
