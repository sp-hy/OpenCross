// Checks for table rows (a mod's tables\<table>.txt): parsing named and raw fields, references and bad lines; applying several mods'
// rows over a table's records with the first mod in the list winning a field; records added by name (ids after the table's own, a
// mod's [Elements] order first, a base copy, references between tables, menu_pg following pgdat) and the mods' own element ids.
#include <array>
#include <cstdio>
#include <map>
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

data::TableMod mod_with(const std::string& name, std::map<std::string, std::string> files,
                        std::vector<std::pair<int, std::string>> elements = {},
                        std::vector<std::pair<int, std::string>> element_shops = {}) {
  data::TableMod mod{name, {}, {}};
  if (!elements.empty()) mod.own_ids["pgdat"] = std::move(elements);
  if (!element_shops.empty()) mod.own_ids["el_shop"] = std::move(element_shops);
  for (const auto& [table, text] : files) mod.files[table] = [text] { return std::optional<Bytes>(Bytes(text.begin(), text.end())); };
  return mod;
}

using Stat = std::array<u8, 0xB0>;
using Elem = std::array<u8, 0x2C>;

u16 rd16(std::span<const u8> rec, size_t offset) { return static_cast<u16>(rec[offset] | (rec[offset + 1] << 8)); }

bool logged(const data::TableRows& rows, const std::string& text) {
  for (const std::string& line : rows.log_lines())
    if (line.find(text) != std::string::npos) return true;
  return false;
}
}  // namespace

int main() {
  // Parsing: named fields with their widths, raw fields, hex and negative values, references, record names; problems per line.
  {
    std::vector<std::string> problems;
    const auto blocks = data::parse_rows(
        "\xEF\xBB\xBF; a comment\n[12]\nmax_hp=900\r\nATK = 0x2A\n0xA6:u8=-1\ncell_47=@pgdat:my_spark+0x4000\n"
        "[0x10]\nnot_a_field=1\nmag=256\nhit\n0x30:u24=1\nbase=3\n[My Bad]\nevd=1\n[new_one]\nbase=5\ncell_0=@nothing:x\n",
        data::table_fields("monster"), &problems);
    CHECK(blocks.size() == 3);
    if (blocks.size() == 3) {
      const auto& first = blocks[0].writes;
      CHECK(blocks[0].index == 12 && first.size() == 4);
      CHECK(first.size() == 4 && first[0].offset == 0x16 && first[0].width == 2 && first[0].value == 900);
      CHECK(first.size() == 4 && first[1].offset == 0x18 && first[1].value == 0x2A);
      CHECK(first.size() == 4 && first[2].offset == 0xA6 && first[2].width == 1 && first[2].value == 0xFF);
      CHECK(first.size() == 4 && first[3].offset == 0x40 + 2 * 47 && first[3].ref_table == "pgdat" && first[3].ref_name == "my_spark" &&
            first[3].value == 0x4000);
      CHECK(blocks[2].name == "new_one" && blocks[2].base == 5 && blocks[2].writes.empty());
    }
    CHECK(problems.size() == 8);
  }
  CHECK(!data::table_fields("pgdat").empty() && !data::table_fields("char_w3").empty() && data::table_fields("nothing").empty());
  CHECK(data::table_names_of("menu_pg") == "pgdat" && data::table_grows("menu_pg") && !data::table_grows("char"));

  // Edits: rows go over the records; the first mod in the list wins a field both set; a record past the table is skipped.
  {
    std::vector<Stat> records(3);
    records[1][0x16] = 100;
    const std::vector<data::TableMod> mods = {
        mod_with("Top", {{"monster", "[1]\nmax_hp=500\n"}}),
        mod_with("Other", {{"pgdat", "[1]\npower=9\n"}}),
        mod_with("Bottom", {{"monster", "[1]\nmax_hp=300\natk=40\n[2]\ngold=1234\n[3]\natk=1\n"}}),
    };
    data::TableRows rows;
    rows.build(mods, {{"monster", 3}, {"pgdat", 2}});
    CHECK(rows.total("monster", 3) == 3);
    const size_t written = rows.apply("monster", records.size(), [&](size_t i) { return std::span<u8>(records[i]); });
    CHECK(written == 4);
    CHECK(rd16(records[1], 0x16) == 500);
    CHECK(records[1][0x18] == 40);
    CHECK(rd16(records[2], 0xA2) == 1234);
    CHECK(logged(rows, "tables: monster [1] 0x16: 'Top' wins over 'Bottom'"));
    CHECK(logged(rows, "no record 3"));
  }

  // The global list (set at launch) belongs to the VFS it was set for only.
  {
    const vfs::Vfs game, plain;
    CHECK(!data::table_mods(game));
    data::set_table_mods(&game, {mod_with("Only", {{"ditem", "[0]\nslot_3=0x1FF\n"}})});
    CHECK(!data::table_mods(plain));
    CHECK(data::table_mods(game) && data::table_mods(game)->size() == 1 && data::table_mods(game)->front().name == "Only");
    data::set_table_mods(nullptr, {});
  }

  // Records by name: ids after the table's own records, mod by mod in priority order ([Elements] order first, then name order); a
  // name two mods give is one record; base copies; references from another table; menu_pg follows pgdat; the mods' own ids.
  {
    const std::vector<data::TableMod> mods = {
        mod_with("First", {{"pgdat", "[zap]\nbase=1\npower=50\n[bolt]\npower=7\n"}, {"menu_pg", "[zap]\nprice=300\n"}},
                 {{600, "zap"}}),
        mod_with("Second", {{"pgdat", "[alpha]\npower=1\n[zap]\npower=99\nhits=3\n"},
                            {"char", "[0]\ncell_3=@pgdat:alpha+0x4000\ncell_4=@pgdat:missing\n"}},
                 {{527, "alpha"}, {528, "zap"}}),
    };
    data::TableRows rows;
    rows.build(mods, {{"pgdat", 4}});
    CHECK(rows.id_of("pgdat", "zap") == 4);
    CHECK(rows.id_of("pgdat", "bolt") == 5);
    CHECK(rows.id_of("pgdat", "alpha") == 6);
    CHECK(rows.id_of("menu_pg", "zap") == 4);
    CHECK(rows.name_of("pgdat", 5) == "bolt" && rows.name_of("pgdat", 1).empty());
    CHECK(rows.total("pgdat", 4) == 7);
    CHECK(rows.total("menu_pg", 2) == 7);
    CHECK(rows.element_of("First", 600) == 4);
    CHECK(rows.element_of("Second", 527) == 6 && rows.element_of("Second", 528) == 4);
    CHECK(!rows.element_of("Second", 600));
    CHECK(rows.element_for("First", 600) == 4 && rows.element_for("First", 3) == 3 && rows.element_for("", 2) == 2);
    CHECK(!rows.element_for("First", 5) && !rows.element_for("", 6));

    std::vector<Elem> elems(7);
    elems[1][0x0E] = 2;
    elems[1][0x08] = 10;
    rows.apply("pgdat", elems.size(), [&](size_t i) { return std::span<u8>(elems[i]); });
    CHECK(elems[4][0x08] == 50 && elems[4][0x0E] == 3);
    CHECK(elems[5][0x08] == 7 && elems[6][0x08] == 1);
    CHECK(logged(rows, "tables: pgdat [zap] 0x8: 'First' wins over 'Second'"));

    std::vector<std::array<u8, 16>> menu(7);
    menu[1][0] = 0x0F;
    rows.apply("menu_pg", menu.size(), [&](size_t i) { return std::span<u8>(menu[i]); });
    CHECK(rd16(menu[4], 0x08) == 300 && menu[4][0] == 0x0F);
    CHECK(menu[5][0] == 0);

    std::vector<Stat> chars(1);
    rows.apply("char", chars.size(), [&](size_t i) { return std::span<u8>(chars[i]); });
    CHECK(rd16(chars[0], 0x40 + 6) == 0x4006);
    CHECK(rd16(chars[0], 0x40 + 8) == 0 && logged(rows, "no record pgdat:missing"));
  }

  // A table rows cannot grow, and a name no mod adds in [Elements], are reported.
  {
    data::TableRows rows;
    rows.build({mod_with("Lone", {{"char", "[newcomer]\natk=1\n"}}, {{700, "ghost"}})}, {});
    CHECK(logged(rows, "records cannot be added to this table"));
    CHECK(logged(rows, "[Elements] 700=ghost: no mod adds that record"));
  }

  // A reference past its field's width is a problem, not a wrapped id.
  {
    data::TableRows rows;
    rows.build({mod_with("Big", {{"monster", "[big]\natk=1\n"}, {"scenedat", "[0]\n0x10:u8=@monster:big\n"}})},
               {{"monster", 256}, {"scenedat", 1}});
    std::vector<std::array<u8, 28>> scenes(1);
    scenes[0][0x10] = 7;
    rows.apply("scenedat", scenes.size(), [&](size_t i) { return std::span<u8>(scenes[i]); });
    CHECK(scenes[0][0x10] == 7 && logged(rows, "monster:big is record 256, past a 1-byte field") && rows.wide_writes("scenedat").empty());
  }

  // Shops are lists: every mod's adds and removes land, lowest priority first (so the first mod's remove follows another's add); a
  // shop by name follows the table's rows as a copy of its base; entries may name records; a mod's own shop ids ([ElementShops]).
  {
    const std::vector<data::TableMod> mods = {
        mod_with("Top", {{"el_shop", "[3]\nremove=7\nadd=@pgdat:zap\n[hub_shop]\nbase=3\nadd=9\nset=1\n"}, {"pgdat", "[zap]\npower=1\n"}},
                 {}, {{32, "hub_shop"}}),
        mod_with("Bottom", {{"el_shop", "[3]\nadd=7\nadd=8\n[2]\nclear=1\nadd=5\n"}}),
    };
    data::TableRows rows;
    rows.build(mods, {{"pgdat", 4}, {"el_shop", 32}});
    std::vector<std::vector<u16>> shops(32);
    shops[3] = {1, 7};
    shops[2] = {4, 6};
    rows.apply_lists("el_shop", shops);
    CHECK(shops.size() == 33);
    CHECK((shops[3] == std::vector<u16>{1, 8, 4}));
    CHECK((shops[2] == std::vector<u16>{5}));
    CHECK((shops[32] == std::vector<u16>{1, 7, 9}));
    CHECK(logged(rows, "a list row takes add=, remove= and clear="));
    CHECK(rows.record_for("Top", "el_shop", 32) == 32 && !rows.record_for("Bottom", "el_shop", 32) && rows.record_for("", "el_shop", 5) == 5);
    CHECK(data::table_is_list("waa_shop") && !data::table_is_list("pgdat"));
  }

  // A reference that doesn't fit its field (a formation slot's byte; 0xFF is empty) is kept as a wide write for the loader; a plain
  // write from a mod higher in the list takes the slot back.
  {
    const std::vector<data::TableMod> mods = {
        mod_with("Top", {{"scenedat", "[1]\nenemy_1=4\n"}}),
        mod_with("Bottom", {{"scenedat", "[0]\nenemy_0=@monster:big_boss\n[1]\nenemy_1=@monster:big_boss\n"},
                            {"monster", "[big_boss]\nmax_hp=9999\n"}}),
    };
    data::TableRows rows;
    rows.build(mods, {{"monster", 300}, {"scenedat", 2}});
    std::vector<std::array<u8, 28>> scenes(2);
    rows.apply("scenedat", scenes.size(), [&](size_t i) { return std::span<u8>(scenes[i]); });
    const auto& wide = rows.wide_writes("scenedat");
    CHECK(wide.size() == 1 && wide[0].record == 0 && wide[0].offset == 0 && wide[0].value == 300);
    CHECK(scenes[0][0] == 0 && scenes[1][1] == 4);
  }

  if (failures == 0) std::printf("table_rows: OK\n");
  return failures == 0 ? 0 : 1;
}
