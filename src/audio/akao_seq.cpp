#include "audio/akao_seq.h"

#include <cstring>
#include <set>

#include "audio/byte_io.h"

namespace oc::audio {

namespace {
// Operand byte counts for A0..E6 (-1 = finish channel / unimplemented).
constexpr s8 kArgs[0x47] = {
    /*A0*/ -1, 1, 1, 1, 2, 1, 0, 0, 1, 2, 1, 2, 1, 1, 1, 1,
    /*B0*/ 2, 1, 1, 0, 3, 1, 0, 1, 3, 1, 0, 1, 2, 1, 0, 1,
    /*C0*/ 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1,
    /*D0*/ 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 2, 2, 2,
    /*E0*/ 0, 1, 0, -1, 2, 2, 2};
// FE sub-opcode operand counts (-1 = finish).
constexpr s8 kFeArgs[0x20] = {2, 3, 2, 3, 0, 0, 2, 3, 3, 3, 1, 1, -1, -1, 2, 0,
                              1, 0, 2, 0, 1, 2, 2, -1, -1, 3, 0, 0, 1, 0, 0, -1};
}  // namespace

int akao_op_length(const u8* code, size_t avail) {
  if (avail == 0) return 0;
  const u8 op = code[0];
  int length;
  if (op < 0xA0) {
    if (op >= 0x9A) return 0;
    return 1;
  }
  if (op >= 0xF0) {
    if (op == 0xFF) return 0;
    if (op == 0xFE) {
      if (avail < 2) return 0;
      const u8 sub = code[1];
      if (sub >= 0x20 || kFeArgs[sub] < 0) return 0;
      length = 2 + kFeArgs[sub];
    } else {
      length = 2;
    }
  } else {
    if (op - 0xA0 >= int(sizeof(kArgs)) || kArgs[op - 0xA0] < 0) return 0;
    length = 1 + kArgs[op - 0xA0];
  }
  return size_t(length) <= avail ? length : 0;
}

std::shared_ptr<const AkaoSeq> parse_seq(Bytes data) {
  if (data.size() < 0x40 || std::memcmp(data.data(), "AKAO", 4) != 0) return nullptr;
  auto seq = std::make_shared<AkaoSeq>();
  const u8* header = data.data();
  seq->id = read_u16(header + 4);
  seq->bank_id = read_u32(header + 0x14);
  seq->reverb_mode = read_u16(header + 8);
  seq->channel_mask = read_u32(header + 0x20);
  seq->gate_mask = read_u32(header + 0x28);
  const s32 patch_rel = s32(read_u32(header + 0x30)), keymap_rel = s32(read_u32(header + 0x34));
  u32 table = 0x40;
  for (int channel = 0; channel < 32; ++channel) {
    if (!(seq->channel_mask >> channel & 1)) continue;
    if (table + 2 > data.size()) return nullptr;
    const u32 pc = table + read_u16(header + table);
    if (pc >= data.size()) return nullptr;
    seq->channels.emplace_back(channel, pc);
    table += 2;
  }
  auto code = std::make_shared<AkaoCode>();
  if (patch_rel) code->patch_abs = u32(0x30 + patch_rel);
  if (keymap_rel) code->keymap_abs = u32(0x34 + keymap_rel);
  if (code->patch_abs >= data.size()) code->patch_abs = 0;
  if (code->keymap_abs >= data.size()) code->keymap_abs = 0;
  code->data = std::move(data);
  seq->code = std::move(code);
  return seq;
}

// Each channel is decoded linearly from its start to the first opcode that finishes it; code past an infinite loop belongs to the
// next channel, so an id found there is that channel's.
std::vector<int> akao_sfx_triggers(const AkaoSeq& seq) {
  std::set<int> ids;
  if (!seq.code) return {};
  const Bytes& data = seq.code->data;
  for (const auto& [number, start] : seq.channels) {
    for (size_t pc = start; pc < data.size();) {
      const int length = akao_op_length(&data[pc], data.size() - pc);
      if (length == 0) break;
      if (data[pc] == 0xFE && data[pc + 1] == 0x0B) ids.insert(data[pc + 2]);
      pc += size_t(length);
    }
  }
  return {ids.begin(), ids.end()};
}

// The patch table holds 16 u16 offsets (>= 0x8000 = no patch); the zone list starts 0x20 past the offset and a zone with
// sustain mode 0 ends it.
std::vector<AkaoZone> akao_patch_zones(const AkaoCode& code, int patch) {
  std::vector<AkaoZone> out;
  if (!code.patch_abs || patch < 0 || patch >= 16 || code.patch_abs + 32 > code.data.size()) return out;
  const u16 off = read_u16(&code.data[code.patch_abs + size_t(patch) * 2]);
  if (off >= 0x8000) return out;
  size_t pos = size_t(code.patch_abs) + off + 0x20;
  while (pos + 8 <= code.data.size() && out.size() < 64) {
    const u8* zone = &code.data[pos];
    if (zone[5] == 0) break;
    out.push_back({zone[0], zone[1], zone[2], zone[3], zone[4], zone[5], zone[6], zone[7]});
    pos += 8;
  }
  return out;
}

}  // namespace oc::audio
