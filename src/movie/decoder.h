#pragma once
// Movie decoder: demux + video (-> RGBA8) + audio (-> 48 kHz stereo float). Bink 2 (and HEVC / H.264 in mp4 / mkv for mod movies)
// through librempeg, video on the codec's own threads; the PS1 discs' movie streams (.stv) through import::StrStream and
// import::mdec_decode. No GPU. The Player drives it from a worker thread, ocmovie dump directly.
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/types.h"

namespace oc::movie {

// One decoded frame: display-order index (0-based), presentation time in seconds, RGBA pixels (width*height*4).
struct VideoFrame {
  u64 index = 0;
  double pts = 0.0;
  std::vector<u8> rgba;
};

class Decoder {
 public:
  static constexpr u32 kAudioRate = 48000;

  Decoder();
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  // Takes ownership of the movie's bytes; `name` is the format hint (a name ending in ".stv" is a disc movie stream).
  bool open(Bytes file, const std::string& name = "movie.bk2");
  // Streams a movie file from disk (same probing as open, the file name as the format hint); the file stays open until the decoder
  // is destroyed.
  bool open_file(const std::filesystem::path& path);

  u32 width() const { return width_; }
  u32 height() const { return height_; }
  // The picture's shape on screen: the frame size, except a disc stream, whose frames fill a 4:3 picture (4x the width across).
  u32 display_width() const { return stream_ ? width_ * 4 : width_; }
  u32 display_height() const { return stream_ ? width_ * 3 : height_; }
  double fps() const { return fps_; }
  // A disc stream's staff-roll strip (RGBA, w x h, one row per strip sector): row A (`second` = false) or row B. False when the
  // movie carries none.
  bool staff_strip(bool second, std::vector<u8>& rgba, u32& w, u32& h) const;
  // The container's video frame count (0 when it does not say).
  u64 frame_count() const { return frames_; }
  bool has_audio() const { return has_audio_; }
  // True when the demuxer and decoders are drained (queues may still hold data).
  bool eof() const { return eof_; }

  // Reads and decodes one packet, appending to the queues. False once everything is drained.
  bool step();
  std::deque<VideoFrame>& video() { return video_; }
  // Interleaved stereo float at kAudioRate.
  std::vector<float>& audio() { return audio_; }

 private:
  // Format / stream / codec setup over the AVIO already in Impl; `name` hints the container.
  bool probe(const std::string& name);
  bool open_stream(Bytes file);
  bool step_stream();
  // Appends `count` interleaved stereo frames of impl_->src at `rate` Hz to audio_, resampled to kAudioRate.
  void resample(size_t count, int rate);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  struct Stream;
  std::unique_ptr<Stream> stream_;
  u32 width_ = 0, height_ = 0;
  double fps_ = 15.0;
  u64 frames_ = 0;
  bool has_audio_ = false;
  bool eof_ = false;
  std::deque<VideoFrame> video_;
  std::vector<float> audio_;
};

}  // namespace oc::movie
