# recomp: static recompilation of the game program (stage R1, ADR 0006).
# So far: executable reader, R5900 instruction decoder, function discovery,
# and the developer tool that checks discovery against the reference core.

add_library(hp2recomp STATIC
  src/elf.cpp
  src/r5900.cpp
  src/discover.cpp
  include/hp2/elf.h
  include/hp2/r5900.h
  include/hp2/discover.h)
target_include_directories(hp2recomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hp2recomp PUBLIC hp2_build_flags hp2assets)

# --- developer tool -----------------------------------------------------------
add_executable(hp2recomp-tool tool/main.cpp)
set_target_properties(hp2recomp-tool PROPERTIES OUTPUT_NAME hp2recomp)
target_link_libraries(hp2recomp-tool PRIVATE hp2recomp)

# --- tests ----------------------------------------------------------------------
if(BUILD_TESTING)
  hp2_add_test(test_recomp SOURCES tests/test_recomp.cpp LIBS hp2recomp)
  hp2_add_test(test_recomp_dump SOURCES tests/test_recomp_dump.cpp LIBS hp2recomp)
endif()
