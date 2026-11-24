#include "data/table_rows.h"

#include <algorithm>
#include <charconv>
#include <deque>
#include <format>
#include <set>

#include "core/features.h"
#include "core/log.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

namespace oc::data {

namespace {

struct TableModStore {
  const vfs::Vfs* files = nullptr;
  std::vector<TableMod> mods;
};
TableModStore& mods_storage() {
  static TableModStore store;
  return store;
}

TableRows& rows_storage() {
  static TableRows rows;
  return rows;
}

const std::string kNoName;

// Field names built from a prefix and an index (cell_0 .. cell_47) need storage that outlives the tables.
const char* own(std::string name) {
  static std::deque<std::string> names;
  return names.emplace_back(std::move(name)).c_str();
}

void add_series(std::vector<TableField>& out, const char* prefix, u16 first, int count, u8 width) {
  for (int i = 0; i < count; ++i)
    out.push_back({own(std::format("{}_{}", prefix, i)), static_cast<u16>(first + i * width), width});
}

// monster.bin and char*.bin records (data::StatRec).
std::vector<TableField> stat_fields() {
  std::vector<TableField> out = {
      {"innate", 0x01, 1},       {"growth_col", 0x03, 1}, {"gender", 0x15, 1},      {"max_hp", 0x16, 2},
      {"atk", 0x18, 1},          {"def", 0x19, 1},        {"hit", 0x1A, 1},         {"evd", 0x1B, 1},
      {"mag", 0x1C, 1},          {"mdef", 0x1D, 1},       {"status_dur", 0x1E, 1},  {"stamina_rate", 0x1F, 1},
      {"weapon", 0x20, 2},       {"armour", 0x22, 2},     {"drop_base", 0x2C, 1},   {"model", 0x2D, 1},
      {"ap", 0x2E, 1},           {"weapon_model", 0x2F, 1}, {"drop_row", 0xA0, 2},  {"gold", 0xA2, 2},
      {"hit_med", 0xA4, 1},      {"hit_heavy", 0xA5, 1},  {"star_growth", 0xA7, 1},
  };
  add_series(out, "accessory", 0x24, 4, 2);
  add_series(out, "level_mask", 0x30, 8, 2);
  add_series(out, "cell", 0x40, 48, 2);
  return out;
}

// pgdat.bin records (data::ElemRec).
std::vector<TableField> elem_fields() {
  return {{"flags0", 0x00, 1}, {"colour_bit", 0x05, 1}, {"level", 0x06, 1},    {"mode", 0x07, 1},
          {"power", 0x08, 1},  {"status_pct", 0x0A, 1}, {"variance", 0x0C, 1}, {"hits", 0x0E, 1},
          {"status_lo", 0x18, 4}, {"status_hi", 0x1C, 4}};
}

// menu_pg.bin rows (who may place the element, price, menu use, auto-fill scores).
std::vector<TableField> menu_elem_fields() {
  return {{"eligible_lo", 0x00, 4}, {"eligible_hi", 0x04, 2}, {"price", 0x08, 2},       {"menu_flags", 0x0A, 1},
          {"use_flags", 0x0B, 1},   {"fill_score_0", 0x0C, 1}, {"fill_score_1", 0x0D, 1}};
}

// player.dat part 4 "waa.bin" records (data::GearRec).
std::vector<TableField> gear_fields() {
  return {{"tier", 0x00, 1},      {"class", 0x01, 1},      {"mode", 0x02, 1},       {"colour", 0x03, 1},
          {"power", 0x04, 1},     {"mag", 0x05, 1},        {"acc", 0x06, 1},        {"var", 0x07, 1},
          {"flags", 0x08, 2},     {"slot_mask", 0x0A, 2},  {"mask_lo", 0x0C, 4},    {"mask_hi", 0x10, 4},
          {"status_lo", 0x14, 4}, {"status_hi", 0x18, 4}};
}

// exdat part 5 "menu_waa.bin" records (data::ItemInfo).
std::vector<TableField> item_fields() {
  return {{"equip_lo", 0, 4}, {"equip_hi", 4, 4}, {"price10", 8, 2}, {"cat", 10, 1}, {"extra", 11, 1}};
}

// scenedat.bin records (data::SceneRec).
std::vector<TableField> scene_fields() {
  std::vector<TableField> out;
  add_series(out, "enemy", 0, 3, 1);
  for (int i = 0; i < 3; ++i) out.push_back({own(std::format("count_{}", i)), static_cast<u16>(3 + i), 1});
  return out;
}

// ditem.dat records (data::DropRow).
std::vector<TableField> drop_fields() {
  std::vector<TableField> out;
  add_series(out, "slot", 0, 4, 2);
  return out;
}

bool parse_number(std::string_view text, long long& out) {
  bool negative = false;
  if (!text.empty() && text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text.remove_prefix(2);
  }
  if (text.empty()) return false;
  unsigned long long value = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (ec != std::errc() || end != text.data() + text.size() || value > 0xFFFFFFFFull) return false;
  out = negative ? -static_cast<long long>(value) : static_cast<long long>(value);
  return true;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& letter : out)
    if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
  return out;
}

// "@<table>:<name>[+<number>]" -> (table, name, number).
bool parse_reference(std::string_view text, std::string& table, std::string& name, long long& addend) {
  if (text.empty() || text.front() != '@') return false;
  text.remove_prefix(1);
  const size_t colon = text.find(':');
  if (colon == std::string_view::npos) return false;
  table = lower(trim(text.substr(0, colon)));
  std::string_view rest = text.substr(colon + 1);
  addend = 0;
  if (const size_t plus = rest.find('+'); plus != std::string_view::npos) {
    if (!parse_number(trim(rest.substr(plus + 1)), addend) || addend < 0) return false;
    rest = rest.substr(0, plus);
  }
  name = std::string(trim(rest));
  return features::is_record_name(name) && !table_fields(table).empty();
}

}  // namespace

std::span<const TableField> table_fields(std::string_view table) {
  static const std::vector<TableField> stat = stat_fields(), elem = elem_fields(), menu_elem = menu_elem_fields(), gear = gear_fields(),
                                       item = item_fields(), scene = scene_fields(), drop = drop_fields();
  if (table == "monster" || table == "char" || table == "char20" || (table.starts_with("char_w") && table.size() == 7)) return stat;
  if (table == "pgdat") return elem;
  if (table == "menu_pg") return menu_elem;
  if (table == "waa") return gear;
  if (table == "menu_waa") return item;
  if (table == "scenedat") return scene;
  if (table == "ditem") return drop;
  return {};
}

std::string_view table_names_of(std::string_view table) { return table == "menu_pg" ? std::string_view("pgdat") : table; }

bool table_grows(std::string_view table) {
  return table == "monster" || table == "pgdat" || table == "menu_pg" || table == "scenedat" || table == "ditem" || table_is_list(table);
}

bool table_is_list(std::string_view table) { return table == "el_shop" || table == "waa_shop"; }

void set_table_mods(const vfs::Vfs* files, std::vector<TableMod> mods) { mods_storage() = {files, std::move(mods)}; }

const std::vector<TableMod>* table_mods(const vfs::Vfs& files) {
  const TableModStore& store = mods_storage();
  return store.files == &files ? &store.mods : nullptr;
}

std::vector<TableMod> table_mods_of(const vfs::ModManager& mods, const vfs::Vfs& files) {
  std::vector<TableMod> out;
  for (const vfs::Mod& mod : mods.mods()) {
    const bool lists_ids = mod.has_manifest && !mod.manifest.own_ids.empty();
    if (!mod.enabled || (mod.tables.empty() && !lists_ids)) continue;
    TableMod table{mod.name, {}, lists_ids ? mod.manifest.own_ids : std::map<std::string, std::vector<std::pair<int, std::string>>>{}};
    for (const vfs::ModFile& file : mod.tables)
      table.files[file.target] = [&files, source = vfs::OverlayFile{file.file, file.member}] { return files.read_mod_file(source); };
    out.push_back(std::move(table));
  }
  return out;
}

const TableRows& table_rows() { return rows_storage(); }
void set_table_rows(TableRows rows) { rows_storage() = std::move(rows); }

std::vector<RowBlock> parse_rows(std::string_view text, std::span<const TableField> fields, std::vector<std::string>* problems,
                                 bool list) {
  std::vector<RowBlock> out;
  auto problem = [&](int line, std::string what) {
    if (problems) problems->push_back(std::format("line {}: {}", line, what));
  };
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  bool in_record = false;
  int line_no = 0;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    ++line_no;
    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
    if (line.front() == '[') {
      const std::string_view inside = line.back() == ']' ? trim(line.substr(1, line.size() - 2)) : std::string_view{};
      RowBlock block;
      block.line = line_no;
      long long index = 0;
      if (features::is_record_name(inside)) {
        block.name = std::string(inside);
      } else if (parse_number(inside, index) && index >= 0) {
        block.index = static_cast<size_t>(index);
      } else {
        problem(line_no, "bad record header");
        in_record = false;
        continue;
      }
      out.push_back(std::move(block));
      in_record = true;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      problem(line_no, "expected <field>=<value>");
      continue;
    }
    if (!in_record) {
      problem(line_no, "field before any [record]");
      continue;
    }
    RowBlock& block = out.back();
    const std::string name = lower(trim(line.substr(0, eq)));
    const std::string_view value_text = trim(line.substr(eq + 1));
    if (name == "base") {
      long long index = 0;
      if (block.name.empty() || !parse_number(value_text, index) || index < 0) problem(line_no, "base=<index> belongs to a named record");
      else block.base = static_cast<size_t>(index);
      continue;
    }
    if (list) {
      if (name == "clear") {
        long long flag = 0;
        if (!parse_number(value_text, flag) || (flag != 0 && flag != 1)) problem(line_no, "clear=1 empties the row");
        else block.clear = flag == 1;
        continue;
      }
      RowWrite entry;
      entry.line = line_no;
      entry.width = 2;
      entry.op = name == "add" ? RowOp::Add : name == "remove" ? RowOp::Remove : RowOp::Set;
      long long value = 0;
      if (entry.op == RowOp::Set) {
        problem(line_no, "a list row takes add=, remove= and clear=");
        continue;
      }
      if (value_text.starts_with("@")) {
        if (!parse_reference(value_text, entry.ref_table, entry.ref_name, value) || value > 0xFFFF) {
          problem(line_no, "a reference is @<table>:<record name>[+<number>]");
          continue;
        }
      } else if (!parse_number(value_text, value) || value <= 0 || value > 0xFFFF) {
        problem(line_no, "an entry is 1..0xFFFF");
        continue;
      }
      entry.value = static_cast<u32>(value);
      block.writes.push_back(std::move(entry));
      continue;
    }
    RowWrite write;
    write.line = line_no;
    if (name.starts_with("0x")) {
      const size_t colon = name.find(':');
      long long offset = 0;
      const std::string_view type = colon == std::string::npos ? std::string_view{} : std::string_view(name).substr(colon + 1);
      write.width = type == "u8" ? 1 : type == "u16" ? 2 : type == "u32" ? 4 : 0;
      if (colon == std::string::npos || !write.width || !parse_number(std::string_view(name).substr(0, colon), offset) || offset < 0 ||
          offset > 0xFFFF) {
        problem(line_no, "raw field must be 0x<offset>:u8|u16|u32");
        continue;
      }
      write.offset = static_cast<u16>(offset);
    } else {
      const TableField* field = nullptr;
      for (const TableField& candidate : fields)
        if (name == candidate.name) field = &candidate;
      if (!field) {
        problem(line_no, std::format("unknown field '{}'", name));
        continue;
      }
      write.offset = field->offset;
      write.width = field->width;
    }
    const long long max = write.width == 4 ? 0xFFFFFFFFll : (1ll << (8 * write.width)) - 1;
    const long long min = -(1ll << (8 * write.width - 1));
    long long value = 0;
    if (value_text.starts_with("@")) {
      if (!parse_reference(value_text, write.ref_table, write.ref_name, value) || value > max) {
        problem(line_no, "a reference is @<table>:<record name>[+<number>]");
        continue;
      }
    } else if (!parse_number(value_text, value) || value > max || value < min) {
      problem(line_no, "value out of range for the field");
      continue;
    }
    write.value = static_cast<u32>(value) & static_cast<u32>(max);
    block.writes.push_back(std::move(write));
  }
  return out;
}

void TableRows::note(bool warning, std::string line) const {
  if (warning) log::warn("{}", line);
  else log::info("{}", line);
  log_lines_.push_back(std::move(line));
}

void TableRows::build(const std::vector<TableMod>& mods, const std::map<std::string, size_t>& counts) {
  *this = TableRows{};
  for (const TableMod& mod : mods) mod_names_.push_back(mod.name);
  for (size_t mod_index = 0; mod_index < mods.size(); ++mod_index) {
    for (const auto& [table, read] : mods[mod_index].files) {
      const std::span<const TableField> fields = table_fields(table);
      if (fields.empty() && !table_is_list(table)) {
        note(true, std::format("tables: '{}' {}.txt: not a table rows can change", mods[mod_index].name, table));
        continue;
      }
      const std::optional<Bytes> bytes = read();
      if (!bytes) continue;
      std::vector<std::string> problems;
      Parsed parsed{mod_index, parse_rows(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), fields, &problems,
                                          table_is_list(table))};
      for (const std::string& problem : problems) note(true, std::format("tables: '{}' {}.txt {}", mods[mod_index].name, table, problem));
      parsed_[table].push_back(std::move(parsed));
    }
  }
  // Named records get ids after the table's own records, per set of names (menu_pg shares pgdat's): mod by mod in priority order, a
  // mod's [Elements] (by its own id) first, then its other names in name order. A name another mod already added keeps its id.
  std::map<std::string, std::map<size_t, std::set<std::string>>> added;
  for (const auto& [table, list] : parsed_) {
    const std::string names_table(table_names_of(table));
    for (const Parsed& parsed : list)
      for (const RowBlock& block : parsed.blocks) {
        if (block.name.empty()) continue;
        if (!table_grows(table) || !counts.contains(names_table)) {
          note(true, std::format("tables: '{}' {}.txt line {}: records cannot be added to this table", mod_names_[parsed.mod], table,
                                 block.line));
          continue;
        }
        added[names_table][parsed.mod].insert(block.name);
      }
  }
  for (auto& [names_table, by_mod] : added) {
    Names& names = names_[names_table];
    names.first = counts.at(names_table);
    for (auto& [mod_index, mod_names] : by_mod) {
      std::vector<std::pair<int, std::string>> listed;
      if (const auto own = mods[mod_index].own_ids.find(names_table); own != mods[mod_index].own_ids.end()) listed = own->second;
      std::sort(listed.begin(), listed.end());
      std::vector<std::string> order;
      for (const auto& [own_id, name] : listed)
        if (mod_names.erase(name)) order.push_back(name);
      order.insert(order.end(), mod_names.begin(), mod_names.end());
      for (const std::string& name : order)
        if (names.ids.emplace(name, names.first + names.names.size()).second) names.names.push_back(name);
    }
  }
  for (const auto& [table, names] : names_)
    if (!names.names.empty())
      note(false, std::format("tables: {} adds {} named record(s) from {}", table, names.names.size(), names.first));
  // The mods' own ids: each listed name that some mod adds.
  for (const TableMod& mod : mods)
    for (const auto& [table, ids] : mod.own_ids) {
      std::string_view section = table;
      for (const features::OwnIdSection& own : features::own_id_sections())
        if (table == own.table) section = own.section;
      for (const auto& [own_id, name] : ids) {
        if (const std::optional<size_t> id = id_of(table, name)) own_ids_[mod.name][table][own_id] = *id;
        else note(true, std::format("tables: '{}' [{}] {}={}: no mod adds that record", mod.name, section, own_id, name));
      }
    }
}

size_t TableRows::total(std::string_view table, size_t count) const {
  const auto names = names_.find(table_names_of(table));
  if (names == names_.end() || names->second.names.empty()) return count;
  return std::max(count, names->second.first + names->second.names.size());
}

std::optional<size_t> TableRows::id_of(std::string_view table, std::string_view name) const {
  const auto names = names_.find(table_names_of(table));
  if (names == names_.end()) return std::nullopt;
  const auto it = names->second.ids.find(name);
  return it == names->second.ids.end() ? std::nullopt : std::optional<size_t>(it->second);
}

const std::string& TableRows::name_of(std::string_view table, size_t id) const {
  const auto names = names_.find(table_names_of(table));
  if (names == names_.end() || id < names->second.first || id - names->second.first >= names->second.names.size()) return kNoName;
  return names->second.names[id - names->second.first];
}

std::optional<size_t> TableRows::record_of(std::string_view mod, std::string_view table, int own_id) const {
  const auto by_mod = own_ids_.find(mod);
  if (by_mod == own_ids_.end()) return std::nullopt;
  const auto by_table = by_mod->second.find(table);
  if (by_table == by_mod->second.end()) return std::nullopt;
  const auto it = by_table->second.find(own_id);
  return it == by_table->second.end() ? std::nullopt : std::optional<size_t>(it->second);
}

std::optional<size_t> TableRows::record_for(std::string_view mod, std::string_view table, int own_id) const {
  if (own_id < 0) return std::nullopt;
  if (const std::optional<size_t> listed = record_of(mod, table, own_id)) return listed;
  const auto names = names_.find(table_names_of(table));
  if (names != names_.end() && !names->second.names.empty() && static_cast<size_t>(own_id) >= names->second.first) return std::nullopt;
  return static_cast<size_t>(own_id);
}

const std::vector<TableRows::WideWrite>& TableRows::wide_writes(std::string_view table) const {
  static const std::vector<WideWrite> kNone;
  const auto it = wide_.find(table);
  return it == wide_.end() ? kNone : it->second;
}
size_t TableRows::apply_lists(std::string_view table, std::vector<std::vector<u16>>& rows) const {
  rows.resize(total(table, rows.size()));
  const auto list = parsed_.find(table);
  if (list == parsed_.end()) return 0;
  // A new row starts as a copy of its base; the first mod in the list that gives a base decides.
  std::set<size_t> based;
  for (const Parsed& parsed : list->second)
    for (const RowBlock& block : parsed.blocks) {
      const std::optional<size_t> index = block.name.empty() ? std::nullopt : id_of(table, block.name);
      if (!block.base || !index || *index >= rows.size() || based.contains(*index)) continue;
      if (*block.base >= rows.size() || *block.base == *index) {
        note(true, std::format("tables: '{}' {}.txt line {}: no base row {}", mod_names_[parsed.mod], table, block.line, *block.base));
        continue;
      }
      rows[*index] = rows[*block.base];
      based.insert(*index);
    }
  size_t total_changed = 0;
  for (auto parsed = list->second.rbegin(); parsed != list->second.rend(); ++parsed) {
    const std::string& mod = mod_names_[parsed->mod];
    size_t changed = 0;
    std::vector<std::string> problems;
    for (const RowBlock& block : parsed->blocks) {
      const std::optional<size_t> index = block.index ? block.index : id_of(table, block.name);
      if (!index || *index >= rows.size()) {
        problems.push_back(std::format("line {}: no row {} (the table has {})", block.line,
                                       block.index ? std::to_string(*block.index) : block.name, rows.size()));
        continue;
      }
      std::vector<u16>& row = rows[*index];
      if (block.clear) row.clear();
      for (const RowWrite& entry : block.writes) {
        u32 value = entry.value;
        if (!entry.ref_name.empty()) {
          const std::optional<size_t> target = id_of(entry.ref_table, entry.ref_name);
          if (!target) {
            problems.push_back(std::format("line {}: no record {}:{}", entry.line, entry.ref_table, entry.ref_name));
            continue;
          }
          if (*target + value > 0xFFFF) {
            problems.push_back(std::format("line {}: {}:{} is record {}, past a list entry", entry.line, entry.ref_table, entry.ref_name,
                                           *target + value));
            continue;
          }
          value += static_cast<u32>(*target);
        }
        const u16 item = static_cast<u16>(value);
        if (entry.op == RowOp::Add && std::find(row.begin(), row.end(), item) == row.end()) {
          row.push_back(item);
          ++changed;
        } else if (entry.op == RowOp::Remove) {
          const auto kept = std::remove(row.begin(), row.end(), item);
          changed += static_cast<size_t>(row.end() - kept);
          row.erase(kept, row.end());
        }
      }
    }
    for (const std::string& problem : problems) note(true, std::format("tables: '{}' {}.txt {}", mod, table, problem));
    note(false, std::format("tables: '{}' {}: {} entr{} added or removed", mod, table, changed, changed == 1 ? "y" : "ies"));
    total_changed += changed;
  }
  return total_changed;
}
size_t TableRows::apply(std::string_view table, size_t count, const std::function<std::span<u8>(size_t)>& record) const {
  // A new record starts as a copy of its base, before any field is written; the first mod in the list that gives a base decides. A
  // table that shares another's names (menu_pg) takes the base that table's rows give when its own rows give none.
  std::set<size_t> based;
  auto copy_bases = [&](const std::vector<Parsed>& source, bool own) {
    for (const Parsed& parsed : source)
      for (const RowBlock& block : parsed.blocks) {
        const std::optional<size_t> index = block.name.empty() ? std::nullopt : id_of(table, block.name);
        if (!block.base || !index || *index >= count || based.contains(*index)) continue;
        if (*block.base >= count || *block.base == *index) {
          if (own)
            note(true, std::format("tables: '{}' {}.txt line {}: no base record {}", mod_names_[parsed.mod], table, block.line, *block.base));
          continue;
        }
        const std::span<u8> base = record(*block.base);
        const std::span<u8> rec = record(*index);
        std::copy_n(base.begin(), std::min(base.size(), rec.size()), rec.begin());
        if (const auto wide = wide_.find(table); wide != wide_.end())
          std::erase_if(wide->second, [&](const WideWrite& entry) { return entry.record == *index; });
        based.insert(*index);
      }
  };
  const auto list = parsed_.find(table);
  if (list != parsed_.end()) copy_bases(list->second, true);
  if (const std::string_view names_table = table_names_of(table); names_table != table)
    if (const auto shared = parsed_.find(names_table); shared != parsed_.end()) copy_bases(shared->second, false);
  if (list == parsed_.end()) return 0;
  struct Owner {
    size_t mod;
    u32 value;
  };
  std::map<std::pair<size_t, u16>, Owner> written;
  size_t total_written = 0;
  for (auto parsed = list->second.rbegin(); parsed != list->second.rend(); ++parsed) {
    const std::string& mod = mod_names_[parsed->mod];
    size_t applied = 0;
    std::vector<std::string> problems;
    for (const RowBlock& block : parsed->blocks) {
      const std::optional<size_t> index = block.index ? block.index : id_of(table, block.name);
      if (!index || *index >= count) {
        problems.push_back(std::format("line {}: no record {} (the table has {})", block.line,
                                       block.index ? std::to_string(*block.index) : block.name, count));
        continue;
      }
      std::span<u8> rec = record(*index);
      for (const RowWrite& write : block.writes) {
        u32 value = write.value;
        const bool enemy_slot = table == "scenedat" && write.offset < kSceneEnemySlots;
        if (!write.ref_name.empty()) {
          const std::optional<size_t> target = id_of(write.ref_table, write.ref_name);
          if (!target) {
            problems.push_back(std::format("line {}: no record {}:{}", write.line, write.ref_table, write.ref_name));
            continue;
          }
          if (enemy_slot && *target + value == kEmptyEnemySlot) {
            problems.push_back(std::format("line {}: {}:{} is record {}, the empty formation slot", write.line, write.ref_table, write.ref_name,
                                           *target + value));
            continue;
          }
          if (!enemy_slot && write.width < 4 && *target + value >= (size_t{1} << (8 * write.width))) {
            problems.push_back(std::format("line {}: {}:{} is record {}, past a {}-byte field", write.line, write.ref_table, write.ref_name,
                                           *target + value, write.width));
            continue;
          }
          value += static_cast<u32>(*target);
        }
        if (static_cast<size_t>(write.offset) + write.width > rec.size()) {
          problems.push_back(std::format("line {}: no field 0x{:X} in a {}-byte record", write.line, write.offset, rec.size()));
          continue;
        }
        if (enemy_slot && !write.ref_name.empty() && value >= 0xFF) {
          auto& wide = wide_[std::string(table)];
          std::erase_if(wide, [&](const WideWrite& entry) { return entry.record == *index && entry.offset == write.offset; });
          wide.push_back({*index, write.offset, value});
          ++applied;
          continue;
        }
        if (const auto wide = wide_.find(table); wide != wide_.end())
          std::erase_if(wide->second, [&](const WideWrite& entry) { return entry.record == *index && entry.offset == write.offset; });
        const auto key = std::make_pair(*index, write.offset);
        if (auto prev = written.find(key); prev != written.end() && prev->second.mod != parsed->mod && prev->second.value != value)
          note(false, std::format("tables: {} [{}] 0x{:X}: '{}' wins over '{}'", table,
                                  block.name.empty() ? std::to_string(*index) : block.name, write.offset, mod,
                                  mod_names_[prev->second.mod]));
        written[key] = {parsed->mod, value};
        for (u8 i = 0; i < write.width; ++i) rec[write.offset + i] = static_cast<u8>(value >> (8 * i));
        ++applied;
      }
    }
    for (const std::string& problem : problems) note(true, std::format("tables: '{}' {}.txt {}", mod, table, problem));
    note(false, std::format("tables: '{}' {}: {} field(s)", mod, table, applied));
    total_written += applied;
  }
  return total_written;
}

}  // namespace oc::data
