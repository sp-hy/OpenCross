// occ save-* commands (no GameDir needed):
//   occ save-info <save.dat>                       container members, cards, slot count
//   occ save-slots <save.dat>                      per-slot preview fields, version, checksum and the version-gated fields
//   occ save-roundtrip <save.dat> <out.dat>        decode -> encode every block (bit-exact check), rewrite + reopen
//   occ save-blank <save.dat> <out.dat>            copy with card 1 emptied (scratch file for the save route)
#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "save/cc_save.h"

namespace {

using namespace oc;
using namespace oc::save;

const char* kCharNames[44] = {"Serge", "Kid", "Guile", "Norris", "Nikki", "Viper", "Riddel", "Karsh", "Zoah", "Marcy",
                              "Korcha", "Luccia", "Poshul", "Razzly", "Zappa", "Orcha", "Radius", "Fargo", "Macha",
                              "Glenn", "Leena", "Miki", "Harle", "Janice", "Draggy", "Starky", "Sprigg", "Mojo",
                              "Turnip", "NeoFio", "Greco", "Skelly", "Funguy", "Irenes", "Mel", "Leah", "Van", "Sneff",
                              "Steena", "Doc", "Grobyc", "Pierre", "Orlha", "Pip"};

std::string char_name(int id) { return id == 255 ? "-" : id >= 0 && id < 44 ? kCharNames[id] : "?" + std::to_string(id); }

int cmd_info(const std::string& path) {
  const SaveDat save = SaveDat::load(path);
  for (const auto& member : save.members())
    std::printf("%-16s %7zu bytes  %s\n", member.name.c_str(), member.data.size(), member.deflate ? "deflate" : "stored");
  for (int card_no = 1; card_no <= 2; ++card_no) {
    const MemCard* card = save.card(card_no);
    if (!card) continue;
    int bad = 0, ok_blocks = 0;
    const auto slots = list_slots(*card);
    for (int block_no = 1; block_no <= 15; ++block_no)
      if (!card->dir(block_no).xor_ok) ++bad;
    for (const auto& slot : slots) ok_blocks += block_checksum_ok(card->block(slot.block)) ? 1 : 0;
    std::printf("card %d: header %s, %d bad dir frames, %zu CC slots (%d checksums ok)\n", card_no, card->header_ok() ? "ok" : "BAD",
                bad, slots.size(), ok_blocks);
  }
  return 0;
}

int cmd_slots(const std::string& path) {
  const SaveDat save = SaveDat::load(path);
  for (int card_no = 1; card_no <= 2; ++card_no) {
    const MemCard* card = save.card(card_no);
    if (!card) continue;
    for (const auto& slot_ref : list_slots(*card)) {
      const ByteSpan block = card->block(slot_ref.block);
      const SaveSlot slot = decode_block(block);
      const u32 seconds = slot.playtime_frames() / 60;
      std::string leader;
      for (int i = 0; i < 12 && slot.head[head::kLeaderName + i]; ++i) {
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02X", slot.head[head::kLeaderName + i]);
        leader += hex;
      }
      const auto party = slot.party();
      std::printf("card %d slot %02d block %2d  %u:%02u:%02u  gold %u  stars %u/%u  prog 0x%02X  loc 0x%X  party %s/%s/%s  leader(sjis)=%s  ver 0x%X  cksum %s  frames 0x%X  colours %u/%u  ng+ 0x%X/0x%X\n",
                  card_no, slot_ref.slot, slot_ref.block, seconds / 3600, seconds / 60 % 60, seconds % 60, slot.gold(), slot.stars_current(),
                  slot.stars_total(), slot.progression(), slot.location(), char_name(party[0]).c_str(), char_name(party[1]).c_str(),
                  char_name(party[2]).c_str(), leader.c_str(), block_version(block), slot.corrupted ? "BAD" : "ok", slot.frames,
                  slot.chars[0].colour_bit, slot.chars[43].colour_bit, slot.unk_e1ec, slot.unk_e1f0);
    }
  }
  return 0;
}

int cmd_roundtrip(const std::string& in, const std::string& out) {
  SaveDat save = SaveDat::load(in);
  int total = 0, exact = 0;
  for (int card_no = 1; card_no <= 2; ++card_no) {
    MemCard* card = save.card(card_no);
    if (!card) continue;
    for (const auto& slot_ref : list_slots(*card)) {
      ++total;
      const ByteSpan block = card->block(slot_ref.block);
      const Bytes encoded = encode_block(decode_block(block));
      const bool same = encoded.size() == block.size() && std::equal(encoded.begin(), encoded.end(), block.begin());
      exact += same;
      if (!same) std::printf("card %d slot %02d: NOT bit-exact\n", card_no, slot_ref.slot);
    }
  }
  std::printf("%d/%d blocks bit-exact\n", exact, total);

  save.save(out);
  const SaveDat reopened = SaveDat::load(out);
  bool ok = reopened.members().size() == save.members().size();
  for (const auto& member : save.members()) {
    const ZipMember* other = reopened.member(member.name);
    if (!other || other->data != member.data) {
      ok = false;
      std::printf("member %s differs after rewrite\n", member.name.c_str());
    }
  }
  std::printf("container rewrite: %s (%zu members) -> %s\n", ok ? "reopened, all members identical" : "MISMATCH",
              reopened.members().size(), out.c_str());
  return (exact == total && ok) ? 0 : 1;
}

// A copy of a save.dat whose card 1 holds no Chrono Cross blocks (tests write their saves into it; every other member is kept).
int cmd_blank(const std::string& in, const std::string& out) {
  SaveDat save = SaveDat::load(in);
  MemCard* card = save.card(1);
  if (!card) throw std::runtime_error("no memcard1.mcr");
  for (const auto& slot_ref : list_slots(*card)) card->free_dir(slot_ref.block);
  card->fix_checksums();
  save.save(out);
  std::printf("%s: card 1 cleared (%zu CC slots left)\n", out.c_str(), list_slots(*SaveDat::load(out).card(1)).size());
  return 0;
}

}  // namespace

int occ_save_main(const std::vector<std::string>& args) {
  try {
    if (args[0] == "save-info" && args.size() == 2) return cmd_info(args[1]);
    if (args[0] == "save-slots" && args.size() == 2) return cmd_slots(args[1]);
    if (args[0] == "save-roundtrip" && args.size() == 3) return cmd_roundtrip(args[1], args[2]);
    if (args[0] == "save-blank" && args.size() == 3) return cmd_blank(args[1], args[2]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
  std::fprintf(stderr, "usage: occ (save-info <save.dat> | save-slots <save.dat> | save-roundtrip <save.dat> <out.dat> | save-blank <save.dat> <out.dat>)\n");
  return 2;
}
