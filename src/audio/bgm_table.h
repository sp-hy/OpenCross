#pragma once
// Song id -> remastered BGM file: the 68-entry song table of the RDE exe. The lengths and loop points are the SEAD material
// header values (loop_end inclusive), kept here so tests and tools can cross-check a decode.
#include <string>

#include "core/types.h"

namespace oc::audio {

struct BgmEntry {
  u8 song_id;           // the game's music number
  u8 file_no;           // BGMnnn.sab
  u32 total_samples;    // HCA frames*1024 - mute_header - mute_footer
  u32 loop_start;       // 0/0 = plays once
  u32 loop_end;         // last sample, inclusive
};

constexpr int kBgmSongCount = 68;

const BgmEntry* bgm_entry(int song_id);        // nullptr if out of range
const BgmEntry* bgm_entry_by_file(int file_no);  // nullptr if the file is not referenced by the song table
std::string bgm_path(int file_no);             // "cdrom/sound/bgm/BGM002.sab"

// The encounter song: the battle theme 0x14 on both sources, or the boss theme 0x31 for a battle request with flag 0x40.
constexpr int kBattleSong = 0x14, kBossBattleSong = 0x31;
constexpr int battle_song(int battle_flags) { return (battle_flags & 0x40) ? kBossBattleSong : kBattleSong; }

}  // namespace oc::audio
