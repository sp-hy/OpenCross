#pragma once
// Pull decoder for the stream inside a .sab: HCA, or the 16-bit PCM some replacement tracks carry (mono or stereo, 48 kHz).
// Output is interleaved s16 stereo at the stream rate (48 kHz), a mono PCM sample on both sides.
// Sample positions are in output samples (after the HCA mute_header is skipped), the same coordinates as the SEAD
// material loop points: sample s lives in HCA block (s + mute_header) / 1024, or at PCM frame s.
#include <memory>
#include <vector>

#include "audio/sab.h"
#include "core/types.h"

struct clHCA;

namespace oc::audio {

// Frames until the position reaches the end of the loop body; the caller wraps when this hits 0.
// pos is the next sample to play, loop_end is inclusive.
constexpr u32 frames_until_wrap(u32 pos, u32 loop_end) { return pos > loop_end ? 0 : loop_end - pos + 1; }

class HcaStream {
 public:
  HcaStream();
  ~HcaStream();
  HcaStream(const HcaStream&) = delete;
  HcaStream& operator=(const HcaStream&) = delete;

  // Takes a copy of the .sab bytes. False on a malformed container / unsupported codec / bad HCA header.
  bool open(ByteSpan sab_bytes);

  u32 sample_rate() const { return rate_; }
  u32 total_samples() const { return total_; }
  u32 loop_start() const { return loop_start_; }
  u32 loop_end() const { return loop_end_; }  // inclusive
  bool has_loop() const { return loop_end_ > loop_start_; }
  u32 position() const { return pos_; }

  // When enabled (default) and the song has loop points, playback continues from loop_start after loop_end.
  void set_looping(bool on) { looping_ = on; }
  bool looping() const { return looping_; }

  // Positions the stream so the next read starts at `sample` (clamped to the end). Primes the MDCT overlap with the
  // preceding block so the audio is bit-identical to continuous decoding.
  void seek(u32 sample);

  // Writes up to `frames` stereo frames (2*frames s16). Returns frames written; < frames only at the end of a
  // non-looping stream.
  u32 read(s16* out, u32 frames);

  // Convenience: returns interleaved samples (2 per frame).
  std::vector<s16> read(u32 frames);

 private:
  // The raw (de-obfuscated) HCA frame of `block`.
  Bytes block_bytes(u32 block) const;
  // Decodes `block` into block_pcm_; false on error (block_pcm_ zeroed).
  bool decode_block(u32 block);
  bool open_pcm(const SabMaterial& material);
  u32 read_pcm(s16* out, u32 frames);

  Bytes data_;
  struct HcaDeleter { void operator()(clHCA* hca) const; };
  std::unique_ptr<clHCA, HcaDeleter> hca_;
  u32 rate_ = 0, total_ = 0, loop_start_ = 0, loop_end_ = 0;
  u32 delay_ = 0, block_count_ = 0, block_size_ = 0, data_off_ = 0;
  bool encrypted_ = false;
  // Channels of a PCM material; 0 = HCA.
  u32 pcm_channels_ = 0;
  u32 key_start_ = 0;
  u32 pos_ = 0;
  bool looping_ = true;
  std::vector<s16> block_pcm_;  // 1024 * 2
  u32 cur_block_ = 0xFFFFFFFFu;  // block currently held in block_pcm_
  u32 next_block_ = 0;           // next block the decoder state expects (MDCT overlap continuity)
};

}  // namespace oc::audio
