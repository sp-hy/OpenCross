// Cheats: the [Cheats] ini round trip (all off by default, RDE's toggles session-only), the first-launch language mapping, RDE's
// encounter tables and its text pace.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "game/cheats.h"
#include "platform/config.h"
#include "platform/keymap.h"

#include <SDL3/SDL_scancode.h>

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)
}  // namespace

int main() {
  // --- ini: every toggle defaults off, keeps its value through save / load, and a key is only set when asked
  platform::Config config;
  const platform::Cheats& cheats = config.cheats;
  CHECK(!cheats.no_encounters && !cheats.battle_boost && !cheats.auto_battle && !cheats.god_mode && !cheats.no_clip && !cheats.max_stamina && !cheats.free_elements &&
        !cheats.always_hit && !cheats.max_money && !cheats.shop_all_elements && !cheats.shop_all_weapons && !cheats.run_speed && !cheats.always_loot && !cheats.instant_growth && !cheats.no_craft_cost && !cheats.shared_growth);
  config.cheats.no_encounters = config.cheats.no_clip = config.cheats.always_loot = config.cheats.no_craft_cost = config.cheats.shop_all_weapons = config.cheats.shared_growth = true;
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "oc_test_cheats.ini";
  CHECK(config.save(path));
  platform::Config reloaded;
  CHECK(reloaded.load(path));
  std::filesystem::remove(path);
  CHECK(reloaded.cheats.no_clip && reloaded.cheats.always_loot && reloaded.cheats.no_craft_cost && reloaded.cheats.shop_all_weapons && reloaded.cheats.shared_growth);
  CHECK(!reloaded.cheats.god_mode && !reloaded.cheats.max_money && !reloaded.cheats.instant_growth);
  // RDE's five play-time toggles are never written; a hand-written (or --config) ini still sets them, and clear_session_toggles
  // switches them off as every launch with the player's ini does.
  CHECK(!reloaded.cheats.no_encounters);
  {
    platform::Config toggles;
    toggles.fast_mode = toggles.cheats.no_encounters = toggles.cheats.battle_boost = toggles.cheats.auto_battle = true;
    toggles.fast_rate = 3;
    CHECK(toggles.save(path));
    std::stringstream text;
    text << std::ifstream(path).rdbuf();
    for (const char* key : {"FastMode", "SlowMode", "NoEncounters", "BattleBoost", "AutoBattle"}) CHECK(text.str().find(key) == std::string::npos);
    CHECK(text.str().find("FastRate=3") != std::string::npos);
    std::ofstream(path, std::ios::app) << "[Game]\nSlowMode=1\n[Cheats]\nNoEncounters=1\nBattleBoost=1\nAutoBattle=1\n";
    platform::Config hand;
    CHECK(hand.load(path));
    CHECK(hand.slow_mode && hand.cheats.no_encounters && hand.cheats.battle_boost && hand.cheats.auto_battle && hand.fast_rate == 3);
    hand.clear_session_toggles();
    CHECK(!hand.fast_mode && !hand.slow_mode && !hand.cheats.no_encounters && !hand.cheats.battle_boost && !hand.cheats.auto_battle && hand.fast_rate == 3);
    std::filesystem::remove(path);
  }

  // --- [Ui] settings window geometry: default -1, round trip
  platform::Config ui_config;
  CHECK(ui_config.settings_x == -1 && ui_config.settings_y == -1 && ui_config.settings_w == -1 && ui_config.settings_h == -1 && ui_config.settings_tab.empty());
  ui_config.settings_x = 120, ui_config.settings_y = 80, ui_config.settings_w = 900, ui_config.settings_h = 640, ui_config.settings_tab = "Cheats";
  const std::filesystem::path f2 = std::filesystem::temp_directory_path() / "oc_test_ui.ini";
  CHECK(ui_config.save(f2));
  platform::Config ui_reloaded;
  CHECK(ui_reloaded.load(f2));
  std::filesystem::remove(f2);
  CHECK(ui_reloaded.settings_x == 120 && ui_reloaded.settings_y == 80 && ui_reloaded.settings_w == 900 && ui_reloaded.settings_h == 640 && ui_reloaded.settings_tab == "Cheats");

  // --- first launch language: en fr it de es -> same, ja -> jp, anything else en; the first supported preference wins
  using platform::language_from_locales;
  CHECK(language_from_locales({"fr"}) == "fr" && language_from_locales({"de-DE"}) == "de" && language_from_locales({"ES"}) == "es");
  CHECK(language_from_locales({"ja"}) == "jp" && language_from_locales({"it_IT"}) == "it" && language_from_locales({"en"}) == "en");
  CHECK(language_from_locales({"pt", "fr"}) == "fr");
  CHECK(language_from_locales({"zh"}) == "en" && language_from_locales({}) == "en");

  // --- Auto-won encounters, table A (no story byte known: -1)
  const game::StoryByte none = [](unsigned) { return -1; };
  using game::encounter_auto_won;
  // An ordinary A3 is won; so are id 0 and ids >= 0x57.
  CHECK(encounter_auto_won(0x100, 4, 0x20, false, none));
  CHECK(encounter_auto_won(0x100, 4, 0, false, none));
  CHECK(encounter_auto_won(0x100, 4, 0x57, false, none));
  // Map 0x213 always fights; id 3 at map 0x110 entity 0x16 runs but is won elsewhere; id 0xC at map 0x9E fights for any entity.
  CHECK(!encounter_auto_won(0x213, 4, 0x20, false, none));
  CHECK(!encounter_auto_won(0x110, 0x16, 3, false, none));
  CHECK(encounter_auto_won(0x110, 0x15, 3, false, none));
  CHECK(!encounter_auto_won(0x9E, 1, 0x0C, false, none));
  CHECK(!encounter_auto_won(0x32, 4, 1, false, none) && encounter_auto_won(0x32, 5, 1, false, none));
  // With the story byte's bit 2 set the fight is always won.
  CHECK(encounter_auto_won(0x32, 4, 1, false, [](unsigned var) { return var == 0x1C0 ? 4 : -1; }));
  CHECK(!encounter_auto_won(0x1A0, 8, 0x3B, false, none) && !encounter_auto_won(0x1A0, 0x0A, 0x3B, false, none));
  // An unknown story byte lets the fight run.
  CHECK(!encounter_auto_won(0x50, 1, 0x42, false, none));
  CHECK(!encounter_auto_won(0x50, 1, 0x42, false, [](unsigned var) { return var == 0xDC ? 1 : var == 0xC4 ? 2 : -1; }));
  CHECK(encounter_auto_won(0x50, 1, 0x42, false, [](unsigned var) { return var == 0xDC ? 0 : var == 0xC4 ? 2 : -1; }));
  // The story bytes come from the VM's global variable array (byte ids: even = low byte, odd = high byte of the slot).
  {
    std::vector<u16> vars(0x400, 0);
    CHECK(game::story_byte_from_vars(vars, 0xDC) == 0 && game::story_byte_from_vars(vars, game::kStoryVarBytes) == -1 &&
          game::story_byte_from_vars(vars, 0xFFFF) == -1);
    // The low byte sits at the even id, the high byte at the odd one.
    vars[0xDC / 2] = 0x1234;
    CHECK(game::story_byte_from_vars(vars, 0xDC) == 0x34 && game::story_byte_from_vars(vars, 0xDD) == 0x12);
    const game::StoryByte fromvars = [&](unsigned var) { return game::story_byte_from_vars(vars, var); };
    // Id 0x42 at an ordinary map: story byte A = 1 and B = 2 -> the story battle runs; A = 0 -> won (skipped)
    vars[0xDC / 2] = 1, vars[0xC4 / 2] = 2;
    CHECK(!encounter_auto_won(0x50, 1, 0x42, false, fromvars));
    vars[0xDC / 2] = 0;
    CHECK(encounter_auto_won(0x50, 1, 0x42, false, fromvars));
    // Id 0x01: a clear bit 2 of its story byte keeps the fight at map 0x32 / entity 4, a set one wins it
    CHECK(!encounter_auto_won(0x32, 4, 1, false, fromvars));
    vars[0x1C0 / 2] = 4;
    CHECK(encounter_auto_won(0x32, 4, 1, false, fromvars));
    // Id 0x45 depends on bit 6 of its story byte; table B (model swaps) on bit 7 of another.
    CHECK(encounter_auto_won(0x50, 1, 0x45, false, fromvars));
    vars[0x1C6 / 2] = 0x40;
    CHECK(!encounter_auto_won(0x50, 1, 0x45, false, fromvars));
    CHECK(!game::model_swap_skipped(0x9E, 5, fromvars));
    vars[0x108 / 2] = 0x80;
    CHECK(game::model_swap_skipped(0x9E, 5, fromvars));
  }
  // C9: only 0x180 0x181 0x187 0x1B3 are won
  CHECK(encounter_auto_won(0x50, 1, 0x180, true, none) && encounter_auto_won(0x50, 1, 0x1B3, true, none));
  CHECK(!encounter_auto_won(0x50, 1, 0x182, true, none));

  // --- Table B (D9): needs bit 7 of its story byte
  const game::StoryByte b7 = [](unsigned var) { return var == 0x108 ? 0x80 : -1; };
  // Unknown story byte: never skipped.
  CHECK(!game::model_swap_skipped(0x9E, 5, none));
  CHECK(game::model_swap_skipped(0x9E, 5, b7) && game::model_swap_skipped(0xAB, 9, b7));
  // Steps below 4 are never skipped.
  CHECK(!game::model_swap_skipped(0x9E, 3, b7));
  CHECK(game::model_swap_skipped(0x99, 5, b7) && !game::model_swap_skipped(0x99, 0x0C, b7));
  CHECK(!game::model_swap_skipped(0xAA, 0x0E, b7) && game::model_swap_skipped(0xAA, 6, b7));
  // Other maps: never skipped.
  CHECK(!game::model_swap_skipped(0xA0, 6, b7));

  // --- RDE hotkeys: toggles flip on the press only; slow / fast exclude each other; the speed keys do nothing in the Radius tutorial
  {
    game::RdeHotkeys keys;
    game::Toggles on;
    auto frame = [&](game::HotkeyLevels in, bool locked = false) { keys.poll(in, locked, on); };
    game::HotkeyLevels fast_key;
    fast_key.fast_key = true;
    frame(fast_key);
    frame(fast_key);
    CHECK(on.fast && !on.slow);
    frame({});
    game::HotkeyLevels lt;
    lt.lt = true;
    frame(lt);
    CHECK(on.slow && !on.fast);
    frame({});
    frame(lt);
    CHECK(!on.slow && !on.fast);
    frame({});
    frame(fast_key, true);
    CHECK(!on.fast);
    frame({});
    // F3 / F4 / F5
    game::HotkeyLevels fkeys;
    fkeys.encounters_key = fkeys.boost_key = fkeys.auto_key = true;
    frame(fkeys);
    frame(fkeys);
    CHECK(on.encounters && on.boost && on.auto_battle);
    frame({});
    frame(fkeys);
    frame({});
    CHECK(!on.encounters && !on.boost && !on.auto_battle);
    // L3 tap = encounters, R3 tap = boost, both on release
    game::HotkeyLevels l3, r3, both;
    l3.l3 = true, r3.r3 = true, both.l3 = both.r3 = true;
    frame(l3);
    CHECK(!on.encounters);
    frame({});
    CHECK(on.encounters && !on.boost && !on.auto_battle);
    frame(r3);
    frame({});
    CHECK(on.boost);
    // L3 + R3: auto battle when one is let go; letting go of the other one does not tap
    frame(l3);
    frame(both);
    frame(r3);
    CHECK(on.auto_battle && on.encounters && on.boost);
    frame({});
    CHECK(on.auto_battle && on.encounters && on.boost);
  }
  // --- speed factors: RDE's own = 4x field / 2x battle; slow = percent; nothing in the tutorial
  CHECK(game::speed_factor(true, false, 0, 50, false, false) == 4.0f && game::speed_factor(true, false, 0, 50, true, false) == 2.0f);
  CHECK(game::speed_factor(true, false, 3, 50, true, false) == 3.0f && game::speed_factor(false, true, 0, 50, false, false) == 0.5f);
  CHECK(game::speed_factor(true, false, 0, 50, false, true) == 1.0f && game::speed_factor(false, false, 0, 50, false, false) == 1.0f);
  // --- field slow mode: 299 slow 1/30 s frames, then one at normal speed; restarts when slow mode is left
  {
    game::SlowFieldCycle cycle;
    const double frame = game::SlowFieldCycle::kFrame;
    double game_time = 0.0;
    for (int i = 0; i < 300; ++i) {
      const float factor = cycle.apply(frame, true, 0.5f);
      CHECK(std::abs(factor - (i == 299 ? 1.0f : 0.5f)) < 1e-4f);
      game_time += frame * factor;
    }
    CHECK(std::abs(game_time - 301.0 * frame / 2.0) < 1e-6);
    CHECK(std::abs(cycle.apply(frame, true, 0.5f) - 0.5f) < 1e-4f);
    // A 1/60 s frame half way into the normal frame: 3/4 speed.
    game::SlowFieldCycle fine;
    for (int i = 0; i < 599; ++i) fine.apply(frame / 2.0, true, 0.5f);
    CHECK(std::abs(fine.apply(frame * 0.75, true, 0.5f) - (0.5 * (1.0 / 3.0) + 2.0 / 3.0)) < 1e-4);
    // Leaving slow mode restarts the count; the frame's own factor comes back.
    CHECK(fine.apply(frame, false, 2.0f) == 2.0f);
    for (int i = 0; i < 299; ++i) CHECK(fine.apply(frame, true, 0.5f) == 0.5f);
  }
  // --- [Game] speed settings: RDE defaults; the speed keys are RDE's fixed F1 / F2
  {
    platform::Config defaults;
    CHECK(!defaults.fast_mode && !defaults.slow_mode && defaults.fast_rate == 0 && defaults.slow_rate == 50);
    // RDE's text pace: fast reveals x2.25 with waits / 8, slow x0.5 with waits x2.
    const ui::TextPace fast = game::text_pace(true, false), slow = game::text_pace(false, true), normal = game::text_pace(false, false);
    CHECK(fast.reveal == 2.25f && fast.wait == 0.125f && slow.reveal == 0.5f && slow.wait == 2.0f && normal.reveal == 1.0f && normal.wait == 1.0f);
    using platform::Action;
    CHECK(platform::action_keys(Action::SlowSpeed)[0] == SDL_SCANCODE_F1 && platform::action_keys(Action::FastSpeed)[0] == SDL_SCANCODE_F2);
  }
  // --- RDE's key sets: Ok Enter / X, Cancel Backspace / Left Shift, Menu I / Tab; key caps of set 0
  {
    using platform::Action;
    CHECK(platform::action_keys(Action::Ok)[0] == SDL_SCANCODE_RETURN && platform::action_keys(Action::Ok)[1] == SDL_SCANCODE_X);
    CHECK(platform::action_keys(Action::Cancel)[0] == SDL_SCANCODE_BACKSPACE && platform::action_keys(Action::Cancel)[1] == SDL_SCANCODE_LSHIFT);
    CHECK(platform::action_keys(Action::Menu)[0] == SDL_SCANCODE_I && platform::action_keys(Action::Menu)[1] == SDL_SCANCODE_TAB);
    CHECK(platform::keyboard_cell(platform::action_vk(Action::Menu)) == 27 && platform::keyboard_cell(platform::action_vk(Action::Item)) == 24);
    platform::Input in;
    in.key_pressed[SDL_SCANCODE_X] = true;
    CHECK(platform::action_pressed(in, Action::Ok) && !platform::action_pressed(in, Action::Cancel));
  }

  if (failures) return 1;
  std::printf("test_cheats: ok\n");
  return 0;
}
