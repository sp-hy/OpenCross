add_library(oc_platform STATIC
  config.h
  config.cpp
  keymap.h
  keymap.cpp
  pad_remap.h
  pad_remap.cpp
  process_exit.h
  process_exit.cpp
  paths.h
  paths.cpp
  rumble.h
  rumble.cpp
  window.h
  window.cpp
  updater.h
  updater.cpp
  steam.h
  steam.cpp
  steam_abi.h
  steam_browser.h
  steam_browser.cpp
  web_page.h
  web_page.cpp
)
target_link_libraries(oc_platform PUBLIC oc_core SDL3::SDL3 PRIVATE miniz::miniz)
# Optical drive access, kept apart from oc_platform so the disc reader can use it without SDL or the web view.
add_library(oc_optical STATIC
  optical.h
  optical.cpp
)
target_link_libraries(oc_optical PUBLIC oc_core)
if(WIN32)
  find_package(unofficial-webview2 CONFIG REQUIRED)
  target_link_libraries(oc_platform PRIVATE winhttp bcrypt version advapi32 unofficial::webview2::webview2 ole32 shell32 shlwapi user32)
endif()
