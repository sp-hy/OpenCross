#pragma once
// The OpenCross collection file: what the player has met, beaten and received across every playthrough and every save, kept in one
// file of the OpenCross save folder (never in the game's saves). Chunked: "OCCL", u32 version, u32 chunk count, then per chunk a
// four-byte tag, a u32 size and the payload, then a CRC-32 of everything before it. Each feature owns its chunk; a chunk this build
// does not know is kept as it was and written back.
//
//   Collection c;  c.set_path(dir / Collection::kFileName);  c.load();
//   if (c.mark_enemy(id, Collection::kEnemyMet)) c.save();
#include <filesystem>
#include <map>

#include "core/types.h"
#include "save/cc_save.h"

namespace oc::save {

// A chunk tag: the little-endian word of its four ASCII bytes.
constexpr u32 collection_tag(const char (&name)[5]) {
  return static_cast<u32>(static_cast<u8>(name[0])) | static_cast<u32>(static_cast<u8>(name[1])) << 8 |
         static_cast<u32>(static_cast<u8>(name[2])) << 16 | static_cast<u32>(static_cast<u8>(name[3])) << 24;
}

class Collection {
 public:
  static constexpr const char* kFileName = "collection.dat";
  static constexpr u32 kVersion = 1;
  // The enemy chunk: one byte of flags per monster.bin record id.
  static constexpr u32 kEnemyChunk = collection_tag("ENMY");
  enum EnemyFlag : u8 { kEnemyMet = 0x01, kEnemyDefeated = 0x02, kEnemyLoot0 = 0x04 };
  static constexpr int kMaxEnemies = 0x400;

  void set_path(std::filesystem::path path) { path_ = std::move(path); }
  const std::filesystem::path& path() const { return path_; }
  // Reads the file (missing = empty). A file that fails to parse is renamed to "<name>.bad" with a warning and the collection starts
  // empty. False when the file was bad.
  bool load();
  // Writes "<name>.tmp", then renames it over the file; creates the folder. False (with a warning) on an I/O error or without a path.
  bool save() const;
  // Forgets everything, the chunks of other builds included.
  void clear() { chunks_.clear(); }
  bool empty() const { return chunks_.empty(); }

  // Loot slot 0..3: the ditem row's kill drop common / rare, steal common / rare.
  static constexpr u8 loot_flag(int slot) { return static_cast<u8>(kEnemyLoot0 << slot); }
  u8 enemy_flags(int monster_id) const;
  bool enemy_met(int monster_id) const { return (enemy_flags(monster_id) & kEnemyMet) != 0; }
  bool enemy_defeated(int monster_id) const { return (enemy_flags(monster_id) & kEnemyDefeated) != 0; }
  bool enemy_loot(int monster_id, int slot) const { return slot >= 0 && slot < 4 && (enemy_flags(monster_id) & loot_flag(slot)) != 0; }
  // ORs `flags` into the record's byte; true when that changed it.
  bool mark_enemy(int monster_id, u8 flags);

  // The "ever had" chunks: one byte per id, bit kHad once some save of the player had it. CHAR: the character ids recruited (0..43);
  // ELEM: the element ids held in the stock or in a recruited character's grid (1..0x7F); KEYI: the key item ids held (1..64).
  static constexpr u32 kCharacterChunk = collection_tag("CHAR");
  static constexpr u32 kElementChunk = collection_tag("ELEM");
  static constexpr u32 kKeyItemChunk = collection_tag("KEYI");
  static constexpr u8 kHad = 0x01;
  static constexpr int kMaxHadId = 0x100;
  u8 had_flags(u32 chunk_tag, int id) const;
  bool had(u32 chunk_tag, int id) const { return (had_flags(chunk_tag, id) & kHad) != 0; }
  // ORs `flags` into the id's byte of the chunk; true when that changed it.
  bool mark_had(u32 chunk_tag, int id, u8 flags = kHad);
  // Marks what the slot holds: its recruited characters, every element in its stock or in a recruited character's grid, its key
  // items. True when anything was new.
  bool record_slot(const SaveSlot& slot);

  // A feature's chunk; null when absent.
  const Bytes* chunk(u32 chunk_tag) const;
  void set_chunk(u32 chunk_tag, Bytes payload) { chunks_[chunk_tag] = std::move(payload); }

  Bytes serialize() const;
  // Throws on a short file, a wrong magic, a version above kVersion, a chunk running past the end, a repeated tag or a bad checksum.
  static Collection parse(ByteSpan bytes);

 private:
  std::filesystem::path path_;
  std::map<u32, Bytes> chunks_;
};

}  // namespace oc::save
