#pragma once
// Text importer: lang.dat tables, the PS1 JP glyph set, message-block parsing and one rich-text model for both the
// PS1 byte stream and the RDE markup.
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::import {

// ---------------------------------------------------------------------------------------------- languages
// Language index 0..5.
enum class Lang { En, Fr, It, De, Es, Jp };
// Two-letter code: "en", "fr", ...
const char* lang_code(Lang lang);
bool parse_lang(std::string_view code, Lang& out);

// UTF-8 <-> UTF-32 (invalid input becomes U+FFFD).
std::u32string utf8_to_u32(std::string_view text);
std::string u32_to_utf8(std::u32string_view text);

// ---------------------------------------------------------------------------------------------- rich text
enum class TokKind : u8 {
  Text,        // text = code points to draw
  Newline,
  Page,        // wait for confirm, clear window ({bpage}, PS1 02; a directly following newline is swallowed)
  Wait,        // wait for confirm inside the page ({bwait}, PS1 03)
  Delay,       // arg = frames (30 Hz) ({wait(n)}, PS1 04)
  Speed,       // arg = typewriter speed, 255 = instant ({speed(n)}, PS1 05)
  AutoClose,   // arg = frames until the window closes by itself ({wend(n)}, PS1 06)
  End,         // end of message ({mend}, PS1 00)
  Color,       // arg = colour index (0 white, 1 blue, ... 5 gray) ({color(n)}, PS1 0E)
  Italic,      // arg = 1 on / 0 off
  Name,        // arg = speaker id (SERGE% = 0 ...), arg2 = JP: use only the first n chars (0 = all)
  PartyName,   // arg = party slot ({PARTYn%}, PS1 18)
  PlayWork,    // arg = script variable holding a character ({playwork(n)}, PS1 15)
  ItemWork,    // arg = script variable holding an item ({itemwork(n)}, PS1 19)
  Item,        // arg = item id (PS1 07)
  Num,         // arg = script variable ({num(n)}, PS1 0A/0B)
  Button,      // arg = button glyph id 0..3, 8, 0x10 up / 0x11 down / 0x12 right / 0x13 left ({key(n)}, PS1 1B)
  Case,        // arg = capitalisation mode 0/1/2 ({Cap}=1, {CAP}=2, {CapL}=3, PS1 1C)
  Suffix,      // arg = name suffix ({-y} = 1, {-i} = 2, PS1 1E)
  HideName,    // {HIDE}: suppress the next name
  FWait,       // arg = event ({fwait(n)}, PS1 16)
  FSet,        // arg = flag ({fset(n)}, PS1 17)
  Input,       // arg = digit count n of a number entry; the next n glyphs are its digits ({input(n)}, PS1 10)
  Accent,      // arg = grammar form (PS1 12, 1F)
};

struct Token {
  TokKind kind = TokKind::Text;
  int arg = 0;
  int arg2 = 0;
  std::u32string text;
};

struct Message {
  std::vector<Token> tokens;
  std::string source;  // the markup string (or the PS1 stream decoded to markup) this was parsed from
};

// Markup string as stored in lang.dat after lookup (real LF newlines; a literal backslash+n is also accepted) -> tokens.
// For English the bare name tokens ("SERGE%", "Sir SERGE%", ...) become Name tokens, "◆" is dropped.
Message parse_markup(std::string_view text, Lang lang);

// Names for the placeholder tokens; any unset resolver leaves the placeholder as readable text.
struct TextContext {
  std::function<std::u32string(int speaker)> speaker_name;
  std::function<std::u32string(int slot)> party_name;
  std::function<std::u32string(int item_id)> item_name;
  std::function<std::u32string(int var)> script_name;  // {playwork(n)}
  std::function<std::u32string(int var)> script_item;   // {itemwork(n)}
  std::function<int(int var)> script_number;            // {num(n)}
};

// Expands every placeholder (names, items, numbers, case, suffix) into Text tokens and merges adjacent text.
// Result contains only: Text, Newline, Page, Wait, Delay, Speed, AutoClose, End, Color, Italic, Button, FWait, FSet.
Message flatten(const Message& message, const TextContext& ctx);

// Debug rendering of a token list (one compact line), e.g. `[txt "Hi"][nl][wait 20][page]`.
std::string describe(const Message& message);

// Speaker tag table: id 0..49 -> "SERGE" ... ; nullptr for the unused ids 44..48.
const char* speaker_tag(int id);
// "SERGE" -> 0; -1 if unknown.
int speaker_id_from_tag(std::string_view name);

// ---------------------------------------------------------------------------------------------- JP glyphs
constexpr size_t kJpFontOffset = 0x46F0C;  // in cdrom/prog/slps_023.64
constexpr int kJpGlyphSlots = 2016;
constexpr int kJpGlyphBytes = 22;
constexpr int kJpGlyphSize = 11;  // pixels, square

class JpGlyphs {
 public:
  // The embedded jp_glyphs.tsv table (disc glyph code -> Unicode). Always available.
  JpGlyphs();

  // Reads the 1-bit bitmaps from the PS-X EXE bytes. False if the file is too short.
  bool load_bitmaps(ByteSpan exe);
  bool has_bitmaps() const { return !bitmaps_.empty(); }

  // PS1 renderer index of a glyph code (b1 = second byte for prefixes 0xF8..0xFE, ignored otherwise).
  static int bitmap_index(u8 b0, u8 b1);
  // Unicode code point of a glyph code, or 0 if the code is unresolved.
  char32_t to_unicode(u8 b0, u8 b1) const;
  // Bitmap slot for a code point (-1 if the PS1 font has no such glyph). First code wins for duplicates.
  int index_for(char32_t cp) const;
  // 11 rows of u16, bit 15 = leftmost pixel; nullptr if bitmaps are not loaded or the index is out of range.
  const u16* rows(int index) const;
  // Pixel advance in the PS1 renderer (8 for single-byte codes <= 0x74, else 12).
  static int advance(int index) { return index <= 0x74 - 0x20 ? 8 : 12; }
  size_t known_codes() const { return unicode_.size(); }

 private:
  std::unordered_map<u32, char32_t> unicode_;     // key = b0 or b0<<8|b1
  std::unordered_map<char32_t, int> index_of_;
  std::vector<u16> bitmaps_;                      // kJpGlyphSlots * 11
};

// ---------------------------------------------------------------------------------------------- PS1 executable strings
constexpr size_t kExeStringsOffset = 0x508E0;  // in cdrom/prog/slps_023.64

// The string tables of the PS1 main executable, PS1 text bytes each: table 0 items, 1 character names (0x31 Lynx), 2..22 the
// per-speaker grammar forms of message control 0x12 (indexed by character id), 23 elements and techs, 24 key items, 25 enemies,
// 26 battle strings, 27 buttons. Layout: u32 table count, u32 offset per table (from the start); a table is u16 offsets (from the
// table start, the first one = 2 * entry count) to strings of glyphs and message control codes ending in 0x00.
class ExeStrings {
 public:
  // False if the executable is too short or the table header does not fit.
  bool load(ByteSpan exe);
  bool loaded() const { return !tables_.empty(); }
  // The string's bytes up to (not including) its 0x00; empty for an unknown table or entry.
  ByteSpan get(int table, int index) const;

 private:
  Bytes exe_;
  std::vector<std::vector<std::pair<u32, u32>>> tables_;
};

// ---------------------------------------------------------------------------------------------- PS1 dialogue blocks
// Message block of a room (subfile 8, already LZSS-decoded) -> one byte span per record, trailing sentinel included.
struct DialogueBlock {
  struct Record {
    u16 ptr = 0;
    u16 width_chars = 0, lines = 0, width_px = 0;
    ByteSpan bytes;
  };
  std::vector<Record> records;
};
std::optional<DialogueBlock> parse_dialogue_block(ByteSpan block);

// PS1 byte stream -> tokens. Stops at END (00); a trailing newline after END/AutoClose is dropped.
Message parse_ps1(ByteSpan stream, const JpGlyphs& glyphs);
// Same stream rendered as RDE markup (the jp.csv notation) for display and comparison.
std::string ps1_to_markup(ByteSpan stream, const JpGlyphs& glyphs);

// Loads and decodes the dialogue block of a room: cdrom/map/mapbin/<name>.bin subfile 8. nullopt if absent.
std::optional<Bytes> read_room_dialogue_block(const vfs::Vfs& vfs, std::string_view room_name);
// lang.dat room id -> map file stem ("alu_a01f"); nullptr if unknown. Accepts the stem too via room_id_from_name().
const char* room_name_for_id(int room_id);
// First id using that stem; -1 if unknown.
int room_id_from_name(std::string_view name);

// ---------------------------------------------------------------------------------------------- lang.dat
// A language-column table (sysmes.txt, menutest.txt, ...): header row names the columns, first column is the key.
class TextTable {
 public:
  // `text` is the decoded file (UTF-16LE/UTF-8 BOM already handled). Cells are unquoted; "{66}"/"{99}" become curly
  // quotes and a literal backslash+n becomes LF.
  void parse(std::string_view utf8);
  // Key as written in the file (e.g. "0x000001", "12", or an English string).
  const std::string* get(std::string_view key, Lang lang) const;
  // Numeric key (the file's own number parsed with base auto-detection).
  const std::string* get(u64 key, Lang lang) const;
  size_t rows() const { return rows_.size(); }
  const std::vector<std::string>& header() const { return header_; }

 private:
  int column(Lang lang) const;
  std::vector<std::string> header_;
  std::vector<std::vector<std::string>> rows_;
  std::unordered_map<std::string, u32> by_text_;
  std::unordered_map<u64, u32> by_num_;
};

struct LookupInfo {
  int speaker = 0;
  bool variant = false;  // an Ex_<lang> per-speaker variant was used
};

// Field csv: (room, msg) -> text. `load_rows_rule` implements "a row is loaded only if no earlier row has a larger room".
struct FieldTable {
  std::unordered_map<u64, std::string> rows;
  static u64 key(u32 room, u32 msg) { return (static_cast<u64>(room) << 32) | msg; }
  void parse(std::string_view utf8, bool load_rows_rule);
  const std::string* find(u32 room, u32 msg) const {
    auto it = rows.find(key(room, msg));
    return it == rows.end() ? nullptr : &it->second;
  }
};

class LangTables {
 public:
  // Binds to a mounted install; nothing is read until needed. `vfs` must outlive this object.
  void init(const vfs::Vfs* vfs) { vfs_ = vfs; }

  // Reads the per-language field tables (<lang>.csv, Ex_<lang>.csv, mesgen, mess_wend, btel_<lang>). Returns false if
  // <lang>.csv is not available. Lookups call this lazily.
  bool load_language(Lang lang);

  // Synthetic data for tests (no Vfs): replaces the corresponding table.
  void set_field_csv(Lang lang, std::string_view utf8);
  void set_ex_csv(Lang lang, std::string_view utf8);
  void set_mesgen(Lang lang, std::string_view utf8);
  void set_wend_csv(std::string_view utf8);
  void set_battle_csv(Lang lang, std::string_view utf8);
  void set_table(std::string_view name, std::string_view utf8);

  // Field dialogue (RDE 0x4517B0): row (room, msg) of <lang>.csv with the mess_wend, Ex_<lang> and mesgen steps. The
  // result uses LF newlines. `speaker` is the script's speaker (0 = unknown) when the text carries no name tag.
  std::optional<std::string> lookup_field(int room_id, int msg_index, Lang lang, int speaker = 0,
                                          LookupInfo* info = nullptr);
  // Raw `<lang>.csv` row without any of the steps (nullptr if absent / not loadable).
  const std::string* raw_field(int room_id, int msg_index, Lang lang);
  // Parsed message; a missing row yields "MESSAGE NOT FOUND".
  Message field_message(int room_id, int msg_index, Lang lang, int speaker = 0);

  // Battle event messages: btel_<lang>.csv key (battle id, msg).
  const std::string* battle_event(u32 battle_id, u32 msg, Lang lang);

  // Language-column tables by member name without ".txt": "sysmes", "menutest", "shopmenu", "btmes", "cmnmes", ...
  const TextTable* table(std::string_view name);
  // sysmes key (category << 16) | index, e.g. 0x10000 + id = default character name.
  const std::string* sysmes(u32 key, Lang lang);
  // menutest.txt entry.
  const std::string* menu_text(u32 guest_addr, Lang lang);
  // btmes.txt entry.
  const std::string* battle_message(u32 id, Lang lang);
  // sysmes 0x10000 + id, up to 15 UTF-16 units.
  std::u32string default_name(int speaker, Lang lang);
  // sysmes 0x000000 + id.
  std::u32string item_name(int item_id, Lang lang);

  // A TextContext that resolves names/items from sysmes for `lang` (party slots map to Serge, Kid, Guile).
  TextContext make_context(Lang lang);

  // Number of rows in <lang>.csv (after the loading rule), for coverage reports.
  size_t field_row_count(Lang lang);

 private:
  struct PerLang {
    bool loaded = false;
    FieldTable field;
    std::unordered_map<u64, std::vector<std::string>> ex;  // 50 slots, empty = none
    std::vector<std::pair<std::string, std::vector<std::string>>> mesgen;
    FieldTable battle;
    bool has_ex = false;
  };
  PerLang& per(Lang lang) { return langs_[static_cast<int>(lang)]; }
  bool ensure(Lang lang);
  void ensure_wend();
  bool read_member(std::string_view name, std::string& out_utf8) const;

  const vfs::Vfs* vfs_ = nullptr;
  std::array<PerLang, 6> langs_;
  FieldTable wend_;
  bool wend_loaded_ = false;
  std::unordered_map<std::string, TextTable> tables_;
  std::unordered_map<std::string, bool> tables_tried_;
};

// Decodes a lang.dat text member: UTF-16LE BOM, UTF-8 BOM or plain UTF-8 -> UTF-8 without BOM.
std::string decode_text_member(ByteSpan raw);

}  // namespace oc::import
