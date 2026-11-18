#include "vfs/rde_backend.h"

#include "vfs/archive.h"

namespace oc::vfs {

RdeBackend::RdeBackend(std::filesystem::path data_dir) : data_dir_(std::move(data_dir)) {}
RdeBackend::~RdeBackend() = default;

Archive* RdeBackend::archive(std::string_view name) const {
  std::lock_guard lock(mutex_);
  auto it = archives_.find(name);
  if (it != archives_.end()) return it->second.get();
  const std::string key(name);
  const auto file = data_dir_ / (key + ".dat");
  std::unique_ptr<Archive> opened;
  std::error_code ec;
  if (std::filesystem::is_regular_file(file, ec)) {
    opened = std::make_unique<Archive>(file);
    if (!opened->ok()) opened.reset();
  }
  Archive* raw = opened.get();
  archives_.emplace(key, std::move(opened));
  return raw;
}

bool RdeBackend::has_archive(std::string_view archive_name) const { return archive(archive_name) != nullptr; }

bool RdeBackend::contains(std::string_view archive_name, const std::string& member, int) const {
  const Archive* shipped = archive(archive_name);
  return shipped && shipped->contains(member);
}

std::optional<Bytes> RdeBackend::read(std::string_view archive_name, const std::string& member, int) const {
  const Archive* shipped = archive(archive_name);
  if (!shipped) return std::nullopt;
  return shipped->read(member);
}

std::vector<std::string> RdeBackend::list(std::string_view archive_name) const {
  const Archive* shipped = archive(archive_name);
  return shipped ? shipped->names() : std::vector<std::string>{};
}

}  // namespace oc::vfs
