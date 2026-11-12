#pragma once
// RDE's companion file ext_save.dat (a member of save.dat).
// 0xA634 bytes: "EXT\0", u32 version 0x104, 30 slot records of 0x588 B, 30 x {u8 form byte, u8 valid}.
// A record: u32 language, u32 battle counter, 44 x 32 B UTF-8 custom character names (NUL padded, empty = none).
// Slot index = 15 * (card - 1) + CHRONOnn number (0 = the auto-save).
#include <functional>
#include <map>
#include <string>

#include "core/types.h"

namespace oc::save {

inline constexpr size_t kExtSize = 0xA634;
// Legacy size (version 0x103): no pair table; RDE upgrades it.
inline constexpr size_t kExtLegacySize = 0xA5F8;
inline constexpr u32 kExtVersion = 0x104;
inline constexpr int kExtSlots = 30;
inline constexpr size_t kExtRecord = 0x588;
inline constexpr size_t kExtNameLen = 32;
inline constexpr int kExtNames = 44;

inline int ext_slot_index(int card, int slot_no) { return (card - 1) * 15 + slot_no; }

struct ExtSlot {
  // RDE language index: 0 en, 1 fr, 2 it, 3 de, 4 es, 5 jp.
  u32 language = 0;
  // Battle counter, capped at 0xFFFF by the game.
  u32 battles = 0;
  // Character id 0..43 -> custom name.
  std::map<int, std::u32string> names;
  // Pair table form byte (3..6) and its valid flag.
  u8 form = 0;
  u8 valid = 0;
  // OpenCross saves only (not in ext_save.dat): the stem of the mod room the save was made in, "" for a game room. Mod room ids are
  // each mod's own (game/mod_rooms.h), so Continue finds a mod room by this name, not by the location word.
  std::string room;
};

class ExtFile {
 public:
  // "EXT\0", 0x104, everything zero.
  ExtFile();
  // RDE's loader rule: 0xA634 + version 0x104 is taken as is; 0xA5F8 + 0x103 is upgraded; anything else gives the defaults.
  static ExtFile parse(ByteSpan bytes);
  const Bytes& bytes() const { return bytes_; }
  // index 0..29; out of range gives an empty slot.
  ExtSlot slot(int index) const;
  // Names: <= 31 UTF-8 bytes each (cut at a code point), ids outside 0..43 dropped.
  void set_slot(int index, const ExtSlot& record);

 private:
  Bytes bytes_;
};

// What the host program offers when a slot is written (set once on the SaveDat; the pointers must outlive it).
struct ExtHost {
  // RDE language index of the running game.
  u32 language = 0;
  // Won battles so far; replaced by the slot's counter on every load.
  u32 battles = 0;
  // The player's custom names (null = none).
  const std::map<int, std::u32string>* names = nullptr;
  // The current mod room's stem for OpenCross saves (ExtSlot::room; null or "" = a game room).
  std::function<std::string()> room;
};

std::string utf32_to_utf8(const std::u32string& text);
std::u32string utf8_to_utf32(const std::string& text);

}  // namespace oc::save
