# launcher: the player's front end of the native game (ADR 0016). First-run setup from the user's own disc (copy,
# check, prepare with hp2recomp), then Play, settings (the same hp2native.ini as the in-game menu), memory card
# backups, screenshots, disc & setup, about. Windows system libraries only (GDI+, DWM, shell, bcrypt).

# The supported disc's files, from the converter's manifest (path, size, SHA-256), as a C++ table.
set(HP2_DISC_MANIFEST "${PROJECT_SOURCE_DIR}/converter/data/slus20362_rom_manifest.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HP2_DISC_MANIFEST}")
file(STRINGS "${HP2_DISC_MANIFEST}" hp2_manifest_lines)
set(hp2_manifest_body "// Generated from converter/data/slus20362_rom_manifest.tsv by launcher/CMakeLists.txt.\n")
foreach(line IN LISTS hp2_manifest_lines)
  string(REPLACE "\t" ";" fields "${line}")
  list(LENGTH fields n)
  if(n LESS 3)
    continue()
  endif()
  list(GET fields 0 file)
  list(GET fields 1 size)
  list(GET fields 2 sha)
  string(APPEND hp2_manifest_body "    {L\"${file}\", ${size}ull, \"${sha}\"},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/disc_manifest.inc" CONTENT "${hp2_manifest_body}" @ONLY)

add_executable(hp2-frontend WIN32 src/main.cpp src/tasks.cpp src/tasks.h src/app.manifest
               "${PROJECT_SOURCE_DIR}/native/src/hp2_style.cpp" "${PROJECT_SOURCE_DIR}/native/src/controls.cpp"
               "${PROJECT_SOURCE_DIR}/native/src/i18n.cpp"
               "${PROJECT_SOURCE_DIR}/native/src/overlay.cpp")  # the settings table and hp2native.ini, shared with the game
set_target_properties(hp2-frontend PROPERTIES OUTPUT_NAME HotPursuit2Launcher)
target_include_directories(hp2-frontend PRIVATE "${PROJECT_SOURCE_DIR}/native/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(hp2-frontend PRIVATE hp2_build_flags user32 gdi32 gdiplus dwmapi shell32 ole32 bcrypt)

if(BUILD_TESTING)
  # Draws every screen off-screen in a folder of its own (no game, no ini of the user's).
  add_test(NAME launcher_selftest COMMAND hp2-frontend --selftest)
  set_tests_properties(launcher_selftest PROPERTIES ENVIRONMENT "HP2_LAUNCHER_DIR=${CMAKE_CURRENT_BINARY_DIR}/selftest")
endif()
