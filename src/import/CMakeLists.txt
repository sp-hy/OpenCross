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
  room_fixes.h
  room_fixes.cpp
  room_cards.h
  room_cards.cpp
  script_fixes.h
  script_fixes.cpp
  chip2.h
  chip2.cpp
  model.h
  model.cpp
  str_stream.h
  str_stream.cpp
  mdec.h
  mdec.cpp
)
target_link_libraries(oc_import PUBLIC oc_core oc_assets)

# Text importer (lang.dat tables, JP glyphs, message parsing, DirectXTK spritefonts). Kept as its own target so the
# oc_import source list above stays untouched.
add_library(oc_text STATIC
  text.h
  text.cpp
  text_tables.inc
  ps1_text.h
  ps1_text.cpp
  ps1_font.h
  ps1_font.cpp
  battle_hud.h
  battle_hud.cpp
  spritefont.h
  spritefont.cpp
)
target_link_libraries(oc_text PUBLIC oc_import oc_vfs)

# PS1 disc images (containers, the game's file table, build identification). Unlike the decoders above it opens the image files
# itself.
add_library(oc_disc STATIC
  disc/sector_source.h
  disc/sector_source.cpp
  disc/image_files.h
  disc/disc_image.h
  disc/disc_image.cpp
  disc/pbp.cpp
  disc/chd_source.h
  disc/chd_source.cpp
  disc/disc_table.h
  disc/disc_table.cpp
  disc/disc_ident.h
  disc/disc_ident.cpp
  disc/sha256.h
  disc/sha256.cpp
  disc/drive_source.h
  disc/drive_source.cpp
  disc/disc_search.h
  disc/disc_search.cpp
  disc/disc_backend.h
  disc/disc_backend.cpp
)
target_link_libraries(oc_disc PUBLIC oc_core oc_vfs PRIVATE miniz::miniz chdr oc_optical)

# Battle stage importer (battle/bg/bg_NN.prd + .ctd), its own target next to oc_text.
add_library(oc_stage STATIC
  battle_stage.h
  battle_stage.cpp
)
target_link_libraries(oc_stage PUBLIC oc_import)
