#pragma once
// Is a disc Chrono Cross, which disc of the set, and which build: the boot executable's SHA-256 against the known builds. A disc
// with the game's structure (volume CHRONOCROSS, a file table, disc number 1 or 2) but an unknown executable is an unknown build
// whose closest region follows the boot file's product prefix.
#include <string>

#include "core/types.h"
#include "import/disc/sector_source.h"

namespace oc::import::disc {

enum class Region {
  Unknown,
  Japan,
  Usa,
};

const char* region_name(Region region);

struct DiscIdentity {
  bool chrono_cross = false;
  // The executable matched a known build; otherwise `region` is the closest region (Unknown when the prefix says nothing).
  bool known_build = false;
  Region region = Region::Unknown;
  // Known build's name ("" for an unknown build).
  std::string build;
  // From SYSTEM.CNF, e.g. "SLPS_023.64".
  std::string boot_file;
  std::string exe_sha256;
  std::string volume_id;
  u32 disc_number = 0;
  u32 file_records = 0;
  // Why the disc is not Chrono Cross.
  std::string reason;
};

DiscIdentity identify(const SectorSource& source);

// Region a boot file's product prefix belongs to: SLPS / SLPM / SCPS Japan, SLUS USA.
Region region_of_boot_file(std::string_view boot_file);

}  // namespace oc::import::disc
