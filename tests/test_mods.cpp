// Mods found directly in the mods folder: bare .dat files and ZIP / 7z / RAR archives read live, like folders. The archives are built
// here: ZIP and 7z with libarchive's writer, RAR by hand (stored members need no compressor). Model replacements (models\classic|hd\...glb)
// and the problems reported for them.
#include <archive.h>
#include <archive_entry.h>
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "import/text.h"

#include "vfs/mod_archive.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

using namespace oc;
namespace fs = std::filesystem;

static int g_failed = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failed;                                                  \
    }                                                              \
  } while (0)

using Entries = std::vector<std::pair<std::string, std::string>>;

static void write_archive(const fs::path& file, bool seven_zip, const Entries& entries) {
  archive* writer = archive_write_new();
  if (seven_zip) archive_write_set_format_7zip(writer);
  else archive_write_set_format_zip(writer);
  archive_write_open_filename_w(writer, file.c_str());
  for (const auto& [name, data] : entries) {
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, static_cast<la_int64_t>(data.size()));
    archive_write_header(writer, entry);
    archive_write_data(writer, data.data(), data.size());
    archive_entry_free(entry);
  }
  archive_write_close(writer);
  archive_write_free(writer);
}

static void put_u16(std::string& out, unsigned value) {
  out += static_cast<char>(value & 0xFF);
  out += static_cast<char>((value >> 8) & 0xFF);
}

static void put_u32(std::string& out, unsigned long value) {
  put_u16(out, value & 0xFFFF);
  put_u16(out, (value >> 16) & 0xFFFF);
}

static unsigned long crc_of(const std::string& text) {
  return mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

// RAR 4: signature, main header, stored files. Every header starts with the low 16 bits of the CRC-32 of the rest of the header.
static std::string rar4_file(const Entries& entries) {
  const auto header = [](const std::string& body) {
    std::string out;
    put_u16(out, crc_of(body) & 0xFFFF);
    return out + body;
  };
  std::string out = "Rar!\x1a\x07";
  out += '\0';
  std::string main_body;
  main_body += static_cast<char>(0x73);
  put_u16(main_body, 0);
  put_u16(main_body, 13);
  main_body.append(6, '\0');
  out += header(main_body);
  for (const auto& [name, data] : entries) {
    std::string file_body;
    file_body += static_cast<char>(0x74);
    put_u16(file_body, 0x8000);
    put_u16(file_body, static_cast<unsigned>(32 + name.size()));
    put_u32(file_body, static_cast<unsigned long>(data.size()));
    put_u32(file_body, static_cast<unsigned long>(data.size()));
    file_body += static_cast<char>(3);
    put_u32(file_body, crc_of(data));
    put_u32(file_body, 0x5A210000);
    file_body += static_cast<char>(20);
    file_body += static_cast<char>(0x30);
    put_u16(file_body, static_cast<unsigned>(name.size()));
    put_u32(file_body, 0x81A4);
    file_body += name;
    out += header(file_body);
    out += data;
  }
  return out;
}

static void put_vint(std::string& out, size_t value) {
  while (value >= 0x80) {
    out += static_cast<char>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  out += static_cast<char>(value);
}

// RAR 5: signature, main header, one stored file, end header. A header is CRC-32, then its size as a vint, then the body.
static std::string rar5_file(const std::string& name, const std::string& data) {
  const auto header = [](const std::string& body) {
    std::string sized;
    put_vint(sized, body.size());
    sized += body;
    std::string out;
    put_u32(out, crc_of(sized));
    return out + sized;
  };
  std::string out("Rar!\x1a\x07\x01\0", 8);
  std::string body;
  put_vint(body, 1);
  put_vint(body, 0);
  put_vint(body, 0);
  out += header(body);
  body.clear();
  put_vint(body, 2);
  put_vint(body, 2);
  put_vint(body, data.size());
  put_vint(body, 4);
  put_vint(body, data.size());
  put_vint(body, 0x1A4);
  put_u32(body, crc_of(data));
  put_vint(body, 0);
  put_vint(body, 1);
  put_vint(body, name.size());
  body += name;
  out += header(body);
  out += data;
  body.clear();
  put_vint(body, 5);
  put_vint(body, 0);
  put_vint(body, 0);
  return out + header(body);
}

static const vfs::Mod* find_mod(const vfs::ModManager& mods, const std::string& name) {
  for (const vfs::Mod& mod : mods.mods())
    if (mod.name == name) return &mod;
  return nullptr;
}

int main() {
  const fs::path tmp = fs::temp_directory_path() / "oc_test_mods";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  const fs::path dir = tmp / "mods";
  fs::create_directories(dir);

  std::ofstream(dir / "lang.dat", std::ios::binary) << "PK";
  std::ofstream(dir / "notes.txt") << "not a mod";
  fs::create_directories(dir / "Folder Mod");
  std::ofstream(dir / "Folder Mod" / "x.bin") << "x";
  fs::create_directories(dir / "Folder Mod" / "Wrapper" / "models" / "hd" / "battle" / "player" / "selju2");
  std::ofstream(dir / "Folder Mod" / "Wrapper" / "models" / "hd" / "battle" / "player" / "selju2" / "Selju.glb") << "glb";
  fs::create_directories(dir / "Folder Mod" / "models" / "battle");
  std::ofstream(dir / "Folder Mod" / "models" / "battle" / "nowhere.glb") << "glb";
  // Wrapped in a folder of its own: the voices folder then sits at the top of the mod.
  write_archive(dir / "Wrapped.zip", false, {{"Wrapped Pack/voices/123/5-0-serge.mp3", "zip voice"}, {"Wrapped Pack/textures/tiles/cafef00d.png", "png"}});
  // Not unwrapped: a single textures folder is the layout itself.
  write_archive(dir / "OnlyTextures.zip", false, {{"textures/tiles/cafef00d.png", "png"}});
  write_archive(dir / "Solid.7z", true, {{"voices/9/1-0-kid.mp3", "7z voice"}, {"voices/9/2-0-kid.mp3", "7z voice two"}});
  // Named voices: reads like a folder called voices, even with one scene folder.
  write_archive(dir / "voices.zip", false, {{"55/1-0-npc.mp3", "npc"}});
  std::ofstream(dir / "Rar4.rar", std::ios::binary) << rar4_file({{"voices\\7\\1-0-kid.mp3", "rar4 voice"}});
  std::ofstream(dir / "Rar5.rar", std::ios::binary) << rar5_file("voices/8/1-0-kid.mp3", "rar5 voice");
  std::ofstream(dir / "Broken.zip", std::ios::binary) << "this is not an archive";
  write_archive(dir / "Off.zip", false, {{"voices/1/1-0-kid.mp3", "off"}});
  // Names that could leave a folder mean nothing here: nothing is ever written.
  write_archive(dir / "Hostile.zip", false, {{"../outside.txt", "x"}, {"voices/2/1-0-kid.mp3", "fine"}});

  vfs::Vfs files;
  vfs::ModManager mods;
  mods.scan(files, dir, {{"Off.zip", false, vfs::ModLoadOn::Modded}});
  CHECK(mods.mods().size() == 11);
  CHECK(mods.mods()[0].name == "Off.zip");
  CHECK(find_mod(mods, "notes.txt") == nullptr);
  CHECK(fs::is_regular_file(dir / "Hostile.zip") && !fs::exists(tmp / "outside.txt"));
  const vfs::Mod* lang = find_mod(mods, "lang.dat");
  CHECK(lang && lang->root.empty() && lang->unmatched.size() == 1);

  // Nothing is unpacked: the folder holds what it held, and every archive is known at once, enabled or not.
  size_t entries = 0;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir)) entries += entry.exists() ? 1 : 0;
  CHECK(entries == 12);
  const auto voices = [&](const char* name) {
    const vfs::Mod* mod = find_mod(mods, name);
    return mod && mod->error.empty() ? mod->voices.size() : size_t{99};
  };
  CHECK(voices("Wrapped.zip") == 1);
  CHECK(voices("Solid.7z") == 2);
  CHECK(voices("voices.zip") == 1);
  CHECK(voices("Rar4.rar") == 1);
  CHECK(voices("Rar5.rar") == 1);
  CHECK(voices("Off.zip") == 1);
  CHECK(find_mod(mods, "Wrapped.zip")->textures.size() == 1);
  CHECK(find_mod(mods, "OnlyTextures.zip")->textures.size() == 1);

  const auto line = [&](const fs::path& path) {
    const auto source = mods.packed_voice(path);
    const auto bytes = source ? files.read_mod_file(*source) : std::nullopt;
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(missing)");
  };
  CHECK(line(dir / "Wrapped.zip" / "voices" / "123" / "5-0-serge.mp3") == "zip voice");
  CHECK(line(dir / "WRAPPED.ZIP" / "VOICES" / "123" / "5-0-SERGE.MP3") == "zip voice");
  CHECK(line(dir / "Solid.7z" / "voices" / "9" / "2-0-kid.mp3") == "7z voice two");
  CHECK(line(dir / "voices.zip" / "voices" / "55" / "1-0-npc.mp3") == "npc");
  CHECK(line(dir / "Rar4.rar" / "voices" / "7" / "1-0-kid.mp3") == "rar4 voice");
  CHECK(line(dir / "Rar5.rar" / "voices" / "8" / "1-0-kid.mp3") == "rar5 voice");
  CHECK(line(dir / "Rar5.rar" / "voices" / "8" / "9-9-kid.mp3") == "(missing)");

  // Textures read through the overlay, by priority.
  mods.mods()[0].enabled = true;
  mods.apply(files, vfs::ModArt::Modded);
  const auto texture = files.texture_file("tiles/cafef00d.png");
  CHECK(texture && !texture->member.empty());
  const auto png = texture ? files.read_mod_file(*texture) : std::nullopt;
  CHECK(png && std::string(png->begin(), png->end()) == "png");

  // A model replacement: models\<classic|hd>\<member stem>.glb below any wrapper; without the art set folder it matches nothing.
  const vfs::Mod* with_model = find_mod(mods, "Folder Mod");
  CHECK(with_model && with_model->models.size() == 1 && with_model->models[0].target == "hd/battle/player/selju2/selju" &&
        std::find(with_model->unmatched.begin(), with_model->unmatched.end(), "models/battle/nowhere.glb") != with_model->unmatched.end());
  const auto model = files.model_file("hd/battle/player/selju2/selju");
  CHECK(model && files.model_file("classic/battle/player/selju2/selju") == std::nullopt);
  const unsigned before = vfs::file_problems_generation();
  if (model) vfs::report_file_problem(*model, "not used, joint_03 is missing");
  CHECK(vfs::file_problems_generation() != before && with_model && vfs::file_problems(*with_model).size() == 1 &&
        vfs::file_problems(*with_model)[0] == "Wrapper/models/hd/battle/player/selju2/Selju.glb: not used, joint_03 is missing");

  const vfs::Mod* broken = find_mod(mods, "Broken.zip");
  CHECK(broken && !broken->error.empty() && broken->files.empty());

  // Replacing an archive is seen by the next scan.
  write_archive(dir / "Solid.7z", true, {{"voices/9/1-0-kid.mp3", "new voice"}, {"voices/9/2-0-kid.mp3", "more"}, {"voices/9/3-0-kid.mp3", "third"}});
  fs::last_write_time(dir / "Solid.7z", fs::last_write_time(dir / "Solid.7z") + std::chrono::seconds(5));
  mods.scan(files, dir, mods.prefs());
  CHECK(voices("Solid.7z") == 3);
  CHECK(find_mod(mods, "Folder Mod") && vfs::file_problems(*find_mod(mods, "Folder Mod")).empty());
  CHECK(line(dir / "Solid.7z" / "voices" / "9" / "1-0-kid.mp3") == "new voice");

  // Past the memory limit an archive is walked per read; members are found in any order and in any number of reads.
  {
    const std::string big(static_cast<size_t>(vfs::ModArchive::kMemoryLimit) + 1024, 'b');
    std::ofstream(tmp / "Big.rar", std::ios::binary) << rar4_file({{"first.bin", "first"}, {"big.bin", big}, {"last.bin", "last"}});
    std::string error;
    const auto packed = vfs::open_mod_archive(tmp / "Big.rar", error);
    CHECK(packed && packed->names().size() == 3 && error.empty());
    if (packed) {
      const auto text_of = [&](const char* name) {
        const auto bytes = packed->read(name);
        return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(missing)");
      };
      CHECK(text_of("last.bin") == "last");
      CHECK(text_of("first.bin") == "first");
      CHECK(text_of("last.bin") == "last");
      const auto whole = packed->read("big.bin");
      CHECK(whole && whole->size() == big.size());
      CHECK(!packed->read("nothing.bin"));
      CHECK(packed->may_be_slow());
    }
    fs::rename(tmp / "Big.rar", dir / "Big.rar");
    mods.scan(files, dir, mods.prefs());
    CHECK(find_mod(mods, "Big.rar") && find_mod(mods, "Big.rar")->may_be_slow);
    CHECK(!find_mod(mods, "Solid.7z")->may_be_slow);
    CHECK(!find_mod(mods, "Wrapped.zip")->may_be_slow);
    fs::remove(dir / "Big.rar");
  }

  // A draft folder comes first and enabled, and stays out of the stored preferences (a player's [Mods] never names it).
  {
    fs::create_directories(tmp / "Draft");
    mods.scan(files, dir, mods.prefs(), tmp / "Draft");
    CHECK(!mods.mods().empty() && mods.mods().front().name == vfs::kDraftMod && mods.mods().front().enabled);
    const std::vector<vfs::ModPref> stored = mods.prefs();
    CHECK(std::none_of(stored.begin(), stored.end(), [](const vfs::ModPref& pref) { return pref.name == vfs::kDraftMod; }));
    mods.scan(files, dir, stored);
    CHECK(!find_mod(mods, std::string(vfs::kDraftMod)));
  }

  // A .dat inside an archive cannot be read in place; it is reported, not loaded.
  write_archive(dir / "Nested.zip", false, {{"lang.dat", "PK"}, {"voices/3/1-0-kid.mp3", "x"}});
  mods.scan(files, dir, mods.prefs());
  CHECK(find_mod(mods, "Nested.zip") && find_mod(mods, "Nested.zip")->unmatched.size() == 1 && find_mod(mods, "Nested.zip")->archives.empty());

  // mod.ini at a mod's root is its manifest (core/features.h), not an unmatched file; in a packed mod, after the wrapper folder. One deeper
  // down is an ordinary file. feature_inputs lists the enabled mods that have one, in priority order.
  {
    fs::create_directories(dir / "Manifest Mod" / "docs");
    std::ofstream(dir / "Manifest Mod" / "mod.ini") << "[Mod]\nName=Party Swap\nVersion=1.0\n[PartySwap]\nEnabled=1\n";
    std::ofstream(dir / "Manifest Mod" / "docs" / "mod.ini") << "[Mod]\nName=not the manifest\n";
    write_archive(dir / "Packed Manifest.zip", false, {{"Packed/MOD.INI", "[Mod]\nName=Packed\nRequires=PartySwap\n"}, {"Packed/x.bin", "x"}});
    mods.scan(files, dir, mods.prefs());
    const vfs::Mod* folder = find_mod(mods, "Manifest Mod");
    CHECK(folder && folder->has_manifest && folder->manifest.name == "Party Swap" && folder->manifest.sections.size() == 1);
    CHECK(folder && folder->unmatched.size() == 1 && folder->unmatched[0] == "docs/mod.ini");
    const vfs::Mod* packed = find_mod(mods, "Packed Manifest.zip");
    CHECK(packed && packed->has_manifest && packed->manifest.name == "Packed" && packed->manifest.requires_features.size() == 1);
    CHECK(packed && packed->unmatched.size() == 1);
    for (vfs::Mod& mod : mods.mods())
      if (mod.name == "Packed Manifest.zip") mod.enabled = false;
    const std::vector<features::ModInput> inputs = mods.feature_inputs();
    CHECK(inputs.size() == 1 && inputs[0].name == "Manifest Mod" && inputs[0].manifest == &find_mod(mods, "Manifest Mod")->manifest);
  }

  // shaders/matcaps/*.png and shaders/luts/*.png are shader looks (target "matcaps/<name>" / "luts/<name>", lower case), also inside a
  // wrapped archive; a png elsewhere under shaders is not one.
  {
    fs::create_directories(dir / "Looks" / "shaders" / "matcaps");
    fs::create_directories(dir / "Looks" / "shaders" / "luts");
    fs::create_directories(dir / "Looks" / "shaders" / "other");
    std::ofstream(dir / "Looks" / "shaders" / "matcaps" / "Chrome.png") << "png";
    std::ofstream(dir / "Looks" / "shaders" / "luts" / "dusk.png") << "png";
    std::ofstream(dir / "Looks" / "shaders" / "other" / "x.png") << "png";
    write_archive(dir / "Packed Looks.zip", false, {{"Pack/shaders/luts/Teal.png", "png"}});
    mods.scan(files, dir, mods.prefs());
    const vfs::Mod* looks = find_mod(mods, "Looks");
    CHECK(looks && looks->shaders.size() == 2);
    bool chrome = false, dusk = false;
    for (const vfs::ModFile& file : looks ? looks->shaders : std::vector<vfs::ModFile>{}) {
      chrome |= file.target == "matcaps/chrome";
      dusk |= file.target == "luts/dusk";
    }
    CHECK(chrome && dusk);
    const vfs::Mod* packed = find_mod(mods, "Packed Looks.zip");
    CHECK(packed && packed->shaders.size() == 1 && packed->shaders[0].target == "luts/teal");
  }

  // A mod's <archive>.dat overlays the shipped archive member by member.
  {
    const fs::path game = tmp / "game";
    fs::create_directories(game / "data");
    fs::create_directories(game / "mods" / "Top");
    fs::create_directories(game / "mods" / "Bottom");
    write_archive(game / "data" / "cdrom.dat", false,
                  {{"a.bin", "disc a"}, {"map/b.bin", "disc b"}, {"other/b.bin", "disc other b"}, {"monster/dario/dario.obj", "boss"},
                   {"deep/room/c.bin", "disc c"}, {"deep/room/e.bin", "disc e"}});
    write_archive(game / "data" / "cdrom2.dat", false, {{"a.bin", "hd a"}});
    write_archive(game / "data" / "hd.dat", false, {{"one.txt", "game one"}, {"two.txt", "game two"}, {"three.txt", "game three"}});

    // Top holds one.txt and a file the game does not have; Bottom holds one.txt and two.txt; a loose file in Top wins over Bottom's member.
    write_archive(game / "mods" / "Top" / "hd.dat", false, {{"ONE.txt", "top one"}, {"new.txt", "not in the game"}});
    std::ofstream(game / "mods" / "Top" / "three.txt", std::ios::binary) << "top loose three";
    // New loose files: below an archive folder (after a wrapper), in a mod folder called after an archive, and a stray readme.
    fs::create_directories(game / "mods" / "Top" / "Wrapper" / "cdrom" / "map" / "mapbin");
    std::ofstream(game / "mods" / "Top" / "Wrapper" / "cdrom" / "map" / "mapbin" / "NewRoom.bin", std::ios::binary) << "new room";
    std::ofstream(game / "mods" / "Top" / "readme.txt", std::ios::binary) << "about Top";
    // A name two members carry: below an archive folder, in a folder the game ships nothing in, a new member; in one of the game's
    // folders (a folder left out) or without an archive folder it stays ambiguous.
    fs::create_directories(game / "mods" / "Top" / "Wrapper" / "cdrom" / "third");
    std::ofstream(game / "mods" / "Top" / "Wrapper" / "cdrom" / "third" / "b.bin", std::ios::binary) << "third b";
    std::ofstream(game / "mods" / "Top" / "Wrapper" / "cdrom" / "b.bin", std::ios::binary) << "folder left out";
    std::ofstream(game / "mods" / "Top" / "b.bin", std::ios::binary) << "which b";
    // A name one member carries: below an archive folder, a path whose folders contradict it is a new member (a character's field
    // model named like a boss's); a path that only leaves folders out still replaces it.
    fs::create_directories(game / "mods" / "Top" / "Wrapper" / "cdrom" / "player" / "dario");
    std::ofstream(game / "mods" / "Top" / "Wrapper" / "cdrom" / "player" / "dario" / "dario.obj", std::ios::binary) << "hero";
    std::ofstream(game / "mods" / "Top" / "Wrapper" / "cdrom" / "c.bin", std::ios::binary) << "mod c";
    fs::create_directories(game / "mods" / "Top" / "Wrapper" / "cdrom" / "deep");
    std::ofstream(game / "mods" / "Top" / "Wrapper" / "cdrom" / "deep" / "e.bin", std::ios::binary) << "mod e";
    fs::create_directories(game / "mods" / "cdrom" / "flux");
    std::ofstream(game / "mods" / "cdrom" / "flux" / "ui.bin", std::ios::binary) << "crossfix layout";
    write_archive(game / "mods" / "Bottom" / "hd.dat", false, {{"one.txt", "bottom one"}, {"two.txt", "bottom two"}, {"three.txt", "bottom three"}});
    write_archive(game / "mods" / "cdrom.dat", false, {{"a.bin", "mod a"}, {"map/b.bin", "mod b"}});

    vfs::Vfs game_files;
    CHECK(game_files.open(game));
    const auto text = [&](const char* path) {
      const auto bytes = game_files.read(path);
      return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(missing)");
    };
    vfs::ModManager game_mods;
    const std::vector<vfs::ModPref> order = {{"Top", true, vfs::ModLoadOn::All}, {"Bottom", true, vfs::ModLoadOn::All}, {"cdrom.dat", true, vfs::ModLoadOn::All}};
    game_mods.scan(game_files, game / "mods", order);
    const vfs::Mod* top = find_mod(game_mods, "Top");
    CHECK(top && top->archives.size() == 1 && top->files.size() == 8 && top->added.size() == 4 && top->unmatched.size() == 1);
    CHECK(top && top->ambiguous.size() == 2 && std::find(top->ambiguous.begin(), top->ambiguous.end(), "b.bin") != top->ambiguous.end());
    CHECK(!game_files.exists("cdrom/b.bin"));
    const vfs::Mod* crossfix = find_mod(game_mods, "cdrom");
    CHECK(crossfix && crossfix->added.size() == 1 && crossfix->files.size() == 1 && crossfix->unmatched.empty());
    CHECK(game_mods.conflicts(0).size() == 2);

    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(text("hd/one.txt") == "top one");
    CHECK(text("hd/two.txt") == "bottom two");
    CHECK(text("hd/three.txt") == "top loose three");
    CHECK(text("cdrom/third/b.bin") == "third b");
    // New members are read like replacements; list() still lists only the shipped archive.
    CHECK(text("hd/new.txt") == "not in the game");
    CHECK(game_files.exists("hd/new.txt"));
    // added(): a file a mod adds, not a replacement of a shipped member, not a shipped member alone.
    CHECK(game_files.added("hd/new.txt") && game_files.added("cdrom/third/b.bin"));
    CHECK(!game_files.added("hd/one.txt") && !game_files.added("hd/two.txt") && !game_files.added("cdrom/other/b.bin"));
    CHECK(text("cdrom/map/mapbin/newroom.bin") == "new room");
    CHECK(text("cdrom/flux/ui.bin") == "crossfix layout");
    CHECK(text("cdrom/player/dario/dario.obj") == "hero" && text("cdrom/monster/dario/dario.obj") == "boss");
    CHECK(text("cdrom/deep/room/c.bin") == "mod c" && text("cdrom/deep/room/e.bin") == "mod e");
    CHECK(!game_files.exists("hd/readme.txt") && !game_files.exists("cdrom/readme.txt"));
    CHECK(game_files.list("hd").size() == 3);
    CHECK(game_files.exists("hd/two.txt"));
    CHECK(!game_files.loose_file("hd/two.txt"));
    CHECK(game_files.loose_file("hd/three.txt").has_value());
    // In HD art a cdrom.dat member wins over the cdrom2 copy; a member it lacks follows the usual order.
    game_files.set_hd_mode(true);
    CHECK(text("cdrom/a.bin") == "mod a");
    CHECK(text("cdrom/other/b.bin") == "disc other b");
    game_files.set_hd_mode(false);
    const auto disc = game_files.read_disc("cdrom/map/b.bin");
    CHECK(disc && std::string(disc->begin(), disc->end()) == "mod b");

    // Reordering changes the winner of each member; switching the mods off restores the game.
    const std::uint32_t generation = game_files.generation();
    std::swap(game_mods.mods()[0], game_mods.mods()[1]);
    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(game_files.generation() != generation);
    CHECK(text("hd/one.txt") == "bottom one");
    CHECK(text("hd/three.txt") == "bottom three");
    for (vfs::Mod& mod : game_mods.mods()) mod.enabled = false;
    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(text("hd/one.txt") == "game one");
    CHECK(text("cdrom/a.bin") == "disc a");
  }

  // Game data ignores Load on and stays as booted, whatever changes; art next to it still swaps live.
  {
    CHECK(vfs::is_game_data("lang/en.csv") && vfs::is_game_data("cdrom/battle/data/monster.bin") && vfs::is_game_data("cdrom/menu/trade.bin"));
    CHECK(!vfs::is_game_data("hd/face/2021/00_seljyu.png") && !vfs::is_game_data("cdrom/menu/title/cmptitle.tim") && !vfs::is_game_data("cdrom/map/mapbin/alu_a01f.bin"));
    const fs::path game = tmp / "game2";
    fs::create_directories(game / "data");
    fs::create_directories(game / "mods");
    write_archive(game / "data" / "cdrom.dat", false, {{"battle/data/monster.bin", "game monsters"}, {"map/mapbin/room.bin", "game room"}});
    write_archive(game / "data" / "lang.dat", false, {{"en.csv", "game text"}});
    write_archive(game / "mods" / "Thief.7z", true, {{"data/monster.bin", "thief monsters"}, {"room.bin", "thief room"}});
    write_archive(game / "mods" / "Other.zip", false, {{"data/monster.bin", "other monsters"}});

    vfs::Vfs game_files;
    CHECK(game_files.open(game));
    const auto text = [&](const char* path) {
      const auto bytes = game_files.read(path);
      return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(missing)");
    };
    vfs::ModManager game_mods;
    // Set to load on Modded, booted in HD art.
    game_mods.scan(game_files, game / "mods", {{"Thief.7z", true, vfs::ModLoadOn::Modded}, {"Other.zip", true, vfs::ModLoadOn::Modded}});
    const vfs::Mod* thief = find_mod(game_mods, "Thief.7z");
    CHECK(thief && thief->data.size() == 1 && thief->files.size() == 1);
    CHECK(game_mods.conflicts(0).size() == 1);
    game_mods.apply(game_files, vfs::ModArt::Hd);
    CHECK(text("cdrom/battle/data/monster.bin") == "thief monsters");
    CHECK(text("cdrom/map/mapbin/room.bin") == "game room");

    // An art switch moves only the art.
    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(text("cdrom/map/mapbin/room.bin") == "thief room");
    CHECK(text("cdrom/battle/data/monster.bin") == "thief monsters");
    // Switched off, reordered and rescanned, the booted data stays; the art follows.
    game_mods.mods()[0].enabled = false;
    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(text("cdrom/map/mapbin/room.bin") == "game room");
    CHECK(text("cdrom/battle/data/monster.bin") == "thief monsters");
    game_mods.scan(game_files, game / "mods", game_mods.prefs());
    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(text("cdrom/battle/data/monster.bin") == "thief monsters");

    // The base sound banks are game data; other sound follows Load on and swaps live.
    CHECK(vfs::is_game_data("cdrom/sound/wave/wave0000.dat") && vfs::is_game_data("cdrom/sound/effect/common/effect00.dat"));
    CHECK(!vfs::is_game_data("cdrom/sound/bgm/bgm001.sab") && !vfs::is_game_data("cdrom/sound/wave/wave0500.dat"));
    {
      const fs::path audio = tmp / "game3";
      fs::create_directories(audio / "data");
      fs::create_directories(audio / "mods");
      write_archive(audio / "data" / "cdrom.dat", false, {{"sound/bgm/bgm001.sab", "game song"}, {"sound/wave/wave0000.dat", "game bank"}});
      write_archive(audio / "mods" / "Music.zip", false, {{"sound/bgm/bgm001.sab", "mod song"}, {"sound/wave/wave0000.dat", "mod bank"}});
      vfs::Vfs audio_files;
      CHECK(audio_files.open(audio));
      const auto audio_text = [&](const char* path) {
        const auto bytes = audio_files.read(path);
        return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(missing)");
      };
      vfs::ModManager audio_mods;
      audio_mods.scan(audio_files, audio / "mods", {{"Music.zip", true, vfs::ModLoadOn::Modded}});
      CHECK(audio_mods.mods()[0].files.size() == 1 && audio_mods.mods()[0].data.size() == 1);
      audio_mods.apply(audio_files, vfs::ModArt::Hd);
      CHECK(audio_text("cdrom/sound/bgm/bgm001.sab") == "game song");
      CHECK(audio_text("cdrom/sound/wave/wave0000.dat") == "mod bank");
      audio_mods.apply(audio_files, vfs::ModArt::Modded);
      CHECK(audio_text("cdrom/sound/bgm/bgm001.sab") == "mod song");
      audio_mods.mods()[0].enabled = false;
      audio_mods.apply(audio_files, vfs::ModArt::Modded);
      CHECK(audio_text("cdrom/sound/bgm/bgm001.sab") == "game song");
      CHECK(audio_text("cdrom/sound/wave/wave0000.dat") == "mod bank");
    }

    // The next launch sees the new state.
    vfs::Vfs next_files;
    CHECK(next_files.open(game));
    vfs::ModManager next_mods;
    next_mods.scan(next_files, game / "mods", game_mods.prefs());
    next_mods.apply(next_files, vfs::ModArt::Sd);
    const auto bytes = next_files.read("cdrom/battle/data/monster.bin");
    CHECK(bytes && std::string(bytes->begin(), bytes->end()) == "other monsters");
  }

  // Text rows (import::TextMod): a mod's text\<member> holds only its own rows, merged over the game's member; the first mod in the
  // list wins a row both give. Field rows from room 1000 on are in each mod's own room ids and land under that room's internal id.
  {
    const fs::path game = tmp / "textgame";
    fs::create_directories(game / "data");
    write_archive(game / "data" / "cdrom.dat", false, {{"map/mapbin/room.bin", "game room"}});
    write_archive(game / "data" / "lang.dat", false,
                  {{"en.csv", "27\t0\tgame line\n27\t1\tgame second\n"},
                   {"sysmes.txt", "key\ten\tfr\n0x000001\tPotion\tPotion fr\n"},
                   {"waa_spec.txt", "key\ten\n3\tMonster bones.\n"},
                   {"keyitem.txt", "key\ten\n3\tA map.\n"},
                   {"ome.txt", "key\ten\n1\tMysterious Traveler\n"}});
    fs::create_directories(game / "mods" / "Alpha" / "text");
    fs::create_directories(game / "mods" / "Beta" / "text");
    std::ofstream(game / "mods" / "Alpha" / "text" / "en.csv", std::ios::binary) << "27\t1\talpha second\n1001\t0\talpha room\n1002\t0\tunlisted\n";
    std::ofstream(game / "mods" / "Alpha" / "text" / "SYSMES.txt", std::ios::binary) << "key\ten\n0x000001\tHi-Potion\n0x000099\tNew item\n";
    std::ofstream(game / "mods" / "Beta" / "text" / "en.csv", std::ios::binary) << "27\t1\tbeta second\n1001\t0\tbeta room\n";
    // Beta's own element 527 (its [Elements]): the name and first help line, written under its own id.
    // Beta's own item 300 (its [Items]): the name and second description line.
    std::ofstream(game / "mods" / "Beta" / "text" / "sysmes.txt", std::ios::binary)
        << "key\ten\n0x17020F\tDaze\n0x1A062D\tDazes a foe.\n0x1900DE\tGrunt\n0x00012C\tStar Blade\n0x180046\tSky Key\n";
    std::ofstream(game / "mods" / "Beta" / "text" / "waa_spec.txt", std::ios::binary) << "key\ten\n901\tA blade of stars.\n";
    // ... and its own key item 70: the name above and the first description line.
    std::ofstream(game / "mods" / "Beta" / "text" / "keyitem.txt", std::ios::binary) << "key\ten\n210\tOpens the sky.\n";
    // ... and its own formation 513's battle text (btel rows by scene).
    std::ofstream(game / "mods" / "Beta" / "text" / "btel_en.csv", std::ios::binary) << "513\t0\tStand and fight!\n";
    // ... and its own character 51's profile.
    std::ofstream(game / "mods" / "Beta" / "text" / "ome.txt", std::ios::binary) << "key\ten\n51\tWandering Witch\n";
    vfs::Vfs text_files;
    CHECK(text_files.open(game));
    vfs::ModManager text_mods_found;
    text_mods_found.scan(text_files, game / "mods", {{"Alpha", true, vfs::ModLoadOn::All}, {"Beta", true, vfs::ModLoadOn::All}});
    const vfs::Mod* alpha = find_mod(text_mods_found, "Alpha");
    CHECK(alpha && alpha->text.size() == 2 && alpha->unmatched.empty() && alpha->files.empty());
    std::vector<import::TextMod> text_mods;
    for (const std::string name : {"Alpha", "Beta"}) {
      const vfs::Mod* mod = find_mod(text_mods_found, name);
      if (!mod) continue;
      import::TextMod text{name, {}, {}};
      for (const vfs::ModFile& file : mod->text)
        text.files[file.target] = [&text_files, source = vfs::OverlayFile{file.file, file.member}] { return text_files.read_mod_file(source); };
      // Both call their room 1001; they are different rooms (internal ids 0x10000 / 0x10001).
      text.rooms[1001] = name == "Alpha" ? 0x10000 : 0x10001;
      text_mods.push_back(std::move(text));
    }
    import::set_text_mods(std::move(text_mods));
    // The tables gave Beta's element 527 the engine id 600: its rows move there.
    import::set_text_mod_ids("Beta", "pgdat", {{527, 600}});
    import::set_text_mod_ids("Beta", "monster", {{222, 300}});
    import::set_text_mod_ids("Beta", "scenedat", {{513, 700}});
    import::set_text_mod_ids("Beta", "waa", {{300, 260}});
    import::set_text_mod_ids("Beta", "keyitem", {{70, 66}});
    import::set_text_mod_ids("Beta", "char", {{51, 53}});
    import::LangTables tables;
    tables.init(&text_files);
    const auto row = [&](int room, int msg) {
      const std::string* found = tables.raw_field(room, msg, import::Lang::En);
      return found ? *found : std::string("(none)");
    };
    CHECK(row(27, 0) == "game line");
    CHECK(row(27, 1) == "alpha second");
    CHECK(row(0x10000, 0) == "alpha room" && row(0x10001, 0) == "beta room");
    // 1001 is a mod room (not the game's row 1001); 1002, which Alpha does not list, is the game's row of that id.
    CHECK(row(1001, 0) == "(none)" && row(1002, 0) == "unlisted");
    const import::TextTable* sysmes = tables.table("sysmes");
    const auto cell = [&](const char* key, import::Lang lang) {
      const std::string* found = sysmes ? sysmes->get(std::string_view(key), lang) : nullptr;
      return found ? *found : std::string("(none)");
    };
    // Columns by heading: the mod sets English only, French keeps the game's; a new key is added.
    CHECK(cell("0x000001", import::Lang::En) == "Hi-Potion" && cell("0x000001", import::Lang::Fr) == "Potion fr");
    CHECK(cell("0x000099", import::Lang::En) == "New item");
    CHECK(tables.sysmes(0x170258, import::Lang::En) && *tables.sysmes(0x170258, import::Lang::En) == "Daze");
    CHECK(tables.sysmes(0x1A0708, import::Lang::En) && *tables.sysmes(0x1A0708, import::Lang::En) == "Dazes a foe.");
    CHECK(!tables.sysmes(0x17020F, import::Lang::En));
    CHECK(tables.sysmes(0x19012C, import::Lang::En) && *tables.sysmes(0x19012C, import::Lang::En) == "Grunt");
    CHECK(tables.battle_event(700, 0, import::Lang::En) && *tables.battle_event(700, 0, import::Lang::En) == "Stand and fight!");
    CHECK(!tables.battle_event(513, 0, import::Lang::En));
    // Beta's item 300 is record 260: its name (0x000104) and description line (260 * 3 + 1) move there.
    CHECK(tables.sysmes(0x000104, import::Lang::En) && *tables.sysmes(0x000104, import::Lang::En) == "Star Blade");
    CHECK(!tables.sysmes(0x00012C, import::Lang::En));
    const import::TextTable* spec = tables.table("waa_spec");
    CHECK(spec && spec->get(static_cast<u64>(781), import::Lang::En) && *spec->get(static_cast<u64>(781), import::Lang::En) == "A blade of stars.");
    CHECK(spec && !spec->get(static_cast<u64>(901), import::Lang::En));
    CHECK(spec && spec->get(static_cast<u64>(3), import::Lang::En) && *spec->get(static_cast<u64>(3), import::Lang::En) == "Monster bones.");
    // Beta's key item 70 is key item 66: its name (0x180042) and description line 0 (66 * 3) move there.
    CHECK(tables.sysmes(0x180042, import::Lang::En) && *tables.sysmes(0x180042, import::Lang::En) == "Sky Key");
    const import::TextTable* key_rows = tables.table("keyitem");
    CHECK(key_rows && key_rows->get(static_cast<u64>(198), import::Lang::En) &&
          *key_rows->get(static_cast<u64>(198), import::Lang::En) == "Opens the sky.");
    CHECK(key_rows && !key_rows->get(static_cast<u64>(210), import::Lang::En));
    // Beta's character 51 is character 53: its profile moves there; the game's rows stay.
    const import::TextTable* profiles = tables.table("ome");
    CHECK(profiles && profiles->get(static_cast<u64>(53), import::Lang::En) &&
          *profiles->get(static_cast<u64>(53), import::Lang::En) == "Wandering Witch");
    CHECK(profiles && !profiles->get(static_cast<u64>(51), import::Lang::En));
    CHECK(profiles && profiles->get(static_cast<u64>(1), import::Lang::En) &&
          *profiles->get(static_cast<u64>(1), import::Lang::En) == "Mysterious Traveler");
    // The mods' rows set again (a controlled run's reload: an editor's draft text): a LangTables that read them reads them again.
    std::ofstream(game / "mods" / "Beta" / "text" / "en.csv", std::ios::binary) << "27\t1\tbeta edited\n";
    {
      const vfs::Mod* beta = find_mod(text_mods_found, "Beta");
      import::TextMod text{"Beta", {}, {}};
      for (const vfs::ModFile& file : beta->text)
        text.files[file.target] = [&text_files, source = vfs::OverlayFile{file.file, file.member}] { return text_files.read_mod_file(source); };
      std::vector<import::TextMod> again;
      again.push_back(std::move(text));
      const u32 generation = import::text_mods_generation();
      import::set_text_mods(std::move(again));
      CHECK(import::text_mods_generation() == generation + 1);
    }
    CHECK(row(27, 1) == "beta edited" && row(27, 0) == "game line");
    import::set_text_mods({});
  }

  fs::remove_all(tmp, ec);
  if (g_failed) {
    std::printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  std::printf("mods ok\n");
  return 0;
}
