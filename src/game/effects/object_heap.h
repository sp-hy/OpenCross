#pragma once
// The effect object arena: a fixed byte range of blocks in address order, each behind a 12-byte node header. An allocation
// searches backwards (towards lower addresses) from the search point for the first free block that fits, wrapping once to the
// top, and cuts the new block from the top of that free block; a freed block merges with its free neighbours and becomes the
// search point. Effect objects are stepped and drawn in ascending address order, so this placement decides their order: on a
// heap without holes the newest object comes first.
#include <vector>

#include "core/types.h"

namespace oc::effects {

class ObjectHeap {
 public:
  static constexpr u32 kNodeBytes = 0xC;

  explicit ObjectHeap(u32 bytes);

  // Node offset of a new block of `size` payload bytes (rounded up to 4), or -1 when no free block fits.
  s64 alloc(u32 size);
  void free(u32 node);
  // The next search starts at the highest block, so the next object lands at the top of the arena.
  void search_from_top();
  void reset();

 private:
  struct Block {
    u32 node = 0;
    bool used = false;
  };
  size_t containing(u32 offset) const;

  u32 bytes_ = 0;
  // Ascending node offsets; the last block is the end sentinel (always used, no payload).
  std::vector<Block> blocks_;
  // 0 = none: the search starts at the first block.
  u32 search_ = 0;
};

}  // namespace oc::effects
