#include "import/disc/disc_search.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <system_error>

#include "import/disc/disc_image.h"

namespace oc::import::disc {

namespace fs = std::filesystem;

namespace {

std::string lower_extension(const fs::path& file) {
  const std::u8string text = file.extension().u8string();
  std::string out(text.begin(), text.end());
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

// 0 for the files that name other files, 1 for the rest.
int scan_rank(const fs::path& file) {
  const std::string ext = lower_extension(file);
  return ext == ".m3u" || ext == ".cue" || ext == ".ccd" || ext == ".mds" || ext == ".pbp" ? 0 : 1;
}

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

}  // namespace

bool has_image_extension(const fs::path& file) {
  static constexpr std::string_view kExtensions[] = {".cue", ".bin", ".img", ".iso", ".ccd", ".mds", ".mdf", ".m3u", ".pbp", ".chd"};
  const std::string ext = lower_extension(file);
  return std::find(std::begin(kExtensions), std::end(kExtensions), ext) != std::end(kExtensions);
}

std::vector<fs::path> image_files_in(const fs::path& folder) {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
    std::error_code type_error;
    if (entry.is_regular_file(type_error) && has_image_extension(entry.path())) files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
    const int lhs_rank = scan_rank(lhs), rhs_rank = scan_rank(rhs);
    return lhs_rank != rhs_rank ? lhs_rank < rhs_rank : lhs < rhs;
  });
  return files;
}

void add_disc(std::shared_ptr<const SectorSource> source, std::string where, std::vector<FoundDisc>& found, std::vector<std::string>& notes) {
  DiscIdentity identity = identify(*source);
  if (!identity.chrono_cross) {
    notes.push_back(where + ": not a Chrono Cross disc (" + identity.reason + ")");
    return;
  }
  found.push_back({std::move(where), std::move(source), std::move(identity)});
}

void add_image_file(const fs::path& file, std::vector<FoundDisc>& found, std::vector<std::string>& notes) {
  DiscSet set = open_disc_set(file);
  if (set.discs.empty()) {
    notes.push_back(utf8(file) + ": " + set.error);
    return;
  }
  const bool several = set.discs.size() > 1;
  for (size_t i = 0; i < set.discs.size(); ++i)
    add_disc(std::shared_ptr<const SectorSource>(std::move(set.discs[i])), utf8(file) + (several ? " #" + std::to_string(i + 1) : ""), found,
             notes);
}

std::string build_key(const DiscIdentity& identity) { return identity.known_build ? identity.build : "unknown " + identity.exe_sha256; }

std::vector<DiscGroup> group_discs(const std::vector<FoundDisc>& found) {
  std::vector<DiscGroup> groups;
  for (size_t i = 0; i < found.size(); ++i) {
    const DiscIdentity& identity = found[i].identity;
    if (identity.disc_number != 1 && identity.disc_number != 2) continue;
    const std::string key = build_key(identity);
    auto group = std::find_if(groups.begin(), groups.end(), [&](const DiscGroup& candidate) { return candidate.key == key; });
    if (group == groups.end()) {
      groups.push_back({key, identity.known_build, identity.region, {-1, -1}});
      group = groups.end() - 1;
    }
    int& slot = group->disc[identity.disc_number - 1];
    if (slot < 0) slot = static_cast<int>(i);
  }
  return groups;
}

void merge_place(std::vector<FoundDisc>& found, std::vector<FoundDisc> place) {
  if (found.empty()) {
    found = std::move(place);
    return;
  }
  const size_t earlier = found.size();
  for (FoundDisc& disc : place) {
    const std::string key = build_key(disc.identity);
    const auto same_build = [&](const FoundDisc& known) { return build_key(known.identity) == key; };
    if (std::none_of(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(earlier), same_build)) continue;
    const bool have_number = std::any_of(found.begin(), found.end(), [&](const FoundDisc& known) {
      return same_build(known) && known.identity.disc_number == disc.identity.disc_number;
    });
    if (!have_number) found.push_back(std::move(disc));
  }
}

bool has_complete_set(const std::vector<FoundDisc>& found) {
  const std::vector<DiscGroup> groups = group_discs(found);
  return std::any_of(groups.begin(), groups.end(), [](const DiscGroup& group) { return group.complete(); });
}

DiscFit disc_fit(const DiscIdentity& identity, int needed, const std::string& build) {
  if (!identity.chrono_cross) return DiscFit::NotChronoCross;
  if (static_cast<int>(identity.disc_number) != needed) return DiscFit::OtherDisc;
  if (build_key(identity) != build) return DiscFit::OtherBuild;
  return DiscFit::Fits;
}

DiscPick pick_disc(const std::vector<FoundDisc>& found, int needed, const std::string& build) {
  DiscPick pick;
  for (size_t i = 0; i < found.size(); ++i) {
    const DiscFit fit = disc_fit(found[i].identity, needed, build);
    if (fit == DiscFit::Fits) return {static_cast<int>(i), fit};
    pick.fit = std::max(pick.fit, fit);
  }
  return pick;
}

SetChoice choose_set(const std::vector<DiscGroup>& groups, Region preferred) {
  const bool any_complete = std::any_of(groups.begin(), groups.end(), [](const DiscGroup& group) { return group.complete(); });
  std::vector<int> pool;
  for (size_t i = 0; i < groups.size(); ++i)
    if (!any_complete || groups[i].complete()) pool.push_back(static_cast<int>(i));
  if (pool.empty()) return {};
  // The best of the pool among those `accept` takes: a known build first.
  auto best = [&](auto accept) {
    int fallback = -1;
    for (int index : pool) {
      if (!accept(groups[static_cast<size_t>(index)])) continue;
      if (groups[static_cast<size_t>(index)].known_build) return index;
      if (fallback < 0) fallback = index;
    }
    return fallback;
  };
  if (preferred != Region::Unknown)
    if (const int index = best([&](const DiscGroup& group) { return group.region == preferred; }); index >= 0) return {index, false};
  const auto has_region = [&](Region region) {
    return std::any_of(pool.begin(), pool.end(), [&](int index) { return groups[static_cast<size_t>(index)].region == region; });
  };
  if (has_region(Region::Japan) && has_region(Region::Usa)) return {-1, true};
  return {best([](const DiscGroup&) { return true; }), false};
}

}  // namespace oc::import::disc
