#pragma once
// The dialogue log file ([Game] OpenCrossMenu): the finished message windows, newest last, kept across launches in one UTF-8 text file of
// the OpenCross save folder (never in the game's saves). One entry per line: a kind letter and tab-separated fields, a backslash
// escaping tab, newline and itself. New entries are appended; the file is rewritten with the newest kMaxEntries once it holds twice
// as many, and a load keeps only the newest kMaxEntries.
//
//   DialogueLog log;  log.set_path(dir / DialogueLog::kFileName);  log.load();
//   log.append(entry);   (written at once while set_writable(true))
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace oc::save {

struct DialogueEntry {
  enum class Kind : u8 { Line, Divider };
  Kind kind = Kind::Line;
  // Line: the portrait shown (an RDE face id, -1 none), the speaker's name ("" unknown), the text as the window showed it (lines and
  // pages separated by '\n'), the option picked when the message was a choice, and the voice files of its pages that played (paths
  // below a mod's voices folder, in page order).
  int face = -1;
  std::string speaker;
  std::string text;
  std::string choice;
  std::vector<std::string> voices;
  // Divider (a game loaded): the area's name and the play time in seconds.
  std::string area;
  u32 play_seconds = 0;

  bool operator==(const DialogueEntry&) const = default;
};

class DialogueLog {
 public:
  static constexpr const char* kFileName = "dialogue_log.txt";
  static constexpr const char* kHeader = "OpenCross dialogue log 1";
  static constexpr size_t kMaxEntries = 1000;

  void set_path(std::filesystem::path path) { path_ = std::move(path); }
  const std::filesystem::path& path() const { return path_; }
  // Off: append() keeps the entry in memory only (a --config run never writes the file).
  void set_writable(bool on) { writable_ = on; }
  // Reads the file (missing = empty). A file without the header line is renamed "<name>.bad" with a warning; an unreadable line is
  // skipped. False when the file was bad.
  bool load();
  // Adds an entry (the oldest goes past kMaxEntries) and, while writable, appends it to the file.
  void append(DialogueEntry entry);
  // Forgets every entry and, while writable, empties the file.
  void clear();
  const std::vector<DialogueEntry>& entries() const { return entries_; }
  // Incremented by every append / clear / load (a view rebuilds its layout when it changes).
  u32 generation() const { return generation_; }

  // One entry as a file line (without the newline), and back; nullopt for a line of another kind or with a bad field.
  static std::string format_entry(const DialogueEntry& entry);
  static std::optional<DialogueEntry> parse_entry(std::string_view line);
  // The whole file's text, and the newest kMaxEntries entries of one (nullopt without the header line).
  static std::string format_file(const std::vector<DialogueEntry>& entries);
  static std::optional<std::vector<DialogueEntry>> parse_file(std::string_view text);

 private:
  bool rewrite() const;
  std::filesystem::path path_;
  bool writable_ = false;
  std::vector<DialogueEntry> entries_;
  // Entry lines the file holds (it is compacted once this reaches 2 * kMaxEntries).
  size_t file_lines_ = 0;
  u32 generation_ = 0;
};

}  // namespace oc::save
