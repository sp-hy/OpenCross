// Mods found directly in the mods folder: bare .dat files and ZIP / 7z / RAR archives read live, like folders. The archives are built
// here: ZIP and 7z with libarchive's writer, RAR by hand (stored members need no compressor).
#include <archive.h>
#include <archive_entry.h>
#include <miniz.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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

static std::string read_text(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

  const vfs::Mod* broken = find_mod(mods, "Broken.zip");
  CHECK(broken && !broken->error.empty() && broken->files.empty());

  // Replacing an archive is seen by the next scan.
  write_archive(dir / "Solid.7z", true, {{"voices/9/1-0-kid.mp3", "new voice"}, {"voices/9/2-0-kid.mp3", "more"}, {"voices/9/3-0-kid.mp3", "third"}});
  fs::last_write_time(dir / "Solid.7z", fs::last_write_time(dir / "Solid.7z") + std::chrono::seconds(5));
  mods.scan(files, dir, mods.prefs());
  CHECK(voices("Solid.7z") == 3);
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

  // A .dat inside an archive cannot be read in place; it is reported, not loaded.
  write_archive(dir / "Nested.zip", false, {{"lang.dat", "PK"}, {"voices/3/1-0-kid.mp3", "x"}});
  mods.scan(files, dir, mods.prefs());
  CHECK(find_mod(mods, "Nested.zip") && find_mod(mods, "Nested.zip")->unmatched.size() == 1 && find_mod(mods, "Nested.zip")->archives.empty());

  // A mod's <archive>.dat overlays the shipped archive member by member.
  {
    const fs::path game = tmp / "game";
    fs::create_directories(game / "data");
    fs::create_directories(game / "mods" / "Top");
    fs::create_directories(game / "mods" / "Bottom");
    write_archive(game / "data" / "cdrom.dat", false, {{"a.bin", "disc a"}, {"map/b.bin", "disc b"}, {"other/b.bin", "disc other b"}});
    write_archive(game / "data" / "cdrom2.dat", false, {{"a.bin", "hd a"}});
    write_archive(game / "data" / "hd.dat", false, {{"one.txt", "game one"}, {"two.txt", "game two"}, {"three.txt", "game three"}});

    // Top holds one.txt and a file the game does not have; Bottom holds one.txt and two.txt; a loose file in Top wins over Bottom's member.
    write_archive(game / "mods" / "Top" / "hd.dat", false, {{"ONE.txt", "top one"}, {"new.txt", "not in the game"}});
    std::ofstream(game / "mods" / "Top" / "three.txt", std::ios::binary) << "top loose three";
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
    CHECK(top && top->archives.size() == 1 && top->files.size() == 2 && top->unmatched.size() == 1);
    CHECK(game_mods.conflicts(0).size() == 2);

    game_mods.apply(game_files, vfs::ModArt::Modded);
    CHECK(text("hd/one.txt") == "top one");
    CHECK(text("hd/two.txt") == "bottom two");
    CHECK(text("hd/three.txt") == "top loose three");
    // A mod replaces and never adds.
    CHECK(text("hd/new.txt") == "(missing)");
    CHECK(!game_files.exists("hd/new.txt"));
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

  fs::remove_all(tmp, ec);
  if (g_failed) {
    std::printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  std::printf("mods ok\n");
  return 0;
}
