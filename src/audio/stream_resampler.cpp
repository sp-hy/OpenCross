#include "audio/stream_resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "audio/hca_stream.h"

namespace oc::audio {
namespace {
constexpr u32 kChunkFrames = 1024;
constexpr size_t kCompactAt = 8192;
}  // namespace

// Row p holds h(x) = c * sinc(c * x) * blackman(x) at the tap offsets x = k - (kHalfTaps - 1) - p / kPhases, normalised to unit
// DC gain; c is 1 when upsampling and 0.95 * out / in when downsampling.
void StreamResampler::reset(u32 in_rate, u32 out_rate) {
  in_rate_ = in_rate;
  out_rate_ = out_rate;
  index_ = kHalfTaps - 1;
  phase_ = 0;
  valid_end_ = 0;
  ended_ = false;
  src_.assign(size_t(kHalfTaps - 1) * 2, 0.0f);
  src_.reserve((kCompactAt + kChunkFrames + 2 * kHalfTaps) * 2);
  chunk_.resize(size_t(kChunkFrames) * 2);
  if (in_rate_ == out_rate_ || in_rate_ == 0 || out_rate_ == 0) return;
  const double cutoff = in_rate_ > out_rate_ ? 0.95 * double(out_rate_) / double(in_rate_) : 1.0;
  table_.assign(size_t(kPhases + 1) * 2 * kHalfTaps, 0.0f);
  for (int p = 0; p <= kPhases; ++p) {
    float* row = table_.data() + size_t(p) * 2 * kHalfTaps;
    double sum = 0.0;
    for (int k = 0; k < 2 * kHalfTaps; ++k) {
      const double x = double(k - (kHalfTaps - 1)) - double(p) / kPhases;
      const double arg = std::numbers::pi * cutoff * x;
      const double sinc = std::abs(arg) < 1e-12 ? 1.0 : std::sin(arg) / arg;
      const double edge = std::numbers::pi * x / kHalfTaps;
      const double window = std::abs(x) >= kHalfTaps ? 0.0 : 0.42 + 0.5 * std::cos(edge) + 0.08 * std::cos(2.0 * edge);
      const double tap = cutoff * sinc * window;
      row[k] = float(tap);
      sum += tap;
    }
    for (int k = 0; k < 2 * kHalfTaps; ++k) row[k] = float(row[k] / sum);
  }
}

void StreamResampler::pull(HcaStream& stream) {
  const u32 got = stream.read(chunk_.data(), kChunkFrames);
  for (u32 i = 0; i < got * 2; ++i) src_.push_back(float(chunk_[i]) * (1.0f / 32768.0f));
  if (got < kChunkFrames) {
    ended_ = true;
    valid_end_ = src_.size() / 2;
    src_.insert(src_.end(), size_t(kHalfTaps) * 2, 0.0f);
  }
}

u32 StreamResampler::read(HcaStream& stream, float* out, u32 frames) {
  if (in_rate_ == out_rate_ || table_.empty()) {
    u32 done = 0;
    while (done < frames) {
      const u32 want = std::min(frames - done, kChunkFrames);
      const u32 got = stream.read(chunk_.data(), want);
      for (u32 i = 0; i < got * 2; ++i) out[size_t(done) * 2 + i] = float(chunk_[i]) * (1.0f / 32768.0f);
      done += got;
      if (got < want) break;
    }
    return done;
  }
  u32 done = 0;
  while (done < frames) {
    if (ended_ && index_ >= valid_end_) break;
    while (!ended_ && index_ + kHalfTaps >= src_.size() / 2) pull(stream);
    const double position = double(phase_) * kPhases / double(out_rate_);
    const int row = std::min(int(position), kPhases - 1);
    const float blend = float(position - row);
    const float* near_row = table_.data() + size_t(row) * 2 * kHalfTaps;
    const float* far_row = near_row + 2 * kHalfTaps;
    const float* frame = src_.data() + (index_ - (kHalfTaps - 1)) * 2;
    float left = 0.0f, right = 0.0f;
    for (int k = 0; k < 2 * kHalfTaps; ++k) {
      const float tap = near_row[k] + (far_row[k] - near_row[k]) * blend;
      left += frame[size_t(k) * 2] * tap;
      right += frame[size_t(k) * 2 + 1] * tap;
    }
    out[size_t(done) * 2] = left;
    out[size_t(done) * 2 + 1] = right;
    ++done;
    phase_ += in_rate_;
    index_ += phase_ / out_rate_;
    phase_ %= out_rate_;
  }
  if (index_ >= kCompactAt) {
    const size_t drop = index_ - (kHalfTaps - 1);
    src_.erase(src_.begin(), src_.begin() + std::ptrdiff_t(drop * 2));
    index_ -= drop;
    if (ended_) valid_end_ -= std::min(valid_end_, drop);
  }
  return done;
}

}  // namespace oc::audio
