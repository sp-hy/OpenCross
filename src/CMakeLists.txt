# Engine libraries and the game executable.
# Each subdirectory is one module; keep dependencies pointing downward:
#   platform <- vfs <- import <- assets <- render/audio/script <- game/ui <- opencross (exe)

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(net)
add_subdirectory(vfs)
add_subdirectory(assets)
add_subdirectory(import)
add_subdirectory(save)
add_subdirectory(script)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(movie)
add_subdirectory(ui)
add_subdirectory(game)
add_subdirectory(data)
add_subdirectory(battle)

# The exe's File Version / Product Version resource = project(OpenCross VERSION ...).
configure_file(opencross.rc.in ${CMAKE_CURRENT_BINARY_DIR}/opencross.rc @ONLY)
add_executable(opencross WIN32
  main.cpp
  tool_main.cpp
  app/app.h
  app/app_battle.cpp
  app/app_boot.cpp
  app/app_debug.cpp
  app/app_draw.cpp
  app/app_feedback.cpp
  app/app_multiplayer.cpp
  app/app_route.cpp
  app/app_run.cpp
  app/app_sim.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/opencross.rc)
target_link_libraries(opencross PRIVATE oc_core oc_platform oc_net oc_vfs oc_import oc_render oc_ui oc_audio oc_game oc_data oc_battle_view oc_flow oc_movie oc_save oc_menu_model oc_tools SDL3::SDL3)
set_target_properties(opencross PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
