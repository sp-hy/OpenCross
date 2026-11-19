// The Enemies page's numbering: area keys, the story-area table, the species order from battle sites, the world of a room and of a story
// area's name, and the place names cut from their source text. No game data is read.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "data/bestiary.h"

using namespace oc;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void test_keys() {
  CHECK(data::area_key("opa_a04f") == "opa_a");
  CHECK(data::area_key("jac_b16i") == "jac_b");
  CHECK(data::area_key("wld_a02") == "wld_a");
  CHECK(data::area_key("dem_06") == "dem");
  CHECK(data::area_key("fin_27") == "fin");
  const auto areas = data::story_areas();
  const int opassa = data::story_area_of("opa_a04f"), lizard = data::story_area_of("opa_a01f");
  CHECK(opassa >= 0 && std::string(areas[static_cast<size_t>(opassa)].key) == "opa_a04f");
  CHECK(lizard >= 0 && std::string(areas[static_cast<size_t>(lizard)].key) == "opa_a");
  CHECK(data::story_area_of("tel_b04f") < 0);
  CHECK(data::story_area_of("xyz_a01f") < 0);
  for (size_t i = 1; i < areas.size(); ++i) CHECK(areas[i - 1].story <= areas[i].story);
}

static void test_order() {
  // 0 A, 1 B, 2 A (second variant), 3 C, 4 D, 5 E (no encounter group names it), 6 F (no battle fields it), 7 G (a room of no area).
  const std::vector<std::string> names = {"A", "B", "A", "C", "D", "E", "F", "G"};
  const std::vector<bool> placed = {true, true, true, true, true, false, true, true};
  std::vector<data::BattleSite> sites = {
      {"alu_a02i", 112, {3}},
      {"alu_a04f", 0, {2}},
      {"hid_a01f", 0, {4}},
      {"jac_b05f", 0, {1, 0}},
      {"xyz_a01f", 0, {7}},
      {"opa_a01f", 0, {5}},
  };
  const data::BestiaryOrder order = data::bestiary_order(sites, names, placed, {});
  // A at Arni (8) from record 2; B in Viper Manor (36); D in the Home Hydra Marshes (104) before C, whose battle needs story 112; G in an
  // unlisted room; F fielded nowhere; E left out.
  CHECK(order.species.size() == 6);
  if (order.species.size() == 6) {
    CHECK((order.species[0] == std::vector<int>{0, 2}));
    CHECK((order.species[1] == std::vector<int>{1}));
    CHECK((order.species[2] == std::vector<int>{4}));
    CHECK((order.species[3] == std::vector<int>{3}));
    CHECK((order.species[4] == std::vector<int>{7}));
    CHECK((order.species[5] == std::vector<int>{6}));
  }
  CHECK(order.places.size() == names.size());
  CHECK(order.places[0].size() == 1 && order.places[0][0] == data::story_area_of("jac_b05f"));
  CHECK(order.places[2].size() == 1 && order.places[2][0] == data::story_area_of("alu_a04f"));
  CHECK(order.places[7].empty() && order.places[6].empty());
  // A site scanned later in an earlier area moves B's place to Arni, still after A's (same area, later in the scan).
  sites.push_back({"alu_a01f", 0, {1}});
  const data::BestiaryOrder again = data::bestiary_order(sites, names, placed, {});
  CHECK(again.species.size() == 6 && (again.species[1] == std::vector<int>{1}));
  CHECK(again.places[1].size() == 2 && again.places[1][0] == data::story_area_of("alu_a01f"));
  // Names that differ only in letter case are one species.
  const data::BestiaryOrder cased = data::bestiary_order({{"des_b01f", 0, {1}}}, {"Lava-Boy", "Lava-boy"}, {true, true}, {});
  CHECK(cased.species.size() == 1 && (cased.species[0] == std::vector<int>{0, 1}));
  // Unnamed records: 0 shares a scene with H and joins it after H's own records; 3 shares a scene only with another unnamed one and
  // stays its own entry, as does 4 with no scene.
  const std::vector<std::vector<int>> scenes = {{3, 5}, {0, 1}, {2, 0}};
  const data::BestiaryOrder hosted =
      data::bestiary_order({{"fat_b01f", 0, {1, 0}}}, {"", "H", "J", "", "", ""}, {true, true, true, true, true, true}, scenes);
  CHECK(hosted.species.size() == 5);
  if (hosted.species.size() == 5) {
    CHECK((hosted.species[0] == std::vector<int>{1, 0}));
    CHECK((hosted.species[1] == std::vector<int>{2}));
    CHECK((hosted.species[2] == std::vector<int>{3}));
  }
}

static void test_worlds() {
  CHECK(data::world_of("alu_a01f") == data::World::Home);
  CHECK(data::world_of("alu_b01f") == data::World::Another);
  CHECK(data::world_of("wld_a02") == data::World::Home && data::world_of("wld_b00") == data::World::Another);
  CHECK(data::world_of("opa_b") == data::World::Another);
  CHECK(data::world_of("dem_06") == data::World::None && data::world_of("fin_19") == data::World::None);
  CHECK(data::world_of("opening") == data::World::None && data::world_of("blk_00") == data::World::None);
  // Arni Village and Lizard Rock have rooms in both worlds; Terra Tower only Home rooms, Time Fortress only Another ones; chi has no place
  // name, dem no world.
  const std::vector<std::string> stems = {"alu_a01f", "alu_a04f", "alu_b01f", "opa_a01f", "opa_b01f", "opa_a04f", "sta_a01i",
                                          "crn_b01i", "chi_a01d", "chi_b01d", "dem_06", "jac_b29i"};
  const std::set<std::string> both = data::places_in_both_worlds(stems);
  CHECK((both == std::set<std::string>{"Arni Village", "Lizard Rock"}));
  const std::vector<data::World> worlds = data::area_worlds(stems);
  CHECK(worlds.size() == data::story_areas().size());
  const auto world_at = [&](const char* stem) { return worlds[static_cast<size_t>(data::story_area_of(stem))]; };
  CHECK(world_at("alu_a01f") == data::World::Home);
  CHECK(world_at("opa_b01f") == data::World::Another);
  // Opassa Beach has only its Home room in the list; Terra Tower, Time Fortress, the bluffs, the Hydra's room and Viper Manor (one world
  // here) keep their plain names.
  CHECK(world_at("opa_a04f") == data::World::None);
  CHECK(world_at("sta_a01i") == data::World::None && world_at("crn_b01i") == data::World::None);
  CHECK(world_at("chi_a01d") == data::World::None && world_at("dem_06") == data::World::None && world_at("jac_b29i") == data::World::None);
  CHECK(world_at("mis_a01f") == data::World::None);
}

static void test_labels() {
  const std::string north = "Lizard Rock\n - North -", south = "Lizard Rock\n - South -";
  CHECK(data::place_label(north, &south, false) == "Lizard Rock");
  const std::string nord = "Roc aux L\xC3\xA9zards\xC2\xA0: nord", sud = "Roc aux L\xC3\xA9zards\xC2\xA0: sud";
  CHECK(data::place_label(nord, &sud, false) == "Roc aux L\xC3\xA9zards");
  const std::string kita = "\xE3\x83\x88\xE3\x82\xAB\xE3\x82\xB2\xE5\xB2\xA9\xE3\x83\xBB\xE5\x8C\x97";
  const std::string minami = "\xE3\x83\x88\xE3\x82\xAB\xE3\x82\xB2\xE5\xB2\xA9\xE3\x83\xBB\xE5\x8D\x97";
  CHECK(data::place_label(kita, &minami, false) == "\xE3\x83\x88\xE3\x82\xAB\xE3\x82\xB2\xE5\xB2\xA9");
  CHECK(data::place_label("Viper Manor\n     Bluffs", nullptr, false) == "Viper Manor Bluffs");
  CHECK(data::place_label("Temporal Vortex -", nullptr, true) == "Temporal Vortex");
  CHECK(data::place_label("-Fato del presente: Clotho", nullptr, false) == "Fato del presente: Clotho");
  CHECK(data::place_label("Die Zeitkr\xC3\xBCmmung: Das Reich", nullptr, true) == "Die Zeitkr\xC3\xBCmmung");
  CHECK(data::place_label("Der Opassa-Strand: Ein Ruf", nullptr, true) == "Der Opassa-Strand");
  CHECK(data::place_label("\xE6\xAD\xBB\xE6\xB5\xB7\xE3\x80\x80\xE7\xA5\x9E", nullptr, true) == "\xE6\xAD\xBB\xE6\xB5\xB7");
}

int main() {
  test_keys();
  test_order();
  test_worlds();
  test_labels();
  if (g_fail) std::printf("%d failure(s)\n", g_fail);
  else std::printf("bestiary: all passed\n");
  return g_fail ? 1 : 0;
}
