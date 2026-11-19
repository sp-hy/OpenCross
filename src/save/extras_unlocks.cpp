#include "save/extras_unlocks.h"

namespace oc::save {

u32 unlock_chunk(Unlock kind) {
  switch (kind) {
    case Unlock::Song: return kSongChunk;
    case Unlock::Movie: return kMovieChunk;
    case Unlock::Face: return kFaceChunk;
  }
  return kSongChunk;
}

bool unlocked(const Collection& collection, Unlock kind, int index) {
  const Bytes* bits = collection.chunk(unlock_chunk(kind));
  if (!bits || index < 0 || static_cast<size_t>(index >> 3) >= bits->size()) return false;
  return ((*bits)[static_cast<size_t>(index >> 3)] >> (index & 7) & 1) != 0;
}

bool unlock(Collection& collection, Unlock kind, int index) {
  if (index < 0 || index >= kUnlockBits || unlocked(collection, kind, index)) return false;
  const u32 tag = unlock_chunk(kind);
  const Bytes* old = collection.chunk(tag);
  Bytes bits = old ? *old : Bytes{};
  if (static_cast<size_t>(index >> 3) >= bits.size()) bits.resize(static_cast<size_t>(index >> 3) + 1, 0);
  bits[static_cast<size_t>(index >> 3)] |= static_cast<u8>(1u << (index & 7));
  collection.set_chunk(tag, std::move(bits));
  return true;
}

}  // namespace oc::save
