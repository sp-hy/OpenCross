#include "audio/akao_player.h"

#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <unordered_map>

#include "audio/akao_tables.h"
#include "core/log.h"
#include "vfs/vfs.h"

namespace oc::audio {

std::atomic<unsigned long long> g_sfx_trace_clock{0};
bool sfx_trace_on() {
  static const bool on = [] {
    const char* mode = std::getenv("OC_SFX_TRACE");
    return mode && *mode && *mode != '0';
  }();
  return on;
}

namespace {
double trace_ms() { return double(g_sfx_trace_clock.load(std::memory_order_relaxed)) * 1000.0 / 48000.0; }

constexpr int kLenTable[11] = {192, 96, 48, 24, 12, 6, 3, 32, 16, 8, 4};
constexpr double kUpdateHz = 240.0;
constexpr double kHwRate = 44100.0;

int floor_div(int dividend, int divisor) { return dividend / divisor - ((dividend % divisor != 0) && ((dividend < 0) != (divisor < 0))); }

// Sustain-mode operand (BB / patch zones) -> ADSR2 bits 15:14: 1 linear increase, 3 linear decrease,
// 5 exponential increase, 7 exponential decrease.
u16 sustain_bits(int mode) {
  switch (mode) {
    case 1: return 0x0000;
    case 3: return 0x4000;
    case 5: return 0x8000;
    case 7: return 0xC000;
    default: return 0x4000;
  }
}

// Pitch of a note with 32-bit arithmetic: the value returned is
// `(pitch >> 8) & 0xFFFF` (0x1000 = native rate). The SPU register is written later as `(pitch + modulation) & 0x3FFF`
// a MASK, not a clamp, so a pitch of 4 octaves or more wraps round to a low value.
int pitch_register(int note, const Instrument& in) {
  const int from_base = note - in.base_note;
  const int oct = floor_div(from_base, 12);
  const int idx = from_base - oct * 12;
  const u32 semi = kAkaoSemitone[idx];
  const int ft = in.fine_tune;
  u32 pitch;
  if (ft == 0)
    pitch = semi << 8;
  else if (ft > 0)
    pitch = ((semi * u32(ft)) >> 7) + (semi << 8);
  else
    pitch = (semi * u32(u16(ft))) >> 8;
  if (oct >= 0) {
    pitch <<= (oct & 31);
  } else {
    for (int k = oct; k < 0 && pitch; ++k) pitch = u32(s32(pitch) >> 1);
  }
  return int((pitch >> 8) & 0xFFFF);
}

// The bytes of the sample at `from` (through the later of its two end blocks) differ between the two sound RAM images.
bool sample_changed(const std::vector<u8>& before, const std::vector<u8>& after, u32 from) {
  const size_t end = std::min({size_t(std::max(sample_end(before, from), sample_end(after, from))), before.size(), after.size()});
  return from < end && !std::equal(before.begin() + from, before.begin() + ptrdiff_t(end), after.begin() + from);
}

// The play position `next` of `old` in `live`, the same sample decoded from changed RAM: the decoded run of blocks (start through
// the end block) lines up block for block; the loop body's copy after the end block counts as those blocks again. Past the live
// sample's run the position goes to its loop copy, or its end.
size_t remap_position(const AdpcmSample& old, const AdpcmSample& live, size_t next) {
  size_t index = next;
  if (old.loops && next >= size_t(old.loop_start)) index = size_t(old.loop_start) - (old.pcm.size() - size_t(old.loop_start)) + (next - size_t(old.loop_start));
  const size_t run = live.loops ? size_t(live.loop_start) : live.pcm.size();
  if (index < run) return index;
  return live.loops ? size_t(live.loop_start) : live.pcm.size();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- envelope
void AkaoPlayer::Env::tick() {
  if (--counter > 0) return;
  bool exp = false, dec = false;
  int shift = 0, stepf = 0;
  switch (phase) {
    case 0: exp = (adsr1 >> 15) != 0; shift = adsr1 >> 10 & 31; stepf = adsr1 >> 8 & 3; break;
    case 1: exp = true; dec = true; shift = adsr1 >> 4 & 15; break;
    case 2: exp = (adsr2 >> 15) != 0; dec = (adsr2 >> 14 & 1) != 0; shift = adsr2 >> 8 & 31; stepf = adsr2 >> 6 & 3; break;
    default: exp = (adsr2 >> 5 & 1) != 0; dec = true; shift = adsr2 & 31; break;
  }
  const int step = dec ? (-8 + (phase == 2 ? stepf : 0)) : (7 - stepf);
  int adj = step * (1 << std::max(0, 11 - shift));
  int cycles = 1 << std::max(0, shift - 11);
  if (exp && !dec && level > 0x6000) cycles *= 4;
  if (exp && dec) adj = (adj * level) >> 15;
  counter = cycles;
  level = std::clamp(level + adj, 0, 0x7FFF);
  if (phase == 0 && level >= 0x7FFF) {
    phase = 1;
    counter = 1;
  } else if (phase == 1 && level <= ((adsr1 & 15) + 1) * 0x800) {
    phase = 2;
    counter = 1;
  }
}

s16 AkaoPlayer::noise_step() {
  const int clk = sfx_mode_ ? noise_clock_sfx_ : noise_clock_music_;
  noise_timer_ -= 4 + (clk & 3);
  if (noise_timer_ < 0) {
    noise_timer_ += 0x20000 >> (clk >> 2);
    const u32 parity = ((noise_lfsr_ >> 15) ^ (noise_lfsr_ >> 12) ^ (noise_lfsr_ >> 11) ^ (noise_lfsr_ >> 10) ^ 1) & 1;
    noise_lfsr_ = u16((noise_lfsr_ << 1) | parity);
  }
  return s16(noise_lfsr_);
}

int AkaoPlayer::gauss_interpolate(const s16 (&history)[4], int index) {
  index &= 0xFF;
  const int out = kSpuGauss[0xFF - index] * history[0] + kSpuGauss[0x1FF - index] * history[1] + kSpuGauss[0x100 + index] * history[2] + kSpuGauss[index] * history[3];
  return out >> 15;
}

// ---------------------------------------------------------------------------------------------------- setup
AkaoPlayer::AkaoPlayer(u32 out_rate, bool sfx_mode) : rate_(out_rate ? out_rate : kSpuRate), sfx_mode_(sfx_mode) {
  // SFX tempo: the tick adds this value per 240 Hz update (~96 ticks/s).
  if (sfx_mode_) tempo_ = 0x66A8;
  // One frame of resampler history.
  spu_.assign(2, 0.0f);
  // Default reverb: mode 4, depth 0x3FFF.
  rev_.set_mode(4);
}

void AkaoPlayer::set_reverb(int mode, int depth) {
  rev_.set_mode(mode);
  rev_depth_ = s32(depth) << 16;
  rev_slide_left_ = 0;
}

void AkaoPlayer::set_sfx_mods(u32 mask, int volume127, int pan127) {
  if (mask == 0) return;
  for (auto& channel : ch_) {
    if (!channel.active || channel.music || !(channel.mask & mask)) continue;
    // Cmd A0: volume modifier = v & 0x7F, slide cleared. Cmd A2: pan modifier = pan * 2.
    if (volume127 >= 0) channel.vol_mod = volume127 & 0x7F, channel.vol_mod_frac = 0, channel.vol_fade_left = 0;
    if (pan127 >= 0) channel.pan_mod = std::clamp(pan127 * 2, 0, 0xFF);
  }
}

void AkaoPlayer::fade_sfx_volume(u32 mask, int frames, int volume127) {
  if (mask == 0) return;
  const int fade_frames = frames > 0 ? frames : 1;
  for (auto& channel : ch_) {
    if (!channel.active || channel.music || !(channel.mask & mask)) continue;
    const int cur = channel.vol_mod << 8 | channel.vol_mod_frac;
    channel.vol_fade_left = fade_frames;
    // The C division truncates toward zero.
    channel.vol_fade_step = (s16((volume127 & 0x7F) * 0x100 - cur)) / fade_frames;
  }
}

void AkaoPlayer::tick_60hz() {
  ++random_index_;
  for (auto& channel : ch_) {
    if (!channel.active || channel.music || channel.vol_fade_left <= 0) continue;
    --channel.vol_fade_left;
    const int volume = std::clamp((channel.vol_mod << 8 | channel.vol_mod_frac) + channel.vol_fade_step, 0, 0x7FFF);
    channel.vol_mod = volume >> 8;
    channel.vol_mod_frac = volume & 0xFF;
  }
}

u32 AkaoPlayer::active_sfx_mask() const {
  u32 mask = 0;
  for (const auto& channel : ch_)
    if (channel.active && !channel.music) mask |= channel.mask;
  return mask;
}

// Run by every field PLAY_SFX: every running SFX channel is cut, except those whose program set
// flag 0x100000 (op E0), which are marked to end at their next loop pass (0x200000).
void AkaoPlayer::evict_sfx() {
  for (auto& channel : ch_) {
    if (!channel.active || channel.music) continue;
    if (channel.protect) channel.stop_at_loop = true;
    else end_channel(channel);
  }
}

void AkaoPlayer::evict_sfx(u32 mask, bool untagged) {
  if (mask == 0 && !untagged) return;
  for (auto& channel : ch_) {
    if (!channel.active || channel.music) continue;
    if (!(channel.mask & mask) && !(untagged && channel.mask == 0)) continue;
    if (channel.protect) channel.stop_at_loop = true;
    else end_channel(channel);
  }
}

void AkaoPlayer::upload_ram(std::shared_ptr<const std::vector<u8>> ram) {
  if (!ram) return;
  for (Voice& voice : voice_) {
    if (!voice.active || !voice.ram || voice.ram == ram) continue;
    if (sample_changed(*voice.ram, *ram, voice.spu_start)) {
      auto live = std::make_shared<AdpcmSample>(adpcm_decode_sample(*ram, voice.spu_start, voice.spu_loop));
      if (!live->pcm.empty()) {
        voice.next = remap_position(*voice.smp, *live, voice.next);
        voice.smp = std::move(live);
      }
    }
    voice.ram = ram;
  }
  ram_ = std::move(ram);
}

void AkaoPlayer::stop() {
  for (auto& channel : ch_) channel = Channel{};
  for (auto& voice : voice_) voice = Voice{};
}

AkaoPlayer::Channel* AkaoPlayer::alloc_channel() {
  Channel* oldest = nullptr;
  for (auto& channel : ch_) {
    if (!channel.active) return &channel;
    if (!oldest || channel.serial < oldest->serial) oldest = &channel;
  }
  if (oldest) end_channel(*oldest);
  return oldest;
}

void AkaoPlayer::load_music(const std::shared_ptr<const AkaoSeq>& seq, std::shared_ptr<const InstrumentTable> table) {
  stop();
  tempo_ = 0x2800;
  tempo_slide_left = 0;
  tempo_acc_ = 0;
  ticks_ = 0;
  upd_left_ = 0;
  if (!seq) return;
  // The header's reverb mode applies when the song starts.
  rev_.set_mode(seq->reverb_mode);
  sfx_request_mask_ = 1;
  sfx_requests_.clear();
  int channel_count = 0;
  for (const auto& [number, pc] : seq->channels) {
    if (channel_count >= kMaxChannels) break;
    Channel& channel = ch_[size_t(channel_count++)];
    channel = Channel{};
    channel.active = true;
    channel.music = true;
    channel.number = number;
    channel.serial = serial_++;
    channel.code = seq->code;
    channel.table = table;
    channel.pc = pc;
    channel.tenuto = !(seq->gate_mask >> (number & 31) & 1);
  }
  for (auto& channel : ch_)
    if (channel.active) run_ops(channel);
}

// Starts one request (one entry of a pointer file / one effect00 id: up to two programs, a pair).
void AkaoPlayer::start_sfx(const SfxProgram& program) {
  if (!sfx_mode_) {
    int first = -1;
    for (int i = 0; i < program.count; ++i) {
      last_ch_ = -1;
      start_program(program.code, program.pc[i], program.table, program.volume, program.pan, program.mask, &program.tag);
      if (i == 0) first = last_ch_;
      else if (first >= 0 && last_ch_ >= 0) {
        ch_[size_t(last_ch_)].fm_carrier = true;
        ch_[size_t(last_ch_)].fm_partner = first;
      }
    }
    return;
  }
  u32 pcs[2];
  int program_count = 0;
  for (int i = 0; i < program.count && i < 2; ++i)
    if (program.code && program.pc[i] < program.code->data.size()) pcs[program_count++] = program.pc[i];
  if (program_count == 0) return;
  const int base = alloc_sfx_slots(program_count);
  if (base < 0) {
    ++dropped_;
    if (sfx_trace_on()) log::info("sfxtrace {:9.1f} ms DROP '{}' mask {:x} programs {} (no free channel, nothing to steal)", trace_ms(), program.tag, program.mask, program_count);
    return;
  }
  for (int i = 0; i < program_count; ++i) {
    Channel* channel = begin_sfx_channel(base + i, program.code, pcs[i], program.table, program.volume, program.pan, program.mask, &program.tag);
    // The second program of a pair is flagged 0x10000 (the FM carrier).
    if (i == 1) {
      channel->fm_carrier = true;
      channel->fm_partner = base;
    }
  }
}

u32 AkaoPlayer::busy_channels() const {
  u32 mask = 0;
  for (int k = 0; k < kSfxChannels; ++k)
    if (ch_[size_t(k)].active && !ch_[size_t(k)].music) mask |= 1u << k;
  return mask;
}

// Steals a channel: among the busy channels whose mask is 0, those with the greatest age
// (the signed maximum starts at 0, so a channel younger than its third tick is never taken) end; E0-protected ones only end at their
// loop end (they stay busy, so they do not count as freed).
bool AkaoPlayer::steal_sfx_channel() {
  const u32 before = busy_channels();
  int max_age = 0;
  for (int k = 0; k < kSfxChannels; ++k) {
    const Channel& channel = ch_[size_t(k)];
    if ((before >> k & 1) && (channel.mask & kPs1MaskBits) == 0 && channel.age > max_age) max_age = channel.age;
  }
  for (int k = 0; k < kSfxChannels; ++k) {
    Channel& channel = ch_[size_t(k)];
    if (!(before >> k & 1) || (channel.mask & kPs1MaskBits) != 0 || channel.age != max_age) continue;
    if (channel.protect) {
      channel.stop_at_loop = true;
    } else {
      if (sfx_trace_on()) log::info("sfxtrace {:9.1f} ms STEAL channel {} '{}' #{} age {} ticks", trace_ms(), k, channel.tag, channel.trace_id, channel.age);
      end_channel(channel);
      ++stolen_;
    }
  }
  return busy_channels() != before;
}

// The SFX channel search: a single program takes the highest free channel, a pair the highest free adjacent pair
// (the first program on the lower channel); with none free the steal rule runs and the search repeats until nothing more can be freed.
int AkaoPlayer::alloc_sfx_slots(int program_count) {
  for (;;) {
    const u32 busy = busy_channels();
    for (int k = kSfxChannels - program_count; k >= 0; --k) {
      const u32 want = (program_count == 2 ? 3u : 1u) << k;
      if ((busy & want) == 0) return k;
    }
    if (!steal_sfx_channel()) return -1;
  }
}

void AkaoPlayer::trace_report(const char* why) const {
  for (size_t i = 0; i < ch_.size(); ++i)
    if (ch_[i].active)
      log::info("sfxtrace {:9.1f} ms REPORT({}) channel {} '{}' #{} mask {:x} age {:.0f} ms music {}", trace_ms(), why, i, ch_[i].tag,
                ch_[i].trace_id, ch_[i].mask, trace_ms() - ch_[i].started_at * 1000.0 / 48000.0, ch_[i].music);
  for (size_t i = 0; i < voice_.size(); ++i)
    if (voice_[i].active)
      log::info("sfxtrace {:9.1f} ms REPORT({}) voice {} chan {} released {} env phase {} level {} age {:.0f} ms", trace_ms(), why, i,
                voice_[i].chan, voice_[i].released, voice_[i].env.phase, voice_[i].env.level,
                trace_ms() - voice_[i].started_at * 1000.0 / 48000.0);
}

void AkaoPlayer::start_program(const std::shared_ptr<const AkaoCode>& code, u32 pc,
                               std::shared_ptr<const InstrumentTable> table, float volume, int pan, u32 mask,
                               const std::string* tag) {
  if (!code || pc >= code->data.size()) return;
  // A single program of a request (tests / tools); start_sfx is the full request entry.
  if (sfx_mode_) {
    const int slot = alloc_sfx_slots(1);
    if (slot < 0) {
      ++dropped_;
      return;
    }
    last_ch_ = slot;
    begin_sfx_channel(slot, code, pc, std::move(table), volume, pan, mask, tag);
    return;
  }
  Channel* channel = alloc_channel();
  if (!channel) return;
  last_ch_ = int(channel - ch_.data());
  *channel = Channel{};
  channel->active = true;
  channel->music = false;
  channel->serial = serial_++;
  channel->mask = mask;
  if (sfx_trace_on()) {
    static int next_id = 1;
    channel->trace_id = next_id++;
    channel->tag = tag ? *tag : std::string();
    channel->started_at = g_sfx_trace_clock.load(std::memory_order_relaxed);
    std::string hex;
    for (u32 k = 0; k < (std::getenv("OC_SFX_TRACE")[0] == '2' ? 0x1000u : 0x30u) && k < code->data.size(); ++k) hex += std::format("{:02x} ", code->data[k]);
    hex += "| ";
    for (u32 k = 0; k < 160 && pc + k < code->data.size(); ++k) hex += std::format("{:02x} ", code->data[pc + k]);
    log::info("sfxtrace {:9.1f} ms START #{} '{}' pc {:x} mask {:x} vol {:.2f} pan {} code {}", trace_ms(), channel->trace_id, channel->tag, pc, mask, volume, pan, hex);
  }
  channel->code = code;
  channel->table = std::move(table);
  channel->pc = pc;
  // A program that plays a note before any A1 uses instrument 0.
  channel->inst_index = 0;
  channel->inst = channel->table ? channel->table->get(0) : nullptr;
  if (channel->inst) channel->adsr1 = channel->inst->adsr1, channel->adsr2 = channel->inst->adsr2;
  // Channel reset: pan = 0x8000, pan modifier = pan << 8, volume modifier = (vol & 0x7F) << 8, balance = 0x6E00,
  // volume = 0x32000000.
  channel->vol_mod = std::clamp(int(volume * 127.0f + 0.5f), 0, 0x7F);
  channel->pan_mod = pan >= 0 ? std::clamp(pan * 2, 0, 0xFF) : 0x80;
  if (!channel->music) {
    channel->balance = 0x6E00;
    channel->volume = 0x32000000;
  }
  // The length counter starts at 2: the program's first instructions run on the second SFX
  // tick after the start (~10-20 ms), not at once.
  if (sfx_mode_)
    channel->len_left = 2;
  else
    run_ops(*channel);
}

// Resets SFX channel `slot`: pan = 0x8000, pan modifier = pan << 8, volume modifier = (vol & 0x7F) << 8, balance = 0x6E00,
// volume = 0x32000000, age = -2, length counter = 2: the program's first instructions run on the second SFX tick after
// the start (~10-20 ms), not at once.
AkaoPlayer::Channel* AkaoPlayer::begin_sfx_channel(int slot, const std::shared_ptr<const AkaoCode>& code, u32 pc,
                                                   std::shared_ptr<const InstrumentTable> table, float volume, int pan, u32 mask,
                                                   const std::string* tag) {
  Channel* channel = &ch_[size_t(slot)];
  *channel = Channel{};
  channel->active = true;
  channel->music = false;
  channel->serial = serial_++;
  channel->mask = mask;
  channel->age = -2;
  if (sfx_trace_on()) {
    static int next_id = 1;
    channel->trace_id = next_id++;
    channel->tag = tag ? *tag : std::string();
    channel->started_at = g_sfx_trace_clock.load(std::memory_order_relaxed);
    std::string hex;
    for (u32 k = 0; k < (std::getenv("OC_SFX_TRACE")[0] == '2' ? 0x1000u : 0x30u) && k < code->data.size(); ++k) hex += std::format("{:02x} ", code->data[k]);
    hex += "| ";
    for (u32 k = 0; k < 160 && pc + k < code->data.size(); ++k) hex += std::format("{:02x} ", code->data[pc + k]);
    log::info("sfxtrace {:9.1f} ms START #{} '{}' ch {} pc {:x} mask {:x} vol {:.2f} pan {} code {}", trace_ms(), channel->trace_id, channel->tag, slot, pc, mask, volume, pan, hex);
  }
  channel->code = code;
  channel->table = std::move(table);
  channel->pc = pc;
  // A program that plays a note before any A1 uses instrument 0.
  channel->inst_index = 0;
  channel->inst = channel->table ? channel->table->get(0) : nullptr;
  if (channel->inst) channel->adsr1 = channel->inst->adsr1, channel->adsr2 = channel->inst->adsr2;
  channel->vol_mod = std::clamp(int(volume * 127.0f + 0.5f), 0, 0x7F);
  channel->pan_mod = pan >= 0 ? std::clamp(pan * 2, 0, 0xFF) : 0x80;
  channel->balance = 0x6E00;
  channel->volume = 0x32000000;
  channel->len_left = 2;
  return channel;
}

bool AkaoPlayer::finished() const { return active_channels() == 0 && active_voices() == 0 && rev_tail_ == 0; }
int AkaoPlayer::active_channels() const {
  int count = 0;
  for (const auto& channel : ch_) count += channel.active;
  return count;
}
int AkaoPlayer::active_voices() const {
  int count = 0;
  for (const auto& voice : voice_) count += voice.active;
  return count;
}
AkaoPlayer::VoiceProbe AkaoPlayer::probe_voice(int index) const {
  if (index < 0 || index >= kVoices) return {};
  const Voice& voice = voice_[size_t(index)];
  return {voice.active, voice.reg, voice.gl, voice.gr, voice.env.level};
}

// ---------------------------------------------------------------------------------------------------- voices
int AkaoPlayer::alloc_voice() {
  int best = 0;
  float best_score = 1e30f;
  for (int i = 0; i < kVoices; ++i) {
    const Voice& voice = voice_[size_t(i)];
    if (!voice.active) return i;
    // Steal released voices first, then the quietest one.
    const float score = (voice.released ? 0.0f : 1e6f) + float(voice.env.level) * (std::abs(voice.gl) + std::abs(voice.gr) + 1e-3f);
    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }
  const Voice& voice = voice_[size_t(best)];
  if (voice.chan >= 0 && ch_[size_t(voice.chan)].voice == best && ch_[size_t(voice.chan)].voice_serial == voice.serial)
    ch_[size_t(voice.chan)].voice = -1;
  return best;
}

int AkaoPlayer::alloc_voice_after(int prev) {
  const int i = prev + 1;
  if (i >= kVoices) return -1;
  const Voice& voice = voice_[size_t(i)];
  // Never cut a sounding voice for the adjacency.
  if (voice.active && !voice.released) return -1;
  if (voice.active && voice.chan >= 0 && ch_[size_t(voice.chan)].voice == i && ch_[size_t(voice.chan)].voice_serial == voice.serial)
    ch_[size_t(voice.chan)].voice = -1;
  return i;
}

void AkaoPlayer::key_off(Channel& channel) {
  if (channel.voice >= 0) {
    Voice& voice = voice_[size_t(channel.voice)];
    if (voice.active && voice.serial == channel.voice_serial && !voice.released) {
      voice.released = true;
      voice.env.key_off();
      if (key_trace) key_trace(upd_count_, channel.number, channel.voice, false);
    }
  }
  channel.gate_left = 0;
}

void AkaoPlayer::end_channel(Channel& channel) {
  if (sfx_trace_on() && !channel.music && channel.active)
    log::info("sfxtrace {:9.1f} ms CEND #{} '{}' after {:.0f} ms voice {}", trace_ms(), channel.trace_id, channel.tag,
              trace_ms() - channel.started_at * 1000.0 / 48000.0, channel.voice);
  key_off(channel);
  channel.active = false;
  channel.voice_update = false;
}

void AkaoPlayer::note_on(Channel& channel, int key) {
  // A key-on (a legato re-pitch is none) restarts the FE 19 slide from its start volume, the vibrato and tremolo waves from their
  // first sample and their delays, and stops a running bend; autopan runs on.
  if (!channel.legato_prev) {
    if (channel.keyon_slide_len) {
      channel.volume = channel.keyon_volume;
      channel.vol_slide_left = channel.keyon_slide_len;
      channel.vol_slide_step = channel.keyon_slide_step;
    }
    for (Lfo* lfo : {&channel.vib, &channel.trem}) {
      if (!lfo->on) continue;
      lfo->wave = akao_lfo(lfo->type);
      lfo->counter = 1;
      lfo->delay_cur = lfo->delay;
    }
    channel.bend_left = 0;
  }
  // Every note, legato too: the vibrato and tremolo outputs and the bend restart from 0, and the voice takes updates again.
  channel.vib.value = channel.trem.value = 0;
  channel.bend = 0;
  channel.voice_update = true;
  const int base_note = channel.octave * 12 + key;
  int note = base_note + channel.transpose;
  const Instrument* in = channel.inst;
  u16 a1 = channel.adsr1, a2 = channel.adsr2;
  channel.drum_pan = -1;
  channel.zone_scale = 0;
  const bool keymap_note = channel.drum && channel.code->keymap_abs;
  if (keymap_note) {
    const size_t off = size_t(channel.code->keymap_abs) + size_t(std::clamp(base_note, 0, 255)) * 8;
    if (off + 8 <= channel.code->data.size()) {
      const u8* entry = &channel.code->data[off];
      in = channel.table->get(entry[0]);
      if (!in) return;
      note = entry[1];
      a1 = in->adsr1;
      a2 = in->adsr2;
      if (entry[2] | entry[3] | entry[4] | entry[5]) {
        a1 = u16((a1 & 0xFF) | entry[2] << 8);
        a2 = u16((entry[5] & 0x1F) | (a2 & 0x20) | (entry[3] & 0x7F) << 6 | sustain_bits(entry[4]));
      }
      channel.zone_scale = entry[6];
      channel.drum_pan = entry[7] & 0x7F;
    }
  } else if (channel.patch >= 0 && !channel.zones.empty()) {
    const AkaoZone* zone = &channel.zones.front();
    for (const AkaoZone& cand : channel.zones) {
      if (note >= cand.low_key && note <= cand.high_key) {
        zone = &cand;
        break;
      }
      if (cand.high_key < note) zone = &cand;
    }
    in = channel.table->get(zone->instrument);
    if (!in) return;
    a1 = u16((in->adsr1 & 0xFF) | zone->ar << 8);
    a2 = u16((zone->rr & 0x1F) | (in->adsr2 & 0x20) | (zone->sr & 0x7F) << 6 | sustain_bits(zone->mode));
    channel.zone_scale = zone->volume;
  }
  if (!in || !in->sample || in->sample->pcm.empty()) return;
  // OC_SFX_KEEP=<hex mask of SFX request numbers (1-based, trace_id)>: dev knob, plays only those channels' notes.
  {
    static const unsigned keep_mask = [] {
      const char* env = std::getenv("OC_SFX_KEEP");
      return env ? unsigned(std::strtoul(env, nullptr, 16)) : 0xFFFFFFFFu;
    }();
    if (!channel.music && channel.trace_id >= 1 && channel.trace_id <= 32 && !(keep_mask >> (channel.trace_id - 1) & 1)) return;
  }

  // DA portamento after a remembered note: the note starts at the remembered note's pitch and the A4 bend carries it to its own over
  // the portamento steps.
  int pitch_note = note;
  if (!keymap_note) {
    if (channel.porta_steps > 0 && channel.last_note != 0) {
      pitch_note = channel.last_note + channel.last_transpose;
      channel.bend_len = channel.porta_steps;
      channel.bend_delta = note - pitch_note;
    }
    channel.bend_note = pitch_note - channel.transpose;
  }
  channel.pitch_base = pitch_register(pitch_note, *in);
  channel.bend_inst = in;
  if (note_trace) note_trace(note, *in, channel.pitch_base);
  const int fd = channel.fine_tune;
  if (fd < 0)
    channel.fine_delta = ((channel.pitch_base * (fd & 0xFF)) >> 8) - channel.pitch_base;
  else
    channel.fine_delta = (channel.pitch_base * fd) >> 7;
  // E1 random pitch moves the base after the fine-tune delta was taken from it; keymap (drum) notes have none.
  if (channel.random_pitch && !keymap_note)
    channel.pitch_base = int(akao_random_pitch(u32(channel.pitch_base), u32(channel.random_pitch), kAkaoRandom[random_index_]));

  // Legato into a sounding voice: re-pitch only.
  const bool keep = channel.legato_prev && channel.voice >= 0 && voice_[size_t(channel.voice)].active &&
                    voice_[size_t(channel.voice)].serial == channel.voice_serial && !voice_[size_t(channel.voice)].released;
  if (keep) return;
  // A new key-on on a channel releases the note it still holds (the SPU voice of the channel is keyed off first): a note of length 1
  // has no gate key-off, a full-length / tenuto note none either, and the voice would sound on forever (a stuck note) once the
  // channel owns a newer voice.
  key_off(channel);

  int vi = -1;
  int fm_src = -1;
  if (sfx_mode_ && !channel.music) {
    // SFX channel k owns hardware voice 12 + k: no voice
    // search or stealing, a new note re-keys the channel's own voice (the previous note's release tail is cut), and the FM carrier (the second
    // program of a pair, channel k + 1) is modulated by voice k - 1 = its partner's.
    vi = kSfxVoiceBase + int(&channel - ch_.data());
    if (channel.fm_carrier && channel.fm_partner >= 0) fm_src = vi - 1;
    const Voice& old = voice_[size_t(vi)];
    if (old.active && old.chan >= 0 && ch_[size_t(old.chan)].voice == vi && ch_[size_t(old.chan)].voice_serial == old.serial)
      ch_[size_t(old.chan)].voice = -1;
  } else {
    // FM: the voice right after the modulator's.
    if (channel.fm_carrier && channel.fm_partner >= 0) {
      const Channel& modulator = ch_[size_t(channel.fm_partner)];
      if (modulator.active && modulator.voice >= 0 && voice_[size_t(modulator.voice)].active) vi = alloc_voice_after(modulator.voice);
    }
    fm_src = vi >= 1 && channel.fm_carrier && channel.fm_partner >= 0 && ch_[size_t(channel.fm_partner)].voice == vi - 1 ? vi - 1 : -1;
    if (vi < 0) vi = alloc_voice();
  }
  Voice& voice = voice_[size_t(vi)];
  voice = Voice{};
  voice.fm_src = fm_src;
  voice.fm = channel.fm && fm_src >= 0;
  voice.active = true;
  voice.chan = int(&channel - ch_.data());
  voice.serial = serial_++;
  voice.smp = in->sample;
  if (sfx_mode_ && !channel.music && channel.table && channel.table->ram()) {
    voice.spu_start = in->spu + in->start;
    voice.spu_loop = in->spu + in->loop;
    voice.ram = channel.table->ram();
    if (ram_ && ram_ != voice.ram) {
      if (sample_changed(*voice.ram, *ram_, voice.spu_start)) {
        auto live = std::make_shared<AdpcmSample>(adpcm_decode_sample(*ram_, voice.spu_start, voice.spu_loop));
        if (!live->pcm.empty()) voice.smp = std::move(live);
      }
      voice.ram = ram_;
    }
  }
  voice.env.key_on(a1, a2);
  voice.counter = 0;
  // Key on: the filter history before the first block is silence.
  voice.hist[0] = voice.hist[1] = voice.hist[2] = 0;
  voice.hist[3] = voice.smp->pcm[0];
  voice.next = 1;
  voice.reg = channel.pitch_base;
  channel.voice = vi;
  channel.voice_serial = voice.serial;
  if (key_trace) key_trace(upd_count_, channel.number, vi, true);
  voice.started_at = g_sfx_trace_clock.load(std::memory_order_relaxed);
  if (sfx_trace_on() && !channel.music) log::info("sfxtrace {:9.1f} ms VON #{} '{}' voice {} note {} reg {:04X} adsr {:04X} {:04X} base {} ft {} loops {} len {} noise {} fm {}", trace_ms(), channel.trace_id, channel.tag, vi, note, channel.pitch_base, a1, a2, in->base_note, in->fine_tune, voice.smp->loops ? voice.smp->loop_start : -1, voice.smp->pcm.size(), channel.noise, voice.fm);
}

// A note or a tie consumes the armed A4 delta: the bend note moves by it and the register ramps over the A4 length from where it is
// now (a tie keeps the running bend) to the moved note's pitch, in 16.16 steps truncated toward zero. A rest leaves the bend armed.
// Then the bend note and the transpose are remembered as the start of the next portamento glide.
void AkaoPlayer::start_bend(Channel& channel) {
  if (channel.bend_delta && channel.bend_len > 0 && channel.bend_inst) {
    channel.bend_note += channel.bend_delta;
    channel.bend_delta = 0;
    const s64 target = s64(pitch_register(channel.bend_note + channel.transpose, *channel.bend_inst)) * 0x10000;
    const s64 current = s64(channel.pitch_base) * 0x10000 + channel.bend;
    channel.bend_left = channel.bend_len;
    channel.bend_step = s32((target - current) / channel.bend_len);
  }
  channel.last_note = channel.bend_note;
  channel.last_transpose = channel.transpose;
}

// ADSR ops (AD..B3, B7, BB, BF) mark the channel's ADSR words dirty and the next voice update writes them to the SPU voice that is playing
// a note already sounding changes its envelope.
void AkaoPlayer::sync_voice_adsr(Channel& channel) {
  if (channel.voice < 0 || channel.drum || channel.patch >= 0) return;
  Voice& voice = voice_[size_t(channel.voice)];
  if (!voice.active || voice.serial != channel.voice_serial) return;
  voice.env.adsr1 = channel.adsr1;
  voice.env.adsr2 = channel.adsr2;
  // The SPU re-reads the rate on a register write: the pending wait of the old rate ends.
  voice.env.counter = 1;
}

// ---------------------------------------------------------------------------------------------------- VM
void AkaoPlayer::run_ops(Channel& channel) {
  const Bytes& data = channel.code->data;
  auto rd8 = [&](u32 pos) -> int { return pos < data.size() ? data[pos] : 0; };
  auto rds16 = [&](u32 pos) -> int { return s16(rd8(pos) | rd8(pos + 1) << 8); };
  auto slide = [](int len) { return len == 0 ? 256 : len; };
  // Peeks the next note / tie / rest opcode, skipping ordinary opcodes (A4, A9, ...) and following loop ends,
  // jumps and calls the way the VM will. A rest or a finishing opcode clears the legato and full-length (D0) flags.
  auto peek_next = [&](const Channel& ch) -> int {
    u32 pc = ch.pc;
    int sp = ch.loop_top;
    for (int guard = 0; guard < 256; ++guard) {
      const int opcode = rd8(pc);
      if (opcode < 0x9A) {
        if (opcode > 0x8E) channel.legato = channel.full_length = false;
        return opcode;
      }
      if (opcode < 0xA0) break;
      const int op_len = pc < data.size() ? akao_op_length(&data[pc], data.size() - pc) : 0;
      if (opcode == 0xC9) {
        if (rd8(pc + 1) == ch.loop_count[sp] + 1) {
          sp = (sp - 1) & 3;
          pc += 2;
        } else {
          pc = ch.loop_pc[sp];
        }
        continue;
      }
      if (opcode == 0xCA) {
        if (ch.stop_at_loop) break;
        pc = ch.loop_pc[sp];
        continue;
      }
      if (opcode == 0xCB || opcode == 0xCD || opcode == 0xD1 || opcode == 0xDB) {
        channel.legato = channel.full_length = false;
        pc += 1;
        continue;
      }
      if (opcode >= 0xF0 && opcode <= 0xFB) return 0x83;
      if (opcode == 0xFC) return 0x84;
      if (opcode == 0xFD) return 0x8F;
      if (opcode == 0xFE) {
        const int sub = rd8(pc + 1);
        const u32 tgt = pc + 2;
        switch (sub) {
          case 0x06: case 0x0E: pc = tgt + u32(rds16(tgt)); continue;
          case 0x07:
            if (jump_threshold_ < rd8(pc + 2)) { pc += 5; continue; }
            pc = pc + 3 + u32(rds16(pc + 3));
            continue;
          case 0x08: case 0x09:
            if (rd8(pc + 2) != ch.loop_count[sp] + 1) { pc += 5; continue; }
            sp = (sp - 1) & 3;
            pc = pc + 3 + u32(rds16(pc + 3));
            continue;
          case 0x0F: pc = ch.ret_pc; continue;
          default:
            if (op_len > 0) { pc += u32(op_len); continue; }
        }
      } else if (op_len > 0) {
        pc += u32(op_len);
        continue;
      }
      break;
    }
    channel.legato = channel.full_length = false;
    return 0xA0;
  };

  for (int guard = 0; guard < 4096; ++guard) {
    if (channel.pc >= data.size()) return end_channel(channel);
    const u8* bytes = &data[channel.pc];
    const int op_len = akao_op_length(bytes, data.size() - channel.pc);
    if (op_len == 0) return end_channel(channel);
    const u8 op = bytes[0];
    channel.pc += u32(op_len);

    // --- note / tie / rest
    if (op < 0xA0 || (op >= 0xF0 && op <= 0xFD)) {
      bool is_note = false, is_tie = false;
      int key = 0, len = 0;
      if (op < 0x84) {
        is_note = true;
        key = op / 11;
        len = kLenTable[op % 11];
      } else if (op < 0x8F) {
        is_tie = true;
        len = kLenTable[op - 0x84];
      } else if (op < 0x9A) {
        len = kLenTable[op - 0x8F];
      } else {
        // F0..FD: explicit length byte.
        len = bytes[1];
        if (op < 0xFC) {
          is_note = true;
          key = op - 0xF0;
        } else if (op == 0xFC) {
          is_tie = true;
        }
        if (len == 0) len = std::max(channel.a2_len, 1);
      }
      if (is_note) {
        if (channel.a2_len > 0 && op < 0xF0) len = channel.a2_len;
        channel.a2_len = 0;
        if (channel.fixed_len > 0) len = channel.fixed_len;
        channel.last_len = len;
      }
      // Gate rule: key off at len-2 ticks unless the next opcode continues the note (tie, or a note under legato).
      const int next = peek_next(channel);
      const bool next_tie = (next >= 0x84 && next <= 0x8E) || next == 0xFC;
      const bool next_note = next < 0x84 || (next >= 0xF0 && next <= 0xFB);
      // Legato / full-length flags (CC / D0); a rest cleared them in peek_next.
      const bool continues = next_tie || (next_note && (channel.legato || channel.full_length));
      auto set_gate = [&] {
        // FE 1A tenuto only counts on music channels.
        if (continues || (channel.tenuto && channel.music)) channel.gate_left = 0;
        else channel.gate_left = len > 2 ? len - 2 : (len > 1 ? 1 : 0);
      };
      channel.len_left = len;
      if (is_note) {
        note_on(channel, key);
        start_bend(channel);
        set_gate();
        // The next note under legato only re-pitches.
        channel.legato_prev = channel.legato;
      } else if (is_tie) {
        start_bend(channel);
        set_gate();
        channel.legato_prev = channel.legato;
      } else {
        // Rest: a note still sounding (no gate) is released here, the LFO outputs return to 0, the voice takes no more updates and
        // the portamento ends.
        key_off(channel);
        channel.legato_prev = false;
        channel.porta_steps = 0;
        channel.vib.value = channel.trem.value = 0;
        channel.voice_update = false;
      }
      return;
    }

    // --- control opcodes
    switch (op) {
      case 0xA1: {
        channel.inst_index = bytes[1];
        channel.inst = channel.table ? channel.table->get(bytes[1]) : nullptr;
        if (channel.inst) channel.adsr1 = channel.inst->adsr1, channel.adsr2 = channel.inst->adsr2;
        channel.drum = false;
        channel.patch = -1;
        channel.zone_scale = 0;
        break;
      }
      case 0xA2: channel.a2_len = bytes[1]; channel.fixed_len = 0; break;
      case 0xA3: channel.balance = u16(bytes[1] << 8); break;
      case 0xA4: {
        // Length operand 0 = 256.
        channel.bend_len = slide(bytes[1]);
        channel.bend_delta = s8(bytes[2]);
        break;
      }
      case 0xA5: channel.octave = bytes[1] & 15; break;
      case 0xA6: channel.octave = (channel.octave + 1) & 15; break;
      case 0xA7: channel.octave = (channel.octave - 1) & 15; break;
      // A8 / A9 volumes are signed bytes: 0x80..0xFF is a phase-inverted voice (a pair at +v / -v nearly cancels).
      case 0xA8: channel.volume = s32(s8(bytes[1])) * (1 << 23); channel.vol_slide_left = channel.keyon_slide_len = 0; break;
      case 0xA9: {
        channel.keyon_slide_len = 0;
        channel.vol_slide_left = slide(bytes[1]);
        const s32 prev = channel.volume & s32(0xFFFF0000);
        channel.volume = prev;
        channel.vol_slide_step = (s32(s8(bytes[2])) * (1 << 23) - prev) / channel.vol_slide_left;
        break;
      }
      case 0xAA: channel.pan = u16(((bytes[1] + 0x40) & 0xFF) << 8); channel.pan_slide_left = 0; break;
      case 0xAB: {
        channel.pan_slide_left = slide(bytes[1]);
        const int prev = channel.pan & 0xFF00;
        channel.pan_slide_step = ((((bytes[2] + 0x40) & 0xFF) << 8) - prev) / channel.pan_slide_left;
        channel.pan = u16(prev);
        break;
      }
      case 0xAD: channel.adsr1 = u16((channel.adsr1 & ~0x7F00) | (bytes[1] & 0x7F) << 8); sync_voice_adsr(channel); break;
      case 0xAE: channel.adsr1 = u16((channel.adsr1 & ~0x00F0) | (bytes[1] & 15) << 4); sync_voice_adsr(channel); break;
      case 0xAF: channel.adsr1 = u16((channel.adsr1 & ~0x000F) | (bytes[1] & 15)); sync_voice_adsr(channel); break;
      case 0xB0:
        channel.adsr1 = u16((channel.adsr1 & ~0x00FF) | (bytes[1] & 15) << 4 | (bytes[2] & 15));
        sync_voice_adsr(channel);
        break;
      case 0xB1: channel.adsr2 = u16((channel.adsr2 & ~0x1FC0) | (bytes[1] & 0x7F) << 6); sync_voice_adsr(channel); break;
      case 0xB2: channel.adsr2 = u16((channel.adsr2 & ~0x001F) | (bytes[1] & 0x1F)); sync_voice_adsr(channel); break;
      case 0xB3:
        if (channel.inst) channel.adsr1 = channel.inst->adsr1, channel.adsr2 = channel.inst->adsr2;
        sync_voice_adsr(channel);
        break;
      case 0xB4: {
        Lfo& lfo = channel.vib;
        lfo.on = true;
        int off = 1;
        if (channel.music) {
          lfo.delay = bytes[off++];
        } else {
          lfo.delay = 0;
          if (bytes[off] != 0) lfo.depth = bytes[off] << 8;
          ++off;
        }
        lfo.rate_phase = u32(bytes[off]) << 10;
        if (!lfo.rate_phase) lfo.rate_phase = 0x40000;
        lfo.type = bytes[off + 1];
        lfo.wave = akao_lfo(lfo.type);
        lfo.delay_cur = lfo.delay;
        lfo.counter = 1;
        lfo.rate_slide_left = 0;
        break;
      }
      case 0xB5: channel.vib.depth = bytes[1] << 8; break;
      case 0xB6: channel.vib.on = false; channel.vib.value = 0; break;
      case 0xB7: channel.adsr1 = u16(bytes[1] == 5 ? (channel.adsr1 | 0x8000) : (channel.adsr1 & ~0x8000)); sync_voice_adsr(channel); break;
      case 0xB8: {
        Lfo& lfo = channel.trem;
        lfo.on = true;
        if (channel.music) {
          lfo.delay = bytes[1];
        } else {
          lfo.delay = 0;
          if (bytes[1] != 0) lfo.depth = (bytes[1] & 0x7F) << 8;
        }
        lfo.rate_phase = u32(bytes[2]) << 10;
        if (!lfo.rate_phase) lfo.rate_phase = 0x40000;
        lfo.type = bytes[3];
        lfo.wave = akao_lfo(lfo.type);
        lfo.delay_cur = lfo.delay;
        lfo.counter = 1;
        lfo.rate_slide_left = 0;
        break;
      }
      case 0xB9: channel.trem.depth = (bytes[1] & 0x7F) << 8; break;
      case 0xBA: channel.trem.on = false; channel.trem.value = 0; break;
      case 0xBB: channel.adsr2 = u16((channel.adsr2 & 0x3FFF) | sustain_bits(bytes[1])); sync_voice_adsr(channel); break;
      case 0xBC: {
        Lfo& lfo = channel.apan;
        lfo.on = true;
        lfo.rate_phase = u32(bytes[1]) << 10;
        if (!lfo.rate_phase) lfo.rate_phase = 0x40000;
        lfo.type = bytes[2];
        lfo.wave = akao_lfo(lfo.type);
        lfo.counter = 1;
        lfo.rate_slide_left = 0;
        break;
      }
      // Protect: eviction only ends the channel at its loop end.
      case 0xE0: channel.protect = true; break;
      case 0xBD: channel.apan.depth = bytes[1] << 7; break;
      case 0xBE: channel.apan.on = false; channel.apan.value = 0; break;
      case 0xBF: channel.adsr2 = u16(bytes[1] == 7 ? (channel.adsr2 | 0x20) : (channel.adsr2 & ~0x20)); sync_voice_adsr(channel); break;
      case 0xC0: channel.transpose = s8(bytes[1]); break;
      case 0xC1: channel.transpose += s8(bytes[1]); break;
      case 0xC8:
        channel.loop_top = (channel.loop_top + 1) & 3;
        channel.loop_pc[channel.loop_top] = channel.pc;
        channel.loop_count[channel.loop_top] = 0;
        channel.loop_inf[channel.loop_top] = false;
        break;
      case 0xC9: {
        const int want = bytes[1] ? bytes[1] : 256;
        // An evicted protected channel ends at its loop end.
        if (channel.stop_at_loop) {
          end_channel(channel);
          return;
        }
        if (++channel.loop_count[channel.loop_top] != want) channel.pc = channel.loop_pc[channel.loop_top];
        else channel.loop_top = (channel.loop_top - 1) & 3;
        break;
      }
      case 0xCA:
        if (channel.stop_at_loop) {
          end_channel(channel);
          return;
        }
        ++channel.loop_count[channel.loop_top];
        channel.loop_inf[channel.loop_top] = true;
        channel.pc = channel.loop_pc[channel.loop_top];
        break;
      // C2 / C3: reverb on / off for this channel's voice.
      case 0xC2: channel.reverb = true; break;
      case 0xC3: channel.reverb = false; break;
      // C6: FM on; only the flagged second channel of an SFX pair takes effect. C7: FM off.
      case 0xC6: channel.fm = channel.fm_carrier; break;
      case 0xC7: channel.fm = false; channel.fm_delay = 0; break;
      // C4 / C5: noise on / off. CE: noise on, toggle after N ticks. CF: toggle after N ticks.
      case 0xC4: channel.noise = true; break;
      case 0xC5: channel.noise = false; channel.noise_delay = 0; break;
      case 0xCE: channel.noise = true; channel.noise_delay = bytes[1] ? bytes[1] + 1 : 0x101; break;
      case 0xCF: channel.noise_delay = bytes[1] ? bytes[1] + 1 : 0x101; break;
      case 0xAC: {  // noise clock; bits 7-6 set = add (b & 0x3F) to the current clock, else assign
        int& clk = channel.music ? noise_clock_music_ : noise_clock_sfx_;
        clk = (bytes[1] & 0xC0) ? (clk + (bytes[1] & 0x3F)) & 0x3F : bytes[1];
        // the hardware timer only re-arms at its next underflow; clamping it to the new period avoids a silent start (up to 0.74 s at clock 0)
        noise_timer_ = std::min(noise_timer_, 0x20000 >> (clk >> 2));
        break;
      }
      // D2: enable FM, toggle it after N ticks. D3: toggle after N ticks.
      case 0xD2: channel.fm = channel.fm_carrier; channel.fm_delay = bytes[1] ? bytes[1] + 1 : 0x101; break;
      case 0xD3: channel.fm_delay = bytes[1] ? bytes[1] + 1 : 0x101; break;
      case 0xCB:
        channel.reverb = false;
        channel.noise = false;
        channel.noise_delay = 0;
        channel.fm = false;
        channel.fm_delay = 0;
        channel.vib.on = channel.trem.on = channel.apan.on = false;
        channel.vib.value = channel.trem.value = channel.apan.value = 0;
        channel.legato = channel.full_length = false;
        break;
      // CC: legato on; "previous note was legato" is cleared too, so the first note after CC keys on.
      case 0xCC: channel.legato = true; channel.full_length = channel.legato_prev = false; break;
      case 0xD0:  // SFX channels only: full length (the note is not shortened by 2 ticks)
        if (!channel.music) channel.full_length = true, channel.legato = channel.legato_prev = false;
        break;
      case 0xE1: channel.random_pitch = bytes[1]; break;
      case 0xE2: channel.random_pitch = 0; break;
      case 0xD8: channel.fine_tune = s8(bytes[1]); break;
      case 0xD9: channel.fine_tune += s8(bytes[1]); break;
      // DA: portamento on with legato (the legato mode word, as CC); the next note does not glide since the remembered note is cleared.
      case 0xDA:
        channel.porta_steps = bytes[1] ? bytes[1] : 256;
        channel.last_note = channel.last_transpose = 0;
        channel.legato = true;
        channel.full_length = channel.legato_prev = false;
        break;
      case 0xDB: channel.porta_steps = 0; break;
      case 0xDC: {
        int fixed = s8(bytes[1]);
        if (fixed != 0) fixed = std::clamp(fixed + channel.last_len, 1, 255);
        channel.fixed_len = fixed;
        break;
      }
      case 0xDD: case 0xDE: case 0xDF: {
        Lfo& lfo = op == 0xDD ? channel.vib : op == 0xDE ? channel.trem : channel.apan;
        lfo.depth_slide_left = slide(bytes[1]);
        const int target = op == 0xDD ? bytes[2] << 8 : op == 0xDE ? (bytes[2] & 0x7F) << 8 : bytes[2] << 7;
        lfo.depth_slide_step = (target - lfo.depth) / lfo.depth_slide_left;
        break;
      }
      case 0xE4: case 0xE5: case 0xE6: {
        Lfo& lfo = op == 0xE4 ? channel.vib : op == 0xE5 ? channel.trem : channel.apan;
        lfo.rate_slide_left = slide(bytes[1]);
        u32 target = u32(bytes[2]) << 10;
        if (!target) target = 0x40000;
        lfo.rate_slide_step = (int(target) - int(lfo.rate_phase)) / lfo.rate_slide_left;
        break;
      }
      case 0xFE: {
        const u32 sub = bytes[1];
        switch (sub) {
          case 0x00: tempo_ = double(bytes[2] | bytes[3] << 8); tempo_slide_left = 0; break;
          case 0x02:  // master reverb depth (vLOUT = vROUT = the s16 operand)
            rev_depth_ = s32(s16(bytes[2] | bytes[3] << 8)) << 16;
            rev_slide_left_ = 0;
            break;
          case 0x03: {  // reverb depth slide over n ticks (0 = 256) to the s16 operand
            rev_slide_left_ = slide(bytes[2]);
            const s32 target = s32(s16(bytes[3] | bytes[4] << 8)) << 16;
            rev_slide_step_ = (target - rev_depth_) / rev_slide_left_;
            break;
          }
          case 0x01:
            tempo_slide_left = slide(bytes[2]);
            tempo_slide_step = (double(bytes[3] | bytes[4] << 8) - tempo_) / tempo_slide_left;
            break;
          case 0x04:
            if (channel.code->keymap_abs) channel.drum = true;
            channel.patch = -1;
            break;
          case 0x05: channel.drum = false; break;
          case 0x06: channel.pc = channel.pc - 2 + u32(rds16(channel.pc - 2)); break;
          case 0x07:
            if (jump_threshold_ >= bytes[2]) channel.pc = channel.pc - 2 + u32(rds16(channel.pc - 2));
            break;
          case 0x08: case 0x09: {
            const int want = bytes[2] ? bytes[2] : 256;
            if (channel.loop_count[channel.loop_top] + 1 == want) {
              channel.pc = channel.pc - 2 + u32(rds16(channel.pc - 2));
              if (sub == 0x09) channel.loop_top = (channel.loop_top - 1) & 3;
            }
            break;
          }
          case 0x0A:  // load instrument without attack sample: approximated as a plain load
            channel.inst = channel.table ? channel.table->get(bytes[2]) : nullptr;
            if (channel.inst) channel.adsr1 = channel.inst->adsr1, channel.adsr2 = channel.inst->adsr2;
            channel.drum = false;
            channel.patch = -1;
            break;
          case 0x0B:
            if (collect_sfx_requests) sfx_requests_.push_back({bytes[2], channel.balance >> 8, channel.pan >> 8, sfx_request_mask_});
            sfx_request_mask_ ^= 3;
            break;
          case 0x0E: channel.ret_pc = channel.pc; channel.pc = channel.pc - 2 + u32(rds16(channel.pc - 2)); break;
          case 0x0F: channel.pc = channel.ret_pc; break;
          case 0x12:
            channel.bal_slide_left = slide(bytes[2]);
            channel.balance &= 0x7F00;
            channel.bal_slide_step = ((bytes[3] << 8) - channel.balance) / channel.bal_slide_left;
            break;
          case 0x13: channel.no_rekey = true; break;
          case 0x14:
            channel.patch = bytes[2];
            channel.zones = akao_patch_zones(*channel.code, channel.patch);
            channel.drum = false;
            if (channel.zones.empty()) channel.patch = -1;
            break;
          case 0x19:  // key-on volume slide: signed start volume, length, signed target volume
            channel.keyon_volume = s32(s8(bytes[2])) * (1 << 23);
            channel.keyon_slide_len = slide(bytes[3]);
            channel.keyon_slide_step = (s32(s8(bytes[4])) * (1 << 23) - channel.keyon_volume) / channel.keyon_slide_len;
            break;
          case 0x1A: channel.tenuto = true; break;
          case 0x1B: channel.tenuto = false; break;
          // Voice reservation, bar timers: no audible effect here.
          default: break;
        }
        break;
      }
      default:  // sidechains, no-ops
        // OC_SFX_UNIMPL=1: reports (once per op) the opcodes the SFX programs run that have no audible model here.
        {
          static const bool want = std::getenv("OC_SFX_UNIMPL") != nullptr;
          static std::set<int> seen;
          if (want && !channel.music && seen.insert(op).second) log::info("sfx UNIMPL op {:02X} (first in '{}')", op, channel.tag);
        }
        break;
    }
  }
  // Runaway loop without a length: stop the channel.
  end_channel(channel);
}

void AkaoPlayer::tick_channel(Channel& channel) {
  // SFX channels count the ticks they have run.
  if (!channel.music) ++channel.age;
  // slides (sequencer ticks)
  if (channel.vol_slide_left) {
    --channel.vol_slide_left;
    channel.volume += channel.vol_slide_step;
  }
  if (channel.bal_slide_left) {
    --channel.bal_slide_left;
    channel.balance = u16(channel.balance + channel.bal_slide_step);
  }
  if (channel.pan_slide_left) {
    --channel.pan_slide_left;
    channel.pan = u16(channel.pan + channel.pan_slide_step);
  }
  if (channel.bend_left) {
    --channel.bend_left;
    channel.bend = s32(u32(channel.bend) + u32(channel.bend_step));
  }
  // The noise / FM state toggles after the delay.
  if (channel.noise_delay && --channel.noise_delay == 0) channel.noise = !channel.noise;
  if (channel.fm_delay && --channel.fm_delay == 0) channel.fm = !channel.fm;
  for (Lfo* lfo : {&channel.vib, &channel.trem, &channel.apan}) {
    if (lfo->delay_cur) --lfo->delay_cur;
    if (lfo->rate_slide_left) {
      --lfo->rate_slide_left;
      lfo->rate_phase = u32(int(lfo->rate_phase) + lfo->rate_slide_step);
    }
    if (lfo->depth_slide_left) {
      --lfo->depth_slide_left;
      lfo->depth += lfo->depth_slide_step;
    }
  }
  if (channel.gate_left > 0 && --channel.gate_left == 0) {
    key_off(channel);
    if (!channel.music) channel.voice_update = false;
  }
  if (--channel.len_left <= 0) run_ops(channel);
}

void AkaoPlayer::tick_all() {
  ++ticks_;
  // The reverb depth slides by one step per tick.
  if (rev_slide_left_) {
    --rev_slide_left_;
    rev_depth_ += rev_slide_step_;
  }
  if (tempo_slide_left) {
    --tempo_slide_left;
    tempo_ += tempo_slide_step;
  }
  for (auto& channel : ch_)
    if (channel.active) tick_channel(channel);
}

// ---------------------------------------------------------------------------------------------------- per-update
void AkaoPlayer::update_lfo(Channel& channel) {
  auto step = [](Lfo& lfo, int& out) {
    if (!lfo.on || !lfo.wave || lfo.delay_cur) return false;
    if (--lfo.counter > 0) return false;
    lfo.counter = std::max<int>(1, int(lfo.rate_phase >> 10));
    if (lfo.wave[0] == 0 && lfo.wave[1] == 0) lfo.wave += lfo.wave[2];
    out = *lfo.wave++;
    return true;
  };
  int wave = 0;
  if (step(channel.vib, wave)) {
    const int depth7 = (channel.vib.depth & 0x7F00) >> 8;
    const int vib_base = ((channel.vib.depth & 0x8000) ? depth7 * channel.pitch_base : depth7 * ((channel.pitch_base * 15) >> 8)) >> 7;
    int vib = int((s64(vib_base) * wave) >> 16);
    if (vib >= 0) vib *= 2;
    channel.vib.value = vib;
  }
  // Wave sample, scaled by depth at volume time.
  if (step(channel.trem, wave)) channel.trem.value = wave;
  if (step(channel.apan, wave)) channel.apan.value = int((s64(channel.apan.depth >> 8) * wave) >> 15);
}

void AkaoPlayer::update_channel(Channel& channel, int) {
  if (channel.voice < 0) return;
  Voice& voice = voice_[size_t(channel.voice)];
  if (!voice.active || voice.serial != channel.voice_serial) {
    channel.voice = -1;
    return;
  }
  // The reverb / noise / FM modes reach every voice; volume, pan and pitch only while the channel updates its voice.
  voice.fm = channel.fm && voice.fm_src >= 0;
  voice.reverb = channel.reverb;
  voice.noise = channel.noise;
  if (!channel.voice_update) return;
  // volume: ((Volume>>16) * (Balance>>8)) >> 7, tremolo; music: x master volume (cfg 0x7F) and pan index (pan >> 8) + autopan;
  // SFX: x volume modifier (>> 8) >> 7 and pan index ((pan + pan modifier) >> 8) + autopan
  // + 0x80, no master volume.
  int base = ((channel.volume >> 16) * (channel.balance >> 8)) >> 7;
  if (channel.trem.on) base += int(((s64(base) * (channel.trem.depth >> 8) * 512) >> 16) * channel.trem.value >> 15);
  base = std::clamp(base, -0x8000, 0x7FFF);
  const int chan_pan = channel.drum_pan >= 0 ? (channel.drum_pan + 0x40) << 8 : channel.pan;
  int idx;
  if (channel.music) {
    base = (base * 127) >> 7;
    idx = ((chan_pan >> 8) + channel.apan.value) & 0xFF;
  } else {
    base = (base * channel.vol_mod) >> 7;
    idx = (((chan_pan + (channel.pan_mod << 8)) >> 8) + channel.apan.value + 0x80) & 0xFF;
  }
  int left = (base * kAkaoPanGain[idx]) >> 15;
  int right = (base * kAkaoPanGain[idx ^ 0xFF]) >> 15;
  if (channel.zone_scale) {
    left = (left * channel.zone_scale) >> 7;
    right = (right * channel.zone_scale) >> 7;
  }
  voice.gl = float(left) / 16384.0f;
  voice.gr = float(right) / 16384.0f;

  // pitch register (fine delta + pitch + vibrato, & 0x3FFF); the A4 bend and the DA glide as a 16.16 register ramp
  double reg = channel.pitch_base + channel.vib.value + channel.fine_delta;
  reg += double(channel.bend >> 16);
  // Masked, not clamped.
  voice.reg = int(std::max(reg, 0.0) + 0.5) & 0x3FFF;
}

// ---------------------------------------------------------------------------------------------------- render
// The SPU mixes every voice at 44100 Hz: per output sample the voice's Gaussian filter reads the four newest source samples at
// the interpolation index (counter bits 4..11), the envelope scales it, then counter += pitch register (at most 0x4000) and every
// carry out of bit 11 shifts the next decoded sample in. An end block without the repeat flag silences the voice.
void AkaoPlayer::render_spu(float* out, u32 frames) {
  std::fill(out, out + size_t(frames) * 2, 0.0f);
  u32 done = 0;
  // 183.75 SPU samples per 240 Hz update.
  const double per_update = kHwRate / kUpdateHz;
  while (done < frames) {
    if (upd_left_ <= 0) {
      upd_left_ += per_update;
      if ((++upd_count_ & 3) == 0) tick_60hz();
      tempo_acc_ += u32(std::max(0.0, tempo_ * tempo_scale_));
      while (tempo_acc_ >= 0x10000) {
        tempo_acc_ -= 0x10000;
        tick_all();
      }
      if (sfx_trace_on() && sfx_mode_)
        for (auto& voice : voice_)
          if (voice.active && !voice.long_logged && trace_ms() - voice.started_at * 1000.0 / 48000.0 > 6000.0) {
            voice.long_logged = true;
            log::info("sfxtrace {:9.1f} ms LONG voice {} chan {} released {} env phase {} level {}", trace_ms(), &voice - voice_.data(), voice.chan,
                      voice.released, voice.env.phase, voice.env.level);
          }
      for (int i = 0; i < kMaxChannels; ++i) {
        Channel& channel = ch_[size_t(i)];
        if (channel.active && channel.voice_update) update_lfo(channel);
        update_channel(channel, i);
      }
    }
    const u32 chunk = std::min<u32>(frames - done, std::max<u32>(1, u32(std::ceil(upd_left_))));
    float* dest = out + size_t(done) * 2;
    rev_in_.assign(size_t(chunk) * 2, 0.0f);
    bool any_rev = false;
    for (auto& vo : vout_) vo.assign(chunk, 0);
    noise_buf_.resize(chunk);
    for (u32 i = 0; i < chunk; ++i) noise_buf_[i] = noise_step();
    for (auto& voice : voice_) {
      if (!voice.active) continue;
      const std::vector<s16>& pcm = voice.smp->pcm;
      const u32 base_step = u32(std::min(voice.reg, 0x4000));
      std::vector<s16>& my_out = vout_[size_t(&voice - voice_.data())];
      const std::vector<s16>* fm_in = voice.fm && voice.fm_src >= 0 ? &vout_[size_t(voice.fm_src)] : nullptr;
      for (u32 i = 0; i < chunk; ++i) {
        voice.env.tick();
        if (voice.released && voice.env.level == 0) {
          voice.active = false;
          if (sfx_trace_on() && sfx_mode_) log::info("sfxtrace {:9.1f} ms VOFF voice {} (release done) after {:.0f} ms", trace_ms(), &voice - voice_.data(), trace_ms() - voice.started_at * 1000.0 / 48000.0);
          break;
        }
        const float sample = float(voice.noise ? int(noise_buf_[i]) : gauss_interpolate(voice.hist, int(voice.counter >> 4))) * (1.0f / 32768.0f);
        const float env_gain = float(voice.env.level) * (1.0f / 32768.0f);
        my_out[i] = s16(std::clamp(sample * env_gain * 32768.0f, -32768.0f, 32767.0f));
        dest[i * 2] += sample * env_gain * voice.gl;
        dest[i * 2 + 1] += sample * env_gain * voice.gr;
        if (voice.reverb) {
          rev_in_[i * 2] += sample * env_gain * voice.gl;
          rev_in_[i * 2 + 1] += sample * env_gain * voice.gr;
          any_rev = true;
        }
        // SPU PMON: step = (pitch * (previous voice's output + 0x8000)) >> 15
        const u32 step = fm_in ? u32(std::min<s64>((s64(base_step) * (s64((*fm_in)[i]) + 0x8000)) >> 15, 0x4000)) : base_step;
        voice.counter += step;
        bool ended = false;
        while (voice.counter >= 0x1000) {
          voice.counter -= 0x1000;
          if (voice.next >= pcm.size()) {
            if (!voice.smp->loops) {
              ended = true;
              break;
            }
            voice.next = size_t(voice.smp->loop_start);
          }
          voice.hist[0] = voice.hist[1];
          voice.hist[1] = voice.hist[2];
          voice.hist[2] = voice.hist[3];
          voice.hist[3] = pcm[voice.next++];
        }
        if (ended) {
          voice.active = false;
          if (sfx_trace_on() && sfx_mode_) log::info("sfxtrace {:9.1f} ms VOFF voice {} (sample end) after {:.0f} ms", trace_ms(), &voice - voice_.data(), trace_ms() - voice.started_at * 1000.0 / 48000.0);
          break;
        }
      }
    }
    // SPU reverb: the send of this chunk (float full scale = 2 x the SPU's s16 voice output, see update_channel) through the unit.
    const int depth = rev_depth_ >> 16;
    rev_.set_depth(depth, depth);
    if (any_rev || rev_tail_ > 0) {
      for (u32 i = 0; i < chunk; ++i) {
        int out_left, out_right;
        rev_.process(int(rev_in_[i * 2] * 16384.0f), int(rev_in_[i * 2 + 1] * 16384.0f), out_left, out_right);
        dest[i * 2] += float(out_left) * (1.0f / 16384.0f);
        dest[i * 2 + 1] += float(out_right) * (1.0f / 16384.0f);
      }
      rev_tail_ = any_rev ? kRevTail : (rev_tail_ > int(chunk) ? rev_tail_ - int(chunk) : 0);
    }
    upd_left_ -= chunk;
    done += chunk;
  }
}

void AkaoPlayer::render(float* out, u32 frames) {
  if (frames == 0) return;
  if (rate_ == kSpuRate) {
    render_spu(out, frames);
  } else {
    // Resample the SPU-rate stream (Catmull-Rom over 4 frames). spu_ holds frames from one before floor(rs_pos_) onwards.
    const double ratio = kHwRate / double(rate_);
    const size_t need = size_t(std::floor(rs_pos_ + double(frames - 1) * ratio)) + 3;
    const size_t have = spu_.size() / 2;
    if (need > have) {
      spu_.resize(need * 2);
      render_spu(&spu_[have * 2], u32(need - have));
    }
    for (u32 k = 0; k < frames; ++k) {
      const size_t i = size_t(rs_pos_);
      const float frac = float(rs_pos_ - double(i));
      for (int ch = 0; ch < 2; ++ch) {
        const float x0 = spu_[(i - 1) * 2 + size_t(ch)], x1 = spu_[i * 2 + size_t(ch)], x2 = spu_[(i + 1) * 2 + size_t(ch)],
                    x3 = spu_[(i + 2) * 2 + size_t(ch)];
        out[size_t(k) * 2 + size_t(ch)] =
            x1 + 0.5f * frac * (x2 - x0 + frac * (2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3 + frac * (3.0f * (x1 - x2) + x3 - x0)));
      }
      rs_pos_ += ratio;
    }
    // Keep one frame of history before the read position.
    const size_t drop = size_t(rs_pos_) - 1;
    spu_.erase(spu_.begin(), spu_.begin() + std::ptrdiff_t(drop * 2));
    rs_pos_ -= double(drop);
  }
  if (master_ != 1.0f)
    for (u32 i = 0; i < frames * 2; ++i) out[i] *= master_;
  rendered_ += frames;
}

// ---------------------------------------------------------------------------------------------------- live source switch
SongTiming AkaoPlayer::measure_timing(double max_seconds) {
  SongTiming timing;
  // Channel-state hash -> update index.
  std::unordered_map<u64, u64> seen;
  const u64 limit = u64(std::max(0.0, max_seconds) * kUpdateHz);
  auto mix = [](u64 hash, u64 value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash * 0xFF51AFD7ED558CCDull;
  };
  for (u64 upd = 0; upd < limit; ++upd) {
    const u32 before = ticks_;
    // The 240 Hz update clock of render_spu, without any audio.
    tempo_acc_ += u32(std::max(0.0, tempo_));
    while (tempo_acc_ >= 0x10000) {
      tempo_acc_ -= 0x10000;
      tick_all();
    }
    if (ticks_ == before) continue;
    // The whole sequencer state: where every channel is (pc, rest counter, loop stack with the counts of counted loops; a CA loop that
    // bumps its count forever is left out), its octave / transpose, and the tempo.
    u64 state_hash = 1469598103934665603ull;
    state_hash = mix(state_hash, u64(s64(tempo_)));
    state_hash = mix(state_hash, u64(tempo_slide_left));
    int alive = 0;
    for (const auto& channel : ch_) {
      if (!channel.active || !channel.music) continue;
      ++alive;
      state_hash = mix(state_hash, u64(channel.number));
      state_hash = mix(state_hash, channel.pc);
      state_hash = mix(state_hash, u64(s64(channel.len_left)));
      state_hash = mix(state_hash, u64(channel.loop_top));
      for (int k = 0; k < 4; ++k) state_hash = mix(state_hash, channel.loop_inf[k] ? 0 : u64(std::clamp(channel.loop_count[k], 0, 256)));
      state_hash = mix(state_hash, channel.ret_pc);
      state_hash = mix(state_hash, u64(channel.octave));
      state_hash = mix(state_hash, u64(s64(channel.transpose)));
    }
    // Every channel ended: a one-shot.
    if (alive == 0) {
      timing.length = double(upd) / kUpdateHz;
      return timing;
    }
    const auto [it, fresh] = seen.emplace(state_hash, upd);
    if (!fresh) {
      timing.intro = double(it->second) / kUpdateHz;
      timing.loop = double(upd - it->second) / kUpdateHz;
      timing.length = double(upd) / kUpdateHz;
      return timing;
    }
  }
  return timing;
}

void AkaoPlayer::seek_music(double seconds, double warm) {
  const u64 total = u64(std::max(0.0, seconds) * kUpdateHz + 0.5);
  const u64 warm_u = std::min<u64>(total, u64(std::max(0.0, warm) * kUpdateHz));
  const u64 silent = total - warm_u;
  for (u64 i = 0; i < silent; ++i) {
    tempo_acc_ += u32(std::max(0.0, tempo_));
    while (tempo_acc_ >= 0x10000) {
      tempo_acc_ -= 0x10000;
      tick_all();
    }
  }
  // Voices started during the silent part never sounded: the warm-up re-creates the live ones.
  if (silent > 0) {
    for (auto& voice : voice_) voice = Voice{};
    for (auto& channel : ch_) channel.voice = -1;
    upd_left_ = 0;
  }
  rendered_ += u64(double(silent) / kUpdateHz * rate_ + 0.5);
  u64 frames = u64(double(warm_u) / kUpdateHz * rate_ + 0.5);
  std::vector<float> scratch(size_t(4096) * 2);
  while (frames) {
    const u32 chunk = u32(std::min<u64>(frames, 4096));
    render(scratch.data(), chunk);
    frames -= chunk;
  }
}

// ---------------------------------------------------------------------------------------------------- music push / pop
struct AkaoPlayer::MusicContext {
  // A channel inside a note sounds again on the restore: the sample and envelope it was playing and the gate it then counts down
  // (0 = no key-off before the next opcode).
  struct Rekey {
    bool on = false;
    std::shared_ptr<const AdpcmSample> smp;
    u16 adsr1 = 0, adsr2 = 0;
    int gate = 0;
  };
  std::array<Channel, kMaxChannels> channels;
  std::array<Rekey, kMaxChannels> rekey;
  double tempo = 0x10000;
  int tempo_slide_left = 0;
  double tempo_slide_step = 0;
  u32 tempo_acc = 0;
  u32 ticks = 0;
  s32 rev_depth = 0;
  int rev_slide_left = 0;
  s32 rev_slide_step = 0;
  int noise_clock = 0;
  u64 rendered = 0;
};

std::shared_ptr<const AkaoPlayer::MusicContext> AkaoPlayer::save_context() const {
  auto context = std::make_shared<MusicContext>();
  context->channels = ch_;
  for (size_t i = 0; i < ch_.size(); ++i) {
    const Channel& channel = ch_[i];
    if (!channel.active || !channel.music || !channel.voice_update || channel.no_rekey) continue;
    MusicContext::Rekey& rekey = context->rekey[i];
    const Voice* voice = channel.voice >= 0 ? &voice_[size_t(channel.voice)] : nullptr;
    if (voice && (!voice->active || voice->serial != channel.voice_serial)) voice = nullptr;
    if (voice) {
      rekey.smp = voice->smp;
      rekey.adsr1 = voice->env.adsr1;
      rekey.adsr2 = voice->env.adsr2;
    } else if (channel.bend_inst) {
      rekey.smp = channel.bend_inst->sample;
      rekey.adsr1 = channel.adsr1;
      rekey.adsr2 = channel.adsr2;
    }
    rekey.on = rekey.smp && !rekey.smp->pcm.empty();
    // A pending key-off moves two ticks later; a released note keys off again two ticks before its next opcode, as its gate had.
    if (channel.gate_left > 0) rekey.gate = channel.gate_left + 2;
    else if (voice && !voice->released) rekey.gate = 0;
    else rekey.gate = channel.len_left;
  }
  context->tempo = tempo_;
  context->tempo_slide_left = tempo_slide_left;
  context->tempo_slide_step = tempo_slide_step;
  context->tempo_acc = tempo_acc_;
  context->ticks = ticks_;
  context->rev_depth = rev_depth_;
  context->rev_slide_left = rev_slide_left_;
  context->rev_slide_step = rev_slide_step_;
  context->noise_clock = noise_clock_music_;
  context->rendered = rendered_;
  u32 sounding = 0, held_back = 0, rekeyed = 0;
  for (size_t i = 0; i < ch_.size(); ++i) {
    const Channel& channel = ch_[i];
    if (!channel.active || !channel.music || channel.number < 0 || channel.number > 31) continue;
    const u32 bit = 1u << channel.number;
    if (channel.voice_update) sounding |= bit;
    if (channel.no_rekey) held_back |= bit;
    if (context->rekey[i].on) rekeyed |= bit;
  }
  log::debug("audio: music context saved at tick {}: sounding channels {:08x}, FE 13 {:08x}, keyed again on the resume {:08x}", ticks_,
             sounding, held_back, rekeyed);
  return context;
}

void AkaoPlayer::restore_context(const MusicContext& context) {
  for (auto& voice : voice_) voice = Voice{};
  ch_ = context.channels;
  tempo_ = context.tempo;
  tempo_slide_left = context.tempo_slide_left;
  tempo_slide_step = context.tempo_slide_step;
  tempo_acc_ = context.tempo_acc;
  ticks_ = context.ticks;
  rev_depth_ = context.rev_depth;
  rev_slide_left_ = context.rev_slide_left;
  rev_slide_step_ = context.rev_slide_step;
  noise_clock_music_ = context.noise_clock;
  rendered_ = context.rendered;
  // The ops the player ran at its song load are replaced, so are the FE 0B requests they made; the request mask stays as the last song
  // load left it (1: only music063 has FE 0B).
  sfx_requests_.clear();
  sfx_request_mask_ = 1;
  for (size_t i = 0; i < ch_.size(); ++i) {
    Channel& channel = ch_[i];
    channel.voice = -1;
    if (!channel.active || !channel.music) continue;
    channel.len_left += 2;
    const MusicContext::Rekey& rekey = context.rekey[i];
    if (!rekey.on) {
      if (channel.gate_left > 0) channel.gate_left += 2;
      continue;
    }
    channel.gate_left = rekey.gate;
    const int index = alloc_voice();
    Voice& voice = voice_[size_t(index)];
    voice = Voice{};
    voice.active = true;
    voice.chan = int(i);
    voice.serial = serial_++;
    voice.smp = rekey.smp;
    voice.env.key_on(rekey.adsr1, rekey.adsr2);
    voice.hist[3] = voice.smp->pcm[0];
    voice.reg = channel.pitch_base;
    channel.voice = index;
    channel.voice_serial = voice.serial;
  }
}

std::shared_ptr<const InstrumentTable> load_akao_base_table(const vfs::Vfs& fs) {
  auto table = std::make_shared<InstrumentTable>();
  if (auto bank = fs.read("cdrom/sound/wave/wave0000.dat")) table->overlay(parse_bank(*bank));
  return table;
}

std::optional<AkaoMusic> load_akao_music(const vfs::Vfs& fs, int file_no, const std::shared_ptr<const InstrumentTable>& base) {
  char buf[96];
  std::snprintf(buf, sizeof buf, "cdrom/sound/music/music%03d.dat", file_no);
  auto raw = fs.read(buf);
  if (!raw) {
    log::warn("audio: {} not found", buf);
    return std::nullopt;
  }
  auto seq = parse_seq(std::move(*raw));
  if (!seq) {
    log::warn("audio: {} is not an AKAO sequence", buf);
    return std::nullopt;
  }
  const char* dirs[] = {"cdrom/sound/wave/wave%04d.dat", "cdrom/sound/exwave/wave%04d.dat",
                        "cdrom/battle/effects/syoukan/wave%04d.dat"};
  std::optional<Bytes> bank_raw;
  for (const char* dir : dirs) {
    std::snprintf(buf, sizeof buf, dir, int(seq->bank_id));
    if ((bank_raw = fs.read(buf))) break;
  }
  auto bank = bank_raw ? parse_bank(*bank_raw) : nullptr;
  if (!bank) {
    log::warn("audio: instrument bank {} for music{:03} missing", seq->bank_id, file_no);
    return std::nullopt;
  }
  auto table = std::make_shared<InstrumentTable>(*(base ? base : load_akao_base_table(fs)));
  table->overlay(bank);
  return AkaoMusic{seq, table};
}

}  // namespace oc::audio
