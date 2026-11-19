#pragma once
// Music resume ([Audio] MusicResume): where each song that was cut off stopped, so that the next start of the same song within
// kSongResumeSeconds continues from there instead of from its start. One place per song; a place is used up when it is taken.
// Times are seconds of the caller's clock (the mixer's rendered audio: real time while an output device runs).
#include <memory>
#include <optional>
#include <vector>

#include "audio/akao_player.h"

namespace oc::audio {

constexpr double kSongResumeSeconds = 300.0;

// Under kSongResumeSeconds since the cut (a clock that went backwards counts as stale).
constexpr bool song_resume_fresh(double cut_at, double now) { return now >= cut_at && now - cut_at < kSongResumeSeconds; }

struct SongPlace {
  // The source the song played on: the AKAO sequencer (`context`) or the remastered stream (`stream_seconds`, its position wrapped at
  // the loop, in seconds of the stream's own rate).
  bool akao = false;
  double stream_seconds = 0;
  // Seconds since the song start, unwrapped across loops.
  double song_seconds = 0;
  std::shared_ptr<const AkaoPlayer::MusicContext> context;
};

class SongMemory {
 public:
  // Replaces the song's place; places older than kSongResumeSeconds are dropped.
  void remember(int song, SongPlace place, double now) {
    std::erase_if(entries_, [&](const Entry& entry) { return entry.song == song || !song_resume_fresh(entry.cut_at, now); });
    entries_.push_back({song, now, std::move(place)});
  }
  // The place of `song` on the given source when it was cut less than kSongResumeSeconds ago. The song's place is dropped either way.
  std::optional<SongPlace> take(int song, bool akao, double now) {
    std::optional<SongPlace> found;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->song != song) continue;
      if (it->place.akao == akao && song_resume_fresh(it->cut_at, now)) found = std::move(it->place);
      entries_.erase(it);
      break;
    }
    return found;
  }
  void forget(int song) {
    std::erase_if(entries_, [&](const Entry& entry) { return entry.song == song; });
  }
  void clear() { entries_.clear(); }
  bool empty() const { return entries_.empty(); }

 private:
  struct Entry {
    int song;
    double cut_at;
    SongPlace place;
  };
  std::vector<Entry> entries_;
};

}  // namespace oc::audio
