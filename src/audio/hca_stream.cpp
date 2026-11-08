#include "audio/hca_stream.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include "clhca.h"
}

namespace oc::audio {
namespace {
constexpr u8 kSeadKey[256] = {
        0x3A,0x32,0x32,0x32,0x03,0x7E,0x12,0xF7,0xB2,0xE2,0xA2,0x67,0x32,0x32,0x22,0x32, // 00-0F
        0x32,0x52,0x16,0x1B,0x3C,0xA1,0x54,0x7B,0x1B,0x97,0xA6,0x93,0x1A,0x4B,0xAA,0xA6, // 10-1F
        0x7A,0x7B,0x1B,0x97,0xA6,0xF7,0x02,0xBB,0xAA,0xA6,0xBB,0xF7,0x2A,0x51,0xBE,0x03, // 20-2F
        0xF4,0x2A,0x51,0xBE,0x03,0xF4,0x2A,0x51,0xBE,0x12,0x06,0x56,0x27,0x32,0x32,0x36, // 30-3F
        0x32,0xB2,0x1A,0x3B,0xBC,0x91,0xD4,0x7B,0x58,0xFC,0x0B,0x55,0x2A,0x15,0xBC,0x40, // 40-4F
        0x92,0x0B,0x5B,0x7C,0x0A,0x95,0x12,0x35,0xB8,0x63,0xD2,0x0B,0x3B,0xF0,0xC7,0x14, // 50-5F
        0x51,0x5C,0x94,0x86,0x94,0x59,0x5C,0xFC,0x1B,0x17,0x3A,0x3F,0x6B,0x37,0x32,0x32, // 60-6F
        0x30,0x32,0x72,0x7A,0x13,0xB7,0x26,0x60,0x7A,0x13,0xB7,0x26,0x50,0xBA,0x13,0xB4, // 70-7F
        0x2A,0x50,0xBA,0x13,0xB5,0x2E,0x40,0xFA,0x13,0x95,0xAE,0x40,0x38,0x18,0x9A,0x92, // 80-8F
        0xB0,0x38,0x00,0xFA,0x12,0xB1,0x7E,0x00,0xDB,0x96,0xA1,0x7C,0x08,0xDB,0x9A,0x91, // 90-9F
        0xBC,0x08,0xD8,0x1A,0x86,0xE2,0x70,0x39,0x1F,0x86,0xE0,0x78,0x7E,0x03,0xE7,0x64, // A0-AF
        0x51,0x9C,0x8F,0x34,0x6F,0x4E,0x41,0xFC,0x0B,0xD5,0xAE,0x41,0xFC,0x0B,0xD5,0xAE, // B0-BF
        0x41,0xFC,0x3B,0x70,0x71,0x64,0x33,0x32,0x12,0x32,0x32,0x36,0x70,0x34,0x2B,0x56, // C0-CF
        0x22,0x70,0x3A,0x13,0xB7,0x26,0x60,0xBA,0x1B,0x94,0xAA,0x40,0x38,0x00,0xFA,0xB2, // D0-DF
        0xE2,0xA2,0x67,0x32,0x32,0x12,0x32,0xB2,0x32,0x32,0x32,0x32,0x75,0xA3,0x26,0x7B, // E0-EF
        0x83,0x26,0xF9,0x83,0x2E,0xFF,0xE3,0x16,0x7D,0xC0,0x1E,0x63,0x21,0x07,0xE3,0x01, // F0-FF
    };
}  // namespace

void HcaStream::HcaDeleter::operator()(clHCA* hca) const { clHCA_delete(hca); }

HcaStream::HcaStream() = default;
HcaStream::~HcaStream() = default;

// Trailing bytes short of a whole block are ignored.
bool HcaStream::open(ByteSpan sab_bytes) {
  auto sab = parse_sab(sab_bytes);
  if (!sab || sab->materials.empty()) return false;
  const SabMaterial& material = sab->materials[0];
  data_.assign(sab_bytes.begin(), sab_bytes.end());
  pcm_channels_ = 0;
  pos_ = 0;
  if (material.codec == kSabCodecPcm16) return open_pcm(material);
  if (material.codec != kSabCodecHca || material.channels != 2) return false;

  hca_.reset(clHCA_new());
  if (!hca_) return false;
  if (clHCA_isOurFile(data_.data() + material.hca_offset, u32(data_.size() - material.hca_offset)) < 0) return false;
  if (clHCA_DecodeHeader(hca_.get(), data_.data() + material.hca_offset, u32(data_.size() - material.hca_offset)) < 0) return false;
  clHCA_stInfo info{};
  if (clHCA_getInfo(hca_.get(), &info) < 0 || info.channelCount != 2 || info.samplesPerBlock != 1024 || info.blockSize == 0)
    return false;

  rate_ = info.samplingRate;
  delay_ = info.encoderDelay;
  block_size_ = info.blockSize;
  data_off_ = material.data_offset;
  encrypted_ = material.encryption != 0;
  key_start_ = material.sound_id & 0xFF;
  block_count_ = u32((data_.size() - data_off_) / block_size_);
  u64 raw = u64(block_count_) * 1024;
  total_ = raw > delay_ + info.encoderPadding ? u32(raw - delay_ - info.encoderPadding) : 0;
  loop_start_ = material.loop_start;
  loop_end_ = std::min(material.loop_end, total_ ? total_ - 1 : 0);
  if (!(loop_end_ > loop_start_)) loop_start_ = loop_end_ = 0;
  block_pcm_.assign(1024 * 2, 0);
  cur_block_ = 0xFFFFFFFFu;
  next_block_ = 0;
  return true;
}

// The samples start right after the material header and its extradata; the stream size bounds them, the file end too.
bool HcaStream::open_pcm(const SabMaterial& material) {
  if ((material.channels != 1 && material.channels != 2) || material.sample_rate != 48000) return false;
  const u32 frame_bytes = 2u * material.channels;
  const size_t available = std::min<size_t>(material.stream_size, data_.size() - material.data_offset);
  pcm_channels_ = material.channels;
  rate_ = material.sample_rate;
  data_off_ = material.data_offset;
  total_ = u32(available / frame_bytes);
  loop_start_ = material.loop_start;
  loop_end_ = std::min(material.loop_end, total_ ? total_ - 1 : 0);
  if (!(loop_end_ > loop_start_)) loop_start_ = loop_end_ = 0;
  return total_ > 0;
}

// SEAD frame obfuscation (material extradata byte 0x0d = 1): XOR with a fixed 256-byte table, indexed from (sound_id & 0xff) at
// the first frame byte.
Bytes HcaStream::block_bytes(u32 block) const {
  const size_t rel = size_t(block) * block_size_;
  Bytes bytes(data_.begin() + data_off_ + rel, data_.begin() + data_off_ + rel + block_size_);
  if (encrypted_)
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] ^= kSeadKey[(key_start_ + rel + i) & 0xFF];
  return bytes;
}

// The decoder keeps MDCT overlap state, so blocks must be fed consecutively; a non-consecutive block is primed from block - 1.
bool HcaStream::decode_block(u32 block) {
  if (block >= block_count_) {
    std::fill(block_pcm_.begin(), block_pcm_.end(), s16(0));
    cur_block_ = block;
    return false;
  }
  if (block != next_block_) {
    if (block > 0) {
      Bytes previous = block_bytes(block - 1);
      clHCA_DecodeReset(hca_.get());
      clHCA_DecodeBlock(hca_.get(), previous.data(), block_size_);
    } else {
      clHCA_DecodeReset(hca_.get());
    }
  }
  Bytes frame = block_bytes(block);
  bool ok = clHCA_DecodeBlock(hca_.get(), frame.data(), block_size_) >= 0;
  if (ok)
    clHCA_ReadSamples16(hca_.get(), block_pcm_.data());
  else
    std::fill(block_pcm_.begin(), block_pcm_.end(), s16(0));
  cur_block_ = block;
  next_block_ = block + 1;
  return ok;
}

void HcaStream::seek(u32 sample) {
  pos_ = std::min(sample, total_);
}

u32 HcaStream::read_pcm(s16* out, u32 frames) {
  u32 done = 0;
  while (done < frames) {
    if (looping_ && has_loop() && pos_ > loop_end_) pos_ = loop_start_;
    if (pos_ >= total_) break;
    const u32 limit = (looping_ && has_loop()) ? frames_until_wrap(pos_, loop_end_) : total_ - pos_;
    const u32 want = std::min(frames - done, limit);
    const u8* src = data_.data() + data_off_ + size_t(pos_) * 2 * pcm_channels_;
    for (u32 i = 0; i < want; ++i) {
      const u8* frame = src + size_t(i) * 2 * pcm_channels_;
      const s16 left = s16(frame[0] | (frame[1] << 8));
      out[size_t(done + i) * 2] = left;
      out[size_t(done + i) * 2 + 1] = pcm_channels_ == 2 ? s16(frame[2] | (frame[3] << 8)) : left;
    }
    done += want;
    pos_ += want;
  }
  return done;
}

u32 HcaStream::read(s16* out, u32 frames) {
  if (pcm_channels_) return read_pcm(out, frames);
  if (!hca_) return 0;
  u32 done = 0;
  while (done < frames) {
    if (looping_ && has_loop() && pos_ > loop_end_) pos_ = loop_start_;
    if (pos_ >= total_) break;
    u32 want = frames - done;
    u32 limit = (looping_ && has_loop()) ? frames_until_wrap(pos_, loop_end_) : total_ - pos_;
    want = std::min(want, limit);
    u32 raw = pos_ + delay_;
    u32 block = raw / 1024, off = raw % 1024;
    if (block != cur_block_) decode_block(block);
    want = std::min(want, 1024 - off);
    std::memcpy(out + size_t(done) * 2, block_pcm_.data() + size_t(off) * 2, size_t(want) * 2 * sizeof(s16));
    done += want;
    pos_ += want;
  }
  return done;
}

std::vector<s16> HcaStream::read(u32 frames) {
  std::vector<s16> pcm(size_t(frames) * 2);
  pcm.resize(size_t(read(pcm.data(), frames)) * 2);
  return pcm;
}

}  // namespace oc::audio
