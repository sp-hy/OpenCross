#pragma once
// The Extras pages' link to the host, and its rules that need no screen. oc_ui has no audio, movie or disc-index code: the host lists
// the songs and movies (ExtrasCatalog) and plays them (ExtrasHost); the menu picks what plays next (shuffle_next) and which pictures the
// portrait gallery shows (gallery_faces).
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::ui {

struct ExtrasCatalog {
  struct Song {
    int id = 0;
    // One pass in seconds: to the loop end of a song that loops, to the end of a one-shot song.
    double seconds = 0;
    bool loops = true;
  };
  struct Movie {
    // The unlock key (save::movie_key), the disc (1 / 2) and the movie-table row of that disc that plays the whole movie.
    int key = 0, disc = 1, row = 0;
  };
  std::vector<Song> songs;
  std::vector<Movie> movies;
};

class ExtrasHost {
 public:
  virtual ~ExtrasHost() = default;
  // The jukebox's song in place of the field's music, on the remastered (true) or the original source; false when it cannot be loaded.
  virtual bool play_song(int song, bool remastered) = 0;
  // The playing jukebox song goes on from the same place on the other source.
  virtual void switch_song_source(bool remastered) = 0;
  // The jukebox's song stops and the field's own music comes back.
  virtual void stop_song() = 0;
  // A theatre entry, full screen; the menu stands still until it ends, then the field's own music comes back. False when it cannot open.
  virtual bool play_movie(int disc, int row) = 0;
  // The settings' music source ([Audio] BgmSource): the jukebox starts on it.
  virtual bool remastered_music() const = 0;
};

// The next shuffled song: a random pick among `songs` other than `current` (the one song again when it is the only one; -1 with none).
// `state` is the xorshift32 state (a 0 state is taken as 1).
int shuffle_next(const std::vector<int>& songs, int current, u32& state);

// One gallery entry per portrait picture: its name (hd/face/<set>/<art>.png) and the dialogue face ids that show it, in id order.
struct GalleryFace {
  std::string art;
  std::vector<int> ids;
};
std::vector<GalleryFace> gallery_faces();

}  // namespace oc::ui
