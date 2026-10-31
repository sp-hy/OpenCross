#pragma once
// Voice lines: CrossFix-compatible field dialogue voices (`voices/<scene>/<dialog>-<page>-<character>.mp3`) plus our own battle
// layout (`voices/battle/<battle>/<msg>-<page>-<unit>.mp3`). An MP3 is decoded whole by a worker thread (Windows Media Foundation)
// to 48 kHz stereo float and handed to the mixer's voice bus.
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/types.h"

namespace oc::audio {

class Mixer;

// Relative path (below a mod's `voices` folder, '/' separators) of a field dialogue line: CrossFix's `<scene>/<dialog>-<page>-<name>.mp3`.
std::string field_voice_rel(int scene, int dialog, int page, std::string_view name);
// Our battle layout: `battle/<battle id>/<msg index>-<page>-<unit name>.mp3` (battle id = scene id, not the btel remap).
std::string battle_voice_rel(int battle, int msg, int page, std::string_view unit_name);
// CrossFix's character table (avatar id 0..82 -> lowercase name); nullptr past the end.
const char* voice_char_name(int id);
// Candidate character names of a field line in lookup order (CrossFix's literal rule first, then "serge" for a speaker-less line, then
// the name tag, then - only for a line without a speaker - the previous speaker `prev_speaker` of the room, which CrossFix's avatar
// global still holds).
std::vector<std::string> field_voice_names(int speaker, int tag_speaker, int prev_speaker = 0);
// Scene folder numbers to try for a lang.dat room id, in order: the room id itself, except fin_19 (id 501 by name) whose ending use is
// the +1000 id 1501 (the `fin_` rooms are 1000 + map id in RDE's text sets), tried first.
std::vector<int> field_voice_scenes(int room);
// Per-line lookup state of the field voices: the previous speaker of the current room (reset when the room id changes).
class FieldVoiceNames {
 public:
  // Relative paths to try, best first (scenes x names). Updates the previous speaker (a line with speaker > 0 becomes it).
  std::vector<std::string> rels(int room, int dialog, int page, int speaker, int tag_speaker);
 private:
  int room_ = -1;
  int prev_ = 0;
};
// A unit name as a file-name part: lowercase, spaces and path-illegal characters removed.
std::string voice_unit_name(std::string_view unit);

// Decodes an MP3 file to 48 kHz interleaved stereo float. `cancel` (optional) is polled; false = failed, cancelled or no MP3 decoder.
bool decode_mp3_file(const std::filesystem::path& file, std::vector<float>& out, const std::function<bool()>& cancel = {});
// The same from the bytes of an MP3.
bool decode_mp3_bytes(const Bytes& mp3, std::vector<float>& out, const std::function<bool()>& cancel = {});

class VoicePlayer {
 public:
  explicit VoicePlayer(Mixer& mixer);
  ~VoicePlayer();
  VoicePlayer(const VoicePlayer&) = delete;
  VoicePlayer& operator=(const VoicePlayer&) = delete;

  // The process-wide player (set by main; null in tools / tests).
  static VoicePlayer* instance();
  static void set_instance(VoicePlayer* player);

  // [Audio] Voices; off stops the running line and ignores requests.
  void set_enabled(bool on);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  // Where lines are searched: `roots` returns the enabled mod folders in priority order (called per lookup).
  void set_search(std::function<std::vector<std::filesystem::path>()> roots) { roots_ = std::move(roots); }
  // Reads the bytes of one line; runs on the worker.
  using PackedLine = std::function<std::optional<Bytes>()>;
  // A root may be a packed mod: `lookup` gives the reader of the line at <root>/voices/<rel>, empty when it has none. Main thread.
  void set_packed_lookup(std::function<PackedLine(const std::filesystem::path&)> lookup) { packed_ = std::move(lookup); }

  // First existing `<root>/voices/<rel>` over the roots (a root folder called `voices` also serves `<root>/<rel>`: CrossFix's
  // `mods/voices/...` layout). Empty when none.
  std::filesystem::path find(std::string_view rel) const;

  // Plays the file (replacing the running line). `owner` tags the line for stop_if.
  void play(const std::filesystem::path& file, const void* owner = nullptr);
  void stop();
  void stop_if(const void* owner);
  // Decoding or playing.
  bool active() const;

  // Field dialogue (ui::DialogueDemo events): a page is shown / the window closed.
  void field_page(const void* owner, int scene, int dialog, int speaker, int tag_speaker, int page);
  void field_close(const void* owner);
  // Battle script message (BattleView::smsg_*): the message opens, then per tick the current page and whether the window closes.
  void battle_open(int battle, int msg, std::string_view unit_name);
  void battle_sync(int page, bool closing);

 private:
  void worker();
  void request(const std::string& rel, const std::vector<std::string>& alt, const void* owner);

  Mixer& mixer_;
  std::atomic<bool> enabled_{true};
  std::function<std::vector<std::filesystem::path>()> roots_;
  std::function<PackedLine(const std::filesystem::path&)> packed_;

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool quit_ = false;
  std::filesystem::path job_;
  PackedLine job_packed_;
  unsigned long long job_gen_ = 0;
  std::atomic<unsigned long long> gen_{0};
  std::atomic<bool> busy_{false};
  const void* owner_ = nullptr;

  mutable std::mutex log_mu_;
  std::set<std::string> logged_;

  FieldVoiceNames field_names_;  // main thread only

  // Battle line in progress (main thread only).
  int b_battle_ = -1, b_msg_ = -1, b_page_ = -1;
  std::string b_unit_;
};

}  // namespace oc::audio
