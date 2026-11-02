#pragma once
// Which group's draw offset a field primitive runs under, header-only so the unit tests reach it. The ordering table runs from the deepest
// slot to the nearest. Every layer links its group's offset at the head of slot z, ahead of its tiles, and the global scroll at slot z - 2;
// anything filed after the layers runs first in its own slot. So a primitive takes the offset left by the nearest deeper slot holding one:
// the global scroll where that slot has one, else the group offset of the first layer linked there.
#include <climits>
#include <span>

#include "core/types.h"

namespace oc::game::draw_offset {

struct LayerDepth {
  int z = 0, group = 0;
  bool operator==(const LayerDepth&) const = default;
};

constexpr int kGlobal = -1;

// The group whose offset is in force at `slot`, or kGlobal.
constexpr int group_at(std::span<const LayerDepth> layers, int slot) {
  int nearest = INT_MAX;
  for (const LayerDepth& layer : layers)
    for (const int linked : {layer.z, layer.z - 2})
      if (linked > slot && linked < nearest) nearest = linked;
  for (const LayerDepth& layer : layers)
    if (layer.z - 2 == nearest) return kGlobal;
  for (const LayerDepth& layer : layers)
    if (layer.z == nearest) return layer.group;
  return kGlobal;
}

}  // namespace oc::game::draw_offset
