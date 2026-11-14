# Game modes: the field (room backdrop, actors, walk mesh, FieldWorld = script host + player + triggers + dialogue).
add_library(oc_game STATIC
  room_lighting.h
  room_lighting.cpp
  room_view.h
  room_view.cpp
  room_models.h
  room_models.cpp
  walk_mesh.h
  walk_mesh.cpp
  model_files.h
  model_files.cpp
  model_export.h
  model_export.cpp
  model_mods.h
  model_mods.cpp
  field_model_source.h
  field_model_source.cpp
  menu_rig.h
  menu_rig.cpp
  actor.h
  actor.cpp
  disc_index.h
  disc_index.cpp
  mod_rooms.h
  mod_rooms.cpp
  mod_vars.h
  mod_vars.cpp
  field_camera.h
  field_camera.cpp
  field_watchdog.h
  field_watchdog.cpp
  field_battle_handoff.h
  field_battle_handoff.cpp
  button_panel.h
  button_panel.cpp
  field_input.h
  field_input.cpp
  field_world.h
  field_world.cpp
  field_world_entity.cpp
  field_world_sim.cpp
  field_world_collide.cpp
  field_world_transition.cpp
  field_world_dialogue.cpp
  field_world_party.cpp
  field_world_tick.cpp
  field_world_internal.h
  field_world_audio.cpp
  field_world_draw.cpp
  field_world_effects.cpp
  field_world_remote.cpp
  remote_parties.h
  remote_parties.cpp
  route_script.h
  route_script.cpp
  achievements.h
  achievements.cpp
  oc_achievements.h
  oc_achievements.cpp
  oc_achievement_table.inc
  oc_achievements_ui.h
  oc_achievements_ui.cpp
  autosave.h
  autosave.cpp
  cheats.h
  cheats.cpp
  new_game_plus.h
  new_game_plus.cpp
  new_game.h
  new_game.cpp
  randomizer.h
  randomizer.cpp
  randomizer_gather.cpp
  room_scan.h
  room_scan.cpp
  records_info.h
  records_info.cpp
  scan_service.h
  scan_service.cpp
)
target_link_libraries(oc_game PUBLIC oc_core oc_platform oc_vfs oc_import oc_assets oc_render oc_script oc_ui oc_text oc_audio oc_effects oc_effects_gpu oc_battle oc_battle_anim SDL3::SDL3)
target_include_directories(oc_game PRIVATE ${Stb_INCLUDE_DIR})

add_subdirectory(battle)
add_subdirectory(effects)

# Game flow: Title -> NameEntry -> Opening -> Field, Continue -> slot list -> Field (game_flow.h).
add_library(oc_flow STATIC
  game_flow.h
  game_flow.cpp
  game_session.h
  game_session.cpp
  party_editor.h
  party_editor.cpp
  title_backdrop.h
  title_backdrop.cpp
  debug_tools.h
  debug_tools.cpp
)
target_link_libraries(oc_flow PUBLIC oc_game oc_battle_view oc_title oc_keymap oc_ui oc_data oc_save oc_import oc_audio oc_battle oc_battle_anim oc_stage oc_effects oc_effects_gpu oc_render oc_vfs oc_assets)

