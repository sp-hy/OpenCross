#pragma once
// Virtual file system over the Radical Dreamers Edition install.
//
// RDE ships the game as ZIP archives in <GameDir>\data: cdrom.dat (the PS1 file tree), cdrom2.dat (HD-mode
// overrides of some PS1 files), hd.dat, lang.dat, font.dat, ui.dat, movie.dat. Members are addressed by the same
// paths the game uses, e.g. "cdrom/map/mapbin/alu_a01f.bin". This module only knows archives and paths; it has
// no idea what the bytes mean.
//
// Lookup order for read(path): mod overlay (vfs/mods.h: loose files from <GameDir>\mods\<mod>\..., resolved to these
// logical paths, or members of a mod's <archive>.dat) -> archive <first path segment>.dat.
// HD-mode override: when set_hd_mode(true), every "cdrom/<x>" read is tried as "cdrom2/<x>" first (RDE's HD
// override layer: HD-mode rooms, models, textures). In original mode only RDE's fixed exception list goes to cdrom2
// (see cdrom2_first). Callers always ask for "cdrom/..." paths.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace oc::vfs {

// One mounted .dat (ZIP) file; thread-safe reads.
class Archive;
class ModArchive;

// One overlay file: the loose file `file`, or, when `member` is set, that normalised member of the packed mod `file`.
struct OverlayFile {
  std::filesystem::path file;
  std::string member;
  bool operator==(const OverlayFile&) const = default;
  // For logs (UTF-8).
  std::string label() const;
};

// The active mods' files for the current art mode (built by ModManager::apply). Keys are logical paths, normalised (lower case, '/'):
// `files` maps "cdrom/<member>" (cdrom2 members are addressed as cdrom/, like every caller does), "hd/<member>", ... to the mod's
// file (first mod in priority wins); `textures` maps a canonical texture path ("battle_stage/bg_07/00_m1_c320x480.png") to its
// replacement PNG.
struct Overlay {
  std::unordered_map<std::string, OverlayFile> files;
  std::unordered_map<std::string, OverlayFile> textures;
  // Modded art: OpenCross's own improvements on RDE's art are on (HD art stays as RDE draws it).
  bool enhance = false;
};

class Vfs {
 public:
  Vfs();
  ~Vfs();

  // Mounts <game_dir>/data/*.dat lazily; returns false if the directory does not look like an RDE install.
  bool open(const std::filesystem::path& game_dir);
  const std::filesystem::path& game_dir() const { return game_dir_; }

  // Replaces the mod overlay (empty = no mods). Thread-safe against concurrent reads. Bumps generation() when the content changes.
  void set_overlay(Overlay overlay);
  // Changes whenever the overlay's content changes (0 = never had one). Texture caches keyed by path use cache_key() so a mod
  // switch loads the new file instead of the cached one.
  std::uint32_t generation() const { return generation_.load(); }
  std::string cache_key(std::string_view path) const;

  // RDE HD art mode: cdrom2 overrides cdrom for every file (see the header comment).
  void set_hd_mode(bool hd) { hd_mode_ = hd; }
  bool hd_mode() const { return hd_mode_; }

  // "cdrom/map/mapbin/alu_a01f.bin" -> bytes; std::nullopt if absent. Case-insensitive; '\' accepted.
  std::optional<Bytes> read(std::string_view path) const;
  // "cdrom/..." from the PS1 disc image itself (data\cdrom.dat), never the cdrom2 override: original-art screens need the files RDE
  // blanks in cdrom2 (map/window/w*.tim are all-transparent there). Other archives: as read(). The mod overlay still applies.
  std::optional<Bytes> read_disc(std::string_view path) const;
  bool exists(std::string_view path) const;
  // The mod's loose file on disk when `path` resolves to one through the active overlay (never a member of a packed mod), so
  // big files (movies) can be streamed instead of read whole.
  std::optional<std::filesystem::path> loose_file(std::string_view path) const;

  // Member names of one shipped archive ("cdrom", "hd", ...), in archive order. Empty if the archive is absent. Mod path resolution
  // matches against these; a mod replaces members but never adds any.
  std::vector<std::string> list(std::string_view archive) const;

 private:
  Archive* archive(std::string_view name) const;
  std::optional<Bytes> read_overlay(const std::string& logical) const;
  bool cdrom2_first(const std::string& member) const;
  std::shared_ptr<const Overlay> overlay() const;

 public:
  // The packed mod (ZIP, 7z, RAR) at `file`, opened once and reopened when the file changes; null with `error` set if it cannot be read.
  // Thread-safe. Archives the overlay does not use are closed by the next set_overlay.
  std::shared_ptr<ModArchive> mod_archive(const std::filesystem::path& file, std::string* error = nullptr) const;
  // The bytes of a mod's file. Empty on failure.
  std::optional<Bytes> read_mod_file(const OverlayFile& source) const;

  // The active mods' replacement PNG for a dumped texture (render/texture_registry.h: canonical path, lower case, '/'), if any.
  std::optional<OverlayFile> texture_file(std::string_view canonical_path) const;
  // The first (by path) active mod texture whose canonical path starts with `prefix` and ends with `suffix` (tile replacements are matched by their hash only).
  std::optional<OverlayFile> texture_file_matching(std::string_view prefix, std::string_view suffix) const;
  bool has_textures() const;
  // Overlay::enhance of the installed overlay.
  bool enhanced() const;

 private:
  std::filesystem::path game_dir_;
  bool hd_mode_ = false;
  std::atomic<std::uint32_t> generation_{0};
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Splits "cdrom/map/x.bin" into {"cdrom", "map/x.bin"}; normalises separators and case.
std::pair<std::string, std::string> split_archive_path(std::string_view path);
// Lower case, '\' -> '/', no leading '/'.
std::string normalise_path(std::string_view path);

}  // namespace oc::vfs
