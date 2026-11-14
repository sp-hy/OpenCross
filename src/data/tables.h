#pragma once
// Typed loaders for every battle / menu table. All integers are little-endian in the
// files; records keep their raw bytes so later code can reach fields that are not decoded yet.
#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Element records past vanilla's 527 are a mod's own: their kind byte (+0x01, bit 1 = tech) says element or tech. Vanilla ids keep
// their blocks (1..0x7F elements and consumables, 0x80.. techs, enemy actions and specials), whatever their kind.
inline constexpr int kVanillaElemRecs = 527;
// Rows of the element shop and blacksmith tables (el_shop.bin, waa_shop.bin); a mod's shops by name follow them.
inline constexpr size_t kShopRows = 32;
// Field-script item ids (BB / B4 / D5, ItemWork text) from here name element `id - kScriptElementBase` (in a mod's room its own id,
// TableRows::element_for), any record: the vanilla
// range 0x200 + id ends at 511 where the key items start (0x401..). Operands carry 15 bits, so ids up to 0x6FFF.
inline constexpr int kScriptElementBase = 0x1000;
// The game's item records (equipment and materials: waa.bin, menu_waa.bin, waa_tbl.bin, names sysmes 0x00<id>); a mod's items by name
// follow them. A save's stream holds only these ids.
inline constexpr size_t kGameItems = 256;
// Field-script item ids from here (to kScriptElementBase) name item `id - kScriptItemBase`: a mod's own item ids (features::kModItemFirst..),
// resolved through the running room's owner as element ids are.
inline constexpr int kScriptItemBase = 0x800;
// The game's key items (1..64: the save's 64 bits; names sysmes 0x18<id>, descriptions keyitem.txt <id * 3 + line>); a mod's key items
// by name ([KeyItems]) follow them. Field-script item ids 0x400 + n name key item n (the game's scripts reach 1..63; past 64 a mod's
// own key item ids through the running room's owner).
inline constexpr size_t kGameKeyItems = 64;
inline constexpr int kScriptKeyItemBase = 0x400;
inline bool mod_element(const std::vector<ElemRec>& elements, int id) {
  return id >= kVanillaElemRecs && id < static_cast<int>(elements.size()) && (elements[static_cast<size_t>(id)].raw[1] & 0x02) == 0;
}

// What a stock element is, from its record's flag bytes +0x00 / +0x01, tested in this order: 0x40 summon, 0x02 trap, (b & 0x30) ==
// 0x10 consumable, 0x04 healing, 0x08 support, 0x01 with +0x01 bit 0 an attack element, anything else Other. The vanilla blocks come
// out as 1..0x24 attack, 0x25..0x30 summons, 0x31..0x54 support, 0x55..0x60 healing, 0x61..0x6B consumables, 0x6C..0x7D traps,
// 0x7E (the Chrono Cross) and 0x7F Other. Ids that are not elements (techs, enemy actions, a mod's techs) and ids without a record
// are Other.
enum class ElementKind : u8 { Attack, Summon, Support, Healing, Consumable, Trap, Other };
inline ElementKind element_kind(const std::vector<ElemRec>& elements, int id) {
  if (id <= 0 || id >= static_cast<int>(elements.size()) || (id >= 0x80 && !mod_element(elements, id))) return ElementKind::Other;
  const ElemRec& record = elements[static_cast<size_t>(id)];
  const u8 flags = record.raw[0];
  if (flags & 0x40) return ElementKind::Summon;
  if (flags & 0x02) return ElementKind::Trap;
  if ((flags & 0x30) == 0x10) return ElementKind::Consumable;
  if (flags & 0x04) return ElementKind::Healing;
  if (flags & 0x08) return ElementKind::Support;
  if ((flags & 0x01) && (record.raw[1] & 0x01)) return ElementKind::Attack;
  return ElementKind::Other;
}

// scenedat.bin record. boss() marks a star-up battle.
// wide = a slot's monster id that doesn't fit the record's byte (a monster mods add by name, data/table_rows.h); 0 = the byte's.
struct SceneRec {
  std::array<u8, 28> raw{};
  std::array<u16, 3> wide{};
  int enemy_id(int slot) const { return wide[static_cast<size_t>(slot)] ? wide[static_cast<size_t>(slot)] : raw[static_cast<size_t>(slot)]; }
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

// Equipment stat record (player.dat part 4 "waa.bin": 256 x 0x1C, index = item id; mods' items by name follow). Field notes: tier() is the material tier 0..9;
// klass() the weapon class (CRIT table index); sweeps() its bit 7 (rods and boomerangs: a PC's heavy hit strikes every enemy); mode() the
// weapon attack mode 0x10..0x1B (animation group);
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
  bool sweeps() const { return (raw[1] & 0x80) != 0; }
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

// Item info (exdat part 5 "menu_waa.bin": 256 x 12, then mods' items by name): who can equip, price, category. equip_lo / equip_hi form a 51-bit mask (bit =
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
// part 0 rows {n, b, s lo, s hi} (100); el_shop has trailing zeros trimmed (the smiths' waa_shop.bin is read by ui::shop::ShopData); gear = 256 records and the items mods add by
// name (empty if player.dat part 4 is missing); item_info and item_names = as many; forge = menu/waa_tbl.bin, per item id the units needed of material id 0..31; keyitem_jp_rows = u16
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
  std::vector<std::vector<u16>> el_shop;
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

  // Loads everything; returns false (and fills `err`) if a mandatory table is missing. `publish`: these are the app's tables, so the
  // named records' ids and the mods' own ids for their text are handed to what reads them later (table_rows(), the text); a load on
  // another thread (the background room scan) passes false and keeps them to itself.
  bool load(const vfs::Vfs& game_fs,std::string* err = nullptr, bool publish = true);
};

// The versus battle protocol: raised whenever the battle rules change in a way that would put two games of different builds out of step.
inline constexpr u32 kVersusRulesVersion = 1;
// The battle data fingerprint two versus players compare: 16 lower-case hex digits over kVersusRulesVersion and the parsed tables a
// battle reads (monster, character and element records, scenes, battlefield colours, gear, drops, the generator's table); names and
// the tables of the field and menus are left out.
std::string battle_data_fingerprint(const GameTables& tables);

// The rows of menu_pg.bin (16 bytes each, index = element id) with the mods' table rows applied (data/table_rows.h) when `game_fs` is
// the VFS they were set for: as many as there are elements once the mods' named records are added. Nothing when the file is missing.
std::optional<std::vector<std::array<u8, 16>>> read_menu_elements(const vfs::Vfs& game_fs);

class TableRows;
struct TableMod;
// Gives the mods' text their own ids' records (import::set_text_mod_ids): a mod's element and monster names and its battle text are
// written under its own ids, and the text moves them to the records `rows` gives those ids.
void set_text_mod_ids_of(const TableRows& rows, const std::vector<TableMod>& mods);

// Colour bit (0x04..0x80) -> index 0..5 green white black red yellow blue; -1 if none.
int colour_index_of_bit(u8 bit);
const char* colour_name(int idx);

}  // namespace oc::data
