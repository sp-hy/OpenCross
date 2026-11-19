#pragma once
// Element grid presets: the cell layouts a player saved per character, kept in the GRID chunk of the OpenCross collection file (never
// in the game's saves). A preset names element ids, so it applies to any save.
//
//   GridPresets presets = GridPresets::read(collection);
//   presets.set({character, slot, cells});  presets.write(collection);  collection.save();
#include <vector>

#include "core/types.h"
#include "save/collection.h"

namespace oc::save {

// One filled cell: grid row (level - 1), column, element id, copies held (a stack cell; 1 for any other).
struct GridPresetCell {
  u8 row = 0, col = 0;
  u16 id = 0;
  u8 count = 1;
};

struct GridPreset {
  u8 character = 0, slot = 0;
  std::vector<GridPresetCell> cells;
};

class GridPresets {
 public:
  static constexpr u32 kChunk = collection_tag("GRID");
  static constexpr int kSlots = 3;
  static constexpr u8 kVersion = 1;

  // The chunk's presets; none when it is absent or damaged (a damaged chunk is logged and replaced by the next write).
  static GridPresets read(const Collection& collection);
  // Throws on a short payload, an unknown version, a character or slot out of range or a repeated (character, slot).
  static GridPresets parse(ByteSpan payload);
  Bytes serialize() const;
  void write(Collection& collection) const { collection.set_chunk(kChunk, serialize()); }

  const GridPreset* find(int character, int slot) const;
  // Replaces the preset of the same character and slot; an empty preset removes it.
  void set(GridPreset preset);
  const std::vector<GridPreset>& all() const { return presets_; }

 private:
  std::vector<GridPreset> presets_;
};

}  // namespace oc::save
