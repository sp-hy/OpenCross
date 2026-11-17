// CHD CD images. A CD CHD stores the disc as 2448-byte frames (2352 sector bytes + 96 subchannel bytes), eight to a hunk, the tracks
// back to back with each padded to a multiple of four frames. The track list is in CHT2 / CHTR metadata strings. Hunks are decoded by
// libchdr (the cd* codecs) and the last few are kept; a track's virtual pregap (one the file does not store) reads as zeros and
// takes up disc LBAs, as a cue sheet's PREGAP does.
#include "import/disc/chd_source.h"

#include <libchdr/chd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace oc::import::disc {

namespace {

constexpr size_t kFrameSize = kRawSectorSize + kSubchannelSize;
constexpr u32 kTrackPadding = 4;
constexpr size_t kCachedHunks = 8;
constexpr u32 kMaxTracks = 99;
constexpr u32 kGapFrame = ~0u;
constexpr size_t kMode2SectorSize = 2336;
constexpr u32 kLeadIn = 150;

enum class Layout {
  Cooked,
  Raw,
  Mode2,
};

struct Run {
  u32 first_lba = 0;
  u32 count = 0;
  u32 frame_start = kGapFrame;
  Layout layout = Layout::Raw;
};

struct Hunk {
  u32 number = ~0u;
  u64 used = 0;
  std::vector<u8> data;
};

struct TrackInfo {
  Layout layout = Layout::Raw;
  u32 frames = 0;
  u32 pregap = 0;
  bool pregap_stored = false;
};

bool layout_of(const std::string& type, Layout& layout) {
  if (type == "MODE1" || type == "MODE1/2048" || type == "MODE2_FORM1" || type == "MODE2/2048") {
    layout = Layout::Cooked;
  } else if (type == "MODE1_RAW" || type == "MODE1/2352" || type == "MODE2_RAW" || type == "MODE2/2352" || type == "AUDIO") {
    layout = Layout::Raw;
  } else if (type == "MODE2" || type == "MODE2/2336" || type == "MODE2_FORM_MIX") {
    layout = Layout::Mode2;
  } else {
    return false;
  }
  return true;
}

u8 bcd(u32 value) { return static_cast<u8>((value / 10) << 4 | value % 10); }

// A full sector built around a mode-2 payload that the image stores without sync and header.
void synthesize_mode2(u32 lba, const u8* payload, u8* sector) {
  static constexpr u8 kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  const u32 at = lba + kLeadIn;
  std::memcpy(sector, kSync, sizeof(kSync));
  sector[12] = bcd(at / (60 * 75));
  sector[13] = bcd(at / 75 % 60);
  sector[14] = bcd(at % 75);
  sector[15] = 2;
  std::memcpy(sector + 16, payload, kMode2SectorSize);
}

class ChdSource final : public SectorSource {
 public:
  ChdSource(FILE* file, chd_file* chd, u32 frames_per_hunk, std::vector<Run> runs, u32 sector_count, bool raw, std::string description)
      : SectorSource(sector_count, raw, std::move(description)),
        file_(file),
        chd_(chd),
        frames_per_hunk_(frames_per_hunk),
        runs_(std::move(runs)),
        scratch_(frames_per_hunk * kFrameSize) {}

  ~ChdSource() override {
    chd_close(chd_);
    std::fclose(file_);
  }

 protected:
  bool fetch(u32 lba, u32 count, u8* out) const override {
    const size_t out_size = raw() ? kRawSectorSize : kUserSectorSize;
    for (; count > 0; ++lba, --count, out += out_size) {
      const auto next = std::upper_bound(runs_.begin(), runs_.end(), lba, [](u32 at, const Run& run) { return at < run.first_lba; });
      const Run& run = *(next - 1);
      if (run.frame_start == kGapFrame) {
        std::memset(out, 0, out_size);
        continue;
      }
      const u8* frame = frame_data(run.frame_start + lba - run.first_lba);
      if (!frame) return false;
      convert(run.layout, lba, frame, out);
    }
    return true;
  }

 private:
  void convert(Layout layout, u32 lba, const u8* frame, u8* out) const {
    if (layout == Layout::Cooked) {
      std::memcpy(out, frame, kUserSectorSize);
      return;
    }
    u8 built[kRawSectorSize];
    const u8* sector = frame;
    if (layout == Layout::Mode2) {
      synthesize_mode2(lba, frame, built);
      sector = built;
    }
    if (raw()) {
      std::memcpy(out, sector, kRawSectorSize);
      return;
    }
    size_t at = 0;
    if (has_sync(sector)) at = sector[15] == 1 ? kMode1PayloadOffset : kMode2PayloadOffset;
    std::memcpy(out, sector + at, kUserSectorSize);
  }

  const u8* frame_data(u32 frame) const {
    const u32 number = frame / frames_per_hunk_;
    Hunk* hunk = nullptr;
    for (Hunk& cached : cache_) {
      if (cached.number == number) {
        hunk = &cached;
        break;
      }
    }
    if (!hunk) {
      if (cache_.size() < kCachedHunks) {
        cache_.emplace_back();
        hunk = &cache_.back();
      } else {
        hunk = &*std::min_element(cache_.begin(), cache_.end(), [](const Hunk& a, const Hunk& b) { return a.used < b.used; });
      }
      hunk->number = ~0u;
      if (chd_read(chd_, number, scratch_.data()) != CHDERR_NONE) return nullptr;
      hunk->data = scratch_;
      hunk->number = number;
    }
    hunk->used = ++clock_;
    return hunk->data.data() + static_cast<size_t>(frame % frames_per_hunk_) * kFrameSize;
  }

  FILE* file_;
  chd_file* chd_;
  u32 frames_per_hunk_;
  std::vector<Run> runs_;
  mutable std::vector<Hunk> cache_;
  mutable std::vector<u8> scratch_;
  mutable u64 clock_ = 0;
};

FILE* open_stdio(const std::filesystem::path& path) {
#ifdef _WIN32
  return _wfopen(path.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

// The CHT2 / CHTR track strings in track order; false when there are none or one cannot be read.
bool read_tracks(chd_file* chd, std::vector<TrackInfo>& tracks, std::string* error) {
  for (u32 index = 0; index < kMaxTracks + 1; ++index) {
    char text[512];
    u32 length = 0;
    chd_error err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, text, sizeof(text) - 1, &length, nullptr, nullptr);
    const bool v2 = err == CHDERR_NONE;
    if (!v2) err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, text, sizeof(text) - 1, &length, nullptr, nullptr);
    if (err != CHDERR_NONE) break;
    text[std::min<u32>(length, sizeof(text) - 1)] = 0;
    char type[32] = {}, subtype[32] = {}, pgtype[32] = {}, pgsub[32] = {};
    u32 number = 0, frames = 0, pregap = 0, postgap = 0;
    const int fields = v2 ? std::sscanf(text, "TRACK:%u TYPE:%31s SUBTYPE:%31s FRAMES:%u PREGAP:%u PGTYPE:%31s PGSUB:%31s POSTGAP:%u", &number,
                                        type, subtype, &frames, &pregap, pgtype, pgsub, &postgap)
                          : std::sscanf(text, "TRACK:%u TYPE:%31s SUBTYPE:%31s FRAMES:%u", &number, type, subtype, &frames);
    if (fields != (v2 ? 8 : 4) || number != index + 1 || frames == 0) {
      *error = "CHD: unreadable track description: " + std::string(text);
      return false;
    }
    TrackInfo track;
    if (!layout_of(type, track.layout)) {
      *error = "CHD: unsupported track type " + std::string(type);
      return false;
    }
    track.frames = frames;
    track.pregap = pregap;
    track.pregap_stored = pgtype[0] == 'V';
    tracks.push_back(track);
  }
  if (tracks.empty()) *error = "CHD: no CD track metadata (not a CD image, or an old CHCD file)";
  return !tracks.empty();
}

}  // namespace

std::unique_ptr<SectorSource> open_chd(const std::filesystem::path& path, std::string* error) {
  std::string scratch_error;
  if (!error) error = &scratch_error;
  FILE* file = open_stdio(path);
  if (!file) {
    *error = "cannot open " + path.string();
    return nullptr;
  }
  chd_file* chd = nullptr;
  const chd_error err = chd_open_file(file, CHD_OPEN_READ, nullptr, &chd);
  if (err != CHDERR_NONE) {
    std::fclose(file);
    *error = std::string("CHD: ") + chd_error_string(err) + (err == CHDERR_REQUIRES_PARENT ? " (child images are not supported)" : "");
    return nullptr;
  }
  const auto fail = [&](std::string message) -> std::unique_ptr<SectorSource> {
    chd_close(chd);
    std::fclose(file);
    *error = std::move(message);
    return nullptr;
  };
  const chd_header* header = chd_get_header(chd);
  if (header->hunkbytes == 0 || header->hunkbytes % kFrameSize != 0) return fail("CHD: not a CD image (hunk size " + std::to_string(header->hunkbytes) + ")");
  const u32 frames_per_hunk = header->hunkbytes / static_cast<u32>(kFrameSize);
  const u64 stored_frames = static_cast<u64>(header->totalhunks) * frames_per_hunk;

  std::vector<TrackInfo> tracks;
  if (!read_tracks(chd, tracks, error)) return fail(*error);

  std::vector<Run> runs;
  u64 lba = 0;
  u64 frame = 0;
  bool raw = true;
  for (const TrackInfo& track : tracks) {
    if (!track.pregap_stored && track.pregap > 0) runs.push_back({static_cast<u32>(lba), track.pregap, kGapFrame, track.layout});
    if (!track.pregap_stored) lba += track.pregap;
    if (frame + track.frames > stored_frames) return fail("CHD: a track runs past the end of the data");
    runs.push_back({static_cast<u32>(lba), track.frames, static_cast<u32>(frame), track.layout});
    lba += track.frames;
    frame += (track.frames + kTrackPadding - 1) / kTrackPadding * kTrackPadding;
    if (track.layout == Layout::Cooked) raw = false;
  }
  if (lba > 0xFFFFFFFFull) return fail("CHD: too many sectors");
  std::string description = std::string("chd, ") + (raw ? "raw 2352" : "cooked 2048 (no form-2 data)");
  return std::make_unique<ChdSource>(file, chd, frames_per_hunk, std::move(runs), static_cast<u32>(lba), raw, std::move(description));
}

}  // namespace oc::import::disc
