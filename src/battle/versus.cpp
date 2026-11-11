// The versus side exchange format (one format byte, then the stars, the pool and each member's fields in a fixed order), the check that
// a received side is one a real game could send, the battlefield and carrier scene picks, and the random legal player.
#include "battle/versus.h"

#include <algorithm>
#include <format>
#include <limits>

namespace oc::battle {

namespace {

constexpr size_t kMaxNameBytes = 256;
constexpr size_t kMaxHpPct = 16;
// Seeds the battlefield pick apart from the battle's own generator.
constexpr u32 kFieldPickSalt = 0x9E3779B1u;

class Writer {
 public:
  void unsigned_value(u64 value) {
    while (value >= 0x80) {
      out_.push_back(static_cast<char>((value & 0x7F) | 0x80));
      value >>= 7;
    }
    out_.push_back(static_cast<char>(value));
  }
  void signed_value(s64 value) { unsigned_value((static_cast<u64>(value) << 1) ^ static_cast<u64>(value >> 63)); }
  void text(const std::string& value) {
    unsigned_value(value.size());
    out_ += value;
  }
  template <size_t N>
  void bits(const std::array<bool, N>& flags) {
    for (size_t first = 0; first < N; first += 8) {
      u8 byte = 0;
      for (size_t k = 0; k < 8 && first + k < N; ++k) byte = static_cast<u8>(byte | (flags[first + k] ? 1u << k : 0u));
      out_.push_back(static_cast<char>(byte));
    }
  }
  template <class T, size_t N>
  void values(const std::array<T, N>& items) {
    for (const T& item : items) signed_value(static_cast<s64>(item));
  }
  std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

// Every read checks the bounds of its field; the first failure sticks and the result is dropped.
class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  bool ok() const { return ok_; }
  bool at_end() const { return pos_ == bytes_.size(); }
  u64 unsigned_value(u64 max) {
    u64 value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos_ >= bytes_.size()) return fail();
      const u8 byte = static_cast<u8>(bytes_[pos_++]);
      value |= static_cast<u64>(byte & 0x7F) << shift;
      if (!(byte & 0x80)) return value <= max ? value : fail();
    }
    return fail();
  }
  s64 signed_value(s64 min, s64 max) {
    const u64 raw = unsigned_value(std::numeric_limits<u64>::max());
    const s64 value = static_cast<s64>(raw >> 1) ^ -static_cast<s64>(raw & 1);
    if (value < min || value > max) {
      fail();
      return 0;
    }
    return value;
  }
  int int_value() { return static_cast<int>(signed_value(std::numeric_limits<int>::min(), std::numeric_limits<int>::max())); }
  std::string text() {
    const size_t size = static_cast<size_t>(unsigned_value(kMaxNameBytes));
    if (!ok_ || bytes_.size() - pos_ < size) {
      fail();
      return {};
    }
    std::string value(bytes_.substr(pos_, size));
    pos_ += size;
    return value;
  }
  template <size_t N>
  void bits(std::array<bool, N>& flags) {
    for (size_t first = 0; first < N; first += 8) {
      if (pos_ >= bytes_.size()) {
        fail();
        return;
      }
      const u8 byte = static_cast<u8>(bytes_[pos_++]);
      for (size_t k = 0; k < 8 && first + k < N; ++k) flags[first + k] = (byte >> k & 1u) != 0;
    }
  }
  template <class T, size_t N>
  void values(std::array<T, N>& items) {
    for (T& item : items) item = static_cast<T>(signed_value(std::numeric_limits<T>::min(), std::numeric_limits<T>::max()));
  }

 private:
  u64 fail() {
    ok_ = false;
    pos_ = bytes_.size();
    return 0;
  }
  std::string_view bytes_;
  size_t pos_ = 0;
  bool ok_ = true;
};

void write_equip(Writer& out, const Equip& eq) {
  for (const int value : {eq.wp, eq.wp_acc, eq.wp_var, eq.wp_class, static_cast<int>(eq.wp_sweeps), eq.weapon_id, eq.arm, eq.arm_var, eq.mag_gear,
                          eq.mdef_gear, eq.evd_gear, static_cast<int>(eq.gear_flags), static_cast<int>(eq.armour_colour), eq.stamina_mod, eq.start_gauge,
                          eq.dur_mod})
    out.signed_value(value);
  for (const u32 mask : {eq.slayer, eq.resist_lo, eq.resist_hi, eq.status_lo, eq.status_hi, eq.guard_hi}) out.unsigned_value(mask);
  out.values(eq.acc);
  out.unsigned_value(eq.hp_pct.size());
  for (const int pct : eq.hp_pct) out.signed_value(pct);
}

void read_equip(Reader& in, Equip& eq) {
  for (int* value : {&eq.wp, &eq.wp_acc, &eq.wp_var, &eq.wp_class}) *value = in.int_value();
  eq.wp_sweeps = in.signed_value(0, 1) != 0;
  for (int* value : {&eq.weapon_id, &eq.arm, &eq.arm_var, &eq.mag_gear, &eq.mdef_gear, &eq.evd_gear}) *value = in.int_value();
  eq.gear_flags = static_cast<u16>(in.signed_value(0, 0xFFFF));
  eq.armour_colour = static_cast<u8>(in.signed_value(0, 0xFF));
  for (int* value : {&eq.stamina_mod, &eq.start_gauge, &eq.dur_mod}) *value = in.int_value();
  for (u32* mask : {&eq.slayer, &eq.resist_lo, &eq.resist_hi, &eq.status_lo, &eq.status_hi, &eq.guard_hi}) *mask = static_cast<u32>(in.unsigned_value(0xFFFFFFFFu));
  in.values(eq.acc);
  eq.hp_pct.resize(static_cast<size_t>(in.unsigned_value(kMaxHpPct)));
  for (int& pct : eq.hp_pct) pct = in.int_value();
}

void write_member(Writer& out, const PartyMember& member) {
  out.signed_value(member.char_id);
  out.text(member.name);
  out.text(member.own_name);
  for (const auto* stats : {&member.stat, &member.snap, &member.pending, &member.lv99, &member.base}) out.values(*stats);
  for (const int value : {member.hp, static_cast<int>(member.innate), member.growth_col, member.dur, member.rate, member.hit_med, member.hit_heavy, member.kind,
                          static_cast<int>(member.evolved)})
    out.signed_value(value);
  out.unsigned_value(member.lo_persist);
  out.values(member.cells);
  out.bits(member.unlocked);
  out.values(member.qty);
  out.bits(member.stack);
  write_equip(out, member.eq);
}

void read_member(Reader& in, PartyMember& member) {
  member.char_id = in.int_value();
  member.name = in.text();
  member.own_name = in.text();
  for (auto* stats : {&member.stat, &member.snap, &member.pending, &member.lv99, &member.base}) in.values(*stats);
  member.hp = in.int_value();
  member.innate = static_cast<u8>(in.signed_value(0, 0xFF));
  for (int* value : {&member.growth_col, &member.dur, &member.rate, &member.hit_med, &member.hit_heavy, &member.kind}) *value = in.int_value();
  member.evolved = in.signed_value(0, 1) != 0;
  member.lo_persist = static_cast<u32>(in.unsigned_value(0xFFFFFFFFu));
  in.values(member.cells);
  in.bits(member.unlocked);
  in.values(member.qty);
  in.bits(member.stack);
  read_equip(in, member.eq);
}

// The save's characters are ids 0 to Pip's.
constexpr int kLastPlayable = kPipCharId;
// What a record holds after any fight: max HP 1 to 999, the other stats 0 to 127, the growth words likewise.
constexpr int kMaxHp = 999, kMaxStat = 127;
// A record's kind byte: the playable bodies and the party override slots end here.
constexpr int kLastBody = 0x3E;
constexpr int kFirstTech = 0x80;
constexpr size_t kMaxStackCopies = 5;
constexpr int kItemWeapon = 0, kItemArmour = 1, kItemAccessory = 2;

// What the records allow one character, in every record set (some techs are only in the later sets): its member, the grid of the body it
// wears (Serge in Lynx's body holds Lynx's techs), and for Pip every form's base stats, Lv 99 targets and growth column.
struct RecordValues {
  std::vector<PartyMember> members;
  // The members and the body's own records: the grids whose cells and techs the member may hold.
  std::vector<PartyMember> grids;
  std::vector<std::array<int, 7>> bases, lv99s;
  std::vector<int> growth_cols;
};

template <class T>
bool one_of(const std::vector<T>& values, const T& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

RecordValues record_values(const data::GameTables& tables, int char_id, int kind) {
  RecordValues out;
  for (const auto& [set, records] : tables.char_sets) {
    const PartyMember member = make_party_member(tables, char_id, 0, set);
    if (kind >= 0 && kind < data::kNumChars && kind != char_id) out.grids.push_back(make_party_member(tables, kind, 0, set));
    out.bases.push_back(member.base);
    out.lv99s.push_back(member.lv99);
    out.growth_cols.push_back(member.growth_col);
    out.grids.push_back(member);
    out.members.push_back(member);
    if (char_id != kPipCharId) continue;
    for (int form = 0x2B; form <= 0x30; ++form) {
      const data::StatRec& rec = records.rec[static_cast<size_t>(form)];
      out.bases.push_back({rec.max_hp(), rec.atk(), rec.def(), rec.hit(), rec.evd(), rec.mag(), rec.mdef()});
      out.lv99s.push_back(lv99_targets(tables, form, set));
      out.growth_cols.push_back(kPipGrowthCol[static_cast<size_t>(form - 0x2B)]);
    }
  }
  return out;
}

// Well-formed UTF-8 without control characters (C0, DEL, C1).
bool clean_text(std::string_view text) {
  size_t pos = 0;
  while (pos < text.size()) {
    const u8 lead = static_cast<u8>(text[pos]);
    int extra = 0;
    u32 point = lead;
    if (lead >= 0xF0 && lead <= 0xF4) extra = 3, point = lead & 0x07;
    else if (lead >= 0xE0) extra = lead <= 0xEF ? 2 : -1, point = lead & 0x0F;
    else if (lead >= 0xC2) extra = 1, point = lead & 0x1F;
    else if (lead >= 0x80) extra = -1;
    if (extra < 0 || text.size() - pos <= static_cast<size_t>(extra)) return false;
    for (int k = 1; k <= extra; ++k) {
      const u8 next = static_cast<u8>(text[pos + static_cast<size_t>(k)]);
      if ((next & 0xC0) != 0x80) return false;
      point = point << 6 | (next & 0x3Fu);
    }
    const u32 shortest = extra == 3 ? 0x10000 : extra == 2 ? 0x800 : extra == 1 ? 0x80 : 0;
    if (point < shortest || point > 0x10FFFF || (point >= 0xD800 && point < 0xE000)) return false;
    if (point < 0x20 || (point >= 0x7F && point < 0xA0)) return false;
    pos += static_cast<size_t>(extra) + 1;
  }
  return true;
}

// An equipment slot holds nothing (0), an item of that kind the character may equip, or the record's own item.
bool item_fits(const data::GameTables& tables, int item_id, int kind, int char_id, const std::vector<int>& own) {
  if (item_id == 0 || one_of(own, item_id)) return true;
  if (tables.item_info.empty()) return tables.gear_rec(item_id) != nullptr;
  const data::ItemInfo* info = tables.item(item_id);
  return info && info->kind() == kind && info->can_equip(char_id);
}

// The gear totals are those of the equipped items: the weapon and accessories the member names, and some armour it may wear.
bool equip_fits(const data::GameTables& tables, const PartyMember& member) {
  std::vector<int> own_weapons, own_armours, own_accessories;
  for (const auto& [set, records] : tables.char_sets) {
    const data::StatRec& rec = records.rec[static_cast<size_t>(member.char_id)];
    own_weapons.push_back(rec.weapon());
    own_armours.push_back(rec.armour());
    for (int slot = 0; slot < 3; ++slot) own_accessories.push_back(rec.accessory(slot));
  }
  if (!item_fits(tables, member.eq.weapon_id, kItemWeapon, member.char_id, own_weapons)) return false;
  for (const u16 accessory : member.eq.acc)
    if (!item_fits(tables, accessory, kItemAccessory, member.char_id, own_accessories)) return false;
  const int items = std::max(static_cast<int>(tables.gear.size()), static_cast<int>(tables.item_info.size()));
  for (int armour = 0; armour < std::max(items, 1); ++armour)
    if (item_fits(tables, armour, kItemArmour, member.char_id, own_armours) && make_equip(tables, member.eq.weapon_id, armour, member.eq.acc) == member.eq)
      return true;
  return false;
}

// A player's tech (an element past the ordinary ones that the element table marks fixed to its cell; the other side's elements are not)
// in the grid row of its level, where a tech the story teaches or swaps (a weapon, a body) goes.
bool player_tech_at(const data::GameTables& tables, int row, int id) {
  if (id < kFirstTech || static_cast<size_t>(id) >= tables.elements.size()) return false;
  const data::ElemRec& tech = tables.elements[static_cast<size_t>(id)];
  return (tech.raw[1] & 0x02) && tech.level() == row + 1;
}
// A grid cell holds nothing, the element a record puts there (the starting kit, its techs; Pip's tech cells any of his techs), a player's
// tech in the row of its level, or an element a player may move: an element below the techs, not one fixed to its own cell, in its
// placement window, of the member's colour when only that colour may hold it. Consumable stacks hold up to 5 copies.
std::string cell_problem(const data::GameTables& tables, const PartyMember& member, const RecordValues& records, size_t cell) {
  const int id = member.cells[cell] & 0x3FFF;
  const auto at = [&](const char* problem) { return std::format("{} (cell {}, element 0x{:X})", problem, cell, id); };
  if (id == 0) return member.qty[cell] != 0 || member.stack[cell] ? at("an empty cell holds copies") : std::string();
  bool exists = false, own = false;
  for (const PartyMember& record : records.grids) {
    exists = exists || record.cells[cell] != 0;
    own = own || (record.cells[cell] & 0x3FFF) == id;
  }
  if (!exists) return at("an element in a cell the grid does not have");
  const int row = static_cast<int>(cell) / data::kGridCols;
  const bool pip_cell = std::find(kPipTechCells.begin(), kPipTechCells.end(), static_cast<int>(cell)) != kPipTechCells.end();
  const bool record_placed = own || (member.char_id == kPipCharId && pip_cell && pip_tech(id));
  if (!record_placed && id >= kFirstTech && !player_tech_at(tables, row, id)) return at("a tech no player holds there");
  if (!record_placed && id < kFirstTech) {
    if (static_cast<size_t>(id) >= tables.elements.size()) return at("an element no player may place");
    const data::ElemRec& element = tables.elements[static_cast<size_t>(id)];
    if (element.raw[1] & 0x02) return at("a fixed element outside its cell");
    const int level = element.level(), above = element.raw[0x12] >> 4, below = element.raw[0x12] & 15;
    if (row + 1 < level - above || row + 1 > level + below) return at("an element outside its placement window");
    if ((element.raw[3] & 0x80) && element.colour_bit() != member.innate) return at("an element of another colour");
  }
  if (member.stack[cell]) {
    const bool stacks = static_cast<size_t>(id) < tables.elements.size() && (tables.elements[static_cast<size_t>(id)].flags0() & 0x10);
    if (!stacks) return at("a stack of an element that does not stack");
    if (member.qty[cell] > kMaxStackCopies) return at("a stack of more than 5 copies");
  } else if (member.qty[cell] != 0) {
    return at("copies in a cell that is no stack");
  }
  return {};
}

std::string member_problem(const data::GameTables& tables, const PartyMember& member) {
  if (member.char_id < 0 || member.char_id > kLastPlayable) return "not a playable character";
  if (!clean_text(member.name) || !clean_text(member.own_name)) return "a name with control characters";
  const RecordValues records = record_values(tables, member.char_id, member.kind);
  if (records.members.empty()) return "no character records";
  const bool pip = member.char_id == kPipCharId;
  if (member.kind != -1 && (member.kind < 0 || member.kind > kLastBody || pip_form(member.kind) != pip)) return "a body the character cannot have";
  if (member.evolved) return "a form change before the battle";
  const auto record_constant = [&](auto field) {
    return std::any_of(records.members.begin(), records.members.end(), [&](const PartyMember& record) { return field(record) == field(member); });
  };
  if (!record_constant([](const PartyMember& entry) { return entry.dur; }) || !record_constant([](const PartyMember& entry) { return entry.rate; }) ||
      !record_constant([](const PartyMember& entry) { return entry.hit_med; }) ||
      !record_constant([](const PartyMember& entry) { return entry.hit_heavy; }))
    return "record constants that are not the character's";
  if (!one_of(records.growth_cols, member.growth_col)) return "a growth column that is not the character's";
  if (data::colour_index_of_bit(member.innate) < 0) return "an innate colour that is no colour";
  if (!one_of(records.bases, member.base) || !one_of(records.lv99s, member.lv99)) return "a growth record that is not the character's";
  for (size_t k = 0; k < 7; ++k) {
    const int cap = k == 0 ? kMaxHp : kMaxStat;
    if (member.stat[k] < (k == 0 ? 1 : 0) || member.stat[k] > cap) return "a stat past the record limits";
    if (member.snap[k] < 0 || member.snap[k] > cap || member.pending[k] < -cap || member.pending[k] > cap) return "growth words past the record limits";
  }
  if (member.hp < 0 || member.hp > member.stat[0]) return "HP past max HP";
  if (member.lo_persist & ~st::kPersist) return "a status a record does not keep";
  if (!equip_fits(tables, member)) return "gear that is not the equipped items'";
  for (size_t cell = 0; cell < data::kGridCells; ++cell)
    if (std::string problem = cell_problem(tables, member, records, cell); !problem.empty()) return problem;
  return {};
}

}  // namespace

bool legal_versus_side(const data::GameTables& tables, const VersusSide& side, std::string* why) {
  const auto no = [&](const std::string& reason) {
    if (why) *why = reason;
    return false;
  };
  if (side.party.empty() || side.party.size() > kPcSlots) return no("not 1 to 3 characters");
  for (size_t i = 0; i < side.party.size(); ++i) {
    for (size_t k = 0; k < i; ++k)
      if (side.party[k].char_id == side.party[i].char_id) return no("a character twice");
    if (const std::string problem = member_problem(tables, side.party[i]); !problem.empty()) return no(std::format("member {}: {}", i, problem));
  }
  const int last_star = tables.growth.empty() ? 0 : static_cast<int>(tables.growth.size()) - 1;
  if (side.stars < 0 || side.stars > last_star) return no("more stars than the game has");
  for (size_t slot = kDoppleTable.size(); slot < side.dopple.size() * 16; ++slot)
    if (side.dopple[slot >> 4] >> (slot & 15) & 1u) return no(std::format("a creature the pool cannot hold (slot {})", slot));
  return true;
}

std::string encode_versus_side(const VersusSide& side) {
  Writer out;
  out.unsigned_value(kVersusSideFormat);
  out.signed_value(side.stars);
  out.values(side.dopple);
  out.unsigned_value(side.party.size());
  for (const PartyMember& member : side.party) write_member(out, member);
  return out.take();
}

std::optional<VersusSide> decode_versus_side(std::string_view bytes) {
  Reader in(bytes);
  if (in.unsigned_value(0xFF) != kVersusSideFormat) return std::nullopt;
  VersusSide side;
  side.stars = in.int_value();
  in.values(side.dopple);
  side.party.resize(static_cast<size_t>(in.unsigned_value(kPcSlots)));
  for (PartyMember& member : side.party) read_member(in, member);
  if (!in.ok() || !in.at_end()) return std::nullopt;
  return side;
}

int versus_battlefield(const data::GameTables& tables, u32 seed) {
  std::vector<int> fields;
  for (const data::SceneRec& scene : tables.scenes) {
    bool enemies = false;
    for (int group = 0; group < 3; ++group) enemies = enemies || (scene.enemy_id(group) != 0xFF && scene.enemy_count(group) != 0);
    if (enemies && !scene.boss() && scene.bg() < 100) fields.push_back(scene.bg());
  }
  std::sort(fields.begin(), fields.end());
  fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
  if (fields.empty()) return -1;
  GameRng pick(seed * kFieldPickSalt + 1u);
  return fields[static_cast<size_t>(pick.rnd(0, static_cast<int>(fields.size()) - 1))];
}

VersusSide versus_test_side(const data::GameTables& tables, const std::vector<int>& char_ids, int star) {
  VersusSide side;
  for (const int id : char_ids)
    if (side.party.size() < kPcSlots) side.party.push_back(make_party_member(tables, id, star));
  side.stars = kVersusTestStars;
  for (size_t row = 0; row < kDoppleTable.size(); ++row)
    if (kDoppleTable[row].val != 0) side.dopple[row >> 4] = static_cast<u16>(side.dopple[row >> 4] | (1u << (row & 15)));
  return side;
}

int versus_carrier_scene(const data::GameTables& tables, int battlefield) {
  for (size_t id = 0; id < tables.scenes.size(); ++id) {
    const data::SceneRec& scene = tables.scenes[id];
    bool enemies = false;
    for (int group = 0; group < 3; ++group) enemies = enemies || (scene.enemy_id(group) != 0xFF && scene.enemy_count(group) != 0);
    if (enemies && !scene.boss() && scene.bg() == battlefield) return static_cast<int>(id);
  }
  return -1;
}

std::vector<Command> VersusAutoPlayer::next(const Battle& battle) {
  const int side = battle.turn_side();
  std::vector<int> actors;
  for (int unit = side_first(side); unit < side_first(side) + kPcSlots; ++unit)
    if (battle.can_act(unit)) actors.push_back(unit);
  Command cmd;
  cmd.actor = actors.empty() ? side_first(side) : pick(actors);
  if (actors.empty() || (rng_.rnd(0, 99) < 12 && options_.early_end)) {
    cmd.kind = Command::Kind::EndTurn;
    cmd.timeout = rng_.rnd(0, 9) == 0 && options_.timeouts;
    return {cmd};
  }
  std::vector<Command> out;
  if (cmd.actor != focused_) {
    Command focus;
    focus.kind = Command::Kind::Focus;
    focus.actor = focused_ = cmd.actor;
    out.push_back(focus);
  }
  const int roll = rng_.rnd(0, 1999);
  if (roll == 0 && options_.surrender) {
    cmd.kind = Command::Kind::Run;
  } else if (roll < 800) {
    attack(battle, cmd);
  } else if (roll < 1500 && element(battle, cmd)) {
    Command menu;
    menu.kind = Command::Kind::ElementMenu;
    menu.actor = cmd.actor;
    out.push_back(menu);
  } else {
    cmd.kind = Command::Kind::Defend;
  }
  out.push_back(cmd);
  if (cmd.kind == Command::Kind::Element && battle.cell_element(cmd.actor, cmd.cell) == kDopplegangElement) {
    const std::vector<int> pool = battle.dopple_pool(side);
    Command form;
    form.kind = Command::Kind::Form;
    form.actor = cmd.actor;
    form.form = battle.unit(cmd.actor).form >= 0 && rng_.rnd(0, 3) == 0 ? -1 : pool.empty() ? -1 : pick(pool);
    out.push_back(form);
  }
  return out;
}

int VersusAutoPlayer::pick(const std::vector<int>& items) { return items[static_cast<size_t>(rng_.rnd(0, static_cast<int>(items.size()) - 1))]; }

std::vector<int> VersusAutoPlayer::standing(const Battle& battle, int side) const {
  std::vector<int> units;
  for (int unit = side_first(side); unit < side_first(side) + kPcSlots; ++unit)
    if (battle.unit(unit).standing()) units.push_back(unit);
  return units;
}

void VersusAutoPlayer::attack(const Battle& battle, Command& cmd) {
  cmd.kind = Command::Kind::Attack;
  const int length = rng_.rnd(1, 4);
  for (int i = 0; i < length; ++i) cmd.chain.push_back(rng_.rnd(0, 2));
  while (cmd.chain.size() > 1 && !battle.validate(cmd)) cmd.chain.pop_back();
  if (!battle.validate(cmd)) cmd.chain = {0};
  // A fatigued character can have the menu without the stamina for a weak hit.
  if (!battle.validate(cmd)) {
    cmd.kind = Command::Kind::Defend;
    cmd.chain.clear();
    return;
  }
  const std::vector<int> foes = standing(battle, 1 - side_of(cmd.actor));
  cmd.target = rng_.rnd(0, 1) == 0 || foes.empty() ? -1 : pick(foes);
}

bool VersusAutoPlayer::element(const Battle& battle, Command& cmd) {
  std::vector<int> cells;
  for (int cell = 0; cell < data::kGridCells; ++cell)
    if (battle.element_usable(cmd.actor, cell)) cells.push_back(cell);
  if (cells.empty()) return false;
  cmd.kind = Command::Kind::Element;
  cmd.cell = pick(cells);
  const int id = battle.cell_element(cmd.actor, cmd.cell);
  const TargetSpec spec = target_spec(battle.tables().elements[static_cast<size_t>(id)]);
  std::vector<int> targets;
  for (int unit = 0; unit < kPlayerUnits; ++unit) {
    const Unit& target = battle.unit(unit);
    const bool own = side_of(unit) == side_of(cmd.actor);
    if (!target.present || (own ? !spec.party : !spec.enemies) || (spec.self_only && unit != cmd.actor)) continue;
    if (spec.ko_only ? (target.lo & st::kKO) != 0 : target.standing()) targets.push_back(unit);
  }
  cmd.target = rng_.rnd(0, 3) == 0 || targets.empty() ? -1 : pick(targets);
  return true;
}

}  // namespace oc::battle
