cmake_minimum_required(VERSION 3.28)
project(OpenCross VERSION 0.4.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# One self-contained exe: static vcpkg libraries (x64-windows-static) and the static C runtime (librempeg is built /MT to match).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Objects are cached by content when ccache is on PATH. A shared compile PDB (/Zi) makes every compile uncacheable, so debug info
# stays in the objects (/Z7); the linker still writes the exe's PDB.
option(OC_CCACHE "Compile through ccache when it is installed" ON)
find_program(OC_CCACHE_PROGRAM ccache)
if(OC_CCACHE AND OC_CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER ${OC_CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${OC_CCACHE_PROGRAM})
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:Embedded>")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor /MP)
  # Release: function / data sections + whole-program optimisation, unreferenced code folded away (smaller exe).
  add_compile_options($<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw> $<$<CONFIG:Release>:/GL>)
  add_link_options($<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # No incremental linking: the .ilk databases took ~3.5 GB across the Debug / RelWithDebInfo exes and tests on a nearly full disk.
  add_link_options(/INCREMENTAL:NO)
else()
  add_compile_options(-Wall -Wextra)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(Stb REQUIRED)
find_package(SPNG CONFIG REQUIRED)
# shaderc (glslc) and sdl3-shadercross are build tools only (src/render/CMakeLists.txt); neither library is linked.

add_subdirectory(src)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)
