#pragma once
// Saved variables that mods add, and the handles each mod's scripts use for them.
//
// A mod names its variables in its mod.ini ([Variables] <handle>=<name>, features::Manifest::variables). The handles are the mod's own
// (features::kModVarFirst..kModVarLast): a field script's handle in that range means the variable the [Variables] of the mod that owns
// the running room (ModRooms::owner) gives it, and nothing in a room no such mod owns. The name is the identity: two mods that name the
// same variable share it, and a save keeps the values by name, so a mod may renumber its handles and adding or removing a mod moves no
// value.
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace oc::game {

class ModVars {
 public:
  struct Mod {
    std::string name;
    // [Variables]: the mod's handle -> variable name.
    std::map<int, std::string> handles;
  };

  // `mods` = the enabled mods with a [Variables] section, in priority order.
  void set(std::vector<Mod> mods);

  bool empty() const { return names_.empty(); }
  // Every variable some mod names, in name order; a variable's index is its place here.
  const std::vector<std::string>& names() const { return names_; }
  // A mod's handles -> variable index, null when the mod names no variable.
  const std::map<int, int>* handles_of(std::string_view mod) const;

 private:
  std::vector<std::string> names_;
  std::map<std::string, std::map<int, int>, std::less<>> handles_;
};

}  // namespace oc::game
