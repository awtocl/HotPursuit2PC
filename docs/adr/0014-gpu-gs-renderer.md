# ADR 0014 — GPU renderer for the GS command stream (ADR 0013 stage 2)

- **Status:** Accepted (2026-11-26), in progress
- **Depends on:** ADR 0013 (Direct3D 11, stages), ADR 0012 (native runtime)
- **Owner requirements in play:** full use of the PC hardware (1), modern graphics options (2), no emulator layer (4)

## Context

The software GS (`native/src/gs.cpp`) is accurate and now runs on its own thread with band-parallel workers. Races hold
the console's 30 fps, but fill-heavy moments (dust in front of the camera: 10–13 million pixels per game frame) are bound
by CPU shading speed (~22–26 fps there), and higher internal resolutions are out of reach on the CPU.

## Decision

A Direct3D 11 renderer (`native/src/gs_gpu.cpp`) consumes the same primitives the software GS sets up:

1. **Opt-in while it grows:** `HP2_GPU=1` (later a setting). The software GS stays the default until the GPU path matches it
   in the race and the menus; either path can be compared at any time.
2. **One device, two threads:** a single D3D11 device with multithread protection; the GS thread draws, the window thread
   presents. The presenter samples a copy of the displayed target made on the GPU (no read-back to the CPU).
3. **GS memory stays the CPU's for uploads, palettes and texture decoding.** Image transfers, CLUT loads and the decoded
   texture cache are unchanged; decoded textures are uploaded to GPU textures once per decode.
4. **Render targets and depth buffers live on the GPU**, keyed by their GS address (FBP/ZBP, width, format), at N× internal
   resolution. A texture whose address and format match a target is sampled from the target directly (environment maps,
   mirror, frame copies). Reading a target in another format, transfers into a target and local copies involving one are
   handled per case (conversion draw, upload into the target, read-back as a last resort), each counted in the perf lines.
5. **Pixel rules in the shader:** texture functions, TCC, CLAMP/REGION wrap modes and alpha tests (with AFAIL via two
   passes when depth writes differ) are evaluated in the pixel shader with integer texel addressing and the GS's integer
   sample positions. Gouraud colour and S/T/Q interpolate linearly in screen space (`noperspective`), the division by Q is
   per pixel as on the GS.
6. **Blending with dual-source blending:** the GS formula ((A − B) × C >> 7) + D needs C on a 0..128 scale while the stored
   alpha stays as written; the shader outputs the colour/alpha to store in target 0 and the blend factor (C × 255/128) in
   target 1. Combinations without a fixed-function equivalent are listed and approximated until a later pass handles them.
7. **Depth:** 32-bit float depth buffers holding Z / 2^24 (Z24 exact); GEQUAL/GREATER map to D3D comparisons.

## Order of work

1. Device sharing, target cache, texture upload, triangle and sprite draws with the core states, presentation from the
   GPU target (opt-in). Compare against the software GS in races and menus.
2. DATE (stencil from the target's alpha bit: car shadows), PABE, FBA, partial FBMSK, format-converting reads.
3. Internal resolution 1×–8× and texture filtering options in Graphics Settings; GPU path becomes the default.

## Status of step 1 (2026-11-26)

Done: shared device, targets/depth, texture upload from the CPU cache, targets sampled directly, triangles and sprites,
TFX/TCC/wrap modes/alpha test with AFAIL passes, blending through dual-source factors, presentation of a GPU copy.
Targets store alpha as a / 128 so that `DEST_ALPHA` equals the GS's Ad / 128 (the race's car paint adds its reflection as
Cs × Ad + Cd); alpha above 128 saturates (bit 7, which DATE and PABE test, is kept). Menus, the in-game settings and the
race render; throughput 53–63 game frames/s (software 33–48), independent of fill. Not yet: DATE, PABE, lines and points,
partial FBMSK, format-converting reads of targets, local copies involving targets, internal resolution above 1×.

## Consequences

- Two renderers to keep equivalent until the GPU one is the default; the software one stays as the reference and the
  fallback (no GPU, `HP2_GPU=0`).
- No third-party code; only Windows system DLLs (`d3d11`, `dxgi`, `d3dcompiler_47` at run time).
