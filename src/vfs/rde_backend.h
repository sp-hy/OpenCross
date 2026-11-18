#pragma once
// The Radical Dreamers Edition install as a VFS backend: <GameDir>\data\<archive>.dat, ZIP files mounted on first use and kept open.
// The disc number is ignored (cdrom.dat holds the files of both discs).
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include "vfs/backend.h"

namespace oc::vfs {

class Archive;

class RdeBackend final : public Backend {
 public:
  // `data_dir` is <GameDir>\data.
  explicit RdeBackend(std::filesystem::path data_dir);
  ~RdeBackend() override;

  bool has_archive(std::string_view archive) const override;
  bool contains(std::string_view archive, const std::string& member, int disc) const override;
  std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const override;
  std::vector<std::string> list(std::string_view archive) const override;

 private:
  // The mounted archive, null when its file is missing or not a readable ZIP (remembered either way).
  Archive* archive(std::string_view name) const;

  std::filesystem::path data_dir_;
  mutable std::mutex mutex_;
  mutable std::map<std::string, std::unique_ptr<Archive>, std::less<>> archives_;
};

}  // namespace oc::vfs
