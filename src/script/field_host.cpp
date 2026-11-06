#include "script/field_host.h"

#include <array>

#include "core/log.h"

namespace oc::script {

const char* global_name(Global which) {
  static constexpr std::array<const char*, static_cast<size_t>(Global::Count)> kNames = {
      "WindowFrames", "ControlYaw", "CameraYOffset", "SubmergedOtPush", "RegisterInvA6", "PushStrength",
      "ExitTransition", "FrameVsyncs", "OverlayGridA0", "OverlayGridA1", "OverlayGridA2", "OverlayGridB0",
      "OverlayGridB1", "OverlayGridB2", "OverlayOt", "KeepSfxSlots", "FootstepVolume", "FootstepDryType",
      "FootstepWaterType", "FootstepListenerX", "FootstepListenerY", "FootstepListenerZ", "FootstepNear", "FootstepFar",
      "Byte4D", "MenuPreamble3", "MusicSwapArmed", "TradingPostRow", "TradingPostCue", "Reg6A",
      "BattleIntroAnchor", "SavePointPreamble", "Selection", "SelectionFlag", "PadMask",
  };
  const auto index = static_cast<size_t>(which);
  return index < kNames.size() ? kNames[index] : "?";
}

void NullHost::note(const char* method) {
  ++calls_;
  if (called_.insert(method).second && verbose_) log::debug("NullHost: first call of {}", method);
}

#define NOTE(name) note(name)

bool NullHost::system_ready() { NOTE("system_ready"); return true; }
bool NullHost::entry_window() { NOTE("entry_window"); return false; }
bool NullHost::window_open() { NOTE("window_open"); return false; }
void NullHost::end_of_tick(FieldVM&) { NOTE("end_of_tick"); }
int NullHost::party_entity(int) { NOTE("party_entity"); return -1; }
int NullHost::character_entity(int) { NOTE("character_entity"); return -1; }
int NullHost::party_slot_of_entity(int) { NOTE("party_slot_of_entity"); return -1; }
void NullHost::setup_pc(int, int, int) { NOTE("setup_pc"); }
void NullHost::place_on_controlled(int) { NOTE("place_on_controlled"); }
void NullHost::place_followers() { NOTE("place_followers"); }
void NullHost::setup_npc(int, int) { NOTE("setup_npc"); }
int NullHost::load_model(int, int) { NOTE("load_model"); return 0; }
bool NullHost::monster_defeated(int) { NOTE("monster_defeated"); return false; }
void NullHost::set_monster_defeated(int) { NOTE("set_monster_defeated"); }
void NullHost::update_entity(int) { NOTE("update_entity"); }
void NullHost::set_position_xz(int, s32, s32, int) { NOTE("set_position_xz"); }
void NullHost::set_position_xyz(int, s32, s32, s32) { NOTE("set_position_xyz"); }
Vec3 NullHost::get_position(int) { NOTE("get_position"); return {}; }
void NullHost::set_facing(int, int) { NOTE("set_facing"); }
int NullHost::get_facing(int) { NOTE("get_facing"); return 0; }
void NullHost::turn_to_entity(int, int) { NOTE("turn_to_entity"); }
bool NullHost::rotation_busy(int) { NOTE("rotation_busy"); return false; }
void NullHost::face_point(int, s32, s32) { NOTE("face_point"); }
void NullHost::set_animation(int, int) { NOTE("set_animation"); }
void NullHost::play_animation(int, int) { NOTE("play_animation"); }
void NullHost::set_walk_animation(int, int, int) { NOTE("set_walk_animation"); }
bool NullHost::animation_busy(int) { NOTE("animation_busy"); return false; }
void NullHost::set_move_speed(int, int) { NOTE("set_move_speed"); }
void NullHost::start_motion(int, const MotionRequest&) { NOTE("start_motion"); }
bool NullHost::motion_busy(int) { NOTE("motion_busy"); return false; }
bool NullHost::near_and_facing(int, int, int, int) { NOTE("near_and_facing"); return false; }
bool NullHost::region_test(int, int, bool) { NOTE("region_test"); return false; }
int NullHost::entity_state(int) { NOTE("entity_state"); return 0; }
void NullHost::head_op(HeadOp, int, int, s32, s32, s32, int) { NOTE("head_op"); }
bool NullHost::attach_point(int, int, Vec3&) { NOTE("attach_point"); return false; }
void NullHost::entity_effect(u16, int, std::span<const s32>) { NOTE("entity_effect"); }
void NullHost::attach_entity(int, int, int) { NOTE("attach_entity"); }
void NullHost::extra_animation(u16, bool, int, int) { NOTE("extra_animation"); }
bool NullHost::extra_animation_busy() { NOTE("extra_animation_busy"); return false; }
void NullHost::camera_set_position(s32, s32, int) { NOTE("camera_set_position"); }
void NullHost::camera_to_actor(int) { NOTE("camera_to_actor"); }
void NullHost::camera_follow(int, int) { NOTE("camera_follow"); }
bool NullHost::camera_wait(CameraWait, s32, s32) { NOTE("camera_wait"); return true; }
void NullHost::camera_limits(s32, s32, s32, s32) { NOTE("camera_limits"); }
void NullHost::camera_auto_follow(bool) { NOTE("camera_auto_follow"); }
std::pair<s32, s32> NullHost::camera_position() { NOTE("camera_position"); return {0, 0}; }
void NullHost::set_camera_origin(s32, s32) { NOTE("set_camera_origin"); }
void NullHost::set_camera_target(int) { NOTE("set_camera_target"); }
void NullHost::set_controlled_entity(int) { NOTE("set_controlled_entity"); }
void NullHost::shake(int, int, int) { NOTE("shake"); }
void NullHost::formation(u16, std::span<const s32>) { NOTE("formation"); }
bool NullHost::formation_done() { NOTE("formation_done"); return true; }
bool NullHost::follow_wait() { NOTE("follow_wait"); return true; }
void NullHost::setup_dialog_window(int, std::span<const s32>) { NOTE("setup_dialog_window"); }
void NullHost::set_dialog_face(int) { NOTE("set_dialog_face"); }
bool NullHost::dialogue_open() { NOTE("dialogue_open"); return false; }
bool NullHost::drop_culled_dialogue() { NOTE("drop_culled_dialogue"); return false; }
bool NullHost::dialogue_blocked() { NOTE("dialogue_blocked"); return false; }
void NullHost::open_dialogue(int, int, int) { NOTE("open_dialogue"); }
void NullHost::dialogue_choice(int, int) { NOTE("dialogue_choice"); }
int NullHost::dialogue_result() { NOTE("dialogue_result"); return 0; }
void NullHost::bg_layer(int, bool) { NOTE("bg_layer"); }
void NullHost::set_layer_opacity(int, int, int, int) { NOTE("set_layer_opacity"); }
bool NullHost::bg_wave(int, int) { NOTE("bg_wave"); return true; }
void NullHost::bg_wave_start(std::span<const s32>) { NOTE("bg_wave_start"); }
void NullHost::set_shadow_colors(std::span<const u8>) { NOTE("set_shadow_colors"); }
void NullHost::set_screen_rgb(int, int, int, int, int) { NOTE("set_screen_rgb"); }
void NullHost::set_underwater_rgb(int, int, int) { NOTE("set_underwater_rgb"); }
void NullHost::fade(int, int) { NOTE("fade"); }
void NullHost::draw_sync() { NOTE("draw_sync"); }
void NullHost::set_entity_rgb(int, int, int, int) { NOTE("set_entity_rgb"); }
bool NullHost::play_song(int) { NOTE("play_song"); return true; }
bool NullHost::play_song_ex(int, int) { NOTE("play_song_ex"); return true; }
bool NullHost::song_changing() { NOTE("song_changing"); return false; }
void NullHost::play_sfx(int, int, int, int, int) { NOTE("play_sfx"); }
void NullHost::sfx_volume(int, int, int) { NOTE("sfx_volume"); }
void NullHost::fade_master_volume(int, int) { NOTE("fade_master_volume"); }
void NullHost::named_effect(NamedOp, std::string_view, int) { NOTE("named_effect"); }
void NullHost::play_movie(int, int) { NOTE("play_movie"); }
bool NullHost::movie_playing() { NOTE("movie_playing"); return false; }
void NullHost::request_battle(int, int, int) { NOTE("request_battle"); }
bool NullHost::battle_active() { NOTE("battle_active"); return false; }
void NullHost::request_map_jump(int, int) { NOTE("request_map_jump"); }
void NullHost::set_movie_jump(int, int) { NOTE("set_movie_jump"); }
void NullHost::request_menu(MenuKind, int) { NOTE("request_menu"); }
bool NullHost::menu_active() { NOTE("menu_active"); return false; }
void NullHost::field_reset() { NOTE("field_reset"); }
int NullHost::party_member(int) { NOTE("party_member"); return -1; }
bool NullHost::in_party(int) { NOTE("in_party"); return false; }
bool NullHost::party_add(int) { NOTE("party_add"); return true; }
void NullHost::party_remove(int) { NOTE("party_remove"); }
void NullHost::party_override(bool, int) { NOTE("party_override"); }
void NullHost::swap_leader(int) { NOTE("swap_leader"); }
void NullHost::roster_add(int) { NOTE("roster_add"); }
void NullHost::roster_remove(int) { NOTE("roster_remove"); }
int NullHost::item_count(int) { NOTE("item_count"); return 0; }
void NullHost::give_item(int) { NOTE("give_item"); }
void NullHost::take_item(int) { NOTE("take_item"); }
s32 NullHost::money() { NOTE("money"); return 0; }
void NullHost::set_money(s32) { NOTE("set_money"); }
int NullHost::char_life(int) { NOTE("char_life"); return 0; }
void NullHost::change_char_life(LifeOp, int, s32) { NOTE("change_char_life"); }
int NullHost::char_status(int) { NOTE("char_status"); return 0; }
void NullHost::heal_group(int) { NOTE("heal_group"); }
void NullHost::change_status(int, int, bool) { NOTE("change_status"); }
void NullHost::learn_limit(int, int, int) { NOTE("learn_limit"); }
void NullHost::set_limit_mode(int) { NOTE("set_limit_mode"); }
int NullHost::set_character_model(int, int) { NOTE("set_character_model"); return 0; }
bool NullHost::model_swap_busy() { NOTE("model_swap_busy"); return false; }
void NullHost::model_swap_commit() { NOTE("model_swap_commit"); }
int NullHost::character_model_byte(int) { NOTE("character_model_byte"); return 0; }
void NullHost::refresh_party_models() { NOTE("refresh_party_models"); }
void NullHost::table_op(u16, int, int) { NOTE("table_op"); }
u16 NullHost::keys_held() { NOTE("keys_held"); return 0; }
u16 NullHost::key_latch() { NOTE("key_latch"); return 0; }
void NullHost::clear_key_latch() { NOTE("clear_key_latch"); }
bool NullHost::predicate(Predicate, int) { NOTE("predicate"); return false; }
s32 NullHost::query(Query) { NOTE("query"); return 0; }
void NullHost::set_global(Global, s32) { NOTE("set_global"); }
void NullHost::opaque_op(u16, std::span<const u8>) { NOTE("opaque_op"); }

#undef NOTE

}  // namespace oc::script
