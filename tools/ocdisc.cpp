// ocdisc: PS1 disc image inspector.
//   ocdisc identify <image>                                   every disc the image names: container, layout, build, disc number
//   ocdisc ls <image> [--disc N] [--from R] [--count M]       file table records: record, LBA, size, empty flag
//   ocdisc extract <image> <record> <out> [--disc N]          one record's bytes to a file
//   ocdisc verify <image> --against <cdrom.dat> [--disc N]    every named record against the cdrom.dat member of that name
//   ocdisc audit <image> --against <cdrom.dat> [--disc N] [--only C1,C2]
//                                                             every file through the engine's importers, next to the member
//   ocdisc drives                                             optical drives: device, disc, read mode, Chrono Cross identity
// <image> is any container the disc reader opens (cue, raw / cooked image, ccd, mds, m3u, pbp) or an optical drive ("D:"). --disc
// picks a disc of a playlist or multi-disc PBP (1-based, default 1).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "import/disc/disc_ident.h"
#include "import/disc/disc_image.h"
#include "import/disc/disc_table.h"
#include "import/disc/drive_source.h"
#include "vfs/archive.h"

using namespace oc;
using namespace oc::import::disc;

int ocdisc_audit(const std::string& image, const std::string& against, size_t disc_index, const std::string& only);

namespace {

struct Options {
  std::vector<std::string> positional;
  size_t disc = 1;
  size_t from = 0;
  size_t count = ~size_t{0};
  std::string against;
  std::string only;
};

bool parse(int argc, char** argv, Options& options) {
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--disc" && has_value) options.disc = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--from" && has_value) options.from = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--count" && has_value) options.count = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--against" && has_value) options.against = argv[++i];
    else if (arg == "--only" && has_value) options.only = argv[++i];
    else if (arg.starts_with("--")) return false;
    else options.positional.push_back(arg);
  }
  return options.disc >= 1;
}

int usage() {
  std::fprintf(stderr,
               "usage: ocdisc identify <image>\n"
               "       ocdisc ls <image> [--disc N] [--from R] [--count M]\n"
               "       ocdisc extract <image> <record> <out> [--disc N]\n"
               "       ocdisc verify <image> --against <cdrom.dat> [--disc N]\n"
               "       ocdisc audit <image> --against <cdrom.dat> [--disc N] [--only C1,C2]\n"
               "       ocdisc drives\n");
  return 2;
}

// "D:" or "D:\": a drive letter rather than a file.
bool is_drive_name(const std::string& path) {
  return (path.size() == 2 || (path.size() == 3 && (path[2] == '\\' || path[2] == '/'))) && std::isalpha(static_cast<unsigned char>(path[0])) &&
         path[1] == ':';
}

std::unique_ptr<SectorSource> open_disc(const std::string& path, size_t disc) {
  std::string error;
  std::unique_ptr<SectorSource> source;
  if (is_drive_name(path)) source = open_optical_drive(path, &error);
  else source = open_image(std::filesystem::path(path), &error, disc - 1);
  if (!source) std::fprintf(stderr, "ocdisc: %s\n", error.c_str());
  return source;
}

std::optional<FileTable> open_table(const SectorSource& source) {
  std::string error;
  std::optional<FileTable> table = FileTable::read(source, &error);
  if (!table) std::fprintf(stderr, "ocdisc: %s\n", error.c_str());
  return table;
}

void print_identity(const DiscIdentity& id) {
  std::printf("    volume %s, boot %s, exe sha256 %s\n", id.volume_id.c_str(), id.boot_file.c_str(), id.exe_sha256.c_str());
  if (!id.chrono_cross) std::printf("    not Chrono Cross: %s\n", id.reason.c_str());
  else if (id.known_build) std::printf("    Chrono Cross disc %u, build %s, %u file records\n", id.disc_number, id.build.c_str(), id.file_records);
  else
    std::printf("    Chrono Cross disc %u, unknown build, closest region %s, %u file records\n", id.disc_number, region_name(id.region),
                id.file_records);
}

int identify_command(const std::string& path) {
  const std::filesystem::path file(path);
  std::printf("%s: %s\n", path.c_str(), format_name(detect_format(file)));
  DiscSet set = open_disc_set(file);
  if (set.discs.empty()) {
    std::printf("  error: %s\n", set.error.c_str());
    return 1;
  }
  int failures = 0;
  for (size_t i = 0; i < set.discs.size(); ++i) {
    const SectorSource& source = *set.discs[i];
    const DiscIdentity id = identify(source);
    std::printf("  image disc %zu of %zu: %s, %u sectors\n", i + 1, set.discs.size(), source.description().c_str(), source.sector_count());
    print_identity(id);
    if (!id.chrono_cross) ++failures;
  }
  return failures == 0 ? 0 : 1;
}

int drives_command() {
  const std::vector<DriveCandidate> drives = probe_optical_drives();
  if (drives.empty()) {
    std::printf("no optical drives\n");
    return 1;
  }
  size_t found = 0;
  for (const DriveCandidate& drive : drives) {
    std::printf("%s %s: %s%s%s\n", drive.drive.c_str(), drive.product.empty() ? "(unknown device)" : drive.product.c_str(),
                drive.media ? "disc" : "no disc", drive.label.empty() ? "" : ", label ", drive.label.c_str());
    if (!drive.source) {
      if (drive.media) std::printf("  error: %s\n", drive.error.c_str());
      continue;
    }
    std::printf("  %s, %u sectors\n", drive.source->description().c_str(), drive.source->sector_count());
    print_identity(drive.identity);
    if (drive.identity.chrono_cross) ++found;
  }
  std::printf("%zu drive(s), %zu with a Chrono Cross disc\n", drives.size(), found);
  return found > 0 ? 0 : 1;
}

int ls_command(const Options& options) {
  const std::unique_ptr<SectorSource> source = open_disc(options.positional[0], options.disc);
  if (!source) return 1;
  const std::optional<FileTable> table = open_table(*source);
  if (!table) return 1;
  std::printf("record      lba       size  empty\n");
  for (size_t i = options.from; i < table->size() && i - options.from < options.count; ++i) {
    const FileSpan& span = table->span(i);
    std::printf("%6zu %8u %10u  %s\n", i, span.lba, span.size, span.empty ? "yes" : "no");
  }
  return 0;
}

int extract_command(const Options& options) {
  if (options.positional.size() < 3) return usage();
  const std::unique_ptr<SectorSource> source = open_disc(options.positional[0], options.disc);
  if (!source) return 1;
  const std::optional<FileTable> table = open_table(*source);
  if (!table) return 1;
  const size_t record = std::strtoull(options.positional[1].c_str(), nullptr, 10);
  const std::optional<Bytes> bytes = read_record(*source, *table, record);
  if (!bytes) {
    std::fprintf(stderr, "ocdisc: record %zu cannot be read (the table has %zu)\n", record, table->size());
    return 1;
  }
  std::ofstream out(std::filesystem::path(options.positional[2]), std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
  if (!out) {
    std::fprintf(stderr, "ocdisc: cannot write %s\n", options.positional[2].c_str());
    return 1;
  }
  std::printf("record %zu: %zu bytes -> %s\n", record, bytes->size(), options.positional[2].c_str());
  return 0;
}

// The .fnd record as a cdrom.dat member name, doubled separators collapsed ("" when the record names no file under the cdrom
// tree).
std::string member_of(std::string_view record, bool& outside) {
  std::string name;
  for (const char ch : record) {
    const char mapped = ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (mapped != '/' || !name.ends_with('/')) name += mapped;
  }
  constexpr std::string_view kRoot = "c:/work/cdrom/";
  outside = !name.empty() && !name.starts_with(kRoot);
  return outside || name.empty() ? std::string() : name.substr(kRoot.size());
}

// Equal, or equal up to the shorter length with the longer one less than a sector longer (trailing sector padding).
bool same_file(const Bytes& disc, const Bytes& member) {
  const size_t shorter = std::min(disc.size(), member.size());
  const size_t longer = std::max(disc.size(), member.size());
  return longer - shorter < kUserSectorSize && std::equal(disc.begin(), disc.begin() + static_cast<std::ptrdiff_t>(shorter), member.begin());
}

int verify_command(const Options& options) {
  if (options.against.empty()) return usage();
  const std::unique_ptr<SectorSource> source = open_disc(options.positional[0], options.disc);
  if (!source) return 1;
  const std::optional<FileTable> table = open_table(*source);
  if (!table) return 1;
  const u32 disc_number = read_disc_number(*source).value_or(0);
  const vfs::Archive archive{std::filesystem::path(options.against)};
  if (!archive.ok()) {
    std::fprintf(stderr, "ocdisc: cannot open %s\n", options.against.c_str());
    return 1;
  }
  const std::string fnd_name = disc_number == 2 ? "cdrom2.fnd" : "cdrom.fnd";
  const std::optional<Bytes> fnd = archive.read(fnd_name);
  if (!fnd) {
    std::fprintf(stderr, "ocdisc: %s has no %s\n", options.against.c_str(), fnd_name.c_str());
    return 1;
  }
  constexpr size_t kRecordSize = 0x40;
  size_t identical = 0, padded = 0, empty_slots = 0, unnamed = 0, outside = 0, missing = 0;
  std::vector<std::string> differ, missing_names;
  for (size_t i = 0; i < table->size(); ++i) {
    const FileSpan& span = table->span(i);
    std::string_view record;
    if ((i + 1) * kRecordSize <= fnd->size()) {
      const char* text = reinterpret_cast<const char*>(fnd->data() + i * kRecordSize);
      record = std::string_view(text, strnlen(text, kRecordSize));
    }
    bool other_tree = false;
    const std::string member = member_of(record, other_tree);
    if (span.empty) {
      ++empty_slots;
      continue;
    }
    if (other_tree) {
      ++outside;
      continue;
    }
    if (member.empty()) {
      ++unnamed;
      std::printf("unnamed record %zu: %u bytes at LBA %u\n", i, span.size, span.lba);
      continue;
    }
    if (!archive.contains(member)) {
      ++missing;
      missing_names.push_back(std::to_string(i) + " " + member);
      continue;
    }
    const std::optional<Bytes> disc_bytes = read_record(*source, *table, i);
    const std::optional<Bytes> member_bytes = archive.read(member);
    if (!disc_bytes || !member_bytes || !same_file(*disc_bytes, *member_bytes)) {
      differ.push_back(std::to_string(i) + " " + member);
      continue;
    }
    ++identical;
    if (disc_bytes->size() != member_bytes->size()) ++padded;
  }
  for (const std::string& name : differ) std::printf("differs: %s\n", name.c_str());
  for (const std::string& name : missing_names) std::printf("missing: %s\n", name.c_str());
  std::printf("disc %u against %s (%s): %zu records\n", disc_number, options.against.c_str(), fnd_name.c_str(), table->size());
  std::printf("  identical %zu (%zu with trailing padding), differ %zu, missing %zu, unnamed %zu, outside cdrom.dat %zu, empty slots %zu\n",
              identical, padded, differ.size(), missing, unnamed, outside, empty_slots);
  return differ.empty() && missing == 0 ? 0 : 1;
}

}  // namespace

int ocdisc_main(int argc, char** argv) {
  Options options;
  if (argc == 2 && std::string_view(argv[1]) == "drives") return drives_command();
  if (argc < 3 || !parse(argc, argv, options) || options.positional.empty()) return usage();
  const std::string command = argv[1];
  if (command == "identify") return identify_command(options.positional[0]);
  if (command == "ls") return ls_command(options);
  if (command == "extract") return extract_command(options);
  if (command == "verify") return verify_command(options);
  if (command == "audit") return options.against.empty() ? usage() : ocdisc_audit(options.positional[0], options.against, options.disc, options.only);
  return usage();
}
