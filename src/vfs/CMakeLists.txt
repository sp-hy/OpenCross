add_library(oc_vfs STATIC
  vfs.h
  vfs.cpp
  mods.h
  mods.cpp
  archive.h
  archive.cpp
  mod_archive.h
  mod_archive.cpp
  backend.h
  rde_backend.h
  rde_backend.cpp
  disc_names.h
  disc_names.cpp
  disc_names.inc
)
target_link_libraries(oc_vfs PUBLIC oc_core PRIVATE miniz::miniz LibArchive::LibArchive)
