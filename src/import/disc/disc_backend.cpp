#include "import/disc/disc_backend.h"

#include <mutex>

namespace oc::import::disc {

namespace {
size_t side_of(int disc) { return disc >= 2 ? 1 : 0; }
}  // namespace

DiscBackend::DiscBackend(const vfs::DiscNames& names) : names_(names) {}

bool DiscBackend::mount(int disc, std::shared_ptr<const SectorSource> source, std::string* error) {
  if (!source) {
    if (error) *error = "no disc";
    return false;
  }
  std::optional<FileTable> table = FileTable::read(*source, error);
  if (!table) return false;
  auto mounted_disc = std::make_shared<Disc>(Disc{std::move(source), std::move(*table)});
  std::unique_lock lock(mutex_);
  discs_[side_of(disc)] = std::move(mounted_disc);
  rebuild_list();
  return true;
}

void DiscBackend::unmount(int disc) {
  std::unique_lock lock(mutex_);
  discs_[side_of(disc)].reset();
  rebuild_list();
}

bool DiscBackend::mounted(int disc) const {
  std::shared_lock lock(mutex_);
  return discs_[side_of(disc)] != nullptr;
}

int DiscBackend::first_with_data(const Disc& disc, const std::vector<int>& records) {
  for (int record : records)
    if (static_cast<size_t>(record) < disc.table.size() && !disc.table.span(static_cast<size_t>(record)).empty) return record;
  return -1;
}

DiscBackend::Located DiscBackend::locate(const std::string& member, int disc) const {
  const std::string path = "cdrom/" + member;
  const size_t here = side_of(disc);
  std::array<std::shared_ptr<const Disc>, 2> discs;
  {
    std::shared_lock lock(mutex_);
    discs = discs_;
  }
  if (discs[here]) {
    if (const int record = first_with_data(*discs[here], names_.records_of(path, static_cast<int>(here) + 1)); record >= 0)
      return {discs[here], static_cast<size_t>(record)};
  }
  // A record number means a different file on each disc.
  if (member.starts_with(vfs::DiscNames::kRecordFolder)) return {};
  const size_t there = 1 - here;
  if (discs[there]) {
    if (const int record = first_with_data(*discs[there], names_.records_of(path, static_cast<int>(there) + 1)); record >= 0)
      return {discs[there], static_cast<size_t>(record)};
  }
  return {};
}

void DiscBackend::rebuild_list() {
  members_.clear();
  for (const std::string& member : names_.members()) {
    const std::string path = "cdrom/" + member;
    for (size_t side = 0; side < discs_.size(); ++side) {
      if (discs_[side] && first_with_data(*discs_[side], names_.records_of(path, static_cast<int>(side) + 1)) >= 0) {
        members_.push_back(member);
        break;
      }
    }
  }
}

bool DiscBackend::has_archive(std::string_view archive) const { return archive == "cdrom"; }

bool DiscBackend::contains(std::string_view archive, const std::string& member, int disc) const {
  return archive == "cdrom" && locate(member, disc).disc != nullptr;
}

std::optional<Bytes> DiscBackend::read(std::string_view archive, const std::string& member, int disc) const {
  if (archive != "cdrom") return std::nullopt;
  const Located found = locate(member, disc);
  if (!found.disc) return std::nullopt;
  return read_record(*found.disc->source, found.disc->table, found.record);
}

std::vector<std::string> DiscBackend::list(std::string_view archive) const {
  if (archive != "cdrom") return {};
  std::shared_lock lock(mutex_);
  return members_;
}

}  // namespace oc::import::disc
