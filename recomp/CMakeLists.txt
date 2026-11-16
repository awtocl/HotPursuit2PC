# recomp: static recompilation of the game program (stage R1, ADR 0006).
# So far: executable reader, R5900 instruction decoder, function discovery,
# and the developer tool that checks discovery against the reference core.

add_library(hp2recomp STATIC
  src/elf.cpp
  src/r5900.cpp
  src/discover.cpp
  src/vu.cpp
  include/hp2/elf.h
  include/hp2/r5900.h
  include/hp2/discover.h
  include/hp2/vu.h)
target_include_directories(hp2recomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The tool is called hp2recomp.exe; its import library would clash with hp2recomp.lib.
set_target_properties(hp2recomp PROPERTIES OUTPUT_NAME hp2recomp_core)
target_link_libraries(hp2recomp PUBLIC hp2_build_flags hp2assets)

# --- LLVM (the code generator, ADR 0007) -----------------------------------------
# Not in this repository: tools/fetch-llvm.ps1 + tools/build-llvm.ps1 put it
# into .cache/third_party/. Without it the translator is simply not built.
set(HP2_LLVM_DIR "${PROJECT_SOURCE_DIR}/.cache/third_party/llvm-23.1.5-install" CACHE PATH
    "LLVM install tree (built by tools/build-llvm.ps1)")
find_package(LLVM CONFIG PATHS "${HP2_LLVM_DIR}/lib/cmake/llvm" NO_DEFAULT_PATH QUIET)
if(LLVM_FOUND)
  message(STATUS "recomp: LLVM ${LLVM_PACKAGE_VERSION} found in ${HP2_LLVM_DIR} - the translator is built")
  llvm_map_components_to_libnames(HP2_LLVM_LIBS core orcjit native passes)
  add_library(hp2_llvm INTERFACE)
  target_include_directories(hp2_llvm SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(hp2_llvm INTERFACE ${LLVM_DEFINITIONS_LIST} NOMINMAX)
  target_link_libraries(hp2_llvm INTERFACE ${HP2_LLVM_LIBS})
  if(MSVC)
    # LLVM's headers are not written for /W4; keep our warning level for our
    # own code and silence what comes out of their templates.
    target_compile_options(hp2_llvm INTERFACE /external:templates- /wd4244 /wd4267 /wd4146 /wd4245 /wd4624 /wd4702)
  endif()
else()
  message(STATUS "recomp: LLVM not found in ${HP2_LLVM_DIR} - the translator is NOT built "
                 "(run: pwsh tools/fetch-llvm.ps1; pwsh tools/build-llvm.ps1)")
endif()

# --- translator (needs LLVM) ---------------------------------------------------
if(LLVM_FOUND)
  add_library(hp2lift STATIC src/lift.cpp src/lift_internal.h src/codegen.cpp src/vulift.cpp include/hp2/lift.h include/hp2/vulift.h)
  target_link_libraries(hp2lift PUBLIC hp2recomp PRIVATE hp2_llvm)

  # The converter's "code" stage (ADR 0009): everything translated, compiled
  # in parallel and linked into a DLL with LLD (built by tools/build-llvm.ps1).
  find_package(LLD CONFIG PATHS "${HP2_LLVM_DIR}/lib/cmake/lld" NO_DEFAULT_PATH QUIET)
  if(LLD_FOUND OR TARGET lldCOFF)
    add_library(hp2package STATIC src/package.cpp include/hp2/package.h include/hp2/code_package.h)
    target_link_libraries(hp2package PUBLIC hp2lift PRIVATE hp2_llvm lldCOFF lldCommon)
    target_include_directories(hp2package SYSTEM PRIVATE "${HP2_LLVM_DIR}/include")
    message(STATUS "recomp: LLD found - the code package builder is built")
  else()
    message(STATUS "recomp: LLD not found in ${HP2_LLVM_DIR} - no code package builder (run tools/build-llvm.ps1)")
  endif()
endif()

# --- developer tool -----------------------------------------------------------
add_executable(hp2recomp-tool tool/main.cpp)
set_target_properties(hp2recomp-tool PROPERTIES OUTPUT_NAME hp2recomp)
target_link_libraries(hp2recomp-tool PRIVATE hp2recomp)
if(LLVM_FOUND)
  target_link_libraries(hp2recomp-tool PRIVATE hp2lift)
  target_compile_definitions(hp2recomp-tool PRIVATE HP2_HAVE_LLVM=1)
  if(TARGET hp2package)
    target_link_libraries(hp2recomp-tool PRIVATE hp2package)
    target_compile_definitions(hp2recomp-tool PRIVATE HP2_HAVE_PACKAGE=1)
  endif()
endif()

# --- tests ----------------------------------------------------------------------
if(BUILD_TESTING)
  hp2_add_test(test_recomp SOURCES tests/test_recomp.cpp LIBS hp2recomp)
  hp2_add_test(test_recomp_dump SOURCES tests/test_recomp_dump.cpp LIBS hp2recomp)
  if(LLVM_FOUND)
    hp2_add_test(test_llvm_smoke SOURCES tests/test_llvm_smoke.cpp LIBS hp2_llvm)
    hp2_add_test(test_lift SOURCES tests/test_lift.cpp LIBS hp2lift)
    hp2_add_test(test_vulift SOURCES tests/test_vulift.cpp LIBS hp2lift)
  endif()
endif()
