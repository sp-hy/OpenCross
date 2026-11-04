#pragma once
// Audio mixer: 48 kHz stereo float output through an SDL3 audio stream; buses (master, BGM, SFX, UI, movie, voice) with linear
// gains; one BGM channel driven by a thread-safe command queue (play/stop/fade/pause/resume). render() is the
// device-independent core (called from the SDL audio callback, or directly by tests/tools).
//
// BGM gain follows RDE:
//   gain(v) = v * 0.45 / 127 * bgmCfg * masterCfg      v = song volume 0..127, cfg = config percent / 100.
// The channel level is v*0.45/127; the BGM and master bus gains carry the two config factors. Fades are linear in gain.
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/akao_player.h"
#include "audio/hca_stream.h"
#include "core/types.h"

struct SDL_AudioStream;

namespace oc::vfs {
class Vfs;
}

namespace oc::audio {

enum class Bus { Master, Bgm, Sfx, Ui, Movie, Voice, Count };

// SFX channel pool output level before the SFX/master bus gains. 1.0 = the PS1 SPU's own level: AkaoPlayer's output is the SPU mix
// at 44.1 kHz (float 1.0 = s16 full scale), and RDE passes the PS1 stream on scaled by MasterVolume x SfxVolume only.
constexpr float kSfxLevel = 1.0f;
constexpr float kBgmVolumeScale = 0.45f / 127.0f;
// RDE gain(v) with explicit config factors (0..1).
constexpr float bgm_gain(int v127, float bgm_cfg = 1.0f, float master_cfg = 1.0f) {
  return float(v127) * kBgmVolumeScale * bgm_cfg * master_cfg;
}
// Original (AKAO) music level: the PS1 applies the song volume as the music master volume inside the sequencer and mixes the music
// on the same SPU output as the sound effects, so the AKAO stream gets the SFX pool's output level times v/127 (RDE's 0.45 factor
// belongs to its SEAD stream only).
constexpr float akao_gain(int v127) { return float(v127) / 127.0f * kSfxLevel; }

class Mixer {
 public:
  Mixer();
  ~Mixer();
  Mixer(const Mixer&) = delete;
  Mixer& operator=(const Mixer&) = delete;

  static constexpr u32 kRate = 48000;

  // Opens the default playback device (stereo float, 48 kHz) and starts it. False if no device.
  bool open_device();
  void close_device();

  // Bus gains (linear, 0..1+); thread-safe.
  void set_bus_gain(Bus bus, float gain) { bus_[size_t(bus)].store(gain, std::memory_order_relaxed); }
  float bus_gain(Bus bus) const { return bus_[size_t(bus)].load(std::memory_order_relaxed); }

  // BGM commands (any thread). volume is RDE's 0..127; start_seconds as RDE's play command.
  void play(std::shared_ptr<HcaStream> stream, int song_id, int volume127 = 127, double start_seconds = 0.0);
  // Original music: AKAO sequence `musicNNN` (+ its instrument bank) rendered by our own sequencer on the BGM bus.
  // `song_id` is the game's music number (mapped to the file through the song table like play()). Reads the files on
  // the calling thread; returns false (and plays nothing) if they cannot be loaded. Needs set_vfs().
  void set_vfs(const vfs::Vfs* fs) { vfs_ = fs; }
  bool play_akao_bgm(int song_id, int volume127 = 127);
  // SFX channel pool (AKAO programs on the SFX bus; up to 40 concurrent channels, 24 voices).
  void play_sfx(const SfxProgram& program);
  void stop_sfx();
  // OC_SFX_TRACE: logs the SFX channels / voices still running.
  void trace_report(const char* why);
  // Field PLAY_SFX rule: cut the running SFX channels first (AkaoPlayer::evict_sfx).
  void evict_sfx();
  // Only the channels started with an overlapping channel mask; `untagged` also ends the channels started without a mask.
  void evict_sfx(u32 mask, bool untagged = false);
  // Volume / pan modifiers of the running channels whose mask overlaps (volume 0..127, pan 0..127; -1 keeps).
  void set_sfx_mods(u32 mask, int volume127, int pan127);
  // The channels with an overlapping mask slide to volume127 over `frames` 60 Hz steps.
  void fade_sfx_volume(u32 mask, int frames, int volume127);
  // True while a channel started with an overlapping mask runs (or its start is still queued).
  bool sfx_active(u32 mask) const {
    return ((sfx_active_.load(std::memory_order_acquire) | sfx_pending_.load(std::memory_order_acquire)) & mask) != 0;
  }
  // The one instrument table / sound RAM the effect banks share: each loader (field room, battle) uploads over what the last one left
  // and publishes the result here (null until the first room). Main thread.
  const std::shared_ptr<const InstrumentTable>& sfx_instruments() const { return sfx_instruments_; }
  // A new sound RAM image reaches the SFX voices that are sounding (AkaoPlayer::upload_ram).
  void set_sfx_instruments(std::shared_ptr<const InstrumentTable> table);

  // Movie PCM stream (own bus): the movie player pushes interleaved stereo float at kRate from its decoder thread
  // (single producer); the audio thread drains it. movie_frames_played() is the audio clock for A/V sync.
  // movie_start clears the ring and the clock and stays silent until movie_run starts consuming (and the clock counting). `level`
  // scales this movie's sound on top of the movie bus (the field movie table row's volume / 127). The music keeps playing.
  void movie_start(float level = 1.0f);
  void movie_run();
  void movie_stop();
  // Returns the frames accepted (the ring may be full).
  u32 movie_push(const float* stereo, u32 frames);
  u32 movie_free_frames() const;
  u64 movie_frames_played() const { return movie_played_.load(std::memory_order_acquire); }
  bool movie_active() const { return movie_on_.load(std::memory_order_acquire); }

  // Voice bus: one streaming-style channel playing a fully decoded 48 kHz interleaved stereo float line (audio::VoicePlayer decodes
  // the MP3 on a worker thread). voice_play replaces the running line; any thread.
  void voice_play(std::shared_ptr<const std::vector<float>> pcm);
  void voice_stop();
  bool voice_active() const { return voice_on_.load(std::memory_order_acquire); }

  void stop();
  // No-op if nothing is playing.
  void fade(int target_volume127, double seconds);
  void pause();
  void resume();
  // Pause screen (RDE suspends its sound thread): the music (remastered BGM and the AKAO sequencer), the voice line and the movie
  // PCM ring with its clock stand still, silent, until hold(false). The SFX engine keeps running so the pause menu's own sounds play
  // (one-shots run out). Any thread; independent of pause() / resume() (the BGM-only pause).
  void hold(bool on) { hold_.store(on, std::memory_order_release); }
  bool held() const { return hold_.load(std::memory_order_acquire); }

  // Live [Audio] BgmSource switch: restarts the playing song on the other source at the same musical position (same volume / fade
  // state, ~15 ms declick instead of a gap). `song` = the PS1 song id on the NEW source (-1 = the current id; the remastered battle
  // theme 0x14 <-> AKAO song 2 differ). Main thread (reads the song files). False when nothing plays or the new source cannot load;
  // true also when the song already plays on that source. Position: bgm_seconds() mapped through map_song_position().
  bool switch_bgm_source(bool remastered, int song = -1);
  bool bgm_is_akao() const { return bgm_akao_.load(std::memory_order_relaxed); }
  // Seconds since the song start, unwrapped across loops.
  double bgm_seconds() const { return bgm_secs_.load(std::memory_order_relaxed); }

  // Status (audio thread updates; relaxed reads). current_song: -1 = none; bgm_position in samples.
  int current_song() const { return song_.load(std::memory_order_relaxed); }
  bool playing() const { return song_.load(std::memory_order_relaxed) >= 0; }
  u32 bgm_position() const { return bgm_pos_.load(std::memory_order_relaxed); }
  bool device_open() const { return stream_ != nullptr; }
  // Settings row "Sound: Stereo / Monaural": the final mix is folded to L = R = (L + R) / 2.
  void set_mono(bool on) { mono_.store(on, std::memory_order_relaxed); }
  float bgm_level() const { return level_view_.load(std::memory_order_relaxed); }

  // Produces `frames` stereo float frames (interleaved) by draining the queue and mixing all channels.
  void render(float* out, u32 frames);

  // Measurement hook (env OC_AUDIO_DUMP): without an output device, capture_tick(dt) renders dt seconds of game time into a
  // 48 kHz stereo 16-bit WAV, so scripted runs can check the audio across room changes. Closed by the destructor.
  bool start_capture(const std::string& wav_path);
  void capture_tick(double seconds);
  u64 captured_frames() const { return capture_frames_; }

 private:
  struct Cmd {
    enum Type { Play, PlayAkao, Stop, Fade, Pause, Resume, Sfx, StopSfx, EvictSfx, SfxMods, SfxFade, SpuUpload, Switch, VoicePlay, VoiceStop } type;
    std::shared_ptr<HcaStream> stream;
    int song = -1;
    int volume = 0;
    double seconds = 0;
    std::shared_ptr<AkaoPlayer> akao;
    std::shared_ptr<const std::vector<float>> voice;  // VoicePlay
    SfxProgram sfx;
    std::shared_ptr<const std::vector<u8>> ram;  // SpuUpload
    u32 mask = 0;  // EvictSfx: 0 = every SFX channel, else the channel mask; SfxMods: the channels to change
    bool untagged = false;  // EvictSfx with a mask: also the untagged channels
    int pan = -1;           // SfxMods (volume in `volume`); SfxFade: the frames
    int vol_target = -1;    // Switch: volume the running fade ends at (-1 = none); `seconds` = the start position, fade_secs its rest
    double fade_secs = 0;
  };
  void push(Cmd cmd);
  void apply(const Cmd& cmd);
  // Sets the BGM level at once (no fade) and unpauses.
  void arm_level(float gain);
  // One frame of the linear BGM level ramp.
  void step_fade();

  static constexpr u32 kMovieRing = 1u << 17;  // frames (~2.7 s)
  std::vector<float> movie_ring_;                // kMovieRing * 2 floats
  std::atomic<u64> movie_w_{0}, movie_r_{0}, movie_played_{0};
  std::atomic<bool> movie_on_{false}, movie_flush_{false};
  std::atomic<float> movie_level_{1.0f};
  std::shared_ptr<const std::vector<float>> voice_;  // audio thread
  size_t voice_pos_ = 0;                             // frames played
  std::atomic<bool> voice_on_{false};

  std::shared_ptr<const InstrumentTable> sfx_instruments_;
  std::atomic<float> bus_[size_t(Bus::Count)];
  std::mutex mu_;
  std::vector<Cmd> queue_;

  // Audio-thread state.
  std::shared_ptr<HcaStream> bgm_;
  std::shared_ptr<AkaoPlayer> akao_;
  std::unique_ptr<AkaoPlayer> sfx_player_;
  std::vector<float> fscratch_;
  const vfs::Vfs* vfs_ = nullptr;
  std::shared_ptr<const InstrumentTable> base_table_;  // wave0000 (shared system bank), loaded on first use
  bool paused_ = false;
  std::atomic<bool> hold_{false};
  float level_ = 0, level_target_ = 0, level_step_ = 0;  // per-frame linear ramp
  u32 fade_left_ = 0;
  std::vector<s16> scratch_;
  std::atomic<int> song_{-1};
  std::atomic<bool> mono_{false};
  std::atomic<u32> sfx_active_{0};   // union of the running SFX channels' masks (audio thread)
  std::atomic<u32> sfx_pending_{0};  // masks of queued SFX starts not applied yet
  std::FILE* capture_ = nullptr;
  u64 capture_frames_ = 0;
  double capture_carry_ = 0;
  std::atomic<u32> bgm_pos_{0};
  std::atomic<float> level_view_{0};
  // Published by the audio thread for the live source switch.
  std::atomic<bool> bgm_akao_{false};
  std::atomic<double> bgm_secs_{0};
  std::atomic<float> level_target_view_{0};
  std::atomic<u32> fade_left_view_{0};
  double bgm_base_ = 0;    // HCA: song seconds at bgm_frames_ == 0 (the start / seek position)
  u64 bgm_frames_ = 0;     // HCA: frames played since bgm_base_

  SDL_AudioStream* stream_ = nullptr;
  static void sdl_callback(void* user, SDL_AudioStream* stream, int additional, int total);
};

}  // namespace oc::audio
