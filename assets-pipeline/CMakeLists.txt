# assets-pipeline: reads the PS2 dump's containers (Z fileset, LZC). ADR 0004.

# --- name table: data/hp2_slus20362_names.txt -> generated/hp2_names.inc ----
set(HP2_NAMES_TXT ${CMAKE_CURRENT_SOURCE_DIR}/data/hp2_slus20362_names.txt)
set(HP2_NAMES_INC ${CMAKE_CURRENT_BINARY_DIR}/generated/hp2_names.inc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HP2_NAMES_TXT})

file(STRINGS ${HP2_NAMES_TXT} hp2_name_lines)
set(hp2_names_inc "// Generated from data/hp2_slus20362_names.txt - do not edit.\n")
set(hp2_name_count 0)
foreach(line IN LISTS hp2_name_lines)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  if(NOT line MATCHES "^([0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F])\t([A-D])\t([A-Z0-9_ .\\\\-]+)$")
    message(FATAL_ERROR "Malformed line in ${HP2_NAMES_TXT}: '${line}'")
  endif()
  set(name "${CMAKE_MATCH_3}")
  string(REPLACE "\\" "\\\\" name "${name}")
  string(APPEND hp2_names_inc "{0x${CMAKE_MATCH_1}u, \"${name}\", '${CMAKE_MATCH_2}'},\n")
  math(EXPR hp2_name_count "${hp2_name_count} + 1")
endforeach()
if(hp2_name_count EQUAL 0)
  message(FATAL_ERROR "${HP2_NAMES_TXT} contains no names")
endif()
file(CONFIGURE OUTPUT ${HP2_NAMES_INC} CONTENT "${hp2_names_inc}" @ONLY)
message(STATUS "hp2assets: ${hp2_name_count} file names compiled in")

# --- library ----------------------------------------------------------------
add_library(hp2assets STATIC
  src/io.cpp
  src/zdir.cpp
  src/lzc.cpp
  src/names.cpp
  src/fileset.cpp
  include/hp2/common.h
  include/hp2/io.h
  include/hp2/zdir.h
  include/hp2/lzc.h
  include/hp2/names.h
  include/hp2/fileset.h)
target_include_directories(hp2assets
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(hp2assets PUBLIC hp2_build_flags PRIVATE bcrypt)

# --- developer tool -----------------------------------------------------------
add_executable(hp2assets-tool tool/main.cpp)
set_target_properties(hp2assets-tool PROPERTIES OUTPUT_NAME hp2assets)
target_link_libraries(hp2assets-tool PRIVATE hp2assets)

# --- tests ----------------------------------------------------------------------
if(BUILD_TESTING)
  hp2_add_test(test_assets SOURCES tests/test_assets.cpp LIBS hp2assets)
  hp2_add_test(test_dump SOURCES tests/test_dump.cpp LIBS hp2assets)
endif()
