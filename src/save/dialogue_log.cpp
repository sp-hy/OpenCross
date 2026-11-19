#include "save/dialogue_log.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>

#include "core/log.h"

namespace oc::save {

namespace {

std::string escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\\': out += "\\\\"; break;
      default: out += ch; break;
    }
  }
  return out;
}

std::string unescape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 == text.size()) {
      out += text[i];
      continue;
    }
    switch (text[++i]) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      default: out += text[i]; break;
    }
  }
  return out;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
  std::vector<std::string_view> fields;
  for (size_t from = 0;;) {
    const size_t tab = line.find('\t', from);
    fields.push_back(line.substr(from, tab == std::string_view::npos ? std::string_view::npos : tab - from));
    if (tab == std::string_view::npos) return fields;
    from = tab + 1;
  }
}

template <typename T>
bool parse_number(std::string_view text, T& value) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  return ec == std::errc() && end == text.data() + text.size();
}

std::string_view strip_cr(std::string_view line) { return !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line; }

}  // namespace

std::string DialogueLog::format_entry(const DialogueEntry& entry) {
  if (entry.kind == DialogueEntry::Kind::Divider) return "D\t" + std::to_string(entry.play_seconds) + "\t" + escape(entry.area);
  std::string line = "L\t" + std::to_string(entry.face) + "\t" + escape(entry.speaker) + "\t" + escape(entry.text) + "\t" + escape(entry.choice);
  for (const std::string& voice : entry.voices) line += "\t" + escape(voice);
  return line;
}

std::optional<DialogueEntry> DialogueLog::parse_entry(std::string_view line) {
  const std::vector<std::string_view> fields = split_tabs(strip_cr(line));
  DialogueEntry entry;
  if (fields[0] == "D" && fields.size() == 3) {
    entry.kind = DialogueEntry::Kind::Divider;
    if (!parse_number(fields[1], entry.play_seconds)) return std::nullopt;
    entry.area = unescape(fields[2]);
    return entry;
  }
  if (fields[0] != "L" || fields.size() < 5 || !parse_number(fields[1], entry.face)) return std::nullopt;
  entry.speaker = unescape(fields[2]);
  entry.text = unescape(fields[3]);
  entry.choice = unescape(fields[4]);
  for (size_t i = 5; i < fields.size(); ++i)
    if (!fields[i].empty()) entry.voices.push_back(unescape(fields[i]));
  return entry;
}

std::string DialogueLog::format_file(const std::vector<DialogueEntry>& entries) {
  std::string text = std::string(kHeader) + "\n";
  for (const DialogueEntry& entry : entries) text += format_entry(entry) + "\n";
  return text;
}

std::optional<std::vector<DialogueEntry>> DialogueLog::parse_file(std::string_view text) {
  const size_t first_end = text.find('\n');
  if (strip_cr(text.substr(0, first_end)) != kHeader) return std::nullopt;
  std::vector<DialogueEntry> entries;
  for (size_t from = first_end; from != std::string_view::npos && from + 1 < text.size();) {
    const size_t end = text.find('\n', from + 1);
    const std::string_view line = text.substr(from + 1, end == std::string_view::npos ? std::string_view::npos : end - from - 1);
    if (std::optional<DialogueEntry> entry = parse_entry(line)) entries.push_back(std::move(*entry));
    from = end;
  }
  if (entries.size() > kMaxEntries) entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(kMaxEntries));
  return entries;
}

bool DialogueLog::load() {
  entries_.clear();
  file_lines_ = 0;
  ++generation_;
  std::error_code ec;
  if (path_.empty() || !std::filesystem::exists(path_, ec)) return true;
  std::ifstream in(path_, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::optional<std::vector<DialogueEntry>> parsed = parse_file(text);
  if (!parsed) {
    in.close();
    std::filesystem::path bad = path_;
    bad += ".bad";
    std::filesystem::rename(path_, bad, ec);
    log::warn("dialogue log: {} is not a dialogue log; moved to {}", path_.string(), bad.string());
    return false;
  }
  entries_ = std::move(*parsed);
  for (size_t at = text.find('\n'); at != std::string::npos && at + 1 < text.size(); at = text.find('\n', at + 1)) ++file_lines_;
  return true;
}

bool DialogueLog::rewrite() const {
  if (path_.empty()) return false;
  std::error_code ec;
  if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);
  std::filesystem::path tmp = path_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    const std::string text = format_file(entries_);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
      log::warn("dialogue log: cannot write {}", tmp.string());
      return false;
    }
  }
  std::filesystem::rename(tmp, path_, ec);
  if (ec) log::warn("dialogue log: cannot replace {}: {}", path_.string(), ec.message());
  return !ec;
}

void DialogueLog::append(DialogueEntry entry) {
  entries_.push_back(std::move(entry));
  if (entries_.size() > kMaxEntries) entries_.erase(entries_.begin(), entries_.end() - static_cast<std::ptrdiff_t>(kMaxEntries));
  ++generation_;
  if (!writable_ || path_.empty()) return;
  std::error_code ec;
  if (file_lines_ + 1 >= 2 * kMaxEntries || !std::filesystem::exists(path_, ec)) {
    if (rewrite()) file_lines_ = entries_.size();
    return;
  }
  std::ofstream out(path_, std::ios::binary | std::ios::app);
  const std::string line = format_entry(entries_.back()) + "\n";
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  if (out) ++file_lines_;
  else log::warn("dialogue log: cannot append to {}", path_.string());
}

void DialogueLog::clear() {
  entries_.clear();
  ++generation_;
  if (writable_ && rewrite()) file_lines_ = 0;
}

}  // namespace oc::save
