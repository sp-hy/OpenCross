# XINPUT9_1_0.dll, the launcher proxy. A project of its own: the launcher is a 32-bit process, the engine a 64-bit one (build.ps1 -Launcher
# configures this with the MSVC x86 environment). The managed patch is compiled by the C# compiler every .NET Framework install has and
# embedded in the DLL, so one file ships.
cmake_minimum_required(VERSION 3.28)
project(OpenCrossLauncherProxy LANGUAGES CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The launcher proxy is 32-bit: configure it from the MSVC x86 environment")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

file(TO_CMAKE_PATH "$ENV{WINDIR}/Microsoft.NET/Framework/v4.0.30319" framework_dir)

set(patch_source ${CMAKE_CURRENT_SOURCE_DIR}/LauncherPatch.cs)
set(patch_assembly ${CMAKE_CURRENT_BINARY_DIR}/LauncherPatch.dll)
file(TO_NATIVE_PATH ${patch_assembly} patch_assembly_native)
add_custom_command(
  OUTPUT ${patch_assembly}
  COMMAND ${framework_dir}/csc.exe /nologo /target:library /platform:x86 /optimize+ /warnaserror+
          /r:System.Drawing.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll
          /out:${patch_assembly_native} LauncherPatch.cs
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${patch_source}
  VERBATIM)

add_library(xinput_proxy SHARED xinput_proxy.cpp xinput_proxy.def payload.rc ${patch_assembly})
set_target_properties(xinput_proxy PROPERTIES OUTPUT_NAME XINPUT9_1_0 PREFIX "" RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
target_compile_options(xinput_proxy PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor /I${framework_dir})
target_compile_definitions(xinput_proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(xinput_proxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(SOURCE payload.rc APPEND PROPERTY OBJECT_DEPENDS ${patch_assembly})
target_link_libraries(xinput_proxy PRIVATE oleaut32 ole32)
