// Checks for table rows (a mod's tables\<table>.txt): parsing named and raw fields, references and bad lines; applying several mods'
// rows over a table's records with the first mod in the list winning a field; records added by name (ids after the table's own, a
// mod's [Elements] order first, a base copy, references between tables, menu_pg following pgdat, items past the game's 256 with menu_waa
// following waa, characters with every character set following char) and the mods' own element and item ids; who may equip an item
// (equip_add / equip_remove, an added character's equips_like).
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "data/table_rows.h"
#include "data/tables.h"
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
  CHECK(data::table_names_of("menu_pg") == "pgdat" && data::table_grows("menu_pg") && !data::table_grows("btbgdat"));
  // The character sets share char's names: a character added by name has one id in every set.
  CHECK(data::table_names_of("char20") == "char" && data::table_names_of("char_w4") == "char" && data::table_grows("char") &&
        data::table_grows("char_w0"));

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

  // Items by name: waa grows past the game's 256 records, menu_waa follows waa, a base copy, an item reference from a drop row, a mod's
  // own item ids ([Items]) and the game's items passing through.
  {
    data::TableMod mod = mod_with("Smithy", {{"waa", "[star_blade]\nbase=40\npower=99\n"},
                                             {"menu_waa", "[star_blade]\nprice10=500\n"},
                                             {"waa_tbl", "[star_blade]\nmat_3=2\n[40]\nmat_5=7\n"},
                                             {"ditem", "[3]\nslot_1=@waa:star_blade\n"}});
    mod.own_ids["waa"] = {{300, "star_blade"}};
    data::TableRows rows;
    rows.build({mod}, {{"waa", 256}, {"ditem", 4}});
    CHECK(data::table_names_of("menu_waa") == "waa" && data::table_grows("waa") && data::table_grows("menu_waa"));
    CHECK(rows.id_of("waa", "star_blade") == 256 && rows.id_of("menu_waa", "star_blade") == 256);
    CHECK(rows.total("waa", 256) == 257 && rows.total("menu_waa", 256) == 257);
    CHECK(rows.item_for("Smithy", 300) == 256 && !rows.item_for("Smithy", 301) && rows.item_for("", 40) == 40);
    std::vector<std::array<u8, 0x1C>> gear(257);
    gear[40][0x01] = 3;
    gear[40][0x04] = 20;
    rows.apply("waa", gear.size(), [&](size_t i) { return std::span<u8>(gear[i]); });
    CHECK(gear[256][0x01] == 3 && gear[256][0x04] == 99);
    std::vector<std::array<u8, 12>> info(257);
    rows.apply("menu_waa", info.size(), [&](size_t i) { return std::span<u8>(info[i]); });
    CHECK(rd16(info[256], 0x08) == 500);
    CHECK(data::table_names_of("waa_tbl") == "waa" && data::table_grows("waa_tbl"));
    std::vector<std::array<u8, 32>> forge(257);
    rows.apply("waa_tbl", forge.size(), [&](size_t i) { return std::span<u8>(forge[i]); });
    CHECK(forge[256][3] == 2 && forge[40][5] == 7);
    std::vector<std::array<u8, 8>> drops(4);
    rows.apply("ditem", drops.size(), [&](size_t i) { return std::span<u8>(drops[i]); });
    CHECK(rd16(drops[3], 2) == 256);
  }

  // Trading posts: entries keyed by goods; add sets the words it gives on an entry the post has, a new entry needs levels=; remove;
  // a post by name with base=; goods named by reference past the game's 256; the 16-entry cap; shop entries take no words.
  {
    const std::vector<data::TableMod> mods = {
        mod_with("Top", {{"trade", "[2]\nadd=7 levels=99\nadd=@waa:star_blade colour=0x10 levels=12\nremove=9\nadd=11 colour=4\n"
                                   "[bazaar]\nbase=2\nadd=12 levels=5\n"},
                         {"waa", "[star_blade]\nbase=40\n"},
                         {"el_shop", "[1]\nadd=5 levels=3\n"}}),
        mod_with("Bottom", {{"trade", "[2]\nadd=7 levels=50 colour=8\n[3]\nclear=1\nadd=1 levels=1\nadd=2 levels=1\nadd=3 levels=1\n"
                                      "add=4 levels=1\nadd=5 levels=1\nadd=6 levels=1\nadd=7 levels=1\nadd=8 levels=1\nadd=9 levels=1\n"
                                      "add=10 levels=1\nadd=11 levels=1\nadd=12 levels=1\nadd=13 levels=1\nadd=14 levels=1\nadd=15 levels=1\n"
                                      "add=16 levels=1\nadd=17 levels=1\n"}}),
    };
    data::TradeEntry seven{7, 0x80, 6}, nine{9, 0x80, 16};
    std::vector<std::vector<data::TradeEntry>> posts(32);
    posts[2] = {seven, nine};
    data::TableRows rows;
    rows.build(mods, {{"trade", 32}, {"waa", 256}, {"el_shop", 32}});
    rows.apply_trade(posts);
    CHECK(posts.size() == 33);
    const auto& post = posts[2];
    CHECK(post.size() == 2 && post[0].goods == 7 && post[0].levels == 99 && post[0].colour == 8);
    CHECK(post.size() == 2 && post[1].goods == 256 && post[1].colour == 0x10 && post[1].levels == 12);
    CHECK(logged(rows, "a new trade entry needs levels="));
    CHECK(posts[32].size() == 3 && posts[32][2].goods == 12);
    CHECK(posts[3].size() == 16 && logged(rows, "the post already has 16 entries"));
    std::vector<std::vector<u16>> shops(32);
    rows.apply_lists("el_shop", shops);
    CHECK(shops[1].empty() && logged(rows, "a shop entry takes no words after it"));
    std::vector<std::string> problems;
    data::parse_rows("[1]\nadd=5 levels\nremove=5 colour=1\nadd=6 levels=4294967297\n", {}, &problems, true);
    CHECK(problems.size() == 3);
  }

  // Key items: no table, a mod's [KeyItems] entry adds the name (ids from 65, mod by mod in priority order, own id order); two mods
  // naming one key item share it; the game's 1..64 pass through.
  {
    data::TableMod first = mod_with("First", {});
    first.own_ids["keyitem"] = {{70, "sky_key"}, {65, "moon_key"}};
    data::TableMod second = mod_with("Second", {});
    second.own_ids["keyitem"] = {{65, "sky_key"}, {66, "sun_key"}};
    data::TableRows rows;
    rows.build({first, second}, {{"keyitem", 65}});
    CHECK(data::table_declared_by_ids("keyitem") && !data::table_declared_by_ids("waa"));
    CHECK(rows.id_of("keyitem", "moon_key") == 65 && rows.id_of("keyitem", "sky_key") == 66 && rows.id_of("keyitem", "sun_key") == 67);
    CHECK(rows.total("keyitem", 65) == 68);
    CHECK(rows.key_item_for("First", 70) == 66 && rows.key_item_for("Second", 65) == 66 && rows.key_item_for("Second", 66) == 67);
    CHECK(!rows.key_item_for("First", 66) && rows.key_item_for("", 12) == 12);
    CHECK(rows.own_record_id("Second", "keyitem", 66) == 65);
    CHECK(!logged(rows, "no mod adds that record"));
  }

  // Characters by name: char's names hold for every set; equips_like and equip_add / equip_remove write no record byte, the loaders
  // read them (character_writes); an added character equips what its equips_like (else its base) does, unless a row names it.
  {
    const std::vector<data::TableMod> mods = {
        mod_with("Cast", {{"char", "[alice]\nbase=1\n[bob]\nbase=2\nequips_like=5\n"},
                          {"char20", "[alice]\natk=9\n"},
                          {"menu_waa", "[3]\nequip_add=@char:alice\n[1]\nequip_remove=@char:alice\n[4]\nequip_add=7\n"}},
                 {}, {}),
    };
    data::TableRows rows;
    rows.build(mods, {{"char", 51}, {"waa", 6}});
    CHECK(rows.id_of("char", "alice") == 51 && rows.id_of("char20", "bob") == 52 && rows.total("char_w2", 51) == 53);
    CHECK(rows.base_of("char", 52) == 2 && rows.base_of("char", 51) == 1 && !rows.base_of("char", 3));
    std::vector<Stat> chars(53);
    rows.apply("char", chars.size(), [&](size_t i) { return std::span<u8>(chars[i]); });
    CHECK((rows.character_writes("char").size() == 1 && rows.character_writes("char")[0].record == 52 &&
           rows.character_writes("char")[0].value == 5));
    std::vector<std::array<u8, 12>> info(6);
    rows.apply("menu_waa", info.size(), [&](size_t i) { return std::span<u8>(info[i]); });
    CHECK((rows.character_writes("menu_waa").size() == 3 && info[3] == std::array<u8, 12>{}));
    data::GameTables tables;
    tables.equips_like.resize(53);
    for (int i = 0; i < 51; ++i) tables.equips_like[static_cast<size_t>(i)] = i;
    tables.equips_like[51] = 1;
    tables.equips_like[52] = 5;
    // Item r's own bits: character r only.
    std::vector<data::Eligibility> who(6);
    std::vector<data::Eligibility*> pointers;
    for (data::Eligibility& entry : who) pointers.push_back(&entry);
    tables.fill_eligibility(rows, "menu_waa", pointers, [&](size_t row, int character) {
      return who[row].rule(character).value_or(static_cast<int>(row) == character);
    });
    CHECK(who[3].rule(51) == true && who[1].rule(51) == false && who[4].rule(7) == true);
    CHECK(who[5].rule(52) == true && !who[5].rule(51) && who[2].rule(52) != true && !who[0].rule(51));
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

  // An added monster whose id is the empty slot's (255) cannot stand in a formation slot: a problem, the slot keeps its value.
  {
    data::TableRows rows;
    rows.build({mod_with("Edge", {{"monster", "[edge]\nmax_hp=1\n"}, {"scenedat", "[0]\nenemy_2=@monster:edge\n"}})},
               {{"monster", 255}, {"scenedat", 1}});
    std::vector<std::array<u8, 28>> scenes(1);
    scenes[0][2] = 9;
    rows.apply("scenedat", scenes.size(), [&](size_t i) { return std::span<u8>(scenes[i]); });
    CHECK(scenes[0][2] == 9 && rows.wide_writes("scenedat").empty() && logged(rows, "monster:edge is record 255, the empty formation slot"));
  }

  // Element ids end at kLastElementId (a grid cell's 14 bits): a name past it gets no record; other tables have no such end.
  {
    data::TableRows rows;
    rows.build({mod_with("Many", {{"pgdat", "[last_one]\npower=1\n[one_more]\npower=1\n"}, {"monster", "[big]\nmax_hp=1\n"}})},
               {{"pgdat", data::kLastElementId}, {"monster", data::kLastElementId + 1}});
    CHECK(rows.id_of("pgdat", "last_one") == data::kLastElementId && !rows.id_of("pgdat", "one_more"));
    CHECK(rows.id_of("menu_pg", "last_one") == data::kLastElementId);
    CHECK(rows.total("pgdat", data::kLastElementId) == data::kLastElementId + 1);
    CHECK(logged(rows, "no id left for one_more") && rows.id_of("monster", "big") == data::kLastElementId + 1);
  }
  // Item ids end at kLastItemId (an equipment word past it is an element or a key item).
  {
    data::TableRows rows;
    rows.build({mod_with("Gear", {{"waa", "[last_item]\npower=1\n[one_more]\npower=1\n"}})}, {{"waa", data::kLastItemId}});
    CHECK(rows.id_of("waa", "last_item") == data::kLastItemId && !rows.id_of("waa", "one_more"));
    CHECK(rows.id_of("menu_waa", "last_item") == data::kLastItemId);
    CHECK(logged(rows, "no id left for one_more"));
  }

  if (failures == 0) std::printf("table_rows: OK\n");
  return failures == 0 ? 0 : 1;
}
