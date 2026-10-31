#include "movie/player.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "audio/mixer.h"
#include "core/log.h"
#include "render/camera.h"
#include "vfs/vfs.h"

namespace oc::movie {

namespace {

// The movie table counts frames of the original 15 fps movies.
constexpr double kTableFps = 15.0;

}  // namespace

std::string resolve_member(const vfs::Vfs& fs, std::string_view name, std::string_view lang) {
  std::string base(name);
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".bk2") == 0) base.resize(base.size() - 4);
  std::vector<std::string> stems;
  if (!lang.empty()) stems.push_back("movie/" + base + "_" + std::string(lang));
  stems.push_back("movie/" + base);
  for (const std::string& stem : stems) {
    for (const char* ext : {".mp4", ".mkv"}) {
      if (fs.exists(stem + ext)) return stem + ext;
    }
    if (fs.exists(stem + ".bk2")) return stem + ".bk2";
  }
  return {};
}

bool Player::open(const vfs::Vfs& fs, std::string_view name, std::string_view lang, u32 start, u32 end) {
  close();
  member_ = resolve_member(fs, name, lang);
  if (member_.empty()) {
    log::error("movie: '{}' not found in movie.dat", name);
    return false;
  }
  dec_ = std::make_unique<Decoder>();
  // A mod's loose movie streams from disk (they are hundreds of MB); archive members, a packed mod's included, are read whole.
  if (auto loose = fs.loose_file(member_)) {
    if (!dec_->open_file(*loose)) {
      dec_.reset();
      return false;
    }
  } else {
    auto bytes = fs.read(member_);
    if (!bytes || !dec_->open(std::move(*bytes), member_)) {
      dec_.reset();
      return false;
    }
  }
  width_ = dec_->width();
  height_ = dec_->height();
  fps_ = dec_->fps();
  has_audio_ = dec_->has_audio() && mixer_;
  if (has_audio_) mixer_->movie_start();
  log::info("movie: {} {}x{} @ {:.2f} fps{}", member_, width_, height_, fps_, dec_->has_audio() ? " + audio" : "");
  // Table frames are 1-based (the first shown is max(start, 1)); decoder indices are 0-based, at the file's own rate.
  const double rate_scale = fps_ / kTableFps;
  start_ = start > 1 ? static_cast<u32>(std::lround((start - 1) * rate_scale)) : 0;
  const u32 scaled_end = static_cast<u32>(std::lround(end * rate_scale));
  end_ = scaled_end > start_ ? scaled_end : 0;
  skip_floats_ = static_cast<size_t>(static_cast<double>(start_) / fps_ * Decoder::kAudioRate) * 2;
  limit_floats_ = end_ ? static_cast<size_t>(static_cast<double>(end_ - start_) / fps_ * Decoder::kAudioRate) * 2 : 0;
  seen_floats_ = kept_floats_ = 0;
  range_done_ = false;
  stop_ = false;
  dec_done_ = false;
  audio_done_ = !has_audio_;
  pushed_audio_ = 0;
  finished_ = running_ = have_frame_ = paused_ = false;
  clock_ = last_change_ = 0.0;
  shown_ = 0;
  thread_ = std::thread([this] { worker(); });
  return true;
}

void Player::close(bool keep_frame) {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (mixer_ && has_audio_) mixer_->movie_stop();
  const bool keep = keep_frame && have_frame_ && ctx_ && set_;
  if (!keep) {
    set_ = {};
    texture_.destroy();
    sampler_.destroy();
    ctx_ = nullptr;
    sprites_ = nullptr;
  }
  held_ = keep;
  held_alpha_ = 1.0;
  ready_.clear();
  apend_.clear();
  apend_pos_ = 0;
  dec_.reset();
  has_audio_ = false;
  finished_ = true;
}

void Player::skip() {
  if (!dec_) return;
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (mixer_ && has_audio_) mixer_->movie_stop();
  finished_ = true;
}

bool Player::init_gpu(render::GpuContext& ctx, render::SpriteBatch& sprites) {
  if (!dec_) return false;
  std::vector<u8> black(static_cast<size_t>(width_) * height_ * 4, 0);
  for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
  if (!texture_.create(ctx, width_, height_, render::TextureFormat::Rgba8, black.data())) return false;
  if (!sampler_.create(ctx, true)) return false;
  set_ = render::TexRef(texture_, sampler_);
  ctx_ = &ctx;
  sprites_ = &sprites;
  return static_cast<bool>(set_);
}

// Worker: decode ahead, then feed the mixer's ring as it frees up.
void Player::worker() {
  while (!stop_) {
    bool want_video;
    {
      std::lock_guard lk(mu_);
      want_video = ready_.size() < kReadyMax;
    }
    // Keep up to ~1 s of audio decoded ahead.
    const bool want_audio = apend_.size() - apend_pos_ < Decoder::kAudioRate;
    if (!dec_done_ && (want_video || (has_audio_ && want_audio)) && !src_done()) {
      if (!dec_->step()) dec_done_ = true;
      if (!dec_->video().empty()) {
        std::lock_guard lk(mu_);
        for (auto& video_frame : dec_->video()) {
          if (video_frame.index < start_) continue;
          if (end_ && video_frame.index >= end_) {
            range_done_ = true;
            continue;
          }
          video_frame.pts -= static_cast<double>(start_) / fps_;
          ready_.push_back(std::move(video_frame));
        }
        dec_->video().clear();
      }
      if (has_audio_ && !dec_->audio().empty()) {
        const auto& samples = dec_->audio();
        size_t from = 0, to = samples.size();
        if (seen_floats_ < skip_floats_) from = std::min(to, skip_floats_ - seen_floats_);
        seen_floats_ += samples.size();
        if (limit_floats_) to = std::min(to, from + (kept_floats_ >= limit_floats_ ? 0 : limit_floats_ - kept_floats_));
        if (to > from) {
          apend_.insert(apend_.end(), samples.begin() + static_cast<std::ptrdiff_t>(from), samples.begin() + static_cast<std::ptrdiff_t>(to));
          kept_floats_ += to - from;
        }
        if (limit_floats_ && kept_floats_ >= limit_floats_) range_done_ = true;
      }
      dec_->audio().clear();
      if (!has_audio_) dec_->audio().clear();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (has_audio_) pump_audio();
    else if (src_done()) dec_done_ = true;
    if (src_done()) dec_done_ = true;
  }
}

void Player::pump_audio() {
  const size_t avail = (apend_.size() - apend_pos_) / 2;
  if (avail) {
    const u32 accepted = mixer_->movie_push(apend_.data() + apend_pos_, static_cast<u32>(avail));
    apend_pos_ += static_cast<size_t>(accepted) * 2;
    pushed_audio_ += accepted;
    if (apend_pos_ == apend_.size()) {
      apend_.clear();
      apend_pos_ = 0;
    } else if (apend_pos_ > (1u << 20)) {
      apend_.erase(apend_.begin(), apend_.begin() + static_cast<std::ptrdiff_t>(apend_pos_));
      apend_pos_ = 0;
    }
  }
  if (src_done() && apend_.empty()) audio_done_ = true;
}

void Player::update(double dt) {
  if (!dec_ || finished_ || paused_) return;

  if (!running_) {
    // Wait for the first frame (and a little audio) before the clock starts, so playback does not begin with a stall.
    bool ready;
    {
      std::lock_guard lk(mu_);
      ready = !ready_.empty();
    }
    const bool audio_ok = !has_audio_ || pushed_audio_ >= Decoder::kAudioRate / 4 || audio_done_;
    if (!ready || !audio_ok) {
      // An empty or failed movie ends at once.
      if (dec_done_ && !ready) finished_ = true;
      return;
    }
    running_ = true;
    if (has_audio_) mixer_->movie_run();
    dt = 0;
  }

  // Clock: the audio device's consumed-sample count while there is audio left; wall time otherwise.
  const u64 pushed = pushed_audio_;
  if (has_audio_ && !(audio_done_ && mixer_->movie_frames_played() >= pushed)) {
    clock_ = std::max(clock_, static_cast<double>(mixer_->movie_frames_played()) / Decoder::kAudioRate);
  } else {
    clock_ += dt;
  }

  bool changed = false;
  {
    std::lock_guard lk(mu_);
    while (!ready_.empty() && ready_.front().pts <= clock_ + 1e-6) {
      current_ = std::move(ready_.front());
      ready_.pop_front();
      changed = true;
    }
    if (!ready_.empty() || !dec_done_) {
      // keep going
    }
  }
  if (changed) {
    have_frame_ = true;
    shown_ = current_.index;
    last_change_ = clock_;
    if (ctx_ && texture_) texture_.update(*ctx_, current_.rgba.data());
  }
  bool queue_empty;
  {
    std::lock_guard lk(mu_);
    queue_empty = ready_.empty();
  }
  if (dec_done_ && queue_empty && have_frame_ && clock_ - last_change_ >= 1.0 / fps_ &&
      (!has_audio_ || (audio_done_ && mixer_->movie_frames_played() >= pushed_audio_))) {
    skip();
  }
}

void Player::draw_held(const render::Frame& frame, float vw, float vh) {
  if (!held_ || !ctx_ || !set_) return;
  // The shared 4:3 fit zoom.
  const float scale = render::canvas_fit(vw, vh, static_cast<float>(width_), static_cast<float>(height_)).s;
  const float w = static_cast<float>(width_) * scale, h = static_cast<float>(height_) * scale;
  const u32 a = static_cast<u32>(std::clamp(held_alpha_, 0.0, 1.0) * 255.0 + 0.5);
  const float x0 = (vw - w) * 0.5f, y0 = (vh - h) * 0.5f;
  sprites_->begin(frame, vw, vh, true);
  sprites_->draw(set_, x0, y0, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0x00FFFFFFu | (a << 24));
  // The movie's own black bars fade with it over the rest of the screen in use (widescreen: the widened frame, Letterbox::veil_rect):
  // the (opaque) movie texels tinted black at the same alpha, one strip per side of the frame.
  const render::Letterbox::Rect veil = render::Letterbox::veil_rect(vw, vh);
  const u32 black = a << 24;
  const float rx1 = veil.x + veil.w, ry1 = veil.y + veil.h, x1 = x0 + w, y1 = y0 + h;
  if (x0 > veil.x) sprites_->draw(set_, veil.x, veil.y, x0 - veil.x, veil.h, 0.0f, 0.0f, 1.0f, 1.0f, black);
  if (rx1 > x1) sprites_->draw(set_, x1, veil.y, rx1 - x1, veil.h, 0.0f, 0.0f, 1.0f, 1.0f, black);
  const float cx0 = std::max(x0, veil.x), cx1 = std::min(x1, rx1);
  if (cx1 > cx0 && y0 > veil.y) sprites_->draw(set_, cx0, veil.y, cx1 - cx0, y0 - veil.y, 0.0f, 0.0f, 1.0f, 1.0f, black);
  if (cx1 > cx0 && ry1 > y1) sprites_->draw(set_, cx0, y1, cx1 - cx0, ry1 - y1, 0.0f, 0.0f, 1.0f, 1.0f, black);
  sprites_->end();
}

bool Player::fade_held(double dt, double seconds) {
  if (!held_) return false;
  held_alpha_ -= dt / seconds;
  if (held_alpha_ <= 0.0) {
    release_held();
    return false;
  }
  return true;
}

void Player::draw(const render::Frame& frame, float vw, float vh) {
  if (!ctx_ || !set_) return;
  const float scale = render::canvas_fit(vw, vh, static_cast<float>(width_), static_cast<float>(height_)).s;
  const float w = static_cast<float>(width_) * scale, h = static_cast<float>(height_) * scale;
  sprites_->begin(frame, vw, vh);
  sprites_->draw(set_, (vw - w) * 0.5f, (vh - h) * 0.5f, w, h);
  sprites_->end();
}

}  // namespace oc::movie
