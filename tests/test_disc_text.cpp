// The game's text from the PS1 files against RDE's lang.dat, on the installed game: one LangTables reads the install as it is, the
// other sees only the PS1 file tree (cdrom.dat holds the Japanese discs' files byte for byte), so it takes every string from the
// PS1 files. Compares every room message, battle event message and table row of the Japanese text, prints the match counts and a
// few differences per kind, and checks room 27 (alu_a01f) message 15 in full. Needs the game install (SKIP without one). With the
// player's own disc images (OC_TEST_DISC_JP / OC_TEST_DISC_USA, else Documents\DuckStation\games), the same text through the disc
// reader: the JP disc gives the room messages the file tree gives, the USA disc its English (names, a room, menus, no glyph left
// undecoded in any room of disc 1).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "import/disc/disc_backend.h"
#include "import/disc/disc_image.h"
#include "import/ps1_text.h"
#include "import/text.h"
#include "platform/paths.h"
#include "vfs/rde_backend.h"
#include "vfs/vfs.h"

using namespace oc;
using import::Lang;

namespace {

int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// The install with only its PS1 file tree, the way a disc session sees the game.
class CdromOnly final : public vfs::Backend {
 public:
  explicit CdromOnly(const std::filesystem::path& data_dir) : inner_(data_dir) {}
  bool has_archive(std::string_view archive) const override { return archive == "cdrom" && inner_.has_archive(archive); }
  bool contains(std::string_view archive, const std::string& member, int disc) const override {
    return archive == "cdrom" && inner_.contains(archive, member, disc);
  }
  std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const override {
    return archive == "cdrom" ? inner_.read(archive, member, disc) : std::nullopt;
  }
  std::vector<std::string> list(std::string_view archive) const override {
    return archive == "cdrom" ? inner_.list(archive) : std::vector<std::string>();
  }

 private:
  vfs::RdeBackend inner_;
};

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
}

// Match counts of one kind, with the first few differences printed.
struct Tally {
  const char* name;
  int match = 0, differ = 0, missing = 0;
  void add(const std::string& key, const std::string* disc, const std::string* rde) {
    if (!disc) {
      ++missing;
      return;
    }
    if (*disc == *rde) {
      ++match;
      return;
    }
    if (++differ <= 3) std::printf("  %s %s\n    disc %s\n    rde  %s\n", name, key.c_str(), disc->c_str(), rde->c_str());
  }
  void print() const { std::printf("%-9s %6d of %6d match, %5d differ, %5d without a PS1 source\n", name, match, match + differ + missing, differ, missing); }
};

import::FieldTable member_rows(const vfs::Vfs& files, const char* member) {
  import::FieldTable table;
  if (const auto raw = files.read(std::string("lang/") + member)) table.parse(import::decode_text_member(*raw), true);
  return table;
}

// RDE prefixes some battle rows with a window size and does not end a page break with a newline; the window reads neither.
std::string battle_form(std::string text) {
  if ((text.starts_with("{wmode") || text.starts_with("{size")) && text.find('}') != std::string::npos) text.erase(0, text.find('}') + 1);
  replace_all(text, "{bpage}\\n", "{bpage}");
  while (text.ends_with("\\n")) text.resize(text.size() - 2);
  return text;
}

void compare_tables(import::LangTables& rde, import::LangTables& disc, const char* name, int floor) {
  Tally tally{name};
  const import::TextTable* want = rde.table(name);
  const import::TextTable* got = disc.table(name);
  CHECK(want && got);
  if (!want || !got) return;
  for (size_t i = 0; i < want->rows(); ++i) {
    const std::string& key = want->row(i)[0];
    const std::string* rde_text = want->get(std::string_view(key), Lang::Jp);
    if (!rde_text) continue;
    tally.add(key, got->get(std::string_view(key), Lang::Jp), rde_text);
  }
  tally.print();
  CHECK(tally.match >= floor);
}

// Disc 1 of a build through the disc reader; false when the image is absent.
bool mount_disc(vfs::Vfs& files, const char* variable, const char* name) {
  std::filesystem::path path;
  if (const char* set = std::getenv(variable)) path = set;
  else if (const char* home = std::getenv("USERPROFILE")) path = std::filesystem::path(home) / "Documents" / "DuckStation" / "games" / name;
  std::error_code error_code;
  if (path.empty() || !std::filesystem::exists(path, error_code)) return false;
  std::string error;
  std::shared_ptr<const import::disc::SectorSource> source = import::disc::open_image(path, &error);
  auto backend = std::make_unique<import::disc::DiscBackend>();
  if (!source || !backend->mount(1, source, &error)) {
    std::printf("FAIL %s: %s\n", path.string().c_str(), error.c_str());
    ++failures;
    return false;
  }
  files.open(std::move(backend), path.parent_path());
  return true;
}

void check_disc_images(import::LangTables& tree) {
  vfs::Vfs jp_files;
  if (mount_disc(jp_files, "OC_TEST_DISC_JP", "Chrono Cross (Japan, Asia) (Disc 1).cue")) {
    import::LangTables jp;
    jp.init(&jp_files);
    CHECK(jp.from_disc());
    int same = 0;
    for (int msg = 0; msg < 40; ++msg) same += jp.lookup_field(27, msg, Lang::Jp) == tree.lookup_field(27, msg, Lang::Jp);
    CHECK(same == 40);
    std::printf("JP disc image: room 27 messages 0..39 equal to the file tree's\n");
  } else {
    std::printf("JP disc image: SKIP\n");
  }
  vfs::Vfs usa_files;
  if (!mount_disc(usa_files, "OC_TEST_DISC_USA", "Chrono Cross (USA) (Disc 1).cue")) {
    std::printf("USA disc image: SKIP\n");
    return;
  }
  import::LangTables usa;
  usa.init(&usa_files);
  CHECK(usa.from_disc());
  import::Ps1Text text;
  CHECK(text.open(usa_files) && std::string(text.build().name) == "usa");
  CHECK(usa.default_name(0, Lang::En) == U"Serge" && usa.default_name(1, Lang::En) == U"Kid");
  const auto elder = usa.lookup_field(27, 15, Lang::En);
  CHECK(elder && elder->find("lion shark") != std::string::npos);
  if (elder) std::printf("USA room 27 message 15: %s\n", elder->c_str());
  int messages = 0, undecoded = 0, grammar_left = 0;
  for (int room = 0; room < 1600; ++room) {
    if (!import::room_name_for_id(room)) continue;
    for (int msg = 0;; ++msg) {
      const std::string* markup = text.field(room, msg);
      if (!markup) break;
      ++messages;
      // 0xFF has no glyph in the USA font; a few messages carry it.
      bool unknown = false;
      for (size_t at = markup->find('['); at != std::string::npos && at + 3 < markup->size(); at = markup->find('[', at + 1))
        unknown |= (*markup)[at + 3] == ']' && std::isxdigit(static_cast<unsigned char>((*markup)[at + 1])) &&
                   std::isxdigit(static_cast<unsigned char>((*markup)[at + 2])) && markup->compare(at, 4, "[ff]") != 0;
      if (unknown && ++undecoded <= 3)
        std::printf("  undecoded %d/%d: %s\n", room, msg, markup->c_str());
      const auto resolved = usa.lookup_field(room, msg, Lang::En);
      if (resolved && resolved->find("{gram(") != std::string::npos) ++grammar_left;
    }
  }
  std::printf("USA disc 1: %d room messages, %d with an undecoded glyph other than 0xFF, %d with a grammar form left\n", messages, undecoded, grammar_left);
  CHECK(messages > 20000 && undecoded == 0 && grammar_left == 0);
  const import::TextTable* menu = usa.table("menutest");
  const import::TextTable* titles = usa.table("savetbl");
  CHECK(menu && menu->rows() > 80 && titles && titles->rows() > 20);
  if (const std::string* yes = menu ? menu->get(std::string_view("0x801F27E4"), Lang::En) : nullptr) std::printf("USA menu 0x801F27E4: %s\n", yes->c_str());
  if (const std::string* arni = titles ? titles->get(std::string_view("2"), Lang::En) : nullptr) std::printf("USA save title 2: %s\n", arni->c_str());
  const std::string* battle = usa.battle_event(386, 1, Lang::En);
  CHECK(battle && !battle->empty());
}

}  // namespace

int main() {
  const char* configured = std::getenv("OPENCROSS_GAMEDIR");
  const auto dir = platform::find_game_dir(configured ? configured : "");
  vfs::Vfs rde_files;
  if (dir.empty() || !rde_files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  vfs::Vfs disc_files;
  disc_files.open(std::make_unique<CdromOnly>(dir / "data"), dir);
  import::LangTables rde, disc;
  rde.init(&rde_files);
  disc.init(&disc_files);
  CHECK(!rde.from_disc());
  CHECK(disc.from_disc());

  // Room 27 message 15: the PS1 text with RDE's one word changed ("海の男" -> "海の民").
  const auto elder = disc.lookup_field(27, 15, Lang::Jp);
  auto elder_rde = rde.lookup_field(27, 15, Lang::Jp);
  CHECK(elder && elder_rde);
  if (elder && elder_rde) {
    replace_all(*elder_rde, "海の民", "海の男");
    CHECK(*elder == *elder_rde);
  }

  Tally field{"field"};
  const import::FieldTable jp = member_rows(rde_files, "jp.csv");
  std::map<u64, const std::string*> ordered;
  for (const auto& [key, text] : jp.rows) ordered.emplace(key, &text);
  for (const auto& [key, unused] : ordered) {
    const int room = static_cast<int>(key >> 32), msg = static_cast<int>(key & 0xFFFFFFFFu);
    const auto want = rde.lookup_field(room, msg, Lang::Jp);
    const auto got = disc.lookup_field(room, msg, Lang::Jp);
    field.add(std::to_string(room) + "/" + std::to_string(msg), got ? &*got : nullptr, &*want);
  }
  field.print();
  CHECK(field.match >= 56400);

  Tally battle{"btel"};
  const import::FieldTable btel = member_rows(rde_files, "btel_jp.csv");
  std::map<u64, const std::string*> battle_rows;
  for (const auto& [key, text] : btel.rows) battle_rows.emplace(key, &text);
  for (const auto& [key, text] : battle_rows) {
    const std::string* got = disc.battle_event(static_cast<u32>(key >> 32), static_cast<u32>(key & 0xFFFFFFFFu), Lang::Jp);
    const std::string want = battle_form(*text);
    const std::string got_form = got ? battle_form(*got) : std::string();
    battle.add(std::to_string(key >> 32) + "/" + std::to_string(key & 0xFFFFFFFFu), got ? &got_form : nullptr, &want);
  }
  battle.print();
  CHECK(battle.match >= 200);

  compare_tables(rde, disc, "sysmes", 2550);
  compare_tables(rde, disc, "btmes", 55);
  compare_tables(rde, disc, "keyitem", 175);
  compare_tables(rde, disc, "waa_spec", 560);
  compare_tables(rde, disc, "menutest", 100);
  compare_tables(rde, disc, "shopmenu", 60);
  compare_tables(rde, disc, "savetbl", 25);
  compare_tables(rde, disc, "ome", 0);
  CHECK(!disc.table("cmnmes") && !disc.table("ui_pausemenu"));
  CHECK(disc.default_name(1, Lang::Jp) == rde.default_name(1, Lang::Jp));
  check_disc_images(disc);

  std::printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
