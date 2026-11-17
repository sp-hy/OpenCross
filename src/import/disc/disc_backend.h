#pragma once
// The PS1 discs as a VFS backend. A "cdrom/<member>" path is looked up by name in the disc indexes (vfs/disc_names.h) and read from
// that record of the disc in the drive (Vfs::set_disc); a file the disc in the drive does not have comes from the other disc when it
// is mounted and its index names the same file (a name holds the same bytes on both discs). "cdrom/#record/<n>" reads record n of the
// disc in the drive only. Only the "cdrom" archive exists; RDE's other archives are absent. Discs can be mounted while files are read.
#include <array>
#include <memory>
#include <shared_mutex>
#include <string>

#include "import/disc/disc_table.h"
#include "import/disc/sector_source.h"
#include "vfs/backend.h"
#include "vfs/disc_names.h"

namespace oc::import::disc {

class DiscBackend final : public vfs::Backend {
 public:
  explicit DiscBackend(const vfs::DiscNames& names = vfs::DiscNames::builtin());

  // Mounts `source` as disc 1 or 2 (others clamp), replacing what was there. False with `error` set when its file table does not read.
  bool mount(int disc, std::shared_ptr<const SectorSource> source, std::string* error = nullptr);
  // Drops disc 1 or 2 (a disc taken out of its drive); its files then come from the other disc where it names them.
  void unmount(int disc);
  bool mounted(int disc) const;

  bool has_archive(std::string_view archive) const override;
  bool contains(std::string_view archive, const std::string& member, int disc) const override;
  std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const override;
  // "cdrom": every member the indexes name that a mounted disc holds data for, disc 1's names first.
  std::vector<std::string> list(std::string_view archive) const override;

 private:
  struct Disc {
    std::shared_ptr<const SectorSource> source;
    FileTable table;
  };
  struct Located {
    std::shared_ptr<const Disc> disc;
    size_t record = 0;
  };
  // The mounted disc and record holding `member` for disc `disc` in the drive; no disc when none does.
  Located locate(const std::string& member, int disc) const;
  // The first of `records` that holds data on `disc`, else -1.
  static int first_with_data(const Disc& disc, const std::vector<int>& records);
  void rebuild_list();

  const vfs::DiscNames& names_;
  mutable std::shared_mutex mutex_;
  std::array<std::shared_ptr<const Disc>, 2> discs_;
  std::vector<std::string> members_;
};

}  // namespace oc::import::disc
