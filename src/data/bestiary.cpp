#include "data/bestiary.h"

#include <algorithm>
#include <map>
#include <tuple>

#include "data/battlefields.h"

namespace oc::data {

namespace {

const char* const kBendOfTime[6] = {"Bend of Time", "Courbure du Temps", "Curva temporale", "Zeitkrümmung", "Abismo del Tiempo", "次元のはざま"};

constexpr PlaceName marker(const char* world, int msg, int sibling = -1) { return PlaceName{world, msg, sibling, 0, nullptr}; }
constexpr PlaceName chapter(int row) { return PlaceName{nullptr, -1, -1, row, nullptr}; }

// The areas in story order. The story values come from the story-counter sets of each area's own scripts where its first visit sets one,
// else from the chapter the save list names at that value, the story or flag tests that guard its entrances and battles, and the route.
const StoryArea kAreas[] = {
    {"opn_a", 0, chapter(1)},
    {"alu_a", 8, marker("wld_a01", 4)},
    {"opa_a", 8, marker("wld_a01", 10, 11)},
    {"mis_a", 8, marker("wld_a01", 5)},
    {"opa_b", 24, marker("wld_b01", 10, 11)},
    {"mis_b", 26, marker("wld_b01", 5)},
    {"obo_b", 28, marker("wld_b01", 12, 13)},
    {"man_b", 36, marker("wld_b01", 6)},
    {"chi_b", 36, marker("wld_b00", 12)},
    {"jac_b", 36, marker("wld_b01", 7)},
    {"gal_b", 52, marker("wld_a00", 10)},
    {"hid_b", 52, marker("wld_b01", 16)},
    {"hid_b05f", 52, marker("wld_b00", 1)},
    {"dem", 52, marker("wld_b01", 16)},
    {"kur_b", 52, marker("wld_b00", 14)},
    {"inj_b", 64, marker("wld_b00", 21)},
    {"you_b", 64, marker("wld_b00", 16)},
    {"sui_b", 80, marker("wld_b00", 24)},
    {"sui_a", 80, marker("wld_a00", 21)},
    {"des_b", 88, marker("wld_b01", 14, 15)},
    {"des_a", 88, marker("wld_a01", 14, 15)},
    {"ryu_b", 92, marker("wld_b01", 17)},
    {"cao_a", 100, chapter(15)},
    {"gal_b16e", 100, marker("wld_a00", 10)},
    {"hid_a", 104, marker("wld_a01", 16)},
    {"hid_a05f", 104, marker("wld_a00", 1)},
    {"kur_a", 108, marker("wld_a00", 14)},
    {"obo_a", 116, marker("wld_a01", 12, 13)},
    {"man_a", 116, marker("wld_a01", 6)},
    {"chi_a", 116, marker("wld_a00", 12)},
    {"jac_a", 124, marker("wld_a01", 7)},
    {"mab_a", 124, marker("wld_a00", 17)},
    {"zel_a", 124, marker("wld_a00", 11)},
    {"mja_a", 128, marker("wld_a00", 19)},
    {"wld_a", 140, chapter(21)},
    {"bak_a", 140, chapter(21)},
    {"mja_b", 144, marker("wld_b00", 22)},
    {"dri_a", 144, PlaceName{}},
    {"jac_b29i", 156, marker("wld_b01", 7)},
    {"jac_b30d", 156, marker("wld_b01", 7)},
    {"jac_b31d", 156, marker("wld_b01", 7)},
    {"jac_b32d", 156, marker("wld_b01", 7)},
    {"jac_b38i", 156, marker("wld_b01", 7)},
    {"jac_b39i", 156, marker("wld_b01", 7)},
    {"jac_b46i", 156, marker("wld_b01", 7)},
    {"dry_a", 176, marker("wld_a00", 20)},
    {"dry_b", 176, marker("wld_b00", 23)},
    {"hes_a", 176, marker("wld_a00", 0)},
    {"hes_b", 176, marker("wld_b00", 0)},
    {"ten_a", 176, marker("wld_a00", 22)},
    {"ten_b", 176, marker("wld_b00", 25)},
    {"mab_b", 176, marker("wld_b00", 20)},
    {"fat_b", 184, marker("wld_b04", 4)},
    {"fat_b02f", 184, marker("wld_b04", 5)},
    {"fat_b03f", 184, marker("wld_b04", 6)},
    {"crn_b", 188, marker("wld_b04", 7)},
    {"sta_a", 195, marker("wld_b00", 19)},
    {"ryu_a", 195, marker("wld_a01", 17)},
    {"kji_b", 195, PlaceName{}},
    {"opa_a04f", 209, marker("wld_a01", 9)},
    {"opa_b04f", 209, marker("wld_b01", 9)},
    {"ten_b07f", 209, marker("wld_b00", 25)},
    {"tok_a", 210, PlaceName{nullptr, -1, -1, 0, kBendOfTime}},
    {"fin", 999, PlaceName{}},
};

// UTF-8 length of the character that starts with `lead`.
size_t char_length(unsigned char lead) { return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4; }

// Line breaks and blanks (U+3000 included) as one blank, trimmed at both ends.
std::string squeeze(std::string_view text) {
  std::string out;
  bool blank = false;
  for (size_t i = 0; i < text.size();) {
    const size_t length = std::min(char_length(static_cast<unsigned char>(text[i])), text.size() - i);
    const std::string_view ch = text.substr(i, length);
    i += length;
    if (ch == " " || ch == "\n" || ch == "\r" || ch == "\t" || ch == "　" || ch == " ") {
      blank = !out.empty();
      continue;
    }
    if (blank) out += ' ';
    blank = false;
    out += ch;
  }
  return out;
}

}  // namespace

std::span<const StoryArea> story_areas() { return kAreas; }

std::string species_key(std::string_view name) {
  std::string key(name);
  for (char& ch : key)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return key;
}

std::string area_key(std::string_view stem) {
  const size_t bar = stem.find('_');
  if (bar == std::string_view::npos) return std::string(stem);
  const std::string_view rest = stem.substr(bar + 1);
  if (rest.size() >= 2 && (rest[0] == 'a' || rest[0] == 'b') && rest[1] >= '0' && rest[1] <= '9') return std::string(stem.substr(0, bar + 2));
  return std::string(stem.substr(0, bar));
}

int story_area_of(std::string_view stem) {
  const std::string area = area_key(stem);
  int found = -1;
  for (size_t i = 0; i < std::size(kAreas); ++i) {
    if (stem == kAreas[i].key) return static_cast<int>(i);
    if (found < 0 && area == kAreas[i].key) found = static_cast<int>(i);
  }
  return found;
}

World world_of(std::string_view stem) {
  const size_t bar = stem.find('_');
  if (bar == std::string_view::npos || bar + 1 >= stem.size()) return World::None;
  const std::string_view rest = stem.substr(bar + 1);
  if (rest.size() >= 2 && (rest[1] < '0' || rest[1] > '9')) return World::None;
  return rest[0] == 'a' ? World::Home : rest[0] == 'b' ? World::Another : World::None;
}

std::set<std::string> places_in_both_worlds(const std::vector<std::string>& stems) {
  std::map<std::string, std::set<World>> worlds;
  for (const std::string& stem : stems)
    if (const char* name = area_name(stem); name && world_of(stem) != World::None) worlds[name].insert(world_of(stem));
  std::set<std::string> out;
  for (const auto& [name, seen] : worlds)
    if (seen.size() == 2) out.insert(name);
  return out;
}

std::vector<World> area_worlds(const std::vector<std::string>& stems) {
  const std::set<std::string> both = places_in_both_worlds(stems);
  std::vector<World> out(std::size(kAreas), World::None);
  for (const std::string& stem : stems) {
    const int area = story_area_of(stem);
    const char* name = area_name(stem);
    if (area >= 0 && name && both.contains(name)) out[static_cast<size_t>(area)] = world_of(kAreas[static_cast<size_t>(area)].key);
  }
  return out;
}

BestiaryOrder bestiary_order(const std::vector<BattleSite>& sites, const std::vector<std::string>& names, const std::vector<bool>& placed,
                             const std::vector<std::vector<int>>& scenes) {
  BestiaryOrder out;
  out.places.resize(names.size());
  // A site's place in the story: (story value, table row, scan index); rooms of no listed area come after every listed one.
  using Rank = std::tuple<int, int, size_t>;
  constexpr int kUnlisted = 0x7FFFFFFF;
  std::vector<Rank> rank_of(names.size(), Rank{kUnlisted, kUnlisted, 0});
  std::vector<bool> fielded(names.size(), false);
  std::vector<std::vector<std::pair<Rank, int>>> areas_of(names.size());
  for (size_t index = 0; index < sites.size(); ++index) {
    const BattleSite& site = sites[index];
    const int area = story_area_of(site.stem);
    const Rank rank{area < 0 ? kUnlisted : std::max(kAreas[static_cast<size_t>(area)].story, site.story_lo), area < 0 ? kUnlisted : area, index};
    for (int record : site.enemies) {
      if (record < 0 || static_cast<size_t>(record) >= names.size()) continue;
      const size_t at = static_cast<size_t>(record);
      if (!fielded[at] || rank < rank_of[at]) rank_of[at] = rank;
      fielded[at] = true;
      if (area >= 0 && kAreas[static_cast<size_t>(area)].name.named()) areas_of[at].push_back({rank, area});
    }
  }
  for (size_t record = 0; record < names.size(); ++record) {
    std::sort(areas_of[record].begin(), areas_of[record].end());
    for (const auto& [rank, area] : areas_of[record])
      if (std::find(out.places[record].begin(), out.places[record].end(), area) == out.places[record].end()) out.places[record].push_back(area);
  }
  // An unnamed record's host: the first named record of the first scene that fields both.
  std::vector<int> host(names.size(), -1);
  for (const std::vector<int>& scene : scenes)
    for (int record : scene) {
      if (record < 0 || static_cast<size_t>(record) >= names.size() || !names[static_cast<size_t>(record)].empty() || host[static_cast<size_t>(record)] >= 0)
        continue;
      for (int other : scene)
        if (other >= 0 && static_cast<size_t>(other) < names.size() && !names[static_cast<size_t>(other)].empty()) {
          host[static_cast<size_t>(record)] = other;
          break;
        }
    }
  // The species by name, each with the best rank of its records; the unfielded ones by their first record after them. The hosted
  // records join in a second pass, after their species' named records.
  std::map<std::string, size_t> by_name;
  std::vector<std::vector<int>> species;
  std::vector<std::pair<bool, Rank>> species_rank;
  for (const bool hosted : {false, true})
    for (size_t record = 0; record < names.size(); ++record) {
      if (record >= placed.size() || !placed[record] || (host[record] >= 0) != hosted) continue;
      const std::string& name = hosted ? names[static_cast<size_t>(host[record])] : names[record];
      const std::string key = name.empty() ? "#" + std::to_string(record) : species_key(name);
      const auto [it, fresh] = by_name.try_emplace(key, species.size());
      if (fresh) {
        species.emplace_back();
        species_rank.push_back({false, Rank{kUnlisted, kUnlisted, record}});
      }
      species[it->second].push_back(static_cast<int>(record));
      auto& [any, best] = species_rank[it->second];
      if (fielded[record] && (!any || rank_of[record] < best)) best = rank_of[record], any = true;
    }
  std::vector<size_t> order(species.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (species_rank[a].first != species_rank[b].first) return species_rank[a].first;
    return species_rank[a].first ? species_rank[a].second < species_rank[b].second : a < b;
  });
  for (size_t index : order) out.species.push_back(std::move(species[index]));
  return out;
}

std::string place_label(std::string_view text, const std::string* sibling, bool chapter) {
  std::string_view source = text;
  if (chapter) {
    size_t cut = source.size();
    for (const std::string_view mark : {std::string_view(" -"), std::string_view(":"), std::string_view("："), std::string_view("　")})
      cut = std::min(cut, source.find(mark));
    source = source.substr(0, cut);
  }
  std::string label = squeeze(source);
  if (sibling) {
    const std::string other = squeeze(*sibling);
    size_t same = 0;
    while (same < label.size() && same < other.size() && label[same] == other[same]) ++same;
    // Back to the start of a character.
    while (same > 0 && same < label.size() && (static_cast<unsigned char>(label[same]) & 0xC0) == 0x80) --same;
    label.resize(same);
  }
  while (label.starts_with("-") || label.starts_with(" ")) label.erase(0, 1);
  for (bool trimmed = true; trimmed && !label.empty();) {
    trimmed = false;
    for (const std::string_view tail : {std::string_view(" "), std::string_view("-"), std::string_view(":"), std::string_view("・")})
      if (label.ends_with(tail)) label.resize(label.size() - tail.size()), trimmed = true;
  }
  return label;
}

std::string place_name(import::LangTables& texts, int area, import::Lang lang) {
  if (area < 0 || static_cast<size_t>(area) >= std::size(kAreas)) return {};
  const PlaceName& source = kAreas[static_cast<size_t>(area)].name;
  auto marker_text = [&](int msg) {
    std::string out;
    const import::Message flat = import::flatten(texts.field_message(import::room_id_from_name(source.world), msg, lang), texts.make_context(lang));
    for (const import::Token& token : flat.tokens) out += token.kind == import::TokKind::Text ? import::u32_to_utf8(token.text) : std::string(" ");
    return out;
  };
  if (source.world) {
    const std::string sibling = source.sibling >= 0 ? marker_text(source.sibling) : std::string();
    return place_label(marker_text(source.msg), source.sibling >= 0 ? &sibling : nullptr, false);
  }
  if (source.chapter > 0) {
    const import::TextTable* titles = texts.table("savetbl");
    const std::string* title = titles ? titles->get(static_cast<u64>(source.chapter), lang) : nullptr;
    return title ? place_label(title->substr(0, title->find('\n')), nullptr, true) : std::string();
  }
  return source.own ? std::string(source.own[std::clamp(static_cast<int>(lang), 0, 5)]) : std::string();
}

}  // namespace oc::data
