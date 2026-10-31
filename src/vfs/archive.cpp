#include "vfs/archive.h"

#include <cstring>

#include "core/log.h"
#include "vfs/vfs.h"

namespace oc::vfs {

Archive::Archive(const std::filesystem::path& file) {
  std::memset(&zip_, 0, sizeof(zip_));
#if defined(_WIN32)
  file_ = _wfopen(file.c_str(), L"rb");
#else
  file_ = std::fopen(file.c_str(), "rb");
#endif
  ok_ = file_ && mz_zip_reader_init_cfile(&zip_, file_, 0, 0) != 0;
  if (!ok_) return;
  const mz_uint count = mz_zip_reader_get_num_files(&zip_);
  names_.reserve(count);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip_, i, &stat)) continue;
    names_.emplace_back(stat.m_filename);
    index_.emplace(normalise_path(stat.m_filename), i);
    if (stat.m_is_directory) continue;
    encrypted_ = encrypted_ || stat.m_is_encrypted;
    readable_ = readable_ && (stat.m_method == 0 || stat.m_method == MZ_DEFLATED);
  }
}

Archive::~Archive() {
  if (ok_) mz_zip_reader_end(&zip_);
  if (file_) std::fclose(file_);
}

std::optional<Bytes> Archive::read(const std::string& member) const {
  auto it = index_.find(member);
  if (it == index_.end()) return std::nullopt;
  std::lock_guard lock(mutex_);
  mz_zip_archive_file_stat stat;
  if (!mz_zip_reader_file_stat(const_cast<mz_zip_archive*>(&zip_), it->second, &stat)) return std::nullopt;
  Bytes out(static_cast<size_t>(stat.m_uncomp_size));
  if (!mz_zip_reader_extract_to_mem(const_cast<mz_zip_archive*>(&zip_), it->second, out.data(), out.size(), 0)) {
    log::warn("vfs: extract failed for {}", member);
    return std::nullopt;
  }
  return out;
}

}  // namespace oc::vfs
