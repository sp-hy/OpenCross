#include "core/features.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

namespace oc::features {

namespace {

// Party swap: a battle command that trades the acting PC with a reserve member; the formation is restored after the battle unless
// KeepLineup. Reserve = the reserve slots set on the Party screen (FE 5B) and by SET_RESERVE; 0 = everyone recruited outside the party.
constexpr std::array<Setting, 3> kPartySwapSettings = {{
    {"Enabled", 0, 0, 1, "Party swap in battle"},
    {"KeepLineup", 0, 0, 1, "Keep the swapped party after the battle"},
    {"Reserve", 0, 0, 41, "Reserve slots (0 = everyone outside the party)"},
}};

// Party screen behaviour that differs from the game: ReturnToSlot = a pick returns the cursor to the slot it filled.
constexpr std::array<Setting, 2> kPartyScreenSettings = {{
    {"Enabled", 0, 0, 1, "Party screen tweaks"},
    {"ReturnToSlot", 1, 0, 1, "A pick returns the cursor to the slot it filled"},
}};

// Party swap and the party screen tweaks work on the game as shipped (addons). The mod UI and model source ops have no setting: a mod's
// own scripts use them (and check the level first), and name them in Requires (mod kits). So does the button panel: the engine's own
// correction of kji_b01i uses it for the story's code machine, and a mod's script can use it for a code of its own.
constexpr std::array<Feature, 6> kFeatures = {{
    {kExtensionRange, 1, "", "Extension ops (FE C0-FE FF)", {}},
    {kPartySwap, 2, "PartySwap", "Party swap", kPartySwapSettings, true,
     "A Swap command in battle: the acting character trades places with a reserve member. The party comes back after the battle unless "
     "you keep it."},
    {kModUi, 2, "ModUi", "Mod UI", {}, false,
     "Script ops a mod builds its own screens from: panels, text, images, ovals, lists and a cursor the script reads the pick from. Field "
     "script FE C1 to FE CB, FE CE and FE CF."},
    {kPartyScreen, 1, "PartyScreen", "Party screen tweaks", kPartyScreenSettings, true,
     "Party screen (Tele-Porter) behaviour that differs from the game."},
    {kModelSources, 1, "ModelSources", "Models from elsewhere", {}, false,
     "Lets an NPC wear a model from somewhere else: a playable character, another room's model or any battle model. Field script FE D0 "
     "MODEL_SOURCE."},
    {kButtonPanel, 1, "ButtonPanel", "Button panel", {}, false,
     "RDE's on-screen button picker for a code a script reads from the pad: a Circle, Cross or Triangle opens it, and the button picked "
     "on its grid is what the script's key-latch tests read. Field script FE D1 BUTTON_PANEL, just before the tests."},
}};

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

bool same(std::string_view lhs, std::string_view rhs) {
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                    [](char a, char b) { return (a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b); });
}

// An integer, or on / off words for 1 / 0.
bool parse_value(std::string_view text, int& out) {
  if (same(text, "on") || same(text, "true") || same(text, "yes")) return out = 1, true;
  if (same(text, "off") || same(text, "false") || same(text, "no")) return out = 0, true;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

int setting_index(const Feature& feature, std::string_view key) {
  for (size_t k = 0; k < feature.settings.size(); ++k)
    if (same(feature.settings[k].key, key)) return static_cast<int>(k);
  return -1;
}

}  // namespace

std::span<const Feature> table() { return kFeatures; }

const Feature* find(std::span<const Feature> features, int id) {
  for (const Feature& feature : features)
    if (feature.id == id) return &feature;
  return nullptr;
}

const Feature* find(std::span<const Feature> features, std::string_view section) {
  for (const Feature& feature : features)
    if (*feature.section && same(feature.section, section)) return &feature;
  return nullptr;
}

int level(int id) {
  const Feature* feature = find(table(), id);
  return feature ? feature->level : 0;
}

// Comments are whole lines (; or #), as in opencross.ini. Keys before the first section, lines without '=' and a [Mod] key that is not
// one of the four are problems; repeated sections are merged.
Manifest parse_manifest(std::string_view text) {
  Manifest out;
  Manifest::Section* current = nullptr;
  bool in_mod = false, in_rooms = false, any_section = false;
  int line_no = 0;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    const std::string_view line = trim(text.substr(0, nl));
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    ++line_no;
    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
    if (line.front() == '[') {
      const size_t close = line.find(']');
      const std::string_view name = trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
      if (close == std::string_view::npos || name.empty()) {
        out.problems.push_back(std::format("line {}: bad section header", line_no));
        current = nullptr, in_mod = in_rooms = false;
        continue;
      }
      any_section = true;
      in_mod = same(name, "Mod");
      in_rooms = same(name, "Rooms");
      current = nullptr;
      if (in_mod || in_rooms) continue;
      const auto it = std::find_if(out.sections.begin(), out.sections.end(), [&](const Manifest::Section& section) { return same(section.name, name); });
      if (it != out.sections.end()) {
        current = &*it;
      } else {
        out.sections.push_back({std::string(name), {}});
        current = &out.sections.back();
      }
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos || !any_section) {
      out.problems.push_back(std::format("line {}: {}", line_no, any_section ? "no '='" : "a key before any section"));
      continue;
    }
    const std::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (in_mod) {
      if (same(key, "Name")) out.name = value;
      else if (same(key, "Version")) out.version = value;
      else if (same(key, "Author")) out.author = value;
      else if (same(key, "Requires")) {
        std::string_view rest = value;
        while (!rest.empty()) {
          const size_t comma = rest.find(',');
          const std::string_view item = trim(rest.substr(0, comma));
          if (!item.empty()) out.requires_features.emplace_back(item);
          rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
      } else {
        out.problems.push_back(std::format("line {}: unknown [Mod] key '{}'", line_no, key));
      }
      continue;
    }
    if (in_rooms) {
      // "<id> = <stem>": a room id from kModRoomFirst, a bare file stem (no folder, no extension).
      int id = 0;
      const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
      if (parsed.ec != std::errc() || parsed.ptr != key.data() + key.size() || id < kModRoomFirst || id > kModRoomLast) {
        out.problems.push_back(std::format("line {}: [Rooms] '{}' is not a room id {}..{}", line_no, key, kModRoomFirst, kModRoomLast));
      } else if (value.empty() || value.find_first_of("/\\.: ") != std::string_view::npos) {
        out.problems.push_back(std::format("line {}: [Rooms] {}: '{}' is not a file stem", line_no, id, value));
      } else if (std::any_of(out.rooms.begin(), out.rooms.end(), [&](const auto& room) { return room.first == id; })) {
        out.problems.push_back(std::format("line {}: [Rooms] {} is named twice", line_no, id));
      } else {
        std::string stem(value);
        for (char& letter : stem) letter = (letter >= 'A' && letter <= 'Z') ? static_cast<char>(letter + 32) : letter;
        out.rooms.emplace_back(id, std::move(stem));
      }
      continue;
    }
    if (current) current->values.emplace_back(std::string(key), std::string(value));
  }
  return out;
}

bool Resolved::enabled(int id) const {
  const auto it = values.find(id);
  return it != values.end() && !it->second.empty() && it->second.front() != 0;
}

int Resolved::value(int id, std::string_view key, std::span<const Feature> features) const {
  const Feature* feature = find(features, id);
  const int index = feature ? setting_index(*feature, key) : -1;
  if (index < 0) return 0;
  const auto it = values.find(id);
  return it != values.end() && static_cast<size_t>(index) < it->second.size() ? it->second[static_cast<size_t>(index)] : feature->settings[static_cast<size_t>(index)].def;
}

// Every feature starts at its defaults; the mods are applied lowest priority first, so the first mod in the list ends up winning each key,
// and the overrides last. A key two mods set to different values is a conflict (the higher mod wins).
Resolved resolve(std::span<const ModInput> mods, std::span<const std::pair<std::string, std::string>> overrides, std::span<const Feature> features) {
  Resolved out;
  for (const Feature& feature : features) {
    std::vector<int>& values = out.values[feature.id];
    for (const Setting& setting : feature.settings) values.push_back(setting.def);
    out.sources[feature.id].assign(feature.settings.size(), std::string());
  }
  auto apply = [&](const std::string& from, const Feature& feature, std::string_view key, std::string_view text) {
    const int index = setting_index(feature, key);
    if (index < 0) {
      out.issues.push_back({from, std::format("[{}] has no setting '{}'", feature.section, key)});
      return;
    }
    const Setting& setting = feature.settings[static_cast<size_t>(index)];
    int value = 0;
    if (!parse_value(text, value)) {
      out.issues.push_back({from, std::format("[{}] {}: '{}' is not a number", feature.section, setting.key, text)});
      return;
    }
    if (value < setting.min || value > setting.max) {
      out.issues.push_back({from, std::format("[{}] {}: {} is outside {}..{}", feature.section, setting.key, value, setting.min, setting.max)});
      value = std::clamp(value, setting.min, setting.max);
    }
    std::string& source = out.sources[feature.id][static_cast<size_t>(index)];
    int& slot = out.values[feature.id][static_cast<size_t>(index)];
    if (!source.empty() && source != from && slot != value && from != "opencross.ini")
      out.conflicts.push_back({feature.section, setting.key, from, source});
    slot = value;
    source = from;
  };
  for (const ModInput& mod : mods) {
    if (!mod.manifest) continue;
    auto use = [&](std::string_view section) {
      const Feature* feature = find(features, section);
      if (!feature) return;
      std::vector<std::string>& users = out.users[feature->id];
      if (std::find(users.begin(), users.end(), mod.name) == users.end()) users.push_back(mod.name);
    };
    for (const std::string& required : mod.manifest->requires_features) use(required);
    for (const Manifest::Section& section : mod.manifest->sections) use(section.name);
  }
  for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {
    if (!mod->manifest) continue;
    for (const std::string& required : mod->manifest->requires_features)
      if (!find(features, required)) out.issues.push_back({mod->name, std::format("needs a newer OpenCross (feature '{}')", required)});
    for (const Manifest::Section& section : mod->manifest->sections) {
      const Feature* feature = find(features, section.name);
      if (!feature) {
        out.issues.push_back({mod->name, std::format("needs a newer OpenCross (feature '{}')", section.name)});
        continue;
      }
      for (const auto& [key, text] : section.values) apply(mod->name, *feature, key, text);
    }
  }
  for (const auto& [name, text] : overrides) {
    const size_t dot = name.find('.');
    const Feature* feature = dot == std::string::npos ? nullptr : find(features, std::string_view(name).substr(0, dot));
    if (!feature) {
      out.issues.push_back({"opencross.ini", std::format("[Features] {}: no such feature", name)});
      continue;
    }
    apply("opencross.ini", *feature, std::string_view(name).substr(dot + 1), text);
  }
  return out;
}

}  // namespace oc::features
