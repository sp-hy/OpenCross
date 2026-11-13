#include "game/mod_rooms.h"

#include <algorithm>

#include "core/features.h"

namespace oc::game {

void ModRooms::set(std::vector<Mod> mods, std::map<std::string, std::string> owners) {
  mods_ = std::move(mods);
  owners_ = std::move(owners);
  stems_.clear();
  internal_.clear();
  // One internal id per stem, in stem order (the same mods give the same ids, whatever their order).
  std::vector<std::string> all;
  for (const Mod& mod : mods_)
    for (const auto& [id, stem] : mod.rooms) all.push_back(stem);
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  for (std::string& stem : all) {
    internal_.emplace(stem, kInternalFirst + static_cast<int>(stems_.size()));
    stems_.push_back(std::move(stem));
  }
}

int ModRooms::internal_id(std::string_view stem) const {
  const auto it = internal_.find(stem);
  return it == internal_.end() ? -1 : it->second;
}

std::string ModRooms::stem(int id) const {
  if (!internal(id) || id - kInternalFirst >= static_cast<int>(stems_.size())) return {};
  return stems_[static_cast<size_t>(id - kInternalFirst)];
}

std::string ModRooms::owner(std::string_view stem) const {
  const auto it = owners_.find(std::string(stem));
  return it == owners_.end() ? std::string() : it->second;
}

const std::map<int, std::string>* ModRooms::rooms_of(std::string_view mod) const {
  for (const Mod& entry : mods_)
    if (entry.name == mod) return &entry.rooms;
  return nullptr;
}

std::string ModRooms::first_listing(int id) const {
  for (const Mod& mod : mods_)
    if (const auto it = mod.rooms.find(id); it != mod.rooms.end()) return it->second;
  return {};
}

int ModRooms::from_mod(std::string_view mod, int id) const {
  const std::map<int, std::string>* rooms = rooms_of(mod);
  if (!rooms) return -1;
  const auto it = rooms->find(id);
  return it == rooms->end() ? -1 : internal_id(it->second);
}

int ModRooms::from_script(std::string_view running_stem, int id) const {
  if (id < features::kModRoomFirst) return id;
  return from_mod(owner(running_stem), id);
}

int ModRooms::to_script(std::string_view running_stem, int id) const {
  if (!internal(id)) return id;
  const std::string target = stem(id);
  if (const std::map<int, std::string>* rooms = rooms_of(owner(running_stem)))
    for (const auto& [script_id, listed] : *rooms)
      if (listed == target) return script_id;
  return kUnlisted;
}

}  // namespace oc::game
