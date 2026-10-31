#pragma once
// A mod packed in one file, read live with nothing written to disk. A ZIP with the usual methods reads each member on its own. Anything
// else (7z, RAR, other ZIP methods; the format is recognised by content) can only be walked from the start, so an archive of up to
// kMemoryLimit unpacked bytes is decoded into memory at its first read, and a larger one is walked per read, which is fine when the
// members are stored separately and slow when the archive is solid.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::vfs {

class ModArchive {
 public:
  virtual ~ModArchive() = default;
  // The files, with normalised names (lower case, '/'), in archive order.
  const std::vector<std::string>& names() const { return names_; }
  // `member` is one of names(). Thread-safe.
  virtual std::optional<Bytes> read(const std::string& member) const = 0;
  // Reads may have to decode from the start of the archive: true past kMemoryLimit unless members can be read on their own. Whether the
  // archive really is solid cannot be told in advance.
  virtual bool may_be_slow() const { return false; }

  static constexpr std::uint64_t kMemoryLimit = 256ull << 20;

 protected:
  std::vector<std::string> names_;
};

// Null with `error` set when the file cannot be listed: damaged, password-protected, not an archive, one part of a multi-part archive.
std::shared_ptr<ModArchive> open_mod_archive(const std::filesystem::path& file, std::string& error);

}  // namespace oc::vfs
