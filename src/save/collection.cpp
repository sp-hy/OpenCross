#include "save/collection.h"

#include <miniz.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#include "core/log.h"

namespace oc::save {

namespace {

constexpr char kMagic[4] = {'O', 'C', 'C', 'L'};
constexpr size_t kHeaderSize = 12, kChunkHeaderSize = 8, kCrcSize = 4;

u32 rd32(ByteSpan bytes, size_t offset) {
  return static_cast<u32>(bytes[offset]) | static_cast<u32>(bytes[offset + 1]) << 8 | static_cast<u32>(bytes[offset + 2]) << 16 |
         static_cast<u32>(bytes[offset + 3]) << 24;
}

void put32(Bytes& bytes, u32 value) {
  for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(value >> (8 * i)));
}

u32 crc(ByteSpan bytes) { return static_cast<u32>(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size())); }

}  // namespace

u8 Collection::enemy_flags(int monster_id) const {
  const Bytes* enemies = chunk(kEnemyChunk);
  if (!enemies || monster_id < 0 || static_cast<size_t>(monster_id) >= enemies->size()) return 0;
  return (*enemies)[static_cast<size_t>(monster_id)];
}

bool Collection::mark_enemy(int monster_id, u8 flags) {
  if (monster_id < 0 || monster_id >= kMaxEnemies || flags == 0) return false;
  Bytes& enemies = chunks_[kEnemyChunk];
  if (static_cast<size_t>(monster_id) >= enemies.size()) enemies.resize(static_cast<size_t>(monster_id) + 1, 0);
  u8& byte = enemies[static_cast<size_t>(monster_id)];
  if ((byte | flags) == byte) return false;
  byte |= flags;
  return true;
}

u8 Collection::had_flags(u32 chunk_tag, int id) const {
  const Bytes* bytes = chunk(chunk_tag);
  if (!bytes || id < 0 || static_cast<size_t>(id) >= bytes->size()) return 0;
  return (*bytes)[static_cast<size_t>(id)];
}

bool Collection::mark_had(u32 chunk_tag, int id, u8 flags) {
  if (id < 0 || id >= kMaxHadId || flags == 0) return false;
  Bytes& bytes = chunks_[chunk_tag];
  if (static_cast<size_t>(id) >= bytes.size()) bytes.resize(static_cast<size_t>(id) + 1, 0);
  u8& byte = bytes[static_cast<size_t>(id)];
  if ((byte | flags) == byte) return false;
  byte |= flags;
  return true;
}

bool Collection::record_slot(const SaveSlot& slot) {
  constexpr int kFirstTech = 0x80, kKeyItems = 64;
  bool changed = false;
  for (int character = 0; character < kNumChars; ++character) {
    if (!slot.recruited(character)) continue;
    changed = mark_had(kCharacterChunk, character) || changed;
    for (const u16 cell : slot.chars[static_cast<size_t>(character)].grid)
      if (const int id = cell & 0x1FF; id > 0 && id < kFirstTech) changed = mark_had(kElementChunk, id) || changed;
  }
  for (const ItemStack& stack : slot.elements)
    if (stack.id > 0 && stack.id < kFirstTech && stack.qty > 0) changed = mark_had(kElementChunk, stack.id) || changed;
  const u64 keys = slot.key_items();
  for (int id = 1; id <= kKeyItems; ++id)
    if ((keys >> (id - 1)) & 1) changed = mark_had(kKeyItemChunk, id) || changed;
  return changed;
}

const Bytes* Collection::chunk(u32 chunk_tag) const {
  const auto it = chunks_.find(chunk_tag);
  return it == chunks_.end() ? nullptr : &it->second;
}

Bytes Collection::serialize() const {
  Bytes out(kMagic, kMagic + 4);
  put32(out, kVersion);
  put32(out, static_cast<u32>(chunks_.size()));
  for (const auto& [chunk_tag, payload] : chunks_) {
    put32(out, chunk_tag);
    put32(out, static_cast<u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
  }
  put32(out, crc(out));
  return out;
}

Collection Collection::parse(ByteSpan bytes) {
  if (bytes.size() < kHeaderSize + kCrcSize) throw std::runtime_error("too short");
  if (std::memcmp(bytes.data(), kMagic, 4) != 0) throw std::runtime_error("not a collection file");
  const u32 version = rd32(bytes, 4);
  if (version == 0 || version > kVersion) throw std::runtime_error("version " + std::to_string(version) + " is not supported");
  const size_t body = bytes.size() - kCrcSize;
  if (crc(bytes.first(body)) != rd32(bytes, body)) throw std::runtime_error("checksum mismatch");
  const u32 count = rd32(bytes, 8);
  Collection out;
  size_t at = kHeaderSize;
  for (u32 i = 0; i < count; ++i) {
    if (body - at < kChunkHeaderSize) throw std::runtime_error("chunk table runs past the end");
    const u32 chunk_tag = rd32(bytes, at);
    const u32 size = rd32(bytes, at + 4);
    at += kChunkHeaderSize;
    if (body - at < size) throw std::runtime_error("chunk runs past the end");
    if (out.chunks_.contains(chunk_tag)) throw std::runtime_error("repeated chunk");
    out.chunks_[chunk_tag] = Bytes(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
    at += size;
  }
  if (at != body) throw std::runtime_error("trailing bytes");
  return out;
}

bool Collection::load() {
  chunks_.clear();
  std::error_code error;
  if (path_.empty() || !std::filesystem::exists(path_, error)) return true;
  try {
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open");
    const Bytes bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    chunks_ = parse(bytes).chunks_;
    return true;
  } catch (const std::exception& failure) {
    std::filesystem::path bad = path_;
    bad += ".bad";
    std::filesystem::remove(bad, error);
    std::filesystem::rename(path_, bad, error);
    log::warn("collection {}: {}; kept as {}, starting empty", path_.string(), failure.what(), bad.string());
    chunks_.clear();
    return false;
  }
}

bool Collection::save() const {
  if (path_.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  std::filesystem::path temp = path_;
  temp += ".tmp";
  {
    const Bytes bytes = serialize();
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      log::warn("collection: cannot write {}", temp.string());
      return false;
    }
  }
  std::filesystem::rename(temp, path_, error);
  if (error) {
    log::warn("collection: cannot replace {}: {}", path_.string(), error.message());
    return false;
  }
  return true;
}

}  // namespace oc::save
