#include "game/mod_vars.h"

#include <algorithm>

namespace oc::game {

void ModVars::set(std::vector<Mod> mods) {
  names_.clear();
  handles_.clear();
  // One index per name, in name order (the same mods give the same indexes, whatever their order).
  for (const Mod& mod : mods)
    for (const auto& [handle, name] : mod.handles) names_.push_back(name);
  std::sort(names_.begin(), names_.end());
  names_.erase(std::unique(names_.begin(), names_.end()), names_.end());
  std::map<std::string, int, std::less<>> index;
  for (size_t i = 0; i < names_.size(); ++i) index.emplace(names_[i], static_cast<int>(i));
  for (const Mod& mod : mods) {
    std::map<int, int>& handles = handles_[mod.name];
    for (const auto& [handle, name] : mod.handles) handles.emplace(handle, index.find(name)->second);
  }
}

const std::map<int, int>* ModVars::handles_of(std::string_view mod) const {
  const auto it = handles_.find(mod);
  return it == handles_.end() ? nullptr : &it->second;
}

}  // namespace oc::game
