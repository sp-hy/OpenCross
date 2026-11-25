#pragma once
// Table rows: a mod's tables\<table>.txt changes single fields of the records of one battle / menu table instead of replacing the
// whole file, and adds records by name, so several mods that each change or add a few records stack. The table is named after the file
// it changes (monster, char, char20, char_w0..char_w5, pgdat, menu_pg, waa, menu_waa, waa_tbl, scenedat, ditem, el_shop, waa_shop).
// Rows apply over the table as the game files (or a mod's whole-file replacement) give it, lowest priority first, so the first mod in
// the list wins a field; a field two mods set differently is logged. A record name is one record across all mods; named records
// follow the table's own records, mod by mod in priority order (each mod's names listed in the table's own-id section first, by its
// own id: [Elements] for pgdat, [Items] for waa, [ElementShops] for el_shop, [Smiths] for waa_shop). menu_pg follows pgdat, menu_waa
// and waa_tbl follow waa: their records are the same ids and names. The shop tables (el_shop, waa_shop) are lists: a row adds and
// removes entries instead of setting fields, so two mods' additions to one shop both land.
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/types.h"

namespace oc::vfs {
class ModManager;
class Vfs;
}  // namespace oc::vfs

namespace oc::data {

// The enemy slots at the start of a scenedat record (one byte each; a mod's monster id past 254 is kept beside the record).
inline constexpr u16 kSceneEnemySlots = 3;
// An enemy slot holding this id is empty, so a mod's monster with this id cannot stand in a formation slot.
inline constexpr size_t kEmptyEnemySlot = 0xFF;
// The highest element id: a grid cell keeps its element in the low 14 bits.
inline constexpr size_t kLastElementId = 0x3FFF;
// The highest item id: an equipment word below 0x200 is an item (higher words are elements and key items).
inline constexpr size_t kLastItemId = 0x1FF;

// One named field of a record: byte offset and width (1, 2 or 4 bytes, little-endian).
struct TableField {
  const char* name;
  u16 offset;
  u8 width;
};

// The fields a row may name for `table` (empty for an unknown table). A field of width 0 names a character and writes no record byte:
// equip_add / equip_remove of menu_waa and menu_pg (offset kCharacterAdd / kCharacterRemove: who may equip the item or place the
// element besides / unlike the record's bits), equips_like of the character sets (whose equipment and elements a character mods add
// takes); TableRows::character_writes keeps them for the loaders.
std::span<const TableField> table_fields(std::string_view table);
inline constexpr u16 kCharacterAdd = 0, kCharacterRemove = 1;
// The table whose record names and ids `table` shares (menu_pg -> pgdat, menu_waa and waa_tbl -> waa; otherwise the table itself).
std::string_view table_names_of(std::string_view table);
// Whether named rows may add records to `table` (its loader sizes it from the file).
bool table_grows(std::string_view table);
// Whether `table` is names only (key items: no record bytes), its records added by the mods' own-id sections ([KeyItems]).
bool table_declared_by_ids(std::string_view table);
// Whether `table` is a list table (a shop or trading post row: entries added and removed, apply_lists / apply_trade).
bool table_is_list(std::string_view table);
// The highest id a named record of `table` may take (pgdat and menu_pg: kLastElementId; waa, menu_waa and waa_tbl: kLastItemId); a
// name past it gets no record.
size_t table_last_id(std::string_view table);

// One trading post entry (menu/trade.bin: {u8 goods, u8 colour mask, u16 level sum}; a mod's goods may be any item id). levels 0 = a
// hole the post keeps its place for.
struct TradeEntry {
  u16 goods = 0;
  u8 colour = 0;
  u16 levels = 0;
};
// A trading post holds at most this many entries.
inline constexpr size_t kTradeSlots = 16;

// One mod's row files: table name (lower case) -> the file's bytes; `own_ids` = its mod.ini own-id sections (features::own_id_sections:
// table -> own id -> record name).
struct TableMod {
  std::string name;
  std::map<std::string, std::function<std::optional<Bytes>()>> files;
  std::map<std::string, std::vector<std::pair<int, std::string>>> own_ids;
};
// Set once at launch (priority order, first wins), before the tables are loaded. The rows belong to the tables read through `files`
// only: tables loaded through another VFS (the background scan's unmodified game) keep the files as they are.
void set_table_mods(const vfs::Vfs* files, std::vector<TableMod> mods);
// The mods set for `files`; null when they were set for another VFS, or never set.
const std::vector<TableMod>* table_mods(const vfs::Vfs& files);
// The table mods of the enabled mods `mods` scanned: their tables\<table>.txt files (read through `files`) and own-id sections.
std::vector<TableMod> table_mods_of(const vfs::ModManager& mods, const vfs::Vfs& files);

// What a row line does: set a field; in a list table add or remove an entry.
enum class RowOp : u8 { Set, Add, Remove };
// One field write parsed from a row file. With `ref_name` set the value is that record's id plus `value`.
struct RowWrite {
  RowOp op = RowOp::Set;
  u16 offset = 0;
  u8 width = 0;
  u32 value = 0;
  std::string ref_table, ref_name;
  // A list entry's words after its value ("add=<value> colour=16 levels=12"): name (lower case) and number.
  std::vector<std::pair<std::string, u32>> extra;
  int line = 0;
};
// One record of a row file: `[<index>]` (index set) or `[<name>]` (name set, `base` = the index a new record copies, if given).
// `[?<name>]` (`optional`): changes the record another mod adds by that name, and is skipped when no enabled mod adds it (a mod's
// changes to another mod's record that work with or without that mod); it adds nothing and takes no base.
struct RowBlock {
  std::optional<size_t> index;
  std::string name;
  std::optional<size_t> base;
  bool optional = false;
  // List tables: "clear=1" empties the row before this mod's adds.
  bool clear = false;
  int line = 0;
  std::vector<RowWrite> writes;
};
// Parses one file. A field line is "<name>=<value>" with a name from `fields`, or "0x<offset>:u8|u16|u32=<value>"; "base=<index>" in a
// named record. Values are decimal, 0x hex, negative decimal (two's complement in the field's width), or "@<table>:<record name>" with
// an optional "+<number>". In a list table (`list`) the lines are "add=<value>" (optionally followed by "<name>=<number>" words),
// "remove=<value>" and "clear=1". Blank lines and lines starting with ';' or '#' are skipped; lines that cannot be read go to
// `problems`.
std::vector<RowBlock> parse_rows(std::string_view text, std::span<const TableField> fields, std::vector<std::string>* problems,
                                 bool list = false);

// Every mod's rows, the named records' ids, and the mods' own element ids. Built once when the tables load (GameTables::load).
class TableRows {
 public:
  // Parses the mods' files and gives the named records ids: `counts` = each growable table's record count before rows.
  void build(const std::vector<TableMod>& mods, const std::map<std::string, size_t>& counts);
  // Records of `table` once the named ones are added (`count` = before).
  size_t total(std::string_view table, size_t count) const;
  // Applies the rows of `table` to records 0..count-1 (count = total()), lowest priority first; `record(i)` gives record i's bytes; a new
  // record is copied from its base first. Returns the number of fields written.
  size_t apply(std::string_view table, size_t count, const std::function<std::span<u8>(size_t)>& record) const;
  // A reference whose id doesn't fit its field (a formation slot's one byte, where 0xFF is empty) leaves the field as it is and is kept
  // here for the loader to hold beside the record (SceneRec::wide): the writes of `table` after apply(), the first mod's last.
  struct WideWrite {
    size_t record;
    u16 offset;
    u32 value;
  };
  const std::vector<WideWrite>& wide_writes(std::string_view table) const;
  // The character fields (width 0) of `table` after apply(), in the order applied (the first mod's last): offset = the field, value =
  // the character.
  const std::vector<WideWrite>& character_writes(std::string_view table) const;
  // Applies the rows of list table `table` to its rows (grown to total()), lowest priority first: each mod's clear, then its adds (an
  // entry already in the row is not added twice) and removes, in file order. Returns the number of entries added or removed.
  size_t apply_lists(std::string_view table, std::vector<std::vector<u16>>& rows) const;
  // The same for the trading posts (table trade): an entry is keyed by its goods, "add=<goods> colour=<mask> levels=<sum>" adds it, or
  // sets the words it gives on the entry the post already has for those goods; a new entry needs levels=; a post keeps at most
  // kTradeSlots entries (more are logged and dropped). Returns the number of entries added, changed or removed.
  size_t apply_trade(std::vector<std::vector<TradeEntry>>& posts) const;

  // The id of the record `name` of `table`'s names, if a mod adds it.
  std::optional<size_t> id_of(std::string_view table, std::string_view name) const;
  // The name of record `id` of `table`'s names ("" for the table's own records).
  const std::string& name_of(std::string_view table, size_t id) const;
  // The names mods add to `table`, in id order.
  const std::vector<std::string>& added_names(std::string_view table) const;
  // The record named record `id` of `table` is copied from ("base="; the first mod in the list that gives one), or nothing.
  std::optional<size_t> base_of(std::string_view table, size_t id) const;
  // The record of `table` that `own_id` means in mod `mod`'s scripts and text (its own-id section), or nothing when it lists no such id.
  std::optional<size_t> record_of(std::string_view mod, std::string_view table, int own_id) const;
  // The mod's own id for record `id` of `table`, or nothing.
  std::optional<int> own_record_id(std::string_view mod, std::string_view table, size_t id) const;
  // The record a script or text of mod `mod` names with `own_id`: its own-id section's entry; else a record of the table itself (the
  // game's, or one a whole-file replacement adds) keeps its id; an id in the named records' range the mod does not list names nothing.
  // `mod` = "" for the game's own scripts.
  std::optional<size_t> record_for(std::string_view mod, std::string_view table, int own_id) const;
  // The same for elements (pgdat, [Elements]).
  std::optional<size_t> element_of(std::string_view mod, int own_id) const { return record_of(mod, "pgdat", own_id); }
  std::optional<size_t> element_for(std::string_view mod, int own_id) const { return record_for(mod, "pgdat", own_id); }
  // The same for items (waa, [Items]).
  std::optional<size_t> item_for(std::string_view mod, int own_id) const { return record_for(mod, "waa", own_id); }
  // The same for key items ([KeyItems]): the game's 1..64 keep their number.
  std::optional<size_t> key_item_for(std::string_view mod, int own_id) const { return record_for(mod, "keyitem", own_id); }

  // Log lines of build() and apply() (also written to the log).
  const std::vector<std::string>& log_lines() const { return log_lines_; }

 private:
  struct Parsed {
    size_t mod = 0;
    std::vector<RowBlock> blocks;
  };
  struct Names {
    size_t first = 0;
    std::vector<std::string> names;
    std::map<std::string, size_t, std::less<>> ids;
  };
  void note(bool warning, std::string line) const;
  // A [?<name>] block whose record no enabled mod adds: noted and skipped (true).
  bool skip_optional(std::string_view mod, std::string_view table, const RowBlock& block, const std::optional<size_t>& index) const;

  std::vector<std::string> mod_names_;
  std::map<std::string, std::vector<Parsed>, std::less<>> parsed_;
  std::map<std::string, Names, std::less<>> names_;
  // mod -> table -> own id -> record.
  std::map<std::string, std::map<std::string, std::map<int, size_t>, std::less<>>, std::less<>> own_ids_;
  mutable std::vector<std::string> log_lines_;
  mutable std::map<std::string, std::vector<WideWrite>, std::less<>> wide_;
  mutable std::map<std::string, std::vector<WideWrite>, std::less<>> character_;
};

// The rows the tables of the mods' VFS were built with (GameTables::load through that VFS sets them; a load through another VFS, as the
// background scan's, leaves them).
const TableRows& table_rows();
void set_table_rows(TableRows rows);

}  // namespace oc::data
