# minitest: the in-repo, dependency-free test harness (ADR 0002: no package manager).
add_library(hp2_minitest INTERFACE)
target_include_directories(hp2_minitest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hp2_minitest INTERFACE hp2_build_flags)

# hp2_add_test(<name> SOURCES a.cpp ... [LIBS lib ...])
# Registers one CTest test per executable. Exit code 77 = skipped
# (used by tests that need the owner's dump, which CI does not have).
function(hp2_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE hp2_minitest ${ARG_LIBS})
  target_compile_definitions(${name} PRIVATE
    HP2_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endfunction()
