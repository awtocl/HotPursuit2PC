// Game patches for SLUS-20362 (ADR 0015), shared by the recompiler and the native runtime.
// Widescreen 16:9: the camera and HUD values of PCSX2's patch database (pcsx2_patches, SLUS-20362_1D2818AF.pnach,
// "Widescreen 16:9" by nemesis2000 and "HUD fixed for widescreen" by Aero_), applied so that 4:3 and 16:9 can be switched
// while the game runs:
//  - code words that change nothing with the game's own data are built into the translation (kBakedPatches);
//  - `lui` instructions whose constant differs between the modes read it from guest memory at run time
//    (kRuntimeImmediates): the runtime writes the wanted constant into the instruction word's low half;
//  - data words are written by the runtime (kWidescreenWords).
#pragma once

#include <cstdint>

namespace hp2::patches {

struct CodeWord {
  std::uint32_t address, original, patched;
};

// The camera's horizontal extent: f28 = width * min(0.5, [gp-24736]) becomes width * [gp-24736]. [gp-24736] (0x0032F850)
// is 0.5 in the game's data, so with it unchanged the result is the same as the original instruction's.
inline constexpr CodeWord kBakedPatches[] = {
    {0x0010E994u, 0x460E1702u, 0x46011702u},  // mul.s f28, f2, f14 -> mul.s f28, f2, f1
};

// `lui` instructions translated to read their 16-bit constant from the instruction word in guest memory.
struct Immediate {
  std::uint32_t address;
  std::uint16_t original, widescreen;
};
inline constexpr Immediate kRuntimeImmediates[] = {
    {0x0010EDECu, 0x4320u, 0x42F0u},  // HUD: 160.0 -> 120.0
    {0x0010EE0Cu, 0x43B0u, 0x4384u},  // HUD: 352.0 -> 264.0
};

struct DataWord {
  std::uint32_t address, original, widescreen;
};
inline constexpr DataWord kWidescreenWords[] = {
    {0x0032F6ECu, 0x3F800000u, 0x3F400000u},  // camera: 1.0 -> 0.75
    {0x0032F850u, 0x3F000000u, 0x3F2AAAAAu},  // camera width scale: 0.5 -> 2/3
    {0x0032F6F4u, 0x3F4CCCCDu, 0x3F19999Au},  // HUD: 0.8 -> 0.6
    {0x002CFD64u, 0x00000000u, 0x00000040u},  // HUD
    {0x00330188u, 0x00000000u, 0x44555555u},  // HUD: 0 -> 853.33 (640 * 4/3)
};

constexpr bool IsRuntimeImmediate(std::uint32_t address) {
  for (const Immediate& i : kRuntimeImmediates)
    if (i.address == address) return true;
  return false;
}

}  // namespace hp2::patches
