// ocbattleseq: dumps what the animation-sequence VM does for a battle model (dev tool).
//   ocbattleseq [-g <gamedir>] <folder e.g. player/selju2 | monster/chouchin> [entry variant [a b hit dead]]
//   ocbattleseq [-g <gamedir>] sfx <folder> <tag> [weapon class [seconds]]    renders a battle sound offline and prints its level
//   ocbattleseq [-g <gamedir>] emitters <cdrom path> | audit | sfxscan [weapon class] | sfxslots <stage.prd> <unit> <weapon class> [pack ...]
//                              | sfxbanks <.prd> | sfxfind <hex> | opscan <ops>    dev audits of effect emitters, sequence ops and battle sounds
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "audio/akao_bank.h"
#include "game/battle/battle_audio.h"
#include "game/battle/battle_seq.h"
#include "import/container.h"
#include "vfs/vfs.h"

using namespace oc;

int ocbattleseq_main(int argc, char** argv) {
  std::string game;
  int i = 1;
  if (i < argc && std::string(argv[i]) == "-g") {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "ocbattleseq: -g needs a game directory\n");
      return 2;
    }
    game = argv[i + 1];
    i += 2;
  } else if (const char* env_dir = std::getenv("OPENCROSS_GAMEDIR")) {
    game = env_dir;
  }
  if (i >= argc) {
    std::printf(
        "usage: ocbattleseq [-g gamedir] <player/selju2 | monster/x | pack <effect .prd path>> [entry variant [a b hit dead]]  (entry -1 = atk table)\n"
        "       ocbattleseq [-g gamedir] emitters <cdrom path of a drp>\n"
        "       ocbattleseq [-g gamedir] audit\n"
        "       ocbattleseq [-g gamedir] sfx <folder|.prd> <tag> [weapon class [seconds]]\n"
        "       ocbattleseq [-g gamedir] sfxscan [weapon class]\n"
        "       ocbattleseq [-g gamedir] sfxslots <stage.prd> <unit folder|.prd> <weapon class> [pack .prd ...]\n"
        "       ocbattleseq [-g gamedir] sfxbanks <.prd under cdrom/battle/>\n"
        "       ocbattleseq [-g gamedir] sfxfind <hex byte pattern>\n"
        "       ocbattleseq [-g gamedir] opscan <hex op,hex op,...>\n");
    return 2;
  }
  vfs::Vfs fs;
  if (!fs.open(game)) {
    std::printf("cannot open game dir\n");
    return 1;
  }
  // emitters <cdrom path>: the type-0x0A / 0x16 entry names of a drp, nested drps included.
  if (std::string(argv[i]) == "emitters" && i + 1 < argc) {
    auto raw = fs.read(argv[i + 1]);
    if (!raw) return 1;
    std::function<void(ByteSpan, const std::string&)> walk = [&](ByteSpan data, const std::string& pre) {
      auto drp = import::parse_drp(data);
      if (!drp) return;
      std::string line;
      for (const auto& entry : drp->entries) {
        if (entry.type == 0x0A || entry.type == 0x16) line += (entry.type == 0x16 ? "snd:" : "") + entry.name + " ";
        if (entry.type == 1 && import::is_drp(entry.data)) walk(entry.data, pre + "/" + entry.name);
      }
      std::printf("%s: %s\n", pre.c_str(), line.c_str());
    };
    walk(*raw, "root");
    return 0;
  }
  // audit: every executed op over every atk / S5 script of every battle model (dev audit).
  if (std::string(argv[i]) == "audit") {
    std::map<int, int> atk_ops, s5_ops;
    std::map<int, std::set<std::string>> atk_who, s5_who;
    std::map<std::string, int> fx_tags;
    std::set<std::string> printed_all;
    int models = 0;
    for (const std::string& path : fs.list("cdrom")) {
      if (path.size() < 12 || path.compare(path.size() - 8, 8, "/at0.prd") != 0 || path.compare(0, 7, "battle/") != 0) continue;
      const std::string folder = path.substr(7, path.size() - 15);
      const std::string stem = folder.substr(folder.find_last_of('/') + 1);
      auto prd = fs.read("cdrom/" + path);
      auto obj = fs.read("cdrom/battle/" + folder + "/" + stem + ".obj");
      if (!obj && !stem.empty() && stem.back() >= '0' && stem.back() <= '9') obj = fs.read("cdrom/battle/" + folder + "/" + stem.substr(0, stem.size() - 1) + ".obj");
      game::bseq::Table s5, atk;
      const bool has_s5 = obj && game::bseq::load_model_table(*obj, s5);
      const bool has_atk = prd && game::bseq::load_atk_table(*prd, atk);
      if (has_atk && has_s5) atk.set_model_table(&s5);
      std::set<std::string> emit_names;
      if (auto drp = prd ? import::parse_drp(*prd) : std::nullopt)
        for (const auto& entry : drp->entries)
          if (entry.type == 0x0A) emit_names.insert(entry.name);
      ++models;
      auto run = [&](const game::bseq::Table& table, int entry, int variant, std::map<int, int>& ops, std::map<int, std::set<std::string>>& who, game::bseq::Ctx ctx) {
        game::bseq::Vm vm;
        if (!vm.start(&table, entry, variant)) return;
        // OC_AUDIT_OPS, e.g. "19,1A,20": print the operands of those ops once per script.
        static const char* want = std::getenv("OC_AUDIT_OPS");
        vm.trace = [&](int, const s16* words, int len) {
          ++ops[words[0]];
          who[words[0]].insert(stem);
          if (want) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "%02X", words[0]);
            if ((std::string(",") + want + ",").find(std::string(",") + hex + ",") != std::string::npos) {
              std::string line = stem + (&table == &atk ? " atk" : " S5") + std::to_string(entry) + "/" + std::to_string(variant) + " [a" + std::to_string(ctx.action_a) + " b" + std::to_string(ctx.action_b) + " hit" + std::to_string(ctx.hit_code) + " dead" + std::to_string(ctx.dead) + "] op " + hex + ":";
              for (int k = 1; k < len; ++k) line += " " + std::to_string(words[k]);
              if (printed_all.insert(line).second) std::printf("%s\n", line.c_str());
            }
          }
        };
        // OC_AUDIT_TRACE: ordered op trace of one reaction (hit_code / dead from OC_AUDIT_HIT).
        if (static const char* tr = std::getenv("OC_AUDIT_TRACE"); tr && stem == tr && &table == &atk && variant == 7 && ctx.action_a == 0 && ctx.action_b == 0) {
          static const int want_hit = std::getenv("OC_AUDIT_HIT") ? std::atoi(std::getenv("OC_AUDIT_HIT")) : 0;
          if (ctx.hit_code == want_hit % 10 && ctx.dead == (want_hit >= 10)) {
            auto prev = vm.trace;
            vm.trace = [prev](int pc, const s16* words, int len) {
              std::printf("      %4d:", pc);
              for (int k = 0; k < len; ++k) std::printf(" %d", words[k]);
              std::printf("\n");
              if (prev) prev(pc, words, len);
            };
          }
        }
        ctx.clip_frames = [](int) { return 8; };
  // OC_SEQ_GETPOS: x,z of the unit and of every GetPos point (makes the position-driven branches of a pack script run).
  if (const char* gp = std::getenv("OC_SEQ_GETPOS")) {
    int ux = 0, uz = 0, tx = 0, tz = 0;
    if (std::sscanf(gp, "%d,%d,%d,%d", &ux, &uz, &tx, &tz) == 4) {
      ctx.pos_x = ux;
      ctx.pos_z = uz;
      ctx.get_pos = [tx, tz](int, int, int, int& x, int& z) {
        x = tx;
        z = tz;
        return true;
      };
    }
  }
        std::vector<game::bseq::Event> ev;
        for (int tick = 0; tick < 600 && vm.running(); ++tick) {
          ev.clear();
          vm.tick(ctx, ev);
          for (const auto& event : ev)
            if (event.kind == game::bseq::Event::Effect) {
              const std::string tg = game::bseq::tag_str(event.tag);
              ++fx_tags[stem + " " + tg + (emit_names.count(tg) ? " (own emitter)" : " (not in at0.prd)")];
            }
        }
      };
      for (int action_a = 0; action_a < 3; ++action_a)
        for (int action_b = 0; action_b < 2; ++action_b)
          for (int hit = 0; hit < 4; ++hit)
            for (int dead = 0; dead < 2; ++dead) {
              game::bseq::Ctx ctx;
              ctx.action_a = action_a, ctx.action_b = action_b, ctx.hit_code = hit, ctx.dead = dead != 0;
              if (has_atk) {
                run(atk, 0, 0, atk_ops, atk_who, ctx);
                run(atk, 0, 7, atk_ops, atk_who, ctx);
              }
              if (has_s5)
                for (int en = 0; en < s5.entries(); ++en) run(s5, en, 0, s5_ops, s5_who, ctx);
            }
    }
    auto dump = [&](const char* nm, std::map<int, int>& ops, std::map<int, std::set<std::string>>& who) {
      std::printf("%s ops (%zu distinct):\n", nm, ops.size());
      for (auto& [op, count] : ops) std::printf("  %02X x%d in %zu models\n", op, count, who[op].size());
    };
    std::printf("%d models\n", models);
    dump("atk", atk_ops, atk_who);
    dump("S5", s5_ops, s5_who);
    for (auto& [tag, count] : fx_tags) std::printf("fx %s x%d\n", tag.c_str(), count);
    return 0;
  }
  // opscan <hex,hex,...>: a static sweep of every animation-sequence table on the disc (model S5 of every .obj, entry 1 of every
  // fieldobj/ex pack, every type-0x1A table of a battle .prd, nested drps included): each use of the listed ops with its operands,
  // then the totals per table kind.
  if (std::string(argv[i]) == "opscan" && i + 1 < argc) {
    std::set<int> want;
    const std::string list = argv[i + 1];
    for (size_t pos = 0; pos < list.size();) {
      const size_t comma = std::min(list.find(',', pos), list.size());
      want.insert(static_cast<int>(std::strtol(list.substr(pos, comma - pos).c_str(), nullptr, 16)));
      pos = comma + 1;
    }
    std::map<std::string, int> tables;
    std::map<std::string, std::map<int, int>> hits;
    auto scan = [&](const game::bseq::Table& table, const std::string& kind, const std::string& name) {
      ++tables[kind];
      const auto& words = table.words();
      table.each_op([&](int entry, int pc) {
        const int op = words[static_cast<size_t>(pc)];
        if (!want.count(op)) return;
        ++hits[kind][op];
        std::string line = kind + " " + name + " entry " + std::to_string(entry) + " pc " + std::to_string(pc);
        char hex[8];
        std::snprintf(hex, sizeof hex, " op %02X", op);
        line += hex;
        for (size_t k = 1; k < 4 && static_cast<size_t>(pc) + k < words.size(); ++k) line += " " + std::to_string(words[static_cast<size_t>(pc) + k]);
        std::printf("%s\n", line.c_str());
      });
    };
    std::function<void(ByteSpan, const std::string&)> scan_drp = [&](ByteSpan data, const std::string& name) {
      auto drp = import::parse_drp(data);
      if (!drp) return;
      for (const auto& entry : drp->entries) {
        game::bseq::Table table;
        if (entry.type == 0x1A && table.parse(entry.data)) scan(table, "prd", name + ":" + entry.name);
        if (entry.type == 1 && import::is_drp(entry.data)) scan_drp(entry.data, name + "/" + entry.name);
      }
    };
    for (const std::string& path : fs.list("cdrom")) {
      auto ends = [&](const char* tail) { return path.size() >= std::strlen(tail) && path.compare(path.size() - std::strlen(tail), std::strlen(tail), tail) == 0; };
      const std::string kind = path.compare(0, 7, "battle/") == 0 ? "battle" : "field";
      if (ends(".obj")) {
        auto obj = fs.read("cdrom/" + path);
        game::bseq::Table table;
        if (obj && game::bseq::load_model_table(*obj, table)) scan(table, kind + "-S5", path);
      } else if (ends(".bin") && path.compare(0, 12, "fieldobj/ex/") == 0) {
        auto bin = fs.read("cdrom/" + path);
        auto cpt = bin ? import::parse_cpt(*bin) : std::nullopt;
        game::bseq::Table table;
        if (cpt && cpt->entries.size() == 2 && table.parse(cpt->entries[1].data)) scan(table, "field-ex", path);
      } else if (ends(".prd") && kind == "battle") {
        if (auto prd = fs.read("cdrom/" + path)) scan_drp(*prd, path);
      }
    }
    for (const auto& [kind, count] : tables) {
      std::printf("%s: %d tables", kind.c_str(), count);
      for (const int op : want) std::printf(", %02X x%d", op, hits[kind][op]);
      std::printf("\n");
    }
    return 0;
  }
  // sfxscan: every battle sound of every unit: stuck voices / parked channels / long sounds.
  if (std::string(argv[i]) == "sfxscan") {
    const int wc = i + 1 < argc ? std::atoi(argv[i + 1]) : 0;
    const double secs = 40.0;
    int total = 0, stuck = 0;
    auto scan_one = [&](const std::string& label, game::BattleAudio& ba) {
      for (const std::string& tag : ba.tags()) {
        game::BattleAudio::Analysis analysis;
        if (!ba.analyse(tag, secs, analysis)) continue;
        ++total;
        if (analysis.voices_left > 0) ++stuck;
        if (analysis.voices_left > 0 || analysis.last_voice_s > 6.0 || analysis.channels_left > 0)
          std::printf("%-28s %-6s programs %d last voice %5.1f s, left: %d voices %d channels%s\n", label.c_str(), tag.c_str(), analysis.programs,
                      analysis.last_voice_s, analysis.voices_left, analysis.channels_left, analysis.voices_left > 0 ? "  STUCK" : "");
      }
    };
    if (auto battle_prd = fs.read("cdrom/battle/battle.prd")) {
      game::BattleAudio ba;
      ba.init(nullptr, &fs);
      ba.load_group("battle", *battle_prd, -1);
      ba.activate("battle");
      scan_one("battle.prd (shared)", ba);
    }
    for (const std::string& path : fs.list("cdrom")) {
      if (path.size() < 12 || path.compare(path.size() - 4, 4, ".prd") != 0 || path.rfind("battle/", 0) != 0 || path == "battle/battle.prd") continue;
      auto unit_prd = fs.read("cdrom/" + path);
      if (!unit_prd) continue;
      // The unit file alone: the shared battle.prd set is scanned once above.
      game::BattleAudio ba;
      ba.init(nullptr, &fs);
      ba.load_group("unit", *unit_prd, wc);
      ba.activate("unit");
      scan_one(path.substr(7), ba);
    }
    std::printf("%d sounds scanned, %d with a voice still sounding after %.0f s\n", total, stuck, secs);
    return 0;
  }
  if (std::string(argv[i]) == "sfxslots" && i + 3 < argc) {
    // Which instrument slots each battle sound reads and which bank the single PS1 table gives them, against a per-file table
    // (wave0000 + the sound's own file's banks). sfxslots <stage.prd> <unit folder|.prd> <weapon class> [pack .prd ...] (paths under cdrom/battle/);
    // files open in the engine's order: battle.prd, stage, unit, packs. Prints every sound that reads a slot the per-file table resolves differently.
    auto base = audio::load_akao_base_table(fs);
    struct GroupFile { std::string label; Bytes data; int wc; game::BattleAudio::Kind kind; };
    std::vector<GroupFile> files;
    auto add = [&](const std::string& path, int wc, game::BattleAudio::Kind kind) {
      if (auto bytes = fs.read("cdrom/battle/" + path)) files.push_back({path, *bytes, wc, kind});
      else std::printf("missing %s\n", path.c_str());
    };
    add("battle.prd", -1, game::BattleAudio::Kind::Resident);
    add(argv[i + 1], -1, game::BattleAudio::Kind::Resident);
    std::string unit_path = argv[i + 2];
    if (unit_path.size() < 4 || unit_path.compare(unit_path.size() - 4, 4, ".prd") != 0) unit_path += "/at0.prd";
    add(unit_path, std::atoi(argv[i + 3]), game::BattleAudio::Kind::Actor);
    for (int k = i + 4; k < argc; ++k) add(argv[k], -1, game::BattleAudio::Kind::Pack);
    std::function<void(ByteSpan, std::vector<std::shared_ptr<const audio::Bank>>&)> banks_of = [&](ByteSpan data, std::vector<std::shared_ptr<const audio::Bank>>& out) {
      auto parsed = import::parse_drp(data);
      if (!parsed) return;
      for (const import::DrpEntry& entry : parsed->entries)
        if (entry.type == 5)
          if (auto bank = audio::parse_bank(entry.data)) out.push_back(bank);
      for (const import::DrpEntry& entry : parsed->entries)
        if (entry.type == 1 && import::is_drp(entry.data)) banks_of(entry.data, out);
    };
    game::BattleAudio ba;
    ba.init(nullptr, &fs);
    ba.begin_battle();
    for (size_t k = 0; k < files.size(); ++k) {
      ba.load_group("f" + std::to_string(k), files[k].data, files[k].wc, files[k].kind);
      ba.activate("f" + std::to_string(k));
      std::vector<std::shared_ptr<const audio::Bank>> bs;
      banks_of(files[k].data, bs);
      for (const auto& bank : bs) std::printf("open %-34s bank id %u slots %u..%zu\n", files[k].label.c_str(), bank->id, bank->first_index, bank->first_index + bank->inst.size() - 1);
    }
    std::printf("live table: ");
    for (int slot = 0; slot < 256; ++slot) {
      const audio::Instrument* in = ba.instrument_table().peek(slot);
      if (in && in->bank_id != 0) std::printf("%d:%u ", slot, in->bank_id);
    }
    std::printf("(slot:bank id, wave0000 = id 0)\n");
    int nsound = 0, ndiff = 0;
    for (const std::string& tag : ba.tags()) {
      std::bitset<256> hits;
      std::vector<float> pcm;
      if (!ba.render(tag, pcm, 44100 * 4, 127, -1, &hits)) continue;
      ++nsound;
      const std::string key = ba.resolve(tag);
      const size_t fi = key.size() > 1 ? std::strtoul(key.c_str() + 1, nullptr, 10) : 0;
      std::vector<std::shared_ptr<const audio::Bank>> own;
      banks_of(files[fi].data, own);
      audio::InstrumentTable old(*base);
      for (const auto& bank : own) old.overlay(bank);
      std::string diff, used;
      for (int slot = 0; slot < 256; ++slot) {
        if (!hits[slot]) continue;
        const audio::Instrument* now = ba.instrument_table().peek(slot);
        const audio::Instrument* was = old.peek(slot);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d(%u) ", slot, now ? now->bank_id : 9999u);
        used += buf;
        const bool same = now && was && now->bank_id == was->bank_id && now->start == was->start && now->adsr1 == was->adsr1 && now->adsr2 == was->adsr2 && now->base_note == was->base_note && now->fine_tune == was->fine_tune;
        if (!same) {
          std::snprintf(buf, sizeof buf, "%d: was bank %u now %u; ", slot, was ? was->bank_id : 9999u, now ? now->bank_id : 9999u);
          diff += buf;
        }
      }
      if (!diff.empty()) ++ndiff;
      std::printf("%-8s %-3s uses %s%s%s\n", tag.c_str(), key.c_str(), used.c_str(), diff.empty() ? "" : " DIFF ", diff.c_str());
    }
    std::printf("%d sounds, %d resolve differently from the per-file table\n", nsound, ndiff);
    return 0;
  }
  // sfxbanks: the type-5 instrument banks of a battle .prd (path under cdrom/battle/): first index, count, sample bytes.
  if (std::string(argv[i]) == "sfxbanks" && i + 1 < argc) {
    auto bytes = fs.read(std::string("cdrom/battle/") + argv[i + 1]);
    auto parsed = bytes ? import::parse_drp(*bytes) : std::nullopt;
    if (!parsed) return 1;
    for (const import::DrpEntry& entry : parsed->entries) {
      if (entry.type != 5) continue;
      auto bank = audio::parse_bank(entry.data);
      std::printf("bank '%s' %zu bytes: ", entry.name.c_str(), entry.data.size());
      if (!bank) { std::printf("not a bank\n"); continue; }
      std::printf("id %u first %u count %zu\n", bank->id, bank->first_index, bank->inst.size());
      for (size_t k = 0; k < bank->inst.size(); ++k)
        std::printf("   inst %zu start 0x%X loop 0x%X fine %d base %d adsr %04X %04X pcm %zu\n", bank->first_index + k, bank->inst[k].start, bank->inst[k].loop, bank->inst[k].fine_tune, bank->inst[k].base_note, bank->inst[k].adsr1, bank->inst[k].adsr2, bank->inst[k].sample ? bank->inst[k].sample->pcm.size() : 0);
    }
    return 0;
  }
  // sfxfind: which battle sound contains this hex byte pattern ("c2a870a10a").
  if (std::string(argv[i]) == "sfxfind" && i + 1 < argc) {
    std::vector<uint8_t> pat;
    const std::string hx = argv[i + 1];
    for (size_t k = 0; k + 1 < hx.size(); k += 2) pat.push_back(static_cast<uint8_t>(std::stoi(hx.substr(k, 2), nullptr, 16)));
    auto probe = [&](const std::string& label, const Bytes& prd, int wc) {
      game::BattleAudio ba;
      ba.init(nullptr, &fs);
      ba.load_group("g", prd, wc);
      ba.activate("g");
      for (const std::string& tag : ba.find_code(pat)) std::printf("%s class %d tag %s\n", label.c_str(), wc, tag.c_str());
    };
    if (auto battle_prd = fs.read("cdrom/battle/battle.prd")) probe("battle.prd", *battle_prd, -1);
    for (const std::string& path : fs.list("cdrom")) {
      if (path.size() < 12 || path.compare(path.size() - 4, 4, ".prd") != 0 || path.rfind("battle/", 0) != 0 || path == "battle/battle.prd") continue;
      if (auto unit_prd = fs.read("cdrom/" + path))
        for (int wc = 0; wc < 8; ++wc) probe(path, *unit_prd, wc);
    }
    return 0;
  }
  if (std::string(argv[i]) == "sfx" && i + 2 < argc) {
    const std::string fd = argv[i + 1], tag = argv[i + 2];
    const int wc = i + 3 < argc ? std::atoi(argv[i + 3]) : 0;
    game::BattleAudio ba;
    ba.init(nullptr, &fs);
    if (auto battle_prd = fs.read("cdrom/battle/battle.prd")) ba.load_group("battle", *battle_prd, -1);
    if (auto unit_prd = fs.read("cdrom/battle/" + (fd.size() > 4 && fd.compare(fd.size() - 4, 4, ".prd") == 0 ? fd : fd + "/at0.prd"))) ba.load_group("unit", *unit_prd, wc);
    ba.activate("battle");
    ba.activate("unit");
    std::vector<float> pcm;
    // OC_SFX_VOL: the volume / pan the engine would pass (0x7F / pan8 / 2).
    const char* ev = std::getenv("OC_SFX_VOL");
    const char* ep = std::getenv("OC_SFX_PAN");
    if (!ba.render(tag, pcm, 44100 * 6, ev ? std::atoi(ev) : 127, ep ? std::atoi(ep) : -1)) {
      std::printf("no sound '%s'\n", tag.c_str());
      return 1;
    }
    // OC_SFX_WAV: write the rendered sound as a 44.1 kHz stereo 16-bit WAV.
    if (const char* wav = std::getenv("OC_SFX_WAV")) {
      if (FILE* wav_file = std::fopen(wav, "wb")) {
        const uint32_t data_bytes = static_cast<uint32_t>(pcm.size()) * 2, rate = 44100, byte_rate = rate * 4;
        const uint32_t riff = 36 + data_bytes, fmt = 16;
        const uint16_t pcmfmt = 1, ch = 2, align = 4, bits = 16;
        std::fwrite("RIFF", 1, 4, wav_file); std::fwrite(&riff, 4, 1, wav_file); std::fwrite("WAVEfmt ", 1, 8, wav_file); std::fwrite(&fmt, 4, 1, wav_file);
        std::fwrite(&pcmfmt, 2, 1, wav_file); std::fwrite(&ch, 2, 1, wav_file); std::fwrite(&rate, 4, 1, wav_file); std::fwrite(&byte_rate, 4, 1, wav_file);
        std::fwrite(&align, 2, 1, wav_file); std::fwrite(&bits, 2, 1, wav_file); std::fwrite("data", 1, 4, wav_file); std::fwrite(&data_bytes, 4, 1, wav_file);
        for (float sample : pcm) { const int16_t quantised = static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f); std::fwrite(&quantised, 2, 1, wav_file); }
        std::fclose(wav_file);
      }
    }
    float peak = 0;
    size_t nonzero = 0;
    for (float sample : pcm) {
      peak = std::max(peak, std::fabs(sample));
      nonzero += std::fabs(sample) > 1e-4f;
    }
    {
      game::BattleAudio::Analysis an;
      if (ba.analyse(tag, i + 4 < argc ? std::atof(argv[i + 4]) : 12.0, an))
        std::printf("analysis: %d programs, last voice %.1f s, left %d voices %d channels\n", an.programs, an.last_voice_s, an.voices_left, an.channels_left);
    }
    std::printf("%s: peak %.3f, %zu of %zu samples audible\n", tag.c_str(), static_cast<double>(peak), nonzero, pcm.size());
    return 0;
  }
  // pack <cdrom path of an effect .prd> [variant]: the pack's LAST type-0x1A table ('main' of a tech: the stock host runs entry 0 variant 0 on the
  // caster, variant 7 = the targets' reaction), the way the engine registers it.
  bool pack_mode = false;
  std::optional<Bytes> obj, prd;
  std::string folder, stem;
  if (std::string(argv[i]) == "pack" && i + 1 < argc) {
    pack_mode = true;
    folder = argv[i + 1];
    i += 2;
    prd = fs.read("cdrom/" + folder);
    if (!prd) {
      std::printf("no pack '%s'\n", folder.c_str());
      return 1;
    }
  } else {
    folder = argv[i++];
    stem = folder.substr(folder.find_last_of('/') + 1);
    obj = fs.read("cdrom/battle/" + folder + "/" + stem + ".obj");
    if (!obj && !stem.empty() && stem.back() >= '0' && stem.back() <= '9') obj = fs.read("cdrom/battle/" + folder + "/" + stem.substr(0, stem.size() - 1) + ".obj");
    prd = fs.read("cdrom/battle/" + folder + "/at0.prd");
  }
  game::bseq::Table s5, atk;
  bool has_s5 = !pack_mode && obj && game::bseq::load_model_table(*obj, s5);
  // OC_PACK_MODEL: run the S5 entry of the pack's type-0x0B model of this name (ghost1's `act0`).
  const char* pack_model = std::getenv("OC_PACK_MODEL");
  if (pack_mode && pack_model)
    if (auto drp = import::parse_drp(*prd))
      for (const auto& entry : drp->entries)
        if (entry.type == 0x0B && entry.name == pack_model) has_s5 = game::bseq::load_model_table(entry.data, s5);
  bool has_atk = false;
  if (pack_mode) {
    if (auto drp = import::parse_drp(*prd))
      for (const auto& entry : drp->entries)
        if (entry.type == 0x1A) {
          game::bseq::Table candidate;
          // OC_PACK_TABLE: pick the type-0x1A entry by name (default = the last one, normally `main`).
          const char* want = std::getenv("OC_PACK_TABLE");
          if ((!want || entry.name == want) && candidate.parse(entry.data)) {
            atk = std::move(candidate);
            has_atk = true;
            std::printf("type-0x1A entry '%s' (%d entries)\n", entry.name.c_str(), atk.entries());
          }
        } else if (entry.type == 0x19) {
          std::printf("type-0x19 anim pack '%s' (%zu bytes)\n", entry.name.c_str(), entry.data.size());
        }
  } else {
    has_atk = prd && game::bseq::load_atk_table(*prd, atk);
  }
  if (has_atk && has_s5) atk.set_model_table(&s5);
  std::printf("%s: S5 %s (%d entries), atk %s (%d entries)\n", folder.c_str(), has_s5 ? "ok" : "none", s5.entries(), has_atk ? "ok" : "none",
              atk.entries());
  const int entry = pack_mode && !pack_model ? -1 : (i < argc ? std::atoi(argv[i++]) : 1);
  const int variant = i < argc ? std::atoi(argv[i++]) : 0;
  game::bseq::Ctx ctx;
  if (i < argc) ctx.action_a = std::atoi(argv[i++]);
  if (i < argc) ctx.action_b = std::atoi(argv[i++]);
  if (i < argc) ctx.hit_code = std::atoi(argv[i++]);
  if (i < argc) ctx.dead = std::atoi(argv[i++]) != 0;
  ctx.clip_frames = [](int) { return 8; };
  // OC_SEQ_MONSTER: the script runs on an enemy (ops 0x63 / 0x67).
  if (std::getenv("OC_SEQ_MONSTER")) ctx.monster = true, ctx.model_num = 64;
  // OC_SEQ_MODEL: the model number op 0x67 reads (a PC's model byte, e.g. 50 = Glenn with two swords).
  if (const char* model = std::getenv("OC_SEQ_MODEL")) ctx.model_num = std::atoi(model);
  const bool use_atk = entry < 0 || (entry == 0 && variant == 7) || (entry == 0 && variant == 0 && !has_s5);
  const game::bseq::Table& table = use_atk ? atk : s5;
  game::bseq::Vm vm;
  // OC_SEQ_G: preset global anmseq words, "idx=val,idx=val" (index with bit 0x80, e.g. 143=1,137=0).
  if (const char* globals_spec = std::getenv("OC_SEQ_G"))
    for (std::string spec(globals_spec); !spec.empty();) {
      const size_t comma = spec.find(','), equals = spec.find('=');
      game::bseq::Vm::set_global(std::atoi(spec.c_str()), equals != std::string::npos && equals < comma ? std::atoi(spec.c_str() + equals + 1) : 1);
      spec = comma == std::string::npos ? std::string() : spec.substr(comma + 1);
    }
  if (!vm.start(&table, use_atk ? 0 : entry, variant)) {
    std::printf("no such script\n");
    return 1;
  }
  if (std::getenv("OC_SEQ_TRACE")) vm.trace = [](int pc, const s16* words, int len) {
    std::printf("      %4d:", pc);
    for (int k = 0; k < len; ++k) std::printf(" %d", words[k]);
    std::printf("\n");
  };
  std::vector<game::bseq::Event> ev;
  for (int tick = 0; tick < 1200 && vm.running(); ++tick) {
    ev.clear();
    vm.tick(ctx, ev);
    for (const auto& event : ev) {
      using EventType = game::bseq::Event;
      const char* nm[] = {"SetAnim", "Speed", "Loop", "Sfx", "Sequence", "Camera", "Damage", "React", "MoveSpeed", "MoveTime", "MoveHome", "MovePos", "Face", "Rotate", "Effect", "TrailColors", "TrailMode", "TrailFade", "TrailOn", "TrailOff", "ColorRamp", "LightPos", "LightSet", "ModelShow", "PartShow", "ModelColor", "ModelAbr", "Jump", "WeaponShow", "LightAbs", "Blink", "Eye", "Link", "SetPos", "SetRot", "Shadow", "SlowK", "ModelScale", "ScreenFade", "ModelFade", "PartScale", "PartScaleStop", "PartBlink", "PartBlinkStop", "ElemMotion", "ElemRot", "PartColor", "ElemRotRate", "ElemTilt", "ElemTiltBack", "PoseWord", "HeadLook", "HeadRelease", "TexRect", "TexDest", "TexFlip", "Done"};
      std::printf("%4d %-11s a=%d b=%d c=%d d=%d %s\n", tick, nm[event.kind], event.a, event.b, event.c, event.d, event.tag ? game::bseq::tag_str(event.tag).c_str() : "");
      (void)sizeof(EventType);
    }
  }
  return 0;
}
