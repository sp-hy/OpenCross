// Engine features (core/features.h): the mod.ini parser, the build's table and the resolution of mods and the player's overrides.
#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/features.h"

using namespace oc;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                  \
    }                                                              \
  } while (0)

namespace {

bool has_issue(const features::Resolved& resolved, const std::string& mod, const std::string& part) {
  for (const features::Resolved::Issue& issue : resolved.issues)
    if (issue.mod == mod && issue.text.find(part) != std::string::npos) return true;
  return false;
}

void test_parse() {
  const features::Manifest manifest = features::parse_manifest(
      "; a comment\r\n"
      "[Mod]\r\n"
      "Name = Party Swap\r\n"
      "Version=1.2\r\n"
      "Author=Someone\r\n"
      "Requires=PartySwap, Other ,\r\n"
      "Colour=blue\r\n"
      "[PartySwap]\r\n"
      "Enabled=on\r\n"
      "no equals sign\r\n"
      "[partyswap]\r\n"
      "Reserve=3\r\n"
      "[Broken\r\n");
  CHECK(manifest.name == "Party Swap" && manifest.version == "1.2" && manifest.author == "Someone");
  CHECK((manifest.requires_features == std::vector<std::string>{"PartySwap", "Other"}));
  // Repeated sections merge (any case); the problems are the unknown [Mod] key, the line without '=' and the bad header.
  CHECK(manifest.sections.size() == 1 && manifest.sections[0].name == "PartySwap" && manifest.sections[0].values.size() == 2);
  CHECK(manifest.problems.size() == 3);
  CHECK(features::parse_manifest("Key=before any section\n").problems.size() == 1);
}

void test_table() {
  // Feature 0 is the extension range itself: level 1, no section (no mod sets it).
  CHECK(features::level(0) == 1);
  CHECK(features::level(9999) == 0);
  CHECK(features::find(features::table(), 0) && !features::find(features::table(), ""));
  // Settings start with Enabled; an addon (the Mods tab's toggle) has settings, a mod kit none.
  for (const features::Feature& feature : features::table()) {
    if (!feature.settings.empty()) CHECK(*feature.section && std::string(feature.settings[0].key) == "Enabled");
    CHECK(feature.player == !feature.settings.empty());
  }
  // The tiers: party swap is the player's, the mod UI a mod's (named in Requires by its section).
  CHECK(features::find(features::table(), "PartySwap")->player && !features::find(features::table(), "ModUi")->player);
}

void test_resolve() {
  constexpr std::array<features::Setting, 2> kSettings = {{{"Enabled", 0, 0, 1, "On"}, {"Reserve", 3, 1, 9, "Reserve size"}}};
  const std::array<features::Feature, 2> table = {{{0, 1, "", "Extension ops", {}}, {7, 1, "Swap", "Swap", kSettings}}};
  const features::Manifest top = features::parse_manifest("[Swap]\nEnabled=1\nReserve=5\n");
  const features::Manifest bottom = features::parse_manifest("[Mod]\nRequires=Nope\n[Swap]\nReserve=2\nColour=7\n[Future]\nOn=1\n");
  const features::Manifest wild = features::parse_manifest("[Swap]\nReserve=50\nEnabled=maybe\n");
  // Nothing: the defaults.
  const features::Resolved none = features::resolve({}, {}, table);
  CHECK(!none.enabled(7) && none.value(7, "Reserve", table) == 3 && none.issues.empty());
  // The first mod in the list wins a key (a conflict when another sets it differently); what the build lacks is an issue of that mod.
  const std::array<features::ModInput, 2> mods = {{{"Top", &top}, {"Bottom", &bottom}}};
  const features::Resolved both = features::resolve(mods, {}, table);
  CHECK(both.enabled(7) && both.value(7, "reserve", table) == 5);
  CHECK(both.sources.at(7)[1] == "Top");
  CHECK(both.conflicts.size() == 1 && both.conflicts[0].winner == "Top" && both.conflicts[0].loser == "Bottom" && both.conflicts[0].key == "Reserve");
  CHECK(has_issue(both, "Bottom", "feature 'Nope'") && has_issue(both, "Bottom", "feature 'Future'") && has_issue(both, "Bottom", "no setting 'Colour'"));
  CHECK(both.issues.size() == 3);
  // Who uses a feature: every mod that sets its section or requires it, in list order (a feature nobody names has none).
  CHECK(both.users.at(7) == std::vector<std::string>({"Top", "Bottom"}) && !both.users.count(0));
  // The player's overrides win over every mod; a value out of range is clamped, a word that is no number is ignored.
  const std::vector<std::pair<std::string, std::string>> overrides = {{"Swap.Enabled", "off"}, {"Swap.Reserve", "4"}, {"Gone.Enabled", "1"}};
  const features::Resolved player = features::resolve(mods, overrides, table);
  CHECK(!player.enabled(7) && player.value(7, "Reserve", table) == 4 && player.sources.at(7)[0] == "opencross.ini");
  CHECK(has_issue(player, "opencross.ini", "Gone.Enabled"));
  const std::array<features::ModInput, 1> wild_mod = {{{"Wild", &wild}}};
  const features::Resolved clamped = features::resolve(wild_mod, {}, table);
  CHECK(clamped.value(7, "Reserve", table) == 9 && !clamped.enabled(7));
  CHECK(has_issue(clamped, "Wild", "outside 1..9") && has_issue(clamped, "Wild", "not a number"));
}

void test_rooms() {
  // [Rooms] is not a feature: "<id> = <stem>" lines, ids 1000..0x7FFF, a bare stem (lower-cased); bad lines are problems.
  const features::Manifest top = features::parse_manifest(
      "[Rooms]\n1001 = Flux1001\n1002=hub\n999=low\n40000=high\nabc=x\n1003=map/x\n1004=x.bin\n1001=again\n");
  CHECK((top.rooms == std::vector<std::pair<int, std::string>>{{1001, "flux1001"}, {1002, "hub"}}));
  CHECK(top.sections.empty() && top.problems.size() == 6);
  // The ids are each mod's own (game/mod_rooms.h): two mods using 1001 for different rooms is no conflict, no issue, no feature user.
  const features::Manifest bottom = features::parse_manifest("[Rooms]\n1001=other\n");
  const std::array<features::ModInput, 2> mods = {{{"Top", &top}, {"Bottom", &bottom}}};
  const features::Resolved both = features::resolve(mods, {});
  CHECK(both.conflicts.empty() && both.issues.empty() && both.users.empty());
}

}  // namespace

int main() {
  test_parse();
  test_table();
  test_resolve();
  test_rooms();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("feature tests ok\n");
  return 0;
}
