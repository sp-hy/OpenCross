#pragma once
// The Extras pages' unlocks ([Game] OpenCrossMenu) in the collection file: three bit sets, one chunk each. Bit i of a chunk is bit (i & 7)
// of payload byte i >> 3; the payload is as long as its highest set bit needs.
//   SONG  song ids (the song table's numbers, the same on both music sources)
//   MOVI  movie keys: (disc - 1) * 32 + the movie number of the disc's index (movie_key)
//   FACE  dialogue face ids (the 2021 and 1999 portraits of a face unlock together)
#include "save/collection.h"

namespace oc::save {

enum class Unlock { Song, Movie, Face };

constexpr u32 kSongChunk = collection_tag("SONG"), kMovieChunk = collection_tag("MOVI"), kFaceChunk = collection_tag("FACE");
// Bits a chunk can hold.
constexpr int kUnlockBits = 256;

constexpr int movie_key(int disc, int number) { return (disc >= 2 ? 32 : 0) + number; }

u32 unlock_chunk(Unlock kind);
bool unlocked(const Collection& collection, Unlock kind, int index);
// Sets the bit; true when that changed it. An index outside 0..kUnlockBits - 1 changes nothing.
bool unlock(Collection& collection, Unlock kind, int index);

}  // namespace oc::save
