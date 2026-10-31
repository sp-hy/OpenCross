#include "vfs/vfs.h"

#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "core/log.h"
#include "vfs/archive.h"
#include "vfs/mod_archive.h"

namespace oc::vfs {

std::string normalise_path(std::string_view path) {
  std::string out(path);
  for (char& ch : out) {
    if (ch == '\\') ch = '/';
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  while (!out.empty() && out.front() == '/') out.erase(out.begin());
  return out;
}

namespace {

std::optional<Bytes> read_loose(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    log::warn("vfs: cannot read mod file {}", file.string());
    return std::nullopt;
  }
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

std::string OverlayFile::label() const {
  const std::u8string text = file.generic_u8string();
  return std::string(text.begin(), text.end()) + (member.empty() ? "" : "|" + member);
}

std::pair<std::string, std::string> split_archive_path(std::string_view path) {
  std::string normalised = normalise_path(path);
  size_t slash = normalised.find('/');
  if (slash == std::string::npos) return {normalised, ""};
  return {normalised.substr(0, slash), normalised.substr(slash + 1)};
}

// `archives`: shipped archive name -> the opened archive, nullptr = known missing. `mods`: packed mod path -> what opening it gave,
// with the size and time it had then.
struct Vfs::Impl {
  struct OpenedMod {
    std::shared_ptr<ModArchive> archive;
    std::string error;
    std::string stamp;
  };
  mutable std::mutex mutex;
  mutable std::map<std::string, std::unique_ptr<Archive>> archives;
  mutable std::map<std::filesystem::path, OpenedMod> mods;
  std::shared_ptr<const Overlay> overlay = std::make_shared<Overlay>();
};

Vfs::Vfs() : impl_(std::make_unique<Impl>()) {}
Vfs::~Vfs() = default;

bool Vfs::open(const std::filesystem::path& game_dir) {
  game_dir_ = game_dir;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(game_dir / "data" / "cdrom.dat", ec)) {
    log::error("vfs: {} does not contain data\\cdrom.dat", game_dir.string());
    return false;
  }
  impl_->archives.clear();
  return true;
}

void Vfs::set_overlay(Overlay overlay) {
  auto next = std::make_shared<const Overlay>(std::move(overlay));
  std::set<std::filesystem::path> used;
  for (const auto* files : {&next->files, &next->textures})
    for (const auto& [target, source] : *files)
      if (!source.member.empty()) used.insert(source.file);
  std::lock_guard lock(impl_->mutex);
  if (next->files != impl_->overlay->files || next->textures != impl_->overlay->textures || next->enhance != impl_->overlay->enhance)
    ++generation_;
  impl_->overlay = std::move(next);
  // A reader in progress keeps its archive alive through the shared pointer.
  std::erase_if(impl_->mods, [&](const auto& entry) { return !used.contains(entry.first); });
}

std::string Vfs::cache_key(std::string_view path) const {
  const std::uint32_t gen = generation_.load();
  return gen ? std::string(path) + "|mods" + std::to_string(gen) : std::string(path);
}

std::shared_ptr<const Overlay> Vfs::overlay() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->overlay;
}

std::shared_ptr<ModArchive> Vfs::mod_archive(const std::filesystem::path& file, std::string* error) const {
  std::error_code ec;
  const std::string stamp = std::to_string(std::filesystem::file_size(file, ec)) + "/" +
                            std::to_string(std::filesystem::last_write_time(file, ec).time_since_epoch().count());
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->mods.find(file);
  if (it == impl_->mods.end() || it->second.stamp != stamp) {
    Impl::OpenedMod opened;
    opened.stamp = stamp;
    opened.archive = open_mod_archive(file, opened.error);
    it = impl_->mods.insert_or_assign(file, std::move(opened)).first;
  }
  if (error) *error = it->second.error;
  return it->second.archive;
}

std::optional<Bytes> Vfs::read_mod_file(const OverlayFile& source) const {
  if (source.member.empty()) return read_loose(source.file);
  const std::shared_ptr<ModArchive> packed = mod_archive(source.file);
  return packed ? packed->read(source.member) : std::nullopt;
}

// The active overlay's file for a logical path; nothing when there is none or it cannot be read, so the caller goes on to the game's copy.
std::optional<Bytes> Vfs::read_overlay(const std::string& logical) const {
  auto ov = overlay();
  auto it = ov->files.find(logical);
  if (it == ov->files.end()) return std::nullopt;
  return read_mod_file(it->second);
}

Archive* Vfs::archive(std::string_view name) const {
  std::lock_guard lock(impl_->mutex);
  std::string key(name);
  auto it = impl_->archives.find(key);
  if (it != impl_->archives.end()) return it->second.get();
  auto file = game_dir_ / "data" / (key + ".dat");
  std::unique_ptr<Archive> opened;
  std::error_code ec;
  if (std::filesystem::is_regular_file(file, ec)) {
    opened = std::make_unique<Archive>(file);
    if (!opened->ok()) opened.reset();
  }
  Archive* raw = opened.get();
  impl_->archives.emplace(key, std::move(opened));
  return raw;
}

// RDE tries data\cdrom2\X before data\cdrom\X for every file in HD mode, and for a fixed list of files in original
// mode (openfont.tim, cmptitle.tim, map/window/*, ending.ctd).
bool Vfs::cdrom2_first(const std::string& member) const {
  if (hd_mode_) return true;
  if (member == "map/etc/openfont.tim" || member == "menu/title/cmptitle.tim" || member == "map/mapbin/ending.ctd") return true;
  return member.rfind("map/window/", 0) == 0;
}

std::optional<Bytes> Vfs::read(std::string_view path) const {
  auto [arc, member] = split_archive_path(path);
  if (auto bytes = read_overlay(arc + "/" + member)) return bytes;
  if (arc == "cdrom" && cdrom2_first(member)) {
    if (Archive* cdrom2 = archive("cdrom2")) {
      if (auto bytes = cdrom2->read(member)) return bytes;
    }
  }
  Archive* shipped = archive(arc);
  if (!shipped) return std::nullopt;
  return shipped->read(member);
}

std::optional<Bytes> Vfs::read_disc(std::string_view path) const {
  auto [arc, member] = split_archive_path(path);
  if (arc != "cdrom") return read(path);
  if (auto bytes = read_overlay(arc + "/" + member)) return bytes;
  Archive* shipped = archive(arc);
  if (!shipped) return std::nullopt;
  return shipped->read(member);
}

bool Vfs::exists(std::string_view path) const {
  auto [arc, member] = split_archive_path(path);
  if (auto ov = overlay(); ov->files.contains(arc + "/" + member)) return true;
  if (arc == "cdrom" && cdrom2_first(member)) {
    if (Archive* cdrom2 = archive("cdrom2"); cdrom2 && cdrom2->contains(member)) return true;
  }
  Archive* shipped = archive(arc);
  return shipped && shipped->contains(member);
}

std::optional<std::filesystem::path> Vfs::loose_file(std::string_view path) const {
  auto [arc, member] = split_archive_path(path);
  auto ov = overlay();
  auto it = ov->files.find(arc + "/" + member);
  if (it == ov->files.end() || !it->second.member.empty()) return std::nullopt;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(it->second.file, ec)) return std::nullopt;
  return it->second.file;
}

bool Vfs::has_textures() const { return !overlay()->textures.empty(); }

bool Vfs::enhanced() const { return overlay()->enhance; }

std::optional<OverlayFile> Vfs::texture_file(std::string_view canonical_path) const {
  auto ov = overlay();
  if (ov->textures.empty()) return std::nullopt;
  auto it = ov->textures.find(std::string(canonical_path));
  if (it == ov->textures.end()) return std::nullopt;
  return it->second;
}

std::optional<OverlayFile> Vfs::texture_file_matching(std::string_view prefix, std::string_view suffix) const {
  auto ov = overlay();
  const std::string* best = nullptr;
  const OverlayFile* file = nullptr;
  for (const auto& [target, path] : ov->textures)
    if (target.size() >= prefix.size() + suffix.size() && target.starts_with(prefix) && target.ends_with(suffix) && (!best || target < *best)) best = &target, file = &path;
  if (!file) return std::nullopt;
  return *file;
}

std::vector<std::string> Vfs::list(std::string_view name) const {
  Archive* arc = archive(name);
  return arc ? arc->names() : std::vector<std::string>{};
}

}  // namespace oc::vfs
