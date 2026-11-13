# engine

What runs the game. Decisions: [ADR 0001](../docs/adr/0001-architecture.md)
(staging) and [ADR 0005](../docs/adr/0005-rendering-audio.md) (this launcher).

The owner tried the emulator-backed build and did not accept it (stutter,
rendering defects). Per [ADR 0006](../docs/adr/0006-recompilation-main-line.md)
it is now the **reference build**; the product is the recompiled one.

| Stage | What runs the game code | Status |
|---|---|---|
| Reference (was "M1") | the Play! PS2 core (BSD 2-Clause) linked into our launcher — **emulator-backed, not native** | works; known problems below |
| R0 | same, with our frame pacer and per-frame measurement | **done** |
| R1 | game code and vector-unit microcode translated ahead of time by the converter | analysis done (`recomp/`); translator waits for ADR 0007 |
| R2–R4 | host services, own renderer, native audio | not started |

## What is here

| Path | Contents |
|---|---|
| `src/main.cpp` | `HotPursuit2.exe`: window, WGL 3.2 context for Play!'s OpenGL renderer, WaveOut audio, XInput + keyboard, data path inside the game folder, crash handler, automation switches |
| `vdisc/` | virtual disc: validates `<game>/rom` and serves it to the core as DVD sectors (no image file) |
| `CMakeLists.txt` | adds Play!'s `PlayCore` and `gsh_opengl` from `.cache/third_party/Play-` and links the launcher statically |

Play!'s source is not in this repository. Fetch the pinned revision first:

```powershell
pwsh tools/fetch-play.ps1      # into .cache/third_party/Play- (about 80 MB)
pwsh tools/build.ps1 -Preset release
```

Without it, everything else still builds and the converter's `build` stage
reports itself as unavailable.

## Running it

Normally the converter installs it as `<game>/build/HotPursuit2.exe` and it
needs no arguments (`..\rom` is the disc, `.\data` holds settings and memory
cards). Options are listed at the top of `src/main.cpp` and in the README the
converter writes.

For development, `tools/run-launcher.ps1` runs a fixed number of frames with
timed pad input and screenshots, e.g.

```powershell
pwsh tools/run-launcher.ps1 -RunName try -Rom ".cache\m1-run\game\rom" -Frames 3600 -ShotEvery 600 -Mute `
     -Script "800:start,1100:start,1400:start,1700:cross,2000:cross,2300:cross,2600:cross,2900:cross:600"
```

## Measuring frame pacing

Per-second averages hide stutter. The launcher can record every emulated
refresh and every presented image:

```powershell
pwsh tools/run-launcher.ps1 -RunName pace -Rom ".cache\m1-run\game\rom" -Frames 4600 -Mute -Size 1280x896 -Res 2 `
     -Script "800:start,1100:start,1400:start,1700:cross,2000:cross,2300:cross,2600:cross,2900:cross:1600"
python -B tools/frametimes.py .cache\pace\frametimes.csv --from 52000 --to 75000 --label race
```

`-Extra '--core-limiter'` reproduces the pacing the build had before the
pacer. Numbers so far: `docs/evidence/m1-acceptance.md` §4a and §4b.

## Reference measurements for the recompiler

`--trace <dir>` (`src/ref_trace.cpp`; `-Trace` in `tools/run-launcher.ps1`)
replaces the core's executors by recording subclasses and writes:

| File | Contents |
|---|---|
| `blocks.tsv` | every block of game code the core compiled, i.e. executed |
| `vu1-blocks.tsv` | every vector-unit microcode block, with its contents |
| `profile.csv` | per emulated refresh: CPU time of the VM and graphics threads, pacer wait, and — in the `profile` build — the core's own zones (EE, VU, VIF, GIF, IOP, SPU) |
| `ee-ram.bin`, `vu1-micro.bin`, `ee-bios.bin` | memory at the end of the session |

```powershell
pwsh tools/build.ps1 -Preset profile -NoTest     # Release + the core's profiler zones, in .cache/build/profile
pwsh tools/run-launcher.ps1 -RunName trace -Preset profile -Rom ".cache\m1-run\game\rom" -Frames 4600 -Mute -Trace ...
python -B tools/profile.py .cache\trace\trace\profile.csv --from-frame 3120 --to-frame 4500 --label race
```

`--trace-counts` also counts how often each block runs; `--lift-check <list>` (profile build only: it links
LLVM) translates the listed functions and compares them with the core on every real call
(`lift-check.tsv`). From inside PowerShell: `-Extra @('--lift-check', '.cache\r1-lift-list.txt')`.

Analysis and results: `recomp/README.md`, `docs/recomp-analysis.md`.

## Known problems of the reference build

- Heavy frames cost 13–15 ms on one thread of the core and sometimes miss the
  16.7 ms refresh; code (≈ 0.2 ms per block) and shaders compile on first use
  (hitches).
- Tyre smoke and brake-light glow are drawn as flat translucent polygons
  (cause in the core's OpenGL renderer not identified).
- Races are drawn at 30 frames per second by the game itself; menus at 60 (ADR 0005 §6).
- Digital buttons only (no DualShock 2 button pressure), no rumble, one controller.
- No in-game settings UI, no save states exposed, no widescreen: Milestone 2.
