#pragma once
// FMV player: decodes a movie.dat Bink 2 member (or a mod's mp4 / mkv replacement) on a worker thread, paces frames against the
// audio clock (the mixer's movie PCM stream; wall clock without a mixer) and exposes the current frame as an RGBA texture.
//
// Usage:  Player p; p.set_mixer(&mixer); p.open(vfs, "a01op", "en"); p.init_gpu(ctx, sprites);
//         each frame:  p.update(dt);  ...  p.draw(frame, vw, vh);  until p.finished().
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "movie/decoder.h"
#include "render/sprite_batch.h"
#include "render/texture.h"

namespace oc::audio {
class Mixer;
}
namespace oc::vfs {
class Vfs;
}

namespace oc::movie {

// Member for a movie name: "<name>_<lang>" if that exists (a01op, b28end, final), else "<name>"; at each, a mod's .mp4 / .mkv
// replacement before the .bk2. Empty if absent.
std::string resolve_member(const vfs::Vfs& fs, std::string_view name, std::string_view lang);
// The staff-roll credits strip for a language: the Japanese strip for "jp", the English one for every other language.
std::string_view staff_strip_member(std::string_view lang);
// Staff roll: the part of a strip_w x strip_h strip its area shows (a fraction of the strip's height), and the strip row (same
// unit) at the top of the area while the 1-based frame `frame` of a roll whose last frame number is `last` is on screen.
float staff_strip_span(u32 strip_w, u32 strip_h);
float staff_strip_top(u64 frame, u64 last, u32 strip_w, u32 strip_h);

class Player {
 public:
  Player() = default;
  ~Player() { close(); }
  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  // Optional, before open(): movie audio goes to this mixer's movie bus and drives the clock.
  void set_mixer(audio::Mixer* mixer) { mixer_ = mixer; }
  // `start` / `end`: play only the file's 1-based frames [start, end) (the field movie table's range; start 0 = from the beginning,
  // end 0 = the frame count, so the last frame is never shown). Earlier frames / audio are decoded and dropped, the clock starts at
  // `start`. `level` scales the movie's sound (Mixer::movie_start).
  bool open(const vfs::Vfs& fs, std::string_view name, std::string_view lang = "en", u32 start = 0, u32 end = 0, float level = 1.0f);
  // `keep_frame`: the movie ended on screen - keep the last decoded frame's texture alive so it can be faded into the next scene
  // (held() / draw_held() / fade_held()); the next open() / close() / release_held() frees it.
  void close(bool keep_frame = false);
  bool held() const { return held_; }
  void release_held() { if (held_) close(); }
  // Draws the held last frame over what is already in the frame (resume pass) at the current hold alpha.
  void draw_held(const render::Frame& frame, float view_w, float view_h);
  // Lowers the hold alpha by dt / seconds; frees the frame at 0. Returns true while still visible.
  bool fade_held(double dt, double seconds);

  // Creates the frame texture and sampler. Optional for headless use (then see frame_rgba()).
  bool init_gpu(render::GpuContext& ctx, render::SpriteBatch& sprites);
  // After init_gpu: switches to the staff-roll layout with the credits strip `member` (a PNG, one row per movie frame). False (and
  // the plain layout) when the strip cannot be read.
  bool set_staff_roll(const vfs::Vfs& fs, std::string_view member);

  // Advances the clock by dt seconds (when no audio clock applies) and picks the frame due now; uploads it if it changed.
  void update(double dt);
  // Letterboxed, aspect-preserving full-view draw (the staff roll: the panel and the strip); wraps its own sprites.begin()/end().
  void draw(const render::Frame& frame, float view_w, float view_h);

  // Pause (a host feature, not retail behaviour of the PS1 player): update() does nothing while paused - the wall clock
  // stops, no frame is picked, the on-screen frame stays (draw() keeps showing it). The audio clock is the mixer's movie PCM count, which
  // stops by itself while Mixer::hold() freezes the output (the host holds the mixer together with the pause screen).
  void set_paused(bool on) { paused_ = on; }
  bool paused() const { return paused_; }
  bool finished() const { return finished_; }
  // Stop now (finished() becomes true).
  void skip();

  u32 width() const { return width_; }
  u32 height() const { return height_; }
  double fps() const { return fps_; }
  double clock() const { return clock_; }
  // Index of the frame on screen, and a CPU copy of it.
  u64 frame_index() const { return shown_; }
  const std::vector<u8>& frame_rgba() const { return current_.rgba; }
  const std::string& member() const { return member_; }

 private:
  struct Rect {
    float x, y, w, h;
  };
  void worker();
  void pump_audio();
  // Where the movie picture goes: the 4:3 fit, or the staff roll's panel.
  Rect picture_rect(float view_w, float view_h) const;
  void draw_strip(float view_w, float view_h);
  void release_strip();

  audio::Mixer* mixer_ = nullptr;
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::Texture texture_;
  render::Sampler sampler_;
  render::TexRef set_;
  render::Texture strip_texture_;
  render::TexRef strip_set_;
  u32 strip_w_ = 0, strip_h_ = 0;

  std::string member_;
  u32 width_ = 0, height_ = 0;
  double fps_ = 15.0;
  bool has_audio_ = false;
  bool held_ = false;
  bool paused_ = false;
  double held_alpha_ = 1.0;

  // Main-thread state.
  VideoFrame current_;
  u64 shown_ = 0;
  bool have_frame_ = false, running_ = false, finished_ = false;
  double clock_ = 0.0, last_change_ = 0.0;

  // Worker -> main handoff.
  std::unique_ptr<Decoder> dec_;
  std::thread thread_;
  std::mutex mu_;
  std::deque<VideoFrame> ready_;
  std::atomic<bool> stop_{false}, dec_done_{false};
  // pushed_audio_: frames handed to the mixer; audio_done_: decoder drained and every sample pushed; apend_ (worker-only): decoded
  // audio waiting for ring space.
  std::atomic<u64> pushed_audio_{0};
  std::atomic<bool> audio_done_{false};
  std::vector<float> apend_;
  size_t apend_pos_ = 0;
  static constexpr size_t kReadyMax = 8;
  bool src_done() const { return range_done_ || dec_->eof(); }
  // Frame indices (0-based), end exclusive, 0 = none; last_ = the 1-based end frame (the range's end clamped to the frame count).
  u32 start_ = 0, end_ = 0, last_ = 0;
  size_t skip_floats_ = 0, limit_floats_ = 0, seen_floats_ = 0, kept_floats_ = 0;
  bool range_done_ = false;
};

}  // namespace oc::movie
