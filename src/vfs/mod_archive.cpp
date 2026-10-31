#include "vfs/mod_archive.h"

#include <archive.h>
#include <archive_entry.h>

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "vfs/archive.h"
#include "vfs/vfs.h"

namespace oc::vfs {

namespace {

namespace fs = std::filesystem;

constexpr size_t kBlockSize = 1 << 20;
constexpr double kSlowReadSeconds = 0.5;

class ZipMod final : public ModArchive {
 public:
  explicit ZipMod(std::unique_ptr<Archive> zip) : zip_(std::move(zip)) {
    for (const std::string& name : zip_->names()) {
      std::string member = normalise_path(name);
      if (!member.empty() && member.back() != '/') names_.push_back(std::move(member));
    }
  }
  std::optional<Bytes> read(const std::string& member) const override { return zip_->read(member); }

 private:
  std::unique_ptr<Archive> zip_;
};

struct ReaderDeleter {
  void operator()(archive* reader) const { archive_read_free(reader); }
};
using Reader = std::unique_ptr<archive, ReaderDeleter>;

std::string reader_error(archive* reader) {
  const char* text = archive_error_string(reader);
  return text ? text : "unreadable archive";
}

Reader open_reader(const fs::path& file, std::string& error) {
  Reader reader(archive_read_new());
  archive_read_support_format_zip(reader.get());
  archive_read_support_format_7zip(reader.get());
  archive_read_support_format_rar(reader.get());
  archive_read_support_format_rar5(reader.get());
#if defined(_WIN32)
  const int status = archive_read_open_filename_w(reader.get(), file.c_str(), kBlockSize);
#else
  const int status = archive_read_open_filename(reader.get(), file.c_str(), kBlockSize);
#endif
  if (status != ARCHIVE_OK) {
    error = reader_error(reader.get());
    reader.reset();
  }
  return reader;
}

// The entry's normalised name when it is a plain file. Type 0 is a RAR made on Unix with attributes that carry no file type.
std::string file_name(archive_entry* entry) {
  const auto type = archive_entry_filetype(entry);
  if (type != AE_IFREG && type != 0) return {};
  const char* name = archive_entry_pathname_utf8(entry);
  std::string member = name ? normalise_path(name) : std::string();
  return !member.empty() && member.back() == '/' ? std::string() : member;
}

std::string display_name(const fs::path& file) {
  const std::u8string text = file.filename().generic_u8string();
  return std::string(text.begin(), text.end());
}

bool read_entry(archive* reader, Bytes& out) {
  std::vector<char> block(kBlockSize);
  while (true) {
    const la_ssize_t got = archive_read_data(reader, block.data(), block.size());
    if (got < 0) return false;
    if (got == 0) return true;
    out.insert(out.end(), block.data(), block.data() + got);
  }
}

class WalkedMod final : public ModArchive {
 public:
  explicit WalkedMod(fs::path file) : file_(std::move(file)) {}

  // Reads the headers only, except that solid RAR data has to be decoded to reach the next header.
  bool list(std::string& error) {
    Reader reader = open_reader(file_, error);
    if (!reader) return false;
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
      std::string name = file_name(entry);
      if (name.empty()) continue;
      if (archive_entry_is_encrypted(entry)) {
        error = "password-protected";
        return false;
      }
      total_ += static_cast<std::uint64_t>(archive_entry_size(entry));
      names_.push_back(std::move(name));
    }
    if (status != ARCHIVE_EOF) {
      error = reader_error(reader.get());
      return false;
    }
    return true;
  }

  bool may_be_slow() const override { return total_ > kMemoryLimit; }

  std::optional<Bytes> read(const std::string& member) const override {
    std::lock_guard lock(mutex_);
    if (total_ <= kMemoryLimit) {
      if (!loaded_) load_all();
      auto it = members_.find(member);
      if (it == members_.end()) return std::nullopt;
      return it->second;
    }
    return walk_to(member);
  }

 private:
  void load_all() const {
    loaded_ = true;
    std::string error;
    Reader reader = open_reader(file_, error);
    if (!reader) {
      log::warn("mods: cannot read {}: {}", display_name(file_), error);
      return;
    }
    archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
      std::string name = file_name(entry);
      if (name.empty()) continue;
      Bytes bytes;
      if (!read_entry(reader.get(), bytes)) {
        log::warn("mods: damaged archive member {}: {}", name, reader_error(reader.get()));
        return;
      }
      members_.try_emplace(std::move(name), std::move(bytes));
    }
  }

  std::optional<Bytes> walk_to(const std::string& member) const {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    Reader reader = open_reader(file_, error);
    if (!reader) return std::nullopt;
    archive_entry* entry = nullptr;
    while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
      if (file_name(entry) != member) continue;
      Bytes bytes;
      if (!read_entry(reader.get(), bytes)) {
        log::warn("mods: damaged archive member {}: {}", member, reader_error(reader.get()));
        return std::nullopt;
      }
      const std::chrono::duration<double> taken = std::chrono::steady_clock::now() - start;
      if (taken.count() > kSlowReadSeconds && !warned_) {
        warned_ = true;
        log::warn("mods: {} is slow to read ({:.1f} s for one file): a large solid archive, repack it as a ZIP or unpack it to a folder", display_name(file_), taken.count());
      }
      return bytes;
    }
    return std::nullopt;
  }

  fs::path file_;
  std::uint64_t total_ = 0;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable bool warned_ = false;
  mutable std::unordered_map<std::string, Bytes> members_;
};

}  // namespace

std::shared_ptr<ModArchive> open_mod_archive(const fs::path& file, std::string& error) {
  auto zip = std::make_unique<Archive>(file);
  if (zip->ok()) {
    if (zip->encrypted()) {
      error = "password-protected";
      return nullptr;
    }
    if (zip->readable()) return std::make_shared<ZipMod>(std::move(zip));
  }
  auto walked = std::make_shared<WalkedMod>(file);
  if (!walked->list(error)) return nullptr;
  return walked;
}

}  // namespace oc::vfs
