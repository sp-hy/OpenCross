#include "game/cheats.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "render/imgui_layer.h"

namespace oc::game {

battle::Battle::Cheats battle_cheats() {
  const platform::Cheats& cheats = platform::cheats();
  battle::Battle::Cheats out;
  out.god_mode = cheats.god_mode;
  out.boost = cheats.battle_boost;
  out.max_stamina = cheats.max_stamina;
  out.free_elements = cheats.free_elements;
  out.always_hit = cheats.always_hit;
  out.always_loot = cheats.always_loot;
  out.instant_growth = cheats.instant_growth;
  out.shared_growth = cheats.shared_growth;
  out.summon_any_ring = cheats.summon_any_time;
  out.summon_repeat = cheats.more_summons;
  out.gold_percent = cheats.gold_percent;
  out.rare_loot = cheats.rare_loot;
  out.hp_percent = cheats.enemy_hp_percent;
  out.taken_percent = cheats.damage_taken_percent;
  out.dealt_percent = cheats.damage_dealt_percent;
  out.difficulty_bosses_only = cheats.difficulty_bosses_only;
  return out;
}

int story_byte_from_vars(std::span<const u16> vars, unsigned var) {
  if (var >= kStoryVarBytes) return -1;
  const size_t slot = var >> 1;
  if (slot >= vars.size()) return -1;
  return (vars[slot] >> ((var & 1) * 8)) & 0xFF;
}

namespace {
// Script variable byte ids the encounter rules test.
constexpr unsigned kVarStoryBattle01 = 0x1C0, kVarStoryA = 0xDC, kVarStoryB = 0xC4, kVarStoryBattle45 = 0x1C6, kVarModelSwapGate = 0x108;

bool bit(const StoryByte& story, unsigned var, int bit_index, bool unknown) {
  const int value = story ? story(var) : -1;
  return value < 0 ? unknown : ((value >> bit_index) & 1) != 0;
}
}  // namespace

// The A3 battles listed run when their map / entity / story condition holds, everything else is won. Every C9 battle runs except
// the four listed ids (and C9 has no map 0x213 test). An unknown story byte lets the story battle run.
bool encounter_auto_won(int map, int ent, int id, bool c9, const StoryByte& story) {
  id &= 0x7FF;
  if (c9) return id == 0x180 || id == 0x181 || id == 0x187 || id == 0x1B3;
  if (map == 0x213) return false;
  if (id == 0 || id >= 0x57) return true;
  auto at = [&](int at_map, int at_ent) { return map == at_map && ent == at_ent; };
  bool runs = false;
  switch (id) {
    case 0x01:
      if (bit(story, kVarStoryBattle01, 2, false)) return true;
      runs = at(0x32, 4) || at(0x34, 4) || at(0x36, 0x0A);
      break;
    case 0x03: runs = at(0x110, 0x16); break;
    case 0x0C: runs = at(0x96, 5) || map == 0x9E; break;
    case 0x15: runs = at(0x10B, 7) || at(0x107, 7) || at(0x108, 0x0E) || at(0x10A, 8) || at(0x10E, 4); break;
    case 0x18:
    case 0x1A: runs = at(0x122, 0x11); break;
    case 0x3B: runs = map == 0x1A0 && (ent == 8 || ent == 0x0A); break;
    case 0x3E: runs = at(0x126, 5); break;
    case 0x42:
    case 0x43:
    case 0x44: {
      const int a = story ? story(kVarStoryA) : -1, b = story ? story(kVarStoryB) : -1;
      if (a < 0 || b < 0) return false;
      runs = (a & 5) == 1 && (b <= 3 || (a & 0x38) != 0x38);
      break;
    }
    case 0x45: runs = bit(story, kVarStoryBattle45, 6, true); break;
    case 0x54:
    case 0x55: runs = at(0xF1, 7) || at(0xF9, 6); break;
    case 0x56: runs = at(0x10A, 7); break;
    default: return true;
  }
  return !runs;
}

// D9 is skipped on maps 0x99..0xAD (entity >= 4, with a particular story flag bit set) except at the listed entities.
bool model_swap_skipped(int map, int ent, const StoryByte& story) {
  if (ent < 4 || !bit(story, kVarModelSwapGate, 7, false)) return false;
  switch (map) {
    case 0x99: return ent != 0x0C;
    case 0x9D: return ent != 0x08;
    case 0x9E:
    case 0xAB: return true;
    case 0xA7: return ent != 0x05 && ent != 0x08;
    case 0xAA: return ent != 0x05 && ent != 0x0E && ent != 0x17;
    case 0xAD: return ent != 0x0F;
    default: return false;
  }
}

// ------------------------------------------------------------------------------------------------ hotkeys
void RdeHotkeys::poll(const HotkeyLevels& in, bool speed_locked, Toggles& toggles) {
  // Stick clicks act on release: both were held = the chord (and the later release of the other one does nothing), one alone = a tap.
  bool chord = false, tap_l3 = false, tap_r3 = false;
  if ((!in.l3 || !in.r3) && l3_ && r3_) {
    chord = true;
    chord_used_ = true;
  } else if (!in.l3 && !in.r3 && l3_ != r3_) {
    (l3_ ? tap_l3 : tap_r3) = !chord_used_;
    chord_used_ = false;
  }
  const bool slow = in.slow_key || in.lt, fast = in.fast_key || in.rt;
  if (slow && !slow_ && !speed_locked) {
    toggles.slow = !toggles.slow;
    toggles.fast = false;
  }
  if (fast && !fast_ && !speed_locked) {
    toggles.fast = !toggles.fast;
    toggles.slow = false;
  }
  const bool encounters = tap_l3 || in.encounters_key, boost = tap_r3 || in.boost_key, auto_battle = chord || in.auto_key;
  if (encounters && !encounters_) toggles.encounters = !toggles.encounters;
  if (boost && !boost_) toggles.boost = !toggles.boost;
  if (auto_battle && !auto_) toggles.auto_battle = !toggles.auto_battle;

  slow_ = slow, fast_ = fast, encounters_ = encounters, boost_ = boost, auto_ = auto_battle;
  l3_ = in.l3, r3_ = in.r3;
}

bool MenuChord::poll(bool lt, bool l3) {
  const bool pulse = lt && l3 && (!lt_ || !l3_);
  const bool toggle = pulse_ && !pulse;
  lt_ = lt, l3_ = l3, pulse_ = pulse;
  return toggle;
}

float speed_factor(bool fast, bool slow, int fast_rate, int slow_percent, bool in_battle, bool speed_locked) {
  if (speed_locked) return 1.0f;
  if (fast) return fast_rate <= 0 ? (in_battle ? 2.0f : 4.0f) : static_cast<float>(fast_rate);
  if (slow) return static_cast<float>(slow_percent) / 100.0f;
  return 1.0f;
}

ui::TextPace text_pace(bool fast, bool slow) {
  if (fast) return {2.25f, 1.0f / 8.0f};
  if (slow) return {0.5f, 2.0f};
  return {};
}

float SlowFieldCycle::apply(double dt, bool active, float slow) {
  if (!active) clock_ = 0.0;
  if (!active || dt <= 0.0) return slow;
  // The normal frames are [k * kCycle - kFrame, k * kCycle) for k >= 1.
  const double from = clock_, to = clock_ + dt;
  double normal = 0.0;
  for (double k = std::floor(from / kCycle); k * kCycle - kFrame < to; k += 1.0) {
    const double lo = std::max(from, k * kCycle - kFrame), hi = std::min(to, k * kCycle);
    if (k >= 1.0 && hi > lo) normal += hi - lo;
  }
  clock_ = std::fmod(to, kCycle);
  const double share = normal / dt;
  return static_cast<float>(slow * (1.0 - share) + share);
}

// ------------------------------------------------------------------------------------------------ tab
bool draw_cheats_tab(platform::Config& cfg) {
  using render::ImGuiLayer;
  platform::Cheats& cheats = cfg.cheats;
  bool changed = false;
  auto toggle = [&](const char* label, const char* id, bool& value, const char* help) {
    ImGuiLayer::row(label, help);
    if (ImGui::Checkbox(id, &value)) changed = true;
  };
  auto left = [&] {
    ImGuiLayer::section("Radical Dreamers Edition set");
    if (ImGuiLayer::begin_rows("cheats_rde")) {
      toggle("No encounters", "##c_noenc", cheats.no_encounters,
             "RDE's \"encounters off\" (F3, or click the left stick): ordinary field battles (A3 / C9 ops) are won without being fought and the script goes on as after a win. "
             "Story battles, forced battles and every battle RDE keeps (its Table A) still run. Every launch starts with it off.");
      toggle("Battle boost", "##c_boost", cheats.battle_boost,
             "RDE's battle boost (F4, or click the right stick): hits on the party do no damage (they read as a miss; heals still work), nobody is knocked out, "
             "negative status effects are cleared after every hit and the element gauge stays full. HP and stamina are not refilled. "
             "Every launch starts with it off.");
      toggle("Auto battle", "##c_auto", cheats.auto_battle,
             "RDE's auto-battle (F5, or click both sticks together): each ready party member attacks its remembered target (the first living enemy when "
             "it is gone) and the hit is picked by the Auto rule; it never defends, runs or uses elements. The results screen still waits for you.");
      ImGuiLayer::end_rows();
    }
    ImGuiLayer::section("Battle");
    if (ImGuiLayer::begin_rows("cheats_battle")) {
      toggle("God mode", "##c_god", cheats.god_mode, "Hits on the party do no damage (heals still work), a knocked-out member is revived at full HP and negative status effects are cleared. "
             "The Debug tab's Invincible party box still works.");
      toggle("Max stamina", "##c_stam", cheats.max_stamina, "The party's stamina never runs down in battle.");
      toggle("Free elements", "##c_free", cheats.free_elements, "Casting an element costs no gauge level, a cell can be cast again and consumables (stacks) are not used up.");
      toggle("Summon any time", "##c_sumring", cheats.summon_any_time,
             "A summon can be cast whatever colours the field effect shows; it no longer has to be all the summon's colour. A star is still needed and "
             "used up (Free elements keeps the star).");
      toggle("More than one summon", "##c_summore", cheats.more_summons,
             "A summon can be placed once per copy you own, and a cast uses it up for the battle. With this on, placing a summon in the element grid keeps its "
             "copy in the stock, so it can be placed again (other cells, other characters who can hold it), and in battle a summon can be cast again "
             "while stars are left.");
      toggle("100% hit", "##c_hit", cheats.always_hit, "Every weapon attack of the party hits.");
      toggle("Always loot", "##c_loot", cheats.always_loot, "Every enemy drops its rare drop (else the common one) and every steal lands on the rare item.");
      ImGuiLayer::row("Enemy colours",
                      "Shows each enemy's innate colour as an icon beside its name in the target window: never, only for enemies you have already "
                      "defeated (from the collection), or always. It does not need Element colour symbols.");
      {
        int mode = static_cast<int>(cheats.enemy_colours);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##c_enemycolours", &mode, "Off\0Defeated\0Always\0")) {
          cheats.enemy_colours = static_cast<platform::EnemyColours>(mode);
          changed = true;
        }
      }
      toggle("Max stat growth", "##c_growth", cheats.instant_growth, "Every won battle grants each member the full stat gain of the star, and raises any stat below what a typical character has from the star-ups so far up to it.");
      toggle("Growth for everyone", "##c_shared", cheats.shared_growth,
             "Every won battle also grows the stats of every recruited character outside the party, and of members who were knocked out, "
             "just as it grows the fighters (with Max stat growth on, one win gives everyone the full amount for the star). "
             "Knocked-out members also get their star-up bonus from a boss.");
      ImGuiLayer::end_rows();
    }
  };
  auto percent_slider = [&](const char* label, const char* id, int& value, int lo, int hi, const char* help) {
    ImGuiLayer::row(label, help);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt(id, &value, lo, hi, "%d%%")) changed = true;
  };
  auto boosts = [&] {
    ImGuiLayer::section("Boosts");
    if (ImGuiLayer::begin_rows("cheats_boosts")) {
      percent_slider("Gold from fights", "##c_gold", cheats.gold_percent, 0, 400,
                     "Multiplies the gold a won fight pays, after the Profiteer Purse bonus (100% = the game's own, 0% pays nothing); the total stays within the 9,999,999 G cap.");
      ImGuiLayer::row("Rare loot chance",
                      "Multiplies the chance of the rare kill drop and of the rare item when stealing: 1x is the game's own, 2x doubles it, and so on up to 100%. "
                      "The top step, Always, is the Always loot cheat: every enemy drops its rare drop and every steal lands on it.");
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::SliderInt("##c_rare", &cheats.rare_loot, 1, battle::kRareLootAlways, cheats.rare_loot >= battle::kRareLootAlways ? "Always" : "%dx")) changed = true;
      ImGuiLayer::end_rows();
    }
    ImGuiLayer::section("Difficulty");
    if (ImGuiLayer::begin_rows("cheats_difficulty")) {
      ImGuiLayer::row("Preset", "Sets the three values below in one click; each stays adjustable. Story 50% / 50% / 150%, Normal 100% / 100% / 100%, "
                                "Hard 150% / 150% / 100%, Very Hard 200% / 200% / 75% (enemy HP / damage taken / damage dealt).");
      const battle::Difficulty now{cheats.enemy_hp_percent, cheats.damage_taken_percent, cheats.damage_dealt_percent};
      constexpr std::array<std::pair<const char*, battle::DifficultyPreset>, 4> kPresets{{{"Story", battle::DifficultyPreset::Story},
                                                                                          {"Normal", battle::DifficultyPreset::Normal},
                                                                                          {"Hard", battle::DifficultyPreset::Hard},
                                                                                          {"Very Hard", battle::DifficultyPreset::VeryHard}}};
      for (size_t i = 0; i < kPresets.size(); ++i) {
        const battle::Difficulty preset = battle::difficulty_preset(kPresets[i].second);
        if (i > 0) ImGui::SameLine();
        const bool current = now == preset;
        if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kPresets[i].first)) {
          cheats.enemy_hp_percent = preset.hp, cheats.damage_taken_percent = preset.taken, cheats.damage_dealt_percent = preset.dealt;
          changed = true;
        }
        if (current) ImGui::PopStyleColor();
      }
      percent_slider("Enemy HP", "##c_ehp", cheats.enemy_hp_percent, battle::kDifficultyMin, battle::kDifficultyMax,
                     "Enemies start with this share of their HP (applied after the New Game+ doubling and the gender scaling).");
      percent_slider("Damage taken", "##c_taken", cheats.damage_taken_percent, battle::kDifficultyMin, battle::kDifficultyMax,
                     "Scales every hit on the party: weapon attacks, elements and techs. Heals are not scaled.");
      percent_slider("Damage dealt", "##c_dealt", cheats.damage_dealt_percent, battle::kDifficultyMin, battle::kDifficultyMax,
                     "Scales every hit the party does to enemies: weapon attacks, elements and techs. Heals are not scaled.");
      toggle("Bosses only", "##c_bossonly", cheats.difficulty_bosses_only, "The three difficulty values apply in boss fights only. Gold and loot boosts always apply.");
      ImGuiLayer::end_rows();
    }
  };
  auto right = [&] {
    boosts();
    ImGuiLayer::section("Game speed");
    if (ImGuiLayer::begin_rows("cheats_speed")) {
      ImGuiLayer::row("Fast mode", "RDE's high-speed mode: the field and battles run faster, message text shows faster. Music, movies, menus, shops and the play time "
                                   "keep their normal speed. F2 or the right trigger (RT) switches it on and off; it turns Slow mode off. "
                                   "Like RDE, every launch starts with it off.");
      if (ImGui::Checkbox("##fast", &cfg.fast_mode)) {
        if (cfg.fast_mode) cfg.slow_mode = false;
        changed = true;
      }
      ImGuiLayer::row("Fast speed", "How much faster the game runs in Fast mode. RDE = the Radical Dreamers Edition's own speeds: 4x in the field, 2x in battle.");
      ImGui::SetNextItemWidth(-FLT_MIN);
      {
        int rate = cfg.fast_rate <= 0 ? 1 : cfg.fast_rate;
        if (ImGui::SliderInt("##fastspeed", &rate, 1, 5, rate == 1 ? "RDE" : "%dx")) {
          cfg.fast_rate = rate == 1 ? 0 : rate;
          changed = true;
        }
      }
      ImGuiLayer::row("Slow mode", "RDE's slow mode: the field and battles run slower, message text shows slower. F1 or the left trigger (LT) switches it on and off; "
                                   "it turns Fast mode off. Like RDE, every launch starts with it off.");
      if (ImGui::Checkbox("##slow", &cfg.slow_mode)) {
        if (cfg.slow_mode) cfg.fast_mode = false;
        changed = true;
      }
      ImGuiLayer::row("Slow speed", "How fast the game runs in Slow mode, in percent of normal speed (RDE: 50%).");
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::SliderInt("##slowspeed", &cfg.slow_rate, 10, 90, "%d%%")) changed = true;
      ImGuiLayer::end_rows();
    }
    ImGuiLayer::section("Enemy HP");
    if (ImGuiLayer::begin_rows("cheats_enemyhp")) {
      ImGuiLayer::row("HP bars", "Shows enemy HP bars in battle. Off: none. Defeated: only for kinds of enemy you have already defeated (the Records collection). Always: every enemy.");
      ImGui::SetNextItemWidth(-FLT_MIN);
      {
        int mode = static_cast<int>(cheats.enemy_hp_mode);
        if (ImGui::Combo("##c_hpmode", &mode, "Off\0Defeated\0Always\0")) {
          cheats.enemy_hp_mode = static_cast<platform::EnemyHpMode>(mode);
          changed = true;
        }
      }
      toggle("Show numbers", "##c_hpnum", cheats.enemy_hp_numbers, "The bars come with the enemy's HP and maximum HP as numbers, in the target window even before an Infoscope has shown them.");
      ImGuiLayer::row("Where", "Target window: beside the enemy's name while you choose a target. Over enemies: a thin bar above each enemy for the whole battle. Both: the two together.");
      ImGui::SetNextItemWidth(-FLT_MIN);
      {
        int place = static_cast<int>(cheats.enemy_hp_place);
        if (ImGui::Combo("##c_hpplace", &place, "Target window\0Over enemies\0Both\0")) {
          cheats.enemy_hp_place = static_cast<platform::EnemyHpPlace>(place);
          changed = true;
        }
      }
      ImGuiLayer::end_rows();
    }
    ImGuiLayer::section("Field");
    if (ImGuiLayer::begin_rows("cheats_field")) {
      toggle("No clip", "##c_noclip", cheats.no_clip,
             "The leader walks through walls, the edge of the walk mesh and solid characters. Triggers, doors and room exits still fire where you stand; "
             "your height follows the floor only while you are above it.");
      toggle("Run speed x2", "##c_speed", cheats.run_speed, "Walking and running are twice as fast in the field.");
      ImGuiLayer::end_rows();
    }
    ImGuiLayer::section("Money and shops");
    if (ImGuiLayer::begin_rows("cheats_shops")) {
      toggle("Max money", "##c_money", cheats.max_money, "G is kept at the maximum (9,999,999).");
      toggle("Every element in shops", "##c_shop", cheats.shop_all_elements, "Element shops also sell every other element that can be bought, after their own stock.");
      toggle("Every weapon in shops", "##c_wshop", cheats.shop_all_weapons,
             "The blacksmith's forge lists every weapon (and armour / accessory) that has a recipe, after its own stock. Materials and gold are still needed unless No crafting cost is on.");
      toggle("No crafting cost", "##c_craft", cheats.no_craft_cost,
             "The forge (Zappa's smithy) builds and upgrades without using up materials or gold. Taking a weapon apart still returns its materials as usual.");
      ImGuiLayer::end_rows();
    }
  };
  if (ImGuiLayer::begin_panes("cheats_cols")) {
    ImGuiLayer::next_pane();
    left();
    ImGuiLayer::next_pane();
    right();
    ImGuiLayer::end_panes();
  }
  return changed;
}

}  // namespace oc::game
