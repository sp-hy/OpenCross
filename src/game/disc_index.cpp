#include "game/disc_index.h"

#include <cctype>

#include "core/log.h"
#include "vfs/vfs.h"

namespace oc::game {

namespace {
std::vector<std::string> parse(ByteSpan bytes) {
  std::vector<std::string> out;
  out.reserve(bytes.size() / 0x40);
  for (size_t offset = 0; offset + 0x40 <= bytes.size(); offset += 0x40) {
    std::string path;
    for (size_t i = 0; i < 0x40 && bytes[offset + i]; ++i) {
      const char ch = static_cast<char>(bytes[offset + i]);
      path += ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    out.push_back(std::move(path));
  }
  return out;
}
const std::string kEmpty;
}  // namespace

bool DiscIndex::from_bytes(ByteSpan disc1, ByteSpan disc2) {
  rec_[0] = parse(disc1);
  rec_[1] = disc2.empty() ? rec_[0] : parse(disc2);
  return !rec_[0].empty();
}

bool DiscIndex::load(const vfs::Vfs& vfs) {
  const auto disc1 = vfs.read("cdrom/cdrom.fnd");
  if (!disc1) return false;
  const auto disc2 = vfs.read("cdrom/cdrom2.fnd");
  const bool ok = from_bytes(*disc1, disc2 ? ByteSpan(*disc2) : ByteSpan());
  if (ok) log::info("disc index: {} records (disc 2 {})", rec_[0].size(), disc2 ? "present" : "missing, shares disc 1");
  return ok;
}

const std::string& DiscIndex::record(int disc, int index) const {
  const auto& records = rec_[disc >= 2 ? 1 : 0];
  return index >= 0 && static_cast<size_t>(index) < records.size() ? records[static_cast<size_t>(index)] : kEmpty;
}

std::string DiscIndex::battle_script(int disc, int scene) const {
  const std::string& rec = record(disc, kBattleScriptBase + scene);
  const size_t at = rec.find("/battle/event/");
  return at == std::string::npos ? "cdrom/battle/event/evt0000.pac" : "cdrom" + rec.substr(at);
}

std::string DiscIndex::room_stem(int disc, int room_id) const {
  if (room_id < 0) return {};
  const std::string& rec = record(disc, kMapBase + 3 * room_id);
  const size_t at = rec.find("/map/mapbin/");
  if (at == std::string::npos || !rec.ends_with(".bin")) return {};
  return rec.substr(at + 12, rec.size() - at - 12 - 4);
}

bool DiscIndex::has_room(int disc, std::string_view stem) const {
  const size_t count = rec_[disc >= 2 ? 1 : 0].size();
  for (int id = 0; kMapBase + 3 * static_cast<size_t>(id) < count; ++id)
    if (room_stem(disc, id) == stem) return true;
  return false;
}

std::string DiscIndex::movie_name_of(int disc, int number) const {
  if (number < 0) return {};
  const std::string& rec = record(disc, kMovieBase + number);
  const size_t at = rec.find("/movie/");
  if (at == std::string::npos || !rec.ends_with(".stv")) return {};
  std::string name = rec.substr(at + 7, rec.size() - at - 7 - 4);
  return name == "newop" ? "a01op" : name;
}

// The movie parameter table of the field overlay, {start, end, mode, number, volume} per row. Disc 1 uses rows 0..23, disc 2 the
// rows from 24.
namespace {
constexpr DiscIndex::MovieRow kMovieRows[37] = {
    {0, 0x8e8, 0, 0x13, 0x6e}, {0, 0x113, 0, 1, 0x7f},  {0, 0x1d3, 0, 2, 0x7f},  {0, 0x14e, 0, 3, 0x7f},  {0, 0x32d, 0, 4, 0x7f},
    {0, 0x8b, 0, 5, 0x7f},     {0, 0x10e, 0, 6, 0x7f},  {0, 0x1e9, 0, 7, 0x7f},  {0, 0xca, 0, 8, 0x7f},   {0, 0x179, 0, 9, 0x7f},
    {0, 0xbe, 0, 0xa, 0x7f},   {0, 0x1d3, 0, 0xb, 0x7f}, {0, 0x176, 0, 0xc, 0x7f}, {0, 0x19c, 0, 0xd, 0x7f}, {0, 0x284, 0, 0xe, 0x7f},
    {0, 0x2de, 0, 0xf, 0x7f},  {0, 0x41, 0, 0, 0x7f},   {0, 0x1ed, 0, 0x10, 0x7f}, {0, 0x319, 0, 0x11, 0x7f}, {0, 0x5b8, 0, 0x12, 0x7f},
    {0x133, 0x1e9, 0, 7, 0x50}, {0, 0x5a, 0, 5, 0x50},  {0x118, 0x1ab, 0, 0xf, 0x50}, {0, 0x102, 0, 0x10, 0x50},
    {0, 0x1ba, 0, 0, 0x50},    {0, 0x18b, 0, 1, 0x7f},  {0, 0x474, 0, 2, 0},     {0, 0x1ba, 0, 0, 0},     {0, 0x294, 0, 3, 0x7f},
    {0, 0x1be, 0, 4, 0x7f},    {0, 0x2fc, 0, 5, 0x7f},  {0, 0x59d, 0, 6, 0x7f},  {0, 0xcd, 0, 7, 0x7f},   {0, 0xfa, 0, 8, 0x7f},
    {0, 0x403, 0, 9, 0x7f},    {0, 0x22f6, 3, 0xa, 0x50}, {0, 0x1fc2, 4, 0xa, 0x50},
};
}  // namespace

const DiscIndex::MovieRow* DiscIndex::movie_row(int disc, int op) {
  const int row = (disc >= 2 ? 24 : 0) + op;
  if (op < 0 || row >= 37 || (disc < 2 && row >= 24)) return nullptr;
  return &kMovieRows[row];
}

void DiscIndex::movie_range(int disc, int op, u32& start, u32& end) {
  start = end = 0;
  const MovieRow* row = movie_row(disc, op);
  if (!row) return;
  if (disc < 2) {
    if (op == 20) start = 1u + 0x131u, end = row->end;
    else if (op == 21 || op == 23) start = 1u + row->start, end = row->end;
    else if (op == 22) start = 1u + row->start, end = row->end - 10u;
  } else if (op == 12) {
    start = 1u + row->start, end = row->end;
  }
}

std::string DiscIndex::movie_name(int disc, int op) const {
  const MovieRow* row = movie_row(disc, op);
  return row ? movie_name_of(disc, row->number) : std::string();
}

}  // namespace oc::game
