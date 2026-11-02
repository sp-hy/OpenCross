#pragma once
// The OpenCross save page: an auto-save (number 0) and 100 numbered files (1..100), one file each in a folder next to save.dat
// (RDE never reads them). A file: "OCSV", u32 version 1, u32 language, u32 battle counter, 44 x 32 B UTF-8 custom names (NUL
// padded, empty = none), then the 0x2000 Chrono Cross slot block exactly as a memory card holds it.
#include <filesystem>
#include <map>

#include "core/types.h"
#include "save/cc_save.h"
#include "save/ext_save.h"

namespace oc::save {

inline constexpr int kOcFiles = 100;

class OcSaves {
 public:
  struct File {
    SaveSlot data;
    ExtSlot ext;
    // Last write time (newest file = the default cursor).
    std::filesystem::file_time_type written{};
  };

  void set_dir(std::filesystem::path dir) { dir_ = std::move(dir); }
  const std::filesystem::path& dir() const { return dir_; }
  // Reads every file of the folder (a missing folder = an empty page; unreadable files are skipped with a warning).
  void load();
  bool empty() const { return files_.empty(); }
  // number 0 = the auto-save, 1..100 = the files; null when absent.
  const File* file(int number) const;
  // The newest manual file (0 when there is none).
  int newest() const;

  // The host's language, battle counter and names are stored with every write; a load hands the battle counter back.
  void set_ext_host(ExtHost* host) { ext_host_ = host; }
  // Writes a temp file, then renames; creates the folder. Throws on I/O errors.
  void write(int number, const SaveSlot& state);
  // The names only when the file's language == `language`; the battle counter replaces the host's.
  ExtSlot read_ext(int number, u32 language) const;
  // read_ext without taking the battle counter: a save list looking at a file.
  ExtSlot peek_ext(int number, u32 language) const;

  static std::filesystem::path file_name(int number);
  static Bytes serialize(const SaveSlot& state, const ExtSlot& ext);
  // Throws on a wrong size / magic / version.
  static File parse(ByteSpan bytes);

 private:
  std::filesystem::path dir_;
  std::map<int, File> files_;
  ExtHost* ext_host_ = nullptr;
};

}  // namespace oc::save
