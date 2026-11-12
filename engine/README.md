# engine

What runs the game. Decisions: [ADR 0001](../docs/adr/0001-architecture.md)
(staging) and [ADR 0005](../docs/adr/0005-rendering-audio.md) (this launcher).

| Stage | What runs the game code | Label | Status |
|---|---|---|---|
| **M1** | the Play! PS2 core (BSD 2-Clause) linked into our launcher | **emulator-backed (Path C)** — not native | **done** |
| N1 | game code translated ahead-of-time at conversion | hybrid | not started |
| N2 | translated code on our own runtime (GS, audio, input, kernel services) | native (Path A′) | not started |

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

## Known limits (Milestone 1)

- Races are drawn at 30 frames per second by the game itself; menus at 60 (ADR 0005 §6).
- Digital buttons only (no DualShock 2 button pressure), no rumble, one controller.
- No in-game settings UI, no save states exposed, no widescreen: Milestone 2.
