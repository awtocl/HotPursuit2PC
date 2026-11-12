# engine: the launcher that runs the game (ADR 0001 path C1, ADR 0005).
#
# Milestone 1 = emulator-backed: our own window, input and settings around
# the Play! PS2 core (BSD 2-Clause), linked statically. Play!'s source is not
# in this repository; tools/fetch-play.ps1 downloads the pinned revision into
# .cache/third_party/Play-. Without it the launcher is simply not built.

set(HP2_PLAY_DIR "${PROJECT_SOURCE_DIR}/.cache/third_party/Play-" CACHE PATH
    "Play! source tree (fetched by tools/fetch-play.ps1)")

if(NOT EXISTS "${HP2_PLAY_DIR}/Source/CMakeLists.txt" OR
   NOT EXISTS "${HP2_PLAY_DIR}/deps/Framework/build_cmake/Framework/CMakeLists.txt")
  message(STATUS "engine: Play! source not found in ${HP2_PLAY_DIR} - the launcher is NOT built "
                 "(run: pwsh tools/fetch-play.ps1)")
  return()
endif()
message(STATUS "engine: building the launcher on Play! from ${HP2_PLAY_DIR}")

# ---- Play!'s build expects to be the top-level project -----------------------
# Its Header.cmake only sets these when it is; we are a subdirectory, so the
# same settings are made here. They are scoped to engine/ and below.
set(TARGET_PLATFORM_WIN32 TRUE)
set(TARGET_PLATFORM_WIN32_X64 TRUE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS -D_UNICODE -DUNICODE
                -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DPLAY_VERSION="hp2pc")
list(PREPEND CMAKE_MODULE_PATH "${HP2_PLAY_DIR}/deps/Dependencies/cmake-modules")

# Bundled C libraries (zlib, zstd, bzip2, glew, ...) declare old CMake minimum
# versions; make them honour the static C runtime chosen at the top level so
# the launcher needs no Visual C++ redistributable.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)   # option() must not override the values set below

# No network features in the launcher.
set(ENABLE_AMAZON_S3 OFF CACHE BOOL "Play!: loading discs from Amazon S3" FORCE)
set(BUILD_SHARED_LIBS OFF)
set(HP2_ROOT_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)                   # keep third-party libraries from adding their own test targets

# Measurement builds only (preset "profile"): Play!'s own per-zone profiler
# (EE, VU, VIF, GIF, IOP, SPU ...), read by the launcher's --trace switch.
option(HP2_PLAY_PROFILE "Build the Play! core with its profiler zones (measurement builds)" OFF)
if(HP2_PLAY_PROFILE)
  set(PROFILE ON)
endif()

add_subdirectory("${HP2_PLAY_DIR}/Source" "${CMAKE_CURRENT_BINARY_DIR}/play/core" EXCLUDE_FROM_ALL)
add_subdirectory("${HP2_PLAY_DIR}/Source/gs/GSH_OpenGL" "${CMAKE_CURRENT_BINARY_DIR}/play/gsh_opengl" EXCLUDE_FROM_ALL)

# Play!'s code generator registers its own test suite with CTest
# unconditionally. Build it, so that `ctest` really runs it: it checks the
# JIT the game code runs on, on the machine doing the build.
if(HP2_ROOT_BUILD_TESTING AND TARGET CodeGenTestSuite)
  set_target_properties(CodeGenTestSuite PROPERTIES EXCLUDE_FROM_ALL FALSE)
endif()

# ---- virtual disc: the ROM folder as disc sectors (our code, C++20 like hp2core) ----
add_library(hp2vdisc STATIC vdisc/virtual_disc.cpp vdisc/virtual_disc.h)
set_target_properties(hp2vdisc PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(hp2vdisc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vdisc)
target_link_libraries(hp2vdisc PRIVATE hp2core)

# ---- the launcher -------------------------------------------------------------
add_executable(hp2-launcher WIN32
  src/main.cpp
  src/ref_trace.cpp
  src/ref_trace.h
  src/launcher.manifest
  "${HP2_PLAY_DIR}/Source/sound/SH_WaveOut/SH_WaveOut.cpp")
set_target_properties(hp2-launcher PROPERTIES OUTPUT_NAME HotPursuit2)
target_include_directories(hp2-launcher PRIVATE
  "${HP2_PLAY_DIR}/Source"
  "${HP2_PLAY_DIR}/deps/Framework/include"
  "${HP2_PLAY_DIR}/deps/CodeGen/include")
target_link_libraries(hp2-launcher PRIVATE hp2vdisc gsh_opengl PlayCore opengl32 winmm xinput dbghelp user32 gdi32 shell32)
if(MSVC)
  target_compile_options(hp2-launcher PRIVATE /W3 /utf-8)
endif()
