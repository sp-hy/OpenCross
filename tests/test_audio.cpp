// Checks for oc_audio: SAB header parse on a synthetic blob, loop-wrap maths, the BGM table, the mixer gain formula, and the AKAO side
// (ADPCM decode, ADSR envelopes, the sequencer, SFX channel allocation, pitch vs output rate, live BGM switching, battle sound rendering,
// the music gate mask, DA portamento, FE 0B sound requests, music push / pop, note lengths, held-note fine tune, LFO rate words across
// channel starts, keymap / patch-zone envelopes, drum notes under legato, fixed voices and the note-on fine tune).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#include "audio/adpcm.h"
#include "audio/akao_bank.h"
#include "audio/akao_player.h"
#include "audio/akao_seq.h"
#include "audio/akao_tables.h"
#include "audio/bgm_switch.h"
#include "audio/bgm_table.h"
#include "audio/hca_stream.h"
#include "audio/mixer.h"
#include "audio/sab.h"
#include "audio/sfx.h"

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

void put32(Bytes& bytes, size_t at, u32 value) {
  for (int i = 0; i < 4; ++i) bytes[at + size_t(i)] = u8(value >> (8 * i));
}

// SAB layout: "sabf" header, 4 section entries (snd seq trk mtrl) at 0x20 with their offsets at +8, a mtrl section with its entry count at +4
// and the entry offset (relative to the section) at +16; the material's extradata byte +0x0D is the frame obfuscation flag.
Bytes synthetic_sab() {
  Bytes bytes(0x400, 0);
  std::memcpy(bytes.data(), "sabf", 4);
  bytes[4] = 2;
  const char* tags[4] = {"snd ", "seq ", "trk ", "mtrl"};
  for (size_t i = 0; i < 4; ++i) {
    size_t section = 0x20 + i * 16;
    std::memcpy(&bytes[section], tags[i], 4);
    bytes[section + 6] = 0x10;
    put32(bytes, section + 8, 0x100 + u32(i) * 0x20);
  }
  const size_t material_section = 0x100 + 3 * 0x20;
  put32(bytes, material_section + 4, 1);
  put32(bytes, material_section + 16, 0x20);
  const size_t entry = material_section + 0x20;
  bytes[entry + 4] = 2;
  bytes[entry + 5] = 7;
  put32(bytes, entry + 8, 48000);
  put32(bytes, entry + 0x0C, 3685753);
  put32(bytes, entry + 0x10, 7398138);
  put32(bytes, entry + 0x14, 0x170);
  put32(bytes, entry + 0x1C, 0x45);
  bytes[entry + 0x20 + 0x0D] = 1;
  return bytes;
}

// The synthetic SAB parses to one material with its codec, loop, extradata and offsets; a bad magic, a truncated file and an extradata size
// beyond EOF are refused; a valid container without an HCA header inside does not open as a stream.
void test_sab() {
  Bytes bytes = synthetic_sab();
  auto sab = audio::parse_sab(bytes);
  CHECK(sab && sab->materials.size() == 1);
  if (!sab) return;
  const auto& material = sab->materials[0];
  CHECK(material.channels == 2 && material.codec == 7 && material.sample_rate == 48000);
  CHECK(material.loop_start == 3685753 && material.loop_end == 7398138 && material.loops());
  CHECK(material.sound_id == 0x45 && material.extradata_size == 0x170 && material.encryption == 1);
  const u32 entry = 0x100 + 3 * 0x20 + 0x20;
  CHECK(material.hca_offset == entry + 0x30);
  CHECK(material.data_offset == entry + 0x20 + 0x170);

  Bytes bad = bytes;
  bad[0] = 'x';
  CHECK(!audio::parse_sab(bad));
  CHECK(!audio::parse_sab(ByteSpan(bytes.data(), 0x40)));
  Bytes trunc = bytes;
  put32(trunc, 0x100 + 3 * 0x20 + 0x20 + 0x14, 0x7FFFFFF);
  CHECK(!audio::parse_sab(trunc));
  audio::HcaStream stream;
  CHECK(!stream.open(bytes));
}

// loop_end is inclusive: from pos 10 with loop_end 19 there are 10 frames before the wrap, at 20 the caller wraps to loop_start.
void test_loop_math() {
  CHECK(audio::frames_until_wrap(10, 19) == 10);
  CHECK(audio::frames_until_wrap(19, 19) == 1);
  CHECK(audio::frames_until_wrap(20, 19) == 0);
  // BGM002: reading 100 frames starting 40 before the end consumes 40, then 60 from loop_start.
  u32 pos = 7398138 - 39, loop_start = 3685753, loop_end = 7398138, left = 100;
  while (left) {
    if (pos > loop_end) pos = loop_start;
    u32 step = std::min(left, audio::frames_until_wrap(pos, loop_end));
    pos += step;
    left -= step;
  }
  CHECK(pos == loop_start + 60);
}

void test_table() {
  CHECK(audio::bgm_entry(0)->file_no == 1);
  CHECK(audio::bgm_entry(1)->file_no == 2 && audio::bgm_entry(1)->total_samples == 7398139);
  CHECK(audio::bgm_entry(24)->file_no == 67 && audio::bgm_entry(26)->file_no == 68);
  CHECK(!audio::bgm_entry(68) && !audio::bgm_entry(-1));
  CHECK(!audio::bgm_entry_by_file(25) && !audio::bgm_entry_by_file(73));
  CHECK(audio::bgm_path(2) == "cdrom/sound/bgm/BGM002.sab");
  for (int i = 0; i < audio::kBgmSongCount; ++i) CHECK(audio::bgm_entry(i)->song_id == i);
  // Battle song of the A3 / C9 flags: 0x40 = the boss theme on both sources (des_b06f's Fire Dragon C9 #453 carries flags 0x40).
  CHECK(audio::battle_song(0, false) == 2 && audio::battle_song(0, true) == 0x14);
  CHECK(audio::battle_song(0x40, false) == 0x31 && audio::battle_song(0x40, true) == 0x31 && audio::battle_song(0x70, true) == 0x31);
  CHECK(audio::bgm_entry(audio::kBossBattleSong)->file_no == 50);
}

void test_mixer_gain() {
  CHECK(std::fabs(audio::bgm_gain(127) - 0.45f) < 1e-6f);
  CHECK(audio::bgm_gain(0) == 0.0f);
  CHECK(std::fabs(audio::bgm_gain(64, 0.5f, 0.8f) - 64 * 0.45f / 127 * 0.4f) < 1e-6f);
  audio::Mixer mixer;
  CHECK(mixer.bus_gain(audio::Bus::Master) == 1.0f && mixer.bus_gain(audio::Bus::Bgm) == 1.0f);
  mixer.set_bus_gain(audio::Bus::Bgm, 0.25f);
  CHECK(mixer.bus_gain(audio::Bus::Bgm) == 0.25f);
  // No song: silence, commands do not crash, state stays idle.
  mixer.fade(0, 1.0);
  mixer.pause();
  mixer.resume();
  mixer.stop();
  std::vector<float> out(512 * 2, 1.0f);
  mixer.render(out.data(), 512);
  bool silent = true;
  for (float sample : out) silent = silent && sample == 0.0f;
  CHECK(silent && !mixer.playing() && mixer.current_song() == -1);
}

// ---- AKAO: ADPCM, ADSR, sequencer ------------------------------------------------------------------------------
// Block 0 (shift 12, filter 0, loop-start flag): the sample is the nibble exactly, low nibble first, so bytes 0x21 0x43 give 1 2 3 4 and 0xF8
// gives -8 -1, then 7 7 ... Block 1 (filter 1 = 60/64 of the previous sample, end + repeat flags, zero nibbles) keeps the DC value 7. The loop
// body (blocks 0..1) is decoded a second time from the filter history at the end of the sample and appended; the voice jumps to that copy
// (the SPU keeps the ADPCM history across the jump), so block 0's nibbles repeat at 56. Shift 0 turns nibble 7 into 28672 and filter 2
// (115, -52) then saturates at 32767. Data that runs out without an end flag decodes what exists and reports it.
void test_adpcm() {
  Bytes data(32, 0);
  data[0] = 12;
  data[1] = 4;
  data[2] = 0x21;
  data[3] = 0x43;
  data[4] = 0xF8;
  for (size_t i = 5; i < 16; ++i) data[i] = 0x77;
  data[16] = 12 | (1 << 4);
  data[17] = 3;
  auto sample = audio::adpcm_decode_sample(data, 0, 0);
  CHECK(sample.pcm.size() == 112 && sample.has_end && sample.loops && sample.loop_start == 56);
  if (sample.pcm.size() != 112) return;
  CHECK(sample.pcm[0] == 1 && sample.pcm[1] == 2 && sample.pcm[2] == 3 && sample.pcm[3] == 4);
  CHECK(sample.pcm[4] == -8 && sample.pcm[5] == -1 && sample.pcm[6] == 7);
  CHECK(sample.pcm[27] == 7);
  CHECK(sample.pcm[28] == ((7 * 60 + 32) >> 6));
  CHECK(sample.pcm[56] == 1 && sample.pcm[62] == 7);
  Bytes loud_data(16, 0x77);
  loud_data[0] = 0 | (2 << 4);
  loud_data[1] = 1;
  auto loud = audio::adpcm_decode_sample(loud_data, 0, 0);
  CHECK(loud.pcm.size() == 28 && !loud.loops && loud.has_end);
  CHECK(loud.pcm[0] == 28672);
  CHECK(loud.pcm[1] == std::min(32767, 28672 + ((28672 * 115 + 32) >> 6)));
  Bytes endless_data(16, 0);
  auto endless = audio::adpcm_decode_sample(endless_data, 0, 0);
  CHECK(endless.pcm.size() == 28 && !endless.has_end);
}

// Attack (linear, shift 0, step +7 -> 7<<11 = 14336 per tick) reaches 0x7FFF on the 3rd tick, then the exponential decay (each step subtracts
// level * 16384 / 32768 at shift 0) runs until at/below (SL+1)*0x800. Release at shift 11 is -8 per tick (100 ticks drop exactly 800), shift
// 13 waits 4 ticks per step (8 ticks = 2 steps of -8), shift 0 is -8<<11 per tick and always ends at 0.
void test_adsr() {
  using Env = audio::AkaoPlayer::Env;
  Env env;
  env.key_on(0x0001, 0x1F00);
  env.tick();
  CHECK(env.level == 14336 && env.phase == 0);
  env.tick();
  CHECK(env.level == 28672);
  env.tick();
  CHECK(env.level == 0x7FFF && env.phase == 1);
  int ticks = 0;
  while (env.phase == 1 && ticks < 32) env.tick(), ++ticks;
  CHECK(env.phase == 2 && env.level <= 0x1000 && env.level > 0 && ticks >= 3 && ticks <= 6);
  Env release;
  release.level = 0x7FFF;
  release.phase = 2;
  release.adsr2 = 11;
  release.key_off();
  for (int i = 0; i < 100; ++i) release.tick();
  CHECK(release.level == 0x7FFF - 800);
  Env slow_release;
  slow_release.level = 0x7FFF;
  slow_release.phase = 2;
  slow_release.adsr2 = 13;
  slow_release.key_off();
  for (int i = 0; i < 8; ++i) slow_release.tick();
  CHECK(slow_release.level == 0x7FFF - 16);
  Env fast_release;
  fast_release.level = 100;
  fast_release.adsr2 = 0;
  fast_release.key_off();
  fast_release.tick();
  CHECK(fast_release.level == 0);
}

// Instrument bank image with one looping square-ish sample: instrument 0 at 0x40 (sample start / loop 0, base note 60 as s16 LE at +10,
// ADSR1 0x000F = attack fastest + sustain level 15, ADSR2 0x1F00 = sustain shift 31 increase (holds) + release shift 0), then 16 bytes of
// ADPCM at shift 2 / filter 0 with the end + repeat + loop-start flags and +7 / -7 alternating nibbles (+-7168).
Bytes tiny_bank() {
  Bytes bytes(0x40 + 16 + 16, 0);
  std::memcpy(bytes.data(), "AKAO", 4);
  bytes[4] = 99;
  put32(bytes, 0x10, 0x1100);
  put32(bytes, 0x14, 16);
  put32(bytes, 0x18, 0);
  put32(bytes, 0x1C, 1);
  const size_t inst = 0x40;
  put32(bytes, inst, 0);
  put32(bytes, inst + 4, 0);
  bytes[inst + 10] = 60;
  bytes[inst + 12] = 0x0F;
  bytes[inst + 13] = 0x00;
  bytes[inst + 14] = 0x00;
  bytes[inst + 15] = 0x1F;
  const size_t sample = 0x40 + 16;
  bytes[sample] = 2;
  bytes[sample + 1] = 7;
  for (size_t k = 2; k < 16; ++k) bytes[sample + k] = 0x79;
  return bytes;
}

// A two-note program (tempo 0x8000 = 120 ticks/s; octave 5 so note 60 is the base; C then E, 24 ticks each, op = key * 11 + length index 3):
// both notes sound (the SPU Gaussian filter keeps ~40% of this Nyquist-rate square), the gate keys off 2 ticks (16.7 ms) before the next
// note, the last audible frame is close to 0.4 s - 2 ticks (0.3833 s) + the fast release, and the program ends after ~120 ticks. A looping
// SFX (`C8 <note> CA`, the opn_a01i lift rumble) runs until the next field PLAY_SFX evicts it; an E0-protected one only ends at its next
// loop pass.
void test_sequence() {
  auto bank = audio::parse_bank(tiny_bank());
  CHECK(bank && bank->inst.size() == 1 && bank->inst[0].sample && bank->inst[0].sample->loops);
  if (!bank) return;
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);

  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0x2F, 0xA0};
  CHECK(audio::akao_op_length(&code->data[0], code->data.size()) == 4);
  CHECK(audio::akao_op_length(&code->data[10], 3) == 1 && audio::akao_op_length(&code->data[12], 1) == 0);

  audio::AkaoPlayer player(44100);
  player.start_program(code, 0, table);
  const u32 frames = 44100;
  std::vector<float> out(size_t(frames) * 2);
  player.render(out.data(), frames);
  auto win_rms = [&](double t0, double t1) {
    double acc = 0;
    size_t begin = size_t(t0 * 44100), end = size_t(t1 * 44100);
    for (size_t i = begin; i < end; ++i) acc += double(out[i * 2]) * out[i * 2] + double(out[i * 2 + 1]) * out[i * 2 + 1];
    return std::sqrt(acc / double(2 * (end - begin)));
  };
  const double loud1 = win_rms(0.05, 0.17), loud2 = win_rms(0.25, 0.37);
  CHECK(loud1 > 0.01 && loud2 > 0.01);
  CHECK(win_rms(0.184, 0.194) < loud1 * 0.05);
  CHECK(win_rms(0.45, 0.95) < 1e-6);
  size_t last = 0;
  for (size_t i = 0; i < size_t(frames); ++i)
    if (std::fabs(out[i * 2]) > 1e-4f || std::fabs(out[i * 2 + 1]) > 1e-4f) last = i;
  CHECK(last > size_t(0.370 * 44100) && last < size_t(0.405 * 44100));
  CHECK(player.finished() && player.ticks() >= 119 && player.ticks() <= 121);
  audio::AkaoPlayer player2(44100);
  player2.start_program(code, 0, table);
  player2.render(out.data(), 1000);
  CHECK(player2.active_voices() == 1);

  for (const bool prot : {false, true}) {
    auto loop = std::make_shared<audio::AkaoCode>();
    loop->data = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F};
    if (prot) loop->data.push_back(0xE0);
    for (u8 byte : {u8{0xC8}, u8{0x03}, u8{0xCA}}) loop->data.push_back(byte);
    audio::AkaoPlayer sfx_player(44100, true);
    sfx_player.start_program(loop, 0, table);
    sfx_player.render(out.data(), frames);
    CHECK(sfx_player.active_channels() == 1);
    sfx_player.evict_sfx();
    if (!prot) CHECK(sfx_player.active_channels() == 0);
    sfx_player.render(out.data(), frames);
    CHECK(sfx_player.active_channels() == 0 && sfx_player.finished());
  }
}

// SFX pool through the mixer: starting a program produces output without a device (commands are applied at the start of render()), stop
// clears it; a masked program's active mask is visible at once (queued) and after the start, a volume modifier of 0 silences it while it keeps
// running, and a masked eviction ends it.
void test_akao_mixer() {
  auto bank = audio::parse_bank(tiny_bank());
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0xA0};
  audio::SfxProgram program;
  program.code = code;
  program.pc[0] = 0;
  program.count = 1;
  program.table = table;
  audio::Mixer mixer;
  mixer.play_sfx(program);
  std::vector<float> out(2048 * 2);
  mixer.render(out.data(), 2048);
  mixer.render(out.data(), 2048);
  double energy = 0;
  for (float sample : out) energy += double(sample) * sample;
  CHECK(energy > 0.0);
  mixer.stop_sfx();
  mixer.render(out.data(), 2048);
  mixer.render(out.data(), 2048);
  energy = 0;
  for (float sample : out) energy += double(sample) * sample;
  CHECK(energy == 0.0);

  program.mask = 0x10000;
  audio::SfxProgram masked = program;
  masked.code = std::make_shared<audio::AkaoCode>();
  const_cast<audio::AkaoCode&>(*masked.code).data = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC8, 0x03, 0xCA};
  mixer.play_sfx(masked);
  CHECK(mixer.sfx_active(0x10000) && !mixer.sfx_active(0x20000));
  mixer.render(out.data(), 2048);
  CHECK(mixer.sfx_active(0x10000));
  mixer.set_sfx_mods(0x10000, 0, -1);
  mixer.render(out.data(), 2048);
  mixer.render(out.data(), 2048);
  energy = 0;
  for (float sample : out) energy += double(sample) * sample;
  CHECK(energy == 0.0 && mixer.sfx_active(0x10000));
  mixer.evict_sfx(0x10000);
  mixer.render(out.data(), 2048);
  CHECK(!mixer.sfx_active(0x10000));
}

// Monaural output: an SPU voice takes the pan table's centre gain on both sides whatever its pan, so a hard-panned effect (AA 00:
// 24576 / 276) sounds on both sides at 17418 / 24576 of its loud side, not at the average of the two.
void test_mono_voice() {
  auto bank = audio::parse_bank(tiny_bank());
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xAA, 0x00, 0x03, 0xA0};
  audio::SfxProgram program;
  program.code = code;
  program.count = 1;
  program.table = table;
  auto side_energy = [&](bool mono, double& left, double& right, bool& equal) {
    audio::Mixer mixer;
    mixer.set_mono(mono);
    mixer.play_sfx(program);
    left = right = 0;
    equal = true;
    std::vector<float> out(2048 * 2);
    for (int block = 0; block < 4; ++block) {
      mixer.render(out.data(), 2048);
      for (size_t i = 0; i < out.size(); i += 2) {
        left += double(out[i]) * out[i];
        right += double(out[i + 1]) * out[i + 1];
        equal = equal && out[i] == out[i + 1];
      }
    }
  };
  double stereo_left, stereo_right, mono_left, mono_right;
  bool stereo_equal, mono_equal;
  side_energy(false, stereo_left, stereo_right, stereo_equal);
  side_energy(true, mono_left, mono_right, mono_equal);
  CHECK(!stereo_equal && mono_equal);
  const double loud = std::max(stereo_left, stereo_right);
  CHECK(loud > 0.0 && std::min(stereo_left, stereo_right) < loud * 0.01);
  CHECK(std::fabs(std::sqrt(mono_left / loud) - 17418.0 / 24576.0) < 0.01);
}

// SFX channel allocation and stealing: 12 channels, a single program takes the highest free channel, a pair the highest free adjacent pair,
// with none free the oldest mask-0 channels are stolen (never one younger than its third tick, never a masked or E0-protected one; the high
// mask bits above the PS1 mask do not protect) and a request that frees nothing is dropped; channel k plays on hardware voice 12 + k.
void test_sfx_channels() {
  auto bank = audio::parse_bank(tiny_bank());
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto make = [&](bool prot, u32 mask) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F};
    if (prot) code->data.push_back(0xE0);
    for (u8 byte : {u8{0xC8}, u8{0x03}, u8{0xCA}}) code->data.push_back(byte);
    audio::SfxProgram program;
    program.code = code;
    program.count = 1;
    program.table = table;
    program.mask = mask;
    return program;
  };
  std::vector<float> out(2048 * 2);
  // Highest free channel first; with channel 11 busy a pair goes to 9 + 10.
  {
    audio::AkaoPlayer player(44100, true);
    player.start_sfx(make(false, 0));
    CHECK(player.busy_channels() == 0x800);
    audio::SfxProgram pair = make(false, 0);
    pair.pc[1] = 3;
    pair.count = 2;
    player.start_sfx(pair);
    CHECK(player.busy_channels() == 0x800 + 0x600);
  }
  // Saturation: each render is ~4 SFX ticks, so every earlier channel is older; the 13th request steals the oldest mask-0 channel (the first
  // one started = channel 11), the next steal takes channel 10 and never the fresh channel 11; a pair needs two adjacent channels, so
  // stealing frees one channel at a time until a pair exists.
  {
    audio::AkaoPlayer player(44100, true);
    for (int i = 0; i < 12; ++i) {
      player.start_sfx(make(false, 0));
      player.render(out.data(), 2048);
    }
    CHECK(player.busy_channels() == 0xFFF && player.dropped_requests() == 0);
    player.start_sfx(make(false, 0));
    CHECK(player.stolen_channels() == 1 && player.dropped_requests() == 0 && player.busy_channels() == 0xFFF);
    player.start_sfx(make(false, 0));
    CHECK(player.stolen_channels() == 2 && player.busy_channels() == 0xFFF);
    audio::SfxProgram pair = make(false, 0);
    pair.pc[1] = 3;
    pair.count = 2;
    player.render(out.data(), 2048);
    player.start_sfx(pair);
    CHECK(player.dropped_requests() == 0 && player.stolen_channels() >= 4 && player.busy_channels() == 0xFFF);
  }
  // Channels younger than their third tick are not stolen (the maximum age starts at 0): the request is dropped.
  {
    audio::AkaoPlayer player(44100, true);
    for (int i = 0; i < 12; ++i) player.start_sfx(make(false, 0));
    player.start_sfx(make(false, 0));
    CHECK(player.dropped_requests() == 1 && player.stolen_channels() == 0 && player.busy_channels() == 0xFFF);
  }
  // Masked sounds (menu / field) and E0-protected sounds are never freed by the steal: dropped.
  for (const bool prot : {false, true}) {
    audio::AkaoPlayer player(44100, true);
    for (int i = 0; i < 12; ++i) {
      player.start_sfx(make(prot, prot ? 0 : 0x40));
      player.render(out.data(), 2048);
    }
    player.start_sfx(make(false, 0));
    CHECK(player.dropped_requests() == 1 && player.stolen_channels() == 0 && player.busy_channels() == 0xFFF);
  }
  // The high mask bits (OpenCross file key, e.g. the battle tag) are not part of the PS1 mask: such a sound is stealable.
  {
    audio::AkaoPlayer player(44100, true);
    for (int i = 0; i < 12; ++i) {
      player.start_sfx(make(false, 0x80000000u));
      player.render(out.data(), 2048);
    }
    player.start_sfx(make(false, 0));
    CHECK(player.stolen_channels() == 1 && player.dropped_requests() == 0);
  }
  // 12 simultaneous notes use voices 12..23 and nothing else.
  {
    audio::AkaoPlayer player(44100, true);
    for (int i = 0; i < 12; ++i) player.start_sfx(make(false, 0));
    for (int k = 0; k < 4; ++k) player.render(out.data(), 2048);
    CHECK(player.active_voices() == 12);
  }
}

// A room yos0 subfile holding one bank: tag, size - 8, count 1, the block's start / end offsets relative to byte 8, the block.
Bytes room_yos0(const Bytes& bank) {
  Bytes bytes(20, 0);
  std::memcpy(bytes.data(), "yos0", 4);
  put32(bytes, 4, u32(bytes.size() + bank.size() - 8));
  put32(bytes, 8, 1);
  put32(bytes, 12, 12);
  put32(bytes, 16, u32(12 + bank.size()));
  bytes.insert(bytes.end(), bank.begin(), bank.end());
  return bytes;
}

// tiny_bank() with its one instrument at `first`, uploaded to `spu`, its sample's data nibbles all `nibbles`.
Bytes tiny_bank_at(u32 first, u32 spu, u8 nibbles) {
  Bytes bytes = tiny_bank();
  put32(bytes, 0x10, spu);
  put32(bytes, 0x18, first);
  for (size_t k = 2; k < 16; ++k) bytes[0x40 + 16 + k] = nibbles;
  return bytes;
}

// The banks share one table and one sound RAM: a room's bank overwrites only its own slots of the table the earlier loaders left, and a
// room without banks keeps it (after a room whose bank fills slot 208, a bankless room still plays that instrument there). A later upload
// to the same RAM leaves an earlier slot's entry but makes it play the new bytes (208 keeps its bank id, decodes the bank at 209's
// sample); wave0000's RAM elsewhere stays untouched. reload_room uploads the room's banks again over a table another loader published:
// that loader's slot on the same RAM (176) keeps its entry and plays the room's bytes.
void test_sfx_room_banks() {
  Bytes effect00(0x820, 0);
  std::memcpy(effect00.data(), "AKAO", 4);
  for (size_t i = 0; i < 256; ++i) put32(effect00, 0x10 + i * 4, 0xFFFFFFFFu);
  put32(effect00, 0x10, 0xFFFF0000u);
  audio::SfxBank bank;
  CHECK(bank.load(effect00, tiny_bank_at(0, 0x71100, 0x79)));
  auto slots = [&](int index) { return bank.table()->peek(index); };
  const audio::Instrument* wave0000 = slots(0);
  CHECK(wave0000 != nullptr && slots(208) == nullptr);
  bank.set_room(room_yos0(tiny_bank_at(208, 0x1100, 0x79)), {});
  const audio::Instrument* room = slots(208);
  CHECK(room != nullptr && slots(0) == wave0000);
  bank.set_room({}, {});
  CHECK(slots(208) == room && slots(0) == wave0000);
  const std::shared_ptr<const audio::InstrumentTable> before = bank.table();
  bank.set_room(room_yos0(tiny_bank_at(209, 0x1100, 0x17)), {});
  const audio::Instrument* later = slots(209);
  CHECK(later && later->sample && slots(0) == wave0000);
  const audio::Instrument* reached = slots(208);
  CHECK(reached && reached != room && reached->overwritten && reached->bank_id == room->bank_id && reached->start == room->start);
  CHECK(reached && reached->sample && later && later->sample && reached->sample->pcm == later->sample->pcm && reached->sample->pcm != room->sample->pcm);
  CHECK(before->peek(208) == room);
  auto other = std::make_shared<audio::InstrumentTable>(*bank.table());
  other->overlay(audio::parse_bank(tiny_bank_at(176, 0x1100, 0x35)));
  bank.reload_room(other);
  const audio::Instrument* kept = slots(176);
  CHECK(kept && kept->overwritten && kept->sample && slots(209)->sample && kept->sample->pcm == slots(209)->sample->pcm);
}

// A battle ends and the field leaves the room before any sound played (no resume yet): the next room, bankless like the world map, still
// inherits the room's banks and not what the battle left over the same RAM (the forest sound after Opassa read slot 209).
void test_sfx_room_leave_after_battle() {
  Bytes effect00(0x820, 0);
  std::memcpy(effect00.data(), "AKAO", 4);
  for (size_t i = 0; i < 256; ++i) put32(effect00, 0x10 + i * 4, 0xFFFFFFFFu);
  put32(effect00, 0x10, 0xFFFF0000u);
  audio::SfxBank bank;
  CHECK(bank.load(effect00, tiny_bank_at(0, 0x71100, 0x79)));
  bank.set_room(room_yos0(tiny_bank_at(209, 0x1100, 0x79)), {});
  const audio::Instrument* room = bank.table()->peek(209);
  CHECK(room && room->sample);
  auto battle = std::make_shared<audio::InstrumentTable>(*bank.table());
  battle->overlay(audio::parse_bank(tiny_bank_at(210, 0x1100, 0x35)));
  const audio::Instrument* clobbered = battle->peek(209);
  CHECK(clobbered && clobbered->overwritten && clobbered->sample && clobbered->sample->pcm != room->sample->pcm);
  bank.set_room({}, {}, battle);
  const audio::Instrument* inherited = bank.table()->peek(209);
  CHECK(inherited && !inherited->overwritten && inherited->sample && inherited->sample->pcm == room->sample->pcm);
  const audio::Instrument* battle_slot = bank.table()->peek(210);
  CHECK(battle_slot && battle_slot->overwritten && battle_slot->sample && battle_slot->sample->pcm == room->sample->pcm);
  const std::shared_ptr<const audio::InstrumentTable> kept = bank.table();
  bank.set_room({}, {}, kept);
  CHECK(bank.table() == kept);
}

// The SPU voice reads the sound RAM live: a held note on instrument 0 (uploaded at 0x1100) keeps sounding when a later bank for slot 1
// is uploaded over the same RAM, and from then on plays the new bytes - the same output as a note started on those bytes; a note that
// starts after the upload on the older table plays them too. Without the upload the note keeps the first bytes.
void test_sfx_live_ram() {
  auto first = std::make_shared<audio::InstrumentTable>();
  first->overlay(audio::parse_bank(tiny_bank_at(0, 0x1100, 0x79)));
  auto second = std::make_shared<audio::InstrumentTable>(*first);
  second->overlay(audio::parse_bank(tiny_bank_at(1, 0x1100, 0x35)));
  auto reference = std::make_shared<audio::InstrumentTable>();
  reference->overlay(audio::parse_bank(tiny_bank_at(0, 0x1100, 0x35)));
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xA1, 0x00, 0xA5, 0x06, 0xA8, 0x7F, 0xF0, 0xC0, 0xA0};
  constexpr u32 kFrames = 4096;
  auto run = [&](const std::shared_ptr<const audio::InstrumentTable>& table, bool upload_midway, bool upload_first) {
    audio::AkaoPlayer player(44100, true);
    if (upload_first) player.upload_ram(second->ram());
    player.start_program(code, 0, table);
    std::vector<float> out(size_t(kFrames) * 2);
    player.render(out.data(), kFrames);
    if (upload_midway) player.upload_ram(second->ram());
    player.render(out.data(), kFrames);
    return out;
  };
  auto max_diff = [](const std::vector<float>& lhs, const std::vector<float>& rhs) {
    float diff = 0;
    for (size_t i = 200; i < lhs.size(); ++i) diff = std::max(diff, std::fabs(lhs[i] - rhs[i]));
    return diff;
  };
  const std::vector<float> live = run(first, true, false), kept = run(first, false, false), after = run(first, false, true);
  const std::vector<float> expected = run(reference, false, false);
  CHECK(max_diff(live, expected) < 1e-4f);
  CHECK(max_diff(after, expected) < 1e-4f);
  CHECK(max_diff(kept, expected) > 1e-2f);
}

// A8 / A9 volumes are signed bytes: A8 0xC0 (-64) plays the A8 0x40 note phase-inverted, so a +0x40 / 0xC0 pair cancels; an A9 slide to
// 0xC0 ends at the same inverted volume.
void test_signed_volume() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank_at(0, 0x1100, 0x79)));
  auto render = [&](std::vector<u8> volume_ops) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = {0xA1, 0x00, 0xA5, 0x06};
    code->data.insert(code->data.end(), volume_ops.begin(), volume_ops.end());
    code->data.insert(code->data.end(), {0xF0, 0xC0, 0xA0});
    audio::AkaoPlayer player(44100, true);
    player.start_program(code, 0, table);
    std::vector<float> out(size_t(8192) * 2);
    player.render(out.data(), 8192);
    return out;
  };
  const std::vector<float> positive = render({0xA8, 0x40}), negative = render({0xA8, 0xC0}), slid = render({0xA8, 0x40, 0xA9, 0x01, 0xC0});
  float peak = 0, residual = 0, slide_residual = 0;
  for (size_t i = 4096; i < positive.size(); ++i) {
    peak = std::max(peak, std::fabs(positive[i]));
    residual = std::max(residual, std::fabs(positive[i] + negative[i]));
    slide_residual = std::max(slide_residual, std::fabs(slid[i] - negative[i]));
  }
  CHECK(peak > 1e-2f);
  CHECK(residual < 1e-5f);
  CHECK(slide_residual < 1e-5f);
}

// FE 19 arms a volume slide that every note key-on starts from its signed start volume: with start = target it plays the note at that volume
// whatever the channel volume was (positive or phase-inverted), and A8 / A9 cancel it.
void test_keyon_volume_slide() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank_at(0, 0x1100, 0x79)));
  auto render = [&](std::vector<u8> volume_ops) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = {0xA1, 0x00, 0xA5, 0x06};
    code->data.insert(code->data.end(), volume_ops.begin(), volume_ops.end());
    code->data.insert(code->data.end(), {0xF0, 0xC0, 0xA0});
    audio::AkaoPlayer player(44100, true);
    player.start_program(code, 0, table);
    std::vector<float> out(size_t(8192) * 2);
    player.render(out.data(), 8192);
    return out;
  };
  auto max_diff = [](const std::vector<float>& lhs, const std::vector<float>& rhs) {
    float diff = 0;
    for (size_t i = 4096; i < lhs.size(); ++i) diff = std::max(diff, std::fabs(lhs[i] - rhs[i]));
    return diff;
  };
  const std::vector<float> plain = render({0xA8, 0x40}), inverted = render({0xA8, 0xC0}), loud = render({0xA8, 0x7F});
  CHECK(max_diff(render({0xA8, 0x7F, 0xFE, 0x19, 0x40, 0x01, 0x40}), plain) < 1e-5f);
  CHECK(max_diff(render({0xA8, 0x7F, 0xFE, 0x19, 0xC0, 0x01, 0xC0}), inverted) < 1e-5f);
  CHECK(max_diff(render({0xA8, 0x7F, 0xFE, 0x19, 0x40, 0x01, 0x40, 0xA8, 0x7F}), loud) < 1e-5f);
  float peak = 0;
  for (size_t i = 4096; i < loud.size(); ++i) peak = std::max(peak, std::fabs(loud[i]));
  CHECK(peak > 1e-2f);
  CHECK(max_diff(render({0xA8, 0x7F, 0xFE, 0x19, 0x40, 0x01, 0x40}), loud) > 0.2f * peak);
}

// Pitch is a property of the SPU model, not of the output rate: a 14-sample square (+7 x7, -7 x7 nibbles, low nibble first; the decoded
// sample is 28 frames + the loop block again) played one octave above the base note (C6 = note 72 = base + 12, 192 ticks) sounds at
// 2 * 44100 / 14 = 6300 Hz whether the player renders at 44.1 kHz or 48 kHz, measured by zero crossings over 0.1 .. 0.5 s; E1 random pitch
// moves the note by the driver's random byte.
void test_pitch_rate() {
  Bytes bytes(0x40 + 16 + 16, 0);
  std::memcpy(bytes.data(), "AKAO", 4);
  put32(bytes, 0x14, 16);
  put32(bytes, 0x1C, 1);
  bytes[0x40 + 10] = 60;
  bytes[0x40 + 12] = 0x0F;
  bytes[0x40 + 15] = 0x1F;
  const size_t sample = 0x40 + 16;
  bytes[sample] = 2;
  bytes[sample + 1] = 7;
  for (size_t k = 0; k < 14; ++k) {
    const size_t i0 = 2 * k, i1 = 2 * k + 1;
    const u8 lo = (i0 / 7) % 2 ? 0x9 : 0x7, hi = (i1 / 7) % 2 ? 0x9 : 0x7;
    bytes[sample + 2 + k] = u8(lo | hi << 4);
  }
  auto bank = audio::parse_bank(bytes);
  CHECK(bank && bank->inst[0].sample && bank->inst[0].sample->pcm.size() == 56 && bank->inst[0].sample->loop_start == 28);
  if (!bank) return;
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xA1, 0x00, 0xA5, 0x06, 0xA8, 0x7F, 0xF0, 0xC0, 0xA0};
  auto measure_hz = [&](const std::shared_ptr<audio::AkaoCode>& program, u32 rate) {
    audio::AkaoPlayer player(rate);
    player.start_program(program, 0, table);
    const u32 frames = rate;
    std::vector<float> out(size_t(frames) * 2);
    player.render(out.data(), frames);
    int crossings = 0;
    const size_t begin = rate / 10, end = rate / 2;
    for (size_t i = begin + 1; i < end; ++i) crossings += (out[i * 2] >= 0) != (out[(i - 1) * 2] >= 0);
    return crossings / 2.0 / (double(end - begin) / rate);
  };
  for (const u32 rate : {44100u, 48000u}) CHECK(std::fabs(measure_hz(code, rate) - 6300.0) < 30.0);
  // E1 random pitch: the first note reads random byte 0 (0x8D, bit 7 set = down): 0x2000 - ((0x2000 * 0xFF >> 8) * 0x8D >> 9) = 0x1739,
  // 6300 * 0x1739 / 0x2000 = 4564 Hz. E2 turns it off again.
  auto random_code = std::make_shared<audio::AkaoCode>();
  random_code->data = {0xA1, 0x00, 0xA5, 0x06, 0xA8, 0x7F, 0xE1, 0xFF, 0xF0, 0xC0, 0xA0};
  CHECK(std::fabs(measure_hz(random_code, 44100) - 4564.0) < 30.0);
  random_code->data.insert(random_code->data.begin() + 8, 0xE2);
  CHECK(std::fabs(measure_hz(random_code, 44100) - 6300.0) < 30.0);
  CHECK(audio::kAkaoRandom[0] == 0x8D && audio::kAkaoRandom[255] == 0x89);
  CHECK(audio::akao_random_pitch(0x1000, 20, 0x00) == 0x1000);
  CHECK(audio::akao_random_pitch(0x1000, 20, 0x7F) == 0x1000 + ((0x1000 * 20 >> 8) * 0x7F >> 7));
  CHECK(audio::akao_random_pitch(0x1000, 20, 0xFF) == 0x1000 - ((0x1000 * 20 >> 8) * 0xFF >> 9));
}

// Live BGM source switch (bgm_switch.h, AkaoPlayer::measure_timing / seek_music): the unrolled song time is scaled by the loop-length ratio
// (1.5 here, also 3 loops later), a jingle or an implausible ratio (10) keeps the elapsed time; the stream position is the time itself inside
// the intro / first pass and the loop body after (intro 3, loop 20 -> [3, 23)); BGM002's HCA timing from its loop 3685753..7398138
// (inclusive) at 48 kHz; the AKAO measurement of an intro of one 24-tick note (0.2 s) and a C8 .. CA loop of two (0.4 s) at tempo 0x8000
// = 120 ticks/s, a one-shot's length; and seek_music lands where continuous playback is (same tick count and clock, the warm-up kept the
// sounding note).
void test_bgm_switch() {
  using audio::SongTiming;
  const SongTiming from{2.0, 10.0, 0}, to{3.0, 15.0, 0};
  CHECK(std::fabs(audio::map_song_position(1.0, from, to) - 1.5) < 1e-9);
  CHECK(std::fabs(audio::map_song_position(34.5, from, to) - 51.75) < 1e-9);
  CHECK(std::fabs(audio::map_song_position(5.0, SongTiming{}, to) - 5.0) < 1e-9);
  CHECK(std::fabs(audio::map_song_position(5.0, from, SongTiming{}) - 5.0) < 1e-9);
  CHECK(std::fabs(audio::map_song_position(5.0, from, SongTiming{0, 100, 0}) - 5.0) < 1e-9);
  const SongTiming timing{3.0, 20.0, 0};
  CHECK(std::fabs(audio::stream_position(1.0, timing) - 1.0) < 1e-9 && std::fabs(audio::stream_position(22.0, timing) - 22.0) < 1e-9);
  CHECK(std::fabs(audio::stream_position(23.0, timing) - 3.0) < 1e-9);
  CHECK(std::fabs(audio::stream_position(69.0, timing) - 9.0) < 1e-9);
  CHECK(std::fabs(audio::stream_position(7.0, SongTiming{}) - 7.0) < 1e-9);
  const SongTiming bgm002 = audio::hca_timing(3685753, 7398138, 48000);
  CHECK(std::fabs(bgm002.intro - 3685753.0 / 48000) < 1e-9 && std::fabs(bgm002.loop - 3712386.0 / 48000) < 1e-9);
  CHECK(!audio::hca_timing(0, 0, 48000).looping());

  auto bank = audio::parse_bank(tiny_bank());
  if (!bank) return;
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto seq = std::make_shared<audio::AkaoSeq>();
  seq->code = std::make_shared<audio::AkaoCode>();
  seq->code->data = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0xC8, 0x03, 0x2F, 0xCA};
  seq->channels = {{0, 0}};
  audio::AkaoPlayer measured(48000);
  measured.load_music(seq, table);
  const SongTiming looped = measured.measure_timing();
  CHECK(looped.looping() && std::fabs(looped.intro - 0.2) < 0.03 && std::fabs(looped.loop - 0.4) < 0.03);
  auto once = std::make_shared<audio::AkaoSeq>(*seq);
  once->code = std::make_shared<audio::AkaoCode>();
  once->code->data = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0x2F, 0xA0};
  audio::AkaoPlayer measured_once(48000);
  measured_once.load_music(once, table);
  const SongTiming one_shot = measured_once.measure_timing();
  CHECK(!one_shot.looping() && one_shot.length > 0.35 && one_shot.length < 0.5);
  audio::AkaoPlayer seeked(48000), continuous(48000);
  seeked.load_music(seq, table);
  continuous.load_music(seq, table);
  seeked.seek_music(1.3);
  std::vector<float> buf(size_t(48000) * 2);
  continuous.render(buf.data(), 48000);
  continuous.render(buf.data(), 14400);
  CHECK(std::fabs(seeked.seconds() - 1.3) < 0.01 && std::fabs(continuous.seconds() - 1.3) < 0.01);
  CHECK(std::abs(int(seeked.ticks()) - int(continuous.ticks())) <= 2);
  CHECK(seeked.active_voices() > 0);
  std::printf("bgm switch: loop timing intro %.3f loop %.3f; seek ticks %u vs %u\n", looped.intro, looped.loop, seeked.ticks(), continuous.ticks());
}

// Battle sound rendering: the gate rule looks through intervening opcodes (a 16-tick note, a pitch-bend op A4, then a 24-tick tie holds the
// note for 40 ticks = 0.42 s, not 14); a program that plays a note before any A1 uses instrument 0 (the channel reset); SPU noise (C4 + AC)
// replaces the +-7168 square by the LFSR so many distinct levels appear; the 60 Hz SFX volume fade of the chant mask 0x100 slides linearly
// to 0 over 60 frames (1 s).
void test_battle_render() {
  auto bank = audio::parse_bank(tiny_bank());
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto run = [&](std::vector<u8> bytes, u32 mask, double seconds, auto&& hook) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = std::move(bytes);
    audio::SfxProgram program;
    program.code = code;
    program.count = 1;
    program.table = table;
    program.mask = mask;
    audio::AkaoPlayer player(44100, true);
    player.start_sfx(program);
    std::vector<float> out(size_t(seconds * 44100) * 2);
    hook(player, out);
    return out;
  };
  auto rms = [](const std::vector<float>& out, double t0, double t1) {
    double acc = 0;
    size_t begin = size_t(t0 * 44100), end = std::min(out.size() / 2, size_t(t1 * 44100));
    for (size_t i = begin; i < end; ++i) acc += double(out[i * 2]) * out[i * 2] + double(out[i * 2 + 1]) * out[i * 2 + 1];
    return std::sqrt(acc / double(std::max<size_t>(1, 2 * (end - begin))));
  };
  auto plain = [](audio::AkaoPlayer& player, std::vector<float>& out) { player.render(out.data(), u32(out.size() / 2)); };
  auto out = run({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x08, 0xA4, 0x10, 0x02, 0x87, 0xA0}, 0, 0.8, plain);
  CHECK(rms(out, 0.20, 0.30) > 0.01);
  CHECK(rms(out, 0.55, 0.8) == 0.0);
  out = run({0xA5, 0x05, 0xA8, 0x7F, 0x03, 0xA0}, 0, 0.2, plain);
  CHECK(rms(out, 0.05, 0.15) > 0.01);
  out = run({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC4, 0xAC, 0x2F, 0xC8, 0x03, 0xCA}, 0, 0.3, plain);
  std::set<int> levels;
  for (size_t i = 4000; i < 8000; ++i) levels.insert(int(out[i * 2] * 32768.0f) / 64);
  CHECK(levels.size() > 20);
  out = run({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC8, 0x03, 0xCA}, 0, 0.3, plain);
  levels.clear();
  for (size_t i = 4000; i < 8000; ++i) levels.insert(int(out[i * 2] * 32768.0f) / 64);
  CHECK(levels.size() < 5);
  out = run({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC8, 0x03, 0xCA}, 0x100, 2.0, [](audio::AkaoPlayer& player, std::vector<float>& buffer) {
    player.render(buffer.data(), 22050);
    player.fade_sfx_volume(0x100, 60, 0);
    player.render(buffer.data() + 22050 * 2, u32(buffer.size() / 2 - 22050));
  });
  const double full = rms(out, 0.2, 0.5), mid = rms(out, 0.95, 1.05), end = rms(out, 1.6, 2.0);
  CHECK(full > 0.01 && mid > 0.4 * full && mid < 0.6 * full && end < 0.001 * full);
}

// Per-note restarts and voice updates, read from the pitch register of the voice every 147 samples (60 samples per 0.2 s note at
// 120 ticks/s). An A4 armed before a tie bends the held note (+12 then -12 from the moved note: back to the root, 0x1000); a music
// vibrato with a 10-tick delay (counted from the key-on's own tick, so 9 more ticks) holds the root and then runs the same wave from
// its first sample in each note; a +12 bend over 64 ticks (64 register units per tick, the first step in the note's own tick) on a
// 24-tick note stops reaching the voice at the SFX gate key-off (22 steps: 0x1580) and on a music channel at the rest, not at the gate
// 2 ticks before it (24 steps: 0x1600), instead of running to 0x2000 in the release tail.
void test_note_restarts() {
  auto bank = audio::parse_bank(tiny_bank());
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  constexpr int kStep = 147;
  auto trace = [](audio::AkaoPlayer& player, double seconds) {
    std::vector<int> regs;
    std::vector<float> out(size_t(kStep) * 2);
    for (int i = 0; i < int(seconds * 44100 / kStep); ++i) {
      player.render(out.data(), kStep);
      int reg = -1;
      for (int v = 0; v < audio::AkaoPlayer::kVoices; ++v)
        if (player.probe_voice(v).active) reg = player.probe_voice(v).reg;
      regs.push_back(reg);
    }
    return regs;
  };
  auto get = [](const std::vector<int>& regs, size_t index) { return index < regs.size() ? regs[index] : -2; };
  auto at = [&](const std::vector<int>& regs, double seconds) { return get(regs, size_t(seconds * 44100 / kStep)); };
  auto sfx = [&](std::vector<u8> bytes) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = std::move(bytes);
    auto player = std::make_unique<audio::AkaoPlayer>(44100, true);
    player->start_program(code, 0, table);
    return player;
  };
  auto music = [&](std::vector<u8> bytes) {
    auto seq = std::make_shared<audio::AkaoSeq>();
    seq->code = std::make_shared<audio::AkaoCode>();
    seq->code->data = std::move(bytes);
    seq->channels = {{0, 0}};
    auto player = std::make_unique<audio::AkaoPlayer>(44100);
    player->load_music(seq, table);
    return player;
  };

  auto tie_bend = sfx({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0xA4, 0x08, 0x0C, 0x87, 0xA4, 0x08, 0xF4, 0x87, 0xA0});
  const std::vector<int> bent = trace(*tie_bend, 0.75);
  CHECK(at(bent, 0.15) == 0x1000);
  CHECK(std::abs(at(bent, 0.42) - 0x2000) <= 2);
  CHECK(std::abs(at(bent, 0.68) - 0x1000) <= 2);

  auto vibrato = music({0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xB4, 0x0A, 0x07, 0x02, 0xB5, 0xFF, 0x03, 0x03, 0x03, 0xA0});
  const std::vector<int> vib = trace(*vibrato, 0.6);
  size_t first = 0;
  while (first < vib.size() && vib[first] < 0) ++first;
  bool delayed = true, same_wave = true, moves = false;
  for (size_t note = 0; note < 3; ++note)
    for (size_t i = 1; i < 21; ++i) delayed = delayed && get(vib, first + note * 60 + i) == 0x1000;
  for (size_t i = 23; i < 52; ++i) {
    same_wave = same_wave && get(vib, first + i) == get(vib, first + 60 + i) && get(vib, first + i) == get(vib, first + 120 + i);
    moves = moves || get(vib, first + i) != 0x1000;
  }
  CHECK(delayed && same_wave && moves);

  auto sfx_tail = sfx({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xB2, 0x1F, 0xA4, 0x40, 0x0C, 0x03, 0x92, 0x92, 0x92, 0xA0});
  const std::vector<int> sfx_regs = trace(*sfx_tail, 0.8);
  CHECK(at(sfx_regs, 0.4) == 0x1580 && at(sfx_regs, 0.7) == 0x1580);

  auto music_tail = music({0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xB2, 0x1F, 0xA4, 0x40, 0x0C, 0x03, 0x92, 0x92, 0xA0});
  const std::vector<int> music_regs = trace(*music_tail, 0.6);
  CHECK(at(music_regs, 0.3) == 0x1600 && at(music_regs, 0.55) == 0x1600);
  std::printf("note restarts: tie bend %04X %04X %04X; vibrato delayed %d same wave %d; released bend sfx %04X music %04X\n", at(bent, 0.15),
              at(bent, 0.42), at(bent, 0.68), delayed, same_wave, at(sfx_regs, 0.7), at(music_regs, 0.55));
}

// Music gate mask (header +0x28): two channels play the same three length-24 notes (tempo 0x8000 = 2 updates per tick). The channel
// whose bit is clear (0) holds every note to its full length: each old voice is keyed off in the update of the next key-on. The masked
// channel (1) keys off 2 ticks (4 updates) before each next key-on.
void test_gate_mask() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto seq = std::make_shared<audio::AkaoSeq>();
  seq->code = std::make_shared<audio::AkaoCode>();
  seq->code->data = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0x03, 0x03, 0xA0};
  seq->channels = {{0, 0}, {1, 4}};
  seq->gate_mask = 2;
  audio::AkaoPlayer player(44100);
  std::vector<u32> on[2], off[2];
  player.key_trace = [&](u32 update, int channel, int, bool key_on) { (key_on ? on : off)[channel & 1].push_back(update); };
  player.load_music(seq, table);
  std::vector<float> out(size_t(44100) * 2);
  player.render(out.data(), 44100);
  CHECK(on[0].size() == 3 && on[1].size() == 3 && off[0].size() == 3 && off[1].size() == 3);
  if (on[0].size() != 3 || on[1].size() != 3 || off[0].size() < 2 || off[1].size() < 2) return;
  for (int k = 0; k < 2; ++k) {
    CHECK(off[0][size_t(k)] == on[0][size_t(k) + 1]);
    CHECK(off[1][size_t(k)] + 4 == on[1][size_t(k) + 1]);
  }
  std::printf("gate mask: full-length key-off at next key-on %u/%u, masked channel %u updates early\n", off[0][0], on[0][1], on[1][1] - off[1][0]);
}

// DA portamento, an SFX program shaped like a room's (`DA 0A; note 62 len 12; note 59 len 96`, transpose 3): DA clears the remembered
// note, so the first note keys on at its own pitch; the second is legato (no second key-on) and glides linearly in the pitch register
// from the first note's register to its own over 10 ticks. After a rest the portamento is off: the next note jumps to its pitch.
void test_portamento() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto run = [&](std::vector<u8> bytes, std::vector<int>& regs) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = std::move(bytes);
    audio::AkaoPlayer player(44100, true);
    int key_ons = 0;
    player.key_trace = [&](u32, int, int, bool key_on) { key_ons += key_on; };
    player.start_program(code, 0, table);
    std::vector<float> out(size_t(184) * 2);
    for (int i = 0; i < 400; ++i) {
      player.render(out.data(), 184);
      const audio::AkaoPlayer::VoiceProbe voice = player.probe_voice(audio::AkaoPlayer::kSfxVoiceBase + audio::AkaoPlayer::kSfxChannels - 1);
      if (voice.active) regs.push_back(voice.reg);
    }
    return key_ons;
  };
  std::vector<int> regs;
  CHECK(run({0xA1, 0x00, 0xA5, 0x04, 0xC0, 0x03, 0xAD, 0x30, 0xDA, 0x0A, 0x7D, 0x59, 0xA0}, regs) == 1);
  std::vector<int> steps;
  for (int reg : regs)
    if (steps.empty() || steps.back() != reg) steps.push_back(reg);
  CHECK(steps.size() == 11);
  if (steps.size() < 11) return;
  const int from = steps[steps.size() - 11], to = steps.back();
  bool linear = true;
  for (int k = 1; k < 10; ++k) linear = linear && std::abs(steps[steps.size() - 11 + size_t(k)] - (from + (to - from) * k / 10)) <= 1;
  CHECK(from > to && linear);
  std::printf("portamento: %04X -> %04X in %zu register steps, linear %d\n", from, to, steps.size() - 1, linear);

  std::vector<int> after_rest;
  run({0xA1, 0x00, 0xA5, 0x04, 0xDA, 0x0A, 0x7D, 0x95, 0x7D, 0x59, 0xA0}, after_rest);
  std::vector<int> levels;
  for (int reg : after_rest)
    if (levels.empty() || levels.back() != reg) levels.push_back(reg);
  CHECK(levels.size() == 2);
}

// FE 0B: a music channel requests effect00 ids with its balance and pan high bytes and masks 1, 2, 1 (1 again after a new song start);
// nothing is kept unless requests are collected. The requests replay on an SFX player like the play-SFX command: a new request ends
// the running sound of its own mask, so two looping sounds stay (masks 1 and 2).
void test_sfx_trigger() {
  Bytes effect00(0x830, 0);
  std::memcpy(effect00.data(), "AKAO", 4);
  for (size_t i = 0; i < 256; ++i) put32(effect00, 0x10 + i * 4, 0xFFFFFFFFu);
  put32(effect00, 0x10 + 5 * 4, 0xFFFF0000u);
  const u8 loop[] = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC8, 0x03, 0xCA};
  std::memcpy(&effect00[0x810], loop, sizeof(loop));
  audio::SfxBank bank;
  CHECK(bank.load(effect00, tiny_bank()));
  auto seq = std::make_shared<audio::AkaoSeq>();
  seq->code = std::make_shared<audio::AkaoCode>();
  seq->code->data = {0xFE, 0x00, 0x00, 0x80, 0xA3, 0x20, 0xAA, 0x50, 0xFE, 0x0B, 0x05, 0x92, 0xFE, 0x0B, 0x05, 0x92, 0xFE, 0x0B, 0x05, 0x92, 0xA0};
  seq->channels = {{0, 0}};
  CHECK(audio::akao_sfx_triggers(*seq) == std::vector<int>{5});
  const auto triggers = audio::make_sfx_triggers(bank, *seq);
  CHECK(triggers && triggers->programs.size() == 1);
  audio::AkaoPlayer music(44100), sfx(44100, true);
  music.load_music(seq, bank.table());
  std::vector<float> out(size_t(44100) * 2);
  music.render(out.data(), 44100);
  CHECK(music.take_sfx_requests().empty());
  music.collect_sfx_requests = true;
  music.load_music(seq, bank.table());
  std::vector<u32> masks;
  for (int i = 0; i < 240; ++i) {
    music.render(out.data(), 184);
    const auto requests = music.take_sfx_requests();
    for (const auto& request : requests) {
      CHECK(request.id == 5 && request.volume == 0x20 && request.pan == 0x90);
      masks.push_back(request.mask);
    }
    if (triggers) triggers->play(sfx, requests);
    sfx.render(out.data(), 184);
  }
  CHECK((masks == std::vector<u32>{1, 2, 1}));
  CHECK(sfx.active_channels() == 2 && sfx.active_sfx_mask() == 3);
  music.load_music(seq, bank.table());
  const auto first = music.take_sfx_requests();
  CHECK(first.size() == 1 && first[0].mask == 1);
}

// Music push / pop: a song saved after 1.1 s (inside the gate of its sixth note) and restored into a fresh player of the same song
// carries the song clock and tick count over, keys the held note on again at once, and reaches the next note two ticks (16.7 ms at
// 120 ticks/s) later than the song that kept playing; a song saved during a rest restores with no voice sounding,
// and a channel marked by FE 13 stays silent until its next note.
void test_music_push() {
  auto bank = audio::parse_bank(tiny_bank());
  if (!bank) return;
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(bank);
  auto song = [&](std::vector<u8> bytes) {
    auto seq = std::make_shared<audio::AkaoSeq>();
    seq->code = std::make_shared<audio::AkaoCode>();
    seq->code->data = std::move(bytes);
    seq->channels = {{0, 0}};
    return seq;
  };
  auto scale = song({0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC8, 3, 25, 47, 58, 80, 102, 124, 0xCA});
  constexpr int kStep = 147;
  std::vector<float> out(size_t(48510) * 2);
  auto sounding_reg = [](const audio::AkaoPlayer& player) {
    for (int v = 0; v < audio::AkaoPlayer::kVoices; ++v)
      if (player.probe_voice(v).active) return player.probe_voice(v).reg;
    return -1;
  };
  auto first_change = [&](audio::AkaoPlayer& player) {
    const int start = sounding_reg(player);
    for (int i = 1; i < 300; ++i) {
      player.render(out.data(), kStep);
      if (sounding_reg(player) != start) return i;
    }
    return -1;
  };
  audio::AkaoPlayer saved(44100), kept(44100), resumed(44100);
  saved.load_music(scale, table);
  kept.load_music(scale, table);
  saved.render(out.data(), 48510);
  kept.render(out.data(), 48510);
  const auto context = saved.save_context();
  resumed.load_music(scale, table);
  resumed.restore_context(*context);
  CHECK(resumed.ticks() == saved.ticks() && std::fabs(resumed.seconds() - saved.seconds()) < 1e-9);
  resumed.render(out.data(), kStep);
  kept.render(out.data(), kStep);
  const int held = sounding_reg(kept);
  CHECK(held > 0 && sounding_reg(resumed) == held);
  const int kept_change = first_change(kept), resumed_change = first_change(resumed);
  const double delay_ms = double(resumed_change - kept_change) * kStep * 1000.0 / 44100.0;
  CHECK(kept_change > 0 && delay_ms > 12.0 && delay_ms < 21.0);
  CHECK(sounding_reg(resumed) == sounding_reg(kept));

  auto rests = song({0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xB2, 0x00, 0xC8, 0x03, 0x92, 0xCA});
  audio::AkaoPlayer resting(44100), after_rest(44100);
  resting.load_music(rests, table);
  resting.render(out.data(), 15876);
  after_rest.load_music(rests, table);
  after_rest.restore_context(*resting.save_context());
  after_rest.render(out.data(), kStep);
  CHECK(after_rest.active_voices() == 0 && after_rest.active_channels() == 1);

  auto marked = song({0xFE, 0x00, 0x00, 0x80, 0xFE, 0x13, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xC8, 3, 25, 47, 58, 80, 102, 124, 0xCA});
  audio::AkaoPlayer marked_saved(44100), marked_resumed(44100);
  marked_saved.load_music(marked, table);
  marked_saved.render(out.data(), 48510);
  marked_resumed.load_music(marked, table);
  marked_resumed.restore_context(*marked_saved.save_context());
  marked_resumed.render(out.data(), kStep);
  const bool silent = marked_resumed.active_voices() == 0;
  const bool next_note = first_change(marked_resumed) > 0 && marked_resumed.active_voices() == 1;
  CHECK(silent && next_note);
  std::printf("music push: held reg %04X, next note %.1f ms later than the song that kept playing\n", held, delay_ms);
}

// Steps an SFX player one sequencer tick at a time and records the probe of the channel slot the program runs on.
std::vector<audio::AkaoPlayer::ChannelProbe> trace_sfx_ticks(const std::vector<u8>& program, int ticks,
                                                             const std::function<void(audio::AkaoPlayer&, int)>& at_tick = {}) {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = program;
  audio::AkaoPlayer player(44100, true);
  player.start_program(code, 0, table);
  int slot = -1;
  for (int k = 0; k < audio::AkaoPlayer::kSfxChannels; ++k)
    if (player.probe_channel(k).active) slot = k;
  std::vector<audio::AkaoPlayer::ChannelProbe> rows;
  std::vector<float> out(16 * 2);
  for (u32 last = player.ticks(); int(rows.size()) < ticks;) {
    player.render(out.data(), 16);
    if (player.ticks() == last) continue;
    last = player.ticks();
    rows.push_back(player.probe_channel(slot));
    if (at_tick) at_tick(player, int(rows.size()));
  }
  return rows;
}

// E4 / E5 / E6 rate slides: the target (operand << 10) is not substituted; the step comes from the rate with its low 10 bits cleared,
// +1 when rising, -1 with the low bits filled when falling, so every slide ends with rate >> 10 = the operand. A rate below 0x400 reloads
// the 16-bit LFO counter with 0, which wraps: the LFO holds. Values: the PS1 sound driver running this program.
void test_lfo_rate_slide() {
  const std::vector<u8> program = {0xA1, 0x00, 0xA5, 0x03, 0xA8, 0x40, 0xB4, 0x13, 0x01, 0x01, 0xE4, 0x0C, 0x06, 0x03,
                                   0xE4, 0x0A, 0x00, 0x87, 0x87, 0xE4, 0x00, 0x05, 0x84, 0x85, 0xB8, 0x40, 0x02, 0x01,
                                   0xE5, 0x07, 0x09, 0x87, 0xBC, 0x03, 0x01, 0xE6, 0x05, 0x00, 0x87, 0x87, 0xA0};
  const auto rows = trace_sfx_ticks(program, 420);
  auto seen = [&](int lfo, u32 value) {
    for (const auto& row : rows)
      if (row.rate[lfo] == value) return true;
    return false;
  };
  CHECK(seen(0, 0x400 + 427) && seen(0, 0x1804));
  CHECK(seen(0, 0x1BFF - 615) && seen(0, 0x3F9));
  CHECK(seen(0, 21) && seen(0, 0x1500));
  CHECK(seen(1, 0x800 + 1025) && seen(1, 0x2407));
  CHECK(seen(2, 0xFFF - 615) && seen(2, 0x3FC));
  bool held = false;
  for (const auto& row : rows)
    if (row.rate[0] == 0x3F9 && row.counter[0] < 0) held = true;
  CHECK(held);
}

// An evicted E0-protected channel runs every pass of a counted C9 loop and ends at its next CA (C9 ends nothing): 4 passes of a
// 6-tick note, one more note, then the CA = 30 ticks.
void test_protected_loop_end() {
  const std::vector<u8> program = {0xA1, 0x00, 0xA5, 0x03, 0xA8, 0x40, 0xE0, 0xC8, 0x05, 0xC9, 0x04, 0xC8, 0x05, 0xCA, 0xA0};
  const auto rows = trace_sfx_ticks(program, 40, [](audio::AkaoPlayer& player, int tick) {
    if (tick == 3) player.evict_sfx();
  });
  size_t first = 0, end = 0;
  while (first < rows.size() && rows[first].pc == 0) ++first;
  for (end = first; end < rows.size() && rows[end].active;) ++end;
  CHECK(rows[20].active && rows[20].pc == 9);
  CHECK(end - first == 30);
}

// Runs an SFX program one sequencer tick at a time from its first key-on and records the key events in ticks from that key-on (a key-off
// in the tick of a key-on is the re-key of the channel's voice and is left out), and calls `at_tick` after every tick.
struct SfxKeys {
  std::vector<int> on, off;
};
SfxKeys sfx_keys(const std::vector<u8>& program, int ticks, const std::function<void(audio::AkaoPlayer&, int)>& at_tick = {}) {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = program;
  audio::AkaoPlayer player(44100, true);
  std::vector<int> on, off;
  player.key_trace = [&](u32, int, int, bool key_on) { (key_on ? on : off).push_back(int(player.ticks())); };
  player.start_program(code, 0, table);
  std::vector<float> out(16 * 2);
  for (u32 last = player.ticks(); on.empty() || int(player.ticks()) - on.front() < ticks;) {
    player.render(out.data(), 16);
    if (player.ticks() == last) continue;
    last = player.ticks();
    if (at_tick && !on.empty()) at_tick(player, int(player.ticks()) - on.front());
  }
  SfxKeys keys;
  for (int tick : on) keys.on.push_back(tick - on.front());
  for (int tick : off)
    if (std::find(on.begin(), on.end(), tick) == on.end()) keys.off.push_back(tick - on.front());
  return keys;
}

// Note, tie and rest lengths: the key events of this program match the PS1 driver running the same bytes. An F0 note (length 12) has no
// gate key-off; a rest keys the voice off only by its own gate (length - 2); A2 2 gives a gate of 0 (no key-off), A2 5 a gate of 3;
// DC +2 lengthens the following rest and note to 14; A2 7 sets the length of a rest; an F0 note of length 0 lasts 192 ticks; the
// program's own end leaves the last F0 note sounding.
void test_note_lengths() {
  bool sounding = false;
  const auto keys = sfx_keys({0xA1, 0x00, 0xA5, 0x04, 0xA8, 0x40, 0xF0, 0x0C, 0x04, 0x93, 0xF0, 0x0C, 0x93, 0xA2, 0x02, 0x04, 0x93,
                              0xA2, 0x05, 0x04, 0x93, 0x04, 0xDC, 0x02, 0x93, 0x04, 0xDC, 0x00, 0x93, 0xA2, 0x07, 0x93, 0xF0, 0x00,
                              0x93, 0x04, 0x93, 0xF0, 0x0C, 0xA0},
                             420, [&](audio::AkaoPlayer& player, int) {
                               sounding = player.active_channels() == 0 && player.active_voices() == 1;
                             });
  CHECK((keys.on == std::vector<int>{0, 12, 36, 60, 74, 91, 117, 150, 354, 378}));
  CHECK((keys.off == std::vector<int>{22, 58, 72, 77, 101, 129, 340, 364}));
  CHECK(sounding);
}

// D8 / D9 re-tune a held note at once (register 0x1000 -> 0x1400 -> 0x1600 on the following ties), and an FE 12 balance slide does
// not move on an SFX channel.
void test_fine_tune_held() {
  std::vector<int> regs;
  std::vector<float> gains;
  sfx_keys({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x03, 0xD8, 0x20, 0x87, 0xD9, 0x10, 0x87, 0xFE, 0x12, 0x0C, 0x20, 0x87, 0xA0}, 95,
           [&](audio::AkaoPlayer& player, int) {
             for (int v = 0; v < audio::AkaoPlayer::kVoices; ++v)
               if (player.probe_voice(v).active) regs.push_back(player.probe_voice(v).reg), gains.push_back(player.probe_voice(v).gl);
           });
  CHECK(regs.size() > 90);
  if (regs.size() <= 90) return;
  CHECK(regs[10] == 0x1000 && regs[34] == 0x1400 && regs[58] == 0x1600);
  CHECK(gains[74] > 0 && gains[90] == gains[74]);
}

// The LFO rate words survive a channel start: a second program on the same slot runs E4 before its B4 from the 0x800 the first one
// left (step 0x156: 0x956 after one tick, 0x1808 at the end); its B4 then sets 0x400. Values: the PS1 driver running the same programs.
void test_lfo_rate_carry() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto program = [](std::vector<u8> bytes) {
    auto code = std::make_shared<audio::AkaoCode>();
    code->data = std::move(bytes);
    return code;
  };
  audio::AkaoPlayer player(44100, true);
  player.start_program(program({0xA1, 0x00, 0xA5, 0x04, 0xA8, 0x40, 0xB4, 0x00, 0x02, 0x00, 0x03, 0xA0}), 0, table);
  std::vector<float> out(16 * 2);
  for (int i = 0; i < 4000 && player.active_channels() > 0; ++i) player.render(out.data(), 16);
  const int slot = audio::AkaoPlayer::kSfxChannels - 1;
  CHECK(player.probe_channel(slot).rate[0] == 0x800);
  player.start_program(program({0xA1, 0x00, 0xA5, 0x04, 0xA8, 0x40, 0xE4, 0x0C, 0x06, 0x03, 0xB4, 0x00, 0x01, 0x00, 0x03, 0xA0}), 0, table);
  std::set<u32> rates;
  for (int i = 0; i < 8000 && player.active_channels() > 0; ++i) {
    player.render(out.data(), 16);
    rates.insert(player.probe_channel(slot).rate[0]);
  }
  CHECK(rates.count(0x956) && rates.count(0x1808) && rates.count(0x400));
}

// Keymap (drum) and patch-zone notes write their envelope into the channel's ADSR words (attack byte << 8 with the instrument's attack
// mode, decay and sustain level; sustain rate byte << 6; sustain mode 7 = bits 15:14 set; release byte; the instrument's release mode),
// keep the channel's field where AD / B1 / B2 set an override (B3 clears them), and a keymap note's pan and reverb bit become the
// channel's: a plain note after FE 05 keeps them.
void test_zone_envelopes() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto probes = [&](std::vector<u8> bytes, u32 keymap, u32 patch, int notes) {
    auto seq = std::make_shared<audio::AkaoSeq>();
    seq->code = std::make_shared<audio::AkaoCode>();
    seq->code->data = std::move(bytes);
    seq->code->keymap_abs = keymap;
    seq->code->patch_abs = patch;
    seq->channels = {{0, 0}};
    audio::AkaoPlayer player(44100);
    player.load_music(seq, table);
    std::vector<audio::AkaoPlayer::ChannelProbe> rows;
    std::vector<float> out(16 * 2);
    for (u32 last = player.ticks(); int(rows.size()) < notes;) {
      player.render(out.data(), 16);
      if (player.ticks() == last) continue;
      last = player.ticks();
      if (last % 24 == 2) rows.push_back(player.probe_channel(0));
    }
    return rows;
  };
  std::vector<u8> drum = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x00, 0xFE, 0x04, 0x03, 0xAD, 0x7F, 0x03,
                          0xB3, 0x03, 0xFE, 0x05, 0x03, 0xA0};
  drum.resize(0x40);
  drum.insert(drum.end(), {0x00, 60, 0x05, 0x40, 0x07, 0x0C, 0x60, 0x90});
  const auto rows = probes(drum, 0x40, 0, 4);
  CHECK(rows[0].adsr[0] == 0x050F && rows[0].adsr[1] == 0xD00C && rows[0].pan == 0x5000 && rows[0].reverb);
  CHECK(rows[1].adsr[0] == 0x7F0F && rows[1].adsr[1] == 0xD00C);
  CHECK(rows[2].adsr[0] == 0x050F && rows[2].adsr[1] == 0xD00C);
  CHECK(rows[3].adsr[0] == 0x050F && rows[3].adsr[1] == 0xD00C && rows[3].pan == 0x5000 && rows[3].reverb);

  std::vector<u8> zones = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xFE, 0x14, 0x00, 0xB1, 0x20, 0x03, 0xA0};
  zones.resize(0x40);
  for (int i = 0; i < 16; ++i) zones.insert(zones.end(), {u8(i ? 0xFF : 0x00), u8(i ? 0xFF : 0x00)});
  zones.insert(zones.end(), {0x00, 0, 127, 0x05, 0x40, 0x07, 0x0C, 0x60, 0, 0, 0, 0, 0, 0, 0, 0});
  const auto zone_rows = probes(zones, 0, 0x40, 1);
  CHECK(zone_rows[0].adsr[0] == 0x050F && zone_rows[0].adsr[1] == 0xC80C);
}

// A slide armed in a tick takes its first step in that tick (the slides run after the opcodes): in `note; A8 vol; A4 01 semis; tie` the
// one-tick bend reaches the pitch register in the same voice update as the A8 volume, as effect 12's A9 / A4 pair does on the PS1.
void test_slide_same_tick() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x10, 0x03, 0xA8, 0x60, 0xA4, 0x01, 0xF4, 0x87, 0xA0};
  audio::AkaoPlayer player(44100, true);
  player.start_program(code, 0, table);
  const int voice = audio::AkaoPlayer::kSfxVoiceBase + audio::AkaoPlayer::kSfxChannels - 1;
  std::vector<float> out(size_t(184) * 2);
  int first_reg = -1, first_gl = -1, reg_step = -1, gl_step = -1;
  float gl = 0;
  for (int i = 0; i < 200 && (reg_step < 0 || gl_step < 0); ++i) {
    player.render(out.data(), 184);
    const audio::AkaoPlayer::VoiceProbe probe = player.probe_voice(voice);
    if (!probe.active) continue;
    if (first_reg < 0) first_reg = probe.reg, first_gl = i, gl = probe.gl;
    if (reg_step < 0 && probe.reg != first_reg) reg_step = i;
    if (gl_step < 0 && probe.gl != gl) gl_step = i;
  }
  CHECK(first_gl >= 0 && reg_step > first_gl && reg_step == gl_step);
  std::printf("slide same tick: volume step %d, pitch step %d\n", gl_step, reg_step);
}

// Music voice search: 25 channels key on in one update; the 24 voices keyed in that update count as full level, so the 25th note is
// dropped. A channel keying on one tick later takes the voice with the lowest envelope level (here all equal: the first).
void test_music_voice_search() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto seq = std::make_shared<audio::AkaoSeq>();
  seq->code = std::make_shared<audio::AkaoCode>();
  seq->code->data = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0x00, 0xA0, 0xA1, 0x00, 0xA5, 0x05, 0xFD, 0x01, 0x00, 0xA0};
  for (int i = 0; i < 25; ++i) seq->channels.push_back({i, 0});
  seq->channels.push_back({25, 12});
  audio::AkaoPlayer player(44100);
  std::vector<std::pair<int, int>> key_ons;
  player.key_trace = [&](u32, int channel, int voice, bool key_on) {
    if (key_on) key_ons.push_back({channel, voice});
  };
  player.load_music(seq, table);
  std::vector<float> out(size_t(4410) * 2);
  player.render(out.data(), 4410);
  CHECK(key_ons.size() == 25);
  if (key_ons.size() != 25) return;
  bool first_24 = true;
  for (int i = 0; i < 24; ++i) first_24 = first_24 && key_ons[size_t(i)].first == i && key_ons[size_t(i)].second == i;
  CHECK(first_24);
  CHECK(key_ons[24].first == 25 && key_ons[24].second == 0);
  std::printf("music voice search: %zu key-ons for 26 notes, late channel took voice %d\n", key_ons.size(), key_ons[24].second);
}

// Patch zones with a gap (60..60 scale 0x40, 62..64 scale 0x7F): note 61 after a lower note takes the zone above the gap, after a higher
// note the zone below it.
void test_patch_zone() {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto gain = [&](u8 before) {
    auto seq = std::make_shared<audio::AkaoSeq>();
    seq->code = std::make_shared<audio::AkaoCode>();
    seq->code->data = {0xFE, 0x00, 0x00, 0x80, 0xFE, 0x14, 0x00, 0xA5, 0x05, 0xA8, 0x7F, before, 0x0E, 0xA0};
    seq->code->patch_abs = 0x10;
    seq->code->data.resize(0x10 + 0x20 + 24, 0);
    const u8 zones[16] = {0x00, 60, 60, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 62, 64, 0x00, 0x00, 0x03, 0x00, 0x7F};
    std::memcpy(&seq->code->data[0x30], zones, sizeof(zones));
    seq->channels = {{0, 0}};
    audio::AkaoPlayer player(44100);
    player.load_music(seq, table);
    std::vector<float> out(size_t(184) * 2);
    float last = 0;
    for (int i = 0; i < 70; ++i) {
      player.render(out.data(), 184);
      for (int v = 0; v < audio::AkaoPlayer::kVoices; ++v)
        if (player.probe_voice(v).active) last = std::fabs(player.probe_voice(v).gl);
    }
    return last;
  };
  const float rising = gain(0x03), falling = gain(0x2F);
  CHECK(rising > 0 && falling > 0 && std::fabs(rising / falling - 127.0f / 64.0f) < 0.05f);
  std::printf("patch zone: note 61 after 60 gain %.4f, after 64 gain %.4f\n", rising, falling);
}

// One music channel of `bytes` (keymap / patch table at 0x40 when set) run for `ticks`: its key-on voices and its channel probe two
// ticks into each 24-tick step.
struct MusicRun {
  std::vector<int> voices;
  std::vector<audio::AkaoPlayer::ChannelProbe> rows;
};
MusicRun run_music(std::vector<u8> bytes, u32 keymap, u32 patch, int ticks, int number = 0, u32 gate_mask = 0) {
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(tiny_bank()));
  auto seq = std::make_shared<audio::AkaoSeq>();
  seq->code = std::make_shared<audio::AkaoCode>();
  seq->code->data = std::move(bytes);
  seq->code->keymap_abs = keymap;
  seq->code->patch_abs = patch;
  seq->channels = {{number, 0}};
  seq->gate_mask = gate_mask;
  audio::AkaoPlayer player(44100);
  MusicRun run;
  player.key_trace = [&](u32, int, int voice, bool key_on) {
    if (key_on) run.voices.push_back(voice);
  };
  player.load_music(seq, table);
  std::vector<float> out(16 * 2);
  for (u32 last = player.ticks(); int(last) < ticks;) {
    player.render(out.data(), 16);
    if (player.ticks() == last) continue;
    last = player.ticks();
    if (last % 24 == 2) run.rows.push_back(player.probe_channel(0));
  }
  return run;
}

// Under CC legato a keymap (drum) note still keys on (PS1 trace of music004 channel 21 with its first A8 replaced by CC: every drum note
// keys a voice while the mode word reads 3); a plain note under legato only re-pitches.
void test_drum_legato() {
  std::vector<u8> drum = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x00, 0xFE, 0x04, 0xCC, 0x03, 0x03, 0x03, 0xA0};
  drum.resize(0x40);
  drum.insert(drum.end(), {0x00, 60, 0x05, 0x40, 0x07, 0x0C, 0x60, 0x90});
  const MusicRun drums = run_music(drum, 0x40, 0, 80);
  const MusicRun plain = run_music({0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xCC, 0x03, 0x03, 0x03, 0xA0}, 0, 0, 80);
  CHECK(drums.voices.size() == 3 && plain.voices.size() == 1);
  std::printf("drum legato: %zu key-ons for 3 drum notes, %zu for 3 plain notes\n", drums.voices.size(), plain.voices.size());
}

// A patch key-on of the note the channel already holds keeps the zone and loads nothing: after B3 the channel's ADSR words stay the
// instrument's (0x000F / 0x1F00) instead of the zone envelope (0x050F / 0xD00C) the first key-on wrote. PS1: music062 channel 0, B3
// then the same D as before.
void test_patch_same_note() {
  std::vector<u8> zones = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xFE, 0x14, 0x00, 0x03, 0xB3, 0x03, 0x2F, 0xA0};
  zones.resize(0x40);
  for (int i = 0; i < 16; ++i) zones.insert(zones.end(), {u8(i ? 0xFF : 0x00), u8(i ? 0xFF : 0x00)});
  zones.insert(zones.end(), {0x00, 0, 127, 0x05, 0x40, 0x07, 0x0C, 0x60, 0, 0, 0, 0, 0, 0, 0, 0});
  const MusicRun run = run_music(zones, 0, 0x40, 80);
  CHECK(run.rows.size() >= 3);
  if (run.rows.size() < 3) return;
  CHECK(run.rows[0].adsr[0] == 0x050F && run.rows[0].adsr[1] == 0xD00C);
  CHECK(run.rows[1].adsr[0] == 0x000F && run.rows[1].adsr[1] == 0x1F00);
  CHECK(run.rows[2].adsr[0] == 0x050F && run.rows[2].adsr[1] == 0xD00C);
}

// The header word at +0x28 also pins a channel to the hardware voice of its own number: channel 5 keys voice 5 with its bit set and the
// first free voice (0) without it (PS1 traces of music069: channels 0..9 stay on their voices, channels 12 and 13 search).
void test_fixed_voice() {
  const std::vector<u8> code = {0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0x03, 0xA0};
  const MusicRun fixed = run_music(code, 0, 0, 30, 5, 1u << 5), free = run_music(code, 0, 0, 30, 5, 0);
  CHECK(fixed.voices == std::vector<int>{5} && free.voices == std::vector<int>{0});
}

// Note-on fine tune and the ADSR operand bytes, against the PS1 driver running the same program (instrument with fine tune 15835, base
// note 60, ADSR 0x00FF / 0x5FC7): the note-on delta is taken before the octave shift, so `D8 C1` on G2 gives -281 (register 0x0358)
// and `D8 35` on B8, whose 16-bit pitch has wrapped, 37989 (0x3AC8). AD 85 sets the exponential attack bit and AD 05 leaves it; B1 FF
// and B2 FF OR the whole byte in.
void test_note_fine_tune() {
  Bytes bank = tiny_bank();
  bank[0x40 + 8] = u8(15835 & 0xFF);
  bank[0x40 + 9] = u8(15835 >> 8);
  bank[0x40 + 12] = 0xFF;
  bank[0x40 + 13] = 0x00;
  bank[0x40 + 14] = 0xC7;
  bank[0x40 + 15] = 0x5F;
  auto table = std::make_shared<audio::InstrumentTable>();
  table->overlay(audio::parse_bank(bank));
  auto code = std::make_shared<audio::AkaoCode>();
  code->data = {0xA1, 0x00, 0xA8, 0x40, 0xD8, 0x7F, 0xA5, 0x06, 0x04, 0xD8, 0xC1, 0x30, 0xA5, 0x02, 0x51, 0xD8, 0x35, 0xA5, 0x08,
                0x7D, 0xAD, 0x85, 0x04, 0xAD, 0x05, 0x04, 0xAE, 0x1F, 0x04, 0xAF, 0x1F, 0x04, 0xB1, 0xFF, 0x04, 0xB2, 0xFF, 0x04, 0xA0};
  audio::AkaoPlayer player(44100, true);
  player.start_program(code, 0, table);
  const int slot = audio::AkaoPlayer::kSfxChannels - 1;
  std::vector<int> regs;
  std::vector<std::pair<int, int>> adsr;
  std::vector<float> out(16 * 2);
  for (int i = 0; i < 40000 && player.active_channels() > 0; ++i) {
    player.render(out.data(), 16);
    const audio::AkaoPlayer::VoiceProbe voice = player.probe_voice(audio::AkaoPlayer::kSfxVoiceBase + slot);
    if (voice.active && (regs.empty() || regs.back() != voice.reg)) regs.push_back(voice.reg);
    const audio::AkaoPlayer::ChannelProbe channel = player.probe_channel(slot);
    const std::pair<int, int> words{channel.adsr[0], channel.adsr[1]};
    if (channel.active && (adsr.empty() || adsr.back() != words)) adsr.push_back(words);
  }
  if (!adsr.empty() && adsr.front() != std::pair<int, int>{0x00FF, 0x5FC7}) adsr.erase(adsr.begin());
  CHECK((regs == std::vector<int>{0x1E8D, 0x2D14, 0x0358, 0x3AC8, 0x0C77}));
  CHECK((adsr == std::vector<std::pair<int, int>>{{0x00FF, 0x5FC7}, {0x85FF, 0x5FC7}, {0x85FF, 0x7FC7}, {0x85FF, 0x7FFF}}));
}

}  // namespace

int main() {
  test_adpcm();
  test_adsr();
  test_sequence();
  test_akao_mixer();
  test_mono_voice();
  test_sfx_channels();
  test_sfx_room_banks();
  test_sfx_room_leave_after_battle();
  test_sfx_live_ram();
  test_signed_volume();
  test_keyon_volume_slide();
  test_note_restarts();
  test_gate_mask();
  test_portamento();
  test_sfx_trigger();
  test_battle_render();
  test_pitch_rate();
  test_sab();
  test_loop_math();
  test_table();
  test_mixer_gain();
  test_bgm_switch();
  test_music_push();
  test_lfo_rate_slide();
  test_protected_loop_end();
  test_note_lengths();
  test_fine_tune_held();
  test_lfo_rate_carry();
  test_zone_envelopes();
  test_slide_same_tick();
  test_music_voice_search();
  test_patch_zone();
  test_drum_legato();
  test_patch_same_note();
  test_fixed_voice();
  test_note_fine_tune();
  if (failures == 0) std::printf("test_audio: all passed\n");
  return failures ? 1 : 0;
}
