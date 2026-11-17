#pragma once
// Opening CD images. The container is recognised by its content, not its file name: a cue sheet, a CloneCD .ccd, an Alcohol .mds,
// an .m3u playlist, an unencrypted PSN-style .pbp (one or several discs), a raw .bin / .img with no sheet, or a cooked 2048-byte
// .iso, or a CD-type CHD.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "import/disc/sector_source.h"

namespace oc::import::disc {

enum class ImageFormat {
  Unknown,
  Cue,
  RawImage,
  CookedImage,
  CloneCd,
  Mds,
  Playlist,
  Pbp,
  Chd,
};

const char* format_name(ImageFormat format);

// What the file at `path` is, from its first bytes (and, for a playlist, from its lines naming existing files).
ImageFormat detect_format(const std::filesystem::path& path);

// Every disc a file names, in order: one for an image, one per line for a playlist, one per disc for a multi-disc PBP. On failure
// `discs` is empty and `error` says why.
struct DiscSet {
  std::vector<std::unique_ptr<SectorSource>> discs;
  std::string error;
};
DiscSet open_disc_set(const std::filesystem::path& path);

// Disc `index` (0-based) of the set `path` names, or null with `error` filled.
std::unique_ptr<SectorSource> open_image(const std::filesystem::path& path, std::string* error, size_t index = 0);

// The image paths of an .m3u playlist, each resolved against the playlist's folder. Blank lines and lines starting with '#' are
// skipped.
std::vector<std::filesystem::path> read_playlist(const std::filesystem::path& path, std::string* error);

}  // namespace oc::import::disc
