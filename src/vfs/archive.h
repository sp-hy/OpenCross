#pragma once
// One ZIP file on disk. miniz keeps the central directory in memory; member reads go through the file on demand. Thread-safe reads.
#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace oc::vfs {

class Archive {
 public:
  explicit Archive(const std::filesystem::path& file);
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  bool ok() const { return ok_; }
  // Member names as stored, directories included.
  const std::vector<std::string>& names() const { return names_; }
  // Any member needs a password.
  bool encrypted() const { return encrypted_; }
  // Every member is stored or deflated, the methods read() handles.
  bool readable() const { return readable_; }

  bool contains(const std::string& member) const { return index_.contains(member); }
  std::optional<Bytes> read(const std::string& member) const;

 private:
  mz_zip_archive zip_{};
  FILE* file_ = nullptr;
  bool ok_ = false;
  bool encrypted_ = false;
  bool readable_ = true;
  std::vector<std::string> names_;
  std::unordered_map<std::string, mz_uint> index_;
  mutable std::mutex mutex_;
};

}  // namespace oc::vfs
