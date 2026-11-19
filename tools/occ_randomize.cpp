// occ randomize <out.txt> [--seed N] [--spread N] [--no-items] [--no-elements] [--enemies] [--start-gear] [--start-elements] [--entries]: the
// randomizer's plan (game/randomizer.h) for those settings, written as the spoiler log the game writes, and the moved counts per pool on
// stdout; --entries appends every pool entry (value, stage) and the areas' means. The same arguments give the same file on every machine
// with the same data.
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "data/tables.h"
#include "game/randomizer.h"
#include "game/room_scan.h"
#include "ui/menu/game_state.h"
#include "ui/shop/shop_logic.h"
#include "vfs/vfs.h"

using namespace oc;

int occ_randomize(const vfs::Vfs& fs, const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::fprintf(stderr, "usage: occ randomize <out.txt> [--seed N] [--spread N] [--no-items] [--no-elements] [--enemies] [--start-gear] [--start-elements] [--entries]\n");
    return 2;
  }
  game::randomizer::Settings settings;
  settings.items = settings.elements = true;
  bool entries = false;
  for (size_t i = 2; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--seed" && i + 1 < args.size()) settings.seed = static_cast<u32>(std::strtoul(args[++i].c_str(), nullptr, 10));
    else if (arg == "--spread" && i + 1 < args.size()) settings.spread = std::atoi(args[++i].c_str());
    else if (arg == "--no-items") settings.items = false;
    else if (arg == "--no-elements") settings.elements = false;
    else if (arg == "--enemies") settings.enemies = true;
    else if (arg == "--start-gear") settings.start_gear = true;
    else if (arg == "--start-elements") settings.start_elements = true;
    else if (arg == "--entries") entries = true;
    else {
      std::fprintf(stderr, "randomize: unknown argument '%s'\n", arg.c_str());
      return 2;
    }
  }
  data::GameTables tables;
  std::string error;
  if (!tables.load(fs, &error)) {
    std::fprintf(stderr, "randomize: %s\n", error.c_str());
    return 1;
  }
  ui::MenuRules rules;
  rules.load_pg(fs);
  ui::shop::ShopData smiths = ui::shop::ShopData::from_tables(tables);
  smiths.load_waa_shop(fs);
  const game::randomizer::Pools pools = game::randomizer::gather(game::scan_rooms(fs), tables, &rules, smiths.waa_shop);
  const game::randomizer::Plan plan = game::randomizer::build(settings, pools);
  std::ofstream out(args[1], std::ios::binary);
  out << plan.spoiler();
  if (entries) {
    for (size_t pool = 0; pool < game::randomizer::kPools; ++pool) {
      out << "\n== entries: " << game::randomizer::pool_name(static_cast<game::randomizer::Pool>(pool)) << " (site slot thing value stage where) ==\n";
      for (const game::randomizer::Entry& entry : pools.entries[pool])
        out << std::format("{} {} {} {} {} {}\n", entry.site, entry.slot, pools.thing_name(entry.thing), entry.value, entry.stage, entry.where);
    }
    out << "\n== areas (key hp power gold stage) ==\n";
    for (const auto& [key, area] : pools.areas) out << std::format("{} {} {} {} {} {}\n", key, area.hp, area.power, area.gold, area.stage, area.name);
  }
  if (!out) {
    std::fprintf(stderr, "randomize: cannot write %s\n", args[1].c_str());
    return 1;
  }
  for (size_t pool = 0; pool < game::randomizer::kPools; ++pool) {
    const auto [count, moved] = plan.counts()[pool];
    std::printf("%s: %d entries, %d moved\n", game::randomizer::pool_name(static_cast<game::randomizer::Pool>(pool)), count, moved);
  }
  std::printf("spoiler log: %s\n", args[1].c_str());
  return 0;
}
