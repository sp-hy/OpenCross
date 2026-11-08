#pragma once
// SEAD "sabf" container (remastered BGM). Only the material table is parsed; the HCA or 16-bit PCM stream inside is decoded by hca_stream.
#include <optional>
#include <vector>

#include "core/types.h"

namespace oc::audio {

constexpr u8 kSabCodecPcm16 = 1;
constexpr u8 kSabCodecHca = 7;

struct SabMaterial {
  u8 channels = 0;
  u8 codec = 0;
  u32 sample_rate = 0;
  u32 loop_start = 0;     // samples; 0/0 = no loop
  u32 loop_end = 0;       // last sample, inclusive
  u32 extradata_size = 0;
  u32 stream_size = 0;
  u32 sound_id = 0;
  u8 encryption = 0;      // extradata byte 0x0d: 1 = frames XOR-obfuscated (all RDE files)
  u32 hca_offset = 0;     // file offset of the HCA header ("HCA\0"); 0 for a PCM material
  u32 data_offset = 0;    // file offset of the first HCA frame or PCM sample
  bool loops() const { return loop_end > loop_start; }
};

struct Sab {
  std::vector<SabMaterial> materials;
};

// nullopt on a bad magic / truncated tables. Offsets are validated against the buffer size.
std::optional<Sab> parse_sab(ByteSpan data);

}  // namespace oc::audio
