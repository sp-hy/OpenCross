# libchdr (BSD-3-Clause), vendored: CHD decoding only. zlib comes from the vcpkg zlib the other libraries already use; LZMA (public
# domain, portable C decoder) and the zstd decoder are the copies libchdr ships.
find_package(ZLIB REQUIRED)

add_library(chdr STATIC
  src/libchdr_bitstream.c
  src/libchdr_cdrom.c
  src/libchdr_chd.c
  src/libchdr_codec_avhuff.c
  src/libchdr_codec_cdfl.c
  src/libchdr_codec_cdlz.c
  src/libchdr_codec_cdzl.c
  src/libchdr_codec_cdzs.c
  src/libchdr_codec_flac.c
  src/libchdr_codec_huff.c
  src/libchdr_codec_lzma.c
  src/libchdr_codec_zlib.c
  src/libchdr_codec_zstd.c
  src/libchdr_flac.c
  src/libchdr_huffman.c
  deps/lzma-26.02/src/LzmaDec.c
  deps/zstd-1.5.7/zstddeclib.c
)
target_include_directories(chdr PUBLIC include)
target_compile_definitions(chdr PUBLIC
  WANT_RAW_DATA_SECTOR=1
  WANT_SUBCODE=1
  VERIFY_BLOCK_CRC=1
  LOWRAM_TARGET=0
  CHDR_SYSTEM_ZLIB)
target_link_libraries(chdr PRIVATE ZLIB::ZLIB)
if(MSVC)
  target_compile_definitions(chdr PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(chdr PRIVATE /w)
else()
  target_compile_options(chdr PRIVATE -w)
endif()
set_target_properties(chdr PROPERTIES POSITION_INDEPENDENT_CODE ON)
