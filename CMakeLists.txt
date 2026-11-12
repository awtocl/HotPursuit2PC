cmake_minimum_required(VERSION 3.25)

project(HotPursuit2PC
  VERSION 0.1.0
  DESCRIPTION "Converter that turns a user's own NFS Hot Pursuit 2 (PS2, NTSC-U) dump into a PC build"
  LANGUAGES C CXX)   # C: only for third-party libraries pulled in by engine/

if(NOT WIN32)
  message(FATAL_ERROR "HotPursuit2PC targets Windows 10/11 x64 only (brief: target platform).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "HotPursuit2PC must be built for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: every shipped .exe must run on a clean Windows machine (ADR 0002).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(HP2_WERROR "Treat compiler warnings as errors" OFF)

# Common compile settings for every first-party target.
add_library(hp2_build_flags INTERFACE)
target_compile_definitions(hp2_build_flags INTERFACE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(hp2_build_flags INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus
    $<$<BOOL:${HP2_WERROR}>:/WX>)
endif()

include(CTest)   # defines BUILD_TESTING (ON by default) and enables testing

add_subdirectory(tests/harness)
add_subdirectory(assets-pipeline)
add_subdirectory(engine)      # before converter/: the converter embeds the launcher when it exists
add_subdirectory(converter)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
