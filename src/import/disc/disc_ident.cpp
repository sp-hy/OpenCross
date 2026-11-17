#include "import/disc/disc_ident.h"

#include "import/disc/disc_table.h"
#include "import/disc/sha256.h"

namespace oc::import::disc {

namespace {

constexpr std::string_view kVolumeId = "CHRONOCROSS";

struct KnownBuild {
  std::string_view sha256;
  Region region;
  std::string_view name;
};

// The executable is the same on both discs of a set.
constexpr KnownBuild kKnownBuilds[] = {
    {"441a6ed70fc2e8e9a0e6ac9d30586359dd163f548e7ecff04e7df7adca096f61", Region::Japan, "Japan (SLPS-02364 / SLPS-02365)"},
    {"9a5453bf09d7f5c71a6451d255471374fb2d7cc5f3dfb1ea51223a5c88cdda47", Region::Usa, "USA (SLUS-01041 / SLUS-01080)"},
};

}  // namespace

const char* region_name(Region region) {
  switch (region) {
    case Region::Unknown: return "unknown";
    case Region::Japan: return "JP";
    case Region::Usa: return "USA";
  }
  return "unknown";
}

Region region_of_boot_file(std::string_view boot_file) {
  const std::string_view prefix = boot_file.substr(0, 4);
  if (prefix == "SLPS" || prefix == "SLPM" || prefix == "SCPS") return Region::Japan;
  if (prefix == "SLUS") return Region::Usa;
  return Region::Unknown;
}

DiscIdentity identify(const SectorSource& source) {
  DiscIdentity out;
  std::string error;
  const std::optional<IsoVolume> volume = read_iso_volume(source, &error);
  if (!volume) {
    out.reason = error;
    return out;
  }
  out.volume_id = volume->volume_id;
  if (const IsoEntry* cnf = volume->find("SYSTEM.CNF")) {
    if (const std::optional<Bytes> text = read_iso_file(source, *cnf))
      out.boot_file = boot_file_name(std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
  }
  if (out.boot_file.empty()) {
    out.reason = "no SYSTEM.CNF boot line";
    return out;
  }
  const IsoEntry* exe = volume->find(out.boot_file);
  if (!exe) {
    out.reason = "boot file " + out.boot_file + " is not in the root directory";
    return out;
  }
  if (const std::optional<Bytes> bytes = read_iso_file(source, *exe)) out.exe_sha256 = sha256_hex(*bytes);
  if (out.volume_id != kVolumeId) {
    out.reason = "volume is " + out.volume_id + ", not " + std::string(kVolumeId);
    return out;
  }
  const std::optional<FileTable> table = FileTable::read(source, &error);
  if (!table || !table->plausible(source.sector_count())) {
    out.reason = table ? "the file table does not fit the image" : error;
    return out;
  }
  out.file_records = static_cast<u32>(table->size());
  out.disc_number = read_disc_number(source).value_or(0);
  if (out.disc_number != 1 && out.disc_number != 2) {
    out.reason = "disc-number sector holds " + std::to_string(out.disc_number);
    out.disc_number = 0;
    return out;
  }
  out.chrono_cross = true;
  for (const KnownBuild& build : kKnownBuilds) {
    if (build.sha256 != out.exe_sha256) continue;
    out.known_build = true;
    out.region = build.region;
    out.build = build.name;
    return out;
  }
  out.region = region_of_boot_file(out.boot_file);
  return out;
}

}  // namespace oc::import::disc
