// Field message window width of an element name shown by script item id (ItemWork text, code 0x19): a test mod adds element record
// 527 (pgdat.bin one record longer) and its sysmes name; a message naming 0x1000 + 527 is as wide as that name in the build's font,
// and the vanilla element ids keep the width of the executable's own strings (0x200 + id and 0x1000 + id measure alike).
// Needs the game install (OPENCROSS_GAMEDIR or a parent of the exe); without it the test reports SKIP and passes.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "data/tables.h"
#include "game/field_world.h"
#include "import/ps1_font.h"
#include "import/ps1_text.h"
#include "import/text.h"
#include "platform/paths.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

using namespace oc;
namespace fs = std::filesystem;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

constexpr int kModElement = data::kVanillaElemRecs;
constexpr char32_t kModName[] = U"Phantom Bolt";

// The message "{itemwork(0)}" and its end: script variable 0x1A names the item, the end code adds the page leaf's 12.
int item_message_width(game::FieldWorld& world, int item) {
  world.vm().set_var(0x1A, static_cast<s32>(item));
  const std::vector<u8> bytes{0x19, 0x00, 0x00};
  return world.ps1_message_width(bytes, -1) - 12;
}
}  // namespace

int main() {
  const char* configured = std::getenv("OPENCROSS_GAMEDIR");
  const auto dir = platform::find_game_dir(configured ? configured : "");
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP: no game install\n");
    return 0;
  }
  const auto pgdat = files.read("cdrom/battle/data/pgdat.bin");
  const auto exe = files.read("cdrom/prog/slps_023.64");
  if (!pgdat || !exe) {
    std::printf("SKIP: no element table or executable\n");
    return 0;
  }

  const fs::path root = fs::temp_directory_path() / "oc_test_message_width";
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path mod = root / "mods" / "WidthTest";
  fs::create_directories(mod / "battle" / "data");
  fs::create_directories(mod / "text");
  {
    Bytes longer = *pgdat;
    longer.resize(static_cast<size_t>(kModElement + 1) * 0x2C, 0);
    std::ofstream(mod / "battle" / "data" / "pgdat.bin", std::ios::binary).write(reinterpret_cast<const char*>(longer.data()),
                                                                                  static_cast<std::streamsize>(longer.size()));
    std::ofstream(mod / "text" / "sysmes.txt", std::ios::binary) << "key\ten\n0x17020F\t" << import::u32_to_utf8(kModName) << "\n";
  }
  vfs::ModManager mods;
  mods.scan(files, root / "mods", {{"WidthTest", true, vfs::ModLoadOn::All}});
  mods.apply(files, vfs::ModArt::Modded);
  std::vector<import::TextMod> text_mods;
  for (const vfs::Mod& found : mods.mods()) {
    import::TextMod text{found.name, {}, {}};
    for (const vfs::ModFile& file : found.text)
      text.files[file.target] = [&files, source = vfs::OverlayFile{file.file, file.member}] { return files.read_mod_file(source); };
    text_mods.push_back(std::move(text));
  }
  import::set_text_mods(std::move(text_mods));

  // The mod's record is an element of its own, past the executable's element names.
  data::GameTables tables;
  CHECK(tables.load(files));
  CHECK(data::mod_element(tables.elements, kModElement));
  import::ExeStrings strings;
  const import::Ps1TextBuild* build = import::load_exe_strings(*exe, strings);
  CHECK(build && strings.count(import::ps1_category_table(*build, 0x17)) <= static_cast<size_t>(kModElement));

  const auto font = import::load_ps1_font(files);
  CHECK(font != nullptr);
  game::FieldWorld world;
  world.load_text_metrics(files);
  const int name_width = font ? font->width(kModName) : 0;
  CHECK(name_width > 0);
  CHECK(item_message_width(world, data::kScriptElementBase + kModElement) == name_width);

  // Vanilla elements: both item id ranges read the executable's string.
  for (int id : {1, 2, 0x10, 0x31, 0x40, 0x61, 0x7D, 0x80, 0xA3, 0x100, 0x150, 0x1FF})
    CHECK(item_message_width(world, data::kScriptElementBase + id) == item_message_width(world, 0x200 + id));

  import::set_text_mods({});
  fs::remove_all(root, ec);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("message width: ok\n");
  return 0;
}
