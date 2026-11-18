#pragma once
// Where the game data comes from, for the app and the dev viewers alike. [Paths] Source=auto searches, in order: an RDE install
// (find_game_dir), the images [Paths] Disc1 / Disc2 name and those in [Paths] DiscDir, the images next to opencross.exe, the optical
// drives. The first place holding a Chrono Cross disc gives the build; later places are searched only for the disc it still lacks. rde
// searches only the install, disc skips it. Opening a set of discs mounts them as the VFS backend and turns disc mode on, with its fixed
// settings and the console font of a BIOS image found next to the exe or the images.
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "import/disc/disc_backend.h"
#include "import/disc/disc_search.h"
#include "import/disc/drive_source.h"
#include "platform/config.h"
#include "vfs/vfs.h"

namespace oc::app {

struct SourceSearch {
  std::filesystem::path rde_dir;
  std::vector<import::disc::FoundDisc> found;
  std::vector<import::disc::DiscGroup> groups;
  import::disc::SetChoice choice;
  // The places searched and the files there that cannot be played, for the not-found screen.
  std::vector<std::string> searched, notes;
  bool playable() const { return !rde_dir.empty() || choice.group >= 0; }
};

// Adds the discs of one image file to `search` (found, or a note saying why not).
using ScanImage = std::function<void(const std::filesystem::path& file, SourceSearch& search)>;
// The plain scan: the file is opened every time.
void scan_image(const std::filesystem::path& file, SourceSearch& search);

// The search the config's [Paths] keys ask for; `exe` is the folder of opencross.exe. `scan` opens each image file found.
SourceSearch search_sources(const platform::Config& config, const std::filesystem::path& exe, const ScanImage& scan = scan_image);
// The Chrono Cross discs in the optical drives; with `every_disc` also every other disc in a drive, and one that does not read as a
// disc with no source (identity.reason says why).
std::vector<import::disc::FoundDisc> optical_discs(bool every_disc = false);

struct OpenedDiscs {
  // Owned by the VFS.
  import::disc::DiscBackend* backend = nullptr;
  // The mounted discs that sit in an optical drive (null for an image).
  std::array<const import::disc::DriveSource*, 2> drives{};
  // "jp" or "usa": the build, or the closest region of an unknown build.
  std::string region_word;
  std::string build_key;
  // Set for an unknown build: what is played instead.
  std::string warning;
};
// Mounts the chosen set of `search` and opens `files` over it (its game dir = `exe`, disc 1 in the drive), turns disc mode on and
// loads the console font.
OpenedDiscs open_discs(const SourceSearch& search, vfs::Vfs& files, const std::filesystem::path& exe);
// Disc mode's fixed settings for the build `region_word` plays (classic art, original music, PS1-resolution UI, its language, the
// field models, the 1999 portraits).
void apply_disc_settings(platform::Config& config, std::string_view region_word);

// For the dev viewers, which have no screens for it: searches, opens `files` over what was found and, on discs, applies the fixed
// settings to `config`. False with `error` set when nothing can be played, or when sets of both regions are found and [Disc] Region
// picks neither.
bool open_game_data(platform::Config& config, const std::filesystem::path& exe, vfs::Vfs& files, std::string* error);

}  // namespace oc::app
