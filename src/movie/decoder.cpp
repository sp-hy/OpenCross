#include "movie/decoder.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
}

namespace oc::movie {

namespace {

std::string av_err(int error) {
  char buf[128];
  av_strerror(error, buf, sizeof buf);
  return buf;
}

inline u8 clamp8(int value) { return static_cast<u8>(value < 0 ? 0 : (value > 255 ? 255 : value)); }

// YUV -> RGB in 16.16 fixed point: R = g (Y - off) + cr_r Cr', G = g (Y - off) - cb_g Cb' - cr_g Cr', B = g (Y - off) + cb_b Cb'
// (Cb', Cr' centred on 0). Limited range scales luma by 255/219 and chroma by 255/224 around the matrix.
struct YuvMatrix {
  int luma_offset, luma_gain, cr_r, cb_g, cr_g, cb_b;
};
constexpr YuvMatrix kFull601{0, 65536, 91881, 22554, 46802, 116130};
constexpr YuvMatrix kLimited601{16, 76309, 104597, 25675, 53279, 132201};
constexpr YuvMatrix kFull709{0, 65536, 103206, 12276, 30679, 121609};
constexpr YuvMatrix kLimited709{16, 76309, 117489, 13975, 34925, 138438};

// Bink is always full-range BT.601. Other codecs follow the stream's tags; untagged they are limited range, BT.709 from 720 lines up.
const YuvMatrix& yuv_matrix(const AVFrame& frame, bool bink) {
  if (bink) return kFull601;
  const bool full = frame.color_range == AVCOL_RANGE_JPEG || frame.format == AV_PIX_FMT_YUVJ420P;
  const bool bt709 = frame.colorspace == AVCOL_SPC_BT709 || frame.colorspace == AVCOL_SPC_BT2020_NCL ||
                     (frame.colorspace == AVCOL_SPC_UNSPECIFIED && frame.height >= 720);
  if (bt709) return full ? kFull709 : kLimited709;
  return full ? kFull601 : kLimited601;
}

// Sample: u8 planes, or u16 planes of `depth` bits (shifted down to 8). Chroma subsampling through the shifts (4:2:0 -> 1,1).
template <typename Sample>
void yuv_to_rgba(const AVFrame& frame, const YuvMatrix& matrix, int depth, int cw_shift, int ch_shift, const u8* alpha_plane,
                 int alpha_stride, u8* out) {
  const int down = depth - 8;
  auto row = [&](int plane, int line) {
    return reinterpret_cast<const Sample*>(frame.data[plane] + static_cast<ptrdiff_t>(line) * frame.linesize[plane]);
  };
  for (int y = 0; y < frame.height; ++y) {
    const Sample* py = row(0, y);
    const Sample* pu = row(1, y >> ch_shift);
    const Sample* pv = row(2, y >> ch_shift);
    const u8* pa = alpha_plane ? alpha_plane + static_cast<ptrdiff_t>(y) * alpha_stride : nullptr;
    u8* dst = out + static_cast<size_t>(y) * static_cast<size_t>(frame.width) * 4;
    for (int x = 0; x < frame.width; ++x, dst += 4) {
      const int luma = ((py[x] >> down) - matrix.luma_offset) * matrix.luma_gain;
      const int cb = (pu[x >> cw_shift] >> down) - 128, cr = (pv[x >> cw_shift] >> down) - 128;
      dst[0] = clamp8((luma + matrix.cr_r * cr + 32768) >> 16);
      dst[1] = clamp8((luma - matrix.cb_g * cb - matrix.cr_g * cr + 32768) >> 16);
      dst[2] = clamp8((luma + matrix.cb_b * cb + 32768) >> 16);
      dst[3] = pa ? pa[x] : 255;
    }
  }
}

bool convert(const AVFrame& frame, bool bink, std::vector<u8>& out) {
  out.resize(static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4);
  const YuvMatrix& matrix = yuv_matrix(frame, bink);
  switch (frame.format) {
    case AV_PIX_FMT_YUV420P: yuv_to_rgba<u8>(frame, matrix, 8, 1, 1, nullptr, 0, out.data()); return true;
    case AV_PIX_FMT_YUV422P: yuv_to_rgba<u8>(frame, matrix, 8, 1, 0, nullptr, 0, out.data()); return true;
    case AV_PIX_FMT_YUV444P: yuv_to_rgba<u8>(frame, matrix, 8, 0, 0, nullptr, 0, out.data()); return true;
    case AV_PIX_FMT_YUVJ420P: yuv_to_rgba<u8>(frame, matrix, 8, 1, 1, nullptr, 0, out.data()); return true;
    case AV_PIX_FMT_YUVA420P: yuv_to_rgba<u8>(frame, matrix, 8, 1, 1, frame.data[3], frame.linesize[3], out.data()); return true;
    case AV_PIX_FMT_YUV420P10LE: yuv_to_rgba<u16>(frame, matrix, 10, 1, 1, nullptr, 0, out.data()); return true;
    default: log::error("movie: unsupported pixel format {}", frame.format); return false;
  }
}

struct MemIo {
  const u8* data;
  size_t size;
  size_t pos;
};

int io_read(void* opaque, uint8_t* buf, int count) {
  auto* mem = static_cast<MemIo*>(opaque);
  if (mem->pos >= mem->size) return AVERROR_EOF;
  const size_t copied = std::min<size_t>(static_cast<size_t>(count), mem->size - mem->pos);
  std::memcpy(buf, mem->data + mem->pos, copied);
  mem->pos += copied;
  return static_cast<int>(copied);
}

int64_t io_seek(void* opaque, int64_t off, int whence) {
  auto* mem = static_cast<MemIo*>(opaque);
  if (whence == AVSEEK_SIZE) return static_cast<int64_t>(mem->size);
  int64_t target;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = off; break;
    case SEEK_CUR: target = static_cast<int64_t>(mem->pos) + off; break;
    case SEEK_END: target = static_cast<int64_t>(mem->size) + off; break;
    default: return -1;
  }
  if (target < 0 || target > static_cast<int64_t>(mem->size)) return -1;
  mem->pos = static_cast<size_t>(target);
  return target;
}

// File-backed AVIO (mod movies: hundreds of MB, streamed from disk instead of read whole).
int file_read(void* opaque, uint8_t* buf, int count) {
  auto* file = static_cast<std::FILE*>(opaque);
  const size_t read = std::fread(buf, 1, static_cast<size_t>(count), file);
  if (read == 0) return std::feof(file) ? AVERROR_EOF : AVERROR(EIO);
  return static_cast<int>(read);
}

int64_t file_seek(void* opaque, int64_t off, int whence) {
  auto* file = static_cast<std::FILE*>(opaque);
  if (whence == AVSEEK_SIZE) {
    const int64_t cur = _ftelli64(file);
    if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
    const int64_t size = _ftelli64(file);
    _fseeki64(file, cur, SEEK_SET);
    return size;
  }
  int origin;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: origin = SEEK_SET; break;
    case SEEK_CUR: origin = SEEK_CUR; break;
    case SEEK_END: origin = SEEK_END; break;
    default: return -1;
  }
  if (_fseeki64(file, off, origin) != 0) return -1;
  return _ftelli64(file);
}

}  // namespace

struct Decoder::Impl {
  Bytes file;
  MemIo mem{};
  std::FILE* fp = nullptr;
  AVFormatContext* fmt = nullptr;
  AVIOContext* io = nullptr;
  AVCodecContext* vctx = nullptr;
  AVCodecContext* actx = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  int vstream = -1, astream = -1;
  u64 vcount = 0;
  bool bink = false;
  bool demux_done = false, flushed = false;
  // Linear resampler state (source rate -> 48 kHz): rs_pos is the source position of the next output sample, relative to the next
  // chunk's sample 0; rs_prev the last source frame of the previous chunk (source index -1); src a scratch of interleaved stereo.
  double rs_pos = 0.0;
  float rs_prev[2] = {0, 0};
  std::vector<float> src;

  ~Impl() {
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vctx);
    avcodec_free_context(&actx);
    // CUSTOM_IO: closing the format does not free the AVIOContext.
    if (fmt) avformat_close_input(&fmt);
    if (io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
    if (fp) std::fclose(fp);
  }
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;

bool Decoder::open(Bytes file, const std::string& name) {
  Impl& state = *impl_;
  state.file = std::move(file);
  state.mem = {state.file.data(), state.file.size(), 0};
  constexpr int kBuf = 1 << 16;
  state.io = avio_alloc_context(static_cast<unsigned char*>(av_malloc(kBuf)), kBuf, 0, &state.mem, io_read, nullptr, io_seek);
  return probe(name);
}

bool Decoder::open_file(const std::filesystem::path& path) {
  Impl& state = *impl_;
  state.fp = _wfopen(path.c_str(), L"rb");
  if (!state.fp) {
    log::error("movie: cannot open {}", path.string());
    return false;
  }
  // Large reads: the source is a multi-hundred-MB file.
  constexpr int kBuf = 1 << 20;
  state.io = avio_alloc_context(static_cast<unsigned char*>(av_malloc(kBuf)), kBuf, 0, state.fp, file_read, nullptr, file_seek);
  return probe(path.filename().string());
}

bool Decoder::probe(const std::string& name) {
  Impl& state = *impl_;
  state.fmt = avformat_alloc_context();
  if (!state.io || !state.fmt) return false;
  state.fmt->pb = state.io;
  state.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  int error = avformat_open_input(&state.fmt, name.c_str(), nullptr, nullptr);
  if (error < 0) {
    log::error("movie: open failed: {}", av_err(error));
    return false;
  }
  if ((error = avformat_find_stream_info(state.fmt, nullptr)) < 0) {
    log::error("movie: stream info failed: {}", av_err(error));
    return false;
  }
  for (unsigned i = 0; i < state.fmt->nb_streams; ++i) {
    const AVCodecParameters* params = state.fmt->streams[i]->codecpar;
    if (params->codec_type == AVMEDIA_TYPE_VIDEO && state.vstream < 0) state.vstream = static_cast<int>(i);
    if (params->codec_type == AVMEDIA_TYPE_AUDIO && state.astream < 0) state.astream = static_cast<int>(i);
  }
  if (state.vstream < 0) {
    log::error("movie: no video stream");
    return false;
  }
  auto open_codec = [&](int idx, AVCodecContext*& ctx) {
    const AVCodecParameters* params = state.fmt->streams[idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
      log::error("movie: no decoder for stream {}", idx);
      return false;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, params) < 0) return false;
    if (idx == state.vstream) ctx->thread_count = 0;
    const int result = avcodec_open2(ctx, codec, nullptr);
    if (result < 0) log::error("movie: decoder open failed: {}", av_err(result));
    return result >= 0;
  };
  if (!open_codec(state.vstream, state.vctx)) return false;
  state.bink = state.vctx->codec_id == AV_CODEC_ID_BINKVIDEO || state.vctx->codec_id == AV_CODEC_ID_BINKVIDEO2;
  if (state.astream >= 0 && open_codec(state.astream, state.actx)) has_audio_ = true;
  width_ = static_cast<u32>(state.vctx->width);
  height_ = static_cast<u32>(state.vctx->height);
  const AVStream* video_stream = state.fmt->streams[state.vstream];
  const AVRational frame_rate = video_stream->avg_frame_rate.num ? video_stream->avg_frame_rate : video_stream->r_frame_rate;
  if (frame_rate.num > 0 && frame_rate.den > 0) fps_ = av_q2d(frame_rate);
  state.pkt = av_packet_alloc();
  state.frame = av_frame_alloc();
  return state.pkt && state.frame && width_ && height_;
}

bool Decoder::step() {
  Impl& state = *impl_;
  if (eof_) return false;

  auto drain_video = [&]() {
    for (;;) {
      if (avcodec_receive_frame(state.vctx, state.frame) < 0) return;
      VideoFrame video_frame;
      video_frame.index = state.vcount++;
      video_frame.pts = static_cast<double>(video_frame.index) / fps_;
      if (convert(*state.frame, state.bink, video_frame.rgba)) video_.push_back(std::move(video_frame));
      av_frame_unref(state.frame);
    }
  };
  auto drain_audio = [&]() {
    for (;;) {
      if (avcodec_receive_frame(state.actx, state.frame) < 0) return;
      const AVFrame& frame = *state.frame;
      const int channels = frame.ch_layout.nb_channels;
      const bool planar = frame.format == AV_SAMPLE_FMT_FLTP;
      if ((planar || frame.format == AV_SAMPLE_FMT_FLT) && channels >= 1 && frame.nb_samples > 0) {
        const size_t sample_count = static_cast<size_t>(frame.nb_samples);
        state.src.resize(sample_count * 2);
        for (size_t i = 0; i < sample_count; ++i) {
          for (int channel = 0; channel < 2; ++channel) {
            const int src_channel = std::min(channel, channels - 1);
            state.src[i * 2 + static_cast<size_t>(channel)] =
                planar ? reinterpret_cast<const float*>(frame.data[src_channel])[i]
                       : reinterpret_cast<const float*>(frame.data[0])[i * static_cast<size_t>(channels) + static_cast<size_t>(src_channel)];
          }
        }
        // Output sample at source position p interpolates src[floor(p)] and src[floor(p)+1]; index -1 is rs_prev.
        const double ratio = static_cast<double>(frame.sample_rate ? frame.sample_rate : 44100) / kAudioRate;
        // p must stay below n-1 so src[i0+1] exists; p = n-1 is produced by the next chunk from rs_prev.
        const double limit = static_cast<double>(sample_count) - 1.0;
        while (state.rs_pos < limit) {
          const long i0 = static_cast<long>(std::floor(state.rs_pos));
          const float frac = static_cast<float>(state.rs_pos - static_cast<double>(i0));
          for (size_t channel = 0; channel < 2; ++channel) {
            const float prev = i0 < 0 ? state.rs_prev[channel] : state.src[static_cast<size_t>(i0) * 2 + channel];
            const float next = state.src[static_cast<size_t>(i0 + 1) * 2 + channel];
            audio_.push_back(prev + (next - prev) * frac);
          }
          state.rs_pos += ratio;
        }
        state.rs_pos -= static_cast<double>(sample_count);
        state.rs_prev[0] = state.src[(sample_count - 1) * 2];
        state.rs_prev[1] = state.src[(sample_count - 1) * 2 + 1];
      } else {
        log::error("movie: unsupported audio sample format {}", frame.format);
      }
      av_frame_unref(state.frame);
    }
  };

  if (!state.demux_done) {
    if (av_read_frame(state.fmt, state.pkt) >= 0) {
      if (state.pkt->stream_index == state.vstream) {
        if (avcodec_send_packet(state.vctx, state.pkt) >= 0) drain_video();
      } else if (has_audio_ && state.pkt->stream_index == state.astream) {
        if (avcodec_send_packet(state.actx, state.pkt) >= 0) drain_audio();
      }
      av_packet_unref(state.pkt);
      return true;
    }
    state.demux_done = true;
  }
  // End of file: flush both decoders.
  if (!state.flushed) {
    state.flushed = true;
    avcodec_send_packet(state.vctx, nullptr);
    drain_video();
    if (has_audio_) {
      avcodec_send_packet(state.actx, nullptr);
      drain_audio();
    }
  }
  eof_ = true;
  return false;
}

}  // namespace oc::movie
