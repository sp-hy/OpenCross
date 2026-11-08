// ocaudio: remastered BGM tool.
//   ocaudio [-g <game dir>] decode <song id | BGMnnn | file.sab> <out.wav> [seconds]   (no seconds = whole track, no looping)
//   ocaudio [-g <game dir>] play <song id | BGMnnn> [seconds]
//   ocaudio [-g <game dir>] render-seq <music NNN> <out.wav> [seconds] [rate] [keys.txt]   original AKAO music through our sequencer
//     (with the sound effects its FE 0B ops request; keys.txt lists every voice key-on / key-off: update, KON / KOFF, channel, voice)
//   ocaudio [-g <game dir>] play-seq <music NNN> [seconds]
//   ocaudio [-g <game dir>] render-sfx <sfx id> <out.wav> [seconds] [rate] [room]   (rate default 44100 = the SPU rate; 48000 = the mixer;
//     room = the room whose yos0 bank overlays instruments 208..223; battle:<room> = that room entered again after a battle;
//     sfx id r<n> = the room's yos1 frame n)
//   ocaudio [-g <game dir>] sfx-pitch | sfx-bytes <id> | room-sfx <room> | banks   pitch-register audit against the retail formula
//   ocaudio [-g <game dir>] timing | switch-test <song> <seconds> [volume] | inst-dump <index> | sfx-ops
//   ocaudio [-g <game dir>] play-sfx <sfx id> [seconds]
// The game dir defaults to $OPENCROSS_GAMEDIR.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/akao_bank.h"
#include "audio/akao_player.h"
#include "audio/akao_seq.h"
#include "audio/bgm_table.h"
#include "audio/hca_stream.h"
#include "audio/mixer.h"
#include "audio/sfx.h"
#include "import/container.h"
#include "import/room.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: ocaudio [-g <game dir>] decode <song id|BGMnnn|file.sab> <out.wav> [seconds]\n"
               "       ocaudio [-g <game dir>] play <song id|BGMnnn> [seconds]\n"
               "       ocaudio [-g <game dir>] render-seq <music NNN> <out.wav> [seconds] [rate] [keys.txt]\n"
               "       ocaudio [-g <game dir>] play-seq <music NNN> [seconds]\n"
               "       ocaudio [-g <game dir>] render-sfx <sfx id | r<frame>> <out.wav> [seconds] [rate] [room]\n"
               "       ocaudio [-g <game dir>] sfx-pitch\n"
               "       ocaudio [-g <game dir>] room-sfx <room>\n"
               "       ocaudio [-g <game dir>] sfx-bytes <sfx id>\n"
               "       ocaudio [-g <game dir>] banks\n"
               "       ocaudio [-g <game dir>] timing\n"
               "       ocaudio [-g <game dir>] switch-test <song> <seconds> [volume]\n"
               "       ocaudio [-g <game dir>] inst-dump <index>\n"
               "       ocaudio [-g <game dir>] sfx-ops\n"
               "       ocaudio [-g <game dir>] play-sfx <sfx id> [seconds]\n");
  return 2;
}

// "BGM002" -> file number; plain number -> song id (through the exe table). Returns file number or -1.
int resolve(const std::string& text, int& song_id) {
  song_id = -1;
  if (text.size() > 3 && (text[0] == 'B' || text[0] == 'b')) return std::atoi(text.c_str() + 3);
  if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0]))) return -1;
  const audio::BgmEntry* entry = audio::bgm_entry(std::atoi(text.c_str()));
  if (!entry) return -1;
  song_id = entry->song_id;
  return entry->file_no;
}

bool write_wav(const std::string& path, const std::vector<s16>& pcm, u32 rate) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  u32 bytes = u32(pcm.size() * 2);
  auto w32 = [&](u32 value) { std::fwrite(&value, 4, 1, file); };
  auto w16 = [&](u16 value) { std::fwrite(&value, 2, 1, file); };
  std::fwrite("RIFF", 1, 4, file);
  w32(36 + bytes);
  std::fwrite("WAVEfmt ", 1, 8, file);
  w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16);
  std::fwrite("data", 1, 4, file);
  w32(bytes);
  std::fwrite(pcm.data(), 2, pcm.size(), file);
  std::fclose(file);
  return true;
}

double rms(const std::vector<s16>& pcm, size_t frame0, size_t frames) {
  if (frame0 * 2 >= pcm.size()) return 0;
  size_t end = std::min(pcm.size(), (frame0 + frames) * 2);
  double acc = 0;
  for (size_t i = frame0 * 2; i < end; ++i) acc += double(pcm[i]) * pcm[i];
  size_t count = end - frame0 * 2;
  return count ? std::sqrt(acc / double(count)) : 0;
}

std::shared_ptr<audio::HcaStream> open_stream(const std::optional<Bytes>& bytes, const std::string& name) {
  if (!bytes) {
    std::fprintf(stderr, "not found: %s\n", name.c_str());
    return nullptr;
  }
  auto stream = std::make_shared<audio::HcaStream>();
  if (!stream->open(*bytes)) {
    std::fprintf(stderr, "%s: cannot open as SAB (HCA or 16-bit PCM)\n", name.c_str());
    return nullptr;
  }
  return stream;
}

std::shared_ptr<audio::HcaStream> load(const vfs::Vfs& fs, int file_no) { return open_stream(fs.read(audio::bgm_path(file_no)), audio::bgm_path(file_no)); }

// A .sab file on disk, such as a replacement track.
std::optional<Bytes> read_disk_file(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return std::nullopt;
  Bytes bytes;
  std::vector<u8> chunk(65536);
  for (size_t got; (got = std::fread(chunk.data(), 1, chunk.size(), file)) > 0;) bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + std::ptrdiff_t(got));
  std::fclose(file);
  return bytes;
}

int cmd_decode(const vfs::Vfs& fs, const std::string& which, const std::string& out, double seconds) {
  const bool disk = which.size() > 4 && which.ends_with(".sab");
  int song = -1, file = disk ? -1 : resolve(which, song);
  if (!disk && file < 0) return usage();
  auto stream = disk ? open_stream(read_disk_file(which), which) : load(fs, file);
  if (!stream) return 1;
  char label[16];
  std::snprintf(label, sizeof label, "BGM%03d", file);
  const std::string name = disk ? which : label;
  const u32 rate = stream->sample_rate();
  const bool whole = seconds <= 0;
  stream->set_looping(!whole);
  u32 want = whole ? stream->total_samples() + 1 : u32(seconds * rate);
  std::vector<s16> pcm = stream->read(want);
  size_t frames = pcm.size() / 2;
  std::printf("%s: %u Hz, total_samples %u, loop %u..%u, decoded %zu frames (%.3f s)\n", name.c_str(), rate, stream->total_samples(),
              stream->loop_start(), stream->loop_end(), frames, double(frames) / rate);
  std::printf("RMS first 2 s: %.1f   overall: %.1f", rms(pcm, 0, size_t(2) * rate), rms(pcm, 0, frames));
  if (frames > size_t(32) * rate) std::printf("   2 s at 30 s: %.1f", rms(pcm, size_t(30) * rate, size_t(2) * rate));
  std::printf("\n");
  if (const audio::BgmEntry* entry = audio::bgm_entry_by_file(file); entry && whole)
    std::printf("table total_samples %u: %s\n", entry->total_samples, frames == entry->total_samples ? "MATCH" : "MISMATCH");
  if (!write_wav(out, pcm, rate)) {
    std::fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  std::printf("wrote %s\n", out.c_str());
  return 0;
}

int cmd_play(const vfs::Vfs& fs, const std::string& which, double seconds) {
  int song = -1, file = resolve(which, song);
  if (file < 0) return usage();
  auto stream = load(fs, file);
  if (!stream) return 1;
  audio::Mixer mixer;
  if (!mixer.open_device()) return 1;
  mixer.play(stream, song < 0 ? file : song, 127, 0.0);
  std::printf("playing BGM%03d for %.1f s\n", file, seconds);
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::printf("position %u samples, level %.4f\n", mixer.bgm_position(), double(mixer.bgm_level()));
  mixer.close_device();
  return 0;
}

int cmd_render_seq(const vfs::Vfs& fs, const std::string& which, const std::string& out, double seconds, u32 rate, const std::string& keys) {
  // `which` is the music file number (musicNNN.dat).
  const int file = std::atoi(which.c_str());
  if (file <= 0) return usage();
  auto music = audio::load_akao_music(fs, file);
  if (!music) return 1;
  audio::SfxBank bank;
  const auto triggers = bank.load(fs) ? audio::make_sfx_triggers(bank, *music->seq) : nullptr;
  audio::AkaoPlayer pl(rate, false), sfx(rate, true);
  pl.collect_sfx_requests = triggers != nullptr;
  FILE* key_file = keys.empty() ? nullptr : std::fopen(keys.c_str(), "w");
  if (key_file)
    pl.key_trace = [&](u32 update, int channel, int voice, bool on) { std::fprintf(key_file, "%u %s %d %d\n", update, on ? "KON " : "KOFF", channel, voice); };
  pl.load_music(music->seq, music->table);
  const u32 frames = u32(std::max(0.1, seconds) * rate);
  std::vector<float> buf(size_t(frames) * 2), sfx_buf;
  int max_voices = 0, requests = 0;
  // OC_AUDIO_PROBE=<csv>: every active voice after each 1/240 s step, as for render-sfx.
  const char* probe_path = std::getenv("OC_AUDIO_PROBE");
  FILE* probe = probe_path ? std::fopen(probe_path, "w") : nullptr;
  if (probe) std::fprintf(probe, "step,tick,voice,reg,vl,vr,level\n");
  // Rendered in 1/240 s steps (one sequencer update) so the voice stats are sampled and a requested sound effect starts in the update
  // that requested it.
  for (u32 done = 0, step = 0; done < frames; ++step) {
    const u32 chunk = std::min(frames, std::max(done + 1, u32((u64(step) + 1) * rate / 240))) - done;
    pl.render(&buf[size_t(done) * 2], chunk);
    for (int i = 0; probe && i < audio::AkaoPlayer::kVoices; ++i) {
      const audio::AkaoPlayer::VoiceProbe voice = pl.probe_voice(i);
      if (voice.active)
        std::fprintf(probe, "%u,%u,%d,%d,%d,%d,%d\n", step, pl.ticks(), i, voice.reg, int(std::lround(voice.gl * 0x4000)),
                     int(std::lround(voice.gr * 0x4000)), voice.level);
    }
    if (triggers) {
      const auto pending = pl.take_sfx_requests();
      requests += int(pending.size());
      triggers->play(sfx, pending);
      sfx.set_reverb(pl.reverb_mode(), pl.reverb_depth());
      sfx_buf.resize(size_t(chunk) * 2);
      sfx.render(sfx_buf.data(), chunk);
      for (size_t i = 0; i < sfx_buf.size(); ++i) buf[size_t(done) * 2 + i] += sfx_buf[i];
    }
    max_voices = std::max(max_voices, pl.active_voices());
    done += chunk;
  }
  if (key_file) std::fclose(key_file);
  if (probe) std::fclose(probe);
  std::vector<s16> pcm(buf.size());
  float peak = 0;
  for (size_t i = 0; i < buf.size(); ++i) {
    peak = std::max(peak, std::fabs(buf[i]));
    pcm[i] = s16(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f);
  }
  std::printf("music%03d: bank %u, %zu channels, gate mask 0x%X, %.1f s rendered, ticks %u, max voices %d, channels left %d, "
              "sound effect requests %d\n",
              file, music->seq->bank_id, music->seq->channels.size(), music->seq->gate_mask, double(frames) / rate, pl.ticks(), max_voices,
              pl.active_channels(), requests);
  std::printf("RMS %.1f  peak %.3f (full scale 1.0)\n", rms(pcm, 0, frames), double(peak));
  if (!write_wav(out, pcm, rate)) {
    std::fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  std::printf("wrote %s\n", out.c_str());
  return 0;
}

int cmd_play_seq(const vfs::Vfs& fs, const std::string& which, double seconds) {
  // `which` is the music file number (musicNNN.dat).
  const int file = std::atoi(which.c_str());
  if (file <= 0) return usage();
  audio::Mixer mixer;
  mixer.set_vfs(&fs);
  if (!mixer.open_device()) return 1;
  // play_akao_bgm takes the game's song id.
  int song = file;
  for (int i = 0; i < audio::kBgmSongCount + 8; ++i)
    if (const audio::BgmEntry* entry = audio::bgm_entry(i); entry && entry->file_no == file) song = entry->song_id;
  if (!mixer.play_akao_bgm(song)) return 1;
  std::printf("playing music%03d (AKAO) for %.1f s\n", file, seconds);
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  mixer.close_device();
  return 0;
}

int cmd_render_sfx(const vfs::Vfs& fs, const std::string& which, const std::string& out, double seconds, u32 rate, const std::string& room) {
  const bool room_frame = !which.empty() && which[0] == 'r';
  const int id = std::atoi(which.c_str() + (room_frame ? 1 : 0));
  audio::SfxBank bank;
  if (!bank.load(fs)) {
    std::fprintf(stderr, "cannot load effect00.dat / wave0000.dat\n");
    return 1;
  }
  // The room's yos0 bank overlays instruments 208..223 for the global effects too. "battle:<room>" = the room entered again after a
  // battle: battle.prd's bank (176..207, sound RAM 0x1100) was uploaded before the room's banks.
  if (!room.empty()) {
    const bool after_battle = room.rfind("battle:", 0) == 0;
    const std::string name = after_battle ? room.substr(7) : room;
    auto bin = fs.read("cdrom/map/mapbin/" + name + ".bin");
    auto ctd = fs.read("cdrom/map/mapbin/" + name + ".ctd");
    std::string err;
    auto scene = bin && ctd ? import::load_room(name, *bin, *ctd, {}, &err) : std::nullopt;
    if (!scene) {
      std::fprintf(stderr, "room %s not loadable\n", name.c_str());
      return 1;
    }
    std::shared_ptr<const audio::InstrumentTable> current;
    if (after_battle) {
      auto battle_prd = fs.read("cdrom/battle/battle.prd");
      auto parsed = battle_prd ? import::parse_drp(*battle_prd) : std::nullopt;
      if (!parsed) return 1;
      auto table = std::make_shared<audio::InstrumentTable>(*bank.table());
      for (const import::DrpEntry& entry : parsed->entries)
        if (entry.type == 5) table->overlay(audio::parse_bank(entry.data));
      current = table;
    }
    bank.set_room(scene->yos0_bytes, scene->yos1_bytes, current);
  }
  audio::SfxProgram program;
  std::vector<audio::SfxProgram> extra;
  if (room_frame ? !bank.make_room(id, program, &extra) : !bank.make_global(id, program, &extra)) {
    std::fprintf(stderr, "sfx %d is not defined (%d ids defined)\n", id, bank.defined_ids());
    return 1;
  }
  // OC_SFX_ONLY=0|1 plays just program 0 or 1 of the pair, with no chained ids.
  if (const char* only = std::getenv("OC_SFX_ONLY")) {
    const int only_index = std::atoi(only);
    if (only_index >= 0 && only_index < program.count) {
      program.pc[0] = program.pc[only_index];
      program.count = 1;
      extra.clear();
    }
  }
  audio::AkaoPlayer pl(rate, true);
  pl.start_sfx(program);
  for (auto& chained : extra) pl.start_sfx(chained);
  const u32 frames = u32(std::max(0.1, seconds) * rate);
  std::vector<float> buf(size_t(frames) * 2);
  // OC_AUDIO_PROBE=<csv>: renders in 1/240 s steps (one sequencer update each) and logs every active voice after each one (step,
  // sequencer tick, voice, pitch register, pan gains in voice volume units, envelope level).
  const char* probe_path = std::getenv("OC_AUDIO_PROBE");
  FILE* probe = probe_path ? std::fopen(probe_path, "w") : nullptr;
  if (probe) {
    std::fprintf(probe, "step,tick,voice,reg,vl,vr,level\n");
    for (u32 done = 0, step = 0; done < frames; ++step) {
      const u32 next = std::min(frames, u32((u64(step) + 1) * rate / 240));
      pl.render(buf.data() + size_t(done) * 2, next - done);
      done = next;
      for (int i = 0; i < audio::AkaoPlayer::kVoices; ++i) {
        const audio::AkaoPlayer::VoiceProbe voice = pl.probe_voice(i);
        if (voice.active)
          std::fprintf(probe, "%u,%u,%d,%d,%d,%d,%d\n", step, pl.ticks(), i, voice.reg, int(std::lround(voice.gl * 0x4000)),
                       int(std::lround(voice.gr * 0x4000)), voice.level);
      }
    }
    std::fclose(probe);
  } else {
    pl.render(buf.data(), frames);
  }
  std::vector<s16> pcm(buf.size());
  for (size_t i = 0; i < buf.size(); ++i) pcm[i] = s16(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f);
  std::printf("sfx %d: %d programs (+%zu chained), RMS %.1f, finished %s\n", id, program.count, extra.size(), rms(pcm, 0, frames),
              pl.finished() ? "yes" : "no");
  return write_wav(out, pcm, rate) ? 0 : 1;
}

// The retail pitch-register calculation with its 32-bit unsigned arithmetic; channel fine tune 0.
int reference_pitch_reg(int note, const audio::Instrument& in) {
  static const unsigned semi[12] = {0x1000, 0x10F3, 0x11F5, 0x1306, 0x1428, 0x155B, 0x16A0, 0x17F9, 0x1966, 0x1AE8, 0x1C82, 0x1E34};
  int degree = note - in.base_note;
  while (degree < 0) degree += 12;
  degree %= 12;
  const int ft = in.fine_tune;
  unsigned reg;
  if (ft == 0) reg = semi[degree] << 8;
  else if (ft < 0) reg = (semi[degree] * unsigned(u16(ft))) >> 8;
  else reg = ((unsigned(semi[degree] * unsigned(ft))) >> 7) + semi[degree] * 0x100u;
  int shifted = note;
  if (shifted < in.base_note) {
    do { reg = unsigned(int(reg) >> 1); shifted += 12; } while (shifted < in.base_note);
  } else {
    const unsigned oct = unsigned(shifted - in.base_note) / 12;
    if (oct) reg <<= (oct & 31);
  }
  return int(unsigned(int(reg) >> 8) & 0xFFFF);
}

// sfx-pitch: per SFX id the first notes' instrument (base note, fine tune), note, our pitch register and the retail one; lists mismatches.
int cmd_sfx_pitch(const vfs::Vfs& fs) {
  audio::SfxBank bank;
  if (!bank.load(fs)) return 1;
  int bad = 0;
  std::map<int, u32> used;
  for (int id = 0; id < 256; ++id) {
    audio::SfxProgram program;
    if (!bank.make_global(id, program, nullptr)) continue;
    audio::AkaoPlayer pl(44100, true);
    int printed = 0;
    pl.note_trace = [&](int note, const audio::Instrument& in, int reg) {
      int idx = -1;
      for (int k = 0; k < 256; ++k)
        if (program.table->get(k) == &in) idx = k;
      used[idx] |= 1u << (id & 31);
      const int ref = reference_pitch_reg(note, in);
      const bool diff = ref != reg;
      bad += diff;
      if (printed++ < 3 || diff)
        std::printf("sfx %3d (0x%03X) inst %3d note %3d inst base %3d ft %5d  reg 0x%04X (x%.3f) reference 0x%04X (SPU reg 0x%04X) %s\n", id, id + 0x100, idx, note,
                    in.base_note, in.fine_tune, reg, reg / 4096.0, ref, reg & 0x3FFF, diff ? "DIFF" : "");
    };
    pl.start_sfx(program);
    std::vector<float> buf(44100 * 2);
    pl.render(buf.data(), 44100);
  }
  std::printf("global-SFX instrument indices used:");
  for (auto& [index, id_mask] : used) std::printf(" %d", index);
  std::printf("\n");
  std::printf("%d mismatching notes\n", bad);
  return 0;
}

// room-sfx <room>: the same trace for the room's yos1 frames (footstep frames 0..3 among them), with the room bank's instruments.
int cmd_room_sfx(const vfs::Vfs& fs, const std::string& room) {
  auto bin = fs.read("cdrom/map/mapbin/" + room + ".bin");
  auto ctd = fs.read("cdrom/map/mapbin/" + room + ".ctd");
  if (!bin || !ctd) return 1;
  std::string err;
  auto scene = import::load_room(room, *bin, *ctd, {}, &err);
  if (!scene) {
    std::fprintf(stderr, "room %s: %s\n", room.c_str(), err.c_str());
    return 1;
  }
  audio::SfxBank bank;
  if (!bank.load(fs)) return 1;
  bank.set_room(scene->yos0_bytes, scene->yos1_bytes);
  // The room's instrument banks.
  {
    const auto& bytes = scene->yos0_bytes;
    auto r32 = [&](size_t offset) { return offset + 4 <= bytes.size() ? u32(bytes[offset]) | u32(bytes[offset + 1]) << 8 | u32(bytes[offset + 2]) << 16 | u32(bytes[offset + 3]) << 24 : 0u; };
    const u32 cnt = bytes.size() >= 16 ? r32(8) : 0;
    for (u32 i = 0; i < cnt && i < 8; ++i) {
      const size_t block_begin = r32(12 + i * 4) + 8, block_end = r32(16 + i * 4) + 8;
      if (block_begin + 0x40 > block_end || block_end > bytes.size()) continue;
      std::printf("%s yos0 block %u: bank id %u spu 0x%X bytes %u first %u count %u\n", room.c_str(), i, unsigned(bytes[block_begin + 4] | bytes[block_begin + 5] << 8),
                  r32(block_begin + 0x10), r32(block_begin + 0x14), r32(block_begin + 0x18), r32(block_begin + 0x1C));
      for (u32 k = 0; k < r32(block_begin + 0x1C) && k < 40; ++k) {
        const size_t inst = block_begin + 0x40 + k * 16;
        std::printf("   inst %3u start %6u loop %6u ft %6d base %3d adsr %04X %04X\n", r32(block_begin + 0x18) + k, r32(inst), r32(inst + 4),
                    int(s16(bytes[inst + 8] | bytes[inst + 9] << 8)), int(s16(bytes[inst + 10] | bytes[inst + 11] << 8)), unsigned(bytes[inst + 12] | bytes[inst + 13] << 8),
                    unsigned(bytes[inst + 14] | bytes[inst + 15] << 8));
      }
    }
  }
  for (int frame = 0; frame < 32; ++frame) {
    audio::SfxProgram program;
    if (!bank.make_room(frame, program)) continue;
    audio::AkaoPlayer pl(44100, true);
    int printed = 0;
    pl.note_trace = [&](int note, const audio::Instrument& in, int reg) {
      if (printed++ < 4)
        std::printf("%s frame %2d note %3d inst base %3d ft %5d start %6u reg 0x%04X (x%.3f)\n", room.c_str(), frame, note, in.base_note,
                    in.fine_tune, in.start, reg, reg / 4096.0);
    };
    pl.start_sfx(program);
    std::vector<float> buf(44100 * 2);
    pl.render(buf.data(), 44100);
  }
  return 0;
}

// sfx-bytes <id>: raw program bytes of a global SFX id (both channels) and its chain count.
int cmd_sfx_bytes(const vfs::Vfs& fs, int id) {
  audio::SfxBank bank;
  if (!bank.load(fs)) return 1;
  audio::SfxProgram program;
  if (!bank.make_global(id, program, nullptr)) return 1;
  for (int i = 0; i < program.count; ++i) {
    std::printf("sfx %d pc %u:", id, program.pc[i] - 0x810);
    for (u32 k = program.pc[i]; k < program.pc[i] + 48 && k < program.code->data.size(); ++k) std::printf(" %02X", program.code->data[k]);
    std::printf("\n");
  }
  return 0;
}

// timing: the live BGM source switch's view of every song: HCA intro / loop vs the AKAO sequence's measured intro / loop (seconds).
int cmd_timing(const vfs::Vfs& fs) {
  auto base = audio::load_akao_base_table(fs);
  for (int song = 0; song < audio::kBgmSongCount; ++song) {
    const audio::BgmEntry* entry = audio::bgm_entry(song);
    if (!entry) continue;
    const audio::SongTiming hca = audio::hca_timing(entry->loop_start, entry->loop_end, 48000);
    auto music = audio::load_akao_music(fs, entry->file_no, base);
    if (!music) {
      std::printf("song %2d BGM%03d: hca intro %7.2f loop %7.2f | no AKAO\n", song, entry->file_no, hca.intro, hca.loop);
      continue;
    }
    audio::AkaoPlayer pl(48000, false);
    pl.load_music(music->seq, music->table);
    const auto t0 = std::chrono::steady_clock::now();
    const audio::SongTiming akao = pl.measure_timing();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("song %2d BGM%03d: hca intro %7.2f loop %7.2f | akao intro %7.2f loop %7.2f len %7.2f | loop ratio %5.2f (%.0f ms)\n", song,
                entry->file_no, hca.intro, hca.loop, akao.intro, akao.loop, akao.length, akao.loop > 0 && hca.loop > 0 ? hca.loop / akao.loop : 0.0, ms);
  }
  return 0;
}

// switch-test <song> <seconds> [volume]: headless Mixer run of the live BGM source switch (remastered -> original -> remastered).
int cmd_switch_test(const vfs::Vfs& fs, int song, double secs, int volume) {
  const audio::BgmEntry* entry = audio::bgm_entry(song);
  auto bytes = entry ? fs.read(audio::bgm_path(entry->file_no)) : std::nullopt;
  auto stream = std::make_shared<audio::HcaStream>();
  if (!bytes || !stream->open(*bytes)) return 1;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  audio::Mixer mixer;
  mixer.set_vfs(&fs);
  mixer.play(stream, song, volume);
  std::vector<float> buf(4800 * 2);
  auto run = [&](double duration) {
    double peak = 0;
    for (u32 done = 0, total = u32(duration * 48000); done < total; done += 4800) {
      mixer.render(buf.data(), 4800);
      for (float sample : buf) peak = std::max(peak, double(std::fabs(sample)));
    }
    return peak;
  };
  std::printf("hca  : peak %.3f, %.2f s, level %.4f\n", run(secs), mixer.bgm_seconds(), double(mixer.bgm_level()));
  for (bool rem : {false, true}) {
    const bool ok = mixer.switch_bgm_source(rem);
    const double peak = run(0.02);
    std::printf("%s switch %d: first 20 ms peak %.3f, then %.2f s, akao %d, song %d level %.4f\n", rem ? "->hca " : "->akao", int(ok), peak,
                mixer.bgm_seconds(), int(mixer.bgm_is_akao()), mixer.current_song(), double(mixer.bgm_level()));
    std::printf("      next 3 s peak %.3f, %.2f s\n", run(3.0), mixer.bgm_seconds());
  }
  return 0;
}

// banks: first index / count / SPU address of every instrument bank file (wave*.dat), music file -> bank.
int cmd_banks(const vfs::Vfs& fs) {
  for (const char* dir : {"cdrom/sound/wave/", "cdrom/sound/exwave/"}) {
    for (int bank_no = 0; bank_no < 1000; ++bank_no) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "%swave%04d.dat", dir, bank_no);
      auto file = fs.read(buf);
      if (!file || file->size() < 0x40) continue;
      const u8* data = file->data();
      auto r32 = [&](size_t offset) { return u32(data[offset]) | u32(data[offset + 1]) << 8 | u32(data[offset + 2]) << 16 | u32(data[offset + 3]) << 24; };
      std::printf("%s: spu 0x%X bytes %u first %u count %u\n", buf, r32(0x10), r32(0x14), r32(0x18), r32(0x1C));
    }
  }
  return 0;
}

// inst-dump <index>: wave0000 instrument table entry + the ADPCM block flags and first decoded samples (X2 analysis).
int cmd_inst_dump(const vfs::Vfs& fs, int index) {
  auto file = fs.read("cdrom/sound/wave/wave0000.dat");
  if (!file) return 1;
  auto bank = audio::parse_bank(*file);
  if (!bank || index < int(bank->first_index) || index >= int(bank->first_index + bank->inst.size())) return 1;
  const auto& in = bank->inst[size_t(index) - bank->first_index];
  std::printf("inst %d: start 0x%X loop 0x%X fine %d base %d adsr %04X %04X\n", index, in.start, in.loop, in.fine_tune, in.base_note, in.adsr1, in.adsr2);
  const u32 count = u32(file->data()[0x1C]) | u32((*file)[0x1D]) << 8;
  const size_t data_off = 0x40 + size_t(count) * 16;
  for (u32 off = in.start; data_off + off + 16 <= file->size() && off < in.start + 16 * 40; off += 16) {
    const u8* block = &(*file)[data_off + off];
    std::printf("  block @%X shift/filter %02X flags %02X\n", off, block[0], block[1]);
    if (block[1] & 1) break;
  }
  if (in.sample) {
    std::printf("  decoded %zu samples, loop_start %d loops %d:", in.sample->pcm.size(), in.sample->loop_start, in.sample->loops ? 1 : 0);
    for (size_t i = 0; i < in.sample->pcm.size() && i < 84; ++i) std::printf(" %d", in.sample->pcm[i]);
    std::printf("\n");
  }
  return 0;
}

// sfx-ops: per global SFX id the sequencer opcodes the engine ignores (noise C4/CE/CF/AC, FM C6/D2/D3, random pitch E1, sidechains D4..D7).
int cmd_sfx_ops(const vfs::Vfs& fs) {
  audio::SfxBank bank;
  if (!bank.load(fs)) return 1;
  for (int id = 0; id < 256; ++id) {
    audio::SfxProgram program;
    if (!bank.make_global(id, program, nullptr)) continue;
    std::map<int, int> seen;
    for (int i = 0; i < program.count; ++i) {
      const auto& code = program.code->data;
      size_t pc = program.pc[i];
      for (int guard = 0; guard < 600 && pc < code.size(); ++guard) {
        const int length = audio::akao_op_length(&code[pc], code.size() - pc);
        if (length == 0) break;
        const int op = code[pc];
        if (op == 0xA0 || op == 0xFF) break;
        if (op == 0xC4 || op == 0xCE || op == 0xCF || op == 0xAC || op == 0xC6 || op == 0xD2 || op == 0xD3 || op == 0xE1 || (op >= 0xD4 && op <= 0xD7))
          ++seen[op];
        pc += size_t(length);
      }
    }
    if (seen.empty()) continue;
    std::printf("sfx %3d:", id);
    for (auto& [op, count] : seen) std::printf(" %02X x%d", op, count);
    std::printf("\n");
  }
  return 0;
}

int cmd_play_sfx(const vfs::Vfs& fs, int id, double seconds) {
  audio::SfxBank bank;
  if (!bank.load(fs)) {
    std::fprintf(stderr, "cannot load effect00.dat / wave0000.dat\n");
    return 1;
  }
  audio::Mixer mixer;
  if (!mixer.open_device()) return 1;
  audio::AkaoSfx sfx(&mixer, &bank);
  sfx.play(id);
  std::printf("playing sfx %d for %.1f s\n", id, seconds);
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  mixer.close_device();
  return 0;
}

}  // namespace

int ocaudio_main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string game_dir = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
  if (args.size() >= 2 && args[0] == "-g") {
    game_dir = args[1];
    args.erase(args.begin(), args.begin() + 2);
  }
  if (args.empty() || game_dir.empty()) return usage();
  vfs::Vfs fs;
  if (!fs.open(game_dir)) return 1;
  if (args[0] == "decode" && args.size() >= 3)
    return cmd_decode(fs, args[1], args[2], args.size() > 3 ? std::atof(args[3].c_str()) : 0.0);
  if (args[0] == "play" && args.size() >= 2) return cmd_play(fs, args[1], args.size() > 2 ? std::atof(args[2].c_str()) : 10.0);
  if (args[0] == "render-seq" && args.size() >= 3)
    return cmd_render_seq(fs, args[1], args[2], args.size() > 3 ? std::atof(args[3].c_str()) : 20.0,
                          args.size() > 4 ? u32(std::atoi(args[4].c_str())) : 44100u, args.size() > 5 ? args[5] : std::string());
  if (args[0] == "play-seq" && args.size() >= 2)
    return cmd_play_seq(fs, args[1], args.size() > 2 ? std::atof(args[2].c_str()) : 20.0);
  if (args[0] == "render-sfx" && args.size() >= 3)
    return cmd_render_sfx(fs, args[1], args[2], args.size() > 3 ? std::atof(args[3].c_str()) : 3.0,
                          args.size() > 4 ? u32(std::atoi(args[4].c_str())) : 44100u,
                          args.size() > 5 ? args[5] : std::string());
  if (args[0] == "sfx-pitch") return cmd_sfx_pitch(fs);
  if (args[0] == "room-sfx" && args.size() >= 2) return cmd_room_sfx(fs, args[1]);
  if (args[0] == "sfx-bytes" && args.size() >= 2) return cmd_sfx_bytes(fs, std::atoi(args[1].c_str()));
  if (args[0] == "banks") return cmd_banks(fs);
  if (args[0] == "timing") return cmd_timing(fs);
  if (args[0] == "switch-test" && args.size() >= 3)
    return cmd_switch_test(fs, std::atoi(args[1].c_str()), std::atof(args[2].c_str()), args.size() > 3 ? std::atoi(args[3].c_str()) : 100);
  if (args[0] == "inst-dump" && args.size() >= 2) return cmd_inst_dump(fs, std::atoi(args[1].c_str()));
  if (args[0] == "sfx-ops") return cmd_sfx_ops(fs);
  if (args[0] == "play-sfx" && args.size() >= 2)
    return cmd_play_sfx(fs, std::atoi(args[1].c_str()), args.size() > 2 ? std::atof(args[2].c_str()) : 3.0);
  return usage();
}
