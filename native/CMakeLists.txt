# native: the native runtime (ADR 0012). It links none of the reference core:
# only this project's own code (the ELF reader and helpers of recomp/ and
# assets-pipeline/). The translated game code comes from hp2code.dll, built
# for this runtime's own state layout by `hp2recomp package`.
add_executable(hp2-native src/main.cpp)
set_target_properties(hp2-native PROPERTIES OUTPUT_NAME HotPursuit2Native)
target_link_libraries(hp2-native PRIVATE hp2recomp)
