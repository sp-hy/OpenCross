#pragma once
// The game's text read from the PS1 files themselves, for a session with no lang.dat (the PS1 discs). Every LangTables lookup has a
// source here, in the same form lang.dat gives it (markup, rows keyed as lang.dat keys them):
//   field dialogue    the room's message block (subfile 8 of map/mapbin/<room>.bin), decoded to markup, grammar forms resolved
//   battle events     part 1 of the battle scene's script (battle/event/evt*.pac)
//   sysmes            the executable's string tables (items, names, techs, key items, enemies, battle strings, buttons)
//   btmes             battle/data/btmes.dat; keyitem / waa_spec: menu/keyitem.dat, menu/waa_spec.dat
//   menutest, shopmenu  JP: the menu overlays' Shift-JIS strings at the key address, or the parts a row is put together from;
//                     USA: a line of the "mess" messages of menu/menutest.prd
//   savetbl           the "stbl" entry of menu/menutest.prd; ome (JP): the character profiles menu/player/<name>.ome
// The tables RDE added for its own screens (cmnmes, ui_*, memmenu, btlexit, error) have no PS1 source: their users keep their own
// built-in text. A build is recognised by its executable's string-table header (ps1_text_builds()); the text is in the build's own
// language whatever language is asked for.
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/types.h"
#include "import/text.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::import {

// JP: a menu row that is not the one overlay string at its key: `count` overlay strings (guest addresses) in a row, bit i of
// `lines` set when part i starts a new line.
struct Ps1MenuPart {
  u8 table;
  u32 key;
  u8 count;
  u32 parts[4];
  u8 lines;
};

// USA: the menu message holding a menutest (table 0) / shopmenu (1) key's text and its line / tab field (255 = the whole message;
// a run of line breaks and tabs separates two fields). A key whose text sits inside a longer field takes `length` characters of it
// from `start` (length 0 = the whole field).
struct Ps1MenuString {
  u8 table;
  u32 key;
  u16 message;
  u8 segment;
  u8 start;
  u8 length;
};

enum class Ps1Charset {
  // Single-byte codes up to 0xF7 and page-prefixed pairs (0xF8..0xFE).
  JpGlyphs,
  // Single-byte codes only.
  UsaGlyphs,
};

// Where one build keeps its text.
struct Ps1TextBuild {
  const char* name;
  // The language of its text.
  Lang lang;
  Ps1Charset charset;
  // File offset of the executable's string tables and their count (the header's first word; it tells the builds apart).
  u32 strings_offset;
  u32 table_count;
  // sysmes category -> executable table.
  struct Category {
    u32 category;
    int table;
  };
  std::array<Category, 7> categories;
  // Message control 0x12 with argument n (1..grammar_forms) reads table grammar_table + n - 1. Its row is the speaker's character id
  // when `grammar_columns` is empty, else 1 + the speaker's column in the byte table at `grammar_columns_offset` of that file;
  // form `grammar_by_id` (0 = none) is read by character id + 1 even then.
  int grammar_table;
  int grammar_forms;
  const char* grammar_columns;
  u32 grammar_columns_offset;
  int grammar_by_id;
  // Glyphs of character 13's name that name code 0x83 shows.
  int short_name_glyphs;
  // Load address of the menu overlays (prog/menutest.prg, prog/shopmenu.prg; the same in both builds); JP menus: their composed rows.
  u32 menu_base;
  std::span<const Ps1MenuPart> menu_parts;
  // USA menus: the key -> message lines.
  std::span<const Ps1MenuString> menu_strings;
  // Profile file stem per ome row (menu/player/<stem>.ome); empty = no profile table.
  std::span<const char* const> ome_files;
};

// The known builds.
std::span<const Ps1TextBuild> ps1_text_builds();
// Loads the string tables of the build the executable belongs to into `out`; that build, or null when none is recognised.
const Ps1TextBuild* load_exe_strings(ByteSpan exe, ExeStrings& out);
// The table of `build` that holds sysmes category `category` (0x00 items, 0x01 names, 0x17 techs, ...); -1 when none does.
int ps1_category_table(const Ps1TextBuild& build, u32 category);

// Two-byte Shift-JIS code -> Unicode for the characters the JP menu files use; 0 if not in the table.
char32_t sjis_to_unicode(u16 code);
// A Shift-JIS string (CR LF = newline) -> UTF-8; nullopt if a byte sequence is not a character the table knows or is a control code.
std::optional<std::string> sjis_to_utf8(ByteSpan bytes);
// Every NUL-terminated Shift-JIS string of an overlay image under its guest address (`base` + offset), strings holding a line break
// left out. A run that does not decode (a string right behind a pointer table, with no NUL between them) is tried again from its next
// byte, so the string after the table keeps its own address.
std::unordered_map<u32, std::string> overlay_sjis_strings(ByteSpan image, u32 base);

class Ps1Text {
 public:
  // Reads the executable and recognises the build. False if it is missing or no known build.
  bool open(const vfs::Vfs& vfs);
  const Ps1TextBuild& build() const { return *build_; }

  // A room message (lang.dat room id, message index) as markup in the jp.csv notation (a newline is backslash + n); nullptr if the
  // room or the message is absent. Rooms are read on first use and kept. The index is the build's own (the USA blocks are numbered
  // differently from RDE's rows in some rooms; the build's scripts use its own numbers).
  const std::string* field(int room_id, int msg);
  // `markup` with the grammar forms (message control 0x12) of `speaker` filled in and the empty placeholder (0x1F) dropped.
  std::string resolve_grammar(std::string_view markup, int speaker) const;
  // Record `msg` of the dialogue part of battle `battle_id`'s script as markup (no newline after the last control); nullptr if absent.
  const std::string* battle_event(u32 battle_id, u32 msg);
  // A table in lang.dat's shape (header "#" + the six language codes, every column the same text); nullopt for a table with no PS1
  // source.
  std::optional<TextTable> table(std::string_view name) const;

  // A PS1 text byte stream of this build -> markup in the jp.csv notation.
  std::string markup(ByteSpan stream) const;

 private:
  void sysmes(TextTable& out) const;
  void dialogue_records(TextTable& out, std::string_view path) const;
  void line_triples(TextTable& out, std::string_view path) const;
  void overlay_menu(TextTable& out, int table, std::string_view path) const;
  void message_menu(TextTable& out, int table) const;
  void save_titles(TextTable& out) const;
  void profiles(TextTable& out) const;

  const vfs::Vfs* vfs_ = nullptr;
  const Ps1TextBuild* build_ = nullptr;
  JpGlyphs glyphs_;
  ExeStrings strings_;
  // Speaker character id -> grammar column (empty: read by id).
  Bytes grammar_columns_;
  std::unordered_map<int, std::vector<std::string>> rooms_;
  std::unordered_map<u32, std::vector<std::string>> battles_;
};

}  // namespace oc::import
