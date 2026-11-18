#pragma once
// Where the shipped game data comes from. Vfs asks its backend for members of the shipped archives by name ("cdrom", "cdrom2", "hd",
// "lang", "font", "ui", "movie"); the mod overlay and the cdrom2 override rule stay in Vfs, so they work the same over every backend.
// RDE's install (RdeBackend) serves every archive; a PS1 disc serves only "cdrom" and reports the others absent. Members are
// normalised (vfs::normalise_path). Every method may be called from several threads at once.
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace oc::vfs {

// A member read piece by piece instead of whole (a movie stream from an optical drive). One thread uses a reader at a time.
class Reader {
 public:
  virtual ~Reader() = default;
  virtual u64 size() const = 0;
  // `count` bytes from `offset` into `out`; false when the range runs past the end or the read fails.
  virtual bool read(u64 offset, size_t count, u8* out) = 0;
};

// A reader over bytes already in memory.
class MemoryReader final : public Reader {
 public:
  explicit MemoryReader(Bytes bytes) : bytes_(std::move(bytes)) {}
  u64 size() const override { return bytes_.size(); }
  bool read(u64 offset, size_t count, u8* out) override {
    if (offset > bytes_.size() || count > bytes_.size() - offset) return false;
    if (count) std::memcpy(out, bytes_.data() + offset, count);
    return true;
  }

 private:
  Bytes bytes_;
};

class Backend {
 public:
  virtual ~Backend() = default;

  // The backend serves this archive at all (its members may still be missing).
  virtual bool has_archive(std::string_view archive) const = 0;
  // `disc` is the disc in the drive (1 or 2, Vfs::set_disc); a backend whose files do not depend on it ignores it.
  virtual bool contains(std::string_view archive, const std::string& member, int disc) const = 0;
  virtual std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const = 0;
  // The member as a reader, null when absent. The default reads it whole; a backend that can read part of a member overrides it.
  virtual std::unique_ptr<Reader> open_reader(std::string_view archive, const std::string& member, int disc) const {
    std::optional<Bytes> bytes = read(archive, member, disc);
    return bytes ? std::make_unique<MemoryReader>(std::move(*bytes)) : nullptr;
  }
  // Member names of an archive in the backend's own order, directories possibly included; empty when the archive is absent.
  virtual std::vector<std::string> list(std::string_view archive) const = 0;
};

}  // namespace oc::vfs
