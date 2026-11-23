#pragma once
// New Game+ / Continue+. Pure state transforms on save::SaveSlot / ui::GameState:
//   * the ending's save prompt (field op FE 85): every character of the roster gives its grid elements and gear back to the stock,
//     variable 0x7E = 3, then the save list (launcher mode 4) writes the "cleared" save;
//   * Continue of a cleared save re-enters the ending room with ROOM_PARAM 3: FE 84 keeps the cleared game in the carry-over buffer
//     and opens the title list in variant 2 (launcher mode 0x30): "New Game +" / "Continue +";
//   * the carry-over takes the cleared game into the new game (New Game+) or into a loaded save (Continue+);
//   * FE 3F (Serge's room at the real start): the dream party is dropped; a normal game also gets fresh data, a New Game+ keeps the
//     carried state and gets the Relief Charm + Time Shifter.
#include <array>
#include <span>
#include <vector>

#include "core/types.h"
#include "save/cc_save.h"

namespace oc::data {
struct ElemRec;
struct GameTables;
}
namespace oc::ui {
class GameState;
}

namespace oc::game::ngplus {

// The New Game+ state variable; its values: 2 = a New Game+ / Continue+ game, 3 = the ending's save.
inline constexpr int kFlagVar = 0x7E;
inline constexpr u16 kFlagPlus = 2;
inline constexpr u16 kFlagCleared = 3;
// Key items (64 bits; bit n = key item n + 1): Time Egg is key item 53 (Continue+ only), Relief Charm 61, Time Shifter 62 (the
// L2 / R2 speed gate).
inline constexpr u64 kKeyTimeEgg = 1ull << 52;
inline constexpr u64 kKeyReliefCharm = 1ull << 60;
inline constexpr u64 kKeyTimeShifter = 1ull << 61;
// Item ids that are NOT carried (Mastermune, Einlanzer x2, Hero's Medal / Shield / Blade, Memento Pendant, Blue Brooch, Dragoon's
// Honor, Star Fragment, Master Hammer). Compared as bytes: only ids < 0x100 can match.
inline constexpr std::array<u8, 11> kNotCarriedItems = {0x26, 0x3F, 0x40, 0xC0, 0xC1, 0x3D, 0xD6, 0xEC, 0xDA, 0xFD, 0x58};
inline constexpr u32 kGoldCap = 9999999;
// The ending room ("ending", ids 0 and 9) and its entrance for the NG+ title (the FE 85 script saves with `SET $3A, #3` in map 9).
inline constexpr int kEndingRoomId = 9;
inline constexpr int kNgTitleParam = 3;

// Launcher mode -> title variant (1 title list, 2 New Game+ list, 3 save list only; 0 = main menu).
int title_variant(int launcher_mode);
// The field op that selects the launcher mode (the FE 61 after it waits for the screen): FE 59 -> 3, FE 84 -> 0x30, FE 85 -> 4.
int launcher_mode_for_op(int fe_op);

// Quantity != 0 is tested separately.
bool carries_item(int id);
// By the record's kind (data::element_kind): every element but the summons and the Other kind (the Chrono Cross, ids that are not
// elements); vanilla that is the ids below 0x7E outside 0x25..0x30.
bool carries_element(const std::vector<data::ElemRec>& elements, int id);

// Why the Continue+ list (variant 2) refuses a slot: the 2 summary flag bits, bit 0 -> "This game data has been completed.".
// Inline: the title list (oc_title) uses it.
enum class Refusal : u8 { None, Completed, AlreadyPlus };
inline Refusal refusal(const save::SaveSlot& slot) {
  const u8 flags = slot.summary_flags();
  return (flags & 1) ? Refusal::Completed : (flags & 2) ? Refusal::AlreadyPlus : Refusal::None;
}

// Stock add of the field: the slot already holding the id, else the first slot with id 0 or quantity 0; quantity += count, capped
// at 99; false when the stock is full.
bool add_stack(std::span<save::ItemStack> stock, int id, int count);

// The carry-over on top of `dst` (the fresh new game, or the save Continue+ loaded):
//   character records (all 44): everything from `cleared` except the innate colour, gear, model byte and grid; status byte cleared;
//   items / elements added (not-carried lists);  carry-over bitmaps = cleared bitmaps | recruited | roster;  gold += cleared gold
//   (cap 9999999);  names of characters neither recruited nor in the roster of `dst`;  unlock masks and the stars copied;  frames and
//   the Dopplegang pool (vars 0x3E0..) ORed;  variable 0x7E = 2;  key items |= Relief Charm + Time Shifter.  With `tables`:
//   newgame::star_unlocks (the star unlocks of stars 0 .. stars - 1 ORed into the copied masks).
void carry_over(const save::SaveSlot& cleared, save::SaveSlot& dst, const data::GameTables* tables = nullptr);
// Continue+: carry_over + the Time Egg.
void continue_plus(const save::SaveSlot& cleared, save::SaveSlot& loaded, const data::GameTables* tables = nullptr);

// A fresh character record from char.bin: stats, HP, gear, model, grid, innate colour, the unlock masks newgame::kInitialUnlocks
// (`tables` null = zeros).
void fresh_record(const data::GameTables* tables, int id, save::SaveChar& out);
// The new game of New Game+ before the carry (newgame::boot_state, then newgame::real_records, Serge un-recruited around the carry,
// variable 0x42 = 0): char.bin records with their growth / nibbles 5, Serge alone recruited and in the party, relief 0xFF, gold 100,
// options 2 + default pad tables, play time 0, frames 0x203, vars 0x3D0.. from the exe table, MAP_ID 8, DEBUG_MODE kept; then
// carry_over(cleared, tables) with every name. The ending room's script that follows names Serge (FE 4A) and builds the dream
// party (Serge, Kid, random member) before MAP_JUMP_DC 22.
save::SaveSlot new_game_plus_slot(const data::GameTables* tables, const save::SaveSlot& cleared);

// FE 85: characters recruited or in the roster return their grid elements (stacks with their copies) and their five gear slots to
// the stock; variable 0x7E = 3 (the caller sets the VM variable).
void strip_for_cleared_save(ui::GameState& gs);

// FE 3F. Both branches: newgame::reset_party = party {Serge, -, -}, key items = Time Shifter, recruited = Serge, lock bitmap = 1,
// the other roster bitmaps 0. Variable 0x7E != 0: key items |= Relief Charm + Time Shifter. Variable 0x7E == 0: newgame::real_records
// first (char.bin records with names kept, growth for 0 stars, initial masks, nibbles 5, items / elements cleared, gold 100, stars 0,
// vars 0x3D0..0x3FE from the exe table), frames 0x203 (the caller writes the variables into the VM: new_game_globals()). `seed`
// seeds the growth roll.
std::array<u16, 24> new_game_globals();
void fe3f(ui::GameState& gs, bool ng_plus, u32 seed = 1);

}  // namespace oc::game::ngplus
