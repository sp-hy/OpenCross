#include "audio/voice.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>

#include "audio/mixer.h"
#include "core/log.h"

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#endif

namespace oc::audio {

namespace {
// CrossFix's character table (avatar id 0..82), the same names in the same order so existing packs keep working.
const char* const kCharacterNames[] = {
    "serge", "kid", "guile", "norris", "nikki", "viper", "riddel", "karsh", "zoa", "marcy", "korcha", "luccia", "poshul",
    "razly", "zappa", "orcha", "radius", "fargo", "macha", "glenn", "leena", "miki", "harle", "janice", "draggy", "starky",
    "sprigg", "mojo", "turnip", "neofio", "greco", "skelly", "funguy", "irenes", "mel", "leah", "van", "sneff", "steena",
    "doc", "grobyc", "pierre", "orlha", "pip", "pip", "pip", "pip", "pip", "pip", "lynx", "glenn", "lynx", "kid child",
    "balthasar", "dwarf", "dario", "dark-serge", "dark-serge", "green-dragon", "sky-dragon", "black-dragon", "water-dragon",
    "fire-dragon", "earth-dragon", "lynx", "dragon god", "dario", "glenn", "karsh", "wazuki", "miguel", "garai", "marge",
    "tia", "rosetta", "una", "lisa", "sage", "direa", "zippa", "gogh", "gogh", "orcha",
};
constexpr int kCharacterNameCount = int(sizeof(kCharacterNames) / sizeof(kCharacterNames[0]));

VoicePlayer* g_instance = nullptr;
}  // namespace

std::string field_voice_rel(int scene, int dialog, int page, std::string_view name) {
  return std::to_string(scene) + "/" + std::to_string(dialog) + "-" + std::to_string(page) + "-" + std::string(name) + ".mp3";
}

// An empty unit_name gives the speaker-less form `<msg>-<page>.mp3`: the battle script's issuing entity is not always the speaker,
// so a pack may name a line without a unit (the unit form is tried first).
std::string battle_voice_rel(int battle, int msg, int page, std::string_view unit_name) {
  return "battle/" + std::to_string(battle) + "/" + std::to_string(msg) + "-" + std::to_string(page) + (unit_name.empty() ? "" : "-" + std::string(unit_name)) + ".mp3";
}

const char* voice_char_name(int id) { return id >= 0 && id < kCharacterNameCount ? kCharacterNames[id] : nullptr; }

// CrossFix's rule: speaker register 0 (Serge or nobody) = "npc", otherwise the avatar table entry (or the number past the table);
// CrossFix's own example names Serge for id 0; then the speaker named by the line's {NAME%} tag; then, since CrossFix names a
// window after its avatar global, which keeps the last speaker when a later window has none, the previous speaker.
std::vector<std::string> field_voice_names(int speaker, int tag_speaker, int prev_speaker) {
  std::vector<std::string> out;
  auto add = [&](std::string name) {
    if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
  };
  if (speaker <= 0) add("npc");
  else if (const char* name = voice_char_name(speaker)) add(name);
  else add(std::to_string(speaker));
  if (speaker <= 0) add("serge");
  if (tag_speaker > 0)
    if (const char* name = voice_char_name(tag_speaker)) add(name);
  if (speaker <= 0 && prev_speaker > 0)
    if (const char* name = voice_char_name(prev_speaker)) add(name);
  return out;
}

std::vector<int> field_voice_scenes(int room) {
  if (room == 501) return {1501, 501};
  return {room};
}

std::vector<std::string> FieldVoiceNames::rels(int room, int dialog, int page, int speaker, int tag_speaker) {
  if (room != room_) {
    room_ = room;
    prev_ = 0;
  }
  const std::vector<std::string> names = field_voice_names(speaker, tag_speaker, prev_);
  if (speaker > 0) prev_ = speaker;
  std::vector<std::string> out;
  for (int scene : field_voice_scenes(room))
    for (const std::string& name : names) out.push_back(field_voice_rel(scene, dialog, page, name));
  return out;
}

std::string voice_unit_name(std::string_view unit) {
  std::string name;
  for (char ch : unit) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte <= ' ' || std::strchr("\\/:*?\"<>|", ch)) continue;
    name += byte < 0x80 ? char(std::tolower(byte)) : ch;
  }
  return name;
}

// ---------------------------------------------------------------------------------------------------------------------------
// MP3 decode through Windows Media Foundation (the project's librempeg build has no MP3 decoder): the first audio stream is read
// as 32-bit float at its native channel count and rate, folded to stereo, then linearly resampled to the mixer rate if needed.

#ifdef _WIN32
namespace {
using Microsoft::WRL::ComPtr;

// `open` makes the source reader once Media Foundation is up.
bool decode_mp3(const std::function<HRESULT(ComPtr<IMFSourceReader>&)>& open, std::vector<float>& out, const std::function<bool()>& cancel) {
  out.clear();
  const HRESULT co_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Guard {
    bool com;
    bool mf = false;
    ~Guard() {
      if (mf) MFShutdown();
      if (com) CoUninitialize();
    }
  } guard{SUCCEEDED(co_init)};
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
  guard.mf = true;

  ComPtr<IMFSourceReader> reader;
  if (FAILED(open(reader))) return false;
  constexpr DWORD kStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(kStream, TRUE);
  ComPtr<IMFMediaType> native;
  UINT32 channels = 2, rate = 44100;
  if (SUCCEEDED(reader->GetNativeMediaType(kStream, 0, &native))) {
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  }
  channels = std::clamp<UINT32>(channels, 1, 8);
  if (rate == 0) rate = 44100;
  ComPtr<IMFMediaType> want;
  if (FAILED(MFCreateMediaType(&want))) return false;
  want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
  want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
  want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
  want->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 4);
  want->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 4);
  if (FAILED(reader->SetCurrentMediaType(kStream, nullptr, want.Get()))) return false;
  ComPtr<IMFMediaType> cur;
  if (SUCCEEDED(reader->GetCurrentMediaType(kStream, &cur))) {
    cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  }
  if (channels == 0 || rate == 0) return false;

  std::vector<float> src;
  for (;;) {
    if (cancel && cancel()) return false;
    DWORD flags = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(reader->ReadSample(kStream, 0, nullptr, &flags, nullptr, &sample))) break;
    if (sample) {
      ComPtr<IMFMediaBuffer> buf;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
          const size_t frames = len / (sizeof(float) * channels);
          const float* samples = reinterpret_cast<const float*>(data);
          const size_t at = src.size();
          src.resize(at + frames * 2);
          for (size_t i = 0; i < frames; ++i) {
            src[at + i * 2] = samples[i * channels];
            src[at + i * 2 + 1] = samples[i * channels + (channels > 1 ? 1 : 0)];
          }
          buf->Unlock();
        }
      }
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
  }
  const size_t src_frames = src.size() / 2;
  if (src_frames == 0) return false;
  if (rate == Mixer::kRate) {
    out = std::move(src);
  } else {
    const double ratio = double(rate) / double(Mixer::kRate);
    const size_t out_frames = size_t(std::floor(double(src_frames - 1) / ratio)) + 1;
    out.resize(out_frames * 2);
    for (size_t i = 0; i < out_frames; ++i) {
      const double src_pos = double(i) * ratio;
      const size_t i0 = size_t(src_pos);
      const size_t i1 = std::min(i0 + 1, src_frames - 1);
      const float frac = float(src_pos - double(i0));
      for (size_t side = 0; side < 2; ++side) out[i * 2 + side] = src[i0 * 2 + side] + (src[i1 * 2 + side] - src[i0 * 2 + side]) * frac;
    }
  }
  return true;
}
}  // namespace

bool decode_mp3_file(const std::filesystem::path& file, std::vector<float>& out, const std::function<bool()>& cancel) {
  return decode_mp3([&](ComPtr<IMFSourceReader>& reader) { return MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader); }, out, cancel);
}

bool decode_mp3_bytes(const Bytes& mp3, std::vector<float>& out, const std::function<bool()>& cancel) {
  const auto open = [&](ComPtr<IMFSourceReader>& reader) -> HRESULT {
    ComPtr<IStream> memory;
    memory.Attach(SHCreateMemStream(mp3.data(), static_cast<UINT>(mp3.size())));
    if (!memory) return E_OUTOFMEMORY;
    ComPtr<IMFByteStream> stream;
    if (const HRESULT status = MFCreateMFByteStreamOnStream(memory.Get(), &stream); FAILED(status)) return status;
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(stream.As(&attributes))) attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"audio/mpeg");
    return MFCreateSourceReaderFromByteStream(stream.Get(), nullptr, &reader);
  };
  return decode_mp3(open, out, cancel);
}
#else
bool decode_mp3_file(const std::filesystem::path&, std::vector<float>&, const std::function<bool()>&) { return false; }
bool decode_mp3_bytes(const Bytes&, std::vector<float>&, const std::function<bool()>&) { return false; }
#endif

// ---------------------------------------------------------------------------------------------------------------------------

VoicePlayer* VoicePlayer::instance() { return g_instance; }
void VoicePlayer::set_instance(VoicePlayer* player) { g_instance = player; }

VoicePlayer::VoicePlayer(Mixer& mixer) : mixer_(mixer) { thread_ = std::thread([this] { worker(); }); }

VoicePlayer::~VoicePlayer() {
  if (g_instance == this) g_instance = nullptr;
  {
    std::lock_guard lk(mu_);
    quit_ = true;
    gen_.fetch_add(1);
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void VoicePlayer::set_enabled(bool on) {
  enabled_.store(on, std::memory_order_relaxed);
  if (!on) stop();
}

std::filesystem::path VoicePlayer::find(std::string_view rel) const {
  if (!roots_) return {};
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path rel_path = fs::path(std::u8string(rel.begin(), rel.end()));
  for (const fs::path& root : roots_()) {
    fs::path candidate = root / "voices" / rel_path;
    if (fs::is_regular_file(candidate, ec)) return candidate;
    if (packed_ && packed_(candidate)) return candidate;
    std::string leaf = root.filename().string();
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (leaf == "voices") {
      candidate = root / rel_path;
      if (fs::is_regular_file(candidate, ec)) return candidate;
    }
  }
  return {};
}

void VoicePlayer::play(const std::filesystem::path& file, const void* owner) {
  if (!enabled()) return;
  PackedLine packed = packed_ ? packed_(file) : PackedLine();
  {
    std::lock_guard lk(mu_);
    job_ = file;
    job_packed_ = std::move(packed);
    job_gen_ = gen_.fetch_add(1) + 1;
    owner_ = owner;
    busy_.store(!file.empty());
  }
  mixer_.voice_stop();
  cv_.notify_all();
}

void VoicePlayer::stop() {
  {
    std::lock_guard lk(mu_);
    job_.clear();
    job_gen_ = gen_.fetch_add(1) + 1;
    owner_ = nullptr;
    busy_.store(false);
  }
  mixer_.voice_stop();
}

void VoicePlayer::stop_if(const void* owner) {
  {
    std::lock_guard lk(mu_);
    if (owner_ != owner) return;
  }
  stop();
}

bool VoicePlayer::active() const { return busy_.load() || mixer_.voice_active(); }

void VoicePlayer::worker() {
  for (;;) {
    std::filesystem::path file;
    PackedLine packed;
    unsigned long long gen = 0;
    {
      std::unique_lock lk(mu_);
      cv_.wait(lk, [&] { return quit_ || (!job_.empty() && job_gen_ == gen_.load()); });
      if (quit_) return;
      file = std::move(job_);
      packed = std::move(job_packed_);
      job_.clear();
      gen = job_gen_;
    }
    auto pcm = std::make_shared<std::vector<float>>();
    const auto cancelled = [&] { return gen_.load() != gen; };
    bool ok = false;
    if (packed) {
      const std::optional<Bytes> bytes = packed();
      ok = bytes && decode_mp3_bytes(*bytes, *pcm, cancelled);
    } else {
      ok = decode_mp3_file(file, *pcm, cancelled);
    }
    if (gen_.load() == gen) {
      if (ok) {
        log::debug("voice: playing {} ({:.2f} s)", file.string(), double(pcm->size() / 2) / Mixer::kRate);
        mixer_.voice_play(pcm);
      } else {
        log::warn("voice: cannot decode {} (Media Foundation MP3 decoder missing or file damaged)", file.string());
      }
      busy_.store(false);
    }
  }
}

// A new line always ends the previous one, even when its file is not found.
void VoicePlayer::request(const std::string& rel, const std::vector<std::string>& alt, const void* owner) {
  if (!enabled()) return;
  std::filesystem::path file = find(rel);
  for (size_t i = 0; file.empty() && i < alt.size(); ++i) file = find(alt[i]);
  if (file.empty()) {
    stop();
    std::lock_guard lk(log_mu_);
    if (roots_ && logged_.insert(rel).second) log::debug("voice: not found voices/{}", rel);
    return;
  }
  play(file, owner);
}

void VoicePlayer::field_page(const void* owner, int scene, int dialog, int speaker, int tag_speaker, int page) {
  if (!enabled()) return;
  std::vector<std::string> rels = field_names_.rels(scene, dialog, page, speaker, tag_speaker);
  const std::string first = rels.front();
  rels.erase(rels.begin());
  request(first, rels, owner);
}

void VoicePlayer::field_close(const void* owner) { stop_if(owner); }

void VoicePlayer::battle_open(int battle, int msg, std::string_view unit_name) {
  b_battle_ = battle;
  b_msg_ = msg;
  b_unit_ = voice_unit_name(unit_name);
  b_page_ = -1;
}

void VoicePlayer::battle_sync(int page, bool closing) {
  if (b_battle_ < 0) return;
  if (closing) {
    if (b_page_ != -2) stop_if(this);
    b_page_ = -2;
    return;
  }
  if (page == b_page_) return;
  b_page_ = page;
  request(battle_voice_rel(b_battle_, b_msg_, page, b_unit_), {battle_voice_rel(b_battle_, b_msg_, page, "")}, this);
}

}  // namespace oc::audio
