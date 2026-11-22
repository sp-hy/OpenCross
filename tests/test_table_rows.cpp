// Checks for table rows (a mod's tables\<table>.txt): parsing named and raw fields, value ranges and bad lines, and applying several
// mods' rows over a table's records with the first mod in the list winning a field.
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "data/table_rows.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

data::TableMod mod_with(const std::string& name, const std::string& table, const std::string& text) {
  data::TableMod mod{name, {}};
  mod.files[table] = [text] { return std::optional<Bytes>(Bytes(text.begin(), text.end())); };
  return mod;
}

using Record = std::array<u8, 0xB0>;

u16 rd16(const Record& rec, size_t offset) { return static_cast<u16>(rec[offset] | (rec[offset + 1] << 8)); }
}  // namespace

int main() {
  // Parsing: named fields with their widths, raw fields, hex and negative values, comments; problems are reported per line.
  {
    std::vector<std::string> problems;
    const auto writes = data::parse_rows(
        "\xEF\xBB\xBF; a comment\n[12]\nmax_hp=900\r\nATK = 0x2A\n0xA6:u8=-1\ncell_47=0x4005\n"
        "[0x10]\nnot_a_field=1\nmag=256\nhit\n0x30:u24=1\n[x]\nevd=1\n",
        data::table_fields("monster"), &problems);
    CHECK(writes.size() == 4);
    CHECK(writes.size() == 4 && writes[0].record == 12 && writes[0].offset == 0x16 && writes[0].width == 2 && writes[0].value == 900);
    CHECK(writes.size() == 4 && writes[1].offset == 0x18 && writes[1].value == 0x2A);
    CHECK(writes.size() == 4 && writes[2].offset == 0xA6 && writes[2].width == 1 && writes[2].value == 0xFF);
    CHECK(writes.size() == 4 && writes[3].offset == 0x40 + 2 * 47 && writes[3].value == 0x4005);
    CHECK(problems.size() == 6);
  }
  CHECK(!data::table_fields("pgdat").empty() && !data::table_fields("char_w3").empty() && data::table_fields("nothing").empty());

  // Applying: rows go over the records; the first mod in the list wins a field both set; a record past the table is skipped.
  {
    std::vector<Record> records(3);
    records[1][0x16] = 100;
    records[1][0x18] = 7;
    const std::vector<data::TableMod> mods = {
        mod_with("Top", "monster", "[1]\nmax_hp=500\n"),
        mod_with("Other", "pgdat", "[1]\npower=9\n"),
        mod_with("Bottom", "monster", "[1]\nmax_hp=300\natk=40\n[2]\ngold=1234\n[3]\natk=1\n"),
    };
    std::vector<std::string> log_lines;
    const size_t written =
        data::apply_table_rows(mods, "monster", records.size(), [&](size_t i) { return std::span<u8>(records[i]); }, &log_lines);
    CHECK(written == 4);
    CHECK(rd16(records[1], 0x16) == 500);
    CHECK(records[1][0x18] == 40);
    CHECK(rd16(records[2], 0xA2) == 1234);
    bool conflict = false, skipped = false;
    for (const std::string& line : log_lines) {
      conflict = conflict || line == "tables: monster [1] 0x16: 'Top' wins over 'Bottom'";
      skipped = skipped || line.find("no field 0x18 in record 3") != std::string::npos;
    }
    CHECK(conflict);
    CHECK(skipped);
  }

  // The global list (set at launch) is what apply_table_rows reads without an explicit list, for the VFS it was set for only.
  {
    const vfs::Vfs game, plain;
    data::set_table_mods(&game, {mod_with("Only", "ditem", "[0]\nslot_3=0x1FF\n")});
    std::array<u8, 8> row{};
    CHECK(data::apply_table_rows(plain, "ditem", 1, [&](size_t) { return std::span<u8>(row); }) == 0);
    CHECK(row[6] == 0 && row[7] == 0);
    data::apply_table_rows(game, "ditem", 1, [&](size_t) { return std::span<u8>(row); });
    CHECK(row[6] == 0xFF && row[7] == 0x01);
    data::set_table_mods(nullptr, {});
  }

  if (failures == 0) std::printf("table_rows: OK\n");
  return failures == 0 ? 0 : 1;
}
