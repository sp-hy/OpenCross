// Checks for the weapon a party member shows (game::pc_weapon, game::own_weapon_bin): an item mods add by name whose mod adds
// battle/wepon/<name>.bin shows that pack's first part; any other item the wearer's body pack at the item's tier; no gear record, no
// weapon; rows set for another VFS give no item of its own.
#include <cstdio>
#include <optional>
#include <string>

#include "data/table_rows.h"
#include "data/tables.h"
#include "game/battle/model_table.h"
#include "game/model_files.h"
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

data::TableMod gear_mod() {
  data::TableMod mod{"Gear", {}, {}};
  mod.files["waa"] = [] {
    const std::string text = "[my_blade]\nbase=1\n[plain_blade]\nbase=1\n";
    return std::optional<Bytes>(Bytes(text.begin(), text.end()));
  };
  return mod;
}
}  // namespace

int main() {
  vfs::Vfs files, other;
  vfs::Overlay overlay;
  overlay.files["cdrom/battle/wepon/my_blade.bin"] = {"my_blade.bin", ""};
  files.set_overlay(overlay);
  data::TableRows rows;
  rows.build({gear_mod()}, {{"waa", data::kGameItems}});
  const std::optional<size_t> my_blade = rows.id_of("waa", "my_blade"), plain_blade = rows.id_of("waa", "plain_blade");
  CHECK(my_blade && plain_blade);
  data::set_table_rows(std::move(rows));
  data::set_table_mods(&files, {gear_mod()});

  data::GameTables tables;
  tables.gear.resize(data::kGameItems + 2);
  tables.gear[1].raw[0] = 3;
  tables.gear[*my_blade].raw[0] = 3;
  tables.gear[*plain_blade].raw[0] = 3;
  const int serge = 0;
  const std::string body_pack(game::kBattleWeaponBins[serge]);

  // Its own pack: part 0, whoever wears it.
  CHECK(game::own_weapon_bin(files, static_cast<int>(*my_blade)) == "battle/wepon/my_blade.bin");
  const game::WeaponPick own = game::pc_weapon(files, tables, static_cast<int>(*my_blade), serge);
  CHECK(own.bin == "battle/wepon/my_blade.bin" && own.part == 0);
  // No pack of its own, or a game item: the body's pack at the tier.
  const game::WeaponPick plain = game::pc_weapon(files, tables, static_cast<int>(*plain_blade), serge);
  CHECK(game::own_weapon_bin(files, static_cast<int>(*plain_blade)).empty() && plain.bin == body_pack && plain.part == 3);
  const game::WeaponPick game_item = game::pc_weapon(files, tables, 1, serge);
  CHECK(game_item.bin == body_pack && game_item.part == 3);
  // No gear record: no weapon.
  CHECK(game::pc_weapon(files, tables, data::kGameItems + 5, serge).part == -1);
  // Rows set for another VFS (the background scan's unmodified game): no item of its own there.
  CHECK(game::own_weapon_bin(other, static_cast<int>(*my_blade)).empty());

  data::set_table_mods(nullptr, {});
  data::set_table_rows({});
  if (failures == 0) std::printf("weapon_pick: OK\n");
  return failures == 0 ? 0 : 1;
}
