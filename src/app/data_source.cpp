#include "app/data_source.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <system_error>

#include "core/log.h"
#include "import/console_font.h"
#include "platform/paths.h"

namespace oc::app {

namespace fs = std::filesystem;
using import::disc::Region;

namespace {

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

Region region_of_word(const std::string& word) { return word == "jp" ? Region::Japan : word == "usa" ? Region::Usa : Region::Unknown; }

// The console font comes from a BIOS image in the exe's folder or in a folder of the opened disc images (only those folders, not
// their subfolders).
void load_console_font(const SourceSearch& search, const import::disc::DiscGroup& group, const fs::path& exe) {
  std::vector<fs::path> dirs{exe};
  for (const int index : group.disc) {
    if (index < 0) continue;
    std::string where = search.found[static_cast<size_t>(index)].where;
    std::error_code ec;
    if (const size_t mark = where.rfind(" #"); mark != std::string::npos && !fs::is_regular_file(from_utf8(where), ec)) where.resize(mark);
    const fs::path file = from_utf8(where);
    if (!fs::is_regular_file(file, ec)) continue;
    const fs::path dir = file.parent_path();
    if (std::none_of(dirs.begin(), dirs.end(), [&](const fs::path& known) { return fs::equivalent(known, dir, ec); })) dirs.push_back(dir);
  }
  fs::path file;
  import::set_console_font(import::find_console_font(dirs, &file));
  if (file.empty()) log::info("console font: no BIOS image in the exe's or the disc images' folders; the menus draw the disc's font");
  else log::info("console font: {}", utf8(file));
}

}  // namespace

void scan_image(const fs::path& file, SourceSearch& search) { import::disc::add_image_file(file, search.found, search.notes); }

std::vector<import::disc::FoundDisc> optical_discs(bool every_disc) {
  std::vector<import::disc::FoundDisc> found;
  std::vector<import::disc::DriveCandidate> drives = every_disc ? import::disc::probe_optical_drives() : import::disc::scan_optical_drives();
  for (import::disc::DriveCandidate& drive : drives) {
    if (!drive.media) continue;
    if (!drive.source) drive.identity.reason = drive.error;
    found.push_back({drive.drive + (drive.product.empty() ? "" : " (" + drive.product + ")"),
                     std::shared_ptr<const import::disc::SectorSource>(std::move(drive.source)), std::move(drive.identity)});
  }
  return found;
}

SourceSearch search_sources(const platform::Config& config, const fs::path& exe, const ScanImage& scan) {
  using platform::DataSource;
  SourceSearch search;
  if (config.source != DataSource::Disc) {
    search.rde_dir = platform::find_game_dir(config.game_dir);
    search.searched.push_back("Radical Dreamers Edition install (data\\cdrom.dat): " +
                              (config.game_dir.empty() ? std::string() : "[Paths] GameDir " + config.game_dir + ", ") + "the folder of opencross.exe (" +
                              utf8(exe) + ") and its parent folders");
    if (!search.rde_dir.empty() || config.source == DataSource::Rde) return search;
  }
  const auto finish = [&] {
    search.groups = import::disc::group_discs(search.found);
    search.choice = import::disc::choose_set(search.groups, region_of_word(config.disc_region));
    return search;
  };
  // The discs one place added to search.found from `first` on, merged with those of the places before; true once a whole set is found.
  const auto merge = [&](size_t first) {
    std::vector<import::disc::FoundDisc> place(std::make_move_iterator(search.found.begin() + static_cast<std::ptrdiff_t>(first)),
                                               std::make_move_iterator(search.found.end()));
    search.found.resize(first);
    import::disc::merge_place(search.found, std::move(place));
    return import::disc::has_complete_set(search.found);
  };
  for (const std::string* named : {&config.disc1, &config.disc2}) {
    if (named->empty()) continue;
    const fs::path file = from_utf8(*named);
    search.searched.push_back("[Paths] Disc" + std::string(named == &config.disc1 ? "1" : "2") + ": " + *named);
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) scan(file, search);
    else search.notes.push_back(*named + ": file not found");
  }
  if (!config.disc_dir.empty()) {
    search.searched.push_back("[Paths] DiscDir: " + config.disc_dir);
    for (const fs::path& file : import::disc::image_files_in(from_utf8(config.disc_dir))) scan(file, search);
  }
  if (merge(0)) return finish();
  size_t first = search.found.size();
  search.searched.push_back("Disc images in the folder of opencross.exe: " + utf8(exe));
  for (const fs::path& file : import::disc::image_files_in(exe)) scan(file, search);
  if (merge(first)) return finish();
  first = search.found.size();
  search.searched.push_back("Optical drives");
  for (import::disc::FoundDisc& disc : optical_discs()) search.found.push_back(std::move(disc));
  merge(first);
  return finish();
}

OpenedDiscs open_discs(const SourceSearch& search, vfs::Vfs& files, const fs::path& exe) {
  const import::disc::DiscGroup& group = search.groups[static_cast<size_t>(search.choice.group)];
  OpenedDiscs opened;
  auto backend = std::make_unique<import::disc::DiscBackend>();
  std::string boot_file;
  for (int number = 1; number <= 2; ++number) {
    const int index = group.disc[static_cast<size_t>(number - 1)];
    if (index < 0) {
      log::info("data: disc {} not found; it is asked for when the story needs it", number);
      continue;
    }
    const import::disc::FoundDisc& disc = search.found[static_cast<size_t>(index)];
    boot_file = disc.identity.boot_file;
    opened.drives[static_cast<size_t>(number - 1)] = dynamic_cast<const import::disc::DriveSource*>(disc.source.get());
    std::string error;
    if (backend->mount(number, disc.source, &error))
      log::info("data: disc {}: {} ({}, {})", number, disc.where, disc.identity.known_build ? disc.identity.build : "unknown build",
                disc.source->description());
    else
      log::warn("data: disc {}: {}: {}", number, disc.where, error);
  }
  const Region region = group.region == Region::Unknown ? Region::Japan : group.region;
  opened.region_word = region == Region::Usa ? "usa" : "jp";
  opened.build_key = group.key;
  if (!group.known_build) {
    opened.warning = "Unknown Chrono Cross version (boot file " + boot_file + "): playing it as the " + import::disc::region_name(region) + " version.";
    log::warn("data: {}", opened.warning);
  }
  opened.backend = backend.get();
  files.open(std::move(backend), exe);
  load_console_font(search, group, exe);
  // Disc 1 is in the drive until the story or a save says otherwise.
  files.set_disc(1);
  platform::set_disc_mode(true);
  return opened;
}

void apply_disc_settings(platform::Config& config, std::string_view region_word) {
  config.art_mode = platform::ArtMode::Original;
  config.bgm_source = "original";
  config.ps1_res_ui = true;
  config.language = region_word == "usa" ? "en" : "jp";
  config.legacy_field_models = true;
  config.portraits = "1999";
  log::info("disc mode ({}): classic art, original music, PS1-resolution UI, language {}", region_word, config.language);
}

bool open_game_data(platform::Config& config, const fs::path& exe, vfs::Vfs& files, std::string* error) {
  const SourceSearch search = search_sources(config, exe);
  if (!search.playable()) {
    std::string text = search.choice.ask_region ? "Japanese and USA discs found: set [Disc] Region to jp or usa" : "no game files found";
    for (const std::string& line : search.searched) text += "\n  searched " + line;
    for (const std::string& line : search.notes) text += "\n  " + line;
    if (error) *error = text;
    return false;
  }
  if (!search.rde_dir.empty()) {
    if (files.open(search.rde_dir)) return true;
    if (error) *error = "the install at " + utf8(search.rde_dir) + " does not open";
    return false;
  }
  apply_disc_settings(config, open_discs(search, files, exe).region_word);
  return true;
}

}  // namespace oc::app
