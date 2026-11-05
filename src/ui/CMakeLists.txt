# UI: dialogue window, typewriter, main menu. Depends on render + the text importer (+ save/data for the menu state).
add_library(oc_ui STATIC
  typewriter.h
  typewriter.cpp
  dialogue_box.h
  dialogue_box.cpp
  keyitem_popup.h
  keyitem_popup.cpp
  lowres.h
  lowres.cpp
  ps1_window.h
  ps1_window.cpp
  window_frame.h
  window_frame.cpp
  ps1_font.h
  ps1_font.cpp
  party_screen.h
  party_screen.cpp
  monster_pick.h
  monster_pick.cpp
  monster_pick_screen.h
  monster_pick_screen.cpp
  pause_screen.h
  pause_screen.cpp
  feature_icons.h
  feature_icons.cpp
  world_name.h
  world_name.cpp
  menu/game_state.h
  menu/game_state.cpp
  menu/use_flow.h
  menu/use_flow.cpp
  menu/main_menu.h
  menu/main_menu_impl.h
  menu/main_menu.cpp
  menu/main_menu_draw.cpp
  menu/main_menu_main.cpp
  menu/main_menu_elements.cpp
  menu/main_menu_equip.cpp
  menu/main_menu_settings.cpp
  menu/main_menu_save.cpp
  menu/menu_demo.h
  menu/menu_demo.cpp
  menu/ps1_art.h
  menu/ps1_art.cpp
  menu/hand.h
  menu/hand.cpp
  shop/shop_logic.h
  shop/shop_logic.cpp
  shop/shop_screen.h
  shop/shop_screen.cpp
  shop/shop_demo.h
  shop/shop_demo.cpp
)
target_link_libraries(oc_ui PUBLIC oc_core oc_platform oc_vfs oc_import oc_text oc_render oc_save oc_data SDL3::SDL3)

# Status-screen and Grand Slam picker model views (they need the field Actor, so they live outside oc_ui, which oc_game depends on).
add_library(oc_menu_model STATIC menu/status_model.h menu/status_model.cpp menu/picker_model.h menu/picker_model.cpp)
target_link_libraries(oc_menu_model PUBLIC oc_ui oc_game oc_battle_view oc_render oc_import)

add_subdirectory(title)

# RDE user guide (controls / key map) shown by New Game before the name entry; draws with the title UiKit, so it sits above oc_title.
add_library(oc_keymap STATIC keymap_screen.h keymap_screen.cpp)
target_link_libraries(oc_keymap PUBLIC oc_title oc_ui oc_platform oc_import)
