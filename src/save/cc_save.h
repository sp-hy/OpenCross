#pragma once
// Chrono Cross (RDE) save layer: the 0x2000 slot block codec and the save.dat wrapper.
// Everything here is plain data (no dependency on asset importers).
//
// A slot block = 0x5AC raw head (header frame, icon, preview + raw game state) + 0x1975 bit-packed section stream
// + 0xDB raw tail + u32 checksum. decode_block() splits it into a SaveSlot; encode_block() is the exact inverse.
// Head and tail are kept verbatim so unknown fields survive a round trip.
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "save/encrypted_zip.h"
#include "save/ext_save.h"
#include "save/memcard.h"

namespace oc::save {

inline constexpr size_t kBlockSize = 0x2000;
// Block layout: raw head 0x0000..0x05AB, packed stream 0x05AC..0x1F20, raw tail 0x1F21..0x1FFB.
inline constexpr size_t kHeadSize = 0x5AC;
inline constexpr size_t kPackedSize = 0x1975;
inline constexpr size_t kTailSize = 0xDB;
inline constexpr size_t kChecksumOffset = 0x1FFC;
// u32 save version; 0x106 from RDE / the PS1 US build on.
inline constexpr size_t kVersionOffset = 0x1FF8;
inline constexpr u32 kSaveVersion = 0x106;
// Bit count word at 0x24 of the header summary stream (name 96 + party 24 + time 32 + gold 32 + stars 8 + location 16 + flags 2).
inline constexpr u32 kSummaryBits = 210;
// Size of the unpacked section image.
inline constexpr size_t kImageSize = 0xE1F4;
inline constexpr int kNumChars = 44;
// Highest face a summary party byte holds: the 44 characters, then the forms 0x2C..0x31.
inline constexpr u8 kLastFace = 0x31;
inline constexpr int kNumItems = 384;
inline constexpr int kNumElements = 192;

// Raw head offsets. Playtime is in frames; the preview playtime / gold are unaligned u32; key items are 64 bits,
// recruited 44 bits, party 3 bytes; kLeaderName is char[12] Shift-JIS; kFlags42 bits 0-1 = variable 0x7E & 3 (2 New Game+, 3 cleared).
namespace head {
inline constexpr size_t kLeaderName = 0x28;
inline constexpr size_t kPreviewParty = 0x34;
inline constexpr size_t kPreviewPlaytime = 0x37;
inline constexpr size_t kPreviewGold = 0x3B;
inline constexpr size_t kPreviewStars = 0x3F;
inline constexpr size_t kProgression = 0x40;
inline constexpr size_t kFlags42 = 0x42;
inline constexpr size_t kKeyItems = 0x180;
inline constexpr size_t kLocation = 0x18E;
inline constexpr size_t kPlaytime = 0x588;
inline constexpr size_t kGold = 0x58C;
inline constexpr size_t kRecruited = 0x590;
inline constexpr size_t kGrowthProgress = 0x598;
inline constexpr size_t kParty = 0x59C;
inline constexpr size_t kStarsTotal = 0x5A4;
inline constexpr size_t kStarsCurrent = 0x5A5;
}  // namespace head

// The OpenCross record in the raw tail (block 0x1F21..0x1FF7, bytes no loader reads): "OCX", version 1, then entries {u8 tag, u8 size,
// size bytes} up to a tag 0. A tail without the magic (any RDE / PS1 save) has no entries; writing an entry stamps the magic.
inline constexpr size_t kTailExtSize = 0xD7;
namespace tail_tag {
// The party reserve (party swap): character ids in slot order, 0xFF = an empty slot.
inline constexpr u8 kReserve = 1;
}  // namespace tail_tag

// Item stack: id stored in 9 bits, qty in 7 bits.
struct ItemStack {
  u16 id = 0;
  u8 qty = 0;
};

// One of the 44 characters: the saved parts of the 0xCC-byte record plus dynamic state, name and grid.
// Stored widths: max_hp 10 bits; the six stats 7 bits each; bonus values 7 bits; model_id 9 bits; grid cells (element id) 9 bits;
// unlocked-level masks 10 bits; equipped_qty 3-bit nibbles; name = game-font bytes, NUL terminated (6 symbols max).
// colour_bit is stored only for characters 0 and 43; status holds the condition bits.
struct SaveChar {
  u8 colour_bit = 0;
  u8 rec02 = 0;
  u8 rec_a8 = 0;
  u8 status = 0;
  u16 max_hp = 0;
  u16 cur_hp = 0;
  u16 hp_extra = 0;
  u16 unk_be = 0;
  u8 atk = 0, def = 0, hit = 0, evd = 0, mag = 0, mdef = 0;
  std::array<u8, 12> bonus{};
  u16 weapon = 0, armour = 0;
  std::array<u16, 3> accessory{};
  u16 model_id = 0;
  std::array<u16, 48> grid{};
  std::array<u16, 8> unlocked{};
  std::array<u8, 48> equipped_qty{};
  std::array<u8, 16> name{};
};

struct SaveSlot {
  std::array<u8, kHeadSize> head{};
  std::array<u8, kTailSize> tail{};
  std::array<SaveChar, kNumChars> chars{};
  std::array<ItemStack, kNumItems> items{};
  std::array<ItemStack, kNumElements> elements{};
  // Button configuration: field, battle, field analog, battle analog.
  std::array<u32, 4> button_cfg{};
  std::array<u32, 4> unk_e1b8{};
  u32 unk_e1e0 = 0, unk_e1e4 = 0, unk_e1ec = 0, unk_e1f0 = 0;
  // Obtained Frames bitfield (new game 0x203); the picker skips clear bits.
  u16 frames = 0x203;
  // Not saved (session only): the records come from char20.bin (the dream: boot / FE 2F state until FE 3F reloads char.bin).
  // OpenCross looks the file's record bytes (level masks, growth column) up in this set (game/new_game.h).
  bool dream_records = false;
  // Not saved: the block failed the loader's checksum. Its head still previews; a load is refused with the "corrupted" message.
  bool corrupted = false;

  // Raw-head accessors (writes keep the preview copies in sync where one exists).
  std::array<u8, 3> party() const;
  void set_party(const std::array<u8, 3>& ids);
  // The summary's party bytes (head 0x34..0x36): 0xFF for an empty seat, else the member's face: its record model byte when that
  // is at most 0x31 (a Pip form, Serge as Lynx), else its id. The save writes them when it stamps the block.
  std::array<u8, 3> preview_party() const { return {head[head::kPreviewParty], head[head::kPreviewParty + 1], head[head::kPreviewParty + 2]}; }
  void stamp_preview_party();
  u64 key_items() const;
  void set_key_items(u64 bits);
  u32 playtime_frames() const;
  void set_playtime_frames(u32 frame_count);
  u32 gold() const;
  void set_gold(u32 amount);
  u32 growth_progress() const;
  void set_growth_progress(u32 value);
  bool recruited(int char_id) const;
  void set_recruited(int char_id, bool on);
  // Summary copy of script variable 0 (SCENARIO), u16: the key of the slot list's location caption.
  u16 progression() const { return static_cast<u16>(head[head::kProgression] | (head[head::kProgression + 1] << 8)); }
  void set_progression(u16 value) {
    head[head::kProgression] = static_cast<u8>(value & 0xFF);
    head[head::kProgression + 1] = static_cast<u8>(value >> 8);
  }
  u16 location() const;
  // The tail record's entry `tag` (empty when absent). Setting an empty entry removes it; false when the record has no room.
  std::vector<u8> tail_entry(u8 tag) const;
  bool set_tail_entry(u8 tag, const std::vector<u8>& bytes);
  // The party reserve slots (tail_tag::kReserve): character ids, -1 = empty; empty when no reserve was ever written.
  std::vector<int> reserve() const;
  void set_reserve(const std::vector<int>& slots);
  u8 stars_total() const { return head[head::kStarsTotal]; }
  u8 stars_current() const { return head[head::kStarsCurrent]; }
  // Script variable `id` (0..0x3FE, even): the global variable block is the head at 0x188 + id.
  u16 var(int id) const { return static_cast<u16>(head[0x188 + static_cast<size_t>(id)] | (head[0x189 + static_cast<size_t>(id)] << 8)); }
  void set_var(int id, u16 v) {
    head[0x188 + static_cast<size_t>(id)] = static_cast<u8>(v & 0xFF);
    head[0x189 + static_cast<size_t>(id)] = static_cast<u8>(v >> 8);
  }
  // New Game+ state = variable 0x7E (head 0x206): 0 normal, 2 New Game+ / Continue+ game,
  // 3 cleared save (FE 85, the ending's save prompt).
  u16 ng_flag() const { return var(0x7E); }
  // Battle init doubles enemy HP when the flag is non-zero.
  bool new_game_plus() const { return ng_flag() != 0; }
  // The 2 flag bits of the slot summary stream (head 0x42 bits 0-1 = variable 0x7E & 3, written by the save).
  u8 summary_flags() const { return head[head::kFlags42] & 3; }
};

// Sum of bytes 0..0x1FF7 (what a save stores).
u32 block_sum(ByteSpan block);
// u32 save version at 0x1FF8 (0 for a short block).
u32 block_version(ByteSpan block);
// The loader's test: the u32 at 0x1FFC equals the sum of bytes 0x180..0x1FF7, plus 0x000..0x17F when the version is above 0x102.
bool block_checksum_ok(ByteSpan block);

// Section stream <-> 0xE1F4 image (the 54-step table): unpack 0x1975 bytes -> 0xE1F4, pack the reverse. `version` gates the last
// steps as the loader does: a step the version lacks is not in the stream and takes its default.
Bytes unpack_image(ByteSpan packed, u32 version = kSaveVersion);
Bytes pack_image(ByteSpan image);

// decode_block throws on wrong size; it reads the block's version gates and sets SaveSlot::corrupted when the checksum fails.
// encode_block returns 0x2000 bytes with version 0x106 and a recomputed checksum.
SaveSlot decode_block(ByteSpan block);
Bytes encode_block(const SaveSlot& slot);

// Memory-card slot helpers. Slot numbers are the CHRONOnn suffix (00 = auto-save).
struct SlotRef {
  int slot = 0;
  int block = 0;
  std::string name;
};
// File name is "BISLPSP02364CHRONOnn".
std::string slot_file_name(int slot);
std::vector<SlotRef> list_slots(const MemCard& card);
SaveSlot read_slot(const MemCard& card, int block);
// Rewrites the slot's existing file (JP or US name) in place, else allocates; returns the block, or 0 if the card is full.
int write_slot(MemCard& card, int slot, const SaveSlot& src);

// save.dat: ZIP of memcard1.mcr, memcard2.mcr, rd_save.dat, achievement.dat, ext_save.dat.
class SaveDat {
 public:
  // A card member that is not 0x20000 bytes is left out (card(n) == nullptr), as RDE's boot check does.
  static SaveDat open(ByteSpan bytes);
  static SaveDat load(const std::filesystem::path& path);
  // The save.dat RDE creates on its first boot: memcard1.mcr + memcard2.mcr, both freshly formatted, in that order.
  // ext_save.dat follows on the first save (write_ext); achievement.dat / rd_save.dat are not ours.
  static SaveDat create_empty();
  // RDE's boot rule: a card member that is missing or not 0x20000 bytes is replaced by a formatted card
  // (appended when absent). Returns true when something changed (the caller saves the file).
  bool ensure_cards();

  // card(n): n is 1 or 2; nullptr if the member is absent.
  MemCard* card(int card_number);
  const MemCard* card(int card_number) const;
  // Finds a member by name (ext_save.dat, achievement.dat, rd_save.dat, ...).
  const ZipMember* member(std::string_view name) const;
  const std::vector<ZipMember>& members() const { return members_; }

  // ext_save.dat. The host (language, battle counter, custom names) is set once by the program.
  void set_ext_host(ExtHost* host) { ext_host_ = host; }
  ExtHost* ext_host() const { return ext_host_; }
  // The member parsed by RDE's rule (defaults when absent / unusable).
  ExtFile ext() const;
  // Replaces / appends the member.
  void set_ext(const ExtFile& file);
  // Stores the host's state as slot (card, slot_no) (RDE does this on every manual save and auto-save). False = no host.
  bool write_ext(int card, int slot_no);
  // The slot's record as RDE's load hook sees it: the names only when the record's language == `language`; the battle counter replaces
  // the host's (always). Without the member, an empty record.
  ExtSlot read_ext(int card, int slot_no, u32 language) const;
  // read_ext without taking the battle counter: a save list looking at a file.
  ExtSlot peek_ext(int card, int slot_no, u32 language) const;

  // Cards are written back into their members.
  Bytes serialize(u64 salt_seed = 0) const;
  // Writes a temp file, then renames.
  void save(const std::filesystem::path& path) const;

 private:
  std::vector<ZipMember> members_;
  std::array<std::optional<MemCard>, 2> cards_;
  ExtHost* ext_host_ = nullptr;
};

}  // namespace oc::save
