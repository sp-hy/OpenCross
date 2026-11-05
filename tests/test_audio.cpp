// Checks for oc_audio: SAB header parse on a synthetic blob, loop-wrap maths, the BGM table, the mixer gain formula, and the AKAO side
// (ADPCM decode, ADSR envelopes, the sequencer, SFX channel allocation, pitch vs output rate, live BGM switching, battle sound rendering).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
// vibrato with a 10-tick delay holds the root for the first 10 ticks of every key-on note and then runs the same wave from its first
// sample in each; a +12 bend over 64 ticks (64 register units per tick) on a 24-tick note stops reaching the voice at the SFX gate
// key-off (21 steps: 0x1540) and on a music channel at the rest, not at the gate 2 ticks before it (23 steps: 0x15C0), instead of
// running to 0x2000 in the release tail.
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
    for (size_t i = 1; i < 23; ++i) delayed = delayed && get(vib, first + note * 60 + i) == 0x1000;
  for (size_t i = 23; i < 52; ++i) {
    same_wave = same_wave && get(vib, first + i) == get(vib, first + 60 + i) && get(vib, first + i) == get(vib, first + 120 + i);
    moves = moves || get(vib, first + i) != 0x1000;
  }
  CHECK(delayed && same_wave && moves);

  auto sfx_tail = sfx({0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xB2, 0x1F, 0xA4, 0x40, 0x0C, 0x03, 0x92, 0x92, 0x92, 0xA0});
  const std::vector<int> sfx_regs = trace(*sfx_tail, 0.8);
  CHECK(at(sfx_regs, 0.4) == 0x1540 && at(sfx_regs, 0.7) == 0x1540);

  auto music_tail = music({0xFE, 0x00, 0x00, 0x80, 0xA1, 0x00, 0xA5, 0x05, 0xA8, 0x7F, 0xB2, 0x1F, 0xA4, 0x40, 0x0C, 0x03, 0x92, 0x92, 0xA0});
  const std::vector<int> music_regs = trace(*music_tail, 0.6);
  CHECK(at(music_regs, 0.3) == 0x15C0 && at(music_regs, 0.55) == 0x15C0);
  std::printf("note restarts: tie bend %04X %04X %04X; vibrato delayed %d same wave %d; released bend sfx %04X music %04X\n", at(bent, 0.15),
              at(bent, 0.42), at(bent, 0.68), delayed, same_wave, at(sfx_regs, 0.7), at(music_regs, 0.55));
}

}  // namespace

int main() {
  test_adpcm();
  test_adsr();
  test_sequence();
  test_akao_mixer();
  test_sfx_channels();
  test_sfx_room_banks();
  test_sfx_room_leave_after_battle();
  test_sfx_live_ram();
  test_signed_volume();
  test_keyon_volume_slide();
  test_note_restarts();
  test_battle_render();
  test_pitch_rate();
  test_sab();
  test_loop_math();
  test_table();
  test_mixer_gain();
  test_bgm_switch();
  if (failures == 0) std::printf("test_audio: all passed\n");
  return failures ? 1 : 0;
}
