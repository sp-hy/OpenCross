#include "data/tables.h"

#include <algorithm>
#include <optional>

#include "import/container.h"
#include "import/lzss.h"
#include "import/text.h"
#include "vfs/vfs.h"

namespace oc::data {

namespace {

const std::string kEmpty;

// RDE's corrections to single element records, kept as data: a byte is replaced when it still holds the retail value.
struct ElementFix {
  u16 element;
  u8 offset, retail, rde;
};
constexpr ElementFix kElementFixes[] = {
    // Flower (mode 7, M. DEF. +): the end-of-action message row 47 "M. DEF. temporarily increased!" instead of 52, the M. ATT. row.
    {201, 0x13, 0x34, 0x2F},
};

std::optional<Bytes> read_battle(const vfs::Vfs& game_fs, const std::string& name) {
  return game_fs.read("cdrom/battle/data/" + name);
}

// player.dat / exdat.dat: cpt of (usually sszl) parts.
std::vector<Bytes> load_bundle(const vfs::Vfs& game_fs, const std::string& name) {
  std::vector<Bytes> out;
  auto raw = read_battle(game_fs, name);
  if (!raw) return out;
  auto cpt = import::parse_cpt(*raw);
  if (!cpt) return out;
  for (const auto& entry : cpt->entries) {
    if (import::is_lzss(entry.data)) {
      auto decoded = import::lzss_decode(entry.data);
      out.push_back(decoded ? std::move(*decoded) : Bytes{});
    } else {
      out.emplace_back(entry.data.begin(), entry.data.end());
    }
  }
  return out;
}

u16 rd16(const Bytes& bytes, size_t offset) { return static_cast<u16>(bytes[offset] | (bytes[offset + 1] << 8)); }

std::vector<std::vector<u16>> rows_u16(const Bytes& bytes, int rows) {
  std::vector<std::vector<u16>> out;
  for (int row_index = 0; row_index < rows; ++row_index) {
    std::vector<u16> row;
    for (int col = 0; col < 32; ++col) {
      size_t offset = static_cast<size_t>(row_index) * 64 + static_cast<size_t>(col) * 2;
      row.push_back(offset + 2 <= bytes.size() ? rd16(bytes, offset) : u16(0));
    }
    while (!row.empty() && row.back() == 0) row.pop_back();
    out.push_back(std::move(row));
  }
  return out;
}

template <size_t N>
void copy_rec(std::array<u8, N>& dst, const Bytes& src, size_t off) {
  std::copy_n(src.begin() + static_cast<std::ptrdiff_t>(off), N, dst.begin());
}

}  // namespace

std::array<u16, kGridCells> StatRec::grid() const {
  std::array<u16, kGridCells> cells{};
  int listed = 0;
  for (int row = 0; row < kGridRows; ++row)
    for (int col = 0; col < kGridCols; ++col)
      if (level_mask(row) >> col & 1) {
        if (listed < 48) cells[static_cast<size_t>(row * kGridCols + col)] = cell_listed(listed);
        ++listed;
      }
  return cells;
}

int colour_index_of_bit(u8 bit) {
  switch (bit) {
    case 0x04: return 0;
    case 0x08: return 1;
    case 0x10: return 2;
    case 0x20: return 3;
    case 0x40: return 4;
    case 0x80: return 5;
    default: return -1;
  }
}

const char* colour_name(int idx) {
  static const char* names[] = {"green", "white", "black", "red", "yellow", "blue"};
  return idx >= 0 && idx < 6 ? names[idx] : "none";
}

static const std::string& pick(const std::vector<std::string>& list, int i) {
  return i >= 0 && i < static_cast<int>(list.size()) ? list[static_cast<size_t>(i)] : kEmpty;
}
const std::string& GameTables::enemy_name(int i) const { return pick(enemy_names, i); }
const std::string& GameTables::element_name(int i) const { return pick(element_names, i); }
const std::string& GameTables::char_name(int i) const { return pick(char_names, i); }
const std::string& GameTables::item_name(int i) const { return pick(item_names, i); }

int GameTables::scene_for(int battle_id, int variant) const {
  if (battle_id >= 0x200) return battle_id - 0x200;
  size_t i = static_cast<size_t>(battle_id) * 8 + static_cast<size_t>(variant & 7);
  return i < symtbl.size() ? symtbl[i] : -1;
}

bool GameTables::load(const vfs::Vfs& game_fs, std::string* err) {
  auto fail = [&](const std::string& message) {
    if (err) *err = message;
    return false;
  };
  // --- enemies
  auto mon = read_battle(game_fs, "monster.bin");
  if (!mon) return fail("battle/data/monster.bin missing");
  enemies.resize(mon->size() / kStatRecSize);
  for (size_t i = 0; i < enemies.size(); ++i) copy_rec(enemies[i].raw, *mon, i * kStatRecSize);

  // --- characters (+ presets). Every set takes its Lv 99 targets from lv99para.bin; the bytes after the 51 records of char_w1..5 are
  // leftover record copies, not Lv 99 rows.
  Bytes lv99_file;
  if (auto lv99_bytes = read_battle(game_fs, "lv99para.bin")) lv99_file = *lv99_bytes;
  for (const char* set_name : {"char", "char20", "char_w0", "char_w1", "char_w2", "char_w3", "char_w4", "char_w5"}) {
    auto set_bytes = read_battle(game_fs, std::string(set_name) + ".bin");
    if (!set_bytes || set_bytes->size() < kNumChars * kStatRecSize) {
      if (std::string(set_name) == "char") return fail("battle/data/char.bin missing");
      continue;
    }
    CharSet char_set;
    for (size_t i = 0; i < kNumChars; ++i) copy_rec(char_set.rec[i].raw, *set_bytes, i * kStatRecSize);
    for (size_t i = 0; i < kNumChars && i * 8 + 8 <= lv99_file.size(); ++i) {
      const size_t offset = i * 8;
      char_set.lv99[i] = {rd16(lv99_file, offset), lv99_file[offset + 2], lv99_file[offset + 3], lv99_file[offset + 4],
                          lv99_file[offset + 5], lv99_file[offset + 6], lv99_file[offset + 7]};
    }
    char_sets.emplace(set_name, std::move(char_set));
  }

  // --- elements / techs
  auto pg = read_battle(game_fs, "pgdat.bin");
  if (!pg) return fail("battle/data/pgdat.bin missing");
  elements.resize(pg->size() / kElemRecSize);
  for (size_t i = 0; i < elements.size(); ++i) copy_rec(elements[i].raw, *pg, i * kElemRecSize);
  for (const ElementFix& fix : kElementFixes)
    if (fix.element < elements.size() && elements[fix.element].raw[fix.offset] == fix.retail) elements[fix.element].raw[fix.offset] = fix.rde;

  // --- growth
  auto gr = read_battle(game_fs, "el_mask.bin");
  if (!gr) return fail("battle/data/el_mask.bin missing");
  growth.resize(gr->size() / 8);
  for (size_t i = 0; i < growth.size(); ++i) copy_rec(growth[i], *gr, i * 8);

  // --- player.dat parts: 0 scenedat, 2 symtbl, 3 btbgdat
  auto player = load_bundle(game_fs, "player.dat");
  if (player.size() < 4) return fail("battle/data/player.dat unreadable");
  scenes.resize(player[0].size() / 28);
  for (size_t i = 0; i < scenes.size(); ++i) copy_rec(scenes[i].raw, player[0], i * 28);
  symtbl.resize(player[2].size() / 2);
  for (size_t i = 0; i < symtbl.size(); ++i) symtbl[i] = rd16(player[2], i * 2);
  btbgdat.resize(player[3].size() / 3);
  for (size_t i = 0; i < btbgdat.size(); ++i) copy_rec(btbgdat[i], player[3], i * 3);

  // --- equipment stats (player.dat part 4)
  if (player.size() > 4 && player[4].size() >= 256 * kGearRecSize) {
    gear.resize(256);
    for (size_t i = 0; i < 256; ++i) copy_rec(gear[i].raw, player[4], i * kGearRecSize);
  }

  // --- drops
  auto dit = read_battle(game_fs, "ditem.dat");
  if (!dit) return fail("battle/data/ditem.dat missing");
  Bytes dd = import::is_lzss(*dit) ? import::lzss_decode(*dit).value_or(Bytes{}) : *dit;
  drops.resize(dd.size() / 8);
  for (size_t i = 0; i < drops.size(); ++i)
    for (size_t k = 0; k < 4; ++k) drops[i].slot[k] = rd16(dd, i * 8 + k * 2);

  // --- exdat part 0 (star progress rows)
  auto ex = load_bundle(game_fs, "exdat.dat");
  if (!ex.empty()) {
    if (ex.size() > 5 && ex[5].size() >= 256 * 12) {
      item_info.resize(256);
      for (size_t i = 0; i < 256; ++i) {
        const size_t offset = i * 12;
        item_info[i].equip_lo = static_cast<u32>(rd16(ex[5], offset)) | (static_cast<u32>(rd16(ex[5], offset + 2)) << 16);
        item_info[i].equip_hi = static_cast<u32>(rd16(ex[5], offset + 4)) | (static_cast<u32>(rd16(ex[5], offset + 6)) << 16);
        item_info[i].price10 = static_cast<s16>(rd16(ex[5], offset + 8));
        item_info[i].cat = ex[5][offset + 10];
        item_info[i].extra = ex[5][offset + 11];
      }
    }
    star_rows.resize(ex[0].size() / 4);
    for (size_t i = 0; i < star_rows.size(); ++i) copy_rec(star_rows[i], ex[0], i * 4);
  }

  // --- shops / trade
  if (auto shop_bytes = game_fs.read("cdrom/menu/el_shop.bin")) el_shop = rows_u16(*shop_bytes, 32);
  if (auto shop_bytes = game_fs.read("cdrom/menu/waa_shop.bin")) waa_shop = rows_u16(*shop_bytes, 32);
  if (auto trade_bytes = game_fs.read("cdrom/menu/trade.bin")) {
    for (auto& row : rows_u16(*trade_bytes, 32)) {
      std::vector<TradeEntry> entries;
      for (size_t k = 0; k + 1 < row.size(); k += 2) entries.push_back({row[k], row[k + 1]});
      trade.push_back(std::move(entries));
    }
  }
  if (auto forge_bytes = game_fs.read("cdrom/menu/waa_tbl.bin"); forge_bytes && forge_bytes->size() >= 256 * 32) {
    forge.resize(256);
    for (size_t i = 0; i < 256; ++i) copy_rec(forge[i], *forge_bytes, i * 32);
  }
  if (auto keyitem_bytes = game_fs.read("cdrom/menu/keyitem.dat"); keyitem_bytes && keyitem_bytes->size() >= 2)
    keyitem_jp_rows = rd16(*keyitem_bytes, 0) / 2u - 1u;

  // --- RNG permutation
  if (auto exe = game_fs.read("cdrom/prog/slps_023.64"); exe && exe->size() >= 0x5B584 + 256)
    rng_table.assign(exe->begin() + 0x5B584, exe->begin() + 0x5B584 + 256);

  // --- encounter variant bands
  if (auto prg = game_fs.read("cdrom/prog/btlinit.prg"); prg && prg->size() >= 0x6AF8 + 8 * 4)
    for (size_t i = 0; i < 8; ++i) variant_bands.push_back({rd16(*prg, 0x6AF8 + i * 4), rd16(*prg, 0x6AF8 + i * 4 + 2)});

  // --- names
  import::LangTables lang_tables;
  lang_tables.init(&game_fs);
  auto names = [&](u32 category, size_t count) {
    std::vector<std::string> out(count);
    for (size_t i = 0; i < count; ++i)
      if (const std::string* text = lang_tables.sysmes((category << 16) | static_cast<u32>(i), import::Lang::En)) out[i] = *text;
    return out;
  };
  enemy_names = names(0x19, enemies.size());
  element_names = names(0x17, elements.size());
  char_names = names(0x01, kNumChars);
  item_names = names(0x00, 256);
  key_item_names = names(0x18, 65);
  return true;
}

}  // namespace oc::data
