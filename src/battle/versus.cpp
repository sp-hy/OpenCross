// The versus side exchange format (one format byte, then the stars, the pool and each member's fields in a fixed order), the
// battlefield and carrier scene picks, and the random legal player.
#include "battle/versus.h"

#include <algorithm>
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

}  // namespace

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
