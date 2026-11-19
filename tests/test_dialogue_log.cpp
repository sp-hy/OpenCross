// The dialogue log file: an entry line round trip with every escaped character, the kept 1,000 newest entries on a load, the appended
// file compacted at twice that, a damaged file set aside, and a log that is not writable leaving the disk alone (a temp folder; no real
// save folder is touched).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "save/dialogue_log.h"

using namespace oc;
using save::DialogueEntry;
using save::DialogueLog;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static DialogueEntry line_entry(int number) {
  DialogueEntry entry;
  entry.face = number % 92;
  entry.speaker = "Kid";
  entry.text = "Kid:\nLine " + std::to_string(number);
  return entry;
}

static std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static size_t line_count(const std::string& text) {
  size_t lines = 0;
  for (const char ch : text) lines += ch == '\n';
  return lines;
}

static void test_entry_format() {
  DialogueEntry entry;
  entry.face = 1;
  entry.speaker = "Kid";
  entry.text = "Kid:\n\"Tab\there, back\\slash\"\r\nend";
  entry.choice = "   Yes";
  entry.voices = {"22/1-0-kid.mp3", "22/1-1-kid.mp3"};
  const std::string line = DialogueLog::format_entry(entry);
  CHECK(line == "L\t1\tKid\tKid:\\n\"Tab\\there, back\\\\slash\"\\r\\nend\t   Yes\t22/1-0-kid.mp3\t22/1-1-kid.mp3");
  CHECK(line.find('\n') == std::string::npos);
  CHECK(DialogueLog::parse_entry(line) == entry);

  DialogueEntry divider;
  divider.kind = DialogueEntry::Kind::Divider;
  divider.area = "Termina";
  divider.play_seconds = 45600;
  CHECK(DialogueLog::format_entry(divider) == "D\t45600\tTermina");
  CHECK(DialogueLog::parse_entry("D\t45600\tTermina\r") == divider);

  DialogueEntry plain;
  plain.text = "No portrait.";
  CHECK(DialogueLog::format_entry(plain) == "L\t-1\t\tNo portrait.\t");
  CHECK(DialogueLog::parse_entry(DialogueLog::format_entry(plain)) == plain);

  CHECK(!DialogueLog::parse_entry(""));
  CHECK(!DialogueLog::parse_entry("X\t1\t2"));
  CHECK(!DialogueLog::parse_entry("L\tface\tKid\ttext\t"));
  CHECK(!DialogueLog::parse_entry("L\t1\tKid\ttext"));
  CHECK(!DialogueLog::parse_entry("D\tlong\tTermina"));
}

static void test_file_text() {
  std::vector<DialogueEntry> entries;
  for (int i = 0; i < 1500; ++i) entries.push_back(line_entry(i));
  const std::string text = DialogueLog::format_file(entries);
  CHECK(text.rfind(std::string(DialogueLog::kHeader) + "\n", 0) == 0);
  const auto back = DialogueLog::parse_file(text);
  CHECK(back && back->size() == DialogueLog::kMaxEntries);
  CHECK(back && back->front() == line_entry(500) && back->back() == line_entry(1499));
  CHECK(!DialogueLog::parse_file("not a log\nL\t1\tKid\ttext\t\n"));
  const auto skipped = DialogueLog::parse_file(std::string(DialogueLog::kHeader) + "\r\nbroken line\nD\t60\tArni\n");
  CHECK(skipped && skipped->size() == 1 && skipped->front().kind == DialogueEntry::Kind::Divider);
}

static void test_rolling_file() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "oc_test_dialogue_log";
  std::filesystem::remove_all(dir);
  const std::filesystem::path path = dir / "OpenCross" / DialogueLog::kFileName;

  DialogueLog memory_only;
  memory_only.set_path(path);
  CHECK(memory_only.load() && memory_only.entries().empty());
  memory_only.append(line_entry(1));
  CHECK(memory_only.entries().size() == 1 && !std::filesystem::exists(path));

  DialogueLog log;
  log.set_path(path);
  log.set_writable(true);
  CHECK(log.load());
  const u32 before = log.generation();
  log.append(line_entry(0));
  CHECK(log.generation() != before);
  CHECK(std::filesystem::exists(path) && line_count(read_file(path)) == 2);
  for (int i = 1; i < 2500; ++i) log.append(line_entry(i));
  CHECK(log.entries().size() == DialogueLog::kMaxEntries);
  CHECK(log.entries().front() == line_entry(1500) && log.entries().back() == line_entry(2499));
  // The file grows by one line per entry and is cut back to the newest 1,000 when it would reach 2,000.
  const size_t lines = line_count(read_file(path)) - 1;
  CHECK(lines >= DialogueLog::kMaxEntries && lines < 2 * DialogueLog::kMaxEntries);
  CHECK(!std::filesystem::exists(path.string() + ".tmp"));

  DialogueLog again;
  again.set_path(path);
  CHECK(again.load());
  CHECK(again.entries() == log.entries());

  again.set_writable(true);
  again.clear();
  CHECK(again.entries().empty() && read_file(path) == std::string(DialogueLog::kHeader) + "\n");

  {
    std::ofstream junk(path, std::ios::binary | std::ios::trunc);
    junk << "not a dialogue log";
  }
  DialogueLog bad;
  bad.set_path(path);
  CHECK(!bad.load() && bad.entries().empty());
  CHECK(!std::filesystem::exists(path) && std::filesystem::exists(path.string() + ".bad"));
  std::filesystem::remove_all(dir);
}

int main() {
  test_entry_format();
  test_file_text();
  test_rolling_file();
  if (g_fail == 0) std::printf("dialogue_log: all passed\n");
  return g_fail == 0 ? 0 : 1;
}
