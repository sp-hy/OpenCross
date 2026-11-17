#pragma once
// Finding the player's Chrono Cross discs: image files named in the settings or lying in a folder (recognised by their content, never by
// their name), grouped into sets by build, and the set to play chosen.
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "import/disc/disc_ident.h"
#include "import/disc/sector_source.h"

namespace oc::import::disc {

struct FoundDisc {
  // Where it was found, for the log and the screens: the image file (UTF-8), with " #n" for disc n of a multi-disc file, or a drive.
  std::string where;
  std::shared_ptr<const SectorSource> source;
  DiscIdentity identity;
};

// The file's extension is one of the image containers' (.cue .bin .img .iso .ccd .mds .mdf .m3u .pbp .chd), any case. Only a filter
// for folder scans: the content decides what the file is.
bool has_image_extension(const std::filesystem::path& file);
// The image files directly inside `folder` (no subfolders), the files that name others (.m3u, .cue, .ccd, .mds, .pbp) first, then by
// name. Empty when the folder does not exist.
std::vector<std::filesystem::path> image_files_in(const std::filesystem::path& folder);

// Identifies `source` and adds it to `found` when it is a Chrono Cross disc; otherwise `notes` gets a line saying why not.
void add_disc(std::shared_ptr<const SectorSource> source, std::string where, std::vector<FoundDisc>& found, std::vector<std::string>& notes);
// Every disc the image file names (open_disc_set), through add_disc.
void add_image_file(const std::filesystem::path& file, std::vector<FoundDisc>& found, std::vector<std::string>& notes);

// The build a disc belongs to: the known build's name, else "unknown " + the executable's SHA-256 (both discs of a build carry the same
// executable).
std::string build_key(const DiscIdentity& identity);

// One build's discs among those found.
struct DiscGroup {
  // build_key of its discs.
  std::string key;
  bool known_build = false;
  // Closest region for an unknown build (Unknown when its boot file says nothing).
  Region region = Region::Unknown;
  // Index into the found list of disc 1 and disc 2, -1 = not found. The first disc found of each number wins.
  std::array<int, 2> disc = {-1, -1};
  bool complete() const { return disc[0] >= 0 && disc[1] >= 0; }
};
// The builds in the order their first disc was found.
std::vector<DiscGroup> group_discs(const std::vector<FoundDisc>& found);

struct SetChoice {
  // Index into the groups, -1 = nothing to play (or the region must be asked first).
  int group = -1;
  // Japanese and USA sets are both found and no region with a set is preferred: the player picks one, then choose_set runs again
  // with it.
  bool ask_region = false;
};
// Complete sets are preferred over single discs, a known build over an unknown one, then the order found. `preferred` (Unknown = none)
// picks among the regions; a region with no set is ignored.
SetChoice choose_set(const std::vector<DiscGroup>& groups, Region preferred);

}  // namespace oc::import::disc
