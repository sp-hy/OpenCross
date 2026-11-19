// The collection file of the record pages: where it lives, what a field battle adds to it (the enemies met, the ones beaten and the loot
// each gave), what a loaded or saved game adds (its characters, elements and key items), the scan's knowledge handed to the pages, the
// OpenCross row's pages and Options pages, the Chronicle's Reset, and the General tab's OpenCross menu row. Recording runs whether or not
// [Game] OpenCrossMenu shows the row. Only field battles record enemies: a [Debug] Battle runs no field battle, a versus battle is
// skipped, and a --config run (routes, tests) records in memory and never writes the file.
#include <imgui.h>

#include <string>

#include "app/app.h"
#include "core/log.h"
#include "game/records_info.h"
#include "save/grid_preset.h"

namespace oc::app {

void App::init_collection() {
  if (oc_saves.dir().empty()) return;
  collection.set_path(oc_saves.dir() / save::Collection::kFileName);
  collection.load();
  if (config.opencross_menu && data_ready()) scan_service.start(files);
}

void App::save_collection() {
  if (own_ini) collection.save();
}

// Every enemy unit the scene put on the field is met.
void App::record_battle_start() {
  const battle::Battle* fight = battle.battle();
  if (versus_running || !fight) return;
  std::string met;
  bool changed = false;
  for (int i = battle::kPcSlots; i < battle::kUnits; ++i) {
    const battle::Unit& unit = fight->unit(i);
    if (!unit.present || unit.pc) continue;
    changed = collection.mark_enemy(unit.src_id, save::Collection::kEnemyMet) || changed;
    met += " " + std::to_string(unit.src_id);
  }
  log::info("records: scene {} met{}", battle_scene, met);
  if (changed) save_collection();
}

// The enemies down or marked defeated at the end are beaten, whatever the outcome; on a victory the drops and the pilfered words the party
// receives mark their records' loot slots, unless the randomizer is on (the loot is not the record's then).
void App::record_battle_end(int code) {
  const battle::Battle* fight = battle.battle();
  if (!fight) return;
  std::string line;
  bool changed = false;
  for (const int monster : fight->defeated_enemies()) {
    changed = collection.mark_enemy(monster, save::Collection::kEnemyMet | save::Collection::kEnemyDefeated) || changed;
    line += " " + std::to_string(monster);
  }
  line += randomizer_settings.enabled() ? " loot not recorded (randomizer)" : " loot";
  if (fight->outcome() == battle::Outcome::Victory && !randomizer_settings.enabled()) {
    const battle::Rewards& rewards = fight->rewards();
    for (const std::vector<battle::Rewards::Loot>* loot : {&rewards.drop_loot, &rewards.stolen_loot})
      for (const battle::Rewards::Loot& got : *loot) {
        changed = collection.mark_enemy(got.monster, save::Collection::loot_flag(got.slot)) || changed;
        line += " " + std::to_string(got.monster) + ":" + std::to_string(got.slot);
      }
  }
  log::info("records: scene {} result {} beaten{}", battle_scene, code, line);
  if (changed) save_collection();
}

void App::record_slot(const char* why) {
  if (!flow_ready) return;
  if (collection.record_slot(flow.game_state().slot)) {
    log::info("records: the {} (story {}) added characters, elements or key items", why, flow.game_state().slot.var(0));
    save_collection();
  }
}

void App::update_records_info() {
  if (records_info || !config.opencross_menu) return;
  if (const game::ScriptScan* scan = scan_service.ready()) {
    records_info = game::records_info(*scan, tables);
    menu.set_records_info(&*records_info);
    log::info("records: {} treasures in {} areas", records_info->treasures.size(), records_info->areas.size());
  }
}

void App::reset_collection() {
  const Bytes* held = collection.chunk(save::GridPresets::kChunk);
  const Bytes presets = held ? *held : Bytes{};
  collection.clear();
  if (held) collection.set_chunk(save::GridPresets::kChunk, presets);
  save_collection();
  log::info("records: the collection was reset");
}

// The pages follow the switch; the Options pages show the settings as they are and store what they change in the ini.
void App::update_opencross_menu() {
  if (!menu_ok) return;
  for (const ui::OcPage page : {ui::OcPage::Bestiary, ui::OcPage::Characters, ui::OcPage::Elements, ui::OcPage::KeyItems, ui::OcPage::Treasure,
                                ui::OcPage::DialogueLog, ui::OcPage::Extras, ui::OcPage::Achievements})
    menu.set_oc_page(page, config.opencross_menu);
  menu.set_records_show_all(config.records_show_all);
  menu.set_records_hints(config.records_hints);
  menu.set_extras_unlock_all(config.extras_unlock_all);
  menu.set_achievement_settings(config.achievements, config.hide_achievement_notices);
  if (!config.opencross_menu) return;
  menu.set_enemy_order(scan_service.bestiary());
  update_records_info();
}

void App::opencross_menu_row() {
  using render::ImGuiLayer;
  ImGuiLayer::row("OpenCross menu", "Adds the OpenCross row to the main menu, above Save/Load. Chronicle: every enemy you have met, its stats "
                                    "once beaten and each drop or steal once received; every character, element and key item you have had; "
                                    "the treasures opened in each area. Extras: a jukebox of the songs you have heard, a theatre of the movies "
                                    "you have watched, the portraits and the menu models. Dialogue log: the messages you have read, also "
                                    "over the field with L or the pad's RB + Back. Achievements: OpenCross's own achievements, kept in "
                                    "achievements.txt next to opencross.ini. Their options are on each one's Options page. What the others "
                                    "find is kept for every game and every save, in the OpenCross save folder; nothing goes into your saves.");
  if (ImGui::Checkbox("##opencrossmenu", &config.opencross_menu)) {
    ui.mark_changed();
    if (config.opencross_menu && data_ready()) scan_service.start(files);
  }
}

}  // namespace oc::app
