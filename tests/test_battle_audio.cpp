// BattleAudio's tag lookup against the engine's resource list: newest group first, a drp's nested drps before its own
// entries, its own entries last to first, weapon-class renames inside the actor's file only, a closed group's tags gone.
// Synthetic drps (the payloads are only checked for the 'A' signature); then the song change on the original music. Needs the game install
// for the instrument base table and the songs (SKIP without).
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/mixer.h"
#include "game/battle/battle_audio.h"
#include "platform/paths.h"
#include "vfs/vfs.h"

using namespace oc;
using namespace oc::game;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_fail;
  }
}
void p32(Bytes& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}
struct Ent {
  const char* name;
  u8 type;
  Bytes data;
};
Bytes make_drp(const std::vector<Ent>& ents) {
  Bytes drp = {'d', 'r', 'p', 0};
  p32(drp, 0);
  p32(drp, static_cast<u32>(ents.size()) * 64);
  Bytes body;
  const u32 base = 12 + 4 * static_cast<u32>(ents.size());
  for (const auto& ent : ents) {
    p32(drp, base + static_cast<u32>(body.size()));
    p32(body, 0);
    std::string name = ent.name;
    name.resize(4, '\0');
    body.insert(body.end(), name.begin(), name.end());
    body.push_back(ent.type);
    const u32 length = static_cast<u32>(ent.data.size()) * 16;
    body.push_back(static_cast<u8>(length));
    body.push_back(static_cast<u8>(length >> 8));
    body.push_back(static_cast<u8>(length >> 16));
    body.insert(body.end(), ent.data.begin(), ent.data.end());
  }
  drp.insert(drp.end(), body.begin(), body.end());
  return drp;
}
// A type 5 instrument bank: `count` instruments from slot `first`, each with base note `note` + i, 32 bytes of (silent) sample data.
Bytes make_bank(u8 id, u32 first, u32 count, s16 note) {
  Bytes bank(0x40 + count * 16 + 32, 0);
  bank[0] = 'A', bank[1] = 'K', bank[2] = 'A', bank[3] = 'O', bank[4] = id;
  auto put32 = [&](size_t at, u32 value) { for (int i = 0; i < 4; ++i) bank[at + size_t(i)] = static_cast<u8>(value >> (8 * i)); };
  put32(0x10, 0x9100), put32(0x14, 32), put32(0x18, first), put32(0x1C, count);
  for (u32 i = 0; i < count; ++i) {
    const size_t at = 0x40 + size_t(i) * 16;
    put32(at, 0), put32(at + 4, 0);
    bank[at + 10] = static_cast<u8>(note + i);
  }
  return bank;
}
Bytes akao(u8 id) {
  Bytes code(0x20, 0);
  code[0] = 'A';
  code[1] = 'K';
  code[2] = 'A';
  code[3] = 'O';
  code[4] = id;
  return code;
}
}  // namespace

int main() {
  std::string gd = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
  const auto dir = platform::find_game_dir(gd);
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  BattleAudio audio;
  audio.init(nullptr, &files);
  audio.begin_battle();

  // battle.prd: a nested "chih" with dam0 / dead; the stage: a plain "dead" of its own, loaded later (so at the head).
  const Bytes chih = make_drp({{"dam0", 0x16, akao(1)}, {"dead", 0x16, akao(2)}});
  audio.load_group("battle", make_drp({{"chih", 1, chih}}), -1);
  audio.load_group("stage", make_drp({{"dead", 0x16, akao(3)}, {"wind", 0x16, akao(4)}}), -1);
  audio.activate("battle");
  audio.activate("stage");
  check(audio.resolve("dam0") == "battle", "dam0 comes from battle.prd");
  check(audio.resolve("dead") == "stage", "the newer group wins a duplicate tag");
  check(audio.resolve("none").empty(), "unknown tag resolves nowhere");

  // The actor's file: weapon class 2 selects "2s00" -> "_s00"; the lower digit 1 is not used while a class-2 entry exists. A tag the
  // actor's file duplicates with an earlier entry resolves to the LAST one of the file; nested drps come before the file's own entries.
  const Bytes nested = make_drp({{"hit_", 0x16, akao(10)}});
  audio.load_group("unit", make_drp({{"1s00", 0x16, akao(5)}, {"2s00", 0x16, akao(6)}, {"dup_", 0x16, akao(7)}, {"dup_", 0x16, akao(8)}, {"hit_", 0x16, akao(13)}, {"sub", 1, nested}}), 2);
  audio.activate("unit");
  check(audio.resolve("_s00") == "unit", "weapon-class entry renamed to _s00");
  check(audio.resolve("1s00").empty() && audio.resolve("2s00").empty(), "the unrenamed names are not asked for");
  const auto has_id = [&](const std::string& tag, u8 id) {
    for (const std::string& found : audio.find_code({'A', 'K', 'A', 'O', id}))
      if (found == tag) return true;
    return false;
  };
  check(has_id("dup_", 8) && !has_id("dup_", 7), "a duplicate inside one file: the last entry wins");
  check(has_id("hit_", 10) && !has_id("hit_", 13), "an entry of a nested drp comes before the file's own");

  // A pack: its tag shadows nothing it does not hold; once closed the tag is gone and the older groups answer again.
  audio.load_group("fx7", make_drp({{"dead", 0x16, akao(9)}, {"se60", 0x16, akao(11)}}), -1, BattleAudio::Kind::Pack);
  audio.activate("fx7");
  check(audio.resolve("se60") == "fx7", "pack tag found while the pack is open");
  check(audio.resolve("dead") == "fx7", "pack's dead shadows the stage's while it runs");
  check(audio.resolve("_s00") == "unit", "the actor's tags stay reachable under a pack");
  audio.end_pack();
  check(audio.resolve("se60").empty(), "pack tag gone after the pack");
  check(audio.resolve("dead") == "stage", "dead is the stage's again after the pack");

  // The next actor's file replaces the previous one's.
  audio.load_group("unit2", make_drp({{"0s00", 0x16, akao(12)}}), 3);
  audio.activate("unit2");
  check(audio.resolve("_s00") == "unit2", "second actor's _s00");
  audio.end_actor();
  check(audio.resolve("_s00").empty(), "no actor file open after the action");
  check(audio.resolve("dam0") == "battle", "resident groups stay");

  // The weapon-class digit falls DOWN to the highest digit the unit's file holds (the class, then lower digits until one
  // matches), never up: Serge's file has 0 and 5 (`selju2`), so digits 0..4 -> "0s00", 5..9 -> "5s00"; a file without digit 0 and a lower request
  // renames nothing (silent, as on the PS1). The digit is the weapon's material tier, see BattleView::weapon_digit.
  {
    const std::vector<Ent> serge = {{"0s00", 0x16, akao(20)}, {"0m00", 0x16, akao(21)}, {"5s00", 0x16, akao(22)}, {"5m00", 0x16, akao(23)}};
    const auto pick = [&](int digit, u8 id, const char* tag) {
      BattleAudio battle_audio;
      battle_audio.init(nullptr, &files);
      battle_audio.begin_battle();
      battle_audio.load_group("unit", make_drp(serge), digit);
      battle_audio.activate("unit");
      for (const std::string& found : battle_audio.find_code({'A', 'K', 'A', 'O', id}))
        if (found == tag) return true;
      return false;
    };
    check(pick(0, 20, "_s00") && pick(1, 20, "_s00") && pick(4, 20, "_s00"), "digits 0..4 fall down to the 0 group");
    check(pick(5, 22, "_s00") && pick(7, 22, "_s00") && pick(9, 22, "_s00"), "digits 5..9 use the 5 group");
    check(pick(3, 21, "_m00") && pick(6, 23, "_m00"), "the medium / large hits follow the same digit");
    BattleAudio sparse_audio;
    sparse_audio.init(nullptr, &files);
    sparse_audio.begin_battle();
    sparse_audio.load_group("unit", make_drp({{"5s00", 0x16, akao(30)}}), 3);
    sparse_audio.activate("unit");
    check(sparse_audio.resolve("_s00").empty(), "no group at or below the digit: nothing is renamed");
  }

  // One instrument table per battle, overwritten in the order the files open: slots 208.. belong to the last bank loaded there,
  // closing a file restores nothing, a bank only touches its own slots.
  {
    BattleAudio bank_audio;
    bank_audio.init(nullptr, &files);
    bank_audio.begin_battle();
    const int wave_note_100 = bank_audio.instrument_table().get(100) ? bank_audio.instrument_table().get(100)->base_note : -1;
    bank_audio.load_group("battle", make_drp({{"wave", 5, make_bank(1, 176, 2, 31)}, {"dam0", 0x16, akao(1)}}), -1);
    bank_audio.load_group("stage", make_drp({{"wave", 5, make_bank(2, 208, 3, 41)}, {"wind", 0x16, akao(2)}}), -1);
    bank_audio.load_group("unit", make_drp({{"wave", 5, make_bank(3, 208, 2, 51)}, {"1s00", 0x16, akao(3)}}), 1);
    bank_audio.load_group("fx", make_drp({{"wave", 5, make_bank(4, 209, 1, 61)}, {"se60", 0x16, akao(4)}}), -1, BattleAudio::Kind::Pack);
    const auto note = [&](int slot) { const auto* instrument = bank_audio.instrument_table().get(slot); return instrument ? int(instrument->base_note) : -1; };
    check(note(208) != 41, "nothing loaded before the files open");
    bank_audio.activate("battle");
    bank_audio.activate("stage");
    check(note(176) == 31 && note(177) == 32 && note(208) == 41 && note(210) == 43, "battle.prd and the stage bank land at their first index");
    bank_audio.activate("unit");
    check(note(208) == 51 && note(209) == 52 && note(210) == 43, "the unit's bank overwrites 208..209 only; the stage's 210 stays");
    bank_audio.activate("fx");
    check(note(208) == 51 && note(209) == 61, "the pack's bank overwrites slot 209");
    bank_audio.end_pack();
    bank_audio.end_actor();
    check(note(209) == 61 && note(208) == 51, "closing a file restores nothing");
    check(note(176) == 31 && note(100) == wave_note_100, "other slots keep their instruments");
    bank_audio.activate("unit");
    check(note(209) == 52, "reopening a file loads its bank again");
    bank_audio.begin_battle();
    check(note(208) != 51 && note(176) != 31, "a new battle starts from wave0000 again");
  }

  // The song change, one tick per refresh: the old song plays on (fading) until kChangeCutTick, the new one starts at kChangeStartTick; a
  // held one stays silent until release_song(); a second request while the change runs is dropped. Original music, no output device.
  {
    audio::Mixer mixer;
    BattleAudio music;
    music.init(&mixer, &files);
    music.set_remastered(false);
    music.begin_battle();
    music.start_battle_theme(0, false);
    std::vector<float> pcm(800 * 2);
    const auto step = [&] {
      music.tick();
      mixer.render(pcm.data(), 800);
      double energy = 0;
      for (float sample : pcm) energy += double(sample) * sample;
      return energy;
    };
    step();
    const int theme = mixer.current_song();
    check(theme >= 0, "the battle theme plays");
    music.change_song(0x16, true);
    music.change_song(0x3B, false);
    for (int i = 1; i < BattleAudio::kChangeCutTick; ++i) step();
    check(mixer.current_song() == theme, "the old song plays until the cut");
    step();
    check(!mixer.playing() && music.song_changing(), "the old song stops at the cut");
    for (int i = BattleAudio::kChangeCutTick + 1; i < BattleAudio::kChangeStartTick; ++i) step();
    check(!mixer.playing(), "nothing plays between the cut and the start");
    step();
    check(!music.song_changing() && mixer.current_song() == 0x16, "the requested song starts; the second request was dropped");
    double held = 0;
    for (int i = 0; i < 120; ++i) held += step();
    check(held == 0, "a held song is silent");
    music.release_song();
    double played = 0;
    for (int i = 0; i < 240; ++i) played += step();
    check(played > 0, "the released song plays");
  }

  std::printf(g_fail ? "test_battle_audio: %d failures\n" : "test_battle_audio: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
