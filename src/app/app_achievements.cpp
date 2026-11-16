// OpenCross's own achievements: the profile file next to the player's ini, the tab, the per-frame feed of game::OcAchievements (room
// samples, the battle's actions, forges) and the unlock popup.
#include "app/app.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include "core/log.h"
#include "script/field_vm.h"

namespace oc::app {

static_assert(game::OcAchievements::kCharacters == save::kNumChars);

// Pip's record holds his current form whether or not he is in the party.
constexpr int kPip = 43;

// A --config run keeps its unlocks in memory: no file is read or written.
void App::init_oc_achievements() {
  ui.add_tab("Achievements", [this] {
    if (game::draw_oc_achievements_tab(oc_achievements, oc_achievements_tab, config.hide_achievement_notices)) ui.mark_changed();
  }, 45);
  if (tables_ok) {
    std::vector<game::OcAchievements::ElementInfo> elements(tables.elements.size());
    for (size_t id = 0; id < tables.elements.size(); ++id) {
      const data::ElemRec& record = tables.elements[id];
      game::OcAchievements::ElementInfo& info = elements[id];
      info.level = record.level();
      info.colour = data::colour_index_of_bit(static_cast<u8>(record.colour_bit() & 0xFC));
      if (!record.empty() && record.b(0x14) != 0xFF)
        for (size_t k = 0; k < 3; ++k) info.casters += record.b(0x14 + k) != 0xFF ? 1 : 0;
    }
    oc_achievements.set_elements(std::move(elements));
    std::string techs;
    for (const int tech : oc_achievements.techs()) techs += " " + std::to_string(tech);
    log::info("achievements: double and triple techs{}", techs);
  }
  if (!own_ini) return;
  oc_achievements_file = profile_dir / "achievements.txt";
  std::error_code error;
  if (!std::filesystem::exists(oc_achievements_file, error)) return;
  std::ifstream in(oc_achievements_file, std::ios::binary);
  if (!in) {
    log::warn("achievements: cannot read {}; unlocks of this session are not written", oc_achievements_file.string());
    oc_achievements_file.clear();
    return;
  }
  std::stringstream text;
  text << in.rdbuf();
  in.close();
  if (std::optional<game::OcAchievements::Profile> profile = game::OcAchievements::parse_profile(text.str())) {
    oc_achievements.load(std::move(*profile));
    return;
  }
  const std::filesystem::path bad = profile_dir / "achievements.bad.txt";
  std::filesystem::remove(bad, error);
  std::filesystem::rename(oc_achievements_file, bad, error);
  log::warn("achievements: {} cannot be parsed; kept as {}, starting a new one", oc_achievements_file.string(), bad.string());
}

void App::save_oc_achievements() {
  if (oc_achievements_file.empty()) return;
  std::filesystem::path temp = oc_achievements_file;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << game::OcAchievements::serialize_profile(oc_achievements.profile());
    if (!out) {
      log::warn("achievements: cannot write {}", temp.string());
      return;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp, oc_achievements_file, error);
  if (error) log::warn("achievements: cannot replace {}: {}", oc_achievements_file.string(), error.message());
}

void App::update_oc_achievements(int ticks) {
  // Taken every frame, so nothing piles up while the set is off. A versus battle counts nothing on either side.
  const std::vector<std::vector<battle::ActionEvent>> actions = battle_ready ? battle.take_begun_actions() : std::vector<std::vector<battle::ActionEvent>>{};
  const int forged = shop_ok ? shop.take_forged_count() : 0;
  oc_achievements.set_enabled(config.achievements);
  oc_achievements.set_blocked(debug_tools.saving_disabled());
  if (!config.achievements || config.hide_achievement_notices) oc_popup.clear();
  if (!config.achievements) return;
  if (!battle.versus()) oc_achievements.battle_actions(actions);
  oc_achievements.forged(forged);
  if (world_ok && !(flow_ready && flow.scene_mode()))
    if (const int due = oc_achievements.tick(ticks, battle_in_field); due > 0) {
      const save::SaveSlot& slot = world.game_state().slot;
      game::OcAchievements::Sample sample;
      sample.room = world.vm().var(script::var::kMapId);
      sample.prev_room = world.vm().var(script::var::kPrevMapId);
      for (int i = 0; i < save::kNumChars; ++i) sample.recruited[static_cast<size_t>(i)] = slot.recruited(i);
      for (const int member : world.game_state().party())
        if (member >= 0 && member < save::kNumChars) sample.party_bodies.push_back(slot.chars[static_cast<size_t>(member)].model_id);
      if (slot.recruited(kPip)) sample.party_bodies.push_back(slot.chars[static_cast<size_t>(kPip)].model_id);
      sample.gold = slot.gold();
      sample.var = [this](int id) { return static_cast<int>(world.vm().var(static_cast<u16>(id))); };
      for (int i = 0; i < due; ++i) oc_achievements.sample(sample);
    }
  for (const size_t row : oc_achievements.take_unlocked()) {
    log::info("oc achievement earned: {}", oc_achievements.rows()[row].id);
    if (!config.hide_achievement_notices) oc_popup.push(oc_achievements.rows()[row].title);
  }
  if (oc_achievements.take_dirty()) save_oc_achievements();
}

void App::oc_achievements_overlay() { oc_popup.draw(frame_dt * 60.0, movie_open || ui.options_open); }

}  // namespace oc::app
