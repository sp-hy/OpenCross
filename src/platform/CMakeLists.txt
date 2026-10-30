add_library(oc_platform STATIC
  config.h
  config.cpp
  keymap.h
  keymap.cpp
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
)
target_link_libraries(oc_platform PUBLIC oc_core SDL3::SDL3 PRIVATE miniz::miniz)
if(WIN32)
  target_link_libraries(oc_platform PRIVATE winhttp bcrypt version advapi32)
endif()
