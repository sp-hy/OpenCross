// Container recognition and the file-backed sector source: cue sheets, raw and cooked images, CloneCD, Alcohol MDS and playlists.
// PBP lives in pbp.cpp, CHD in chd_source.cpp.
#include "import/disc/disc_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#include "import/disc/chd_source.h"
#include "import/disc/image_files.h"

namespace oc::import::disc {

namespace fs = std::filesystem;

namespace {

constexpr u32 kSubchannelStride = kRawSectorSize + kSubchannelSize;
constexpr size_t kSniffSize = 0x8800;
constexpr u64 kMaxTextSize = 1u << 20;

class FileSource final : public SectorSource {
 public:
  FileSource(std::vector<std::ifstream> streams, std::vector<Extent> extents, u32 sector_count, bool raw, std::string description)
      : SectorSource(sector_count, raw, std::move(description)), streams_(std::move(streams)), extents_(std::move(extents)) {}

 protected:
  bool fetch(u32 lba, u32 count, u8* out) const override {
    const size_t out_size = raw() ? kRawSectorSize : kUserSectorSize;
    while (count > 0) {
      const auto next = std::upper_bound(extents_.begin(), extents_.end(), lba, [](u32 at, const Extent& ext) { return at < ext.first_lba; });
      const Extent& ext = *(next - 1);
      const u32 run = std::min(count, ext.first_lba + ext.count - lba);
      if (ext.file < 0) {
        std::memset(out, 0, run * out_size);
      } else if (!read_run(ext, lba, run, out, out_size)) {
        return false;
      }
      out += run * out_size;
      lba += run;
      count -= run;
    }
    return true;
  }

 private:
  bool read_run(const Extent& ext, u32 lba, u32 run, u8* out, size_t out_size) const {
    std::ifstream& stream = streams_[static_cast<size_t>(ext.file)];
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(ext.offset + static_cast<u64>(lba - ext.first_lba) * ext.stride));
    const size_t bytes = static_cast<size_t>(run) * ext.stride;
    if (ext.stride == out_size) return static_cast<bool>(stream.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(bytes)));
    scratch_.resize(bytes);
    if (!stream.read(reinterpret_cast<char*>(scratch_.data()), static_cast<std::streamsize>(bytes))) return false;
    for (u32 i = 0; i < run; ++i) {
      const u8* sector = scratch_.data() + static_cast<size_t>(i) * ext.stride;
      size_t at = 0;
      if (out_size == kUserSectorSize) at = has_sync(sector) && sector[15] == 1 ? kMode1PayloadOffset : kMode2PayloadOffset;
      std::memcpy(out + i * out_size, sector + at, out_size);
    }
    return true;
  }

  mutable std::vector<std::ifstream> streams_;
  std::vector<Extent> extents_;
  mutable Bytes scratch_;
};

u64 size_of_file(const fs::path& path) {
  std::error_code ec;
  const u64 size = fs::file_size(path, ec);
  return ec ? 0 : size;
}

fs::path utf8_path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string lower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

std::string read_text(const fs::path& path, std::string* error) {
  const u64 size = size_of_file(path);
  Bytes bytes;
  if (size > kMaxTextSize || !read_file_range(path, 0, static_cast<size_t>(size), bytes)) {
    if (error) *error = "cannot read " + path.string();
    return {};
  }
  std::string text(bytes.begin(), bytes.end());
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  return text;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    lines.push_back(first == std::string::npos ? std::string() : line.substr(first));
    start = end + 1;
  }
  return lines;
}

// Whitespace-separated words; a double-quoted word may hold spaces.
std::vector<std::string> split_words(const std::string& line) {
  std::vector<std::string> words;
  size_t at = 0;
  while (at < line.size()) {
    if (line[at] == ' ' || line[at] == '\t') {
      ++at;
      continue;
    }
    if (line[at] == '"') {
      const size_t end = line.find('"', at + 1);
      words.push_back(line.substr(at + 1, (end == std::string::npos ? line.size() : end) - at - 1));
      at = end == std::string::npos ? line.size() : end + 1;
      continue;
    }
    const size_t end = line.find_first_of(" \t", at);
    words.push_back(line.substr(at, (end == std::string::npos ? line.size() : end) - at));
    at = end == std::string::npos ? line.size() : end;
  }
  return words;
}

// "mm:ss:ff" as a frame (sector) count, or -1.
long parse_msf(const std::string& text) {
  int minutes = 0, seconds = 0, frames = 0;
  char tail = 0;
  if (std::sscanf(text.c_str(), "%d:%d:%d%c", &minutes, &seconds, &frames, &tail) != 3) return -1;
  if (minutes < 0 || seconds < 0 || seconds >= 60 || frames < 0 || frames >= 75) return -1;
  return (minutes * 60L + seconds) * 75L + frames;
}

bool is_text(ByteSpan head) {
  for (const u8 byte : head)
    if (byte == 0) return false;
  return true;
}

DiscSet failure(std::string error) {
  DiscSet set;
  set.error = std::move(error);
  return set;
}

DiscSet single(std::unique_ptr<SectorSource> source, std::string error) {
  DiscSet set;
  if (source) set.discs.push_back(std::move(source));
  else set.error = std::move(error);
  return set;
}

struct CueTrack {
  u32 stride = 0;
  long pregap = 0;
  long start = -1;
};
struct CueFile {
  fs::path path;
  std::vector<CueTrack> tracks;
};

u32 cue_stride(const std::string& mode) {
  const std::string key = lower(mode);
  if (key == "audio" || key == "mode1/2352" || key == "mode2/2352") return kRawSectorSize;
  if (key == "mode1/2048" || key == "mode2/2048") return kUserSectorSize;
  return 0;
}

DiscSet open_cue(const fs::path& path) {
  std::string error;
  const std::string text = read_text(path, &error);
  if (!error.empty()) return failure(error);
  std::vector<CueFile> files;
  for (const std::string& line : split_lines(text)) {
    const std::vector<std::string> words = split_words(line);
    if (words.empty()) continue;
    const std::string keyword = lower(words[0]);
    if (keyword == "file") {
      if (words.size() < 3) return failure("cue: FILE line without a name and type: " + line);
      const std::string type = lower(words.back());
      if (type != "binary") return failure("cue: only BINARY files are supported, not " + words.back());
      std::string name = words[1];
      for (size_t i = 2; i + 1 < words.size(); ++i) name += " " + words[i];
      files.push_back({path.parent_path() / utf8_path(name), {}});
    } else if (keyword == "track") {
      if (files.empty() || words.size() < 3) return failure("cue: TRACK before FILE: " + line);
      const u32 stride = cue_stride(words[2]);
      if (stride == 0) return failure("cue: unsupported track mode " + words[2]);
      files.back().tracks.push_back({stride, 0, -1});
    } else if (keyword == "index" || keyword == "pregap") {
      if (files.empty() || files.back().tracks.empty()) return failure("cue: " + words[0] + " outside a track");
      const long frames = parse_msf(words.back());
      if (frames < 0) return failure("cue: bad time in: " + line);
      CueTrack& track = files.back().tracks.back();
      if (keyword == "pregap") track.pregap = frames;
      else if (track.start < 0) track.start = frames;
    }
  }
  std::vector<fs::path> paths;
  std::vector<Extent> extents;
  u32 lba = 0;
  for (const CueFile& file : files) {
    const u64 size = size_of_file(file.path);
    if (size == 0) return failure("cue: cannot open " + file.path.string());
    const int index = static_cast<int>(paths.size());
    paths.push_back(file.path);
    u64 offset = 0;
    for (size_t i = 0; i < file.tracks.size(); ++i) {
      const CueTrack& track = file.tracks[i];
      if (track.start < 0) return failure("cue: a track has no INDEX");
      if (track.pregap > 0) {
        extents.push_back({-1, 0, kRawSectorSize, lba, static_cast<u32>(track.pregap)});
        lba += static_cast<u32>(track.pregap);
      }
      u64 count = 0;
      if (i + 1 < file.tracks.size()) {
        if (file.tracks[i + 1].start < track.start) return failure("cue: track indexes go backwards");
        count = static_cast<u64>(file.tracks[i + 1].start - track.start);
      } else {
        count = offset < size ? (size - offset) / track.stride : 0;
      }
      if (count > 0) extents.push_back({index, offset, track.stride, lba, static_cast<u32>(count)});
      offset += count * track.stride;
      lba += static_cast<u32>(count);
    }
  }
  if (extents.empty()) return failure("cue: no tracks");
  std::unique_ptr<SectorSource> source = make_file_source(paths, std::move(extents), "cue", &error);
  return single(std::move(source), error);
}

u32 raw_stride(ByteSpan head) {
  if (head.size() >= kRawSectorSize + 12 && has_sync(head.data() + kRawSectorSize)) return kRawSectorSize;
  if (head.size() >= kSubchannelStride + 12 && has_sync(head.data() + kSubchannelStride)) return kSubchannelStride;
  return kRawSectorSize;
}

DiscSet open_plain(const fs::path& path, u32 stride, const std::string& container) {
  const u64 count = size_of_file(path) / stride;
  if (count == 0) return failure(container + ": empty image " + path.string());
  std::string error;
  std::unique_ptr<SectorSource> source = make_file_source({path}, {{0, 0, stride, 0, static_cast<u32>(count)}}, container, &error);
  return single(std::move(source), error);
}

struct CcdEntry {
  long point = -1;
  long control = 0;
  long plba = 0;
};

// The CloneCD control file: "[Disc]" gives the session count and whether data tracks are scrambled, each "[Entry n]" one table of
// contents row (point 1..99 a track start, 0xA2 the lead-out; PLBA counts from disc time 00:02:00 like an LBA).
struct CcdToc {
  long sessions = 1;
  long scrambled = 0;
  std::vector<CcdEntry> entries;
};

CcdToc parse_ccd(const std::string& text) {
  CcdToc toc;
  std::string section;
  for (const std::string& line : split_lines(text)) {
    if (line.empty()) continue;
    if (line.front() == '[') {
      section = lower(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1));
      if (section.starts_with("entry")) toc.entries.emplace_back();
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = lower(line.substr(0, eq));
    const long value = std::strtol(line.c_str() + eq + 1, nullptr, 0);
    if (section == "disc") {
      if (key == "sessions") toc.sessions = value;
      else if (key == "datatracksscrambled") toc.scrambled = value;
    } else if (section.starts_with("entry")) {
      if (key == "point") toc.entries.back().point = value;
      else if (key == "control") toc.entries.back().control = value;
      else if (key == "plba") toc.entries.back().plba = value;
    }
  }
  return toc;
}

DiscSet open_clonecd(const fs::path& path) {
  std::string error;
  const std::string text = read_text(path, &error);
  if (!error.empty()) return failure(error);
  const CcdToc toc = parse_ccd(text);
  if (toc.sessions > 1) return failure("ccd: multi-session images are not supported");
  if (toc.scrambled != 0) return failure("ccd: scrambled data tracks are not supported");
  fs::path image = path;
  image.replace_extension(".img");
  const u64 available = size_of_file(image) / kRawSectorSize;
  if (available == 0) return failure("ccd: image file not found: " + image.string());
  u64 count = available;
  long previous = -1;
  bool tracks = false;
  for (const CcdEntry& entry : toc.entries) {
    if (entry.point >= 1 && entry.point <= 99) {
      if (entry.plba < 0 || entry.plba <= previous) return failure("ccd: track starts do not ascend");
      previous = entry.plba;
      tracks = true;
    } else if (entry.point == 0xA2 && entry.plba > 0) {
      if (static_cast<u64>(entry.plba) > available) return failure("ccd: the image is shorter than the lead-out");
      count = static_cast<u64>(entry.plba);
    }
  }
  if (!toc.entries.empty() && !tracks) return failure("ccd: no tracks");
  fs::path sub = path;
  sub.replace_extension(".sub");
  std::unique_ptr<SectorSource> source = make_file_source({image}, {{0, 0, kRawSectorSize, 0, static_cast<u32>(count)}},
                                                          size_of_file(sub) > 0 ? "ccd+img+sub" : "ccd+img", &error);
  return single(std::move(source), error);
}

u16 get16(ByteSpan bytes, size_t at) { return static_cast<u16>(bytes[at] | bytes[at + 1] << 8); }
u32 get32(ByteSpan bytes, size_t at) { return get16(bytes, at) | static_cast<u32>(get16(bytes, at + 2)) << 16; }
u64 get64(ByteSpan bytes, size_t at) { return get32(bytes, at) | static_cast<u64>(get32(bytes, at + 4)) << 32; }

// The data file an MDS track block names: "*.mdf" means the descriptor's own name with the .mdf extension.
fs::path mds_data_file(const fs::path& path, ByteSpan mds, u32 footer) {
  fs::path fallback = path;
  fallback.replace_extension(".mdf");
  if (footer == 0 || footer + 8 > mds.size()) return fallback;
  const u32 name_at = get32(mds, footer);
  const bool wide = mds[footer + 4] != 0;
  std::string name;
  for (size_t at = name_at; at < mds.size(); at += wide ? 2 : 1) {
    const u16 ch = wide ? (at + 1 < mds.size() ? get16(mds, at) : 0) : mds[at];
    if (ch == 0) break;
    name += ch < 0x80 ? static_cast<char>(ch) : '_';
  }
  if (name.empty() || name.starts_with("*.")) {
    if (!name.empty()) fallback.replace_extension(name.substr(1));
    return fallback;
  }
  return path.parent_path() / utf8_path(name);
}

// Alcohol 120% descriptor: header, then one session block pointing at 80-byte track blocks. Blocks with a point of 0xA0 and up
// are lead-in entries; tracks 1..99 give the start LBA, the byte offset in the data file and the stored sector size.
DiscSet open_mds(const fs::path& path) {
  Bytes mds;
  const u64 size = size_of_file(path);
  if (size < 0x58 || size > kMaxTextSize || !read_file_range(path, 0, static_cast<size_t>(size), mds)) return failure("mds: cannot read " + path.string());
  const u32 session = get32(mds, 0x50);
  if (session + 0x18 > mds.size()) return failure("mds: no session block");
  const u32 blocks = mds[session + 0x0A];
  const u32 tracks_at = get32(mds, session + 0x14);
  if (tracks_at + blocks * 0x50ull > mds.size()) return failure("mds: track blocks run past the file");
  struct Track {
    u32 lba;
    u64 offset;
    u32 stride;
    u32 length;
    u32 footer;
  };
  std::vector<Track> tracks;
  for (u32 i = 0; i < blocks; ++i) {
    const size_t at = tracks_at + i * 0x50u;
    const u8 point = mds[at + 4];
    if (point == 0 || point > 99) continue;
    Track track{get32(mds, at + 0x24), get64(mds, at + 0x28), get16(mds, at + 0x10), 0, get32(mds, at + 0x34)};
    const u32 extra = get32(mds, at + 0x0C);
    if (extra != 0 && extra + 8 <= mds.size()) track.length = get32(mds, extra + 4);
    if (track.stride != kUserSectorSize && track.stride != kRawSectorSize && track.stride != kSubchannelStride)
      return failure("mds: unsupported sector size " + std::to_string(track.stride));
    tracks.push_back(track);
  }
  if (tracks.empty()) return failure("mds: no tracks");
  std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.lba < b.lba; });
  const fs::path data = mds_data_file(path, mds, tracks[0].footer);
  const u64 data_size = size_of_file(data);
  if (data_size == 0) return failure("mds: data file not found: " + data.string());
  std::vector<Extent> extents;
  u32 lba = 0;
  for (size_t i = 0; i < tracks.size(); ++i) {
    const Track& track = tracks[i];
    if (track.lba > lba) {
      extents.push_back({-1, 0, kRawSectorSize, lba, track.lba - lba});
      lba = track.lba;
    }
    u64 count = track.length;
    if (count == 0) count = i + 1 < tracks.size() ? tracks[i + 1].lba - track.lba : (data_size > track.offset ? (data_size - track.offset) / track.stride : 0);
    if (track.offset + count * track.stride > data_size) return failure("mds: track runs past the data file");
    if (count > 0) extents.push_back({0, track.offset, track.stride, lba, static_cast<u32>(count)});
    lba += static_cast<u32>(count);
  }
  std::string error;
  std::unique_ptr<SectorSource> source = make_file_source({data}, std::move(extents), "mds+mdf", &error);
  return single(std::move(source), error);
}

}  // namespace

bool read_file_range(const fs::path& path, u64 offset, size_t size, Bytes& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return false;
  stream.seekg(static_cast<std::streamoff>(offset));
  out.resize(size);
  return size == 0 || static_cast<bool>(stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)));
}

std::unique_ptr<SectorSource> make_file_source(const std::vector<fs::path>& files, std::vector<Extent> extents, const std::string& container,
                                               std::string* error) {
  std::vector<std::ifstream> streams;
  for (const fs::path& file : files) {
    streams.emplace_back(file, std::ios::binary);
    if (!streams.back()) {
      if (error) *error = container + ": cannot open " + file.string();
      return nullptr;
    }
  }
  bool raw = true;
  bool subchannel = false;
  for (const Extent& ext : extents) {
    if (ext.file >= 0 && ext.stride == kUserSectorSize) raw = false;
    if (ext.stride == kSubchannelStride) subchannel = true;
  }
  const u32 count = extents.back().first_lba + extents.back().count;
  std::string description = container + (raw ? ", raw 2352" : ", cooked 2048 (no form-2 data)");
  if (subchannel) description += " with subchannel";
  return std::make_unique<FileSource>(std::move(streams), std::move(extents), count, raw, std::move(description));
}

const char* format_name(ImageFormat format) {
  switch (format) {
    case ImageFormat::Unknown: return "unknown";
    case ImageFormat::Cue: return "cue sheet";
    case ImageFormat::RawImage: return "raw image";
    case ImageFormat::CookedImage: return "2048-byte image";
    case ImageFormat::CloneCd: return "CloneCD";
    case ImageFormat::Mds: return "Alcohol MDS";
    case ImageFormat::Playlist: return "m3u playlist";
    case ImageFormat::Pbp: return "PBP";
    case ImageFormat::Chd: return "CHD";
  }
  return "unknown";
}

ImageFormat detect_format(const fs::path& path) {
  const u64 size = size_of_file(path);
  Bytes head;
  if (size == 0 || !read_file_range(path, 0, static_cast<size_t>(std::min<u64>(size, kSniffSize)), head)) return ImageFormat::Unknown;
  const auto starts = [&](std::string_view magic) { return head.size() >= magic.size() && std::memcmp(head.data(), magic.data(), magic.size()) == 0; };
  if (starts("MComprHD")) return ImageFormat::Chd;
  if (starts(std::string_view("\0PBP", 4))) return ImageFormat::Pbp;
  if (starts("MEDIA DESCRIPTOR")) return ImageFormat::Mds;
  if (head.size() >= 12 && has_sync(head.data())) return ImageFormat::RawImage;
  if (head.size() >= 0x8006 && std::memcmp(head.data() + 0x8001, "CD001", 5) == 0) return ImageFormat::CookedImage;
  if (size > kMaxTextSize || !is_text(head)) return ImageFormat::Unknown;
  std::string text(head.begin(), head.end());
  if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
  if (text.starts_with("[CloneCD]")) return ImageFormat::CloneCd;
  bool has_file = false, has_track = false;
  for (const std::string& line : split_lines(text)) {
    const std::vector<std::string> words = split_words(line);
    if (words.empty()) continue;
    has_file = has_file || lower(words[0]) == "file";
    has_track = has_track || lower(words[0]) == "track";
  }
  if (has_file && has_track) return ImageFormat::Cue;
  std::string error;
  const std::vector<fs::path> entries = read_playlist(path, &error);
  if (entries.empty()) return ImageFormat::Unknown;
  for (const fs::path& entry : entries)
    if (size_of_file(entry) == 0) return ImageFormat::Unknown;
  return ImageFormat::Playlist;
}

std::vector<fs::path> read_playlist(const fs::path& path, std::string* error) {
  const std::string text = read_text(path, error);
  std::vector<fs::path> out;
  for (const std::string& line : split_lines(text))
    if (!line.empty() && line[0] != '#') out.push_back(path.parent_path() / utf8_path(line));
  return out;
}

DiscSet open_disc_set(const fs::path& path) {
  const ImageFormat format = detect_format(path);
  switch (format) {
    case ImageFormat::Unknown: return failure("not a recognised disc image: " + path.string());
    case ImageFormat::Cue: return open_cue(path);
    case ImageFormat::RawImage: {
      Bytes head;
      read_file_range(path, 0, kSubchannelStride + 12, head);
      return open_plain(path, raw_stride(head), "raw image");
    }
    case ImageFormat::CookedImage: return open_plain(path, kUserSectorSize, "iso");
    case ImageFormat::CloneCd: return open_clonecd(path);
    case ImageFormat::Mds: return open_mds(path);
    case ImageFormat::Playlist: {
      std::string error;
      DiscSet set;
      for (const fs::path& entry : read_playlist(path, &error)) {
        if (detect_format(entry) == ImageFormat::Playlist) return failure("m3u: a playlist inside a playlist: " + entry.string());
        DiscSet part = open_disc_set(entry);
        if (part.discs.empty()) return failure("m3u: " + part.error);
        for (auto& disc : part.discs) set.discs.push_back(std::move(disc));
      }
      if (set.discs.empty()) set.error = error.empty() ? "m3u: empty playlist" : error;
      return set;
    }
    case ImageFormat::Pbp: {
      DiscSet set;
      set.discs = open_pbp(path, &set.error);
      return set;
    }
    case ImageFormat::Chd: {
      DiscSet set;
      if (std::unique_ptr<SectorSource> disc = open_chd(path, &set.error)) set.discs.push_back(std::move(disc));
      return set;
    }
  }
  return failure("not a recognised disc image");
}

std::unique_ptr<SectorSource> open_image(const fs::path& path, std::string* error, size_t index) {
  DiscSet set = open_disc_set(path);
  if (index < set.discs.size()) return std::move(set.discs[index]);
  if (error) *error = set.discs.empty() ? set.error : "the image holds " + std::to_string(set.discs.size()) + " disc(s), not " + std::to_string(index + 1);
  return nullptr;
}

}  // namespace oc::import::disc
