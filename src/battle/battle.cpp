#include "battle/battle.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <format>
#include <tuple>
#include <utility>

namespace oc::battle {

namespace {

constexpr int kMaxChain = 4;
constexpr int kVaryInnateElement = 336;
constexpr int kChronoCrossElement = 0x7E;
// Lo statuses that keep a PC out of a double / triple tech (KO, confusion, defeat ...).
constexpr u32 kComboPartnerBlockLo = 0x203000C0u;

// The shiny material a summon kill gives, by colour index: Leaf, Salt, Soot, Ember, Sand, Dew.
constexpr std::array<u16, 6> kShinyMaterial = {0x13, 0x15, 0x16, 0x11, 0x14, 0x12};

// An action's target PC has its colour-seal flags judged again once the action has run, whatever the action was.
struct SealRefreshOnExit {
  Battle& battle;
  int target;
  ~SealRefreshOnExit() { battle.refresh_seal_flags(target); }
};

// The armour colour byte's x150/100 step looks its colour MASK up in the six-entry opposite-colour table that is indexed by colour index
// everywhere else: the green, white and black masks (1, 2, 4) land inside it and raise black, white and green elements; red and yellow (8, 16)
// read the multiplier table's low bytes (0x99, 0x65: never a colour); blue (32) reads a flag that is 0 whenever damage is computed: green.
int armour_boost_colour(u8 armour_colour) {
  if (!(armour_colour & 2)) return -1;
  const int mask = armour_colour >> 2;
  if (mask == 0x20) return 0;
  return mask < 6 ? kOpp[static_cast<size_t>(mask)] : -1;
}

int stat_hp_cap(int value) { return std::min(value, 999); }

// STUB starting kit: the populated cells of grid rows 0..1 are unlocked whatever the star unlocks say.
void unlock_starting_kit(PartyMember& member) {
  for (int cell = 0; cell < data::kGridCells; ++cell)
    if (cell / data::kGridCols <= 1 && (member.cells[static_cast<size_t>(cell)] & 0x3FFF) != 0) member.unlocked[static_cast<size_t>(cell)] = true;
}

Equip equip_of(const data::GameTables& tables, const data::StatRec& rec) {
  return make_equip(tables, rec.weapon(), rec.armour(),
                    {static_cast<u16>(rec.accessory(0)), static_cast<u16>(rec.accessory(1)), static_cast<u16>(rec.accessory(2))});
}

// A grid cell is usable when it holds a valid element id.
void mark_usable_cells(Unit& unit, size_t element_count) {
  for (size_t cell = 0; cell < data::kGridCells; ++cell) {
    const int id = unit.cells[cell] & 0x3FFF;
    unit.usable[cell] = id != 0 && id < static_cast<int>(element_count);
  }
}

// The hero set: weapon 0x3D (Hero's Blade) and two accessories that are 0xC1 (Hero's Shield) or 0xC0 (Hero's Medal).
bool wears_hero_set(const Equip& eq) {
  if (eq.weapon_id != 0x3D) return false;
  return std::count_if(eq.acc.begin(), eq.acc.end(), [](u16 id) { return id == 0xC0 || id == 0xC1; }) == 2;
}

// The attacker's weapon term of every weapon hit, weapon tech and ATK-based element: weapon power through Att%, then +10 % per hi
// status bit 29..31 level, doubled by the hero set.
int weapon_term(const Unit& unit) {
  const int term = ceil100(ceil100(unit.eq.wp, unit.mod[0]), 100 + 10 * static_cast<int>(unit.hi >> 29));
  return unit.hero_set ? term * 2 : term;
}

// Element mode 13 moves these lo statuses from the caster to the target.
constexpr u32 kRevengeStatuses = 0xAA9EED39u;

}  // namespace

// ------------------------------------------------------------------------------------------------ party setup
void unlock_grid(PartyMember& member, const data::GameTables& tables, int star) {
  const int last = std::min<int>(star, static_cast<int>(tables.growth.size()) - 1);
  for (int row = 0; row <= last; ++row) {
    const int cell = tables.growth[static_cast<size_t>(row)][static_cast<size_t>(member.growth_col & 7)];
    if (cell == 0xFF || cell >= data::kGridCells) continue;
    if (member.cells[static_cast<size_t>(cell)] != 0) member.unlocked[static_cast<size_t>(cell)] = true;
  }
}

Equip make_equip(const data::GameTables& tables, int weapon, int armour, const std::array<u16, 3>& acc) {
  Equip equip;
  equip.weapon_id = weapon;
  equip.acc = acc;
  const data::GearRec* weapon_rec = tables.gear_rec(weapon);
  const data::GearRec* armour_rec = tables.gear_rec(armour);
  if (tables.gear.empty() || (!weapon_rec && !armour_rec)) return equip;
  equip.wp = equip.wp_acc = equip.wp_var = equip.wp_class = equip.arm = equip.arm_var = equip.mag_gear = equip.mdef_gear = equip.evd_gear = 0;
  auto add_flags = [&](const data::GearRec& rec) { equip.gear_flags |= static_cast<u16>(rec.flags() & 0xF); };
  if (weapon_rec) {
    equip.wp = weapon_rec->power(), equip.wp_acc = weapon_rec->acc(), equip.wp_var = weapon_rec->var(), equip.wp_class = weapon_rec->klass(),
    equip.mag_gear = weapon_rec->mag();
    equip.slayer = weapon_rec->mask_hi();
    equip.status_lo |= weapon_rec->status_lo(), equip.status_hi |= weapon_rec->status_hi();
    add_flags(*weapon_rec);
  }
  if (armour_rec) {
    equip.arm = armour_rec->power(), equip.arm_var = armour_rec->var(), equip.mdef_gear = armour_rec->mag(), equip.evd_gear = armour_rec->acc();
    equip.resist_lo |= armour_rec->mask_lo(), equip.resist_hi |= armour_rec->mask_hi();
    equip.guard_hi |= armour_rec->mask_hi();
    equip.armour_colour = armour_rec->colour();
    equip.status_lo |= armour_rec->status_lo(), equip.status_hi |= armour_rec->status_hi();
    add_flags(*armour_rec);
  }
  for (u16 id : acc) {
    const data::GearRec* rec = tables.gear_rec(id);
    if (!rec || id == 0) continue;
    const u16 flags = rec->flags();
    add_flags(*rec);
    equip.status_lo |= rec->status_lo(), equip.status_hi |= rec->status_hi();
    if (flags & data::gearflag::kOffence) equip.wp += rec->power(), equip.wp_acc += rec->acc(), equip.mag_gear += rec->mag();
    if (flags & data::gearflag::kDefence) {
      equip.arm += rec->power(), equip.mdef_gear += rec->mag(), equip.evd_gear += rec->acc();
      equip.resist_lo |= rec->mask_lo(), equip.resist_hi |= rec->mask_hi();
      equip.guard_hi |= rec->mask_hi();
    }
    if (flags & data::gearflag::kHpPct) equip.hp_pct.push_back(rec->power());
    if (flags & data::gearflag::kStamina) equip.stamina_mod += rec->mag();
    if (flags & data::gearflag::kStartGauge) equip.start_gauge += rec->power();
  }
  return equip;
}

// The pending star-up deltas are rolled here with a per-character seed: the minor growth of every fight is
// 0.5 * ((snap + 0.7 * pending) - cur) * sqrt(P / S), so a member without them would never grow. The session uses the same seed rule for a
// fresh save record.
PartyMember make_party_member(const data::GameTables& tables, int char_id, int star, const std::string& set) {
  PartyMember member;
  const auto& chars = tables.chars(set);
  const auto& rec = chars.rec[static_cast<size_t>(char_id)];
  const auto& lv99 = chars.lv99[static_cast<size_t>(char_id)];
  member.char_id = char_id;
  member.name = tables.char_name(char_id);
  member.stat = {rec.max_hp(), rec.atk(), rec.def(), rec.hit(), rec.evd(), rec.mag(), rec.mdef()};
  member.lv99 = {lv99.hp, lv99.atk, lv99.def, lv99.hit, lv99.evd, lv99.mag, lv99.mdef};
  member.hp = member.stat[0];
  member.snap = member.stat;
  member.innate = rec.innate();
  member.growth_col = rec.growth_col();
  member.dur = rec.status_dur();
  member.rate = rec.stamina_rate();
  member.hit_med = rec.hit_med();
  member.hit_heavy = rec.hit_heavy();
  member.cells = rec.grid();
  unlock_starting_kit(member);
  member.eq = equip_of(tables, rec);
  unlock_grid(member, tables, star);
  GameRng prng(0x5EED0000u + static_cast<u32>(char_id) * 977u);
  member.pending[0] = next_pending_hp(prng, member.stat[0], member.lv99[0], star);
  for (size_t k = 1; k < 7; ++k) member.pending[k] = next_pending_stat(prng, member.stat[k], member.lv99[k], star);
  return member;
}

// Pip's tech cells follow his CURRENT form whatever the record holds: ids 0x11F / 0x121 / 0x123 become the form's first tech,
// 0x120 / 0x122 / 0x124 its second, and his 18 techs take his innate colour. A gear HP% flag scales max HP by (100 + n) / 100 (+1 when
// that changes nothing), cap 999.
void Battle::init_pc(int slot, const PartyMember& member) {
  Unit& unit = units_[static_cast<size_t>(slot)];
  unit = Unit{};
  unit.present = unit.pc = true;
  unit.src_id = member.char_id;
  unit.name = member.name;
  unit.max_hp = member.stat[0];
  unit.hp = std::min(member.hp, member.stat[0]);
  unit.atk = member.stat[1], unit.def = member.stat[2], unit.hit = member.stat[3], unit.evd = member.stat[4], unit.mag = member.stat[5],
  unit.mdef = member.stat[6];
  unit.hit_med = member.hit_med, unit.hit_heavy = member.hit_heavy;
  unit.innate = member.innate;
  unit.dur = member.dur, unit.rate = member.rate;
  unit.ap = 8, unit.countdown = 8, unit.stamina = 70, unit.gauge = -1;
  unit.lo = member.lo_persist & st::kPersist;
  unit.cells = member.cells;
  unit.qty = member.qty;
  unit.kind = member.kind >= 0 ? member.kind : member.char_id;
  if (pip_form(unit.kind)) {
    const size_t tech_base = static_cast<size_t>(unit.kind - 0x2B) * 3;
    for (u16& cell : unit.cells) {
      const int id = cell & 0x3FFF;
      if (id == 0x11F || id == 0x121 || id == 0x123) cell = static_cast<u16>((cell & 0xC000) | kPipTechs[tech_base]);
      else if (id == 0x120 || id == 0x122 || id == 0x124) cell = static_cast<u16>((cell & 0xC000) | kPipTechs[tech_base + 1]);
    }
    pip_tech_colour_ = member.innate;
  }
  // The gauge tops out at the deepest grid row with an unlocked cell holding an element; -1 (no gauge) when there is none.
  unit.gauge_max = -1;
  for (size_t cell = 0; cell < data::kGridCells; ++cell) {
    const int id = member.cells[cell] & 0x3FFF;
    unit.usable[cell] = member.unlocked[cell] && id != 0 && id < static_cast<int>(tables_.elements.size());
    if (member.unlocked[cell] && (unit.cells[cell] & 0x3FFF) != 0) unit.gauge_max = static_cast<int>(cell) / data::kGridCols;
  }
  unit.eq = member.eq;
  unit.perm_lo = unit.eq.status_lo, unit.perm_hi = unit.eq.status_hi;
  unit.skip_lo = unit.eq.resist_lo, unit.skip_hi = unit.eq.resist_hi;
  unit.lo |= unit.perm_lo, unit.hi |= unit.perm_hi;
  unit.hero_set = wears_hero_set(unit.eq);
  if (const auto chars = tables_.char_sets.find("char"); chars != tables_.char_sets.end() && member.char_id >= 0 && member.char_id < data::kNumChars)
    unit.gender = static_cast<u16>(chars->second.rec[static_cast<size_t>(member.char_id)].rd16(0x14));
  unit.rate = std::max(0, unit.rate + unit.eq.stamina_mod);
  for (int pct : unit.eq.hp_pct) {
    const int scaled = unit.max_hp * (pct + 100) / 100;
    unit.max_hp = std::min(999, scaled == unit.max_hp ? scaled + 1 : scaled);
  }
  unit.gauge = std::clamp(-1 + unit.eq.start_gauge, -1, unit.gauge_max);
  unit.hp = std::min(member.hp >= member.stat[0] ? unit.max_hp : member.hp, unit.max_hp);
  if (unit.hp <= 0) {
    unit.hp = 0;
    unit.lo |= st::kKO;
  }
}

// The fields a unit takes from a monster record (enemy spawn and a Dopplegang form): stats, colour, status words (`perm_lo_bits` are OR-ed
// into the permanent lo word), weakness / skip masks, element grid and gear.
void Battle::load_monster_record(Unit& unit, const data::StatRec& rec, u32 perm_lo_bits) const {
  unit.perm_lo = rec.rd32(0xA8) | perm_lo_bits;
  unit.perm_hi = rec.rd32(0xAC);
  unit.weak_lo = rec.rd32(0x04), unit.weak_hi = rec.rd32(0x08);
  unit.lo = unit.perm_lo, unit.hi = unit.perm_hi;
  unit.atk = rec.atk(), unit.def = rec.def(), unit.hit = rec.hit(), unit.evd = rec.evd(), unit.mag = rec.mag(), unit.mdef = rec.mdef();
  unit.hit_med = rec.hit_med(), unit.hit_heavy = rec.hit_heavy();
  unit.innate = rec.innate();
  unit.dur = rec.status_dur();
  unit.rate = rec.stamina_rate();
  unit.cells = rec.grid();
  mark_usable_cells(unit, tables_.elements.size());
  unit.eq = equip_of(tables_, rec);
  unit.skip_lo = rec.rd32(0x0C) | unit.eq.resist_lo, unit.skip_hi = rec.rd32(0x10) | unit.eq.resist_hi;
}

// Max HP: NG+ doubles it (cap 30000, not in the final battle), a rolled / recorded gender scales it x105/100 (male) or x95/100 (female);
// the unit HP field is a signed 16-bit word, so it is clamped to 32767. AP gets + rnd(0, AP / 4); the first countdown is rnd(0 or 1, AP).
void Battle::init_enemy(int slot, int monster_id, const BattleOptions& opt) {
  Unit& unit = units_[static_cast<size_t>(slot)];
  const auto& rec = tables_.enemies[static_cast<size_t>(monster_id)];
  unit = Unit{};
  unit.present = true;
  unit.pc = false;
  unit.src_id = monster_id;
  unit.name = tables_.enemy_name(monster_id);
  if (unit.name.empty()) unit.name = std::format("monster{}", monster_id);
  int max_hp = std::min<int>(rec.max_hp(), 32767);
  if (opt.ng_plus && scene_ != 0x1DD) max_hp = std::min(max_hp * 2, 30000);
  int gender = rec.gender() & 0xC0;
  if (gender == 0) gender = rng_.rnd(1, 2) << 6;
  if (gender & 0x40) max_hp = max_hp * 0x69 / 100;
  else if (gender & 0x80) max_hp = std::max(1, max_hp * 0x5F / 100);
  max_hp = std::min(max_hp, 32767);
  unit.max_hp = unit.hp = max_hp;
  unit.gender = static_cast<u16>((rec.rd16(0x14) & 0x3FFF) | (gender << 8));
  load_monster_record(unit, rec, 0x400u);
  if (monster_party_) unlock_monster_grid(unit);
  // The gauge tops out at the deepest grid row holding an element in a cell without the lock bit (0x4000); every stock monster cell
  // carries that bit, so an enemy gauge stays at -1.
  unit.gauge_max = -1;
  for (size_t cell = 0; cell < data::kGridCells; ++cell)
    if (!(unit.cells[cell] & 0x4000) && (unit.cells[cell] & 0x7FFF) != 0) unit.gauge_max = static_cast<int>(cell) / data::kGridCols;
  int ap = rec.ap();
  ap += rng_.rnd(0, ap >> 2);
  unit.ap = ap;
  unit.countdown = rng_.rnd(scene_ > 0x17F ? 1 : 0, ap);
  if (tables_.gear.empty()) unit.eq.wp = 22;
  unit.gold = rec.gold();
  unit.drop_row = rec.drop_row();
  unit.kind = rec.model();
  unit.drop_base = rec.drop_base();
}

// A monster-party scene: every monster record's grid cells of rows 1..7 outside column 8 lose their flag bits (0xC000), so the
// elements in them can be cast; row 0 and column 8 keep theirs.
void Battle::unlock_monster_grid(Unit& unit) const {
  for (int row = 1; row < data::kGridRows; ++row)
    for (int col = 0; col < data::kGridCols; ++col)
      if (col != 8) unit.cells[static_cast<size_t>(row * data::kGridCols + col)] &= 0x3FFF;
}

// A party slot of a monster-party scene: the whole monster record (stats, colour, status words with lo 0x400400, weakness / skip masks,
// grid, gear, name, record gender) under the slot's character id, shown as the creature (form = the record). Max HP = the record's
// (NG+ doubles it outside the final battle, cap 30000), then capped at 999 like the current HP. AP and the first countdown 8, stamina 70;
// the gauge starts at -1 plus the gear's start bonus, its top is the deepest row with an element in an unlocked cell (the usable cells).
void Battle::init_party_monster(int slot, int monster_id, const BattleOptions& opt) {
  Unit& unit = units_[static_cast<size_t>(slot)];
  const int char_id = unit.src_id;
  const auto& rec = tables_.enemies[static_cast<size_t>(monster_id)];
  unit = Unit{};
  unit.present = unit.pc = true;
  unit.src_id = char_id;
  unit.form = monster_id;
  unit.name = tables_.enemy_name(monster_id);
  if (unit.name.empty()) unit.name = std::format("monster{}", monster_id);
  int max_hp = rec.max_hp();
  if (opt.ng_plus && scene_ != 0x1DD) max_hp = std::min(max_hp * 2, 30000);
  unit.max_hp = unit.hp = std::min(max_hp, 999);
  unit.gender = rec.rd16(0x14);
  load_monster_record(unit, rec, 0x400400u);
  unlock_monster_grid(unit);
  unit.gauge_max = -1;
  for (size_t cell = 0; cell < data::kGridCells; ++cell) {
    const int id = unit.cells[cell] & 0x3FFF;
    unit.usable[cell] = id != 0 && !(unit.cells[cell] & 0x4000) && id < static_cast<int>(tables_.elements.size());
    if (unit.usable[cell]) unit.gauge_max = static_cast<int>(cell) / data::kGridCols;
  }
  unit.ap = 8, unit.countdown = 8, unit.stamina = 70;
  unit.gauge = std::clamp(-1 + unit.eq.start_gauge, -1, unit.gauge_max);
  unit.kind = rec.model();
}

bool Battle::start(std::vector<PartyMember> party, int scene, const BattleOptions& opt) {
  if (scene < 0 || scene >= static_cast<int>(tables_.scenes.size())) return false;
  if (party.empty() || party.size() > kPcSlots) return false;
  scene_ = scene;
  opt_ = opt;
  growth_ = opt.growth;
  outcome_ = Outcome::Ongoing;
  result_code_ = -1;
  attacked_.clear();
  kill_script_run_ = {};
  summon_kills_ = {};
  summon_colour_ = -1;
  field_state_ = 0;
  trap_ = {};
  trap_state_ = 0;
  trap_before_ = {};
  ring_repainted_ = fizzled_ = false;
  vary_innate_colour_ = 0;
  rewards_ = {};
  log_.clear();
  actions_ = 0;
  cursor_ = 0;
  reset_chain();
  units_ = {};
  party_ = std::move(party);
  colour_row_ = std::any_of(party_.begin(), party_.end(), [](const PartyMember& member) {
    return std::any_of(member.cells.begin(), member.cells.end(), [](u16 cell) { return (cell & 0x7FFF) == kChronoCrossElement; });
  });
  bench_ = opt.bench;
  dopple_mask_ = opt.dopple_mask;
  const auto& scene_rec = tables_.scenes[static_cast<size_t>(scene)];
  boss_ = scene_rec.boss();
  cast_mask_ = ~(static_cast<u32>(scene_rec.raw[0x1B]) << 3) & 0x1FFu;

  // Field ring: the btbgdat row of the stage the fight is drawn on, i.e. the battlefield operand when it names a row, else the scene's
  // background.
  ring_ = {-1, -1, -1};
  const bool operand_row = opt.battlefield >= 0 && static_cast<size_t>(opt.battlefield) < tables_.btbgdat.size();
  const size_t ring_bg = operand_row ? static_cast<size_t>(opt.battlefield) : static_cast<size_t>(scene_rec.bg());
  if (ring_bg < tables_.btbgdat.size())
    for (size_t k = 0; k < 3; ++k) {
      const int colour = tables_.btbgdat[ring_bg][k];
      ring_[k] = colour < 6 ? colour : -1;
    }

  for (size_t i = 0; i < party_.size(); ++i) init_pc(static_cast<int>(i), party_[i]);
  // Monster-party scene: each party slot takes its pick, a slot without one stays empty. Without any pick (dev tools) the party fights.
  monster_party_ = (scene_rec.raw[0x0F] & 1) &&
                   std::any_of(opt.monster_party.begin(), opt.monster_party.end(), [&](int record) {
                     return record >= 0 && static_cast<size_t>(record) < tables_.enemies.size();
                   });
  if (monster_party_)
    for (size_t i = 0; i < party_.size(); ++i) {
      const int record = opt.monster_party[i];
      if (record >= 0 && static_cast<size_t>(record) < tables_.enemies.size()) init_party_monster(static_cast<int>(i), record, opt);
      else units_[i] = Unit{};
    }
  int slot = kPcSlots;
  for (int group = 0; group < 3; ++group) {
    if (scene_rec.enemy_id(group) == 0xFF || scene_rec.enemy_count(group) == 0) continue;
    if (scene_rec.enemy_id(group) >= tables_.enemies.size()) continue;
    for (int copy = 0; copy < scene_rec.enemy_count(group) && slot < kUnits; ++copy) {
      init_enemy(slot, scene_rec.enemy_id(group), opt);
      // The instances of a group of more than one are numbered 1, 2, 3 ... (letters A, B, C ...) in slot order, restarting per group; a
      // group of one gets none.
      if (scene_rec.enemy_count(group) > 1) units_[static_cast<size_t>(slot)].letter = static_cast<u8>(copy + 1);
      ++slot;
    }
  }
  if (slot == kPcSlots) return false;
  for (int pc = 0; pc < kPcSlots; ++pc) refresh_seal_flags(pc);
  say(std::format("battle start: scene {}  ring [{} {} {}]", scene, data::colour_name(ring_[0]), data::colour_name(ring_[1]),
                  data::colour_name(ring_[2])));
  brain_.begin_battle(*this, rng_);
  settle();
  return true;
}

// ------------------------------------------------------------------------------------------------ queries
int Battle::alive_count(bool pcs) const {
  int count = 0;
  for (int i = pcs ? 0 : kPcSlots; i < (pcs ? kPcSlots : kUnits); ++i)
    if (units_[static_cast<size_t>(i)].alive()) ++count;
  return count;
}

// Rainbow (0xFC) and colourless cells are never sealed.
// A sealed cell is judged by the element it casts now (a tech that became a double / triple tech takes that tech's colour).
bool Battle::element_sealed(int pc, int cell) const {
  if (!is_pc_slot(pc) || cell < 0 || cell >= data::kGridCells) return false;
  return colour_sealed(pc, cell_element(pc, cell));
}

bool Battle::colour_sealed(int pc, int id) const {
  if (id <= 0 || static_cast<size_t>(id) >= tables_.elements.size()) return false;
  const int colour = data::colour_index_of_bit(element_colour(id) & 0xFC);
  return colour >= 0 && (unit(pc).lo & (0x800u << (4 * colour))) != 0;
}

// The partner's cell for a double / triple tech: the first open grid cell whose word is exactly `tech`; it counts only while that cell is
// unused and its seal flag is clear, the partner's gauge reaches the cell's row, it has 1.0 stamina and none of the lo statuses 0x203000C0.
int Battle::combo_partner_cell(int pc, int tech) const {
  const Unit& partner = unit(pc);
  if (!partner.present || !partner.pc) return -1;
  for (int cell = 0; cell < data::kGridCells; ++cell) {
    if (!partner.usable[static_cast<size_t>(cell)] || partner.cells[static_cast<size_t>(cell)] != tech) continue;
    if (partner.used[static_cast<size_t>(cell)] || partner.seal_flag[static_cast<size_t>(cell)]) return -1;
    if (partner.gauge < cell / data::kGridCols || partner.stamina < 10 || (partner.lo & kComboPartnerBlockLo)) return -1;
    return cell;
  }
  return -1;
}

// A tech record names its double-tech partner tech at +0x20 (the result at +0x26), or its two triple-tech partner techs at +0x22 / +0x24
// (the result at +0x28). The other two party slots are searched in slot order: a double tech takes the first of them that holds the partner
// tech; a triple tech needs each of them to hold one of its two partner techs. The casting PCs are the record's character bytes +0x14..+0x16
// matched against the party; each gives up the cell that offered the tech.
ComboTech Battle::combo_at(int pc, int cell) const {
  ComboTech combo;
  if (!is_pc_slot(pc) || cell < 0 || cell >= data::kGridCells) return combo;
  const Unit& caster = unit(pc);
  const int id = caster.cells[static_cast<size_t>(cell)] & 0x3FFF;
  if (!caster.usable[static_cast<size_t>(cell)] || id <= 0 || static_cast<size_t>(id) >= tables_.elements.size()) return combo;
  const data::ElemRec& tech = tables_.elements[static_cast<size_t>(id)];
  const std::array<int, 2> others{pc == 0 ? 1 : 0, pc == 2 ? 1 : 2};
  std::array<int, kPcSlots> given{-1, -1, -1};
  given[static_cast<size_t>(pc)] = cell;
  if (tech.rd16(0x22) == 0) {
    const int partner_tech = tech.rd16(0x20);
    if (partner_tech == 0) return combo;
    for (int other : others) {
      const int partner_cell = combo_partner_cell(other, partner_tech);
      if (partner_cell < 0) continue;
      given[static_cast<size_t>(other)] = partner_cell;
      combo.element = tech.rd16(0x26);
      break;
    }
  } else {
    for (int other : others) {
      int partner_cell = -1;
      for (const size_t offset : {size_t{0x22}, size_t{0x24}}) {
        const int found = combo_partner_cell(other, tech.rd16(offset));
        if (found >= 0) partner_cell = found;
      }
      if (partner_cell < 0) return combo;
      given[static_cast<size_t>(other)] = partner_cell;
    }
    combo.element = tech.rd16(0x28);
  }
  if (combo.element <= 0 || static_cast<size_t>(combo.element) >= tables_.elements.size()) return ComboTech{};
  combo.slot_cells = given;
  const data::ElemRec& result = tables_.elements[static_cast<size_t>(combo.element)];
  for (size_t k = 0; k < combo.units.size(); ++k) {
    const int char_id = result.b(0x14 + k);
    if (char_id == 0xFF) continue;
    for (int slot = 0; slot < kPcSlots; ++slot)
      if (given[static_cast<size_t>(slot)] >= 0 && unit(slot).src_id == char_id) {
        combo.units[k] = slot;
        combo.cells[k] = given[static_cast<size_t>(slot)];
      }
  }
  return combo;
}

// The element menu of a PC. Every PC's table starts again from its own cells; each cell of `pc` that offers a double / triple tech puts
// it into its own cell and into the cell each partner gives up, and `pc`'s seal flags are judged anew by what its cells show. The
// partners' flags stay as they were.
void Battle::open_element_menu(int pc) {
  if (!is_pc_slot(pc)) return;
  for (int slot = 0; slot < kPcSlots; ++slot) units_[static_cast<size_t>(slot)].combo_word.fill(0);
  for (int cell = 0; cell < data::kGridCells; ++cell) {
    const ComboTech combo = combo_at(pc, cell);
    if (!combo.element) continue;
    for (int slot = 0; slot < kPcSlots; ++slot)
      if (combo.slot_cells[static_cast<size_t>(slot)] >= 0)
        units_[static_cast<size_t>(slot)].combo_word[static_cast<size_t>(combo.slot_cells[static_cast<size_t>(slot)])] = static_cast<u16>(combo.element);
  }
  units_[static_cast<size_t>(pc)].seal_flag.fill(false);
  refresh_seal_flags(pc);
}

void Battle::refresh_seal_flags(int pc) {
  if (!is_pc_slot(pc)) return;
  Unit& unit = units_[static_cast<size_t>(pc)];
  for (size_t cell = 0; cell < data::kGridCells; ++cell) {
    const int id = unit.combo_word[cell] ? unit.combo_word[cell] : unit.cells[cell] & 0x3FFF;
    if (!unit.usable[cell] || id <= 0 || static_cast<size_t>(id) >= tables_.elements.size() || (element_colour(id) & 0xFC) == 0xFC) continue;
    unit.seal_flag[cell] = colour_sealed(pc, id);
  }
}

int Battle::cell_element(int pc, int cell) const {
  if (!is_pc_slot(pc) || cell < 0 || cell >= data::kGridCells) return 0;
  const ComboTech combo = combo_at(pc, cell);
  return combo.element ? combo.element : unit(pc).cells[static_cast<size_t>(cell)] & 0x3FFF;
}

bool Battle::element_usable(int pc, int cell) const {
  if (!is_pc_slot(pc) || cell < 0 || cell >= data::kGridCells) return false;
  const Unit& caster = unit(pc);
  if (element_sealed(pc, cell)) return false;
  if (!caster.can_open_menu() || caster.stamina <= 0) return false;
  if (!caster.usable[static_cast<size_t>(cell)] || caster.used[static_cast<size_t>(cell)]) return false;
  return caster.gauge >= cell / data::kGridCols;
}

int Battle::auto_hit_level(int actor, int target, bool fallback_weak) const {
  const Unit& attacker = unit(actor);
  const bool fatigued = (attacker.lo & st::kFatigued) != 0;
  static const int kNeedNormal[3] = {10, 20, 30}, kNeedFatigued[3] = {10, 22, 35};
  for (int k = 2; k >= 0; --k)
    if (hit_chance_for(actor, target, k) > 79 && attacker.stamina >= (fatigued ? kNeedFatigued[k] : kNeedNormal[k])) return k;
  return fallback_weak && chain_cost(0, attacker) <= attacker.stamina ? 0 : -1;
}

int Battle::hit_chance_for(int actor, int target, int strength) const {
  const Unit& attacker = unit(actor);
  const Unit& target_unit = unit(target);
  if (cheats_.always_hit && attacker.pc) return 100;
  ChanceIn in;
  const int stat = strength == 0 ? attacker.hit : strength == 1 ? attacker.hit_med : attacker.hit_heavy;
  const int scripted_base = attacker.hit_ovr[static_cast<size_t>(std::clamp(strength, 0, 2))];
  in.base = scripted_base >= 0 ? scripted_base : hit_base(stat, attacker.eq.wp_acc);
  in.hit_mod = attacker.mod[1];
  in.evd = target_unit.evd + target_unit.eq.evd_gear;
  in.evd_mod = target_unit.mod[3];
  in.attacker_hero_set = attacker.hero_set;
  in.target_dizzy = (target_unit.lo & st::kDizzy) != 0;
  in.pc_attacker = attacker.pc;
  in.hits = (attacker.pc && chain_actor_ == actor && chain_target_ == target) ? chain_hits_ : 0;
  in.strength = strength;
  in.always_hit = (attacker.hi & st::kHiAlwaysHit) != 0;
  in.dark = (attacker.lo & st::kDark) != 0;
  in.target_asleep = (target_unit.lo & st::kAsleep) != 0;
  return battle::hit_chance(in);
}

// A hidden / off-stage enemy (ops 34 / 35 / 5B) is not on the cursor ring, so a single-target command cannot aim at it.
bool Battle::validate(const Command& cmd, std::string* why) const {
  auto no = [&](const char* reason) {
    if (why) *why = reason;
    return false;
  };
  if (outcome_ != Outcome::Ongoing) return no("battle is over");
  if (!is_pc_slot(cmd.actor) || !can_act(cmd.actor)) return no("actor cannot act");
  const Unit& actor_unit = unit(cmd.actor);
  switch (cmd.kind) {
    case Command::Kind::Attack: {
      if (cmd.chain.empty() || cmd.chain.size() > kMaxChain) return no("chain length must be 1..4");
      int budget = actor_unit.stamina;
      for (int strength : cmd.chain) {
        if (strength < 0 || strength > 2) return no("bad strength");
        const int cost = chain_cost(strength, actor_unit);
        if (cost > budget) return no("not enough stamina for the chain");
        budget -= cost;
      }
      break;
    }
    case Command::Kind::Element:
      if (!element_usable(cmd.actor, cmd.cell)) return no("element not usable");
      break;
    case Command::Kind::Defend:
    case Command::Kind::Run: break;
  }
  if (cmd.target >= kUnits) return no("bad target");
  if (cmd.target >= 0 && cmd.kind != Command::Kind::Defend && cmd.kind != Command::Kind::Run && !is_pc_slot(cmd.target) && brain_.untargetable(cmd.target))
    return no("the target is off-stage");
  if (cmd.kind == Command::Kind::Element && cmd.target >= 0 && element_usable(cmd.actor, cmd.cell)) {
    const int id = cell_element(cmd.actor, cmd.cell);
    if (id > 0 && static_cast<size_t>(id) < tables_.elements.size()) {
      const TargetSpec spec = target_spec(tables_.elements[static_cast<size_t>(id)]);
      const Unit& target_unit = units_[static_cast<size_t>(cmd.target)];
      if (!target_unit.present) return no("no such target");
      if (is_pc_slot(cmd.target) ? !spec.party : !spec.enemies) return no("this element cannot be aimed at that side");
      if (spec.self_only && cmd.target != cmd.actor) return no("this element only affects its caster");
      if (spec.ko_only ? !(target_unit.lo & st::kKO) : !target_unit.alive()) return no(spec.ko_only ? "this element needs a knocked-out target" : "the target is knocked out");
    }
  }
  return true;
}

// ------------------------------------------------------------------------------------------------ Dopplegang / Pip
std::vector<int> Battle::dopple_pool() const {
  std::vector<int> out;
  for (int i = 0; i < 256; ++i)
    if (dopple_has(i)) out.push_back(i);
  return out;
}

bool Battle::dopple_credit(int enemy_unit, int last_pc) {
  if (enemy_unit < kPcSlots || enemy_unit >= kUnits || last_pc < 0 || last_pc >= kPcSlots) return false;
  const Unit& last_unit = units_[static_cast<size_t>(last_pc)];
  const Unit& enemy = units_[static_cast<size_t>(enemy_unit)];
  if (!last_unit.present || !enemy.present || enemy.pc || enemy.src_id < 0 || static_cast<size_t>(enemy.src_id) >= tables_.enemies.size()) return false;
  bool eligible = last_unit.pc && last_unit.src_id == 26;
  for (const u16 acc_id : last_unit.eq.acc) eligible = eligible || acc_id == 0xFC;
  const auto& rec = tables_.enemies[static_cast<size_t>(enemy.src_id)];
  if (!eligible || (rec.raw[0xA6] & 0x80)) return false;
  const int slot = rec.raw[0x2B];
  const u16 bit = static_cast<u16>(1u << (slot & 15));
  u16& word = dopple_mask_[static_cast<size_t>(slot >> 4)];
  const bool added = (word & bit) == 0;
  word = static_cast<u16>(word | bit);
  return added;
}

bool Battle::transform(int unit_index, int form) {
  if (unit_index < 0 || unit_index >= kPcSlots || !units_[static_cast<size_t>(unit_index)].present || !units_[static_cast<size_t>(unit_index)].pc) return false;
  Unit& unit = units_[static_cast<size_t>(unit_index)];
  if (form < 0) {
    if (unit.form < 0) return false;
    Unit restored = form_backup_[static_cast<size_t>(unit_index)];
    restored.hp = unit.hp, restored.max_hp = unit.max_hp;
    restored.countdown = 8;
    restored.stamina = unit.stamina;
    unit = std::move(restored);
    return true;
  }
  if (static_cast<size_t>(form) >= tables_.enemies.size()) return false;
  if (unit.form < 0) form_backup_[static_cast<size_t>(unit_index)] = unit;
  const auto& rec = tables_.enemies[static_cast<size_t>(form)];
  const Unit old = unit;
  Unit fresh{};
  fresh.present = fresh.pc = true;
  fresh.src_id = old.src_id;
  fresh.form = form;
  fresh.name = tables_.enemy_name(form);
  if (fresh.name.empty()) fresh.name = std::format("monster{}", form);
  fresh.max_hp = old.max_hp, fresh.hp = old.hp;
  fresh.ap = 8, fresh.countdown = 8, fresh.stamina = old.stamina, fresh.gauge = -1;
  fresh.gender = old.gender;
  fresh.hero_set = old.hero_set;
  load_monster_record(fresh, rec, 0x400400u);
  // The form's grid loses its lock bits, so the gauge tops out at the deepest row holding an element.
  fresh.gauge_max = -1;
  for (size_t cell = 0; cell < data::kGridCells; ++cell) {
    fresh.cells[cell] &= 0x3FFF;
    if (fresh.cells[cell] != 0) fresh.gauge_max = static_cast<int>(cell) / data::kGridCols;
  }
  unit = std::move(fresh);
  return true;
}

// Pip's form change (pgdat mode 45): the innate colour becomes the colour with the most colour points (script vars 4..0xE = green white
// black red yellow blue; the last of equal ones), the six counters are zeroed, the kind byte, AP / countdown 8, the form's tech cells
// (bits 14-15 kept; the third only for a second form) and the growth column are written to the unit and the party record.
void Battle::evolve(int unit_index, int form) {
  if (unit_index < 0 || unit_index >= kPcSlots || !pip_form(form)) return;
  Unit& unit = units_[static_cast<size_t>(unit_index)];
  int best_points = 0, best_colour = 0;
  for (int k = 0; k < 6; ++k) {
    const int points = brain_.script_var(4 + 2 * k) & 0xFFFF;
    if (best_points <= points) best_points = points, best_colour = k;
  }
  unit.innate = static_cast<u8>((unit.innate & 3) | (4 << best_colour));
  for (int k = 0; k < 6; ++k) brain_.set_script_var(4 + 2 * k, 0);
  evo_pending_ = 0;
  unit.kind = form;
  unit.ap = 8, unit.countdown = 8;
  const size_t tech_base = static_cast<size_t>(form - 0x2B) * 3;
  for (size_t k = 0; k < kPipTechCells.size(); ++k) {
    if (k == 2 && form <= 0x2D) break;
    u16& cell = unit.cells[static_cast<size_t>(kPipTechCells[k])];
    cell = static_cast<u16>((cell & 0xC000) | kPipTechs[tech_base + k]);
    if (static_cast<size_t>(unit_index) < party_.size()) {
      u16& record_cell = party_[static_cast<size_t>(unit_index)].cells[static_cast<size_t>(kPipTechCells[k])];
      record_cell = static_cast<u16>((record_cell & 0xC000) | kPipTechs[tech_base + k]);
    }
  }
  pip_tech_colour_ = unit.innate;
  if (static_cast<size_t>(unit_index) < party_.size()) {
    PartyMember& member = party_[static_cast<size_t>(unit_index)];
    member.kind = form;
    member.evolved = true;
    member.growth_col = kPipGrowthCol[static_cast<size_t>(form - 0x2B)];
  }
  say(std::format("{} changes form: kind {:#x}, innate {:#x}", unit.name, form, unit.innate));
}

u8 Battle::element_colour(int id) const {
  if (id < 0 || id >= static_cast<int>(tables_.elements.size())) return 0;
  const u8 colour_bit = tables_.elements[static_cast<size_t>(id)].colour_bit();
  if (id == kVaryInnateElement && vary_innate_colour_) return static_cast<u8>((colour_bit & 3) | vary_innate_colour_);
  return pip_tech_colour_ && pip_tech(id) ? static_cast<u8>((colour_bit & 3) | (pip_tech_colour_ & 0xFC)) : colour_bit;
}

int Battle::seq_colour(int element_id) const {
  if (element_id < 0 || element_id >= static_cast<int>(tables_.elements.size())) return -1;
  if (tables_.elements[static_cast<size_t>(element_id)].b(0) & 0x80) return -1;
  const u8 colour_mask = element_colour(element_id) & 0xFC;
  if (colour_mask == 0) return 0;
  if (colour_mask == 0xFC) return 6;
  return data::colour_index_of_bit(colour_mask);
}

void Battle::finish_ring(const data::ElemRec& element, int colour) {
  if (ring_repainted_) ring_ = ring_repaint_;
  else if (!(element.b(0) & 0x80) && element.mode() != 5) ring_push(ring_, colour);
  ring_repainted_ = false;
}

// ------------------------------------------------------------------------------------------------ damage plumbing
// A knocked-out enemy runs its kill script; the death element it queues (a scripted death) is cast by the dying unit once the killing
// action has ended (run_reactions). An invincible unit (Debug tab) stays up at 1 HP.
// An enemy that dies while a summon is the current action is noted under the summon's colour; the first colour to claim it keeps it.
void Battle::note_summon_kill(int unit_index) {
  if (summon_colour_ < 0) return;
  const u16 bit = static_cast<u16>(1u << unit_index);
  if (std::none_of(summon_kills_.begin(), summon_kills_.end(), [&](u16 mask) { return (mask & bit) != 0; }))
    summon_kills_[static_cast<size_t>(summon_colour_)] |= bit;
}

void Battle::kill(int unit_index) {
  Unit& victim = units_[static_cast<size_t>(unit_index)];
  if (invincible(unit_index)) {
    victim.hp = std::max(victim.hp, 1);
    return;
  }
  victim.hp = 0;
  victim.lo |= st::kKO;
  victim.guard = false;
  victim.defending = false;
  say(std::format("  {} is knocked out", victim.name));
  if (!victim.pc) run_kill_script(unit_index);
}

void Battle::run_kill_script(int unit_index) {
  note_summon_kill(unit_index);
  kill_script_run_[static_cast<size_t>(unit_index)] = true;
  std::vector<EnemyAction> plan;
  brain_.on_killed(*this, unit_index, rng_, plan);
  if (!plan.empty()) {
    death_plans_.emplace_back(unit_index, std::move(plan));
    death_pending_[static_cast<size_t>(unit_index)] = true;
  }
}

// The kill script starts for every enemy at HP 0 (or KO'd), however it got there: a battle script can write the HP to 0 itself.
void Battle::scan_kills() {
  for (int i = kPcSlots; i < kUnits && outcome_ == Outcome::Ongoing; ++i) {
    const Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.present) continue;
    if (unit.hp > 0 && !(unit.lo & st::kKO)) {
      kill_script_run_[static_cast<size_t>(i)] = false;
      continue;
    }
    if (kill_script_run_[static_cast<size_t>(i)]) continue;
    say(std::format("  {} is knocked out", unit.name));
    run_kill_script(i);
  }
}

// Cheats tab, God mode: a hit (weapon / element damage, not a heal) on a PC does nothing -- value 0, code 1 ("no damage": no number, HP kept).
void Battle::god_filter(int target, DamageOut& dmg) const {
  if (!cheats_.god_mode || target < 0 || target >= kPcSlots || dmg.code == 3) return;
  dmg.value = 0;
  dmg.code = 1;
}

// Code 3 heals, code 2 and negative values do nothing; a self-inflicted KO leaves the actor at 1 HP.
void Battle::damage_unit(int actor, int target, int value, int code) {
  Unit& target_unit = units_[static_cast<size_t>(target)];
  // A KO'd unit takes neither damage nor healing: its HP stays 0 (only a revive clears the KO).
  if (target_unit.lo & st::kKO) {
    target_unit.hp = 0;
    return;
  }
  if (code == 3) {
    target_unit.hp = std::min(target_unit.max_hp, target_unit.hp + value);
    return;
  }
  if (code == 2 || value < 0) return;
  target_unit.hp -= value;
  if (target_unit.lo & st::kStaminaDrain) target_unit.stamina -= 10;
  if (target_unit.hp <= 0 && invincible(target)) target_unit.hp = 1;
  if (target_unit.hp <= 0) {
    if (target == actor) target_unit.hp = 1;
    else kill(target);
  }
}

// The queued death casts of the units killed during the last action (kill script, slot 3). The dying unit is still in the alive mask until
// its script ends, so the plan runs without the actor's alive test and the unit keeps blocking the victory test (death_pending_) until
// it has been cast.
void Battle::run_death_plans() {
  scan_kills();
  run_idle_plans();
  for (int guard = 0; guard < 16 && !death_plans_.empty() && outcome_ == Outcome::Ongoing; ++guard) {
    auto [unit, plan] = std::move(death_plans_.front());
    death_plans_.erase(death_plans_.begin());
    begin_action();
    in_death_cast_ = true;
    int poison_step = 1, timer_steps = 1;
    for (size_t i = 0; i < plan.size(); ++i) {
      const EnemyAction& action = plan[i];
      if (outcome_ != Outcome::Ongoing) break;
      // Every op 67 of a kill script is its own executor action (the Time Devourer's 341, then 526); its targets share one.
      if (i > 0 && action.seq != plan[i - 1].seq) begin_action();
      cur_script_seq_ = action.seq;
      cur_group_ = action.group;
      std::tie(poison_step, timer_steps) = exec_action(unit, action);
      cur_script_seq_ = -1;
      cur_group_.fill(0xFF);
    }
    in_death_cast_ = false;
    death_pending_[static_cast<size_t>(unit)] = false;
    post_action(unit, poison_step, timer_steps);
    check_end();
  }
}

// What the idle loops queued while the scripts settled (a hidden unit's arrival cast) is one action per unit; casting can wake further loops.
void Battle::run_idle_plans() {
  brain_.take_idle_plans(*this, idle_plans_);
  for (int guard = 0; guard < 16 && !idle_plans_.empty() && outcome_ == Outcome::Ongoing; ++guard) {
    auto [unit, plan] = std::move(idle_plans_.front());
    idle_plans_.erase(idle_plans_.begin());
    if (units_[static_cast<size_t>(unit)].present) {
      begin_action();
      run_plan(unit, plan, false);
      check_end();
    }
    brain_.take_idle_plans(*this, idle_plans_);
  }
}

// After an action the target list is walked in order and every unit still alive gets its "attacked" script (slot 4); the attacks it
// queues are executed like any queued action (counter-attacks). Death plans run before, between and after the rounds.
void Battle::run_reactions() {
  run_death_plans();
  for (int guard = 0; guard < 64 && !attacked_.empty() && outcome_ == Outcome::Ongoing; ++guard) {
    std::vector<int> list;
    list.swap(attacked_);
    for (const int unit : list) {
      if (outcome_ != Outcome::Ongoing) break;
      if (!units_[static_cast<size_t>(unit)].alive()) continue;
      std::vector<EnemyAction> plan;
      brain_.on_attacked(*this, unit, rng_, plan);
      if (plan.empty()) continue;
      begin_action();
      run_plan(unit, plan, false);
      check_end();
    }
    run_death_plans();
  }
  attacked_.clear();
  run_death_plans();
}

void Battle::set_status_bit(int target, int set, int bit) {
  Unit& target_unit = units_[static_cast<size_t>(target)];
  (set == 0 ? target_unit.lo : target_unit.hi) |= 1u << bit;
  const int idx = bit + 32 * set;
  if (status_is_timed(set, bit)) target_unit.timers[idx] = status_timer(idx, target_unit.dur, 0);
}

// Nothing lands on an immune target. Hi bits 29..31 only stay when one of the target's accessories is 0xF9..0xFB (then an old one of them
// in the permanent word goes, the element's colour joins as the shield bits, and bits 29..31 / 0..5 land without a roll), else bits 0..5 and
// 29..31 are stripped. A colour shield in the mask replaces the inflicted one. Per bit: skipped when the skip mask has it, chance doubled
// when the weakness mask has it, lands on rnd(1,99) <= chance unless already set. No bit is special (lo bit 31 is the blue seal, not a kill).
bool Battle::apply_status_bits(int target, u32 lo, u32 hi, int chance, u8 colour_bit) {
  Unit& target_unit = units_[static_cast<size_t>(target)];
  if (target_unit.hi & st::kHiImmune) return false;
  bool special = false;
  if (hi & 0xE0000000u) {
    special = std::any_of(target_unit.eq.acc.begin(), target_unit.eq.acc.end(), [](u16 id) { return id >= 0xF9 && id <= 0xFB; });
    if (special) {
      if (target_unit.perm_hi & hi & 0xE0000000u) {
        target_unit.hi &= ~(target_unit.perm_hi & ~0x1FFFFFC0u);
        target_unit.perm_hi &= 0x1FFFFFC0u;
      }
      hi |= static_cast<u32>(colour_bit >> 2);
    } else {
      hi &= 0x1FFFFFC0u;
    }
  }
  if (hi & 0x3Fu) target_unit.hi = (target_unit.hi & ~0x3Fu) | (target_unit.perm_hi & 0x3Fu);
  bool any = false;
  for (int set = 0; set < 2; ++set) {
    const u32 mask = set == 0 ? lo : hi;
    for (int bit = 0; bit < 32; ++bit) {
      if (!(mask >> bit & 1u) || !target_unit.alive()) continue;
      if ((set == 0 ? target_unit.skip_lo : target_unit.skip_hi) >> bit & 1u) continue;
      const bool weak = ((set == 0 ? target_unit.weak_lo : target_unit.weak_hi) >> bit & 1u) != 0;
      const bool direct = special && set == 1 && (bit < 6 || bit > 28);
      if (!direct && !status_roll(rng_, chance, weak)) continue;
      any = true;
      if ((set == 0 ? target_unit.lo : target_unit.hi) >> bit & 1u) continue;
      set_status_bit(target, set, bit);
      say(std::format("  {} gains status {}:{:#x}", target_unit.name, set, 1u << bit));
    }
  }
  return any;
}

bool Battle::apply_status(int target, const data::ElemRec& rec) {
  return apply_status_bits(target, rec.status_lo(), rec.status_hi() & st::kHiApplyMask, rec.status_pct(),
                           element_colour(static_cast<int>(&rec - tables_.elements.data())));
}

bool Battle::force_status_bits(int target, u32 lo) {
  Unit& target_unit = units_[static_cast<size_t>(target)];
  bool any = false;
  for (int bit = 0; bit < 32; ++bit) {
    if (!(lo >> bit & 1u) || !target_unit.alive() || (target_unit.lo >> bit & 1u) || (target_unit.skip_lo >> bit & 1u)) continue;
    set_status_bit(target, 0, bit);
    any = true;
  }
  return any;
}

// ------------------------------------------------------------------------------------------------ weapon attack
void Battle::do_weapon_attack(int actor, int target, int strength) {
  const SealRefreshOnExit refresh{*this, target};
  summon_colour_ = -1;
  Unit& attacker = units_[static_cast<size_t>(actor)];
  Unit& target_unit = units_[static_cast<size_t>(target)];
  // Script var 0x22 / 0x28 + 6 * pc = the hit strength (0 weak / 1 strong / 2 fierce), not the weapon class.
  brain_.on_attack(*this, actor, 0, strength, target);
  if (attacker.pc && (chain_actor_ != actor || chain_target_ != target)) {
    chain_actor_ = actor;
    chain_target_ = target;
    chain_hits_ = 0;
  }
  const int chance = hit_chance_for(actor, target, strength);
  // Dev: every PC weapon attack misses.
  static const bool force_miss = std::getenv("OC_BATTLE_MISS") != nullptr;
  const bool rolled = roll_hit(rng_, chance);
  const bool hit = rolled && !(force_miss && attacker.pc);
  static const char* kStrName[] = {"weak", "medium", "heavy"};
  ActionEvent ev;
  ev.kind = ActionEvent::Kind::Weapon;
  ev.actor = actor;
  ev.death_cast = in_death_cast_;
  ev.target = target;
  ev.strength = strength;
  ev.hp_before = ev.hp_after = target_unit.hp;
  if (!hit) {
    if (attacker.pc) chain_hits_ = 0;
    say(std::format("{} {} attack on {}: miss ({}%)", attacker.name, kStrName[strength], target_unit.name, chance));
    ev.text = log_.back();
    ev.code = 2;
    note_event(ev);
    return;
  }
  const int ci = colour_of(attacker) < 0 ? kNoColour : colour_of(attacker);
  // The hit takes the colour of the attacker's colour status (hi bits 0..5, set by the Turn elements) when it has one, else its innate
  // colour; the innate colour keeps its own point either way.
  const int shield = data::colour_index_of_bit(static_cast<u8>((attacker.hi & 0x3Fu) << 2));
  const int attack_colour = shield >= 0 ? shield : ci;
  const int ct = colour_of(target_unit);
  WeaponIn input;
  input.atk = attacker.atk;
  input.wp = weapon_term(attacker);
  input.wp_var = attacker.eq.wp_var;
  input.def = target_unit.def;
  input.arm = ceil100(target_unit.eq.arm, target_unit.mod[2]);
  input.arm_var = target_unit.eq.arm_var;
  input.arm_soft = (target_unit.lo & 0x4008) != 0;
  input.mult = attacker.pc ? kPcStrMul[static_cast<size_t>(strength)] : kEnemyStrMul[static_cast<size_t>(strength)];
  input.colour_mille = colour_multiplier(ci, attack_colour, ring_, ct);
  input.strength = strength;
  input.weapon_class = attacker.eq.wp_class;
  input.hero_set = attacker.hero_set;
  input.slayer = (attacker.eq.slayer & target_unit.hi) != 0;
  input.resisted = (target_unit.eq.guard_hi & attacker.hi) != 0;
  input.gear_up = (attacker.eq.gear_flags & data::gearflag::kDmgUp) != 0;
  input.gear_down = (target_unit.eq.gear_flags & data::gearflag::kDmgDown) != 0;
  input.target_afraid = (target_unit.lo & st::kAfraid) != 0;
  input.opposite_colour = ct >= 0 && attack_colour < 6 && kOpp[static_cast<size_t>(ct)] == attack_colour;
  input.guarding = target_unit.pc && target_unit.guard;
  DamageOut damage = weapon_damage(rng_, input);
  if (target_unit.hi & st::kHiWeaponImmune) {
    damage.value = 0;
    damage.code = 2;
  } else if ((target_unit.hi & st::kHiWeaponAbsorb) && damage.value > 0) {
    damage.code = 3;
  }
  god_filter(target, damage);
  say(std::format("{} {} attack on {}: {}{} ({}%, x{})", attacker.name, kStrName[strength], target_unit.name, damage.value, damage.crit ? " CRIT" : "",
                  chance, input.colour_mille));
  ev.text = log_.back();
  if (target_unit.lo & st::kAsleep) {
    target_unit.lo &= ~st::kAsleep;
    target_unit.timers.erase(14);
  }
  const bool alive0 = target_unit.alive();
  damage_unit(actor, target, damage.value, damage.code);
  ev.value = damage.value;
  ev.code = damage.code == 3 || damage.code == 2 ? damage.code : (damage.value == 0 ? 1 : 0);
  ev.crit = damage.crit;
  ev.ko = alive0 && !target_unit.alive();
  ev.ko_owed = ev.ko && death_pending_[static_cast<size_t>(target)];
  ev.hp_after = target_unit.hp;
  note_event(ev);
  if (attacker.pc) chain_hits_ += kChainAdd[static_cast<size_t>(strength)];
  // Any attacker's gauge fills.
  attacker.gauge = std::clamp(attacker.gauge + strength + 1, -1, attacker.gauge_max);
}

// ------------------------------------------------------------------------------------------------ element / tech
std::array<int, 6> Battle::tech_participants(int actor, const data::ElemRec& element) const {
  std::array<int, 6> list{-1, -1, -1, -1, -1, -1};
  if (actor < kPcSlots) {
    if (element.b(0x14) == 0xFF) return list;
    for (size_t k = 0; k < 3; ++k)
      for (int slot = 0; slot < kPcSlots; ++slot)
        if (element.b(0x14 + k) != 0xFF && units_[static_cast<size_t>(slot)].present && units_[static_cast<size_t>(slot)].src_id == element.b(0x14 + k)) list[k] = slot;
    return list;
  }
  if (element.b(0x17) == 0xFF) return list;
  for (size_t k = 0; k < std::min<size_t>(element.b(0x17), list.size()); ++k)
    if (cur_group_[k] < kUnits) list[k] = cur_group_[k];
  return list;
}

void Battle::do_element(int actor, int element_id, int row, int target, bool push_ring) {
  const SealRefreshOnExit refresh{*this, target};
  Unit& caster = units_[static_cast<size_t>(actor)];
  if (element_id < 0 || element_id >= static_cast<int>(tables_.elements.size())) return;
  const data::ElemRec& element = tables_.elements[static_cast<size_t>(element_id)];
  summon_colour_ = (element.b(0) & 0x40) ? data::colour_index_of_bit(element.colour_bit()) : -1;
  Unit& target_unit = units_[static_cast<size_t>(target)];
  const int hp0 = target_unit.hp;
  const bool alive0 = target_unit.alive();
  const size_t log0 = log_.size();
  brain_.on_attack(*this, actor, 1, element_id, target);
  const int ci = colour_of(caster) < 0 ? kNoColour : colour_of(caster);
  int ce = data::colour_index_of_bit(element_colour(element_id));
  const int ce_pool = ce < 0 ? kNoColour : ce;
  const int ct = colour_of(target_unit);
  int cmul = colour_multiplier(ci, ce_pool, ring_, ct);
  // The element modes' colour step: the timed field state (mode 6) halves (1) or raises by half (2) the multiplier.
  if ((field_state_ & 3) == 1) cmul /= 2;
  else if ((field_state_ & 3) == 2) cmul = cmul * 300 / 200;
  const int slot = slot_mille(row, element.level());
  const std::string& nm = tables_.element_name(element_id);
  const int mode = element.mode();
  // Shared hit block of the damage modes: source 0 = MAG vs MDEF, 32 = ATK vs DEF, 33 = a fixed value.
  auto elem_in = [&](int source) {
    ElementIn in;
    in.source = source;
    in.mag = caster.mag;
    in.mag_term = ceil100(1 + caster.eq.mag_gear, caster.mod[4]);
    in.power = element.power();
    in.var = element.variance();
    in.mdef = target_unit.mdef;
    in.mdef_term = ceil100(1 + target_unit.eq.mdef_gear, target_unit.mod[5]);
    in.mdef_var = target_unit.eq.arm_var;
    in.atk = caster.atk;
    in.wp_term = weapon_term(caster);
    in.def = target_unit.def;
    in.arm_term = ceil100(target_unit.eq.arm, target_unit.mod[2]);
    in.arm_var = target_unit.eq.arm_var;
    in.arm_soft = (target_unit.lo & 0x4008) != 0;
    in.gear_up = (caster.eq.gear_flags & data::gearflag::kDmgUp) != 0;
    in.gear_down = (target_unit.eq.gear_flags & data::gearflag::kDmgDown) != 0;
    in.mdef_soft = (target_unit.lo & 8) != 0;
    in.slot_mille = slot;
    in.atk_matt_pct = caster.mod[4];
    in.tgt_mdef_pct = target_unit.mod[5];
    in.colour_mille = cmul;
    in.elem_colour = ce_pool;
    for (int colour = 0; colour < 6; ++colour)
      if (target_unit.hi >> colour & 1u) in.shield_colour = colour;
    in.target_colour = ct;
    in.opposite_colour = ct >= 0 && ce >= 0 && kOpp[static_cast<size_t>(ct)] == ce;
    in.armour_boost_colour = armour_boost_colour(target_unit.eq.armour_colour);
    // Absorbed: the element absorb status, or bit 1 of the innate byte / the armour colour byte with the element's colour in its mask.
    u8 absorb_mask = (target_unit.innate & 2) ? static_cast<u8>(target_unit.innate & 0xFC) : 0;
    if (target_unit.eq.armour_colour & 2) absorb_mask |= static_cast<u8>(target_unit.eq.armour_colour & 0xFC);
    in.target_absorb = (target_unit.hi & st::kHiAbsorb) != 0 || (absorb_mask & element_colour(element_id)) != 0;
    in.target_immune = (target_unit.hi & st::kHiImmune) != 0;
    // Exploit: the element hi status mask meets the target hi word.
    in.exploit = (element.status_hi() & target_unit.hi & 0xE007FFFFu) != 0;
    return in;
  };
  auto deal = [&](const ElementIn& in) {
    DamageOut damage = element_damage(rng_, in);
    god_filter(target, damage);
    say(std::format("{} casts {} on {}: {}{} (mode {}, slot {}, x{})", caster.name, nm, target_unit.name, damage.value, damage.code == 3 ? " healed" : "", mode, slot, cmul));
    damage_unit(actor, target, damage.value, damage.code);
    return damage;
  };
  // A heal hurts a target with hi 0x2000000 (code 0) and heals anyone else (code 3).
  auto heal_code = [&]() { return (target_unit.hi & 0x2000000u) ? 0 : 3; };
  // The cure of the record's +0x18 / +0x1C statuses: the inflicted bits leave the target with their timers, no roll; the permanent ones stay.
  auto cure = [&]() {
    const u32 lo = element.status_lo() & ~target_unit.perm_lo;
    const u32 hi = element.status_hi() & st::kHiApplyMask & ~target_unit.perm_hi;
    target_unit.lo &= ~lo;
    target_unit.hi &= ~hi;
    for (auto it = target_unit.timers.begin(); it != target_unit.timers.end();) {
      const bool cured = it->first < 32 ? (lo >> it->first & 1u) != 0 : (hi >> (it->first - 32) & 1u) != 0;
      it = cured ? target_unit.timers.erase(it) : std::next(it);
    }
  };
  // The hit wakes (lo 0x4000) and un-confuses (lo 0x100000) the target.
  auto wake = [&]() {
    if (target_unit.lo & (st::kAsleep | st::kConfused)) {
      target_unit.lo &= ~(st::kAsleep | st::kConfused);
      target_unit.timers.erase(14);
      target_unit.timers.erase(20);
    }
  };
  // ActionEvent code for the status / utility modes (2 = nothing landed).
  int code_override = -1;
  // Mode 30: message row (record +4) and the stolen word (record +0).
  int steal_msg = -1, steal_word = 0;
  // Set to 1 before the mode, cleared by a mode whose roll failed (modes 1 / 7 here).
  bool landed = true;
  const bool frozen0 = (target_unit.lo & 0x20000000u) != 0;
  // The mode repainted the field ring itself.
  bool ring_done = false;
  // Mode 0's hit pass: the status roll first; element record +0xE bit 15 = a landed status cancels the damage, bit 14 = a target with hi
  // 0x2000000 is knocked out outright; else MAG damage.
  auto hit_pass = [&]() {
    const bool status_landed = apply_status(target, element);
    const u16 flags = element.rd16(0xE);
    if ((flags & 0x4000) && (target_unit.hi & 0x2000000u)) {
      if (target_unit.alive() && !(target_unit.skip_lo & st::kKO)) {
        say(std::format("{} casts {} on {}: knocked out", caster.name, nm, target_unit.name));
        kill(target);
      }
      code_override = 0;
    } else if (!(flags & 0x8000) || !status_landed) {
      deal(elem_in(0));
    } else {
      code_override = 0;
    }
  };

  switch (mode) {
    case 0:
      hit_pass();
      break;
    case 35: {  // MAG damage without the absorb branch; the caster gains damage / element record +0xE (0 = all of it)
      ElementIn in = elem_in(0);
      in.target_absorb = false;
      const DamageOut damage = deal(in);
      if (damage.code == 0 && damage.value > 0) {
        const int div = static_cast<s16>(element.rd16(0xE));
        caster.hp = std::min(caster.max_hp, caster.hp + (div != 0 ? damage.value / div : damage.value));
      }
      break;
    }
    case 32: {  // ATK / DEF damage, then the status roll (does not cancel the damage), wakes the target
      deal(elem_in(32));
      apply_status(target, element);
      wake();
      break;
    }
    case 33: {  // power % of the current HP of the target (element record +0xE == 0) or of the caster, no colour / MDef stage, then status
      const Unit& src = element.rd16(0xE) == 0 ? target_unit : caster;
      ElementIn in = elem_in(33);
      in.fixed = src.hp * element.power() / 100;
      deal(in);
      apply_status(target, element);
      break;
    }
    case 37: {  // mode 0 damage (no status), then the ring is repainted with element record +0xA / +0xC / +0xE, colour masks in the ring's
               // own form (bit 0 green .. bit 5 blue)
      deal(elem_in(0));
      for (int k = 0; k < 3; ++k) {
        const int colour = data::colour_index_of_bit(static_cast<u8>(element.b(static_cast<size_t>(0xA + 2 * k)) << 2));
        ring_repaint_[static_cast<size_t>(k)] = colour >= 0 && colour < 6 ? colour : -1;
      }
      ring_done = true;
      break;
    }
    case 39: {  // Big Catch (element 192): the creature on the hook comes from the Dopplegang pool; its power replaces the slot term
      // Cast state (once per cast, shared by every target): 0 = roll rnd(0, 16), 0 = the line breaks (state 1: no creature, every target misses);
      // else rnd(0, 255) until the slot's pool bit is set (an empty pool counts as a broken line), the record +2 :=
      // the creature table's model, state 2. The hit is mode 0's MAG damage (no status roll) with a slot term of max(10, (10000 - model) / 200) * 100
      // in place of the grid slot's 1000 +/- 125 n (e.g. model 86 gives 4900). The creature's own colour / strength does not enter the code.
      if (creature_state_ == 0) {
        creature_state_ = 1;
        if (rng_.rnd(0, 16) != 0) {
          bool any = false;
          // The creature table has one row per creature; a pool bit past it is never set.
          const int pool_n = static_cast<int>(kDoppleTable.size());
          for (int i = 0; i < pool_n && !any; ++i) any = dopple_has(i);
          for (int tries = 0; any && tries < 4096; ++tries) {
            const int pool_slot = rng_.rnd(0, 255);
            if (pool_slot >= pool_n || !dopple_has(pool_slot)) continue;
            creature_slot_ = pool_slot;
            creature_model_ = kDoppleTable[static_cast<size_t>(pool_slot)].model;
            creature_state_ = 2;
            break;
          }
        }
      }
      if (creature_state_ == 2) {
        ElementIn in = elem_in(0);
        in.slot_mille = std::max(10, (10000 - creature_model_) / 200) * 100;
        deal(in);
        say(std::format("{} hooks pool slot {} (model {})", caster.name, creature_slot_, creature_model_));
      } else {
        say(std::format("{} casts {} on {}: the line breaks, no creature", caster.name, nm, target_unit.name));
        code_override = 2;
      }
      break;
    }
    case 15: {  // the status roll first; when nothing lands, mode 0 damage
      if (apply_status(target, element)) {
        say(std::format("{} casts {} on {}: status", caster.name, nm, target_unit.name));
        code_override = 0;
      } else {
        deal(elem_in(0));
      }
      break;
    }
    case 2: {  // the element's statuses, else lo 0x20000 or 0x40000 (rnd(0,1)) at the same chance; nothing -> code 2
      bool ok = apply_status(target, element);
      if (!ok) ok = apply_status_bits(target, 1u << (17 + rng_.rnd(0, 1)), 0, element.status_pct());
      say(std::format("{} casts {} on {}: {}", caster.name, nm, target_unit.name, ok ? "status" : "no effect"));
      code_override = ok ? 0 : 2;
      break;
    }
    case 10: {  // gate rnd(1,99) <= element record +0xA, a random lo bit of 0x8E8EA939, then the normal roll for it
      bool ok = false;
      if (rng_.rnd(1, 99) <= element.status_pct()) {
        int bit = 0;
        // Retry until a bit of the mask is hit (bounded, since a scripted roll source may never hit).
        for (int tries = 0; tries < 1024; ++tries) {
          bit = rng_.rnd(0, 31);
          if (0x8E8EA939u >> bit & 1u) break;
        }
        if (0x8E8EA939u >> bit & 1u) ok = apply_status_bits(target, 1u << bit, 0, element.status_pct());
      }
      say(std::format("{} casts {} on {}: {}", caster.name, nm, target_unit.name, ok ? "random status" : "no effect"));
      code_override = ok ? 0 : 2;
      break;
    }
    case 36: {  // rnd(0,99) < element record +0xA sets the lo bits of the target's innate colour, no further roll
      // Per colour: green white black red yellow blue.
      static constexpr u32 kCurse[6] = {0x901u, 0xE000u, 0xE0000u, 0x900008u, 0xE000010u, 0xA0000020u};
      bool ok = false;
      if (rng_.rnd(0, 99) < element.status_pct() && ct >= 0 && ct < 6) ok = force_status_bits(target, kCurse[static_cast<size_t>(ct)]);
      say(std::format("{} casts {} on {}: {}", caster.name, nm, target_unit.name, ok ? "colour curse" : "no effect"));
      code_override = ok ? 0 : 2;
      break;
    }
    case 45:  // Pip's form change (elements 0x80..0x85, power = the new kind); no damage list entry of its own
      evolve(actor, element.power());
      code_override = 1;
      break;
    case 50:  // a forced miss; the chain counter restarts
      say(std::format("{} uses {} on {}: miss", caster.name, nm, target_unit.name));
      chain_hits_ = 0;
      code_override = 2;
      break;
    case 6: {  // element record +0xE & 0xFC = a colour: the whole ring takes it; else the timed field state (1 weaker / 2 stronger elements)
      const int field_word = element.rd16(0xE);
      if (field_word & 0xFC) {
        const int colour = data::colour_index_of_bit(static_cast<u8>(field_word & 0xFC));
        ring_repaint_.fill(colour >= 0 && colour < 6 ? colour : -1);
        ring_done = true;
      } else if ((field_state_ & 3) == 0) {
        // The state's timer (20 PC actions) never clears it in retail.
        field_state_ = field_word & 0xFF;
      }
      say(std::format("{} uses {}: field {:#x}", caster.name, nm, field_word));
      code_override = 0;
      break;
    }
    case 42: {  // the caster's innate colour := the ring colour present 3 times, else twice, else a random colour on the ring
      int pick = -1;
      for (int need : {3, 2}) {
        for (int colour = 0; colour < 6; ++colour)
          if (std::count(ring_.begin(), ring_.end(), colour) == need) pick = colour;
        if (pick >= 0) break;
      }
      if (pick < 0 && std::any_of(ring_.begin(), ring_.end(), [](int colour) { return colour >= 0 && colour < 6; })) {
        // Retail draws rnd(0,5) until it hits a ring colour (with an empty ring it never ends; we leave the colour).
        int colour = 0;
        for (int tries = 0; tries < 1024; ++tries) {
          colour = rng_.rnd(0, 5);
          if (std::count(ring_.begin(), ring_.end(), colour) != 0) {
            pick = colour;
            break;
          }
        }
      }
      if (pick >= 0) caster.innate = static_cast<u8>((caster.innate & 3u) | (4u << pick));
      say(std::format("{} uses {}: colour {}", caster.name, nm, pick));
      code_override = 0;
      break;
    }
    case 5: {  // element trap; the caster's side waits for element record +0xE cast by the other side
      int& trap = trap_[caster.pc ? 0 : 1];
      if (trap != element.rd16(0xE)) {
        trap = element.rd16(0xE);
        trap_state_ = 2;
      }
      say(std::format("{} sets an element trap for {}", caster.name, element.rd16(0xE)));
      code_override = 0;
      break;
    }
    case 12:  // grid row of the cast < 2: no effect (code 2); else the target's slots of rows 0..row-1
              // lose their used bit: its lower levels are recharged
      if (row < 2) {
        code_override = 2;
      } else {
        for (int cell = 0; cell < 10 * row && cell < static_cast<int>(data::kGridCells); ++cell) target_unit.used[static_cast<size_t>(cell)] = false;
        code_override = 0;
      }
      say(std::format("{} uses {}: {}", caster.name, nm, row < 2 ? "no effect" : "lower levels recharged"));
      break;
    case 13: {  // gate rnd(1,99) <= +0xA, then the caster's inflicted negative statuses take the status roll on the target at +0xA % and
                // leave the caster (with their timers) whether they landed or not
      if (rng_.rnd(1, 99) > element.status_pct()) {
        say(std::format("{} casts {} on {}: no effect", caster.name, nm, target_unit.name));
        landed = false;
        code_override = 2;
        break;
      }
      const u32 moved = caster.lo & ~caster.perm_lo & kRevengeStatuses;
      apply_status_bits(target, moved, 0, element.status_pct());
      caster.lo &= ~moved;
      for (int bit = 0; bit < 32; ++bit)
        if (moved >> bit & 1u) caster.timers.erase(bit);
      say(std::format("{} casts {} on {}: statuses {:#x} passed on", caster.name, nm, target_unit.name, moved));
      code_override = 0;
      break;
    }
    case 46: {  // Piggy Boink: mode 32's ATK / DEF damage without the status roll, power + the piggy-bank savings; wakes the target.
                // The rnd(0,15) draw would double the savings when below a counter that the battle never raises.
      rng_.rnd(0, 15);
      ElementIn in = elem_in(32);
      in.power += opt_.piggy_bank;
      deal(in);
      wake();
      break;
    }
    case 47: {  // rnd(0,5) colours until one that some PC (units 0..2, innate colour mask >> 2) lacks; the whole field ring takes it; no damage
      int colour = 0;
      for (int tries = 0; tries < 1024; ++tries) {
        colour = rng_.rnd(0, 5);
        bool lacking = false;
        for (int pc = 0; pc < kPcSlots; ++pc)
          if (!((units_[static_cast<size_t>(pc)].innate >> 2) & (1u << colour))) lacking = true;
        if (lacking) break;
      }
      ring_repaint_.fill(colour);
      ring_done = true;
      say(std::format("{} uses {}: field colour {}", caster.name, nm, colour));
      code_override = 0;
      break;
    }
    case 48: {  // the target's HP digits h / 100, (h % 100) / 10, h % 10 swapped three times (positions rnd(0,2), rnd(0,2)); the actor
                // gets HP 2 v and takes v damage, so it ends at v (0 -> the actor keeps 1)
      std::array<int, 3> digits{target_unit.hp % 10, (target_unit.hp % 100) / 10, target_unit.hp / 100};
      for (int k = 0; k < 3; ++k) {
        const int i = rng_.rnd(0, 2), j = rng_.rnd(0, 2);
        std::swap(digits[static_cast<size_t>(i)], digits[static_cast<size_t>(j)]);
      }
      const int value = digits[0] + digits[2] * 100 + digits[1] * 10;
      caster.hp = std::min(2 * value, 0x7FFF);
      damage_unit(actor, actor, value, 0);
      say(std::format("{} uses {}: HP digits of {} -> {}", caster.name, nm, target_unit.name, value));
      code_override = 0;
      break;
    }
    case 49:  // the target's stamina := 70; in battle 0x1D8 a PC target's occupied slots lose their used bit
      target_unit.stamina = 70;
      // The port compares the scene id.
      if (scene_ == 0x1D8 && target_unit.pc)
        for (size_t cell = 0; cell < data::kGridCells; ++cell)
          if (target_unit.cells[cell] != 0) target_unit.used[cell] = false;
      say(std::format("{} uses {} on {}: stamina restored", caster.name, nm, target_unit.name));
      code_override = 0;
      break;
    case 31:  // no-op modes
    case 34:
    case 38:
      say(std::format("{} uses {} on {}: no effect (mode {})", caster.name, nm, target_unit.name, mode));
      code_override = 0;
      break;
    case 40: {  // Suicide Bomb (348): after the mode 0 hit pass, unless it missed, the damage of the caster
                // AND the target := the target's HP / 2 (toward zero). The caster is the dying
                // Bomber (HP 0 already, so it stays at 0), so only the target takes it here.
      // The hit pass's damage roll leaves its result code and the mode only rewrites the
      // damage, so a zero roll keeps code 1 ("no damage") and the HP / 2 number is shown with the no-damage digits.
      DamageOut damage = element_damage(rng_, elem_in(0));
      god_filter(target, damage);
      const int half_hp = target_unit.hp / 2;
      say(std::format("{} uses {} on {}: {} (half HP)", caster.name, nm, target_unit.name, half_hp));
      damage_unit(actor, target, half_hp, 0);
      code_override = damage.value == 0 && damage.code != 3 ? 1 : 0;
      break;
    }
    case 41:  // the caster's control word := script var 45 (nothing else); the Mode Change packs read it back with script op 0x9F
      caster.ctrl12 = brain_.script_var(0xDE) & 0xFFFF;
      say(std::format("{} uses {}: no effect (mode 41)", caster.name, nm));
      code_override = 0;
      break;
    case 30: {  // pilfer: one steal per enemy; a row of the drop table, slot +4 (common) / +6 (rare)
      // Already stolen -> result 3 (nothing); the chance is the PC's heavy-attack hit chance; the caster / target gender
      // test (caster male -> target male, else target female) doubles it (byte arithmetic); element record +0xE != 0 then adds the partner's
      // chance (only the scripted steal, mode 44, names one) and raises the rare chance from 10 to 20; row = the enemy's drop row & 0x3FFF,
      // both words empty -> result 3; rnd(0,100) >= chance -> result 2 (a miss); rnd(0,99) < rare chance -> slot +6 else +4; the word's bit 15 = an
      // element (id & 0x7FFF), else an item id. The loot goes into the stock at battle end on a victory only.
      // Every non-success path (already stolen, empty row, failed roll) sets the same result code 2 in the damage list (the MISS pop) and
      // the message row 3 ('Nothing to steal!') or 2 ('Attempt at thievery failed!') in the record.
      code_override = 2;
      steal_msg = 3;
      if (target_unit.stolen) {
        say(std::format("{} uses {} on {}: nothing left to steal", caster.name, nm, target_unit.name));
        break;
      }
      // The chance byte is NOT the caster's stored heavy-hit byte: the command confirm writes the HEAVY attack's HIT% of the box
      // (strength 2: capped at 99, minus the target's evade, plus the chain bonus) - the figure the HIT% box shows.
      // hit_chance_for(.., 2) is that figure incl. the running chain.
      u8 chance = static_cast<u8>(std::clamp(hit_chance_for(actor, target, 2), 0, 255));
      const bool opposite = (caster.gender & 0x4000) == 0 ? (target_unit.gender & 0x4000) != 0 : (target_unit.gender & 0x8000) != 0;
      int rare = 10;
      if (opposite) {
        chance = static_cast<u8>(chance * 2);
        // The partner's chance is not added: only the scripted steal (mode 44) names one.
        if (element.rd16(0xE) != 0) rare = 20;
      }
      const size_t drop_index = static_cast<size_t>(target_unit.drop_row & 0x3FFF);
      if (drop_index >= tables_.drops.size() || (tables_.drops[drop_index].slot[2] == 0 && tables_.drops[drop_index].slot[3] == 0)) {
        say(std::format("{} uses {} on {}: nothing to steal", caster.name, nm, target_unit.name));
        break;
      }
      if (!(cheats_.always_loot && caster.pc) && rng_.rnd(0, 100) >= chance) {
        say(std::format("{} uses {} on {}: the steal fails", caster.name, nm, target_unit.name));
        steal_msg = 2;
        break;
      }
      const bool rare_roll = rng_.rnd(0, 99) < rare;
      // Cheats: always loot -> the rare word.
      const u16 word = tables_.drops[drop_index].slot[(rare_roll || (cheats_.always_loot && caster.pc && tables_.drops[drop_index].slot[3] != 0)) ? 3 : 2];
      target_unit.stolen = true;
      code_override = 0;
      steal_word = word;
      // Message drop_index: 0 item, 1 element.
      steal_msg = (word & 0x8000) ? 1 : 0;
      rewards_.stolen.push_back(word);
      say(std::format("{} steals {} from {}", caster.name, (word & 0x8000) ? tables_.element_name(word & 0x7FFF) : tables_.item_name(word), target_unit.name));
      break;
    }
    case 44:  // actor := unit 0, partner := unit 4 for a scripted steal (mode 30 reads it): no damage
      say(std::format("{} uses {} on {}", caster.name, nm, target_unit.name));
      code_override = 0;
      break;
    case 14:  // by the target's slot: an enemy slot takes mode 0's hit pass, a party slot the mode 3 heal (Saints hurts the enemies and
              // heals the party, whoever casts it)
      if (!is_pc_slot(target)) {
        hit_pass();
        break;
      }
      [[fallthrough]];
    case 3:
    case 43: {
      // Mode 3 heals by the formula, mode 43 (the consumables) by the flat +0x0A; both then x125/100 with the caster's damage-up gear and
      // x75/100 with the target's damage-down gear. Mode 43's zero is "no damage" (code 1).
      int amount = mode == 43 ? element.status_pct() : heal_amount(rng_, caster.mag, element.status_pct(), slot, cmul);
      if (caster.eq.gear_flags & data::gearflag::kDmgUp) amount = amount * 125 / 100;
      if (target_unit.eq.gear_flags & data::gearflag::kDmgDown) amount = amount * 75 / 100;
      code_override = mode == 43 && amount == 0 ? 1 : heal_code();
      if (target_unit.lo & st::kKO) say(std::format("{} casts {} on {}: {} is down, no effect", caster.name, nm, target_unit.name, target_unit.name));
      else say(std::format("{} casts {} on {}: heals {}", caster.name, nm, target_unit.name, amount));
      damage_unit(actor, target, amount, heal_code());
      // The formula heal also cures the record's statuses, gives the target power x 10 stamina and recharges each of its used cells
      // with chance +0x0C % (one rnd(0, 99) per cell, used or not).
      if (mode == 3 || mode == 14) {
        cure();
        target_unit.stamina = cap_stamina(target_unit.stamina + element.power() * 10);
        for (size_t cell = 0; cell < data::kGridCells; ++cell)
          if (rng_.rnd(0, 99) < element.variance() && target_unit.used[cell]) target_unit.used[cell] = false;
      }
      break;
    }
    case 1:  // +0xE bit 0 clear: the status roll decides `landed`; set: the statuses are cured instead (the consumables, Purify) and
             // `landed` stays true
      say(std::format("{} casts {} on {} (status only)", caster.name, nm, target_unit.name));
      if (element.rd16(0xE) & 1) cure();
      else landed = apply_status(target, element);
      break;
    case 4: {
      if (!(target_unit.lo & st::kKO)) {
        say(std::format("{} casts {}: {} is not down", caster.name, nm, target_unit.name));
        break;
      }
      int pct = 15;
      if (element.hits() != 0) pct = 100;
      else if (!caster.pc) pct = 15;
      else {
        static const int kTab[16] = {30, 20, 20, 20, 10, 10, 10, 5, 30, 40, 50, 60, 70, 80, 90, 100};
        const int native = element.level() - 1;
        const int distance = std::abs(row - native);
        pct = kTab[std::clamp(distance + 8 * (row > native ? 1 : 0), 0, 15)];
      }
      target_unit.lo &= ~st::kKO;
      target_unit.hp = std::max(1, target_unit.max_hp * pct / 100);
      say(std::format("{} casts {}: {} revives with {} HP", caster.name, nm, target_unit.name, target_unit.hp));
      break;
    }
    case 7: {  // the +0x18 / +0x1C statuses at the +0xA chance, skip / immune masks; none landed -> code 2;
               // landed -> each stat of (+0x1C & 0xFC0) >> 6 takes power %. With +0xE != 0 that part needs a target wearing the hero set.
               // Then +0xC != 0 heals by the mode-3 formula with +0xC as its power, landed or not.
      if (element.rd16(0xE) == 0 || target_unit.hero_set) {
        landed = apply_status(target, element);
        if (!landed) {
          say(std::format("{} casts {} on {}: no effect", caster.name, nm, target_unit.name));
          code_override = 2;
        } else {
          say(std::format("{} casts {} on {}: stat modifier {}%", caster.name, nm, target_unit.name, element.power()));
          const u32 bits = (element.status_hi() & 0xFC0u) >> 6;
          for (int k = 0; k < 6; ++k)
            if (bits >> k & 1u) target_unit.mod[static_cast<size_t>(k)] = element.power();
        }
      }
      if (element.variance() != 0) {
        int amount = heal_amount(rng_, caster.mag, element.variance(), slot, cmul);
        if (caster.eq.gear_flags & data::gearflag::kDmgUp) amount = amount * 125 / 100;
        if (target_unit.eq.gear_flags & data::gearflag::kDmgDown) amount = amount * 75 / 100;
        say(std::format("{} casts {} on {}: heals {}", caster.name, nm, target_unit.name, amount));
        damage_unit(actor, target, amount, heal_code());
        code_override = heal_code();
      }
      break;
    }
    case 51:  // mode 29 when the caster wears the hero set, else a miss
      if (!caster.hero_set) {
        say(std::format("{} uses {} on {}: no effect without the hero set", caster.name, nm, target_unit.name));
        landed = false;
        code_override = 2;
        break;
      }
      [[fallthrough]];
    case 29: {  // weapon tech: weapon formula per hit, no hit roll
      // Hit k is dealt by participant k of a double / triple or group tech, else every hit by the caster.
      const std::array<int, 6> participants = tech_participants(actor, element);
      const int hits = std::max<int>(1, element.hits());
      int total = 0;
      bool any = false;
      for (int hit = 0; hit < hits; ++hit) {
        const int hitter = participants[0] >= 0 && hit < static_cast<int>(participants.size()) ? participants[static_cast<size_t>(hit)] : -1;
        const Unit& attacker = hitter >= 0 ? units_[static_cast<size_t>(hitter)] : caster;
        WeaponIn input;
        input.atk = attacker.atk;
        input.wp = weapon_term(attacker);
        input.wp_var = attacker.eq.wp_var;
        input.def = target_unit.def;
        input.arm = ceil100(target_unit.eq.arm, target_unit.mod[2]);
        input.arm_var = target_unit.eq.arm_var;
        input.arm_soft = (target_unit.lo & 0x4008) != 0;
        input.mult = element.power();
        // Element record +0xC is NOT a damage floor: the raw damage is applied as computed (e.g. Headbutt's raw 5 stays 5).
        // Like every element command the hit takes the record's colour (not the caster's), without the field state step.
        const int attacker_colour = colour_of(attacker) < 0 ? kNoColour : colour_of(attacker);
        input.colour_mille = colour_multiplier(attacker_colour, ce_pool, ring_, ct);
        input.strength = 1;
        input.weapon_class = attacker.eq.wp_class;
        input.slayer = (attacker.eq.slayer & target_unit.hi) != 0;
        input.resisted = (target_unit.eq.guard_hi & attacker.hi) != 0;
        input.gear_up = (attacker.eq.gear_flags & data::gearflag::kDmgUp) != 0;
        input.gear_down = (target_unit.eq.gear_flags & data::gearflag::kDmgDown) != 0;
        input.target_afraid = (target_unit.lo & st::kAfraid) != 0;
        input.opposite_colour = ct >= 0 && ce >= 0 && kOpp[static_cast<size_t>(ct)] == ce;
        input.guarding = target_unit.pc && target_unit.guard;
        const DamageOut damage = weapon_damage(rng_, input);
        total += damage.value;
        any = true;
      }
      say(std::format("{} uses {} on {}: {} ({} hits)", caster.name, nm, target_unit.name, any ? total : 0, hits));
      damage_unit(actor, target, total, total == 0 ? 1 : 0);
      break;
    }
    default:
      say(std::format("{} uses {} on {}: mode {} not implemented (no effect)", caster.name, nm, target_unit.name, mode));
      note_stub(std::format("element mode {}", mode));
      break;
  }
  ActionEvent ev;
  ev.kind = ActionEvent::Kind::Element;
  ev.actor = actor;
  ev.death_cast = in_death_cast_;
  ev.target = target;
  ev.element = element_id;
  ev.row = row;
  ev.hp_before = hp0;
  ev.hp_after = target_unit.hp;
  ev.ko = alive0 && !target_unit.alive();
  ev.ko_owed = ev.ko && death_pending_[static_cast<size_t>(target)];
  ev.value = std::abs(target_unit.hp - hp0);
  const bool friendly_mode = mode == 3 || mode == 43 || mode == 4 || mode == 7;
  ev.code = target_unit.hp > hp0 || friendly_mode ? 3 : 0;
  if (code_override >= 0) ev.code = code_override;
  ev.all_targets = cast_all_;
  ev.silent = mode == 50 || mode == 45;
  if (mode == 45) ev.evolve_form = element.power();
  if (mode == 39) ev.creature = creature_model_;
  ev.steal_msg = steal_msg;
  ev.steal_word = steal_word;
  ev.ring_repaint = ring_done;
  ev.seq_colour = seq_colour(element_id);
  ev.colour_seq = brain_.colour_sequence();
  // After the target loop: Frozen (lo 0x20000000, `kDefeated` here: Ice Blast's status) newly set and the
  // target's HP != 0 -> row 6 'Frozen -- can't move!'; else element record +0x13 != 0 and
  // the mode landed on any target -> that row (opened at the action end for 120 ticks)
  if (!frozen0 && (target_unit.lo & 0x20000000u) && target_unit.hp != 0) ev.sys_msg = 6;
  else if (element.b(0x13) != 0 && landed) ev.sys_msg = element.b(0x13);
  if (log_.size() > log0) ev.text = log_[log0];
  note_event(ev);
  if (ring_done) ring_repainted_ = true;
  if (push_ring) finish_ring(element, ring_colour_of(element_colour(element_id)));
}

void Battle::cast_element(int actor, int element_id, int row, int target, u32 targets) {
  if (element_id < 0 || element_id >= static_cast<int>(tables_.elements.size())) return;
  const data::ElemRec& element = tables_.elements[static_cast<size_t>(element_id)];
  ring_repainted_ = false;
  fizzled_ = false;
  if (element_id == kVaryInnateElement) vary_innate_colour_ = static_cast<u8>(units_[static_cast<size_t>(actor)].innate & 0xFC);
  // The creature state is cleared for each cast.
  creature_state_ = 0, creature_model_ = 0, creature_slot_ = 0;
  {  // Before every cast: an element the other side has trapped (mode 5) fizzles and clears the trap
    Unit& caster = units_[static_cast<size_t>(actor)];
    int& trap = trap_[caster.pc ? 1 : 0];
    if (trap != 0 && trap == element_id) {
      fizzled_ = true;
      trap_state_ = caster.pc ? 4 : 3;
      if (caster.pc) say(std::format("{} casts {}: caught in the element trap, the element is lost", caster.name, tables_.element_name(element_id)));
      else {
        // The PC side's trap takes the element into the stock.
        rewards_.trapped.push_back(static_cast<u16>(element_id));
        say(std::format("{} casts {}: caught in the element trap, the element is taken", caster.name, tables_.element_name(element_id)));
      }
      ActionEvent ev;
      ev.kind = ActionEvent::Kind::Element;
      ev.actor = actor;
      ev.death_cast = in_death_cast_;
      ev.target = target;
      ev.element = element_id;
      ev.row = row;
      ev.code = 2;
      ev.caught = true;
      ev.hp_before = ev.hp_after = units_[static_cast<size_t>(target)].hp;
      ev.text = log_.back();
      note_event(ev);
      trap = 0;
      return;
    }
  }
  // Element record byte 4 bit 0x10: every unit of the target's side.
  if (!(element.b(4) & 0x10)) {
    // A scripted cast at several units: each of them that is still standing (KO'd for a KO-only element) takes the cast, then the ring
    // takes the colour once.
    if (std::popcount(targets) > 1) {
      int hits = 0;
      for (int i = 0; i < kUnits; ++i) {
        const Unit& unit = units_[static_cast<size_t>(i)];
        if (!(targets >> i & 1u) || !unit.present) continue;
        if (target_spec(element).ko_only || element.mode() == 4 ? !(unit.lo & st::kKO) : !unit.alive()) continue;
        do_element(actor, element_id, row, i, false);
        ++hits;
        if (outcome_ != Outcome::Ongoing) break;
      }
      if (hits > 0) {
        finish_ring(element, ring_colour_of(element_colour(element_id)));
        return;
      }
    }
    do_element(actor, element_id, row, target, true);
    return;
  }
  // 0x10 with both side bits (Seal-All, the colour fields, Saints, Chrono Cross) covers all nine units (mask 0x1FF); otherwise the target's side
  const bool both_sides = target_spec(element).party && target_spec(element).enemies;
  const bool party_side = is_pc_slot(target);
  int hits = 0;
  cast_all_ = true;
  for (int i = both_sides ? 0 : party_side ? 0 : kPcSlots; i < (both_sides ? kUnits : party_side ? kPcSlots : kUnits); ++i) {
    const Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.present || (target_spec(element).ko_only || element.mode() == 4 ? !(unit.lo & st::kKO) : !unit.alive())) continue;
    // Enemy-side masks are ANDed with the cast mask (off-stage units).
    if (!is_pc_slot(i) && !(cast_mask_ >> i & 1u)) continue;
    do_element(actor, element_id, row, i, false);
    ++hits;
    if (outcome_ != Outcome::Ongoing) break;
  }
  cast_all_ = false;
  if (hits == 0) {
    do_element(actor, element_id, row, target, true);
    return;
  }
  finish_ring(element, ring_colour_of(element_colour(element_id)));
}

// ------------------------------------------------------------------------------------------------ per-action upkeep
void Battle::step_after_pc_action(int actor, int steps, bool weapon, u32 excluded) {
  const int recovery_steps = weapon ? steps : 1;
  for (int i = 0; i < kPcSlots; ++i) {
    Unit& unit = units_[static_cast<size_t>(i)];
    if (i == actor || (excluded >> i & 1u) || !unit.alive()) continue;
    unit.stamina = cap_stamina(unit.stamina + recovery_step(unit.rate, 0, recovery_steps, (unit.lo & st::kFlu) != 0));
  }
  for (int i = kPcSlots; i < kUnits; ++i) {
    Unit& unit = units_[static_cast<size_t>(i)];
    if (unit.alive() && unit.countdown != 0xFFFF) unit.countdown = std::max(0, unit.countdown - steps);
  }
}

void Battle::post_action(int actor, int poison_step, int timer_steps) {
  (void)actor;
  ++actions_;
  for (int i = 0; i < kUnits; ++i) {
    Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.alive()) continue;
    if (unit.lo & st::kPoison) {
      unit.hp -= poison_step;
      if (unit.hp <= 0) {
        if (unit.pc) kill(i);
        else unit.hp = 1;
      }
    }
    if (unit.alive() && (unit.hi & st::kHiRegen)) unit.hp = std::min(unit.max_hp, unit.hp + poison_step);
    for (auto it = unit.timers.begin(); it != unit.timers.end();) {
      it->second -= timer_steps;
      if (it->second <= 0) {
        const int set = it->first / 32, bit = it->first % 32;
        (set == 0 ? unit.lo : unit.hi) &= ~(1u << bit);
        if (set == 1 && bit >= 6 && bit <= 11) unit.mod[static_cast<size_t>(bit - 6)] = 100;
        it = unit.timers.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void Battle::time_advance() {
  apply_cheat_state();
  int step = 255;
  for (int i = kPcSlots; i < kUnits; ++i) {
    const Unit& unit = units_[static_cast<size_t>(i)];
    // Hidden (off-stage) units do not count.
    if (unit.alive() && unit.countdown > 0 && unit.countdown != 0xFFFF && !brain_.hidden(i)) step = std::min(step, unit.countdown);
  }
  for (int i = 0; i < kUnits; ++i) {
    Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.alive()) continue;
    if (unit.countdown != 0xFFFF) unit.countdown = std::max(0, unit.countdown - step);
    if (unit.pc) unit.stamina = cap_stamina(unit.stamina + step * (unit.rate));
    // The time step is what a lone defender waits for before its menu can reopen.
    if (unit.pc) unit.defending = false;
  }
}

// ------------------------------------------------------------------------------------------------ scheduler
int Battle::next_ready_enemy() {
  for (int k = 0; k < kEnemySlots; ++k) {
    const int slot = kPcSlots + (cursor_ + k) % kEnemySlots;
    const Unit& unit = units_[static_cast<size_t>(slot)];
    if (unit.alive() && unit.countdown == 0) {
      cursor_ = (slot - kPcSlots + 1) % kEnemySlots;
      return slot;
    }
  }
  return -1;
}

std::pair<int, int> Battle::exec_action(int ui, const EnemyAction& act) {
  Unit& unit = units_[static_cast<size_t>(ui)];
  int tgt = act.target;
  if (tgt < 0 || tgt >= kUnits || !units_[static_cast<size_t>(tgt)].present) tgt = 0;
  int poison_step = 1, timer_steps = 1;
  switch (act.kind) {
    case EnemyAction::Kind::Weapon:
      if (!units_[static_cast<size_t>(tgt)].alive()) {
        for (int i = 0; i < kPcSlots; ++i)
          if (units_[static_cast<size_t>(i)].alive()) {
            tgt = i;
            break;
          }
      }
      do_weapon_attack(ui, tgt, std::clamp(act.strength, 0, 2));
      poison_step = timer_steps = std::clamp(act.strength, 0, 2) + 1;
      break;
    case EnemyAction::Kind::Element:
      if (act.element > 0 && act.element < static_cast<int>(tables_.elements.size()))
        cast_element(ui, act.element, std::max(0, tables_.elements[static_cast<size_t>(act.element)].level() - 1), tgt, act.targets);
      poison_step = 7;
      break;
    case EnemyAction::Kind::None: say(std::format("{} waits", unit.name)); break;
  }
  return {poison_step, timer_steps};
}

void Battle::run_enemy_action(int ui, const EnemyAction& act) {
  Unit& unit = units_[static_cast<size_t>(ui)];
  const auto [poison_step, timer_steps] = exec_action(ui, act);
  // The same countdown reset a script's op 67 type 4 does.
  unit.countdown = unit.ap + rng_.rnd(0, unit.ap >> 2);
  post_action(ui, poison_step, timer_steps);
}

// Executes a scripted plan (several queued attacks = one turn). Stops early when the battle ends or the actor dies.
bool Battle::run_plan(int ui, const std::vector<EnemyAction>& plan, bool set_countdown) {
  Unit& unit = units_[static_cast<size_t>(ui)];
  int poison_step = 1, timer_steps = 1;
  if (plan.empty()) say(std::format("{} waits", unit.name));
  for (const EnemyAction& action : plan) {
    if (outcome_ != Outcome::Ongoing || !unit.alive()) break;
    cur_script_seq_ = action.seq;
    cur_group_ = action.group;
    std::tie(poison_step, timer_steps) = exec_action(ui, action);
    cur_script_seq_ = -1;
    cur_group_.fill(0xFF);
    check_end();
  }
  if (set_countdown) unit.countdown = unit.ap + rng_.rnd(0, unit.ap >> 2);
  post_action(ui, poison_step, timer_steps);
  return true;
}

void Battle::enemy_turn(int ui) {
  begin_action();
  units_[static_cast<size_t>(ui)].countdown = 0xFFFF;
  std::vector<EnemyAction> plan;
  if (brain_.plan_turn(*this, ui, rng_, plan)) {
    run_plan(ui, plan, true);
    return;
  }
  EnemyAction act = brain_.choose(*this, ui, rng_);
  run_enemy_action(ui, act);
}

void Battle::confused_turn(int ui) {
  begin_action();
  Unit& unit = units_[static_cast<size_t>(ui)];
  {
    std::vector<EnemyAction> plan;
    if (brain_.plan_turn(*this, ui, rng_, plan) && !plan.empty()) {
      say(std::format("{} is confused", unit.name));
      run_plan(ui, plan, false);
      return;
    }
  }
  std::vector<int> cands;
  for (int i = 0; i < kUnits; ++i)
    if (i != ui && units_[static_cast<size_t>(i)].alive()) cands.push_back(i);
  if (cands.empty()) return;
  const int tgt = cands[static_cast<size_t>(rng_.rnd(0, static_cast<int>(cands.size()) - 1))];
  say(std::format("{} is confused", unit.name));
  do_weapon_attack(ui, tgt, 0);
  post_action(ui, 1, 1);
}

// Cheats tab: Max stamina keeps the PCs' stamina full; Battle boost (gauge to max, negative statuses cured)
// refills HP, gauge and stamina and clears the negative statuses of every PC (KO itself is prevented in kill() through invincible()).
void Battle::apply_cheat_state() {
  if (!cheats_.max_stamina && !cheats_.boost && !cheats_.god_mode) return;
  for (int i = 0; i < kPcSlots; ++i) {
    Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.present || !unit.pc) continue;
    // God mode: a KO'd PC is revived, HP full, negative statuses cleared (mask 0xAA9EE939, which holds frozen, so a frozen PC is
    // freed too); hits do no damage (god_filter). Boost = God mode + gauge + stamina.
    if (cheats_.god_mode || cheats_.boost) {
      unit.lo &= ~st::kKO;
      unit.hp = unit.max_hp;
      constexpr u32 kNegative = 0xAA9EE939u;
      unit.lo &= ~kNegative;
      for (auto it = unit.timers.begin(); it != unit.timers.end();)
        it = (it->first < 32 && (kNegative >> it->first & 1)) ? unit.timers.erase(it) : std::next(it);
    }
    if (!unit.alive()) continue;
    if (cheats_.max_stamina || cheats_.boost) unit.stamina = 70;
    if (cheats_.boost) unit.gauge = unit.gauge_max;
  }
}

// Nothing while script op 81 has switched the test off; a code forced by op 83 (negative = pending) ends the battle first, but not inside a
// death plan: the kill script forces its code after its end-turn op, i.e. once the executor has run every cast it queued (the Time Devourer's
// 341 then 526); then the party test (every PC in the alive mask KO'd / defeated -> 1) comes before the enemy test (-> 0), so a mutual KO is a defeat.
void Battle::check_end() {
  apply_cheat_state();
  if (outcome_ != Outcome::Ongoing) return;
  if (!brain_.end_check_enabled()) return;
  if (const int forced = brain_.forced_result(); forced >= 0 && !in_death_cast_) {
    finish(forced == 0 ? Outcome::Victory : forced == 1 ? Outcome::Wiped : forced == 2 ? Outcome::Fled : Outcome::Scripted, forced);
    return;
  }
  if (alive_count(true) == 0) {
    finish(Outcome::Wiped);
    return;
  }
  // An enemy in the alive mask blocks victory when its death is still owed (op 5C: its kill script still owes the death; the mask bit
  // goes with the script's A6) or it is not off-stage (ops 34 / 35 / 5B). The port's mask = alive(), plus the KO'd units whose
  // death is still owed (retail keeps them in the mask until A6).
  for (int i = kPcSlots; i < kUnits; ++i) {
    const Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.present) continue;
    // A KO'd unit is in the mask only until its death script's A6 clears the bit. The
    // brain's op-5C flag is NOT that test: a slot-1 idle loop (e.g. `5C 0 e3; halt`) stores it again every pass after A6, which would hold the victory
    // for ever.
    const bool owed = death_pending_[static_cast<size_t>(i)];
    if ((unit.alive() || owed) && (owed || !brain_.hidden(i))) return;
  }
  finish(Outcome::Victory);
}

void Battle::finish_external_action() {
  run_reactions();
  chain_actor_ = chain_target_ = -1;
  chain_hits_ = 0;
  check_end();
}

void Battle::settle() {
  for (int guard = 0; guard < 20000 && outcome_ == Outcome::Ongoing; ++guard) {
    check_end();
    if (outcome_ != Outcome::Ongoing) return;
    bool acted = false;
    for (int i = 0; i < kPcSlots; ++i) {
      Unit& unit = units_[static_cast<size_t>(i)];
      if (unit.alive() && unit.countdown == 0) {
        unit.countdown = unit.ap;
        if (evo_pending_ && !(unit.lo & st::kConfused)) {
          // With a pending form element the PC's turn is its script slot 2: Pip (kind 0x2B..0x2D) casts the pending form
          // element on himself (`66 87 67 01 v[A0] self`), any other PC ends its turn (A4)
          begin_action();
          std::vector<EnemyAction> plan;
          brain_.plan_turn(*this, i, rng_, plan);
          run_plan(i, plan, false);
          run_reactions();
          chain_actor_ = chain_target_ = -1;
          chain_hits_ = 0;
          acted = true;
        } else if (unit.lo & st::kConfused) {
          confused_turn(i);
          run_reactions();
          chain_actor_ = chain_target_ = -1;
          chain_hits_ = 0;
          acted = true;
        }
      }
    }
    if (acted) continue;
    const int enemy = next_ready_enemy();
    if (enemy >= 0) {
      enemy_turn(enemy);
      run_reactions();
      chain_actor_ = chain_target_ = -1;
      chain_hits_ = 0;
      continue;
    }
    // No menu opens while a form change is pending, so time runs on until the PCs' countdowns give them their scripted turns.
    for (int i = 0; i < kPcSlots; ++i)
      if (can_act(i)) return;
    time_advance();
  }
  if (outcome_ == Outcome::Ongoing) finish(Outcome::Stalled);
}

// ------------------------------------------------------------------------------------------------ command application
bool Battle::apply(const Command& cmd, std::string* why) {
  if (cmd.kind == Command::Kind::Element) open_element_menu(cmd.actor);
  if (!validate(cmd, why)) return false;
  attacked_.clear();
  begin_action();
  Unit& unit = units_[static_cast<size_t>(cmd.actor)];
  unit.guard = false;
  // A menu was used: every "defended" flag is released (the next PC is picked without looking at the flag, so a defender's menu reopens
  // once another PC has acted).
  for (int i = 0; i < kPcSlots; ++i) units_[static_cast<size_t>(i)].defending = false;
  // The automatic target: the weakest unit the cursor ring could reach (hidden units are never on it).
  auto pick_enemy = [&]() {
    if (cmd.target >= kPcSlots && units_[static_cast<size_t>(cmd.target)].alive() && !brain_.untargetable(cmd.target)) return cmd.target;
    int best = -1;
    for (int i = kPcSlots; i < kUnits; ++i)
      if (units_[static_cast<size_t>(i)].alive() && !brain_.untargetable(i) && (best < 0 || units_[static_cast<size_t>(i)].hp < units_[static_cast<size_t>(best)].hp)) best = i;
    return best;
  };

  switch (cmd.kind) {
    case Command::Kind::Attack: {
      int tgt = pick_enemy();
      for (int strength : cmd.chain) {
        if (tgt < 0 || !units_[static_cast<size_t>(tgt)].alive()) tgt = pick_enemy();
        if (tgt < 0) break;
        unit.stamina -= chain_cost(strength, unit);
        do_weapon_attack(cmd.actor, tgt, strength);
        step_after_pc_action(cmd.actor, strength + 1, true);
        post_action(cmd.actor, strength + 1, strength + 1);
        check_end();
        if (outcome_ != Outcome::Ongoing || !unit.alive()) break;
      }
      break;
    }
    case Command::Kind::Element: {
      const ComboTech combo = combo_at(cmd.actor, cmd.cell);
      const int id = combo.element ? combo.element : unit.cells[static_cast<size_t>(cmd.cell)] & 0x3FFF;
      const int row = cmd.cell / data::kGridCols;
      const data::ElemRec& element = tables_.elements[static_cast<size_t>(id)];
      int tgt = cmd.target;
      const TargetSpec ts = target_spec(element);
      if (tgt < 0 || !units_[static_cast<size_t>(tgt)].present) {
        if (ts.self_only) {
          tgt = cmd.actor;
        } else if (ts.party && !ts.enemies) {
          tgt = cmd.actor;
          for (int i = 0; i < kPcSlots; ++i)
            if (units_[static_cast<size_t>(i)].alive() &&
                units_[static_cast<size_t>(i)].hp * units_[static_cast<size_t>(tgt)].max_hp <
                    units_[static_cast<size_t>(tgt)].hp * units_[static_cast<size_t>(i)].max_hp)
              tgt = i;
        } else {
          tgt = pick_enemy();
        }
      }
      if (tgt < 0) break;
      // Cheats: free elements stay usable.
      unit.used[static_cast<size_t>(cmd.cell)] = !cheats_.free_elements;
      // Consumable: the stack counts down and the cell is marked used only when the last copy goes.
      if ((element.flags0() & 0x10) && !cheats_.free_elements) {
        u8& quantity = unit.qty[static_cast<size_t>(cmd.cell)];
        if (quantity > 1) {
          --quantity;
          unit.used[static_cast<size_t>(cmd.cell)] = false;
        } else {
          quantity = 0;
          unit.usable[static_cast<size_t>(cmd.cell)] = false;
        }
        if (unit.form < 0 && static_cast<size_t>(cmd.actor) < party_.size()) party_[static_cast<size_t>(cmd.actor)].qty[static_cast<size_t>(cmd.cell)] = quantity;
      }
      // A double / triple tech: every casting PC gives up its cell, and the casters are the action's caster group (the effect's 'tech' /
      // 'tec1' / 'tec2' clips go to entries 0 / 1 / 2).
      u32 casters = 1u << cmd.actor;
      if (combo.element) {
        std::array<u8, 6> group{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        for (size_t k = 0; k < combo.units.size(); ++k) {
          if (combo.units[k] < 0) continue;
          casters |= 1u << combo.units[k];
          group[k] = static_cast<u8>(combo.units[k]);
          units_[static_cast<size_t>(combo.units[k])].used[static_cast<size_t>(combo.cells[k])] = !cheats_.free_elements;
        }
        cur_group_ = group;
      }
      cast_element(cmd.actor, id, row, tgt);
      cur_group_.fill(0xFF);
      // The enemy's trap destroys the cell (battle copy and the save record: cell &= 0xC000).
      if (fizzled_) {
        unit.cells[static_cast<size_t>(cmd.cell)] &= 0xC000;
        unit.usable[static_cast<size_t>(cmd.cell)] = false;
        if (unit.form < 0 && static_cast<size_t>(cmd.actor) < party_.size()) party_[static_cast<size_t>(cmd.actor)].cells[static_cast<size_t>(cmd.cell)] &= 0xC000;
        // A creature form's grid is not a save cell.
        if (unit.form < 0) rewards_.lost_cells.emplace_back(cmd.actor, cmd.cell);
      }
      if (!combo.element) {
        unit.stamina = cap_stamina(unit.stamina - kElementCost);
        if (!cheats_.free_elements) unit.gauge = std::max(-1, unit.gauge - (row + 1));
      } else {
        // Every casting PC pays the element's stamina. Caster entry k's gauge becomes PARTY SLOT k's gauge minus the caster's own cell
        // level, clamped to -1 .. slot k's top (entries in order, each reading the gauges as the earlier ones left them), so a caster that
        // is not in slot k takes another PC's gauge.
        for (size_t k = 0; k < combo.units.size(); ++k) {
          if (combo.units[k] < 0) continue;
          Unit& caster = units_[static_cast<size_t>(combo.units[k])];
          caster.stamina = cap_stamina(caster.stamina - kElementCost);
          if (cheats_.free_elements) continue;
          const Unit& slot_unit = units_[k];
          caster.gauge = std::max(-1, std::min(slot_unit.gauge - (combo.cells[k] / data::kGridCols + 1), slot_unit.gauge_max));
        }
      }
      step_after_pc_action(cmd.actor, 1, false, casters | (is_pc_slot(tgt) ? 1u << tgt : 0u));
      post_action(cmd.actor, 7, 1);
      break;
    }
    case Command::Kind::Defend:
      unit.stamina = std::min(70, unit.stamina + 10);
      unit.guard = true;
      // The menu passes to the next PC (order pc+1, pc+2, pc).
      unit.defending = true;
      say(std::format("{} defends", unit.name));
      {
        ActionEvent ev;
        ev.kind = ActionEvent::Kind::Defend;
        ev.actor = cmd.actor;
        ev.text = log_.back();
        note_event(ev);
      }
      step_after_pc_action(cmd.actor, 1, false);
      post_action(cmd.actor, 1, 1);
      break;
    case Command::Kind::Run:
      say(std::format("{} flees", unit.name));
      {
        ActionEvent ev;
        ev.kind = ActionEvent::Kind::Run;
        ev.actor = cmd.actor;
        ev.text = log_.back();
        // Running away with pilfered loot drops it: the loot is gone and the system window shows row 5 ('Dropped the stolen goods!').
        if (!rewards_.stolen.empty()) {
          rewards_.stolen.clear();
          ev.sys_msg = 5;
        }
        note_event(ev);
      }
      finish(Outcome::Fled);
      break;
  }
  // The chain bonus survives between single-strength commands of the same PC / target (the menu applies each attack at
  // once); anything else the PC does, or any enemy / confused turn (settle), ends the chain.
  if (cmd.kind != Command::Kind::Attack) {
    chain_actor_ = chain_target_ = -1;
    chain_hits_ = 0;
  }
  run_reactions();
  if (outcome_ == Outcome::Ongoing) settle();
  return true;
}

// ------------------------------------------------------------------------------------------------ end of battle
void Battle::finish(Outcome outcome, int code) {
  if (outcome_ != Outcome::Ongoing) return;
  outcome_ = outcome;
  result_code_ = code >= 0 ? code : static_cast<int>(outcome);
  // A monster-party fight pays nothing and leaves the party records as they were.
  if (outcome == Outcome::Victory && !monster_party_) compute_rewards();
  for (size_t i = 0; i < party_.size() && !monster_party_; ++i) {
    const Unit& unit = units_[i];
    PartyMember& member = party_[i];
    member.hp = std::min(unit.hp, member.stat[0]);
    if (member.hp <= 0) member.hp = 1;
    member.lo_persist = unit.lo & st::kPersist;
  }
  say(std::format("battle over: {} (code {})",
                  outcome == Outcome::Victory ? "victory" : outcome == Outcome::Wiped ? "party wiped" : outcome == Outcome::Fled ? "fled" : outcome == Outcome::Scripted ? "scripted" : "stalled",
                  result_code_));
}

void Battle::compute_rewards() {
  int gold = 0;
  for (int i = kPcSlots; i < kUnits; ++i) {
    const Unit& unit = units_[static_cast<size_t>(i)];
    if (!unit.present) continue;
    // The battle end pays gold and rolls the drop for the slots in the defeated mask only - the units that died (the kill script's A6)
    // and the ones a script marked with op 8C. A forced victory (op 83 result 0) with a boxer still standing pays for the holder alone;
    // the standing one gives nothing.
    if (unit.alive() && !brain_.defeated(i)) continue;
    gold += unit.gold;
    // Script op 7C: no drop rolls.
    if (brain_.no_drops()) continue;
    // An enemy a summon killed skips the roll and gives that colour's shiny material; boss fights keep the roll.
    const auto summon_kill = std::find_if(summon_kills_.begin(), summon_kills_.end(), [&](u16 mask) { return (mask >> i) & 1; });
    if (!boss_ && summon_kill != summon_kills_.end()) {
      rewards_.items.push_back(kShinyMaterial[static_cast<size_t>(summon_kill - summon_kills_.begin())]);
      continue;
    }
    int slot = drop_roll(rng_, unit.drop_base);
    const size_t drop_index = static_cast<size_t>(unit.drop_row & 0x3FFF);
    // Cheats: always loot - the rare drop (row slot 1), else the common one (slot 0).
    if (cheats_.always_loot && drop_index < tables_.drops.size()) slot = tables_.drops[drop_index].slot[1] ? 1 : 0;
    if (slot >= 0 && drop_index < tables_.drops.size()) {
      const u16 drop = tables_.drops[drop_index].slot[static_cast<size_t>(slot)];
      if (drop & 0x8000) rewards_.elements.push_back(drop & 0x7FFF);
      else if (drop) rewards_.items.push_back(drop);
    }
  }
  bool gold_gear = opt_.gold_bonus;
  for (const Unit& pu : units_)
    if (pu.present && pu.pc && (pu.eq.gear_flags & data::gearflag::kGold)) gold_gear = true;
  rewards_.gold = gold_reward(gold, gold_gear);

  // Growth. STUB: only the party members present are updated (the 44-character pending roll is not).
  if (growth_.star >= 99 || growth_.star >= static_cast<int>(tables_.star_rows.size())) return;
  const auto& row = tables_.star_rows[static_cast<size_t>(growth_.star)];
  const StarRow sr{row[0], row[1], row[2] | (row[3] << 8)};
  const int cap = progress_cap(sr);
  if (!boss_) {
    // Cheats: instant growth = the full star's growth.
    growth_.progress = cheats_.instant_growth ? cap : std::min(growth_.progress + sr.step, cap);
    for (size_t i = 0; i < party_.size(); ++i) {
      if (!units_[i].alive()) continue;
      PartyMember& member = party_[i];
      for (size_t k = 0; k < 7; ++k) {
        int gain = minor_growth(member.snap[k], member.pending[k], member.stat[k], sr.pending_tenths, growth_.progress, cap);
        const int room = std::max(0, member.lv99[k] - member.stat[k]);
        gain = std::min(gain, room);
        member.stat[k] += gain;
        rewards_.gains[i][k] = gain;
      }
      member.stat[0] = stat_hp_cap(member.stat[0]);
    }
  } else {
    rewards_.star_up = true;
    for (size_t i = 0; i < party_.size(); ++i) {
      if (!units_[i].alive()) continue;
      PartyMember& member = party_[i];
      for (size_t k = 0; k < 7; ++k) {
        const int gain = std::min(member.pending[k], std::max(0, member.lv99[k] - member.stat[k]));
        member.stat[k] += gain;
        rewards_.gains[i][k] = gain;
      }
      member.stat[0] = stat_hp_cap(member.stat[0]);
    }
    // Characters outside the party get their old pending deltas first.
    rewards_.bench_gains.assign(bench_.size(), {});
    for (size_t i = 0; i < bench_.size(); ++i) {
      PartyMember& member = bench_[i];
      for (size_t k = 0; k < 7; ++k) {
        const int gain = std::min(member.pending[k], std::max(0, member.lv99[k] - member.stat[k]));
        member.stat[k] += gain;
        rewards_.bench_gains[i][k] = gain;
      }
      member.stat[0] = stat_hp_cap(member.stat[0]);
    }
    growth_.progress = 0;
    ++growth_.star;
    ++growth_.counter;
    // Next pending deltas for all characters, in char id order (party members that were KO'd also get them).
    std::vector<PartyMember*> all;
    for (auto& member : party_) all.push_back(&member);
    for (auto& member : bench_) all.push_back(&member);
    std::stable_sort(all.begin(), all.end(), [](const PartyMember* lhs, const PartyMember* rhs) { return lhs->char_id < rhs->char_id; });
    for (PartyMember* member_ptr : all) {
      PartyMember& member = *member_ptr;
      member.snap = member.stat;
      member.pending[0] = next_pending_hp(rng_, member.stat[0], member.lv99[0], growth_.star);
      for (size_t k = 1; k < 7; ++k) member.pending[k] = next_pending_stat(rng_, member.stat[k], member.lv99[k], growth_.star);
      member.unlocked.fill(false);
      for (int cell = 0; cell < data::kGridCells; ++cell)
        if (cell / data::kGridCols <= 1 && (member.cells[static_cast<size_t>(cell)] & 0x3FFF) != 0) member.unlocked[static_cast<size_t>(cell)] = true;
      unlock_grid(member, tables_, growth_.star);
    }
  }
}

// ------------------------------------------------------------------------------------------------ AI
EnemyAction RandomBrain::choose(const Battle& battle, int ui, Rng& rng) {
  const Unit& unit = battle.unit(ui);
  EnemyAction action;
  std::vector<int> targets;
  for (int i = 0; i < kPcSlots; ++i)
    if (battle.unit(i).alive()) targets.push_back(i);
  if (targets.empty()) {
    action.kind = EnemyAction::Kind::None;
    return action;
  }
  action.target = targets[static_cast<size_t>(rng.rnd(0, static_cast<int>(targets.size()) - 1))];

  // Damage elements the enemy owns.
  std::vector<int> elems;
  for (size_t cell = 0; cell < data::kGridCells; ++cell) {
    if (!unit.usable[cell] || (unit.cells[cell] & 0x8000)) continue;
    const int id = unit.cells[cell] & 0x3FFF;
    if (battle.tables().elements[static_cast<size_t>(id)].mode() == 0) elems.push_back(id);
  }
  if (!elems.empty() && rng.rnd(0, 99) < 40) {
    action.kind = EnemyAction::Kind::Element;
    action.element = elems[static_cast<size_t>(rng.rnd(0, static_cast<int>(elems.size()) - 1))];
    return action;
  }
  const int roll = rng.rnd(0, 99);
  action.kind = EnemyAction::Kind::Weapon;
  action.strength = roll < 60 ? 0 : roll < 90 ? 1 : 2;
  return action;
}

Command auto_command(const Battle& battle, int pc) {
  const Unit& unit = battle.unit(pc);
  Command command;
  command.actor = pc;
  const auto& elements = battle.tables().elements;

  // heal the most hurt ally below 40%
  int hurt = -1;
  for (int i = 0; i < kPcSlots; ++i) {
    const Unit& ally = battle.unit(i);
    if (ally.alive() && ally.hp * 100 < ally.max_hp * 40 && (hurt < 0 || ally.hp * battle.unit(hurt).max_hp < battle.unit(hurt).hp * ally.max_hp)) hurt = i;
  }
  int best_heal = -1, best_dmg = -1, best_pow = -1;
  for (int cell = 0; cell < data::kGridCells; ++cell) {
    if (!battle.element_usable(pc, cell) || (unit.cells[static_cast<size_t>(cell)] & 0x8000)) continue;
    const auto& element = elements[unit.cells[static_cast<size_t>(cell)] & 0x3FFF];
    if ((element.mode() == 3 || element.mode() == 43) && best_heal < 0) best_heal = cell;
    if (element.mode() == 0 && element.power() > best_pow) best_pow = element.power(), best_dmg = cell;
  }
  if (hurt >= 0 && best_heal >= 0) {
    command.kind = Command::Kind::Element;
    command.cell = best_heal;
    command.target = hurt;
    return command;
  }
  if (best_dmg >= 0) {
    command.kind = Command::Kind::Element;
    command.cell = best_dmg;
    return command;
  }
  int budget = unit.stamina;
  command.kind = Command::Kind::Attack;
  for (int i = 0; i < kMaxChain; ++i) {
    int strength = std::min(i, 2);
    while (strength >= 0 && battle.chain_cost(strength, unit) > budget) --strength;
    if (strength < 0) break;
    budget -= battle.chain_cost(strength, unit);
    command.chain.push_back(strength);
  }
  if (command.chain.empty()) command.kind = Command::Kind::Defend;
  return command;
}

}  // namespace oc::battle
