# Developer tools, linked into opencross.exe and run as `opencross <tool> [args]` (src/tool_main.cpp): one shipped binary.
#   occ (rooms / models / scripts / effects / saves), octext, ocaudio, ochd, ocprd, ocstage, ocbattle, ocmovie,
#   ocbattleview, ocbattleseq, ocflow, ocmenudemo, ocshopdemo, ocmodelview (the "oc" prefix is optional: `opencross battleview 7`).
add_library(oc_tools STATIC
  occ.cpp
  occ_save.cpp
  occ_dump_textures.cpp
  octext.cpp
  ocaudio.cpp
  ochd.cpp
  ocprd.cpp
  ocstage.cpp
  ocbattle.cpp
  ocbattle_fx.cpp
  ocbattle_reg.cpp
  ocmovie.cpp
  ocmodelview.cpp
  model_view.cpp
  tool_util.cpp
  ${CMAKE_SOURCE_DIR}/src/game/battle/demo_main.cpp
  ${CMAKE_SOURCE_DIR}/src/game/battle/seq_tool.cpp
  ${CMAKE_SOURCE_DIR}/src/game/flow_demo_main.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/menu/demo_main.cpp
  ${CMAKE_SOURCE_DIR}/src/ui/shop/demo_main.cpp
)
target_include_directories(oc_tools PRIVATE ${Stb_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(oc_tools PRIVATE oc_core oc_platform oc_vfs oc_import oc_assets oc_save oc_script oc_text oc_audio oc_effects
  oc_stage oc_data oc_battle oc_flow oc_ui oc_battle_fx oc_battle_view oc_battle_anim oc_render oc_movie oc_game oc_menu_model SDL3::SDL3)
