// The Esc overlay's "Debug" tab (F8): field / player / script VM status, the debug tools, the entity and variable tables; and the
// per-frame debug-tool requests (free camera, battle / room / party / cast / script actions).
#include "app/app.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <format>

#include <imgui.h>

#include "core/log.h"
#include "game/free_cam.h"
#include "script/field_script.h"

namespace oc::app {

namespace {

std::string op_name(u16 key) {
  if (key == 0) return "-";
  const script::OpInfo* info = script::find_op(key);
  return std::format("{:02X} {}", key & 0xFF, info ? info->name : "?");
}

// Top of the tab: two panes side by side (field | player + script VM).
void draw_debug_status(game::FieldWorld* world) {
  using render::ImGuiLayer;
  if (!ImGuiLayer::begin_panes("dbg_status")) return;
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  if (world) {
    ImGuiLayer::section("Field");
    ImGui::TextUnformatted(world->status().c_str());
    const game::RoomView& view = world->view();
    ImGui::Text("%zu layers, %zu groups, %zu textures, layer mask %08X", view.layer_count(), view.group_count(), view.draw_count(),
                view.layer_mask());
    ImGui::Text("scroll %d, %d  camera mode %s  control yaw %03X  fade %.2f", view.scroll_x(), view.scroll_y(),
                world->camera_mode() == 0 ? "auto" : world->camera_mode() == 1 ? "fixed" : "follow", world->control_yaw(),
                static_cast<double>(world->fade_level()));
  }
  ImGui::TableNextColumn();
  if (!world) {
    ImGuiLayer::section("Field");
    ImGui::TextDisabled("No field room is shown.");
  } else {
    ImGuiLayer::section("Player and script VM");
    const int player_index = world->player_entity();
    if (player_index >= 0 && static_cast<size_t>(player_index) < world->entity_count()) {
      const game::Ent& ent = world->ent(static_cast<size_t>(player_index));
      const game::ActorPose& pose = ent.actor.current();
      ImGui::Text("player ent %d  %.0f, %.0f, %.0f  tri %d  facing %03X  clip %s", player_index, static_cast<double>(pose.x),
                  static_cast<double>(pose.y), static_cast<double>(pose.z), ent.actor.triangle(), ent.actor.facing(),
                  ent.actor.clip_name().c_str());
    }
    const auto& vs = world->vm().stats();
    ImGui::Text("tick %llu  vm instructions %llu  unknown ops %zu", static_cast<unsigned long long>(world->tick_count()),
                static_cast<unsigned long long>(vs.instructions), vs.unknown_ops.size());
    if (world->dialogue_is_open()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "dialogue open");
    if (ImGui::TreeNode("Stubbed host calls")) {
      for (const auto& [name, hit_count] : world->stub_hits())
        ImGui::Text("%s  x%llu", name.c_str(), static_cast<unsigned long long>(hit_count));
      ImGui::TreePop();
    }
  }
  ImGui::EndTable();
}

// Full-width tables below the tool panes: entities, variables, the room-local variables.
void draw_debug_tab(game::FieldWorld* world) {
  using render::ImGuiLayer;
  if (!world) {
    ImGuiLayer::section("Entities");
    ImGui::TextDisabled("No field room is loaded: the entity / script table needs one.");
    return;
  }

  ImGuiLayer::section("Entities");
  if (ImGui::BeginTable("ents", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000").x);
    ImGui::TableSetupColumn("kind");
    ImGui::TableSetupColumn("x, y, z");
    ImGui::TableSetupColumn("face", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("face").x);
    ImGui::TableSetupColumn("f38 / f270");
    ImGui::TableSetupColumn("clip");
    ImGui::TableSetupColumn("last op");
    ImGui::TableSetupColumn("scripts");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < world->entity_count(); ++i) {
      const game::Ent& ent = world->ent(i);
      const script::Entity& vm_ent = world->vm().entity(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", i);
      ImGui::TableNextColumn();
      if (ent.pc_slot >= 0) ImGui::Text("PC%d c%d", ent.pc_slot, ent.char_id);
      else if (ent.model >= 0) ImGui::Text("NPC m%d", ent.model);
      else ImGui::TextDisabled("-");
      if (ent.motion.active) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 1, 0.5f, 1), "mv");
      }
      ImGui::TableNextColumn();
      const game::ActorPose& pose = ent.actor.current();
      ImGui::Text("%.0f, %.0f, %.0f", static_cast<double>(pose.x), static_cast<double>(pose.y), static_cast<double>(pose.z));
      ImGui::TableNextColumn();
      ImGui::Text("%03X", ent.actor.facing());
      ImGui::TableNextColumn();
      ImGui::Text("%06X / %X", vm_ent.flags38, vm_ent.flags270);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(ent.actor.clip_name().c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(op_name(vm_ent.last_op).c_str());
      ImGui::TableNextColumn();
      std::string scripts_text;
      for (const auto& rec : vm_ent.rec)
        if (rec.state != script::kRecIdle) scripts_text += std::format("{}:c{}@{:X} ", rec.id, rec.state, rec.pc);
      ImGui::TextUnformatted(scripts_text.c_str());
    }
    ImGui::EndTable();
  }

  ImGuiLayer::section("Variables");
  static const struct {
    u16 id;
    const char* name;
  } kNamed[] = {{0x00, "SCENARIO"}, {0x02, "ROOM_PARAM"}, {0x04, "CHOICE_RESULT"}, {0x06, "MAP_ID"}, {0x08, "PREV_MAP_ID"},
                {0x0A, "PLAYER_FACING"}, {0x0C, "PARTY_0"}, {0x0E, "PARTY_1"}, {0x10, "PARTY_2"}, {0x12, "PLAYER_X"},
                {0x14, "PLAYER_Z"}, {0x16, "PLAYER_Y"}, {0x18, "DIGIT_RESULT"}, {0x32, "BATTLE_RESULT"}, {0x44, "FRAME"}};
  if (ImGui::BeginTable("vars", 5, ImGuiTableFlags_SizingStretchSame)) {
    for (const auto& named : kNamed) {
      ImGui::TableNextColumn();
      ImGui::Text("%02X %-14s %d", named.id, named.name, world->vm().var(named.id));
    }
    ImGui::EndTable();
  }
  const bool room_open = ImGui::TreeNode("room-local 0x400..");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    render::ImGuiLayer::tooltip("Click to expand: the non-zero room-local script variables (0x400 and up, the current room's scratch values) as "
                      "id [index] value.");
  if (room_open) {
    for (u16 id = 0x400; id < 0x400 + 2 * 64; id += 2)
      if (world->vm().var(id) != 0) ImGui::Text("%03X [%u]  %d", id, (id - 0x400u) / 2, world->vm().var(id));
    ImGui::TreePop();
  }
}

}  // namespace

// What the tab's actions may touch right now (field_free = a field room with nothing else owning the screen).
game::DebugTools::Ctx App::debug_tools_ctx() {
  game::DebugTools::Ctx tools_ctx;
  tools_ctx.world = world_ok ? &world : nullptr;
  tools_ctx.battle = battle_ready ? &battle : nullptr;
  tools_ctx.tables = tables_ok ? &tables : nullptr;
  tools_ctx.battle_running = battle_in_field || battle_ok;
  tools_ctx.field_free = world_ok && !sweeping && !routing && !replaying && field_visible() && !movie_open && !battle_in_field && !battle_ok && !world.battle_active() &&
                 !world.menu_active() && !shop_open && !(menu_ok && !menu.closed()) && !(party_ok && party_screen.active()) &&
                 !(key_popup_ok && key_popup.active()) && !(pause_ok && pause_screen.active()) && !(flow_ready && (flow.owns_screen() || flow.scene_mode() || flow.fading_out()));
  return tools_ctx;
}

// [Debug] Panels=1 opens the tab at start.
void App::init_debug_tab() {
  ui.add_tab("Debug", [this] {
    if (!log_forced) {
      if (ImGui::Checkbox("Write log file", &config.debug_log)) {
        if (config.debug_log) log::init(log_path, true), log::info("log enabled");
        else log::info("log disabled"), log::close();
        ui.mark_changed();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        render::ImGuiLayer::tooltip("Writes opencross.log next to the exe (engine messages, warnings, errors) for bug reports. Off by default; "
                                    "takes effect at once and is remembered.");
    }
    // Sound test: plays one effect00 sound (the menus' and the field's global sound ids) to find what the original used.
    if (ImGui::CollapsingHeader("Sound test") && sfx_ok) {
      static int sound_id = 0;
      static bool auto_play = true;
      bool play = false;
      if (ImGui::Button("<")) sound_id = (sound_id + 255) % 256, play = auto_play;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120);
      if (ImGui::InputInt("##sound_id", &sound_id, 0, 0)) sound_id = std::clamp(sound_id, 0, 255), play = auto_play;
      ImGui::SameLine();
      if (ImGui::Button(">")) sound_id = (sound_id + 1) % 256, play = auto_play;
      ImGui::SameLine();
      if (ImGui::Button("Play")) play = true;
      ImGui::SameLine();
      ImGui::Checkbox("Play on change", &auto_play);
      audio::SfxProgram program;
      const bool defined = sfx_bank.make_global(sound_id, program);
      ImGui::Text("Sound %d (0x%02X)%s", sound_id, sound_id, defined ? "" : "  - not defined");
      if (play && defined) akao_sfx.play_menu(sound_id);
    }
    draw_debug_status(debug_field ? &world : nullptr);
    debug_tools.draw(debug_tools_ctx());
    draw_debug_tab(debug_field ? &world : nullptr);
  }, 90);
  if (config.debug_panels && !routing && !replaying) ui.open("Debug");
}

// Debug tab requests (battle / room / party / cast / script) run before the game advances.
void App::debug_tools_frame() {
  if (routing || replaying || sweeping) return;
  {  // Debug free camera (Debug tab): fly input; the keys / sticks it takes are removed from `input` before the game reads them
    const bool fc_battle = battle_ready && (battle_ok || battle_in_field) && battle.scene_visible();
    const bool fc_title = flow_ready && flow.scene_mode() && world_ok && field_visible() && !movie_open && world.room_name() == game::kTitleRoom && title_bd.running();
    debug_tools.poll_free_camera(input, fc_battle || fc_title, window.handle());
    // With the overlays hidden the game gets NO input at all - only Esc / F8 / F7 (read above) and the F-keys below still work; the
    // pad is blanked too.
    if (game::free_cam().hide_overlays() && (fc_battle || fc_title)) {
      platform::Input blank;
      blank.pad_connected = input.pad_connected;
      for (int sc = SDL_SCANCODE_F1; sc <= SDL_SCANCODE_F12; ++sc) {
        blank.key_down[static_cast<size_t>(sc)] = input.key_down[static_cast<size_t>(sc)];
        blank.key_pressed[static_cast<size_t>(sc)] = input.key_pressed[static_cast<size_t>(sc)];
      }
      input = blank;
    }
  }
  debug_tools.apply(debug_tools_ctx());
  if (menu_ok) menu.set_save_locked(debug_tools.saving_disabled());
}

}  // namespace oc::app
