#include "vfs/disc_names.h"

#include <algorithm>
#include <cctype>
#include <iterator>

#include "vfs/vfs.h"

namespace oc::vfs {

namespace {

// One record of disc 2's index that differs from disc 1's.
struct DiscRecordChange {
  int record;
  const char* name;
};

#include "vfs/disc_names.inc"

std::string expand_stored(std::string_view stored) {
  if (stored.empty() || stored.starts_with("c:/")) return std::string(stored);
  return std::string(kDiscWorkRoot) + std::string(stored);
}

}  // namespace

std::vector<std::string> parse_disc_records(ByteSpan index) {
  std::vector<std::string> out;
  out.reserve(index.size() / 0x40);
  for (size_t offset = 0; offset + 0x40 <= index.size(); offset += 0x40) {
    std::string path;
    for (size_t i = 0; i < 0x40 && index[offset + i]; ++i) {
      const char ch = static_cast<char>(index[offset + i]);
      path += ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    out.push_back(std::move(path));
  }
  return out;
}

const std::vector<std::string>& builtin_disc_records(int disc) {
  static const std::vector<std::string> disc1 = [] {
    std::vector<std::string> records;
    records.reserve(std::size(kDisc1));
    for (const char* stored : kDisc1) records.push_back(expand_stored(stored));
    return records;
  }();
  static const std::vector<std::string> disc2 = [] {
    std::vector<std::string> records = disc1;
    for (const DiscRecordChange& change : kDisc2Changes) {
      if (static_cast<size_t>(change.record) >= records.size()) records.resize(static_cast<size_t>(change.record) + 1);
      records[static_cast<size_t>(change.record)] = expand_stored(change.name);
    }
    return records;
  }();
  return disc >= 2 ? disc2 : disc1;
}

Bytes builtin_index_bytes(int disc) {
  const std::vector<std::string>& records = builtin_disc_records(disc);
  Bytes out(records.size() * 0x40, 0);
  for (size_t i = 0; i < records.size(); ++i)
    std::copy_n(records[i].begin(), std::min<size_t>(records[i].size(), 0x3F), out.begin() + static_cast<std::ptrdiff_t>(i * 0x40));
  return out;
}

std::string disc_record_member(std::string_view record) {
  std::string_view rest = record;
  if (rest.starts_with(kDiscWorkRoot)) rest.remove_prefix(kDiscWorkRoot.size());
  else if (rest.size() >= 3 && rest[1] == ':' && rest[2] == '/') rest.remove_prefix(3);
  std::string out;
  out.reserve(rest.size());
  for (char ch : rest)
    if (ch != '/' || (!out.empty() && out.back() != '/')) out += ch;
  return out;
}

DiscNames::DiscNames(const std::vector<std::string>& disc1, const std::vector<std::string>& disc2) {
  const std::vector<std::string>* indexes[2] = {&disc1, &disc2};
  for (int side = 0; side < 2; ++side) {
    const std::vector<std::string>& records = *indexes[side];
    count_[side] = records.size();
    for (size_t record = 0; record < records.size(); ++record) {
      std::string member = disc_record_member(records[record]);
      if (member.empty()) continue;
      std::vector<int>& named = named_[side][member];
      named.push_back(static_cast<int>(record));
      if (named.size() == 1 && (side == 0 || !named_[0].contains(member))) members_.push_back(std::move(member));
    }
  }
}

const DiscNames& DiscNames::builtin() {
  static const DiscNames names(builtin_disc_records(1), builtin_disc_records(2));
  return names;
}

std::string DiscNames::record_path(int record) {
  std::string number = std::to_string(record);
  if (number.size() < 5) number.insert(0, 5 - number.size(), '0');
  return "cdrom/" + std::string(kRecordFolder) + number;
}

std::vector<int> DiscNames::records_of(std::string_view path, int disc) const {
  const auto [archive, member] = split_archive_path(path);
  if (archive != "cdrom") return {};
  const int side = disc >= 2 ? 1 : 0;
  if (member.starts_with(kRecordFolder)) {
    const std::string_view digits = std::string_view(member).substr(kRecordFolder.size());
    if (digits.empty() || digits.size() > 9) return {};
    size_t record = 0;
    for (char ch : digits) {
      if (ch < '0' || ch > '9') return {};
      record = record * 10 + static_cast<size_t>(ch - '0');
    }
    if (record >= count_[side]) return {};
    return {static_cast<int>(record)};
  }
  auto it = named_[side].find(member);
  return it == named_[side].end() ? std::vector<int>{} : it->second;
}

DiscNames::Records DiscNames::find(std::string_view path, int disc) const {
  Records out;
  const int side = disc >= 2 ? 1 : 0;
  if (const std::vector<int> here = records_of(path, side + 1); !here.empty()) out.disc = here.front();
  if (const std::vector<int> there = records_of(path, 2 - side); !there.empty()) out.other = there.front();
  return out;
}

}  // namespace oc::vfs
