#pragma once
// The room-script scan over every room the VFS holds (script/script_scan.h does one room), and the battle tables that turn a
// battle id into its encounter groups. It reads every room, so the caller runs it once and keeps the result.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data/bestiary.h"
#include "script/field_script.h"
#include "script/script_scan.h"

namespace oc::vfs {
class Vfs;
}
namespace oc::data {
struct GameTables;
}

namespace oc::game {

struct RoomScan {
  std::string stem;
  // lang.dat room id of the stem, -1 when it has none.
  int room_id = -1;
  script::RoomScanResult scan;
  // Why the room's script could not be read; empty when it was scanned.
  std::string error;
  // Time spent on the room, reading included.
  double milliseconds = 0;
};

struct ScriptScan {
  std::vector<RoomScan> rooms;
  // Wall time of scan_rooms, and the time spent reading and decoding the room files summed over the rooms (the rooms are scanned on
  // several threads, so the sum can exceed the wall time).
  double milliseconds = 0;
  double read_milliseconds = 0;
};

// Stems of every room with a .bin and a .ctd in map/mapbin, sorted.
std::vector<std::string> room_stems(const vfs::Vfs& files);
// The room's field script as the field runs it (subfile 3, decoded, with the room's script corrections applied).
std::optional<script::FieldScriptModule> load_room_script(const vfs::Vfs& files, std::string_view stem, std::string* error = nullptr);
// Scans every room of room_stems(), on up to hardware_concurrency threads; the result does not depend on the thread count.
ScriptScan scan_rooms(const vfs::Vfs& files);

// A treasure site: a give of an item, element or key item whose flag some INIT of the room tests, so the room shows it taken when
// it loads.
bool is_treasure(const script::ScanEvent& event);
// Stable id of an event of a room file: "<stem>:<entity>:<code offset, 4 hex digits>".
std::string event_id(std::string_view stem, const script::ScanEvent& event);

// One treasure: every site that sets the same flag (the same chest in the copies of a room, or one pickup that gives two things).
// `key` is the flag as one number, stable across room edits that keep the flag: bit flags `var * 8 + bit` (var is even, so this is
// the variable's slot * 16 + bit), value flags 0x10000 + var * 0x10000 + value. `chest`: some site runs the chest routine (the 99
// capacity test). `gives`: the distinct values of every site, in site order. `sites`: (room index, event index) into the scan.
struct Treasure {
  u32 key = 0;
  script::ScanFlag flag;
  bool chest = false;
  std::vector<int> gives;
  std::vector<std::pair<size_t, size_t>> sites;
};
u32 flag_key(const script::ScanFlag& flag);
// Every treasure of the scan, ordered by key.
std::vector<Treasure> treasures(const ScriptScan& scan);

// The encounter groups (scenes) of a battle id: the distinct scenes of its eight variants in variant order; the one scene for an id
// from 0x200; empty when the id is outside the tables.
std::vector<int> battle_scenes(const data::GameTables& tables, int battle_id);

// Every battle op of the scan (rooms in scan order, each room's ops in code order) with the lowest story value its guards allow and the
// enemy records the encounter groups of its battle ids place, each once.
std::vector<data::BattleSite> battle_sites(const ScriptScan& scan, const data::GameTables& tables);
// The Bestiary page's numbers and places: data::bestiary_order over battle_sites, the records' English names, the records some encounter
// group names and the party can fight in one of them (battle::scene_opponents over the battle scripts of `files`; the others are left
// out), the records of each battle scene, and the world each story area's name is shown with (data::area_worlds over the scan's rooms).
data::BestiaryOrder bestiary_order(const ScriptScan& scan, const data::GameTables& tables, const vfs::Vfs& files);
// The stems of the scan's rooms, in scan order.
std::vector<std::string> room_stems(const ScriptScan& scan);

}  // namespace oc::game
