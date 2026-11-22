#pragma once
// Table rows: a mod's tables\<table>.txt changes single fields of the records of one battle / menu table instead of replacing the
// whole file, so several mods that each change a few records stack. The table is named after the file it changes (monster,
// pgdat, char, char20, waa, menu_waa, scenedat, ditem). Rows apply over the table as the game files (or a mod's whole-file
// replacement) give it, lowest priority first, so the first mod in the list wins a field; a field two mods set differently is
// logged.
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::data {

// One named field of a record: byte offset and width (1, 2 or 4 bytes, little-endian).
struct TableField {
  const char* name;
  u16 offset;
  u8 width;
};

// The fields a row may name for `table` (empty for an unknown table).
std::span<const TableField> table_fields(std::string_view table);

// One mod's row files: table name (lower case) -> the file's bytes.
struct TableMod {
  std::string name;
  std::map<std::string, std::function<std::optional<Bytes>()>> files;
};
// Set once at launch (priority order, first wins), before the tables are loaded. The rows belong to the tables read through `files`
// only: tables loaded through another VFS (the background scan's unmodified game) keep the files as they are.
void set_table_mods(const vfs::Vfs* files, std::vector<TableMod> mods);

// One field write parsed from a row file.
struct RowWrite {
  size_t record = 0;
  u16 offset = 0;
  u8 width = 0;
  u32 value = 0;
  int line = 0;
};
// Parses the rows of one file. A record header is "[<index>]"; a field line is "<name>=<value>" with a name from `fields`, or
// "0x<offset>:u8|u16|u32=<value>" for a byte the names do not cover. Values are decimal, 0x hex, or negative decimal (two's complement
// in the field's width). Blank lines and lines starting with ';' or '#' are skipped. Lines that cannot be read go to `problems`.
std::vector<RowWrite> parse_rows(std::string_view text, std::span<const TableField> fields, std::vector<std::string>* problems);

// Applies the rows of every table mod for `table` to records 0..count-1, lowest priority first; `record(i)` gives record i's bytes.
// A row for a record past `count` or a field past the record's end is skipped and logged. Returns the number of fields written; none
// when `files` is not the VFS the mods were set for.
size_t apply_table_rows(const vfs::Vfs& files, std::string_view table, size_t count, const std::function<std::span<u8>(size_t)>& record);
// The same for an explicit mod list (tests).
size_t apply_table_rows(const std::vector<TableMod>& mods, std::string_view table, size_t count,
                        const std::function<std::span<u8>(size_t)>& record, std::vector<std::string>* log_lines = nullptr);

}  // namespace oc::data
