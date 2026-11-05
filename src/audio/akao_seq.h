#pragma once
// AKAO sequence containers: music files (0x40-byte header, u16 channel-code offsets for each bit of the channel mask, optional
// patch / keymap regions) and bare programs (SFX). The player executes channel code directly from the shared byte buffer.
#include <memory>
#include <utility>
#include <vector>

#include "core/types.h"

namespace oc::audio {

// A byte buffer holding AKAO channel code plus the optional per-song instrument zones.
struct AkaoCode {
  Bytes data;
  u32 patch_abs = 0;   // patch table (FE 14), 0 = none
  u32 keymap_abs = 0;  // drum keymap (FE 04), 0 = none
};

struct AkaoSeq {
  std::shared_ptr<AkaoCode> code;
  u16 id = 0;
  u32 bank_id = 0;
  u16 reverb_mode = 4;  // header u16 at +8: the libspu reverb mode type set when the song starts (4 in all files)
  u32 channel_mask = 0;
  // Header u32 at +0x28: the music channels whose bit is set gate their notes at length - 2; every other channel starts with
  // full-length notes (the flag FE 1A sets and FE 1B clears).
  u32 gate_mask = 0;
  std::vector<std::pair<int, u32>> channels;  // (channel number, absolute pc)
};

// Parses a musicNNN.dat image. Null on a malformed header.
std::shared_ptr<const AkaoSeq> parse_seq(Bytes data);

// The effect00 ids the sequence's FE 0B ops request, ascending, each once.
std::vector<int> akao_sfx_triggers(const AkaoSeq& seq);

// Total length (opcode included) of the instruction at `code`, clamped by `avail`; 0 for opcodes that finish the channel or
// are truncated. Notes/ties/rests are 1 (2 for the explicit-length forms F0..FD).
int akao_op_length(const u8* code, size_t avail);

// Patch / keymap zone (8 bytes in the file).
struct AkaoZone {
  u8 instrument, low_key, high_key, ar, sr, mode, rr, volume;
};
// Patch zone list for `patch` (empty when absent). Keys are note numbers.
std::vector<AkaoZone> akao_patch_zones(const AkaoCode& code, int patch);

}  // namespace oc::audio
