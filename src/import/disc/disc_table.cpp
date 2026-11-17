#include "import/disc/disc_table.h"

#include <algorithm>
#include <cctype>

namespace oc::import::disc {

namespace {

constexpr u32 kPrimaryVolumeSector = 16;
constexpr u32 kLbaMask = 0x7FFFFF;
constexpr u32 kEmptyFlag = 0x800000;

u32 get32(ByteSpan bytes, size_t at) {
  return bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | static_cast<u32>(bytes[at + 3]) << 24;
}

std::string trimmed(ByteSpan bytes) {
  std::string text(bytes.begin(), bytes.end());
  while (!text.empty() && (text.back() == ' ' || text.back() == 0)) text.pop_back();
  return text;
}

bool same_name(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](char one, char other) { return std::toupper(static_cast<unsigned char>(one)) == std::toupper(static_cast<unsigned char>(other)); });
}

u32 sectors_for(u32 size) { return (size + static_cast<u32>(kUserSectorSize) - 1) / static_cast<u32>(kUserSectorSize); }

}  // namespace

const IsoEntry* IsoVolume::find(std::string_view name) const {
  for (const IsoEntry& entry : root)
    if (same_name(entry.name, name)) return &entry;
  return nullptr;
}

std::optional<IsoVolume> read_iso_volume(const SectorSource& source, std::string* error) {
  const Bytes pvd = source.read_user(kPrimaryVolumeSector, 1);
  if (pvd.empty() || pvd[0] != 1 || std::string_view(reinterpret_cast<const char*>(&pvd[1]), 5) != "CD001") {
    if (error) *error = "no ISO9660 primary volume descriptor";
    return std::nullopt;
  }
  IsoVolume volume;
  volume.system_id = trimmed(ByteSpan(pvd).subspan(8, 32));
  volume.volume_id = trimmed(ByteSpan(pvd).subspan(40, 32));
  volume.volume_sectors = get32(pvd, 80);
  const ByteSpan root_record = ByteSpan(pvd).subspan(156, 34);
  const u32 root_lba = get32(root_record, 2);
  const u32 root_size = get32(root_record, 10);
  if (root_size == 0 || root_size > 64 * kUserSectorSize) {
    if (error) *error = "ISO9660 root directory size out of range";
    return std::nullopt;
  }
  const Bytes dir = source.read_user(root_lba, sectors_for(root_size));
  if (dir.empty()) {
    if (error) *error = "cannot read the ISO9660 root directory";
    return std::nullopt;
  }
  size_t at = 0;
  while (at < root_size) {
    const u8 length = dir[at];
    if (length == 0) {
      at = (at / kUserSectorSize + 1) * kUserSectorSize;
      continue;
    }
    if (at + length > dir.size() || length < 34) break;
    const u8 name_length = dir[at + 32];
    if (33u + name_length <= length) {
      IsoEntry entry;
      entry.lba = get32(dir, at + 2);
      entry.size = get32(dir, at + 10);
      entry.directory = (dir[at + 25] & 0x02) != 0;
      const bool special = name_length == 1 && dir[at + 33] <= 1;
      entry.name.assign(reinterpret_cast<const char*>(&dir[at + 33]), name_length);
      if (const size_t version = entry.name.find(';'); version != std::string::npos) entry.name.resize(version);
      if (!special) volume.root.push_back(std::move(entry));
    }
    at += length;
  }
  return volume;
}

std::optional<Bytes> read_iso_file(const SectorSource& source, const IsoEntry& entry) {
  Bytes bytes = source.read_user(entry.lba, sectors_for(entry.size));
  if (bytes.empty() && entry.size != 0) return std::nullopt;
  bytes.resize(entry.size);
  return bytes;
}

std::string boot_file_name(std::string_view system_cnf) {
  size_t at = 0;
  while (at < system_cnf.size()) {
    size_t end = system_cnf.find('\n', at);
    if (end == std::string_view::npos) end = system_cnf.size();
    std::string_view line = system_cnf.substr(at, end - at);
    at = end + 1;
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) continue;
    std::string_view key = line.substr(0, equals);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
    if (!same_name(key, "BOOT")) continue;
    std::string_view value = line.substr(equals + 1);
    if (const size_t colon = value.find(':'); colon != std::string_view::npos) value = value.substr(colon + 1);
    if (const size_t slash = value.find_last_of("\\/"); slash != std::string_view::npos) value = value.substr(slash + 1);
    if (const size_t version = value.find(';'); version != std::string_view::npos) value = value.substr(0, version);
    std::string name;
    for (const char ch : value)
      if (ch != ' ' && ch != '\t' && ch != '\r') name += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return name;
  }
  return {};
}

FileTable FileTable::decode(ByteSpan words, u32 end_lba) {
  FileTable table;
  std::vector<u32> raw;
  for (size_t at = 0; at + 4 <= words.size(); at += 4) {
    const u32 word = get32(words, at);
    if (word == 0) break;
    raw.push_back(word);
  }
  table.spans_.resize(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    FileSpan& span = table.spans_[i];
    span.lba = raw[i] & kLbaMask;
    span.empty = (raw[i] & kEmptyFlag) != 0;
    const u32 next = i + 1 < raw.size() ? raw[i + 1] & kLbaMask : end_lba;
    const u64 unused = static_cast<u64>(raw[i] >> 24) * 8;
    const u64 stored = next > span.lba ? static_cast<u64>(next - span.lba) * kUserSectorSize : 0;
    span.size = span.empty || stored < unused ? 0 : static_cast<u32>(stored - unused);
  }
  return table;
}

std::optional<FileTable> FileTable::read(const SectorSource& source, std::string* error) {
  const Bytes words = source.read_user(kFileTableFirstSector, kFileTableSectors);
  if (words.empty()) {
    if (error) *error = "cannot read the file table sectors";
    return std::nullopt;
  }
  FileTable table = decode(words, source.sector_count());
  if (table.size() == 0) {
    if (error) *error = "the file table is empty";
    return std::nullopt;
  }
  return table;
}

bool FileTable::plausible(u32 sector_count) const {
  for (size_t i = 0; i < spans_.size(); ++i) {
    if (spans_[i].lba > sector_count) return false;
    if (i > 0 && spans_[i].lba < spans_[i - 1].lba) return false;
  }
  return !spans_.empty();
}

std::optional<Bytes> read_record(const SectorSource& source, const FileTable& table, size_t record) {
  if (record >= table.size()) return std::nullopt;
  const FileSpan& span = table.span(record);
  if (span.size == 0) return Bytes{};
  Bytes bytes = source.read_user(span.lba, sectors_for(span.size));
  if (bytes.empty()) return std::nullopt;
  bytes.resize(span.size);
  return bytes;
}

std::optional<u32> read_disc_number(const SectorSource& source) {
  const Bytes sector = source.read_user(kDiscNumberSector, 1);
  if (sector.empty()) return std::nullopt;
  return get32(sector, 0);
}

}  // namespace oc::import::disc
