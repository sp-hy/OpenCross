#include "audio/sab.h"

#include <cstring>

#include "audio/byte_io.h"

namespace oc::audio {

// Section table at 0x20: 4 x { tag[4], u16 0, u16 0x10, u32 offset, u32 0 }; the "mtrl" section holds u32 count at +4 and
// the material offsets (relative to the section) from +16. A material is 0x20 bytes of header followed by its extradata and the stream: an
// HCA material has at least 0x10 bytes of extradata (the HCA header inside), a 16-bit PCM material may have none.
std::optional<Sab> parse_sab(ByteSpan data) {
  if (data.size() < 0x60 || std::memcmp(data.data(), "sabf", 4) != 0) return std::nullopt;
  u32 mtrl = 0;
  for (size_t i = 0; i < 4; ++i) {
    size_t section = 0x20 + i * 16;
    if (std::memcmp(data.data() + section, "mtrl", 4) == 0) mtrl = read_u32(data, section + 8);
  }
  if (mtrl == 0 || size_t(mtrl) + 16 > data.size()) return std::nullopt;
  u32 count = read_u32(data, mtrl + 4);
  if (count == 0 || count > 256 || size_t(mtrl) + 16 + size_t(count) * 4 > data.size()) return std::nullopt;

  Sab sab;
  sab.materials.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    size_t entry = size_t(mtrl) + read_u32(data, size_t(mtrl) + 16 + size_t(i) * 4);
    if (entry + 0x30 > data.size()) return std::nullopt;
    SabMaterial material;
    material.channels = data[entry + 4];
    material.codec = data[entry + 5];
    material.sample_rate = read_u32(data, entry + 8);
    material.loop_start = read_u32(data, entry + 0x0C);
    material.loop_end = read_u32(data, entry + 0x10);
    material.extradata_size = read_u32(data, entry + 0x14);
    material.stream_size = read_u32(data, entry + 0x18);
    material.sound_id = read_u32(data, entry + 0x1C);
    size_t data_off = entry + 0x20 + size_t(material.extradata_size);
    if (data_off > data.size()) return std::nullopt;
    if (material.codec == kSabCodecHca) {
      if (material.extradata_size < 0x10) return std::nullopt;
      material.encryption = data[entry + 0x20 + 0x0D];
      material.hca_offset = u32(entry + 0x30);
    }
    material.data_offset = u32(data_off);
    sab.materials.push_back(material);
  }
  return sab;
}

}  // namespace oc::audio
