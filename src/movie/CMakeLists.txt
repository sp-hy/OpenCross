# oc_movie: FMV playback. Bink 2 decoding by librempeg (GPL-3), built by scripts/build-librempeg.ps1 into third_party/librempeg
# (static, /MT); the PS1 discs' movie streams through oc_import. Video goes to an RGBA texture, audio to the mixer's movie bus.
set(LIBREMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/librempeg)
if(NOT EXISTS ${LIBREMPEG_DIR}/lib/avcodec.lib)
  message(FATAL_ERROR "librempeg not built: run scripts\\build-librempeg.ps1")
endif()
add_library(oc_movie STATIC
  decoder.h
  decoder.cpp
  player.h
  player.cpp
)
target_include_directories(oc_movie SYSTEM PRIVATE ${LIBREMPEG_DIR}/include)
target_link_libraries(oc_movie PUBLIC oc_core oc_import oc_vfs oc_render oc_audio
  ${LIBREMPEG_DIR}/lib/avformat.lib ${LIBREMPEG_DIR}/lib/avcodec.lib
  ${LIBREMPEG_DIR}/lib/avfilter.lib ${LIBREMPEG_DIR}/lib/avutil.lib
  bcrypt.lib ole32.lib user32.lib)
# librempeg asks for the release static CRT; the Debug exe links the debug one.
target_link_options(oc_movie INTERFACE $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt>)
