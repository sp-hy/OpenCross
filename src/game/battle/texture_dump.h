#pragma once
// "Dump all textures": loads every texture that is not a whole file in the archives headlessly (battle stages, the title, battle effect packs,
// every room's effect sheets / mesh effects / room models, every battle and field model, every weapon pack part) and writes it through render::texture_registry::dump_forced.
// No GPU: it only needs decoded images, so it runs on a worker thread
// (TextureDumpJob, started by the Mods tab button) or blocking from `occ dump-textures`. It reads the shipped archives through its own Vfs (no mods).
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/types.h"

namespace oc::game {

// kDumpRaw (debug): also the raw (mode, CLUT) atlases of stages / title / room meshes under raw/. kDumpModelSource: the models are written
// at their 1x size (the art RDE's 4x images were upscaled from) instead of as RDE's 4x images, under the same names.
enum DumpSet : unsigned {
  kDumpStages = 1, kDumpTitle = 2, kDumpEffects = 4, kDumpRooms = 8, kDumpModels = 16, kDumpRaw = 32, kDumpWeapons = 64, kDumpModelSource = 128,
  kDumpAll = kDumpStages | kDumpTitle | kDumpEffects | kDumpRooms | kDumpModels | kDumpWeapons
};

struct TextureDumpProgress {
  // done / total: items of the current phase (stages / packs / rooms / models / weapon packs).
  std::atomic<u32> done{0}, total{0};
  // PNG files written so far (all phases).
  std::atomic<u32> files{0};
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> seconds{0};

  std::string phase() const;
  // Per phase, the number of files written in it.
  std::vector<std::pair<std::string, u32>> report() const;
  // One line, available after finished.
  std::string summary() const;

  void set_phase(std::string name);
  void end_phase(u32 files_in_phase);

 private:
  mutable std::mutex mutex_;
  std::string phase_, summary_;
  std::vector<std::pair<std::string, u32>> report_;
  friend int run_texture_dump(const std::filesystem::path&, unsigned, TextureDumpProgress&);
};

// Blocking. Returns 0 when every phase finished (items that fail to load are skipped and counted in the summary).
int run_texture_dump(const std::filesystem::path& game_dir, unsigned what, TextureDumpProgress& progress);

class TextureDumpJob {
 public:
  ~TextureDumpJob() {
    // Quitting the game mid-dump: stop at the next item.
    request_cancel();
    join();
  }
  // False when already running.
  bool start(const std::filesystem::path& game_dir, unsigned what = kDumpAll);
  void request_cancel() { progress_.cancel = true; }
  bool running() const { return progress_.running; }
  TextureDumpProgress& progress() { return progress_; }
  void join();

 private:
  TextureDumpProgress progress_;
  std::thread thread_;
};

}  // namespace oc::game
