#include "data/table_rows.h"

#include <charconv>
#include <deque>
#include <format>
#include <utility>

#include "core/log.h"

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
  for (char& ch : out)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return out;
}

}  // namespace

std::span<const TableField> table_fields(std::string_view table) {
  static const std::vector<TableField> stat = stat_fields(), elem = elem_fields(), gear = gear_fields(), item = item_fields(),
                                       scene = scene_fields(), drop = drop_fields();
  if (table == "monster" || table == "char" || table == "char20" || (table.starts_with("char_w") && table.size() == 7)) return stat;
  if (table == "pgdat") return elem;
  if (table == "waa") return gear;
  if (table == "menu_waa") return item;
  if (table == "scenedat") return scene;
  if (table == "ditem") return drop;
  return {};
}

void set_table_mods(const vfs::Vfs* files, std::vector<TableMod> mods) { mods_storage() = {files, std::move(mods)}; }

std::vector<RowWrite> parse_rows(std::string_view text, std::span<const TableField> fields, std::vector<std::string>* problems) {
  std::vector<RowWrite> out;
  auto problem = [&](int line, std::string what) {
    if (problems) problems->push_back(std::format("line {}: {}", line, what));
  };
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  std::optional<size_t> record;
  int line_no = 0;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    ++line_no;
    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
    if (line.front() == '[') {
      long long index = 0;
      if (line.back() != ']' || !parse_number(trim(line.substr(1, line.size() - 2)), index) || index < 0) {
        problem(line_no, "bad record header");
        record.reset();
        continue;
      }
      record = static_cast<size_t>(index);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      problem(line_no, "expected <field>=<value>");
      continue;
    }
    if (!record) {
      problem(line_no, "field before any [record]");
      continue;
    }
    const std::string name = lower(trim(line.substr(0, eq)));
    RowWrite write{*record, 0, 0, 0, line_no};
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
    long long value = 0;
    const long long max = write.width == 4 ? 0xFFFFFFFFll : (1ll << (8 * write.width)) - 1;
    const long long min = -(1ll << (8 * write.width - 1));
    if (!parse_number(trim(line.substr(eq + 1)), value) || value > max || value < min) {
      problem(line_no, "value out of range for the field");
      continue;
    }
    write.value = static_cast<u32>(value) & static_cast<u32>(max);
    out.push_back(write);
  }
  return out;
}

size_t apply_table_rows(const std::vector<TableMod>& mods, std::string_view table, size_t count,
                        const std::function<std::span<u8>(size_t)>& record, std::vector<std::string>* log_lines) {
  auto note = [&](bool warning, std::string line) {
    if (warning) log::warn("{}", line);
    else log::info("{}", line);
    if (log_lines) log_lines->push_back(std::move(line));
  };
  const std::span<const TableField> fields = table_fields(table);
  struct Owner {
    size_t mod;
    u32 value;
  };
  std::map<std::pair<size_t, u16>, Owner> written;
  size_t total = 0;
  for (size_t mod_index = mods.size(); mod_index-- > 0;) {
    const TableMod& mod = mods[mod_index];
    const auto file = mod.files.find(std::string(table));
    if (file == mod.files.end()) continue;
    const std::optional<Bytes> bytes = file->second();
    if (!bytes) continue;
    std::vector<std::string> problems;
    const std::vector<RowWrite> writes =
        parse_rows(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), fields, &problems);
    size_t applied = 0;
    for (const RowWrite& write : writes) {
      std::span<u8> rec = write.record < count ? record(write.record) : std::span<u8>{};
      if (write.record >= count || static_cast<size_t>(write.offset) + write.width > rec.size()) {
        problems.push_back(std::format("line {}: no field 0x{:X} in record {} (the table has {})", write.line, write.offset,
                                       write.record, count));
        continue;
      }
      const auto key = std::make_pair(write.record, write.offset);
      if (auto prev = written.find(key); prev != written.end() && prev->second.mod != mod_index && prev->second.value != write.value)
        note(false, std::format("tables: {} [{}] 0x{:X}: '{}' wins over '{}'", table, write.record, write.offset, mod.name,
                                mods[prev->second.mod].name));
      written[key] = {mod_index, write.value};
      for (u8 i = 0; i < write.width; ++i) rec[write.offset + i] = static_cast<u8>(write.value >> (8 * i));
      ++applied;
    }
    for (const std::string& problem : problems) note(true, std::format("tables: '{}' {}.txt {}", mod.name, table, problem));
    note(false, std::format("tables: '{}' {}: {} field(s)", mod.name, table, applied));
    total += applied;
  }
  return total;
}

size_t apply_table_rows(const vfs::Vfs& files, std::string_view table, size_t count, const std::function<std::span<u8>(size_t)>& record) {
  const TableModStore& store = mods_storage();
  if (store.files != &files || store.mods.empty()) return 0;
  return apply_table_rows(store.mods, table, count, record);
}

}  // namespace oc::data
