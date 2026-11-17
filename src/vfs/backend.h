#pragma once
// Where the shipped game data comes from. Vfs asks its backend for members of the shipped archives by name ("cdrom", "cdrom2", "hd",
// "lang", "font", "ui", "movie"); the mod overlay and the cdrom2 override rule stay in Vfs, so they work the same over every backend.
// RDE's install (RdeBackend) serves every archive; a PS1 disc serves only "cdrom" and reports the others absent. Members are
// normalised (vfs::normalise_path). Every method may be called from several threads at once.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace oc::vfs {

class Backend {
 public:
  virtual ~Backend() = default;

  // The backend serves this archive at all (its members may still be missing).
  virtual bool has_archive(std::string_view archive) const = 0;
  // `disc` is the disc in the drive (1 or 2, Vfs::set_disc); a backend whose files do not depend on it ignores it.
  virtual bool contains(std::string_view archive, const std::string& member, int disc) const = 0;
  virtual std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const = 0;
  // Member names of an archive in the backend's own order, directories possibly included; empty when the archive is absent.
  virtual std::vector<std::string> list(std::string_view archive) const = 0;
};

}  // namespace oc::vfs
