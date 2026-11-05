#include "movie/player.h"

#include <algorithm>
#include <chrono>

#include "audio/mixer.h"
#include "core/log.h"
#include "render/camera.h"
#include "vfs/vfs.h"

namespace oc::movie {

namespace {

// Staff-roll layout, in fractions of the 4:3 picture: the movie panel's corner and size, and the strip's width from the left edge.
constexpr float kPanelX = 0.48f, kPanelY = 0.46f, kPanelSize = 0.49f;
constexpr float kStripWidth = 0.45f;
// By the roll's last frame the end of the strip has risen this part of the visible span above the bottom of the strip area.
constexpr float kStripEndRise = 0.52f;

}  // namespace

std::string_view staff_strip_member(std::string_view lang) {
  return lang == "jp" ? "hd/staff/staff24.png" : "hd/staff/staff24us.png";
}

float staff_strip_span(u32 strip_w, u32 strip_h) {
  if (!strip_h) return 0.0f;
  // Square texels on the 4:3 picture: the area is kStripWidth of its width across and the full height down.
  return static_cast<float>(strip_w) * 3.0f / (static_cast<float>(strip_h) * 4.0f * kStripWidth);
}

float staff_strip_top(u64 frame, u64 last, u32 strip_w, u32 strip_h) {
  const float span = staff_strip_span(strip_w, strip_h);
  if (!last) return -span;
  return (span * kStripEndRise + 1.0f) * static_cast<float>(frame) / static_cast<float>(last) - span;
}

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

bool Player::open(const vfs::Vfs& fs, std::string_view name, std::string_view lang, u32 start, u32 end, float level) {
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
  if (has_audio_) mixer_->movie_start(level);
  log::info("movie: {} {}x{} @ {:.2f} fps{}", member_, width_, height_, fps_, dec_->has_audio() ? " + audio" : "");
  // Table frames are the file's own 1-based frame numbers: frames max(start, 1) .. last - 1 are shown, last = the end clamped to the
  // frame count. Decoder indices are 0-based.
  const u64 count = dec_->frame_count();
  start_ = start > 1 ? start - 1 : 0;
  last_ = static_cast<u32>(end && (!count || end < count) ? end : count);
  end_ = last_ > start_ + 1 ? last_ - 1 : 0;
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

void Player::close(bool keep_frame, bool fade) {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (mixer_ && has_audio_) mixer_->movie_stop();
  const bool keep = keep_frame && have_frame_ && ctx_ && set_;
  if (!keep) {
    release_strip();
    set_ = {};
    texture_.destroy();
    sampler_.destroy();
    ctx_ = nullptr;
    sprites_ = nullptr;
  }
  held_ = keep;
  held_fade_ = fade;
  held_alpha_ = 1.0;
  held_frames_ = 0.0;
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

bool Player::set_staff_roll(const vfs::Vfs& fs, std::string_view member) {
  release_strip();
  if (!ctx_) return false;
  auto bytes = fs.read(member);
  render::DecodedImage image;
  if (!bytes || !render::decode_image(*bytes, image) ||
      !strip_texture_.create(*ctx_, image.width, image.height, render::TextureFormat::Rgba8, image.rgba.data())) {
    log::error("movie: staff strip '{}' could not be read", member);
    strip_texture_.destroy();
    return false;
  }
  strip_set_ = render::TexRef(strip_texture_, sampler_);
  strip_w_ = image.width;
  strip_h_ = image.height;
  log::info("movie: staff roll, strip {} {}x{}, last frame {}", member, strip_w_, strip_h_, last_);
  return true;
}

void Player::release_strip() {
  strip_set_ = {};
  strip_texture_.destroy();
  strip_w_ = strip_h_ = 0;
}

Player::Rect Player::picture_rect(float vw, float vh) const {
  if (strip_set_) {
    // The staff roll's panel sits at the lower right of the plain 4:3 fit of the window.
    const float h = std::min(vh, vw * 0.75f), w = h * 4.0f / 3.0f;
    const float x0 = (vw - w) * 0.5f, y0 = (vh - h) * 0.5f;
    return {x0 + w * kPanelX, y0 + h * kPanelY, w * kPanelSize, h * kPanelSize};
  }
  // The shared 4:3 fit zoom.
  const float scale = render::canvas_fit(vw, vh, static_cast<float>(width_), static_cast<float>(height_)).s;
  const float w = static_cast<float>(width_) * scale, h = static_cast<float>(height_) * scale;
  return {(vw - w) * 0.5f, (vh - h) * 0.5f, w, h};
}

void Player::draw_strip(float vw, float vh) {
  if (!strip_set_ || !have_frame_) return;
  // The strip area: the left kStripWidth of RDE's 1280 x 960 canvas, its full height (render::overlay_fit's band and display squash).
  const render::OverlayFit fit = render::overlay_fit(vw, vh);
  const float x0 = fit.ox, x1 = fit.ox + 1280.0f * kStripWidth * fit.sx;
  float y0 = fit.oy, y1 = fit.oy + 960.0f * fit.sy;
  const float span = staff_strip_span(strip_w_, strip_h_);
  // The decoder has moved on to the next frame by the time the shown one is drawn: frame number = 0-based index + 2.
  float v0 = staff_strip_top(shown_ + 2, last_, strip_w_, strip_h_), v1 = v0 + span;
  if (span <= 0.0f || v1 <= 0.0f || v0 >= 1.0f) return;
  const float per_v = (y1 - y0) / span;
  if (v0 < 0.0f) y0 -= v0 * per_v, v0 = 0.0f;
  if (v1 > 1.0f) y1 -= (v1 - 1.0f) * per_v, v1 = 1.0f;
  sprites_->draw(strip_set_, x0, y0, x1 - x0, y1 - y0, 0.0f, v0, 1.0f, v1);
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
  // A staff roll ends with its strip scrolled out (staff_strip_top at the last frame), so only the panel is held.
  const Rect picture = picture_rect(vw, vh);
  const float w = picture.w, h = picture.h, x0 = picture.x, y0 = picture.y;
  const u32 a = static_cast<u32>(std::clamp(held_alpha_, 0.0, 1.0) * 255.0 + 0.5);
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

double held_frame_alpha(double frames, bool fade) {
  if (frames >= (fade ? kHeldFadeFrames : kHeldHoldFrames)) return 0.0;
  return fade ? 1.0 - std::max(0.0, frames) / kHeldFadeFrames : 1.0;
}

bool Player::step_held(double dt) {
  if (!held_) return false;
  held_frames_ += dt * 60.0;
  held_alpha_ = held_frame_alpha(held_frames_, held_fade_);
  if (held_alpha_ <= 0.0) {
    release_held();
    return false;
  }
  return true;
}

void Player::draw(const render::Frame& frame, float vw, float vh) {
  if (!ctx_ || !set_) return;
  const Rect picture = picture_rect(vw, vh);
  sprites_->begin(frame, vw, vh);
  sprites_->draw(set_, picture.x, picture.y, picture.w, picture.h);
  draw_strip(vw, vh);
  sprites_->end();
}

}  // namespace oc::movie
