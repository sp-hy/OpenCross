// Disc reader on synthetic images written to a temp folder: a small disc with the Chrono Cross layout (ISO volume, file table,
// disc-number sector) in every container (cooked iso, raw bin, cue with one and two files, ccd, mds with subchannel, m3u, single
// and multi-disc pbp), CHD and encrypted-PBP refusal, the table decode, identification of an unknown build and of other discs.
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "import/disc/disc_ident.h"
#include "import/disc/disc_image.h"
#include "import/disc/disc_table.h"
#include "import/disc/sha256.h"

using namespace oc;
using namespace oc::import::disc;
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

constexpr u32 kSectors = 50;
constexpr u32 kForm2Lba = 49;
constexpr u32 kExeSize = 7 * 2048 - 1000;
constexpr u32 kBigFileSize = 5000;
const std::string kSystemCnf = "BOOT = cdrom:\\SLPS_999.99;1\r\nTCB = 4\r\n";

void put32(u8* out, u32 value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<u8>(value >> (8 * i));
}

void dir_record(Bytes& dir, size_t& at, const std::string& name, u32 lba, u32 size, bool directory) {
  const size_t length = 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0);
  dir[at] = static_cast<u8>(length);
  put32(&dir[at + 2], lba);
  put32(&dir[at + 10], size);
  dir[at + 25] = directory ? 0x02 : 0x00;
  dir[at + 32] = static_cast<u8>(name.size());
  std::memcpy(&dir[at + 33], name.data(), name.size());
  at += length;
}

u8 pattern(u32 lba, size_t i) { return static_cast<u8>(lba * 7 + i * 13 + 1); }

// User data of every sector of the test disc.
std::vector<Bytes> make_user_sectors(const std::string& volume, u32 disc_number) {
  std::vector<Bytes> sectors(kSectors, Bytes(kUserSectorSize, 0));
  Bytes& pvd = sectors[16];
  pvd[0] = 1;
  std::memcpy(&pvd[1], "CD001", 5);
  pvd[6] = 1;
  std::memset(&pvd[8], ' ', 64);
  std::memcpy(&pvd[8], "PLAYSTATION", 11);
  std::memcpy(&pvd[40], volume.data(), volume.size());
  put32(&pvd[80], kSectors);
  pvd[156] = 34;
  put32(&pvd[158], 22);
  put32(&pvd[166], kUserSectorSize);
  pvd[181] = 0x02;
  size_t at = 0;
  dir_record(sectors[22], at, std::string(1, '\0'), 22, kUserSectorSize, true);
  dir_record(sectors[22], at, std::string(1, '\1'), 22, kUserSectorSize, true);
  dir_record(sectors[22], at, "SLPS_999.99;1", 38, kExeSize, false);
  dir_record(sectors[22], at, "SYSTEM.CNF;1", 45, static_cast<u32>(kSystemCnf.size()), false);
  // Records: 0 the executable (7 sectors), 1 SYSTEM.CNF, 2 an empty slot, 3 a 3-sector file, 4 an empty slot at the end.
  const u32 words[] = {38u | ((7 * 2048 - kExeSize) / 8) << 24, 45u | ((2048 - 40u) / 8) << 24, 46u | 0x800000u,
                       46u | ((3 * 2048 - kBigFileSize) / 8) << 24, 49u | 0x800000u};
  for (size_t i = 0; i < 5; ++i) put32(&sectors[24][4 * i], words[i]);
  put32(&sectors[37][0], disc_number);
  for (u32 lba = 38; lba < 45; ++lba)
    for (size_t i = 0; i < kUserSectorSize; ++i) sectors[lba][i] = pattern(lba, i);
  std::memcpy(sectors[45].data(), kSystemCnf.data(), kSystemCnf.size());
  for (u32 lba = 46; lba < 49; ++lba)
    for (size_t i = 0; i < kUserSectorSize; ++i) sectors[lba][i] = pattern(lba, i);
  return sectors;
}

u8 bcd(u32 value) { return static_cast<u8>((value / 10) << 4 | value % 10); }

// Mode-2 raw sectors; the last one is form 2 with a 2324-byte payload.
Bytes make_raw(const std::vector<Bytes>& user) {
  Bytes raw(user.size() * kRawSectorSize, 0);
  for (u32 lba = 0; lba < user.size(); ++lba) {
    u8* sector = &raw[lba * kRawSectorSize];
    std::memset(sector + 1, 0xFF, 10);
    const u32 address = lba + 150;
    sector[12] = bcd(address / 4500);
    sector[13] = bcd(address / 75 % 60);
    sector[14] = bcd(address % 75);
    sector[15] = 2;
    const u8 submode = lba == kForm2Lba ? kSubmodeForm2 | kSubmodeAudio : kSubmodeData;
    sector[16] = sector[20] = 1;
    sector[18] = sector[22] = submode;
    if (lba == kForm2Lba) {
      for (size_t i = 0; i < kForm2PayloadSize; ++i) sector[24 + i] = static_cast<u8>(i * 3);
    } else {
      std::memcpy(sector + 24, user[lba].data(), kUserSectorSize);
    }
  }
  return raw;
}

Bytes join(const std::vector<Bytes>& user) {
  Bytes out;
  for (const Bytes& sector : user) out.insert(out.end(), sector.begin(), sector.end());
  return out;
}

void write_file(const fs::path& path, const void* data, size_t size) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}
void write_file(const fs::path& path, const Bytes& bytes) { write_file(path, bytes.data(), bytes.size()); }
void write_file(const fs::path& path, const std::string& text) { write_file(path, text.data(), text.size()); }

// Every 2048-byte sector the source gives matches the user data (the form-2 sector aside), and the table decodes.
void check_disc(const SectorSource& source, const std::vector<Bytes>& user, bool raw, u32 disc_number) {
  CHECK(source.sector_count() == kSectors);
  CHECK(source.raw() == raw);
  const Bytes all = source.read_user(0, kForm2Lba);
  CHECK(all.size() == kForm2Lba * kUserSectorSize);
  bool same = all.size() == kForm2Lba * kUserSectorSize;
  for (u32 lba = 0; same && lba < kForm2Lba; ++lba) same = std::memcmp(&all[lba * kUserSectorSize], user[lba].data(), kUserSectorSize) == 0;
  CHECK(same);
  CHECK(source.read_user(kSectors - 1, 2).empty());
  u8 sector[kRawSectorSize];
  CHECK(source.read_raw(0, 1, sector) == raw);
  const std::optional<SectorInfo> info = source.info(kForm2Lba);
  CHECK(info.has_value());
  u8 payload[kForm2PayloadSize];
  if (raw) {
    CHECK(info && info->mode == 2 && info->form == 2 && (info->submode & kSubmodeAudio) && info->payload_size() == kForm2PayloadSize);
    CHECK(source.info(30) && source.info(30)->form == 1 && source.info(30)->submode == kSubmodeData);
    CHECK(source.read_form2(kForm2Lba, payload) && payload[100] == static_cast<u8>(300) && payload[2323] == static_cast<u8>(2323 * 3));
  } else {
    CHECK(info && info->mode == 1 && info->form == 1);
    CHECK(!source.read_form2(kForm2Lba, payload));
  }
  const DiscIdentity id = identify(source);
  CHECK(id.chrono_cross);
  CHECK(!id.known_build);
  CHECK(id.region == Region::Japan);
  CHECK(id.boot_file == "SLPS_999.99");
  CHECK(id.disc_number == disc_number);
  CHECK(id.file_records == 5);
}

void test_table_decode() {
  const std::vector<Bytes> user = make_user_sectors("CHRONOCROSS", 1);
  const FileTable table = FileTable::decode(user[24], kSectors);
  CHECK(table.size() == 5);
  CHECK(table.span(0).lba == 38 && table.span(0).size == kExeSize && !table.span(0).empty);
  CHECK(table.span(1).size == 40);
  CHECK(table.span(2).empty && table.span(2).size == 0 && table.span(2).lba == 46);
  CHECK(table.span(3).lba == 46 && table.span(3).size == kBigFileSize);
  CHECK(table.span(4).empty && table.span(4).size == 0);
  CHECK(table.plausible(kSectors));
  CHECK(!table.plausible(40));
  CHECK(boot_file_name("BOOT\t= cdrom:\\SLUS_010.41;1\r\nTCB = 4\r\n") == "SLUS_010.41");
  CHECK(boot_file_name("boot=cdrom:slps_023.64;1") == "SLPS_023.64");
  CHECK(boot_file_name("TCB = 4\n").empty());
  CHECK(region_of_boot_file("SCPS_100.00") == Region::Japan);
  CHECK(region_of_boot_file("SLES_000.00") == Region::Unknown);
  CHECK(sha256_hex(ByteSpan()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string abc = "abc";
  CHECK(sha256_hex(ByteSpan(reinterpret_cast<const u8*>(abc.data()), abc.size())) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_text(1000, 'a');
  CHECK(sha256_hex(ByteSpan(reinterpret_cast<const u8*>(long_text.data()), long_text.size())) ==
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

void test_containers(const fs::path& dir) {
  const std::vector<Bytes> user = make_user_sectors("CHRONOCROSS", 1);
  const Bytes raw = make_raw(user);
  std::string error;

  write_file(dir / "cooked.iso", join(user));
  CHECK(detect_format(dir / "cooked.iso") == ImageFormat::CookedImage);
  if (auto source = open_image(dir / "cooked.iso", &error)) {
    check_disc(*source, user, false, 1);
    const std::optional<FileTable> table = FileTable::read(*source, &error);
    CHECK(table && table->size() == 5);
    if (table) {
      const std::optional<Bytes> file = read_record(*source, *table, 3);
      CHECK(file && file->size() == kBigFileSize && (*file)[2048] == pattern(47, 0) && (*file)[4999] == pattern(48, 4999 - 4096));
      CHECK(read_record(*source, *table, 2) && read_record(*source, *table, 2)->empty());
      CHECK(!read_record(*source, *table, 5));
    }
  } else {
    CHECK(!"cooked iso opens");
  }

  write_file(dir / "plain.bin", raw);
  CHECK(detect_format(dir / "plain.bin") == ImageFormat::RawImage);
  if (auto source = open_image(dir / "plain.bin", &error)) check_disc(*source, user, true, 1);
  else CHECK(!"raw bin opens");

  write_file(dir / "my disc.bin", raw);
  write_file(dir / "single.cue", std::string("\xEF\xBB\xBF") + "FILE \"my disc.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
  CHECK(detect_format(dir / "single.cue") == ImageFormat::Cue);
  if (auto source = open_image(dir / "single.cue", &error)) check_disc(*source, user, true, 1);
  else CHECK(!"cue opens");

  write_file(dir / "part a.bin", raw.data(), 30 * kRawSectorSize);
  write_file(dir / "tracks" / "b.bin", raw.data() + 30 * kRawSectorSize, raw.size() - 30 * kRawSectorSize);
  write_file(dir / "multi.cue",
             std::string("FILE \"part a.bin\" BINARY\n TRACK 01 MODE2/2352\n  INDEX 01 00:00:00\nFILE tracks/b.bin BINARY\n TRACK 02 "
                         "MODE2/2352\n  INDEX 00 00:00:00\n  INDEX 01 00:00:10\n"));
  if (auto source = open_image(dir / "multi.cue", &error)) {
    check_disc(*source, user, true, 1);
    CHECK(source->description().starts_with("cue"));
  } else {
    CHECK(!"multi-file cue opens");
  }

  write_file(dir / "cooked data.bin", join(user));
  write_file(dir / "cooked.cue", std::string("FILE \"cooked data.bin\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n"));
  if (auto source = open_image(dir / "cooked.cue", &error)) check_disc(*source, user, false, 1);
  else CHECK(!"cooked cue opens");

  write_file(dir / "bad.cue", std::string("FILE \"x.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"));
  CHECK(!open_image(dir / "bad.cue", &error) && error.find("BINARY") != std::string::npos);

  write_file(dir / "clone.img", raw);
  write_file(dir / "clone.sub", Bytes(kSectors * kSubchannelSize, 0));
  write_file(dir / "clone.ccd", std::string("[CloneCD]\r\nVersion=3\r\n[Disc]\r\nTocEntries=4\r\n[TRACK 1]\r\nMODE=2\r\nINDEX 1=0\r\n"));
  CHECK(detect_format(dir / "clone.ccd") == ImageFormat::CloneCd);
  if (auto source = open_image(dir / "clone.ccd", &error)) {
    check_disc(*source, user, true, 1);
    CHECK(source->description().starts_with("ccd+img+sub"));
  } else {
    CHECK(!"ccd opens");
  }

  Bytes mdf;
  for (u32 lba = 0; lba < kSectors; ++lba) {
    mdf.insert(mdf.end(), raw.begin() + lba * kRawSectorSize, raw.begin() + (lba + 1) * kRawSectorSize);
    mdf.insert(mdf.end(), kSubchannelSize, static_cast<u8>(0xA5));
  }
  write_file(dir / "alcohol.mdf", mdf);
  Bytes mds(0xE0, 0);
  std::memcpy(mds.data(), "MEDIA DESCRIPTOR", 16);
  put32(&mds[0x50], 0x58);
  mds[0x58 + 0x0A] = 1;
  put32(&mds[0x58 + 0x14], 0x70);
  mds[0x70] = 0xAB;
  mds[0x70 + 1] = 0x08;
  mds[0x70 + 4] = 1;
  put32(&mds[0x70 + 0x0C], 0xC0);
  mds[0x70 + 0x10] = static_cast<u8>(2448 & 0xFF);
  mds[0x70 + 0x11] = static_cast<u8>(2448 >> 8);
  put32(&mds[0x70 + 0x34], 0xC8);
  put32(&mds[0xC0], 150);
  put32(&mds[0xC4], kSectors);
  put32(&mds[0xC8], 0xD0);
  std::memcpy(&mds[0xD0], "*.mdf", 5);
  write_file(dir / "alcohol.mds", mds);
  CHECK(detect_format(dir / "alcohol.mds") == ImageFormat::Mds);
  if (auto source = open_image(dir / "alcohol.mds", &error)) {
    check_disc(*source, user, true, 1);
    CHECK(source->description().find("subchannel") != std::string::npos);
  } else {
    CHECK(!"mds opens");
  }

  const std::vector<Bytes> user2 = make_user_sectors("CHRONOCROSS", 2);
  write_file(dir / "sub" / "disc2.iso", join(user2));
  write_file(dir / "set.m3u", std::string("# a set\r\ncooked.iso\r\n\r\nsub/disc2.iso\r\n"));
  CHECK(detect_format(dir / "set.m3u") == ImageFormat::Playlist);
  DiscSet set = open_disc_set(dir / "set.m3u");
  CHECK(set.discs.size() == 2);
  if (set.discs.size() == 2) {
    check_disc(*set.discs[0], user, false, 1);
    check_disc(*set.discs[1], user2, false, 2);
  }
  CHECK(!open_image(dir / "set.m3u", &error, 2));
  write_file(dir / "broken.m3u", std::string("cooked.iso\nmissing.iso\n"));
  CHECK(open_disc_set(dir / "broken.m3u").discs.empty());

  write_file(dir / "image.chd", std::string("MComprHD") + std::string(120, '\x01'));
  CHECK(detect_format(dir / "image.chd") == ImageFormat::Chd);
  CHECK(!open_image(dir / "image.chd", &error) && error.find("CHD") != std::string::npos);

  write_file(dir / "random.bin", std::string(5000, '\x01'));
  CHECK(detect_format(dir / "random.bin") == ImageFormat::Unknown);
  CHECK(!open_image(dir / "random.bin", &error));
}

// One PSISOIMG disc: header, block index at +0x4000, blocks from +0x100000 (block 0 deflated, the others stored).
Bytes make_psisoimg(const Bytes& raw) {
  constexpr size_t kBlock = 16 * kRawSectorSize;
  Bytes disc(0x100000, 0);
  std::memcpy(disc.data(), "PSISOIMG0000", 12);
  u32 offset = 0;
  for (size_t block = 0; block * kBlock < raw.size(); ++block) {
    Bytes plain(kBlock, 0);
    std::memcpy(plain.data(), raw.data() + block * kBlock, std::min(kBlock, raw.size() - block * kBlock));
    Bytes stored = plain;
    if (block == 0) {
      Bytes packed(kBlock * 2);
      const size_t size = tdefl_compress_mem_to_mem(packed.data(), packed.size(), plain.data(), plain.size(), TDEFL_DEFAULT_MAX_PROBES);
      packed.resize(size);
      stored = packed;
    }
    put32(&disc[0x4000 + block * 32], offset);
    disc[0x4000 + block * 32 + 4] = static_cast<u8>(stored.size());
    disc[0x4000 + block * 32 + 5] = static_cast<u8>(stored.size() >> 8);
    disc.insert(disc.end(), stored.begin(), stored.end());
    offset += static_cast<u32>(stored.size());
  }
  return disc;
}

Bytes pbp_header(u32 psar) {
  Bytes header(psar, 0);
  std::memcpy(header.data(), "\0PBP", 4);
  put32(&header[4], 0x10000);
  put32(&header[0x24], psar);
  return header;
}

void test_pbp(const fs::path& dir) {
  const std::vector<Bytes> user = make_user_sectors("CHRONOCROSS", 1);
  const std::vector<Bytes> user2 = make_user_sectors("CHRONOCROSS", 2);
  std::string error;

  Bytes single = pbp_header(0x100);
  const Bytes disc1 = make_psisoimg(make_raw(user));
  single.insert(single.end(), disc1.begin(), disc1.end());
  write_file(dir / "game.pbp", single);
  CHECK(detect_format(dir / "game.pbp") == ImageFormat::Pbp);
  if (auto source = open_image(dir / "game.pbp", &error)) {
    CHECK(source->sector_count() == 64);
    const Bytes all = source->read_user(0, kForm2Lba);
    bool same = all.size() == kForm2Lba * kUserSectorSize;
    for (u32 lba = 0; same && lba < kForm2Lba; ++lba) same = std::memcmp(&all[lba * kUserSectorSize], user[lba].data(), kUserSectorSize) == 0;
    CHECK(same);
    const DiscIdentity id = identify(*source);
    CHECK(id.chrono_cross && id.disc_number == 1);
  } else {
    std::printf("pbp: %s\n", error.c_str());
    CHECK(!"single pbp opens");
  }

  Bytes title(0x400, 0);
  std::memcpy(title.data(), "PSTITLEIMG000000", 16);
  const Bytes disc2 = make_psisoimg(make_raw(user2));
  put32(&title[0x200], 0x400);
  put32(&title[0x204], static_cast<u32>(0x400 + disc1.size()));
  Bytes multi = pbp_header(0x100);
  multi.insert(multi.end(), title.begin(), title.end());
  multi.insert(multi.end(), disc1.begin(), disc1.end());
  multi.insert(multi.end(), disc2.begin(), disc2.end());
  write_file(dir / "set.pbp", multi);
  DiscSet set = open_disc_set(dir / "set.pbp");
  CHECK(set.discs.size() == 2);
  if (set.discs.size() == 2) {
    CHECK(identify(*set.discs[0]).disc_number == 1);
    CHECK(identify(*set.discs[1]).disc_number == 2);
  }

  Bytes encrypted = single;
  for (size_t i = 0x100 + 0x100000; i < 0x100 + 0x100000 + 64; ++i) encrypted[i] ^= 0x5A;
  write_file(dir / "encrypted.pbp", encrypted);
  CHECK(!open_image(dir / "encrypted.pbp", &error) && error.find("encrypted") != std::string::npos);
  Bytes scrambled = pbp_header(0x100);
  scrambled.insert(scrambled.end(), 0x200, static_cast<u8>(0x77));
  write_file(dir / "scrambled.pbp", scrambled);
  CHECK(!open_image(dir / "scrambled.pbp", &error) && error.find("encrypted") != std::string::npos);
}

void test_identity(const fs::path& dir) {
  std::string error;
  write_file(dir / "other.iso", join(make_user_sectors("OTHERGAME", 1)));
  if (auto source = open_image(dir / "other.iso", &error)) {
    const DiscIdentity id = identify(*source);
    CHECK(!id.chrono_cross && !id.reason.empty() && id.boot_file == "SLPS_999.99" && id.exe_sha256.size() == 64);
  } else {
    CHECK(!"other iso opens");
  }
  write_file(dir / "disc7.iso", join(make_user_sectors("CHRONOCROSS", 7)));
  if (auto source = open_image(dir / "disc7.iso", &error)) CHECK(!identify(*source).chrono_cross);
  std::vector<Bytes> no_volume = make_user_sectors("CHRONOCROSS", 1);
  no_volume[16][0] = 0;
  write_file(dir / "novolume.iso", join(no_volume));
  if (auto source = open_image(dir / "novolume.iso", &error)) CHECK(!identify(*source).chrono_cross);
}

void test_threads(const fs::path& dir) {
  std::string error;
  const std::unique_ptr<SectorSource> source = open_image(dir / "multi.cue", &error);
  if (!source) return;
  const std::vector<Bytes> user = make_user_sectors("CHRONOCROSS", 1);
  std::vector<int> bad(4, 0);
  std::vector<std::thread> threads;
  for (int k = 0; k < 4; ++k)
    threads.emplace_back([&, k] {
      for (int i = 0; i < 200; ++i) {
        const u32 lba = static_cast<u32>((i * 7 + k * 11) % kForm2Lba);
        const Bytes sector = source->read_user(lba, 1);
        if (sector != user[lba]) ++bad[static_cast<size_t>(k)];
      }
    });
  for (std::thread& thread : threads) thread.join();
  for (const int count : bad) CHECK(count == 0);
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / ("oc_test_disc_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  test_table_decode();
  test_containers(dir);
  test_pbp(dir);
  test_identity(dir);
  test_threads(dir);
  fs::remove_all(dir, ec);
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("disc: ok\n");
  return failures ? 1 : 0;
}
