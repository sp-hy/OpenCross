#include "save/oc_saves.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "core/log.h"

namespace oc::save {

namespace {

constexpr char kMagic[4] = {'O', 'C', 'S', 'V'};
constexpr u32 kVersion = 1;
constexpr size_t kNamesOffset = 16;
constexpr size_t kBlockOffset = kNamesOffset + kExtNames * kExtNameLen;
constexpr size_t kFileSize = kBlockOffset + kBlockSize;

u32 rd32(ByteSpan bytes, size_t offset) {
  return static_cast<u32>(bytes[offset]) | static_cast<u32>(bytes[offset + 1]) << 8 | static_cast<u32>(bytes[offset + 2]) << 16 |
         static_cast<u32>(bytes[offset + 3]) << 24;
}

void wr32(Bytes& bytes, size_t offset, u32 value) {
  for (int i = 0; i < 4; ++i) bytes[offset + static_cast<size_t>(i)] = static_cast<u8>(value >> (8 * i));
}

}  // namespace

std::filesystem::path OcSaves::file_name(int number) {
  if (number == 0) return "auto.ocsav";
  char name[16];
  std::snprintf(name, sizeof name, "%03d.ocsav", number);
  return name;
}

Bytes OcSaves::serialize(const SaveSlot& state, const ExtSlot& ext) {
  Bytes out(kFileSize, 0);
  std::memcpy(out.data(), kMagic, 4);
  wr32(out, 4, kVersion);
  wr32(out, 8, ext.language);
  wr32(out, 12, ext.battles);
  for (const auto& [id, name] : ext.names) {
    if (id < 0 || id >= kExtNames) continue;
    // At most 31 bytes, cut at a code point.
    std::string encoded;
    for (char32_t cp : name) {
      const std::string one = utf32_to_utf8(std::u32string(1, cp));
      if (encoded.size() + one.size() > kExtNameLen - 1) break;
      encoded += one;
    }
    std::memcpy(&out[kNamesOffset + static_cast<size_t>(id) * kExtNameLen], encoded.data(), encoded.size());
  }
  const Bytes block = encode_block(state);
  std::copy(block.begin(), block.end(), out.begin() + static_cast<std::ptrdiff_t>(kBlockOffset));
  return out;
}

OcSaves::File OcSaves::parse(ByteSpan bytes) {
  if (bytes.size() != kFileSize || std::memcmp(bytes.data(), kMagic, 4) != 0) throw std::runtime_error("not an OpenCross save");
  if (rd32(bytes, 4) != kVersion) throw std::runtime_error("unknown OpenCross save version");
  const ByteSpan block = bytes.subspan(kBlockOffset, kBlockSize);
  if (!block_checksum_ok(block)) throw std::runtime_error("checksum mismatch");
  File file;
  file.ext.language = rd32(bytes, 8);
  file.ext.battles = rd32(bytes, 12);
  for (int id = 0; id < kExtNames; ++id) {
    const char* raw = reinterpret_cast<const char*>(&bytes[kNamesOffset + static_cast<size_t>(id) * kExtNameLen]);
    size_t length = 0;
    while (length < kExtNameLen && raw[length]) ++length;
    if (length) file.ext.names[id] = utf8_to_utf32(std::string(raw, length));
  }
  file.data = decode_block(block);
  return file;
}

void OcSaves::load() {
  files_.clear();
  std::error_code ec;
  if (dir_.empty() || !std::filesystem::is_directory(dir_, ec)) return;
  for (int number = 0; number <= kOcFiles; ++number) {
    const std::filesystem::path path = dir_ / file_name(number);
    if (!std::filesystem::exists(path, ec)) continue;
    try {
      std::ifstream stream(path, std::ios::binary);
      const Bytes bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
      File file = parse(bytes);
      file.written = std::filesystem::last_write_time(path, ec);
      files_[number] = std::move(file);
    } catch (const std::exception& error) {
      log::warn("OpenCross save {}: {}", path.string(), error.what());
    }
  }
}

const OcSaves::File* OcSaves::file(int number) const {
  const auto it = files_.find(number);
  return it == files_.end() ? nullptr : &it->second;
}

int OcSaves::newest() const {
  int best = 0;
  for (const auto& [number, file] : files_)
    if (number > 0 && (best == 0 || file.written > files_.at(best).written)) best = number;
  return best;
}

void OcSaves::write(int number, const SaveSlot& state) {
  if (number < 0 || number > kOcFiles || dir_.empty()) throw std::runtime_error("OpenCross save: bad file number");
  File file;
  file.data = state;
  if (ext_host_) {
    file.ext.language = ext_host_->language;
    file.ext.battles = ext_host_->battles;
    if (ext_host_->names) file.ext.names = *ext_host_->names;
  }
  const Bytes bytes = serialize(state, file.ext);
  std::filesystem::create_directories(dir_);
  const std::filesystem::path path = dir_ / file_name(number);
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("OpenCross save: cannot write " + tmp.string());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("OpenCross save: write failed " + tmp.string());
  }
  std::filesystem::rename(tmp, path);
  std::error_code ec;
  file.written = std::filesystem::last_write_time(path, ec);
  files_[number] = std::move(file);
}

ExtSlot OcSaves::peek_ext(int number, u32 language) const {
  ExtSlot record;
  if (const File* found = file(number)) record = found->ext;
  if (record.language != language) record.names.clear();
  return record;
}

ExtSlot OcSaves::read_ext(int number, u32 language) const {
  ExtSlot record = peek_ext(number, language);
  if (ext_host_) ext_host_->battles = record.battles;
  return record;
}

}  // namespace oc::save
