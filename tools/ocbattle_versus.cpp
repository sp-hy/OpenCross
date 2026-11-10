// ocbattle versus - a versus battle played headless twice in lockstep, as two players' games would: one Battle built from the sides as
// given, the other from the sides after the exchange format (encode_versus_side / decode_versus_side), both fed the same command stream;
// the state hashes are compared after every command and the first divergence is reported.
//   ocbattle versus <seed> [--a save.dat:slot] [--b save.dat:slot] [--cmds FILE | --auto N] [--fuzz SEEDS] [--cap N] [-v]
// Without a save a side is a built-in test party. --cmds reads one command per line ('#' starts a comment):
//   attack <unit> <s[,s...]> [target] | element <unit> <cell> [target] | defend <unit> | run <unit> | end <unit> [timeout]
//   focus <unit> | menu <unit> | form <unit> <form>
// --auto N plays random legal commands from generator seed N (the default); --fuzz SEEDS plays battle seeds 1..SEEDS that way and prints
// the outcome counts and a digest of the final hashes (equal on every build when the rules are deterministic).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "battle/battle.h"
#include "battle/versus.h"
#include "core/hash.h"
#include "data/tables.h"
#include "game/game_session.h"
#include "save/cc_save.h"
#include "ui/menu/game_state.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::battle;

namespace {

constexpr int kDefaultCap = 4000;
// The built-in sides: three characters each at this star (versus_test_side).
constexpr int kTestStar = 20;
const std::vector<int> kTestPartyA = {0, 1, 2}, kTestPartyB = {3, 4, 26};

// "file[:slot]": the party, summon stars and creature pool of that save slot (the newest when no slot is given). Read-only.
bool load_side(const data::GameTables& tables, const vfs::Vfs& vfs, std::string spec, VersusSide& out) {
  int want = -1;
  const size_t colon = spec.rfind(':');
  if (colon != std::string::npos && colon + 1 < spec.size() && spec.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
    want = std::atoi(spec.c_str() + colon + 1);
    spec.resize(colon);
  }
  try {
    const save::SaveDat save_dat = save::SaveDat::load(spec);
    for (int card_no = 1; card_no <= 2; ++card_no) {
      const save::MemCard* card = save_dat.card(card_no);
      if (!card) continue;
      for (const save::SlotRef& slot_ref : save::list_slots(*card)) {
        if (want >= 0 ? slot_ref.slot != want : slot_ref.slot == 0) continue;
        ui::MenuRules rules;
        rules.tables = &tables;
        rules.load_pg(vfs);
        const ui::GameState state(save::read_slot(*card, slot_ref.block), &rules);
        std::array<u16, 16> dopple{};
        for (size_t i = 0; i < dopple.size(); ++i) dopple[i] = static_cast<u16>(state.slot.var(static_cast<int>(0x3E0 + 2 * i)));
        out = game::versus_side(tables, state, state.party(), dopple);
        return !out.party.empty();
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "versus: %s\n", error.what());
  }
  return false;
}

std::optional<Command> parse_command(const std::string& line) {
  std::istringstream words(line);
  std::string verb;
  Command cmd;
  if (!(words >> verb >> cmd.actor)) return std::nullopt;
  if (verb == "attack") {
    std::string chain;
    if (!(words >> chain)) return std::nullopt;
    std::istringstream strengths(chain);
    for (std::string strength; std::getline(strengths, strength, ',');) cmd.chain.push_back(std::atoi(strength.c_str()));
    cmd.kind = Command::Kind::Attack;
    words >> cmd.target;
  } else if (verb == "element") {
    if (!(words >> cmd.cell)) return std::nullopt;
    cmd.kind = Command::Kind::Element;
    words >> cmd.target;
  } else if (verb == "defend") {
    cmd.kind = Command::Kind::Defend;
  } else if (verb == "run") {
    cmd.kind = Command::Kind::Run;
  } else if (verb == "end") {
    std::string flag;
    cmd.kind = Command::Kind::EndTurn;
    cmd.timeout = (words >> flag) && flag == "timeout";
  } else if (verb == "focus") {
    cmd.kind = Command::Kind::Focus;
  } else if (verb == "menu") {
    cmd.kind = Command::Kind::ElementMenu;
  } else if (verb == "form") {
    if (!(words >> cmd.form)) return std::nullopt;
    cmd.kind = Command::Kind::Form;
  } else if (verb == "engage") {
    if (!(words >> cmd.target)) return std::nullopt;
    cmd.kind = Command::Kind::Engage;
  } else {
    return std::nullopt;
  }
  return cmd;
}

std::string describe(const Command& cmd) {
  switch (cmd.kind) {
    case Command::Kind::Attack: {
      std::string chain;
      for (const int strength : cmd.chain) chain += (chain.empty() ? "" : ",") + std::to_string(strength);
      return std::format("attack {} {} {}", cmd.actor, chain, cmd.target);
    }
    case Command::Kind::Element: return std::format("element {} {} {}", cmd.actor, cmd.cell, cmd.target);
    case Command::Kind::Defend: return std::format("defend {}", cmd.actor);
    case Command::Kind::Run: return std::format("run {}", cmd.actor);
    case Command::Kind::Swap: return std::format("swap {} {}", cmd.actor, cmd.swap_in);
    case Command::Kind::EndTurn: return std::format("end {}{}", cmd.actor, cmd.timeout ? " timeout" : "");
    case Command::Kind::Focus: return std::format("focus {}", cmd.actor);
    case Command::Kind::ElementMenu: return std::format("menu {}", cmd.actor);
    case Command::Kind::Form: return std::format("form {} {}", cmd.actor, cmd.form);
    case Command::Kind::Engage: return std::format("engage {} {}", cmd.actor, cmd.target);
  }
  return "?";
}

struct Run {
  Outcome outcome = Outcome::Ongoing;
  VersusEnd end = VersusEnd::None;
  int commands = 0, rejected = 0, turns = 0;
  // The most commands one turn took (Focus and the element menu included).
  int longest_turn = 0;
  // The command index after which the two games first differed, -1 = never (-2 = right after the start).
  int divergence = -1;
  std::string diverged_on;
  u64 final_hash = 0;
};

void print_units(const char* label, const Battle& battle) {
  std::printf("  %s:", label);
  for (int unit = 0; unit < kPlayerUnits; ++unit) {
    const Unit& state = battle.unit(unit);
    if (state.present) std::printf(" [%d] hp %d st %d g %d lo %08X", unit, state.hp, state.stamina, state.gauge, state.lo);
  }
  std::printf("\n");
}

// Plays one battle in both games. `stream` (when not empty) is the whole command list; otherwise `player` decides until the battle ends or
// `cap` commands have been applied.
Run play(const data::GameTables& tables, const VersusSide& side_a, const VersusSide& side_b, u32 seed, const std::vector<Command>& stream,
         VersusAutoPlayer* player, int cap, bool verbose) {
  Run run;
  std::optional<VersusSide> sent_a = decode_versus_side(encode_versus_side(side_a)), sent_b = decode_versus_side(encode_versus_side(side_b));
  if (!sent_a || !sent_b) {
    run.divergence = -2;
    run.diverged_on = "the exchange format did not decode";
    return run;
  }
  GameRng rng_one(seed, tables.rng_table), rng_two(seed, tables.rng_table);
  VersusBrain brain_one, brain_two;
  Battle one(tables, rng_one, brain_one), two(tables, rng_two, brain_two);
  const int field = versus_battlefield(tables, seed);
  if (!one.start_versus(side_a, side_b, field) || !two.start_versus(*sent_a, *sent_b, field)) {
    run.divergence = -2;
    run.diverged_on = "a side cannot fight";
    return run;
  }
  if (one.state_hash() != two.state_hash()) {
    run.divergence = -2;
    run.diverged_on = "start";
  }
  size_t next = 0;
  int turn_commands = 0;
  std::vector<Command> pending;
  while (run.divergence == -1 && one.outcome() == Outcome::Ongoing && run.commands < cap) {
    if (pending.empty()) {
      if (player) pending = player->next(one);
      else if (next < stream.size()) pending.push_back(stream[next++]);
      else break;
    }
    const Command cmd = pending.front();
    pending.erase(pending.begin());
    const int side_before = one.turn_side();
    std::string why;
    const bool ok_one = one.apply(cmd, &why), ok_two = two.apply(cmd);
    ++run.commands;
    if (!ok_one) ++run.rejected;
    run.longest_turn = std::max(run.longest_turn, ++turn_commands);
    if (one.turn_side() != side_before) ++run.turns, turn_commands = 0;
    if (verbose)
      std::printf("[%d] %-22s %s  side %c  %016llx\n", run.commands, describe(cmd).c_str(), ok_one ? "ok" : ("rejected: " + why).c_str(),
                  one.turn_side() == 0 ? 'A' : 'B', static_cast<unsigned long long>(one.state_hash()));
    for (Battle* game : {&one, &two}) {
      game->drain_events();
      game->drain_log();
    }
    if (ok_one != ok_two || one.state_hash() != two.state_hash()) {
      run.divergence = run.commands;
      run.diverged_on = describe(cmd);
    }
  }
  if (run.divergence != -1) {
    print_units("game 1", one);
    print_units("game 2", two);
  }
  run.outcome = one.outcome();
  run.end = one.versus_end();
  run.final_hash = one.state_hash();
  return run;
}

const char* end_name(VersusEnd end) {
  switch (end) {
    case VersusEnd::None: return "none";
    case VersusEnd::Knockout: return "knockout";
    case VersusEnd::Draw: return "draw";
    case VersusEnd::Surrender: return "surrender";
    case VersusEnd::Timeouts: return "timeouts";
    case VersusEnd::Conceded: return "conceded";
    case VersusEnd::Stalled: return "stalled";
  }
  return "?";
}

const char* outcome_name(Outcome outcome) {
  return outcome == Outcome::Victory ? "side A wins" : outcome == Outcome::Wiped ? "side B wins" : outcome == Outcome::Draw ? "draw" : "still on";
}

void print_side(const char* label, const VersusSide& side) {
  std::printf("side %s (stars %d, %zu bytes exchanged):", label, side.stars, encode_versus_side(side).size());
  for (const PartyMember& member : side.party) std::printf(" %d %s %d HP;", member.char_id, member.name.c_str(), member.stat[0]);
  std::printf("\n");
}

}  // namespace

int cmd_versus(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "usage: ocbattle versus <seed> [--a save.dat:slot] [--b save.dat:slot] [--cmds FILE | --auto N] [--fuzz SEEDS] [--cap N] [-v]\n");
    return 2;
  }
  const u32 seed = static_cast<u32>(std::strtoul(args[0].c_str(), nullptr, 0));
  VersusSide side_a = versus_test_side(tables, kTestPartyA, kTestStar), side_b = versus_test_side(tables, kTestPartyB, kTestStar);
  std::string cmds_path;
  u32 auto_seed = seed;
  int fuzz = 0, cap = kDefaultCap;
  bool verbose = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const bool has_value = i + 1 < args.size();
    if ((args[i] == "--a" || args[i] == "--b") && has_value) {
      VersusSide& side = args[i] == "--a" ? side_a : side_b;
      if (!load_side(tables, vfs, args[++i], side)) {
        std::fprintf(stderr, "versus: no party in %s\n", args[i].c_str());
        return 1;
      }
    } else if (args[i] == "--cmds" && has_value) {
      cmds_path = args[++i];
    } else if (args[i] == "--auto" && has_value) {
      auto_seed = static_cast<u32>(std::strtoul(args[++i].c_str(), nullptr, 0));
    } else if (args[i] == "--fuzz" && has_value) {
      fuzz = std::atoi(args[++i].c_str());
    } else if (args[i] == "--cap" && has_value) {
      cap = std::atoi(args[++i].c_str());
    } else if (args[i] == "-v") {
      verbose = true;
    }
  }
  print_side("A", side_a);
  print_side("B", side_b);
  if (fuzz > 0) {
    int wins_a = 0, wins_b = 0, draws = 0, open = 0, divergences = 0, longest_turn = 0;
    long long commands = 0, rejected = 0;
    std::map<std::string, int> ends;
    Fnv64 digest;
    for (int run_seed = 1; run_seed <= fuzz; ++run_seed) {
      VersusAutoPlayer player(static_cast<u32>(run_seed) * 7919u + 1u);
      const Run run = play(tables, side_a, side_b, static_cast<u32>(run_seed), {}, &player, cap, false);
      wins_a += run.outcome == Outcome::Victory, wins_b += run.outcome == Outcome::Wiped, draws += run.outcome == Outcome::Draw;
      open += run.outcome == Outcome::Ongoing;
      commands += run.commands, rejected += run.rejected;
      longest_turn = std::max(longest_turn, run.longest_turn);
      ++ends[end_name(run.end)];
      digest.add(run.final_hash);
      if (run.divergence != -1) {
        ++divergences;
        std::printf("seed %d: diverged after command %d (%s)\n", run_seed, run.divergence, run.diverged_on.c_str());
      }
    }
    std::printf("versus fuzz: %d battles, %lld commands (%lld rejected), %d divergences\n", fuzz, commands, rejected, divergences);
    std::printf("  %.1f commands per battle, longest turn %d commands\n", static_cast<double>(commands) / fuzz, longest_turn);
    std::printf("  outcomes: side A %d, side B %d, draw %d, still on %d | ends:", wins_a, wins_b, draws, open);
    for (const auto& [name, count] : ends) std::printf(" %s %d", name.c_str(), count);
    std::printf("\n  digest %s\n", digest.hex().c_str());
    return divergences == 0 ? 0 : 1;
  }
  std::vector<Command> stream;
  std::optional<VersusAutoPlayer> player;
  if (!cmds_path.empty()) {
    std::ifstream file(cmds_path);
    if (!file) {
      std::fprintf(stderr, "versus: cannot read %s\n", cmds_path.c_str());
      return 1;
    }
    int line_no = 0;
    for (std::string line; std::getline(file, line);) {
      ++line_no;
      line = line.substr(0, line.find('#'));
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
      const std::optional<Command> cmd = parse_command(line);
      if (!cmd) {
        std::fprintf(stderr, "versus: %s:%d: cannot read '%s'\n", cmds_path.c_str(), line_no, line.c_str());
        return 1;
      }
      stream.push_back(*cmd);
    }
  } else {
    player.emplace(auto_seed);
  }
  std::printf("battle seed %u, battlefield %d, data %s\n", seed, versus_battlefield(tables, seed), data::battle_data_fingerprint(tables).c_str());
  const Run run = play(tables, side_a, side_b, seed, stream, player ? &*player : nullptr, cap, verbose);
  std::printf("outcome: %s (%s) after %d commands (%d rejected), %d turn changes, final hash %016llx\n", outcome_name(run.outcome), end_name(run.end),
              run.commands, run.rejected, run.turns, static_cast<unsigned long long>(run.final_hash));
  if (run.divergence == -1) {
    std::printf("in step: no divergence\n");
    return 0;
  }
  std::printf("DIVERGED after command %d (%s)\n", run.divergence, run.diverged_on.c_str());
  return 1;
}
