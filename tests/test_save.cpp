// Save layer tests: crypto known-answer vectors, encrypted ZIP round trip, memory-card directory, slot codec.
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "save/cc_save.h"
#include "save/crypto.h"
#include "save/encrypted_zip.h"
#include "save/memcard.h"
#include "platform/paths.h"

using namespace oc;
using namespace oc::save;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

Bytes unhex(const std::string& text) {
  Bytes bytes;
  for (size_t i = 0; i + 1 < text.size(); i += 2) bytes.push_back(static_cast<u8>(std::stoul(text.substr(i, 2), nullptr, 16)));
  return bytes;
}
std::string hex(const u8* data, size_t count) {
  std::string text;
  char pair[3];
  for (size_t i = 0; i < count; ++i) {
    std::snprintf(pair, sizeof pair, "%02x", data[i]);
    text += pair;
  }
  return text;
}
ByteSpan sp(const std::string& text) { return ByteSpan(reinterpret_cast<const u8*>(text.data()), text.size()); }

void test_crypto() {
  // FIPS-197 C.3
  const Bytes key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const Bytes pt = unhex("00112233445566778899aabbccddeeff");
  crypto::Aes256 aes(key.data());
  u8 ct[16], back[16];
  aes.encrypt_block(pt.data(), ct);
  CHECK(hex(ct, 16) == "8ea2b7ca516745bfeafc49904b496089");
  aes.decrypt_block(ct, back);
  CHECK(std::memcmp(back, pt.data(), 16) == 0);

  const auto digest = crypto::sha1(sp("abc"));
  CHECK(hex(digest.data(), 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  const auto digest2 = crypto::sha1(sp("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
  CHECK(hex(digest2.data(), 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

  const Bytes hmac_key(20, 0x0b);
  // RFC 2202 case 1.
  const auto mac = crypto::hmac_sha1(hmac_key, sp("Hi There"));
  CHECK(hex(mac.data(), 20) == "b617318655057264e28bc0b6fb378c8ef146be00");

  // RFC 6070 cases 1 and 2.
  const Bytes dk = crypto::pbkdf2_hmac_sha1(sp("password"), sp("salt"), 1, 20);
  CHECK(hex(dk.data(), dk.size()) == "0c60c80f961f0e71f3a9b524af6012062fe037a6");
  const Bytes dk2 = crypto::pbkdf2_hmac_sha1(sp("password"), sp("salt"), 2, 20);
  CHECK(hex(dk2.data(), dk2.size()) == "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
}

void test_zip() {
  std::vector<ZipMember> in;
  Bytes big(5000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>((i * 7) >> 3);
  in.push_back({"a.bin", big, true});
  in.push_back({"b.bin", Bytes{1, 2, 3}, false});
  in.push_back({"empty", {}, true});
  const Bytes zip = zip_write(in, kRdeSavePassword, 42);
  // Deterministic with a seed.
  CHECK(zip == zip_write(in, kRdeSavePassword, 42));
  const auto out = zip_open(zip);
  CHECK(out.size() == 3);
  CHECK(out.size() == 3 && out[0].name == "a.bin" && out[0].data == big && out[0].deflate);
  CHECK(out.size() == 3 && out[1].data == (Bytes{1, 2, 3}) && !out[1].deflate);
  CHECK(out.size() == 3 && out[2].data.empty());
  bool threw = false;
  try {
    zip_open(zip, "wrong password");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  Bytes bad = zip;
  // Corrupt the ciphertext of the first member.
  bad[60] ^= 1;
  threw = false;
  try {
    zip_open(bad);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

void test_memcard() {
  MemCard card = MemCard::blank();
  CHECK(card.header_ok());
  CHECK(card.allocate() == 1);
  CHECK(card.find("BISLPSP02364CHRONO03") == 0);
  const int frame = card.allocate();
  card.set_dir(frame, 0x51, 0x2000, 0xFFFF, "BISLPSP02364CHRONO03");
  Bytes blk(MemCard::kBlock, 0xAB);
  card.write_block(frame, blk);
  const DirFrame dir = card.dir(frame);
  CHECK(dir.first() && dir.size == 0x2000 && dir.next == 0xFFFF && dir.xor_ok && dir.name == "BISLPSP02364CHRONO03");
  CHECK(card.find("BISLPSP02364CHRONO03") == frame);
  CHECK(card.allocate() == frame + 1);
  CHECK(card.block(frame)[5] == 0xAB);
  CHECK(list_slots(card).size() == 1 && list_slots(card)[0].slot == 3);
  card.bytes();
  Bytes raw = card.bytes();
  // Corrupting a name byte must be flagged by the XOR check.
  raw[0x80 * frame + 20] ^= 1;
  CHECK(!MemCard(raw).dir(frame).xor_ok);
  bool threw = false;
  try {
    MemCard(Bytes(100));
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

SaveSlot synthetic_slot() {
  SaveSlot slot;
  for (size_t i = 0; i < slot.head.size(); ++i) slot.head[i] = static_cast<u8>(i * 31 + 5);
  for (size_t i = 0; i < slot.tail.size(); ++i) slot.tail[i] = static_cast<u8>(i * 17 + 3);
  slot.set_party({0, 1, 10});
  slot.set_gold(7763);
  slot.set_growth_progress(0x12345);
  slot.set_playtime_frames(123456);
  slot.set_key_items(0x8000000000000001ull);
  for (int i = 0; i < kNumChars; ++i) {
    SaveChar& character = slot.chars[i];
    character.colour_bit = (i == 0 || i == 43) ? 8 : 0;
    character.rec02 = static_cast<u8>(i);
    character.rec_a8 = static_cast<u8>(i * 3);
    character.status = static_cast<u8>(i);
    character.max_hp = static_cast<u16>(100 + i * 7);
    character.cur_hp = static_cast<u16>(50 + i);
    character.hp_extra = character.max_hp;
    character.unk_be = static_cast<u16>(i & 0x3FF);
    character.atk = static_cast<u8>(i % 128); character.def = static_cast<u8>((i + 1) % 128); character.hit = static_cast<u8>((i + 2) % 128);
    character.evd = static_cast<u8>((i + 3) % 128); character.mag = static_cast<u8>((i + 4) % 128); character.mdef = static_cast<u8>((i + 5) % 128);
    for (int k = 0; k < 12; ++k) character.bonus[k] = static_cast<u8>((i + k) % 128);
    character.weapon = static_cast<u16>(i + 1); character.armour = static_cast<u16>(i + 100);
    for (int k = 0; k < 3; ++k) character.accessory[k] = static_cast<u16>(200 + i + k);
    character.model_id = static_cast<u16>(i % 512);
    for (int k = 0; k < 48; ++k) character.grid[k] = static_cast<u16>((i * 11 + k) % 512);
    for (int k = 0; k < 8; ++k) character.unlocked[k] = static_cast<u16>((i * 5 + k * 3) % 1024);
    for (int k = 0; k < 48; ++k) character.equipped_qty[k] = static_cast<u8>((i + k) % 8);
    // name: "セルジュ"-like game-font codes F827 F842 F84E F868, then NUL
    const u8 nm[] = {0xF8, 0x27, 0xF8, 0x42, 0xF8, 0x4E, 0xF8, 0x68, 0x75, 0x76, 0, 0};
    std::memcpy(character.name.data(), nm, sizeof nm);
  }
  for (int k = 0; k < kNumItems; ++k) slot.items[k] = {static_cast<u16>(k % 512), static_cast<u8>(k % 100)};
  for (int k = 0; k < kNumElements; ++k) slot.elements[k] = {static_cast<u16>((k * 3) % 512), static_cast<u8>(k % 100)};
  slot.button_cfg = {0x76543210, 0x76543211, 0x3210, 0x3211};
  slot.unk_e1b8 = {1, 2, 3, 4};
  slot.unk_e1e0 = 0xDEADBEEF; slot.unk_e1e4 = 5; slot.unk_e1ec = 6; slot.unk_e1f0 = 7;
  slot.frames = 0x203;
  // The fields encode_block stamps for RDE's loader (version 0x106 at 0x1FF8, summary bit count 210 at 0x24, "CRD0" marker at 0x50).
  slot.head[0x24] = 210;
  slot.head[0x25] = slot.head[0x26] = slot.head[0x27] = 0;
  const u8 crd[6] = {0x01, 0x00, 'C', 'R', 'D', '0'};
  std::memcpy(&slot.head[0x50], crd, 6);
  slot.tail[kVersionOffset - (kHeadSize + kPackedSize)] = 0x06;
  slot.tail[kVersionOffset - (kHeadSize + kPackedSize) + 1] = 0x01;
  slot.tail[kVersionOffset - (kHeadSize + kPackedSize) + 2] = slot.tail[kVersionOffset - (kHeadSize + kPackedSize) + 3] = 0;
  return slot;
}

void test_slot() {
  const SaveSlot slot = synthetic_slot();
  const Bytes blk = encode_block(slot);
  CHECK(blk.size() == kBlockSize);
  CHECK(block_checksum_ok(blk));
  const SaveSlot decoded = decode_block(blk);
  CHECK(decoded.head == slot.head && decoded.tail == slot.tail);
  CHECK(decoded.chars[0].max_hp == slot.chars[0].max_hp && decoded.chars[43].grid == slot.chars[43].grid);
  CHECK(decoded.chars[7].name == slot.chars[7].name && decoded.chars[43].colour_bit == 8);
  CHECK(decoded.items[300].id == slot.items[300].id && decoded.elements[191].qty == slot.elements[191].qty);
  CHECK(decoded.button_cfg == slot.button_cfg && decoded.frames == 0x203 && decoded.unk_e1e0 == 0xDEADBEEF);
  CHECK(decoded.growth_progress() == 0x12345);
  CHECK(decoded.gold() == 7763 && decoded.playtime_frames() == 123456 && decoded.key_items() == 0x8000000000000001ull);
  // Decode -> encode is bit exact.
  CHECK(encode_block(decoded) == blk);
  // image-level inverse
  const Bytes packed(blk.begin() + kHeadSize, blk.begin() + kHeadSize + kPackedSize);
  CHECK(pack_image(unpack_image(packed)) == packed);

  // through a memory card
  MemCard card = MemCard::blank();
  CHECK(write_slot(card, 4, slot) == 1);
  // The second write replaces the first.
  CHECK(write_slot(card, 4, slot) == 1);
  CHECK(write_slot(card, 0, slot) == 2);
  const auto slots = list_slots(card);
  CHECK(slots.size() == 2 && slots[0].slot == 0 && slots[1].slot == 4);
  CHECK(read_slot(card, slots[1].block).chars[5].weapon == slot.chars[5].weapon);
  // A US-named file of the same number is rewritten in place under its own name.
  MemCard us_card = MemCard::blank();
  const int us_block = us_card.allocate();
  us_card.write_block(us_block, encode_block(slot));
  us_card.set_dir(us_block, 0x51, MemCard::kBlock, 0xFFFF, "BASLUSP01041USCHRO07");
  CHECK(write_slot(us_card, 7, slot) == us_block);
  CHECK(list_slots(us_card).size() == 1 && list_slots(us_card)[0].name == "BASLUSP01041USCHRO07");
  // Values wider than their stream field saturate instead of wrapping.
  SaveSlot wide = slot;
  wide.items[3].qty = 130;
  wide.chars[2].max_hp = 1500;
  wide.chars[2].weapon = 600;
  const SaveSlot narrowed = decode_block(encode_block(wide));
  CHECK(narrowed.items[3].qty == 127 && narrowed.chars[2].max_hp == 1023 && narrowed.chars[2].weapon == 511);
  // The summary's faces follow the model byte up to 0x31: Serge as Lynx (0x31) shows Lynx, Glenn with two swords (0x32) his own face.
  SaveSlot forms = slot;
  forms.set_party({0, 0x13, 0xFF});
  forms.chars[0].model_id = 0x31;
  forms.chars[0x13].model_id = 0x32;
  CHECK(write_slot(card, 5, forms) == 3);
  const SaveSlot written = read_slot(card, 3);
  CHECK(written.preview_party() == (std::array<u8, 3>{0x31, 0x13, 0xFF}));
  CHECK(written.party() == (std::array<u8, 3>{0, 0x13, 0xFF}));

  // whole save.dat container
  std::vector<ZipMember> members;
  members.push_back({"memcard1.mcr", card.bytes(), true});
  members.push_back({"memcard2.mcr", MemCard::blank().bytes(), true});
  members.push_back({"ext_save.dat", Bytes{'E', 'X', 'T', 0}, true});
  const SaveDat sd = SaveDat::open(zip_write(members, kRdeSavePassword, 7));
  CHECK(sd.card(1) && sd.card(2) && sd.card(1)->bytes() == card.bytes());
  const SaveDat sd2 = SaveDat::open(sd.serialize(9));
  CHECK(sd2.members().size() == 3 && sd2.member("ext_save.dat") && sd2.member("ext_save.dat")->data.size() == 4);
}

// ext_save.dat: format, RDE's loader rules, SaveDat write / read.
// The loader's checksum and version gates: an older block (0x102) is summed without the header frame and lacks the last steps, which
// take their defaults; a block with one byte changed in the summed range is refused (corrupted) but still decodes for its preview.
void restamp(Bytes& blk, u32 version) {
  for (int k = 0; k < 4; ++k) blk[kVersionOffset + k] = static_cast<u8>(version >> (8 * k));
  u32 sum = 0;
  for (size_t i = version > 0x102 ? 0 : 0x180; i < kVersionOffset; ++i) sum += blk[i];
  for (int k = 0; k < 4; ++k) blk[kChecksumOffset + k] = static_cast<u8>(sum >> (8 * k));
}

void test_version_gates() {
  SaveSlot slot = synthetic_slot();
  slot.chars[0].colour_bit = slot.chars[43].colour_bit = 3;
  slot.frames = 0x3FF;
  slot.unk_e1ec = 0x11;
  slot.unk_e1f0 = 0x22;
  const Bytes current = encode_block(slot);
  CHECK(block_version(current) == kSaveVersion && !decode_block(current).corrupted);

  Bytes old = current;
  restamp(old, 0x102);
  CHECK(block_checksum_ok(old));
  const SaveSlot v102 = decode_block(old);
  CHECK(!v102.corrupted);
  CHECK(v102.frames == 0xFF && v102.chars[0].colour_bit == 8 && v102.chars[43].colour_bit == 8);
  CHECK(v102.unk_e1ec == 0 && v102.unk_e1f0 == 0);
  CHECK(v102.unk_e1e0 == slot.unk_e1e0 && v102.unk_e1e4 == slot.unk_e1e4 && v102.chars[9].rec_a8 == slot.chars[9].rec_a8);
  CHECK(v102.chars[12].grid == slot.chars[12].grid && v102.items[300].id == slot.items[300].id);
  Bytes old_header = old;
  old_header[0x10] ^= 0x40;
  CHECK(block_checksum_ok(old_header));

  Bytes v100 = current;
  restamp(v100, 0x100);
  const SaveSlot first = decode_block(v100);
  CHECK(!first.corrupted && first.unk_e1e0 == 0 && first.unk_e1e4 == 0 && first.chars[9].rec_a8 == 0 && first.frames == 0xFF);
  Bytes v105 = current;
  restamp(v105, 0x105);
  const SaveSlot fifth = decode_block(v105);
  CHECK(fifth.frames == 0x3FF && fifth.chars[0].colour_bit == 3 && fifth.unk_e1ec == 0 && fifth.unk_e1f0 == 0);

  Bytes summed_without_header = current;
  restamp(summed_without_header, 0x102);
  for (int k = 0; k < 4; ++k) summed_without_header[kVersionOffset + k] = static_cast<u8>(kSaveVersion >> (8 * k));
  CHECK(!block_checksum_ok(summed_without_header));

  Bytes flipped = current;
  flipped[0x1000] ^= 0x01;
  CHECK(!block_checksum_ok(flipped));
  const SaveSlot refused = decode_block(flipped);
  CHECK(refused.corrupted && refused.head == slot.head && refused.party() == slot.party());
  Bytes head_flipped = current;
  head_flipped[0x10] ^= 0x40;
  CHECK(decode_block(head_flipped).corrupted);
}

void test_ext() {
  ExtFile ext;
  CHECK(ext.bytes().size() == kExtSize && std::memcmp(ext.bytes().data(), "EXT", 4) == 0 && ext.bytes()[4] == 0x04 && ext.bytes()[5] == 0x01);
  ExtSlot slot;
  slot.language = 3;
  slot.battles = 0x1234;
  slot.names[0] = U"Zoe";
  // Accents + a 3-byte code point; 2-byte chars: 15 fit in 31 bytes.
  slot.names[43] = U"Émilie€";
  slot.names[7] = std::u32string(40, U'é');
  slot.names[99] = U"dropped";
  slot.form = 4;
  slot.valid = 1;
  ext.set_slot(17, slot);
  const Bytes& bytes = ext.bytes();
  const size_t base = 8 + 17 * 0x588;
  CHECK(bytes[base] == 3 && bytes[base + 1] == 0 && bytes[base + 4] == 0x34 && bytes[base + 5] == 0x12);
  CHECK(std::memcmp(&bytes[base + 8], "Zoe\0", 4) == 0 && bytes[base + 8 + 3] == 0);
  // UTF-8 "E acute".
  CHECK(bytes[base + 8 + 43 * 32] == 0xC3 && bytes[base + 8 + 43 * 32 + 1] == 0x89);
  CHECK(bytes[0xA5F8 + 34] == 4 && bytes[0xA5F8 + 35] == 1);
  const ExtSlot back = ext.slot(17);
  CHECK(back.language == 3 && back.battles == 0x1234 && back.names.size() == 3);
  CHECK(back.names.at(0) == U"Zoe" && back.names.at(43) == U"Émilie€" && back.names.at(7).size() == 15);
  CHECK(back.form == 4 && back.valid == 1);
  CHECK(ext.slot(16).names.empty() && ext.slot(18).language == 0 && ext.slot(30).names.empty());
  // set_slot clears the old names first (the game memsets the 0x580-byte area)
  ExtSlot replacement;
  replacement.names[1] = U"B";
  ext.set_slot(17, replacement);
  CHECK(ext.slot(17).names.size() == 1 && ext.slot(17).names.count(1) && ext.slot(17).battles == 0);
  // parse: full files round trip, 0xA5F8 / 0x103 is upgraded, other sizes / versions / magics give the defaults
  CHECK(ExtFile::parse(ext.bytes()).bytes() == ext.bytes());
  Bytes old(ext.bytes().begin(), ext.bytes().begin() + kExtLegacySize);
  old[4] = 0x03;
  const ExtFile up = ExtFile::parse(old);
  CHECK(up.bytes().size() == kExtSize && up.bytes()[4] == 0x04 && up.slot(17).names.count(1) && up.bytes()[0xA5F8] == 0);
  Bytes bad = ext.bytes();
  bad[4] = 0x05;
  CHECK(ExtFile::parse(bad).bytes() == ExtFile().bytes());
  bad = ext.bytes();
  bad[0] = 'X';
  CHECK(ExtFile::parse(bad).bytes() == ExtFile().bytes());
  CHECK(ExtFile::parse(Bytes{'E', 'X', 'T', 0}).bytes() == ExtFile().bytes());
  // utf8 helpers
  CHECK(utf8_to_utf32(utf32_to_utf8(U"aéあ\U0001F600")) == U"aéあ\U0001F600");
  CHECK(utf8_to_utf32(std::string("a\xFF")) == U"a�");

  // A hand-built RDE file (real layout: the pair (3, 1) in slot 0, a battle counter of 4, a renamed Serge in card 1 slot 2)
  Bytes rde(kExtSize, 0);
  std::memcpy(rde.data(), "EXT", 4);
  rde[4] = 0x04;
  rde[5] = 0x01;
  rde[0xA5F8] = 3;
  rde[0xA5F9] = 1;
  rde[8 + 4] = 4;
  const size_t serge_row = 8 + 2 * 0x588;
  // Language 1 = French.
  rde[serge_row] = 1;
  std::memcpy(&rde[serge_row + 8], "Serge2", 6);
  rde[0xA5F8 + 4] = 5;
  rde[0xA5F8 + 5] = 1;

  // SaveDat: no host = nothing written; with a host the slot record is stored and the others are kept byte for byte
  std::vector<ZipMember> members;
  members.push_back({"memcard1.mcr", MemCard::blank().bytes(), true});
  members.push_back({"memcard2.mcr", MemCard::blank().bytes(), true});
  members.push_back({"achievement.dat", Bytes{'A', 'C', 'H', 0}, true});
  members.push_back({"ext_save.dat", rde, true});
  SaveDat dat = SaveDat::open(zip_write(members, kRdeSavePassword, 3));
  CHECK(!dat.write_ext(1, 4));
  std::map<int, std::u32string> names{{0, U"Lena"}, {5, U"Vip"}};
  ExtHost host;
  host.language = 1;
  host.battles = 9;
  host.names = &names;
  dat.set_ext_host(&host);
  CHECK(!dat.write_ext(3, 0) && !dat.write_ext(1, 15));
  CHECK(dat.write_ext(1, 4));
  CHECK(dat.write_ext(2, 14));
  SaveDat reopened = SaveDat::open(dat.serialize(5));
  CHECK(reopened.members().size() == 4 && reopened.member("achievement.dat"));
  const Bytes& nb = reopened.member("ext_save.dat")->data;
  CHECK(nb.size() == kExtSize);
  // untouched records are identical to the RDE file
  for (int i : {0, 1, 2, 3, 5, 20, 28}) {
    CHECK(std::equal(nb.begin() + 8 + i * 0x588, nb.begin() + 8 + (i + 1) * 0x588, rde.begin() + 8 + i * 0x588));
    CHECK(nb[0xA5F8 + 2 * i] == rde[0xA5F8 + 2 * i] && nb[0xA5F8 + 2 * i + 1] == rde[0xA5F8 + 2 * i + 1]);
  }
  CHECK(nb[0xA5F8] == 3 && nb[0xA5F9] == 1);
  const ExtSlot written = reopened.ext().slot(4);
  CHECK(written.language == 1 && written.battles == 9 && written.names == names && written.form == 0 && written.valid == 1);
  // Slot 29 is card 2 slot 14.
  CHECK(reopened.ext().slot(29).names == names);
  // read: names only for the same language; the counter always replaces the host's
  host.battles = 0;
  reopened.set_ext_host(&host);
  // OC_TEST_FORGE_SAVE writes the forged save.dat (hand-built RDE ext_save + our records) to that path.
  if (const char* forge = std::getenv("OC_TEST_FORGE_SAVE")) {
    const Bytes fb = reopened.serialize(11);
    if (std::FILE* ff = std::fopen(forge, "wb")) {
      std::fwrite(fb.data(), 1, fb.size(), ff);
      std::fclose(ff);
    }
  }
  const ExtSlot read_back = reopened.read_ext(1, 4, 1);
  CHECK(read_back.names == names && host.battles == 9);
  const ExtSlot read_first = reopened.read_ext(1, 0, 0);
  CHECK(read_first.names.empty() && host.battles == 4 && read_first.form == 3 && read_first.valid == 1);
  // A language mismatch gives defaults.
  CHECK(reopened.read_ext(1, 4, 0).names.empty());
  CHECK(reopened.read_ext(1, 2, 1).names.at(0) == U"Serge2");
  // a save.dat without the member: write_ext appends it
  std::vector<ZipMember> cards_only(members.begin(), members.begin() + 3);
  SaveDat no_ext = SaveDat::open(zip_write(cards_only, kRdeSavePassword, 4));
  CHECK(!no_ext.member("ext_save.dat") && no_ext.read_ext(1, 0, 0).battles == 0);
  no_ext.set_ext_host(&host);
  CHECK(no_ext.write_ext(1, 0));
  CHECK(no_ext.member("ext_save.dat") && no_ext.member("ext_save.dat")->data.size() == kExtSize);
}
// RDE's first-boot save.dat: the formatted card, memcard1 + memcard2 in that order, the
// archive entries as libzip writes them in a real RDE save.dat, and a slot written into it reads back.
u32 rd16(const Bytes& bytes, size_t offset) { return bytes[offset] | (bytes[offset + 1] << 8); }
u32 rd32(const Bytes& bytes, size_t offset) { return rd16(bytes, offset) | (rd16(bytes, offset + 2) << 16); }

void test_create_empty() {
  // The formatted card, byte for byte: "MC" + XOR 0x0E; 15 frames {u32 0xA0, next 0xFFFF, XOR 0xA0}; 20 frames {0xFFFFFFFF, next 0xFFFF, XOR 0}.
  Bytes rde(MemCard::kSize, 0);
  rde[0] = 'M', rde[1] = 'C', rde[0x7F] = 0x0E;
  for (int frame = 1; frame <= 15; ++frame) rde[frame * 0x80] = 0xA0, rde[frame * 0x80 + 8] = rde[frame * 0x80 + 9] = 0xFF, rde[frame * 0x80 + 0x7F] = 0xA0;
  for (int frame = 16; frame < 36; ++frame)
    for (int k : {0, 1, 2, 3, 8, 9}) rde[frame * 0x80 + k] = 0xFF;
  CHECK(MemCard::blank().bytes() == rde);

  SaveDat dat = SaveDat::create_empty();
  CHECK(dat.members().size() == 2 && dat.members()[0].name == "memcard1.mcr" && dat.members()[1].name == "memcard2.mcr");
  CHECK(dat.card(1) && dat.card(2) && dat.card(1)->bytes() == rde && dat.card(2)->bytes() == rde);
  CHECK(!dat.ensure_cards());

  const Bytes zip = dat.serialize(11);
  // Archive structure = RDE's (tmp\save.dat): every central entry made-by 0x33F, needed 20, flags 3, method 99, real CRC-32 (>= 20 bytes),
  // external attributes 0x81B60000, extra 0x9901 {7, AE-2, "AE", strength 3, method 8}; local headers agree.
  const size_t eocd = zip.size() - 22;
  CHECK(rd32(zip, eocd) == 0x06054B50 && rd16(zip, eocd + 10) == 2);
  size_t entry = rd32(zip, eocd + 16);
  const Bytes extra = unhex("0199070002004145030800");
  for (int i = 0; i < 2; ++i) {
    CHECK(rd32(zip, entry) == 0x02014B50 && rd16(zip, entry + 4) == 0x33F && rd16(zip, entry + 6) == 20 && rd16(zip, entry + 8) == 3 && rd16(zip, entry + 10) == 99);
    CHECK(rd32(zip, entry + 16) != 0 && rd32(zip, entry + 24) == MemCard::kSize && rd32(zip, entry + 38) == 0x81B60000u);
    const u32 nl = rd16(zip, entry + 28), el = rd16(zip, entry + 30);
    CHECK(Bytes(zip.begin() + static_cast<long>(entry + 46 + nl), zip.begin() + static_cast<long>(entry + 46 + nl + el)) == extra);
    const size_t lho = rd32(zip, entry + 42);
    CHECK(rd32(zip, lho) == 0x04034B50 && rd16(zip, lho + 4) == 20 && rd16(zip, lho + 6) == 3 && rd16(zip, lho + 8) == 99 &&
          rd32(zip, lho + 14) == rd32(zip, entry + 16));
    entry += 46 + nl + el + rd16(zip, entry + 32);
  }

  // round trip: open -> write a slot (+ its ext_save record, the first save adds the member) -> serialize -> open -> the slot reads back
  SaveDat opened = SaveDat::open(zip);
  CHECK(opened.card(1) && opened.card(2) && list_slots(*opened.card(1)).empty() && list_slots(*opened.card(2)).empty());
  ExtHost host;
  host.language = 0;
  host.battles = 3;
  opened.set_ext_host(&host);
  const SaveSlot slot = synthetic_slot();
  CHECK(write_slot(*opened.card(1), 1, slot) == 1);
  CHECK(opened.write_ext(1, 1));
  const SaveDat round_trip = SaveDat::open(opened.serialize(12));
  CHECK(round_trip.members().size() == 3 && round_trip.members()[2].name == "ext_save.dat" && round_trip.member("ext_save.dat")->data.size() == kExtSize);
  const auto sl = list_slots(*round_trip.card(1));
  CHECK(sl.size() == 1 && sl[0].slot == 1 && sl[0].name == "BISLPSP02364CHRONO01");
  CHECK(block_checksum_ok(round_trip.card(1)->block(sl[0].block)));
  const SaveSlot loaded = read_slot(*round_trip.card(1), sl[0].block);
  CHECK(loaded.gold() == slot.gold() && loaded.party() == slot.party() && loaded.chars[7].weapon == slot.chars[7].weapon && loaded.items[9].id == slot.items[9].id);
  CHECK(round_trip.read_ext(1, 1, 0).battles == 3);

  // RDE's boot rule on an existing file: a card member of the wrong size is reformatted in place, a missing one appended
  std::vector<ZipMember> members;
  members.push_back({"memcard1.mcr", Bytes(100, 1), true});
  members.push_back({"rd_save.dat", Bytes(1024, 0x60), true});
  SaveDat repaired = SaveDat::open(zip_write(members, kRdeSavePassword, 13));
  CHECK(!repaired.card(1) && !repaired.card(2));
  CHECK(repaired.ensure_cards());
  CHECK(repaired.members().size() == 3 && repaired.members()[0].name == "memcard1.mcr" && repaired.members()[0].data == rde && repaired.members()[1].name == "rd_save.dat" &&
        repaired.members()[2].name == "memcard2.mcr");
  CHECK(!repaired.ensure_cards());
}

// The Steam user for a missing save folder (platform/paths.h): loginusers.vdf's MostRecent entry.
void test_loginusers() {
  const char* vdf =
      "\"users\"\n{\n\t\"76561198000000001\"\n\t{\n\t\t\"AccountName\"\t\t\"a\"\n\t\t\"MostRecent\"\t\t\"0\"\n\t}\n"
      "\t\"76561198037572628\"\n\t{\n\t\t\"AccountName\"\t\t\"b\"\n\t\t\"mostrecent\"\t\t\"1\"\n\t}\n}\n";
  CHECK(platform::parse_loginusers_most_recent(vdf) == 76561198037572628ull);
  CHECK(platform::parse_loginusers_most_recent("\"users\" { }") == 0);
  CHECK(platform::kSteamId64Base + 77306900u == 76561198037572628ull);
}
}  // namespace

int main() try {
  test_create_empty();
  test_loginusers();
  test_crypto();
  test_zip();
  test_memcard();
  test_slot();
  test_version_gates();
  test_ext();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_save: ok\n");
  return 0;
} catch (const std::exception& error) {
  std::printf("FAIL: uncaught exception: %s\n", error.what());
  return 1;
}
