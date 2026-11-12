# converter: the user-facing tool (brief §3) - core library, CLI and GUI. ADR 0003.

# --- reference manifest of the supported disc -> generated/hp2_rom_manifest.inc ---
set(HP2_ROM_TSV ${CMAKE_CURRENT_SOURCE_DIR}/data/slus20362_rom_manifest.tsv)
set(HP2_ROM_INC ${CMAKE_CURRENT_BINARY_DIR}/generated/hp2_rom_manifest.inc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HP2_ROM_TSV})

file(STRINGS ${HP2_ROM_TSV} hp2_rom_lines)
set(hp2_rom_inc "// Generated from data/slus20362_rom_manifest.tsv - do not edit.\n")
set(hp2_rom_count 0)
foreach(line IN LISTS hp2_rom_lines)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  if(NOT line MATCHES "^([A-Za-z0-9_./]+)\t([0-9]+)\t([0-9a-f]+)$")
    message(FATAL_ERROR "Malformed line in ${HP2_ROM_TSV}: '${line}'")
  endif()
  string(APPEND hp2_rom_inc "{\"${CMAKE_MATCH_1}\", ${CMAKE_MATCH_2}ull, \"${CMAKE_MATCH_3}\"},\n")
  math(EXPR hp2_rom_count "${hp2_rom_count} + 1")
endforeach()
if(hp2_rom_count EQUAL 0)
  message(FATAL_ERROR "${HP2_ROM_TSV} lists no files")
endif()
file(CONFIGURE OUTPUT ${HP2_ROM_INC} CONTENT "${hp2_rom_inc}" @ONLY)
message(STATUS "hp2core: reference manifest with ${hp2_rom_count} disc files compiled in")

# --- core -----------------------------------------------------------------------
add_library(hp2core STATIC
  core/src/rom.cpp
  core/src/convert.cpp
  core/include/hp2/rom.h
  core/include/hp2/convert.h)
target_include_directories(hp2core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(hp2core PUBLIC hp2assets)

# --- CLI: hp2pc-cli.exe -----------------------------------------------------------
add_executable(hp2pc-cli cli/main.cpp)
target_link_libraries(hp2pc-cli PRIVATE hp2core)

# --- GUI: HotPursuit2PC.exe ---------------------------------------------------------
add_executable(hp2pc-gui WIN32 gui/main.cpp gui/app.manifest)
set_target_properties(hp2pc-gui PROPERTIES OUTPUT_NAME HotPursuit2PC)
target_link_libraries(hp2pc-gui PRIVATE hp2core comctl32 ole32 shell32 user32 gdi32)

# --- tests -----------------------------------------------------------------------
if(BUILD_TESTING)
  hp2_add_test(test_core SOURCES tests/test_core.cpp LIBS hp2core)
  hp2_add_test(test_core_dump SOURCES tests/test_core_dump.cpp LIBS hp2core)
  # The GUI can check itself without anyone clicking: creates the window,
  # verifies its controls and exits (ADR 0003 §3).
  add_test(NAME gui_selftest COMMAND hp2pc-gui --selftest)
  set_tests_properties(gui_selftest PROPERTIES TIMEOUT 60)
  # Drives the real window through a real conversion of the owner's dump
  # (reference mode, no asset unpacking: a few KB). Skipped without a dump.
  add_test(NAME gui_convert_dump COMMAND hp2pc-gui --selftest-convert
           "${PROJECT_SOURCE_DIR}/ROM/Need for Speed - Hot Pursuit 2 (USA)"
           "${CMAKE_CURRENT_BINARY_DIR}/gui-selftest-game")
  set_tests_properties(gui_convert_dump PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
  add_test(NAME cli_usage COMMAND hp2pc-cli version)
endif()
