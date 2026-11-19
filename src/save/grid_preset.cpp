#include "save/grid_preset.h"

#include <stdexcept>

#include "core/log.h"

namespace oc::save {

namespace {

constexpr int kMaxCharacter = 48, kMaxRows = 8, kMaxCols = 16;

}  // namespace

GridPresets GridPresets::read(const Collection& collection) {
  const Bytes* payload = collection.chunk(kChunk);
  if (!payload) return {};
  try {
    return parse(*payload);
  } catch (const std::exception& failure) {
    log::warn("grid presets: {}; starting empty", failure.what());
    return {};
  }
}

GridPresets GridPresets::parse(ByteSpan payload) {
  size_t at = 0;
  auto next = [&]() -> u8 {
    if (at >= payload.size()) throw std::runtime_error("short grid preset chunk");
    return payload[at++];
  };
  auto next16 = [&] {
    const int low = next();
    return low | next() << 8;
  };
  if (next() != kVersion) throw std::runtime_error("unknown grid preset version");
  GridPresets out;
  const int count = next16();
  for (int i = 0; i < count; ++i) {
    GridPreset preset;
    preset.character = next();
    preset.slot = next();
    if (preset.character >= kMaxCharacter || preset.slot >= kSlots) throw std::runtime_error("grid preset out of range");
    if (out.find(preset.character, preset.slot)) throw std::runtime_error("repeated grid preset");
    const int cells = next();
    for (int cell = 0; cell < cells; ++cell) {
      GridPresetCell entry;
      entry.row = next();
      entry.col = next();
      entry.id = static_cast<u16>(next16());
      entry.count = next();
      if (entry.row >= kMaxRows || entry.col >= kMaxCols) throw std::runtime_error("grid preset cell out of range");
      preset.cells.push_back(entry);
    }
    out.presets_.push_back(std::move(preset));
  }
  return out;
}

Bytes GridPresets::serialize() const {
  Bytes out{kVersion, static_cast<u8>(presets_.size()), static_cast<u8>(presets_.size() >> 8)};
  for (const GridPreset& preset : presets_) {
    out.push_back(preset.character);
    out.push_back(preset.slot);
    out.push_back(static_cast<u8>(preset.cells.size()));
    for (const GridPresetCell& cell : preset.cells) {
      out.push_back(cell.row);
      out.push_back(cell.col);
      out.push_back(static_cast<u8>(cell.id));
      out.push_back(static_cast<u8>(cell.id >> 8));
      out.push_back(cell.count);
    }
  }
  return out;
}

const GridPreset* GridPresets::find(int character, int slot) const {
  for (const GridPreset& preset : presets_)
    if (preset.character == character && preset.slot == slot) return &preset;
  return nullptr;
}

void GridPresets::set(GridPreset preset) {
  std::erase_if(presets_, [&](const GridPreset& held) { return held.character == preset.character && held.slot == preset.slot; });
  if (!preset.cells.empty()) presets_.push_back(std::move(preset));
}

}  // namespace oc::save
