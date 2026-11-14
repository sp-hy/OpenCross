#pragma once
// Engine features that mods turn on: the build's feature table, the manifest a mod carries at its root
// (mod.ini), and the resolution of the enabled mods' manifests and the player's overrides into the feature settings of a run.
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oc::features {

// One setting of a feature: an integer (an on / off setting is 0 / 1) with its default and its range.
struct Setting {
  const char* key;
  int def;
  int min;
  int max;
  const char* label;
};

// One feature. `id` = the number GET_FEATURE_LEVEL (field script FE C0) takes, `level` = the version this build implements, `section` =
// its mod.ini section (a mod sets it, or names it in Requires) and its key prefix in opencross.ini [Features] (empty: no mod names it,
// as feature 0, the extension range itself). A feature with settings has the setting "Enabled" first, except the randomizer:
// it is on while any of its switches is, and a stored "Enabled=0" turns them all off.
// `player`: an addon works on the game as shipped, so the player keeps its toggle and settings in the Mods tab (it has settings). Otherwise
// it is a mod kit: it does nothing without a mod's own scripts or data, has no settings (its mod.ini section takes no keys), and the Mods
// tab only names the enabled mods that use it. `about` = one or
// two sentences for the Mods tab (a mod kit's ends with the ops a modder uses).
struct Feature {
  int id;
  int level;
  const char* section;
  const char* label;
  std::span<const Setting> settings;
  bool player = false;
  const char* about = "";
};

// Feature ids (GET_FEATURE_LEVEL, the table's `id`).
constexpr int kExtensionRange = 0;
constexpr int kPartySwap = 1;
constexpr int kModUi = 2;
constexpr int kPartyScreen = 3;
constexpr int kModelSources = 5;
constexpr int kButtonPanel = 6;
constexpr int kRandomizer = 7;

std::span<const Feature> table();
// nullptr when the build has no such feature. Sections compare case-insensitively.
const Feature* find(std::span<const Feature> features, int id);
const Feature* find(std::span<const Feature> features, std::string_view section);
// The level of feature `id` in this build, 0 when it has none.
int level(int id);

// Mod rooms: the game's own room ids stop below 1000, so a mod's new rooms take ids from kModRoomFirst up to a script's literal room id.
// The ids are the mod's own (game/mod_rooms.h): two mods may both use 1001; the stem is the room's identity.
constexpr int kModRoomFirst = 1000;
constexpr int kModRoomLast = 0x7FFF;

// Mod elements: the game's element records stop at 526, so a mod's own element ids (its scripts' and its text's numbers for the records
// it adds by name, data/table_rows.h) run from kModElementFirst to kModElementLast. Two mods may both use 527; the name is the identity.
constexpr int kModElementFirst = 527;
constexpr int kModElementLast = 0x3FFF;
// Mod shops: the game's element shops and blacksmiths are rows 0..31, so a mod's own shop ids run from kModShopFirst.
constexpr int kModShopFirst = 32;
constexpr int kModShopLast = 0x7FFF;
// Mod monsters and formations: the game's monster records stop at 221 and its battle scenes at 512, so a mod's own ids start after
// them. A formation id is a scene: a field script starts it as battle id 0x200 + id (15-bit operand, so ids up to 0x7DFF).
constexpr int kModMonsterFirst = 222;
constexpr int kModMonsterLast = 0x3FFF;
constexpr int kModFormationFirst = 513;
constexpr int kModFormationLast = 0x7DFF;
// Mod variables: a field script's variable handles are byte offsets (0x000..0x3FE saved, 0x400..0x7FE the room's own), so a mod's own
// saved variables take the even handles kModVarFirst..kModVarLast. A bit operand reaches them too (handle << 4 | bit: 0x8000..0xFFEF).
// Two mods may both use 0x800; the name is the identity (game/mod_vars.h).
constexpr int kModVarFirst = 0x800;
constexpr int kModVarLast = 0xFFE;

// A record name (a table row header, an [Elements] value, a variable name): a lower-case letter, then lower-case letters, digits and '_'.
bool is_record_name(std::string_view name);

// The mod.ini sections that give a mod's own ids for records mods add by name ("<id> = <record name>"), the table they name records of
// (data/table_rows.h) and the ids they take: [Elements] pgdat, [ElementShops] el_shop, [Smiths] waa_shop, [Monsters] monster,
// [Formations] scenedat.
struct OwnIdSection {
  const char* section;
  const char* table;
  int first;
  int last;
};
std::span<const OwnIdSection> own_id_sections();

// A parsed mod.ini: [Mod] Name / Version / Author / Requires (a comma-separated list of feature sections the mod's own scripts use),
// [Rooms] (the room ids this mod's scripts and text use, "<id> = <stem>": the files cdrom/map/mapbin/<stem>.bin / .prd / .ctd, its own
// rooms or another mod's), the own-id sections (own_id_sections: the record ids this mod's scripts and text use), [Variables] (the saved
// variables this mod's scripts use, "<handle> = <name>"), and every other section as written (a feature section, checked by resolve).
// `problems` = lines that are not understood.
struct Manifest {
  struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
  };
  std::string name, version, author;
  std::vector<std::string> requires_features;
  // [Rooms]: ids kModRoomFirst..kModRoomLast, stems in lower case, in file order.
  std::vector<std::pair<int, std::string>> rooms;
  // The own-id sections: table (OwnIdSection::table) -> its ids and record names, in file order.
  std::map<std::string, std::vector<std::pair<int, std::string>>> own_ids;
  // [Variables]: even handles kModVarFirst..kModVarLast (0x-prefixed hex or decimal), variable names (is_record_name), in file order.
  std::vector<std::pair<int, std::string>> variables;
  std::vector<Section> sections;
  std::vector<std::string> problems;
};
Manifest parse_manifest(std::string_view text);

// One enabled mod with a manifest, in priority order (first wins).
struct ModInput {
  std::string name;
  const Manifest* manifest = nullptr;
};

// The settings of a run. `values` = per feature id the value of each setting (table order); `sources` = where each came from: "" the
// default, a mod name, or "opencross.ini". `issues` = per mod name what the build cannot honour (a feature it does not have, an unknown
// key, a value out of range) and the player's own; `conflicts` = a key two mods set, with the mod that wins.
struct Resolved {
  struct Issue {
    std::string mod;
    std::string text;
  };
  struct Conflict {
    std::string section, key, winner, loser;
  };
  std::map<int, std::vector<int>> values;
  std::map<int, std::vector<std::string>> sources;
  // Per feature id the mods that use it (set its section or name it in Requires), in list order.
  std::map<int, std::vector<std::string>> users;
  std::vector<Issue> issues;
  std::vector<Conflict> conflicts;
  // False / the default when the feature or the key is unknown.
  bool enabled(int id) const;
  int value(int id, std::string_view key, std::span<const Feature> features = table()) const;
};

// `overrides` = opencross.ini [Features] entries: "<Section>.<Key>" = value; they win over every mod.
Resolved resolve(std::span<const ModInput> mods, std::span<const std::pair<std::string, std::string>> overrides,
                 std::span<const Feature> features = table());

}  // namespace oc::features
