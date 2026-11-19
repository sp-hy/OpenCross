// The randomizer's shuffles and the plan's answers (randomizer.h). Everything here is integer arithmetic on a counter-based generator
// (splitmix64 of seed, stream and position) with every sort fully ordered, so a seed gives the same plan on every machine and build.
#include "game/randomizer.h"

#include <algorithm>
#include <format>
#include <numeric>
#include <tuple>

#include "core/features.h"
#include "save/cc_save.h"

namespace oc::game::randomizer {

namespace {

constexpr int kPercentFloor = 25;
constexpr int kPercentCeiling = 400;
constexpr u32 kStartStream = 100;
constexpr int kRankScale = 1000000;

u64 mix(u64 value) {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

// A random stream: the n-th draw is mix(seed, stream, n), so a stream does not depend on how much another one drew.
class Stream {
 public:
  Stream(u32 seed, u32 stream) : base_(mix((static_cast<u64>(seed) << 32) | stream)) {}
  u64 next() { return mix(base_ + counter_++); }
  u64 below(u64 bound) { return bound ? next() % bound : 0; }

 private:
  u64 base_;
  u64 counter_ = 0;
};

// The window a thing may move in, for a pool or a choice list of `count`.
size_t window(size_t count, int spread) { return std::max<size_t>(2, count * static_cast<size_t>(400 + 96 * std::clamp(spread, 0, 100)) / 10000); }

int ratio_percent(int to, int from) {
  if (to <= 0 || from <= 0) return 100;
  return std::clamp(static_cast<int>(static_cast<s64>(to) * 100 / from), kPercentFloor, kPercentCeiling);
}

bool pool_on(const Settings& settings, Pool pool) {
  switch (pool) {
    case Pool::ChestItems:
    case Pool::DropItems:
    case Pool::SmithStock: return settings.items;
    case Pool::ChestElements:
    case Pool::DropElements:
    case Pool::ElementStock: return settings.elements;
    case Pool::Encounters: return settings.enemies;
  }
  return false;
}

// A shop row must not list one thing twice: a repeat swaps with the nearest later entry of another row that takes it without a repeat.
void spread_repeats(std::span<const Entry> entries, std::vector<int>& things) {
  const auto row_of = [&](size_t i) { return entries[i].site / 64; };
  const auto in_row = [&](u32 row, int thing, size_t except) {
    for (size_t k = 0; k < entries.size(); ++k)
      if (k != except && row_of(k) == row && things[k] == thing) return true;
    return false;
  };
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!in_row(row_of(i), things[i], i)) continue;
    for (size_t step = 1; step < entries.size(); ++step) {
      const size_t j = (i + step) % entries.size();
      if (row_of(j) == row_of(i) || in_row(row_of(i), things[j], i) || in_row(row_of(j), things[i], j)) continue;
      std::swap(things[i], things[j]);
      break;
    }
  }
}

std::string percent_note(const battle::EnemyTuning& tuning) {
  return std::format("HP {} %, ATK DEF MAG MDEF {} %, gold {} %", tuning.hp_percent, tuning.stat_percent, tuning.gold_percent);
}

}  // namespace

Settings settings_from(const features::Resolved& features) {
  using features::kRandomizer;
  Settings out;
  out.seed = static_cast<u32>(features.value(kRandomizer, "Seed"));
  out.spread = features.value(kRandomizer, "Spread");
  out.items = features.value(kRandomizer, "Items") != 0;
  out.elements = features.value(kRandomizer, "Elements") != 0;
  out.enemies = features.value(kRandomizer, "Enemies") != 0;
  out.start_gear = features.value(kRandomizer, "StartGear") != 0;
  out.start_elements = features.value(kRandomizer, "StartElements") != 0;
  return out;
}

const char* pool_name(Pool pool) {
  switch (pool) {
    case Pool::ChestItems: return "Chests and pickups: items";
    case Pool::ChestElements: return "Chests and pickups: elements";
    case Pool::DropItems: return "Kill drops and steals: items";
    case Pool::DropElements: return "Kill drops and steals: elements";
    case Pool::SmithStock: return "Smiths";
    case Pool::ElementStock: return "Element shops";
    case Pool::Encounters: return "Encounter groups";
  }
  return "";
}

std::string Pools::thing_name(int thing) const {
  const size_t index = static_cast<size_t>(thing);
  if (thing >= 0 && index < names.size() && !names[index].empty()) return names[index];
  return thing >= 0x200 ? std::format("element {}", thing - 0x200) : std::format("item {}", thing);
}

std::vector<size_t> shuffle(std::span<const Entry> entries, u32 seed, u32 stream, int spread) {
  const size_t count = entries.size();
  std::vector<size_t> out(count);
  std::iota(out.begin(), out.end(), size_t{0});
  if (count < 2) return out;
  const auto ids = [&](size_t i) { return std::tie(entries[i].site, entries[i].slot); };
  // The value percentile among the entries with a known value, the stage percentile among those with a known stage; the rank is their
  // mean, or the one that is known. An entry with neither has no rank: it keeps its thing unless the shuffle is pure.
  const auto percentile = [&](auto known, auto key) {
    std::vector<s64> out_rank(count, -1);
    std::vector<size_t> list;
    for (size_t i = 0; i < count; ++i)
      if (known(i)) list.push_back(i);
    std::sort(list.begin(), list.end(), [&](size_t lhs, size_t rhs) {
      return std::make_tuple(key(lhs), entries[lhs].thing, ids(lhs)) < std::make_tuple(key(rhs), entries[rhs].thing, ids(rhs));
    });
    for (size_t p = 0; p < list.size(); ++p)
      out_rank[list[p]] = list.size() > 1 ? static_cast<s64>(p) * kRankScale / static_cast<s64>(list.size() - 1) : kRankScale / 2;
    return out_rank;
  };
  const std::vector<s64> by_value = percentile([&](size_t i) { return entries[i].value >= 0; }, [&](size_t i) { return entries[i].value; });
  const std::vector<s64> by_stage = percentile([&](size_t i) { return entries[i].stage >= 0; }, [&](size_t i) { return entries[i].stage; });
  std::vector<s64> rank(count, 0);
  std::vector<size_t> sorted;
  for (size_t i = 0; i < count; ++i) {
    if (by_value[i] < 0 && by_stage[i] < 0 && spread < 100) continue;
    rank[i] = by_value[i] >= 0 && by_stage[i] >= 0 ? (by_value[i] + by_stage[i]) / 2 : std::max(by_value[i], by_stage[i]);
    sorted.push_back(i);
  }
  std::sort(sorted.begin(), sorted.end(),
            [&](size_t lhs, size_t rhs) { return std::make_tuple(rank[lhs], ids(lhs)) < std::make_tuple(rank[rhs], ids(rhs)); });
  if (sorted.size() < 2) return out;
  // pick[p]: the sorted position whose thing goes to sorted position p.
  Stream rng(seed, stream);
  const size_t ranked = sorted.size();
  std::vector<size_t> pick(ranked);
  std::iota(pick.begin(), pick.end(), size_t{0});
  if (spread >= 100) {
    for (size_t i = ranked - 1; i > 0; --i) std::swap(pick[i], pick[static_cast<size_t>(rng.below(i + 1))]);
  } else {
    constexpr u64 kFine = 1 << 16;
    const u64 width = window(ranked, spread) * kFine;
    std::vector<u64> key(ranked);
    for (size_t p = 0; p < ranked; ++p) key[p] = p * kFine + rng.below(width);
    std::sort(pick.begin(), pick.end(), [&](size_t lhs, size_t rhs) { return std::tie(key[lhs], lhs) < std::tie(key[rhs], rhs); });
  }
  for (size_t p = 0; p < ranked; ++p) out[sorted[p]] = sorted[pick[p]];
  return out;
}

int Plan::treasure(u32 key, int original) const {
  const auto found = treasures_.find({key, original});
  return found == treasures_.end() ? original : found->second;
}

int Plan::patch_room(std::string_view stem, Bytes& code) const {
  const auto found = literals_.find(stem);
  if (found == literals_.end()) return 0;
  int changed = 0;
  for (const Literal& literal : found->second) {
    const int thing = treasure(literal.treasure, literal.thing);
    if (thing == literal.thing || literal.pos + 1 >= code.size()) continue;
    const int raw = code[literal.pos] | (code[literal.pos + 1] << 8);
    // A U operand keeps its literal flag 0x8000; an S / T operand's mode byte marks it, the word is the plain value.
    const int flag = raw == (literal.thing | 0x8000) ? 0x8000 : raw == literal.thing ? 0 : -1;
    if (flag < 0) continue;
    code[literal.pos] = static_cast<u8>((thing | flag) & 0xFF);
    code[literal.pos + 1] = static_cast<u8>((thing | flag) >> 8);
    ++changed;
  }
  return changed;
}

std::vector<int> Plan::stock(bool elements, int shop_id, std::vector<int> row) const {
  const std::vector<std::vector<int>>& rows = elements ? el_rows_ : smith_rows_;
  if (shop_id < 0 || static_cast<size_t>(shop_id) >= rows.size()) return row;
  return rows[static_cast<size_t>(shop_id)];
}

int Plan::scene(int original) const {
  const auto found = scenes_.find(original);
  return found == scenes_.end() ? original : found->second;
}

std::array<battle::EnemyTuning, 3> Plan::tuning(int original_scene) const {
  const auto found = tuning_.find(original_scene);
  return found == tuning_.end() ? std::array<battle::EnemyTuning, 3>{} : found->second;
}

void Plan::start_game(save::SaveSlot& slot) const {
  for (const auto& [piece, thing] : start_gear_) {
    const auto [character, gear_slot] = piece;
    if (character < 0 || character >= save::kNumChars) continue;
    save::SaveChar& member = slot.chars[static_cast<size_t>(character)];
    u16* worn = gear_slot == 0 ? &member.weapon : gear_slot == 1 ? &member.armour : &member.accessory[static_cast<size_t>(gear_slot - 2)];
    *worn = static_cast<u16>(thing);
  }
  for (const auto& [piece, thing] : start_elements_) {
    const auto [character, original] = piece;
    if (character < 0 || character >= save::kNumChars) continue;
    for (u16& cell : slot.chars[static_cast<size_t>(character)].grid)
      if ((cell & 0x1FF) == original - 0x200) cell = static_cast<u16>((cell & ~0x1FF) | (thing - 0x200));
  }
}

Plan build(const Settings& settings, const Pools& pools) {
  Plan plan;
  plan.settings_ = settings;
  std::string& log = plan.spoiler_;
  log = std::format("OpenCross randomizer spoiler log\nSeed {}, spread {}. Items {}, elements {}, enemies {}, starting gear {}, starting elements {}.\n",
                    settings.seed, settings.spread, settings.items ? "on" : "off", settings.elements ? "on" : "off", settings.enemies ? "on" : "off",
                    settings.start_gear ? "on" : "off", settings.start_elements ? "on" : "off");
  if (!settings.enabled()) return plan;
  std::vector<data::DropRow> drops = pools.drops;
  bool drops_moved = false;
  const auto scene_label = [&](int scene) {
    std::string out;
    if (scene >= 0 && static_cast<size_t>(scene) < pools.scene_enemies.size())
      for (int enemy : pools.scene_enemies[static_cast<size_t>(scene)]) {
        if (enemy < 0) continue;
        const size_t index = static_cast<size_t>(enemy);
        out += (out.empty() ? "" : ", ") + (index < pools.enemy_names.size() && !pools.enemy_names[index].empty() ? pools.enemy_names[index] : std::format("enemy {}", enemy));
      }
    return std::format("scene {} ({})", scene, out);
  };
  for (size_t index = 0; index < kPools; ++index) {
    const Pool pool = static_cast<Pool>(index);
    const std::vector<Entry>& entries = pools.entries[index];
    plan.counts_[index] = {static_cast<int>(entries.size()), 0};
    if (!pool_on(settings, pool) || entries.empty()) continue;
    const std::vector<size_t> perm = shuffle(entries, settings.seed, static_cast<u32>(index), settings.spread);
    std::vector<int> things(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) things[i] = entries[perm[i]].thing;
    if (pool == Pool::SmithStock || pool == Pool::ElementStock) spread_repeats(entries, things);
    log += std::format("\n== {} ==\n", pool_name(pool));
    int moved = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
      const Entry& entry = entries[i];
      const int thing = things[i];
      moved += thing != entry.thing ? 1 : 0;
      switch (pool) {
        case Pool::ChestItems:
        case Pool::ChestElements: plan.treasures_[{entry.site, entry.thing}] = thing; break;
        case Pool::DropItems:
        case Pool::DropElements:
          drops[entry.site / 4].slot[entry.site % 4] = thing_drop(thing);
          drops_moved = true;
          break;
        case Pool::SmithStock:
        case Pool::ElementStock: {
          std::vector<std::vector<int>>& rows = pool == Pool::SmithStock ? plan.smith_rows_ : plan.el_rows_;
          if (rows.empty()) {
            for (const std::vector<u16>& source : pool == Pool::SmithStock ? pools.smith_shop : pools.el_shop) {
              std::vector<int>& row = rows.emplace_back();
              for (u16 id : source) {
                if (id == 0) break;
                row.push_back(id);
              }
            }
          }
          rows[entry.site / 64][entry.site % 64] = pool == Pool::ElementStock ? thing - 0x200 : thing;
          break;
        }
        case Pool::Encounters: plan.scenes_[static_cast<int>(entry.site)] = thing; break;
      }
      if (pool == Pool::Encounters) log += std::format("{}: {} -> {}\n", entry.where, scene_label(entry.thing), scene_label(thing));
      else log += std::format("{}: {} -> {}\n", entry.where, pools.thing_name(entry.thing), pools.thing_name(thing));
    }
    plan.counts_[index].second = moved;
  }
  if (drops_moved) plan.drops_ = std::make_shared<const std::vector<data::DropRow>>(std::move(drops));

  // A moved group takes the area's place: its stats follow the ratio of the two areas' means, its drops the rows of the group it replaced.
  if (settings.enemies) {
    std::map<int, const Encounter*> by_scene;
    for (const Encounter& encounter : pools.encounters) by_scene[encounter.scene] = &encounter;
    log += "\n== Encounter groups: scaling ==\n";
    for (const auto& [site, thing] : plan.scenes_) {
      if (site == thing) continue;
      const Encounter* home = by_scene[site];
      const Encounter* guest = by_scene[thing];
      if (!home || !guest) continue;
      const auto area_of = [&](const std::string& key) {
        const auto found = pools.areas.find(key);
        return found == pools.areas.end() ? Area{} : found->second;
      };
      const Area to = area_of(home->area), from = area_of(guest->area);
      std::array<battle::EnemyTuning, 3> tuning{};
      int last_row = -1;
      for (size_t group = 0; group < 3; ++group) {
        if (home->drop_rows[group] >= 0) last_row = home->drop_rows[group];
        tuning[group] = {ratio_percent(to.hp, from.hp), ratio_percent(to.power, from.power), ratio_percent(to.gold, from.gold),
                         home->drop_rows[group] >= 0 ? home->drop_rows[group] : last_row};
      }
      plan.tuning_[site] = tuning;
      log += std::format("{} in {}: {}\n", scene_label(thing), to.name.empty() ? home->area : to.name, percent_note(tuning[0]));
    }
  }

  // Starting gear and elements: each piece becomes a thing of the same kind (gear) or level (element) the character may use, picked from
  // the window around the piece's own value.
  if (settings.start_gear || settings.start_elements) {
    std::string gear_log, element_log;
    for (const StartPiece& piece : pools.start) {
      const bool gear = piece.slot >= 0;
      if (gear ? !settings.start_gear : !settings.start_elements) continue;
      const int kind = std::min(piece.slot, 2);
      int level = -1, value = -1;
      for (const Choice& choice : pools.choices)
        if (choice.thing == piece.thing) level = choice.level, value = choice.value;
      if (!gear && level < 0) continue;
      std::vector<const Choice*> fits;
      for (const Choice& choice : pools.choices) {
        if ((choice.thing >= 0x200) == gear || ((choice.equip >> piece.character) & 1) == 0) continue;
        if (gear ? choice.kind != kind : choice.level != level) continue;
        fits.push_back(&choice);
      }
      if (fits.empty()) continue;
      std::sort(fits.begin(), fits.end(), [](const Choice* lhs, const Choice* rhs) { return std::tie(lhs->value, lhs->thing) < std::tie(rhs->value, rhs->thing); });
      const auto at = std::find_if(fits.begin(), fits.end(), [&](const Choice* choice) { return choice->value >= value; });
      const s64 centre = at - fits.begin();
      const s64 half = static_cast<s64>(settings.spread >= 100 ? fits.size() : window(fits.size(), settings.spread));
      const s64 lo = std::max<s64>(0, centre - half / 2), hi = std::min<s64>(static_cast<s64>(fits.size()) - 1, centre + half / 2);
      Stream rng(settings.seed, kStartStream + static_cast<u32>(piece.character * 8 + (gear ? piece.slot : 7)) * 1024 + static_cast<u32>(piece.thing & 0x3FF));
      const int thing = fits[static_cast<size_t>(lo + static_cast<s64>(rng.below(static_cast<u64>(hi - lo + 1))))]->thing;
      const std::string who = static_cast<size_t>(piece.character) < pools.char_names.size() ? pools.char_names[static_cast<size_t>(piece.character)]
                                                                                              : std::format("character {}", piece.character);
      if (gear) {
        plan.start_gear_[{piece.character, piece.slot}] = thing;
        static constexpr const char* kSlots[] = {"weapon", "armour", "accessory", "accessory", "accessory"};
        gear_log += std::format("{} {}: {} -> {}\n", who, kSlots[std::clamp(piece.slot, 0, 4)], pools.thing_name(piece.thing), pools.thing_name(thing));
      } else {
        plan.start_elements_[{piece.character, piece.thing}] = thing;
        element_log += std::format("{}: {} -> {}\n", who, pools.thing_name(piece.thing), pools.thing_name(thing));
      }
    }
    if (settings.start_gear) log += "\n== Starting equipment ==\n" + gear_log;
    if (settings.start_elements) log += "\n== Starting elements ==\n" + element_log;
  }

  // The chest literals of the moved treasures, per room.
  for (const auto& [stem, literals] : pools.literals)
    for (const Literal& literal : literals)
      if (plan.treasure(literal.treasure, literal.thing) != literal.thing) plan.literals_[stem].push_back(literal);
  return plan;
}

}  // namespace oc::game::randomizer
