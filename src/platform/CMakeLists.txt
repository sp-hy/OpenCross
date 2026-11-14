add_library(oc_platform STATIC
  config.h
  config.cpp
  keymap.h
  keymap.cpp
  pad_remap.h
  pad_remap.cpp
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
  web_page.h
  web_page.cpp
)
target_link_libraries(oc_platform PUBLIC oc_core SDL3::SDL3 PRIVATE miniz::miniz)
if(WIN32)
  find_package(unofficial-webview2 CONFIG REQUIRED)
  target_link_libraries(oc_platform PRIVATE winhttp bcrypt version advapi32 unofficial::webview2::webview2 ole32 shell32 shlwapi user32)
endif()
