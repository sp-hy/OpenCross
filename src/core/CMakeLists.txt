add_library(oc_core STATIC
  features.cpp
  features.h
  log.cpp
  log.h
  types.h
  span_reader.h
  parallel.h
)
# core/version.h from the project version (also the exe's version resource, src/opencross.rc.in).
configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/gen/core/version.h @ONLY)
target_include_directories(oc_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/gen)
