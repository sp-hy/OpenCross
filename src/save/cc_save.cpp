#include "save/cc_save.h"

#include "core/log.h"

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
void SaveSlot::stamp_preview_party() {
  constexpr u8 kEmpty = 0xFF;
  for (size_t i = 0; i < 3; ++i) {
    const u8 id = head[head::kParty + i];
    u8 face = kEmpty;
    if (id < kNumChars) {
      const u8 model = static_cast<u8>(chars[id].model_id & 0xFF);
      face = model <= kLastFace ? model : id;
    }
    head[head::kPreviewParty + i] = face;
  }
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

namespace {
constexpr std::array<u8, 4> kTailMagic = {'O', 'C', 'X', 1};
bool tail_has_record(const std::array<u8, kTailSize>& tail) { return std::equal(kTailMagic.begin(), kTailMagic.end(), tail.begin()); }
// The entries of a tail record in order, each as {tag, bytes}; stops at tag 0 or a size that runs past the record.
std::vector<std::pair<u8, std::vector<u8>>> tail_entries(const std::array<u8, kTailSize>& tail) {
  std::vector<std::pair<u8, std::vector<u8>>> entries;
  if (!tail_has_record(tail)) return entries;
  for (size_t pos = kTailMagic.size(); pos + 2 <= kTailExtSize && tail[pos] != 0;) {
    const size_t size = tail[pos + 1];
    if (pos + 2 + size > kTailExtSize) break;
    const auto first = tail.begin() + static_cast<std::ptrdiff_t>(pos + 2);
    entries.emplace_back(tail[pos], std::vector<u8>(first, first + static_cast<std::ptrdiff_t>(size)));
    pos += 2 + size;
  }
  return entries;
}
}  // namespace

std::vector<u8> SaveSlot::tail_entry(u8 tag) const {
  for (auto& [entry_tag, bytes] : tail_entries(tail))
    if (entry_tag == tag) return bytes;
  return {};
}
// The record is rewritten whole: the magic, the other entries in their order, this one last, zeros after the end tag. Removing an
// entry that isn't there leaves the tail as it is (a vanilla slot's tail bytes round-trip).
bool SaveSlot::set_tail_entry(u8 tag, const std::vector<u8>& bytes) {
  if (tag == 0 || bytes.size() > 0xFF) return false;
  auto entries = tail_entries(tail);
  const size_t before = entries.size();
  std::erase_if(entries, [&](const auto& entry) { return entry.first == tag; });
  if (bytes.empty() && entries.size() == before) return true;
  if (!bytes.empty()) entries.emplace_back(tag, bytes);
  size_t total = kTailMagic.size() + 1;
  for (const auto& entry : entries) total += 2 + entry.second.size();
  if (total > kTailExtSize) return false;
  std::fill(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(kTailExtSize), u8{0});
  std::copy(kTailMagic.begin(), kTailMagic.end(), tail.begin());
  size_t pos = kTailMagic.size();
  for (const auto& [entry_tag, entry_bytes] : entries) {
    tail[pos] = entry_tag;
    tail[pos + 1] = static_cast<u8>(entry_bytes.size());
    std::copy(entry_bytes.begin(), entry_bytes.end(), tail.begin() + static_cast<std::ptrdiff_t>(pos + 2));
    pos += 2 + entry_bytes.size();
  }
  return true;
}

std::vector<int> SaveSlot::reserve() const {
  std::vector<int> slots;
  for (u8 id : tail_entry(tail_tag::kReserve)) slots.push_back(id < kNumChars ? id : -1);
  return slots;
}
void SaveSlot::set_reserve(const std::vector<int>& slots) {
  std::vector<u8> bytes;
  for (int id : slots) bytes.push_back(id >= 0 && id < kNumChars ? static_cast<u8>(id) : u8{0xFF});
  set_tail_entry(tail_tag::kReserve, bytes);
}

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
  // Counts, stats, HP and ids saturate at the field's largest value; level masks and name glyphs keep their low bits.
  auto mask = [](u32 value, int bit_count) { return bit_count >= 32 ? value : (value & ((1u << bit_count) - 1)); };
  auto saturate = [](u32 value, int bit_count) { return bit_count >= 32 ? value : std::min(value, (1u << bit_count) - 1); };
  for (const Step& step : kSteps) {
    switch (step.mode) {
      case 0: {
        const int sz = value_bytes(step.length);
        for (int i = 0; i < step.count; ++i) bw.put(saturate(rd(&img[step.off + size_t(i) * step.special], sz), step.length), step.length);
        break;
      }
      case 1:
        for (int character = 0; character < step.count; ++character)
          for (int j = 0; j < step.special; ++j) {
            const u8 cell = img[step.off + size_t(character) * 0x18 + (j >> 1)];
            bw.put(saturate((j & 1) ? (cell >> 4) : (cell & 15), step.length), step.length);
          }
        break;
      case 2:
        for (int character = 0; character < step.count; ++character)
          for (int k = 0; k < 0x30; ++k) bw.put(saturate(rd(&img[step.off + size_t(character) * kStride + k * step.special], 2), step.length), step.length);
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

// ----------------------------------------------------------------- side file
// "OCXD", u32 version 1, then sections {u16 tag, u32 size, size bytes} to the end; a reader skips the tags it doesn't know. State the
// block can't hold gets a tag of its own here rather than a second file.
constexpr std::array<u8, 4> kSideMagic = {'O', 'C', 'X', 'D'};
constexpr u32 kSideVersion = 1;
constexpr size_t kSideHeader = 8, kSectionHeader = 6;
namespace side_tag {
// Element ids past 511.
inline constexpr u16 kElementIds = 1;
}  // namespace side_tag

std::filesystem::path& wide_dir() {
  static std::filesystem::path dir;
  return dir;
}
void put16(Bytes& out, u32 value) {
  out.push_back(static_cast<u8>(value));
  out.push_back(static_cast<u8>(value >> 8));
}
void put32(Bytes& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}

// Section kElementIds: u16 n + n x {u16 stock slot, u16 id, u8 qty}, then u16 m + m x {u8 character, u8 grid cell, u16 id}. Empty
// when every id fits the stream.
Bytes element_id_section(const SaveSlot& slot) {
  Bytes stacks, cells;
  u32 stack_count = 0, cell_count = 0;
  for (size_t i = 0; i < slot.elements.size(); ++i)
    if (slot.elements[i].id >= 0x200) {
      put16(stacks, static_cast<u32>(i));
      put16(stacks, slot.elements[i].id);
      stacks.push_back(slot.elements[i].qty);
      ++stack_count;
    }
  for (size_t character = 0; character < slot.chars.size(); ++character)
    for (size_t k = 0; k < slot.chars[character].grid.size(); ++k)
      if (const u32 id = slot.chars[character].grid[k] & 0x3FFFu; id >= 0x200) {
        cells.push_back(static_cast<u8>(character));
        cells.push_back(static_cast<u8>(k));
        put16(cells, id);
        ++cell_count;
      }
  if (stack_count == 0 && cell_count == 0) return {};
  Bytes out;
  put16(out, stack_count);
  out.insert(out.end(), stacks.begin(), stacks.end());
  put16(out, cell_count);
  out.insert(out.end(), cells.begin(), cells.end());
  return out;
}
void apply_element_ids(SaveSlot& slot, ByteSpan bytes) {
  size_t pos = 0;
  if (bytes.size() < 2) return;
  const u32 stack_count = rd(&bytes[pos], 2);
  pos += 2;
  for (u32 i = 0; i < stack_count && pos + 5 <= bytes.size(); ++i, pos += 5)
    if (const u32 at = rd(&bytes[pos], 2); at < slot.elements.size())
      slot.elements[at] = {rd16(&bytes[pos + 2]), bytes[pos + 4]};
  if (pos + 2 > bytes.size()) return;
  const u32 cell_count = rd(&bytes[pos], 2);
  pos += 2;
  for (u32 i = 0; i < cell_count && pos + 4 <= bytes.size(); ++i, pos += 4)
    if (bytes[pos] < slot.chars.size() && bytes[pos + 1] < slot.chars[0].grid.size())
      slot.chars[bytes[pos]].grid[bytes[pos + 1]] = rd16(&bytes[pos + 2]);
}

// The side file of a slot: empty when no section has anything.
Bytes side_file(const SaveSlot& slot) {
  const std::pair<u16, Bytes> sections[] = {{side_tag::kElementIds, element_id_section(slot)}};
  Bytes out(kSideMagic.begin(), kSideMagic.end());
  put32(out, kSideVersion);
  for (const auto& [tag, bytes] : sections) {
    if (bytes.empty()) continue;
    put16(out, tag);
    put32(out, static_cast<u32>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out.size() > kSideHeader ? out : Bytes{};
}
u64 fnv1a(ByteSpan bytes) {
  u64 hash = 0xCBF29CE484222325ull;
  for (const u8 byte : bytes) hash = (hash ^ byte) * 0x100000001B3ull;
  return hash;
}
std::filesystem::path side_path(u64 key) {
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.ocx", static_cast<unsigned long long>(key));
  return wide_dir() / name;
}
// Lays the side file the tail record names over a decoded slot. No entry, no store or no file: the slot stays as the block has it.
void apply_side_file(SaveSlot& slot) {
  const std::vector<u8> key_bytes = slot.tail_entry(tail_tag::kSideFile);
  if (key_bytes.size() != 8 || wide_dir().empty()) return;
  u64 key = 0;
  for (int i = 7; i >= 0; --i) key = key << 8 | key_bytes[static_cast<size_t>(i)];
  std::ifstream stream(side_path(key), std::ios::binary);
  const Bytes bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (bytes.size() < kSideHeader || !std::equal(kSideMagic.begin(), kSideMagic.end(), bytes.begin())) {
    log::warn("save: OpenCross side file {:016x} missing", key);
    return;
  }
  for (size_t at = kSideHeader; at + kSectionHeader <= bytes.size();) {
    const u32 tag = rd(&bytes[at], 2);
    const u32 size = rd(&bytes[at + 2], 4);
    if (size > bytes.size() - at - kSectionHeader) break;
    const ByteSpan section = ByteSpan(bytes).subspan(at + kSectionHeader, size);
    if (tag == side_tag::kElementIds) apply_element_ids(slot, section);
    at += kSectionHeader + size;
  }
}

}  // namespace

void set_wide_store(std::filesystem::path dir) { wide_dir() = std::move(dir); }

SaveSlot decode_block(ByteSpan block) {
  if (block.size() != kBlockSize) throw std::runtime_error("save: slot block must be 0x2000 bytes");
  SaveSlot slot;
  std::copy_n(block.begin(), kHeadSize, slot.head.begin());
  std::copy_n(block.begin() + kHeadSize + kPackedSize, kTailSize, slot.tail.begin());
  image_to_slot(unpack_image(block.subspan(kHeadSize, kPackedSize), block_version(block)), slot);
  slot.corrupted = !block_checksum_ok(block);
  apply_side_file(slot);
  return slot;
}

Bytes encode_block(const SaveSlot& full) {
  // The stream gets the slot without what the side file carries: ids past 511 as empty, grid words as their id bits only.
  SaveSlot slot = full;
  const Bytes side = side_file(full);
  for (auto& stack : slot.elements)
    if (stack.id >= 0x200) stack = {};
  for (auto& ch : slot.chars)
    for (u16& word : ch.grid) word = (word & 0x3FFF) >= 0x200 ? u16{0} : static_cast<u16>(word & 0x3FFF);
  if (side.empty()) {
    slot.set_tail_entry(tail_tag::kSideFile, {});
  } else if (wide_dir().empty()) {
    log::warn("save: {} bytes of element ids past 511 dropped (no OpenCross save folder)", side.size());
    slot.set_tail_entry(tail_tag::kSideFile, {});
  } else {
    const u64 key = fnv1a(side);
    std::error_code ec;
    std::filesystem::create_directories(wide_dir(), ec);
    std::ofstream(side_path(key), std::ios::binary).write(reinterpret_cast<const char*>(side.data()), static_cast<std::streamsize>(side.size()));
    std::vector<u8> key_bytes(8);
    for (int i = 0; i < 8; ++i) key_bytes[static_cast<size_t>(i)] = static_cast<u8>(key >> (8 * i));
    if (!slot.set_tail_entry(tail_tag::kSideFile, key_bytes)) log::warn("save: no room in the tail record for the side file's key");
  }
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
  std::string name = slot_file_name(slot);
  int block = 0;
  for (const SlotRef& existing : list_slots(card))
    if (existing.slot == slot) {
      block = existing.block;
      name = existing.name;
      break;
    }
  if (!block) block = card.allocate();
  if (!block) return 0;
  SaveSlot stamped = src;
  stamp_title(stamped, slot);
  stamped.stamp_preview_party();
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
