# ADR 0013 — Native runtime: Direct3D 11 presentation and GPU renderer, graphics options

- **Status:** Accepted (2026-11-26)
- **Depends on:** ADR 0001 (native track), ADR 0011 (host services); supersedes the GDI window of the native runtime
- **Owner requirements in play:** native with full use of the PC hardware (1), modern graphics settings inside the game (2),
  no emulator layer (4)

## Context

The native runtime renders the GS command stream with its own software rasteriser (`native/src/gs.cpp`) and shows the
result through GDI `StretchDIBits`. That gives a correct picture at the PS2 resolution (≈ 640×448) but no use of the GPU,
no filtering, no full screen and no settings. The owner asked for an exceptional picture on modern hardware and for the
settings to live in the game.

## Decisions

### 1. Graphics API: Direct3D 11 first, behind a backend boundary

| Option | Verdict |
|---|---|
| **Direct3D 11.1 + DXGI flip model** | **chosen** — state objects can be set one by one, which suits the GS (blend, alpha/Z tests, formats, filters combine freely); stable drivers; same picture features as D3D12 for this workload (high internal resolution, SSAA/MSAA, anisotropic filtering, HDR via DXGI 1.6, tearing/VRR) |
| Direct3D 12 | later, if profiling shows draw submission is the limit: every GS state combination becomes a pipeline state object (needs a PSO cache from day one to avoid hitches), explicit barriers/descriptor heaps/fences cost more code before the first picture. PS2 frames have a few thousand draws, so D3D12's lower per-draw CPU cost is not the bottleneck; the GS bookkeeping (render-target vs texture tracking in GS memory, CLUT, PS2 blending) is the same work under either API |
| Vulkan / OpenGL | no gain over D3D11 on the target platform (Windows x64 only) |

No D3D types leave the backend files; `gs.cpp` talks to the presenter (and later the GPU rasteriser) through plain
functions, so a D3D12 backend can be added without touching the GS interpreter. Only Windows system DLLs are used:
`d3d11.dll`, `dxgi.dll`, and `d3dcompiler_47.dll` loaded at run time (shaders are compiled from embedded HLSL; if it is
missing the runtime falls back to GDI).

### 2. Stages

1. **Presenter (this change):** the window thread owns a D3D11 device and a flip-model swap chain; each PS2 frame is uploaded
   as a texture and drawn with a post-processing shader. Options: display mode (window / borderless full screen), V-Sync,
   scaling filter (sharp bilinear, bilinear, nearest), aspect (4:3, stretch, integer), sharpening, CRT scanlines.
2. **GPU rasteriser:** GS primitives become D3D11 draws into render targets kept per GS frame-buffer address at N× internal
   resolution, with a texture cache keyed like the software one (TBP/PSM/CLUT version) and invalidation on GS memory
   writes; read-backs to GS memory only where the game reads a target as data. Options added then: internal resolution
   1×–8×, texture filtering (incl. anisotropic), MSAA/SSAA, widescreen (projection patch, game-specific).
3. Optional: race frame rate 60 (the game draws races at 30; changes game behaviour → opt-in), HDR output.

### 4. Frame rate (owner requirement 2026-11-26)

Unlocked frame rate with limiter presets 60, 90, 120, 144, 240, 320, 360 and a stable frame time, without changing the
speed of the game's physics. Fixed points: the game's logic and physics advance on the emulated vertical blank, which
stays at 59.94 Hz whatever the display does, so the presentation rate never changes game speed.
- **Decision (owner, 2026-11-26): the cap stays at 60.** The frame rate goes above 60 only with real in-between frames
  (level 3); a limiter that repeats frames above the game's rate was built and removed (no added smoothness).
- **Level 2 — races at 60:** the game simulates per vblank but draws every second one in races; making it draw every
  vblank doubles the distinct frames if the simulation is not tied to the draw rate (to verify in the code; opt-in).
- **Level 3 — more than 60 distinct frames:** intermediate frames from the state between two simulation ticks. Candidate:
  interpolate the per-object matrices the game sends to VU1 between consecutive frames (no change to game logic).
  Research item; its own ADR before implementation.

### 5. 16:9 by default (owner requirement 2026-11-26)

The default picture is 16:9 and the interface is laid out for it: (a) the 3D scene uses Hor+ (wider horizontal field of
view from the game's projection, found in its code), (b) 2D elements drawn by the EE paths (menus, HUD: GIF paths 2/3,
not the VU1 path 1 of the 3D scene) are narrowed back to their 4:3 proportions around their anchor so they are not
stretched, full-screen layers (backgrounds, video, fades) excepted, (c) the presenter shows 16:9. 4:3, stretch and
integer scale stay as options.

### 3. Settings inside the game

A native settings overlay opened with **F1** or **Select + Start** on the pad, drawn by the runtime over the game in the
visual language of Hot Pursuit 2 (steel-blue translucent angled panels, amber/orange selection, bold italic type). While
it is open the game receives no input. Settings are stored in `hp2native.ini` next to the executable (nothing is written
outside the game folder).

**Owner requirement (2026-11-26): the extended graphics settings must be reachable from the game's own menus.** Plan:
find the Options screen's menu definition and handler in the game code/data, add a "Graphics" entry (or take over a
console-only entry such as screen centring, which has no meaning on PC) whose selection hands control to the native
settings page; the page keeps the game's look so the flow reads as part of the game. The F1 overlay stays as a shortcut.

## Consequences

- The runtime links `d3d11.lib`/`dxgi.lib` (system import libraries); no third-party code.
- GDI stays as a fallback when D3D11 or the shader compiler is unavailable.
- Stage 2 is the large piece; until it lands, "internal resolution" is not offered (no option is shown that does nothing).

## What would reopen this ADR

- Profiling showing CPU-bound draw submission in stage 2 → add the D3D12 backend.
- The owner preferring the settings inside the game's Options menu from the start.
