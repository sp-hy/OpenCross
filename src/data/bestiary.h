#pragma once
// The Bestiary page's fixed numbers and places. The story areas (a room prefix and its world, or one room) in the order the story reaches
// them, each with the story value it is first entered at and where its name comes from; bestiary_order numbers the species by the first
// battle that can field them in that order and lists, per enemy record, the areas whose battles can field it.
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "import/text.h"

namespace oc::data {

// Where an area's name comes from, read in the text language: a location marker of a world-map room (`world` stem, message `msg`; with
// `sibling` >= 0 the shared beginning of two markers of the same place, "Lizard Rock - North -" / "Lizard Rock - South -"), else the
// first words of the save list's location title `chapter` (a savetbl row), else `own` (en fr it de es jp). No source: the area has no
// name and is not listed.
struct PlaceName {
  const char* world = nullptr;
  int msg = -1;
  int sibling = -1;
  int chapter = 0;
  const char* const* own = nullptr;
  bool named() const { return world || chapter > 0 || own; }
};

// `key`: an area ("opa_a": prefix and world letter, "dem": a prefix without a world) or one room stem, which takes its rooms out of their
// area. `story`: the story value (variable 0) the area is first entered at.
struct StoryArea {
  const char* key;
  int story;
  PlaceName name;
};

// The table, in story order (ascending `story`; equal values in the order the story reaches them).
std::span<const StoryArea> story_areas();
// "opa_a04f" -> "opa_a", "dem_06" -> "dem", "wld_a02" -> "wld_a".
std::string area_key(std::string_view stem);
// The table entry of a room: its own row, else its area's; -1 for a room of no listed area.
int story_area_of(std::string_view stem);

// The two worlds. A room's world is the letter after its prefix ("alu_a01f" Home, "alu_b01f" Another; an area key "alu_a" too); a stem
// with no world letter ("dem_06", "fin_19", "opening") is in neither.
enum class World : u8 { None, Home, Another };
World world_of(std::string_view stem);
// The place names (area_name, English) that rooms of both worlds carry, over `stems` (the game's rooms).
std::set<std::string> places_in_both_worlds(const std::vector<std::string>& stems);
// Per story area (story_areas() index): its world when the place name of one of its rooms in `stems` is in both worlds, else None.
std::vector<World> area_worlds(const std::vector<std::string>& stems);

// One battle op of a room: the room, the lowest story value its guards allow, and the enemy records its encounter groups place.
struct BattleSite {
  std::string stem;
  int story_lo = 0;
  std::vector<int> enemies;
};

struct BestiaryOrder {
  // The species in number order (number = index + 1), each its records in record order.
  std::vector<std::vector<int>> species;
  // Per enemy record: the story areas (story_areas() indices) whose battles can field it, in story order, each once.
  std::vector<std::vector<int>> places;
  // Per story area: the world its name is shown with (area_worlds).
  std::vector<World> worlds;
  // Per enemy record: kept from the party in every battle that fields it (battle::kept_from_party), so not listed.
  std::vector<bool> left_out;
  // Per enemy record: every battle the party fights it in ends itself on a count (battle::ends_on_count), so its HP never decides the
  // fight and the Bestiary shows "???" for it.
  std::vector<bool> counted_end;
};

// The most HP a battle unit holds: a record above it (Radius' tutorial record, 65535) never fights with its own value, its battle script
// sets the unit's HP, and the Bestiary shows "???" for it.
constexpr int kUnitHpCap = 32767;
constexpr bool hp_is_placeholder(int record_hp) { return record_hp > kUnitHpCap; }

// The species key of an English enemy name: the name with its ASCII letters lower case ("Lava-Boy" and "Lava-boy" are one species).
std::string species_key(std::string_view name);

// `sites` in scan order (rooms by stem, each room's ops in code order); `names` the records' English names (records of one species_key
// are one species); `placed` the records some encounter group names (the others are left out); `scenes` the records each battle scene
// fields. A record with an empty name is a variant of the species of the first named record it shares a scene with (in scene order,
// then group order), listed after that species' named records; with no such scene it is its own species. A site's place in the story is
// the later of its area's story value and its own guards' lowest story value, then the table order, then the scan order; a species takes
// the place of its first site, and species no site fields follow by their first record.
BestiaryOrder bestiary_order(const std::vector<BattleSite>& sites, const std::vector<std::string>& names, const std::vector<bool>& placed,
                             const std::vector<std::vector<int>>& scenes);

// A place name from its source text (markup already removed): line breaks and runs of blanks (U+3000 included) become one blank; with a
// `sibling` only the shared beginning of the two is kept; a chapter title (`chapter`) is cut at its first " -", ":" or U+3000. Blanks and
// the separators " - : ・" are then trimmed from the end.
std::string place_label(std::string_view text, const std::string* sibling, bool chapter);
// The name of story area `area` (a story_areas() index) in `lang`; empty when it has none.
std::string place_name(import::LangTables& texts, int area, import::Lang lang);

}  // namespace oc::data
