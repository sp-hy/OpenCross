#pragma once
// Rooms that mods add, and the room ids each mod's scripts and text use for them.
//
// A mod names its rooms in its mod.ini ([Rooms] <id>=<stem>, features::Manifest::rooms). The ids are the mod's own: two mods built
// apart may both call their first room 1001, so an id only means something together with the mod whose script or text uses it. The
// stem (the room's file name) is the identity: two mods cannot ship the same room file without one replacing the other.
//
// Inside the engine every mod room has an internal id from kInternalFirst on (one per stem, never saved, never shown to a script). A
// script id is turned into an internal id where the engine takes one from a script (map jumps, model sources), and an internal id into
// the script's own id where a script reads one (MAP_ID / PREVIOUS_MAP_ID): in the namespace of the mod that owns the running room, the
// mod whose .bin of that room is loaded. A game room nobody edits is the game's own: ids below 1000 only.
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oc::game {

class ModRooms {
 public:
  // Internal ids of mod rooms start here: above any id a script can write (0x7FFF), so the two never meet.
  static constexpr int kInternalFirst = 0x10000;
  // What a script reads for a mod room its owner's [Rooms] does not list (MAP_ID / PREVIOUS_MAP_ID): no room any script can name.
  static constexpr int kUnlisted = 0x7FFF;

  struct Mod {
    std::string name;
    // [Rooms]: the mod's id -> stem (lower case).
    std::map<int, std::string> rooms;
  };

  // `mods` = the enabled mods with a [Rooms] section, in priority order. `owners` = room stem -> the name of the mod whose
  // cdrom/map/mapbin/<stem>.bin is loaded (game rooms a mod edits included); a room missing there is the game's own.
  void set(std::vector<Mod> mods, std::map<std::string, std::string> owners);

  bool empty() const { return stems_.empty(); }
  static bool internal(int id) { return id >= kInternalFirst; }
  // A mod room's internal id, -1 when no mod lists the stem.
  int internal_id(std::string_view stem) const;
  // The stem of an internal id, "" for any other id.
  std::string stem(int internal_id) const;
  // Every stem some mod lists, in internal id order.
  const std::vector<std::string>& stems() const { return stems_; }
  // The mod that owns a room (its .bin), "" = the game.
  std::string owner(std::string_view stem) const;

  // A room id a script of the room `running_stem` gave -> the engine's id: below 1000 as it is (a game room); from 1000 the room the
  // owner's [Rooms] lists under that id (internal id), -1 when it lists none.
  int from_script(std::string_view running_stem, int id) const;
  // The engine's id -> the id a script of the room `running_stem` reads: a game room as it is; a mod room by the id its owner's [Rooms]
  // gives that stem, kUnlisted when it gives none.
  int to_script(std::string_view running_stem, int id) const;
  // A mod's id -> internal id (its text rows, a save's room): -1 when the mod does not list it.
  int from_mod(std::string_view mod, int id) const;
  // The stem the first mod in the list gives that id ("" = none): only for a card save, which keeps no room name.
  std::string first_listing(int id) const;
  // The [Rooms] of a mod, null when it has none.
  const std::map<int, std::string>* rooms_of(std::string_view mod) const;

 private:
  std::vector<Mod> mods_;
  std::map<std::string, std::string> owners_;
  std::vector<std::string> stems_;
  std::map<std::string, int, std::less<>> internal_;
};

}  // namespace oc::game
