// Checks for the voice lines (audio/voice.h): the voice-pack path layout, the character name table, the candidate speaker names, the mod
// folder lookup and the voice bus of the mixer. The MP3 decoder runs only when OC_TEST_MP3 names a file (no sample is shipped).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "audio/mixer.h"
#include "audio/voice.h"

using namespace oc;
using namespace oc::audio;
namespace fs = std::filesystem;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)
}  // namespace

int main() {
  // Path layout: <scene>/<dialog>-<page>-<speaker>.mp3 (scene 123, dialog 5, first page, Serge); battle lines below battle/<n>/, with a
  // speaker-less form.
  CHECK(field_voice_rel(123, 5, 0, "serge") == "123/5-0-serge.mp3");
  CHECK(field_voice_rel(0x1D6, 12, 3, "npc") == "470/12-3-npc.mp3");
  CHECK(battle_voice_rel(4, 7, 1, "serge") == "battle/4/7-1-serge.mp3");
  CHECK(battle_voice_rel(4, 7, 1, "") == "battle/4/7-1.mp3");

  // Character name table: 83 entries, a few anchors.
  CHECK(std::string(voice_char_name(0)) == "serge");
  CHECK(std::string(voice_char_name(1)) == "kid");
  CHECK(std::string(voice_char_name(19)) == "glenn");
  CHECK(std::string(voice_char_name(43)) == "pip");
  CHECK(std::string(voice_char_name(48)) == "pip");
  CHECK(std::string(voice_char_name(49)) == "lynx");
  CHECK(std::string(voice_char_name(82)) == "orcha");
  CHECK(voice_char_name(83) == nullptr && voice_char_name(-1) == nullptr);

  // Candidate names: speaker 0 / none = "npc" first, then "serge"; others by table; the tag speaker last.
  CHECK((field_voice_names(0, 0) == std::vector<std::string>{"npc", "serge"}));
  CHECK((field_voice_names(1, 1) == std::vector<std::string>{"kid"}));
  CHECK((field_voice_names(20, 1) == std::vector<std::string>{"leena", "kid"}));
  CHECK((field_voice_names(0, 10) == std::vector<std::string>{"npc", "serge", "korcha"}));
  CHECK((field_voice_names(90, 0) == std::vector<std::string>{"90"}));

  // Previous speaker (the voice packs name speaker-less E0 lines after the last speaker): its candidate comes after npc / serge / the tag; a line
  // with its own speaker ignores it; FieldVoiceNames remembers the latest speaker per room and a new room forgets it.
  CHECK((field_voice_names(0, 0, 20) == std::vector<std::string>{"npc", "serge", "leena"}));
  CHECK((field_voice_names(0, 10, 20) == std::vector<std::string>{"npc", "serge", "korcha", "leena"}));
  CHECK((field_voice_names(0, 0, 0) == std::vector<std::string>{"npc", "serge"}));
  CHECK((field_voice_names(1, 0, 20) == std::vector<std::string>{"kid"}));
  {
    FieldVoiceNames names;
    CHECK((names.rels(7, 3, 0, 20, 0) == std::vector<std::string>{"7/3-0-leena.mp3"}));
    CHECK((names.rels(7, 4, 1, 0, 0) == std::vector<std::string>{"7/4-1-npc.mp3", "7/4-1-serge.mp3", "7/4-1-leena.mp3"}));
    CHECK((names.rels(7, 5, 0, 1, 0) == std::vector<std::string>{"7/5-0-kid.mp3"}));
    CHECK((names.rels(7, 6, 0, 0, 0) == std::vector<std::string>{"7/6-0-npc.mp3", "7/6-0-serge.mp3", "7/6-0-kid.mp3"}));
    CHECK((names.rels(8, 1, 0, 0, 0) == std::vector<std::string>{"8/1-0-npc.mp3", "8/1-0-serge.mp3"}));
    CHECK((names.rels(8, 2, 0, 0, 0).size() == 2));
  }
  // fin_ rooms: lang.dat ids are 1000 + the map id (1459 = fin_06); fin_19 resolves by name to 501, its ending id is 1501.
  CHECK((field_voice_scenes(1459) == std::vector<int>{1459}));
  CHECK((field_voice_scenes(501) == std::vector<int>{1501, 501}));
  CHECK((field_voice_scenes(123) == std::vector<int>{123}));
  {
    FieldVoiceNames names;
    CHECK((names.rels(1459, 2, 0, 0, 0) == std::vector<std::string>{"1459/2-0-npc.mp3", "1459/2-0-serge.mp3"}));
    CHECK((names.rels(501, 2, 0, 1, 0) == std::vector<std::string>{"1501/2-0-kid.mp3", "501/2-0-kid.mp3"}));
  }

  CHECK(voice_unit_name("Serge") == "serge");
  CHECK(voice_unit_name("Dead Ahead") == "deadahead");
  CHECK(voice_unit_name("A/B:C") == "abc");

  // Mod lookup: <root>/voices/<rel> in search-list priority order, and a root called "voices" serves <root>/<rel>. The fixture holds an ending
  // line of fin_19 under its +1000 scene folder and an E0 line named after the previous speaker.
  const fs::path tmp = fs::temp_directory_path() / "oc_test_voice";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp / "packA" / "voices" / "123", ec);
  fs::create_directories(tmp / "packB" / "voices" / "123", ec);
  fs::create_directories(tmp / "voices" / "55", ec);
  std::ofstream(tmp / "packA" / "voices" / "123" / "5-0-serge.mp3") << "a";
  std::ofstream(tmp / "packB" / "voices" / "123" / "5-0-serge.mp3") << "b";
  std::ofstream(tmp / "packB" / "voices" / "123" / "6-0-kid.mp3") << "b";
  std::ofstream(tmp / "voices" / "55" / "1-0-npc.mp3") << "v";
  fs::create_directories(tmp / "voices" / "1501", ec);
  std::ofstream(tmp / "voices" / "1501" / "2-0-kid.mp3") << "e";
  std::ofstream(tmp / "voices" / "55" / "3-0-leena.mp3") << "p";
  {
    Mixer mixer;
    VoicePlayer player(mixer);
    player.set_search([&] { return std::vector<fs::path>{tmp / "packB", tmp / "packA", tmp / "voices"}; });
    CHECK(player.find("123/5-0-serge.mp3") == tmp / "packB" / "voices" / "123" / "5-0-serge.mp3");
    CHECK(player.find("123/6-0-kid.mp3") == tmp / "packB" / "voices" / "123" / "6-0-kid.mp3");
    CHECK(player.find("55/1-0-npc.mp3") == tmp / "voices" / "55" / "1-0-npc.mp3");
    CHECK(player.find("123/9-0-serge.mp3").empty());
    {
      auto first_hit = [&](const std::vector<std::string>& rels) {
        for (const std::string& rel : rels)
          if (!player.find(rel).empty()) return rel;
        return std::string();
      };
      FieldVoiceNames names;
      CHECK(first_hit(names.rels(501, 2, 0, 1, 0)) == "1501/2-0-kid.mp3");
      CHECK(first_hit(names.rels(55, 1, 0, 0, 0)) == "55/1-0-npc.mp3");
      names.rels(55, 2, 0, 20, 0);
      CHECK(first_hit(names.rels(55, 3, 0, 0, 0)) == "55/3-0-leena.mp3");
    }
    player.set_search([&] { return std::vector<fs::path>{tmp / "packA"}; });
    CHECK(player.find("123/5-0-serge.mp3") == tmp / "packA" / "voices" / "123" / "5-0-serge.mp3");
    // Missing line: nothing plays; disabled player ignores requests.
    player.field_page(&player, 123, 99, 0, 0, 0);
    CHECK(!player.active());
    player.set_enabled(false);
    player.play(tmp / "packA" / "voices" / "123" / "5-0-serge.mp3");
    CHECK(!player.active());
  }
  fs::remove_all(tmp, ec);

  // Voice bus: a 100-frame line is mixed with master x voice gain over two 64-frame renders (36 frames left in the second, then silence) and ends.
  {
    Mixer mixer;
    auto pcm = std::make_shared<std::vector<float>>(2 * 100, 0.5f);
    mixer.set_bus_gain(Bus::Voice, 0.5f);
    mixer.voice_play(pcm);
    std::vector<float> out(2 * 64, 0.0f);
    mixer.render(out.data(), 64);
    CHECK(mixer.voice_active());
    CHECK(std::fabs(out[0] - 0.25f) < 1e-6f && std::fabs(out[127] - 0.25f) < 1e-6f);
    std::fill(out.begin(), out.end(), 0.0f);
    mixer.render(out.data(), 64);
    CHECK(std::fabs(out[0] - 0.25f) < 1e-6f && std::fabs(out[2 * 35] - 0.25f) < 1e-6f && std::fabs(out[2 * 36]) < 1e-6f);
    CHECK(!mixer.voice_active());
    mixer.voice_play(pcm);
    mixer.voice_stop();
    std::fill(out.begin(), out.end(), 0.0f);
    mixer.render(out.data(), 64);
    CHECK(std::fabs(out[0]) < 1e-6f);
  }

  // Decoder, only with a sample.
  if (const char* mp3 = std::getenv("OC_TEST_MP3"); mp3 && *mp3) {
    std::vector<float> pcm;
    const bool ok = decode_mp3_file(mp3, pcm);
    CHECK(ok);
    CHECK(pcm.size() > 2 * 4800);
    double rms = 0;
    for (float sample : pcm) rms += double(sample) * sample;
    CHECK(!pcm.empty() && std::sqrt(rms / double(pcm.size())) > 1e-4);
    // From memory, as a packed mod's line is decoded.
    std::ifstream in(mp3, std::ios::binary);
    const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<float> from_memory;
    CHECK(decode_mp3_bytes(bytes, from_memory));
    CHECK(from_memory == pcm);
    std::printf("decoded %s: %zu frames at 48 kHz\n", mp3, pcm.size() / 2);
  } else {
    std::printf("OC_TEST_MP3 not set: MP3 decoder not exercised\n");
  }

  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("test_voice: OK\n");
  return failures ? 1 : 0;
}
