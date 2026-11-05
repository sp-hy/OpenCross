#include "save/cc_save.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace oc::save {

namespace {

// ----------------------------------------------------------------- little-endian helpers
u32 rd(const u8* bytes, int size) {
  u32 value = 0;
  for (int i = 0; i < size; ++i) value |= u32(bytes[i]) << (8 * i);
  return value;
}
void wr(u8* bytes, int size, u32 value) {
  for (int i = 0; i < size; ++i) bytes[i] = static_cast<u8>(value >> (8 * i));
}
u16 rd16(const u8* bytes) { return static_cast<u16>(rd(bytes, 2)); }

// ----------------------------------------------------------------- the 54-step section table
// mode: 0 plain values, 1 nibbles, 2 grid cells, 3 level masks, 4 names. length = bits per value, off = image offset,
// special = stride in bytes for mode 0.
struct Step {
  u8 mode;
  u8 length;
  u16 count;
  u16 off;
  u16 special;
};

constexpr u16 kChars = kNumChars;
// Character record stride.
constexpr u16 kStride = 0xCC;

constexpr Step kSteps[] = {
    {0, 9, 0x180, 0xD13C, 2},  {0, 7, 0x180, 0xD43C, 1}, {0, 9, 0xC0, 0xD5BC, 2},  {0, 7, 0xC0, 0xD73C, 1},
    {1, 3, kChars, 0xDD98, 0x30},   {0, 8, kChars, 0xAAD4, kStride},      {0, 8, kChars, 0xAA26, kStride},      {2, 9, kChars, 0xAA64, 2},
    {0, 7, kChars, 0xAA3C, kStride},      {0, 7, kChars, 0xAAE4, kStride},      {0, 7, kChars, 0xAAE5, kStride},      {0, 7, kChars, 0xAA3E, kStride},
    {0, 7, kChars, 0xAAE6, kStride},      {0, 7, kChars, 0xAAE7, kStride},      {0, 7, kChars, 0xAA3D, kStride},      {0, 7, kChars, 0xAAE8, kStride},
    {0, 7, kChars, 0xAAE9, kStride},      {0, 7, kChars, 0xAA3F, kStride},      {0, 7, kChars, 0xAAEA, kStride},      {0, 7, kChars, 0xAAEB, kStride},
    {0, 7, kChars, 0xAA40, kStride},      {0, 7, kChars, 0xAAEC, kStride},      {0, 7, kChars, 0xAAED, kStride},      {0, 7, kChars, 0xAA41, kStride},
    {3, 10, kChars, 0xDAD8, 0},     {0, 10, kChars, 0xAADC, kStride},     {0, 10, kChars, 0xAA3A, kStride},     {0, 10, kChars, 0xAAE0, kStride},
    {0, 9, kChars, 0xAA44, kStride},      {0, 9, kChars, 0xAA46, kStride},      {0, 9, kChars, 0xAA48, kStride},      {0, 9, kChars, 0xAA4A, kStride},
    {0, 9, kChars, 0xAA4C, kStride},      {0, 9, kChars, 0xAA51, kStride},      {0, 10, kChars, 0xAAE2, kStride},     {0, 7, kChars, 0xAAEE, kStride},
    {0, 7, kChars, 0xAAEF, kStride},      {4, 11, kChars, 0xD80C, 0},     {0, 32, 1, 0xE1B8, 1},     {0, 32, 1, 0xE1BC, 1},
    {0, 32, 1, 0xE1C0, 1},     {0, 32, 1, 0xE1C4, 1},     {0, 32, 1, 0x0000, 1},     {0, 32, 1, 0x0004, 1},
    {0, 32, 1, 0x0008, 1},     {0, 32, 1, 0x000C, 1},     {0, 32, 1, 0xE1E0, 1},     {0, 32, 1, 0xE1E4, 1},
    {0, 8, kChars, 0xAACC, kStride},      {0, 16, 1, 0xE1E8, 1},     {0, 8, 1, 0xAA25, 1},      {0, 8, 1, 0xCC69, 1},
    {0, 32, 1, 0xE1EC, 1},     {0, 32, 1, 0xE1F0, 1},
};
static_assert(sizeof(kSteps) / sizeof(kSteps[0]) == 54);

// Version gates: steps 46-47 need a version above 0x100, step 48 above 0x101, step 49 0x104, steps 50-51 0x105, steps 52-53 0x106.
bool step_in_version(size_t step, u32 version) {
  if (step >= 52) return version >= 0x106;
  if (step >= 50) return version >= 0x105;
  if (step == 49) return version >= 0x104;
  if (step == 48) return version > 0x101;
  if (step >= 46) return version > 0x100;
  return true;
}

int value_bytes(int length) { return length == 32 ? 4 : length > 8 ? 2 : 1; }

// LSB-first bit stream.
struct BitReader {
  ByteSpan data;
  size_t pos = 0;
  u32 get(int bit_count) {
    u32 value = 0;
    for (int k = 0; k < bit_count; ++k, ++pos) {
      if (pos >> 3 >= data.size()) throw std::runtime_error("save: packed stream overrun");
      value |= u32((data[pos >> 3] >> (pos & 7)) & 1) << k;
    }
    return value;
  }
};
struct BitWriter {
  Bytes data;
  size_t pos = 0;
  explicit BitWriter(size_t bytes) : data(bytes, 0) {}
  void put(u32 value, int bit_count) {
    for (int k = 0; k < bit_count; ++k, ++pos) {
      if (pos >> 3 >= data.size()) throw std::runtime_error("save: packed stream overflow");
      if ((value >> k) & 1) data[pos >> 3] |= static_cast<u8>(1 << (pos & 7));
    }
  }
};

// Image char record base.
constexpr size_t rec(int i) { return 0xAA24 + size_t(i) * kStride; }

}  // namespace

// ----------------------------------------------------------------- SaveSlot accessors
std::array<u8, 3> SaveSlot::party() const { return {head[head::kParty], head[head::kParty + 1], head[head::kParty + 2]}; }
void SaveSlot::set_party(const std::array<u8, 3>& ids) {
  for (int i = 0; i < 3; ++i) head[head::kParty + i] = head[head::kPreviewParty + i] = ids[i];
}
u64 SaveSlot::key_items() const { return u64(rd(&head[head::kKeyItems], 4)) | (u64(rd(&head[head::kKeyItems + 4], 4)) << 32); }
void SaveSlot::set_key_items(u64 bits) {
  wr(&head[head::kKeyItems], 4, static_cast<u32>(bits));
  wr(&head[head::kKeyItems + 4], 4, static_cast<u32>(bits >> 32));
}
u32 SaveSlot::playtime_frames() const { return rd(&head[head::kPlaytime], 4); }
void SaveSlot::set_playtime_frames(u32 frame_count) {
  wr(&head[head::kPlaytime], 4, frame_count);
  wr(&head[head::kPreviewPlaytime], 4, frame_count);
}
u32 SaveSlot::gold() const { return rd(&head[head::kGold], 4); }
void SaveSlot::set_gold(u32 amount) {
  wr(&head[head::kGold], 4, amount);
  wr(&head[head::kPreviewGold], 4, amount);
}
u32 SaveSlot::growth_progress() const { return rd(&head[head::kGrowthProgress], 4); }
void SaveSlot::set_growth_progress(u32 value) { wr(&head[head::kGrowthProgress], 4, value); }
bool SaveSlot::recruited(int character) const { return character >= 0 && character < kNumChars && ((head[head::kRecruited + character / 8] >> (character % 8)) & 1); }
void SaveSlot::set_recruited(int character, bool on) {
  if (character < 0 || character >= kNumChars) return;
  u8& flags = head[head::kRecruited + character / 8];
  flags = static_cast<u8>(on ? (flags | (1 << (character % 8))) : (flags & ~(1 << (character % 8))));
}
u16 SaveSlot::location() const { return rd16(&head[head::kLocation]); }

// ----------------------------------------------------------------- checksum
u32 block_sum(ByteSpan block) {
  u32 sum = 0;
  for (size_t i = 0; i < 0x1FF8 && i < block.size(); ++i) sum += block[i];
  return sum;
}
u32 block_version(ByteSpan block) { return block.size() == kBlockSize ? rd(&block[kVersionOffset], 4) : 0; }
bool block_checksum_ok(ByteSpan block) {
  if (block.size() != kBlockSize) return false;
  u32 sum = 0;
  for (size_t i = block_version(block) > 0x102 ? 0 : 0x180; i < kVersionOffset; ++i) sum += block[i];
  return rd(&block[kChecksumOffset], 4) == sum;
}

// ----------------------------------------------------------------- section stream codec
Bytes unpack_image(ByteSpan packed, u32 version) {
  if (packed.size() != kPackedSize) throw std::runtime_error("save: packed stream must be 0x1975 bytes");
  BitReader br{packed};
  Bytes img(kImageSize, 0);
  // Defaults of the gated steps: obtained-frames mask 0xFF, the two innate colours 8, everything else 0.
  if (version < 0x104) wr(&img[kSteps[49].off], 2, 0xFF);
  if (version < 0x105) img[kSteps[50].off] = img[kSteps[51].off] = 8;
  for (size_t index = 0; index < std::size(kSteps); ++index) {
    if (!step_in_version(index, version)) continue;
    const Step& step = kSteps[index];
    switch (step.mode) {
      case 0: {
        const int sz = value_bytes(step.length);
        for (int i = 0; i < step.count; ++i) wr(&img[step.off + size_t(i) * step.special], sz, br.get(step.length));
        break;
      }
      case 1:
        for (int character = 0; character < step.count; ++character)
          for (int j = 0; j < step.special; ++j) {
            const u32 value = br.get(step.length);
            u8& cell = img[step.off + size_t(character) * 0x18 + (j >> 1)];
            cell = static_cast<u8>(cell | ((j & 1) ? value << 4 : value));
          }
        break;
      case 2:
        for (int character = 0; character < step.count; ++character)
          for (int k = 0; k < 0x30; ++k) wr(&img[step.off + size_t(character) * kStride + k * step.special], 2, br.get(step.length));
        break;
      case 3:
        for (int character = 0; character < step.count; ++character)
          for (int k = 0; k < 8; ++k) wr(&img[step.off + size_t(character) * 0x10 + k * 2], 2, br.get(step.length));
        break;
      case 4:
        for (int character = 0; character < step.count; ++character) {
          size_t offset = step.off + size_t(character) * 0x10;
          for (int k = 0; k < 6; ++k) {
            const u32 glyph = br.get(step.length);
            if (glyph & 0x700) img[offset++] = static_cast<u8>(((glyph & 0x700) >> 8) + 0xF7);
            img[offset++] = static_cast<u8>(glyph);
          }
          img[offset] = 0;
        }
        break;
    }
  }
  return img;
}

Bytes pack_image(ByteSpan img) {
  if (img.size() != kImageSize) throw std::runtime_error("save: image must be 0xE1F4 bytes");
  BitWriter bw(kPackedSize);
  auto mask = [](u32 value, int bit_count) { return bit_count >= 32 ? value : (value & ((1u << bit_count) - 1)); };
  for (const Step& step : kSteps) {
    switch (step.mode) {
      case 0: {
        const int sz = value_bytes(step.length);
        for (int i = 0; i < step.count; ++i) bw.put(mask(rd(&img[step.off + size_t(i) * step.special], sz), step.length), step.length);
        break;
      }
      case 1:
        for (int character = 0; character < step.count; ++character)
          for (int j = 0; j < step.special; ++j) {
            const u8 cell = img[step.off + size_t(character) * 0x18 + (j >> 1)];
            bw.put((j & 1) ? (cell >> 4) : (cell & 15), step.length);
          }
        break;
      case 2:
        for (int character = 0; character < step.count; ++character)
          for (int k = 0; k < 0x30; ++k) bw.put(mask(rd(&img[step.off + size_t(character) * kStride + k * step.special], 2), step.length), step.length);
        break;
      case 3:
        for (int character = 0; character < step.count; ++character)
          for (int k = 0; k < 8; ++k) bw.put(mask(rd(&img[step.off + size_t(character) * 0x10 + k * 2], 2), step.length), step.length);
        break;
      case 4:
        for (int character = 0; character < step.count; ++character) {
          size_t offset = step.off + size_t(character) * 0x10;
          for (int k = 0; k < 6; ++k) {
            u32 glyph = img[offset++];
            if (glyph > 0xF7 && offset < step.off + size_t(character) * 0x10 + 16) glyph = ((glyph - 0xF7) << 8) | img[offset++];
            bw.put(mask(glyph, step.length), step.length);
          }
        }
        break;
    }
  }
  return std::move(bw.data);
}

// ----------------------------------------------------------------- image <-> typed structs
namespace {

void image_to_slot(const Bytes& img, SaveSlot& slot) {
  const u8* image = img.data();
  for (int i = 0; i < kNumChars; ++i) {
    SaveChar& character = slot.chars[i];
    const u8* record = image + rec(i);
    character.colour_bit = (i == 0 || i == 43) ? record[1] : 0;
    character.rec02 = record[2];
    character.rec_a8 = record[0xA8];
    character.status = record[0xB0];
    character.max_hp = rd16(record + 0x16);
    character.cur_hp = rd16(record + 0xB8);
    character.hp_extra = rd16(record + 0xBC);
    character.unk_be = rd16(record + 0xBE);
    character.atk = record[0x18]; character.def = record[0x19]; character.hit = record[0x1A]; character.evd = record[0x1B]; character.mag = record[0x1C]; character.mdef = record[0x1D];
    std::copy_n(record + 0xC0, 12, character.bonus.begin());
    character.weapon = rd16(record + 0x20);
    character.armour = rd16(record + 0x22);
    for (int k = 0; k < 3; ++k) character.accessory[k] = rd16(record + 0x24 + 2 * k);
    character.model_id = rd16(record + 0x2D);
    for (int k = 0; k < 48; ++k) character.grid[k] = rd16(record + 0x40 + 2 * k);
    for (int k = 0; k < 8; ++k) character.unlocked[k] = rd16(image + 0xDAD8 + i * 0x10 + 2 * k);
    for (int k = 0; k < 48; ++k) character.equipped_qty[k] = (image[0xDD98 + i * 0x18 + (k >> 1)] >> (4 * (k & 1))) & 15;
    std::copy_n(image + 0xD80C + i * 0x10, 16, character.name.begin());
  }
  for (int k = 0; k < kNumItems; ++k) slot.items[k] = {rd16(image + 0xD13C + 2 * k), image[0xD43C + k]};
  for (int k = 0; k < kNumElements; ++k) slot.elements[k] = {rd16(image + 0xD5BC + 2 * k), image[0xD73C + k]};
  for (int k = 0; k < 4; ++k) slot.button_cfg[k] = rd(image + 4 * k, 4);
  for (int k = 0; k < 4; ++k) slot.unk_e1b8[k] = rd(image + 0xE1B8 + 4 * k, 4);
  slot.unk_e1e0 = rd(image + 0xE1E0, 4);
  slot.unk_e1e4 = rd(image + 0xE1E4, 4);
  slot.frames = rd16(image + 0xE1E8);
  slot.unk_e1ec = rd(image + 0xE1EC, 4);
  slot.unk_e1f0 = rd(image + 0xE1F0, 4);
}

Bytes slot_to_image(const SaveSlot& slot) {
  Bytes img(kImageSize, 0);
  u8* image = img.data();
  for (int i = 0; i < kNumChars; ++i) {
    const SaveChar& character = slot.chars[i];
    u8* record = image + rec(i);
    if (i == 0 || i == 43) record[1] = character.colour_bit;
    record[2] = character.rec02;
    record[0xA8] = character.rec_a8;
    record[0xB0] = character.status;
    wr(record + 0x16, 2, character.max_hp);
    wr(record + 0xB8, 2, character.cur_hp);
    wr(record + 0xBC, 2, character.hp_extra);
    wr(record + 0xBE, 2, character.unk_be);
    record[0x18] = character.atk; record[0x19] = character.def; record[0x1A] = character.hit; record[0x1B] = character.evd; record[0x1C] = character.mag; record[0x1D] = character.mdef;
    std::copy_n(character.bonus.begin(), 12, record + 0xC0);
    wr(record + 0x20, 2, character.weapon);
    wr(record + 0x22, 2, character.armour);
    for (int k = 0; k < 3; ++k) wr(record + 0x24 + 2 * k, 2, character.accessory[k]);
    wr(record + 0x2D, 2, character.model_id);
    for (int k = 0; k < 48; ++k) wr(record + 0x40 + 2 * k, 2, character.grid[k]);
    for (int k = 0; k < 8; ++k) wr(image + 0xDAD8 + i * 0x10 + 2 * k, 2, character.unlocked[k]);
    for (int k = 0; k < 48; ++k) image[0xDD98 + i * 0x18 + (k >> 1)] |= static_cast<u8>((character.equipped_qty[k] & 15) << (4 * (k & 1)));
    std::copy_n(character.name.begin(), 16, image + 0xD80C + i * 0x10);
  }
  for (int k = 0; k < kNumItems; ++k) { wr(image + 0xD13C + 2 * k, 2, slot.items[k].id); image[0xD43C + k] = slot.items[k].qty; }
  for (int k = 0; k < kNumElements; ++k) { wr(image + 0xD5BC + 2 * k, 2, slot.elements[k].id); image[0xD73C + k] = slot.elements[k].qty; }
  for (int k = 0; k < 4; ++k) wr(image + 4 * k, 4, slot.button_cfg[k]);
  for (int k = 0; k < 4; ++k) wr(image + 0xE1B8 + 4 * k, 4, slot.unk_e1b8[k]);
  wr(image + 0xE1E0, 4, slot.unk_e1e0);
  wr(image + 0xE1E4, 4, slot.unk_e1e4);
  wr(image + 0xE1E8, 2, slot.frames);
  wr(image + 0xE1EC, 4, slot.unk_e1ec);
  wr(image + 0xE1F0, 4, slot.unk_e1f0);
  return img;
}

}  // namespace

SaveSlot decode_block(ByteSpan block) {
  if (block.size() != kBlockSize) throw std::runtime_error("save: slot block must be 0x2000 bytes");
  SaveSlot slot;
  std::copy_n(block.begin(), kHeadSize, slot.head.begin());
  std::copy_n(block.begin() + kHeadSize + kPackedSize, kTailSize, slot.tail.begin());
  image_to_slot(unpack_image(block.subspan(kHeadSize, kPackedSize), block_version(block)), slot);
  slot.corrupted = !block_checksum_ok(block);
  return slot;
}

Bytes encode_block(const SaveSlot& slot) {
  Bytes out(kBlockSize, 0);
  std::copy(slot.head.begin(), slot.head.end(), out.begin());
  const Bytes packed = pack_image(slot_to_image(slot));
  std::copy(packed.begin(), packed.end(), out.begin() + kHeadSize);
  std::copy(slot.tail.begin(), slot.tail.end(), out.begin() + kHeadSize + kPackedSize);
  // What RDE's loader reads back: the save version at 0x1FF8 (0x106: all 54 steps present; > 0x102 makes the
  // checksum cover the header frame as well - a block with version 0 is summed over 0x180..0x1FF7 only and reads "data corrupted"),
  // the bit count of the summary stream at 0x24 (210 bits) and the header frame's 01 00 "CRD0" marker at 0x50.
  wr(&out[kVersionOffset], 4, kSaveVersion);
  wr(&out[0x24], 4, kSummaryBits);
  static const u8 kCrd[6] = {0x01, 0x00, 'C', 'R', 'D', '0'};
  std::copy(std::begin(kCrd), std::end(kCrd), out.begin() + 0x50);
  wr(&out[kChecksumOffset], 4, block_sum(out));
  return out;
}

// ----------------------------------------------------------------- memory card slots
std::string slot_file_name(int slot) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "BISLPSP02364CHRONO%02d", slot);
  return buf;
}

std::vector<SlotRef> list_slots(const MemCard& card) {
  std::vector<SlotRef> out;
  for (int block = 1; block <= 15; ++block) {
    const DirFrame frame = card.dir(block);
    if (!frame.first()) continue;
    const bool jp = frame.name.find("SLPSP02364") != std::string::npos && frame.name.find("CHRONO") != std::string::npos;
    const bool us = frame.name.find("SLUSP01041") != std::string::npos && frame.name.find("USCHRO") != std::string::npos;
    if ((!jp && !us) || frame.name.size() < 2) continue;
    const char tens = frame.name[frame.name.size() - 2], ones = frame.name[frame.name.size() - 1];
    if (tens < '0' || tens > '9' || ones < '0' || ones > '9') continue;
    out.push_back({(tens - '0') * 10 + (ones - '0'), block, frame.name});
  }
  std::sort(out.begin(), out.end(), [](const SlotRef& lhs, const SlotRef& rhs) { return lhs.slot < rhs.slot; });
  return out;
}

SaveSlot read_slot(const MemCard& card, int block) { return decode_block(card.block(block)); }

namespace {
// Header frame title (block offset 4, Shift-JIS): "クロノ・クロス　" + the two fullwidth digits of (slot + 1), as the real blocks have it.
void stamp_title(SaveSlot& target, int slot) {
  static const u8 kPrefix[] = {0x83, 0x4E, 0x83, 0x8D, 0x83, 0x6D, 0x81, 0x45, 0x83, 0x4E, 0x83, 0x8D, 0x83, 0x58, 0x81, 0x40};
  const int number = std::clamp(slot + 1, 0, 99);
  target.head[0] = 'S';
  target.head[1] = 'C';
  if (target.head[2] == 0) target.head[2] = 0x11;
  if (target.head[3] == 0) target.head[3] = 0x01;
  for (size_t i = 4; i < 4 + 64 && i < 0x28; ++i) target.head[i] = 0;
  size_t offset = 4;
  for (u8 byte : kPrefix) target.head[offset++] = byte;
  for (int digit : {number / 10, number % 10}) {
    target.head[offset++] = 0x82;
    target.head[offset++] = static_cast<u8>(0x4F + digit);
  }
}
}  // namespace

int write_slot(MemCard& card, int slot, const SaveSlot& src) {
  const std::string name = slot_file_name(slot);
  int block = card.find(name);
  if (!block) block = card.allocate();
  if (!block) return 0;
  SaveSlot stamped = src;
  stamp_title(stamped, slot);
  card.write_block(block, encode_block(stamped));
  card.set_dir(block, 0x51, MemCard::kBlock, 0xFFFF, name);
  return block;
}

// ----------------------------------------------------------------- save.dat
SaveDat SaveDat::open(ByteSpan bytes) {
  SaveDat dat;
  dat.members_ = zip_open(bytes);
  for (int card_number = 1; card_number <= 2; ++card_number) {
    const ZipMember* entry = zip_find(dat.members_, "memcard" + std::to_string(card_number) + ".mcr");
    if (entry && entry->data.size() == MemCard::kSize) dat.cards_[card_number - 1].emplace(entry->data);
  }
  return dat;
}

SaveDat SaveDat::create_empty() {
  SaveDat dat;
  dat.ensure_cards();
  return dat;
}

bool SaveDat::ensure_cards() {
  bool changed = false;
  for (int card_number = 1; card_number <= 2; ++card_number) {
    if (cards_[card_number - 1]) continue;
    const std::string name = "memcard" + std::to_string(card_number) + ".mcr";
    cards_[card_number - 1].emplace(MemCard::blank());
    bool found = false;
    for (auto& entry : members_)
      if (entry.name == name) entry.data = cards_[card_number - 1]->bytes(), entry.deflate = true, found = true;
    if (!found) members_.push_back({name, cards_[card_number - 1]->bytes(), true});
    changed = true;
  }
  return changed;
}

SaveDat SaveDat::load(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("save: cannot open " + path.string());
  Bytes bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return open(bytes);
}

MemCard* SaveDat::card(int card_number) { return (card_number == 1 || card_number == 2) && cards_[card_number - 1] ? &*cards_[card_number - 1] : nullptr; }
const MemCard* SaveDat::card(int card_number) const { return (card_number == 1 || card_number == 2) && cards_[card_number - 1] ? &*cards_[card_number - 1] : nullptr; }
const ZipMember* SaveDat::member(std::string_view name) const { return zip_find(members_, name); }

ExtFile SaveDat::ext() const {
  const ZipMember* entry = zip_find(members_, "ext_save.dat");
  return entry ? ExtFile::parse(entry->data) : ExtFile();
}

void SaveDat::set_ext(const ExtFile& file) {
  for (auto& entry : members_)
    if (entry.name == "ext_save.dat") {
      entry.data = file.bytes();
      return;
    }
  members_.push_back({"ext_save.dat", file.bytes(), true});
}

bool SaveDat::write_ext(int card, int slot_no) {
  if (!ext_host_ || (card != 1 && card != 2) || slot_no < 0 || slot_no > 14) return false;
  ExtSlot record;
  record.language = ext_host_->language;
  record.battles = ext_host_->battles;
  if (ext_host_->names) record.names = *ext_host_->names;
  // The pair (form, valid): RDE's loader takes the form byte only when it is
  // 3..6, else derives it from its own story byte (0x2B..0x2D -> 3, 0x2E -> 4, 0x2F -> 5, 0x30 -> 6; otherwise the live value stays) and runs a stat
  // sync whenever valid == 0 and the story byte is 0x2C..0x30. Form 0 (we have no guest form byte) lets it derive as before; valid 1 is what
  // every RDE save writes, so a story-end OpenCross save triggers no spurious stat sync (which rewrites stats) on its first RDE load.
  record.form = 0;
  record.valid = 1;
  ExtFile file = ext();
  file.set_slot(ext_slot_index(card, slot_no), record);
  set_ext(file);
  return true;
}

ExtSlot SaveDat::peek_ext(int card, int slot_no, u32 language) const {
  ExtSlot record;
  if ((card == 1 || card == 2) && slot_no >= 0 && slot_no <= 14) record = ext().slot(ext_slot_index(card, slot_no));
  if (record.language != language) record.names.clear();
  return record;
}

ExtSlot SaveDat::read_ext(int card, int slot_no, u32 language) const {
  ExtSlot record = peek_ext(card, slot_no, language);
  if (ext_host_) ext_host_->battles = record.battles;
  return record;
}

Bytes SaveDat::serialize(u64 salt_seed) const {
  std::vector<ZipMember> out = members_;
  for (int card_number = 1; card_number <= 2; ++card_number) {
    if (!cards_[card_number - 1]) continue;
    const std::string name = "memcard" + std::to_string(card_number) + ".mcr";
    for (auto& entry : out)
      if (entry.name == name) entry.data = cards_[card_number - 1]->bytes();
  }
  return zip_write(out, kRdeSavePassword, salt_seed);
}

void SaveDat::save(const std::filesystem::path& path) const {
  const Bytes bytes = serialize();
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream stream(tmp, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("save: cannot write " + tmp.string());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("save: write failed " + tmp.string());
  }
  std::filesystem::rename(tmp, path);
}

}  // namespace oc::save
