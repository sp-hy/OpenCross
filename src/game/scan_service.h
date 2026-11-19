#pragma once
// The room-script scan (room_scan.h) run once in the background for the record pages: start() launches it on a worker thread the first
// time it is called; ready() answers nothing until it has finished, then the kept result for the rest of the run. The scan reads the
// unmodified game, so what the pages derive from it (the Bestiary page's numbers) is the same for every player whatever mods are on.
#include <future>
#include <optional>

#include "data/bestiary.h"
#include "game/room_scan.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

class ScanService {
 public:
  ScanService() = default;
  ScanService(const ScanService&) = delete;
  ScanService& operator=(const ScanService&) = delete;
  ~ScanService();

  // `files` must outlive the service. The worker reads the game through a VFS of its own on the same game folder (no mods; the PS1
  // file tree, as in original art), or through `files` when that folder is no RDE install (the game played from its discs).
  void start(const vfs::Vfs& files);
  bool started() const { return started_; }
  // The finished scan, or null while it runs or before start().
  const ScriptScan* ready();
  // Blocks until the started scan has finished (the randomizer needs it before the first room loads); null before start().
  const ScriptScan* wait();
  // The Bestiary page's numbers and places (data::bestiary_order over the scan's battle sites and the unmodified game's battle tables),
  // or null until ready(); empty when the tables could not be read.
  const data::BestiaryOrder* bestiary();

 private:
  struct Result {
    ScriptScan scan;
    data::BestiaryOrder bestiary;
  };
  bool started_ = false;
  std::future<Result> job_;
  std::optional<Result> result_;
};

}  // namespace oc::game
