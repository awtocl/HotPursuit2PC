# native: the native runtime (ADR 0012). It links none of the reference core:
# only this project's own code (the ELF reader and helpers of recomp/ and
# assets-pipeline/). The translated game code comes from hp2code.dll, built
# for this runtime's own state layout by `hp2recomp package`.
# Presentation (ADR 0013): Direct3D 11 through the Windows system DLLs (d3dcompiler_47.dll is loaded at run time).
# Audio: XAudio2 (xaudio2_9.dll / xaudio2_8.dll, loaded at run time).
add_executable(hp2-native src/main.cpp src/gs.cpp src/gs_gpu.cpp src/d3d11_present.cpp src/overlay.cpp src/hp2_style.cpp src/audio.cpp src/memcard.cpp)
set_target_properties(hp2-native PROPERTIES OUTPUT_NAME HotPursuit2Native)
target_link_libraries(hp2-native PRIVATE hp2recomp hp2vdisc user32 gdi32 d3d11 ole32 windowscodecs gdiplus)  # hp2vdisc: our own ISO 9660 view of the ROM folder
# Symbols in every configuration (a separate .pdb; the code is unchanged): the HP2_SAMPLE profiler names functions with them.
if(MSVC)
  target_compile_options(hp2-native PRIVATE /Zi)
  target_link_options(hp2-native PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
