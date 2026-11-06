#pragma once
// Typed loaders for every battle / menu table. All integers are little-endian in the
// files; records keep their raw bytes so later code can reach fields that are not decoded yet.
#include <array>
#include <map>
#include <string>
#include <utility>

#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::data {

constexpr size_t kStatRecSize = 0xB0;
constexpr size_t kElemRecSize = 0x2C;
constexpr int kNumChars = 51;
constexpr int kGridRows = 8, kGridCols = 10, kGridCells = 80;

// monster.bin / char*.bin record (shared layout). Field notes: innate() is a colour bit (04 green 08 white 10 black 20 red 40 yellow
// 80 blue); growth_col() is the el_mask column (players); gender() is 0 random, 0x40 male, 0x80/0xA0 female; drop_base() is the "x" of
// the drop roll (150..200); ap() is the action-period seed; weapon_model() is 0 none, 1..17 = battle/wepon/*.kmd; cell_listed() covers
// 48 cells in mask order.
struct StatRec {
  std::array<u8, kStatRecSize> raw{};
  u8 innate() const { return raw[0x01]; }
  u8 growth_col() const { return raw[0x03]; }
  u8 gender() const { return raw[0x15]; }
  u16 max_hp() const { return rd16(0x16); }
  u8 atk() const { return raw[0x18]; }
  u8 def() const { return raw[0x19]; }
  u8 hit() const { return raw[0x1A]; }
  u8 evd() const { return raw[0x1B]; }
  u8 mag() const { return raw[0x1C]; }
  u8 mdef() const { return raw[0x1D]; }
  u8 status_dur() const { return raw[0x1E]; }
  u8 stamina_rate() const { return raw[0x1F]; }
  u16 weapon() const { return rd16(0x20); }
  u16 armour() const { return rd16(0x22); }
  u16 accessory(int i) const { return rd16(static_cast<size_t>(0x24 + 2 * i)); }
  u8 drop_base() const { return raw[0x2C]; }
  u8 model() const { return raw[0x2D]; }
  u8 ap() const { return raw[0x2E]; }
  u8 weapon_model() const { return raw[0x2F]; }
  u16 level_mask(int row) const { return rd16(static_cast<size_t>(0x30 + 2 * row)); }
  u16 cell_listed(int i) const { return rd16(static_cast<size_t>(0x40 + 2 * i)); }
  u16 drop_row() const { return rd16(0xA0); }
  u16 gold() const { return rd16(0xA2); }
  u8 hit_med() const { return raw[0xA4]; }
  u8 hit_heavy() const { return raw[0xA5]; }
  // A creature form's growth: the stars left above star_threshold() raise ATK DEF MAG MDEF by the 2-bit selectors of star_growth().
  u8 star_threshold() const { return raw[0xA6] & 0x7F; }
  u8 star_growth() const { return raw[0xA7]; }
  u16 type_bits() const { return static_cast<u16>((raw[0xAE] << 8) | raw[0xAF]); }
  u16 rd16(size_t offset) const { return static_cast<u16>(raw[offset] | (raw[offset + 1] << 8)); }
  u32 rd32(size_t offset) const { return static_cast<u32>(rd16(offset)) | (static_cast<u32>(rd16(offset + 2)) << 16); }
  // Expands the mask + listed cells to the 8x10 grid (value 0 where no cell exists). Raw cell values: bit 14 set on
  // every populated player cell in the files, bit 15 weapon tech, low bits the element id (0 = empty).
  std::array<u16, kGridCells> grid() const;
};

struct Lv99 {
  u16 hp = 0;
  u8 atk = 0, def = 0, hit = 0, evd = 0, mag = 0, mdef = 0;
};

// pgdat.bin record. colour_bit() is 0 for none.
struct ElemRec {
  std::array<u8, kElemRecSize> raw{};
  u8 b(size_t offset) const { return raw[offset]; }
  u8 flags0() const { return raw[0]; }
  u8 colour_bit() const { return raw[5]; }
  u8 level() const { return raw[6]; }
  u8 mode() const { return raw[7]; }
  u8 power() const { return raw[8]; }
  u8 status_pct() const { return raw[0xA]; }
  u8 variance() const { return raw[0xC]; }
  u8 hits() const { return raw[0xE]; }
  u32 status_lo() const { return rd32(0x18); }
  u32 status_hi() const { return rd32(0x1C); }
  u16 rd16(size_t offset) const { return static_cast<u16>(raw[offset] | (raw[offset + 1] << 8)); }
  u32 rd32(size_t offset) const {
    return static_cast<u32>(raw[offset] | (raw[offset + 1] << 8) | (raw[offset + 2] << 16)) | (static_cast<u32>(raw[offset + 3]) << 24);
  }
  bool empty() const {
    for (u8 byte : raw)
      if (byte) return false;
    return true;
  }
};

// scenedat.bin record. boss() marks a star-up battle.
struct SceneRec {
  std::array<u8, 28> raw{};
  u8 enemy_id(int slot) const { return raw[static_cast<size_t>(slot)]; }
  u8 enemy_count(int slot) const { return raw[static_cast<size_t>(3 + slot)]; }
  bool boss() const { return (raw[0x1A] & 0x80) != 0; }
  u8 bg() const { return raw[0x1A] & 0x7F; }
};

struct DropRow {
  std::array<u16, 4> slot{};
};
struct TradeEntry {
  u16 id = 0, qty = 0;
};

// Equipment stat record (player.dat part 4 "waa.bin": 256 x 0x1C, index = item id). Field notes: tier() is the material tier 0..9;
// klass() the weapon class (CRIT table index), bit 7 = multi/ranged; mode() the weapon attack mode 0x10..0x1B (animation group);
// colour() the colour bits of materials / plates; power() is weapon wp | armour DEF term | accessory value; mag() weapon MAG bonus |
// armour MDEF term; acc() weapon accuracy | armour EVD bonus; var() the weapon / armour damage variance.
constexpr size_t kGearRecSize = 0x1C;
namespace gearflag {
constexpr u16 kHpPct = 0x01, kGold = 0x02, kDmgUp = 0x04, kDmgDown = 0x08, kStartGauge = 0x10, kOffence = 0x20,
              kDefence = 0x40, kStamina = 0x80;
}
struct GearRec {
  std::array<u8, kGearRecSize> raw{};
  u8 tier() const { return raw[0]; }
  u8 klass() const { return raw[1] & 0x1F; }
  u8 mode() const { return raw[2]; }
  u8 colour() const { return raw[3]; }
  u8 power() const { return raw[4]; }
  u8 mag() const { return raw[5]; }
  u8 acc() const { return raw[6]; }
  u8 var() const { return raw[7]; }
  u16 flags() const { return rd16(8); }
  u16 slot_mask() const { return rd16(0xA); }
  u32 mask_lo() const { return rd32(0xC); }
  u32 mask_hi() const { return rd32(0x10); }
  u32 status_lo() const { return rd32(0x14); }
  u32 status_hi() const { return rd32(0x18); }
  u16 rd16(size_t offset) const { return static_cast<u16>(raw[offset] | (raw[offset + 1] << 8)); }
  u32 rd32(size_t offset) const { return static_cast<u32>(rd16(offset)) | (static_cast<u32>(rd16(offset + 2)) << 16); }
};

// Item info (exdat part 5 "menu_waa.bin": 256 x 12): who can equip, price, category. equip_lo / equip_hi form a 51-bit mask (bit =
// character id); price10 is the price / 10; cat's low nibble is 0 weapon 1 armour 2 accessory 4 material, its high nibble the tier.
struct ItemInfo {
  u32 equip_lo = 0, equip_hi = 0;
  s16 price10 = 0;
  u8 cat = 0, extra = 0;
  bool can_equip(int char_id) const {
    return char_id < 32 ? ((equip_lo >> char_id) & 1) != 0 : char_id < 64 && ((equip_hi >> (char_id - 32)) & 1) != 0;
  }
  int price() const { return price10 * 10; }
  int kind() const { return cat & 0xF; }
};

struct CharSet {
  std::array<StatRec, kNumChars> rec;
  std::array<Lv99, kNumChars> lv99;
};

// The loaded tables. Sources: enemies = monster.bin; char_sets = "char", "char20", "char_w0".."char_w5"; elements = pgdat.bin;
// growth = el_mask.bin (100 x 8); scenes = scenedat.bin; symtbl = 384 x 8; btbgdat = 100 x 3; drops = ditem (256); star_rows = exdat
// part 0 rows {n, b, s lo, s hi} (100); el_shop / waa_shop have trailing zeros trimmed; gear = 256 records (empty if player.dat part 4
// is missing); item_info = 256; forge = menu/waa_tbl.bin, per item id the units needed of material id 0..31; keyitem_jp_rows = u16
// offsets in keyitem.dat; rng_table = the 256-byte permutation read from the executable (empty if absent); variant_bands = the 8
// encounter-variant roll bands {lo, hi} of the battle loader btlinit.prg (empty if absent).
struct GameTables {
  std::vector<StatRec> enemies;
  std::map<std::string, CharSet> char_sets;
  std::vector<ElemRec> elements;
  std::vector<std::array<u8, 8>> growth;
  std::vector<SceneRec> scenes;
  std::vector<u16> symtbl;
  std::vector<std::array<u8, 3>> btbgdat;
  std::vector<DropRow> drops;
  std::vector<std::array<u8, 4>> star_rows;
  std::vector<std::vector<u16>> el_shop, waa_shop;
  std::vector<std::vector<TradeEntry>> trade;
  std::vector<GearRec> gear;
  std::vector<ItemInfo> item_info;
  std::vector<std::array<u8, 32>> forge;
  u32 keyitem_jp_rows = 0;

  // Names (EN column of lang.dat sysmes.txt). Empty string when absent.
  std::vector<std::string> enemy_names, element_names, char_names, item_names, key_item_names;
  std::vector<u8> rng_table;
  std::vector<std::array<u16, 2>> variant_bands;

  const CharSet& chars(const std::string& set = "char") const { return char_sets.at(set); }
  const std::string& enemy_name(int i) const;
  const std::string& element_name(int i) const;
  const std::string& char_name(int i) const;
  const std::string& item_name(int i) const;
  const GearRec* gear_rec(int item_id) const { return item_id >= 0 && item_id < static_cast<int>(gear.size()) ? &gear[static_cast<size_t>(item_id)] : nullptr; }
  const ItemInfo* item(int item_id) const { return item_id >= 0 && item_id < static_cast<int>(item_info.size()) ? &item_info[static_cast<size_t>(item_id)] : nullptr; }
  // Scene for a battle id: id >= 0x200 -> id-0x200, else symtbl[id*8+variant].
  int scene_for(int battle_id, int variant) const;

  // Loads everything; returns false (and fills `err`) if a mandatory table is missing.
  bool load(const vfs::Vfs& game_fs,std::string* err = nullptr);
};

// Colour bit (0x04..0x80) -> index 0..5 green white black red yellow blue; -1 if none.
int colour_index_of_bit(u8 bit);
const char* colour_name(int idx);

}  // namespace oc::data
