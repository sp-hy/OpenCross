# Typed loaders for the battle / menu tables.
add_library(oc_data STATIC
  tables.h
  tables.cpp
  table_rows.h
  table_rows.cpp
  char_name.h
  char_name.cpp
  bestiary.h
  bestiary.cpp
)
target_link_libraries(oc_data PUBLIC oc_core oc_vfs oc_import oc_text)
