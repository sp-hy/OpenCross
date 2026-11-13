#include "game/scan_service.h"

#include <chrono>

#include "core/log.h"
#include "data/tables.h"
#include "vfs/vfs.h"

namespace oc::game {

ScanService::~ScanService() {
  if (job_.valid()) job_.wait();
}

void ScanService::start(const vfs::Vfs& files) {
  if (started_) return;
  started_ = true;
  job_ = std::async(std::launch::async, [&files] {
    Result out;
    vfs::Vfs plain;
    const vfs::Vfs& source = !files.game_dir().empty() && plain.open(files.game_dir()) ? plain : files;
    out.scan = scan_rooms(source);
    data::GameTables tables;
    if (std::string error; tables.load(source, &error, false)) out.bestiary = bestiary_order(out.scan, tables, source);
    else log::warn("scan: battle tables: {}", error);
    return out;
  });
}

const ScriptScan* ScanService::ready() {
  if (result_) return &result_->scan;
  if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
  result_ = job_.get();
  log::info("scan: {} rooms scanned in {:.0f} ms, {} enemy species numbered", result_->scan.rooms.size(), result_->scan.milliseconds,
            result_->bestiary.species.size());
  return &result_->scan;
}

const ScriptScan* ScanService::wait() {
  if (!result_ && job_.valid()) job_.wait();
  return ready();
}

const data::BestiaryOrder* ScanService::bestiary() { return ready() ? &result_->bestiary : nullptr; }

}  // namespace oc::game
