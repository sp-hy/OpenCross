add_library(oc_vfs STATIC
  vfs.h
  vfs.cpp
  mods.h
  mods.cpp
  archive.h
  archive.cpp
  mod_archive.h
  mod_archive.cpp
)
target_link_libraries(oc_vfs PUBLIC oc_core PRIVATE miniz::miniz LibArchive::LibArchive)
