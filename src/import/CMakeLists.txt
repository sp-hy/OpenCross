# Decoders for the PS1 / RDE file formats.
add_library(oc_import STATIC
  lzss.h
  lzss.cpp
  container.h
  container.cpp
  tim.h
  tim.cpp
  room.h
  room.cpp
  room_effects.h
  room_effects.cpp
  script_fixes.h
  script_fixes.cpp
  chip2.h
  chip2.cpp
  model.h
  model.cpp
)
target_link_libraries(oc_import PUBLIC oc_core oc_assets)

# Text importer (lang.dat tables, JP glyphs, message parsing, DirectXTK spritefonts). Kept as its own target so the
# oc_import source list above stays untouched.
add_library(oc_text STATIC
  text.h
  text.cpp
  text_tables.inc
  spritefont.h
  spritefont.cpp
)
target_link_libraries(oc_text PUBLIC oc_import oc_vfs)

# Battle stage importer (battle/bg/bg_NN.prd + .ctd), its own target next to oc_text.
add_library(oc_stage STATIC
  battle_stage.h
  battle_stage.cpp
)
target_link_libraries(oc_stage PUBLIC oc_import)
