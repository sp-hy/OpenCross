// Checks for oc_movie: member resolution, Bink 2 decode of the first frames/audio (needs the RDE install; skipped
// without it), the mixer's movie PCM clock and level, and wall-clock Player pacing without a GPU.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "audio/mixer.h"
#include "movie/decoder.h"
#include "movie/player.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// The movie PCM clock stands still (and renders silence) until movie_run(), then counts the frames consumed; the ring empties to silence.
void test_mixer_movie_clock() {
  audio::Mixer mixer;
  mixer.movie_start();
  std::vector<float> pcm(2 * 1000, 0.5f);
  CHECK(mixer.movie_push(pcm.data(), 1000) == 1000);
  std::vector<float> out(2 * 600);
  mixer.render(out.data(), 600);
  CHECK(mixer.movie_frames_played() == 0);
  mixer.movie_run();
  mixer.render(out.data(), 600);
  CHECK(mixer.movie_frames_played() == 600);
  CHECK(std::fabs(out[0] - 0.5f) < 1e-6f);
  mixer.render(out.data(), 600);
  CHECK(mixer.movie_frames_played() == 1000);
  CHECK(out[2 * 400] == 0.0f);
}

// The movie table row's volume scales the movie's sound: level 80 / 127 plays at that fraction, level 0 (the silent rows) plays nothing
// while the clock still runs, and the next movie_start without a level is back at full.
void test_mixer_movie_level() {
  audio::Mixer mixer;
  std::vector<float> pcm(2 * 100, 0.5f);
  std::vector<float> out(2 * 100);
  mixer.movie_start(80.0f / 127.0f);
  CHECK(mixer.movie_push(pcm.data(), 100) == 100);
  mixer.movie_run();
  mixer.render(out.data(), 100);
  CHECK(std::fabs(out[0] - 0.5f * 80.0f / 127.0f) < 1e-6f);
  mixer.movie_start(0.0f);
  CHECK(mixer.movie_push(pcm.data(), 100) == 100);
  mixer.movie_run();
  mixer.render(out.data(), 100);
  CHECK(out[0] == 0.0f && mixer.movie_frames_played() == 100);
  mixer.movie_start();
  CHECK(mixer.movie_push(pcm.data(), 100) == 100);
  mixer.movie_run();
  mixer.render(out.data(), 100);
  CHECK(std::fabs(out[0] - 0.5f) < 1e-6f);
}

// Pause screen: Mixer::hold freezes the movie PCM ring and its clock (silent output, SFX engine untouched); release continues exactly there.
void test_mixer_hold() {
  audio::Mixer mixer;
  mixer.movie_start();
  std::vector<float> pcm(2 * 1000, 0.5f);
  CHECK(mixer.movie_push(pcm.data(), 1000) == 1000);
  mixer.movie_run();
  std::vector<float> out(2 * 300, 1.0f);
  mixer.render(out.data(), 300);
  CHECK(mixer.movie_frames_played() == 300);
  mixer.hold(true);
  CHECK(mixer.held());
  mixer.render(out.data(), 300);
  CHECK(mixer.movie_frames_played() == 300);
  CHECK(out[0] == 0.0f && out[599] == 0.0f);
  mixer.render(out.data(), 300);
  CHECK(mixer.movie_frames_played() == 300);
  mixer.hold(false);
  mixer.render(out.data(), 300);
  CHECK(mixer.movie_frames_played() == 600);
  CHECK(std::fabs(out[0] - 0.5f) < 1e-6f);
}

// The pause flag round-trips and an update() with no movie open leaves the clock at zero.
void test_player_pause_flag() {
  movie::Player player;
  CHECK(!player.paused());
  player.set_paused(true);
  CHECK(player.paused());
  player.update(1.0);
  CHECK(player.clock() == 0.0);
  player.set_paused(false);
  CHECK(!player.paused());
}
}  // namespace

int main() {
  test_mixer_movie_clock();
  test_mixer_movie_level();
  test_mixer_hold();
  test_player_pause_flag();

  const auto dir = platform::find_game_dir("");
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir) || !files.exists("movie/a01op_en.bk2")) {
    std::printf("movie: no RDE install, skipping data checks\n");
    return failures ? 1 : 0;
  }
  // Member resolution: per-language variants where they exist, the plain name otherwise.
  CHECK(movie::resolve_member(files, "a01op", "en") == "movie/a01op_en.bk2");
  CHECK(movie::resolve_member(files, "a01op", "jp") == "movie/a01op_jp.bk2");
  CHECK(movie::resolve_member(files, "title", "fr") == "movie/title.bk2");
  CHECK(movie::resolve_member(files, "nope", "en").empty());

  // Decoder: the opening's size / rate, sequential frame indices and pts, and audio interleaved with the video.
  movie::Decoder decoder;
  auto bytes = files.read("movie/a01op_en.bk2");
  CHECK(bytes && decoder.open(std::move(*bytes)));
  CHECK(decoder.width() == 1280 && decoder.height() == 960);
  CHECK(std::fabs(decoder.fps() - 15.0) < 0.01);
  CHECK(decoder.has_audio());
  u64 samples = 0;
  while (decoder.video().size() < 20 && decoder.step()) {
    samples += decoder.audio().size() / 2;
    decoder.audio().clear();
  }
  CHECK(decoder.video().size() >= 20);
  for (size_t i = 0; i < decoder.video().size(); ++i) {
    CHECK(decoder.video()[i].index == i);
    CHECK(std::fabs(decoder.video()[i].pts - static_cast<double>(i) / 15.0) < 1e-3);
  }
  CHECK(decoder.video()[0].rgba.size() == 1280u * 960u * 4u);
  CHECK(decoder.video()[0].rgba[3] == 255);
  CHECK(samples > movie::Decoder::kAudioRate / 2);

  // Player on the wall clock (no mixer, no GPU): frames advance with update(dt).
  movie::Player player;
  CHECK(player.open(files, "ending_s", "en"));
  CHECK(player.width() == 640 && player.height() == 480 && std::fabs(player.fps() - 30.0) < 0.01);
  int guard = 0;
  while (!player.finished() && player.clock() < 1.0 && ++guard < 100000) {
    player.update(1.0 / 60.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(player.clock() >= 1.0 || player.finished());
  CHECK(player.frame_index() >= 20 && player.frame_index() <= 31);
  // Pause: while paused update() changes nothing (the wall clock and the frame on screen stand still), and resuming continues.
  {
    player.set_paused(true);
    const double clock_before = player.clock();
    const u64 frame_before = player.frame_index();
    for (int i = 0; i < 30; ++i) {
      player.update(1.0 / 60.0);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(player.clock() == clock_before && player.frame_index() == frame_before && !player.finished());
    player.set_paused(false);
    for (int i = 0; i < 20 && player.clock() < clock_before + 0.2; ++i) player.update(1.0 / 60.0);
    CHECK(player.clock() > clock_before);
  }
  player.skip();
  CHECK(player.finished());

  std::printf(failures ? "movie: %d FAILED\n" : "movie: ok\n", failures);
  return failures ? 1 : 0;
}
