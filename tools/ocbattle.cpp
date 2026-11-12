// ocbattle - battle tables validator, headless battle simulator, formula evaluator.
//   ocbattle tables                       counts and a few named rows (compare with tools/py/cc_tables.py)
//   ocbattle scenes | elements            one line per scene record (bg, boss, enemies) / per element record (name, colour, level, mode)
//   ocbattle sim <scene> [seed]           play one battle with the auto policy and print the log
//        [--party 0,1,2] [--star N] [--set char|char_w0..] [--quiet] [--brain scripted|random]  (default scripted)
//        [--party-from save.dat:slot]     party, stats and star level of a save slot
//        [--attack] [--field N]           weapon chains only (no element casts); battlefield N; ring changes print as `ring [..]`
//   ocbattle xcheck [--n N] [--seed N]    random attacker / target / attack triples: engine damage against an independent formula
//   ocbattle soak [--seeds N] [--scene S | --from A --to B] [--cap N] [--party-from save.dat:slot] [--brain scripted|random] [-v]
//                                         every scene's battle with the auto policy over N seeds (default 3), command cap per battle (default 2000)
//   ocbattle cast <scene> <actor> <element[,element...]> <target> [seeds]   cast elements headless, print the target's status words
//   ocbattle debug-kill [enemy_id [count]] [--scene S] [--keep-script]      the Debug tab's enemy replacement: scenes that do not end in victory
//   ocbattle pac-disasm <scene>           disassemble battle/event/evtNNN.pac (same text as cc_tables.py bscript dump)
//   ocbattle pac-soak [turns] [-v]        run every scene's scripts in a dummy battle for N commands (default 40)
//        [--scene N | --from A --to B]    only these scenes; OC_SOAK_LIST=1 prints each scene's outcome
//   ocbattle formula <name> <args...>     evaluate one formula; `ocbattle formula` lists them
//   ocbattle fx-audit [--csv F] [--md F]  play every battle effect pack headless, per-pack status (tools/ocbattle_fx.cpp)
//        [--rng lo|mid|hi|seed:N]         roll source for formulas with random terms (default mid)
//   ocbattle versus <seed> [--a S:N] [--b S:N] [--cmds F | --auto N] [--fuzz SEEDS] [--cap N] [-v]
//                                         a versus battle run twice in lockstep, state hashes compared after every command (tools/ocbattle_versus.cpp)
//   ocbattle versus-net <ws-url> [--fuzz N] [--cut K] [--timeout S] [-v]
//                                         versus battles between two clients over a running multiplayer server (tools/ocbattle_versus_net.cpp)
// GameDir comes from -g <dir>, OPENCROSS_GAMEDIR, or platform::find_game_dir.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "battle/battle.h"
#include "battle/pac_vm.h"
#include "data/tables.h"
#include "game/disc_index.h"
#include "game/game_session.h"
#include "platform/paths.h"
#include "save/cc_save.h"
#include "ui/menu/game_state.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::battle;

// tools/ocbattle_fx.cpp
int cmd_fx_audit(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args, const std::map<int, int>& enemy_refs);

// Defined in tools/ocbattle_reg.cpp.
int cmd_fx_reg(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args);

// Defined in tools/ocbattle_versus.cpp.
int cmd_versus(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args);

// Defined in tools/ocbattle_versus_net.cpp.
int cmd_versus_net(const data::GameTables& tables, const std::vector<std::string>& args);

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: ocbattle [-g <gamedir>] tables\n"
               "       ocbattle [-g <gamedir>] scenes\n"
               "       ocbattle [-g <gamedir>] sim <scene> [seed] [--party 0,1,2] [--star N] [--set name] [--quiet] [--brain scripted|random] [--party-from save.dat:slot] [--attack] [--field N] [--poison]\n"
               "       ocbattle [-g <gamedir>] xcheck [--n N] [--seed N]\n"
               "       ocbattle [-g <gamedir>] soak [--seeds N] [--scene S | --from A --to B] [--cap N] [--party-from save.dat:slot] [--brain scripted|random] [-v]\n"
               "       ocbattle [-g <gamedir>] pac-disasm <scene>\n"
               "       ocbattle [-g <gamedir>] pac-soak [turns] [--scene S | --from A --to B] [-v]\n"
               "       ocbattle [-g <gamedir>] elements\n"
               "       ocbattle [-g <gamedir>] cast <scene> <actor> <element[,element...]> <target> [seeds]\n"
               "       ocbattle [-g <gamedir>] debug-kill [enemy_id [count]] [--scene S] [--keep-script]\n"
               "       ocbattle [-g <gamedir>] fx-reg (SCENE [--bg N] | --index [--all] | --file PATH | --refs NAME | --s6 | --grep NAME)\n"
               "       ocbattle [-g <gamedir>] fx-audit [--csv FILE] [--md FILE] [--eid N] [--ticks N] [-v] [--find NAME] [--rng lo|mid|hi|seed:N]\n"
               "       ocbattle [-g <gamedir>] versus <seed> [--a save.dat:slot] [--b save.dat:slot] [--cmds FILE | --auto N] [--fuzz SEEDS] [--cap N] [-v]\n"
               "       ocbattle [-g <gamedir>] versus-net <ws-url> [--fuzz N] [--cut K] [--timeout S] [-v]\n"
               "       ocbattle formula <name> <args...> [--rng lo|mid|hi|seed:N]\n");
  return 2;
}

// ------------------------------------------------------------------------------------------------ scenes
// `ocbattle scenes`: one line per scene (enemy ids / names / counts, battlefield bg, boss flag) to look a retail battle up by its enemies.
int cmd_scenes(const data::GameTables& tables) {
  for (size_t i = 0; i < tables.scenes.size(); ++i) {
    const auto& sc = tables.scenes[i];
    std::printf("scene %zu bg=%d%s%s:", i, sc.bg(), sc.boss() ? " boss" : "", sc.raw[0x1B] ? (" offstage=" + std::to_string(sc.raw[0x1B])).c_str() : "");
    for (int k = 0; k < 3; ++k)
      if (sc.enemy_count(k) && sc.enemy_id(k) != 0xFF) std::printf(" %dx[%d]%s", sc.enemy_count(k), sc.enemy_id(k), tables.enemy_name(sc.enemy_id(k)).c_str());
    std::putchar(10);
  }
  return 0;
}

// ------------------------------------------------------------------------------------------------ tables
int cmd_tables(const data::GameTables& tables) {
  auto nonempty = [](const auto& rows) {
    size_t count = 0;
    for (const auto& row : rows)
      if (!row.empty()) ++count;
    return count;
  };
  size_t el_ne = 0;
  for (const auto& element : tables.elements)
    if (!element.empty()) ++el_ne;
  std::printf("enemies: %zu\n", tables.enemies.size());
  std::printf("char sets:");
  for (const auto& [set_name, records] : tables.char_sets) std::printf(" %s", set_name.c_str());
  std::printf("  (%d records each)\n", data::kNumChars);
  std::printf("elements: %zu (non-empty %zu)\n", tables.elements.size(), el_ne);
  std::printf("growth rows: %zu\n", tables.growth.size());
  std::printf("scenes: %zu\n", tables.scenes.size());
  std::printf("symtbl: %zu entries (%zu x 8)\n", tables.symtbl.size(), tables.symtbl.size() / 8);
  std::printf("btbgdat: %zu\n", tables.btbgdat.size());
  std::printf("drops: %zu\n", tables.drops.size());
  std::printf("star rows: %zu\n", tables.star_rows.size());
  std::printf("el_shop non-empty rows: %zu\n", nonempty(tables.el_shop));
  std::printf("waa_shop non-empty rows: %zu\n", nonempty(tables.waa_shop));
  std::printf("trade non-empty rows: %zu\n", nonempty(tables.trade));
  size_t kn = 0;
  for (size_t i = 1; i < tables.key_item_names.size(); ++i)
    if (!tables.key_item_names[i].empty()) ++kn;
  std::printf("key items named: %zu (keyitem.dat offsets %u)\n", kn, tables.keyitem_jp_rows);
  std::printf("rng table: %zu bytes\n", tables.rng_table.size());
  std::printf("named rows (ASCII names only):\n");
  auto ascii = [](const std::string& text) {
    std::string out;
    for (unsigned char ch : text) out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    return out;
  };
  const auto& bb = tables.enemies[1];
  std::printf("  enemy[1] %s hp=%d atk=%d def=%d hit=%d evd=%d mag=%d mdef=%d gold=%d drop=%d innate=%s\n",
              ascii(tables.enemy_name(1)).c_str(), bb.max_hp(), bb.atk(), bb.def(), bb.hit(), bb.evd(), bb.mag(), bb.mdef(), bb.gold(),
              bb.drop_row(), data::colour_name(data::colour_index_of_bit(bb.innate())));
  const auto& aq = tables.elements[1];
  std::printf("  element[1] %s level=%d mode=%d power=%d variance=%d colour=%s\n", ascii(tables.element_name(1)).c_str(), aq.level(),
              aq.mode(), aq.power(), aq.variance(), data::colour_name(data::colour_index_of_bit(aq.colour_bit())));
  const auto& cure = tables.elements[85];
  std::printf("  element[85] %s mode=%d status_pct=%d\n", ascii(tables.element_name(85)).c_str(), cure.mode(), cure.status_pct());
  const auto& sg = tables.chars().rec[0];
  const auto& sl = tables.chars().lv99[0];
  std::printf("  char[0] %s hp=%d atk=%d def=%d hit=%d evd=%d mag=%d mdef=%d weapon=0x%X armour=0x%X growth_col=%d lv99=%d/%d/%d/%d/%d/%d/%d\n",
              ascii(tables.char_name(0)).c_str(), sg.max_hp(), sg.atk(), sg.def(), sg.hit(), sg.evd(), sg.mag(), sg.mdef(), sg.weapon(),
              sg.armour(), sg.growth_col(), sl.hp, sl.atk, sl.def, sl.hit, sl.evd, sl.mag, sl.mdef);
  if (!tables.gear.empty()) {
    for (int id : {0x20, 0x96, 0xC8, 0xDE}) {
      const auto* gear = tables.gear_rec(id);
      const auto* ii = tables.item(id);
      std::printf("  gear[0x%02X] %s power=%d mag=%d acc=%d var=%d class=%d flags=0x%X price=%d cat=0x%02X serge=%d\n", id,
                  ascii(tables.item_name(id)).c_str(), gear->power(), gear->mag(), gear->acc(), gear->var(), gear->klass(), gear->flags(),
                  ii ? ii->price() : 0, ii ? ii->cat : 0, ii && ii->can_equip(0) ? 1 : 0);
    }
    const auto eq = battle::make_equip(tables, sg.weapon(), sg.armour(), {sg.accessory(0), sg.accessory(1), sg.accessory(2)});
    std::printf("  serge gear: weapon %s armour %s acc 0x%X,0x%X,0x%X -> wp=%d acc=%d var=%d arm=%d armvar=%d mdef+%d evd+%d\n",
                ascii(tables.item_name(sg.weapon())).c_str(), ascii(tables.item_name(sg.armour())).c_str(), sg.accessory(0), sg.accessory(1),
                sg.accessory(2), eq.wp, eq.wp_acc, eq.wp_var, eq.arm, eq.arm_var, eq.mdef_gear, eq.evd_gear);
  } else {
    std::printf("  gear table missing\n");
  }
  std::printf("  growth[1][0]=0x%02X growth[2][0]=0x%02X growth[3][0]=0x%02X\n", tables.growth[1][0], tables.growth[2][0], tables.growth[3][0]);
  const auto& sc = tables.scenes[0];
  std::printf("  scene[0] ids=%d,%d,%d counts=%d,%d,%d boss=%d\n", sc.enemy_id(0), sc.enemy_id(1), sc.enemy_id(2), sc.enemy_count(0),
              sc.enemy_count(1), sc.enemy_count(2), sc.boss() ? 1 : 0);
  std::printf("  drops[1] = %04X %04X %04X %04X\n", tables.drops[1].slot[0], tables.drops[1].slot[1], tables.drops[1].slot[2], tables.drops[1].slot[3]);
  std::printf("  symtbl[8..15] =");
  for (int i = 8; i < 16; ++i) std::printf(" %u", tables.symtbl[(size_t)i]);
  std::printf("\n");
  std::printf("  el_shop[0] =");
  for (u16 entry : tables.el_shop[0]) std::printf(" %u", entry);
  std::printf("\n");
  return 0;
}

// ------------------------------------------------------------------------------------------------ sim
std::vector<int> parse_ids(const std::string& text) {
  std::vector<int> ids;
  std::stringstream stream(text);
  std::string tok;
  while (std::getline(stream, tok, ',')) ids.push_back(std::atoi(tok.c_str()));
  return ids;
}

std::string ascii(const std::string& text) {
  std::string out;
  for (unsigned char ch : text) out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
  return out;
}

// The scene's battle script, found through the disc index as the game does.
std::optional<Bytes> read_pac(const vfs::Vfs& vfs, int scene, std::string* name = nullptr) {
  static const game::DiscIndex discs = [&vfs] {
    game::DiscIndex index;
    index.load(vfs);
    return index;
  }();
  const std::string path = discs.battle_script(1, scene);
  if (name) *name = path.substr(path.rfind('/') + 1);
  return vfs.read(path);
}

pac::ScriptedBrain::Loader pac_loader(const vfs::Vfs& vfs) {
  return [&vfs](int scene) { return read_pac(vfs, scene); };
}

// ------------------------------------------------------------------------------------------------ save party
// "file[:slot]" -> the party of that save slot (CHRONOnn suffix; default newest = highest slot number found). Read-only.
struct SavedParty {
  std::vector<battle::PartyMember> party;
  int star = 0, slot = -1;
};

bool load_party_from(const data::GameTables& tables, const vfs::Vfs& vfs, std::string spec, SavedParty& out) {
  int want = -1;
  const size_t colon = spec.rfind(':');
  if (colon != std::string::npos && colon + 1 < spec.size() &&
      spec.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
    want = std::atoi(spec.c_str() + colon + 1);
    spec.resize(colon);
  }
  try {
    const save::SaveDat sd = save::SaveDat::load(spec);
    for (int card_no = 1; card_no <= 2; ++card_no) {
      const save::MemCard* card = sd.card(card_no);
      if (!card) continue;
      for (const save::SlotRef& slot_ref : save::list_slots(*card)) {
        if (want >= 0 && slot_ref.slot != want) continue;
        if (want < 0 && slot_ref.slot == 0) continue;
        ui::MenuRules rules;
        rules.tables = &tables;
        rules.load_pg(vfs);
        ui::GameState gs(save::read_slot(*card, slot_ref.block), &rules);
        out.slot = slot_ref.slot;
        out.star = game::session_star(gs);
        out.party.clear();
        for (int id : gs.party()) out.party.push_back(game::party_member_from_state(tables, gs, id));
        return !out.party.empty();
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "party-from: %s\n", error.what());
  }
  return false;
}

void print_party(const std::vector<battle::PartyMember>& party, int star) {
  std::printf("party from save (star %d):\n", star);
  for (const auto& member : party) {
    const auto& equip = member.eq;
    int unl = 0, filled = 0;
    for (size_t cell = 0; cell < member.cells.size(); ++cell) unl += member.unlocked[cell], filled += (member.cells[cell] & 0x3FFF) != 0;
    std::printf("  [%d] %s hp=%d/%d atk=%d def=%d hit=%d evd=%d mag=%d mdef=%d | gear wp=%d acc=%d var=%d arm=%d armvar=%d mdef+%d evd+%d | grid %d filled %d unlocked | snap %d,%d,%d pending %d,%d,%d\n",
                member.char_id, ascii(member.name).c_str(), member.hp, member.stat[0], member.stat[1], member.stat[2], member.stat[3], member.stat[4], member.stat[5], member.stat[6], equip.wp,
                equip.wp_acc, equip.wp_var, equip.arm, equip.arm_var, equip.mdef_gear, equip.evd_gear, filled, unl, member.snap[0], member.snap[1], member.snap[2], member.pending[0],
                member.pending[1], member.pending[2]);
  }
}

// ------------------------------------------------------------------------------------------------ soak
std::atomic<long long> g_progress_ms{0};
std::atomic<int> g_cur_scene{-1}, g_cur_seed{0}, g_cur_cmd{0};
long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void crash_handler(int sig) {
  std::fprintf(stderr, "\nSOAK CRASH signal %d at scene %d seed %d command %d\n", sig, g_cur_scene.load(), g_cur_seed.load(), g_cur_cmd.load());
  std::fflush(stderr);
  std::_Exit(3);
}

struct PolicyRng {
  u32 state;
  u32 next() { state ^= state << 13, state ^= state >> 17, state ^= state << 5; return state; }
  int below(int bound) { return static_cast<int>(next() % static_cast<u32>(bound)); }
};

// Rotating party policy: attack chains of mixed strength, elements chosen by colour against the target, defend,
// heal when hurt, run occasionally (~2%).
Command soak_command(const Battle& battle, int pc, int step, PolicyRng& pr) {
  const Unit& unit = battle.unit(pc);
  Command command;
  command.actor = pc;
  command.target = -1;
  std::vector<int> foes;
  for (int i = kPcSlots; i < kUnits; ++i)
    if (battle.unit(i).alive() && !battle.hidden(i)) foes.push_back(i);
  if (pr.below(50) == 0) {
    command.kind = Command::Kind::Run;
    return command;
  }
  const int mode = (step + pc + pr.below(3)) % 6;
  if (!foes.empty() && pr.below(3) == 0) command.target = foes[static_cast<size_t>(pr.below(static_cast<int>(foes.size())))];
  if (mode == 3) {
    command.kind = Command::Kind::Defend;
    return command;
  }
  if (mode == 2 || mode == 4 || mode == 5) {
    const auto& el = battle.tables().elements;
    const int tc = foes.empty() ? 0 : battle.unit(command.target >= 0 ? command.target : foes[0]).innate;
    int best = -1, best_score = -1;
    int hurt = -1;
    for (int i = 0; i < kPcSlots; ++i)
      if (battle.unit(i).alive() && battle.unit(i).hp * 2 < battle.unit(i).max_hp) hurt = i;
    int heal_target = -1;
    for (int cell = 0; cell < data::kGridCells; ++cell) {
      if (!battle.element_usable(pc, cell) || (unit.cells[static_cast<size_t>(cell)] & 0x8000)) continue;
      const auto& element = el[unit.cells[static_cast<size_t>(cell)] & 0x3FFF];
      const bool heal = element.mode() == 3 || element.mode() == 43;
      int score = pr.below(20);
      if (heal) {
        if (mode != 4 || hurt < 0) continue;
        score += 100;
        heal_target = hurt;
      } else if (tc != 0 && element.colour_bit() != 0) {
        const int ci = data::colour_index_of_bit(element.colour_bit()), ct = data::colour_index_of_bit(static_cast<u8>(tc));
        if (ci >= 0 && ci < 6 && ct >= 0 && ct < 6 && kOpp[static_cast<size_t>(ct)] == ci) score += 50;
      }
      // Only damage and heal elements are considered.
      if (element.mode() != 0 && !heal) continue;
      if (score > best_score) best_score = score, best = cell;
    }
    if (best >= 0) {
      command.kind = Command::Kind::Element;
      command.cell = best;
      if (best_score >= 100) command.target = heal_target;
      return command;
    }
  }
  int budget = unit.stamina;
  command.kind = Command::Kind::Attack;
  const bool heavy = mode == 1;
  for (int i = 0; i < 4; ++i) {
    int strength = heavy ? 2 : std::min(i, 2);
    while (strength >= 0 && battle.chain_cost(strength, unit) > budget) --strength;
    if (strength < 0) break;
    budget -= battle.chain_cost(strength, unit);
    command.chain.push_back(strength);
  }
  if (command.chain.empty()) command.kind = Command::Kind::Defend;
  return command;
}

struct SoakStats {
  int battles = 0, victory = 0, wiped = 0, fled = 0, scripted = 0, capped = 0, stalled = 0, star_ups = 0;
  int invalid_cmds = 0, exceptions = 0;
  long long cmd_total = 0;
};

int cmd_soak(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  int seeds = 3, from = 0, to = static_cast<int>(tables.scenes.size()) - 1, cap = 2000;
  bool verbose = false;
  std::string party_from, brain_name = "scripted";
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--seeds" && i + 1 < args.size()) seeds = std::atoi(args[++i].c_str());
    else if (arg == "--scene" && i + 1 < args.size()) from = to = std::atoi(args[++i].c_str());
    else if (arg == "--from" && i + 1 < args.size()) from = std::atoi(args[++i].c_str());
    else if (arg == "--to" && i + 1 < args.size()) to = std::atoi(args[++i].c_str());
    else if (arg == "--cap" && i + 1 < args.size()) cap = std::atoi(args[++i].c_str());
    else if (arg == "--party-from" && i + 1 < args.size()) party_from = args[++i];
    else if (arg == "--brain" && i + 1 < args.size()) brain_name = args[++i];
    else if (arg == "-v") verbose = true;
  }
  SavedParty saved;
  if (!party_from.empty() && !load_party_from(tables, vfs, party_from, saved)) {
    std::fprintf(stderr, "cannot load party from %s\n", party_from.c_str());
    return 1;
  }
  std::signal(SIGSEGV, crash_handler);
  std::signal(SIGABRT, crash_handler);
  std::signal(SIGFPE, crash_handler);
  g_progress_ms = now_ms();
  // Watchdog: a battle that makes no progress for 60 s is a hang.
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (now_ms() - g_progress_ms.load() > 60000) {
        std::fprintf(stderr, "\nSOAK HANG at scene %d seed %d command %d\n", g_cur_scene.load(), g_cur_seed.load(), g_cur_cmd.load());
        std::fflush(stderr);
        std::_Exit(4);
      }
    }
  }).detach();

  SoakStats tot;
  int anomalies = 0;
  pac::ScriptedBrain::Stats pst;
  std::set<int> unimpl, invalid_ops;
  // Battle::stub_hits summed over every battle.
  std::map<std::string, int> stubs;
  int invalid_scenes = 0;
  std::vector<std::string> problems;
  auto problem = [&](int scene, u32 seed, int cmd, const std::string& what) {
    ++anomalies;
    if (problems.size() < 60) problems.push_back(std::format("scene {} seed {} cmd {}: {}", scene, seed, cmd, what));
  };
  for (int scene = from; scene <= to && scene < static_cast<int>(tables.scenes.size()); ++scene) {
    SoakStats ss;
    for (int seed = 1; seed <= seeds; ++seed) {
      g_cur_scene = scene, g_cur_seed = seed, g_cur_cmd = 0;
      g_progress_ms = now_ms();
      try {
        GameRng rng(static_cast<u32>(seed) * 7919u + static_cast<u32>(scene), tables.rng_table);
        RandomBrain random_brain;
        pac::ScriptedBrain scripted(pac_loader(vfs));
        EnemyBrain& brain = brain_name == "random" ? static_cast<EnemyBrain&>(random_brain) : static_cast<EnemyBrain&>(scripted);
        Battle battle(tables, rng, brain);
        std::vector<PartyMember> party;
        int star = 0;
        if (!saved.party.empty()) {
          party = saved.party;
          star = saved.star;
        } else {
          const int sets[3][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
          star = (seed - 1) * 15 % 90;
          for (int id : sets[(seed - 1) % 3]) party.push_back(make_party_member(tables, id, star, "char"));
        }
        BattleOptions opt;
        opt.growth.star = star;
        opt.ng_plus = seed % 4 == 0;
        if (!battle.start(party, scene, opt)) {
          if (seed == 1) ++invalid_scenes;
          break;
        }
        ++ss.battles;
        PolicyRng pr{static_cast<u32>(seed) * 2654435761u + static_cast<u32>(scene) * 40503u + 12345u};
        int steps = 0;
        bool stalled = false;
        auto check = [&](const char* when) {
          for (int i = 0; i < kUnits; ++i) {
            const Unit& unit = battle.unit(i);
            if (!unit.present) continue;
            const char* side = unit.pc ? "pc" : "enemy";
            if (unit.hp < 0 || unit.hp > unit.max_hp) problem(scene, seed, steps, std::format("{} {} hp {}/{} {}", side, i, unit.hp, unit.max_hp, when));
            if (unit.max_hp < 1 || unit.max_hp > (unit.pc ? 999 : 32767)) problem(scene, seed, steps, std::format("{} {} max_hp {} {}", side, i, unit.max_hp, when));
            if (unit.pc && (unit.stamina > 70 || unit.stamina < -200)) problem(scene, seed, steps, std::format("pc {} stamina {} {}", i, unit.stamina, when));
            if (unit.gauge < -1 || unit.gauge > unit.gauge_max) problem(scene, seed, steps, std::format("{} {} gauge {} {}", side, i, unit.gauge, when));
            for (const auto& [timer, remaining] : unit.timers)
              if (remaining < 0 || remaining > 100000) problem(scene, seed, steps, std::format("{} {} timer {}={} {}", side, i, timer, remaining, when));
            if (unit.countdown < -1000 || unit.countdown > 100000) problem(scene, seed, steps, std::format("{} {} countdown {} {}", side, i, unit.countdown, when));
          }
        };
        check("after start");
        while (battle.outcome() == Outcome::Ongoing && steps < cap) {
          int pc = -1;
          for (int i = 0; i < kPcSlots; ++i)
            if (battle.can_act(i)) {
              pc = i;
              break;
            }
          if (pc < 0) {
            stalled = true;
            break;
          }
          Command command = soak_command(battle, pc, steps, pr);
          std::string why;
          if (!battle.apply(command, &why)) {
            ++ss.invalid_cmds;
            Command defend;
            defend.kind = Command::Kind::Defend;
            defend.actor = pc;
            if (!battle.apply(defend, &why)) {
              problem(scene, seed, steps, "even Defend rejected: " + why);
              stalled = true;
              break;
            }
          }
          ++steps;
          g_cur_cmd = steps;
          g_progress_ms = now_ms();
          battle.drain_log();
          battle.drain_events();
          check("after command");
        }
        ss.cmd_total += steps;
        for (const auto& [stub, hits] : battle.stub_hits()) stubs[stub] += hits;
        const Outcome outcome = battle.outcome();
        if (outcome == Outcome::Victory) ++ss.victory;
        else if (outcome == Outcome::Wiped) ++ss.wiped;
        else if (outcome == Outcome::Fled) ++ss.fled;
        else if (outcome == Outcome::Scripted) ++ss.scripted;
        else if (stalled) {
          ++ss.stalled;
          problem(scene, seed, steps, "stalled: no PC can act and the battle is not over (alive pcs " + std::to_string(battle.alive_count(true)) + ")");
        } else {
          ++ss.capped;
          problem(scene, seed, steps, "step cap reached without an outcome");
        }
        if (outcome != Outcome::Ongoing) {
          for (size_t i = 0; i < battle.party().size(); ++i) {
            const auto& member = battle.party()[i];
            if (member.hp < 1 || member.hp > member.stat[0]) problem(scene, seed, steps, std::format("party {} final hp {}/{}", i, member.hp, member.stat[0]));
            for (int k = 0; k < 7; ++k) {
              const int gain = battle.rewards().gains[i][static_cast<size_t>(k)];
              if (gain < 0 || member.stat[static_cast<size_t>(k)] > (k == 0 ? 999 : 127) || member.stat[static_cast<size_t>(k)] < 0)
                problem(scene, seed, steps, std::format("party {} stat {} = {} gain {}", i, k, member.stat[static_cast<size_t>(k)], gain));
            }
          }
          const Rewards& rewards = battle.rewards();
          if (rewards.gold < 0 || rewards.gold > 9999999) problem(scene, seed, steps, std::format("gold {}", rewards.gold));
          if (rewards.star_up) ++ss.star_ups;
        }
        if (brain_name == "scripted") {
          const auto& brain_stats = scripted.stats();
          pst.turns += brain_stats.turns, pst.empty_plans += brain_stats.empty_plans, pst.fallbacks += brain_stats.fallbacks, pst.timeouts += brain_stats.timeouts,
              pst.reaction_attacks += brain_stats.reaction_attacks;
          if (brain_stats.timeouts) problem(scene, seed, steps, std::format("{} script timeouts (script did not terminate within budget)", brain_stats.timeouts));
          if (scripted.active()) {
            unimpl.insert(scripted.vm()->unimplemented().begin(), scripted.vm()->unimplemented().end());
            invalid_ops.insert(scripted.vm()->invalid().begin(), scripted.vm()->invalid().end());
          }
        }
      } catch (const std::exception& ex) {
        ++ss.exceptions;
        problem(scene, seed, g_cur_cmd.load(), std::string("exception: ") + ex.what());
      }
    }
    if (verbose && ss.battles)
      std::printf("scene %3d: %d battles V%d W%d F%d stall%d cap%d avg %lld cmds\n", scene, ss.battles, ss.victory, ss.wiped, ss.fled,
                  ss.stalled, ss.capped, ss.cmd_total / ss.battles);
    tot.battles += ss.battles, tot.victory += ss.victory, tot.wiped += ss.wiped, tot.fled += ss.fled, tot.scripted += ss.scripted, tot.capped += ss.capped,
        tot.stalled += ss.stalled, tot.star_ups += ss.star_ups, tot.invalid_cmds += ss.invalid_cmds, tot.exceptions += ss.exceptions,
        tot.cmd_total += ss.cmd_total;
  }
  std::printf("soak: scenes %d..%d, %d seeds, %d invalid scenes skipped, %d battles, %lld commands\n", from, to, seeds, invalid_scenes,
              tot.battles, tot.cmd_total);
  std::printf("  outcomes: victory %d wiped %d fled %d scripted %d | stalled %d, step-cap %d | star-ups %d | exceptions %d\n", tot.victory,
              tot.wiped, tot.fled, tot.scripted, tot.stalled, tot.capped, tot.star_ups, tot.exceptions);
  std::printf("  rejected policy commands (fell back to defend) %d; invariant anomalies %d\n", tot.invalid_cmds, anomalies);
  std::printf("  scripts: turns %d, empty plans %d, no-slot fallbacks %d, timeouts %d, slot-4 reaction attacks %d\n", pst.turns, pst.empty_plans,
              pst.fallbacks, pst.timeouts, pst.reaction_attacks);
  std::printf("  unimplemented ops:");
  for (int op : unimpl) std::printf(" %02X", op);
  std::printf("\n  invalid vm states:");
  for (int op : invalid_ops) std::printf(" %d", op);
  std::printf("\n");
  // Stubs by frequency.
  {
    std::vector<std::pair<int, std::string>> by_hits;
    for (const auto& [stub, hits] : stubs) by_hits.push_back({hits, stub});
    std::sort(by_hits.begin(), by_hits.end(), [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    std::printf("  stub hits (%zu kinds):", by_hits.size());
    for (const auto& [hits, stub] : by_hits) std::printf("\n    %6d  %s", hits, stub.c_str());
    std::printf("\n");
  }
  for (const auto& issue : problems) std::printf("  ! %s\n", issue.c_str());
  return anomalies || tot.exceptions ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------ xcheck
// Cross-check of the engine against an independent damage-formula transcription for random (attacker, target, attack)
// triples. The engine runs the real do_weapon_attack / do_element with a scripted mid-range roll source; the independent
// formula is transcribed below, with spread = variance/2 and roll = 2(what rnd(0,v) gives for Mid).
int comp_colour(int ci, int ce, const std::array<int, 3>& ring, int ct, int base) {
  int idx = base;
  if (ci == ce) idx += 1;
  if (ce < 6 && ci == kOpp[static_cast<size_t>(ce)]) idx -= 1;
  for (int ring_colour : ring) {
    if (ring_colour == ce) idx += 1;
    else if (ce < 6 && ring_colour == kOpp[static_cast<size_t>(ce)]) idx -= 1;
  }
  if (ct == ce && ce < 6) idx -= 1;
  idx = std::clamp(idx, 0, 9);
  return kColourMul[static_cast<size_t>(idx)];
}

// `ocbattle cast <scene> <actor> <element> <target> [seeds]`: the rules alone (no view): start the scene with the default party, cast the element from
// `actor` at `target` as the Debug tab does, finish the action, print the target's status words and (for a PC) which grid cells the seal greys.
int cmd_cast(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  if (args.size() < 4) {
    std::fprintf(stderr, "usage: ocbattle cast <scene> <actor> <element[,element...]> <target> [seeds]\n");
    return 2;
  }
  const int scene = std::stoi(args[0]), actor = std::stoi(args[1]), target = std::stoi(args[3]);
  std::vector<int> casts;
  for (size_t from = 0; from < args[2].size();) {
    const size_t comma = std::min(args[2].find(',', from), args[2].size());
    casts.push_back(std::stoi(args[2].substr(from, comma - from)));
    from = comma + 1;
  }
  const int el = casts.back();
  const int seeds = args.size() > 4 ? std::stoi(args[4]) : 1;
  for (int seed = 1; seed <= seeds; ++seed) {
    GameRng rng(static_cast<u32>(seed) * 7919u + static_cast<u32>(scene), tables.rng_table);
    pac::ScriptedBrain brain(pac_loader(vfs));
    Battle battle(tables, rng, brain);
    std::vector<PartyMember> party;
    for (int id : {0, 1, 2}) party.push_back(make_party_member(tables, id, 0, "char"));
    BattleOptions opt;
    if (!battle.start(party, scene, opt)) {
      std::fprintf(stderr, "scene %d does not start\n", scene);
      return 1;
    }
    const Unit& tu = battle.unit(target);
    const u32 lo0 = tu.lo, hi0 = tu.hi;
    const int countdown0 = tu.countdown;
    int code = -1;
    for (const int cast : casts) {
      if (battle.outcome() != Outcome::Ongoing) break;
      battle.begin_external_action();
      battle.do_element(actor, cast, 0, target);
      if (!battle.events().empty()) code = battle.events().back().code;
      battle.finish_external_action();
    }
    if (casts.size() > 1)
      for (const std::string& line : battle.drain_log()) std::printf("   %s\n", line.c_str());
    std::printf("seed %d: %s casts %d at %s: lo %08x -> %08x, hi %08x -> %08x, countdown %d -> %d, code %d, outcome %d result %d\n", seed,
                battle.unit(actor).name.c_str(), el, tu.name.c_str(), lo0, tu.lo, hi0, tu.hi, countdown0, tu.countdown, code, static_cast<int>(battle.outcome()),
                battle.result_code());
    if (tu.pc) {
      int sealed = 0, filled = 0;
      for (int cell = 0; cell < data::kGridCells; ++cell) {
        const int id = tu.cells[static_cast<size_t>(cell)] & 0x3FFF;
        if (id <= 0) continue;
        ++filled;
        if (battle.element_sealed(target, cell)) {
          ++sealed;
          std::printf("   cell %d %s sealed (usable %d)\n", cell, tables.element_name(id).c_str(), battle.element_usable(target, cell) ? 1 : 0);
        }
      }
      std::printf("   grid: %d filled cells, %d sealed\n", filled, sealed);
    }
  }
  return 0;
}

int cmd_xcheck(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  int cases = 20;
  u32 seed = 1234;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--n" && i + 1 < args.size()) cases = std::atoi(args[++i].c_str());
    else if (args[i] == "--seed" && i + 1 < args.size()) seed = static_cast<u32>(std::strtoul(args[++i].c_str(), nullptr, 0));
  }
  PolicyRng pr{seed * 2654435761u + 99u};
  std::vector<int> scenes, dmg_elems;
  for (int scene_index = 0; scene_index < static_cast<int>(tables.scenes.size()); ++scene_index) {
    ScriptedRng probe_rng(ScriptedRng::Fallback::Mid);
    RandomBrain rb;
    Battle battle(tables, probe_rng, rb);
    if (battle.start({make_party_member(tables, 0, 0, "char")}, scene_index)) scenes.push_back(scene_index);
  }
  for (size_t element_id = 1; element_id < tables.elements.size(); ++element_id)
    if (tables.elements[element_id].mode() == 0 && tables.elements[element_id].power() > 0) dmg_elems.push_back(static_cast<int>(element_id));
  int mism = 0, floors = 0, ext_diff = 0, ext_colour = 0;
  for (int kind = 0; kind < 2; ++kind)
    for (int k = 0; k < cases; ++k) {
      const int scene = scenes[static_cast<size_t>(pr.below(static_cast<int>(scenes.size())))];
      const int a_id = pr.below(9);
      std::vector<PartyMember> party;
      for (int id : {a_id, (a_id + 1) % 9, (a_id + 2) % 9}) party.push_back(make_party_member(tables, id, pr.below(10), "char"));
      ScriptedRng rng(ScriptedRng::Fallback::Mid);
      RandomBrain rb;
      Battle battle(tables, rng, rb);
      battle.start(party, scene);
      for (int i = 0; i < kUnits; ++i) {
        Unit& unit = battle.unit_mut(i);
        if (!unit.present) continue;
        unit.lo = unit.hi = 0;
        unit.timers.clear();
        unit.mod = {100, 100, 100, 100, 100, 100};
        unit.guard = false;
        // Big pools so the event value is not capped by remaining HP.
        unit.hp = unit.max_hp = unit.pc ? 999 : 32767;
      }
      battle.drain_events();
      int actor = 0;
      for (int i = 0; i < kPcSlots; ++i)
        if (battle.unit(i).src_id == a_id) actor = i;
      std::vector<int> foes;
      for (int i = kPcSlots; i < kUnits; ++i)
        if (battle.unit(i).present) foes.push_back(i);
      const int tgt = foes[static_cast<size_t>(pr.below(static_cast<int>(foes.size())))];
      const Unit& attacker = battle.unit(actor);
      const Unit& defender = battle.unit(tgt);
      const int ci = data::colour_index_of_bit(attacker.innate) < 0 ? kNoColour : data::colour_index_of_bit(attacker.innate);
      const int ct = data::colour_index_of_bit(defender.innate);
      const auto ring = battle.ring();
      long long engine = -1;
      int lit = 0, ext = 0, extE = 0;
      std::string what;
      if (kind == 0) {
        const int eid = dmg_elems[static_cast<size_t>(pr.below(static_cast<int>(dmg_elems.size())))];
        const int row = pr.below(8);
        const auto& element = tables.elements[static_cast<size_t>(eid)];
        battle.do_element(actor, eid, row, tgt);
        const auto ev = battle.drain_events();
        engine = ev.empty() ? -1 : (ev[0].code == 0 || ev[0].code == 1 ? ev[0].value : -2);
        const int ce = data::colour_index_of_bit(element.colour_bit()) < 0 ? kNoColour : data::colour_index_of_bit(element.colour_bit());
        const int spread = element.variance() / 2, roll = 2;
        const int mterm = 1 + defender.eq.mdef_gear, gterm = 1 + attacker.eq.mag_gear;
        const int slot = slot_mille(row, element.level());
        auto calc = [&](int tvar, int cmode = 0) {
          const long long attack_term = (long long)(attacker.mag + 1 + gterm + 1) * element.power() * (100 - spread);
          const long long defence_term = (long long)(defender.mdef + 2) * mterm * (100 - tvar);
          int raw = static_cast<int>((attack_term - defence_term) * slot / 400000);
          raw = std::max(raw, (element.power() - mterm) / 4);
          if (raw < 0) raw = 1;
          int damage = raw * (cmode ? colour_multiplier(ci, ce, ring, ct) : comp_colour(ci, ce, ring, ct, 3)) / 1000;
          damage = damage * (100 + roll) / 100;
          if (ct >= 0 && ce < 6 && kOpp[static_cast<size_t>(ct)] == ce) damage = damage * 300 / 200;
          if (attacker.eq.gear_flags & data::gearflag::kDmgUp) damage = damage * 125 / 100;
          if (defender.eq.gear_flags & data::gearflag::kDmgDown) damage = damage * 75 / 100;
          return damage;
        };
        lit = calc(0);
        ext = calc(defender.eq.arm_var / 2);
        extE = calc(defender.eq.arm_var / 2, 1);
        what = std::format("element {} ({} pow {} var {} lv {}) row {} slot {}", eid, ascii(tables.element_name(eid)), element.power(), element.variance(), element.level(), row, slot);
      } else {
        const int strength = pr.below(3);
        rng.push(0);
        battle.do_weapon_attack(actor, tgt, strength);
        const auto ev = battle.drain_events();
        engine = ev.empty() ? -1 : (ev[0].code == 0 || ev[0].code == 1 ? ev[0].value : -2);
        const int mult = kPcStrMul[static_cast<size_t>(strength)];
        const int spread = attacker.eq.wp_var / 2;
        auto calc = [&](int tvar, int cmode = 0) {
          const long long attack_term = (long long)(attacker.atk + 2) * attacker.eq.wp * (100 - spread);
          const long long defence_term = (long long)(defender.def + 2) * defender.eq.arm * (100 - tvar);
          int raw = static_cast<int>((attack_term - defence_term) * mult / 20000);
          raw = std::max(raw, (attacker.eq.wp - defender.eq.arm) / 2);
          if (raw < 0) raw = 1;
          int damage = raw * (cmode ? colour_multiplier(ci, ci, ring, ct) : comp_colour(ci, ci, ring, ct, 3)) / 1000;
          if (ct >= 0 && ci < 6 && kOpp[static_cast<size_t>(ct)] == ci) damage = damage * 300 / 200;
          if (attacker.eq.gear_flags & data::gearflag::kDmgUp) damage = damage * 125 / 100;
          if (defender.eq.gear_flags & data::gearflag::kDmgDown) damage = damage * 75 / 100;
          return damage;
        };
        lit = calc(0);
        ext = calc(defender.eq.arm_var / 2);
        extE = calc(defender.eq.arm_var / 2, 1);
        what = std::format("weapon {} wp {} var {}", strength == 0 ? "weak" : strength == 1 ? "medium" : "heavy", attacker.eq.wp, attacker.eq.wp_var);
      }
      std::string elog = battle.log().empty() ? std::string() : battle.log().back();
      if (!battle.log().empty()) for (const auto& line : battle.log()) if (line.find("attack on") != std::string::npos || line.find("casts") != std::string::npos) elog = line;
      const bool ok = engine == ext;
      const bool colour_only = !ok && engine == extE;
      if (colour_only) ++ext_colour;
      else if (!ok) ++mism;
      if (lit != ext) ++ext_diff;
      std::printf("%s %-7s scene %3d %s(atk) -> %s(tgt): %s | engine %lld reference %d (target-armVar-free %d)%s [ci %d ct %d ring %d %d %d | %s]\n", ok ? "ok  " : colour_only ? "COLR" : "DIFF", kind ? "weapon" : "element",
                  scene, ascii(attacker.name).c_str(), ascii(defender.name).c_str(), what.c_str(), engine, ext, lit, engine == -2 ? " [miss/immune]" : "", ci, ct, ring[0], ring[1], ring[2], ascii(elog).c_str());
    }
  std::printf("xcheck: %d colour-rule-only differences (literal field-circle reading vs the engine's colour multiplier)\n", ext_colour);
  std::printf("xcheck: %d mismatches of %d; %d cases where the literal reference formula (no target armour variance) differs\n", mism, 2 * cases,
              ext_diff);
  (void)floors;
  (void)vfs;
  return mism ? 1 : 0;
}

int cmd_sim(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  int scene = -1;
  u32 seed = 1;
  std::vector<int> party_ids = {0, 1, 2};
  int star = 0;
  std::string set = "char";
  bool quiet = false;
  std::string brain_name = "scripted";
  std::string party_from;
  int positional = 0;
  bool attack_only = false, poison = false;
  int battlefield = -1;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--quiet") quiet = true;
    else if (arg == "--attack") attack_only = true;
    else if (arg == "--poison") poison = true;
    else if (arg == "--field" && i + 1 < args.size()) battlefield = std::atoi(args[++i].c_str());
    else if (arg == "--brain" && i + 1 < args.size()) brain_name = args[++i];
    else if (arg == "--party-from" && i + 1 < args.size()) party_from = args[++i];
    else if (arg == "--party" && i + 1 < args.size()) party_ids = parse_ids(args[++i]);
    else if (arg == "--star" && i + 1 < args.size()) star = std::atoi(args[++i].c_str());
    else if (arg == "--set" && i + 1 < args.size()) set = args[++i];
    else if (positional == 0) scene = std::atoi(arg.c_str()), ++positional;
    else if (positional == 1) seed = static_cast<u32>(std::strtoul(arg.c_str(), nullptr, 0)), ++positional;
  }
  SavedParty saved;
  if (!party_from.empty()) {
    if (!load_party_from(tables, vfs, party_from, saved)) {
      std::fprintf(stderr, "cannot load party from %s\n", party_from.c_str());
      return 1;
    }
    star = saved.star;
    party_ids.assign(saved.party.size(), 0);
    print_party(saved.party, saved.star);
  }
  if (scene < 0 || party_ids.empty() || party_ids.size() > 3 || !tables.char_sets.count(set)) return usage();

  GameRng rng(seed, tables.rng_table);
  if (brain_name != "scripted" && brain_name != "random") return usage();
  RandomBrain random_brain;
  pac::ScriptedBrain scripted_brain(pac_loader(vfs));
  EnemyBrain& brain = brain_name == "random" ? static_cast<EnemyBrain&>(random_brain) : static_cast<EnemyBrain&>(scripted_brain);
  Battle battle(tables, rng, brain);
  std::vector<PartyMember> party;
  for (int id : party_ids) party.push_back(make_party_member(tables, id, star, set));
  if (!saved.party.empty()) party = saved.party;
  else if (!quiet) print_party(party, star);
  BattleOptions opt;
  opt.growth.star = star;
  opt.battlefield = battlefield;
  if (!battle.start(party, scene, opt)) {
    std::fprintf(stderr, "scene %d is invalid\n", scene);
    return 1;
  }
  if (poison)
    for (int i = 0; i < kUnits; ++i)
      if (battle.unit(i).present) battle.unit_mut(i).lo |= st::kPoison;
  std::array<int, 3> shown_ring = battle.ring();
  // --poison: after each command, every unit's HP change that no hit or heal event explains (the poison steps), and the actions that ran.
  std::array<int, kUnits> hp_before{};
  const auto note_hp = [&] {
    for (int i = 0; i < kUnits; ++i) hp_before[static_cast<size_t>(i)] = battle.unit(i).hp;
  };
  const auto print_upkeep = [&] {
    std::array<int, kUnits> explained{};
    std::string actions;
    int last_action = -1;
    for (const ActionEvent& event : battle.drain_events()) {
      if (event.target >= 0 && event.target < kUnits) explained[static_cast<size_t>(event.target)] += event.hp_after - event.hp_before;
      const bool acts = event.kind == ActionEvent::Kind::Weapon || event.kind == ActionEvent::Kind::Element || event.kind == ActionEvent::Kind::Defend;
      if (acts && event.action != last_action) {
        actions += std::format(" {}{}", event.actor < kPcSlots ? "pc" : "enemy", event.actor);
        last_action = event.action;
      }
    }
    std::string residual;
    for (int i = 0; i < kUnits; ++i)
      if (battle.unit(i).present) residual += std::format(" u{} {:+d}", i, battle.unit(i).hp - hp_before[static_cast<size_t>(i)] - explained[static_cast<size_t>(i)]);
    std::printf("  upkeep:%s | actions:%s\n", residual.c_str(), actions.c_str());
  };
  if (poison) battle.drain_events();
  auto flush = [&] {
    for (const auto& line : battle.drain_log())
      if (!quiet) std::printf("%s\n", ascii(line).c_str());
    if (!quiet && battle.ring() != shown_ring) {
      shown_ring = battle.ring();
      std::printf("  ring [%s %s %s]\n", data::colour_name(shown_ring[0]), data::colour_name(shown_ring[1]), data::colour_name(shown_ring[2]));
    }
  };
  flush();
  int turns = 0;
  while (battle.outcome() == Outcome::Ongoing && turns < 2000) {
    int pc = -1;
    for (int i = 0; i < kPcSlots; ++i)
      if (battle.can_act(i)) {
        pc = i;
        break;
      }
    if (pc < 0) break;
    Command command = auto_command(battle, pc);
    if (attack_only && command.kind == Command::Kind::Element) {
      command.kind = Command::Kind::Attack;
      command.cell = command.target = -1;
      int budget = battle.unit(pc).stamina;
      constexpr int max_chain = 4;
      for (int k = 0; k < max_chain; ++k) {
        int strength = std::min(k, 2);
        while (strength >= 0 && battle.chain_cost(strength, battle.unit(pc)) > budget) --strength;
        if (strength < 0) break;
        budget -= battle.chain_cost(strength, battle.unit(pc));
        command.chain.push_back(strength);
      }
      if (command.chain.empty()) command.kind = Command::Kind::Defend;
    }
    std::string why;
    if (poison) note_hp();
    if (!battle.apply(command, &why)) {
      std::fprintf(stderr, "auto policy produced an invalid command: %s\n", why.c_str());
      return 1;
    }
    ++turns;
    flush();
    if (poison) print_upkeep();
  }
  flush();
  const char* outcome_names[] = {"victory", "wiped", "fled"};
  const int outcome = static_cast<int>(battle.outcome());
  std::printf("result: %s  (scene %d, seed %u, %d commands, %d actions, %u rng calls)\n",
              outcome >= 0 && outcome <= 2 ? outcome_names[outcome] : "stalled/ongoing", scene, seed, turns, battle.actions(), rng.calls());
  const Rewards& rewards = battle.rewards();
  std::printf("rewards: gold=%d items=%zu elements=%zu star_up=%d progress=%d star=%d counter=%d\n", rewards.gold, rewards.items.size(), rewards.elements.size(),
              rewards.star_up ? 1 : 0, battle.growth().progress, battle.growth().star, battle.growth().counter);
  for (const u16 item : rewards.items) std::printf("  item 0x%X %s\n", item, ascii(tables.item_name(item)).c_str());
  for (const u16 element : rewards.elements) std::printf("  element %u %s\n", element, ascii(tables.element_name(element)).c_str());
  for (size_t i = 0; i < battle.party().size(); ++i) {
    const auto& member = battle.party()[i];
    std::printf("  %s hp=%d/%d gains:", ascii(member.name).c_str(), member.hp, member.stat[0]);
    for (int k = 0; k < 7; ++k) std::printf(" %d", rewards.gains[i][(size_t)k]);
    std::printf("\n");
  }
  return 0;
}

// `ocbattle elements`: id, raw name (UTF-8 bytes shown as hex when not ASCII), colour, level, mode and the first record bytes of every element.
int cmd_elements(const data::GameTables& tables) {
  for (size_t i = 0; i < tables.elements.size(); ++i) {
    const auto& element = tables.elements[i];
    const std::string& name = tables.element_name(static_cast<int>(i));
    std::string txt;
    for (unsigned char ch : name) txt += ch < 0x80 ? std::string(1, static_cast<char>(ch)) : std::format("<{:02X}>", ch);
    std::printf("%3zu %-26s col=%-6s lv=%d mode=%2d b0=%02X b1=%02X b4=%02X b5=%02X\n", i, txt.c_str(), data::colour_name(data::colour_index_of_bit(element.colour_bit())),
                element.level(), element.mode(), element.b(0), element.b(1), element.b(4), element.b(5));
  }
  return 0;
}

// ------------------------------------------------------------------------------------------------ debug-kill
// `ocbattle debug-kill [enemy_id [count]] [--scene S] [--keep-script]`: what the Debug tab's "Replace the enemies" does, headless. Per scene the record's
// enemy groups are replaced by `count` x `enemy_id` (default: the scene's own, i.e. no replacement), a scripted-brain battle starts, every
// enemy is knocked out with weapon attacks (hp set low) and the end test is run. Prints the scenes whose battle does NOT end in victory.
int cmd_debug_kill(const data::GameTables& source_tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  data::GameTables tables = source_tables;
  int enemy = -1, count = 1, only = -1;
  bool keep_script = false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--scene" && i + 1 < args.size()) only = std::atoi(args[++i].c_str());
    else if (args[i] == "--keep-script") keep_script = true;
    else if (enemy < 0) enemy = std::atoi(args[i].c_str());
    else count = std::atoi(args[i].c_str());
  }
  int bad = 0, total = 0;
  for (int scene = 0; scene < static_cast<int>(tables.scenes.size()); ++scene) {
    if (only >= 0 && scene != only) continue;
    const auto saved = tables.scenes[static_cast<size_t>(scene)].raw;
    if (enemy >= 0) {
      auto& raw = tables.scenes[static_cast<size_t>(scene)].raw;
      for (size_t group = 0; group < 3; ++group) {
        raw[group] = group == 0 ? static_cast<u8>(enemy) : u8{0xFF};
        raw[3 + group] = group == 0 ? static_cast<u8>(count) : u8{0};
      }
    }
    std::vector<PartyMember> party;
    for (int id : {0, 1, 2}) party.push_back(make_party_member(tables, id, 0, "char"));
    GameRng rng(static_cast<u32>(scene) + 7, tables.rng_table);
    pac::ScriptedBrain brain(pac_loader(vfs));
    // The Debug tab runs replaced enemies without the scene's script.
    brain.set_enabled(enemy < 0 || keep_script);
    Battle battle(tables, rng, brain);
    if (!battle.start(party, scene)) {
      tables.scenes[static_cast<size_t>(scene)].raw = saved;
      continue;
    }
    ++total;
    for (int round = 0; round < 40 && battle.outcome() == Outcome::Ongoing; ++round) {
      bool any = false;
      for (int unit = kPcSlots; unit < kUnits; ++unit) {
        if (!battle.unit(unit).present || !battle.unit(unit).alive()) continue;
        any = true;
        battle.unit_mut(unit).hp = 1;
        battle.do_weapon_attack(0, unit, 2);
      }
      if (!any) break;
      battle.check_end();
    }
    battle.check_end();
    int alive = 0;
    for (int unit = kPcSlots; unit < kUnits; ++unit) alive += battle.unit(unit).present && battle.unit(unit).alive();
    if (battle.outcome() != Outcome::Victory) {
      ++bad;
      std::printf("scene %d: outcome %d, enemies alive %d, end_check %d, forced %d\n", scene, static_cast<int>(battle.outcome()), alive,
                  brain.end_check_enabled() ? 1 : 0, brain.forced_result());
    }
    tables.scenes[static_cast<size_t>(scene)].raw = saved;
  }
  std::printf("debug-kill: %d battles, %d did not end in victory\n", total, bad);
  return bad ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------ pac
int cmd_pac_disasm(const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  if (args.empty()) return usage();
  const int scene = std::atoi(args[0].c_str());
  std::string name;
  auto data = read_pac(vfs, scene, &name);
  if (!data) {
    std::fprintf(stderr, "no pac for scene %d\n", scene);
    return 1;
  }
  pac::Module pac_module;
  std::string err;
  if (!pac_module.parse(std::move(*data), &err)) {
    std::fprintf(stderr, "%s: %s\n", name.c_str(), err.c_str());
    return 1;
  }
  std::printf("%s: entities=%d base=0x%zX end=0x%zX\n", name.c_str(), pac_module.entities, pac_module.base, pac_module.end);
  std::set<std::pair<u32, u32>> seen;
  for (const pac::ScriptExtent& script : pac_module.scripts()) {
    if (!seen.insert({script.start, script.end}).second) continue;
    std::printf("entity %d script %d  [%04zX..%04zX) rel %04X\n", script.entity, script.slot, pac_module.base + script.start, pac_module.base + script.end, script.start);
    for (u32 pos = script.start; pos < script.end;) {
      const u8 op = pac_module.at(pos);
      const pac::OpInfo& oi = pac::op_info(op);
      if (oi.len == 0 || pos + oi.len > script.end) {
        std::printf("  bad op %02X at %X\n", op, pos);
        break;
      }
      std::string hex;
      for (u32 k = 1; k < oi.len; ++k) {
        char byte_hex[4];
        std::snprintf(byte_hex, sizeof byte_hex, "%02x", pac_module.at(pos + k));
        if (k > 1) hex += ' ';
        hex += byte_hex;
      }
      std::printf("  %04X: %02X %-20s %s\n", pos, op, oi.name, hex.c_str());
      pos += oi.len;
    }
  }
  return 0;
}

int cmd_pac_soak(const data::GameTables& tables, const vfs::Vfs& vfs, const std::vector<std::string>& args) {
  int turns = 40, from = 0, to = static_cast<int>(tables.scenes.size()) - 1;
  bool verbose = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "-v") verbose = true;
    else if (arg == "--scene" && i + 1 < args.size()) from = to = std::atoi(args[++i].c_str());
    else if (arg == "--from" && i + 1 < args.size()) from = std::atoi(args[++i].c_str());
    else if (arg == "--to" && i + 1 < args.size()) to = std::atoi(args[++i].c_str());
    else if (!arg.empty() && std::all_of(arg.begin(), arg.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; })) turns = std::atoi(arg.c_str());
    else {
      std::fprintf(stderr, "pac-soak: unknown argument '%s'\n", arg.c_str());
      return 2;
    }
  }
  int started = 0, no_pac = 0, failed = 0, cmds = 0, scenes = 0;
  u64 ops = 0;
  std::array<u64, 256> opc{};
  pac::ScriptedBrain::Stats tot;
  std::set<int> unimpl, invalid, fields;
  int ended[4] = {};
  for (int scene = std::max(0, from); scene <= to && scene < static_cast<int>(tables.scenes.size()); ++scene) {
    ++scenes;
    try {
      GameRng rng(static_cast<u32>(scene) + 1, tables.rng_table);
      pac::ScriptedBrain brain(pac_loader(vfs));
      Battle battle(tables, rng, brain);
      std::vector<PartyMember> party;
      for (int id : {0, 1, 2}) party.push_back(make_party_member(tables, id, 0, "char"));
      if (!battle.start(party, scene)) continue;
      ++started;
      if (!brain.active()) {
        ++no_pac;
        continue;
      }
      // Direct pass on a throw-away battle: every slot of every unit's entity, as the scheduler would launch them. The kill script (slot 3)
      // runs last; its A6 takes an enemy's entity out of the run list, so the unit is revived for the second pass.
      {
        GameRng rng2(static_cast<u32>(scene) + 1000, tables.rng_table);
        pac::ScriptedBrain brain2(pac_loader(vfs));
        Battle b2(tables, rng2, brain2);
        if (b2.start(party, scene) && brain2.active()) {
          for (int rep = 0; rep < 2; ++rep)
            for (int unit = 0; unit < kUnits; ++unit)
              for (int slot : {2, 4, 3}) {
                std::vector<pac::QueuedAttack> queued;
                brain2.vm()->run_slot(unit, slot, queued);
                brain2.vm()->tick();
                if (slot == 3) brain2.vm()->revive(unit);
              }
          ops += brain2.vm()->ops_executed();
          for (size_t i = 0; i < 256; ++i) opc[i] += brain2.vm()->op_counts()[i];
          unimpl.insert(brain2.vm()->unimplemented().begin(), brain2.vm()->unimplemented().end());
          invalid.insert(brain2.vm()->invalid().begin(), brain2.vm()->invalid().end());
          fields.insert(brain2.unknown_fields().begin(), brain2.unknown_fields().end());
        }
      }
      for (int turn = 0; turn < turns && battle.outcome() == Outcome::Ongoing; ++turn) {
        int pc = -1;
        for (int i = 0; i < kPcSlots; ++i)
          if (battle.can_act(i)) {
            pc = i;
            break;
          }
        if (pc < 0) break;
        if (!battle.apply(auto_command(battle, pc))) break;
        ++cmds;
      }
      const int outcome = static_cast<int>(battle.outcome());
      ++ended[outcome < 0 || outcome > 2 ? 3 : outcome];
      // OC_SOAK_LIST lists the per-scene outcomes (to diff two builds' soaks).
      if (std::getenv("OC_SOAK_LIST")) std::printf("scene %d outcome %d\n", scene, outcome);
      const auto& brain_stats = brain.stats();
      if (verbose && brain_stats.empty_plans)
        std::printf("scene %d: %d turns, %d empty plans, outcome %d\n", scene, brain_stats.turns, brain_stats.empty_plans, outcome);
      tot.turns += brain_stats.turns, tot.empty_plans += brain_stats.empty_plans, tot.fallbacks += brain_stats.fallbacks, tot.timeouts += brain_stats.timeouts,
          tot.reaction_attacks += brain_stats.reaction_attacks;
      ops += brain.vm()->ops_executed();
      for (size_t i = 0; i < 256; ++i) opc[i] += brain.vm()->op_counts()[i];
      unimpl.insert(brain.vm()->unimplemented().begin(), brain.vm()->unimplemented().end());
      invalid.insert(brain.vm()->invalid().begin(), brain.vm()->invalid().end());
      fields.insert(brain.unknown_fields().begin(), brain.unknown_fields().end());
    } catch (const std::exception& ex) {
      std::printf("scene %d: exception %s\n", scene, ex.what());
      ++failed;
    }
  }
  std::printf("pac-soak: %d scenes, %d battles started (%d without pac), %d commands, %d exceptions\n", scenes, started, no_pac, cmds, failed);
  std::printf("  outcomes: victory %d wiped %d fled %d ongoing/other %d\n", ended[0], ended[1], ended[2], ended[3]);
  std::printf("  ops executed %llu; scripted turns %d (empty plans %d, no slot-2 fallbacks %d, timeouts %d, slot-4 reaction attacks %d)\n",
              static_cast<unsigned long long>(ops), tot.turns, tot.empty_plans, tot.fallbacks, tot.timeouts, tot.reaction_attacks);
  auto list = [](const char* title, const std::set<int>& values, const char* fmt) {
    std::printf("  %s:", title);
    for (int value : values) std::printf(fmt, value);
    std::printf("\n");
  };
  list("unused ('U') ops executed", unimpl, " %02X");
  std::printf("  executions of ops resolved from battle.prg:");
  for (const int op : {0x39, 0x5C, 0x7B, 0x7D, 0x81, 0x85, 0x91, 0xAB, 0xAE, 0xAF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xE3})
    std::printf(" %02X=%llu", op, static_cast<unsigned long long>(opc[static_cast<size_t>(op)]));
  std::printf("\n");
  list("invalid opcodes reached (-1 pc out of range, -2 call overflow)", invalid, " %d");
  list("unmodelled unit fields (0x10000 = write)", fields, " %X");
  return failed ? 1 : 0;
}

// Element ids the enemy battle scripts queue (QueuedAttack type 1/2/3/6), counted once per scene: every slot 2..4 of every unit's entity run
// twice on a throw-away battle (the pac-soak direct pass, slot 3 last and the unit revived after it). Feeds the usage rank of `fx-audit`.
std::map<int, int> enemy_element_refs(const data::GameTables& tables, const vfs::Vfs& vfs) {
  std::map<int, int> refs;
  for (int scene = 0; scene < static_cast<int>(tables.scenes.size()); ++scene) {
    try {
      std::vector<PartyMember> party;
      for (int id : {0, 1, 2}) party.push_back(make_party_member(tables, id, 0, "char"));
      GameRng rng(static_cast<u32>(scene) + 1000, tables.rng_table);
      pac::ScriptedBrain brain(pac_loader(vfs));
      Battle battle(tables, rng, brain);
      if (!battle.start(party, scene) || !brain.active()) continue;
      std::set<int> seen;
      for (int rep = 0; rep < 2; ++rep)
        for (int unit = 0; unit < kUnits; ++unit)
          for (int slot : {2, 4, 3}) {
            std::vector<pac::QueuedAttack> queued;
            brain.vm()->run_slot(unit, slot, queued);
            brain.vm()->tick();
            if (slot == 3) brain.vm()->revive(unit);
            for (const auto& attack : queued)
              if ((attack.type == 1 || attack.type == 2 || attack.type == 3 || attack.type == 6) && attack.value > 0) seen.insert(attack.value);
          }
      for (int element_id : seen) ++refs[element_id];
    } catch (const std::exception&) {
    }
  }
  return refs;
}

// ------------------------------------------------------------------------------------------------ formula
std::unique_ptr<Rng> make_rng(const std::string& spec) {
  if (spec.rfind("seed:", 0) == 0) return std::make_unique<GameRng>(static_cast<u32>(std::strtoul(spec.c_str() + 5, nullptr, 0)));
  if (spec == "lo") return std::make_unique<ScriptedRng>(ScriptedRng::Fallback::Lo);
  if (spec == "hi") return std::make_unique<ScriptedRng>(ScriptedRng::Fallback::Hi);
  return std::make_unique<ScriptedRng>(ScriptedRng::Fallback::Mid);
}

int cmd_formula(const std::vector<std::string>& args, const std::string& rng_spec) {
  auto int_arg = [&](size_t i) { return i < args.size() ? std::atoi(args[i].c_str()) : 0; };
  auto rng = make_rng(rng_spec);
  if (args.empty()) {
    std::printf(
        "ceil100 x y | colour ci ce r0 r1 r2 ct | hit base hitmod evd evdmod hits strength | slot row level\n"
        "weapon atk wp wpvar def arm armvar mult colour strength | element mag magterm power var mdef mdefterm mdefvar slot colour\n"
        "heal mag p slot | status_timer idx dur mod | growth x star | minor snap pending cur b P S\n"
        "recovery rate mod steps flu | stamina_cost strength fatigued | sum n p v\n");
    return 0;
  }
  const std::string& formula = args[0];
  if (formula == "ceil100") std::printf("%d\n", ceil100(int_arg(1), int_arg(2)));
  else if (formula == "colour") std::printf("%d\n", colour_multiplier(int_arg(1), int_arg(2), {int_arg(3), int_arg(4), int_arg(5)}, int_arg(6)));
  else if (formula == "hit") {
    ChanceIn in;
    in.base = int_arg(1), in.hit_mod = int_arg(2), in.evd = int_arg(3), in.evd_mod = int_arg(4), in.hits = int_arg(5), in.strength = int_arg(6);
    std::printf("%d\n", hit_chance(in));
  } else if (formula == "slot") std::printf("%d\n", slot_mille(int_arg(1), int_arg(2)));
  else if (formula == "weapon") {
    WeaponIn weapon;
    weapon.atk = int_arg(1), weapon.wp = int_arg(2), weapon.wp_var = int_arg(3), weapon.def = int_arg(4), weapon.arm = int_arg(5), weapon.arm_var = int_arg(6), weapon.mult = int_arg(7), weapon.colour_mille = int_arg(8),
    weapon.strength = int_arg(9);
    const DamageOut damage = weapon_damage(*rng, weapon);
    std::printf("%d code=%d crit=%d\n", damage.value, damage.code, damage.crit ? 1 : 0);
  } else if (formula == "element") {
    ElementIn element;
    element.mag = int_arg(1), element.mag_term = int_arg(2), element.power = int_arg(3), element.var = int_arg(4), element.mdef = int_arg(5), element.mdef_term = int_arg(6), element.mdef_var = int_arg(7),
    element.slot_mille = int_arg(8), element.colour_mille = int_arg(9);
    const DamageOut damage = element_damage(*rng, element);
    std::printf("%d code=%d\n", damage.value, damage.code);
  } else if (formula == "heal") std::printf("%d\n", heal_amount(*rng, int_arg(1), int_arg(2), int_arg(3)));
  else if (formula == "status_timer") std::printf("%d\n", status_timer(int_arg(1), int_arg(2), int_arg(3)));
  else if (formula == "growth") std::printf("%d\n", growth_roll(*rng, int_arg(1), int_arg(2)));
  else if (formula == "minor") std::printf("%d\n", minor_growth(int_arg(1), int_arg(2), int_arg(3), int_arg(4), int_arg(5), int_arg(6)));
  else if (formula == "recovery") std::printf("%d\n", recovery_step(int_arg(1), int_arg(2), int_arg(3), int_arg(4) != 0));
  else if (formula == "stamina_cost") std::printf("%d\n", weapon_stamina_cost(int_arg(1), int_arg(2) != 0));
  else if (formula == "sum") std::printf("%d\n", sum_rolls(*rng, int_arg(1), int_arg(2), int_arg(3)));
  else {
    std::fprintf(stderr, "unknown formula '%s'\n", formula.c_str());
    return 1;
  }
  return 0;
}

}  // namespace

int ocbattle_main(int argc, char** argv) {
  std::vector<std::string> args;
  std::string gamedir, rng_spec = "mid";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-g" && i + 1 < argc) gamedir = argv[++i];
    else if (arg == "--rng" && i + 1 < argc) rng_spec = argv[++i];
    else args.push_back(arg);
  }
  if (gamedir.empty())
    if (const char* env_dir = std::getenv("OPENCROSS_GAMEDIR")) gamedir = env_dir;
  if (args.empty()) return usage();
  if (args[0] == "formula") return cmd_formula({args.begin() + 1, args.end()}, rng_spec);

  vfs::Vfs vfs;
  const auto dir = platform::find_game_dir(gamedir);
  if (dir.empty() || !vfs.open(dir)) {
    std::fprintf(stderr, "game directory not found (use -g <dir> or OPENCROSS_GAMEDIR)\n");
    return 1;
  }
  data::GameTables tables;
  std::string err;
  if (!tables.load(vfs, &err)) {
    std::fprintf(stderr, "table load failed: %s\n", err.c_str());
    return 1;
  }
  if (args[0] == "tables") return cmd_tables(tables);
  if (args[0] == "scenes") return cmd_scenes(tables);
  if (args[0] == "sim") return cmd_sim(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "xcheck") return cmd_xcheck(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "soak") return cmd_soak(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "pac-disasm") return cmd_pac_disasm(vfs, {args.begin() + 1, args.end()});
  if (args[0] == "pac-soak") return cmd_pac_soak(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "elements") return cmd_elements(tables);
  if (args[0] == "cast") return cmd_cast(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "debug-kill") return cmd_debug_kill(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "fx-reg") return cmd_fx_reg(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "versus") return cmd_versus(tables, vfs, {args.begin() + 1, args.end()});
  if (args[0] == "versus-net") return cmd_versus_net(tables, {args.begin() + 1, args.end()});
  if (args[0] == "fx-audit") return cmd_fx_audit(tables, vfs, {args.begin() + 1, args.end()}, enemy_element_refs(tables, vfs));
  return usage();
}
