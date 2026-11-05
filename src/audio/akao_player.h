#pragma once
// AKAO sequencer + SPU voice model. One AkaoPlayer is a complete
// engine: channels (VM cursors), 24 hardware voices with ADSR, 240 Hz update clock. Music = one sequence with all its
// channels and a tempo; SFX = any number of independent program channels on the global SFX tick (~96 ticks/s).
// The voices run at the SPU's own rate (44100 Hz: pitch register 0x1000 = one source sample per output sample) with the
// SPU's 4-tap Gaussian interpolation; other output rates are resampled from that stream.
// render() is not thread-safe against the control calls: the mixer drives both from the audio thread.
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio/akao_bank.h"
#include "audio/akao_seq.h"
#include "audio/bgm_switch.h"
#include "audio/spu_reverb.h"
#include "core/types.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::audio {

// OC_SFX_TRACE=1: logs every SFX request / channel start / voice on / voice off / channel end with its tick. The clock is the
// frames the mixer rendered (48 kHz); the mixer advances it.
extern std::atomic<unsigned long long> g_sfx_trace_clock;
bool sfx_trace_on();

// One SFX request: up to two channel programs of one SFX id.
struct SfxProgram {
  std::shared_ptr<const AkaoCode> code;
  u32 pc[2] = {0, 0};
  int count = 0;
  std::shared_ptr<const InstrumentTable> table;
  // Volume modifier (1.0 = 0x7F); pan modifier as 0..127 (64 = centre, stored as pan * 2), -1 = centre.
  float volume = 1.0f;
  int pan = -1;
  // OC_SFX_TRACE label (battle tag / id).
  std::string tag;
  // Channel mask (field: C6 slot 1 << (slot + 8), footsteps 0x40 / 0x80, menus 0x40); 0 = untagged.
  u32 mask = 0;
};

class AkaoPlayer {
 public:
  static constexpr int kVoices = 24;
  static constexpr int kMaxChannels = 40;
  static constexpr u32 kSpuRate = 44100;
  // SFX channel model: 12 channels (mask bits 0x1000..0x800000), channel k always plays on hardware voice 12 + k.
  static constexpr int kSfxChannels = 12;
  static constexpr int kSfxVoiceBase = 12;
  // The channel mask bits the original driver keeps (`& 0xFFFFFF`); higher bits are OpenCross file keys.
  static constexpr u32 kPs1MaskBits = 0xFFFFFF;

  // sfx_mode: channels are SFX programs (global SFX tick, SFX operand variants for B4/B8, SFX volume / pan rule).
  explicit AkaoPlayer(u32 out_rate = 44100, bool sfx_mode = false);

  // Music: replaces everything. The table must contain the sequence's banks.
  void load_music(const std::shared_ptr<const AkaoSeq>& seq, std::shared_ptr<const InstrumentTable> table);
  // SFX: adds one channel running `code` from `pc`. volume = modifier (1 = 0x7F), pan 0..127 (-1 = centre).
  void start_program(const std::shared_ptr<const AkaoCode>& code, u32 pc, std::shared_ptr<const InstrumentTable> table,
                     float volume = 1.0f, int pan = -1, u32 mask = 0, const std::string* tag = nullptr);
  void start_sfx(const SfxProgram& program);
  // Requests that found no free channel / pair and could not steal one (dropped without playing), and channels
  // stolen; counters for the trace and the unit tests.
  int dropped_requests() const { return dropped_; }
  int stolen_channels() const { return stolen_; }
  // Bit k = SFX channel k busy.
  u32 busy_channels() const;
  // OC_SFX_TRACE: lists the channels / voices still running.
  void trace_report(const char* why) const;
  // Field PLAY_SFX rule: cut running SFX channels (E0-protected ones end at their next CA).
  void evict_sfx();
  // With a non-zero mask: only the channels started with an overlapping
  // channel mask end (E0-protected ones end at their next CA). `untagged` also ends channels started without a mask.
  void evict_sfx(u32 mask, bool untagged = false);
  // Commands A0 / A2: the running SFX channels whose
  // mask overlaps `mask` get volume modifier `volume127` (0..127) and pan modifier `pan127` * 2; -1 keeps one.
  void set_sfx_mods(u32 mask, int volume127, int pan127);
  // Command A1, mask path: the volume modifier of every running SFX channel with an overlapping mask slides
  // linearly to volume127 over `frames` 60 Hz steps (every 4th 240 Hz update). The battle effect pack start calls it with
  // (0x100, 60, 0): the spell chant fades out over one second.
  void fade_sfx_volume(u32 mask, int frames, int volume127);
  // SFX tick rate multiplier (fast / slow mode); 1 = the normal rate.
  void set_tempo_scale(double scale) { tempo_scale_ = scale; }
  // Union of the masks of the running SFX channels.
  u32 active_sfx_mask() const;
  // SPU reverb: mode = libspu type, depth = vLOUT/vROUT (s16). Music sets both itself (header +8, FE 02 / FE 03);
  // the SFX player gets the music's (the hardware has one reverb unit; without AKAO music they stay at mode 4 / depth 0x3FFF).
  void set_reverb(int mode, int depth);
  int reverb_mode() const { return rev_.mode(); }
  int reverb_depth() const { return rev_depth_ >> 16; }
  // A bank upload left this sound RAM image: the sounding SFX voices whose sample bytes changed play the new bytes from their
  // current position on, and later notes read the image instead of an older table's copy (the SPU voice reads the RAM live).
  void upload_ram(std::shared_ptr<const std::vector<u8>> ram);
  void stop();                                   // silence everything at once
  void set_master(float gain) { master_ = gain; }  // linear output gain

  // Interleaved stereo float, overwritten. Full scale = 1.0.
  void render(float* out, u32 frames);

  bool finished() const;  // no running channels, no sounding voices and no reverb tail
  int active_channels() const;
  int active_voices() const;
  u32 ticks() const { return ticks_; }
  // Live BGM source switch (call right after load_music, on a player that has not rendered yet).
  // measure_timing: runs the sequencer clock silently (no audio) until the whole channel state recurs = the song loop; the player is
  // consumed (reload it before playing). intro = first recurring state, loop = the period; a song whose channels all end is a one-shot.
  SongTiming measure_timing(double max_seconds = 600.0);
  // seek_music: fast-forwards `seconds` of song time with the sequencer only (tempo changes and loop ops run), then renders the last
  // `warm` seconds to a scratch buffer so the notes still sounding (and the reverb tail) are alive at the target.
  void seek_music(double seconds, double warm = 1.0);
  double seconds() const { return double(rendered_) / rate_; }
  u32 rate() const { return rate_; }
  // Music context push / pop: a copy of the whole sequencer state of the song (every channel with its position, loop stack, note and
  // gate counters, volumes, pans, LFOs and instrument), the tempo with its slide, the reverb depth and the song clock. Voices are not
  // part of it: restore_context silences every voice, gives every channel two more ticks on its note and gate counters, and keys the
  // note of each channel that was inside a note (from its note to the next rest, FE 13 channels excepted) on again from its attack.
  struct MusicContext;
  std::shared_ptr<const MusicContext> save_context() const;
  void restore_context(const MusicContext& context);
  // Diagnostics (ocaudio sfx-pitch): called at every note-on with the sounding note, its instrument and the SPU pitch register.
  std::function<void(int note, const Instrument& in, int reg)> note_trace;
  // Diagnostics (ocaudio render-seq key list, tests): called at every voice key-on and key-off with the 240 Hz update count, the channel number and the voice.
  std::function<void(u32 update, int channel, int voice, bool on)> key_trace;
  // FE 0B: a channel requests effect00 id `id` the way the play-SFX command does (the id, then its chained ids, after the running sounds
  // of the same mask end). volume = the high byte of the channel balance, pan = the high byte of the channel pan (0x80 = centre),
  // mask = 1 and 2 by turns (1 at every song start). The player keeps them while `collect_sfx_requests` is set; the owner of the SFX
  // player takes them after each render (the first ops of a song already run in load_music).
  struct SfxRequest {
    int id = 0, volume = 0, pan = 0x80;
    u32 mask = 0;
  };
  bool collect_sfx_requests = false;
  std::vector<SfxRequest> take_sfx_requests() { return std::exchange(sfx_requests_, {}); }
  // Diagnostics (ocaudio OC_AUDIO_PROBE, tests): one hardware voice's pitch register, pan gains (1.0 = 0x4000) and envelope level.
  struct VoiceProbe {
    bool active = false;
    int reg = 0;
    float gl = 0, gr = 0;
    int level = 0;
  };
  VoiceProbe probe_voice(int index) const;
  // Diagnostics (tests): one channel slot's program position and its vibrato / tremolo / autopan rate words and reload counters.
  struct ChannelProbe {
    bool active = false;
    u32 pc = 0;
    u32 rate[3] = {};
    int counter[3] = {};
  };
  ChannelProbe probe_channel(int index) const;

  // Envelope maths exposed for tests: one hardware-rate (44.1 kHz) step of a SPU ADSR generator.
  struct Env {
    int level = 0;    // 0..0x7FFF
    int phase = 0;    // 0 attack, 1 decay, 2 sustain, 3 release
    int counter = 1;  // hardware ticks until the next step
    u16 adsr1 = 0, adsr2 = 0;
    void key_on(u16 a1, u16 a2) { level = 0; phase = 0; counter = 1; adsr1 = a1; adsr2 = a2; }
    void key_off() { phase = 3; counter = 1; }
    void tick();
  };
  // SPU Gaussian interpolation of four consecutive samples (oldest first) at sub-sample position `index` (0..255).
  static int gauss_interpolate(const s16 (&history)[4], int index);

 private:
  struct Lfo {
    bool on = false;
    int delay = 0, delay_cur = 0;
    int counter = 1;
    u32 rate_phase = 0x40000;
    int depth = 0;
    int type = 0;
    const s16* wave = nullptr;
    int value = 0;  // last sampled output (pitch / volume / pan units)
    int rate_slide_left = 0, rate_slide_step = 0;
    int depth_slide_left = 0, depth_slide_step = 0;
  };
  struct Channel {
    bool active = false;
    bool protect = false, stop_at_loop = false;  // E0 flag / evicted-protected
    bool music = true;
    int number = 0;
    u64 serial = 0;
    // SFX ticks run (starts at -2); the steal rule takes the greatest.
    int age = -2;
    // SFX: the channel mask it was started with (the eviction key).
    u32 mask = 0;
    // OC_SFX_TRACE request number.
    int trace_id = 0;
    std::string tag;
    u64 started_at = 0;
    std::shared_ptr<const AkaoCode> code;
    std::shared_ptr<const InstrumentTable> table;
    u32 pc = 0;
    u32 loop_pc[4] = {};
    int loop_count[4] = {};
    bool loop_inf[4] = {};  // the loop level was closed by CA (infinite): its count is not part of the loop-detection state
    int loop_top = 0;
    u32 ret_pc = 0;
    int len_left = 0, gate_left = 0;
    int octave = 0, transpose = 0;
    int a2_len = 0, fixed_len = 0, last_len = 0;
    bool legato = false, full_length = false, tenuto = false;
    // FE 13: a resumed music context does not key this channel's note on again (kept until the song is loaded again).
    bool no_rekey = false;
    // legato_prev: the previous note op ran under legato (this note does not key on). reverb (C2 / C3): the voice feeds the reverb unit.
    bool legato_prev = false;
    bool reverb = false;
    // The per-update voice parameters (volume, pan, pitch, LFO steps) reach the voice: set by every note, cleared by a rest and the
    // channel end, and on SFX channels by the gate key-off. A released voice decays with the registers it had then.
    bool voice_update = false;
    // instrument
    int inst_index = 0;
    const Instrument* inst = nullptr;
    u16 adsr1 = 0, adsr2 = 0;
    bool drum = false;
    int patch = -1;
    std::vector<AkaoZone> zones;
    // volume / pan
    s32 volume = 0x3FFF0000;
    u16 balance = 0x7F00;
    u16 pan = 0x8000;
    // SFX volume modifier (8.8): vol_mod is the integer part (v * mod >> 7), vol_mod_frac the low byte (only a fade makes it non-zero).
    // A1 fade: frames left (60 Hz) and the signed 8.8 step added per frame. pan_mod: SFX pan modifier, 0x80 = centre.
    int vol_mod_frac = 0;
    int vol_fade_left = 0, vol_fade_step = 0;
    int vol_mod = 0x7F;
    int pan_mod = 0x80;
    int vol_slide_left = 0, vol_slide_step = 0;
    // FE 19: the volume slide every note key-on starts (start volume, length in ticks, step per tick); length 0 = none.
    s32 keyon_volume = 0;
    int keyon_slide_len = 0, keyon_slide_step = 0;
    int bal_slide_left = 0, bal_slide_step = 0;
    int pan_slide_left = 0, pan_slide_step = 0;
    int drum_pan = -1;  // keymap pan override for the current note
    int zone_scale = 0;  // instrument volume scale (v * scale >> 7), 0 = none
    // pitch
    int fine_tune = 0;
    // E1 / E2: random pitch depth (0 = off).
    int random_pitch = 0;
    int pitch_base = 0, fine_delta = 0;
    // DA portamento: steps (0 = off). last_note / last_transpose: the note index (without transpose) and the transpose of the last note
    // or tie, the glide's start; note 0 counts as none.
    int porta_steps = 0;
    int last_note = 0, last_transpose = 0;
    int bend_left = 0;
    // A4 pitch bend: a 16.16 ramp of the pitch REGISTER (bend, bend_step), zeroed by every note. Its operands (bend_len, bend_delta)
    // are armed for the next note or tie, which moves bend_note (the note index without transpose) by the delta and ramps from the
    // current register (running bend included) to that note's pitch; bend_inst is the instrument of the last note.
    s32 bend = 0, bend_step = 0;
    int bend_len = 0, bend_delta = 0;
    int bend_note = 0;
    const Instrument* bend_inst = nullptr;
    Lfo vib, trem, apan;
    // SPU pitch modulation (FM, C6 / C7 / D2 / D3): only the second program of an SFX pair (flag 0x10000) is a carrier;
    // its voice sits right after the first program's voice and is modulated by that voice's output (hardware PMON: voice n by n-1).
    bool fm_carrier = false, fm = false;
    int fm_partner = -1, fm_delay = 0;
    // C4 / C5 / CE / CF: the voice outputs the SPU noise generator instead of its sample; CE / CF toggle it after noise_delay ticks.
    bool noise = false;
    int noise_delay = 0;
    // voice link
    int voice = -1;
    u64 voice_serial = 0;
  };
  struct Voice {
    bool active = false;
    int chan = -1;
    u64 serial = 0;
    std::shared_ptr<const AdpcmSample> smp;
    // SFX voices: the sound RAM the sample was read from and its addresses, so an upload over it reaches the sounding voice.
    std::shared_ptr<const std::vector<u8>> ram;
    u32 spu_start = 0, spu_loop = 0;
    u32 counter = 0;  // SPU pitch counter: bits 4..11 = interpolation index, a carry out of bit 11 = next sample
    size_t next = 1;   // index of the next source sample to enter the history
    s16 hist[4] = {};  // the four samples the Gaussian filter reads (oldest first; hist[3] = current)
    int reg = 0x1000;  // pitch register (0x1000 = one source sample per 44.1 kHz output sample)
    float gl = 0, gr = 0;
    bool reverb = false;
    bool noise = false;
    bool fm = false;   // pitch-modulated by voice fm_src (the carrier channel's C6 state, updated every 240 Hz step)
    int fm_src = -1;
    Env env;
    bool released = false;
    u64 started_at = 0;  // OC_SFX_TRACE
    bool long_logged = false;
  };

  void tick_all();
  void tick_channel(Channel& channel);
  void run_ops(Channel& channel);
  void note_on(Channel& channel, int key);
  void start_bend(Channel& channel);
  void sync_voice_adsr(Channel& channel);
  void key_off(Channel& channel);
  void end_channel(Channel& channel);
  void update_channel(Channel& channel, int channel_index);
  void update_lfo(Channel& channel);
  int alloc_voice();
  int alloc_voice_after(int prev);  // the voice right after `prev` for an FM carrier, or alloc_voice() when it is taken
  int dropped_ = 0, stolen_ = 0;
  int last_ch_ = -1;  // channel slot of the last start_program
  std::array<std::vector<s16>, kVoices> vout_{};  // per-voice envelope-applied output of the current render chunk (FM source)
  Channel* alloc_channel();
  // Channel search (+ steal loop): first slot of program_count adjacent free channels, -1 = dropped.
  int alloc_sfx_slots(int program_count);
  // True when a busy channel was freed.
  bool steal_sfx_channel();
  Channel* begin_sfx_channel(int slot, const std::shared_ptr<const AkaoCode>& code, u32 pc, std::shared_ptr<const InstrumentTable> table,
                             float volume, int pan, u32 mask, const std::string* tag);
  void render_spu(float* out, u32 frames);  // kSpuRate output

  u32 rate_;
  bool sfx_mode_;
  std::shared_ptr<const std::vector<u8>> ram_;  // the last uploaded sound RAM image (SFX mode)
  float master_ = 1.0f;
  std::array<Channel, kMaxChannels> ch_{};
  std::array<Voice, kVoices> voice_{};
  double tempo_scale_ = 1.0;
  double tempo_ = 0x10000;  // FE00 operand units; one sequencer tick per 0x10000 per 240 Hz update
  int tempo_slide_left = 0;
  double tempo_slide_step = 0;
  u32 tempo_acc_ = 0;
  u32 ticks_ = 0;
  // 240 Hz updates; every 4th runs the 60 Hz tasks.
  u32 upd_count_ = 0;
  void tick_60hz();
  // 60 Hz frame counter: the index into kAkaoRandom.
  u8 random_index_ = 0;
  u32 sfx_request_mask_ = 1;
  std::vector<SfxRequest> sfx_requests_;
  double upd_left_ = 0;  // SPU samples until the next 240 Hz update
  u64 rendered_ = 0;
  u64 serial_ = 1;
  int jump_threshold_ = 0;
  SpuReverb rev_;
  // SPU noise generator (psx-spx): timer reload 0x20000 >> shift, step 4 + (clock & 3), 16-bit LFSR. The clock is the SFX one (AC from an
  // SFX channel) while any SFX channel runs, else the music one; range 0..0x3F.
  int noise_clock_sfx_ = 0, noise_clock_music_ = 0;
  s32 noise_timer_ = 0;
  u16 noise_lfsr_ = 0;
  std::vector<s16> noise_buf_;
  s16 noise_step();
  // Reverb depth as (depth << 16), default 0x3FFF; FE 03 slides it per tick.
  s32 rev_depth_ = 0x3FFF << 16;
  int rev_slide_left_ = 0;
  static constexpr int kRevTail = 44100 * 4;  // samples the unit keeps running after the last send (the work area decays)
  int rev_tail_ = 0;
  s32 rev_slide_step_ = 0;
  std::vector<float> rev_in_;     // per-chunk reverb send (stereo)
  std::vector<float> spu_;   // resampler input (SPU-rate stereo frames), one frame of history before rs_pos_
  double rs_pos_ = 1.0;      // read position in spu_ frames (one frame of history before it)
};

// A loaded song: the sequence plus the instrument table (wave0000 shared bank overlaid with the song's own bank).
struct AkaoMusic {
  std::shared_ptr<const AkaoSeq> seq;
  std::shared_ptr<const InstrumentTable> table;
};
// Reads cdrom/sound/music/musicNNN.dat and its bank through the VFS. `base` = pre-built wave0000 table (cache it across
// songs); null loads it. Logs and returns nullopt on failure.
std::optional<AkaoMusic> load_akao_music(const vfs::Vfs& fs, int music_file_no,
                                         const std::shared_ptr<const InstrumentTable>& base = nullptr);
std::shared_ptr<const InstrumentTable> load_akao_base_table(const vfs::Vfs& fs);

}  // namespace oc::audio
