# oc_audio: SAB container + HCA streaming + mixer (SDL3 audio device). clHCA is vendored in third_party/clhca (ISC).
add_library(oc_audio STATIC
  clhca_unit.c
  byte_io.h
  sab.h
  sab.cpp
  hca_stream.h
  hca_stream.cpp
  bgm_table.h
  bgm_table.cpp
  mixer.h
  mixer.cpp
  adpcm.h
  adpcm.cpp
  akao_tables.h
  akao_tables.cpp
  akao_bank.h
  akao_bank.cpp
  akao_seq.h
  akao_seq.cpp
  akao_player.h
  akao_player.cpp
  spu_reverb.h
  spu_reverb.cpp
  sfx.h
  sfx.cpp
  voice.h
  voice.cpp
)
target_include_directories(oc_audio PRIVATE ${CMAKE_SOURCE_DIR}/third_party/clhca)
target_link_libraries(oc_audio PUBLIC oc_core PRIVATE oc_vfs SDL3::SDL3)
# voice lines: MP3 decode through Windows Media Foundation (the librempeg build is Bink-only)
if(WIN32)
  target_link_libraries(oc_audio PRIVATE mfplat mfreadwrite mfuuid ole32 shlwapi)
endif()
