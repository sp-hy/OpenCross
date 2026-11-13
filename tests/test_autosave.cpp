// Auto-save (RDE): trigger rule + slot 0 round trip through a save.dat built in memory (no real save is touched); the OpenCross
// save page (a temp folder) and where an auto-save goes.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include "game/autosave.h"

using namespace oc;
using game::Autosave;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static Autosave::Field make_field(u16 loc, bool allowed = true, int entrance = 0) { return {loc, allowed, loc, entrance}; }

static void test_trigger() {
  Autosave autosave;
  // Not allowed: no write. Allowed turning on writes even at the same location; the same location then does not; a location
  // change does.
  CHECK(!autosave.update(make_field(5, false)));
  CHECK(autosave.update(make_field(5)));
  CHECK(!autosave.update(make_field(5)));
  CHECK(autosave.update(make_field(6)));
  CHECK(!autosave.update(make_field(6)));
  // A change while not allowed is recorded without a write; allowed again writes.
  CHECK(!autosave.update(make_field(7, false)));
  CHECK(autosave.update(make_field(7)));
  autosave.note_load();
  // The first tick after a load only records.
  CHECK(!autosave.update(make_field(9)));
  CHECK(!autosave.update(make_field(9)));
  CHECK(autosave.update(make_field(10)));
  // Excluded entrances: nothing written, but the location is recorded.
  CHECK(!autosave.update(make_field(0xFA, true, 0x14)));
  CHECK(!autosave.update(make_field(0xFA, true, 0x14)));
  CHECK(autosave.update(make_field(0xFB, true, 0x18)));
  CHECK(!autosave.update(make_field(0xFC, true, 0x1D)));
  CHECK(autosave.update(make_field(0xFD, true, 0x16)));
  CHECK(Autosave::entrance_excluded(0xFA, 0x13) && !Autosave::entrance_excluded(0xFA, 0x17));
  CHECK(Autosave::entrance_excluded(0xFB, 0x17) && !Autosave::entrance_excluded(0xFB, 0x18));
  CHECK(Autosave::entrance_excluded(0xFC, 0x10) && !Autosave::entrance_excluded(0xFC, 0x16) && !Autosave::entrance_excluded(0xFC, 0x1B));
  CHECK(Autosave::entrance_excluded(0xFD, 0x17) && !Autosave::entrance_excluded(0xFD, 0x16) && !Autosave::entrance_excluded(0xFD, 0x1C));
  CHECK(!Autosave::entrance_excluded(0x10, 0x14));
}

static void test_write() {
  save::MemCard blank = save::MemCard::blank();
  std::vector<save::ZipMember> members;
  members.push_back({"memcard1.mcr", blank.bytes(), true});
  members.push_back({"memcard2.mcr", blank.bytes(), true});
  save::SaveDat dat = save::SaveDat::open(save::zip_write(members, save::kRdeSavePassword, 1));

  save::SaveSlot st;
  st.head[0] = 'S';
  st.head[1] = 'C';
  st.head[save::head::kLocation] = 0x42;
  st.set_gold(1234);
  st.set_playtime_frames(777);
  CHECK(Autosave::write(dat, st));
  // The caller's state is untouched.
  CHECK(st.head[save::head::kLeaderName] == 0);

  const save::MemCard* card = dat.card(1);
  const auto slots = save::list_slots(*card);
  CHECK(slots.size() == 1 && slots[0].slot == 0 && slots[0].name == "BISLPSP02364CHRONO00");
  const save::SaveSlot back = save::read_slot(*card, slots[0].block);
  CHECK(back.gold() == 1234 && back.playtime_frames() == 777 && back.location() == 0x42);
  CHECK(back.head[save::head::kLeaderName] == 0x83 && back.head[save::head::kLeaderName + 11] == 0x75);
  CHECK(save::block_checksum_ok(card->block(slots[0].block)));
  // RDE's loader verdict, emulated: version at 0x1FF8; > 0x102 adds the header frame (0..0x17F) to the payload sum.
  {
    const auto blk = card->block(slots[0].block);
    auto u32at = [&](size_t offset) { return u32(blk[offset]) | u32(blk[offset + 1]) << 8 | u32(blk[offset + 2]) << 16 | u32(blk[offset + 3]) << 24; };
    const u32 ver = u32at(0x1FF8);
    u32 sum = 0;
    for (size_t i = 0x180; i < 0x1FF8; ++i) sum += blk[i];
    if (ver > 0x102)
      for (size_t i = 0; i < 0x180; ++i) sum += blk[i];
    CHECK(ver == 0x106);
    // A mismatch would read as "data corrupted"; 0x24 holds the summary stream bit count.
    CHECK(sum == u32at(0x1FFC));
    CHECK(u32at(0x24) == 210);
    CHECK(blk[0x50] == 1 && blk[0x52] == 'C' && blk[0x53] == 'R' && blk[0x54] == 'D' && blk[0x55] == '0');
    // Playtime: the summary copy (0x37, unaligned) and the state word (0x588) both carry the frames.
    const u32 pv = u32(blk[0x37]) | u32(blk[0x38]) << 8 | u32(blk[0x39]) << 16 | u32(blk[0x3A]) << 24;
    CHECK(pv == 777 && u32at(0x588) == 777);
  }
  // A second write reuses the block; a manual slot 1 gets its own.
  CHECK(Autosave::write(dat, st));
  CHECK(save::write_slot(*dat.card(1), 1, st) != 0);
  CHECK(save::list_slots(*dat.card(1)).size() == 2);
  // ext_save.dat: no host = untouched; with a host slot 0 gets the language / counter / names and the other records stay.
  CHECK(!dat.member("ext_save.dat"));
  std::map<int, std::u32string> names{{0, U"Zed"}};
  save::ExtHost host;
  host.language = 2;
  host.battles = 77;
  host.names = &names;
  dat.set_ext_host(&host);
  CHECK(Autosave::write(dat, st));
  CHECK(dat.member("ext_save.dat") && dat.member("ext_save.dat")->data.size() == save::kExtSize);
  const save::ExtSlot ex = dat.ext().slot(0);
  CHECK(ex.language == 2 && ex.battles == 77 && ex.names == names);
  CHECK(dat.ext().slot(1).names.empty());
  // Serialize / reload keeps it.
  save::SaveDat re = save::SaveDat::open(dat.serialize(1));
  CHECK(re.card(1) && re.card(1)->find("BISLPSP02364CHRONO00") != 0);
  CHECK(re.ext().slot(0).battles == 77 && re.ext().slot(0).names == names);
}

static save::SaveDat blank_dat() {
  save::MemCard blank = save::MemCard::blank();
  std::vector<save::ZipMember> members;
  members.push_back({"memcard1.mcr", blank.bytes(), true});
  members.push_back({"memcard2.mcr", blank.bytes(), true});
  return save::SaveDat::open(save::zip_write(members, save::kRdeSavePassword, 1));
}

static void test_page() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "oc_test_ocsaves";
  std::filesystem::remove_all(dir);
  std::map<int, std::u32string> names{{0, U"Sergio"}, {43, U"Pipp\u00e9"}};
  save::ExtHost host;
  host.language = 1;
  host.battles = 42;
  host.names = &names;
  save::OcSaves page;
  page.set_dir(dir);
  page.set_ext_host(&host);
  page.load();
  CHECK(page.empty() && page.newest() == 0);
  save::SaveSlot st;
  st.head[0] = 'S';
  st.head[1] = 'C';
  st.set_gold(4321);
  page.write(7, st);
  page.write(100, st);
  Autosave::write(page, st);
  CHECK(std::filesystem::exists(dir / "007.ocsav") && std::filesystem::exists(dir / "100.ocsav") && std::filesystem::exists(dir / "auto.ocsav"));
  // Two writes in a row can share a file time (the clock's resolution), so the later write is dated one second after the first.
  std::filesystem::last_write_time(dir / "100.ocsav", std::filesystem::last_write_time(dir / "007.ocsav") + std::chrono::seconds(1));
  // A fresh scan reads them back: block, names, counter; the auto-save carries the caption, the newest is the last manual write.
  save::OcSaves again;
  again.set_dir(dir);
  again.set_ext_host(&host);
  again.load();
  CHECK(again.file(7) && again.file(100) && again.file(0) && !again.file(1));
  CHECK(again.file(7)->data.gold() == 4321);
  CHECK(again.file(0)->data.head[save::head::kLeaderName] == 0x83);
  CHECK(again.newest() == 100);
  host.battles = 0;
  CHECK(again.read_ext(7, 1).names == names && host.battles == 42);
  CHECK(again.read_ext(7, 0).names.empty());
  // A damaged file is skipped.
  {
    Bytes bytes = save::OcSaves::serialize(st, {});
    bytes[bytes.size() - 1] ^= 0xFF;
    save::OcSaves::File parsed;
    bool threw = false;
    try {
      parsed = save::OcSaves::parse(bytes);
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }
  // Version 2 keeps the mod room's stem (ids are each mod's own); a version 1 file (no room field) still loads, with no room.
  {
    save::ExtSlot ext;
    ext.room = "flux1005";
    const Bytes bytes = save::OcSaves::serialize(st, ext);
    CHECK(save::OcSaves::parse(bytes).ext.room == "flux1005");
    Bytes v1(bytes.begin(), bytes.end() - 64);
    v1[4] = 1;
    const save::OcSaves::File old = save::OcSaves::parse(v1);
    CHECK(old.ext.room.empty() && old.data.gold() == 4321);
    host.room = [] { return std::string("flux1007"); };
    page.write(9, st);
    save::OcSaves third;
    third.set_dir(dir);
    third.set_ext_host(&host);
    third.load();
    CHECK(third.file(9) && third.read_ext(9, 1).room == "flux1007");
    host.room = nullptr;
  }
  std::filesystem::remove_all(dir);
}

static void test_targets() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "oc_test_targets";
  std::filesystem::remove_all(dir);
  save::OcSaves page;
  page.set_dir(dir);
  save::SaveDat dat = blank_dat();
  save::SaveSlot st;
  st.head[0] = 'S';
  st.head[1] = 'C';
  // Nothing anywhere: the page only.
  Autosave::Targets targets = Autosave::targets(&dat, &page);
  CHECK(!targets.card && targets.page);
  // A memory-card save only (card 2 counts too): the card only.
  CHECK(save::write_slot(*dat.card(2), 3, st) != 0);
  targets = Autosave::targets(&dat, &page);
  CHECK(targets.card && !targets.page);
  // Both hold saves: both.
  page.write(1, st);
  targets = Autosave::targets(&dat, &page);
  CHECK(targets.card && targets.page);
  // The page only.
  save::SaveDat empty = blank_dat();
  targets = Autosave::targets(&empty, &page);
  CHECK(!targets.card && targets.page);
  // No page (no save folder): the card rule alone.
  targets = Autosave::targets(&empty, nullptr);
  CHECK(!targets.card && !targets.page);
  std::filesystem::remove_all(dir);
}

int main() {
  test_trigger();
  test_write();
  test_page();
  test_targets();
  std::printf(g_fail ? "test_autosave: %d FAILED\n" : "test_autosave: OK\n", g_fail);
  return g_fail ? 1 : 0;
}
