#include "import/ps1_text.h"

#include <algorithm>
#include <cstdio>

#include "import/container.h"
#include "vfs/disc_names.h"
#include "vfs/vfs.h"

namespace oc::import {

#include "import/text_tables.inc"

namespace {

constexpr std::string_view kNewline = "\\n";
constexpr std::string_view kExecutable = "cdrom/prog/slps_023.64";
// Record of battle scene 0's script in the disc file numbering; scene n is the record after it by n.
constexpr int kBattleScriptRecord = 2679;
// A JP save-title record of menutest.prd's "stbl" entry: the title (Shift-JIS, NUL padded) and its story-progress value.
constexpr size_t kSaveTitleRecord = 36;
constexpr size_t kSaveTitleText = 32;
// Characters the grammar column table covers.
constexpr size_t kGrammarSpeakers = 50;

constexpr Ps1TextBuild kBuilds[] = {
    {"jp", Lang::Jp, Ps1Charset::JpGlyphs, 0x508E0, 28,
     {{{0x00, 0}, {0x01, 1}, {0x17, 23}, {0x18, 24}, {0x19, 25}, {0x1A, 26}, {0x1B, 27}}},
     2, 21, nullptr, 0, 0, 2, 0x801D2800, kMenuPartsJp, {}, kOmeFilesJp},
    {"usa", Lang::En, Ps1Charset::UsaGlyphs, 0x4803C, 55,
     {{{0x00, 0}, {0x01, 1}, {0x17, 4}, {0x18, 5}, {0x19, 6}, {0x1A, 2}, {0x1B, 3}}},
     7, 0x30, "cdrom/prog/field.prg", 0x3EA84, 0x20, 3, 0, {}, kMenuStringsUsa, {}},
};

u32 le16(ByteSpan bytes, size_t at) { return at + 2 <= bytes.size() ? static_cast<u32>(bytes[at] | (bytes[at + 1] << 8)) : 0; }
u32 le32(ByteSpan bytes, size_t at) { return at + 4 <= bytes.size() ? le16(bytes, at) | (le16(bytes, at + 2) << 16) : 0; }

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
}

// Only blanks (U+0020, U+3000) or nothing: RDE's tables leave these rows empty.
bool blank(std::string_view text) {
  while (!text.empty()) {
    if (text.front() == ' ') text.remove_prefix(1);
    else if (text.starts_with("\xE3\x80\x80")) text.remove_prefix(3);
    else return false;
  }
  return true;
}

std::string trim_blanks(std::string text) {
  while (!text.empty() && text.back() == ' ') text.pop_back();
  while (text.ends_with("\xE3\x80\x80")) text.resize(text.size() - 3);
  size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text.compare(begin, 3, "\xE3\x80\x80") == 0)) begin += text[begin] == ' ' ? 1 : 3;
  return text.substr(begin);
}

std::vector<std::string> table_header() {
  std::vector<std::string> header{"#"};
  for (int i = 0; i < 6; ++i) {
    std::string code = lang_code(static_cast<Lang>(i));
    for (char& ch : code) ch = static_cast<char>(ch - 'a' + 'A');
    header.push_back(code);
  }
  return header;
}

void add(TextTable& out, std::string key, const std::string& text) {
  std::vector<std::string> row{std::move(key)};
  row.resize(7, text);
  out.add_row(std::move(row));
}

std::string hex_key(u32 key, int digits) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%0*X", digits, key);
  return buf;
}

ByteSpan up_to_nul(ByteSpan bytes) {
  return bytes.first(static_cast<size_t>(std::find(bytes.begin(), bytes.end(), u8{0}) - bytes.begin()));
}

// Strings of a u16 offset table (the first offset is twice the entry count), each up to its 0x00. The last offset of the executable
// and description tables only marks the end (`with_last` false); the menu messages' last offset is a message.
std::vector<ByteSpan> offset_strings(ByteSpan file, bool with_last = false) {
  std::vector<ByteSpan> out;
  const u32 count = le16(file, 0) / 2;
  for (u32 i = 0; i + (with_last ? 0 : 1) < count; ++i) {
    const size_t at = le16(file, 2 * i);
    if (at >= file.size()) break;
    out.push_back(up_to_nul(file.subspan(at)));
  }
  return out;
}

char32_t usa_glyph(ByteSpan bytes, size_t& length) {
  length = 1;
  for (const auto& glyph : kUsaGlyphChars)
    if (glyph.code == bytes[0]) return glyph.cp;
  return 0;
}

// The USA menu messages: ASCII with LF / CR line breaks and TAB fields, the half-width full stop, and a few Shift-JIS symbols.
std::string usa_menu_text(ByteSpan bytes) {
  std::u32string text;
  for (size_t i = 0; i < bytes.size(); ++i) {
    const u8 code = bytes[i];
    const u16 pair = i + 1 < bytes.size() ? static_cast<u16>(code << 8 | bytes[i + 1]) : 0;
    if (code == 0x0A || code == 0x0D) {
      text.push_back(U'\n');
    } else if (code == 0x09 || (code >= 0x20 && code < 0x7F)) {
      text.push_back(code);
    } else if (code == 0xA1) {
      text.push_back(0xFF61);
    } else if (pair == 0x8167 || pair == 0x8168 || pair == 0x819B || pair == 0x817E || pair == 0x8277) {
      text.push_back(pair == 0x8167 ? 0x201C : pair == 0x8168 ? 0x201D : pair == 0x819B ? 0x25CB : 0xD7);
      ++i;
    }
  }
  return u32_to_utf8(text);
}

}  // namespace

std::span<const Ps1TextBuild> ps1_text_builds() { return kBuilds; }

char32_t sjis_to_unicode(u16 code) {
  const auto* end = std::end(kSjisChars);
  const auto* it = std::lower_bound(std::begin(kSjisChars), end, code, [](const auto& entry, u16 value) { return entry.code < value; });
  return it != end && it->code == code ? it->cp : 0;
}

std::optional<std::string> sjis_to_utf8(ByteSpan bytes) {
  std::u32string text;
  for (size_t i = 0; i < bytes.size(); ++i) {
    const u8 lead = bytes[i];
    if (lead >= 0x20 && lead < 0x7F) {
      text.push_back(lead);
    } else if (lead >= 0xA1 && lead <= 0xDF) {
      text.push_back(0xFF61 + (lead - 0xA1));
    } else if (lead == 0x0D && i + 1 < bytes.size() && bytes[i + 1] == 0x0A) {
      text.push_back(U'\n');
      ++i;
    } else {
      const char32_t cp = i + 1 < bytes.size() ? sjis_to_unicode(static_cast<u16>(lead << 8 | bytes[i + 1])) : 0;
      if (!cp) return std::nullopt;
      text.push_back(cp);
      ++i;
    }
  }
  return u32_to_utf8(text);
}

bool Ps1Text::open(const vfs::Vfs& vfs) {
  vfs_ = &vfs;
  build_ = nullptr;
  const auto exe = vfs.read(kExecutable);
  if (!exe) return false;
  for (const Ps1TextBuild& build : kBuilds) {
    if (le32(*exe, build.strings_offset) != build.table_count || !strings_.load(*exe, build.strings_offset)) continue;
    build_ = &build;
    grammar_columns_.clear();
    if (build.grammar_columns) {
      const auto file = vfs.read(build.grammar_columns);
      const size_t at = build.grammar_columns_offset;
      if (file && file->size() >= at + kGrammarSpeakers) grammar_columns_.assign(file->begin() + at, file->begin() + at + kGrammarSpeakers);
    }
    return true;
  }
  return false;
}

std::string Ps1Text::markup(ByteSpan stream) const {
  const bool usa = build_->charset == Ps1Charset::UsaGlyphs;
  std::string text = usa ? ps1_to_markup(stream, GlyphDecoder(usa_glyph)) : ps1_to_markup(stream, glyphs_);
  // The name code's first-glyphs forms: 0x80 + n is the first n glyphs of Serge's name, 0x83 the first few of Razzly's.
  replace_all(text, "{name(129)}", "{SERGE%1}");
  replace_all(text, "{name(130)}", "{SERGE%2}");
  replace_all(text, "{name(131)}", "{RAZZLY%" + std::to_string(build_->short_name_glyphs) + "}");
  if (usa) {
    // Capitals (the next glyph, the rest, the next glyph for one speaker), the name suffixes, colour 8 = grey.
    replace_all(text, "{case(0)}", "{Cap}");
    replace_all(text, "{case(1)}", "{CAP}");
    replace_all(text, "{case(2)}", "{CapL}");
    replace_all(text, "{suffix(0)}", "{-y}");
    replace_all(text, "{suffix(1)}", "{-i}");
    replace_all(text, "{color(8)}", "{color(GRAY)}");
  }
  return text;
}

const std::string* Ps1Text::field(int room_id, int msg) {
  auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    std::vector<std::string> messages;
    if (const char* stem = room_name_for_id(room_id); stem && vfs_) {
      if (const auto block = read_room_dialogue_block(*vfs_, stem))
        if (const auto dialogue = parse_dialogue_block(*block))
          for (const auto& record : dialogue->records) messages.push_back(markup(record.bytes));
    }
    it = rooms_.emplace(room_id, std::move(messages)).first;
  }
  return msg >= 0 && static_cast<size_t>(msg) < it->second.size() ? &it->second[static_cast<size_t>(msg)] : nullptr;
}

std::string Ps1Text::resolve_grammar(std::string_view markup_text, int speaker) const {
  std::string out;
  for (size_t pos = 0; pos < markup_text.size();) {
    const size_t open = markup_text.find('{', pos);
    if (open == std::string_view::npos) {
      out += markup_text.substr(pos);
      break;
    }
    out += markup_text.substr(pos, open - pos);
    const size_t close = markup_text.find('}', open);
    if (close == std::string_view::npos) {
      out += markup_text.substr(open);
      break;
    }
    const std::string_view tag = markup_text.substr(open, close + 1 - open);
    pos = close + 1;
    if (tag == "{accent}") continue;
    int form = 0;
    if (std::sscanf(std::string(tag).c_str(), "{gram(%d)}", &form) == 1 && form >= 1 && form <= build_->grammar_forms) {
      int row = speaker;
      if (!grammar_columns_.empty()) {
        const bool known = speaker >= 0 && static_cast<size_t>(speaker) < grammar_columns_.size();
        row = form == build_->grammar_by_id ? speaker + 1 : known ? grammar_columns_[static_cast<size_t>(speaker)] + 1 : 1;
      }
      std::string value = markup(strings_.get(build_->grammar_table + form - 1, row));
      replace_all(value, "{accent}", "");
      out += value;
      continue;
    }
    out += tag;
  }
  return out;
}

const std::string* Ps1Text::battle_event(u32 battle_id, u32 msg) {
  auto it = battles_.find(battle_id);
  if (it == battles_.end()) {
    std::vector<std::string> messages;
    const auto& records = vfs::builtin_disc_records(vfs_ ? vfs_->disc() : 1);
    const size_t record = kBattleScriptRecord + static_cast<size_t>(battle_id);
    const std::string member = record < records.size() ? vfs::disc_record_member(records[record]) : std::string();
    const auto pac = member.starts_with("battle/event/") && vfs_ ? vfs_->read("cdrom/" + member) : std::nullopt;
    // Part 1 of the two-part pack: [end of part 0, end of file).
    const u32 begin = pac ? le32(*pac, 8) : 0, end = pac ? le32(*pac, 12) : 0;
    if (pac && begin < end && end <= pac->size())
      if (const auto dialogue = parse_dialogue_block(ByteSpan(*pac).subspan(begin, end - begin)))
        for (const auto& entry : dialogue->records) {
          std::string text = markup(entry.bytes);
          // The newline after the closing control belongs to the record, not to the text.
          if (text.ends_with(kNewline)) text.resize(text.size() - kNewline.size());
          messages.push_back(std::move(text));
        }
    it = battles_.emplace(battle_id, std::move(messages)).first;
  }
  return msg < it->second.size() ? &it->second[msg] : nullptr;
}

std::optional<TextTable> Ps1Text::table(std::string_view name) const {
  if (!build_ || !vfs_) return std::nullopt;
  TextTable out;
  out.set_header(table_header());
  const bool menu_messages = !build_->menu_strings.empty();
  if (name == "sysmes") sysmes(out);
  else if (name == "btmes") dialogue_records(out, "cdrom/battle/data/btmes.dat");
  else if (name == "keyitem") line_triples(out, "cdrom/menu/keyitem.dat");
  else if (name == "waa_spec") line_triples(out, "cdrom/menu/waa_spec.dat");
  else if (name == "menutest" && menu_messages) message_menu(out, 0);
  else if (name == "shopmenu" && menu_messages) message_menu(out, 1);
  else if (name == "menutest") overlay_menu(out, 0, "cdrom/prog/menutest.prg");
  else if (name == "shopmenu") overlay_menu(out, 1, "cdrom/prog/shopmenu.prg");
  else if (name == "savetbl") save_titles(out);
  else if (name == "ome" && !build_->ome_files.empty()) profiles(out);
  else return std::nullopt;
  return out;
}

// The battle strings hold (target, line 1, line 2) triples; RDE's rows keep both lines in the middle one.
void Ps1Text::sysmes(TextTable& out) const {
  for (const auto& [category, table] : build_->categories) {
    std::vector<std::string> rows;
    for (int i = 0;; ++i) {
      const ByteSpan entry = strings_.get(table, i);
      if (entry.data() == nullptr) break;
      std::string text = resolve_grammar(markup(entry), 0);
      replace_all(text, kNewline, "\n");
      rows.push_back(blank(text) ? std::string() : std::move(text));
    }
    // The table's last offset marks its end.
    if (!rows.empty()) rows.pop_back();
    if (category == 0x1A)
      for (size_t i = 0; i + 2 < rows.size(); i += 3) {
        if (!rows[i + 2].empty()) rows[i + 1] += "\n" + rows[i + 2];
        rows[i + 2].clear();
      }
    for (size_t i = 0; i < rows.size(); ++i) add(out, hex_key(category << 16 | static_cast<u32>(i), 6), rows[i]);
  }
}

// Message-block records (btmes.dat): the PS1 centres a line with leading blanks and ends it with a newline, RDE's rows have
// neither; the stolen item is code 07 with id 0 on the PS1, {itemwork(0)} in RDE's rows. The block is the whole file, so its last
// record runs to the end of the file and takes in the zeros a file read from the disc ends with: message-end codes, dropped too.
void Ps1Text::dialogue_records(TextTable& out, std::string_view path) const {
  constexpr std::string_view kMessageEnd = "{mend}";
  const auto file = vfs_->read(path);
  const auto dialogue = file ? parse_dialogue_block(*file) : std::nullopt;
  if (!dialogue) return;
  for (size_t i = 0; i < dialogue->records.size(); ++i) {
    std::string text = markup(dialogue->records[i].bytes);
    while (text.ends_with(kMessageEnd)) text.resize(text.size() - kMessageEnd.size());
    while (text.ends_with(kNewline)) text.resize(text.size() - kNewline.size());
    replace_all(text, "{item(0)}", "{itemwork(0)}");
    replace_all(text, kNewline, "\n");
    add(out, std::to_string(i), trim_blanks(std::move(text)));
  }
}

// Description files of three lines per entry (keyitem.dat, waa_spec.dat): the row of the first line holds the lines joined with
// newlines, trailing blank lines dropped; the other two rows are empty.
void Ps1Text::line_triples(TextTable& out, std::string_view path) const {
  const auto file = vfs_->read(path);
  if (!file) return;
  std::vector<std::string> lines;
  for (const ByteSpan entry : offset_strings(*file)) {
    std::string text = markup(entry);
    lines.push_back(blank(text) ? std::string() : std::move(text));
  }
  for (size_t i = 0; i < lines.size(); i += 3) {
    std::string joined;
    size_t used = std::min(lines.size(), i + 3);
    while (used > i && lines[used - 1].empty()) --used;
    for (size_t k = i; k < used; ++k) joined += (k > i ? "\n" : "") + lines[k];
    add(out, std::to_string(i), joined);
    for (size_t k = i + 1; k < std::min(lines.size(), i + 3); ++k) add(out, std::to_string(k), std::string());
  }
}

// JP: every Shift-JIS string of the overlay under its guest address, then the composed rows over them.
void Ps1Text::overlay_menu(TextTable& out, int table, std::string_view path) const {
  const auto file = vfs_->read(path);
  if (!file) return;
  const ByteSpan bytes(*file);
  std::unordered_map<u32, std::string> strings;
  for (size_t i = 0; i < bytes.size();) {
    if (bytes[i] == 0) {
      ++i;
      continue;
    }
    const ByteSpan run = up_to_nul(bytes.subspan(i));
    if (auto text = sjis_to_utf8(run); text && text->find('\n') == std::string::npos)
      strings.emplace(build_->menu_base + static_cast<u32>(i), std::move(*text));
    i += run.size() + 1;
  }
  std::vector<std::pair<u32, std::string>> rows(strings.begin(), strings.end());
  for (const Ps1MenuPart& part : build_->menu_parts) {
    if (part.table != table) continue;
    std::string text;
    for (int i = 0; i < part.count; ++i) {
      const auto found = strings.find(part.parts[i]);
      if (found == strings.end()) continue;
      if (part.lines >> i & 1) text += '\n';
      text += found->second;
    }
    rows.emplace_back(part.key, std::move(text));
  }
  for (const auto& [key, text] : rows) add(out, hex_key(key, 8), text);
}

// USA: a line or TAB field of a "mess" message of menutest.prd (u16 offset table of NUL-terminated messages).
void Ps1Text::message_menu(TextTable& out, int table) const {
  const auto file = vfs_->read("cdrom/menu/menutest.prd");
  const auto drp = file ? parse_drp(*file) : std::nullopt;
  if (!drp) return;
  const auto entry = std::find_if(drp->entries.begin(), drp->entries.end(), [](const DrpEntry& item) { return item.name == "mess"; });
  if (entry == drp->entries.end()) return;
  std::vector<std::string> messages;
  for (const ByteSpan message : offset_strings(entry->data, true)) messages.push_back(usa_menu_text(message));
  for (const Ps1MenuString& row : build_->menu_strings) {
    if (row.table != table || row.message >= messages.size()) continue;
    std::string text = messages[row.message];
    if (row.segment != 0xFF) {
      std::vector<std::string> fields(1);
      for (char ch : text) {
        if (ch == '\n' || ch == '\t') fields.emplace_back();
        else fields.back().push_back(ch);
      }
      text = row.segment < fields.size() ? fields[row.segment] : std::string();
    }
    while (text.ends_with('\n')) text.pop_back();
    add(out, hex_key(row.key, 8), trim_blanks(std::move(text)));
  }
}

// JP: 36-byte records of a NUL-padded Shift-JIS title and its story-progress value, row n = record n - 1. USA: u32 offsets to titles
// that start with their progress value and have a CR between their two lines; a title takes the row of the JP title with the same
// progress value.
void Ps1Text::save_titles(TextTable& out) const {
  const auto file = vfs_->read("cdrom/menu/menutest.prd");
  const auto drp = file ? parse_drp(*file) : std::nullopt;
  if (!drp) return;
  for (const DrpEntry& entry : drp->entries) {
    if (entry.name != "stbl") continue;
    if (build_->charset == Ps1Charset::UsaGlyphs) {
      const u32 count = le32(entry.data, 0) / 4;
      for (u32 i = 0; i < count; ++i) {
        const size_t at = le32(entry.data, 4 * i);
        if (at >= entry.data.size()) break;
        const ByteSpan title = up_to_nul(entry.data.subspan(at));
        if (title.size() < 2) continue;
        const auto* row = std::find(std::begin(kSaveTitleProgressJp), std::end(kSaveTitleProgressJp), title[0]);
        if (row != std::end(kSaveTitleProgressJp))
          add(out, std::to_string(row - std::begin(kSaveTitleProgressJp) + 1), usa_menu_text(title.subspan(1)));
      }
      continue;
    }
    for (size_t at = 0; at + kSaveTitleRecord <= entry.data.size(); at += kSaveTitleRecord) {
      const auto text = sjis_to_utf8(up_to_nul(entry.data.subspan(at, kSaveTitleText)));
      add(out, std::to_string(at / kSaveTitleRecord + 1), text ? trim_blanks(*text) : std::string());
    }
  }
}

// A profile file starts with a part table (u32 count, u32 offset per part); part 0 is the text, lines ending in CR LF.
void Ps1Text::profiles(TextTable& out) const {
  for (size_t row = 0; row < build_->ome_files.size(); ++row) {
    const std::string stem = build_->ome_files[row];
    const auto file = stem.empty() ? std::nullopt : vfs_->read("cdrom/menu/player/" + stem + ".ome");
    if (!file || le32(*file, 0) < 2) continue;
    const u32 begin = le32(*file, 4), end = le32(*file, 8);
    if (begin >= end || end > file->size()) continue;
    const ByteSpan text = ByteSpan(*file).subspan(begin, end - begin);
    size_t last = 0;
    for (size_t i = 0; i + 1 < text.size(); ++i)
      if (text[i] == 0x0D && text[i + 1] == 0x0A) last = i;
    if (auto decoded = sjis_to_utf8(text.first(last))) add(out, std::to_string(row), *decoded);
  }
}

}  // namespace oc::import
