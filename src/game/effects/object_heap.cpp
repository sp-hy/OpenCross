#include "game/effects/object_heap.h"

#include <algorithm>

namespace oc::effects {

ObjectHeap::ObjectHeap(u32 bytes) : bytes_(bytes) { reset(); }

void ObjectHeap::reset() {
  blocks_.clear();
  blocks_.push_back({4, false});
  blocks_.push_back({bytes_ - kNodeBytes, true});
  search_ = 0;
}

size_t ObjectHeap::containing(u32 offset) const {
  const auto it = std::upper_bound(blocks_.begin(), blocks_.end(), offset, [](u32 value, const Block& block) { return value < block.node; });
  return it == blocks_.begin() ? 0 : static_cast<size_t>(it - blocks_.begin()) - 1;
}

void ObjectHeap::search_from_top() { search_ = blocks_[blocks_.size() - 2].node; }

s64 ObjectHeap::alloc(u32 size) {
  size = (size + 3u) & ~3u;
  const size_t top = blocks_.size() - 2;
  const size_t start = search_ ? containing(search_) : 0;
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = pass == 0 ? start + 1 : top + 1; i-- > 0;) {
      const Block block = blocks_[i];
      const u32 end = blocks_[i + 1].node;
      if (block.used || end - (block.node + kNodeBytes) < size) continue;
      const u32 node = end - (size + kNodeBytes);
      if (node - block.node < kNodeBytes) {
        blocks_[i].used = true;
        search_ = 0;
        return block.node;
      }
      blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(i) + 1, Block{node, true});
      search_ = block.node;
      return node;
    }
  return -1;
}

void ObjectHeap::free(u32 node) {
  size_t i = containing(node);
  if (blocks_[i].node != node || !blocks_[i].used) return;
  search_ = node;
  blocks_[i].used = false;
  if (i > 0 && !blocks_[i - 1].used) {
    blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(i));
    --i;
  }
  if (i + 2 < blocks_.size() && !blocks_[i + 1].used) blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
}

}  // namespace oc::effects
