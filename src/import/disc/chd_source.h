#pragma once
// CHD (compressed hunks of data) CD images, decoded with libchdr.
#include <filesystem>
#include <memory>
#include <string>

#include "import/disc/sector_source.h"

namespace oc::import::disc {

// The disc a CD-type CHD holds, or null with `error` filled (not a CD image, needs a parent, a damaged file, an unsupported track type).
std::unique_ptr<SectorSource> open_chd(const std::filesystem::path& path, std::string* error);

}  // namespace oc::import::disc
