#include "game/debug_tools.h"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <cstring>
#include <chrono>
#include <format>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "battle/battle.h"
#include "core/log.h"
#include "data/battlefields.h"
#include "data/tables.h"
#include "game/battle/battle_view.h"
#include "game/field_world.h"
#include "game/free_cam.h"
#include "render/imgui_layer.h"
#include "save/cc_save.h"
#include "script/field_script.h"

namespace oc::game {

namespace {

using render::ImGuiLayer;

bool contains_nocase(const std::string& hay, const char* needle) {
  if (!*needle) return true;
  const size_t needle_len = std::strlen(needle);
  if (needle_len > hay.size()) return false;
  for (size_t i = 0; i + needle_len <= hay.size(); ++i) {
    size_t k = 0;
    while (k < needle_len && std::tolower(static_cast<unsigned char>(hay[i + k])) == std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
    if (k == needle_len) return true;
  }
  return false;
}

// Hover tooltip for the item just drawn (also when it is disabled).
void tip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip(text);
}

// A searchable combo box: the popup starts with a filter field (typing narrows the list), then the matching rows. `p.sel` is the chosen
// value: items[i]'s value is values[i] (or i when `values` is null). Returns true when the choice changed.
bool combo(const char* id, int& sel, char* filter, size_t filter_size, const std::vector<std::string>& items, const std::vector<int>* values = nullptr) {
  bool changed = false;
  const char* preview = "-";
  for (size_t i = 0; i < items.size(); ++i)
    if ((values ? (*values)[i] : static_cast<int>(i)) == sel) preview = items[i].c_str();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo(id, preview)) {
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
      filter[0] = 0;
    }
    ImGui::InputTextWithHint("##filter", "type to search", filter, filter_size);
    ImGui::Separator();
    if (ImGui::BeginChild("##items", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), ImGuiChildFlags_None)) {
      for (size_t i = 0; i < items.size(); ++i) {
        if (!contains_nocase(items[i], filter)) continue;
        const int value = values ? (*values)[i] : static_cast<int>(i);
        ImGui::PushID(static_cast<int>(i));
        const bool selected = value == sel;
        if (ImGui::Selectable(items[i].c_str(), selected)) {
          sel = value;
          changed = true;
          ImGui::CloseCurrentPopup();
        }
        if (selected && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    ImGui::EndCombo();
  }
  return changed;
}

std::string enemy_text(const data::GameTables& tables, int id) {
  const std::string name = DebugTools::plain_name(tables.enemy_name(id));
  return std::format("{:3}  {}", id, name.empty() ? "(unnamed)" : name);
}

// The six element colours as drawn in the picker (data colour index: green white black red yellow blue).
ImU32 colour_u32(int idx) {
  switch (idx) {
    case 0: return IM_COL32(70, 200, 80, 255);
    case 1: return IM_COL32(245, 245, 245, 255);
    case 2: return IM_COL32(30, 30, 30, 255);
    case 3: return IM_COL32(225, 60, 55, 255);
    case 4: return IM_COL32(240, 210, 50, 255);
    case 5: return IM_COL32(70, 120, 235, 255);
    default: return 0;
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------------ helpers
// The names are UTF-8 from the game's text tables. Element names start with a prefix glyph (alpha item, beta technique / enemy skill, gamma
// enemy summon, a star for the party's summons) that the UI font (Latin-1 only) draws as '?': the picker shows a kind instead.
std::string DebugTools::plain_name(const std::string& raw_name) {
  std::string out;
  for (size_t i = 0; i < raw_name.size();) {
    const u8 lead = static_cast<u8>(raw_name[i]);
    const size_t len = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
    // Keep ASCII and U+0080..U+00FF.
    if (lead < 0x80 || (len == 2 && lead <= 0xC3)) out.append(raw_name, i, len);
    i += len;
  }
  const size_t first = out.find_first_not_of(' ');
  return first == std::string::npos ? std::string() : out.substr(first);
}

DebugTools::ElemKind DebugTools::element_kind(const std::string& raw, const data::ElemRec& rec) {
  if (raw.empty()) return ElemKind::Unnamed;
  const auto starts = [&](const char* prefix) { return raw.rfind(prefix, 0) == 0; };
  // alpha: consumable elements (Tablet, Medicine ...)
  if (starts("\xCE\xB1")) return ElemKind::Item;
  // beta: weapon techs (record byte 1 == 2) / enemy skills
  if (starts("\xCE\xB2")) return rec.b(1) == 2 ? ElemKind::Tech : ElemKind::EnemySkill;
  // gamma: the enemies' summon-class attacks
  if (starts("\xCE\xB3")) return ElemKind::EnemySummon;
  // star: the party's summons
  if (starts("\xE2\x98\x86")) return ElemKind::Summon;
  return ElemKind::Element;
}

const char* DebugTools::element_kind_name(ElemKind kind) {
  switch (kind) {
    case ElemKind::Element: return "Element";
    case ElemKind::Item: return "Item (consumable)";
    case ElemKind::Tech: return "Tech (weapon)";
    case ElemKind::EnemySkill: return "Enemy skill";
    case ElemKind::Summon: return "Summon";
    case ElemKind::EnemySummon: return "Enemy summon";
    case ElemKind::Unnamed: return "Unnamed";
  }
  return "";
}

std::string DebugTools::scene_label(const data::GameTables& tables, size_t scene_index) {
  const data::SceneRec& scene = tables.scenes[scene_index];
  std::string out = std::format("{:03}  ", scene_index);
  bool any = false;
  for (int group = 0; group < 3; ++group) {
    if (scene.enemy_id(group) == 0xFF || scene.enemy_count(group) == 0) continue;
    const std::string name = plain_name(tables.enemy_name(scene.enemy_id(group)));
    out += std::format("{}{} x{}", any ? ", " : "", name.empty() ? std::format("enemy {}", scene.enemy_id(group)) : name, scene.enemy_count(group));
    any = true;
  }
  if (!any) out += "(no enemies)";
  out += std::format("   bg_{:02}", scene.bg());
  if (scene.boss()) out += "  boss";
  return out;
}

std::array<u8, 28> DebugTools::with_enemies(const std::array<u8, 28>& base, const std::array<u8, 3>& ids, const std::array<u8, 3>& counts) {
  std::array<u8, 28> record = base;
  for (size_t group = 0; group < 3; ++group) {
    record[group] = counts[group] ? ids[group] : u8{0xFF};
    record[3 + group] = record[group] == 0xFF ? u8{0} : counts[group];
  }
  return record;
}

void DebugTools::queue(Action action) {
  // No "Disable saving?" question and no lock: the first use of any tool only marks the session as debugged; the Debug
  // tab and the Save menu show a warning ("Debug tools used this session") and saving stays allowed.
  // saving_disabled_ means "debug tools were used".
  saving_disabled_ = true;
  queue_.push_back(std::move(action));
}

void DebugTools::build_caches(const Ctx& ctx) {
  const void* key = ctx.tables;
  if (cache_key_ == key && !room_labels_.empty()) return;
  cache_key_ = key;
  scene_labels_.clear();
  elem_info_.clear();
  char_labels_.clear();
  enemy_labels_.clear();
  room_labels_.clear();
  room_stems_.clear();
  if (ctx.tables) {
    const data::GameTables& tables = *ctx.tables;
    for (size_t i = 0; i < tables.scenes.size(); ++i) scene_labels_.push_back(scene_label(tables, i));
    for (size_t i = 0; i < tables.elements.size(); ++i) {
      const std::string& raw = tables.element_name(static_cast<int>(i));
      const data::ElemRec& rec = tables.elements[i];
      ElemInfo info;
      info.kind = element_kind(raw, rec);
      info.colour = data::colour_index_of_bit(rec.colour_bit());
      const std::string name = plain_name(raw);
      info.label = std::format("{:3}  {}  [{}]  {}", i, name.empty() ? "(unnamed)" : name, info.colour < 0 ? "no colour" : data::colour_name(info.colour),
                               element_kind_name(info.kind));
      elem_info_.push_back(std::move(info));
    }
    for (int i = 0; i < 0x2C; ++i) {
      const std::string name = plain_name(tables.char_name(i));
      char_labels_.push_back(std::format("{:2}  {}", i, name.empty() ? "(unnamed)" : name));
    }
    for (size_t i = 0; i < tables.enemies.size(); ++i) enemy_labels_.push_back(enemy_text(tables, static_cast<int>(i)));
    // Battle stages: the data has no names; the rooms whose scripts fight on a stage (data/battlefields.h, `occ battlefields`) stand in for the location.
    field_labels_.clear();
    field_values_.clear();
    field_labels_.push_back("-1  the scene's own stage");
    field_values_.push_back(-1);
    std::vector<int> scenes_per_bg(100, 0);
    for (const data::SceneRec& scene : tables.scenes)
      if (scene.bg() < 100) ++scenes_per_bg[scene.bg()];
    for (int bg = 0; bg < 100; ++bg) {
      const std::string areas = data::battlefield_areas(bg);
      if (scenes_per_bg[static_cast<size_t>(bg)] == 0 && areas.empty()) continue;
      field_labels_.push_back(std::format("bg_{:02}  {} scenes  {}", bg, scenes_per_bg[static_cast<size_t>(bg)], areas.empty() ? "(no scripted battle)" : areas));
      field_values_.push_back(bg);
    }
  }
  if (ctx.world) {
    room_stems_ = ctx.world->room_list();
    std::vector<std::string> by_stem(room_stems_.size());
    for (int id = 0; id < 1024; ++id) {
      const std::string stem = ctx.world->room_stem_for_id(id);
      if (stem.empty()) continue;
      const auto it = std::find(room_stems_.begin(), room_stems_.end(), stem);
      if (it != room_stems_.end() && by_stem[static_cast<size_t>(it - room_stems_.begin())].empty())
        by_stem[static_cast<size_t>(it - room_stems_.begin())] = std::format("   (map {})", id);
    }
    for (size_t i = 0; i < room_stems_.size(); ++i) room_labels_.push_back(room_stems_[i] + by_stem[i]);
  }
  // The placeholder entry marks the caches as built.
  if (room_labels_.empty()) room_labels_.push_back("-");
}

// The element / summon / skill picker: a coloured square per entry (the element's colour), "[colour]  Kind" in the text, a kind filter next
// to the text filter. Returns true when the choice changed.
bool DebugTools::element_combo() {
  bool changed = false;
  const bool have = el_pick_.sel >= 0 && static_cast<size_t>(el_pick_.sel) < elem_info_.size();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##element", have ? elem_info_[static_cast<size_t>(el_pick_.sel)].label.c_str() : "-")) {
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
      el_pick_.filter[0] = 0;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::InputTextWithHint("##filter", "type to search (name, colour, kind)", el_pick_.filter, sizeof el_pick_.filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##kind", kind_filter_ < 0 ? "All kinds" : element_kind_name(static_cast<ElemKind>(kind_filter_)))) {
      if (ImGui::Selectable("All kinds", kind_filter_ < 0)) kind_filter_ = -1;
      for (int k = 0; k < static_cast<int>(ElemKind::Unnamed); ++k)
        if (ImGui::Selectable(element_kind_name(static_cast<ElemKind>(k)), kind_filter_ == k)) kind_filter_ = k;
      ImGui::EndCombo();
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##items", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), ImGuiChildFlags_None)) {
      const float line_height = ImGui::GetTextLineHeight();
      const float space = std::max(1.0f, ImGui::CalcTextSize(" ").x);
      const std::string pad(static_cast<size_t>(std::ceil((line_height + 6.0f) / space)), ' ');
      for (size_t i = 0; i < elem_info_.size(); ++i) {
        const ElemInfo& info = elem_info_[i];
        if (kind_filter_ >= 0 && static_cast<int>(info.kind) != kind_filter_) continue;
        if (!contains_nocase(info.label, el_pick_.filter)) continue;
        ImGui::PushID(static_cast<int>(i));
        const bool selected = static_cast<int>(i) == el_pick_.sel;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable((pad + info.label).c_str(), selected)) {
          el_pick_.sel = static_cast<int>(i);
          changed = true;
          ImGui::CloseCurrentPopup();
        }
        if (info.colour >= 0) {
          const float size = line_height * 0.75f;
          const ImVec2 corner(cursor.x + 2.0f, cursor.y + (line_height - size) * 0.5f + 1.0f);
          ImDrawList* dl = ImGui::GetWindowDrawList();
          dl->AddRectFilled(corner, ImVec2(corner.x + size, corner.y + size), colour_u32(info.colour), 2.0f);
          dl->AddRect(corner, ImVec2(corner.x + size, corner.y + size), IM_COL32(150, 150, 150, 255), 2.0f);
        }
        if (selected && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    ImGui::EndCombo();
  }
  return changed;
}

// ------------------------------------------------------------------------------------------------------------------- the tab
void DebugTools::draw(const Ctx& ctx) {
  build_caches(ctx);
  if (saving_disabled_) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), "Debug tools were used this session - the Save menu warns about it (saving stays allowed).");
  if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());

  // ---- free camera (battle 3D and the title's sea)
  ImGuiLayer::section("Free camera");
  if (ImGuiLayer::begin_rows("dbg_freecam")) {
    FreeCam& fc = free_cam();
    ImGuiLayer::row("Free camera", "Fly the 3D camera of a battle or of the title's sea");
    bool on = fc.on();
    if (ImGui::Checkbox("##freecam", &on)) {
      if (on) {
        Action action;
        action.kind = Action::Kind::FreeCam;
        action.arg0 = 1;
        // Switching on marks the session as debugged; the box changes when the request is applied.
        queue(std::move(action));
      } else {
        fc.set_on(false);
        status_ = "Free camera off.";
      }
    }
    tip("Replaces the 3D camera with one you fly, starting from the current shot's view. The battle keeps running (watch the spells); the shot "
        "timeline is ignored meanwhile. Works in a battle and on the title screen. Not saved: always off at start. W A S D move, Q / E down / up, "
        "mouse look while the left button is held (the pointer is trapped), Shift (or the left stick click) sprints at 4x; 4000 units per second; gamepad: left stick moves, "
        "right stick looks, triggers down / up. The view uses FovDeg.");
    ImGuiLayer::row("Hide overlays", "Battle: HUD, menus, windows, results, damage digits. Title: logo and menu");
    bool hide = fc.hide_setting();
    if (ImGui::Checkbox("##freecamhide", &hide)) fc.set_hide_overlays(hide);
    tip("While the free camera is on: hides the battle's 2D layer (party panels, command and element menus, target cursors, message windows, results, "
        "damage numbers) and the title's logo and menu. Screen fades stay.");
    ImGuiLayer::end_rows();
  }
  ImGui::BeginDisabled(!free_cam().on());
  if (ImGui::Button("Reset camera")) free_cam().reset();
  ImGui::EndDisabled();
  tip("Back to the current shot's view.");
  if (free_cam().on()) {
    ImGui::SameLine();
    ImGui::TextDisabled("W A S D move, Q / E down / up, hold the left mouse button + mouse to look, Shift sprints");
  }

  const bool have_tables = ctx.tables != nullptr && ctx.world != nullptr;
  const data::GameTables* tables = ctx.tables;

  if (!have_tables) {
    ImGui::TextDisabled("No game data / field world is loaded.");
  } else if (ImGuiLayer::begin_panes("dbg_tools")) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    // ---- teleport
    ImGuiLayer::section("Teleport to a room");
    if (ImGuiLayer::begin_rows("dbg_tp")) {
      ImGuiLayer::row("Room", "File stem and the script's map id");
      combo("##room", room_pick_.sel, room_pick_.filter, sizeof room_pick_.filter, room_labels_);
      ImGuiLayer::row("Entrance", "ROOM_PARAM: selects the entrance record");
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputInt("##ent", &entrance_);
      tip("Entrance number (0-255) used when entering the room: it picks the entrance record, i.e. where the party appears.");
      entrance_ = std::clamp(entrance_, 0, 255);
      ImGuiLayer::end_rows();
    }
    ImGui::BeginDisabled(!ctx.field_free || room_stems_.empty());
    if (ImGui::Button("Teleport")) {
      Action action;
      action.kind = Action::Kind::Room;
      action.room = room_stems_[static_cast<size_t>(std::clamp(room_pick_.sel, 0, static_cast<int>(room_stems_.size()) - 1))];
      action.arg0 = entrance_;
      queue(std::move(action));
    }
    ImGui::EndDisabled();
    tip("Switches the field to the room chosen above, entering at the entrance number. Needs a field room with nothing else on screen.");
    if (!ctx.field_free) {
      ImGui::SameLine();
      ImGui::TextDisabled("(needs a field room with nothing else on screen)");
    }

    // ---- battle
    ImGuiLayer::section("Start a battle");
    if (!scene_labels_.empty() && ImGuiLayer::begin_rows("dbg_btl")) {
      ImGuiLayer::row("Scene", "scenedat.bin formation: enemies, stage, boss flag");
      combo("##scene", scene_pick_.sel, scene_pick_.filter, sizeof scene_pick_.filter, scene_labels_);
      ImGuiLayer::row("Battlefield", "Stage bg_NN; -1 = the scene's own. The stages have no names: the label lists the places whose rooms fight on "
                                     "the stage (named after the world map's location markers) and how many scene records use it.");
      if (!field_labels_.empty()) {
        combo("##field", field_pick_.sel, field_pick_.filter, sizeof field_pick_.filter, field_labels_);
        field_ = field_values_[static_cast<size_t>(std::clamp(field_pick_.sel, 0, static_cast<int>(field_values_.size()) - 1))];
      }
      tip("Battle stage (bg_NN) with the places whose rooms fight on it and its number of scene records; -1 uses the scene's own stage. Type to filter.");
      ImGuiLayer::row("Replace the enemies", "Up to three enemy groups instead of the scene's own");
      ImGui::Checkbox("##custom", &custom_enemies_);
      tip("Replace the scene's enemies with up to three groups chosen below.");
      if (custom_enemies_) {
        // Fill the groups from the scene.
        if (custom_scene_ != scene_pick_.sel && static_cast<size_t>(scene_pick_.sel) < tables->scenes.size()) {
          custom_scene_ = scene_pick_.sel;
          const data::SceneRec& scene = tables->scenes[static_cast<size_t>(scene_pick_.sel)];
          for (int group = 0; group < 3; ++group) {
            const bool used = scene.enemy_id(group) != 0xFF && scene.enemy_count(group) != 0 && scene.enemy_id(group) < tables->enemies.size();
            enemy_pick_[group].sel = used ? scene.enemy_id(group) : 0;
            enemy_count_[group] = used ? scene.enemy_count(group) : 0;
          }
        }
        for (int group = 0; group < 3; ++group) {
          ImGuiLayer::row(std::format("  Group {}", group + 1).c_str(), "Enemy and how many (0 = unused)");
          ImGui::PushID(group);
          combo("##enemy", enemy_pick_[group].sel, enemy_pick_[group].filter, sizeof enemy_pick_[group].filter, enemy_labels_);
          ImGui::SetNextItemWidth(-FLT_MIN);
          ImGui::InputInt("##n", &enemy_count_[group]);
          tip("How many of this enemy (0 = group unused, at most 6).");
          enemy_count_[group] = std::clamp(enemy_count_[group], 0, 6);
          ImGui::PopID();
        }
      }
      ImGuiLayer::row("Use the party below", "Apply the party pickers first");
      ImGui::Checkbox("##useparty", &use_party_);
      tip("Sets the party from the Field party pickers before the battle starts.");
      ImGuiLayer::end_rows();
    }
    // Invincibility (any battle: the next one and a running one), part of the Start a battle section.
    {
      bool ip = inv_party_, ie = inv_enemies_;
      const bool cp = ImGui::Checkbox("Invincible party", &ip);
      tip("Your party's HP never drops below 1 in battle (damage, poison and instant KO included), in the next battle and a running one.");
      ImGui::SameLine();
      const bool ce = ImGui::Checkbox("Invincible enemies", &ie);
      tip("The enemies' HP never drops below 1, so they cannot be killed. A battle against enemies you cannot kill keeps going.");
      if (cp || ce) {
        Action action;
        action.kind = Action::Kind::Invincible;
        action.arg0 = ip ? 1 : 0;
        action.arg1 = ie ? 1 : 0;
        // The boxes change when the request is applied.
        queue(std::move(action));
      }
    }
    {
      int total = 0;
      for (int group = 0; group < 3; ++group) total += enemy_count_[group];
      const bool bad_custom = custom_enemies_ && total == 0;
      ImGui::BeginDisabled(!ctx.field_free || scene_labels_.empty() || bad_custom);
      if (ImGui::Button("Start battle")) {
        Action action;
        action.kind = Action::Kind::Battle;
        action.arg0 = scene_pick_.sel;
        action.arg1 = field_;
        action.custom_enemies = custom_enemies_;
        for (int group = 0; group < 3; ++group) {
          action.ids[static_cast<size_t>(group)] = static_cast<u8>(enemy_pick_[group].sel);
          action.counts[static_cast<size_t>(group)] = static_cast<u8>(enemy_count_[group]);
        }
        action.set_party_first = use_party_;
        for (int k = 0; k < 3; ++k) action.party[static_cast<size_t>(k)] = char_pick_[k].sel;
        queue(std::move(action));
      }
      ImGui::EndDisabled();
    tip("Starts a battle with the scene, battlefield and enemies chosen above. Needs a field room with nothing else on screen.");
      if (bad_custom) {
        ImGui::SameLine();
        ImGui::TextDisabled("(pick at least one enemy)");
      }
    }

    // Second pane: party, cast, script / variable.
    ImGui::TableNextColumn();
    // ---- party
    ImGuiLayer::section("Field party");
    if (!party_init_ && ctx.world) {
      party_init_ = true;
      const std::vector<int> cur = ctx.world->game_state().party();
      for (int k = 0; k < 3; ++k) char_pick_[k].sel = k < static_cast<int>(cur.size()) ? cur[static_cast<size_t>(k)] : -1;
    }
    {
      std::vector<std::string> labels{"(empty)"};
      std::vector<int> values{-1};
      for (size_t i = 0; i < char_labels_.size(); ++i) {
        labels.push_back(char_labels_[i]);
        values.push_back(static_cast<int>(i));
      }
      if (ImGuiLayer::begin_rows("dbg_party")) {
        for (int k = 0; k < 3; ++k) {
          ImGuiLayer::row(std::format("Member {}", k + 1).c_str(), "Who stands in this party slot; (empty) leaves it vacant.");
          ImGui::PushID(k);
          combo("##char", char_pick_[k].sel, char_pick_[k].filter, sizeof char_pick_[k].filter, labels, &values);
          ImGui::PopID();
        }
        ImGuiLayer::end_rows();
      }
    }
    ImGui::BeginDisabled(!ctx.field_free);
    if (ImGui::Button("Apply to the field party")) {
      Action action;
      action.kind = Action::Kind::Party;
      for (int k = 0; k < 3; ++k) action.party[static_cast<size_t>(k)] = char_pick_[k].sel;
      queue(std::move(action));
    }
    ImGui::EndDisabled();
    tip("Sets the party to the three members chosen above (the field party; also used for battles when 'Use the party below' is on).");
    ImGui::SameLine();
    ImGui::TextDisabled("Also the battle party when 'Use the party below' is on.");

    // ---- cast
    ImGuiLayer::section("Cast in a battle");
    const battle::Battle* running_battle = ctx.battle ? ctx.battle->battle() : nullptr;
    if (!ctx.battle_running || !running_battle) {
      ImGui::TextDisabled("Start a battle first (above), then cast an element / summon / skill here.");
    } else {
      std::vector<std::string> ulabels;
      std::vector<int> uvalues;
      for (int i = 0; i < battle::kUnits; ++i) {
        const battle::Unit& unit = running_battle->unit(i);
        if (!unit.present) continue;
        ulabels.push_back(std::format("{}  {}{}", i < battle::kPcSlots ? "PC" : "Enemy", unit.name.empty() ? std::format("unit {}", i) : unit.name,
                                      unit.alive() ? "" : " (KO)"));
        uvalues.push_back(i);
      }
      if (ImGuiLayer::begin_rows("dbg_cast")) {
        ImGuiLayer::row("Caster", "The battle unit that uses the element.");
        combo("##caster", caster_pick_.sel, caster_pick_.filter, sizeof caster_pick_.filter, ulabels, &uvalues);
        ImGuiLayer::row("Target", "An all-targets element hits the whole side of this unit");
        combo("##target", target_pick_.sel, target_pick_.filter, sizeof target_pick_.filter, ulabels, &uvalues);
        ImGuiLayer::row("Element / summon / skill", "pgdat element id (enemy skills included)");
        element_combo();
        ImGuiLayer::end_rows();
      }
      if (static_cast<size_t>(el_pick_.sel) < tables->elements.size()) {
        const battle::TargetSpec ts = battle::target_spec(tables->elements[static_cast<size_t>(el_pick_.sel)]);
        ImGui::TextDisabled("Aimed at: %s%s%s", ts.party && !ts.enemies ? "the party" : ts.party ? "anyone" : "enemies", ts.all ? ", whole side" : "",
                            ts.ko_only ? ", knocked-out only" : "");
      }
      ImGui::BeginDisabled(el_pick_.sel <= 0);
      if (ImGui::Button("Cast")) {
        Action action;
        action.kind = Action::Kind::Cast;
        action.arg0 = caster_pick_.sel;
        action.arg1 = el_pick_.sel;
        action.arg2 = target_pick_.sel;
        queue(std::move(action));
      }
      ImGui::EndDisabled();
    tip("Makes the caster use the chosen element / summon / skill on the target, now, in the running battle.");
    }

    // ---- script
    ImGuiLayer::section("Run a script / set a story variable");
    {
      const FieldWorld& world = *ctx.world;
      std::vector<std::string> elabels;
      for (size_t i = 0; i < world.entity_count(); ++i) {
        const Ent& entity = world.ent(i);
        elabels.push_back(entity.pc_slot >= 0 ? std::format("{:2}  party member {} (char {})", i, entity.pc_slot, entity.char_id)
                          : entity.model >= 0 ? std::format("{:2}  NPC / object, model {}", i, entity.model)
                                         : std::format("{:2}  entity", i));
      }
      static const char* const kSlotNames[] = {"INIT", "UPDATE", "ON_ACTION (talk)", "ON_COLLISION", "ON_TOUCH"};
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("Run script: starts one entity script of this room, like the room does itself. Set variable: writes a number into a story / room variable.");
      ImGui::PopStyleColor();
      if (ImGuiLayer::begin_rows("dbg_script")) {
        ImGuiLayer::row("Entity", "The room entity (party member, NPC or object) whose script is started. The number is its index in the entity table on this tab.");
        combo("##ent", ent_pick_.sel, ent_pick_.filter, sizeof ent_pick_.filter, elabels);
        ImGuiLayer::row("Script slot", "Which of the entity's scripts to start: INIT, UPDATE, ON_ACTION (talk), ON_COLLISION or ON_TOUCH. Only slots the entity has are listed.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const int ent = std::clamp(ent_pick_.sel, 0, std::max(0, static_cast<int>(world.entity_count()) - 1));
        const auto slot_text = [&](int slot) {
          return std::format("{}  {}{}", slot, slot < 5 ? kSlotNames[slot] : "script", world.vm().module().script_ptr(static_cast<size_t>(ent), static_cast<size_t>(slot)) ? "" : "  (none)");
        };
        if (ImGui::BeginCombo("##slot", slot_text(slot_).c_str())) {
          for (int slot = 0; slot < script::kScriptSlots; ++slot) {
            if (!world.vm().module().script_ptr(static_cast<size_t>(ent), static_cast<size_t>(slot))) continue;
            if (ImGui::Selectable(slot_text(slot).c_str(), slot == slot_)) slot_ = slot;
          }
          ImGui::EndCombo();
        }
        ImGuiLayer::end_rows();
      }
      ImGui::BeginDisabled(!ctx.field_free || world.entity_count() == 0);
      if (ImGui::Button("Run script")) {
        Action action;
        action.kind = Action::Kind::Script;
        action.arg0 = std::clamp(ent_pick_.sel, 0, std::max(0, static_cast<int>(world.entity_count()) - 1));
        action.arg1 = slot_;
        queue(std::move(action));
      }
      ImGui::EndDisabled();
    tip("Starts the chosen script slot of the chosen entity in the current room, as if the room had triggered it (INIT, UPDATE, talk, collision, touch).");
      if (ImGuiLayer::begin_rows("dbg_var")) {
        ImGuiLayer::row("Variable id (hex)", "Which variable to write, in hex (0 = SCENARIO, the story counter; 0x400 and up are the room-local ones). The Variables list below names the common ones. The - and + buttons step by 2.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##vid", &var_id_, 2, 16, ImGuiInputTextFlags_CharsHexadecimal);
        var_id_ = std::clamp(var_id_ & 0x7FE, 0, 0x7FE);
        ImGuiLayer::row("Value", "The number written into the variable when you press Set variable.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputInt("##vval", &var_val_);
        ImGuiLayer::end_rows();
      }
      ImGui::BeginDisabled(!ctx.field_free);
      if (ImGui::Button("Set variable")) {
        Action action;
        action.kind = Action::Kind::SetVar;
        action.arg0 = var_id_;
        action.arg1 = var_val_;
        queue(std::move(action));
      }
      ImGui::EndDisabled();
    tip("Writes the value into the story / room variable with that id (see the Variables list below the tools). Takes effect at once.");
    }
    ImGui::EndTable();
  }

}

// ------------------------------------------------------------------------------------------------------------------ the host
void DebugTools::poll_free_camera(platform::Input& in, bool applies, SDL_Window* window) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  const Clock::time_point now = Clock::now();
  const float raw_dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
  last = now;
  // The loop's frame times jitter (VSync / the frame limiter: e.g. alternating 8 / 25 ms iterations while the display shows frames evenly), and motion
  // scaled by the raw dt then stutters although the frame rate is steady. Integrate with a smoothed dt (EMA
  // over ~6 frames; a gap > 3x the mean, i.e. a hitch or a pause, resets it) - the pose only drifts from real time by the smoothing lag, never visibly.
  static float dt_avg = 1.0f / 60.0f;
  if (raw_dt > dt_avg * 3.0f || raw_dt <= 0.0f) dt_avg = std::clamp(raw_dt, 1.0f / 480.0f, 1.0f / 20.0f);
  else dt_avg += (raw_dt - dt_avg) * (1.0f / 6.0f);
  const float dt = dt_avg;
  static const bool trace = std::getenv("OC_FREECAM_TRACE") != nullptr;
  if (trace && free_cam().on()) log::info("freecam: raw dt {:.2f} ms  used {:.2f} ms", raw_dt * 1000.0f, dt * 1000.0f);
  float mx = 0, my = 0;
  // Always read: the delta since the last call must not pile up.
  const SDL_MouseButtonFlags mb = SDL_GetRelativeMouseState(&mx, &my);
  FreeCam& fc = free_cam();
  const ImGuiIO& io = ImGui::GetIO();
  // Look: hold the left button (the right one too) with the press outside every UI window: the pointer is trapped (relative mode, hidden) and
  // the frame's raw relative motion turns the camera; the release gives the cursor back where it was. Per rendered frame, no interpolation.
  const bool held = (mb & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) != 0;
  const bool want_look = fc.on() && applies && held && (fc_looking_ || !io.WantCaptureMouse);
  if (want_look != fc_looking_) {
    if (window) SDL_SetWindowRelativeMouseMode(window, want_look);
    fc_looking_ = want_look;
    // The first motion after the mode switch is a jump.
    mx = my = 0;
  }
  // Mouse look not yet applied (radians; paid out below).
  static float look_bank[2] = {0, 0};
  if (!fc.on()) {
    look_bank[0] = look_bank[1] = 0;
    return;
  }
  // No battle 3D / title sea this frame: the next scene seeds a fresh pose.
  if (!applies) {
    fc.reset();
    return;
  }
  // Held keys come from SDL's own keyboard state, NOT from `in.key_down`: that array is event-maintained (set on KEY_DOWN, cleared on KEY_UP) and the
  // masking below clears it for the game, so a key read back from it would be "held" only on the frames an event arrives - one frame at the press, then
  // nothing until the typematic delay (~0.5 s), then the repeat rate.
  const bool* ks = SDL_GetKeyboardState(nullptr);
  auto key = [&](SDL_Scancode sc) { return ks[static_cast<size_t>(sc)]; };
  float fwd = 0, strafe = 0, up = 0;
  if (!io.WantTextInput) {
    constexpr SDL_Scancode taken[] = {SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_Q, SDL_SCANCODE_E};
    fwd += (key(SDL_SCANCODE_W) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_S) ? 1.0f : 0.0f);
    strafe += (key(SDL_SCANCODE_D) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_A) ? 1.0f : 0.0f);
    up += (key(SDL_SCANCODE_E) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_Q) ? 1.0f : 0.0f);
    // The camera takes these keys: the battle / title does not see them.
    for (SDL_Scancode sc : taken) {
      in.key_down[static_cast<size_t>(sc)] = false;
      in.key_pressed[static_cast<size_t>(sc)] = false;
    }
  }
  bool sprint = (key(SDL_SCANCODE_LSHIFT) || key(SDL_SCANCODE_RSHIFT)) && !io.WantTextInput;
  // The relative mouse delta is whatever arrived during the loop's (jittery) iteration, quantised by the mouse's report rate (a 125 Hz mouse gives 0, 1 or 2
  // reports per 8 ms frame), so per-frame rotation is lumpy while the keyboard's dt-scaled motion is even.
  // The delta is banked and paid out exponentially (time constant ~25 ms of the smoothed dt): total rotation is exact, per-frame rotation even.
  if (fc_looking_) {
    look_bank[0] += mx * 0.0035f;
    look_bank[1] -= my * 0.0035f;
  }
  const float pay = 1.0f - std::exp(-dt / 0.025f);
  const float dyaw = look_bank[0] * pay, dpitch = look_bank[1] * pay;
  look_bank[0] -= dyaw;
  look_bank[1] -= dpitch;
  fc.look(dyaw, dpitch);
  if (in.pad_connected) {
    auto dz = [](float value) { return std::fabs(value) < 0.18f ? 0.0f : value; };
    const float lx = dz(in.pad_axis[SDL_GAMEPAD_AXIS_LEFTX]), ly = dz(in.pad_axis[SDL_GAMEPAD_AXIS_LEFTY]);
    const float rx = dz(in.pad_axis[SDL_GAMEPAD_AXIS_RIGHTX]), ry = dz(in.pad_axis[SDL_GAMEPAD_AXIS_RIGHTY]);
    const float lt = in.pad_axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER], rt = in.pad_axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
    sprint = sprint || in.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK];
    in.pad_down[SDL_GAMEPAD_BUTTON_LEFT_STICK] = in.pad_pressed[SDL_GAMEPAD_BUTTON_LEFT_STICK] = false;
    strafe += lx;
    fwd -= ly;
    up += (rt > 0.1f ? rt : 0.0f) - (lt > 0.1f ? lt : 0.0f);
    fc.look(rx * 2.2f * dt, -ry * 1.6f * dt);
    for (int ax : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                   SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
      in.pad_axis[static_cast<size_t>(ax)] = 0.0f;
  }
  // The pose is seeded by the camera code on its next query; moving before that would be lost.
  if (fc.needs_init()) return;
  fc.move(std::clamp(fwd, -1.0f, 1.0f), std::clamp(strafe, -1.0f, 1.0f), std::clamp(up, -1.0f, 1.0f), dt, sprint);
}

void DebugTools::apply(const Ctx& ctx) {
  if (restore_scene_ >= 0 && ctx.tables && ctx.world && (ctx.battle_running || !ctx.world->battle_active())) {
    // The battle has loaded its units: put the record back.
    ctx.tables->scenes[static_cast<size_t>(restore_scene_)].raw = restore_raw_;
    restore_scene_ = -1;
    // The battle has started (or was dropped): the next one uses its script again.
    if (ctx.battle) {
      BattleView::Debug debug = ctx.battle->debug();
      debug.no_script = false;
      ctx.battle->set_debug(debug);
    }
  }
  // The two checkboxes reach the view's battle (also the next ones).
  if (ctx.battle) {
    BattleView::Debug debug = ctx.battle->debug();
    if (debug.inv_party != inv_party_ || debug.inv_enemies != inv_enemies_) {
      debug.inv_party = inv_party_;
      debug.inv_enemies = inv_enemies_;
      ctx.battle->set_debug(debug);
    }
  }
  if (queue_.empty()) return;
  std::vector<Action> todo;
  todo.swap(queue_);
  for (Action& action : todo) {
    // Needs neither the field nor the tables.
    if (action.kind == Action::Kind::FreeCam) {
      free_cam().set_on(action.arg0 != 0);
      status_ = action.arg0 ? "Free camera on (a battle's 3D picture or the title's sea)." : "Free camera off.";
      continue;
    }
    FieldWorld* world = ctx.world;
    if (!world || !ctx.tables) continue;
    switch (action.kind) {
      case Action::Kind::Room:
        if (!ctx.field_free) status_ = "Teleport refused: the field is busy.";
        else if (world->load_room(action.room, action.arg0)) status_ = std::format("Teleported to {} (entrance {}).", action.room, action.arg0);
        else status_ = std::format("Could not load {}.", action.room);
        break;
      case Action::Kind::Party:
      case Action::Kind::Battle: {
        if (action.kind == Action::Kind::Battle && (!ctx.field_free || world->battle_active() || restore_scene_ >= 0)) {
          status_ = "Battle refused: the field is busy or a battle is starting.";
          break;
        }
        if (action.kind == Action::Kind::Party || action.set_party_first) {
          std::array<u8, 3> ids{0xFF, 0xFF, 0xFF};
          size_t count = 0;
          for (int char_id : action.party)
            if (char_id >= 0 && char_id < 0x2C && std::find(ids.begin(), ids.begin() + static_cast<long>(count), static_cast<u8>(char_id)) == ids.begin() + static_cast<long>(count))
              ids[count++] = static_cast<u8>(char_id);
          if (count == 0) {
            status_ = "Party refused: pick at least one member.";
            if (action.kind == Action::Kind::Party) break;
          } else {
            for (size_t k = 0; k < count; ++k) world->game_state().slot.set_recruited(ids[k], true);
            world->game_state().slot.set_party(ids);
            world->state_edited();
            status_ = std::format("Field party set ({} member{}).", count, count == 1 ? "" : "s");
          }
          if (action.kind == Action::Kind::Party) break;
        }
        const int scene = action.arg0;
        if (scene < 0 || static_cast<size_t>(scene) >= ctx.tables->scenes.size()) {
          status_ = "Battle refused: no such scene.";
          break;
        }
        if (action.custom_enemies) {
          restore_scene_ = scene;
          restore_raw_ = ctx.tables->scenes[static_cast<size_t>(scene)].raw;
          ctx.tables->scenes[static_cast<size_t>(scene)].raw = with_enemies(restore_raw_, action.ids, action.counts);
          // The scene's battle script addresses its own enemies (units, kill scripts, end-of-battle control): not these.
          if (ctx.battle) {
            BattleView::Debug debug = ctx.battle->debug();
            debug.no_script = true;
            ctx.battle->set_debug(debug);
          }
        }
        // Like A3 / C9 with the scene id: the field plays 'enct', then the battle starts.
        world->request_battle(0x200 + scene, action.arg1, 0);
        status_ = std::format("Battle started: scene {}, battlefield {}{}.", scene, action.arg1 < 0 ? std::string("default") : std::format("bg_{:02}", action.arg1),
                              action.custom_enemies ? ", custom enemies" : "");
        break;
      }
      case Action::Kind::Cast: {
        const battle::Battle* running_battle = ctx.battle ? ctx.battle->battle() : nullptr;
        if (!ctx.battle_running || !running_battle || action.arg0 < 0 || action.arg0 >= battle::kUnits || action.arg2 < 0 || action.arg2 >= battle::kUnits || !running_battle->unit(action.arg0).present ||
            !running_battle->unit(action.arg2).present || action.arg1 <= 0) {
          status_ = "Cast refused: no battle, or the caster / target is gone.";
          break;
        }
        BattleView::Debug debug = ctx.battle->debug();
        debug.cast_element = action.arg1;
        debug.cast_actor = action.arg0;
        debug.cast_target = action.arg2;
        // Runs as soon as the view is idle (not during the intro or another action).
        ctx.battle->set_debug(debug);
        status_ = std::format("Cast queued: element {} ({}) from unit {} on unit {}.", action.arg1, plain_name(ctx.tables->element_name(action.arg1)), action.arg0, action.arg2);
        break;
      }
      case Action::Kind::Invincible:
        inv_party_ = action.arg0 != 0;
        inv_enemies_ = action.arg1 != 0;
        status_ = std::format("Invincible party {}, invincible enemies {}.", inv_party_ ? "on" : "off", inv_enemies_ ? "on" : "off");
        break;
      case Action::Kind::Script:
        if (!ctx.field_free || action.arg0 < 0 || static_cast<size_t>(action.arg0) >= world->entity_count()) {
          status_ = "Script refused: the field is busy.";
          break;
        }
        {
          // The priority class the room loop starts that slot with.
          const int cls = action.arg1 == 2 ? 3 : action.arg1 == 3 ? 4 : action.arg1 == 4 ? 2 : 7;
          status_ = world->vm().start_script(action.arg0, action.arg1, cls) ? std::format("Started entity {} script {}.", action.arg0, action.arg1)
                                                                            : std::format("Entity {} script {} did not start (empty, already running, no free record).", action.arg0, action.arg1);
        }
        break;
      case Action::Kind::SetVar:
        if (!ctx.field_free) {
          status_ = "Variable refused: the field is busy.";
          break;
        }
        world->vm().set_var(static_cast<u16>(action.arg0), action.arg1);
        status_ = std::format("Variable {:03X} = {}.", action.arg0, action.arg1);
        break;
    }
    log::info("debug tools: {}", status_);
  }
}

}  // namespace oc::game
