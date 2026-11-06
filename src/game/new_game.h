#pragma once
// The new-game initialisation of the main executable, as pure transforms on save::SaveSlot:
//   * boot / title (and field op FE 2F): the DREAM state the opening plays in - every record from char20.bin, 20 stars, growth for
//     20 stars, the star unlocks of stars 0..19, options 2 + default pad tables + play time 0, party / key items / roster reset,
//     frames 0x203, variables 0..0x3CF = 0;
//   * the real start (field op FE 3F's normal branch, and the New Game+ path): every record from char.bin, names kept, stars 0,
//     growth for 0 stars, no star unlocks, element nibbles 5;
//   * RDE's hook at the end of the party reset: key item 62 Time Shifter on every new game (and its load hook on every load:
//     ensure_time_shifter).
// The record set a state was built from is SaveSlot::dream_records (char20 vs char): GameState::rec / the battle party read it.
#include <array>

#include "core/types.h"
#include "save/cc_save.h"

namespace oc::data {
struct GameTables;
}

namespace oc::game::newgame {

// The unlock masks every record starts with (8 u16, one per element level): levels 1-3, column 6.
inline constexpr std::array<u16, 8> kInitialUnlocks = {0x40, 0x40, 0x40, 0, 0, 0, 0, 0};
// The pad remap tables (field, battle, field analog, battle analog).
inline constexpr std::array<u32, 4> kDefaultButtons = {0x76543210u, 0x76543210u, 0x3210u, 0x3210u};
// The 3-bit element-count nibbles of every grid cell on a fresh start.
inline constexpr u8 kEquippedFill = 5;
// Default options: Accuracy Selection = Auto.
inline constexpr u16 kOptionsDefault = 2;
inline constexpr u8 kDreamStars = 0x14;
inline constexpr u16 kNewGameFrames = 0x203;
// Key item 62.
inline constexpr u64 kKeyTimeShifter = 1ull << 61;

// "char20" (dream) or "char".
const char* record_set(bool dream);

// The 24 words of variables 0x3D0..0x3FE of a fresh game (var 0x3D2 = 0x3F).
std::array<u16, 24> globals();

// Loads the record set of `dream`: every record = the file's 0xB0 bytes (status 0, current HP = max HP, growth bytes 0), unlock
// masks = kInitialUnlocks, element nibbles 0; the 384 items / 192 elements emptied; gold 100; the growth progress word 0;
// variables 0x3D0.. = globals(). reset_names: every name back to the default (OpenCross: an empty record = the language's default name).
void load_records(const data::GameTables* tables, save::SaveSlot& slot, bool dream, bool reset_names);
// Per record the growth snapshot / pending values from lv99para.bin for the current star count (divisor 101 - stars,
// battle::growth_roll; HP + 2). `seed` seeds the roll (the PS1's rand state is not reproduced).
void derive_growth(const data::GameTables* tables, save::SaveSlot& slot, bool dream, u32 seed);
// Every record's masks |= el_mask[star][growth column] for star = 0 .. stars - 1 (no cell-existence test, as the PS1); `fresh`
// also sets every element nibble to kEquippedFill. (The PS1 also rewrites the grid lock bit 0x4000 from the masks; OpenCross
// derives the lock from the masks: GameState::cell_available.)
void star_unlocks(const data::GameTables* tables, save::SaveSlot& slot, bool dream, bool fresh);
// Party {Serge, -, -}, key items = Time Shifter only (the PS1: none), recruited = Serge, lock bitmap = 1, the other roster bitmaps
// 0. The relief member is not touched.
void reset_party(save::SaveSlot& slot);
// Options 2, pad tables kDefaultButtons, play time 0.
void reset_options(save::SaveSlot& slot);
// char.bin records (names kept), stars 0, growth, no star unlocks, nibbles 5. dream_records = false.
void real_records(const data::GameTables* tables, save::SaveSlot& slot, u32 seed);
// The boot / title state: dream records, names reset, 20 stars, relief member 0xFF, Serge alone recruited, frames 0x203,
// variables 0..0x3CF = 0, the two carry-over bitmaps 0. dream_records = true.
save::SaveSlot boot_state(const data::GameTables* tables, u32 seed);
// RDE's load hook: every loaded game gets the Time Shifter.
void ensure_time_shifter(save::SaveSlot& slot);

// Record 43; its forms are char.bin / lv99para.bin records 0x2B..0x30 (the model byte of the record).
inline constexpr int kPip = 43;
// The growth column (record +3): Pip in a form takes the form record's column (char.bin 43..48: 3 3 3 4 5 6), everybody else his
// own record's.
int growth_column(const data::GameTables* tables, const save::SaveSlot& slot, int char_id, bool dream);
// RDE's hook at the end of the carry-over: Pip in a form 0x2B..0x30 gets his 7 pending growth values re-rolled against lv99para.bin
// record <form>: with stars < 99, pending = rand() % ((target - snapshot) / (99 - stars) + 1), 0 when the snapshot is not below the
// target; with 99 stars all 0. Then the 8 host words `pip_masks` are ORed into Pip's masks (and unlock those cells): the masks of the
// save Continue+ loaded, null = 0 (New Game+).
void rde_playinit_end(const data::GameTables* tables, save::SaveSlot& slot, const std::array<u16, 8>* pip_masks, u32 seed);
// RDE's Pip stat sync, only with 99 stars and Pip in a form 0x2B..0x30: max HP and the six stats become the lv99para.bin record of
// the form, the current HP moves with max HP (clamped 0..the new max HP), the HP and stat snapshots take the new values; the pending
// growth words stay. Runs after a fight that changed Pip's form and on a card load whose ext_save.dat pair is not valid.
void pip_form_stats(const data::GameTables* tables, save::SaveSlot& slot);
// After a fight that changed Pip's form at 99 stars: his masks gain those of a full 99-star growth in the form, the new-game masks
// plus every el_mask cell of the form's growth column (no cell-existence test).
void pip_form_unlocks(const data::GameTables* tables, save::SaveSlot& slot);

}  // namespace oc::game::newgame
