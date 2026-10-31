#pragma once
// Movie decoder over librempeg (Bink 2; HEVC / H.264 in mp4 / mkv for mod movies): demux + video (-> RGBA8) + audio (-> 48 kHz
// stereo float). No GPU; video decodes on the codec's own threads. The Player drives it from a worker thread, ocmovie dump directly.
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

  // Takes ownership of the movie's bytes; `name` is the format hint.
  bool open(Bytes file, const std::string& name = "movie.bk2");
  // Streams a movie file from disk (same probing as open, the file name as the format hint); the file stays open until the decoder
  // is destroyed.
  bool open_file(const std::filesystem::path& path);

  u32 width() const { return width_; }
  u32 height() const { return height_; }
  double fps() const { return fps_; }
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
  u32 width_ = 0, height_ = 0;
  double fps_ = 15.0;
  bool has_audio_ = false;
  bool eof_ = false;
  std::deque<VideoFrame> video_;
  std::vector<float> audio_;
};

}  // namespace oc::movie
