#pragma once
// Rate converter between a remastered BGM stream and the mixer: windowed-sinc interpolation (Blackman window, 32 taps, a polyphase
// table interpolated between phases), with the cutoff lowered to the output Nyquist when the stream rate is higher, so a 88.2 or
// 96 kHz replacement track plays at its own pitch and speed without aliasing. A stream already at the output rate passes through
// unchanged. Output is interleaved stereo float, 1.0 = s16 full scale.
#include <vector>

#include "core/types.h"

namespace oc::audio {

class HcaStream;

class StreamResampler {
 public:
  // Forgets the history; the next read starts at the stream's current position.
  void reset(u32 in_rate, u32 out_rate);

  // Writes up to `frames` output frames pulled from `stream`; fewer only when a non-looping stream ends.
  u32 read(HcaStream& stream, float* out, u32 frames);

 private:
  static constexpr int kHalfTaps = 16;
  static constexpr int kPhases = 256;

  // Appends one chunk of the stream to src_; at the end, zeros that let the last frames through the filter.
  void pull(HcaStream& stream);

  u32 in_rate_ = 0, out_rate_ = 0;
  // (kPhases + 1) rows of 2 * kHalfTaps coefficients; row p is the kernel at fraction p / kPhases.
  std::vector<float> table_;
  // Stereo source frames; src_[index_] is the frame at or before the next output position.
  std::vector<float> src_;
  std::vector<s16> chunk_;
  size_t index_ = 0;
  // Fraction of the next output position past index_, in units of 1 / out_rate_.
  u32 phase_ = 0;
  // Frames of src_ that carry stream audio; set once the stream ended.
  size_t valid_end_ = 0;
  bool ended_ = false;
};

}  // namespace oc::audio
