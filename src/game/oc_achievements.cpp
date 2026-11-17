#include "game/oc_achievements.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <format>

#include "platform/config.h"

namespace oc::game {

namespace {

using Cat = OcCategory;
using Rule = OcRule;
using Src = OcSource;

constexpr OcRow kRows[] = {
#include "game/oc_achievement_table.inc"
};

constexpr int kSampleTicks = 60;
constexpr int kBigHit = 9999;
constexpr int kTopLevel = 8;
constexpr int kChronoCross = 126;
constexpr int kColours = 6;
constexpr int kMaxMembers = 64;

constexpr const char* kUnlockedSection = "Unlocked";
constexpr const char* kCountersSection = "Counters";
constexpr const char* kSeenSection = "Seen";

const char* counter_name(Src counter) {
  switch (counter) {
    case Src::BattlesWon: return "battles_won";
    case Src::Steals: return "steals";
    case Src::Forges: return "forges";
    default: return "";
  }
}

bool parse_count(std::string_view text, std::uint64_t& value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return !text.empty() && error == std::errc() && end == text.data() + text.size();
}

// YYYY-MM-DDTHH:MMZ.
bool valid_time(std::string_view text) {
  constexpr std::string_view kShape = "0000-00-00T00:00Z";
  if (text.size() != kShape.size()) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool digit = text[i] >= '0' && text[i] <= '9';
    if (kShape[i] == '0' ? !digit : text[i] != kShape[i]) return false;
  }
  return true;
}

int index_in(const OcIds& ids, int value) {
  for (int i = 0; i < ids.count; ++i)
    if (ids.values[static_cast<size_t>(i)] == value) return i;
  return -1;
}

std::uint64_t full_mask(int members) { return members >= kMaxMembers ? ~0ull : (1ull << members) - 1; }

// The rows a Set names: listed ids, and for an entry ending in `*` every other row with that prefix.
std::vector<size_t> set_members(std::span<const OcRow> rows, size_t set_row) {
  std::vector<size_t> out;
  std::string_view rest = rows[set_row].members;
  while (!rest.empty()) {
    const size_t space = rest.find(' ');
    const std::string_view token = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
    if (token.empty()) continue;
    const bool prefix = token.back() == '*';
    const std::string_view name = prefix ? token.substr(0, token.size() - 1) : token;
    for (size_t i = 0; i < rows.size(); ++i) {
      const std::string_view id = rows[i].id;
      if (i != set_row && (prefix ? id.starts_with(name) : id == name)) out.push_back(i);
    }
  }
  return out;
}

}  // namespace

std::span<const OcRow> OcAchievements::table() { return kRows; }

std::optional<OcAchievements::Profile> OcAchievements::parse_profile(std::string_view text) {
  platform::Ini ini;
  ini.parse(text);
  Profile profile;
  for (const std::string& section : ini.section_names()) {
    if (section.empty()) continue;
    const std::vector<std::pair<std::string, std::string>> entries = ini.entries(section);
    if (section == kUnlockedSection) {
      for (const auto& [id, time] : entries) {
        if (!valid_time(time)) return std::nullopt;
        profile.unlocked[id] = time;
      }
    } else if (section == kCountersSection || section == kSeenSection) {
      std::map<std::string, std::uint64_t>& values = section == kCountersSection ? profile.counters : profile.seen;
      for (const auto& [name, text_value] : entries) {
        std::uint64_t value = 0;
        if (!parse_count(text_value, value)) return std::nullopt;
        values[name] = value;
      }
    } else {
      profile.other.emplace_back(section, entries);
    }
  }
  return profile;
}

std::string OcAchievements::serialize_profile(const Profile& profile) {
  platform::Ini ini;
  for (const auto& [id, time] : profile.unlocked) ini.set(kUnlockedSection, id, time);
  for (const auto& [name, value] : profile.counters) ini.set(kCountersSection, name, std::to_string(value));
  for (const auto& [id, mask] : profile.seen) ini.set(kSeenSection, id, std::to_string(mask));
  for (const auto& [section, entries] : profile.other)
    for (const auto& [key, value] : entries) ini.set(section, key, value);
  return ini.serialize();
}

std::string OcAchievements::utc_now() {
  return std::format("{:%Y-%m-%dT%H:%MZ}", std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now()));
}

OcAchievements::OcAchievements(std::span<const OcRow> rows) : rows_(rows), clock_(&OcAchievements::utc_now) {}

void OcAchievements::load(Profile profile) {
  profile_ = std::move(profile);
  dirty_ = false;
  recheck_ = true;
}

bool OcAchievements::take_dirty() { return std::exchange(dirty_, false); }

std::vector<size_t> OcAchievements::take_unlocked() { return std::exchange(new_unlocks_, {}); }

void OcAchievements::set_elements(std::vector<ElementInfo> elements) {
  elements_ = std::move(elements);
  techs_.clear();
  for (size_t id = 1; id < elements_.size(); ++id)
    if (elements_[id].casters >= 2) techs_.push_back(static_cast<int>(id));
  recheck_ = true;
}

int OcAchievements::tick(int ticks, bool in_battle) {
  if (!active()) return 0;
  if (recheck_) recheck();
  if (in_battle) return 0;
  sample_ticks_ += ticks;
  const int due = sample_ticks_ / kSampleTicks;
  sample_ticks_ %= kSampleTicks;
  return due;
}

void OcAchievements::sample(const Sample& now) {
  if (!active()) return;
  const int ending = endings_.sample(now.room, now.prev_room);
  const int recruits = static_cast<int>(std::count(now.recruited.begin(), now.recruited.end(), true));
  const auto var = [&](int id) { return now.var ? now.var(id) : 0; };
  for (size_t i = 0; i < rows_.size(); ++i) {
    const OcRow& row = rows_[i];
    if (unlocked(i)) continue;
    bool earned = false;
    switch (row.rule) {
      case Rule::Room: earned = row.ids.has(now.room); break;
      case Rule::RoomPair: earned = row.prev_ids.has(now.prev_room) && row.ids.has(now.room); break;
      case Rule::Recruit: {
        const int character = row.ids.values[0];
        earned = character >= 0 && character < kCharacters && now.recruited[static_cast<size_t>(character)];
        break;
      }
      case Rule::Recruits: earned = recruits >= row.value; break;
      case Rule::Body:
        earned = std::any_of(now.party_bodies.begin(), now.party_bodies.end(), [&](int body) { return row.ids.has(body); });
        break;
      case Rule::Save:
        earned = row.source == Src::Gold ? now.gold >= static_cast<std::uint32_t>(row.value) : var(row.var) == row.value;
        break;
      case Rule::VarBits:
        earned = std::any_of(row.ids.begin(), row.ids.end(), [&](int bit) { return (var(row.var) >> bit & 1) != 0; });
        break;
      case Rule::Profile:
        if (row.source == Src::VarBits)
          for (int bit : row.ids)
            if ((var(row.var) >> bit & 1) != 0) mark(i, bit);
        break;
      case Rule::Ending: earned = ending >= 0 && var(row.var) == row.value; break;
      default: break;
    }
    if (earned) unlock(i);
  }
  see(Src::Room, now.room);
  for (int body : now.party_bodies) see(Src::Body, body);
  if (ending >= 0) see(Src::Ending, ending);
}

void OcAchievements::battle_started() { ring_pending_ = false; }

void OcAchievements::battle_actions(std::span<const std::vector<battle::ActionEvent>> actions) {
  if (!active()) return;
  using Kind = battle::ActionEvent::Kind;
  for (const std::vector<battle::ActionEvent>& action : actions) {
    if (action.empty()) continue;
    const battle::ActionEvent& first = action.front();
    if (ring_pending_) {
      check_ring(first.ring);
      ring_pending_ = false;
    }
    if (first.kind == Kind::Element && battle::is_pc_slot(first.actor) && !first.caught) {
      element_cast(first.element);
      ring_pending_ = true;
    }
    // The party's trap held the element: a forced fizzle that springs the trap with another element (or none) is no catch.
    if (first.kind == Kind::Element && first.actor >= battle::kPcSlots &&
        std::any_of(action.begin(), action.end(), [](const battle::ActionEvent& hit) { return hit.caught && hit.trap.pc == hit.element; }))
      happen(Src::TrapCatch);
    for (const battle::ActionEvent& hit : action) {
      if (!battle::is_pc_slot(hit.actor)) continue;
      const bool on_enemy = hit.target >= battle::kPcSlots && hit.target < battle::kUnits;
      if ((hit.kind == Kind::Weapon || hit.kind == Kind::Element) && on_enemy && hit.code == 0 && hit.value >= kBigHit) happen(Src::BigHit);
      if (hit.steal_msg == 0) add_counter(Src::Steals, 1);
    }
  }
}

void OcAchievements::battle_ended(int scene, int result, bool boss, int standing, const std::array<int, 3>& ring) {
  if (!active()) return;
  if (ring_pending_) check_ring(ring);
  ring_pending_ = false;
  if (result != 0 && result != 3) return;
  add_counter(Src::BattlesWon, 1);
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Fight && rows_[i].ids.has(scene)) unlock(i);
  see(Src::Fight, scene);
  if (boss && standing == 1) happen(Src::LastStanding);
}

void OcAchievements::item_given(int id) {
  if (!active()) return;
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Item && rows_[i].ids.has(id)) unlock(i);
}

void OcAchievements::forged(int count) {
  if (!active() || count <= 0) return;
  add_counter(Src::Forges, static_cast<std::uint64_t>(count));
}

bool OcAchievements::unlocked(size_t row) const { return profile_.unlocked.contains(rows_[row].id); }

std::string OcAchievements::unlock_time(size_t row) const {
  const auto found = profile_.unlocked.find(rows_[row].id);
  return found == profile_.unlocked.end() ? std::string() : found->second;
}

int OcAchievements::unlocked_count() const {
  int count = 0;
  for (size_t i = 0; i < rows_.size(); ++i) count += unlocked(i) ? 1 : 0;
  return count;
}

std::optional<OcAchievements::Progress> OcAchievements::progress(size_t row) const {
  const OcRow& entry = rows_[row];
  if (entry.rule == Rule::Count) {
    const auto found = profile_.counters.find(counter_name(entry.source));
    return Progress{found == profile_.counters.end() ? 0 : found->second, static_cast<std::uint64_t>(entry.value)};
  }
  if (entry.rule == Rule::Profile) {
    const int members = member_count(entry);
    const auto found = profile_.seen.find(entry.id);
    const std::uint64_t mask = found == profile_.seen.end() ? 0 : found->second & full_mask(members);
    return Progress{static_cast<std::uint64_t>(std::popcount(mask)), static_cast<std::uint64_t>(members)};
  }
  return std::nullopt;
}

void OcAchievements::unlock(size_t row) {
  if (unlocked(row)) return;
  profile_.unlocked[rows_[row].id] = clock_();
  new_unlocks_.push_back(row);
  dirty_ = true;
  check_sets();
}

void OcAchievements::check_sets() {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].rule != Rule::Set || unlocked(i)) continue;
    const std::vector<size_t> members = set_members(rows_, i);
    if (!members.empty() && std::all_of(members.begin(), members.end(), [&](size_t member) { return unlocked(member); })) unlock(i);
  }
}

void OcAchievements::check_counter(OcSource counter) {
  const auto found = profile_.counters.find(counter_name(counter));
  const std::uint64_t value = found == profile_.counters.end() ? 0 : found->second;
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Count && rows_[i].source == counter && value >= static_cast<std::uint64_t>(rows_[i].value)) unlock(i);
}

void OcAchievements::check_profiles() {
  for (size_t i = 0; i < rows_.size(); ++i) {
    const std::optional<Progress> seen = rows_[i].rule == Rule::Profile ? progress(i) : std::nullopt;
    if (seen && seen->need > 0 && seen->have == seen->need) unlock(i);
  }
}

void OcAchievements::add_counter(OcSource counter, std::uint64_t amount) {
  profile_.counters[counter_name(counter)] += amount;
  dirty_ = true;
  check_counter(counter);
}

void OcAchievements::mark(size_t row, int member) {
  const int bit = member_index(rows_[row], member);
  if (bit < 0 || bit >= kMaxMembers || unlocked(row)) return;
  std::uint64_t& mask = profile_.seen[rows_[row].id];
  const std::uint64_t before = mask;
  mask |= 1ull << bit;
  dirty_ = dirty_ || mask != before;
  const std::optional<Progress> seen = progress(row);
  if (seen && seen->need > 0 && seen->have == seen->need) unlock(row);
}

void OcAchievements::see(OcSource source, int member) {
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Profile && rows_[i].source == source) mark(i, member);
}

void OcAchievements::happen(OcSource source) {
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Event && rows_[i].source == source) unlock(i);
}

void OcAchievements::element_cast(int element) {
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Cast && rows_[i].ids.has(element)) unlock(i);
  see(Src::Tech, element);
  if (element < 0 || static_cast<size_t>(element) >= elements_.size()) return;
  const ElementInfo& info = elements_[static_cast<size_t>(element)];
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].rule == Rule::Profile && rows_[i].source == Src::SummonColour && rows_[i].ids.has(element)) mark(i, info.colour);
  if (info.level == kTopLevel && element != kChronoCross) happen(Src::LevelEight);
}

void OcAchievements::check_ring(const std::array<int, 3>& ring) {
  if (ring[0] >= 0 && ring[0] < kColours && ring[1] == ring[0] && ring[2] == ring[0]) happen(Src::OneColour);
}

void OcAchievements::recheck() {
  recheck_ = false;
  for (Src counter : {Src::BattlesWon, Src::Steals, Src::Forges}) check_counter(counter);
  check_profiles();
  check_sets();
}

int OcAchievements::member_count(const OcRow& row) const {
  switch (row.source) {
    case Src::Room:
    case Src::VarBits:
    case Src::Fight:
    case Src::Body: return row.ids.count;
    case Src::SummonColour: return kColours;
    case Src::Tech: return static_cast<int>(techs_.size());
    case Src::Ending: return EndingTracker::kEndings;
    default: return 0;
  }
}

int OcAchievements::member_index(const OcRow& row, int member) const {
  switch (row.source) {
    case Src::Room:
    case Src::VarBits:
    case Src::Fight:
    case Src::Body: return index_in(row.ids, member);
    case Src::SummonColour: return member >= 0 && member < kColours ? member : -1;
    case Src::Tech: {
      const auto found = std::find(techs_.begin(), techs_.end(), member);
      return found == techs_.end() ? -1 : static_cast<int>(found - techs_.begin());
    }
    case Src::Ending: return member >= 0 && member < EndingTracker::kEndings ? member : -1;
    default: return -1;
  }
}

}  // namespace oc::game
